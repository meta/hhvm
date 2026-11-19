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

#ifndef incl_HPHP_JIT_TYPE_INL_H_
#error "type-inl.h should only be included by type.h"
#endif

#include "hphp/runtime/base/bespoke-array.h"
#include "hphp/runtime/base/string-data.h"
#include "hphp/runtime/vm/class.h"
#include "hphp/runtime/vm/class-meth-data-ref.h"

#include "hphp/util/hash.h"

#include <cstring>

namespace HPHP::jit {
///////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////
// Predefined Types

constexpr inline Type::Type(bits_t bits, PtrLocation ptr)
  : m_bits(bits)
  , m_ptr(ptr)
  , m_hasConstVal(false)
  , m_extra(0)
{}

#define IRT(name, ...) \
  constexpr Type T##name{Type::k##name, PtrLocation::Bottom};
#define IRTP(name, ptr) \
  constexpr Type T##name{Type::kPtr, PtrLocation::ptr};
#define IRTL(name, ptr) \
  constexpr Type T##name{Type::kLval, PtrLocation::ptr};
#define IRTM(name, ptr) \
  constexpr Type T##name{Type::kMem, PtrLocation::ptr};
#define IRTX(name, x, bits) \
  constexpr Type T##name{Type::bits, PtrLocation::x};
IRT_PHP
IRT_PHP_UNIONS
IRT_PTRS_LVALS
IRT_SPECIAL
#undef IRT
#undef IRTP
#undef IRTL
#undef IRTM
#undef IRTX

#define IRT(name, ...) \
  constexpr Type T##name{Type::k##name, PtrLocation::Bottom};
IRT_RUNTIME
#undef IRT

// Vanilla types that appear in irgen.
static auto const TVanillaVec     = TVec.narrowToVanilla();
static auto const TVanillaDict    = TDict.narrowToVanilla();
static auto const TVanillaKeyset  = TKeyset.narrowToVanilla();
static auto const TVanillaArrLike = TArrLike.narrowToVanilla();

/*
 * Abbreviated namespace for predefined types.
 *
 * Used for macro codegen for types declared in ir.specification.
 */
namespace TypeNames {
#define IRT(name, ...) UNUSED constexpr Type name = T##name;
#define IRTP(name, ...) IRT(name)
#define IRTL(name, ...) IRT(name)
#define IRTM(name, ...) IRT(name)
#define IRTX(name, ...) IRT(name)
  IR_TYPES
#undef IRT
#undef IRTP
#undef IRTL
#undef IRTM
#undef IRTX
}

namespace type_detail {
///////////////////////////////////////////////////////////////////////////////

/*
 * Widen a constant value if needed.
 */
template<class T>
using needs_promotion = std::integral_constant<
  bool,
  (std::is_integral<T>::value ||
   std::is_same<T,bool>::value ||
   std::is_enum<T>::value)
>;

template<class T>
typename std::enable_if<needs_promotion<T>::value,uint64_t>::type
promote_cns_if_needed(T t) { return static_cast<uint64_t>(t); }

template<class T>
typename std::enable_if<!needs_promotion<T>::value,T>::type
promote_cns_if_needed(T t) { return t; }

/*
 * Return the Type to use for a given C++ value.
 *
 * The only interesting case is int/bool disambiguation.  Enums are treated as
 * ints.
 */
template<class T>
typename std::enable_if<
  std::is_integral<T>::value || std::is_enum<T>::value,
  Type
>::type for_const(T) {
  return std::is_same<T,bool>::value ? TBool : TInt;
}
inline Type for_const(const StringData* sd) {
  assertx(sd->isStatic());
  return TStaticStr;
}
inline Type for_const(const ArrayData* ad) {
  assertx(ad->isStatic());
  if (ad->isVecType()) return TStaticVec;
  if (ad->isDictType()) return TStaticDict;
  if (ad->isKeysetType()) return TStaticKeyset;
  not_reached();
}
inline Type for_const(double)        { return TDbl; }
inline Type for_const(const Func*)   { return TFunc; }
inline Type for_const(const Class*)  { return TCls; }
inline Type for_const(LazyClassData) { return TLazyCls; }
inline Type for_const(ClsMethDataRef) { return TClsMeth; }
inline Type for_const(TCA)           { return TTCA; }
inline Type for_const(void*)         { return TVoidPtr; }
///////////////////////////////////////////////////////////////////////////////
}

///////////////////////////////////////////////////////////////////////////////
// Type.

inline Type::Type()
  : m_bits(kBottom)
  , m_ptr(PtrLocation::Bottom)
  , m_hasConstVal(false)
  , m_extra(0)
{}

inline Type::Type(DataType outer)
  : m_bits(bitsFromDataType(outer))
  , m_ptr(PtrLocation::Bottom)
  , m_hasConstVal(false)
  , m_extra(0)
{}

inline size_t Type::hash() const {
  return hash_int64_pair(
    hash_int64_pair(
      m_bits.hash(),
      static_cast<uint64_t>(m_ptr) ^ m_hasConstVal
    ),
    m_extra
  );
}

///////////////////////////////////////////////////////////////////////////////
// Comparison.

inline bool Type::operator==(Type rhs) const {
  return m_bits == rhs.m_bits &&
    m_ptr == rhs.m_ptr &&
    m_hasConstVal == rhs.m_hasConstVal &&
    m_extra == rhs.m_extra;
}

inline bool Type::operator!=(Type rhs) const {
  return !operator==(rhs);
}

inline bool Type::operator>=(Type rhs) const {
  return rhs <= *this;
}

inline bool Type::operator<(Type rhs) const {
  return *this != rhs && *this <= rhs;
}

inline bool Type::operator>(Type rhs) const {
  return rhs < *this;
}

template<typename... Types>
bool Type::subtypeOfAny(Type t2, Types... ts) const {
  return *this <= t2 || subtypeOfAny(ts...);
}
inline bool Type::subtypeOfAny() const {
  return false;
}

inline bool Type::nonTrivialSubtypeOf(Type t2) const {
  return !(*this <= TBottom) && (*this <= t2);
}

inline bool Type::maybe(Type t2) const {
  return (*this & t2) != TBottom;
}

///////////////////////////////////////////////////////////////////////////////
// Combinators.

inline Type Type::unionAll() {
  return TBottom;
}

template<typename... Types>
inline Type Type::unionAll(Type t, Types... ts) {
  return t | unionAll(ts...);
}

///////////////////////////////////////////////////////////////////////////////
// Is-a methods.

inline bool Type::isUnion() const {
  // This will return true iff more than 1 bit is set in m_bits.
  return m_bits.count() > 1;
}

inline bool Type::isKnownDataType() const {
  assertx(*this <= TCell);

  // Some unions correspond to single KindOfs.
  if (!isUnion()) return true;
  return subtypeOfAny(TStr, TVec, TDict, TKeyset);
}

inline bool Type::needsReg() const {
  return *this <= TCell && !isKnownDataType();
}

inline bool Type::isSimpleType() const {
  return subtypeOfAny(TBool, TInt, TDbl, TNull);
}

inline bool Type::isSingularReferenceType() const {
  return *this != TBottom && subtypeOfAny(TStr, TVec, TDict, TKeyset, TObj, TRes);
}

///////////////////////////////////////////////////////////////////////////////
// Constant type creation.

template<typename T>
Type Type::cns(T val, Type ret) {
  assertx(!ret.m_hasConstVal);
  ret.m_hasConstVal = true;

  static_assert(sizeof(T) <= sizeof(ret),
                "Constant data was larger than supported");
  static_assert(std::is_pod<T>::value,
                "Constant data wasn't a pod");

  const auto toCopy = type_detail::promote_cns_if_needed(val);
  static_assert(sizeof(toCopy) == 8,
                "Unexpected size for toCopy");

  std::memcpy(&ret.m_extra, &toCopy, sizeof(toCopy));
  assertx(ret.checkValid());
  return ret;
}

template<typename T>
Type Type::cns(T val) {
  return cns(val, type_detail::for_const(val));
}

inline Type Type::cns(std::nullptr_t) {
  return TNullptr;
}

inline Type Type::cns(TypedValue tv) {
  if (auto const t = tryCns(tv)) return *t;
  always_assert(false && "Invalid KindOf for constant TypedValue");
}

inline Optional<Type> Type::tryCns(TypedValue tv) {
  assertx(tvIsPlausible(tv));

  if (tv.m_type == KindOfUninit) return TUninit;
  if (tv.m_type == KindOfNull)   return TInitNull;

  auto ret = [&] () -> Optional<Type> {
    switch (tv.m_type) {
      case KindOfUninit:
      case KindOfNull:
        not_reached();

      case KindOfBoolean:
      case KindOfInt64:
      case KindOfDouble:
      case KindOfEnumClassLabel:
        return Type(tv.m_type);

      case KindOfString:
        return type_detail::for_const(tv.m_data.pstr);

      case KindOfVec:
      case KindOfDict:
      case KindOfKeyset:
        return type_detail::for_const(tv.m_data.parr);
      case KindOfLazyClass:
        return type_detail::for_const(tv.m_data.plazyclass);
      // TODO (T29639296)
      case KindOfFunc:
        return type_detail::for_const(tv.m_data.pfunc);
      case KindOfClass:
        return type_detail::for_const(tv.m_data.pclass);
      case KindOfClsMeth:
        return type_detail::for_const(tv.m_data.pclsmeth);

      case KindOfObject:
      case KindOfResource:
      case KindOfRFunc:
      case KindOfRClsMeth:
        return std::nullopt;
    }
    not_reached();
  }();
  if (ret) {
    ret->m_hasConstVal = true;
    ret->m_extra = tv.m_data.num;
  }
  return ret;
}

inline Type Type::dropConstVal() const {
  if (!m_hasConstVal) return *this;

  assertx(!isUnion());
  auto const result = Type(m_bits, ptrLocation());

  if (*this <= TArrLike) {
    return Type(result, ArraySpec(ArrayLayout::FromArray(m_arrVal)));
  }
  return result;
}

///////////////////////////////////////////////////////////////////////////////
// Constant introspection.

inline bool Type::hasConstVal() const {
  return m_hasConstVal;
}

inline bool Type::hasConstVal(Type t) const {
  return hasConstVal() && *this <= t;
}

template<typename T>
bool Type::hasConstVal(T val) const {
  return hasConstVal(cns(val));
}

inline bool Type::admitsSingleVal() const {
  return hasConstVal() || subtypeOfAny(TNullptr, TInitNull, TUninit);
}

inline uint64_t Type::rawVal() const {
  assertx(hasConstVal());
  return m_extra;
}

#define IMPLEMENT_CNS_VAL(TypeName, name, valtype)      \
  inline valtype Type::name##Val() const {              \
    assertx(hasConstVal(TypeName));                      \
    return m_##name##Val;                               \
  }

