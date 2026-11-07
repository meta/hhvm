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

#include "hphp/runtime/base/runtime-error.h"
#include "hphp/runtime/base/static-string-table.h"

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

struct Func;

///////////////////////////////////////////////////////////////////////////////

namespace FSM {

extern const StringData* null_key;

inline int32_t hashImpl(const StringData* sd, uint32_t c1, uint32_t c2) {
  auto const intval = reinterpret_cast<int64_t>(sd);
  auto const hash = hash_int64(intval);
  return (hash >> c1) ^ (hash >> c2);
}

inline int32_t hash(const StringData* sd, uint32_t consts, uint32_t size) {
  assertx(consts);
  auto const c1 = consts & ((1 << size) - 1);
  auto const c2 = consts >> size;
  return hashImpl(sd, c1, c2);
}

} // FSM

template<class V, class E>
NEVER_INLINE
void FixedStringMap<V,E>::clear() {
  if (m_table && m_table != (Elm*)&FSM::null_key + 1) {
    vm_free(m_table - capacity());
  }
  setMaskAndConsts(0);
  m_table = nullptr;
}

template<class V, class E>
NEVER_INLINE
void FixedStringMap<V,E>::setMaskAndConsts(uint32_t mask, uint32_t consts) {
  auto const perfect = consts != 0;
  always_assert(mask >> (perfect ? kMaskSizePerfect : kMaskSize) == 0 &&
                consts >> (kHashConstSize * kNumHashConsts) == 0);
  m_maskAndConst = mask |
                   (consts << (perfect ? kMaskSizePerfect : kMaskSize)) |
                   (perfect << kMaskSize);
}

template<class V, class E>
NEVER_INLINE
void FixedStringMap<V,E>::init(int num, uint32_t numExtraBytes) {
  if (!num && !numExtraBytes) {
    m_table = (Elm*)&FSM::null_key + 1;
    setMaskAndConsts(0, 1); // So that empty table uses perfect hashing
    return;
  }

  static const double kLoadScale = 5;
  int capac = 1;
  while (kLoadScale * num >= (kLoadScale - 1) * capac) {
    capac *= 2;
  }
  TRACE_MOD(Trace::runtime, 4, "FixedStringMap::init: %d -> %d\n", num, capac);
  assertx(!m_table);
  auto const allocSize = capac * sizeof(Elm) + numExtraBytes;
  auto ptr = vm_malloc(allocSize);
  std::memset(ptr, 0, allocSize);
  m_table = (Elm*)ptr + capac;
  assertx(m_table);
  setMaskAndConsts(capac - 1);
}

template<class V, class E>
NEVER_INLINE
void FixedStringMap<V,E>::add(const StringData* sd, const V& v) {
  assertx(sd->isStatic());
  auto const mask = this->mask();
  assertx(folly::isPowTwo(capacity()));

  if (LIKELY(isPerfectHash())) {
    // Perfect hashing
    auto const hash = FSM::hash(sd, hashConsts(), kHashConstSize) & mask;
    Elm* elm = &m_table[-1 - int32_t(hash)];
    elm->sd = sd;
    elm->data = v;
    return;
  }

  Elm* elm = &m_table[-1 - int32_t(sd->hash() & mask)];
  UNUSED unsigned numProbes = 0;
  while (elm->sd) {
    assertx(numProbes++ < mask + 1);
    // Semantics for multiple insertion: new value wins.
    if (elm->sd == sd || sd->same(elm->sd)) break;
    if (UNLIKELY(++elm == m_table)) elm -= mask + 1;
  }
  elm->sd = sd;
  elm->data = v;
}

