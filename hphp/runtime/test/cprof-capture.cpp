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

#include "hphp/runtime/vm/jit/cprof-capture.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include <folly/ScopeGuard.h>
#include <folly/testing/TestUtil.h>
#include <gtest/gtest.h>

#include "hphp/runtime/base/rds.h"
#include "hphp/runtime/base/runtime-option.h"
#include "hphp/runtime/base/string-data.h"
#include "hphp/runtime/vm/as.h"
#include "hphp/runtime/vm/func.h"
#include "hphp/runtime/vm/hhbc.h"
#include "hphp/runtime/vm/jit/cprof-checkpoint.h"
#include "hphp/runtime/vm/jit/cprof-controller.h"
#include "hphp/runtime/vm/jit/cprof-reader.h"
#include "hphp/runtime/vm/jit/cprof-target-profile.h"
#include "hphp/runtime/vm/jit/decref-profile.h"
#include "hphp/runtime/vm/jit/prof-data.h"
#include "hphp/runtime/vm/jit/region-selection.h"
#include "hphp/runtime/vm/named-entity.h"
#include "hphp/runtime/vm/unit-emitter.h"
#include "hphp/runtime/vm/unit.h"
#include "hphp/util/assertions.h"
#include "hphp/util/configs/jit.h"
#include "hphp/util/configs/server.h"
#include "hphp/util/service-data.h"
#include "hphp/util/sha1.h"

namespace HPHP::jit::cprof {
namespace {

constexpr auto kUnitPath = "hphp/runtime/test/cont-prof-capture-test.php";

constexpr auto kHhas = R"HHAS(
.function N cont_prof_capture_test_81d926f4(named N $x = DV) {
  .declvars $y $z;
main:
  Null
  PopC
  Null
  RetC None
DV:
  Null
  PopL $x
  Enter main
}

.function N cont_prof_capture_required_test_81d926f4(N $x) {
main:
  Null
  RetC None
}

.function N cont_prof_capture_refresh_test_81d926f4() {
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
    unit->destroy();
  }
};

using TestUnit = std::unique_ptr<Unit, DestroyTestUnit>;

TestUnit makeTestUnit() {
  auto const emitter = assemble_string(
    kHhas,
    kUnitPath,
    SHA1{"2222222222222222222222222222222222222222"},
    nullptr,
    RepoOptions::defaults().packageInfo(),
    UnitEmitterAttributes::defaults(),
    false
  );
  if (!emitter || emitter->m_fatalUnit) return nullptr;
  return TestUnit{emitter->create().release()};
}

TransID addProfileTranslation(
  ProfData& profData,
  SrcKey start,
  int regionLength,
  int64_t executionCount,
  SBInvOffset spOffset = SBInvOffset{0},
  GuardedLocations preconditions = {},
  RegionDesc::BlockIdSet incoming = {},
  PostConditions postConditions = {}
) {
  auto region = std::make_shared<RegionDesc>();
  auto const block = region->addBlock(start, regionLength, spOffset);
  for (auto const& precondition : preconditions) {
    block->addPreCondition(precondition);
  }

  auto const transId = profData.allocTransID();
  region->incoming(std::move(incoming));
  profData.addTransProfile(transId, region, postConditions, 0);

  always_assert(executionCount >= 0);
  always_assert(executionCount <= profData.counterDefault());
  *profData.transCounterAddr(transId) =
    profData.counterDefault() - executionCount;
  return transId;
}

DecRefProfile makeDecRefProfileValue(
  uint32_t total,
  uint32_t refcounted,
  uint32_t released,
  uint32_t decremented,
  uint32_t arrayOfUncountedReleaseCount,
  DataType datatype
) {
  return {
    total,
    refcounted,
    released,
    decremented,
    arrayOfUncountedReleaseCount,
    datatype,
  };
}

struct ScopedDecRefProfile {
  ScopedDecRefProfile(
    TransID transId,
    Offset bytecodeOffset,
    const StringData* name,
    const DecRefProfile& value
  )
    : m_key{
        static_cast<DecRefProfile*>(nullptr),
        transId,
        bytecodeOffset,
        name,
      }
    , m_handle{
        rds::bind<DecRefProfile, rds::Mode::Local>(
          rds::Symbol{m_key}
        ).handle()
      } {
    set(value);
  }

  ~ScopedDecRefProfile() {
    rds::unbind(rds::Symbol{m_key}, m_handle);
  }

  ScopedDecRefProfile(const ScopedDecRefProfile&) = delete;
  ScopedDecRefProfile& operator=(const ScopedDecRefProfile&) = delete;

  void set(const DecRefProfile& value) {
    rds::handleToRef<DecRefProfile, rds::Mode::Local>(m_handle) = value;
  }

private:
  rds::Profile m_key;
  rds::Handle m_handle;
};

template<class T>
void appendTargetProfilePayload(std::vector<uint8_t>& payload, T value) {
  static_assert(std::is_unsigned_v<T>);

  auto const bytes = reinterpret_cast<const uint8_t*>(&value);
  payload.insert(payload.end(), bytes, bytes + sizeof(value));
}

ContProfTargetProfile makeContProfDecRefTargetProfile(
  Offset bytecodeOffset,
  int32_t profileId,
  const DecRefProfile& value
) {
  std::vector<uint8_t> payload;
  appendTargetProfilePayload(payload, static_cast<uint32_t>(value.total));
  appendTargetProfilePayload(
    payload,
    static_cast<uint32_t>(value.refcounted)
  );
  appendTargetProfilePayload(payload, static_cast<uint32_t>(value.released));
  appendTargetProfilePayload(
    payload,
    static_cast<uint32_t>(value.decremented)
  );
  appendTargetProfilePayload(
    payload,
    static_cast<uint32_t>(value.arrayOfUncountedReleaseCount)
  );
  appendTargetProfilePayload(payload, static_cast<uint8_t>(value.datatype));

  return {
    ContProfTargetProfileKind::DecRef,
    bytecodeOffset,
    decRefProfileKey(profileId)->toCppString(),
    std::move(payload),
  };
}

struct ContProfCaptureTest : testing::Test {
  static void SetUpTestSuite() {
    s_unit = makeTestUnit();
    ASSERT_NE(nullptr, s_unit);
    ASSERT_EQ(3, s_unit->funcs().size());

    s_func = s_unit->funcs()[0];
    s_requiredFunc = s_unit->funcs()[1];
    s_refreshFunc = s_unit->funcs()[2];
    ASSERT_EQ(3, s_func->numLocals());
    ASSERT_EQ(1, s_func->numFuncEntryInputs());
    ASSERT_EQ(1, s_requiredFunc->numRequiredPositionalParams());
    s_midOffset = instrLen(s_func->at(0));
    ASSERT_GT(s_midOffset, 0);
    ASSERT_TRUE(s_func->contains(s_midOffset));
    ASSERT_FALSE(s_func->isEntry(s_midOffset));
    s_emptyMidOffset = s_midOffset + instrLen(s_func->at(s_midOffset));
    ASSERT_TRUE(s_func->contains(s_emptyMidOffset));
    ASSERT_FALSE(s_func->isEntry(s_emptyMidOffset));
    s_unit->merge();
  }

