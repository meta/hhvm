/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/fibers/Baton.h>
#include <folly/futures/Future.h>
#include <folly/io/async/EventBase.h>

#include "squangle/base/Base.h"
#include "squangle/mysql_client/ConnectionHolder.h"
#include "squangle/mysql_client/ConnectionOptions.h"
#include "squangle/mysql_client/MultiQueryOperation.h"
#include "squangle/mysql_client/MultiQueryStreamOperation.h"
#include "squangle/mysql_client/MysqlClientBase.h"
#include "squangle/mysql_client/Operation.h"
#include "squangle/mysql_client/Query.h"
#include "squangle/mysql_client/QueryGenerator.h"
#include "squangle/mysql_client/QueryOperation.h"

namespace facebook::common::mysql_client {

class ChangeUserOperation;
class ResetOperation;
class MultiQueryStreamHandler;

namespace mysql_protocol {
class MysqlResetOperation;
class MysqlChangeUserOperation;
} // namespace mysql_protocol

using ConnectionDyingCallback =
    std::function<void(std::unique_ptr<ConnectionHolder>)>;

// Connection is a thin wrapper around a MYSQL object, associating it
// with an AsyncMysqlClient.  Its primary purpose is to manage that
// connection and initiate queries.
//
// It also holds a notification descriptor, used across queries, to
// signal their completion.  Operation::wait blocks on this fd.
class Connection {
 public:
  Connection(
      MysqlClientBase& mysql_client,
      std::shared_ptr<const ConnectionKey> conn_key,
      std::unique_ptr<ConnectionHolder> conn)
      : mysql_connection_(std::move(conn)),
        conn_key_(std::move(conn_key)),
        mysql_client_(mysql_client),
        initialized_(false) {}

  virtual ~Connection();

  // Don't allow copy or move
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  Connection(Connection&&) = delete;
  Connection& operator=(Connection&&) = delete;

  // Like beginConnection, this is how you start a query.  Note that
  // ownership of the Connection is passed into this function; the
  // returned QueryOperation allows access to it (once the query
  // completes).  This is a limitation of MySQL as you cannot perform
  // operations while a query is in progress.  We use unique_ptr to
  // represent this connection-level statefulness.
  //
  // To run subsequent queries, after query_op->wait() returns, you
  // can call query_op->releaseConnection() to retrieve the connection
  // itself and run further queries.
  //
  // The query itself is constructed from args....  If args... is a single
  // Query object, it is used directly; otherwise a Query object is
  // constructed via Query(args...) and that is used for the query.
  template <typename... Args>
  [[nodiscard]] static std::shared_ptr<QueryOperation> beginQuery(
      std::unique_ptr<Connection> conn,
      Args&&... args);

  template <typename... Args>
  [[nodiscard]] static std::shared_ptr<QueryOperation>
  beginQueryWithLoggingFuncs(
      std::unique_ptr<Connection> conn,
      LoggingFuncsPtr logging_funcs,
      Args&&... args);

  template <typename... Args>
  [[nodiscard]] static std::shared_ptr<MultiQueryOperation> beginMultiQuery(
      std::unique_ptr<Connection> conn,
      Args&&... args) {
    return beginMultiQueryWithLoggingFuncs(
        std::move(conn), nullptr, std::forward<Args>(args)...);
  }

  template <typename... Args>
  [[nodiscard]] static std::shared_ptr<MultiQueryOperation>
  beginMultiQueryWithLoggingFuncs(
      std::unique_ptr<Connection> conn,
      LoggingFuncsPtr logging_funcs,
      Args&&... args);

  [[nodiscard]] static folly::SemiFuture<DbQueryResult> querySemiFuture(
      std::unique_ptr<Connection> conn,
      Query&& query,
      QueryCallback&& cb,
      QueryOptions&& options = QueryOptions());

  [[nodiscard]] static folly::SemiFuture<DbQueryResult> querySemiFuture(
      std::unique_ptr<Connection> conn,
      Query&& query,
      QueryOptions&& options = QueryOptions()) {
    return querySemiFuture(
        std::move(conn), std::move(query), nullptr, std::move(options));
  }

  [[nodiscard]] static folly::SemiFuture<DbMultiQueryResult>
  multiQuerySemiFuture(
      std::unique_ptr<Connection> conn,
      Query&& query,
      MultiQueryCallback&& cb,
      QueryOptions&& options = QueryOptions());

