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

#include "hphp/runtime/vm/jit/cprof-target-profile.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <folly/ScopeGuard.h>
#include <gtest/gtest.h>

#include "hphp/runtime/base/rds.h"
#include "hphp/runtime/base/runtime-option.h"
#include "hphp/runtime/base/static-string-table.h"
#include "hphp/runtime/base/string-data.h"
#include "hphp/runtime/vm/as.h"
#include "hphp/runtime/vm/class.h"
#include "hphp/runtime/vm/func.h"
#include "hphp/runtime/vm/jit/array-access-profile.h"
#include "hphp/runtime/vm/jit/array-iter-profile.h"
#include "hphp/runtime/vm/jit/call-target-profile.h"
#include "hphp/runtime/vm/jit/cls-cns-profile.h"
#include "hphp/runtime/vm/jit/coeffect-fun-param-profile.h"
#include "hphp/runtime/vm/jit/cow-profile.h"
#include "hphp/runtime/vm/jit/cprof-key.h"
#include "hphp/runtime/vm/jit/cprof-serde.h"
#include "hphp/runtime/vm/jit/decref-profile.h"
#include "hphp/runtime/vm/jit/incref-profile.h"
#include "hphp/runtime/vm/jit/is-type-struct-profile.h"
#include "hphp/runtime/vm/jit/meth-profile.h"
#include "hphp/runtime/vm/jit/prof-data-target-profile.h"
#include "hphp/runtime/vm/jit/prof-data.h"
#include "hphp/runtime/vm/jit/switch-profile.h"
#include "hphp/runtime/vm/jit/target-profile.h"
#include "hphp/runtime/vm/jit/type-profile.h"
#include "hphp/runtime/vm/named-entity.h"
#include "hphp/runtime/vm/unit-emitter.h"
#include "hphp/runtime/vm/unit.h"
#include "hphp/util/service-data.h"
#include "hphp/util/sha1.h"

namespace HPHP::jit::cprof {
namespace {

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

template<class T>
struct ScopedTargetProfile {
  ScopedTargetProfile(
    TransID transId,
    Offset bytecodeOffset,
    const StringData* name,
    const T& value,
    size_t extraSize = 0
  )
    : m_key{
        static_cast<T*>(nullptr),
        transId,
        bytecodeOffset,
        name,
      }
    , m_handle{
        rds::bind<T, rds::Mode::Local>(
          rds::Symbol{m_key},
          extraSize
        ).handle()
      }
    , m_allocationSize{
        static_cast<uint32_t>(sizeof(T) + extraSize)
      } {
    new (&rds::handleToRef<T, rds::Mode::Local>(m_handle)) T(value);
  }

  ~ScopedTargetProfile() {
    rds::unbind(rds::Symbol{m_key}, m_handle);
  }

  ScopedTargetProfile(const ScopedTargetProfile&) = delete;
  ScopedTargetProfile& operator=(const ScopedTargetProfile&) = delete;

  std::optional<ContProfTargetProfile> snapshot() const {
    return snapshotContProfTargetProfile(
      m_key,
      m_handle,
      m_allocationSize
    );
  }

private:
  rds::Profile m_key;
  rds::Handle m_handle;
  uint32_t m_allocationSize;
};

void expectDecRefProfile(
  const DecRefProfile& expected,
  const DecRefProfile& actual
) {
  EXPECT_EQ(expected.total, actual.total);
  EXPECT_EQ(expected.refcounted, actual.refcounted);
  EXPECT_EQ(expected.released, actual.released);
  EXPECT_EQ(expected.decremented, actual.decremented);
  EXPECT_EQ(
    expected.arrayOfUncountedReleaseCount,
    actual.arrayOfUncountedReleaseCount
  );
  EXPECT_EQ(expected.datatype, actual.datatype);
}

TEST(ContProfTargetProfile, FallsBackToLocalRdsAndPrefersImportedData) {
  auto const previousProfData = jit::profData();

  ProfData isolatedProfData;
  *rl_profData = &isolatedProfData;
  SCOPE_EXIT { *rl_profData = previousProfData; };

  isolatedProfData.setTargetProfile(
    std::make_unique<ProfDataTargetProfile>(/*liveFallback=*/true)
  );
  auto const importedProfiles = isolatedProfData.targetProfile();
  ASSERT_NE(nullptr, importedProfiles);

  constexpr auto transId = TransID{2'100'000};
  constexpr auto bytecodeOffset = Offset{123};
  constexpr auto profileId = int32_t{2'100'001};
  auto const name = decRefProfileKey(profileId);

  auto const localValue = makeDecRefProfileValue(11, 8, 2, 4, 1, KindOfString);
  ScopedTargetProfile<DecRefProfile> const local{
    transId,
    bytecodeOffset,
    name,
    localValue,
  };

  TransIDSet transIds;
  transIds.insert(transId);
  auto const profile = TargetProfile<DecRefProfile>::deserialize(
    transIds,
    TransKind::Optimize,
    bytecodeOffset,
    name,
    0
  );

  ASSERT_TRUE(profile.optimizing());
  expectDecRefProfile(localValue, profile.data());

  auto const importedValue =
    makeDecRefProfileValue(17, 12, 3, 5, 0, KindOfObject);
  ScopedTargetProfile<DecRefProfile> const importedSource{
    TransID{2'100'004},
    bytecodeOffset,
    name,
    importedValue,
  };
  auto const importedSnapshot = importedSource.snapshot();
  ASSERT_TRUE(importedSnapshot);
  auto const prepared = prepareContProfTargetProfile(*importedSnapshot);
  ASSERT_TRUE(prepared);
  ASSERT_TRUE(installContProfTargetProfile(
    *prepared,
    transId,
    *importedProfiles
  ));
  expectDecRefProfile(importedValue, profile.data());

  constexpr auto importedOnlyTransId = TransID{2'100'002};
  constexpr auto importedOnlyProfileId = int32_t{2'100'003};
  auto const importedOnlyName = decRefProfileKey(importedOnlyProfileId);
  auto const importedOnlyValue =
    makeDecRefProfileValue(23, 15, 4, 7, 1, KindOfVec);
  ScopedTargetProfile<DecRefProfile> const importedOnlySource{
    TransID{2'100'005},
    bytecodeOffset,
    importedOnlyName,
    importedOnlyValue,
  };
  auto const importedOnlySnapshot = importedOnlySource.snapshot();
  ASSERT_TRUE(importedOnlySnapshot);
  auto const importedOnlyPrepared =
    prepareContProfTargetProfile(*importedOnlySnapshot);
  ASSERT_TRUE(importedOnlyPrepared);
  ASSERT_TRUE(installContProfTargetProfile(
    *importedOnlyPrepared,
    importedOnlyTransId,
    *importedProfiles
  ));

  TransIDSet importedOnlyTransIds;
  importedOnlyTransIds.insert(importedOnlyTransId);
  auto const importedOnly = TargetProfile<DecRefProfile>::deserialize(
    importedOnlyTransIds,
    TransKind::Optimize,
    bytecodeOffset,
    importedOnlyName,
    0
  );

  ASSERT_TRUE(importedOnly.optimizing());
  expectDecRefProfile(importedOnlyValue, importedOnly.data());

  // An exhaustive import suppresses the still-populated live RDS slot.
  isolatedProfData.setTargetProfile(std::make_unique<ProfDataTargetProfile>());
  EXPECT_FALSE(profile.optimizing());
}

TEST(ContProfTargetProfile, PreservesRecordedNameDuringPreparation) {
  ScopedTargetProfile<DecRefProfile> const source{
    TransID{2'200'000},
    Offset{123},
    decRefProfileKey(2'200'001),
    makeDecRefProfileValue(11, 8, 2, 4, 1, KindOfString),
  };
  auto snapshot = source.snapshot();
  ASSERT_TRUE(snapshot);
  snapshot->name = "DecRefProfile-02200001";

  auto const prepared = prepareContProfTargetProfile(*snapshot);
  ASSERT_TRUE(prepared);
  ASSERT_NE(nullptr, prepared->name);
  EXPECT_EQ(snapshot->name, prepared->name->toCppString());
}

std::optional<ContProfProfileRecord> roundTripTargetProfiles(
  std::vector<ContProfTargetProfile> profiles
) {
  ContProfProfileRecord record{};
  record.header.funcKey.resolutionUnitPath =
    "hphp/runtime/test/cont-prof-target-profile.php";
  record.header.funcKey.bytecodeUnitHash =
    SHA1{"3333333333333333333333333333333333333333"};
  record.header.funcKey.functionName = "cont_prof_target_profile_test";
  record.header.capturedAtMs = 1'700'000'000'000;

  ContProfProfileTranslation translation{};
  translation.regionLength = 1;
  translation.executionCount = 7;
  translation.targetProfiles = std::move(profiles);
  record.translations.push_back(std::move(translation));

  auto const serialized = serializeContProfProfileRecord(record);
  if (!serialized) return std::nullopt;
  return deserializeContProfProfileRecord(
    folly::ByteRange{serialized->data(), serialized->size()}
  );
}

struct DestroyTargetProfileTestUnit {
  void operator()(Unit* unit) const {
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
};

using TargetProfileTestUnit = std::unique_ptr<Unit, DestroyTargetProfileTestUnit>;

TargetProfileTestUnit makeCallTargetTestUnit(size_t index, bool changed = false) {
  auto const suffix = std::to_string(index);
  auto const hhas = ".function N cont_prof_call_target_28f619d3_" + suffix +
    "() {\n" + (changed ? "Int 42\n" : "Null\n") + "RetC None\n}\n";
  auto const path = "hphp/runtime/test/cont-prof-call-target-" + suffix + ".php";
  auto const emitter = assemble_string(
    hhas.c_str(),
    path.c_str(),
    SHA1{"5555555555555555555555555555555555555555"},
    nullptr,
    RepoOptions::defaults().packageInfo(),
    UnitEmitterAttributes::defaults(),
    false
  );
  if (!emitter || emitter->m_fatalUnit) return nullptr;
  return TargetProfileTestUnit{emitter->create().release()};
}

struct ContProfCallTargetTest : testing::Test {
  void SetUp() override {
    for (size_t i = 0; i < 7; ++i) {
      auto unit = makeCallTargetTestUnit(i);
      ASSERT_NE(nullptr, unit);
      ASSERT_EQ(1, unit->funcs().size());
      unit->merge();
      auto const key = makeContProfFuncKey(*unit->funcs()[0]);
      ASSERT_TRUE(key);
      m_units.emplace(key->resolutionUnitPath, unit.get());
      m_ownedUnits.push_back(std::move(unit));
    }
  }

