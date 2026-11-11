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

#include "hphp/util/hash.h"

#include <folly/lang/Bits.h>
#include <fmt/format.h>

#include <cstdint>

namespace HPHP {

/*
 * BitSet is mostly equivalent to std::bitset, except it allows values to
 * be constexpr and contains a couple of extra useful convenience methods.
 */
template<size_t N>
struct BitSet {
  static constexpr size_t M = (N - 1) / 64 + 1;
  static_assert(N > 0, "BitSet requires a positive number of bits");

  constexpr BitSet() {}

  /*
   * Return a BitSet with a single enabled bit.
   * This is a template to enable static assertions. We don't need to
   * construct new BitSets at runtime anyway.
   */
  template<size_t Bit>
  static constexpr BitSet bit() {
    static_assert(Bit < N, "Desired bit must fit in BitSet capacity");

    constexpr size_t idx = Bit / 64;
    constexpr size_t shift = Bit - (idx * 64);

    BitSet<N> r;
    r.m_data[idx] = 1ULL << shift;

    return r;
  }

  constexpr BitSet<N> operator|(BitSet<N> rhs) const {
    BitSet<N> r;
    for (int i = 0; i < M; i++) {
      r.m_data[i] = m_data[i] | rhs.m_data[i];
    }
    return r;
  }

  constexpr BitSet<N> operator&(BitSet<N> rhs) const {
    BitSet<N> r;
    for (int i = 0; i < M; i++) {
      r.m_data[i] = m_data[i] & rhs.m_data[i];
    }
    return r;
  }

  constexpr BitSet<N> operator~() const {
    BitSet<N> r;
    for (int i = 0; i < M; i++) {
      r.m_data[i] = ~m_data[i];
    }
    r.cleanUpperBits();
    return r;
  }

  constexpr BitSet<N>& operator|=(BitSet<N> rhs) {
    return *this = *this | rhs;
  }

  constexpr BitSet<N>& operator&=(BitSet<N> rhs) {
    return *this = *this & rhs;
  }

  constexpr bool operator==(BitSet<N> rhs) const {
    bool r = true;
    for (int i = 0; i < M; i++) {
      r &= m_data[i] == rhs.m_data[i];
    }
    return r;
  }

  constexpr bool operator!=(BitSet<N> rhs) const {
    bool r = false;
    for (int i = 0; i < M; i++) {
      r |= m_data[i] != rhs.m_data[i];
    }
    return r;
  }

  constexpr BitSet<N> operator<<(size_t shift) const {
    size_t delta = shift / 64;
    shift -= (delta * 64);

    if (delta >= M) {
      return BitSet<N>();
    }

    BitSet<N> r;
    for (int i = M - 1; i > delta; i--) {
      r.m_data[i] = (m_data[i - delta] << shift) |
                    (shift ? m_data[i - delta - 1] >> (64 - shift) : 0);
    }
    r.m_data[delta] = m_data[0] << shift;

    r.cleanUpperBits();
    return r;
  }

  constexpr BitSet<N> operator>>(size_t shift) const {
    size_t delta = shift / 64;
    shift -= (delta * 64);

    if (delta >= M) {
      return BitSet<N>();
    }

    BitSet<N> r;
    for (int i = 0; i < M - delta - 1; i++) {
      r.m_data[i] = (m_data[i + delta] >> shift) |
                    (shift ? m_data[i + delta + 1] << (64 - shift) : 0);
    }
    r.m_data[M - delta - 1] = m_data[M - 1] >> shift;

    return r;
  }

  /*
   * Return the number of 1-bits.
   */
  constexpr size_t count() const {
    size_t n = 0;
    for (int i = 0; i < M; i++) {
      n += folly::popcount(m_data[i]);
    }
    return n;
  }

  /**
   * Return true if no bits are set.
   */
  constexpr bool empty() const {
    size_t n = 0;
    for (int i = 0; i < M; i++) {
      n |= m_data[i];
    }
    return n == 0;
  }

  constexpr explicit operator bool() const { return !empty(); }

  /*
   * Return a 64-bit hash code.
   */
  constexpr size_t hash() const {
    if constexpr (M == 1) {
      return hash_int64(m_data[0]);
    } else {
      size_t h = hash_int64_pair(m_data[0], m_data[1]);
      for (int i = 2; i < M; i++) {
        h = hash_int64_pair(h, m_data[i]);
      }
      return h;
    }
  }

  /*
   * Return a human-readable string representing this BitSet.
   * e.g. "0x0123456789abcdef"
   */
  std::string hexStr() const {
    std::string s = "0x";
    for (int i = M - 1; i >= 0; i--) {
      s += fmt::format("{:016x}", m_data[i]);
    }
    return s;
  }

private:
  /*
   * Zero out the unused, uppermost bits, for use following an operator
   * that may have modified them.
   */
  constexpr void cleanUpperBits() {
    constexpr size_t maskShift = N - (M - 1) * 64;
    static_assert(maskShift > 0 && maskShift <= 64,
                 "mask shift must fit in 1 to 64 range");
    constexpr uint64_t mask = maskShift == 64 ? ~0ULL : (1ULL << maskShift) - 1;
    m_data[M - 1] &= mask;
  }

  // std::array has no constexpr methods, so use a bare array.
  uint64_t m_data[M] = {0};
};

}