  [[nodiscard]] static folly::SemiFuture<DbMultiQueryResult>
  multiQuerySemiFuture(
      std::unique_ptr<Connection> conn,
      Query&& query,
      QueryOptions&& options = QueryOptions()) {
    return multiQuerySemiFuture(
        std::move(conn), std::move(query), nullptr, std::move(options));
  }

  [[nodiscard]] static folly::SemiFuture<DbMultiQueryResult>
  multiQuerySemiFuture(
      std::unique_ptr<Connection> conn,
      std::vector<Query>&& queries,
      MultiQueryCallback&& cb,
      QueryOptions&& options = QueryOptions());

  [[nodiscard]] static folly::SemiFuture<DbMultiQueryResult>
  multiQuerySemiFuture(
      std::unique_ptr<Connection> conn,
      std::vector<Query>&& queries,
      QueryOptions&& options = QueryOptions()) {
    return multiQuerySemiFuture(
        std::move(conn), std::move(queries), nullptr, std::move(options));
  }

  [[nodiscard]] virtual std::shared_ptr<MultiQueryStreamOperation>
  createOperation(
      std::unique_ptr<OperationBase::ConnectionProxy> proxy,
      MultiQuery&& multi_query) {
    auto impl = client().createFetchOperationImpl(
        std::move(proxy), db::OperationType::MultiQueryStream, nullptr);
    return MultiQueryStreamOperation::create(
        std::move(impl), std::move(multi_query));
  }

  template <typename... Args>
  [[nodiscard]] static std::shared_ptr<MultiQueryStreamOperation>
  beginMultiQueryStreaming(std::unique_ptr<Connection> conn, Args&&... args);

  // Synchronous calls
  template <typename... Args>
  DbQueryResult query(Args&&... args);

  template <typename... Args>
  DbMultiQueryResult multiQuery(Args&&... args);

  template <typename... Args>
  DbQueryResult queryWithGenerator(
      QueryGenerator&& query_generator,
      Args&&... args);

  template <typename... Args>
  DbQueryResult queryWithGenerator(
      QueryGenerator& query_generator,
      Args&&... args);

  DbQueryResult queryWithGenerator(
      QueryGenerator& query_generator,
      QueryCallback&& cb,
      QueryOptions&& options);

  template <typename... Args>
  DbMultiQueryResult multiQueryWithGenerators(
      std::vector<std::unique_ptr<QueryGenerator>>&& query_generators,
      Args&&... args);

  template <typename... Args>
  DbMultiQueryResult multiQueryWithGenerator(
      std::unique_ptr<QueryGenerator>&& query_generator,
      Args&&... args);

  [[nodiscard]] static std::unique_ptr<MultiQueryStreamHandler>
  streamMultiQuery(
      std::unique_ptr<Connection> connection,
      std::vector<Query> queries,
      const AttributeMap& attributes = AttributeMap());

  [[nodiscard]] static std::unique_ptr<MultiQueryStreamHandler>
  streamMultiQuery(
      std::unique_ptr<Connection> connection,
      Query query,
      const AttributeMap& attributes = AttributeMap());

  // Called in the libevent thread to create the MYSQL* client.
  void initMysqlOnly();
  void initialize(bool initMysql = true);

  [[nodiscard]] bool hasInitialized() const noexcept {
    return initialized_;
  }

  [[nodiscard]] bool ok() const noexcept {
    return mysql_connection_ != nullptr;
  }

  void close() noexcept {
    if (mysql_connection_) {
      mysql_connection_.reset();
    }
  }

  // Default timeout for queries created by this client.
  void setDefaultQueryTimeout(Duration t) {
    conn_options_.setQueryTimeout(t);
  }
  // TODO #9834064
  void setQueryTimeout(Duration t) {
    conn_options_.setQueryTimeout(t);
  }

  // Reset last successful query time to ConnectionHolder
  void resetLastActivityTime() {
    CHECK_THROW(mysql_connection_ != nullptr, db::InvalidConnectionException);
    mysql_connection_->resetLastActivityTime();
  }

  [[nodiscard]] Timepoint getLastActivityTime() const {
    CHECK_THROW(mysql_connection_ != nullptr, db::InvalidConnectionException);
    return mysql_connection_->getLastActivityTime();
  }

