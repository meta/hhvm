/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#pragma once

#include <cstdint>
#include <limits>
#include <fmt/core.h>

#include "hphp/util/hash.h"
#include "hphp/util/ptr.h"

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

struct Func;

///////////////////////////////////////////////////////////////////////////////

/*
 * Unique identifier for a Func*.
 */
struct FuncId {
  static const FuncId Invalid;
  static const FuncId Dummy;
  using Int = uint32_t;

  using Id = PackedPtr<const Func>;
  Int toInt() const {
    return m_id.getRaw();
  }
  const Func* getFunc() const {
    assertx(!isInvalid() && !isDummy());
    return m_id;
  }
  constexpr static FuncId fromInt(Int num) {
    return FuncId{Id(num, Id::from_raw)};
  }

  bool isInvalid() const { return m_id == Invalid.m_id; }
  bool isDummy()   const { return m_id == Dummy.m_id; }

  size_t hash()    const { return hash_int64(toInt()); }

  bool operator==(const FuncId& id) const {
    return m_id == id.m_id;
  }
  bool operator<(const FuncId& id) const {
    return toInt() < id.toInt();
  }

  Id m_id;
};

inline constexpr FuncId FuncId::Invalid = FuncId::fromInt(std::numeric_limits<FuncId::Int>::max());
inline constexpr FuncId FuncId::Dummy   = FuncId::fromInt(std::numeric_limits<FuncId::Int>::max() - 1);

static_assert(sizeof(FuncId) == sizeof(uint32_t), "");

struct FuncIdHashCompare {
  static size_t hash(const FuncId& id) { return id.hash(); }
  static bool equal(const FuncId& a, const FuncId& b) { return a == b; }
};

///////////////////////////////////////////////////////////////////////////////
}

namespace std {
  template<> struct hash<HPHP::FuncId> {
    size_t operator()(HPHP::FuncId id) const { return id.hash(); }
  };
}

template <>
struct fmt::formatter<HPHP::FuncId> : fmt::formatter<HPHP::FuncId::Int> {
  template <typename FormatContext>
  auto format(HPHP::FuncId id, FormatContext& ctx) const {
    return fmt::formatter<HPHP::FuncId::Int>::format(id.toInt(), ctx);
  }
};