  static void TearDownTestSuite() {
    s_func = nullptr;
    s_requiredFunc = nullptr;
    s_refreshFunc = nullptr;
    s_unit.reset();
  }

  static Func* func() { return s_func; }
  static Func* requiredFunc() { return s_requiredFunc; }
  static Func* refreshFunc() { return s_refreshFunc; }

  static SrcKey mid() { return SrcKey{func(), s_midOffset, ResumeMode::None}; }

  static SrcKey emptyMid() {
    return SrcKey{func(), s_emptyMidOffset, ResumeMode::None};
  }

private:
  static inline TestUnit s_unit;
  static inline Func* s_func{nullptr};
  static inline Func* s_requiredFunc{nullptr};
  static inline Func* s_refreshFunc{nullptr};
  static inline Offset s_midOffset{0};
  static inline Offset s_emptyMidOffset{0};
};

void expectEntryOnlyRecord(
  const std::optional<ContProfProfileRecord>& record
) {
  ASSERT_TRUE(record);

  std::vector<ContProfProfileTranslation> const expected{
    {ContProfStartKind::FuncEntry, 0, 2, 7},
  };
  EXPECT_EQ(expected, record->translations);
}

using Rejection = ContProfCaptureRejection;

void expectRejections(
  const ContProfCaptureStats& stats,
  std::initializer_list<std::pair<Rejection, size_t>> counts
) {
  decltype(stats.rejections) expected{};
  for (auto const& [reason, count] : counts) {
    expected[static_cast<size_t>(reason)] = count;
  }
  EXPECT_EQ(expected, stats.rejections);
}

TEST_F(ContProfCaptureTest, ResetsStatsWhenNoProfileTranslations) {
  ProfData profData;
  ContProfCaptureStats stats{};
  stats.translationsExamined = 7;
  stats.translationsEligible = 3;
  stats.translationsRetained = 2;
  stats.translationsStartDeduplicated = 1;
  stats.missingRegions = 1;
  stats.multipleBlockRegions = 1;
  stats.mismatchedBlockStarts = 1;
  stats.nonzeroStackOffsets = 1;
  stats.emptyBlocks = 1;
  stats.rejections.fill(1);
  std::vector<TransID> sourceTransIds{0};

  EXPECT_FALSE(snapshotContProfProfileRecord(
    profData, *func(), &sourceTransIds, &stats
  ));
  EXPECT_TRUE(sourceTransIds.empty());
  EXPECT_EQ(0, stats.translationsExamined);
  EXPECT_EQ(0, stats.translationsEligible);
  EXPECT_EQ(0, stats.translationsRetained);
  EXPECT_EQ(0, stats.translationsStartDeduplicated);
  EXPECT_EQ(0, stats.missingRegions);
  EXPECT_EQ(0, stats.multipleBlockRegions);
  EXPECT_EQ(0, stats.mismatchedBlockStarts);
  EXPECT_EQ(0, stats.nonzeroStackOffsets);
  EXPECT_EQ(0, stats.emptyBlocks);
  expectRejections(stats, {{Rejection::NoProfileTranslations, 1}});
}

TEST(ContProfCapture, CountsInvalidFunctionKey) {
  auto const builtin = Func::lookup(makeStaticString("strlen"));
  ASSERT_NE(nullptr, builtin);
  ASSERT_TRUE(builtin->isBuiltin());
  ProfData profData;
  ContProfCaptureStats stats{};

  EXPECT_FALSE(snapshotContProfProfileRecord(
    profData, *builtin, nullptr, &stats
  ));
  EXPECT_EQ(0, stats.translationsExamined);
  EXPECT_EQ(0, stats.translationsEligible);
  expectRejections(stats, {{Rejection::InvalidFuncKey, 1}});
}

TEST_F(ContProfCaptureTest, PreservesIncomingAcrossProfileRenumbering) {
  ProfData profData;
  profData.resetCounters(100);

  auto const transId = profData.allocTransID();
  auto const laterTransId = profData.allocTransID();
  auto region = std::make_shared<RegionDesc>();
  auto const oldId = region->addBlock(emptyMid(), 1, SBInvOffset{0})->id();
  region->incoming({transId, laterTransId});

  profData.addTransProfile(transId, region, PostConditions{}, 0);

  auto const record = profData.transRec(transId);
  ASSERT_NE(nullptr, record);
  auto const& published = *record->region();
  ASSERT_EQ(1, published.blocks().size());
  EXPECT_EQ(transId, published.entry()->id());
  EXPECT_TRUE(published.hasBlock(transId));
  EXPECT_FALSE(published.hasBlock(oldId));
  EXPECT_FALSE(published.hasBlock(laterTransId));
  RegionDesc::BlockIdSet const expectedIncoming{transId, laterTransId};
  ASSERT_NE(nullptr, published.incoming());
  EXPECT_EQ(expectedIncoming, *published.incoming());
  EXPECT_TRUE(published.preds(transId).empty());
  EXPECT_TRUE(published.succs(transId).empty());
}

TEST_F(ContProfCaptureTest, CapturesNonzeroTranslations) {
  ProfData profData;
  profData.resetCounters(100);

  auto const main = SrcKey{func(), 0, false, SrcKey::FuncEntryTag{}};
  auto const named = SrcKey{func(), 0, true, SrcKey::FuncEntryTag{}};

  addProfileTranslation(profData, named, 1, 11);
  auto const mainTransId = addProfileTranslation(profData, main, 2, 7);
  addProfileTranslation(
    profData,
    emptyMid(),
    1,
    13,
    SBInvOffset{0},
    {},
    {mainTransId}
  );
  addProfileTranslation(profData, main, 3, 0);

  ContProfCaptureStats stats{};
  auto const record = snapshotContProfProfileRecord(
    profData, *func(), nullptr, &stats
  );
  ASSERT_TRUE(record);
  EXPECT_EQ(4, stats.translationsExamined);
  EXPECT_EQ(3, stats.translationsEligible);
  expectRejections(stats, {{Rejection::ZeroExecutionCount, 1}});

  auto const expectedKey = makeContProfFuncKey(*func());
  ASSERT_TRUE(expectedKey);

  std::vector<ContProfProfileTranslation> const expected{
    {ContProfStartKind::FuncEntry, 0, 2, 7},
    {ContProfStartKind::NamedParamsFuncEntry, 0, 1, 11},
    {
      ContProfStartKind::Bytecode,
      static_cast<uint32_t>(emptyMid().offset()),
      1,
      13,
      {},
      {0},
    },
  };
  EXPECT_EQ(*expectedKey, record->header.funcKey);
  EXPECT_GT(record->header.capturedAtMs, 0);
  EXPECT_EQ(18, record->functionExecutions());
  EXPECT_EQ(expected, record->translations);
}

TEST_F(ContProfCaptureTest, SelectsBestDuplicate) {
  ProfData profData;
  profData.resetCounters(100);

  auto const main = SrcKey{func(), 0, false, SrcKey::FuncEntryTag{}};
  addProfileTranslation(profData, main, 3, 5);
  addProfileTranslation(profData, main, 2, 7);
  addProfileTranslation(profData, main, 1, 7);

  ContProfCaptureStats stats{};
  auto const record = snapshotContProfProfileRecord(
    profData, *func(), nullptr, &stats
  );
  ASSERT_TRUE(record);
  EXPECT_EQ(3, stats.translationsExamined);
  EXPECT_EQ(3, stats.translationsEligible);
  EXPECT_EQ(1, stats.translationsRetained);
  EXPECT_EQ(2, stats.translationsStartDeduplicated);
  expectRejections(stats, {});

  std::vector<ContProfProfileTranslation> const expected{
    {ContProfStartKind::FuncEntry, 0, 1, 7},
  };
  EXPECT_EQ(7, record->functionExecutions());
  EXPECT_EQ(expected, record->translations);
}

TEST_F(ContProfCaptureTest, SelectsBestBytecodeDuplicate) {
  ProfData profData;
  profData.resetCounters(100);

  auto const main = SrcKey{func(), 0, false, SrcKey::FuncEntryTag{}};
  addProfileTranslation(profData, main, 2, 7);
  addProfileTranslation(profData, emptyMid(), 3, 5);
  addProfileTranslation(profData, emptyMid(), 2, 7);
  addProfileTranslation(profData, emptyMid(), 1, 7);

  auto const record = snapshotContProfProfileRecord(profData, *func());
  ASSERT_TRUE(record);

  std::vector<ContProfProfileTranslation> const expected{
    {ContProfStartKind::FuncEntry, 0, 2, 7},
    {
      ContProfStartKind::Bytecode,
      static_cast<uint32_t>(emptyMid().offset()),
      1,
      7,
    },
  };
  EXPECT_EQ(7, record->functionExecutions());
  EXPECT_EQ(expected, record->translations);
}

TEST_F(ContProfCaptureTest, RequiresEntryTranslation) {
  ProfData profData;
  profData.resetCounters(100);

  addProfileTranslation(profData, emptyMid(), 1, 7);

  ContProfCaptureStats stats{};
  EXPECT_FALSE(snapshotContProfProfileRecord(
    profData, *func(), nullptr, &stats
  ));
  EXPECT_EQ(1, stats.translationsExamined);
  EXPECT_EQ(1, stats.translationsEligible);
  EXPECT_EQ(1, stats.translationsRetained);
  EXPECT_EQ(0, stats.translationsStartDeduplicated);
  expectRejections(stats, {{Rejection::InvalidFinalRecord, 1}});
}

TEST_F(ContProfCaptureTest, SkipsBytecodeWithNonzeroStackOffset) {
  ProfData profData;
  profData.resetCounters(100);

  auto const main = SrcKey{func(), 0, false, SrcKey::FuncEntryTag{}};
  addProfileTranslation(profData, main, 2, 7);
  addProfileTranslation(profData, mid(), 1, 5, SBInvOffset{1});
  addProfileTranslation(profData, mid(), 1, 11, SBInvOffset{-1});

  ContProfCaptureStats stats{};
  expectEntryOnlyRecord(snapshotContProfProfileRecord(
    profData, *func(), nullptr, &stats
  ));
  EXPECT_EQ(3, stats.translationsExamined);
  EXPECT_EQ(1, stats.translationsEligible);
  EXPECT_EQ(2, stats.nonzeroStackOffsets);
  EXPECT_EQ(0, stats.mismatchedBlockStarts);
  EXPECT_EQ(0, stats.emptyBlocks);
  expectRejections(stats, {{Rejection::UnexpectedBlock, 2}});
}

TEST_F(ContProfCaptureTest, SkipsBytecodeWithNonLocalTypeGuards) {
  ProfData profData;
  profData.resetCounters(100);

  auto const main = SrcKey{func(), 0, false, SrcKey::FuncEntryTag{}};
  addProfileTranslation(profData, main, 2, 7);
  addProfileTranslation(
    profData,
    emptyMid(),
    1,
    5,
    SBInvOffset{0},
    {{Location::Stack{SBInvOffset{0}}, TObj, DataTypeSpecific}}
  );
  addProfileTranslation(
    profData,
    emptyMid(),
    1,
    11,
    SBInvOffset{0},
    {{Location::MBase{0}, TObj, DataTypeSpecific}}
  );

  ContProfCaptureStats stats{};
  expectEntryOnlyRecord(snapshotContProfProfileRecord(
    profData, *func(), nullptr, &stats
  ));
  expectRejections(stats, {{Rejection::InvalidLocalTypeGuards, 2}});
}

TEST_F(ContProfCaptureTest, SkipsInvalidEntryRegion) {
  ProfData profData;
  profData.resetCounters(100);

  auto const main = SrcKey{func(), 0, false, SrcKey::FuncEntryTag{}};
  addProfileTranslation(profData, main, 1, 5, SBInvOffset{1});

  ContProfCaptureStats stats{};
  EXPECT_FALSE(snapshotContProfProfileRecord(
    profData, *func(), nullptr, &stats
  ));
  expectRejections(stats, {
    {Rejection::UnexpectedBlock, 1},
    {Rejection::NoEligibleCandidates, 1},
  });
}

TEST_F(ContProfCaptureTest, CountsFirstTranslationRejection) {
  ProfData profData;
  profData.resetCounters(100);
  auto const main = SrcKey{func(), 0, false, SrcKey::FuncEntryTag{}};
  addProfileTranslation(profData, main, 1, 0, SBInvOffset{1});

  ContProfCaptureStats stats{};
  EXPECT_FALSE(snapshotContProfProfileRecord(
    profData, *func(), nullptr, &stats
  ));
  EXPECT_EQ(1, stats.translationsExamined);
  EXPECT_EQ(0, stats.translationsEligible);
  expectRejections(stats, {
    {Rejection::UnexpectedBlock, 1},
    {Rejection::NoEligibleCandidates, 1},
  });
}

TEST_F(ContProfCaptureTest, CountsZeroExecutionCountWithoutCandidates) {
  ProfData profData;
  profData.resetCounters(100);
  auto const main = SrcKey{func(), 0, false, SrcKey::FuncEntryTag{}};
  addProfileTranslation(profData, main, 1, 0);

  ContProfCaptureStats stats{};
  EXPECT_FALSE(snapshotContProfProfileRecord(
    profData, *func(), nullptr, &stats
  ));
  expectRejections(stats, {
    {Rejection::ZeroExecutionCount, 1},
    {Rejection::NoEligibleCandidates, 1},
  });
}

TEST_F(ContProfCaptureTest, CountsUnexpectedMultiblockRegion) {
  ProfData profData;
  profData.resetCounters(100);
  auto const main = SrcKey{func(), 0, false, SrcKey::FuncEntryTag{}};
  auto const transId = addProfileTranslation(profData, main, 1, 7);
  // addTransProfile requires one block; alter it afterwards to exercise the
  // defensive rejection of an unexpected region shape.
  profData.transRec(transId)->region()->addBlock(emptyMid(), 1, SBInvOffset{0});

  ContProfCaptureStats stats{};
  EXPECT_FALSE(snapshotContProfProfileRecord(
    profData, *func(), nullptr, &stats
  ));
  EXPECT_EQ(1, stats.multipleBlockRegions);
  EXPECT_EQ(0, stats.missingRegions);
  expectRejections(stats, {
    {Rejection::UnexpectedRegion, 1},
    {Rejection::NoEligibleCandidates, 1},
  });
}

TEST_F(ContProfCaptureTest, CountsResumedAndInvalidBytecodeStarts) {
  ProfData profData;
  profData.resetCounters(100);
  addProfileTranslation(
    profData, SrcKey{func(), emptyMid().offset(), ResumeMode::Async}, 1, 7
  );
  addProfileTranslation(profData, SrcKey{func(), 0, ResumeMode::None}, 1, 7);

  ContProfCaptureStats stats{};
  EXPECT_FALSE(snapshotContProfProfileRecord(
    profData, *func(), nullptr, &stats
  ));
  EXPECT_EQ(2, stats.translationsExamined);
  EXPECT_EQ(0, stats.translationsEligible);
  expectRejections(stats, {
    {Rejection::PrologueOrResumedTranslation, 1},
    {Rejection::InvalidEntry, 1},
    {Rejection::NoEligibleCandidates, 1},
  });
}

TEST_F(ContProfCaptureTest, SkipsUnusableTranslations) {
  ProfData profData;
  profData.resetCounters(100);

  auto const main = SrcKey{func(), 0, false, SrcKey::FuncEntryTag{}};
  addProfileTranslation(profData, main, 1, 5, SBInvOffset{1});
  addProfileTranslation(profData, main, 2, 9);

  auto const record = snapshotContProfProfileRecord(profData, *func());
  ASSERT_TRUE(record);

  std::vector<ContProfProfileTranslation> const expected{
    {ContProfStartKind::FuncEntry, 0, 2, 9},
  };
  EXPECT_EQ(9, record->functionExecutions());
  EXPECT_EQ(expected, record->translations);
}

TEST_F(ContProfCaptureTest, CapturesCanonicalLocalTypeGuards) {
  ProfData profData;
  profData.resetCounters(100);

  auto const main = SrcKey{func(), 0, false, SrcKey::FuncEntryTag{}};
  GuardedLocations const preconditions{
    {Location::Local{1}, TStaticStr, DataTypeSpecific},
    {
      Location::Local{0},
      Type::cns(int64_t{42}),
      DataTypeSpecific,
    },
    {Location::Local{0}, TInt, DataTypeSpecific},
    {Location::Local{1}, TCell, DataTypeGeneric},
  };

  addProfileTranslation(profData, main, 2, 7);
  addProfileTranslation(
    profData,
    emptyMid(),
    1,
    11,
    SBInvOffset{0},
    preconditions
  );

  auto const record = snapshotContProfProfileRecord(profData, *func());
  ASSERT_TRUE(record);

  std::vector<ContProfProfileTranslation> const expected{
    {ContProfStartKind::FuncEntry, 0, 2, 7},
    {
      ContProfStartKind::Bytecode,
      static_cast<uint32_t>(emptyMid().offset()),
      1,
      11,
      {
        {0, Type::kInt},
        {1, Type::kStaticStr},
      },
    },
  };
  EXPECT_EQ(expected, record->translations);
}

TEST_F(ContProfCaptureTest, CapturesUnionLocalTypeGuard) {
  ProfData profData;
  profData.resetCounters(100);

  auto const main = SrcKey{func(), 0, false, SrcKey::FuncEntryTag{}};
  GuardedLocations const preconditions{
    {Location::Local{0}, TInt | TBool, DataTypeCountnessInit},
  };

  addProfileTranslation(profData, main, 2, 7);
  addProfileTranslation(
    profData,
    emptyMid(),
    1,
    11,
    SBInvOffset{0},
    preconditions
  );

  auto const record = snapshotContProfProfileRecord(profData, *func());
  ASSERT_TRUE(record);
  ASSERT_EQ(2, record->translations.size());
  std::vector<ContProfLocalTypeGuard> const expected{
    {0, Type::kUncountedInit, DataTypeCountnessInit},
  };
  EXPECT_EQ(expected, record->translations.back().localTypeGuards);
}

TEST_F(ContProfCaptureTest, CapturesEntryUnionLocalTypeGuard) {
  ProfData profData;
  profData.resetCounters(100);

  auto const main = SrcKey{func(), 0, false, SrcKey::FuncEntryTag{}};
  GuardedLocations const preconditions{
    {Location::Local{0}, TVec | TDict, DataTypeGeneric},
  };

  addProfileTranslation(
    profData,
    main,
    2,
    7,
    SBInvOffset{0},
    preconditions
  );

  auto const record = snapshotContProfProfileRecord(profData, *func());
  ASSERT_TRUE(record);
  ASSERT_EQ(1, record->translations.size());
  std::vector<ContProfLocalTypeGuard> const expected{
    {0, Type::kArrLike, DataTypeGeneric},
  };
  EXPECT_EQ(expected, record->translations.front().localTypeGuards);
}

TEST_F(ContProfCaptureTest, KeepsCountnessAndDropsSpecializedGuardCategories) {
  ProfData profData;
  profData.resetCounters(100);
  auto const main = SrcKey{func(), 0, false, SrcKey::FuncEntryTag{}};
  addProfileTranslation(profData, main, 2, 7);
  addProfileTranslation(
    profData, emptyMid(), 1, 11, SBInvOffset{0},
    {
      {Location::Local{0}, TInt | TBool, DataTypeCountnessInit},
      {Location::Local{1}, TVec.narrowToVanilla(), DataTypeSpecialized},
      {Location::Local{2}, TStaticStr, DataTypeCountnessInit},
      {Location::Local{2}, TStaticStr, DataTypeSpecific},
    }
  );

  auto const record = snapshotContProfProfileRecord(profData, *func());
  ASSERT_TRUE(record);
  ASSERT_EQ(2, record->translations.size());
  std::vector<ContProfLocalTypeGuard> const expected{
    {0, Type::kUncountedInit, DataTypeCountnessInit},
    {1, Type::kVec, DataTypeSpecific},
    {2, Type::kStaticStr, DataTypeSpecific},
  };
  EXPECT_EQ(expected, record->translations.back().localTypeGuards);
}

TEST_F(ContProfCaptureTest, CapturesEntryLocalTypeGuard) {
  ProfData profData;
  profData.resetCounters(100);

  auto const entry = SrcKey{
    func(),
    func()->numPositionalParams(),
    false,
    SrcKey::FuncEntryTag{},
  };
  GuardedLocations const preconditions{
    {Location::Local{0}, TInt, DataTypeSpecific},
  };

  addProfileTranslation(
    profData,
    entry,
    2,
    7,
    SBInvOffset{0},
    preconditions
  );

  auto const record = snapshotContProfProfileRecord(profData, *func());
  ASSERT_TRUE(record);

  std::vector<ContProfProfileTranslation> const expected{
    {
      ContProfStartKind::FuncEntry,
      func()->numPositionalParams(),
      2,
      7,
      {{0, Type::kInt}},
    },
  };
  EXPECT_EQ(expected, record->translations);
}

TEST_F(ContProfCaptureTest, SkipsEntryGuardOutsideEntryInputs) {
  ProfData profData;
  profData.resetCounters(100);

  auto const main = SrcKey{func(), 0, false, SrcKey::FuncEntryTag{}};
  GuardedLocations const preconditions{
    {Location::Local{func()->numFuncEntryInputs()}, TInt, DataTypeSpecific},
  };

  addProfileTranslation(
    profData,
    main,
    2,
    7,
    SBInvOffset{0},
    preconditions
  );

  EXPECT_FALSE(snapshotContProfProfileRecord(profData, *func()));
}

TEST_F(ContProfCaptureTest, SkipsGuardedEntryBelowRequiredArguments) {
  ProfData profData;
  profData.resetCounters(100);

  auto const entry = SrcKey{requiredFunc(), 0, false, SrcKey::FuncEntryTag{}};
  GuardedLocations const preconditions{
    {Location::Local{0}, TInt, DataTypeSpecific},
  };

  addProfileTranslation(profData, entry, 1, 5);
  addProfileTranslation(
    profData,
    entry,
    1,
    7,
    SBInvOffset{0},
    preconditions
  );

  ContProfCaptureStats stats{};
  auto const record = snapshotContProfProfileRecord(
    profData, *requiredFunc(), nullptr, &stats
  );
  ASSERT_TRUE(record);
  expectRejections(stats, {{Rejection::InvalidEntryTypeGuards, 1}});

  std::vector<ContProfProfileTranslation> const expected{
    {ContProfStartKind::FuncEntry, 0, 1, 5},
  };
  EXPECT_EQ(expected, record->translations);
}

TEST_F(ContProfCaptureTest, SkipsConflictingLocalTypeGuards) {
  ProfData profData;
  profData.resetCounters(100);

  auto const main = SrcKey{func(), 0, false, SrcKey::FuncEntryTag{}};
  GuardedLocations const preconditions{
    {Location::Local{0}, TInt, DataTypeSpecific},
    {Location::Local{0}, TStr, DataTypeSpecific},
  };

  addProfileTranslation(profData, main, 2, 7);
  addProfileTranslation(
    profData,
    emptyMid(),
    1,
    11,
    SBInvOffset{0},
    preconditions
  );

  expectEntryOnlyRecord(snapshotContProfProfileRecord(profData, *func()));
}

TEST_F(ContProfCaptureTest, SkipsOutOfRangeLocalTypeGuard) {
  ProfData profData;
  profData.resetCounters(100);

  auto const main = SrcKey{func(), 0, false, SrcKey::FuncEntryTag{}};
  GuardedLocations const preconditions{
    {
      Location::Local{
        static_cast<uint32_t>(func()->numLocals())
      },
      TInt,
      DataTypeSpecific,
    },
  };

  addProfileTranslation(profData, main, 2, 7);
  addProfileTranslation(
    profData,
    emptyMid(),
    1,
    11,
    SBInvOffset{0},
    preconditions
  );

  expectEntryOnlyRecord(snapshotContProfProfileRecord(profData, *func()));
}

TEST_F(ContProfCaptureTest, CapturesLocalPostConditions) {
  ProfData profData;
  profData.resetCounters(100);

  auto const main = SrcKey{func(), 0, false, SrcKey::FuncEntryTag{}};
  PostConditions postConditions{};
  postConditions.changed = {
    {Location::Local{0}, Type::cns(int64_t{42})},
    {Location::Local{1}, TInt | TStr},
  };
  postConditions.refined = {
    {Location::Local{2}, TStaticStr | TInt},
  };

  addProfileTranslation(profData, main, 2, 7);
  addProfileTranslation(
    profData,
    emptyMid(),
    1,
    11,
    SBInvOffset{0},
    {},
    {},
    postConditions
  );

  auto const record = snapshotContProfProfileRecord(profData, *func());
  ASSERT_TRUE(record);

  std::vector<ContProfLocalPostCondition> const expected{
    {0, true, Type::kInt},
    {1, true, Type::kInt | Type::kStr},
    {2, false, Type::kStaticStr | Type::kInt},
  };
  ASSERT_EQ(2, record->translations.size());
  EXPECT_EQ(expected, record->translations.back().localPostConditions);
}

TEST_F(ContProfCaptureTest, CountsInvalidLocalPostconditions) {
  ProfData profData;
  profData.resetCounters(100);
  auto const main = SrcKey{func(), 0, false, SrcKey::FuncEntryTag{}};
  addProfileTranslation(profData, main, 2, 7);
  for (auto const type : {TBottom, TStkPtr}) {
    PostConditions posts{};
    posts.changed = {{Location::Local{0}, type}};
    addProfileTranslation(
      profData, emptyMid(), 1, 11, SBInvOffset{0}, {}, {}, posts
    );
  }
  PostConditions posts{};
  posts.refined = {
    {Location::Local{static_cast<uint32_t>(func()->numLocals())}, TInt},
  };
  addProfileTranslation(
    profData, emptyMid(), 1, 11, SBInvOffset{0}, {}, {}, posts
  );

  ContProfCaptureStats stats{};
  expectEntryOnlyRecord(snapshotContProfProfileRecord(
    profData, *func(), nullptr, &stats
  ));
  EXPECT_EQ(4, stats.translationsExamined);
  EXPECT_EQ(1, stats.translationsEligible);
  expectRejections(stats, {{Rejection::InvalidLocalPostconditions, 3}});
}

TEST_F(ContProfCaptureTest, CountsOnlyEnabledCaptureOpportunities) {
  auto const enabled = std::exchange(Cfg::Jit::ContProfCaptureEnabled, false);
  SCOPE_EXIT { Cfg::Jit::ContProfCaptureEnabled = enabled; };
  ProfData profData;
  auto const opportunities = ServiceData::exportCounterByKey(
    "jit.cprof.capture.opportunities"
  );
  auto const asyncOpportunities = ServiceData::exportCounterByKey(
    "jit.cprof.capture.async_opportunities"
  );
  auto const inactive = ServiceData::exportCounterByKey(
    "jit.cprof.capture.skipped_inactive"
  );
  auto const attempts = ServiceData::exportCounterByKey(
    "jit.cprof.capture.attempts"
  );
  auto const opportunitiesBefore = opportunities.value_or(0);
  auto const asyncOpportunitiesBefore = asyncOpportunities.value_or(0);
  auto const inactiveBefore = inactive.value_or(0);

  maybeCaptureContProfProfile(profData, *func(), true /* async */);
  EXPECT_EQ(opportunities, ServiceData::exportCounterByKey(
    "jit.cprof.capture.opportunities"
  ));
  EXPECT_EQ(inactive, ServiceData::exportCounterByKey(
    "jit.cprof.capture.skipped_inactive"
  ));
  EXPECT_EQ(asyncOpportunities, ServiceData::exportCounterByKey(
    "jit.cprof.capture.async_opportunities"
  ));

  Cfg::Jit::ContProfCaptureEnabled = true;
  ASSERT_FALSE(contProfActive());
  maybeCaptureContProfProfile(profData, *func());
  EXPECT_EQ(opportunitiesBefore + 1, ServiceData::exportCounterByKey(
    "jit.cprof.capture.opportunities"
  ).value_or(0));
  EXPECT_EQ(inactiveBefore + 1, ServiceData::exportCounterByKey(
    "jit.cprof.capture.skipped_inactive"
  ).value_or(0));
  EXPECT_EQ(asyncOpportunities, ServiceData::exportCounterByKey(
    "jit.cprof.capture.async_opportunities"
  ));
  maybeCaptureContProfProfile(profData, *func(), true /* async */);
  EXPECT_EQ(opportunitiesBefore + 2, ServiceData::exportCounterByKey(
    "jit.cprof.capture.opportunities"
  ).value_or(0));
  EXPECT_EQ(asyncOpportunitiesBefore + 1, ServiceData::exportCounterByKey(
    "jit.cprof.capture.async_opportunities"
  ).value_or(0));
  EXPECT_EQ(inactiveBefore + 2, ServiceData::exportCounterByKey(
    "jit.cprof.capture.skipped_inactive"
  ).value_or(0));
  EXPECT_EQ(attempts, ServiceData::exportCounterByKey(
    "jit.cprof.capture.attempts"
  ));
}

TEST_F(ContProfCaptureTest, StoresFirstRecordAndFinalizesTargetProfilesOnce) {
  auto const before = snapshotContProfProfileRecords();
  auto const counters = [] {
    return std::array{
      ServiceData::exportCounterByKey(
        "jit.cprof.capture.attempts").value_or(0),
      ServiceData::exportCounterByKey(
        "jit.cprof.capture.snapshot_successes").value_or(0),
      ServiceData::exportCounterByKey(
        "jit.cprof.capture.snapshot_failures").value_or(0),
      ServiceData::exportCounterByKey(
        "jit.cprof.capture.stored_records").value_or(0),
      ServiceData::exportCounterByKey(
        "jit.cprof.capture.duplicates").value_or(0),
      ServiceData::exportCounterByKey(
        "jit.cprof.capture.translations_examined").value_or(0),
      ServiceData::exportCounterByKey(
        "jit.cprof.capture.translations_eligible").value_or(0),
      ServiceData::exportCounterByKey(
        "jit.cprof.capture.translations_retained").value_or(0),
      ServiceData::exportCounterByKey(
        "jit.cprof.capture.translations_start_deduplicated").value_or(0),
      ServiceData::exportCounterByKey(
        "jit.cprof.capture.rejected.nonzero_stack_offset").value_or(0),
      ServiceData::exportCounterByKey(
        "jit.cprof.capture.records").value_or(0),
    };
  };
  auto const countersBefore = counters();
  for (auto const count : countersBefore) ASSERT_GE(count, 0);
  EXPECT_EQ(before.size(), countersBefore.back());
  auto const missingProfiles = ServiceData::exportCounterByKey(
    "jit.cprof.capture.rejected.no_profile_translations"
  ).value_or(0);
  auto const unexpectedBlocks = ServiceData::exportCounterByKey(
    "jit.cprof.capture.rejected.unexpected_block"
  ).value_or(0);

  ProfData profData;
  profData.resetCounters(100);
  EXPECT_FALSE(captureContProfProfile(profData, *func()));

  auto const main = SrcKey{func(), 0, false, SrcKey::FuncEntryTag{}};
  auto const discardedMainTransId = addProfileTranslation(profData, main, 3, 5);
  auto const selectedMainTransId = addProfileTranslation(profData, main, 2, 7);
  addProfileTranslation(profData, main, 1, 5, SBInvOffset{1});
  addProfileTranslation(profData, main, 1, 5, SBInvOffset{-1});
  auto const midTransId = addProfileTranslation(
    profData,
    emptyMid(),
    1,
    13,
    SBInvOffset{0}
  );

  auto const selectedDefaultValue =
    makeDecRefProfileValue(9, 6, 2, 3, 1, KindOfString);
  ScopedDecRefProfile selectedDefault{
    selectedMainTransId,
    0,
    decRefProfileKey(-1),
    selectedDefaultValue,
  };

  auto const direct = snapshotContProfProfileRecord(profData, *func());
  ASSERT_TRUE(direct);
  ASSERT_EQ(2, direct->translations.size());
  EXPECT_TRUE(direct->translations[0].targetProfiles.empty());
  EXPECT_TRUE(direct->translations[1].targetProfiles.empty());

  EXPECT_TRUE(captureContProfProfile(profData, *func()));

  auto const laterMainTransId = addProfileTranslation(profData, main, 1, 11);
  EXPECT_FALSE(captureContProfProfile(profData, *func()));

  auto const selectedNamedValue =
    makeDecRefProfileValue(17, 12, 3, 5, 1, KindOfObject);
  ScopedDecRefProfile const selectedNamed{
    selectedMainTransId,
    0,
    decRefProfileKey(2'000'000),
    selectedNamedValue,
  };
  auto const selectedMidValue =
    makeDecRefProfileValue(11, 8, 2, 4, 0, kInvalidDataType);
  ScopedDecRefProfile const selectedMid{
    midTransId,
    emptyMid().offset(),
    decRefProfileKey(2'000'001),
    selectedMidValue,
  };
  ScopedDecRefProfile const discarded{
    discardedMainTransId,
    0,
    decRefProfileKey(2'000'002),
    makeDecRefProfileValue(23, 15, 4, 7, 1, KindOfVec),
  };
  ScopedDecRefProfile const later{
    laterMainTransId,
    0,
    decRefProfileKey(2'000'003),
    makeDecRefProfileValue(29, 19, 5, 8, 2, KindOfDict),
  };
  ScopedDecRefProfile const negativeOffset{
    selectedMainTransId,
    Offset{-1},
    decRefProfileKey(2'000'011),
    makeDecRefProfileValue(7, 4, 1, 2, 0, KindOfString),
  };
  ScopedDecRefProfile const outsideFunction{
    selectedMainTransId,
    func()->bclen(),
    decRefProfileKey(2'000'012),
    makeDecRefProfileValue(7, 4, 1, 2, 0, KindOfString),
  };
  ScopedDecRefProfile const zero{
    selectedMainTransId,
    0,
    decRefProfileKey(2'000'014),
    makeDecRefProfileValue(0, 0, 0, 0, 0, kExtraInvalidDataType),
  };

  auto const requiredTransId = addProfileTranslation(
    profData,
    SrcKey{requiredFunc(), 1, false, SrcKey::FuncEntryTag{}},
    2,
    5
  );
  EXPECT_TRUE(captureContProfProfile(profData, *requiredFunc()));

  auto const records = snapshotContProfProfileRecords();
  ASSERT_EQ(before.size() + 2, records.size());

  auto const expectedKey = makeContProfFuncKey(*func());
  ASSERT_TRUE(expectedKey);

  auto const stored = std::find_if(
    records.begin(),
    records.end(),
    [&](auto const& record) {
      return record.header.funcKey == *expectedKey;
    }
  );
  ASSERT_NE(records.end(), stored);

  EXPECT_EQ(7, stored->functionExecutions());
  ASSERT_EQ(2, stored->translations.size());

  auto const& entry = stored->translations[0];
  EXPECT_EQ(ContProfStartKind::FuncEntry, entry.startKind);
  EXPECT_EQ(2, entry.regionLength);
  EXPECT_EQ(7, entry.executionCount);
  ASSERT_EQ(2, entry.targetProfiles.size());
  EXPECT_EQ(
    makeContProfDecRefTargetProfile(0, -1, selectedDefaultValue),
    entry.targetProfiles[0]
  );
  EXPECT_EQ(
    makeContProfDecRefTargetProfile(0, 2'000'000, selectedNamedValue),
    entry.targetProfiles[1]
  );

  auto const& bytecode = stored->translations[1];
  ASSERT_EQ(ContProfStartKind::Bytecode, bytecode.startKind);
  EXPECT_EQ(emptyMid().offset(), bytecode.offset());
  EXPECT_EQ(13, bytecode.executionCount);
  ASSERT_EQ(1, bytecode.targetProfiles.size());
  EXPECT_EQ(
    makeContProfDecRefTargetProfile(
      emptyMid().offset(),
      2'000'001,
      selectedMidValue
    ),
    bytecode.targetProfiles[0]
  );

  selectedDefault.set(
    makeDecRefProfileValue(31, 21, 6, 9, 2, KindOfVec)
  );

  auto const requiredKey = makeContProfFuncKey(*requiredFunc());
  ASSERT_TRUE(requiredKey);
  auto const required = std::find_if(
    records.begin(),
    records.end(),
    [&](auto const& record) {
      return record.header.funcKey == *requiredKey;
    }
  );
  ASSERT_NE(records.end(), required);
  ASSERT_EQ(1, required->translations.size());
  EXPECT_TRUE(required->translations[0].targetProfiles.empty());

  // Even a sweep with no target profiles finalizes the record.
  ScopedDecRefProfile const afterFinalization{
    requiredTransId,
    0,
    decRefProfileKey(-1),
    selectedDefaultValue,
  };
  EXPECT_EQ(records, snapshotContProfProfileRecords());

  // The direct snapshot above is not a capture attempt. The controller saw
  // one failure, two new records, and one valid duplicate snapshot.
  auto const countersAfter = counters();
  std::array<int64_t, 11> const deltas{4, 3, 1, 2, 1, 12, 8, 5, 3, 4, 2};
  for (size_t i = 0; i < deltas.size(); ++i) {
    EXPECT_EQ(countersBefore[i] + deltas[i], countersAfter[i]);
  }
  EXPECT_EQ(records.size(), countersAfter.back());
  EXPECT_EQ(missingProfiles + 1, ServiceData::exportCounterByKey(
    "jit.cprof.capture.rejected.no_profile_translations"
  ).value_or(0));
  EXPECT_EQ(unexpectedBlocks + 4, ServiceData::exportCounterByKey(
    "jit.cprof.capture.rejected.unexpected_block"
  ).value_or(0));
}

TEST_F(ContProfCaptureTest, ReplacesRetainedRecordAndRewritesCheckpoint) {
  auto const counters = [] {
    return std::array{
      ServiceData::exportCounterByKey(
        "jit.cprof.startup.retained_records").value_or(0),
      ServiceData::exportCounterByKey(
        "jit.cprof.capture.replaced_records").value_or(0),
      ServiceData::exportCounterByKey(
        "jit.cprof.capture.stored_records").value_or(0),
    };
  };
  auto expectedCounters = counters();
  for (auto const count : expectedCounters) ASSERT_GE(count, 0);
  auto const before = snapshotContProfProfileRecords();
  auto const key = makeContProfFuncKey(*refreshFunc());
  ASSERT_TRUE(key);
  ContProfProfileRecord original;
  original.header = {*key, 1};
  original.translations = {{ContProfStartKind::FuncEntry, 0, 2, 41}};
  original.translations.front().targetProfiles = {
    makeContProfDecRefTargetProfile(
      0, -1, makeDecRefProfileValue(9, 6, 2, 3, 1, KindOfString)
    ),
  };
  ASSERT_TRUE(retainContProfProfileRecord(original));
  ++expectedCounters[0];
  EXPECT_EQ(expectedCounters, counters());
  EXPECT_EQ(before.size() + 1, numContProfProfileRecords());

  auto duplicate = original;
  duplicate.header.capturedAtMs = 2;
  duplicate.translations.front().executionCount = 99;
  EXPECT_FALSE(retainContProfProfileRecord(duplicate));
  ProfData profData;
  profData.resetCounters(100);
  EXPECT_FALSE(captureContProfProfile(profData, *refreshFunc()));
  EXPECT_EQ(expectedCounters, counters());

  auto const retained = snapshotContProfProfileRecords();
  auto const stored = std::find_if(
    retained.begin(), retained.end(),
    [&](auto const& record) { return record.header.funcKey == *key; }
  );
  ASSERT_NE(retained.end(), stored);
  // Retention is already finalized: no live RDS is needed to preserve it.
  EXPECT_EQ(original, *stored);
  EXPECT_EQ(retained, snapshotContProfProfileRecords());

  folly::test::TemporaryDirectory directory{"cont-prof-refresh"};
  auto const server = std::exchange(Cfg::Server::Mode, true);
  auto const enabled = std::exchange(Cfg::Jit::ContProfCaptureEnabled, true);
  auto const interval = std::exchange(
    Cfg::Jit::ContProfCheckpointIntervalSeconds, uint32_t{1}
  );
  auto const path = std::exchange(
    Cfg::Jit::ContProfCheckpointDirectory, directory.path().native()
  );
  SCOPE_EXIT {
    stopContProfCheckpointWriter();
    Cfg::Server::Mode = server;
    Cfg::Jit::ContProfCaptureEnabled = enabled;
    Cfg::Jit::ContProfCheckpointIntervalSeconds = interval;
    Cfg::Jit::ContProfCheckpointDirectory = path;
  };
  auto const writes = [] {
    return ServiceData::exportCounterByKey(
      "jit.cprof.checkpoint.write_successes"
    ).value_or(0);
  };
  auto const writesBefore = writes();
  ASSERT_GE(writesBefore, 0);
  startContProfCheckpointWriter();
  ASSERT_TRUE(contProfActive());
  auto const deadline = std::chrono::steady_clock::now() +
    std::chrono::seconds{30};
  std::mutex mutex;
  std::condition_variable condition;
  std::unique_lock lock{mutex};
  // Two scheduled writes must succeed without changing the stored records.
  while (writes() < writesBefore + 2 &&
         std::chrono::steady_clock::now() < deadline) {
    condition.wait_for(lock, std::chrono::milliseconds{20});
  }
  ASSERT_GE(writes(), writesBefore + 2);
  auto const first = readContProfCheckpointDirectory(directory.path().native());
  ASSERT_TRUE(first);
  EXPECT_EQ(retained, first->records);

  auto const transId = addProfileTranslation(
    profData, SrcKey{refreshFunc(), 0, false, SrcKey::FuncEntryTag{}}, 3, 7
  );
  ASSERT_TRUE(captureContProfProfile(profData, *refreshFunc()));
  ++expectedCounters[1];
  EXPECT_EQ(expectedCounters, counters());
  EXPECT_EQ(before.size() + 1, numContProfProfileRecords());
  auto const fresh = snapshotContProfProfileRecords();
  auto const replacement = std::find_if(
    fresh.begin(), fresh.end(),
    [&](auto const& record) { return record.header.funcKey == *key; }
  );
  ASSERT_NE(fresh.end(), replacement);
  EXPECT_GT(replacement->header.capturedAtMs, original.header.capturedAtMs);
  std::vector<ContProfProfileTranslation> const expected{
    {ContProfStartKind::FuncEntry, 0, 3, 7},
  };
  EXPECT_EQ(expected, replacement->translations);
  EXPECT_EQ(7, replacement->functionExecutions());

  *profData.transCounterAddr(transId) = profData.counterDefault() - 99;
  EXPECT_FALSE(captureContProfProfile(profData, *refreshFunc()));
  EXPECT_FALSE(retainContProfProfileRecord(original));
  EXPECT_EQ(expectedCounters, counters());
  EXPECT_EQ(fresh, snapshotContProfProfileRecords());

  // The final flush includes the replacement despite unchanged cardinality.
  auto const writesBeforeStop = writes();
  stopContProfCheckpointWriter();
  auto const last = readContProfCheckpointDirectory(directory.path().native());
  ASSERT_TRUE(last);
  EXPECT_EQ(fresh, last->records);
  EXPECT_GT(writes(), writesBeforeStop);
}

}
}
