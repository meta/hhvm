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
#include "hphp/runtime/base/tv-arith.h"

#include <type_traits>
#include <limits>
#include <algorithm>

#include <fmt/format.h>
#include <usdt/usdt.h>

#include "hphp/runtime/base/array-data-defs.h"
#include "hphp/runtime/base/double-to-int64.h"
#include "hphp/runtime/base/runtime-error.h"
#include "hphp/runtime/base/tv-conversions.h"
#include "hphp/runtime/base/tv-refcount.h"
#include "hphp/runtime/ext/std/ext_std_math.h"
#include "hphp/runtime/vm/class-meth-data-ref.h"

#include "hphp/util/configs/eval.h"

namespace HPHP {

//////////////////////////////////////////////////////////////////////

namespace {

[[noreturn]] NEVER_INLINE
void throw_bad_array_operand(const ArrayData* ad) {
  const char* type = [&]{
    if (ad->isVecType()) return "vecs";
    if (ad->isDictType()) return "dicts";
    if (ad->isKeysetType()) return "keysets";
    always_assert(false);
  }();
  SystemLib::throwInvalidOperationExceptionObject(
    fmt::format(
      "Invalid operand type was used: "
      "cannot perform this operation with {}", type
    )
  );
}

/*
 * unsigned to signed conversion when the unsigned value is out of
 * range is implementation defined behavior. We can work around it
 * with something like
 *
 *  return v > std::numeric_limits<int64_t>::max() ?
 *             -int64_t(~v) - 1 : int64_t(v);
 *
 * which gcc appears to optimize to a no-op. But I'd rather avoid that
 * until it becomes a problem.
 *
 * Putting this here as a placeholder.
 */
inline int64_t u2s(uint64_t v) {
  return v;
}

inline int64_t add_ignore_overflow(int64_t a, int64_t b) {
  return u2s(static_cast<uint64_t>(a) + b);
}

inline int64_t sub_ignore_overflow(int64_t a, int64_t b) {
  return u2s(static_cast<uint64_t>(a) - b);
}

inline int64_t mul_ignore_overflow(int64_t a, int64_t b) {
  return u2s(static_cast<uint64_t>(a) * b);
}

inline int64_t shl_ignore_overflow(int64_t a, int64_t b) {
  return u2s(static_cast<uint64_t>(a) << (b & 63));
}

TypedValue make_int(int64_t n) { return make_tv<KindOfInt64>(n); }
TypedValue make_dbl(double d)  { return make_tv<KindOfDouble>(d); }

bool is_numeric(TypedValue tv) { return tvIsInt(tv) || tvIsDouble(tv); }

inline void check_numeric(const TypedValue& c1, const TypedValue& c2) {
  if (UNLIKELY(!is_numeric(c1) || !is_numeric(c2))) {
    throwMathBadTypesException(&c1, &c2);
  }
}

template<class Op>
TypedValue tvArith(Op o, TypedValue c1, TypedValue c2) {
  check_numeric(c1, c2);
  if (tvIsInt(c1)) {
    return tvIsInt(c2)
      ? o(c1.m_data.num, c2.m_data.num)
      : o(c1.m_data.num, c2.m_data.dbl);
  }
  return tvIsInt(c2)
    ? o(c1.m_data.dbl, c2.m_data.num)
    : o(c1.m_data.dbl, c2.m_data.dbl);
}

/*
 * Template params:
 * 1. A functor class for doing the standard mathematical operation
 * 2. A fn ptr for doing the mathematical operation ignoring int overflow issues
 * 3. true if the result should be wrapped in a typedvalue or left as a double
 */
template <typename Op, int64_t (*ovrflw)(int64_t a, int64_t b), bool tv_ret>
struct ArithBase {
  template<class T, class U> typename std::enable_if_t<
    std::is_floating_point_v<T> || std::is_floating_point_v<U>,
    std::conditional_t<tv_ret, TypedValue, double>
  > operator()(T a, U b) const {
    if constexpr (tv_ret) return make_dbl(Op()(a, b));
    else return Op()(a, b);
  }