  std::vector<TargetProfileTestUnit> m_ownedUnits;
  std::map<std::string, Unit*> m_units;
};

struct EmptyCallTargetReader {
  bool initialized;
};

const Func* read_func(EmptyCallTargetReader&) { return nullptr; }
void read_raw(EmptyCallTargetReader&, uint32_t& count) { count = 0; }
void read_raw(EmptyCallTargetReader& reader, bool& initialized) {
  initialized = reader.initialized;
}

TEST(ContProfTargetProfile, SkipsCallTargetWithoutTargets) {
  for (auto const initialized : {false, true}) {
    SCOPED_TRACE(initialized);
    CallTargetProfile value{};
    EmptyCallTargetReader reader{initialized};
    value.deserialize(reader);
    ScopedTargetProfile<CallTargetProfile> const source{
      TransID{2'600'002}, 0, callTargetProfileKey(), value
    };
    EXPECT_FALSE(source.snapshot());
  }
}

TEST_F(ContProfCallTargetTest, RoundTripsCountsAndUntrackedProbability) {
  std::map<std::string, int64_t> countersBefore;
  for (auto const suffix : {"captured", "prepared", "installed"}) {
    auto const key = std::string{"jit.cprof.target.call_target."} + suffix;
    countersBefore.emplace(key, ServiceData::exportCounterByKey(key).value_or(0));
  }
  constexpr auto transId = TransID{2'600'000};
  auto const name = callTargetProfileKey();
  for (auto const numTargets : {size_t{2}, size_t{7}}) {
    SCOPED_TRACE(numTargets);
    CallTargetProfile value{};
    uint32_t total = 0;
    std::vector<std::string> expectedPaths;
    for (size_t i = 0; i < numTargets; ++i) {
      auto const func = m_ownedUnits[i]->funcs()[0];
      auto const count = static_cast<uint32_t>(7 - i);
      for (uint32_t n = 0; n < count; ++n) value.report(func);
      total += count;
      if (i < 6) {
        auto const key = makeContProfFuncKey(*func);
        ASSERT_TRUE(key);
        expectedPaths.push_back(key->resolutionUnitPath);
        if (key->bytecodeUnitPath) {
          expectedPaths.push_back(*key->bytecodeUnitPath);
        }
      }
    }
    ScopedTargetProfile<CallTargetProfile> const source{
      transId, 0, name, value
    };
    auto const snapshot = source.snapshot();
    ASSERT_TRUE(snapshot);
    auto const decoded = roundTripTargetProfiles({*snapshot});
    ASSERT_TRUE(decoded);
    ASSERT_EQ(1, decoded->translations.size());
    auto const& profiles = decoded->translations[0].targetProfiles;
    ASSERT_EQ(1, profiles.size());
    EXPECT_EQ(expectedPaths, contProfTargetProfileUnitPaths(profiles[0]));

    auto const prepared = prepareContProfTargetProfile(profiles[0], m_units);
    ASSERT_TRUE(prepared);
    ProfDataTargetProfile imported;
    ASSERT_TRUE(installContProfTargetProfile(*prepared, transId, imported));
    auto const actual = imported.get<CallTargetProfile>(rds::Profile{
      static_cast<CallTargetProfile*>(nullptr), transId, 0, name
    });
    ASSERT_NE(nullptr, actual);
    EXPECT_EQ(value.toDynamic(), actual->toDynamic());

    auto const choices = actual->choose();
    ASSERT_EQ(std::min(numTargets, size_t{6}), choices.size());
    for (size_t i = 0; i < choices.size(); ++i) {
      EXPECT_EQ(m_ownedUnits[i]->funcs()[0], choices[i].func);
      EXPECT_DOUBLE_EQ(double(7 - i) / total, choices[i].probability);
    }
  }
  for (auto const& [key, before] : countersBefore) {
    EXPECT_EQ(before + 2, ServiceData::exportCounterByKey(key).value_or(0));
  }
}

TEST_F(ContProfCallTargetTest, RejectsUnavailableOrStaleRequiredTargets) {
  // Exercise both an initial hole and a hole before a later valid target.
  for (auto const missing : {size_t{0}, size_t{1}}) {
    SCOPED_TRACE(missing);
    CallTargetProfile value{};
    for (size_t i = 0; i < 3; ++i) {
      for (size_t n = 0; n < 3 - i; ++n) {
        value.report(m_ownedUnits[i]->funcs()[0]);
      }
    }
    std::optional<ContProfTargetProfile> snapshot;
    {
      ScopedTargetProfile<CallTargetProfile> const source{
        TransID{2'600'001}, 0, callTargetProfileKey(), value
      };
      snapshot = source.snapshot();
    }
    ASSERT_TRUE(snapshot);
    ASSERT_TRUE(prepareContProfTargetProfile(*snapshot, m_units));
    auto const paths = contProfTargetProfileUnitPaths(*snapshot);
    ASSERT_EQ(3, paths.size());
    auto const key = makeContProfFuncKey(*m_ownedUnits[missing]->funcs()[0]);
    ASSERT_TRUE(key);
    EXPECT_EQ(key->resolutionUnitPath, paths[missing]);

    auto unavailable = m_units;
    unavailable.erase(key->resolutionUnitPath);
    auto const missingBefore = ServiceData::exportCounterByKey(
      "jit.cprof.target.prepare.skipped.missing_unit").value_or(0);
    EXPECT_FALSE(prepareContProfTargetProfile(*snapshot, unavailable));
    EXPECT_EQ(missingBefore + 1, ServiceData::exportCounterByKey(
      "jit.cprof.target.prepare.skipped.missing_unit").value_or(0));
    EXPECT_FALSE(prepareContProfTargetProfile(*snapshot));
    EXPECT_EQ(missingBefore + 2, ServiceData::exportCounterByKey(
      "jit.cprof.target.prepare.skipped.missing_unit").value_or(0));

    auto replacement = makeCallTargetTestUnit(missing, true);
    ASSERT_NE(nullptr, replacement);
    m_ownedUnits[missing].reset();
    replacement->merge();
    m_units.at(key->resolutionUnitPath) = replacement.get();
    m_ownedUnits[missing] = std::move(replacement);
    auto const replacementKey =
      makeContProfFuncKey(*m_ownedUnits[missing]->funcs()[0]);
    ASSERT_TRUE(replacementKey);
    ASSERT_NE(key->bytecodeUnitHash, replacementKey->bytecodeUnitHash);
    auto const staleBefore = ServiceData::exportCounterByKey(
      "jit.cprof.target.prepare.skipped.unresolved_or_stale").value_or(0);
    EXPECT_FALSE(prepareContProfTargetProfile(*snapshot, m_units));
    EXPECT_EQ(staleBefore + 1, ServiceData::exportCounterByKey(
      "jit.cprof.target.prepare.skipped.unresolved_or_stale").value_or(0));
  }
}

constexpr auto kMethUnitPath = "hphp/runtime/test/cont-prof-meth-profile.php";
constexpr auto kMethHhas = R"HHAS(
.class [interface abstract] ContProfMethInterface_19ab32d5 {
  .method [public abstract] N f() {
    String "Cannot call abstract method ContProfMethInterface_19ab32d5::f()"
    Fatal RuntimeOmitFrame
  }
}
.class ContProfMethBase_19ab32d5 implements (ContProfMethInterface_19ab32d5) {
  .method [public] N f() {
    Int 11
    RetC None
  }
  .method [private] N privateMethod() {
    Null
    RetC None
  }
  .method [protected] N protectedMethod() {
    Null
    RetC None
  }
  .method [public internal] N internalMethod() {
    Null
    RetC None
  }
}
.class ContProfMethChild_19ab32d5 extends ContProfMethBase_19ab32d5 {}
.class ContProfMethOverride_19ab32d5 extends ContProfMethBase_19ab32d5 {
  .method [public] N f() {
    Null
    RetC None
  }
}
.class ContProfMethOther_19ab32d5 implements (ContProfMethInterface_19ab32d5) {
  .method [public] N f() {
    Null
    RetC None
  }
}
.class ContProfMethPrivateBase_19ab32d5 {
  .method [private] N f() {
    Null
    RetC None
  }
}
.class ContProfMethPrivateChild_19ab32d5 extends ContProfMethPrivateBase_19ab32d5 {
  .method [public] N f() {
    Null
    RetC None
  }
}
.class ContProfMethUnrelated_19ab32d5 {
  .method [public] N f() {
    Null
    RetC None
  }
}
)HHAS";

TargetProfileTestUnit makeMethTestUnit(bool changed = false) {
  auto hhas = std::string{kMethHhas};
  if (changed) hhas.replace(hhas.find("Int 11"), 6, "Int 12");
  auto const emitter = assemble_string(
    hhas,
    kMethUnitPath,
    SHA1{"7777777777777777777777777777777777777777"},
    nullptr,
    RepoOptions::defaults().packageInfo(),
    UnitEmitterAttributes::defaults(),
    false
  );
  if (!emitter || emitter->m_fatalUnit) return nullptr;
  return TargetProfileTestUnit{emitter->create().release()};
}

struct ContProfMethTest : testing::Test {
  void SetUp() override {
    m_unit = makeMethTestUnit();
    ASSERT_NE(nullptr, m_unit);
    m_unit->merge();
    m_units.emplace(kMethUnitPath, m_unit.get());
    m_base = Class::lookup(makeStaticString("ContProfMethBase_19ab32d5"));
    m_child = Class::lookup(makeStaticString("ContProfMethChild_19ab32d5"));
    m_override = Class::lookup(makeStaticString("ContProfMethOverride_19ab32d5"));
    ASSERT_NE(nullptr, m_base);
    ASSERT_NE(nullptr, m_child);
    ASSERT_NE(nullptr, m_override);
    m_method = m_base->lookupMethod(makeStaticString("f"));
    ASSERT_NE(nullptr, m_method);
    ASSERT_EQ(m_method, m_child->lookupMethod(makeStaticString("f")));
  }

