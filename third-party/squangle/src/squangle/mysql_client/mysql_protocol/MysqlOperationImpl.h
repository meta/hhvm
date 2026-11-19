/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/Function.h>
#include <string_view>

#include "squangle/mysql_client/Operation.h"

namespace facebook::common::mysql_client::mysql_protocol {

class MysqlConnection;

// The abstract base for our available Operations.  Subclasses share
// intimate knowledge with the Operation class (most member variables
// are protected).
class MysqlOperationImpl : virtual public OperationBase,
                           public folly::EventHandler,
                           public folly::AsyncTimeout {
 public:
  // No public constructor.
  virtual ~MysqlOperationImpl() override = default;

  // copy and move not allowed
  MysqlOperationImpl(const MysqlOperationImpl&) = delete;
  MysqlOperationImpl& operator=(const MysqlOperationImpl&) = delete;

  MysqlOperationImpl(MysqlOperationImpl&&) = delete;
  MysqlOperationImpl& operator=(MysqlOperationImpl&&) = delete;

  Duration getMaxThreadBlockTime() {
    return max_thread_block_time_;
  }

  Duration getTotalThreadBlockTime() {
    return total_thread_block_time_;
  }

  void logThreadBlockTime(const folly::stop_watch<Duration> sw) {
    auto block_time = sw.elapsed();
    max_thread_block_time_ = std::max(max_thread_block_time_, block_time);
    total_thread_block_time_ += block_time;
  }

  static std::string connectStageString(connect_stage stage);

 protected:
  MysqlOperationImpl();

  // Must be called after the connection is set up to properly initialize
  // the EventHandler and AsyncTimeout with the correct event base.
  void initializeFromConnection();

  MysqlConnection* getMysqlConnection();
  const MysqlConnection* getMysqlConnection() const;

  static MysqlConnection* getMysqlConnection(InternalConnection* conn);
  static const MysqlConnection* getMysqlConnection(
      const InternalConnection* conn);

  // Called when an Operation needs to wait for the data to be readable or
  // writable (aka actionable).
  void waitForActionable();

  // Overridden in child classes and invoked when the status is actionable. This
  // function should either completeOperation or waitForActionable.
  virtual void actionable() = 0;

  // Runs `fn` on behalf of a noexcept libevent callback named `what`.  Logs
  // anything it throws, records it as the operation's error if the operation
  // does not already carry one, and completes the operation with
  // `failureResult`, so that swallowing the exception cannot leave the caller
  // waiting on an operation that will never complete.
  void runCallbackGuarded(
      std::string_view what,
      OperationResult failureResult,
      folly::FunctionRef<void()> fn) noexcept;

  // Runs `fn`, a task this operation queued with runInThread.  An async client
  // runs queued tasks from a noexcept EventBase runner, so there `fn` goes
  // through runCallbackGuarded.  A sync client runs the task inline on the
  // caller's thread, and its exceptions still reach the caller.
  void runQueuedTask(
      std::string_view what,
      OperationResult failureResult,
      folly::FunctionRef<void()> fn);

  // Takes the operation off the event base.  Virtual because a subclass may own
  // timers beyond the one AsyncTimeout this base knows about.
  virtual void detachFromEventBase();

  // How runCallbackGuarded finishes a failed callback.  Subclasses whose
  // normal exits pair completion with waking a parked owner have to override
  // this, so the recovery path honours the same contract as the rest of them.
  virtual void completeOperationFromCallbackFailure(OperationResult result);

  // EventHandler override
  void handlerReady(uint16_t /*events*/) noexcept override;

  // AsyncTimeout override
  void timeoutExpired() noexcept override;

  // Called by AsyncTimeout::timeoutExpired when the operation timed out
  void timeoutTriggered() override;

  // Our operation has completed.  During completeOperation,
  // specializedCompleteOperation is invoked for subclasses to perform
  // their own finalization (typically annotating errors and handling
  // timeouts).
  void completeOperation(OperationResult result);
  void completeOperationInner(OperationResult result);
  virtual void specializedTimeoutTriggered() = 0;
  virtual void specializedCompleteOperation() = 0;

  void protocolCompleteOperation(OperationResult result) override;

  bool isInEventBaseThread() const;
  bool isEventBaseSet() const;

  // This will contain the max block time of the thread
  Duration max_thread_block_time_ = Duration(0);
  Duration total_thread_block_time_ = Duration(0);

  // Friends because they need to access the query callbacks on this class
  template <typename Operation>
  friend folly::SemiFuture<folly::Unit> handlePreQueryCallback(Operation& op);
  template <typename ReturnType, typename Operation, typename QueryResult>
  friend void handleQueryCompletion(
      Operation& op,
      QueryResult query_result,
      QueryCallbackReason reason,
      folly::Promise<std::pair<ReturnType, AsyncPostQueryCallback>>& promise);

 private:
  // Restore folly::RequestContext and also invoke actionable()
  void invokeActionable();

  void completeQueuedOperation(OperationResult result);

  friend class Operation;
  friend class Connection;
  friend class ConnectOperation;
  friend class SyncConnection;
  friend class SyncConnectionPool;
};

} // namespace facebook::common::mysql_client::mysql_protocol