  std::conditional_t<tv_ret, TypedValue, int64_t>
  operator()(int64_t a, int64_t b) const {
    if constexpr (tv_ret) return make_int(ovrflw(a, b));
    else return ovrflw(a, b);
  }
};

using Add = ArithBase<std::plus<>, add_ignore_overflow, true>;
using Sub = ArithBase<std::minus<>, sub_ignore_overflow, true>;
using Mul = ArithBase<std::multiplies<>, mul_ignore_overflow, true>;

using AddEq = ArithBase<std::plus<>, add_ignore_overflow, false>;
using SubEq = ArithBase<std::minus<>, sub_ignore_overflow, false>;
int64_t mul(int64_t a, int64_t b) { return a * b; }
using MulEq = ArithBase<std::multiplies<>, mul, false>;

struct Div {
  TypedValue operator()(int64_t t, int64_t u) const {
    if (UNLIKELY(u == 0)) {
      SystemLib::throwDivisionByZeroExceptionObject();
    }

    // Avoid SIGFPE when dividing the miniumum respresentable integer
    // by -1.
    auto const minInt = std::numeric_limits<int64_t>::min();
    if (UNLIKELY(u == -1 && t == minInt)) {
      return make_dbl(static_cast<double>(minInt) / -1);
    }

    return (t % u == 0) ? make_int(t / u) : make_dbl(double(t) / u);
  }

