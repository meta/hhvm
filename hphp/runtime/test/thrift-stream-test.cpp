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

#include <cstdint>
#include <exception>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <folly/ExceptionWrapper.h>
#include <folly/ScopeGuard.h>
#include <folly/io/IOBuf.h>
#include <folly/io/async/ScopedEventBaseThread.h>
#include <gtest/gtest.h>
#include <thrift/lib/cpp2/async/ClientStreamBridge.h>
#include <thrift/lib/cpp2/async/StreamCallbacks.h>
#include <thrift/lib/cpp2/async/StreamPayload.h>

#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/base/comparisons.h"
#include "hphp/runtime/base/execution-context.h"
#include "hphp/runtime/base/program-functions.h"
#include "hphp/runtime/base/runtime-option.h"
#include "hphp/runtime/base/type-array.h"
#include "hphp/runtime/base/type-object.h"
#include "hphp/runtime/ext/asio/asio-session.h"
#include "hphp/runtime/ext/asio/ext_asio.h"
#include "hphp/runtime/ext/asio/ext_external-thread-event-wait-handle.h"
#include "hphp/runtime/ext/asio/ext_wait-handle.h"
#include "hphp/runtime/ext/thrift/ext_thrift.h"
#include "hphp/runtime/vm/class.h"
#include "hphp/runtime/vm/func.h"
#include "hphp/runtime/vm/named-entity.h"
#include "hphp/runtime/vm/runtime-compiler.h"
#include "hphp/runtime/vm/unit-parser.h"
#include "hphp/runtime/vm/unit.h"
#include "hphp/system/systemlib.h"
#include "hphp/util/configs/eval.h"

namespace HPHP {
namespace {

struct FirstResponseCallback final
  : apache::thrift::detail::ClientStreamBridge::FirstResponseCallback {
  void onFirstResponse(
    apache::thrift::FirstResponsePayload&&,
    apache::thrift::detail::ClientStreamBridge::ClientPtr stream
  ) override {
    ptr = std::move(stream);
  }

  void onFirstResponseError(folly::exception_wrapper) override {
    std::terminate();
  }

  apache::thrift::detail::ClientStreamBridge::ClientPtr ptr;
};

struct ServerCallback final : apache::thrift::StreamServerCallback {
  bool onStreamRequestN(int32_t n) override {
    credits += n;
    requests.push_back(n);
    return true;
  }

  void onStreamCancel() override {
    canceled = true;
  }

  void resetClientCallback(apache::thrift::StreamClientCallback&) override {
    std::terminate();
  }

  int32_t credits = 0;
  std::vector<int32_t> requests;
  bool canceled = false;
};

struct ThriftStreamTest : testing::Test {
  void SetUp() override {
    oldFastPath = std::exchange(
      Cfg::Eval::EnableThriftStreamBufferedReadFastPath, true
    );
    client = apache::thrift::detail::ClientStreamBridge::create(&firstResponse);
    std::ignore = client->onFirstResponse(
      {nullptr, {}}, eventBase.getEventBase(), &server
    );
    object = thrift::TClientBufferedStream::newInstance();
    stream = thrift::TClientBufferedStream::GetDataOrThrowException(object.get());
    stream->init(std::move(firstResponse.ptr), {8, 0});
  }

  void TearDown() override {
    SCOPE_EXIT { Cfg::Eval::EnableThriftStreamBufferedReadFastPath = oldFastPath; };
    object = Object{};
    // Flush cancellation before destroying the server callback.
    flush();
  }

  void flush() {
    eventBase.getEventBase()->runInEventBaseThreadAndWait([] {});
  }
  void push(std::unique_ptr<folly::IOBuf> bytes, bool ordered = false) {
    eventBase.getEventBase()->runInEventBaseThreadAndWait([&] {
      EXPECT_TRUE(client->onStreamNext({std::move(bytes), {}, ordered}));
    });
  }
  void header() {
    eventBase.getEventBase()->runInEventBaseThreadAndWait([&] {
      EXPECT_TRUE(client->onStreamHeaders({{}, {}}));
    });
  }
  void complete() {
    eventBase.getEventBase()->runInEventBaseThreadAndWait([&] {
      client->onStreamComplete();
    });
  }
  void error(folly::exception_wrapper ex) {
    eventBase.getEventBase()->runInEventBaseThreadAndWait([&] {
      client->onStreamError(std::move(ex));
    });
  }
  void buffer() {
    ASSERT_TRUE(stream->queue_.empty());
    stream->queue_ = stream->streamBridge_->getMessages();
    ASSERT_FALSE(stream->queue_.empty());
  }
  Object genNext() {
    return thrift::HHVM_MN(TClientBufferedStream, genNext)(object.get());
  }
  static c_Awaitable* wh(const Object& handle) {
    return wait_handle<c_Awaitable>(handle.get());
  }

