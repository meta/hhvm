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

#include "hphp/runtime/vm/jit/cprof-reader.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include <folly/testing/TestUtil.h>
#include <gtest/gtest.h>

#include "hphp/runtime/base/runtime-option.h"
#include "hphp/runtime/ext/extension-registry.h"
#include "hphp/runtime/vm/as.h"
#include "hphp/runtime/vm/func.h"
#include "hphp/runtime/vm/hhbc.h"
#include "hphp/runtime/vm/jit/cprof-checkpoint.h"
#include "hphp/runtime/vm/jit/mcgen-translate.h"
#include "hphp/runtime/vm/unit-emitter.h"
#include "hphp/runtime/vm/unit.h"
#include "hphp/util/sha1.h"

namespace HPHP::jit::cprof {
namespace {

template<class T>
void appendTargetProfilePayload(std::vector<uint8_t>& payload, T value) {
  static_assert(std::is_unsigned_v<T>);

  auto const bytes = reinterpret_cast<const uint8_t*>(&value);
  payload.insert(payload.end(), bytes, bytes + sizeof(value));
}

ContProfTargetProfile makeDecRefTargetProfile(Offset bytecodeOffset) {
  std::vector<uint8_t> payload;
  payload.reserve(5 * sizeof(uint32_t) + sizeof(uint8_t));
  appendTargetProfilePayload(payload, uint32_t{1});
  appendTargetProfilePayload(payload, uint32_t{0});
  appendTargetProfilePayload(payload, uint32_t{0});
  appendTargetProfilePayload(payload, uint32_t{0});
  appendTargetProfilePayload(payload, uint32_t{0});
  appendTargetProfilePayload(payload, static_cast<uint8_t>(KindOfString));

  return {
    ContProfTargetProfileKind::DecRef,
    static_cast<int32_t>(bytecodeOffset),
    "DecRefProfile--1",
    std::move(payload),
  };
}

ContProfTargetProfile makeSwitchTargetProfile(
  Offset bytecodeOffset,
  size_t numCases
) {
  return {
    ContProfTargetProfileKind::Switch,
    static_cast<int32_t>(bytecodeOffset),
    "SwitchProfile",
    std::vector<uint8_t>(numCases * sizeof(uint32_t)),
  };
}

constexpr auto kUnitPath = "hphp/runtime/test/cont-prof-reader-test.php";

constexpr auto kHhas = R"HHAS(
.function N cont_prof_reader_test_962aeb3f() {
  Null
  RetC None
}
)HHAS";

constexpr auto kBytecodeUnitPath =
  "hphp/runtime/test/cont-prof-reader-bytecode-test.php";

constexpr auto kBytecodeHhas = R"HHAS(
.function N cont_prof_reader_bytecode_test_61f82d9a(named N $x = DV) {
  .declvars $local;
main:
  Int 42
  PopC
  True
  JmpZ done
  Null
  PopC
done:
  Int 0
  Switch Unbounded 0 <case0 case1 case2>
case0:
  Null
  RetC None
case1:
  Null
  RetC None
case2:
  Null
  RetC None
DV:
  Null
  PopL $x
  Enter main
}
)HHAS";

constexpr auto kMemoHhas = R"HHAS(
.function N cont_prof_reader_memo_get_81c7e9d2() {
  .ismemoizewrapper;
  MemoGet miss L:0+0
  RetC None
miss:
  Null
  MemoSet L:0+0
  RetC None
}
.function N cont_prof_reader_memo_get_eager_81c7e9d2() isAsync {
  .ismemoizewrapper;
  MemoGetEager miss suspended L:0+0
  RetC None
miss:
  Null
  MemoSetEager L:0+0
  RetC None
suspended:
  RetCSuspended
}
)HHAS";

