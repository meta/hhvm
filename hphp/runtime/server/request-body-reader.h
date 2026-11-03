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
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <folly/Function.h>
#include <folly/io/IOBuf.h>
#include <folly/io/IOBufQueue.h>
#include <folly/io/async/AsyncTimeout.h>

namespace HPHP {

struct RequestBodyProfile final {
  RequestBodyProfile(std::string name, size_t highWaterMark, uint32_t capacity);
  bool tryAcquire() noexcept;
  void release() noexcept;
  uint32_t inUse() const noexcept;

  const std::string name;
  // Ingress pause threshold and upper bound on a single read.
  const size_t highWaterMark;
  const uint32_t capacity;

 private:
  std::atomic<uint32_t> m_inUse{0};
};

enum class RequestBodyReadStatus : uint8_t {
  Data,
  Eof,
  Timeout,
  BodyTooLarge,
  TransportError,
  Canceled,
  Abandoned,
};

struct RequestBodyReader;

// Owns one completion obligation, including when a command is discarded.
// A discarded command completes TransportError independently of reader failure.
struct RequestBodyReadCompletion final {
  using Callback = folly::Function<void(
    RequestBodyReadStatus, std::unique_ptr<folly::IOBuf>) noexcept>;

  explicit RequestBodyReadCompletion(Callback callback);
  RequestBodyReadCompletion(RequestBodyReadCompletion&&) noexcept = default;
  RequestBodyReadCompletion& operator=(RequestBodyReadCompletion&&) = delete;
  ~RequestBodyReadCompletion();
  void complete(RequestBodyReadStatus, std::unique_ptr<folly::IOBuf>) noexcept;

 private:
  friend struct RequestBodyReader;
  Callback m_callback;
  std::weak_ptr<RequestBodyReader> m_reader;
};

struct RequestBodyReader final
  : std::enable_shared_from_this<RequestBodyReader> {
  using Clock = std::chrono::steady_clock;
  static constexpr size_t kMaxReadBytes = 65536;

  struct Metadata {
    std::string route;
    std::vector<std::string> segments;
  };

  struct Callbacks {
    // IO-thread callbacks. Resuming can re-enter body/EOF/error handlers.
    folly::Function<void(bool) noexcept> pauseIngress;
    // Thread-safe FIFO posting; dispatchRead may throw on a failed post.
    folly::Function<void(size_t, Clock::time_point,
                        std::unique_ptr<RequestBodyReadCompletion>)>
      dispatchRead;
    // May run on either thread; enqueue an abandonment control message.
    folly::Function<void() noexcept> requestAbandon;
  };

  RequestBodyReader(
    std::shared_ptr<RequestBodyProfile>, folly::EventBase*, Callbacks,
    Metadata = {});
  ~RequestBodyReader();

  const RequestBodyProfile& profile() const noexcept;
  const Metadata& metadata() const noexcept;
  size_t maxReadBytes() const noexcept;

  // Request-thread forwarding without IO queue/timer processing. Reserve credit
  // before event allocation; absent/failed dispatch records TransportError.
  // Credit remains owned until disposeRead, including failed dispatch.
  // Sizes outside [1, min(highWaterMark, kMaxReadBytes)] fail without dispatch.
  // They preserve an earlier failure, otherwise recording TransportError.
  void requestRead(
    size_t maxBytes, Clock::time_point deadline,
    std::unique_ptr<RequestBodyReadCompletion>) noexcept;
  void requestAbandon() noexcept;

  // Request-thread entry points. A credit lasts through native-result disposal.
  bool tryAdmit() noexcept;
  bool tryReserveRead() noexcept;
  void disposeRead(bool delivered, RequestBodyReadStatus status) noexcept;
  bool hasReadCredit() const noexcept;
  // Data denotes no recorded failure, not a delivered read result.
  RequestBodyReadStatus failure() const noexcept;

  // IO-thread entry points. Caller retains transport ownership across calls.
  void read(
    size_t maxBytes, Clock::time_point deadline,
    std::unique_ptr<RequestBodyReadCompletion>) noexcept;
  void append(const folly::IOBuf&) noexcept;
  void eof() noexcept;
  void fail(RequestBodyReadStatus) noexcept;
  void abandon() noexcept;
  // Post-heap FINISH closes body IO independently of response egress.
  void finish() noexcept;
  void detached() noexcept;

  // IO-thread diagnostics, not request-thread polling APIs.
  size_t queuedBytes() const noexcept;
  size_t queuedCapacity() const noexcept;
  // Thread-safe closed/empty-body-IO flag; response and Proxygen storage may remain.
  bool isQuiescent() const noexcept;

 private:
  friend struct RequestBodyReadCompletion;
  struct ReadTimeout final : folly::AsyncTimeout {
    ReadTimeout(folly::EventBase*, RequestBodyReader&);
    void timeoutExpired() noexcept override;
    RequestBodyReader& reader;
  };

  bool validReadSize(size_t) const noexcept;
  void satisfyRead() noexcept;
  void satisfyReadAndResume() noexcept;
  void drainTerminal() noexcept;
  void completeRead(RequestBodyReadStatus,
                    std::unique_ptr<folly::IOBuf> = nullptr) noexcept;
  void clearInput() noexcept;
  // The first failure posts abandonment once unless the caller already owns it.
  void setFailure(RequestBodyReadStatus) noexcept;
  // FINISH or detach must close body IO before empty storage is quiescent.
  void updateQuiescence() noexcept;
  void releaseIfReady() noexcept;
  void pause() noexcept;
  void resume() noexcept;

  std::shared_ptr<RequestBodyProfile> m_profile;
  const Metadata m_metadata;
  Callbacks m_callbacks;
  folly::EventBase* const m_eventBase;
  folly::IOBufQueue m_queue{folly::IOBufQueue::cacheChainLength()};
  ReadTimeout m_timeout;
  std::unique_ptr<RequestBodyReadCompletion> m_pending;
  Clock::time_point m_deadline;
  size_t m_maxBytes{0};
  bool m_eof{false};
  bool m_paused{false};
  bool m_detached{false};
  std::atomic<RequestBodyReadStatus> m_failure{RequestBodyReadStatus::Data};
  std::atomic<bool> m_abandonRequested{false};
  std::atomic<bool> m_admitted{false};
  std::atomic<bool> m_readCredit{false};
  std::atomic<bool> m_finished{false};
  std::atomic<bool> m_quiescent{false};
};

}
