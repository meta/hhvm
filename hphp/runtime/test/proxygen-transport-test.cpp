#include <folly/portability/GTest.h>
#include <proxygen/lib/http/session/test/HTTPTransactionMocks.h>
#include <folly/ScopeGuard.h>
#include <folly/io/async/HHWheelTimer.h>
#include <chrono>
#include <limits>
#include <ostream>
#include <string>
#include <vector>

#include "hphp/runtime/server/proxygen/proxygen-server.h"
#include "hphp/runtime/server/proxygen/proxygen-transport.h"
#include "hphp/runtime/server/request-body-registry.h"

#include <string_view>

namespace {
// Not in the common header list, so addressed by name.
constexpr std::string_view kExpect{"Expect"};
} // namespace

using namespace testing;
using proxygen::HTTPCodec;
using proxygen::HTTPException;
using proxygen::HTTP_HEADER_CONTENT_LENGTH;
using proxygen::HTTPMessage;
using proxygen::HTTPMethod;
using proxygen::HTTPTransaction;
using proxygen::MockHTTPTransaction;
using proxygen::TransportDirection;
using proxygen::WheelTimerInstance;

MATCHER_P(IsResponseStatusCode, statusCode, "") {
  return arg.getStatusCode() == statusCode;
}

namespace boost {
/*
 * Under gcc-4.9, gtest ends up needing this, but not instantiating it.
 * Since its not needed under 4.8, or clang, I'm guessing its a subtle
 * bug in 4.9.
 */
std::ostream& operator << (std::ostream &o,
                           const boost::optional<unsigned char>& a) {
  if (!a) {
    o << "None";
  } else {
    o << *a;
  }
  return o;
}
}

namespace HPHP {

static ServerOptions s_options{std::string(""), 80, 1};

struct MockProxygenServer : ProxygenServer {
  MockProxygenServer()
      : ProxygenServer(s_options) {}

  MOCK_METHOD1(onRequestError, void(Transport*));

  MOCK_METHOD1(onRequest, void(std::shared_ptr<ProxygenTransport>));

  MOCK_METHOD0(decrementEnqueuedCount, void());
};

struct MockHPHPWorkerThread : HPHPWorkerThread {
  explicit MockHPHPWorkerThread(MockProxygenServer* server)
      : HPHPWorkerThread(nullptr, server) {}

  void putResponseMessage(ResponseMessage&& message) override {
    m_messageQueue.emplace_back(std::move(message));
  }

  void deliverMessages(int32_t n = -1) {
    while (n > 0 || (n < 0 && !m_messageQueue.empty())) {
      EXPECT_FALSE(m_messageQueue.empty());
      auto message = std::move(m_messageQueue.front());
      auto transport = message.m_transport;
      transport->messageAvailable(std::move(message));
      m_messageQueue.pop_front();
      n--;
    }
  }

  std::list<ResponseMessage> m_messageQueue;
};

std::unique_ptr<HTTPMessage> getRequest(HTTPMethod type) {
  auto req = std::make_unique<HTTPMessage>();
  req->setMethod(type);
  req->setHTTPVersion(1, 1);
  req->setURL("/");
  return req;
}

struct ProxygenTransportBasicTest : testing::Test {
  ProxygenTransportBasicTest()
      : m_timeouts(folly::HHWheelTimer::newTimer(
            &m_eventBase,
            std::chrono::milliseconds(
                folly::HHWheelTimer::DEFAULT_TICK_INTERVAL),
            folly::AsyncTimeout::InternalEnum::NORMAL,
            std::chrono::milliseconds(100))),
        m_worker(&m_server),
        m_txn(
            TransportDirection::DOWNSTREAM,
            HTTPCodec::StreamID(1),
            1,
            m_egressQueue,
            WheelTimerInstance(m_timeouts.get())) {
    m_transport = std::make_shared<ProxygenTransport>(&m_server, &m_worker);
    m_transport->setTransactionReference(m_transport);
    m_transport->setTransaction(&m_txn);
  }

  void SetUp() override {
  }

  void TearDown() override {
  }

 protected:
  folly::EventBase m_eventBase;
  folly::HHWheelTimer::UniquePtr m_timeouts;
  proxygen::HTTP2PriorityQueue m_egressQueue;
  StrictMock<MockProxygenServer> m_server;
  StrictMock<MockHPHPWorkerThread> m_worker;
  StrictMock<MockHTTPTransaction> m_txn;
  std::shared_ptr<ProxygenTransport> m_transport;
};

struct ProxygenTransportTest : ProxygenTransportBasicTest {

  // Initiates a simple GET request to the transport
  void SetUp() override {
    EXPECT_CALL(m_server, onRequest(_))
      .WillOnce(Invoke([] (std::shared_ptr<ProxygenTransport> transport) {
            transport->setEnqueued();
          }));
    auto req = getRequest(HTTPMethod::GET);
    m_transport->onHeadersComplete(std::move(req));
    m_transport->onEOM();
  }

  // Simulates finishing from the VM thread
  void TearDown() override {
    auto transport = m_transport.get();
    if (!transport) {
      return;
    }
    m_transport->finish(std::move(m_transport));
    EXPECT_EQ(m_worker.m_messageQueue.size(), 1);
    m_worker.deliverMessages();
    EXPECT_CALL(m_server, decrementEnqueuedCount());
    transport->detachTransaction();
  }

  void sendResponse(const std::string& body) {
    m_transport->sendImpl(body.data(), body.length(), 200, false, true);
    EXPECT_EQ(m_worker.m_messageQueue.size(), 1);
    EXPECT_CALL(m_txn, sendHeaders(_));
    EXPECT_CALL(m_txn, sendBody(_));
    EXPECT_CALL(m_txn, sendEOM());
    m_worker.deliverMessages();
  }
};

struct ProxygenTransportRepostTest : ProxygenTransportTest {
  // Initiates a simple GET request to the transport
  void SetUp() override {
  }

