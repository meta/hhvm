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
#include "hphp/runtime/base/tv-comparisons.h"

#include <folly/Random.h>
#include <type_traits>

#include "hphp/runtime/base/comparisons.h"
#include "hphp/runtime/base/datatype.h"
#include "hphp/runtime/base/opaque-resource.h"
#include "hphp/runtime/base/tv-conversions.h"
#include "hphp/runtime/base/tv-conv-throw.h"
#include "hphp/runtime/base/vanilla-dict.h"
#include "hphp/runtime/base/vanilla-keyset.h"
#include "hphp/runtime/base/vanilla-vec.h"
#include "hphp/runtime/ext/datetime/ext_datetime.h"
#include "hphp/runtime/vm/class-meth-data-ref.h"
#include "hphp/runtime/vm/rclass-meth-data.h"

#include "hphp/util/random.h"

namespace HPHP {

//////////////////////////////////////////////////////////////////////
namespace collections {
extern bool equals(const ObjectData*, const ObjectData*);
}
//////////////////////////////////////////////////////////////////////

namespace {

//////////////////////////////////////////////////////////////////////

/*
 * Strict equality check between two vecs, accounting for bespoke vecs.
 */
bool vecSameHelper(const ArrayData* ad1, const ArrayData* ad2) {
  assertx(ad1->isVecType());
  assertx(ad2->isVecType());
  return ArrayData::bothVanilla(ad1, ad2)
    ? VanillaVec::VecSame(ad1, ad2)
    : ArrayData::Same(ad1, ad2);
}

bool isStringOrClassish(DataType t) {
  return isStringType(t) || isClassType(t) || isLazyClassType(t);
}

const StringData* convStringishToStringData(TypedValue cell, bool warn = true) {
  assertx(isStringOrClassish(cell.type()));
  auto const op = "comparison";
  if (tvIsClass(cell)) {
    return warn
      ? classToStringHelper(cell.m_data.pclass, op)
      : cell.m_data.pclass->name();
  }
  if (tvIsLazyClass(cell)) {
    return warn
      ? lazyClassToStringHelper(cell.m_data.plazyclass, op)
      : cell.m_data.plazyclass.name();
  }
  return cell.m_data.pstr;
}

/*
 * Return whether two DataTypes for primitive types are "equivalent" including
 * testing for in-progress migrations
 */
bool equivDataTypesIncludingMigrations(DataType t1, DataType t2) {
  return equivDataTypes(t1, t2) ||
          (isStringOrClassish(t1) && isStringOrClassish(t2));
}

struct Eq;

template<class Op>
typename Op::RetType tvRelOp(Op op, TypedValue c1, TypedValue c2) {
  assertx(tvIsPlausible(c1));
  assertx(tvIsPlausible(c2));
  assertx(IMPLIES((std::is_same_v<Op, Eq>), equivDataTypesIncludingMigrations(c1.m_type, c2.m_type)));

  if (UNLIKELY(!(equivDataTypesIncludingMigrations(c1.m_type, c2.m_type) ||
      (tvIsInt(c1) && tvIsDouble(c2)) || (tvIsDouble(c1) && tvIsInt(c2))))) {
    throwCmpBadTypesException(&c1, &c2);
  }

  switch (c2.m_type) {
    case KindOfUninit:
    case KindOfNull:         return op(false, false);
    case KindOfBoolean:      return op(!!c1.m_data.num, !!c2.m_data.num);
    case KindOfInt64:        return tvIsInt(c1)
                                      ? op(c1.m_data.num, c2.m_data.num)
                                      : op(c1.m_data.dbl, c2.m_data.num);
    case KindOfDouble:       return tvIsInt(c1)
                                      ? op(c1.m_data.num, c2.m_data.dbl)
                                      : op(c1.m_data.dbl, c2.m_data.dbl);
    case KindOfPersistentString:
    case KindOfString:       return op(convStringishToStringData(c1), c2.m_data.pstr);
    case KindOfClass:        return op(c1, c2.m_data.pclass);
    case KindOfLazyClass:    return op(c1, c2.m_data.plazyclass);
    case KindOfPersistentVec:
    case KindOfVec:
    case KindOfPersistentDict:
    case KindOfDict:
    case KindOfPersistentKeyset:
    case KindOfKeyset:       return op(c1.m_data.parr, c2.m_data.parr);
    case KindOfObject:       return op(c1.m_data.pobj, c2.m_data.pobj);
    case KindOfResource:     return op(c1.m_data.pres, c2.m_data.pres);
    case KindOfEnumClassLabel: return op(ECLString{c1.m_data.pstr},
                                         ECLString{c2.m_data.pstr});
    case KindOfRFunc:        return op(c1.m_data.prfunc, c2.m_data.prfunc);
    case KindOfFunc:         return op(c1.m_data.pfunc, c2.m_data.pfunc);
    case KindOfClsMeth:      return op(c1.m_data.pclsmeth, c2.m_data.pclsmeth);
    case KindOfRClsMeth:     return op(c1.m_data.prclsmeth, c2.m_data.prclsmeth);
  }
  not_reached();
}

/*
 * These relative ops helper function objects define operator() for
 * each primitive type, and for the case of a complex type being
 * compared with itself (that is obj with obj, string with string,
 * array with array).
 */

template<typename T>
using remove_ptr_const = std::remove_const_t<std::remove_pointer_t<T>>*;

template<class T1, class T2, class RetType>
using enable_if_same_ptr_ty = typename std::enable_if_t<
  std::is_pointer_v<T1> && std::is_pointer_v<T2> &&
  std::is_same_v<remove_ptr_const<T1>, remove_ptr_const<T2>>,
  RetType
>;

struct Eq {
  using RetType = bool;

