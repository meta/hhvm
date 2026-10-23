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
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <fmt/format.h>
#include <folly/Range.h>

namespace HPHP {

///////////////////////////////////////////////////////////////////////////////

/*
 * debugFormat() is fmt::format() for debug output (FTRACE, ITRACE,
 * always_assert_flog, ...). The format string is checked at compile time, and
 * each argument is first passed through debugFormatArg(), which prints it the
 * way folly::format() did:
 *
 *  - A null pointer (including nullptr and a null char*) prints "(null)".
 *  - Any other non-char pointer prints its address as "0x<hex>".
 *  - A float prints as the double it promotes to.
 *  - A type with an fmt::formatter uses it.
 *  - Otherwise, a type with a toString() member prints toString().
 *  - Otherwise, a type convertible to folly::StringPiece prints as a string.
 *
 * fmt refuses to format non-void pointers even with a user-provided formatter,
 * so a type T (usually a pointer) can print differently by declaring
 *
 *   auto hphpDebugFormatArg(DebugFormatTag<T>, const T& v);
 *
 * in namespace HPHP. StringData pointers use this to print their contents.
 */
template <typename T> struct DebugFormatTag {};

void hphpDebugFormatArg() = delete;

namespace detail {

template <typename T>
concept HasDebugFormatHook = requires(const T& v) {
  hphpDebugFormatArg(DebugFormatTag<T>{}, v);
};

template <typename T>
concept HasFmtFormatter = std::is_constructible_v<fmt::formatter<T, char>>;

template <typename T>
concept HasToString = requires(const T& v) { v.toString(); };

enum class DebugFormatKind {
  Hook,
  Null,
  CharPointer,
  Pointer,
  Float,
  Formatter,
  ToString,
  StringPiece,
  Other,
};

template <typename D>
constexpr DebugFormatKind debugFormatKind() {
  if constexpr (HasDebugFormatHook<D>) {
    return DebugFormatKind::Hook;
  } else if constexpr (std::is_null_pointer_v<D>) {
    return DebugFormatKind::Null;
  } else if constexpr (std::is_pointer_v<D>) {
    using Pointee = std::remove_cv_t<std::remove_pointer_t<D>>;
    return std::is_same_v<Pointee, char> ? DebugFormatKind::CharPointer
                                         : DebugFormatKind::Pointer;
  } else if constexpr (std::is_same_v<D, float>) {
    return DebugFormatKind::Float;
  } else if constexpr (HasFmtFormatter<D>) {
    return DebugFormatKind::Formatter;
  } else if constexpr (HasToString<D> &&
                       !std::is_convertible_v<D, folly::StringPiece>) {
    return DebugFormatKind::ToString;
  } else if constexpr (std::is_convertible_v<const D&, folly::StringPiece>) {
    return DebugFormatKind::StringPiece;
  } else {
    return DebugFormatKind::Other;
  }
}

template <typename... T>
std::string vDebugFormat(fmt::string_view fmtStr, T&&... args) {
  return fmt::vformat(fmtStr, fmt::make_format_args(args...));
}

}

// The kind is a template parameter so that translation units which see
// different hooks or formatters for a type instantiate different functions.
template <typename T,
          detail::DebugFormatKind Kind =
            detail::debugFormatKind<std::decay_t<T>>()>
decltype(auto) debugFormatArg(T&& v) {
  using D = std::decay_t<T>;
  using K = detail::DebugFormatKind;
  if constexpr (Kind == K::Hook) {
    return hphpDebugFormatArg(DebugFormatTag<D>{}, v);
  } else if constexpr (Kind == K::Null) {
    return std::string_view{"(null)"};
  } else if constexpr (Kind == K::CharPointer) {
    return std::string_view{v ? v : "(null)"};
  } else if constexpr (Kind == K::Pointer) {
    if (!v) return std::string{"(null)"};
    return fmt::format("{:#x}", reinterpret_cast<uintptr_t>(v));
  } else if constexpr (Kind == K::Float) {
    return static_cast<double>(v);
  } else if constexpr (Kind == K::ToString) {
    return v.toString();
  } else if constexpr (Kind == K::StringPiece) {
    auto const sp = folly::StringPiece(v);
    return std::string_view{sp.data(), sp.size()};
  } else {
    return std::forward<T>(v);
  }
}

/*
 * Formats an integer with a comma between each group of three digits, like
 * folly::format's "{:,}". fmt::group_digits() does the same but needs fmt 9,
 * and the OSS build supports fmt 8.
 */
template <typename Int>
std::string groupDigits(Int value) {
  static_assert(std::is_integral_v<Int> && !std::is_same_v<Int, bool>);
  auto const digits = fmt::format("{}", value);
  size_t const first = digits[0] == '-' ? 1 : 0;
  std::string result = digits.substr(0, first);
  for (size_t i = first; i < digits.size(); ++i) {
    result += digits[i];
    auto const remaining = digits.size() - i - 1;
    if (remaining > 0 && remaining % 3 == 0) result += ',';
  }
  return result;
}

template <typename T>
using DebugFormatArgT = decltype(debugFormatArg(std::declval<T>()));

template <typename... Args>
using DebugFormatString = fmt::format_string<DebugFormatArgT<Args>...>;

template <typename... Args>
std::string debugFormat(DebugFormatString<Args...> fmtStr, Args&&... args) {
  return detail::vDebugFormat(fmtStr,
                              debugFormatArg(std::forward<Args>(args))...);
}

///////////////////////////////////////////////////////////////////////////////

}
