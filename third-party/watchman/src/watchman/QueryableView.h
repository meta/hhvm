/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/futures/Future.h>
#include <vector>

#include "watchman/Clock.h"
#include "watchman/CookieSync.h"
#include "watchman/PerfSample.h"
#include "watchman/telemetry/LogEvent.h"
#include "watchman/watchman_string.h"

namespace watchman {

struct Query;
struct QueryContext;
class Root;
class SCM;

class QueryableView : public std::enable_shared_from_this<QueryableView> {
 public:
  /**
   * Set if this view requires crawling the filesystem.
   */
  const bool requiresCrawl;

  QueryableView(const w_string& root_path, bool requiresCrawl);
  virtual ~QueryableView();

  /**
   * Perform a time-based (since) query and emit results to the supplied
   * query context.
   */
  virtual void timeGenerator(const Query* query, QueryContext* ctx) const;

  /**
   * Walks files that match the supplied set of paths.
   */
  virtual void pathGenerator(const Query* query, QueryContext* ctx) const;

  virtual void globGenerator(const Query* query, QueryContext* ctx) const;

  virtual void allFilesGenerator(const Query* query, QueryContext* ctx) const;

  virtual ClockPosition getMostRecentRootNumberAndTickValue() const = 0;
  virtual w_string getCurrentClockString() const = 0;
  virtual ClockTicks getLastAgeOutTickValue() const;
  virtual std::chrono::system_clock::time_point getLastAgeOutTimeStamp() const;
  virtual void ageOut(
      int64_t& walked,
      int64_t& files,
      int64_t& dirs,
      std::chrono::seconds minAge);

  /**
   * Whether a clock that another watchman process gave out for this root still
   * names a position this view can answer from, so a query can resume from it
   * instead of starting over with a fresh instance.
   */
  virtual bool canResumeOtherProcessClocks() const {
    return false;
  }

  virtual folly::SemiFuture<folly::Unit> waitForSettle(
      std::chrono::milliseconds settle_period) = 0;
  virtual CookieSync::SyncResult syncToNow(
      const std::shared_ptr<Root>& root,
      std::chrono::milliseconds timeout) = 0;

  /**
   * Synchronize this view with the working copy.
   *
   * The returned future will complete when this view caught up with all the
   * writes to the working copy.
   */
  virtual folly::SemiFuture<CookieSync::SyncResult> sync(
      const std::shared_ptr<Root>& root) = 0;

  // Specialized query function that is used to test whether
  // version control files exist as part of some settling handling.
  // It should query the view and return true if any of the named
  // files current exist in the view.
  virtual bool doAnyOfTheseFilesExist(
      const std::vector<w_string>& fileNames) const = 0;

  bool isVCSOperationInProgress() const;

  /**
   * Start up any helper threads.
   */
  virtual void startThreads(const std::shared_ptr<Root>& /*root*/) {}
  /**
   * Request that helper threads shutdown (but does not join them).
   */
  virtual void stopThreads(std::string_view /*reason*/) {}
  /**
   * Request that helper threads wake up and re-evaluate their state.
   */
  virtual void wakeThreads() {}

  virtual const w_string& getName() const = 0;
  virtual json_ref getWatcherDebugInfo() const = 0;
  virtual void clearWatcherDebugInfo() = 0;
  [[nodiscard]] virtual folly::SemiFuture<folly::Unit>
  waitUntilReadyToQuery() = 0;

  // Return the SCM detected for this watched root
  SCM* getSCM() const {
    return scm_.get();
  }

 private:
  // The source control system that we detected during initialization
  std::unique_ptr<SCM> scm_;
};
} // namespace watchman