  TargetProfileTestUnit m_unit;
  std::map<std::string, Unit*> m_units;
  Class* m_base{nullptr};
  Class* m_child{nullptr};
  Class* m_override{nullptr};
  const Func* m_method{nullptr};
};

TEST_F(ContProfMethTest, RestoresSupportedMethodAndReceiverStates) {
  auto const name = makeStaticString("MethProfile-FCallObjMethod");
  for (auto const multipleClasses : {false, true}) {
    SCOPED_TRACE(multipleClasses);
    MethProfile value;
    value.reportMeth(m_base, m_method, nullptr);
    if (multipleClasses) value.reportMeth(m_child, m_method, nullptr);
    ASSERT_EQ(multipleClasses ? nullptr : m_base, value.uniqueClass());
    ASSERT_EQ(m_method, value.uniqueMeth());
    ScopedTargetProfile<MethProfile> const source{
      TransID{2'800'000}, 0, name, value
    };
    auto const snapshot = source.snapshot();
    ASSERT_TRUE(snapshot);
    auto const decoded = roundTripTargetProfiles({*snapshot});
    ASSERT_TRUE(decoded);
    ASSERT_EQ(1, decoded->translations.size());
    auto const& profiles = decoded->translations.front().targetProfiles;
    ASSERT_EQ(1, profiles.size());
    auto const key = makeContProfFuncKey(*m_method);
    ASSERT_TRUE(key);
    std::vector<std::string> expectedPaths{key->resolutionUnitPath};
    if (!multipleClasses) expectedPaths.push_back(kMethUnitPath);
    EXPECT_EQ(expectedPaths, contProfTargetProfileUnitPaths(profiles.front()));

    auto const prepared = prepareContProfTargetProfile(profiles.front(), m_units);
    ASSERT_TRUE(prepared);
    ProfDataTargetProfile imported;
    ASSERT_TRUE(installContProfTargetProfile(
      *prepared, TransID{2'800'001}, imported
    ));
    auto const actual = imported.get<MethProfile>(rds::Profile{
      static_cast<MethProfile*>(nullptr), TransID{2'800'001}, 0, name
    });
    ASSERT_NE(nullptr, actual);
    EXPECT_EQ(multipleClasses ? nullptr : m_base, actual->uniqueClass());
    EXPECT_EQ(m_method, actual->uniqueMeth());
    EXPECT_EQ(nullptr, actual->baseMeth());
    EXPECT_EQ(nullptr, actual->interfaceMeth());
    // A UniqueMeth fallback checks the receiver's slot against this exact Func*.
    EXPECT_EQ(m_method, m_child->getMethod(m_method->methodSlot()));
    EXPECT_NE(m_method, m_override->getMethod(m_method->methodSlot()));
  }
}

TEST_F(ContProfMethTest, SkipsContextDependentMethods) {
  auto const expectRejected = [&](const Class* cls, const Func* method) {
    ASSERT_NE(nullptr, method);
    MethProfile value;
    value.reportMeth(cls, method, nullptr);
    ASSERT_EQ(method, value.uniqueMeth());
    auto const name = makeStaticString("MethProfile-FCallObjMethod");
    ScopedTargetProfile<MethProfile> const source{
      TransID{2'800'002}, 0, name, value
    };
    EXPECT_FALSE(source.snapshot());

    // Preparation independently enforces eligibility for a well-formed key.
    auto const key = makeContProfFuncKey(*method);
    ASSERT_TRUE(key);
    auto const methodPayload = serializeContProfFuncKey(*key);
    ASSERT_TRUE(methodPayload);
    auto const methodSize = methodPayload->size();
    std::vector<uint8_t> payload(sizeof(methodSize));
    std::memcpy(payload.data(), &methodSize, sizeof(methodSize));
    payload.insert(payload.end(), methodPayload->begin(), methodPayload->end());
    ContProfTargetProfile const profile{
      ContProfTargetProfileKind::Meth, 0, name->toCppString(), std::move(payload)
    };
    EXPECT_TRUE(isValidContProfTargetProfile(profile));
    EXPECT_FALSE(prepareContProfTargetProfile(profile, m_units));
  };
  for (auto const methodName : {
    "privateMethod", "protectedMethod", "internalMethod"
  }) {
    SCOPED_TRACE(methodName);
    expectRejected(m_base, m_base->lookupMethod(makeStaticString(methodName)));
  }

  auto const cls =
    Class::lookup(makeStaticString("ContProfMethPrivateChild_19ab32d5"));
  ASSERT_NE(nullptr, cls);
  auto const method = cls->lookupMethod(makeStaticString("f"));
  ASSERT_NE(nullptr, method);
  ASSERT_TRUE(method->hasPrivateAncestor());
  expectRejected(cls, method);
}

TEST_F(ContProfMethTest, SkipsEmptyAndBroaderStates) {
  MethProfile base;
  base.reportMeth(m_base, m_method, nullptr);
  base.reportMeth(m_override, m_override->lookupMethod(makeStaticString("f")),
                  nullptr);
  ASSERT_NE(nullptr, base.baseMeth());
  auto const other = Class::lookup(makeStaticString("ContProfMethOther_19ab32d5"));
  ASSERT_NE(nullptr, other);
  MethProfile interface;
  interface.reportMeth(m_base, m_method, nullptr);
  interface.reportMeth(other, other->lookupMethod(makeStaticString("f")), nullptr);
  ASSERT_NE(nullptr, interface.interfaceMeth());
  auto const unrelated =
    Class::lookup(makeStaticString("ContProfMethUnrelated_19ab32d5"));
  ASSERT_NE(nullptr, unrelated);
  MethProfile invalid{interface};
  invalid.reportMeth(unrelated, unrelated->lookupMethod(makeStaticString("f")),
                     nullptr);
  ASSERT_EQ(nullptr, invalid.interfaceMeth());

  for (auto const& value : {MethProfile{}, base, interface, invalid}) {
    ScopedTargetProfile<MethProfile> const source{
      TransID{2'800'003}, 0, makeStaticString("MethProfile-FCallObjMethod"), value
    };
    EXPECT_FALSE(source.snapshot());
  }
}

TEST_F(ContProfMethTest, SkipsUnavailableOrStaleMethod) {
  MethProfile value;
  value.reportMeth(m_base, m_method, nullptr);
  std::optional<ContProfTargetProfile> snapshot;
  {
    ScopedTargetProfile<MethProfile> const source{
      TransID{2'800'004}, 0, makeStaticString("MethProfile-FCallObjMethod"), value
    };
    snapshot = source.snapshot();
  }
  ASSERT_TRUE(snapshot);
  ASSERT_TRUE(prepareContProfTargetProfile(*snapshot, m_units));
  EXPECT_FALSE(prepareContProfTargetProfile(*snapshot));

  auto replacement = makeMethTestUnit(true);
  ASSERT_NE(nullptr, replacement);
  m_unit.reset();
  replacement->merge();
  m_units.at(kMethUnitPath) = replacement.get();
  m_unit = std::move(replacement);
  EXPECT_FALSE(prepareContProfTargetProfile(*snapshot, m_units));
}

TEST_F(ContProfMethTest, RejectsMalformedMethodPayloads) {
  auto const key = makeContProfFuncKey(*m_method);
  ASSERT_TRUE(key);
  auto const payload = serializeContProfFuncKey(*key);
  ASSERT_TRUE(payload);
  ContProfTargetProfile const legacy{
    ContProfTargetProfileKind::Meth, 0, "MethProfile-FCallObjMethod", *payload
  };

  EXPECT_FALSE(isValidContProfTargetProfile(legacy));
  EXPECT_FALSE(prepareContProfTargetProfile(legacy, m_units));

  MethProfile value;
  value.reportMeth(m_base, m_method, nullptr);
  ScopedTargetProfile<MethProfile> const source{
    TransID{2'800'007}, 0, makeStaticString("MethProfile-FCallObjMethod"), value
  };
  auto const snapshot = source.snapshot();
  ASSERT_TRUE(snapshot);
  auto const methodEnd = sizeof(size_t) + payload->size();
  ASSERT_GT(snapshot->payload.size(), methodEnd);

  // Ending exactly after the framed method is a valid UniqueMeth payload.
  auto withoutReceiver = *snapshot;
  withoutReceiver.payload.resize(methodEnd);
  auto const prepared = prepareContProfTargetProfile(withoutReceiver, m_units);
  ASSERT_TRUE(prepared);
  ASSERT_NE(nullptr, prepared->meth);
  EXPECT_EQ(nullptr, prepared->meth->uniqueClass());
  EXPECT_EQ(m_method, prepared->meth->uniqueMeth());

  for (auto end = methodEnd + 1; end < snapshot->payload.size(); ++end) {
    SCOPED_TRACE(end);
    auto truncated = *snapshot;
    truncated.payload.resize(end);
    EXPECT_FALSE(isValidContProfTargetProfile(truncated));
  }
  auto trailing = *snapshot;
  trailing.payload.push_back(0);
  EXPECT_FALSE(isValidContProfTargetProfile(trailing));
}

constexpr auto kMethAncestorUnitPath =
  "hphp/runtime/test/cont-prof-meth-ancestor.php";
constexpr auto kMethReceiverUnitPath =
  "hphp/runtime/test/cont-prof-meth-receiver.php";
constexpr auto kMethAncestorHhas = R"HHAS(
.class ContProfMethAncestor_19ab32d5 extends ContProfMethBase_19ab32d5 {}
)HHAS";
constexpr auto kMethReceiverHhas = R"HHAS(
.class ContProfMethReceiver_19ab32d5 extends ContProfMethAncestor_19ab32d5 {
  .method [public] N marker() {
    Int 17
    RetC None
  }
}
)HHAS";