  template<class T, class U>
  std::enable_if_t<!std::is_pointer_v<T> && !std::is_pointer_v<U>, bool>
  operator()(T t, U u) const { return t == u; }

  template<class T1, class T2>
  enable_if_same_ptr_ty<T1, T2, bool> operator()(T1 t1, T2 t2) const {
    using U = remove_ptr_const<T1>;
    if constexpr (std::is_same_v<U, Func*>)         return t1 == t2;
    if constexpr (std::is_same_v<U, ResourceHdr*>)  return t1 == t2;
    if constexpr (std::is_same_v<U, StringData*>)   return t1->equal(t2);
    if constexpr (std::is_same_v<U, ObjectData*>)   return t1->equal(*t2);
    if constexpr (std::is_same_v<U, RFuncData*>)    return RFuncData::Same(t1, t2);
    if constexpr (std::is_same_v<U, RClsMethData*>) return RClsMethData::Same(t1, t2);
    if constexpr (std::is_same_v<U, ArrayData*>) {
      if (t1->toDataType() != t2->toDataType()) return false;
      switch (t1->toDataType()) {
        case KindOfVec:    return arrImpl<VanillaVec::VecEqual>(t1, t2);
        case KindOfDict:   return arrImpl<VanillaDict::DictEqual>(t1, t2);
        case KindOfKeyset: return arrImpl<VanillaKeyset::Equal>(t1, t2);
        default: always_assert(false);
      }
    }
  }

  bool eqStringishTypes(TypedValue lhs, const StringData* rhs) const {
    assertx(tvIsLazyClass(lhs) || tvIsString(lhs));
    if (tvIsLazyClass(lhs)) return lhs.m_data.plazyclass.name() == rhs;
    if (folly::Random::oneIn(Cfg::Eval::RaiseClassConversionNoticeSampleRate, threadLocalRng64())) {
      raise_class_to_string_conversion_notice("comparison");
    }
    return lhs.m_data.pstr->equal(rhs);
  }

  bool operator()(TypedValue t, LazyClassData u) const {
    if (tvIsClass(t)) return t.m_data.pclass->name() == u.name();
    return eqStringishTypes(t, u.name());
  }
  bool operator()(TypedValue t, Class* u) const {
    if (tvIsClass(t)) return t.m_data.pclass == u;
    return eqStringishTypes(t, u->name());
  }

private:
  template<bool (*vanillaCmp)(const ArrayData* ad1, const ArrayData* ad2)>
  bool arrImpl(const ArrayData* ad1, const ArrayData* ad2) const {
    return ArrayData::bothVanilla(ad1, ad2)
      ? vanillaCmp(ad1, ad2)
      : ArrayData::Equal(ad1, ad2);
  }
};

template<
  class RetTy,
  class PrimitiveCmpOp,
  RetTy (ObjectData::*objCmp)(const ObjectData&) const,
  RetTy (*arrCmp)(const ArrayData*, const ArrayData*)>
struct CompareBase {
  using RetType = RetTy;

