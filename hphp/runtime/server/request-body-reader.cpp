/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   +----------------------------------------------------------------------+
*/

#include "hphp/runtime/server/request-body-reader.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <utility>

#include "folly/io/Cursor.h"
#include "folly/io/async/EventBase.h"

#include "hphp/util/assertions.h"

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

RequestBodyProfile::RequestBodyProfile(
  std::string name, size_t highWaterMark, uint32_t capacity
)
  : name(std::move(name))
  , highWaterMark(highWaterMark)
  , capacity(capacity)
{}

bool RequestBodyProfile::tryAcquire() noexcept {
  auto count = m_inUse.load();
  while (count < capacity) {
    if (m_inUse.compare_exchange_weak(count, count + 1)) return true;
  }
  return false;
}

void RequestBodyProfile::release() noexcept {
  auto const previous = m_inUse.fetch_sub(1);
  always_assert(previous > 0);
}

uint32_t RequestBodyProfile::inUse() const noexcept {
  return m_inUse.load();
}

RequestBodyReadCompletion::RequestBodyReadCompletion(Callback callback)
  : m_callback(std::move(callback))
{}

RequestBodyReadCompletion::~RequestBodyReadCompletion() {
  complete(RequestBodyReadStatus::TransportError, nullptr);
}

void RequestBodyReadCompletion::complete(
  RequestBodyReadStatus status, std::unique_ptr<folly::IOBuf> data
) noexcept {
  std::shared_ptr<RequestBodyReader> reader;
  auto callback = std::move(m_callback);
  if (!callback) return;
  if (status != RequestBodyReadStatus::Data &&
      status != RequestBodyReadStatus::Eof) {
    reader = m_reader.lock();
    if (reader) reader->setFailure(status);
  }
  callback(status, std::move(data));
}

RequestBodyReader::ReadTimeout::ReadTimeout(
  folly::EventBase* eventBase, RequestBodyReader& reader
)
  : folly::AsyncTimeout(eventBase)
  , reader(reader)
{}

void RequestBodyReader::ReadTimeout::timeoutExpired() noexcept {
  auto const self = reader.shared_from_this();
  // Recheck the absolute deadline if the timer fires early. No resume here.
  reader.satisfyRead();
}

RequestBodyReader::RequestBodyReader(
  std::shared_ptr<RequestBodyProfile> profile,
  folly::EventBase* eventBase,
  Callbacks callbacks,
  Metadata metadata
)
  : m_profile(std::move(profile))
  , m_metadata(std::move(metadata))
  , m_callbacks(std::move(callbacks))
  , m_eventBase(eventBase)
  , m_timeout(eventBase, *this)
{
  assertx(m_profile && m_profile->highWaterMark > 0);
  assertx(eventBase);
  assertx(m_callbacks.pauseIngress);
  assertx(m_callbacks.requestAbandon);
}

// The owners must retain the reader through post-heap FINISH, body-IO closure
// and result disposal. Destruction is not a substitute for those release gates.
RequestBodyReader::~RequestBodyReader() = default;

const RequestBodyProfile& RequestBodyReader::profile() const noexcept {
  return *m_profile;
}

const RequestBodyReader::Metadata& RequestBodyReader::metadata() const noexcept {
  return m_metadata;
}

void RequestBodyReader::requestRead(
  size_t maxBytes,
  Clock::time_point deadline,
  std::unique_ptr<RequestBodyReadCompletion> completion
) noexcept {
  auto const self = shared_from_this();
  assertx(hasReadCredit() && completion);
  completion->m_reader = self;
  if (!validReadSize(maxBytes)) {
    setFailure(RequestBodyReadStatus::TransportError);
    completion->complete(failure(), nullptr);
    return;
  }
  // Destroy the command before self; by-value parameters outlive local guards.
  auto command = std::move(completion);
  try {
    if (m_callbacks.dispatchRead) {
      m_callbacks.dispatchRead(maxBytes, deadline, std::move(command));
    } else {
      setFailure(RequestBodyReadStatus::TransportError);
    }
  } catch (...) {
    setFailure(RequestBodyReadStatus::TransportError);
  }
  // A discarded posting argument completes TransportError through its own
  // destructor. Credit remains with the native result until disposal.
}

