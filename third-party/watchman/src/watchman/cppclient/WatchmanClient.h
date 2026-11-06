/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

/* A C++ client library for accessing Watchman. This builds on top of
 * WatchmanConnection to provide a C++ friendly subscription API and maybe
 * later other convienence functions. The entry point for this API is via
 * the WatchmanClient type.
 *
 * Example usage:
 *  // Set-up an EventBase to execute I/O operations in.
 *  folly::EventBase eb = ...;
 *
 *  // Set-up a session with a subscription using chained future actions to:
 *  // * Connect to the watchman server (and print out Watchman version)
 *  // * Set-up a watch for /some/path
 *  // * Subscribe to a query of name data for any updated files
 *  WatchmanClient client(&eb);
 *  auto subFuture = client.connect().then([](folly::dynamic&& response) {
 *    std::cout << "Server version " << response["version"] << std::endl;
 *
 *    return client.watch("/some/path").then([](WatchPathPtr watch) {
 *      folly::dynamic query = folly::dynamic::object("fields", {"name"});
 *
 *      return client.subscribe(
 *          query,
 *          watch,
 *          &eb,
 *          [](folly::Try<folly::dynamic>&& data) {
 *            if (data.hasValue()) {
 *              std::cout << "Got file update data: " << data << std::endl;
 *            } else {
 *              std::cout << "subscribe() failed with: " << data.exception()
 *                        << std::endl;
 *            }
 *          });
 *    });
 *  });
 *
 *  // Wait for chain of futures to complete or raise an exception.
 *  auto subscription = subFuture.wait().value();
 *
 *  // Run a one-shot Watchman command and extract the output.
 *  auto watchman_version_str =
 *    client.run({"version"}).wait().value()["version"];
 *
 *  // ... do stuff ...
 *
 *  // Unsubscribe from query above. Note this is not strictly needed if we're
 *  // going to immediately shut down the connection anyway.
 *  client.unsubscribe(subscription);
 *
 *  // Close connection. Note again this is not strictly needed as
 *  // deconstruction will also cause the connection to close.
 *  client.close();
 */

#include "WatchmanConnection.h"

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include <folly/Executor.h>
#include <folly/Optional.h>
#include <folly/Try.h>
#include <folly/futures/Future.h>
#include <folly/json/dynamic.h>

namespace watchman {

struct WatchmanClient;
struct Subscription;

struct WatchPath {
  friend WatchmanClient;

  WatchPath(
      const std::string& root,
      const std::optional<std::string>& relativePath);

 private:
  const std::string root_;
  const std::optional<std::string> relativePath_;
};

using Clock = std::string;
using WatchPathPtr = std::shared_ptr<WatchPath>;

using SubscriptionCallback = std::function<void(folly::Try<folly::dynamic>&&)>;
using ErrorCallback = std::function<void(folly::exception_wrapper&)>;

struct QueryResult {
  QueryResult(folly::dynamic raw) : raw_{std::move(raw)} {}

  folly::dynamic raw_;
};

struct Subscription {
  friend WatchmanClient;

  Subscription(
      folly::Executor* executor,
      SubscriptionCallback&& callback,
      const std::string& name,
      WatchPathPtr watchPath);

 private:
  folly::Executor::KeepAlive<folly::Executor> executor_;
  SubscriptionCallback callback_;
  const std::string name_;
  WatchPathPtr watchPath_;
  bool active_{true};
};

using SubscriptionPtr = std::shared_ptr<Subscription>;

struct WatchmanClient {
  ~WatchmanClient();

  explicit WatchmanClient(
      folly::EventBase* eventBase,
      std::optional<std::string>&& sockPath = {},
      folly::Executor* cpuExecutor = {},
      ErrorCallback errCb = {});

