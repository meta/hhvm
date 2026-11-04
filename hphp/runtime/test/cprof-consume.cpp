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

#include "hphp/runtime/vm/jit/cprof-consume.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <folly/ScopeGuard.h>
#include <folly/testing/TestUtil.h>
#include <gtest/gtest.h>

#include "hphp/runtime/base/autoload-handler.h"
#include "hphp/runtime/base/execution-context.h"
#include "hphp/runtime/base/program-functions.h"
#include "hphp/runtime/base/rds.h"
#include "hphp/runtime/base/request-info.h"
#include "hphp/runtime/base/runtime-option.h"
#include "hphp/runtime/base/static-string-table.h"
#include "hphp/runtime/base/unit-cache.h"
#include "hphp/runtime/vm/as.h"
#include "hphp/runtime/vm/class.h"
#include "hphp/runtime/vm/constant.h"
#include "hphp/runtime/vm/func.h"
#include "hphp/runtime/vm/hhbc.h"
#include "hphp/runtime/vm/jit/call-target-profile.h"
#include "hphp/runtime/vm/jit/cprof-checkpoint.h"
#include "hphp/runtime/vm/jit/cprof-controller.h"
#include "hphp/runtime/vm/jit/cprof-key.h"
#include "hphp/runtime/vm/jit/cprof-reader.h"
#include "hphp/runtime/vm/jit/cprof-record.h"
#include "hphp/runtime/vm/jit/cprof-target-profile.h"
#include "hphp/runtime/vm/jit/decref-profile.h"
#include "hphp/runtime/vm/jit/mcgen-translate.h"
#include "hphp/runtime/vm/jit/meth-profile.h"
#include "hphp/runtime/vm/jit/prof-data-target-profile.h"
#include "hphp/runtime/vm/jit/prof-data.h"
#include "hphp/runtime/vm/jit/region-selection.h"
#include "hphp/runtime/vm/jit/tc.h"
#include "hphp/runtime/vm/named-entity.h"
#include "hphp/runtime/vm/repo-autoload-map-builder.h"
#include "hphp/runtime/vm/repo-file.h"
#include "hphp/runtime/vm/repo-global-data.h"
#include "hphp/runtime/vm/unit-emitter.h"
#include "hphp/runtime/vm/unit.h"
#include "hphp/util/configs/jit.h"
#include "hphp/util/configs/repo.h"
#include "hphp/util/configs/server.h"
#include "hphp/util/sha1.h"

namespace HPHP::jit::cprof {
namespace {

constexpr auto kUnitPath = "hphp/runtime/test/cont-prof-consume-test.php";
constexpr auto kHhas = R"HHAS(
.function N cont_prof_consume_alpha_52d9c47b(N $x) {
  .declvars $local;
  Null
  PopC
  Null
  RetC None
}
.function N cont_prof_consume_beta_52d9c47b() {
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
    unit->destroy();
  }
};

std::unique_ptr<Unit, DestroyTestUnit> makeCalleeUnit() {
  constexpr auto hhas = R"HHAS(
.function N cont_prof_consume_callee_c2a07e49() {
  Null
  RetC None
}
.class ContProfConsumeMethBase_c2a07e49 {
  .method [public] N f() {
    Null
    RetC None
  }
}
.class ContProfConsumeMethChild_c2a07e49 extends ContProfConsumeMethBase_c2a07e49 {
  .method [public] N f() {
    Int 1
    RetC None
  }
}
)HHAS";
  auto const emitter = assemble_string(
    hhas,
    "hphp/runtime/test/cont-prof-consume-callee.php",
    SHA1{"6666666666666666666666666666666666666666"},
    nullptr,
    RepoOptions::defaults().packageInfo(),
    UnitEmitterAttributes::defaults(),
    false
  );
  if (!emitter || emitter->m_fatalUnit) return nullptr;
  return std::unique_ptr<Unit, DestroyTestUnit>{emitter->create().release()};
}

template<class T>
std::optional<ContProfTargetProfile> snapshotTestTargetProfile(
  const T& value,
  const StringData* name,
  Offset bytecodeOffset = 0
) {
  auto const key = rds::Profile{
    static_cast<T*>(nullptr), TransID{2'700'000}, bytecodeOffset, name
  };
  auto const handle = rds::bind<T, rds::Mode::Local>(rds::Symbol{key}).handle();
  SCOPE_EXIT { rds::unbind(rds::Symbol{key}, handle); };
  new (&rds::handleToRef<T, rds::Mode::Local>(handle)) T(value);
  return snapshotContProfTargetProfile(key, handle, sizeof(T));
}

struct ContProfConsumeTest : testing::Test {
  void SetUp() override {
    auto const path = std::string{kUnitPath} + "." +
      testing::UnitTest::GetInstance()->current_test_info()->name();
    auto const emitter = assemble_string(
      kHhas,
      path.c_str(),
      SHA1{"3333333333333333333333333333333333333333"},
      nullptr,
      RepoOptions::defaults().packageInfo(),
      UnitEmitterAttributes::defaults(),
      false
    );
    ASSERT_NE(nullptr, emitter);
    ASSERT_FALSE(emitter->m_fatalUnit);
    m_unit.reset(emitter->create().release());
    ASSERT_NE(nullptr, m_unit);
    ASSERT_EQ(2, m_unit->funcs().size());
    m_unit->merge();

    m_alphaFunc = m_unit->funcs()[0];
    m_betaFunc = m_unit->funcs()[1];
    auto const alphaKey = makeContProfFuncKey(*m_alphaFunc);
    auto const betaKey = makeContProfFuncKey(*m_betaFunc);
    ASSERT_TRUE(alphaKey);
    ASSERT_TRUE(betaKey);
    m_alpha.header = {*alphaKey, 1};
    m_alpha.translations = {{ContProfStartKind::FuncEntry, 1, 2, 7}};
    m_beta.header = {*betaKey, 1};
    m_beta.translations = {{ContProfStartKind::FuncEntry, 0, 2, 13}};
    m_units.emplace(alphaKey->resolutionUnitPath, m_unit.get());
    m_units.emplace(betaKey->resolutionUnitPath, m_unit.get());
  }