  [[nodiscard]] std::chrono::milliseconds getConnIdleTime() const noexcept {
    return mysql_connection_ ? mysql_connection_->getConnIdleTime()
                             : std::chrono::milliseconds{0};
  }

  // Pool-source observability
  [[nodiscard]] bool wasFromPoolHit() const noexcept {
    return mysql_connection_ ? mysql_connection_->wasFromPoolHit() : false;
  }
  [[nodiscard]] bool wasReusedWithChangeUser() const noexcept {
    return mysql_connection_ ? mysql_connection_->wasReusedWithChangeUser()
                             : false;
  }

  // Returns the MySQL server version. If the connection has been closed
  // an error is generated.
  [[nodiscard]] std::string serverInfo() const {
    if (!mysql_connection_) {
      return "";
    }

    return mysql_connection_->serverInfo();
  }

  // Returns whether or not the SSL session was reused from a previous
  // connection.
  // If the connection isn't SSL, it will return false as well.
  [[nodiscard]] bool sslSessionReused() const {
    CHECK_THROW(mysql_connection_ != nullptr, db::InvalidConnectionException);
    return mysql_connection_->sslSessionReused();
  }

  [[nodiscard]] std::string getTlsVersion() const {
    CHECK_THROW(mysql_connection_ != nullptr, db::InvalidConnectionException);
    return mysql_connection_->getTlsVersion();
  }

  // Checks if `client_flag` is set for SSL.
  [[nodiscard]] bool isSSL() const;

  // Escape the provided string using mysql_real_escape_string_quote() with a
  // single quote. You almost certainly don't want to use this - look at the
  // Query class instead.
  //
  // This is provided so that non-Facebook users of the HHVM extension have
  // a familiar API.
  [[nodiscard]] std::string escapeString(folly::StringPiece unescaped) const {
    CHECK_THROW(mysql_connection_ != nullptr, db::InvalidConnectionException);
    return mysql_connection_->InternalConnection::escapeString(unescaped);
  }

  // Returns the number of errors, warnings, and notes generated during
  // execution of the previous SQL statement
  [[nodiscard]] unsigned int warningCount() const {
    CHECK_THROW(mysql_connection_ != nullptr, db::InvalidConnectionException);
    return mysql_connection_->warningCount();
  }

  [[nodiscard]] const std::string& host() const {
    return getKeyRef().host();
  }
  [[nodiscard]] int port() const {
    return getKeyRef().port();
  }
  [[nodiscard]] const std::string& user() const {
    return getKeyRef().user();
  }
  [[nodiscard]] const std::string& database() const {
    return getKeyRef().db_name();
  }

  [[nodiscard]] const std::string& password() const {
    return getKeyRef().password();
  }

  [[nodiscard]] MysqlClientBase& client() const noexcept {
    return mysql_client_;
  }

  [[nodiscard]] long mysqlThreadId() const {
    CHECK_THROW(mysql_connection_ != nullptr, db::InvalidConnectionException);
    return mysql_connection_->threadId();
  }

  // Whether the session is in autocommit mode, from the server status the
  // server last reported.
  [[nodiscard]] bool getAutocommit() const {
    CHECK_THROW(mysql_connection_ != nullptr, db::InvalidConnectionException);
    return mysql_connection_->getAutocommit();
  }

  [[nodiscard]] ConnectionHolder* mysql_for_testing_only() const {
    return mysql_connection_.get();
  }

  [[nodiscard]] std::unique_ptr<ConnectionHolder> stealConnectionHolder(
      bool skipCheck = false) {
    if (!skipCheck) {
      DCHECK(isInEventBaseThread());
    }
    return std::move(mysql_connection_);
  }

  [[nodiscard]] std::shared_ptr<const ConnectionKey> getKey() const {
    return conn_key_;
  }

  [[nodiscard]] const ConnectionKey& getKeyRef() const {
    return *conn_key_;
  }

  void setReusable(bool reusable) {
    if (mysql_connection_) {
      mysql_connection_->setReusable(reusable);
    }
  }

  [[nodiscard]] bool isReusable() const {
    if (mysql_connection_) {
      return mysql_connection_->isReusable();
    }
    return false;
  }

