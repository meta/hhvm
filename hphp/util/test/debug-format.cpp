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

#include "hphp/util/debug-format.h"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include <fmt/format.h>
#include <folly/Range.h>
#include <gtest/gtest.h>

#include "hphp/util/ptr-impl.h"

// The expected strings are what folly::format() printed for the same
// arguments.

namespace HPHP {

namespace {

struct WithToString {
  std::string toString() const { return "toString"; }
};

struct WithFormatter {
  std::string toString() const { return "toString"; }
};

struct WithHook {};

std::string_view hphpDebugFormatArg(DebugFormatTag<const WithHook*>,
                                    const WithHook*) {
  return "hooked";
}

std::string address(const void* p) {
  return fmt::format("0x{:x}", reinterpret_cast<uintptr_t>(p));
}

}

}

template <>
struct fmt::formatter<HPHP::WithFormatter> : fmt::formatter<std::string_view> {
  auto format(const HPHP::WithFormatter&, fmt::format_context& ctx) const {
    return fmt::formatter<std::string_view>::format("formatter", ctx);
  }
};

namespace HPHP {

TEST(DebugFormat, Numbers) {
  EXPECT_EQ("42 -7 ff 0x2a 101", debugFormat("{} {} {:x} {:#x} {:b}",
                                             42, -7, 255, 42, 5));
  EXPECT_EQ("   42|-0000042", debugFormat("{:>5}|{:08}", 42, -42));
  EXPECT_EQ("65 65", debugFormat("{} {}", uint8_t{65}, int8_t{65}));
  EXPECT_EQ("a 97", debugFormat("{} {:d}", 'a', 'a'));
  EXPECT_EQ("true false 1", debugFormat("{} {} {:d}", true, false, true));
  EXPECT_EQ("1.5 0.3333333333333333 1e+16 0.33",
            debugFormat("{} {} {} {:.2f}", 1.5, 1.0 / 3, 1e16, 1.0 / 3));
}

TEST(DebugFormat, FloatPrintsAsDouble) {
  EXPECT_EQ("0.10000000149011612 0.100", debugFormat("{} {:.3f}", 0.1f, 0.1f));
}

TEST(DebugFormat, Strings) {
  const char* cstr = "cstr";
  char* mutableCstr = const_cast<char*>("mutable");
  std::string str = "str";
  folly::StringPiece piece = "piece";
  EXPECT_EQ("cstr mutable str piece lit",
            debugFormat("{} {} {} {} {}", cstr, mutableCstr, str, piece, "lit"));
  EXPECT_EQ("str   |   str| str  |st",
            debugFormat("{:6}|{:>6}|{:^6}|{:.2}", str, str, str, str));
}

TEST(DebugFormat, NullPointers) {
  const char* nullCstr = nullptr;
  int* nullInt = nullptr;
  void* nullVoid = nullptr;
  EXPECT_EQ("(null) (null) (null) (null)",
            debugFormat("{} {} {} {}", nullCstr, nullInt, nullVoid, nullptr));
  EXPECT_EQ("(null)  |", debugFormat("{:8}|", nullInt));
}

TEST(DebugFormat, PointersPrintAddresses) {
  int x = 0;
  int* p = &x;
  const int* cp = &x;
  void* vp = &x;
  auto const addr = address(&x);
  EXPECT_EQ(addr + " " + addr + " " + addr,
            debugFormat("{} {} {}", p, cp, vp));
  EXPECT_EQ(fmt::format("{:<20}|{:>20}|", addr, addr),
            debugFormat("{:20}|{:>20}|", p, p));
}

TEST(DebugFormat, PtrImplPrintsLikeRawPointer) {
  int x = 0;
  ptrimpl::PtrImpl<int, ptrimpl::Normal, ptrimpl::UInt64> p{&x};
  ptrimpl::PtrImpl<int, ptrimpl::Normal, ptrimpl::UInt64> null{nullptr};
  EXPECT_EQ(address(&x) + " (null)", debugFormat("{} {}", p, null));
}

TEST(DebugFormat, ToString) {
  WithToString t;
  EXPECT_EQ("toString|  toString|toString  |",
            debugFormat("{}|{:>10}|{:10}|", t, t, WithToString{}));
}

TEST(DebugFormat, FormatterBeatsToString) {
  EXPECT_EQ("formatter", debugFormat("{}", WithFormatter{}));
}

TEST(DebugFormat, Hook) {
  WithHook h;
  const WithHook* p = &h;
  EXPECT_EQ("hooked", debugFormat("{}", p));
}

TEST(DebugFormat, GroupDigits) {
  EXPECT_EQ("0 999 1,000 -1,000 1,234,567 -123,456",
            fmt::format("{} {} {} {} {} {}", groupDigits(0), groupDigits(999),
                        groupDigits(1000), groupDigits(-1000),
                        groupDigits(1234567), groupDigits(int64_t{-123456})));
  EXPECT_EQ("-9,223,372,036,854,775,808",
            groupDigits(std::numeric_limits<int64_t>::min()));
  EXPECT_EQ("18,446,744,073,709,551,615",
            groupDigits(std::numeric_limits<uint64_t>::max()));
}

}
