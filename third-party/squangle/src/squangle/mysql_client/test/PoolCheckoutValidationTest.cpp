/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

//
// Checkout validation tests
//
// By default (PoolOptions::validateOnCheckout), a cached connection is handed
// out only if it is within its idle/age limit and its socket reports nothing.
// These tests drive the pool's cache directly with mock connections; no MySQL
// instance is involved.
//

#include <gtest/gtest.h>

#include "squangle/mysql_client/SyncConnectionPool.h"
#include "squangle/mysql_client/SyncMysqlClient.h"
#include "squangle/mysql_client/test/MockConnection.h"

namespace facebook::common::mysql_client::test {

namespace {

class CheckoutTestPool : public SyncConnectionPool {
 public:
  using SyncConnectionPool::SyncConnectionPool;

  void cache(std::unique_ptr<MysqlPooledHolder<SyncMysqlClient>> conn) {
    conn_storage_.queueConnection(std::move(conn));
  }

  std::unique_ptr<MysqlPooledHolder<SyncMysqlClient>> checkout(
      const PoolKey& key,
      bool any_database = false) {
    return popCachedConnection(key, any_database);
  }
};

class PoolCheckoutValidationTest : public ::testing::Test {
 protected:
  void TearDown() override {
    ConnectionPoolBase::setCheckoutValidationKilled(false);
  }

  std::shared_ptr<CheckoutTestPool> makePool(PoolOptions options) {
    return std::make_shared<CheckoutTestPool>(client_, std::move(options));
  }

  std::unique_ptr<MysqlPooledHolder<SyncMysqlClient>> makeConn(
      const std::shared_ptr<CheckoutTestPool>& pool,
      bool unusable,
      MockInternalConnection** mock = nullptr) {
    MockConnectionConfig config;
    config.unusableOnCheckout = unusable;
    auto internal = std::make_unique<MockInternalConnection>(config);
    if (mock != nullptr) {
      *mock = internal.get();
    }
    auto holder =
        std::make_unique<ConnectionHolder>(*client_, std::move(internal), key_);
    return std::make_unique<MysqlPooledHolder<SyncMysqlClient>>(
        std::move(holder), pool, poolKey());
  }

  PoolKey poolKey() const {
    return PoolKey(key_, ConnectionOptions());
  }

  std::shared_ptr<SyncMysqlClient> client_ =
      std::make_shared<SyncMysqlClient>();
  std::shared_ptr<const ConnectionKey> key_ =
      MockMysqlClient::createTestConnectionKey();
};

} // namespace

TEST_F(PoolCheckoutValidationTest, IdleAndAgeLimitsAreEnforcedAtCheckoutTime) {
  auto pool = makePool(PoolOptions());
  auto conn = makeConn(pool, false);
  const auto now = std::chrono::steady_clock::now();

  EXPECT_FALSE(
      PoolStorageData<SyncMysqlClient>::isExpired(
          *conn, std::chrono::hours(1), now));
  EXPECT_TRUE(
      PoolStorageData<SyncMysqlClient>::isExpired(
          *conn, Duration::zero(), now + std::chrono::milliseconds(1)));

  conn->setLifeDuration(std::chrono::milliseconds(1));
  EXPECT_TRUE(
      PoolStorageData<SyncMysqlClient>::isExpired(
          *conn, std::chrono::hours(1), now + std::chrono::milliseconds(2)));
}

TEST_F(PoolCheckoutValidationTest, UnusableConnectionIsDiscarded) {
  auto pool = makePool(PoolOptions());
  pool->cache(makeConn(pool, true));

  EXPECT_EQ(pool->checkout(poolKey()), nullptr);
  EXPECT_EQ(pool->stats()->numValidationDiscards(), 1);
  EXPECT_EQ(pool->stats()->numDestroyedPoolConnections(), 1);
}

TEST_F(PoolCheckoutValidationTest, DiscardContinuesToNextUsableConnection) {
  auto pool = makePool(PoolOptions());
  pool->cache(makeConn(pool, true));
  MockInternalConnection* healthy = nullptr;
  pool->cache(makeConn(pool, false, &healthy));

  auto conn = pool->checkout(poolKey());
  ASSERT_NE(conn, nullptr);
  EXPECT_EQ(&conn->getInternalConnection(), healthy);
  EXPECT_EQ(pool->stats()->numValidationDiscards(), 1);
}

TEST_F(PoolCheckoutValidationTest, ChangeUserCheckoutIsValidatedToo) {
  auto pool = makePool(PoolOptions());
  pool->cache(makeConn(pool, true));

  EXPECT_EQ(pool->checkout(poolKey(), /* any_database */ true), nullptr);
  EXPECT_EQ(pool->stats()->numValidationDiscards(), 1);
}

TEST_F(PoolCheckoutValidationTest, DisabledValidationNeverInspectsTheSocket) {
  auto pool = makePool(PoolOptions().setValidateOnCheckout(false));
  MockInternalConnection* mock = nullptr;
  pool->cache(makeConn(pool, true, &mock));

  auto conn = pool->checkout(poolKey());
  ASSERT_NE(conn, nullptr);
  EXPECT_EQ(mock->checkoutValidations(), 0);
  EXPECT_EQ(pool->stats()->numValidationDiscards(), 0);
}

TEST_F(PoolCheckoutValidationTest, KillSwitchOverridesThePoolOption) {
  auto pool = makePool(PoolOptions());
  MockInternalConnection* mock = nullptr;
  pool->cache(makeConn(pool, true, &mock));

  ConnectionPoolBase::setCheckoutValidationKilled(true);
  auto conn = pool->checkout(poolKey());
  ASSERT_NE(conn, nullptr);
  EXPECT_EQ(mock->checkoutValidations(), 0);

  ConnectionPoolBase::setCheckoutValidationKilled(false);
  pool->cache(std::move(conn));
  EXPECT_EQ(pool->checkout(poolKey()), nullptr);
  EXPECT_EQ(pool->stats()->numValidationDiscards(), 1);
}

} // namespace facebook::common::mysql_client::test
