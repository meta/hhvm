/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
   +----------------------------------------------------------------------+
   | This source path is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the path LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <folly/json/dynamic.h>
#include "hphp/hack/src/hackc/ffi_bridge/compiler_ffi.rs.h"

namespace HPHP {
namespace Facts {

using TypeKindMask = int;
using hackc::TypeKind;
inline constexpr TypeKindMask kTypeKindAll = static_cast<int>(TypeKind::Class) |
    static_cast<int>(TypeKind::Interface) | static_cast<int>(TypeKind::Enum) |
    static_cast<int>(TypeKind::Trait) | static_cast<int>(TypeKind::TypeAlias);

constexpr std::string_view kTypeKindClass = "class";
constexpr std::string_view kTypeKindInterface = "interface";
constexpr std::string_view kTypeKindEnum = "enum";
constexpr std::string_view kTypeKindTrait = "trait";
constexpr std::string_view kTypeKindTypeAlias = "typeAlias";

using TypeFlagMask = int;
enum class TypeFlag {
  Empty = 0,
  Abstract = 1 << 0,
  Final = 1 << 1,
};
constexpr TypeFlagMask kTypeFlagAll = static_cast<int>(TypeFlag::Empty) |
    static_cast<int>(TypeFlag::Abstract) | static_cast<int>(TypeFlag::Final);

// Two different forms of inheritance.
//
// `trait T { require extends B; }` means that, while T does not extend B
// itself, any class using T must also extend B in order to be sound.
// `trait T { require class B; }` means that trait T can only be used
// by class T
using DeriveKindMask = int;
enum class DeriveKind {
  Extends = 1 << 0,
  RequireExtends = 1 << 1,
  // This should be merged into RequireExtends.
  RequireImplements = 1 << 2,
  RequireClass = 1 << 3,
  RequireThisAs = 1 << 4,
};
constexpr DeriveKindMask kDeriveKindAll =
    static_cast<int>(DeriveKind::Extends) |
    static_cast<int>(DeriveKind::RequireExtends) |
    static_cast<int>(DeriveKind::RequireImplements) |
    static_cast<int>(DeriveKind::RequireClass) |
    static_cast<int>(DeriveKind::RequireThisAs);

using hackc::AttrFacts;
using hackc::FileFacts;
using hackc::FunctionFacts;
using hackc::MethodFacts;
using hackc::ModuleFacts;
using hackc::TypeFacts;

inline bool isAbstract(const hackc::TypeFacts& t) noexcept {
  return (t.flags & static_cast<int>(TypeFlag::Abstract)) != 0;
}

inline bool isFinal(const hackc::TypeFacts& t) noexcept {
  return (t.flags & static_cast<int>(TypeFlag::Final)) != 0;
}

inline bool isEmpty(const FileFacts& f) noexcept {
  return f.types.empty() && f.functions.empty() &&
      f.function_attributes.empty() && f.constants.empty() &&
      f.modules.empty() && f.file_attributes.empty();
}

inline std::string_view as_slice(const rust::String& s) noexcept {
  return {s.data(), s.size()};
}

/**
 * A string from Watchman representing a point in time.
 */
struct Clock {
  /**
   * True iff this represents an initial load, where all files have changed
   * since this point in time
   */
  bool isInitial() const noexcept {
    return m_clock.empty() && m_mergebase.empty();
  }

  bool operator==(const Clock& o) const noexcept {
    return m_clock == o.m_clock && m_mergebase == o.m_mergebase;
  }

  bool operator!=(const Clock& o) const noexcept {
    return !operator==(o);
  }

  /**
   * Represents a timestamp on the local machine.
   */
  std::string m_clock;

  /**
   * Represents a public commit in the repo.
   */
  std::string m_mergebase;
};

} // namespace Facts
} // namespace HPHP

template <>
struct fmt::formatter<HPHP::Facts::Clock> {
  // We don't support any settings within the braces, so nothing to do here.
  constexpr auto parse(format_parse_context& ctx) -> decltype(ctx.begin()) {
    return ctx.end();
  }

  template <typename FormatContext>
  auto format(const HPHP::Facts::Clock& c, FormatContext& ctx) const
      -> decltype(ctx.out()) {
    return fmt::format_to(ctx.out(), "Clock({}, {})", c.m_clock, c.m_mergebase);
  }
};