  template<class T, class U>
  typename std::enable_if<
    std::is_floating_point<T>::value || std::is_floating_point<U>::value,
    TypedValue
  >::type operator()(T t, U u) const {
    if (UNLIKELY(u == 0)) {
      SystemLib::throwDivisionByZeroExceptionObject();
    }
    return make_dbl(t / u);
  }
};

template<class Op>
void tvOpEq(Op op, tv_lval c1, TypedValue c2) {
  check_numeric(*c1, c2);

  if (tvIsDouble(c1)) {
    val(c1).dbl = op(val(c1).dbl, tvIsInt(c2) ? c2.m_data.num : c2.m_data.dbl);
    return;
  }

  assertx(tvIsInt(c1));
  if (tvIsInt(c2)) {
    val(c1).num = op(val(c1).num, c2.m_data.num);
  } else {
    type(c1) = KindOfDouble;
    val(c1).dbl = op(val(c1).num, c2.m_data.dbl);
  }
}

template<class SzOp, class BitOp>
StringData* stringBitOp(BitOp bop, SzOp sop, StringData* s1, StringData* s2) {
  auto const s1Size = s1->size();
  auto const s2Size = s2->size();
  auto const newLen = sop(s1Size, s2Size);
  auto const newStr = StringData::Make(newLen);
  auto const s1Data = s1->data();
  auto const s2Data = s2->data();
  auto const outData = newStr->mutableData();

  for (uint32_t i = 0; i < newLen; ++i) {
    outData[i] = bop((i < s1Size) ? s1Data[i] : 0,
                     (i < s2Size) ? s2Data[i] : 0);
  }
  newStr->setSize(newLen);

  return newStr;
}

void raiseBitwiseOpInvalidType() {
  switch (Cfg::Eval::WarnOnBitwiseOpInvalidType) {
    case 0:
      break;
    case 1:
      raise_warning("Deprecated operand type (string) for bitwise operation");
      break;
    case 2:
      raise_error("Unsupported operand type (string) for bitwise operation");
    default:
      always_assert(false);
  }
}

template<template<class> class BitOp, class StrLenOp>
TypedValue tvBitOp(StrLenOp strLenOp, TypedValue c1, TypedValue c2) {
  assertx(tvIsPlausible(c1));
  assertx(tvIsPlausible(c2));
  if (isStringType(c1.m_type) && isStringType(c2.m_type)) {
    raiseBitwiseOpInvalidType();
    return make_tv<KindOfString>(
      stringBitOp(
        BitOp<char>(),
        strLenOp,
        c1.m_data.pstr,
        c2.m_data.pstr
      )
    );
  }

  if (!tvIsInt(c1) || !tvIsInt(c2)) throwBitOpBadTypesException(&c1, &c2);
  return make_int(BitOp<int64_t>()(c1.m_data.num, c2.m_data.num));
}

template<class Op>
void tvBitOpEq(Op op, tv_lval c1, TypedValue c2) {
  auto const result = op(*c1, c2);
  auto const old = *c1;
  tvCopy(result, c1);
  tvDecRefGen(old);
}

// Op must implement the interface described for cellIncDecOp.
template<class Op>
void stringIncDecOp(Op op, tv_lval cell, StringData* sd) {
  assertx(isStringType(type(cell)) || isFuncType(type(cell)) ||
          isClassType(type(cell)));

  if (sd->empty())     throwIncDecBadTypeException("empty string");
  if (sd->isNumeric()) throwIncDecBadTypeException("numeric string");
  op.nonNumericString(cell);
}

void raiseIncDecInvalidType(tv_lval cell) {
  switch (Cfg::Eval::WarnOnIncDecInvalidType) {
    case 0:
      break;
    case 1:
      raise_warning("Unsupported operand type (%s) for IncDec",
                    describe_actual_type(cell).c_str());
      break;
    case 2:
      raise_error("Unsupported operand type (%s) for IncDec",
                  describe_actual_type(cell).c_str());
    default:
      always_assert(false);
  }
}

/*
 * Inc or Dec for a string, depending on Op.  Op must implement
 *
 *   - a function call operator for numeric types
 *   - a nullCase(TypedValue&) function that returns the result for null types
 *   - an emptyString() function that performs the operation for empty strings
 *   - and a nonNumericString(TypedValue&) function used for non-numeric strings
 *
 * PHP's Inc and Dec behave differently in all these cases, so this
 * abstracts out the common parts from those differences.
 */
template<class Op>
void tvIncDecOp(Op op, tv_lval cell) {
  assertx(tvIsPlausible(*cell));

  auto const source = "increment op";
  switch (type(cell)) {
    case KindOfUninit:
    case KindOfNull:
      raiseIncDecInvalidType(cell);
      op.nullCase(cell);
      return;

    case KindOfInt64:
      op.intCase(cell);
      return;

    case KindOfDouble:
      op.dblCase(cell);
      return;

    case KindOfFunc: {
      raiseIncDecInvalidType(cell);
      invalidFuncConversion("int");
    }

    case KindOfClass: {
      raiseIncDecInvalidType(cell);
      auto s = classToStringHelper(val(cell).pclass, source);
      stringIncDecOp(op, cell, const_cast<StringData*>(s));
      return;
    }

    case KindOfLazyClass: {
      raiseIncDecInvalidType(cell);
      auto s = lazyClassToStringHelper(val(cell).plazyclass, source);
      stringIncDecOp(op, cell, const_cast<StringData*>(s));
      return;
    }

    case KindOfString:
      raiseIncDecInvalidType(cell);
      stringIncDecOp(op, cell, val(cell).pstr);
      return;

    case KindOfBoolean:
    case KindOfVec:
    case KindOfDict:
    case KindOfKeyset:
    case KindOfObject:
    case KindOfResource:
    case KindOfClsMeth:
    case KindOfRClsMeth:
    case KindOfRFunc:
    case KindOfEnumClassLabel:
      raiseIncDecInvalidType(cell);
      return;
  }
  not_reached();
}

const StaticString s_1("1");


struct Inc {
  void intCase(tv_lval cell) const {
    auto& n = val(cell).num;
    n = add_ignore_overflow(n, 1);
  }
  void dblCase(tv_lval cell) const { ++val(cell).dbl; }
  void nullCase(tv_lval cell) const {
    throwIncDecBadTypeException("null");
    not_reached();
  }

