/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef COMMON_DB_CLIENT_STATS_H
#define COMMON_DB_CLIENT_STATS_H

#include <folly/Optional.h>
#include <folly/Range.h>
#include <atomic>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace facebook {
namespace db {

using AddNormalValueFunction =
    std::function<void(folly::StringPiece key, folly::StringPiece value)>;
using AddIntValueFunction =
    std::function<void(folly::StringPiece key, int64_t value)>;

/*
 * Base class to allow dynamic logging data efficiently saved in Squangle core
 * classes. Should be used for data about the connection.
 */
class ConnectionContextBase {
 public:
  ConnectionContextBase() = default;
  ConnectionContextBase(const ConnectionContextBase&) = default;
  ConnectionContextBase(ConnectionContextBase&&) = default;
  ConnectionContextBase& operator=(const ConnectionContextBase&) = default;
  ConnectionContextBase& operator=(ConnectionContextBase&&) = default;
  virtual ~ConnectionContextBase() = default;
  virtual void collectValues(
      const AddIntValueFunction&,
      const AddNormalValueFunction&) const;
  virtual std::unique_ptr<ConnectionContextBase> copy() const {
    return std::make_unique<ConnectionContextBase>(*this);
  }

  /**
   * Provide a more efficient mechanism to access a single value stored in the
   * ConnectionContextBase that does not require executing a functor against
   * every possible value and filtering in the functor
   */
  virtual folly::Optional<std::string> getNormalValue(
      folly::StringPiece key) const;
  bool isSslConnection = false;
  bool sslSessionReused = false;
  std::string sslVersion;
  folly::Optional<std::string> sslCertCn;
  folly::Optional<std::vector<std::string>> sslCertSan;
  folly::Optional<std::vector<std::string>> sslCertIdentities;
  bool isServerCertValidated = false;
  bool isIdentityClientCert = false;
  std::string endpointVersion;
  std::optional<size_t> certCacheSize;
};

class ExponentialMovingAverage {
 public:
  explicit ExponentialMovingAverage(double smootingFactor);
  void addSample(double sample);

  double value() const {
    return currentValue_;
  }

 private:
  double smoothingFactor_ = 0;
  double currentValue_ = 0;
  bool hasRegisteredFirstSample_ = false;
};

// An wrapper around the metrics we care about the performance of the Client.
// Mainly a struct to easily pass to HHVM and other loggers.
struct ClientPerfStats {
  double callbackDelayMicrosAvg = 0;
  double ioEventLoopMicrosAvg = 0;
  double ioThreadBusyTime = 0;
  double ioThreadIdleTime = 0;
  uint64_t notificationQueueSize = 0;
};

class DBCounterBase {
 public:
  DBCounterBase() {}

  virtual ~DBCounterBase() {}

  // opened connections
  virtual void incrOpenedConnections(
      const db::ConnectionContextBase* context) = 0;

  // closed connections
  virtual void incrClosedConnections(
      const db::ConnectionContextBase* context) = 0;

  // failed connections
  virtual void incrFailedConnections(
      const db::ConnectionContextBase* context,
      unsigned int mysql_errno,
      const std::string& errStr) = 0;

  // query failures
  virtual void incrFailedQueries(
      const db::ConnectionContextBase* context,
      unsigned int mysql_errno,
      const std::string& errStr) = 0;

  // query successes
  virtual void incrSucceededQueries(
      const db::ConnectionContextBase* context) = 0;

  // SSL session reuse tracking, split by TLS version (e.g. "TLSv1.2")
  virtual void incrReusedSSLSessions(std::string_view tlsVersion) = 0;
  virtual void incrSSLSessionNotReused(std::string_view tlsVersion) = 0;
};

// Holds the stats for a client, thread safe and allows some good stats in
// tests
class SimpleDbCounter : public DBCounterBase {
 public:
  SimpleDbCounter()
      : DBCounterBase(),
        opened_connections_(0),
        closed_connections_(0),
        failed_connections_(0),
        failed_queries_(0),
        succeeded_queries_(0),
        reused_ssl_sessions_(0) {}

  // opened connections
  uint64_t numOpenedConnections() {
    return opened_connections_.load(std::memory_order_relaxed);
  }

  void incrOpenedConnections(
      const db::ConnectionContextBase* /* context */) override {
    opened_connections_.fetch_add(1, std::memory_order_relaxed);
  }

  // closed connections
  uint64_t numClosedConnections() {
    return closed_connections_.load(std::memory_order_relaxed);
  }

