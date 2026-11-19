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

#include "hphp/runtime/base/array-data.h"
#include "hphp/runtime/base/tv-conversions.h"
#include "hphp/runtime/base/tv-mutate.h"
#include "hphp/runtime/base/tv-refcount.h"
#include "hphp/runtime/base/tv-val.h"
#include "hphp/runtime/base/tv-variant.h"
#include "hphp/runtime/base/type-object.h"
#include "hphp/runtime/base/type-resource.h"
#include "hphp/runtime/base/type-string.h"
#include "hphp/runtime/base/typed-value.h"
#include "hphp/runtime/vm/class-meth-data-ref.h"
#include "hphp/runtime/vm/rclass-meth-data.h"
#include "hphp/runtime/vm/rfunc-data.h"

#include <algorithm>
#include <type_traits>

#include <folly/json/dynamic.h>

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

// Forward declare these to avoid including tv-conversions.h which has a
// circular dependency with this file.

struct OptionalVariant;

/*
 * These structs are substitutes for Variant& or const Variant&, when there's
 * no actual Variant in memory to take a reference to (when working with a
 * tv_lval from an Array, for example). They should be treated with the same
 * care as normal references: when copying or storing a variant_ref or
 * const_variant_ref, think carefully about the lifetime of the underlying
 * data.
 */
namespace variant_ref_detail {
template<bool is_const>
struct base {
private:
  using tv_val_t = typename std::conditional<is_const, tv_rval, tv_lval>::type;

public:
  explicit base(tv_val_t val) : m_val{val} {}

  DataType getType() const {
    return type(m_val);
  }
  bool isNull()      const { return isNullType(getType()); }
  bool isBoolean()   const { return isBooleanType(getType()); }
  bool isInteger()   const { return isIntType(getType()); }
  bool isDouble()    const { return isDoubleType(getType()); }
  bool isString()    const { return isStringType(getType()); }
  bool isArray()     const { return isArrayLikeType(getType()); }
  bool isVec()       const { return isVecType(getType()); }
  bool isDict()      const { return isDictType(getType()); }
  bool isKeyset()    const { return isKeysetType(getType()); }
  bool isObject()    const { return isObjectType(getType()); }
  bool isResource()  const { return isResourceType(getType()); }
  bool isFunc()      const { return isFuncType(getType()); }
  bool isClass()     const { return isClassType(getType()); }
  bool isLazyClass() const { return isLazyClassType(getType()); }
  bool isClsMeth()   const { return isClsMethType(getType()); }
  bool isEnumClassLabel() const { return isEnumClassLabelType(getType()); }

  bool isPrimitive() const { return !isRefcountedType(type(m_val)); }

  auto toBoolean() const { return tvCastToBoolean(*m_val); }
  auto toInt64()   const { return tvCastToInt64(*m_val); }
  auto toDouble()  const { return tvCastToDouble(*m_val); }
  auto toString(ConvThrowMode mode = ConvThrowMode::None) const {
    if (isStringType(type(m_val))) return OptString{val(m_val).pstr};
    return OptString::attach(tvCastToStringData(*m_val, mode));
  }
  auto toArray() const {
    if (isArrayLikeType(type(m_val))) return Array{val(m_val).parr};
    return Array::attach(tvCastToArrayLikeData<IntishCast::None>(*m_val));
  }
  auto toObject() const {
    if (isObjectType(type(m_val))) return Object{val(m_val).pobj};
    return Object::attach(tvCastToObjectData(*m_val));
  }

  auto& asCStrRef() const { return HPHP::asCStrRef(m_val); }
  auto& asCArrRef() const { return HPHP::asCArrRef(m_val); }
  auto& asCObjRef() const { return HPHP::asCObjRef(m_val); }

  tv_rval asTypedValue() const { return m_val; }

  ArrayData *getArrayData() const {
    assertx(isArray());
    return val(m_val).parr;
  }

  auto toFuncVal() const {
    assertx(isFunc());
    return val(m_val).pfunc;
  }

  auto toClassVal() const {
    assertx(isClass());
    return val(m_val).pclass;
  }

  auto toLazyClassVal() const {
    assertx(isLazyClass());
    return val(m_val).plazyclass;
  }

  ClsMethDataRef toClsMethVal() const {
    assertx(isClsMeth());
    return val(m_val).pclsmeth;
  }

  RefCount getRefCount() const noexcept {
    return isRefcountedType(type(m_val))
      ? val(m_val).pcnt->count()
      : OneReference;
  }

protected:
  const tv_val_t m_val;
};
}

struct const_variant_ref;

struct variant_ref : variant_ref_detail::base<false> {
  using variant_ref_detail::base<false>::base;

  /* implicit */ variant_ref(Variant& v);

  /* implicit */ operator const_variant_ref() const;

  tv_lval lval() const { return m_val; }

  void unset() const {
    tvMove(make_tv<KindOfUninit>(), m_val);
  }

  void setNull() noexcept {
    tvSetNull(m_val);
  }

  variant_ref& operator=(const Variant& v) noexcept;

  variant_ref& assign(const Variant& v) noexcept;

  variant_ref& operator=(Variant &&rhs) noexcept;

  // Generic assignment operator. Forward argument (preserving rvalue-ness and
  // lvalue-ness) to the appropriate set function, as long as its not a Variant.
  template <typename T>
  typename std::enable_if<
    !std::is_same<
      Variant,
      typename std::remove_reference<typename std::remove_cv<T>::type>::type
    >::value
    &&
    !std::is_same<
      VarNR,
      typename std::remove_reference<typename std::remove_cv<T>::type>::type
    >::value,
    variant_ref&
  >::type operator=(T&& v) {
    set(std::forward<T>(v));
    return *this;
  }

  void set(bool    v) noexcept;
  void set(int     v) noexcept;
  void set(int64_t   v) noexcept;
  void set(double  v) noexcept;
  void set(const char* v) = delete;
  void set(const std::string & v) {
    return set(OptString(v));
  }
  void set(StringData  *v) noexcept;
  void set(ArrayData   *v) noexcept;
  void set(ObjectData  *v) noexcept;
  void set(ResourceHdr *v) noexcept;
  void set(ResourceData *v) noexcept { set(v->hdr()); }
  void set(const StringData *v) = delete;
  void set(const ArrayData *v) = delete;
  void set(const ObjectData *v) = delete;
  void set(const ResourceData *v) = delete;
  void set(const ResourceHdr *v) = delete;

