/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once
#include "squangle/base/ConnectionKey.h"
#include "squangle/mysql_client/Operation.h"

namespace facebook::common::mysql_client {

class MysqlExceptionBuilder {
 public:
  virtual ~MysqlExceptionBuilder() = default;

  virtual folly::exception_wrapper buildMysqlException(
      OperationResult result,
      unsigned int mysql_errno,
      const std::string& mysql_error,
      std::shared_ptr<const ConnectionKey> conn_key,
      Duration elapsed_time) const {
    return folly::make_exception_wrapper<MysqlException>(
        result, mysql_errno, mysql_error, std::move(conn_key), elapsed_time);
  }

  virtual folly::exception_wrapper buildQueryException(
      int num_executed_queries,
      OperationResult result,
      unsigned int mysql_errno,
      const std::string& mysql_error,
      std::shared_ptr<const ConnectionKey> conn_key,
      Duration elapsed_time) const {
    return folly::make_exception_wrapper<QueryException>(
        num_executed_queries,
        result,
        mysql_errno,
        mysql_error,
        std::move(conn_key),
        elapsed_time);
  }
};
} // namespace facebook::common::mysql_client
