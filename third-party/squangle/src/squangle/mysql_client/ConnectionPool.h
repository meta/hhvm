/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/Synchronized.h>
#include <folly/container/F14Map.h>
#include <folly/container/F14Set.h>
#include <folly/synchronization/Baton.h>
#include <memory>

#include "squangle/base/Base.h"
#include "squangle/logger/DBEventCounter.h"
#include "squangle/mysql_client/ChangeUserOperation.h" // IWYU pragma: keep
#include "squangle/mysql_client/ConnectPoolOperation.h"
#include "squangle/mysql_client/Connection.h"
#include "squangle/mysql_client/ConnectionHolder.h"
#include "squangle/mysql_client/Operation.h"
#include "squangle/mysql_client/PoolKey.h"
#include "squangle/mysql_client/PoolStorage.h"
#include "squangle/mysql_client/SpecialOperation.h"

// The normal LOG_EVERY_N() macro is not thread safe, so we need to use this
#define SQUANGLE_LOG_EVERY_N_ATOMIC(severity, n)                            \
  DCHECK(n > 1);                                                            \
  /* For performance reasons, let's require a power of 2 for `n` to make */ \
  /* the `%` operation a simpler operation. */                              \
  DCHECK_EQ(n&(n - 1), 0);                                                  \
  static std::atomic<int> LOG_OCCURRENCES = 0;                              \
  auto val = LOG_OCCURRENCES.fetch_add(1, std::memory_order_relaxed);       \
  /* We can just rely on the `%` operator here.  It will work regardless */ \
  /* of what `n` is, but if `n` is a power of 2 (like we check above), */   \
  /* the compiler will replace it with `if (val & (n - 1) == 0) {` */       \
  if (val % n == 0)                                                         \
  google::LogMessage(                                                       \
      __FILE__,                                                             \
      __LINE__,                                                             \
      google::GLOG_##severity,                                              \
      LOG_OCCURRENCES,                                                      \
      &google::LogMessage::SendToLog)                                       \
      .stream()

namespace facebook::common::mysql_client {
namespace mysql_protocol {
template <typename C>
class MysqlConnectPoolOperationImpl;
}

class ConnectionKey;
template <typename Client>
class ConnectionPool;

// In order to keep always healthy connections avoid and avoid holding one
// connection for way too long, we have the options:
//   Age: Connection will be closed when reaches a limit from the time it
// was opened. With this option the connections also get killed by idle time.
//   IdleTime: Doesn't close a connection due the total time it has been opened,
// only closes a connection due being idle for a given amount of time.
enum class ExpirationPolicy {
  Age,
  IdleTime,
  // TODO: Add KeepAlive option, this one will keep it alive and only close by
  // age
};

std::ostream& operator<<(std::ostream& os, ExpirationPolicy policy);

class PoolOptions {
 public:
  // Doing these the long way as we want to avoid depending on C++14 or FB's
  // TimeLiterals.h for Open Source.
  static constexpr Duration kDefaultMaxAge = std::chrono::seconds(60);
  static constexpr Duration kDefaultMaxIdleTime = std::chrono::seconds(4);
  static constexpr Millis kCleanUpTimeout = Millis(300);
  static const int kDefaultMaxOpenConn = 100;

  PoolOptions()
      : per_key_limit_(kDefaultMaxOpenConn),
        pool_limit_(kDefaultMaxOpenConn * 100),
        idle_timeout_(kDefaultMaxIdleTime),
        age_timeout_(kDefaultMaxAge),
        exp_policy_(ExpirationPolicy::Age),
        pool_per_instance_{false} {}

  PoolOptions(PoolOptions&& other) = default;
  PoolOptions(const PoolOptions& other) = default;

  PoolOptions& operator=(const PoolOptions& other) = default;
  PoolOptions& operator=(PoolOptions&& other) = default;

