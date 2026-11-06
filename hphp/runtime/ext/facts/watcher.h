/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
   +----------------------------------------------------------------------+
   | This source path is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the path LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <folly/Executor.h>
#include <folly/Unit.h>
#include <folly/futures/Future.h>
#include <folly/json/dynamic.h>

#include "hphp/runtime/ext/facts/file-facts.h"
#include "hphp/util/optional.h"

namespace HPHP {

namespace Facts {

/**
 * Keeps track of which files have changed on the filesystem.
 */
struct Watcher {
  struct FileDelta {
    std::filesystem::path m_path;
    bool m_exists{false};
    Optional<std::string> m_watcher_hash;

    bool operator==(const FileDelta& o) const {
      return m_path == o.m_path && m_exists == o.m_exists &&
          m_watcher_hash == o.m_watcher_hash;
    }
  };

  struct Delta {
    // These represent points in time.
    Optional<Clock> m_lastClock;
    Clock m_newClock;

    // If true, it means `m_files` is a list of all the files on the system.
    // This may be because `m_lastClock` is empty (because we're starting up
    // for the first time). Or it may be because the Watchman server has
    // restarted.
    bool m_fresh{false};

    // All the files that changed between m_lastClock and m_newClock.
    std::vector<FileDelta> m_files;
  };

  Watcher() = default;
  virtual ~Watcher() = default;
  Watcher(const Watcher&) = delete;
  Watcher& operator=(const Watcher&) = delete;
  Watcher(Watcher&&) = delete;
  Watcher& operator=(Watcher&&) = delete;

  /**
   * Return the changed files since `lastVersion`.
   */
  virtual folly::SemiFuture<Delta> getChanges(Clock lastClock) = 0;

  /**
   * A subscription that lasts until this object is destroyed.
   */
  struct Subscription {
    virtual ~Subscription() = default;
  };

  /**
   * Subscribe to the filesystem. Whenever the filesystem changes, this Watcher
   * will send a Delta object describing the change to the given callback,
   * until the returned Subscription is destroyed. A Delta that's already being
   * delivered can still reach the callback after that. A fresh Delta from a
   * subscription may list no files, so use it only as a signal to call
   * getChanges().
   */
  [[nodiscard]] virtual std::unique_ptr<Subscription> subscribe(
      std::function<void(Delta&&)> callback) = 0;
};

} // namespace Facts
} // namespace HPHP
