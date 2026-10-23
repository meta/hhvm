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

#include "hphp/runtime/vm/jit/call-target-profile.h"

#include "hphp/runtime/vm/func.h"

#include "hphp/util/trace.h"

#include <cstring>
#include <sstream>
#include <fmt/format.h>

namespace HPHP::jit {

TRACE_SET_MOD(irlower)

///////////////////////////////////////////////////////////////////////////////

const size_t CallTargetProfile::kMaxEntries;

void CallTargetProfile::init() {
  if (m_init) return;
  for (size_t i = 0; i < kMaxEntries; i++) {
    m_entries[i] = Entry{};
  }
  m_init = true;
}

void CallTargetProfile::report(const Func* func) {
  assertx(func);
  auto const funcId = func->getFuncId();
  FTRACE(5, "CallTargetProfile::report: funcId {} ({})\n", funcId,
         func->fullName());
  init();
  size_t i = 0;
  for (; i < kMaxEntries; i++) {
    auto& entry = m_entries[i];
    if (entry.funcId.isInvalid()) {
      entry.funcId = funcId;
      entry.count = 1;
      return;
    }
    if (entry.funcId == funcId) {
      entry.count++;
      return;
    }
  }
  m_untracked++;
}

void CallTargetProfile::reduce(CallTargetProfile& profile,
                               const CallTargetProfile& other) {
  Entry allEntries[kMaxEntries * 2];
  size_t nEntries = 0;

  if (profile.m_init) {
    // Copy into `allEntries' all the valid entries from `profile'.
    for (size_t i = 0; i < kMaxEntries; i++) {
      auto const& entry = profile.m_entries[i];
      if (entry.funcId.isInvalid()) break;
      if (entry.funcId.toInt() == 0) continue;
      allEntries[nEntries++] = entry;
    }
  }

  if (other.m_init) {
    // Handle the entries from `other', either by adding to matching entries
    // from `profile' or by adding new entries to `allEntries'.
    for (size_t o = 0; o < kMaxEntries; o++) {
      auto const& otherEntry = other.m_entries[o];
      if (otherEntry.funcId.isInvalid()) break;
      if (otherEntry.funcId.toInt() == 0) continue;
      size_t p = 0;
      for (; p < kMaxEntries; p++) {
        auto& entry = allEntries[p];
        if (entry.funcId == otherEntry.funcId) {
          entry.count += otherEntry.count;
          break;
        }
      }
      if (p == kMaxEntries) {
        // Didn't find `otherEntry' among the entries from `profile', so add a
        // new entry.
        allEntries[nEntries++] = otherEntry;
      }
    }
  }

  profile.m_init |= other.m_init;

  if (!profile.m_init) return;

  // Sort the entries in `allEntries' and select the top kMaxEntries.  The rest
  // is put in m_untracked.
  std::sort(allEntries, allEntries + nEntries,
            [&] (const Entry& a, const Entry& b) {
              // Sort in decreasing order of `count' while keeping invalid
              // entries at the end.
              if (b.funcId.isInvalid()) return !a.funcId.isInvalid();
              if (a.funcId.isInvalid()) return false;
              return a.count > b.count;
            });
  auto const nEntriesToCopy = std::min(kMaxEntries, nEntries);
  memcpy(profile.m_entries, allEntries, nEntriesToCopy * sizeof(Entry));

  // Update `profile's untracked count.
  profile.m_untracked += other.m_untracked;
  for (size_t i = kMaxEntries; i < nEntries; i++) {
    auto const& entry = allEntries[i];
    if (entry.funcId.isInvalid()) break;
    profile.m_untracked += entry.count;
  }
}

jit::vector<CallTargetProfile::Choice>
CallTargetProfile::choose() const {
  jit::vector<Choice> choices;
  if (!m_init) return choices;

  assertx(!m_entries[0].funcId.isInvalid());

  FTRACE(3, "CallTargetProfile::choose(): {}\n", *this);

  uint64_t total = m_untracked;

  for (size_t i = 0; i < kMaxEntries ; i++) {
    auto const& entry = m_entries[i];
    if (entry.funcId.isInvalid()) continue;
    total += entry.count;
    choices.push_back({Func::fromFuncId(entry.funcId), double(entry.count)});
  }

  std::sort(choices.begin(), choices.end(),
            [&] (const Choice& c1, const Choice& c2) {
              return c1.probability > c2.probability;
            });

  FTRACE(2, "CallTargetProfile::choose():\n");
  for (size_t i = 0; i < choices.size(); i++) {
    choices[i].probability /= total;
    FTRACE(2, "  - funcId = {} ({}): probability = {:.3f}\n",
           choices[i].func->getFuncId(), choices[i].func->fullName(),
           choices[i].probability);
  }

  return choices;
}

std::string CallTargetProfile::toString() const {
  if (!m_init) return std::string("uninitialized");
  std::ostringstream out;
  uint64_t total = m_untracked;
  for (auto const& entry : m_entries) {
    if (!entry.funcId.isInvalid()) {
      total += entry.count;
    }
  }
  for (auto const& entry : m_entries) {
    if (!entry.funcId.isInvalid()) {
      out << fmt::format("FuncId {}: {} ({:.1f}%), ",
                           entry.funcId, entry.count,
                           total == 0 ? 0 : 100.0 * entry.count / total);
    }
  }
  out << fmt::format("Untracked: {} ({:.1f}%)", m_untracked,
                       total == 0 ? 0 : 100.0 * m_untracked / total);
  return out.str();
}

folly::dynamic CallTargetProfile::toDynamic() const {
  using folly::dynamic;

  if (!m_init) return dynamic();

  uint64_t total = m_untracked;
  for (auto const& entry : m_entries) {
    if (!entry.funcId.isInvalid()) {
      total += entry.count;
    }
  }

  dynamic entries = dynamic::array;
  for (auto const& entry : m_entries) {
    if (!entry.funcId.isInvalid()) {
      auto percent = total == 0 ? 0 : 100.0 * entry.count / total;
      entries.push_back(dynamic::object("funcId", entry.funcId.toInt())
                                       ("count", entry.count)
                                       ("percent", percent));
    }
  }

  auto percent = total == 0 ? 0 : 100.0 * m_untracked / total;
  dynamic untracked = dynamic::object("count", m_untracked)
                                     ("percent", percent);

  return dynamic::object("entries", entries)
                        ("untracked", untracked)
                        ("total", total)
                        ("profileType", "CallTargetProfile");
}

///////////////////////////////////////////////////////////////////////////////

}