  PoolOptions& setPerKeyLimit(uint64_t conn_limit) {
    per_key_limit_ = conn_limit;
    return *this;
  }
  PoolOptions& setPoolLimit(uint64_t total_limit) {
    pool_limit_ = total_limit;
    return *this;
  }
  PoolOptions& setIdleTimeout(Duration idle_timeout) {
    idle_timeout_ = idle_timeout;
    return *this;
  }
  PoolOptions& setAgeTimeout(Duration age_timeout) {
    age_timeout_ = age_timeout;
    return *this;
  }
  PoolOptions& setExpPolicy(ExpirationPolicy exp_policy) {
    exp_policy_ = exp_policy;
    return *this;
  }
  // If pooling per instance is chosen, then the db name will be ignored
  // for the purposes of connection pooling. The user will be responsible
  // for ensuring they are connected to the correct database. This is useful
  // for instances with many databases
  PoolOptions& setPoolPerMysqlInstance(bool poolPerInstance) {
    pool_per_instance_ = poolPerInstance;
    return *this;
  }

  uint64_t getPerKeyLimit() const {
    return per_key_limit_;
  }
  uint64_t getPoolLimit() const {
    return pool_limit_;
  }
  Duration getIdleTimeout() const {
    return idle_timeout_;
  }
  Duration getAgeTimeout() const {
    return age_timeout_;
  }
  ExpirationPolicy getExpPolicy() const {
    return exp_policy_;
  }
  bool poolPerMysqlInstance() const {
    return pool_per_instance_;
  }

  bool operator==(const PoolOptions& other) const {
    return per_key_limit_ == other.per_key_limit_ &&
        pool_limit_ == other.pool_limit_ &&
        idle_timeout_ == other.idle_timeout_ &&
        // Note: we ignore age_timeout if using the IdleTime expiration policy
        (exp_policy_ == ExpirationPolicy::IdleTime ||
         age_timeout_ == other.age_timeout_) &&
        exp_policy_ == other.exp_policy_ &&
        pool_per_instance_ == other.pool_per_instance_;
  }
  bool operator!=(const PoolOptions& other) const {
    return !(operator==(other));
  }

 private:
  uint64_t per_key_limit_;
  uint64_t pool_limit_;
  Duration idle_timeout_;
  Duration age_timeout_;
  ExpirationPolicy exp_policy_;
  bool pool_per_instance_;
};

std::ostream& operator<<(std::ostream& os, const PoolOptions& options);

template <typename Client>
class MysqlPooledHolder : public ConnectionHolder {
 public:
  // Constructed based on an already existing MysqlConnectionHolder, the
  // values are going to be copied and the old holder will be destroyed.
  MysqlPooledHolder(
      std::unique_ptr<ConnectionHolder> holder_base,
      std::weak_ptr<ConnectionPool<Client>> weak_pool,
      const PoolKey& pool_key)
      : ConnectionHolder(*holder_base, pool_key.getConnectionKey()),
        good_for_(Duration::zero()),
        weak_pool_(weak_pool),
        pool_key_(pool_key) {
    auto lock_pool = weak_pool.lock();
    if (lock_pool) {
      lock_pool->stats()->incrCreatedPoolConnections();
      lock_pool->addOpenConnection(pool_key_);
    }
  }

  virtual ~MysqlPooledHolder() override {
    removeFromPool();
  }

  // Don't allow copy or move
  MysqlPooledHolder(const MysqlPooledHolder&) = delete;
  MysqlPooledHolder& operator=(const MysqlPooledHolder&) = delete;

  MysqlPooledHolder(MysqlPooledHolder&&) = delete;
  MysqlPooledHolder& operator=(MysqlPooledHolder&&) = delete;

  void setLifeDuration(Duration dur) {
    good_for_ = dur;
  }

  Duration getLifeDuration() {
    return good_for_;
  }

  void setOwnerPool(std::weak_ptr<ConnectionPool<Client>> pool) {
    // In case this connection belonged to a pool before
    removeFromPool();
    weak_pool_ = std::move(pool);
    // Extra care here, checking if we changing it to nullptr
    if (auto lock_pool = weak_pool_.lock(); lock_pool) {
      lock_pool->stats()->incrCreatedPoolConnections();
      lock_pool->addOpenConnection(pool_key_);
    }
  }