  template<class T, class U>
  std::enable_if_t<!std::is_pointer_v<T> && !std::is_pointer_v<U>, RetType>
  operator()(T t, U u) const { return PrimitiveCmpOp()(t, u); }

  template<class T1, class T2>
  enable_if_same_ptr_ty<T1, T2, RetType> operator()(T1 t1, T2 t2) const {
    using U = remove_ptr_const<T1>;
    if constexpr (std::is_same_v<U, ResourceHdr*>) {
      if (t1->data()->template instanceof<OpaqueResource>() ||
          t2->data()->template instanceof<OpaqueResource>()) {
        throw_opaque_resource_compare_exception();
      }
      return operator()(t1->data()->o_toInt64(), t2->data()->o_toInt64());
    }
    if constexpr (std::is_same_v<U, StringData*>)   return operator()(t1->compare(t2), 0);
    if constexpr (std::is_same_v<U, ObjectData*>)   return (t1->*objCmp)(*t2);
    if constexpr (std::is_same_v<U, Func*>)         throw_func_compare_exception();
    if constexpr (std::is_same_v<U, RFuncData*>)    throw_rfunc_compare_exception();
    if constexpr (std::is_same_v<U, RClsMethData*>) throw_rclsmeth_compare_exception();
    if constexpr (std::is_same_v<U, ArrayData*>) {
      assertx(t1->toDataType() == t2->toDataType());
      switch (t1->toDataType()) {
        case KindOfVec:    return arrCmp(t1, t2);
        case KindOfDict:   throw_dict_compare_exception();
        case KindOfKeyset: throw_keyset_compare_exception();
        default: always_assert(false);
      }
    }
  }

  RetType operator()(ClsMethDataRef c1, ClsMethDataRef c2) const {
    throw_clsmeth_compare_exception();
  }

  RetType operator()(ECLString c1, ECLString c2) const {
    throw_ecl_compare_exception();
  }