template<class V, class E>
template<class Iter>
NEVER_INLINE
void FixedStringMap<V,E>::addFrom(Iter begin, Iter end) {
  uint32_t capacity = this->capacity();
  uint32_t mask = this->mask();
  assertx(folly::isPowTwo(capacity));

  boost::dynamic_bitset<> bitset(capacity);
  auto const isPerfectHashFunction = [&](uint32_t consts) {
    bitset.reset();
    for (auto it = begin; it != end; ++it) {
      auto const hash = FSM::hash(it->first, consts, kHashConstSize) & mask;
      if (bitset.test(hash)) return false;
      bitset.set(hash);
    }
    return true;
  };

  // hash(c1, c2) == hash(c2, c1), and c1 == c2 always hashes to 0, so only
  // c1 < c2 is worth trying.
  static_assert(kNumHashConsts == 2);
  constexpr uint32_t kNumShifts = 1u << kHashConstSize;
  constexpr double kNumCandidates = kNumShifts * (kNumShifts - 1) / 2;
  auto const findPerfectHashConsts = [&]() -> uint32_t {
    for (uint32_t c2 = 1; c2 < kNumShifts; ++c2) {
      for (uint32_t c1 = 0; c1 < c2; ++c1) {
        auto const consts = c1 | (c2 << kHashConstSize);
        if (isPerfectHashFunction(consts)) return consts;
      }
    }
    return 0;
  };

  // Odds that one uniformly random hash function puts every key in its own
  // slot.
  auto const perfectOdds = [&] {
    double odds = 1.0;
    uint32_t i = 0;
    for (auto it = begin; it != end && odds > 0; ++it) {
      odds *= 1.0 - double(i++) / capacity;
    }
    return odds;
  };

  // For larger maps no candidate is likely to be perfect, and a failed search
  // tries every one of them.
  constexpr double kMinExpectedPerfect = 0.01;
  if (capacity < (1 << kMaskSizePerfect) &&
      perfectOdds() * kNumCandidates >= kMinExpectedPerfect) {
    if (auto const consts = findPerfectHashConsts()) {
      setMaskAndConsts(mask, consts);
    }
  }

  for (auto it = begin; it != end; ++it) add(it->first, it->second);
}

template<class V, class E>
NEVER_INLINE
V* FixedStringMap<V,E>::find(const StringData* sd) const {
  auto const mask = this->mask();
  assertx(folly::isPowTwo(capacity()));

  if (LIKELY(isPerfectHash())) {
    auto sstr = sd;
    if (!is_static_string(sd)) {
      sstr = lookupStaticString(sd);
      if (UNLIKELY(!sstr)) return nullptr;
    }
    // Perfect hashing
    auto const hash = FSM::hash(sstr, hashConsts(), kHashConstSize) & mask;
    Elm* elm = &m_table[-1 - int32_t(hash)];
    if (elm->sd == sstr) return &elm->data;
    assertx(!elm->sd || !sstr->same(elm->sd));
    return nullptr;
  }

  Elm* elm = &m_table[-1 - int32_t(sd->hash() & mask)];
  UNUSED unsigned numProbes = 0;
  for(;;) {
    assertx(numProbes++ < mask + 1);
    if (UNLIKELY(nullptr == elm->sd)) return nullptr;
    if (elm->sd == sd || sd->same(elm->sd)) return &elm->data;
    if (UNLIKELY(++elm == m_table)) elm -= mask + 1;
  }
}

///////////////////////////////////////////////////////////////////////////////

template<class T, class E>
inline
T& FixedStringToSlotMapBuilder<T,E>::operator[](V idx) {
  assertx(idx >= 0);
  assertx(size_t(idx) < m_list.size());
  return m_list[idx];
}

template<class T, class E>
inline
const T& FixedStringToSlotMapBuilder<T,E>::operator[](V idx) const {
  return (*const_cast<FixedStringToSlotMapBuilder*>(this))[idx];
}

template<class T, class E>
inline
void FixedStringToSlotMapBuilder<T,E>::add(const StringData* name,
                                       const T& t) {
  if (m_list.size() >= size_t(std::numeric_limits<V>::max())) {
    assertx(false && "FixedStringMap::Builder overflowed");
    abort();
  }
  m_map[name] = m_list.size();
  m_list.push_back(t);
}

template<class T, class E>
inline
void FixedStringToSlotMapBuilder<T,E>::addUnnamed(const T& t) {
  m_list.push_back(t);
}

template<class T, class E>
inline
void FixedStringToSlotMapBuilder<T,E>::create(FSMap& map) {
  map.extra() = size();
  map.init(size(), 0);
  if (!size()) return;

  map.addFrom(begin(), end());
}

///////////////////////////////////////////////////////////////////////////////
}
