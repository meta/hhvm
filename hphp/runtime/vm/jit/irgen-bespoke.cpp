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
#include "hphp/runtime/vm/jit/irgen-bespoke.h"

#include <hphp/runtime/vm/jit/ssa-tmp.h>
#include "hphp/runtime/base/bespoke-array.h"
#include "hphp/runtime/base/bespoke/layout-selection.h"
#include "hphp/runtime/base/bespoke/logging-array.h"
#include "hphp/runtime/base/bespoke/logging-profile.h"
#include "hphp/runtime/base/bespoke/struct-dict.h"
#include "hphp/runtime/base/type-structure-helpers-defs.h"

#include "hphp/runtime/vm/jit/irgen-builtin.h"
#include "hphp/runtime/vm/jit/irgen-control.h"
#include "hphp/runtime/vm/jit/irgen-exit.h"
#include "hphp/runtime/vm/jit/irgen-internal.h"
#include "hphp/runtime/vm/jit/irgen-interpone.h"
#include "hphp/runtime/vm/jit/irgen-state.h"
#include "hphp/runtime/vm/jit/irgen-minstr.h"
#include "hphp/runtime/vm/jit/mutation.h"
#include "hphp/runtime/vm/jit/type-array-elem.h"
#include "hphp/runtime/vm/srckey.h"
#include "hphp/util/configs/eval.h"
#include "hphp/util/configs/hhir.h"
#include "hphp/util/trace.h"