  void checkReuse(double probability, bool captureEnabled);

  std::unique_ptr<Unit, DestroyTestUnit> m_unit;
  Func* m_alphaFunc{nullptr};
  Func* m_betaFunc{nullptr};
  ContProfProfileRecord m_alpha;
  ContProfProfileRecord m_beta;
  std::map<std::string, Unit*> m_units;
};

TEST_F(ContProfConsumeTest, StaleHotterRecordDoesNotConsumeSlot) {
  auto stale = m_alpha;
  stale.header.funcKey.bytecodeUnitHash = SHA1{uint64_t{2}};
  stale.translations.front().executionCount = 100;

  auto const prepared = prepareContProfStartupCandidates(
    {stale, m_alpha}, m_units, 1
  );

  ASSERT_EQ(1, prepared.candidates.size());
  auto const& candidate = prepared.candidates.front();
  EXPECT_EQ(m_alphaFunc, candidate.func);
  ASSERT_EQ(1, candidate.translations.size());
  EXPECT_EQ(7, candidate.translations.front().executionCount);
}

TEST_F(ContProfConsumeTest, PreparesBytecodeTranslation) {
  auto const popOffset = instrLen(m_alphaFunc->at(0));
  auto const midOffset = popOffset + instrLen(m_alphaFunc->at(popOffset));
  auto const midStart = SrcKey{m_alphaFunc, midOffset, ResumeMode::None};
  // Include the synthetic entry, Null, and PopC to reach midStart.
  m_alpha.translations.front().regionLength = 3;
  m_alpha.translations.front().localTypeGuards = {
    {0, Type::kUncountedInit, DataTypeCountnessInit},
  };
  m_alpha.translations.front().localPostConditions = {
    {0, true, Type::kInt | Type::kInitNull},
    {1, false, Type::kStr | Type::kInt},
  };
  m_alpha.translations.push_back({
    ContProfStartKind::Bytecode,
    static_cast<uint32_t>(midOffset),
    2,
    11,
    {{1, Type::kArrLike, DataTypeGeneric}},
    {0},
    {{0, true, Type::kCell}},
  });

  auto const prepared = prepareContProfStartupCandidates(
    {m_alpha}, m_units, 1
  );

  ASSERT_EQ(1, prepared.candidates.size());
  auto const& candidate = prepared.candidates.front();
  EXPECT_EQ(m_alphaFunc, candidate.func);
  ASSERT_EQ(2, candidate.translations.size());

  auto const& entry = candidate.translations[0];
  EXPECT_TRUE(entry.start.funcEntry());
  EXPECT_EQ(7, entry.executionCount);
  ASSERT_NE(nullptr, entry.region);
  ASSERT_EQ(1, entry.region->blocks().size());
  GuardedLocations const expectedEntryGuards{
    {Location::Local{0}, TUncountedInit, DataTypeCountnessInit},
  };
  EXPECT_EQ(expectedEntryGuards, entry.region->entry()->typePreConditions());
  EXPECT_TRUE(entry.incoming.empty());
  TypedLocations const expectedChanged{
    {Location::Local{0}, TInt | TInitNull},
  };
  TypedLocations const expectedRefined{
    {Location::Local{1}, TStr | TInt},
  };
  EXPECT_EQ(expectedChanged, entry.postConditions.changed);
  EXPECT_EQ(expectedRefined, entry.postConditions.refined);

  auto const& translation = candidate.translations[1];
  EXPECT_EQ(midStart, translation.start);
  EXPECT_FALSE(translation.start.anyFuncEntry());
  EXPECT_EQ(11, translation.executionCount);
  ASSERT_NE(nullptr, translation.region);
  ASSERT_EQ(1, translation.region->blocks().size());
  EXPECT_EQ(midStart, translation.region->start());
  auto const block = translation.region->entry();
  EXPECT_EQ(2, block->length());
  EXPECT_EQ(SBInvOffset{0}, block->initialSpOffset());
  GuardedLocations const expectedBytecodeGuards{
    {Location::Local{1}, TArrLike, DataTypeGeneric},
  };
  EXPECT_EQ(expectedBytecodeGuards, block->typePreConditions());
  std::vector<uint32_t> const expectedIncoming{0};
  EXPECT_EQ(expectedIncoming, translation.incoming);
  TypedLocations const expectedUnknownChanged{
    {Location::Local{0}, TCell},
  };
  EXPECT_EQ(expectedUnknownChanged, translation.postConditions.changed);
  EXPECT_TRUE(translation.postConditions.refined.empty());
}

TEST_F(ContProfConsumeTest, IncompatibleEntryDoesNotConsumeSlot) {
  m_beta.translations.front().startKind =
    ContProfStartKind::NamedParamsFuncEntry;

  auto const prepared = prepareContProfStartupCandidates(
    {m_beta, m_alpha}, m_units, 1
  );

  ASSERT_EQ(1, prepared.candidates.size());
  EXPECT_EQ(m_alphaFunc, prepared.candidates.front().func);
  EXPECT_EQ(2, prepared.recordsResolved);
}

TEST_F(ContProfConsumeTest, CapSelectsHottestCompatibleFunction) {
  auto const prepared = prepareContProfStartupCandidates(
    {m_alpha, m_beta}, m_units, 1
  );

  ASSERT_EQ(1, prepared.candidates.size());
  EXPECT_EQ(m_betaFunc, prepared.candidates.front().func);
  EXPECT_EQ(1, prepared.recordsResolved);
}

TEST_F(ContProfConsumeTest, EqualHotnessUsesKeyOrder) {
  m_beta.translations.front().executionCount = 7;

  auto const prepared = prepareContProfStartupCandidates(
    {m_beta, m_alpha}, m_units, 1
  );

  ASSERT_EQ(1, prepared.candidates.size());
  EXPECT_EQ(m_alphaFunc, prepared.candidates.front().func);
}

