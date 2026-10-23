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

#include "hphp/runtime/vm/jit/irgen-call.h"

#include "hphp/runtime/base/stats.h"
#include "hphp/runtime/ext/asio/ext_async-generator.h"
#include "hphp/runtime/ext/asio/ext_wait-handle.h"
#include "hphp/runtime/ext/generator/ext_generator.h"
#include "hphp/runtime/vm/method-lookup.h"
#include "hphp/runtime/vm/reified-generics.h"
#include "hphp/runtime/vm/runtime.h"

#include "hphp/runtime/vm/jit/call-target-profile.h"
#include "hphp/runtime/vm/jit/guard-constraint.h"
#include "hphp/runtime/vm/jit/meth-profile.h"
#include "hphp/runtime/vm/jit/target-profile.h"
#include "hphp/runtime/vm/jit/translate-region.h"
#include "hphp/runtime/vm/jit/type.h"
#include "hphp/runtime/vm/jit/type-array-elem.h"

#include "hphp/runtime/vm/jit/irgen-basic.h"
#include "hphp/runtime/vm/jit/irgen-builtin.h"
#include "hphp/runtime/vm/jit/irgen-control.h"
#include "hphp/runtime/vm/jit/irgen-create.h"
#include "hphp/runtime/vm/jit/irgen-exit.h"
#include "hphp/runtime/vm/jit/irgen-func-prologue.h"
#include "hphp/runtime/vm/jit/irgen-inlining.h"
#include "hphp/runtime/vm/jit/irgen-internal.h"
#include "hphp/runtime/vm/jit/irgen-interpone.h"

#include "hphp/util/configs/debugger.h"
#include "hphp/util/configs/eval.h"
#include "hphp/util/configs/jit.h"
#include "hphp/util/configs/sandbox.h"