namespace HPHP::jit { namespace irgen {

///////////////////////////////////////////////////////////////////////////////

namespace {

StaticString s_ColFromArray("ColFromArray");

// Simple code-gen helpers that do a single bespoke op, possibly with a few
// additional ops around them to produce better types. All of the mutating
// helpers here consume a ref on the input and produce one on the output.

template <typename Finish>
SSATmp* emitProfiledGet(IRGS& env, SSATmp* arr, SSATmp* key,
                        Block* taken, Finish finish,
                        bool profiled = true) {
  auto const val = [&] {
    if (arr->isA(TVec)) {
      gen(env, CheckVecBounds, taken, arr, key);
      auto const data = BespokeGetData { BespokeGetData::KeyState::Present };
      return gen(env, BespokeGet, data, arr, key);
    }
    if (arr->type().arrSpec().is_struct()) {
      if (key->isA(TInt)) return cns(env, TUninit);
      auto const slot = gen(env, StructDictSlot, taken, arr, key);
      auto const elem = gen(env, StructDictElemAddr, arr, key, slot, arr);
      return gen(env, LdMem, TCell, elem);
    }
    if (arr->type().arrSpec().is_type_structure()) {
      if (key->isA(TInt)) return cns(env, TUninit);
      return gen(env, LdTypeStructureVal, taken, arr, key);
    }
    auto const data = BespokeGetData { BespokeGetData::KeyState::Unknown };
    return gen(env, BespokeGet, data, arr, key);
  }();

  auto const pval = [&] {
    if (!profiled) return val;
    // We prefer to test the profiledType first, as this will rule out
    // TUninit when we have profiledType information. In this case, the
    // latter test will be optimized out.
    return profiledType(
      env, val,
      [&] { finish(gen(env, CheckType, TInitCell, taken, val)); }
    );
  }();

  auto const ival = gen(env, CheckType, TInitCell, taken, pval);
  // We need this assertion because we lose const-val information
  // about the output type when we union it with TUninit for the
  // "missing key" case.
  auto const resultType =
    arrLikeElemType(arr->type(), key->type(), curClass(env));
  return gen(env, AssertType, resultType.first, ival);
}

template <typename Finish>
SSATmp* emitProfiledGetThrow(IRGS& env, SSATmp* arr, SSATmp* key,
                             Finish finish, bool profiled = true) {
  if (arr->isA(TVec)) {
    return cond(
      env,
      [&](Block* taken) { gen(env, CheckVecBounds, taken, arr, key); },
      [&] {
        auto const data = BespokeGetData { BespokeGetData::KeyState::Present };
        auto const val = gen(env, BespokeGet, data, arr, key);
        return profiled
          ? profiledType(env, val, [&] { finish(val); })
          : val;
      },
      [&] {
        hint(env, Block::Hint::Unlikely);
        gen(env, ThrowOutOfBounds, arr, key);
        return cns(env, TBottom);
      }
    );
  }

  if (arr->type().arrSpec().is_struct()) {
    if (key->isA(TInt)) {
      gen(env, ThrowOutOfBounds, arr, key);
      return cns(env, TBottom);
    }
    return cond(
      env,
      [&] (Block* taken) {
        auto const slot = gen(env, StructDictSlot, taken, arr, key);
        auto const elem = gen(env, StructDictElemAddr, arr, key, slot, arr);
        auto const val = gen(env, LdMem, TCell, elem);
        auto const pval = [&] {
          if (!profiled) return val;
          return profiledType(
            env, val,
            [&] { finish(gen(env, CheckType, TInitCell, taken, val)); }
          );
        }();
        auto const ival = gen(env, CheckType, TInitCell, taken, pval);
        // We need this assertion because we lose const-val
        // information about the output type when we union it with
        // TUninit for the "missing key" case.
        auto const resultType =
          arrLikeElemType(arr->type(), key->type(), curClass(env));
        return gen(env, AssertType, resultType.first, ival);
      },
      [&] (SSATmp* val) { return val; },
      [&] {
        hint(env, Block::Hint::Unlikely);
        gen(env, ThrowOutOfBounds, arr, key);
        return cns(env, TBottom);
      }
    );
  }

  if (arr->type().arrSpec().is_type_structure()) {
    if (key->isA(TInt)) {
      gen(env, ThrowOutOfBounds, arr, key);
      return cns(env, TBottom);
    }

    SSATmp* result;
    ifThen(
      env,
      [&] (Block *taken) {
        auto const val = gen(env, LdTypeStructureVal, taken, arr, key);
        result = profiled
          ? profiledType(env, val, [&] { finish(val); })
          : val;
      },
      [&] {
        hint(env, Block::Hint::Unlikely);
        gen(env, ThrowOutOfBounds, arr, key);
      }
    );
    return result;
  }

  auto const val = gen(env, BespokeGetThrow, arr, key);
  if (!profiled) return val;
  return profiledType(env, val, [&] { finish(val); });
}

SSATmp* emitGet(IRGS& env, SSATmp* arr, SSATmp* key, Block* taken) {
  return emitProfiledGet(env, arr, key, taken, [&](SSATmp*) {}, false);
}

template <typename Pre, typename Success, typename Fail>
SSATmp* structDictMutation(IRGS& env,
                           SSATmp* arr,
                           SSATmp* key,
                           SSATmp* basePtr,
                           Pre pre,
                           Success success,
                           Fail fail) {
  assertx(arr->type().arrSpec().is_struct());
  assertx(key->isA(TStr));

  SSATmp* preVal = nullptr;
  return cond(
    env,
    [&] (Block* taken) {
      auto const slot = gen(env, StructDictSlot, taken, arr, key);
      preVal = pre(slot, taken);
      return slot;
    },
    [&] (SSATmp* slot) {
      auto const copy = cond(
        env,
        [&] (Block* taken) { return gen(env, CheckArrayCOW, taken, arr); },
        [&] (SSATmp* single) {
          gen(env, StMemMeta, basePtr, single);
          return single;
        },
        [&] {
          decRefNZ(env, arr);
          auto const copy = gen(env, CopyArray, arr);
          gen(env, StMem, basePtr, copy);
          return copy;
        }
      );

      auto const elem = gen(env, StructDictElemAddr, copy, key, slot, arr);
      return cond(
        env,
        [&] (Block* taken) {
          return success(copy, elem, slot, preVal, taken);
        },
        [&] (SSATmp* s) { return s; },
        [&] { return fail(copy); }
      );
    },
    [&] { return fail(arr); }
  );
}

SSATmp* emitElem(IRGS& env, SSATmp* arrLval, Type baseType, SSATmp* key,
                 bool throwOnMissing) {
  auto const arr = gen(env, LdMem, baseType, arrLval);

  if (arr->type().arrSpec().is_struct() && key->isA(TStr)) {
    return structDictMutation(
      env, arr, key, arrLval,
      [] (SSATmp*, Block*) { return nullptr; },
      [&] (SSATmp*, SSATmp* elem, SSATmp*, SSATmp*, Block* fail) {
        gen(env, CheckType, TInitCell, fail, gen(env, LdMem, TCell, elem));
        return elem;
      },
      [&] (SSATmp* arr) {
        hint(env, Block::Hint::Unlikely);
        if (throwOnMissing) {
          // We've written the COWed array back to the base already, so
          // set exception boundary to let frame state know that it's
          // okay.
          env.irb->exceptionStackBoundary();
          gen(env, ThrowOutOfBounds, arr, key);
          return cns(env, TBottom);
        }
        return ptrToInitNull(env);
      }
    );
  }

  return gen(
    env,
    BespokeElem,
    baseType,
    arrLval,
    key,
    cns(env, throwOnMissing)
  );
}

template <typename Finish>
void emitSet(IRGS& env, SSATmp* arr, SSATmp* key, SSATmp* uncheckedVal,
             SSATmp* basePtr, Finish finish) {
  auto const sideExit = [&] {
    hint(env, Block::Hint::Unlikely);
    gen(env, StMem, basePtr, gen(env, BespokeSet, arr, key, uncheckedVal));
    finish(uncheckedVal);
    gen(env, Jmp, makeExit(env, nextSrcKey(env)));
    return cns(env, TBottom);
  };

  if (arr->type().arrSpec().is_struct() && key->isA(TStr)) {
    structDictMutation(
      env, arr, key, basePtr,
      [&] (SSATmp* slot, Block* fail) {
        return gen(
          env,
          StructDictTypeBoundCheck,
          fail,
          uncheckedVal,
          arr,
          slot
        );
      },
      [&] (SSATmp* cowed, SSATmp* elem, SSATmp* slot, SSATmp* val, Block*) {
        ifThen(
          env,
          [&] (Block* taken) {
            auto const oldVal = gen(
              env,
              CheckType,
              TInitCell,
              taken,
              gen(env, LdMem, TCell, elem)
            );
            decRef(env, oldVal, DecRefProfileId::ProfiledArraySet);
          },
          [&] { gen(env, StructDictAddNextSlot, cowed, slot); }
        );
        gen(env, StMem, elem, val);
        finish(val);
        return cns(env, TBottom);
      },
      [&] (SSATmp*) { return sideExit(); }
    );
    return;
  }

  // Emit a type check if the base's layout places type bounds on
  // values. Doing the check here is useful because it can
  // potentially save an IncRef if we push val onto the stack.
  //
  // We don't want to over-constrain the type here, so we relax type
  // bounds to either DataTypeSpecific (if applicable) or
  // TUncounted. We also don't do the check if profiling is awry and
  // the bound is definitely violated.
  auto const layout = arr->type().arrSpec().layout();
  auto const bound = layout.elemType(key->type()).first;
  auto const type = [&]{
    if (bound.isKnownDataType()) {
      return Type(bound.toDataType());
    }
    if (bound == TUncountedInit) return TUncounted;
    return TCell;
  }();

  auto const val = [&] {
    if (uncheckedVal->type().maybe(type) && !uncheckedVal->isA(type)) {
      return cond(
        env,
        [&] (Block* taken) {
          return gen(env, CheckType, type, taken, uncheckedVal);
        },
        [&] (SSATmp* v) { return v; },
        sideExit
      );
    }
    return uncheckedVal;
  }();
  gen(env, StMem, basePtr, gen(env, BespokeSet, arr, key, val));
  finish(val);
}

SSATmp* emitAppend(IRGS& env, SSATmp* arr, SSATmp* val) {
  return gen(env, BespokeAppend, arr, val);
}

SSATmp* emitEscalateToVanilla(
    IRGS& env, SSATmp* arr, const StaticString& reason) {
  auto const layout = arr->type().arrSpec().layout();
  if (layout.vanilla()) return arr;
  if (layout.bespoke()) {
    auto const str = cns(env, reason.get());
    auto const result = gen(env, BespokeEscalateToVanilla, arr, str);
    decRef(env, arr);
    return result;
  }
  return cond(
    env,
    [&](Block* taken) {
      return gen(env, CheckType, TVanillaArrLike, taken, arr);
    },
    [&](SSATmp* vanilla) { return vanilla; },
    [&]{
      auto const str = cns(env, reason.get());
      auto const type = TArrLike.narrowToLayout(ArrayLayout::Bespoke());
      auto const bespoke = gen(env, AssertType, type, arr);
      return gen(env, BespokeEscalateToVanilla, bespoke, str);
    }
  );
}

SSATmp* extractBase(IRGS& env) {
  return gen(env, LdMem, TCell, ldMBase(env));
}

SSATmp* classConvertPuntOnRaise(IRGS& env, SSATmp* key) {
  if (key->isA(TCls)) {
    if (Cfg::Eval::RaiseClassConversionNoticeSampleRate > 0) {
      // TODO(vmladenov) if punting is too slow, could gen RaiseNotice
      PUNT(BespokeClsConvert);
    }
    return gen(env, LdClsName, key);
  }
  if (key->isA(TLazyCls)) {
    if (Cfg::Eval::RaiseClassConversionNoticeSampleRate > 0) {
      PUNT(BespokeClsConvert);
    }
    return gen(env, LdLazyClsName, key);
  }
  return key;
}

SSATmp* memberKey(IRGS& env, MemberKey mk) {
  auto const res = [&] () -> SSATmp* {
    switch (mk.mcode) {
      case MW:
        return nullptr;
      case MEL: case MPL:
        return ldLoc(env, mk.local.id, DataTypeSpecific);
      case MEC: case MPC:
        return topC(env, BCSPRelOffset{int32_t(mk.iva)});
      case MEI:
        return cns(env, mk.int64);
      case MET: case MPT: case MQT:
        return cns(env, mk.litstr);
    }
    not_reached();
  }();
  if (!res) return nullptr;

  if (res->type().maybe(TUninit)) PUNT(MInstr-Uninit-Key);
  if (!res->type().isKnownDataType()) PUNT(MInstr-KeyNotKnown);
  return classConvertPuntOnRaise(env, res);
}

SSATmp* emitSetNewElem(IRGS& env, SSATmp* origValue) {
  auto const baseType = env.irb->fs().mbase().type;
  auto const base = extractBase(env);
  auto const value = [&] {
    if (!(baseType <= TKeyset)) return origValue;

    if (!origValue->type().isKnownDataType()) {
      PUNT(Bespoke-SetNewElem-Keyset);
    }
    return classConvertPuntOnRaise(env, origValue);
  }();

  if (baseType <= TKeyset && !value->isA(TInt | TStr)) {
    gen(env, ThrowInvalidArrayKey, base, value);
    return value;
  }

  auto const newArr = emitAppend(env, base, value);

  // Update the base's location with the new array.
  gen(env, StMem, ldMBase(env), newArr);
  gen(env, IncRef, value);
  return value;
}

bool keyCheckForSet(IRGS& env, SSATmp* key) {
  assertx(key->type().isKnownDataType());

  auto const baseType = env.irb->fs().mbase().type;
  if (baseType <= TVec) {
    if (!key->isA(TInt)) {
      gen(env, ThrowInvalidArrayKey, extractBase(env), key);
      return false;
    }
  } else if (baseType <= TDict) {
    if (!key->isA(TInt | TStr)) {
      gen(env, ThrowInvalidArrayKey, extractBase(env), key);
      return false;
    }
  } else if (baseType <= TKeyset) {
    gen(
      env,
      ThrowInvalidOperation,
      cns(env, s_InvalidKeysetOperationMsg.get())
    );
    return false;
  }
  return true;
}

template <typename Finish>
void emitSetElem(IRGS& env, SSATmp* key, SSATmp* value, Finish finish) {
  assertx(key->type().isKnownDataType());
  if (!keyCheckForSet(env, key)) return finish(cns(env, TBottom));
  auto const base = extractBase(env);

  auto const finish2 = [&] (SSATmp* v) {
    gen(env, IncRef, v);
    finish(v);
  };
  emitSet(env, base, key, value, ldMBase(env), finish2);
}

void emitBespokeSetM(IRGS& env, uint32_t nDiscard, MemberKey mk) {
  auto const value = topC(env, BCSPRelOffset{0}, DataTypeGeneric);

  auto const finish = [&] (SSATmp* result) {
    popC(env, DataTypeGeneric);
    mFinalImpl(env, nDiscard, result);
  };

  if (mcodeIsProp(mk.mcode)) PUNT(BespokeSetMProp);
  if (mk.mcode == MW) return finish(emitSetNewElem(env, value));

  assertx(mcodeIsElem(mk.mcode));
  auto const key = memberKey(env, mk);
  emitSetElem(env, key, value, finish);
}

template <typename Finish>
void structDictIncDec(IRGS& env, IncDecOp op, SSATmp* arr, SSATmp* key,
                      Finish finish) {
  assertx(arr->type().arrSpec().is_struct());
  assertx(key->isA(TStr));

  auto const lhsType = arrLikeElemType(
    arr->type(),
    key->type(),
    curClass(env)
  ).first;

  if (!lhsType.maybe(TInt)) {
    return finish(gen(env, IncDecElem, IncDecData{op}, ldMBase(env), key));
  }

  auto const result = structDictMutation(
    env, arr, key, ldMBase(env),
    [&] (SSATmp*, Block*) { return nullptr; },
    [&] (SSATmp*, SSATmp* elem, SSATmp*, SSATmp*, Block* fail) {
      auto const oldVal = [&] {
        auto const v = gen(env, LdMem, TCell, elem);
        if (v->isA(TInt | TUninit)) {
          return gen(env, CheckType, TInt, fail, v);
        } else {
          return cond(
            env,
            [&] (Block* taken) {
              return gen(env, CheckType, TInt, taken, v);
            },
            [&] (SSATmp* r) { return r; },
            [&] {
              hint(env, Block::Hint::Unlikely);
              // We've written the COWed array back to the base already,
              // so set exception boundary to let frame state know that
              // it's okay.
              env.irb->exceptionStackBoundary();
              finish(gen(env, IncDecElem, IncDecData{op}, ldMBase(env), key));
              gen(env, Jmp, makeExit(env, nextSrcKey(env)));
              return cns(env, TBottom);
            }
          );
        }
      }();

      auto const newVal = gen(
        env,
        isInc(op) ? AddInt : SubInt,
        oldVal,
        cns(env, 1)
      );
      gen(env, StMem, elem, newVal);
      return isPre(op) ? newVal : oldVal;
    },
    [&] (SSATmp* arr) {
      hint(env, Block::Hint::Unlikely);
      // We've written the COWed array back to the base already, so
      // set exception boundary to let frame state know that it's
      // okay.
      env.irb->exceptionStackBoundary();
      gen(env, ThrowOutOfBounds, arr, key);
      return cns(env, TBottom);
    }
  );
  finish(result);
}

template <typename Finish>
void structDictSetOp(IRGS& env, SetOpOp op, SSATmp* arr,
                     SSATmp* key, SSATmp* rhs, Finish finish) {
  assertx(arr->type().arrSpec().is_struct());
  assertx(key->isA(TStr));

  auto const lhsType = arrLikeElemType(
    arr->type(),
    key->type(),
    curClass(env)
  ).first;

  auto const type = simpleSetOpType(op);
  if (!type || !lhsType.maybe(*type) || !rhs->isA(*type)) {
    return finish(gen(env, SetOpElem, SetOpData{op}, ldMBase(env), key, rhs));
  }

  auto const result = structDictMutation(
    env, arr, key, ldMBase(env),
    [&] (SSATmp*, Block*) { return nullptr; },
    [&] (SSATmp*, SSATmp* elem, SSATmp*, SSATmp*, Block* fail) {
      auto const oldVal = [&] {
        auto const v = gen(env, LdMem, TCell, elem);
        if (v->isA(*type | TUninit)) {
          return gen(env, CheckType, *type, fail, v);
        } else {
          return cond(
            env,
            [&] (Block* taken) {
              return gen(env, CheckType, *type, taken, v);
            },
            [&] (SSATmp* r) { return r; },
            [&] {
              hint(env, Block::Hint::Unlikely);
              // We've written the COWed array back to the base already,
              // so set exception boundary to let frame state know that
              // it's okay.
              env.irb->exceptionStackBoundary();
              finish(
                gen(env, SetOpElem, SetOpData{op}, ldMBase(env), key, rhs)
              );
              gen(env, Jmp, makeExit(env, nextSrcKey(env)));
              return cns(env, TBottom);
            }
          );
        }
      }();

      auto const newVal = simpleSetOpAction(env, op, oldVal, rhs);
      gen(env, StMem, elem, newVal);
      return newVal;
    },
    [&] (SSATmp* arr) {
      hint(env, Block::Hint::Unlikely);
      // We've written the COWed array back to the base already, so
      // set exception boundary to let frame state know that it's
      // okay.
      env.irb->exceptionStackBoundary();
      gen(env, ThrowOutOfBounds, arr, key);
      return cns(env, TBottom);
    }
  );
  finish(result);
}

void emitBespokeIncDecM(IRGS& env, int32_t nDiscard, IncDecOp incDec,
                        MemberKey mk) {
  if (!mcodeIsElem(mk.mcode)) PUNT(BespokeIncDecM);
  auto const key = memberKey(env, mk);

  auto const finish = [&] (SSATmp* result) {
    mFinalImpl(env, nDiscard, result);
  };

  if (!keyCheckForSet(env, key)) return finish(cns(env, TBottom));

  auto const baseType = env.irb->fs().mbase().type;
  if (baseType.arrSpec().is_struct() && key->isA(TStr)) {
    return structDictIncDec(env, incDec, extractBase(env), key, finish);
  }
  finish(gen(env, IncDecElem, IncDecData{incDec}, ldMBase(env), key));
}

void emitBespokeSetOpM(IRGS& env, int32_t nDiscard, SetOpOp op, MemberKey mk) {
  if (!mcodeIsElem(mk.mcode)) PUNT(BespokeSetOpM);
  auto const key = memberKey(env, mk);

  auto const finish = [&] (SSATmp* result) {
    popDecRef(env);
    mFinalImpl(env, nDiscard, result);
  };

  if (!keyCheckForSet(env, key)) return finish(cns(env, TBottom));

  auto const baseType = env.irb->fs().mbase().type;
  if (baseType.arrSpec().is_struct() && key->isA(TStr)) {
    return structDictSetOp(env, op, extractBase(env), key, topC(env), finish);
  }
  finish(gen(env, SetOpElem, SetOpData{op}, ldMBase(env), key, topC(env)));
}

SSATmp* emitIsset(IRGS& env, SSATmp* key) {
  auto const baseType = env.irb->fs().mbase().type;
  auto const base = extractBase(env);

  if (!key->isA(TInt | TStr)) {
    gen(env, ThrowInvalidArrayKey, base, key);
    return cns(env, TBottom);
  }
  if ((baseType <= TVec) && !key->isA(TInt)) {
    return cns(env, false);
  }

  return cond(
    env,
    [&](Block* taken) { return emitGet(env, base, key, taken); },
    [&](SSATmp* val) { return isType(env, TNonNull, val); },
    [&] { return cns(env, false); }
  );
}

SSATmp* emitGetElem(IRGS& env, SSATmp* key, bool quiet, uint32_t nDiscard) {
  auto const baseType = env.irb->fs().mbase().type;
  auto const base = extractBase(env);

  if (!key->isA(TInt | TStr)) {
    gen(env, ThrowInvalidArrayKey, base, key);
    return cns(env, TBottom);
  }
  if ((baseType <= TVec) && !key->isA(TInt)) {
    if (quiet) return cns(env, TInitNull);

    gen(env, ThrowInvalidArrayKey, base, key);
    return cns(env, TBottom);
  }

  auto const finish = [&](SSATmp* val) {
    gen(env, IncRef, val);
    mFinalImpl(env, nDiscard, val);
  };

  if (quiet) {
    return cond(
      env,
      [&](Block* taken) {
        return emitProfiledGet(env, base, key, taken, finish);
      },
      [&](SSATmp* val) {
        gen(env, IncRef, val);
        return val;
      },
      [&] {
        return cns(env, TInitNull);
      }
    );
  } else {
    auto const val = emitProfiledGetThrow(env, base, key, finish);
    gen(env, IncRef, val);
    return val;
  }
}

void emitBespokeQueryM(
    IRGS& env, uint32_t nDiscard, QueryMOp query, MemberKey mk) {
  if (mk.mcode == MW) PUNT(BespokeQueryMNewElem);
  if (mcodeIsProp(mk.mcode)) PUNT(BespokeQueryMProp);
  auto const key = memberKey(env, mk);
  auto const result = [&] {
    switch (query) {
      case QueryMOp::InOut:
      case QueryMOp::CGet:
        return emitGetElem(env, key, false, nDiscard);
      case QueryMOp::CGetQuiet:
        return emitGetElem(env, key, true, nDiscard);
      case QueryMOp::Isset:
        return emitIsset(env, key);
    }
    not_reached();
  }();
  mFinalImpl(env, nDiscard, result);
}

void emitBespokeIdx(IRGS& env) {
  auto const def = topC(env, BCSPRelOffset{0});
  auto const base = topC(env, BCSPRelOffset{2});
  auto const origKey = topC(env, BCSPRelOffset{1});
  if (!origKey->type().isKnownDataType()) PUNT(Bespoke-Idx-KeyNotKnown);
  auto const key = classConvertPuntOnRaise(env, origKey);

  auto const finish = [&](SSATmp* elem) {
    discard(env, 3);
    pushIncRef(env, elem);
    decRef(env, def, DecRefProfileId::IdxDef);
    decRef(env, key, DecRefProfileId::IdxKey);
    decRef(env, base, DecRefProfileId::IdxBase);
  };

  auto const baseType = base->type();
  if (key->isA(TNull) || ((baseType <= TVec) && key->isA(TNull | TStr))) {
    finish(def);
    return;
  }

  if (!key->isA(TInt) && !key->isA(TStr)) {
    gen(env, ThrowInvalidArrayKey, base, key);
    return;
  }

  auto const res = cond(
    env,
    [&](Block* taken) {
      return emitGet(env, base, key, taken);
    },
    [&](SSATmp* val) { return val; },
    [&] { return def; }
  );
  auto const pres = profiledType(env, res, [&] { finish(res); });
  finish(pres);
}

void emitBespokeAKExists(IRGS& env) {
  auto const base = topC(env, BCSPRelOffset{0});
  auto const origKey = topC(env, BCSPRelOffset{1});
  if (!origKey->type().isKnownDataType()) PUNT(Bespoke-AKExists-KeyNotKnown);
  auto const key = classConvertPuntOnRaise(env, origKey);

  auto const finish = [&](bool res) {
    discard(env, 2);
    push(env, cns(env, res));
    decRef(env, base, DecRefProfileId::AKExistsArr);
    decRef(env, key, DecRefProfileId::AKExistsKey);
  };

  auto const baseType = base->type();
  if ((baseType <= TVec) && key->isA(TStr)) {
    finish(false);
    return;
  } else if (!key->type().subtypeOfAny(TInt, TStr)) {
    gen(env, ThrowInvalidArrayKey, base, key);
    return;
  }

  ifThenElse(
    env,
    [&](Block* taken) {
      return emitGet(env, base, key, taken);
    },
    [&] { finish(true); },
    [&] { finish(false); }
  );
}

SSATmp* tvTempBasePtr(IRGS& env) {
  return gen(env, LdMIStateTempBaseAddr);
}

SSATmp* baseValueToLval(IRGS& env, SSATmp* base) {
  auto const temp = tvTempBasePtr(env);
  gen(env, StMem, temp, base);
  return gen(env, ConvPtrToLval, temp);
}

template <typename Finish>
SSATmp* bespokeElemImpl(IRGS& env, MOpMode mode,
                        Type baseType, SSATmp* key,
                        Finish finish) {
  auto const needsLval = mode == MOpMode::Unset || mode == MOpMode::Define;
  auto const shouldThrow = mode == MOpMode::Warn || mode == MOpMode::InOut ||
                           mode == MOpMode::Define;

  auto const invalid_key = [&] {
    gen(env, ThrowInvalidArrayKey, extractBase(env), key);
    return cns(env, TBottom);
  };

  if ((baseType <= TVec) && key->isA(TStr)) {
    if (shouldThrow) return invalid_key();
    return ptrToInitNull(env);
  }
  if (!key->isA(TInt | TStr)) return invalid_key();

  if (baseType <= TKeyset && needsLval) {
    gen(env, ThrowInvalidOperation,
        cns(env, s_InvalidKeysetOperationMsg.get()));
    return cns(env, TBottom);
  }

  if (needsLval) {
    auto const baseLval = ldMBase(env);
    return emitElem(env, baseLval, baseType, key, shouldThrow);
  } else if (shouldThrow) {
    auto const base = extractBase(env);
    auto const val = emitProfiledGetThrow(env, base, key, [&](SSATmp* val) {
      finish(baseValueToLval(env, val));
    });
    return baseValueToLval(env, val);
  } else {
    return cond(
      env,
      [&](Block* taken) {
        auto const base = extractBase(env);
        return emitProfiledGet(env, base, key, taken, [&](SSATmp* val) {
          finish(baseValueToLval(env, val));
        });
      },
      [&](SSATmp* val) { return baseValueToLval(env, val); },
      [&] { return ptrToInitNull(env); }
    );
  }
}

void emitBespokeDim(IRGS& env, MOpMode mode, MemberKey mk) {
  auto const key = memberKey(env, mk);
  if (mk.mcode == MW) PUNT(BespokeDimNewElem);
  if (mcodeIsProp(mk.mcode)) PUNT(BespokeDimProp);
  assertx(mcodeIsElem(mk.mcode));

  auto const baseType = env.irb->fs().mbase().type;
  auto const finish = [&](SSATmp* base) {
    if (base->isA(TPtr)) base = gen(env, ConvPtrToLval, base);
    assert_flog(base->isA(TLval), "Unexpected mbase: {}", *base->inst());
    gen(env, StMBase, base);
    checkElemDimForReadonly(env);
  };
  auto const base = bespokeElemImpl(env, mode, baseType, key, finish);
  finish(base);
}

void emitBespokeAddElemC(IRGS& env) {
  auto const keyType = topC(env, BCSPRelOffset{1})->type();
  auto const arrType = topC(env, BCSPRelOffset{2})->type();
  if (!(arrType <= TDict)) {
    PUNT(AddElemC-Bespoke-WrongType);
  } else if (!keyType.subtypeOfAny(TInt, TStr, TCls, TLazyCls)) {
    interpOne(env, arrType.unspecialize(), 3);
    return;
  }

  auto const value = popC(env, DataTypeGeneric);
  auto const key = classConvertPuntOnRaise(env, popC(env));
  auto const arrLoc = ldStkAddr(env, BCSPRelOffset{0});
  auto const arr = gen(env, LdMem, arrType, arrLoc);

  auto const finish = [&] (SSATmp*) { decRef(env, key); };
  emitSet(env, arr, key, value, arrLoc, finish);
}

void emitBespokeAddNewElemC(IRGS& env) {
  auto const arrType = topC(env, BCSPRelOffset{1})->type();
  if (!arrType.subtypeOfAny(TKeyset, TVec)) {
    PUNT(AddNewElemC-Bespoke-WrongType);
  }

  auto const value = popC(env, DataTypeGeneric);
  auto const arr = popC(env);
  auto const newArr = emitAppend(env, arr, value);
  push(env, newArr);
}

void emitBespokeColFromArray(IRGS& env,
                             CollectionType type) {
  assertx(type != CollectionType::Pair);
  auto const arr = popC(env);
  auto const arrType = arr->type();
  if (!arrType.subtypeOfAny(TVec, TDict)) PUNT(Bespoke-BadColType);
  if (arrType <= TVec &&
      !(type == CollectionType::Vector || type == CollectionType::ImmVector)) {
    PUNT(Bespoke-ColTypeMismatch);
  }
  if (arrType <= TDict &&
      (type == CollectionType::Vector || type == CollectionType::ImmVector)) {
    PUNT(Bespoke-ColTypeMismatch);
  }
  auto const vanilla = emitEscalateToVanilla(env, arr, s_ColFromArray);
  auto const col = gen(env, NewColFromArray, NewColData { type }, vanilla);
  push(env, col);
}

void classGetTSImpl(IRGS& env, bool pushGenerics) {
  auto const arr = topC(env);
  auto const arrType = arr->type();
  if (!(arrType <= TDict)) {
    if (arrType.maybe(TDict)) {
      PUNT(Bespoke-ClassGetTSWithGenerics-UnguardedTS);
    } else {
      gen(env, RaiseError, cns(env, s_reified_type_must_be_ts.get()));
      return;
    }
  }

  if (pushGenerics) {
    auto const generics = cns(env, s_generic_types.get());
    ifElse(
      env,
      [&](Block* taken) { emitGet(env, arr, generics, taken); },
      [&] { gen(env, Jmp, makeExitSlow(env)); }
    );
  }

  auto const classKey = cns(env, s_classname.get());
  auto const classVal = cond(
    env,
    [&](Block* taken) {
      return emitGet(env, arr, classKey, taken);
    },
    [&] (SSATmp* val) { return val; },
    [&] {
      gen(env, ThrowArrayKeyException, arr, classKey);
      return cns(env, TBottom);
    }
  );

  auto const className = cond(
    env,
    [&] (Block* taken) {
      return gen(env, CheckType, TStr, taken, classVal);
    },
    [&] (SSATmp* val) { return val; },
    [&] {
      hint(env, Block::Hint::Unlikely);
      gen(env, RaiseError, cns(env, s_new_instance_of_not_string.get()));
      return cns(env, TBottom);
    }
  );

  auto const cls = ldCls(env, className);
  popDecRef(env);
  push(env, cls);
  if (pushGenerics) push(env, cns(env, staticEmptyVec()));
}

void emitBespokeClassGetTS(IRGS& env) {
  classGetTSImpl(env, false);
}

void emitBespokeClassGetTSWithGenerics(IRGS& env) {
  classGetTSImpl(env, true);
}

void emitBespokeShapesAt(IRGS& env, int32_t numArgs) {
  assertx(numArgs == 2);
  auto const arr = topC(env, BCSPRelOffset{1});
  auto const key = topC(env, BCSPRelOffset{0});
  assertx(arr->isA(TDict));
  assertx(key->type().subtypeOfAny(TInt, TStr));

  auto const finish = [&](SSATmp* val) {
    discard(env, 2 + kNumActRecCells);
    pushIncRef(env, val);
    decRef(env, arr, DecRefProfileId::AtArr);
    decRef(env, key, DecRefProfileId::AtKey);
  };

  auto const exit = makeExitSlow(env);
  finish(emitProfiledGet(env, arr, key, exit, finish));
}

void emitBespokeShapesIdx(IRGS& env, int32_t numArgs) {
  assertx(numArgs == 2 || numArgs == 3);
  auto const arr = topC(env, BCSPRelOffset{numArgs - 1});
  auto const key = topC(env, BCSPRelOffset{numArgs - 2});
  auto const def = numArgs == 3 ? topC(env) : cns(env, TInitNull);
  assertx(arr->isA(TDict));
  assertx(key->type().subtypeOfAny(TInt, TStr));

  auto const finish = [&](SSATmp* val) {
    discard(env, numArgs + kNumActRecCells);
    pushIncRef(env, val);
    decRef(env, arr, DecRefProfileId::IdxBase);
    decRef(env, key, DecRefProfileId::IdxKey);
    decRef(env, def, DecRefProfileId::IdxDef);
  };

  auto const val = cond(
    env,
    [&](Block* taken) { return emitGet(env, arr, key, taken); },
    [&](SSATmp* val) { return val; },
    [&] { return def; }
  );
  finish(profiledType(env, val, [&] { finish(val); }));
}

void emitBespokeShapesExists(IRGS& env, int32_t numArgs) {
  assertx(numArgs == 2);
  auto const arr = topC(env, BCSPRelOffset{1});
  auto const key = topC(env, BCSPRelOffset{0});
  assertx(arr->isA(TDict));
  assertx(key->type().subtypeOfAny(TInt, TStr));

  auto const val = cond(
    env,
    [&](Block* taken) { emitGet(env, arr, key, taken); },
    [&] { return cns(env, true); },
    [&] { return cns(env, false); }
  );

  discard(env, 2 + kNumActRecCells);
  push(env, val);
  decRef(env, arr, DecRefProfileId::AKExistsArr);
  decRef(env, key, DecRefProfileId::AKExistsKey);
}

///////////////////////////////////////////////////////////////////////////////

enum class LayoutSensitiveCall {
  ShapesAt,
  ShapesIdx,
  ShapesExists,
};

const StaticString
  s_HH_Shapes("HH\\Shapes"),
  s_HH_Readonly_Shapes("HH\\Readonly\\Shapes"),
  s_at("at"),
  s_idx("idx"),
  s_keyExists("keyExists");

// If this call is to a layout-sensitive callee, returns it. Does not do any
// validation of e.g. whether we can inline the callee.
Optional<LayoutSensitiveCall>
getLayoutSensitiveCall(const IRGS& env, SrcKey sk) {
  if (sk.anyFuncEntry()) return std::nullopt;
  if (sk.op() != Op::FCallClsMethodD) return std::nullopt;

  auto const cls  = sk.unit()->lookupLitstrId(getImm(sk.pc(), 1).u_SA);
  auto const meth = sk.unit()->lookupLitstrId(getImm(sk.pc(), 2).u_SA);

  if (!cls->tsame(s_HH_Shapes.get()) &&
      !cls->tsame(s_HH_Readonly_Shapes.get())) {
    return std::nullopt;
  }

  if (meth == s_at.get())        return LayoutSensitiveCall::ShapesAt;
  if (meth == s_idx.get())       return LayoutSensitiveCall::ShapesIdx;
  if (meth == s_keyExists.get()) return LayoutSensitiveCall::ShapesExists;

  return std::nullopt;
}

// If this call is layout-sensitive and we can specialize it, returns a pair
// of locations to guard: {arr, key}. Otherwise, returns an empty vector.
jit::vector<Location> guardsForLayoutSensitiveCall(const IRGS& env, SrcKey sk) {
  auto const call = getLayoutSensitiveCall(env, sk);
  if (!call) return {};
  auto const fca = getImm(sk.pc(), 0).u_FCA;

  if (fca.numRets != 1) return {};
  if (fca.hasGenerics() || fca.hasUnpack()) return {};

  auto const hasInOut = [&]{
    if (!fca.enforceInOut()) return false;
    for (auto i = 0; i < fca.numArgs; i++) {
      if (fca.isInOut(i)) return true;
    }
    return false;
  }();
  if (hasInOut) return {};

  auto const numArgsOkay = [&]{
    if (fca.numRets != 1) return false;
    auto const is_shapes_idx = call == LayoutSensitiveCall::ShapesIdx;
    return fca.numArgs == 2 || (fca.numArgs == 3 && is_shapes_idx);
  }();
  if (!numArgsOkay) return {};

  auto const readonlyOkay = [&]{
    auto const callee = [&]() -> Func* {
      auto const className  = sk.unit()->lookupLitstrId(getImm(sk.pc(), 1).u_SA);
      auto const funcName = sk.unit()->lookupLitstrId(getImm(sk.pc(), 2).u_SA);
      auto const cls = Class::lookup(className);
      if (!cls) return nullptr;
      return cls->lookupMethod(funcName);
    }();
    if (!callee) return false;
    if (fca.enforceReadonly()) {
      for (auto i = 0; i < fca.numArgs; ++i) {
        if (fca.isReadonly(i) && !callee->isReadonly(i)) return false;
      }
    }
    if (fca.enforceMutableReturn() && (callee->attrs() & AttrReadonlyReturn)) {
      return false;
    }
    if (fca.enforceReadonlyThis() && !(callee->attrs() & AttrReadonlyThis)) {
      return false;
    }
    return true;
  }();
  if (!readonlyOkay) return {};

  auto const numArgs = safe_cast<int32_t>(fca.numArgs);
  auto const al = Location::Stack{env.irb->fs().bcSPOff() - numArgs + 1};
  auto const kl = Location::Stack{env.irb->fs().bcSPOff() - numArgs + 2};
  return {al, kl};
}

// Check if we can specialize a layout-sensitive call. This method does all
// checks on param counts, inouts, generics, etc. needed to allow inlining.
bool canSpecializeCall(const IRGS& env, SrcKey sk) {
  auto const locations = guardsForLayoutSensitiveCall(env, sk);
  if (locations.empty()) return false;

  assertx(locations.size() == 2);
  auto const al = locations[0];
  auto const kl = locations[1];
  auto const arr = env.irb->typeOf(al, DataTypeSpecific);
  auto const key = env.irb->typeOf(kl, DataTypeSpecific);
  FTRACE_MOD(Trace::hhir, 3, "Guarded arguments of layout-sensitive call:\n"
             "  arr: {} @ {}\n  key: {} @ {}\n", arr, show(al), key, show(kl));

  auto const specialize = (arr <= TDict) && key.subtypeOfAny(TInt, TStr);
  FTRACE_MOD(Trace::hhir, 3, "Types {}pecializing call on layout.\n",
             specialize ? "match. S" : "do not match. Not s");
  return specialize;
}

///////////////////////////////////////////////////////////////////////////////

// This mechanism is deprecated and being removed. Implement bespoke handling
// as part of regular bytecode implementation.
void emitBespoke(IRGS& env, const NormalizedInstruction& ni,
                 std::function<void(IRGS&)> emit) {
  auto const DEBUG_ONLY sk = ni.source;
  FTRACE_MOD(Trace::hhir, 2, "At {}: {}: perform bespoke translation\n",
             sk.offset(), opcodeToName(sk.op()));
  switch (ni.op()) {
    case Op::IterGetKey:
    case Op::IterGetValue:
    case Op::IterSetValue:
    case Op::IterInit:
    case Op::IterNext:
    case Op::UnsetM:
      // Standard implementation of these bytecodes handles all situations,
      // but benefits from specialized array typing.
      emit(env);
      return;
    case Op::QueryM:
      emitBespokeQueryM(env, ni.imm[0].u_IVA, (QueryMOp) ni.imm[1].u_OA,
                        ni.imm[2].u_KA);
      return;
    case Op::SetM:
      emitBespokeSetM(env, ni.imm[0].u_IVA, ni.imm[1].u_KA);
      return;
    case Op::IncDecM:
      emitBespokeIncDecM(env, ni.imm[0].u_IVA, (IncDecOp)ni.imm[1].u_OA,
                         ni.imm[2].u_KA);
      return;
    case Op::SetOpM:
      emitBespokeSetOpM(env, ni.imm[0].u_IVA, (SetOpOp)ni.imm[1].u_OA,
                        ni.imm[2].u_KA);
      return;
    case Op::Idx:
    case Op::ArrayIdx:
      emitBespokeIdx(env);
      return;
    case Op::AKExists:
      emitBespokeAKExists(env);
      return;
    case Op::Dim:
      emitBespokeDim(env, (MOpMode) ni.imm[0].u_OA, ni.imm[1].u_KA);
      return;
    case Op::AddElemC:
      emitBespokeAddElemC(env);
      return;
    case Op::AddNewElemC:
      emitBespokeAddNewElemC(env);
      return;
    case Op::ColFromArray:
      emitBespokeColFromArray(env, (CollectionType) ni.imm[0].u_OA);
      return;
    case Op::ClassGetTS:
      emitBespokeClassGetTS(env);
      return;
    case Op::ClassGetTSWithGenerics:
      emitBespokeClassGetTSWithGenerics(env);
      return;
    case Op::FCallClsMethodD: {
      using LSC = LayoutSensitiveCall;
      auto const call = getLayoutSensitiveCall(env, ni.source);
      auto const numArgs = safe_cast<int32_t>(ni.imm[0].u_FCA.numArgs);
      switch (*call) {
        case LSC::ShapesAt:     return emitBespokeShapesAt(env, numArgs);
        case LSC::ShapesIdx:    return emitBespokeShapesIdx(env, numArgs);
        case LSC::ShapesExists: return emitBespokeShapesExists(env, numArgs);
      }
    }
    default:
      SCOPE_ASSERT_DETAIL("Unhandled Bespoke op") { return ni.toString(); };
      always_assert(false);
  }
}

Optional<Location> getVanillaLocation(const IRGS& env, SrcKey sk) {
  auto const op = sk.op();
  auto const soff = env.irb->fs().bcSPOff();

  if (isMemberDimOp(op) || (isMemberFinalOp(op) && op != Op::SetRangeM)) {
    return {Location::MBase{}};
  }

  switch (op) {
    // Array accesses constrain the base.
    case Op::Idx:
    case Op::ArrayIdx:
    case Op::AddElemC:
      return {Location::Stack{soff - 2}};
    case Op::AddNewElemC:
      return {Location::Stack{soff - 1}};
    case Op::AKExists:
    case Op::ClassGetTS:
    case Op::ClassGetTSWithGenerics:
    case Op::ColFromArray:
      return {Location::Stack{soff}};

    // Local iterators constrain the local base.
    case Op::IterGetKey:
    case Op::IterGetValue:
    case Op::IterSetValue:
    case Op::IterInit:
    case Op::IterNext: {
      auto const local = getImm(sk.pc(), localImmIdx(op)).u_LA;
      return {Location::Local{safe_cast<uint32_t>(local)}};
    }

    case Op::FCallClsMethodD: {
      if (!canSpecializeCall(env, sk)) return std::nullopt;
      auto const numArgs = safe_cast<int32_t>(getImm(sk.pc(), 0).u_FCA.numArgs);
      return {Location::Stack{soff - numArgs + 1}};
    }

    default:
      return std::nullopt;
  }
  always_assert(false);
}

// Returns a location that we should do some layout-sensitive guards for.
// Unlike getVanillaLocation, this helper checks known types.
Optional<Location> getLocationToGuard(const IRGS& env, SrcKey sk) {
  // If this check fails, the bytecode is not layout-sensitive.
  auto const loc = getVanillaLocation(env, sk);
  if (!loc) return std::nullopt;

  // Even if the bytecode is layout-sensitive, it may be applied to e.g. an
  // object input, or our known types may be too general for us to guard.
  auto const type = env.irb->typeOf(*loc, DataTypeSpecific);
  auto const needsGuard = type != TBottom && type <= TArrLike &&
                          typeFitsConstraint(type, DataTypeSpecific);
  FTRACE_MOD(Trace::hhir, 2, "At {}: {}: location {}: {} {} layout guard\n",
             sk.offset(), opcodeToName(sk.op()), show(*loc), type,
             needsGuard ? "needs" : "does not need");
  return needsGuard ? loc : std::nullopt;
}

// Decide on what layout to specialize code for. In live translations,
// we simply use the known layout of the array, which allows us to completely
// avoid guarding on layouts (and thus over-specializing these translations).
//
// In optimized translations, we use the layout chosen by layout selection,
// emitting a check if necessary to refine the array's type to that layout.
jit::ArrayLayout guardToLayout(
    IRGS& env, const NormalizedInstruction& ni, Location loc,
    Type type, std::function<void(IRGS&)> emitVanilla,
    const bespoke::SinkLayouts& sinkLayouts) {
  assertx(sinkLayouts.layouts.size() == 1);

  auto const kind = env.context.kind;
  if (kind != TransKind::Optimize) return type.arrSpec().layout();

  auto const sl = sinkLayouts.layouts[0];
  auto const target = TArrLike.narrowToLayout(sl.layout);
  if (type <= target || !type.maybe(target)) {
    // If the type test would be trivial (either always taken or never taken),
    // skip it and just emit code that works for the current layout.
    return type.arrSpec().layout();
  }

  if (sinkLayouts.sideExit && !Cfg::Eval::BespokeEscalationSampleRate) {
    checkType(env, loc, target, makeExit(env));
    return sl.layout;
  }

  auto const next_hint = env.irb->curBlock()->hint();

  ifThen(
    env,
    [&](Block* taken) {
      checkType(env, loc, target, taken);
    },
    [&]{
      hint(env, Block::Hint::Unlikely);
      IRUnit::Hinter hinter(env.irb->unit(), Block::Hint::Unlikely);

      // If the type check was for "vanilla" or "bespoke", we know we have
      // the other kind in the taken branch, so assert that information.
      auto const vanilla = TArrLike.narrowToLayout(ArrayLayout::Vanilla());
      auto const bespoke = TArrLike.narrowToLayout(ArrayLayout::Bespoke());
      if (target == vanilla) {
        assertTypeLocation(env, loc, bespoke);
      } else if (target == bespoke) {
        assertTypeLocation(env, loc, vanilla);
      }

      // Side exit or do codegen as needed. Use emitVanilla if we have it.
      if (sinkLayouts.sideExit) {
        assertx(Cfg::Eval::BespokeEscalationSampleRate);
        auto const arr = loadLocation(env, loc);
        gen(env, LogGuardFailure, target, arr);
        gen(env, Jmp, makeExit(env));
      } else {
        if (emitVanilla && target == bespoke) {
          emitVanilla(env);
        } else {
          emitBespoke(env, ni, emitVanilla);
        }
        auto const next = [&]{
          IRUnit::Hinter next_hinter(env.irb->unit(), next_hint);
          return getBlock(env, nextSrcKey(env));
        }();
        gen(env, Jmp, next);
      }
    }
  );
  return sl.layout;
}

// In optimized translations, use the layouts passed in to guard and emit
// translations. Each layout will be checked to refine the array's type and
// translation will be emitted accordingly.
void guardToMultipleLayoutsAndEmit(
    IRGS& env, const NormalizedInstruction& ni, Location loc,
    Type type, std::function<void(IRGS&)> emitVanilla,
    const bespoke::SinkLayouts& sinkLayouts) {
  assertx(sinkLayouts.layouts.size() > 1);

  auto const emitTranslation = [&](const bool vanilla) {
    vanilla && emitVanilla
      ? emitVanilla(env)
      : emitBespoke(env, ni, emitVanilla);
  };

  auto const kind = env.context.kind;
  if (kind != TransKind::Optimize) {
    emitTranslation(type.arrSpec().layout().vanilla());
    return;
  }

  for (auto const& sl: sinkLayouts.layouts) {
    auto target = TArrLike.narrowToLayout(sl.layout);
    if (type <= target || !type.maybe(target)) {
      // If the type test would be trivial (either always taken or never taken),
      // skip it and just emit code that works for the current layout.
      emitTranslation(type.arrSpec().layout().vanilla());
      return;
    }

    if (sinkLayouts.sideExit && !Cfg::Eval::BespokeEscalationSampleRate) {
      checkType(env, loc, target, makeExit(env));
      emitTranslation(sl.layout.vanilla());
      return;
    }
  }

  auto const vanilla = TArrLike.narrowToLayout(ArrayLayout::Vanilla());
  auto const bespoke = TArrLike.narrowToLayout(ArrayLayout::Bespoke());

  // Generic fallback for taken branch.
  auto const fallback = [&](const jit::Type& target){
    hint(env, Block::Hint::Unlikely);
    IRUnit::Hinter hinter(env.irb->unit(), Block::Hint::Unlikely);

    // If the type check was for "vanilla" or "bespoke", we know we have
    // the other kind in the taken branch, so assert that information.
    if (target == vanilla) {
      assertTypeLocation(env, loc, bespoke);
    } else if (target == bespoke) {
      assertTypeLocation(env, loc, vanilla);
    }

    // Side exit or do codegen as needed. Use emitVanilla if we have it.
    if (sinkLayouts.sideExit) {
      assertx(Cfg::Eval::BespokeEscalationSampleRate);
      auto const arr = loadLocation(env, loc);
      gen(env, LogGuardFailure, target, arr);
      gen(env, Jmp, makeExit(env, curSrcKey(env)));
    } else {
      emitTranslation(target == bespoke);
      auto const next = getBlock(env, nextSrcKey(env));
      gen(env, Jmp, next);
    }
  };

  // Check each layout, otherwise fallback
  MultiCond mc{env};
  Type target;
  for (auto const& sl : sinkLayouts.layouts) {
    target = TArrLike.narrowToLayout(sl.layout);

    mc.ifThen(
      [&](Block* taken) {
        checkType(env, loc, target, taken);
        // Dead-code, but needed to satisfy MultiCond
        return cns(env, staticEmptyString());
      },
      [&](SSATmp* /* unused */) {
        emitTranslation(target == vanilla);
        // Dead-code, but needed to satisfy MultiCond
        return cns(env, staticEmptyString());
      });
  }

  mc.elseDo([&]{
    fallback(target);
    // Dead-code, but needed to satisfy MultiCond
    return cns(env, staticEmptyString());
  });
}

void emitLogArrayReach(IRGS& env, Location loc, SrcKey sk) {
  // We won't have a tracelet ID to use during tracelet-selection time.
  // In profiling tracelets, we should have exactly one tracelet ID to log.
  if (env.formingRegion || env.context.kind != TransKind::Profile) return;
  assertx(env.context.transIDs.size() == 1);

  auto const transID = *env.context.transIDs.begin();
  auto const profile = bespoke::getSinkProfile(transID, sk);
  if (!profile) return;

  auto const arr = loadLocation(env, loc);
  gen(env, LogArrayReach, SinkProfileData(profile), arr);
}

// In a profiling tracelet, we don't want to guard on the array being vanilla,
// so we emit code to handle both vanilla and logging arrays.
void emitLoggingDiamond(
    IRGS& env, const NormalizedInstruction& ni, Location loc,
    std::function<void(IRGS&)> emitVanilla) {
  assertx(env.context.kind == TransKind::Profile);

  auto const dropArrSpec = [&](Type type) {
    return type.arrSpec() ? type.unspecialize() : type;
  };
  std::vector<Type> vanillaLocalTypes;
  std::vector<Type> vanillaStackTypes;
  ifThen(
    env,
    [&](Block* taken) {
      checkType(env, loc, TVanillaArrLike, taken);

      emitVanilla(env);

      // We have a vanilla and a logging side of the diamond. The logging side
      // may have lost type information because of using InterpOne.  We will
      // emit AssertTypes with the type information from the vanilla side after
      // the generated code on the logging side to regain this information.
      auto const locals = curFunc(env)->numLocals();
      auto const pushed = getStackPushed(ni.source.pc());
      vanillaLocalTypes.reserve(locals);
      vanillaStackTypes.reserve(pushed);
      for (uint32_t i = 0; i < locals; i ++) {
        auto const lType = env.irb->fs().local(i).type;
        vanillaLocalTypes.push_back(dropArrSpec(lType));
      }
      for (int32_t i = 0; i < pushed; i ++) {
        auto const idx = BCSPRelOffset{-i};
        auto const sType = env.irb->fs().stack(offsetFromIRSP(env, idx)).type;
        vanillaStackTypes.push_back(dropArrSpec(sType));
      }
    },
    [&] {
      hint(env, Block::Hint::Unlikely);

      auto const layout = ArrayLayout::Bespoke();
      auto const type = TArrLike.narrowToLayout(layout);
      assertTypeLocation(env, loc, type);

      try {
        emitBespoke(env, ni, emitVanilla);
      } catch ([[maybe_unused]] const FailedIRGen& exn) {
        FTRACE_MOD(Trace::region, 1,
          "bespoke irgen for {} failed with {} while vanilla irgen succeeded\n",
           ni.toString(), exn.what());
        throw;
      }

      if (debug) {
        // For layout-sensitive bytecodes, opcodeChangesPC implies that
        // opcodeBreaksBB and we are at the end of the tracelet. Therefore we
        // don't need to worry about control flow after the InterpOneCF.
        auto const op = curFunc(env)->getOp(bcOff(env));
        auto const opChangePC = opcodeChangesPC(op);
        always_assert(IMPLIES(opChangePC, opcodeBreaksBB(op, false)));

        // IncDecM and SetOpM might produce worse stack types right
        // now, as irgen-minstr optimizes certain cases while
        // irgen-bespoke does not yet.
        // IterInit and IterNext might produce back control flow edges that
        // reset type information.
        if (ni.op() != Op::IncDecM && ni.op() != Op::SetOpM &&
            !isIteratorControlFlow(ni.op())) {
          for (uint32_t i = 0; i < vanillaLocalTypes.size(); i ++) {
            always_assert_flog(
              env.irb->fs().local(i).type <= vanillaLocalTypes[i],
              "Local {}: expected type: {}, inferred type: {}",
              i, vanillaLocalTypes[i], env.irb->fs().local(i).type
            );
          }

          for (int32_t i = 0; i < vanillaStackTypes.size(); i ++) {
            auto const offset = offsetFromIRSP(env, BCSPRelOffset{-i});
            always_assert_flog(
              env.irb->fs().stack(offset).type <= vanillaStackTypes[i],
              "Stack {}: expected type: {}, inferred type: {}",
              i, vanillaStackTypes[i], env.irb->fs().stack(offset).type
            );
          }
        }
      }
    }
  );
}

bool canProfilePropsInline(const Class* cls) {
  if (!cls) return false;
  if (!cls->pinitVec().empty()) return false;
  if (cls->hasReifiedGenerics() || cls->hasReifiedParent()) return false;

  auto num_array_props = 0;
  auto const limit = Cfg::HHIR::InliningMaxInitObjProps;
  for (auto slot = 0; slot < cls->numDeclProperties(); slot++) {
    if (cls->declProperties()[slot].attrs & AttrIsConst) return false;
    auto const index = cls->propSlotToIndex(slot);
    auto const tv = cls->declPropInit()[index].val.tv();
    if (!tvIsArrayLike(tv)) continue;
    if ((++num_array_props) > limit) return false;
  }
  return true;
}

LoggingProfileData makeLoggingProfileData(bespoke::LoggingProfile* profile) {
  auto const isStatic = profile->data && profile->data->staticLoggingArray;
  return LoggingProfileData(profile, isStatic);
}

void profileArrLikeProps(IRGS& env) {
  auto const obj = topC(env);
  auto const cls = obj->type().clsSpec().exactCls();

  if (cls && cls->needsInitThrowable()) return;

  if (!canProfilePropsInline(cls)) {
    gen(env, ProfileArrLikeProps, obj);
    return;
  }

  for (auto slot = 0; slot < cls->numDeclProperties(); slot++) {
    auto const index = cls->propSlotToIndex(slot);
    auto const tv = cls->declPropInit()[index].val.tv();
    if (!tvIsArrayLike(tv)) continue;
    if (!arrayTypeCouldBeBespoke(tv.val().parr->toDataType())) continue;
    auto const profile = bespoke::getLoggingProfile(
      cls, slot, bespoke::LocationType::InstanceProperty
    );
    if (!profile) continue;

    auto const arr = gen(
      env,
      NewLoggingArray,
      makeLoggingProfileData(profile),
      cns(env, tv.val().parr)
    );
    auto const data = IndexData(index);
    auto const addr = gen(env, LdPropAddr, data, TCell, obj);
    gen(env, StMem, addr, arr);
  }
}

void profileSource(IRGS& env, SrcKey sk) {
  DEBUG_ONLY auto const op = sk.op();
  assertx(isArrLikeConstructorOp(op) || isArrLikeCastOp(op));
  auto const newArr = topC(env);
  assertx(newArr->type().isKnownDataType());
  if (!arrayTypeCouldBeBespoke(newArr->type().toDataType())) return;

  auto const profile = bespoke::getLoggingProfile(sk);
  if (!profile) return;
  auto const data = makeLoggingProfileData(profile);
  push(env, gen(env, NewLoggingArray, data, popC(env)));
}

void skipTrivialCast(IRGS& env, Op op) {
  assertx(isArrLikeCastOp(op));
  auto const type = [&]{
    switch (op) {
      case OpCastVec:    return TVec;
      case OpCastDict:   return TDict;
      case OpCastKeyset: return TKeyset;
      default: always_assert(false);
    }
  }();

  auto const input = topC(env);
  if (!input->type().maybe(type)) return;

  auto const next = getBlock(env, nextSrcKey(env));
  if (input->isA(type)) gen(env, Jmp, next);
  ifElse(env,
    [&](Block* taken) { gen(env, CheckType, type, taken, input); },
    [&]{ gen(env, Jmp, next); }
  );
}

bool specializeStructSource(IRGS& env, SrcKey sk, ArrayLayout layout) {
  auto const op = sk.op();
  assertx(op == Op::NewDictArray || op == Op::NewStructDict);

  if (op == Op::NewDictArray) {
    auto const data = ArrayLayoutData { layout };
    auto const arr = gen(env, AllocBespokeStructDict, data);
    push(env, arr);
    return true;
  }

  auto const gc = DataTypeGeneric;
  auto const imms = getImmVector(sk.pc());
  auto const size = safe_cast<uint32_t>(imms.size());
  auto const slayout = bespoke::StructLayout::As(layout.bespokeLayout());

  // Validate that the layout is compatible with the requested array. We must
  // check that all required fields are set, and that all type bounds *may* be
  // met by the provided value for that field.
  auto guards = std::vector<std::pair<int32_t, Type>>{};
  auto required = slayout->numRequiredFields();
  for (auto i = 0; i < size; i++) {
    auto const key = curUnit(env)->lookupLitstrId(imms.vec32()[i]);
    auto const slot = slayout->keySlot(key);
    if (slot == kInvalidSlot) return false;
    if (slayout->field(slot).required) required--;

    // Compute the type to check for this field. If the value cannot possibly
    // meet this bound, profiling has gone awry and we should bail.
    auto const offset = safe_cast<int32_t>(size - i - 1);
    auto const known = topType(env, BCSPRelOffset{offset}, gc);
    auto const bound = slayout->getTypeBound(slot);
    auto const type = bound.isKnownDataType() ? bound : bound | TUninit;
    if (!known.maybe(type)) return false;
    if (!(known <= type)) guards.push_back({offset, type});
  }
  if (required) return false;

  // Check any type bounds that are not already known to be satisfied.
  if (!guards.empty()) {
    auto const exit = makeExitSlow(env);
    for (auto const& guard : guards) {
      auto const soff = BCSPRelOffset{guard.first};
      gen(env, CheckType, guard.second, exit, topC(env, soff, gc));
    }
  }

  // All preconditions have been checked. Write the slots to this unit's data
  // section, and then either emit an outlined or inline array constructor.
  auto const slots = size ? new (env.unit.arena()) Slot[size] : nullptr;
  for (auto i = 0; i < size; i++) {
    auto const key = curUnit(env)->lookupLitstrId(imms.vec32()[i]);
    slots[i] = slayout->keySlot(key);
    assertx(slots[i] != kInvalidSlot);
  }

  if (size > Cfg::HHIR::MaxInlineInitStructElements) {
    auto const data = NewBespokeStructData {
        layout, spOffBCFromIRSP(env), safe_cast<uint32_t>(size), slots};
    auto const arr = gen(env, NewBespokeStructDict, data, sp(env));
    discard(env, size);
    push(env, arr);
    return true;
  }

  auto const data = ArrayLayoutData { layout };
  auto const positions = InitStructPositionsData {
      layout, safe_cast<uint32_t>(size), slots };
  auto const arr = gen(env, AllocBespokeStructDict, data);
  gen(env, InitStructPositions, positions, arr);
  for (auto i = 0; i < size; ++i) {
    auto const idx = size - i - 1;
    auto const key = curUnit(env)->lookupLitstrId(imms.vec32()[idx]);
    auto const kid = KeyedIndexData { slots[idx], key };
    gen(env, InitStructElem, kid, arr, popC(env, gc));
  }
  push(env, arr);
  return true;
}

bool specializeSource(IRGS& env, SrcKey sk) {
  DEBUG_ONLY auto const op = sk.op();
  assertx(isArrLikeConstructorOp(op) || isArrLikeCastOp(op));
  auto const kind = env.context.kind;
  if (kind != TransKind::Live && kind != TransKind::Optimize) return false;
  auto const profile = bespoke::getLoggingProfile(sk);
  auto const layout = profile ? profile->getLayout() : ArrayLayout::Top();
  if (!layout.bespoke()) return false;

  if (auto const bad = profile->getStaticBespokeArray()) {
    assertx(arrayTypeCouldBeBespoke(bad->toDataType()));
    assertx(isArrLikeConstructorOp(op));
    push(env, cns(env, bad));
    return true;
  }

  assertx(layout.is_struct());
  return specializeStructSource(env, sk, layout);
}

///////////////////////////////////////////////////////////////////////////////

/*
 * Handle a bespoke array source.
 *
 * Arrays that are property initial values are easy to handle, because we only
 * need to emit code here when profiling. If we select a bespoke layout for a
 * property initial value, we'll update the object's static initial value and
 * regular irgen will do the right thing.
 *
 * Otherwise, we have an array-like constructor or cast op:
 *
 *  - When profiling, emit the vanilla IR, then conditionally wrap the result
 *    in a LoggingArray via a call to NewLoggingArray.
 *
 *  - When optimizing, look up the source's layout, and if it's bespoke, emit
 *    IR to construct an array of that layout instead of a vanilla one.
 */
bool handleSource(IRGS& env, SrcKey sk,
                  std::function<void(IRGS&)> emitVanilla) {
  auto const op = sk.op();
  auto const profile = env.context.kind == TransKind::Profile ||
                       shouldTestBespokeArrayLikes();
  if (profile && isObjectConstructorOp(op)) {
    emitVanilla(env);
    profileArrLikeProps(env);
    return true;
  }

  if (!isArrLikeConstructorOp(op) && !isArrLikeCastOp(op)) return false;
  if (!profile) return specializeSource(env, sk);

  if (isArrLikeCastOp(op)) {
    skipTrivialCast(env, op);
  }
  emitVanilla(env);
  profileSource(env, sk);
  return true;
}

/*
 * Handle a bespoke sink that takes an array-like input at `loc`. Some ops are
 * handled specially, but the basic flow is:
 *
 *  - When profiling, update the sink profile via a LogArrayReach call. Then
 *    branch on whether the array is bespoke, emitting a diamond with vanilla
 *    IR on one side and BespokeTop IR on the other.
 *
 *  - When optimizing, look up the sink's layout and specialize code for it.
 *    Call into vanilla IR if the selected layout is vanilla.
 */
void handleSink(IRGS& env, const NormalizedInstruction& ni,
                std::function<void(IRGS&)> emitVanilla, Location loc) {
  auto const sk = ni.source;
  auto const type = env.irb->typeOf(loc, DataTypeGeneric);
  assertx(type <= TArrLike);
  if (type.isKnownDataType() && !arrayTypeCouldBeBespoke(type.toDataType())) {
    assertTypeLocation(env, loc, TVanillaArrLike);
    return emitVanilla(env);
  }

  emitLogArrayReach(env, loc, sk);

  auto const sinkLayouts = bespoke::layoutsForSink(
    env.profTransIDs, ni.source, type.arrSpec().layout());

  if (isFCall(sk.op())) {
    assertx(sinkLayouts.layouts.size() > 0);
    if (sinkLayouts.layouts.size() == 1) {
      guardToLayout(env, ni, loc, type, nullptr, sinkLayouts);
      emitBespoke(env, ni, emitVanilla);
    } else {
      guardToMultipleLayoutsAndEmit(env, ni, loc, type, nullptr, sinkLayouts);
    }
  } else if (env.context.kind == TransKind::Profile) {
    // In a profiling tracelet, we'll emit a diamond that handles vanilla
    // array-likes on one side and bespoke array-likes on the other.
    if (type.arrSpec().vanilla()) {
      emitVanilla(env);
    } else {
      emitLoggingDiamond(env, ni, loc, emitVanilla);
    }
  } else {
    // In an optimized or live translation, we guard to a specialized layout
    // and then emit either vanilla or bespoke code.
    assertx(sinkLayouts.layouts.size() > 0);
    if (sinkLayouts.layouts.size() == 1) {
      auto const layout =
        guardToLayout(env, ni, loc, type, emitVanilla, sinkLayouts);
      if (layout.vanilla()) {
        emitVanilla(env);
      } else {
        emitBespoke(env, ni, emitVanilla);
      }
    } else {
      guardToMultipleLayoutsAndEmit(env, ni, loc, type, emitVanilla, sinkLayouts);
    }
  }
}

}

///////////////////////////////////////////////////////////////////////////////

jit::vector<Location> guardsForBespoke(const IRGS& env, SrcKey sk) {
  if (!allowBespokeArrayLikes()) return {};
  return guardsForLayoutSensitiveCall(env, sk);
}

void translateDispatchBespoke(IRGS& env, const NormalizedInstruction& ni,
                              std::function<void(IRGS&)> emitVanilla) {
  if (!allowBespokeArrayLikes()) return emitVanilla(env);
  if (handleSource(env, ni.source, emitVanilla)) return;
  auto const loc = getLocationToGuard(env, ni.source);
  return loc ? handleSink(env, ni, emitVanilla, *loc) : emitVanilla(env);
}

///////////////////////////////////////////////////////////////////////////////

}

///////////////////////////////////////////////////////////////////////////////

void lowerStructBespokeGet(IRUnit& unit, IRInstruction* inst) {
  assertx(inst->is(BespokeGet));

  auto const arr = inst->src(0);
  auto const key = inst->src(1);
  auto const block = inst->block();

  assertx(arr->type().arrSpec().is_struct());
  assertx(key->isA(TStr));

  /*
   * Before:
   *
   * B1:
   *   Foo
   *   t3:Uninit|InitCell = BespokeGet t2:Dict, t1:Str
   *   Bar
   *
   * After:
   *
   * B1:
   *   Foo
   *   t4:Int = StructDictSlot t2:Dict, t1:Str -> B2, B3
   *
   * B2:
   *   t5:Lval = StructDictElemAddr t2:Dict, t1:Str, t4:Int
   *   t6:Cell = LdMem t5:Lval
   *   Jmp B4 t6
   *
   * B3:
   *   Jmp B4 Uninit
   *
   * B4:
   *   t7:Uninit|InitCell = DefLabel
   *   Bar
   *
   * This is the same logic as irgen, but we produce a diamond where
   * irgen might not.
   */
  auto const present = unit.defBlock(block->profCount(), block->hint());
  auto const notPresent = unit.defBlock(block->profCount(), block->hint());
  auto const join = unit.defBlock(block->profCount(), block->hint());

  auto const slot = unit.gen(
    StructDictSlot,
    inst->bcctx(),
    notPresent,
    arr,
    key
  );
  slot->setNext(present);

  auto elemType = arrLikeElemType(arr->type(), key->type(), inst->ctx());
  auto const& layout = arr->type().arrSpec().layout();
  if (!elemType.second && !layout.slotAlwaysPresent(slot->dst()->type())) {
    elemType.first |= TUninit;
  }

  auto const elemAddr = unit.gen(
    StructDictElemAddr,
    inst->bcctx(),
    arr,
    key,
    slot->dst(),
    arr
  );
  auto const load =
    unit.gen(LdMem, inst->bcctx(), elemType.first, elemAddr->dst());
  auto const jmp = unit.gen(Jmp, inst->bcctx(), join, load->dst());
  present->append(elemAddr);
  present->append(load);
  present->append(jmp);

  auto const val = inst->dst();
  auto const origType = val->type();
  if (origType.maybe(TUninit)) {
    notPresent->append(unit.gen(Jmp, inst->bcctx(), join, unit.cns(TUninit)));
  } else {
    notPresent->append(unit.gen(Unreachable, inst->bcctx(), ASSERT_REASON));
  }

  auto const defLabel = unit.defLabel(1, join, inst->bcctx());
  val->setInstruction(defLabel);
  defLabel->setDst(val, 0);
  val->setType(origType);

  auto iter = block->iteratorTo(inst);
  ++iter;
  join->splice(join->end(), block, iter, block->end());
  block->erase(inst);
  block->append(slot);
  inst->convertToNop();
}

void lowerStructBespokeGetThrow(IRUnit& unit, IRInstruction* inst) {
  assertx(inst->is(BespokeGetThrow));

  auto const arr = inst->src(0);
  auto const key = inst->src(1);
  auto const block = inst->block();
  auto const catchBlock = inst->taken();
  auto const next = inst->next();

  assertx(arr->type().arrSpec().is_struct());
  assertx(key->isA(TStr));
  assertx(catchBlock->isCatch());

  /*
   * Before:
   *
   * B1:
   *   Foo
   *   t3:InitCell = BespokeGetThrow t2:Dict, t1:Str -> B2, B3<Catch>
   *
   * After:
   *
   * B1:
   *   t4:Int = StructDictSlot t2:Dict, t1:Str -> B4, B5
   *
   * B4:
   *   t5:Lval = StructDictElemAddr t2:Dict, t1:Str, t4:Int
   *   t6:Cell = LdMem t5:Lval
   *   t7:InitCell = CheckType InitCell t6 -> B2, B5
   *
   * B5:
   *   ThrowOutOfBounds -> B3<Catch>
   *
   * This is the same logic as irgen, but we produce a diamond where
   * irgen might not.
   */

  auto const check = unit.defBlock(block->profCount(), block->hint());
  auto const notPresent =
    unit.defBlock(block->profCount(), Block::Hint::Unlikely);

  auto const slot = unit.gen(
    StructDictSlot,
    inst->bcctx(),
    notPresent,
    arr,
    key
  );
  slot->setNext(check);

  auto elemType = arrLikeElemType(arr->type(), key->type(), inst->ctx());
  auto const& layout = arr->type().arrSpec().layout();
  if (!elemType.second && !layout.slotAlwaysPresent(slot->dst()->type())) {
    elemType.first |= TUninit;
  }

  auto const elemAddr = unit.gen(
    StructDictElemAddr,
    inst->bcctx(),
    arr,
    key,
    slot->dst(),
    arr
  );
  auto const load =
    unit.gen(LdMem, inst->bcctx(), elemType.first, elemAddr->dst());
  auto const checkType =
    unit.gen(CheckType, inst->bcctx(), TInitCell, notPresent, load->dst());
  checkType->setNext(next);

  auto const val = inst->dst();
  val->setInstruction(checkType);
  checkType->setDst(val);
  val->setType(elemType.first & TInitCell);

  check->append(elemAddr);
  check->append(load);
  check->append(checkType);

  notPresent->append(
    unit.gen(ThrowOutOfBounds, inst->bcctx(), catchBlock, arr, key)
  );

  block->erase(inst);
  block->append(slot);
}

void lowerTypeStructureBespokeGet(IRUnit& unit, IRInstruction* inst) {
  assertx(inst->is(BespokeGet));

  auto const arr = inst->src(0);
  auto const key = inst->src(1);
  auto const block = inst->block();

  assertx(arr->type().arrSpec().is_type_structure());
  assertx(key->isA(TStr));

  /*
   * Before:
   *
   * B1:
   *   Foo
   *   t3:InitCell = BespokeGet t2:Dict, t1:Str
   *   Bar
   *
   * After:
   *
   * B1:
   *   Foo
   *   t4:InitCell = LdTypeStructureVal t2:Dict, t1:Str -> B2, B3
   *
   * B2:
   *   Jmp B4 t4
   *
   * B3:
   *   Jmp B4 Uninit
   *
   * B4:
   *   t5:Uninit|InitCell = DefLabel
   *   Bar
   */
  auto const present = unit.defBlock(block->profCount(), block->hint());
  auto const notPresent = unit.defBlock(block->profCount(), block->hint());
  auto const join = unit.defBlock(block->profCount(), block->hint());

  auto const tsVal = unit.gen(
    LdTypeStructureVal,
    inst->bcctx(),
    notPresent,
    arr,
    key
  );
  tsVal->setNext(present);

  auto const jmp = unit.gen(Jmp, inst->bcctx(), join, tsVal->dst());
  present->append(jmp);

  auto const val = inst->dst();
  if (val->type().maybe(TUninit)) {
    notPresent->append(unit.gen(Jmp, inst->bcctx(), join, unit.cns(TUninit)));
  } else {
    notPresent->append(unit.gen(Unreachable, inst->bcctx(), ASSERT_REASON));
  }

  auto const defLabel = unit.defLabel(1, join, inst->bcctx());
  val->setInstruction(defLabel);
  defLabel->setDst(val, 0);
  retypeDests(tsVal, &unit);

  auto iter = block->iteratorTo(inst);
  ++iter;
  join->splice(join->end(), block, iter, block->end());
  block->erase(inst);
  block->append(tsVal);
  inst->convertToNop();
}


void lowerTypeStructureBespokeGetThrow(IRUnit& unit, IRInstruction* inst) {
  assertx(inst->is(BespokeGetThrow));

  auto const arr = inst->src(0);
  auto const key = inst->src(1);
  auto const block = inst->block();
  auto const catchBlock = inst->taken();
  auto const next = inst->next();

  assertx(arr->type().arrSpec().is_type_structure());
  assertx(key->isA(TStr));
  assertx(catchBlock->isCatch());

  /*
   * Before:
   *
   * B1:
   *   Foo
   *   t3:InitCell = BespokeGetThrow t2:Dict, t1:Str -> B2, B3<Catch>
   *
   * After:
   *
   * B1:
   *   t3:InitCell = LdTypeStructureVal t2:Dict, t1:Str -> B2, B4
   *
   * B4:
   *   ThrowOutOfBounds -> B3<Catch>
   */
  auto const notPresent =
    unit.defBlock(block->profCount(), Block::Hint::Unlikely);

  auto const tsVal = unit.gen(
    LdTypeStructureVal,
    inst->bcctx(),
    notPresent,
    arr,
    key
  );
  tsVal->setNext(next);

  auto const val = inst->dst();
  val->setInstruction(tsVal);
  tsVal->setDst(val);
  retypeDests(tsVal, &unit);

  notPresent->append(
    unit.gen(ThrowOutOfBounds, inst->bcctx(), catchBlock, arr, key)
  );

  block->erase(inst);
  block->append(tsVal);
}

///////////////////////////////////////////////////////////////////////////////

}