  void TearDown() override {
    auto transport = m_transport.get();
    if (!transport) {
      return;
    }
    transport->detachTransaction();
    EXPECT_EQ(m_transport.use_count(), 1);
  }
};

TEST_F(ProxygenTransportTest, basic) {
  sendResponse("12345");
}

TEST_F(ProxygenTransportTest, error_response_does_not_preempt_committed_response) {
  // Response committed but only queued: the transaction is untouched, so
  // canSendHeaders() is still true. This is the production core's state; an
  // ingress timeout here used to abort on CHECK(!m_sendStarted).
  m_transport->sendImpl("12345", 5, 200, false, true);
  EXPECT_EQ(m_worker.m_messageQueue.size(), 1);

  HTTPException ex(HTTPException::Direction::INGRESS,
      folly::to<std::string>("ingress timeout, requestId=test"));
  ex.setProxygenError(proxygen::kErrorTimeout);
  ex.setCodecStatusCode(proxygen::ErrorCode::CANCEL);
  EXPECT_CALL(m_txn, canSendHeaders()).WillOnce(Return(true));
  // No sendHeaders/sendEOM/sendAbort/onRequestError expectations on purpose:
  // these are StrictMocks, so replacing or aborting the response fails here.
  m_transport->onError(ex);
  EXPECT_EQ(m_worker.m_messageQueue.size(), 1);

  // The committed response still flushes, unchanged.
  EXPECT_CALL(m_txn, sendHeaders(IsResponseStatusCode(200)));
  EXPECT_CALL(m_txn, sendBody(_));
  EXPECT_CALL(m_txn, sendEOM());
  m_worker.deliverMessages();
}

TEST_F(ProxygenTransportBasicTest, unsupported_method) {
  auto req = getRequest(HTTPMethod::UNSUB);
  EXPECT_CALL(m_server, onRequestError(_));
  EXPECT_CALL(m_txn, sendHeaders(IsResponseStatusCode(400)));
  EXPECT_CALL(m_txn, sendEOM());
  m_transport->onHeadersComplete(std::move(req));
  m_transport->onEOM();
}

TEST_F(ProxygenTransportBasicTest, body_after_413) {
  auto req = getRequest(HTTPMethod::POST);
  m_transport->setMaxPost(Cfg::Server::MaxPostSize, 30);
  auto length = folly::to<std::string>(Cfg::Server::MaxPostSize + 1);
  req->getHeaders().add(HTTP_HEADER_CONTENT_LENGTH, length);
  EXPECT_CALL(m_server, onRequestError(_));
  // The WillOnces will call the non-mocked functions to move the state machine.
  EXPECT_CALL(m_txn, sendHeaders(IsResponseStatusCode(413)))
  .WillOnce(Invoke(
    [&](const HTTPMessage& m) {
      m_txn.HTTPTransaction::sendHeaders(m);
    }));
  EXPECT_CALL(m_txn, sendHeadersWithOptionalEOM(_, _))
  .WillOnce(Invoke(
    [&](const HTTPMessage& m, bool eom) {
      m_txn.HTTPTransaction::sendHeadersWithOptionalEOM(m, eom);
    }));
  EXPECT_CALL(m_txn, sendEOM())
  .WillOnce(Invoke(
    [&]() {
      m_txn.HTTPTransaction::sendEOM();
    }));
  EXPECT_CALL(m_txn, sendAbort(_));
  m_transport->onHeadersComplete(std::move(req));
  m_transport->onBody(folly::IOBuf::copyBuffer("I still have so much to say"));
  m_transport->onBody(folly::IOBuf::copyBuffer("I still have so much to say"));
}

TEST_F(ProxygenTransportBasicTest, invalid_expect) {
  auto req = getRequest(HTTPMethod::POST);
  auto length = folly::to<std::string>(Cfg::Server::MaxPostSize);
  req->getHeaders().add(HTTP_HEADER_CONTENT_LENGTH, length);
  req->getHeaders().add(kExpect, "105-stop");
  EXPECT_CALL(m_server, onRequestError(_));
  EXPECT_CALL(m_txn, sendHeaders(IsResponseStatusCode(417)));
  EXPECT_CALL(m_txn, sendEOM());
  m_transport->onHeadersComplete(std::move(req));
}

TEST_F(ProxygenTransportBasicTest, valid_expect) {
  auto req = getRequest(HTTPMethod::POST);
  auto length = folly::to<std::string>(Cfg::Server::MaxPostSize);
  req->getHeaders().add(HTTP_HEADER_CONTENT_LENGTH, length);
  req->getHeaders().add(kExpect, "100-continue");
  EXPECT_CALL(m_txn, sendHeaders(IsResponseStatusCode(100)));
  m_transport->onHeadersComplete(std::move(req));
}

TEST_F(ProxygenTransportBasicTest, valid_expect_overlarge_length) {
  auto req = getRequest(HTTPMethod::POST);
  auto length = folly::to<std::string>(Cfg::Server::MaxPostSize + 1);
  req->getHeaders().add(HTTP_HEADER_CONTENT_LENGTH, length);
  req->getHeaders().add(kExpect, "100-continue");
  EXPECT_CALL(m_server, onRequestError(_));
  EXPECT_CALL(m_txn, sendHeaders(IsResponseStatusCode(417)));
  EXPECT_CALL(m_txn, sendEOM());
  m_transport->onHeadersComplete(std::move(req));
}

TEST_F(ProxygenTransportBasicTest, invalid_length) {
  auto req = getRequest(HTTPMethod::POST);
  req->getHeaders().add(HTTP_HEADER_CONTENT_LENGTH, "blarf");
  EXPECT_CALL(m_server, onRequestError(_));
  EXPECT_CALL(m_txn, sendHeaders(IsResponseStatusCode(400)));
  EXPECT_CALL(m_txn, sendEOM());
  m_transport->onHeadersComplete(std::move(req));
}

TEST_F(ProxygenTransportBasicTest, weird_length) {
  auto req = getRequest(HTTPMethod::POST);
  req->getHeaders().add(HTTP_HEADER_CONTENT_LENGTH, "25, 200");
  EXPECT_CALL(m_server, onRequestError(_));
  EXPECT_CALL(m_txn, sendHeaders(IsResponseStatusCode(400)));
  EXPECT_CALL(m_txn, sendEOM());
  m_transport->onHeadersComplete(std::move(req));
}

TEST_F(ProxygenTransportBasicTest, negative_length) {
  auto req = getRequest(HTTPMethod::POST);
  req->getHeaders().add(HTTP_HEADER_CONTENT_LENGTH, "-500");
  EXPECT_CALL(m_server, onRequestError(_));
  EXPECT_CALL(m_txn, sendHeaders(IsResponseStatusCode(400)));
  EXPECT_CALL(m_txn, sendEOM());
  m_transport->onHeadersComplete(std::move(req));
}

TEST_F(ProxygenTransportBasicTest, overflowed_length) {
  auto req = getRequest(HTTPMethod::POST);
  std::string length(std::numeric_limits<long long>::digits10 + 1, '9');
  req->getHeaders().add(HTTP_HEADER_CONTENT_LENGTH, length);
  EXPECT_CALL(m_server, onRequestError(_));
  EXPECT_CALL(m_txn, sendHeaders(IsResponseStatusCode(413)));
  EXPECT_CALL(m_txn, sendEOM());
  m_transport->onHeadersComplete(std::move(req));
}

TEST_F(ProxygenTransportBasicTest, no_length) {
  auto req = getRequest(HTTPMethod::POST);
  m_transport->onHeadersComplete(std::move(req));
}

TEST_F(ProxygenTransportBasicTest, overlarge_body) {
  auto req = getRequest(HTTPMethod::POST);
  EXPECT_CALL(m_server, onRequestError(_));
  EXPECT_CALL(m_txn, sendHeaders(IsResponseStatusCode(413)))
  .WillOnce(Invoke(
    [&](const HTTPMessage& m) {
      m_txn.HTTPTransaction::sendHeaders(m);
    }));
  EXPECT_CALL(m_txn, sendHeadersWithOptionalEOM(_, _))
  .WillOnce(Invoke(
    [&](const HTTPMessage& m, bool eom) {
      m_txn.HTTPTransaction::sendHeadersWithOptionalEOM(m, eom);
    }));
  EXPECT_CALL(m_txn, sendEOM())
  .WillOnce(Invoke(
    [&]() {
      m_txn.HTTPTransaction::sendEOM();
    }));
  EXPECT_CALL(m_txn, sendAbort(_));
  m_transport->onHeadersComplete(std::move(req));
  m_transport->setMaxPost(10, 5);
  m_transport->onBody(folly::IOBuf::copyBuffer("More than 10 bytes"));
  // Give it multiple bodies since realistically this can happen.
  m_transport->onBody(folly::IOBuf::copyBuffer("Even more than 10 bytes"));
}

TEST_F(ProxygenTransportTest, client_timeout) {
  // Verify a Http 408 response would be returned on client request timeout
  HTTPException ex(HTTPException::Direction::INGRESS,
      folly::to<std::string>("ingress timeout, requestId=test"));
  ex.setProxygenError(proxygen::kErrorTimeout);
  ex.setCodecStatusCode(proxygen::ErrorCode::CANCEL);

  // Setting up the expectation that canSendHeaders returns true is required
  // due to the fact that most of the methods that control internal state
  // within the MockHTTPTransaction are mocked and thus its internal state is
  // invalid
  EXPECT_CALL(m_txn, canSendHeaders()).WillOnce(Return(true));
  EXPECT_CALL(m_txn, sendHeaders(IsResponseStatusCode(408)));
  EXPECT_CALL(m_txn, sendEOM());
  EXPECT_CALL(m_server, onRequestError(_));
  m_transport->onError(ex);
}

TEST_F(ProxygenTransportTest, client_timeout_incomplete_reply) {
  // Verify the connection is aborted in case where there is a client timeout
  // but we are in a mid reply
  HTTPException ex(HTTPException::Direction::INGRESS,
      folly::to<std::string>("ingress timeout, requestId=test"));
  ex.setProxygenError(proxygen::kErrorTimeout);
  ex.setCodecStatusCode(proxygen::ErrorCode::CANCEL);

  // Setting up the expectation that canSendHeaders returns false is required
  // due to the fact that most of the methods that control internal state
  // within the MockHTTPTransaction are mocked and thus its internal state is
  // invalid
  EXPECT_CALL(m_txn, canSendHeaders()).WillOnce(Return(false));
  EXPECT_CALL(m_txn, sendAbort(_));
  m_transport->onError(ex);
}

TEST_F(ProxygenTransportRepostTest, no_body) {
  InSequence enforceOrder;
  auto req = getRequest(HTTPMethod::POST);

  EXPECT_CALL(m_txn, canSendHeaders());
  EXPECT_CALL(m_txn, sendHeaders(_));
  EXPECT_CALL(m_txn, sendEOM());
  m_transport->onHeadersComplete(std::move(req));
  m_transport->beginPartialPostEcho();
  m_transport->onEOM();
}

TEST_F(ProxygenTransportRepostTest, mid_body) {
  InSequence enforceOrder;
  auto req = getRequest(HTTPMethod::POST);
  auto body1 = folly::IOBuf::copyBuffer(std::string("hello"));
  auto body2 = folly::IOBuf::copyBuffer(std::string("world"));

  EXPECT_CALL(m_txn, canSendHeaders());
  EXPECT_CALL(m_txn, sendHeaders(_));
  EXPECT_CALL(m_txn, sendBody(_))
    .Times(2);
  EXPECT_CALL(m_txn, sendEOM());
  m_transport->onHeadersComplete(std::move(req));
  m_transport->onBody(std::move(body1));
  m_transport->beginPartialPostEcho();
  m_transport->onBody(std::move(body2));
  m_transport->onEOM();
}

TEST_F(ProxygenTransportRepostTest, after_body) {
  InSequence enforceOrder;
  auto req = getRequest(HTTPMethod::POST);
  auto body1 = folly::IOBuf::copyBuffer(std::string("hello"));
  auto body2 = folly::IOBuf::copyBuffer(std::string("world"));

  EXPECT_CALL(m_txn, canSendHeaders());
  EXPECT_CALL(m_txn, sendHeaders(_));
  EXPECT_CALL(m_txn, sendBody(_));
  EXPECT_CALL(m_txn, sendEOM());
  m_transport->onHeadersComplete(std::move(req));
  m_transport->onBody(std::move(body1));
  m_transport->onBody(std::move(body2));
  m_transport->beginPartialPostEcho();
  m_transport->onEOM();
}

TEST_F(ProxygenTransportRepostTest, mid_body_abort) {
  InSequence enforceOrder;
  auto req = getRequest(HTTPMethod::POST);
  auto body1 = folly::IOBuf::copyBuffer(std::string("hello"));
  auto body2 = folly::IOBuf::copyBuffer(std::string("world"));

  EXPECT_CALL(m_txn, canSendHeaders());
  EXPECT_CALL(m_txn, sendHeaders(_));
  EXPECT_CALL(m_txn, sendBody(_))
    .Times(2);
  EXPECT_CALL(m_txn, sendAbort(_));
  m_transport->onHeadersComplete(std::move(req));
  m_transport->onBody(std::move(body1));
  m_transport->beginPartialPostEcho();
  m_transport->onBody(std::move(body2));
  m_transport->abort();
}

using ReadStatus = RequestBodyReadStatus;

// Native result receipt standing in for the request-thread binding.
struct ReadReceipt {
  size_t calls{0};
  ReadStatus status{ReadStatus::Data};
  std::string data;
};

struct ProxygenTransportReaderTest : ProxygenTransportBasicTest {
  void SetUp() override {
    m_savedRegistry = RuntimeOption::RequestBodyReaders;
    RequestBodyRegistry::Profile profile;
    profile.name = "test";
    profile.highWaterMark = 8;
    profile.routes.push_back({{"default"}, {"example.com"}, "/upload/{id}",
                              RequestBodyRegistry::PathMode::Raw});
    RuntimeOption::RequestBodyReaders =
      std::make_shared<const RequestBodyRegistry>(
        std::vector<RequestBodyRegistry::Profile>{profile});
  }

