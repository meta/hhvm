/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "squangle/mysql_client/InternalConnection.h"
#include "squangle/mysql_client/test/MockInternalResult.h"
#include "squangle/mysql_client/test/MockInternalRowMetadata.h"

namespace facebook::common::mysql_client::test {

/**
 * MockQueryResult holds the expected result for a mock query.
 */
struct MockQueryResult {
  std::vector<MockFieldInfo> fields;
  std::vector<std::vector<MockColumnValue>> rows;
  uint64_t lastInsertId = 0;
  uint64_t affectedRows = 0;
  unsigned int warningCount = 0;
  std::optional<std::string> recvGtid;
  std::optional<std::string> info;
};

/**
 * MockConnectionConfig configures the behavior of MockInternalConnection.
 */
struct MockConnectionConfig {
  // Server info string
  std::string serverInfo = "MockMySQLServer 8.0.0";

  // Error state (0 = no error)
  unsigned int errorNumber = 0;
  std::string errorMessage;

  // Transaction state
  bool inTransaction = false;

  // Connection state
  bool isReusable = true;
  bool isSSL = false;
  bool autocommit = true;
  bool needReset = false;

  // Query results indexed by query string (for exact match)
  std::unordered_map<std::string, MockQueryResult> queryResults;

  // Default result for any query not in the map
  std::optional<MockQueryResult> defaultResult;
};

/**
 * MockInternalConnection implements InternalConnection for unit testing.
 *
 * This allows testing Squangle Operation classes without a real MySQL server.
 *
 * Usage:
 *   MockConnectionConfig config;
 *   config.serverInfo = "TestServer";
 *   config.queryResults["SELECT 1"] = MockQueryResult{
 *       .fields = {{"1", "", MYSQL_TYPE_LONG, 0}},
 *       .rows = {{int64_t{1}}},
 *   };
 *
 *   auto conn = std::make_unique<MockInternalConnection>(config);
 *   // Use conn in tests...
 */
class MockInternalConnection : public InternalConnection {
 public:
  explicit MockInternalConnection(MockConnectionConfig config = {})
      : config_(std::move(config)) {}

  // Configuration accessors for test setup
  MockConnectionConfig& config() {
    return config_;
  }
  const MockConnectionConfig& config() const {
    return config_;
  }

  // Error injection for testing error paths
  void setError(unsigned int errno_, std::string message) {
    config_.errorNumber = errno_;
    config_.errorMessage = std::move(message);
  }

  void clearError() {
    config_.errorNumber = 0;
    config_.errorMessage.clear();
  }

  // Query result injection
  void setQueryResult(const std::string& query, MockQueryResult result) {
    config_.queryResults[query] = std::move(result);
  }

  // Get the last query executed (for test verification)
  const std::string& lastQuery() const {
    return lastQuery_;
  }

  // --- InternalConnection interface implementation ---

  void setReusable(bool reusable) override {
    config_.isReusable = reusable;
  }

  bool isReusable() const override {
    return config_.isReusable;
  }

  bool isSSL() const override {
    return config_.isSSL;
  }

  bool inTransaction() const override {
    return config_.inTransaction;
  }

  void setNeedResetBeforeReuse() override {
    config_.needReset = true;
  }

  bool needResetBeforeReuse() override {
    return config_.needReset;
  }

  std::string serverInfo() const override {
    return config_.serverInfo;
  }

  unsigned int warningCount() const override {
    if (currentResult_) {
      return currentResult_->warningCount;
    }
    return 0;
  }

  size_t escapeString(char* out, const char* src, size_t length, char /*quote*/)
      const override {
    // Simple escaping: just copy the string
    // A real implementation would escape special characters
    std::memcpy(out, src, length);
    return length;
  }

  unsigned int getErrno() const override {
    return config_.errorNumber;
  }

  std::string getErrorMessage() const override {
    return config_.errorMessage;
  }

  void setConnectAttributes(const AttributeMap& /*attributes*/) override {
    // No-op for mock
  }

  int setQueryAttributes(const AttributeMap& attributes) override {
    lastQueryAttributes_ = attributes;
    return 0;
  }

  int setQueryAttribute(const std::string& key, const std::string& value)
      override {
    lastQueryAttributes_[key] = value;
    return 0;
  }

  AttributeMap getResponseAttributes() const override {
    return responseAttributes_;
  }

  void setConnectTimeout(Millis /*timeout*/) const override {}
  void setReadTimeout(Millis /*timeout*/) const override {}
  void setWriteTimeout(Millis /*timeout*/) const override {}

  uint64_t getLastInsertId() const override {
    if (currentResult_) {
      return currentResult_->lastInsertId;
    }
    return 0;
  }

  uint64_t getAffectedRows() const override {
    if (currentResult_) {
      return currentResult_->affectedRows;
    }
    return 0;
  }

  std::optional<std::string> getRecvGtid() const override {
    if (currentResult_) {
      return currentResult_->recvGtid;
    }
    return std::nullopt;
  }

  std::optional<std::string> getMySQLInfo() const override {
    if (currentResult_) {
      return currentResult_->info;
    }
    return std::nullopt;
  }

  std::optional<std::string> getSchemaChanged() const override {
    return std::nullopt;
  }

  bool getNoIndexUsed() const override {
    return false;
  }

  bool wasSlow() const override {
    return false;
  }

  bool getAutocommit() const override {
    return config_.autocommit;
  }

  bool ping() const override {
    return config_.errorNumber == 0;
  }

  Status resetConn() const override {
    return config_.errorNumber == 0 ? Status::DONE : Status::ERROR;
  }

  Status changeUser(
      std::shared_ptr<const ConnectionKey> /*conn_key*/) const override {
    return config_.errorNumber == 0 ? Status::DONE : Status::ERROR;
  }

  bool dumpDebugInfo() const override {
    return true;
  }

  // --- Mock-specific query execution ---

  /**
   * Simulates running a query. Looks up the result in queryResults map.
   * Returns the result for test verification.
   */
  const MockQueryResult* runQuery(const std::string& query) {
    lastQuery_ = query;

    // Check for configured error
    if (config_.errorNumber != 0) {
      return nullptr;
    }

    // Look up query result
    auto it = config_.queryResults.find(query);
    if (it != config_.queryResults.end()) {
      currentResult_ = &it->second;
      return currentResult_;
    }

    // Use default result if available
    if (config_.defaultResult) {
      currentResult_ = &*config_.defaultResult;
      return currentResult_;
    }

    return nullptr;
  }

  /**
   * Returns a MockInternalResult for the current query result.
   * Use this to simulate fetching rows.
   */
  std::unique_ptr<MockInternalResult> getResult() {
    if (!currentResult_) {
      return nullptr;
    }
    return std::make_unique<MockInternalResult>(currentResult_->rows);
  }

  /**
   * Returns MockInternalRowMetadata for the current query result.
   */
  std::unique_ptr<MockInternalRowMetadata> getMetadata() {
    if (!currentResult_) {
      return nullptr;
    }
    return std::make_unique<MockInternalRowMetadata>(currentResult_->fields);
  }

  // Test verification helpers
  const AttributeMap& lastQueryAttributes() const {
    return lastQueryAttributes_;
  }

  void setResponseAttributes(AttributeMap attrs) {
    responseAttributes_ = std::move(attrs);
  }

 private:
  MockConnectionConfig config_;
  std::string lastQuery_;
  const MockQueryResult* currentResult_ = nullptr;
  AttributeMap lastQueryAttributes_;
  AttributeMap responseAttributes_;
};

} // namespace facebook::common::mysql_client::test