void RequestBodyReader::requestAbandon() noexcept {
  auto const self = shared_from_this();
  setFailure(RequestBodyReadStatus::Abandoned);
}

bool RequestBodyReader::tryAdmit() noexcept {
  if (m_finished.load()) return false;
  if (m_admitted.load()) return true;
  if (!m_profile->tryAcquire()) return false;
  bool expected = false;
  if (!m_admitted.compare_exchange_strong(expected, true)) {
    m_profile->release();
  }
  releaseIfReady();
  return !m_finished.load();
}

bool RequestBodyReader::tryReserveRead() noexcept {
  if (!m_admitted.load() || m_finished.load()) return false;
  bool expected = false;
  if (!m_readCredit.compare_exchange_strong(expected, true)) return false;
  if (m_finished.load()) {
    m_readCredit.store(false);
    releaseIfReady();
    return false;
  }
  return true;
}

void RequestBodyReader::disposeRead(
  bool delivered, RequestBodyReadStatus status
) noexcept {
  auto const self = shared_from_this();
  assertx(hasReadCredit());
  // Native storage has already been reset by the binding. Publish the poison
  // before returning credit; queue/timer cleanup stays on the IO thread.
  if (status != RequestBodyReadStatus::Data &&
      status != RequestBodyReadStatus::Eof) {
    setFailure(status);
  } else if (!delivered) {
    setFailure(RequestBodyReadStatus::Canceled);
  }
  m_readCredit.store(false);
  releaseIfReady();
}

bool RequestBodyReader::hasReadCredit() const noexcept {
  return m_readCredit.load();
}

RequestBodyReadStatus RequestBodyReader::failure() const noexcept {
  return m_failure.load();
}

void RequestBodyReader::read(
  size_t maxBytes,
  Clock::time_point deadline,
  std::unique_ptr<RequestBodyReadCompletion> completion
) noexcept {
  auto const self = shared_from_this();
  assertx(hasReadCredit() && !m_pending && completion);
  completion->m_reader = self;
  m_pending = std::move(completion);
  if (!validReadSize(maxBytes)) {
    fail(RequestBodyReadStatus::TransportError);
    return;
  }
  m_maxBytes = maxBytes;
  m_deadline = deadline;
  // State and completion are established before any synchronous ingress.
  satisfyReadAndResume();
}

void RequestBodyReader::append(const folly::IOBuf& input) noexcept {
  auto const self = shared_from_this();
  if (failure() != RequestBodyReadStatus::Data) {
    clearInput();
    return;
  }
  if (m_eof || m_detached) return;
  try {
    for (auto const range : input) {
      if (!range.empty()) m_queue.append(range.data(), range.size());
    }
  } catch (...) {
    fail(RequestBodyReadStatus::TransportError);
    return;
  }
  if (queuedBytes() >= m_profile->highWaterMark) pause();
  satisfyReadAndResume();
}

void RequestBodyReader::eof() noexcept {
  auto const self = shared_from_this();
  m_eof = true;
  satisfyReadAndResume();
}

void RequestBodyReader::fail(RequestBodyReadStatus status) noexcept {
  auto const self = shared_from_this();
  setFailure(status);
  drainTerminal();
  // The queued abandonment control owns terminal draining and pause release.
}

void RequestBodyReader::abandon() noexcept {
  auto const self = shared_from_this();
  m_abandonRequested.store(true);
  setFailure(RequestBodyReadStatus::Abandoned);
  drainTerminal();
  resume();
}

void RequestBodyReader::finish() noexcept {
  auto const self = shared_from_this();
  m_finished.store(true);
  abandon();
}

void RequestBodyReader::detached() noexcept {
  auto const self = shared_from_this();
  m_detached = true;
  m_paused = false;
  m_abandonRequested.store(true);
  setFailure(RequestBodyReadStatus::Abandoned);
  drainTerminal();
}

size_t RequestBodyReader::queuedBytes() const noexcept {
  assertx(m_eventBase->isInEventBaseThread());
  return m_queue.chainLength();
}

size_t RequestBodyReader::queuedCapacity() const noexcept {
  assertx(m_eventBase->isInEventBaseThread());
  // Includes head slack, tail slack, allocation capacity and IOBuf nodes.
  // This is not total transport memory or an allocator-metadata measurement.
  auto const head = m_queue.front();
  if (!head) return 0;
  size_t capacity = 0;
  auto node = head;
  do {
    capacity += node->capacity() + sizeof(folly::IOBuf);
    node = node->next();
  } while (node != head);
  return capacity;
}

