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

#include <type_traits>

#include "hphp/runtime/base/array-data-defs.h"
#include "hphp/runtime/base/array-data.h"
#include "hphp/runtime/base/bespoke-array.h"
#include "hphp/runtime/base/builtin-functions.h"
#include "hphp/runtime/base/collections.h"
#include "hphp/runtime/base/datatype.h"
#include "hphp/runtime/base/req-root.h"
#include "hphp/runtime/base/strings.h"
#include "hphp/runtime/base/tv-conversions.h"
#include "hphp/runtime/base/tv-refcount.h"
#include "hphp/runtime/base/tv-type.h"
#include "hphp/runtime/base/type-array.h"
#include "hphp/runtime/base/type-string.h"
#include "hphp/runtime/base/vanilla-dict.h"
#include "hphp/runtime/base/vanilla-keyset.h"
#include "hphp/runtime/base/vanilla-vec.h"
#include "hphp/runtime/vm/runtime.h"
#include "hphp/runtime/vm/type-constraint.h"
#include "hphp/system/systemlib.h"
#include "hphp/util/configs/php7.h"

#include <fmt/format.h>
#include <usdt/usdt.h>

namespace HPHP {

struct InvalidSetMException : std::runtime_error {
  InvalidSetMException()
    : std::runtime_error("Empty InvalidSetMException")
    , m_tv(make_tv<KindOfNull>())
  {}

  explicit InvalidSetMException(const TypedValue& value)
    : std::runtime_error(fmt::format("InvalidSetMException containing {}",
                                       value.pretty()))
    , m_tv(value)
  {}

  ~InvalidSetMException() noexcept override {}

  const TypedValue& tv() const { return m_tv; }