  void set(const OptString& v) noexcept { set(v.get()); }
  void set(const StaticString & v) noexcept;
  void set(const Array& v) noexcept { set(v.get()); }
  void set(const Object& v) noexcept { set(v.get()); }
  void set(const OptResource& v) noexcept { set(v.hdr()); }

  void set(OptString&& v) noexcept { steal(v.detach()); }
  void set(Array&& v) noexcept { steal(v.detach()); }
  void set(Object&& v) noexcept { steal(v.detach()); }
  void set(OptResource&& v) noexcept { steal(v.detachHdr()); }

  template<typename T>
  void set(const req::ptr<T> &v) noexcept {
    return set(v.get());
  }

  template <typename T>
  void set(req::ptr<T>&& v) noexcept {
    return steal(v.detach());
  }

  void steal(StringData* v) noexcept;
  void steal(ArrayData* v) noexcept;
  void steal(ObjectData* v) noexcept;
  void steal(ResourceHdr* v) noexcept;
  void steal(ResourceData* v) noexcept { steal(v->hdr()); }
};

struct const_variant_ref : variant_ref_detail::base<true> {
  using variant_ref_detail::base<true>::base;

  /* implicit */ const_variant_ref(const Variant& v);

  tv_rval rval() const { return m_val; }
};

inline variant_ref::operator const_variant_ref() const {
  return const_variant_ref{m_val};
}

/*
 * This class predates HHVM.
 *
 * In hphpc, when type inference failed to know type of a variable, we
 * would use Variant to represent the php variable in generated C++.
 *
 * Now Variant is only used in C++ extensions, and the API is mostly
 * legacy stuff.  If you're writing a C++ extension, try to avoid
 * Variant when you can (but you often can't, and we don't really have
 * a replacement yet, sorry).
 *
 * In C++ extensions, this class can be used as a generic handle to
 * one of our other data types (e.g. StringData, ArrayData)
 *
 * Beware:
 *
 *    For historical reasons, this class does a lot of things you
 *    don't really expect in a well-behaved C++ class.
 *
 *    For example, the copy constructor is not a copy constructor (it converts
 *    KindOfUninit to KindOfNull).  A similar story applies to the move
 *    constructor.  (And this means we may actually rely on whether copy
 *    elision (NRVO etc) is happening in some places for correctness.)
 *
 *    Use carefully.
 *
 */

struct Variant : private TypedValue {
  friend variant_ref;

  // Used by VariantTraits to create a Optional-like
  // optional Variant which fits in 16 bytes.
  using Optional = OptionalVariant;

  enum class NullInit {};
  enum class NoInit {};
  enum class TVCopy {};
  enum class TVDup {};
  enum class ArrayInitCtor {};
  enum class Attach {};
  enum class Wrap {};

  Variant() noexcept { m_type = KindOfUninit; }
  explicit Variant(NullInit) noexcept { m_type = KindOfNull; }
  explicit Variant(NoInit) noexcept {}

  /* implicit */ Variant(bool    v) noexcept {
    m_type = KindOfBoolean; m_data.num = v;
  }
  /* implicit */ Variant(int     v) noexcept {
    m_type = KindOfInt64; m_data.num = v;
  }
  // The following two overloads will accept int64_t whether it's
  // implemented as long or long long.
  /* implicit */ Variant(long   v) noexcept {
    m_type = KindOfInt64; m_data.num = v;
  }
  /* implicit */ Variant(long long v) noexcept {
    m_type = KindOfInt64; m_data.num = v;
  }
  /* implicit */ Variant(unsigned v) noexcept {
    m_type = KindOfInt64; m_data.num = v;
  }
  /* implicit */ Variant(unsigned long v) noexcept {
    m_type = KindOfInt64; m_data.num = v;
  }
  /* implicit */ Variant(unsigned long long v) noexcept {
    m_type = KindOfInt64; m_data.num = v;
  }
  /* implicit */ Variant(double    v) noexcept {
    m_type = KindOfDouble; m_data.dbl = v;
  }

  /* implicit */ Variant(const char* v) {
    m_type = KindOfString;
    m_data.pstr = StringData::Make(v);
  }
  /* implicit */ Variant(const std::string &v) {
    m_type = KindOfString;
    StringData *s = StringData::Make(v.c_str(), v.size(), CopyString);
    assertx(s);
    m_data.pstr = s;
  }
  /* implicit */ Variant(const StaticString &v) noexcept {
    assertx(v.get() && !v.get()->isRefCounted());
    m_type = KindOfString;
    m_data.pstr = v.get();
  }

  /* implicit */ Variant(const OptString& v) noexcept : Variant(v.get()) {}
  explicit Variant(const String& v) noexcept : Variant(v.get()) {
    assertx(m_type != KindOfNull);
  }
  /* implicit */ Variant(const Array& v) noexcept : Variant(v.get()) { }
  /* implicit */ Variant(const Object& v) noexcept : Variant(v.get()) {}
  /* implicit */ Variant(const OptResource& v) noexcept
  : Variant(v.hdr()) {}

  /* implicit */ Variant(Class* v) {
    m_type = KindOfClass;
    m_data.pclass = v;
  }

  /* implicit */ Variant(LazyClassData v) {
    m_type = KindOfLazyClass;
    m_data.plazyclass = v;
  }

  /* implicit */ Variant(const ClsMethDataRef v) {
    m_type = KindOfClsMeth;
    m_data.pclsmeth = v;
  }

  /*
   * Explicit conversion constructors. These all manipulate ref-counts of bare
   * pointers as a side-effect, so we want to be explicit when its happening.
   */
  explicit Variant(StringData* v) noexcept;
  explicit Variant(ArrayData* v) noexcept {
    if (v) {
      m_data.parr = v;
      m_type = v->toDataType();
      if (v->isRefCounted()) {
        v->rawIncRefCount();
      }
    } else {
      m_type = KindOfNull;
    }
  }
  explicit Variant(ObjectData* v) noexcept {
    if (v) {
      m_type = KindOfObject;
      m_data.pobj = v;
      v->incRefCount();
    } else {
      m_type = KindOfNull;
    }
  }

  explicit Variant(const Func* f) noexcept {
    assertx(f);
    m_type = KindOfFunc;
    m_data.pfunc = f;
  }

  explicit Variant(RFuncData* v) noexcept {
    if (v) {
      assertx(v);
      m_type = KindOfRFunc;
      m_data.prfunc = v;
      v->incRefCount();
    } else {
      m_type = KindOfNull;
    }
  }