TEST_F(ContProfConsumeTest, ZeroCapSkipsPreparation) {
  auto const prepared = prepareContProfStartupCandidates(
    {m_alpha, m_beta}, m_units, 0
  );

  EXPECT_TRUE(prepared.candidates.empty());
  EXPECT_EQ(0, prepared.recordsResolved);
}

TEST_F(ContProfConsumeTest, InstallsAllProfilesBeforeOptimization) {
  DecRefProfile const alphaValue{7, 5, 2, 3, 0, KindOfString};
  DecRefProfile const betaValue{13, 9, 4, 5, 0, KindOfDict};
  auto const name = decRefProfileKey(0);
  auto const alphaProfile = snapshotTestTargetProfile(alphaValue, name);
  auto const betaProfile = snapshotTestTargetProfile(betaValue, name);
  ASSERT_TRUE(alphaProfile);
  ASSERT_TRUE(betaProfile);
  m_alpha.translations.front().targetProfiles = {*alphaProfile};
  m_beta.translations.front().targetProfiles = {*betaProfile};
  auto const prepared = prepareContProfStartupCandidates(
    {m_alpha, m_beta}, m_units, 2
  );
  ASSERT_EQ(2, prepared.candidates.size());

  auto const previousProfData = jit::profData();
  auto const createdProfData = globalProfData() == nullptr;
  if (createdProfData) {
    auto const oldPGO = std::exchange(Cfg::Jit::PGO, true);
    SCOPE_EXIT { Cfg::Jit::PGO = oldPGO; };
    processInitProfData();
  }
  SCOPE_EXIT {
    if (createdProfData) discardProfData();
    *rl_profData = previousProfData;
  };
  requestInitProfData();
  auto const profData = jit::profData();
  ASSERT_NE(nullptr, profData);
  auto const hadTargetProfiles = profData->targetProfile() != nullptr;
  SCOPE_EXIT {
    profData->cleanupFunc(m_alphaFunc, {}, {});
    profData->cleanupFunc(m_betaFunc, {}, {});
    if (!hadTargetProfiles) profData->setTargetProfile(nullptr);
  };

  auto const installed = installContProfStartupCandidates(
    *profData, prepared.candidates
  );
  ASSERT_EQ(2, installed.size());
  EXPECT_EQ(m_betaFunc, installed[0]->func);
  EXPECT_EQ(m_alphaFunc, installed[1]->func);
  auto const targetProfiles = profData->targetProfile();
  ASSERT_NE(nullptr, targetProfiles);

  std::vector<TransID> transIds;
  for (auto const candidate : installed) {
    auto const funcId = candidate->func->getFuncId();
    EXPECT_TRUE(profData->profiling(funcId));
    EXPECT_FALSE(profData->optimized(funcId));
    auto const ids = profData->funcProfTransIDs(funcId);
    ASSERT_EQ(1, ids.size());
    transIds.push_back(ids.front());
    auto const record = profData->transRec(ids.front());
    ASSERT_NE(nullptr, record);
    EXPECT_EQ(candidate->translations.front().start, record->srcKey());
    EXPECT_EQ(candidate->translations.front().executionCount,
              profData->transCounter(ids.front()));

    auto const target = targetProfiles->get<DecRefProfile>(rds::Profile{
      static_cast<DecRefProfile*>(nullptr), ids.front(), 0, name
    });
    ASSERT_NE(nullptr, target);
    auto const& expected = candidate->func == m_alphaFunc
      ? alphaValue : betaValue;
    EXPECT_EQ(expected.toDynamic(), target->toDynamic());
  }
  EXPECT_NE(transIds[0], transIds[1]);

  auto const numTransRecs = profData->numTransRecs();
  EXPECT_TRUE(installContProfStartupCandidates(
    *profData, prepared.candidates
  ).empty());
  EXPECT_EQ(numTransRecs, profData->numTransRecs());
  for (size_t i = 0; i < installed.size(); ++i) {
    TransIDVec const expected{transIds[i]};
    EXPECT_EQ(expected,
              profData->funcProfTransIDs(installed[i]->func->getFuncId()));
    EXPECT_FALSE(profData->optimized(installed[i]->func->getFuncId()));
  }
}

