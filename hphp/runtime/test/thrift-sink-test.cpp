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

#include <exception>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>

#include <folly/ExceptionWrapper.h>
#include <folly/ScopeGuard.h>
#include <folly/io/IOBuf.h>
#include <folly/io/async/ScopedEventBaseThread.h>
#include <gtest/gtest.h>
#include <thrift/lib/cpp2/async/ClientSinkBridge.h>
#include <thrift/lib/cpp2/async/StreamCallbacks.h>

#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/base/comparisons.h"
#include "hphp/runtime/base/execution-context.h"
#include "hphp/runtime/base/program-functions.h"
#include "hphp/runtime/base/runtime-option.h"
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
namespace thrift {
Object HHVM_METHOD(TClientSink, genCreditsOrFinalResponse);
}
namespace {
using SinkBridge = apache::thrift::detail::ClientSinkBridge;
using StreamPayload = apache::thrift::StreamPayload;
using SinkMessage = apache::thrift::detail::ClientSinkMessageServerToClient;
StaticString s_events("events");

// The server cannot legally send anything after a terminal message, so a batch
// with a trailing suffix can only be built by hand.
void append(SinkBridge::ClientQueue& queue, SinkMessage&& message) {
  auto** slot = &queue.head_;
  while (*slot) slot = &(*slot)->next;
  *slot = new SinkBridge::ClientQueue::Node(std::move(message));
}

struct SinkFirstResponse final : SinkBridge::FirstResponseCallback {
  void onFirstResponse(
    apache::thrift::FirstResponsePayload&&, SinkBridge::ClientPtr sink
  ) override { ptr = std::move(sink); }
  void onFirstResponseError(folly::exception_wrapper) override {
    std::terminate();
  }
  SinkBridge::ClientPtr ptr;
};

struct SinkServer final : apache::thrift::SinkServerCallback {
  bool onSinkNext(StreamPayload&&) override { ++inputs; return true; }
  void onSinkError(folly::exception_wrapper) override { ++cancellations; }
  bool onSinkComplete() override { return true; }
  void resetClientCallback(apache::thrift::SinkClientCallback&) override {
    std::terminate();
  }
  int inputs = 0;
  int cancellations = 0;
};

struct ThriftSinkTest : testing::Test {
  void SetUp() override {
    oldFastPath = std::exchange(Cfg::Eval::EnableThriftSinkBufferedReadFastPath, true);
    client = SinkBridge::create(&first);
    std::ignore = client->onFirstResponse(
      {nullptr, {}}, evb.getEventBase(), &server
    );
    object = thrift::TClientSink::newInstance();
    sink = thrift::TClientSink::GetDataOrThrowException(object.get());
    sink->init(std::move(first.ptr));
  }
  void TearDown() override {
    SCOPE_EXIT { Cfg::Eval::EnableThriftSinkBufferedReadFastPath = oldFastPath; };
    object = Object{};
    flush();
  }
  void flush() { evb.getEventBase()->runInEventBaseThreadAndWait([] {}); }
  Object genCredits() {
    return thrift::HHVM_MN(TClientSink, genCreditsOrFinalResponse)(object.get());
  }
  void grant(int32_t n) {
    evb.getEventBase()->runInEventBaseThreadAndWait([&] {
      EXPECT_TRUE(client->onSinkRequestN(n));
    });
  }
  void finish(std::unique_ptr<folly::IOBuf> bytes) {
    evb.getEventBase()->runInEventBaseThreadAndWait([&] {
      client->onFinalResponse({std::move(bytes), {}});
    });
  }
  void error(folly::exception_wrapper ex) {
    evb.getEventBase()->runInEventBaseThreadAndWait([&] {
      client->onFinalResponseError(std::move(ex));
    });
  }
  static c_Awaitable* wh(const Object& handle) {
    return wait_handle<c_Awaitable>(handle.get());
  }
  folly::ScopedEventBaseThread evb;
  SinkFirstResponse first;
  SinkServer server;
  apache::thrift::SinkClientCallback* client = nullptr;
  Object object;
  thrift::TClientSink* sink = nullptr;
  bool oldFastPath = false;
};

TEST(ThriftSinkConfig, DisabledByDefault) {
  EXPECT_FALSE(Cfg::Eval::EnableThriftSinkBufferedReadFastPath);
}

enum class Reply { Credits, Zero, Final, EmptyFinal, EncodedError, Error };
struct ThriftSinkProtocolTest : ThriftSinkTest,
  testing::WithParamInterface<std::tuple<bool, bool, Reply>> {};

TEST_P(ThriftSinkProtocolTest, ValueAndClosure) {
  auto const [enabled, buffered, reply] = GetParam();
  Cfg::Eval::EnableThriftSinkBufferedReadFastPath = enabled;
  Object handle;
  if (!buffered) handle = genCredits();
  auto const terminal = reply != Reply::Credits && reply != Reply::Zero;
  if (buffered && terminal) grant(9);
  uint64_t credits = 0;
  auto response = null_string;
  auto errorStr = null_string;
  switch (reply) {
    case Reply::Credits: grant(7); credits = 7; break;
    case Reply::Zero: grant(0); break;
    case Reply::Final: {
      auto const bytes = std::string{"final\0response", 14};
      response = String{bytes};
      finish(folly::IOBuf::copyBuffer(bytes));
      break;
    }
    case Reply::EmptyFinal: finish(nullptr); break;
    case Reply::EncodedError: {
      auto const bytes = std::string{"encoded\0error", 13};
      response = String{bytes};
      error(folly::make_exception_wrapper<apache::thrift::detail::EncodedError>(
        folly::IOBuf::copyBuffer(bytes)
      ));
      break;
    }
    case Reply::Error: {
      auto ex = folly::make_exception_wrapper<std::runtime_error>("sink error");
      errorStr = String{ex.what().toStdString()};
      error(std::move(ex));
      break;
    }
  }
  if (buffered) handle = genCredits();
  EXPECT_EQ(enabled && buffered, wh(handle)->isSucceeded());
  EXPECT_EQ(enabled && buffered ? c_Awaitable::Kind::Static
                                : c_Awaitable::Kind::ExternalThreadEvent,
            wh(handle)->getKind());
  EXPECT_TRUE(same(HHVM_FN(join)(handle),
                   Variant{make_vec_array(credits, response, errorStr)}));
  EXPECT_EQ(!terminal, bool(sink->sinkBridge_));
  if (terminal) EXPECT_FALSE(genCredits());
}

INSTANTIATE_TEST_SUITE_P(EnabledDisabledBufferedWaiting, ThriftSinkProtocolTest,
  testing::Combine(testing::Bool(), testing::Bool(), testing::Values(
    Reply::Credits, Reply::Zero, Reply::Final, Reply::EmptyFinal,
    Reply::EncodedError, Reply::Error
  )));

TEST_F(ThriftSinkTest, OwnedBatchStopsAtFirstTerminal) {
  grant(3);
  finish(folly::IOBuf::copyBuffer("first"));
  auto messages = sink->sinkBridge_->getMessages();
  ASSERT_FALSE(messages.empty());
  append(messages, SinkMessage{apache::thrift::StreamMessage::RequestN{99}});
  append(messages, SinkMessage{apache::thrift::StreamMessage::PayloadOrError{
    folly::Try<StreamPayload>{
      folly::make_exception_wrapper<std::runtime_error>("ignored suffix")
    }
  }});
  auto const response = sink->getCreditsOrFinalResponse(std::move(messages));
  auto const& payload = std::get<std::unique_ptr<folly::IOBuf>>(response);
  EXPECT_EQ("first", ioBufToString(*payload).toCppString());
  EXPECT_FALSE(sink->sinkBridge_);
  EXPECT_FALSE(genCredits());
}

struct ThriftSinkOptionTest : ThriftSinkTest,
                             testing::WithParamInterface<bool> {};

TEST_P(ThriftSinkOptionTest, BufferedReadsConserveCreditsAndBoundInlineWork) {
  auto const enabled = GetParam();
  Cfg::Eval::EnableThriftSinkBufferedReadFastPath = enabled;
  uint64_t granted = 0;
  uint64_t received = 0;
  for (int i = 0; i < 4; ++i) {
    grant(i + 1);
    granted += i + 1;
    auto const handle = genCredits();
    // Only every other buffered read may skip the external-event boundary.
    EXPECT_EQ(enabled && i % 2 == 0 ? c_Awaitable::Kind::Static
                                    : c_Awaitable::Kind::ExternalThreadEvent,
              wh(handle)->getKind());
    received += HHVM_FN(join)(handle).toArray()[0].toInt64();
  }
  EXPECT_EQ(granted, received);
}

TEST_P(ThriftSinkOptionTest, AccumulatedCreditsAndResultOwnership) {
  auto const enabled = GetParam();
  Cfg::Eval::EnableThriftSinkBufferedReadFastPath = enabled;
  grant(3);
  grant(0);
  grant(5);
  auto handle = genCredits();
  EXPECT_EQ(enabled, wh(handle)->isSucceeded());
  auto const result = HHVM_FN(join)(handle);
  EXPECT_TRUE(handle->hasExactlyOneRef());
  auto const* tuple = wh(handle)->getResult().m_data.parr;
  EXPECT_TRUE(tuple->hasMultipleRefs());
  EXPECT_TRUE(same(result, Variant{make_vec_array(8, init_null_variant, init_null_variant)}));
  handle = Object{};
  EXPECT_TRUE(tuple->hasExactlyOneRef());
  EXPECT_TRUE(same(result, Variant{make_vec_array(8, init_null_variant, init_null_variant)}));
}

TEST_P(ThriftSinkOptionTest, PendingUntilGrantAndResetAfterPending) {
  auto const enabled = GetParam();
  Cfg::Eval::EnableThriftSinkBufferedReadFastPath = enabled;
  auto pending = genCredits();
  EXPECT_FALSE(wh(pending)->isFinished());
  flush();
  EXPECT_FALSE(wh(pending)->isFinished());
  grant(1);
  EXPECT_TRUE(same(HHVM_FN(join)(pending),
                   Variant{make_vec_array(1, init_null_variant, init_null_variant)}));
  grant(1);
  auto const ready = genCredits();
  EXPECT_EQ(enabled, wh(ready)->isSucceeded());
  EXPECT_TRUE(same(HHVM_FN(join)(ready),
                   Variant{make_vec_array(1, init_null_variant, init_null_variant)}));
}

INSTANTIATE_TEST_SUITE_P(EnabledDisabled, ThriftSinkOptionTest, testing::Bool());

TEST_F(ThriftSinkTest, DisablingResetsInlineBudget) {
  grant(1);
  auto const first = genCredits();
  EXPECT_EQ(c_Awaitable::Kind::Static, wh(first)->getKind());
  Cfg::Eval::EnableThriftSinkBufferedReadFastPath = false;
  grant(2);
  auto const disabled = genCredits();
  EXPECT_EQ(c_Awaitable::Kind::ExternalThreadEvent, wh(disabled)->getKind());
  EXPECT_TRUE(same(HHVM_FN(join)(disabled),
    Variant{make_vec_array(2, init_null_variant, init_null_variant)}));
  Cfg::Eval::EnableThriftSinkBufferedReadFastPath = true;
  grant(3);
  auto const reenabled = genCredits();
  EXPECT_EQ(c_Awaitable::Kind::Static, wh(reenabled)->getKind());
  EXPECT_TRUE(same(HHVM_FN(join)(reenabled),
    Variant{make_vec_array(3, init_null_variant, init_null_variant)}));
}

TEST_F(ThriftSinkTest, ArrivalRacesEmptyPollAndWait) {
  for (int i = 0; i < 100; ++i) {
    evb.getEventBase()->runInEventBaseThread([&] {
      std::ignore = client->onSinkRequestN(1);
    });
    EXPECT_TRUE(same(HHVM_FN(join)(genCredits()),
                     Variant{make_vec_array(1, init_null_variant, init_null_variant)}));
    flush();
  }
}

constexpr auto kHelpers = R"HACK(<?hh
function sink_test_state(): stdClass {
  $state = new stdClass();
  $state->events = vec[];
  return $state;
}
function sink_test_io_hooks(): vec<mixed> {
  return vec[() ==> {}, ($_handle) ==> {}];
}
function sink_test_hooks(stdClass $state): vec<mixed> {
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
async function sink_test_output(Awaitable<mixed> $gate, stdClass $state): Awaitable<void> {
  await $gate;
  $state->events[] = 'output';
}
async function sink_test_input(TClientSink $sink, stdClass $state): Awaitable<void> {
  invariant(await $sink->genCreditsOrFinalResponse() === tuple(1, null, null),
            'Expected one actual native credit');
  invariant($sink->sendPayloadOrSinkComplete('input'), 'Native sink closed early');
  $state->events[] = 'input';
}
function sink_test_start(TClientSink $sink, stdClass $state, bool $deferred): vec<mixed> {
  $work = vec[];
  $conditions = vec[];
  for ($i = 0; $i < 4; ++$i) {
    $child = HH\RescheduleWaitHandle::create(HH\RescheduleWaitHandle::QUEUE_DEFAULT, 0);
    $gate = $deferred ? $child : HH\ConditionWaitHandle::create($child);
    $work[] = sink_test_output($gate, $state);
    $work[] = $child;
    if (!$deferred) $conditions[] = $gate;
  }
  foreach ($conditions as $condition) $condition->succeed(null);
  $input = sink_test_input($sink, $state);
  $work[] = $input;
  return vec[$input, HH\AwaitAllWaitHandle::fromVec($work)];
}
function sink_test_join(vec<Awaitable<mixed>> $work): void {
  HH\Asio\join(HH\AwaitAllWaitHandle::fromVec($work));
}
)HACK";

struct ThriftSinkSchedulerTest : ThriftSinkTest {
  void SetUp() override {}
  void TearDown() override {}