  explicit Variant(RClsMethData* v) noexcept {
    if (v) {
      assertx(v);
      m_type = KindOfRClsMeth;
      m_data.prclsmeth = v;
      v->incRefCount();
    } else {
      m_type = KindOfNull;
    }
  }

  template <typename T>
  explicit Variant(const req::ptr<T>& ptr) : Variant(ptr.get()) { }
  template <typename T>
  explicit Variant(req::ptr<T>&& ptr) noexcept
    : Variant(ptr.detach(), Attach{}) { }

  /*
   * Creation constructor from ArrayInit that avoids a null check and an
   * inc-ref.
   */
  explicit Variant(ArrayData* ad, DataType dt, ArrayInitCtor) noexcept {
    assertx(ad->toDataType() == dt);
    m_type = dt;
    m_data.parr = ad;
  }

  enum class PersistentArrInit {};
  Variant(const ArrayData* ad, DataType dt, PersistentArrInit) noexcept {
    // TODO(T58820726): Switch back to trusting caller and strict equality.
    assertx(equivDataTypes(ad->toDataType(), dt));
    assertx(!ad->isRefCounted());
    m_data.parr = const_cast<ArrayData*>(ad);
    m_type = dt;
  }

  enum class PersistentStrInit {};
  explicit Variant(const StringData *s, PersistentStrInit) noexcept {
    assertx(!s->isRefCounted());
    m_data.pstr = const_cast<StringData*>(s);
    m_type = KindOfString;
  }

  enum class EnumClassLabelInit {};
  explicit Variant(const StringData *s, EnumClassLabelInit) noexcept {
    assertx(!s->isRefCounted());
    m_data.pstr = const_cast<StringData*>(s);
    m_type = KindOfEnumClassLabel;
  }

  // These are prohibited, but declared just to prevent accidentally
  // calling the bool constructor just because we had a pointer to
  // const.
  /* implicit */ Variant(const void*) = delete;
  template<typename Ret, typename... Args>
  /* implicit */ Variant(Ret (*)(Args...)) = delete;
  template<class Class, typename Ret, typename... Args>
  /* implicit */ Variant(Ret (Class::*)(Args...)) = delete;
  template<class Class, typename Ret, typename... Args>
  /* implicit */ Variant(Ret (Class::*)(Args...) const) = delete;

  //////////////////////////////////////////////////////////////////////

  /*
   * Copy constructor and copy assignment do not semantically make
   * copies: they turn uninits to null.
   */

  Variant(const Variant& v) noexcept;
  explicit Variant(const_variant_ref v) noexcept;

  Variant(const Variant& v, TVCopy) noexcept {
    m_type = v.m_type;
    m_data = v.m_data;
  }

  Variant(const Variant& v, TVDup) noexcept {
    m_type = v.m_type;
    m_data = v.m_data;
    tvIncRefGen(*asTypedValue());
  }

  Variant& operator=(const Variant& v) noexcept {
    return assign(v);
  }

  /*
   * Move ctors
   *
   * Note: not semantically moves.  Like our "copy constructor", these
   * turn uninits to null.
   */

  Variant(Variant&& v) noexcept {
    assertx(this != &v);
    if (v.m_type != KindOfUninit) {
      m_type = v.m_type;
      m_data = v.m_data;
      v.m_type = KindOfNull;
    } else {
      m_type = KindOfNull;
    }
  }

  // Move ctor for strings
  /* implicit */ Variant(OptString&& v) noexcept {
    StringData *s = v.get();
    if (LIKELY(s != nullptr)) {
      m_data.pstr = s;
      m_type = KindOfString;
      v.detach();
    } else {
      m_type = KindOfNull;
    }
  }

  // Move ctor for arrays
  /* implicit */ Variant(Array&& v) noexcept {
    ArrayData *a = v.get();
    if (LIKELY(a != nullptr)) {
      m_data.parr = a;
      m_type = a->toDataType();
      v.detach();
    } else {
      m_type = KindOfNull;
    }
  }

  // Move ctor for objects
  /* implicit */ Variant(Object&& v) noexcept {
    ObjectData *pobj = v.get();
    if (pobj) {
      m_type = KindOfObject;
      m_data.pobj = pobj;
      v.detach();
    } else {
      m_type = KindOfNull;
    }
  }

  // Move ctor for resources
  /* implicit */ Variant(OptResource&& v) noexcept {
    auto hdr = v.hdr();
    if (hdr) {
      m_type = KindOfResource;
      m_data.pres = hdr;
      v.detachHdr();
    } else {
      m_type = KindOfNull;
    }
  }

  /*
   * Move assign
   *
   * Note: not semantically moves.  Like our "copies", these turn uninits
   * to null.
   */
  Variant& operator=(Variant &&rhs) noexcept {
    assertx(this != &rhs); // we end up as null on a self move-assign.
    Variant& lhs = *this;

    Variant goner((NoInit()));
    goner.m_data = lhs.m_data;
    goner.m_type = lhs.m_type;

    if (rhs.m_type == KindOfUninit) {
      lhs.m_type = KindOfNull;
    } else {
      lhs.m_type = rhs.m_type;
      lhs.m_data = rhs.m_data;
      rhs.m_type = KindOfNull;
    }

    return *this;
  }

  ALWAYS_INLINE ~Variant() noexcept {
    tvDecRefGen(asTypedValue());
    if (debug) {
      memset(static_cast<void*>(this), kTVTrashFill2, sizeof(*this));
    }
  }


  //////////////////////////////////////////////////////////////////////

  /*
   * During sweeping, request-allocated things are not allowed to be decref'd
   * or manipulated.  This function is used to cause a Variant to go
   * into a state where its destructor will have no effects on the
   * request local heap, in cases where sweepable objects can't
   * organize things to avoid running Variant destructors.
   */
  void releaseForSweep() { m_type = KindOfNull; }

  //////////////////////////////////////////////////////////////////////

  /**
   * Break bindings and set to uninit.
   */
  void unset() {
    auto const old = *asTypedValue();
    m_type = KindOfUninit;
    tvDecRefGen(old);
  }

  /**
   * set to null without breaking bindings (if any), faster than v_a = null;
   */
  void setNull() noexcept {
    tvSetNull(*asTypedValue());
  }

  /**
   * Create from a JSON-like dynamic object
   */
  static Variant fromDynamic(const folly::dynamic& dy);