TargetProfileTestUnit makeMethHierarchyUnit(
  const char* path,
  const std::string& hhas
) {
  auto const emitter = assemble_string(
    hhas,
    path,
    SHA1{"8888888888888888888888888888888888888888"},
    nullptr,
    RepoOptions::defaults().packageInfo(),
    UnitEmitterAttributes::defaults(),
    false
  );
  if (!emitter || emitter->m_fatalUnit) return nullptr;
  return TargetProfileTestUnit{emitter->create().release()};
}

struct ContProfMethReceiverTest : ContProfMethTest {
  void SetUp() override {
    ContProfMethTest::SetUp();
    ASSERT_FALSE(HasFatalFailure());
    m_ancestor = makeMethHierarchyUnit(kMethAncestorUnitPath, kMethAncestorHhas);
    ASSERT_NE(nullptr, m_ancestor);
    m_ancestor->merge();
    m_receiver = makeMethHierarchyUnit(kMethReceiverUnitPath, kMethReceiverHhas);
    ASSERT_NE(nullptr, m_receiver);
    m_receiver->merge();
    m_units.emplace(kMethAncestorUnitPath, m_ancestor.get());
    m_units.emplace(kMethReceiverUnitPath, m_receiver.get());
    ASSERT_NE(nullptr, receiver());
    ASSERT_EQ(m_method, receiver()->lookupMethod(makeStaticString("f")));
  }

