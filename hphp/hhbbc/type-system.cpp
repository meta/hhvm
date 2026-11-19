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
#include "hphp/hhbbc/type-system.h"

#include <type_traits>
#include <algorithm>
#include <iterator>
#include <vector>

#include <folly/Traits.h>

#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/base/array-iterator.h"
#include "hphp/runtime/base/repo-auth-type.h"
#include "hphp/runtime/base/tv-comparisons.h"
#include "hphp/runtime/base/type-structure-helpers-defs.h"
#include "hphp/runtime/base/vanilla-vec.h"

#include "hphp/hhbbc/class-util.h"
#include "hphp/hhbbc/context.h"
#include "hphp/hhbbc/eval-cell.h"
#include "hphp/hhbbc/index.h"

#include "hphp/util/check-size.h"
#include "hphp/util/configs/eval.h"

namespace HPHP::HHBBC {

TRACE_SET_MOD(hhbbc)

#define X(y, ...) const Type T##y{B##y};
HHBBC_TYPE_PREDEFINED(X)
#undef X

const StaticString s_Awaitable("HH\\Awaitable");

namespace {

//////////////////////////////////////////////////////////////////////

// When widening a type, allow no specialized information at a nesting depth
// greater than this. This keeps any such types from growing unbounded.
constexpr int kTypeWidenMaxDepth = 8;

//////////////////////////////////////////////////////////////////////

// Each of these bits corresponds to a particular specialization
// type(s). We say these bits "support" the specialization because the
// specialization is only allowed if the bit is present. Right now we
// only allow specialized data if there's only one support bit (there
// can be any non-support bits).
constexpr trep kSupportBits =
  BStr | BDbl | BInt | BCls | BObj | BArrLikeN | BLazyCls | BEnumClassLabel;
// These bits don't correspond to any potential specialized data and
// can be present freely.
constexpr trep kNonSupportBits = BCell & ~kSupportBits;

//////////////////////////////////////////////////////////////////////

bool canSuspendSubtype(bool a, bool b) {
  return !a || b;
}

bool intersectCanSuspend(bool a, bool b) {
  return a && b;
}

//////////////////////////////////////////////////////////////////////

// HHBBC consumes a LOT of memory, and much of it is used by Types.
// We keep the type representation compact; don't expand it accidentally.
static_assert(CheckSize<DCls, 8>(), "");
static_assert(CheckSize<Type, 16>(), "");

//////////////////////////////////////////////////////////////////////

template <typename T, typename... Args>
void construct(T& dest, Args&&... src) {
  new (&dest) T { std::forward<Args>(src)... };
}

template <typename T>
void destroy(T& t) { t.~T(); }

template <typename T, typename... Args>
void construct_inner(T& dest, Args&&... args) {
  construct(dest);
  dest.emplace(std::forward<Args>(args)...);
}

//////////////////////////////////////////////////////////////////////

LegacyMark legacyMarkFromSArr(SArray a) {
  if (a->isKeysetType()) return LegacyMark::Bottom;
  return a->isLegacyArray() ? LegacyMark::Marked : LegacyMark::Unmarked;
}

LegacyMark unionOf(LegacyMark a, LegacyMark b) {
  if (a == b)                  return a;
  if (a == LegacyMark::Bottom) return b;
  if (b == LegacyMark::Bottom) return a;
  return LegacyMark::Unknown;
}

LegacyMark intersectionOf(LegacyMark a, LegacyMark b) {
  if (a == b) return a;
  if (a == LegacyMark::Bottom) return LegacyMark::Bottom;
  if (b == LegacyMark::Bottom) return LegacyMark::Bottom;
  if (a == LegacyMark::Unknown) return b;
  if (b == LegacyMark::Unknown) return a;
  return LegacyMark::Bottom;
}

bool legacyMarkSubtypeOf(LegacyMark a, LegacyMark b) {
  if (a == b) return true;
  if (a == LegacyMark::Bottom) return true;
  return b == LegacyMark::Unknown;
}

bool legacyMarkCouldBe(LegacyMark a, LegacyMark b) {
  if (a == b) return true;
  if (a == LegacyMark::Bottom || b == LegacyMark::Bottom) return false;
  return a == LegacyMark::Unknown || b == LegacyMark::Unknown;
}

LegacyMark project(LegacyMark m, trep b) {
  return couldBe(b, Type::kLegacyMarkBits) ? m : LegacyMark::Bottom;
}

LegacyMark legacyMark(LegacyMark m, trep b) {
  if (!couldBe(b, Type::kLegacyMarkBits)) return LegacyMark::Unmarked;
  assertx(m != LegacyMark::Bottom);
  return m;
}

//////////////////////////////////////////////////////////////////////

// Return true if specializations with the given supports bits must
// contain at least uncounted types. That is, they have to be at least
// "could be" uncounted. If any of the specific array types in the
// trep are entirely static, the specialization must contain uncounted
// values.
bool mustContainUncounted(trep b) {
  if ((b & BVecN)    == BSVecN)    return true;
  if ((b & BDictN)   == BSDictN)   return true;
  if ((b & BKeysetN) == BSKeysetN) return true;
  return false;
}

// Return the allowed key types for the given array type. The first
// type is the upper bound. All keys in the array must be a subtype of
// this. The second type is the lower bound. All keys in the array
// must at least *could be* this. In other words, the keys are allowed
// to be wider than the lower bound, but not any wider than the upper
// bound.
std::pair<trep, trep> allowedKeyBits(trep b) {
  // Ignore anything but the array bits
  b &= BArrLikeN;
  assertx(b != BBottom);

  auto upper = BBottom;
  auto lower = BArrKey;

  if (couldBe(b, BVecN)) {
    // Vecs only ever have int keys
    upper |= BInt;
    lower &= BInt;
    b &= ~BVecN;
  }
  if (couldBe(b, BArrLikeN)) {
    if (subtypeOf(b, BSArrLikeN)) {
      // If the array is entirely static, all keys must be static.
      upper |= BUncArrKey;
      lower &= BUncArrKey;
    } else {
      // Otherwise, the array isn't entirely static, so the upper
      // bound is not UncArrKey. However, the lower bound might be if
      // at least one of the specific array types is entirely static.
      upper |= BArrKey;
      lower &= mustContainUncounted(b) ? BUncArrKey : BArrKey;
    }
  }
  return std::make_pair(upper, lower);
}

// Same as allowedKeyBits, except for the array values. In some cases
// we might know that the array is packed, which further constrains
// the values for a Keyset.
std::pair<trep, trep> allowedValBits(trep b, bool packed) {
  // Ignore anything but the array bits
  b &= BArrLikeN;
  assertx(b != BBottom);

  auto upper = BBottom;
  auto lower = BInitCell;

  if (couldBe(b, BKeysetN)) {
    // For a packed keyset, we know the values must be ints. Otherwise
    // we can infer the staticness of the values if the keyset is
    // static.
    if (packed) {
      upper |= BInt;
      lower &= BInt;
    } else if (subtypeAmong(b, BSKeysetN, BKeysetN)) {
      upper |= BUncArrKey;
      lower |= BUncArrKey;
    } else {
      upper |= BArrKey;
      lower &= BArrKey;
    }
    b &= ~BKeysetN;
  }
  if (couldBe(b, BArrLikeN)) {
    if (subtypeOf(b, BSArrLikeN)) {
      // Same logic as allowedKeyBits()
      upper |= BInitUnc;
      lower &= BInitUnc;
    } else {
      upper |= BInitCell;
      lower &= mustContainUncounted(b) ? BInitUnc : BInitCell;
    }
  }
  return std::make_pair(upper, lower);
}

//////////////////////////////////////////////////////////////////////

/*
 * The following functions make DArr* structs out of static arrays, to
 * simplify implementing some of the type system operations on them.
 *
 * When they return std::nullopt it is not a conservative thing: it
 * implies the array is definitely not packed, packedN, struct-like,
 * etc (we use this to return false in couldBe).
 */

Optional<DArrLikePacked> toDArrLikePacked(SArray ar) {
  assertx(!ar->empty());

  std::vector<Type> elems;
  elems.reserve(ar->size());
  auto idx = size_t{0};
  for (ArrayIter iter(ar); iter; ++iter, ++idx) {
    auto const key = *iter.first().asTypedValue();
    if (key.m_type != KindOfInt64) return std::nullopt;
    if (key.m_data.num != idx)     return std::nullopt;
    elems.emplace_back(from_cell(iter.secondVal()));
  }

  return DArrLikePacked { std::move(elems) };
}

Optional<DArrLikePackedN> toDArrLikePackedN(SArray ar) {
  assertx(!ar->empty());

  auto t = TBottom;
  auto idx = int64_t{0};
  for (ArrayIter iter(ar); iter; ++iter, ++idx) {
    auto const key = *iter.first().asTypedValue();
    if (key.m_type != KindOfInt64) return std::nullopt;
    if (key.m_data.num != idx)     return std::nullopt;
    t |= from_cell(iter.secondVal());
  }

  return DArrLikePackedN { std::move(t) };
}

Optional<DArrLikeMap> toDArrLikeMap(SArray ar) {
  assertx(!ar->empty());
  auto map = MapElems{};
  auto idx = int64_t{0};
  auto packed = true;
  for (ArrayIter iter(ar); iter; ++iter) {
    auto const key = *iter.first().asTypedValue();
    if (packed) {
      packed = isIntType(key.m_type) && key.m_data.num == idx;
      ++idx;
    }
    auto const value = iter.secondVal();
    map.emplace_back(
      key,
      isIntType(key.m_type)
        ? MapElem::IntKey(from_cell(value))
        : MapElem::SStrKey(from_cell(value))
    );
  }
  if (packed) return std::nullopt;

  return DArrLikeMap { std::move(map), TBottom, TBottom };
}

Optional<DArrLikeMapN> toDArrLikeMapN(SArray ar) {
  assertx(!ar->empty());

  auto k = TBottom;
  auto v = TBottom;
  auto idx = int64_t{0};
  auto packed = true;
  for (ArrayIter iter(ar); iter; ++iter) {
    auto const key = *iter.first().asTypedValue();
    k |= from_cell(key);
    v |= from_cell(iter.secondVal());
    if (packed) {
      packed = isIntType(key.m_type) && key.m_data.num == idx;
      ++idx;
    }
  }

  if (packed || is_scalar_counted(k)) return std::nullopt;
  return DArrLikeMapN { std::move(k), std::move(v) };
}

// Convert a static array to a Type containing either DArrLikePacked
// or DArrLikeMap, whatever is appropriate.
Type toDArrLike(SArray ar, trep bits, LegacyMark mark) {
  assertx(!ar->empty());
  assertx(bits & BSArrLikeN);

  if (auto p = toDArrLikePacked(ar)) {
    return packed_impl(bits, mark, std::move(p->elems));
  }

  auto d = toDArrLikeMap(ar);
  assertx(d);
  return map_impl(
    bits,
    mark,
    std::move(d->map),
    std::move(d->optKey),
    std::move(d->optVal)
  );
}

//////////////////////////////////////////////////////////////////////

std::tuple<Type,Type,int64_t> val_key_values(SArray a) {
  assertx(!a->empty());
  auto key = TBottom;
  auto val = TBottom;
  int64_t lastK = -1;
  for (ArrayIter iter(a); iter; ++iter) {
    auto const k = *iter.first().asTypedValue();
    key |= from_cell(k);
    val |= from_cell(iter.secondVal());
    if (k.m_type != KindOfInt64) continue;
    if (k.m_data.num > lastK) lastK = k.m_data.num;
  }
  return std::make_tuple(std::move(key), std::move(val), lastK);
}

// Calculate the appropriate key type for a DArrLikePacked. Normally
// this would just be TInt, but a packed array of length 1 is actually
// ival(0). This isn't just an optimization. This distinction is
// needed to properly enforce invariants on keysets.
Type packed_key(const DArrLikePacked& a) {
  return a.elems.size() == 1 ? ival(0) : TInt;
}

Type packed_values(const DArrLikePacked& a) {
  auto ret = TBottom;
  for (auto const& e : a.elems) ret |= e;
  return ret;
}

//////////////////////////////////////////////////////////////////////

// Convert the key for a given MapElem into its equivalent type.
Type map_key(TypedValue k, const MapElem& mapElem) {
  assertx(tvIsPlausible(k));
  if (isIntType(k.m_type)) {
    assertx(mapElem.keyStaticness == TriBool::Yes);
    return ival(k.m_data.num);
  }
  assertx(k.m_type == KindOfString);
  assertx(!k.m_data.pstr->isRefCounted());
  switch (mapElem.keyStaticness) {
    case TriBool::Yes:   return sval(k.m_data.pstr);
    case TriBool::Maybe: return sval_nonstatic(k.m_data.pstr);
    case TriBool::No:    return sval_counted(k.m_data.pstr);
  }
  always_assert(false);
}

// Like map_key but only produces sval_nonstatic() types (this is
// needed to model key lookup properly, since it does not care about
// staticness).
Type map_key_nonstatic(TypedValue k) {
  assertx(tvIsPlausible(k));
  if (isIntType(k.m_type)) return ival(k.m_data.num);
  assertx(k.m_type == KindOfString);
  assertx(!k.m_data.pstr->isRefCounted());
  return sval_nonstatic(k.m_data.pstr);
}

std::pair<Type,Type> map_key_values(const DArrLikeMap& a) {
  auto ret = std::make_pair(a.optKey, a.optVal);
  for (auto const& kv : a.map) {
    ret.first |= map_key(kv.first, kv.second);
    ret.second |= kv.second.val;
  }
  return ret;
}

//////////////////////////////////////////////////////////////////////

template <bool context>
bool subtypeCls(const DCls& a, const DCls& b) {
  if (context && !a.isCtx() && b.isCtx()) return false;

  auto const nonRegA = a.containsNonRegular();
  auto const nonRegB = b.containsNonRegular();

  auto const impl = [&] (Optional<res::Class> exact1,
                         folly::Range<const res::Class*> sub1,
                         Optional<res::Class> exact2,
                         folly::Range<const res::Class*> sub2) {
    if (exact2 &&
        (!exact1 || !exact1->exactSubtypeOfExact(*exact2, nonRegA, nonRegB))) {
      return false;
    }

    return std::all_of(
      std::begin(sub2), std::end(sub2),
      [&] (res::Class c2) {
        if (exact1 && exact1->exactSubtypeOf(c2, nonRegA, nonRegB)) {
          return true;
        }
        return std::any_of(
          std::begin(sub1), std::end(sub1),
          [&] (res::Class c1) {
            return c1.subSubtypeOf(c2, nonRegA, nonRegB);
          }
        );
      }
    );
  };

  auto const toExact = [] (const DCls& c) -> Optional<res::Class> {
    if (c.isExact()) return c.cls();
    if (c.isIsectAndExact()) return c.isectAndExact().first;
    return std::nullopt;
  };

  auto const toSub = [] (const DCls& c) -> folly::Range<const res::Class*> {
    assertx(!c.isSub());
    if (c.isIsect()) return c.isect();
    if (c.isIsectAndExact()) return *c.isectAndExact().second;
    return {};
  };

  return impl(
    toExact(a),
    a.isSub() ? std::array<res::Class, 1>{a.cls()} : toSub(a),
    toExact(b),
    b.isSub() ? std::array<res::Class, 1>{b.cls()} : toSub(b)
  );
}

bool couldBeCls(const DCls& a, const DCls& b) {
  auto const nonRegA = a.containsNonRegular();
  auto const nonRegB = b.containsNonRegular();

  auto const impl = [&] (Optional<res::Class> exact1,
                         folly::Range<const res::Class*> sub1,
                         Optional<res::Class> exact2,
                         folly::Range<const res::Class*> sub2) {
    if (exact1) {
      if (exact2 && !exact1->exactCouldBeExact(*exact2, nonRegA, nonRegB)) {
        return false;
      }
      auto const all = std::all_of(
        std::begin(sub2), std::end(sub2),
        [&] (res::Class c2) {
          return exact1->exactCouldBe(c2, nonRegA, nonRegB);
        }
      );
      if (!all) return false;
    }

    auto const all = std::all_of(
      std::begin(sub1), std::end(sub1),
      [&] (res::Class c1) {
        if (exact2 && !exact2->exactCouldBe(c1, nonRegB, nonRegA)) {
          return false;
        }
        return std::all_of(
          std::begin(sub2), std::end(sub2),
          [&] (res::Class c2) { return c1.subCouldBe(c2, nonRegA, nonRegB); }
        );
      }
    );
    if (!all) return false;

    if (sub1.empty() || sub2.empty()) return true;
    if (sub1.size() < 2 && sub2.size() < 2) return true;

    return res::Class::couldBeIsect(sub1, sub2, nonRegA, nonRegB);
  };

  auto const toExact = [] (const DCls& c) -> Optional<res::Class> {
    if (c.isExact()) return c.cls();
    if (c.isIsectAndExact()) return c.isectAndExact().first;
    return std::nullopt;
  };

  auto const toSub = [] (const DCls& c) -> folly::Range<const res::Class*> {
    assertx(!c.isSub());
    if (c.isIsect()) return c.isect();
    if (c.isIsectAndExact()) return *c.isectAndExact().second;
    return {};
  };

  return impl(
    toExact(a),
    a.isSub() ? std::array<res::Class, 1>{a.cls()} : toSub(a),
    toExact(b),
    b.isSub() ? std::array<res::Class, 1>{b.cls()} : toSub(b)
  );
}

//////////////////////////////////////////////////////////////////////
template <typename T>
struct DataTagTrait {};

// Represents no specialization. Used in a few places where we need to
// dispatch between specializations and no specialization.
struct DArrLikeNone { trep bits; };

template<> struct DataTagTrait<DArrLikePacked>  { using tag = SArray; };
template<> struct DataTagTrait<DArrLikePackedN> { using tag = SArray; };
template<> struct DataTagTrait<DArrLikeMap>     { using tag = SArray; };
template<> struct DataTagTrait<DArrLikeMapN>    { using tag = SArray; };
template<> struct DataTagTrait<SArray>          { using tag = SArray; };
template<> struct DataTagTrait<DArrLikeNone>    { using tag = SArray; };

/*
 * Helper for dealing with dualDispatchDataFn's---most of them are commutative.
 * This shuffles values to the right in a canonical order to need less
 * overloads.
 */
template<class InnerFn>
struct Commute : InnerFn {

  template<class... Args>
  explicit Commute(Args&&... args) : InnerFn(std::forward<Args>(args)...) {}

  using result_type = typename InnerFn::result_type;

  using InnerFn::operator();

  template<class T1, class T2>
  typename std::enable_if<!std::is_same<T1, T2>::value &&
                          std::is_same<typename DataTagTrait<T1>::tag,
                                       typename DataTagTrait<T2>::tag>::value,
                          result_type>::type
  operator()(const T1& a, const T2& b) const {
    return InnerFn::operator()(b, a);
  }

};

struct DualDispatchCouldBeImpl {
  static constexpr bool disjoint = false;
  using result_type = bool;

  static bool couldBePacked(const DArrLikePacked& a, const DArrLikePacked& b) {
    auto const asz = a.elems.size();
    auto const bsz = b.elems.size();
    if (asz != bsz) return false;
    for (auto i = size_t{0}; i < asz; ++i) {
      if (!a.elems[i].couldBe(b.elems[i])) return false;
    }
    return true;
  }

  static bool couldBeMap(const DArrLikeMap& a, const DArrLikeMap& b) {
    // If A and B don't have optional elements, their values are
    // completely disjoint if the number of keys are different.
    if (!a.hasOptElements() && !b.hasOptElements() &&
        a.map.size() != b.map.size()) {
      return false;
    }

    // Check the common prefix of A and B. The keys must be the same and
    // there must be an intersection among the values.
    auto aIt = begin(a.map);
    auto bIt = begin(b.map);
    while (aIt != end(a.map) && bIt != end(b.map)) {
      if (!tvSame(aIt->first, bIt->first)) return false;
      if (aIt->second.keyStaticness != bIt->second.keyStaticness &&
          aIt->second.keyStaticness != TriBool::Maybe &&
          bIt->second.keyStaticness != TriBool::Maybe) {
          return false;
      }
      if (!aIt->second.val.couldBe(bIt->second.val)) return false;
      ++aIt;
      ++bIt;
    }

    // Process the remaining known keys (only A or B will be
    // processed). The known keys must have an intersection with the
    // other map's optional values (if any).
    while (aIt != end(a.map)) {
      if (!map_key(aIt->first, aIt->second).couldBe(b.optKey)) {
        return false;
      }
      if (!aIt->second.val.couldBe(b.optVal)) return false;
      ++aIt;
    }

    while (bIt != end(b.map)) {
      if (!map_key(bIt->first, bIt->second).couldBe(a.optKey)) {
        return false;
      }
      if (!bIt->second.val.couldBe(a.optVal)) return false;
      ++bIt;
    }

    return true;
  }

  explicit DualDispatchCouldBeImpl(trep b) : isect{b} {}

  bool operator()() const { return false; }

  bool operator()(SArray a, SArray b) const {
    return a == b;
  }
  bool operator()(const DArrLikePacked& a, const DArrLikePacked& b) const {
    return couldBePacked(a, b);
  }
  bool operator()(const DArrLikePackedN& a, const DArrLikePackedN& b) const {
    return a.type.couldBe(b.type);
  }
  bool operator()(const DArrLikeMap& a, const DArrLikeMap& b) const {
    return couldBeMap(a, b);
  }
  bool operator()(const DArrLikeMapN& a, const DArrLikeMapN& b) const {
    if (!a.key.couldBe(b.key) || !a.val.couldBe(b.val)) return false;
    // Keyset specializations can only be either other if the key ==
    // val invariant holds after intersecting them.
    if (!subtypeAmong(isect, BKeysetN, BArrLikeN)) return true;
    if (equal(a.key, a.val) && equal(b.key, b.val)) return true;
    return intersection_of(a.key, b.key).couldBe(intersection_of(a.val, b.val));
  }

  bool operator()(const DArrLikePacked& a, SArray b) const {
    if (a.elems.size() != b->size()) return false;
    auto const p = toDArrLikePacked(b);
    return p && couldBePacked(a, *p);
  }

  bool operator()(const DArrLikeMap& a, SArray b) const {
    // If A doesn't have optional elements, there's no intersection if
    // they have a different number of known keys.
    if (!a.hasOptElements() && a.map.size() != b->size()) return false;
    auto const m = toDArrLikeMap(b);
    return m && couldBeMap(a, *m);
  }

  bool operator()(const DArrLikePackedN& a, SArray b) const {
    auto const p = toDArrLikePackedN(b);
    return p && a.type.couldBe(p->type);
  }

  bool operator()(const DArrLikeMapN& a, SArray b) const {
    assertx(!b->empty());
    bool bad = false;
    IterateKV(
      b,
      [&] (TypedValue k, TypedValue v) {
        bad |= !(a.key.couldBe(from_cell(k)) && a.val.couldBe(from_cell(v)));
        return bad;
      }
    );
    return !bad;
  }

  bool operator()(const DArrLikePacked& a, const DArrLikePackedN& b) const {
    for (auto const& t : a.elems) {
      if (!t.couldBe(b.type)) return false;
    }
    return true;
  }

  bool operator()(const DArrLikePackedN& a, const DArrLikeMapN& b) const {
    if (!b.key.couldBe(BInt) || !a.type.couldBe(b.val)) return false;
    if (!subtypeAmong(isect, BKeysetN, BArrLikeN)) return true;
    if (a.type.is(BInt) && equal(b.key, b.val)) return true;
    return intersection_of(b.key, TInt).couldBe(intersection_of(a.type, b.val));
  }

  bool operator()(const DArrLikeMap& a, const DArrLikeMapN& b) const {
    for (auto const& kv : a.map) {
      if (!map_key(kv.first, kv.second).couldBe(b.key)) return false;
      if (!kv.second.val.couldBe(b.val)) return false;
    }
    // We can ignore optional elements here. If the values
    // corresponding to just the known keys already intersect with B,
    // then we have an intersection so we're done.
    return true;
  }

  bool operator()(const DArrLikePacked&, const DArrLikeMap&) const {
    // Map does not contain any packed arrays.
    return false;
  }
  bool operator()(const DArrLikePacked& a, const DArrLikeMapN& b) const {
    auto const packedKey = packed_key(a);
    if (!b.key.couldBe(packedKey)) return false;
    for (auto const& t : a.elems) {
      if (!t.couldBe(b.val)) return false;
    }
    if (!subtypeAmong(isect, BKeysetN, BArrLikeN)) return true;
    auto const vals = packed_values(a);
    if (vals.equal(packedKey) && equal(b.key, b.val)) return true;
    return
      intersection_of(b.key, packedKey).couldBe(intersection_of(vals, b.val));
  }
  bool operator()(const DArrLikePackedN&, const DArrLikeMap&) const {
    // Map does not contain any packed arrays.
    return false;
  }

  // Operators on DArrLikeNone. These must check if the
  // specialization's key and value are "allowed" for None's trep.

  bool operator()(const DArrLikeNone&, SArray) const {
    // None contains all possible arrays, so it always could be.
    return true;
  }
  bool operator()(const DArrLikeNone& a, const DArrLikePackedN& b) const {
    auto const key = allowedKeyBits(a.bits).first;
    if (!couldBe(key, BInt)) return false;
    auto const val = allowedValBits(a.bits, true).first;
    return b.type.couldBe(val);
  }
  bool operator()(const DArrLikeNone& a, const DArrLikePacked& b) const {
    auto const key = allowedKeyBits(a.bits).first;
    if (!couldBe(key, BInt)) return false;
    auto const val = allowedValBits(a.bits, true).first;
    for (auto const& t : b.elems) {
      if (!t.couldBe(val)) return false;
    }
    return true;
  }
  bool operator()(const DArrLikeNone& a, const DArrLikeMapN& b) const {
    auto const key = allowedKeyBits(a.bits).first;
    auto const val = allowedValBits(a.bits, false).first;
    return b.key.couldBe(key) && b.val.couldBe(val);
  }
  bool operator()(const DArrLikeNone& a, const DArrLikeMap& b) const {
    // Vec cannot have a Map specialization.
    if (subtypeAmong(isect, BVecN, BArrLikeN)) return false;
    auto const key = allowedKeyBits(a.bits).first;
    auto const val = allowedValBits(a.bits, false).first;
    for (auto const& kv : b.map) {
      if (!map_key(kv.first, kv.second).couldBe(key)) return false;
      if (!kv.second.val.couldBe(val)) return false;
    }
    return true;
  }

private:
  trep isect;
};

// pre: neither type is a subtype of the other (except for the
// DArrLikeNone variants)
struct DualDispatchIntersectionImpl {
  static constexpr bool disjoint = false;
  using result_type = Type;

  DualDispatchIntersectionImpl(trep b, LegacyMark m)
    : isect{b}, mark{m} {}

  // For any specific array type which is entirely static, remove it.
  static trep remove_single_static_bits(trep b) {
    if ((b & BVecN)    == BSVecN)    b &= ~BSVecN;
    if ((b & BDictN)   == BSDictN)   b &= ~BSDictN;
    if ((b & BKeysetN) == BSKeysetN) b &= ~BSKeysetN;
    return b;
  }

  Type operator()() const { not_reached(); }

  static MapElem intersect_map_elem(MapElem e1, MapElem e2) {
    TriBool staticness;
    if (e1.keyStaticness != e2.keyStaticness) {
      if (e1.keyStaticness == TriBool::Maybe) {
        staticness = e2.keyStaticness;
      } else if (e2.keyStaticness == TriBool::Maybe) {
        staticness = e1.keyStaticness;
      } else {
        return MapElem { TBottom, TriBool::No };
      }
    } else {
      staticness = e1.keyStaticness;
    }

    return MapElem {
      intersection_of(std::move(e1.val), std::move(e2.val)),
      staticness
    };
  }

  static MapElem intersect_map_elem(MapElem e, Type key, Type val) {
    auto staticness = e.keyStaticness;
    if (!key.couldBe(BCounted)) {
      if (e.keyStaticness == TriBool::No) {
        return MapElem { TBottom, TriBool::No };
      }
      if (e.keyStaticness == TriBool::Maybe) staticness = TriBool::Yes;
    }
    if (!key.couldBe(BUnc)) {
      if (e.keyStaticness == TriBool::Yes) {
        return MapElem { TBottom, TriBool::No };
      }
      if (e.keyStaticness == TriBool::Maybe) staticness = TriBool::No;
    }

    return MapElem {
      intersection_of(std::move(e.val), std::move(val)),
      staticness
    };
  }

  /*
   * When intersecting two array types, we may have to change the
   * types in the specialization to match. For example, if the
   * intersection produces a trep of BVec, we cannot have non-int
   * keys. Likewise, if the intersection produces a static array type
   * trep, we cannot have counted types in the
   * specialization. However, the key/value types in the
   * specialization can also cause us to remove array types from the
   * trep!
   *
   * Namely, conflicts due to the allowed upper bound (see
   * allowedKeyBits()/allowedValBits()) are handled by modifying the
   * specialization's types. Conflicts due to the allowed lower bound
   * are handled by removing violating trep bits.
   *
   * This makes the interactions tricky to model, since changing the
   * trep bits might change what is allowed in the
   * specialization. When we perform the intersection, we proceed as
   * normal. If we need to modify the trep bits, we do so, but then
   * restart the intersection from the beginning (with the new trep
   * bits). This is a rare operation so there's no real cost.
   *
   * All the intersection helper functions take the current trep by
   * ref and return an optional Type. If it returns std::nullopt, the
   * trep is assumed to be modified and the process
   * restarts. Otherwise the operation is assumed to have finished
   * successfully. This is guaranteed to terminate because we only
   * return std::nullopt when we've removed bits from the trep (and
   * thus we'll eventually run out of bits to remove).
   */

  template <typename F>
  Optional<Type> intersect_packed(trep& bits,
                                  std::vector<Type> elems,
                                  F next) const {
    auto const valBits = allowedValBits(bits, true);

    size_t i = 0;
    for (auto& e : elems) {
      e &= next();
      if (e.is(BBottom)) return TBottom;

      if (!e.subtypeOf(valBits.first)) {
        e = intersection_of(std::move(e), Type{valBits.first});
        if (e.is(BBottom)) return TBottom;
      }

      if (subtypeAmong(bits, BKeysetN, BArrLikeN)) {
        auto iv = ival(i);
        if (!e.subtypeOf(iv)) {
          e = intersection_of(std::move(e), std::move(iv));
        }
        if (e.is(BBottom)) return TBottom;
      }

      if (!e.couldBe(valBits.second)) {
        if (couldBe(bits, BKeysetN) && !e.couldBe(BInt)) {
          bits &= ~BKeysetN;
          if (!couldBe(bits, BArrLikeN)) return TBottom;
          return std::nullopt;
        }

        if (mustContainUncounted(bits) && !e.couldBe(BUnc)) {
          bits = remove_single_static_bits(bits);
          if (!couldBe(bits, BArrLikeN)) return TBottom;
          return std::nullopt;
        }
      }

      if (couldBe(bits, BKeysetN) && !e.couldBe(ival(i))) {
        bits &= ~BKeysetN;
        if (!couldBe(bits, BArrLikeN)) return TBottom;
        return std::nullopt;
      }

      ++i;
    }

    return packed_impl(bits, mark, std::move(elems));
  }

  Optional<Type> handle_packedn(trep& bits, Type val) const {
    if (val.is(BBottom)) return TBottom;

    auto const valBits = allowedValBits(bits, true);
    if (!val.subtypeOf(valBits.first)) {
      val = intersection_of(std::move(val), Type{valBits.first});
      if (val.is(BBottom)) return TBottom;
    }

    if (subtypeAmong(bits, BKeysetN, BArrLikeN)) {
      if (!val.subtypeOf(BInt)) {
        val = intersection_of(std::move(val), TInt);
        if (val.is(BBottom)) return TBottom;
      }
      if (is_specialized_int(val)) {
        if (ival_of(val) == 0) {
          return packed_impl(bits, mark, std::vector<Type>{std::move(val)});
        }
        return TBottom;
      }
    }

    if (!val.couldBe(valBits.second)) {
      if (couldBe(bits, BKeysetN) && !val.couldBe(BInt)) {
        bits &= ~BKeysetN;
        if (!couldBe(bits, BArrLikeN)) return TBottom;
        return std::nullopt;
      }

      if (mustContainUncounted(bits) && !val.couldBe(BUnc)) {
        bits = remove_single_static_bits(bits);
        if (!couldBe(bits, BArrLikeN)) return TBottom;
        return std::nullopt;
      }
    }

    if (couldBe(bits, BKeysetN) &&
        is_specialized_int(val) &&
        ival_of(val) != 0) {
      bits &= ~BKeysetN;
      if (!couldBe(bits, BArrLikeN)) return TBottom;
      return std::nullopt;
    }

    return packedn_impl(bits, mark, std::move(val));
  }

  Optional<Type> handle_mapn(trep& bits, Type key, Type val) const {
    if (key.is(BBottom) || val.is(BBottom)) return TBottom;

    // Special case: A Vec on its own cannot have a Map specialization
    // (it can unioned with other array types). If the intersection has
    // produced one, turn the specialization into a PackedN.
    if (subtypeAmong(bits, BVecN, BArrLikeN)) {
      if (!key.couldBe(BInt)) return TBottom;
      return handle_packedn(bits, std::move(val));
    }

    auto const keyBits = allowedKeyBits(bits);
    auto const valBits = allowedValBits(bits, false);

    if (!key.subtypeOf(keyBits.first)) {
      key = intersection_of(std::move(key), Type{keyBits.first});
      if (key.is(BBottom)) return TBottom;
    }
    if (!val.subtypeOf(valBits.first)) {
      val = intersection_of(std::move(val), Type{valBits.first});
      if (val.is(BBottom)) return TBottom;
    }

    if (subtypeAmong(bits, BKeysetN, BArrLikeN) && !equal(key, val)) {
      auto isect = intersection_of(key, val);
      if (isect.is(BBottom)) return TBottom;
      if (!key.subtypeOf(isect)) key = isect;
      if (!val.subtypeOf(isect)) val = isect;
    }

    if (!key.couldBe(keyBits.second)) {
      if (couldBe(bits, BVecN) && !key.couldBe(BInt)) {
        bits &= ~BVecN;
        if (!couldBe(bits, BArrLikeN)) return TBottom;
        return std::nullopt;
      }

      if (couldBe(bits, BKeysetN)) {
        auto const arrkey =
          subtypeAmong(bits, BSKeysetN, BKeysetN) ? BUncArrKey : BArrKey;
        if (!key.couldBe(arrkey)) {
          bits &= ~BKeysetN;
          if (!couldBe(bits, BArrLikeN)) return TBottom;
          return std::nullopt;
        }
      }

      if (mustContainUncounted(bits) && !key.couldBe(BUnc)) {
        bits = remove_single_static_bits(bits);
        if (!couldBe(bits, BArrLikeN)) return TBottom;
        return std::nullopt;
      }
    }

    if (!val.couldBe(valBits.second)) {
      if (couldBe(bits, BKeysetN)) {
        auto const arrkey =
          subtypeAmong(bits, BSKeysetN, BKeysetN) ? BUncArrKey : BArrKey;
        if (!val.couldBe(arrkey)) {
          bits &= ~BKeysetN;
          if (!couldBe(bits, BArrLikeN)) return TBottom;
          return std::nullopt;
        }
      }

      if (mustContainUncounted(bits) && !val.couldBe(BUnc)) {
        bits = remove_single_static_bits(bits);
        if (!couldBe(bits, BArrLikeN)) return TBottom;
        return std::nullopt;
      }
    }

    if (couldBe(bits, BKeysetN) && !key.couldBe(val)) {
      bits &= ~BKeysetN;
      if (!couldBe(bits, BArrLikeN)) return TBottom;
      return std::nullopt;
    }

    return mapn_impl(bits, mark, std::move(key), std::move(val));
  }

