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

#include "hphp/runtime/vm/jit/cprof-checkpoint.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <folly/ScopeGuard.h>
#include <folly/testing/TestUtil.h>
#include <gtest/gtest.h>

#include "hphp/runtime/base/array-data.h"
#include "hphp/runtime/base/program-functions.h"
#include "hphp/runtime/ext/apc/ext_apc.h"
#include "hphp/runtime/ext/extension-registry.h"
#include "hphp/runtime/vm/jit/cprof-consume.h"
#include "hphp/runtime/vm/jit/cprof-controller.h"
#include "hphp/runtime/vm/jit/cprof-reader.h"
#include "hphp/runtime/vm/treadmill.h"
#include "hphp/util/blob-encoder.h"
#include "hphp/util/configs/jit.h"
#include "hphp/util/configs/server.h"
#include "hphp/util/service-data.h"

namespace HPHP {
Variant HHVM_FUNCTION(apc_delete, const Variant& key);
}

namespace HPHP::jit::cprof {
namespace {

ContProfProfileRecord makeRecord(
  std::string name,
  uint8_t identity,
  uint64_t executionCount
) {
  ContProfProfileRecord record{};
  record.header.funcKey.resolutionUnitPath = "src/" + name + ".php";
  record.header.funcKey.bytecodeUnitHash =
    SHA1{static_cast<uint64_t>(identity)};
  record.header.funcKey.functionName = std::move(name);
  record.header.capturedAtMs = 1'700'000'000'000 + identity;
  record.translations = {
    {
      ContProfStartKind::FuncEntry,
      0,
      2,
      executionCount,
    },
  };
  return record;
}

void expectCheckpointCounters(const std::string& path, size_t records) {
  EXPECT_EQ(records, ServiceData::exportCounterByKey(
    "jit.cprof.checkpoint.records"
  ).value_or(-1));
  EXPECT_EQ(std::filesystem::file_size(path), ServiceData::exportCounterByKey(
    "jit.cprof.checkpoint.bytes"
  ).value_or(-1));
}

void expectCheckpointRecords(
  const std::string& path,
  const std::vector<ContProfProfileRecord>& records
) {
  auto const checkpoint = readContProfCheckpointFile(path);
  ASSERT_TRUE(checkpoint);
  EXPECT_EQ(records, checkpoint->records);
}

void appendUint32(std::vector<uint8_t>& bytes, uint32_t value) {
  auto const data = reinterpret_cast<const uint8_t*>(&value);
  bytes.insert(bytes.end(), data, data + sizeof(value));
}

std::vector<std::string> directoryEntries(const std::filesystem::path& path) {
  std::vector<std::string> names;
  for (auto const& entry : std::filesystem::directory_iterator{path}) {
    names.push_back(entry.path().filename().native());
  }
  std::sort(names.begin(), names.end());
  return names;
}

TEST(ContProfCheckpoint, RoundTripsRecordsAndCanonicalUnitPaths) {
  std::vector<ContProfProfileRecord> const expected{
    makeRecord("alpha", 1, 7),
    makeRecord("beta", 2, 11),
  };

  std::vector<std::string> const unitPaths{"src/alpha.php", "src/beta.php"};
  auto const encoded = serializeContProfCheckpoint(
    expected, {"src/beta.php", "src/alpha.php", "src/beta.php"}
  );
  ASSERT_TRUE(encoded);

  auto const decoded = deserializeContProfCheckpoint(
    folly::ByteRange{encoded->data(), encoded->size()}
  );
  ASSERT_TRUE(decoded);
  EXPECT_EQ(expected, decoded->records);
  EXPECT_EQ(unitPaths, decoded->unitPaths);
  EXPECT_EQ(encoded, serializeContProfCheckpoint(expected, unitPaths));
}

TEST(ContProfCheckpoint, RoundTripsOptionalExtensionData) {
  std::vector<ContProfProfileRecord> const records{makeRecord("alpha", 1, 7)};
  auto const legacy = serializeContProfCheckpoint(records);
  ASSERT_TRUE(legacy);
  auto const oldCheckpoint = deserializeContProfCheckpoint(
    folly::ByteRange{legacy->data(), legacy->size()}
  );
  ASSERT_TRUE(oldCheckpoint);
  EXPECT_FALSE(oldCheckpoint->extensionData);

  for (auto const& snapshot :
       std::vector<ExtensionRegistry::SerializedExtensions>{
    {},
    {
      {"z_dependency", {'a', '\0', static_cast<char>(0xff)}},
      {"apc", {'x'}},
      {"apc", {'y'}},
      {"a_consumer", {}},
    },
  }) {
    auto const encoded = serializeContProfCheckpoint(records, {}, snapshot);
    ASSERT_TRUE(encoded);
    auto expectedSize = legacy->size() + sizeof(uint32_t);
    for (auto const& [name, payload] : snapshot) {
      expectedSize += 2 * sizeof(uint32_t) + name.size() + payload.size();
    }
    ASSERT_EQ(expectedSize, encoded->size());
    EXPECT_TRUE(std::equal(legacy->begin(), legacy->end(), encoded->begin()));
    auto const decoded = deserializeContProfCheckpoint(
      folly::ByteRange{encoded->data(), encoded->size()}
    );
    ASSERT_TRUE(decoded);
    EXPECT_EQ(records, decoded->records);
    ASSERT_TRUE(decoded->extensionData);
    EXPECT_EQ(snapshot, *decoded->extensionData);
  }
}

TEST(ContProfCheckpoint, BoundsTheWholeExtensionBatch) {
  std::vector<ContProfProfileRecord> const records{makeRecord("alpha", 1, 7)};
  auto const framingSize = sizeof(uint32_t) +
    2 * (2 * sizeof(uint32_t) + 1);
  ExtensionRegistry::SerializedExtensions data{{"a", {}}, {"b", {'x'}}};
  data.front().payload.resize(
    kMaxContProfExtensionPayloadBytes - framingSize - 1, 'x'
  );
  auto encoded = serializeContProfCheckpoint(records, {}, data);
  ASSERT_TRUE(encoded);
  auto decoded = deserializeContProfCheckpoint(
    folly::ByteRange{encoded->data(), encoded->size()}
  );
  ASSERT_TRUE(decoded);
  ASSERT_TRUE(decoded->extensionData);
  EXPECT_EQ(data, *decoded->extensionData);
  data.back().payload.push_back('x');
  encoded = serializeContProfCheckpoint(records, {}, data);
  ASSERT_TRUE(encoded);
  decoded = deserializeContProfCheckpoint(
    folly::ByteRange{encoded->data(), encoded->size()}
  );
  ASSERT_TRUE(decoded);
  EXPECT_EQ(records, decoded->records);
  EXPECT_FALSE(decoded->extensionData);
}

TEST(ContProfCheckpoint, MalformedExtensionDataPreservesRecords) {
  std::vector<ContProfProfileRecord> const records{makeRecord("alpha", 1, 7)};
  std::vector<char> const payload{'x', '\0', 'y'};
  ExtensionRegistry::SerializedExtensions const snapshot{
    {"apc", payload}, {"servicerouter", payload}
  };
  auto const legacy = serializeContProfCheckpoint(records);
  auto const encoded = serializeContProfCheckpoint(records, {}, snapshot);
  ASSERT_TRUE(legacy);
  ASSERT_TRUE(encoded);

  for (size_t size = legacy->size() + 1; size < encoded->size(); ++size) {
    auto const decoded = deserializeContProfCheckpoint(
      folly::ByteRange{encoded->data(), size}
    );
    ASSERT_TRUE(decoded) << size;
    EXPECT_EQ(records, decoded->records);
    EXPECT_FALSE(decoded->extensionData);
  }

  for (auto bytes : {*legacy, *encoded}) {
    bytes.push_back(0);
    auto const decoded = deserializeContProfCheckpoint(
      folly::ByteRange{bytes.data(), bytes.size()}
    );
    ASSERT_TRUE(decoded);
    EXPECT_EQ(records, decoded->records);
    EXPECT_FALSE(decoded->extensionData);
  }

  auto corrupted = *encoded;
  auto const payloadSizeOffset =
    corrupted.size() - payload.size() - sizeof(uint32_t);
  std::fill_n(
    corrupted.begin() + payloadSizeOffset, sizeof(uint32_t), uint8_t{0xff}
  );
  auto const decoded = deserializeContProfCheckpoint(
    folly::ByteRange{corrupted.data(), corrupted.size()}
  );
  ASSERT_TRUE(decoded);
  EXPECT_EQ(records, decoded->records);
  EXPECT_FALSE(decoded->extensionData);

  for (auto const& names : std::vector<std::vector<std::string>>{
    {""}, {std::string{"apc\0suffix", 10}},
  }) {
    auto bytes = *legacy;
    appendUint32(bytes, static_cast<uint32_t>(names.size()));
    for (auto const& name : names) {
      appendUint32(bytes, static_cast<uint32_t>(name.size()));
      bytes.insert(bytes.end(), name.begin(), name.end());
      appendUint32(bytes, static_cast<uint32_t>(payload.size()));
      bytes.insert(bytes.end(), payload.begin(), payload.end());
    }
    auto const invalid = deserializeContProfCheckpoint(
      folly::ByteRange{bytes.data(), bytes.size()}
    );
    ASSERT_TRUE(invalid);
    EXPECT_EQ(records, invalid->records);
    EXPECT_FALSE(invalid->extensionData);
  }
}

TEST(ContProfCheckpoint, SkipsInvalidRecords) {
  folly::test::TemporaryDirectory temp{"cont-prof-checkpoint"};
  auto const path = temp.path().native() + "/checkpoint.cprof";
  auto const first = makeRecord("alpha", 1, 7);
  auto invalid = makeRecord("beta", 2, 11);
  invalid.header.capturedAtMs = 0;
  auto const last = makeRecord("gamma", 3, 13);

  ASSERT_TRUE(writeContProfCheckpointFile(path, {
    first,
    invalid,
    last,
  }));
  expectCheckpointCounters(path, 2);

  auto const decoded = readContProfCheckpointFile(path);
  ASSERT_TRUE(decoded);

  std::vector<ContProfProfileRecord> const expected{first, last};
  EXPECT_EQ(expected, decoded->records);
  EXPECT_TRUE(decoded->unitPaths.empty());
}

TEST(ContProfCheckpoint, RejectsMalformedOrNoncanonicalInput) {
  auto const first = makeRecord("alpha", 1, 7);
  auto const second = makeRecord("beta", 2, 11);

  EXPECT_FALSE(serializeContProfCheckpoint({}));
  EXPECT_FALSE(serializeContProfCheckpoint({second, first}));
  EXPECT_FALSE(serializeContProfCheckpoint({first, first}));

  auto const encoded = serializeContProfCheckpoint(
    {first, second}, {"src/dependency.php"}
  );
  ASSERT_TRUE(encoded);

  for (size_t size = 0; size < encoded->size(); ++size) {
    EXPECT_FALSE(deserializeContProfCheckpoint(
      folly::ByteRange{encoded->data(), size}
    ));
  }

  auto wrongRecordCount = *encoded;
  wrongRecordCount[0] ^= 1;
  EXPECT_FALSE(deserializeContProfCheckpoint(
    folly::ByteRange{wrongRecordCount.data(), wrongRecordCount.size()}
  ));
}

TEST(ContProfCheckpoint, RequiresCanonicalUnitManifestFrame) {
  std::vector<ContProfProfileRecord> const records{makeRecord("alpha", 1, 7)};
  auto legacy = serializeContProfCheckpoint(records);
  ASSERT_TRUE(legacy);
  legacy->resize(legacy->size() - sizeof(uint32_t));
  EXPECT_FALSE(deserializeContProfCheckpoint(
    folly::ByteRange{legacy->data(), legacy->size()}
  ));

  auto const withManifest = [&](const std::vector<std::string>& paths) {
    auto bytes = *legacy;
    appendUint32(bytes, static_cast<uint32_t>(paths.size()));
    for (auto const& path : paths) {
      appendUint32(bytes, static_cast<uint32_t>(path.size()));
      bytes.insert(bytes.end(), path.begin(), path.end());
    }
    return bytes;
  };
  for (auto const& paths : std::vector<std::vector<std::string>>{
    {""}, {"/absolute.php"}, {"../outside.php"},
    {std::string{"src/a\0.php", 10}},
    {"src/b.php", "src/a.php"}, {"src/a.php", "src/a.php"},
  }) {
    auto const bytes = withManifest(paths);
    EXPECT_FALSE(deserializeContProfCheckpoint(
      folly::ByteRange{bytes.data(), bytes.size()}
    ));
  }
  for (auto const& path : {"", "/absolute.php", "../outside.php"}) {
    EXPECT_FALSE(serializeContProfCheckpoint(records, {path}));
  }

  auto oversizedCount = *legacy;
  appendUint32(oversizedCount, uint32_t{0xffffffff});
  EXPECT_FALSE(deserializeContProfCheckpoint(
    folly::ByteRange{oversizedCount.data(), oversizedCount.size()}
  ));
}

TEST(ContProfCheckpoint, AtomicallyReplacesFile) {
  folly::test::TemporaryDirectory temp{"cont-prof-checkpoint"};
  auto const path = temp.path().native() + "/checkpoint.cprof";
  auto const failures = ServiceData::exportCounterByKey(
    "jit.cprof.checkpoint.write_failures"
  ).value_or(0);

  std::vector<ContProfProfileRecord> const first{
    makeRecord("alpha", 1, 7),
  };
  ASSERT_TRUE(writeContProfCheckpointFile(path, first));
  expectCheckpointRecords(path, first);
  expectCheckpointCounters(path, first.size());

  EXPECT_FALSE(writeContProfCheckpointFile(path, {}));
  expectCheckpointRecords(path, first);
  expectCheckpointCounters(path, first.size());
  EXPECT_EQ(failures + 1, ServiceData::exportCounterByKey(
    "jit.cprof.checkpoint.write_failures"
  ).value_or(-1));

  std::vector<ContProfProfileRecord> const replacement{
    makeRecord("alpha", 1, 7),
    makeRecord("beta", 2, 11),
  };
  EXPECT_FALSE(writeContProfCheckpointFile(
    temp.path().native() + "/missing/checkpoint.cprof", replacement
  ));
  expectCheckpointCounters(path, first.size());
  EXPECT_EQ(failures + 2, ServiceData::exportCounterByKey(
    "jit.cprof.checkpoint.write_failures"
  ).value_or(-1));
  std::vector<std::string> const unitPaths{"src/dependency.php"};
  ASSERT_TRUE(writeContProfCheckpointFile(path, replacement, unitPaths));
  auto const decoded = readContProfCheckpointFile(path);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(replacement, decoded->records);
  EXPECT_EQ(unitPaths, decoded->unitPaths);
  expectCheckpointCounters(path, replacement.size());
  EXPECT_EQ(failures + 2, ServiceData::exportCounterByKey(
    "jit.cprof.checkpoint.write_failures"
  ).value_or(-1));
}

TEST(ContProfCheckpoint, SelectsHottestRecordsWithinLimit) {
  folly::test::TemporaryDirectory temp{"cont-prof-checkpoint"};
  auto const path = temp.path().native() + "/checkpoint.cprof";
  constexpr size_t recordLimit = 100'000;

  std::vector<ContProfProfileRecord> records;
  records.reserve(recordLimit + 1);

  for (size_t i = 0; i <= recordLimit; ++i) {
    records.push_back(makeRecord(
      std::to_string(1'000'000 + i),
      1,
      i == recordLimit ? 2 : 1
    ));
  }

  ASSERT_TRUE(writeContProfCheckpointFile(path, records));
  expectCheckpointCounters(path, recordLimit);

  auto const decoded = readContProfCheckpointFile(path);
  ASSERT_TRUE(decoded);

  records.erase(records.end() - 2);
  EXPECT_EQ(records, decoded->records);
}

void writeRetainedCheckpointGeneration(
  const std::string& directory,
  size_t generation
) {
  ASSERT_EQ(0, numContProfProfileRecords());
  auto const original = makeRecord("rotation_original", 1, 7);
  if (generation == 0) {
    ASSERT_TRUE(retainContProfProfileRecord(original));
  } else {
    auto const input = readContProfCheckpointDirectory(directory);
    ASSERT_TRUE(input);
    ASSERT_EQ(generation, input->records.size());
    auto const stored = std::find(
      input->records.begin(), input->records.end(), original
    );
    ASSERT_NE(input->records.end(), stored);
    for (auto const& record : input->records) {
      ASSERT_TRUE(retainContProfProfileRecord(record));
    }
    // Distinct synthetic records stand in for newly captured functions. The
    // real-capture replacement path is exercised in ContProfCaptureTest.
    ASSERT_TRUE(retainContProfProfileRecord(makeRecord(
      "rotation_" + std::to_string(generation),
      static_cast<uint8_t>(generation + 1),
      11 + generation
    )));
  }

  auto const records = snapshotContProfProfileRecords();
  ASSERT_EQ(generation + 1, records.size());
  auto const path = directory + "/jit.cprof";
  ASSERT_TRUE(writeContProfCheckpointFile(path, records));
  expectCheckpointRecords(path, records);
}

TEST(ContProfCheckpoint, CarriesOriginalThroughTwentyFiveProcessGenerations) {
  // GTest's threadsafe death tests re-exec this test for each generation. Keep
  // the parent-owned directory across re-execs, but not the controller's map.
  constexpr auto environment = "HHVM_CPROF_ROTATION_TEST_DIRECTORY";
  auto const inherited = std::getenv(environment);
  std::unique_ptr<folly::test::TemporaryDirectory> temporary;
  std::string directory;
  if (inherited) {
    directory = inherited;
  } else {
    temporary = std::make_unique<folly::test::TemporaryDirectory>(
      "cont-prof-checkpoint-generations"
    );
    directory = temporary->path().native();
    ASSERT_EQ(0, ::setenv(environment, directory.c_str(), 1));
  }
  SCOPE_EXIT { if (!inherited) ::unsetenv(environment); };
  auto const oldDeathTestStyle = GTEST_FLAG_GET(death_test_style);
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  SCOPE_EXIT { GTEST_FLAG_SET(death_test_style, oldDeathTestStyle); };

  constexpr size_t generations = 25;
  for (size_t generation = 0; generation < generations; ++generation) {
    // No parent assertions inside the loop: a re-exec skips the earlier
    // ASSERT_EXIT statements before reaching its own generation.
    ASSERT_EXIT({
      std::thread{[&] {
        HphpSessionAndThread session{Treadmill::SessionKind::UnitTests};
        writeRetainedCheckpointGeneration(directory, generation);
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

  EXPECT_EQ(std::vector<std::string>{"jit.cprof"}, directoryEntries(directory));
  auto const input = readContProfCheckpointDirectory(directory);
  ASSERT_TRUE(input);
  EXPECT_EQ(1, input->filesRead);
  ASSERT_EQ(generations, input->records.size());
  auto const original = makeRecord("rotation_original", 1, 7);
  EXPECT_NE(input->records.end(), std::find(
    input->records.begin(), input->records.end(), original
  ));
}

TEST(ContProfCheckpoint, ActivatesOnlyWhileWriterIsRunning) {
  folly::test::TemporaryDirectory temp{"cont-prof-checkpoint-activation"};

  auto const writerCounters = [] {
    std::vector<std::string> const keys{
      "jit.cprof.writer.active",
      "jit.cprof.writer.start_attempts",
      "jit.cprof.writer.start_failures",
      "jit.cprof.writer.checkpoint_attempts",
      "jit.cprof.writer.empty_checkpoint_skips",
      "jit.cprof.writer.not_server_mode",
      "jit.cprof.writer.capture_disabled",
      "jit.cprof.writer.zero_interval",
      "jit.cprof.writer.deserialized_prof_data",
      "jit.cprof.writer.invalid_directory",
      "jit.cprof.writer.thread_start_failure",
      "jit.cprof.writer.unexpected_stop",
    };
    ServiceData::CounterMap counters;
    for (auto const& key : keys) {
      counters.emplace(key, ServiceData::exportCounterByKey(key).value_or(0));
    }
    return counters;
  };
  auto expectedCounters = writerCounters();
  ASSERT_EQ(size_t{12}, expectedCounters.size());
  ASSERT_EQ(0, expectedCounters.at("jit.cprof.writer.active"));

  auto const oldServerMode = std::exchange(Cfg::Server::Mode, false);
  auto const oldCaptureEnabled =
    std::exchange(Cfg::Jit::ContProfCaptureEnabled, true);
  auto const oldInterval = std::exchange(
    Cfg::Jit::ContProfCheckpointIntervalSeconds,
    uint32_t{3600}
  );
  auto const oldDirectory = std::exchange(
    Cfg::Jit::ContProfCheckpointDirectory,
    temp.path().native()
  );
  SCOPE_EXIT {
    stopContProfCheckpointWriter();
    Cfg::Server::Mode = oldServerMode;
    Cfg::Jit::ContProfCaptureEnabled = oldCaptureEnabled;
    Cfg::Jit::ContProfCheckpointIntervalSeconds = oldInterval;
    Cfg::Jit::ContProfCheckpointDirectory = oldDirectory;
  };

  ++expectedCounters["jit.cprof.writer.start_attempts"];
  ++expectedCounters["jit.cprof.writer.not_server_mode"];
  startContProfCheckpointWriter();
  EXPECT_FALSE(contProfActive());
  EXPECT_FALSE(contProfActive());
  EXPECT_EQ(0, ServiceData::exportCounterByKey("jit.cprof.writer.active").value_or(-1));
  EXPECT_EQ(expectedCounters, writerCounters());

  Cfg::Server::Mode = true;
  Cfg::Jit::ContProfCaptureEnabled = false;
  ++expectedCounters["jit.cprof.writer.start_attempts"];
  ++expectedCounters["jit.cprof.writer.capture_disabled"];
  startContProfCheckpointWriter();
  EXPECT_FALSE(contProfActive());
  EXPECT_EQ(expectedCounters, writerCounters());

  Cfg::Jit::ContProfCaptureEnabled = true;
  Cfg::Jit::ContProfCheckpointIntervalSeconds = 0;
  ++expectedCounters["jit.cprof.writer.start_attempts"];
  ++expectedCounters["jit.cprof.writer.zero_interval"];
  startContProfCheckpointWriter();
  EXPECT_FALSE(contProfActive());
  EXPECT_EQ(expectedCounters, writerCounters());

  Cfg::Jit::ContProfCheckpointIntervalSeconds = 3600;
  Cfg::Jit::ContProfCheckpointDirectory.clear();
  ++expectedCounters["jit.cprof.writer.start_attempts"];
  ++expectedCounters["jit.cprof.writer.start_failures"];
  ++expectedCounters["jit.cprof.writer.invalid_directory"];
  startContProfCheckpointWriter();
  EXPECT_FALSE(contProfActive());
  EXPECT_EQ(expectedCounters, writerCounters());

  Cfg::Jit::ContProfCheckpointDirectory = temp.path().native() + "/missing";
  ++expectedCounters["jit.cprof.writer.start_attempts"];
  ++expectedCounters["jit.cprof.writer.start_failures"];
  ++expectedCounters["jit.cprof.writer.invalid_directory"];
  startContProfCheckpointWriter();
  EXPECT_FALSE(contProfActive());
  EXPECT_EQ(expectedCounters, writerCounters());

  Cfg::Jit::ContProfCheckpointDirectory = temp.path().native();
  ++expectedCounters["jit.cprof.writer.start_attempts"];
  expectedCounters["jit.cprof.writer.active"] = 1;
  startContProfCheckpointWriter();
  EXPECT_TRUE(contProfActive());
  EXPECT_EQ(expectedCounters, writerCounters());

  // Already-running starts and eligibility checks must not count new attempts.
  startContProfCheckpointWriter();
  EXPECT_TRUE(contProfActive());
  EXPECT_EQ(expectedCounters, writerCounters());

  // The writer gauge reports thread state, not dynamic capture eligibility.
  Cfg::Jit::ContProfCaptureEnabled = false;
  startContProfCheckpointWriter();
  EXPECT_FALSE(contProfActive());
  EXPECT_EQ(expectedCounters, writerCounters());
  Cfg::Jit::ContProfCaptureEnabled = true;

  // A standalone run exercises the empty flush; other capture tests may have
  // already retained records in this process. Stop joins the final flush.
  if (numContProfProfileRecords() == 0) {
    ++expectedCounters["jit.cprof.writer.empty_checkpoint_skips"];
  }
  ++expectedCounters["jit.cprof.writer.checkpoint_attempts"];
  expectedCounters["jit.cprof.writer.active"] = 0;
  stopContProfCheckpointWriter();
  EXPECT_FALSE(contProfActive());
  EXPECT_EQ(0, ServiceData::exportCounterByKey("jit.cprof.writer.active").value_or(-1));
  EXPECT_EQ(expectedCounters, writerCounters());

  stopContProfCheckpointWriter();
  EXPECT_EQ(expectedCounters, writerCounters());
}

TEST(ContProfCheckpoint, WriterPreservesAPCKeysAcrossFailedSnapshots) {
  folly::test::TemporaryDirectory temp{"cont-prof-checkpoint-apc"};
  folly::test::TemporaryDirectory next{"cont-prof-checkpoint-restart"};
  auto const apc = ExtensionRegistry::get("apc");
  ASSERT_NE(nullptr, apc);
  auto const key = OptString{"cont-prof-checkpoint-apc:original"};
  auto const freshKey = OptString{"cont-prof-checkpoint-apc:fresh"};
  auto const oversizedKey = OptString{
    std::string{"cont-prof-checkpoint-apc:"} +
    std::string(kMaxContProfExtensionPayloadBytes, 'x')
  };
  auto const oldServerMode = std::exchange(Cfg::Server::Mode, true);
  auto const oldCapture = std::exchange(Cfg::Jit::ContProfCaptureEnabled, true);
  auto const oldInterval = std::exchange(
    Cfg::Jit::ContProfCheckpointIntervalSeconds, uint32_t{1}
  );
  auto const oldDirectory = std::exchange(
    Cfg::Jit::ContProfCheckpointDirectory, temp.path().native()
  );
  auto const oldPrefixes = apcExtension::SerializePrefix;
  auto const oldWarmupData = apc->getWarmupData();
  apcExtension::SerializePrefix = {"cont-prof-checkpoint-apc:"};
  apc->setWarmupData(nullptr);
  SCOPE_EXIT {
    stopContProfCheckpointWriter();
    HHVM_FN(apc_delete)(Variant{key});
    HHVM_FN(apc_delete)(Variant{freshKey});
    HHVM_FN(apc_delete)(Variant{oversizedKey});
    apcExtension::SerializePrefix = oldPrefixes;
    if (auto const data = apc->getWarmupData()) DecRefSharedArray(data);
    apc->setWarmupData(oldWarmupData);
    Cfg::Server::Mode = oldServerMode;
    Cfg::Jit::ContProfCaptureEnabled = oldCapture;
    Cfg::Jit::ContProfCheckpointIntervalSeconds = oldInterval;
    Cfg::Jit::ContProfCheckpointDirectory = oldDirectory;
  };

  auto const waitUntil = [](auto&& ready) {
    auto const deadline = std::chrono::steady_clock::now() +
      std::chrono::seconds{15};
    do {
      if (ready()) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds{10});
    } while (std::chrono::steady_clock::now() < deadline);
    return ready();
  };
  auto const counter = [](const char* name) {
    return ServiceData::exportCounterByKey(name).value_or(-1);
  };
  auto const waitForFailedSnapshotWrite = [&] {
    auto const failures = counter("jit.cprof.extensions.snapshot_failures");
    if (!waitUntil([&] {
      return counter("jit.cprof.extensions.snapshot_failures") > failures;
    })) return false;
    auto const writes = counter("jit.cprof.checkpoint.write_successes");
    return waitUntil([&] {
      return counter("jit.cprof.checkpoint.write_successes") > writes;
    });
  };
  auto const payloadFor = [](std::string_view name) {
    BlobEncoder encoder;
    encoder.lazyCount([&] {
      if (!name.empty()) encoder(name);
      return name.empty() ? uint32_t{0} : uint32_t{1};
    });
    return encoder.take();
  };
  auto const path = (temp.path() / kContProfCheckpointFilename).native();
  std::optional<ContProfCheckpoint> checkpoint;
  ExtensionRegistry::SerializedExtension const* apcSnapshot = nullptr;
  auto const readSnapshot = [&] {
    checkpoint = readContProfCheckpointFile(path);
    if (!checkpoint || !checkpoint->extensionData) return false;
    auto const& extensions = *checkpoint->extensionData;
    auto const found = std::find_if(
      extensions.begin(), extensions.end(),
      [](auto const& data) { return data.name == "apc"; }
    );
    if (found == extensions.end()) return false;
    apcSnapshot = &*found;
    return true;
  };

  auto const record = makeRecord("writer_apc_retention", 73, 7);
  ASSERT_TRUE(retainContProfProfileRecord(record));
  ASSERT_TRUE(HHVM_FN(apc_store)(Variant{key}, Variant{1}).toBoolean());
  startContProfCheckpointWriter();
  ASSERT_TRUE(contProfActive());
  ASSERT_TRUE(waitUntil(readSnapshot));
  EXPECT_EQ(payloadFor(key.data()), apcSnapshot->payload);

  ASSERT_TRUE(HHVM_FN(apc_store)(Variant{oversizedKey}, Variant{1}).toBoolean());
  ASSERT_TRUE(waitForFailedSnapshotWrite());
  ASSERT_TRUE(readSnapshot());
  EXPECT_EQ(payloadFor(key.data()), apcSnapshot->payload);
  prepareContProfStartupCandidates({}, 0, {}, {*apcSnapshot});
  auto const warmup = apc->getWarmupData();
  ASSERT_NE(nullptr, warmup);
  ASSERT_EQ(1, warmup->size());
  auto const restoredKey = warmup->get(int64_t{0});
  ASSERT_TRUE(isStringType(restoredKey.m_type));
  EXPECT_STREQ(key.data(), restoredKey.m_data.pstr->data());

  HHVM_FN(apc_delete)(Variant{oversizedKey});
  HHVM_FN(apc_delete)(Variant{key});
  auto const emptyPayload = payloadFor({});
  ASSERT_TRUE(waitUntil([&] {
    return readSnapshot() && apcSnapshot->payload == emptyPayload;
  }));

  ASSERT_TRUE(HHVM_FN(apc_store)(Variant{freshKey}, Variant{1}).toBoolean());
  auto const freshPayload = payloadFor(freshKey.data());
  ASSERT_TRUE(waitUntil([&] {
    return readSnapshot() && apcSnapshot->payload == freshPayload;
  }));

  ASSERT_TRUE(HHVM_FN(apc_store)(Variant{oversizedKey}, Variant{1}).toBoolean());
  ASSERT_TRUE(waitForFailedSnapshotWrite());
  ASSERT_TRUE(readSnapshot());
  EXPECT_EQ(freshPayload, apcSnapshot->payload);
  auto const beforeFinalFlush = *checkpoint->extensionData;
  stopContProfCheckpointWriter();
  ASSERT_TRUE(readSnapshot());
  EXPECT_EQ(beforeFinalFlush, *checkpoint->extensionData);
  EXPECT_NE(checkpoint->records.end(), std::find(
    checkpoint->records.begin(), checkpoint->records.end(), record
  ));

  // A failed first snapshot must not reuse a previous writer's batch.
  auto const failures = counter("jit.cprof.extensions.snapshot_failures");
  Cfg::Jit::ContProfCheckpointDirectory = next.path().native();
  Cfg::Jit::ContProfCheckpointIntervalSeconds = 3600;
  startContProfCheckpointWriter();
  ASSERT_TRUE(contProfActive());
  stopContProfCheckpointWriter();
  EXPECT_EQ(failures + 1, counter("jit.cprof.extensions.snapshot_failures"));
  auto const nextFiles = directoryEntries(next.path().native());
  ASSERT_EQ(1, nextFiles.size());
  auto const nextCheckpoint = readContProfCheckpointFile(
    (next.path() / nextFiles.front()).native()
  );
  ASSERT_TRUE(nextCheckpoint);
  EXPECT_FALSE(nextCheckpoint->extensionData);
}

}
}