ContProfProfileRecord makeRecord(
  std::string name,
  uint8_t identity,
  uint64_t capturedAtMs,
  uint64_t executionCount,
  uint32_t regionLength = 1
) {
  ContProfProfileRecord record{};
  record.header.funcKey.resolutionUnitPath = "src/" + name + ".php";
  record.header.funcKey.bytecodeUnitHash =
    SHA1{static_cast<uint64_t>(identity)};
  record.header.funcKey.functionName = std::move(name);
  record.header.capturedAtMs = capturedAtMs;
  record.translations = {
    {
      ContProfStartKind::FuncEntry,
      0,
      regionLength,
      executionCount,
    },
  };
  return record;
}

std::unique_ptr<Unit> makeTestUnit() {
  auto const emitter = assemble_string(
    kHhas,
    kUnitPath,
    SHA1{"3333333333333333333333333333333333333333"},
    nullptr,
    RepoOptions::defaults().packageInfo(),
    UnitEmitterAttributes::defaults(),
    false
  );
  if (!emitter || emitter->m_fatalUnit) return nullptr;
  return emitter->create();
}

std::unique_ptr<Unit> makeBytecodeTestUnit() {
  auto const emitter = assemble_string(
    kBytecodeHhas,
    kBytecodeUnitPath,
    SHA1{"4444444444444444444444444444444444444444"},
    nullptr,
    RepoOptions::defaults().packageInfo(),
    UnitEmitterAttributes::defaults(),
    false
  );
  if (!emitter || emitter->m_fatalUnit) return nullptr;
  return emitter->create();
}

std::unique_ptr<Unit> makeMemoTestUnit() {
  auto const emitter = assemble_string(
    kMemoHhas,
    "hphp/runtime/test/cont-prof-reader-memo-test.php",
    SHA1{"5555555555555555555555555555555555555555"},
    nullptr,
    RepoOptions::defaults().packageInfo(),
    UnitEmitterAttributes::defaults(),
    false
  );
  if (!emitter || emitter->m_fatalUnit) return nullptr;
  return emitter->create();
}

ContProfProfileRecord makeBytecodeRecord(
  const ContProfFuncKey& key,
  Offset offset,
  uint32_t regionLength
) {
  ContProfProfileRecord record{};
  record.header.funcKey = key;
  record.header.capturedAtMs = 100;
  record.translations = {
    {
      ContProfStartKind::FuncEntry,
      0,
      1,
      7,
    },
    {
      ContProfStartKind::Bytecode,
      static_cast<uint32_t>(offset),
      regionLength,
      11,
    },
  };
  return record;
}

void expectMemoStart(const Func& func, Offset offset, int expectedDepth) {
  auto const start = SrcKey{&func, offset, ResumeMode::None};
  auto const stackOffset = mcgen::offsetAtLocation(start);
  ASSERT_TRUE(stackOffset);
  EXPECT_EQ(SBInvOffset{expectedDepth}, *stackOffset);

  auto const key = makeContProfFuncKey(func);
  ASSERT_TRUE(key);
  auto const record = makeBytecodeRecord(*key, offset, 1);
  ASSERT_TRUE(isValidContProfProfileRecord(record));
  EXPECT_EQ(
    expectedDepth == 0,
    isContProfProfileRecordCompatible(record, func)
  );
}

bool writeTextFile(const std::string& path, const std::string& contents) {
  std::ofstream output{path, std::ios::binary};
  output.write(contents.data(), contents.size());
  return output.good();
}

TEST(ContProfReader, ReadsEmptyDirectory) {
  folly::test::TemporaryDirectory directory{"hhvm_cont_prof_reader"};

  auto const result =
    readContProfCheckpointDirectory(directory.path().native());
  ASSERT_TRUE(result);
  EXPECT_EQ(0, result->filesRead);
  EXPECT_EQ(0, result->recordsDecoded);
  EXPECT_TRUE(result->records.empty());
  EXPECT_TRUE(result->unitPaths.empty());
}

