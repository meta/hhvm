<?hh
/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the MIT license found in the
 *  LICENSE file in the hphp/hsl/ subdirectory of this source tree.
 *
 */

namespace HH\Lib\Async;

/**
 * A wrapper around ConditionWaitHandle that allows notification events
 * to occur before the condition is awaited.
 */
class Condition<T> {
  private ?Awaitable<T> $condition = null;

  /**
   * Notify the condition variable of success and set the result.
   */
  final public function succeed(T $result): void {
    invariant($this->trySucceed($result), 'Unable to notify Condition twice');
  }

  /**
   * Notify the condition variable of failure and set the exception.
   */
  final public function fail(\Exception $exception): void {
    invariant($this->tryFail($exception), 'Unable to notify Condition twice');
  }

  /**
   * Notify the condition variable of success and set the $result.
   *
   * @return
   *   true if the condition is set to $result successfully, false if the
   *   condition was previously set to another result or exception.
   */
  final public function trySucceed(T $result): bool {
    if ($this->condition === null) {
      $this->condition = async {
        return $result;
      };
      return true;
    } else {
      if (!($this->condition is ConditionWaitHandle<_>)) {
        return false;
      }
      /* HH_FIXME[4110]: Type error revealed by type-safe instanceof feature. See https://fburl.com/instanceof */
      $this->condition->succeed($result);
      return true;
    }
  }

  /**
   * Notify the condition variable of failure and set the $exception.
   *
   * @return
   *   true if the condition is set to $exception successfully, false if the
   *   condition was previously set to another result or exception.
   */
  final public function tryFail(\Exception $exception): bool {
    if ($this->condition === null) {
      $this->condition = async {
        throw $exception;
      };
      return true;
    } else {
      if (!($this->condition is ConditionWaitHandle<_>)) {
        return false;
      }
      $this->condition->fail($exception);
      return true;
    }
  }

  /**
   * Asynchronously wait for the condition variable to be notified and
   * return the result or throw the exception received via notification.
   *
   * The caller must provide an Awaitable $notifiers (which must be a
   * WaitHandle) that must not finish before the notification is received.
   * This means $notifiers must represent work that is guaranteed to
   * eventually trigger the notification. As long as the notification is
   * issued only once, asynchronous execution unrelated to $notifiers is
   * allowed to trigger the notification.
   */
  final public async function waitForNotificationAsync(
    Awaitable<void> $notifiers,
  ): Awaitable<T> {
    if ($this->condition === null) {
      $this->condition = ConditionWaitHandle::create($notifiers);
    }
    return await $this->condition;
  }
}