  Class* receiver() const {
    return Class::lookup(makeStaticString("ContProfMethReceiver_19ab32d5"));
  }

  std::optional<ContProfTargetProfile> snapshot() const {
    MethProfile value;
    value.reportMeth(receiver(), m_method, nullptr);
    ScopedTargetProfile<MethProfile> const source{
      TransID{2'800'005}, 0, makeStaticString("MethProfile-FCallObjMethod"), value
    };
    return source.snapshot();
  }

  // Destroy derived classes before their ancestors.
  TargetProfileTestUnit m_ancestor;
  TargetProfileTestUnit m_receiver;
};

TEST_F(ContProfMethReceiverTest, RestoresReceiverAcrossUnrelatedBytecodeChange) {
  auto const profile = snapshot();
  ASSERT_TRUE(profile);
  auto const methodKey = makeContProfFuncKey(*m_method);
  ASSERT_TRUE(methodKey);
  EXPECT_NE(methodKey->resolutionUnitPath, kMethReceiverUnitPath);
  std::vector<std::string> const expectedPaths{
    methodKey->resolutionUnitPath, kMethReceiverUnitPath
  };
  EXPECT_EQ(expectedPaths, contProfTargetProfileUnitPaths(*profile));

  auto const prepared = prepareContProfTargetProfile(*profile, m_units);
  ASSERT_TRUE(prepared);
  ASSERT_NE(nullptr, prepared->meth);
  EXPECT_EQ(receiver(), prepared->meth->uniqueClass());
  EXPECT_EQ(m_method, prepared->meth->uniqueMeth());

  auto unavailable = m_units;
  unavailable.erase(kMethReceiverUnitPath);
  auto const fallbackBefore = ServiceData::exportCounterByKey(
    "jit.cprof.target.meth.receiver_unavailable").value_or(0);
  auto const preparedBefore = ServiceData::exportCounterByKey(
    "jit.cprof.target.meth.prepared").value_or(0);
  auto const missingBefore = ServiceData::exportCounterByKey(
    "jit.cprof.target.prepare.skipped.missing_unit").value_or(0);
  auto const withoutReceiver = prepareContProfTargetProfile(*profile, unavailable);
  ASSERT_TRUE(withoutReceiver);
  ASSERT_NE(nullptr, withoutReceiver->meth);
  EXPECT_EQ(nullptr, withoutReceiver->meth->uniqueClass());
  EXPECT_EQ(m_method, withoutReceiver->meth->uniqueMeth());
  EXPECT_EQ(fallbackBefore + 1, ServiceData::exportCounterByKey(
    "jit.cprof.target.meth.receiver_unavailable").value_or(0));
  EXPECT_EQ(preparedBefore + 1, ServiceData::exportCounterByKey(
    "jit.cprof.target.meth.prepared").value_or(0));
  EXPECT_EQ(missingBefore, ServiceData::exportCounterByKey(
    "jit.cprof.target.prepare.skipped.missing_unit").value_or(0));

  auto const oldReceiverHash = m_receiver->bcSha1();
  auto changedHhas = std::string{kMethReceiverHhas};
  changedHhas.replace(changedHhas.find("Int 17"), 6, "Int 18");
  auto replacement = makeMethHierarchyUnit(kMethReceiverUnitPath, changedHhas);
  ASSERT_NE(nullptr, replacement);
  m_receiver.reset();
  replacement->merge();
  m_receiver = std::move(replacement);
  m_units.at(kMethReceiverUnitPath) = m_receiver.get();
  ASSERT_NE(nullptr, receiver());
  ASSERT_NE(oldReceiverHash, m_receiver->bcSha1());
  ASSERT_EQ(m_method, resolveContProfFunc(*methodKey, *m_unit));

  auto const changedReceiver = prepareContProfTargetProfile(*profile, m_units);
  ASSERT_TRUE(changedReceiver);
  ASSERT_NE(nullptr, changedReceiver->meth);
  EXPECT_EQ(receiver(), changedReceiver->meth->uniqueClass());
  EXPECT_EQ(m_method, changedReceiver->meth->uniqueMeth());
}