  static Variant attach(TypedValue tv) noexcept {
    return Variant{tv, Attach{}};
  }
  static Variant attach(StringData* var) noexcept {
    return Variant{var, Attach{}};
  }
  static Variant attach(ArrayData* var) noexcept {
    return Variant{var, Attach{}};
  }
  static Variant attach(ObjectData* var) noexcept {
    return Variant{var, Attach{}};
  }
  static Variant attach(ResourceData* var) noexcept {
    return Variant{var, Attach{}};
  }
  static Variant attach(ResourceHdr* var) noexcept {
    return Variant{var, Attach{}};
  }

  static Variant wrap(TypedValue tv) noexcept {
    return Variant{tv, Wrap{}};
  }

///////////////////////////////////////////////////////////////////////////////
// int64

  ALWAYS_INLINE int64_t asInt64Val() const {
    assertx(m_type == KindOfInt64);
    return m_data.num;
  }

  ALWAYS_INLINE int64_t toInt64Val() const {
    assertx(is(KindOfInt64));
    return m_data.num;
  }

///////////////////////////////////////////////////////////////////////////////
// double

  ALWAYS_INLINE double asDoubleVal() const {
    assertx(m_type == KindOfDouble);
    return m_data.dbl;
  }

  ALWAYS_INLINE double toDoubleVal() const {
    assertx(is(KindOfDouble));
    return m_data.dbl;
  }

///////////////////////////////////////////////////////////////////////////////
// boolean

  ALWAYS_INLINE bool asBooleanVal() const {
    assertx(m_type == KindOfBoolean);
    return m_data.num;
  }

  ALWAYS_INLINE bool toBooleanVal() const {
    assertx(is(KindOfBoolean));
    return m_data.num;
  }

///////////////////////////////////////////////////////////////////////////////
// string

  ALWAYS_INLINE const OptString& asCStrRef() const {
    assertx(isStringType(m_type) && m_data.pstr);
    return *reinterpret_cast<const OptString*>(&m_data.pstr);
  }

  ALWAYS_INLINE OptString& asStrRef() {
    assertx(isStringType(m_type) && m_data.pstr);
    m_type = KindOfString;
    return *reinterpret_cast<OptString*>(&m_data.pstr);
  }

///////////////////////////////////////////////////////////////////////////////
// array

  ALWAYS_INLINE const Array& asCArrRef() const {
    assertx(isArrayLikeType(m_type) && m_data.parr);
    return *reinterpret_cast<const Array*>(&m_data.parr);
  }

  ALWAYS_INLINE Array& asArrRef() {
    assertx(isArrayLikeType(m_type) && m_data.parr);
    m_type = m_data.parr->toDataType();
    return *reinterpret_cast<Array*>(&m_data.parr);
  }

///////////////////////////////////////////////////////////////////////////////
// object

  ALWAYS_INLINE const Object& asCObjRef() const {
    assertx(m_type == KindOfObject && m_data.pobj);
    return *reinterpret_cast<const Object*>(&m_data.pobj);
  }

  ALWAYS_INLINE Object & asObjRef() {
    assertx(m_type == KindOfObject && m_data.pobj);
    return *reinterpret_cast<Object*>(&m_data.pobj);
  }

  ALWAYS_INLINE const OptResource& asCResRef() const {
    assertx(m_type == KindOfResource && m_data.pres);
    return *reinterpret_cast<const OptResource*>(&m_data.pres);
  }

  ALWAYS_INLINE const OptResource& toCResRef() const {
    assertx(is(KindOfResource));
    assertx(m_data.pres);
    return *reinterpret_cast<const OptResource*>(&m_data.pres);
  }

  ALWAYS_INLINE OptResource & asResRef() {
    assertx(m_type == KindOfResource && m_data.pres);
    return *reinterpret_cast<OptResource*>(&m_data.pres);
  }

  /**
   * Type testing functions
   */
  DataType getType() const {
    return m_type;
  }
  bool is(DataType type) const {
    return getType() == type;
  }
  bool isInitialized() const {
    return m_type != KindOfUninit;
  }
  bool isNull() const {
    return isNullType(getType());
  }
  bool isBoolean() const {
    return getType() == KindOfBoolean;
  }
  bool isInteger() const {
    return getType() == KindOfInt64;
  }
  bool isDouble() const {
    return getType() == KindOfDouble;
  }
  bool isString() const {
    return isStringType(getType());
  }
  bool isArray() const {
    return isArrayLikeType(getType());
  }
  bool isVec() const {
    return isVecType(getType());
  }
  bool isDict() const {
    return isDictType(getType());
  }
  bool isKeyset() const {
    return isKeysetType(getType());
  }
  bool isObject() const {
    return getType() == KindOfObject;
  }
  bool isResource() const {
    return getType() == KindOfResource;
  }
  bool isFunc() const {
    return isFuncType(getType());
  }
  bool isClass() const {
    return isClassType(getType());
  }
  bool isLazyClass() const {
    return isLazyClassType(getType());
  }
  bool isClsMeth() const {
    return isClsMethType(getType());
  }
  bool isEnumClassLabel() const {
    return getType() == KindOfEnumClassLabel;
  }

  bool isNumeric(bool checkString = false) const noexcept;
  DataType toNumeric(int64_t &ival, double &dval, bool checkString = false)
    const;
  bool isScalar() const noexcept;
  bool isIntVal() const {
    switch (m_type) {
      case KindOfUninit:
      case KindOfNull:
      case KindOfBoolean:
      case KindOfInt64:
      case KindOfObject:
      case KindOfResource:
        return true;
      case KindOfDouble:
      case KindOfString:
      case KindOfVec:
      case KindOfDict:
      case KindOfKeyset:
      case KindOfRFunc:
      case KindOfFunc:
      case KindOfClass:
      case KindOfLazyClass:
      case KindOfClsMeth:
      case KindOfRClsMeth:
      case KindOfEnumClassLabel:
        return false;
    }
    not_reached();
  }

  // Is "define('CONSTANT', <this value>)" legal?
  enum class AllowedAsConstantValue {
      Allowed
    , NotAllowed
    , ContainsObject // Allowed if constant of an "enum class".
  };
  AllowedAsConstantValue isAllowedAsConstantValue() const;

  /**
   * Get reference count of weak or strong binding. For debugging purpose.
   */
  int getRefCount() const noexcept {
    return const_variant_ref{*this}.getRefCount();
  }

  bool getBoolean() const {
    assertx(getType() == KindOfBoolean);
    return m_data.num;
  }
  int64_t getInt64() const {
    assertx(getType() == KindOfInt64);
    return m_data.num;
  }
  double getDouble() const {
    assertx(getType() == KindOfDouble);
    return m_data.dbl;
  }