bool RequestBodyReader::isQuiescent() const noexcept {
  return m_quiescent.load();
}

size_t RequestBodyReader::maxReadBytes() const noexcept {
  return std::min(m_profile->highWaterMark, kMaxReadBytes);
}

bool RequestBodyReader::validReadSize(size_t maxBytes) const noexcept {
  return maxBytes > 0 && maxBytes <= maxReadBytes();
}

void RequestBodyReader::satisfyReadAndResume() noexcept {
  satisfyRead();
  if (failure() == RequestBodyReadStatus::Data) resume();
}

void RequestBodyReader::drainTerminal() noexcept {
  clearInput();
  completeRead(failure());
  updateQuiescence();
}

void RequestBodyReader::satisfyRead() noexcept {
  if (failure() != RequestBodyReadStatus::Data) {
    drainTerminal();
    return;
  }
  if (!m_pending) return;
  auto const now = Clock::now();
  if (now >= m_deadline) {
    fail(RequestBodyReadStatus::Timeout);
    return;
  }
  if (auto const bytes = std::min(m_maxBytes, queuedBytes())) {
    std::unique_ptr<folly::IOBuf> data;
    try {
      data = folly::IOBuf::create(bytes);
      folly::io::Cursor cursor(m_queue.front());
      cursor.pull(data->writableData(), bytes);
      data->append(bytes);
      m_queue.trimStart(bytes);
    } catch (...) {
      fail(RequestBodyReadStatus::TransportError);
      return;
    }
    completeRead(RequestBodyReadStatus::Data, std::move(data));
    if (failure() != RequestBodyReadStatus::Data) clearInput();
    updateQuiescence();
    return;
  }
  if (m_eof) {
    completeRead(RequestBodyReadStatus::Eof);
    updateQuiescence();
    return;
  }
  auto const remaining = std::chrono::ceil<std::chrono::microseconds>(
    m_deadline - now
  );
  if (!m_timeout.scheduleTimeoutHighRes(remaining, {})) {
    fail(RequestBodyReadStatus::TransportError);
  }
}

void RequestBodyReader::completeRead(
  RequestBodyReadStatus status, std::unique_ptr<folly::IOBuf> data
) noexcept {
  auto completion = std::move(m_pending);
  if (m_timeout.isScheduled()) m_timeout.cancelTimeout();
  if (completion) completion->complete(status, std::move(data));
}

void RequestBodyReader::clearInput() noexcept {
  m_queue.reset();
}

void RequestBodyReader::setFailure(RequestBodyReadStatus status) noexcept {
  assertx(status != RequestBodyReadStatus::Data &&
          status != RequestBodyReadStatus::Eof);
  auto expected = RequestBodyReadStatus::Data;
  if (m_failure.compare_exchange_strong(expected, status) &&
      !m_abandonRequested.exchange(true)) {
    m_callbacks.requestAbandon();
  }
}

void RequestBodyReader::updateQuiescence() noexcept {
  if (!m_quiescent.load() && (m_finished.load() || m_detached) &&
      !m_pending && !m_timeout.isScheduled() && queuedBytes() == 0) {
    m_timeout.detachEventBase();
    m_quiescent.store(true);
  }
  releaseIfReady();
}

void RequestBodyReader::releaseIfReady() noexcept {
  // Sequentially consistent gates avoid a missed release when body-IO closure
  // and request-thread disposal race. Only one caller returns the slot.
  if (m_finished.load() && m_quiescent.load() && !m_readCredit.load() &&
      m_admitted.exchange(false)) {
    m_profile->release();
  }
}

void RequestBodyReader::pause() noexcept {
  if (m_paused || m_detached) return;
  m_paused = true;
  m_callbacks.pauseIngress(true);
}

void RequestBodyReader::resume() noexcept {
  if (!m_paused || m_detached) return;
  if (failure() == RequestBodyReadStatus::Data &&
      queuedBytes() >= m_profile->highWaterMark) return;
  m_paused = false;
  m_callbacks.pauseIngress(false);
}

///////////////////////////////////////////////////////////////////////////////
}
