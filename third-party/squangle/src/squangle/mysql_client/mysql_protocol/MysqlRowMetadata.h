/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "squangle/mysql_client/InternalConnection.h"
#include "squangle/mysql_client/mysql_protocol/MysqlResult.h"

namespace facebook::common::mysql_client::mysql_protocol {

class MysqlRowMetadata : public InternalRowMetadata {
 public:
  explicit MysqlRowMetadata(const MysqlResult& result)
      : num_fields_(result.numFields()), fields_(result.fields()) {}

  [[nodiscard]] size_t numFields() const noexcept override {
    return num_fields_;
  }

  [[nodiscard]] folly::StringPiece getTableName(size_t index) const override {
    DCHECK_LT(index, num_fields_);
    return folly::StringPiece(
        fields_[index].table, fields_[index].table_length);
  }

  [[nodiscard]] folly::StringPiece getFieldName(size_t index) const override {
    DCHECK_LT(index, num_fields_);
    return folly::StringPiece(fields_[index].name, fields_[index].name_length);
  }

  [[nodiscard]] enum_field_types getFieldType(size_t index) const override {
    DCHECK_LT(index, num_fields_);
    return fields_[index].type;
  }

  [[nodiscard]] uint64_t getFieldFlags(size_t index) const override {
    DCHECK_LT(index, num_fields_);
    return fields_[index].flags;
  }

  [[nodiscard]] std::optional<unsigned int> getFieldCharsetnr(
      size_t index) const override {
    DCHECK_LT(index, num_fields_);
    return fields_[index].charsetnr;
  }

  [[nodiscard]] unsigned int getFieldDecimals(size_t index) const override {
    DCHECK_LT(index, num_fields_);
    return fields_[index].decimals;
  }

 private:
  size_t num_fields_;
  const MYSQL_FIELD* fields_;
};

} // namespace facebook::common::mysql_client::mysql_protocol
