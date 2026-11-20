/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/io/async/AsyncTimeout.h>

#include "squangle/base/ConnectionKey.h"
#include "squangle/mysql_client/ConnectionOptions.h"
#include "squangle/mysql_client/Operation.h"
#include "squangle/mysql_client/SSLOptionsProviderBase.h"

namespace facebook::common::mysql_client {

class ConnectOperation;
class ConnectionHolder;

// An operation representing a pending connection.  Constructed via
// AsyncMysqlClient::beginConnection.
class ConnectOperationImpl : virtual public OperationBase {
 public:
  ConnectOperationImpl(
      MysqlClientBase* mysql_client,
      std::shared_ptr<const ConnectionKey> conn_key);
  virtual ~ConnectOperationImpl() override = default;

  db::OperationType getOperationType() const override {
    return db::OperationType::Connect;
  }

  const std::string& database() const {
    return conn_key_->db_name();
  }
  const std::string& user() const {
    return conn_key_->user();
  }

  std::shared_ptr<const ConnectionKey> getConnectionKey() const {
    return conn_key_;
  }
  std::shared_ptr<const ConnectionKey> getKey() const {
    return conn_key_;
  }
  const ConnectionKey& getKeyRef() const {
    return *conn_key_;
  }

  void setConnectionOptions(const ConnectionOptions& conn_options);
  const ConnectionOptions& getConnectionOptions() const;

  void setSSLOptionsProviderBase(
      std::unique_ptr<SSLOptionsProviderBase> ssl_options_provider);
  void setSSLOptionsProvider(
      std::shared_ptr<SSLOptionsProviderBase> ssl_options_provider);

  // Default timeout for queries created by the connection this
  // operation will create.
  void setDefaultQueryTimeout(Duration t);

  void setSniServerName(const std::string& sni_servername);
  void enableResetConnBeforeClose();
  void enableDelayedResetConn();
  void enableChangeUser();
  void enableLoopStalledConnectTimeoutErrno();

  void setCertValidationCallback(CertValidatorCallback callback);

  const CertValidatorCallback& getCertValidationCallback() const {
    return conn_options_.getCertValidationCallback();
  }

  // Overriding to narrow the return type
  // Each connect attempt will take at most this timeout to retry to acquire
  // the connection.
  void setTimeout(Duration timeout);

  // This timeout allows for clients to fail fast when tcp handshake
  // latency is high . This method allows to override the tcp timeout
  // connection options. These timeouts can either be set directly or by
  // passing in connection options.
  void setTcpTimeout(Duration timeout);

  const folly::Optional<Duration>& getTcpTimeout() const {
    return conn_options_.getConnectTcpTimeout();
  }

  // Sets the total timeout that the connect operation will use.
  // Each attempt will take at most `setTimeout`. Use this in case
  // you have strong timeout restrictions but still want the connection to
  // retry.
  void setTotalTimeout(Duration total_timeout);

  // Sets the number of attempts this operation will try to acquire a mysql
  // connection.
  void setConnectAttempts(uint32_t max_attempts);

  // Sets the DSCP (QoS) value associated with this connection
  //
  // See Also ConnectionOptions::setDscp
  void setDscp(uint8_t dscp);
  folly::Optional<uint8_t> getDscp() const {
    return conn_options_.getDscp();
  }

  uint32_t attemptsMade() const noexcept {
    return attempts_made_;
  }
  Duration getAttemptTimeout() const noexcept {
    return conn_options_.getTimeout();
  }

  // Set if we should open a new connection to kill a timed out query
  // Should not be used when connecting through a proxy
  void setKillOnQueryTimeout(bool killOnQueryTimeout);
  bool getKillOnQueryTimeout() const noexcept {
    return killOnQueryTimeout_;
  }

  void setCompression(folly::Optional<CompressionAlgorithm> compression_lib) {
    conn_options_.setCompression(std::move(compression_lib));
  }
  const folly::Optional<CompressionAlgorithm>& getCompression() const {
    return conn_options_.getCompression();
  }

  void setCryptoAuthTokenList(std::string cryptoAuthTokenList) {
    conn_options_.setCryptoAuthTokenList(std::move(cryptoAuthTokenList));
  }
  folly::Optional<std::string> getCryptoAuthTokenList() const {
    return conn_options_.getCryptoAuthTokenList();
  }

  virtual bool isActive() const {
    return false;
  }

 protected:
  ConnectOperation& op() const;

  // Functions to deal with the connection
  [[nodiscard]] const InternalConnection& getInternalConnection() const;
  [[nodiscard]] InternalConnection& getInternalConnection();
  ConnectionHolder* mysqlConnection() const;

  [[nodiscard]] std::string generateTimeoutError(
      Millis elapsed,
      const std::function<uint16_t(bool)>& getErrorNo,
      std::string apiName,
      std::optional<std::string> location,
      std::optional<std::string> extra) const;

  ConnectionOptions conn_options_;
  std::shared_ptr<const ConnectionKey> conn_key_;
  uint32_t attempts_made_ = 0;
  bool killOnQueryTimeout_ = false;
};

class ConnectOperation : public Operation {
 public:
  void setCallback(ConnectCallback cb) {
    connect_callback_ = std::move(cb);
  }

  void callConnectCallback();

  // Overriding to narrow the return type
  // Each connect attempt will take at most this timeout to retry to acquire
  // the connection.
  ConnectOperation& setTimeout(Duration timeout) {
    impl()->setTimeout(timeout);
    return *this;
  }

  // This timeout allows for clients to fail fast when tcp handshake
  // latency is high . This method allows to override the tcp timeout
  // connection options. These timeouts can either be set directly or by
  // passing in connection options.
  ConnectOperation& setTcpTimeout(Duration timeout) {
    impl()->setTcpTimeout(timeout);
    return *this;
  }
  const folly::Optional<Duration>& getTcpTimeout() const {
    return impl()->getTcpTimeout();
  }

