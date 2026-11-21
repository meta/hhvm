/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

//
// A throw out of work that AsyncMysqlClient queues on its EventBase must fail
// the operation, not terminate the process: folly runs queued functions
// noexcept.
//

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <folly/synchronization/Baton.h>

#include <stdexcept>

#include "squangle/mysql_client/AsyncConnectionPool.h"
#include "squangle/mysql_client/AsyncMysqlClient.h"
#include "squangle/mysql_client/Connection.h"
#include "squangle/mysql_client/QueryOperation.h"
#include "squangle/mysql_client/test/MockInternalConnection.h"

namespace facebook::common::mysql_client {
namespace {

[[noreturn]] bool throwingPostRenderCallback(std::string_view /*rendered*/) {
  throw std::runtime_error("consumer callback failed");
}

[[noreturn]] void throwingQueryCallback(
    QueryOperation& /*op*/,
    QueryResult* /*result*/,
    QueryCallbackReason /*reason*/) {
  throw std::runtime_error("consumer callback failed");
}

[[noreturn]] void throwingConnectCallback(ConnectOperation& /*op*/) {
  throw std::runtime_error("consumer callback failed");
}

// An AsyncConnection backed by a mock, so queries need no server.
class MockAsyncConnection : public AsyncConnection {
 public:
  using AsyncConnection::AsyncConnection;

 protected:
  std::unique_ptr<InternalConnection> createInternalConnection() override {
    return std::make_unique<test::MockInternalConnection>();
  }
};

class EventBaseTaskExceptionTest : public ::testing::Test {
 protected:
  std::unique_ptr<Connection> makeConnection() {
    std::unique_ptr<Connection> conn = std::make_unique<MockAsyncConnection>(
        client_,
        std::make_shared<MysqlConnectionKey>("localhost", 3306, "db", "user"));
    client_.getEventBase()->runInEventBaseThreadAndWait(
        [&] { conn->initialize(); });
    return conn;
  }

  AsyncMysqlClient client_;
};

TEST_F(EventBaseTaskExceptionTest, ThrowFromQueuedRunFailsTheOperation) {
  auto conn = makeConnection();
  conn->setPostRenderCallback(throwingPostRenderCallback);
  auto op = Connection::beginQuery(std::move(conn), Query("SELECT 1"));

  op->run();
  op->wait();

  EXPECT_EQ(op->result(), OperationResult::Failed);
  EXPECT_EQ(
      op->mysql_errno(),
      static_cast<unsigned int>(SquangleErrno::SQ_INTERNAL_ERROR));
  EXPECT_THAT(op->mysql_error(), ::testing::HasSubstr("consumer callback"));
}

TEST_F(
    EventBaseTaskExceptionTest,
    ResumeAfterCompletionDoesNotUseFreedOperation) {
  auto op = Connection::beginQuery(makeConnection(), Query("SELECT 1"));

  // Cancelling before run() makes run() queue the completion on the
  // EventBase, so the operation is completed by the time wait() returns.
  op->cancel();
  op->run();
  op->wait();
  EXPECT_EQ(op->result(), OperationResult::Cancelled);

  // Retire the operation: completeOperation defers removal to a task
  // queued on the EventBase, and draining the EventBase here runs it,
  // dropping the client's reference to the operation.
  client_.getEventBase()->runInEventBaseThreadAndWait([] {});

  // A resume queued after completion must not dereference the operation
  // once the caller's reference is released: the destructor of
  // XdbStreamQueryRunner resumes unconditionally and then drops its
  // reference, and the queued resume used to crash in
  // MysqlOperationImpl::runQueuedTask dereferencing conn_proxy_.
  op->resume();
  op.reset();

  // Drain the EventBase so the queued resume runs inside this test.
  client_.getEventBase()->runInEventBaseThreadAndWait([] {});
}

TEST_F(EventBaseTaskExceptionTest, ThrowFromQueuedCompletionIsContained) {
  auto op = Connection::beginQuery(makeConnection(), Query("SELECT 1"));
  op->setCallback(throwingQueryCallback);

  // Cancelling before run() makes run() queue the completion on the EventBase.
  op->cancel();
  op->run();
  op->wait();

  EXPECT_EQ(op->result(), OperationResult::Cancelled);
}

TEST(EventBaseTaskExceptionPoolTest, ThrowFromQueuedPoolConnectRunIsContained) {
  std::shared_ptr<AsyncMysqlClient> client(
      new AsyncMysqlClient(), AsyncMysqlClient::deleter);
  auto pool = AsyncConnectionPool::makePool(client, PoolOptions());
  auto op = pool->beginConnection("localhost", 3306, "db", "user", "password");
  op->setTimeout(Duration(1));
  op->setCallback(throwingConnectCallback);

  // Hold the EventBase until after run() has queued its task, so the 1us
  // connect timeout has passed when that task starts; it then times out and
  // calls the callback.
  folly::Baton<> release;
  client->getEventBase()->runInEventBaseThread([&] { release.wait(); });
  op->run();
  release.post();
  op->wait();

  EXPECT_EQ(op->result(), OperationResult::TimedOut);
}

} // namespace
} // namespace facebook::common::mysql_client