  // Don't actually store the new schema at this point - just the presence of
  // an updated schema is sufficient to indicate we shouldn't reuse the
  // connection for pooling as it means that someone ran a "USE <dbname>"
  // command.
  void setCurrentSchema(std::string_view /*schema*/) const {
    CHECK_THROW(mysql_connection_ != nullptr, db::InvalidConnectionException);
    mysql_connection_->setReusable(false);
  }

  [[nodiscard]] bool inTransaction() const {
    CHECK_THROW(mysql_connection_ != nullptr, db::InvalidConnectionException);
    return mysql_connection_->inTransaction();
  }

  [[nodiscard]] const ConnectionOptions& getConnectionOptions() const {
    return conn_options_;
  }

  void setConnectionOptions(const ConnectionOptions& conn_options) {
    conn_options_ = conn_options;
  }

  void setKillOnQueryTimeout(bool killOnQueryTimeout) {
    killOnQueryTimeout_ = killOnQueryTimeout;
  }

  [[nodiscard]] bool getKillOnQueryTimeout() {
    return killOnQueryTimeout_;
  }

  void setConnectionDyingCallback(ConnectionDyingCallback callback) {
    conn_dying_callback_ = std::move(callback);
  }

  // Note that the chained callback is invoked in the MySQL client thread
  // and so any callback should execute *very* quickly and not block
  void setPreOperationCallback(ChainedCallback&& callback) {
    callbacks_.pre_operation_callback_ = setCallback(
        std::move(callbacks_.pre_operation_callback_), std::move(callback));
  }

  void setPostOperationCallback(ChainedCallback&& callback) {
    callbacks_.post_operation_callback_ = setCallback(
        std::move(callbacks_.post_operation_callback_), std::move(callback));
  }

  void setCallbacks(OperationBase::Callbacks&& callbacks) {
    callbacks_.pre_operation_callback_ = setCallback(
        std::move(callbacks_.pre_operation_callback_),
        std::move(callbacks.pre_operation_callback_));
    callbacks_.post_operation_callback_ = setCallback(
        std::move(callbacks_.post_operation_callback_),
        std::move(callbacks.post_operation_callback_));
    callbacks_.pre_query_callback_ = OperationBase::appendCallback(
        std::move(callbacks_.pre_query_callback_),
        std::move(callbacks.pre_query_callback_));
    callbacks_.post_query_callback_ = OperationBase::appendCallback(
        std::move(callbacks_.post_query_callback_),
        std::move(callbacks.post_query_callback_));
    callbacks_.render_prefix_callback_ =
        std::move(callbacks.render_prefix_callback_);
    callbacks_.post_render_callback_ =
        std::move(callbacks.post_render_callback_);
  }

  void setPostRenderCallback(PostRenderCallback&& callback) {
    callbacks_.post_render_callback_ = std::move(callback);
  }

  [[nodiscard]] const folly::EventBase* getEventBase() const {
    return client().getEventBase();
  }

  [[nodiscard]] folly::EventBase* getEventBase() {
    return client().getEventBase();
  }

  [[nodiscard]] bool isInEventBaseThread() const {
    return isInCorrectThread(true);
  }

  [[nodiscard]] bool isNotInEventBaseThread() const {
    return isInCorrectThread(false);
  }

  [[nodiscard]] bool isInCorrectThread(bool expectMysqlThread) const {
    return client().isInCorrectThread(expectMysqlThread);
  }

  virtual bool runInThread(std::function<void()>&& fn) {
    return client().runInThread(std::move(fn));
  }

  template <typename TOp, typename... F, typename... T>
  bool runInThread(TOp* op, void (TOp::*f)(F...), T&&... v) {
    // short circuit
    if (isInEventBaseThread()) {
      (op->*f)(std::forward<T>(v)...);
      return true;
    } else {
      return runInThread(std::bind(f, op, v...));
    }
  }

  // Operations call these methods as the operation becomes unblocked, as
  // callers want to wait for completion, etc.
  virtual void notify() {}
  virtual void wait() const {}
  // Called when a new operation is being started.
  virtual void resetActionable() {}

  // Helper to set up pre/post operation callbacks on a FetchOperation
  template <typename OpType>
  void setupOperationCallbacks(OpType& op, Connection& conn);