  void incrClosedConnections(
      const db::ConnectionContextBase* /* context */) override {
    closed_connections_.fetch_add(1, std::memory_order_relaxed);
  }

  // failed connections
  uint64_t numFailedConnections() {
    return failed_connections_.load(std::memory_order_relaxed);
  }

  void incrFailedConnections(
      const db::ConnectionContextBase* /* context */,
      unsigned int /* unused */,
      const std::string& /* unused */) override {
    failed_connections_.fetch_add(1, std::memory_order_relaxed);
  }

  // query failures
  uint64_t numFailedQueries() {
    return failed_queries_.load(std::memory_order_relaxed);
  }

  void incrFailedQueries(
      const db::ConnectionContextBase* /* context */,
      unsigned int /* unused */,
      const std::string& /* unused */) override {
    failed_queries_.fetch_add(1, std::memory_order_relaxed);
  }

  // query successes
  uint64_t numSucceededQueries() {
    return succeeded_queries_.load(std::memory_order_relaxed);
  }

  void incrSucceededQueries(
      const db::ConnectionContextBase* /* context */) override {
    succeeded_queries_.fetch_add(1, std::memory_order_relaxed);
  }

  uint64_t numReusedSSLSessions() {
    return reused_ssl_sessions_.load(std::memory_order_relaxed);
  }

  void incrReusedSSLSessions(std::string_view /* tlsVersion */) override {
    reused_ssl_sessions_.fetch_add(1, std::memory_order_relaxed);
  }

  void incrSSLSessionNotReused(std::string_view /* tlsVersion */) override {}

  // For logging porpuses
  void printStats();

 private:
  std::atomic<uint64_t> opened_connections_;
  std::atomic<uint64_t> closed_connections_;
  std::atomic<uint64_t> failed_connections_;
  std::atomic<uint64_t> failed_queries_;
  std::atomic<uint64_t> succeeded_queries_;
  std::atomic<uint64_t> reused_ssl_sessions_;
};

class PoolStats {
 public:
  PoolStats()
      : created_pool_connections_(0),
        destroyed_pool_connections_(0),
        connections_requested_(0),
        pool_hits_(0),
        pool_misses_(0),
        pool_hits_change_user_(0) {}
  // created connections
  uint64_t numCreatedPoolConnections() const noexcept {
    return created_pool_connections_.load(std::memory_order_relaxed);
  }

  void incrCreatedPoolConnections() {
    created_pool_connections_.fetch_add(1, std::memory_order_relaxed);
  }
  // destroyed connections
  uint64_t numDestroyedPoolConnections() const noexcept {
    return destroyed_pool_connections_.load(std::memory_order_relaxed);
  }

  void incrDestroyedPoolConnections() {
    destroyed_pool_connections_.fetch_add(1, std::memory_order_relaxed);
  }

  // Number of connect operation that were requests, this helps us to compare
  // this with the actual number of connections that were open
  uint64_t numConnectionsRequested() const noexcept {
    return connections_requested_.load(std::memory_order_relaxed);
  }

  void incrConnectionsRequested() {
    connections_requested_.fetch_add(1, std::memory_order_relaxed);
  }

  // How many times the pool had a connection ready in the cache
  uint64_t numPoolHits() const noexcept {
    return pool_hits_.load(std::memory_order_relaxed);
  }

  void incrPoolHits() {
    pool_hits_.fetch_add(1, std::memory_order_relaxed);
  }

  // How many times the pool had a connection ready after COM_CHANGE_USER
  uint64_t numPoolHitsChangeUser() const noexcept {
    return pool_hits_change_user_.load(std::memory_order_relaxed);
  }

  void incrPoolHitsChangeUser() {
    pool_hits_change_user_.fetch_add(1, std::memory_order_relaxed);
  }

  // how many times the pool didn't have a connection right in cache
  uint64_t numPoolMisses() const noexcept {
    return pool_misses_.load(std::memory_order_relaxed);
  }

  void incrPoolMisses() {
    pool_misses_.fetch_add(1, std::memory_order_relaxed);
  }

 private:
  std::atomic<uint64_t> created_pool_connections_;
  std::atomic<uint64_t> destroyed_pool_connections_;
  std::atomic<uint64_t> connections_requested_;
  std::atomic<uint64_t> pool_hits_;
  std::atomic<uint64_t> pool_misses_;
  std::atomic<uint64_t> pool_hits_change_user_;
};
} // namespace db
} // namespace facebook

#endif // COMMON_DB_CLIENT_STATS_H
