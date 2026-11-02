/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <fmt/format.h>
#include <folly/CppAttributes.h>
#include "mysql/server/include/mysql.h" // @manual=//mysql/server/include:mysql_namespaced

#include "squangle/base/Base.h"
#include "squangle/base/ConnectionKey.h"
#include "squangle/mysql_client/Compression.h"
#include "squangle/mysql_client/InternalConnection.h"
#include "squangle/mysql_client/SSLOptionsProviderBase.h"

namespace facebook::common::mysql_client::mysql_protocol {

class AsyncConnectionPool;
class MysqlResult;
class MysqlRowMetadata;

// Holds the mysql connection for easier re use
class MysqlConnection : public InternalConnection {
 public:
  explicit MysqlConnection(MYSQL* mysql = createMysql());

  // Closes the connection in hold
  virtual ~MysqlConnection() override;

  // copy and move not allowed
  MysqlConnection(const MysqlConnection&) = delete;
  MysqlConnection& operator=(const MysqlConnection&) = delete;

  MysqlConnection(MysqlConnection&&) = delete;
  MysqlConnection& operator=(MysqlConnection&&) = delete;

  void setReusable(bool reusable) override {
    canReuse_ = reusable;
  }

  [[nodiscard]] bool isReusable() const override {
    return canReuse_ && getErrno() == 0;
  }

  // Don't close the mysql fd in the destructor. Useful when connections
  // are managed outside this library.
  void disableCloseOnDestroy() override {
    closeFdOnDestroy_ = false;
  }

  [[nodiscard]] bool isSSL() const override {
    return mysql_->client_flag & CLIENT_SSL;
  }

  // Returns whether or not the connection is in a transaction based on server
  // status
  [[nodiscard]] bool inTransaction() const override {
    return mysql_->server_status & SERVER_STATUS_IN_TRANS;
  }

  void setNeedResetBeforeReuse() override {
    needResetBeforeReuse_ = true;
  }

  [[nodiscard]] bool needResetBeforeReuse() override {
    return needResetBeforeReuse_;
  }

  [[nodiscard]] std::string serverInfo() const override;

  [[nodiscard]] long threadId() const override;

  void disableLocalFiles() override {
    mysql_->options.client_flag &= ~CLIENT_LOCAL_FILES;
  }

  void disableSSL() override;

  [[nodiscard]] bool sslSessionReused() const override;

  [[nodiscard]] std::string getTlsVersion() const override;

  // Returns the raw SSL* handle (as void*), or nullptr if not an SSL
  // connection. Useful for testing TLS-level operations like KeyUpdate.
  [[nodiscard]] void* FOLLY_NULLABLE getSSLHandle() const;

  [[nodiscard]] unsigned int warningCount() const override;

  [[nodiscard]] size_t escapeString(
      char* out,
      const char* src,
      size_t length,
      char quote) const override;

  std::function<void()> getCloseFunction() override;

  [[nodiscard]] unsigned int getErrno() const override;

  [[nodiscard]] std::string getErrorMessage() const override;

  void setConnectAttributes(const AttributeMap& attributes) override;

  int setQueryAttributes(const AttributeMap& attributes) override;

  int setQueryAttribute(const std::string& key, const std::string& value)
      override;

  [[nodiscard]] AttributeMap getResponseAttributes() const override;

  void setConnectTimeout(Millis timeout) const override;

  void setReadTimeout(Millis timeout) const override;

  void setWriteTimeout(Millis timeout) const override;

  [[nodiscard]] uint64_t getLastInsertId() const override;

  [[nodiscard]] uint64_t getAffectedRows() const override;

  [[nodiscard]] std::optional<std::string> getRecvGtid() const override;

  [[nodiscard]] std::optional<std::string> getMySQLInfo() const override;

  [[nodiscard]] std::optional<std::string> getSchemaChanged() const override;

  [[nodiscard]] bool hasMoreResults() const;

  [[nodiscard]] bool getNoIndexUsed() const override {
    return mysql_->server_status & SERVER_QUERY_NO_INDEX_USED;
  }

  [[nodiscard]] bool wasSlow() const override {
    return mysql_->server_status & SERVER_QUERY_WAS_SLOW;
  }

  [[nodiscard]] bool getAutocommit() const override {
    return mysql_->server_status & SERVER_STATUS_AUTOCOMMIT;
  }

  [[nodiscard]] bool dumpDebugInfo() const override;

  [[nodiscard]] bool ping() const override;

  [[nodiscard]] bool isUnusableOnCheckout() const override;