TEST(ContProfReader, RejectsInvalidDirectory) {
  folly::test::TemporaryDirectory directory{"hhvm_cont_prof_reader"};
  auto const path = (directory.path() / "missing").native();

  EXPECT_FALSE(readContProfCheckpointDirectory(path));
  ASSERT_TRUE(writeTextFile(path, "not a directory"));
  EXPECT_FALSE(readContProfCheckpointDirectory(path));
}

TEST(ContProfReader, IgnoresSymlinkedCheckpoints) {
  folly::test::TemporaryDirectory directory{"hhvm_cont_prof_reader"};
  auto const target = (directory.path() / "checkpoint-data").native();
  auto const link = (directory.path() / "jit.cprof").native();

  ASSERT_TRUE(writeContProfCheckpointFile(
    target,
    {makeRecord("alpha", 1, 100, 7)}
  ));

  std::error_code error;
  std::filesystem::create_symlink(target, link, error);
  ASSERT_FALSE(error) << error.message();

  auto const result =
    readContProfCheckpointDirectory(directory.path().native());
  ASSERT_TRUE(result);
  EXPECT_EQ(0, result->filesRead);
  EXPECT_EQ(0, result->recordsDecoded);
  EXPECT_TRUE(result->records.empty());
}

TEST(ContProfReader, ReadsNamedCheckpointAndIgnoresLegacyFiles) {
  folly::test::TemporaryDirectory directory{"hhvm_cont_prof_reader"};
  auto const path = [&](const char* filename) {
    return (directory.path() / filename).native();
  };
  auto const alpha = makeRecord("alpha", 1, 100, 7);
  auto const beta = makeRecord("beta", 2, 200, 11);
  ExtensionRegistry::SerializedExtensions const extensions{{"apc", {'a'}}};
  ASSERT_TRUE(writeContProfCheckpointFile(
    path("cont-prof-1-100.cprof"),
    {makeRecord("alpha", 1, 300, 13), makeRecord("gamma", 3, 300, 13)},
    {"src/legacy.php"},
    ExtensionRegistry::SerializedExtensions{{"servicerouter", {'b'}}}
  ));
  ASSERT_TRUE(writeContProfCheckpointFile(
    path("jit.cprof"),
    {alpha, beta},
    {"src/b.php", "src/a.php", "src/b.php"},
    extensions
  ));
  ASSERT_TRUE(writeTextFile(path("notes.txt"), "ignored"));

  auto const result =
    readContProfCheckpointDirectory(directory.path().native());
  ASSERT_TRUE(result);
  EXPECT_EQ(1, result->filesRead);
  EXPECT_EQ(2, result->recordsDecoded);
  std::vector<ContProfProfileRecord> const expected{alpha, beta};
  EXPECT_EQ(expected, result->records);
  std::vector<std::string> const unitPaths{"src/a.php", "src/b.php"};
  EXPECT_EQ(unitPaths, result->unitPaths);
  EXPECT_EQ(extensions, result->extensions);
}

TEST(ContProfReader, DoesNotFallBackToLegacyCheckpoints) {
  folly::test::TemporaryDirectory directory{"hhvm_cont_prof_reader"};
  ASSERT_TRUE(writeContProfCheckpointFile(
    (directory.path() / "cont-prof-1-100.cprof").native(),
    {makeRecord("alpha", 1, 100, 7)}, {"src/legacy.php"},
    ExtensionRegistry::SerializedExtensions{{"apc", {'a'}}}
  ));

  auto result =
    readContProfCheckpointDirectory(directory.path().native());
  ASSERT_TRUE(result);
  EXPECT_EQ(0, result->filesRead);
  EXPECT_EQ(0, result->recordsDecoded);
  EXPECT_TRUE(result->records.empty());
  EXPECT_TRUE(result->unitPaths.empty());
  EXPECT_TRUE(result->extensions.empty());

  ASSERT_TRUE(writeTextFile(
    (directory.path() / "jit.cprof").native(), "not a checkpoint"
  ));
  result = readContProfCheckpointDirectory(directory.path().native());
  ASSERT_TRUE(result);
  EXPECT_EQ(0, result->filesRead);
  EXPECT_EQ(0, result->recordsDecoded);
  EXPECT_TRUE(result->records.empty());
  EXPECT_TRUE(result->unitPaths.empty());
  EXPECT_TRUE(result->extensions.empty());
}

