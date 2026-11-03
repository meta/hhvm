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
#include "hphp/runtime/server/proxygen/proxygen-transport.h"

#include "hphp/runtime/base/array-iterator.h"
#include "hphp/runtime/server/http-stream-transport.h"
#include "hphp/runtime/server/proxygen/proxygen-server.h"
#include "hphp/runtime/server/server.h"
#include "hphp/util/logger.h"
#include "hphp/util/timer.h"

#include <algorithm>
#include <folly/Range.h>
#include <folly/container/Reserve.h>
#include <memory>
#include <set>

#include <string_view>

namespace {
// Not in the common header list, so addressed by name.
constexpr std::string_view kExpect{"Expect"};
} // namespace

using proxygen::HTTPException;
using proxygen::HTTPMessage;
using proxygen::HTTPMethod;
using proxygen::HTTPTransaction;
using proxygen::HTTP_HEADER_CONNECTION;
using proxygen::HTTP_HEADER_CONTENT_LENGTH;
using proxygen::HTTP_HEADER_HOST;
using proxygen::HTTP_HEADER_TRANSFER_ENCODING;
using std::shared_ptr;
using std::unique_ptr;

using namespace proxygen;

namespace {
constexpr folly::StringPiece k100Continue{"100-continue"};

static std::set<std::string> s_post_methods{
  "OPTIONS",
  "REPORT",
  "PROPFIND",
  "PROPPATCH",
  "PATCH",
  "MKCOL",
  "MKCALENDAR",
  "PUT",
  "DELETE",
  "MOVE",
  "LOCK",
  "UNLOCK",
};
}

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

ServiceData::ExportedTimeSeries* ProxygenTransport::s_requestErrorCount;
ServiceData::ExportedTimeSeries* ProxygenTransport::s_requestNonErrorCount;

///////////////////////////////////////////////////////////////////////////////

ProxygenTransport::ProxygenTransport(ProxygenServer *server,
                                     HPHPWorkerThread *worker)
  : m_server(server)
  , m_worker(worker) {
  m_worker->addPendingTransport(*this);
}

ProxygenTransport::~ProxygenTransport() {
  VLOG(5) << "destroying ProxygenTransport " << (uint64_t) this;
  if (m_enqueued) {
    m_server->decrementEnqueuedCount();
  }
  m_worker->removePendingTransport(*this);
}

bool ProxygenTransport::bufferRequest() const {
  return
    (m_method != Transport::Method::POST ||
     Cfg::Server::RequestBodyReadLimit <= 0) &&
    !isStreamTransport();
}

bool ProxygenTransport::handlePOST(const proxygen::HTTPHeaders& headers) {
  // Fail early if we have an unexpected Expect header.
  // Note also that whether the client expects a 100 determines the error code:
  // if they expect a 100 and we fail the request for any reason besides an
  // invalid request, we need to return a 417 (Expectation Failed).
  auto expectation = headers.getSingleOrEmpty(kExpect);
  bool expects_100 = false;
  if (!expectation.empty() && !k100Continue.equals(expectation, folly::AsciiCaseInsensitive())) {
    sendErrorResponse(417);
    return false;
  } else if (!expectation.empty()) {
    expects_100 = true;
  }

  // Strictly parse the Content-Length, failing on either an overflowed
  // value or an invalid positive numeric string.
  int64_t content_length = -1;
  if (headers.exists(HTTP_HEADER_CONTENT_LENGTH)) {
    auto conv = folly::tryTo<int64_t>(
      headers.getSingleOrEmpty(HTTP_HEADER_CONTENT_LENGTH));
    if (conv.hasValue()) {
      content_length = conv.value();
    } else if (conv.error() == folly::ConversionCode::POSITIVE_OVERFLOW) {
        Logger::Warning("Overflowed parsing Content-Length");
        sendErrorResponse(expects_100 ? 417 : 413);
        return false;
    }
    if (content_length < 0) {
        Logger::Warning("Invalid Content-Length");
        sendErrorResponse(400);
        return false;
    }
  }

  // fail fast if the post is too large, but only bother resolving host
  // if content_length is larger than the minimum setting.
  m_maxPost = Cfg::Server::LowestMaxPostSize;
  if (content_length > m_maxPost) {
    auto host = headers.getSingleOrEmpty(HTTP_HEADER_HOST);
    if (auto vhost = VirtualHost::Resolve(host)) {
      m_maxPost = vhost->getMaxPostSize();
    } else {
      m_maxPost = Cfg::Server::MaxPostSize;
    }
  }
  if (content_length > m_maxPost) {
    Logger::Warning("POST Content-Length of %" PRId64 " bytes exceeds "
                    "the limit of %" PRId64 " bytes",
                    content_length, m_maxPost);
    sendErrorResponse(expects_100 ? 417 : 413);
    return false;
  }
  if (expects_100) {
    m_response.setStatusCode(100);
    m_response.setStatusMessage(HTTPMessage::getDefaultReason(100));
    m_response.setHTTPVersion(1, 1);
    m_response.dumpMessage(4);
    m_clientTxn->sendHeaders(m_response);
  }
  return true;
}

