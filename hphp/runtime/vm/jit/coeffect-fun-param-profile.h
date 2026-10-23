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

#include <fmt/format.h>
#include <cstdint>
#include <string>

#include "hphp/runtime/base/typed-value.h"

#include <folly/json/dynamic.h>

namespace HPHP::jit {
///////////////////////////////////////////////////////////////////////////////

struct CoeffectFunParamProfile {
public:

  enum class OptType {
    Null,
    Closure,
    MethCaller,
    Func,
    ClsMeth
  };

  std::vector<OptType> order() const;
  void update(const TypedValue*);
  static void reduce(CoeffectFunParamProfile& l,
                     const CoeffectFunParamProfile& r);
  std::string toString() const;
  folly::dynamic toDynamic() const;

private:
  uint32_t m_null{0};
  uint32_t m_closure{0};
  uint32_t m_methcaller{0};
  uint32_t m_func{0};
  uint32_t m_clsmeth{0};
  uint32_t m_total{0};
};

///////////////////////////////////////////////////////////////////////////////
}

template <>
struct fmt::formatter<HPHP::jit::CoeffectFunParamProfile> : fmt::formatter<std::string_view> {
  auto format(const HPHP::jit::CoeffectFunParamProfile& v, fmt::format_context& ctx) const {
    return fmt::formatter<std::string_view>::format(v.toString(), ctx);
  }
};