IMPLEMENT_CNS_VAL(TBool,       bool, bool)
IMPLEMENT_CNS_VAL(TInt,        int,  int64_t)
IMPLEMENT_CNS_VAL(TDbl,        dbl,  double)
IMPLEMENT_CNS_VAL(TStaticStr,  str,  const StringData*)
IMPLEMENT_CNS_VAL(TStaticVec,  vec,  const ArrayData*)
IMPLEMENT_CNS_VAL(TStaticDict, dict, const ArrayData*)
IMPLEMENT_CNS_VAL(TStaticKeyset, keyset, const ArrayData*)
IMPLEMENT_CNS_VAL(TFunc,       func, const HPHP::Func*)
IMPLEMENT_CNS_VAL(TCls,        cls,  const Class*)
IMPLEMENT_CNS_VAL(TLazyCls,    lcls,  LazyClassData)
IMPLEMENT_CNS_VAL(TClsMeth,    clsmeth,  ClsMethDataRef)
IMPLEMENT_CNS_VAL(TTCA,        tca,  jit::TCA)
IMPLEMENT_CNS_VAL(TVoidPtr,    voidPtr, void*)
IMPLEMENT_CNS_VAL(TRDSHandle,  rdsHandle,  rds::Handle)
IMPLEMENT_CNS_VAL(TMem,        ptr, const TypedValue*)