void ProxygenTransport::onHeadersComplete(
  unique_ptr<HTTPMessage> msg) noexcept {

  Timer::GetMonotonicTime(m_requestStart);
  m_request = std::move(msg);
  if (m_request->isSecure()) {
    setSSL();
  }
  m_request->dumpMessage(4);
  auto method = m_request->getMethod();
  const auto& methodStr = m_request->getMethodString();
  m_extended_method = methodStr.c_str();
  if (method == HTTPMethod::GET) {
    m_method = Transport::Method::GET;
  } else if (method == HTTPMethod::POST ||
             s_post_methods.contains(methodStr)) {
    m_method = Transport::Method::POST;
  } else if (method == HTTPMethod::HEAD) {
    m_method = Transport::Method::HEAD;
  } else if (method == HTTPMethod::CONNECT) {
    sendErrorResponse(400 /* Bad Request */);
    return;
  } else {
    // looks like proxygen HTTP parser understands a few more methods
    // than libevent:
    //   TRACE, COPY, MOVE, MKACTIVITY, CHECKOUT, MERGE, MSEARCH, NOTIFY,
    //   SUBSCRIBE, UNSUBSCRIBE, PATCH
    m_method = Transport::Method::Unknown;
    sendErrorResponse(400 /* Bad Request */);
    return;
  }

  const auto& headers = m_request->getHeaders();
  m_proxygenHeaders = &headers;
  folly::grow_capacity_by(m_requestHeaders, headers.size());
  headers.forEach([&] (const std::string &header, const std::string &val) {
      m_requestHeaders[header.c_str()].push_back(val.c_str());
    });

  if (Cfg::Server::AllowNonBlockingPosts &&
      m_method == Transport::Method::POST &&
      m_requestHeaders.contains("NonBlockingPost")) {
    m_streamTransport =
      std::make_shared<stream_transport::HttpStreamServerTransport>(this);
  }

  if (m_method == Transport::Method::POST && m_request->isHTTP1_1()) {
    if (!handlePOST(headers)) {
      // We already fully responded.
      return;
    }
  }

  if (m_shouldRepost) {
    VLOG(2) << "Reposting transaction's completed receiving header,"
            << " beginning partial post";
    beginPartialPostEcho();
  }
  if (!bufferRequest()) {
    m_server->onRequest(shared_from_this());
  } // otherwise we wait for EOM
}

void ProxygenTransport::onBody(std::unique_ptr<folly::IOBuf> chain) noexcept {
  // If we've already sent a response, allow up to 128k of data to be received
  // before just sending an abort. This gives the client an opportunity to
  // process the response before receiving some form of a reset caused by the
  // abort.
  if (m_sendEnded && m_clientTxn->isEgressComplete()) {
    m_bodyLengthPastLimit -= chain->computeChainDataLength();
    if (m_bodyLengthPastLimit <= 0) {
      Logger::Warning("Received body after a response has completed, aborting");
      abort();
    }
    return;
  }

  VLOG(4) << *m_clientTxn << "Recevied body len="
          << chain->computeChainDataLength();
  m_requestBodyLength += chain->computeChainDataLength();

  if (m_maxPost >= 0 && m_requestBodyLength > m_maxPost &&
      !isStreamTransport()) {
    Logger::Warning("Request body length of %" PRId64 " now exceeds max POST of"
                    "size %" PRId64 ".", m_requestBodyLength.load(), m_maxPost);
    sendErrorResponse(413);
    return;
  }

  if (bufferRequest()) {
    CHECK(!m_enqueued);
    if (m_reposting) {
      VLOG(2) << "Reposting transaction " << *m_clientTxn
              << " received body bytes";
      if (m_clientTxn && !m_egressError) {
        VLOG(2) << "Reposting transaction " << *m_clientTxn
                << " is sending received body bytes";
        m_clientTxn->sendBody(std::move(chain));
      }
    } else {
      m_bodyData.append(std::move(chain));
    }
  } else {
    Lock lock(this);
    if (isStreamTransport() && m_streamTransport->isReady()) {
      // TODO: pause ingress if request cannot process the data fast enough?
      m_streamTransport->doOnData(std::move(chain));
    } else {
      m_bodyData.append(std::move(chain));
    }
    notify();
    if (m_bodyData.chainLength() >= Cfg::Server::RequestBodyReadLimit) {
      VLOG(4) << *m_clientTxn << "Buffer max reached, pausing ingress";
      m_clientTxn->pauseIngress();
    }
  }
}