void ContProfConsumeTest::checkReuse(double probability, bool captureEnabled) {
  DecRefProfile const value{13, 9, 4, 5, 0, KindOfDict};
  auto const name = decRefProfileKey(0);
  auto const target = snapshotTestTargetProfile(value, name);
  ASSERT_TRUE(target);
  m_beta.translations.front().targetProfiles = {*target};

  auto stale = m_alpha;
  stale.header.funcKey.bytecodeUnitHash = SHA1{uint64_t{2}};
  stale.translations.front().executionCount = 100;
  auto invalid = m_alpha;
  invalid.translations.front().regionLength = 0;
  invalid.translations.front().executionCount = 50;
  std::vector<ContProfProfileRecord> records{stale, invalid, m_beta};
  std::sort(records.begin(), records.end(), [](auto const& a, auto const& b) {
    return a.header.funcKey < b.header.funcKey;
  });
  auto const prepared = prepareContProfStartupCandidates(records, m_units, 1);
  ASSERT_EQ(1, prepared.candidates.size());
  ASSERT_EQ(m_betaFunc, prepared.candidates.front().func);

  folly::test::TemporaryDirectory directory{"cont-prof-refresh"};
  auto const oldServerMode = std::exchange(Cfg::Server::Mode, true);
  auto const oldCaptureEnabled =
    std::exchange(Cfg::Jit::ContProfCaptureEnabled, captureEnabled);
  auto const oldProbability =
    std::exchange(Cfg::Jit::ContProfReuseProbability, probability);
  auto const oldInterval = std::exchange(
    Cfg::Jit::ContProfCheckpointIntervalSeconds, uint32_t{3600}
  );
  auto const oldDirectory = std::exchange(
    Cfg::Jit::ContProfCheckpointDirectory, directory.path().native()
  );
  SCOPE_EXIT {
    Cfg::Server::Mode = oldServerMode;
    Cfg::Jit::ContProfCaptureEnabled = oldCaptureEnabled;
    Cfg::Jit::ContProfReuseProbability = oldProbability;
    Cfg::Jit::ContProfCheckpointIntervalSeconds = oldInterval;
    Cfg::Jit::ContProfCheckpointDirectory = oldDirectory;
  };

  auto const previousProfData = jit::profData();
  auto const createdProfData = globalProfData() == nullptr;
  if (createdProfData) {
    auto const oldPGO = std::exchange(Cfg::Jit::PGO, true);
    SCOPE_EXIT { Cfg::Jit::PGO = oldPGO; };
    processInitProfData();
  }
  SCOPE_EXIT {
    if (createdProfData) discardProfData();
    *rl_profData = previousProfData;
  };
  requestInitProfData();
  auto const profData = jit::profData();
  ASSERT_NE(nullptr, profData);
  auto const hadTargetProfiles = profData->targetProfile() != nullptr;
  SCOPE_EXIT {
    profData->cleanupFunc(m_alphaFunc, {}, {});
    profData->cleanupFunc(m_betaFunc, {}, {});
    if (!hadTargetProfiles) profData->setTargetProfile(nullptr);
  };

  startContProfCheckpointWriter();
  SCOPE_EXIT { stopContProfCheckpointWriter(); };
  ASSERT_EQ(captureEnabled, contProfActive());

  auto const beforeBudget = profData->profilingBCSize();
  auto const beforeTranslations = profData->numTransRecs();
  auto const beforeTargets = ServiceData::exportCounterByKey(
    "jit.cprof.target.install.attempts"
  ).value_or(0);
  auto const beforeRefresh = ServiceData::exportCounterByKey(
    "jit.cprof.startup.refresh_selected"
  ).value_or(0);

  auto const installed = installContProfStartupCandidates(
    *profData, prepared.candidates, records
  );

  auto const refresh = captureEnabled && probability == 0.0;
  EXPECT_EQ(refresh ? 0 : 1, installed.size());
  EXPECT_EQ(beforeBudget + (refresh ? 0 : m_betaFunc->bclen()),
            profData->profilingBCSize());
  EXPECT_EQ(beforeTranslations + (refresh ? 0 : 1), profData->numTransRecs());
  EXPECT_EQ(beforeTargets + (refresh ? 0 : 1),
    ServiceData::exportCounterByKey(
      "jit.cprof.target.install.attempts"
    ).value_or(0)
  );
  EXPECT_EQ(beforeRefresh + (refresh ? 1 : 0),
    ServiceData::exportCounterByKey(
      "jit.cprof.startup.refresh_selected"
    ).value_or(0)
  );
  EXPECT_FALSE(profData->profiling(m_alphaFunc));
  EXPECT_TRUE(profData->funcProfTransIDs(m_alphaFunc->getFuncId()).empty());
  EXPECT_FALSE(profData->optimized(m_betaFunc->getFuncId()));
  EXPECT_EQ(!refresh, profData->profiling(m_betaFunc));
  auto const ids = profData->funcProfTransIDs(m_betaFunc->getFuncId());
  if (refresh) {
    EXPECT_TRUE(ids.empty());
  } else {
    ASSERT_EQ(1, ids.size());
    EXPECT_EQ(13, profData->transCounter(ids.front()));
    ASSERT_NE(nullptr, profData->targetProfile());
    auto const restored = profData->targetProfile()->get<DecRefProfile>(
      rds::Profile{static_cast<DecRefProfile*>(nullptr), ids.front(), 0, name}
    );
    ASSERT_NE(nullptr, restored);
    EXPECT_EQ(value.toDynamic(), restored->toDynamic());
  }

  std::vector<ContProfProfileRecord> retained;
  for (auto const& record : snapshotContProfProfileRecords()) {
    if (record.header.funcKey.resolutionUnitPath ==
        m_beta.header.funcKey.resolutionUnitPath) {
      retained.push_back(record);
    }
  }
  auto const expected = captureEnabled
    ? std::vector<ContProfProfileRecord>{m_beta}
    : std::vector<ContProfProfileRecord>{};
  EXPECT_EQ(expected, retained);
}

TEST_F(ContProfConsumeTest, ZeroReuseProbabilityRetainsWithoutInstallingProfiles) {
  checkReuse(0.0, true);
}

TEST_F(ContProfConsumeTest, FullReuseProbabilityRetainsAndInstallsProfiles) {
  checkReuse(1.0, true);
}

TEST_F(ContProfConsumeTest, InactiveCaptureIgnoresReuseProbability) {
  checkReuse(0.0, false);
}

TEST_F(ContProfConsumeTest, RejectsUnrelatedProfDataWithoutInstalling) {
  auto const prepared = prepareContProfStartupCandidates(
    {m_alpha, m_beta}, m_units, 2
  );
  ASSERT_EQ(2, prepared.candidates.size());
  ProfData unrelated;

  EXPECT_TRUE(installContProfStartupCandidates(
    unrelated, prepared.candidates
  ).empty());

  EXPECT_EQ(0, unrelated.numTransRecs());
  EXPECT_EQ(nullptr, unrelated.targetProfile());
  for (auto const func : {m_alphaFunc, m_betaFunc}) {
    EXPECT_FALSE(unrelated.profiling(func));
    EXPECT_TRUE(unrelated.funcProfTransIDs(func->getFuncId()).empty());
  }
}

enum class ConstantDependency {
  Direct,
  CallTarget,
};