  [[deprecated("use std::optional instead")]] explicit WatchmanClient(
      folly::EventBase* eventBase,
      folly::Optional<std::string>&& sockPath,
      folly::Executor* cpuExecutor = {},
      ErrorCallback errCb = {})
      : WatchmanClient(
            eventBase,
            std::optional<std::string>{std::move(sockPath)},
            cpuExecutor,
            std::move(errCb)) {}

  /**
   * Establishes a connection, returning version and capability information per
   * https://meta.github.io/watchman/docs/cmd/version.html#capabilities
   */
  folly::SemiFuture<folly::dynamic> connect(
      folly::dynamic versionArgs = folly::dynamic::object()(
          "required",
          folly::dynamic::array("relative_root")));

  /**
   * Close the underlying connection to Watchman, including automatically
   * unsubscribing from all subscriptions.
   */
  void close();

  /**
   * Returns true if the underlying connection is closed or broken.
   */
  bool isDead() {
    return conn_->isDead();
  }

  /**
   * Execute a watchman command, yielding the command response.
   * cmd is typically an array.
   * See
   * https://meta.github.io/watchman/docs/socket-interface.html#watchman-protocol
   * for the conventions.
   *
   * Errors, both at the transport layer and at the watchman protocol layer
   * (where the "error" field is set in the response) are captured in the Future
   * as exceptions. Watchman protocol response errors are represented by the
   * WatchmanResponseError type.
   */
  folly::SemiFuture<folly::dynamic> run(const folly::dynamic& cmd);

  /**
   * Create a watch for a path, automatically sharing scarce OS resources
   * between multiple watchers of the same (super-)tree. This should be the
   * preferred way to create a WatchPath instance unless you want to explicitly
   * avoid sharing Watchman tree configurations between independent watchers.
   *
   * See https://meta.github.io/watchman/docs/cmd/watch-project.html for
   * details.
   */
  folly::SemiFuture<WatchPathPtr> watch(std::string_view path);

  /**
   * Ask Watchman for its current clock in a given root.
   */
  folly::SemiFuture<Clock> getClock(WatchPathPtr path);

  /**
   * Send a single query to Watchman. queryObj is an object rather than an
   * array.
   */
  folly::SemiFuture<QueryResult> query(
      folly::dynamic queryObj,
      WatchPathPtr path);

  /**
   * Establishes a subscription that will trigger callback (via your specified
   * executor) whenever matching files change.
   */
  folly::SemiFuture<SubscriptionPtr> subscribe(
      folly::dynamic query,
      WatchPathPtr path,
      folly::Executor* executor,
      SubscriptionCallback&& callback,
      std::string subscriptionName);

  /**
   * As-per subscribe above but automatically creates a WatchPath from a string.
   * This is probably what you want but see comments on watch() for details.
   */
  folly::SemiFuture<SubscriptionPtr> subscribe(
      const folly::dynamic& query,
      std::string_view path,
      folly::Executor* executor,
      SubscriptionCallback&& callback,
      std::string subscriptionName);

  /**
   * Returns a future which completes when all outstanding Watchman updates have
   * been received or a timeout occurs.
   * See https://meta.github.io/watchman/docs/cmd/flush-subscriptions.html
   */
  folly::SemiFuture<folly::dynamic> flushSubscription(
      SubscriptionPtr subscription,
      std::chrono::milliseconds timeout);

  /** Cancels an existing subscription. */
  folly::SemiFuture<folly::dynamic> unsubscribe(SubscriptionPtr subscription);

  /** Intended for test only. */
  WatchmanConnection& getConnection() {
    return *conn_;
  }

 private:
  void connectionCallback(folly::Try<folly::dynamic>&& try_data);

  folly::Future<WatchPathPtr> watchImpl(std::string_view path);

  std::shared_ptr<WatchmanConnection> conn_;
  ErrorCallback errorCallback_;
  std::unordered_map<std::string, SubscriptionPtr> subscriptionMap_;
  std::mutex mutex_;
  int nextSubID_{0};
};

} // namespace watchman