  folly::ScopedEventBaseThread eventBase;
  FirstResponseCallback firstResponse;
  ServerCallback server;
  apache::thrift::StreamClientCallback* client = nullptr;
  Object object;
  thrift::TClientBufferedStream* stream = nullptr;
  bool oldFastPath = false;
};

TEST(ThriftStreamConfig, EnabledByDefault) {
  EXPECT_TRUE(Cfg::Eval::EnableThriftStreamBufferedReadFastPath);
}

struct ThriftStreamHeaderTest : ThriftStreamTest,
                              testing::WithParamInterface<bool> {};

TEST_P(ThriftStreamHeaderTest, HeaderPreservesOpenStream) {
  Object pending;
  if (!GetParam()) pending = genNext();
  eventBase.getEventBase()->runInEventBaseThreadAndWait([&] {
    EXPECT_TRUE(client->onStreamHeaders({{}, {}}));
  });
  if (GetParam()) {
    stream->queue_ = stream->streamBridge_->getMessages();
    pending = genNext();
  }

  // With no payload or completion, an empty batch must keep the stream open.
  auto const result = HHVM_FN(join)(pending).toArray();
  ASSERT_TRUE(result[0].isArray());
  EXPECT_TRUE(result[0].toArray().empty());
  EXPECT_TRUE(result[1].isNull());
  EXPECT_TRUE(stream->streamBridge_);
  EXPECT_EQ(8, stream->outstanding_);
}

INSTANTIATE_TEST_SUITE_P(BufferedAndWaiting, ThriftStreamHeaderTest,
                        testing::Bool());

TEST_F(ThriftStreamTest, OrderedHeadersReplenishCredits) {
  // Exceed the initial credit budget using only ordered headers.
  for (int batch = 0; batch < 3; ++batch) {
    eventBase.getEventBase()->runInEventBaseThreadAndWait([&] {
      for (int i = 0; i < 4; ++i) {
        std::ignore = client->onStreamNext(
          {folly::IOBuf::create(0), {}, true}
        );
      }
    });
    auto pending = genNext();
    // Wait for queued RequestN callbacks before reading server.credits.
    eventBase.getEventBase()->runInEventBaseThreadAndWait([] {});
    auto const result = HHVM_FN(join)(pending).toArray();
    ASSERT_TRUE(result[0].isArray());
    EXPECT_TRUE(result[0].toArray().empty());
    EXPECT_TRUE(result[1].isNull());
    EXPECT_EQ(4, stream->outstanding_);
    EXPECT_EQ(batch * 4, server.credits);
  }

  // The refill threshold leaves a header and completion for the next drain.
  eventBase.getEventBase()->runInEventBaseThreadAndWait([&] {
    std::ignore = client->onStreamNext({folly::IOBuf::copyBuffer("A"), {}});
    EXPECT_TRUE(client->onStreamHeaders({{}, {}}));
    std::ignore = client->onStreamNext({folly::IOBuf::create(0), {}, true});
    std::ignore = client->onStreamNext({folly::IOBuf::copyBuffer("B"), {}});
    std::ignore = client->onStreamNext({folly::IOBuf::copyBuffer("C"), {}});
    EXPECT_TRUE(client->onStreamHeaders({{}, {}}));
    client->onStreamComplete();
  });
  auto result = HHVM_FN(join)(genNext()).toArray();
  EXPECT_TRUE(same(result[0], Variant{make_vec_array("A", "B", "C")}));
  EXPECT_TRUE(result[1].isNull());
  EXPECT_EQ(4, stream->outstanding_);
  EXPECT_TRUE(stream->streamBridge_);

  result = HHVM_FN(join)(genNext()).toArray();
  EXPECT_TRUE(result[0].isNull());
  EXPECT_TRUE(result[1].isNull());
  EXPECT_FALSE(stream->streamBridge_);
  EXPECT_FALSE(genNext());
}

TEST_F(ThriftStreamTest, EmptyCompletion) {
  auto pending = genNext();
  eventBase.getEventBase()->runInEventBaseThreadAndWait([&] {
    client->onStreamComplete();
  });
  auto const result = HHVM_FN(join)(pending).toArray();
  EXPECT_TRUE(result[0].isNull());
  EXPECT_TRUE(result[1].isNull());
  EXPECT_FALSE(genNext());
}

TEST_F(ThriftStreamTest, PayloadBeforeError) {
  eventBase.getEventBase()->runInEventBaseThreadAndWait([&] {
    EXPECT_TRUE(client->onStreamHeaders({{}, {}}));
    std::ignore = client->onStreamNext({folly::IOBuf::copyBuffer("value"), {}});
    client->onStreamError(
      folly::make_exception_wrapper<std::runtime_error>("stream error")
    );
  });
  auto const result = HHVM_FN(join)(genNext()).toArray();
  ASSERT_TRUE(result[0].isArray());
  auto const values = result[0].toArray();
  ASSERT_EQ(1, values.size());
  EXPECT_EQ("value", values[0].toString().toCppString());
  ASSERT_TRUE(result[1].isString());
  EXPECT_NE(std::string::npos,
            result[1].toString().toCppString().find("stream error"));
  EXPECT_FALSE(genNext());
}

TEST_F(ThriftStreamTest, PayloadBeforeEncodedError) {
  eventBase.getEventBase()->runInEventBaseThreadAndWait([&] {
    EXPECT_TRUE(client->onStreamHeaders({{}, {}}));
    std::ignore = client->onStreamNext({folly::IOBuf::copyBuffer("value"), {}});
    client->onStreamError(
      folly::make_exception_wrapper<apache::thrift::detail::EncodedError>(
        folly::IOBuf::copyBuffer("encoded error")
      )
    );
  });
  auto const result = HHVM_FN(join)(genNext()).toArray();
  ASSERT_TRUE(result[0].isArray());
  auto const values = result[0].toArray();
  ASSERT_EQ(2, values.size());
  EXPECT_EQ("value", values[0].toString().toCppString());
  EXPECT_EQ("encoded error", values[1].toString().toCppString());
  EXPECT_TRUE(result[1].isNull());
  EXPECT_FALSE(genNext());
}

TEST_F(ThriftStreamTest, CancelPendingRead) {
  auto pending = genNext();
  auto exception = SystemLib::AllocInvalidOperationExceptionObject("cancelled");
  auto handle = wait_handle<c_ExternalThreadEventWaitHandle>(pending.get());
  ASSERT_TRUE(handle->cancel(exception));

  object = Object{};
  eventBase.getEventBase()->runInEventBaseThreadAndWait([] {});
  EXPECT_TRUE(server.canceled);
  try {
    HHVM_FN(join)(pending);
    FAIL() << "Cancelled read succeeded";
  } catch (const Object& thrown) {
    EXPECT_EQ(exception.get(), thrown.get());
  }
}

enum class StreamReply {
  Value, EmptyValue, Header, OrderedHeader, End, ValueEnd,
  Error, ValueError, EncodedError, ValueEncodedError
};
struct StreamScenario {
  bool enabled;
  bool buffered;
  StreamReply reply;
};

std::vector<StreamScenario> streamScenarios() {
  std::vector<StreamScenario> scenarios;
  for (auto enabled : {false, true}) {
    for (auto buffered : {false, true}) {
      for (auto reply : {
        StreamReply::Value, StreamReply::EmptyValue, StreamReply::Header,
        StreamReply::OrderedHeader, StreamReply::End, StreamReply::ValueEnd,
        StreamReply::Error, StreamReply::ValueError,
        StreamReply::EncodedError, StreamReply::ValueEncodedError
      }) {
        scenarios.push_back({enabled, buffered, reply});
      }
    }
  }
  return scenarios;
}

struct ThriftStreamProtocolTest : ThriftStreamTest,
  testing::WithParamInterface<StreamScenario> {};

TEST_P(ThriftStreamProtocolTest, ValueCreditsAndClosure) {
  auto const [enabled, buffered, reply] = GetParam();
  Cfg::Eval::EnableThriftStreamBufferedReadFastPath = enabled;
  auto values = empty_vec_array();
  auto errorStr = null_string;
  int consumed = 0;
  bool terminal = false;
  auto const value = std::string{"value\0bytes", 11};
  auto const encoded = std::string{"encoded\0error", 13};
  auto const addValue = [&] {
    auto bytes = folly::IOBuf::copyBuffer(value.data(), 5);
    bytes->appendChain(folly::IOBuf::copyBuffer(value.data() + 5, value.size() - 5));
    push(std::move(bytes));
    values.append(Variant{String{value}});
    ++consumed;
  };
  switch (reply) {
    case StreamReply::Value: addValue(); break;
    case StreamReply::EmptyValue: push(folly::IOBuf::create(0)); break;
    case StreamReply::Header: header(); break;
    case StreamReply::OrderedHeader:
      push(folly::IOBuf::create(0), true);
      ++consumed;
      break;
    case StreamReply::ValueEnd: addValue(); [[fallthrough]];
    case StreamReply::End: complete(); terminal = true; break;
    case StreamReply::ValueError: addValue(); [[fallthrough]];
    case StreamReply::Error: {
      auto ex = folly::make_exception_wrapper<std::runtime_error>("stream error");
      errorStr = String{ex.what().toStdString()};
      error(std::move(ex));
      terminal = true;
      break;
    }
    case StreamReply::ValueEncodedError: addValue(); [[fallthrough]];
    case StreamReply::EncodedError:
      error(folly::make_exception_wrapper<apache::thrift::detail::EncodedError>(
        folly::IOBuf::copyBuffer(encoded)
      ));
      values.append(Variant{String{encoded}});
      terminal = true;
      break;
  }
  if (terminal && values.empty()) values = null_array;
  if (buffered) buffer();
  auto const handle = genNext();
  EXPECT_EQ(enabled && buffered, wh(handle)->isFinished());
  EXPECT_EQ(enabled && buffered, wh(handle)->isSucceeded());
  EXPECT_EQ(enabled && buffered ? c_Awaitable::Kind::Static
                                : c_Awaitable::Kind::ExternalThreadEvent,
            wh(handle)->getKind());
  EXPECT_TRUE(same(HHVM_FN(join)(handle),
                   Variant{make_vec_array(values, errorStr)}));
  EXPECT_EQ(8 - consumed, stream->outstanding_);
  EXPECT_EQ(!terminal, bool(stream->streamBridge_));
  flush();
  EXPECT_EQ(0, server.credits);
  if (terminal) EXPECT_FALSE(genNext());
}

INSTANTIATE_TEST_SUITE_P(EnabledDisabledLocalBridgeBuffered,
  ThriftStreamProtocolTest, testing::ValuesIn(streamScenarios()));

struct ThriftStreamOptionTest : ThriftStreamTest,
                               testing::WithParamInterface<bool> {};

TEST_P(ThriftStreamOptionTest, BufferedRemaindersConserveCreditsAndBoundInlineWork) {
  auto const enabled = GetParam();
  Cfg::Eval::EnableThriftStreamBufferedReadFastPath = enabled;
  for (int i = 0; i < 8; ++i) {
    push(folly::IOBuf::copyBuffer(std::string(1, 'A' + i)));
  }
  buffer();
  int received = 0;
  std::vector<int32_t> requests;
  for (int batch = 0; batch < 4; ++batch) {
    if (batch >= 2) {
      for (int i = 0; i < 4; ++i) {
        push(folly::IOBuf::copyBuffer(std::string(1, 'A' + batch * 4 + i)));
      }
      buffer();
    }
    auto const handle = genNext();
    EXPECT_EQ(enabled && batch % 2 == 0, wh(handle)->isSucceeded());
    EXPECT_EQ(enabled && batch % 2 == 0 ? c_Awaitable::Kind::Static
                                       : c_Awaitable::Kind::ExternalThreadEvent,
              wh(handle)->getKind());
    VecInit expected(4);
    for (int i = 0; i < 4; ++i) {
      expected.append(Variant{String{std::string(1, char('A' + batch * 4 + i))}});
    }
    EXPECT_TRUE(same(HHVM_FN(join)(handle),
      Variant{make_vec_array(expected.toArray(), init_null_variant)}));
    received += 4;
    if (batch) requests.push_back(4);
    flush();
    EXPECT_EQ(requests, server.requests);
    EXPECT_EQ(8 + server.credits - received, stream->outstanding_);
    EXPECT_EQ(4, stream->outstanding_);
    EXPECT_EQ(4, stream->payloadDataSize_);
    EXPECT_EQ(batch == 0, !stream->queue_.empty());
  }
}

TEST_P(ThriftStreamOptionTest, PayloadThresholdRefillsBeforeBufferedDrain) {
  Cfg::Eval::EnableThriftStreamBufferedReadFastPath = false;
  auto const large = std::string(16 * 1024, 'x');
  push(folly::IOBuf::copyBuffer(large));
  push(folly::IOBuf::copyBuffer("next"));
  buffer();
  auto const first = genNext();
  EXPECT_EQ(c_Awaitable::Kind::ExternalThreadEvent, wh(first)->getKind());
  EXPECT_TRUE(same(HHVM_FN(join)(first),
    Variant{make_vec_array(make_vec_array(String{large}), init_null_variant)}));
  ASSERT_FALSE(stream->queue_.empty());
  EXPECT_EQ(7, stream->outstanding_);
  EXPECT_EQ(16 * 1024, stream->payloadDataSize_);
  flush();
  EXPECT_EQ(0, server.credits);

  auto const enabled = GetParam();
  Cfg::Eval::EnableThriftStreamBufferedReadFastPath = enabled;
  auto const second = genNext();
  EXPECT_EQ(enabled, wh(second)->isSucceeded());
  EXPECT_EQ(enabled ? c_Awaitable::Kind::Static
                    : c_Awaitable::Kind::ExternalThreadEvent,
            wh(second)->getKind());
  EXPECT_TRUE(same(HHVM_FN(join)(second),
    Variant{make_vec_array(make_vec_array("next"), init_null_variant)}));
  flush();
  EXPECT_EQ((std::vector<int32_t>{1}), server.requests);
  EXPECT_EQ(8 + server.credits - 2, stream->outstanding_);
  EXPECT_EQ(7, stream->outstanding_);
  EXPECT_EQ(4, stream->payloadDataSize_);
  EXPECT_TRUE(stream->queue_.empty());
}

TEST_P(ThriftStreamOptionTest, PendingUntilPayloadAndResetAfterPending) {
  auto const enabled = GetParam();
  Cfg::Eval::EnableThriftStreamBufferedReadFastPath = enabled;
  auto const initial = genNext();
  EXPECT_EQ(c_Awaitable::Kind::ExternalThreadEvent, wh(initial)->getKind());
  EXPECT_FALSE(wh(initial)->isFinished());
  flush();
  EXPECT_FALSE(wh(initial)->isFinished());
  push(folly::IOBuf::copyBuffer("initial"));
  EXPECT_TRUE(same(HHVM_FN(join)(initial),
    Variant{make_vec_array(make_vec_array("initial"), init_null_variant)}));

  push(folly::IOBuf::copyBuffer("first"));
  buffer();
  auto const first = genNext();
  EXPECT_EQ(enabled, wh(first)->isSucceeded());
  EXPECT_TRUE(same(HHVM_FN(join)(first),
    Variant{make_vec_array(make_vec_array("first"), init_null_variant)}));

  auto const pending = genNext();
  EXPECT_EQ(c_Awaitable::Kind::ExternalThreadEvent, wh(pending)->getKind());
  EXPECT_FALSE(wh(pending)->isFinished());
  flush();
  EXPECT_FALSE(wh(pending)->isFinished());
  push(folly::IOBuf::copyBuffer("arrival"));
  EXPECT_TRUE(same(HHVM_FN(join)(pending),
    Variant{make_vec_array(make_vec_array("arrival"), init_null_variant)}));

  push(folly::IOBuf::copyBuffer("ready"));
  buffer();
  auto const ready = genNext();
  EXPECT_EQ(enabled, wh(ready)->isSucceeded());
  EXPECT_TRUE(same(HHVM_FN(join)(ready),
    Variant{make_vec_array(make_vec_array("ready"), init_null_variant)}));
}

TEST_P(ThriftStreamOptionTest, BridgeBufferedQueueRetainsExternalEvent) {
  Cfg::Eval::EnableThriftStreamBufferedReadFastPath = GetParam();
  push(folly::IOBuf::copyBuffer("bridge buffered"));
  ASSERT_TRUE(stream->queue_.empty());
  auto const handle = genNext();
  EXPECT_EQ(c_Awaitable::Kind::ExternalThreadEvent, wh(handle)->getKind());
  EXPECT_FALSE(wh(handle)->isFinished());
  EXPECT_TRUE(same(HHVM_FN(join)(handle),
    Variant{make_vec_array(make_vec_array("bridge buffered"), init_null_variant)}));
}

TEST_P(ThriftStreamOptionTest, ResultOwnership) {
  auto const enabled = GetParam();
  Cfg::Eval::EnableThriftStreamBufferedReadFastPath = enabled;
  auto const bytes = std::string{"owned\0bytes", 11};
  push(folly::IOBuf::copyBuffer(bytes));
  buffer();
  auto handle = genNext();
  EXPECT_EQ(enabled, wh(handle)->isSucceeded());
  auto const result = HHVM_FN(join)(handle);
  EXPECT_TRUE(handle->hasExactlyOneRef());
  auto const* tuple = wh(handle)->getResult().m_data.parr;
  EXPECT_TRUE(tuple->hasMultipleRefs());
  auto const expected = Variant{
    make_vec_array(make_vec_array(String{bytes}), init_null_variant)
  };
  EXPECT_TRUE(same(result, expected));
  handle = Object{};
  EXPECT_TRUE(tuple->hasExactlyOneRef());
  EXPECT_TRUE(same(result, expected));
}

INSTANTIATE_TEST_SUITE_P(EnabledDisabled, ThriftStreamOptionTest, testing::Bool());

TEST_F(ThriftStreamTest, DisablingResetsInlineBudget) {
  header();
  buffer();
  auto const first = genNext();
  EXPECT_EQ(c_Awaitable::Kind::Static, wh(first)->getKind());
  EXPECT_TRUE(same(HHVM_FN(join)(first),
    Variant{make_vec_array(empty_vec_array(), init_null_variant)}));
  Cfg::Eval::EnableThriftStreamBufferedReadFastPath = false;
  header();
  buffer();
  auto const disabled = genNext();
  EXPECT_EQ(c_Awaitable::Kind::ExternalThreadEvent, wh(disabled)->getKind());
  EXPECT_TRUE(same(HHVM_FN(join)(disabled),
    Variant{make_vec_array(empty_vec_array(), init_null_variant)}));
  Cfg::Eval::EnableThriftStreamBufferedReadFastPath = true;
  header();
  buffer();
  auto const reenabled = genNext();
  EXPECT_EQ(c_Awaitable::Kind::Static, wh(reenabled)->getKind());
  EXPECT_TRUE(same(HHVM_FN(join)(reenabled),
    Variant{make_vec_array(empty_vec_array(), init_null_variant)}));
}

StaticString s_events("events");
constexpr auto kHelpers = R"HACK(<?hh
final class TTransportException extends Exception {}
function stream_test_state(): stdClass {
  $state = new stdClass();
  $state->events = vec[];
  return $state;
}
function stream_test_io_hooks(): vec<mixed> {
  return vec[() ==> {}, ($_handle) ==> {}];
}
function stream_test_hooks(stdClass $state): vec<mixed> {
  return vec[
    ($h) ==> { $state->events[] = tuple('create', HH\Asio\has_finished($h)); },
    ($h, $result, $finish) ==> {
      $state->events[] = tuple('success', HH\Asio\has_finished($h), $result, $finish);
    },
    ($h, $_exception, $finish) ==> {
      $state->events[] = tuple('fail', HH\Asio\has_finished($h), $finish);
    },
  ];
}
function stream_test_join_error(Awaitable<mixed> $handle): vec<mixed> {
  try {
    HH\Asio\join($handle);
    return vec['succeeded'];
  } catch (Exception $e) {
    return vec[get_class($e), $e->getMessage(), $e->getCode()];
  }
}
async function stream_test_output(Awaitable<mixed> $gate, stdClass $state): Awaitable<void> {
  await $gate;
  $state->events[] = 'output';
}
async function stream_test_input(TClientBufferedStream $stream, stdClass $state): Awaitable<void> {
  invariant(await $stream->genNext() === tuple(vec['value'], null),
            'Expected one actual native stream value');
  $state->events[] = 'input';
}
function stream_test_start(TClientBufferedStream $stream, stdClass $state): vec<mixed> {
  $work = vec[];
  $conditions = vec[];
  for ($i = 0; $i < 4; ++$i) {
    $child = HH\RescheduleWaitHandle::create(HH\RescheduleWaitHandle::QUEUE_DEFAULT, 0);
    $gate = HH\ConditionWaitHandle::create($child);
    $work[] = stream_test_output($gate, $state);
    $work[] = $child;
    $conditions[] = $gate;
  }
  foreach ($conditions as $condition) $condition->succeed(null);
  $input = stream_test_input($stream, $state);
  $work[] = $input;
  return vec[$input, HH\AwaitAllWaitHandle::fromVec($work)];
}
function stream_test_join(vec<Awaitable<mixed>> $work): void {
  HH\Asio\join(HH\AwaitAllWaitHandle::fromVec($work));
}
)HACK";

struct ThriftStreamSchedulerTest : ThriftStreamTest {
  void SetUp() override {}
  void TearDown() override {}

