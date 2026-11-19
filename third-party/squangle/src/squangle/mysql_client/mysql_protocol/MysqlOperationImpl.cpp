/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/polymorphic_cast.hpp>
#include <fmt/chrono.h>

#include "squangle/mysql_client/Connection.h"
#include "squangle/mysql_client/mysql_protocol/MysqlConnection.h"
#include "squangle/mysql_client/mysql_protocol/MysqlOperationImpl.h"

namespace facebook::common::mysql_client::mysql_protocol {

void MysqlOperationImpl::protocolCompleteOperation(OperationResult result) {
  conn().runInThread(
      this, &MysqlOperationImpl::completeQueuedOperation, result);
}

void MysqlOperationImpl::completeQueuedOperation(OperationResult result) {
  runQueuedTask(
      "completeOperation", result, [&] { completeOperation(result); });
}

void MysqlOperationImpl::runQueuedTask(
    std::string_view what,
    OperationResult failureResult,
    folly::FunctionRef<void()> fn) {
  if (!isEventBaseSet()) {
    fn();
    return;
  }
  runCallbackGuarded(what, failureResult, fn);
}

MysqlOperationImpl::MysqlOperationImpl()
    : OperationBase(),
      EventHandler(nullptr, folly::NetworkSocket()),
      AsyncTimeout() {}

void MysqlOperationImpl::initializeFromConnection() {
  // Re-initialize EventHandler and AsyncTimeout with the correct event base
  // now that the connection is set up.
  // Note: For sync clients, getEventBase() returns nullptr, and that's OK -
  // we only attach when there's actually an EventBase available.
  auto* eventBase = conn().getEventBase();
  if (eventBase) {
    EventHandler::changeHandlerFD(folly::NetworkSocket());
    EventHandler::attachEventBase(eventBase);
    AsyncTimeout::attachEventBase(eventBase);
  }
}

bool MysqlOperationImpl::isInEventBaseThread() const {
  auto* c = connection();
  return c != nullptr && c->isInEventBaseThread();
}

bool MysqlOperationImpl::isEventBaseSet() const {
  return conn().getEventBase() != nullptr;
}

void MysqlOperationImpl::invokeActionable() {
  DCHECK(isInEventBaseThread());
  auto guard = makeRequestGuard();
  actionable();
}

/*static*/
MysqlConnection* MysqlOperationImpl::getMysqlConnection(
    InternalConnection* conn) {
  return boost::polymorphic_cast<MysqlConnection*>(conn);
}

/*static*/
const MysqlConnection* MysqlOperationImpl::getMysqlConnection(
    const InternalConnection* conn) {
  return boost::polymorphic_cast<const MysqlConnection*>(conn);
}

const MysqlConnection* MysqlOperationImpl::getMysqlConnection() const {
  return getMysqlConnection(&getInternalConnection());
}

MysqlConnection* MysqlOperationImpl::getMysqlConnection() {
  return getMysqlConnection(&getInternalConnection());
}

void MysqlOperationImpl::waitForActionable() {
  DCHECK(isInEventBaseThread());

  // Check if timeout manager is still valid (EventBase not shutting down)
  // This can happen if an operation is scheduled during client shutdown
  if (!getTimeoutManager()) {
    completeOperation(OperationResult::Failed);
    return;
  }

  auto event_mask = getMysqlConnection()->getReadWriteState();

  if (hasOpElapsed(getTimeout())) {
    timeoutTriggered();
    return;
  }

  auto leftUs = getTimeout() - opElapsed();
  auto leftMs = std::chrono::duration_cast<Millis>(leftUs);
  scheduleTimeout(leftMs.count());
  registerHandler(event_mask);
}

void MysqlOperationImpl::completeOperationFromCallbackFailure(
    OperationResult result) {
  completeOperation(result);
}

void MysqlOperationImpl::detachFromEventBase() {
  unregisterHandler();
  cancelTimeout();
}

void MysqlOperationImpl::runCallbackGuarded(
    std::string_view what,
    OperationResult failureResult,
    folly::FunctionRef<void()> fn) noexcept {
  std::string error;
  try {
    fn();
    return;
  } catch (const std::exception& ex) {
    error = ex.what();
  } catch (...) {
    error = "unknown exception";
  }
  LOG(ERROR) << "Exception in " << what << ": " << error;

  // Recovery.  Each step is independent and separately guarded: this is a
  // noexcept frame, so a second failure must not escape, and one step failing
  // must not cost the others.
  auto step = [&](std::string_view stage, auto&& body) noexcept {
    try {
      body();
    } catch (const std::exception& ex) {
      LOG(ERROR) << "Could not " << stage << " after " << what
                 << " failed: " << ex.what();
    } catch (...) {
      LOG(ERROR) << "Could not " << stage << " after " << what << " failed";
    }
  };

  // Swallowing the exception must not leave the caller waiting forever.  The
  // retry path in a connect attempt unregisters the event handler and cancels
  // both timeouts before re-arming them, so a throw in that window strands the
  // operation with nothing left to drive it and no terminal state.
  //
  // The error is recorded only when there is nothing to lose by recording it.
  // setAsyncClientError overwrites mysql_errno_/mysql_error_ unconditionally,
  // and both halves of the condition matter: a completed operation may have
  // succeeded, and a *pending* one may already carry a real server error --
  // MysqlConnectOperationImpl::actionable() snapshots the MySQL errno before
  // calling attemptFailed(), which is the very window this recovery exists
  // for, so state alone would let SQ_INTERNAL_ERROR bury an "Access denied".
  step("complete operation", [&] {
    if (state() != OperationState::Completed && mysql_errno() == 0) {
      setAsyncClientError(
          static_cast<unsigned int>(SquangleErrno::SQ_INTERNAL_ERROR),
          fmt::format("{} failed: {}", what, error));
    }
    completeOperationFromCallbackFailure(failureResult);
  });

  // Detach from the event base, which the completion above cannot be relied on
  // to have done.  completeOperationInner sets Completed on its first line but
  // only unregisters and cancels several statements later, so a throw in that
  // window leaves the handler registered and the timeout armed while
  // completeOperation has already become a no-op.  Retiring the operation
  // below without this would leave a readiness event or timer able to re-enter
  // these callbacks on an operation that is finished and gone.  Both calls are
  // idempotent, so repeating them on the paths that did run costs nothing.
  step("detach from the event base", [&] { detachFromEventBase(); });

  // Retire the operation.  Skipping this leaves the client holding its
  // shared_ptr in pending_.operations, so the operation is never retired and a
  // drain or shutdown still counts it as in flight.  Repeating it is safe, but
  // not because the operation has already left pending_.operations -- it has
  // not, since removal happens later in cleanupCompletedOperations, so the
  // contains() guard passes and the body runs again.  It is safe because
  // to_remove is no longer empty, which suppresses a second runInThread, and
  // because re-inserting into the set is a no-op.
  step("retire operation", [&] { deferRemoveOperation(op_); });
}

void MysqlOperationImpl::handlerReady(uint16_t /*events*/) noexcept {
  // handlerReady is `noexcept` so we can't throw from it.
  runCallbackGuarded("handlerReady", OperationResult::Failed, [&] {
    DCHECK(conn().isInEventBaseThread());

    auto st = state();
    if (st == OperationState::Cancelling) {
      cancel();
    } else if (
        st != OperationState::Completed && st != OperationState::Unstarted) {
      invokeActionable();
    } else {
      LOG(WARNING) << "handlerReady() called in unexpected state: " << st;
    }
  });
}

void MysqlOperationImpl::timeoutExpired() noexcept {
  // timeoutExpired is `noexcept` so we can't throw from it.
  runCallbackGuarded(
      "timeoutExpired", OperationResult::TimedOut, [&] { timeoutTriggered(); });
}

void MysqlOperationImpl::timeoutTriggered() {
  specializedTimeoutTriggered();
}

void MysqlOperationImpl::completeOperation(OperationResult result) {
  DCHECK(isInEventBaseThread());
  if (state() == OperationState::Completed) {
    return;
  }

  CHECK_THROW(
      state() == OperationState::Pending ||
          state() == OperationState::Cancelling ||
          state() == OperationState::Unstarted,
      db::OperationStateException);
  completeOperationInner(result);
}

void MysqlOperationImpl::completeOperationInner(OperationResult result) {
  setState(OperationState::Completed);
  setResult(result);
  setDuration();
  if (result == OperationResult::Cancelled ||
      result == OperationResult::TimedOut) {
    if (auto* c = connection(); c && c->hasInitialized()) {
      // Cancelled/timed out ops leave our connection in an undefined
      // state.  Close it to prevent trouble.
      c->close();
    }
  }

  unregisterHandler();
  cancelTimeout();

  if (callbacks_.post_operation_callback_) {
    callbacks_.post_operation_callback_(*op_);
  }

  specializedCompleteOperation();

  // call observer callback
  if (observer_callback_) {
    observer_callback_(*op_);
  }

  deferRemoveOperation(op_);
}

/*static*/ std::string MysqlOperationImpl::connectStageString(
    connect_stage stage) {
  return MysqlConnection::findConnectStageName(stage).value_or("");
}

} // namespace facebook::common::mysql_client::mysql_protocol
