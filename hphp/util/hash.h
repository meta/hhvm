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

#include <array>
#include <bit>
#include <cstring>
#include <memory>
#include <random>
#include <stdint.h>
#include <string_view>

#include "hphp/util/portability.h"

#ifndef HHVM_FACEBOOK
#  include "hphp/util/hphp-config.h"
#endif

#if defined(__x86_64__)
#  include <nmmintrin.h>
#  if (!defined USE_HWCRC)
#    define USE_HWCRC
#  endif
#elif defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
#  if !defined(USE_HWCRC)
#    define USE_HWCRC
#    define AARCH64_HASH_HELPER
#    include <arm_acle.h>
#  endif
#else
#  undef USE_HWCRC
#endif

// Killswitch
#if NO_HWCRC
#  undef USE_HWCRC
#endif

namespace HPHP {

bool IsHWHashSupported();

///////////////////////////////////////////////////////////////////////////////

using strhash_t = int32_t;
using inthash_t = int32_t;
constexpr strhash_t STRHASH_MASK = 0x7fffffff;
constexpr strhash_t STRHASH_MSB  = 0x80000000;

#if defined(USE_HWCRC) && defined(__ARM_FEATURE_CRC32)

ALWAYS_INLINE uint32_t crc32c_u64_helper(uint32_t a, uint64_t b) {
  return __crc32cd(a, b);
}

#endif

inline size_t hash_int64_fallback(uint64_t key) {
  // "64 bit Mix Functions", from Thomas Wang's "Integer Hash Function."
  // http://www.concentric.net/~ttwang/tech/inthash.htm
  key = (~key) + (key << 21); // key = (key << 21) - key - 1;
  key = key ^ (key >> 24);
  key = (key + (key << 3)) + (key << 8); // key * 265
  key = key ^ (key >> 14);
  key = (key + (key << 2)) + (key << 4); // key * 21
  key = key ^ (key >> 28);
  return static_cast<size_t>(static_cast<uint32_t>(key));
}

ALWAYS_INLINE size_t hash_int64(uint64_t k) {
#if defined(USE_HWCRC) && defined(__SSE4_2__)
  return _mm_crc32_u64(0, k);
#elif defined(USE_HWCRC) && defined(__ARM_FEATURE_CRC32)
  return crc32c_u64_helper(0, k);
#else
  return hash_int64_fallback(k);
#endif
}


inline size_t hash_int64_pair(uint64_t k1, uint64_t k2) {
#if defined(USE_HWCRC) && defined(__SSE4_2__)
  // crc32 is commutative, so we need to perturb k1 so that (k1, k2) hashes
  // differently from (k2, k1).
  k1 += k1;
  return _mm_crc32_u64(k1, k2);
#elif defined(USE_HWCRC) && defined(__ARM_FEATURE_CRC32)
  k1 += k1;
  return crc32c_u64_helper(k2, k1);
#else
  return (hash_int64(k1) << 1) ^ hash_int64(k2);
#endif

}

namespace MurmurHash3 {
///////////////////////////////////////////////////////////////////////////////
// The following code is based on MurmurHash3:
//   http://code.google.com/p/smhasher/wiki/MurmurHash3
//
// The case-insensitive version converts lowercase characters to uppercase
// under the assumption that character data are 7-bit ASCII. This should work
// as identifiers usually only contain alphanumeric characters and the
// underscore. Although PHP allows higher ASCII characters (> 127) in an
// identifier, they should be very rare, and do not change the correctness.

#define ROTL64(x,y) rotl64(x,y)
#define BIG_CONSTANT(x) (x##LLU)

ALWAYS_INLINE uint64_t rotl64(uint64_t x, int8_t r) {
  return (x << r) | (x >> (64 - r));
}

template <bool caseSensitive>
ALWAYS_INLINE uint64_t getblock64(const uint8_t *p) {
  uint64_t block;
  // Read 64 bits without violating alias rules. Compiler will optimize the
  // `memcpy` away.
  std::memcpy(&block, p, sizeof(block));
  if (!caseSensitive) {
    block &= 0xdfdfdfdfdfdfdfdfLLU; // a-z => A-Z
  }
  return block;
}

template <bool caseSensitive>
ALWAYS_INLINE uint8_t getblock8(const uint8_t *p, int i) {
  uint8_t block = p[i];
  if (!caseSensitive) {
    block &= 0xdfU; // a-z => A-Z
  }
  return block;
}

//-----------------------------------------------------------------------------
// Finalization mix - force all bits of a hash block to avalanche
ALWAYS_INLINE uint64_t fmix64(uint64_t k) {
  k ^= k >> 33;
  k *= BIG_CONSTANT(0xff51afd7ed558ccd);
  k ^= k >> 33;
  k *= BIG_CONSTANT(0xc4ceb9fe1a85ec53);
  k ^= k >> 33;
  return k;
}

// Optimized for 64-bit architectures.  MurmurHash3 also implements a 128-bit
// hash that is optimized for 32-bit architectures (omitted here).
template <bool caseSensitive>
ALWAYS_INLINE void hash128(const void *key, size_t len, uint64_t seed,
                           uint64_t out[2]) {
  const uint8_t *data = (const uint8_t *)key;
  const size_t nblocks = len / 16;
  uint64_t h1 = seed;
  uint64_t h2 = seed;
  const uint64_t c1 = BIG_CONSTANT(0x87c37b91114253d5);
  const uint64_t c2 = BIG_CONSTANT(0x4cf5ad432745937f);

  //----------
  // body
  const uint8_t * head = data;
  const uint8_t * tail = data + nblocks*16;
  for(const uint8_t *b = head; b < tail; b += 16)
  {
    uint64_t k1 = getblock64<caseSensitive>(b);
    uint64_t k2 = getblock64<caseSensitive>(b + 8);
    k1 *= c1; k1  = ROTL64(k1,31); k1 *= c2; h1 ^= k1;
    h1 = ROTL64(h1,27); h1 += h2; h1 = h1*5+0x52dce729;
    k2 *= c2; k2  = ROTL64(k2,33); k2 *= c1; h2 ^= k2;
    h2 = ROTL64(h2,31); h2 += h1; h2 = h2*5+0x38495ab5;
  }

  //----------
  // tail
  uint64_t k1 = 0;
  uint64_t k2 = 0;
  switch(len & 15)
  {
  case 15: k2 ^= uint64_t(getblock8<caseSensitive>(tail, 14)) << 48; [[fallthrough]];
  case 14: k2 ^= uint64_t(getblock8<caseSensitive>(tail, 13)) << 40; [[fallthrough]];
  case 13: k2 ^= uint64_t(getblock8<caseSensitive>(tail, 12)) << 32; [[fallthrough]];
  case 12: k2 ^= uint64_t(getblock8<caseSensitive>(tail, 11)) << 24; [[fallthrough]];
  case 11: k2 ^= uint64_t(getblock8<caseSensitive>(tail, 10)) << 16; [[fallthrough]];
  case 10: k2 ^= uint64_t(getblock8<caseSensitive>(tail,  9)) << 8;  [[fallthrough]];
  case  9: k2 ^= uint64_t(getblock8<caseSensitive>(tail,  8)) << 0;
           k2 *= c2; k2  = ROTL64(k2,33); k2 *= c1; h2 ^= k2;
           [[fallthrough]];

  case  8: k1 ^= uint64_t(getblock8<caseSensitive>(tail,  7)) << 56; [[fallthrough]];
  case  7: k1 ^= uint64_t(getblock8<caseSensitive>(tail,  6)) << 48; [[fallthrough]];
  case  6: k1 ^= uint64_t(getblock8<caseSensitive>(tail,  5)) << 40; [[fallthrough]];
  case  5: k1 ^= uint64_t(getblock8<caseSensitive>(tail,  4)) << 32; [[fallthrough]];
  case  4: k1 ^= uint64_t(getblock8<caseSensitive>(tail,  3)) << 24; [[fallthrough]];
  case  3: k1 ^= uint64_t(getblock8<caseSensitive>(tail,  2)) << 16; [[fallthrough]];
  case  2: k1 ^= uint64_t(getblock8<caseSensitive>(tail,  1)) << 8;  [[fallthrough]];
  case  1: k1 ^= uint64_t(getblock8<caseSensitive>(tail,  0)) << 0;
           k1 *= c1; k1  = ROTL64(k1,31); k1 *= c2; h1 ^= k1;
  }

  //----------
  // finalization
  h1 ^= len; h2 ^= len;
  h1 += h2;
  h2 += h1;
  h1 = fmix64(h1);
  h2 = fmix64(h2);
  h1 += h2;
  h2 += h1;

  ((uint64_t*)out)[0] = h1;
  ((uint64_t*)out)[1] = h2;
}

#undef ROTL64
#undef BIG_CONSTANT
///////////////////////////////////////////////////////////////////////////////
} // namespace MurmurHash3

#ifdef AARCH64_HASH_HELPER

template <bool unsafe, bool caseSensitive> 
ALWAYS_INLINE strhash_t aarch64_hash_helper(const char *buf, uint32_t len){
  constexpr uint64_t caseSensitiveMask = caseSensitive ? 0xffffffffffffffffull : 0xdfdfdfdfdfdfdfdfull;
  uint32_t crc0 = ~0;
  if (len >= 16) {
    /* Main loop. */
    do {
      auto const x = *(const uint64_t*)buf;
      auto const y = *(const uint64_t*)(buf + 8);
      crc0 = crc32c_u64_helper(crc0, x & caseSensitiveMask);
      crc0 = crc32c_u64_helper(crc0, y & caseSensitiveMask);
      buf += 16;
      len -= 16;
    } while (len >= 16);
  }
  if (len >= 8) {
    auto const x = *(const uint64_t*)buf;
    crc0 = crc32c_u64_helper(crc0, x & caseSensitiveMask);
    buf += 8;
    len -= 8;
  }
  if (len > 0) {
    static_assert(std::endian::native == std::endian::little);
    uint64_t x;
    auto shift = (8 - len) << 3;
    if constexpr (unsafe) {
      x = *(const uint64_t*)buf;
      x <<= shift;
    } else {
      x = *buf++;
      x <<= shift;
      for (; len > 1; --len) {
        shift += 8;
        uint64_t y = *buf++;
        y <<= shift;
        x |= y;
      }
    }
    crc0 = crc32c_u64_helper(crc0, x & caseSensitiveMask);
  }
  return crc0 >> 1;
}

#endif

// Four functions for hashing: hash_string_(cs|i)(_unsafe)?.
//   cs: case-sensitive;
//   i: case-insensitive;
//   unsafe: safe for strings aligned at 8-byte boundary;

#if defined USE_HWCRC && (defined(__SSE4_2__) || defined(__ARM_FEATURE_CRC32))

// We will surely use CRC32, these are implemented directly in hash-crc-*.S
strhash_t hash_string_cs_unsafe(const char *arKey, uint32_t nKeyLength);
strhash_t hash_string_i_unsafe(const char *arKey, uint32_t nKeyLength);
strhash_t hash_string_cs(const char *arKey, uint32_t nKeyLength);
strhash_t hash_string_i(const char *arKey, uint32_t nKeyLength);
#else

strhash_t hash_string_cs_fallback(const char*, uint32_t);
strhash_t hash_string_i_fallback(const char*, uint32_t);

// We may need to do CPUID checks in the fallback versions, only when we are not
// sure CRC hash is used.
inline strhash_t hash_string_cs(const char *arKey, uint32_t nKeyLength) {
  return hash_string_cs_fallback(arKey, nKeyLength);
}

inline
strhash_t hash_string_cs_unsafe(const char *arKey, uint32_t nKeyLength) {
  return hash_string_cs_fallback(arKey, nKeyLength);
}

inline strhash_t hash_string_i(const char *arKey, uint32_t nKeyLength) {
  return hash_string_i_fallback(arKey, nKeyLength);
}

inline
strhash_t hash_string_i_unsafe(const char *arKey, uint32_t nKeyLength) {
  return hash_string_i_fallback(arKey, nKeyLength);
}

#endif

// Convenience wrapper for std::string_view
inline strhash_t hash_string_cs(std::string_view s) {
  return hash_string_cs(s.data(), s.size());
}

// These functions implement hashing in software. And will return the same thing
// between different hardware
strhash_t hash_string_cs_software(const char*, uint32_t);
strhash_t hash_string_i_software(const char*, uint32_t);

// This function returns true and sets the res parameter if arKey
// is a non-empty string that matches one of the following conditions:
//   1) The string is "0".
//   2) The string starts with a non-zero digit, followed by at most
//      18 more digits, and is less than or equal to 2^63 - 1.
//   3) The string starts with a negative sign, followed by a non-zero
//      digit, followed by at most 18 more digits, and is greater than
//      or equal to -2^63.
inline bool is_strictly_integer(const char* arKey, size_t nKeyLength,
                                int64_t& res) {
  if ((unsigned char)(arKey[0] - '-') > ('9' - '-'))
    return false;
  if (nKeyLength <= 19 ||
      (arKey[0] == '-' && nKeyLength == 20)) {
    unsigned long long num = 0;
    bool neg = false;
    unsigned i = 0;
    if (arKey[0] == '-') {
      neg = true;
      i = 1;
      // The string "-" is NOT strictly an integer
      if (nKeyLength == 1)
        return false;
      // A string that starts with "-0" is NOT strictly an integer
      if (arKey[1] == '0')
        return false;
    } else if (arKey[0] == '0') {
      // The string "0" is strictly an integer
      if (nKeyLength == 1) {
        res = 0;
        return true;
      }
      // A string that starts with "0" followed by at least one digit
      // is NOT strictly an integer
      return false;
    }
    bool good = true;
    for (; i < nKeyLength; ++i) {
      if (arKey[i] >= '0' && arKey[i] <= '9') {
        num = 10*num + (arKey[i] - '0');
      }
      else {
        good = false;
        break;
      }
    }
    if (good) {
      if (num <= 0x7FFFFFFFFFFFFFFFULL ||
          (neg && num == 0x8000000000000000ULL)) {
        res = neg ? 0 - num : (long long)num;
        return true;
      }
    }
  }
  return false;
}

struct StringData;

///////////////////////////////////////////////////////////////////////////////

// Map an integer into one of N buckets. The mapping is calculated
// such that increasing or decreasing the number of buckets causes
// minimal disturbance to the mapping. (Most keys will continue to map
// to the same bucket they did previously). Salt can be used to
// calculate different mappings for the same set of keys.
size_t consistent_hash(int64_t key, size_t buckets, int64_t salt = 0);

///////////////////////////////////////////////////////////////////////////////

/**
 * Tabulation hashing implementation with optional twisted tabulation extension.
 *
 * Tabulation hashing is a simple and efficient hashing technique that
 * provides strong theoretical guarantees, including k-independence
 * for certain values of k.  It works by splitting the input into
 * blocks, using each block as an index into a randomly-initialized
 * lookup table, and XORing the results together.
 *
 * When D > 0, this implements "twisted" or "mixed" tabulation
 * hashing, which provides even stronger guarantees by introducing
 * additional derived blocks that are hashed through a second layer of
 * tables.
 *
 * Template parameters:
 *   I: Input type (must be unsigned integral). The value to be hashed.
 *   O: Output type (defaults to I). Can be an integral type or std::array
 *      of integral types for wider hash outputs.
 *   B: Block type (defaults to uint8_t). The input is split into blocks of
 *      this size. Smaller blocks use less memory but may be slower.
 *   D: Number of expanded/derived blocks (defaults to 0). When D > 0, enables
 *      twisted tabulation hashing for stronger theoretical properties.
 *
 * Example usage:
 *   std::mt19937_64 rng(seed);
 *   TabulationHash<uint64_t> hash(rng);
 *   uint64_t result = hash(12345);
 */
template <typename I, typename O = I, typename B = uint8_t, size_t D = 0>
struct TabulationHash {
  using Input = I;
  using Output = O;
  using Block = B;

