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

#include "hphp/runtime/vm/jit/irgen-arith.h"

#include "hphp/runtime/base/strings.h"

#include "hphp/runtime/base/tv-conv-throw.h"
#include "hphp/runtime/vm/jit/irgen-exit.h"
#include "hphp/runtime/vm/jit/irgen-incdec.h"
#include "hphp/runtime/vm/jit/irgen-interpone.h"

#include "hphp/runtime/vm/jit/irgen-internal.h"

#include "hphp/util/configs/eval.h"
#include "hphp/util/text-util.h"

namespace HPHP::jit::irgen {

bool areBinaryArithTypesSupported(Op op, Type t1, Type t2) {
  auto is_numeric = [](Type ty) { return ty.subtypeOfAny(TInt, TDbl); };

  switch (op) {
  case Op::Add:
  case Op::Sub:
  case Op::Mul:
  case Op::Div:
  case Op::Mod:
    return is_numeric(t1) && is_numeric(t2);
  case Op::BitAnd:
  case Op::BitOr:
  case Op::BitXor:
    return t1 <= TInt && t2 <= TInt;
  default:
    always_assert(0);
  }
}

Opcode intArithOp(Op op) {
  switch (op) {
  case Op::Add:  return AddInt;
  case Op::Sub:  return SubInt;
  case Op::Mul:  return MulInt;
  default:
    break;
  }
  always_assert(0);
}

Opcode dblArithOp(Op op) {
  switch (op) {
  case Op::Add:  return AddDbl;
  case Op::Sub:  return SubDbl;
  case Op::Mul:  return MulDbl;
  default:
    break;
  }
  always_assert(0);
}

Opcode bitOp(Op op) {
  switch (op) {
  case Op::BitAnd: return AndInt;
  case Op::BitOr:  return OrInt;
  case Op::BitXor: return XorInt;
  default:
    break;
  }
  always_assert(0);
}

bool isBitOp(Op op) {
  switch (op) {
  case Op::BitAnd:
  case Op::BitOr:
  case Op::BitXor:
    return true;
  default:
    return false;
  }
}

Opcode promoteBinaryDoubles(IRGS& env, Op op, SSATmp*& src1, SSATmp*& src2) {
  auto const type1 = src1->type();
  auto const type2 = src2->type();
  auto opc = intArithOp(op);
  if (type1 <= TDbl) {
    opc = dblArithOp(op);
    if (type2 <= TInt) {
      src2 = gen(env, ConvIntToDbl, src2);
    }
  } else if (type2 <= TDbl) {
    opc = dblArithOp(op);
    src1 = gen(env, ConvIntToDbl, src1);
  }
  return opc;
}

namespace {

Type interpBinaryArithResult(Op op, Type lhs, Type rhs) {
  auto const numeric = TInt | TDbl;
  if (!lhs.maybe(numeric) || !rhs.maybe(numeric)) return TBottom;
  return op == Op::Mod ? TInt : numeric;
}

void binaryBitOp(IRGS& env, Op op) {
  auto const type2 = topC(env, BCSPRelOffset{0})->type();
  auto const type1 = topC(env, BCSPRelOffset{1})->type();
  if (!areBinaryArithTypesSupported(op, type1, type2)) {
    PUNT(BinaryBitOp-Unsupported);
    return;
  }

  auto const src2 = popC(env);
  auto const src1 = popC(env);
  push(env, gen(env, bitOp(op), src1, src2));
}

void binaryArith(IRGS& env, Op op) {
  auto const type2 = topC(env, BCSPRelOffset{0})->type();
  auto const type1 = topC(env, BCSPRelOffset{1})->type();
  if (!areBinaryArithTypesSupported(op, type1, type2)) {
    interpOne(env, interpBinaryArithResult(op, type1, type2), 2);
    return;
  }

  auto src2 = popC(env);
  auto src1 = popC(env);
  auto const opc = promoteBinaryDoubles(env, op, src1, src2);

  push(env, gen(env, opc, src1, src2));
}

// Helpers for comparison generation:

Opcode toBoolCmpOpcode(Op op) {
  switch (op) {
    case Op::Gt:    return GtBool;
    case Op::Gte:   return GteBool;
    case Op::Lt:    return LtBool;
    case Op::Lte:   return LteBool;
    case Op::Eq:
    case Op::Same:  return EqBool;
    case Op::Neq:
    case Op::NSame: return NeqBool;
    case Op::Cmp:   return CmpBool;
    default: always_assert(false);
  }
}

Opcode toIntCmpOpcode(Op op) {
  switch (op) {
    case Op::Gt:    return GtInt;
    case Op::Gte:   return GteInt;
    case Op::Lt:    return LtInt;
    case Op::Lte:   return LteInt;
    case Op::Eq:
    case Op::Same:  return EqInt;
    case Op::Neq:
    case Op::NSame: return NeqInt;
    case Op::Cmp:   return CmpInt;
    default: always_assert(false);
  }
}

Opcode toDblCmpOpcode(Op op) {
  switch (op) {
    case Op::Gt:    return GtDbl;
    case Op::Gte:   return GteDbl;
    case Op::Lt:    return LtDbl;
    case Op::Lte:   return LteDbl;
    case Op::Eq:
    case Op::Same:  return EqDbl;
    case Op::Neq:
    case Op::NSame: return NeqDbl;
    case Op::Cmp:   return CmpDbl;
    default: always_assert(false);
  }
}

Opcode toStrCmpOpcode(Op op) {
  switch (op) {
    case Op::Gt:    return GtStr;
    case Op::Gte:   return GteStr;
    case Op::Lt:    return LtStr;
    case Op::Lte:   return LteStr;
    case Op::Eq:    return EqStr;
    case Op::Same:  return SameStr;
    case Op::Neq:   return NeqStr;
    case Op::NSame: return NSameStr;
    case Op::Cmp:   return CmpStr;
    default: always_assert(false);
  }
}

Opcode toObjCmpOpcode(Op op) {
  switch (op) {
    case Op::Gt:    return GtObj;
    case Op::Gte:   return GteObj;
    case Op::Lt:    return LtObj;
    case Op::Lte:   return LteObj;
    case Op::Eq:    return EqObj;
    case Op::Same:  return SameObj;
    case Op::Neq:   return NeqObj;
    case Op::NSame: return NSameObj;
    case Op::Cmp:   return CmpObj;
    default: always_assert(false);
  }
}

Opcode toArrLikeCmpOpcode(Op op) {
  switch (op) {
    case Op::Gt:    return GtArrLike;
    case Op::Gte:   return GteArrLike;
    case Op::Lt:    return LtArrLike;
    case Op::Lte:   return LteArrLike;
    case Op::Eq:    return EqArrLike;
    case Op::Same:  return SameArrLike;
    case Op::Neq:   return NeqArrLike;
    case Op::NSame: return NSameArrLike;
    case Op::Cmp:   return CmpArrLike;
    default: always_assert(false);
  }
}

Opcode toArrLikeCmpOpcodeNoRelational(Op op) {
  switch (op) {
    case Op::Eq:    return EqArrLike;
    case Op::Same:  return SameArrLike;
    case Op::Neq:   return NeqArrLike;
    case Op::NSame: return NSameArrLike;
    default: always_assert(false);
  }
}

Opcode toResCmpOpcode(Op op) {
  switch (op) {
    case Op::Gt:    return GtRes;
    case Op::Gte:   return GteRes;
    case Op::Lt:    return LtRes;
    case Op::Lte:   return LteRes;
    case Op::Eq:
    case Op::Same:  return EqRes;
    case Op::Neq:
    case Op::NSame: return NeqRes;
    case Op::Cmp:   return CmpRes;
    default: always_assert(false);
  }
}

// Emit a boolean comparison against two constants. Will be simplified to a
// constant later on.
SSATmp* emitConstCmp(IRGS& env, Op op, bool left, bool right) {
  return gen(env, toBoolCmpOpcode(op), cns(env, left), cns(env, right));
}

SSATmp* implDictOrKeysetCmp(
    IRGS& env, Op op, SSATmp* left, SSATmp* right, StaticString err) {
  assertx(equivDataTypes(left->type().toDataType(), right->type().toDataType()));
  if (op == Op::Eq || op == Op::Neq || op == Op::Same || op == Op::NSame) {
    return gen(env, toArrLikeCmpOpcodeNoRelational(op), left, right);
  } else {
    // Dicts and keysets can't use relational comparisons.
    gen(env, ThrowInvalidOperation, cns(env, err.get()));
    return cns(env, false);
  }
}

SSATmp* implDictCmp(IRGS& env, Op op, SSATmp* left, SSATmp* right) {
  return implDictOrKeysetCmp(env, op, left, right, s_cmpWithDict);
}
SSATmp* implKeysetCmp(IRGS& env, Op op, SSATmp* left, SSATmp* right) {
  return implDictOrKeysetCmp(env, op, left, right, s_cmpWithKeyset);
}

SSATmp* negate(IRGS& env, SSATmp* orig) {
  return gen(env, XorBool, orig, cns(env, true));
}

SSATmp* implFunCmp(IRGS& env, Op op, SSATmp* left, SSATmp* right) {
  if (op == Op::Eq || op == Op::Same)   return gen(env, EqFunc, left, right);
  if (op == Op::Neq || op == Op::NSame) return negate(env, gen(env, EqFunc, left, right));
  PUNT(Func-cmp);
}

SSATmp* convToStr(IRGS& env, SSATmp* in, bool should_warn) {
  if (should_warn &&
      Cfg::Eval::RaiseClassConversionNoticeSampleRate > 0 &&
      in->type().subtypeOfAny(TCls, TLazyCls)) {
    std::string msg;
    string_printf(msg, Strings::CLASS_TO_STRING_IMPLICIT, "comparison");
    gen(env,
        RaiseNotice,
        SampleRateData { Cfg::Eval::RaiseClassConversionNoticeSampleRate },
        cns(env, makeStaticString(msg)));
  }
  if (in->isA(TCls))     return gen(env, LdClsName, in);
  if (in->isA(TLazyCls)) return gen(env, LdLazyClsName, in);
  assertx(in->isA(TStr));
  return in;
}


SSATmp* implStrCmp(IRGS& env, Op op, SSATmp* left, SSATmp* right) {
  return gen(env, toStrCmpOpcode(op), left, convToStr(env, right, true));
}

SSATmp* implClsishCmp(IRGS& env, Op op, SSATmp* left, SSATmp* right, bool lazy) {
 auto const rightTy = right->type();

  if (rightTy <= TStr) return implStrCmp(env, op, convToStr(env, left, true), right);

  if (rightTy <= (lazy ? TLazyCls : TCls)) {
    const auto eq = (lazy ? EqLazyCls : EqCls);
    if (op == Op::Eq || op == Op::Same)   return gen(env, eq, left, right);
    if (op == Op::Neq || op == Op::NSame) return negate(env, gen(env, eq, left, right));
  } else {
    // we're the alternately lazy type of cls pointer
    if (op == Op::Eq || op == Op::Neq || op == Op::Same || op == Op::NSame) {
      return implStrCmp(env, op, convToStr(env, left, false), convToStr(env, right, false));
    }
  }

  PUNT(lazy ? LazyCls-cmp : Cls-cmp);
}

SSATmp* implLazyClsCmp(IRGS& env, Op op, SSATmp* left, SSATmp* right) {
  return implClsishCmp(env, op, left, right, true);
}
SSATmp* implClsCmp(IRGS& env, Op op, SSATmp* left, SSATmp* right) {
  return implClsishCmp(env, op, left, right, false);
}

SSATmp* implEnumClassLabelCmp(IRGS& env, Op op, SSATmp* left, SSATmp* right) {
  assertx(left->isA(TEnumClassLabel));
  assertx(right->isA(TEnumClassLabel));

  switch (op) {
    case Op::Gt:
    case Op::Gte:
    case Op::Lt:
    case Op::Lte:
    case Op::Cmp:
      PUNT(EnumClassLabel-relationalcmp);
    case Op::Eq:
    case Op::Same:
    case Op::Neq:
    case Op::NSame: {
      auto const l = gen(env, LdEnumClassLabelName, left);
      auto const r = gen(env, LdEnumClassLabelName, right);
      return gen(env, toStrCmpOpcode(op), l, r);
    }
    default: always_assert(false);
  }
  not_reached();
}

SSATmp* implClsMethCmp(IRGS& env, Op op, SSATmp* left, SSATmp* right) {
  const auto eq = [&] { return cond(
    env,
    [&](Block* taken) {
      auto const leftCls = gen(env, LdClsFromClsMeth, left);
      auto const rightCls = gen(env, LdClsFromClsMeth, right);
      assertx(leftCls->type() <= TCls);
      assertx(rightCls->type() <= TCls);
      gen(env, JmpZero, taken, gen(env, EqCls, leftCls, rightCls));
    },
    [&] {
      auto const leftFunc = gen(env, LdFuncFromClsMeth, left);
      auto const rightFunc = gen(env, LdFuncFromClsMeth, right);
      assertx(leftFunc->type() <= TFunc);
      assertx(rightFunc->type() <= TFunc);
      return gen(env, EqFunc, leftFunc, rightFunc);
    },
    [&] {
      return cns(env, false);
    }
  ); };
  if (op == Op::Eq || op == Op::Same) return eq();
  if (op == Op::Neq || op == Op::NSame) return negate(env, eq());
  PUNT(ClsMeth-ClsMeth-cmp);
}

/*
 * Responsible for converting the bytecode comparisons (which are type-agnostic)
 * to IR comparisons (which are typed). This generally involves inserting the
 * right kind of type conversions to satisfy PHP semantics. For a few special
 * cases, (object-string and string-int), we have special IR opcodes because the
 * required semantics cannot be easily represented via type conversions.
 */
void implCmp(IRGS& env, Op op) {
  auto const right = topC(env);
  auto const left = topC(env, BCSPRelOffset{1});
  auto const leftTy = left->type();
  auto const rightTy = right->type();

  if (!leftTy.isKnownDataType() || !rightTy.isKnownDataType()) {
    // Can't do much if we don't even know the types.
    PUNT(cmpUnknownDataType);
  }

  auto equiv = [&] {
    const auto isStringOrClassish = [](DataType t) {
      return isStringType(t) || isClassType(t) || isLazyClassType(t);
    };
    return
      equivDataTypes(leftTy.toDataType(), rightTy.toDataType()) ||
      (isStringOrClassish(leftTy.toDataType()) &&
       isStringOrClassish(rightTy.toDataType()));
  };

  auto done = [&] {
    decRef(env, left, DecRefProfileId::CmpLhs);
    decRef(env, right, DecRefProfileId::CmpRhs);
  };

  if (!equiv()) {
    if (op == Op::Same || op == Op::NSame || op == Op::Eq || op == Op::Neq) {
      discard(env, 2);
      push(env, emitConstCmp(env, op, false, true));
    } else if ((leftTy <= TInt && rightTy <= TDbl) ||
               (leftTy <= TDbl && rightTy <= TInt)) {
      // int v dbl is allowed for relational comparisons
      const auto convToDbl = [&](SSATmp* v) {
        return v->isA(TDbl) ? v : gen(env, ConvIntToDbl, v);
      };
      discard(env, 2);
      push(env, gen(env, toDblCmpOpcode(op), convToDbl(left), convToDbl(right)));
    } else {
      // relational comparisons on different types throw every time
      interpOne(env, TBottom, 2);
      return;
    }
    return done();
  }
  discard(env, 2);

  if (leftTy <= TNull) push(env, emitConstCmp(env, op, false, false));
  else if (leftTy <= TRFunc)    PUNT(RFunc-cmp);
  else if (leftTy <= TRClsMeth) PUNT(RClsMeth-cmp);
  else {
    const auto opConv = [&]() -> Optional<Opcode (*)(HPHP::Op)> {
      if (leftTy <= TBool) return toBoolCmpOpcode;
      if (leftTy <= TInt)  return toIntCmpOpcode;
      if (leftTy <= TDbl)  return toDblCmpOpcode;
      if (leftTy <= TVec)  return toArrLikeCmpOpcode;
      if (leftTy <= TObj)  return toObjCmpOpcode;
      if (leftTy <= TRes)  return toResCmpOpcode;
      return std::nullopt;
    }();

    if (opConv) {
       push(env, gen(env, (*opConv)(op), left, right));
    } else {
      const auto impl = [&] {
        if (leftTy <= TFunc)    return implFunCmp;
        if (leftTy <= TStr)     return implStrCmp;
        if (leftTy <= TCls)     return implClsCmp;
        if (leftTy <= TDict)    return implDictCmp;
        if (leftTy <= TKeyset)  return implKeysetCmp;
        if (leftTy <= TLazyCls) return implLazyClsCmp;
        if (leftTy <= TClsMeth) return implClsMethCmp;
        if (leftTy <= TEnumClassLabel) return implEnumClassLabelCmp;
        always_assert(false);
      }();
      push(env, (*impl)(env, op, left, right));
    }
  }

  done();
}

void implAdd(IRGS& env, Op op) {
  binaryArith(env, op);
}



template<class PreDecRef> void implConcat(
    IRGS& env, SSATmp* c1, SSATmp* c2, PreDecRef preDecRef, bool setop) {
  auto cast =
    [&] (SSATmp* s, DecRefProfileId locId) {
      if (s->isA(TStr)) return s;
      auto const ret = gen(
        env,
        ConvTVToStr,
        ConvThrowData{ConvThrowMode::Throw},
        s);
      decRef(env, s, locId);
      return ret;
    };

  /*
   * We have some special translations for common combinations that avoid extra
   * conversion calls.
   */
  auto const str =
    [&] () -> SSATmp* {
      if (c1->isA(TInt)) {
        return gen(
            env, ConcatStrInt, cast(c2, DecRefProfileId::ConcatSrc2), c1);
      }
      if (c2->isA(TInt)) {
        return gen(
            env, ConcatIntStr, c2, c1 = cast(c1, DecRefProfileId::ConcatSrc1));
      }
      return nullptr;
    }();

  if (str) {
    preDecRef(str);
    // Note that the ConcatFoo opcode consumed the reference on its first
    // argument, so we only need to decref the second one.
    decRef(env, c1, DecRefProfileId::ConcatSrc1);
    return;
  }

  /*
   * Generic translation: convert both to strings, and then concatenate them.
   *
   * NB: the order we convert to strings is observable because of __toString
   * methods and error/notice messages.
   *
   * We don't want to convert to strings if either was already a string.  Note
   * that for the c2 string, failing to do this could change big-O program
   * behavior if refcount opts were off, since we'd COW strings that we
   * shouldn't (a ConvTVToStr of a Str will simplify into an IncRef).
   */
  if (!setop) c2 = cast(c2, DecRefProfileId::ConcatSrc2);
  c1 = cast(c1, DecRefProfileId::ConcatSrc1);
  if (setop) c2 = cast(c2, DecRefProfileId::ConcatSrc2);
  auto const r  = gen(env, ConcatStrStr, c2, c1);  // consumes c2 reference
  preDecRef(r);
  decRef(env, c1, DecRefProfileId::ConcatSrc1);
}

//////////////////////////////////////////////////////////////////////

}

void emitConcat(IRGS& env) {
  auto const c1 = popC(env);
  auto const c2 = popC(env);
  implConcat(env, c1, c2, [&] (SSATmp* r) { push(env, r); }, false);
}

void emitConcatN(IRGS& env, uint32_t n) {
  if (n == 2) return emitConcat(env);

  auto const t1 = popC(env);
  auto const t2 = popC(env);
  auto const t3 = popC(env);
  auto const t4 = n == 4 ? popC(env) : nullptr;

  const auto convData =
    ConvThrowData{ConvThrowMode::Throw};
  auto const s4 = !t4 || t4->isA(TStr) ? t4 : gen(env, ConvTVToStr, convData, t4);
  auto const s3 = t3->isA(TStr) ? t3 : gen(env, ConvTVToStr, convData, t3);
  auto const s2 = t2->isA(TStr) ? t2 : gen(env, ConvTVToStr, convData, t2);
  auto const s1 = t1->isA(TStr) ? t1 : gen(env, ConvTVToStr, convData, t1);

  if (n == 3) {
    push(env, gen(env, ConcatStr3, s3, s2, s1));
  } else {
    always_assert(n == 4);

    push(env, gen(env, ConcatStr4, s4, s3, s2, s1));
    decRef(env, s3, DecRefProfileId::ConcatStr3);
  }
  decRef(env, s2, DecRefProfileId::ConcatStr2);
  decRef(env, s1, DecRefProfileId::ConcatStr1);

  if (s1 != t1) decRef(env, t1, DecRefProfileId::ConcatSrc1);
  if (s2 != t2) decRef(env, t2, DecRefProfileId::ConcatSrc2);
  if (s3 != t3) decRef(env, t3, DecRefProfileId::ConcatSrc3);
  if (s4 != t4) decRef(env, t4, DecRefProfileId::ConcatSrc4);
}

void emitSetOpL(IRGS& env, int32_t id, SetOpOp subop) {
  auto const subOpc = [&]() -> Optional<Op> {
    switch (subop) {
    case SetOpOp::PlusEqual:   return Op::Add;
    case SetOpOp::MinusEqual:  return Op::Sub;
    case SetOpOp::MulEqual:    return Op::Mul;
    case SetOpOp::DivEqual:    return std::nullopt;
    case SetOpOp::ConcatEqual: return Op::Concat;
    case SetOpOp::ModEqual:    return std::nullopt;
    case SetOpOp::PowEqual:    return std::nullopt;
    case SetOpOp::AndEqual:    return Op::BitAnd;
    case SetOpOp::OrEqual:     return Op::BitOr;
    case SetOpOp::XorEqual:    return Op::BitXor;
    case SetOpOp::SlEqual:     return std::nullopt;
    case SetOpOp::SrEqual:     return std::nullopt;
    }
    not_reached();
  }();
  if (!subOpc) PUNT(SetOpL-Unsupported);

  auto loc = ldLoc(env, id, DataTypeGeneric);

  if (*subOpc == Op::Concat) {
    /*
     * The concat helpers incref their results, which will be consumed by
     * the stloc. We need an extra incref for the push onto the stack.
     */
    auto const val    = popC(env);
    env.irb->constrainValue(loc, DataTypeSpecific);
    implConcat(env, val, loc, [&] (SSATmp* result) {
      pushIncRef(env, stLocNRC(env, id, result));
    }, true);
    return;
  }

  if (!areBinaryArithTypesSupported(*subOpc, loc->type(), topC(env)->type())) {
    PUNT(SetOpL);
  }

  auto val = popC(env);
  env.irb->constrainValue(loc, DataTypeSpecific);
  auto opc = isBitOp(*subOpc)
    ? bitOp(*subOpc)
    : promoteBinaryDoubles(env, *subOpc, loc, val);

  auto const result = gen(env, opc, loc, val);
  pushStLoc(env, id, result);
}

void emitIncDecL(IRGS& env, NamedLocal loc, IncDecOp subop) {
  auto const src = ldLocWarn(env, loc, DataTypeSpecific);

  if (auto const result = incDec(env, subop, src)) {
    pushIncRef(env, isPre(subop) ? result : src);
    // Update marker to ensure newly-pushed value isn't clobbered by DecRef.
    updateMarker(env);
    stLoc(env, loc.id, result);
    return;
  }

  PUNT(IncDecL);
}

void implShift(IRGS& env, Opcode op) {
  auto const shiftAmount    = topC(env);
  auto const lhs            = topC(env, BCSPRelOffset{1});
  if (!lhs->isA(TInt) || !shiftAmount->isA(TInt)) {
    interpOne(env, TBottom, 2);
    return;
  }
  discard(env, 2);

  // - PHP7 defines shifts of width >= 64 to return the value you get from a
  //   naive shift, i.e., either 0 or -1 depending on the shift and value. This
  //   is notably *not* the semantics of the x86 shift instructions, so we need
  //   to do some comparison logic here.
  // - PHP7 defines negative shifts to throw an ArithmeticError.
  // - PHP5 semantics for such operations are machine-dependent.
  push(env, gen(env, op, lhs, shiftAmount));
  decRef(env, lhs, DecRefProfileId::ShiftBase);
  decRef(env, shiftAmount, DecRefProfileId::ShiftAmount);
}

void emitShl(IRGS& env) {
  implShift(env, Shl);
}

void emitShr(IRGS& env) {
  implShift(env, Shr);
}

void emitPow(IRGS& env) {
  // Special-case exponent of 0, 1, 2 or 3, i.e.
  // $x ** 0 == 1
  // $x ** 1 == $x
  // $x**2 becomes $x*$x,
  // $x**3 becomes ($x*$x)*$x
  // For other constant integer exponents, use binary exponentiation
  auto exponent = topC(env);
  auto base = topC(env, BCSPRelOffset{1});
  if ((base->isA(TInt) || base->isA(TDbl)) &&
    exponent->hasConstVal(TInt)) {
    auto const intVal = exponent->intVal();
    if (intVal == 0) {
      discard(env, 2);
      push(env, base->isA(TInt) ? cns(env, 1) : cns(env, 1.0));
      return;
    }
    if (intVal == 1) {
      // discard exponent, leave base on stack
      discard(env);
      return;
    }
    
    auto const isNegExp = intVal < 0;
    if (isNegExp) return interpOne(env, TUncountedInit, 2); // TODO
    if (base->isA(TInt)) {
      // Create exit block for potential integer overflow
      auto makeExitPow = [&] () {
        auto const exit = defBlock(env, Block::Hint::Unlikely);
        BlockPusher bp(*env.irb, makeMarker(env, curSrcKey(env)), exit);

        // Convert base to double and compute power
        auto baseVal = base->isA(TInt) ? gen(env, ConvIntToDbl, base) : base;
        auto result = cns(env, 1.0);

        auto exp = intVal;
        while (exp > 0) {
          if (exp & 1) {
            result = gen(env, MulDbl, result, baseVal);
          }
          if (exp > 1) {
            baseVal = gen(env, MulDbl, baseVal, baseVal);
          }
          exp >>= 1;
        }

        discard(env, 2);
        push(env, result);
        gen(env, Jmp, makeExit(env, nextSrcKey(env)));
        return exit;
      };

      auto exitPow = makeExitPow();
      auto result = cns(env, 1);
      auto baseVal = base;

      auto exp = intVal;
      while (exp > 0) {
        if (exp & 1) {
          result = gen(env, MulIntO, exitPow, result, baseVal);
        }
        if (exp > 1) {
          baseVal = gen(env, MulIntO, exitPow, baseVal, baseVal);
        }
        exp >>= 1;
      }

      discard(env, 2);
      push(env, result);
      return;
    } else {
      assert(base->isA(TDbl));
      auto result = cns(env, 1.0);
      auto exp = intVal;
      auto baseVal = base;

      while (exp > 0) {
        if (exp & 1) {
          result = gen(env, MulDbl, result, baseVal);
        }
        if (exp > 1) {
          baseVal = gen(env, MulDbl, baseVal, baseVal);
        }
        exp >>= 1;
      }

      discard(env, 2);
      push(env, result);
      gen(env, Jmp, makeExit(env, nextSrcKey(env)));
      return;

    }
  }
  return interpOne(env, TUncountedInit, 2);
}

void emitBitNot(IRGS& env) {
  auto const srcType = topC(env)->type();
  if (srcType <= TInt) {
    auto const src = popC(env);
    push(env, gen(env, XorInt, src, cns(env, -1)));
    return;
  }

  if (srcType <= TDbl) {
    interpOne(env, TBottom, 1);
    return;
  }

  auto const resultType = srcType <= TStr || srcType <= TCls
                        ? TStr
                        : (srcType.needsReg() ? TCell : TInt);
  interpOne(env, resultType, 1);
}


void emitNot(IRGS& env) {
  auto const src = popC(env);
  push(env, negate(env, gen(env, ConvTVToBool, src)));
  decRef(env, src);
}

const StaticString s_DIVISION_BY_ZERO(Strings::DIVISION_BY_ZERO);
void emitDiv(IRGS& env) {
  auto const divisorType  = topC(env, BCSPRelOffset{0})->type();
  auto const dividendType = topC(env, BCSPRelOffset{1})->type();

  if (!areBinaryArithTypesSupported(Op::Div, divisorType, dividendType)) {
    interpOne(env, interpBinaryArithResult(Op::Div, divisorType, dividendType), 2);
    return;
  }

  auto const divisor  = popC(env);
  auto const dividend = popC(env);

  ifThen(
    env,
    [&] (Block* taken) {
      auto const checkZero =
        divisor->isA(TInt) ? gen(env, EqInt,  divisor, cns(env, 0)) :
        divisor->isA(TDbl) ? gen(env, EqDbl,  divisor, cns(env, 0.0)) :
                             gen(env, EqBool, divisor, cns(env, false));
      gen(env, JmpNZero, taken, checkZero);
    },
    [&] {
      hint(env, Block::Hint::Unlikely);
      gen(env, ThrowDivisionByZeroException);
      }
  );

  auto toDbl = [&] (SSATmp* x) {
    return x->isA(TInt) ? gen(env, ConvIntToDbl, x) : x;
  };

  if (divisor->isA(TDbl) || dividend->isA(TDbl)) {
    push(env, gen(env, DivDbl, toDbl(dividend), toDbl(divisor)));
    return;
  }

  if (divisor->isA(TInt) && dividend->isA(TInt)) {
    ifThen(
      env,
      [&] (Block* taken) {
        auto const badDividend = gen(env, EqInt, dividend, cns(env, LLONG_MIN));
        gen(env, JmpNZero, taken, badDividend);
      },
      [&] {
        hint(env, Block::Hint::Unlikely);
        ifThen(
          env,
          [&] (Block* taken) {
            auto const badDivisor = gen(env, EqInt, divisor, cns(env, -1));
            gen(env, JmpNZero, taken, badDivisor);
          },
          [&] {
            hint(env, Block::Hint::Unlikely);

            // Avoid SIGFPE when dividing the miniumum respresentable integer
            // by -1.
            push(env, gen(env, DivDbl, toDbl(dividend), toDbl(divisor)));
            gen(env, Jmp, makeExit(env, nextSrcKey(env)));
          }
        );
      }
    );
  }

  auto const result = cond(
    env,
    [&] (Block* taken) {
      gen(env, JmpNZero, taken, gen(env, Mod, dividend, divisor));
    },
    [&] { return gen(env, DivInt, dividend, divisor); },
    [&] { return gen(env, DivDbl, toDbl(dividend), toDbl(divisor)); }
  );
  push(env, result);
}

void emitMod(IRGS& env) {
  auto const leftTy  = topC(env, BCSPRelOffset{0})->type();
  auto const rightTy = topC(env, BCSPRelOffset{1})->type();

  if (!areBinaryArithTypesSupported(Op::Mod, leftTy, rightTy)) {
    interpOne(env, interpBinaryArithResult(Op::Mod, leftTy, rightTy), 2);
    return;
  }

  auto toInt = [&] (SSATmp* x) {
    return x->isA(TDbl) ? gen(env, ConvDblToInt, x) : x;
  };

  auto const tr = toInt(popC(env));
  auto const tl = toInt(popC(env));

  // Generate an exit for the rare case that r is zero.
  ifThen(
    env,
    [&] (Block* taken) {
      gen(env, JmpZero, taken, tr);
    },
    [&] {
      hint(env, Block::Hint::Unlikely);
      gen(env, ThrowDivisionByZeroException);
    }
  );

  // Check for -1.  The Mod IR instruction has undefined behavior for -1, but
  // php semantics are to return zero.
  auto const res = cond(
    env,
    [&] (Block* taken) {
      auto const negone = gen(env, EqInt, tr, cns(env, -1));
      gen(env, JmpNZero, taken, negone);
    },
    [&] {
      return gen(env, Mod, tl, tr);
    },
    [&] {
      hint(env, Block::Hint::Unlikely);
      return cns(env, 0);
    }
  );
  push(env, res);
}

//////////////////////////////////////////////////////////////////////

void emitBitAnd(IRGS& env) { binaryBitOp(env, Op::BitAnd); }
void emitBitOr(IRGS& env)  { binaryBitOp(env, Op::BitOr); }
void emitBitXor(IRGS& env) { binaryBitOp(env, Op::BitXor); }

void emitSub(IRGS& env)    { binaryArith(env, Op::Sub); }
void emitMul(IRGS& env)    { binaryArith(env, Op::Mul); }

void emitGt(IRGS& env)     { implCmp(env, Op::Gt);    }
void emitGte(IRGS& env)    { implCmp(env, Op::Gte);   }
void emitLt(IRGS& env)     { implCmp(env, Op::Lt);    }
void emitLte(IRGS& env)    { implCmp(env, Op::Lte);   }
void emitEq(IRGS& env)     { implCmp(env, Op::Eq);    }
void emitNeq(IRGS& env)    { implCmp(env, Op::Neq);   }
void emitSame(IRGS& env)   { implCmp(env, Op::Same);  }
void emitNSame(IRGS& env)  { implCmp(env, Op::NSame); }
void emitCmp(IRGS& env)    { implCmp(env, Op::Cmp); }

void emitAdd(IRGS& env)    { implAdd(env, Op::Add); }

//////////////////////////////////////////////////////////////////////

}
