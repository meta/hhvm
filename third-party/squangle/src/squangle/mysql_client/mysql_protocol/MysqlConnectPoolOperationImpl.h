/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/polymorphic_cast.hpp>
#include <atomic>

#include "squangle/mysql_client/ConnectPoolOperation.h"
#include "squangle/mysql_client/mysql_protocol/MysqlConnectOperationImpl.h"

namespace facebook::common::mysql_client::mysql_protocol {

template <typename Client>
class MysqlConnectPoolOperationImpl : public MysqlConnectOperationImpl,
                                      public ConnectPoolOperationImpl<Client> {
 public:
  // Don't call this; it's public strictly for ConnectionPool to be able to call
  // make_shared.
  MysqlConnectPoolOperationImpl(
      std::weak_ptr<ConnectionPool<Client>> pool,
      std::shared_ptr<Client> client,
      std::shared_ptr<const ConnectionKey> conn_key)
      : OperationBase(
            std::make_unique<OperationBase::OwnedConnection>(
                client->createConnection(conn_key))),
        ConnectOperationImpl(client.get(), conn_key),
        MysqlConnectOperationImpl(client.get(), std::move(conn_key)),
        pool_(pool) {}

  ConnectPoolOperation<Client>& getConnectPoolOp() const {
    return *boost::polymorphic_downcast<ConnectPoolOperation<Client>*>(op_);
  }

 protected:
  void specializedRun() override;

  void specializedTimeoutTriggered() override {
    if (auto locked_pool = pool_.lock(); locked_pool) {
      auto& op = getConnectPoolOp();
      op.cancelPreOperation();

      // Check if the timeout happened because of the host is being slow or the
      // pool is lacking resources
      auto pool_key = PoolKey(op.getKey(), getConnectionOptions());
      auto key_stats = locked_pool->getPoolKeyStats(pool_key);
      auto num_open = key_stats.open_connections;
      auto num_opening = key_stats.pending_connections;

      // As a way to be realistic regarding the reason a connection was not
      // obtained, we start from the principle that this is pool's fault.
      // We can only blame the host (by forwarding 2013) if we have no
      // open connections and none trying to be open.
      // The second rule is applied where the resource restriction is so small
      // that the pool can't even try to open a connection.
      if (!(num_open == 0 &&
            (num_opening > 0 ||
             locked_pool->canCreateMoreConnections(pool_key)))) {
        auto location = fmt::format(
            "in pool (open {}, opening {}, key limit {})",
            key_stats.open_connections,
            key_stats.pending_connections,
            locked_pool->perKeyLimit());

        bool loop_stalled = false;
        auto errorStr = generateTimeoutError(
            opElapsedMs(),
            [&loop_stalled](bool stalled) {
              loop_stalled = stalled;
              return static_cast<uint16_t>(
                  SquangleErrno::SQ_ERRNO_POOL_CONN_TIMEOUT);
            },
            "Connection",
            std::move(location),
            std::nullopt);
        if (loop_stalled) {
          client().stats()->incrPoolConnTimeoutsLoopStalled();
        }

        op.setAsyncClientError(
            static_cast<uint16_t>(SquangleErrno::SQ_ERRNO_POOL_CONN_TIMEOUT),
            std::move(errorStr));
        attemptFailed(OperationResult::TimedOut);
        return;
      }
    }

    MysqlConnectOperationImpl::timeoutHandler(false, true);
  }

  // Completion in this class must always pair with signalWaiter(), and the
  // recovery path is no different: completeOperation() can throw from consumer
  // callbacks, and skipping the post leaves the owner asleep.  syncWait() is a
  // timed wait, so the cost is the rest of the connect timeout and a spurious
  // failure rather than a permanent hang -- still the failure this override
  // exists to prevent.
  //
  // handoff_ is left as it is.  A connectionCallback() arriving after this
  // finds handoff_ already out of Waiting, so its CAS fails and it completes
  // inline through attemptSucceeded(), which is a no-op on an already-Completed
  // operation.  Nothing is left undone either way.
  void completeOperationFromCallbackFailure(OperationResult result) override {
    // The connect wake-up is owed on both branches.  It does not touch baton_,
    // so the race that gates signalWaiter() below does not apply to it, and the
    // state this override returns early on is exactly the one the base class's
    // wake-up exists for -- a throw out of specializedCompleteOperation after
    // Completed was already set.
    //
    // signalWaiter() only when this call is the one completing the operation.
    // Once it is already Completed the owner may have woken and run
    // cleanupWait(), which resets baton_ on its own thread, so posting from
    // here would race that reset.
    //
    // Skipping the post cannot strand an owner, but not for the reason it
    // first looks like.  "Already Completed" does not imply "already
    // signalled": everywhere else in this class the two are separate
    // statements, and completeOperation() reaches a consumer-supplied logger
    // that can throw between them.  It is safe because no baton is armed on
    // any path that reaches here -- prepWait() is called only by
    // SyncConnectionPool, and SyncMysqlClient has no event base, so it never
    // attaches the handler or the timeout that drive runCallbackGuarded.
    if (state() == OperationState::Completed) {
      // Skipping the post is only safe while that invariant holds.  DFATAL
      // rather than DCHECK: this is a reachability claim spanning several
      // classes rather than a local invariant, so a change elsewhere could
      // break it, and release builds are where the signal would be needed.
      if (baton_) {
        LOG(DFATAL) << "pool connect already completed with a waiter armed; "
                       "the owner will wait out its timeout";
      }
      wakeCallerOnce();
      return;
    }

    // Wake before posting, matching every path in this class that completes
    // inline.  Were an owner ever parked here, the post would release it to run
    // completeDeferred(), so the wake has to come first.  Only the wake, note:
    // runCallbackGuarded still detaches and retires this operation after this
    // returns, so the post is not the last thing to touch it.
    //
    // Not SCOPE_EXIT, for the same reason as the base class: its body is
    // `noexcept` and wakeCallerOnce() runs a consumer callback.
    try {
      completeOperation(result);
    } catch (...) {
      dischargeWaiters();
      throw;
    }
    dischargeWaiters();
  }

  void attemptFailed(OperationResult result) override {
    ++attempts_made_;
    if (shouldCompleteOperation(result)) {
      completeOperation(result);
      signalWaiter();
      return;
    }

    unregisterHandler();
    cancelTimeout();

    // Adjust timeout
    Duration timeout_attempt_based =
        getConnectionOptions().getTimeout() + opElapsedMs();

    setTimeoutInternal(
        min(timeout_attempt_based, getConnectionOptions().getTotalTimeout()));

    // A retry runs prepWait() again, which replaces the baton. If an owner is
    // parked on the current one that would destroy it underneath them and
    // leave them unwoken, so hand the retry back instead and let them run it
    // on their own thread. Same rule as the completion: Waiting is the only
    // state in which somebody is coming back for this operation.
    auto expected = Handoff::Waiting;
    if (handoff_.compare_exchange_strong(expected, Handoff::RetryPending)) {
      signalWaiter();
      return;
    }

    specializedRun();
  }

 private:
  void specializedRunImpl() override {
    // Initialize EventHandler/AsyncTimeout now that we're in the event base
    // thread. This only needs to be done on the first attempt.
    if (attempts_made_ == 0) {
      initializeFromConnection();
      conn().initialize(false);
    }

    withOptionalConnectionContext([&](auto& connection_context) {
      conn_options_.withPossibleSSLOptionsProvider(
          [&](const auto& /*provider*/) {
            connection_context.isSslConnection = true;
          });
    });

    // Set timeout for waiting for connection
    auto elapsed = OperationBase::opElapsed();
    if (elapsed >= getTimeout()) {
      timeoutTriggered();
      return;
    }

    if constexpr (uses_one_thread_v<Client>) {
      scheduleTimeout(
          std::chrono::duration_cast<Millis>(getTimeout() - elapsed).count());
    }

    // Remove before to not count against itself
    removeClientReference();

    if (auto shared_pool = pool_.lock(); shared_pool) {
      // Sync attributes in conn_options_ with the Operation::attributes_ value
      // as pool key uses the attributes from ConnectionOptions
      conn_options_.setAttributes(getAttributes());
      shared_pool->registerForConnection(&getConnectPoolOp());
    } else {
      VLOG(2) << "Pool is gone, operation must cancel";
      cancel();
    }
  }

  // Called when the connection is matched by the pool client
  void connectionCallback(
      std::unique_ptr<MysqlPooledHolder<Client>> pooled_conn) override {
    // TODO: validate we are in the correct thread (for async)

    // Every exit from here must signalWaiter(): a waiting owner thread that
    // has already timed out relies on the post to know the handoff is over
    // (see SyncConnectionPool::openNewConnectionFinish).
    if (!pooled_conn) {
      LOG(DFATAL) << "Unexpected error";
      completeOperation(OperationResult::Failed);
      signalWaiter();
      return;
    }

    if (mysql_errno()) {
      LOG_EVERY_N(ERROR, 1000)
          << "Connection pool callback was called with mysql err: "
          << mysql_errno();
      completeOperation(OperationResult::Failed);
      signalWaiter();
      return;
    }

    const auto* mysql_conn =
        getMysqlConnection(&pooled_conn->getInternalConnection());
    changeHandlerFD(
        folly::NetworkSocket::fromFd(mysql_conn->getSocketDescriptor()));

    conn().setConnectionHolder(std::move(pooled_conn));
    conn().setConnectionOptions(getConnectionOptions());
    conn().setConnectionDyingCallback(
        [pool = pool_](std::unique_ptr<ConnectionHolder> mysql_conn) {
          auto shared_pool = pool.lock();
          if (shared_pool) {
            shared_pool->recycleMysqlConnection(std::move(mysql_conn));
          }
        });
    // Who finishes the operation depends on who is running this callback.
    //
    // The pool hands a finished connection to whichever operation is queued
    // for it (ConnectionPool::addConnection), and for a client that does not
    // confine its operations to one thread that queued operation may belong to
    // a different thread than this one. Completing it here would touch its
    // state -- notably the ConnectionContext it shares with the
    // ConnectOperation that produced this connection -- from two threads at
    // once.
    //
    // Deferring is gated on handoff_ being Waiting, which prepWait() sets and
    // nothing else does. That is the only state in which some thread is
    // guaranteed to come back for this operation, so it is the only state in
    // which it is safe not to finish the job here. A pool hit, an operation
    // whose previous attempt already resolved, and every client that confines
    // its operations to one thread all leave it non-Waiting and complete
    // inline, on the thread that owns the operation.
    //
    // When it is Waiting, the owner may still be about to stop waiting, so the
    // two sides settle it with one CAS each: whoever moves the state out of
    // Waiting owns the completion. Exactly one can win, so the operation is
    // neither completed twice nor dropped.
    auto expected = Handoff::Waiting;
    if (!handoff_.compare_exchange_strong(expected, Handoff::HandedOff)) {
      // The owner is not coming: either it never armed a wait, or it timed out
      // and stopped touching this operation. Completing here is safe, and
      // necessary, because nobody else is going to.
      attemptSucceeded(OperationResult::Succeeded);
    }

    signalWaiter();
  }

  void actionable() override {
    DCHECK(conn().client().getEventBase()->isInEventBaseThread());
    LOG(DFATAL) << "Should not be called";
  }

  void prepWait() override {
    baton_ = std::make_unique<folly::Baton<>>();
    // Arm the handoff for this attempt. Waiting means, and only means, that a
    // waiter is coming: this runs from openNewConnectionPrep(), which is
    // always followed by openNewConnectionFinish(). Re-arming here is what
    // makes retries work -- a previous attempt may have left the state at
    // Abandoned, and without this the retry's handoff would be completed on
    // the fulfilling thread.
    handoff_.store(Handoff::Waiting, std::memory_order_relaxed);
  }

  bool syncWait() override {
    DCHECK(baton_);
    return baton_->try_wait_for(getTimeout() - opElapsed());
  }

  void cleanupWait() override {
    baton_.reset();
  }

  // Called by the owning thread to take over a handoff. A no-op unless
  // connectionCallback() actually handed off, so the inline-completion,
  // failure and timeout paths are all unaffected.
  void completeDeferred() override {
    auto expected = Handoff::HandedOff;
    if (handoff_.compare_exchange_strong(expected, Handoff::Completed)) {
      attemptSucceeded(OperationResult::Succeeded);
    }
  }

  // Called by the owning thread to run a retry that attemptFailed() handed
  // back to it. Returns whether there was one. Releases the baton it was parked
  // on first, so that the retry's prepWait() arms a fresh one.
  bool resumeRetry() override {
    auto expected = Handoff::RetryPending;
    if (!handoff_.compare_exchange_strong(expected, Handoff::Unarmed)) {
      return false;
    }

    baton_.reset();
    specializedRun();
    return true;
  }

  // Called by the owning thread when it stops waiting. Returns true if it got
  // there before any handoff, in which case connectionCallback() will complete
  // the operation itself if it arrives later. Returns false if a handoff had
  // already landed, leaving the completion to completeDeferred().
  bool abandonWait() override {
    auto expected = Handoff::Waiting;
    return handoff_.compare_exchange_strong(expected, Handoff::Abandoned);
  }

  // Called by the owning thread when it stops waiting and finds that
  // attemptFailed() handed a retry back in the race window after the CAS to
  // RetryPending but before signalWaiter() posted the baton, so syncWait()
  // timed out on the pre-retry deadline instead of waking. The owner has
  // stopped waiting, so unlike resumeRetry() it must not touch the baton -- the
  // handing-off thread may still be in signalWaiter() -- which is why it cannot
  // run the retry (that re-arms the baton). It claims the retry so nobody else
  // acts on it and leaves the operation for the caller to time out. Returns
  // true if a handed-back retry was consumed. Abandoned still wins permanently.
  bool abandonRetry() override {
    auto expected = Handoff::RetryPending;
    return handoff_.compare_exchange_strong(expected, Handoff::Abandoned);
  }

  // Both the connect wake-up and the baton post are owed once this operation
  // is finished.  wakeCallerOnce() runs the consumer's connect callback and can
  // throw; signalWaiter() only posts a baton and cannot, and is still owed if
  // the wake fails -- so the post goes in the wake's catch rather than after
  // it.
  void dischargeWaiters() {
    try {
      wakeCallerOnce();
    } catch (...) {
      signalWaiter();
      throw;
    }
    signalWaiter();
  }

  void signalWaiter() {
    if (baton_) {
      baton_->post();
    }
  }

  std::weak_ptr<ConnectionPool<Client>> pool_;

  std::unique_ptr<folly::Baton<>> baton_;

  // Which side owns completing this operation.
  //
  // Unarmed unless prepWait() has armed a wait for the current attempt, so
  // "Waiting" is exactly the condition under which handing work back is safe.
  // From Waiting exactly one CAS wins and decides who carries on:
  // connectionCallback() takes HandedOff to give the owner a connection to
  // complete, attemptFailed() takes RetryPending to give it another attempt to
  // run, or the owner takes Abandoned to say it has stopped waiting. Completed
  // only exists to keep completeDeferred() idempotent.
  enum class Handoff {
    Unarmed,
    Waiting,
    HandedOff,
    RetryPending,
    Abandoned,
    Completed,
  };
  std::atomic<Handoff> handoff_{Handoff::Unarmed};

  friend class AsyncConnectionPool;
  friend class ConnectPoolOperation<Client>;
};

} // namespace facebook::common::mysql_client::mysql_protocol