void ProxygenTransport::requestDoneLocking() {
  Lock lock(this);
  if (isStreamTransport()) {
    m_streamTransport->doOnClose();
  }
  m_clientComplete = true;
  notify();
}

void ProxygenTransport::onEOM() noexcept {
  VLOG(4) << *m_clientTxn << "received eom";
  if (bufferRequest()) {
    CHECK(!m_enqueued);
    if (m_reposting) {
      VLOG(2) << "Transaction " << *m_clientTxn << " is reposting";
      if (m_clientTxn && !m_egressError) {
        VLOG(2) << "Transaction " << *m_clientTxn
                << " is sending EOM for repost";
        m_clientTxn->sendEOM();
      }
      return;
    }
    if (!m_bodyData.empty()) {
      // TODO: let's remove gratuitous mallocs and copies please
      m_currentBodyBuf = m_bodyData.move();
      m_currentBodyBuf->coalesce();
      m_firstBody = true;
      VLOG(4) << *m_clientTxn << "m_currentBodyBuf.length=" <<
        m_currentBodyBuf->length();
    }
    m_clientComplete = true;
    // If we've already responded to the request (most likely with a call to
    // sendErrorResponse), then we have nothing to do and don't want to service
    // this request further.
    if (m_sendEnded) {
      LOG(WARNING) << "Transaction " << *m_clientTxn << " has already been "
              << "sent a response.";
    } else {
      m_server->onRequest(shared_from_this());
    }
    return;
  } else {
    requestDoneLocking();
  }
}

std::shared_ptr<stream_transport::StreamTransport>
ProxygenTransport::getStreamTransport() const {
  return m_streamTransport;
}

bool ProxygenTransport::isStreamTransport() const {
  return m_streamTransport != nullptr;
}

const char *ProxygenTransport::getUrl() {
  return m_request->getURL().c_str();
}

const char *ProxygenTransport::getRemoteHost() {
  if (m_addressStr.empty()) {
    m_addressStr = m_clientAddress.getAddressStr();
  }
  return m_addressStr.c_str();
}

uint16_t ProxygenTransport::getRemotePort() {
  return m_clientAddress.getPort();
}

const void *ProxygenTransport::getPostData(size_t &size) {
  if (m_sendEnded || isStreamTransport()) {
    size = 0;
    return 0;
  }

  // the API contract is that you can call getPostData repeatedly until
  // you call getMorePostData
  if (m_firstBody) {
    CHECK(m_currentBodyBuf);
    size = m_currentBodyBuf->length();
    return m_currentBodyBuf->data();
  }
  return getMorePostData(size);
}

bool ProxygenTransport::hasMorePostData() {
  if (bufferRequest()) {
    CHECK(m_clientComplete);
    return false;
  }
  // This API should not be used for stream transports.
  if (isStreamTransport()) {
    return false;
  }

  Lock lock(this);
  // We have more post data if a) there's some in the queue or b) the client
  // EOM hasn't come yet
  bool result =  (m_method == Transport::Method::POST &&
                  (!m_bodyData.empty() || !m_clientComplete));
  VLOG(4) << "hasMorePostData=" << result;
  return result;
}

void ProxygenTransport::onStreamReady() {
  assertx(m_streamTransport && m_streamTransport->isReady());

  // Now that the stream is ready send a blank datum which will trigger the
  // headers to send.
  m_responseCode = 200;
  sendStreamResponse("", 0);

  Lock lock(this);
  while (!m_bodyData.empty()) {
    auto buf = m_bodyData.pop_front();
    CHECK(buf && buf->length() > 0);
    m_streamTransport->doOnData(std::move(buf));
  }
  notify();
}