  static_assert(std::is_integral_v<Input> && !std::is_signed_v<Input>);
  static_assert(std::is_integral_v<Block> && !std::is_signed_v<Block>);
  static_assert(std::numeric_limits<Input>::digits %
                std::numeric_limits<Block>::digits == 0);

  /**
   * Construct a tabulation hash function with random tables.
   *
   * @param r A random number generator (e.g., std::mt19937_64) used to
   *          initialize the lookup tables. Different generators produce
   *          different hash functions.
   */
  template <typename R> explicit TabulationHash(R&&);

  /**
   * Hash an input value.
   *
   * This operation is thread-safe and can be called concurrently from
   * multiple threads on the same hash instance.
   *
   * @param v The value to hash (must be of type Input).
   * @return The hash value of type Output.
   */
  Output operator()(Input v) const noexcept;

private:

  // Helper to XOR two arrays element-wise (for std::array Output types).
  template <typename T, size_t N>
  static void exclusive_or(std::array<T, N>& a, const std::array<T, N>& b) {
    static_assert(std::is_integral_v<T> && !std::is_signed_v<T>);
    for (size_t i = 0; i < N; ++i) a[i] ^= b[i];
  }
  // Helper to XOR two scalar values (for integral Output types).
  template <typename T> static void exclusive_or(T& a, const T& b) {
    static_assert(std::is_integral_v<T> && !std::is_signed_v<T>);
    a ^= b;
  }