  bool operator()(TypedValue t, LazyClassData u) const {
    assertx(isStringOrClassish(t.type()));
    // this seems like an oversight?
    if (tvIsLazyClass(t)) return operator()(t.m_data.plazyclass.name(), u.name());
    return operator()(convStringishToStringData(t),
                      lazyClassToStringHelper(u, "comparison"));
  }
  bool operator()(TypedValue t, Class* u) const {
    assertx(isStringOrClassish(t.type()));
    return operator()(convStringishToStringData(t),
                      classToStringHelper(u, "comparison"));
  }
};

struct PHPPrimitiveCmp {
  template<class T, class U>
  constexpr int64_t operator()(const T& t, const U& u) const {
    // This ordering is required so that -1 is returned for NaNs (to match PHP7
    // behavior).
    return (t == u) ? 0 : ((t > u) ? 1 : -1);
  }
};

using Lt  = CompareBase<bool, std::less<>, &ObjectData::less, ArrayData::Lt>;
using Lte = CompareBase<bool, std::less_equal<>, &ObjectData::lessEqual, ArrayData::Lte>;
using Gt  = CompareBase<bool, std::greater<>, &ObjectData::more, ArrayData::Gt>;
using Gte = CompareBase<bool, std::greater_equal<>, &ObjectData::moreEqual, ArrayData::Gte>;
using Cmp = CompareBase<int64_t, struct PHPPrimitiveCmp, &ObjectData::compare, ArrayData::Compare>;

}

bool tvSame(TypedValue c1, TypedValue c2) {
  assertx(tvIsPlausible(c1));
  assertx(tvIsPlausible(c2));

  bool const null1 = isNullType(c1.m_type);
  bool const null2 = isNullType(c2.m_type);
  if (null1 && null2) return true;
  if (null1 || null2) return false;

  if (!equivDataTypesIncludingMigrations(c1.m_type, c2.m_type)) return false;

  switch (c1.m_type) {
    case KindOfBoolean:
    case KindOfInt64:
      return c1.m_data.num == c2.m_data.num;
    case KindOfDouble:
      return c1.m_data.dbl == c2.m_data.dbl;
    case KindOfPersistentString:
    case KindOfString:
      return c1.m_data.pstr->same(convStringishToStringData(c2));
    case KindOfRFunc:
      return RFuncData::Same(c1.m_data.prfunc, c2.m_data.prfunc);
    case KindOfFunc:
      return c1.m_data.pfunc == c2.m_data.pfunc;
    case KindOfClass:
      if (tvIsClass(c2)) return c1.m_data.pclass == c2.m_data.pclass;
      [[fallthrough]];
    case KindOfLazyClass: {
      const auto warn_on_conv = tvIsString(c2);
      return convStringishToStringData(c1, warn_on_conv)
              ->same(convStringishToStringData(c2, warn_on_conv));
    }
    case KindOfPersistentVec:
    case KindOfVec:
      return vecSameHelper(c1.m_data.parr, c2.m_data.parr);
    case KindOfPersistentDict:
    case KindOfDict: {
      auto const ad1 = c1.m_data.parr;
      auto const ad2 = c2.m_data.parr;
      return ArrayData::bothVanilla(ad1, ad2)
        ? VanillaDict::DictSame(ad1, ad2)
        : ArrayData::Same(ad1, ad2);
    }
    case KindOfPersistentKeyset:
    case KindOfKeyset: {
      auto const ad1 = c1.m_data.parr;
      auto const ad2 = c2.m_data.parr;
      return ArrayData::bothVanilla(ad1, ad2)
        ? VanillaKeyset::Same(ad1, ad2)
        : ArrayData::Same(ad1, ad2);
    }
    case KindOfObject:
      return c1.m_data.pobj == c2.m_data.pobj;
    case KindOfResource:
     return c1.m_data.pres == c2.m_data.pres;
    case KindOfEnumClassLabel:
      return c1.m_data.pstr == c2.m_data.pstr;
    case KindOfClsMeth:
      return c1.m_data.pclsmeth == c2.m_data.pclsmeth;
    case KindOfRClsMeth:
      return RClsMethData::Same(c1.m_data.prclsmeth, c2.m_data.prclsmeth);
    case KindOfUninit:
    case KindOfNull:
      break;
  }
  not_reached();
}

//////////////////////////////////////////////////////////////////////

namespace {

template <DataType DT>
void checkForBadComparison(TypedValue cell) {
 auto check = [cell]() {
    switch (DT) {
      case DataType::Boolean:  return tvIsBool(cell);
      case DataType::Int64:    return tvIsInt(cell);
      case DataType::Double:   return tvIsDouble(cell);
      case DataType::String:   return isStringOrClassish(cell.type());
      case DataType::Resource: return tvIsResource(cell);
      case DataType::ClsMeth:  return tvIsClsMeth(cell);
      case DataType::Vec:      return tvIsVec(cell);
      case DataType::Dict:     return tvIsDict(cell);
      case DataType::Keyset:   return tvIsKeyset(cell);
    }
  }();
  if (!check) throwCmpBadTypesException(&cell, DT);
}

void checkForBadComparisonObj(TypedValue lhs, const ObjectData* rhs) {
  if (tvIsObject(lhs)) return;
  const auto rhsStr = make_tv<DataType::Object>(const_cast<ObjectData*>(rhs));
  throwCmpBadTypesException(&lhs, &rhsStr);
}

}

bool tvEqual(TypedValue cell, bool val) {
  if (!tvIsBool(cell)) return false;
  return val == cell.m_data.num;
}

bool tvEqual(TypedValue cell, int64_t val) {
  if (!tvIsInt(cell)) return false;
  return val == cell.m_data.num;
}

bool tvEqual(TypedValue cell, double val) {
  if (!tvIsDouble(cell)) return false;
  return val == cell.m_data.dbl;
}

bool tvEqual(TypedValue cell, const StringData* val) {
  if (!isStringOrClassish(cell.type())) return false;
  return convStringishToStringData(cell)->equal(val);
}

bool tvEqual(TypedValue cell, const ArrayData* val) {
  if (!tvIsArrayLike(cell)) return false;
  return Eq()(cell.m_data.parr, val);
}

bool tvEqual(TypedValue cell, const ObjectData* val) {
  if (!tvIsObject(cell)) return false;
  return Eq()(cell.m_data.pobj, val);
}

bool tvEqual(TypedValue c1, TypedValue c2) {
  if (!equivDataTypesIncludingMigrations(c1.m_type, c2.m_type)) return false;
  return tvRelOp(Eq(), c1, c2);
}

template<class Op>
typename Op::RetType tvRelOpArr(TypedValue cell, const ArrayData* val) {
  if (val->isVecType()) checkForBadComparison<DataType::Vec>(cell);
  else if (val->isDictType()) checkForBadComparison<DataType::Dict>(cell);
  else checkForBadComparison<DataType::Keyset>(cell);

  return Op()(cell.m_data.parr, val);
}

bool tvLess(TypedValue cell, bool val) {
  checkForBadComparison<DataType::Boolean>(cell);
  return cell.m_data.num < val;
}

bool tvLess(TypedValue cell, int64_t val) {
  if (tvIsDouble(cell)) return cell.m_data.dbl < val;
  checkForBadComparison<DataType::Int64>(cell);
  return cell.m_data.num < val;
}

bool tvLess(TypedValue cell, double val) {
  if (tvIsInt(cell)) return cell.m_data.num < val;
  checkForBadComparison<DataType::Double>(cell);
  return cell.m_data.dbl < val;
}

bool tvLess(TypedValue cell, const StringData* val) {
  checkForBadComparison<DataType::String>(cell);
  return Lt()(convStringishToStringData(cell), val);
}

bool tvLess(TypedValue cell, const ArrayData* val) {
  return tvRelOpArr<Lt>(cell, val);
}

bool tvLess(TypedValue cell, const ObjectData* val) {
  checkForBadComparisonObj(cell, val);
  return Lt()(cell.m_data.pobj, val);
}

bool tvLess(TypedValue tv1, TypedValue tv2) {
  return tvRelOp(Lt(), tv1, tv2);
}

bool tvGreater(TypedValue cell, bool val) {
  checkForBadComparison<DataType::Boolean>(cell);
  return cell.m_data.num > val;
}

bool tvGreater(TypedValue cell, int64_t val) {
  if (tvIsDouble(cell)) return cell.m_data.dbl > val;
  checkForBadComparison<DataType::Int64>(cell);
  return cell.m_data.num > val;
}

bool tvGreater(TypedValue cell, double val) {
  if (tvIsInt(cell)) return cell.m_data.num > val;
  checkForBadComparison<DataType::Double>(cell);
  return cell.m_data.dbl > val;
}

bool tvGreater(TypedValue cell, const StringData* val) {
  checkForBadComparison<DataType::String>(cell);
  return Gt()(convStringishToStringData(cell), val);
}

bool tvGreater(TypedValue cell, const ArrayData* val) {
  return tvRelOpArr<Gt>(cell, val);
}

bool tvGreater(TypedValue cell, const ObjectData* val) {
  checkForBadComparisonObj(cell, val);
  return Gt()(cell.m_data.pobj, val);
}

bool tvGreater(TypedValue tv1, TypedValue tv2) {
  return tvRelOp(Gt(), tv1, tv2);
}

//////////////////////////////////////////////////////////////////////

int64_t tvCompare(TypedValue cell, bool val) {
  checkForBadComparison<DataType::Boolean>(cell);
  return Cmp()(cell.m_data.num, val);
}

int64_t tvCompare(TypedValue cell, int64_t val) {
  if (tvIsDouble(cell)) return Cmp()(cell.m_data.dbl, val);
  checkForBadComparison<DataType::Int64>(cell);
  return Cmp()(cell.m_data.num, val);
}

int64_t tvCompare(TypedValue cell, double val) {
  if (tvIsInt(cell)) return Cmp()(cell.m_data.num, val);
  checkForBadComparison<DataType::Double>(cell);
  return Cmp()(cell.m_data.dbl, val);
}

int64_t tvCompare(TypedValue cell, const StringData* val) {
  checkForBadComparison<DataType::String>(cell);
  return Cmp()(convStringishToStringData(cell), val);
}

int64_t tvCompare(TypedValue cell, const ArrayData* val) {
  return tvRelOpArr<Cmp>(cell, val);
}

int64_t tvCompare(TypedValue cell, const ObjectData* val) {
  checkForBadComparisonObj(cell, val);
  return cell.m_data.pobj->compare(*val);
}

int64_t tvCompare(TypedValue tv1, TypedValue tv2) {
  return tvRelOp(Cmp(), tv1, tv2);
}

//////////////////////////////////////////////////////////////////////

bool tvLessOrEqual(TypedValue c1, TypedValue c2) {
  assertx(tvIsPlausible(c1));
  assertx(tvIsPlausible(c2));
  return tvRelOp(Lte(), c1, c2);
}

bool tvGreaterOrEqual(TypedValue c1, TypedValue c2) {
  assertx(tvIsPlausible(c1));
  assertx(tvIsPlausible(c2));
  return tvRelOp(Gte(), c1, c2);
}

//////////////////////////////////////////////////////////////////////

}