TEST(ContProfReader, ReadsReplacedWholeExtensionSnapshotWithoutUnion) {
  folly::test::TemporaryDirectory directory{"hhvm_cont_prof_reader_extensions"};
  auto const path = (directory.path() / "jit.cprof").native();
  ExtensionRegistry::SerializedExtensions const older{{"apc", {'a'}}};
  ExtensionRegistry::SerializedExtensions const newer{{"servicerouter", {'b'}}};
  auto const record = makeRecord("alpha", 1, 100, 7);
  ASSERT_TRUE(writeContProfCheckpointFile(path, {record}, {}, older));

  auto result = readContProfCheckpointDirectory(directory.path().native());
  ASSERT_TRUE(result);
  EXPECT_EQ(older, result->extensions);

  ASSERT_TRUE(writeContProfCheckpointFile(path, {record}, {}, newer));
  result = readContProfCheckpointDirectory(directory.path().native());
  ASSERT_TRUE(result);
  EXPECT_EQ(newer, result->extensions);

  ExtensionRegistry::SerializedExtensions const empty;
  ASSERT_TRUE(writeContProfCheckpointFile(path, {record}, {}, empty));
  result = readContProfCheckpointDirectory(directory.path().native());
  ASSERT_TRUE(result);
  EXPECT_EQ(empty, result->extensions);

  ASSERT_TRUE(writeContProfCheckpointFile(path, {record}));
  result = readContProfCheckpointDirectory(directory.path().native());
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->extensions.empty());
}

TEST(ContProfReader, ChecksFunctionKey) {
  auto const unit = makeTestUnit();
  ASSERT_NE(nullptr, unit);
  ASSERT_EQ(1, unit->funcs().size());

  auto const func = unit->funcs()[0];
  auto const key = makeContProfFuncKey(*func);
  ASSERT_TRUE(key);

  ContProfProfileRecord record{};
  record.header.funcKey = *key;
  record.header.capturedAtMs = 100;
  record.translations = {
    {
      ContProfStartKind::FuncEntry,
      0,
      3,
      7,
    },
  };

  EXPECT_TRUE(isContProfProfileRecordCompatible(record, *func));

  record.header.funcKey.bytecodeUnitHash =
    SHA1{"4444444444444444444444444444444444444444"};
  EXPECT_FALSE(isContProfProfileRecordCompatible(record, *func));
}

TEST(ContProfReader, RejectsEntriesWhichDoNotExist) {
  auto const unit = makeTestUnit();
  ASSERT_NE(nullptr, unit);
  ASSERT_EQ(1, unit->funcs().size());

  auto const func = unit->funcs()[0];
  auto const key = makeContProfFuncKey(*func);
  ASSERT_TRUE(key);

  ContProfProfileRecord record{};
  record.header.funcKey = *key;
  record.header.capturedAtMs = 100;
  record.translations = {
    {
      ContProfStartKind::FuncEntry,
      1,
      1,
      7,
    },
  };
  EXPECT_FALSE(isContProfProfileRecordCompatible(record, *func));

  record.translations[0] = {
    ContProfStartKind::NamedParamsFuncEntry,
    0,
    1,
    7,
  };
  EXPECT_FALSE(isContProfProfileRecordCompatible(record, *func));
}

