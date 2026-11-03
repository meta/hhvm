#include <folly/portability/GTest.h>
#include <proxygen/lib/http/session/test/HTTPTransactionMocks.h>
#include <folly/io/async/HHWheelTimer.h>
#include <limits>
#include <ostream>

#include "hphp/runtime/server/proxygen/proxygen-server.h"
#include "hphp/runtime/server/proxygen/proxygen-transport.h"

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

}
