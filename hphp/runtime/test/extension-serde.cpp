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
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <folly/ScopeGuard.h>
#include <gtest/gtest.h>

#include "hphp/runtime/base/array-data.h"
#include "hphp/runtime/base/tv-shared.h"
#include "hphp/runtime/ext/extension-registry.h"
#include "hphp/util/blob-encoder.h"

namespace HPHP {
namespace {

TEST(ExtensionSerde, RestoresEmptyAndNonemptyKeyLists) {
  auto const apc = ExtensionRegistry::get("apc");
  ASSERT_NE(nullptr, apc);
  auto const oldWarmup = apc->getWarmupData();
  apc->setWarmupData(nullptr);
  SCOPE_EXIT {
    if (auto const data = apc->getWarmupData()) DecRefSharedArray(data);
    apc->setWarmupData(oldWarmup);
  };

  for (auto const& keys : std::vector<std::vector<std::string>>{
    {}, {"extension-serde:first", "extension-serde:second"},
  }) {
    BlobEncoder encoder;
    encoder.lazyCount([&] {
      for (auto const& key : keys) encoder(key);
      return static_cast<uint32_t>(keys.size());
    });
    ASSERT_TRUE(ExtensionRegistry::deserialize({"apc", encoder.take()}));
    auto const data = apc->getWarmupData();
    ASSERT_NE(nullptr, data);
    ASSERT_EQ(keys.size(), data->size());
    for (size_t i = 0; i < keys.size(); ++i) {
      auto const value = data->get(static_cast<int64_t>(i));
      ASSERT_TRUE(isStringType(value.m_type));
      EXPECT_EQ(keys[i], value.m_data.pstr->toCppString());
    }
    DecRefSharedArray(data);
    apc->setWarmupData(nullptr);
  }
}

TEST(ExtensionSerde, RejectsMalformedKeyListsBeforeReplacingWarmupData) {
  auto const apc = ExtensionRegistry::get("apc");
  ASSERT_NE(nullptr, apc);
  auto const oldWarmup = apc->getWarmupData();
  apc->setWarmupData(nullptr);
  SCOPE_EXIT {
    if (auto const data = apc->getWarmupData()) DecRefSharedArray(data);
    apc->setWarmupData(oldWarmup);
  };

  BlobEncoder encoder;
  encoder.lazyCount([&] {
    encoder(std::string_view{"extension-serde:retained"});
    return uint32_t{1};
  });
  auto const payload = encoder.take();
  ASSERT_TRUE(ExtensionRegistry::deserialize({"apc", payload}));
  auto const retained = apc->getWarmupData();
  ASSERT_NE(nullptr, retained);

  for (size_t size = 0; size < payload.size(); ++size) {
    std::vector<char> const truncated{payload.begin(), payload.begin() + size};
    EXPECT_THROW(
      ExtensionRegistry::deserialize({"apc", truncated}), std::runtime_error
    ) << size;
    EXPECT_EQ(retained, apc->getWarmupData());
  }

  auto invalidCount = payload;
  std::fill_n(invalidCount.begin(), sizeof(uint32_t), char{0x7f});
  auto invalidVarint = payload;
  invalidVarint.resize(sizeof(uint32_t));
  invalidVarint.insert(invalidVarint.end(), 10, static_cast<char>(0x80));
  auto trailing = payload;
  trailing.push_back(0);
  for (auto const& invalid : {invalidCount, invalidVarint, trailing}) {
    EXPECT_THROW(
      ExtensionRegistry::deserialize({"apc", invalid}), std::runtime_error
    );
    EXPECT_EQ(retained, apc->getWarmupData());
  }
}

}
}