  const PoolKey& getPoolKey() const {
    return pool_key_;
  }

  std::shared_ptr<ConnectionPool<Client>> getPoolPtr() const {
    return weak_pool_.lock();
  }

 private:
  void removeFromPool() {
    if (auto lock_pool = weak_pool_.lock(); lock_pool) {
      lock_pool->stats()->incrDestroyedPoolConnections();
      lock_pool->removeOpenConnection(pool_key_);
    }
  }

  Duration good_for_;
  std::weak_ptr<ConnectionPool<Client>> weak_pool_;

  const PoolKey pool_key_;
};

class ConnectionPoolBase {
 public:
  explicit ConnectionPoolBase(PoolOptions pool_options)
      : pool_options_(std::move(pool_options)) {}

  virtual ~ConnectionPoolBase() {}

  // Don't allow copy or move
  ConnectionPoolBase(const ConnectionPoolBase&) = delete;
  ConnectionPoolBase& operator=(const ConnectionPoolBase&) = delete;

  ConnectionPoolBase(ConnectionPoolBase&&) = delete;
  ConnectionPoolBase& operator=(ConnectionPoolBase&&) = delete;

  using ThrottlingCallback = std::function<void(uint32_t, const std::string&)>;
  using ShouldThrottleCallback = std::function<bool(
      const PoolKey&,
      std::shared_ptr<db::ConnectionContextBase> context,
      ThrottlingCallback throttlingCallback)>;

  db::PoolStats* stats() noexcept {
    return &pool_stats_;
  }

  const db::PoolStats* stats() const noexcept {
    return &pool_stats_;
  }

  PoolKeyStats getPoolKeyStats(const PoolKey& key) const;

  [[nodiscard]] size_t perKeyLimit() const noexcept {
    return pool_options_.rlock()->getPerKeyLimit();
  }

  [[nodiscard]] size_t totalLimit() const noexcept {
    return pool_options_.rlock()->getPoolLimit();
  }

  [[nodiscard]] Duration ageTimeout() const noexcept {
    return pool_options_.rlock()->getAgeTimeout();
  }

  [[nodiscard]] Duration idleTimeout() const noexcept {
    return pool_options_.rlock()->getIdleTimeout();
  }

  [[nodiscard]] ExpirationPolicy expirationPolicy() const noexcept {
    return pool_options_.rlock()->getExpPolicy();
  }

  [[nodiscard]] bool poolPerMysqlInstance() const noexcept {
    return pool_options_.rlock()->poolPerMysqlInstance();
  }

  void setPerKeyLimit(int limit) {
    pool_options_.wlock()->setPerKeyLimit(limit);
  }

  // Note that unlike the AsyncMysqlClient this similar counter is for open
  // connections only, the intent of opening a connect is controlled
  // separately.
  void addOpenConnection(const PoolKey& conn_key);
  bool tryAddOpeningConn(
      const PoolKey& conn_key,
      std::shared_ptr<db::ConnectionContextBase> context,
      size_t enqueued_pool_ops,
      uint32_t client_total_conns,
      uint64_t client_conn_limit,
      ThrottlingCallback throttlingCallback);

  void removeOpenConnection(const PoolKey& conn_key);
  void removeOpeningConn(const PoolKey& conn_key);

  // For debugging
  void displayOpenConnections();
  int getNumKeysInOpenConnections();
  int getNumKeysInPendingConnections();

  uint32_t getNumOpenConnections() const noexcept {
    return counters_.rlock()->num_open_connections;
  }

  uint32_t getNumPendingConnections() const noexcept {
    return counters_.rlock()->num_pending_connections;
  }

  static constexpr bool implementsPooling() {
    return true;
  }

  void setShouldThrottleCallback(ShouldThrottleCallback cb) {
    shouldThrottleCallback_ = std::move(cb);
  }

