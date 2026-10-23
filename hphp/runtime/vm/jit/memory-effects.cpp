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

#include "hphp/runtime/vm/jit/memory-effects.h"

#include "hphp/util/configs/debugger.h"
#include "hphp/util/match.h"
#include "hphp/util/safe-cast.h"
#include "hphp/util/assertions.h"

#include "hphp/runtime/base/implicit-context.h"

#include "hphp/runtime/vm/bytecode.h"
#include "hphp/runtime/vm/jit/analysis.h"
#include "hphp/runtime/vm/jit/ir-instruction.h"
#include "hphp/runtime/vm/jit/ssa-tmp.h"
#include "hphp/runtime/vm/jit/type-array-elem.h"

namespace HPHP::jit {

namespace {

const StaticString s_GLOBALS("GLOBALS");

uint32_t iterId(const IRInstruction& inst) {
  return inst.extra<IterId>()->iterId;
}

//////////////////////////////////////////////////////////////////////

AliasClass all_pointees(folly::Range<SSATmp**> srcs) {
  auto ret = AliasClass{AEmpty};
  for (auto const& src : srcs) {
    if (src->isA(TMem)) ret = ret | pointee(src);
  }
  return ret;
}

// Return an AliasClass containing all locations pointed to by any MemToCell
// sources to an instruction.
AliasClass all_pointees(const IRInstruction& inst) {
  return all_pointees(inst.srcs());
}

// Return an AliasClass representing a range of the eval stack that contains
// everything below a logical depth.
AliasClass stack_below(IRSPRelOffset offset) {
  return AStack::below(offset);
}

//////////////////////////////////////////////////////////////////////

// Return an AliasClass representing an entire ActRec at base + offset.
AliasClass actrec(SSATmp* base, IRSPRelOffset offset) {
  return AStack::range(offset, offset + int32_t{kNumActRecCells});
}

/*
 * AliasClass that can be used to represent effects on liveFrame().
 */
AliasClass livefp(SSATmp* fp) {
  return AFBasePtr | AActRec { fp };
}

AliasClass livefp(const IRInstruction& inst) {
  return livefp(inst.marker().fixupFP());
}

AliasClass frame_locals(const Func* func, SSATmp* fp) {
  auto const numLocals = func->numLocals();
  return numLocals ? ALocal { fp, AliasIdSet::IdRange(0, numLocals) } : AEmpty;
}

AliasClass frame_iterators(const Func* func, SSATmp* fp) {
  auto const numIterators = func->numIterators();
  return numIterators ? aiter_range(fp, 0, numIterators) : AEmpty;
}

//////////////////////////////////////////////////////////////////////

// Determine an AliasClass representing any locals in the instruction's frame
// which might be accessed via debug_backtrace().

std::pair<const Func*, uint32_t> func_and_depth_from_fp(SSATmp* fp) {
  if (!fp) return {nullptr, 0};
  return {funcFromFP(fp), frameDepthIndex(fp)};
}

jit::vector<AliasClass> backtrace_locals(const IRInstruction& inst) {
  auto eachFunc = [&] (auto fn) {
    std::vector<AliasClass> acs;
    for (auto fp = inst.marker().fp(); fp; ) {
      auto const [func, depth] = func_and_depth_from_fp(fp);
      auto ac = fn(fp, func, depth);
      if (ac != AEmpty) acs.push_back(std::move(ac));
      if (fp->inst()->is(DefCalleeFP)) {
        // Walking the marker fp chain in this manner is suspect, but here we
        // are careful to only materialize func, depth pair using it.
        fp = fp->inst()->marker().fp();
      } else {
        fp = nullptr;
      }
    }
    return acs;
  };

  auto const addInspectable =
    [&] (SSATmp*, const Func* func, uint32_t depth) -> AliasClass {
      auto const meta = func->lookupVarId(s_86metadata.get());
      auto const productId = func->lookupVarId(s_86productAttributionData.get());
      const AliasClass coeffect = func->hasCoeffectsLocal()
        ? ALocal { depth, func->coeffectsLocalId() }
        : AEmpty;
      auto result = coeffect;
      if (meta != kInvalidId) result |= ALocal { depth, (uint32_t)meta };
      if (productId != kInvalidId) result |= ALocal { depth, (uint32_t)productId };
      return result;
    };

  // Either there's no func or no frame-pointer. Either way, be conservative and
  // assume anything can be read. This can happen in test code, for instance.
  if (!inst.marker().fp()) return { ALocalAny };

  if (!Cfg::Eval::EnableArgsInBacktraces) return eachFunc(addInspectable);

  return eachFunc([&] (SSATmp* fp, const Func* func, uint32_t depth) {
    auto ac = addInspectable(fp, func, depth);
    // Normally only function parameters need to be sync'ed at the call-site
    // when EnableArgsInBacktraces is true. However, if debugging is enabled,
    // all named locals need to be sync'ed.
    auto const numLocals =
      Cfg::Debugger::EnableVSDebugger &&
      Cfg::Eval::EmitDebuggerIntrCheck ?
      func->numNamedLocals() : func->numParams();

    if (func->hasReifiedGenerics()) {
      ac |= ALocal { depth, func->reifiedGenericsLocalId() };
    }

    if (func->cls() && func->cls()->hasReifiedGenerics()) {
      // There is no way to access the SSATmp for ObjectData of `this` here,
      // so be very pessimistic
      ac |= APropAny;
    }

    if (numLocals) {
      AliasIdSet locals { AliasIdSet::IdRange { 0, numLocals } };
      ac |= ALocal { depth, locals };
    }

    // $this
    if (func->cls() && !func->isClosureBody() && !func->isStatic()) {
      ac |= AFContext { fp };
    }

    return ac;
  });
}

/////////////////////////////////////////////////////////////////////

/*
 * Modify a GeneralEffects to take potential VM re-entry into account.  This
 * affects may-load, may-store, and kills information for the instruction.  The
 * GeneralEffects should already contain AHeapAny in both loads and stores if
 * it affects those locations for reasons other than re-entry, but does not
 * need to if it doesn't.
 *
 * For loads, we need to take into account any locals potentially accessed by
 * debug_backtrace().
 *
 * For kills, locations on the eval stack below the re-entry depth should all
 * be added.
 */
GeneralEffects may_reenter(const IRInstruction& inst, const GeneralEffects& x) {
  auto const may_reenter_is_ok =
    inst.taken() && inst.taken()->isCatch();
  always_assert_flog(
    may_reenter_is_ok,
    "instruction {} claimed may_reenter, but it isn't allowed to say that",
    inst
  );

  /*
   * We want to union `killed_stack' into whatever else the instruction already
   * said it must kill, but if we end up with an unrepresentable AliasClass we
   * can't return a set that's too big (the `kills' set is unlike the other
   * AliasClasses in GeneralEffects in that means it kills /everything/ in the
   * set, since it's must-information).
   *
   * If we can't represent the union, just take the stack, in part because we
   * have some debugging asserts about this right now---but also nothing
   * actually uses may_reenter with a non-AEmpty kills at the time of this
   * writing anyway.
   */
  auto const new_kills = [&] {
    if (inst.marker().fp() == nullptr) return AEmpty;

    auto const offset = [&]() -> IRSPRelOffset {
      auto const fp = canonical(inst.marker().fp());
      if (fp->inst()->is(DefCalleeFP)) {
        auto const extra = fp->inst()->extra<DefCalleeFP>();
        auto const fpOffset = extra->spOffset;
        auto const numSlotsInFrame = extra->func->numSlotsInFrame();
        auto const numStackElems = inst.marker().bcSPOff() - SBInvOffset{0};
        return fpOffset - numSlotsInFrame - numStackElems;
      }

      assertx(fp->inst()->is(DefFP, DefFuncEntryFP, EnterFrame));
      auto const sp = inst.marker().sp();
      auto const irSPOff = sp->inst()->extra<DefStackData>()->irSPOff;
      return inst.marker().bcSPOff().to<IRSPRelOffset>(irSPOff);
    }();

    auto const killed_stack = stack_below(offset);
    auto const kills_union = x.kills.precise_union(killed_stack);
    return kills_union ? *kills_union : killed_stack;
  }();

  return GeneralEffects {
    x.loads | AHeapAny | ARdsAny | AVMRegAny | AVMRegState,
    x.stores | AHeapAny | ARdsAny | AVMRegAny,
    x.moves,
    new_kills,
    x.inout,
    backtrace_locals(inst)
  };
}

//////////////////////////////////////////////////////////////////////

GeneralEffects may_load_store(AliasClass loads, AliasClass stores) {
  return GeneralEffects { loads, stores, AEmpty, AEmpty, AEmpty, {} };
}

GeneralEffects may_load_store_kill(AliasClass loads,
                                   AliasClass stores,
                                   AliasClass kill) {
  return GeneralEffects { loads, stores, AEmpty, kill, AEmpty, {} };
}

GeneralEffects may_load_store_move(AliasClass loads,
                                   AliasClass stores,
                                   AliasClass move) {
  assertx(move <= loads);
  return GeneralEffects { loads, stores, move, AEmpty, AEmpty, {} };
}

GeneralEffects may_load_store_move_kill(AliasClass loads,
                                        AliasClass stores,
                                        AliasClass move,
                                        AliasClass kill) {
  assertx(move <= loads);
  return GeneralEffects { loads, stores, move, kill, AEmpty, {} };
}

//////////////////////////////////////////////////////////////////////

/*
 * Construct effects for InterpOne, using the information in its extra data.
 *
 * We always consider an InterpOne as potentially doing anything to the heap,
 * potentially re-entering, potentially raising warnings in the current frame,
 * potentially reading any locals, and potentially reading/writing any stack
 * location that isn't below the bottom of the stack.
 *
 * The extra data for the InterpOne comes with some additional information
 * about which local(s) it may modify, which is all we try to be more precise
 * about right now.
 */
GeneralEffects interp_one_effects(const IRInstruction& inst) {
  auto const extra  = inst.extra<InterpOne>();
  auto loads  = AHeapAny | AStackAny | ALocalAny | ARdsAny | livefp(inst);
  auto stores = AHeapAny | AStackAny | ARdsAny | AVMRegAny | AVMRegState;
  if (extra->smashesAllLocals) {
    stores = stores | ALocalAny;
  } else {
    for (auto i = uint32_t{0}; i < extra->nChangedLocals; ++i) {
      stores = stores | ALocal { inst.src(1), extra->changedLocals[i].id };
    }
  }

  auto kills = AEmpty;
  if (isMemberBaseOp(extra->opcode)) {
    stores = stores | AMIStateAny;
    kills = kills | AMIStateAny;
  } else if (isMemberDimOp(extra->opcode) || isMemberFinalOp(extra->opcode)) {
    stores = stores | AMIStateAny;
    loads = loads | AMIStateAny;
  } else if (!isTypeAssert(extra->opcode)) {
    kills = kills | AMIStateAny;
  }

  return may_load_store_kill(loads, stores, kills);
}

////////////////////////////////////////////////////////////////////////////////

/*
 * Construct effects for member instructions that take &tvRef as their last
 * argument.
 *
 * These instructions never load tvRef or roProp, but they might store to it.
 */
MemEffects minstr_with_tvref(const IRInstruction& inst) {
  auto const tvRef = inst.src(2);
  assertx(tvRef->isA(TMemToMISTemp) || tvRef->isA(TNullptr));
  auto stores = AHeapAny | AMIStateROProp;
  if (!tvRef->isA(TNullptr)) stores |= pointee(tvRef);
  return may_load_store(AHeapAny, stores);
}

//////////////////////////////////////////////////////////////////////

MemEffects memory_effects_impl(const IRInstruction& inst) {
  switch (inst.op()) {

  //////////////////////////////////////////////////////////////////////
  // Region exits

  // These exits don't leave the current php function, and could head to code
  // that could read or write anything as far as we know (including frame
  // locals).
  case ReqBindJmp: {
    auto const uninitArgs = [&]() -> AliasClass {
      auto const extra = inst.extra<ReqBindJmp>();
      if (!extra->target.funcEntry()) {
        return AEmpty;
      }
      auto const func = extra->target.func();
      // No kills for the main func entry.
      if (extra->target.funcEntry() &&
          extra->target.numEntryArgs() == func->numPositionalParams()) {
        return AEmpty;
      }
      auto const numParams = func->numNonVariadicParams();
      auto const totalEntryArgs = extra->target.numEntryArgs() + func->numNamedParams();
      assertx(totalEntryArgs <= numParams);
      // Kill TUninit positionals passed by prologue.
      auto killedPosArgs = totalEntryArgs == numParams ? AEmpty : AStack::range(
        extra->irSPOff + func->numFuncEntryInputs() - numParams,
        extra->irSPOff + func->numFuncEntryInputs() - totalEntryArgs
      );
      return killedPosArgs;
    }();

    return ExitEffects {
      *AUnknown.exclude_vm_reg(),
      stack_below(inst.extra<ReqBindJmp>()->irSPOff),
      uninitArgs
    };
  }
  case ReqInterpBBNoTranslate:
    return ExitEffects {
      *AUnknown.exclude_vm_reg(),
      stack_below(inst.extra<ReqInterpBBNoTranslate>()->irSPOff),
      AEmpty
    };
  case ReqRetranslate:
    return ExitEffects {
      *AUnknown.exclude_vm_reg(),
      stack_below(inst.extra<ReqRetranslate>()->offset),
      AEmpty
    };
  case ReqRetranslateOpt:
    return ExitEffects {
      *AUnknown.exclude_vm_reg(),
      stack_below(inst.extra<ReqRetranslateOpt>()->offset),
      AEmpty
    };
  case JmpExit:
    return ExitEffects {
      *AUnknown.exclude_vm_reg(),
      stack_below(inst.extra<JmpExit>()->offset),
      AEmpty
    };

  //////////////////////////////////////////////////////////////////////
  // Unusual instructions

  /*
   * The ReturnHook sets up the ActRec so the unwinder knows everything is
   * already released (i.e. it calls ar->setLocalsDecRefd()).
   *
   * The eval stack is also dead at this point (the return value is passed to
   * ReturnHook as src(1), and the ReturnHook may not access the stack).
   */
  case ReturnHook:
    return may_load_store_kill(
      AHeapAny | AFFunc { inst.src(0) } | AFMeta { inst.src(0) },
      AHeapAny,
      *AStackAny
        .precise_union(ALocalAny)
        ->precise_union(AMIStateAny)
        ->precise_union(AFContext { inst.src(0) })
    );

  // The suspend hooks can load anything (re-entering the VM), but can't write
  // to frame locals.
  case SuspendHookAwaitEF:
  case SuspendHookAwaitEG:
  case SuspendHookAwaitR:
  case SuspendHookCreateCont:
  case SuspendHookYield:
    // We rely on the may_reenter effects to add the appropriate local asets.
    return may_load_store_kill(
      AHeapAny | AActRec {inst.src(0)}, AHeapAny, AMIStateAny);

  /*
   * If we're returning from a function, it's ReturnEffects.  The RetCtrl
   * opcode also suspends resumables, which we model as having any possible
   * effects.
   */
  case RetCtrl:
    if (inst.extra<RetCtrl>()->suspendingResumed) {
      // Suspending can go anywhere, and doesn't even kill locals.
      return UnknownEffects {};
    }
    return ReturnEffects {
      AStackAny | ALocalAny | AMIStateAny | AFBasePtr | AFContextAny | AFFuncAny
    };

  case AsyncFuncRetPrefetch:
    return IrrelevantEffects {};

  case AsyncFuncRet:
  case AsyncFuncRetSlow:
  case AsyncGenRetR:
    return ReturnEffects { AStackAny | AMIStateAny | livefp(inst.src(1)) };

  case AsyncGenYieldR:
  case AsyncSwitchFast:
    // Suspending can go anywhere, and doesn't even kill locals.
    return UnknownEffects {};

  case GenericRetDecRefs:
    /*
     * The may-store information here is ALocalAny: even though we
     * know it doesn't really "store" to the frame locals, the values
     * that used to be there are no longer available because they are
     * DecRef'd, which we are required to report as may-store
     * information to make it visible to reference count
     * optimizations.  It's conceptually the same as if it was storing
     * an Uninit over each of the locals, but the stores of uninits
     * would be dead so we're not actually doing that.
     */
    return may_load_store_kill(
      ALocalAny | AHeapAny,
      ALocalAny | AHeapAny,
      AMIStateAny
    );

  case EndCatch: {
    auto const data = inst.extra<EndCatch>();
    auto const stack_kills = stack_below(data->offset);

    auto const numLocals = inst.func()->numLocals();
    auto const local_kills = [&]() {
      if ((data->teardown == EndCatchData::Teardown::OnlyThis
          || data->teardown == EndCatchData::Teardown::None)
          && numLocals > 0
      ) {
        return static_cast<AliasClass>(
            ALocal {inst.src(0), AliasIdSet::IdRange(0, numLocals)}
        );
      }
      return AEmpty;
    }();
    auto const ctx = data->teardown == EndCatchData::Teardown::None
      ? AFContext{inst.src(0)}
      : AEmpty;

    return ExitEffects {
      AUnknown,
      stack_kills,
      *local_kills.precise_union(ctx)
    };
  }

  /*
   * DefCalleeFP must always be the first instruction in the inlined call. It
   * defines a new FP for the callee but does not perform any stores or
   * otherwise initialize the FP.
   */
  case DefCalleeFP:
    return IrrelevantEffects {};

  /*
   * EnterInlineFrame marks that an inline frame pointer allocated using
   * DefCalleeFP is now live. The frame pointer should not be accessed
   * prior to this instruction as it may alias locations that overlap with
   * live stack locations in the caller.
   */
  case EnterInlineFrame: {
    /*
     * SP relative offset of the firstin the inlined call.
     */
    auto inlineStackOff =
      inst.src(0)->inst()->extra<DefCalleeFP>()->spOffset + kNumActRecCells;
    return may_load_store_kill(
      AEmpty,
      AEmpty,
      /*
       * This prevents stack slots from the caller from being sunk into the
       * callee. Note that some of these stack slots overlap with the frame
       * locals of the callee-- those slots are inacessible in the inlined
       * call as frame and stack locations may not alias.
       */
      stack_below(inlineStackOff)
    );
  }

  case LeaveInlineFrame: {
    assertx(inst.src(0)->inst()->is(DefCalleeFP));
    auto const fp = inst.src(0);
    auto const callee = fp->inst()->extra<DefCalleeFP>()->func;
    const AliasClass ar = AActRec { fp };
    auto const locals = [&] () -> AliasClass {
      if (!callee->numLocals()) return AEmpty;
      return ALocal {fp, AliasIdSet::IdRange(0, callee->numLocals())};
    }();

    // Ensure the kill-set is precise- it represents *must*-kill locations
    //
    // NB: It's okay if the AliasIdSet for locals cannot be precise. We want to
    //     kill *every* local in the frame so there's nothing else that can
    //     accidentally be included in the set.
    auto const kills = *ar.precise_union(locals)->precise_union(AMIStateAny);
    return may_load_store_kill(AEmpty, AEmpty, kills);
  }

  case InlineCall:
    return PureInlineCall {
      AFBasePtr,
      inst.src(0),

      // Right now when we "publish" a frame by storing it in rvmfp() we
      // implicitly depend on the AFFunc and AFMeta bits being stored. In the
      // future we may want to track this explicitly.
      //
      // We also need to ensure that all of our parent frames have this stored
      // this information. To achieve this we also register a load on AFBasePtr,
      // forcing them to also be published. Notice that we don't actually
      // depend on this load to properly initialize m_sfp or rvmfp().
      AliasClass(AFFunc { inst.src(0) }) | AFMeta { inst.src(0) } | AFBasePtr
    };

  case InlineSideExit: {
    auto const callee = inst.extra<InlineSideExit>()->callee;
    auto const arOffset = inst.src(1)->inst()->extra<DefCalleeFP>()->spOffset;
    return CallEffects {
      // Kills. Stack was already handled by InlineSideExitSyncStack.
      AVMRegAny,
      // No uninits to kill.
      AEmpty,
      // Input state.
      frame_locals(callee, inst.src(1)) | frame_iterators(callee, inst.src(1)) |
        AActRec { inst.src(1) } | AMIStateAny,
      // No empty ActRec space to kill.
      AEmpty,
      // Inout outputs.
      callee->numInOutParams() == 0 ? AEmpty : AStack::range(
        arOffset + kNumActRecCells,
        arOffset + kNumActRecCells + callee->numInOutParams()
      ),
      // Backtrace locals.
      backtrace_locals(inst)
    };
  }

  case InlineSideExitSyncStack: {
    auto const extra = inst.extra<InlineSideExitSyncStack>();
    auto const start = extra->start;
    auto const count = extra->count;
    return may_load_store_move_kill(
      count == 0 ? AEmpty : AStack::range(start, start + count),
      AEmpty,
      count == 0 ? AEmpty : AStack::range(start, start + count),
      AStack::below(start)
    );
  }

  case InterpOne:
    return interp_one_effects(inst);
  case InterpOneCF: {
    auto const extra = inst.extra<InterpOneData>();
    return ExitEffects {
      *AUnknown.exclude_vm_reg(),
      stack_below(extra->spOffset) | AMIStateAny,
      AEmpty
    };
  }

  case NativeImpl:
    return UnknownEffects {};

  // This opcode will read all passed args and may push up to
  // len(optional named param) extra typed values on the VM stack, which
  // also involves moving existing positional args.
  case CheckFunNamedArgsMismatch: {
    auto const extra = inst.extra<CheckFunNamedArgsMismatch>();
    auto const func = extra->callee;
    auto const reified = func->hasReifiedGenerics() ? 1 : 0;
    auto const last = static_cast<int32_t>(extra->lastArgOffset);
    auto const named = static_cast<int32_t>(func->numNamedParams());
    auto const posArgc = static_cast<int32_t>(extra->posArgc);
    auto const stackRange = AStack::range(
      IRSPRelOffset { last - named },
      IRSPRelOffset { last + reified + posArgc }
    );
    return may_load_store(AHeapAny | stackRange, AHeapAny | stackRange);
  }

  case PushEmptyReifiedGenericsWithNamedArgs: {
   auto const extra = inst.extra<GenericsWithNamedArgsData>();
   auto startOffset = IRSPRelOffset{static_cast<int32_t>(extra->lastArgOffset) + 1};
   if (extra->callee->numNamedParams() > 0) {
      auto const writeRange = AStack::range(
        startOffset,
        startOffset + extra->callee->numNamedParams()
      );
      return may_load_store(AHeapAny, AHeapAny | writeRange);
    }
    return may_load_store(AHeapAny, AHeapAny);
  }

  // These C++ helpers can invoke the user error handler and go do whatever
  // they want to non-frame locations.
  case VerifyParam:
  case VerifyParamCallable:
  case VerifyParamCls:
  case VerifyParamCoerce:
  case VerifyParamFail:
  case VerifyParamFailHard:
  case VerifyProp:
  case VerifyPropAll:
  case VerifyPropCls:
  case VerifyPropCoerce:
  case VerifyPropCoerceAll:
  case VerifyPropFail:
  case VerifyPropFailHard:
  case VerifyReifiedLocalType:
  case VerifyReifiedReturnType:
  case VerifyType:
  case VerifyRet:
  case VerifyRetCallable:
  case VerifyRetCls:
  case VerifyRetCoerce:
  case VerifyRetFail:
  case VerifyRetFailHard:
    return may_load_store(AHeapAny, AHeapAny);

  case ContEnter:
    {
      auto const extra = inst.extra<ContEnter>();
      return CallEffects {
        // Kills. Everything on the stack.
        stack_below(extra->spOffset) | AMIStateAny | AVMRegAny,
        // No input uninits.
        AEmpty,
        // No inputs. The value being sent is passed explicitly.
        AEmpty,
        // ActRec. It is on the heap and we already implicitly assume that
        // CallEffects can perform arbitrary heap operations.
        AEmpty,
        // No outputs.
        AEmpty,
        // Backtrace locals.
        backtrace_locals(inst)
      };
    }

  case Call:
    {
      auto const extra = inst.extra<Call>();
      return CallEffects {
        // Kills. Everything on the stack below the incoming parameters.
        stack_below(extra->spOffset) | AMIStateAny | AVMRegAny,
        // No input uninits when calling a prologue.
        AEmpty,
        // Input arguments.
        extra->numInputs() == 0 ? AEmpty : AStack::range(
          extra->spOffset,
          extra->spOffset + extra->numInputs()
        ),
        // ActRec.
        actrec(inst.src(0), extra->spOffset + extra->numInputs()),
        // Inout outputs.
        extra->numOut == 0 ? AEmpty : AStack::range(
          extra->spOffset + extra->numInputs() + kNumActRecCells,
          extra->spOffset + extra->numInputs() + kNumActRecCells +
            extra->numOut
        ),
        // Backtrace locals.
        backtrace_locals(inst)
      };
    }

  case CallFuncEntry:
    {
      auto const extra = inst.extra<CallFuncEntry>();
      auto const callee = extra->calleePrototype;
      auto const numPosParams = callee->numPositionalParams();

      return CallEffects {
        // Kills. Everything on the stack below the incoming parameters.
        stack_below(extra->spOffset) | AMIStateAny | AVMRegAny,
        // Kill TUninit positionals where we pushed uninits.
        extra->numInitPositionals >= numPosParams ? AEmpty : AStack::range(
          extra->spOffset
            + callee->numFuncEntryInputs() - callee->numNamedParams()
            - numPosParams,
          extra->spOffset
            + callee->numFuncEntryInputs() - callee->numNamedParams()
            - extra->numInitPositionals
        ),
        // Input arguments.
        callee->numFuncEntryInputs() == 0 ? AEmpty : AStack::range(
          extra->spOffset,
          extra->spOffset + callee->numFuncEntryInputs()
        ),
        // ActRec.
        actrec(inst.src(0), extra->spOffset + callee->numFuncEntryInputs()),
        // Inout outputs.
        callee->numInOutParams() == 0 ? AEmpty : AStack::range(
          extra->spOffset + callee->numFuncEntryInputs() + kNumActRecCells,
          extra->spOffset + callee->numFuncEntryInputs() + kNumActRecCells +
          callee->numInOutParams()
        ),
        // Backtrace locals.
        backtrace_locals(inst)
      };
    }

  case CallBuiltin:
    {
      auto const extra = inst.extra<CallBuiltin>();
      auto const callee = extra->callee;
      auto const [inout, read]  = [&] {
        auto read = AEmpty;
        auto inout = AEmpty;
        auto const paramOff = callee->isMethod() ? 3 : 2;
        for (auto i = paramOff; i < inst.numSrcs(); ++i) {
          if (inst.src(i)->type() <= TPtr) {
            auto const cls = pointee(inst.src(i));
            if (callee->isInOut(i - paramOff)) {
              inout = inout | cls;
            } else {
              read = read | cls;
            }
          }
        }
        return std::make_pair(inout, read);
      }();
      auto const foldable = callee->isFoldable() ? AEmpty : ARdsAny;
      return GeneralEffects {
        read | AHeapAny | foldable | AVMRegAny | AVMRegState,
        AHeapAny | foldable | AVMRegAny,
        AEmpty,
        stack_below(extra->spOffset) | AMIStateAny,
        inout,
        {}
      };
    }

  // Resumable suspension takes everything from the frame and moves it into the
  // heap.
  case CreateGen:
  case CreateAGen:
  case CreateAFWH:
  case CreateAFWHL: {
    auto const fp = canonical(inst.src(0));
    auto fpInst = fp->inst();
    auto const frame = [&] () -> AliasClass {
      if (fpInst->is(DefFP, EnterFrame)) return ALocalAny | AIterAny;
      assertx(fpInst->is(DefCalleeFP));
      auto const func = fpInst->extra<DefCalleeFP>()->func;
      return frame_locals(func, fp) | frame_iterators(func, fp);
    }();
    return may_load_store_move(
      frame | AActRec { fp },
      AHeapAny,
      frame
    );
  }

  // AGWH construction updates the AsyncGenerator object.
  case CreateAGWH:
    return may_load_store(AHeapAny | AActRec { inst.src(0) }, AHeapAny);

  case CreateCCWH:
    {
      auto const extra = inst.extra<CreateCCWH>();
      auto const frame = ALocal {
        inst.src(0),
        AliasIdSet {
          AliasIdSet::IdRange{ extra->first, extra->first + extra->count }
        }
      };
      return may_load_store(frame, AHeapAny);
    }

  case CountWHNotDone:
    {
      auto const extra = inst.extra<CountWHNotDone>();
      auto const frame = ALocal {
        inst.src(0),
        AliasIdSet {
          AliasIdSet::IdRange{ extra->first, extra->first + extra->count }
        }
      };
      return may_load_store(frame, AEmpty);
    }

  // This re-enters to call extension-defined instance constructors.
  case ConstructInstance:
    return may_load_store(AHeapAny, AHeapAny);

  // Closures don't ever throw or reenter on construction
  case ConstructClosure:
    return IrrelevantEffects{};

  case CheckStackOverflow:
    return may_load_store(AEmpty, AEmpty);

  case HandleSurpriseEnter: {
    // Function call event hook inspects parameters.
    auto const fp = inst.src(0);
    auto const callee = inst.extra<HandleSurpriseEnter>()->func;
    auto const params = callee->numParams() > 0
      ? ALocal { fp, AliasIdSet::IdRange(0, callee->numParams()) }
      : AEmpty;
    return may_load_store(params | AActRec { fp }, AEmpty);
  }

  //////////////////////////////////////////////////////////////////////
  // Iterator instructions

  case IterExtractBase:
    return may_load_store(AEmpty, AEmpty);

  case IterGetKeyArr:
  case IterGetValArr:
    return PureLoad { AElemAny };

  case IterInitArr:
  case IterInitObj:
  case IterNextArr:
  case IterNextObj: {
    auto const fp = inst.src(1);
    auto const iters =
      AliasClass { aiter_all(fp, inst.extra<IterData>()->args.iterId) };
    return may_load_store_kill(
      iters | AHeapAny,
      iters | AHeapAny,
      AMIStateAny
    );
  }

  case LdIterPos:
    return PureLoad { aiter_pos(inst.src(0), iterId(inst)) };

  case LdIterEnd:
    return PureLoad { aiter_end(inst.src(0), iterId(inst)) };

  case StIterPos:
    return PureStore { aiter_pos(inst.src(0), iterId(inst)), inst.src(1) };

  case StIterEnd:
    return PureStore { aiter_end(inst.src(0), iterId(inst)), inst.src(1) };

  case KillActRec:
    return may_load_store_kill(AEmpty, AEmpty, AActRec { inst.src(0) });

  case KillLoc: {
    auto const local = inst.extra<LocalId>()->locId;
    return may_load_store_kill(AEmpty, AEmpty, ALocal { inst.src(0), local });
  }

  case KillIter: {
    auto const iters = aiter_all(inst.src(0), iterId(inst));
    return may_load_store_kill(AEmpty, AEmpty, iters);
  }

  //////////////////////////////////////////////////////////////////////
  // Instructions that explicitly manipulate locals

  case StLoc:
  case StLocMeta:
    return PureStore {
      ALocal { inst.src(0), inst.extra<LocalId>()->locId },
      inst.src(1),
      nullptr
    };

  case StLocRange:
    {
      auto const extra = inst.extra<StLocRange>();
      auto acls = AEmpty;

      for (auto locId = extra->start; locId < extra->end; ++locId) {
        acls = acls | ALocal { inst.src(0), locId };
      }
      return PureStore { acls, inst.src(1), nullptr };
    }

  case LdLoc:
    return PureLoad { ALocal { inst.src(0), inst.extra<LocalId>()->locId } };

  case LdLocForeign:
    return may_load_store(ALocalAny, AEmpty);

  //////////////////////////////////////////////////////////////////////
  // Pointer-based loads and stores

  case LdMem:
    return PureLoad { pointee(inst.src(0)) };
  case StMem:
  case StMemMeta:
    return PureStore { pointee(inst.src(0)), inst.src(1), inst.src(0) };

  case LdClsInitElem:
    return PureLoad { AHeapAny };

  case StClsInitElem:
    return PureStore { AHeapAny };

  case LdPairElem:
    return PureLoad { AHeapAny };

  case LdMBase:
    return PureLoad { AMIStateBase };

  case StMBase:
    return PureStore { AMIStateBase, inst.src(0), nullptr };

  case StMROProp:
    return PureStore { AMIStateROProp, inst.src(0), nullptr };

  case CheckMROProp:
    return may_load_store(AMIStateROProp, AEmpty);

  case FinishMemberOp:
    return may_load_store_kill(AEmpty, AEmpty, AMIStateAny);

  case CheckRDSInitialized:
    return may_load_store(
      ARds { inst.extra<CheckRDSInitialized>()->handle },
      AEmpty
    );
  case MarkRDSInitialized:
    return may_load_store(
      AEmpty,
      ARds { inst.extra<MarkRDSInitialized>()->handle }
    );
  case MarkRDSAccess:
    return IrrelevantEffects{};
  // LdTVFromRDS and StTVInRDS load/store aux bit, so they cannot be
  // PureLoad/PureStore -- load/store elim do not track aux bit accesses.
  case LdTVFromRDS:
    return may_load_store(
      ARds { inst.extra<LdTVFromRDS>()->handle },
      AEmpty
    );
  case StTVInRDS:
    return may_load_store(
      AEmpty,
      ARds { inst.extra<StTVInRDS>()->handle }
    );

  case InitProps:
    return may_load_store(
      AHeapAny,
      AHeapAny | ARds { inst.extra<InitProps>()->cls->propHandle() }
    );

  case InitSProps:
    return may_load_store(
      AHeapAny,
      AHeapAny | ARds { inst.extra<InitSProps>()->cls->sPropInitHandle() }
    );

  case LdARFunc:
  case LdClsFromClsMeth:
  case LdFuncFromClsMeth:
  case LdFuncFromRFunc:
  case LdGenericsFromRFunc:
  case LdClsFromRClsMeth:
  case LdFuncFromRClsMeth:
  case LdGenericsFromRClsMeth:
    return may_load_store(AEmpty, AEmpty);

  //////////////////////////////////////////////////////////////////////
  // Object/Ref loads/stores

  case InitObjProps:
    return may_load_store(AEmpty, APropAny);

  case InitObjMemoSlots:
    // Writes to memo slots, but these are not modeled.
    return IrrelevantEffects {};

  case LockObj:
    // Writes object attributes, but these are not modeled.
    return IrrelevantEffects {};

  // Loads $obj->trace, stores $obj->file and $obj->line.
  case InitThrowableFileAndLine:
    return may_load_store(AHeapAny, APropAny);

  //////////////////////////////////////////////////////////////////////
  // Array loads and stores

  case InitVecElem: {
    auto const arr = inst.src(0);
    auto const val = inst.src(1);
    auto const idx = inst.extra<InitVecElem>()->index;
    return PureStore { AElemI { arr, idx }, val, arr };
  }

  case InitDictElem: {
    auto const arr = inst.src(0);
    auto const val = inst.src(1);
    auto const key = inst.extra<InitDictElem>()->key;
    return PureStore { AElemS { arr, key }, val, arr };
  }

  case InitStructElem: {
    auto const arr = inst.src(0);
    auto const val = inst.src(1);
    auto const key = inst.extra<InitStructElem>()->key;
    return PureStore { AElemS { arr, key }, val, arr };
  }

  case LdMonotypeDictVal: {
    // TODO(mcolavita): When we have a type-array-elem method to get the key
    // of an arbitrary array-like type, use that to narrow this load.
    return PureLoad { AElemAny };
  }

  case LdMonotypeVecElem:
  case LdVecElem: {
    auto const base = inst.src(0);
    auto const key  = inst.src(1);
    return PureLoad {
      key->hasConstVal() ? AElemI { base, key->intVal() } : AElemIAny
    };
  }

  case DictGetK:
  case KeysetGetK:
  case BespokeGet:
  case KeysetGetQuiet:
  case DictGetQuiet: {
    auto const base = inst.src(0);
    auto const key  = inst.src(1);
    assertx(key->type().subtypeOfAny(TInt, TStr));
    if (key->isA(TInt)) {
      return PureLoad {
        key->hasConstVal() ? AElemI { base, key->intVal() } : AElemIAny,
      };
    } else {
      return PureLoad {
        key->hasConstVal() ? AElemS { base, key->strVal() } : AElemSAny,
      };
    }
  }

  case LdTypeStructureVal: {
    auto const base = inst.src(0);
    auto const key  = inst.src(1);
    return PureLoad {
      key->hasConstVal() ? AElemS { base, key->strVal() } : AElemSAny,
    };
  }

  case LdTypeStructureValCns: {
    auto const base = inst.src(0);
    auto const key = inst.extra<KeyedData>()->key;
    return PureLoad { AElemS { base, key } };
  }

  case DictIsset:
  case DictIdx:
  case KeysetIsset:
  case KeysetIdx:
  case AKExistsDict:
  case AKExistsKeyset:
  case BespokeGetThrow: {
    auto const base = inst.src(0);
    auto const key  = inst.src(1);
    assertx(key->type().subtypeOfAny(TInt, TStr));
    auto const elem = [&] {
      if (key->isA(TInt)) {
        return key->hasConstVal() ? AElemI { base, key->intVal() } : AElemIAny;
      } else {
        return key->hasConstVal() ? AElemS { base, key->strVal() } : AElemSAny;
      }
    }();
    return may_load_store(elem, AEmpty);
  }

  case InitVecElemLoop:
    {
      auto const extra = inst.extra<InitVecElemLoop>();
      auto const stack_in = AStack::range(
        extra->offset,
        extra->offset + static_cast<int32_t>(extra->size)
      );
      return may_load_store_move(stack_in, AElemIAny, stack_in);
    }

  // These ops may read anything referenced by the input array or object,
  // but not any of the locals or stack frame slots.
  case NewLoggingArray:
  case ProfileArrLikeProps:
    return may_load_store(AHeapAny, AEmpty);

  case NewKeysetArray:
    {
      // NewKeysetArray is reading elements from the stack, but writes to a
      // completely new array, so we can treat the store set as empty.
      auto const extra = inst.extra<NewKeysetArray>();
      auto const stack_in = AStack::range(
        extra->offset,
        extra->offset + static_cast<int32_t>(extra->size)
      );
      return may_load_store_move(stack_in, AEmpty, stack_in);
    }

  case NewStructDict:
    {
      // NewStructDict reads elements from the stack, but writes to a
      // completely new array, so we can treat the store set as empty.
      auto const extra = inst.extra<NewStructData>();
      auto const stack_in = AStack::range(
        extra->offset,
        extra->offset + static_cast<int32_t>(extra->numKeys)
      );
      return may_load_store_move(stack_in, AEmpty, stack_in);
    }

  case NewBespokeStructDict:
    {
      // NewBespokeStructDict reads elements from the stack, but writes to
      // a completely new array, so we can treat the stores as empty.
      auto const extra = inst.extra<NewBespokeStructDict>();
      auto const stack_in = AStack::range(
        extra->offset,
        extra->offset + static_cast<int32_t>(extra->numSlots)
      );
      return may_load_store_move(stack_in, AEmpty, stack_in);
    }

  case MemoGetStaticValue:
  case MemoGetLSBValue:
  case MemoGetInstanceValue:
    // Only reads the memo value (which isn't modeled here).
    return may_load_store(AEmpty, AEmpty);

  case MemoSetStaticValue:
  case MemoSetLSBValue:
  case MemoSetInstanceValue:
    // Writes to the memo value (which isn't modeled)
    return may_load_store(AEmpty, AEmpty);

  case MemoGetStaticCache:
  case MemoGetLSBCache:
  case MemoSetStaticCache:
  case MemoSetLSBCache: {
    // Reads some (non-zero) set of locals for keys, and reads/writes from the
    // memo cache (which isn't modeled).
    auto const extra = inst.extra<MemoCacheStaticData>();
    auto const frame = ALocal {
      inst.src(0),
      AliasIdSet{
        AliasIdSet::IdRange{
          extra->keys.first,
          extra->keys.first + extra->keys.count
        }
      }
    };

    return may_load_store(frame, AEmpty);
  }

  case MemoGetInstanceCache:
  case MemoSetInstanceCache: {
    // Reads some set of locals for keys, and reads/writes from the memo cache
    // (which isn't modeled).
    auto const extra = inst.extra<MemoCacheInstanceData>();
    auto const frame = [&]() -> AliasClass {
      // Unlike MemoGet/SetStaticCache, we can have an empty key range here.
      if (extra->keys.count == 0) return AEmpty;

      return ALocal {
        inst.src(0),
        AliasIdSet{
          AliasIdSet::IdRange{
            extra->keys.first,
            extra->keys.first + extra->keys.count
          }
        }
      };
    }();
    return may_load_store(frame, AEmpty);
  }

  case BespokeIterGetKey:
  case LdPtrIterKey:
    // Array element keys are not tracked by memory effects right
    // now. Be conservative and use AElemAny.
    return may_load_store(AElemAny, AEmpty);

  case LdPtrIterVal:
    return PureLoad { AElemAny };

  case StPtrIterVal:
    return PureStore { AElemAny, inst.src(2), inst.src(0) };

  case BespokeIterGetVal:
  case CheckPtrIterTombstone:
    return may_load_store(AElemAny, AEmpty);

  case ElemDictK:
    return IrrelevantEffects {};

  case VecFirst: {
    auto const base = inst.src(0);
    return may_load_store(AElemI { base, 0 }, AEmpty);
  }
  case VecLast: {
    auto const base = inst.src(0);
    if (base->hasConstVal(TArrLike)) {
      auto const index = static_cast<int64_t>(base->arrLikeVal()->size() - 1);
      return may_load_store(AElemI { base, index }, AEmpty);
    }
    return may_load_store(AElemIAny, AEmpty);
  }
  case DictFirst:
  case DictLast:
  case KeysetFirst:
  case KeysetLast:
    return may_load_store(AElemAny, AEmpty);

  case DictFirstKey:
  case DictLastKey:
  case LdMonotypeDictTombstones:
  case LdMonotypeDictKey:
  case StructDictSlotInPos:
  case LdStructDictKey:
    return may_load_store(AEmpty, AEmpty);

  case LdStructDictVal:
    return PureLoad { AElemSAny };

  case CheckDictKeys:
  case CheckDictOffset:
  case CheckKeysetOffset:
  case CheckMissingKeyInArrLike:
  case ProfileDictAccess:
  case ProfileKeysetAccess:
  case ProfileIterInit:
  case CheckArrayCOW:
  case ProfileArrayCOW:
    return may_load_store(AHeapAny, AEmpty);

  case SameArrLike:
  case NSameArrLike:
    return may_load_store(AElemAny, AEmpty);

  case EqArrLike:
  case NeqArrLike: {
    if (inst.src(0)->type() <= TKeyset && inst.src(1)->type() <= TKeyset) {
      return may_load_store(AElemAny, AEmpty);
    } else {
      return may_load_store(AHeapAny, AHeapAny);
    }
  }

  case AKExistsObj:
    return may_load_store(AHeapAny, AHeapAny);

  //////////////////////////////////////////////////////////////////////
  // Member instructions

  /*
   * Various minstr opcodes that take a Lval in src 0, which may or may not
   * point to a frame local or the evaluation stack. Some may read or write to
   * that pointer while some only read. They can all re-enter the VM and access
   * arbitrary heap locations.
   */
  case IncDecElem:
  case SetElem:
  case SetNewElem:
  case SetOpElem:
  case UnsetElem:
  case ElemDictD:
  case ElemDictU:
  case BespokeElem:
  case ElemDX:
  case ElemUX:
  case SetRange:
  case SetRangeRev:
    // These member ops will load and store from the base lval which they
    // take as their first argument, which may point anywhere in the heap.
    return may_load_store(
      AHeapAny | all_pointees(inst),
      AHeapAny | all_pointees(inst)
    );

  case CGetElem:
  case IssetElem:
  case ElemX:
  case CGetProp:
  case CGetPropQ:
  case SetProp:
  case IssetProp:
  case IncDecProp:
  case SetOpProp:
  case ReserveVecNewElem:
    return may_load_store(AHeapAny, AHeapAny);

  /*
   * Intermediate minstr operations. In addition to a base pointer like the
   * operations above, these may take a pointer to MInstrState::tvRef
   * which they may store to (but not read from).
   */
  case PropX:
  case PropDX:
  case PropQ:
    return minstr_with_tvref(inst);

  /*
   * Collection accessors can read from their inner array buffer, but stores
   * COW and behave as if they only affect collection memory locations.  We
   * don't track those, so it's returning AEmpty for now.
   */
  case MapIsset:
  case PairIsset:
  case VectorIsset:
    return may_load_store(AHeapAny, AEmpty /* Note */);
  case MapGet:
  case MapSet:
  case VectorSet:
    return may_load_store(AHeapAny, AEmpty /* Note */);

  case DeserializeLazyProp:
    return may_load_store(AHeapAny, AHeapAny);

  case LdInitPropAddr:
    return may_load_store(
      AProp {
        inst.src(0),
        safe_cast<uint16_t>(inst.extra<LdInitPropAddr>()->index)
      },
      AEmpty
    );
  case LdInitRDSAddr:
    return may_load_store(
      ARds { inst.extra<LdInitRDSAddr>()->handle },
      AEmpty
    );

  //////////////////////////////////////////////////////////////////////
  // Instructions that allocate new objects, without reading any other memory
  // at all, so any effects they have on some types of memory locations we
  // track are isolated from anything else we care about.

  case NewClsMeth:
  case NewRClsMeth:
  case NewCol:
  case NewColFromArray:
  case NewPair:
  case NewInstanceRaw:
  case NewDictArray:
  case NewRFunc:
  case FuncCred:
  case AllocVec:
  case AllocStructDict:
  case AllocBespokeStructDict:
  case ConvDblToStr:
  case ConvIntToStr:
  case InitStructPositions:
  case AllocInitROM:
  case StPtrAt:
  case StTypeAt:
  case VoidPtrAsDataType:
    return IrrelevantEffects {};

  case AllocObj:
    // AllocObj re-enters to call constructors, but if it weren't for that we
    // could ignore its loads and stores since it's a new object.
    return may_load_store(AEmpty, AEmpty);

  //////////////////////////////////////////////////////////////////////
  // Instructions that explicitly manipulate the stack.

  case LdStk:
    return PureLoad { AStack::at(inst.extra<LdStk>()->offset) };

  case StStk:
  case StStkMeta:
    return PureStore {
      AStack::at(inst.extra<IRSPRelOffsetData>()->offset),
      inst.src(1),
      nullptr
    };

  case StStkRange: {
    auto const extra = inst.extra<StStkRange>();
    auto const startOff = extra->start;
    auto const count = extra->count;
    return PureStore {
      AStack::range(startOff, startOff + static_cast<int32_t>(count)),
      inst.src(1),
      nullptr
    };
  }

  case StOutValue:
    // Technically these writes affect the caller's stack, but there is no way
    // to actually observe them from within the callee. They can also only
    // occur once on any exit path from a function.
    return may_load_store(AEmpty, AEmpty);

  case LdOutAddr:
    return IrrelevantEffects{};

  case DbgTraceCall: {
    auto const irSPOff = inst.src(1)->inst()->extra<DefStackData>()->irSPOff;
    auto const stkHigh = SBInvOffset{0}.to<IRSPRelOffset>(irSPOff);
    auto const stkLow = inst.extra<DbgTraceCall>()->offset;
    auto const stk = stkLow == stkHigh
      ? AEmpty : AStack::range(stkLow, stkHigh);
    return may_load_store(stk | ALocalAny, AEmpty);
  }

  case Unreachable:
    // Unreachable code kills every memory location.
    return may_load_store_kill(AEmpty, AEmpty, AUnknown);

  case ResolveTypeStruct: {
    auto const extra = inst.extra<ResolveTypeStructData>();
    auto const stack_in = AStack::range(
      extra->offset,
      extra->offset + static_cast<int32_t>(extra->size)
    );
    return may_load_store(AliasClass(stack_in)|AHeapAny, AHeapAny);
  }

  case DefFP:
  case DefFuncEntryFP:
    return may_load_store(AFBasePtr, AFBasePtr);

  case EnterFrame:
    return may_load_store(
      AFBasePtr,
      AFBasePtr | AFFunc { inst.dst() } | AFMeta { inst.dst() }
    );

  case LdARFlags:
    return PureLoad { AFMeta { inst.src(0) }};

  case LdUnitPerRequestFilepath:
    return PureLoad {
      ARds { inst.extra<LdUnitPerRequestFilepath>()->handle },
    };

  case LdImplicitContext:
    return PureLoad { ARds { ImplicitContext::activeCtx.handle() } };

  case StImplicitContext:
    return PureStore {
      ARds { ImplicitContext::activeCtx.handle() }, inst.src(0), nullptr
    };
  case EndBlock: {
    auto extra = inst.extra<EndBlock>();
    auto spOff = extra->spOff;
    auto numInOutParams = extra->func->numInOutParams();
    auto stack = AStack::range(spOff, spOff + 1 + numInOutParams);
    return may_load_store(stack, AEmpty);
  }
  //////////////////////////////////////////////////////////////////////
  // Instructions that never read or write memory locations tracked by this
  // module.

  case AbsDbl:
  case AddDbl:
  case AddInt:
  case AddOffset:
  case AdvanceDictPtrIter:
  case AdvanceKeysetPtrIter:
  case AdvanceVecPtrIter:
  case AndInt:
  case AssertType:
  case AssertLoc:
  case AssertStk:
  case AssertMBase:
  case BespokeIterEnd:
  case BespokeIterFirstPos:
  case BespokeIterLastPos:
  case ConvFuncPrologueFlagsToARFlags:
  case DefFrameRelSP:
  case DefFuncEntryArFlags:
  case DefFuncEntryCalleeId:
  case DefFuncEntryCtx:
  case DefFuncEntryPrevFP:
  case DefFuncPrologueCallee:
  case DefFuncPrologueCtx:
  case DefFuncPrologueFlags:
  case DefFuncPrologueNumArgs:
  case DefFuncPrologueNamedArgs:
  case DefRegSP:
  case DictIterEnd:
  case EnterPrologue:
  case EnterTranslation:
  case EqBool:
  case EqCls:
  case EqLazyCls:
  case EqFunc:
  case EqFuncId:
  case EqStrPtr:
  case EqArrayDataPtr:
  case EqDbl:
  case EqInt:
  case EqPtrIter:
  case ExitPrologue:
  case GetDictPtrIter:
  case GetKeysetPtrIter:
  case GetVecPtrIter:
  case GteBool:
  case GteInt:
  case GtBool:
  case GtInt:
  case Jmp:
  case JmpNZero:
  case JmpZero:
  case KeysetIterEnd:
  case LdPropAddr:
  case LdStkAddr:
  case LdVecElemAddr:
  case LteBool:
  case LteDbl:
  case LteInt:
  case LtBool:
  case LtInt:
  case GtDbl:
  case GteDbl:
  case LtDbl:
  case DivDbl:
  case DivInt:
  case MulDbl:
  case MulInt:
  case MulIntO:
  case NeqBool:
  case NeqDbl:
  case NeqInt:
  case SameObj:
  case NSameObj:
  case EqRes:
  case NeqRes:
  case CmpBool:
  case CmpInt:
  case CmpDbl:
  case SubDbl:
  case SubInt:
  case XorBool:
  case XorInt:
  case OrInt:
  case AssertNonNull:
  case CheckNonNull:
  case CheckNullptr:
  case CheckSmashableClass:
  case Ceil:
  case Floor:
  case DefLabel:
  case Nop:
  case Mod:
  case PseudoRandomInt:
  case Conjure:
  case ConvBoolToInt:
  case ConvBoolToDbl:
  case DefConst:
  case LdLocAddr:
  case Sqrt:
  case Shl:
  case Shr:
  case Lshr:
  case Mov:
  case ConvDblToBool:
  case ConvDblToInt:
  case DblAsBits:
  case IntAsPtrToElem:
  case PtrToElemAsInt:
  case LdMIStateTempBaseAddr:
  case LdClsCns:
  case LdSubClsCns:
  case LdResolvedTypeCns:
  case LdResolvedTypeCnsClsName:
  case LdResolvedTypeCnsNoCheck:
  case LdClsCtxCns:
  case CheckSubClsCns:
  case LdClsCnsVecLen:
  case EqClassId:
  case FuncHasAttr:
  case ClassHasAttr:
  case LdFuncRequiredCoeffects:
  case IsFunReifiedGenericsMatched:
  case LdSmashable:
  case LdSmashableFunc:
  case LdRDSAddr:
  case CheckRange:
  case ProfileType:
  case ProfileCoeffectFunParam:
  case LdIfaceMethod:
  case InstanceOfIfaceVtable:
  case IsTypeStructCached:
  case LdTVAux:
  case MethodExists:
  case GetTime:
  case GetTimeNs:
  case ProfileInstanceCheck:
  case Select:
  case LookupSPropSlot:
  case ConvPtrToLval:
  case ProfileProp:
  case ProfileIsTypeStruct:
  case LdLazyClsName:
  case LdEnumClassLabelName:
  case DirFromFilepath:
  case CheckFuncNeedsCoverage:
  case RecordFuncCall:
  case LoadBCSP:
  case StructDictSlot:
  case StructDictElemAddr:
  case StructDictAddNextSlot:
  case StructDictTypeBoundCheck:
  case LdImplicitContextMemoKey:
  case LdMemoAgnosticIC:
  case RetranslateOptAsync:
  case DbgTrap:
    return IrrelevantEffects {};

  case LookupClsCns:
  case LookupClsCtxCns:
    return may_load_store(AEmpty, AEmpty);

  case LdClosureArg:
    return PureLoad {
      AClosureArg {
        inst.src(0),
        safe_cast<uint16_t>(inst.extra<LdClosureArg>()->index)
    }};

  case StClosureArg:
    return PureStore {
      AClosureArg {
        inst.src(0),
        safe_cast<uint16_t>(inst.extra<StClosureArg>()->index)
      },
      inst.src(1),
      inst.src(0)
    };

  case StArResumeAddr:
    return PureStore { AFMeta { inst.src(0) }, nullptr };

  case LdFrameThis:
  case LdFrameCls:
    return PureLoad { AFContext { inst.src(0) }};

  case StFrameCtx:
    return PureStore { AFContext { inst.src(0) }, inst.src(1) };

  case StFrameFunc:
    return PureStore { AFFunc { inst.src(0) }, nullptr };

  case StFrameMeta:
    return PureStore { AFMeta { inst.src(0) }, nullptr };

  case StVMFP:
    return PureStore { AVMFP, inst.src(0) };

  case StVMSP:
    return PureStore { AVMSP, inst.src(0) };

  case StVMPC:
    return PureStore { AVMPC, nullptr };

  case StVMReturnAddr:
    return PureStore { AVMRetAddr, inst.src(0) };

  case StVMRegState:
    return PureStore { AVMRegState, inst.src(0) };

  //////////////////////////////////////////////////////////////////////
  // Instructions that technically do some things w/ memory, but not in any way
  // we currently care about.  They however don't return IrrelevantEffects
  // because we assume (in refcount-opts) that IrrelevantEffects instructions
  // can't even inspect Countable reference count fields, and several of these
  // can.  All GeneralEffects instructions are assumed to possibly do so.

  case LdPublicFunc:
  case DecRefNZ:
  case ProfileDecRef:
  case AFWHBlockOn:
  case AFWHPushTailFrame:
  case IncRef:
  case LdClosureCls:
  case LdClosureThis:
  case LdRetVal:
  case ConvStrToInt:
  case ConvResToInt:
  case OrdStr:
  case ChrInt:
  case CreateSSWH:
  case CreateFSWH:
  case CheckSurpriseFlags:
  case CheckSurpriseFlagsEnter:
  case CheckType:
  case CheckCold:
  case ContValid:
  case IncProfCounter:
  case IncCallCounter:
  case IncStatCounter:
  case IncStat:
  case ContCheckNext:
  case CountVec:
  case CountDict:
  case CountKeyset:
  case FuncHasReifiedGenerics:
  case ClassHasReifiedGenerics:
  case HasReifiedParent:
  case ReifiedInit:
  case InstanceOf:
  case InstanceOfBitmask:
  case NInstanceOfBitmask:
  case InstanceOfIface:
  case InterfaceSupportsArrLike:
  case InterfaceSupportsDbl:
  case InterfaceSupportsInt:
  case InterfaceSupportsStr:
  case IsLegacyArrLike:
  case IsWaitHandle:
  case IsCol:
  case HasToString:
  case DbgAssertRefCount:
  case DbgCheckLocalsDecRefd:
  case GtStr:
  case GteStr:
  case LtStr:
  case LteStr:
  case EqStr:
  case NeqStr:
  case SameStr:
  case NSameStr:
  case CmpStr:
  case GtRes:
  case GteRes:
  case LtRes:
  case LteRes:
  case CmpRes:
  case LdBindAddr:
  case LdSwitchDest:
  case LdSSwitchDest:
  case RBTraceEntry:
  case RBTraceMsg:
  case ConvIntToBool:
  case ConvIntToDbl:
  case ConvStrToBool:
  case ConvStrToDbl:
  case ConvResToDbl:
  case ExtendsClass:
  case LdUnwinderValue:
  case StUnwinderExn:
  case LdClsName:
  case LdAFWHActRec:
  case LdContActRec:
  case LdContArKey:
  case LdContArValue:
  case LdContField:
  case LdContResumeAddr:
  case StContArKey:
  case StContArValue:
  case StContArState:
  case ContArIncIdx:
  case ContArIncKey:
  case ContArUpdateIdx:
  case LookupClsCached:
  case LdClsInitData:
  case UnwindCheckSideExit:
  case CallViolatesModuleBoundary:
  case LdCns:
  case LdFuncVecLen:
  case LdClsMethod:
  case LdClsMethodCacheCls:
  case LdClsMethodCacheFunc:
  case LdClsMethodFCacheFunc:
  case LdTypeCns:
  case LdTypeCnsNoThrow:
  case LdTypeCnsClsName:
  case ProfileSwitchDest:
  case LdFuncCls:
  case LdFuncInOutBits:
  case LdFuncNumParams:
  case LdFuncName:
  case LdMethCallerName:
  case LdObjClass:
  case LdObjInvoke:
  case LdObjMethodD:
  case LdObjMethodS:
  case LdStrLen:
  case StringIsset:
  case LdWHResult:
  case LdWHState:
  case LdWHNotDone:
  case LookupClsMethod:
  case LookupCls:
  case StrictlyIntegerConv:
  case DbgAssertFunc:
  case ProfileCall:
  case ProfileMethod:
  case DecReleaseCheck:
    return may_load_store(AEmpty, AEmpty);

  case BeginCatch:
    return may_load_store_kill(AEmpty, AVMRegAny | AVMRegState, AMIStateAny);

  case LogArrayReach:
  case LogGuardFailure:
  case LogClsSpeculation:
    return may_load_store(AHeapAny, AEmpty);

  // Some that touch memory we might care about later, but currently don't:
  case ColIsEmpty:
  case ColIsNEmpty:
  case ConvTVToBool:
  case ConvObjToBool:
  case CountCollection:
  case LdVectorSize:
  case CheckVecBounds:
  case LdColVec:
  case LdColDict:
    return may_load_store(AEmpty, AEmpty);

  //////////////////////////////////////////////////////////////////////
  // Instructions that can re-enter the VM and touch most heap things.  They
  // also may generally write to the eval stack below an offset (see
  // alias-class.h above AStack for more).

  case DecRef:
    return may_load_store(AHeapAny, AHeapAny);

  case ReleaseShallow:
    return may_load_store(AHeapAny, AHeapAny);

  case GetMemoKey:
    return may_load_store(AHeapAny, AHeapAny);

  case GetMemoKeyScalar:
    return IrrelevantEffects{};

  case ProfileGlobal:
  case LdGblAddr:
  case LdGblAddrDef:
    return may_load_store(AEmpty, AEmpty);

  case LdClsCtor:
    return may_load_store(AEmpty, AEmpty);

  case RaiseCoeffectsCallViolation:
  case RaiseCoeffectsFunParamCoeffectRulesViolation:
  case RaiseCoeffectsFunParamTypeViolation:
  case LdCoeffectFunParamNaive:
  case RaiseModuleBoundaryViolation:
  case RaiseModulePropertyViolation:
  case ThrowMissingNamedParam:
  case ThrowNamedArgumentNameMismatch:
  case ThrowUnexpectedNamedArguments:
    return may_load_store(AEmpty, AEmpty);

  case StaticAnalysisError:
    if (Cfg::Eval::CrashOnStaticAnalysisError) return IrrelevantEffects{};
    return may_load_store(AHeapAny, AHeapAny);

  case LdClsPropAddrOrNull:   // may run 86{s,p}init, which can autoload
  case LdClsPropAddrOrRaise:  // raises errors, and 86{s,p}init
    return may_load_store(
      AHeapAny,
      AHeapAny | all_pointees(inst) | AMIStateROProp
    );
  case Clone:
  case ThrowArrayIndexException:
  case ThrowArrayKeyException:
  case ThrowUninitLoc:
  case ThrowUndefPropException:
  case RaiseTooManyArg:
  case RaiseError:
  case RaiseNotice:
  case RaiseWarning:
  case RaiseForbiddenDynCall:
  case RaiseForbiddenDynConstruct:
  case CheckStrictPackageDynamicReference:
  case RaiseMissingDynamicallyReferenced:
  case RaiseStrToClassNotice:
  case CheckClsMethFunc:
  case CheckClsReifiedGenericMismatch:
  case CheckClsRGSoft:
  case CheckFunReifiedGenericMismatch:
  case CheckFunReifiedGenericsWithNamedArgs:
  case GetClsRGProp:
  case CheckInOutMismatch:
  case CheckReadonlyMismatch:
  case ConvTVToStr:
  case ConvObjToStr:
  case Count:      // re-enters on CountableClass
  case GtObj:
  case GteObj:
  case LtObj:
  case LteObj:
  case EqObj:
  case NeqObj:
  case CmpObj:
  case GtArrLike:
  case GteArrLike:
  case LtArrLike:
  case LteArrLike:
  case CmpArrLike:
  case OODeclExists:
  case LdCls:          // autoload
  case LdClsCached:    // autoload
  case LdFunc:         // autoload
  case LdFuncCached:   // autoload
  case InitClsCns:      // autoload
  case InitSubClsCns: // May run 86cinit
  case ProfileSubClsCns: // May run 86cinit
  case LookupClsMethodCache:  // autoload
  case LookupClsMethodFCache: // autoload
  case LookupCnsE:
  case LookupFuncCached: // autoload
  case StringGet:      // raise_notice
  case OrdStrIdx:      // raise_notice
  case AddNewElemKeyset:   // can re-enter
  case DictGet:
  case KeysetGet:
  case DictSet:
  case BespokeSet:
  case BespokeAppend:
  case BespokeUnset:
  case StructDictUnset:
  case ConcatStrStr:
  case PrintStr:
  case PrintBool:
  case PrintInt:
  case ConcatIntStr:
  case ConcatStrInt:
  case ConvObjToDbl:
  case ConvObjToInt:
  case ConvTVToInt:
  case ConcatStr3:
  case ConcatStr4:
  case ConvTVToDbl:
  case ConvObjToVec:
  case ConvObjToDict:
  case ConvObjToKeyset:
  case ThrowOutOfBounds:
  case ThrowInvalidArrayKey:
  case ThrowInvalidOperation:
  case ThrowCallReifiedFunctionWithoutGenerics:
  case ThrowDivisionByZeroException:
  case ThrowHasThisNeedStatic:
  case ThrowInOutMismatch:
  case ThrowReadonlyMismatch:
  case ThrowLateInitPropError:
  case ThrowMissingArg:
  case ThrowMissingThis:
  case ArrayMarkLegacyShallow:
  case ArrayMarkLegacyRecursive:
  case ThrowCannotModifyReadonlyCollection:
  case ThrowLocalMustBeValueTypeException:
  case ThrowMustBeEnclosedInReadonly:
  case ThrowMustBeMutableException:
  case ThrowMustBeReadonlyException:
  case ThrowMustBeValueTypeException:
  case ArrayUnmarkLegacyShallow:
  case ArrayUnmarkLegacyRecursive:
  case SetOpTV:
  case OutlineSetOp:
  case ThrowAsTypeStructException:
  case ThrowAsTypeStructError:
  case PropTypeRedefineCheck: // Can raise and autoload
  case PropTypeValid: // Can raise and autoload
  case HandleRequestSurprise:
  case BespokeEscalateToVanilla:
    return may_load_store(AHeapAny, AHeapAny);

  case BespokeSetPos:
    return may_load_store(AElemAny, AElemAny);

  case AddNewElemVec:
  case RaiseErrorOnInvalidIsAsExpressionType:
  case IsTypeStruct:
  case IsTypeStructShallow:
  case RecordReifiedGenericsAndGetTSList:
  case CopyArray:
    return may_load_store(AElemAny, AEmpty);

  case ConvArrLikeToVec:
  case ConvArrLikeToDict:
  case ConvArrLikeToKeyset: // Decrefs input values
    return may_load_store(AElemAny, AEmpty);

  // debug_backtrace() traverses stack and WaitHandles on the heap.
  case DebugBacktrace:
    return may_load_store(AHeapAny|ALocalAny, AHeapAny);

  // This instruction doesn't touch memory we track, except that it may
  // re-enter to construct php Exception objects.  During this re-entry anything
  // can happen (e.g. a surprise flag check could cause a php signal handler to
  // run arbitrary code).
  case AFWHPrepareChild:
    return may_load_store(AActRec { inst.src(0) }, AEmpty);

  //////////////////////////////////////////////////////////////////////
  // The following instructions are used for debugging memory optimizations.
  // We can't ignore them, because they can prevent future optimizations;
  // eg t1 = LdStk<N>; DbgTrashStk<N>; StStk<N> t1
  // If we ignore the DbgTrashStk it looks like the StStk is redundant

  case DbgTrashStk:
    return may_load_store_kill(
      AEmpty, AEmpty, AStack::at(inst.extra<DbgTrashStk>()->offset));
  case DbgTrashFrame:
    return may_load_store_kill(
      AEmpty, AEmpty, actrec(inst.src(0), inst.extra<DbgTrashFrame>()->offset));
  case DbgTrashMem:
    return may_load_store_kill(AEmpty, AEmpty, pointee(inst.src(0)));

  //////////////////////////////////////////////////////////////////////

  }

  not_reached();
}

//////////////////////////////////////////////////////////////////////

DEBUG_ONLY bool check_effects(const IRInstruction& inst, const MemEffects& me) {
  SCOPE_ASSERT_DETAIL("Memory Effects") {
    return fmt::format("  inst: {}\n  effects: {}\n", inst, show(me));
  };

  auto const check_obj = [&] (SSATmp* obj) {
    // canonicalize() may have replaced the SSATmp with a less refined
    // one, so we cannot assert <= TObj.
    always_assert_flog(
      obj->type() <= TBottom ||
      obj->type().maybe(TObj),
      "Non obj pointer in memory effects"
    );
  };

  auto const check = [&] (AliasClass a) {
    if (auto const pr = a.prop())  check_obj(pr->obj);
  };

  match(
    me,
    [&] (const GeneralEffects& x) {
      check(x.loads);
      check(x.stores);
      check(x.moves);
      check(x.kills);
      check(x.inout);
      for (auto const& frame : x.backtrace) {
        check(frame);
      }

      // Locations may-moved always should also count as may-loads.
      always_assert(x.moves <= x.loads);

      if (inst.mayRaiseErrorWithSources()) {
        // Any instruction that can raise an error can run a user error handler
        // and have arbitrary effects on the heap.
        always_assert(AHeapAny <= x.loads);
        always_assert(AHeapAny <= x.stores);
        /*
         * They also ought to kill /something/ on the stack, because of
         * possible re-entry.  It's not incorrect to leave things out of the
         * kills set, but this assertion is here because we shouldn't do it on
         * purpose, so this is here until we have a reason not to assert it.
         *
         * The mayRaiseError instructions should all be going through
         * may_reenter right now, which will kill the stack below the re-entry
         * depth---unless the marker for `inst' doesn't have an fp set.
         */
        always_assert(inst.marker().fixupFP() == nullptr ||
                      AStackAny.maybe(x.kills));
      }
    },
    [&] (const PureLoad& x)         { check(x.src); },
    [&] (const PureStore& x)        { check(x.dst); },
    [&] (const ExitEffects& x)      { check(x.live);
                                      check(x.kills);
                                      check(x.uninits); },
    [&] (const IrrelevantEffects&)  {},
    [&] (const UnknownEffects&)     {},
    [&] (const CallEffects& x) {
      check(x.kills);
      check(x.uninits);
      check(x.inputs);
      check(x.actrec);
      check(x.outputs);
      for (auto const& frame : x.backtrace) {
        check(frame);
      }
    },
    [&] (const PureInlineCall& x)   { check(x.base);
                                      check(x.actrec); },
    [&] (const ReturnEffects& x)    { check(x.kills); }
  );

  return true;
}

//////////////////////////////////////////////////////////////////////

}

MemEffects memory_effects(const IRInstruction& inst) {
  auto const inner = memory_effects_impl(inst);
  auto const ret = [&] () -> MemEffects {
    if (!inst.mayRaiseErrorWithSources()) {
      if (inst.maySyncVMRegsWithSources()) {
        auto fail = [&] {
          always_assert_flog(
            false,
            "Instruction {} has effects {} but has been marked as MaySyncVMRegs "
            "without MayRaiseError.",
            inst,
            show(inner)
          );
          return may_load_store(AUnknown, AUnknown);
        };
        return match<MemEffects>(
          inner,
          [&] (const GeneralEffects& x)   {
            return GeneralEffects {
              x.loads | AVMRegAny | AVMRegState,
              x.stores | AVMRegAny,
              x.moves, x.kills, x.inout, x.backtrace,
            };
          },
          [&] (const CallEffects&)        { return fail(); },
          [&] (const UnknownEffects&)     { return fail(); },
          [&] (const PureLoad& x)         {
            return may_load_store(
              x.src | AVMRegAny | AVMRegState,
              AVMRegAny
            );
          },
          [&] (const PureStore&)          { return fail(); },
          [&] (const ExitEffects&)        { return fail(); },
          [&] (const PureInlineCall&)     { return fail(); },
          [&] (const IrrelevantEffects&)  { return fail(); },
          [&] (const ReturnEffects&)      { return fail(); }
        );
      }
      return inner;
    }

    auto fail = [&] {
      always_assert_flog(
        false,
        "Instruction {} has effects {}, but has been marked as MayRaiseError "
        "and must use a UnknownEffects, GeneralEffects, or CallEffects type.",
        inst,
        show(inner)
      );
      return may_load_store(AUnknown, AUnknown);
    };

    // Calls are implicitly MayRaise, all other instructions must use the
    // GeneralEffects or UnknownEffects class of memory effects
    return match<MemEffects>(
      inner,
      [&] (const GeneralEffects& x)   { return may_reenter(inst, x); },
      [&] (const CallEffects& x)      { return x; },
      [&] (const UnknownEffects& x)   { return x; },
      [&] (const PureLoad&)           { return fail(); },
      [&] (const PureStore&)          { return fail(); },
      [&] (const ExitEffects&)        { return fail(); },
      [&] (const PureInlineCall&)     { return fail(); },
      [&] (const IrrelevantEffects&)  { return fail(); },
      [&] (const ReturnEffects&)      { return fail(); }
    );
  }();
  assertx(check_effects(inst, ret));
  return ret;
}

AliasClass pointee(const SSATmp* tmp) {
  auto acls = AEmpty;
  auto const visit = [&] (const IRInstruction* sinst, const SSATmp* ptr) {
    acls |= [&] () -> AliasClass {
      auto const type = ptr->type() & TMem;
      always_assert(type != TBottom);

      if (sinst->is(LdMBase)) return sinst->extra<LdMBase>()->acls;
      if (sinst->is(LdRDSAddr, LdInitRDSAddr)) {
        return ARds { sinst->extra<RDSHandleData>()->handle };
      }

      auto const specific = [&] () -> Optional<AliasClass> {
        if (type <= TMemToFrame) {
          if (sinst->is(LdLocAddr)) {
            return AliasClass {
              ALocal { sinst->src(0), sinst->extra<LdLocAddr>()->locId }
            };
          }
          return ALocalAny;
        }

        if (type <= TMemToStk) {
          if (sinst->is(LdStkAddr)) {
            return AliasClass {
              AStack::at(sinst->extra<LdStkAddr>()->offset)
            };
          }
          return AStackAny;
        }

        if (type <= TMemToProp) {
          if (sinst->is(LdPropAddr, LdInitPropAddr)) {
            return AliasClass {
              AProp {
                sinst->src(0),
                safe_cast<uint16_t>(sinst->extra<IndexData>()->index)
              }
            };
          }
          return APropAny;
        }

        auto const elem = [&] () -> AliasClass {
          auto const base = sinst->src(0);
          auto const key  = sinst->src(1);
          always_assert(base->isA(TArrLike));

          if (key->isA(TInt)) {
            if (key->hasConstVal()) return AElemI { base, key->intVal() };
            return AElemIAny;
          }
          if (key->isA(TStr)) {
            assertx(base->isA(TBottom) || !base->isA(TVec));
            if (key->hasConstVal()) return AElemS { base, key->strVal() };
            return AElemSAny;
          }
          return AElemAny;
        };

        if (type <= TMemToElem) {
          if (sinst->is(LdVecElemAddr, ElemDictK, StructDictElemAddr)) {
            return elem();
          }
          return AElemAny;
        }

        return std::nullopt;
      }();

      if (specific) {
        // A pointer has to point at *something*, so we should not get
        // AEmpty here.
        assertx(*specific != AEmpty);
        // We don't currently ever form pointers to something that's not a
        // TypedValue.
        assertx(*specific <= AUnknownTV);
        return *specific;
      }

      /*
       * None of the above worked, so try to make the smallest union
       * we can based on the pointer type.
       */
      return pointee(type);
    }();
    return true;
  };
  visitEveryDefiningInst(tmp, visit);
  return acls;
}

AliasClass pointee(const Type& type) {
  assertx(type.maybe(TMem));

  auto ret = AEmpty;
  if (type.maybe(TMemToStk))     ret = ret | AStackAny;
  if (type.maybe(TMemToFrame))   ret = ret | ALocalAny;
  if (type.maybe(TMemToProp))    ret = ret | APropAny;
  if (type.maybe(TMemToElem))    ret = ret | AElemAny;
  if (type.maybe(TMemToMISTemp)) ret = ret | AMIStateTempBase;
  if (type.maybe(TMemToClsInit)) ret = ret | AHeapAny;
  if (type.maybe(TMemToSProp))   ret = ret | ARdsAny;
  if (type.maybe(TMemToGbl))     ret = ret | AOther | ARdsAny;
  if (type.maybe(TMemToOther))   ret = ret | AOther | ARdsAny;
  if (type.maybe(TMemToConst))   ret = ret | AOther;

  // The pointer type should lie completely within the above
  // locations.
  assertx(type <= (TMemToStk|TMemToFrame|TMemToProp|TMemToElem|
                   TMemToMISTemp|TMemToClsInit|TMemToSProp|TMemToGbl|
                   TMemToOther|TMemToConst));
  assertx(ret != AEmpty);
  assertx(ret <= AUnknownTV);
  return ret;
}

//////////////////////////////////////////////////////////////////////

MemEffects canonicalize(const MemEffects& me) {
  using R = MemEffects;
  return match<R>(
    me,
    [&] (const GeneralEffects& x) -> R {
      return GeneralEffects {
        canonicalize(x.loads),
        canonicalize(x.stores),
        canonicalize(x.moves),
        canonicalize(x.kills),
        canonicalize(x.inout),
        canonicalize(x.backtrace),
      };
    },
    [&] (const PureLoad& x) -> R {
      return PureLoad { canonicalize(x.src) };
    },
    [&] (const PureStore& x) -> R {
      return PureStore { canonicalize(x.dst), x.value, x.dep };
    },
    [&] (const ExitEffects& x) -> R {
      return ExitEffects {
        canonicalize(x.live),
        canonicalize(x.kills),
        canonicalize(x.uninits)
      };
    },
    [&] (const PureInlineCall& x) -> R {
      return PureInlineCall {
        canonicalize(x.base),
        x.fp,
        canonicalize(x.actrec)
      };
    },
    [&] (const CallEffects& x) -> R {
      return CallEffects {
        canonicalize(x.kills),
        canonicalize(x.uninits),
        canonicalize(x.inputs),
        canonicalize(x.actrec),
        canonicalize(x.outputs),
        canonicalize(x.backtrace)
      };
    },
    [&] (const ReturnEffects& x) -> R {
      return ReturnEffects { canonicalize(x.kills) };
    },
    [&] (const IrrelevantEffects& x) -> R { return x; },
    [&] (const UnknownEffects& x)    -> R { return x; }
  );
}

//////////////////////////////////////////////////////////////////////

std::string show(const MemEffects& effects) {
  return match<std::string>(
    effects,
    [&] (const GeneralEffects& x) {
      return fmt::format("mlsmkib({} ; {} ; {} ; {} ; {} ; {})",
        show(x.loads),
        show(x.stores),
        show(x.moves),
        show(x.kills),
        show(x.inout),
        show(x.backtrace)
      );
    },
    [&] (const ExitEffects& x) {
      return fmt::format("exit({} ; {}; {})",
        show(x.live),
        show(x.kills),
        show(x.uninits)
      );
    },
    [&] (const PureInlineCall& x) {
      return fmt::format("inline_call({} ; {})",
        show(x.base),
        show(x.actrec)
      );
    },
    [&] (const CallEffects& x) {
      return fmt::format("call({} ; {} ; {} ; {} ; {} ; {})",
        show(x.kills),
        show(x.uninits),
        show(x.inputs),
        show(x.actrec),
        show(x.outputs),
        show(x.backtrace)
      );
    },
    [&] (const PureLoad& x)        { return fmt::format("ld({})", show(x.src)); },
    [&] (const PureStore& x)       { return fmt::format("st({})", show(x.dst)); },
    [&] (const ReturnEffects& x) {
      return fmt::format("return({})", show(x.kills));
    },
    [&] (const IrrelevantEffects&) { return "IrrelevantEffects"; },
    [&] (const UnknownEffects&)    { return "UnknownEffects"; }
  );
}

//////////////////////////////////////////////////////////////////////

GeneralEffects general_effects_for_vmreg_liveness(
    const GeneralEffects& l, KnownRegState liveness) {
  auto ret = GeneralEffects { l.loads, l.stores, l.moves, l.kills, l.inout, l.backtrace };

  if (liveness == KnownRegState::Dead) {
    ret.loads = l.loads.exclude_vm_reg().value_or(AEmpty);
  } else if (liveness == KnownRegState::Live) {
    ret.stores = l.stores.exclude_vm_reg().value_or(AEmpty);
  }

  return ret;
}

//////////////////////////////////////////////////////////////////////

bool hasMInstrBaseEffects(const IRInstruction& inst) {
  switch (inst.op()) {
    case ElemDictD:
    case ElemDX:
    case BespokeElem:
    case ElemDictU:
    case ElemUX:
    case SetElem:
    case UnsetElem:
    case SetOpElem:
    case IncDecElem:
    case SetNewElem:
    case SetRange:
    case SetRangeRev:
      return true;
    default:
      return false;
  }
}

Optional<Type> mInstrBaseEffects(const IRInstruction& inst, Type old) {
  assertx(hasMInstrBaseEffects(inst));

  switch (inst.op()) {
    case ElemDictD:
    case ElemDX:
    case SetOpElem:
    case IncDecElem:
      // Always COWs arrays, leaves strings alone
      return old.maybe(TArrLike)
        ? make_optional(
          ((old & TArrLike).modified() & TCounted) | (old - TArrLike)
        )
        : std::nullopt;
    case ElemDictU:
    case ElemUX:
    case UnsetElem:
      // Might COW arrays (depending if key is present), leaves strings alone
      return old.maybe(TArrLike)
        ? make_optional(((old & TArrLike).modified() & TCounted) | old)
        : std::nullopt;
    case SetElem:
      // COWs both arrays and strings
      return old.maybe(TArrLike | TStr)
        ? make_optional(old.modified() & TCounted)
        : std::nullopt;
    case SetNewElem: {
      // Vecs and keysets will always COW. Dicts will COW in almost
      // all situations except if the "next key" hits the limit.
      if (!old.maybe(TArrLike)) return std::nullopt;
      return
        ((old & TArrLike).modified() & TCounted) | (old - (TVec | TKeyset));
    }
    case SetRange:
    case SetRangeRev:
      // Always COWs strings
      return old.maybe(TStr)
        ? make_optional(((old & TStr).modified() & TCounted) | (old - TStr))
        : std::nullopt;
    case BespokeElem: {
      // Behaves like define if S2 is true, unset if false
      if (!old.maybe(TArrLike)) return std::nullopt;
      auto const t = (old & TArrLike).modified() & TCounted;
      return inst.src(2)->boolVal()
        ? t | (old - TArrLike)
        : t | old;
    }
    default:
      not_reached();
  }
}

//////////////////////////////////////////////////////////////////////

}