  template<class F>
  void run(F&& body) {
    std::thread([&] {
      HphpSessionAndThread request{Treadmill::SessionKind::UnitTests};
      ASSERT_TRUE(rds::local::tcCheck());
      ThriftSinkTest::SetUp();
      SCOPE_EXIT { shutdownRequest(); };
      setupFunctions();
      if (!HasFatalFailure()) body();
    }).join();
  }

  void setupFunctions() {
    unit = compile_string(kHelpers, CodeSource::User,
      "/tmp/hhvm-thrift-sink-scheduler.php", nullptr, nullptr,
      RepoOptions::defaults());
    ASSERT_TRUE(unit);
    unit->merge();
  }
  void shutdownRequest() {
    auto const session = AsioSession::Get();
    session->setOnExternalThreadEventCreate(init_null_variant);
    session->setOnExternalThreadEventSuccess(init_null_variant);
    session->setOnExternalThreadEventFail(init_null_variant);
    session->setOnIOWaitEnter(init_null_variant);
    session->setOnIOWaitExit(init_null_variant);
    ThriftSinkTest::TearDown();
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
  Object state() { return invoke("sink_test_state").toObject(); }
  Array events(const Object& state) {
    return state.toArray()[s_events].toArray();
  }
  Array start(const Object& state, bool deferred = false) {
    return invoke("sink_test_start", make_vec_array(object, state, deferred)).toArray();
  }
  void join(const Array& work) { invoke("sink_test_join", make_vec_array(work)); }
  Unit* unit = nullptr;
};

struct ThriftSinkHookTest : ThriftSinkSchedulerTest,
                          testing::WithParamInterface<std::tuple<bool, int>> {};
TEST_P(ThriftSinkHookTest, RetainsActualExternalEvent) {
  run([&] {
    auto const [enabled, hook] = GetParam();
    Cfg::Eval::EnableThriftSinkBufferedReadFastPath = enabled;
    auto const recorded = state();
    auto const hooks = invoke("sink_test_hooks", make_vec_array(recorded)).toArray();
    auto const session = AsioSession::Get();
    switch (hook) {
      case 0: session->setOnExternalThreadEventCreate(hooks[0]); break;
      case 1: session->setOnExternalThreadEventSuccess(hooks[1]); break;
      case 2: session->setOnExternalThreadEventFail(hooks[2]); break;
    }
    grant(1);
    auto const handle = genCredits();
    EXPECT_EQ(c_Awaitable::Kind::ExternalThreadEvent, wh(handle)->getKind());
    EXPECT_FALSE(wh(handle)->isFinished());
    EXPECT_TRUE(same(HHVM_FN(join)(handle),
                     Variant{make_vec_array(1, init_null_variant, init_null_variant)}));
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
        EXPECT_TRUE(same(success[2], Variant{make_vec_array(1, init_null_variant, init_null_variant)}));
        EXPECT_GT(success[3].toInt64(), 0);
        break;
      }
      case 2: EXPECT_TRUE(observed.empty()); break;
    }
  });
}
INSTANTIATE_TEST_SUITE_P(EnabledDisabledCreateSuccessFail, ThriftSinkHookTest,
                        testing::Combine(testing::Bool(), testing::Values(0, 1, 2)));