  // Wrapper for std::uniform_int_distribution to generate random values
  // of type T (either integral types or std::array).
  template <typename T>
  struct Distribution {
    template <typename R> T operator()(R&& r) { return d(r); }
    std::uniform_int_distribution<T> d;
  };

  // Specialization for generating random std::array values.
  template <typename T, size_t N>
  struct Distribution<std::array<T, N>> {
    template <typename R>
    std::array<T, N> operator()(R&& r) {
      std::array<T, N> out;
      for (size_t i = 0; i < N; ++i) out[i] = d(r);
      return out;
    }
    std::uniform_int_distribution<T> d;
  };

  // Number of blocks the input is split into (e.g., 8 for uint64_t with uint8_t blocks).
  static constexpr size_t kNumInputBlocks =
    std::numeric_limits<Input>::digits / std::numeric_limits<Block>::digits;
  // Number of expanded/derived blocks for twisted tabulation (0 for simple tabulation).
  static constexpr size_t kNumExpandedBlocks = D;
  // Size of each lookup table dimension (e.g., 256 for uint8_t blocks).
  static constexpr size_t kBlockSize =
    size_t{std::numeric_limits<Block>::max()} + 1;

  // Type of lookup table mapping Block -> Output.
  using ToOutput = std::array<Output, kBlockSize>;

