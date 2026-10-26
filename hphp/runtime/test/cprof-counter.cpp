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

#include "hphp/runtime/vm/jit/cprof-counter.h"

#include <barrier>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

#include <folly/ScopeGuard.h>
#include <gtest/gtest.h>

#include "hphp/runtime/server/http-server.h"
#include "hphp/runtime/vm/jit/cprof-checkpoint.h"
#include "hphp/runtime/vm/jit/cprof-consume.h"
#include "hphp/util/configs/jit.h"
#include "hphp/util/service-data.h"

namespace HPHP::jit::cprof {
namespace {

constexpr ContProfCounter s_lazyCounter{"jit.cprof.test.lazy"};
constexpr ContProfCounter s_concurrentCounter{"jit.cprof.test.concurrent"};

TEST(ContProfCounterDeathTest, DisabledProcessDoesNotExportCounters) {
  auto const oldStyle = GTEST_FLAG_GET(death_test_style);
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  SCOPE_EXIT { GTEST_FLAG_SET(death_test_style, oldStyle); };

  EXPECT_EXIT({
    if (Cfg::Jit::ContProfCaptureEnabled ||
        Cfg::Jit::ContProfStartupMaxFunctions) std::_Exit(1);
    consumeContProfAtStartup();
    startContProfCheckpointWriter();
    stopContProfCheckpointWriter();
    if (contProfActive() || contProfStartupActive()) std::_Exit(1);

    HttpServer::Server = std::make_shared<HttpServer>();
    ServiceData::CounterMap counters;
    ServiceData::exportAll(counters);
    for (auto const& counter : counters) {
      auto const& name = counter.first;
      if (!name.starts_with("jit.cprof.")) continue;
      std::fprintf(stderr, "Unexpected cprof counter: %s\n", name.c_str());
      std::_Exit(1);
    }
    std::_Exit(0);
  }, testing::ExitedWithCode(0), "");
}

TEST(ContProfCounter, RegistersOnFirstUseAndCachesHandle) {
  EXPECT_FALSE(s_lazyCounter.initialized());
  EXPECT_FALSE(ServiceData::exportCounterByKey("jit.cprof.test.lazy"));

  s_lazyCounter->increment();
  EXPECT_TRUE(s_lazyCounter.initialized());
  EXPECT_EQ(1, ServiceData::exportCounterByKey("jit.cprof.test.lazy").value_or(-1));
  auto const handle = s_lazyCounter.operator->();
  EXPECT_EQ(handle, s_lazyCounter.operator->());
  EXPECT_EQ(handle, ServiceData::getCounterIfExists("jit.cprof.test.lazy"));

  s_lazyCounter->addValue(4);
  EXPECT_EQ(5, ServiceData::exportCounterByKey("jit.cprof.test.lazy").value_or(-1));
  s_lazyCounter->setValue(0);
  EXPECT_EQ(0, ServiceData::exportCounterByKey("jit.cprof.test.lazy").value_or(-1));
}

TEST(ContProfCounter, ConcurrentFirstUsePreservesAllUpdates) {
  constexpr size_t threads = 8;
  constexpr size_t increments = 256;
  std::barrier start{threads};
  std::vector<std::thread> workers;
  for (size_t i = 0; i < threads; ++i) {
    workers.emplace_back([&] {
      start.arrive_and_wait();
      for (size_t j = 0; j < increments; ++j) {
        s_concurrentCounter->increment();
      }
    });
  }
  for (auto& worker : workers) worker.join();
  EXPECT_EQ(threads * increments, ServiceData::exportCounterByKey(
    "jit.cprof.test.concurrent"
  ).value_or(-1));
}

}
}