const void *ProxygenTransport::getMorePostData(size_t &size) {
  if (bufferRequest()) {
    CHECK(m_clientComplete);
    size = 0;
    return nullptr;
  }
  if (isStreamTransport()) {
    size = 0;
    return nullptr;
  }

  // proxygen will send onTimeout if we don't receive data in this much time
  long maxWait = Cfg::Server::ConnectionTimeoutSeconds;
  if (maxWait <= 0) {
    maxWait = 50; // this was the default read timeout in LibEventServer
  }
  Lock lock(this);
  while (m_bodyData.empty() && !m_clientComplete) {
    VLOG(4) << "waiting for POST data for maxWait=" << maxWait;
    wait(maxWait);
  }
  auto oldLength = m_bodyData.chainLength();

  // For chunk encodings, we way receive an EOM with no data, such that
  // hasMorePostData returns true (because client is not yet complete),
  // client sends EOM, getMorePostData should return 0/nullptr
  size = 0;
  const void *data = nullptr;
  while (!m_bodyData.empty()) {
    // this is the first body if it wasn't set and buf is unset
    m_firstBody = !(m_firstBody && m_currentBodyBuf);
    m_currentBodyBuf = m_bodyData.pop_front();
    CHECK(m_currentBodyBuf && m_currentBodyBuf->length() > 0);
    size = m_currentBodyBuf->length();
    data = m_currentBodyBuf->data();
    break;
  }
  // RequestBodyReadLimit is int64_t and could be -1
  if (oldLength >= Cfg::Server::RequestBodyReadLimit &&
      m_bodyData.chainLength() < Cfg::Server::RequestBodyReadLimit) {
    VLOG(4) << "resuming ingress";
    m_worker->putResponseMessage(ResponseMessage(
        shared_from_this(), ResponseMessage::Type::RESUME_INGRESS));
  }
  VLOG(4) << "returning POST body chunk size=" << size;
  return data;
}

Transport::Method ProxygenTransport::getMethod() {
  return m_method;
}

bool ProxygenTransport::getClientComplete() {
  return m_clientComplete;
}

const char *ProxygenTransport::getExtendedMethod() {
  return m_extended_method;
}

std::string ProxygenTransport::getHTTPVersion() const {
  return m_request->getVersionString();
}

size_t ProxygenTransport::getRequestSize() const {
  return m_requestBodyLength; // header length isn't tracked
}

std::string ProxygenTransport::getHeader(const char *name) {
  assertx(name && *name);

  HeaderMap::const_iterator iter = m_requestHeaders.find(name);
  if (iter != m_requestHeaders.end()) {
    return iter->second[0];
  }
  return "";
}

const HeaderMap& ProxygenTransport::getHeaders() {
  return m_requestHeaders;
}

const proxygen::HTTPHeaders* ProxygenTransport::getProxygenHeaders() {
  return m_proxygenHeaders;
}

folly::ssl::X509UniquePtr ProxygenTransport::getPeerCertificate() {
  if (X509* x509raw = m_peerCert.get()) {
    X509_up_ref(x509raw);
    return folly::ssl::X509UniquePtr(x509raw);
  }
  return nullptr;
}

void ProxygenTransport::addHeaderImpl(const char *name, const char *value) {
  assertx(name && *name);
  assertx(value);

  if (m_sendStarted) {
    Logger::Error("trying to add header '%s: %s' after 1st chunk",
                  name, value);
    return;
  }

  m_response.getHeaders().add(name, value);
}

void ProxygenTransport::removeHeaderImpl(const char *name) {
  assertx(name && *name);

  if (m_sendStarted) {
    Logger::Error("trying to remove header '%s' after 1st chunk", name);
    return;
  }

  m_response.getHeaders().remove(name);
}

void ProxygenTransport::addRequestHeaderImpl(const char *name,
                                             const char *value) {
  assertx(name && *name);
  assertx(value);

  m_request->getHeaders().add(name, value);
  m_requestHeaders[name].push_back(value);
}

void ProxygenTransport::removeRequestHeaderImpl(const char *name) {
  assertx(name && *name);
  m_request->getHeaders().remove(name);
  m_requestHeaders.erase(name);
}

bool ProxygenTransport::isServerStopping() {
  return m_server->getStatus() == Server::RunStatus::STOPPED;
}

