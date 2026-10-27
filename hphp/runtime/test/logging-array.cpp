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

#include <array>
#include <unistd.h>

#include <folly/portability/GTest.h>

#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/base/bespoke/logging-array.h"
#include "hphp/runtime/base/bespoke/logging-profile.h"
#include "hphp/runtime/base/static-string-table.h"
#include "hphp/runtime/base/type-structure.h"
#include "hphp/runtime/base/typed-value.h"
#include "hphp/runtime/base/vanilla-dict-defs.h"
#include "hphp/runtime/vm/treadmill.h"
#include "hphp/util/configs/eval.h"
#include "hphp/util/configs/jit.h"
#include "hphp/util/configs/repo.h"

namespace HPHP {

namespace {

/*
 * A small static dict whose keys are all static strings gets a StrKeyTable
 * allocated ahead of it; see the shouldCreateStrKeyTable computation in
 * VanillaDict::CopyStatic.
 */
ArrayData* makeStaticDictWithStrKeyTable() {
  DictInit init{3};
  init.set(makeStaticString("alpha"), make_tv<KindOfInt64>(1));
  init.set(makeStaticString("beta"), make_tv<KindOfInt64>(2));
  init.set(makeStaticString("gamma"), make_tv<KindOfInt64>(3));
  auto ad = init.create();
  ArrayData::GetScalarArray(&ad);
  return ad;
}

}

/*
 * LoggingArray::MakeShared used to size its allocation prefix with
 * sharedAllocExtra(wrapped, hasApcTv), reserving room for the wrapped array's
 * StrKeyTable. It never propagated kHasStrKeyTable onto the LoggingArray
 * itself, but BespokeArray::ReleaseShared recomputes that prefix from the
 * LoggingArray. The two disagreed by sizeof(StrKeyTable) rounded up to 16, so
 * the array was handed to sdallocx() as an interior pointer with a size 16
 * bytes short of the real allocation -- which is what trips jemalloc's
 * arena_ptr_array_flush_size_check_fail.
 *
 * The arrays made here are intentionally leaked: releasing them requires the
 * bespoke layout vtables, which a unit test doesn't set up.
 */
TEST(LoggingArray, SharedAllocGeometryMatchesRelease) {
  auto const ad = makeStaticDictWithStrKeyTable();
  ASSERT_TRUE(ad->isStatic());
  ASSERT_TRUE(ad->isVanillaDict());
  ASSERT_TRUE(ad->hasStrKeyTable());

  for (auto const hasApcTv : {false, true}) {
    // Keep this test from silently becoming a no-op: the prefix the old code
    // computed from the wrapped array must actually differ from the correct
    // one, or nothing below can detect the regression.
    ASSERT_NE(sharedAllocExtra(ad, hasApcTv),
              bespoke::LoggingArray::SharedAllocExtra(hasApcTv));

    // MakeShared asserts that the prefix it allocates equals the one
    // BespokeArray::ReleaseShared recomputes from the result. Sizing the
    // allocation from the wrapped array trips that assert here.
    auto const lad = bespoke::LoggingArray::MakeShared(ad, nullptr, hasApcTv);
    ASSERT_NE(lad, nullptr);
    EXPECT_FALSE(lad->hasStrKeyTable());
    EXPECT_EQ(lad->hasApcTv(), hasApcTv);

    // These are the two operands BespokeArray::ReleaseShared derives from lad
    // and passes to FreeShared.
    EXPECT_EQ(sharedAllocExtra(lad, lad->hasApcTv()),
              bespoke::LoggingArray::SharedAllocExtra(hasApcTv));
    EXPECT_EQ(lad->heapSize(), sizeof(bespoke::LoggingArray));
  }
}

namespace {

enum class LoggingMutation { Set, Remove, Pop, TypeStructure };

void checkLoggingMutation(ArrayData* cached, LoggingMutation op,
                          bool profiling) {
  auto arr = Array(cached);
  auto expected = make_dict_array("kind", int64_t(TypeStructure::Kind::T_int));
  switch (op) {
    case LoggingMutation::Set:
      arr.set(StaticString("nullable"), make_tv<KindOfBoolean>(true));
      expected.set(StaticString("nullable"), make_tv<KindOfBoolean>(true));
      break;
    case LoggingMutation::Remove:
      arr.remove(StaticString("kind"));
      expected.remove(StaticString("kind"));
      break;
    case LoggingMutation::Pop:
      EXPECT_EQ(arr.pop().toInt64(), int64_t(TypeStructure::Kind::T_int));
      expected.pop();
      break;
    case LoggingMutation::TypeStructure: {
      auto input = make_dict_array(
        "kind", int64_t(TypeStructure::Kind::T_shape),
        "fields", make_dict_array("field", make_dict_array(
          "value", arr, "optional_shape_field", true)));
      input.setEvalScalar();
      ASSERT_EQ(input[StaticString("fields")].toArray()
                      [StaticString("field")].toArray()
                      [StaticString("value")].toArray().get(), cached);
      auto persistent = false;
      auto resolved = TypeStructure::resolve(
        input.get(), nullptr, nullptr, nullptr, persistent);
      EXPECT_TRUE(persistent);
      arr = resolved[StaticString("fields")].toArray()
                    [StaticString("field")].toArray();
      expected.set(StaticString("optional_shape_field"),
                   make_tv<KindOfBoolean>(true));
      break;
    }
  }
  EXPECT_TRUE(arr->same(expected.get()));
  ASSERT_FALSE(arr->isVanilla());
  auto const ko = bespoke::LoggingArray::As(arr.get())->keyOrder;
  if (profiling) {
    EXPECT_TRUE(ko.valid());
  } else {
    // Pointer equality does not read the profiling-only backing storage.
    EXPECT_TRUE(ko == bespoke::KeyOrder::MakeInvalid());
  }
  auto original = make_dict_array("kind", int64_t(TypeStructure::Kind::T_int));
  EXPECT_TRUE(cached->same(original.get()));
}

void checkLoggingProfileTransition() {
  Cfg::Eval::BespokeArrayLikeMode = 2;
  Cfg::Repo::Authoritative = true;
  Cfg::Jit::PGO = true;
  Cfg::Jit::RetranslateAllRequest = 1;
  Cfg::Jit::RetranslateAllSeconds = 1;
  Cfg::Eval::ExportLoggingArrayDataPath.clear();
  bespoke::setLoggingEnabled(true);

  auto const profile = bespoke::getLoggingProfile(bespoke::APCKey{
    size_t(makeStaticString("logging-key-order-transition")->hash())});
  ASSERT_NE(profile, nullptr);
  auto primitive = make_dict_array("kind", int64_t(TypeStructure::Kind::T_int));
  primitive.setEvalScalar();
  std::array<ArrayData*, 2> cached{
    bespoke::LoggingArray::MakeStatic(primitive.get(), profile),
    bespoke::LoggingArray::MakeShared(primitive.get(), profile, false)
  };
  std::array<LoggingMutation, 4> ops{
    LoggingMutation::Set, LoggingMutation::Remove,
    LoggingMutation::Pop, LoggingMutation::TypeStructure
  };
  for (auto const ad : cached) {
    for (auto const op : ops) checkLoggingMutation(ad, op, true);
  }
  if (testing::Test::HasFailure()) _exit(11);

  bespoke::selectBespokeLayouts();
  ASSERT_EQ(profile->data, nullptr);
  for (auto const ad : cached) {
    for (auto const op : ops) checkLoggingMutation(ad, op, false);
  }
  // Stop on tracking failures before reclaiming backing storage.
  if (testing::Test::HasFailure()) _exit(12);

  auto reclaimed = false;
  Treadmill::enqueue([&] { reclaimed = true; });
  Treadmill::finishRequest();
  ASSERT_TRUE(reclaimed);
  Treadmill::startRequest(Treadmill::SessionKind::UnitTests);
  for (auto const ad : cached) {
    for (auto const op : ops) checkLoggingMutation(ad, op, false);
  }
  _exit(testing::Test::HasFailure() ? 1 : 0);
}

}

TEST(LoggingArray, KeyOrderExpiresAfterLayoutSelection) {
  // Layout selection and profiling shutdown are process-wide and monotonic.
  auto const style = testing::FLAGS_gtest_death_test_style;
  testing::FLAGS_gtest_death_test_style = "threadsafe";
  EXPECT_EXIT(checkLoggingProfileTransition(), testing::ExitedWithCode(0), "");
  testing::FLAGS_gtest_death_test_style = style;
}

}