  /**
   * Operators
   */
  Variant& assign(const Variant& v) noexcept {
    tvSet(tvToInit(*v.asTypedValue()), *asTypedValue());
    return *this;
  }

  // Generic assignment operator. Forward argument (preserving rvalue-ness and
  // lvalue-ness) to the appropriate set function, as long as its not a Variant.
  template <typename T>
  typename std::enable_if<
    !std::is_same<
      Variant,
      typename std::remove_reference<typename std::remove_cv<T>::type>::type
    >::value
    &&
    !std::is_same<
      VarNR,
      typename std::remove_reference<typename std::remove_cv<T>::type>::type
    >::value,
    Variant&
  >::type operator=(T&& v) {
    set(std::forward<T>(v));
    return *this;
  }

  Variant  operator +  () const = delete;
  Variant &operator += (const Variant& v) = delete;
  Variant &operator += (int     n) = delete;
  Variant &operator += (int64_t   n) = delete;
  Variant &operator += (double  n)  = delete;

  Variant  operator -  () const = delete;
  Variant  operator -  (const Variant& v) const = delete;
  Variant &operator -= (const Variant& v) = delete;
  Variant &operator -= (int     n) = delete;
  Variant &operator -= (int64_t   n) = delete;
  Variant &operator -= (double  n) = delete;

  Variant  operator *  (const Variant& v) const = delete;
  Variant &operator *= (const Variant& v) = delete;
  Variant &operator *= (int     n) = delete;
  Variant &operator *= (int64_t   n) = delete;
  Variant &operator *= (double  n) = delete;

  Variant  operator /  (const Variant& v) const = delete;
  Variant &operator /= (const Variant& v) = delete;
  Variant &operator /= (int     n) = delete;
  Variant &operator /= (int64_t   n) = delete;
  Variant &operator /= (double  n) = delete;

  int64_t    operator %  (const Variant& v) const = delete;
  Variant &operator %= (const Variant& v) = delete;
  Variant &operator %= (int     n) = delete;
  Variant &operator %= (int64_t   n) = delete;
  Variant &operator %= (double  n) = delete;

  Variant  operator |  (const Variant& v) const = delete;
  Variant &operator |= (const Variant& v) = delete;
  Variant  operator &  (const Variant& v) const = delete;
  Variant &operator &= (const Variant& v) = delete;
  Variant  operator ^  (const Variant& v) const = delete;
  Variant &operator ^= (const Variant& v) = delete;
  Variant &operator <<=(int64_t n) = delete;
  Variant &operator >>=(int64_t n) = delete;

  Variant &operator ++ () = delete;
  Variant  operator ++ (int) = delete;
  Variant &operator -- () = delete;
  Variant  operator -- (int) = delete;

  /**
   * Explicit type conversions
   */
  bool toBoolean() const {
    if (isNullType(m_type)) return false;
    if (hasNumData(m_type)) return m_data.num;
    return toBooleanHelper();
  }
  int64_t toInt64() const {
    if (isNullType(m_type)) return 0;
    if (hasNumData(m_type)) return m_data.num;
    return toInt64Helper(10);
  }
  int64_t toInt64(int base) const {
    if (isNullType(m_type)) return 0;
    if (hasNumData(m_type)) return m_data.num;
    return toInt64Helper(base);
  }
  double toDouble() const {
    if (m_type == KindOfDouble) return m_data.dbl;
    return tvCastToDouble(*asTypedValue());
  }

  OptString toString(ConvThrowMode mode = ConvThrowMode::None) const& {
    if (isStringType(m_type)) return OptString{m_data.pstr};
    return OptString::attach(tvCastToStringData(*this, mode));
  }

  OptString toString(ConvThrowMode mode = ConvThrowMode::None) && {
    if (isStringType(m_type)) {
      m_type = KindOfNull;
      return OptString::attach(m_data.pstr);
    }
    return toString(mode);
  }

  // Convert a non-array-like type to a PHP array, leaving PHP arrays and Hack
  // arrays unchanged. Use toPHPArray() if you want the result to always be a
  // PHP array.
  template <IntishCast IC = IntishCast::None>
  Array toArray() const {
    if (isArrayLikeType(m_type)) return Array{m_data.parr};
    return Array::attach(tvCastToArrayLikeData<IC>(*this));
  }
  Array toPHPArray() const {
    return toPHPArrayHelper();
  }
  Object toObject() const {
    if (isObjectType(m_type)) return Object{m_data.pobj};
    return Object::attach(tvCastToObjectData(*this));
  }
  OptResource toResource() const {
    if (m_type == KindOfResource) return OptResource{m_data.pres};
    return toResourceHelper();
  }

  Array toVec() const {
    if (isVecType(m_type)) return Array{m_data.parr};
    auto copy = *this;
    tvCastToVecInPlace(copy.asTypedValue());
    assertx(copy.isVec());
    return Array::attach(copy.detach().m_data.parr);
  }

  Array toDict() const {
    if (isDictType(m_type)) return Array{m_data.parr};
    auto copy = *this;
    tvCastToDictInPlace(copy.asTypedValue());
    assertx(copy.isDict());
    return Array::attach(copy.detach().m_data.parr);
  }

  Array toKeyset() const {
    if (isKeysetType(m_type)) return Array{m_data.parr};
    auto copy = *this;
    tvCastToKeysetInPlace(copy.asTypedValue());
    assertx(copy.isKeyset());
    return Array::attach(copy.detach().m_data.parr);
  }

  template <typename T>
  typename std::enable_if<std::is_base_of<ResourceData,T>::value, bool>::type
  isa() const {
    if (m_type == KindOfResource) {
      return m_data.pres->data()->instanceof<T>();
    }
    return false;
  }

  template <typename T>
  typename std::enable_if<std::is_base_of<ObjectData,T>::value, bool>::type
  isa() const {
    if (m_type == KindOfObject) {
      return m_data.pobj->instanceof<T>();
    }
    return false;
  }

  /*
   * Convert to a valid key or throw an exception. If convertStrKeys is true
   * int-like string keys will be converted to int keys.
   */
  template <IntishCast IC = IntishCast::None>
  VarNR toKey(const ArrayData*) const;

  /* Creating a temporary Array, String, or Object with no ref-counting and
   * no type checking, use it only when we have checked the variant type and
   * we are sure the internal data will have a reference until the temporary
   * one gets out-of-scope.
   */
  StrNR toStrNR() const {
    return StrNR(getStringData());
  }
  ArrNR toArrNR() const {
    return ArrNR(getArrayData());
  }
  ObjNR toObjNR() const {
    return ObjNR(getObjectData());
  }