TEST_F(ThriftSinkSchedulerTest, DefaultIOCallbacksAllowBufferedCompletion) {
  run([&] {
    auto const hooks = invoke("sink_test_io_hooks").toArray();
    auto const session = AsioSession::Get();
    session->setOnIOWaitEnter(hooks[0]);
    session->setOnIOWaitExit(hooks[1]);
    grant(1);
    auto const handle = genCredits();
    EXPECT_TRUE(wh(handle)->isSucceeded());
    EXPECT_TRUE(same(HHVM_FN(join)(handle),
      Variant{make_vec_array(1, init_null_variant, init_null_variant)}));
  });
}

struct ThriftSinkSchedulerOptionTest : ThriftSinkSchedulerTest,
                                      testing::WithParamInterface<bool> {};

TEST_P(ThriftSinkSchedulerOptionTest, CancelPendingReadAndFailHook) {
  run([&] {
    Cfg::Eval::EnableThriftSinkBufferedReadFastPath = GetParam();
    auto const recorded = state();
    auto const hooks = invoke("sink_test_hooks", make_vec_array(recorded)).toArray();
    AsioSession::Get()->setOnExternalThreadEventFail(hooks[2]);
    auto const handle = genCredits();
    auto const exception = SystemLib::AllocInvalidOperationExceptionObject("cancelled");
    ASSERT_TRUE(wait_handle<c_ExternalThreadEventWaitHandle>(handle.get())->cancel(exception));
    grant(1);
    EXPECT_TRUE(same(Variant{events(recorded)},
                     Variant{make_vec_array(make_vec_array("fail", true, 0))}));
    try {
      HHVM_FN(join)(handle);
      FAIL() << "Canceled read succeeded";
    } catch (const Object& thrown) { EXPECT_EQ(exception.get(), thrown.get()); }
    object = Object{};
    flush();
    EXPECT_EQ(1, server.cancellations);
  });
}

