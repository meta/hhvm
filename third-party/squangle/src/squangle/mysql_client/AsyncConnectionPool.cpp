/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/ExceptionWrapper.h>
#include <folly/logging/xlog.h>

#include <folly/MapUtil.h>
#include <folly/Memory.h>
#include <folly/synchronization/Baton.h>
#include <memory>

#include "squangle/mysql_client/AsyncConnectionPool.h"
#include "squangle/mysql_client/AsyncMysqlClient.h"
#include "squangle/mysql_client/ConnectPoolOperation.h"
#include "squangle/mysql_client/SemiFutureAdapter.h"

namespace facebook::common::mysql_client {

std::shared_ptr<AsyncConnectionPool> AsyncConnectionPool::makePool(
    std::shared_ptr<AsyncMysqlClient> mysql_client,
    PoolOptions pool_options) {
  auto connectionPool = std::make_shared<AsyncConnectionPool>(
      std::move(mysql_client), std::move(pool_options));
  return connectionPool;
}

AsyncConnectionPool::AsyncConnectionPool(
    std::shared_ptr<AsyncMysqlClient> mysql_client,
    PoolOptions pool_options)
    : ConnectionPool<AsyncMysqlClient>(
          std::move(mysql_client),
          std::move(pool_options)),
      cleanup_timer_(mysql_client_->getEventBase(), conn_storage_) {
  if (!mysql_client_->runInThread([this]() {
        if (!cleanup_timer_.scheduleTimeout(PoolOptions::kCleanUpTimeout)) {
          LOG(DFATAL) << "Unable to arm pool cleanup; it will never run";
        }
      })) {
    LOG(DFATAL) << "Unable to schedule timeout due to event base issue";
  }
}

AsyncConnectionPool::~AsyncConnectionPool() {
  VLOG(2) << "Connection pool dying";

  if (!shutdown_data_.rlock()->finished_shutdown) {
    // shutdown() cancels every operation still queued, and cancelling one
    // completes it, which runs the caller's connect callback.  This is a
    // destructor, so that would end the process rather than the pool.
    if (auto ew = folly::try_and_catch([&] { shutdown(); })) {
      XLOG(ERR) << "Exception shutting down the connection pool: " << ew.what();
    }
  }

  VLOG(2) << "Connection pool shutdown completed";
}

void AsyncConnectionPool::shutdown() {
  VLOG(2) << "Shutting down";

  auto shutdown_func = [&](auto& shutdown_data) {
    cleanup_timer_.cancelTimeout();
    conn_storage_.clearAll();
    shutdown_data.finished_shutdown = true;
    VLOG(1) << "Shutting down in mysql_client thread";
  };

  // New scope to limit the lifetime of the write lock
  {
    auto shutdown_data = shutdown_data_.wlock();
    if (shutdown_data->shutting_down) {
      return;
    }

    // Will block adding anything to the pool
    shutdown_data->shutting_down = true;

    // cancelTimeout can only be ran in the mysql_client thread
    if (std::this_thread::get_id() == mysql_client_->threadId()) {
      shutdown_func(*shutdown_data);
      return;
    }
  }

  // We aren't already in the right thread, cause the shutdown to get run in the
  // correct thread and wait for it to complete.
  mysql_client_->runInThread(
      [&]() { shutdown_func(*shutdown_data_.wlock()); }, /*wait*/ true);
}

folly::SemiFuture<ConnectResult> AsyncConnectionPool::connectSemiFuture(
    const std::string& host,
    int port,
    const std::string& database_name,
    const std::string& user,
    const std::string& password,
    const ConnectionOptions& conn_opts) {
  return connectSemiFuture(
      host, port, database_name, user, password, "", conn_opts);
}

folly::SemiFuture<ConnectResult> AsyncConnectionPool::connectSemiFuture(
    const std::string& host,
    int port,
    const std::string& database_name,
    const std::string& user,
    const std::string& password,
    const std::string& special_tag,
    const ConnectionOptions& conn_opts) {
  auto op =
      beginConnection(host, port, database_name, user, password, special_tag);
  op->setConnectionOptions(conn_opts);
  return toSemiFuture(std::move(op));
}

std::unique_ptr<Connection> AsyncConnectionPool::connect(
    const std::string& host,
    int port,
    const std::string& database_name,
    const std::string& user,
    const std::string& password,
    const ConnectionOptions& conn_opts) {
  auto op = beginConnection(host, port, database_name, user, password);
  op->setConnectionOptions(conn_opts);
  // This will throw (intended behaviour) in case the operation didn't succeed
  return blockingConnectHelper(*op);
}

template <>
void AsyncConnectPoolOperationImpl::specializedRun() {
  std::weak_ptr<Operation> weakSelf = getOp().getSharedPointer();
  if (!conn().client().runInThread([&, wself = std::move(weakSelf)]() {
        // There is a race confition that allows a cancelled or completed
        // operation getting here. The self ptr check ensures that the client
        // has not freed the reference to the operation, and the state() check
        // verifies whether other relevant memebers have been cleaned up by
        // connect callbacks
        auto self = wself.lock();
        if (!self || (self->state() == OperationState::Completed)) {
          LOG(ERROR) << "ConnectPoolOperation freed before running";
          return;
        }

        // `this` is valid if `self` is still alive
        specializedRunImpl();
      })) {
    completeOperationInner(OperationResult::Failed);
  }
}

std::ostream& operator<<(std::ostream& os, ExpirationPolicy policy) {
  auto str = (policy == ExpirationPolicy::Age)
      ? "Age"
      : (policy == ExpirationPolicy::IdleTime ? "IdleTime" : "<invalid>");
  return os << str;
}

std::ostream& operator<<(std::ostream& os, const PoolOptions& options) {
  return os << "{per key limit:" << options.getPerKeyLimit()
            << ",pool limit:" << options.getPoolLimit()
            << ",idle timeout:" << options.getIdleTimeout().count()
            << "us,age timeout:" << options.getAgeTimeout().count()
            << "us,expiration policy:" << options.getExpPolicy()
            << ",pool per instance:" << options.poolPerMysqlInstance() << "}";
}

std::ostream& operator<<(std::ostream& os, const PoolKey& key) {
  return os << "{key:" << key.getConnectionKeyRef().getDisplayString()
            << ",options:" << key.getConnectionOptions().getDisplayString()
            << "}";
}

template <>
std::unique_ptr<ConnectPoolOperationImpl<AsyncMysqlClient>>
createConnectPoolOperationImpl(
    std::weak_ptr<ConnectionPool<AsyncMysqlClient>> pool,
    std::shared_ptr<AsyncMysqlClient> client,
    std::shared_ptr<const ConnectionKey> conn_key) {
  return std::make_unique<
      mysql_protocol::MysqlConnectPoolOperationImpl<AsyncMysqlClient>>(
      std::move(pool), client, std::move(conn_key));
}

} // namespace facebook::common::mysql_client