  void TearDown() override {
    EXPECT_TRUE(m_worker.m_messageQueue.empty());
    RuntimeOption::RequestBodyReaders = m_savedRegistry;
  }

  static std::unique_ptr<HTTPMessage> selectedRequest(
      HTTPMethod method = HTTPMethod::POST,
      const std::string& path = "/upload/abc") {
    auto req = getRequest(method);
    req->setURL(path);
    req->getHeaders().add(proxygen::HTTP_HEADER_HOST, "example.com");
    return req;
  }

  void startSelected(std::unique_ptr<HTTPMessage> req) {
    EXPECT_CALL(m_server, onRequest(_))
      .WillOnce(Invoke([] (std::shared_ptr<ProxygenTransport> transport) {
            transport->setEnqueued();
          }));
    EXPECT_CALL(m_server, decrementEnqueuedCount());
    m_transport->onHeadersComplete(std::move(req));
    m_reader = m_transport->getRequestBodyReader();
    ASSERT_NE(nullptr, m_reader);
  }

  void requestRead(ReadReceipt& receipt, size_t maxBytes = 8) {
    ASSERT_TRUE(m_reader->tryReserveRead());
    m_reader->requestRead(
      maxBytes, RequestBodyReader::Clock::now() + std::chrono::hours(1),
      std::make_unique<RequestBodyReadCompletion>(
        [&receipt] (ReadStatus status,
                    std::unique_ptr<folly::IOBuf> data) noexcept {
          ++receipt.calls;
          receipt.status = status;
          if (data) receipt.data = data->to<std::string>();
        }));
  }

