/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fmt/core.h>
#include <glog/logging.h>

#include "squangle/base/Base.h"

namespace facebook::common::mysql_client {

std::string_view toString(QueryCallbackReason reason) {
  switch (reason) {
    case QueryCallbackReason::RowsFetched:
      return "RowsFetched";
    case QueryCallbackReason::QueryBoundary:
      return "QueryBoundary";
    case QueryCallbackReason::Failure:
      return "Failure";
    case QueryCallbackReason::Success:
      return "Success";
  }

  LOG(DFATAL) << "unable to convert reason to string: "
              << static_cast<int>(reason);
  return "Unknown reason";
}

// overload of operator<< for QueryCallbackReason
std::ostream& operator<<(std::ostream& os, QueryCallbackReason reason) {
  return os << toString(reason);
}

std::string queryTimeoutAttributeValue(Duration timeout) {
  const auto millis = std::chrono::ceil<Millis>(timeout).count();
  const auto seconds = millis / 1000;
  const auto fraction = millis % 1000;
  if (fraction == 0) {
    return fmt::format("{}", seconds);
  }
  return fmt::format("{}.{:03}", seconds, fraction);
}

} // namespace facebook::common::mysql_client
