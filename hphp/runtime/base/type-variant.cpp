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

#include "hphp/runtime/base/type-variant.h"

#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/base/array-iterator.h"
#include "hphp/runtime/base/bespoke-array.h"
#include "hphp/runtime/base/double-to-int64.h"
#include "hphp/runtime/base/dummy-resource.h"
#include "hphp/runtime/base/req-root.h"
#include "hphp/runtime/base/request-info.h"
#include "hphp/runtime/base/tv-refcount.h"
#include "hphp/runtime/base/vanilla-dict.h"
#include "hphp/runtime/base/vanilla-keyset.h"
#include "hphp/runtime/base/vanilla-vec.h"

#include "hphp/runtime/vm/class-meth-data-ref.h"
#include "hphp/runtime/vm/func.h"
#include "hphp/system/systemlib.h"

#include "hphp/util/abi-cxx.h"
#include "hphp/util/roar.h"

#include <utility>

extern "C" __attribute__((weak))
int __roar_api_flag_safe_function_pointer_site(void*, void*);

namespace HPHP {

using BlackHoleStorage = std::aligned_storage<
  sizeof(req::root<Variant>),
  alignof(req::root<Variant>)
>::type;

static RDS_LOCAL(BlackHoleStorage, bhStorage);

///////////////////////////////////////////////////////////////////////////////

Variant::Variant(StringData *v) noexcept {
  if (v) {
    m_data.pstr = v;
    m_type = KindOfString;
    if (v->isRefCounted()) {
      v->rawIncRefCount();
    }
  } else {
    m_type = KindOfNull;
  }
}

namespace {
ALWAYS_INLINE
void implCopyConstruct(TypedValue tv, Variant& v) {
  tvDup(tvToInit(tv), v.asTypedValue());
}
}

Variant::Variant(const Variant& v) noexcept {
  implCopyConstruct(*v.asTypedValue(), *this);
}

Variant::Variant(const_variant_ref v) noexcept {
  implCopyConstruct(*v.rval(), *this);
}

Variant Variant::fromDynamic(const folly::dynamic& dy) {
  if (dy.isNull()) {
    return Variant{Variant::NullInit{}};
  } else if (dy.isInt()) {
    return {dy.getInt()};
  } else if (dy.isDouble()) {
    return {dy.getDouble()};
  } else if (dy.isString()) {
    return {dy.getString()};
  } else if (dy.isBool()) {
    return {dy.getBool()};
  } else if (dy.isArray()) {
    VecInit ret{dy.size()};
    for (auto const& v : dy) {
      ret.append(Variant::fromDynamic(v));
    }
    return ret.toVariant();
  } else {
    always_assert(dy.isObject());
    DictInit ret{dy.size()};
    for (auto const& [k, v] : dy.items()) {
      if (k.isString()) {
        ret.set(OptString{k.getString()}, Variant::fromDynamic(v));
      } else if (k.isInt()) {
        ret.set(k.getInt(), Variant::fromDynamic(v));
      }
    }
    return ret.toVariant();
  }
}

namespace {

template<bool OnlyVanilla>
void vecReleaseWrapper(ArrayData* ad) noexcept {
  // If we are approaching a stack overflow.  Leak the heap object.  The GC can
  // clean it up if it is a problem.
  if (!stack_in_bounds()) return;

  if (OnlyVanilla) {
    VanillaVec::Release(ad);
  } else {
    ad->isVanilla() ? VanillaVec::Release(ad) : BespokeArray::Release(ad);
  }
}
template void vecReleaseWrapper<false>(ArrayData* ad) noexcept;
template void vecReleaseWrapper<true>(ArrayData* ad) noexcept;

template<bool OnlyVanilla>
void dictReleaseWrapper(ArrayData* ad) noexcept {
  // If we are approaching a stack overflow.  Leak the heap object.  The GC can
  // clean it up if it is a problem.
  if (!stack_in_bounds()) return;

  if (OnlyVanilla) {
    VanillaDict::Release(ad);
  } else {
    ad->isVanilla() ? VanillaDict::Release(ad) : BespokeArray::Release(ad);
  }
}
template void dictReleaseWrapper<false>(ArrayData* ad) noexcept;
template void dictReleaseWrapper<true>(ArrayData* ad) noexcept;

template<bool OnlyVanilla>
void keysetReleaseWrapper(ArrayData* ad) noexcept {
  if (OnlyVanilla) {
    VanillaKeyset::Release(ad);
  } else {
    ad->isVanilla() ? VanillaKeyset::Release(ad) : BespokeArray::Release(ad);
  }
}
template void keysetReleaseWrapper<false>(ArrayData* ad) noexcept;
template void keysetReleaseWrapper<true>(ArrayData* ad) noexcept;

void objReleaseWrapper(ObjectData* obj) noexcept {
  // If we are approaching a stack overflow.  Leak the heap object.  The GC can
  // clean it up if it is a problem.
  if (!stack_in_bounds()) return;

  auto const cls = obj->getVMClass();
  cls->releaseFunc()(obj, cls);
}

}

RawDestructors computeDestructors() {
  RawDestructors result;
  for (auto i = 0; i < kDestrTableSize; i++) {
    result[i] = nullptr;
  }
  auto const set = [&](auto const type, auto const destructor) {
    result[typeToDestrIdx(type)] = (RawDestructor)destructor;
    if (use_roar && __roar_api_flag_safe_function_pointer_site) {
      __roar_api_flag_safe_function_pointer_site(
          &result[typeToDestrIdx(type)], reinterpret_cast<void*>(destructor));
    }
  };
  set(KindOfVec,      &vecReleaseWrapper<false>);
  set(KindOfDict,     &dictReleaseWrapper<false>);
  set(KindOfKeyset,   &keysetReleaseWrapper<false>);
  set(KindOfString,   getMethodPtr(&StringData::release));
  set(KindOfObject,   &objReleaseWrapper);
  set(KindOfResource, getMethodPtr(&ResourceHdr::release));
  set(KindOfRClsMeth, getMethodPtr(&RClsMethData::release));
  set(KindOfRFunc,    getMethodPtr(&RFuncData::release));
#define DT(name, ...)                                       \
  assertx(IMPLIES(isRefcountedType(KindOf##name),           \
          result[typeToDestrIdx(KindOf##name)] != nullptr));
#undef DT
  return result;
}

RawDestructors g_destructors = computeDestructors();

void specializeVanillaDestructors() {
  auto const specialize = [](auto const type, auto const destructor) {
    if (allowBespokeArrayLikes() && arrayTypeCouldBeBespoke(type)) return;
    g_destructors[typeToDestrIdx(type)] = (RawDestructor)destructor;
    if (use_roar && __roar_api_flag_safe_function_pointer_site) {
      __roar_api_flag_safe_function_pointer_site(
          &g_destructors[typeToDestrIdx(type)],
          reinterpret_cast<void*>(destructor));
    }
  };
  specialize(KindOfVec,    &vecReleaseWrapper<true>);
  specialize(KindOfDict,   &dictReleaseWrapper<true>);
  specialize(KindOfKeyset, &keysetReleaseWrapper<true>);
}

#define IMPLEMENT_SET(argType, setOp)                     \
  void Variant::set(argType v) noexcept {                 \
    if (isPrimitive()) {                                  \
      setOp;                                              \
    } else {                                              \
      auto const old = *asTypedValue();                   \
      setOp;                                              \
      tvDecRefCountable(old);                             \
    }                                                     \
  }                                                       \
  void variant_ref::set(argType v) noexcept {             \
    Value& m_data = val(m_val);                           \
    DataType& m_type = type(m_val);                       \
    if (isPrimitive()) {                                  \
      setOp;                                              \
    } else {                                              \
      auto const old = m_val.tv();                        \
      setOp;                                              \
      tvDecRefCountable(old);                             \
    }                                                     \
  }

IMPLEMENT_SET(bool, m_type = KindOfBoolean; m_data.num = v)
IMPLEMENT_SET(int, m_type = KindOfInt64; m_data.num = v)
IMPLEMENT_SET(int64_t, m_type = KindOfInt64; m_data.num = v)
IMPLEMENT_SET(double, m_type = KindOfDouble; m_data.dbl = v)
IMPLEMENT_SET(const StaticString&,
              StringData* s = v.get();
              assertx(s);
              m_type = KindOfString;
              m_data.pstr = s)

#undef IMPLEMENT_SET

#define IMPLEMENT_PTR_SET(ptr, member, dtype)                           \
  void Variant::set(ptr *v) noexcept {                                  \
    if (UNLIKELY(!v)) {                                                 \
      this->setNull();                                                  \
    } else {                                                            \
      v->incRefCount();                                                 \
      const TypedValue old = *this->asTypedValue();                     \
      this->m_type = dtype;                                             \
      this->m_data.member = v;                                          \
      tvDecRefGen(old);                                                 \
    }                                                                   \
  }                                                                     \
  void variant_ref::set(ptr *v) noexcept {                              \
    if (UNLIKELY(!v)) {                                                 \
      this->setNull();                                                  \
    } else {                                                            \
      v->incRefCount();                                                 \
      const TypedValue old = this->m_val.tv();                          \
      type(this->m_val) = dtype;                                        \
      val(this->m_val).member = v;                                      \
      tvDecRefGen(old);                                                 \
    }                                                                   \
  }

IMPLEMENT_PTR_SET(StringData, pstr, KindOfString)
IMPLEMENT_PTR_SET(ArrayData, parr, v->toDataType())
IMPLEMENT_PTR_SET(ObjectData, pobj, KindOfObject)
IMPLEMENT_PTR_SET(ResourceHdr, pres, KindOfResource)

#undef IMPLEMENT_PTR_SET

#define IMPLEMENT_STEAL(ptr, member, dtype)                             \
  void Variant::steal(ptr* v) noexcept {                                \
    if (UNLIKELY(!v)) {                                                 \
      this->setNull();                                                  \
    } else {                                                            \
      const TypedValue old = *this->asTypedValue();                     \
      this->m_type = dtype;                                             \
      this->m_data.member = v;                                          \
      tvDecRefGen(old);                                                 \
    }                                                                   \
  }                                                                     \
  void variant_ref::steal(ptr* v) noexcept {                            \
    if (UNLIKELY(!v)) {                                                 \
      this->setNull();                                                  \
    } else {                                                            \
      const TypedValue old = this->m_val.tv();                          \
      type(this->m_val) = dtype;                                        \
      val(this->m_val).member = v;                                      \
      tvDecRefGen(old);                                                 \
    }                                                                   \
  }

IMPLEMENT_STEAL(StringData, pstr, KindOfString)
IMPLEMENT_STEAL(ArrayData, parr, v->toDataType())
IMPLEMENT_STEAL(ObjectData, pobj, KindOfObject)
IMPLEMENT_STEAL(ResourceHdr, pres, KindOfResource)

#undef IMPLEMENT_STEAL

///////////////////////////////////////////////////////////////////////////////
// informational

bool Variant::isNumeric(bool checkString /* = false */) const noexcept {
  int64_t ival;
  double dval;
  DataType t = toNumeric(ival, dval, checkString);
  return t == KindOfInt64 || t == KindOfDouble;
}

DataType Variant::toNumeric(int64_t &ival, double &dval,
                            bool checkString /* = false */) const {
  switch (m_type) {
    case KindOfUninit:
    case KindOfNull:
    case KindOfBoolean:
    case KindOfVec:
    case KindOfDict:
    case KindOfKeyset:
    case KindOfObject:
    case KindOfResource:
    case KindOfRFunc:
    case KindOfFunc:
    case KindOfClass:
    case KindOfLazyClass:
    case KindOfClsMeth:
    case KindOfRClsMeth:
    case KindOfEnumClassLabel:
      return m_type;

    case KindOfInt64:
      ival = m_data.num;
      return KindOfInt64;

    case KindOfDouble:
      dval = m_data.dbl;
      return KindOfDouble;

    case KindOfString:
      return checkString ? m_data.pstr->toNumeric(ival, dval) : m_type;
  }
  not_reached();
}

bool Variant::isScalar() const noexcept {
  switch (getType()) {
    case KindOfUninit:
    case KindOfNull:
    case KindOfVec:
    case KindOfDict:
    case KindOfKeyset:
    case KindOfObject:
    case KindOfResource:
    case KindOfClsMeth:
    case KindOfRClsMeth:
    case KindOfRFunc:
      return false;

    case KindOfBoolean:
    case KindOfInt64:
    case KindOfDouble:
    case KindOfString:
    case KindOfFunc:
    case KindOfClass:
    case KindOfLazyClass:
    case KindOfEnumClassLabel:
      return true;
  }
  not_reached();
}

static Variant::AllowedAsConstantValue isAllowedAsConstantValueImpl(TypedValue tv) {
  switch (tv.m_type) {
    case KindOfNull:
    case KindOfBoolean:
    case KindOfInt64:
    case KindOfDouble:
    case KindOfString:
    case KindOfKeyset:
    case KindOfFunc:
    case KindOfClsMeth:
    case KindOfLazyClass:
      return Variant::AllowedAsConstantValue::Allowed;

    case KindOfVec:
    case KindOfDict: {
      auto allowed = Variant::AllowedAsConstantValue::Allowed;
      if (!tv.m_data.parr->isRefCounted()) return allowed;

      IterateV(tv.m_data.parr, [&] (TypedValue v) {
        switch (isAllowedAsConstantValueImpl(v)) {
          case Variant::AllowedAsConstantValue::NotAllowed: {
            allowed = Variant::AllowedAsConstantValue::NotAllowed;
            break;
          }
          case Variant::AllowedAsConstantValue::ContainsObject: {
            allowed = Variant::AllowedAsConstantValue::ContainsObject;
            break;
          }
          case Variant::AllowedAsConstantValue::Allowed: {
            break;
          }
        }
        return (allowed != Variant::AllowedAsConstantValue::NotAllowed);
       });

      return allowed;
    }

    case KindOfObject:
      return Variant::AllowedAsConstantValue::ContainsObject;

    case KindOfUninit:
    case KindOfClass:
    case KindOfRFunc:
    case KindOfRClsMeth:
    case KindOfResource:
    case KindOfEnumClassLabel:
      return Variant::AllowedAsConstantValue::NotAllowed;
  }
  not_reached();
}

Variant::AllowedAsConstantValue Variant::isAllowedAsConstantValue() const {
  return isAllowedAsConstantValueImpl(*this);
}

///////////////////////////////////////////////////////////////////////////////
// type conversions

bool Variant::toBooleanHelper() const {
  switch (m_type) {
    case KindOfUninit:
    case KindOfNull:
    case KindOfBoolean:
    case KindOfInt64:         assertx(false); return m_data.num;
    case KindOfDouble:        return m_data.dbl != 0;
    case KindOfString:        return m_data.pstr->toBoolean();
    case KindOfVec:
    case KindOfDict:
    case KindOfKeyset:        return !m_data.parr->empty();
    case KindOfObject:        return m_data.pobj->toBoolean();
    case KindOfResource:
    case KindOfRFunc:
    case KindOfEnumClassLabel:
    case KindOfFunc:
    case KindOfClass:
    case KindOfClsMeth:
    case KindOfRClsMeth:
    case KindOfLazyClass:        return true;
  }
  not_reached();
}

int64_t Variant::toInt64Helper(int base /* = 10 */) const {
  auto const op = "int conversion";
  switch (m_type) {
    case KindOfUninit:
    case KindOfNull:
    case KindOfBoolean:
    case KindOfInt64:         assertx(false); return m_data.num;
    case KindOfDouble:        return double_to_int64(m_data.dbl);
    case KindOfString:        return m_data.pstr->toInt64(base);
    case KindOfVec:
    case KindOfDict:
    case KindOfKeyset:        return m_data.parr->empty() ? 0 : 1;
    case KindOfObject:        return m_data.pobj->toInt64();
    case KindOfResource:      return m_data.pres->data()->o_toInt64();
    case KindOfRFunc:
      throw_convert_rfunc_to_type("int");
      return 1;
    case KindOfFunc:
      invalidFuncConversion("int");
    case KindOfClass:
      return classToStringHelper(m_data.pclass, op)->toInt64();
    case KindOfLazyClass:
      return lazyClassToStringHelper(m_data.plazyclass, op)->toInt64();
    case KindOfClsMeth:
      throwInvalidClsMethToType("int");
    case KindOfRClsMeth:
      throw_convert_rcls_meth_to_type("int");
    case KindOfEnumClassLabel:
      throw_convert_ecl_to_type("int");
  }
  not_reached();
}

Array Variant::toPHPArrayHelper() const {
  auto const op = "PHPArray conversion";
  switch (m_type) {
    case KindOfUninit:
    case KindOfNull:          return empty_dict_array();

    // These scalars all get converted into single-element arrays.
    case KindOfBoolean:
    case KindOfInt64:
    case KindOfDouble:
    case KindOfString:
      return make_dict_array(0, *this);

    case KindOfVec:
    case KindOfDict:
    case KindOfKeyset:        return ArrNR{m_data.parr}.asArray().toDict();
    case KindOfObject:        return m_data.pobj->toArray().toDict();
    case KindOfResource:      return m_data.pres->data()->o_toArray();
    case KindOfRFunc:
      SystemLib::throwInvalidOperationExceptionObject("RFunc to PHPArray conversion");
      return empty_dict_array();
    case KindOfFunc:
      invalidFuncConversion("array");
    case KindOfClass: {
      auto const str = classToStringHelper(m_data.pclass, op);
      return make_dict_array(0, Variant{str, PersistentStrInit{}});
    }
    case KindOfLazyClass: {
      auto const str = lazyClassToStringHelper(m_data.plazyclass, op);
      return make_dict_array(0, Variant{str, PersistentStrInit{}});
    }
    case KindOfClsMeth:
      throwInvalidClsMethToType("array");
      return empty_dict_array();
    case KindOfRClsMeth:
      SystemLib::throwInvalidOperationExceptionObject(
        "RClsMeth to PHPArray conversion");
      return empty_dict_array();
    case KindOfEnumClassLabel:
      throw_convert_ecl_to_type("PHPArray");
      return empty_dict_array();
  }
  not_reached();
}

OptResource Variant::toResourceHelper() const {
  switch (m_type) {
    case KindOfUninit:
    case KindOfNull:
    case KindOfBoolean:
    case KindOfInt64:
    case KindOfDouble:
    case KindOfString:
    case KindOfVec:
    case KindOfDict:
    case KindOfKeyset:
    case KindOfObject:
    case KindOfRFunc:
    case KindOfFunc:
    case KindOfClass:
    case KindOfLazyClass:
    case KindOfClsMeth:
    case KindOfRClsMeth:
    case KindOfEnumClassLabel:
      return OptResource(req::make<DummyResource>());

    case KindOfResource:
      return OptResource{m_data.pres};
  }
  not_reached();
}

req::root<Variant>* blackHolePtr() {
  void* p = bhStorage.get();
  return reinterpret_cast<req::root<Variant>*>(p);
}

void initBlackHole() {
  new (blackHolePtr()) req::root<Variant>();
}

void clearBlackHole() {
  using req::root;
  blackHolePtr()->~root<Variant>();
}

Variant& lvalBlackHole() {
  blackHolePtr()->unset();
  blackHolePtr()->setNull();
  return *blackHolePtr();
}

void Variant::setEvalScalar() {
  assertx(tvIsPlausible(*this));

  auto const do_array = [this]{
    if (!m_data.parr->isStatic()) {
      ArrayData::GetScalarArray(&m_data.parr);
      assertx(m_data.parr->isStatic());
    }
  };

  switch (m_type) {
    case KindOfUninit:
    case KindOfNull:
    case KindOfBoolean:
    case KindOfInt64:
    case KindOfDouble:
    case KindOfLazyClass:
    case KindOfEnumClassLabel:
      return;

    case KindOfString: {
      auto pstr = m_data.pstr;
      if (!pstr->isStatic()) {
        StringData *sd = makeStaticString(pstr);
        decRefStr(pstr);
        m_data.pstr = sd;
        assertx(m_data.pstr->isStatic());
      }
      return;
    }

    case KindOfVec:
    case KindOfDict:
    case KindOfKeyset:
      do_array();
      return;

    case KindOfFunc:
      if (m_data.pfunc->isPersistent()) return;
      break;
    case KindOfClass:
      if (m_data.pclass->isPersistent()) return;
      break;

    case KindOfObject:
    case KindOfResource:
      break;

    case KindOfRFunc:
      raise_error(Strings::RFUNC_NOT_SUPPORTED);

    case KindOfRClsMeth:
      raise_error(Strings::RCLS_METH_NOT_SUPPORTED);

    case KindOfClsMeth:
      if (m_data.pclsmeth->isPersistent()) return;
      raise_error(Strings::CLS_METH_NOT_SUPPORTED);
  }
  not_reached();
}

VarNR::VarNR(StringData *v) {
  if (v) {
    m_type = KindOfString;
    m_data.pstr = v;
  } else {
    m_type = KindOfNull;
  }
}

VarNR::VarNR(ArrayData *v) {
  if (v) {
    m_type = v->toDataType();
    m_data.parr = v;
  } else {
    m_type = KindOfNull;
  }
}

VarNR::VarNR(ObjectData *v) {
  if (v) {
    m_type = KindOfObject;
    m_data.pobj = v;
  } else {
    m_type = KindOfNull;
  }
}

///////////////////////////////////////////////////////////////////////////////
}