  void setConnectionHolder(std::unique_ptr<ConnectionHolder> conn) {
    CHECK_THROW(mysql_connection_ == nullptr, db::InvalidConnectionException);
    CHECK_THROW(*getKey() == *conn->getKey(), db::InvalidConnectionException);
    mysql_connection_ = std::move(conn);
  }

  // If this is set and other necessary conditions are met, we clone
  // Connection object in its destructor to properly reset the connection by
  // sending COM_RESET_CONNECTION before it is completed destructed.
  bool needToCloneConnection_{true};

  static std::shared_ptr<ResetOperation> resetConn(
      std::unique_ptr<Connection> conn);

  static std::shared_ptr<ChangeUserOperation> changeUser(
      std::unique_ptr<Connection> conn,
      std::shared_ptr<const ConnectionKey> key);

  // The returned pointer is only valid for the lifetime of this Connection.
  const db::ConnectionContextBase* getConnectionContext() const {
    return connection_context_.get();
  }

  std::shared_ptr<const db::ConnectionContextBase> getConnectionContextShared()
      const {
    return connection_context_;
  }

  void setPersistentQueryAttributes(QueryAttributes attrs) {
    persistentQueryAttributes_ = std::move(attrs);
  }

  const QueryAttributes& getPersistentQueryAttributes() const {
    return persistentQueryAttributes_;
  }

 protected:
  // Methods primarily invoked by Operations and AsyncMysqlClient.
  friend class AsyncMysqlClient;
  friend class SyncMysqlClient;
  friend class MysqlClientBase;
  friend class OperationBase;
  friend class ConnectOperationImpl;
  template <typename Client>
  friend class ConnectPoolOperation;
  friend class FetchOperationImpl;
  friend class QueryOperationImpl;
  friend class MultiQueryOperationImpl;
  friend class MultiQueryStreamHandler;
  friend class MultiQueryStreamOperation;
  friend class SpecialOperationImpl;
  friend class ResetOperation;
  friend class ChangeUserOperation;
  friend class ConnectionHolder;

  // PR3: Unified protocol-specific operation classes need access to
  // getInternalConnection()
  friend class mysql_protocol::MysqlResetOperation;
  friend class mysql_protocol::MysqlChangeUserOperation;

  virtual std::unique_ptr<InternalConnection> createInternalConnection() = 0;

  [[nodiscard]] ChainedCallback setCallback(
      ChainedCallback orgCallback,
      ChainedCallback newCallback) {
    if (!orgCallback) {
      return newCallback;
    }

    if (!newCallback) {
      return orgCallback;
    }

    return [orgCallback = std::move(orgCallback),
            newCallback = std::move(newCallback)](Operation& op) mutable {
      orgCallback(op);
      newCallback(op);
    };
  }

  [[nodiscard]] ConnectionHolder* mysqlConnection() const {
    DCHECK(isInEventBaseThread());
    return mysql_connection_.get();
  }

  void checkOperationInProgress() {
    if (operation_in_progress_) {
      throw db::InvalidConnectionException(
          "Attempting to run parallel queries in same connection");
    }
  }

  void setConnectionContext(
      std::shared_ptr<db::ConnectionContextBase> context) {
    connection_context_ = std::move(context);
  }

  void mergePersistentQueryAttributes(QueryAttributes& attrs) const;

  void disableCloseOnDestroy() {
    if (mysql_connection_) {
      mysql_connection_->disableCloseOnDestroy();
    }
  }

  [[nodiscard]] unsigned int getErrno() const {
    CHECK_THROW(mysql_connection_ != nullptr, db::InvalidConnectionException);
    return mysql_connection_->getErrno();
  }

  [[nodiscard]] std::string getErrorMessage() const {
    CHECK_THROW(mysql_connection_ != nullptr, db::InvalidConnectionException);
    return mysql_connection_->getErrorMessage();
  }

  void setConnectAttributes(const AttributeMap& attributes) {
    CHECK_THROW(mysql_connection_ != nullptr, db::InvalidConnectionException);
    mysql_connection_->setConnectAttributes(attributes);
  }

  [[nodiscard]] int setQueryAttributes(const AttributeMap& attributes) {
    CHECK_THROW(mysql_connection_ != nullptr, db::InvalidConnectionException);
    return mysql_connection_->setQueryAttributes(attributes);
  }