  virtual bool isInCorrectThread(bool /*expectMysqlThread*/) const {
    return true;
  }

 protected:
  virtual bool isShuttingDown() const = 0;

  virtual void connectionSpotFreed(const PoolKey& conn_key) = 0;

  struct Counters {
    uint32_t num_open_connections = 0;
    // Counts the number of open connections for a given connectionKey
    folly::F14FastMap<PoolKey, uint64_t, PoolKeyHash> open_connections;
    // Same as above but for connections that we are still opening
    // This one doesn't need locking, only accessed by client thread
    uint32_t num_pending_connections = 0;
    folly::F14FastMap<PoolKey, uint64_t, PoolKeyHash> pending_connections;
  };

  bool canCreateMoreConnections(
      const PoolKey& pool_key,
      size_t enqueued_pool_ops,
      uint32_t client_total_conns,
      uint64_t client_conn_limit) const {
    return counters_.withRLock([&](const auto& counters) {
      return canCreateMoreConnections(
          pool_key,
          enqueued_pool_ops,
          client_total_conns,
          client_conn_limit,
          counters);
    });
  }

 private:
  template <typename Client>
  friend class MysqlPooledHolder;

  bool canCreateMoreConnections(
      const PoolKey& pool_key,
      size_t enqueued_pool_ops,
      uint32_t client_total_conns,
      uint64_t client_conn_limit,
      const Counters& counters) const;

  folly::Synchronized<PoolOptions> pool_options_;

  folly::Synchronized<Counters> counters_;

  ShouldThrottleCallback shouldThrottleCallback_;

