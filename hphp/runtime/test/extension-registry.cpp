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

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <folly/testing/TestUtil.h>
#include <gtest/gtest.h>

#include "hphp/runtime/ext/apc/ext_apc.h"
#include "hphp/runtime/ext/extension-registry.h"
#include "hphp/runtime/vm/jit/prof-data-serialize.h"
#include "hphp/util/blob-encoder.h"

namespace HPHP {

Variant HHVM_FUNCTION(apc_delete, const Variant& key);

namespace {

struct ExtensionRegistryTest : testing::Test {
  void SetUp() override {
    apc = ExtensionRegistry::get("apc");
    ASSERT_NE(nullptr, apc);
    oldPrefixes = apcExtension::SerializePrefix;
    oldEnabled = apcExtension::Enable;
    oldWarmupData = apc->getWarmupData();
    apc->setWarmupData(nullptr);
    apcExtension::SerializePrefix = {"extension-registry-test:"};
    ASSERT_TRUE(HHVM_FN(apc_store)(Variant{key}, Variant{1}).toBoolean());
  }

  void TearDown() override {
    if (!apc) return;
    apcExtension::Enable = oldEnabled;
    HHVM_FN(apc_delete)(Variant{key});
    if (auto const data = apc->getWarmupData()) DecRefSharedArray(data);
    apc->setWarmupData(oldWarmupData);
    apcExtension::SerializePrefix = oldPrefixes;
  }

  ExtensionRegistry::SerializedExtension expectedRecord() const {
    BlobEncoder encoder;
    encoder.lazyCount([&] {
      encoder(std::string_view{key.data(), size_t(key.size())});
      return uint32_t{1};
    });
    return {"apc", encoder.take()};
  }

  OptString key{"extension-registry-test:key"};
  Extension* apc{nullptr};
  ArrayData* oldWarmupData{nullptr};
  std::vector<std::string> oldPrefixes;
  bool oldEnabled{true};
};

TEST_F(ExtensionRegistryTest, CapturesAllAndSupportsSandboxFilter) {
  auto const expected = expectedRecord();
  auto const all = ExtensionRegistry::serialize();
  auto const found = std::find_if(all.begin(), all.end(), [](auto const& data) {
    return data.name == "apc";
  });
  ASSERT_NE(all.end(), found);
  EXPECT_EQ(expected, *found);
  EXPECT_EQ(ExtensionRegistry::SerializedExtensions{expected},
            ExtensionRegistry::serialize("apc"));

  EXPECT_TRUE(ExtensionRegistry::serialize("extension-registry-missing").empty());
}

TEST_F(ExtensionRegistryTest, SkipsMissingAndDisabledWithoutDecoding) {
  ExtensionRegistry::SerializedExtension const invalid{"apc", {'x'}};
  EXPECT_FALSE(ExtensionRegistry::deserialize({
    "extension-registry-missing", {'x'}
  }));
  apcExtension::Enable = false;
  EXPECT_FALSE(ExtensionRegistry::deserialize(invalid));
  EXPECT_EQ(nullptr, apc->getWarmupData());
}

TEST_F(ExtensionRegistryTest, PreservesNativeFramingAndSkipsDisabledExtensions) {
  folly::test::TemporaryDirectory temp{"extension-registry-profile"};
  auto const path = (temp.path() / "jit.prof").native();
  auto const expected = expectedRecord();
  constexpr uint32_t sentinel = 0x12345678;
  {
    jit::ProfDataSerializer ser{path, jit::ProfDataSerializer::FileMode::Create};
    jit::write_raw(ser, uint32_t{0});
    jit::write_raw(ser, off_t{0});
    ExtensionRegistry::serialize(ser, "apc");
    jit::write_raw(ser, sentinel);
    ser.finalize();
  }
  {
    jit::ProfDataDeserializer des{path};
    jit::read_raw<uint32_t>(des);
    des.setExtraDataOffset(jit::read_raw<off_t>(des));
    ASSERT_EQ(1, jit::read_raw<uint32_t>(des));
    EXPECT_EQ(expected.name, jit::read_cpp_string(des));
    auto const size = jit::read_raw<uint32_t>(des);
    ASSERT_EQ(expected.payload.size(), size);
    std::vector<char> payload(size);
    jit::read_raw(des, payload.data(), payload.size());
    EXPECT_EQ(expected.payload, payload);
    EXPECT_EQ(sentinel, jit::read_raw<uint32_t>(des));
    EXPECT_TRUE(des.done());
  }
  apcExtension::Enable = false;
  {
    jit::ProfDataDeserializer des{path};
    jit::read_raw<uint32_t>(des);
    des.setExtraDataOffset(jit::read_raw<off_t>(des));
    ExtensionRegistry::deserialize(des);
    EXPECT_EQ(nullptr, apc->getWarmupData());
    EXPECT_EQ(sentinel, jit::read_raw<uint32_t>(des));
    EXPECT_TRUE(des.done());
  }
  apcExtension::Enable = oldEnabled;
  {
    jit::ProfDataDeserializer des{path};
    jit::read_raw<uint32_t>(des);
    des.setExtraDataOffset(jit::read_raw<off_t>(des));
    ExtensionRegistry::deserialize(des);
    auto const data = apc->getWarmupData();
    ASSERT_NE(nullptr, data);
    ASSERT_EQ(1, data->size());
    auto const restored = data->get(int64_t{0});
    ASSERT_TRUE(isStringType(restored.m_type));
    EXPECT_STREQ(key.data(), restored.m_data.pstr->data());
    EXPECT_EQ(sentinel, jit::read_raw<uint32_t>(des));
    EXPECT_TRUE(des.done());
  }
}

}
}
