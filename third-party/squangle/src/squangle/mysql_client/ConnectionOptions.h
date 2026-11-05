/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/ssl/OpenSSLPtrTypes.h>

#include "squangle/base/Base.h"
#include "squangle/mysql_client/Compression.h"

namespace facebook::common::mysql_client {

class Operation;
class SSLOptionsProviderBase;

using CertValidatorCallback = std::function<bool(
    X509* server_cert,
    const std::weak_ptr<Operation>& context,
    folly::StringPiece& errMsg)>;

class ConnectionOptions {
 public:
  ConnectionOptions();

  // Each attempt to acquire a connection will take at maximum this duration.
  // Use setTotalTimeout if you want to limit the timeout for all attempts.
  ConnectionOptions& setTimeout(Duration dur) noexcept {
    connection_timeout_ = dur;
    return *this;
  }

  Duration getTimeout() const noexcept {
    return connection_timeout_;
  }

  // This time out is used for each connect attempt and this is the maximum
  // time allowed for client<->server tcp handshake. This timeout allows client
  // to failfast in case the MYSQL server is not reachable, where we don't want
  // to wait for the entire connection_timeout_ which also includes time buffer
  // for ssl and MYSQL protocol exchange
  ConnectionOptions& setConnectTcpTimeout(Duration dur) noexcept {
    connection_tcp_timeout_ = dur;
    return *this;
  }

  const folly::Optional<Duration>& getConnectTcpTimeout() const noexcept {
    return connection_tcp_timeout_;
  }

  // The connection created by these options will apply this query timeout
  // to all statements executed
  ConnectionOptions& setQueryTimeout(Duration dur) noexcept {
    query_timeout_ = dur;
    return *this;
  }

  Duration getQueryTimeout() const noexcept {
    return query_timeout_;
  }

  // Used to provide an SSLContext and SSL_Session provider
  ConnectionOptions& setSSLOptionsProvider(
      std::shared_ptr<SSLOptionsProviderBase> ssl_options_provider) noexcept {
    ssl_options_provider_ = std::move(ssl_options_provider);
    return *this;
  }

  std::shared_ptr<SSLOptionsProviderBase> getSSLOptionsProvider()
      const noexcept {
    return ssl_options_provider_;
  }

  template <typename Func>
  void withPossibleSSLOptionsProvider(Func&& func) {
    if (ssl_options_provider_) {
      func(*ssl_options_provider_);
    }
  }

  // Provide a Connection Attribute to be passed in the connection handshake
  ConnectionOptions& setAttribute(std::string_view attr, std::string value) {
    attributes_[attr] = std::move(value);
    return *this;
  }

  // MySQL 5.6 connection attributes.  Sent at time of connect.
  const AttributeMap& getAttributes() const noexcept {
    return attributes_;
  }

  ConnectionOptions& setAttributes(const AttributeMap& attributes) {
    for (auto& [key, value] : attributes) {
      attributes_[key] = value;
    }
    return *this;
  }

  // Sorry for the weird API, there is no enum for compression = None
  ConnectionOptions& setCompression(
      folly::Optional<CompressionAlgorithm> comp_lib) noexcept {
    compression_lib_ = std::move(comp_lib);
    return *this;
  }

  const folly::Optional<CompressionAlgorithm>& getCompression() const noexcept {
    return compression_lib_;
  }

  ConnectionOptions& setUseChecksum(bool useChecksum) noexcept {
    use_checksum_ = useChecksum;
    return *this;
  }

  [[nodiscard]] bool getUseChecksum() const noexcept {
    return use_checksum_;
  }

  // Sets the amount of attempts that will be tried in order to acquire the
  // connection. Each attempt will take at maximum the given timeout. To set
  // a global timeout that the operation shouldn't take more than, use
  // setTotalTimeout.
  //
  // This is no longer recommended for use, due to higher level retries
  ConnectionOptions& setConnectAttempts(uint32_t max_attempts) noexcept {
    max_attempts_ = max_attempts;
    return *this;
  }

  uint32_t getConnectAttempts() const noexcept {
    return max_attempts_;
  }

  ConnectionOptions& setDscp(uint8_t dscp);

  folly::Optional<uint8_t> getDscp() const noexcept {
    return dscp_;
  }

  // If this is not set, but regular timeout was, the TotalTimeout for the
  // operation will be the number of attempts times the primary timeout.
  // Set this if you have strict timeout needs.
  //
  // This should generally not be set, as connectAttempts is 1
  ConnectionOptions& setTotalTimeout(Duration dur) noexcept {
    total_timeout_ = dur;
    return *this;
  }

  Duration getTotalTimeout() const noexcept {
    return total_timeout_;
  }

  std::string getDisplayString() const;

  ConnectionOptions& setSniServerName(const std::string& sniName) noexcept {
    sni_servername_ = sniName;
    return *this;
  }

  const folly::Optional<std::string>& getSniServerName() const noexcept {
    return sni_servername_;
  }

  ConnectionOptions& enableResetConnBeforeClose() noexcept {
    reset_conn_before_close_ = true;
    return *this;
  }

  bool isEnableResetConnBeforeClose() const noexcept {
    return reset_conn_before_close_;
  }

  ConnectionOptions& enableDelayedResetConn() noexcept {
    delayed_reset_conn_ = true;
    return *this;
  }

  bool isEnableDelayedResetConn() const noexcept {
    return delayed_reset_conn_;
  }

  ConnectionOptions& enableChangeUser() noexcept {
    change_user_ = true;
    return *this;
  }

  bool isEnableChangeUser() const noexcept {
    return change_user_;
  }

  ConnectionOptions& setCertValidationCallback(
      CertValidatorCallback callback) noexcept;

  const CertValidatorCallback& getCertValidationCallback() const noexcept {
    return certValidationCallback_;
  }

  ConnectionOptions& setCryptoAuthTokenList(const std::string& tokenList) {
    crypt_auth_token_list_ = tokenList;
    return *this;
  }

  const folly::Optional<std::string>& getCryptoAuthTokenList() const {
    return crypt_auth_token_list_;
  }

 private:
  Duration connection_timeout_;
  folly::Optional<Duration> connection_tcp_timeout_;
  Duration total_timeout_;
  Duration query_timeout_;
  std::shared_ptr<SSLOptionsProviderBase> ssl_options_provider_;
  AttributeMap attributes_;
  folly::Optional<CompressionAlgorithm> compression_lib_;
  bool use_checksum_ = false;
  uint32_t max_attempts_ = 1;
  folly::Optional<uint8_t> dscp_;
  folly::Optional<std::string> sni_servername_;
  folly::Optional<std::string> crypt_auth_token_list_;
  bool reset_conn_before_close_ = false;
  bool delayed_reset_conn_ = false;
  bool change_user_ = false;
  CertValidatorCallback certValidationCallback_{nullptr};
};

} // namespace facebook::common::mysql_client