  Optional<Type> handle_map(trep& bits,
                                   MapElems elems,
                                   Type optKey,
                                   Type optVal) const {
    auto const valBits = allowedValBits(bits, false);
    auto const keyBits = allowedKeyBits(bits);

    for (auto it = elems.begin(); it != elems.end(); ++it) {
      auto& kv = *it;
      assertx(tvIsPlausible(kv.first));

      if (subtypeOf(keyBits.first, BUncArrKey)) {
        if (kv.second.keyStaticness == TriBool::No) return TBottom;
        if (kv.second.keyStaticness == TriBool::Maybe) {
          elems.update(it, kv.second.withStaticness(TriBool::Yes));
        }
      }

      if (!kv.second.val.subtypeOf(valBits.first)) {
        auto val = intersection_of(kv.second.val, Type{valBits.first});
        if (val.is(BBottom)) return TBottom;
        if (!kv.second.val.subtypeOf(val)) {
          elems.update(it, kv.second.withType(std::move(val)));
        }
      }
      if (subtypeAmong(bits, BKeysetN, BArrLikeN)) {
        auto key = map_key(kv.first, kv.second);
        if (!equal(key, kv.second.val)) {
          auto val = intersection_of(std::move(key), kv.second.val);
          if (val.is(BBottom)) return TBottom;
          elems.update(it, MapElem::KeyFromType(val, val));
        }
      }

      if (mustContainUncounted(bits) &&
          kv.second.keyStaticness == TriBool::No) {
        bits = remove_single_static_bits(bits);
        if (!couldBe(bits, BArrLikeN)) return TBottom;
        return std::nullopt;
      }

      if (!kv.second.val.couldBe(valBits.second)) {
        if (couldBe(bits, BKeysetN)) {
          auto const val =
            subtypeAmong(bits, BSKeysetN, BKeysetN) ? BUncArrKey : BArrKey;
          if (!kv.second.val.couldBe(val)) {
            bits &= ~BKeysetN;
            if (!couldBe(bits, BArrLikeN)) return TBottom;
            return std::nullopt;
          }
        }
        if (mustContainUncounted(bits) && !kv.second.val.couldBe(BUnc)) {
          bits = remove_single_static_bits(bits);
          if (!couldBe(bits, BArrLikeN)) return TBottom;
          return std::nullopt;
        }
      }
      if (couldBe(bits, BKeysetN) &&
          !kv.second.val.couldBe(map_key(kv.first, kv.second))) {
        bits &= ~BKeysetN;
        if (!couldBe(bits, BArrLikeN)) return TBottom;
        return std::nullopt;
      }
    }

    if (!optKey.is(BBottom)) {
      assertx(!optVal.is(BBottom));

      if (!optVal.subtypeOf(valBits.first)) {
        optVal = intersection_of(std::move(optVal), Type{valBits.first});
      }

      if (!optKey.subtypeOf(keyBits.first)) {
        optKey = intersection_of(std::move(optKey), Type{keyBits.first});
      }

      if (optKey.is(BBottom) || optVal.is(BBottom)) {
        optKey = TBottom;
        optVal = TBottom;
      }

      if (subtypeAmong(bits, BKeysetN, BArrLikeN) && !equal(optKey, optVal)) {
        auto isect = intersection_of(optKey, optVal);

        if (is_specialized_int(isect)) {
          auto const tv = make_tv<KindOfInt64>(ival_of(isect));
          if (elems.find(tv) != elems.end()) {
            isect = remove_int(std::move(isect));
          }
        } else if (is_specialized_string(isect)) {
          auto const tv = make_tv<KindOfString>(sval_of(isect));
          if (elems.find(tv) != elems.end()) {
            isect = remove_string(std::move(isect));
          }
        }

        if (!optKey.subtypeOf(isect)) optKey = isect;
        if (!optVal.subtypeOf(isect)) optVal = isect;
      }
    }

    return map_impl(
      bits,
      mark,
      std::move(elems),
      std::move(optKey),
      std::move(optVal)
    );
  }

  // Helper function to deal with the restart logic. Keep looping,
  // calling f with the bits until it returns something.
  template <typename F> Type handle(F f) const {
    auto bits = isect;
    do {
      auto const DEBUG_ONLY origBits = bits;
      auto const result = f(bits);
      if (result) return *result;
      assertx(couldBe(bits, BArrLikeN));
      assertx(bits != origBits && subtypeOf(bits, origBits));
    } while (true);
  }

  // The SArray is known to not be a subtype, so the intersection must be empty
  Type operator()(const DArrLikePacked&, const SArray) const {
    return TBottom;
  }
  Type operator()(const DArrLikePackedN&, const SArray) const {
    return TBottom;
  }
  Type operator()(const DArrLikeMapN&, const SArray) const {
    return TBottom;
  }
  Type operator()(const DArrLikeMap&, const SArray) const {
    return TBottom;
  }
  Type operator()(const SArray, const SArray) const {
    return TBottom;
  }

  Type operator()(const DArrLikePacked& a, const DArrLikePacked& b) const {
    if (a.elems.size() != b.elems.size()) return TBottom;
    return handle(
      [&] (trep& bits) {
        auto i = size_t{};
        return intersect_packed(bits, a.elems, [&] { return b.elems[i++]; });
      }
    );
  }
  Type operator()(const DArrLikePacked& a, const DArrLikePackedN& b) const {
    return handle(
      [&] (trep& bits) {
        return intersect_packed(bits, a.elems, [&] { return b.type; });
      }
    );
  }
  Type operator()(const DArrLikePacked& a, const DArrLikeMapN& b) const {
    if (!b.key.couldBe(packed_key(a))) return TBottom;
    return handle(
      [&] (trep& bits) {
        return intersect_packed(bits, a.elems, [&] { return b.val; });
      }
    );
  }
  Type operator()(const DArrLikePacked&, const DArrLikeMap&) const {
    // We don't allow DArrLikeMaps which are packed
    return TBottom;
  }

  Type operator()(const DArrLikePackedN& a, const DArrLikePackedN& b) const {
    return handle(
      [&] (trep& bits) {
        return handle_packedn(bits, intersection_of(a.type, b.type));
      }
    );
  }
  Type operator()(const DArrLikePackedN& a, const DArrLikeMapN& b) const {
    if (!b.key.couldBe(BInt)) return TBottom;
    return handle(
      [&] (trep& bits) {
        return handle_packedn(bits, intersection_of(b.val, a.type));
      }
    );
  }
  Type operator()(const DArrLikePackedN&, const DArrLikeMap&) const {
    return TBottom;
  }

  Type operator()(const DArrLikeMapN& a, const DArrLikeMapN& b) const {
    return handle(
      [&] (trep& bits) {
        return handle_mapn(
          bits,
          intersection_of(a.key, b.key),
          intersection_of(a.val, b.val)
        );
      }
    );
  }
  Type operator()(const DArrLikeMapN& a, const DArrLikeMap& b) const {
    return handle(
      [&] (trep& bits) -> Optional<Type> {
        auto map = MapElems{};

        for (auto const& kv : b.map) {
          if (!map_key(kv.first, kv.second).couldBe(a.key)) return TBottom;
          auto elem = intersect_map_elem(kv.second, a.key, a.val);
          if (elem.val.is(BBottom)) return TBottom;
          map.emplace_back(kv.first, std::move(elem));
        }

        auto optKey = TBottom;
        auto optVal = TBottom;
        if (b.hasOptElements()) {
          optKey = intersection_of(b.optKey, a.key);
          optVal = intersection_of(b.optVal, a.val);
          if (optKey.is(BBottom) || optVal.is(BBottom)) {
            optKey = TBottom;
            optVal = TBottom;
          }
        }

        return handle_map(
          bits, std::move(map), std::move(optKey), std::move(optVal)
        );
      }
    );
  }

  Type operator()(const DArrLikeMap& a, const DArrLikeMap& b) const {
    // Two maps without optional elements have no values in common if
    // they have different sets of keys.
    if (!a.hasOptElements() && !b.hasOptElements() &&
        a.map.size() != b.map.size()) {
      return TBottom;
    }

    return handle(
      [&] (trep& bits) -> Optional<Type> {
        auto map = MapElems{};

        auto aIt = begin(a.map);
        auto bIt = begin(b.map);

        // Intersect the common known keys
        while (aIt != end(a.map) && bIt != end(b.map)) {
          if (!tvSame(aIt->first, bIt->first)) return TBottom;
          auto elem = intersect_map_elem(aIt->second, bIt->second);
          if (elem.val.is(BBottom)) return TBottom;
          map.emplace_back(aIt->first, std::move(elem));
          ++aIt;
          ++bIt;
        }

        // Any remaining known keys are only in A, or in B. Intersect them
        // with the optional elements in the other map. If the other map
        // doesn't have optional elements, the intersection will be
        // Bottom.
        while (aIt != end(a.map)) {
          if (!map_key(aIt->first, aIt->second).couldBe(b.optKey)) {
            return TBottom;
          }
          auto elem =
            intersect_map_elem(aIt->second, b.optKey, b.optVal);
          if (elem.val.is(BBottom)) return TBottom;
          map.emplace_back(aIt->first, std::move(elem));
          ++aIt;
        }

        while (bIt != end(b.map)) {
          if (!map_key(bIt->first, bIt->second).couldBe(a.optKey)) {
            return TBottom;
          }
          auto elem =
            intersect_map_elem(bIt->second, a.optKey, a.optVal);
          if (elem.val.is(BBottom)) return TBottom;
          map.emplace_back(bIt->first, std::move(elem));
          ++bIt;
        }

        auto optKey = TBottom;
        auto optVal = TBottom;
        if (a.hasOptElements() && b.hasOptElements()) {
          optKey = intersection_of(a.optKey, b.optKey);
          optVal = intersection_of(a.optVal, b.optVal);
          if (optKey.is(BBottom) || optVal.is(BBottom)) {
            optKey = TBottom;
            optVal = TBottom;
          }
        }

        return handle_map(
          bits, std::move(map), std::move(optKey), std::move(optVal)
        );
      }
    );
  }

  Type operator()(const DArrLikeNone&, SArray) const {
    // We know one is not a subtype of the other, so we shouldn't get
    // here.
    not_reached();
  }
  Type operator()(const DArrLikeNone&, const DArrLikePackedN& b) const {
    return handle(
      [&] (trep& bits) -> Optional<Type> {
        auto const keys = allowedKeyBits(bits).first;
        if (!couldBe(keys, BInt)) return TBottom;
        auto const vals = allowedValBits(bits, true).first;
        return handle_packedn(bits, intersection_of(Type{vals}, b.type));
      }
    );
  }
  Type operator()(const DArrLikeNone&, const DArrLikePacked& b) const {
    return handle(
      [&] (trep& bits) -> Optional<Type> {
        auto const keys = allowedKeyBits(bits).first;
        if (!couldBe(keys, BInt)) return TBottom;
        auto const vals = Type{allowedValBits(bits, true).first};
        return intersect_packed(bits, b.elems, [&] { return vals; });
      }
    );
  }
  Type operator()(const DArrLikeNone&, const DArrLikeMapN& b) const {
    return handle(
      [&] (trep& bits) {
        auto const keys = allowedKeyBits(bits).first;
        auto const vals = allowedValBits(bits, false).first;
        return handle_mapn(
          bits,
          intersection_of(b.key, Type{keys}),
          intersection_of(b.val, Type{vals})
        );
      }
    );
  }
  Type operator()(const DArrLikeNone&, const DArrLikeMap& b) const {
    return handle(
      [&] (trep& bits) -> Optional<Type> {
        auto const keys = Type{allowedKeyBits(bits).first};
        auto const vals = Type{allowedValBits(bits, false).first};

        auto map = MapElems{};
        for (auto const& kv : b.map) {
          if (!map_key(kv.first, kv.second).couldBe(keys)) return TBottom;
          auto elem = intersect_map_elem(kv.second, keys, vals);
          if (elem.val.is(BBottom)) return TBottom;
          map.emplace_back(kv.first, std::move(elem));
        }

        auto optKey = TBottom;
        auto optVal = TBottom;
        if (b.hasOptElements()) {
          optKey = intersection_of(b.optKey, keys);
          optVal = intersection_of(b.optVal, vals);
          if (optKey.is(BBottom) || optVal.is(BBottom)) {
            optKey = TBottom;
            optVal = TBottom;
          }
        }

        return handle_map(
          bits, std::move(map), std::move(optKey), std::move(optVal)
        );
      }
    );
  }

private:
  trep isect;
  LegacyMark mark;
};

struct DualDispatchUnionImpl {
  static constexpr bool disjoint = false;
  using result_type = Type;

  DualDispatchUnionImpl(trep combined, LegacyMark m)
    : combined{combined}, mark{m} {}

  static MapElem union_map_elem(MapElem e1, MapElem e2) {
    return MapElem {
      union_of(std::move(e1.val), std::move(e2.val)),
      e1.keyStaticness | e2.keyStaticness
    };
  }

  Type operator()() const { not_reached(); }

  Type operator()(const DArrLikePacked& a, const DArrLikePacked& b) const {
    if (a.elems.size() != b.elems.size()) {
      return packedn_impl(
        combined, mark, union_of(packed_values(a), packed_values(b))
      );
    }
    auto ret = a.elems;
    for (auto i = size_t{0}; i < a.elems.size(); ++i) {
      ret[i] |= b.elems[i];
    }
    return packed_impl(combined, mark, std::move(ret));
  }

  Type operator()(const DArrLikePackedN& a, const DArrLikePackedN& b) const {
    return packedn_impl(combined, mark, union_of(a.type, b.type));
  }

  Type operator()(const DArrLikePacked& a, const DArrLikePackedN& b) const {
    return (*this)(DArrLikePackedN { packed_values(a) }, b);
  }

  Type operator()(const DArrLikeMap& a, const DArrLikeMap& b) const {
    auto map = MapElems{};

    // Keep the common prefix of known keys in both A and B.
    auto aIt = begin(a.map);
    auto bIt = begin(b.map);
    while (aIt != end(a.map) && bIt != end(b.map)) {
      if (!tvSame(aIt->first, bIt->first)) break;
      map.emplace_back(aIt->first, union_map_elem(aIt->second, bIt->second));
      ++aIt;
      ++bIt;
    }

    // If there's no common prefix, fall back to MapN.
    if (map.empty()) {
      auto mkva = map_key_values(a);
      auto mkvb = map_key_values(b);
      return mapn_impl(
        combined,
        mark,
        union_of(std::move(mkva.first), std::move(mkvb.first)),
        union_of(std::move(mkva.second), std::move(mkvb.second))
      );
    }

    // Any non-common known keys will be combined into the optional
    // elements.
    auto optKey = union_of(a.optKey, b.optKey);
    auto optVal = union_of(a.optVal, b.optVal);

    while (aIt != end(a.map)) {
      optKey |= map_key(aIt->first, aIt->second);
      optVal |= aIt->second.val;
      ++aIt;
    }
    while (bIt != end(b.map)) {
      optKey |= map_key(bIt->first, bIt->second);
      optVal |= bIt->second.val;
      ++bIt;
    }

    return map_impl(
      combined,
      mark,
      std::move(map),
      std::move(optKey),
      std::move(optVal)
    );
  }

  Type operator()(SArray a, SArray b) const {
    assertx(a != b); // Should've been handled earlier in union_of.
    assertx(!a->empty());
    assertx(!b->empty());

    auto const p1 = toDArrLikePacked(a);
    auto const p2 = toDArrLikePacked(b);
    if (p1) {
      if (p2) return (*this)(*p1, *p2);
      return (*this)(*p1, *toDArrLikeMap(b));
    } else if (p2) {
      return (*this)(*p2, *toDArrLikeMap(a));
    } else {
      return (*this)(*toDArrLikeMap(a), *toDArrLikeMap(b));
    }
  }

  Type operator()(const DArrLikeMapN& a, const DArrLikeMapN& b) const {
    return mapn_impl(
      combined,
      mark,
      union_of(a.key, b.key),
      union_of(a.val, b.val)
    );
  }

  Type operator()(const DArrLikePacked& a, SArray b) const {
    assertx(!b->empty());
    auto const p = toDArrLikePacked(b);
    if (p) return (*this)(a, *p);
    return (*this)(a, *toDArrLikeMap(b));
  }

  Type operator()(const DArrLikePackedN& a, SArray b) const {
    assertx(!b->empty());
    auto const p = toDArrLikePackedN(b);
    if (p) return (*this)(a, *p);
    return (*this)(a, *toDArrLikeMap(b));
  }

  Type operator()(const DArrLikeMap& a, SArray b) const {
    assertx(!b->empty());
    auto const m = toDArrLikeMap(b);
    if (m) return (*this)(a, *m);
    return (*this)(*toDArrLikePacked(b), a);
  }

  Type operator()(const DArrLikeMapN& a, SArray b) const {
    assertx(!b->empty());
    auto const m1 = toDArrLikeMapN(b);
    if (m1) return (*this)(a, *m1);
    auto const m2 = toDArrLikeMap(b);
    if (m2) return (*this)(*m2, a);
    return (*this)(*toDArrLikePackedN(b), a);
  }

  Type operator()(const DArrLikePacked& a, const DArrLikeMap& b) const {
    auto mkv = map_key_values(b);
    return mapn_impl(
      combined,
      mark,
      union_of(std::move(mkv.first), packed_key(a)),
      union_of(std::move(mkv.second), packed_values(a))
    );
  }

  Type operator()(const DArrLikePacked& a, const DArrLikeMapN& b) const {
    return mapn_impl(
      combined,
      mark,
      union_of(packed_key(a), b.key),
      union_of(packed_values(a), b.val)
    );
  }

  Type operator()(const DArrLikePackedN& a, const DArrLikeMap& b) const {
    auto mkv = map_key_values(b);
    return mapn_impl(
      combined,
      mark,
      union_of(TInt, std::move(mkv.first)),
      union_of(a.type, std::move(mkv.second))
    );
  }

  Type operator()(const DArrLikePackedN& a, const DArrLikeMapN& b) const {
    return mapn_impl(
      combined,
      mark,
      union_of(TInt, b.key),
      union_of(a.type, b.val)
    );
  }

  Type operator()(const DArrLikeMap& a, const DArrLikeMapN& b) const {
    auto mkv = map_key_values(a);
    return mapn_impl(
      combined,
      mark,
      union_of(std::move(mkv.first), b.key),
      union_of(std::move(mkv.second), b.val)
    );
  }

  Type operator()(const DArrLikeNone& a, const DArrLikePackedN& b) const {
    // Special case: A Vec is always known to be packed. So if we
    // union together a Vec with something else with a packed
    // specialization, we can keep the packed specialization.
    if (!subtypeAmong(a.bits, BVecN, BArrLikeN)) {
      return (*this)(a, DArrLikeMapN{ TInt, b.type });
    }
    auto val = Type{allowedValBits(a.bits, true).first};
    if (!val.strictSubtypeOf(BInitCell)) return Type { combined, mark };
    return packedn_impl(combined, mark, union_of(std::move(val), b.type));
  }
  Type operator()(const DArrLikeNone& a, const DArrLikeMapN& b) const {
    // Special case: Map includes all possible array structures, so we
    // can just union together None's implied key/value types into the
    // Map.
    auto key = Type{allowedKeyBits(a.bits).first};
    auto val = Type{allowedValBits(a.bits, false).first};
    if (!key.strictSubtypeOf(BArrKey) && !val.strictSubtypeOf(BInitCell)) {
      return Type { combined, mark };
    }
    return mapn_impl(
      combined,
      mark,
      union_of(std::move(key), b.key),
      union_of(std::move(val), b.val)
    );
  }
  Type operator()(const DArrLikeNone& a, SArray b) const {
    assertx(!b->empty());
    if (auto const p = toDArrLikePacked(b)) {
      return (*this)(a, *p);
    }
    return (*this)(a, *toDArrLikeMap(b));
  }
  Type operator()(const DArrLikeNone& a, const DArrLikePacked& b) const {
    if (!subtypeAmong(a.bits, BVecN, BArrLikeN)) {
      return (*this)(a, DArrLikeMapN{ packed_key(b), packed_values(b) });
    }
    auto val = Type{allowedValBits(a.bits, true).first};
    if (!val.strictSubtypeOf(BInitCell)) return Type { combined, mark };
    return packedn_impl(
      combined,
      mark,
      union_of(std::move(val), packed_values(b))
    );
  }
  Type operator()(const DArrLikeNone& a, const DArrLikeMap& b) const {
    auto key = Type{allowedKeyBits(a.bits).first};
    auto val = Type{allowedValBits(a.bits, false).first};
    if (!key.strictSubtypeOf(BArrKey) && !val.strictSubtypeOf(BInitCell)) {
      return Type { combined, mark };
    }
    auto mkv = map_key_values(b);
    return mapn_impl(
      combined,
      mark,
      union_of(std::move(key), std::move(mkv.first)),
      union_of(std::move(val), std::move(mkv.second))
    );
  }

private:
  trep combined;
  LegacyMark mark;
};

/*
 * Subtype is not a commutative relation, so this is the only
 * dualDispatchDataFn helper that doesn't use Commute<>.
 */
template<bool contextSensitive>
struct DualDispatchSubtype {
  static constexpr bool disjoint = false;
  using result_type = bool;

  static bool subtype(const Type& a, const Type& b) {
    return contextSensitive ? a.moreRefined(b) : a.subtypeOf(b);
  }

  static bool subtypePacked(const DArrLikePacked& a, const DArrLikePacked& b) {
    auto const asz = a.elems.size();
    auto const bsz = b.elems.size();
    if (asz != bsz) return false;
    for (auto i = size_t{0}; i < asz; ++i) {
      if (!subtype(a.elems[i], b.elems[i])) return false;
    }
    return true;
  }

  static bool subtypeMap(const DArrLikeMap& a, const DArrLikeMap& b) {
    // If both A and B both don't have optional elements, their values
    // are completely disjoint if there's a different number of keys.
    if (!a.hasOptElements() && !b.hasOptElements() &&
        a.map.size() != b.map.size()) {
      return false;
    }

    // Check the common prefix of known keys. The keys must be the same
    // and have compatible types.
    auto aIt = begin(a.map);
    auto bIt = begin(b.map);
    while (aIt != end(a.map) && bIt != end(b.map)) {
      if (!tvSame(aIt->first, bIt->first)) return false;
      if (aIt->second.keyStaticness != bIt->second.keyStaticness) {
        if (bIt->second.keyStaticness != TriBool::Maybe) return false;
      }
      if (!subtype(aIt->second.val, bIt->second.val)) return false;
      ++aIt;
      ++bIt;
    }
    // If B has more known keys than A, A cannot be a subtype of B. It
    // doesn't matter if A has optional elements since there's values in
    // A which doesn't have them, while B always has the keys.
    if (bIt != end(b.map)) return false;

    // If A has any remaining known keys, check they're compatible with
    // B's optional elements (if any).
    while (aIt != end(a.map)) {
      if (!subtype(map_key(aIt->first, aIt->second), b.optKey)) {
        return false;
      }
      if (!subtype(aIt->second.val, b.optVal)) return false;
      ++aIt;
    }

    // Finally the optional values (if any) of A and B must be
    // compatible.
    return
      subtype(a.optKey, b.optKey) &&
      subtype(a.optVal, b.optVal);
  }

  bool operator()() const { return false; }

  bool operator()(SArray a, SArray b) const {
    return a == b;
  }
  bool operator()(const DArrLikePacked& a, const DArrLikePacked& b) const {
    return subtypePacked(a, b);
  }
  bool operator()(const DArrLikePackedN& a, const DArrLikePackedN& b) const {
    return subtype(a.type, b.type);
  }
  bool operator()(const DArrLikeMap& a, const DArrLikeMap& b) const {
    return subtypeMap(a, b);
  }
  bool operator()(const DArrLikeMapN& a, const DArrLikeMapN& b) const {
    return subtype(a.key, b.key) && subtype(a.val, b.val);
  }

  bool operator()(const DArrLikeMap&, SArray) const {
    // A map (even with all constant values) is never considered a
    // subtype of a static array.
    return false;
  }

  bool operator()(SArray a, const DArrLikeMap& b) const {
    // If the map doesn't have optional elements, its values are
    // disjoint from the static array if the keys are of different
    // sizes.
    if (!b.hasOptElements() && a->size() != b.map.size()) return false;
    auto const m = toDArrLikeMap(a);
    return m && subtypeMap(*m, b);
  }

  bool operator()(SArray a, const DArrLikePacked& b) const {
    if (a->size() != b.elems.size()) return false;
    auto const p = toDArrLikePacked(a);
    return p && subtypePacked(*p, b);
  }

  bool operator()(const DArrLikePacked& a, SArray b) const {
    // A packed array (even with all constant values) is never
    // considered a subtype of a static array.
    return false;
  }

  bool operator()(const DArrLikePackedN& a, const DArrLikeMapN& b) const {
    return b.key.couldBe(BInt) && subtype(a.type, b.val);
  }

  bool operator()(const DArrLikePacked& a, const DArrLikeMapN& b) const {
    if (!b.key.couldBe(packed_key(a))) return false;
    for (auto const& v : a.elems) {
      if (!subtype(v, b.val)) return false;
    }
    return true;
  }

  bool operator()(const DArrLikeMap& a, const DArrLikeMapN& b) const {
    for (auto const& kv : a.map) {
      if (!subtype(map_key(kv.first, kv.second), b.key)) return false;
      if (!subtype(kv.second.val, b.val)) return false;
    }
    return subtype(a.optKey, b.key) && subtype(a.optVal, b.val);
  }

  bool operator()(SArray a, const DArrLikeMapN& b) const {
    auto bad = false;
    IterateKV(
      a,
      [&] (TypedValue k, TypedValue v) {
        bad |= !(b.key.couldBe(from_cell(k)) && b.val.couldBe(from_cell(v)));
        return bad;
      }
    );
    return !bad;
  }

  bool operator()(const DArrLikePacked& a, const DArrLikePackedN& b) const {
    for (auto const& t : a.elems) {
      if (!subtype(t, b.type)) return false;
    }
    return true;
  }

  bool operator()(SArray a, const DArrLikePackedN& b) const {
    assertx(!a->empty());
    auto p = toDArrLikePackedN(a);
    return p && subtype(p->type, b.type);
  }

  bool operator()(const DArrLikeNone&, SArray) const {
    return false;
  }
  bool operator()(const DArrLikeNone&, const DArrLikeMap&) const {
    return false;
  }
  bool operator()(const DArrLikeNone&, const DArrLikePacked&) const {
    return false;
  }

  bool operator()(const DArrLikeNone& a, const DArrLikePackedN& b) const {
    if (!subtypeAmong(a.bits, BVecN, BArrLikeN)) return false;
    auto const val = Type{allowedValBits(a.bits, true).first};
    return val.subtypeOf(b.type);
  }
  bool operator()(const DArrLikeNone& a, const DArrLikeMapN& b) const {
    auto const key = Type{allowedKeyBits(a.bits).first};
    auto const val = Type{allowedValBits(a.bits, false).first};
    return
      key.subtypeOf(b.key) &&
      val.subtypeOf(b.val);
  }

  bool operator()(const DArrLikePackedN&, const DArrLikePacked&) const {
    // PackedN contains arrays with an arbitrary number of keys, while Packed
    // contains arrays with a fixed number of keys, so there's always arrays in
    // PackedN which aren't in Packed.
    return false;
  }
  bool operator()(const DArrLikePackedN&, SArray) const {
    // PackedN contains arrays with an arbitrary number of keys, while SArray is
    // just a single array.
    return false;
  }
  bool operator()(const DArrLikeMap&, const DArrLikePacked&) const {
    // Map does not contain any packed arrays.
    return false;
  }
  bool operator()(const DArrLikeMap&, const DArrLikePackedN&) const {
    // Map does not contain any packed arrays.
    return false;
  }
  bool operator()(const DArrLikePacked&, const DArrLikeMap&) const {
    // Map does not contain any packed arrays.
    return false;
  }
  bool operator()(const DArrLikePackedN&, const DArrLikeMap&) const {
    // Map does not contain any packed arrays.
    return false;
  }
  bool operator()(const DArrLikeMapN&, const DArrLikePackedN&) const {
    // MapN will always contain more arrays than PackedN because packed arrays
    // are a subset of all possible arrays.
    return false;
  }
  bool operator()(const DArrLikeMapN&, const DArrLikePacked&) const {
    // MapN contains arrays with an arbitrary number of keys, while Packed
    // contains arrays with a fixed number of keys, so there's always arrays in
    // MapN which aren't in Packed.
    return false;
  }
  bool operator()(const DArrLikeMapN&, const DArrLikeMap&) const {
    // MapN contains arrays with an arbitrary number of keys, while Map contains
    // arrays with a fixed number of keys, so there's always arrays in MapN
    // which aren't in Map.
    return false;
  }
  bool operator()(const DArrLikeMapN&, SArray) const {
    // MapN contains arrays with an arbitrary number of keys, while SArray is
    // just a single array.
    return false;
  }
};

using DualDispatchCouldBe      = Commute<DualDispatchCouldBeImpl>;
using DualDispatchUnion        = Commute<DualDispatchUnionImpl>;
using DualDispatchIntersection = Commute<DualDispatchIntersectionImpl>;

//////////////////////////////////////////////////////////////////////
// Helpers for creating literal array-like types

template<typename AInit, bool force_static, bool allow_counted>
Optional<TypedValue> fromTypeVec(const std::vector<Type>& elems,
                                 trep bits,
                                 LegacyMark mark) {
  mark = legacyMark(mark, bits);
  if (mark == LegacyMark::Unknown) return std::nullopt;

  AInit ai{elems.size()};
  for (auto const& t : elems) {
    auto const v = allow_counted ? tvCounted(t) : tv(t);
    if (!v) return std::nullopt;
    ai.append(tvAsCVarRef(&*v));
  }
  auto var = ai.toVariant();

  if (mark == LegacyMark::Marked) {
    var.asArrRef().setLegacyArray(true);
  }

  if (force_static) var.setEvalScalar();
  return tvReturn(std::move(var));
}

template<bool allow_counted>
bool checkTypeVec(const std::vector<Type>& elems, trep bits, LegacyMark mark) {
  if (legacyMark(mark, bits) == LegacyMark::Unknown) return false;
  for (auto const& t : elems) {
    if (allow_counted ? !is_scalar_counted(t) : !is_scalar(t)) return false;
  }
  return true;
}

Variant keyHelper(SString key) {
  return Variant{ key, Variant::PersistentStrInit{} };
}
const Variant& keyHelper(const TypedValue& v) {
  return tvAsCVarRef(&v);
}
template <typename AInit>
void add(AInit& ai, const Variant& key, const Variant& value) {
  ai.setValidKey(key, value);
}
void add(KeysetInit& ai, const Variant& key, const Variant& value) {
  assertx(tvSame(*key.asTypedValue(), *value.asTypedValue()));
  ai.add(key);
}

template<typename AInit, bool force_static, bool allow_counted>
Optional<TypedValue> fromTypeMap(const MapElems& elems,
                                 trep bits,
                                 LegacyMark mark) {
  mark = legacyMark(mark, bits);
  if (mark == LegacyMark::Unknown) return std::nullopt;

  auto val = eval_cell_value([&] () -> TypedValue {
    AInit ai{elems.size()};
    for (auto const& elm : elems) {
      if (!allow_counted && elm.second.keyStaticness == TriBool::No) {
        return make_tv<KindOfUninit>();
      }
      auto const v =
        allow_counted ? tvCounted(elm.second.val) : tv(elm.second.val);
      if (!v) return make_tv<KindOfUninit>();
      add(ai, keyHelper(elm.first), tvAsCVarRef(&*v));
    }
    auto var = ai.toVariant();

    if (mark == LegacyMark::Marked) {
      var.asArrRef().setLegacyArray(true);
    }

    if (force_static) var.setEvalScalar();
    return tvReturn(std::move(var));
  });
  if (val && val->m_type == KindOfUninit) val.reset();
  return val;
}

template<bool allow_counted>
bool checkTypeMap(const MapElems& elems, trep bits, LegacyMark mark) {
  if (legacyMark(mark, bits) == LegacyMark::Unknown) return false;
  for (auto const& elem : elems) {
    if (!allow_counted && elem.second.keyStaticness == TriBool::No) {
      return false;
    }
    if (allow_counted
        ? !is_scalar_counted(elem.second.val)
        : !is_scalar(elem.second.val)) {
      return false;
    }
  }
  return true;
}

struct KeysetAppendInit : KeysetInit {
  using KeysetInit::KeysetInit;
  KeysetAppendInit& append(const Variant& v) {
    add(*v.asTypedValue());
    return *this;
  }
};

//////////////////////////////////////////////////////////////////////

}

//////////////////////////////////////////////////////////////////////

// We ref-count the IsectSet, so multiple copies of a DCls can share
// it. This is basically copy_ptr, but we can't use that easily with
// our CompactTaggedPtr representation.
struct DCls::IsectWrapper {
  IsectSet isects;
  std::atomic<uint32_t> refcount{1};
  void acquire() { refcount.fetch_add(1, std::memory_order_acq_rel); }
  void release() {
    if (refcount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      delete this;
    }
  }
};

struct DCls::IsectAndExactWrapper {
  res::Class exact;
  IsectSet isects;
  std::atomic<uint32_t> refcount{1};
  void acquire() { refcount.fetch_add(1, std::memory_order_acq_rel); }
  void release() {
    if (refcount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      delete this;
    }
  }
};

DCls::DCls(const DCls& o) : val{o.val} {
  if (isIsect()) {
    rawIsect()->acquire();
  } else if (isIsectAndExact()) {
    rawIsectAndExact()->acquire();
  }
}

DCls::~DCls() {
  if (isIsect()) {
    rawIsect()->release();
  } else if (isIsectAndExact()) {
    rawIsectAndExact()->release();
  }
}

DCls& DCls::operator=(const DCls& o) {
  if (this == &o) return *this;

  auto const copy = [&] {
    if (o.isIsect()) {
      o.rawIsect()->acquire();
    } else if (o.isIsectAndExact()) {
      o.rawIsectAndExact()->acquire();
    }
    val = o.val;
  };

  if (isIsect()) {
    auto const i = rawIsect();
    copy();
    i->release();
  } else if (isIsectAndExact()) {
    auto const i = rawIsectAndExact();
    copy();
    i->release();
  }

  return *this;
}

DCls DCls::MakeExact(res::Class cls, bool nonReg) {
  return DCls{
    nonReg ? TagExact : Tag(TagExact | TagReg),
    (void*)cls.toOpaque()
  };
}

DCls DCls::MakeSub(res::Class cls, bool nonReg) {
  return DCls{
    nonReg ? TagNone : TagReg,
    (void*)cls.toOpaque()
  };
}

DCls DCls::MakeIsect(IsectSet isect, bool nonReg) {
  assertx(isect.size() > 1);
  auto w = new IsectWrapper{std::move(isect)};
  return DCls{
    nonReg ? TagIsect : Tag(TagIsect | TagReg),
    (void*)w
  };
}

DCls DCls::MakeIsectAndExact(res::Class exact, IsectSet isect, bool nonReg) {
  assertx(!isect.empty());
  assertx(exact.isSerialized() || exact.isMissingDebug());
  auto w = new IsectAndExactWrapper{exact, std::move(isect)};
  return DCls{
    nonReg ? Tag(TagIsect | TagExact) : Tag(TagIsect | TagExact | TagReg),
    (void*)w
  };
}

res::Class DCls::cls() const {
  assertx(!isIsect() && !isIsectAndExact());
  assertx(val.ptr());
  return res::Class::fromOpaque((uintptr_t)val.ptr());
}

res::Class DCls::smallestCls() const {
  return isIsect()
    ? isect().front()
    : (isIsectAndExact() ? isectAndExact().first : cls());
}

const DCls::IsectSet& DCls::isect() const {
  return rawIsect()->isects;
}

std::pair<res::Class, const DCls::IsectSet*> DCls::isectAndExact() const {
  auto const r = rawIsectAndExact();
  return std::make_pair(r->exact, &r->isects);
}

void DCls::setCls(res::Class cls) {
  assertx(!isIsect() && !isIsectAndExact());
  val.set(val.tag(), (void*)cls.toOpaque());
}

bool DCls::same(const DCls& o, bool checkCtx) const {
  if (checkCtx) {
    if (val.tag() != o.val.tag()) return false;
  } else {
    if (removeCtx(val.tag()) != removeCtx(o.val.tag())) return false;
  }

  if (isIsect()) {
    auto const& isect1 = isect();
    auto const& isect2 = o.isect();
    if (&isect1 == &isect2) return true;
    if (isect1.size() != isect2.size()) return false;
    return std::equal(
      begin(isect1), end(isect1),
      begin(isect2), end(isect2),
      [] (res::Class c1, res::Class c2) { return c1.same(c2); }
    );
  } else if (isIsectAndExact()) {
    auto const [exact1, isect1] = isectAndExact();
    auto const [exact2, isect2] = o.isectAndExact();
    if (!exact1.same(exact2)) return false;
    if (isect1 == isect2) return true;
    if (isect1->size() != isect2->size()) return false;
    return std::equal(
      begin(*isect1), end(*isect1),
      begin(*isect2), end(*isect2),
      [] (res::Class c1, res::Class c2) { return c1.same(c2); }
    );
  } else {
    return cls().same(o.cls());
  }
}

DCls::IsectWrapper* DCls::rawIsect() const {
  assertx(isIsect());
  assertx(val.ptr());
  return (IsectWrapper*)val.ptr();
}

DCls::IsectAndExactWrapper* DCls::rawIsectAndExact() const {
  assertx(isIsectAndExact());
  assertx(val.ptr());
  return (IsectAndExactWrapper*)val.ptr();
}

void DCls::serde(BlobEncoder& sd) const {
  sd(val.tag());
  if (isIsect()) {
    sd(isect());
  } else if (isIsectAndExact()) {
    auto [e, i] = isectAndExact();
    sd(e);
    sd(*i);
  } else {
    sd(cls());
  }
}
void DCls::serde(BlobDecoder& sd) {
  auto const tag = sd.template make<decltype(val.tag())>();
  if (tagIsIsect(tag)) {
    auto i = sd.make<IsectSet>();
    val.set(tag, new IsectWrapper{std::move(i)});
  } else if (tagIsIsectAndExact(tag)) {
    auto e = sd.make<res::Class>();
    auto i = sd.make<IsectSet>();
    val.set(tag, new IsectAndExactWrapper{e, std::move(i)});
  } else {
    auto c = sd.make<res::Class>();
    val.set(tag, (void*)c.toOpaque());
  }
}

//////////////////////////////////////////////////////////////////////

MapElem MapElem::KeyFromType(const Type& key, Type val) {
  assertx(is_scalar_counted(key));
  assertx(is_specialized_int(key) || is_specialized_string(key));
  return MapElem{
    std::move(val),
    !key.couldBe(BCounted)
      ? TriBool::Yes
      : (!key.couldBe(BUnc) ? TriBool::No : TriBool::Maybe)
  };
}

//////////////////////////////////////////////////////////////////////
// Helpers for managing context types.

Type Type::unctxHelper(Type t, bool& changed) {
  changed = false;

  switch (t.m_dataTag) {
  case DataTag::Obj:
    if (t.m_data.dobj.isCtx()) {
      t.m_data.dobj.setCtx(false);
      changed = true;
    }
    break;
  case DataTag::WaitHandle: {
    auto const clsChanged = t.m_data.dwh->cls.isCtx();
    if (clsChanged) t.m_data.dwh.mutate()->cls.setCtx(false);
    bool innerChanged;
    auto const& inner = t.m_data.dwh->inner;
    auto ty = unctxHelper(inner, innerChanged);
    if (innerChanged) t.m_data.dwh.mutate()->inner = std::move(ty);
    changed = clsChanged || innerChanged;
    break;
  }
  case DataTag::Cls:
    if (t.m_data.dcls.isCtx()) {
      t.m_data.dcls.setCtx(false);
      changed = true;
    }
    break;
  case DataTag::ArrLikePacked: {
    auto const packed = t.m_data.packed.get();
    HPHP::HHBBC::DArrLikePacked* mutated = nullptr;
    for (size_t i = 0; i < packed->elems.size(); ++i) {
      bool c;
      const auto ty = unctxHelper(packed->elems[i], c);
      if (c) {
        if (!mutated) {
          changed = true;
          mutated = t.m_data.packed.mutate();
        }
        mutated->elems[i] = ty;
      }
    }
    break;
  }
  case DataTag::ArrLikePackedN: {
    auto const packedn = t.m_data.packedn.get();
    auto ty = unctxHelper(packedn->type, changed);
    if (changed) {
      t.m_data.packedn.mutate()->type = ty;
    }
    break;
  }
  case DataTag::ArrLikeMap: {
    auto const map = t.m_data.map.get();
    size_t offset = 0;
    HPHP::HHBBC::DArrLikeMap* mutated = nullptr;
    for (auto it = map->map.begin(); it != map->map.end(); ++it) {
      auto const ty = unctxHelper(it->second.val, changed);
      if (changed) {
        offset = std::distance(it, map->map.begin());
        mutated = t.m_data.map.mutate();
        break;
      }
    }
    if (mutated) {
      auto it = mutated->map.begin();
      for (std::advance(it, offset);
           it != mutated->map.end();
           ++it) {
        bool c;
        auto ty = unctxHelper(it->second.val, c);
        if (c) {
          mutated->map.update(it, it->second.withType(std::move(ty)));
        }
      }
    }
    bool changed2;
    auto ty = unctxHelper(t.m_data.map->optVal, changed2);
    if (changed2) t.m_data.map.mutate()->optVal = std::move(ty);
    changed |= changed2;
    break;
  }
  case DataTag::ArrLikeMapN: {
    auto const mapn = t.m_data.mapn.get();
    auto ty = unctxHelper(mapn->val, changed);
    if (changed) t.m_data.mapn.mutate()->val = std::move(ty);
    break;
  }
  case DataTag::None:
  case DataTag::Int:
  case DataTag::Dbl:
  case DataTag::Str:
  case DataTag::ArrLikeVal:
  case DataTag::LazyCls:
  case DataTag::EnumClassLabel:
    break;
  }
  return t;
}

//////////////////////////////////////////////////////////////////////

Type& Type::operator=(const Type& o) noexcept {
  SCOPE_EXIT { assertx(checkInvariants()); };
  if (this == &o) return *this;
  destroy(*this);
  construct(*this, o);
  return *this;
}

Type& Type::operator=(Type&& o) noexcept {
  SCOPE_EXIT { assertx(checkInvariants());
               assertx(o.checkInvariants()); };
  if (this == &o) return *this;
  destroy(*this);
  construct(*this, std::move(o));
  return *this;
}

const Type& Type::operator |= (const Type& other) {
  *this = union_of(std::move(*this), other);
  return *this;
}

const Type& Type::operator |= (Type&& other) {
  *this = union_of(std::move(*this), std::move(other));
  return *this;
}

const Type& Type::operator &= (const Type& other) {
  *this = intersection_of(std::move(*this), other);
  return *this;
}

const Type& Type::operator &= (Type&& other) {
  *this = intersection_of(std::move(*this), std::move(other));
  return *this;
}

//////////////////////////////////////////////////////////////////////

void Type::copyData(const Type& o) {
  assertx(m_dataTag != DataTag::None);
  switch (m_dataTag) {
    case DataTag::None:   not_reached();
    #define DT(tag_name,type,name)              \
      case DataTag::tag_name:                   \
        construct(m_data.name, o.m_data.name);  \
        return;
    DATATAGS
    #undef DT
  }
  not_reached();
}

void Type::moveData(Type&& o) {
  assertx(m_dataTag != DataTag::None);
  o.m_dataTag = DataTag::None;
  switch (m_dataTag) {
    case DataTag::None:   not_reached();
    #define DT(tag_name,type,name)                              \
      case DataTag::tag_name:                                   \
        construct(m_data.name, std::move(o.m_data.name));       \
        return;
    DATATAGS
    #undef DT
  }
  not_reached();
}

void Type::destroyData() {
  assertx(m_dataTag != DataTag::None);
  switch (m_dataTag) {
    case DataTag::None: not_reached();
    #define DT(tag_name,type,name)              \
      case DataTag::tag_name:                   \
        destroy(m_data.name);                   \
        return;
    DATATAGS
    #undef DT
  }
  not_reached();
}

//////////////////////////////////////////////////////////////////////

template<typename Ret, typename T, typename Function>
struct Type::DDHelperFn {
  template <class Y>
  typename std::enable_if<(!std::is_same<Y,T>::value || !Function::disjoint) &&
                          std::is_same<typename DataTagTrait<Y>::tag,
                                       typename DataTagTrait<T>::tag>::value,
                          Ret>::type
  operator()(const Y& y) const { return f(t, y); }

