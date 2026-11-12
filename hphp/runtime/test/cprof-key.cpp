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

#include "hphp/runtime/vm/jit/cprof-key.h"

#include <memory>
#include <string>
#include <utility>

#include <folly/ScopeGuard.h>
#include <gtest/gtest.h>

#include "hphp/runtime/base/runtime-option.h"
#include "hphp/runtime/base/string-data.h"
#include "hphp/runtime/vm/as.h"
#include "hphp/runtime/vm/func.h"
#include "hphp/runtime/vm/named-entity.h"
#include "hphp/runtime/vm/unit-emitter.h"
#include "hphp/runtime/vm/unit.h"
#include "hphp/util/configs/server.h"
#include "hphp/util/sha1.h"

namespace HPHP::jit::cprof {
namespace {

constexpr auto kUnitPath = "hphp/runtime/test/cont-prof-key-test.php";
constexpr auto kSourceRoot = "/tmp/cont-prof-source-root/";
constexpr auto kAbsoluteUnitPath =
  "/tmp/cont-prof-source-root/hphp/runtime/test/cont-prof-key-test.php";

constexpr auto kHhas = R"HHAS(
.function N cont_prof_key_round_trip_test_5f4a7e8b() {
  Null
  RetC None
}
)HHAS";

struct DestroyTestUnit {
  void operator()(Unit* unit) const {
    if (!unit) return;

    for (auto const func : unit->funcs()) {
      auto const named = func->getNamedFunc();
      if (named->getCachedFunc() == func) {
        named->setCachedFunc(nullptr);
      }
    }
    unit->startDestroy();
  }
};

using TestUnit = std::unique_ptr<Unit, DestroyTestUnit>;

TestUnit makeTestUnit() {
  auto const emitter = assemble_string(
    kHhas,
    kAbsoluteUnitPath,
    SHA1{"1111111111111111111111111111111111111111"},
    nullptr,
    RepoOptions::defaults().packageInfo(),
    UnitEmitterAttributes::defaults(),
    false
  );
  if (!emitter || emitter->m_fatalUnit) return nullptr;
  return TestUnit{emitter->create().release()};
}

ContProfFuncKey exampleKey() {
  ContProfFuncKey key{};
  key.resolutionUnitPath = "src/example.php";
  key.bytecodeUnitHash = SHA1{uint64_t{1}};
  key.functionName = "example";
  return key;
}

TEST(ContProfKey, EqualityAndOrdering) {
  auto const key = exampleKey();
  auto same = key;

  EXPECT_TRUE(key == same);
  EXPECT_FALSE(key < same);
  EXPECT_FALSE(same < key);

  auto differentName = key;
  differentName.functionName = "later";
  EXPECT_FALSE(key == differentName);
  EXPECT_TRUE(key < differentName);

  auto differentHash = key;
  differentHash.bytecodeUnitHash = SHA1{uint64_t{2}};
  EXPECT_FALSE(key == differentHash);
}

TEST(ContProfKey, RejectsInvalidPaths) {
  auto const oldSourceRoot =
    std::exchange(Cfg::Server::SourceRoot, kSourceRoot);
  SCOPE_EXIT { Cfg::Server::SourceRoot = oldSourceRoot; };

  auto unit = makeTestUnit();
  ASSERT_NE(nullptr, unit);
  ASSERT_EQ(1, unit->funcs().size());

  auto const func = unit->funcs()[0];
  unit->merge();

  auto const validKey = makeContProfFuncKey(*func);
  ASSERT_TRUE(validKey);
  EXPECT_TRUE(isValidContProfFuncKey(*validKey));

  auto key = *validKey;

  for (auto const path : {
         "",
         "/absolute.php",
         ".",
         "..",
         "src/../example.php",
         "src//example.php",
         "src\\example.php",
         "src/",
    }) {
    key.resolutionUnitPath = path;
    EXPECT_FALSE(isValidContProfFuncKey(key));
    EXPECT_EQ(nullptr, resolveContProfFunc(key, *unit));
  }

  key = *validKey;
  key.bytecodeUnitPath = "../bytecode.php";
  EXPECT_FALSE(isValidContProfFuncKey(key));
  EXPECT_EQ(nullptr, resolveContProfFunc(key, *unit));
}

TEST(ContProfKey, BuildsAndResolvesFunction) {
  auto const oldSourceRoot =
    std::exchange(Cfg::Server::SourceRoot, kSourceRoot);
  SCOPE_EXIT { Cfg::Server::SourceRoot = oldSourceRoot; };

  auto unit = makeTestUnit();
  ASSERT_NE(nullptr, unit);
  ASSERT_EQ(1, unit->funcs().size());

  auto const func = unit->funcs()[0];
  ASSERT_FALSE(func->isBuiltin());
  ASSERT_EQ(kAbsoluteUnitPath, unit->origFilepath()->toCppString());
  ASSERT_NE(SHA1{}, unit->bcSha1());

  unit->merge();

  auto const key = makeContProfFuncKey(*func);
  ASSERT_TRUE(key);
  EXPECT_EQ(kUnitPath, key->resolutionUnitPath);
  EXPECT_FALSE(key->bytecodeUnitPath);

  EXPECT_EQ(func, resolveContProfFunc(*key, *unit));

  Cfg::Server::SourceRoot = "/tmp/cont-prof-source";
  EXPECT_FALSE(makeContProfFuncKey(*func));
  Cfg::Server::SourceRoot = "relative/source/root";
  EXPECT_FALSE(makeContProfFuncKey(*func));
  Cfg::Server::SourceRoot = kSourceRoot;

  auto mismatched = *key;
  mismatched.bytecodeUnitHash = SHA1{uint64_t{2}};
  EXPECT_EQ(nullptr, resolveContProfFunc(mismatched, *unit));
}

}
}
