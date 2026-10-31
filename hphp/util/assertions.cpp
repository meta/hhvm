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

#include "hphp/util/assertions.h"

#include <fmt/core.h>
#include <fmt/format.h>

#include <cstdio>
#include <string>

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

static bool s_assert_failed{false};

__thread AssertDetailImpl* AssertDetailImpl::s_head = nullptr;

///////////////////////////////////////////////////////////////////////////////

std::pair<std::string,std::string>
AssertDetailImpl::log_one(const AssertDetailImpl* adi, const char* name) {
  auto title = fmt::format("{:-^80}\n", name);
  auto msg = adi->run();

  fprintf(stderr, "\n%s%s\n", title.c_str(), msg.c_str());
  return { std::move(title), std::move(msg) };
}

bool AssertDetailImpl::readAndRemove(std::string& msg) {
  if (!s_head) return false;
  auto const head = s_head;
  auto const name = head->m_name;
  head->m_name = nullptr;
  s_head = head->m_next;

  auto pair = log_one(head, name);
  msg = std::move(pair.first) + std::move(pair.second) + "\n";
  return true;
}

//////////////////////////////////////////////////////////////////////

void assert_fail(const char* e, const char* file,
                 unsigned int line, const char* func,
                 const std::string& msg) {
  // If we re-enter this function it's because we hit a second assertion while
  // processing the first. The second assertion is meaningless, so just
  // short-circuit straight to abort().
  if (s_assert_failed) std::abort();
  s_assert_failed = true;

  auto const assertion = fmt::format("{}:{}: {}: assertion `{}' failed.",
                                        file, line, func, e);

  fprintf(stderr, "\nAssertion failure: %s\n\n%s\n",
          assertion.c_str(), msg.c_str());

  SCOPE_ASSERT_DETAIL("Assertion Failure") { return assertion; };

  if (msg.empty()) std::abort();

  SCOPE_ASSERT_DETAIL("Assertion Message") { return msg; };
  std::abort();
}

///////////////////////////////////////////////////////////////////////////////
}
