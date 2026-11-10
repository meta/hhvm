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
#include <gtest/gtest.h>

#include <folly/ScopeGuard.h>
#include <folly/testing/TestUtil.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <set>
#include <string_view>
#include <vector>

#include "hphp/runtime/base/array-data.h"
#include "hphp/runtime/base/concurrent-shared-store.h"
#include "hphp/runtime/base/tv-comparisons.h"
#include "hphp/runtime/ext/apc/ext_apc.h"
#include "hphp/runtime/ext/extension-registry.h"
#include "hphp/runtime/vm/jit/cprof-consume.h"
#include "hphp/runtime/vm/jit/prof-data-serialize.h"
#include "hphp/util/blob-encoder.h"
#include "hphp/util/configs/eval.h"
#include "hphp/util/configs/jit.h"
#include "hphp/util/service-data.h"

namespace HPHP {

Variant HHVM_FUNCTION(apc_delete, const Variant& key);

//////////////////////////////////////////////////////////////////////

namespace {

using Store = ConcurrentTableSharedStore;

/*
 * Just an empty table.
 */
std::unique_ptr<Store> new_store() {
  return std::make_unique<Store>();
}

const StaticString s_key("key");
const StaticString s_key2("key2");
const StaticString s_foo("foo");
const StaticString s_value1("value1");
const StaticString s_value2("value2");

ServiceData::CounterMap contProfRestoreCounters() {
  ServiceData::CounterMap counters;
  for (auto const name : {
    "jit.cprof.extensions.restore_attempts",
    "jit.cprof.extensions.restored_extensions",
    "jit.cprof.extensions.restore_skipped_existing",
    "jit.cprof.extensions.restore_skipped_unavailable",
    "jit.cprof.extensions.restore_failures",
  }) {
    auto const counter = ServiceData::getCounterIfExists(name);
    counters[name] = counter ? counter->getValue() : 0;
  }
  return counters;
}

}

//////////////////////////////////////////////////////////////////////

TEST(APC, Basic) {
  auto store = new_store();

  EXPECT_EQ(store->add(s_key, Variant(s_value1), 1500, 0, APCHandleLevel::Outer), true);
  EXPECT_EQ(store->exists(s_key), true);
  Variant got;
  EXPECT_EQ(store->get(s_key, got), true);
  EXPECT_TRUE(tvSame(*got.asTypedValue(),
    make_tv<KindOfPersistentString>(s_value1.get())));
  EXPECT_EQ(store->eraseKey(s_key), true);
  EXPECT_EQ(store->get(s_key, got), false);
}

TEST(APC, SetOverwrite) {
  auto store = new_store();

  store->set(s_key, Variant(s_value1), 1500, 0, APCHandleLevel::Outer);
  Variant got;
  EXPECT_EQ(store->get(s_key, got), true);
  EXPECT_TRUE(tvSame(*got.asTypedValue(),
              make_tv<KindOfPersistentString>(s_value1.get())));
  store->set(s_key, Variant(s_value2), 1500, 0, APCHandleLevel::Outer);
  EXPECT_EQ(store->get(s_key, got), true);
  EXPECT_TRUE(tvSame(*got.asTypedValue(),
              make_tv<KindOfPersistentString>(s_value2.get())));
}

TEST(APC, IncCas) {
  auto store = new_store();
  bool found = false;

  store->set(s_key, Variant(1), 1500, 0, APCHandleLevel::Outer);
  EXPECT_EQ(store->inc(s_key, 1, found), 2);
  EXPECT_TRUE(found);
  EXPECT_EQ(store->inc(s_key, 1, found), 3);
  EXPECT_TRUE(found);
  EXPECT_EQ(store->inc(s_key, 1, found), 4);
  EXPECT_TRUE(found);
  EXPECT_EQ(store->inc(s_key, 1, found), 5);
  EXPECT_TRUE(found);

  store->set(s_key, Variant(1.0), 1500, 0, APCHandleLevel::Outer);
  EXPECT_EQ(store->inc(s_key, 1, found), 2);
  EXPECT_TRUE(found);
  EXPECT_EQ(store->inc(s_key, 1, found), 3);
  EXPECT_TRUE(found);
  EXPECT_EQ(store->inc(s_key, 1, found), 4);
  EXPECT_TRUE(found);
  EXPECT_EQ(store->inc(s_key, 1, found), 5);
  EXPECT_TRUE(found);

  store->set(s_key, Variant(1), 1500, 0, APCHandleLevel::Outer);
  EXPECT_TRUE(store->cas(s_key, 1, 2));
  EXPECT_TRUE(store->cas(s_key, 2, 3));
  EXPECT_TRUE(store->cas(s_key, 3, 4));
  EXPECT_TRUE(store->cas(s_key, 4, 5));
  EXPECT_FALSE(store->cas(s_key, 4, 5));

  store->set(s_key, Variant(1.0), 1500, 0, APCHandleLevel::Outer);
  EXPECT_TRUE(store->cas(s_key, 1, 2));
  EXPECT_TRUE(store->cas(s_key, 2, 3));
  EXPECT_TRUE(store->cas(s_key, 3, 4));
  EXPECT_TRUE(store->cas(s_key, 4, 5));
  EXPECT_FALSE(store->cas(s_key, 4, 5));

  // make sure it doesn't work on some non-doubles/ints

  store->set(s_key, Variant(s_value2), 1500, 0, APCHandleLevel::Outer);
  EXPECT_EQ(store->inc(s_key, 1, found), 0);
  EXPECT_FALSE(found);
  EXPECT_FALSE(store->cas(s_key, 1, 2));
  store->eraseKey(s_key);
  EXPECT_EQ(store->inc(s_key, 1, found), 0);
  EXPECT_FALSE(found);
  EXPECT_FALSE(store->cas(s_key, 1, 2));
}

TEST(APC, SampleEntries) {
  auto store = new_store();
  // Empty store gives an empty sample.
  auto entries = store->sampleEntriesInfo(10);
  EXPECT_EQ(entries.size(), 0);
  // Single-element store results in repetition.
  store->set(s_foo, s_value1, 1500, 0, APCHandleLevel::Outer);
  for (uint32_t count = 0; count <= 10; ++count) {
    entries = store->sampleEntriesInfo(count);
    EXPECT_EQ(entries.size(), count);
    for (const auto& entry : entries) {
      EXPECT_STREQ(entry.key.c_str(), "foo");
    }
  }
  // More entries.
  store->set(s_key, s_value1, 1500, 0, APCHandleLevel::Outer);
  store->set(s_key2, s_value2, 1500, 0, APCHandleLevel::Outer);
  for (uint32_t count = 0; count <= 10; ++count) {
    entries = store->sampleEntriesInfo(count);
    EXPECT_EQ(entries.size(), count);
    for (const auto& entry : entries) {
      EXPECT_TRUE(entry.key == std::string("foo") ||
                  entry.key == std::string("key") ||
                  entry.key == std::string("key2"));
    }
  }
}

TEST(APC, SandboxJumpstartAPCOnlyOmitsPreloadAndJITPayloads) {
  folly::test::TemporaryDirectory temp{"sb-apc-only-profile"};
  auto const path = (temp.path() / "sb-apc.prof").native();
  auto const matchingKey = StaticString{"sb-apc-only-test:matching"};
  auto const ignoredKey = StaticString{"sb-apc-other:ignored"};

  auto const oldSerialize = Cfg::Eval::EnableSBProfSerialize;
  auto const oldAsyncJIT = Cfg::Eval::EnableAsyncJIT;
  auto const oldPrefixes = apcExtension::SerializePrefix;
  Cfg::Eval::EnableSBProfSerialize = true;
  Cfg::Eval::EnableAsyncJIT = true;
  apcExtension::SerializePrefix = {"sb-apc-only-test:"};
  SCOPE_EXIT {
    Cfg::Eval::EnableSBProfSerialize = oldSerialize;
    Cfg::Eval::EnableAsyncJIT = oldAsyncJIT;
    apcExtension::SerializePrefix = oldPrefixes;
  };

  EXPECT_TRUE(HHVM_FN(apc_store)(
    Variant{matchingKey}, Variant{s_value1}, 1500, 0
  ).toBoolean());
  EXPECT_TRUE(HHVM_FN(apc_store)(
    Variant{ignoredKey}, Variant{s_value2}, 1500, 0
  ).toBoolean());
  SCOPE_EXIT {
    HHVM_FN(apc_delete)(Variant{matchingKey});
    HHVM_FN(apc_delete)(Variant{ignoredKey});
  };

  auto const status = jit::serializeSBProfData(
    "", path, jit::SBWarmupFlags::Apc
  );
  ASSERT_NE(status.find("successful"), std::string::npos) << status;

  jit::ProfDataDeserializer des{path};
  EXPECT_EQ(jit::read_raw<uint32_t>(des), 0x4d564848);
  auto const extraDataOffset = jit::read_raw<off_t>(des);
  des.setExtraDataOffset(extraDataOffset);
  EXPECT_EQ(jit::read_raw<uint32_t>(des), 2);

  EXPECT_EQ(jit::read_raw<uint32_t>(des), 0);

  ASSERT_EQ(jit::read_raw<uint32_t>(des), 1);
  EXPECT_EQ(jit::read_cpp_string(des), "apc");
  auto const apcDataSize = jit::read_raw<uint32_t>(des);
  ASSERT_GT(apcDataSize, 0);
  std::string apcData(apcDataSize, '\0');
  jit::read_raw(des, apcData.data(), apcData.size());

  BlobDecoder apcDecoder{apcData.data(), apcData.size()};
  std::set<std::string> keys;
  apcDecoder.readWithLazyCount([&] {
    std::string_view key;
    apcDecoder(key);
    keys.emplace(key);
  });
  apcDecoder.assertDone();
  EXPECT_EQ(keys, std::set<std::string>{matchingKey.data()});

  EXPECT_EQ(jit::read_raw<uint32_t>(des), 0);
  EXPECT_TRUE(des.done());
}

TEST(APC, ContProfRestoresKeyNamesWithoutReplacingValues) {
  using namespace jit::cprof;
  auto const apc = ExtensionRegistry::get("apc");
  ASSERT_NE(nullptr, apc);
  auto const matchingKey = StaticString{"cont-prof-apc-test:matching"};
  auto const ignoredKey = StaticString{"cont-prof-apc-other:ignored"};
  auto const oldPrefixes = apcExtension::SerializePrefix;
  auto const oldWarmupData = apc->getWarmupData();
  apcExtension::SerializePrefix = {"cont-prof-apc-test:"};
  apc->setWarmupData(nullptr);
  SCOPE_EXIT {
    HHVM_FN(apc_delete)(Variant{matchingKey});
    HHVM_FN(apc_delete)(Variant{ignoredKey});
    apcExtension::SerializePrefix = oldPrefixes;
    if (auto const data = apc->getWarmupData()) DecRefSharedArray(data);
    apc->setWarmupData(oldWarmupData);
  };

  ASSERT_TRUE(HHVM_FN(apc_store)(
    Variant{matchingKey}, Variant{s_value1}, 1500, 0
  ).toBoolean());
  ASSERT_TRUE(HHVM_FN(apc_store)(
    Variant{ignoredKey}, Variant{s_value1}, 1500, 0
  ).toBoolean());
  auto const snapshot = ExtensionRegistry::serialize();
  auto const found = std::find_if(
    snapshot.begin(), snapshot.end(),
    [](auto const& data) { return data.name == "apc"; }
  );
  ASSERT_NE(snapshot.end(), found);
  ExtensionRegistry::SerializedExtensions const apcSnapshot{*found};

  ASSERT_TRUE(HHVM_FN(apc_store)(
    Variant{matchingKey}, Variant{s_value2}, 1500, 0
  ).toBoolean());
  auto expectedCounters = contProfRestoreCounters();
  prepareContProfStartupCandidates({}, 0, {}, apcSnapshot);
  ++expectedCounters["jit.cprof.extensions.restore_attempts"];
  ++expectedCounters["jit.cprof.extensions.restored_extensions"];
  EXPECT_EQ(expectedCounters, contProfRestoreCounters());
  auto const data = apc->getWarmupData();
  ASSERT_NE(nullptr, data);
  ASSERT_EQ(1, data->size());
  auto const key = data->get(int64_t{0});
  ASSERT_TRUE(isStringType(key.m_type));
  EXPECT_STREQ(matchingKey.data(), key.m_data.pstr->data());
  bool success = false;
  auto value = Variant::attach(HHVM_FN(apc_fetch)(Variant{matchingKey}, success));
  ASSERT_TRUE(success);
  EXPECT_TRUE(tvSame(*value.asTypedValue(),
    make_tv<KindOfPersistentString>(s_value2.get())));

  HHVM_FN(apc_delete)(Variant{matchingKey});
  prepareContProfStartupCandidates({}, 0, {}, apcSnapshot);
  ++expectedCounters["jit.cprof.extensions.restore_attempts"];
  ++expectedCounters["jit.cprof.extensions.restore_skipped_existing"];
  EXPECT_EQ(expectedCounters, contProfRestoreCounters());
  EXPECT_EQ(data, apc->getWarmupData());
  value = Variant::attach(HHVM_FN(apc_fetch)(Variant{matchingKey}, success));
  EXPECT_FALSE(success);
}

TEST(APC, ContProfDistinguishesEmptyRestoreFromInvalidInput) {
  using namespace jit::cprof;
  auto const apc = ExtensionRegistry::get("apc");
  ASSERT_NE(nullptr, apc);
  auto const oldWarmupData = apc->getWarmupData();
  apc->setWarmupData(nullptr);
  SCOPE_EXIT {
    if (auto const data = apc->getWarmupData()) DecRefSharedArray(data);
    apc->setWarmupData(oldWarmupData);
  };

  auto expectedCounters = contProfRestoreCounters();
  prepareContProfStartupCandidates({}, 0, {}, {{"apc", {}}});
  ++expectedCounters["jit.cprof.extensions.restore_attempts"];
  ++expectedCounters["jit.cprof.extensions.restore_failures"];
  EXPECT_EQ(expectedCounters, contProfRestoreCounters());
  EXPECT_EQ(nullptr, apc->getWarmupData());

  BlobEncoder encoder;
  encoder.lazyCount([] { return uint32_t{0}; });
  prepareContProfStartupCandidates({}, 0, {}, {{"apc", encoder.take()}});
  ++expectedCounters["jit.cprof.extensions.restore_attempts"];
  ++expectedCounters["jit.cprof.extensions.restored_extensions"];
  EXPECT_EQ(expectedCounters, contProfRestoreCounters());
  ASSERT_NE(nullptr, apc->getWarmupData());
  EXPECT_TRUE(apc->getWarmupData()->empty());
}

TEST(APC, ContProfSkipsUnavailableExtensionsWithoutDecoding) {
  using namespace jit::cprof;
  auto const apc = ExtensionRegistry::get("apc");
  ASSERT_NE(nullptr, apc);
  auto const oldWarmupData = apc->getWarmupData();
  auto const oldEnabled = apcExtension::Enable;
  apc->setWarmupData(nullptr);
  apcExtension::Enable = false;
  SCOPE_EXIT {
    apcExtension::Enable = oldEnabled;
    if (auto const data = apc->getWarmupData()) DecRefSharedArray(data);
    apc->setWarmupData(oldWarmupData);
  };

  auto expectedCounters = contProfRestoreCounters();
  prepareContProfStartupCandidates({}, 0, {}, {
    {"apc", {'x'}}, {"cont-prof-missing-extension", {'x'}}
  });
  expectedCounters["jit.cprof.extensions.restore_attempts"] += 2;
  expectedCounters["jit.cprof.extensions.restore_skipped_unavailable"] += 2;
  EXPECT_EQ(expectedCounters, contProfRestoreCounters());
  EXPECT_EQ(nullptr, apc->getWarmupData());
}

TEST(APC, ContProfRestoresServiceRouterWithCaptureDisabled) {
  using namespace jit::cprof;
  auto const sr = ExtensionRegistry::get("servicerouter");
  if (!sr) GTEST_SKIP() << "ServiceRouter extension is not linked";
  auto const oldCapture = Cfg::Jit::ContProfCaptureEnabled;
  auto const oldWarmupData = sr->getWarmupData();
  Cfg::Jit::ContProfCaptureEnabled = false;
  sr->setWarmupData(nullptr);
  SCOPE_EXIT {
    Cfg::Jit::ContProfCaptureEnabled = oldCapture;
    if (auto const data = sr->getWarmupData()) DecRefSharedArray(data);
    sr->setWarmupData(oldWarmupData);
  };

  BlobEncoder encoder;
  encoder.lazyCount([&] {
    encoder(std::string_view{"cont-prof.test-tier"});
    return uint32_t{1};
  });
  prepareContProfStartupCandidates(
    {}, 0, {}, {{"servicerouter", encoder.take()}}
  );
  auto const data = sr->getWarmupData();
  ASSERT_NE(nullptr, data);
  ASSERT_EQ(1, data->size());
  auto const tier = data->get(int64_t{0});
  ASSERT_TRUE(isStringType(tier.m_type));
  EXPECT_STREQ("cont-prof.test-tier", tier.m_data.pstr->data());
}

//////////////////////////////////////////////////////////////////////

}