  auto toFuncVal() const {
    return const_variant_ref{*this}.toFuncVal();
  }
  auto toClassVal() const {
    return const_variant_ref{*this}.toClassVal();
  }
  auto toLazyClassVal() const {
    return const_variant_ref{*this}.toLazyClassVal();
  }
  ClsMethDataRef toClsMethVal() const {
    return const_variant_ref{*this}.toClsMethVal();
  }

  /*
   * Low level access that should be restricted to internal use.
   */
  int64_t *getInt64Data() const {
    assertx(getType() == KindOfInt64);
    return const_cast<int64_t*>(&m_data.num);
  }
  double *getDoubleData() const {
    assertx(getType() == KindOfDouble);
    return const_cast<double*>(&m_data.dbl);
  }
  StringData *getStringData() const {
    assertx(isStringType(getType()));
    return m_data.pstr;
  }
  StringData *getStringDataOrNull() const {
    // This is a necessary evil because getStringData() returns
    // an undefined result if this is a null variant
    assertx(isNull() || isString());
    return isNullType(m_type) ? nullptr : m_data.pstr;
  }
  ArrayData *getArrayData() const {
    assertx(isArray());
    return m_data.parr;
  }
  ArrayData *getArrayDataOrNull() const {
    // This is a necessary evil because getArrayData() returns
    // an undefined result if this is a null variant
    assertx(isNull() || isArray());
    return isNullType(m_type) ? nullptr : m_data.parr;
  }
  ObjectData* getObjectData() const {
    assertx(is(KindOfObject));
    return m_data.pobj;
  }
  ObjectData *getObjectDataOrNull() const {
    // This is a necessary evil because getObjectData() returns
    // an undefined result if this is a null variant
    assertx(isNull() || is(KindOfObject));
    return isNullType(m_type) ? nullptr : m_data.pobj;
  }

  int64_t getNumData() const { return m_data.num; }

  /*
   * Make any Variant strings and arrays not ref counted (e.g., static).
   * Use it, for example, if you need a long-lived Variant before the Memory
   * Manager has been initialized.
   * You will still get an assertion if the Variant is an object, resource, etc.
   */
  void setEvalScalar();

  /*
   * Access this Variant as a TypedValue.
   */
  const TypedValue* asTypedValue() const { return this; }
        TypedValue* asTypedValue()       { return this; }

  /*
   * Read this Variant as an InitCell, without incrementing the
   * reference count.  I.e. turn KindOfUninit into KindOfNull.
   */
  TypedValue asInitTVTmp() const {
    if (m_type == KindOfUninit) return make_tv<KindOfNull>();
    return *this;
  }

  TypedValue detach() noexcept {
    auto tv = *asTypedValue();
    m_type = KindOfNull;
    return tv;
  }

 private:
  ResourceData* getResourceData() const {
    assertx(is(KindOfResource));
    return m_data.pres->data();
  }

  ResourceData* detachResourceData() {
    assertx(is(KindOfResource));
    assertx(m_type == KindOfResource);
    m_type = KindOfNull;
    return m_data.pres->data();
  }

  ObjectData* detachObjectData() {
    assertx(is(KindOfObject));
    assertx(m_type == KindOfObject);
    m_type = KindOfNull;
    return m_data.pobj;
  }

  template <typename T>
  friend typename std::enable_if<
    std::is_base_of<ResourceData,T>::value,
    ResourceData*
  >::type deref(const Variant& v) { return v.getResourceData(); }

  template <typename T>
  friend typename std::enable_if<
    std::is_base_of<ObjectData,T>::value,
    ObjectData*
  >::type deref(const Variant& v) { return v.getObjectData(); }

  template <typename T>
  friend typename std::enable_if<
    std::is_base_of<ResourceData,T>::value,
    ResourceData*
  >::type detach(Variant&& v) { return v.detachResourceData(); }

  template <typename T>
  friend typename std::enable_if<
    std::is_base_of<ObjectData,T>::value,
    ObjectData*
  >::type detach(Variant&& v) { return v.detachObjectData(); }

  explicit Variant(ResourceData* v) noexcept {
    if (v) {
      m_type = KindOfResource;
      m_data.pres = v->hdr();
      v->incRefCount();
    } else {
      m_type = KindOfNull;
    }
  }
  explicit Variant(ResourceHdr* v) noexcept {
    if (v) {
      m_type = KindOfResource;
      m_data.pres = v;
      v->incRefCount();
    } else {
      m_type = KindOfNull;
    }
  }

  /*
   * This set of constructors act like the normal constructors for the
   * given types except that they do not increment the reference count
   * of the passed value.  They are used for the req::ptr move constructor.
   */
  Variant(StringData* var, Attach) noexcept {
    if (var) {
      m_type = KindOfString;
      m_data.pstr = var;
    } else {
      m_type = KindOfNull;
    }
  }
  Variant(ArrayData* var, Attach) noexcept {
    if (var) {
      m_type = var->toDataType();
      m_data.parr = var;
    } else {
      m_type = KindOfNull;
    }
  }
  Variant(ObjectData* var, Attach) noexcept {
    if (var) {
      m_type = KindOfObject;
      m_data.pobj = var;
    } else {
      m_type = KindOfNull;
    }
  }
  Variant(ResourceData* var, Attach) noexcept {
    if (var) {
      m_type = KindOfResource;
      m_data.pres = var->hdr();
    } else {
      m_type = KindOfNull;
    }
  }
  Variant(ResourceHdr* var, Attach) noexcept {
    if (var) {
      m_type = KindOfResource;
      m_data.pres = var;
    } else {
      m_type = KindOfNull;
    }
  }
  Variant(TypedValue tv, Attach) noexcept : TypedValue(tv) {}
  Variant(TypedValue tv, Wrap) noexcept : TypedValue(tv) {
    tvIncRefGen(*asTypedValue());
  }

  bool isPrimitive() const { return !isRefcountedType(m_type); }
  bool isObjectConvertable() {
    return isNullType(m_type) ||
      (m_type == KindOfBoolean && !m_data.num) ||
      (isStringType(m_type) && m_data.pstr->empty());
  }

