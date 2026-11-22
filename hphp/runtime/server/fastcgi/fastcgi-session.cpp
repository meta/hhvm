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
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include "hphp/runtime/server/fastcgi/fastcgi-session.h"
#include "hphp/runtime/server/fastcgi/fastcgi-server.h"
#include "hphp/runtime/base/runtime-option.h"
#include "hphp/util/logger.h"

#include <folly/io/Cursor.h>
#include <folly/io/IOBuf.h>

#include <limits>

namespace HPHP {

using folly::IOBuf;
using folly::IOBufQueue;
using folly::io::Cursor;
using folly::io::Appender;
using folly::io::QueueAppender;

////////////////////////////////////////////////////////////////////////////////

namespace fcgi {
void KVParser::reset() {
  m_phase = Phase::READ_KEY_LENGTH;
  m_readBuf.move();
  m_keyBuf.move();
  m_valueBuf.move();
}

std::tuple<
  std::unique_ptr<folly::IOBuf>,
  std::unique_ptr<folly::IOBuf>
> KVParser::readNext() {
  assertx(ready());

  m_phase = Phase::READ_KEY_LENGTH;
  auto key = m_keyBuf.move();
  auto val = m_valueBuf.move();
  process(); // check for more data

  return std::make_tuple(std::move(key), std::move(val));
}

bool KVParser::consume(std::unique_ptr<IOBuf> chain) {
  if (chain == nullptr || chain->computeChainDataLength() == 0) {
    if (m_readBuf.chainLength() != 0 || m_phase != Phase::READ_KEY_LENGTH) {
      // Malformed stream
      return false;
    }
    m_phase = Phase::COMPLETE;
    return true;
  }

  // The stream has been restarted
  if (m_phase == Phase::COMPLETE) m_phase = Phase::READ_KEY_LENGTH;

  m_readBuf.append(std::move(chain));

  // Can only process new data if we aren't currently waiting on a call to
  // readNext() to free up the key/value beffers
  if (m_phase != Phase::READY) {
    process();
  }

  return true;
}

void KVParser::process() {
  size_t available = m_readBuf.chainLength();
  size_t avail = available;
  Cursor cursor(m_readBuf.front());

  // if we can read a complete key/value then we are ready to be extracted
  if (parseKeyValue(cursor, avail)) {
    m_phase = Phase::READY;
  }

  m_readBuf.split(available - avail);
}


bool KVParser::parseKeyValue(Cursor& cursor, size_t& available) {
  if (m_phase == Phase::READ_KEY_LENGTH) {
    if (parseKeyValueLength(cursor, available, m_keyLength)) {
      m_keyLeft = m_keyLength;
      m_phase = Phase::READ_VALUE_LENGTH;
    } else {
      return false;
    }
  }
  if (m_phase == Phase::READ_VALUE_LENGTH) {
    if (parseKeyValueLength(cursor, available, m_valueLength)) {
      m_valueLeft = m_valueLength;
      m_phase = Phase::READ_KEY;
    } else {
      return false;
    }
  }
  if (m_phase == Phase::READ_KEY) {
    if (parseKeyValueContent(cursor, available, m_keyLeft, m_keyBuf)) {
      m_phase = Phase::READ_VALUE;
    } else {
      return false;
    }
  }
  if (m_phase == Phase::READ_VALUE) {
    if (parseKeyValueContent(cursor, available, m_valueLeft, m_valueBuf)) {
      m_phase = Phase::READ_KEY_LENGTH;
      return true;
    } else {
      return false;
    }
  }
  return false;
}

bool KVParser::parseKeyValueLength(Cursor& cursor,
                                   size_t& available,
                                   size_t& lengthReturn) {
  if (!available) {
    return false;
  }
  auto peeked = cursor.peek();
  if (*peeked.data() & 0x80) { // highest bit is set
    if (available < sizeof(uint32_t)) {
      return false;
    }
    lengthReturn = cursor.readBE<uint32_t>();
    lengthReturn &= ~(0x80 << 24);
    available -= sizeof(uint32_t);
  } else {
    lengthReturn = cursor.readBE<uint8_t>();
    available--;
  }
  return true;
}

bool KVParser::parseKeyValueContent(Cursor& cursor,
                                    size_t& available,
                                    size_t& length,
                                    IOBufQueue& queue) {
  std::unique_ptr<IOBuf> buf;
  size_t len = cursor.cloneAtMost(buf, length);
  queue.append(std::move(buf));
  assertx(length >= len);
  length -= len;
  assertx(available >= len);
  available -= len;
  return (length == 0);
}
}
////////////////////////////////////////////////////////////////////////////////

FastCGISession::FastCGISession(
  folly::EventBase* evBase,
  JobQueueDispatcher<FastCGIWorker>& dispatcher,
  folly::AsyncTransportWrapper::UniquePtr sock,
  const folly::SocketAddress& localAddr,
  const folly::SocketAddress& peerAddr)
  : m_eventBase(evBase)
  , m_dispatcher(dispatcher)
  , m_localAddr(localAddr)
  , m_peerAddr(peerAddr)
  , m_sock(std::move(sock))
{
  ++m_eventCount; // pending readEOF
  m_sock->setReadCB(this);
}

void FastCGISession::timeoutExpired() noexcept {
  // Hard shutdown; socket timed out
  int timeout = RuntimeOption::ConnectionTimeoutSeconds;
  // default 2 minutes FastCGIServer::FastCGIServer 
  if(timeout <= 0) timeout = 120;
  Logger::Info("Fastcgi connection: connection time took longer than %d s and timeout, it will be shutdown!", timeout);
  dropConnection();
}

void FastCGISession::describe(std::ostream& os) const {
  os << "[peerAddr: " << m_peerAddr
     << ", localAddr: " << m_localAddr
     << ", request_id: " << m_requestId
     << "]";
}

bool FastCGISession::isBusy() const {
  // We are busy whenever we are actively serving a request
  return m_requestId != 0;
}

void FastCGISession::notifyPendingShutdown() {
  closeWhenIdle();
}

void FastCGISession::closeWhenIdle() {
  if (!m_requestId) {
    m_sock->close();   // Flush any pending writes and close, calling close()
                       // will immediately call readEOF and prevent any further
                       // attempts to write data.

    // readEOF will call shutdown() which may free this out from under us, we
    // could add a DestructorGuard, but we'd only end up calling shutdown()
    // ourselves. Instead we return immediately.
    return;
  }
  m_keepConn = false; // will shutdown when request completes
}

void FastCGISession::dropConnection(const std::string& /* errorMsg */) {
  // Nothing else needs to be placed here. Calling closeWithReset() will cause
  // readEOF to be called immediately which will call shutdown().
  //
  // NB: If there are any pending writes they will all be failed. The last one
  // to fail will delete us.
  m_sock->closeWithReset();
}

void FastCGISession::dumpConnectionState(uint8_t /*loglevel*/) { /* nop */
}

const folly::SocketAddress& FastCGISession::getPeerAddress() const noexcept {
  return m_peerAddr;
}

////////////////////////////////////////////////////////////////////////////////

// Borrowed from proxygen
const uint32_t k_minReadSize = 1460;
const uint32_t k_maxReadSize = 4000;
void FastCGISession::getReadBuffer(void** bufReturn, size_t* lenReturn) {
  std::tie(*bufReturn, *lenReturn) = m_readBuf.preallocate(k_minReadSize,
                                                           k_maxReadSize);
}

/*
 * readDataAvailable() - This is the primary entry point for FastCGI records.
 *
 * While processing FastCGI records it's possible that a socket error will
 * cause us to begin destructing, we construct a DestructorGuard and check
 * the value of m_shutdown periodically to guard against deadlock and use-
 * after-free bugs.
 */
void FastCGISession::readDataAvailable(size_t len) noexcept {
  DestructorGuard dg(this);

  m_readBuf.postallocate(len);
  resetTimeout();

  // If we're shutting down don't process any further requests, we may be freed
  if (m_shutdown) {
    return;
  }

  auto origChain = m_readBuf.front();
  size_t avail = origChain ? origChain->computeChainDataLength() : 0;
  size_t total = avail;

  SCOPE_EXIT {
    m_readBuf.trimStart(total - avail);
  };

  if (!avail) return;
  auto chain = origChain->clone();

  while (!m_shutdown && avail >= sizeof(fcgi::record)) {
    chain->gather(sizeof(fcgi::record));
    auto const rec = reinterpret_cast<const fcgi::record*>(chain->data());

    if (avail < rec->size()) {
      return;
    }

    chain->gather(rec->size());
    avail -= rec->size();

    switch (rec->type) {
    case fcgi::BEGIN_REQUEST:
      onRecord(rec->getTyped<fcgi::begin_record>());
      break;
    case fcgi::ABORT_REQUEST:
      onRecord(rec->getTyped<fcgi::abort_record>());
      break;
    case fcgi::PARAMS:
      onStream(rec->getTyped<fcgi::params_record>(), chain.get());
      break;
    case fcgi::STDIN:
      onStream(rec->getTyped<fcgi::stdin_record>(), chain.get());
      break;
    case fcgi::GET_VALUES:
      onStream(rec->getTyped<fcgi::values_record>(), chain.get());
      break;
    case fcgi::GET_VALUES_RESULT:
    case fcgi::UNKNOWN_TYPE:
    case fcgi::END_REQUEST:
    case fcgi::STDOUT:
    case fcgi::STDERR:
    case fcgi::DATA:
      // Received malformed record data- bail out
      dropConnection();
      break;
    default:
      writeUnknownType(rec->type);
    }

    chain->trimStart(rec->size());
  }
}

void FastCGISession::readEOF() noexcept {
  ioStop();
}

////////////////////////////////////////////////////////////////////////////////

void FastCGISession::readErr(const folly::AsyncSocketException&) noexcept {
  ioStop();
}

void FastCGISession::writeErr(size_t,
    const folly::AsyncSocketException&) noexcept {
  ioStop();
}

void FastCGISession::writeSuccess() noexcept {
  if (--m_eventCount == 0 && m_shutdown) {
    // If we were terminating and this was the last pending event then trigger
    // the delete.
    destroy();
  }
}

////////////////////////////////////////////////////////////////////////////////

void FastCGISession::onStdOut(std::unique_ptr<IOBuf> chain) {
  // FastCGITransport doesn't run in the same event base. Calling into internal
  // functions here is unsafe from other threads so we enqueue the work for the
  // event base.
  m_eventBase->runInEventBaseThread([this, chain = std::move(chain)]() mutable {
    writeStream(fcgi::STDOUT, std::move(chain));
  });
}

void FastCGISession::onComplete() {
  // FastCGITransport doesn't run in the same event base. Calling into internal
  // functions here is unsafe from other threads so we enqueue the work for the
  // event base.
  m_eventBase->runInEventBaseThread([&] {
    if (!m_aborting && !m_shutdown) {
      // If we're aborting we already wrote the end request. If we're shutting
      // down the socket is closed.
      writeEndRequest(m_requestId, 0, fcgi::REQUEST_COMPLETE);
    }

    // Reset state
    m_requestId = 0;
    m_paramsReader.reset();
    m_headersComplete = false;
    m_bodyComplete = false;
    m_transport.reset();

    --m_eventCount; // pending onComplete() received

    // Check if we were waiting to shutdown
    if (m_shutdown && !m_eventCount) {
      destroy();
      return; // not safe to continue after we delete ourselves
    }

    // Check if we were the last request on this channel
    if (!m_keepConn) {
      shutdown();
      return; // cannot continue execution after deleting self
    }

    // Clear the persistence flag
    m_keepConn = false;
  });
}

////////////////////////////////////////////////////////////////////////////////

void FastCGISession::ioStop() noexcept {
  if (m_transport) {
    // We set m_shutdown here because if the transport reenters and attempts
    // to write we will put the socket in a very bad state and fail all in
    // flight data.
    m_shutdown = true;

    // If the headers have not been fully received we never started the
    // transport and exiting without receiving an onComplete is safe.
    if (m_headersComplete) {
      m_bodyComplete = true;
      m_transport->onBodyComplete();
    }
  }

  // We may have read an EOF because someone deliberately called close() in
  // which case they may call shutdown() or we may already be inside shutdown()
  --m_eventCount;
  shutdown();
}

void FastCGISession::shutdown() noexcept {
  DestructorGuard dg(this); // close() may call destroy()

  m_shutdown = true;
  m_keepConn = false;

  // We may have gotten here via close(); if not perform the close ourselves.
  // close() may destroy() us, so we have the destructor guard
  if (m_sock->good()) {
    m_sock->close();
  }

  if (m_eventCount == 0) {
    destroy();
  }
}

void FastCGISession::enqueueWrite(std::unique_ptr<IOBuf> chain) {
  if (m_shutdown) {
    // If we're shutting down do not write any more data. Writing data to a
    // socket that has been closed will leave it in a very bad state.
    return;
  }
  ++m_eventCount;
  m_sock->writeChain(this, std::move(chain));
}

////////////////////////////////////////////////////////////////////////////////

void FastCGISession::onRecordImpl(const fcgi::begin_record* rec) {
  if (rec->requestId == 0) {
    // Garbage record
    dropConnection();
    return;
  }

  if (m_requestId) {
    if (m_requestId == rec->requestId) {
      // Malformed stream
      dropConnection();
      return;
    }
    // Already have an active connection
    writeEndRequest(rec->requestId, 0, fcgi::CANT_MULTIPLEX_CONN);
    return;
  }

  if (rec->role != fcgi::RESPONDER) {
    // Invalid role
    writeEndRequest(rec->requestId, 0, fcgi::UNKNOWN_ROLE);
    return;
  }

  // Until the job actually starts once we receive the headers we don't need
  // to register a pending onComplete()
  m_requestId = rec->requestId;
  m_transport = std::make_shared<FastCGITransport>(this);
  m_paramsReader.reset();

  // Determine if the server needs us to keep the channel open after the
  // request completes.
  m_keepConn = rec->flags & fcgi::KEEP_CONN;
}

void FastCGISession::onRecordImpl(const fcgi::abort_record* rec) {
  if (!m_requestId || rec->requestId != m_requestId) {
    // Garbage record
    dropConnection();
    return;
  }

  writeEndRequest(m_requestId, 1, fcgi::REQUEST_COMPLETE);
  m_aborting = true; // don't try to write REQUEST_COMPLETE again

  // There may still be a pending eventCount from an onComplete call from the
  // open tranport. We can't clear it here as there is no way to abort the
  // transport and we need to be around to receive any data it may try to send
  shutdown();
}

void FastCGISession::onStream(const fcgi::params_record* rec,
                              const IOBuf* chain) {
  if (!m_requestId || rec->requestId != m_requestId || m_headersComplete) {
    // Garbage record
    dropConnection();
    return;
  }

  Cursor cur(chain);
  std::unique_ptr<IOBuf> segment;
  cur.skip(sizeof(fcgi::record));

  cur.cloneAtMost(segment, rec->contentLength);
  if (!m_paramsReader.consume(std::move(segment))) {
    // Malformed stream
    dropConnection();
  }

  while (m_paramsReader.ready()) {
    std::unique_ptr<IOBuf> key, val;

    std::tie(key, val) = m_paramsReader.readNext();
    m_transport->onHeader(std::move(key), std::move(val));
  }

  if (m_paramsReader.done()) {
    // If we've started shutting down then don't start the transport job.
    if (m_shutdown) {
      return;
    }

    m_headersComplete = true;
    m_transport->onHeadersComplete();

    // Now that the job is running we need to wait for a call to onComplete()
    ++m_eventCount;

    // This enqueue call would be safe from any thread because as the
    // JobQueueDispatcher is synchronized
    m_dispatcher.enqueue(std::make_shared<FastCGIJob>(m_transport));
  }
}

void FastCGISession::onStream(const fcgi::stdin_record* rec,
                              const IOBuf* chain) {
  if (!m_requestId || rec->requestId != m_requestId || m_bodyComplete) {
    // Garbage record
    dropConnection();
    return;
  }

  if (!rec->contentLength) {
    m_bodyComplete = true;
    m_transport->onBodyComplete();
    return;
  }

  Cursor cur(chain);
  std::unique_ptr<IOBuf> segment;
  cur.skip(sizeof(fcgi::record));

  cur.cloneAtMost(segment, rec->contentLength);
  m_transport->onBody(std::move(segment));
}

void FastCGISession::onStream(const fcgi::values_record* rec,
                              const IOBuf* chain) {
  if (m_requestId != 0) {
    // Garbage record
    dropConnection();
    return;
  }

  Cursor cur(chain);
  std::unique_ptr<IOBuf> segment;
  cur.skip(sizeof(fcgi::record));

  cur.cloneAtMost(segment, rec->contentLength);
  if (!m_capReader.consume(std::move(segment))) {
    // Malformed stream
    dropConnection();
  }

  while (m_capReader.ready()) {
    std::unique_ptr<IOBuf> key, val;

    std::tie(key, val) = m_capReader.readNext();

    size_t key_length = key ? key->computeChainDataLength() : 0;
    Cursor cursor(key.get());
    auto keyStr = cursor.readFixedString(key_length);
    writeCapability(keyStr);
  }
}

////////////////////////////////////////////////////////////////////////////////

const std::string k_getValueMaxConnKey        = "FCGI_MAX_CONNS";
const std::string k_getValueMaxRequestsKey    = "FCGI_MAX_REQS";
const std::string k_getValueMultiplexConnsKey = "FCGI_MPXS_CONNS";

void FastCGISession::writeCapability(const std::string& key) {
  std::string value;
  if (key == k_getValueMaxConnKey) {
    value = std::to_string(m_dispatcher.getTargetNumWorkers());
  } else if (key == k_getValueMaxRequestsKey) {
    value = std::to_string(m_dispatcher.getTargetNumWorkers());
  } else if (key == k_getValueMultiplexConnsKey) {
    // multiplexed connections are not implemented
    value = "0";
  } else {
    // No-op we are supposed to ignore the keys that we
    // don't understand.
    return;
  }

  std::unique_ptr<IOBuf> chain;
  fcgi::values_result_record* rec;

  std::tie(chain, rec) = createRecord<fcgi::values_result_record>(
    fcgi::GET_VALUES_RESULT,
    0, // management stream
    key.size() + value.size() + 2 * sizeof(uint32_t) // size hint
  );

  size_t len = 0;
  Appender cursor(chain.get(), 256);

  // Lengths can be sent as either bytes or double words.
  auto appendLength = [&] (const std::string& lenStr) {
    if (lenStr.size() > 255) {
      len += sizeof(uint32_t);
      cursor.writeBE<uint32_t>(static_cast<uint32_t>(lenStr.size() | (0x80 << 24)));
    } else {
      len += sizeof(uint8_t);
      cursor.writeBE<uint8_t>(static_cast<uint8_t>(lenStr.size()));
    }
  };

  auto appendData = [&] (const std::string& data) {
    len += data.size();
    cursor.push(reinterpret_cast<const uint8_t*>(data.c_str()), data.size());
  };

  appendLength(key);
  appendLength(value);
  appendData(key);
  appendData(value);

  rec->paddingLength = ((len + 7) & ~7) - len;
  rec->contentLength = len;

  cursor.ensure(rec->paddingLength);
  cursor.append(rec->paddingLength);

  enqueueWrite(std::move(chain));
}

void FastCGISession::writeEndRequest(uint16_t request_id,
                                     uint32_t app_status,
                                     fcgi::Status proto_status) {
  std::unique_ptr<IOBuf> chain;
  fcgi::end_record* rec;

  std::tie(chain, rec) = createRecord<fcgi::end_record>(
    fcgi::END_REQUEST,
    request_id
  );

  rec->appStatus = app_status;
  rec->protStatus = proto_status;
  enqueueWrite(std::move(chain));
}

void FastCGISession::writeUnknownType(fcgi::Type record_type) {
  std::unique_ptr<IOBuf> chain;
  fcgi::unknown_record* rec;

  std::tie(chain, rec) = createRecord<fcgi::unknown_record>(
    fcgi::UNKNOWN_TYPE,
    0 // management record
  );

  rec->unknownType = record_type;
  enqueueWrite(std::move(chain));
}

void FastCGISession::writeStream(fcgi::Type type,
                                 std::unique_ptr<IOBuf> stream_chain) {
  assertx(type == fcgi::STDOUT || type == fcgi::STDERR);
  if (stream_chain == nullptr) {
    return; // Nothing to do.
  }

  IOBufQueue queue(IOBufQueue::cacheChainLength());
  QueueAppender appender(&queue, 256);
  Cursor cursor(stream_chain.get());

  size_t maxChunk = std::numeric_limits<uint16_t>::max();
  size_t available = stream_chain->computeChainDataLength();
  while (available > 0) {
    size_t len = std::min(maxChunk, available);
    size_t pad = ((len + 7) & ~7) - len;
    appender.ensure(sizeof(fcgi::record));

    auto rec = reinterpret_cast<fcgi::record*>(appender.writableData());
    rec->version = fcgi::Version::Current;
    rec->type = type;
    rec->requestId = m_requestId;
    rec->contentLength = len;
    rec->paddingLength = pad;

    appender.append(sizeof(fcgi::record));

    std::unique_ptr<IOBuf> chunk;
    cursor.clone(chunk, len);
    available -= len;
    appender.insert(std::move(chunk));
    appender.ensure(pad);
    appender.append(pad);
  }
  enqueueWrite(queue.move());
}

////////////////////////////////////////////////////////////////////////////////
}