void optimizeBeforePersistentConstantUnitLoads(ConstantDependency dependency) {
  folly::test::TemporaryDirectory directory{"cont-prof-constant"};
  auto const root = directory.path().native() + "/";
  auto const callerPath = root + "caller.php";
  auto const calleePath = root + "callee.php";
  auto const constantPath = root + "constant.php";
  auto const name = makeStaticString("CONT_PROF_CONSUME_CONSTANT_83D6E9A1");
  auto const oldSourceRoot = std::exchange(Cfg::Server::SourceRoot, root);
  auto const oldRepoAuthoritative =
    std::exchange(Cfg::Repo::Authoritative, true);
  auto const oldPGO = std::exchange(Cfg::Jit::PGO, true);
  SCOPE_EXIT {
    Cfg::Server::SourceRoot = oldSourceRoot;
    Cfg::Repo::Authoritative = oldRepoAuthoritative;
    Cfg::Jit::PGO = oldPGO;
  };

  constexpr auto directHhas = R"HHAS(
.function N cont_prof_consume_constant_caller_83d6e9a1() {
  CnsE "CONT_PROF_CONSUME_CONSTANT_83D6E9A1"
  RetC None
}
)HHAS";
  constexpr auto callHhas = R"HHAS(
.function N cont_prof_consume_constant_caller_83d6e9a1() {
  NullUninit
  NullUninit
  ResolveFunc "cont_prof_consume_constant_callee_83d6e9a1"
  FCallFunc <> 0 1 "" "" $ - ""
  RetC None
}
)HHAS";
  auto const throughCallTarget = dependency == ConstantDependency::CallTarget;
  auto const emitter = assemble_string(
    throughCallTarget ? callHhas : directHhas,
    callerPath.c_str(),
    SHA1{"8888888888888888888888888888888888888888"},
    nullptr,
    RepoOptions::defaults().packageInfo(),
    UnitEmitterAttributes::defaults(),
    false
  );
  ASSERT_NE(nullptr, emitter);
  ASSERT_FALSE(emitter->m_fatalUnit);
  emitter->m_sn = 0;
  constexpr auto calleeHhas = R"HHAS(