TEST_F(ContProfMethReceiverTest, ChangedAncestorDispatchDropsOnlyReceiver) {
  auto const profile = snapshot();
  ASSERT_TRUE(profile);
  auto const receiverHash = m_receiver->bcSha1();
  auto const methodKey = makeContProfFuncKey(*m_method);
  ASSERT_TRUE(methodKey);
  constexpr auto changedAncestorHhas = R"HHAS(
.class ContProfMethAncestor_19ab32d5 extends ContProfMethBase_19ab32d5 {
  .method [public] N f() {
    Int 29
    RetC None
  }
}
)HHAS";
  m_receiver.reset();
  m_ancestor.reset();
  m_ancestor = makeMethHierarchyUnit(kMethAncestorUnitPath, changedAncestorHhas);
  ASSERT_NE(nullptr, m_ancestor);
  m_ancestor->merge();
  m_receiver = makeMethHierarchyUnit(kMethReceiverUnitPath, kMethReceiverHhas);
  ASSERT_NE(nullptr, m_receiver);
  m_receiver->merge();
  m_units.at(kMethAncestorUnitPath) = m_ancestor.get();
  m_units.at(kMethReceiverUnitPath) = m_receiver.get();
  ASSERT_NE(nullptr, receiver());
  EXPECT_EQ(receiverHash, m_receiver->bcSha1());
  EXPECT_EQ(m_method, resolveContProfFunc(*methodKey, *m_unit));
  EXPECT_NE(m_method, receiver()->lookupMethod(makeStaticString("f")));

  auto const prepared = prepareContProfTargetProfile(*profile, m_units);
  ASSERT_TRUE(prepared);
  ASSERT_NE(nullptr, prepared->meth);
  EXPECT_EQ(nullptr, prepared->meth->uniqueClass());
  EXPECT_EQ(m_method, prepared->meth->uniqueMeth());
  EXPECT_NE(m_method, receiver()->getMethod(m_method->methodSlot()));
}

TEST_F(ContProfMethTest, ReducesRestoredReceiverStatesInEitherOrder) {
  auto const prepare = [&](const MethProfile& value) {
    ScopedTargetProfile<MethProfile> const source{
      TransID{2'800'006}, 0, makeStaticString("MethProfile-FCallObjMethod"), value
    };
    auto const profile = source.snapshot();
    return profile ? prepareContProfTargetProfile(*profile, m_units)
                   : std::nullopt;
  };
  MethProfile base;
  base.reportMeth(m_base, m_method, nullptr);
  MethProfile child;
  child.reportMeth(m_child, m_method, nullptr);
  auto const restoredBase = prepare(base);
  auto const restoredChild = prepare(child);
  auto const restoredMethod = prepare(MethProfile::fromUniqueMeth(m_method));
  ASSERT_TRUE(restoredBase && restoredBase->meth);
  ASSERT_TRUE(restoredChild && restoredChild->meth);
  ASSERT_TRUE(restoredMethod && restoredMethod->meth);

  for (auto const other : {
    restoredBase->meth.get(),
    restoredChild->meth.get(),
    restoredMethod->meth.get(),
  }) {
    SCOPED_TRACE(other->toString());
    auto const expectedClass = other == restoredBase->meth.get()
      ? m_base : nullptr;
    for (auto const reverse : {false, true}) {
      SCOPED_TRACE(reverse);
      MethProfile aggregate;
      MethProfile::reduce(aggregate, reverse ? *other : *restoredBase->meth);
      MethProfile::reduce(aggregate, reverse ? *restoredBase->meth : *other);
      EXPECT_EQ(expectedClass, aggregate.uniqueClass());
      EXPECT_EQ(m_method, aggregate.uniqueMeth());
      EXPECT_EQ(nullptr, aggregate.baseMeth());
      EXPECT_EQ(nullptr, aggregate.interfaceMeth());
    }
  }
}

template<class T>
void expectContProfFixedTargetProfile(
  const ContProfTargetProfile& profile,
  ContProfTargetProfileKind kind,
  Offset bytecodeOffset,
  const StringData* name,
  const T& expected,
  TransID transId,
  ProfDataTargetProfile& imported
) {
  EXPECT_EQ(kind, profile.kind);
  EXPECT_EQ(bytecodeOffset, profile.bytecodeOffset);
  EXPECT_EQ(name->toCppString(), profile.name);
  auto const bytes = reinterpret_cast<const uint8_t*>(&expected);
  EXPECT_EQ(
    std::vector<uint8_t>(bytes, bytes + sizeof(T)),
    profile.payload
  );

  auto const prepared = prepareContProfTargetProfile(profile);
  ASSERT_TRUE(prepared);
  ASSERT_TRUE(installContProfTargetProfile(
    *prepared,
    transId,
    imported
  ));

  auto const key = rds::Profile{
    static_cast<T*>(nullptr),
    transId,
    bytecodeOffset,
    name,
  };
  auto const actual = imported.get<T>(key);
  ASSERT_NE(nullptr, actual);
  EXPECT_EQ(expected.toDynamic(), actual->toDynamic());
}