  [[nodiscard]] bool setQueryAttribute(
      const std::string& key,
      const std::string& value) {
    CHECK_THROW(mysql_connection_ != nullptr, db::InvalidConnectionException);
    return mysql_connection_->setQueryAttribute(key, value);
  }

  [[nodiscard]] AttributeMap getResponseAttributes() const {
    // This function can be called with no valid connection
    if (mysql_connection_) {
      return mysql_connection_->getResponseAttributes();
    }

    return {};
  }

  [[nodiscard]] DeferredAttributesFn getDeferredResponseAttributes() const {
    // This function can be called with no valid connection
    if (mysql_connection_) {
      return mysql_connection_->getDeferredResponseAttributes();
    }

    return nullptr;
  }

  void setConnectTimeout(Millis timeout) {
    CHECK_THROW(mysql_connection_ != nullptr, db::InvalidConnectionException);
    return mysql_connection_->setConnectTimeout(timeout);
  }

  [[nodiscard]] const InternalConnection& getInternalConnection() const {
    CHECK_THROW(mysql_connection_ != nullptr, db::InvalidConnectionException);
    return mysql_connection_->getInternalConnection();
  }

  [[nodiscard]] InternalConnection& getInternalConnection() {
    CHECK_THROW(mysql_connection_ != nullptr, db::InvalidConnectionException);
    return mysql_connection_->getInternalConnection();
  }

  [[nodiscard]] uint64_t getLastInsertId() const {
    CHECK_THROW(mysql_connection_ != nullptr, db::InvalidConnectionException);
    return mysql_connection_->getLastInsertId();
  }

  [[nodiscard]] uint64_t getAffectedRows() const {
    CHECK_THROW(mysql_connection_ != nullptr, db::InvalidConnectionException);
    return mysql_connection_->getAffectedRows();
  }

  [[nodiscard]] std::optional<std::string> getRecvGtid() const {
    CHECK_THROW(mysql_connection_ != nullptr, db::InvalidConnectionException);
    return mysql_connection_->getRecvGtid();
  }

  [[nodiscard]] std::optional<std::string> getSchemaChanged() const {
    CHECK_THROW(mysql_connection_ != nullptr, db::InvalidConnectionException);
    return mysql_connection_->getSchemaChanged();
  }

  [[nodiscard]] bool getNoIndexUsed() const {
    CHECK_THROW(mysql_connection_ != nullptr, db::InvalidConnectionException);
    return mysql_connection_->getNoIndexUsed();
  }

  [[nodiscard]] bool wasSlow() const {
    CHECK_THROW(mysql_connection_ != nullptr, db::InvalidConnectionException);
    return mysql_connection_->wasSlow();
  }

  std::unique_ptr<ConnectionHolder> mysql_connection_;

  std::shared_ptr<const ConnectionKey> conn_key_;
  ConnectionOptions conn_options_;

  bool killOnQueryTimeout_ = false;

  // Context information for logging purposes.
  std::shared_ptr<db::ConnectionContextBase> connection_context_;

  // Unowned pointer to the client we're from.
  MysqlClientBase& mysql_client_;

  ConnectionDyingCallback conn_dying_callback_;

  OperationBase::Callbacks callbacks_;

  bool initialized_;

  QueryAttributes persistentQueryAttributes_;

  // Used for signing that the connection is being used in a synchronous call,
  // eg. `query`. MySQL doesn't allow more than one query being made through
  // the same connection at the same time. So same logic goes here.
  // We don't track for async calls, for async calls the unique Connection
  // gets moved to the operation, so the protection is guaranteed.
  bool operation_in_progress_ = false;

  // Query

  // This method holds the core logic for query() and can be
  // overridden in derived class
  virtual DbQueryResult
  internalQuery(Query&& query, QueryCallback&& cb, QueryOptions&& options);

  // MultiQuery

  // This method holds the core logic for multiQuery() and can be
  // overridden in derived class
  virtual DbMultiQueryResult internalMultiQuery(
      std::vector<Query>&& queries,
      MultiQueryCallback&& cb,
      QueryOptions&& options);

  // QueryWithGenerator

  // This method holds the core logic for queryWithGenerator and can be
  // overridden in derived class.  QueryGenerator is passed as an rvalue
  // reference
  virtual DbQueryResult internalQueryWithGenerator(
      QueryGenerator&& query_Generator,
      QueryCallback&& cb,
      QueryOptions&& options);