  void nonNumericString(tv_lval cell) const {
    auto const sd = val(cell).pstr;
    auto const newSd = [&]() -> StringData* {
      auto const tmp = StringData::Make(sd, CopyString);
      auto const tmp2 = tmp->increment();
      if (tmp2 != tmp) {
        assertx(tmp->hasExactlyOneRef());
        tmp->release();
        return tmp2;
      }
      return tmp;
    }();
    decRefStr(sd);
    tvCopy(make_tv<KindOfString>(newSd), cell);
  }
};

struct Dec {
  void intCase(tv_lval cell) {
    auto& n = val(cell).num;
    n = sub_ignore_overflow(n, 1);
  }
  void dblCase(tv_lval cell) { --val(cell).dbl; }
  void nullCase(tv_lval) const {}
  void nonNumericString(tv_lval cell) const {
    raise_notice("Decrement on string '%s'", val(cell).pstr->data());
  }
};

}

//////////////////////////////////////////////////////////////////////

TypedValue tvAdd(TypedValue c1, TypedValue c2) {
  return tvArith(Add(), c1, c2);
}

TypedValue tvSub(TypedValue c1, TypedValue c2) {
  return tvArith(Sub(), c1, c2);
}

TypedValue tvMul(TypedValue c1, TypedValue c2) {
  return tvArith(Mul(), c1, c2);
}

TypedValue tvDiv(TypedValue c1, TypedValue c2) {
  return tvArith(Div(), c1, c2);
}

TypedValue tvPow(TypedValue c1, TypedValue c2) {
  return *HHVM_FN(pow)(tvAsVariant(&c1), tvAsVariant(&c2)).asTypedValue();
}

TypedValue tvMod(TypedValue c1, TypedValue c2) {
  check_numeric(c1, c2);
  auto to_int = [](TypedValue tv) {
    return tvIsInt(tv) ? tv.m_data.num : double_to_int64(tv.m_data.dbl);
  };
  auto const i2 = to_int(c2);
  if (UNLIKELY(i2 == 0)) SystemLib::throwDivisionByZeroExceptionObject();
  // This is to avoid SIGFPE in the case of INT64_MIN % -1.
  return make_int(UNLIKELY(i2 == -1) ? 0 : to_int(c1) % i2);
}

TypedValue tvBitAnd(TypedValue c1, TypedValue c2) {
  return tvBitOp<std::bit_and>(
    [] (uint32_t a, uint32_t b) { return std::min(a, b); },
    c1, c2
  );
}

TypedValue tvBitOr(TypedValue c1, TypedValue c2) {
  return tvBitOp<std::bit_or>(
    [] (uint32_t a, uint32_t b) { return std::max(a, b); },
    c1, c2
  );
}

TypedValue tvBitXor(TypedValue c1, TypedValue c2) {
  return tvBitOp<std::bit_xor>(
    [] (uint32_t a, uint32_t b) { return std::min(a, b); },
    c1, c2
  );
}

StringData* strBitXor(StringData* s1, StringData* s2) {
  return stringBitOp(
    std::bit_xor<char>(),
    [] (uint32_t a, uint32_t b) { return std::min(a, b); },
    s1, s2
  );
}

TypedValue tvShl(TypedValue c1, TypedValue c2) {
  if (!tvIsInt(c1) || !tvIsInt(c2)) throwBitOpBadTypesException(&c1, &c2);
  return make_int(shl_ignore_overflow(c1.m_data.num, c2.m_data.num));
}

TypedValue tvShr(TypedValue c1, TypedValue c2) {
  if (!tvIsInt(c1) || !tvIsInt(c2)) throwBitOpBadTypesException(&c1, &c2);
  return make_int(c1.m_data.num >> (c2.m_data.num & 63));
}

void tvAddEq(tv_lval c1, TypedValue c2) {
  tvOpEq(AddEq(), c1, c2);
}

void tvSubEq(tv_lval c1, TypedValue c2) {
  tvOpEq(SubEq(), c1, c2);
}

void tvMulEq(tv_lval c1, TypedValue c2) {
  tvOpEq(MulEq(), c1, c2);
}

void tvDivEq(tv_lval c1, TypedValue c2) {
  assertx(tvIsPlausible(*c1));
  assertx(tvIsPlausible(c2));
  if (UNLIKELY(!is_numeric(*c1))) throwMathBadTypesException(c1, &c2);
  tvCopy(tvDiv(*c1, c2), c1);
}

void tvPowEq(tv_lval c1, TypedValue c2) {
  tvSet(tvPow(*c1, c2), c1);
}

void tvModEq(tv_lval c1, TypedValue c2) {
  tvSet(tvMod(*c1, c2), c1);
}

void tvBitAndEq(tv_lval c1, TypedValue c2) {
  tvBitOpEq(tvBitAnd, c1, c2);
}

void tvBitOrEq(tv_lval c1, TypedValue c2) {
  tvBitOpEq(tvBitOr, c1, c2);
}

void tvBitXorEq(tv_lval c1, TypedValue c2) {
  tvBitOpEq(tvBitXor, c1, c2);
}

void tvShlEq(tv_lval c1, TypedValue c2) { tvSet(tvShl(*c1, c2), c1); }
void tvShrEq(tv_lval c1, TypedValue c2) { tvSet(tvShr(*c1, c2), c1); }

void tvInc(tv_lval cell) { tvIncDecOp(Inc(), cell); }
void tvDec(tv_lval cell) { tvIncDecOp(Dec(), cell); }

void tvBitNot(TypedValue& cell) {
  assertx(tvIsPlausible(cell));

  switch (cell.m_type) {
    case KindOfInt64:
      cell.m_data.num = ~cell.m_data.num;
      break;

    case KindOfClass:
      [[fallthrough]];
    case KindOfLazyClass:
      {
        auto const o = "increment op";
        cell.m_data.pstr =
          isClassType(cell.m_type) ?
          const_cast<StringData*>(classToStringHelper(cell.m_data.pclass, o)) :
          const_cast<StringData*>(lazyClassToStringHelper(cell.m_data.plazyclass,
                                                          o));
      }
      cell.m_type = KindOfString;
      [[fallthrough]];
    case KindOfString:
      if (cell.m_data.pstr->cowCheck()) {
        raiseBitwiseOpInvalidType();
        auto const sl = cell.m_data.pstr->slice();
        USDT(hhvm, hhvm_cow_bitnot, sl.size());
        auto const newSd = StringData::Make(sl, CopyString);
        cell.m_data.pstr->decRefCount(); // can't go to zero
        cell.m_data.pstr = newSd;
        cell.m_type = KindOfString;
      } else {
        raiseBitwiseOpInvalidType();
        // Unless we go through this branch, the string was just freshly
        // created, so the following mutation will be safe wrt its
        // internal hash caching.
        cell.m_data.pstr->invalidateHash();
        USDT(hhvm, hhvm_mut_bitnot, cell.m_data.pstr->size());
      }

      {
        auto const sd   = cell.m_data.pstr;
        auto const len  = sd->size();
        auto const data = sd->mutableData();
        assertx(sd->hasExactlyOneRef());
        for (uint32_t i = 0; i < len; ++i) {
          data[i] = ~data[i];
        }
      }
      break;

    case KindOfDouble:
      SystemLib::throwInvalidOperationExceptionObject(
        "Cannot perform a bitwise not on float");
    case KindOfFunc:
      invalidFuncConversion("int");
    case KindOfUninit:
    case KindOfNull:
    case KindOfBoolean:
    case KindOfVec:
    case KindOfDict:
    case KindOfKeyset:
    case KindOfObject:
    case KindOfResource:
    case KindOfClsMeth:
    case KindOfRClsMeth:
    case KindOfRFunc:
    case KindOfEnumClassLabel:
      raise_error("Unsupported operand type for ~");
  }
}

//////////////////////////////////////////////////////////////////////

}