TEST_P(ThriftSinkSchedulerOptionTest, BufferedCreditConditionOrderFollowsOption) {
  run([&] {
    auto const enabled = GetParam();
    Cfg::Eval::EnableThriftSinkBufferedReadFastPath = enabled;
    grant(1);
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

TEST_F(ThriftSinkSchedulerTest, PendingGrantRetainsConditionOrder) {
  run([&] {
    auto const recorded = state();
    auto const work = start(recorded);
    EXPECT_FALSE(wh(work[0].toObject())->isFinished());
    EXPECT_TRUE(events(recorded).empty());
    grant(1);
    join(make_vec_array(work[1]));
    EXPECT_TRUE(same(Variant{events(recorded)},
      Variant{make_vec_array("output", "output", "output", "output", "input")}));
  });
}

TEST_F(ThriftSinkSchedulerTest, RescheduleDeferredControl) {
  run([&] {
    grant(1);
    auto const recorded = state();
    auto const work = start(recorded, true);
    join(make_vec_array(work[1]));
    EXPECT_EQ("input", events(recorded)[0].toString().toCppString());
  });
}

TEST_P(ThriftSinkSchedulerOptionTest, ReadyRefillsYieldToOutstandingOutput) {
  run([&] {
    auto const enabled = GetParam();
    Cfg::Eval::EnableThriftSinkBufferedReadFastPath = enabled;
    for (int pair = 0; pair < 3; ++pair) {
      auto const recorded = state();
      grant(1);
      auto const first = start(recorded);
      EXPECT_EQ(enabled, wh(first[0].toObject())->isSucceeded());
      // Keep the first ready output outstanding across the next ready refill.
      grant(1);
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
      flush();
      EXPECT_EQ(2 * (pair + 1), server.inputs);
    }
  });
}

INSTANTIATE_TEST_SUITE_P(EnabledDisabled, ThriftSinkSchedulerOptionTest,
                        testing::Bool());
}
}