namespace HPHP::jit::irgen {

//////////////////////////////////////////////////////////////////////

namespace {

const StaticString
  s_DynamicContextOverrideUnsafe("__SystemLib\\DynamicContextOverrideUnsafe");
const StaticString s_attr_Deprecated("__Deprecated");


const Class* callContext(IRGS& env, const FCallArgs& fca, const Class* cls) {
  if (!fca.context) return curClass(env);
  if (fca.context->tsame(s_DynamicContextOverrideUnsafe.get())) {
    if (Cfg::Repo::Authoritative) PUNT(Bad-Dyn-Override);
    return cls;
  }
  return lookupKnownWithUnit(env, fca.context);
}

bool emitCallerInOutChecksKnown(IRGS& env, const Func* callee,
                                    const FCallArgs& fca) {
  if (!fca.enforceInOut()) return true;

  for (auto i = 0; i < fca.numArgs; ++i) {
    if (callee->isInOut(i) != fca.isInOut(i)) {
      auto const data = ParamData { i };
      gen(env, ThrowInOutMismatch, data, cns(env, callee));
      return false;
    }
  }
  return true;
}

void emitCallerInOutChecksUnknown(IRGS& env, SSATmp* callee,
                                      const FCallArgs& fca) {
  if (!fca.enforceInOut()) return;

  uint32_t inoutArgBits = 0;
  for (auto i = 0u; i < fca.numArgs; ++i) {
    if (!fca.isInOut(i)) continue;
    inoutArgBits |= 1u << std::min(i, Func::kInoutFastCheckBits);
  }

  if (static_cast<int32_t>(inoutArgBits) >= 0) {
    // All inout arguments are in the 0..31 range, so check for the exact match.
    auto const inoutParamBits = gen(env, LdFuncInOutBits, callee);
    ifThen(
      env,
      [&] (Block* taken) {
        auto const eq = gen(env, EqInt, inoutParamBits, cns(env, inoutArgBits));
        gen(env, JmpZero, taken, eq);
      },
      [&] {
        hint(env, Block::Hint::Unlikely);

        // Even if the bits are different we may not want an inout mismatch error
        // message. It could be that the function was just called with too few arguments
        // And if that is the case we want to show that error instead.
        //
        // But the reason we don't handle that is that it is handled later any way.
        // And it will be very rare that we get here so no reason to make it performant.
        //
        // It could also be that we have packed args and in that case when unpacking them
        // we will verify that they are not in positions of inout args.
        //
        // So we use CheckInOutMismatch that will check if the normal args matched.
        // Otherwise we just continue and later code will handle things
        auto const ioaData = BoolVecArgsData { fca.numArgs, fca.inoutArgs };
        gen(env, CheckInOutMismatch, ioaData, callee);
      }
    );
  } else {
    // Passing inout arg beyond the 0..31 range, call the C++ helper.
    auto const ioaData = BoolVecArgsData { fca.numArgs, fca.inoutArgs };
    gen(env, CheckInOutMismatch, ioaData, callee);
  }
}

bool emitCallerReadonlyChecksKnown(IRGS& env, const Func* callee,
                                   const FCallArgs& fca) {
  if (fca.enforceReadonly()) {
    auto const namedArgNames = fca.hasNamedArgs()
      ? curUnit(env)->lookupArrayId(fca.namedArgNames) : nullptr;
    if (auto const mismatch =
          readonlyMismatch(callee, fca.numArgs, fca.readonlyArgs, namedArgNames)) {
      auto const data = ParamData { static_cast<int32_t>(*mismatch) };
      gen(env, ThrowReadonlyMismatch, data, cns(env, callee));
      return false;
    }
  }
  if (fca.enforceMutableReturn() && (callee->attrs() & AttrReadonlyReturn)) {
    auto const data = ParamData { kReadonlyReturnId };
    gen(env, ThrowReadonlyMismatch, data, cns(env, callee));
    return false;
  }
  if (fca.enforceReadonlyThis() && !(callee->attrs() & AttrReadonlyThis)) {
    auto const data = ParamData { kReadonlyThisId };
    gen(env, ThrowReadonlyMismatch, data, cns(env, callee));
    return false;
  }
  return true;
}

void emitCallerReadonlyChecksUnknown(IRGS& env, SSATmp* callee,
                                      const FCallArgs& fca) {
  if (fca.enforceReadonly()) {
    auto const data = BoolVecArgsData { fca.numArgs, fca.readonlyArgs };
    auto const namedArgNames = fca.hasNamedArgs()
      ? curUnit(env)->lookupArrayId(fca.namedArgNames) : nullptr;
    auto const names = namedArgNames
      ? cns(env, namedArgNames) : cns(env, nullptr);
    gen(env, CheckReadonlyMismatch, data, callee, names);
  }
  if (fca.enforceMutableReturn()) {
    ifThen(
      env,
      [&] (Block* taken) {
        auto const data = AttrData { AttrReadonlyReturn };
        auto const success = gen(env, FuncHasAttr, data, callee);
        gen(env, JmpNZero, taken, success);
      },
      [&] {
        hint(env, Block::Hint::Unlikely);
        auto const data = ParamData { kReadonlyReturnId };
        gen(env, ThrowReadonlyMismatch, data, callee);
      }
    );
  }
  if (fca.enforceReadonlyThis()) {
    ifThen(
      env,
      [&] (Block* taken) {
        auto const data = AttrData { AttrReadonlyThis };
        auto const success = gen(env, FuncHasAttr, data, callee);
        gen(env, JmpZero, taken, success);
      },
      [&] {
        hint(env, Block::Hint::Unlikely);
        auto const data = ParamData { kReadonlyThisId };
        gen(env, ThrowReadonlyMismatch, data, callee);
      }
    );
  }
}

void emitCallerDynamicCallChecksKnown(IRGS& env, const Func* callee) {
  assertx(callee);
  auto const dynCallable = callee->isDynamicallyCallable();
  if (dynCallable && !Cfg::Eval::ForbidDynamicCallsWithAttr) {
    return;
  }
  auto const level = callee->isMethod()
    ? (callee->isStatic()
        ? Cfg::Eval::ForbidDynamicCallsToClsMeth
        : Cfg::Eval::ForbidDynamicCallsToInstMeth)
    : Cfg::Eval::ForbidDynamicCallsToFunc;
  if (level <= 0) return;
  if (dynCallable && level < 2) return;
  gen(env, RaiseForbiddenDynCall, cns(env, callee));
}

void emitCallerDynamicCallChecksUnknown(IRGS& env, SSATmp* callee) {
  assertx(!callee->hasConstVal());
  if (Cfg::Eval::ForbidDynamicCallsWithAttr) {
    gen(env, RaiseForbiddenDynCall, callee);
  } else {
    ifElse(
      env,
      [&] (Block* skip) {
        auto const data = AttrData { AttrDynamicallyCallable };
        auto const dynCallable = gen(env, FuncHasAttr, data, callee);
        gen(env, JmpNZero, skip, dynCallable);
      },
      [&] {
        hint(env, Block::Hint::Unlikely);
        gen(env, RaiseForbiddenDynCall, callee);
      }
    );
  }
}

SSATmp* callImpl(IRGS& env, SSATmp* callee, const FCallArgs& fca,
                 SSATmp* objOrClass, bool skipRepack, bool dynamicCall,
                 bool asyncEagerReturn) {
  // TODO: extend hhbc with bitmap of passed generics, or even better, use one
  // stack value per generic argument and extend hhbc with their count
  auto const genericsBitmap = [&] {
    if (!fca.hasGenerics()) return uint16_t{0};
    auto const generics = topC(env);
    auto const caller = curFunc(env);
    // If caller forwards identical generics to the callee, we can construct
    // the bitmap from caller's information
    if (caller->hasReifiedGenerics() &&
        caller->getGenericsInfo().allGenericsFullyReified() &&
        generics->inst()->is(LdLoc) &&
        generics->inst()->extra<LdLoc>()->locId ==
          caller->reifiedGenericsLocalId()) {
      return getGenericsBitmap(caller);
    }
    // Do not bother calculating the bitmap using a C++ helper if generics are
    // not statically known, as the prologue already has the same logic.
    if (!generics->hasConstVal(TVec)) return uint16_t{0};
    auto const genericsArr = generics->arrLikeVal();
    return getGenericsBitmap(genericsArr);
  }();

  if (objOrClass == nullptr) objOrClass = cns(env, TNullptr);
  assertx(objOrClass->isA(TNullptr) || objOrClass->isA(TObj|TCls));

  auto const pubFP = env.irb->fs()[0].fp();
  auto pubBcOff = bcOff(env);
  if (env.irb->fs().inlineDepth()) {
    auto const firstUnpubFP = env.irb->fs()[1].fp();
    pubBcOff = firstUnpubFP->inst()->marker().sk().offset();
  }

  const ArrayData* namedArgNames = nullptr;
  uint32_t namedArgNamesSize = 0;
  if (fca.hasNamedArgs()) {
    namedArgNames = curUnit(env)->lookupArrayId(fca.namedArgNames);
    namedArgNamesSize = namedArgNames->size();
  }

  auto const data = CallData {
    spOffBCFromIRSP(env),
    fca.numArgs - namedArgNamesSize,
    fca.numRets - 1,
    pubBcOff,
    namedArgNames,
    genericsBitmap,
    fca.hasGenerics(),
    fca.hasUnpack(),
    skipRepack,
    dynamicCall,
    asyncEagerReturn,
    env.formingRegion
  };
  return gen(
    env,
    Call,
    data,
    sp(env),
    pubFP,
    callee,
    objOrClass,
    curCoeffects(env)
  );
}

SSATmp* callFuncEntry(IRGS& env, const Func* calleePrototype,
                      SSATmp* callee, SSATmp* objOrClass,
                      uint32_t posArgcInclUnpack, bool asyncEagerReturn,
                      bool addedUninitNamedParams) {
  if (objOrClass == nullptr) objOrClass = cns(env, TNullptr);
  assertx(objOrClass->isA(TNullptr) || objOrClass->isA(TObj|TCls));

  auto const pubFP = env.irb->fs()[0].fp();
  auto pubBcOff = bcOff(env);
  if (env.irb->fs().inlineDepth()) {
    auto const firstUnpubFP = env.irb->fs()[1].fp();
    pubBcOff = firstUnpubFP->inst()->marker().sk().offset();
  }

  auto const arFlags = ActRec::encodeCallOffsetAndFlags(
    pubBcOff,
    asyncEagerReturn ? (1 << ActRec::AsyncEagerRet) : 0
  );

  auto const data = CallFuncEntryData {
    calleePrototype,
    spOffBCFromIRSP(env),
    posArgcInclUnpack,
    arFlags,
    env.formingRegion,
    addedUninitNamedParams,
  };
  return gen(env, CallFuncEntry, data, sp(env), pubFP, callee, objOrClass);
}

void handleCallReturn(IRGS& env, const Func* callee, SSATmp* retVal,
                      Offset asyncEagerOffset, bool unlikely) {
  // Insert a debugger interrupt check after returning from a call
  // if the translation is not exited before the next source key.
  auto const insertIntrCheck = [&](SrcKey nextSk) {
    if (env.formingRegion) return;
    assertx(env.region);
    // If the call is the last bytecode of the region, there is no need to
    // insert the debugger interrupt check as it will be checked at the
    // beginning of next translation.
    if (Cfg::Debugger::EnableVSDebugger && Cfg::Eval::EmitDebuggerIntrCheck &&
        curSrcKey(env) != env.region->lastSrcKey()) {
      irgen::checkDebuggerIntr(env, nextSk);
    }
  };

  if (asyncEagerOffset == kInvalidOffset) {
    push(env, retVal);
    if (unlikely) {
      gen(env, Jmp, makeExit(env, nextSrcKey(env)));
    } else {
      insertIntrCheck(nextSrcKey(env));
    }
    return;
  }

  // Prevent using the callee information for asserting the return type if we're
  // forming a region, to avoid using information that may be dropped once types
  // are relaxed (see callReturn() in ir-instruction.cpp).
  if (env.formingRegion) {
    callee = nullptr;
  }

  auto const handleEagerReturn = [&] {
    auto const ty = callee
      ? awaitedCallReturnType(callee, true /* mayIntercept */)
      : TInitCell;
    push(env, gen(env, AssertType, ty, retVal));
    auto const absAEOffset = bcOff(env) + asyncEagerOffset;
    if (unlikely) {
      gen(env, Jmp, makeExit(env, SrcKey{curSrcKey(env), absAEOffset}));
    } else {
      insertIntrCheck(SrcKey{curSrcKey(env), absAEOffset});
      jmpImpl(env, absAEOffset);
    }
  };

  if (callee &&
      callee->isEagerOnly() &&
      !callee->isInterceptable() &&
      !callee->isMemoizeImpl()) {
    handleEagerReturn();
    return;
  }

  ifThenElse(
    env,
    [&] (Block* taken) {
      auto const aux = gen(env, LdTVAux, LdTVAuxData {}, retVal);
      auto const tst = gen(env, AndInt, aux, cns(env, 1u << 31));
      gen(env, JmpZero, taken, tst);
    },
    handleEagerReturn,
    [&] {
      hint(env, Block::Hint::Unlikely);
      auto const ty = callee
        ? callReturnType(callee, true /* mayIntercept */)
        : TInitCell;
      push(env, gen(env, AssertType, ty, retVal));
      if (unlikely) {
        gen(env, Jmp, makeExit(env, nextSrcKey(env)));
      } else {
        insertIntrCheck(nextSrcKey(env));
      }
    }
  );
}

//////////////////////////////////////////////////////////////////////

template<class TKnown, class TUnknown>
void speculateTargetFunction(IRGS& env, SSATmp* callee,
                             TKnown callKnown, TUnknown callUnknown,
                             const jit::vector<CallTargetProfile::Choice> &choices,
                             size_t attempts, const size_t maxAttempts) {
  auto remainingProb = 1.0;
  for (auto i = 0; i < attempts; i++) {
    remainingProb -= choices[i].probability;
  }

  auto indirectCall = [&] {
    auto const unlikely = remainingProb <= Cfg::Jit::PGOCalledFuncExitThreshold;
    if (unlikely) {
      hint(env, Block::Hint::Unlikely);
      IRUnit::Hinter h(env.irb->unit(), Block::Hint::Unlikely);
      callUnknown(true);
    } else {
      callUnknown(false);
    }
  };

  if (attempts >= choices.size() || attempts == maxAttempts) {
    return indirectCall();
  }
  const Func* profiledFunc = choices[attempts].func;
  double probability = choices[attempts].probability / remainingProb;
  // Don't emit the check if the probability of it succeeding is below the
  // threshold.
  if (probability < Cfg::Jit::PGOCalledFuncCheckThreshold) {
    return indirectCall();
  }

  ifThenElse(
    env,
    [&] (Block* taken) {
      auto const equal = gen(env, EqFunc, callee, cns(env, profiledFunc));
      gen(env, JmpZero, taken, equal);
    },
    [&] {
      if (profiledFunc->hasOptionalNamedParameters()) {
        indirectCall();
      } else {
        callKnown(profiledFunc);
      }
    },
    [&] {
      speculateTargetFunction(env, callee, callKnown, callUnknown, choices,
                              attempts + 1, maxAttempts);
    }
  );
}
/*
 * In PGO mode, we use profiling to try to determine the most likely target
 * function at each call site. In profiling translations, this function profiles
 * callees. If the most likely called function based on profiling was seen
 * at least Eval.JitPGOCalledFuncCheckThreshold percent of the times during
 * profiling, optimized translations emit a runtime check whether the callee
 * is the given profiled function and if so, emit a code to invoke it directly.
 */
template<class TKnown, class TUnknown>
void callProfiledFunc(IRGS& env, SSATmp* callee, SSATmp* objOrClass,
                      TKnown callKnown, TUnknown callUnknown) {
  if (!Cfg::Repo::Authoritative) return callUnknown(false);

  auto profile = TargetProfile<CallTargetProfile>(
    env.context, env.irb->curMarker(), callTargetProfileKey());

  if (profile.profiling()) {
    gen(env, ProfileCall, ProfileCallTargetData { profile.handle() }, callee);
  }

  if (!profile.optimizing()) return callUnknown(false);

  auto const data = profile.data();
  // Dump annotations if requested.
  if (Cfg::Eval::DumpCallTargets) {
    auto const fnName = curFunc(env)->fullName()->data();
    env.unit.annotationData->add(
      "CallTargets",
      fmt::format("BC={} FN={}: {}\n", bcOff(env), fnName, data.toString())
    );
  }
  auto choices = [&]() {
    jit::vector<CallTargetProfile::Choice> result;
    const Class* targetClass = objOrClass ? objOrClass->type().clsSpec().cls() : nullptr;
    for (auto const& choice: data.choose()) {
      auto const choiceClass = choice.func->implCls();
      bool includeChoice = !targetClass || !choiceClass || choiceClass->classof(targetClass) ||
                           targetClass->lookupMethod(choice.func->name()) == choice.func;
      if (includeChoice) {
        result.push_back(choice);
      }
    }
    return result;
  }();

  speculateTargetFunction(env, callee, callKnown, callUnknown, choices,
                          0, Cfg::Jit::PGOCalledFuncCheckNumSpeculations);
}

//////////////////////////////////////////////////////////////////////

bool hasConstParamMemoCache(IRGS& env, const Func* callee, SSATmp* objOrClass,
                            const std::vector<Type>& inputTypes) {
  // The RDS key is just the callee and its arguments, so a cache keyed by the
  // implicit context can't use it. A hit skips the call, which would also skip
  // an intercept.
  if (!callee->isMemoizeWrapper() ||
      callee->isKeyedByImplicitContextMemoize() ||
      callee->isInterceptable()) {
    return false;
  }
  if (callee->userAttributes().contains(PackedStringPtr(s_attr_Deprecated.get()))) {
    return false;
  }
  // Classes are converted to strings before storing to memo caches.
  // Bail out if we need to raise warning for class to string conversation.
  if (Cfg::Eval::ClassMemoNotices) return false;
  if (objOrClass &&
      (!objOrClass->hasConstVal(TCls) ||
       !objOrClass->clsVal()->isPersistent())) {
    return false;
  }
  if (callee->numParams() == 0) return false;
  for (auto const& t : inputTypes) {
    if (!t.admitsSingleVal()) return false;
    if (t.hasConstVal(TCls) && !t.clsVal()->isPersistent()) return false;
    if (t.hasConstVal(TFunc) && !t.funcVal()->isPersistent()) return false;
    if (t.hasConstVal(TClsMeth) && !t.clsmethVal()->isPersistent()) {
      return false;
    }
  }
  return true;
}

rds::Link<TypedValue, rds::Mode::Normal>
constParamCacheLink(IRGS& env, const Func* callee, SSATmp* cls,
                    bool asyncEagerReturn,
                    const std::vector<Type>& inputTypes) {
  // Match the memo cache this shadows, and so the key clear_static_memoization
  // and clear_lsb_memoization clear: only LSB caches are per class.
  auto const clsVal =
    cls && callee->isMemoizeWrapperLSB() ? cls->clsVal() : nullptr;
  auto arr = Array::CreateVec();
  for (auto const& t : inputTypes) {
    auto const tvOpt = t.tv();
    assertx(tvOpt);
    arr.append(*tvOpt);
  }
  auto const paramVals = ArrayData::GetScalarArray(arr);
  return rds::bindConstMemoCache(callee, clsVal, paramVals, asyncEagerReturn);
}

SSATmp* refineKnownFuncCtx(IRGS& env, const Func* callee, SSATmp* ctx) {
  if (callee->isClosureBody()) {
    return gen(env, AssertType, Type::ExactObj(callee->implCls()), ctx);
  }

  if (!callee->cls()) {
    assertx(ctx == nullptr);
    return ctx;
  }
  assertx(!ctx->type().maybe(TNullptr));

  if (ctx->isA(TBottom)) return ctx;

  if (callee->isStatic()) {
    if (!ctx->type().maybe(TCls)) return cns(env, TBottom);
    if (ctx->hasConstVal(TCls)) return ctx;

    auto const ty = ctx->type() & Type::SubCls(callee->cls());
    if (ctx->type() <= ty) return ctx;
    return gen(env, AssertType, ty, ctx);
  }

  if (!ctx->type().maybe(TObj)) return cns(env, TBottom);

  auto const ty = ctx->type() & thisTypeFromFunc(callee);
  if (ctx->type() <= ty) return ctx;
  return gen(env, AssertType, ty, ctx);
}

std::vector<Type> funcEntryTypes(IRGS& env, const Func* callee,
                                 uint32_t numPositionalArgs) {
  int argc = 0;
  std::vector<Type> inputTypes;
  auto firstInputPos = numPositionalArgs + callee->numNamedParams() - 1
    + (callee->hasReifiedGenerics() ? 1 : 0)
    + (callee->hasCoeffectsLocal() ? 1 : 0);

  auto const nextInput = [&] {
    // DataTypeGeneric is used because we're just passing the locals into the
    // callee.  It's up to the callee to constrain further if needed.
    auto const offset = BCSPRelOffset{
      safe_cast<int32_t>(firstInputPos - argc++)
    };
    auto const type = publicTopType(env, offset);
    assertx(type <= TCell);

    inputTypes.push_back(type);
  };

  // Set passed parameters
  while (argc < numPositionalArgs + callee->numNamedParams()) nextInput();

  // Uninit default values
  for (int i = argc; i < callee->numNonVariadicParams(); ++i) {
    inputTypes.push_back(TUninit);
  }

  // Variadic parameter
  if (callee->hasVariadicCaptureParam() && argc < callee->numParams()) {
    inputTypes.emplace_back(Type::cns(staticEmptyVec()));
  }

  // Generics and coeffects
  if (callee->hasReifiedGenerics()) nextInput();
  if (callee->hasCoeffectsLocal())  nextInput();

  assertx(inputTypes.size() == callee->numFuncEntryInputs());
  return inputTypes;
}

void prepareAndCallKnown(IRGS& env, const Func* callee, const FCallArgs& fca,
                         SSATmp* objOrClass, bool dynamicCall,
                         bool suppressDynCallCheck) {
  assertx(callee);
  updateStackOffsetAndExceptionBoundary(env);

  objOrClass = refineKnownFuncCtx(env, callee, objOrClass);
  if (objOrClass != nullptr && objOrClass->isA(TBottom)) {
    gen(env, Unreachable, ASSERT_REASON);
    return;
  }

  // Caller checks
  if (!emitCallerInOutChecksKnown(env, callee, fca)) return;
  if (!emitCallerReadonlyChecksKnown(env, callee, fca)) return;
  if (dynamicCall && !suppressDynCallCheck) {
    emitCallerDynamicCallChecksKnown(env, callee);
  }

  auto const namedArgNamesVal =
    fca.namedArgNames != kInvalidId
    ? curUnit(env)->lookupArrayId(fca.namedArgNames)
    : nullptr;
  auto const namedArgc =
    static_cast<uint32_t>(namedArgNamesVal ? namedArgNamesVal->size() : 0);

  auto const doCall = [&](const FCallArgs& fca, bool skipRepack) {
    auto const asyncEagerOffset = callee->supportsAsyncEagerReturn()
      ? fca.asyncEagerOffset : kInvalidOffset;
    auto const asyncEagerReturn = asyncEagerOffset != kInvalidOffset;

    if (!skipRepack) {
      // Use the generic prologue method dispatch that can handle arg repacking.
      auto const retVal = callImpl(env, cns(env, callee), fca, objOrClass,
                                   skipRepack, dynamicCall, asyncEagerReturn);
      handleCallReturn(env, callee, retVal, asyncEagerOffset,
                       false /* unlikely */);
      return;
    }

    assertx(
      (!fca.hasUnpack() && fca.numArgs - namedArgc <= callee->numPositionalParams()) ||
      (fca.hasUnpack() && fca.numArgs - namedArgc == callee->numPositionalParams()));

    updateStackOffsetAndExceptionBoundary(env);

    auto const coeffects = curCoeffects(env);
    auto const prologueFlags = cns(env, PrologueFlags(
      fca.hasGenerics(),
      dynamicCall,
      fca.namedArgNames != kInvalidId,
      false,  // async eager return unused by prologue checks
      0,  // call offset unused by prologue checks
      0,  // generics bitmap not needed, generics SSA read from the stack
      RuntimeCoeffects::none()  // coeffects not needed, passed via SSA arg
    ).value());


    // We defer emitting parameter type checks until after allocating the frame
    // pointer (but before we pop the parameters from the stack and make the
    // frame live) so that CheckTypes from within the body of the function can
    // be hoisted through the parameter checks.
    auto const calleeFP = genCalleeFP(env, callee, fca.numInputs());

    SSATmp* namedArgNames =
      namedArgNamesVal ? cns(env, namedArgNamesVal) : cns(env, nullptr);
    auto posArgc = fca.numArgs + (fca.hasUnpack() ? 1U : 0U) - namedArgc;
    // Callee checks and input initialization.
    emitCalleeGenericsChecks(env, callee, prologueFlags, namedArgNames,
                             fca.hasGenerics(),
                             /* namedArgsAccountedInStack */ true);
    emitCalleeNamedArgChecks(env, callee, posArgc, prologueFlags,
                             namedArgNames);
    emitCalleeArgumentArityChecks(env, callee, posArgc);
    if (env.irb->inUnreachableState()) return;
    // We need to push uninits before the argument type checks when calling
    // func entries for type checking.
    bool addedUninitNamed =
      emitInitFuncNamedParams(env, callee, posArgc, namedArgNamesVal);

    emitCalleeArgumentTypeChecks(
      env, callee, posArgc,
      objOrClass ? objOrClass : cns(env, nullptr)
    );
    emitCalleeDynamicCallChecks(env, callee, prologueFlags);
    // Coeffect rules index parameters by their full position (named params
    // precede positionals), so include the named params in the arg count, as
    // the prologue path and interpreter do.
    emitCalleeCoeffectChecks(env, callee, prologueFlags, coeffects,
                             fca.skipCoeffectsCheck(),
                             posArgc + callee->numNamedParams(),
                             objOrClass ? objOrClass : cns(env, nullptr));
    emitCalleeRecordFuncCoverage(env, callee);
    // Some of the checks above may have failed and it may be illegal to emit
    // the code below with incorrect inputs (such as not enough args).
    if (env.irb->inUnreachableState()) return;

    auto const inputTypes = funcEntryTypes(env, callee, posArgc);

    auto const hasRdsCache =
      hasConstParamMemoCache(env, callee, objOrClass, inputTypes);

    auto const numPosArgs =
      std::min(posArgc, callee->numPositionalParams());
    auto const entry =
      SrcKey { callee, numPosArgs, addedUninitNamed, SrcKey::FuncEntryTag {} };

    if (!hasRdsCache) {
      if (irGenTryInlineFCall(env, entry, objOrClass, asyncEagerOffset,
                              calleeFP, posArgc, addedUninitNamed,
                              inputTypes)) {
        return;
      }
    }

    // We didn't end up inlining the callee, discard the frame pointer
    if (!calleeFP->isA(TBottom)) calleeFP->inst()->convertToNop();
    emitInitFuncInputs(env, callee, posArgc);

    if (hasRdsCache) {
      auto const link = constParamCacheLink(env, callee, objOrClass,
                                            asyncEagerReturn, inputTypes);
      assertx(link.isNormal());
      auto const data = TVInRDSHandleData { link.handle(), asyncEagerReturn };
      auto const retType = asyncEagerReturn
        ? TInitCell
        : callReturnType(callee, true /* mayIntercept */);
      auto const res = cond(
        env,
        [&] (Block* taken) {
          gen(env, CheckRDSInitialized, taken, RDSHandleData { data });
        },
        [&] {
          for (auto i = 0; i < callee->numFuncEntryInputs(); ++i) {
            popDecRef(env, static_cast<DecRefProfileId>(i));
          }
          for (auto i = 0; i < kNumActRecCells; ++i) popU(env);
          auto const retVal = gen(env, LdTVFromRDS, data, retType);
          gen(env, IncRef, retVal);
          return retVal;
        },
        [&] {
          hint(env, Block::Hint::Unlikely);
          auto const retVal = callFuncEntry(env, callee, cns(env, callee),
                                            objOrClass, posArgc,
                                            asyncEagerReturn, addedUninitNamed);
          gen(env, StTVInRDS, data, retVal);
          gen(env, IncRef, retVal);
          gen(env, MarkRDSInitialized, RDSHandleData { data });
          return retVal;
        }
      );
      handleCallReturn(env, callee, res, asyncEagerOffset,
                       false /* unlikely */);
    } else {
      auto const retVal = callFuncEntry(env, callee, cns(env, callee),
                                        objOrClass, posArgc,
                                        asyncEagerReturn, addedUninitNamed);
      handleCallReturn(env, callee, retVal, asyncEagerOffset,
                       false /* unlikely */);
    }
  };

  const int32_t numToPack =
    static_cast<int32_t>(fca.numArgs) - namedArgc - callee->numPositionalParams();
  if (fca.hasUnpack()) {
    updateStackOffsetAndExceptionBoundary(env);

    if (numToPack == 0) {
      if (fca.skipRepack()) return doCall(fca, true /* skipRepack */);

      auto const unpackOff = BCSPRelOffset{fca.hasGenerics() ? 1 : 0};
      auto const unpack = topC(env, unpackOff);
      auto const doConvertAndCallImpl = [&](SSATmp* converted) {
        auto const offset = offsetFromIRSP(env, unpackOff);
        gen(env, StStk, IRSPRelOffsetData{offset}, sp(env), converted);
        doCall(fca, true /* skipRepack */);
      };
      auto const doConvertAndCall = [&](Opcode op) {
        doConvertAndCallImpl(gen(env, op, unpack));
      };
      if (unpack->isA(TVec)) return doCall(fca, true /* skipRepack */);
      if (unpack->isA(TArrLike)) return doConvertAndCall(ConvArrLikeToVec);
    }

    // Slow path. Uses funcPrologueRedispatchUnpack to repack the arguments.
    assertx(!fca.skipRepack());
    return doCall(fca, false /* skipRepack */);
  }

  if (numToPack <= 0) {
    return doCall(fca, true /* skipRepack */);
  }

  // Pack extra arguments. The prologue can handle this even if the function
  // does not accept variadic arguments.
  auto const generics = fca.hasGenerics() ? popC(env) : nullptr;
  for (auto i = 0; i < numToPack; ++i) {
    assertTypeStack(env, BCSPRelOffset{i}, TInitCell);
  }
  emitNewVec(env, numToPack);

  if (generics) push(env, generics);

  doCall(FCallArgs(
    static_cast<FCallArgsFlags>(
      fca.flags | FCallArgsFlags::HasUnpack | FCallArgsFlags::SkipRepack),
    callee->numPositionalParams() + namedArgc,
    fca.numRets,
    nullptr,  // inout-ness already checked
    nullptr,  // readonly-ness already checked
    fca.namedArgNames,
    fca.asyncEagerOffset,
    fca.context
  ), true /* skipRepack */);
}

void prepareAndCallUnknown(IRGS& env, SSATmp* callee, const FCallArgs& fca,
                           SSATmp* objOrClass, bool dynamicCall,
                           bool suppressDynCallCheck, bool unlikely) {
  assertx(callee->isA(TFunc));
  if (callee->hasConstVal()) {
    prepareAndCallKnown(env, callee->funcVal(), fca, objOrClass,
                        dynamicCall, suppressDynCallCheck);
    return;
  }

  updateStackOffsetAndExceptionBoundary(env);

  // Caller checks
  emitCallerInOutChecksUnknown(env, callee, fca);
  emitCallerReadonlyChecksUnknown(env, callee, fca);
  if (dynamicCall && !suppressDynCallCheck) {
    emitCallerDynamicCallChecksUnknown(env, callee);
  }

  // Okay to request async eager return even if it is not supported.
  auto const retVal = callImpl(env, callee, fca, objOrClass, fca.skipRepack(),
                               dynamicCall,
                               fca.asyncEagerOffset != kInvalidOffset);
  handleCallReturn(env, nullptr, retVal, fca.asyncEagerOffset, unlikely);
}

void prepareAndCallProfiled(IRGS& env, SSATmp* callee, const FCallArgs& fca,
                            SSATmp* objOrClass, bool dynamicCall,
                            bool suppressDynCallCheck) {
  assertx(callee->isA(TFunc));
  auto const handleKnown = [&] (const Func* knownCallee) {
    prepareAndCallKnown(env, knownCallee, fca, objOrClass,
                        dynamicCall, suppressDynCallCheck);
  };
  if (callee->hasConstVal()) return handleKnown(callee->funcVal());

  auto const handleUnknown = [&] (bool unlikely) {
    prepareAndCallUnknown(env, callee, fca, objOrClass,
                          dynamicCall, suppressDynCallCheck,
                          unlikely);
  };
  callProfiledFunc(env, callee, objOrClass, handleKnown, handleUnknown);
}

//////////////////////////////////////////////////////////////////////

// Calling object method when we don't know the Func* statically.
template<class TProfile>
void fcallObjMethodUnknown(
  IRGS& env,
  const FCallArgs& fca,
  SSATmp* obj,
  SSATmp* methodName,
  bool dynamicCall,
  uint32_t numExtraInputs,
  TProfile profileMethod,
  bool noCallProfiling
) {
  implIncStat(env, Stats::ObjMethod_cached);

  updateStackOffsetAndExceptionBoundary(env);

  auto const callerCtx = [&] {
    if (!fca.context) return curClass(env);
    auto const ret = lookupKnownWithUnit(env, fca.context);
    if (!ret) PUNT(no-context);
    return ret;
  }();

  auto const func = [&] {
    auto const cls = gen(env, LdObjClass, obj);
    // Note that we don't use this inline-caching mechanism (which caches the
    // first class seen) for optimized translations, because they use more
    // advanced, profile-guided method-lookup techniques.  If we get here within
    // an optimized translation, it's because those techniques didn't apply, and
    // therefore the inline caching mechanism in here is very likely to hurt
    // more than help.
    if (!methodName->hasConstVal() || env.context.kind == TransKind::Optimize) {
      auto const focData = OptClassAndFuncData { callerCtx, curFunc(env) };
      return gen(env, LdObjMethodD, focData, cls, methodName);
    }

    auto const tcCache = gen(env, LdSmashable);
    return cond(
      env,
      [&] (Block* taken) {
        // check for TC cache hit; go to the slow check on miss
        gen(env, CheckSmashableClass, taken, tcCache, cls);
      },
      [&] {
        // fast path: load func from TC cache and proceed to regular call
        return gen(env, LdSmashableFunc, tcCache);
      },
      [&] {
        // slow path: run C++ helper to determine Func*, exit if we can't handle
        // the call in the JIT
        auto const fnData =
          FuncNameCtxData { methodName->strVal(), callerCtx, curFunc(env) };
        return gen(env, LdObjMethodS, fnData, cls, tcCache);
      }
    );
  }();

  profileMethod(func);
  discard(env, numExtraInputs);
  if (noCallProfiling) {
    prepareAndCallUnknown(env, func, fca, obj, dynamicCall, false, true);
  } else {
    prepareAndCallProfiled(env, func, fca, obj, dynamicCall, false);
  }
}

SSATmp* lookupObjMethodInterfaceFunc(IRGS& env, SSATmp* obj,
                                     const Func* ifaceFunc) {
  implIncStat(env, Stats::ObjMethod_ifaceslot);
  auto const cls = gen(env, LdObjClass, obj);
  auto const vtableSlot = ifaceFunc->cls()->preClass()->ifaceVtableSlot();
  auto const imData = IfaceMethodData { vtableSlot, ifaceFunc->methodSlot() };
  auto const func = gen(env, LdIfaceMethod, imData, cls);
  if (ifaceFunc->attrs() & AttrStatic) {
    gen(env, ThrowHasThisNeedStatic, func);
  }
  return func;
}

SSATmp* lookupObjMethodNonExactFunc(IRGS& env, SSATmp* obj,
                                    const Func* superFunc) {
  implIncStat(env, Stats::ObjMethod_methodslot);
  auto const cls = gen(env, LdObjClass, obj);
  auto const slot = cns(env, superFunc->methodSlot());
  auto const func = gen(env, LdClsMethod, cls, slot);
  if (superFunc->isStaticInPrologue()) {
    gen(env, ThrowHasThisNeedStatic, func);
  }
  return func;
}

const StaticString methProfileKey{ "MethProfile-FCallObjMethod" };

inline SSATmp* ldCtxForClsMethod(IRGS& env,
                                 const Func* callee,
                                 SSATmp* callCtx,
                                 const Class* cls,
                                 bool exact) {

  assertx(callCtx->isA(TCls));
  assertx(cls);

  if (callee->isStaticInPrologue()) {
    return gen(env, AssertType, Type::SubCls(cls), callCtx);
  };

  auto gen_missing_this = [&] {
    gen(env, ThrowMissingThis, cns(env, callee));
    return cns(env, TBottom);
  };

  if (!hasThis(env)) {
    return gen_missing_this();
  }

  auto const maybeUseThis = curClass(env)->classof(cls);
  if (!maybeUseThis && !cls->classof(curClass(env))) {
    return gen_missing_this();
  }

  auto skipAT = [] (SSATmp* val) {
    while (val->inst()->is(AssertType, CheckType)) {
      val = val->inst()->src(0);
    }
    return val;
  };

  auto const canUseThis = [&] () -> bool {
    // A static::foo() call can always pass through a $this
    // from the caller (if it has one). Match the common patterns
    auto cc = skipAT(callCtx);
    if (cc->inst()->is(LdFrameCls)) return true;
    if (cc->inst()->is(LdObjClass)) {
      cc = skipAT(cc->inst()->src(0));
      if (cc->inst()->is(LdFrameThis)) return true;
    }
    return maybeUseThis && (exact || cls->attrs() & AttrNoOverride);
  }();

  auto thiz = ldThis(env);

  if (canUseThis) {
    gen(env, IncRef, thiz);
    return thiz;
  }

  return cond(
    env,
    [&] (Block* taken) {
      auto thizCls = gen(env, LdObjClass, thiz);
      auto flag = exact ?
        gen(env, ExtendsClass, ExtendsClassData{ cls, true }, thizCls) :
        gen(env, InstanceOf, InstanceOfData { true }, thizCls, callCtx);
      gen(env, JmpZero, taken, flag);
    },
    [&] {
      gen(env, IncRef, thiz);
      return thiz;
    },
    [&] {
      hint(env, Block::Hint::Unlikely);
      return gen_missing_this();
    });
}

template<class Fn>
void optimizeProfiledCallMethod(IRGS& env,
                                const FCallArgs& fca,
                                SSATmp* objOrCls,
                                bool knownIfaceFunc,
                                const StringData* methodName,
                                bool dynamicCall,
                                bool suppressDynCallCheck,
                                uint32_t numExtraInputs,
                                Fn emitFCall) {
  always_assert(objOrCls->type().subtypeOfAny(TObj, TCls));
  auto const isStaticCall = objOrCls->type() <= TCls;
  auto profile = TargetProfile<MethProfile>(env.context, env.irb->curMarker(),
                                            methProfileKey.get());
  if (!profile.optimizing()) {
    emitFCall(&profile);
    return;
  }

  auto getCtx = [&](const Func* callee,
                    SSATmp* ctx,
                    const Class* cls) -> SSATmp* {
    if (isStaticCall) {
      return ldCtxForClsMethod(env, callee, ctx,
                               cls ? cls : callee->cls(), cls != nullptr);
    }

    ctx = cls ? gen(env, AssertType, Type::ExactObj(cls), ctx)
              : gen(env, AssertType, Type::SubObj(callee->implCls()), ctx);

    if (!callee->isStaticInPrologue()) return ctx;

    assertx(ctx->type() <= TObj);
    auto ret = cls ? cns(env, cls) : gen(env, LdObjClass, ctx);
    decRef(env, ctx);
    return ret;
  };

  auto const fallback = [&] {
    hint(env, Block::Hint::Unlikely);
    IRUnit::Hinter h(env.irb->unit(), Block::Hint::Unlikely);
    updateStackOffsetAndExceptionBoundary(env);
    emitFCall(nullptr, true /* no call profiling */);
    gen(env, Jmp, makeExit(env, nextSrcKey(env)));
  };

  MethProfile data = profile.data();

  if (auto const uniqueMeth = data.uniqueMeth()) {
    assertx(uniqueMeth->name()->same(methodName));
    if (auto const uniqueClass = data.uniqueClass()) {
      // Profiling saw a unique class.
      // Check for it, then burn in the func
      ifThen(
        env,
        [&] (Block* sideExit) {
          auto const ty = isStaticCall
            ? Type::ExactCls(uniqueClass) : Type::ExactObj(uniqueClass);
          auto const refined = gen(env, CheckType, ty, sideExit, objOrCls);
          env.irb->constrainValue(refined, GuardConstraint(uniqueClass));
          auto const ctx = getCtx(uniqueMeth, refined, uniqueClass);
          discard(env, numExtraInputs);
          prepareAndCallKnown(env, uniqueMeth, fca, ctx,
                              dynamicCall, suppressDynCallCheck);
        },
        fallback
      );
      return;
    }

    // Although there were multiple classes, the method was unique
    // (this comes up eg for a final method in a base class).
    ifThen(
      env,
      [&] (Block* sideExit) {
        auto const slot = cns(env, uniqueMeth->methodSlot());
        auto const cls = isStaticCall
          ? objOrCls : gen(env, LdObjClass, objOrCls);
        auto const len = gen(env, LdFuncVecLen, cls);
        auto const cmp = gen(env, LteInt, len, slot);
        gen(env, JmpNZero, sideExit, cmp);
        auto const meth = gen(env, LdClsMethod, cls, slot);
        auto const same = gen(env, EqFunc, meth, cns(env, uniqueMeth));
        gen(env, JmpZero, sideExit, same);
        auto const ctx = getCtx(uniqueMeth, objOrCls, nullptr);
        discard(env, numExtraInputs);
        prepareAndCallKnown(env, uniqueMeth, fca, ctx,
                            dynamicCall, suppressDynCallCheck);
      },
      fallback
    );
    return;
  }

  if (auto const baseMeth = data.baseMeth()) {
    if (!baseMeth->name()->same(methodName)) {
      emitFCall();
      return;
    }

    // The method was defined in a common base class.  We just need to check for
    // an instance of the class, and then use the method from the right slot.
    ifThen(
      env,
      [&] (Block* sideExit) {
        auto const cls = isStaticCall
          ? objOrCls : gen(env, LdObjClass, objOrCls);
        auto const ecData = ExtendsClassData{baseMeth->cls(), true};
        auto const flag = gen(env, ExtendsClass, ecData, cls);
        gen(env, JmpZero, sideExit, flag);
        auto const slot = cns(env, baseMeth->methodSlot());
        auto const meth = gen(env, LdClsMethod, cls, slot);
        auto const ctx = getCtx(baseMeth, objOrCls, nullptr);
        discard(env, numExtraInputs);
        prepareAndCallProfiled(env, meth, fca, ctx,
                               dynamicCall, suppressDynCallCheck);
      },
      fallback
    );
    return;
  }

  // If we know the object implements a known interface that defines the called
  // method, the other cases below are not worth the extra checks they insert.
  if (knownIfaceFunc) {
    emitFCall();
    return;
  }

  if (auto const intfMeth = data.interfaceMeth()) {
    if (!intfMeth->name()->same(methodName)) {
      emitFCall();
      return;
    }

    auto const vtableSlot = intfMeth->cls()->preClass()->ifaceVtableSlot();
    if (vtableSlot == kInvalidSlot) {
      emitFCall();
      return;
    }

    // The method was defined in a common interface, so check for that and use
    // LdIfaceMethod.
    ifThen(
      env,
      [&] (Block* sideExit) {
        auto const cls = isStaticCall
          ? objOrCls : gen(env, LdObjClass, objOrCls);
        auto const cData = InstanceOfIfaceVtableData{
          intfMeth->cls(), !isStaticCall
        };
        auto const flag = gen(env, InstanceOfIfaceVtable, cData, cls);
        gen(env, JmpZero, sideExit, flag);
        auto const imData = IfaceMethodData{vtableSlot, intfMeth->methodSlot()};
        auto const meth = gen(env, LdIfaceMethod, imData, cls);
        auto const ctx = getCtx(intfMeth, objOrCls, nullptr);
        discard(env, numExtraInputs);
        prepareAndCallProfiled(env, meth, fca, ctx,
                               dynamicCall, suppressDynCallCheck);
      },
      fallback
    );
    return;
  }

  emitFCall();
}

void fcallObjMethodObj(IRGS& env, const FCallArgs& fca, SSATmp* obj,
                       const StringData* clsHint, SSATmp* methodName,
                       bool dynamicCall, uint32_t numExtraInputs) {
  assertx(obj->isA(TObj));
  assertx(methodName->isA(TStr));

  implIncStat(env, Stats::ObjMethod_total);

  auto const lookup = [&] {
    auto constexpr notFound = ImmutableObjMethodLookup {
      ImmutableObjMethodLookup::Type::NotFound,
      nullptr
    };

    if (!methodName->hasConstVal()) return notFound;

    if (!clsHint->empty()) {
      auto const cls = lookupKnown(env, clsHint);
      if (cls && isNormalClass(cls)) {
        obj = gen(env, AssertType, Type::SubObj(cls), obj);
        auto const callCtx =
          MemberLookupContext(callContext(env, fca, cls), curFunc(env));
        return lookupImmutableObjMethod(cls, methodName->strVal(),
                                        callCtx, true);
      }
    }

    if (auto cls = obj->type().clsSpec().cls()) {
      if (!env.irb->constrainValue(obj, GuardConstraint(cls).setWeak())) {
        // We know the class without having to specialize a guard any further.
        // We may still want to use MethProfile to gather more information in
        // case the class isn't known exactly.
        auto const exactClass =
          obj->type().clsSpec().exact() ||
          cls->attrs() & AttrNoOverrideRegular;
        auto const callCtx =
          MemberLookupContext(callContext(env, fca, cls), curFunc(env));
        return lookupImmutableObjMethod(cls, methodName->strVal(),
                                        callCtx, exactClass);
      }
    }

    return notFound;
  }();

  const Func* knownIfaceFunc = nullptr;

  // If we know which exact or overridden method to call, we don't need PGO.
  switch (lookup.type) {
    case ImmutableObjMethodLookup::Type::Func:
      implIncStat(env, Stats::ObjMethod_known);
      if (lookup.func->isStaticInPrologue()) {
        gen(env, ThrowHasThisNeedStatic, cns(env, lookup.func));
        return;
      }
      discard(env, numExtraInputs);
      prepareAndCallKnown(env, lookup.func, fca, obj, dynamicCall, false);
      return;
    case ImmutableObjMethodLookup::Type::Class: {
      auto const func = lookupObjMethodNonExactFunc(env, obj, lookup.func);
      discard(env, numExtraInputs);
      prepareAndCallProfiled(env, func, fca, obj, dynamicCall, false);
      return;
    }
    case ImmutableObjMethodLookup::Type::NotFound:
      break;
    case ImmutableObjMethodLookup::Type::Interface:
      knownIfaceFunc = lookup.func;
      break;
  }

  auto const emitFCall = [&] (TargetProfile<MethProfile>* profile = nullptr,
                              bool noCallProfiling = false) {
    auto const profileMethod = [&] (SSATmp* callee) {
      if (!profile || !profile->profiling()) return;
      auto const cls = gen(env, LdObjClass, obj);
      auto const pctData = ProfileCallTargetData { profile->handle() };
      gen(env, ProfileMethod, pctData, cls, callee);
    };


    if (knownIfaceFunc == nullptr) {
      fcallObjMethodUnknown(env, fca, obj, methodName, dynamicCall,
                            numExtraInputs, profileMethod, noCallProfiling);
    } else {
      auto const func = lookupObjMethodInterfaceFunc(env, obj, knownIfaceFunc);
      profileMethod(func);
      discard(env, numExtraInputs);
      if (noCallProfiling) {
        prepareAndCallUnknown(env, func, fca, obj, dynamicCall, false, true);
      } else {
        prepareAndCallProfiled(env, func, fca, obj, dynamicCall, false);
      }
    }
  };

  // If the method has reified generics, we can't burn the value in the JIT
  if (!Cfg::Repo::Authoritative || !methodName->hasConstVal() ||
      fca.hasGenerics()) {
    return emitFCall();
  }

  // If we don't know anything about the object's class, or all we know is an
  // interface that it implements, then enable PGO.
  optimizeProfiledCallMethod(env, fca, obj, knownIfaceFunc != nullptr,
                             methodName->strVal(),
                             dynamicCall, false,
                             numExtraInputs, emitFCall);
}

void fcallFuncObj(IRGS& env, const FCallArgs& fca) {
  auto const obj = topC(env);
  assertx(obj->isA(TObj));

  auto const slowExit = makeExitSlow(env);
  auto const cls = gen(env, LdObjClass, obj);
  auto const funcOpt = gen(env, LdObjInvoke, cls);
  auto const func = gen(env, CheckNonNull, slowExit, funcOpt);
  discard(env);
  updateStackOffsetAndExceptionBoundary(env);
  prepareAndCallProfiled(env, func, fca, obj, false, false);
}

void fcallFuncFunc(IRGS& env, const FCallArgs& fca) {
  auto const func = popC(env);
  assertx(func->isA(TFunc));
  assertx(!isRefcountedType(KindOfFunc));
  updateStackOffsetAndExceptionBoundary(env);

  ifElse(
    env,
    [&] (Block* taken) {
      // This is super sketchy, as we did not check for isMethCaller() yet.
      // However, it is faster than checking isMethCaller() first, and it does
      // the right thing(tm) no matter whether the CheckNonNull jumps or not.
      gen(env, CheckNonNull, taken, gen(env, LdFuncCls, func));
      auto const attr = AttrData { AttrIsMethCaller };
      gen(env, JmpNZero, taken, gen(env, FuncHasAttr, attr, func));
    },
    [&] { // next, attrs & IsMethCaller == 0 && Func has Cls
      hint(env, Block::Hint::Unlikely);
      auto const err = cns(env, makeStaticString(Strings::CALL_ILLFORMED_FUNC));
      gen(env, RaiseError, err);
    }
  );
  prepareAndCallProfiled(env, func, fca, nullptr, false, false);
}

void fcallFuncRFunc(IRGS& env, const FCallArgs& fca) {
  auto const rfunc = topC(env);
  assertx(rfunc->isA(TRFunc));

  auto const func = gen(env, LdFuncFromRFunc, rfunc);
  auto const generics = gen(env, LdGenericsFromRFunc, rfunc);
  gen(env, IncRef, generics);

  popDecRef(env);
  push(env, generics);
  updateStackOffsetAndExceptionBoundary(env);
  prepareAndCallProfiled(env, func, fca.withGenerics(), nullptr, false, false);
}

void fcallFuncClsMeth(IRGS& env, const FCallArgs& fca) {
  auto const clsMeth = popC(env);
  assertx(clsMeth->isA(TClsMeth));
  assertx(!isRefcountedType(KindOfClsMeth));
  updateStackOffsetAndExceptionBoundary(env);

  auto const cls = gen(env, LdClsFromClsMeth, clsMeth);
  auto const func = gen(env, LdFuncFromClsMeth, clsMeth);
  prepareAndCallProfiled(env, func, fca, cls, false, false);
}

void fcallFuncRClsMeth(IRGS& env, const FCallArgs& fca) {
  auto const rclsMeth = topC(env);
  assertx(rclsMeth->isA(TRClsMeth));

  auto const cls = gen(env, LdClsFromRClsMeth, rclsMeth);
  auto const func = gen(env, LdFuncFromRClsMeth, rclsMeth);
  auto const generics = gen(env, LdGenericsFromRClsMeth, rclsMeth);
  gen(env, IncRef, generics);

  popDecRef(env);
  push(env, generics);
  updateStackOffsetAndExceptionBoundary(env);
  prepareAndCallProfiled(env, func, fca.withGenerics(), cls, false, false);
}

void fcallFuncStr(IRGS& env, const FCallArgs& fca) {
  auto const str = topC(env);
  assertx(str->isA(TStr));

  // TODO: improve this if str->hasConstVal()
  auto const funcN = gen(env, LdFunc, str);
  auto const func = gen(env, CheckNonNull, makeExitSlow(env), funcN);
  popDecRef(env);
  updateStackOffsetAndExceptionBoundary(env);
  emitModuleBoundaryCheck(env, func);
  prepareAndCallProfiled(env, func, fca, nullptr, true, false);
}

} // namespace

template <typename T>
void emitModuleBoundaryCheckKnownImpl(IRGS& env, const T* symbol) {
  auto const caller = curFunc(env);
  assertx(symbol && caller);
  if (symbol->moduleName() == caller->moduleName()) return;

  if (symbol->isInternal()) {
    if constexpr (std::is_same_v<T, Class>) {
      if (canTestsBypassVisibility(symbol->preClass()->userAttributes(),
                                   curClass(env))) return;
    }
    gen(env,
        RaiseModuleBoundaryViolation,
        OptClassAndFuncData { curClass(env), caller },
        cns(env, symbol));
  }
}

void emitModuleBoundaryCheckKnown(IRGS& env, const Func* symbol) {
  emitModuleBoundaryCheckKnownImpl(env, symbol);
}

void emitModuleBoundaryCheckKnown(IRGS& env, const Class* symbol) {
  emitModuleBoundaryCheckKnownImpl(env, symbol);
}

void emitModuleBoundaryCheckKnown(IRGS& env, const Class::Prop* prop) {
  auto const caller = curFunc(env);
  if (will_symbol_raise_module_boundary_violation(prop, caller)) {
      auto const data = ModulePropAccessData { caller, prop->cls.get(), prop->name.get(), false };
      gen(env, RaiseModulePropertyViolation, data);
  }
}

void emitModuleBoundaryCheckKnown(IRGS& env, const Class::SProp* prop) {
  auto const caller = curFunc(env);
  if (will_symbol_raise_module_boundary_violation(prop, caller)) {
      auto const data = ModulePropAccessData { caller, prop->cls.get(), prop->name.get(), true };
      gen(env, RaiseModulePropertyViolation, data);
  }
}

void emitModuleBoundaryCheck(IRGS& env, SSATmp* symbol, bool func /* = true */) {
  if (!Cfg::Eval::EnforceModules) return;
  auto const caller = curFunc(env);
  if (symbol->hasConstVal()) {
    if (func) {
      emitModuleBoundaryCheckKnown(env, symbol->funcVal());
    } else {
      emitModuleBoundaryCheckKnown(env, symbol->clsVal());
    }
  } else {
    ifElse(
      env,
      [&] (Block* skip) {
        auto const data = AttrData { AttrInternal };
        auto const internal =
          gen(env, func ? FuncHasAttr : ClassHasAttr, data, symbol);
        gen(env, JmpZero, skip, internal);
      },
      [&] {
        ifElse(
          env,
          [&] (Block* skip) {
            auto violate =
              gen(env, CallViolatesModuleBoundary, FuncData { caller }, symbol);
            gen(env, JmpZero, skip, violate);
          },
          [&] {
            hint(env, Block::Hint::Unlikely);
            auto const data = OptClassAndFuncData { curClass(env), caller };
            gen(env, RaiseModuleBoundaryViolation, data, symbol);
          }
        );
      });
  }
}

void emitStrictPackageDynamicReference(IRGS& env, SSATmp* cls) {
  if (Cfg::Repo::Authoritative &&
      !Cfg::Eval::CanReportStrictDynamicReference) {
    return;
  }

  if (cls->hasConstVal()) {
    if (!cls->clsVal()->isInStrictPackage()) return;
    gen(env, CheckStrictPackageDynamicReference, cls);
    return;
  }

  ifThen(
    env,
    [&] (Block* taken) {
      auto const data = AttrData { AttrInStrictPackage };
      gen(env, JmpNZero, taken, gen(env, ClassHasAttr, data, cls));
    },
    [&] {
      hint(env, Block::Hint::Unlikely);
      gen(env, CheckStrictPackageDynamicReference, cls);
    }
  );
}

void emitFCallFuncD(IRGS& env, FCallArgs fca, const StringData* funcName) {
  auto const lookup = lookupKnownFuncMaybe(env, funcName);
  auto const fast = [&]() {
    emitModuleBoundaryCheckKnown(env, lookup.func);
    prepareAndCallKnown(env, lookup.func, fca, nullptr, false, false);
    return;
  };
  auto const slow = [&]() {
    auto const cachedFunc = gen(env, LdFuncCached, FuncNameData { funcName } );
    emitModuleBoundaryCheck(env, cachedFunc);
    prepareAndCallProfiled(env, cachedFunc, fca, nullptr, false, false);
  };
  auto const data = [&](const Func* func, bool success) {
    auto id = func ? func->getFuncId().toInt(): FuncId::Invalid.toInt();
    return LoggingSpeculateData {
      nullptr,
      nullptr,
      funcName,
      Op::FCallFuncD,
      id,
      success,
    };
  };

  switch (lookup.tag) {
    case Func::FuncLookupResult::None:
      if (Cfg::Sandbox::Speculate && Cfg::Eval::LogClsSpeculation) {
        gen(env, LogClsSpeculation, data(nullptr, false));
      }
      return slow();
    case Func::FuncLookupResult::Exact:
      return fast();
    case Func::FuncLookupResult::Maybe:
      auto const loadedFunc = gen(env, LdFuncCached, FuncNameData { funcName } );
      ifThenElse(
        env,
        [&] (Block* taken) {
          gen(env, EqFuncId, FuncData(lookup.func), taken, loadedFunc);
        },
        [&] {
          updateStackOffsetAndExceptionBoundary(env);
          if (Cfg::Eval::LogClsSpeculation) {
            gen(env, LogClsSpeculation, data(lookup.func, true));
          }
          fast();
          return;
        },
        [&] {
          hint(env, Block::Hint::Unlikely);
          updateStackOffsetAndExceptionBoundary(env);
          if (Cfg::Eval::LogClsSpeculation) {
            gen(env, LogClsSpeculation, data(lookup.func, false));
          }
          emitModuleBoundaryCheck(env, loadedFunc);
          prepareAndCallUnknown(env, loadedFunc, fca, nullptr,
                                false, false, true /* makeExit */);
          return;
        }
      );
  };
}

void emitFCallFunc(IRGS& env, FCallArgs fca) {
  auto const callee = topC(env);
  if (callee->isA(TObj)) return fcallFuncObj(env, fca);
  if (callee->isA(TFunc)) return fcallFuncFunc(env, fca);
  if (callee->isA(TClsMeth)) return fcallFuncClsMeth(env, fca);
  if (callee->isA(TStr)) return fcallFuncStr(env, fca);
  if (callee->isA(TRFunc)) return fcallFuncRFunc(env, fca);
  if (callee->isA(TRClsMeth)) return fcallFuncRClsMeth(env, fca);
  return interpOne(env);
}

void emitResolveFunc(IRGS& env, const StringData* name) {
  auto const cachedFunc = lookupKnownFunc(env, name);
  if (!cachedFunc) {
    auto const func = gen(env, LookupFuncCached, FuncNameData { name } );
    emitModuleBoundaryCheck(env, func);
    push(env, func);
    return;
  }
  emitModuleBoundaryCheckKnown(env, cachedFunc);
  push(env, cns(env, cachedFunc));
}

void emitResolveMethCaller(IRGS& env, const StringData* name) {
  auto const func = lookupKnownFunc(env, name);

  // We de-duplicate meth_caller across the repo which may lead to the resolved
  // meth caller being in a different unit (and therefore unavailable at this
  // point). The interpreter will perform the load.
  if (!func) return interpOne(env);

  assertx(func->isMethCaller());

  auto const className = func->methCallerClsName();
  auto const methodName = func->methCallerMethName();

  auto const ok = [&] () -> bool {
    auto const cls = lookupKnown(env, className);
    if (cls && !isTrait(cls)) {
      auto const callCtx = MemberLookupContext(curClass(env), curFunc(env));
      auto const res = lookupImmutableObjMethod(cls, methodName, callCtx, false);
      return res.func && checkMethCallerTarget(res.func, curClass(env), false);
    }
    return false;
  }();

  if (!ok) return interpOne(env);
  push(env, cns(env, func));
}

void emitResolveRFunc(IRGS& env, const StringData* name) {
  if (!topC(env)->isA(TVec)) {
    return interpOne(env);
  }
  auto const tsList = popC(env);

  auto const funcTmp = [&] () -> SSATmp* {
    auto const func = lookupKnownFunc(env, name);
    if (!func) return gen(env, LookupFuncCached, FuncNameData { name } );
    return cns(env, func);
  }();

  ifThenElse(
    env,
    [&] (Block* taken) {
      auto const res = gen(env, FuncHasReifiedGenerics, funcTmp);
      gen(env, JmpZero, taken, res);
    },
    [&] {
      gen(env, CheckFunReifiedGenericMismatch, funcTmp, tsList);
      push(env, gen(env, NewRFunc, funcTmp, tsList));
    },
    [&] {
      decRef(env, tsList);
      push(env, funcTmp);
    }
  );
}

namespace {

//////////////////////////////////////////////////////////////////////

SSATmp* specialClsRefToCls(IRGS& env, SpecialClsRef ref) {
  switch (ref) {
    case SpecialClsRef::LateBoundCls:
      if (!curClass(env)) return nullptr;
      return ldCtxCls(env);
    case SpecialClsRef::SelfCls:
      if (auto const clss = curClass(env)) return cns(env, clss);
      return nullptr;
    case SpecialClsRef::ParentCls:
      if (auto const clss = curClass(env)) {
        if (auto const parent = clss->parent()) return cns(env, parent);
      }
      return nullptr;
  }
  always_assert(false);
}

void emitDynamicConstructChecks(IRGS& env, SSATmp* cls) {
  if (Cfg::Eval::ForbidDynamicConstructs <= 0) return;
  if (cls->hasConstVal()) {
    if (cls->clsVal()->isDynamicallyConstructible()) return;
    gen(env, RaiseForbiddenDynConstruct, cls);
    return;
  }

  ifElse(
    env,
    [&] (Block* skip) {
      auto const data = AttrData { AttrDynamicallyConstructible };
      auto const dynConstructible = gen(env, ClassHasAttr, data, cls);
      gen(env, JmpNZero, skip, dynConstructible);
    },
    [&] {
      hint(env, Block::Hint::Unlikely);
      gen(env, RaiseForbiddenDynConstruct, cls);
    }
  );
}

} // namespace

void emitNewObj(IRGS& env) {
  auto const cls = popC(env);
  if (!cls->isA(TCls)) PUNT(NewObj-NotClass);
  emitDynamicConstructChecks(env, cls);
  push(env, gen(env, AllocObj, cls));
}

void emitNewObjD(IRGS& env, const StringData* className) {
  auto const lookup = lookupKnownMaybe(env, className);
  auto const cls = lookup.cls;
  bool const fastPath = lookup.cls && isNormalClass(cls)
    && !isAbstract(cls) && !cls->hasNativePropHandler();

  auto const knownClass = [&]() {
    if (fastPath) {
      push(env, allocObjFast(env, cls));
    } else {
      push(env, gen(env, AllocObj, cns(env, cls)));
    }
  };

  auto const slow = [&]() {
    auto const cachedCls = gen(env, LdClsCached, LdClsFallbackData::Fatal(),
                               cns(env, className));
    push(env, gen(env, AllocObj, cachedCls));
  };

  auto const data = [&](uint32_t id, bool success) {
    return LoggingSpeculateData {
      className,
      curClass(env) ? curClass(env)->name() : nullptr,
      nullptr,
      Op::NewObjD,
      id,
      success,
    };
  };

  switch (lookup.tag) {
    case Class::ClassLookupResult::Exact: return knownClass();
    case Class::ClassLookupResult::None: {
      if (Cfg::Sandbox::Speculate) {
        if (Cfg::Eval::LogClsSpeculation) {
          gen(env, LogClsSpeculation, data(ClassId::Invalid, false));
        }
        gen(env, LdClsCached, LdClsFallbackData::Fatal(), cns(env, className));
        gen(env, Jmp, makeExit(env));
        return;
      } else {
        return slow();
      }
    };
    case Class::ClassLookupResult::Maybe: {
      if (!fastPath) return slow();
      gen(env, LdClsCached, LdClsFallbackData::Fatal(), cns(env, className));
      auto const isEqual = gen(env, EqClassId, ClassIdData(lookup.cls));
      return ifThenElse(
        env,
        [&] (Block* taken) {
          gen(env, JmpZero, taken, isEqual);
        },
        [&] {
          if (Cfg::Eval::LogClsSpeculation) {
            gen(env, LogClsSpeculation, data(lookup.cls->classId().id(), true));
          }
          push(env, allocObjFast(env, cls));
        },
        [&] {
          hint(env, Block::Hint::Unlikely);
          if (Cfg::Eval::LogClsSpeculation) {
            gen(env, LogClsSpeculation, data(lookup.cls->classId().id(), false));
          }
          slow();
        }
      );
    }
    not_reached();
  }
}

void emitNewObjS(IRGS& env, SpecialClsRef ref) {
  auto const cls = specialClsRefToCls(env, ref);
  if (!cls) return interpOne(env);
  if (!cls->isA(TCls)) PUNT(NewObj-NotClass);
  if (ref == SpecialClsRef::LateBoundCls) {
    ifThen(
      env,
      [&] (Block* taken) {
        auto const res = gen(env, ClassHasReifiedGenerics, cls);
        gen(env, JmpNZero, taken, res);
      },
      [&] {
        hint(env, Block::Hint::Unlikely);
        auto const err = cns(env, makeStaticString("Cannot call new static since class has reified generics"));
        gen(env, RaiseError, err);
      }
    );
  }
  push(env, gen(env, AllocObj, cls));
}

void emitFCallCtor(IRGS& env, FCallArgs fca, const StringData* clsHint) {
  assertx(fca.numRets == 1);
  assertx(fca.asyncEagerOffset == kInvalidOffset);
  auto const objPos = static_cast<int32_t>(fca.numInputs() + (kNumActRecCells - 1));
  auto const obj = topC(env, BCSPRelOffset{objPos});
  if (!obj->isA(TObj)) PUNT(FCallCtor-NonObj);

  auto const exactCls = [&] {
    if (!clsHint->empty()) {
      auto const cls = lookupKnown(env, clsHint);
      if (cls && isNormalClass(cls)) return cls;
    }
    return obj->type().clsSpec().exactCls();
  }();
  if (exactCls) {
    auto const callCtx = MemberLookupContext(curClass(env), curFunc(env));
    if (auto const ctor = lookupImmutableCtor(exactCls, callCtx)) {
      return prepareAndCallKnown(env, ctor, fca, obj, false, false);
    }
  }

  auto const cls = exactCls ? cns(env, exactCls) : gen(env, LdObjClass, obj);
  auto const callee = gen(env, LdClsCtor, cls, cns(env, curFunc(env)));
  prepareAndCallProfiled(env, callee, fca, obj, false, false);
}

void emitLockObjOnFrameUnwind(IRGS& env, PC pc) {
  auto const op = decode_op(pc);
  if (LIKELY(op != OpFCallCtor)) return;
  auto fca = decodeFCallArgs(op, pc, nullptr /* StringDecoder */);
  if (!fca.lockWhileUnwinding()) return;

  auto const obj = gen(env, AssertType, TObj, topC(env));
  gen(env, LockObj, obj);
}

void emitLockObj(IRGS& env) {
  auto obj = topC(env);
  if (!obj->isA(TObj)) PUNT(LockObj-NonObj);
  gen(env, LockObj, obj);
}

namespace {

void fcallObjMethod(IRGS& env, const FCallArgs& fca, const StringData* clsHint,
                    ObjMethodOp subop, SSATmp* methodName, bool dynamicCall,
                    bool extraInput) {
  assertx(methodName->isA(TStr));
  auto const offset = kNumActRecCells - 1;
  auto const objPos = fca.numInputs() + (extraInput ? (offset + 1) : offset);
  auto const obj = topC(env, BCSPRelOffset { static_cast<int32_t>(objPos) });

  if (obj->type() <= TObj) {
    fcallObjMethodObj(env, fca, obj, clsHint, methodName, dynamicCall,
                      extraInput ? 1 : 0);
    return;
  }

  int locId = 0;
  // null?->method(...), pop extra stack input, all arguments and uninits,
  // the null "object" and all uninits for inout returns, then push null.
  if (obj->type() <= TInitNull && subop == ObjMethodOp::NullSafe) {
    if (extraInput) popDecRef(env, static_cast<DecRefProfileId>(locId++), DataTypeGeneric);
    if (fca.hasGenerics()) popDecRef(env, static_cast<DecRefProfileId>(locId++), DataTypeGeneric);
    if (fca.hasUnpack()) popDecRef(env, static_cast<DecRefProfileId>(locId++), DataTypeGeneric);

    // Save any inout arguments, as those will be pushed unchanged as
    // the output.
    std::vector<SSATmp*> inOuts;
    for (uint32_t i = 0; i < fca.numArgs; ++i) {
      if (fca.enforceInOut() && fca.isInOut(fca.numArgs - i - 1)) {
        inOuts.emplace_back(popC(env));
      } else {
        popDecRef(env, static_cast<DecRefProfileId>(locId++), DataTypeGeneric);
      }
    }

    for (uint32_t i = 0; i < kNumActRecCells - 1; ++i) popU(env);
    popDecRef(env, static_cast<DecRefProfileId>(locId), DataTypeGeneric);
    for (uint32_t i = 0; i < fca.numRets - 1; ++i) popU(env);

    assertx(inOuts.size() == fca.numRets - 1);
    for (auto const tmp : inOuts) push(env, tmp);
    push(env, cns(env, TInitNull));
    return;
  }

  interpOne(env);
}

} // namespace

void emitFCallObjMethod(IRGS& env, FCallArgs fca, const StringData* clsHint,
                        ObjMethodOp subop) {
  auto const methodName = topC(env);
  if (!methodName->isA(TStr)) return interpOne(env);
  fcallObjMethod(env, fca, clsHint, subop, methodName, true, true);
}

void emitFCallObjMethodD(IRGS& env, FCallArgs fca, const StringData* clsHint,
                         ObjMethodOp subop, const StringData* methodName) {
  fcallObjMethod(env, fca, clsHint, subop, cns(env, methodName), false, false);
}


namespace {

SSATmp* loadClsMethodUnknown(IRGS& env,
                             const ClsMethodData& data,
                             Block* onFail) {
  // Look up the Func* in the targetcache. If it's not there, try the slow
  // path. If that fails, slow exit.
  return cond(
    env,
    [&] (Block* taken) {
      return gen(env, LdClsMethodCacheFunc, data, taken);
    },
    [&] (SSATmp* func) { // next
      implIncStat(env, Stats::TgtCache_StaticMethodHit);
      return func;
    },
    [&] { // taken
      hint(env, Block::Hint::Unlikely);
      auto const result = gen(env, LookupClsMethodCache, data, fp(env));
      return gen(env, CheckNonNull, onFail, result);
    }
  );
}

}

void emitFCallClsMethodD(IRGS& env,
                         FCallArgs fca,
                         const StringData* className,
                         const StringData* methodName) {
  auto const lookup = lookupKnownMaybe(env, className);
  auto const slow = [&]() {
    auto const callerCtx = [&] {
      if (!fca.context) return curClass(env);
      auto const ret = lookupKnownWithUnit(env, fca.context);
      if (!ret) PUNT(no-context);
      return ret;
    }();

    auto const slowExit = makeExitSlow(env);
    auto const ne = NamedType::getOrCreate(className);
    auto const data =
      ClsMethodData { className, methodName, ne, callerCtx, curFunc(env) };
    auto const func = loadClsMethodUnknown(env, data, slowExit);
    auto const ctx = gen(env, LdClsMethodCacheCls, data);
    emitModuleBoundaryCheck(env, ctx, false);
    prepareAndCallProfiled(env, func, fca, ctx, false, false);
  };

  auto const data = [&](const Class* ctx, uint32_t id, bool success) {
    return LoggingSpeculateData {
      className,
      ctx ? ctx->name() : nullptr,
      methodName,
      Op::FCallClsMethodD,
      id,
      success,
    };
  };

  switch (lookup.tag) {
    case Class::ClassLookupResult::Exact: {
      auto const cls = lookup.cls;
      auto const callCtx =
        MemberLookupContext(callContext(env, fca, cls), curFunc(env));
      auto const func = lookupImmutableClsMethod(cls, methodName, callCtx, true);
      if (!func) return slow();
      auto const ctx = ldCtxForClsMethod(env, func, cns(env, cls), cls, true);
      emitModuleBoundaryCheckKnown(env, cls);
      return prepareAndCallKnown(env, func, fca, ctx, false, false);
    }
    case Class::ClassLookupResult::None: {
      if (Cfg::Sandbox::Speculate) {
        if (Cfg::Eval::LogClsSpeculation) {
          gen(env, LogClsSpeculation, data(nullptr, ClassId::Invalid, false));
        }
        gen(env, LdClsCached, LdClsFallbackData::Fatal(), cns(env, className));
        gen(env, Jmp, makeExit(env));
        return;
      } else {
        return slow();
      }
    }
    case Class::ClassLookupResult::Maybe: {
      auto const cls = lookup.cls;
      assertx(!Cfg::Repo::Authoritative);
      assertx(!lookup.cls->isPersistent());
      assertx(!lookup.cls->classId().isInvalid());
      auto const callCtx =
        MemberLookupContext(callContext(env, fca, cls), curFunc(env));
      auto const func = lookupImmutableClsMethod(cls, methodName, callCtx, true);
      if (!func) return slow();

      gen(env, LdClsCached, LdClsFallbackData::Fatal(), cns(env, className));
      auto const isEqual = gen(env, EqClassId, ClassIdData(lookup.cls));
      return ifThenElse(
        env,
        [&] (Block* taken) {
          gen(env, JmpZero, taken, isEqual);
        },
        [&] {
          updateStackOffsetAndExceptionBoundary(env);
          auto const ctx = ldCtxForClsMethod(env, func, cns(env, cls), cls, true);
          emitModuleBoundaryCheckKnown(env, cls);
          if (Cfg::Eval::LogClsSpeculation) {
            gen(env, LogClsSpeculation, data(callCtx.cls(),
                                             lookup.cls->classId().id(),
                                             true));
          }
          prepareAndCallKnown(env, func, fca, ctx, false, false);
        },
        [&] {
          hint(env, Block::Hint::Unlikely);
          updateStackOffsetAndExceptionBoundary(env);
          if (Cfg::Eval::LogClsSpeculation) {
            gen(env, LogClsSpeculation, data(callCtx.cls(),
                                             lookup.cls->classId().id(),
                                             false));
          }
          slow();
        }
      );
    }
    not_reached();
  }
}

namespace {

SSATmp* forwardCtx(IRGS& env, const Func* parentFunc, SSATmp* funcTmp) {
  assertx(!parentFunc->isClosureBody());
  assertx(funcTmp->type() <= TFunc);

  if (parentFunc->isStatic()) {
    return ldCtxCls(env);
  }

  if (!hasThis(env)) {
    assertx(!parentFunc->isStaticInPrologue());
    gen(env, ThrowMissingThis, funcTmp);
    return cns(env, TBottom);
  }

  auto const obj = ldThis(env);
  gen(env, IncRef, obj);
  return obj;
}

SSATmp* lookupClsMethodKnown(IRGS& env,
                             const StringData* methodName,
                             SSATmp* callerCtx,
                             const Class *baseClass,
                             bool exact,
                             bool forward,
                             SSATmp*& calleeCtx,
                             const Class* ctx) {
  auto const callCtx = MemberLookupContext(ctx, curFunc(env));
  auto const func = lookupImmutableClsMethod(
    baseClass, methodName, callCtx, exact);
  if (!func) return nullptr;

  auto funcTmp = exact || func->isImmutableFrom(baseClass)
    ? cns(env, func)
    : gen(env, LdClsMethod, callerCtx, cns(env, func->methodSlot()));

  calleeCtx = forward
    ? forwardCtx(env, func, funcTmp)
    : ldCtxForClsMethod(env, func, callerCtx, baseClass, exact);
  return funcTmp;
}

void checkGenericsAndResolveRClsMeth(IRGS& env, SSATmp* cls, SSATmp* func,
                                     SSATmp* tsList) {
  popC(env); // Pop generics

  ifThenElse(
    env,
    [&] (Block* taken) {
      auto const res = gen(env, FuncHasReifiedGenerics, func);
      gen(env, JmpZero, taken, res);
    },
    [&] {
      gen(env, CheckFunReifiedGenericMismatch, func, tsList);
      push(env, gen(env, NewRClsMeth, cls, func, tsList));
      // NewRClsMeth consumes the reference to the generics
    },
    [&] {
      push(env, gen(env, NewClsMeth, cls, func));
      decRef(env, tsList);
    }
  );
}

void resolveClsMethodCommon(IRGS& env, SSATmp* clsVal,
                            const StringData* methodName,
                            uint32_t numExtraInputs,
                            SSATmp* generics) {
  assertx(clsVal->isA(TCls));
  auto const cs = clsVal->type().clsSpec();
  if (!cs) return interpOne(env);
  auto const exactClass = cs.exact() || cs.cls()->attrs() & AttrNoOverride;
  SSATmp* ctx;
  auto const funcTmp = lookupClsMethodKnown(env, methodName, clsVal, cs.cls(),
                                            exactClass, false, ctx,
                                            curClass(env));
  if (!funcTmp) return interpOne(env);
  assertx(funcTmp->isA(TFunc));
  gen(env, CheckClsMethFunc, funcTmp);
  discard(env, numExtraInputs);
  if (generics != nullptr) {
    checkGenericsAndResolveRClsMeth(env, clsVal, funcTmp, generics);
  } else {
    push(env, gen(env, NewClsMeth, clsVal, funcTmp));
  }
}

void checkClsMethodAndLdCtx(IRGS& env, const Class* cls, const Func* func,
                            const StringData* className) {
  gen(env, CheckClsMethFunc, cns(env, func));
  if (!lookupKnown(env, cls)) {
    gen(env, LdClsCached, LdClsFallbackData::Fatal(), cns(env, className));
  }
  ldCtxForClsMethod(env, func, cns(env, cls), cls, true);
}

std::pair<SSATmp*, SSATmp*>
resolveClsMethodDSlow(IRGS& env, const StringData* className,
                      const StringData* methodName) {
  auto const slowExit = makeExitSlow(env);
  auto const ne = NamedType::getOrCreate(className);
  auto const data =
    ClsMethodData { className, methodName, ne, curClass(env), curFunc(env) };
  auto const func = loadClsMethodUnknown(env, data, slowExit);
  gen(env, CheckClsMethFunc, func);
  auto const cls = gen(env,
                       LdClsCached,
                       LdClsFallbackData::Fatal(),
                       cns(env, className));
  return std::pair(cls, func);
}

} // namespace

void emitResolveClsMethod(IRGS& env, const StringData* methodName) {
  auto const cls = topC(env);
  if (!cls->isA(TCls)) return interpOne(env);
  resolveClsMethodCommon(env, cls, methodName, 1, nullptr);
}

void emitResolveClsMethodD(IRGS& env, const StringData* className,
                           const StringData* methodName) {
  auto const cls = lookupKnown(env, className);
  if (cls) {
    auto const callCtx = MemberLookupContext(curClass(env), curFunc(env));
    auto const func = lookupImmutableClsMethod(cls, methodName, callCtx, true);
    // Confirm the class is resolvable
    // Elided at simplify step since class is trusted
    if (func) {
      checkClsMethodAndLdCtx(env, cls, func, className);
      // For clsmeth, we want to return the class user gave,
      // not the class where func is associated with.
      push(env, gen(env, NewClsMeth, cns(env, cls), cns(env, func)));
    } else {
      resolveClsMethodCommon(env, cns(env, cls), methodName, 0, nullptr);
    }
    return;
  }

  auto [clsTmp, funcTmp] = resolveClsMethodDSlow(env, className, methodName);
  push(env, gen(env, NewClsMeth, clsTmp, funcTmp));
}

void emitResolveClsMethodS(IRGS& env, SpecialClsRef ref,
                           const StringData* methodName) {
  auto const cls = specialClsRefToCls(env, ref);
  if (!cls) return interpOne(env);
  resolveClsMethodCommon(env, cls, methodName, 0, nullptr);
}

void emitResolveRClsMethod(IRGS& env, const StringData* methodName) {
  auto const generics = topC(env);
  if (!generics->isA(TVec)) {
    return interpOne(env);
  }
  auto const cls = topC(env, BCSPRelOffset { 1 });
  if (!cls->isA(TCls)) return interpOne(env);
  resolveClsMethodCommon(env, cls, methodName, 1, generics);
}

void emitResolveRClsMethodD(IRGS& env, const StringData* className,
                            const StringData* methodName) {
  auto const generics = topC(env);
  if (!generics->isA(TVec)) {
    return interpOne(env);
  }

  auto const cls = lookupKnown(env, className);
  if (cls) {
    auto const callCtx = MemberLookupContext(curClass(env), curFunc(env));
    auto const func = lookupImmutableClsMethod(cls, methodName, callCtx, true);
    if (func) {
      checkClsMethodAndLdCtx(env, cls, func, className);

      // For cls meth, we want to return the class user gave,
      // not the class where func is associated with.
      checkGenericsAndResolveRClsMeth(env, cns(env, cls), cns(env, func), generics);
    } else {
      resolveClsMethodCommon(env, cns(env, cls), methodName, 0, generics);
    }
    return;
  }

  auto [clsTmp, funcTmp] = resolveClsMethodDSlow(env, className, methodName);
  checkGenericsAndResolveRClsMeth(env, clsTmp, funcTmp, generics);
}

void emitResolveRClsMethodS(IRGS& env, SpecialClsRef ref,
                            const StringData* methodName) {
  auto const generics = topC(env);
  if (!generics->isA(TVec)) {
    return interpOne(env);
  }
  auto const cls = specialClsRefToCls(env, ref);
  if (!cls) return interpOne(env);
  resolveClsMethodCommon(env, cls, methodName, 0, generics);
}

namespace {

void fcallClsMethodCommon(IRGS& env,
                          const FCallArgs& fca,
                          const StringData* clsHint,
                          SSATmp* clsVal,
                          const StringData* methName,
                          bool forward,
                          uint32_t numExtraInputs) {
  assertx(clsVal->isA(TCls));

  auto const emitFCall = [&] (TargetProfile<MethProfile>* profile = nullptr,
                              bool noCallProfiling = false) {
    auto const thiz =
      curClass(env) && hasThis(env) ? ldThis(env) : cns(env, nullptr);
    auto const funcN =
      gen(env, LookupClsMethod, clsVal, cns(env, methName), thiz, cns(env, curFunc(env)));
    auto const func = gen(env, CheckNonNull, makeExitSlow(env), funcN);

    if (profile && profile->profiling()) {
      auto const pctData = ProfileCallTargetData { profile->handle() };
      gen(env, ProfileMethod, pctData, clsVal, func);
    }

    auto const ctx = forward ? ldCtxCls(env) : clsVal;
    // For dynamic methods such as $c::foo(), numExtraInputs describes the
    // number of stack values representing the callee that need to be discarded.
    discard(env, numExtraInputs);
    if (noCallProfiling) {
      prepareAndCallUnknown(env, func, fca, ctx, false, false, true);
    } else {
      prepareAndCallProfiled(env, func, fca, ctx, false, false);
    }
  };

  auto const knownClass = [&] () -> std::pair<const Class*, bool> {
    // clsHint is added by HHBBC. Will be empty if not in repo mode.
    assertx(IMPLIES(!clsHint->empty(), Cfg::Repo::Authoritative));
    if (!clsHint->empty()) {
      auto const cls = lookupKnown(env, clsHint);
      if (cls && isNormalClass(cls)) return std::make_pair(cls, true);
    }

    if (auto const cs = clsVal->type().clsSpec()) {
      return std::make_pair(cs.cls(), cs.exact());
    }

    return std::make_pair(nullptr, false);
  }();

  if (knownClass.first) {
    SSATmp* ctx;
    auto const func = lookupClsMethodKnown(env, methName, clsVal,
                                           knownClass.first, knownClass.second,
                                           forward, ctx,
                                           callContext(env, fca,
                                                       knownClass.first));
    if (func) {
      discard(env, numExtraInputs);
      return prepareAndCallProfiled(env, func, fca, ctx, false, false);
    }
  }

  // If the method has reified generics, we can't burn the value in the JIT
  if (!Cfg::Repo::Authoritative || clsVal->hasConstVal() || forward ||
      fca.hasGenerics()) {
    emitFCall();
    return;
  }

  optimizeProfiledCallMethod(env, fca, clsVal, false, methName, false, false,
                             numExtraInputs, emitFCall);
}

} // namespace

void emitFCallClsMethodM(IRGS& env, FCallArgs fca, const StringData* clsHint,
                        const StringData* methName) {
  auto const name = topC(env);
  if (!name->type().subtypeOfAny(TObj, TCls, TStr, TLazyCls)) {
    interpOne(env);
    return;
  }
  auto const cls = [&] {
    if (name->isA(TCls)) return name;
    auto const ret = name->isA(TObj) ?
      gen(env, LdObjClass, name) : ldCls(env, name);
    if (name->isA(TStr)) {
      emitModuleBoundaryCheck(env, ret, false);

      if(Cfg::Eval::RaiseStrToClsConversionNoticeSampleRate > 0) {
        gen(env,
            RaiseStrToClassNotice,
            StrToClassData { StrToClassKind::StaticMethod },
            name);
      }
      emitStrictPackageDynamicReference(env, ret);
    }
    decRef(env, name);
    return ret;
  }();

  fcallClsMethodCommon(env, fca, clsHint, cls, methName, false, 1);
}

void emitFCallClsMethodSD(IRGS& env, FCallArgs fca, const StringData* clsHint,
                          SpecialClsRef ref, const StringData* methName) {
  auto const cls = specialClsRefToCls(env, ref);
  if (!cls) return interpOne(env);

  auto const fwd = ref == SpecialClsRef::SelfCls ||
                   ref == SpecialClsRef::ParentCls;
  fcallClsMethodCommon(env, fca, clsHint, cls, methName, fwd, 0);
}

//////////////////////////////////////////////////////////////////////

Type callReturnType(const Func* callee, bool mayIntercept) {
  // Don't make any assumptions about functions which can be intercepted. The
  // interception functions can return arbitrary types.
  if (mayIntercept && callee->isInterceptable()) return TInitCell;

  // Type based on function signature.
  auto const returnTy = [&] {
    if (callee->isGenerator()) {
      return callee->isAsync()
        ? Type::ExactObj(AsyncGenerator::classof())
        : Type::ExactObj(Generator::classof());
    }
    if (callee->isAsync()) {
      return Type::SubObj(c_Awaitable::classof());
    }
    return typeFromFuncReturn(callee);
  }();

  // Type based on HHBBC analysis.
  auto const hhbbcTy = [&] {
    auto const ty = typeFromRAT(callee->repoReturnType(), callee->cls());
    if (!callee->takesInOutParams()) return ty & TInitCell;
    if (!(ty <= TVec)) return TInitCell;
    return arrLikeElemType(ty, Type::cns(0), callee->cls()).first;
  }();

  return returnTy & hhbbcTy;
}

Type callOutType(const Func* callee, uint32_t inOutIdx, bool mayIntercept) {
  assertx(callee->takesInOutParams());
  assertx(inOutIdx < callee->numInOutParams());

  // Don't make any assumptions about functions which can be intercepted. The
  // interception functions can return arbitrary types.
  if (mayIntercept && callee->isInterceptable()) return TInitCell;

  // Type based on function signature.
  auto const returnTy = typeFromFuncOut(callee, inOutIdx);

  // Type based on HHBBC analysis.
  auto const hhbbcTy = [&] {
    auto const ty = typeFromRAT(callee->repoReturnType(), callee->cls());
    if (!(ty <= TVec)) return TInitCell;
    auto const off = callee->numInOutParams() - inOutIdx - 1;
    return arrLikeElemType(ty, Type::cns(off + 1), callee->cls()).first;
  }();

  return returnTy & hhbbcTy;
}

Type awaitedCallReturnType(const Func* callee, bool mayIntercept) {
  // Don't make any assumptions about functions which can be intercepted. The
  // interception functions can return arbitrary types.
  if (mayIntercept && callee->isInterceptable()) return TInitCell;

  // Type based on function signature.
  auto const returnTy = [&] {
    if (!callee->isAsyncFunction()) return TInitCell;
    return typeFromFuncReturn(callee);
  }();

  // Type based on HHBBC analysis.
  auto const hhbbcTy =
    typeFromRAT(callee->repoAwaitedReturnType(), callee->cls());

  return returnTy & hhbbcTy;
}

//////////////////////////////////////////////////////////////////////

}
