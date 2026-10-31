// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include "hphp/runtime/base/attr.h"

#include <vector>
#include <fmt/format.h>
#include <folly/String.h>

#include "hphp/util/assertions.h"

namespace HPHP {

std::string show(const Attr attrs) {
  if (attrs == AttrNone) return "None";
  std::vector<std::string> parts;
  #define ATTR(name, shift) \
    if (attrs & Attr##name) { \
        parts.push_back(#name); \
    }
    ATTR_BITS
  #undef ATTR
  assertx(!parts.empty());
  if (parts.size() == 1) return parts.front();
  return fmt::format("{{{}}}", folly::join('|', parts));
}

} // namespace HPHP