struct ContProfReaderBytecodeTest : testing::Test {
  static void SetUpTestSuite() {
    s_unit = makeBytecodeTestUnit();
    ASSERT_NE(nullptr, s_unit);
    ASSERT_EQ(1, s_unit->funcs().size());

    s_func = s_unit->funcs()[0];

    auto const intLength = instrLen(s_func->at(0));
    ASSERT_GT(intLength, 1);
    s_popOffset = intLength;
    s_trueOffset = s_popOffset + instrLen(s_func->at(s_popOffset));
    s_jmpOffset = s_trueOffset + instrLen(s_func->at(s_trueOffset));
    ASSERT_EQ(Op::JmpZ, peek_op(s_func->at(s_jmpOffset)));
    s_fallthroughOffset = s_jmpOffset + instrLen(s_func->at(s_jmpOffset));
    ASSERT_EQ(Op::Null, peek_op(s_func->at(s_fallthroughOffset)));

    s_doneOffset = s_jmpOffset + getImm(s_func->at(s_jmpOffset), 0).u_BA;
    ASSERT_EQ(Op::Int, peek_op(s_func->at(s_doneOffset)));
    s_switchOffset = s_doneOffset + instrLen(s_func->at(s_doneOffset));
    ASSERT_EQ(Op::Switch, peek_op(s_func->at(s_switchOffset)));
    ASSERT_EQ(3, getImmVector(s_func->at(s_switchOffset)).size());

    s_namedEntry = s_func->getNamedParamsFuncEntry();
    ASSERT_TRUE(s_func->hasOptionalNamedParameters());
    ASSERT_GT(s_namedEntry, 0);
    ASSERT_TRUE(s_func->isDVEntry(s_namedEntry));
  }

  static void TearDownTestSuite() {
    s_func = nullptr;
    s_unit.reset();
  }

  static Func* func() { return s_func; }

  static Offset popOffset() { return s_popOffset; }

  static Offset jmpOffset() { return s_jmpOffset; }

  static Offset trueOffset() { return s_trueOffset; }

  static Offset fallthroughOffset() { return s_fallthroughOffset; }

  static Offset namedEntry() { return s_namedEntry; }

  static Offset doneOffset() { return s_doneOffset; }

  static Offset switchOffset() { return s_switchOffset; }

private:
  static inline std::unique_ptr<Unit> s_unit;
  static inline Func* s_func{nullptr};
  static inline Offset s_popOffset{0};
  static inline Offset s_trueOffset{0};
  static inline Offset s_jmpOffset{0};
  static inline Offset s_fallthroughOffset{0};
  static inline Offset s_doneOffset{0};
  static inline Offset s_namedEntry{0};
  static inline Offset s_switchOffset{0};
};

TEST_F(ContProfReaderBytecodeTest, AcceptsValidBytecodeBlock) {
  auto const key = makeContProfFuncKey(*func());
  ASSERT_TRUE(key);

  auto const record = makeBytecodeRecord(*key, trueOffset(), 1);
  ASSERT_TRUE(isValidContProfProfileRecord(record));
  EXPECT_TRUE(isContProfProfileRecordCompatible(record, *func()));
}

TEST_F(ContProfReaderBytecodeTest, RejectsNonemptyStack) {
  auto const key = makeContProfFuncKey(*func());
  ASSERT_TRUE(key);

  auto const record = makeBytecodeRecord(*key, popOffset(), 1);
  ASSERT_TRUE(isValidContProfProfileRecord(record));
  EXPECT_FALSE(isContProfProfileRecordCompatible(record, *func()));
}

TEST_F(ContProfReaderBytecodeTest, RejectsOffsetInsideImmediate) {
  auto const key = makeContProfFuncKey(*func());
  ASSERT_TRUE(key);

  auto const record = makeBytecodeRecord(*key, 1, 1);
  ASSERT_TRUE(isValidContProfProfileRecord(record));
  EXPECT_FALSE(isContProfProfileRecordCompatible(record, *func()));
}

TEST_F(ContProfReaderBytecodeTest, RejectsEntryCollision) {
  auto const key = makeContProfFuncKey(*func());
  ASSERT_TRUE(key);

  auto const record = makeBytecodeRecord(*key, namedEntry(), 1);
  ASSERT_TRUE(isValidContProfProfileRecord(record));
  EXPECT_FALSE(isContProfProfileRecordCompatible(record, *func()));
}

