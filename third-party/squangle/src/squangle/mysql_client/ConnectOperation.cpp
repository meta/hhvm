/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/polymorphic_cast.hpp>
#include <memory>

#include "squangle/mysql_client/ConnectOperation.h"
#include "squangle/mysql_client/Connection.h"

namespace facebook::common::mysql_client {

using namespace std::chrono_literals;

ConnectOperationImpl::ConnectOperationImpl(
    MysqlClientBase* mysql_client,
    std::shared_ptr<const ConnectionKey> conn_key)
    : conn_key_(std::move(conn_key)) {
  mysql_client->activeConnectionAdded(conn_key_);
}

void ConnectOperationImpl::setConnectionOptions(
    const ConnectionOptions& conn_opts) {
  setTimeout(conn_opts.getTimeout());
  setDefaultQueryTimeout(conn_opts.getQueryTimeout());
  setAttributes(conn_opts.getAttributes());
  setConnectAttempts(conn_opts.getConnectAttempts());
  if (conn_opts.getDscp().has_value()) {
    setDscp(*conn_opts.getDscp());
  }
  setTotalTimeout(conn_opts.getTotalTimeout());
  setCompression(conn_opts.getCompression());
  if (conn_opts.getConnectTcpTimeout()) {
    setTcpTimeout(*conn_opts.getConnectTcpTimeout());
  }
  if (conn_opts.getSniServerName()) {
    setSniServerName(*conn_opts.getSniServerName());
  }
  if (auto provider = conn_opts.getSSLOptionsProvider()) {
    setSSLOptionsProvider(std::move(provider));
  }
  if (conn_opts.getCertValidationCallback()) {
    setCertValidationCallback(conn_opts.getCertValidationCallback());
  }
  if (auto cats = conn_opts.getCryptoAuthTokenList()) {
    setCryptoAuthTokenList(*std::move(cats));
  }
  if (auto expiration = conn_opts.getCryptoAuthTokenExpiration()) {
    conn_options_.setCryptoAuthTokenExpiration(*expiration);
  }
}

const ConnectionOptions& ConnectOperationImpl::getConnectionOptions() const {
  return conn_options_;
}

void ConnectOperationImpl::setDefaultQueryTimeout(Duration t) {
  CHECK_THROW(
      state() == OperationState::Unstarted, db::OperationStateException);
  conn_options_.setQueryTimeout(t);
}

void ConnectOperationImpl::setSniServerName(const std::string& sni_servername) {
  CHECK_THROW(
      state() == OperationState::Unstarted, db::OperationStateException);
  conn_options_.setSniServerName(sni_servername);
}

void ConnectOperationImpl::enableResetConnBeforeClose() {
  conn_options_.enableResetConnBeforeClose();
}

void ConnectOperationImpl::enableDelayedResetConn() {
  conn_options_.enableDelayedResetConn();
}

void ConnectOperationImpl::enableChangeUser() {
  conn_options_.enableChangeUser();
}

void ConnectOperationImpl::setCertValidationCallback(
    CertValidatorCallback callback) {
  CHECK_THROW(
      state() == OperationState::Unstarted, db::OperationStateException);
  conn_options_.setCertValidationCallback(std::move(callback));
}

void ConnectOperationImpl::setTimeout(Duration timeout) {
  conn_options_.setTimeout(timeout);
  OperationBase::setTimeout(timeout);
}

void ConnectOperationImpl::setTcpTimeout(Duration timeout) {
  conn_options_.setConnectTcpTimeout(timeout);
}

void ConnectOperationImpl::setTotalTimeout(Duration total_timeout) {
  conn_options_.setTotalTimeout(total_timeout);
  OperationBase::setTimeout(min(getTimeout(), total_timeout));
}
void ConnectOperationImpl::setConnectAttempts(uint32_t max_attempts) {
  CHECK_THROW(
      state() == OperationState::Unstarted, db::OperationStateException);
  conn_options_.setConnectAttempts(max_attempts);
}

void ConnectOperationImpl::setDscp(uint8_t dscp) {
  CHECK_THROW(
      state() == OperationState::Unstarted, db::OperationStateException);
  conn_options_.setDscp(dscp);
}

void ConnectOperationImpl::setKillOnQueryTimeout(bool killOnQueryTimeout) {
  CHECK_THROW(
      state() == OperationState::Unstarted, db::OperationStateException);
  killOnQueryTimeout_ = killOnQueryTimeout;
}
void ConnectOperationImpl::setSSLOptionsProviderBase(
    std::unique_ptr<SSLOptionsProviderBase> /*ssl_options_provider*/) {
  CHECK_THROW(
      state() == OperationState::Unstarted, db::OperationStateException);
  LOG(ERROR) << "Using deprecated function";
}
void ConnectOperationImpl::setSSLOptionsProvider(
    std::shared_ptr<SSLOptionsProviderBase> ssl_options_provider) {
  CHECK_THROW(
      state() == OperationState::Unstarted, db::OperationStateException);
  conn_options_.setSSLOptionsProvider(ssl_options_provider);
}

ConnectOperation& ConnectOperationImpl::op() const {
  DCHECK(op_);
  return *(boost::polymorphic_downcast<ConnectOperation*>(op_));
}

[[nodiscard]] const InternalConnection&
ConnectOperationImpl::getInternalConnection() const {
  return conn().getInternalConnection();
}

[[nodiscard]] InternalConnection&
ConnectOperationImpl::getInternalConnection() {
  return conn().getInternalConnection();
}

ConnectionHolder* ConnectOperationImpl::mysqlConnection() const {
  return conn().mysqlConnection();
}

[[nodiscard]] std::string ConnectOperationImpl::generateTimeoutError(
    Millis elapsed,
    const std::function<uint16_t(bool)>& getErrorNo,
    std::string apiName,
    std::optional<std::string> location,
    std::optional<std::string> extra) const {
  auto cbDelay = client().callbackDelayAvg();
  bool stalled = (cbDelay >= kCallbackDelayStallThreshold);

  // Overall the message looks like this:
  //   [<errno>](Mysql Client) <API name> to <host>:<port> timed out [in pool]
  //   [at stage <connect_stage>] (took Nms, timeout was Nms)
  //   [(CLIENT_OVERLOADED: cb delay Nms, N active conns)] [TcpTimeout:N]
  std::vector<std::string> parts;
  parts.push_back(
      fmt::format(
          "[{}]({}){} to {}:{} timed out",
          getErrorNo(stalled),
          kErrorPrefix,
          apiName,
          conn_key_->host(),
          conn_key_->port()));
  if (location) {
    parts.push_back(std::move(*location));
  }
  parts.push_back(timeoutMessage(elapsed));
  if (stalled) {
    parts.push_back(threadOverloadMessage(cbDelay));
  }
  if (extra) {
    parts.push_back(std::move(*extra));
  }

  return folly::join(" ", parts);
}

/*static*/
std::shared_ptr<ConnectOperation> ConnectOperation::create(
    std::unique_ptr<ConnectOperationImpl> impl) {
  // We must do this unusual behavior (with `new`) instead of std::make_shared
  // because we don't want the constructor for ConnectOperation to be public.
  // Without a public constructor there is no standard way of allowing
  // std::make_shared to call the constructor - i.e. no way to mark
  // std::make_shared as a friend.  So we have to do this weirdness.
  return std::shared_ptr<ConnectOperation>(
      new ConnectOperation(std::move(impl)));
}

void ConnectOperation::callConnectCallback() {
  if (connect_callback_) {
    connect_callback_(*this);
    // Release callback since no other callbacks will be made
    connect_callback_ = nullptr;
  }
}

} // namespace facebook::common::mysql_client