  // Sets the total timeout that the connect operation will use.
  // Each attempt will take at most `setTimeout`. Use this in case
  // you have strong timeout restrictions but still want the connection to
  // retry.
  ConnectOperation& setTotalTimeout(Duration total_timeout) {
    impl()->setTotalTimeout(total_timeout);
    return *this;
  }

  // Sets the number of attempts this operation will try to acquire a mysql
  // connection.
  ConnectOperation& setConnectAttempts(uint32_t max_attempts) {
    impl()->setConnectAttempts(max_attempts);
    return *this;
  }

  // Sets the DSCP (QoS) value associated with this connection
  //
  // See Also ConnectionOptions::setDscp
  ConnectOperation& setDscp(uint8_t dscp) {
    impl()->setDscp(dscp);
    return *this;
  }
  folly::Optional<uint8_t> getDscp() const {
    return impl()->getDscp();
  }

  uint32_t attemptsMade() const noexcept {
    return impl()->attemptsMade();
  }

  Duration getAttemptTimeout() const noexcept {
    return impl()->getAttemptTimeout();
  }

  // Set if we should open a new connection to kill a timed out query
  // Should not be used when connecting through a proxy
  ConnectOperation& setKillOnQueryTimeout(bool killOnQueryTimeout) {
    impl()->setKillOnQueryTimeout(killOnQueryTimeout);
    return *this;
  }
  bool getKillOnQueryTimeout() const noexcept {
    return impl()->getKillOnQueryTimeout();
  }

  ConnectOperation& setCompression(
      folly::Optional<CompressionAlgorithm> compression_lib) {
    impl()->setCompression(std::move(compression_lib));
    return *this;
  }
  const folly::Optional<CompressionAlgorithm>& getCompression() const {
    return impl()->getCompression();
  }

  ConnectOperation& setSSLOptionsProviderBase(
      std::unique_ptr<SSLOptionsProviderBase> ssl_options_provider) {
    impl()->setSSLOptionsProviderBase(std::move(ssl_options_provider));
    return *this;
  }
  ConnectOperation& setSSLOptionsProvider(
      std::shared_ptr<SSLOptionsProviderBase> ssl_options_provider) {
    impl()->setSSLOptionsProvider(std::move(ssl_options_provider));
    return *this;
  }

  ConnectOperation& setConnectionOptions(
      const ConnectionOptions& conn_options) {
    impl()->setConnectionOptions(conn_options);
    return *this;
  }
  // Default timeout for queries created by the connection this
  // operation will create.
  ConnectOperation& setDefaultQueryTimeout(Duration t) {
    impl()->setDefaultQueryTimeout(t);
    return *this;
  }
  ConnectOperation& setSniServerName(const std::string& sni_servername) {
    impl()->setSniServerName(sni_servername);
    return *this;
  }
  ConnectOperation& enableResetConnBeforeClose() {
    impl()->enableResetConnBeforeClose();
    return *this;
  }
  ConnectOperation& enableDelayedResetConn() {
    impl()->enableDelayedResetConn();
    return *this;
  }
  ConnectOperation& enableChangeUser() {
    impl()->enableChangeUser();
    return *this;
  }
  ConnectOperation& enableLoopStalledConnectTimeoutErrno() {
    impl()->enableLoopStalledConnectTimeoutErrno();
    return *this;
  }
  ConnectOperation& setCertValidationCallback(CertValidatorCallback callback) {
    impl()->setCertValidationCallback(std::move(callback));
    return *this;
  }

  const ConnectionOptions& getConnectionOptions() {
    return impl()->getConnectionOptions();
  }
  std::shared_ptr<const ConnectionKey> getKey() const {
    return impl()->getKey();
  }
  const ConnectionKey& getKeyRef() const {
    return impl()->getKeyRef();
  }

  db::OperationType getOperationType() const override {
    return db::OperationType::Connect;
  }

  void reportServerCertContent(
      const std::string& sslCertCn,
      const std::vector<std::string>& sslCertSan,
      const std::vector<std::string>& sslCertIdentities,
      bool isValidated) const override {
    // Make sure we have an impl - it is possible to not have one.
    if (auto* implPtr = impl()) {
      implPtr->withOptionalConnectionContext([&](auto& ctx) {
        if (!sslCertCn.empty()) {
          ctx.sslCertCn = sslCertCn;
        }
        if (!sslCertSan.empty()) {
          ctx.sslCertSan = sslCertSan;
        }
        if (!sslCertIdentities.empty()) {
          ctx.sslCertIdentities = sslCertIdentities;
        }
        ctx.isServerCertValidated = isValidated;
      });
    }
  }

  bool isActive() const {
    return impl()->isActive();
  }

  const CertValidatorCallback& getCertValidationCallback() const {
    return impl()->getCertValidationCallback();
  }

 protected:
  static std::shared_ptr<ConnectOperation> create(
      std::unique_ptr<ConnectOperationImpl> impl);
  friend MysqlClientBase;

  explicit ConnectOperation(std::unique_ptr<ConnectOperationImpl> impl)
      : impl_(std::move(impl)) {
    if (!impl_) {
      throw std::runtime_error("ConnectOperationImpl is null");
    }

    impl_->setOperation(*this);
  }

  virtual ConnectOperationImpl* impl() override {
    return impl_.get();
  }
  virtual const ConnectOperationImpl* impl() const override {
    return impl_.get();
  }

  std::unique_ptr<ConnectOperationImpl> impl_;

 private:
  ConnectCallback connect_callback_;
};

} // namespace facebook::common::mysql_client