  template <class Y>
  typename std::enable_if<(std::is_same<Y,T>::value && Function::disjoint) &&
                          std::is_same<typename DataTagTrait<Y>::tag,
                                       typename DataTagTrait<T>::tag>::value,
                          Ret>::type
  operator()(const Y&) const { not_reached(); }

  template <class Y>
  typename std::enable_if<!std::is_same<typename DataTagTrait<Y>::tag,
                                        typename DataTagTrait<T>::tag>::value,
                          Ret>::type
  operator()(const Y&) const { return f(); }

  Ret operator()() const { return f(); }
  Function f;
  const T& t;
};

template<typename Ret, typename T, typename Function>
Type::DDHelperFn<Ret,T,Function> Type::ddbind(const Function& f,
                                              const T& t) const {
  return { f, t };
}

// Dispatcher for the second argument for dualDispatchDataFn.
template<typename Ret, typename T, typename F>
Ret Type::dd2nd(const Type& o, DDHelperFn<Ret,T,F> f) const {
  switch (o.m_dataTag) {
  case DataTag::None:           not_reached();
  case DataTag::Int:            return f();
  case DataTag::Dbl:            return f();
  case DataTag::Cls:            return f();
  case DataTag::Str:            return f();
  case DataTag::LazyCls:        return f();
  case DataTag::EnumClassLabel: return f();
  case DataTag::Obj:            return f();
  case DataTag::WaitHandle:     return f();
  case DataTag::ArrLikeVal:     return f(o.m_data.aval);
  case DataTag::ArrLikePacked:  return f(*o.m_data.packed);
  case DataTag::ArrLikePackedN: return f(*o.m_data.packedn);
  case DataTag::ArrLikeMap:     return f(*o.m_data.map);
  case DataTag::ArrLikeMapN:    return f(*o.m_data.mapn);
  }
  not_reached();
}

/*
 * Dual-dispatch on (this->m_dataTag, o.m_dataTag)
 *
 * See the DualDispatch* classes above
 */
template<typename F>
typename F::result_type
Type::dualDispatchDataFn(const Type& o, F f) const {
  using R = typename F::result_type;
  switch (m_dataTag) {
  case DataTag::None:           not_reached();
  case DataTag::Int:            return f();
  case DataTag::Dbl:            return f();
  case DataTag::Cls:            return f();
  case DataTag::Str:            return f();
  case DataTag::LazyCls:        return f();
  case DataTag::EnumClassLabel: return f();
  case DataTag::Obj:            return f();
  case DataTag::WaitHandle:     return f();
  case DataTag::ArrLikeVal:     return dd2nd(o, ddbind<R>(f, m_data.aval));
  case DataTag::ArrLikePacked:  return dd2nd(o, ddbind<R>(f, *m_data.packed));
  case DataTag::ArrLikePackedN: return dd2nd(o, ddbind<R>(f, *m_data.packedn));
  case DataTag::ArrLikeMap:     return dd2nd(o, ddbind<R>(f, *m_data.map));
  case DataTag::ArrLikeMapN:    return dd2nd(o, ddbind<R>(f, *m_data.mapn));
  }
  not_reached();
}

/*
 * Like dualDispatchDataFn, but use DArrLikeNone for *this.
 */
template<typename Function>
typename Function::result_type
Type::dispatchArrLikeNone(const Type& o, Function f) const {
  assertx(couldBe(BArrLikeN));
  assertx(!is_specialized_array_like(*this));
  return dd2nd(
    o,
    ddbind<typename Function::result_type>(f, DArrLikeNone { bits() })
  );
}

//////////////////////////////////////////////////////////////////////

template<bool contextSensitive>
bool Type::equivImpl(const Type& o) const {
  if (bits() != o.bits()) return false;
  if (m_legacyMark != o.m_legacyMark) return false;
  if (hasData() != o.hasData()) return false;
  if (!hasData()) return true;

  if (m_dataTag != o.m_dataTag) return false;

  switch (m_dataTag) {
  case DataTag::None:
    not_reached();
  case DataTag::Str:
    assertx(m_data.sval->isStatic());
    assertx(o.m_data.sval->isStatic());
    return m_data.sval == o.m_data.sval;
  case DataTag::LazyCls:
    assertx(m_data.lazyclsval->isStatic());
    assertx(o.m_data.lazyclsval->isStatic());
    return m_data.lazyclsval == o.m_data.lazyclsval;
  case DataTag::EnumClassLabel:
    assertx(m_data.eclval->isStatic());
    assertx(o.m_data.eclval->isStatic());
    return m_data.eclval == o.m_data.eclval;
  case DataTag::ArrLikeVal:
    assertx(m_data.aval->isStatic());
    assertx(o.m_data.aval->isStatic());
    return m_data.aval == o.m_data.aval;
  case DataTag::Int:
    return m_data.ival == o.m_data.ival;
  case DataTag::Dbl:
    return double_equals(m_data.dval, o.m_data.dval);
  case DataTag::Obj:
    return m_data.dobj.same(o.m_data.dobj, contextSensitive);
  case DataTag::WaitHandle:
    return
      m_data.dwh->cls.same(o.m_data.dwh->cls, contextSensitive) &&
      m_data.dwh->canSuspend == o.m_data.dwh->canSuspend &&
      m_data.dwh->inner.equivImpl<contextSensitive>(o.m_data.dwh->inner);
  case DataTag::Cls:
    return m_data.dcls.same(o.m_data.dcls, contextSensitive);
  case DataTag::ArrLikePacked:
    if (m_data.packed->elems.size() != o.m_data.packed->elems.size()) {
      return false;
    }
    for (auto i = 0; i <  m_data.packed->elems.size(); i++) {
      if (!m_data.packed->elems[i]
             .equivImpl<contextSensitive>(o.m_data.packed->elems[i])) {
        return false;
      }
    }
    return true;
  case DataTag::ArrLikePackedN:
    return m_data.packedn->type
             .equivImpl<contextSensitive>(o.m_data.packedn->type);
  case DataTag::ArrLikeMap: {
    if (m_data.map->map.size() != o.m_data.map->map.size()) {
      return false;
    }
    auto it = o.m_data.map->map.begin();
    for (auto const& kv : m_data.map->map) {
      if (!tvSame(kv.first, it->first)) return false;
      if (kv.second.keyStaticness != it->second.keyStaticness) return false;
      if (!kv.second.val.equivImpl<contextSensitive>(it->second.val)) {
        return false;
      }
      ++it;
    }
    return
      m_data.map->optKey.equivImpl<contextSensitive>(o.m_data.map->optKey) &&
      m_data.map->optVal.equivImpl<contextSensitive>(o.m_data.map->optVal);
  }
  case DataTag::ArrLikeMapN:
    return m_data.mapn->key.equivImpl<contextSensitive>(o.m_data.mapn->key) &&
           m_data.mapn->val.equivImpl<contextSensitive>(o.m_data.mapn->val);
  }
  not_reached();
}

bool Type::equal(const Type& o) const {
  return equivImpl<true>(o);
}

bool Type::equalNoContext(const Type& o) const {
  return equivImpl<false>(o);
}

size_t Type::hash() const {
  using U1 = std::underlying_type<trep>::type;
  using U2 = std::underlying_type<decltype(m_dataTag)>::type;
  auto const rawBits = U1{bits()};
  auto const rawTag  = static_cast<U2>(m_dataTag);

  auto const data =
    [&] () -> uintptr_t {
      switch (m_dataTag) {
        case DataTag::None:
          return 0;
        case DataTag::Obj: {
          if (m_data.dobj.isExact() || m_data.dobj.isSub()) {
            return folly::hash::hash_combine(
              m_data.dobj.cls().hash(),
              m_data.dobj.containsNonRegular()
            );
          }
          if (m_data.dobj.isIsect()) {
            return folly::hash::hash_range(
              begin(m_data.dobj.isect()),
              end(m_data.dobj.isect()),
              m_data.dobj.containsNonRegular(),
              [] (res::Class c) { return c.hash(); }
            );
          }
          assertx(m_data.dobj.isIsectAndExact());
          auto const [e, i] = m_data.dobj.isectAndExact();
          return folly::hash::hash_range(
            begin(*i), end(*i),
            folly::hash::hash_combine(
              e.hash(),
              m_data.dobj.containsNonRegular()
            ),
            [] (res::Class c) { return c.hash(); }
          );
        }
        case DataTag::WaitHandle:
          return folly::hash::hash_combine(
            m_data.dwh->cls.smallestCls().hash(),
            m_data.dwh->inner.hash(),
            m_data.dwh->canSuspend
          );
        case DataTag::Cls: {
          if (m_data.dcls.isExact() || m_data.dcls.isSub()) {
            return folly::hash::hash_combine(
              m_data.dcls.cls().hash(),
              m_data.dcls.containsNonRegular()
            );
          }
          if (m_data.dcls.isIsect()) {
            return folly::hash::hash_range(
              begin(m_data.dcls.isect()),
              end(m_data.dcls.isect()),
              m_data.dcls.containsNonRegular(),
              [] (res::Class c) { return c.hash(); }
            );
          }
          assertx(m_data.dcls.isIsectAndExact());
          auto const [e, i] = m_data.dcls.isectAndExact();
          return folly::hash::hash_range(
            begin(*i), end(*i),
            folly::hash::hash_combine(
              e.hash(),
              m_data.dcls.containsNonRegular()
            ),
            [] (res::Class c) { return c.hash(); }
          );
        }
        case DataTag::Str:
          return (uintptr_t)m_data.sval;
        case DataTag::LazyCls:
          return (uintptr_t)m_data.lazyclsval;
        case DataTag::EnumClassLabel:
          return (uintptr_t)m_data.eclval;
        case DataTag::Int:
          return m_data.ival;
        case DataTag::Dbl:
          return std::hash<double>{}(m_data.dval);
        case DataTag::ArrLikeVal:
          return (uintptr_t)m_data.aval;
        case DataTag::ArrLikePacked:
          return folly::hash::hash_range(
            m_data.packed->elems.begin(),
            m_data.packed->elems.end(),
            0,
            [] (const Type& t) { return t.hash(); }
          );
        case DataTag::ArrLikePackedN:
          return m_data.packedn->type.hash();
        case DataTag::ArrLikeMap:
          return folly::hash::hash_range(
            m_data.map->map.begin(),
            m_data.map->map.end(),
            folly::hash::hash_combine(
              m_data.map->optKey.hash(),
              m_data.map->optVal.hash()
            ),
            [] (const std::pair<TypedValue, MapElem>& p) {
              return folly::hash::hash_combine(
                map_key(p.first, p.second).hash(),
                p.second.val.hash()
              );
            }
          );
        case DataTag::ArrLikeMapN:
          return folly::hash::hash_combine(
            m_data.mapn->key.hash(),
            m_data.mapn->val.hash()
          );
      }
      not_reached();
    }();

  return folly::hash::hash_combine(rawBits, rawTag, data);
}

template<bool contextSensitive>
bool Type::subtypeOfImpl(const Type& o) const {
  using HPHP::HHBBC::couldBe;

  auto const isect = bits() & o.bits();
  if (isect != bits()) return false;

  if (!legacyMarkSubtypeOf(project(m_legacyMark, isect),
                           project(o.m_legacyMark, isect))) {
    return false;
  }

  // If the (non-empty) intersection cannot contain data, or if the
  // other type has no data, the bits are sufficient.
  if (!couldBe(isect, kSupportBits)) return true;
  if (!o.hasData()) return true;

  // For any specialized data in the other type which is supported by
  // the intersection, there must be the same specialization in *this
  // which is equal or more specific.

  if (couldBe(isect, BObj) && is_specialized_obj(o)) {
    // For BObj, it could be a wait handle or a specialized object, so
    // we must convert between them to match.
    if (!is_specialized_obj(*this)) return false;

    if (is_specialized_wait_handle(*this)) {
      if (is_specialized_wait_handle(o)) {
        return
          subtypeCls<contextSensitive>(m_data.dwh->cls, o.m_data.dwh->cls) &&
          canSuspendSubtype(
            m_data.dwh->canSuspend,
            o.m_data.dwh->canSuspend
          ) &&
          m_data.dwh->inner.subtypeOfImpl<contextSensitive>(
            o.m_data.dwh->inner
          );
      }
      return subtypeCls<contextSensitive>(m_data.dwh->cls, o.m_data.dobj);
    } else if (is_specialized_wait_handle(o)) {
      // Every materialized DWaitHandle contains information which DObj does
      // not carry (the inner type, eagerness, or both).
      return false;
    } else {
      return subtypeCls<contextSensitive>(m_data.dobj, o.m_data.dobj);
    }
  }

  if (couldBe(isect, BCls) && is_specialized_cls(o)) {
    return
      is_specialized_cls(*this) &&
      subtypeCls<contextSensitive>(m_data.dcls, o.m_data.dcls);
  }

  if (couldBe(isect, BArrLikeN) && is_specialized_array_like(o)) {
    if (is_specialized_array_like(*this)) {
      return dualDispatchDataFn(o, DualDispatchSubtype<contextSensitive>{});
    }
    return dispatchArrLikeNone(o, DualDispatchSubtype<contextSensitive>{});
  }

  if (couldBe(isect, BStr) && is_specialized_string(o)) {
    return
      is_specialized_string(*this) &&
      m_data.sval == o.m_data.sval;
  }

  if (couldBe(isect, BLazyCls) && is_specialized_lazycls(o)) {
    return
      is_specialized_lazycls(*this) &&
      m_data.lazyclsval == o.m_data.lazyclsval;
  }

  if (couldBe(isect, BEnumClassLabel) && is_specialized_ecl(o)) {
    return
      is_specialized_ecl(*this) &&
      m_data.eclval == o.m_data.eclval;
  }

  if (couldBe(isect, BInt) && is_specialized_int(o)) {
    return
      is_specialized_int(*this) &&
      m_data.ival == o.m_data.ival;
  }

  if (couldBe(isect, BDbl) && is_specialized_double(o)) {
    return
      is_specialized_double(*this) &&
      double_equals(m_data.dval, o.m_data.dval);
  }

  return true;
}

bool Type::moreRefined(const Type& o) const {
  assertx(checkInvariants());
  assertx(o.checkInvariants());
  return subtypeOfImpl<true>(o);
}

bool Type::strictlyMoreRefined(const Type& o) const {
  assertx(checkInvariants());
  assertx(o.checkInvariants());
  return subtypeOfImpl<true>(o) &&
         !equivImpl<true>(o);
}

bool Type::subtypeOf(const Type& o) const {
  assertx(checkInvariants());
  assertx(o.checkInvariants());
  return subtypeOfImpl<false>(o);
}

bool Type::strictSubtypeOf(const Type& o) const {
  assertx(checkInvariants());
  assertx(o.checkInvariants());
  return !equal_no_context(*this, o) && subtypeOf(o);
}

bool Type::couldBe(const Type& o) const {
  using HPHP::HHBBC::subtypeOf;

  assertx(checkInvariants());
  assertx(o.checkInvariants());

  auto const isect = bits() & o.bits();
  if (!isect) return false;

  if (!legacyMarkCouldBe(project(m_legacyMark, isect),
                         project(o.m_legacyMark, isect))) {
    return false;
  }

  // If the intersection has any non-supported bits, we can simply
  // ignore all specializations. Regardless if the specializations
  // match or not, we know there's a bit unaffected, so they could be
  // each other.
  if (!subtypeOf(isect, kSupportBits)) return true;
  if (!hasData() && !o.hasData()) return true;

  // Specialized data can only make couldBe be false if both sides
  // have the same specialization which has disjoint
  // values. Specializations not supported by the intersection are
  // ignored.

  if (subtypeOf(isect, BObj)) {
    if (!is_specialized_obj(*this) || !is_specialized_obj(o)) return true;

    if (is_specialized_wait_handle(*this)) {
      if (is_specialized_wait_handle(o)) {
        return couldBeCls(m_data.dwh->cls, o.m_data.dwh->cls);
      }
      return couldBeCls(m_data.dwh->cls, o.m_data.dobj);
    } else if (is_specialized_wait_handle(o)) {
      return couldBeCls(m_data.dobj, o.m_data.dwh->cls);
    } else {
      return couldBeCls(m_data.dobj, o.m_data.dobj);
    }
  }

  if (subtypeOf(isect, BCls)) {
    if (!is_specialized_cls(*this) || !is_specialized_cls(o)) return true;
    return couldBeCls(m_data.dcls, o.m_data.dcls);
  }

  if (subtypeOf(isect, BArrLikeN)) {
    if (is_specialized_array_like(*this)) {
      if (is_specialized_array_like(o)) {
        return dualDispatchDataFn(o, DualDispatchCouldBe{isect});
      }
      return o.dispatchArrLikeNone(*this, DualDispatchCouldBe{isect});
    } else if (is_specialized_array_like(o)) {
      return dispatchArrLikeNone(o, DualDispatchCouldBe{isect});
    }
    return true;
  }

  if (subtypeOf(isect, BStr)) {
    if (!is_specialized_string(*this) || !is_specialized_string(o)) return true;
    return m_data.sval == o.m_data.sval;
  }

  if (subtypeOf(isect, BLazyCls)) {
    if (!is_specialized_lazycls(*this) || !is_specialized_lazycls(o)) {
      return true;
    }
    return m_data.lazyclsval == o.m_data.lazyclsval;
  }

  if (subtypeOf(isect, BEnumClassLabel)) {
    if (!is_specialized_ecl(*this) || !is_specialized_ecl(o)) {
      return true;
    }
    return m_data.eclval == o.m_data.eclval;
  }

  if (subtypeOf(isect, BInt)) {
    if (!is_specialized_int(*this) || !is_specialized_int(o)) return true;
    return m_data.ival == o.m_data.ival;
  }

  if (subtypeOf(isect, BDbl)) {
    if (!is_specialized_double(*this) || !is_specialized_double(o)) return true;
    return double_equals(m_data.dval, o.m_data.dval);
  }

  return true;
}

bool Type::checkInvariants() const {
  if (!debug) return true;

  SCOPE_ASSERT_DETAIL("checkInvariants") { return show(*this); };

  constexpr const size_t kMaxArrayCheck = 100;

  // NB: Avoid performing operations which can trigger recursive
  // checkInvariants() calls, which can cause exponential time
  // blow-ups. Try to stick with bit manipulations and avoid more
  // complicated operators.

  assertx(subtypeOf(BTop));
  assertx(subtypeOf(BCell) || bits() == BTop);
  assertx(IMPLIES(bits() == BTop, !hasData()));

  if (HPHP::HHBBC::couldBe(bits(), kLegacyMarkBits)) {
    assertx(m_legacyMark != LegacyMark::Bottom);
  } else {
    assertx(m_legacyMark == LegacyMark::Bottom);
  }

  assertx(IMPLIES(hasData(), couldBe(kSupportBits)));
  assertx(IMPLIES(subtypeOf(kNonSupportBits), !hasData()));

  switch (m_dataTag) {
  case DataTag::None:
    break;
  case DataTag::Str:
    assertx(m_data.sval->isStatic());
    assertx(couldBe(BStr));
    assertx(subtypeOf(BStr | kNonSupportBits));
    break;
  case DataTag::LazyCls:
    assertx(m_data.lazyclsval->isStatic());
    assertx(couldBe(BLazyCls));
    assertx(subtypeOf(BLazyCls | kNonSupportBits));
    break;
  case DataTag::EnumClassLabel:
    assertx(m_data.eclval->isStatic());
    assertx(couldBe(BEnumClassLabel));
    assertx(subtypeOf(BEnumClassLabel | kNonSupportBits));
    break;
  case DataTag::Dbl:
    assertx(couldBe(BDbl));
    assertx(subtypeOf(BDbl | kNonSupportBits));
    break;
  case DataTag::Int:
    assertx(couldBe(BInt));
    assertx(subtypeOf(BInt | kNonSupportBits));
    break;
  case DataTag::Cls:
    assertx(couldBe(BCls));
    assertx(subtypeOf(BCls | kNonSupportBits));
    if (m_data.dcls.isExact()) {
      if (!m_data.dcls.cls().isSerialized()) {
        assertx(
          IMPLIES(
            m_data.dcls.containsNonRegular(),
            m_data.dcls.cls().mightBeNonRegular()
          )
        );
        assertx(
          IMPLIES(
            !m_data.dcls.containsNonRegular(),
            m_data.dcls.cls().mightBeRegular()
          )
        );
      }
    } else if (m_data.dcls.isSub()) {
      if (!m_data.dcls.cls().isSerialized()) {
        assertx(m_data.dcls.cls().couldBeOverridden());
        assertx(
          IMPLIES(
            m_data.dcls.containsNonRegular(),
            m_data.dcls.cls().mightContainNonRegular()
          )
        );
        assertx(
          IMPLIES(
            !m_data.dcls.containsNonRegular(),
            m_data.dcls.cls().couldBeOverriddenByRegular()
          )
        );
      }
    } else if (m_data.dcls.isIsect()) {
      // There's way more things we could verify here, but it gets
      // expensive (and requires the index).
      assertx(m_data.dcls.isect().size() > 1);
      for (auto const DEBUG_ONLY c : m_data.dcls.isect()) {
        if (c.isSerialized()) continue;
        assertx(
          IMPLIES(
            !m_data.dcls.containsNonRegular(),
            c.mightBeRegular() || c.couldBeOverriddenByRegular()
          )
        );
      }
    } else {
      assertx(m_data.dcls.isIsectAndExact());
      // There's way more things we could verify here, but it gets
      // expensive (and requires the index).
      auto const DEBUG_ONLY [e, i] = m_data.dcls.isectAndExact();
      assertx(!i->empty());
      if (!e.isSerialized()) {
        assertx(e.isMissingDebug());
        assertx(IMPLIES(!m_data.dcls.containsNonRegular(),
                        e.mightBeRegular() || e.couldBeOverriddenByRegular()));
      }
      for (auto const DEBUG_ONLY c : *i) {
        if (c.isSerialized()) continue;
        assertx(!c.same(e));
        assertx(!c.hasCompleteChildren());
        assertx(
          IMPLIES(
            !m_data.dcls.containsNonRegular(),
            c.mightBeRegular() || c.couldBeOverriddenByRegular()
          )
        );
      }
    }
    break;
  case DataTag::Obj:
    assertx(couldBe(BObj));
    assertx(subtypeOf(BObj | kNonSupportBits));
    assertx(!m_data.dobj.containsNonRegular());
    if (m_data.dobj.isExact()) {
      if (!m_data.dobj.cls().isSerialized()) {
        assertx(m_data.dobj.cls().mightBeRegular());
      }
    } else if (m_data.dobj.isSub()) {
      if (!m_data.dobj.cls().isSerialized()) {
        assertx(m_data.dobj.cls().couldBeOverriddenByRegular());
      }
    } else if (m_data.dobj.isIsect()) {
      // There's way more things we could verify here, but it gets
      // expensive (and requires the index).
      assertx(m_data.dobj.isect().size() > 1);
      for (auto const DEBUG_ONLY c : m_data.dobj.isect()) {
        if (c.isSerialized()) continue;
        assertx(c.mightBeRegular() || c.couldBeOverriddenByRegular());
      }
    } else {
      assertx(m_data.dcls.isIsectAndExact());
      // There's way more things we could verify here, but it gets
      // expensive (and requires the index).
      auto const DEBUG_ONLY [e, i] = m_data.dcls.isectAndExact();
      assertx(!i->empty());
      if (!e.isSerialized()) {
        assertx(e.isMissingDebug());
        assertx(e.mightBeRegular() || e.couldBeOverriddenByRegular());
      }
      for (auto const DEBUG_ONLY c : *i) {
        if (c.isSerialized()) continue;
        assertx(!c.same(e));
        assertx(!c.hasCompleteChildren());
        assertx(c.mightBeRegular() || c.couldBeOverriddenByRegular());
      }
    }
    break;
  case DataTag::WaitHandle:
    assertx(couldBe(BObj));
    assertx(subtypeOf(BObj | kNonSupportBits));
    assertx(!m_data.dwh.isNull());
    // We need to know something relevant about the inner type or whether the
    // wait handle is guaranteed to be finished if we have a specialization.
    assertx(
      m_data.dwh->inner.strictSubtypeOf(BInitCell) ||
      !m_data.dwh->canSuspend
    );
    assertx(!m_data.dwh->cls.containsNonRegular());
    // DWaitHandle class refinements are only produced by intersecting the
    // canonical HH\\Awaitable specialization with a DObj.
    break;
  case DataTag::ArrLikeVal: {
    assertx(m_data.aval->isStatic());
    assertx(!m_data.aval->empty());
    assertx(couldBe(BArrLikeN));
    assertx(subtypeOf(BArrLikeN | kNonSupportBits));

    // If we know the type is a static array, we should know its
    // (static) specific array type.
    DEBUG_ONLY auto const b = bits() & BArrLikeN;
    switch (m_data.aval->kind()) {
      case ArrayData::kDictKind:
      case ArrayData::kBespokeDictKind:
        assertx(b == BSDictN);
        break;
      case ArrayData::kVecKind:
      case ArrayData::kBespokeVecKind:
        assertx(b == BSVecN);
        break;
      case ArrayData::kKeysetKind:
      case ArrayData::kBespokeKeysetKind:
        assertx(b == BSKeysetN);
        break;
      case ArrayData::kNumKinds:
        always_assert(false);
    }

    // If the array is non-empty, the LegacyMark information should
    // match the static array precisely. This isn't the case if it
    // could be empty since the empty portion could have had different
    // LegaycMark information.
    if (!couldBe(BArrLikeE)) {
      assertx(legacyMarkFromSArr(m_data.aval) == m_legacyMark);
    }

    break;
  }
  case DataTag::ArrLikePacked: {
    assertx(!m_data.packed->elems.empty());
    assertx(couldBe(BArrLikeN));
    assertx(subtypeOf(BArrLikeN | kNonSupportBits));
    if (m_data.packed->elems.size() <= kMaxArrayCheck) {
      DEBUG_ONLY auto const vals = allowedValBits(bits(), true);
      DEBUG_ONLY auto const isKeyset = subtypeAmong(BKeysetN, BArrLikeN);
      DEBUG_ONLY auto const maybeKeyset = couldBe(BKeysetN);
      DEBUG_ONLY auto idx = size_t{0};
      for (DEBUG_ONLY auto const& v : m_data.packed->elems) {
        assertx(!v.is(BBottom));
        assertx(v.subtypeOf(vals.first));
        assertx(v.couldBe(vals.second));
        assertx(IMPLIES(isKeyset, v.equal(ival(idx))));
        assertx(IMPLIES(maybeKeyset, v.couldBe(ival(idx))));
        ++idx;
      }
    }
    break;
  }
  case DataTag::ArrLikeMap: {
    assertx(!m_data.map->map.empty());
    // ArrLikeMap cannot support Vec arrays, since it does not
    // contain any packed arrays.
    assertx(couldBe(BDictN | BKeysetN));
    assertx(subtypeOf(BDictN | BKeysetN | kNonSupportBits));

    DEBUG_ONLY auto const key = allowedKeyBits(bits());
    DEBUG_ONLY auto const val = allowedValBits(bits(), false);
    DEBUG_ONLY auto const isKeyset = subtypeAmong(BKeysetN, BArrLikeN);
    DEBUG_ONLY auto const maybeKeyset = couldBe(BKeysetN);

    if (m_data.map->map.size() <= kMaxArrayCheck) {
      DEBUG_ONLY auto idx = size_t{0};
      DEBUG_ONLY auto packed = true;
      for (DEBUG_ONLY auto const& kv : m_data.map->map) {
        DEBUG_ONLY auto const keyType = map_key(kv.first, kv.second);
        assertx(!kv.second.val.is(BBottom));
        assertx(kv.second.val.subtypeOf(val.first));
        assertx(kv.second.val.couldBe(val.second));
        assertx(keyType.subtypeOf(key.first));
        assertx(keyType.couldBe(key.second));

        if (packed) {
          packed = isIntType(kv.first.m_type) && kv.first.m_data.num == idx;
          ++idx;
        }

        assertx(IMPLIES(isKeyset, keyType.equal(kv.second.val)));
        assertx(IMPLIES(maybeKeyset, keyType.couldBe(kv.second.val)));
      }
      // Map shouldn't have packed-like keys. If it does, it should be Packed
      // instead.
      assertx(!packed);
    }

    // Optional elements are either both Bottom or both not
    assertx(m_data.map->optKey.is(BBottom) ==
            m_data.map->optVal.is(BBottom));
    if (!m_data.map->optKey.is(BBottom)) {
      assertx(m_data.map->optKey.subtypeOf(key.first));
      assertx(m_data.map->optVal.subtypeOf(val.first));
      assertx(IMPLIES(isKeyset, m_data.map->optKey.equal(m_data.map->optVal)));
      // If the optional element has a key with specialized data, it
      // cannot be the same value as a known key.
      if (is_specialized_int(m_data.map->optKey)) {
        DEBUG_ONLY auto const tv =
          make_tv<KindOfInt64>(ival_of(m_data.map->optKey));
        assertx(m_data.map->map.find(tv) == m_data.map->map.end());
      } else if (is_specialized_string(m_data.map->optKey)) {
        DEBUG_ONLY auto const tv =
          make_tv<KindOfString>(sval_of(m_data.map->optKey));
        assertx(m_data.map->map.find(tv) == m_data.map->map.end());
      }
    }
    break;
  }
  case DataTag::ArrLikePackedN: {
    assertx(couldBe(BArrLikeN));
    assertx(subtypeOf(BArrLikeN | kNonSupportBits));
    assertx(!m_data.packedn->type.is(BBottom));
    DEBUG_ONLY auto const vals = allowedValBits(bits(), true);
    DEBUG_ONLY auto const isKeyset = subtypeAmong(BKeysetN, BArrLikeN);
    DEBUG_ONLY auto const maybeKeyset = couldBe(BKeysetN);
    assertx(m_data.packedn->type.subtypeOf(vals.first));
    assertx(m_data.packedn->type.couldBe(vals.second));
    assertx(IMPLIES(maybeKeyset,
                    !is_specialized_int(m_data.packedn->type) ||
                    ival_of(m_data.packedn->type) == 0));
    assertx(IMPLIES(isKeyset, !m_data.packedn->type.hasData()));

    // If the only array bits are BVecN, then we already know the
    // array is packed. We only want a specialization if the value is
    // better than what the bits imply (either TInitCell or TInitUnc).
    if (subtypeAmong(BVecN, BArrLikeN)) {
      assertx(m_data.packedn->type.strictSubtypeOf(vals.first));
    }
    break;
  }
  case DataTag::ArrLikeMapN: {
    // MapN cannot contain just Vec, only a potential union of
    // Vec with other array types. MapN represents all possible
    // arrays, including packed arrays, but an array type of just
    // Vec implies the array is definitely packed, so we should be
    // using PackedN instead.
    assertx(couldBe(BDictN | BKeysetN));
    assertx(subtypeOf(BArrLikeN | kNonSupportBits));
    assertx(!m_data.mapn->key.is(BBottom));
    assertx(!m_data.mapn->val.is(BBottom));

    DEBUG_ONLY auto const key = allowedKeyBits(bits());
    DEBUG_ONLY auto const val = allowedValBits(bits(), false);
    DEBUG_ONLY auto const isKeyset = subtypeAmong(BKeysetN, BArrLikeN);
    DEBUG_ONLY auto const maybeKeyset = couldBe(BKeysetN);

    assertx(m_data.mapn->key.subtypeOf(key.first));
    assertx(m_data.mapn->key.couldBe(key.second));
    assertx(m_data.mapn->val.subtypeOf(val.first));
    assertx(m_data.mapn->val.couldBe(val.second));

    // Either the key or the value need to be a strict subtype of what
    // the bits imply. MapN is already the most general specialized
    // array type. If both the key and value are not any better than
    // what the bits imply, we should not have a specialized type at
    // all (this is needed for == to be correct).
    assertx(m_data.mapn->key.strictSubtypeOf(key.first) ||
            m_data.mapn->val.strictSubtypeOf(val.first));

    // MapN shouldn't have a specialized key. If it does, then that
    // implies it only contains arrays of size 1, which means it
    // should be Map instead.
    assertx(!is_scalar_counted(m_data.mapn->key));
    assertx(IMPLIES(maybeKeyset, m_data.mapn->key.couldBe(m_data.mapn->val)));
    assertx(IMPLIES(isKeyset, m_data.mapn->key.equal(m_data.mapn->val)));
    break;
  }
  }
  return true;
}

//////////////////////////////////////////////////////////////////////

Type Type::specializeWaitHandle(Type obj,
                                Type inner,
                                bool canSuspend) {
  assertx(obj.m_dataTag == DataTag::Obj);
  if (!inner.strictSubtypeOf(BInitCell) && canSuspend) {
    return obj;
  }
  auto ret = Type { obj.bits(), obj.m_legacyMark };
  construct(
    ret.m_data.dwh,
    copy_ptr<DWaitHandle>(
      std::move(obj.m_data.dobj),
      std::move(inner),
      canSuspend
    )
  );
  ret.m_dataTag = DataTag::WaitHandle;
  assertx(ret.checkInvariants());
  return ret;
}

Type wait_handle(Type inner, bool canSuspend) {
  assertx(inner.subtypeOf(BInitCell));
  auto const wh = res::Class::get(s_Awaitable.get());
  assertx(wh.isComplete());
  assertx(wh.couldBeOverriddenByRegular());
  auto obj = Type { BObj, LegacyMark::Bottom };
  construct(obj.m_data.dobj, DCls::MakeSub(wh, false));
  obj.m_dataTag = DataTag::Obj;
  return Type::specializeWaitHandle(
    std::move(obj),
    std::move(inner),
    canSuspend
  );
}

bool is_specialized_wait_handle(const Type& t) {
  return t.m_dataTag == DataTag::WaitHandle;
}

Type wait_handle_inner(const Type& t) {
  assertx(is_specialized_wait_handle(t));
  return t.m_data.dwh->inner;
}

bool wait_handle_can_suspend(const Type& t) {
  if (!t.couldBe(BObj)) return false;
  if (!is_specialized_wait_handle(t)) return true;
  return t.m_data.dwh->canSuspend;
}

Type set_wait_handle_can_suspend(Type t, bool canSuspend) {
  if (!is_specialized_wait_handle(t)) return t;
  t.m_data.dwh.mutate()->canSuspend = canSuspend;
  if (!t.m_data.dwh->inner.strictSubtypeOf(BInitCell) && canSuspend) {
    return demote_wait_handle(std::move(t));
  }
  assertx(t.checkInvariants());
  return t;
}

// Turn a DWaitHandle into the matching DObj specialization (IE,
// dropping any awaited type knowledge).
Type demote_wait_handle(Type wh) {
  assertx(is_specialized_wait_handle(wh));
  auto t = Type { wh.bits(), wh.m_legacyMark };
  construct(t.m_data.dobj, std::move(wh.m_data.dwh->cls));
  t.m_dataTag = DataTag::Obj;
  assertx(!is_specialized_wait_handle(t));
  assertx(is_specialized_obj(t));
  return t;
}

Type sval(SString val) {
  assertx(val->isStatic());
  auto r        = Type { BSStr, LegacyMark::Bottom };
  r.m_data.sval = val;
  r.m_dataTag   = DataTag::Str;
  assertx(r.checkInvariants());
  return r;
}

Type sval_nonstatic(SString val) {
  assertx(val->isStatic());
  auto r        = Type { BStr, LegacyMark::Bottom };
  r.m_data.sval = val;
  r.m_dataTag   = DataTag::Str;
  assertx(r.checkInvariants());
  return r;
}

Type sval_counted(SString val) {
  assertx(val->isStatic());
  auto r        = Type { BCStr, LegacyMark::Bottom };
  r.m_data.sval = val;
  r.m_dataTag   = DataTag::Str;
  assertx(r.checkInvariants());
  return r;
}

Type sempty()           { return sval(staticEmptyString()); }
Type sempty_nonstatic() { return sval_nonstatic(staticEmptyString()); }
Type sempty_counted()   { return sval_counted(staticEmptyString()); }

Type ival(int64_t val) {
  auto r        = Type { BInt, LegacyMark::Bottom };
  r.m_data.ival = val;
  r.m_dataTag   = DataTag::Int;
  assertx(r.checkInvariants());
  return r;
}

Type dval(double val) {
  auto r        = Type { BDbl, LegacyMark::Bottom };
  r.m_data.dval = val;
  r.m_dataTag   = DataTag::Dbl;
  assertx(r.checkInvariants());
  return r;
}

Type lazyclsval(SString val) {
  auto r        = Type { BLazyCls, LegacyMark::Bottom };
  r.m_data.lazyclsval = val;
  r.m_dataTag   = DataTag::LazyCls;
  assertx(r.checkInvariants());
  return r;
}

Type enumclasslabelval(SString val) {
  auto r        = Type { BEnumClassLabel, LegacyMark::Bottom };
  r.m_data.eclval = val;
  r.m_dataTag   = DataTag::EnumClassLabel;
  assertx(r.checkInvariants());
  return r;
}

Type vec_val(SArray val) {
  assertx(val->isStatic());
  assertx(val->isVecType());
  auto const mark = legacyMarkFromSArr(val);
  if (val->empty()) return Type { BSVecE, mark };
  auto t = Type { BSVecN, mark };
  t.m_data.aval = val;
  t.m_dataTag = DataTag::ArrLikeVal;
  assertx(t.checkInvariants());
  return t;
}

Type vec_empty() { return vec_val(staticEmptyVec()); }
Type some_vec_empty() {
  return Type { BVecE, legacyMarkFromSArr(staticEmptyVec()) };
}

Type packedn_impl(trep bits, LegacyMark mark, Type elem) {
  auto t = Type { bits, mark };
  auto const valBits = allowedValBits(bits, true);
  assertx(elem.subtypeOf(valBits.first));
  if (subtypeAmong(bits, BVecN, BArrLikeN)) {
    if (!elem.strictSubtypeOf(valBits.first)) return t;
  }
  construct_inner(t.m_data.packedn, std::move(elem));
  t.m_dataTag = DataTag::ArrLikePackedN;
  assertx(t.checkInvariants());
  return t;
}

Type packed_impl(trep bits, LegacyMark mark, std::vector<Type> elems) {
  assertx(!elems.empty());
  auto t = Type { bits, mark };
  construct_inner(t.m_data.packed, std::move(elems));
  t.m_dataTag = DataTag::ArrLikePacked;
  assertx(t.checkInvariants());
  return t;
}

Type vec_n(Type ty) {
  return packedn_impl(BVecN, LegacyMark::Unmarked, std::move(ty));
}

Type svec_n(Type ty) {
  return packedn_impl(BSVecN, LegacyMark::Unmarked, std::move(ty));
}

Type vec(std::vector<Type> elems) {
  return packed_impl(
    BVecN,
    LegacyMark::Unmarked,
    std::move(elems)
  );
}

Type svec(std::vector<Type> elems) {
  return packed_impl(
    BSVecN,
    LegacyMark::Unmarked,
    std::move(elems)
  );
}

Type dict_val(SArray val) {
  assertx(val->isStatic());
  assertx(val->isDictType());
  auto const mark = legacyMarkFromSArr(val);
  if (val->empty()) return Type { BSDictE, mark };
  auto t = Type { BSDictN, mark };
  t.m_data.aval = val;
  t.m_dataTag   = DataTag::ArrLikeVal;
  assertx(t.checkInvariants());
  return t;
}

Type dict_empty() { return dict_val(staticEmptyDictArray()); }

Type some_dict_empty() {
  return Type { BDictE, legacyMarkFromSArr(staticEmptyDictArray()) };
}

Type dict_map(MapElems m, Type optKey, Type optVal) {
  return map_impl(
    BDictN,
    LegacyMark::Unmarked,
    std::move(m),
    std::move(optKey),
    std::move(optVal)
  );
}

Type sdict_map(MapElems m, Type optKey, Type optVal) {
  return map_impl(
    BSDictN,
    LegacyMark::Unmarked,
    std::move(m),
    std::move(optKey),
    std::move(optVal)
  );
}

Type dict_n(Type k, Type v) {
  return mapn_impl(
    BDictN,
    LegacyMark::Unmarked,
    std::move(k),
    std::move(v)
  );
}

Type sdict_n(Type k, Type v) {
  return mapn_impl(
    BSDictN,
    LegacyMark::Unmarked,
    std::move(k),
    std::move(v)
  );
}

Type dict_packed(std::vector<Type> v) {
  return packed_impl(
    BDictN,
    LegacyMark::Unmarked,
    std::move(v)
  );
}

Type sdict_packed(std::vector<Type> v) {
  return packed_impl(
    BSDictN,
    LegacyMark::Unmarked,
    std::move(v)
  );
}

Type dict_packedn(Type t) {
  return packedn_impl(BDictN, LegacyMark::Unmarked, std::move(t));
}

Type sdict_packedn(Type t) {
  return packedn_impl(BSDictN, LegacyMark::Unmarked, std::move(t));
}

Type keyset_val(SArray val) {
  assertx(val->isStatic());
  assertx(val->isKeysetType());
  if (val->empty()) return keyset_empty();
  auto r        = Type { BSKeysetN, LegacyMark::Bottom };
  r.m_data.aval = val;
  r.m_dataTag   = DataTag::ArrLikeVal;
  assertx(r.checkInvariants());
  return r;
}

Type keyset_empty()         { return Type { BSKeysetE, LegacyMark::Bottom }; }
Type some_keyset_empty()    { return Type { BKeysetE, LegacyMark::Bottom }; }

Type keyset_n(Type kv) {
  assertx(kv.subtypeOf(BArrKey));
  auto v = kv;
  return mapn_impl(
    BKeysetN,
    LegacyMark::Bottom,
    std::move(kv),
    std::move(v)
  );
}

Type skeyset_n(Type kv) {
  assertx(kv.subtypeOf(BUncArrKey));
  auto v = kv;
  return mapn_impl(
    BSKeysetN,
    LegacyMark::Bottom,
    std::move(kv),
    std::move(v)
  );
}

Type keyset_map(MapElems m) {
  return map_impl(
    BKeysetN,
    LegacyMark::Bottom,
    std::move(m),
    TBottom,
    TBottom
  );
}

Type subObj(res::Class val) {
  if (auto const w = val.withoutNonRegular()) {
    val = *w;
  } else {
    return TBottom;
  }
  auto t = Type { BObj, LegacyMark::Bottom };
  construct(
    t.m_data.dobj,
    val.couldBeOverriddenByRegular()
      ? DCls::MakeSub(val, false)
      : DCls::MakeExact(val, false)
  );
  t.m_dataTag = DataTag::Obj;
  assertx(t.checkInvariants());
  return t;
}

Type objExact(res::Class val) {
  if (!val.mightBeRegular()) return TBottom;
  auto t = Type { BObj, LegacyMark::Bottom };
  construct(
    t.m_data.dobj,
    DCls::MakeExact(val, false)
  );
  t.m_dataTag = DataTag::Obj;
  assertx(t.checkInvariants());
  return t;
}

Type subCls(res::Class val, bool nonReg) {
  if (!nonReg || !val.mightContainNonRegular()) {
    if (auto const w = val.withoutNonRegular()) {
      val = *w;
    } else {
      return TBottom;
    }
    nonReg = false;
  }
  auto r        = Type { BCls, LegacyMark::Bottom };
  construct(
    r.m_data.dcls,
    (nonReg ? val.couldBeOverridden() : val.couldBeOverriddenByRegular())
      ? DCls::MakeSub(val, nonReg)
      : DCls::MakeExact(val, nonReg)
  );
  r.m_dataTag = DataTag::Cls;
  assertx(r.checkInvariants());
  return r;
}

Type clsExact(res::Class val, bool nonReg) {
  if (!nonReg || !val.mightBeNonRegular()) {
    if (!val.mightBeRegular()) return TBottom;
    nonReg = false;
  }
  auto r        = Type { BCls, LegacyMark::Bottom };
  construct(
    r.m_data.dcls,
    DCls::MakeExact(val, nonReg)
  );
  r.m_dataTag   = DataTag::Cls;
  assertx(r.checkInvariants());
  return r;
}

Type isectObjInternal(DCls::IsectSet isect) {
  // NB: No canonicalization done here. This is only used internally
  // and we assume the IsectSet is already canonicalized.
  assertx(isect.size() > 1);
  auto t = Type { BObj, LegacyMark::Bottom };
  construct(t.m_data.dobj, DCls::MakeIsect(std::move(isect), false));
  t.m_dataTag = DataTag::Obj;
  assertx(t.checkInvariants());
  return t;
}

Type isectClsInternal(DCls::IsectSet isect, bool nonReg) {
  // NB: No canonicalization done here. This is only used internally
  // and we assume the IsectSet is already canonicalized.
  assertx(isect.size() > 1);
  auto t = Type { BCls, LegacyMark::Bottom };
  construct(t.m_data.dcls, DCls::MakeIsect(std::move(isect), nonReg));
  t.m_dataTag = DataTag::Cls;
  assertx(t.checkInvariants());
  return t;
}

Type isectAndExactObjInternal(res::Class exact, DCls::IsectSet isect) {
  assertx(exact.isMissingDebug());
  assertx(!isect.empty());
  auto t = Type { BObj, LegacyMark::Bottom };
  construct(
    t.m_data.dobj,
    DCls::MakeIsectAndExact(exact, std::move(isect), false)
  );
  t.m_dataTag = DataTag::Obj;
  assertx(t.checkInvariants());
  return t;
}

Type isectAndExactClsInternal(res::Class exact,
                              DCls::IsectSet isect,
                              bool nonReg) {
  assertx(exact.isMissingDebug());
  assertx(!isect.empty());
  auto t = Type { BCls, LegacyMark::Bottom };
  construct(
    t.m_data.dcls,
    DCls::MakeIsectAndExact(exact, std::move(isect), nonReg)
  );
  t.m_dataTag = DataTag::Cls;
  assertx(t.checkInvariants());
  return t;
}

Type map_impl(trep bits, LegacyMark mark, MapElems m,
              Type optKey, Type optVal) {
  assertx(!m.empty());
  assertx(optKey.is(BBottom) == optVal.is(BBottom));

  // A Map cannot be packed, so if it is, use a different
  // representation.
  auto idx = int64_t{0};
  auto packed = true;
  for (auto const& p : m) {
    if (!isIntType(p.first.m_type) || p.first.m_data.num != idx) {
      packed = false;
      break;
    }
    ++idx;
  }
  if (packed) {
    // The map is actually packed. If there's no optional elements, we
    // can turn it into a Packed.
    if (optKey.is(BBottom)) {
      std::vector<Type> elems;
      for (auto& p : m) elems.emplace_back(p.second.val);
      return packed_impl(bits, mark, std::move(elems));
    }

    // There are optional elements. We cannot represent optionals in
    // packed representations, so we need to collapse the values into
    // a single type.
    auto vals = std::move(optVal);
    for (auto const& p : m) vals |= p.second.val;

    // Special case, if the optional elements represent a single key,
    // and that key is next in packed order, we can use PackedN.
    if (auto const k = tvCounted(optKey)) {
      if (isIntType(k->m_type) && k->m_data.num == idx) {
        return packedn_impl(bits, mark, std::move(vals));
      }
    }

    // Not known to be packed including the optional elements, so use
    // MapN, which is most general (it can contain packed and
    // non-packed types).
    return mapn_impl(
      bits,
      mark,
      union_of(idx == 1 ? ival(0) : TInt, optKey),
      std::move(vals)
    );
  }

  auto r = Type { bits, mark };
  construct_inner(
    r.m_data.map,
    std::move(m),
    std::move(optKey),
    std::move(optVal)
  );
  r.m_dataTag = DataTag::ArrLikeMap;
  assertx(r.checkInvariants());
  return r;
}

Type mapn_impl(trep bits, LegacyMark mark, Type k, Type v) {
  assertx(k.subtypeOf(BArrKey));

  auto const keyBits = allowedKeyBits(bits);
  auto const valBits = allowedValBits(bits, false);
  assertx(k.subtypeOf(keyBits.first));
  assertx(v.subtypeOf(valBits.first));

  // A MapN cannot have a constant key (because that can actually make it be a
  // subtype of Map sometimes), so if it does, make it a Map instead.
  if (auto val = tvCounted(k)) {
    MapElems m;
    m.emplace_back(*val, MapElem::KeyFromType(k, std::move(v)));
    return map_impl(bits, mark, std::move(m), TBottom, TBottom);
  }

  auto r = Type { bits, mark };
  if (!k.strictSubtypeOf(keyBits.first) && !v.strictSubtypeOf(valBits.first)) {
    return r;
  }

  construct_inner(
    r.m_data.mapn,
    std::move(k),
    std::move(v)
  );
  r.m_dataTag = DataTag::ArrLikeMapN;
  assertx(r.checkInvariants());
  return r;
}

Type opt(Type t) {
  t.m_bits |= BInitNull;
  assertx(t.checkInvariants());
  return t;
}

Type unopt(Type t) {
  t.m_bits &= ~BInitNull;
  assertx(t.checkInvariants());
  return t;
}

Type return_with_context(Type t, Type context) {
  assertx(t.subtypeOf(BInitCell));
  assertx(context.subtypeOf(BObj) || context.subtypeOf(BCls));

  if (context.is(BBottom)) return unctx(t);

  if (is_specialized_obj(t) && dobj_of(t).isCtx()) {
    if (!context.subtypeOf(BObj)) context = toobj(context);
    if (is_specialized_obj(context)) {
      auto const& d = dobj_of(context);
      if (d.isExact() && d.cls().couldBeMocked()) {
        context = subObj(d.cls());
      }
    }
    auto [obj, rest] = split_obj(std::move(t));
    return union_of(
      intersection_of(unctx(std::move(obj)), std::move(context)),
      rest
    );
  }
  if (is_specialized_cls(t) && t.m_data.dcls.isCtx()) {
    if (!context.subtypeOf(BCls)) context = objcls(context);
    if (is_specialized_cls(context)) {
      auto const& d = dcls_of(context);
      if (d.isExact() && d.cls().couldBeMocked()) {
        context = subCls(d.cls(), true);
      }
    }
    auto [cls, rest] = split_cls(std::move(t));
    return union_of(
      intersection_of(unctx(std::move(cls)), std::move(context)),
      rest
    );
  }
  return unctx(t);
}

Type setctx(Type t, bool to) {
  if (t.m_dataTag == DataTag::Obj) t.m_data.dobj.setCtx(to);
  if (is_specialized_wait_handle(t)) t.m_data.dwh.mutate()->cls.setCtx(to);
  if (is_specialized_cls(t))       t.m_data.dcls.setCtx(to);
  return t;
}

Type unctx(Type t) {
  bool c;
  return Type::unctxHelper(t, c);
}

bool is_specialized_array_like(const Type& t) {
  switch (t.m_dataTag) {
  case DataTag::None:
  case DataTag::Str:
  case DataTag::LazyCls:
  case DataTag::EnumClassLabel:
  case DataTag::Obj:
  case DataTag::WaitHandle:
  case DataTag::Int:
  case DataTag::Dbl:
  case DataTag::Cls:
    return false;
  case DataTag::ArrLikeVal:
  case DataTag::ArrLikePacked:
  case DataTag::ArrLikePackedN:
  case DataTag::ArrLikeMap:
  case DataTag::ArrLikeMapN:
    return true;
  }
  not_reached();
}

bool is_specialized_array_like_arrval(const Type& t) {
  return t.m_dataTag == DataTag::ArrLikeVal;
}
bool is_specialized_array_like_packedn(const Type& t) {
  return t.m_dataTag == DataTag::ArrLikePackedN;
}
bool is_specialized_array_like_packed(const Type& t) {
  return t.m_dataTag == DataTag::ArrLikePacked;
}
bool is_specialized_array_like_mapn(const Type& t) {
  return t.m_dataTag == DataTag::ArrLikeMapN;
}
bool is_specialized_array_like_map(const Type& t) {
  return t.m_dataTag == DataTag::ArrLikeMap;
}

bool is_specialized_obj(const Type& t) {
  return
    t.m_dataTag == DataTag::Obj ||
    t.m_dataTag == DataTag::WaitHandle;
}

bool is_specialized_cls(const Type& t) {
  return t.m_dataTag == DataTag::Cls;
}

bool is_specialized_string(const Type& t) {
  return t.m_dataTag == DataTag::Str;
}

bool is_specialized_lazycls(const Type& t) {
  return t.m_dataTag == DataTag::LazyCls;
}

bool is_specialized_ecl(const Type& t) {
  return t.m_dataTag == DataTag::EnumClassLabel;
}

bool is_specialized_int(const Type& t) {
  return t.m_dataTag == DataTag::Int;
}

bool is_specialized_double(const Type& t) {
  return t.m_dataTag == DataTag::Dbl;
}

Type toobj(const Type& t) {
  assertx(t.subtypeOf(BCls));
  assertx(!t.subtypeOf(BBottom));

  if (!is_specialized_cls(t)) return TObj;

  auto const& d = dcls_of(t);
  return setctx(
    [&] {
      if (d.isExact()) {
        return objExact(d.cls());
      } else if (d.isSub()) {
        return subObj(d.cls());
      } else if (d.isIsect()) {
        if (!d.containsNonRegular()) return isectObjInternal(d.isect());
        auto const u = res::Class::removeNonRegular(d.isect());
        if (u.empty()) return TBottom;
        if (u.size() == 1) return subObj(u.front());
        DCls::IsectSet set{u.begin(), u.end()};
        return isectObjInternal(std::move(set));
      } else {
        assertx(d.isIsectAndExact());
        auto const [e, i] = d.isectAndExact();
        if (!d.containsNonRegular()) return isectAndExactObjInternal(e, *i);
        if (!e.mightBeRegular()) return TBottom;
        auto const u = res::Class::removeNonRegular(*i);
        if (u.empty()) return TBottom;
        DCls::IsectSet set{u.begin(), u.end()};
        return isectAndExactObjInternal(e, std::move(set));
      }
    }(),
    d.isCtx()
  );
}

Type objcls(const Type& t) {
  assertx(t.subtypeOf(BObj));
  assertx(!t.subtypeOf(BBottom));
  if (!is_specialized_obj(t)) return TCls;
  auto const& d = dobj_of(t);
  assertx(!d.containsNonRegular());
  auto r = Type { BCls, LegacyMark::Bottom };
  construct(r.m_data.dcls, d);
  r.m_dataTag = DataTag::Cls;
  return setctx(std::move(r), d.isCtx());
}

//////////////////////////////////////////////////////////////////////

Optional<int64_t> arr_size(const Type& t) {
  if (t.subtypeOf(BArrLikeE)) return 0;
  if (!t.subtypeOf(BArrLikeN)) return std::nullopt;

  switch (t.m_dataTag) {
    case DataTag::ArrLikeVal:
      return t.m_data.aval->size();

    case DataTag::ArrLikeMap:
      if (t.m_data.map->hasOptElements()) return std::nullopt;
      return t.m_data.map->map.size();

    case DataTag::ArrLikePacked:
      return t.m_data.packed->elems.size();

    case DataTag::None:
    case DataTag::Int:
    case DataTag::Dbl:
    case DataTag::Str:
    case DataTag::LazyCls:
    case DataTag::EnumClassLabel:
    case DataTag::ArrLikePackedN:
    case DataTag::ArrLikeMapN:
    case DataTag::Obj:
    case DataTag::WaitHandle:
    case DataTag::Cls:
      return std::nullopt;
  }
  not_reached();
}

Type::ArrayCat categorize_array(const Type& t) {
  auto hasInts = false;
  auto hasStrs = false;
  auto isPacked = true;
  // Even if all the values are constants, we can't produce a constant array
  // unless the d/varray-ness is definitely known.
  auto val = t.subtypeOf(BVec) || t.subtypeOf(BDict) || t.subtypeOf(BKeyset);
  size_t idx = 0;
  auto const checkKey = [&] (const TypedValue& key) {
    if (isStringType(key.m_type)) {
      hasStrs = true;
      isPacked = false;
      return hasInts;
    } else {
      hasInts = true;
      if (key.m_data.num != idx++) isPacked = false;
      return hasStrs && !isPacked;
    }
  };

  switch (t.m_dataTag) {
    case DataTag::ArrLikeVal:
      IterateKV(t.m_data.aval,
                [&] (TypedValue k, TypedValue) {
                  return checkKey(k);
                });
      break;

    case DataTag::ArrLikeMap:
      if (t.m_data.map->hasOptElements()) return {};
      for (auto const& elem : t.m_data.map->map) {
        if (checkKey(elem.first) && !val) break;
        val = val && tv(elem.second.val);
      }
      break;

    case DataTag::ArrLikePacked:
      for (auto const& elem : t.m_data.packed->elems) {
        hasInts = true;
        val = val && tv(elem);
        if (!val) break;
      }
      break;

    case DataTag::None:
    case DataTag::Int:
    case DataTag::Dbl:
    case DataTag::Str:
    case DataTag::LazyCls:
    case DataTag::EnumClassLabel:
    case DataTag::ArrLikePackedN:
    case DataTag::ArrLikeMapN:
    case DataTag::Obj:
    case DataTag::WaitHandle:
    case DataTag::Cls:
      return {};
  }

  auto cat =
    hasInts ? (isPacked ? Type::ArrayCat::Packed : Type::ArrayCat::Mixed) :
    hasStrs ? Type::ArrayCat::Struct : Type::ArrayCat::Empty;

  return { cat , val };
}

CompactVector<LSString> get_string_keys(const Type& t) {
  CompactVector<LSString> strs;
  // No keys if the array is known to be empty.
  if (t.subtypeOf(BArrLikeE)) return strs;

  switch (t.m_dataTag) {
    case DataTag::ArrLikeVal:
      IterateKV(t.m_data.aval,
                [&] (TypedValue k, TypedValue) {
                  assertx(isStringType(k.m_type));
                  strs.push_back(k.m_data.pstr);
                });
      break;

    case DataTag::ArrLikeMap:
      assertx(!t.m_data.map->hasOptElements());
      for (auto const& elem : t.m_data.map->map) {
        assertx(isStringType(elem.first.m_type));
        strs.push_back(elem.first.m_data.pstr);
      }
      break;

    case DataTag::ArrLikePacked:
    case DataTag::None:
    case DataTag::Int:
    case DataTag::Dbl:
    case DataTag::Str:
    case DataTag::LazyCls:
    case DataTag::EnumClassLabel:
    case DataTag::ArrLikePackedN:
    case DataTag::ArrLikeMapN:
    case DataTag::Obj:
    case DataTag::WaitHandle:
    case DataTag::Cls:
      always_assert(false);
  }

  return strs;
}

template<typename R, bool force_static, bool allow_counted>
struct tvHelper {
  template<DataType dt,typename... Args>
  static R make(Args&&... args) {
    return make_tv<dt>(std::forward<Args>(args)...);
  }
  template<typename... Args>
  static R makePersistentArray(Args&&... args) {
    return make_array_like_tv(std::forward<Args>(args)...);
  }
  template<typename Init, typename... Args>
  static R fromMap(Args&&... args) {
    return fromTypeMap<Init, force_static, allow_counted>(
      std::forward<Args>(args)...
    );
  }
  template<typename Init, typename... Args>
  static R fromVec(Args&&... args) {
    return fromTypeVec<Init, force_static, allow_counted>(
      std::forward<Args>(args)...
    );
  }
};

template<bool ignored, bool allow_counted>
struct tvHelper<bool, ignored, allow_counted> {
  template <DataType dt, typename... Args>
  static bool make(Args&&...) { return true; }
  template <typename... Args>
  static bool makePersistentArray(Args&&...) { return true; }
  template<typename Init, typename... Args>
  static bool fromMap(Args&&... args) {
    return checkTypeMap<allow_counted>(std::forward<Args>(args)...);
  }
  template<typename Init, typename... Args>
  static bool fromVec(Args&&... args) {
    return checkTypeVec<allow_counted>(std::forward<Args>(args)...);
  }
};

template<typename R, bool force_static, bool allow_counted>
R tvImpl(const Type& t) {
  assertx(t.checkInvariants());
  using H = tvHelper<R, force_static, allow_counted>;

  auto const emptyArray = [&] (auto unmarked, auto marked) {
    auto const mark = legacyMark(t.m_legacyMark, t.bits());
    if (mark == LegacyMark::Unknown) return R{};

    auto const ad = mark == LegacyMark::Unmarked ? unmarked() : marked();
    assertx(ad->empty());
    return H::makePersistentArray(ad);
  };

  switch (t.bits()) {
  case BUninit:      return H::template make<KindOfUninit>();
  case BInitNull:    return H::template make<KindOfNull>();
  case BTrue:        return H::template make<KindOfBoolean>(true);
  case BFalse:       return H::template make<KindOfBoolean>(false);
  case BVecE:
  case BSVecE:
    return emptyArray(staticEmptyVec, staticEmptyMarkedVec);
  case BDictE:
  case BSDictE:
    return emptyArray(staticEmptyDictArray, staticEmptyMarkedDictArray);
  case BKeysetE:
  case BSKeysetE:
    return H::template make<KindOfKeyset>(staticEmptyKeysetArray());

  default:
    break;
  }

  if (allow_counted) {
    switch (t.bits()) {
      case BCVecE:
        return emptyArray(staticEmptyVec, staticEmptyMarkedVec);
      case BCDictE:
        return emptyArray(staticEmptyDictArray, staticEmptyMarkedDictArray);
      case BCKeysetE:
        return H::template make<KindOfKeyset>(staticEmptyKeysetArray());
      default:
        break;
    }
  }

  switch (t.m_dataTag) {
    case DataTag::Int:
      if (!t.subtypeOf(BInt)) break;
      return H::template make<KindOfInt64>(t.m_data.ival);
    case DataTag::Dbl:
      if (!t.subtypeOf(BDbl)) break;
      return H::template make<KindOfDouble>(t.m_data.dval);
    case DataTag::Str:
      if (!t.subtypeOf(BStr) || (!allow_counted && t.subtypeOf(BCStr))) break;
      return H::template make<KindOfString>(t.m_data.sval);
    case DataTag::LazyCls:
      if (!t.subtypeOf(BLazyCls)) break;
      return H::template make<KindOfLazyClass>(
          LazyClassData::create(t.m_data.lazyclsval));
    case DataTag::EnumClassLabel:
      if (!t.subtypeOf(BEnumClassLabel)) break;
      return H::template make<KindOfEnumClassLabel>(t.m_data.eclval);
    case DataTag::ArrLikeVal:
      // It's a Type invariant that the bits will be exactly one of
      // the array types with ArrLikeVal. We don't care which.
      if (!t.subtypeOf(BArrLikeN)) break;
      return H::makePersistentArray(const_cast<ArrayData*>(t.m_data.aval));
    case DataTag::ArrLikeMap:
      if (t.m_data.map->hasOptElements()) break;
      if (t.subtypeOf(BDictN) && (allow_counted || !t.subtypeOf(BCDictN))) {
        return H::template fromMap<DictInit>(t.m_data.map->map,
                                             t.bits(),
                                             t.m_legacyMark);
      } else if (t.subtypeOf(BKeysetN) &&
                 (allow_counted || !t.subtypeOf(BCKeysetN))) {
        return H::template fromMap<KeysetInit>(t.m_data.map->map,
                                               t.bits(),
                                               t.m_legacyMark);
      }
      break;
    case DataTag::ArrLikePacked:
      if (t.subtypeOf(BVecN) &&
          (allow_counted || !t.subtypeOf(BCVecN))) {
        return H::template fromVec<VecInit>(t.m_data.packed->elems,
                                            t.bits(),
                                            t.m_legacyMark);
      } else if (t.subtypeOf(BDictN) &&
                 (allow_counted || !t.subtypeOf(BCDictN))) {
        return H::template fromVec<DictInit>(t.m_data.packed->elems,
                                             t.bits(),
                                             t.m_legacyMark);
      } else if (t.subtypeOf(BKeysetN) &&
                 (allow_counted || !t.subtypeOf(BCKeysetN))) {
        return H::template fromVec<KeysetAppendInit>(t.m_data.packed->elems,
                                                     t.bits(),
                                                     t.m_legacyMark);
      }
      break;
    case DataTag::ArrLikePackedN:
    case DataTag::ArrLikeMapN:
    case DataTag::Obj:
    case DataTag::WaitHandle:
    case DataTag::Cls:
    case DataTag::None:
      break;
  }

  return R{};
}

Optional<TypedValue> tv(const Type& t) {
  return tvImpl<Optional<TypedValue>, true, false>(t);
}

Optional<TypedValue> tvNonStatic(const Type& t) {
  return tvImpl<Optional<TypedValue>, false, false>(t);
}

Optional<TypedValue> tvCounted(const Type& t) {
  return tvImpl<Optional<TypedValue>, true, true>(t);
}

bool is_scalar(const Type& t) {
  return tvImpl<bool, true, false>(t);
}

bool is_scalar_counted(const Type& t) {
  return tvImpl<bool, true, true>(t);
}

Type scalarize(Type t) {
  assertx(is_scalar(t));

  switch (t.m_dataTag) {
    case DataTag::None:
      assertx(t.subtypeOf(BNull | BBool | BArrLikeE));
      t.m_bits &= (BNull | BBool | BSArrLikeE);
      break;
    case DataTag::Int:
    case DataTag::Dbl:
    case DataTag::LazyCls:
    case DataTag::EnumClassLabel:
      break;
    case DataTag::ArrLikeVal:
      t.m_bits &= BSArrLike;
      break;
    case DataTag::Str:
      t.m_bits &= BSStr;
      break;
    case DataTag::ArrLikeMap:
    case DataTag::ArrLikePacked:
      return from_cell(*tv(t));
    case DataTag::ArrLikePackedN:
    case DataTag::ArrLikeMapN:
    case DataTag::Obj:
    case DataTag::WaitHandle:
    case DataTag::Cls:
      not_reached();
  }

  assertx(t.checkInvariants());
  return t;
}

Type type_of_istype(IsTypeOp op) {
  switch (op) {
  case IsTypeOp::Null:    return TNull;
  case IsTypeOp::Bool:    return TBool;
  case IsTypeOp::Int:     return TInt;
  case IsTypeOp::Dbl:     return TDbl;
  case IsTypeOp::Str:     return union_of(TStr, TCls, TLazyCls);
  case IsTypeOp::Res:     return TRes;
  case IsTypeOp::Vec:     return TVec;
  case IsTypeOp::Dict:    return TDict;
  case IsTypeOp::Keyset:  return TKeyset;
  case IsTypeOp::Obj:     return TObj;
  case IsTypeOp::ClsMeth: return union_of(TClsMeth, TRClsMeth);
  case IsTypeOp::Class:   return union_of(TCls, TLazyCls);
  case IsTypeOp::Func:    return union_of(TFunc, TRFunc);
  case IsTypeOp::ArrLike: return TArrLike;
  case IsTypeOp::Scalar: always_assert(false);
  case IsTypeOp::LegacyArrLike: always_assert(false);
  }
  not_reached();
}

Optional<IsTypeOp> type_to_istypeop(const Type& t) {
  if (t.subtypeOf(BNull))   return IsTypeOp::Null;
  if (t.subtypeOf(BBool))   return IsTypeOp::Bool;
  if (t.subtypeOf(BInt))    return IsTypeOp::Int;
  if (t.subtypeOf(BDbl))    return IsTypeOp::Dbl;
  if (t.subtypeOf(BStr))    return IsTypeOp::Str;
  if (t.subtypeOf(BVec))    return IsTypeOp::Vec;
  if (t.subtypeOf(BDict))   return IsTypeOp::Dict;
  if (t.subtypeOf(BRes))    return IsTypeOp::Res;
  if (t.subtypeOf(BKeyset)) return IsTypeOp::Keyset;
  if (t.subtypeOf(BObj))    return IsTypeOp::Obj;
  if (t.subtypeOf(BClsMeth)) return IsTypeOp::ClsMeth;
  if (t.subtypeOf(BCls)) return IsTypeOp::Class;
  if (t.subtypeOf(BLazyCls)) return IsTypeOp::Class;
  if (t.subtypeOf(BFunc)) return IsTypeOp::Func;
  return std::nullopt;
}

Optional<Type> type_of_type_structure(const IIndex& index,
                                      Context ctx,
                                      SArray ts) {
  auto base = [&] () -> Optional<Type> {
    switch (get_ts_kind(ts)) {
      case TypeStructure::Kind::T_int:      return TInt;
      case TypeStructure::Kind::T_bool:     return TBool;
      case TypeStructure::Kind::T_float:    return TDbl;
      case TypeStructure::Kind::T_string:   return union_of(TStr,TCls,TLazyCls);
      case TypeStructure::Kind::T_resource: return TRes;
      case TypeStructure::Kind::T_num:      return TNum;
      case TypeStructure::Kind::T_arraykey: return TArrKey;
      case TypeStructure::Kind::T_dict:     return TDict;
      case TypeStructure::Kind::T_vec:      return TVec;
      case TypeStructure::Kind::T_keyset:   return TKeyset;
      case TypeStructure::Kind::T_void:
      case TypeStructure::Kind::T_null:     return TNull;
      case TypeStructure::Kind::T_tuple: {
        // Taking a very conservative approach to open tuples analogous to shapes
        // (see comment there)
        if (does_ts_tuple_allow_optional_or_unknown_fields(ts)) return std::nullopt;
        auto const tsElems = get_ts_elem_types(ts);
        std::vector<Type> v;
        for (auto i = 0; i < tsElems->size(); i++) {
          auto t = type_of_type_structure(
            index, ctx, tsElems->getValue(i).getArrayData());
          if (!t) return std::nullopt;
          v.emplace_back(remove_uninit(std::move(t.value())));
        }
        if (v.empty()) return std::nullopt;
        return vec(v);
      }
      case TypeStructure::Kind::T_shape: {
        // An unresolved shape splat carries `splat_elem_types` instead of
        // `fields`; its full field set is unknown, so stay conservative.
        if (get_ts_splat_elem_types_opt(ts)) return std::nullopt;
        // Taking a very conservative approach to shapes where we dont do any
        // conversions if the shape contains unknown or optional fields
        if (does_ts_shape_allow_unknown_fields(ts)) return std::nullopt;
        auto map = MapElems{};
        auto const fields = get_ts_fields(ts);
        for (auto i = 0; i < fields->size(); i++) {
          auto const keyV = fields->getKey(i);
          if (!keyV.isString()) return std::nullopt;
          auto key = keyV.getStringData();
          auto const wrapper = fields->getValue(i).getArrayData();

          // Shapes can be defined using class constants, these keys
          // must be resolved, and to side-step the issue of shapes
          // potentially being packed arrays if the keys are
          // consecutive integers beginning with 0, we allow only keys
          // that resolve to strings here.
          if (wrapper->exists(s_is_cls_cns)) {
            std::string cls, cns;
            auto const matched = folly::split("::", key->data(), cls, cns);
            always_assert(matched);

            auto const rcls = index.resolve_class(makeStaticString(cls));
            if (!rcls) return std::nullopt;

            auto const lookup = index.lookup_class_constant(
              ctx,
              clsExact(*rcls, true),
              sval(makeStaticString(cns))
            );
            if (lookup.found != TriBool::Yes || lookup.mightThrow) {
              return std::nullopt;
            }

            auto const vcns = tv(lookup.ty);
            if (!vcns || !isStringType(type(*vcns))) return std::nullopt;
            key = val(*vcns).pstr;
          }

          // Optional fields are hard to represent as a type
          if (is_optional_ts_shape_field(wrapper)) return std::nullopt;

          auto const value = [&] () -> SArray {
            auto const v = wrapper->get(s_value);
            if (!v.is_init()) return wrapper;
            if (!tvIsDict(v)) return nullptr;
            return val(v).parr;
          }();
          if (!value) return std::nullopt;

          auto t = type_of_type_structure(index, ctx, value);
          if (!t) return std::nullopt;

          map.emplace_back(
            make_tv<KindOfString>(key),
            MapElem::SStrKey(remove_uninit(std::move(t.value())))
          );
        }
        if (map.empty()) return std::nullopt;
        return dict_map(map);
      }
      case TypeStructure::Kind::T_union: {
        auto const tsTypes = get_ts_union_types(ts);
        auto ret = TBottom;
        for (auto i = 0; i < tsTypes->size(); i++) {
          auto t = type_of_type_structure(
            index, ctx, tsTypes->getValue(i).getArrayData());
          if (!t) return std::nullopt;
          ret |= *t;
        }
        return ret;
      }
      case TypeStructure::Kind::T_recursiveUnion: {
        // Since our type can't cleanly represent a recursive type be
        // conservative here and bail.
        return std::nullopt;
      }
      case TypeStructure::Kind::T_vec_or_dict: return union_of(TVec, TDict);
      case TypeStructure::Kind::T_any_array:   return TArrLike;
      case TypeStructure::Kind::T_nothing:
      case TypeStructure::Kind::T_noreturn:
      case TypeStructure::Kind::T_mixed:
      case TypeStructure::Kind::T_dynamic:
      case TypeStructure::Kind::T_nonnull:
      case TypeStructure::Kind::T_class:
      case TypeStructure::Kind::T_interface:
      case TypeStructure::Kind::T_unresolved:
      case TypeStructure::Kind::T_typeaccess:
      case TypeStructure::Kind::T_darray:
      case TypeStructure::Kind::T_varray:
      case TypeStructure::Kind::T_varray_or_darray:
      case TypeStructure::Kind::T_xhp:
      case TypeStructure::Kind::T_enum:
      case TypeStructure::Kind::T_fun:
      case TypeStructure::Kind::T_typevar:
      case TypeStructure::Kind::T_trait:
      case TypeStructure::Kind::T_reifiedtype:
        return std::nullopt;
      case TypeStructure::Kind::T_class_ptr:
        // Don't try to model T_string (classname<T>); just bail
        return std::nullopt;
      case TypeStructure::Kind::T_class_or_classname:
        return std::nullopt;

    }
    not_reached();
  }();
  if (base && is_ts_nullable(ts)) base = opt(std::move(*base));
  return base;
}

const DCls& dobj_of(const Type& t) {
  assertx(t.checkInvariants());
  assertx(is_specialized_obj(t));
  if (t.m_dataTag == DataTag::Obj) return t.m_data.dobj;
  assertx(is_specialized_wait_handle(t));
  return t.m_data.dwh->cls;
}

const DCls& dcls_of(const Type& t) {
  assertx(t.checkInvariants());
  assertx(is_specialized_cls(t));
  return t.m_data.dcls;
}

SString sval_of(const Type& t) {
  assertx(t.checkInvariants());
  assertx(is_specialized_string(t));
  return t.m_data.sval;
}

SString lazyclsval_of(const Type& t) {
  assertx(t.checkInvariants());
  assertx(is_specialized_lazycls(t));
  return t.m_data.lazyclsval;
}

SString eclval_of(const Type& t) {
  assertx(t.checkInvariants());
  assertx(is_specialized_ecl(t));
  return t.m_data.eclval;
}

int64_t ival_of(const Type& t) {
  assertx(t.checkInvariants());
  assertx(is_specialized_int(t));
  return t.m_data.ival;
}

Type from_cell(TypedValue cell) {
  assertx(tvIsPlausible(cell));

  switch (cell.m_type) {
  case KindOfUninit:   return TUninit;
  case KindOfNull:     return TInitNull;
  case KindOfBoolean:  return cell.m_data.num ? TTrue : TFalse;
  case KindOfInt64:    return ival(cell.m_data.num);
  case KindOfDouble:   return dval(cell.m_data.dbl);

  case KindOfString:
    always_assert(cell.m_data.pstr->isStatic());
    return sval(cell.m_data.pstr);

  case KindOfVec:
    always_assert(cell.m_data.parr->isStatic());
    always_assert(cell.m_data.parr->isVecType());
    return vec_val(cell.m_data.parr);

  case KindOfDict:
    always_assert(cell.m_data.parr->isStatic());
    always_assert(cell.m_data.parr->isDictType());
    return dict_val(cell.m_data.parr);

  case KindOfKeyset:
    always_assert(cell.m_data.parr->isStatic());
    always_assert(cell.m_data.parr->isKeysetType());
    return keyset_val(cell.m_data.parr);

  case KindOfLazyClass: return lazyclsval(cell.m_data.plazyclass.name());

  case KindOfEnumClassLabel:
    always_assert(cell.m_data.pstr->isStatic());
    return enumclasslabelval(cell.m_data.pstr);

  case KindOfObject:
  case KindOfResource:
  case KindOfRFunc:
  case KindOfFunc:
  case KindOfClass:
  case KindOfClsMeth:
  case KindOfRClsMeth:
    break;
  }
  always_assert(
      0 && "reference counted/class/func/clsmeth type in from_cell");
}

Type from_DataType(DataType dt) {
  switch (dt) {
  case KindOfUninit:   return TUninit;
  case KindOfNull:     return TInitNull;
  case KindOfBoolean:  return TBool;
  case KindOfInt64:    return TInt;
  case KindOfDouble:   return TDbl;
  case KindOfString:   return TStr;
  case KindOfVec:      return TVec;
  case KindOfDict:     return TDict;
  case KindOfKeyset:   return TKeyset;
  case KindOfObject:   return TObj;
  case KindOfResource: return TRes;
  case KindOfRFunc:    return TRFunc;
  case KindOfFunc:     return TFunc;
  case KindOfClass:    return TCls;
  case KindOfLazyClass:  return TLazyCls;
  case KindOfClsMeth:  return TClsMeth;
  case KindOfRClsMeth: return TRClsMeth;
  case KindOfEnumClassLabel: return TEnumClassLabel;
  }
  always_assert(0 && "dt in from_DataType didn't satisfy preconditions");
}

Type from_hni_constraint(SString s) {
  if (!s) return TCell;

  auto p   = s->data();
  auto ret = TBottom;

  if (*p == '?') {
    ret = opt(std::move(ret));
    ++p;
  }

  if (!tstrcmp(p, annotTypeName(AnnotType::Null)))     return opt(std::move(ret));
  if (!tstrcmp(p, annotTypeName(AnnotType::Object)))   return union_of(std::move(ret), TObj);
  if (!tstrcmp(p, annotTypeName(AnnotType::Resource))) return union_of(std::move(ret), TRes);
  if (!tstrcmp(p, annotTypeName(AnnotType::Bool)))     return union_of(std::move(ret), TBool);
  if (!tstrcmp(p, annotTypeName(AnnotType::Int)))      return union_of(std::move(ret), TInt);
  if (!tstrcmp(p, annotTypeName(AnnotType::Float)))    return union_of(std::move(ret), TDbl);
  if (!tstrcmp(p, annotTypeName(AnnotType::Number)))   return union_of(std::move(ret), TNum);
  if (!tstrcmp(p, annotTypeName(AnnotType::String)))   return union_of(std::move(ret), TStr);
  if (!tstrcmp(p, annotTypeName(AnnotType::ArrayKey))) return union_of(std::move(ret), TArrKey);
  if (!tstrcmp(p, annotTypeName(AnnotType::Dict)))     return union_of(std::move(ret), TDict);
  if (!tstrcmp(p, annotTypeName(AnnotType::Vec)))      return union_of(std::move(ret), TVec);
  if (!tstrcmp(p, annotTypeName(AnnotType::Keyset)))   return union_of(std::move(ret), TKeyset);
  if (!tstrcmp(p, kAnnotTypeVarrayStr)) {
    return union_of(std::move(ret), TVec);
  }
  if (!tstrcmp(p, kAnnotTypeDarrayStr)) {
    return union_of(std::move(ret), TDict);
  }
  if (!tstrcmp(p, kAnnotTypeVarrayOrDarrayStr)) {
    return union_of(std::move(ret), TVec, TDict);
  }
  if (!tstrcmp(p, annotTypeName(AnnotType::VecOrDict))) {
    return union_of(std::move(ret), TVec, TDict);
  }
  if (!tstrcmp(p, annotTypeName(AnnotType::ArrayLike))) {
    return union_of(std::move(ret), TArrLike);
  }
  if (!tstrcmp(p, annotTypeName(AnnotType::Classname))) {
    if (!Cfg::Eval::ClassPassesClassname) {
      return union_of(ret, TStr);
    }
    return union_of(ret, union_of(TStr, union_of(TCls, TLazyCls)));
  }
  if (!tstrcmp(p, annotTypeName(AnnotType::Class))) {
    if (Cfg::Eval::ClassTypeLevel > 0) {
      return union_of(ret, union_of(TCls, TLazyCls));
    }
    return union_of(ret, union_of(TStr, union_of(TCls, TLazyCls)));
  }
  if (!tstrcmp(p, annotTypeName(AnnotType::Mixed)))    return TInitCell;
  if (!tstrcmp(p, annotTypeName(AnnotType::Nonnull)))  return union_of(std::move(ret), TNonNull);

  // It might be an object, or we might want to support type aliases in HNI at
  // some point.  For now just be conservative.
  return TCell;
}

Type intersection_of(Type a, Type b) {
  using HPHP::HHBBC::couldBe;

  auto isect = a.bits() & b.bits();
  if (!isect) return TBottom;

  auto mark = intersectionOf(
    project(a.m_legacyMark, isect),
    project(b.m_legacyMark, isect)
  );
  if (couldBe(isect, Type::kLegacyMarkBits) &&
      mark == LegacyMark::Bottom) {
    isect &= ~Type::kLegacyMarkBits;
    if (!isect) return TBottom;
    a = remove_bits(std::move(a), Type::kLegacyMarkBits);
    b = remove_bits(std::move(b), Type::kLegacyMarkBits);
    assertx(!a.is(BBottom));
    assertx(!b.is(BBottom));
  }

  // Return intersected type without a specialization.
  auto const nodata = [&] {
    assertx(isect);
    return Type { isect, mark };
  };
  // If the intersection cannot support a specialization, there's no
  // need to check them.
  if (!couldBe(isect, kSupportBits)) return nodata();
  if (!a.hasData() && !b.hasData()) return nodata();

  // Return intersected type re-using the specialization in the given
  // type. Update the bits to match the intersection.
  auto const reuse = [&] (Type& dst) {
    dst.m_bits = isect;
    dst.m_legacyMark = mark;
    assertx(dst.checkInvariants());
    return std::move(dst);
  };

  // Intersect the class constraints independently, then reattach the
  // wait-handle facts when the result can represent both specializations.
  auto const whAndObj = [&] (Type wh, Type obj) {
    auto inner = wh.m_data.dwh->inner;
    auto const canSuspend = wh.m_data.dwh->canSuspend;
    auto cls = intersection_of(
      demote_wait_handle(std::move(wh)),
      std::move(obj)
    );
    if (cls.m_dataTag != DataTag::Obj) return cls;
    return Type::specializeWaitHandle(
      std::move(cls),
      std::move(inner),
      canSuspend
    );
  };

  // If both sides have matching specialized data, check if the
  // specializations match. If so, reuse one of the sides. If not, the
  // supported bits for this data cannot be present in the
  // intersection, so remove it. If this makes the intersection
  // TBottom, we're done. Otherwise, just return nodata() (the
  // intersection cannot possibly have any specialized data because
  // each side can only have one and we already determined they don't
  // match). If only one side has a specialization, check if that
  // specialization is supported by the intersection. If so, we can
  // keep it. If not, ignore it and check for other specialized
  // types. Note that this means the order of checking for
  // specializations determines the priority of which specializations
  // to keep when there's a mismatch.

  // For wait handles, intersect the class specialization independently from
  // the result type and eagerness. This lets an eager wait handle retain a
  // narrower concrete Awaitable class.
  if (is_specialized_wait_handle(a)) {
    if (is_specialized_wait_handle(b)) {
      assertx(couldBe(isect, BObj));
      auto const canSuspend = intersectCanSuspend(
        a.m_data.dwh->canSuspend,
        b.m_data.dwh->canSuspend
      );
      auto i = intersection_of(
        a.m_data.dwh->inner,
        b.m_data.dwh->inner
      );
      auto cls = intersection_of(
        demote_wait_handle(std::move(a)),
        demote_wait_handle(std::move(b))
      );
      if (cls.m_dataTag != DataTag::Obj) return cls;
      return Type::specializeWaitHandle(
        std::move(cls),
        std::move(i),
        canSuspend
      );
    }
    if (is_specialized_obj(b)) {
      assertx(couldBe(isect, BObj));
      return whAndObj(std::move(a), std::move(b));
    } else if (couldBe(isect, BObj)) {
      return reuse(a);
    }
  } else if (is_specialized_wait_handle(b)) {
    if (is_specialized_obj(a)) {
      assertx(couldBe(isect, BObj));
      return whAndObj(std::move(b), std::move(a));
    } else if (couldBe(isect, BObj)) {
      return reuse(b);
    }
  }

  auto const isectDCls = [&] (DCls& acls, DCls& bcls, bool isObj) {
    auto const fail = [&] {
      isect &= isObj ? ~BObj : ~BCls;
      return isect ? nodata() : TBottom;
    };

    auto const ctx = acls.isCtx() || bcls.isCtx();
    if (subtypeCls<false>(acls, bcls)) return setctx(reuse(a), ctx);
    if (subtypeCls<false>(bcls, acls)) return setctx(reuse(b), ctx);
    if (!couldBeCls(acls, bcls)) return fail();

    auto const nonRegA = acls.containsNonRegular();
    auto const nonRegB = bcls.containsNonRegular();

    auto isectNonReg = nonRegA && nonRegB;

    // Exact classes should have been definitively resolved by one of
    // the above checks. The exception is if we have unresolved
    // classes and the other class isn't exact. In that case, the
    // intersection is just the unresolved class. Since the unresolved
    // class is exact, any intersection must contain only it (or be
    // Bottom).

    auto const handleExact = [&] (DCls& exact, DCls& sub, bool subNonReg) {
      assertx(exact.isExact());
      assertx(exact.cls().isMissingDebug());
      assertx(!sub.isExact());

      auto const ecls = exact.cls();
      if (!isectNonReg && !ecls.mightBeRegular()) return fail();

      DCls::IsectSet set;
      if (sub.isSub()) {
        auto scls = sub.cls();
        if (!isectNonReg && subNonReg) {
          auto const w = scls.withoutNonRegular();
          if (!w) return fail();
          scls = *w;
        }
        set.emplace_back(scls);
      } else if (sub.isIsect()) {
        if (!isectNonReg && subNonReg) {
          auto const u = res::Class::removeNonRegular(sub.isect());
          if (u.empty()) return fail();
          set.insert(std::end(set), std::begin(u), std::end(u));
        } else {
          set = sub.isect();
        }
      } else {
        assertx(sub.isIsectAndExact());
        auto const [ecls2, i] = sub.isectAndExact();

        auto e = isObj ? objExact(ecls) : clsExact(ecls, isectNonReg);
        if (e.is(BBottom)) return fail();
        auto e2 = isObj ? objExact(ecls2) : clsExact(ecls2, isectNonReg);
        if (e2.is(BBottom)) return fail();

        auto rhs = [&, i=i] {
          if (i->size() == 1) {
            return isObj
              ? subObj(i->front())
              : subCls(i->front(), isectNonReg);
          } else {
            DCls::IsectSet set{i->begin(), i->end()};
            return isObj
              ? isectObjInternal(std::move(set))
              : isectClsInternal(std::move(set), isectNonReg);
          }
        }();
        if (rhs.is(BBottom)) return fail();

        auto ret = intersection_of(
          intersection_of(std::move(e), std::move(e2)),
          std::move(rhs)
        );
        if (ret.is(BBottom)) return fail();
        return setctx(reuse(ret), ctx);
      }

      set.erase(
        std::remove_if(
          begin(set), end(set),
          [&] (res::Class c) { return c.same(ecls); }
        ),
        set.end()
      );

      auto ret = isObj
        ? (set.empty()
           ? objExact(ecls)
           : isectAndExactObjInternal(ecls, std::move(set))
          )
        : (set.empty()
           ? clsExact(ecls, isectNonReg)
           : isectAndExactClsInternal(ecls, std::move(set), isectNonReg)
          );
      if (ret.is(BBottom)) return fail();
      return setctx(reuse(ret), ctx);
    };

    auto const handleIsectAndExact = [&] (DCls& isectExact, Type rhs) {
      assertx(isectExact.isIsectAndExact());

      auto const [e, i] = isectExact.isectAndExact();
      auto exact =
        isObj ? objExact(e) : clsExact(e, isectNonReg);
      if (exact.is(BBottom)) return fail();

      auto lhs = [&, i=i] {
        if (i->size() == 1) {
          return isObj
            ? subObj(i->front())
            : subCls(i->front(), isectNonReg);
        } else {
          DCls::IsectSet set{i->begin(), i->end()};
          return isObj
            ? isectObjInternal(std::move(set))
            : isectClsInternal(std::move(set), isectNonReg);
        }
      }();
      if (lhs.is(BBottom)) return fail();

      auto ret = intersection_of(
        intersection_of(std::move(lhs), std::move(rhs)),
        std::move(exact)
      );
      if (ret.is(BBottom)) return fail();
      return setctx(reuse(ret), ctx);
    };

    if (acls.isExact()) {
      return handleExact(acls, bcls, nonRegB);
    } else if (bcls.isExact()) {
      return handleExact(bcls, acls, nonRegA);
    } else if (acls.isIsectAndExact()) {
      return handleIsectAndExact(acls, std::move(b));
    } else if (bcls.isIsectAndExact()) {
      return handleIsectAndExact(bcls, std::move(a));
    }

    auto const i = [&] {
      if (acls.isIsect()) {
        if (bcls.isIsect()) {
          return res::Class::intersect(
            acls.isect(),
            bcls.isect(),
            nonRegA,
            nonRegB,
            isectNonReg
          );
        } else {
          return res::Class::intersect(
            acls.isect(),
            std::array<res::Class, 1>{bcls.cls()},
            nonRegA,
            nonRegB,
            isectNonReg
          );
        }
      } else if (bcls.isIsect()) {
        return res::Class::intersect(
          std::array<res::Class, 1>{acls.cls()},
          bcls.isect(),
          nonRegA,
          nonRegB,
          isectNonReg
        );
      } else {
        return res::Class::intersect(
          std::array<res::Class, 1>{acls.cls()},
          std::array<res::Class, 1>{bcls.cls()},
          nonRegA,
          nonRegB,
          isectNonReg
        );
      }
    }();

    // Empty list here means the intersection is empty, which
    // shouldn't happen because we already checked that they could be
    // each other.
    assertx(!i.empty());
    assertx(IMPLIES(!nonRegA || !nonRegB, !isectNonReg));
    if (i.size() == 1) {
      auto ret = isObj
        ? subObj(i.front())
        : subCls(i.front(), isectNonReg);
      return setctx(reuse(ret), ctx);
    }

    DCls::IsectSet set{i.begin(), i.end()};
    auto ret = isObj
      ? isectObjInternal(std::move(set))
      : isectClsInternal(std::move(set), isectNonReg);
    return setctx(reuse(ret), ctx);
  };

  if (is_specialized_obj(a)) {
    if (is_specialized_obj(b)) {
      assertx(!is_specialized_wait_handle(a));
      assertx(!is_specialized_wait_handle(b));
      assertx(couldBe(isect, BObj));
      return isectDCls(a.m_data.dobj, b.m_data.dobj, true);
    }
    if (couldBe(isect, BObj)) return reuse(a);
  } else if (is_specialized_obj(b)) {
    if (couldBe(isect, BObj)) return reuse(b);
  }

  if (is_specialized_cls(a)) {
    if (is_specialized_cls(b)) {
      assertx(couldBe(isect, BCls));
      return isectDCls(a.m_data.dcls, b.m_data.dcls, false);
    }
    if (couldBe(isect, BCls)) return reuse(a);
  } else if (is_specialized_cls(b)) {
    if (couldBe(isect, BCls)) return reuse(b);
  }

  // Attempt to re-use t or intersect it with the isect bits.
  auto const adjustArrSpec = [&] (Type& t) {
    if (t.m_dataTag == DataTag::ArrLikeVal) {
      // ArrLikeVal doesn't require any intersection
      if (t.couldBe(BArrLikeE) && !couldBe(isect, BArrLikeE)) {
        // If the intersection stripped empty bits off leaving just a
        // ArrLikeVal, we need to restore the precise LegacyMark.
        mark = legacyMarkFromSArr(t.m_data.aval);
      }
      return reuse(t);
    }
    // Optimization: if t's existing trep is the same as the
    // intersection (array wise), the intersection won't modify
    // anything, so we can skip it.
    if ((t.bits() & BArrLikeN) == (isect & BArrLikeN)) {
      return reuse(t);
    }
    // Otherwise we can't reuse t and must perform the intersection.
    return Type{isect}.dispatchArrLikeNone(
      t, DualDispatchIntersection{ isect, mark }
    );
  };

  // Arrays need more care because we have to intersect the
  // specialization with a DArrNone to constrain the specialization
  // types appropriately.
  if (is_specialized_array_like(a)) {
    if (is_specialized_array_like(b)) {
      assertx(couldBe(isect, BArrLikeN));
      auto const i = [&] {
        if (a.dualDispatchDataFn(b, DualDispatchSubtype<true>{})) {
          return adjustArrSpec(a);
        }
        if (b.dualDispatchDataFn(a, DualDispatchSubtype<true>{})) {
          return adjustArrSpec(b);
        }
        return a.dualDispatchDataFn(
          b,
          DualDispatchIntersection{ isect, mark }
        );
      }();
      if (!i.is(BBottom)) return i;
      isect &= ~BArrLikeN;
      mark = project(mark, isect);
      return isect ? nodata() : TBottom;
    }
    if (couldBe(isect, BArrLikeN)) {
      auto const i = adjustArrSpec(a);
      if (!i.is(BBottom)) return i;
      isect &= ~BArrLikeN;
      mark = project(mark, isect);
      return isect ? nodata() : TBottom;
    }
  } else if (is_specialized_array_like(b)) {
    if (couldBe(isect, BArrLikeN)) {
      auto const i = adjustArrSpec(b);
      if (!i.is(BBottom)) return i;
      isect &= ~BArrLikeN;
      mark = project(mark, isect);
      return isect ? nodata() : TBottom;
    }
  }

  if (is_specialized_string(a)) {
    if (is_specialized_string(b)) {
      assertx(couldBe(isect, BStr));
      if (a.m_data.sval == b.m_data.sval) return reuse(a);
      isect &= ~BStr;
      return isect ? nodata() : TBottom;
    }
    if (couldBe(isect, BStr)) return reuse(a);
  } else if (is_specialized_string(b)) {
    if (couldBe(isect, BStr)) return reuse(b);
  }

  if (is_specialized_lazycls(a)) {
    if (is_specialized_lazycls(b)) {
      assertx(couldBe(isect, BLazyCls));
      if (a.m_data.lazyclsval == b.m_data.lazyclsval) return reuse(a);
      isect &= ~BLazyCls;
      return isect ? nodata() : TBottom;
    }
    if (couldBe(isect, BLazyCls)) return reuse(a);
  } else if (is_specialized_lazycls(b)) {
    if (couldBe(isect, BLazyCls)) return reuse(b);
  }

  if (is_specialized_ecl(a)) {
    if (is_specialized_ecl(b)) {
      assertx(couldBe(isect, BEnumClassLabel));
      if (a.m_data.eclval == b.m_data.eclval) return reuse(a);
      isect &= ~BEnumClassLabel;
      return isect ? nodata() : TBottom;
    }
    if (couldBe(isect, BEnumClassLabel)) return reuse(a);
  } else if (is_specialized_ecl(b)) {
    if (couldBe(isect, BEnumClassLabel)) return reuse(b);
  }

  if (is_specialized_int(a)) {
    if (is_specialized_int(b)) {
      assertx(couldBe(isect, BInt));
      if (a.m_data.ival == b.m_data.ival) return reuse(a);
      isect &= ~BInt;
      return isect ? nodata() : TBottom;
    }
    if (couldBe(isect, BInt)) return reuse(a);
  } else if (is_specialized_int(b)) {
    if (couldBe(isect, BInt)) return reuse(b);
  }

  if (is_specialized_double(a)) {
    if (is_specialized_double(b)) {
      assertx(couldBe(isect, BDbl));
      if (double_equals(a.m_data.dval, b.m_data.dval)) return reuse(a);
      isect &= ~BDbl;
      return isect ? nodata() : TBottom;
    }
    if (couldBe(isect, BDbl)) return reuse(a);
  } else if (is_specialized_double(b)) {
    if (couldBe(isect, BDbl)) return reuse(b);
  }

  return nodata();
}

Type union_of(Type a, Type b) {
  auto const combined = a.bits() | b.bits();
  auto const mark = unionOf(a.m_legacyMark, b.m_legacyMark);

  auto const nodata = [&] { return Type { combined, mark }; };
  if (!a.hasData() && !b.hasData()) return nodata();

  auto const reuse = [&] (Type& dst) {
    dst.m_bits = combined;
    dst.m_legacyMark = mark;
    assertx(dst.checkInvariants());
    return std::move(dst);
  };

  // Check if the given DWaitHandle or DObj is a subtype of each
  // other, returning the less specific type. Returns std::nullopt if
  // neither of them is.
  auto const whAndObj = [&] (Type& wh, Type& obj) -> Optional<Type> {
    // The order of checks is important here. If the obj is the
    // Awaitable object, then both subtype checks will pass (because
    // they're equal), but we should reuse obj in that case, so check
    // that first.
    if (subtypeCls<true>(wh.m_data.dwh->cls, obj.m_data.dobj)) return reuse(obj);
    if (subtypeCls<true>(obj.m_data.dobj, wh.m_data.dwh->cls)) {
      // The object specialization has neither an inner type nor eagerness
      // information, so it cannot retain the DWaitHandle specialization.
      return std::nullopt;
    }
    return std::nullopt;
  };

  // If both sides have the same specialization, check if they are
  // equal. If they are, reuse one of the sides. Otherwise if they are
  // not, return nodata() (we assume the union of two unequal
  // specializations is the full type). If only one side has a
  // specialization, check if the other side has the support bits. If
  // it does not, we can reuse the type with the specialization (since
  // it's not being unioned with anything on the other
  // side). Otherwise we have nodata(). If the union contains more
  // than one set of support bits, we can never keep any
  // specialization.

  // For wait handles, if one is a specialized object try to determine
  // if one of them is a subtype of the other. If not, demote the wait
  // handle to a DObj and use that.
  if (is_specialized_wait_handle(a)) {
    if (is_specialized_wait_handle(b)) {
      auto const& atype = a.m_data.dwh->inner;
      auto const& btype = b.m_data.dwh->inner;
      auto const canSuspend =
        a.m_data.dwh->canSuspend || b.m_data.dwh->canSuspend;
      auto u = union_of(atype, btype);
      auto cls = union_of(
        demote_wait_handle(std::move(a)),
        demote_wait_handle(std::move(b))
      );
      if (cls.m_dataTag != DataTag::Obj) return cls;
      return Type::specializeWaitHandle(
        std::move(cls),
        std::move(u),
        canSuspend
      );
    }
    if (is_specialized_obj(b)) {
      if (auto const t = whAndObj(a, b)) return *t;
      a = demote_wait_handle(std::move(a));
    } else {
      if (b.couldBe(BObj) || !subtypeOf(combined, BObj | kNonSupportBits)) {
        return nodata();
      }
      return reuse(a);
    }
  } else if (is_specialized_wait_handle(b)) {
    if (is_specialized_obj(a)) {
      if (auto const t = whAndObj(b, a)) return *t;
      b = demote_wait_handle(std::move(b));
    } else {
      if (a.couldBe(BObj) || !subtypeOf(combined, BObj | kNonSupportBits)) {
        return nodata();
      }
      return reuse(b);
    }
  }

  auto const unionDcls = [&] (const DCls& acls, const DCls& bcls, bool isObj) {
    auto const isCtx = acls.isCtx() && bcls.isCtx();
    if (subtypeCls<false>(acls, bcls)) return setctx(reuse(b), isCtx);
    if (subtypeCls<false>(bcls, acls)) return setctx(reuse(a), isCtx);

    auto const nonRegA = acls.containsNonRegular();
    auto const nonRegB = bcls.containsNonRegular();

    auto const handleIsectAndExact = [&] (const DCls& isectExact,
                                          Type rhs,
                                          bool nonReg) {
      assertx(isectExact.isIsectAndExact());

      auto const [e, i] = isectExact.isectAndExact();
      auto exact =
        isObj ? objExact(e) : clsExact(e, nonReg);
      assertx(!exact.is(BBottom));

      auto lhs = [&, i=i] {
        if (i->size() == 1) {
          return isObj
            ? subObj(i->front())
            : subCls(i->front(), nonReg);
        } else {
          DCls::IsectSet set{i->begin(), i->end()};
          return isObj
            ? isectObjInternal(std::move(set))
            : isectClsInternal(std::move(set), nonReg);
        }
      }();
      assertx(!lhs.is(BBottom));

      auto ret = intersection_of(
        union_of(std::move(exact), rhs),
        union_of(std::move(lhs), rhs)
      );
      assertx(!ret.is(BBottom));
      return setctx(reuse(ret), isCtx);
    };

    if (acls.isIsectAndExact()) {
      return handleIsectAndExact(acls, std::move(b), nonRegA);
    } else if (bcls.isIsectAndExact()) {
      return handleIsectAndExact(bcls, std::move(a), nonRegB);
    }

    auto const u = [&] {
      if (acls.isIsect()) {
        if (bcls.isIsect()) {
          return res::Class::combine(
            acls.isect(),
            bcls.isect(),
            true,
            true,
            nonRegA,
            nonRegB
          );
        } else {
          return res::Class::combine(
            acls.isect(),
            std::array<res::Class, 1>{bcls.cls()},
            true,
            !bcls.isExact(),
            nonRegA,
            nonRegB
          );
        }
      } else if (bcls.isIsect()) {
        return res::Class::combine(
          std::array<res::Class, 1>{acls.cls()},
          bcls.isect(),
          !acls.isExact(),
          true,
          nonRegA,
          nonRegB
        );
      } else {
        return res::Class::combine(
          std::array<res::Class, 1>{acls.cls()},
          std::array<res::Class, 1>{bcls.cls()},
          !acls.isExact(),
          !bcls.isExact(),
          nonRegA,
          nonRegB
        );
      }
    }();

    // Empty list means there's no classes in common, which means it's
    // just a TObj/TCls.
    if (u.empty()) return nodata();
    if (u.size() == 1) {
      // We need not to distinguish between Obj<=T and Obj=T, and
      // always return an Obj<=Ancestor, because that is the single
      // type that includes both children.
      auto ret = isObj
        ? subObj(u.front())
        : subCls(u.front(), nonRegA || nonRegB);
      assertx(!ret.is(BBottom));
      return setctx(reuse(ret), isCtx);
    }

    DCls::IsectSet set{u.begin(), u.end()};
    auto ret = isObj
      ? isectObjInternal(std::move(set))
      : isectClsInternal(std::move(set), nonRegA || nonRegB);
    assertx(!ret.is(BBottom));
    return setctx(reuse(ret), isCtx);
  };

  if (is_specialized_obj(a)) {
    if (is_specialized_obj(b)) {
      assertx(!is_specialized_wait_handle(a));
      assertx(!is_specialized_wait_handle(b));
      return unionDcls(a.m_data.dobj, b.m_data.dobj, true);
    }
    if (b.couldBe(BObj) || !subtypeOf(combined, BObj | kNonSupportBits)) {
      return nodata();
    }
    return reuse(a);
  } else if (is_specialized_obj(b)) {
    if (a.couldBe(BObj) || !subtypeOf(combined, BObj | kNonSupportBits)) {
      return nodata();
    }
    return reuse(b);
  }

  if (is_specialized_cls(a)) {
    if (is_specialized_cls(b)) return unionDcls(a.m_data.dcls, b.m_data.dcls, false);
    if (b.couldBe(BCls) || !subtypeOf(combined, BCls | kNonSupportBits)) {
      return nodata();
    }
    return reuse(a);
  } else if (is_specialized_cls(b)) {
    if (a.couldBe(BCls) || !subtypeOf(combined, BCls | kNonSupportBits)) {
      return nodata();
    }
    return reuse(b);
  }

  if (is_specialized_array_like(a)) {
    if (is_specialized_array_like(b)) {
      if (a.dualDispatchDataFn(b, DualDispatchSubtype<true>{})) return reuse(b);
      if (b.dualDispatchDataFn(a, DualDispatchSubtype<true>{})) return reuse(a);
      return a.dualDispatchDataFn(b, DualDispatchUnion{ combined, mark });
    }
    if (!subtypeOf(combined, BArrLikeN | kNonSupportBits)) return nodata();
    if (!b.couldBe(BArrLikeN)) return reuse(a);
    if (b.dispatchArrLikeNone(a, DualDispatchSubtype<true>{})) return reuse(a);
    return b.dispatchArrLikeNone(a, DualDispatchUnion{ combined, mark });
  } else if (is_specialized_array_like(b)) {
    if (!subtypeOf(combined, BArrLikeN | kNonSupportBits)) return nodata();
    if (!a.couldBe(BArrLikeN)) return reuse(b);
    if (a.dispatchArrLikeNone(b, DualDispatchSubtype<true>{})) return reuse(b);
    return a.dispatchArrLikeNone(b, DualDispatchUnion{ combined, mark });
  }

  if (is_specialized_string(a)) {
    if (is_specialized_string(b)) {
      if (a.m_data.sval == b.m_data.sval) return reuse(a);
      return nodata();
    }
    if (b.couldBe(BStr) || !subtypeOf(combined, BStr | kNonSupportBits)) {
      return nodata();
    }
    return reuse(a);
  } else if (is_specialized_string(b)) {
    if (a.couldBe(BStr) || !subtypeOf(combined, BStr | kNonSupportBits)) {
      return nodata();
    }
    return reuse(b);
  }

  if (is_specialized_lazycls(a)) {
    if (is_specialized_lazycls(b)) {
      if (a.m_data.lazyclsval == b.m_data.lazyclsval) return reuse(a);
      return nodata();
    }
    if (b.couldBe(BLazyCls) ||
        !subtypeOf(combined, BLazyCls | kNonSupportBits)) {
      return nodata();
    }
    return reuse(a);
  } else if (is_specialized_lazycls(b)) {
    if (a.couldBe(BLazyCls) ||
        !subtypeOf(combined, BLazyCls | kNonSupportBits)) {
      return nodata();
    }
    return reuse(b);
  }

  if (is_specialized_ecl(a)) {
    if (is_specialized_ecl(b)) {
      if (a.m_data.eclval == b.m_data.eclval) return reuse(a);
      return nodata();
    }
    if (b.couldBe(BEnumClassLabel) ||
        !subtypeOf(combined, BEnumClassLabel | kNonSupportBits)) {
      return nodata();
    }
    return reuse(a);
  } else if (is_specialized_ecl(b)) {
    if (a.couldBe(BEnumClassLabel) ||
        !subtypeOf(combined, BEnumClassLabel | kNonSupportBits)) {
      return nodata();
    }
    return reuse(b);
  }

  if (is_specialized_int(a)) {
    if (is_specialized_int(b)) {
      if (a.m_data.ival == b.m_data.ival) return reuse(a);
      return nodata();
    }
    if (b.couldBe(BInt) || !subtypeOf(combined, BInt | kNonSupportBits)) {
      return nodata();
    }
    return reuse(a);
  } else if (is_specialized_int(b)) {
    if (a.couldBe(BInt) || !subtypeOf(combined, BInt | kNonSupportBits)) {
      return nodata();
    }
    return reuse(b);
  }

  if (is_specialized_double(a)) {
    if (is_specialized_double(b)) {
      if (double_equals(a.m_data.dval, b.m_data.dval)) return reuse(a);
      return nodata();
    }
    if (b.couldBe(BDbl) || !subtypeOf(combined, BDbl | kNonSupportBits)) {
      return nodata();
    }
    return reuse(a);
  } else if (is_specialized_double(b)) {
    if (a.couldBe(BDbl) || !subtypeOf(combined, BDbl | kNonSupportBits)) {
      return nodata();
    }
    return reuse(b);
  }

  always_assert(false);
}

bool could_have_magic_bool_conversion(const Type& t) {
  if (!t.couldBe(BObj)) return false;
  if (!is_specialized_obj(t)) return true;
  auto const& dobj = dobj_of(t);
  if (dobj.isIsect()) {
    for (auto const cls : dobj.isect()) {
      if (!cls.couldHaveMagicBool()) return false;
    }
    return true;
  } else if (dobj.isIsectAndExact()) {
    auto const [e, i] = dobj.isectAndExact();
    if (!e.couldHaveMagicBool()) return false;
    for (auto const cls : *i) {
      if (!cls.couldHaveMagicBool()) return false;
    }
    return true;
  }
  return dobj.cls().couldHaveMagicBool();
}

std::pair<Emptiness, bool> emptiness(const Type& t) {
  assertx(t.subtypeOf(BCell));

  auto const emptyMask = BNull | BFalse | BArrLikeE;
  auto const nonEmptyMask =
    BTrue | BArrLikeN | BObj | BCls | BLazyCls | BFunc |
    BRFunc | BClsMeth | BRClsMeth | BEnumClassLabel;
  auto const bothMask =
    BCell & ~(emptyMask | nonEmptyMask | BInt | BDbl | BStr);
  auto empty = t.couldBe(emptyMask | bothMask);
  auto nonempty = t.couldBe(nonEmptyMask | bothMask);
  auto effectfree =
    t.subtypeOf(BPrim | BStr | BObj | BArrLike | BCls | BLazyCls |
                BFunc | BRFunc | BClsMeth | BRClsMeth | BEnumClassLabel);

  if (could_have_magic_bool_conversion(t)) {
    empty = true;
    effectfree = false;
  }

  auto const check = [&] (TypedValue tv) {
    (tvToBool(tv) ? nonempty : empty) = true;
  };

  if (t.couldBe(BInt)) {
    if (is_specialized_int(t)) {
      check(make_tv<KindOfInt64>(t.m_data.ival));
    } else {
      empty = true;
      nonempty = true;
    }
  }
  if (t.couldBe(BDbl)) {
    if (is_specialized_double(t)) {
      check(make_tv<KindOfDouble>(t.m_data.dval));
    } else {
      empty = true;
      nonempty = true;
    }
  }
  if (t.couldBe(BStr)) {
    if (is_specialized_string(t)) {
      check(make_tv<KindOfString>(t.m_data.sval));
    } else {
      empty = true;
      nonempty = true;
    }
  }

  if (nonempty) {
    return std::make_pair(
      empty ? Emptiness::Maybe : Emptiness::NonEmpty,
      effectfree
    );
  }
  assertx(empty || t.is(BBottom));
  return std::make_pair(Emptiness::Empty, effectfree);
}

void widen_type_impl(Type& t, uint32_t depth) {
  // Right now to guarantee termination we need to just limit the nesting depth
  // of the type to a fixed degree.
  auto const checkDepth = [&] {
    if (depth >= kTypeWidenMaxDepth) {
      t = Type { t.bits(), t.m_legacyMark };
      return true;
    }
    return false;
  };

  switch (t.m_dataTag) {
    case DataTag::None:
    case DataTag::Str:
    case DataTag::LazyCls:
    case DataTag::EnumClassLabel:
    case DataTag::Int:
    case DataTag::Dbl:
    case DataTag::Obj:
    case DataTag::Cls:
    case DataTag::ArrLikeVal:
      return;

    case DataTag::WaitHandle: {
      auto wh = t.m_data.dwh.mutate();
      widen_type_impl(wh->inner, depth + 1);
      if (!wh->inner.strictSubtypeOf(BInitCell) && wh->canSuspend) {
        t = demote_wait_handle(std::move(t));
      }
      return;
    }

    case DataTag::ArrLikePacked: {
      if (checkDepth()) return;
      auto& packed = *t.m_data.packed.mutate();
      for (auto& e : packed.elems) {
        widen_type_impl(e, depth + 1);
      }
      return;
    }

    case DataTag::ArrLikePackedN: {
      if (checkDepth()) return;
      auto& packed = *t.m_data.packedn.mutate();
      widen_type_impl(packed.type, depth + 1);
      return;
    }

    case DataTag::ArrLikeMap: {
      if (checkDepth()) return;
      auto& map = *t.m_data.map.mutate();
      for (auto it = map.map.begin(); it != map.map.end(); it++) {
        auto temp = it->second.val;
        widen_type_impl(temp, depth + 1);
        map.map.update(it, it->second.withType(std::move(temp)));
      }
      if (map.hasOptElements()) {
        // Key must be at least ArrKey, which doesn't need widening.
        widen_type_impl(map.optVal, depth + 1);
      }
      return;
    }

    case DataTag::ArrLikeMapN: {
      if (checkDepth()) return;
      auto& map = *t.m_data.mapn.mutate();
      // Key must be at least ArrKey, which doesn't need widening.
      widen_type_impl(map.val, depth + 1);
      return;
    }
  }

  not_reached();
}

Type widen_type(Type t) {
  widen_type_impl(t, 0);
  return t;
}

Type widening_union(const Type& a, const Type& b) {
  return widen_type(union_of(a, b));
}

Type stack_flav(Type a) {
  if (a.subtypeOf(BUninit))   return TUninit;
  if (a.subtypeOf(BInitCell)) return TInitCell;
  always_assert(0 && "stack_flav passed invalid type");
}

Type loosen_string_staticness(Type t) {
  auto bits = t.bits();
  if (couldBe(bits, BStr)) bits |= BStr;
  t.m_bits = bits;
  assertx(t.checkInvariants());
  return t;
}

Type loosen_array_staticness(Type t) {
  if (!t.couldBe(BArrLike)) return t;

  auto bits = t.bits();
  auto const check = [&] (trep a) {
    if (couldBe(bits, a)) bits |= a;
  };
  check(BVecE);
  check(BVecN);
  check(BDictE);
  check(BDictN);
  check(BKeysetE);
  check(BKeysetN);

  // If the array has no specialization, but it was static, then
  // (implicitly) the contents must also be static. If we simply make
  // the array bits non-static, we lose this information. In this
  // case, add a specialization to the new non-static array to
  // preserve the knowledge about the inner elements. This is a rare
  // case of loosening *adding* a specialization.
  if (t.m_dataTag == DataTag::None &&
      couldBe(bits, BArrLikeN) &&
      subtypeOf(bits, BArrLikeN | kNonSupportBits)) {
    auto const oldKeyBits = allowedKeyBits(t.bits()).first;
    auto const oldValBits = allowedValBits(t.bits(), false).first;
    auto const newKeyBits = allowedKeyBits(bits).first;
    auto const newValBits = allowedValBits(bits, false).first;
    assertx(subtypeOf(oldKeyBits, newKeyBits));
    assertx(subtypeOf(oldValBits, newValBits));
    if (oldKeyBits != newKeyBits || oldValBits != newValBits) {
      if (subtypeAmong(bits, BVecN, BArrLikeN)) {
        assertx(oldKeyBits == newKeyBits);
        return packedn_impl(bits, t.m_legacyMark, Type{oldValBits});
      } else {
        return mapn_impl(
          bits,
          t.m_legacyMark,
          Type{oldKeyBits},
          Type{oldValBits}
        );
      }
    }
  } else if (t.m_dataTag == DataTag::ArrLikeVal) {
    return toDArrLike(t.m_data.aval, bits, t.m_legacyMark);
  }
  t.m_bits = bits;
  assertx(t.checkInvariants());
  return t;
}

Type loosen_staticness(Type t) {
  if (t.m_dataTag == DataTag::ArrLikeVal) {
    // ArrLikeVal always needs static ArrLikeN support, so we if we
    // want to loosen the trep, we need to convert to the equivalent
    // DArrLike instead.
    return loosen_staticness(toDArrLike(t.m_data.aval, t.bits(), t.m_legacyMark));
  }

  auto bits = t.bits();
  auto const check = [&] (trep a) {
    if (couldBe(bits, a)) bits |= a;
  };
  check(BStr);
  check(BLazyCls);
  check(BEnumClassLabel);
  check(BVecE);
  check(BVecN);
  check(BDictE);
  check(BDictN);
  check(BKeysetE);
  check(BKeysetN);

  t.m_bits = bits;

  switch (t.m_dataTag) {
    case DataTag::None:
    case DataTag::Str:
    case DataTag::LazyCls:
    case DataTag::EnumClassLabel:
    case DataTag::Int:
    case DataTag::Dbl:
    case DataTag::Obj:
    case DataTag::Cls:
      break;

    case DataTag::WaitHandle: {
      auto wh = t.m_data.dwh.mutate();
      auto& inner = wh->inner;
      auto loosened = loosen_staticness(std::move(inner));
      if (!loosened.strictSubtypeOf(BInitCell) && wh->canSuspend) {
        return demote_wait_handle(t);
      }
      inner = std::move(loosened);
      break;
    }

    case DataTag::ArrLikePacked: {
      auto& packed = *t.m_data.packed.mutate();
      for (auto& e : packed.elems) {
        e = loosen_staticness(std::move(e));
      }
      break;
    }

    case DataTag::ArrLikePackedN: {
      auto& packed = *t.m_data.packedn.mutate();
      auto loosened = loosen_staticness(std::move(packed.type));
      if (t.subtypeAmong(BVecN, BArrLikeN) &&
          !loosened.strictSubtypeOf(BInitCell)) {
        return Type { bits, t.m_legacyMark };
      }
      packed.type = std::move(loosened);
      break;
    }

    case DataTag::ArrLikeMap: {
      auto& map = *t.m_data.map.mutate();
      for (auto it = map.map.begin(); it != map.map.end(); ++it) {
        auto val = loosen_staticness(it->second.val);
        map.map.update(
          it,
          MapElem {
            std::move(val),
            !isIntType(it->first.m_type) ? TriBool::Maybe : TriBool::Yes
          }
        );
      }
      map.optKey = loosen_staticness(std::move(map.optKey));
      map.optVal = loosen_staticness(std::move(map.optVal));
      break;
    }

    case DataTag::ArrLikeMapN: {
      auto& map = *t.m_data.mapn.mutate();
      auto loosenedKey = loosen_staticness(std::move(map.key));
      auto loosenedVal = loosen_staticness(std::move(map.val));
      auto const keyBits = allowedKeyBits(bits);
      auto const valBits = allowedValBits(bits, false);
      if (!loosenedKey.strictSubtypeOf(keyBits.first) &&
          !loosenedVal.strictSubtypeOf(valBits.first)) {
        return Type { bits, t.m_legacyMark };
      }
      map.key = std::move(loosenedKey);
      map.val = std::move(loosenedVal);
      break;
    }

    case DataTag::ArrLikeVal:
      always_assert(false);
  }

  assertx(t.checkInvariants());
  return t;
}

Type loosen_vec_or_dict(Type t) {
  if (t.couldBe(BVec | BDict))  t |= union_of(TVec, TDict);
  return t;
}

Type loosen_string_values(Type t) {
  return t.m_dataTag == DataTag::Str
    ? Type { t.bits(), t.m_legacyMark } : t;
}

Type loosen_array_values(Type a) {
  switch (a.m_dataTag) {
    case DataTag::ArrLikeVal:
    case DataTag::ArrLikePacked:
    case DataTag::ArrLikePackedN:
    case DataTag::ArrLikeMap:
    case DataTag::ArrLikeMapN:
      return Type { a.bits(), a.m_legacyMark };
    case DataTag::None:
    case DataTag::Str:
    case DataTag::LazyCls:
    case DataTag::EnumClassLabel:
    case DataTag::Int:
    case DataTag::Dbl:
    case DataTag::Obj:
    case DataTag::WaitHandle:
    case DataTag::Cls:
      return a;
  }
  not_reached();
}

Type loosen_values(Type a) {
  auto t = [&]{
    switch (a.m_dataTag) {
    case DataTag::Str:
    case DataTag::LazyCls:
    case DataTag::EnumClassLabel:
    case DataTag::Int:
    case DataTag::Dbl:
    case DataTag::ArrLikeVal:
    case DataTag::ArrLikePacked:
    case DataTag::ArrLikePackedN:
    case DataTag::ArrLikeMap:
    case DataTag::ArrLikeMapN:
      return Type { a.bits(), a.m_legacyMark };
    case DataTag::None:
    case DataTag::Obj:
    case DataTag::WaitHandle:
    case DataTag::Cls:
      return a;
    }
    not_reached();
  }();
  if (t.couldBe(BFalse) || t.couldBe(BTrue)) t |= TBool;
  return t;
}

Type loosen_emptiness(Type t) {
  auto const check = [&] (trep a, trep b) {
    if (t.couldBe(a)) {
      if (t.hasData() && kSupportBits & b & ~t.bits()) {
        t.destroyData();
        t.m_dataTag = DataTag::None;
      }
      t.m_bits |= b;
    }
  };
  check(BSVec,    BSVec);
  check(BCVec,    BVec);
  check(BSDict,   BSDict);
  check(BCDict,   BDict);
  check(BSKeyset, BSKeyset);
  check(BCKeyset, BKeyset);

  assertx(t.checkInvariants());
  return t;
}

Type loosen_likeness(Type t) {
  if (t.couldBe(BCls | BLazyCls)) t |= TSStr;
  return t;
}

Type loosen_likeness_recursively(Type t) {
  t = loosen_likeness(std::move(t));

  switch (t.m_dataTag) {
  case DataTag::None:
  case DataTag::Str:
  case DataTag::LazyCls:
  case DataTag::EnumClassLabel:
  case DataTag::Int:
  case DataTag::Dbl:
  case DataTag::Obj:
  case DataTag::Cls:
    break;

  case DataTag::ArrLikeVal:
    // Static arrays cannot currently contain function or class pointers.
    break;

  case DataTag::WaitHandle: {
    auto wh = t.m_data.dwh.mutate();
    auto& inner = wh->inner;
    auto loosened = loosen_likeness_recursively(inner);
    if (!loosened.strictSubtypeOf(BInitCell) && wh->canSuspend) {
      return demote_wait_handle(std::move(t));
    }
    inner = std::move(loosened);
    break;
  }

  case DataTag::ArrLikePacked: {
    auto& packed = *t.m_data.packed.mutate();
    for (auto& e : packed.elems) {
      e = loosen_likeness_recursively(std::move(e));
    }
    break;
  }

  case DataTag::ArrLikePackedN: {
    auto& packed = *t.m_data.packedn.mutate();
    auto loosened = loosen_likeness_recursively(packed.type);
    if (t.subtypeAmong(BVecN, BArrLikeN) &&
        !loosened.strictSubtypeOf(allowedValBits(t.bits(), true).first)) {
      return Type { t.bits(), t.m_legacyMark };
    }
    packed.type = std::move(loosened);
    break;
  }

  case DataTag::ArrLikeMap: {
    auto& map = *t.m_data.map.mutate();
    for (auto it = map.map.begin(); it != map.map.end(); it++) {
      auto val = loosen_likeness_recursively(it->second.val);
      map.map.update(it, it->second.withType(std::move(val)));
    }
    map.optVal = loosen_likeness_recursively(std::move(map.optVal));
    break;
  }

  case DataTag::ArrLikeMapN: {
    auto& map = *t.m_data.mapn.mutate();
    auto loosened = loosen_likeness_recursively(map.val);
    if (!map.key.strictSubtypeOf(allowedKeyBits(t.bits()).first) &&
        !loosened.strictSubtypeOf(allowedValBits(t.bits(), false).first)) {
      return Type { t.bits(), t.m_legacyMark };
    }
    map.val = std::move(loosened);
    break;
  }
  }

  assertx(t.checkInvariants());
  return t;
}

Type loosen_all(Type t) {
  return
    loosen_staticness(
      loosen_emptiness(
        loosen_likeness(
          loosen_values(
            std::move(t)
          )
        )
      )
    );
}

Type loosen_to_datatype(Type t) {
  if (t.couldBe(BFalse) || t.couldBe(BTrue)) t |= TBool;
  return loosen_staticness(
    loosen_emptiness(
      loosen_likeness(
        // The legacy mark is not part of a value's DataType -- a marked vec
        // and an unmarked one are both KindOfVec -- so a type loosened to its
        // DataType must not carry one.
        Type { t.bits(), project(LegacyMark::Unknown, t.bits()) }
      )
    )
  );
}

Type add_nonemptiness(Type t) {
  auto const check = [&] (trep a, trep b) {
    if (t.couldBe(a)) t.m_bits |= b;
  };
  check(BSVecE,    BSVecN);
  check(BCVecE,    BVecN);
  check(BSDictE,   BSDictN);
  check(BCDictE,   BDictN);
  check(BSKeysetE, BSKeysetN);
  check(BCKeysetE, BKeysetN);
  return t;
}

Type to_cell(Type t) {
  assertx(t.subtypeOf(BCell));
  if (!t.couldBe(BUninit)) return t;
  auto const bits = (t.bits() & ~BUninit) | BInitNull;
  t.m_bits = bits;
  assertx(t.checkInvariants());
  return t;
}

Type remove_uninit(Type t) {
  t.m_bits &= ~BUninit;
  assertx(t.checkInvariants());
  return t;
}

Type remove_data(Type t, trep support) {
  auto const bits = t.bits() & ~support;
  return Type { bits, project(t.m_legacyMark, bits) };
}

Type remove_int(Type t) {
  assertx(t.subtypeOf(BCell));
  if (t.m_dataTag == DataTag::Int) {
    return remove_data(std::move(t), BInt);
  }
  t.m_bits &= ~BInt;
  assertx(t.checkInvariants());
  return t;
}

Type remove_double(Type t) {
  assertx(t.subtypeOf(BCell));
  if (t.m_dataTag == DataTag::Dbl) {
    return remove_data(std::move(t), BDbl);
  }
  t.m_bits &= ~BDbl;
  assertx(t.checkInvariants());
  return t;
}

Type remove_string(Type t) {
  assertx(t.subtypeOf(BCell));
  if (t.m_dataTag == DataTag::Str) {
    return remove_data(std::move(t), BStr);
  }
  t.m_bits &= ~BStr;
  assertx(t.checkInvariants());
  return t;
}

Type remove_lazycls(Type t) {
  assertx(t.subtypeOf(BCell));
  if (t.m_dataTag == DataTag::LazyCls) {
    return remove_data(std::move(t), BLazyCls);
  }
  t.m_bits &= ~BLazyCls;
  assertx(t.checkInvariants());
  return t;
}

Type remove_cls(Type t) {
  assertx(t.subtypeOf(BCell));
  if (t.m_dataTag == DataTag::Cls) {
    return remove_data(std::move(t), BCls);
  }
  t.m_bits &= ~BCls;
  assertx(t.checkInvariants());
  return t;
}

Type remove_obj(Type t) {
  assertx(t.subtypeOf(BCell));
  if (is_specialized_obj(t)) {
    return remove_data(std::move(t), BObj);
  }
  t.m_bits &= ~BObj;
  assertx(t.checkInvariants());
  return t;
}

Type remove_keyset(Type t) {
  assertx(t.subtypeOf(BCell));
  if (!is_specialized_array_like(t)) {
    // If there's no specialization, we can just remove the bits.
    t.m_bits &= ~BKeyset;
    assertx(t.checkInvariants());
    return t;
  }
  auto removed = Type{t.bits() & ~BKeyset, t.m_legacyMark};
  // Otherwise use intersection_of to remove the Keyset while trying
  // to preserve the specialization
  return intersection_of(std::move(t), std::move(removed));
}

Type remove_bits(Type t, trep bits) {
  assertx(t.subtypeOf(BCell));

  // Find the support bits for the specialization, if any
  auto const support = [&] {
    switch (t.m_dataTag) {
      case DataTag::None:       return BBottom;
      case DataTag::Int:        return BInt;
      case DataTag::Dbl:        return BDbl;
      case DataTag::Str:        return BStr;
      case DataTag::LazyCls:    return BLazyCls;
      case DataTag::EnumClassLabel: return BEnumClassLabel;
      case DataTag::Obj:
      case DataTag::WaitHandle: return BObj;
      case DataTag::Cls:        return BCls;
      case DataTag::ArrLikePacked:
      case DataTag::ArrLikePackedN:
      case DataTag::ArrLikeMap:
      case DataTag::ArrLikeMapN:
      case DataTag::ArrLikeVal: return BArrLikeN;
    }
    not_reached();
  }();

  // If the support bits are present, remove the data while removing
  // the bits.
  if (couldBe(bits, support)) return remove_data(std::move(t), bits);
  // Otherwise we can just remove the bits
  auto const old = t.bits();
  t.m_bits &= ~bits;
  if (t.m_dataTag == DataTag::ArrLikeVal &&
      !t.couldBe(BArrLikeE) &&
      couldBe(old, BArrLikeE)) {
    // If removing the bits removes BArrLikeE with a ArrLikeVal
    // specialization, we need to restore the exact LegacyMark
    // corresponding to the static array.
    t.m_legacyMark = legacyMarkFromSArr(t.m_data.aval);
  } else {
    t.m_legacyMark = project(t.m_legacyMark, t.bits());
  }
  assertx(t.checkInvariants());
  return t;
}

std::pair<Type, Type> split_obj(Type t) {
  assertx(t.subtypeOf(BCell));
  auto const b = t.bits();
  if (is_specialized_obj(t)) {
    auto const mark = t.m_legacyMark;
    t.m_bits &= BObj;
    t.m_legacyMark = LegacyMark::Bottom;
    return std::make_pair(std::move(t), Type { b & ~BObj, mark });
  }
  t.m_bits &= ~BObj;
  return std::make_pair(Type { b & BObj, LegacyMark::Bottom }, std::move(t));
}

std::pair<Type, Type> split_cls(Type t) {
  assertx(t.subtypeOf(BCell));
  auto const b = t.bits();
  if (is_specialized_cls(t)) {
    auto const mark = t.m_legacyMark;
    t.m_bits &= BCls;
    t.m_legacyMark = LegacyMark::Bottom;
    return std::make_pair(std::move(t), Type { b & ~BCls, mark });
  }
  t.m_bits &= ~BCls;
  return std::make_pair(Type { b & BCls, LegacyMark::Bottom }, std::move(t));
}

std::pair<Type, Type> split_array_like(Type t) {
  assertx(t.subtypeOf(BCell));
  auto const b = t.bits();
  if (is_specialized_array_like(t)) {
    t.m_bits &= BArrLike;
    return std::make_pair(
      std::move(t),
      Type { b & ~BArrLike, LegacyMark::Bottom }
    );
  }
  auto const mark = t.m_legacyMark;
  t.m_bits &= ~BArrLike;
  t.m_legacyMark = LegacyMark::Bottom;
  return std::make_pair(Type { b & BArrLike, mark }, std::move(t));
}

std::pair<Type, Type> split_string(Type t) {
  assertx(t.subtypeOf(BCell));
  auto const b = t.bits();
  if (is_specialized_string(t)) {
    auto const mark = t.m_legacyMark;
    t.m_bits &= BStr;
    t.m_legacyMark = LegacyMark::Bottom;
    return std::make_pair(std::move(t), Type { b & ~BStr, mark });
  }
  t.m_bits &= ~BStr;
  return std::make_pair(Type { b & BStr, LegacyMark::Bottom }, std::move(t));
}

std::pair<Type, Type> split_lazycls(Type t) {
  assertx(t.subtypeOf(BCell));
  auto const b = t.bits();
  if (is_specialized_lazycls(t)) {
    auto const mark = t.m_legacyMark;
    t.m_bits &= BLazyCls;
    t.m_legacyMark = LegacyMark::Bottom;
    return std::make_pair(std::move(t), Type { b & ~BLazyCls, mark });
  }
  t.m_bits &= ~BLazyCls;
  return std::make_pair(Type { b & BLazyCls, LegacyMark::Bottom }, std::move(t));
}

Type assert_emptiness(Type t) {
  auto const stripVal = [&] (TypedValue tv, trep support) {
    if (tvToBool(tv)) t = remove_data(std::move(t), support);
  };

  if (t.couldBe(BArrLikeN)) {
    t = remove_data(std::move(t), BArrLikeN);
  }

  if (t.couldBe(BLazyCls)) {
    t = remove_data(std::move(t), BLazyCls);
  }

  if (t.couldBe(BCls)) {
    t = remove_data(std::move(t), BCls);
  }

  if (t.couldBe(BFunc)) {
    t = remove_data(std::move(t), BFunc);
  }

  if (t.couldBe(BRFunc)) {
    t = remove_data(std::move(t), BRFunc);
  }

  if (t.couldBe(BClsMeth)) {
    t = remove_data(std::move(t), BClsMeth);
  }

  if (t.couldBe(BRClsMeth)) {
    t = remove_data(std::move(t), BRClsMeth);
  }

  if (t.couldBe(BEnumClassLabel)) {
    t = remove_data(std::move(t), BEnumClassLabel);
  }

  if (!could_have_magic_bool_conversion(t) && t.couldBe(BObj)) {
    t = remove_data(std::move(t), BObj);
  }

  if (t.couldBe(BInt)) {
    if (is_specialized_int(t)) {
      stripVal(make_tv<KindOfInt64>(t.m_data.ival), BInt);
    } else {
      t = union_of(remove_int(std::move(t)), ival(0));
    }
  }

  if (t.couldBe(BStr)) {
    if (is_specialized_string(t)) {
      stripVal(make_tv<KindOfString>(t.m_data.sval), BStr);
    }
    // We can't do what we do for int or double and say that the value is a
    // specific value. Because both "" and "0" are false.
  }

  if (t.couldBe(BDbl)) {
    if (is_specialized_double(t)) {
      stripVal(make_tv<KindOfDouble>(t.m_data.dval), BDbl);
    } else {
      t = union_of(remove_double(std::move(t)), dval(0));
    }
  }

  t.m_bits &= ~BTrue;
  assertx(t.checkInvariants());
  return t;
}

Type assert_nonemptiness(Type t) {
  assertx(t.subtypeOf(BCell));

  auto const stripVal = [&] (TypedValue tv, trep support) {
    if (!tvToBool(tv)) t = remove_data(std::move(t), support);
  };

  switch (t.m_dataTag) {
    case DataTag::None:
    case DataTag::Obj:
    case DataTag::WaitHandle:
    case DataTag::Cls:
    case DataTag::ArrLikePacked:
    case DataTag::ArrLikePackedN:
    case DataTag::ArrLikeMap:
    case DataTag::ArrLikeMapN:
    case DataTag::ArrLikeVal:
    case DataTag::LazyCls:
    case DataTag::EnumClassLabel:
      break;
    case DataTag::Int:
      stripVal(make_tv<KindOfInt64>(t.m_data.ival), BInt);
      break;
    case DataTag::Dbl:
      stripVal(make_tv<KindOfDouble>(t.m_data.dval), BDbl);
      break;
    case DataTag::Str:
      stripVal(make_tv<KindOfString>(t.m_data.sval), BStr);
      break;
  }
  auto const old = t.bits();
  t.m_bits &= ~(BNull | BFalse | BArrLikeE);
  if (t.m_dataTag == DataTag::ArrLikeVal && couldBe(old, BArrLikeE)) {
    t.m_legacyMark = legacyMarkFromSArr(t.m_data.aval);
  } else {
    t.m_legacyMark = project(t.m_legacyMark, t.bits());
  }
  assertx(t.checkInvariants());
  return t;
}

//////////////////////////////////////////////////////////////////////

Type promote_classish(Type t) {
  if (!t.couldBe(BCls | BLazyCls)) return t;
  t.m_bits &= ~(BCls | BLazyCls);
  t.m_bits |= BSStr;

  if (t.m_dataTag == DataTag::LazyCls) {
    auto const name = t.m_data.lazyclsval;
    destroy(t.m_data.lazyclsval);
    construct(t.m_data.sval, name);
    t.m_dataTag = DataTag::Str;
  } else if (t.m_dataTag == DataTag::Cls) {
    // If there could be subclasses we don't know the exact name, so
    // must drop the specialization.
    if (!t.m_data.dcls.isExact()) {
      destroy(t.m_data.dcls);
      t.m_dataTag = DataTag::None;
    } else {
      auto const name = t.m_data.dcls.cls().name();
      destroy(t.m_data.dcls);
      construct(t.m_data.sval, name);
      t.m_dataTag = DataTag::Str;
    }
  } else {
    // Since t could be BCls or BLazyCls, it cannot have any
    // specialization other than the above two.
    assertx(t.m_dataTag == DataTag::None);
  }

  assertx(t.checkInvariants());
  return t;
}

//////////////////////////////////////////////////////////////////////

bool might_have_dcls(const Type& t) {
  // Only these types could contain a DCls.
  if (!t.couldBe(BCArrLikeN | BObj | BCls)) return false;
  switch (t.m_dataTag) {
    case DataTag::None:
    case DataTag::Int:
    case DataTag::Dbl:
    case DataTag::Str:
    case DataTag::LazyCls:
    case DataTag::EnumClassLabel:
    case DataTag::ArrLikeVal:
      return false;
    case DataTag::WaitHandle:
    case DataTag::ArrLikePacked:
    case DataTag::ArrLikePackedN:
    case DataTag::ArrLikeMap:
    case DataTag::ArrLikeMapN:
    case DataTag::Obj:
    case DataTag::Cls:
      return true;
  }
}

//////////////////////////////////////////////////////////////////////

// The COWer subclasses allow for traversing specializations while
// potentially COWing the types. We can avoid COWing the type (and all
// of it's parent types) until we know we're going to make a change.

struct COWer {
  virtual Type& operator()() = 0;
  virtual const Type& noCOW() const = 0;
};

struct WaitHandleCOWer : public COWer {
  WaitHandleCOWer(COWer& parent, const DWaitHandle& h)
    : parent{parent}, current{&h} {}
  const DWaitHandle& get() const { return *current; }
  DWaitHandle& getAndCOW() {
    if (!cowed) {
      auto& t = parent();
      assertx(t.m_dataTag == DataTag::WaitHandle);
      cowed = t.m_data.dwh.mutate();
      current = cowed;
    }
    return *cowed;
  }
  Type& operator()() { return getAndCOW().inner; }
  const Type& noCOW() const { return get().inner; }
  COWer& parent;
  DWaitHandle* cowed{nullptr};
  const DWaitHandle* current;
};

struct ArrLikePackedNCOWer : public COWer {
  ArrLikePackedNCOWer(COWer& parent, const DArrLikePackedN& h)
    : parent{parent}, current{&h} {}
  const DArrLikePackedN& get() const { return *current; }
  DArrLikePackedN& getAndCOW() {
    if (!cowed) {
      auto& t = parent();
      assertx(t.m_dataTag == DataTag::ArrLikePackedN);
      cowed = t.m_data.packedn.mutate();
      current = cowed;
    }
    return *cowed;
  }
  Type& operator()() { return getAndCOW().type; }
  const Type& noCOW() const { return get().type; }
  COWer& parent;
  DArrLikePackedN* cowed{nullptr};
  const DArrLikePackedN* current;
};

struct ArrLikePackedCOWer : public COWer {
  ArrLikePackedCOWer(COWer& parent, const DArrLikePacked& h)
    : parent{parent}, current{&h} {}
  const DArrLikePacked& get() const { return *current; }
  DArrLikePacked& getAndCOW() {
    if (!cowed) {
      auto& t = parent();
      assertx(t.m_dataTag == DataTag::ArrLikePacked);
      cowed = t.m_data.packed.mutate();
      current = cowed;
    }
    return *cowed;
  }
  Type& operator()() { return getAndCOW().elems[idx]; }
  const Type& noCOW() const { return currentElem(); }

