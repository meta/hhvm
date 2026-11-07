/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/container/F14Map.h>
#include <chrono>
#include <functional>
#include <string>

namespace facebook::common::mysql_client {

using Millis = std::chrono::milliseconds;
using Micros = std::chrono::microseconds;
using Duration = std::chrono::duration<uint64_t, std::micro>;
using Timepoint = std::chrono::time_point<std::chrono::steady_clock>;

// The extra StringHasher and std::equal_to<> makes this unordered map
// "transparent", meaning we can .find() by std::string, std::string_view, and
// by const char*. */
using AttributeMap = folly::F14NodeMap<std::string, std::string>;

// Builds response attributes that are too expensive to produce for every query.
// Only invoke it when the attributes will actually be consumed.
using DeferredAttributesFn = std::function<AttributeMap()>;

// For control flows in callbacks. This indicates the reason a callback was
// fired. When a pack of rows if fetched it is used RowsFetched to
// indicate that new rows are available. QueryBoundary means that the
// fetching for current query has completed successfully, and if any
// query failed (OperationResult is Failed) we use Failure. Success is for
// indicating that all queries have been successfully fetched.
enum class QueryCallbackReason { RowsFetched, QueryBoundary, Failure, Success };

// overload of operator<< for QueryCallbackReason
std::ostream& operator<<(std::ostream& os, QueryCallbackReason reason);

// Formats a query timeout as the value of the `query_timeout` attribute that
// proxies honor: seconds, with millisecond precision ("20", "0.200").
// Rounds up to the next millisecond so that a nonzero timeout is never sent
// as "0".
std::string queryTimeoutAttributeValue(Duration timeout);

using LoggingFuncs =
    std::unordered_map<std::string, std::function<std::string()>>;
using LoggingFuncsPtr = std::shared_ptr<LoggingFuncs>;

} // namespace facebook::common::mysql_client
