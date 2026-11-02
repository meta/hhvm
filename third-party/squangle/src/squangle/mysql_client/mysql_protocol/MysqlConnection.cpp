/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/CppAttributes.h>
#include <folly/Exception.h>
#include <memory>
#include <string>
#include "mysql/server/include/mysql_async.h" // @manual=//mysql/server/include:mysql_async_namespaced
#include "mysql/server/include/violite.h" // @manual=//mysql/server/include:violite_namespaced

#include "squangle/base/ExceptionUtil.h"
#include "squangle/mysql_client/ConnectionOptions.h"
#include "squangle/mysql_client/mysql_protocol/MysqlConnection.h"
#include "squangle/mysql_client/mysql_protocol/MysqlResult.h"

namespace facebook::common::mysql_client::mysql_protocol {

MysqlConnection::MysqlConnection(MYSQL* mysql) : mysql_(mysql) {}

MysqlConnection::~MysqlConnection() {
  DCHECK(mysql_ == nullptr);
}

std::function<void()> MysqlConnection::getCloseFunction() {
  if (closeFdOnDestroy_ && mysql_) {
    auto mysql = mysql_;
    mysql_ = nullptr;
    // Close our connection in the thread from which it was created.
    return [=]() {
      // Unregister server cert validation callback
      const void* callback{nullptr};
      auto ret = mysql_options(mysql, MYSQL_OPT_TLS_CERT_CALLBACK, &callback);
      VLOG(4) << logMysqlOptionsImpl(
          mysql, "MYSQL_OPT_TLS_CERT_CALLBACK", callback, ret);
      DCHECK_EQ(ret, 0); // should always succeed

      mysql_close(mysql);
      VLOG(4) << fmt::format("mysql_close({})", (void*)mysql);
    };
  }

  return nullptr;
}

std::string MysqlConnection::serverInfo() const {
  CHECK_THROW(mysql_ != nullptr, db::InvalidConnectionException);

  auto ret = mysql_get_server_info(mysql_);
  VLOG(4) << fmt::format(
      "mysql_get_server_info({}) returned {}", (void*)mysql_, ret);
  return ret;
}

long MysqlConnection::threadId() const {
  CHECK_THROW(mysql_ != nullptr, db::InvalidConnectionException);

  auto ret = mysql_thread_id(mysql_);
  VLOG(4) << fmt::format("mysql_thread_id({}) returned {}", (void*)mysql_, ret);
  return ret;
}

void MysqlConnection::disableSSL() {
  CHECK_THROW(mysql_ != nullptr, db::InvalidConnectionException);

  enum mysql_ssl_mode ssl_mode = SSL_MODE_DISABLED;
  auto ret = mysql_options(mysql_, MYSQL_OPT_SSL_MODE, &ssl_mode);
  VLOG(4) << logMysqlOptions("MYSQL_OPT_SSL_MODE", ssl_mode, ret);
  DCHECK_EQ(ret, 0); // should always succeed
};

bool MysqlConnection::sslSessionReused() const {
  CHECK_THROW(mysql_ != nullptr, db::InvalidConnectionException);

  auto ret = mysql_get_ssl_session_reused(mysql_);
  VLOG(4) << fmt::format(
      "mysql_get_ssl_session_reused({}) returned {}", (void*)mysql_, ret);
  return ret;
}

std::string MysqlConnection::getTlsVersion() const {
  CHECK_THROW(mysql_ != nullptr, db::InvalidConnectionException);

  auto version = mysql_get_ssl_version(mysql_);
  VLOG(4) << fmt::format(
      "mysql_get_ssl_version({}) returned {}",
      (void*)mysql_,
      version ? version : "(null)");
  if (version) {
    return std::string(version);
  }

  return "";
}

void* FOLLY_NULLABLE MysqlConnection::getSSLHandle() const {
  CHECK_THROW(mysql_ != nullptr, db::InvalidConnectionException);
  if (mysql_->net.vio) {
    return mysql_->net.vio->ssl_arg;
  }
  return nullptr;
}

unsigned int MysqlConnection::warningCount() const {
  CHECK_THROW(mysql_ != nullptr, db::InvalidConnectionException);

  return mysql_warning_count(mysql_);
}

size_t MysqlConnection::escapeString(
    char* ptr,
    const char* src,
    size_t length,
    char quote) const {
  CHECK_THROW(mysql_ != nullptr, db::InvalidConnectionException);

  auto ret = mysql_real_escape_string_quote(mysql_, ptr, src, length, quote);
  VLOG(4) << fmt::format(
      "mysql_real_escape_string_quote({}, {}, {:.60} ({}), {}, {}) returned {}",
      (void*)mysql_,
      (void*)ptr,
      src ? src : "(null)",
      src,
      length,
      quote,
      ret);
  return ret;
}

folly::EventHandler::EventFlags MysqlConnection::getReadWriteState() const {
  CHECK_THROW(mysql_ != nullptr, db::InvalidConnectionException);

  NET_ASYNC* net_async = NET_ASYNC_DATA(&mysql_->net);
  // net_async can be null during some stages of connecting
  auto async_blocking_state =
      net_async ? net_async->async_blocking_state : NET_NONBLOCKING_CONNECT;
  switch (async_blocking_state) {
    case NET_NONBLOCKING_READ:
      return folly::EventHandler::READ;
    case NET_NONBLOCKING_WRITE:
    case NET_NONBLOCKING_CONNECT:
      return folly::EventHandler::WRITE;
  }

  // No default label: every net_async_block_state is handled above, so builds
  // with -Wswitch flag a state added by a future libmysqlclient. Correctness
  // does not depend on that warning -- an unhandled value simply falls through
  // to the throw below -- so open-source consumers building without -Wswitch
  // still behave safely.
  throw db::OperationStateException(
      fmt::format(
          "Unknown MySQL nonblocking state {}",
          static_cast<int>(async_blocking_state)));
}

unsigned int MysqlConnection::getErrno() const {
  CHECK_THROW(mysql_ != nullptr, db::InvalidConnectionException);

  auto ret = mysql_errno(mysql_);
  VLOG(4) << fmt::format("mysql_errno({}) returned {}", (void*)mysql_, ret);
  return ret;
}

std::string MysqlConnection::getErrorMessage() const {
  CHECK_THROW(mysql_ != nullptr, db::InvalidConnectionException);

  auto ret = mysql_error(mysql_);
  VLOG(4) << fmt::format("mysql_error({}) returned {}", (void*)mysql_, ret);
  return ret;
}

void MysqlConnection::setConnectAttributes(const AttributeMap& attributes) {
  CHECK_THROW(mysql_ != nullptr, db::InvalidConnectionException);

  // reset all connection attributes on the MYSQL connection
  auto ret = mysql_options(mysql_, MYSQL_OPT_CONNECT_ATTR_RESET, nullptr);
  VLOG(4) << logMysqlOptions("MYSQL_OPT_CONNECT_ATTR_RESET", nullptr, ret);
  DCHECK_EQ(ret, 0); // should always succeed

  // then applly the new attributes
  for (const auto& [key, value] : attributes) {
    ret = mysql_options4(
        mysql_, MYSQL_OPT_CONNECT_ATTR_ADD, key.c_str(), value.c_str());
    VLOG(4) << logMysqlOptions4("MYSQL_OPT_CONNECT_ATTR_ADD", key, value, ret);
    DCHECK_EQ(ret, 0); // should always succeed
  }
}

int MysqlConnection::setQueryAttributes(const AttributeMap& attributes) {
  CHECK_THROW(mysql_ != nullptr, db::InvalidConnectionException);

  // reset all connection attributes on the MYSQL connection
  auto ret = mysql_options(mysql_, MYSQL_OPT_QUERY_ATTR_RESET, nullptr);
  VLOG(4) << logMysqlOptions("MYSQL_OPT_QUERY_ATTR_RESET", nullptr, ret);
  if (ret != 0) {
    return ret;
  }

  // then applly the new attributes
  for (const auto& [key, value] : attributes) {
    if (ret = setQueryAttribute(key, value); ret != 0) {
      return ret;
    }
  }

  return 0;
}

int MysqlConnection::setQueryAttribute(
    const std::string& key,
    const std::string& value) {
  CHECK_THROW(mysql_ != nullptr, db::InvalidConnectionException);

  auto ret = mysql_options4(
      mysql_, MYSQL_OPT_QUERY_ATTR_ADD, key.c_str(), value.c_str());
  VLOG(4) << logMysqlOptions4("MYSQL_OPT_QUERY_ATTR_ADD", key, value, ret);
  return ret;
}

namespace {

std::string formatRespAttrEntry(
    std::string_view func,
    MYSQL* mysql,
    const char*& data,
    const size_t& len,
    int ret) {
  return fmt::format(
      "{}({}, SESSION_TRACK_RESP_ATTR, {}, {}) returned {}, data returned: {}",
      func,
      (void*)mysql,
      (void*)data,
      (void*)&len,
      ret,
      ((ret != 0 || !data) ? "<null>" : std::string_view(data, len)));
}

static std::optional<std::string> readFirstRespAttrEntry(MYSQL* mysql) {
  size_t len;
  const char* data;

  auto ret = mysql_session_track_get_first(
      mysql, SESSION_TRACK_RESP_ATTR, &data, &len);
  VLOG(4) << formatRespAttrEntry(
      "mysql_session_track_get_first", mysql, data, len, ret);
  if (ret == 0) {
    return std::string(data, len);
  }

  return std::nullopt;
}

static std::optional<std::string> readNextRespAttrEntry(MYSQL* mysql) {
  size_t len;
  const char* data;

  auto ret =
      mysql_session_track_get_next(mysql, SESSION_TRACK_RESP_ATTR, &data, &len);
  VLOG(4) << formatRespAttrEntry(
      "mysql_session_track_get_next", mysql, data, len, ret);
  if (ret == 0) {
    return std::string(data, len);
  }

  return std::nullopt;
}

static folly::Optional<std::pair<std::string, std::string>>
readFirstResponseAtribute(MYSQL* mysql) {
  if (auto key = readFirstRespAttrEntry(mysql)) {
    if (auto value = readNextRespAttrEntry(mysql)) {
      return std::make_pair(std::move(*key), std::move(*value));
    }
  }

  return folly::none;
}

static folly::Optional<std::pair<std::string, std::string>>
readNextResponseAtribute(MYSQL* mysql) {
  if (auto key = readNextRespAttrEntry(mysql)) {
    if (auto value = readNextRespAttrEntry(mysql)) {
      return std::make_pair(std::move(*key), std::move(*value));
    }
  }

  return folly::none;
}

} // namespace

AttributeMap MysqlConnection::getResponseAttributes() const {
  CHECK_THROW(mysql_ != nullptr, db::InvalidConnectionException);

  AttributeMap attrs;

  for (auto attr = readFirstResponseAtribute(mysql_); attr;
       attr = readNextResponseAtribute(mysql_)) {
    attrs[std::move(attr->first)] = std::move(attr->second);
  }

  return attrs;
}

void MysqlConnection::setCompression(CompressionAlgorithm algo) {
  CHECK_THROW(mysql_ != nullptr, db::InvalidConnectionException);

  auto ret = mysql_options(mysql_, MYSQL_OPT_COMPRESS, nullptr);
  VLOG(4) << logMysqlOptions("MYSQL_OPT_COMPRESS", nullptr, ret);
  DCHECK_EQ(ret, 0); // should always succeed

  setCompressionOption(mysql_, algo);
}

bool MysqlConnection::setSSLOptionsProvider(SSLOptionsProviderBase& provider) {
  CHECK_THROW(mysql_ != nullptr, db::InvalidConnectionException);

  return provider.setMysqlSSLOptions(mysql_);
}

std::optional<std::string> MysqlConnection::getSniServerName() const {
  CHECK_THROW(mysql_ != nullptr, db::InvalidConnectionException);

  const char* opt_val = nullptr;
  auto ret = mysql_get_option(mysql_, MYSQL_OPT_TLS_SNI_SERVERNAME, &opt_val);
  VLOG(4) << fmt::format(
      "mysql_get_option({}, MYSQL_OPT_TLS_SNI_SERVERNAME, {}), returned {}, data = {}",
      (void*)mysql_,
      (void*)&opt_val,
      ret,
      ((ret != 0 || !opt_val) ? "<null>" : opt_val));
  if (ret == 0 && opt_val != nullptr) {
    return opt_val;
  }

  return std::nullopt;
}

void MysqlConnection::setSniServerName(const std::string& name) {
  CHECK_THROW(mysql_ != nullptr, db::InvalidConnectionException);

  auto ret = mysql_options(mysql_, MYSQL_OPT_TLS_SNI_SERVERNAME, name.c_str());
  VLOG(4) << logMysqlOptions("MYSQL_OPT_COMPRESS", name, ret);
  DCHECK_EQ(ret, 0); // should always succeed
}

bool MysqlConnection::setDscp(uint8_t dscp) {
  CHECK_THROW(mysql_ != nullptr, db::InvalidConnectionException);

  // DS field (QOS/TOS level) is 8 bits with DSCP packed into the most
  // significant 6 bits.
  uint dsf = dscp << 2;
  auto ret = mysql_options(mysql_, MYSQL_OPT_TOS, &dsf) == 0;
  VLOG(4) << logMysqlOptions("MYSQL_OPT_TOS", dsf, ret);
  return ret;
}

void MysqlConnection::setCertValidatorCallback(
    const MysqlCertValidatorCallback& cb,
    void* context) {
  CHECK_THROW(mysql_ != nullptr, db::InvalidConnectionException);

  auto ret = mysql_options(mysql_, MYSQL_OPT_TLS_CERT_CALLBACK, &cb);
  VLOG(4) << logMysqlOptions("MYSQL_OPT_TLS_CERT_CALLBACK", &cb, ret);
  DCHECK_EQ(ret, 0); // should always succeed

  ret = mysql_options(mysql_, MYSQL_OPT_TLS_CERT_CALLBACK_CONTEXT, &context);
  VLOG(4) << logMysqlOptions(
      "MYSQL_OPT_TLS_CERT_CALLBACK_CONTEXT", &context, ret);
  DCHECK_EQ(ret, 0); // should always succeed
}

void MysqlConnection::clearCertValidatorCallback() {
  if (mysql_ == nullptr) {
    return;
  }

  MysqlCertValidatorCallback nullCb = nullptr;
  void* nullCtx = nullptr;

  auto ret = mysql_options(mysql_, MYSQL_OPT_TLS_CERT_CALLBACK, &nullCb);
  VLOG(4) << logMysqlOptions("MYSQL_OPT_TLS_CERT_CALLBACK", nullptr, ret);
  DCHECK_EQ(ret, 0);

  ret = mysql_options(mysql_, MYSQL_OPT_TLS_CERT_CALLBACK_CONTEXT, &nullCtx);
  VLOG(4) << logMysqlOptions(
      "MYSQL_OPT_TLS_CERT_CALLBACK_CONTEXT", nullptr, ret);
  DCHECK_EQ(ret, 0);
}

void MysqlConnection::setConnectTimeout(Millis timeout) const {
  CHECK_THROW(mysql_ != nullptr, db::InvalidConnectionException);

  uint timeoutInMs = timeout.count();
  auto ret = mysql_options(mysql_, MYSQL_OPT_CONNECT_TIMEOUT_MS, &timeoutInMs);
  VLOG(4) << logMysqlOptions("MYSQL_OPT_CONNECT_TIMEOUT_MS", timeoutInMs, ret);
  DCHECK_EQ(ret, 0); // should always succeed
}

void MysqlConnection::setReadTimeout(Millis timeout) const {
  CHECK_THROW(mysql_ != nullptr, db::InvalidConnectionException);

  uint timeoutInMs = timeout.count();
  auto ret = mysql_options(mysql_, MYSQL_OPT_READ_TIMEOUT_MS, &timeoutInMs);
  VLOG(4) << logMysqlOptions("MYSQL_OPT_READ_TIMEOUT_MS", timeoutInMs, ret);
  DCHECK_EQ(ret, 0); // should always succeed
}

void MysqlConnection::setWriteTimeout(Millis timeout) const {
  CHECK_THROW(mysql_ != nullptr, db::InvalidConnectionException);

  uint timeoutInMs = timeout.count();
  auto ret = mysql_options(mysql_, MYSQL_OPT_WRITE_TIMEOUT_MS, &timeoutInMs);
  VLOG(4) << logMysqlOptions("MYSQL_OPT_WRITE_TIMEOUT_MS", timeoutInMs, ret);
  DCHECK_EQ(ret, 0); // should always succeed
}

int MysqlConnection::getSocketDescriptor() const {
  CHECK_THROW(mysql_ != nullptr, db::InvalidConnectionException);

  auto ret = mysql_get_socket_descriptor(mysql_);
  VLOG(4) << fmt::format(
      "mysql_get_socket_descriptor({}) returned {}", (void*)mysql_, ret);
  return ret;
}

/*static*/ std::optional<std::string> MysqlConnection::findConnectStageName(
    connect_stage stage) {
  // enum connect_stage is defined in mysql at include/mysql_com.h
  // and this provides a way to log the string version of this enum
  static const folly::F14FastMap<connect_stage, std::string> stageToStringMap =
      {
          {connect_stage::CONNECT_STAGE_INVALID, "CONNECT_STAGE_INVALID"},
          {connect_stage::CONNECT_STAGE_NOT_STARTED,
           "CONNECT_STAGE_NOT_STARTED"},
          {connect_stage::CONNECT_STAGE_NET_BEGIN_CONNECT,
           "CONNECT_STAGE_NET_BEGIN_CONNECT"},
#if MYSQL_VERSION_ID >= 80020 // csm_wait_connect added in 8.0.20
          {connect_stage::CONNECT_STAGE_NET_WAIT_CONNECT,
           "CONNECT_STAGE_NET_WAIT_CONNECT"},
#endif
          {connect_stage::CONNECT_STAGE_NET_COMPLETE_CONNECT,
           "CONNECT_STAGE_NET_COMPLETE_CONNECT"},
          {connect_stage::CONNECT_STAGE_READ_GREETING,
           "CONNECT_STAGE_READ_GREETING"},
          {connect_stage::CONNECT_STAGE_PARSE_HANDSHAKE,
           "CONNECT_STAGE_PARSE_HANDSHAKE"},
          {connect_stage::CONNECT_STAGE_ESTABLISH_SSL,
           "CONNECT_STAGE_ESTABLISH_SSL"},
          {connect_stage::CONNECT_STAGE_AUTHENTICATE,
           "CONNECT_STAGE_AUTHENTICATE"},
          {connect_stage::CONNECT_STAGE_PREP_SELECT_DATABASE,
           "CONNECT_STAGE_PREP_SELECT_DATABASE"},
          {connect_stage::CONNECT_STAGE_PREP_INIT_COMMANDS,
           "CONNECT_STAGE_PREP_INIT_COMMANDS"},
          {connect_stage::CONNECT_STAGE_SEND_ONE_INIT_COMMAND,
           "CONNECT_STAGE_SEND_ONE_INIT_COMMAND"},
          {connect_stage::CONNECT_STAGE_COMPLETE, "CONNECT_STAGE_COMPLETE"},
      };

  if (auto it = stageToStringMap.find(stage); it != stageToStringMap.end()) {
    return it->second;
  }

  return std::nullopt;
}

uint64_t MysqlConnection::getLastInsertId() const {
  CHECK_THROW(mysql_ != nullptr, db::InvalidConnectionException);

  auto ret = mysql_insert_id(mysql_);
  VLOG(4) << fmt::format("mysql_insert_id({}) returned {}", (void*)mysql_, ret);
  return ret;
}

uint64_t MysqlConnection::getAffectedRows() const {
  CHECK_THROW(mysql_ != nullptr, db::InvalidConnectionException);

  auto ret = mysql_affected_rows(mysql_);
  VLOG(4) << fmt::format(
      "mysql_affected_rows({}) returned {}", (void*)mysql_, ret);
  return ret;
}

std::optional<std::string> MysqlConnection::getMySQLInfo() const {
  CHECK_THROW(mysql_ != nullptr, db::InvalidConnectionException);

  if (auto ret = mysql_info(mysql_)) {
    VLOG(4) << fmt::format("mysql_info({}) returned {}", (void*)mysql_, ret);
    return std::string(ret);
  }
  return std::nullopt;
}

std::string MysqlConnection::getConnectStageName() const {
  auto stage = getMySqlConnectStage();
  if (auto optStageName = findConnectStageName(stage)) {
    return *std::move(optStageName);
  }

  return fmt::format("Unexpected connect_stage: {}", (int)stage);
}

std::optional<std::string> MysqlConnection::getRecvGtid() const {
  CHECK_THROW(mysql_ != nullptr, db::InvalidConnectionException);

  const char* data;
  size_t length;
  auto ret = mysql_session_track_get_first(
      mysql_, SESSION_TRACK_GTIDS, &data, &length);
  VLOG(4) << fmt::format(
      "mysql_session_track_get_first({}, SESSION_TRACK_GTIDS, {}, {}) returned {}, data = {}",
      (void*)mysql_,
      (void*)&data,
      (void*)&length,
      ret,
      ((ret || !data) ? "<null>" : std::string_view(data, length)));
  if (ret == 0) {
    return std::string(data, length);
  }

  return std::nullopt;
}

std::optional<std::string> MysqlConnection::getSchemaChanged() const {
  CHECK_THROW(mysql_ != nullptr, db::InvalidConnectionException);

  const char* data;
  size_t length;
  auto ret = mysql_session_track_get_first(
      mysql_, SESSION_TRACK_SCHEMA, &data, &length);
  VLOG(4) << fmt::format(
      "mysql_session_track_get_first({}, SESSION_TRACK_SCHEMA, {}, {}) returned {}, data = {}",
      (void*)mysql_,
      (void*)&data,
      (void*)&length,
      ret,
      ((ret || !data) ? "<null>" : std::string_view(data, length)));
  if (ret == 0) {
    return std::string(data, length);
  }

  return std::nullopt;
}

bool MysqlConnection::hasMoreResults() const {
  CHECK_THROW(mysql_ != nullptr, db::InvalidConnectionException);

  auto ret = mysql_more_results(mysql_);
  VLOG(4) << fmt::format(
      "mysql_more_results({}) returned {}", (void*)mysql_, ret);
  return ret;
}

InternalConnection::Status SyncMysqlConnection::tryConnect(
    const ConnectionOptions& opts,
    std::shared_ptr<const ConnectionKey> conn_key,
    int flags) {
  static std::string kEmptyString;

  CHECK_THROW(mysql_ != nullptr, db::InvalidConnectionException);

  auto mysqlConnKey =
      std::dynamic_pointer_cast<const MysqlConnectionKey>(conn_key);
  DCHECK(mysqlConnKey);

  auto qtmo = std::chrono::duration_cast<Millis>(opts.getQueryTimeout());
  auto ctmo = std::chrono::duration_cast<Millis>(opts.getTimeout());

  setConnectTimeout(ctmo);
  setReadTimeout(qtmo);
  setWriteTimeout(qtmo);

  const auto& unixSocket = mysqlConnKey->unixSocketPath();
  const auto usingUnixSocket = !unixSocket.empty();

  const auto& host = usingUnixSocket ? kEmptyString : mysqlConnKey->host();
  const auto& user = mysqlConnKey->user();
  const auto& password = mysqlConnKey->password();
  const auto& db_name = mysqlConnKey->db_name();
  auto port = usingUnixSocket ? 0 : mysqlConnKey->port();

  // When using unix socket (AF_UNIX), host/port do not matter.
  auto ret = mysql_real_connect(
      mysql_,
      host.c_str(),
      user.c_str(),
      password.c_str(),
      db_name.c_str(),
      port,
      unixSocket.c_str(),
      flags);
  VLOG(4) << fmt::format(
      "mysql_real_connect({}, {}, {}, {}, {}, {}, {}, {}) returned {}",
      (void*)mysql_,
      host,
      user,
      password,
      db_name,
      port,
      unixSocket,
      flags,
      (void*)ret);
  return ret == nullptr ? ERROR : DONE;
}

InternalConnection::Status AsyncMysqlConnection::tryConnect(
    const ConnectionOptions& /*opts*/,
    std::shared_ptr<const ConnectionKey> conn_key,
    int flags) {
  static std::string kEmptyString;

  CHECK_THROW(mysql_ != nullptr, db::InvalidConnectionException);

  auto mysqlConnKey =
      std::dynamic_pointer_cast<const MysqlConnectionKey>(conn_key);
  DCHECK(mysqlConnKey);

  const auto& unixSocket = mysqlConnKey->unixSocketPath();
  const auto usingUnixSocket = !unixSocket.empty();

  const auto& host = usingUnixSocket ? kEmptyString : mysqlConnKey->host();
  const auto& user = mysqlConnKey->user();
  const auto& password = mysqlConnKey->password();
  const auto& db_name = mysqlConnKey->db_name();
  auto port = usingUnixSocket ? 0 : mysqlConnKey->port();

  // When using unix socket (AF_UNIX), host/port do not matter.
  auto ret = mysql_real_connect_nonblocking(
      mysql_,
      host.empty() ? nullptr : host.c_str(),
      user.c_str(),
      password.c_str(),
      db_name.c_str(),
      port,
      !unixSocket.empty() ? unixSocket.c_str() : nullptr,
      flags);
  VLOG(4) << fmt::format(
      "mysql_real_connect({}, {}, {}, {}, {}, {}, {}, {}) returned {}",
      (void*)mysql_,
      host,
      user,
      password,
      db_name,
      port,
      unixSocket,
      flags,
      ret);
  return toHandlerStatus(ret);
}

InternalConnection::Status SyncMysqlConnection::runQuery(
    std::string_view query) const {
  CHECK_THROW(mysql_ != nullptr, db::InvalidConnectionException);

  auto ret = mysql_real_query(mysql_, query.data(), query.size());
  VLOG(4) << fmt::format(
      "mysql_real_query({}, {:60}, {}) returned {}",
      (void*)mysql_,
      query,
      query.size(),
      ret);
  return ret ? ERROR : DONE;
}

InternalConnection::Status AsyncMysqlConnection::runQuery(
    std::string_view query) const {
  CHECK_THROW(mysql_ != nullptr, db::InvalidConnectionException);

  auto ret = mysql_real_query_nonblocking(mysql_, query.data(), query.size());
  VLOG(4) << fmt::format(
      "mysql_real_query_nonblocking({}, {:60}, {}) returned {}",
      (void*)mysql_,
      query,
      query.size(),
      ret);
  return toHandlerStatus(ret);
}

InternalConnection::Status SyncMysqlConnection::resetConn() const {
  CHECK_THROW(mysql_ != nullptr, db::InvalidConnectionException);

  auto ret = mysql_reset_connection(mysql_);
  VLOG(4) << fmt::format(
      "mysql_reset_connection({}) returned {}", (void*)mysql_, ret);
  return ret ? ERROR : DONE;
}

InternalConnection::Status AsyncMysqlConnection::resetConn() const {
  CHECK_THROW(mysql_ != nullptr, db::InvalidConnectionException);

  auto ret = mysql_reset_connection_nonblocking(mysql_);
  VLOG(4) << fmt::format(
      "mysql_reset_connection_nonblocking({}) returned {}", (void*)mysql_, ret);
  return toHandlerStatus(ret);
}

InternalConnection::Status SyncMysqlConnection::changeUser(
    std::shared_ptr<const ConnectionKey> connKey) const {
  CHECK_THROW(mysql_ != nullptr, db::InvalidConnectionException);

  auto mysqlConnKey =
      std::dynamic_pointer_cast<const MysqlConnectionKey>(connKey);
  DCHECK(mysqlConnKey);
  auto user = mysqlConnKey->user();
  auto password = mysqlConnKey->password();
  auto database = mysqlConnKey->db_name();
  auto ret = mysql_change_user(
      mysql_, user.c_str(), password.c_str(), database.c_str());
  VLOG(4) << fmt::format(
      "mysql_change_user({}, {}, {}, {}) returned {}",
      (void*)mysql_,
      user,
      password,
      database,
      ret);
  return ret ? ERROR : DONE;
}

InternalConnection::Status AsyncMysqlConnection::changeUser(
    std::shared_ptr<const ConnectionKey> connKey) const {
  CHECK_THROW(mysql_ != nullptr, db::InvalidConnectionException);

  auto mysqlConnKey =
      std::dynamic_pointer_cast<const MysqlConnectionKey>(connKey);
  DCHECK(mysqlConnKey);
  auto user = mysqlConnKey->user();
  auto password = mysqlConnKey->password();
  auto database = mysqlConnKey->db_name();
  auto ret = mysql_change_user_nonblocking(
      mysql_, user.c_str(), password.c_str(), database.c_str());
  VLOG(4) << fmt::format(
      "mysql_change_user_nonblocking({}, {}, {}, {}) returned {}",
      (void*)mysql_,
      user,
      password,
      database,
      ret);
  return toHandlerStatus(ret);
}

InternalConnection::Status SyncMysqlConnection::nextResult() const {
  CHECK_THROW(mysql_ != nullptr, db::InvalidConnectionException);

  auto ret = mysql_next_result(mysql_);
  VLOG(4) << fmt::format(
      "mysql_next_result({}) returned {}", (void*)mysql_, ret);
  return ret ? ERROR : DONE;
}

InternalConnection::Status AsyncMysqlConnection::nextResult() const {
  CHECK_THROW(mysql_ != nullptr, db::InvalidConnectionException);

  auto ret = mysql_next_result_nonblocking(mysql_);
  VLOG(4) << fmt::format(
      "mysql_next_result_nonblocking({}) returned {}", (void*)mysql_, ret);
  return toHandlerStatus(ret);
}

std::unique_ptr<MysqlResult> SyncMysqlConnection::getResult() const {
  CHECK_THROW(mysql_ != nullptr, db::InvalidConnectionException);

  MYSQL_RES* res = mysql_store_result(mysql_);
  VLOG(4) << fmt::format(
      "mysql_store_result({}) returned {}", (void*)mysql_, (void*)res);
  if (!res) {
    return {};
  }

  return std::make_unique<SyncMysqlResult>(res);
}

std::unique_ptr<MysqlResult> AsyncMysqlConnection::getResult() const {
  CHECK_THROW(mysql_ != nullptr, db::InvalidConnectionException);

  MYSQL_RES* res = mysql_use_result(mysql_);
  VLOG(4) << fmt::format(
      "mysql_use_result({}) returned {}", (void*)mysql_, (void*)res);
  if (!res) {
    return {};
  }

  return std::make_unique<AsyncMysqlResult>(res);
}

size_t MysqlConnection::getFieldCount() const {
  CHECK_THROW(mysql_ != nullptr, db::InvalidConnectionException);

  auto ret = mysql_field_count(mysql_);
  VLOG(4) << fmt::format(
      "mysql_field_count({}) returned {}", (void*)mysql_, ret);
  return ret;
}

bool MysqlConnection::dumpDebugInfo() const {
  CHECK_THROW(mysql_ != nullptr, db::InvalidConnectionException);

  auto ret = mysql_dump_debug_info(mysql_);
  VLOG(4) << fmt::format(
      "mysql_dump_debug_info({}) returned {}", (void*)mysql_, ret);
  return ret == 0;
}

bool MysqlConnection::ping() const {
  CHECK_THROW(mysql_ != nullptr, db::InvalidConnectionException);

  auto ret = mysql_ping(mysql_);
  VLOG(4) << fmt::format("mysql_ping({}) returned {}", (void*)mysql_, ret);
  return ret == 0;
}

bool MysqlConnection::isUnusableOnCheckout() const {
#ifdef MYSQL_HAS_NET_PENDING_DATA
  // An idle connection has consumed every response it asked for, so any
  // pending input is the server's close notice or other unrequested data.
  return mysql_net_has_pending_data(mysql_);
#else
  // Client libraries without mysql_net_has_pending_data get only the expiry
  // check.
  return false;
#endif
}

MysqlConnection::MySqlConnectStage MysqlConnection::getMySqlConnectStage()
    const {
  CHECK_THROW(mysql_ != nullptr, db::InvalidConnectionException);

  auto ret = mysql_get_connect_stage(mysql_);
  VLOG(4) << fmt::format(
      "mysql_get_connect_stage({}) returned {}", (void*)mysql_, ret);
  return ret;
}

/*static*/ MYSQL* MysqlConnection::createMysql() {
  auto ret = mysql_init(nullptr);
  VLOG(4) << fmt::format("mysql_init({}) returned {}", nullptr, (void*)ret);
  MYSQL* mysql = static_cast<MYSQL*>(ret);
  if (!mysql) {
    throw std::runtime_error("mysql_init failed");
  }
  return mysql;
}

std::string MysqlConnection::logMysqlOptionsImpl(
    MYSQL* mysql,
    std::string_view opt,
    const void* param,
    int ret) {
  return fmt::format(
      "mysql_options({}, {}, {}) returned {}", (void*)mysql, opt, param, ret);
}

std::string MysqlConnection::logMysqlOptionsImpl(
    MYSQL* mysql,
    std::string_view opt,
    int param,
    int ret) {
  return fmt::format(
      "mysql_options({}, {}, {}) returned {}", (void*)mysql, opt, param, ret);
}

std::string MysqlConnection::logMysqlOptionsImpl(
    MYSQL* mysql,
    std::string_view opt,
    const std::string& param,
    int ret) {
  return fmt::format(
      "mysql_options({}, {}, {}) returned {}", (void*)mysql, opt, param, ret);
}

std::string MysqlConnection::logMysqlOptions4Impl(
    MYSQL* mysql,
    std::string_view opt,
    const std::string& param1,
    const std::string& param2,
    int ret) {
  return fmt::format(
      "mysql_options4({}, {}, {}, {}) returned {}",
      (void*)mysql,
      opt,
      param1,
      param2,
      ret);
}

} // namespace facebook::common::mysql_client::mysql_protocol
