/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/coro/Task.h>
#include <folly/io/async/EventHandler.h>
#include <folly/ssl/OpenSSLPtrTypes.h>
#include <mysql/server/include/mysql.h> // @manual
#include <optional>

#include "squangle/base/Base.h"

namespace facebook::common::mysql_client {

class ConnectionKey;
class ConnectionOptions;

using MysqlCertValidatorCallback =
    int (*)(X509* server_cert, const void* context, const char** errptr);

enum InternalStatus {
  PENDING,
  DONE,
  ERROR,
};

class InternalRow {
 public:
  enum class Type {
    Null,
    Bool,
    Int64,
    UInt64,
    Double,
    String,
  };

  virtual ~InternalRow() = default;

  virtual folly::StringPiece columnString(size_t /*col*/) const = 0;
  virtual bool columnBool(size_t /*col*/) const = 0;
  virtual int64_t columnInt64(size_t /*col*/) const = 0;
  virtual uint64_t columnUInt64(size_t /*col*/) const = 0;
  virtual double columnDouble(size_t /*col*/) const = 0;

  virtual Type columnType(size_t col) const = 0;

  virtual size_t columnLength(size_t col) const = 0;
};

class InternalRowMetadata {
 public:
  virtual ~InternalRowMetadata() = default;

  virtual size_t numFields() const = 0;

  virtual folly::StringPiece getTableName(size_t field) const = 0;

  virtual folly::StringPiece getFieldName(size_t field) const = 0;

  virtual enum_field_types getFieldType(size_t field) const = 0;

  virtual uint64_t getFieldFlags(size_t field) const = 0;

  // `MYSQL_FIELD::charsetnr`: the collation of the bytes this column's values
  // arrive in, or `std::nullopt` from a protocol that does not carry it.
  virtual std::optional<unsigned int> getFieldCharsetnr(size_t field) const = 0;
};

class InternalResult {
 public:
  virtual ~InternalResult() = default;

  using FetchRowRet = std::pair<InternalStatus, std::unique_ptr<InternalRow>>;

  virtual FetchRowRet fetchRow() = 0;

  virtual size_t numRows() const = 0;

  virtual void close() = 0;

  // If needed this can be overridden to make sure the rows get cranked
  virtual folly::coro::Task<void> co_crank() {
    co_return;
  }
};

class InternalConnection {
 public:
  using Status = InternalStatus;

  virtual ~InternalConnection() = default;

  virtual void setReusable(bool /*reusable*/) {}

  virtual bool isReusable() const {
    return false;
  }

  virtual void disableCloseOnDestroy() {}

  virtual bool isSSL() const {
    return true;
  }

  virtual bool inTransaction() const = 0;

  virtual void setNeedResetBeforeReuse() {}

  virtual bool needResetBeforeReuse() {
    return true;
  }

  virtual std::string serverInfo() const = 0;

  virtual long threadId() const {
    return 0;
  }

  virtual void disableLocalFiles() {}

  virtual void disableSSL() {}

  virtual bool sslSessionReused() const {
    return false;
  }

  [[nodiscard]] virtual std::string getTlsVersion() const {
    return "";
  }

  virtual unsigned int warningCount() const = 0;

  // `quote` is the character the caller wraps the escaped value in. It only
  // matters under the NO_BACKSLASH_ESCAPES sql_mode, where quotes are escaped
  // by doubling them instead of with a backslash.
  std::string escapeString(std::string_view unescaped, char quote = '\'') const;

  virtual size_t
  escapeString(char* out, const char* src, size_t length, char quote) const = 0;

  virtual std::function<void()> getCloseFunction() {
    return nullptr;
  }

  virtual unsigned int getErrno() const = 0;

  virtual std::string getErrorMessage() const = 0;

  virtual void setConnectAttributes(const AttributeMap& attributes) = 0;

  virtual int setQueryAttributes(const AttributeMap& attributes) = 0;

  virtual int setQueryAttribute(
      const std::string& key,
      const std::string& value) = 0;

  virtual AttributeMap getResponseAttributes() const = 0;

  // Not included in getResponseAttributes(). Null when there are none.
  [[nodiscard]] virtual DeferredAttributesFn getDeferredResponseAttributes()
      const {
    return nullptr;
  }

  virtual void setConnectTimeout(Millis timeout) const = 0;

  virtual void setReadTimeout(Millis timeout) const = 0;

  virtual void setWriteTimeout(Millis timeout) const = 0;

  virtual uint64_t getLastInsertId() const = 0;

  virtual uint64_t getAffectedRows() const = 0;

  virtual std::optional<std::string> getRecvGtid() const = 0;

  virtual std::optional<std::string> getMySQLInfo() const = 0;

  virtual std::optional<std::string> getSchemaChanged() const = 0;

  virtual bool getNoIndexUsed() const = 0;

  virtual bool wasSlow() const = 0;

  virtual bool getAutocommit() const = 0;

  virtual bool ping() const = 0;

  virtual Status resetConn() const = 0;

  virtual Status changeUser(
      std::shared_ptr<const ConnectionKey> conn_key) const = 0;

  virtual bool dumpDebugInfo() const = 0;
};

} // namespace facebook::common::mysql_client