  void set(bool    v) noexcept;
  void set(int     v) noexcept;
  void set(int64_t   v) noexcept;
  void set(double  v) noexcept;
  void set(const char* v) = delete;
  void set(const std::string & v) {
    return set(OptString(v));
  }
  void set(StringData  *v) noexcept;
  void set(ArrayData   *v) noexcept;
  void set(ObjectData  *v) noexcept;
  void set(ResourceHdr *v) noexcept;
  void set(ResourceData *v) noexcept { set(v->hdr()); }
  void set(const StringData *v) = delete;
  void set(const ArrayData *v) = delete;
  void set(const ObjectData *v) = delete;
  void set(const ResourceData *v) = delete;
  void set(const ResourceHdr *v) = delete;

  void set(const OptString& v) noexcept { set(v.get()); }
  void set(const StaticString & v) noexcept;
  void set(const Array& v) noexcept { set(v.get()); }
  void set(const Object& v) noexcept { set(v.get()); }
  void set(const OptResource& v) noexcept { set(v.hdr()); }

  void set(OptString&& v) noexcept { steal(v.detach()); }
  void set(Array&& v) noexcept { steal(v.detach()); }
  void set(Object&& v) noexcept { steal(v.detach()); }
  void set(OptResource&& v) noexcept { steal(v.detachHdr()); }

  template<typename T>
  void set(const req::ptr<T> &v) noexcept {
    return set(v.get());
  }

  template <typename T>
  void set(req::ptr<T>&& v) noexcept {
    return steal(v.detach());
  }

  void steal(StringData* v) noexcept;
  void steal(ArrayData* v) noexcept;
  void steal(ObjectData* v) noexcept;
  void steal(ResourceHdr* v) noexcept;
  void steal(ResourceData* v) noexcept { steal(v->hdr()); }

private:
  bool   toBooleanHelper() const;
  int64_t  toInt64Helper(int base = 10) const;
  Array  toPHPArrayHelper() const;
  OptResource toResourceHelper() const;
};

Variant operator+(const Variant & lhs, const Variant & rhs) = delete;

///////////////////////////////////////////////////////////////////////////////

/*
 * Definitions for some members of variant_ref et al. that use Variant.
 */

inline variant_ref::variant_ref(Variant& v)
  : variant_ref_detail::base<false>{v.asTypedValue()}
{}

inline const_variant_ref::const_variant_ref(const Variant& v)
  : variant_ref_detail::base<true>{v.asTypedValue()}
{}

inline variant_ref& variant_ref::operator=(const Variant& v) noexcept {
  return assign(v);
}

inline variant_ref& variant_ref::assign(const Variant& v) noexcept {
  tvSet(tvToInit(*v.asTypedValue()), m_val);
  return *this;
}

inline variant_ref& variant_ref::operator=(Variant &&rhs) noexcept {
  variant_ref lhs = *this;

  Variant goner((Variant::NoInit()));
  goner.m_data = val(lhs.m_val);
  goner.m_type = type(lhs.m_val);

  if (rhs.m_type == KindOfUninit) {
    type(lhs.m_val) = KindOfNull;
  } else {
    type(lhs.m_val) = rhs.m_type;
    val(lhs.m_val) = rhs.m_data;
    rhs.m_type = KindOfNull;
  }

  return *this;
}

///////////////////////////////////////////////////////////////////////////////
// VarNR

struct VarNR : private TypedValue {
  static VarNR MakeKey(const OptString& s) {
    if (s.empty()) return VarNR(staticEmptyString());
    return VarNR(s);
  }

  // Use to hold variant that do not need ref-counting
  explicit VarNR(bool      v) { m_type = KindOfBoolean; m_data.num = (v?1:0);}
  explicit VarNR(int       v) { m_type = KindOfInt64;   m_data.num = v;}
  // The following two overloads will accept int64_t whether it's
  // implemented as long or long long.
  explicit VarNR(long      v) { m_type = KindOfInt64;   m_data.num = v;}
  explicit VarNR(long long v) { m_type = KindOfInt64;   m_data.num = v;}
  explicit VarNR(uint64_t  v) { m_type = KindOfInt64;   m_data.num = v;}
  explicit VarNR(double    v) { m_type = KindOfDouble;  m_data.dbl = v;}

  explicit VarNR(const StaticString &v) {
    assertx(v.get() && !v.get()->isRefCounted());
    m_type = KindOfString;
    m_data.pstr = v.get();
  }

  explicit VarNR(const OptString& v) : VarNR(v.get()) {}
  explicit VarNR(const Array& v) : VarNR(v.get()) {}
  explicit VarNR(const Object& v) : VarNR(v.get()) {}

  explicit VarNR(StringData *v);
  explicit VarNR(const StringData *v) {
    assertx(v && !v->isRefCounted());
    m_type = KindOfString;
    m_data.pstr = const_cast<StringData*>(v);
  }
  explicit VarNR(ArrayData *v);
  explicit VarNR(const ArrayData*) = delete;
  explicit VarNR(ObjectData *v);
  explicit VarNR(const ObjectData*) = delete;

  explicit VarNR(TypedValue tv) { m_type = tv.m_type; m_data = tv.m_data; }
  explicit VarNR(const Variant& v) : VarNR{*v.asTypedValue()} {}

  VarNR(const VarNR&) = delete;
  VarNR& operator=(const VarNR&) = delete;

  explicit VarNR() { asVariant()->asTypedValue()->m_type = KindOfUninit; }

  ~VarNR() {
    if (debug) {
      checkRefCount();
      memset(static_cast<void*>(this), kTVTrashFill2, sizeof(*this));
    }
  }

  TypedValue tv() const { return *this; }

  operator const Variant&() const { return *asVariant(); }

  bool isNull() const {
    return asVariant()->isNull();
  }
private:
  /* implicit */ VarNR(const char* v) = delete;
  /* implicit */ VarNR(const std::string & v) = delete;

  const Variant *asVariant() const {
    return &tvAsCVarRef(static_cast<const TypedValue*>(this));
  }
  Variant* asVariant() {
    return &tvAsVariant(static_cast<TypedValue*>(this));
  }
  void checkRefCount() {
    assertx(isRealType(m_type));

    switch (m_type) {
      DT_UNCOUNTED_CASE:
        return;
      case KindOfString:
        assertx(m_data.pstr->checkCount());
        return;
      case KindOfVec:
      case KindOfDict:
      case KindOfKeyset:
        assertx(m_data.parr->checkCount());
        return;
      case KindOfRClsMeth:
        assertx(m_data.prclsmeth->checkCount());
        return;
      case KindOfObject:
        assertx(m_data.pobj->checkCount());
        return;
      case KindOfResource:
        assertx(m_data.pres->checkCount());
        return;
      case KindOfRFunc:
        assertx(m_data.prfunc->checkCount());
        return;
    }
    not_reached();
  }
};