.function N cont_prof_consume_constant_callee_83d6e9a1() {
  CnsE "CONT_PROF_CONSUME_CONSTANT_83D6E9A1"
  RetC None
}
)HHAS";
  auto const calleeEmitter = assemble_string(
    calleeHhas,
    calleePath.c_str(),
    SHA1{"7777777777777777777777777777777777777777"},
    nullptr,
    RepoOptions::defaults().packageInfo(),
    UnitEmitterAttributes::defaults(),
    false
  );
  ASSERT_NE(nullptr, calleeEmitter);
  ASSERT_FALSE(calleeEmitter->m_fatalUnit);
  calleeEmitter->m_sn = 2;
  auto const constantEmitter = assemble_string(
    "",
    constantPath.c_str(),
    SHA1{"9999999999999999999999999999999999999999"},
    nullptr,
    RepoOptions::defaults().packageInfo(),
    UnitEmitterAttributes::defaults(),
    false
  );
  ASSERT_NE(nullptr, constantEmitter);
  ASSERT_FALSE(constantEmitter->m_fatalUnit);
  constantEmitter->m_sn = 1;
  Constant constant;
  constant.name = name;
  constant.val = make_tv<KindOfInt64>(42);
  constant.attrs = AttrPersistent;
  constantEmitter->addConstant(constant);

  auto const repoPath = (directory.path() / "test.hhbc").native();
  {
    RepoFileBuilder builder{repoPath};
    RepoAutoloadMapBuilder autoload;
    for (auto const ue : {emitter.get(), constantEmitter.get()}) {
      autoload.addUnit(*ue);
      builder.add(*ue);
    }
    if (throughCallTarget) {
      autoload.addUnit(*calleeEmitter);
      builder.add(*calleeEmitter);
    }
    builder.finish(
      RepoGlobalData{}, autoload, RepoOptions::defaults().packageInfo()
    );
  }
  RepoFile::init(repoPath);
  SCOPE_EXIT { RepoFile::destroy(); };
  RepoFile::loadGlobalTables();
  AutoloadHandler::s_instance->requestShutdown();
  AutoloadHandler::s_instance->requestInit();
  ASSERT_NE(nullptr, AutoloadHandler::s_instance->getAutoloadMap());
  ASSERT_EQ(throughCallTarget ? 3 : 2, RepoFile::numUnits());
  auto const constantSymbols =
    RepoFile::findUnitSymbols(makeStaticString(constantPath));
  ASSERT_NE(nullptr, constantSymbols);
  ASSERT_EQ(1, constantSymbols->size());
  EXPECT_EQ(name, constantSymbols->front().first);
  EXPECT_EQ(RepoSymbolType::CONSTANT, constantSymbols->front().second);

  auto const unit = lookupUnit(
    makeStaticString(callerPath), "", nullptr, nullptr, false
  );
  ASSERT_NE(nullptr, unit);
  unit->merge();
  ASSERT_EQ(1, unit->funcs().size());
  auto const func = unit->funcs().front();
  ASSERT_FALSE(rds::isHandleBound(lookupCnsHandle(name)));

  auto const key = makeContProfFuncKey(*func);
  ASSERT_TRUE(key);
  ContProfProfileRecord record;
  record.header = {*key, 1};
  record.translations = {{ContProfStartKind::FuncEntry, 0, 3, 7}};
  Func* callee{nullptr};
  if (throughCallTarget) {
    auto const calleeUnit = lookupUnit(
      makeStaticString(calleePath), "", nullptr, nullptr, false
    );
    ASSERT_NE(nullptr, calleeUnit);
    calleeUnit->merge();
    ASSERT_EQ(1, calleeUnit->funcs().size());
    callee = calleeUnit->funcs().front();
    auto const calleeKey = makeContProfFuncKey(*callee);
    ASSERT_TRUE(calleeKey);

    Offset callOffset = 0;
    for (int i = 0; i < 3; ++i) callOffset += instrLen(func->at(callOffset));
    ASSERT_EQ(Op::FCallFunc, peek_op(func->at(callOffset)));
    CallTargetProfile target{};
    target.report(callee);
    auto const snapshot = snapshotTestTargetProfile(
      target, callTargetProfileKey(), callOffset
    );
    ASSERT_TRUE(snapshot);
    // End this profile block at the call, before the nonempty-stack return.
    record.translations.front().regionLength = 5;
    record.translations.front().targetProfiles = {*snapshot};
  }
  ASSERT_FALSE(rds::isHandleBound(lookupCnsHandle(name)));
  auto const loadedBefore = snapshotContProfLoadedUnits();
  EXPECT_NE(loadedBefore.end(), std::find(
    loadedBefore.begin(), loadedBefore.end(), "caller.php"
  ));
  EXPECT_EQ(loadedBefore.end(), std::find(
    loadedBefore.begin(), loadedBefore.end(), "constant.php"
  ));

  std::vector<std::string> savedUnits{"caller.php", "constant.php"};
  if (throughCallTarget) savedUnits.push_back("callee.php");
  std::sort(savedUnits.begin(), savedUnits.end());
  auto const checkpointPath = root + "jit.cprof";
  ASSERT_TRUE(writeContProfCheckpointFile(checkpointPath, {record}, savedUnits));
  auto const input = readContProfCheckpointDirectory(directory.path().native());
  ASSERT_TRUE(input);
  EXPECT_EQ(1, input->filesRead);
  EXPECT_EQ(std::vector<ContProfProfileRecord>{record}, input->records);
  EXPECT_EQ(savedUnits, input->unitPaths);
  ASSERT_FALSE(rds::isHandleBound(lookupCnsHandle(name)));

  auto const prepared = prepareContProfStartupCandidates(
    input->records, 1, input->unitPaths
  );
  ASSERT_EQ(1, prepared.candidates.size());
  ASSERT_NE(nullptr, Constant::lookupPersistent(name));
  auto const loadedAfter = snapshotContProfLoadedUnits();
  auto const provider = std::find(
    loadedAfter.begin(), loadedAfter.end(), "constant.php"
  );
  EXPECT_NE(loadedAfter.end(), provider);
  if (throughCallTarget) {
    auto const& targets = prepared.candidates.front().translations.front()
      .targetProfiles;
    ASSERT_EQ(1, targets.size());
    ASSERT_NE(nullptr, targets.front().callTarget);
    auto const choices = targets.front().callTarget->choose();
    ASSERT_EQ(1, choices.size());
    EXPECT_EQ(callee, choices.front().func);
  }

  auto const previousProfData = jit::profData();
  auto const createdProfData = globalProfData() == nullptr;
  if (createdProfData) processInitProfData();
  SCOPE_EXIT {
    if (createdProfData) discardProfData();
    *rl_profData = previousProfData;
  };
  requestInitProfData();
  auto const profData = jit::profData();
  ASSERT_NE(nullptr, profData);
  auto const hadTargetProfiles = profData->targetProfile() != nullptr;
  SCOPE_EXIT {
    profData->cleanupFunc(func, {}, {});
    if (callee) profData->cleanupFunc(callee, {}, {});
    if (!hadTargetProfiles) profData->setTargetProfile(nullptr);
  };

  auto const installed = installContProfStartupCandidates(
    *profData, prepared.candidates
  );
  ASSERT_EQ(1, installed.size());
  if (callee) EXPECT_TRUE(profData->funcProfTransIDs(callee->getFuncId()).empty());
  {
    VMProtect protect;
    LeaseHolder lease{func, TransKind::Optimize, true};
    ASSERT_TRUE(lease);
    profData->setOptimized(func->getFuncId());
    mcgen::optimizeFunc(func);
  }
  auto const srcRec = tc::findSrcRec(
    installed.front()->translations.front().start
  );
  ASSERT_NE(nullptr, srcRec);
  ASSERT_NE(0, srcRec->numTrans());
  auto const handle = lookupCnsHandle(name);
  ASSERT_FALSE(rds::isHandleBound(handle) && rds::isNormalHandle(handle));

  ASSERT_TRUE(RID().getJit());
  ASSERT_TRUE(rds::local::tcCheck());
  for (int i = 0; i < 2; ++i) {
    auto const result = Variant::attach(g_context->invokeFuncFew(
      func, nullptr, 0, nullptr, nullptr, RuntimeCoeffects::fixme(), false
    ));
    ASSERT_TRUE(result.isInteger());
    EXPECT_EQ(42, result.toInt64());
    auto const value = Constant::lookup(name);
    ASSERT_EQ(KindOfInt64, value.m_type);
    EXPECT_EQ(42, value.m_data.num);
    EXPECT_NE(nullptr, Constant::lookupPersistent(name));
  }
}

void runConstantDependencyTest(ConstantDependency dependency) {
  auto const oldDeathTestStyle = GTEST_FLAG_GET(death_test_style);
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  SCOPE_EXIT { GTEST_FLAG_SET(death_test_style, oldDeathTestStyle); };
  // The repo autoload map is process-global and can only be installed once.
  ASSERT_EXIT({
    std::thread{[&] {
      HphpSessionAndThread session{Treadmill::SessionKind::UnitTests};
      optimizeBeforePersistentConstantUnitLoads(dependency);
    }}.join();
    auto const result = testing::UnitTest::GetInstance()
      ->current_test_info()->result();
    for (int i = 0; i < result->total_part_count(); ++i) {
      auto const& part = result->GetTestPartResult(i);
      if (!part.failed()) continue;
      std::fprintf(
        stderr, "%s:%d\n%s\n",
        part.file_name() ? part.file_name() : "unknown",
        part.line_number(), part.message()
      );
    }
    std::fflush(stderr);
    std::_Exit(testing::Test::HasFailure() ? 1 : 0);
  }, testing::ExitedWithCode(0), "");
}

TEST(ContProfConsume, OptimizationBeforePersistentConstantUnitLoads) {
  runConstantDependencyTest(ConstantDependency::Direct);
}

TEST(ContProfConsume, PreparesConstantFromCallTargetWithoutCalleeRecord) {
  runConstantDependencyTest(ConstantDependency::CallTarget);
}