  // MySQL protocol specfic methods

  void setCompression(CompressionAlgorithm algo);

  void setSniServerName(const std::string& name);

  void setCertValidatorCallback(
      const MysqlCertValidatorCallback& cb,
      void* context);

  // Clear any previously registered cert validator callback and its context
  // pointer from the MYSQL* handle.  Must be called before the context object
  // is destroyed to prevent a dangling-pointer dereference if libmysql later
  // triggers an SSL renegotiation or auto-reconnect.
  void clearCertValidatorCallback();

  bool storeSession(SSLOptionsProviderBase& provider) {
    return provider.storeMysqlSSLSession(mysql_);
  }

  [[nodiscard]] bool setSSLOptionsProvider(SSLOptionsProviderBase& provider);

  [[nodiscard]] std::optional<std::string> getSniServerName() const;

  [[nodiscard]] bool setDscp(uint8_t dscp);

  [[nodiscard]] int getSocketDescriptor() const;

  [[nodiscard]] bool isDoneWithTcpHandShake() const {
    return getMySqlConnectStage() > tcpCompletionStage_;
  }

  [[nodiscard]] std::string getConnectStageName() const;

  [[nodiscard]] static std::optional<std::string> findConnectStageName(
      connect_stage stage);

  [[nodiscard]] size_t getFieldCount() const;

  [[nodiscard]] virtual Status runQuery(std::string_view query) const = 0;
  [[nodiscard]] virtual Status changeUser(
      std::shared_ptr<const ConnectionKey> conn_key) const override = 0;

  [[nodiscard]] virtual Status nextResult() const = 0;

  [[nodiscard]] virtual std::unique_ptr<MysqlResult> getResult() const = 0;

  static inline Status toHandlerStatus(net_async_status status) {
    if (status == NET_ASYNC_ERROR) {
      return Status::ERROR;
    } else if (status == NET_ASYNC_COMPLETE) {
      return Status::DONE;
    } else {
      return Status::PENDING;
    }
  }

  [[nodiscard]] virtual Status tryConnect(
      const ConnectionOptions& opts,
      std::shared_ptr<const ConnectionKey> conn_key,
      int flags) = 0;

 protected:
  friend class MysqlOperationImpl;

  // Our MYSQL handle.
  MYSQL* mysql_;
  std::optional<std::string> current_schema_;
  bool closeFdOnDestroy_ = true;
  bool needResetBeforeReuse_ = false;
  bool canReuse_ = true;

  [[nodiscard]] folly::EventHandler::EventFlags getReadWriteState() const;

  // Simple name for mysql internal connect stage enum
  using MySqlConnectStage = enum connect_stage;

  MySqlConnectStage getMySqlConnectStage() const;

  static MYSQL* createMysql();

  std::string logMysqlOptions(std::string_view opt, const void* param, int ret)
      const {
    return logMysqlOptionsImpl(mysql_, opt, param, ret);
  }

  std::string logMysqlOptions(std::string_view opt, int param, int ret) const {
    return logMysqlOptionsImpl(mysql_, opt, param, ret);
  }

  std::string logMysqlOptions(
      std::string_view opt,
      const std::string& param,
      int ret) const {
    return logMysqlOptionsImpl(mysql_, opt, param, ret);
  }

  std::string logMysqlOptions4(
      std::string_view opt,
      const std::string& param1,
      const std::string& param2,
      int ret) const {
    return logMysqlOptions4Impl(mysql_, opt, param1, param2, ret);
  }

  static std::string logMysqlOptionsImpl(
      MYSQL* mysql,
      std::string_view opt,
      const void* param,
      int ret);

  static std::string
  logMysqlOptionsImpl(MYSQL* mysql, std::string_view opt, int param, int ret);

  static std::string logMysqlOptionsImpl(
      MYSQL* mysql,
      std::string_view opt,
      const std::string& param,
      int ret);

  static std::string logMysqlOptions4Impl(
      MYSQL* mysql,
      std::string_view opt,
      const std::string& param1,
      const std::string& param2,
      int ret);

  // Mysql internal connect stage which handles the async tcp handshake
  // completion between client and server
  static constexpr MySqlConnectStage tcpCompletionStage_ =
      MySqlConnectStage::CONNECT_STAGE_NET_COMPLETE_CONNECT;
};

class AsyncMysqlConnection : public MysqlConnection {
  [[nodiscard]] Status tryConnect(
      const ConnectionOptions& opts,
      std::shared_ptr<const ConnectionKey> conn_key,
      int flags) override;

