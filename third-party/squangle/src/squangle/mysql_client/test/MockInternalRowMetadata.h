/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "squangle/mysql_client/InternalConnection.h"
#include "squangle/mysql_client/Row.h"

namespace facebook::common::mysql_client::test {

/**
 * MockFieldInfo describes a single field/column in a mock result set.
 */
struct MockFieldInfo {
  std::string name;
  std::string tableName;
  enum_field_types type = MYSQL_TYPE_STRING;
  uint64_t flags = 0;
  // `std::nullopt` mocks a protocol that carries no charset.
  std::optional<unsigned int> charsetnr = kBinaryCharsetnr;
  unsigned int decimals = kDecimalsNotSpecified;
};

/**
 * MockInternalRowMetadata implements InternalRowMetadata for unit testing.
 *
 * Usage:
 *   std::vector<MockFieldInfo> fields = {
 *       {"id", "users", MYSQL_TYPE_LONG, 0},
 *       {"name", "users", MYSQL_TYPE_STRING, 0},
 *   };
 *   auto metadata = std::make_unique<MockInternalRowMetadata>(fields);
 *   EXPECT_EQ(metadata->numFields(), 2);
 *   EXPECT_EQ(metadata->getFieldName(0), "id");
 */
class MockInternalRowMetadata : public InternalRowMetadata {
 public:
  explicit MockInternalRowMetadata(std::vector<MockFieldInfo> fields)
      : fields_(std::move(fields)) {}

  size_t numFields() const override {
    return fields_.size();
  }

  folly::StringPiece getTableName(size_t field) const override {
    if (field >= fields_.size()) {
      return folly::StringPiece();
    }
    return folly::StringPiece(fields_[field].tableName);
  }

  folly::StringPiece getFieldName(size_t field) const override {
    if (field >= fields_.size()) {
      return folly::StringPiece();
    }
    return folly::StringPiece(fields_[field].name);
  }

  enum_field_types getFieldType(size_t field) const override {
    if (field >= fields_.size()) {
      return MYSQL_TYPE_NULL;
    }
    return fields_[field].type;
  }

  uint64_t getFieldFlags(size_t field) const override {
    if (field >= fields_.size()) {
      return 0;
    }
    return fields_[field].flags;
  }

  std::optional<unsigned int> getFieldCharsetnr(size_t field) const override {
    if (field >= fields_.size()) {
      return std::nullopt;
    }
    return fields_[field].charsetnr;
  }

  unsigned int getFieldDecimals(size_t field) const override {
    if (field >= fields_.size()) {
      return kDecimalsNotSpecified;
    }
    return fields_[field].decimals;
  }

 private:
  std::vector<MockFieldInfo> fields_;
};

} // namespace facebook::common::mysql_client::test
