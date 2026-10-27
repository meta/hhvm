/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "squangle/mysql_client/InternalConnection.h"

namespace facebook::common::mysql_client {

std::string InternalConnection::escapeString(
    std::string_view unescaped,
    char quote) const {
  std::string escaped;
  escaped.resize((2 * unescaped.size()) + 1);
  auto size =
      escapeString(escaped.data(), unescaped.data(), unescaped.size(), quote);
  escaped.resize(size);
  return escaped;
}

} // namespace facebook::common::mysql_client