TEST(ContProfTargetProfile, RoundTripsFixedTargetProfileKinds) {
  constexpr auto transId = TransID{2'300'000};

  auto const cowName = makeStaticString("ArrayCOW");
  auto const coeffectName = makeStaticString("CoeffectFunParam");
  // Raw profile names are preserved without a codec-specific allowlist.
  auto const incRefName = makeStaticString("CustomIncRefProfile");
  auto const isTypeStructName = makeStaticString("IsTypeStruct");
  auto const arrayAccessName = makeStaticString("DictAccess");
  auto const clsCnsName = makeStaticString("ClsCnsProfile");

  COWProfile const cow{};
  CoeffectFunParamProfile const coeffect{};
  IncRefProfile const incRef{17, 12, 5};
  IsTypeStructProfile const isTypeStruct{};
  ArrayAccessProfile const arrayAccess{};
  ClsCnsProfile const clsCns{};

  ScopedTargetProfile<COWProfile> const cowSite{
    transId, 0, cowName, cow
  };
  ScopedTargetProfile<CoeffectFunParamProfile> const coeffectSite{
    transId, 0, coeffectName, coeffect
  };
  ScopedTargetProfile<IncRefProfile> const incRefSite{
    transId, 0, incRefName, incRef
  };
  ScopedTargetProfile<IsTypeStructProfile> const isTypeStructSite{
    transId, 0, isTypeStructName, isTypeStruct
  };
  ScopedTargetProfile<ArrayAccessProfile> const arrayAccessSite{
    transId, 0, arrayAccessName, arrayAccess
  };
  ScopedTargetProfile<ClsCnsProfile> const clsCnsSite{
    transId, 0, clsCnsName, clsCns
  };

  auto cowSnapshot = cowSite.snapshot();
  auto coeffectSnapshot = coeffectSite.snapshot();
  auto incRefSnapshot = incRefSite.snapshot();
  auto isTypeStructSnapshot = isTypeStructSite.snapshot();
  auto arrayAccessSnapshot = arrayAccessSite.snapshot();
  auto clsCnsSnapshot = clsCnsSite.snapshot();
  ASSERT_TRUE(cowSnapshot);
  ASSERT_TRUE(coeffectSnapshot);
  ASSERT_TRUE(incRefSnapshot);
  ASSERT_TRUE(isTypeStructSnapshot);
  ASSERT_TRUE(arrayAccessSnapshot);
  ASSERT_TRUE(clsCnsSnapshot);

  std::vector<ContProfTargetProfile> capturedProfiles{
    std::move(*cowSnapshot),
    std::move(*coeffectSnapshot),
    std::move(*incRefSnapshot),
    std::move(*isTypeStructSnapshot),
    std::move(*arrayAccessSnapshot),
    std::move(*clsCnsSnapshot),
  };
  std::sort(
    capturedProfiles.begin(),
    capturedProfiles.end(),
    contProfTargetProfileKeyLess
  );

  auto const decoded = roundTripTargetProfiles(std::move(capturedProfiles));
  ASSERT_TRUE(decoded);
  ASSERT_EQ(1, decoded->translations.size());

  auto const& profiles = decoded->translations[0].targetProfiles;
  ASSERT_EQ(6, profiles.size());

  ProfDataTargetProfile imported;
  expectContProfFixedTargetProfile(
    profiles[0],
    ContProfTargetProfileKind::COW,
    0,
    cowName,
    cow,
    transId,
    imported
  );
  expectContProfFixedTargetProfile(
    profiles[1],
    ContProfTargetProfileKind::CoeffectFunParam,
    0,
    coeffectName,
    coeffect,
    transId,
    imported
  );
  expectContProfFixedTargetProfile(
    profiles[2],
    ContProfTargetProfileKind::IncRef,
    0,
    incRefName,
    incRef,
    transId,
    imported
  );
  expectContProfFixedTargetProfile(
    profiles[3],
    ContProfTargetProfileKind::IsTypeStruct,
    0,
    isTypeStructName,
    isTypeStruct,
    transId,
    imported
  );
  expectContProfFixedTargetProfile(
    profiles[4],
    ContProfTargetProfileKind::ArrayAccess,
    0,
    arrayAccessName,
    arrayAccess,
    transId,
    imported
  );
  expectContProfFixedTargetProfile(
    profiles[5],
    ContProfTargetProfileKind::ClsCns,
    0,
    clsCnsName,
    clsCns,
    transId,
    imported
  );
}

TEST(ContProfTargetProfile, RoundTripsTypeBearingTargetProfileKinds) {
  constexpr auto transId = TransID{2'500'000};
  auto const typeName = makeStaticString("CustomTypeProfile");
  auto const arrayIterName = makeStaticString("CustomArrayIterProfile");

  TypeProfile typeProfile{};
  typeProfile.type = Type::cns(makeStaticString("cprof value"));
  typeProfile.count = 17;

  auto const keyTypes = ArrayKeyTypes::Ints() | ArrayKeyTypes::StaticStrs();
  auto const arrayIterProfile = ArrayIterProfile{
    .m_key_types = keyTypes,
    .m_value_type = Type::cns(int64_t{7}),
  };

  ScopedTargetProfile<TypeProfile> const typeSite{
    transId, 0, typeName, typeProfile
  };
  ScopedTargetProfile<ArrayIterProfile> const arrayIterSite{
    transId, 0, arrayIterName, arrayIterProfile
  };

  auto typeSnapshot = typeSite.snapshot();
  auto arrayIterSnapshot = arrayIterSite.snapshot();
  ASSERT_TRUE(typeSnapshot);
  ASSERT_TRUE(arrayIterSnapshot);

  std::vector<ContProfTargetProfile> capturedProfiles{
    std::move(*typeSnapshot),
    std::move(*arrayIterSnapshot),
  };
  std::sort(
    capturedProfiles.begin(),
    capturedProfiles.end(),
    contProfTargetProfileKeyLess
  );

  auto const decoded = roundTripTargetProfiles(std::move(capturedProfiles));
  ASSERT_TRUE(decoded);
  ASSERT_EQ(1, decoded->translations.size());

  auto const& profiles = decoded->translations[0].targetProfiles;
  ASSERT_EQ(2, profiles.size());
  EXPECT_EQ(ContProfTargetProfileKind::Type, profiles[0].kind);
  EXPECT_EQ(typeName->toCppString(), profiles[0].name);
  EXPECT_EQ(ContProfTargetProfileKind::ArrayIter, profiles[1].kind);
  EXPECT_EQ(arrayIterName->toCppString(), profiles[1].name);

  ProfDataTargetProfile imported;
  for (auto const& profile : profiles) {
    auto const prepared = prepareContProfTargetProfile(profile);
    ASSERT_TRUE(prepared);
    ASSERT_TRUE(installContProfTargetProfile(*prepared, transId, imported));
  }

  auto const importedType = imported.get<TypeProfile>(rds::Profile{
    static_cast<TypeProfile*>(nullptr),
    transId,
    0,
    typeName,
  });
  ASSERT_NE(nullptr, importedType);
  EXPECT_EQ(17, importedType->count);
  EXPECT_TRUE(importedType->type == TStaticStr);

  auto const importedArrayIter =
    imported.get<ArrayIterProfile>(rds::Profile{
      static_cast<ArrayIterProfile*>(nullptr),
      transId,
      0,
      arrayIterName,
    });
  ASSERT_NE(nullptr, importedArrayIter);
  auto const importedResult = importedArrayIter->result();
  EXPECT_TRUE(importedResult.key_types == keyTypes);
  EXPECT_TRUE(importedResult.value_type == TInt);
}