  // This method holds the core logic for queryWithGenerator and can be
  // overridden in derived class. QueryGenerator is passed as an lvalue
  // reference
  virtual DbQueryResult internalQueryWithGenerator(
      QueryGenerator& query_Generator,
      QueryCallback&& cb,
      QueryOptions&& options);

  // MultiQueryWithGenerator

  // This method holds the core logic for multiQueryWithGenerator and can be
  // overridden in derived class.
  virtual DbMultiQueryResult internalMultiQueryWithGenerators(
      std::vector<std::unique_ptr<QueryGenerator>>&& query_generators,
      MultiQueryCallback&& cb,
      QueryOptions&& options);

 private:
  template <typename Op, typename Cb>
  void applyQueryOptions(Op& op, Cb&& cb, QueryOptions& options);

  template <typename Op>
  void runOperation(Op& op);

  template <typename QueryOperationType>
  static void checkForQueryTimeoutOverride(
      QueryOperationType& op,
      const std::optional<Duration>& timeoutOverride) {
    if (timeoutOverride) {
      op.setTimeout(*timeoutOverride);
      // If the connection had a query timeout set, it was put into the query
      // attributes, where an intervening proxy may apply it as the query
      // timeout. We need to override that here.
      op.setAttribute(
          "query_timeout",
          folly::to<std::string>(
              std::chrono::duration_cast<std::chrono::seconds>(*timeoutOverride)
                  .count()));
    }
  }
};

template <>
DbQueryResult Connection::query(Query&& args);

template <>
DbQueryResult Connection::query(Query&& args, QueryOptions&& options);

template <>
DbQueryResult Connection::query(Query&& args, QueryCallback&& cb);

template <>
DbQueryResult
Connection::query(Query&& args, QueryCallback&& cb, QueryOptions&& options);

template <typename... Args>
DbQueryResult Connection::query(Args&&... args) {
  Query query_obj{std::forward<Args>(args)...};
  return query(std::move(query_obj));
}

template <>
std::shared_ptr<QueryOperation> Connection::beginQueryWithLoggingFuncs(
    std::unique_ptr<Connection> conn,
    LoggingFuncsPtr logging_funcs,
    Query&& args);

template <typename... Args>
std::shared_ptr<QueryOperation> Connection::beginQuery(
    std::unique_ptr<Connection> conn,
    Args&&... args) {
  Query query{std::forward<Args>(args)...};
  return beginQueryWithLoggingFuncs(std::move(conn), nullptr, std::move(query));
}

template <typename... Args>
std::shared_ptr<QueryOperation> Connection::beginQueryWithLoggingFuncs(
    std::unique_ptr<Connection> conn,
    LoggingFuncsPtr logging_funcs,
    Args&&... args) {
  Query query{std::forward<Args>(args)...};
  return beginQueryWithLoggingFuncs(
      std::move(conn), std::move(logging_funcs), std::move(query));
}

// Helper class to support needing to wait on async connections - the sync
// client doesn't need this but other clients might - so making it into its own
// class to be able to derive from it.
class AsyncConnectionHelper : public Connection {
 public:
  AsyncConnectionHelper(
      MysqlClientBase& mysql_client,
      std::shared_ptr<const ConnectionKey> conn_key,
      std::unique_ptr<ConnectionHolder> conn = nullptr)
      : Connection(mysql_client, std::move(conn_key), std::move(conn)) {}

  virtual ~AsyncConnectionHelper() override = default;

 protected:
  // Operations call these methods as the operation becomes unblocked, as
  // callers want to wait for completion, etc.
  void notify() override {
    if (actionableBaton_.try_wait()) {
      LOG(DFATAL) << "asked to notify already-actionable operation";
    }
    actionableBaton_.post();
  }

  void wait() const override {
    CHECK_THROW(
        folly::fibers::onFiber() || isNotInEventBaseThread(),
        std::runtime_error);
    actionableBaton_.wait();
  }

  // Called when a new operation is being started.
  void resetActionable() override {
    actionableBaton_.reset();
  }

 private:
  mutable folly::fibers::Baton actionableBaton_;
};

} // namespace facebook::common::mysql_client