#undef IMPLEMENT_CNS_VAL

inline const StringData* Type::eclVal() const {
  assertx(hasConstVal(TEnumClassLabel));
  return m_strVal;
}

///////////////////////////////////////////////////////////////////////////////
// Specialized type creation.

inline Type Type::Vec(const RepoAuthType::Array* rat) {
  return Type(TVec, ArraySpec(rat));
}

inline Type Type::Dict(const RepoAuthType::Array* rat) {
  return Type(TDict, ArraySpec(rat));
}

inline Type Type::Keyset(const RepoAuthType::Array* rat) {
  return Type(TKeyset, ArraySpec(rat));
}

inline Type Type::StaticVec(const RepoAuthType::Array* rat) {
  return Type(TStaticVec, ArraySpec(rat));
}

inline Type Type::StaticDict(const RepoAuthType::Array* rat) {
  return Type(TStaticDict, ArraySpec(rat));
}

inline Type Type::StaticKeyset(const RepoAuthType::Array* rat) {
  return Type(TStaticKeyset, ArraySpec(rat));
}

inline Type Type::CountedVec(const RepoAuthType::Array* rat) {
  return Type(TCountedVec, ArraySpec(rat));
}

inline Type Type::CountedDict(const RepoAuthType::Array* rat) {
  return Type(TCountedDict, ArraySpec(rat));
}