//////////////////////////////////////////////////////////////////////

/*
 * The lvalBlackHole is used in array operations when a NewElem can't
 * create a new slot.  (Basically if the next integer key in an array
 * is already at the maximum integer.)
 */
Variant& lvalBlackHole();

/*
 * The lvalBlackHole has request lifetime.
 */
void initBlackHole();
void clearBlackHole();

///////////////////////////////////////////////////////////////////////////////
// breaking circular dependencies

inline Variant Array::operator[](TypedValue key) const {
  return Variant::wrap(lookup(key));
}
inline Variant Array::operator[](int key) const {
  return Variant::wrap(lookup(key));
}
inline Variant Array::operator[](int64_t key) const {
  return Variant::wrap(lookup(key));
}
inline Variant Array::operator[](const OptString& key) const {
  return Variant::wrap(lookup(key));
}
inline Variant Array::operator[](const Variant& key) const {
  return Variant::wrap(lookup(key));
}

inline void Array::append(const Variant& v) {
  append(*v.asTypedValue());
}

ALWAYS_INLINE Variant uninit_null() {
  return Variant();
}

ALWAYS_INLINE Variant init_null() {
  return Variant(Variant::NullInit());
}

inline void concat_assign(Variant &v1, const char* s2) = delete;

inline void concat_assign(tv_lval lhs, const OptString& s2) {
  if (!tvIsString(lhs)) {
    tvCastToStringInPlace(lhs, ConvThrowMode::Throw);
  }
  asStrRef(lhs) += s2;
}

//////////////////////////////////////////////////////////////////////

inline Array& forceToArray(Variant& var) {
  if (!var.isArray()) var = Variant(Array::CreateDict());
  return var.asArrRef();
}

inline Array& forceToArray(tv_lval lval) {
  if (!isArrayLikeType(lval.type())) {
    tvMove(make_array_like_tv(staticEmptyDictArray()), lval);
  }
  return asArrRef(lval);
}

inline Array& forceToDict(Variant& var) {
  if (!var.isDict()) var = Variant(Array::CreateDict());
  return var.asArrRef();
}

inline Array& forceToDict(tv_lval lval) {
  if (!isDictType(lval.type())) {
    tvSet(make_tv<KindOfDict>(staticEmptyDictArray()), lval);
  }
  return asArrRef(lval);
}

//////////////////////////////////////////////////////////////////////

ALWAYS_INLINE Variant empty_string_variant() {
  return Variant(staticEmptyString(), Variant::PersistentStrInit{});
}

template <typename T>
inline Variant toVariant(const req::ptr<T>& p) {
  return p ? Variant(p) : Variant(false);
}

template <typename T>
inline Variant toVariant(req::ptr<T>&& p) {
  return p ? Variant(std::move(p)) : Variant(false);
}

template <>
inline bool ptr_is_null(const Variant& v) {
  return v.isNull();
}

template <typename T>
inline bool isa_non_null(const Variant& v) {
  return v.isa<T>();
}

// Defined here to avoid introducing a dependency cycle between type-variant
// and type-array
template <IntishCast IC>
ALWAYS_INLINE TypedValue Array::convertKey(TypedValue k) const {
  return tvToKey<IC>(k, m_arr ? m_arr.get() : staticEmptyDictArray());
}
template <IntishCast IC>
ALWAYS_INLINE TypedValue Array::convertKey(const Variant& k) const {
  return convertKey<IC>(*k.asTypedValue());
}

template <IntishCast IC>
inline VarNR Variant::toKey(const ArrayData* ad) const {
  return VarNR(tvToKey<IC>(*this, ad));
}

struct alignas(16) OptionalVariant {
  OptionalVariant() {
    m_tv.m_type = kInvalidDataType;
  }
  ~OptionalVariant() {
    clear();
  }
  OptionalVariant(const OptionalVariant& other) {
    if (other.hasValue()) {
      construct(other.value());
      return;
    }
    m_tv.m_type = kInvalidDataType;
  }
  OptionalVariant(OptionalVariant&& other) noexcept {
    if (other.hasValue()) {
      construct(std::move(other.value()));
      other.m_tv.m_type = kInvalidDataType;
      return;
    }
    m_tv.m_type = kInvalidDataType;
  }

  template<typename Arg>
  OptionalVariant& operator=(Arg&& arg) {
    assign(std::forward<Arg>(arg));
    return *this;
  }
  OptionalVariant& operator=(const OptionalVariant& arg) {
    assign(arg);
    return *this;
  }
  OptionalVariant& operator=(OptionalVariant&& arg) {
    assign(std::move(arg));
    return *this;
  }

  bool hasValue() const {
    return m_tv.m_type != kInvalidDataType;
  }
  bool has_value() const {
    return hasValue();
  }
  Variant& value() {
    assertx(hasValue());
    return tvAsVariant(&m_tv);
  }
  const Variant& value() const {
    assertx(hasValue());
    return tvAsCVarRef(&m_tv);
  }
  void clear() {
    if (hasValue()) {
      auto const old = m_tv;
      m_tv.m_type = kInvalidDataType;
      tvDecRefGen(old);
    }
  }
  template <class... Args>
  Variant& emplace(Args&&... args) {
    clear();
    construct(std::forward<Args>(args)...);
    return value();
  }
  void assign(const Variant& other) {
    if (hasValue()) {
      value() = other;
      return;
    }
    construct(other);
  }
  void assign(Variant&& other) {
    if (hasValue()) {
      value() = std::move(other);
      return;
    }
    construct(std::move(other));
  }
  void assign(const OptionalVariant& other) {
    if (other.hasValue()) return assign(other.value());
    clear();
  }
  void assign(OptionalVariant&& other) {
    if (other.hasValue()) {
      assign(std::move(other.value()));
      other.m_tv.m_type = kInvalidDataType;
      return;
    }
    clear();
  }

  operator bool() const { return hasValue(); }

  const Variant* operator->() const { return &value(); }
  Variant* operator->() { return &value(); }

  const Variant& operator*() const & { return value(); }
  Variant& operator*() & { return value(); }
  Variant&& operator*() && { return std::move(value()); }
private:
  template<typename... Args>
  void construct(Args&&... args) {
    new (&m_tv) Variant(std::forward<Args>(args)...);
  }
  TypedValue m_tv;
};

//////////////////////////////////////////////////////////////////////

}
