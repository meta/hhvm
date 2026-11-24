/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

//
// Pool connect timeouts while the client's event loop is stalled
//
// A pooled connect that times out reports SQ_ERRNO_POOL_CONN_TIMEOUT whether
// the pool was short of connections or the event loop was too stalled to hand
// one over. Callers retry on that errno, so it stays the same in both cases;
// the stall is reported through incrPoolConnTimeoutsLoopStalled() instead.
//
// No MySQL instance is involved: the pool allows no connections per key, so
// the operation waits in the pool until it times out without opening a socket.
//

#include <gtest/gtest.h>

#include "squangle/mysql_client/SyncConnectionPool.h"
#include "squangle/mysql_client/SyncMysqlClient.h"

namespace facebook::common::mysql_client::test {

namespace {

// Reports a fixed average callback delay, which is how the timeout path decides
// whether the event loop was stalled.
class FixedCallbackDelayClient : public SyncMysqlClient {
 public:
  explicit FixedCallbackDelayClient(Duration callbackDelay)
      : callbackDelay_(callbackDelay) {}

  Duration callbackDelayAvg() const override {
    return callbackDelay_;
  }

 private:
  const Duration callbackDelay_;
};

struct PoolTimeoutOutcome {
  unsigned int mysqlErrno;
  uint64_t loopStalledTimeouts;
};

PoolTimeoutOutcome connectToFullPool(Duration callbackDelay) {
  auto client = std::make_shared<FixedCallbackDelayClient>(callbackDelay);
  auto pool =
      SyncConnectionPool::makePool(client, PoolOptions().setPerKeyLimit(0));

  unsigned int mysqlErrno = 0;
  try {
    pool->connect(
        "localhost",
        3306,
        "test_db",
        "test_user",
        "",
        ConnectionOptions().setTimeout(std::chrono::milliseconds(10)));
    ADD_FAILURE() << "a pool with no connection slots handed one out";
  } catch (const MysqlException& ex) {
    mysqlErrno = ex.mysql_errno();
  }

  auto* stats = dynamic_cast<db::SimpleDbCounter*>(client->stats());
  return PoolTimeoutOutcome{
      .mysqlErrno = mysqlErrno,
      .loopStalledTimeouts =
          stats != nullptr ? stats->numPoolConnTimeoutsLoopStalled() : 0,
  };
}

constexpr auto kPoolConnTimeout =
    static_cast<unsigned int>(SquangleErrno::SQ_ERRNO_POOL_CONN_TIMEOUT);

} // namespace

TEST(PoolConnTimeoutLoopStalledTest, StalledLoopIsCountedAndKeepsTheErrno) {
  auto outcome = connectToFullPool(std::chrono::milliseconds(100));

  EXPECT_EQ(outcome.mysqlErrno, kPoolConnTimeout);
  EXPECT_EQ(outcome.loopStalledTimeouts, 1);
}

TEST(PoolConnTimeoutLoopStalledTest, HealthyLoopIsNotCounted) {
  auto outcome = connectToFullPool(Duration(0));

  EXPECT_EQ(outcome.mysqlErrno, kPoolConnTimeout);
  EXPECT_EQ(outcome.loopStalledTimeouts, 0);
}

} // namespace facebook::common::mysql_client::test
