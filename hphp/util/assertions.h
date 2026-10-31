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

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <string>
#include <type_traits>
#include <utility>

#include <fmt/core.h>
#include <folly/Preprocessor.h>

#include "hphp/util/debug-format.h"
#include "hphp/util/portability.h"

///////////////////////////////////////////////////////////////////////////////

#define IMPLIES(a, b) (!(a) || (b))

#define not_reached() /* gcc-4.5 supports __builtin_unreachable() */  \
  do {                                                                \
    assertx(false);                                                   \
    __builtin_unreachable();                                          \
  } while (true)

#define not_implemented() do {                   \
  fprintf(stderr, "not implemented: %s:%d %s\n", \
          __FILE__, __LINE__, __FUNCTION__);     \
  always_assert(0);                              \
} while (0)

#define assert_not_implemented(pred) do {        \
  if (! (pred) ) {                               \
    not_implemented();                           \
  }                                              \
} while(0)

namespace HPHP {

///////////////////////////////////////////////////////////////////////////////

/*
 * Assertion abort and log functions.
 *
 * These are intended for use primarily by the assert macros below.
 */
[[noreturn]]
void assert_fail(const char* e,
                 const char* file,
                 unsigned int line,
                 const char* func,
                 const std::string& msg);

///////////////////////////////////////////////////////////////////////////////

/*
 * Stack-allocated detailed assertion logger.
 */
struct AssertDetailImpl {
  /*
   * Reads the most recently added message, and removes it from the
   * list. Returns true if there was a message to read.
   */
  static bool readAndRemove(std::string& msg);
protected:
  explicit AssertDetailImpl(const char* name)
    : m_name(name)
    , m_next(s_head)
  {
#ifndef NDEBUG
    if (m_name == nullptr) std::abort();
#endif
    s_head = this;
  }
  ~AssertDetailImpl() { if (m_name) s_head = m_next; }

  AssertDetailImpl(AssertDetailImpl&& other) noexcept {
#ifndef NDEBUG
    if (s_head != &other) std::abort();
#endif
    m_name = other.m_name;
    m_next = other.m_next;
    s_head = this;
    other.m_name = nullptr; // prevents ~other from messing it up
  }

  AssertDetailImpl(const AssertDetailImpl&) = delete;
  AssertDetailImpl& operator=(const AssertDetailImpl&) = delete;

private:
  static std::pair<std::string,std::string>
  log_one(const AssertDetailImpl* adi, const char* name);
  virtual std::string run() const = 0;

private:
  static __thread AssertDetailImpl* s_head;

  const char* m_name;
  AssertDetailImpl* m_next{nullptr};
};

template<class F>
struct AssertDetailT final : AssertDetailImpl {
  AssertDetailT(const char* name, F&& f)
    : AssertDetailImpl(name)
    , m_f(std::move(f))
  {}
  AssertDetailT(AssertDetailT&&) = default;

private:
  std::string run() const override { return m_f(); }

private:
  F m_f;
};

namespace detail {

struct AssertDetailScopeMaker {
  explicit AssertDetailScopeMaker(const char* name) : m_name(name) {}

  template <typename F>
  AssertDetailT<typename std::decay<F>::type> operator+(F&& f) {
    return AssertDetailT<typename std::decay<F>::type>(
        m_name, std::forward<F>(f));
  }

private:
  const char* m_name;
};

}

#define SCOPE_ASSERT_DETAIL(name)           \
  auto const FB_ANONYMOUS_VARIABLE(SCOPE_ASSERT)  \
  = ::HPHP::detail::AssertDetailScopeMaker(name) + [&]()

///////////////////////////////////////////////////////////////////////////////

# if !defined __GNUC__ || defined __STRICT_ANSI__
#define assert_impl(cond, fail) \
  ((cond) ? static_cast<void>(0) : ((fail), static_cast<void>(0)))
#else
/*
 * This is preferred, because "cond" is not over-parenthesized, and
 * thus -Wparentheses can warn about errors like "always_assert(a=1)".
 * With -pedantic, we cannot use "({...})" statement expressions,
 * so must resort to the old way, above.
 */
#define assert_impl(cond, fail) \
  ({ if (cond) ; else { fail; } static_cast<void>(0); })
#endif

#define assert_fail_impl(e, msg) \
  ::HPHP::assert_fail(#e, __FILE__, __LINE__, __PRETTY_FUNCTION__, msg)

#define always_assert(e)            assert_impl(e, assert_fail_impl(e, ""))
#define always_assert_log(e, l)     assert_impl(e, assert_fail_impl(e, l()))
#define always_assert_flog(e, ...)  assert_impl(e, assert_fail_impl(e,        \
                                        ::HPHP::debugFormat(__VA_ARGS__)))

#undef assert

#ifndef NDEBUG
#define assert(e) always_assert(e)
#define assertx(e) always_assert(e)
#define assert_log(e, l) always_assert_log(e, l)
#define assert_flog(e, ...) always_assert_flog(e, __VA_ARGS__)
#else
#define assert(e) static_cast<void>(0)
#define assertx(e) static_cast<void>(0)
#define assert_log(e, l) static_cast<void>(0)
#define assert_flog(e, ...) static_cast<void>(0)
#endif

///////////////////////////////////////////////////////////////////////////////

}
