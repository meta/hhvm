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
#include <fmt/format.h>

#include "hphp/runtime/base/repo-auth-type.h"

#include <vector>

#include "hphp/runtime/base/array-data-defs.h"
#include "hphp/runtime/base/object-data.h"
#include "hphp/runtime/base/typed-value.h"

#include "hphp/runtime/vm/func-emitter.h"
#include "hphp/runtime/vm/unit-emitter.h"
#include "hphp/runtime/vm/unit.h"

#include "hphp/util/blob-encoder.h"

namespace HPHP {

//////////////////////////////////////////////////////////////////////

static_assert(sizeof(RepoAuthType) == sizeof(CompactTaggedPtr<void>), "");

//////////////////////////////////////////////////////////////////////

namespace {

bool tvMatchesArrayType(TypedValue tv, const RepoAuthType::Array* arrTy) {
  assertx(isArrayLikeType(tv.m_type));
  auto const ad = tv.m_data.parr;
  using A = RepoAuthType::Array;

  if (ad->empty()) return arrTy->emptiness() == A::Empty::Maybe;
  if (arrTy->tag() == A::Tag::Tuple && ad->size() != arrTy->size()) {
    return false;
  }

  // O(N) checks are available if you want them for debugging, but
  // they are too slow for general use in debug builds.  These type
  // matching functions are currently only used for assertions, so
  // it's ok to leave them out.
  if (false) {
    switch (arrTy->tag()) {
      case A::Tag::Tuple:
        if (!ad->isVectorData()) return false;
        for (auto i = uint32_t{0}; i < ad->size(); ++i) {
          auto const elem = ad->at(i);
          if (!tvMatchesRepoAuthType(elem, arrTy->tupleElem(i))) {
            return false;
          }
        }
        break;
      case A::Tag::Packed:
        if (!ad->isVectorData()) return false;
        for (auto i = uint32_t{0}; i < ad->size(); ++i) {
          auto const elem = ad->at(i);
          if (!tvMatchesRepoAuthType(elem, arrTy->packedElems())) {
            return false;
          }
        }
        break;
    }
  }

  return true;
}

}

//////////////////////////////////////////////////////////////////////

bool RepoAuthType::operator==(RepoAuthType o) const {
  if (tag() != o.tag()) return false;
  if (name() != o.name()) return false;
  if (array() != o.array()) return false;
  return true;
}

size_t RepoAuthType::hash() const {
  auto const iTag = static_cast<size_t>(tag());
  if (auto const n = name()) {
    return folly::hash::hash_128_to_64(iTag, n->hash());
  }
  if (auto const a = array()) {
    // It's fine to use the pointer for the hash here, as we guarantee
    // the pointer uniquely identifies that array.
    return folly::hash::hash_128_to_64(
      iTag,
      pointer_hash<RepoAuthType::Array>{}(a)
    );
  }
  return iTag;
}

size_t RepoAuthType::stableHash() const {
  auto const iTag = static_cast<size_t>(tag());
  if (auto const n = name()) {
    return folly::hash::hash_128_to_64(iTag, n->hash());
  }
  if (auto const a = array()) {
    return folly::hash::hash_128_to_64(
      iTag, a->stableHash()
    );
  }
  return iTag;
}

//////////////////////////////////////////////////////////////////////

namespace {

//////////////////////////////////////////////////////////////////////

struct repo_auth_array_hash {
  size_t operator()(const RepoAuthType::Array* ar) const {
    return ar->stableHash();
  }
};

struct repo_auth_array_eq {
  bool operator()(const RepoAuthType::Array* a,
                  const RepoAuthType::Array* b) const {
    // We cannot rely on the array pointers being unique like normal
    // (since we're inserting here) (but sub-arrays are fine because
    // those are always inserted).
    if (a->tag() != b->tag() || a->emptiness() != b->emptiness()) {
      return false;
    }
    using T = RepoAuthType::Array::Tag;
    switch (a->tag()) {
      case T::Tuple: {
        if (a->size() != b->size()) return false;
        auto const size = a->size();
        for (auto i = uint32_t{0}; i < size; ++i) {
          if (a->tupleElem(i) != b->tupleElem(i)) return false;
        }
      }
      return true;
    case T::Packed:
      return a->packedElems() == b->packedElems();
    }
    not_reached();
  }
};

//////////////////////////////////////////////////////////////////////

folly_concurrent_hash_map_simd<
  const RepoAuthType::Array*,
  bool,
  repo_auth_array_hash,
  repo_auth_array_eq
> s_arrays;

// Returns the `cand' if it was successfully inserted; otherwise it's
// the callers responsibility to free it.
const RepoAuthType::Array* insert(const RepoAuthType::Array* cand) {
  auto const insert = s_arrays.emplace(cand, true);
  assertx(IMPLIES(insert.second, insert.first->first == cand));
  return insert.first->first;
}

//////////////////////////////////////////////////////////////////////

}

//////////////////////////////////////////////////////////////////////

const RepoAuthType::Array*
RepoAuthType::Array::tuple(Empty emptiness,
                           const std::vector<RepoAuthType>& types) {
  assertx(!types.empty());
  assertx(types.size() <= std::numeric_limits<uint32_t>::max());

  auto const size = types.size() * sizeof(RepoAuthType) +
    sizeof(RepoAuthType::Array);
  auto const arr = new (std::malloc(size)) RepoAuthType::Array(
    RepoAuthType::Array::Tag::Tuple,
    emptiness,
    types.size()
  );

  auto hash = folly::hash::hash_combine(
    RepoAuthType::Array::Tag::Tuple,
    static_cast<uint32_t>(emptiness)
  );
  hash = folly::hash::hash_128_to_64(hash, types.size());

  auto elems = arr->types();
  for (auto& t : types) {
    hash = folly::hash::hash_128_to_64(hash, t.stableHash());
    *elems++ = t;
  }
  arr->m_hash = hash;

  auto const ret = insert(arr);
  if (arr != ret) {
    arr->~Array();
    std::free(arr);
  }

  return ret;
}

const RepoAuthType::Array*
RepoAuthType::Array::packed(Empty emptiness, RepoAuthType elemTy) {
  auto const size = sizeof elemTy + sizeof(RepoAuthType::Array);
  auto const arr = new (std::malloc(size)) RepoAuthType::Array(
    RepoAuthType::Array::Tag::Packed,
    emptiness,
    std::numeric_limits<uint32_t>::max()
  );
  arr->types()[0] = elemTy;
  arr->m_hash = folly::hash::hash_combine(
    RepoAuthType::Array::Tag::Packed,
    static_cast<uint32_t>(emptiness),
    elemTy.stableHash()
  );

  auto const ret = insert(arr);
  if (arr != ret) {
    arr->~Array();
    std::free(arr);
  }

  return ret;
}

template <typename SerDe>
const RepoAuthType::Array*
RepoAuthType::Array::deserialize(SerDe& sd) {
  Tag tag;
  Empty emptiness;
  sd(tag)(emptiness);

  switch (tag) {
    case Tag::Tuple: {
      uint32_t size;
      sd(size);
      std::vector<RepoAuthType> types;
      types.reserve(size);
      for (uint32_t i = 0; i < size; ++i) {
        RepoAuthType type;
        sd(type);
        types.emplace_back(std::move(type));
      }
      return RepoAuthType::Array::tuple(emptiness, std::move(types));
    }
    case Tag::Packed: {
      RepoAuthType type;
      sd(type);
      return RepoAuthType::Array::packed(emptiness, std::move(type));
    }
  }
  always_assert(false);
}

template<typename SerDe>
void RepoAuthType::Array::serialize(SerDe& sd) const {
  sd(m_tag)(m_emptiness);

  switch (m_tag) {
    case Tag::Tuple: {
      sd(m_size);
      for (uint32_t i = 0; i < m_size; ++i) sd(types()[i]);
    }
    return;
  case Tag::Packed:
    sd(types()[0]);
    return;
  }
  always_assert(false);
}

//////////////////////////////////////////////////////////////////////

bool tvMatchesRepoAuthType(TypedValue tv, RepoAuthType ty) {
  assertx(tvIsPlausible(tv));

  auto const isUninit = tv.m_type == KindOfUninit;
  auto const isNull = tv.m_type == KindOfNull;

  auto const base = [&] {
    using T = RepoAuthType::Tag;

    #define O(type, check)                                         \
      case T::Opt##type: if (isNull) return true; [[fallthrough]]; \
      case T::type:      return check(tv.m_type);                  \

    #define U(type, check)                                       \
      case T::Uninit##type: return isUninit || check(tv.m_type); \
      O(type, check)                                             \

    #define S(type, check, ptr)                                                 \
      case T::Opt##type:  if (isNull) return true; [[fallthrough]];             \
      case T::type:       return check(tv.m_type);                              \
      case T::OptS##type: if (isNull) return true; [[fallthrough]];             \
      case T::S##type:    return check(tv.m_type) && tv.m_data.ptr->isStatic(); \

    #define N(type, check)                    \
      O(type,        check)                   \
      O(Exact##type, check)                   \
      O(Sub##type,   check)                   \

    #define UN(type, check)                   \
      U(type,        check)                   \
      U(Exact##type, check)                   \
      U(Sub##type,   check)                   \

    #define US(type, check, ptr)                                            \
      case T::Uninit##type: return isUninit || check(tv.m_type);            \
      case T::UninitS##type:                                                \
        return isUninit || (check(tv.m_type) && tv.m_data.ptr->isStatic()); \
      S(type, check, ptr)                                                   \

    #define OU(type, check1, check2)                                    \
      O(type, check1)                                                   \
      case T::OptUnc##type: if (isNull) return true; [[fallthrough]];   \
      case T::Unc##type:    return check1(tv.m_type) && !check2(tv);    \

    #define A(type, check, ptr)               \
      S(type, check, ptr)                     \
      S(type##Spec, check, ptr)               \

    auto const isNumType = [] (DataType t) {
      return isIntType(t) || isDoubleType(t);
    };
    auto const isInitPrimType = [&] (DataType t) {
      return isNull || isNumType(t) || isBoolType(t);
    };
    auto const isStrLikeType = [] (DataType t) {
      return isStringType(t) || isClassType(t) || isLazyClassType(t);
    };
    auto const isArrLikeCompatType = [] (DataType t) {
      return isArrayLikeType(t) || isClsMethType(t);
    };
    auto const isVecCompatType = [] (DataType t) {
      return isVecType(t) || isClsMethType(t);
    };
    auto const isArrKeyType = [] (DataType t) {
      return isIntType(t) || isStringType(t);
    };
    auto const isArrKeyCompatType = [&] (DataType t) {
      return isArrKeyType(t) || isClassType(t) || isLazyClassType(t);
    };

    auto const isUncounted = [] (const TypedValue& val) {
      return !isRefcountedType(val.m_type) ||
        (val.m_type == KindOfString && val.m_data.pstr->isStatic()) ||
        (isArrayLikeType(val.m_type) && val.m_data.parr->isStatic());
    };

    auto const isCountedStr = [] (const TypedValue& val) {
      return isStringType(val.m_type) && val.m_data.pstr->isRefCounted();
    };

    switch (ty.tag()) {
      U(Bool,          isBoolType)
      U(Int,           isIntType)
      O(Dbl,           isDoubleType)
      O(Res,           isResourceType)
      O(Func,          isFuncType)
      O(ClsMeth,       isClsMethType)
      O(LazyCls,       isLazyClassType)
      O(EnumClassLabel, isEnumClassLabelType)
      O(Num,           isNumType)
      O(VecCompat,     isVecCompatType)
      O(ArrLikeCompat, isArrLikeCompatType)
      N(Cls,           isClassType)
      UN(Obj,          isObjectType)
      OU(ArrKey,       isArrKeyType,       isCountedStr)
      OU(StrLike,      isStrLikeType,      isCountedStr)
      OU(ArrKeyCompat, isArrKeyCompatType, isCountedStr)
      US(Str,          isStringType,       pstr)
      S(ArrLike,       isArrayLikeType,    parr)
      A(Vec,           isVecType,          parr)
      A(Dict,          isDictType,         parr)
      A(Keyset,        isKeysetType,       parr)
      case T::Uninit:   return isUninit;
      case T::InitNull: return isNull;
      case T::Null:     return isNull || isUninit;
      case T::InitPrim: return isInitPrimType(tv.m_type);
      case T::NonNull:  return !isUninit && !isNull;
      case T::InitUnc:  return !isUninit && isUncounted(tv);
      case T::Unc:      return isUncounted(tv);
      case T::InitCell: return !isUninit;
      case T::Cell:     return true;
    }
    not_reached();

    #undef A
    #undef OU
    #undef US
    #undef UN
    #undef N
    #undef S
    #undef U
    #undef O
  }();
  if (!base) return false;
  if (isUninit || isNull) return true;

  if (auto const name = ty.name()) {
    auto const cls = Class::lookup(name);
    if (!cls) return false;
    auto const tvCls = [&] {
      if (tvIsObject(tv)) return tv.m_data.pobj->getVMClass();
      assertx(tvIsClass(tv));
      return tv.m_data.pclass;
    }();
    if (RepoAuthType::tagIsSubName(ty.tag())) {
      return tvCls->classof(cls);
    }
    assertx(RepoAuthType::tagIsExactName(ty.tag()));
    return tvCls == cls;
  }

  if (auto const array = ty.array()) {
    return tvMatchesArrayType(tv, array);
  }

  return true;
}

std::string show(RepoAuthType rat) {
  auto const base = [&] {
    using T = RepoAuthType::Tag;
    switch (rat.tag()) {
      #define TAG(x, name) case T::x: return name;
      REPO_AUTH_TYPE_TAGS(TAG)
      #undef TAG
    }
    not_reached();
  }();
  if (auto const n = rat.name()) {
    return fmt::format("{}{}", base, n->slice());
  } else if (auto const a = rat.array()) {
    return fmt::format("{}{}", base, show(*a));
  }
  return base;
}

std::string show(const RepoAuthType::Array& ar) {
  auto ret = std::string{};

  static constexpr size_t kPrintLimit = 4000;

  using A = RepoAuthType::Array;
  using T = A::Tag;
  switch (ar.emptiness()) {
  case A::Empty::No:
    ret += "N(";    // non-empty
    break;
  case A::Empty::Maybe:
    ret += '(';
    break;
  }

  switch (ar.tag()) {
  case T::Tuple:
    for (auto i = uint32_t{0}; i < ar.size(); ++i) {
      ret += show(ar.tupleElem(i));
      if (i != ar.size() - 1) ret += ',';
      if (ret.size() > kPrintLimit) break;
    }
    break;
  case T::Packed:
    fmt::format_to(std::back_inserter(ret), "[{}]", show(ar.packedElems()));
    break;
  }

  ret += ')';
  if (ret.size() > kPrintLimit) {
    ret.resize(kPrintLimit);
    ret += " <truncated>";
  }
  return ret;
}

//////////////////////////////////////////////////////////////////////

template <typename SerDe>
void RepoAuthType::serde(SerDe& sd) {
  if constexpr (SerDe::deserializing) {
    Tag t;
    sd(t);
    if (tagHasArrData(t)) {
      m_data.set(t, RepoAuthType::Array::deserialize(sd));
    } else if (tagHasName(t)) {
      const StringData* name;
      sd(name);
      always_assert(name);
      m_data.set(t, name);
    } else {
      m_data.set(t, nullptr);
    }
  } else {
    auto const t = tag();
    sd(t);
    if (auto const a = array()) {
      a->serialize(sd);
    } else if (auto const n = name()) {
      sd(n);
    }
  }
}

template void RepoAuthType::serde<>(BlobDecoder&);
template void RepoAuthType::serde<>(BlobEncoder&);

//////////////////////////////////////////////////////////////////////

namespace {

template <typename LookupStr, typename LookupArr>
RepoAuthType decodeRATImpl(const unsigned char*& pc,
                           LookupStr lookupStr,
                           LookupArr lookupArr) {
  using T = RepoAuthType::Tag;
  static_assert(sizeof(T) == sizeof(uint8_t));
  auto const tag = static_cast<T>(*pc++);

  if (RepoAuthType::tagHasArrData(tag)) {
    auto const id = decode_iva(pc);
    return RepoAuthType{tag, lookupArr(id)};
  }

  if (RepoAuthType::tagHasName(tag)) {
    auto const id = decode_iva(pc);
    return RepoAuthType{tag, lookupStr(id)};
  }

  return RepoAuthType{tag};
}

//////////////////////////////////////////////////////////////////////

}

RepoAuthType decodeRAT(const Unit* unit, const unsigned char*& pc) {
  return decodeRATImpl(
    pc,
    [&] (Id id) { return unit->lookupLitstrId(id); },
    [&] (Id id) { return unit->lookupRATArray(id); }
  );
}

RepoAuthType decodeRAT(const UnitEmitter& ue, const unsigned char*& pc) {
  return decodeRATImpl(
    pc,
    [&] (Id id) { return ue.lookupLitstrId(id); },
    [&] (Id id) { return ue.lookupRATArray(id); }
  );
}

void encodeRAT(FuncEmitter& fe, UnitEmitter& ue, RepoAuthType rat) {
  using T = RepoAuthType::Tag;
  static_assert(sizeof(T) == sizeof(uint8_t));

  fe.emitByte((uint8_t)rat.tag());
  if (auto const a = rat.array()) {
    fe.emitIVA(ue.mergeRATArray(a));
  } else if (auto const n = rat.name()) {
    fe.emitIVA(ue.mergeLitstr(n));
  }
}

size_t encodedRATSize(const unsigned char* pc) {
  using T = RepoAuthType::Tag;
  static_assert(sizeof(T) == sizeof(uint8_t));
  auto const tag = static_cast<T>(*pc++);

  if (!RepoAuthType::tagHasArrData(tag) && !RepoAuthType::tagHasName(tag)) {
    return 1;
  }

  auto const start = pc;
  decode_iva(pc);
  return
    reinterpret_cast<uintptr_t>(pc) - reinterpret_cast<uintptr_t>(start) + 1;
}

//////////////////////////////////////////////////////////////////////

}