TEST_F(ContProfConsumeTest, PreparesCallTargetWithoutCalleeRecord) {
  auto calleeUnit = makeCalleeUnit();
  ASSERT_NE(nullptr, calleeUnit);
  ASSERT_EQ(1, calleeUnit->funcs().size());
  calleeUnit->merge();
  auto const callee = calleeUnit->funcs()[0];
  auto const calleeKey = makeContProfFuncKey(*callee);
  ASSERT_TRUE(calleeKey);
  ASSERT_NE(m_alpha.header.funcKey.resolutionUnitPath,
            calleeKey->resolutionUnitPath);
  m_units.emplace(calleeKey->resolutionUnitPath, calleeUnit.get());

  CallTargetProfile value{};
  value.report(callee);
  auto const snapshot = snapshotTestTargetProfile(value, callTargetProfileKey());
  ASSERT_TRUE(snapshot);
  std::vector<std::string> const expectedPaths{calleeKey->resolutionUnitPath};
  EXPECT_EQ(expectedPaths, contProfTargetProfileUnitPaths(*snapshot));
  m_alpha.translations.front().targetProfiles = {*snapshot};

  auto const prepared = prepareContProfStartupCandidates({m_alpha}, m_units, 1);
  ASSERT_EQ(1, prepared.candidates.size());
  auto const& candidate = prepared.candidates.front();
  EXPECT_EQ(m_alphaFunc, candidate.func);
  ASSERT_EQ(1, candidate.translations.size());
  auto const& translation = candidate.translations.front();
  EXPECT_EQ(7, translation.executionCount);
  ASSERT_EQ(1, translation.targetProfiles.size());
  auto const& target = translation.targetProfiles.front().callTarget;
  ASSERT_TRUE(target);
  auto const choices = target->choose();
  ASSERT_EQ(1, choices.size());
  EXPECT_EQ(callee, choices.front().func);
  EXPECT_DOUBLE_EQ(1.0, choices.front().probability);
}

TEST_F(ContProfConsumeTest, UnavailableCallTargetPreservesCallerAndOtherSites) {
  auto calleeUnit = makeCalleeUnit();
  ASSERT_NE(nullptr, calleeUnit);
  ASSERT_EQ(1, calleeUnit->funcs().size());
  calleeUnit->merge();
  CallTargetProfile value{};
  value.report(calleeUnit->funcs()[0]);
  auto const callTarget = snapshotTestTargetProfile(value, callTargetProfileKey());
  ASSERT_TRUE(callTarget);
  DecRefProfile const decRef{7, 5, 2, 3, 0, KindOfString};
  auto const otherSite = snapshotTestTargetProfile(decRef, decRefProfileKey(0));
  ASSERT_TRUE(otherSite);
  m_alpha.translations.front().targetProfiles = {*otherSite, *callTarget};

  // The callee unit is intentionally absent from the available-unit map.
  auto const prepared = prepareContProfStartupCandidates({m_alpha}, m_units, 1);
  ASSERT_EQ(1, prepared.candidates.size());
  auto const& candidate = prepared.candidates.front();
  EXPECT_EQ(m_alphaFunc, candidate.func);
  ASSERT_EQ(1, candidate.translations.size());
  auto const& translation = candidate.translations.front();
  EXPECT_EQ(SrcKey(m_alphaFunc, 1, false, SrcKey::FuncEntryTag{}),
            translation.start);
  EXPECT_EQ(7, translation.executionCount);
  ASSERT_NE(nullptr, translation.region);
  EXPECT_EQ(2, translation.region->entry()->length());
  ASSERT_EQ(1, translation.targetProfiles.size());
  EXPECT_EQ(ContProfTargetProfileKind::DecRef,
            translation.targetProfiles.front().kind);
  EXPECT_EQ(otherSite->payload, translation.targetProfiles.front().payload);
}

