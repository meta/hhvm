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

#include "hphp/runtime/ext/asio/ext_wait-handle.h"
#include "hphp/runtime/ext/asio/ext_async-function-wait-handle.h"
#include "hphp/runtime/ext/asio/ext_async-generator.h"
#include "hphp/runtime/ext/asio/ext_static-wait-handle.h"
#include "hphp/runtime/ext/generator/ext_generator.h"
#include "hphp/runtime/base/repo-auth-type.h"

#include "hphp/runtime/vm/hhbc-codec.h"
#include "hphp/runtime/vm/resumable.h"
#include "hphp/runtime/vm/unwind.h"

#include "hphp/runtime/vm/jit/analysis.h"
#include "hphp/runtime/vm/jit/irgen-call.h"
#include "hphp/runtime/vm/jit/irgen-control.h"
#include "hphp/runtime/vm/jit/irgen-exit.h"
#include "hphp/runtime/vm/jit/irgen-inlining.h"
#include "hphp/runtime/vm/jit/irgen-internal.h"
#include "hphp/runtime/vm/jit/irgen-interpone.h"
#include "hphp/runtime/vm/jit/irgen-ret.h"

#include "hphp/util/configs/hhir.h"
#include "hphp/util/configs/jit.h"
#include "hphp/util/trace.h"

