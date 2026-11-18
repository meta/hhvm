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

#include "hphp/runtime/base/repo-autoload-map.h"
#include "hphp/runtime/base/runtime-option.h"
#include "hphp/runtime/base/static-string-table.h"
#include "hphp/runtime/base/unit-cache.h"
#include "hphp/runtime/vm/treadmill.h"
#include "hphp/runtime/vm/unit-emitter.h"
#include "hphp/runtime/vm/unit-parser.h"

#include <folly/ScopeGuard.h>
#include <folly/synchronization/Baton.h>
#include <folly/testing/TestUtil.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace HPHP {
namespace {

struct CompileState {
  folly::Baton<> entered;
  folly::Baton<> resume;
  std::atomic<bool> reclaimed{false};
  std::atomic<unsigned> compiled{0};
  bool pause{false};
} s_compile;

[[noreturn]] void exitTest() {
  auto const result = testing::UnitTest::GetInstance()
    ->current_test_info()->result();
  for (int i = 0; i < result->total_part_count(); ++i) {
    auto const& part = result->GetTestPartResult(i);
    if (!part.failed()) continue;
    std::fprintf(stderr, "%s:%d\n%s\n",
                 part.file_name(), part.line_number(), part.message());
  }
  std::fflush(stderr);
  std::_Exit(testing::Test::HasFailure() ? 1 : 0);
}

std::unique_ptr<UnitEmitter> compileHook(
    const char*, const SHA1&, folly::StringPiece::size_type,
    HhvmDeclProvider*,
    const std::function<std::unique_ptr<UnitEmitter>(bool)>& compile) {
  if (s_compile.pause) {
    s_compile.entered.post();
    EXPECT_TRUE(s_compile.resume.try_wait_for(std::chrono::seconds{30}));
    if (testing::Test::HasFailure()) exitTest();
  }

  auto ue = compile(true);
  EXPECT_NE(ue, nullptr);
  if (ue) {
    EXPECT_FALSE(ue->m_ICE);
    auto const& paths =
      ue->getPackageInfo().packageAndImplicitFamilyPathsInLookupOrder();
    EXPECT_EQ(paths.size(), 1);
    if (paths.size() == 1) {
      EXPECT_EQ(paths.front().m_path, "src/");
      EXPECT_EQ(paths.front().m_package, "before");
    }
    ++s_compile.compiled;
  }
  return ue;
}

void checkRepoOptionsLifetime(bool eagerReload) {
  folly::test::TemporaryDirectory temp{"prefetch-repo-options"};
  auto root = std::filesystem::path{temp.path().native()};
  std::filesystem::create_directory(root / "src");
  auto const path = makeStaticString((root / "src/unit.php").native());
  std::ofstream{root / ".hhvmconfig.hdf"} << "Autoload {\n}\n";
  std::ofstream{root / "PACKAGES.toml"} << R"(
[packages.before]
include_paths = ["//src/"]
)";
  std::ofstream{path->data()} << "<?hh\nfunction value(): int { return 1; }\n";

  Cfg::Eval::UnitPrefetcherMaxThreads = 1;
  Cfg::Eval::UnitPrefetcherMinThreads = 1;
  Cfg::Eval::AutoloadEagerReloadUnitCache = true;
  Cfg::Eval::UseEdenFS = false;
  Cfg::Eval::ReuseUnitsByHash = false;
  g_unit_emitter_cache_hook = compileHook;

  auto const before = &RepoOptions::forFile(path->data());
  ASSERT_TRUE(before->packageInfo().packages().contains("before"));
  if (eagerReload) {
    prefetchUnit(path, nullptr, nullptr);
    drainUnitPrefetcher();
    ASSERT_EQ(s_compile.compiled.load(), 1);
    std::ofstream{path->data()} << "<?hh\nfunction value(): int { return 2; }\n";
  }

  s_compile.pause = true;
  RepoAutoloadMap map{{}, {}, {}, {}, {}};
  if (eagerReload) {
    std::vector<Facts::PathAndOptionalHash> changed{{"src/unit.php", {}}};
    std::vector<std::filesystem::path> deleted;
    unitCacheSyncRepo(&map, root, changed, deleted);
  } else {
    prefetchUnit(path, nullptr, nullptr);
  }
  EXPECT_TRUE(s_compile.entered.try_wait_for(std::chrono::seconds{30}));
  if (testing::Test::HasFailure()) exitTest();

  std::ofstream{root / "PACKAGES.next"} << R"(
[packages.after]
include_paths = ["//src/"]
)";
  std::filesystem::rename(root / "PACKAGES.next", root / "PACKAGES.toml");
  auto const after = &RepoOptions::forFile(path->data());
  EXPECT_NE(before, after);
  EXPECT_TRUE(after->packageInfo().packages().contains("after"));

  // Retirement queued after the old RepoOptions must also wait for the worker.
  Treadmill::enqueue([] { s_compile.reclaimed = true; });
  Treadmill::finishRequest();
  Treadmill::startRequest(Treadmill::SessionKind::UnitTests);
  EXPECT_FALSE(s_compile.reclaimed.load())
    << "RepoOptions reclaimed while background compilation still borrows them";
  // Do not resume compilation with a dangling reference on failure.
  if (testing::Test::HasFailure()) exitTest();

  s_compile.resume.post();
  drainUnitPrefetcher();
  EXPECT_EQ(s_compile.compiled.load(), eagerReload ? 2 : 1);
  EXPECT_TRUE(s_compile.reclaimed.load());
  shutdownUnitPrefetcher();
}

struct UnitCacheOptionsTest : testing::TestWithParam<bool> {};

TEST_P(UnitCacheOptionsTest, RetainsRepoOptionsUntilCompilationFinishes) {
  auto const style = GTEST_FLAG_GET(death_test_style);
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  SCOPE_EXIT { GTEST_FLAG_SET(death_test_style, style); };
  ASSERT_EXIT({
    checkRepoOptionsLifetime(GetParam());
    exitTest();
  }, testing::ExitedWithCode(0), "");
}

INSTANTIATE_TEST_SUITE_P(
  PrefetchAndEagerReload, UnitCacheOptionsTest, testing::Bool(),
  [](const testing::TestParamInfo<bool>& info) {
    return info.param ? "EagerReload" : "Prefetch";
  }
);

}
}
