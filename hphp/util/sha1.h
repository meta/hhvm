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
#include <cstdlib>

#include <boost/operators.hpp>

#include <fmt/format.h>

#include <folly/Range.h>
#include <folly/String.h>

#include "hphp/util/assertions.h"
#include "hphp/util/byte-order.h"

namespace HPHP {

//////////////////////////////////////////////////////////////////////

struct SHA1 : private boost::totally_ordered<SHA1> {
  static constexpr size_t kQNumWords = 5;
  static constexpr size_t kQWordLen = sizeof(uint32_t);

  // One byte = two ascii hex characters
  static constexpr size_t kQWordHexLen = 2 * kQWordLen;

  static constexpr size_t kStrLen = kQNumWords * kQWordHexLen;

  std::array<uint32_t, kQNumWords> q;

  SHA1() : q{} {}

  explicit SHA1(const std::array<uint8_t, 20>& sha1) {
    for (auto i = 0; i < kQNumWords; i++) {
      uint32_t u =
        (sha1[i*4] << 24) +
        (sha1[i*4+1] << 16) +
        (sha1[i*4+2] << 8) +
         sha1[i*4+3];
      q.at(i) = u;
    }
  }

  /**
   * Build from a SHA1 hexadecimal string
   */
  explicit SHA1(folly::StringPiece str) {
    assertx(str.size() == kStrLen);

    char buf[kQWordHexLen + 1];
    buf[kQWordHexLen] = '\0';
    for (auto i = 0; i < kQNumWords; i++) {
      memcpy(buf, str.begin() + (i * kQWordHexLen), kQWordHexLen);
      q.at(i) = strtoul(buf, nullptr, 16);
    }
  }

  // Blob is assumed to be in network byte order.
  explicit SHA1(const void* blob, DEBUG_ONLY size_t len) {
    assertx(len == kQNumWords * kQWordLen);

    for (auto i = 0; i < kQNumWords; i++) {
      q.at(i) = ntohl(reinterpret_cast<const uint32_t*>(blob)[i]);
    }
  }

  explicit SHA1(uint64_t x): q{} {
    q.at(kQNumWords - 2) = x >> 32;
    q.at(kQNumWords - 1) = x;
  }

  // Copy out in network byte order.
  void nbo(void* blob) const {
    for (auto i = 0; i < kQNumWords; i++) {
      reinterpret_cast<uint32_t*>(blob)[i] = htonl(q.at(i));
    }
  }

  std::string toString() const {
    std::string ret;
    char sha1nbo[kQNumWords * kQWordLen];
    nbo(sha1nbo);
    folly::hexlify(folly::StringPiece(sha1nbo, sizeof sha1nbo), ret);
    return ret;
  }

  // Convert to a string using an arbitrary base-64 alphabet. This
  // leaves two unused bits (27 base-64 characters is 162 bits), which
  // can be used to encode extra data.
  std::string toStringBase64(unsigned int extra = 0) const {
    assertx(extra < 4); // Only room for two bits

    static const char table[64] = {
      '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
      'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j',
      'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't',
      'u', 'v', 'w', 'x', 'y', 'z', 'A', 'B', 'C', 'D',
      'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N',
      'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X',
      'Y', 'Z', '_', '+'
    };

    std::string ret;
    char sha1nbo[kQNumWords * kQWordLen];
    nbo(sha1nbo);

    // Below logic is specialized for this size:
    static_assert(sizeof(sha1nbo) == 20);
    ret.reserve(27);
    for (size_t i = 0; i < sizeof(sha1nbo)-2; i += 3) {
      ret.push_back(table[(sha1nbo[i] >> 2) & 0x3F]);
      ret.push_back(table[((sha1nbo[i] & 0x03) << 4) |
                          ((sha1nbo[i+1] & 0xF0) >> 4)]);
      ret.push_back(table[((sha1nbo[i+1] & 0x0F) << 2) |
                          ((sha1nbo[i+2] & 0xC0) >> 6)]);
      ret.push_back(table[sha1nbo[i+2] & 0x3F]);
    }
    ret.push_back(table[(sha1nbo[sizeof(sha1nbo)-2] >> 2) & 0x3F]);
    ret.push_back(table[((sha1nbo[sizeof(sha1nbo)-2] & 0x03) << 4) |
                        ((sha1nbo[sizeof(sha1nbo)-1] & 0xF0) >> 4)]);
    ret.push_back(table[((sha1nbo[sizeof(sha1nbo)-1] & 0x0F) << 2) + extra]);
    return ret;
  }

  bool operator==(const SHA1& r) const {
    for (auto i = 0; i < kQNumWords; i++) {
      if (q.at(i) != r.q.at(i)) {
        return false;
      }
    }
    return true;
  }

  bool operator<(const SHA1& r) const {
    for (auto i = 0; i < kQNumWords; i++) {
      if (q.at(i) < r.q.at(i)) {
        return true;
      }

      if (q.at(i) > r.q.at(i)) {
        return false;
      }
    }
    return false;
  }

  uint64_t hash() const {
    // All the bits here are fantastically good.
    return (uint64_t(q[0]) << 32) + q[1];
  }

  template <typename SerDe> void serde(SerDe& serde) {
    // Using ZigZag actually results in more bytes when encoding, so
    // prefer direct encoding.
    if constexpr (SerDe::deserializing) {
      serde.readRaw((char*)q.data(), sizeof(q));
    } else {
      serde.writeRaw((const char*)q.data(), sizeof(q));
    }
  }
};

//////////////////////////////////////////////////////////////////////

}

//////////////////////////////////////////////////////////////////////

template <>
struct fmt::formatter<HPHP::SHA1> : fmt::formatter<std::string_view> {
  auto format(const HPHP::SHA1& sha, fmt::format_context& ctx) const {
    return fmt::formatter<std::string_view>::format(sha.toString(), ctx);
  }
};

namespace std {
template<>
struct hash<HPHP::SHA1> {
  size_t operator()(const HPHP::SHA1& sha1) const { return sha1.hash(); }
};
}

//////////////////////////////////////////////////////////////////////
