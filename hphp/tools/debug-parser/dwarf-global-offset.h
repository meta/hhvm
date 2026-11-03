#pragma once

#include <fmt/format.h>

#include "hphp/util/assertions.h"

namespace debug_parser {

struct GlobalOff {
  GlobalOff(uint64_t off, bool isInfo, bool isDWP) {
    m_value = (off << 2) + (isInfo << 1) + (isDWP ? 1 : 0);
    assert(offset() == off);
  }
  GlobalOff(int64_t off, bool isInfo, bool isDWP) :
      GlobalOff{ static_cast<uint64_t>(off), isInfo, isDWP } {}

  static GlobalOff fromRaw(uint64_t raw) {
    return GlobalOff{raw};
  }
  uint64_t offset() const { return m_value >> 2; }
  bool isInfo() const { return m_value & 2; }
  bool isDWP() const { return m_value & 1; }
  uint64_t raw() const { return m_value; }

  friend GlobalOff operator+(GlobalOff a, size_t b) {
    return {a.offset() + b, a.isInfo(), a.isDWP()};
  }
  friend bool operator<(GlobalOff a, GlobalOff b) {
    // we want all the debug_types to sort before all the debug_infos
    if ((a.m_value ^ b.m_value) & 2) {
      return (a.m_value & 2);
    }
    return a.m_value < b.m_value;
  }
  friend bool operator==(GlobalOff a, GlobalOff b) {
    return a.raw() == b.raw();
  }
  friend bool operator>(GlobalOff a, GlobalOff b) { return b < a; }
  friend bool operator<=(GlobalOff a, GlobalOff b) { return !(a > b); }
  friend bool operator>=(GlobalOff a, GlobalOff b) { return !(a < b); }
  friend bool operator!=(GlobalOff a, GlobalOff b) { return !(a == b); }
  struct Hash {
    size_t hash(GlobalOff a) const {
      return std::hash<uint64_t>{}(a.raw());
    }
    bool equal(GlobalOff a, GlobalOff b) const {
      return a == b;
    }
    size_t operator()(GlobalOff a) const { return hash(a); }
  };
private:
  explicit GlobalOff(uint64_t raw) : m_value{raw} {}
  uint64_t m_value;
};

}

////////////////////////////////////////////////////////////////////////////////

template <>
struct fmt::formatter<debug_parser::GlobalOff>
    : fmt::formatter<std::string_view> {
  auto format(debug_parser::GlobalOff val, fmt::format_context& ctx) const {
    return fmt::formatter<std::string_view>::format(
        fmt::format("{}:{}", val.offset(), val.isInfo() ? 1 : 0), ctx);
  }
};

namespace std {
template<>
struct hash<debug_parser::GlobalOff> : debug_parser::GlobalOff::Hash {};
}
