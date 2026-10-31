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

#include "hphp/runtime/vm/jit/alias-id-set.h"

#include "hphp/util/bitops.h"

#include <folly/Bits.h>
#include <folly/Conv.h>

namespace HPHP::jit {
///////////////////////////////////////////////////////////////////////////

/* implicit */ AliasIdSet::AliasIdSet(uint32_t id) {
  if (id <= BitsetMax) {
    m_bits = Empty | (1ull << id);
  } else {
    m_bits = id;
  }
  assertx(checkInvariants());
}

/* implicit */ AliasIdSet::AliasIdSet(IdRange r) {
  // More than one element, must use bitset mode.
  m_bits = Empty;
  set(r);
  assertx(checkInvariants());
}

bool AliasIdSet::checkInvariants() const {
  if (isBigInteger()) {
    // Must use bitset mode for single integers below BitsetMax.
    return m_bits > BitsetMax && m_bits < Max;
  }
  return true;
}

bool AliasIdSet::hasSingleValue() const {
  if (isBigInteger()) return true;
  if (hasUpperRange()) return false;

  auto const x = m_bits & (m_bits - 1);
  // popcount(x) = popcount(m_bits) - 1.
  return x == Empty;
}

uint32_t AliasIdSet::size() const {
  if (isBigInteger()) return 1;
  if (hasUpperRange()) return Max;

  // MSB is always set in bitset mode; it doesn't count.
  return folly::popcount(m_bits) - 1;
}

uint32_t AliasIdSet::singleValue() const {
  assertx(hasSingleValue());

  if (isBitset()) {
    assertx(m_bits != Empty);

    uint64_t ret;
    DEBUG_ONLY auto const success = ffs64(m_bits, ret);
    assertx(success && ret != 63);
    return ret; // `n' is represented by the nth bit set, for n < 63
  }
  assertx(isBigInteger());
  return m_bits;
}

void AliasIdSet::set(uint32_t id) {
  if (id <= BitsetMax) {
    // The only way to represent a small integer is to use the bitset mode.
    toBitset().m_bits |= 1ull << id;
    return;
  }

  // Convert an empty bitset to big-integer mode.
  if (empty()) {
    m_bits = id;
    return;
  }

  if (m_bits == id) return;     // Already representing the same integer.

  // Convert to bitset mode if needed, and use upper range.
  setUpperRange();
}

void AliasIdSet::set(IdRange range) {
  if (range.empty()) return;
  if (range.hasSingleValue()) return set(range.l);

  // Multiple values, must use a bitset.
  if (range.h > BitsetMax + 1) setUpperRange();
  else toBitset();

  // Set lower bits as needed.
  if (range.l <= BitsetMax) {
    auto const nBits = std::min(BitsetMax + 1u, range.h) - range.l;
    m_bits |= ((1ull << nBits) - 1) << range.l;
  }
}

void AliasIdSet::unset(uint32_t id) {
  if (isBigInteger()) {
    // Empty is represented in bitset mode.
    if (m_bits == id) m_bits = Empty;
    return;
  }
  // cannot safely unset upper range.
  if (id > BitsetMax) return;
  m_bits &= ~(1ull << id);
}

bool AliasIdSet::test(uint32_t id) const {
  if (isBitset()) {
    if (id <= BitsetMax) {
      return m_bits & (1ull << id);
    }
    return hasUpperRange();
  }
  // Big integer.
  return m_bits == id;
}

bool AliasIdSet::maybe(const AliasIdSet other) const {
  if (isBigInteger()) {
    if (m_bits == other.m_bits) return true;
    // If `other' is a big integer, the following will return false.
    return other.hasUpperRange();
  }

  if (other.isBigInteger()) {
    return hasUpperRange();
  }

  // Both are bitsets, including cases when one is empty.
  auto r = m_bits & other.m_bits;
  // Does r have a bit set other than MSB?
  return r & (r - 1);
}

AliasIdSet AliasIdSet::operator|=(const AliasIdSet rhs) {
  if (*this == rhs || rhs.empty()) return *this;
  if (empty()) {
    m_bits = rhs.m_bits;
    return *this;
  }

  if (isBigInteger() || rhs.isBigInteger()) {
    // Result contains a big integer, as well as one other integer, so we
    // must use bitset mode.
    setUpperRange();
  }

  if (rhs.isBitset()) {
    // Both are bitsets.
    m_bits |= rhs.m_bits;
  }

  assertx(checkInvariants());
  return *this;
}

bool AliasIdSet::isSubsetOf(const AliasIdSet rhs) const {
  if (*this == rhs || empty()) return true;

  if (isBigInteger()) {
    // If `rhs' is a big integer, the following will return false. We know
    // they cannot be the same integer.
    return rhs.hasUpperRange();
  }

  // nonempty bitset.
  if (rhs.isBigInteger()) return false;

  // Both are bitsets.
  return !(m_bits & (~rhs.m_bits));
}

std::string AliasIdSet::toString() const {
  assertx(checkInvariants());

  if (isBigInteger()) {
    return folly::to<std::string>(m_bits);
  }
  if (empty()) return "None";
  if (isAny()) return "Any";

  std::string result;

  // Try to print the slots by grouping them, expect output like
  // '0~4,9,10,50~...'
  auto first = true;         // whether to avoid priting the separator
  int32_t begin = -1;        // starting bit of the consecutive range.

  // Append slots [begin, end) to result string.
  auto const appendRange = [&] (uint32_t end) {
    assertx(end > begin);
    result += folly::to<std::string>(begin);
    if (end == begin + 1) return;
    if (end == begin + 2) {
      result += folly::to<std::string>(",", begin + 1);
    } else {
      result += folly::to<std::string>("~", end - 1);
    }
  };

  for (uint32_t i = 0; i <= BitsetMax; ++i) {
    if (test(i)) {
      if (begin < 0) begin = static_cast<int32_t>(i);
      else continue;
    } else {
      if (begin < 0) continue;
      if (!first) result += ",";
      appendRange(i);
      begin = -1;
      first = false;
    }
  }

  if (hasUpperRange()) {
    if (!first) result += ",";
    if (begin < 0) begin = BitsetMax + 1;
    result += folly::to<std::string>(begin, "~...");
  } else if (begin >= 0) {
    // Append [begin, BitsetMax].
    appendRange(BitsetMax + 1);
  }

  return result;
}

///////////////////////////////////////////////////////////////////////////
}
