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

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#include "hphp/util/optional.h"

#include <folly/json/dynamic.h>
#include <folly/futures/Future.h>
#include <watchman/cppclient/WatchmanClient.h>

namespace HPHP {

using ClockTime = std::chrono::time_point<std::chrono::steady_clock>;
using WatchmanProfiler = void (*)(
  const watchman::QueryResult& result,
  const folly::dynamic& query,
  ClockTime preLockTime,
  ClockTime preExecTime,
  ClockTime execTime
);

/**
 * Singleton to interact with Watchman for a given root.
 */
class Watchman {
public:
  /**
   * A subscription that lasts until this object is destroyed.
   */
  struct Subscription {
    virtual ~Subscription() = default;
  };

  virtual ~Watchman();

  /**
   * Return the Watchman singleton for the chosen root.
   */
  static std::shared_ptr<Watchman>
  get(const std::filesystem::path& path, const Optional<std::string>& sockPath);

  /**
   * Set a profiling function to invoke with timing information from watchman
   * queries.
   */
  static void setProfiler(WatchmanProfiler&&);

  /**
   * Return information about the altered and deleted paths matching
   * the given Watchman query.
   */
  virtual folly::SemiFuture<folly::dynamic> query(folly::dynamic query) = 0;

  /*
   * Return the current watchman clock.
   */
  virtual folly::SemiFuture<watchman::Clock> getClock() = 0;

  /**
   * Invoke the given callback whenever the given Watchman query
   * returns new results, until the returned Subscription is destroyed.
   *
   * A notification that's already being delivered can still reach the
   * callback after the Subscription is destroyed.
   */
  [[nodiscard]] virtual std::unique_ptr<Subscription> subscribe(
      const folly::dynamic& queryObj,
      watchman::SubscriptionCallback&& callback) = 0;
};

} // namespace HPHP