  void dispose(const ReadReceipt& receipt, bool delivered = true) {
    m_reader->disposeRead(delivered, receipt.status);
  }

  void body(const std::string& bytes) {
    m_transport->onBody(folly::IOBuf::copyBuffer(bytes));
  }

  void sendResponse() {
    m_transport->sendImpl("ok", 2, 200, false, true);
    EXPECT_CALL(m_txn, sendHeaders(IsResponseStatusCode(200)));
    EXPECT_CALL(m_txn, sendBody(_));
    EXPECT_CALL(m_txn, sendEOM());
  }

  // FINISH is posted only after the request heap has been reset.
  void finishRequest() {
    m_transport->finish(std::shared_ptr<ProxygenTransport>(m_transport));
    m_worker.deliverMessages();
  }

  void detach() {
    m_transport->detachTransaction();
    m_worker.deliverMessages();
  }

  uint32_t slotsInUse() const { return m_reader->profile().inUse(); }

  std::shared_ptr<const RequestBodyRegistry> m_savedRegistry;
  std::shared_ptr<RequestBodyReader> m_reader;
};

TEST_F(ProxygenTransportBasicTest, absent_reader_registry_keeps_buffered_post) {
  ASSERT_TRUE(!RuntimeOption::RequestBodyReaders ||
              RuntimeOption::RequestBodyReaders->empty());
  auto req = getRequest(HTTPMethod::POST);
  req->setURL("/upload/abc");
  req->getHeaders().add(proxygen::HTTP_HEADER_HOST, "example.com");
  m_transport->onHeadersComplete(std::move(req));
  m_transport->onBody(folly::IOBuf::copyBuffer("hello"));
  EXPECT_CALL(m_server, onRequest(_));
  m_transport->onEOM();

  EXPECT_EQ(nullptr, m_transport->getRequestBodyReader());
  EXPECT_TRUE(m_transport->shouldReadPostData());
  size_t size = 0;
  auto const data = static_cast<const char*>(m_transport->getPostData(size));
  EXPECT_EQ("hello", std::string(data, size));
}

TEST_F(ProxygenTransportReaderTest, unmatched_post_keeps_buffered_path) {
  m_transport->onHeadersComplete(
    selectedRequest(HTTPMethod::POST, "/upload/abc/extra"));
  EXPECT_EQ(nullptr, m_transport->getRequestBodyReader());
  EXPECT_TRUE(m_transport->shouldReadPostData());
}

TEST_F(ProxygenTransportReaderTest, selection_requires_literal_post) {
  // PUT shares Transport::Method::POST but is not HTTPMethod::POST.
  m_transport->onHeadersComplete(selectedRequest(HTTPMethod::PUT));
  EXPECT_EQ(nullptr, m_transport->getRequestBodyReader());
  EXPECT_TRUE(m_transport->shouldReadPostData());
}

TEST_F(ProxygenTransportReaderTest, duplicate_host_selects_by_first_value) {
  auto req = getRequest(HTTPMethod::POST);
  req->setURL("/upload/abc");
  req->getHeaders().add(proxygen::HTTP_HEADER_HOST, "other.example");
  req->getHeaders().add(proxygen::HTTP_HEADER_HOST, "example.com");
  m_transport->onHeadersComplete(std::move(req));
  EXPECT_EQ(nullptr, m_transport->getRequestBodyReader());
}

TEST_F(ProxygenTransportReaderTest, absent_host_uses_server_name_fallback) {
  auto const savedHost = Cfg::Server::Host;
  Cfg::Server::Host = "example.com";
  SCOPE_EXIT { Cfg::Server::Host = savedHost; };
  auto req = getRequest(HTTPMethod::POST);
  req->setURL("/upload/abc");
  startSelected(std::move(req));
  detach();
}

TEST_F(ProxygenTransportReaderTest, matched_non_blocking_post_is_not_duplex) {
  auto const saved = Cfg::Server::AllowNonBlockingPosts;
  Cfg::Server::AllowNonBlockingPosts = true;
  SCOPE_EXIT { Cfg::Server::AllowNonBlockingPosts = saved; };
  auto req = selectedRequest();
  req->getHeaders().add("NonBlockingPost", "1");
  startSelected(std::move(req));
  EXPECT_FALSE(m_transport->isStreamTransport());
  EXPECT_FALSE(m_transport->shouldReadPostData());
  detach();
}

TEST_F(ProxygenTransportReaderTest, unmatched_non_blocking_post_keeps_duplex) {
  auto const saved = Cfg::Server::AllowNonBlockingPosts;
  Cfg::Server::AllowNonBlockingPosts = true;
  SCOPE_EXIT { Cfg::Server::AllowNonBlockingPosts = saved; };
  auto req = selectedRequest(HTTPMethod::POST, "/other");
  req->getHeaders().add("NonBlockingPost", "1");
  EXPECT_CALL(m_server, onRequest(_));
  m_transport->onHeadersComplete(std::move(req));
  EXPECT_TRUE(m_transport->isStreamTransport());
  EXPECT_EQ(nullptr, m_transport->getRequestBodyReader());
  m_transport->detachTransaction();
}

TEST_F(ProxygenTransportReaderTest, refused_headers_do_not_create_reader) {
  auto req = selectedRequest();
  req->getHeaders().add(kExpect, "105-stop");
  EXPECT_CALL(m_server, onRequestError(_));
  EXPECT_CALL(m_txn, sendHeaders(IsResponseStatusCode(417)));
  EXPECT_CALL(m_txn, sendEOM());
  m_transport->onHeadersComplete(std::move(req));
  EXPECT_EQ(nullptr, m_transport->getRequestBodyReader());
}

TEST_F(ProxygenTransportReaderTest, continue_precedes_selected_enqueue) {
  InSequence enforceOrder;
  auto req = selectedRequest();
  req->getHeaders().add(HTTP_HEADER_CONTENT_LENGTH, "4");
  req->getHeaders().add(kExpect, "100-continue");
  EXPECT_CALL(m_txn, sendHeaders(IsResponseStatusCode(100)));
  startSelected(std::move(req));
  detach();
}

TEST_F(ProxygenTransportReaderTest, selected_body_has_no_legacy_data_or_status) {
  startSelected(selectedRequest());
  EXPECT_EQ("/upload/{id}", m_reader->metadata().route);
  EXPECT_EQ(std::vector<std::string>({"abc"}), m_reader->metadata().segments);
  EXPECT_FALSE(m_transport->shouldReadPostData());
  body("abcd");
  m_transport->onEOM();

  // StrictMock: no response was sent, and nothing is queued for egress.
  EXPECT_TRUE(m_worker.m_messageQueue.empty());
  size_t size = 1;
  EXPECT_EQ(nullptr, m_transport->getPostData(size));
  EXPECT_EQ(0, size);
  EXPECT_FALSE(m_transport->hasMorePostData());
  size = 1;
  EXPECT_EQ(nullptr, m_transport->getMorePostData(size));
  EXPECT_EQ(0, size);

  ASSERT_TRUE(m_reader->tryAdmit());
  ReadReceipt data;
  requestRead(data);
  EXPECT_EQ(0, data.calls);
  m_worker.deliverMessages();
  EXPECT_EQ(ReadStatus::Data, data.status);
  EXPECT_EQ("abcd", data.data);
  dispose(data);
  ReadReceipt eof;
  requestRead(eof);
  m_worker.deliverMessages();
  EXPECT_EQ(ReadStatus::Eof, eof.status);
  dispose(eof);

  sendResponse();
  m_worker.deliverMessages();
  finishRequest();
  EXPECT_EQ(0, slotsInUse());
  detach();
}

TEST_F(ProxygenTransportReaderTest, reader_resumes_last_and_allows_reentry) {
  startSelected(selectedRequest());
  InSequence enforceOrder;
  EXPECT_CALL(m_txn, pauseIngress());
  body("abcdefgh");
  ASSERT_TRUE(m_reader->tryAdmit());

  ReadReceipt first;
  EXPECT_CALL(m_txn, resumeIngress()).WillOnce(InvokeWithoutArgs([&] {
    EXPECT_EQ(1, first.calls);
    body("ijklmnop");
  }));
  EXPECT_CALL(m_txn, pauseIngress());
  requestRead(first);
  m_worker.deliverMessages();
  EXPECT_EQ("abcdefgh", first.data);
  EXPECT_EQ(8, m_reader->queuedBytes());
  dispose(first);

  ReadReceipt second;
  EXPECT_CALL(m_txn, resumeIngress()).WillOnce(InvokeWithoutArgs([&] {
    EXPECT_EQ(1, second.calls);
    m_transport->onEOM();
  }));
  requestRead(second);
  m_worker.deliverMessages();
  EXPECT_EQ("ijklmnop", second.data);
  dispose(second);

  ReadReceipt eof;
  requestRead(eof);
  m_worker.deliverMessages();
  EXPECT_EQ(ReadStatus::Eof, eof.status);
  dispose(eof);

  sendResponse();
  m_worker.deliverMessages();
  finishRequest();
  detach();
}

TEST_F(ProxygenTransportReaderTest, body_limit_is_reader_failure_not_413) {
  startSelected(selectedRequest());
  m_transport->setMaxPost(4, 128 * 1024);
  ASSERT_TRUE(m_reader->tryAdmit());
  ReadReceipt pending;
  requestRead(pending);
  m_worker.deliverMessages();
  EXPECT_EQ(0, pending.calls);

  // HTTP/1 holds the terminal pause until the final response is submitted.
  EXPECT_CALL(m_txn, pauseIngress());
  body("abcdef");
  EXPECT_EQ(1, pending.calls);
  EXPECT_EQ(ReadStatus::BodyTooLarge, pending.status);
  EXPECT_TRUE(pending.data.empty());
  EXPECT_EQ(0, m_reader->queuedBytes());
  dispose(pending, false);
  m_worker.deliverMessages();

  InSequence enforceOrder;
  sendResponse();
  EXPECT_CALL(m_txn, resumeIngress());
  m_worker.deliverMessages();
  finishRequest();
  detach();
}

struct ProxygenTransportReaderLimitTest
  : ProxygenTransportReaderTest, WithParamInterface<int> {};

TEST_P(ProxygenTransportReaderLimitTest, configured_limit_covers_cumulative_body) {
  auto const savedMax = Cfg::Server::MaxPostSize;
  auto const savedLowest = Cfg::Server::LowestMaxPostSize;
  Cfg::Server::MaxPostSize = 4;
  Cfg::Server::LowestMaxPostSize = 2;
  SCOPE_EXIT {
    Cfg::Server::MaxPostSize = savedMax;
    Cfg::Server::LowestMaxPostSize = savedLowest;
  };
  auto req = selectedRequest();
  if (GetParam() == 2) {
    req->setHTTPVersion(2, 0);
    m_txn.setupCodec(proxygen::CodecProtocol::HTTP_2);
  } else {
    req->setHTTPVersion(1, GetParam());
  }
  startSelected(std::move(req));
  ASSERT_TRUE(m_reader->tryAdmit());

  ReadReceipt first;
  requestRead(first);
  m_worker.deliverMessages();
  body("abc");
  EXPECT_EQ(ReadStatus::Data, first.status);
  EXPECT_EQ("abc", first.data);
  dispose(first);

  ReadReceipt crossing;
  requestRead(crossing);
  m_worker.deliverMessages();
  if (GetParam() != 2) EXPECT_CALL(m_txn, pauseIngress());
  body("de");
  EXPECT_EQ(1, crossing.calls);
  EXPECT_EQ(ReadStatus::BodyTooLarge, crossing.status);
  EXPECT_TRUE(crossing.data.empty());
  EXPECT_EQ(0, m_reader->queuedBytes());
  dispose(crossing, false);
  m_worker.deliverMessages();

  ReadReceipt failed;
  requestRead(failed);
  m_worker.deliverMessages();
  EXPECT_EQ(ReadStatus::BodyTooLarge, failed.status);
  EXPECT_TRUE(failed.data.empty());
  dispose(failed, false);
  sendResponse();
  if (GetParam() != 2) EXPECT_CALL(m_txn, resumeIngress());
  m_worker.deliverMessages();
  finishRequest();
  EXPECT_EQ(0, slotsInUse());
  detach();
}

INSTANTIATE_TEST_SUITE_P(
  HttpVersions, ProxygenTransportReaderLimitTest, Values(0, 1, 2));

struct ProxygenTransportReaderTimeoutTest
  : ProxygenTransportReaderTest, WithParamInterface<bool> {
  void startTimeoutRequest() {
    auto const protocol = GetParam() ? proxygen::CodecProtocol::HTTP_2
                                    : proxygen::CodecProtocol::HTTP_1_1;
    m_txn.setupCodec(protocol);
    m_txn.setHandlerUnmocked(m_transport.get());
    EXPECT_CALL(m_server, onRequest(_))
      .WillOnce(Invoke([] (std::shared_ptr<ProxygenTransport> transport) {
        transport->setEnqueued();
      }));
    EXPECT_CALL(m_server, decrementEnqueuedCount());
    m_txn.onIngressHeadersComplete(selectedRequest());
    m_reader = m_transport->getRequestBodyReader();
    ASSERT_NE(nullptr, m_reader);
    ASSERT_TRUE(m_reader->tryAdmit());
  }

  void expireIngress() {
    EXPECT_CALL(m_txn, pauseIngress()).WillOnce(InvokeWithoutArgs([&] {
      m_txn.HTTPTransaction::pauseIngress();
    }));
    EXPECT_CALL(m_txn.mockTransport_, pauseIngress(&m_txn));
    EXPECT_CALL(m_txn, canSendHeaders()).WillOnce(InvokeWithoutArgs([&] {
      EXPECT_TRUE(m_txn.isIngressComplete());
      return m_txn.HTTPTransaction::canSendHeaders();
    }));
    m_txn.onIngressTimeout();
    EXPECT_TRUE(m_txn.isIngressComplete());
  }

  void expectResponse() {
    EXPECT_CALL(m_txn, sendHeaders(IsResponseStatusCode(200)))
      .WillOnce(Invoke([&](const HTTPMessage& response) {
        EXPECT_FALSE(
          response.getHeaders().exists(proxygen::HTTP_HEADER_CONNECTION));
      }));
    EXPECT_CALL(m_txn, sendBody(_)).WillOnce(Invoke(
      [](std::shared_ptr<folly::IOBuf> body) {
        EXPECT_EQ("ok", body->to<std::string>());
      }));
    EXPECT_CALL(m_txn, sendEOM());
  }

  void TearDown() override {
    m_txn.setHandlerUnmocked(nullptr);
    ProxygenTransportReaderTest::TearDown();
  }
};

TEST_P(ProxygenTransportReaderTimeoutTest, ingress_timeout_keeps_app_response) {
  startTimeoutRequest();
  ReadReceipt pending;
  requestRead(pending);
  m_worker.deliverMessages();
  expireIngress();
  EXPECT_EQ(1, pending.calls);
  EXPECT_EQ(ReadStatus::Timeout, pending.status);
  dispose(pending, false);
  m_worker.deliverMessages();

  m_transport->sendImpl("ok", 2, 200, false, true);
  expectResponse();
  m_worker.deliverMessages();
  finishRequest();
  detach();
  EXPECT_EQ(0, slotsInUse());
}

TEST_P(ProxygenTransportReaderTimeoutTest, ingress_timeout_keeps_queued_response) {
  startTimeoutRequest();
  m_transport->sendImpl("ok", 2, 200, false, true);
  EXPECT_EQ(1, m_worker.m_messageQueue.size());
  expireIngress();
  expectResponse();
  m_worker.deliverMessages();
  finishRequest();
  detach();
  EXPECT_EQ(0, slotsInUse());
}

INSTANTIATE_TEST_SUITE_P(
  HttpVersions, ProxygenTransportReaderTimeoutTest, Values(false, true));

TEST_F(ProxygenTransportReaderTest, timeout_after_headers_preserves_abort) {
  startSelected(selectedRequest());
  ASSERT_TRUE(m_reader->tryAdmit());
  ReadReceipt pending;
  requestRead(pending);
  m_worker.deliverMessages();

  HTTPException ex(HTTPException::Direction::EGRESS, "egress timeout");
  ex.setProxygenError(proxygen::kErrorTimeout);
  EXPECT_CALL(m_txn, canSendHeaders()).WillOnce(Return(false));
  EXPECT_CALL(m_txn, sendAbort(_));
  m_transport->onError(ex);
  EXPECT_EQ(1, pending.calls);
  EXPECT_EQ(ReadStatus::TransportError, pending.status);
  EXPECT_TRUE(pending.data.empty());
  dispose(pending, false);
  m_worker.deliverMessages();
  finishRequest();
  EXPECT_EQ(0, slotsInUse());
  detach();
}

TEST_F(ProxygenTransportReaderTest, controls_run_after_post_eof_egress_error) {
  startSelected(selectedRequest());
  body("abcd");
  m_transport->onEOM();
  ASSERT_TRUE(m_reader->tryAdmit());

  EXPECT_CALL(m_txn, sendAbort(_));
  m_transport->onError(
    HTTPException(HTTPException::Direction::EGRESS, "egress failure"));
  ASSERT_EQ(&m_txn, m_transport->getTransaction());

  ReadReceipt read;
  requestRead(read);
  m_worker.deliverMessages();
  EXPECT_EQ(1, read.calls);
  EXPECT_EQ(ReadStatus::TransportError, read.status);
  EXPECT_TRUE(read.data.empty());

  // StrictMock: FINISH after the egress error does not abort again.
  finishRequest();
  EXPECT_EQ(1, slotsInUse());
  dispose(read, false);
  EXPECT_EQ(0, slotsInUse());
  detach();
}

TEST_F(ProxygenTransportReaderTest, controls_run_after_detach) {
  startSelected(selectedRequest());
  ASSERT_TRUE(m_reader->tryAdmit());
  detach();
  EXPECT_EQ(nullptr, m_transport->getTransaction());

  ReadReceipt read;
  requestRead(read);
  m_worker.deliverMessages();
  EXPECT_EQ(ReadStatus::Abandoned, read.status);
  dispose(read, false);
  EXPECT_EQ(1, slotsInUse());
  finishRequest();
  EXPECT_EQ(0, slotsInUse());
}

TEST_F(ProxygenTransportReaderTest, finish_without_response_keeps_native_credit) {
  startSelected(selectedRequest());
  body("abcd");
  ASSERT_TRUE(m_reader->tryAdmit());
  ReadReceipt read;
  requestRead(read);
  m_worker.deliverMessages();
  EXPECT_EQ("abcd", read.data);

  // No response can follow FINISH, so the transaction cannot stay parked.
  EXPECT_CALL(m_txn, sendAbort(_));
  finishRequest();
  EXPECT_EQ(1, slotsInUse());
  dispose(read);
  EXPECT_EQ(0, slotsInUse());
  detach();
}

TEST_F(ProxygenTransportReaderTest, http1_abandon_holds_pause_until_response) {
  startSelected(selectedRequest());
  InSequence enforceOrder;
  m_transport->setMaxPost(Cfg::Server::MaxPostSize, 10);
  EXPECT_CALL(m_txn, pauseIngress());
  body("abcdefgh");
  m_reader->requestAbandon();
  m_worker.deliverMessages();
  EXPECT_EQ(0, m_reader->queuedBytes());

  sendResponse();
  EXPECT_CALL(m_txn, resumeIngress());
  m_worker.deliverMessages();
  // EOM submission is not delivery; the allowance applies anyway.
  EXPECT_FALSE(m_txn.isEgressComplete());
  body("12345");
  EXPECT_CALL(m_txn, sendAbort(_));
  body("67890");
  finishRequest();
  detach();
}

TEST_F(ProxygenTransportReaderTest, received_size_counts_abandoned_input) {
  m_txn.setupCodec(proxygen::CodecProtocol::HTTP_2);
  startSelected(selectedRequest());
  ASSERT_TRUE(m_reader->tryAdmit());
  auto const initialSize = m_transport->getRequestSize();
  body("abcd");
  EXPECT_EQ(initialSize + 4, m_transport->getRequestSize());

  m_reader->requestAbandon();
  m_worker.deliverMessages();
  body("efg");
  EXPECT_EQ(initialSize + 7, m_transport->getRequestSize());
  EXPECT_EQ(0, m_reader->queuedBytes());

  sendResponse();
  m_worker.deliverMessages();
  finishRequest();
  detach();
}

TEST_F(ProxygenTransportReaderTest, http2_abandon_discards_while_egress_blocked) {
  m_txn.setupCodec(proxygen::CodecProtocol::HTTP_2);
  ASSERT_EQ(proxygen::CodecProtocol::HTTP_2,
            m_txn.getTransport().getCodec().getProtocol());
  startSelected(selectedRequest());
  InSequence enforceOrder;
  m_transport->setMaxPost(Cfg::Server::MaxPostSize, 10);
  EXPECT_CALL(m_txn, pauseIngress());
  body("abcdefgh");
  m_reader->requestAbandon();
  // A shared connection is not held paused while awaiting the response.
  EXPECT_CALL(m_txn, resumeIngress());
  m_worker.deliverMessages();
  body("12345");
  EXPECT_EQ(0, m_reader->queuedBytes());

  sendResponse();
  m_worker.deliverMessages();
  EXPECT_FALSE(m_txn.isEgressComplete());
  EXPECT_CALL(m_txn, sendAbort(_));
  body("67890");
  finishRequest();
  detach();
}

// Builds the message HTTP1xCodec hands to the transport: a target that parses
// with a host replaces every Host value (HTTP1xCodec.cpp:1105-1122).
std::unique_ptr<HTTPMessage> codecRequest(
    const std::string& target, const std::vector<std::string>& hosts) {
  auto req = getRequest(HTTPMethod::POST);
  for (auto const& host : hosts) {
    req->getHeaders().add(proxygen::HTTP_HEADER_HOST, host);
  }
  auto const parsed = req->setURL(target);
  if (parsed.hasHost()) {
    req->getHeaders().set(proxygen::HTTP_HEADER_HOST, parsed.hostAndPort());
  }
  return req;
}

TEST_F(ProxygenTransportReaderTest, query_slash_selects_by_raw_target) {
  auto req = codecRequest("http://example.com?/upload/abc", {"example.com"});
  // Proxygen's own path ends at the query; selection reads the raw target.
  EXPECT_EQ("/", req->getPathAsStringPiece());
  startSelected(std::move(req));
  EXPECT_EQ(std::vector<std::string>({"abc"}), m_reader->metadata().segments);
  EXPECT_FALSE(m_transport->shouldReadPostData());
  detach();
}

TEST_F(ProxygenTransportReaderTest, query_slash_path_ends_at_next_query) {
  startSelected(
    codecRequest("http://example.com?x=1/upload/abc?y=2", {"example.com"}));
  EXPECT_EQ(std::vector<std::string>({"abc"}), m_reader->metadata().segments);
  detach();
}

TEST_F(ProxygenTransportReaderTest, query_slash_after_path_keeps_buffered) {
  m_transport->onHeadersComplete(
    codecRequest("http://example.com/other?x=/upload/abc", {"example.com"}));
  EXPECT_EQ(nullptr, m_transport->getRequestBodyReader());
  EXPECT_TRUE(m_transport->shouldReadPostData());
}

TEST_F(ProxygenTransportReaderTest, empty_port_target_keeps_host_header) {
  auto req = codecRequest("http://other.example:/upload/abc", {"example.com"});
  EXPECT_EQ("example.com",
            req->getHeaders().getSingleOrEmpty(proxygen::HTTP_HEADER_HOST));
  startSelected(std::move(req));
  detach();
}

TEST_F(ProxygenTransportReaderTest, overflow_port_target_keeps_host_header) {
  auto req =
    codecRequest("http://other.example:99999/upload/abc", {"example.com"});
  EXPECT_EQ("example.com",
            req->getHeaders().getSingleOrEmpty(proxygen::HTTP_HEADER_HOST));
  startSelected(std::move(req));
  detach();
}

TEST_F(ProxygenTransportReaderTest, empty_port_authority_does_not_select) {
  m_transport->onHeadersComplete(
    codecRequest("http://example.com:/upload/abc", {"other.example"}));
  EXPECT_EQ(nullptr, m_transport->getRequestBodyReader());
}

TEST_F(ProxygenTransportReaderTest, authority_replaces_host_for_selection) {
  startSelected(
    codecRequest("http://example.com/upload/abc", {"other.example"}));
  detach();
}

TEST_F(ProxygenTransportReaderTest, authority_to_other_host_does_not_select) {
  m_transport->onHeadersComplete(
    codecRequest("http://other.example/upload/abc", {"example.com"}));
  EXPECT_EQ(nullptr, m_transport->getRequestBodyReader());
}

TEST_F(ProxygenTransportReaderTest, uppercase_scheme_selects) {
  // HHVM's own script routing strips only lowercase http:// and https://.
  startSelected(
    codecRequest("HTTP://example.com/upload/abc", {"other.example"}));
  detach();
}

TEST_F(ProxygenTransportReaderTest, duplicate_host_first_value_selects) {
  auto req = getRequest(HTTPMethod::POST);
  req->setURL("/upload/abc");
  req->getHeaders().add(proxygen::HTTP_HEADER_HOST, "example.com");
  req->getHeaders().add(proxygen::HTTP_HEADER_HOST, "other.example");
  startSelected(std::move(req));
  detach();
}

}
