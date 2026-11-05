/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "squangle/mysql_client/PoolKey.h"
#include "squangle/mysql_client/PoolStorage.h"
#include "squangle/mysql_client/SyncConnectionPool.h"
#include "squangle/mysql_client/SyncMysqlClient.h"
#include "squangle/mysql_client/TwoLevelCache.h"
#include "squangle/mysql_client/test/MockConnection.h"

namespace facebook::common::mysql_client::test {

namespace {

constexpr std::pair<std::string_view, std::string_view> kAttributes[] = {
    {"role", "web"},
    {"caller", "DBQueryTest"},
    {"job", "none"},
    {"script", "test.php"},
};

ConnectionOptions optionsWithAttributes(bool reversed = false) {
  ConnectionOptions opts;
  const auto count = std::size(kAttributes);
  for (size_t i = 0; i < count; ++i) {
    const auto& [name, value] = kAttributes[reversed ? count - 1 - i : i];
    opts.setAttribute(name, std::string(value));
  }
  return opts;
}

PoolKey keyFor(ConnectionOptions opts) {
  return PoolKey(MockMysqlClient::createTestConnectionKey(), std::move(opts));
}

} // namespace

TEST(PoolKeyTest, AttributeOrderDoesNotChangeTheKey) {
  const auto forward = keyFor(optionsWithAttributes());
  const auto backward = keyFor(optionsWithAttributes(/* reversed */ true));

  EXPECT_EQ(forward.getOptionsHash(), backward.getOptionsHash());
  EXPECT_EQ(forward.getHash(), backward.getHash());
  EXPECT_EQ(forward.getPartialHash(), backward.getPartialHash());
  EXPECT_TRUE(forward == backward);
  EXPECT_TRUE(forward.partialCompare(backward));
}

// A debug build of F14 puts each insert in a random slot, so there two maps
// filled in the same order can still iterate in different orders.
TEST(PoolKeyTest, SameAttributesAlwaysGiveTheSameKey) {
  const auto first = keyFor(optionsWithAttributes());
  int different = 0;
  for (int i = 0; i < 1000; ++i) {
    if (first != keyFor(optionsWithAttributes())) {
      ++different;
    }
  }
  EXPECT_EQ(different, 0) << "of 1000 keys built from the same attributes";
}

TEST(PoolKeyTest, DifferentAttributesGiveDifferentKeys) {
  const auto base = keyFor(optionsWithAttributes());

  EXPECT_TRUE(base != keyFor(ConnectionOptions()));
  EXPECT_TRUE(
      base != keyFor(optionsWithAttributes().setAttribute("job", "other")));
  EXPECT_TRUE(
      base != keyFor(optionsWithAttributes().setAttribute("extra", "1")));

  // The same names and the same values, paired differently.
  ConnectionOptions one;
  one.setAttribute("a", "1").setAttribute("b", "2");
  ConnectionOptions other;
  other.setAttribute("a", "2").setAttribute("b", "1");
  EXPECT_TRUE(keyFor(one) != keyFor(other));
}

TEST(PoolKeyTest, CryptoAuthTokensMustMatchExactly) {
  auto withTokens = [](const std::string& tokens) {
    return keyFor(optionsWithAttributes().setCryptoAuthTokenList(tokens));
  };
  const auto base = withTokens("token-a");

  EXPECT_TRUE(base == withTokens("token-a"));
  EXPECT_TRUE(base.partialCompare(withTokens("token-a")));

  for (const auto& other :
       {keyFor(optionsWithAttributes()),
        withTokens("token-b"),
        withTokens("")}) {
    EXPECT_TRUE(base != other);
    EXPECT_FALSE(base.partialCompare(other));
    EXPECT_FALSE(other.partialCompare(base));
  }
}

TEST(PoolKeyTest, StorageFindsAWaitingOperationByAnEqualKey) {
  auto pool = SyncConnectionPool::makePool(std::make_shared<SyncMysqlClient>());
  const auto connKey = MockMysqlClient::createTestConnectionKey();
  auto op = std::dynamic_pointer_cast<ConnectPoolOperation<SyncMysqlClient>>(
      pool->beginConnection(connKey));
  ASSERT_NE(op, nullptr);

  PoolStorageData<SyncMysqlClient> storage(
      /* conn_limit */ 10, /* max_idle_time */ std::chrono::seconds(60));
  storage.queueOperation(PoolKey(connKey, optionsWithAttributes()), op);

  const PoolKey equalKey(connKey, optionsWithAttributes(/* reversed */ true));
  EXPECT_EQ(storage.popOperation(equalKey), op);
}

// Idle connections wait in a TwoLevelCache, which also groups keys by
// PoolKeyPartialHash.
TEST(PoolKeyTest, CacheFindsAnIdleEntryByAnEqualKey) {
  struct Value {};
  TwoLevelCache<
      SyncMysqlClient,
      PoolKey,
      std::unique_ptr<Value>,
      PoolKeyHash,
      PoolKeyPartialHash>
      cache;
  const auto connKey = MockMysqlClient::createTestConnectionKey();
  cache.push(
      PoolKey(connKey, optionsWithAttributes()),
      std::make_unique<Value>(),
      /* max */ 10);

  const PoolKey equalKey(connKey, optionsWithAttributes(/* reversed */ true));
  EXPECT_EQ(cache.level2Size(equalKey), 1);
  EXPECT_NE(cache.popLevel1(equalKey), nullptr);
}

} // namespace facebook::common::mysql_client::test
