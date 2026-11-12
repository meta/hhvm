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
#include <fmt/core.h>

#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/base/preg.h"
#include "hphp/runtime/base/static-string-table.h"
#include "hphp/runtime/base/timestamp.h"
#include "hphp/runtime/base/type-string.h"
#include "hphp/runtime/base/unit-cache.h"

#include "hphp/runtime/server/http-server.h"
#include "hphp/runtime/server/server-stats.h"

#include "hphp/runtime/vm/jit/cg-meta.h"
#include "hphp/runtime/vm/jit/prof-data.h"
#include "hphp/runtime/vm/jit/tc-internal.h"
#include "hphp/runtime/vm/jit/tc-record.h"

#include "hphp/util/alloc-defs.h"
#include "hphp/util/managed-arena.h"
#include "hphp/util/service-data.h"

namespace HPHP {

namespace {
int64_t getVal(const char* key) {
    auto ret = ServiceData::exportCounterByKey(key);
    EXPECT_NE(ret, std::nullopt);
    return *ret;
}
}

TEST(COUNTERS, static_string) {
    const char* key = "admin.static-strings";
    int ss = getVal(key);
    EXPECT_EQ(ss, makeStaticStringCount());

    makeStaticString("bananas");
    ++ss;
    EXPECT_EQ(ss, getVal(key));
    EXPECT_EQ(ss, makeStaticStringCount());

    refineStaticStringTableSize();
    EXPECT_EQ(ss, getVal(key));
    EXPECT_EQ(ss, makeStaticStringCount());
}

TEST(COUNTERS, pcre_cache) {
    const char* key = "admin.pcre-cache";
    int ss = getVal(key);
    EXPECT_EQ(ss, preg_pcre_cache_size());

    preg_grep(OptString{"/.ba./"}, make_vec_array(OptString{"ababu"}, OptString{"bananas"}));
    ++ss;
    EXPECT_EQ(ss, getVal(key));
    EXPECT_EQ(ss, preg_pcre_cache_size());

    // confirm we're correctly skipping when hitting the cache
    preg_grep(OptString{"/.ba./"}, make_vec_array(OptString{"ababu"}, OptString{"bananas"}));
    EXPECT_EQ(ss, getVal(key));
    EXPECT_EQ(ss, preg_pcre_cache_size());

    // kill table
    pcre_reinit();
    EXPECT_EQ(0, getVal(key));
    EXPECT_EQ(0, preg_pcre_cache_size());

}

TEST(COUNTERS, named_entities) {
  const char* key = "admin.named-entities";
    int ss = getVal(key);
    EXPECT_EQ(ss, namedEntityTableSize());

    const auto str = OptString{"MyNewHilariousType"};
    NamedType::getOrCreate(str.get());
    ++ss;
    EXPECT_EQ(ss, getVal(key));
    EXPECT_EQ(ss, namedEntityTableSize());

    const auto str2 = OptString{"MyNewHilariousFunc"};
    NamedFunc::getOrCreate(str2.get());
    ++ss;
    EXPECT_EQ(ss, getVal(key));
    EXPECT_EQ(ss, namedEntityTableSize());

}

TEST(COUNTERS, expensive_test) {
  HttpServer::Server = std::make_shared<HttpServer>();

  ServiceData::createCounter("counter_key")->increment();

  int expensive_calls = 0;
  int cheap_calls = 0;
  auto expensive_counter_callback = ServiceData::ExpensiveCounterCallback(
    [&](ServiceData::CounterMap& counters) {
      counters.emplace("expensive_key", 42);
      ++expensive_calls;
    }
  );
  auto _ = ServiceData::CounterCallback(
    [&](ServiceData::CounterMap& counters) {
      ++cheap_calls;
      counters.emplace("cheap_key", 1337);
    }
  );

  EXPECT_EQ(getVal("counter_key"), 1);
  // don't compute keys if we already have what we need
  EXPECT_EQ(expensive_calls, 0);
  EXPECT_EQ(cheap_calls, 0);

  EXPECT_EQ(getVal("cheap_key"), 1337);
  // still don't compute expensive key
  EXPECT_EQ(expensive_calls, 0);
  EXPECT_EQ(cheap_calls, 1);


  EXPECT_EQ(getVal("expensive_key"), 42);
  // compute both
  EXPECT_EQ(expensive_calls, 1);
  EXPECT_EQ(cheap_calls, 2);


  EXPECT_EQ(ServiceData::exportCounterByKey("non-key"), std::nullopt);
  // compute both and then fail
  EXPECT_EQ(expensive_calls, 2);
  EXPECT_EQ(cheap_calls, 3);

  ServiceData::CounterMap map;
  ServiceData::exportAll(map);
  EXPECT_EQ(map.at("expensive_key"), 42);
  EXPECT_EQ(map.at("cheap_key"), 1337);
  // compute everything
  EXPECT_EQ(expensive_calls, 3);
  EXPECT_EQ(cheap_calls, 4);

  expensive_counter_callback.~CounterCallbackBase();
  EXPECT_EQ(ServiceData::exportCounterByKey("expensive_key"), std::nullopt);
  EXPECT_EQ(expensive_calls, 3);
  EXPECT_EQ(cheap_calls, 5);

  map.clear();
  ServiceData::exportAll(map);
  EXPECT_EQ(map.find("expensive_key"), end(map));
  EXPECT_EQ(map.at("cheap_key"), 1337);
  EXPECT_EQ(expensive_calls, 3);
  EXPECT_EQ(cheap_calls, 6);
}

TEST(COUNTERS, selected_counters_test) {
  HttpServer::Server = std::make_shared<HttpServer>();

  auto c = ServiceData::createCounter("counter_key");
  c->increment();

  int expensive_calls = 0;
  int cheap_calls = 0;
  auto _1 = ServiceData::ExpensiveCounterCallback(
    [&](ServiceData::CounterMap& counters) {
      static int key_value = 0;
      counters.emplace("expensive_key", 10 + key_value);
      counters.emplace("expensive_key_2", 15 + key_value);
      ++key_value;
      ++expensive_calls;
    }
  );
  auto _2 = ServiceData::CounterCallback(
    [&](ServiceData::CounterMap& counters) {
      static int key_value = 0;
      counters.emplace("cheap_key", 100 + key_value++);
      ++cheap_calls;
    }
  );

  int cheap_two_calls = 0;
  auto _3 = ServiceData::CounterCallback(
    [&](ServiceData::CounterMap& counters) {
      static int key_value = 0;
      counters.emplace("cheap_key_2", 200 + key_value++);
      ++cheap_two_calls;
    }
  );

  ServiceData::CounterMap map;
  // should be no-ops
  ServiceData::exportSelectedCountersByKeys(map, std::vector<std::string>{});
  EXPECT_EQ(expensive_calls, 0);
  EXPECT_EQ(cheap_calls, 0);
  EXPECT_TRUE(map.empty());
  ServiceData::exportSelectedCountersByKeys(map, std::vector<std::string>{""});
  EXPECT_EQ(expensive_calls, 0);
  EXPECT_EQ(cheap_calls, 0);
  EXPECT_TRUE(map.empty());

  ServiceData::exportSelectedCountersByKeys(map, std::vector<std::string>{"counter_key"});
  EXPECT_EQ(map.size(), 1);
  EXPECT_EQ(map.at("counter_key"), 1);
  // no new computation of counters
  EXPECT_EQ(expensive_calls, 0);
  EXPECT_EQ(cheap_calls, 0);

  ServiceData::exportSelectedCountersByKeys(map, std::vector<std::string>{"counter_key", "cheap_key"});
  EXPECT_EQ(map.size(), 2);
  EXPECT_EQ(map.at("cheap_key"), 100);
  EXPECT_EQ(map.at("counter_key"), 1);
  // both computed exactly once
  EXPECT_EQ(cheap_calls, 1);
  EXPECT_EQ(cheap_two_calls, 1);
  // expensive still not computed
  EXPECT_EQ(expensive_calls, 0);

  map.clear();
  ServiceData::exportSelectedCountersByKeys(map, std::vector<std::string>{"cheap_key", "cheap_key_2"});
  EXPECT_EQ(map.size(), 2);
  EXPECT_EQ(map.at("cheap_key"), 101);
  EXPECT_EQ(map.at("cheap_key_2"), 201);
  // only computed once more
  EXPECT_EQ(cheap_calls, 2);
  EXPECT_EQ(cheap_two_calls, 2);
  // expensive still not computed
  EXPECT_EQ(expensive_calls, 0);

  map.clear();
  ServiceData::exportSelectedCountersByKeys(map, std::vector<std::string>{"cheap_key_2", "expensive_key"});
  EXPECT_EQ(map.size(), 2);
  EXPECT_EQ(map.at("expensive_key"), 10);
  EXPECT_EQ(map.at("cheap_key_2"), 202);
  // both computed once more
  EXPECT_EQ(cheap_calls, 3);
  EXPECT_EQ(cheap_two_calls, 3);
  // expensive only now computed
  EXPECT_EQ(expensive_calls, 1);

  c->increment();

  map.clear();
  // note ordering where expensive computed in the middle
  ServiceData::exportSelectedCountersByKeys(map, std::vector<std::string>{"cheap_key_2", "expensive_key", "cheap_key", "counter_key"});
  EXPECT_EQ(map.size(), 4);
  EXPECT_EQ(map.at("expensive_key"), 11);
  EXPECT_EQ(map.at("cheap_key"), 103);
  EXPECT_EQ(map.at("cheap_key_2"), 203);
  EXPECT_EQ(map.at("counter_key"), 2);
  // both computed once more
  EXPECT_EQ(cheap_calls, 4);
  EXPECT_EQ(cheap_two_calls, 4);
  // expensive only computed once
  EXPECT_EQ(expensive_calls, 2);


  map.clear();
  ServiceData::exportSelectedCountersByKeys(map, std::vector<std::string>{"expensive_key", "expensive_key_2"});
  EXPECT_EQ(map.size(), 2);
  EXPECT_EQ(map.at("expensive_key"), 12);
  EXPECT_EQ(map.at("expensive_key_2"), 17);
  // both computed once more
  EXPECT_EQ(cheap_calls, 5);
  EXPECT_EQ(cheap_two_calls, 5);
  // expensive only computed once since both keys from same CounterCallback
  EXPECT_EQ(expensive_calls, 3);

  map.clear();
  ServiceData::exportSelectedCountersByKeys(map, std::vector<std::string>{"not_a_key"});
  EXPECT_TRUE(map.empty());
  // everything gets computed
  EXPECT_EQ(cheap_calls, 6);
  EXPECT_EQ(cheap_two_calls, 6);
  EXPECT_EQ(expensive_calls, 4);

  map.clear();
  ServiceData::exportSelectedCountersByKeys(map, std::vector<std::string>{
    "not_a_key", "cheap_key_2", "expensive_key", "cheap_key", "expensive_key_2", "counter_key"
  });
  EXPECT_EQ(map.size(), 5);
  EXPECT_EQ(map.at("expensive_key"), 14);
  EXPECT_EQ(map.at("expensive_key_2"), 19);
  EXPECT_EQ(map.at("cheap_key"), 106);
  EXPECT_EQ(map.at("cheap_key_2"), 206);
  EXPECT_EQ(map.at("counter_key"), 2);
  // everything gets computed exactly once by the first not a key lookup
  EXPECT_EQ(cheap_calls, 7);
  EXPECT_EQ(cheap_two_calls, 7);
  EXPECT_EQ(expensive_calls, 5);
}

TEST(COUNTERS, tc_space) {
  EXPECT_EQ(getVal("admin.vm-tcspace.RDS"), rds::usedBytes());
  EXPECT_EQ(getVal("admin.vm-tcspace.RDSLocal"), rds::usedLocalBytes());
  EXPECT_EQ(getVal("admin.vm-tcspace.PersistentRDS"), rds::usedPersistentBytes());
}

TEST(COUNTERS, build_age) {
    RuntimeOption::BuildId = "4-1234-9876-20230731081921";
    const auto begin = std::time(nullptr);
    EXPECT_EQ(getVal("admin.build.rev"), 9876);

    bool error = false;
    const auto ts = TimeStamp::Get(
      error,
      /* hour */   8,
      /* minute */ 19,
      /* second */ 21,
      /* month */  7,
      /* day */    31,
      /* year */   2023
    );
    EXPECT_FALSE(error);

    const int age = getVal("admin.build.age");
    const auto end = std::time(nullptr);

    // pinpointing the exact timestamp would be finicky, so just confirm
    // it's within the reasonable range
    EXPECT_GE(begin - ts, age);
    EXPECT_LE(age, end - ts);
}

#if USE_JEMALLOC
TEST(COUNTERS, alloc_arena_usage) {
  #define C(which) \
  { \
    auto val = getVal("admin." #which "_arena_usage"); \
    auto a = alloc::which ## Arena(); \
    auto compare = a ? s_pageSize * mallctl_pactive(a->id()) : 0; \
    EXPECT_EQ(val, compare); \
  }

  C(low);
  C(high);
  #undef C
}

#endif

TEST(COUNTERS, swappable_roarena_capac) {
  setup_swappable_readonly_arena(32);
  auto const arena = get_swappable_readonly_arena();
  EXPECT_NE(arena, nullptr);
  EXPECT_EQ(getVal("admin.swappable-roarena-cap"), 0);
  arena->allocate(4);
  EXPECT_NE(arena->capacity(), 0);
  EXPECT_EQ(getVal("admin.swappable-roarena-cap"), arena->capacity());
}

TEST(COUNTERS, num_catch_traces) {
  EXPECT_EQ(getVal("admin.catch-traces"), jit::numCatchTraces());
}

TEST(COUNTERS, named_entity_stats) {
  const auto stats = namedEntityStats();
  EXPECT_EQ(getVal("admin.named_entities_submaps"), stats[0].second);
  EXPECT_EQ(getVal("admin.named_entities_capacity"), stats[1].second);
}

TEST(COUNTERS, fixmup_map_size) {
  EXPECT_EQ(getVal("admin.fixup_map_size"), jit::FixupMap::size());
}

TEST(COUNTERS, allocs_frees) {
  // Eval.EnableReusableTC is false, so the best we can do is ensure that
  // the counters are being created successfully and that the code runs
  // without crashing.

  const auto confirm_exists_and_zero = []() {
  jit::tc::code().forEachName([](const char* name) {
    EXPECT_EQ(getVal(fmt::format("jit.code.{}.allocs", name).c_str()), 0);
    EXPECT_EQ(getVal(fmt::format("jit.code.{}.frees", name).c_str()), 0);
    EXPECT_EQ(getVal(fmt::format("jit.code.{}.bytes_free", name).c_str()), 0);
    EXPECT_EQ(getVal(fmt::format("jit.code.{}.free_blocks", name).c_str()), 0);
  });
  };
  confirm_exists_and_zero();

  const auto lock = jit::tc::lockCode(true);
  jit::tc::updateCodeSizeCounters();

  confirm_exists_and_zero();
}

TEST(COUNTERS, profiling_and_optimized_funcs) {
  auto profData = const_cast<jit::ProfData*>(jit::globalProfData());
  // this happens for some nopgo tests
  if (!profData) return;
  auto obj = SystemLib::AllocInvalidArgumentExceptionObject("This is a test exception object for counters");
  auto func = obj->getVMClass()->getCtor();
  profData->setProfiling(func);
  EXPECT_EQ(getVal("jit.profiling_funcs"), profData->profilingFuncs());
  profData->setOptimized(func->getFuncId());
  EXPECT_EQ(getVal("jit.optimized_funcs"), profData->optimizedFuncs());
}

TEST(COUNTERS, server_stats) {
  Cfg::Stats::Web = true;
  Cfg::Stats::Enable = true;
  HttpServer::Server = std::make_shared<HttpServer>();
  ServerStats::Log("MyKey", 45);
  ServerStats::LogPage("yup_this_page", 42);
  auto stats = ServerStats::Report({"MyKey", "hit"}, "prefix");
  EXPECT_EQ(stats.at("prefix.MyKey"), 45);
  EXPECT_EQ(stats.at("prefix.hit"), 1);
}
}