  [[nodiscard]] Status runQuery(std::string_view query) const override;

  [[nodiscard]] Status resetConn() const override;

  [[nodiscard]] Status changeUser(
      std::shared_ptr<const ConnectionKey> conn_key) const override;

  [[nodiscard]] Status nextResult() const override;

  [[nodiscard]] std::unique_ptr<MysqlResult> getResult() const override;
};

class SyncMysqlConnection : public MysqlConnection {
  [[nodiscard]] Status tryConnect(
      const ConnectionOptions& opts,
      std::shared_ptr<const ConnectionKey> conn_key,
      int flags) override;

  [[nodiscard]] Status runQuery(std::string_view query) const override;

  [[nodiscard]] Status resetConn() const override;

  [[nodiscard]] Status changeUser(
      std::shared_ptr<const ConnectionKey> conn_key) const override;

  [[nodiscard]] Status nextResult() const override;

  [[nodiscard]] std::unique_ptr<MysqlResult> getResult() const override;
};

} // namespace facebook::common::mysql_client::mysql_protocol

// Tell fmt::format how to print the enum net_async_status
template <>
struct fmt::formatter<enum net_async_status> {
  template <typename ParseContext>
  constexpr auto parse(const ParseContext& ctx) const {
    // No reading of the format needed
    return ctx.begin();
  }

  template <typename FormatContext>
  auto format(enum net_async_status status, FormatContext& ctx) const {
    std::string_view name = "unknown";
    switch (status) {
      case NET_ASYNC_COMPLETE:
        name = "NET_ASYNC_COMPLETE";
        break;
      case NET_ASYNC_NOT_READY:
        name = "NET_ASYNC_NOT_READY";
        break;
      case NET_ASYNC_ERROR:
        name = "NET_ASYNC_ERROR";
        break;
      case NET_ASYNC_COMPLETE_NO_MORE_RESULTS:
        name = "NET_ASYNC_COMPLETE_NO_MORE_RESULTS";
        break;
      default:
        break;
    }
    return fmt::format_to(ctx.out(), "{}", name);
  }
};

// Tell fmt::format how to print the enum connect_stage
template <>
struct fmt::formatter<enum connect_stage> {
  template <typename ParseContext>
  constexpr auto parse(const ParseContext& ctx) const {
    // No reading of the format needed
    return ctx.begin();
  }

  template <typename FormatContext>
  auto format(enum connect_stage stage, FormatContext& ctx) const {
    std::string_view name = "unknown";
    switch (stage) {
      case CONNECT_STAGE_INVALID:
        name = "CONNECT_STAGE_INVALID";
        break;
      case CONNECT_STAGE_NOT_STARTED:
        name = "CONNECT_STAGE_NOT_STARTED";
        break;
      case CONNECT_STAGE_NET_BEGIN_CONNECT:
        name = "CONNECT_STAGE_NET_BEGIN_CONNECT";
        break;
      case CONNECT_STAGE_NET_WAIT_CONNECT:
        name = "CONNECT_STAGE_NET_WAIT_CONNECT";
        break;
      case CONNECT_STAGE_NET_COMPLETE_CONNECT:
        name = "CONNECT_STAGE_NET_COMPLETE_CONNECT";
        break;
      case CONNECT_STAGE_READ_GREETING:
        name = "CONNECT_STAGE_READ_GREETING";
        break;
      case CONNECT_STAGE_PARSE_HANDSHAKE:
        name = "CONNECT_STAGE_PARSE_HANDSHAKE";
        break;
      case CONNECT_STAGE_ESTABLISH_SSL:
        name = "CONNECT_STAGE_ESTABLISH_SSL";
        break;
      case CONNECT_STAGE_AUTHENTICATE:
        name = "CONNECT_STAGE_AUTHENTICATE";
        break;
      case CONNECT_STAGE_PREP_SELECT_DATABASE:
        name = "CONNECT_STAGE_PREP_SELECT_DATABASE";
        break;
      case CONNECT_STAGE_PREP_INIT_COMMANDS:
        name = "CONNECT_STAGE_PREP_INIT_COMMANDS";
        break;
      case CONNECT_STAGE_SEND_ONE_INIT_COMMAND:
        name = "CONNECT_STAGE_SEND_ONE_INIT_COMMAND";
        break;
      case CONNECT_STAGE_COMPLETE:
        name = "CONNECT_STAGE_COMPLETE";
        break;
      default:
        break;
    }
    return fmt::format_to(ctx.out(), "{}", name);
  }
};
