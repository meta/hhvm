/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010- Facebook, Inc. (http://www.facebook.com)         |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#pragma once

#include <algorithm>
#include <memory>

#include <squangle/mysql_client/AsyncMysqlClient.h>
#include <squangle/mysql_client/AsyncConnectionPool.h>
#include <squangle/mysql_client/SSLOptionsProviderBase.h>
#include <squangle/mysql_client/Row.h>

#include <squangle/logger/DBEventCounter.h>

#include "hphp/runtime/ext/asio/asio-external-thread-event.h"
#include "hphp/runtime/ext/extension.h"



namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

struct c_Vector;

namespace am = facebook::common::mysql_client;
namespace db = facebook::db;

///////////////////////////////////////////////////////////////////////////////
// class AsyncMysqlConnectionPool

struct AsyncMysqlConnectionPool :
    SystemLib::ClassLoader<"AsyncMysqlConnectionPool"> {
  AsyncMysqlConnectionPool() = default;
  void sweep();

  std::shared_ptr<am::AsyncConnectionPool> m_async_pool;

  AsyncMysqlConnectionPool(const AsyncMysqlConnectionPool&) = delete;
  AsyncMysqlConnectionPool& operator=(const AsyncMysqlConnectionPool&) = delete;
};

///////////////////////////////////////////////////////////////////////////////
// class AsyncMysqlConnection

struct AsyncMysqlConnection : SystemLib::ClassLoader<"AsyncMysqlConnection"> {
  AsyncMysqlConnection();
  AsyncMysqlConnection(const AsyncMysqlConnection&) = delete;
  AsyncMysqlConnection& operator=(const AsyncMysqlConnection&) = delete;
  void sweep();
  void setConnection(std::unique_ptr<am::Connection> conn);
  void setConnectOperation(std::shared_ptr<am::ConnectOperation> op);
  void setClientStats(db::ClientPerfStats perfStats);
  void verifyValidConnection();
  bool isValidConnection();
  static Object newInstance(
      std::unique_ptr<am::Connection> conn,
      std::shared_ptr<am::ConnectOperation> conn_op = nullptr,
      db::ClientPerfStats clientStats = db::ClientPerfStats());
  using AttributeMap = am::AttributeMap;
  Object query(
      ObjectData* this_,
      const am::Query& query,
      int64_t timeout_micros = -1,
      const AttributeMap& queryAttributes = AttributeMap());

  std::unique_ptr<am::Connection> m_conn;
  OptString m_host;
  int m_port;
  bool m_closed;
  // Stats at the moment the Connection was created
  // Only available if the connection was created inside the AsyncMysqlClient.
  db::ClientPerfStats m_clientStats;
  std::shared_ptr<am::ConnectOperation> m_op;
};

///////////////////////////////////////////////////////////////////////////////
// class MySSLContext

struct MySSLContextProvider : SystemLib::ClassLoader<"MySSLContextProvider"> {
  MySSLContextProvider() {}
  explicit MySSLContextProvider(
      std::shared_ptr<am::SSLOptionsProviderBase> provider);

  MySSLContextProvider& operator=(const MySSLContextProvider& that_) = delete;

  static Object newInstance(
      std::shared_ptr<am::SSLOptionsProviderBase> ssl_provider);
  std::shared_ptr<am::SSLOptionsProviderBase> getSSLProvider();
  void setSSLProvider(std::shared_ptr<am::SSLOptionsProviderBase> ssl_provider);

  std::shared_ptr<am::SSLOptionsProviderBase> m_provider;
};

///////////////////////////////////////////////////////////////////////////////
// class AsyncMysqlConnectionOptions

struct AsyncMysqlConnectionOptions :
    SystemLib::ClassLoader<"AsyncMysqlConnectionOptions"> {
  AsyncMysqlConnectionOptions();
  const am::ConnectionOptions& getConnectionOptions();

  am::ConnectionOptions m_conn_opts;
  TYPE_SCAN_IGNORE_FIELD(m_conn_opts);
};

///////////////////////////////////////////////////////////////////////////////
// class AsyncMysqlResult

struct AsyncMysqlResult {
  virtual ~AsyncMysqlResult() {}
  int64_t elapsedMicros();
  double startTime();
  double endTime();

  Object clientStats();

  void create(std::shared_ptr<am::Operation> op, db::ClientPerfStats values) {
    m_op = std::move(op);
    m_clientStats = std::move(values);
  }

  am::Operation* op();

  void sweep() { m_op.reset(); }

  bool sslSessionReused();
  OptString getSslCertCn();
  OptString getSslCertSan();
  OptString getSslCertExtensions();
  bool isSslCertValidationEnforced();
  OptString getSslVersion();

  std::shared_ptr<am::Operation> m_op;
  db::ClientPerfStats m_clientStats;
};

// Intended to just hold extra data about the Operation. This should be created
// in `AsyncMysqlConnection`.

struct AsyncMysqlConnectResult :
    AsyncMysqlResult,
    SystemLib::ClassLoader<"AsyncMysqlConnectResult"> {
  AsyncMysqlConnectResult() = default;
  ~AsyncMysqlConnectResult() override {}
  static Object newInstance(std::shared_ptr<am::Operation> op,
                            db::ClientPerfStats clientStats);

  AsyncMysqlConnectResult(const AsyncMysqlConnectResult&) = delete;
  AsyncMysqlConnectResult& operator=(const AsyncMysqlConnectResult&) = delete;

};

///////////////////////////////////////////////////////////////////////////////
// class AsyncMysqlErrorResult

struct AsyncMysqlErrorResult :
    AsyncMysqlResult,
    SystemLib::ClassLoader<"AsyncMysqlErrorResult"> {
  AsyncMysqlErrorResult() = default;
  ~AsyncMysqlErrorResult() override {}

  static Object newInstance(std::shared_ptr<am::Operation> op,
                            db::ClientPerfStats clientStats);

  AsyncMysqlErrorResult(const AsyncMysqlErrorResult&) = delete;
  AsyncMysqlErrorResult& operator=(const AsyncMysqlErrorResult&) = delete;
};

///////////////////////////////////////////////////////////////////////////////
// class AsyncMysqlQueryErrorResult

struct AsyncMysqlQueryErrorResult :
    SystemLib::ClassLoader<"AsyncMysqlQueryErrorResult"> {
  AsyncMysqlQueryErrorResult();
  AsyncMysqlQueryErrorResult(const AsyncMysqlQueryErrorResult&) = delete;
  AsyncMysqlQueryErrorResult& operator=(const AsyncMysqlQueryErrorResult&) =
    delete;
  void sweep();
  void create(std::shared_ptr<am::Operation> op,
              db::ClientPerfStats values,
              req::ptr<c_Vector> results);
  static Object newInstance(std::shared_ptr<am::Operation> op,
                            db::ClientPerfStats values,
                            req::ptr<c_Vector> results);

  req::ptr<c_Vector> m_query_results;

  // extends AsyncMysqlErrorResult
  //
  // The address of native data is computed using the size of the native data
  // as an offset from the object data. To ensure that the offset to the parent
  // class remains the same, we simulate inheritance by placing the parent
  // class below the object instead of using c++ inheritance.
  AsyncMysqlErrorResult m_parent;
};

///////////////////////////////////////////////////////////////////////////////
// This class is shared across the QueryResult and RowBlocks for faster
// indexing of row columns.
// By using HPHP::String instead of other string types, we are able
// to build maps where we just reuse them so we avoid copying.
// Thus, eliminating repetition of map keys and also gain speed.

struct FieldIndex {
  explicit FieldIndex(const am::RowFields* row_fields);

  size_t getFieldIndex(const OptString& field_name) const;
  OptString getFieldString(size_t field_index) const;

 private:
  // NB: It's possible to just use a req::vector_map<String> for names,
  // and rely on insertion order to compute indexes, but sometimes this
  // FieldIndex has duplicate names. last-name-wins, requiring the map.
  req::vector<OptString> field_names_;
  req::fast_map<OptString, size_t, hphp_string_hash, hphp_string_same>
    field_name_map_;
};

///////////////////////////////////////////////////////////////////////////////
// class AsyncMysqlQueryResult

struct AsyncMysqlQueryResult :
    AsyncMysqlResult,
    SystemLib::ClassLoader<"AsyncMysqlQueryResult"> {
  AsyncMysqlQueryResult() = default;
  ~AsyncMysqlQueryResult() override {}
  void sweep();
  void
  create(std::shared_ptr<am::FetchOperation> op, db::ClientPerfStats values,
         am::QueryResult query_result, bool noIndexUsed);
  Object buildRows(bool as_maps, bool typed_values);
  Array buildTypedVecMaps();
  static Object
  newInstance(std::shared_ptr<am::FetchOperation> op, db::ClientPerfStats values,
              am::QueryResult query_result, bool noIndexUsed);

  std::unique_ptr<am::QueryResult> m_query_result;
  bool m_no_index_used;

  // Created here for buildRows and passed to RowBlocks
  req::shared_ptr<FieldIndex> m_field_index;

  AsyncMysqlQueryResult(const AsyncMysqlQueryResult&) = delete;
  AsyncMysqlQueryResult& operator=(const AsyncMysqlQueryResult&) = delete;
};

///////////////////////////////////////////////////////////////////////////////
// Async events

struct AsyncMysqlConnectEvent final : AsioExternalThreadEvent {
  explicit AsyncMysqlConnectEvent(std::shared_ptr<am::ConnectOperation> op) {
    m_op = op;
  }

  void opFinished() { markAsFinished(); }

  void setClientStats(db::ClientPerfStats stats) {
    m_clientStats = std::move(stats);
  }

 protected:
  void unserialize(TypedValue& result) final;

 private:
  std::shared_ptr<am::ConnectOperation> m_op;
  db::ClientPerfStats m_clientStats;
};

struct AsyncMysqlQueryEvent final : AsioExternalThreadEvent {
  AsyncMysqlQueryEvent(ObjectData* conn,
                       std::shared_ptr<am::QueryOperation> op)
      : AsioExternalThreadEvent(conn) {
    m_query_op = std::move(op);
  }

  void opFinished() { markAsFinished(); }

  void setClientStats(db::ClientPerfStats stats) {
    m_clientStats = std::move(stats);
  }

 protected:
  void unserialize(TypedValue& result) final;

 private:
  std::shared_ptr<am::QueryOperation> m_query_op;
  db::ClientPerfStats m_clientStats;
};

struct AsyncMysqlMultiQueryEvent final : AsioExternalThreadEvent {
  AsyncMysqlMultiQueryEvent(ObjectData* conn,
                            std::shared_ptr<am::MultiQueryOperation> op)
      : AsioExternalThreadEvent(conn) {
    m_multi_op = op;
  }

  void opFinished() { markAsFinished(); }

  void setClientStats(db::ClientPerfStats stats) {
    m_clientStats = std::move(stats);
  }

 protected:
  void unserialize(TypedValue& result) final;

 private:
  std::shared_ptr<am::MultiQueryOperation> m_multi_op;
  db::ClientPerfStats m_clientStats;
};

struct AsyncMysqlConnectAndMultiQueryEvent final : AsioExternalThreadEvent {
  explicit AsyncMysqlConnectAndMultiQueryEvent(
      std::shared_ptr<am::ConnectOperation> op) {
    m_connect_op = op;
    if (m_connect_op->getCertValidationCallback() &&
        m_connect_op->getConnectionContext() == nullptr) {
      auto context = std::make_unique<db::ConnectionContextBase>();
      m_connect_op->setConnectionContext(std::move(context));
    }
  }

  void setQueryOp(std::shared_ptr<am::MultiQueryOperation> op) {
    m_multi_query_op = std::move(op);
  }

  void opFinished() { markAsFinished(); }

  void setClientStats(db::ClientPerfStats stats) {
    m_clientStats = std::move(stats);
  }

 protected:
  void unserialize(TypedValue& result) final;

 private:
  std::shared_ptr<am::ConnectOperation> m_connect_op;
  std::shared_ptr<am::MultiQueryOperation> m_multi_query_op;
  db::ClientPerfStats m_clientStats;
};

///////////////////////////////////////////////////////////////////////////////
// class AsyncMysqlRowBlock

struct AsyncMysqlRowBlock : SystemLib::ClassLoader<"AsyncMysqlRowBlock"> {
  AsyncMysqlRowBlock() = default;

  void sweep();
  size_t getIndexFromVariant(const Variant& field);
  template <typename FieldType>
  FieldType getFieldAs(int64_t row, const Variant& field);
  static Object newInstance(am::RowBlock* row_block,
                            req::shared_ptr<FieldIndex> indexer);

  std::unique_ptr<am::RowBlock> m_row_block;
  req::shared_ptr<FieldIndex> m_field_index;

  AsyncMysqlRowBlock(const AsyncMysqlRowBlock&) = delete;
  AsyncMysqlRowBlock& operator=(const AsyncMysqlRowBlock&) = delete;
};

///////////////////////////////////////////////////////////////////////////////
// class AsyncMysqlRowBlockIterator

struct AsyncMysqlRowBlockIterator :
    SystemLib::ClassLoader<"AsyncMysqlRowBlockIterator"> {
  AsyncMysqlRowBlockIterator() = default;
  static Object newInstance(Object row_block, size_t row_number);

  Object m_row_block;
  size_t m_row_number;

  AsyncMysqlRowBlockIterator(const AsyncMysqlRowBlockIterator&) = delete;
  AsyncMysqlRowBlockIterator& operator=(const AsyncMysqlRowBlockIterator&) =
      delete;
};

///////////////////////////////////////////////////////////////////////////////
// class AsyncMysqlRow

struct AsyncMysqlRow : SystemLib::ClassLoader<"AsyncMysqlRow"> {
  AsyncMysqlRow() = default;

  static Object newInstance(Object row_block, size_t row_number);

  Object m_row_block;
  size_t m_row_number;

  AsyncMysqlRow(const AsyncMysqlRow&) = delete;
  AsyncMysqlRow& operator=(const AsyncMysqlRow&) = delete;
};

///////////////////////////////////////////////////////////////////////////////
// class AsyncMysqlRowIterator

struct AsyncMysqlRowIterator : SystemLib::ClassLoader<"AsyncMysqlRowIterator"> {
  AsyncMysqlRowIterator() = default;

  static Object newInstance(Object row, size_t field_number);

  Object m_row;
  size_t m_field_number;

  AsyncMysqlRowIterator(const AsyncMysqlRowIterator&) = delete;
  AsyncMysqlRowIterator& operator=(const AsyncMysqlRowIterator&) = delete;
};

///////////////////////////////////////////////////////////////////////////////
}
