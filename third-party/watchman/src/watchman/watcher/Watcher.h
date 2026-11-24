/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once
#include <folly/futures/Future.h>
#include <stdexcept>
#include "watchman/PendingCollection.h"
#include "watchman/fs/DirHandle.h"
#include "watchman/thirdparty/jansson/jansson.h"

#define HINT_NUM_DIRS 128 * 1024
#define CFG_HINT_NUM_DIRS "hint_num_dirs"

struct watchman_file;

namespace watchman {

class QueryableView;
class InMemoryView;
class Root;

class TerminalWatcherError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class Watcher : public std::enable_shared_from_this<Watcher> {
 public:
  /**
   * This Watcher instance's name.
   */
  const w_string name;

  // if this watcher notifies for individual files contained within
  // a watched dir, false if it only notifies for dirs
#define WATCHER_HAS_PER_FILE_NOTIFICATIONS 1
  // if the watcher is comprised of multiple watchers
#define WATCHER_HAS_SPLIT_WATCH 4
  // if a split watcher notifies for individual files in the directories below
  // the root, but not in the root itself
#define WATCHER_HAS_PER_FILE_NOTIFICATIONS_BELOW_ROOT 8
  unsigned flags;

  Watcher(const char* name, unsigned flags);

  Watcher(Watcher&&) = delete;
  Watcher& operator=(Watcher&&) = delete;

  // Start up threads or similar.  Called in the context of the
  // notify thread
  virtual bool start(const std::shared_ptr<Root>& root);

  // Perform watcher-specific cleanup for a watched root when it is freed
  virtual ~Watcher();

  /**
   * If the returned SemiFuture is valid(), then this watcher requires
   * flushing any queued events. A Promise has been placed in the
   * PendingCollection and will be completed when InMemoryView processes the
   * event.
   *
   * In particular, FSEvents may return pending events out of order, so the
   * observation of a cookie file does not guarantee all prior changes have been
   * seen.
   *
   * Otherwise, this watcher does not require flushing, and a cookie file event
   * is considered sufficient synchronization.
   */
  virtual folly::SemiFuture<folly::Unit> flushPendingEvents() {
    return folly::SemiFuture<folly::Unit>::makeEmpty();
  }

  // Initiate an OS-level watch on the provided file
  virtual bool startWatchFile(watchman_file* file);

  // Initiate an OS-level watch on the provided dir, return a DIR
  // handle, or throw on error.
  //
  // path should be absolute.
  //
  // Should be thread-safe.
  virtual std::unique_ptr<DirHandle> startWatchDir(
      const std::shared_ptr<Root>& root,
      const char* path) = 0;

  /**
   * Signal any threads to terminate.  Does not join them, but the reference
   * count on the root will not fall to zero until the threads stop.
   */
  virtual void stopThreads() {}

  enum class WaitNotifyResult {
    Ready,
    Timeout,
    Terminate,
  };

  /**
   * Wait for an inotify event to become available.
   * Returns whether events are available, the timeout elapsed, or the watcher
   * was signaled to terminate.
   */
  virtual WaitNotifyResult waitNotify(int timeoutms) = 0;

  struct ConsumeNotifyRet {
    // Should the watch be cancelled?
    bool cancelSelf;
  };

  /**
   * Consume any available notifications.  If there are none pending,
   * does not block.
   *
   * Notifications are inserted into `coll`.
   */
  virtual ConsumeNotifyRet consumeNotify(
      const std::shared_ptr<Root>& root,
      PendingChanges& coll) = 0;

  /**
   * Returns a JSON value containing this watcher's debug state. Intended for
   * inclusion in diagnostics.
   */
  virtual json_ref getDebugInfo() {
    return json_null();
  }

  /**
   * Clear any accumulated debug state.
   */
  virtual void clearDebugInfo() {}
};

} // namespace watchman