  const Type& currentElem() const { return get().elems[idx]; }
  bool nextElem() {
    assertx(idx < get().elems.size());
    return ++idx < get().elems.size();
  }

  COWer& parent;
  DArrLikePacked* cowed{nullptr};
  const DArrLikePacked* current;
  size_t idx{0};
};

struct ArrLikeMapNCOWer: public COWer {
  ArrLikeMapNCOWer(COWer& parent, const DArrLikeMapN& h)
    : parent{parent}, current{&h} {}
  const DArrLikeMapN& get() const { return *current; }
  DArrLikeMapN& getAndCOW() {
    if (!cowed) {
      auto& t = parent();
      assertx(t.m_dataTag == DataTag::ArrLikeMapN);
      cowed = t.m_data.mapn.mutate();
      current = cowed;
    }
    return *cowed;
  }
  Type& operator()() { return isKey ? getAndCOW().key : getAndCOW().val; }
  const Type& noCOW() const { return keyOrValue(); }

  const Type& keyOrValue() const { return isKey ? get().key : get().val; }
  void toValue() { assertx(isKey); isKey = false; }

  COWer& parent;
  DArrLikeMapN* cowed{nullptr};
  const DArrLikeMapN* current;
  bool isKey{true};
};

struct ArrLikeMapCOWer : public COWer {
  ArrLikeMapCOWer(COWer& parent, const DArrLikeMap& h)
    : parent{parent}, current{&h} { iter = begin(current->map); }
  const DArrLikeMap& get() const { return *current; }
  DArrLikeMap& getAndCOW() {
    if (!cowed) {
      auto& t = parent();
      assertx(t.m_dataTag == DataTag::ArrLikeMap);
      cowed = t.m_data.map.mutate();
      current = cowed;
      iter = begin(cowed->map);
      for (size_t i = 0; i < idx; ++i) ++iter;
    }
    return *cowed;
  }
  Type& operator()() {
    auto& m = getAndCOW();
    if (iter != end(m.map)) return const_cast<Type&>(iter->second.val);
    return isOptKey ? m.optKey : m.optVal;
  }
  const Type& noCOW() const {
    auto const& m = get();
    if (iter != end(m.map)) return iter->second.val;
    return isOptKey ? m.optKey : m.optVal;
  }