void ProxygenTransport::sendErrorResponse(uint32_t code) noexcept {
  HTTPMessage response;
  response.setHTTPVersion(1, 1);
  response.setStatusCode(code);
  response.setStatusMessage(HTTPMessage::getDefaultReason(code));
  response.getHeaders().add(HTTP_HEADER_CONNECTION, "close");

  // The response may already be committed but only queued; don't replace it.
  if (m_sendStarted) {
    return;
  }
  m_sendStarted = true;
  m_sendEnded = true;
  m_headerSent = true;
  m_responseCode = code;
  m_responseCodeInfo = response.getStatusMessage();
  m_server->onRequestError(this);

  CHECK(m_clientTxn && !m_egressError);
  m_clientTxn->sendHeaders(response);
  m_clientTxn->sendEOM();

  if (auto stream = getStreamTransport()) {
    // This shouldn't be necessary - but can't hurt.
    stream->close();
  }
}

void ProxygenTransport::onError(const HTTPException& err) noexcept {
  Logger::Error("HPHP transport error: %s", err.describe().c_str());

  if (err.hasProxygenError() &&
        err.getProxygenError() == ProxygenError::kErrorTimeout &&
        !m_egressError && m_clientTxn && m_clientTxn->canSendHeaders()) {
    sendErrorResponse(408 /* Request Timeout */);
  } else {
    // We either have an error here that we are not mapping to a http response
    // or m_clientTxn is in a state where it is not able to send response
    // headers (as part of a well formed response) and so we simply abort the
    // connection
    this->abort();

    // We could also check err.getDirection() to see if it's an egress error,
    // but sending abort here guarantees than any subsequent send* calls are
    // going to abort.
    m_egressError = true;
  }

  requestDoneLocking();
}

void ProxygenTransport::finish(shared_ptr<ProxygenTransport> &&transport) {
  m_worker->putResponseMessage(ResponseMessage(std::move(transport)));
}

HTTPTransaction *ProxygenTransport::getTransaction(uint64_t id,
                                                   HTTPMessage **msg) {
  if (id == 0) {
    if (m_egressError) {
      *msg = nullptr;
      return nullptr;
    }
    *msg = &m_response;
    return m_clientTxn;
  }
  return nullptr;
}

void ProxygenTransport::messageAvailable(ResponseMessage&& message) noexcept {
  if (!m_clientTxn) {
    return;
  }

  HTTPMessage *msg = nullptr;
  HTTPTransaction *txn = getTransaction(message.m_pushId, &msg);
  if (!txn) {
    // client is gone, just eat the msg
    VLOG(4) << "client is gone, eating the msg";
    return;
  }
  switch (message.m_type) {
    case ResponseMessage::Type::HEADERS:
      CHECK(msg);
      txn->sendHeaders(*msg);
      if (!message.m_chunk && !message.m_eom) {
        break;
      }
      [[fallthrough]];
    case ResponseMessage::Type::BODY:
      if (message.m_chunk && m_method != Transport::Method::HEAD) {
        // TODO: experiment with disabling this chunked flag and letting
        // proxygen framework do the chunking
        if (message.m_chunked) {
          txn->sendChunkHeader(message.m_chunk->length());
          txn->sendBody(std::move(message.m_chunk));
          txn->sendChunkTerminator();
        } else {
          txn->sendBody(std::move(message.m_chunk));
        }
      }
      if (!message.m_eom) {
        break;
      }
      [[fallthrough]];
    case ResponseMessage::Type::EOM:
      txn->sendEOM();
      break;
    case ResponseMessage::Type::FINISH:
      break;
    case ResponseMessage::Type::RESUME_INGRESS:
      txn->resumeIngress();
      break;
  }
}

void ProxygenTransport::sendStreamResponse(const void* data, int size) {
  // TODO: track and log response size.
  assertx(isStreamTransport());
  sendImpl(data, size, m_responseCode, true, false);
}

void ProxygenTransport::sendStreamEOM() {
  assertx(isStreamTransport());
  sendImpl("", 0, m_responseCode, true, true);
}