  // Counters for connections created, cache hits and misses, etc.
  db::PoolStats pool_stats_;
};

//  This pool manages and creates mysql connections using the async or sync
//  client. When using the sync client the client's thread is used.  Multiple
//  pools can use the same client. The pool MUST always be acquired by using
//  `makePool`.
//
// How a connection is acquired:
//   First `beginConnection` is called as we always do with the client. Using
// the pool will only change the type of the returned Operation,
// `ConnectPoolOperation<Client>`. Different from ConnectOperation, instead of
// opening a new connection on `run()`, the pool operation will call
// `registerForConnection`. The pool will then check if there are any spare
// connections available to return or create a new one (based on the limits).
// In the event of not having any available connection later scenario, the
// operation will be queued until a connection is ready for it.
// With the connection in hands, the pool will assign it to the operation
// by calling `connectionCallback`. The last call will work like a
// `socketActionable` call triggering the complete operation procedures.
//   For the user the usage is the same as acquiring a connection using
// `AsyncMysqlClient` or `SyncMysqlClient`, but now using the pool.
template <typename Client>
class ConnectionPool
    : public ConnectionPoolBase,
      public std::enable_shared_from_this<ConnectionPool<Client>> {
 public:
  using ClientType = Client;

  ConnectionPool(std::shared_ptr<Client> mysql_client, PoolOptions pool_options)
      : ConnectionPoolBase(std::move(pool_options)),
        mysql_client_(std::move(mysql_client)),
        conn_storage_(totalLimit(), idleTimeout()) {}

  virtual ~ConnectionPool() override {}

  // Don't allow copy or move
  ConnectionPool(const ConnectionPool&) = delete;
  ConnectionPool& operator=(const ConnectionPool&) = delete;

  ConnectionPool(ConnectionPool&&) = delete;
  ConnectionPool& operator=(ConnectionPool&&) = delete;

  virtual void shutdown() = 0;

  // Returns the client that this pool is using
  std::shared_ptr<Client> getMysqlClient() {
    return mysql_client_;
  }

  // Returns a ConnectPoolOperation that will abstract the wait for the client
  // to find or create a connection for the operation.
  // In shutting down mode, this will return a cancelled operation
  // (same as the client).
  std::shared_ptr<ConnectOperation> beginConnection(
      const std::string& host,
      int port,
      const std::string& database_name,
      const std::string& user,
      const std::string& password,
      const std::string& special_tag = "") {
    return beginConnection(
        std::make_shared<const MysqlConnectionKey>(
            host,
            port,
            database_name,
            user,
            password,
            special_tag,
            poolPerMysqlInstance()));
  }

  std::shared_ptr<ConnectOperation> beginConnection(
      std::shared_ptr<const ConnectionKey> conn_key) {
    auto impl = createConnectPoolOperationImpl(
        getSelfWeakPointer(), mysql_client_, std::move(conn_key));
    auto ret = std::make_shared<ConnectPoolOperation<Client>>(std::move(impl));
    if (isShuttingDown()) {
      LOG(ERROR)
          << "Attempt to start pool operation while pool is shutting down";
      ret->cancel();
    }
    return ret;
  }

  [[nodiscard]] size_t getNumKey() const noexcept {
    return conn_storage_.getNumKey();
  }

 protected:
  // Checks if the limits (global, connections open or being open by pool, or
  // limit per key) can fit one more connection. As a final check, checks if
  // it's a waste to create a new connection to avoid start opening a new
  // connection when we already have enough being open for the demand in
  // queue.
  bool canCreateMoreConnections(const PoolKey& conn_key) const {
    validateCorrectThread();
    return ConnectionPoolBase::canCreateMoreConnections(
        conn_key,
        numQueuedOperations(conn_key),
        mysql_client_->numStartedAndOpenConnections(),
        mysql_client_->getPoolsConnectionLimit());
  }

  bool tryAddOpeningConn(
      const PoolKey& conn_key,
      std::shared_ptr<db::ConnectionContextBase> context,
      ThrottlingCallback throttlingCallback) {
    return ConnectionPoolBase::tryAddOpeningConn(
        conn_key,
        std::move(context),
        numQueuedOperations(conn_key),
        mysql_client_->numStartedAndOpenConnections(),
        mysql_client_->getPoolsConnectionLimit(),
        std::move(throttlingCallback));
  }

  void registerForConnection(ConnectPoolOperation<Client>* raw_pool_op) {
    validateCorrectThread();
    if (isShuttingDown()) {
      VLOG(4) << "Pool is shutting down, operation being canceled";
      raw_pool_op->cancel();
      return;
    }

    stats()->incrConnectionsRequested();
    // Pass that to pool
    auto pool_key =
        PoolKey(raw_pool_op->getKey(), raw_pool_op->getConnectionOptions());

    std::unique_ptr<MysqlPooledHolder<Client>> mysql_conn =
        conn_storage_.popConnection(pool_key);

    if (mysql_conn == nullptr) {
      // One more try before opening a new connection: finding a connection from
      // level2 cache and if found, run COM_CHANGE_USER
      if (raw_pool_op->getConnectionOptions().isEnableChangeUser()) {
        auto ret = conn_storage_.popInstanceConnection(pool_key);
        if (ret) { // found
          reuseConnWithChangeUser(raw_pool_op, pool_key, std::move(ret));
          return;
        }
      }
      openNewConnection(raw_pool_op, pool_key);
    } else {
      // Cache hit
      stats()->incrPoolHits();

      mysql_conn->setFromPoolHit(true);
      mysql_conn->setReusable(true);
      mysql_conn->resetLastActivityTime();
      raw_pool_op->connectionCallback(std::move(mysql_conn));
    }
  }

  // Used internally when want a new connection. It checks if we should open
  // more connections, if so it creates ConnectOperation and once the
  // operation is completed the callback will call `addConnection`.
  // If the expiration policy is age, here we set the life limit of the
  // connection.
  // If we fail in creating the connection, `failedToConnect` will be called.
  // TODO#4527126: maybe have a cache for the unreachable hosts to fail faster
  // these connections.
  void tryRequestNewConnection(
      const PoolKey& pool_key,
      std::shared_ptr<db::ConnectionContextBase> context = nullptr,
      ThrottlingCallback throttlingCallback = nullptr) {
    validateCorrectThread();

    // Only called internally, this doesn't need to check if it's shutting
    // down
    if (isShuttingDown()) {
      return;
    }

    // Checking if limits allow creating more connections
    if (!tryAddOpeningConn(pool_key, context, std::move(throttlingCallback))) {
      return;
    }

    VLOG(11) << "Requesting new Connection";
    // get a shared pointer for operation

    auto connOp = mysql_client_->beginConnection(pool_key.getConnectionKey());
    connOp->setConnectionOptions(pool_key.getConnectionOptions());
    if (!context) {
      context = std::make_shared<db::ConnectionContextBase>();
    }
    connOp->setConnectionContext(std::move(context));

    // ADRIANA The attribute part we can do later :D time to do it
    connOp->setCallback(
        [pool_key, pool_ptr = getSelfWeakPointer()](ConnectOperation& connOp) {
          auto locked_pool = pool_ptr.lock();
          if (!locked_pool) {
            return;
          }
          if (!connOp.ok()) {
            VLOG(2) << "Failed to create new connection";
            locked_pool->removeOpeningConn(pool_key);
            locked_pool->failedToConnect(pool_key, connOp);
            return;
          }
          auto conn = connOp.releaseConnection();
          auto mysql_conn = conn->stealConnectionHolder();
          // Now we got a connection from the client, it will become a pooled
          // connection
          auto pooled_conn = std::make_unique<MysqlPooledHolder<Client>>(
              std::move(mysql_conn), pool_ptr, pool_key);
          locked_pool->removeOpeningConn(pool_key);
          locked_pool->addConnection(std::move(pooled_conn), true);
        });

    try {
      connOp->run();
    } catch (db::OperationStateException& e) {
      LOG(ERROR)
          << "Client is drain or dying, cannot ask for more connections: "
          << e.what();
    }
  }

  void reuseConnWithChangeUser(
      ConnectPoolOperation<Client>* rawPoolOp,
      const PoolKey& poolKey,
      std::unique_ptr<MysqlPooledHolder<Client>> mysqlConn) {
    auto conn = makeNewConnection(rawPoolOp->getKey(), std::move(mysqlConn));
    conn->needToCloneConnection_ = false;
    auto changeUserOp =
        Connection::changeUser(std::move(conn), poolKey.getConnectionKey());

    changeUserOp->setCallback(
        [this, rawPoolOp, poolKey, poolPtr = getSelfWeakPointer()](
            SpecialOperation& op, OperationResult result) {
          rawPoolOp->resetPreOperation();

          if (result == OperationResult::Failed) {
            openNewConnection(rawPoolOp, poolKey);
            return;
          }

          auto connection = op.releaseConnection();
          auto mysqlConn = connection->stealConnectionHolder(true);
          mysqlConn->updateConnectionKey(poolKey.getConnectionKey());
          auto pooledConn = std::make_unique<MysqlPooledHolder<Client>>(
              std::move(mysqlConn), poolPtr, poolKey);
          stats()->incrPoolHits();
          stats()->incrPoolHitsChangeUser();

          pooledConn->setFromPoolHit(true);
          pooledConn->setReusedWithChangeUser(true);
          pooledConn->setReusable(true);
          pooledConn->resetCreationTime();
          rawPoolOp->connectionCallback(std::move(pooledConn));
        });

    // Failing to set pre-operation means rawPoolOp already fails (times out,
    // etc) and its pre-operation is already cancelled.
    if (!rawPoolOp->setPreOperation(changeUserOp)) {
      return;
    }

    // For async - run in mysql thread; for sync - run in current thread
    runInCorrectThread(
        [changeUserOp = std::move(changeUserOp)] { changeUserOp->run(); });
  }

  void connectionSpotFreed(const PoolKey& conn_key) override {
    runInCorrectThread([weak_pool = getSelfWeakPointer(), conn_key]() {
      auto pool = weak_pool.lock();
      if (pool) {
        pool->tryRequestNewConnection(conn_key);
      }
    });
  }

  void openNewConnection(
      ConnectPoolOperation<Client>* rawPoolOp,
      const PoolKey& poolKey) {
    stats()->incrPoolMisses();
    // TODO: Check if we are jammed and fail fast

    // The client holds shared pointers for all active operations
    // this method is called by the `run()` in the operation, so it
    // should always exist in the client
    auto pool_op = std::dynamic_pointer_cast<ConnectPoolOperation<Client>>(
        rawPoolOp->getSharedPointer());
    // Sanity check
    DCHECK(pool_op != nullptr);

    openNewConnectionPrep(*pool_op);

    conn_storage_.queueOperation(poolKey, pool_op);
    // Propagate the ConnectionContext from the incoming operation. These
    // contexts contain application specific logging that will be lost if not
    // passed to the new ConnectOperation that is spawned to fulfill the pool
    // miss. Both operations then log with the expected additional logging.
    //
    // Each gets its own copy rather than a shared pointer: both run the same
    // completion path, which writes connection details into the context, and
    // they may complete on different threads.
    tryRequestNewConnection(
        poolKey,
        pool_op->cloneConnectionContext(),
        [&](uint32_t errnum, const std::string& error) {
          pool_op->setAsyncClientError(errnum, error);
          pool_op->attemptFailed(OperationResult::Failed);

          // We are still in the same thread so no one else should have had a
          // chance to dequeue the operation yet.
          conn_storage_.dequeueOperation(poolKey, *pool_op);
        });

    openNewConnectionFinish(*pool_op, poolKey);
  }

  void resetConnection(
      ConnectPoolOperation<Client>* rawPoolOp,
      const PoolKey& poolKey,
      std::unique_ptr<MysqlPooledHolder<Client>> mysqlConn) {
    auto conn = makeNewConnection(rawPoolOp->getKey(), std::move(mysqlConn));
    conn->needToCloneConnection_ = false;
    auto resetOp = Connection::resetConn(std::move(conn));

    resetOp->setCallback([this, rawPoolOp, poolKey](
                             SpecialOperation& op, OperationResult result) {
      rawPoolOp->resetPreOperation();

      if (result == OperationResult::Failed) {
        openNewConnection(rawPoolOp, poolKey);
        return;
      }

      auto connection = op.releaseConnection();
      auto mysqlConnHolder = connection->stealConnectionHolder(true);
      auto pmysqlConn = mysqlConnHolder.release();
      std::unique_ptr<MysqlPooledHolder<Client>> mysqlConnection(
          static_cast<MysqlPooledHolder<Client>*>(pmysqlConn));

      stats()->incrPoolHits();
      mysqlConnection->setReusable(true);
      mysqlConnection->resetCreationTime();
      rawPoolOp->connectionCallback(std::move(mysqlConnection));
    });

    // Failing to set pre-operation means rawPoolOp already fails
    if (!rawPoolOp->setPreOperation(resetOp)) {
      return;
    }

    // For async - run in mysql thread; for sync - run in current thread
    runInCorrectThread([resetOp = std::move(resetOp)]() { resetOp->run(); });
  }

 protected:
  std::weak_ptr<ConnectionPool<Client>> getSelfWeakPointer() {
    return this->weak_from_this();
  }

  virtual void validateCorrectThread() const = 0;

  virtual bool runInCorrectThread(
      std::function<void()>&& func,
      bool wait = false) = 0;

  size_t numQueuedOperations(const PoolKey& pool_key) const {
    return conn_storage_.numQueuedOperations(pool_key);
  }

  std::shared_ptr<Client> mysql_client_;

  PoolStorage<Client> conn_storage_;

 private:
  template <typename C>
  friend class ConnectPoolOperationImpl;
  template <typename C>
  friend class mysql_protocol::MysqlConnectPoolOperationImpl;

  void recycleMysqlConnection(std::unique_ptr<ConnectionHolder> mysql_conn) {
    // this method can run by any thread where the Connection is dying
    if (isShuttingDown()) {
      return;
    }

    VLOG(2) << "Trying to recycle connection";

    if (!mysql_conn->isReusable()) {
      return;
    }

    // Check server_status for in_transaction bit
    if (mysql_conn->inTransaction()) {
      // To avoid complication, we are just going to close the connection
      SQUANGLE_LOG_EVERY_N_ATOMIC(INFO, 1024)
          << "Closing connection during a transaction."
          << " Transaction will rollback.";
      return;
    }

    auto pmysql_conn = mysql_conn.release();
    bool scheduled =
        runInCorrectThread([pool = getSelfWeakPointer(), pmysql_conn]() {
          std::unique_ptr<MysqlPooledHolder<Client>> mysql_connection(
              static_cast<MysqlPooledHolder<Client>*>(pmysql_conn));
          auto shared_pool = pool.lock();
          if (!shared_pool) {
            return;
          }

          // in mysql 5.7 we can use mysql_reset_connection
          // We don't have a nonblocking version for reset connection, so we
          // are going to delete the old one and the open connection being
          // removed procedure is going to check if it needs to open new one
          shared_pool->addConnection(std::move(mysql_connection), false);
        });

    if (!scheduled) {
      delete pmysql_conn;
    }
  }

  // Anytime a connection is supposed to be added to the pool, being fresh or
  // recycled, we check if there is an operation in wait list for the
  // ConnectionKey inside MysqlPooledHolder, if there is, the match is made
  // here. Otherwise it is added to the pool.
  // At this point, the connection should already have been cleaned.
  // Should only be used internally
  void addConnection(
      std::unique_ptr<MysqlPooledHolder<Client>> mysql_conn,
      bool brand_new) {
    // Only called internally, this doesn't need to check if it's shutting
    // down
    validateCorrectThread();
    if (brand_new && expirationPolicy() == ExpirationPolicy::Age) {
      // TODO add noise to expiration age
      mysql_conn->setLifeDuration(ageTimeout());
    }

    VLOG(11) << "New connection ready to be used";
    auto pool_op = conn_storage_.popOperation(mysql_conn->getPoolKey());
    if (pool_op == nullptr) {
      VLOG(11) << "No operations waiting for Connection, enqueueing it";
      conn_storage_.queueConnection(std::move(mysql_conn));
    } else {
      mysql_conn->setFromPoolHit(!brand_new);
      mysql_conn->setReusable(true);
      mysql_conn->resetLastActivityTime();
      pool_op->connectionCallback(std::move(mysql_conn));
    }
  }

  // Used for when we fail to open a requested connection. In case of mysql
  // failure (e.g. bad password) we propagate the error to all queued
  // AsyncConnectPoolOperation's.
  void failedToConnect(const PoolKey& pool_key, ConnectOperation& conn_op) {
    // Propagating ConnectOperation failure to queued operations in case
    // This will help us fail fast incorrect passwords or users.
    if (conn_op.result() == OperationResult::Failed) {
      conn_storage_.failOperations(
          pool_key,
          conn_op.result(),
          conn_op.mysql_errno(),
          conn_op.mysql_error());
    }
    connectionSpotFreed(pool_key);
  }

  virtual void openNewConnectionPrep(ConnectPoolOperation<Client>& pool_op) = 0;

  virtual void openNewConnectionFinish(
      ConnectPoolOperation<Client>& pool_op,
      const PoolKey& pool_key) = 0;

  virtual std::unique_ptr<Connection> makeNewConnection(
      std::shared_ptr<const ConnectionKey> conn_key,
      std::unique_ptr<MysqlPooledHolder<Client>> mysqlConn) = 0;
};

template <typename Client>
std::unique_ptr<ConnectPoolOperationImpl<Client>>
createConnectPoolOperationImpl(
    std::weak_ptr<ConnectionPool<Client>> pool,
    std::shared_ptr<Client> client,
    std::shared_ptr<const ConnectionKey> conn_key);

} // namespace facebook::common::mysql_client