  const Type& currentElem() const {
    assertx(iter != end(get().map));
    return iter->second.val;
  }
  bool nextElem() {
    assertx(iter != end(get().map));
    ++idx;
    return ++iter != end(get().map);
  }

  const Type& optKeyOrValue() const {
    return isOptKey ? get().optKey : get().optVal;
  }
  void toOptVal() {
    assertx(iter == end(get().map));
    assertx(isOptKey);
    isOptKey = false;
  }

  COWer& parent;
  DArrLikeMap* cowed{nullptr};
  const DArrLikeMap* current;
  size_t idx{0};
  MapElems::iterator iter;
  bool isOptKey{true};
};

struct TypeCOWer : public COWer {
  explicit TypeCOWer(Type& t) : t{t} {}
  Type& operator()() { return t; }
  const Type& noCOW() const { return t; }
  Type& t;
};

//////////////////////////////////////////////////////////////////////

void unserialize_classes_impl(const IIndex& index,
                              const Type& t,
                              COWer& parent) {
  SCOPE_EXIT { parent.noCOW().checkInvariants(); };

  auto const onDCls = [&] (const DCls& dcls, bool isObj) {
    auto newT = [&] {
      if (dcls.isIsect()) {
        auto const nonReg = dcls.containsNonRegular();
        auto out = TInitCell;
        for (auto const i : dcls.isect()) {
          auto const u = i.unserialize(index);
          if (!u) return TBottom;
          out &= isObj ? subObj(*u) : subCls(*u, nonReg);
        }
        return out;
      } else if (dcls.isIsectAndExact()) {
        auto const [e, isect] = dcls.isectAndExact();
        auto const nonReg = dcls.containsNonRegular();
        auto const eu = e.unserialize(index);
        if (!eu) return TBottom;
        auto out = isObj ? objExact(*eu) : clsExact(*eu, nonReg);
        for (auto const i : *isect) {
          auto const u = i.unserialize(index);
          if (!u) return TBottom;
          out &= isObj ? subObj(*u) : subCls(*u, nonReg);
        }
        return out;
      } else {
        auto const u = dcls.cls().unserialize(index);
        if (!u) return TBottom;
        if (dcls.isExact()) {
          return isObj
            ? objExact(*u)
            : clsExact(*u, dcls.containsNonRegular());
        } else {
          assertx(dcls.isSub());
          return isObj
            ? subObj(*u)
            : subCls(*u, dcls.containsNonRegular());
        }
      }
    }();

    auto& p = parent();
    if (newT.is(BBottom)) {
      p = isObj ? remove_obj(std::move(p)) : remove_cls(std::move(p));
    } else {
      auto const bits = p.m_bits;
      auto const mark = p.m_legacyMark;
      p = setctx(std::move(newT), dcls.isCtx());
      p.m_bits = bits;
      p.m_legacyMark = mark;
    }
  };

  switch (t.m_dataTag) {
    case DataTag::None:
    case DataTag::Int:
    case DataTag::Dbl:
    case DataTag::Str:
    case DataTag::LazyCls:
    case DataTag::EnumClassLabel:
    case DataTag::ArrLikeVal:
      // These can never contain objects or classes.
      break;
    case DataTag::WaitHandle: {
      auto cls = unserialize_classes(
        index,
        demote_wait_handle(t)
      );
      if (cls.m_dataTag != DataTag::Obj) {
        parent() = std::move(cls);
        break;
      }
      WaitHandleCOWer next{parent, *t.m_data.dwh};
      unserialize_classes_impl(index, next.get().inner, next);
      next.getAndCOW().cls = std::move(cls.m_data.dobj);
      break;
    }
    case DataTag::ArrLikePacked: {
      ArrLikePackedCOWer next{parent, *t.m_data.packed};
      do {
        unserialize_classes_impl(index, next.currentElem(), next);
        if (next.currentElem().is(BBottom)) {
          auto& p = parent();
          p = remove_bits(std::move(p), BArrLikeN);
          break;
        }
      } while (next.nextElem());
      break;
    }
    case DataTag::ArrLikePackedN: {
      ArrLikePackedNCOWer next{parent, *t.m_data.packedn};
      unserialize_classes_impl(index, next.get().type, next);
      if (next.get().type.is(BBottom)) {
        auto& p = parent();
        p = remove_bits(std::move(p), BArrLikeN);
      }
      break;
    }
    case DataTag::ArrLikeMap: {
      ArrLikeMapCOWer next{parent, *t.m_data.map};
      auto isBottom = false;
      do {
        unserialize_classes_impl(index, next.currentElem(), next);
        if (next.currentElem().is(BBottom)) {
          auto& p = parent();
          p = remove_bits(std::move(p), BArrLikeN);
          isBottom = true;
          break;
        }
      } while(next.nextElem());
      if (!isBottom) {
        assertx(next.optKeyOrValue().is(BBottom) ||
                next.optKeyOrValue().subtypeOf(BArrKey));
        assertx(!next.optKeyOrValue().couldBe(BCls | BObj));
        next.toOptVal();
        unserialize_classes_impl(index, next.optKeyOrValue(), next);
      }
      break;
    }
    case DataTag::ArrLikeMapN: {
      ArrLikeMapNCOWer next{parent, *t.m_data.mapn};
      assertx(next.keyOrValue().subtypeOf(BArrKey));
      assertx(!next.keyOrValue().couldBe(BCls | BObj));
      next.toValue();
      unserialize_classes_impl(index, next.keyOrValue(), next);
      if (next.keyOrValue().is(BBottom)) {
        auto& p = parent();
        p = remove_bits(std::move(p), BArrLikeN);
      }
      break;
    }
    case DataTag::Obj:
      onDCls(t.m_data.dobj, true);
      break;
    case DataTag::Cls:
      onDCls(t.m_data.dcls, false);
      break;
  }
}

Type unserialize_classes(const IIndex& index, Type t) {
  TypeCOWer cower{t};
  unserialize_classes_impl(index, t, cower);
  return t;
}

//////////////////////////////////////////////////////////////////////

void serialize_classes_impl(const Type& t, COWer& parent) {
  SCOPE_EXIT { parent.noCOW().checkInvariants(); };

  auto const onDCls = [&] (const DCls& dcls, bool isObj) {
    auto newDCls = [&] {
      if (dcls.isIsect()) {
        auto const& isect = dcls.isect();

        DCls::IsectSet out;
        out.reserve(isect.size());
        for (auto i : isect) out.emplace_back(i.serialize());

        return DCls::MakeIsect(std::move(out), dcls.containsNonRegular());
      } else if (dcls.isIsectAndExact()) {
        auto const [e, isect] = dcls.isectAndExact();

        DCls::IsectSet out;
        out.reserve(isect->size());
        for (auto i : *isect) out.emplace_back(i.serialize());

        return DCls::MakeIsectAndExact(
          e.serialize(),
          std::move(out),
          dcls.containsNonRegular()
        );
      } else if (dcls.isExact()) {
        return DCls::MakeExact(
          dcls.cls().serialize(),
          dcls.containsNonRegular()
        );
      } else {
        assertx(dcls.isSub());
        return DCls::MakeSub(
          dcls.cls().serialize(),
          dcls.containsNonRegular()
        );
      }
    }();
    newDCls.setCtx(dcls.isCtx());

    auto& p = parent();
    if (isObj) {
      assertx(p.m_dataTag == DataTag::Obj);
      p.m_data.dobj = std::move(newDCls);
    } else {
      assertx(p.m_dataTag == DataTag::Cls);
      p.m_data.dcls = std::move(newDCls);
    }
  };

  switch (t.m_dataTag) {
    case DataTag::None:
    case DataTag::Int:
    case DataTag::Dbl:
    case DataTag::Str:
    case DataTag::LazyCls:
    case DataTag::EnumClassLabel:
    case DataTag::ArrLikeVal:
      // These can never contain objects or classes.
      break;
    case DataTag::WaitHandle: {
      auto cls = serialize_classes(demote_wait_handle(t));
      assertx(cls.m_dataTag == DataTag::Obj);
      WaitHandleCOWer next{parent, *t.m_data.dwh};
      serialize_classes_impl(next.get().inner, next);
      next.getAndCOW().cls = std::move(cls.m_data.dobj);
      break;
    }
    case DataTag::ArrLikePacked: {
      ArrLikePackedCOWer next{parent, *t.m_data.packed};
      do {
        serialize_classes_impl(next.currentElem(), next);
      } while (next.nextElem());
      break;
    }
    case DataTag::ArrLikePackedN: {
      ArrLikePackedNCOWer next{parent, *t.m_data.packedn};
      serialize_classes_impl(next.get().type, next);
      break;
    }
    case DataTag::ArrLikeMap: {
      ArrLikeMapCOWer next{parent, *t.m_data.map};
      do {
        serialize_classes_impl(next.currentElem(), next);
      } while (next.nextElem());
      assertx(next.optKeyOrValue().is(BBottom) ||
              next.optKeyOrValue().subtypeOf(BArrKey));
      assertx(!next.optKeyOrValue().couldBe(BCls | BObj));
      next.toOptVal();
      serialize_classes_impl(next.optKeyOrValue(), next);
      break;
    }
    case DataTag::ArrLikeMapN: {
      ArrLikeMapNCOWer next{parent, *t.m_data.mapn};
      assertx(next.keyOrValue().subtypeOf(BArrKey));
      assertx(!next.keyOrValue().couldBe(BCls | BObj));
      next.toValue();
      serialize_classes_impl(next.keyOrValue(), next);
      break;
    }
    case DataTag::Obj:
      onDCls(t.m_data.dobj, true);
      break;
    case DataTag::Cls:
      onDCls(t.m_data.dcls, false);
      break;
  }
}

Type serialize_classes(Type t) {
  TypeCOWer cower{t};
  serialize_classes_impl(t, cower);
  return t;
}

//////////////////////////////////////////////////////////////////////

IterTypes iter_types(const Type& iterable) {
  // Only array types and objects can be iterated. Everything else raises a
  // warning and jumps out of the loop.
  if (!iterable.couldBe(BArrLike | BObj | BClsMeth)) {
    return { TBottom, TBottom, IterTypes::Count::Empty, true, true };
  }

  // Optional types are okay here because a null will not set any locals (but it
  // might throw).
  if (!iterable.subtypeOf(BOptArrLike)) {
    return {
      TInitCell,
      TInitCell,
      IterTypes::Count::Any,
      true,
      iterable.couldBe(BObj)
    };
  }

  auto const mayThrow = iterable.couldBe(BInitNull);

  if (iterable.subtypeOf(BOptArrLikeE)) {
    return { TBottom, TBottom, IterTypes::Count::Empty, mayThrow, false };
  }

  // If we get a null, it will be as if we have any empty array, so consider
  // that possibly "empty".
  auto const maybeEmpty = mayThrow || !iterable.subtypeOf(BOptArrLikeN);

  auto const count = [&] (Optional<size_t> size) {
    if (size) {
      assertx(*size > 0);
      if (*size == 1) {
        return maybeEmpty
          ? IterTypes::Count::ZeroOrOne
          : IterTypes::Count::Single;
      }
    }
    return maybeEmpty ? IterTypes::Count::Any : IterTypes::Count::NonEmpty;
  };

  if (!is_specialized_array_like(iterable)) {
    return {
      Type{ allowedKeyBits(iterable.bits()).first },
      Type{ allowedValBits(iterable.bits(), false).first },
      count(std::nullopt),
      mayThrow,
      false
    };
  }

  switch (iterable.m_dataTag) {
  case DataTag::None:
  case DataTag::Str:
  case DataTag::LazyCls:
  case DataTag::EnumClassLabel:
  case DataTag::Obj:
  case DataTag::WaitHandle:
  case DataTag::Int:
  case DataTag::Dbl:
  case DataTag::Cls:
    always_assert(false);
  case DataTag::ArrLikeVal: {
    auto [key, val, _] = val_key_values(iterable.m_data.aval);
    return {
      std::move(key),
      std::move(val),
      count(iterable.m_data.aval->size()),
      mayThrow,
      false
    };
  }
  case DataTag::ArrLikePacked:
    return {
      packed_key(*iterable.m_data.packed),
      packed_values(*iterable.m_data.packed),
      count(iterable.m_data.packed->elems.size()),
      mayThrow,
      false
    };
  case DataTag::ArrLikePackedN:
    return {
      TInt,
      iterable.m_data.packedn->type,
      count(std::nullopt),
      mayThrow,
      false
    };
  case DataTag::ArrLikeMap: {
    auto kv = map_key_values(*iterable.m_data.map);
    return {
      std::move(kv.first),
      std::move(kv.second),
      iterable.m_data.map->hasOptElements()
        ? count(std::nullopt)
        : count(iterable.m_data.map->map.size()),
      mayThrow,
      false
    };
  }
  case DataTag::ArrLikeMapN:
    return {
      iterable.m_data.mapn->key,
      iterable.m_data.mapn->val,
      count(std::nullopt),
      mayThrow,
      false
    };
  }

  not_reached();
}

bool could_contain_objects(const Type& t) {
  if (t.couldBe(BObj)) return true;
  if (!t.couldBe(BCArrLikeN)) return false;

  switch (t.m_dataTag) {
  case DataTag::None:
  case DataTag::Str:
  case DataTag::LazyCls:
  case DataTag::EnumClassLabel:
  case DataTag::Obj:
  case DataTag::WaitHandle:
  case DataTag::Int:
  case DataTag::Dbl:
  case DataTag::Cls:
    return true;
  case DataTag::ArrLikeVal: return false;
  case DataTag::ArrLikePacked:
    for (auto const& e : t.m_data.packed->elems) {
      if (could_contain_objects(e)) return true;
    }
    return false;
  case DataTag::ArrLikePackedN:
    return could_contain_objects(t.m_data.packedn->type);
  case DataTag::ArrLikeMap:
    for (auto const& kv : t.m_data.map->map) {
      if (could_contain_objects(kv.second.val)) return true;
    }
    if (could_contain_objects(t.m_data.map->optVal)) return true;
    return false;
  case DataTag::ArrLikeMapN:
    return could_contain_objects(t.m_data.mapn->val);
  }

  not_reached();
}

bool is_type_might_raise(const Type& testTy, const Type& valTy) {
  auto const mayLogClsMeth =
    Cfg::Eval::IsVecNotices &&
    valTy.couldBe(BClsMeth) &&
    (testTy.is(BVec | BClsMeth) ||
     testTy.is(BArrLike | BClsMeth));

  assertx(!testTy.is(BVec | BClsMeth));
  assertx(!mayLogClsMeth);

  if (testTy.couldBe(BInitNull) && !testTy.subtypeOf(BInitNull)) {
    return is_type_might_raise(unopt(testTy), valTy);
  }

  if (testTy.is(BStr | BCls | BLazyCls)) {
    return Cfg::Eval::ClassIsStringNotices && valTy.couldBe(BCls | BLazyCls);
  } else if (testTy.is(BVec) || testTy.is(BVec | BClsMeth)) {
    return mayLogClsMeth;
  } else if (testTy.is(BDict)) {
    return false;
  }
  return false;
}

bool is_type_might_raise(IsTypeOp testOp, const Type& valTy) {
  switch (testOp) {
    case IsTypeOp::Scalar:
    case IsTypeOp::LegacyArrLike:
      return false;
    default:
      return is_type_might_raise(type_of_istype(testOp), valTy);
  }
}

bool inner_types_might_raise(const Type& t1, const Type& t2) {
  assertx(t1.subtypeOf(BArrLike));
  assertx(t2.subtypeOf(BArrLike));

  // If either is an empty array, there are no inner elements to warn about.
  if (!t1.couldBe(BArrLikeN) || !t2.couldBe(BArrLikeN)) return false;

  auto const checkOne = [&] (const Type& t, Optional<size_t>& sz) {
    switch (t.m_dataTag) {
      case DataTag::None:
        return true;

      case DataTag::Str:
      case DataTag::LazyCls:
      case DataTag::EnumClassLabel:
      case DataTag::Obj:
      case DataTag::WaitHandle:
      case DataTag::Int:
      case DataTag::Dbl:
      case DataTag::Cls:
        not_reached();

      case DataTag::ArrLikeVal:
        sz = t.m_data.aval->size();
        return true;
      case DataTag::ArrLikePacked:
        sz = t.m_data.packed->elems.size();
        return true;
      case DataTag::ArrLikePackedN:
        return t.m_data.packedn->type.couldBe(BArrLike | BObj | BClsMeth);
      case DataTag::ArrLikeMap:
        if (!t.m_data.map->hasOptElements()) sz = t.m_data.map->map.size();
        return true;
      case DataTag::ArrLikeMapN:
        return t.m_data.mapn->val.couldBe(BArrLike | BObj | BClsMeth);
    }
    not_reached();
  };

  Optional<size_t> sz1;
  if (!checkOne(t1, sz1)) return false;
  Optional<size_t> sz2;
  if (!checkOne(t2, sz2)) return false;

  // if the arrays have different sizes, we don't even check their contents
  if (sz1 && sz2 && *sz1 != *sz2) return false;
  size_t numToCheck = 1;
  if (sz1 && *sz1 > numToCheck) numToCheck = *sz1;
  if (sz2 && *sz2 > numToCheck) numToCheck = *sz2;

  union ArrPos {
    ArrPos() : pos{} {}
    size_t pos;
    MapElems::iterator it;
  } p1, p2;

  Optional<Type> vals1;
  Optional<Type> vals2;

  for (size_t i = 0; i < numToCheck; i++) {
    auto const nextType = [&] (const Type& t,
                               ArrPos& p,
                               Optional<Type>& vals) {
      switch (t.m_dataTag) {
        case DataTag::None:
          return TInitCell;

        case DataTag::Str:
        case DataTag::LazyCls:
        case DataTag::EnumClassLabel:
        case DataTag::Obj:
        case DataTag::WaitHandle:
        case DataTag::Int:
        case DataTag::Dbl:
        case DataTag::Cls:
          not_reached();

        case DataTag::ArrLikeVal:
          if (!i) {
            p.pos = t.m_data.aval->iter_begin();
          } else {
            p.pos = t.m_data.aval->iter_advance(p.pos);
          }
          return from_cell(t.m_data.aval->nvGetVal(p.pos));
        case DataTag::ArrLikePacked:
          return t.m_data.packed->elems[i];
        case DataTag::ArrLikePackedN:
          return t.m_data.packedn->type;
        case DataTag::ArrLikeMap:
          if (t.m_data.map->hasOptElements()) {
            if (!vals) vals = map_key_values(*t.m_data.map).second;
            return *vals;
          }

          if (!i) {
            p.it = t.m_data.map->map.begin();
          } else {
           ++p.it;
          }
          return p.it->second.val;
        case DataTag::ArrLikeMapN:
          return t.m_data.mapn->val;
      }
      not_reached();
    };
    if (compare_might_raise(nextType(t1, p1, vals1), nextType(t2, p2, vals2))) {
      return true;
    }
  }
  return false;
}

bool compare_might_raise(const Type& t1, const Type& t2) {
  if (!Cfg::Eval::EmitClsMethPointers &&
      Cfg::Eval::RaiseClassConversionNoticeSampleRate == 0) {
    return false;
  }

  auto const checkOne = [&] (const trep bits) -> Optional<bool> {
    if (t1.subtypeOf(bits) && t2.subtypeOf(bits)) {
      return inner_types_might_raise(t1, t2);
    }
    if (t1.couldBe(bits) && t2.couldBe(BArrLike)) return true;
    if (t2.couldBe(bits) && t1.couldBe(BArrLike)) return true;
    return std::nullopt;
  };

  if (auto const f = checkOne(BDict)) return *f;
  if (auto const f = checkOne(BVec)) return *f;
  if (auto const f = checkOne(BKeyset)) return *f;

  if (Cfg::Eval::EmitClsMethPointers) {
    if (t1.couldBe(BClsMeth) && t2.couldBe(BVec))     return true;
    if (t1.couldBe(BVec)     && t2.couldBe(BClsMeth)) return true;
  }

  if (Cfg::Eval::RaiseClassConversionNoticeSampleRate > 0) {
    if (t1.couldBe(BStr) && t2.couldBe(BLazyCls)) return true;
    if (t1.couldBe(BStr) && t2.couldBe(BCls)) return true;
    if (t1.couldBe(BLazyCls) && t2.couldBe(BStr)) return true;
    if (t1.couldBe(BCls) && t2.couldBe(BStr)) return true;
  }

  return t1.couldBe(BObj) && t2.couldBe(BObj);
}

//////////////////////////////////////////////////////////////////////

std::pair<Type, bool> array_like_elem(const Type& arr, const Type& key) {
  assertx(arr.couldBe(BArrLike));
  assertx(key.subtypeOf(BArrKey));
  assertx(!key.is(BBottom));

  // Fast path: if the array is one specific type, we can just do the
  // lookup without intersections.
  if (arr.subtypeAmong(BVec, BArrLike) ||
      arr.subtypeAmong(BDict, BArrLike) ||
      arr.subtypeAmong(BKeyset, BArrLike)) {
    return array_like_elem_impl(arr, key);
  }

  // Otherwise split up the array into its constituent array types, do
  // the lookup for each one, then union together the results.
  auto elem = TBottom;
  auto present = true;
  auto const combine = [&] (std::pair<Type, bool> r) {
    elem |= std::move(r.first);
    present &= r.second;
  };
  if (arr.couldBe(BVec)) {
    combine(array_like_elem_impl(intersection_of(arr, TVec), key));
  }
  if (arr.couldBe(BDict)) {
    combine(array_like_elem_impl(intersection_of(arr, TDict), key));
  }
  if (arr.couldBe(BKeyset)) {
    combine(array_like_elem_impl(intersection_of(arr, TKeyset), key));
  }
  return std::make_pair(elem, present);
}

std::pair<Type, bool> arr_val_elem(const Type& arr, const Type& key) {
  assertx(arr.m_dataTag == DataTag::ArrLikeVal);
  assertx(key.subtypeOf(BArrKey));

  auto const ad = arr.m_data.aval;
  assertx(!ad->empty());

  if (auto const k = tvCounted(key)) {
    auto const r = ad->get(*k);
    if (r.is_init()) return { from_cell(r), true };
    return { TBottom, false };
  }

  auto const loosened = loosen_string_staticness(key);

  auto ty = TBottom;
  IterateKV(
    ad,
    [&] (TypedValue k, TypedValue v) {
      if (from_cell(k).couldBe(loosened)) ty |= from_cell(v);
      return !ty.strictSubtypeOf(BInitUnc);
    }
  );
  assertx(ty.subtypeOf(BInitUnc));
  return { ty, false };
}

std::pair<Type, bool> arr_packed_elem(const Type& arr, const Type& key) {
  assertx(arr.m_dataTag == DataTag::ArrLikePacked);
  assertx(key.subtypeOf(BArrKey));

  if (!key.couldBe(BInt)) return { TBottom, false };

  auto const& pack = *arr.m_data.packed;
  if (is_specialized_int(key)) {
    auto const idx = ival_of(key);
    if (idx >= 0 && idx < pack.elems.size()) {
      return { pack.elems[idx], key.subtypeOf(BInt) };
    }
    return { TBottom, false };
  }

  return { packed_values(pack), false };
}

std::pair<Type, bool> arr_packedn_elem(const Type& arr, const Type& key) {
  assertx(arr.m_dataTag == DataTag::ArrLikePackedN);
  assertx(key.subtypeOf(BArrKey));

  if (!key.couldBe(BInt)) return { TBottom, false };
  if (is_specialized_int(key) && ival_of(key) < 0) {
    return { TBottom, false };
  }
  return { arr.m_data.packedn->type, false };
}

std::pair<Type, bool> arr_map_elem(const Type& arr, const Type& key) {
  assertx(arr.m_dataTag == DataTag::ArrLikeMap);
  assertx(key.subtypeOf(BArrKey));
  assertx(!key.is(BBottom));

  auto const& map = *arr.m_data.map;

  // If the key has a known value, it either matches a known key in
  // the map, or it doesn't, but could match the optional key. The
  // known keys and the optional key are disjoint, so we can skip it
  // for an exact match.
  if (auto const k = tvCounted(key)) {
    auto const it = map.map.find(*k);
    if (it != map.map.end()) return { it->second.val, true };
    if (loosen_string_staticness(map.optKey).couldBe(key)) {
      return { map.optVal, false };
    }
    return { TBottom, false };
  }

  auto ty = TBottom;
  for (auto const& kv : map.map) {
    if (map_key_nonstatic(kv.first).couldBe(key)) ty |= kv.second.val;
    if (!ty.strictSubtypeOf(BInitCell)) break;
  }
  if (loosen_string_staticness(map.optKey).couldBe(key)) {
    ty |= map.optVal;
  }
  return { ty, false };
}

std::pair<Type, bool> arr_mapn_elem(const Type& arr, const Type& key) {
  assertx(arr.m_dataTag == DataTag::ArrLikeMapN);
  assertx(key.subtypeOf(BArrKey));
  if (!loosen_string_staticness(key).couldBe(arr.m_data.mapn->key)) {
    return { TBottom, false };
  }
  return { arr.m_data.mapn->val, false };
}

std::pair<Type, bool> array_like_elem_impl(const Type& arr, const Type& key) {
  assertx(!arr.is(BBottom));
  assertx(arr.subtypeAmong(BVec, BArrLike) ||
          arr.subtypeAmong(BDict, BArrLike) ||
          arr.subtypeAmong(BKeyset, BArrLike));
  assertx(key.subtypeOf(BArrKey));
  assertx(!key.is(BBottom));

  // If it's always empty, there's nothing to lookup
  if (!arr.couldBe(BArrLikeN)) return { TBottom, false };

  auto r = [&] () -> std::pair<Type, bool> {
    switch (arr.m_dataTag) {
      case DataTag::Str:
      case DataTag::LazyCls:
      case DataTag::EnumClassLabel:
      case DataTag::Obj:
      case DataTag::WaitHandle:
      case DataTag::Int:
      case DataTag::Dbl:
      case DataTag::Cls:
      case DataTag::None: {
        // Even without a specialization, there's some special cases
        // we can rule out:
        if (arr.subtypeAmong(BVecN, BArrLikeN)) {
          if (!key.couldBe(BInt)) return { TBottom, false };
          if (is_specialized_int(key) && ival_of(key) < 0) {
            return { TBottom, false };
          }
        }
        // Otherwise we can use the staticness to determine a possible
        // value type.
        auto const isStatic = arr.subtypeAmong(BSArrLikeN, BArrLikeN);
        if (arr.subtypeAmong(BKeysetN, BArrLikeN)) {
          return { isStatic ? TUncArrKey : TArrKey, false };
        }
        return { isStatic ? TInitUnc : TInitCell, false };
      }
      case DataTag::ArrLikeVal:
        return arr_val_elem(arr, key);
      case DataTag::ArrLikePacked:
        return arr_packed_elem(arr, key);
      case DataTag::ArrLikePackedN:
        return arr_packedn_elem(arr, key);
      case DataTag::ArrLikeMap:
        return arr_map_elem(arr, key);
      case DataTag::ArrLikeMapN:
        return arr_mapn_elem(arr, key);
    }
    not_reached();
  }();
  // If the array could be empty, we're never sure we'll find the
  // value.
  if (arr.couldBe(BArrLikeE)) r.second = false;
  if (arr.subtypeAmong(BKeysetN, BArrLikeN)) {
    // Enforce the invariant that the value returned for a Keyset
    // always matches the key.
    r.first = intersection_of(
      std::move(r.first),
      loosen_string_staticness(key)
    );
  }
  return r;
}

//////////////////////////////////////////////////////////////////////

Type array_like_set_values(Type base, Type key, Type value) {
  assertx(base.subtypeOf(BVec) || base.subtypeOf(BDict));
  assertx(base.couldBe(BVecN | BDictN));
  assertx(key.subtypeOf(BArrKey));
  assertx(IMPLIES(base.subtypeOf(BDict), !key.is(BBottom)));
  assertx(value.subtypeOf(BInitCell));
  assertx(!value.is(BBottom));

  auto constraint = base.subtypeOf(BVec)
    ? packedn_impl(
        base.bits() & BVecN,
        base.m_legacyMark,
        std::move(value)
      )
    : mapn_impl(
        base.bits() & BDictN,
        base.m_legacyMark,
        std::move(key),
        std::move(value)
      );
  auto refined = intersection_of(std::move(base), std::move(constraint));
  assertx(!refined.is(BBottom));
  return refined;
}

//////////////////////////////////////////////////////////////////////

std::pair<Type, bool> array_like_set(Type base,
                                     const Type& key,
                                     const Type& val) {
  assertx(base.subtypeOf(BCell));
  assertx(base.couldBe(BArrLike));
  assertx(key.subtypeOf(BArrKey));
  assertx(!key.is(BBottom));

  // NB: Be careful here about unnecessarily copying base/arr
  // here. The impl function will mutate the specialization and we
  // want to modify it in place as much as possible. So we go out of
  // our way to move in as many cases as we can.

  auto [arr, rest] = split_array_like(std::move(base));
  if (arr.subtypeOf(BVec) || arr.subtypeOf(BDict)) {
    // Fast path, we do the set directly without splitting the array
    // into its specific types.
    auto r = array_like_set_impl(std::move(arr), key, val);
    return std::make_pair(
      union_of(std::move(r.first), std::move(rest)),
      r.second
    );
  }

  // Sets aren't allowed on a keyset.
  if (arr.subtypeOf(BKeyset)) return std::make_pair(std::move(rest), true);

  // Otherwise split the array into its specific array types, do the
  // set on each one, then union the results back together.
  auto result = TBottom;
  auto mightThrow = false;
  auto const combine = [&] (std::pair<Type, bool> r) {
    result |= std::move(r.first);
    mightThrow |= r.second;
  };

  if (arr.couldBe(BVec)) {
    // If this is the last usage of arr, move it instead of copying.
    Type i;
    if (arr.couldBe(BDict)) {
      i = arr;
    } else {
      i = std::move(arr);
    }
    combine(
      array_like_set_impl(intersection_of(std::move(i), TVec), key, val)
    );
  }
  if (arr.couldBe(BDict)) {
    combine(
      array_like_set_impl(intersection_of(std::move(arr), TDict), key, val)
    );
  }
  if (arr.couldBe(BKeyset)) mightThrow = true;

  return std::make_pair(
    union_of(std::move(result), std::move(rest)),
    mightThrow
  );
}

bool arr_packed_set(Type& pack,
                    const Type& key,
                    const Type& val,
                    bool maybeEmpty) {
  assertx(pack.m_dataTag == DataTag::ArrLikePacked);
  assertx(key.subtypeOf(BArrKey));

  auto& packed = pack.m_data.packed;

  if (pack.subtypeOf(BVec)) {
    if (is_specialized_int(key)) {
      auto const idx = ival_of(key);
      if (idx < 0 || idx >= packed->elems.size()) {
        pack = TBottom;
        return true;
      }
      packed.mutate()->elems[idx] = val;
      return maybeEmpty || !key.subtypeOf(BInt);
    }

    for (auto& e : packed.mutate()->elems) e |= val;
    return true;
  }

  if (auto const k = tvCounted(key)) {
    if (isIntType(k->m_type)) {
      if (k->m_data.num >= 0) {
        if (maybeEmpty) {
          if (k->m_data.num == 0) {
            if (packed->elems.size() == 1) {
              packed.mutate()->elems[0] = val;
              return false;
            }
            pack = packedn_impl(
              pack.bits(),
              pack.m_legacyMark,
              union_of(packed_values(*packed), val)
            );
            return false;
          }
        } else {
          if (k->m_data.num < packed->elems.size()) {
            packed.mutate()->elems[k->m_data.num] = val;
            return false;
          }
          if (k->m_data.num == packed->elems.size()) {
            packed.mutate()->elems.push_back(val);
            return false;
          }
        }
      }
    }

    if (!maybeEmpty) {
      MapElems elems;
      for (size_t i = 0; i < packed->elems.size(); ++i) {
        elems.emplace_back(
          make_tv<KindOfInt64>(i),
          MapElem::IntKey(packed->elems[i])
        );
      }
      elems.emplace_back(*k, MapElem::KeyFromType(key, val));
      pack =
        map_impl(pack.bits(), pack.m_legacyMark, std::move(elems), TBottom, TBottom);
      return false;
    }
  }

  pack = mapn_impl(
    pack.bits(),
    pack.m_legacyMark,
    union_of(packed_key(*packed), key),
    union_of(packed_values(*packed), val)
  );
  return false;
}

bool arr_packedn_set(Type& pack,
                     const Type& key,
                     const Type& val,
                     bool maybeEmpty) {
  assertx(pack.m_dataTag == DataTag::ArrLikePackedN);
  assertx(key.subtypeOf(BArrKey));

  auto const keepPacked = [&] (bool mightThrow) {
    auto t = union_of(pack.m_data.packedn->type, val);
    if (pack.subtypeOf(BVec) && !t.strictSubtypeOf(BInitCell)) {
      pack = Type { pack.bits(), pack.m_legacyMark };
    } else {
      pack.m_data.packedn.mutate()->type = std::move(t);
    }
    return mightThrow;
  };

  if (pack.subtypeOf(BVec)) {
    if (is_specialized_int(key)) {
      auto const idx = ival_of(key);
      if (idx < 0) {
        pack = TBottom;
        return true;
      }
      if (idx == 0 && !maybeEmpty && key.subtypeOf(BInt)) {
        return keepPacked(false);
      }
    }
    return keepPacked(true);
  }

  if (auto const k = tvCounted(key)) {
    if (isIntType(k->m_type)) {
      if (k->m_data.num == 0) return keepPacked(false);
      if (k->m_data.num == 1 && !maybeEmpty) return keepPacked(false);
    }
  }

  pack = mapn_impl(
    pack.bits(),
    pack.m_legacyMark,
    union_of(TInt, key),
    union_of(pack.m_data.packedn->type, val)
  );
  return false;
}

bool arr_map_set(Type& map,
                 const Type& key,
                 const Type& val,
                 bool maybeEmpty) {
  assertx(map.m_dataTag == DataTag::ArrLikeMap);
  assertx(key.subtypeOf(BArrKey));
  assertx(!map.subtypeOf(BVec));

  auto mutated = map.m_data.map.mutate();

  // NB: At runtime, keysets do *not* get updated with the new value
  // if there's a match. This is irrelevant for tracking the key
  // values, but matters with regards to staticness.

  if (maybeEmpty) {
    // If the array could be empty, we have to effectively deal with
    // the union of a single element array (the result of the set on
    // the empty array), and the updated non-empty array. In most
    // cases this cannot be represented with a Map representation and
    // we need to fall back to MapN.
    if (auto const k = tvCounted(key)) {
      // Special case: if the key is known and matches the first key
      // of the map, we can keep the map representation. The first
      // element is known and the rest of the values become optional.
      assertx(!mutated->map.empty());
      if (tvSame(mutated->map.begin()->first, *k)) {
        auto it = mutated->map.begin();
        auto mapKey = map_key(it->first, it->second);
        DEBUG_ONLY auto const& mapVal = it->second.val;
        ++it;
        auto restKey = mutated->optKey;
        auto restVal = mutated->optVal;
        while (it != mutated->map.end()) {
          restKey |= map_key(it->first, it->second);
          restVal |= it->second.val;
          ++it;
        }
        MapElems elems;
        if (!map.subtypeOf(BKeyset)) {
          elems.emplace_back(
            *k,
            MapElem::KeyFromType(union_of(std::move(mapKey), key), val)
          );
        } else {
          // For Keysets we also have to deal with possibly no longer
          // knowing the key's staticness.
          assertx(equal(key, val));
          assertx(equal(mapKey, mapVal));
          auto const u = union_of(std::move(mapKey), key);
          elems.emplace_back(
            *k,
            MapElem::KeyFromType(u, u)
          );
        }

        map = map_impl(
          map.bits(),
          map.m_legacyMark,
          std::move(elems),
          std::move(restKey),
          std::move(restVal)
        );
        return false;
      }
    }

    // Otherwise we cannot represent the result precisely and must use
    // MapN.
    auto mkv = map_key_values(*mutated);
    map = mapn_impl(
      map.bits(),
      map.m_legacyMark,
      union_of(key, std::move(mkv.first)),
      union_of(val, std::move(mkv.second))
    );
    return false;
  }

  // If the array cannot be empty, we can always keep the Map
  // representation.

  if (auto const k = tvCounted(key)) {
    // The new key is known
    if (auto const it = mutated->map.find(*k); it != mutated->map.end()) {
      // It matches an existing key. Set its associated type to be the
      // new value type.
      if (!map.subtypeOf(BKeyset)) {
        mutated->map.update(it, it->second.withType(val));
      }
      return false;
    }

    if (mutated->hasOptElements()) {
      // The Map has optional elements. If the optional element
      // represents a single key, and that key is the same as the new
      // key, we can turn the optional element into a known key. An
      // optional element representing a single key means "this array
      // might end with this key, or might not". Since we're setting
      // this key, the new array definitely ends with that key.
      if (auto const optK = tvCounted(mutated->optKey)) {
        if (tvSame(*optK, *k)) {
          if (!map.subtypeOf(BKeyset)) {
            mutated->map.emplace_back(
              *k,
              MapElem::KeyFromType(union_of(key, mutated->optKey), val)
            );
          } else {
            // For Keysets we also have to deal with possibly no
            // longer knowing the key's staticness.
            assertx(equal(key, val));
            assertx(equal(mutated->optKey, mutated->optVal));
            auto const u = union_of(key, mutated->optKey);
            mutated->map.emplace_back(
              *k,
              MapElem::KeyFromType(u, u)
            );
          }
          mutated->optKey = TBottom;
          mutated->optVal = TBottom;
          return false;
        }
      }
      // Otherwise just add the new key and value to the optional
      // elements. We've lost the complete key structure of the array.
      mutated->optKey |= key;
      mutated->optVal |= val;
    } else {
      // There's no optional elements and we know this key doesn't
      // exist in the Map. Add it as the next known key.
      mutated->map.emplace_back(*k, MapElem::KeyFromType(key, val));
    }
    return false;
  }

  // The new key isn't known. The key either matches an existing key,
  // or its a new one. For an existing key which can possibly match,
  // union in the new value type. Then add the new key and value to
  // the optional elements since it might not match. We don't have to
  // modify the known keys for Keysets because at runtime updates
  // never modify existing values.
  if (!map.subtypeOf(BKeyset)) {
    for (auto it = mutated->map.begin(); it != mutated->map.end(); ++it) {
      if (key.couldBe(map_key_nonstatic(it->first))) {
        mutated->map.update(
          it,
          it->second.withType(union_of(it->second.val, val))
        );
      }
    }
  }
  mutated->optKey |= key;
  mutated->optVal |= val;
  return false;
}

std::pair<Type,bool> array_like_set_impl(Type arr,
                                         const Type& key,
                                         const Type& val) {
  // Note: array_like_set forbids Keysets, but this can be called by
  // array_like_newelem_impl, which does.
  assertx(arr.subtypeOf(BVec) ||
          arr.subtypeOf(BDict) ||
          arr.subtypeOf(BKeyset));
  assertx(key.subtypeOf(BArrKey));
  assertx(!arr.is(BBottom));
  assertx(!val.is(BBottom));
  assertx(!key.is(BBottom));
  assertx(IMPLIES(arr.subtypeOf(BKeyset), equal(key, val)));

  // Remove emptiness and loosen staticness from the bits
  auto const bits = [&] {
    auto b = BBottom;
    if (arr.couldBe(BVec))    b |= BVecN;
    if (arr.couldBe(BDict))   b |= BDictN;
    if (arr.couldBe(BKeyset)) b |= BKeysetN;
    return b;
  }();

  // Before anything, check for specific cases of bad keys:
  if (arr.subtypeOf(BVec)) {
    if (!key.couldBe(BInt) || (is_specialized_int(key) && ival_of(key) < 0)) {
      return { TBottom, true };
    }
  }

  if (!arr.couldBe(BArrLikeN)) {
    // Can't set into an empty Vec (only newelem)
    if (arr.subtypeOf(BVec)) return { TBottom, true };
    // mapn_impl will use the appropriate map or packed representation
    return { mapn_impl(bits, arr.m_legacyMark, key, val), false };
  }

  switch (arr.m_dataTag) {
    case DataTag::Str:
    case DataTag::LazyCls:
    case DataTag::EnumClassLabel:
    case DataTag::Obj:
    case DataTag::WaitHandle:
    case DataTag::Int:
    case DataTag::Dbl:
    case DataTag::Cls:
      break;
    case DataTag::None: {
      arr.m_bits = bits;
      assertx(arr.checkInvariants());
      auto const isVec = arr.subtypeOf(BVec);
      return { std::move(arr), isVec };
    }
    case DataTag::ArrLikeVal:
      return array_like_set_impl(
        toDArrLike(arr.m_data.aval, arr.bits(), arr.m_legacyMark),
        key, val
      );
    case DataTag::ArrLikePacked: {
      auto const maybeEmpty = arr.couldBe(BArrLikeE);
      arr.m_bits = bits;
      auto const mightThrow = arr_packed_set(arr, key, val, maybeEmpty);
      assertx(arr.checkInvariants());
      return { std::move(arr), mightThrow };
    }
    case DataTag::ArrLikePackedN: {
      auto const maybeEmpty = arr.couldBe(BArrLikeE);
      arr.m_bits = bits;
      auto const mightThrow = arr_packedn_set(arr, key, val, maybeEmpty);
      assertx(arr.checkInvariants());
      return { std::move(arr), mightThrow };
    }
    case DataTag::ArrLikeMap: {
      auto const maybeEmpty = arr.couldBe(BArrLikeE);
      arr.m_bits = bits;
      auto const mightThrow = arr_map_set(arr, key, val, maybeEmpty);
      assertx(arr.checkInvariants());
      return { std::move(arr), mightThrow };
    }
    case DataTag::ArrLikeMapN: {
      assertx(!arr.subtypeOf(BVec));
      arr.m_bits = bits;
      auto m = arr.m_data.mapn.mutate();
      auto newKey = union_of(std::move(m->key), key);
      auto newVal = union_of(std::move(m->val), val);
      if (!newKey.strictSubtypeOf(BArrKey) &&
          !newVal.strictSubtypeOf(
            arr.subtypeOf(BKeyset) ? BArrKey : BInitCell)
         ) {
        return { Type { bits, arr.m_legacyMark }, false };
      }
      m->key = std::move(newKey);
      m->val = std::move(newVal);
      assertx(arr.checkInvariants());
      return { std::move(arr), false };
    }
  }

  not_reached();
}

//////////////////////////////////////////////////////////////////////

std::pair<Type, bool> array_like_newelem(Type base, const Type& val) {
  assertx(base.subtypeOf(BCell));
  assertx(base.couldBe(BArrLike));
  assertx(val.subtypeOf(BInitCell));

  // NB: Be careful here about unnecessarily copying base/arr
  // here. The impl function will mutate the specialization and we
  // want to modify it in place as much as possible. So we go out of
  // our way to move in as many cases as we can.
  auto [arr, rest] = split_array_like(std::move(base));

  // Fast path: if the array is just one of the specific array types,
  // we can skip the intersection and do the newelem directly.
  if (arr.subtypeOf(BVec) ||
      arr.subtypeOf(BDict) ||
      arr.subtypeOf(BKeyset)) {
    auto r = array_like_newelem_impl(std::move(arr), val);
    return std::make_pair(
      union_of(std::move(r.first), std::move(rest)),
      r.second
    );
  }

  // Otherwise split the array into its specific types and perform the
  // newelem on each one, then union the results together.

  auto result = TBottom;
  auto mightThrow = false;
  auto const combine = [&] (std::pair<Type, bool> r) {
    result |= std::move(r.first);
    mightThrow |= r.second;
  };

  if (arr.couldBe(BVec)) {
    combine(array_like_newelem_impl(intersection_of(arr, TVec), val));
  }
  if (arr.couldBe(BDict)) {
    // Try to move arr instead of copying if this will be the last
    // use.
    Type i;
    if (arr.couldBe(BKeyset)) {
      i = arr;
    } else {
      i = std::move(arr);
    }
    combine(
      array_like_newelem_impl(intersection_of(std::move(i), TDict), val)
    );
  }
  if (arr.couldBe(BKeyset)) {
    combine(
      array_like_newelem_impl(intersection_of(std::move(arr), TKeyset), val)
    );
  }
  return std::make_pair(
    union_of(std::move(result), std::move(rest)),
    mightThrow
  );
}

bool arr_map_newelem(Type& map, const Type& val, bool update) {
  assertx(map.m_dataTag == DataTag::ArrLikeMap);

  // If the highest key is int64_t max, the chosen key will wrap
  // around, which will trigger a warning and not actually append
  // anything.
  auto const findLastK = [&] {
    int64_t lastK = -1;
    for (auto const& kv : map.m_data.map->map) {
      if (kv.first.m_type == KindOfInt64 &&
          kv.first.m_data.num > lastK) {
        lastK = kv.first.m_data.num;
      }
    }
    return lastK;
  };

  // If the Map has optional elements, we can't know what the new key
  // is (but it won't modify known keys).
  if (map.m_data.map->hasOptElements()) {
    // If the optional value is a single known key, we might still be
    // able to infer if the append might throw or not.
    auto mightThrow = true;
    if (auto const v = tvCounted(map.m_data.map->optKey)) {
      auto lastK = findLastK();
      if (v->m_type == KindOfInt64 && v->m_data.num > lastK) {
        lastK = v->m_data.num;
      }
      mightThrow = (lastK == std::numeric_limits<int64_t>::max());
    }

    if (update) {
      auto mutated = map.m_data.map.mutate();
      mutated->optKey |= TInt;
      mutated->optVal |= val;
    }
    return mightThrow;
  }

  auto const lastK = findLastK();
  if (lastK == std::numeric_limits<int64_t>::max()) return true;
  if (update) {
    map.m_data.map.mutate()->map.emplace_back(
      make_tv<KindOfInt64>(lastK + 1),
      MapElem::IntKey(val)
    );
  }
  // Otherwise we know the append will succeed without potentially
  // throwing.
  return false;
}

std::pair<Type, bool> array_like_newelem_impl(Type arr, const Type& val) {
  assertx(arr.subtypeOf(BVec) ||
          arr.subtypeOf(BDict) ||
          arr.subtypeOf(BKeyset));
  assertx(!arr.is(BBottom));
  assertx(!val.is(BBottom));

  // "Appends" on a keyset are actually modeled as a set with the same
  // key and value.
  if (arr.subtypeOf(BKeyset)) {
    // Since the val is going to be used like a key, we need to deal
    // with promotions (and possible invalid keys).
    auto [key, promotion] = promote_classlike_to_key(val);
    auto keyMaybeBad = false;
    if (!key.subtypeOf(BArrKey)) {
      key = intersection_of(std::move(key), TArrKey);
      keyMaybeBad = true;
    }
    if (key.is(BBottom)) return { TBottom, true };
    auto r = array_like_set_impl(std::move(arr), key, key);
    return {
      std::move(r.first),
      r.second || keyMaybeBad || promotion == Promotion::YesMightThrow
    };
  }

   /*
   * NB: Appends on dicts and darrays can potentially throw for two
   * reasons:
   *
   * - If m_nextKI is negative, which means that the "next key" for
   *   append would be a negative integer key. This raises a warning
   *   (and does not append anything).
   *
   * - If m_nextKI does not match the highest integer key in the array
   *   (and if Eval.DictDArrayAppendNotices is true, which we
   *   assume). This raises a notice.
   *
   * Since HHBBC does not attempt to track m_nextKI for arrays, we
   * have to be pessimistic and assume that any append on a darry or
   * dict can throw. However we can avoid it in certain situations:
   *
   * Since HHBBC pessimizes its knowledge of inner array structure
   * whenever it encounters an unset, if we have a specialized array
   * type, we know there's never been an unset on the array. Therefore
   * we can safely infer m_nextKI from what we know about the keys. If
   * the array is static, or if have an ArrLikeMap, we can iterate
   * over the keys. If we have a packed representation, we know the
   * keys are contiguous (and thus can only wrap if we have 2^63
   * values).
   *
   * This will have to be revisited if we ever decide to model unsets
   * (hopefully m_nextKI would be gone by then).
   */

  // Loosen staticness and remove emptiness from the bits
  auto const bits = [&] {
    auto b = BBottom;
    if (arr.couldBe(BVec))  b |= BVecN;
    if (arr.couldBe(BDict)) b |= BDictN;
    return b;
  }();

  if (!arr.couldBe(BArrLikeN)) {
    return {
      packed_impl(bits, arr.m_legacyMark, { val }),
      false // Appends cannot throw on an empty array
    };
  }

  switch (arr.m_dataTag) {
    case DataTag::Str:
    case DataTag::LazyCls:
    case DataTag::EnumClassLabel:
    case DataTag::Obj:
    case DataTag::WaitHandle:
    case DataTag::Int:
    case DataTag::Dbl:
    case DataTag::Cls:
      break;
    case DataTag::None: {
      arr.m_bits = bits;
      assertx(arr.checkInvariants());
      // Dict can throw on append, vec will not.
      auto const isDict = arr.subtypeOf(BDict);
      return { std::move(arr), isDict};
    }
    case DataTag::ArrLikeVal:
      return array_like_newelem_impl(
        toDArrLike(arr.m_data.aval, arr.bits(), arr.m_legacyMark),
        val
      );
    case DataTag::ArrLikePacked:
      // Packed arrays have contiguous keys, so the internal iterator
      // should match the size of the array. Therefore appends cannot
      // throw.
      if (arr.couldBe(BArrLikeE)) {
        return {
          packedn_impl(
            bits, arr.m_legacyMark,
            union_of(packed_values(*arr.m_data.packed), val)
          ),
          false
        };
      }
      arr.m_bits = bits;
      arr.m_data.packed.mutate()->elems.push_back(val);
      assertx(arr.checkInvariants());
      return { std::move(arr), false };
    case DataTag::ArrLikePackedN:
      // Ditto, with regards to packed array appends not throwing.
      if (arr.subtypeOf(BVec)) {
        auto t = union_of(arr.m_data.packedn.mutate()->type, val);
        if (!t.strictSubtypeOf(BInitCell)) {
          return { Type { bits, arr.m_legacyMark }, false };
        }
        arr.m_bits = bits;
        arr.m_data.packedn.mutate()->type = std::move(t);
      } else {
        arr.m_bits = bits;
        arr.m_data.packedn.mutate()->type |= val;
      }
      assertx(arr.checkInvariants());
      return { std::move(arr), false };
    case DataTag::ArrLikeMap: {
      assertx(!arr.subtypeOf(BVec));
      if (arr.couldBe(BArrLikeE)) {
        // Dict and darray can throw on append depending on the state of
        // the internal iterator. This isn't an issue for an empty
        // array, so we use the possibility inferred from the non-empty
        // case.
        auto const mightThrow = arr_map_newelem(arr, val, false);
        auto mkv = map_key_values(*arr.m_data.map);
        return {
          mapn_impl(
            bits,
            arr.m_legacyMark,
            union_of(std::move(mkv.first), TInt),
            union_of(std::move(mkv.second), val)
          ),
          mightThrow
        };
      }
      arr.m_bits = bits;
      auto const mightThrow = arr_map_newelem(arr, val, true);
      assertx(arr.checkInvariants());
      return { std::move(arr), mightThrow };
    }
    case DataTag::ArrLikeMapN: {
      // We don't know the specific keys of the map, so its possible
      // the append could throw.
      assertx(!arr.subtypeOf(BVec));
      arr.m_bits = bits;
      auto m = arr.m_data.mapn.mutate();
      auto newKey = union_of(std::move(m->key), TInt);
      auto newVal = union_of(std::move(m->val), val);
      if (!newKey.strictSubtypeOf(BArrKey) &&
          !newVal.strictSubtypeOf(BInitCell)) {
        return { Type { bits, arr.m_legacyMark }, true };
      }
      m->key = std::move(newKey);
      m->val = std::move(newVal);
      assertx(arr.checkInvariants());
      return { std::move(arr), true };
    }
  }

  not_reached();
}

//////////////////////////////////////////////////////////////////////

// Right now the only array set which can undone is one with
// ArrLikeMap specialization and where the key's constant value is
// known. We could also do ArrLikePacked, but there isn't a need at
// the moment. The other specializations don't preserve enough
// information (and ArrLikeVal is dealt with specially already).
bool array_like_set_can_be_undone(const Type& arr, const Type& k) {
  if (!k.subtypeOf(BArrKey) || k.is(BBottom)) return false;
  if (!arr.subtypeAmong(BDictN | BKeysetN, BArrLike)) return false;
  if (arr.m_dataTag != DataTag::ArrLikeMap) return false;
  auto const& map = *arr.m_data.map;
  if (map.hasOptElements()) return false;
  auto const kTV = tvCounted(k);
  if (!kTV) return false;
  return map.map.find(*kTV) == map.map.end();
}

// Undo the last set on this array. Since the pre-set array must meet
// array_like_set_can_be_undone requirements, we only need to delete
// the key from the specialization.
Type undo_array_like_set(Type arr, const Type& k) {
  assertx(k.subtypeOf(BArrKey));
  assertx(!k.is(BBottom));
  assertx(arr.subtypeAmong(BDictN | BKeysetN, BArrLike));
  assertx(arr.m_dataTag == DataTag::ArrLikeMap);
  auto map = arr.m_data.map.mutate();
  assertx(!map->hasOptElements());
  auto const kTV = tvCounted(k);
  assertx(kTV);
  auto const it = map->map.find(*kTV);
  // The key is guaranteed to exist and did not exist before the set,
  // so simply delete it.
  assertx(it != map->map.end());
  map->map.erase(it);
  return arr;
}

//////////////////////////////////////////////////////////////////////

std::pair<Type, Promotion> promote_classlike_to_key(Type ty) {
  // Cls and LazyCls promotes to the name of the class it represents
  // (a static string). If its a Cls, we can possibly know the string
  // statically.
  auto promoted = false;
  if (ty.couldBe(BLazyCls)) {
    auto t = [&] {
      if (!is_specialized_lazycls(ty)) return TSStr;
      auto const name = lazyclsval_of(ty);
      return sval(name);
    }();
    ty = union_of(remove_lazycls(std::move(ty)), std::move(t));
    promoted = true;
  }
  if (ty.couldBe(BCls)) {
    auto t = [&] {
      if (!is_specialized_cls(ty)) return TSStr;
      auto const& dcls = dcls_of(ty);
      if (!dcls.isExact()) return TSStr;
      return sval(dcls.cls().name());
    }();
    ty = union_of(remove_cls(std::move(ty)), std::move(t));
    promoted = true;
  }

  return std::make_pair(
    std::move(ty),
    promoted
    ? (Cfg::Eval::RaiseClassConversionNoticeSampleRate > 0
       ? Promotion::YesMightThrow
       : Promotion::Yes)
    : Promotion::No
  );
}

//////////////////////////////////////////////////////////////////////

Optional<RepoAuthType> make_repo_type_arr(const Type& t) {
  assertx(t.subtypeOf(BOptArrLike));
  assertx(is_specialized_array_like(t));

  using T = RepoAuthType::Tag;

  auto const tag = [&]() -> Optional<T> {
    #define X(tag)                                            \
      if (t.subtypeOf(BS##tag))    return T::S##tag##Spec;    \
      if (t.subtypeOf(B##tag))     return T::tag##Spec;       \
      if (t.subtypeOf(BOptS##tag)) return T::OptS##tag##Spec; \
      if (t.subtypeOf(BOpt##tag))  return T::Opt##tag##Spec;  \

    X(Vec)
    X(Dict)
    X(Keyset)
    #undef X

    // The JIT doesn't (currently) take advantage of specializations
    // for any array types except the above, so there's no point in
    // encoding them.
    return std::nullopt;
  }();
  if (!tag) return std::nullopt;

  // NB: Because of the type checks above for the tag, we know that
  // the empty bits must correspond to the specialization (which is
  // not true in general).
  auto const emptiness = t.couldBe(BArrLikeE)
    ? RepoAuthType::Array::Empty::Maybe
    : RepoAuthType::Array::Empty::No;

  auto const arr = [&]() -> const RepoAuthType::Array* {
    switch (t.m_dataTag) {
    case DataTag::None:
    case DataTag::Str:
    case DataTag::LazyCls:
    case DataTag::EnumClassLabel:
    case DataTag::Obj:
    case DataTag::WaitHandle:
    case DataTag::Int:
    case DataTag::Dbl:
    case DataTag::Cls:
      always_assert(false);
    case DataTag::ArrLikeVal:
    case DataTag::ArrLikeMap:
    case DataTag::ArrLikeMapN:
      return nullptr;
    case DataTag::ArrLikePackedN:
      return RepoAuthType::Array::packed(
        emptiness,
        make_repo_type(t.m_data.packedn->type)
      );
    case DataTag::ArrLikePacked:
      {
        std::vector<RepoAuthType> repoTypes;
        std::transform(
          begin(t.m_data.packed->elems), end(t.m_data.packed->elems),
          std::back_inserter(repoTypes),
          [&] (const Type& t2) { return make_repo_type(t2); }
        );
        return RepoAuthType::Array::tuple(emptiness, repoTypes);
      }
      return nullptr;
    }
    always_assert(false);
  }();
  if (!arr) return std::nullopt;

  return RepoAuthType { *tag, arr };
}

RepoAuthType make_repo_type(const Type& t) {
  assertx(t.subtypeOf(BCell));
  assertx(!t.subtypeOf(BBottom));
  using T = RepoAuthType::Tag;

  if (is_specialized_obj(t)) {
    if (t.subtypeOf(BOptObj)) {
      auto const& dobj = dobj_of(t);
      return RepoAuthType {
        dobj.isExact()
          ? (t.couldBe(BInitNull) ? T::OptExactObj : T::ExactObj)
          : (t.couldBe(BInitNull) ? T::OptSubObj : T::SubObj),
        dobj.smallestCls().name()
      };
    }
    if (t.subtypeOf(BUninit | BObj)) {
      auto const& dobj = dobj_of(t);
      return RepoAuthType {
        dobj.isExact() ? T::UninitExactObj : T::UninitSubObj,
        dobj.smallestCls().name()
      };
    }
  }

  if (is_specialized_cls(t) && t.subtypeOf(BOptCls)) {
    auto const& dcls = dcls_of(t);
    return RepoAuthType {
      dcls.isExact()
        ? (t.couldBe(BInitNull) ? T::OptExactCls : T::ExactCls)
        : (t.couldBe(BInitNull) ? T::OptSubCls : T::SubCls),
      dcls.smallestCls().name()
    };
  }

  if (is_specialized_array_like(t) && t.subtypeOf(BOptArrLike)) {
    if (auto const rat = make_repo_type_arr(t)) return *rat;
  }

#define X(tag) if (t.subtypeOf(B##tag)) return RepoAuthType{T::tag};

#define O(tag)                                                          \
  if (t.subtypeOf(B##tag)) return RepoAuthType{T::tag};                 \
  if (t.subtypeOf(BOpt##tag)) return RepoAuthType{T::Opt##tag};         \

#define U(tag)                                                            \
  O(tag)                                                                  \
  if (t.subtypeOf(BUninit | B##tag)) return RepoAuthType{T::Uninit##tag}; \

#define A(tag)                                \
  O(S##tag)                                   \
  O(tag)                                      \

#define Y(types, tag)                                                   \
  if (t.subtypeOf(types)) return RepoAuthType{T::tag};                  \
  if (t.subtypeOf(BInitNull | types)) return RepoAuthType{T::Opt##tag}; \

  X(Uninit)
  X(InitNull)
  X(Null)
  U(Int)
  O(Dbl)
  O(Num)
  O(Res)
  U(Bool)
  X(InitPrim)
  U(SStr)
  U(Str)
  A(Vec)
  A(Dict)
  A(Keyset)
  A(ArrLike)
  U(Obj)
  O(Func)
  O(Cls)
  O(LazyCls)
  O(ClsMeth)
  O(UncArrKey)
  O(ArrKey)
  Y(BSStr|BCls|BLazyCls,      UncStrLike)
  Y(BStr|BCls|BLazyCls,       StrLike)
  Y(BUncArrKey|BCls|BLazyCls, UncArrKeyCompat)
  Y(BArrKey|BCls|BLazyCls,    ArrKeyCompat)
  Y(BVec|BClsMeth,            VecCompat)
  Y(BArrLike|BClsMeth,        ArrLikeCompat)
  X(InitUnc)
  X(Unc)
  X(NonNull)
  X(InitCell)
  X(Cell)

#undef Y
#undef A
#undef U
#undef O
#undef X
  always_assert(false);
}

//////////////////////////////////////////////////////////////////////

// Testing only functions used to construct Types which are hard to
// construct using the normal interfaces.

Type set_trep_for_testing(Type t, trep bits) {
  t.m_bits = bits;
  t.m_legacyMark = Type::topLegacyMarkForBits(bits);
  assertx(t.checkInvariants());
  return t;
}

trep get_trep_for_testing(const Type& t) {
  return t.bits();
}

Type make_obj_for_testing(trep bits, res::Class cls,
                          bool isSub, bool isCtx, bool canonicalize) {
  if (canonicalize) {
    if (isSub) {
      if (auto const w = cls.withoutNonRegular()) {
        cls = *w;
      } else {
        return TBottom;
      }
    } else if (!cls.mightBeRegular()) {
      return TBottom;
    }
  }
  auto t = Type { bits, Type::topLegacyMarkForBits(bits) };
  auto const useSub = [&] {
    if (!canonicalize) return isSub;
    if (!isSub) return !cls.isComplete();
    return cls.couldBeOverriddenByRegular();
  }();
  construct(
    t.m_data.dobj,
    useSub ? DCls::MakeSub(cls, false) : DCls::MakeExact(cls, false)
  );
  t.m_dataTag = DataTag::Obj;
  t.m_data.dobj.setCtx(isCtx);
  assertx(t.checkInvariants());
  return t;
}

Type make_cls_for_testing(trep bits, res::Class cls,
                          bool isSub, bool isCtx,
                          bool canonicalize, bool nonReg) {
  if (canonicalize) {
    if (isSub) {
      if (!nonReg || !cls.mightContainNonRegular()) {
        if (auto const w = cls.withoutNonRegular()) {
          cls = *w;
        } else {
          return TBottom;
        }
        nonReg = false;
      }
    } else if (!nonReg || !cls.mightBeNonRegular()) {
      if (!cls.mightBeRegular()) return TBottom;
      nonReg = false;
    }
  }
  auto t = Type { bits, Type::topLegacyMarkForBits(bits) };
  auto const useSub = [&] {
    if (!canonicalize) return isSub;
    if (!isSub) return !cls.isComplete();
    return nonReg ? cls.couldBeOverridden() : cls.couldBeOverriddenByRegular();
  }();
  construct(
    t.m_data.dcls,
    useSub ? DCls::MakeSub(cls, nonReg) : DCls::MakeExact(cls, nonReg)
  );
  t.m_dataTag = DataTag::Cls;
  t.m_data.dcls.setCtx(isCtx);
  assertx(t.checkInvariants());
  return t;
}

Type make_arrval_for_testing(trep bits, SArray a) {
  auto t = Type { bits, legacyMarkFromSArr(a) };
  t.m_data.aval = a;
  t.m_dataTag = DataTag::ArrLikeVal;
  assertx(t.checkInvariants());
  return t;
}

Type make_arrpacked_for_testing(trep bits,
                                std::vector<Type> elems,
                                Optional<LegacyMark> mark) {
  auto t = Type { bits, Type::topLegacyMarkForBits(bits) };
  construct_inner(t.m_data.packed, std::move(elems));
  t.m_dataTag = DataTag::ArrLikePacked;
  if (mark) t.m_legacyMark = *mark;
  assertx(t.checkInvariants());
  return t;
}

Type make_arrpackedn_for_testing(trep bits, Type elem) {
  auto t = Type { bits, Type::topLegacyMarkForBits(bits) };
  construct_inner(t.m_data.packedn, std::move(elem));
  t.m_dataTag = DataTag::ArrLikePackedN;
  assertx(t.checkInvariants());
  return t;
}

Type make_arrmap_for_testing(trep bits,
                             MapElems m,
                             Type optKey,
                             Type optVal,
                             Optional<LegacyMark> mark) {
  auto t = Type { bits, Type::topLegacyMarkForBits(bits) };
  construct_inner(
    t.m_data.map,
    std::move(m),
    std::move(optKey),
    std::move(optVal)
  );
  t.m_dataTag = DataTag::ArrLikeMap;
  if (mark) t.m_legacyMark = *mark;
  assertx(t.checkInvariants());
  return t;
}

Type make_arrmapn_for_testing(trep bits, Type key, Type val) {
  auto t = Type { bits, Type::topLegacyMarkForBits(bits) };
  construct_inner(t.m_data.mapn, std::move(key), std::move(val));
  t.m_dataTag = DataTag::ArrLikeMapN;
  assertx(t.checkInvariants());
  return t;
}

Type set_mark_for_testing(Type t, LegacyMark mark) {
  t.m_legacyMark = mark;
  assertx(t.checkInvariants());
  return t;
}

Type loosen_mark_for_testing(Type t) {
  if (t.m_dataTag == DataTag::ArrLikeVal) return t;

  t.m_legacyMark = Type::topLegacyMarkForBits(t.bits());

  switch (t.m_dataTag) {
    case DataTag::None:
    case DataTag::Str:
    case DataTag::LazyCls:
    case DataTag::EnumClassLabel:
    case DataTag::Int:
    case DataTag::Dbl:
    case DataTag::Obj:
    case DataTag::Cls:
      break;

    case DataTag::WaitHandle: {
      auto& inner = t.m_data.dwh.mutate()->inner;
      inner = loosen_mark_for_testing(std::move(inner));
      break;
    }

    case DataTag::ArrLikePacked: {
      auto& packed = *t.m_data.packed.mutate();
      for (auto& e : packed.elems) {
        e = loosen_mark_for_testing(std::move(e));
      }
      break;
    }

    case DataTag::ArrLikePackedN: {
      auto& packed = *t.m_data.packedn.mutate();
      packed.type = loosen_mark_for_testing(std::move(packed.type));
      break;
    }

    case DataTag::ArrLikeMap: {
      auto& map = *t.m_data.map.mutate();
      for (auto it = map.map.begin(); it != map.map.end(); it++) {
        auto val = loosen_mark_for_testing(it->second.val);
        map.map.update(it, it->second.withType(std::move(val)));
      }
      map.optKey = loosen_mark_for_testing(std::move(map.optKey));
      map.optVal = loosen_mark_for_testing(std::move(map.optVal));
      break;
    }

    case DataTag::ArrLikeMapN: {
      auto& map = *t.m_data.mapn.mutate();
      map.key = loosen_mark_for_testing(std::move(map.key));
      map.val = loosen_mark_for_testing(std::move(map.val));
      break;
    }

    case DataTag::ArrLikeVal:
      always_assert(false);
  }

  assertx(t.checkInvariants());
  return t;
}

}
