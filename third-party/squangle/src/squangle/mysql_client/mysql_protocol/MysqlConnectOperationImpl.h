/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/polymorphic_cast.hpp>

#include "squangle/mysql_client/ConnectOperation.h"
#include "squangle/mysql_client/mysql_protocol/MysqlOperationImpl.h"

namespace facebook::common::mysql_client::mysql_protocol {

class MysqlConnectOperationImpl : public MysqlOperationImpl,
                                  virtual public ConnectOperationImpl {
 public:
  // Don't call this; it's public strictly for AsyncMysqlClient to be
  // able to call make_shared.
  MysqlConnectOperationImpl(
      MysqlClientBase* mysql_client,
      std::shared_ptr<const ConnectionKey> conn_key);
  virtual ~MysqlConnectOperationImpl() override;

  // The errno a timed-out connect reports. CR_SERVER_LOST unless the event
  // loop was stalled and the options ask for SQ_ERRNO_CONN_TIMEOUT_LOOP_STALLED
  // (see ConnectionOptions::enableLoopStalledConnectTimeoutErrno).
  static unsigned int timeoutErrno(
      bool loopStalled,
      const ConnectionOptions& options);

  // copy and move not allowed
  MysqlConnectOperationImpl(const MysqlConnectOperationImpl&) = delete;
  MysqlConnectOperationImpl& operator=(const MysqlConnectOperationImpl&) =
      delete;

  MysqlConnectOperationImpl(MysqlConnectOperationImpl&&) = delete;
  MysqlConnectOperationImpl& operator=(MysqlConnectOperationImpl&&) = delete;

  static constexpr Duration kMinimumViableConnectTimeout =
      std::chrono::microseconds(50);

  bool isActive() const override {
    return active_in_client_;
  }

 protected:
  virtual void attemptFailed(OperationResult result);
  virtual void attemptSucceeded(OperationResult result);

  virtual void specializedRun() override;
  void actionable() override;
  void specializedTimeoutTriggered() override;

  // The base takes down the operation's own AsyncTimeout; the per-attempt TCP
  // timer is ours, and is otherwise only cancelled on paths that complete
  // normally.
  void detachFromEventBase() override {
    MysqlOperationImpl::detachFromEventBase();
    tcp_timeout_handler_.cancelTimeout();
  }

  // specializedCompleteOperation() wakes the caller at its tail, behind
  // logConnectCompleted(), which calls a consumer-supplied logger and can
  // throw.  A throw there leaves the operation Completed -- so the recovery's
  // completeOperation() is a no-op -- with the connect callback never invoked
  // and the caller waiting forever.  Finish the wake-up here instead.
  //
  // completeOperation() reaches that same consumer code and can throw in turn,
  // so the wake is owed on both exits.  Not SCOPE_EXIT: its body is `noexcept`,
  // and wakeCallerOnce() runs the consumer's connect callback, so a throw there
  // would terminate instead of reaching runCallbackGuarded's own guard.
  // Rethrowing hands the completion failure back to that guard to log.
  void completeOperationFromCallbackFailure(OperationResult result) override {
    try {
      completeOperation(result);
    } catch (...) {
      wakeCallerOnce();
      throw;
    }
    wakeCallerOnce();
  }

  // Wakes whoever is waiting on this connect, at most once.  Protected so the
  // pooled subclass, which overrides the hook above, can still discharge it.
  //
  // The flag is set before the calls, which matters twice over: notify() is
  // not idempotent -- AsyncConnectionHelper::notify() logs DFATAL when the
  // operation is already actionable -- and callConnectCallback() only clears
  // connect_callback_ after the callback returns, so a callback that throws
  // would otherwise be invoked a second time.
  void wakeCallerOnce();
  void specializedCompleteOperation() override;

  // Called when tcp timeout is triggered
  void tcpConnectTimeoutTriggered();

  // Removes the Client ref, it can be called by child classes without needing
  // to add them as friend classes of AsyncMysqlClient
  virtual void removeClientReference();

  bool shouldCompleteOperation(OperationResult result);

  folly::ssl::SSLSessionUniquePtr getSSLSession();

  // Implementation of timeout handling for tcpTimeout and overall connect
  // timeout
  void timeoutHandler(bool isTcpTimeout, bool isPool = false);

 private:
  virtual void specializedRunImpl();

  void logConnectCompleted(OperationResult result);

  void maybeStoreSSLSession();

  bool isDoneWithTcpHandShake();

  const MysqlConnectionKey& getMysqlKeyRef() const {
    return boost::polymorphic_downcast<const MysqlConnectionKey&>(*conn_key_);
  }

  static int mysqlCertValidator(
      X509* server_cert,
      const void* context,
      const char** errptr);

  int flags_;

  bool active_in_client_;

  // Timeout used for controlling early timeout of just the tcp handshake phase
  // before doing heavy lifting like ssl and other mysql protocol for connection
  // establishment
  class ConnectTcpTimeoutHandler : public folly::AsyncTimeout {
   public:
    ConnectTcpTimeoutHandler(
        folly::EventBase* base,
        MysqlConnectOperationImpl* connect_operation)
        : folly::AsyncTimeout(base), op_(connect_operation) {}

    ConnectTcpTimeoutHandler() = delete;
    ~ConnectTcpTimeoutHandler() override = default;

    // copy and move not allowed
    ConnectTcpTimeoutHandler(const ConnectTcpTimeoutHandler&) = delete;
    ConnectTcpTimeoutHandler& operator=(const ConnectTcpTimeoutHandler&) =
        delete;

    ConnectTcpTimeoutHandler(ConnectTcpTimeoutHandler&&) = delete;
    ConnectTcpTimeoutHandler& operator=(ConnectTcpTimeoutHandler&&) = delete;

    void timeoutExpired() noexcept override {
      // timeoutExpired is `noexcept` so we can't throw from it, and swallowing
      // must not leave the caller waiting on an operation that never
      // completes.  runCallbackGuarded handles both.
      op_->runCallbackGuarded(
          "TCP connect timeoutExpired", OperationResult::TimedOut, [this] {
            op_->tcpConnectTimeoutTriggered();
          });
    }

   private:
    MysqlConnectOperationImpl* op_;
  };

  bool callerWoken_ = false;

  ConnectTcpTimeoutHandler tcp_timeout_handler_;

  friend class AsyncMysqlClient;
  friend class MysqlClientBase;
};

} // namespace facebook::common::mysql_client::mysql_protocol