TEST_F(ContProfReaderBytecodeTest, ValidatesLocalTypeGuardBounds) {
  auto const key = makeContProfFuncKey(*func());
  ASSERT_TRUE(key);

  auto record = makeBytecodeRecord(*key, trueOffset(), 1);
  auto const entryLimit = func()->numFuncEntryInputs();
  auto const bytecodeLimit = static_cast<uint32_t>(func()->numLocals());
  ASSERT_GT(entryLimit, 0);
  ASSERT_LT(entryLimit, bytecodeLimit);

  record.translations.front().localTypeGuards = {
    {0, Type::kInitNull},
  };
  ASSERT_TRUE(isValidContProfProfileRecord(record));
  EXPECT_TRUE(isContProfProfileRecordCompatible(record, *func()));

  record.translations.front().localTypeGuards.clear();
  record.translations.back().localTypeGuards = {
    {entryLimit, Type::kInt},
  };
  ASSERT_TRUE(isValidContProfProfileRecord(record));
  EXPECT_TRUE(isContProfProfileRecordCompatible(record, *func()));

  record.translations.back().localTypeGuards.clear();
  record.translations.front().localTypeGuards = {
    {entryLimit, Type::kInt},
  };
  ASSERT_TRUE(isValidContProfProfileRecord(record));
  EXPECT_FALSE(isContProfProfileRecordCompatible(record, *func()));

  record.translations.front().localTypeGuards.clear();
  record.translations.back().localTypeGuards = {
    {bytecodeLimit, Type::kInt},
  };
  ASSERT_TRUE(isValidContProfProfileRecord(record));
  EXPECT_FALSE(isContProfProfileRecordCompatible(record, *func()));
}

TEST_F(ContProfReaderBytecodeTest, ValidatesIncomingTopology) {
  auto const key = makeContProfFuncKey(*func());
  ASSERT_TRUE(key);

  auto record = makeBytecodeRecord(*key, trueOffset(), 2);
  record.translations.push_back({
    ContProfStartKind::Bytecode,
    static_cast<uint32_t>(fallthroughOffset()),
    1,
    9,
    {},
    {1},
  });

  ASSERT_TRUE(isValidContProfProfileRecord(record));
  EXPECT_TRUE(isContProfProfileRecordCompatible(record, *func()));

  record.translations.back().incoming = {0};
  ASSERT_TRUE(isValidContProfProfileRecord(record));
  EXPECT_FALSE(isContProfProfileRecordCompatible(record, *func()));
}

TEST_F(ContProfReaderBytecodeTest, ValidatesLocalPostConditionBounds) {
  auto const key = makeContProfFuncKey(*func());
  ASSERT_TRUE(key);

  auto record = makeBytecodeRecord(*key, trueOffset(), 1);
  auto const entryLimit = func()->numFuncEntryInputs();
  auto const localLimit = static_cast<uint32_t>(func()->numLocals());
  ASSERT_LT(entryLimit, localLimit);

  record.translations.front().localPostConditions = {
    {entryLimit, true, Type::kCell},
  };
  ASSERT_TRUE(isValidContProfProfileRecord(record));
  EXPECT_TRUE(isContProfProfileRecordCompatible(record, *func()));

  record.translations.front().localPostConditions = {
    {localLimit, false, Type::kInt},
  };
  ASSERT_TRUE(isValidContProfProfileRecord(record));
  EXPECT_FALSE(isContProfProfileRecordCompatible(record, *func()));
}