  // Type representing expanded/derived blocks for twisted tabulation.
  using Expanded = std::array<Block, kNumExpandedBlocks>;
  // Type of lookup table mapping Block -> Expanded.
  using ToExpanded = std::array<Expanded, kBlockSize>;

  // Internal state containing all lookup tables.
  struct State {
    // table1: Maps each input block directly to an Output value.
    // One table per input block position.
    std::array<ToOutput, kNumInputBlocks> table1{};
    // table2: Maps each input block to derived/expanded blocks.
    // Only used when D > 0 (twisted tabulation).
    std::array<ToExpanded, kNumInputBlocks> table2{};
    // table3: Maps each expanded block to an Output value.
    // Only used when D > 0 (twisted tabulation).
    std::array<ToOutput, kNumExpandedBlocks> table3{};
  };
  std::shared_ptr<State> state;
};

template <typename I, typename O, typename B, size_t D>
template <typename R>
TabulationHash<I, O, B, D>::TabulationHash(R&& r) {
  Distribution<Output> distrib1;
  std::uniform_int_distribution<Block> distrib2;

  state = std::make_shared<State>();

  // Initialize table1: Direct input block -> output mappings.
  // For each position in the input, create a random lookup table.
  for (auto& t1 : state->table1) {
    for (auto& t2 : t1) t2 = distrib1(r);
  }
  // Initialize table2: Input block -> expanded block mappings.
  // Only used for twisted tabulation (when D > 0).
  for (auto& t1 : state->table2) {
    for (auto& t2 : t1) {
      for (auto& t3 : t2) t3 = distrib2(r);
    }
  }
  // Initialize table3: Expanded block -> output mappings.
  // Only used for twisted tabulation (when D > 0).
  for (auto& t1 : state->table3) {
    for (auto& t2 : t1) t2 = distrib1(r);
  }
}

template <typename I, typename O, typename B, size_t D>
TabulationHash<I, O, B, D>::Output
TabulationHash<I, O, B, D>::operator()(Input v) const noexcept {
  Output out{};
  Expanded expanded{};

  auto const& table1 = state->table1;
  auto const& table2 = state->table2;
  // Phase 1: Process each block of the input value.
  // For each block position i, extract the i-th block from v and use it
  // to look up values in table1[i] and table2[i], XORing the results.
  for (size_t i = 0; i < kNumInputBlocks; ++i) {
    // Use the current low block as an index into table1 and XOR its output.
    exclusive_or(out, table1[i][static_cast<Block>(v)]);
    // Use the current low block to generate expanded blocks (for twisted tabulation).
    auto const& t = table2[i][static_cast<Block>(v)];
    for (size_t j = 0; j < kNumExpandedBlocks; ++j) expanded[j] ^= t[j];
    // Shift to the next block.
    v >>= std::numeric_limits<Block>::digits;
  }

  // Phase 2: Process the expanded/derived blocks (twisted tabulation).
  // Only executes when D > 0. Each expanded block is used as an index
  // into table3 to produce additional output that is XORed into the result.
  auto const& table3 = state->table3;
  for (size_t i = 0; i < kNumExpandedBlocks; ++i) {
    exclusive_or(out, table3[i][expanded[i]]);
  }
  return out;
}

///////////////////////////////////////////////////////////////////////////////

}

#if defined(USE_HWCRC) && !defined(__SSE4_2__)
// The following functions are implemented in ASM directly for x86_64 and ARM
extern "C" {
  HPHP::strhash_t hash_string_cs_crc(const char*, uint32_t);
  HPHP::strhash_t hash_string_i_crc(const char*, uint32_t);
  HPHP::strhash_t hash_string_cs_unaligned_crc(const char*, uint32_t);
  HPHP::strhash_t hash_string_i_unaligned_crc(const char*, uint32_t);
}
#endif
