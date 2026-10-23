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
#include "hphp/runtime/vm/jit/extra-data.h"

#include <folly/json/dynamic.h>

#include <cstdint>

namespace HPHP {

struct ArrayData;
struct StringData;

namespace jit {

struct DecRefProfile;

///////////////////////////////////////////////////////////////////////////////

/*
 * Target profile used to optimize vanilla dict and keyset hash table lookups.
 * This profile can be used for two types of optimizations:
 *
 *  - Offset profiling: If the array used at a certain source location always
 *    has the same "shape" (the same keys, inserted in the same order), then a
 *    given string will always be at the same offset. We can check it directly.
 *
 *  - Size profiling: If the array used at a certain source location tends to
 *    be small, we might want to do the lookup by scanning instead of hashing.
 *
 *  - Empty profiling: If the array is empty, any lookup will fail. In this
 *    case, we first check the size of the array.
 *
 *  - Missing element profiling: If the array used at a certain source location
 *    with the given key tends to not have the given key, we first check
 *    whether this key exists in the array by a fast side table lookup.
 */
struct ArrayAccessProfile {
  enum class Action { None, Cold, Exit };
  /*
   * The result of both profiling types. Prefer the offset to the size hint.
   */
  struct Result {
    std::pair<Action, uint32_t> offset;
    SizeHintData size_hint;
    Action empty;
    Action missing;
    Action nocow;
    std::string toString() const;
  };

  /*
   * Returns profiling results based on questionable heuristics.
   */
  Result choose() const;

  /*
   * Update the profile to register an access at `key' in `ad'.
   */
  void update(const ArrayData* ad, int64_t key, DecRefProfile*);
  void update(const ArrayData* ad, const StringData* key, DecRefProfile*);

  /*
   * Combine `l' and `r', retaining the kNumTrackedSamples with the highest
   * counts.
   */
  static void reduce(ArrayAccessProfile& l,
                     const ArrayAccessProfile& r);

  std::string toString() const;
  folly::dynamic toDynamic() const;

private:
  /*
   * Initialize the samples.
   *
   * The default `pos' values need to be -1, but since this is stored in RDS,
   * we need to manually initialize.
   */
  void init();

  /*
   * Update the profile for `pos' by `count'.
   *
   * Returns whether or not we were able to account for the update precisely.
   */
  bool update(int32_t pos, uint32_t count);

private:
  struct Line {
    int32_t pos{-1};
    uint32_t count{0};
  };

  static const size_t kNumTrackedSamples = 4;

private:
  Line m_hits[kNumTrackedSamples];
  uint32_t m_untracked{0};
  uint32_t m_small{0};
  uint32_t m_empty{0};
  uint32_t m_missing{0};
  uint32_t m_nocow{0};
  // Keep the raw profile representation free of padding.
  uint32_t m_init{0};
};

///////////////////////////////////////////////////////////////////////////////

}}

template <>
struct fmt::formatter<HPHP::jit::ArrayAccessProfile> : fmt::formatter<std::string_view> {
  auto format(const HPHP::jit::ArrayAccessProfile& v, fmt::format_context& ctx) const {
    return fmt::formatter<std::string_view>::format(v.toString(), ctx);
  }
};

template <>
struct fmt::formatter<HPHP::jit::ArrayAccessProfile::Result> : fmt::formatter<std::string_view> {
  auto format(const HPHP::jit::ArrayAccessProfile::Result& v, fmt::format_context& ctx) const {
    return fmt::formatter<std::string_view>::format(v.toString(), ctx);
  }
};