inline Type Type::CountedKeyset(const RepoAuthType::Array* rat) {
  return Type(TCountedKeyset, ArraySpec(rat));
}

inline Type Type::SubObj(const Class* cls) {
  if (cls->attrs() & AttrNoOverrideRegular) return ExactObj(cls);
  return Type(TObj, ClassSpec(cls, ClassSpec::SubTag{}));
}

inline Type Type::ExactObj(const Class* cls) {
  return Type(TObj, ClassSpec(cls, ClassSpec::ExactTag{}));
}

inline Type Type::SubCls(const Class* cls) {
  if (cls->attrs() & AttrNoOverride) return ExactCls(cls);
  return Type(TCls, ClassSpec(cls, ClassSpec::SubTag{}));
}

inline Type Type::ExactCls(const Class* cls) {
  return Type::cns(cls);
}

inline Type Type::unspecialize() const {
  return Type(m_bits, ptrLocation());
}

///////////////////////////////////////////////////////////////////////////////
// Specialization introspection.

inline bool Type::isSpecialized() const {
  return clsSpec() || arrSpec();
}

inline bool Type::supports(bits_t bits, SpecKind kind) {
  switch (kind) {
    case SpecKind::None:
      return true;
    case SpecKind::Array:
      return (bits & kArrSpecBits) != kBottom;
    case SpecKind::Class:
      return (bits & kClsSpecBits) != kBottom;
  }
  not_reached();
}

inline bool Type::supports(SpecKind kind) const {
  return supports(m_bits, kind);
}

inline ArraySpec Type::arrSpec() const {
  if (!supports(SpecKind::Array)) return ArraySpec::Bottom();

  // Currently, a Type which supports multiple specializations is trivial
  // along all of them.
  if (supports(SpecKind::Class)) return ArraySpec::Top();

  // For constant pointers, we don't have an array-like val, so return Top.
  // Else, use the layout of the array val. (We pun array-like types here.)
  if (m_hasConstVal) {
    if (m_bits & kMem) return ArraySpec::Top();
    return ArraySpec(ArrayLayout::FromArray(m_arrVal));
  }

  assertx(m_arrSpec != ArraySpec::Bottom());
  return m_arrSpec;
}

inline ClassSpec Type::clsSpec() const {
  if (!supports(SpecKind::Class)) return ClassSpec::Bottom();

  // Currently, a Type which supports multiple specializations is trivial
  // along all of them.
  if (supports(SpecKind::Array)) return ClassSpec::Top();

  if (m_hasConstVal) {
    if (m_bits & kMem) return ClassSpec::Top();
    return ClassSpec(clsVal(), ClassSpec::ExactTag{});
  }

  assertx(m_clsSpec != ClassSpec::Bottom());
  return m_clsSpec;
}

inline TypeSpec Type::spec() const {
  return TypeSpec(arrSpec(), clsSpec());
}

///////////////////////////////////////////////////////////////////////////////
// Inner types.

inline PtrLocation Type::ptrLocation() const {
  return m_ptr;
}

inline Type Type::ptrToLval() const {
  auto temp = *this;
  if (temp.m_bits & kPtr) {
    temp.m_bits &= ~kPtr;
    temp.m_bits |= kLval;
  }
  return temp;
}

inline Type Type::lvalToPtr() const {
  auto temp = *this;
  if (temp.m_bits & kLval) {
    temp.m_bits &= ~kLval;
    temp.m_bits |= kPtr;
  }
  return temp;
}

///////////////////////////////////////////////////////////////////////////////
// Private constructors.

inline Type::Type(bits_t bits, PtrLocation ptr, bool hasConstVal,
                  uintptr_t extra)
  : m_bits(bits)
  , m_ptr(ptr)
  , m_hasConstVal(hasConstVal)
  , m_extra(extra)
{
  assertx(checkValid());
}

inline Type::Type(Type t, ArraySpec arraySpec)
  : m_bits(t.m_bits)
  , m_ptr(t.m_ptr)
  , m_hasConstVal(false)
  , m_arrSpec(arraySpec)
{
  assertx(checkValid());
  assertx(m_arrSpec != ArraySpec::Bottom());
}

inline Type::Type(Type t, ClassSpec classSpec)
  : m_bits(t.m_bits)
  , m_ptr(t.m_ptr)
  , m_hasConstVal(false)
  , m_clsSpec(classSpec)
{
  assertx(checkValid());
  assertx(m_clsSpec != ClassSpec::Bottom());
}

///////////////////////////////////////////////////////////////////////////////

}