TEST_F(ContProfConsumeTest, DropsConflictingMethSitesAndKeepsAgreement) {
  auto const popOffset = instrLen(m_alphaFunc->at(0));
  auto const midOffset = popOffset + instrLen(m_alphaFunc->at(popOffset));
  // Cover the synthetic entry, Null, PopC, and the Null at the shared site.
  m_alpha.translations.front().regionLength = 4;
  auto calleeUnit = makeCalleeUnit();
  ASSERT_NE(nullptr, calleeUnit);
  calleeUnit->merge();
  auto const base =
    Class::lookup(makeStaticString("ContProfConsumeMethBase_c2a07e49"));
  auto const child =
    Class::lookup(makeStaticString("ContProfConsumeMethChild_c2a07e49"));
  ASSERT_NE(nullptr, base);
  ASSERT_NE(nullptr, child);
  auto const method = base->lookupMethod(makeStaticString("f"));
  auto const otherMethod = child->lookupMethod(makeStaticString("f"));
  ASSERT_NE(nullptr, method);
  ASSERT_NE(nullptr, otherMethod);
  ASSERT_NE(method, otherMethod);
  auto const key = makeContProfFuncKey(*method);
  ASSERT_TRUE(key);
  m_units.emplace(key->resolutionUnitPath, calleeUnit.get());

  MethProfile first;
  first.reportMeth(base, method, nullptr);
  MethProfile second;
  second.reportMeth(child, otherMethod, nullptr);
  auto const name = makeStaticString("MethProfile-FCallObjMethod");
  auto const firstSite = snapshotTestTargetProfile(first, name, midOffset);
  auto const secondSite = snapshotTestTargetProfile(second, name, midOffset);
  auto const otherName = makeStaticString("MethProfile-Other");
  auto const otherMethSite = snapshotTestTargetProfile(first, otherName, midOffset);
  DecRefProfile const decRef{7, 5, 2, 3, 0, KindOfString};
  auto const decRefSite =
    snapshotTestTargetProfile(decRef, decRefProfileKey(0), midOffset);
  ASSERT_TRUE(firstSite);
  ASSERT_TRUE(secondSite);
  ASSERT_TRUE(otherMethSite);
  ASSERT_TRUE(decRefSite);

  for (auto const conflict : {false, true}) {
    SCOPED_TRACE(conflict);
    auto record = m_alpha;
    record.translations.front().targetProfiles = {
      *decRefSite, *firstSite, *otherMethSite
    };
    ContProfProfileTranslation bytecode{
      ContProfStartKind::Bytecode, static_cast<uint32_t>(midOffset), 2, 11
    };
    bytecode.targetProfiles = {*decRefSite, conflict ? *secondSite : *firstSite};
    record.translations.push_back(std::move(bytecode));

    auto const prepared = prepareContProfStartupCandidates({record}, m_units, 1);
    ASSERT_EQ(1, prepared.candidates.size());
    auto const& translations = prepared.candidates.front().translations;
    ASSERT_EQ(2, translations.size());
    EXPECT_EQ(7, translations[0].executionCount);
    EXPECT_EQ(11, translations[1].executionCount);
    ASSERT_EQ(conflict ? 2 : 3, translations[0].targetProfiles.size());
    ASSERT_EQ(conflict ? 1 : 2, translations[1].targetProfiles.size());
    for (auto const& translation : translations) {
      EXPECT_EQ(decRefSite->payload, translation.targetProfiles.front().payload);
    }
    auto const& retainedOther = translations[0].targetProfiles.back();
    EXPECT_EQ(otherName, retainedOther.name);
    ASSERT_NE(nullptr, retainedOther.meth);
    EXPECT_EQ(method, retainedOther.meth->uniqueMeth());
    if (!conflict) {
      MethProfile aggregate;
      for (auto const& translation : translations) {
        auto const& target = translation.targetProfiles[1].meth;
        ASSERT_NE(nullptr, target);
        MethProfile::reduce(aggregate, *target);
      }
      EXPECT_EQ(method, aggregate.uniqueMeth());
      EXPECT_EQ(nullptr, aggregate.baseMeth());
      EXPECT_EQ(nullptr, aggregate.interfaceMeth());
    }
  }

  auto unavailable = m_units;
  unavailable.erase(key->resolutionUnitPath);
  m_alpha.translations.front().targetProfiles = {*decRefSite, *firstSite};
  auto const prepared = prepareContProfStartupCandidates({m_alpha}, unavailable, 1);
  ASSERT_EQ(1, prepared.candidates.size());
  auto const& translations = prepared.candidates.front().translations;
  ASSERT_EQ(1, translations.size());
  EXPECT_EQ(7, translations.front().executionCount);
  ASSERT_EQ(1, translations.front().targetProfiles.size());
  EXPECT_EQ(decRefSite->payload, translations.front().targetProfiles.front().payload);
}

TEST(ContProfConsume, StartupActivation) {
  auto const oldServerMode = std::exchange(Cfg::Server::Mode, true);
  auto const oldRepoAuthoritative =
    std::exchange(Cfg::Repo::Authoritative, true);
  auto const oldMaxFunctions =
    std::exchange(Cfg::Jit::ContProfStartupMaxFunctions, 1);
  auto const oldDirectory = std::exchange(
    Cfg::Jit::ContProfCheckpointDirectory,
    std::string{"/tmp/cont-prof-startup-activation"}
  );
  auto const oldJitEnabled = std::exchange(Cfg::Jit::Enabled, true);
  auto const oldPGO = std::exchange(Cfg::Jit::PGO, true);
  auto const oldRetranslateAllRequest =
    std::exchange(Cfg::Jit::RetranslateAllRequest, 0u);
  auto const oldRetranslateAllSeconds =
    std::exchange(Cfg::Jit::RetranslateAllSeconds, 1u);
  auto const oldSerdesMode =
    std::exchange(RuntimeOption::EvalJitSerdesMode, JitSerdesMode::Off);
  auto const oldDisabledByVSDebug =
    std::exchange(Cfg::Jit::DisabledByVSDebug, false);
  SCOPE_EXIT {
    Cfg::Server::Mode = oldServerMode;
    Cfg::Repo::Authoritative = oldRepoAuthoritative;
    Cfg::Jit::ContProfStartupMaxFunctions = oldMaxFunctions;
    Cfg::Jit::ContProfCheckpointDirectory = oldDirectory;
    Cfg::Jit::Enabled = oldJitEnabled;
    Cfg::Jit::PGO = oldPGO;
    Cfg::Jit::RetranslateAllRequest = oldRetranslateAllRequest;
    Cfg::Jit::RetranslateAllSeconds = oldRetranslateAllSeconds;
    RuntimeOption::EvalJitSerdesMode = oldSerdesMode;
    Cfg::Jit::DisabledByVSDebug = oldDisabledByVSDebug;
  };

  EXPECT_TRUE(contProfStartupActive());

  Cfg::Server::Mode = false;
  EXPECT_FALSE(contProfStartupActive());
  Cfg::Server::Mode = true;

  Cfg::Repo::Authoritative = false;
  EXPECT_FALSE(contProfStartupActive());
  Cfg::Repo::Authoritative = true;

  Cfg::Jit::ContProfStartupMaxFunctions = 0;
  EXPECT_FALSE(contProfStartupActive());
  Cfg::Jit::ContProfStartupMaxFunctions = 1;

  Cfg::Jit::ContProfCheckpointDirectory.clear();
  EXPECT_FALSE(contProfStartupActive());
  Cfg::Jit::ContProfCheckpointDirectory = "/tmp/cont-prof-startup-activation";

  Cfg::Jit::Enabled = false;
  EXPECT_FALSE(contProfStartupActive());
  Cfg::Jit::Enabled = true;

  Cfg::Jit::PGO = false;
  EXPECT_FALSE(contProfStartupActive());
  Cfg::Jit::PGO = true;

  Cfg::Jit::RetranslateAllRequest = 1;
  EXPECT_FALSE(contProfStartupActive());
  Cfg::Jit::RetranslateAllRequest = 0;

  RuntimeOption::EvalJitSerdesMode = JitSerdesMode::Deserialize;
  EXPECT_FALSE(contProfStartupActive());
  RuntimeOption::EvalJitSerdesMode = JitSerdesMode::Off;

  EXPECT_TRUE(contProfStartupActive());
}

}
}
