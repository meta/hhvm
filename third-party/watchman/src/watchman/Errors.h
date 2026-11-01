/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <fmt/format.h>
#include <cstdint>
#include <string>
#include <system_error>

// We may have to deal with errors from various sources and with
// different error namespaces.  This header defines some helpers
// to make it easier for code to reason about and react to those errors.
// http://blog.think-async.com/2010/04/system-error-support-in-c0x-part-5.html
// explains the concepts behind the error_condition API used here.

namespace watchman {

// Various classes of errors that we wish to programmatially respond to.
// This doesn't need to be an exhaustive list of all possible conditions,
// just those that we want to handle in code.
enum class error_code {
  no_such_file_or_directory,
  not_a_directory,
  too_many_symbolic_link_levels,
  permission_denied,
  system_limits_exceeded,
  timed_out,
  not_a_symlink,
  is_a_directory,
  stale_file_handle,
};

// An error category implementation that is present for comparison purposes
// only; do not use this category to create error_codes explicitly.
class error_category : public std::error_category {
 public:
  const char* name() const noexcept override;
  std::string message(int err) const override;
  bool equivalent(const std::error_code& code, int condition)
      const noexcept override;
};

// Obtain a ref to the above error category
const std::error_category& error_category();

// Helper that is used to implicitly construct an error condition
// during equivalence testing.  Don't use this directly.
inline std::error_condition make_error_condition(error_code e) {
  return std::error_condition(static_cast<int>(e), error_category());
}

// Helper that is used to implicitly construct an error code
// during equivalence testing.  Don't use this directly.
inline std::error_code make_error_code(error_code e) {
  return std::error_code(static_cast<int>(e), error_category());
}

// A type representing windows error codes.  These are stored
// in DWORD values and it is not really feasible to enumerate all
// possible values due to the way that windows error codes are
// structured.
// We use uint32_t here to avoid pulling in the windows header file
// here implicitly.
// We use this purely for convenience with the make_error_XXX
// functions below.  While this is only used on Windows, it doesn't
// hurt to define it unconditionally for all platforms here.
enum windows_error_code : uint32_t {};

// Helper that is used to implicitly construct an windows error condition
// during equivalence testing.  This only makes sense to use on
// windows platforms.
inline std::error_condition make_error_condition(windows_error_code e) {
  return std::error_condition(static_cast<int>(e), std::system_category());
}

// Helper that is used to implicitly construct an windows error code
// during equivalence testing.  This only makes sense to use on
// windows platforms.
inline std::error_code make_error_code(windows_error_code e) {
  return std::error_code(static_cast<int>(e), std::system_category());
}

// An error category for explaining inotify specific errors.
// It is effectively the same as generic_category except that
// the messages for some of the codes are different.
class inotify_category : public std::error_category {
 public:
  const char* name() const noexcept override;
  std::string message(int err) const override;
};

// Obtain a ref to the above error category
const std::error_category& inotify_category();

template <typename T>
class WatchmanError : public std::runtime_error {
  struct NoPrefix {};

 public:
  WatchmanError(const char* what)
      : std::runtime_error{
            T::prefix ? fmt::format("{}: {}", T::prefix, what).c_str()
                      : what} {}
  WatchmanError(const std::string& what)
      : std::runtime_error{
            T::prefix ? fmt::format("{}: {}", T::prefix, what).c_str()
                      : what} {}
  WatchmanError(NoPrefix, const std::string& what) : std::runtime_error{what} {}

  using std::runtime_error::runtime_error;

  template <typename... Args>
  [[noreturn]] static void throwf(
      fmt::format_string<Args...> fmt,
      Args&&... args) {
    if constexpr (nullptr != T::prefix) {
      // It would be nice to avoid the double-format here.
      throw T{
          NoPrefix{},
          fmt::format(
              "{}: {}",
              T::prefix,
              fmt::format(fmt, std::forward<Args>(args)...))};
    } else {
      throw T{NoPrefix{}, fmt::format(fmt, std::forward<Args>(args)...)};
    }
  }
};

class CommandValidationError : public WatchmanError<CommandValidationError> {
 public:
  static constexpr const char* prefix = "failed to validate command";
  using WatchmanError::WatchmanError;
};

/**
 * Represents an error parsing a query.
 */
class QueryParseError : public WatchmanError<QueryParseError> {
 public:
  static constexpr const char* prefix = "failed to parse query";
  using WatchmanError::WatchmanError;
};

/**
 * Represents an error executing a query.
 */
class QueryExecError : public WatchmanError<QueryExecError> {
 public:
  static constexpr const char* prefix = "query failed";
  using WatchmanError::WatchmanError;
};

/**
 * Represents an error resolving a root.
 */
class RootResolveError : public WatchmanError<RootResolveError> {
 public:
  static constexpr const char* prefix = "failed to resolve root";
  using WatchmanError::WatchmanError;
};

/**
 * Represents an error when root is not conntected.
 */
class RootNotConnectedError : public WatchmanError<RootNotConnectedError> {
 public:
  static constexpr const char* prefix = "root not connected";
  using WatchmanError::WatchmanError;
};

} // namespace watchman

// Allow watchman::error_code to implicitly convert to std::error_condition
namespace std {
template <>
struct is_error_condition_enum<watchman::error_code> : public true_type {};

// Allow watchman::windows_error_code to implicitly convert to
// std::error_condition
template <>
struct is_error_condition_enum<watchman::windows_error_code>
    : public true_type {};
} // namespace std