void ProxygenTransport::sendImpl(const void *data, int size, int code,
                                 bool chunked, bool eom) {
  assertx(data);
  assertx(!m_sendStarted || chunked);
  if (m_sendEnded) {
    // This should never happen, but when it does we have to bail out,
    // since there's no sensible way to send data at this point and
    // trying to do so will horribly corrupt memory.
    // TODO #2821803: Figure out whether this is caused by another bug
    // somewhere.
    return;
  }

  VLOG(4) << "sendImpl called with data size=" << size << ", code=" << code
          << ", chunked=" << chunked << ", eom=" << eom;
  eom |= !chunked;
  if (!m_sendStarted) {
    if (!chunked) {
      if (!m_response.getHeaders().exists(HTTP_HEADER_CONTENT_LENGTH)) {
        m_response.getHeaders().add(HTTP_HEADER_CONTENT_LENGTH,
                                    folly::to<std::string>(size));
      }
    } else if (Cfg::Server::SetChunkedTransferEncoding) {
      // Explicitly add Transfer-Encoding: chunked here.  libproxygen will only
      // add it for keep-alive connections
      m_response.getHeaders().set(HTTP_HEADER_TRANSFER_ENCODING, "chunked");
    }
    m_response.setStatusCode(code);
    auto const& reasonStr = getResponseInfo();
    const char* reason = reasonStr.empty() ?
      HTTPMessage::getDefaultReason(code) : reasonStr.c_str();
    m_response.setStatusMessage(reason);
    m_response.setHTTPVersion(1, 1);
    m_response.setIsChunked(chunked);
    m_response.dumpMessage(4);
    m_worker->putResponseMessage(
      ResponseMessage(shared_from_this(),
                      ResponseMessage::Type::HEADERS, 0,
                      chunked, data, size, eom));
    m_sendStarted = true;
  } else {
    m_worker->putResponseMessage(
      ResponseMessage(shared_from_this(),
                      ResponseMessage::Type::BODY, 0,
                      chunked, data, size, eom));
  }

  if (eom) {
    m_sendEnded = true;
  }

  if (chunked) {
    /*
     * Chunked replies are sent async, so there is no way to know the
     * time it took to flush the response, but tracking the bytes sent is
     * very useful.
     */
    onChunkedProgress(size);
  }
  if (code >= 500) {
    s_requestErrorCount->addValue(1);
  } else {
    s_requestNonErrorCount->addValue(1);
  }
}

void ProxygenTransport::onSendEndImpl() {
  if (!m_sendEnded) {
    VLOG(4) << "onSendEndImpl called";
    m_worker->putResponseMessage(
        ResponseMessage(shared_from_this(), ResponseMessage::Type::EOM));
    m_sendEnded = true;
  }
}

void ProxygenTransport::beginPartialPostEcho() {
  VLOG(2) << "Beginning partial post";
  if (!bufferRequest() || m_reposting || !m_clientTxn || m_egressError
      || getClientComplete() || !m_clientTxn->canSendHeaders()) {
    VLOG(2) << "beginPartialPostEcho cannot proceed, "
            << "bufferRequest() = " << bufferRequest()
            << "m_reposting = " << m_reposting
            << "m_clientTxn = " << m_clientTxn
            << "m_egressError = " << m_egressError
            << "getClientComplete() = " << getClientComplete()
            << "canSendHeaders() = " << m_clientTxn->canSendHeaders();
    return;
  }
  VLOG(2) << "beginPartialPostEcho is proceeding, "
          << "bufferRequest() = " << bufferRequest()
          << "m_reposting = " << m_reposting
          << "m_clientTxn = " << m_clientTxn
          << "m_egressError = " << m_egressError
          << "getClientComplete() = " << getClientComplete()
          << "canSendHeaders() = " << m_clientTxn->canSendHeaders();
  CHECK(!m_enqueued);
  m_reposting = true;
  HTTPMessage response;
  response.setHTTPVersion(1,1);
  response.setIsChunked(true);
  response.setStatusCode(Cfg::Server::PartialPostStatusCode);
  response.setStatusMessage("Partial post");

  // All of the clients headers should be retained downstream,
  // so all we need to echo back is the request body
  auto& headers = response.getHeaders();
  headers.add(HTTP_HEADER_TRANSFER_ENCODING, "chunked");

  m_clientTxn->sendHeaders(response);
  if (!m_bodyData.empty()) {
    VLOG(2) << "Reposting body bytes for client transaction " << *m_clientTxn;
    m_clientTxn->sendBody(m_bodyData.move());
  }
}

void ProxygenTransport::abort() {
  m_worker->removePendingTransport(*this);
  if (m_clientTxn) {
    m_clientTxn->sendAbort();
  }
}

void ProxygenTransport::trySetMaxThreadCount(int max) {
  m_server->setMaxThreadCount(max);
}

///////////////////////////////////////////////////////////////////////////////
}
