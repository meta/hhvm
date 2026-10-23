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

#include "hphp/runtime/vm/jit/target-profile.h"

#include <sstream>
#include <string>

#include <folly/json/dynamic.h>
#include <fmt/format.h>

namespace HPHP::jit {

///////////////////////////////////////////////////////////////////////////////

struct SwitchProfile {
  SwitchProfile() = default;
  SwitchProfile(const SwitchProfile&) = delete;
  SwitchProfile& operator=(const SwitchProfile&) = delete;

  std::string toString() const {
    return "";
  }

  folly::dynamic toDynamic() const {
    return folly::dynamic::object();
  }

  std::string toString(uint32_t size) const {
    auto const nCases = size / sizeof(uint32_t);
    std::string out;
    for (int i = 0; i < nCases; ++i) fmt::format_to(std::back_inserter(out), "{},", cases()[i]);
    return out;
  }

  folly::dynamic toDynamic(uint32_t size) const;

  static void reduce(SwitchProfile& a, const SwitchProfile& b, uint32_t size) {
    auto const nCases = size / sizeof(uint32_t);
    for (uint32_t i = 0; i < nCases; ++i) {
      a.cases()[i] += b.cases()[i];
    }
  }

  static size_t extraSize(int n) {
    return n * sizeof(uint32_t) - sizeof(SwitchProfile);
  }

  uint32_t* cases() {
    auto ptr = static_cast<void*>(this);
    ptr = static_cast<char*>(ptr) + offsetof(SwitchProfile, first_case);
    return static_cast<uint32_t*>(ptr);
  }

  const uint32_t* cases() const {
    return const_cast<SwitchProfile*>(this)->cases();
  }

  uint32_t first_case = 0;

  // In RDS but can't contain pointers to request-allocated data
  TYPE_SCAN_IGNORE_ALL;
};

struct SwitchCaseCount {
  int32_t caseIdx;
  uint32_t count;

  bool operator<(const SwitchCaseCount& b) const { return count > b.count; }
};

/*
 * Collect the data for the given SwitchProfile, and return a vector of case
 * indexes and hit count, sorted in descending order of hit count.
 */
std::vector<SwitchCaseCount> sortedSwitchProfile(
  TargetProfile<SwitchProfile>& profile,
  int32_t nCases
);

///////////////////////////////////////////////////////////////////////////////

}