TEST(ContProfTargetProfile, RejectsInvalidTypeBearingTargetProfiles) {
  constexpr auto transId = TransID{2'500'000};
  auto const typeName = makeStaticString("TypeProfile");
  auto const arrayIterName = makeStaticString("ArrayIterProfile");
  ScopedTargetProfile<TypeProfile> const typeSite{
    transId, 0, typeName, TypeProfile{}
  };
  ScopedTargetProfile<ArrayIterProfile> const arrayIterSite{
    transId, 0, arrayIterName, ArrayIterProfile{}
  };
  auto const typeSnapshot = typeSite.snapshot();
  auto const arrayIterSnapshot = arrayIterSite.snapshot();
  ASSERT_TRUE(typeSnapshot);
  ASSERT_TRUE(arrayIterSnapshot);

  for (auto const& profile : {*typeSnapshot, *arrayIterSnapshot}) {
    ASSERT_TRUE(prepareContProfTargetProfile(profile));

    auto truncated = profile;
    truncated.payload.pop_back();
    EXPECT_FALSE(prepareContProfTargetProfile(truncated));

    auto trailing = profile;
    trailing.payload.push_back(0);
    EXPECT_FALSE(prepareContProfTargetProfile(trailing));
  }

  auto invalidType = *typeSnapshot;
  std::fill_n(
    invalidType.payload.begin(), sizeof(Type::bits_t), uint8_t{0xff}
  );
  EXPECT_FALSE(prepareContProfTargetProfile(invalidType));

  auto invalidArrayIter = *arrayIterSnapshot;
  invalidArrayIter.payload[0] = 0x10;
  EXPECT_FALSE(prepareContProfTargetProfile(invalidArrayIter));

  TypeProfile nonCellTypeProfile{};
  nonCellTypeProfile.type = Type{Type::kMem, PtrLocation::Frame};
  ScopedTargetProfile<TypeProfile> const nonCellTypeSite{
    TransID{2'500'001}, 0, typeName, nonCellTypeProfile
  };
  auto nonCellType = nonCellTypeSite.snapshot();
  ASSERT_TRUE(nonCellType);
  EXPECT_FALSE(prepareContProfTargetProfile(*nonCellType));

  auto const uninitArrayIterProfile = ArrayIterProfile{
    .m_key_types = ArrayKeyTypes::Ints(),
    .m_value_type = TUninit,
  };
  ScopedTargetProfile<ArrayIterProfile> const uninitArrayIterSite{
    TransID{2'500'002}, 0, arrayIterName, uninitArrayIterProfile
  };
  auto uninitArrayIter = uninitArrayIterSite.snapshot();
  ASSERT_TRUE(uninitArrayIter);
  EXPECT_FALSE(prepareContProfTargetProfile(*uninitArrayIter));
}

TEST(ContProfTargetProfile, RoundTripsSwitchTargetProfile) {
  constexpr auto transId = TransID{2'400'000};
  for (auto const& counts : {
    std::vector<uint32_t>{17},
    std::vector<uint32_t>{17, 9, 3},
  }) {
    SCOPED_TRACE(counts.size());
    auto const key = rds::Profile{
      static_cast<SwitchProfile*>(nullptr),
      transId,
      0,
      makeStaticString("SwitchProfile"),
    };
    auto const extraSize = SwitchProfile::extraSize(
      static_cast<int>(counts.size())
    );
    auto const allocationSize =
      static_cast<uint32_t>(sizeof(SwitchProfile) + extraSize);
    auto const handle = rds::bind<SwitchProfile, rds::Mode::Local>(
      rds::Symbol{key}, extraSize
    ).handle();
    SCOPE_EXIT { rds::unbind(rds::Symbol{key}, handle); };
    auto& local = rds::handleToRef<SwitchProfile, rds::Mode::Local>(handle);
    std::copy(counts.begin(), counts.end(), local.cases());

    auto snapshot = snapshotContProfTargetProfile(key, handle, allocationSize);
    ASSERT_TRUE(snapshot);
    auto const decoded = roundTripTargetProfiles({std::move(*snapshot)});
    ASSERT_TRUE(decoded);
    ASSERT_EQ(1, decoded->translations.size());
    ASSERT_EQ(1, decoded->translations[0].targetProfiles.size());

    auto const& profile = decoded->translations[0].targetProfiles[0];
    EXPECT_EQ(ContProfTargetProfileKind::Switch, profile.kind);
    EXPECT_EQ(0, profile.bytecodeOffset);
    EXPECT_EQ("SwitchProfile", profile.name);
    ASSERT_EQ(allocationSize, profile.payload.size());

    auto const prepared = prepareContProfTargetProfile(profile);
    ASSERT_TRUE(prepared);
    ProfDataTargetProfile imported;
    ASSERT_TRUE(installContProfTargetProfile(*prepared, transId, imported));
    auto const actual = imported.get<SwitchProfile>(key);
    ASSERT_NE(nullptr, actual);
    auto const restored = std::vector<uint32_t>{
      actual->cases(), actual->cases() + counts.size()
    };
    EXPECT_EQ(counts, restored);
  }
}

TEST(ContProfTargetProfile, RejectsInvalidSwitchPayloadSizes) {
  ContProfTargetProfile profile{
    ContProfTargetProfileKind::Switch,
    0,
    "SwitchProfile",
    {},
  };
  EXPECT_FALSE(prepareContProfTargetProfile(profile));

  profile.payload.resize(sizeof(uint32_t) + 1);
  EXPECT_FALSE(prepareContProfTargetProfile(profile));
}

TEST(ContProfTargetProfile, RejectsInvalidRawTargetProfiles) {
  constexpr auto transId = TransID{2'500'000};

  ScopedTargetProfile<COWProfile> const source{
    transId,
    0,
    makeStaticString("ArrayCOW"),
    COWProfile{},
  };
  ScopedTargetProfile<COWProfile> const negativeOffset{
    transId,
    Offset{-1},
    makeStaticString("ArrayCOW"),
    COWProfile{},
  };
  EXPECT_FALSE(negativeOffset.snapshot());

  auto const snapshot = source.snapshot();
  ASSERT_TRUE(snapshot);

  auto invalid = *snapshot;
  invalid.bytecodeOffset = -1;
  EXPECT_FALSE(prepareContProfTargetProfile(invalid));

  invalid = *snapshot;
  invalid.payload.pop_back();
  EXPECT_FALSE(prepareContProfTargetProfile(invalid));
}

}
}