 private:
  /* m_tv will contain a TypedValue with a reference destined for the
   * VM eval stack. */
  req::root<TypedValue> m_tv;
};

/*
 * KeyType and the associated functions below are used to generate member
 * operation functions specialized for certain key types. Many functions take a
 * KeyType template parameter, then use key_type<keyType> as the type of their
 * key parameter. Depending on which KeyType is used, the parameter will be a
 * TypedValue, int64_t, or StringData*.
 */
enum class KeyType {
  Any, // Key is passed as a TypedValue and could be any type
  Int, // Key is passed as an int64_t
  Str, // Key is passed as a StringData*
};

/* KeyTypeTraits maps from KeyType to the C++ type holding they key. */
template<KeyType> struct KeyTypeTraits;
template<> struct KeyTypeTraits<KeyType::Any> { using type = TypedValue; };
template<> struct KeyTypeTraits<KeyType::Int> { using type = int64_t; };
template<> struct KeyTypeTraits<KeyType::Str> { using type = StringData*; };

/* key_type is the type used in the signatures of functions taking a member
 * key. */
template<KeyType kt> using key_type = typename KeyTypeTraits<kt>::type;

/* initScratchKey is used in scenarios where we want a TypedValue key
 * regardless of what the current function was given. */
inline TypedValue initScratchKey(TypedValue tv) {
  return tv;
}

inline TypedValue initScratchKey(int64_t key) {
  return make_tv<KindOfInt64>(key);
}

inline TypedValue initScratchKey(StringData* key) {
  return make_tv<KindOfString>(key);
}

/* prepareKey is used by operations that need to cast their key to a
 * string. For generic keys, the returned value must be decreffed after use. */
StringData* prepareAnyKey(TypedValue* tv);
inline StringData* prepareKey(TypedValue tv)  { return prepareAnyKey(&tv); }
inline StringData* prepareKey(StringData* sd) { return sd; }

/* releaseKey helps with decreffing a StringData* returned from
 * prepareKey. When used with KeyType::Any, corresponding to
 * prepareKey(TypedValue), it will consume the reference produced by
 * prepareKey. When used with KeyType::Str, corresponding to
 * prepareKey(StringData*), it is a nop. */
template<KeyType keyType>
inline void releaseKey(StringData* sd) {
  static_assert(keyType == KeyType::Any, "bad KeyType");
  decRefStr(sd);
}

template<>
inline void releaseKey<KeyType::Str>(StringData*) {
  // do nothing. we don't own a reference to this string.
}

inline void failOnNonCollectionObjArrayAccess(ObjectData* obj) {
  if (UNLIKELY(!obj->isCollection())) {
    raise_error("Cannot use array access on an object");
  }
}

[[noreturn]] void throw_cannot_use_newelem_for_lval_read_col();
[[noreturn]] void throw_cannot_use_newelem_for_lval_read(const ArrayData*);
[[noreturn]] void throw_cannot_use_newelem_for_lval_read_clsmeth();
[[noreturn]] void throw_cannot_unset_for_clsmeth();

[[noreturn]] void unknownBaseType(DataType);

// Sampled log of a dynamic property access ($foo->$bar); the caller must first
// confirm the access is dynamic (member key MPC/MPL).
void logDynamicPropAccess(TypedValue base, TypedValue key);

namespace detail {

[[noreturn]]
inline void raiseFalseyPromotion(tv_rval base) {
  if (tvIsNull(base)) {
    throwFalseyPromoteException("null");
  } else if (tvIsBool(base)) {
    throwFalseyPromoteException("false");
  } else if (tvIsString(base)) {
    throwFalseyPromoteException("empty string");
  }
  always_assert(false);
}

[[noreturn]]
inline void raiseEmptyObject() {
  if (Cfg::PHP7::EngineExceptions) {
    SystemLib::throwErrorObject(Strings::SET_PROP_NON_OBJECT);
  } else {
    SystemLib::throwExceptionObject(Strings::SET_PROP_NON_OBJECT);
  }
}
}

/**
 * Elem when base is Null
 */
inline TypedValue ElemEmptyish() {
  return make_tv<KindOfNull>();
}

/**
 * Elem when base is a Vec
 */
template<MOpMode mode>
inline TypedValue ElemVecPre(ArrayData* base, int64_t key) {
  return VanillaVec::NvGetInt(base, key);
}

template<MOpMode mode>
inline TypedValue ElemVecPre(ArrayData* base, StringData* key) {
  if (mode == MOpMode::Warn || mode == MOpMode::InOut) {
    throwInvalidArrayKeyException(key, base);
  }
  return make_tv<KindOfUninit>();
}

template<MOpMode mode>
inline TypedValue ElemVecPre(ArrayData* base, TypedValue key) {
  auto const dt = key.m_type;
  if (isIntType(dt))    return ElemVecPre<mode>(base, key.m_data.num);
  if (isStringType(dt)) return ElemVecPre<mode>(base, key.m_data.pstr);
  throwInvalidArrayKeyException(&key, base);
}

template<MOpMode mode, KeyType keyType>
inline TypedValue ElemVec(ArrayData* base, key_type<keyType> key) {
  assertx(base->isVanillaVec());
  auto const result = ElemVecPre<mode>(base, key);
  if (UNLIKELY(!result.is_init())) {
    if (mode != MOpMode::Warn && mode != MOpMode::InOut) return ElemEmptyish();
    throwOOBArrayKeyException(key, base);
  }
  assertx(result.type() != KindOfUninit);
  return result;
}

/**
 * Elem when base is a Dict
 */
inline TypedValue ElemDictPre(ArrayData* base, int64_t key) {
  return VanillaDict::NvGetInt(base, key);
}

inline TypedValue ElemDictPre(ArrayData* base, StringData* key) {
  return VanillaDict::NvGetStr(base, key);
}

inline TypedValue ElemDictPre(ArrayData* base, TypedValue key) {
  auto const dt = key.m_type;
  if (isIntType(dt))    return ElemDictPre(base, key.m_data.num);
  if (isStringType(dt)) return ElemDictPre(base, key.m_data.pstr);
  throwInvalidArrayKeyException(&key, base);
}

// This helper may also be used when we know we have a VanillaDict in the JIT.
template<MOpMode mode, KeyType keyType>
inline TypedValue ElemDict(ArrayData* base, key_type<keyType> key) {
  assertx(base->isVanillaDict());
  auto const result = ElemDictPre(base, key);
  if (UNLIKELY(!result.is_init())) {
    if (mode != MOpMode::Warn && mode != MOpMode::InOut) return ElemEmptyish();
    throwOOBArrayKeyException(key, base);
  }
  assertx(result.type() != KindOfUninit);
  return result;
}

/**
 * Elem when base is a Keyset
 */
inline TypedValue ElemKeysetPre(ArrayData* base, int64_t key) {
  return VanillaKeyset::NvGetInt(base, key);
}

inline TypedValue ElemKeysetPre(ArrayData* base, StringData* key) {
  return VanillaKeyset::NvGetStr(base, key);
}

inline TypedValue ElemKeysetPre(ArrayData* base, TypedValue key) {
  auto const dt = key.m_type;
  if (isIntType(dt))    return ElemKeysetPre(base, key.m_data.num);
  if (isStringType(dt)) return ElemKeysetPre(base, key.m_data.pstr);
  throwInvalidArrayKeyException(&key, base);
}

template<MOpMode mode, KeyType keyType>
inline TypedValue ElemKeyset(ArrayData* base, key_type<keyType> key) {
  assertx(base->isVanillaKeyset());
  auto result = ElemKeysetPre(base, key);
  if (UNLIKELY(!result.is_init())) {
    if (mode != MOpMode::Warn && mode != MOpMode::InOut) return ElemEmptyish();
    throwOOBArrayKeyException(key, base);
  }
  assertx(isIntType(result.type()) || isStringType(result.type()));
  return result;
}

/**
 * Elem when base is a bespoke Hack array
 */
template<MOpMode mode>
inline TypedValue ElemBespokePre(ArrayData* base, int64_t key) {
  return BespokeArray::NvGetInt(base, key);
}

template<MOpMode mode>
inline TypedValue ElemBespokePre(ArrayData* base, StringData* key) {
  if ((mode == MOpMode::Warn || mode == MOpMode::InOut) && base->isVecType()) {
    throwInvalidArrayKeyException(key, base);
  }
  return BespokeArray::NvGetStr(base, key);
}

template<MOpMode mode>
inline TypedValue ElemBespokePre(ArrayData* base, TypedValue key) {
  auto const dt = key.m_type;
  if (isIntType(dt))    return ElemBespokePre<mode>(base, key.m_data.num);
  if (isStringType(dt)) return ElemBespokePre<mode>(base, key.m_data.pstr);
  throwInvalidArrayKeyException(&key, base);
}

template<MOpMode mode, KeyType keyType>
inline TypedValue ElemBespoke(ArrayData* base, key_type<keyType> key) {
  assertx(!base->isVanilla());
  auto const result = ElemBespokePre<mode>(base, key);
  if (UNLIKELY(!result.is_init())) {
    if (mode != MOpMode::Warn && mode != MOpMode::InOut) return ElemEmptyish();
    throwOOBArrayKeyException(key, base);
  }
  return result;
}

/**
 * Elem when base is a ClsMeth
 */
template<MOpMode mode>
inline TypedValue ElemClsMethPre(ClsMethDataRef base, int64_t key) {
  if (key == 0) {
    return make_tv<KindOfString>(
      const_cast<StringData*>(base->getCls()->name()));
  } else if (key == 1) {
    return make_tv<KindOfString>(
      const_cast<StringData*>(base->getFunc()->name()));
  }
  if (mode == MOpMode::Warn || mode == MOpMode::InOut) {
    SystemLib::throwOutOfBoundsExceptionObject(
      fmt::format("Out of bounds clsmeth access: invalid index {}", key));
  }
  return make_tv<KindOfNull>();
}

template<MOpMode mode>
inline TypedValue ElemClsMethPre(ClsMethDataRef base, StringData* key) {
  if (mode == MOpMode::Warn || mode == MOpMode::InOut) {
    SystemLib::throwInvalidArgumentExceptionObject(
      "Invalid clsmeth key: expected a key of type int, string given");
  }
  return make_tv<KindOfNull>();
}

template<MOpMode mode>
inline TypedValue ElemClsMethPre(ClsMethDataRef base, TypedValue key) {
  if (LIKELY(isIntType(type(key)))) {
    return ElemClsMethPre<mode>(base, val(key).num);
  }
  if (mode == MOpMode::Warn || mode == MOpMode::InOut) {
    SystemLib::throwInvalidArgumentExceptionObject(
      "Invalid clsmeth key: expected a key of type int");
  }
  return make_tv<KindOfNull>();
}

template<MOpMode mode, KeyType keyType>
inline TypedValue ElemClsMeth(ClsMethDataRef base, key_type<keyType> key) {
  return ElemClsMethPre<mode>(base, key);
}

/**
 * Elem when base is an Int64, Double, or Resource.
 */
inline TypedValue ElemScalar() {
  raise_warning(Strings::CANNOT_USE_SCALAR_AS_ARRAY);
  return ElemEmptyish();
}

/**
 * Elem when base is a Boolean
 */
inline TypedValue ElemBoolean(TypedValue base) {
  return val(base).num ? ElemScalar() : ElemEmptyish();
}

inline int64_t ElemStringPre(int64_t key) {
  return key;
}

inline int64_t ElemStringPre(StringData* key) {
  return key->toInt64(10);
}

inline int64_t ElemStringPre(TypedValue key) {
  if (LIKELY(isIntType(key.m_type))) {
    return key.m_data.num;
  }
  if (LIKELY(isStringType(key.m_type))) {
    return key.m_data.pstr->toInt64(10);
  }
  SystemLib::throwInvalidArgumentExceptionObject(
    fmt::format(
      "Invalid string key: expected a key of type int or string, {} given",
      describe_actual_type(&key)
    )
  );
}

/**
 * Elem when base is a String
 */
template<MOpMode mode, KeyType keyType>
inline TypedValue ElemString(const StringData* base, key_type<keyType> key) {
  auto const offset = ElemStringPre(key);

  if (size_t(offset) >= base->size()) {
    if (mode == MOpMode::Warn) {
      raise_notice("Uninitialized string offset: %" PRId64, offset);
    }
    return make_tv<KindOfPersistentString>(staticEmptyString());
  } else {
    auto const sd = base->getChar(offset);
    assertx(sd->isStatic());
    return make_tv<KindOfPersistentString>(sd);
  }
}

/**
 * Elem when base is an Object
 */
template<MOpMode mode, KeyType keyType>
inline TypedValue ElemObject(ObjectData* base, key_type<keyType> key) {
  failOnNonCollectionObjArrayAccess(base);

  auto scratch = initScratchKey(key);
  if (mode == MOpMode::Warn) return *collections::at(base, &scratch);
  auto const result = collections::get(base, &scratch);
  return result ? *result : make_tv<KindOfNull>();
}

/**
 * $result = $base[$key];
 */
template<MOpMode mode, KeyType keyType>
NEVER_INLINE TypedValue ElemSlow(TypedValue base, key_type<keyType> key) {
  assertx(tvIsPlausible(base));

  auto const op = "index";
  switch (base.type()) {
    case KindOfUninit:
    case KindOfNull:
      return ElemEmptyish();
    case KindOfBoolean:
      return ElemBoolean(base);
    case KindOfInt64:
    case KindOfDouble:
    case KindOfResource:
    case KindOfRFunc:
    case KindOfRClsMeth:
    case KindOfFunc:
    case KindOfEnumClassLabel:
      return ElemScalar();
    case KindOfClass:
      return ElemString<mode, keyType>(
        classToStringHelper(val(base).pclass, op), key
      );
    case KindOfLazyClass:
      return ElemString<mode, keyType>(
        lazyClassToStringHelper(val(base).plazyclass, op), key
      );
    case KindOfPersistentString:
    case KindOfString:
      return ElemString<mode, keyType>(val(base).pstr, key);

    // These types are handled in Elem.
    case KindOfPersistentVec:
    case KindOfVec:
    case KindOfPersistentDict:
    case KindOfDict:
    case KindOfPersistentKeyset:
    case KindOfKeyset:
      always_assert(false);

    case KindOfObject:
      return ElemObject<mode, keyType>(val(base).pobj, key);

    case KindOfClsMeth:
      return ElemScalar();
  }
  unknownBaseType(type(base));
}

template<MOpMode mode, KeyType keyType = KeyType::Any>
inline TypedValue Elem(TypedValue base, key_type<keyType> key) {
  assertx(mode != MOpMode::Define && mode != MOpMode::Unset);
  assertx(tvIsPlausible(base));

  if (tvIsArrayLike(base)) {
    auto const ad = val(base).parr;
    if (!ad->isVanilla()) {
      return ElemBespoke<mode, keyType>(ad, key);
    } else if (ad->isVanillaVec()) {
      return ElemVec<mode, keyType>(ad, key);
    } else if (ad->isVanillaDict()) {
      return ElemDict<mode, keyType>(ad, key);
    } else {
      return ElemKeyset<mode, keyType>(ad, key);
    }
  }

  if (mode == MOpMode::InOut) throw_invalid_inout_base();
  return ElemSlow<mode, keyType>(base, key);
}

/**
 * ElemD when base is a bespoke array-like
 */
inline tv_lval ElemDBespokePre(tv_lval base, int64_t key) {
  auto ret = BespokeArray::ElemInt(base, key, true);
  assertx(tvIsArrayLike(base));
  assertx(base.val().parr->hasExactlyOneRef());
  return ret;
}

inline tv_lval ElemDBespokePre(tv_lval base, StringData* key) {
  auto ret = BespokeArray::ElemStr(base, key, true);
  assertx(tvIsArrayLike(base));
  assertx(base.val().parr->hasExactlyOneRef());
  return ret;
}

inline tv_lval ElemDBespokePre(tv_lval base, TypedValue key) {
  auto const dt = key.m_type;
  if (isIntType(dt))    return ElemDBespokePre(base, key.m_data.num);
  if (isStringType(dt)) return ElemDBespokePre(base, key.m_data.pstr);
  throwInvalidArrayKeyException(&key, base.val().parr);
}

template <KeyType keyType>
inline tv_lval ElemDBespoke(tv_lval base, key_type<keyType> key) {
  assertx(tvIsArrayLike(base));
  assertx(tvIsPlausible(*base));

  assertx(!base.val().parr->isVanilla());
  auto const result = ElemDBespokePre(base, key);
  assertx(result.type() == dt_modulo_persistence(result.type()));
  assertx(tvIsPlausible(*base));
  assertx(result.type() != KindOfUninit);
  return result;
}

/**
 * ElemD when base is a Vec
 */
inline tv_lval ElemDVecPre(tv_lval base, int64_t key) {
  auto const oldArr = base.val().parr;
  auto const lval = VanillaVec::LvalInt(oldArr, key);

  assertx(lval.arr->hasExactlyOneRef());
  if (lval.arr != oldArr) {
    base.type() = dt_with_rc(base.type());
    base.val().parr = lval.arr;
    assertx(tvIsPlausible(base.tv()));
    decRefArr(oldArr);
  }
  return lval;
}

inline tv_lval ElemDVecPre(tv_lval base, StringData* key) {
  throwInvalidArrayKeyException(key, base.val().parr);
}

inline tv_lval ElemDVecPre(tv_lval base, TypedValue key) {
  auto const dt = key.m_type;
  if (LIKELY(isIntType(dt))) {
    return ElemDVecPre(base, key.m_data.num);
  } else if (isStringType(dt)) {
    return ElemDVecPre(base, key.m_data.pstr);
  }
  throwInvalidArrayKeyException(&key, base.val().parr);
}

template <KeyType keyType>
inline tv_lval ElemDVec(tv_lval base, key_type<keyType> key) {
  assertx(tvIsVec(base));
  assertx(tvIsPlausible(base.tv()));
  auto const result = ElemDVecPre(base, key);
  assertx(tvIsVec(base));
  assertx(tvIsPlausible(base.tv()));

  assertx(result.type() != KindOfUninit);
  return result;
}

/**
 * ElemD when base is a Dict
 */
inline tv_lval ElemDDictPre(tv_lval base, int64_t key) {
  auto const oldArr = base.val().parr;
  auto const lval = VanillaDict::LvalSilentInt(oldArr, key);

  if (UNLIKELY(!lval)) {
    assertx(oldArr == lval.arr);
    throwOOBArrayKeyException(key, oldArr);
  }

  assertx(lval.arr->hasExactlyOneRef());
  if (lval.arr != oldArr) {
    base.type() = dt_with_rc(base.type());
    base.val().parr = lval.arr;
    assertx(tvIsPlausible(base.tv()));
    decRefArr(oldArr);
  }

  return lval;
}

inline tv_lval ElemDDictPre(tv_lval base, StringData* key) {
  auto const oldArr = base.val().parr;
  auto const lval = VanillaDict::LvalSilentStr(oldArr, key);

  if (UNLIKELY(!lval)) {
    assertx(oldArr == lval.arr);
    throwOOBArrayKeyException(key, oldArr);
  }

  assertx(lval.arr->hasExactlyOneRef());
  if (lval.arr != oldArr) {
    base.type() = dt_with_rc(base.type());
    base.val().parr = lval.arr;
    assertx(tvIsPlausible(base.tv()));
    decRefArr(oldArr);
  }

  return lval;
}

inline tv_lval ElemDDictPre(tv_lval base, TypedValue key) {
  auto const dt = key.m_type;
  if (isIntType(dt)) {
    return ElemDDictPre(base, key.m_data.num);
  } else if (isStringType(dt)) {
    return ElemDDictPre(base, key.m_data.pstr);
  }
  throwInvalidArrayKeyException(&key, base.val().parr);
}

template <KeyType keyType>
inline tv_lval ElemDDict(tv_lval base, key_type<keyType> key) {
  assertx(tvIsDict(base));
  assertx(tvIsPlausible(base.tv()));
  auto result = ElemDDictPre(base, key);
  assertx(tvIsDict(base));
  assertx(tvIsPlausible(base.tv()));

  assertx(result.type() != KindOfUninit);
  return result;
}

/**
 * ElemD when base is a Keyset
 */
[[noreturn]]
inline tv_lval ElemDKeysetPre(tv_lval /*base*/, int64_t /*key*/) {
  throwInvalidKeysetOperation();
}

[[noreturn]]
inline tv_lval ElemDKeysetPre(tv_lval /*base*/, StringData* /*key*/) {
  throwInvalidKeysetOperation();
}

[[noreturn]]
inline tv_lval ElemDKeysetPre(tv_lval base, TypedValue key) {
  auto const dt = key.m_type;
  if (isIntType(dt))    ElemDKeysetPre(base, key.m_data.num);
  if (isStringType(dt)) ElemDKeysetPre(base, key.m_data.pstr);
  throwInvalidArrayKeyException(&key, base.val().parr);
}

template <KeyType keyType>
[[noreturn]]
inline tv_lval ElemDKeyset(tv_lval base, key_type<keyType> key) {
  assertx(tvIsKeyset(base));
  assertx(tvIsPlausible(base.tv()));
  ElemDKeysetPre(base, key);
}

/**
 * ElemD when base is Null
 */
[[noreturn]]
inline tv_lval ElemDEmptyish(tv_lval base) {
  detail::raiseFalseyPromotion(base);
  not_reached();
}

/**
 * ElemD when base is an Int64, Double, Resource, Func, or Class.
 * We can use immutable_null_base here because setters on null will throw.
 */
inline tv_lval ElemDScalar() {
  raise_warning(Strings::CANNOT_USE_SCALAR_AS_ARRAY);
  return const_cast<TypedValue*>(&immutable_null_base);
}

/**
 * ElemD when base is a Boolean
 */
inline tv_lval ElemDBoolean(tv_lval base) {
  return base.val().num ? ElemDScalar() : ElemDEmptyish(base);
}

/**
 * ElemD when base is a String
 */
[[noreturn]]
inline tv_lval ElemDString(tv_lval base) {
  if (!base.val().pstr->size()) ElemDEmptyish(base);
  raise_error("Operator not supported for strings");
  not_reached();
}

/**
 * ElemD when base is an Object
 */
template<KeyType keyType>
inline tv_lval ElemDObject(tv_lval base, key_type<keyType> key) {
  auto obj = base.val().pobj;
  failOnNonCollectionObjArrayAccess(obj);
  auto scratchKey = initScratchKey(key);
  return collections::atLval(obj, &scratchKey);
}

/*
 * Intermediate elem operation for defining member instructions.
 */
template<KeyType keyType = KeyType::Any>
tv_lval ElemD(tv_lval base, key_type<keyType> key) {
  assertx(tvIsPlausible(base.tv()));

  // ElemD helpers hand out lvals to immutable_null_base in cases where we know
  // it won't be updated. Confirm that we never do an illegal update on it.
  assertx(type(immutable_null_base) == KindOfNull);

  if (tvIsArrayLike(base) && !base.val().parr->isVanilla()) {
    return ElemDBespoke<keyType>(base, key);
  }

  switch (base.type()) {
    case KindOfUninit:
    case KindOfNull:
      return ElemDEmptyish(base);
    case KindOfBoolean:
      return ElemDBoolean(base);
    case KindOfInt64:
    case KindOfDouble:
    case KindOfResource:
    case KindOfRFunc:
    case KindOfFunc:
    case KindOfRClsMeth:
    case KindOfClass:
    case KindOfLazyClass:
    case KindOfEnumClassLabel:
      return ElemDScalar();
    case KindOfPersistentString:
    case KindOfString:
      return ElemDString(base);
    case KindOfPersistentKeyset:
    case KindOfKeyset:
      return ElemDKeyset<keyType>(base, key);
    case KindOfPersistentDict:
    case KindOfDict:
      return ElemDDict<keyType>(base, key);
    case KindOfPersistentVec:
    case KindOfVec:
      return ElemDVec<keyType>(base, key);
    case KindOfObject:
      return ElemDObject<keyType>(base, key);
    case KindOfClsMeth:
      return ElemDScalar();
  }
  unknownBaseType(type(base));
}

/**
 * ElemU when base is Null. We can use immutable_null_base here because
 * unsets on null will succeed with no further updates.
 */
inline tv_lval ElemUEmptyish() {
  return const_cast<TypedValue*>(&immutable_null_base);
}

/**
 * ElemU when base is a bespoke array-like
 */
inline tv_lval ElemUBespokePre(tv_lval base, int64_t key) {
  if (tvIsKeyset(base)) throwInvalidKeysetOperation();
  return BespokeArray::ElemInt(base, key, false);
}

inline tv_lval ElemUBespokePre(tv_lval base, StringData* key) {
  if (tvIsKeyset(base)) throwInvalidKeysetOperation();
  return BespokeArray::ElemStr(base, key, false);
}

inline tv_lval ElemUBespokePre(tv_lval base, TypedValue key) {
  auto const dt = key.m_type;
  if (isIntType(dt))    return ElemUBespokePre(base, key.m_data.num);
  if (isStringType(dt)) return ElemUBespokePre(base, key.m_data.pstr);
  throwInvalidArrayKeyException(&key, base.val().parr);
}

template <KeyType keyType>
inline tv_lval ElemUBespoke(tv_lval base, key_type<keyType> key) {
  assertx(tvIsArrayLike(base));
  assertx(tvIsPlausible(*base));

  assertx(!base.val().parr->isVanilla());
  auto const result = ElemUBespokePre(base, key);
  assertx(tvIsPlausible(*base));
  assertx(result.type() != KindOfUninit);
  return result;
}

/**
 * ElemU when base is a Vec
 */
inline tv_lval ElemUVecPre(tv_lval base, int64_t key) {
  auto const oldArr = val(base).parr;
  if (UNLIKELY(!VanillaVec::ExistsInt(oldArr, key))) {
    return ElemUEmptyish();
  }

  auto const newArr = [&]{
    if (!oldArr->cowCheck()) return oldArr;
    decRefArr(oldArr);
    auto const newArr = VanillaVec::Copy(oldArr);
    type(base) = dt_with_rc(type(base));
    val(base).parr = newArr;
    assertx(tvIsPlausible(*base));
    return newArr;
  }();

  return VanillaVec::LvalUncheckedInt(newArr, key);
}

inline tv_lval ElemUVecPre(tv_lval /*base*/, StringData* /*key*/) {
  return ElemUEmptyish();
}

inline tv_lval ElemUVecPre(tv_lval base, TypedValue key) {
  auto const dt = key.m_type;
  if (LIKELY(isIntType(dt))) return ElemUVecPre(base, key.m_data.num);
  if (isStringType(dt))      return ElemUVecPre(base, key.m_data.pstr);
  throwInvalidArrayKeyException(&key, val(base).parr);
}

template <KeyType keyType>
inline tv_lval ElemUVec(tv_lval base, key_type<keyType> key) {
  assertx(tvIsVec(base));
  assertx(tvIsPlausible(*base));
  auto result = ElemUVecPre(base, key);
  assertx(tvIsVec(base));
  assertx(tvIsPlausible(*base));
  assertx(type(result) != KindOfUninit);
  return result;
}

/**
 * ElemU when base is a Dict
 */
inline tv_lval ElemUDictPre(tv_lval base, int64_t key) {
  ArrayData* oldArr = val(base).parr;
  auto const lval = VanillaDict::LvalSilentInt(oldArr, key);

  if (UNLIKELY(!lval)) {
    return ElemUEmptyish();
  }
  if (lval.arr != oldArr) {
    type(base) = dt_with_rc(type(base));
    val(base).parr = lval.arr;
    assertx(tvIsPlausible(*base));
    decRefArr(oldArr);
  }
  return lval;
}

inline tv_lval ElemUDictPre(tv_lval base, StringData* key) {
  ArrayData* oldArr = val(base).parr;
  auto const lval = VanillaDict::LvalSilentStr(oldArr, key);

  if (UNLIKELY(!lval)) {
    return ElemUEmptyish();
  }
  if (lval.arr != oldArr) {
    type(base) = dt_with_rc(type(base));
    val(base).parr = lval.arr;
    assertx(tvIsPlausible(*base));
    decRefArr(oldArr);
  }
  return lval;
}

inline tv_lval ElemUDictPre(tv_lval base, TypedValue key) {
  auto const dt = key.m_type;
  if (isIntType(dt))    return ElemUDictPre(base, key.m_data.num);
  if (isStringType(dt)) return ElemUDictPre(base, key.m_data.pstr);
  throwInvalidArrayKeyException(&key, val(base).parr);
}

template <KeyType keyType>
inline tv_lval ElemUDict(tv_lval base, key_type<keyType> key) {
  assertx(tvIsDict(base));
  assertx(tvIsPlausible(*base));
  auto result = ElemUDictPre(base, key);
  assertx(tvIsDict(base));
  assertx(tvIsPlausible(*base));
  assertx(type(result) != KindOfUninit);
  return result;
}

/**
 * ElemU when base is a Keyset
 */
[[noreturn]] inline tv_lval
ElemUKeysetPre(tv_lval /*base*/, int64_t /*key*/) {
  throwInvalidKeysetOperation();
}

[[noreturn]] inline tv_lval
ElemUKeysetPre(tv_lval /*base*/, StringData* /*key*/) {
  throwInvalidKeysetOperation();
}

[[noreturn]]
inline tv_lval ElemUKeysetPre(tv_lval base, TypedValue key) {
  auto const dt = key.m_type;
  if (isIntType(dt))    ElemUKeysetPre(base, key.m_data.num);
  if (isStringType(dt)) ElemUKeysetPre(base, key.m_data.pstr);
  throwInvalidArrayKeyException(&key, val(base).parr);
}

template <KeyType keyType>
[[noreturn]]
inline tv_lval ElemUKeyset(tv_lval base, key_type<keyType> key) {
  assertx(tvIsKeyset(base));
  assertx(tvIsPlausible(*base));
  ElemUKeysetPre(base, key);
}

/**
 * ElemU when base is an Object
 */
template <KeyType keyType>
inline tv_lval ElemUObject(tv_lval base, key_type<keyType> key) {
  auto obj = val(base).pobj;
  failOnNonCollectionObjArrayAccess(obj);
  auto const scratchKey = initScratchKey(key);
  return collections::atLval(obj, &scratchKey);
}

/*
 * Intermediate Elem operation for an unsetting member instruction.
 */
template <KeyType keyType = KeyType::Any>
tv_lval ElemU(tv_lval base, key_type<keyType> key) {
  assertx(tvIsPlausible(*base));

  // ElemU helpers hand out lvals to immutable_null_base in cases where we know
  // it won't be updated. Confirm that we never do an illegal update on it.
  assertx(type(immutable_null_base) == KindOfNull);

  if (tvIsArrayLike(base) && !base.val().parr->isVanilla()) {
    return ElemUBespoke<keyType>(base, key);
  }

  switch (type(base)) {
    case KindOfUninit:
    case KindOfNull:
    case KindOfBoolean:
    case KindOfInt64:
    case KindOfDouble:
    case KindOfResource:
    case KindOfEnumClassLabel:
      // Unset on scalar base never modifies the base, but the const_cast is
      // necessary to placate the type system.
      return const_cast<TypedValue*>(&immutable_uninit_base);
    case KindOfClass:
    case KindOfLazyClass:
      raise_error(Strings::OP_NOT_SUPPORTED_CLASS);
      return nullptr;
    case KindOfRFunc:
      raise_error(Strings::RFUNC_NOT_SUPPORTED);
      return nullptr;
    case KindOfFunc:
      raise_error(Strings::OP_NOT_SUPPORTED_FUNC);
      return nullptr;
    case KindOfPersistentString:
    case KindOfString:
      raise_error(Strings::OP_NOT_SUPPORTED_STRING);
      return nullptr;
    case KindOfClsMeth:
      raise_error(Strings::CLS_METH_NOT_SUPPORTED);
    case KindOfRClsMeth:
      raise_error(Strings::RCLS_METH_NOT_SUPPORTED);
      return nullptr;
    case KindOfPersistentKeyset:
    case KindOfKeyset:
      return ElemUKeyset<keyType>(base, key);
    case KindOfPersistentDict:
    case KindOfDict:
      return ElemUDict<keyType>(base, key);
    case KindOfPersistentVec:
    case KindOfVec:
      return ElemUVec<keyType>(base, key);
    case KindOfObject:
      return ElemUObject<keyType>(base, key);
  }
  unknownBaseType(type(base));
}

/**
 * NewElem when base is Null
 */
[[noreturn]]
inline tv_lval NewElemEmptyish(tv_lval base) {
  detail::raiseFalseyPromotion(base);
  not_reached();
}

/**
 * NewElem when base is an invalid type (number, boolean, string, etc.) and is
 * not falsey. We can use immutable_null_base here because updates will raise.
 */
inline tv_lval NewElemInvalid() {
  raise_warning("Cannot use a scalar value as an array");
  return const_cast<TypedValue*>(&immutable_uninit_base);
}

/**
 * NewElem when base is a Boolean
 */
inline tv_lval NewElemBoolean(tv_lval base) {
  return val(base).num ? NewElemInvalid() : NewElemEmptyish(base);
}

/**
 * NewElem when base is a String
 */
inline tv_lval NewElemString(tv_lval base) {
  return val(base).pstr->size() ? NewElemInvalid() : NewElemEmptyish(base);
}

/**
 * NewElem when base is an Object
 */
[[noreturn]]
inline tv_lval NewElemObject(tv_lval base) {
  failOnNonCollectionObjArrayAccess(val(base).pobj);
  throw_cannot_use_newelem_for_lval_read_col();
  not_reached();
}

/**
 * $result = ($base[] = ...);
 */
inline tv_lval NewElem(tv_lval base) {
  assertx(tvIsPlausible(base.tv()));

  switch (base.type()) {
    case KindOfUninit:
    case KindOfNull:
      return NewElemEmptyish(base);
    case KindOfBoolean:
      return NewElemBoolean(base);
    case KindOfInt64:
    case KindOfDouble:
    case KindOfResource:
    case KindOfRFunc:
    case KindOfFunc:
    case KindOfRClsMeth:
    case KindOfClass:
    case KindOfLazyClass:
    case KindOfEnumClassLabel:
      return NewElemInvalid();
    case KindOfPersistentString:
    case KindOfString:
      return NewElemString(base);
    case KindOfPersistentVec:
    case KindOfVec:
    case KindOfPersistentDict:
    case KindOfDict:
    case KindOfPersistentKeyset:
    case KindOfKeyset:
      throw_cannot_use_newelem_for_lval_read(val(base).parr);
    case KindOfObject:
      return NewElemObject(base);
    case KindOfClsMeth:
      throw_cannot_use_newelem_for_lval_read_clsmeth();
  }
  unknownBaseType(type(base));
}

/**
 * SetElem when base is Null
 */
[[noreturn]]
inline void SetElemEmptyish(tv_lval base) {
  detail::raiseFalseyPromotion(base);
  not_reached();
}

/**
 * SetElem when base is an Int64, Double, Resource, Func, or Class.
 */
template <bool setResult>
inline void SetElemScalar(TypedValue* value) {
  raise_warning(Strings::CANNOT_USE_SCALAR_AS_ARRAY);
  if (!setResult) {
    throw InvalidSetMException(make_tv<KindOfNull>());
  }
  tvDecRefGen(value);
  tvWriteNull(*value);
}

/**
 * SetElem when base is a Boolean
 */
template <bool setResult>
inline void SetElemBoolean(tv_lval base, TypedValue* value) {
  return val(base).num ? SetElemScalar<setResult>(value)
                       : SetElemEmptyish(base);
}

/**
 * Convert a key to integer for SetElem
 */
template<KeyType keyType>
inline int64_t castKeyToInt(key_type<keyType> key) {
  return tvToInt(initScratchKey(key));
}

template<>
inline int64_t castKeyToInt<KeyType::Int>(int64_t key) {
  return key;
}

/**
 * SetElem when base is a String
 */
template <bool setResult, KeyType keyType>
inline StringData* SetElemString(tv_lval base, key_type<keyType> key,
                                 TypedValue* value) {
  auto const baseLen = val(base).pstr->size();
  if (baseLen == 0) {
    SetElemEmptyish(base);
    return nullptr;
  }

  // Convert key to string offset.
  auto const x = castKeyToInt<keyType>(key);
  if (UNLIKELY(x < 0 || x >= StringData::MaxSize)) {
    // Can't use PRId64 here because of order of inclusion issues
    raise_warning("Illegal string offset: %lld", (long long)x);
    if (!setResult) {
      throw InvalidSetMException(make_tv<KindOfNull>());
    }
    tvDecRefGen(value);
    tvWriteNull(*value);
    return nullptr;
  }

  // Compute how long the resulting string will be. Type needs
  // to agree with x.
  int64_t slen;
  if (x >= baseLen) {
    slen = x + 1;
  } else {
    slen = baseLen;
  }

  // Extract the first character of (string)value.
  char y;
  {
    StringData* valStr;
    if (LIKELY(isStringType(value->m_type))) {
      valStr = value->m_data.pstr;
      valStr->incRefCount();
    } else {
      valStr = tvCastToStringData(*value);
    }

    y = valStr->data()[0];
    decRefStr(valStr);
  }

  // Create and save the result.
  assertx(x >= 0); // x < 0 is handled above.
  auto const oldp = val(base).pstr;
  if (x < baseLen && !oldp->cowCheck()) {
    // Modify base in place.  This is safe because the LHS owns the
    // only reference.
    USDT(hhvm, hhvm_mut_modifychar, baseLen, x);
    auto const newp = oldp->modifyChar(x, y);
    if (UNLIKELY(newp != oldp)) {
      // only way we can get here is due to a private (count==1) apc string.
      decRefStr(oldp);
      val(base).pstr = newp;
      type(base) = KindOfString;
    }
    // NB: if x < capacity, we could have appended in-place here.
  } else {
    USDT(hhvm, hhvm_cow_modifychar, baseLen, x);
    StringData* sd = StringData::Make(slen);
    char* s = sd->mutableData();
    memcpy(s, oldp->data(), baseLen);
    if (x > baseLen) {
      memset(&s[baseLen], ' ', slen - baseLen - 1);
    }
    s[x] = y;
    sd->setSize(slen);
    decRefStr(oldp);
    val(base).pstr = sd;
    type(base) = KindOfString;
  }

  return makeStaticString(y);
}

/**
 * SetElem when base is an Object
 */
template <KeyType keyType>
inline void SetElemObject(tv_lval base, key_type<keyType> key,
                          TypedValue* value) {
  auto obj = val(base).pobj;
  failOnNonCollectionObjArrayAccess(obj);
  auto const scratchKey = initScratchKey(key);
  collections::set(obj, &scratchKey, value);
}

/*
 * arraySetUpdateBase is used by SetElem{Array,Vec,Dict} to do the necessary
 * bookkeeping after mutating an array.
 */
ALWAYS_INLINE
void arraySetUpdateBase(ArrayData* newData, tv_lval base) {
  assertx(newData->hasExactlyOneRef());
  assertx(isArrayLikeType(type(base)));
  type(base) = dt_with_rc(type(base));
  val(base).parr = newData;
  assertx(type(base) == newData->toDataType());
  assertx(tvIsPlausible(*base));
}

/**
 * SetElem when base is a Vec
 */
inline ArrayData* SetElemVecPre(ArrayData* a, int64_t key, TypedValue* value) {
  tvIncRefGen(*value);
  return VanillaVec::SetIntMove(a, key, *value);
}

inline ArrayData*
SetElemVecPre(ArrayData* a, StringData* key, TypedValue* /*value*/) {
  throwInvalidArrayKeyException(key, a);
}

inline ArrayData*
SetElemVecPre(ArrayData* a, TypedValue key, TypedValue* value) {
  if (tvIsInt(key))    return SetElemVecPre(a, key.m_data.num, value);
  if (tvIsString(key)) return SetElemVecPre(a, key.m_data.pstr, value);
  throwInvalidArrayKeyException(&key, a);
}

template <KeyType keyType>
inline void SetElemVec(tv_lval base, key_type<keyType> key, TypedValue* value) {
  assertx(tvIsVec(base));
  assertx(tvIsPlausible(*base));
  ArrayData* a = val(base).parr;
  auto const newData = SetElemVecPre(a, key, value);
  arraySetUpdateBase(newData, base);
}

/**
 * SetElem when base is a Dict
 */
inline ArrayData* SetElemDictPre(ArrayData* a, int64_t key, TypedValue* value) {
  tvIncRefGen(*value);
  return VanillaDict::SetIntMove(a, key, *value);
}

inline ArrayData*
SetElemDictPre(ArrayData* a, StringData* key, TypedValue* value) {
  tvIncRefGen(*value);
  return VanillaDict::SetStrMove(a, key, *value);
}

inline ArrayData*
SetElemDictPre(ArrayData* a, TypedValue key, TypedValue* value) {
  if (tvIsInt(key))    return SetElemDictPre(a, key.m_data.num, value);
  if (tvIsString(key)) return SetElemDictPre(a, key.m_data.pstr, value);
  throwInvalidArrayKeyException(&key, a);
}

template <KeyType keyType>
inline void SetElemDict(tv_lval base, key_type<keyType> key,
                        TypedValue* value) {
  assertx(tvIsDict(base));
  assertx(tvIsPlausible(*base));

  ArrayData* a = val(base).parr;
  auto const newData = SetElemDictPre(a, key, value);
  arraySetUpdateBase(newData, base);
}

/**
 * SetElem when base is a bespoke vec or dict
 */
inline ArrayData* SetElemBespokePre(
    ArrayData* a, int64_t key, TypedValue* value) {
  tvIncRefGen(*value);
  return BespokeArray::SetIntMove(a, key, *value);
}

inline ArrayData* SetElemBespokePre(
    ArrayData* a, StringData* key, TypedValue* value) {
  tvIncRefGen(*value);
  return BespokeArray::SetStrMove(a, key, *value);
}

inline ArrayData* SetElemBespokePre(
    ArrayData* a, TypedValue key, TypedValue* value) {
  if (tvIsInt(key))    return SetElemBespokePre(a, key.m_data.num, value);
  if (tvIsString(key)) return SetElemBespokePre(a, key.m_data.pstr, value);
  throwInvalidArrayKeyException(&key, a);
}

template <KeyType keyType>
inline void SetElemBespoke(
    tv_lval base, key_type<keyType> key, TypedValue* value) {
  assertx(tvIsArrayLike(base));
  assertx(tvIsPlausible(*base));

  auto const oldArr = base.val().parr;
  assertx(!oldArr->isVanilla());
  auto const result = SetElemBespokePre(oldArr, key, value);

  arraySetUpdateBase(result, base);
}

/**
 * SetElem() leaves the result in 'value', rather than returning it as in
 * SetOpElem(), because doing so avoids a dup operation that SetOpElem() can't
 * get around.
 */
template <bool setResult, KeyType keyType>
NEVER_INLINE
StringData* SetElemSlow(tv_lval base, key_type<keyType> key,
                        TypedValue* value) {
  assertx(tvIsPlausible(*base));

  switch (type(base)) {
    case KindOfUninit:
    case KindOfNull:
      SetElemEmptyish(base);
      return nullptr;
    case KindOfBoolean:
      SetElemBoolean<setResult>(base, value);
      return nullptr;
    case KindOfInt64:
    case KindOfDouble:
    case KindOfResource:
    case KindOfRFunc:
    case KindOfFunc:
    case KindOfRClsMeth:
    case KindOfClass:
    case KindOfLazyClass:
    case KindOfEnumClassLabel:
      SetElemScalar<setResult>(value);
      return nullptr;
    case KindOfPersistentString:
    case KindOfString:
      return SetElemString<setResult, keyType>(base, key, value);
    case KindOfPersistentKeyset:
    case KindOfKeyset:
      throwInvalidKeysetOperation();

    // Handled in SetElem
    case KindOfPersistentVec:
    case KindOfVec:
    case KindOfPersistentDict:
    case KindOfDict:
      always_assert(false);

    case KindOfObject:
      SetElemObject<keyType>(base, key, value);
      return nullptr;
    case KindOfClsMeth:
      SetElemScalar<setResult>(value);
      return nullptr;
  }
  unknownBaseType(type(base));
}

/**
 * Fast path for SetElem assuming base is an Array
 */
template <bool setResult, KeyType keyType = KeyType::Any>
inline StringData* SetElem(tv_lval base, key_type<keyType> key,
                           TypedValue* value) {
  assertx(tvIsPlausible(*base));

  if (LIKELY(tvIsVec(base))) {
    base.val().parr->isVanilla() ? SetElemVec<keyType>(base, key, value)
                                 : SetElemBespoke<keyType>(base, key, value);
    return nullptr;
  }
  if (LIKELY(tvIsDict(base))) {
    base.val().parr->isVanilla() ? SetElemDict<keyType>(base, key, value)
                                 : SetElemBespoke<keyType>(base, key, value);
    return nullptr;
  }
  return SetElemSlow<setResult, keyType>(base, key, value);
}

template<bool reverse>
void SetRange(
  tv_lval base, int64_t offset, TypedValue src, int64_t count, int64_t size
);

/**
 * SetNewElem when base is Null
 */
[[noreturn]]
inline void SetNewElemEmptyish(tv_lval base) {
  detail::raiseFalseyPromotion(base);
  not_reached();
}

/**
 * SetNewElem when base is Int64, Double, Resource, Func or Class
 */
template <bool setResult>
inline void SetNewElemScalar(TypedValue* value) {
  raise_warning(Strings::CANNOT_USE_SCALAR_AS_ARRAY);
  if (!setResult) {
    throw InvalidSetMException(make_tv<KindOfNull>());
  }
  tvDecRefGen(value);
  tvWriteNull(*value);
}

/**
 * SetNewElem when base is a Boolean
 */
template <bool setResult>
inline void SetNewElemBoolean(tv_lval base, TypedValue* value) {
  return val(base).num ? SetNewElemScalar<setResult>(value)
                       : SetNewElemEmptyish(base);
}

/**
 * SetNewElem when base is a String
 */
[[noreturn]]
inline void SetNewElemString(tv_lval base) {
  if (!val(base).pstr->size()) SetNewElemEmptyish(base);
  raise_error("[] operator not supported for strings");
}

/**
 * SetNewElem when base is a bespoke array-like
 */
inline void SetNewElemBespoke(tv_lval base, TypedValue* value) {
  assertx(tvIsArrayLike(base));
  assertx(tvIsPlausible(*base));
  auto const oldArr = base.val().parr;
  auto const result = BespokeArray::AppendMove(oldArr, *value);
  // An append to a dict whose next integer key has overflowed raises a
  // warning and stores nothing, handing back the array untouched -- which may
  // be shared, or static.  Nothing changed, so the base keeps what it had;
  // the same is true of a keyset append below.
  if (oldArr != result) {
    arraySetUpdateBase(result, base);
    assertx(tvIsPlausible(*base));
  }
}

/**
 * SetNewElem when base is a Vec
 */
inline void SetNewElemVec(tv_lval base, TypedValue* value) {
  assertx(tvIsVec(base));
  assertx(tvIsPlausible(*base));
  auto a = val(base).parr;
  auto a2 = VanillaVec::AppendMove(a, *value);
  arraySetUpdateBase(a2, base);
}

/**
 * SetNewElem when base is a Dict
 */
inline void SetNewElemDict(tv_lval base, TypedValue* value) {
  assertx(tvIsDict(base));
  assertx(tvIsPlausible(*base));
  auto a = val(base).parr;
  auto a2 = VanillaDict::AppendMove(a, *value);
  // An append to a dict whose next integer key has overflowed raises a
  // warning and stores nothing, handing back the array untouched -- which may
  // be shared, or static.  Nothing changed, so the base keeps what it had;
  // the same is true of a keyset append below.
  if (a2 != a) arraySetUpdateBase(a2, base);
}

/**
 * SetNewElem when base is a Keyset
 */
inline void SetNewElemKeyset(tv_lval base, TypedValue* value) {
  assertx(tvIsKeyset(base));
  assertx(tvIsPlausible(*base));
  auto a = val(base).parr;
  auto a2 = VanillaKeyset::AppendMove(a, *value);
  if (a2 != a) {
    type(base) = KindOfKeyset;
    val(base).parr = a2;
    assertx(tvIsPlausible(*base));
  }
}

/**
 * SetNewElem when base is an Object
 */
inline void SetNewElemObject(tv_lval base, TypedValue* value) {
  auto obj = val(base).pobj;
  failOnNonCollectionObjArrayAccess(obj);
  collections::append(obj, value);
}

/**
 * $base[] = ...
 */
template <bool setResult>
inline void SetNewElem(tv_lval base, TypedValue* value) {
  assertx(tvIsPlausible(*base));

  if (tvIsArrayLike(base) && !base.val().parr->isVanilla()) {
    tvIncRefGen(*value);
    return SetNewElemBespoke(base, value);
  }

  switch (type(base)) {
    case KindOfUninit:
    case KindOfNull:
      return SetNewElemEmptyish(base);
    case KindOfBoolean:
      return SetNewElemBoolean<setResult>(base, value);
    case KindOfInt64:
    case KindOfDouble:
    case KindOfResource:
    case KindOfRFunc:
    case KindOfFunc:
    case KindOfRClsMeth:
    case KindOfClass:
    case KindOfLazyClass:
    case KindOfEnumClassLabel:
      return SetNewElemScalar<setResult>(value);
    case KindOfPersistentString:
    case KindOfString:
      return SetNewElemString(base);
    case KindOfPersistentVec:
    case KindOfVec:
      tvIncRefGen(*value);
      return SetNewElemVec(base, value);
    case KindOfPersistentDict:
    case KindOfDict:
      tvIncRefGen(*value);
      return SetNewElemDict(base, value);
    case KindOfPersistentKeyset:
    case KindOfKeyset:
      tvIncRefGen(*value);
      return SetNewElemKeyset(base, value);
    case KindOfObject:
      return SetNewElemObject(base, value);
    case KindOfClsMeth:
      return SetNewElemScalar<setResult>(value);
  }
  unknownBaseType(type(base));
}

/**
 * SetOpElem when base is Null
 */
[[noreturn]]
inline TypedValue SetOpElemEmptyish(tv_lval base) {
  detail::raiseFalseyPromotion(base);
  not_reached();
}

/**
 * TypedValue when base is Int64, Double, Resource, Func, or Class
 */
inline TypedValue SetOpElemScalar() {
  raise_warning(Strings::CANNOT_USE_SCALAR_AS_ARRAY);
  return make_tv<KindOfNull>();
}

/*
 * Perform the operation `update` on a given BespokeArray safely, and with
 * minimum vanilla escalation. To do so, we call Elem, and store the value
 * in a tmp TypedValue, and update it; if its type is unchanged, we write
 * the value back directly, but if the type is changed, we do a full set.
 */
template <typename Update>
TypedValue UpdateBespoke(tv_lval base, TypedValue key, Update update) {
  auto const val = ElemDBespoke<KeyType::Any>(base, key);
  TypedValue tmp = *val;
  if (val.type() == KindOfString) {
    val.val().pstr = staticEmptyString();
  } else {
    tvIncRefGen(tmp);
  }
  auto const result = update(&tmp);
  if (val.type() == tmp.type()) {
    val.val() = tmp.val();
    if (val.type() != KindOfString) {
      tvDecRefGen(tmp);
    }
  } else {
    SetElemBespoke<KeyType::Any>(base, key, &tmp);
  }
  return result;
}

/**
 * $result = ($base[$x] <op>= $y)
 */
inline TypedValue SetOpElem(SetOpOp op, tv_lval base,
                            TypedValue key, TypedValue* rhs) {
  assertx(tvIsPlausible(*base));

  if (tvIsArrayLike(base) && !base.val().parr->isVanilla()) {
    if (base.val().parr->isKeysetType()) throwInvalidKeysetOperation();
    return UpdateBespoke(base, key, [&](auto lval) {
      setopBody(lval, op, rhs);
      return *lval;
    });
  }

  auto const handleVec = [&] {
    auto const result = ElemDVec<KeyType::Any>(base, key);
    setopBody(tvAssertPlausible(result), op, rhs);
    return *result;
  };

  switch (type(base)) {
    case KindOfUninit:
    case KindOfNull:
      return SetOpElemEmptyish(base);

    case KindOfBoolean:
      return val(base).num ? SetOpElemScalar() : SetOpElemEmptyish(base);

    case KindOfInt64:
    case KindOfDouble:
    case KindOfResource:
    case KindOfRFunc:
    case KindOfFunc:
    case KindOfRClsMeth:
    case KindOfClass:
    case KindOfLazyClass:
    case KindOfEnumClassLabel:
      return SetOpElemScalar();

    case KindOfPersistentString:
    case KindOfString:
      if (val(base).pstr->size() != 0) {
        raise_error("Cannot use assign-op operators with overloaded "
          "objects nor string offsets");
      }
      return SetOpElemEmptyish(base);

    case KindOfPersistentKeyset:
    case KindOfKeyset:
      throwInvalidKeysetOperation();

    case KindOfPersistentDict:
    case KindOfDict: {
      auto const result = ElemDDict<KeyType::Any>(base, key);
      setopBody(tvAssertPlausible(result), op, rhs);
      return *result;
    }

    case KindOfPersistentVec:
    case KindOfVec:
      return handleVec();

    case KindOfObject: {
      auto obj = val(base).pobj;
      failOnNonCollectionObjArrayAccess(obj);
      auto const result = collections::atRw(obj, &key);
      setopBody(result, op, rhs);
      return *result;
    }

    case KindOfClsMeth:
      return SetOpElemScalar();
  }
  unknownBaseType(type(base));
}

[[noreturn]]
inline TypedValue SetOpNewElemEmptyish(tv_lval base) {
  detail::raiseFalseyPromotion(base);
  not_reached();
}
inline TypedValue SetOpNewElemScalar() {
  raise_warning(Strings::CANNOT_USE_SCALAR_AS_ARRAY);
  return make_tv<KindOfNull>();
}
inline TypedValue SetOpNewElem(SetOpOp op, tv_lval base, TypedValue* rhs) {
  assertx(tvIsPlausible(*base));

  switch (type(base)) {
    case KindOfUninit:
    case KindOfNull:
      return SetOpNewElemEmptyish(base);

    case KindOfBoolean:
      return val(base).num ? SetOpNewElemScalar() : SetOpNewElemEmptyish(base);

    case KindOfInt64:
    case KindOfDouble:
    case KindOfResource:
    case KindOfRFunc:
    case KindOfFunc:
    case KindOfRClsMeth:
    case KindOfClass:
    case KindOfLazyClass:
    case KindOfEnumClassLabel:
      return SetOpNewElemScalar();

    case KindOfPersistentString:
    case KindOfString:
      if (val(base).pstr->size() != 0) {
        raise_error("[] operator not supported for strings");
      }
      return SetOpNewElemEmptyish(base);

    case KindOfPersistentVec:
    case KindOfVec:
    case KindOfPersistentDict:
    case KindOfDict:
    case KindOfPersistentKeyset:
    case KindOfKeyset:
      throw_cannot_use_newelem_for_lval_read(val(base).parr);

    case KindOfObject: {
      failOnNonCollectionObjArrayAccess(val(base).pobj);
      throw_cannot_use_newelem_for_lval_read_col();
    }

    case KindOfClsMeth:
      throw_cannot_use_newelem_for_lval_read_clsmeth();
  }
  unknownBaseType(type(base));
}

NEVER_INLINE TypedValue incDecBodySlow(IncDecOp op, tv_lval fr);

inline TypedValue IncDecBody(IncDecOp op, tv_lval fr) {
  assertx(tvIsPlausible(*fr));

  if (UNLIKELY(!isIntType(type(fr)))) {
    return incDecBodySlow(op, fr);
  }

  // fast cases, assuming integers overflow to ints. Because int64_t overflow is
  // undefined behavior reinterpret_cast<uint64_t&> first
  switch (op) {
  case IncDecOp::PreInc:
    ++reinterpret_cast<uint64_t&>(val(fr).num);
    return *fr;
  case IncDecOp::PostInc: {
    auto const tmp = *fr;
    ++reinterpret_cast<uint64_t&>(val(fr).num);
    return tmp;
  }
  case IncDecOp::PreDec:
    --reinterpret_cast<uint64_t&>(val(fr).num);
    return *fr;
  case IncDecOp::PostDec: {
    auto const tmp = *fr;
    --reinterpret_cast<uint64_t&>(val(fr).num);
    return tmp;
  }
  default:
    return incDecBodySlow(op, fr);
  }
}

[[noreturn]]
inline TypedValue IncDecElemEmptyish(tv_lval base) {
  detail::raiseFalseyPromotion(base);
  not_reached();
}

inline TypedValue IncDecElemScalar() {
  raise_warning(Strings::CANNOT_USE_SCALAR_AS_ARRAY);
  return make_tv<KindOfNull>();
}

inline TypedValue IncDecElem(IncDecOp op, tv_lval base, TypedValue key) {
  assertx(tvIsPlausible(*base));

  if (tvIsArrayLike(base) && !base.val().parr->isVanilla()) {
    return UpdateBespoke(base, key, [&](auto lval) {
      return IncDecBody(op, lval);
    });
  }

  switch (type(base)) {
    case KindOfUninit:
    case KindOfNull:
      return IncDecElemEmptyish(base);

    case KindOfBoolean:
      return val(base).num ? IncDecElemScalar() : IncDecElemEmptyish(base);

    case KindOfInt64:
    case KindOfDouble:
    case KindOfResource:
    case KindOfRFunc:
    case KindOfFunc:
    case KindOfRClsMeth:
    case KindOfClass:
    case KindOfLazyClass:
    case KindOfEnumClassLabel:
      return IncDecElemScalar();

    case KindOfPersistentString:
    case KindOfString:
      if (val(base).pstr->size() != 0) {
        raise_error("Cannot increment/decrement overloaded objects "
          "nor string offsets");
      }
      return IncDecElemEmptyish(base);

    case KindOfPersistentKeyset:
    case KindOfKeyset:
      throwInvalidKeysetOperation();

    case KindOfPersistentDict:
    case KindOfDict:
      return IncDecBody(op, ElemDDict<KeyType::Any>(base, key));

    case KindOfPersistentVec:
    case KindOfVec:
      return IncDecBody(op, ElemDVec<KeyType::Any>(base, key));

    case KindOfObject: {
      tv_lval result;
      auto localTvRef = make_tv<KindOfUninit>();
      auto obj = val(base).pobj;
      failOnNonCollectionObjArrayAccess(obj);
      result = collections::atRw(obj, &key);
      assertx(tvIsPlausible(*result));

      auto const dest = IncDecBody(op, result);
      tvDecRefGen(localTvRef);
      return dest;
    }

    case KindOfClsMeth:
      return IncDecElemScalar();
  }
  unknownBaseType(type(base));
}

[[noreturn]]
inline TypedValue IncDecNewElemEmptyish(tv_lval base) {
  detail::raiseFalseyPromotion(base);
  not_reached();
}

inline TypedValue IncDecNewElemScalar() {
  raise_warning(Strings::CANNOT_USE_SCALAR_AS_ARRAY);
  return make_tv<KindOfNull>();
}

inline TypedValue IncDecNewElem(IncDecOp op, tv_lval base) {
  assertx(tvIsPlausible(*base));

  switch (type(base)) {
    case KindOfUninit:
    case KindOfNull:
      return IncDecNewElemEmptyish(base);

    case KindOfBoolean:
      return val(base).num ? IncDecNewElemScalar()
                           : IncDecNewElemEmptyish(base);

    case KindOfInt64:
    case KindOfDouble:
    case KindOfResource:
    case KindOfRFunc:
    case KindOfFunc:
    case KindOfRClsMeth:
    case KindOfClass:
    case KindOfLazyClass:
    case KindOfEnumClassLabel:
      return IncDecNewElemScalar();

    case KindOfPersistentString:
    case KindOfString:
      if (val(base).pstr->size() != 0) {
        raise_error("[] operator not supported for strings");
      }
      return IncDecNewElemEmptyish(base);

    case KindOfPersistentVec:
    case KindOfVec:
    case KindOfPersistentDict:
    case KindOfDict:
    case KindOfPersistentKeyset:
    case KindOfKeyset:
      throw_cannot_use_newelem_for_lval_read(val(base).parr);

    case KindOfObject: {
      failOnNonCollectionObjArrayAccess(val(base).pobj);
      throw_cannot_use_newelem_for_lval_read_col();
    }

    case KindOfClsMeth:
      throw_cannot_use_newelem_for_lval_read_clsmeth();
  }
  unknownBaseType(type(base));
}

/**
 * UnsetElem when base is a Vec
 */

inline ArrayData* UnsetElemVecPre(ArrayData* a, int64_t key) {
  return VanillaVec::RemoveIntMove(a, key);
}

inline ArrayData*
UnsetElemVecPre(ArrayData* a, StringData* /*key*/) {
  /* Never contains strings, so a no-op. */
  return a;
}

inline ArrayData* UnsetElemVecPre(ArrayData* a, TypedValue key) {
  auto const dt = key.m_type;
  if (LIKELY(isIntType(dt))) return UnsetElemVecPre(a, key.m_data.num);
  if (isStringType(dt))      return UnsetElemVecPre(a, key.m_data.pstr);
  throwInvalidArrayKeyException(&key, a);
}

template <KeyType keyType>
inline void UnsetElemVec(tv_lval base, key_type<keyType> key) {
  assertx(tvIsVec(base));
  assertx(tvIsPlausible(*base));

  ArrayData* a = val(base).parr;
  ArrayData* a2 = UnsetElemVecPre(a, key);

  type(base) = dt_with_rc(type(base));
  val(base).parr = a2;
}

/**
 * UnsetElem when base is a Dict
 */

inline ArrayData* UnsetElemDictPre(ArrayData* a, int64_t key) {
  return VanillaDict::RemoveIntMove(a, key);
}

inline ArrayData* UnsetElemDictPre(ArrayData* a, StringData* key) {
  return VanillaDict::RemoveStrMove(a, key);
}

inline ArrayData* UnsetElemDictPre(ArrayData* a, TypedValue key) {
  auto const dt = key.m_type;
  if (isIntType(dt))    return UnsetElemDictPre(a, key.m_data.num);
  if (isStringType(dt)) return UnsetElemDictPre(a, key.m_data.pstr);
  throwInvalidArrayKeyException(&key, a);
}

template <KeyType keyType>
inline void UnsetElemDict(tv_lval base, key_type<keyType> key) {
  assertx(tvIsDict(base));
  assertx(tvIsPlausible(*base));

  ArrayData* a = val(base).parr;
  ArrayData* a2 = UnsetElemDictPre(a, key);

  type(base) = dt_with_rc(type(base));
  val(base).parr = a2;
}

/**
 * UnsetElem when base is a Keyset
 */

inline ArrayData* UnsetElemKeysetPre(ArrayData* a, int64_t key) {
  return VanillaKeyset::RemoveIntMove(a, key);
}

inline ArrayData* UnsetElemKeysetPre(ArrayData* a, StringData* key) {
  return VanillaKeyset::RemoveStrMove(a, key);
}

inline ArrayData* UnsetElemKeysetPre(ArrayData* a, TypedValue key) {
  auto const dt = key.m_type;
  if (isIntType(dt))    return UnsetElemKeysetPre(a, key.m_data.num);
  if (isStringType(dt)) return UnsetElemKeysetPre(a, key.m_data.pstr);
  throwInvalidArrayKeyException(&key, a);
}

template <KeyType keyType>
inline void UnsetElemKeyset(tv_lval base, key_type<keyType> key) {
  assertx(tvIsKeyset(base));
  assertx(tvIsPlausible(*base));

  ArrayData* a = val(base).parr;
  ArrayData* a2 = UnsetElemKeysetPre(a, key);

  type(base) = KindOfKeyset;
  val(base).parr = a2;
}

/**
 * UnsetElem when base is a bespoke Hack array
 */
inline ArrayData* UnsetElemBespokePre(ArrayData* a, int64_t key) {
  return BespokeArray::RemoveIntMove(a, key);
}

inline ArrayData* UnsetElemBespokePre(ArrayData* a, StringData* key) {
  return BespokeArray::RemoveStrMove(a, key);
}

inline ArrayData* UnsetElemBespokePre(ArrayData* a, TypedValue key) {
  if (tvIsInt(key))    return UnsetElemBespokePre(a, key.m_data.num);
  if (tvIsString(key)) return UnsetElemBespokePre(a, key.m_data.pstr);
  throwInvalidArrayKeyException(&key, a);
}

template <KeyType keyType>
inline void UnsetElemBespoke(tv_lval base, key_type<keyType> key) {
  assertx(tvIsArrayLike(base));
  assertx(tvIsPlausible(*base));

  auto const oldArr = base.val().parr;
  assertx(!oldArr->isVanilla());
  auto const result = UnsetElemBespokePre(oldArr, key);

  if (result != oldArr) {
    type(base) = dt_with_rc(type(base));
    val(base).parr = result;
  }
  assertx(tvIsPlausible(*base));
}

/**
 * unset($base[$member])
 */
template <KeyType keyType>
NEVER_INLINE
void UnsetElemSlow(tv_lval base, key_type<keyType> key) {
  assertx(tvIsPlausible(*base));

  switch (type(base)) {
    case KindOfUninit:
    case KindOfNull:
    case KindOfBoolean:
    case KindOfInt64:
    case KindOfDouble:
    case KindOfResource:
    case KindOfEnumClassLabel:
      return; // Do nothing.

    case KindOfRFunc:
      raise_error("Cannot unset a reified function");
      return;
    case KindOfFunc:
      raise_error("Cannot unset a func");
      return;
    case KindOfRClsMeth:
      raise_error("Cannot unset a reified class method pointer");
      return;
    case KindOfClass:
    case KindOfLazyClass:
      raise_error("Cannot unset a class");
      return;

    case KindOfPersistentString:
    case KindOfString:
      raise_error(Strings::CANT_UNSET_STRING);
      return;

    // Handled in UnsetElem
    case KindOfPersistentVec:
    case KindOfVec:
    case KindOfPersistentDict:
    case KindOfDict:
    case KindOfPersistentKeyset:
    case KindOfKeyset:
      always_assert(false);

    case KindOfObject: {
      auto obj = val(base).pobj;
      failOnNonCollectionObjArrayAccess(obj);
      auto const& scratchKey = initScratchKey(key);
      collections::unset(obj, &scratchKey);
      return;
    }

    case KindOfClsMeth:
      raise_error("Cannot unset a class method pointer");
  }
  unknownBaseType(type(base));
}

/**
 * Fast path for UnsetElem assuming base is an Array
 */
template <KeyType keyType = KeyType::Any>
inline void UnsetElem(tv_lval base, key_type<keyType> key) {
  assertx(tvIsPlausible(*base));

  if (tvIsArrayLike(base)) {
    auto const ad = base.val().parr;
    if (!ad->isVanilla()) return UnsetElemBespoke<keyType>(base, key);
    if (tvIsVec(base))    return UnsetElemVec<keyType>(base, key);
    if (tvIsDict(base))   return UnsetElemDict<keyType>(base, key);
    return UnsetElemKeyset<keyType>(base, key);
  }
  return UnsetElemSlow<keyType>(base, key);
}

/**
 * IssetElem when base is an Object
 */
template<KeyType keyType>
bool IssetElemObj(ObjectData* instance, key_type<keyType> key) {
  failOnNonCollectionObjArrayAccess(instance);
  auto scratchKey = initScratchKey(key);
  return collections::isset(instance, &scratchKey);
}

/**
 * IssetElem when base is a String
 */
template<KeyType keyType>
bool IssetElemString(const StringData* sd, key_type<keyType> key) {
  auto scratchKey = initScratchKey(key);
  int64_t x;
  if (LIKELY(scratchKey.m_type == KindOfInt64)) {
    x = scratchKey.m_data.num;
  } else {
    TypedValue tv;
    tvDup(scratchKey, tv);
    bool badKey = false;
    if (isStringType(tv.m_type)) {
      const char* str = tv.m_data.pstr->data();
      size_t len = tv.m_data.pstr->size();
      while (len > 0 &&
             (*str == ' ' || *str == '\t' || *str == '\r' || *str == '\n')) {
        ++str;
        --len;
      }
      int64_t n;
      badKey = !is_strictly_integer(str, len, n);
    } else if (isArrayLikeType(tv.m_type) || tv.m_type == KindOfObject ||
               tv.m_type == KindOfResource) {
      badKey = true;
    }
    // Even if badKey == true, we still perform the cast so that we
    // raise the appropriate warnings.
    tvCastToInt64InPlace(&tv);
    if (badKey) {
      return false;
    }
    x = tv.m_data.num;
  }
  return x >= 0 && x < sd->size();
}

/**
 * IssetElem when base is a Vec
 */
template<KeyType keyType>
bool IssetElemVec(ArrayData* a, key_type<keyType> key) {
  assertx(a->isVanillaVec());
  auto const result = ElemVec<MOpMode::None, keyType>(a, key);
  return !tvIsNull(tvAssertPlausible(result));
}

/**
 * IssetElem when base is a Dict
 */
template<KeyType keyType>
bool IssetElemDict(ArrayData* a, key_type<keyType> key) {
  assertx(a->isVanillaDict());
  auto const result = ElemDict<MOpMode::None, keyType>(a, key);
  return !tvIsNull(tvAssertPlausible(result));
}

/**
 * IssetElem when base is a Keyset
 */
template<KeyType keyType>
bool IssetElemKeyset(ArrayData* a, key_type<keyType> key) {
  assertx(a->isVanillaKeyset());
  auto const result = ElemKeyset<MOpMode::None, keyType>(a, key);
  return !tvIsNull(tvAssertPlausible(result));
}

/**
 * IssetElem when base is a bespoke Hack array
 */
template<KeyType keyType>
bool IssetElemBespoke(ArrayData* a, key_type<keyType> key) {
  auto const result = ElemBespoke<MOpMode::None, keyType>(a, key);
  return !tvIsNull(tvAssertPlausible(result));
}

/**
 * IssetElem when base is a ClsMeth
 */
template<KeyType keyType>
bool IssetElemClsMeth(ClsMethDataRef base, key_type<keyType> key) {
  const TypedValue result = ElemClsMethPre<MOpMode::None>(base, key);
  return !tvIsNull(tvAssertPlausible(result));
}

/**
 * isset($base[$key])
 */
template <KeyType keyType>
NEVER_INLINE bool IssetElemSlow(TypedValue base, key_type<keyType> key) {
  assertx(tvIsPlausible(base));

  auto const op = "isset";
  switch (type(base)) {
    case KindOfUninit:
    case KindOfNull:
    case KindOfBoolean:
    case KindOfInt64:
    case KindOfDouble:
    case KindOfResource:
    case KindOfRFunc:
    case KindOfRClsMeth:
    case KindOfFunc:
    case KindOfEnumClassLabel:
      return false;

    case KindOfClass:
      return IssetElemString<keyType>(
        classToStringHelper(val(base).pclass, op), key
      );

    case KindOfLazyClass:
      return IssetElemString<keyType>(
        lazyClassToStringHelper(val(base).plazyclass, op), key
      );

    case KindOfPersistentString:
    case KindOfString:
      return IssetElemString<keyType>(val(base).pstr, key);

    // These types are handled in IssetElem.
    case KindOfPersistentVec:
    case KindOfVec:
    case KindOfPersistentDict:
    case KindOfDict:
    case KindOfPersistentKeyset:
    case KindOfKeyset:
      always_assert(false);

    case KindOfObject:
      return IssetElemObj<keyType>(val(base).pobj, key);

    case KindOfClsMeth:
      return false;
  }
  unknownBaseType(type(base));
}

template <KeyType keyType = KeyType::Any>
bool IssetElem(TypedValue base, key_type<keyType> key) {
  assertx(tvIsPlausible(base));

  if (tvIsArrayLike(base)) {
    auto const ad = val(base).parr;
    if (!ad->isVanilla()) {
      return IssetElemBespoke<keyType>(ad, key);
    } else if (ad->isVanillaVec()) {
      return IssetElemVec<keyType>(ad, key);
    } else if (ad->isVanillaDict()) {
      return IssetElemDict<keyType>(ad, key);
    } else {
      return IssetElemKeyset<keyType>(ad, key);
    }
  }

  return IssetElemSlow<keyType>(base, key);
}

template<MOpMode mode>
inline tv_lval propPreNull(TypedValue& tvRef) {
  tvWriteNull(tvRef);
  if (mode == MOpMode::Warn) {
    raise_notice("Cannot access property on non-object");
  }
  return tv_lval(&tvRef);
}

template<MOpMode mode>
tv_lval propPreStdclass(TypedValue& tvRef) {
  if (mode != MOpMode::Define) return propPreNull<mode>(tvRef);
  detail::raiseEmptyObject();
  not_reached();
}

template<MOpMode mode>
inline tv_lval nullSafeProp(TypedValue& tvRef,
                            MemberLookupContext& ctx,
                            TypedValue base,
                            StringData* key,
                            ReadonlyOp op) {
  switch (base.type()) {
    case KindOfUninit:
    case KindOfNull:
      tvWriteNull(tvRef);
      return &tvRef;
    case KindOfBoolean:
    case KindOfInt64:
    case KindOfDouble:
    case KindOfResource:
    case KindOfPersistentString:
    case KindOfString:
    case KindOfPersistentVec:
    case KindOfVec:
    case KindOfPersistentDict:
    case KindOfDict:
    case KindOfPersistentKeyset:
    case KindOfKeyset:
    case KindOfRFunc:
    case KindOfFunc:
    case KindOfClass:
    case KindOfLazyClass:
    case KindOfClsMeth:
    case KindOfRClsMeth:
    case KindOfEnumClassLabel:
      return propPreNull<mode>(tvRef);
    case KindOfObject:
      return val(base).pobj->prop(&tvRef, ctx, key, op);
  }
  not_reached();
}

/*
 * Generic property access (PropX and PropDX end up here).
 *
 * Returns a pointer to a number of possible places.
 */
template<MOpMode mode, KeyType keyType = KeyType::Any>
inline tv_lval PropObj(TypedValue& tvRef, const MemberLookupContext& ctx,
                       ObjectData* instance, key_type<keyType> key,
                       ReadonlyOp op) {
  auto keySD = prepareKey(key);
  SCOPE_EXIT { releaseKey<keyType>(keySD); };

  // Get property.
  if (mode == MOpMode::Define) {
    return instance->propD(&tvRef, ctx, keySD, op);
  }
  if (mode == MOpMode::None) {
    return instance->prop(&tvRef, ctx, keySD, op);
  }
  if (mode == MOpMode::Warn) {
    return instance->propW(&tvRef, ctx, keySD, op);
  }
  assertx(mode == MOpMode::Unset);
  return instance->propU(&tvRef, ctx, keySD, op);
}

template<MOpMode mode, KeyType keyType = KeyType::Any>
inline tv_lval Prop(TypedValue& tvRef, const MemberLookupContext& ctx,
                    TypedValue base, key_type<keyType> key, ReadonlyOp op) {
  if (LIKELY(type(base) == KindOfObject)) {
    return PropObj<mode,keyType>(tvRef, ctx, val(base).pobj, key, op);
  }

  switch (base.type()) {
    case KindOfUninit:
    case KindOfNull:
      return propPreStdclass<mode>(tvRef);

    case KindOfBoolean:
      return base.val().num ? propPreNull<mode>(tvRef)
                            : propPreStdclass<mode>(tvRef);

    case KindOfInt64:
    case KindOfDouble:
    case KindOfResource:
    case KindOfRFunc:
    case KindOfFunc:
    case KindOfClass:
    case KindOfLazyClass:
    case KindOfEnumClassLabel:
      return propPreNull<mode>(tvRef);

    case KindOfPersistentString:
    case KindOfString:
      return base.val().pstr->size() ? propPreNull<mode>(tvRef)
                                     : propPreStdclass<mode>(tvRef);

    case KindOfPersistentVec:
    case KindOfVec:
    case KindOfPersistentDict:
    case KindOfDict:
    case KindOfPersistentKeyset:
    case KindOfKeyset:
    case KindOfClsMeth:
    case KindOfRClsMeth:
      return propPreNull<mode>(tvRef);

    case KindOfObject:
      always_assert(false);
  }
  unknownBaseType(type(base));
}

template <KeyType kt>
inline bool IssetPropObj(MemberLookupContext& ctx, ObjectData* instance, key_type<kt> key) {
  auto keySD = prepareKey(key);
  SCOPE_EXIT { releaseKey<kt>(keySD); };

  return instance->propIsset(ctx, keySD);
}

template <KeyType kt = KeyType::Any>
bool IssetProp(MemberLookupContext& ctx, TypedValue base, key_type<kt> key) {
  if (LIKELY(type(base) == KindOfObject)) {
    return IssetPropObj<kt>(ctx, val(base).pobj, key);
  }
  return false;
}

[[noreturn]]
inline void SetPropNull() {
  raise_warning("Cannot access property on non-object");
  throw InvalidSetMException(make_tv<KindOfNull>());
}

template <KeyType keyType>
inline void SetPropObj(MemberLookupContext& ctx, ObjectData* instance, key_type<keyType> key,
                       TypedValue val, ReadonlyOp op) {
  StringData* keySD = prepareKey(key);
  SCOPE_EXIT { releaseKey<keyType>(keySD); };

  // Set property.
  instance->setProp(ctx, keySD, val, op);
}

// $base->$key = $val
template <KeyType keyType = KeyType::Any>
inline void SetProp(MemberLookupContext& ctx, TypedValue base, key_type<keyType> key,
                    TypedValue val, ReadonlyOp op) {
  switch (type(base)) {
    case KindOfUninit:
    case KindOfNull:
      return detail::raiseEmptyObject();

    case KindOfBoolean:
      return HPHP::val(base).num ? SetPropNull()
                                 : detail::raiseEmptyObject();

    case KindOfInt64:
    case KindOfDouble:
    case KindOfPersistentVec:
    case KindOfVec:
    case KindOfPersistentDict:
    case KindOfDict:
    case KindOfPersistentKeyset:
    case KindOfKeyset:
    case KindOfResource:
    case KindOfRFunc:
    case KindOfFunc:
    case KindOfClass:
    case KindOfLazyClass:
    case KindOfClsMeth:
    case KindOfRClsMeth:
    case KindOfEnumClassLabel:
      return SetPropNull();

    case KindOfPersistentString:
    case KindOfString:
      return HPHP::val(base).pstr->size() ? SetPropNull()
                                          : detail::raiseEmptyObject();

    case KindOfObject:
      return SetPropObj<keyType>(ctx, HPHP::val(base).pobj, key, val, op);
  }
  unknownBaseType(type(base));
}

inline tv_lval SetOpPropNull(TypedValue& tvRef) {
  raise_warning("Attempt to assign property of non-object");
  tvWriteNull(tvRef);
  return &tvRef;
}

inline tv_lval SetOpPropObj(TypedValue& tvRef, MemberLookupContext& ctx,
                            SetOpOp op, ObjectData* instance,
                            TypedValue key, TypedValue* rhs) {
  StringData* keySD = prepareKey(key);
  SCOPE_EXIT { decRefStr(keySD); };
  return instance->setOpProp(tvRef, ctx, op, keySD, rhs);
}

// $base->$key <op>= $rhs
inline tv_lval SetOpProp(TypedValue& tvRef,
                         MemberLookupContext& ctx, SetOpOp op,
                         TypedValue base, TypedValue key,
                         TypedValue* rhs) {
  switch (type(base)) {
    case KindOfUninit:
    case KindOfNull:
      detail::raiseEmptyObject();
      not_reached();

    case KindOfBoolean:
      if (val(base).num) return SetOpPropNull(tvRef);
      detail::raiseEmptyObject();
      not_reached();

    case KindOfInt64:
    case KindOfDouble:
    case KindOfPersistentVec:
    case KindOfVec:
    case KindOfPersistentDict:
    case KindOfDict:
    case KindOfPersistentKeyset:
    case KindOfKeyset:
    case KindOfResource:
    case KindOfRFunc:
    case KindOfFunc:
    case KindOfClass:
    case KindOfLazyClass:
    case KindOfClsMeth:
    case KindOfRClsMeth:
    case KindOfEnumClassLabel:
      return SetOpPropNull(tvRef);

    case KindOfPersistentString:
    case KindOfString:
      if (val(base).pstr->size()) return SetOpPropNull(tvRef);
      detail::raiseEmptyObject();
      not_reached();

    case KindOfObject:
      return SetOpPropObj(tvRef, ctx, op, val(base).pobj, key, rhs);
  }
  unknownBaseType(type(base));
}

inline TypedValue IncDecPropNull() {
  raise_warning("Attempt to increment/decrement property of non-object");
  return make_tv<KindOfNull>();
}

inline TypedValue IncDecPropObj(MemberLookupContext& ctx,
                          IncDecOp op,
                          ObjectData* base,
                          TypedValue key) {
  auto keySD = prepareKey(key);
  SCOPE_EXIT { decRefStr(keySD); };
  return base->incDecProp(ctx, op, keySD);
}

inline TypedValue IncDecProp(
  MemberLookupContext& ctx,
  IncDecOp op,
  TypedValue base,
  TypedValue key
) {
  switch (type(base)) {
    case KindOfUninit:
    case KindOfNull:
      detail::raiseEmptyObject();
      not_reached();

    case KindOfBoolean:
      if (val(base).num) return IncDecPropNull();
      detail::raiseEmptyObject();
      not_reached();

    case KindOfInt64:
    case KindOfDouble:
    case KindOfPersistentVec:
    case KindOfVec:
    case KindOfPersistentDict:
    case KindOfDict:
    case KindOfPersistentKeyset:
    case KindOfKeyset:
    case KindOfResource:
    case KindOfRFunc:
    case KindOfFunc:
    case KindOfClass:
    case KindOfLazyClass:
    case KindOfClsMeth:
    case KindOfRClsMeth:
    case KindOfEnumClassLabel:
      return IncDecPropNull();

    case KindOfPersistentString:
    case KindOfString:
      if (val(base).pstr->size()) return IncDecPropNull();
      detail::raiseEmptyObject();
      not_reached();

    case KindOfObject:
      return IncDecPropObj(ctx, op, val(base).pobj, key);
  }
  unknownBaseType(type(base));
}

///////////////////////////////////////////////////////////////////////////////
}
