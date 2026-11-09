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

#include <folly/portability/GTest.h>

#include "hphp/runtime/server/request-uri.h"
#include "hphp/runtime/server/transport.h"

namespace HPHP {
namespace {

struct PathResolutionStarted {};

struct RequestTargetTransport : Transport {
  RequestTargetTransport(const char* method, const char* target)
    : m_method(method), m_target(target) {}

  const char* getUrl() override { return m_target; }
  const char* getRemoteHost() override { return "127.0.0.1"; }
  uint16_t getRemotePort() override { return 1234; }
  const void* getPostData(size_t& size) override {
    size = 0;
    return nullptr;
  }
  Method getMethod() override { return Method::Unknown; }
  const char* getExtendedMethod() override { return m_method; }
  std::string getHeader(const char*) override { return ""; }
  const HeaderMap& getHeaders() override { return m_headers; }
  OptString describe() const override { return "RequestTargetTransport"; }
  void addHeaderImpl(const char*, const char*) override {}
  void removeHeaderImpl(const char*) override {}
  void sendImpl(const void*, int, int code, bool, bool) override {
    responseCode = code;
  }
  void onSendEndImpl() override { responseEnded = true; }
  const char* getServerObject() override {
    ++serverObjectCalls;
    return Transport::getServerObject();
  }
  const std::string getScriptFilename() override {
    throw PathResolutionStarted{};
  }

  int serverObjectCalls{0};

  int responseCode{0};
  bool responseEnded{false};

private:
  const char* m_method;
  const char* m_target;
  HeaderMap m_headers;
};

struct RequestTargetCase {
  const char* method;
  const char* target;
  bool valid;
};

struct RequestURITargetTest : testing::TestWithParam<RequestTargetCase> {};

TEST_P(RequestURITargetTest, ValidatesBeforePathResolution) {
  auto const& test = GetParam();
  RequestTargetTransport transport(test.method, test.target);
  if (test.valid) {
    EXPECT_THROW(
      RequestURI(nullptr, &transport, "", "/var/www/"),
      PathResolutionStarted
    );
    EXPECT_EQ(transport.responseCode, 0);
  } else {
    RequestURI uri(nullptr, &transport, "", "/var/www/");
    EXPECT_TRUE(uri.done());
    EXPECT_EQ(transport.responseCode, 400);
    EXPECT_TRUE(transport.responseEnded);
  }
  EXPECT_EQ(transport.serverObjectCalls, 1);
}

TEST(RequestURITest, RejectsAsteriskWithNullMethod) {
  struct NullMethodTransport : RequestTargetTransport {
    NullMethodTransport() : RequestTargetTransport("OPTIONS", "*") {}
    const char* getMethodName() override { return nullptr; }
  } transport;

  RequestURI uri(nullptr, &transport, "", "/var/www/");
  EXPECT_TRUE(uri.done());
  EXPECT_EQ(transport.responseCode, 400);
  EXPECT_TRUE(transport.responseEnded);
}

TEST(RequestURITest, ValidatesTransportServerObjectInsteadOfRawUrl) {
  struct MappedTransport : RequestTargetTransport {
    MappedTransport() : RequestTargetTransport("GET", "*..%2Findex.php") {}
    const char* getServerObject() override {
      ++serverObjectCalls;
      return serverObjectCalls == 1 ? "/index.php" : "*..%2Findex.php";
    }
  } transport;

  EXPECT_THROW(
    RequestURI(nullptr, &transport, "", "/var/www/"),
    PathResolutionStarted
  );
  EXPECT_EQ(transport.serverObjectCalls, 1);
  EXPECT_EQ(transport.responseCode, 0);
}

TEST(RequestURITest, RejectsRelativeServerObjectWithValidRawUrl) {
  struct MappedTransport : RequestTargetTransport {
    MappedTransport() : RequestTargetTransport("GET", "/index.php") {}
    const char* getServerObject() override { return "..%2Findex.php"; }
  } transport;

  RequestURI uri(nullptr, &transport, "", "/var/www/");
  EXPECT_TRUE(uri.done());
  EXPECT_EQ(transport.responseCode, 400);
  EXPECT_TRUE(transport.responseEnded);
}

INSTANTIATE_TEST_SUITE_P(
  RequestTargets,
  RequestURITargetTest,
  testing::Values(
    RequestTargetCase{"GET", "/", true},
    RequestTargetCase{"HEAD", "/index.php?x=1", true},
    RequestTargetCase{"POST", "/upload", true},
    RequestTargetCase{"GET", "/..%2Findex.php", true},
    RequestTargetCase{"GET", "http://localhost/index.php?x=1", true},
    RequestTargetCase{"GET", "https://localhost:8081/", true},
    RequestTargetCase{"GET", "http://localhost", true},
    RequestTargetCase{"GET", "", true},
    RequestTargetCase{"GET", "foo/bar.php", true},
    RequestTargetCase{"GET", "http://localhost:bad/index.php", true},
    RequestTargetCase{"OPTIONS", "*", true},
    RequestTargetCase{"GET", "index.php", false},
    RequestTargetCase{"GET", "%2Findex.php", false},
    RequestTargetCase{"GET", "..%2Findex.php", false},
    RequestTargetCase{"GET", "*", false},
    RequestTargetCase{"POST", "*", false},
    RequestTargetCase{"OPTIONS", "*..%2Findex.php", false},
    RequestTargetCase{"GET",
      "*..%2F..%2F..%2F..%2Fopt%2Fapp%2Ftool.php", false},
    RequestTargetCase{"GET",
      "*..%2F..%2F..%2F..%2F..%2Fopt%2Fapp%2Ftool.php", false},
    RequestTargetCase{"POST", "*..%2F..%2Fopt%2Fapp%2Ftool.php", false}

  )
);

} // namespace
} // namespace HPHP