  void run(const std::function<void()>& body) {
    std::thread([&] {
      HphpSessionAndThread request{Treadmill::SessionKind::UnitTests};
      ASSERT_TRUE(rds::local::tcCheck());
      ThriftStreamTest::SetUp();
      SCOPE_EXIT { shutdownRequest(); };
      unit = compile_string(kHelpers, CodeSource::User,
        "/tmp/hhvm-thrift-stream-scheduler.php", nullptr, nullptr,
        RepoOptions::defaults());
      ASSERT_TRUE(unit);
      unit->merge();
      if (!HasFatalFailure()) body();
    }).join();
  }

  void shutdownRequest() {
    auto const session = AsioSession::Get();
    session->setOnExternalThreadEventCreate(init_null_variant);
    session->setOnExternalThreadEventSuccess(init_null_variant);
    session->setOnExternalThreadEventFail(init_null_variant);
    session->setOnIOWaitEnter(init_null_variant);
    session->setOnIOWaitExit(init_null_variant);
    ThriftStreamTest::TearDown();
    if (!unit) return;
    for (auto const func : unit->funcs()) {
      auto const named = func->getNamedFunc();
      if (named->getCachedFunc() == func) named->setCachedFunc(nullptr);
    }
    auto const preclasses = unit->preclasses();
    for (size_t i = preclasses.size(); i > 0; --i) {
      auto const& preclass = preclasses[i - 1];
      auto const named = preclass->namedType();
      auto const cls = named->getCachedClass();
      if (cls && cls->preClass() == preclass.get()) {
        named->setCachedClass(nullptr);
        cls->destroy();
      }
    }
    unit->startDestroy();
  }
  Variant invoke(const char* name, const Array& args = empty_vec_array()) {
    return Variant::attach(g_context->invokeFunc(
      Func::lookup(makeStaticString(name)), args, nullptr, nullptr, nullptr,
      RuntimeCoeffects::fixme(), false));
  }
  Object state() { return invoke("stream_test_state").toObject(); }
  Array events(const Object& state) { return state.toArray()[s_events].toArray(); }
  Array start(const Object& state) {
    return invoke("stream_test_start", make_vec_array(object, state)).toArray();
  }
  void join(const Array& work) { invoke("stream_test_join", make_vec_array(work)); }
  Unit* unit = nullptr;
};

struct StreamHookScenario {
  bool enabled;
  int hook;
};
constexpr StreamHookScenario kStreamHookScenarios[] = {
  {false, 0}, {false, 1}, {false, 2}, {true, 0}, {true, 1}, {true, 2}
};
struct ThriftStreamHookTest : ThriftStreamSchedulerTest,
                            testing::WithParamInterface<StreamHookScenario> {};
TEST_P(ThriftStreamHookTest, RetainsActualExternalEvent) {
  run([&] {
    auto const [enabled, hook] = GetParam();
    Cfg::Eval::EnableThriftStreamBufferedReadFastPath = enabled;
    auto const recorded = state();
    auto const hooks = invoke("stream_test_hooks", make_vec_array(recorded)).toArray();
    auto const session = AsioSession::Get();
    switch (hook) {
      case 0: session->setOnExternalThreadEventCreate(hooks[0]); break;
      case 1: session->setOnExternalThreadEventSuccess(hooks[1]); break;
      case 2: session->setOnExternalThreadEventFail(hooks[2]); break;
    }
    push(folly::IOBuf::copyBuffer("hook value"));
    buffer();
    auto const handle = genNext();
    EXPECT_EQ(c_Awaitable::Kind::ExternalThreadEvent, wh(handle)->getKind());
    EXPECT_FALSE(wh(handle)->isFinished());
    auto const expected = Variant{
      make_vec_array(make_vec_array("hook value"), init_null_variant)
    };
    EXPECT_TRUE(same(HHVM_FN(join)(handle), expected));
    auto const observed = events(recorded);
    switch (hook) {
      case 0:
        EXPECT_TRUE(same(Variant{observed},
          Variant{make_vec_array(make_vec_array("create", false))}));
        break;
      case 1: {
        ASSERT_EQ(1, observed.size());
        auto const success = observed[0].toArray();
        EXPECT_EQ("success", success[0].toString().toCppString());
        EXPECT_TRUE(success[1].toBoolean());
        EXPECT_TRUE(same(success[2], expected));
        EXPECT_GT(success[3].toInt64(), 0);
        break;
      }
      case 2: EXPECT_TRUE(observed.empty()); break;
    }
  });
}
INSTANTIATE_TEST_SUITE_P(EnabledDisabledCreateSuccessFail, ThriftStreamHookTest,
                        testing::ValuesIn(kStreamHookScenarios));

TEST_F(ThriftStreamSchedulerTest, DefaultIOCallbacksAllowBufferedCompletion) {
  run([&] {
    auto const hooks = invoke("stream_test_io_hooks").toArray();
    auto const session = AsioSession::Get();
    session->setOnIOWaitEnter(hooks[0]);
    session->setOnIOWaitExit(hooks[1]);
    push(folly::IOBuf::copyBuffer("value"));
    buffer();
    auto const handle = genNext();
    EXPECT_TRUE(wh(handle)->isSucceeded());
    EXPECT_TRUE(same(HHVM_FN(join)(handle),
      Variant{make_vec_array(make_vec_array("value"), init_null_variant)}));
  });
}

struct ThriftStreamSchedulerOptionTest : ThriftStreamSchedulerTest,
                                        testing::WithParamInterface<bool> {};

TEST_P(ThriftStreamSchedulerOptionTest, UnexpectedServerErrorStillThrows) {
  run([&] {
    Cfg::Eval::EnableThriftStreamBufferedReadFastPath = GetParam();
    auto const handle = genNext();
    EXPECT_EQ(c_Awaitable::Kind::ExternalThreadEvent, wh(handle)->getKind());
    EXPECT_FALSE(wh(handle)->isFinished());
    object = Object{};
    flush();
    EXPECT_TRUE(server.canceled);
    EXPECT_TRUE(same(invoke("stream_test_join_error", make_vec_array(handle)),
      Variant{make_vec_array("TTransportException",
                            "Something went wrong, queue is empty", 0)}));
  });
}

TEST_P(ThriftStreamSchedulerOptionTest, CancelPendingReadAndFailHook) {
  run([&] {
    Cfg::Eval::EnableThriftStreamBufferedReadFastPath = GetParam();
    auto const recorded = state();
    auto const hooks = invoke("stream_test_hooks", make_vec_array(recorded)).toArray();
    AsioSession::Get()->setOnExternalThreadEventFail(hooks[2]);
    auto const handle = genNext();
    auto const exception = SystemLib::AllocInvalidOperationExceptionObject("cancelled");
    ASSERT_TRUE(wait_handle<c_ExternalThreadEventWaitHandle>(handle.get())->cancel(exception));
    EXPECT_TRUE(same(Variant{events(recorded)},
                     Variant{make_vec_array(make_vec_array("fail", true, 0))}));
    object = Object{};
    flush();
    EXPECT_TRUE(server.canceled);
    try {
      HHVM_FN(join)(handle);
      FAIL() << "Canceled read succeeded";
    } catch (const Object& thrown) { EXPECT_EQ(exception.get(), thrown.get()); }
  });
}

TEST_P(ThriftStreamSchedulerOptionTest, BufferedBatchConditionOrderFollowsOption) {
  run([&] {
    auto const enabled = GetParam();
    Cfg::Eval::EnableThriftStreamBufferedReadFastPath = enabled;
    push(folly::IOBuf::copyBuffer("value"));
    buffer();
    auto const recorded = state();
    auto const work = start(recorded);
    EXPECT_EQ(enabled, wh(work[0].toObject())->isSucceeded());
    EXPECT_TRUE(same(Variant{events(recorded)}, Variant{enabled
      ? make_vec_array("input") : empty_vec_array()}));
    join(make_vec_array(work[1]));
    auto const expected = enabled
      ? make_vec_array("input", "output", "output", "output", "output")
      : make_vec_array("output", "output", "output", "output", "input");
    EXPECT_TRUE(same(Variant{events(recorded)}, Variant{expected}));
  });
}

TEST_P(ThriftStreamSchedulerOptionTest, ReadyRefillsYieldToOutstandingOutput) {
  run([&] {
    auto const enabled = GetParam();
    Cfg::Eval::EnableThriftStreamBufferedReadFastPath = enabled;
    auto const recorded = state();
    push(folly::IOBuf::copyBuffer("value"));
    buffer();
    auto const first = start(recorded);
    EXPECT_EQ(enabled, wh(first[0].toObject())->isSucceeded());
    push(folly::IOBuf::copyBuffer("value"));
    buffer();
    auto const second = start(recorded);
    EXPECT_FALSE(wh(second[0].toObject())->isFinished());
    EXPECT_TRUE(same(Variant{events(recorded)}, Variant{enabled
      ? make_vec_array("input") : empty_vec_array()}));
    join(make_vec_array(first[1], second[1]));
    auto const expected = enabled
      ? make_vec_array("input", "output", "output", "output", "output",
                       "output", "output", "output", "output", "input")
      : make_vec_array("output", "output", "output", "output", "output",
                       "output", "output", "output", "input", "input");
    EXPECT_TRUE(same(Variant{events(recorded)}, Variant{expected}));
  });
}

INSTANTIATE_TEST_SUITE_P(EnabledDisabled, ThriftStreamSchedulerOptionTest,
                        testing::Bool());
}
}
