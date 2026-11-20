/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "squangle/mysql_client/ConnectionOptions.h"
#include "squangle/mysql_client/mysql_protocol/MysqlConnectOperationImpl.h"

namespace facebook::common::mysql_client::test {

namespace {

// MySQL's CR_SERVER_LOST, the errno a timed-out connect has always reported.
constexpr unsigned int kServerLost = 2013;
constexpr unsigned int kLoopStalled = static_cast<unsigned int>(
    SquangleErrno::SQ_ERRNO_CONN_TIMEOUT_LOOP_STALLED);

unsigned int timeoutErrno(bool loopStalled, bool reportLoopStalled) {
  ConnectionOptions options;
  if (reportLoopStalled) {
    options.enableLoopStalledConnectTimeoutErrno();
  }
  return mysql_protocol::MysqlConnectOperationImpl::timeoutErrno(
      loopStalled, options);
}

} // namespace

TEST(ConnectTimeoutErrnoTest, StalledLoopReportsItsOwnErrnoWhenEnabled) {
  EXPECT_EQ(
      timeoutErrno(/*loopStalled=*/true, /*reportLoopStalled=*/true),
      kLoopStalled);
}

TEST(ConnectTimeoutErrnoTest, HealthyLoopStillReportsServerLost) {
  EXPECT_EQ(
      timeoutErrno(/*loopStalled=*/false, /*reportLoopStalled=*/true),
      kServerLost);
}

TEST(ConnectTimeoutErrnoTest, StalledLoopReportsServerLostByDefault) {
  EXPECT_FALSE(ConnectionOptions().isEnableLoopStalledConnectTimeoutErrno());
  EXPECT_EQ(
      timeoutErrno(/*loopStalled=*/true, /*reportLoopStalled=*/false),
      kServerLost);
}

} // namespace facebook::common::mysql_client::test