TEST_F(ContProfReaderBytecodeTest, ValidatesTargetProfileSites) {
  auto const key = makeContProfFuncKey(*func());
  ASSERT_TRUE(key);

  auto record = makeBytecodeRecord(*key, trueOffset(), 2);
  auto profile = makeDecRefTargetProfile(trueOffset());
  auto secondProfile = profile;
  secondProfile.name = "DecRefProfile-0";
  auto laterProfile = makeDecRefTargetProfile(jmpOffset());
  record.translations.back().targetProfiles = {
    profile,
    secondProfile,
    laterProfile,
  };

  ASSERT_TRUE(isValidContProfProfileRecord(record));
  EXPECT_TRUE(isContProfProfileRecordCompatible(record, *func()));

  record.translations.back().targetProfiles = {laterProfile};
  record.translations.back().targetProfiles[0].bytecodeOffset =
    static_cast<int32_t>(popOffset());
  ASSERT_TRUE(isValidContProfProfileRecord(record));
  EXPECT_FALSE(isContProfProfileRecordCompatible(record, *func()));

  record.translations.back().targetProfiles.clear();
  laterProfile.bytecodeOffset = 0;
  record.translations.front().targetProfiles = {laterProfile};
  ASSERT_TRUE(isValidContProfProfileRecord(record));
  EXPECT_FALSE(isContProfProfileRecordCompatible(record, *func()));

  record.translations.front().regionLength = 2;
  EXPECT_TRUE(isContProfProfileRecordCompatible(record, *func()));
}

TEST_F(ContProfReaderBytecodeTest, ValidatesSwitchProfileSize) {
  auto const key = makeContProfFuncKey(*func());
  ASSERT_TRUE(key);

  auto record = makeBytecodeRecord(*key, doneOffset(), 2);
  record.translations.back().targetProfiles = {
    makeSwitchTargetProfile(switchOffset(), 3),
  };
  ASSERT_TRUE(isValidContProfProfileRecord(record));
  EXPECT_TRUE(isContProfProfileRecordCompatible(record, *func()));

  record.translations.back().targetProfiles = {
    makeSwitchTargetProfile(switchOffset(), 2),
  };
  ASSERT_TRUE(isValidContProfProfileRecord(record));
  EXPECT_FALSE(isContProfProfileRecordCompatible(record, *func()));

  record.translations.back().targetProfiles = {
    makeSwitchTargetProfile(switchOffset(), 4),
  };
  ASSERT_TRUE(isValidContProfProfileRecord(record));
  EXPECT_FALSE(isContProfProfileRecordCompatible(record, *func()));

  record = makeBytecodeRecord(*key, trueOffset(), 1);
  record.translations.back().targetProfiles = {
    makeSwitchTargetProfile(trueOffset(), 3),
  };
  ASSERT_TRUE(isValidContProfProfileRecord(record));
  EXPECT_FALSE(isContProfProfileRecordCompatible(record, *func()));
}

TEST(ContProfReader, MemoGetStackOffsets) {
  auto const unit = makeMemoTestUnit();
  ASSERT_NE(nullptr, unit);
  ASSERT_EQ(2, unit->funcs().size());
  auto const func = unit->funcs()[0];
  ASSERT_TRUE(func->isMemoizeWrapper());
  ASSERT_EQ(Op::MemoGet, peek_op(func->entry()));

  auto const targets = instrJumpTargets(func->entry(), 0);
  ASSERT_EQ(1, targets.size());
  // A miss pushes nothing; the fallthrough hit pushes the cached value.
  expectMemoStart(*func, targets[0], 0);
  expectMemoStart(*func, instrLen(func->entry()), 1);
}

TEST(ContProfReader, MemoGetEagerStackOffsets) {
  auto const unit = makeMemoTestUnit();
  ASSERT_NE(nullptr, unit);
  ASSERT_EQ(2, unit->funcs().size());
  auto const func = unit->funcs()[1];
  ASSERT_TRUE(func->isMemoizeWrapper());
  ASSERT_TRUE(func->isAsyncFunction());
  ASSERT_EQ(Op::MemoGetEager, peek_op(func->entry()));

  auto const targets = instrJumpTargets(func->entry(), 0);
  ASSERT_EQ(2, targets.size());
  // Both kinds of hit push a value; only the miss keeps the stack empty.
  expectMemoStart(*func, targets[0], 0);
  expectMemoStart(*func, instrLen(func->entry()), 1);
  expectMemoStart(*func, targets[1], 1);
}

}
}