namespace HPHP::jit::irgen {

namespace {

//////////////////////////////////////////////////////////////////////

TRACE_SET_MOD(hhir)

// Returns true and fills "locals" with types of the function's locals if this
// op is an Await in "tail position" (whose results are immediately returned).
bool isTailAwait(const IRGS& env, std::vector<Type>& locals) {
  auto const unit = curUnit(env);
  auto const func = curFunc(env);
  auto const cls = curClass(env);
  auto sk = curSrcKey(env);

  TRACE(2, "isTailAwait analysis:\n");
  if (sk.op() != Op::Await) {
    FTRACE(2, "  Non-Await opcode: {}\n", instrToString(sk.pc(), func));
    return false;
  } else if (func->isGenerator()) {
    FTRACE(2, "  Function is a generator: {}\n", func->fullName());
    return false;
  } else if (func->lookupVarId(s_86metadata.get()) != kInvalidId) {
    FTRACE(2, "  Function has metadata: {}\n", s_86metadata.get()->data());
    return false;
  } else if (func->lookupVarId(s_86productAttributionData.get()) != kInvalidId) {
    FTRACE(2, "  Function has metadata: {}\n", s_86productAttributionData.get()->data());
    return false;
  }
  auto const offset = findExceptionHandler(func, bcOff(env));
  if (offset != kInvalidOffset) {
    FTRACE(2, "  Found exception handler at offset: {}\n", offset);
    return false;
  }

  // In some cases, we'll use a temporary local for tail awaits.
  // Track up to one usage of such a variable.
  auto resultLocal = kInvalidId;
  sk.advance(func);
  for (auto i = 0; i < func->numLocals(); i++) {
    auto const loc = Location::Local { safe_cast<uint32_t>(i) };
    locals.push_back(env.irb->fs().typeOf(loc));
  }

  auto const isRetCNop = [&]() {
    return getImm(sk.pc(), 0).u_OA == folly::to_underlying(VerifyRetKind::None)
      && (!Cfg::Eval::EnforceOverriddenReturnTypes
          || func->trustReturnType()
          || !func->hasInheritedReturnTypes()
      );
  };

  // Place a limit on the number of iterations in case of infinite loops.
  for (auto i = 256; i-- > 0;) {
    FTRACE(2, "  {}\n", instrToString(sk.pc(), func));
    switch (sk.op()) {
      case Op::RetC: return isRetCNop() && resultLocal == kInvalidId;
      case Op::AssertRATStk: break;
      case Op::AssertRATL: {
        auto const type = typeFromRAT(getImm(sk.pc(), 1, unit).u_RATA, cls);
        locals[getImm(sk.pc(), 0).u_ILA] &= type;
        break;
      }
      case Op::PopL: {
        if (resultLocal != kInvalidId) return false;
        resultLocal = getImm(sk.pc(), 0).u_LA;
        locals[resultLocal] = TCell;
        break;
      }
      case Op::PushL: {
        if (resultLocal != getImm(sk.pc(), 0).u_LA) return false;
        locals[resultLocal] = TUninit;
        resultLocal = kInvalidId;
        break;
      }
      case Op::Jmp: {
        sk = SrcKey(sk, sk.offset() + getImm(sk.pc(), 0).u_BA);
        continue;
      }
      default:               return false;
    }
    sk.advance(func);
  }

  TRACE(2, "  Processed too many opcodes; bailing\n");
  return false;
}

void doTailAwaitDecRefs(IRGS& env, const std::vector<Type>& locals) {
  auto const shouldFreeInline = [&]{
    // GenericRetDecRefs doesn't work in inlined frames.
    if (isInlining(env)) return true;

    auto numRefCounted = 0;
    for (auto i = 0; i < locals.size(); i++) {
      if (locals[i].maybe(TCounted)) numRefCounted++;
    }
    return numRefCounted <= Cfg::HHIR::InliningMaxReturnDecRefs;
  }();

  if (shouldFreeInline) {
    for (auto i = 0; i < locals.size(); i++) {
      if (!locals[i].maybe(TCounted)) continue;
      auto const data = LocalId { safe_cast<uint32_t>(i) };
      gen(env, AssertLoc, data, locals[i], fp(env));
      decRef(
          env,
          gen(env, LdLoc, data, locals[i], fp(env)),
          static_cast<DecRefProfileId>(i)
      );
    }
  } else {
    // GenericRetDecRefs doesn't work in inlined frames.
    assertx(!isInlining(env));
    gen(env, GenericRetDecRefs, fp(env));
  }
  decRefThis(env);
}

template<class Hook>
void suspendHook(IRGS& env, Hook hook) {
  ringbufferMsg(env, Trace::RBTypeFuncExit, curFunc(env)->fullName());
  ifThen(
    env,
    [&] (Block* taken) {
      gen(env, CheckSurpriseFlags, taken, anyStackRegister(env));
    },
    [&] {
      hint(env, Block::Hint::Unlikely);
      hook();
    }
  );
}

template <bool lowPri>
void implAwaitE(IRGS& env, SSATmp* child, Offset suspendOffset,
                Offset resumeOffset) {
  assertx(curFunc(env)->isAsync());
  assertx(!curFunc(env)->isEagerOnly());
  assertx(resumeMode(env) != ResumeMode::Async);
  // FIXME(T88328140): ifThenElse() emits unreachable code with bad state
  // assertx(spOffBCFromStackBase(env) == spOffEmpty(env));
  if constexpr (lowPri) {
    assertx(!child);
  } else {
    assertx(child && child->type() <= TObj);
  }

  // Bind address at which the execution should resume after awaiting.
  auto const func = curFunc(env);
  auto const suspendOff = cns(env, suspendOffset);
  auto const resumeAddr = [&]{
    auto const resumeSk = SrcKey(func, resumeOffset, ResumeMode::Async);
    auto const bindData = LdBindAddrData { resumeSk, spOffEmpty(env) + 1 };
    return gen(env, LdBindAddr, bindData);
  };

  if (!curFunc(env)->isGenerator()) {
    // Create the AsyncFunctionWaitHandle object. CreateAFWH takes care of
    // copying local variables and iterators. We don't support tracing when
    // we do the tail-call optimization, so we push the suspend hook here.
    auto const createNewAFWH = [&]{
      if (isInlining(env)) gen(env, StFrameFunc, FuncData { func }, fp(env));
      auto const wh = lowPri ?
        gen(env, CreateAFWHL, fp(env), cns(env, func->numSlotsInFrame()),
            resumeAddr(), suspendOff) :
        gen(env, CreateAFWH, fp(env), cns(env, func->numSlotsInFrame()),
            resumeAddr(), suspendOff, child);
      // Constructing a waithandle teleports locals and iterators to the heap,
      // kill them here to improve alias analysis.
      for (uint32_t i = 0; i < func->numLocals(); ++i) {
        gen(env, KillLoc, LocalId{i}, fp(env));
      }
      for (uint32_t i = 0; i < func->numIterators(); ++i) {
        gen(env, KillIter, IterId{i}, fp(env));
      }
      suspendHook(env, [&] {
        auto const asyncAR = gen(env, LdAFWHActRec, wh);
        gen(env, SuspendHookAwaitEF, fp(env), asyncAR, wh);
      });
      return wh;
    };

    // We don't need to create the new AFWH if we can do a tail-call check.
    auto const waitHandle = [&]{
      std::vector<Type> locals;
      if (Cfg::Eval::EnableArgsInBacktraces) return createNewAFWH();
      if (!isTailAwait(env, locals)) return createNewAFWH();

      // We can run out of tailFrameIds and fail to make this optimization.
      auto const tailFrameId = getAsyncFrameId(curSrcKey(env));
      if (tailFrameId == kInvalidAsyncFrameId) return createNewAFWH();
      auto const type = Type::ExactObj(c_AsyncFunctionWaitHandle::classof());
      return cond(env,
        [&](Block* taken) {
          gen(env, CheckSurpriseFlags, taken, anyStackRegister(env));
          gen(env, AFWHPushTailFrame, taken, child, cns(env, tailFrameId));
        },
        [&]{
          doTailAwaitDecRefs(env, locals);
          return gen(env, AssertType, type, child);
        },
        [&]{ return createNewAFWH(); }
      );
    }();

    if (isInlining(env)) {
      suspendFromInlined(env, waitHandle);
      return;
    }

    // Return control to the caller.
    auto const spAdjust = offsetToReturnSlot(env);
    auto const retData = RetCtrlData { spAdjust, false, AuxUnion{0} };
    gen(env, RetCtrl, retData, sp(env), fp(env), waitHandle);
  } else {
    assertx(!isInlining(env));

    // Create the AsyncGeneratorWaitHandle object.
    auto const waitHandle =
      gen(env, CreateAGWH, fp(env), resumeAddr(), suspendOff, child);

    // Call the suspend hook.
    suspendHook(env, [&] {
      gen(env, SuspendHookAwaitEG, fp(env), waitHandle);
    });

    // Return control to the caller (AG::next()).
    auto const spAdjust = offsetFromIRSP(env, BCSPRelOffset{-1});
    auto const retData = RetCtrlData { spAdjust, true, AuxUnion{0} };
    gen(env, RetCtrl, retData, sp(env), fp(env), waitHandle);
  }
}

void implAwaitR(IRGS& env, SSATmp* child, Offset suspendOffset,
                Offset resumeOffset) {
  assertx(curFunc(env)->isAsync());
  assertx(!curFunc(env)->isEagerOnly());
  assertx(resumeMode(env) == ResumeMode::Async);
  // FIXME(T88328140): ifThenElse() emits unreachable code with bad state
  // assertx(spOffBCFromStackBase(env) == spOffEmpty(env));
  assertx(child->isA(TObj));
  assertx(!isInlining(env));

  // We must do this before we do anything, because it can throw, and we can't
  // start tearing down the AFWH before that or the unwinder won't be able to
  // react.
  suspendHook(env, [&] {
    gen(env, SuspendHookAwaitR, fp(env), child);
  });

  // Prepare child for establishing dependency.
  gen(env, AFWHPrepareChild, fp(env), child);

  // Suspend the async function.
  auto const resumeSk = SrcKey(curFunc(env), resumeOffset, ResumeMode::Async);
  auto const bindData = LdBindAddrData { resumeSk, spOffEmpty(env) + 1 };
  auto const resumeAddr = gen(env, LdBindAddr, bindData);
  gen(env, StArResumeAddr, SuspendOffset { suspendOffset }, fp(env),
      resumeAddr);

  // Set up the dependency.
  gen(env, AFWHBlockOn, fp(env), child);

  // Call stub that will either transfer control to another ResumableWaitHandle,
  // or return control back to the scheduler. Leave SP pointing to a single
  // uninitialized cell which will be filled by the stub.
  auto const spAdjust = offsetFromIRSP(env, BCSPRelOffset{-1});
  gen(env, AsyncSwitchFast, IRSPRelOffsetData { spAdjust }, sp(env), fp(env));
}

SSATmp* implYieldGen(IRGS& env, SSATmp* key, SSATmp* value) {
  if (key != nullptr) {
    // Teleport yielded key.
    auto const oldKey = gen(env, LdContArKey, TInitCell, fp(env));
    gen(env, StContArKey, fp(env), key);
    decRef(env, oldKey, DecRefProfileId::ResumableOldKey);

    if (key->type() <= TInt) {
      gen(env, ContArUpdateIdx, fp(env), key);
    }
  } else {
    // Increment key.
    if (curFunc(env)->isPairGenerator()) {
      auto const newIdx = gen(env, ContArIncIdx, fp(env));
      auto const oldKey = gen(env, LdContArKey, TInitCell, fp(env));
      gen(env, StContArKey, fp(env), newIdx);
      decRef(env, oldKey, DecRefProfileId::ResumableOldKey);
    } else {
      // Fast path: if this generator has no yield k => v, it is
      // guaranteed that the key is an int.
      gen(env, ContArIncKey, fp(env));
    }
  }

  // Teleport yielded value.
  auto const oldValue = gen(env, LdContArValue, TInitCell, fp(env));
  gen(env, StContArValue, fp(env), value);
  decRef(env, oldValue, DecRefProfileId::ResumableOldValue);

  // Return value of iteration.
  return cns(env, TInitNull);
}

SSATmp* implYieldAGen(IRGS& env, SSATmp* key, SSATmp* value) {
  key = key ? key : cns(env, TInitNull);

  if (resumeMode(env) == ResumeMode::Async) {
    auto const spAdjust = offsetFromIRSP(env, BCSPRelOffset{-1});
    gen(env, AsyncGenYieldR, IRSPRelOffsetData { spAdjust }, sp(env), fp(env),
        key, value);
    return nullptr;
  }

  // Wrap the key and value into a tuple.
  auto const keyValueTuple = gen(env, AllocVec, VanillaVecData { 2 });
  gen(env, InitVecElem, IndexData { 0 }, keyValueTuple, key);
  gen(env, InitVecElem, IndexData { 1 }, keyValueTuple, value);

  // Wrap the tuple into a StaticWaitHandle.
  return gen(env, CreateSSWH, keyValueTuple);
}

void implYield(IRGS& env, bool withKey) {
  assertx(resumeMode(env) != ResumeMode::None);
  assertx(curFunc(env)->isGenerator());
  assertx(spOffBCFromStackBase(env) == spOffEmpty(env) + (withKey ? 2 : 1));
  assertx(!isInlining(env));

  suspendHook(env, [&] {
    gen(env, SuspendHookYield, fp(env));
  });

  // Resumable::setResumeAddr(resumeAddr, suspendOffset)
  auto const suspendOffset = bcOff(env);
  auto const resumeOffset = nextBcOff(env);
  auto const resumeSk = SrcKey(curFunc(env), resumeOffset, ResumeMode::GenIter);
  auto const bindData = LdBindAddrData { resumeSk, spOffEmpty(env) + 1 };
  auto const resumeAddr = gen(env, LdBindAddr, bindData);
  gen(env,
      StArResumeAddr,
      SuspendOffset { suspendOffset },
      fp(env),
      resumeAddr);

  // Set state from Running to Started.
  gen(env, StContArState,
      GeneratorState { BaseGenerator::State::Started },
      fp(env));

  // No inc/dec-ref as keys and values are teleported.
  auto const value = popC(env, DataTypeGeneric);
  auto const key = withKey ? popC(env) : nullptr;

  auto const retVal = !curFunc(env)->isAsync()
    ? implYieldGen(env, key, value)
    : implYieldAGen(env, key, value);

  // Return to the asio scheduler already handled.
  if (retVal == nullptr) return;

  // Return control to the caller (Gen::next()).
  assertx(resumeMode(env) == ResumeMode::GenIter);
  auto const spAdjust = offsetFromIRSP(env, BCSPRelOffset{-1});
  auto const retData = RetCtrlData { spAdjust, true, AuxUnion{0} };
  gen(env, RetCtrl, retData, sp(env), fp(env), retVal);
}

/*
 * HHBBC may have proven something about the inner type of this awaitable.
 *
 * So, we may have an assertion on the type of the top of the stack after
 * this instruction.  We know the next bytecode instruction is reachable from
 * fallthrough on the Await, so if it is an AssertRATStk 0, anything coming
 * out of the awaitable must be a subtype of that type, so this is a safe
 * and conservative way to do this optimization (even if our successor
 * bytecode offset is a jump target from things we aren't thinking about
 * here).
 */
Type awaitedTypeFromHHBBC(IRGS& env, SrcKey nextSk) {
  auto pc = nextSk.pc();
  if (decode_op(pc) != Op::AssertRATStk) return TInitCell;
  auto const stkLoc = decode_iva(pc);
  if (stkLoc != 0) return TInitCell;
  auto const rat = decodeRAT(curUnit(env), pc);
  return typeFromRAT(rat, curClass(env));
}

/*
 * Try to determine the inner awaitable type from the source of SSATmp.
 */
Type awaitedTypeFromSSATmp(const SSATmp* awaitable) {
  awaitable = canonical(awaitable);

  auto const inst = awaitable->inst();
  if (inst->is(Call)) {
    return (inst->src(2)->hasConstVal(TFunc) &&
            !inst->extra<Call>()->asyncEagerReturn)
      ? awaitedCallReturnType(inst->src(2)->funcVal(), true /* mayIntercept */)
      : TInitCell;
  }
  if (inst->is(CallFuncEntry)) {
    auto const extra = inst->extra<CallFuncEntry>();
    return (inst->src(2)->hasConstVal(TFunc) &&
            !extra->asyncEagerReturn())
      ? awaitedCallReturnType(inst->src(2)->funcVal(), true /* mayIntercept */)
      : TInitCell;
  }
  if (inst->is(CreateAFWH) || inst->is(CreateAFWHL)) {
    return awaitedCallReturnType(inst->func(), false /* mayIntercept */);
  }
  if (inst->is(DefLabel)) {
    auto ty = TBottom;
    auto const dsts = inst->dsts();
    inst->block()->forEachSrc(
      std::find(dsts.begin(), dsts.end(), awaitable) - dsts.begin(),
      [&] (const IRInstruction*, const SSATmp* src) {
        ty = ty | awaitedTypeFromSSATmp(src);
      }
    );
    return ty;
  }

  return TInitCell;
}

Type awaitedType(IRGS& env, SSATmp* awaitable, SrcKey nextSk) {
  return awaitedTypeFromHHBBC(env, nextSk) &
         awaitedTypeFromSSATmp(awaitable);
}

bool likelySuspended(const SSATmp* awaitable) {
  awaitable = canonical(awaitable);
  auto const inst = awaitable->inst();
  if (inst->is(Call) && inst->extra<Call>()->asyncEagerReturn) return true;
  if (inst->is(CallFuncEntry) &&
      inst->extra<CallFuncEntry>()->asyncEagerReturn()) {
    return true;
  }
  if (inst->is(CreateAFWH) || inst->is(CreateAFWHL)) return true;
  if (inst->is(DefLabel)) {
    auto likely = true;
    auto const dsts = inst->dsts();
    inst->block()->forEachSrc(
      std::find(dsts.begin(), dsts.end(), awaitable) - dsts.begin(),
      [&] (const IRInstruction*, const SSATmp* src) {
        likely = likely && likelySuspended(src);
      }
    );
    return likely;
  }

  return false;
}

void implAwaitSucceeded(IRGS& env, SSATmp* child) {
  auto const awaitedTy = awaitedType(env, child, nextSrcKey(env));
  auto const res = gen(env, LdWHResult, awaitedTy, child);
  popC(env);
  gen(env, IncRef, res);
  decRef(env, child);
  push(env, res);
}

void implAwaitFailed(IRGS& env, SSATmp* child, Block* exit) {
  auto const ty = Type::SubObj(SystemLib::getThrowableClass());
  auto const exc = gen(env, LdWHResult, ty, child);
  popC(env);
  gen(env, IncRef, exc);
  decRef(env, child);
  updateMarker(env);

  auto constexpr mode = EndCatchData::CatchMode::UnwindOnly;
  emitHandleException(env, mode, exc, std::nullopt, true /* sideEntry */);
}

template<class T>
void implAwait(IRGS& env, bool eagerOnly, T handleNotFinished) {
  // Side exit if not an Awaitable. In most conditions IsWaitHandle check
  // will be optimized out.
  auto const TAwaitable = Type::SubObj(c_Awaitable::classof());
  auto const maybeChild = topC(env);
  if (!maybeChild->isA(TObj)) return interpOne(env);
  if (!maybeChild->type().maybe(TAwaitable)) return interpOne(env);
  auto const exitSlow = makeExitSlow(env);
  gen(env, JmpZero, exitSlow, gen(env, IsWaitHandle, maybeChild));

  auto const childIsSWH =
    maybeChild->type() <= Type::SubObj(c_StaticWaitHandle::classof());
  auto const child = gen(env, AssertType, TAwaitable, maybeChild);

  auto const state = gen(env, LdWHState, child);
  assertx(c_Awaitable::STATE_SUCCEEDED == 0);
  assertx(c_Awaitable::STATE_FAILED == 1);
  if (eagerOnly && Cfg::HHIR::GenerateAsserts) {
    auto const finished = gen(env, LteInt, state, cns(env, 1));
    gen(env, JmpZero, makeUnreachable(env, ASSERT_REASON), finished);
  }

  if (childIsSWH || eagerOnly || !likelySuspended(child)) {
    ifThenElse(env,
      [&] (Block* taken) { gen(env, JmpNZero, taken, state); },
      [&] { implAwaitSucceeded(env, child); },
      [&] {
        if (childIsSWH || eagerOnly) {
          return implAwaitFailed(env, child, exitSlow);
        }
        ifThenElse(env,
          [&] (Block* taken) {
            gen(env, JmpZero, taken, gen(env, EqInt, state, cns(env, 1)));
          },
          [&] { implAwaitFailed(env, child, exitSlow); },
          [&] { handleNotFinished(child, exitSlow); }
        );
      }
    );
  } else {
    ifThenElse(env,
      [&] (Block* taken) {
        gen(env, JmpNZero, taken, gen(env, LteInt, state, cns(env, 1)));
      },
      [&] { handleNotFinished(child, exitSlow); },
      [&] {
        // Coming from a call with request for async eager return that did
        // not return eagerly.
        hint(env, Block::Hint::Unlikely);
        IRUnit::Hinter h(env.irb->unit(), Block::Hint::Unlikely);

        ifThenElse(env,
          [&] (Block* taken) { gen(env, JmpNZero, taken, state); },
          [&] {
            implAwaitSucceeded(env, child);
            gen(env, Jmp, makeExit(env, nextSrcKey(env)));
          },
          [&] { implAwaitFailed(env, child, exitSlow); }
        );
      }
    );
  }
}

//////////////////////////////////////////////////////////////////////

}

void emitWHResult(IRGS& env) {
  implAwait(env, false, [&] (SSATmp*, Block* exit) {
    gen(env, Jmp, exit);
  });
}

void emitAwait(IRGS& env) {
  assertx(curFunc(env)->isAsync());
  assertx(spOffBCFromStackBase(env) == spOffEmpty(env) + 1);

  implAwait(env, curFunc(env)->isEagerOnly(),
            [&] (SSATmp* child, Block* exit) {
    // Work in progress: fast path not supported yet
    if (curFunc(env)->isAsyncGenerator() &&
        resumeMode(env) == ResumeMode::Async) {
      gen(env, Jmp, exit);
      return;
    }

    popC(env);
    updateStackOffsetAndExceptionBoundary(env);

    if (resumeMode(env) == ResumeMode::Async) {
      implAwaitR(env, child, bcOff(env), nextBcOff(env));
    } else {
      implAwaitE<false>(env, child, bcOff(env), nextBcOff(env));
    }
            });
}

void emitAwaitAll(IRGS& env, LocalRange locals) {
  auto const suspendOffset = bcOff(env);
  auto const resumeOffset = nextBcOff(env);
  assertx(curFunc(env)->isAsync());
  assertx(spOffBCFromStackBase(env) == spOffEmpty(env));

  // Work in progress: fast path not supported yet
  auto const suspendInJitNotSupported =
    curFunc(env)->isAsyncGenerator() && resumeMode(env) == ResumeMode::Async;
  auto const exitSlow = makeExitSlow(env);

  auto const cnt = [&] {
    if (locals.count > Cfg::Jit::MaxAwaitAllUnroll) {
      return gen(
        env,
        CountWHNotDone,
        CountWHNotDoneData { locals.first, locals.count },
        exitSlow,
        fp(env)
      );
    }
    auto cnt = cns(env, 0);
    for (int i = 0; i < locals.count; ++i) {
      auto const loc = ldLoc(env, locals.first + i, DataTypeSpecific);
      if (loc->isA(TNull)) continue;
      if (!loc->isA(TObj)) PUNT(Await-NonObject);
      gen(env, JmpZero, exitSlow, gen(env, IsWaitHandle, loc));
      auto const notDone = gen(env, LdWHNotDone, loc);
      if (suspendInJitNotSupported) {
        gen(env, JmpNZero, exitSlow, notDone);
      } else {
        cnt = gen(env, AddInt, cnt, notDone);
      }
    }
    return cnt;
  }();

  if (curFunc(env)->isEagerOnly()) {
    assertx(!suspendInJitNotSupported);
    if (Cfg::HHIR::GenerateAsserts) {
      gen(env, JmpNZero, makeUnreachable(env, ASSERT_REASON), cnt);
    }
    push(env, cns(env, TInitNull));
    return;
  }

  if (suspendInJitNotSupported) {
    // Side-exit if CountWHNotDone was non-zero.
    gen(env, JmpNZero, exitSlow, cnt);
    push(env, cns(env, TInitNull));
    return;
  }

  ifThenElse(
    env,
    [&] (Block* taken) {
      gen(env, JmpNZero, taken, cnt);
    },
    [&] { // Next: all of the wait handles are finished
      push(env, cns(env, TInitNull));
    },
    [&] { // Taken: some of the wait handles have not yet completed
      hint(env, Block::Hint::Unlikely);
      IRUnit::Hinter h(env.irb->unit(), Block::Hint::Unlikely);

      auto const wh = gen(
        env,
        CreateCCWH,
        CreateCCWHData { locals.first, locals.count },
        fp(env),
        cnt
      );

      auto const state = gen(env, LdWHState, wh);
      ifThenElse(
        env,
        [&] (Block* taken) {
          gen(env, JmpNZero, taken, state);
        },
        [&] { // Extremely unlikely: profiling hook finished the CCWH.
          hint(env, Block::Hint::Unused);
          push(env, cns(env, TInitNull));
        },
        [&] {
          if (resumeMode(env) == ResumeMode::Async) {
            implAwaitR(env, wh, suspendOffset, resumeOffset);
          } else {
            implAwaitE<false>(env, wh, suspendOffset, resumeOffset);
          }
        }
      );
    }
  );
}

void emitAwaitLowPri(IRGS& env) {
  assertx(resumeMode(env) == ResumeMode::None);
  assertx(curFunc(env)->isAsyncFunction());
  assertx(spOffBCFromStackBase(env) == spOffEmpty(env));

  if (!Cfg::Eval::EnableLowPriorityAwaitables) {
    push(env, cns(env, TInitNull));
    return;
  }

  if (curFunc(env)->isEagerOnly()) {
    gen(env, Unreachable, ASSERT_REASON);
    return;
  }

  implAwaitE<true>(env, nullptr, bcOff(env), nextBcOff(env));
}

//////////////////////////////////////////////////////////////////////

void emitCreateCont(IRGS& env) {
  auto const suspendOffset = bcOff(env);
  auto const resumeOffset = nextBcOff(env);
  assertx(resumeMode(env) == ResumeMode::None);
  assertx(curFunc(env)->isGenerator());
  assertx(spOffBCFromStackBase(env) == spOffEmpty(env));
  assertx(!isInlining(env));

  // Create the Generator object. CreateCont takes care of copying local
  // variables and iterators.
  auto const func = curFunc(env);
  auto const resumeSk = SrcKey(func, resumeOffset, ResumeMode::GenIter);
  auto const bindData = LdBindAddrData { resumeSk, spOffEmpty(env) + 1 };
  auto const resumeAddr = gen(env, LdBindAddr, bindData);
  auto const cont =
    gen(env,
        curFunc(env)->isAsync() ? CreateAGen : CreateGen,
        fp(env),
        cns(env, func->numSlotsInFrame()),
        resumeAddr,
        cns(env, suspendOffset));

  // The suspend hook will decref the newly created generator if it throws.
  auto const contAR =
    gen(env,
        LdContActRec,
        IsAsyncData::fromEnvFunc(env),
        cont);

  suspendHook(env, [&] {
    gen(env, SuspendHookCreateCont, fp(env), contAR, cont);
  });

  // Grab caller info from the ActRec, free the ActRec, and return control to
  // the caller.
  auto const spAdjust = offsetToReturnSlot(env);
  auto const retData = RetCtrlData { spAdjust, false, AuxUnion{0} };
  gen(env, RetCtrl, retData, sp(env), fp(env), cont);
}

void emitContEnter(IRGS& env) {
  assertx(curClass(env));
  assertx(curClass(env)->classof(AsyncGenerator::classof()) ||
          curClass(env)->classof(Generator::classof()));

  auto const callBCOffset = bcOff(env);
  auto const isAsync = curClass(env)->classof(AsyncGenerator::classof());
  // Load generator's FP and resume address.
  auto const genObj = ldThis(env);
  auto const genFp  = gen(env, LdContActRec, IsAsyncData(isAsync), genObj);
  auto resumeAddr   = gen(env, LdContResumeAddr, IsAsyncData(isAsync), genObj);

  // Make sure function enter hook is called if needed.
  auto const exitSlow = makeExitSlow(env);
  gen(env, CheckSurpriseFlags, exitSlow, anyStackRegister(env));

  // Exit to interpreter if resume address is not known.
  resumeAddr = gen(env, CheckNonNull, exitSlow, resumeAddr);

  // Set state from Running to Started.
  auto const gs = GeneratorState { BaseGenerator::State::Running };
  gen(env, StContArState, gs, genFp);

  auto const sendVal = popC(env, DataTypeGeneric);
  updateStackOffsetAndExceptionBoundary(env);

  auto const retVal = gen(
    env,
    ContEnter,
    ContEnterData { spOffBCFromIRSP(env), callBCOffset, isAsync },
    sp(env),
    fp(env),
    genFp,
    resumeAddr,
    sendVal
  );

  push(env, retVal);
}

void emitYield(IRGS& env) {
  implYield(env, false);
}

void emitYieldK(IRGS& env) {
  implYield(env, true);
}

void emitContCheck(IRGS& env, ContCheckOp subop) {
  assertx(curClass(env));
  assertx(curClass(env)->classof(AsyncGenerator::classof()) ||
          curClass(env)->classof(Generator::classof()));
  auto const cont = ldThis(env);
  auto const state = gen(env, LdContState, IsAsyncData::fromEnvClass(env), cont);

  // These asserts make sure that the startedCheck work.
  static_assert(uint8_t(BaseGenerator::State::Created) == 0, "used below");
  static_assert(uint8_t(BaseGenerator::State::Started) == 1, "used below");

  auto const cmp = subop == ContCheckOp::CheckStarted
    ? gen(env, NeqInt, state, cns(env, BaseGenerator::State::Started))
    : gen(env, GtInt, state, cns(env, BaseGenerator::State::Started));
  gen(env, JmpNZero, makeExitSlow(env), cmp);
}

void emitContValid(IRGS& env) {
  assertx(curClass(env));
  assertx(curClass(env)->classof(AsyncGenerator::classof()) ||
          curClass(env)->classof(Generator::classof()));
  auto const cont = ldThis(env);
  auto const state = gen(env, LdContState, IsAsyncData::fromEnvClass(env), cont);
  push(env, gen(env, NeqInt, state, cns(env, BaseGenerator::State::Done)));
}

void emitContKey(IRGS& env) { PUNT(ContKey); }
void emitContRaise(IRGS& env) { PUNT(ContRaise); }
void emitContCurrent(IRGS& env) { PUNT(ContCurrent); }

//////////////////////////////////////////////////////////////////////

}
