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

#include "hphp/util/assertions.h"

#include <string>

namespace HPHP::jit {
///////////////////////////////////////////////////////////////////////////

/*
 * Use 64 bits to represent a set of non-negative integer IDs less than
 * `Max', encoded in one of the following modes.
 *
 * (1) 'Bitset' mode, when MSB (63th bit) is set.
 *
 *     It uses the lower 62 bits as a bitset, where each bit indicates the
 *     presence of the corresponding number. Bit 62 indicates the presence
 *     of 'the upper range', i.e., [62, Max).
 *
 *     Special cases in this mode:
 *         `Any'   when all bits are set.
 *         `Empty' when only MSB is set.
 *
 * (2) 'Big-integer' mode, when MSB is not set.
 *
 *     It uses the lower 32 bits to represent a single integer in the range
 *     of [62, Max).
 *
 *     Note that for a single integer below 62, we always use bitset
 *     mode, to make sure there is a single unique representation. The
 *     implementation depends on this invariant.
 *
 * This is designed to be used in alias classes. In most cases, a set that
 * cannot be precisely represented will be enlarged (as indicated by the
 * presence of the upper range).  However, we guarantee precise representation
 * of any single ID, and any subset of of IDs below 62. Hopefully, these cover
 * most practical cases.
 */
struct AliasIdSet {
  static constexpr auto Any = ~0ull;
  static constexpr auto Empty = 1ull << 63;
  static constexpr auto UpperRange = 1ull << 62; // indicating [62, Max)
  static constexpr auto Max = std::numeric_limits<uint32_t>::max();
  static constexpr auto BitsetMax = 61u; // any subset of [0, BitsetMax]

  /*
   * We can construct the set directly from 0 or 1 integer, or a range of
   * integers.
   */
  AliasIdSet() : m_bits(Empty) {}
  /* implicit */ AliasIdSet(uint32_t id);

  /*
   * A range of unsigned integers [l, h).
   */
  struct IdRange {
    static constexpr auto Max = AliasIdSet::Max;

    explicit IdRange(uint32_t a, uint32_t b = Max)
      : l(a)
      , h(std::max(a, b))
    {
      assertx(l <= h);
    }

    uint32_t size() const       { return h == Max ? Max : h - l; }
    bool empty() const          { return l == h; }
    bool hasSingleValue() const { return h - l == 1; }

    uint32_t l;
    uint32_t h;
  };

  /* implicit */ AliasIdSet(IdRange r);

  /*
   * Return a representable superset or subset of the requested range.
   * Use atMostIdRange for must-kill effects, which must not include extra IDs.
   */
  static AliasIdSet atLeastIdRange(IdRange range);
  static AliasIdSet atMostIdRange(IdRange range);

  /*
   * A fancier constructor that initializes the set from an arbitrary
   * number of integers and ranges, e.g.,
   *
   *   `AliasIdSet { 0, IdRange { 3, x }, y, IdRange { 12 } }'.
   */
  template<typename... Args>
  explicit AliasIdSet(Args... args) : m_bits(Empty) {
    set(args...);
  }

  AliasIdSet(const AliasIdSet&) = default;
  AliasIdSet& operator=(const AliasIdSet&) = default;

  /*
   * Check if the set is in a specific mode.
   */
  bool isBitset() const     { return m_bits & Empty; }
  bool isBigInteger() const { return !isBitset(); }

  /*
   * @returns: the number of elements in the set, `Max' when the set is
   * unbounded.
   *
   * Note: If you want to see whether the set has exactly 0 or 1 element,
   * use `empty()' or `hasSingleValue()' instead. If you want to see if the
   * set is unbounded, use `hasUpperRange()' instead. Those methods are
   * slightly cheaper.
   */
  uint32_t size() const;

  /*
   * Check if `size()' is 0/1/Max.
   */
  bool empty() const           { return m_bits == Empty; }
  bool hasSingleValue() const;
  bool hasUpperRange() const   { return m_bits & UpperRange; }

  bool isAny() const           { return m_bits == Any; }

  /*
   * Return the single value represented by the set.
   *
   * @requires: hasSingleValue()
   */
  uint32_t singleValue() const;

  /*
   * Convert to bitset mode, possibly enlarging the set, and return *this.
   */
  AliasIdSet& toBitset() {
    if (isBigInteger()) {
      m_bits = Empty | UpperRange;
    }
    return *this;
  }

  /*
   * Expose internal data.
   *
   * Note: since every set has a unique representation, this can be used
   * for hashing.
   */
  uint32_t raw() const {
    assertx(checkInvariants());
    return m_bits;
  }

  /*
   * Add an element or a range to the set.
   */
  void set(uint32_t id);
  void set(IdRange range);

  /*
   * Add elements, list of integers, ranges, or mixture of them.
   */
  template<class T, class... Tail> void set(T first, Tail... tail) {
    set(first);
    set(tail...);
  }

  /*
   * Add upper range to the set, possibly enlarging the set.
   */
  void setUpperRange() {
    if (isBigInteger()) {
      m_bits = Empty | UpperRange;
    } else {
      m_bits |= UpperRange;
    }
  }

  /*
   * Unset an element.
   */
  void unset(uint32_t id);

  /*
   * Test if an element is in the set.
   */
  bool test(uint32_t id) const;

  void clear() { m_bits = Empty; }

  /*
   * Does this have nonempty intersection with another set?
   */
  bool maybe(const AliasIdSet other) const;

  friend bool operator==(const AliasIdSet lhs, const AliasIdSet rhs) {
    return lhs.m_bits == rhs.m_bits;
  }

  friend bool operator!=(const AliasIdSet lhs, const AliasIdSet rhs) {
    return !(lhs == rhs);
  }

  /*
   * @returns: whether `lhs' is a subset of `rhs'.
   */
  friend bool operator<=(const AliasIdSet lhs, const AliasIdSet rhs) {
    return lhs.isSubsetOf(rhs);
  }

  /*
   * @returns: the union of `lhs' and `rhs'.
   *
   * Note: result is possibly larger than the real union when it has upper
   * range.  Communitivity is guaranteed.
   */
  AliasIdSet operator|=(const AliasIdSet rhs);

  friend AliasIdSet operator|(const AliasIdSet lhs, const AliasIdSet rhs) {
    auto res = lhs;
    res |= rhs;
    return res;
  }

  friend std::string show(const AliasIdSet ids) { return ids.toString(); }

private:
  bool isSubsetOf(const AliasIdSet rhs) const;
  bool checkInvariants() const;
  std::string toString() const;

private:
  uint64_t m_bits;
};

///////////////////////////////////////////////////////////////////////////////

using IdRange = AliasIdSet::IdRange;

static_assert(sizeof(AliasIdSet) == sizeof(uint64_t), "");

///////////////////////////////////////////////////////////////////////////////
}
