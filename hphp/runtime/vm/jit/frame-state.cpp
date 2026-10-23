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

#include "hphp/runtime/vm/jit/frame-state.h"

#include "hphp/runtime/vm/jit/alias-class.h"
#include "hphp/runtime/vm/jit/analysis.h"
#include "hphp/runtime/vm/jit/ir-instruction.h"
#include "hphp/runtime/vm/jit/irgen-call.h"
#include "hphp/runtime/vm/jit/location.h"
#include "hphp/runtime/vm/jit/memory-effects.h"
#include "hphp/runtime/vm/jit/ssa-tmp.h"
#include "hphp/runtime/vm/jit/stack-offsets.h"
#include "hphp/runtime/vm/jit/translator.h"
#include "hphp/runtime/vm/jit/type-array-elem.h"

#include "hphp/util/match.h"
#include "hphp/util/trace.h"

#include <boost/range/adaptor/reversed.hpp>
#include <fmt/format.h>

TRACE_SET_MOD(hhir_fsm)

namespace HPHP::jit::irgen {

namespace {

using Trace::Indent;

///////////////////////////////////////////////////////////////////////////////

/*
 * Helper that sets a value to a new value and also returns whether it changed.
 */
template<class T>
bool merge_util(T& oldVal, const T& newVal) {
  auto changed = oldVal != newVal;
  oldVal = newVal;
  return changed;
}

/*
 * Merge TypeSourceSets, returning whether anything changed.
 *
 * TypeSourceSets are merged a join points by unioning the type sources.  The
 * reason for this is that if a type is constrained, we need to be able to find
 * "all" possible sources of the type and constrain them.
 */
bool merge_into(TypeSourceSet& dst, const TypeSourceSet& src) {
  assertx(&dst != &src);
  auto initialSize = dst.size();
  dst.insert(boost::container::ordered_unique_range, src.begin(), src.end());
  return dst.size() > initialSize;
}

Type bound_type(Type type, Type limit) {
  type &= limit;
  if (!(type <= limit)) type = limit;
  return type;
}

// Merge a SSATmp and Type pair, which are meant to stay in sync
bool merge_value_and_type(SSATmp*& dstValue, SSATmp* srcValue,
                          Type& dstType, const Type& srcType) {
  auto changed = false;

  changed |= merge_util(dstType, dstType | srcType);

  // Get the least common ancestor across both states.
  changed |= merge_util(dstValue, least_common_ancestor(dstValue, srcValue));

  // Certain unions of types (involving interfaces) may widen the type
  // beyond that of the known value. Clamp the type to the known value
  // in that case. Alternately, we could drop the known value, but
  // that's usually more valuable.
  if (dstValue != nullptr && !(dstType <= dstValue->type())) {
    dstType = bound_type(dstType, dstValue->type());
    changed = true;
  }

  return changed;
}

/*
 * Merge LocationStates, returning whether anything changed.
 */
template<LTag lt, LTag rt>
bool merge_into(LocationState<lt>& dst, const LocationState<rt>& src) {
  auto changed = false;

  changed |= merge_value_and_type(dst.value, src.value, dst.type, src.type);
  changed |= merge_into(dst.typeSrcs, src.typeSrcs);

  if (!dst.maybeChanged && src.maybeChanged) {
    dst.maybeChanged = true;
    changed = true;
  }

  return changed;
}

bool merge_memory_stack_into(StackStateMap& dst, const StackStateMap& src) {
  auto changed = false;
  // Throw away any information only known in dst.
  for (auto& [dIdx, dState] : dst) {
    if (!src.contains(dIdx)) {
      dState = StackState{};
      changed = true;
    }
  }
  // Merge the information from src into dst.
  for (auto& [sIdx, sState] : src) {
    changed |= merge_into(dst[sIdx], sState);
  }
  return changed;
}

/*
 * Merge the MBase states, returning whether anything changed.
 */
bool merge_into(Optional<MBaseState>& dst, const Optional<MBaseState>& src) {
  if (!dst && !src) return false;

  auto changed = !dst;
  if (changed) dst.emplace();
  changed |= merge_into(*dst, src ? *src : MBaseState{});
  return changed;
}

/*
 * Merge one FrameState into another, returning whether it changed.  Frame
 * pointers and stack depth must match.  If the stack pointer tmps are
 * different, clear the tracked value (we can make a new one, given fp and
 * irSPOff).
 */
bool merge_into(FrameState& dst, const FrameState& src) {
  auto changed = false;

  // Cannot merge irSPOff state, so assert they match.
  always_assert(dst.irSPOff == src.irSPOff);
  always_assert(dst.curFunc == src.curFunc);

  // The only thing that can change the FP is inlining, but we can't have one
  // of the predecessors in an inlined callee while the other isn't.
  always_assert(dst.fpValue == src.fpValue);
  always_assert(dst.fixupFPValue == src.fixupFPValue);

  // We must always have the same spValue.
  always_assert(dst.spValue == src.spValue);

  // We must always have the same stublogue mode.
  always_assert(dst.stublogue == src.stublogue);

  // We must always have the same original ctx.
  always_assert(dst.origCtxType == src.origCtxType);

  changed |= merge_value_and_type(
    dst.ctx, src.ctx,
    dst.ctxType, src.ctxType
  );

  changed |= merge_value_and_type(
    dst.mbr.ptr, src.mbr.ptr,
    dst.mbr.ptrType, src.mbr.ptrType
  );
  changed |= merge_util(dst.mbr.pointee, dst.mbr.pointee | src.mbr.pointee);

  changed |= merge_into(dst.mbase, src.mbase);

  changed |= merge_value_and_type(
    dst.mTempBase.value, src.mTempBase.value,
    dst.mTempBase.type, src.mTempBase.type
  );

  changed |= merge_util(dst.mROProp, dst.mROProp | src.mROProp);

  changed |= merge_util(dst.mbaseLocalType,
                        dst.mbaseLocalType | src.mbaseLocalType);
  changed |= merge_util(dst.mbaseStackType,
                        dst.mbaseStackType | src.mbaseStackType);
  changed |= merge_util(dst.mbaseTempType,
                        dst.mbaseTempType | src.mbaseTempType);
  // If we clamped the mbase type above, we might need to clamp the
  // location specific types as well.
  auto const mbaseType = dst.mbase ? dst.mbase->type : TCell;
  if (!(dst.mbaseLocalType <= mbaseType)) {
    dst.mbaseLocalType = bound_type(dst.mbaseLocalType, mbaseType);
    changed = true;
  }
  if (!(dst.mbaseStackType <= mbaseType)) {
    dst.mbaseStackType = bound_type(dst.mbaseStackType, mbaseType);
    changed = true;
  }
  if (!(dst.mbaseTempType <= mbaseType)) {
    dst.mbaseTempType = bound_type(dst.mbaseTempType, mbaseType);
    changed = true;
  }

  // Throw away any local information only known at dst.
  for (auto& it : dst.locals) {
    auto const id = it.first;
    auto& dState = it.second;
    if (!src.locals.contains(id)) {
      dState = LocalState{};
      changed = true;
    }
  }
  // Merge the information from src into dst.
  for (auto& it : src.locals) {
    auto const srcId = it.first;
    auto const& srcState = it.second;
    changed |= merge_into(dst.locals[srcId], srcState);
  }

  changed |= merge_memory_stack_into(dst.stack, src.stack);

  changed |= merge_util(dst.stackModified,
                        dst.stackModified || src.stackModified);

  changed |= merge_util(dst.localsCleared,
                        dst.localsCleared || src.localsCleared);

  // Eval stack depth should be the same at merge points.
  always_assert(dst.bcSPOff == src.bcSPOff);

  assertx(dst.checkInvariants());
  return changed;
}

/*
 * Merge two state-stacks.  The stacks must have the same depth.  Returns
 * whether any states changed.
 */
bool merge_into(jit::vector<FrameState>& dst,
                const jit::vector<FrameState>& src) {
  always_assert(src.size() == dst.size());
  auto changed = false;
  for (auto idx = uint32_t{0}; idx < dst.size(); ++idx) {
    changed |= merge_into(dst[idx], src[idx]);
  }
  return changed;
}

///////////////////////////////////////////////////////////////////////////////

}

///////////////////////////////////////////////////////////////////////////////

FrameState::FrameState(const Func* func)
  : curFunc(func)
{}

FrameStateMgr::FrameStateMgr(const Func* func)
  : m_stack{FrameState(func)}
{}

void FrameStateMgr::update(const IRInstruction* inst) {
  ITRACE(3, "FrameStateMgr::update processing {}\n", *inst);
  Indent _i;

  if (auto const taken = inst->taken()) {
    // If CheckMROProp fails, we know the bit is false, so propagate
    // that to the taken edge.
    auto const oldMROProp = cur().mROProp;
    if (inst->is(CheckMROProp)) cur().mROProp = TriBool::No;

    /*
     * TODO(#4323657): we should make this assertion for all non-empty blocks
     * (exits in addition to catches).  It would fail right now for exit
     * traces.
     *
     * If you hit this assertion: you've created a catch block and then
     * modified tracked state, then generated a potentially exception-throwing
     * instruction using that catch block as a target.  This is not allowed.
     */
    if (debug && taken->isCatch()) {
      auto const tmp = save(taken);
      always_assert_flog(
        !tmp,
        "catch block B{} had non-matching in state",
        taken->id()
      );
    }

    // When we're building the IR, we append a conditional jump after
    // generating its target block: see emitJmpCondHelper, where we
    // call makeExit() before gen(JmpZero).  It doesn't make sense to
    // update the target block state at this point, so don't.  The
    // state doesn't have this problem during optimization passes,
    // because we'll always process the jump before the target block.
    if (taken->empty()) save(taken);

    // Restore the original value since we're going to process the
    // next edge now.
    cur().mROProp = oldMROProp;
  }

  auto const setFrameCtx = [&] (const SSATmp* fp, SSATmp* ctx) {
    if (cur().fpValue != fp) return;
    cur().ctx = ctx;
    cur().ctxType = ctx->type();
    cur().origCtxType = ctx->type();
  };

  switch (inst->op()) {
  case EnterInlineFrame: trackEnterInlineFrame(inst); break;
  case LeaveInlineFrame: trackLeaveInlineFrame(); break;
  case InlineCall:       trackInlineCall(inst); break;
  case InlineSideExit:   trackInlineSideExit(inst); break;
  case InlineSideExitSyncStack:
    assertx(cur().bcSPOff ==
            inst->extra<StackRange>()->start.to<SBInvOffset>(irSPOff()));
    cur().bcSPOff = SBInvOffset{0};
    break;
  case StFrameCtx:
    setFrameCtx(inst->src(0), inst->src(1));
    break;

  case Call: {
    assertx(cur().checkMInstrStateDead());
    auto const extra = inst->extra<Call>();
    // Remove tracked state for the slots for args and the actrec.
    uint32_t numCells = kNumActRecCells + extra->numInputs();
    for (auto i = uint32_t{0}; i < numCells; ++i) {
      setValue(stk(extra->spOffset + i), nullptr);
    }
    // Mark out parameter locations as being at least InitCell
    auto const callee = inst->src(2)->hasConstVal(TFunc)
      ? inst->src(2)->funcVal() : nullptr;
    auto const base = extra->spOffset + numCells;
    for (auto i = uint32_t{0}; i < extra->numOut; ++i) {
      auto const ty = callee && callee->takesInOutParams()
        ? irgen::callOutType(callee, i, true /* mayIntercept */)
        : TInitCell;
      setType(stk(base + i), ty);
    }
    // We consider popping an ActRec and args to be synced to memory.
    assertx(cur().bcSPOff == inst->marker().bcSPOff());
    assertx(cur().bcSPOff.offset >= numCells);
    cur().bcSPOff -= numCells;
    break;
  }

  case CallFuncEntry: {
    assertx(cur().checkMInstrStateDead());
    auto const extra = inst->extra<CallFuncEntry>();
    auto const calleePrototype = extra->calleePrototype;
    // Remove tracked state for the slots for argc and the actrec.
    uint32_t numCells = kNumActRecCells + calleePrototype->numFuncEntryInputs();
    for (auto i = uint32_t{0}; i < numCells; ++i) {
      setValue(stk(extra->spOffset + i), nullptr);
    }
    // Set the type of out parameter locations.
    auto const base = extra->spOffset + numCells;
    for (auto i = uint32_t{0}; i < calleePrototype->numInOutParams(); ++i) {
      setType(
        stk(base + i),
        irgen::callOutType(calleePrototype, i, true /* mayIntercept */)
      );
    }
    // We consider popping an ActRec and args to be synced to memory.
    assertx(cur().bcSPOff == inst->marker().bcSPOff());
    assertx(cur().bcSPOff.offset >= numCells);
    cur().bcSPOff -= numCells;
    break;
  }

  case ContEnter:
    assertx(cur().checkMInstrStateDead());
    break;

  case DefFP:
  case DefFuncEntryFP:
  case EnterFrame:
    cur().fpValue = inst->dst();
    cur().fixupFPValue = inst->dst();
    break;

  case EnterPrologue:
    cur().stublogue = true;
    break;

  case ExitPrologue:
    cur().stublogue = false;
    break;

  case RetCtrl:
    uninitStack();
    cur().fpValue = nullptr;
    cur().fixupFPValue = nullptr;
    break;

  case DefFrameRelSP:
  case DefRegSP: {
    auto const data = inst->extra<DefStackData>();
    initStack(inst->dst(), data->irSPOff, data->bcSPOff);
    break;
  }

  case LdMem:
    pointerLoad(inst->src(0), inst->dst());
    break;

  case StMem:
  case StMemMeta:
    pointerStore(inst->src(0), inst->src(1));
    break;

  case LdStk: {
    auto const offset = inst->extra<LdStk>()->offset;
    // Nearly all callers of setValue() for stack slots represent a
    // modification of the stack, so it sets stackModified. LdStk is the one
    // exception, so we compensate for that here.
    auto const oldModified = cur().stackModified;
    setValueAndSyncMBase(stk(offset), inst->dst(), true);
    cur().stackModified = oldModified;
    break;
  }

  case StStk:
  case StStkMeta:
    setValueAndSyncMBase(
      stk(inst->extra<IRSPRelOffsetData>()->offset),
      inst->src(1),
      false
    );
    break;

  case CheckType:
  case AssertType: {
    auto const oldVal = inst->src(0);
    auto const newVal = inst->dst();
    for (auto& it : cur().locals) {
      refineValue(it.second, oldVal, newVal);
    }
    for (auto& it : cur().stack) {
      refineValue(it.second, oldVal, newVal);
    }
    if (cur().ctx && canonical(cur().ctx) == canonical(inst->src(0))) {
      cur().ctx = inst->dst();
      cur().ctxType = inst->dst()->type();
    }
    // MInstrState can only be live for the current frame.
    refineMBaseValue(oldVal, newVal);
    auto const canonOldVal = canonical(oldVal);
    if (cur().mTempBase.value &&
        canonOldVal == canonical(cur().mTempBase.value)) {
      setMTempBase(newVal);
    }
    if (cur().mbr.ptr && canonOldVal == canonical(cur().mbr.ptr)) {
      setMBR(newVal, true);
    }
    break;
  }


  case AssertLoc: {
    auto const id = inst->extra<LocalId>()->locId;
    refineTypeAndSyncMBase(
      loc(id),
      inst->typeParam(),
      TypeSource::makeGuard(inst)
    );
    break;
  }

  case AssertStk:
    refineTypeAndSyncMBase(
      stk(inst->extra<IRSPRelOffsetData>()->offset),
      inst->typeParam(),
      TypeSource::makeGuard(inst)
    );
    break;

  case AssertMBase:
    refineTypeAndSyncMBase(
      Location::MBase{},
      inst->typeParam(),
      TypeSource::makeGuard(inst)
    );
    break;

  case StLoc:
  case StLocMeta:
    setValueAndSyncMBase(
      loc(inst->extra<LocalId>()->locId),
      inst->src(1),
      false
    );
    break;

  case LdLoc: {
    auto const id = inst->extra<LdLoc>()->locId;
    setValueAndSyncMBase(loc(id), inst->dst(), true);
    break;
  }

  case EndCatch:
    /*
     * Hitting this means we've messed up with syncing the stack in a catch
     * trace.  If the stack isn't clean or doesn't match the marker's irSPOff,
     * the unwinder won't see what we expect.
     */
    always_assert_flog(
      inst->extra<EndCatch>()->offset.to<SBInvOffset>(cur().irSPOff) ==
        inst->marker().bcSPOff(),
      "EndCatch stack didn't seem right:\n"
      "                 spOff: {}\n"
      "       EndCatch offset: {}\n"
      "        marker's spOff: {}\n",
      cur().irSPOff.offset,
      inst->extra<EndCatch>()->offset.offset,
      inst->marker().bcSPOff().offset
    );
    break;

  case InterpOne:
  case InterpOneCF: {
    auto const& extra = *inst->extra<InterpOneData>();
    assertx(!extra.smashesAllLocals || extra.nChangedLocals == 0);
    if (extra.smashesAllLocals) {
      clearLocals();
    } else {
      auto it = extra.changedLocals;
      auto const end = it + extra.nChangedLocals;
      for (; it != end; ++it) {
        auto& local = *it;
        setType(loc(local.id), local.type);
      }
    }

    // Offset of the bytecode stack top relative to the IR stack pointer.
    auto const bcSPOff = extra.spOffset;

    // Clear tracked information for slots pushed and popped.
    for (auto i = uint32_t{0}; i < extra.cellsPopped; ++i) {
      setValue(stk(bcSPOff + i), nullptr);
    }
    for (auto i = uint32_t{0}; i < extra.cellsPushed; ++i) {
      setValue(stk(bcSPOff + extra.cellsPopped - 1 - i), nullptr);
    }
    auto adjustedTop = bcSPOff + extra.cellsPopped - extra.cellsPushed;

    switch (extra.opcode) {
      case Op::CGetL2:
        setType(stk(adjustedTop + 1), inst->typeParam());
        break;
      default:
        // We don't track cells pushed by interp one except the top of the
        // stack, aside from the above special cases.
        if (inst->hasTypeParam()) {
          auto const instrInfo = getInstrInfo(extra.opcode);
          if (instrInfo.out & InstrFlags::Stack1) {
            setType(stk(adjustedTop), inst->typeParam());
          }
        }
        break;
    }

    cur().bcSPOff += extra.cellsPushed;
    cur().bcSPOff -= extra.cellsPopped;
    assertx(cur().bcSPOff.offset >= 0);

    // Be conservative and drop minstr state
    clearMInstr();
    break;
  }

  case LdMBase:
    setMBR(inst->dst(), true);
    break;

  case StMBase:
    setMBR(inst->src(0), false);
    break;

  case StMROProp:
    cur().mROProp = inst->src(0)->hasConstVal()
      ? yesOrNo(inst->src(0)->boolVal())
      : TriBool::Maybe;
    break;

  case CheckMROProp:
    cur().mROProp = TriBool::Yes;
    break;

  case FinishMemberOp:
    clearMInstr();
    break;

  case CallBuiltin:
    // CallBuiltin uses the same memory as mTempBase, so it had better
    // be dead.
    assertx(cur().checkMInstrStateDead());
    break;

  case PropX:
  case PropDX:
  case PropQ:
    cur().mROProp |= TriBool::Yes;
    if (!inst->src(2)->isA(TNullptr)) {
      assertx(inst->src(2)->isA(TPtrToMISTemp));
      pointerStore(inst->src(2), nullptr);
    }
    break;

  default:
    // Use precise Minstr effects if we can
    if (hasMInstrBaseEffects(*inst)) {
      handleMInstr(inst);
    } else {
      // Handle everything else conservatively according to its memory
      // effects
      handleConservatively(inst);
    }
    break;
  }

  assertx(checkInvariants());
}

// Handle instructions which modify tracked state in a way we can
// track more precisely than handleConservatively.
void FrameStateMgr::handleMInstr(const IRInstruction* inst) {
  auto const basePtr = inst->src(0);
  assertx(basePtr->isA(TLval));

  auto const acls = canonicalize(pointee(basePtr));

  ITRACE(4, "FrameStateMgr::handleMInstr: {}\n", jit::show(acls));
  Indent _i;

  // mInstrBaseEffects tells us what the base will become after this
  // instruction. We need to apply it to any tracked location which
  // might intersect with the Lval passed to the instruction:

  auto const update = [&] (Location l, bool widen) {
    if (auto const u = mInstrBaseEffects(*inst, typeOf(l))) {
      widen ? widenType(l, *u) : setType(l, *u);
    }
  };

  if (auto const updateMBase = isMBase(basePtr, acls);
      updateMBase != TriBool::No) {
    auto const widen = updateMBase == TriBool::Maybe;
    update(Location::MBase{}, widen);

    // Adjust the mbase location specific types as well:
    auto const spec = [&] (Type& t) {
      auto const u = mInstrBaseEffects(*inst, t);
      if (!u) return;
      t = widen ? (t | *u) : *u;
    };
    if (acls.maybe(ALocalAny))        spec(cur().mbaseLocalType);
    if (acls.maybe(AStackAny))        spec(cur().mbaseStackType);
    if (acls.maybe(AMIStateTempBase)) spec(cur().mbaseTempType);
  }

  // If it's a single location, we can change the known type to that
  // type precisely.
  if (acls.isSingleLocation()) {
    if (auto const l = acls.is_local()) {
      update(loc(l->ids.singleValue()), false);
    } else if (auto const s = acls.is_stack()) {
      assertx(s->size() == 1);
      if (auto const l = optStk(s->low)) update(*l, false);
    } else if (acls <= AMIStateTempBase) {
      if (auto const u = mInstrBaseEffects(*inst, mTempBase().type)) {
        setMTempBaseType(*u);
      }
    }
  } else {
    // Otherwise it might be one of multiple locations, so we need to
    // widen in the type (not replace).
    if (acls.maybe(ALocalAny)) {
      for (auto const& local : cur().locals) {
        if (!acls.maybe(ALocal{fp(), local.first})) continue;
        update(loc(local.first), true);
      }
      // This instruction could also affect locals that aren't being
      // tracked. Be conservative and assume that they could be
      // affected.
      cur().localsCleared = true;
    }
    if (acls.maybe(AStackAny)) {
      for (auto const& stack : cur().stack) {
        auto const offset = stack.first.to<IRSPRelOffset>(irSPOff());
        if (!acls.maybe(AStack::at(offset))) continue;
        update(stk(stack.first), true);
      }
    }
    if (acls.maybe(AMIStateTempBase)) {
      if (auto const u = mInstrBaseEffects(*inst, mTempBase().type)) {
        widenMTempBase(*u);
      }
    }
  }
}

// Update tracked state conservatively according to the instruction's
// memory effects.
void FrameStateMgr::handleConservatively(const IRInstruction* inst) {
  auto const store = [&] (const AliasClass& stores) {
    if (stores.maybe(AMIStateBase)) {
      setMBR(nullptr, false);
    } else if (isMBase(nullptr, stores) != TriBool::No) {
      setValue(Location::MBase{}, nullptr);
      if (stores.maybe(ALocalAny))        cur().mbaseLocalType = TCell;
      if (stores.maybe(AStackAny))        cur().mbaseStackType = TCell;
      if (stores.maybe(AMIStateTempBase)) cur().mbaseTempType  = TCell;
    }

    if (stores.maybe(ALocalAny)) {
      for (auto const& local : cur().locals) {
        if (!stores.maybe(ALocal{fp(), local.first})) continue;
        setValue(loc(local.first), nullptr);
      }
      // This instruction could also affect locals that aren't being
      // tracked. Be conservative and assume that they could be
      // affected.
      cur().localsCleared = true;
    }
    if (stores.maybe(AStackAny)) {
      for (auto const& stack : cur().stack) {
        auto const offset = stack.first.to<IRSPRelOffset>(irSPOff());
        if (!stores.maybe(AStack::at(offset))) continue;
        setValue(stk(stack.first), nullptr);
      }
    }
    if (stores.maybe(AMIStateTempBase)) setMTempBase(nullptr);
    if (stores.maybe(AMIStateROProp))   cur().mROProp = TriBool::Maybe;
  };

  auto const effects = memory_effects(*inst);

  ITRACE(4, "FrameStateMgr::handleConservatively: {}\n", jit::show(effects));
  Indent _i;

  match(
    effects,
    [&] (const GeneralEffects& x) {
      store(x.stores);
      store(x.inout);
    },
    [&] (const CallEffects& x) {
      store(x.actrec);
      store(x.outputs);
    },
    [&] (const PureStore& x)       { store(x.dst); },
    [&] (const PureInlineCall& x)  { store(x.base); },
    [&] (const UnknownEffects&)    { store(AUnknown); },
    [&] (const ReturnEffects&)     {},
    [&] (const ExitEffects&)       {},
    [&] (const PureLoad&)          {},
    [&] (const IrrelevantEffects&) {}
  );
}

///////////////////////////////////////////////////////////////////////////////

/*
 * Collects the post-conditions associated with the current state, which is
 * essentially a list of local/stack locations and their known types at the end
 * of a block.
 */
PostConditions FrameStateMgr::collectPostConds() {
  PostConditions postConds;

  if (sp() != nullptr) {
    for (auto& it : cur().stack) {
      auto const sbRel = it.first;
      auto& state = it.second;
      auto const type = state.type;
      auto const changed = state.maybeChanged;

      if (changed || type < TCell) {
        ITRACE(1, "Stack({}, {}): {} ({})\n",
               sbRel.to<BCSPRelOffset>(bcSPOff()).offset, sbRel.offset,
               type, changed ? "changed" : "refined");
        auto& vec = changed ? postConds.changed : postConds.refined;
        vec.push_back({ Location::Stack{sbRel}, type });
      }
    }
  }

  if (fp() != nullptr) {
    for (auto& l : cur().locals) {
      auto id = l.first;
      auto& state = l.second;
      auto const type = state.type;
      auto const changed = state.maybeChanged;
      if (changed || type < TCell) {
        ITRACE(1, "Local {}: {} ({})\n", id, type.toString(),
               changed ? "changed" : "refined");
        auto& vec = changed ? postConds.changed : postConds.refined;
        vec.push_back({ Location::Local{id}, type });
      }
    }
  }

  auto const ty = mbase().type;
  auto const changed = mbase().maybeChanged;
  if (changed || ty < TCell) {
    ITRACE(1, "MBase{{}}: {} ({})\n", ty, changed ? "changed" : "refined");
    auto& vec = changed ? postConds.changed : postConds.refined;
    vec.push_back({ Location::MBase{}, ty });
  }

  return postConds;
}

///////////////////////////////////////////////////////////////////////////////
// Per-block state.

bool FrameStateMgr::hasStateFor(Block* block) const {
  return m_states.contains(block);
}

void FrameStateMgr::startBlock(Block* block, bool hasUnprocessedPred) {
  ITRACE(3, "FrameStateMgr::startBlock: {}\n", block->id());
  auto const it = m_states.find(block);
  auto const end = m_states.end();

  if (it != end) {
    always_assert_flog(
      block->empty(),
      "tried to startBlock a non-empty block while building"
    );
    m_stack = it->second.in;
    ITRACE(4, "Loading state for B{}:\n{}\n", block->id(), show());
    always_assert_flog(
      !m_stack.empty(),
      "invalid startBlock for B{}",
      block->id()
    );
  } else if (debug) {
    // NOTE: Highly suspect; different debug vs. non-debug behavior.
    save(block);
  }
  assertx(!m_stack.empty());

  // Reset state if the block has any predecessor that we haven't processed yet.
  if (hasUnprocessedPred) {
    Indent _;
    ITRACE(4, "B{} is a loop header; resetting state\n", block->id());
    clearForUnprocessedPred();

    // pointee() won't do the right thing if a DefLabel isn't fully
    // formed
    always_assert_flog(
      block->empty() || !block->front().is(DefLabel),
      "B{} has a DefLabel with an unprocessed pred. Frame-state can't "
      "handle this, and it shouldn't happen with how we do irgen.",
      block->id()
    );
  }
}

bool FrameStateMgr::finishBlock(Block* block) {
  assertx(block->back().isTerminal() == !block->next());

  if (block->isExitNoThrow()) {
    m_exitPostConds[block] = collectPostConds();
    ITRACE(2, "PostConditions for exit Block {}:\n{}\n",
           block->id(), jit::show(m_exitPostConds[block]));
  }

  assertx(hasStateFor(block));

  auto changed = false;
  if (!block->back().isTerminal()) changed |= save(block->next());
  return changed;
}

void FrameStateMgr::pauseBlock(Block* block) {
  // Note: this can't use std::move, because pauseBlock must leave the current
  // state alone so startBlock can use it as the in state for another block.
  m_states[block].paused = m_stack;
}

void FrameStateMgr::unpauseBlock(Block* block) {
  assertx(hasStateFor(block));
  m_stack = *m_states[block].paused;
}

const PostConditions& FrameStateMgr::postConds(Block* exitBlock) const {
  assertx(exitBlock->isExitNoThrow());
  auto it = m_exitPostConds.find(exitBlock);
  assertx(it != m_exitPostConds.end());
  auto& pconds = it->second;
  if (debug) {
    for (DEBUG_ONLY auto& c : pconds.changed) {
      for (DEBUG_ONLY auto& r : pconds.refined) {
        assert_flog(c.location != r.location,
                    "Location {} in both changed and refined sets",
                    jit::show(c.location));
      }
    }
  }
  return pconds;
}

/*
 * Save the current state as the in-state for `block'.
 *
 * If this is the first time saving state for `block', create a new snapshot
 * using either the current state or the out-state of `pred' if we are given
 * one.  Otherwise merge the current state into the existing snapshot.
 */
bool FrameStateMgr::save(Block* block) {
  ITRACE(4, "Saving current state to B{}:\n{}\n", block->id(), show());

  // If the destination block is unreachable, there's no need to merge in the
  // frame state.
  if (block->isUnreachable()) return false;

  auto const it = m_states.find(block);
  auto changed = true;

  if (it != m_states.end()) {
    changed = merge_into(it->second.in, m_stack);
    ITRACE(4, "Merged state:\n{}\n", show());
  } else {
    assertx(!m_stack.empty());
    m_states[block].in = m_stack;
  }

  return changed;
}

/*
 * Modify state to conservative values given an unprocessed predecessor.
 *
 * The fpValue, irSPOff, and curFunc are not cleared because they must agree
 * at bytecode-level control-flow merge points (which can be either merge
 * points at the bytecode or due to retranslated blocks).
 */
void FrameStateMgr::clearForUnprocessedPred() {
  ITRACE(1, "clearForUnprocessedPred\n");

  // Forget any information about stack values in memory.
  for (auto& it : cur().stack) {
    it.second = StackState{};
  }

  // These values must go toward their conservative state.
  clearCtx();
  clearLocals();
  clearMInstr();
}

/*
 * Modify state to conservative values, allowing the current exception handler
 * block to be shared.
 */
void FrameStateMgr::clearForEH() {
  ITRACE(1, "clearForEH\n");

  // Forget any non-trivial information about stack values in memory.
  for (auto& it : cur().stack) {
    auto const value = it.second.value;
    it.second = StackState{};
    it.second.value = value;
    if (it.second.value != nullptr) it.second.type = value->type();
  }

  // These values must go toward their conservative state.
  clearCtx();
  clearLocals();
  clearMInstr();
}

///////////////////////////////////////////////////////////////////////////////

void FrameStateMgr::initStack(SSATmp* sp, SBInvOffset irSPOff,
                              SBInvOffset bcSPOff) {
  assertx(bcSPOff.offset >= 0);
  cur().spValue = sp;
  cur().irSPOff = irSPOff;
  cur().bcSPOff = bcSPOff;
  cur().stack.clear();
}

void FrameStateMgr::uninitStack() {
  cur().spValue = nullptr;
  cur().irSPOff = SBInvOffset{0};
  cur().bcSPOff = SBInvOffset{0};
  cur().stack.clear();
}

void FrameStateMgr::trackEnterInlineFrame(const IRInstruction* inst) {
  assertx(inst->is(EnterInlineFrame));
  assertx(cur().checkMInstrStateDead());

  auto const extra = inst->src(0)->inst()->extra<DefCalleeFP>();
  auto const callee = extra->func;
  auto const spOffset = extra->spOffset;
  assertx(cur().bcSPOff == spOffset.to<SBInvOffset>(irSPOff()));

  // Remove space from callee's frame from the caller's stack.
  for (auto i = uint32_t{0}; i < kNumActRecCells; ++i) {
    setValue(stk(spOffset + i), nullptr);
  }
  assertx(cur().bcSPOff.offset >= kNumActRecCells);
  cur().bcSPOff -= kNumActRecCells;

  if (callee->isCPPBuiltin()) {
    auto const inout = callee->numInOutParams();
    for (auto i = uint32_t{0}; i < inout; ++i) {
      auto const type = irgen::callOutType(callee, i, false /* mayIntercept */);
      setType(stk(extra->spOffset + kNumActRecCells + i), type);
    }
  }

  // Push a new state for the inlined callee; saving the state we'll need to
  // pop on return.
  m_stack.emplace_back(FrameState{callee});

  // Set up the callee's frame.
  cur().fpValue = inst->src(0);
  cur().fixupFPValue = caller().fixupFPValue;

  /*
   * Set up the callee's stack.
   *
   * We need to calculate the new `irSPOff`, which is an offset of `spValue`
   * from the new stack base. It consists of two parts:
   *
   * - the inverse offset of the new `fpValue` from the new stack base, given by
   *   `-callee->numSlotsInFrame()`
   * - the inverse offset of `spValue` from the new `fpValue`, which is the same
   *   numeric value as a regular offset of `fpValue` from `spValue`, given by
   *   `spOffset`
   *
   * The callee's stack starts empty, so `bcSPOff` is zero.
   */
  auto const irSPOff = SBInvOffset{spOffset.offset - callee->numSlotsInFrame()};
  auto const bcSPOff = SBInvOffset{0};
  initStack(caller().spValue, irSPOff, bcSPOff);
  ITRACE(6, "DefCalleeFP setting irSPOff: {}\n", irSPOff.offset);
}

void FrameStateMgr::trackLeaveInlineFrame() {
  // LeaveInlineFrame is not allowed after InlineCall
  assertx(caller().fixupFPValue == cur().fixupFPValue);

  // Inlining may not change spValue
  assertx(caller().spValue == cur().spValue);

  // Pop the inlined frame.
  m_stack.pop_back();
  assertx(!m_stack.empty());
  assertx(cur().checkMInstrStateDead());
}

void FrameStateMgr::trackInlineCall(const IRInstruction* inst) {
  assertx(cur().fixupFPValue == inst->src(1));
  cur().fixupFPValue = inst->src(0);
}

void FrameStateMgr::trackInlineSideExit(const IRInstruction* inst) {
  auto const callee = inst->extra<InlineSideExit>()->callee;
  auto const arOffset = inst->src(1)->inst()->extra<DefCalleeFP>()->spOffset;

  // The callee frame was already popped from the frame state. The only thing
  // left is to set the type of out parameter locations.
  auto const base = arOffset + kNumActRecCells;
  for (auto i = uint32_t{0}; i < callee->numInOutParams(); ++i) {
    auto const type = irgen::callOutType(callee, i, false /* mayIntercept */);
    setType(stk(base + i), type);
  }

  assertx(cur().bcSPOff == base.to<SBInvOffset>(irSPOff()));
}

///////////////////////////////////////////////////////////////////////////////

bool FrameState::checkInvariants() const {
  for (auto const& it : locals) {
    auto const id = it.first;
    auto const& local = it.second;
    always_assert_flog(
      local.value == nullptr || local.type <= local.value->type(),
      "local {} had incompatible type {} with value {}",
      id,
      local.type,
      local.value->toString()
    );
  }
  for (auto const& it : stack) {
    auto const id = it.first;
    auto const& stk = it.second;
    always_assert_flog(
      stk.value == nullptr || stk.type <= stk.value->type(),
      "stack {} had incompatible type {} with value {}",
      id.offset,
      stk.type,
      stk.value->toString()
    );
  }

  auto const mbase = this->mbase.value_or(MBaseState{});
  always_assert_flog(
    mbase.value == nullptr || mbase.type <= mbase.value->type(),
    "mbase had incompatible type {} with value {}",
    mbase.type,
    mbase.value->toString()
  );
  always_assert_flog(
    mbr.ptr == nullptr || mbr.ptrType <= mbr.ptr->type(),
    "MBR had incompatible type {} with value {}",
    mbr.ptrType,
    mbr.ptr->toString()
  );
  always_assert_flog(
    mbr.ptrType <= TLval,
    "MBR contains a {}, not a Lval like it should",
    mbr.ptrType
  );
  always_assert_flog(
    mbr.pointee <= AUnknownTV,
    "MBR's pointee {} is wider than AUnknownTV",
    jit::show(mbr.pointee)
  );
  always_assert_flog(
    mbr.ptr == nullptr || mbr.pointee <= canonicalize(pointee(mbr.ptr->type())),
    "MBR's pointee {} is wider than it's ptr implies {}",
    jit::show(mbr.pointee),
    jit::show(canonicalize(pointee(mbr.ptr->type())))
  );
  always_assert_flog(
    mbaseStackType <= mbase.type,
    "mbase's stack-only type {} is wider than it's general type {}",
    mbaseStackType,
    mbase.type
  );
  always_assert_flog(
    mbaseLocalType <= mbase.type,
    "mbase's local-only type {} is wider than it's general type {}",
    mbaseLocalType,
    mbase.type
  );
  always_assert_flog(
    mbaseTempType <= mbase.type,
    "mbase's temp-base-only type {} is wider than it's general type {}",
    mbaseTempType,
    mbase.type
  );
  always_assert_flog(
    IMPLIES(!mbr.pointee.maybe(AStackAny), mbaseStackType == TBottom),
    "mbase has a stack-only type {} when it can't be the stack",
    mbaseStackType
  );
  always_assert_flog(
    IMPLIES(!mbr.pointee.maybe(ALocalAny), mbaseLocalType == TBottom),
    "mbase has a local-only type {} when it can't be a local",
    mbaseLocalType
  );
  always_assert_flog(
    IMPLIES(!mbr.pointee.maybe(AMIStateTempBase), mbaseTempType == TBottom),
    "mbase has a temp-base-only type {} when it can't be the MTempBase",
    mbaseTempType
  );

  if (auto const l = mbr.pointee.is_local()) {
    if (l->ids.hasSingleValue()) {
      auto const localId = l->ids.singleValue();
      if (auto const it = locals.find(localId); it != locals.end()) {
        auto const& state = it->second;
        always_assert_flog(
          mbase.value == state.value &&
          mbase.type == state.type,
          "MBase and local {} do not agree on state ({} {} != {} {})",
          localId,
          mbase.value ? mbase.value->toString() : "<>",
          mbase.type,
          state.value ? state.value->toString() : "<>",
          state.type
        );
        always_assert_flog(
          mbase.type == mbaseLocalType,
          "mbase has an incompatible local-only type ({} and {}) "
          "when it must be a local",
          mbase.type,
          mbaseLocalType
        );
      }
    }
  } else if (auto const s = mbr.pointee.is_stack()) {
    if (s->size() == 1) {
      auto const offset = s->low.to<SBInvOffset>(irSPOff);
      if (auto const it = stack.find(offset); it != stack.end()) {
        auto const& state = it->second;
        always_assert_flog(
          mbase.value == state.value &&
          mbase.type == state.type,
          "MBase and stack {} do not agree on state ({} {} != {} {})",
          offset.offset,
          mbase.value ? mbase.value->toString() : "<>",
          mbase.type,
          state.value ? state.value->toString() : "<>",
          state.type
        );
        always_assert_flog(
          mbase.type == mbaseStackType,
          "mbase has an incompatible stack-only type ({} and {}) "
          "when it must be a stack slot",
          mbase.type,
          mbaseStackType
        );
      }
    }
  } else if (mbr.pointee <= AMIStateTempBase) {
    always_assert_flog(
      mbase.value == mTempBase.value &&
      mbase.type == mTempBase.type,
      "MBase and MTempBase do not agree on state ({} {} != {} {})",
      mbase.value ? mbase.value->toString() : "<>",
      mbase.type,
      mTempBase.value ? mTempBase.value->toString() : "<>",
      mTempBase.type
    );
    always_assert_flog(
      mbase.type == mbaseTempType,
      "mbase has an incompatible temp-base-only type ({} and {}) "
      "when it must be the MTempBase",
      mbase.type,
      mbaseTempType
    );
  }

  // Make sure the stack is either initialized or read-only empty.
  always_assert_flog(
    spValue != nullptr || (irSPOff.offset == 0 && bcSPOff.offset == 0),
    "incorrectly initialized stack"
  );

  return true;
}

bool FrameStateMgr::checkInvariants() const {
  for (auto const& state : m_stack) {
    // Every state except the current one should have a dead minstr
    // state.
    if (&state != &m_stack.back()) always_assert(state.checkMInstrStateDead());
    always_assert(state.checkInvariants());
  }
  return true;
}

bool FrameState::checkMInstrStateDead() const {
  always_assert_flog(
    !mbr.ptr && mbr.ptrType == TLval && !(mbr.pointee < AUnknownTV),
    "MBR is not dead when it should be ({} {} {})",
    mbr.ptr ? mbr.ptr->toString() : "<>",
    mbr.ptrType.toString(),
    jit::show(mbr.pointee)
  );

  auto const mbase = this->mbase.value_or(MBaseState{});
  always_assert_flog(
    !mbase.value && mbase.type == TCell,
    "MBase is not dead when it should be ({} {})",
    mbase.value ? mbase.value->toString() : "<>",
    mbase.type.toString()
  );
  always_assert_flog(
    !mTempBase.value && mTempBase.type == TCell,
    "MTempBase is not dead when it should be ({} {})",
    mTempBase.value ? mTempBase.value->toString() : "<>",
    mTempBase.type.toString()
  );
  always_assert_flog(
    mbaseStackType == TCell &&
    mbaseLocalType == TCell &&
    mbaseTempType == TCell,
    "MBase has location specific types ({} {} {}) when it should be dead",
    mbaseStackType,
    mbaseLocalType,
    mbaseTempType
  );
  always_assert_flog(
    mROProp == TriBool::Maybe,
    "MROProp has a known value {} when it should be dead",
    HPHP::show(mROProp)
  );
  return true;
}

///////////////////////////////////////////////////////////////////////////////

/*
 * Wrap a local or stack ID into a Location.
 */
Location FrameStateMgr::loc(uint32_t id) const {
  return Location::Local { id };
}
Location FrameStateMgr::stk(SBInvOffset off) const {
  return Location::Stack { off };
}
Location FrameStateMgr::stk(IRSPRelOffset off) const {
  auto const sbRel = off.to<SBInvOffset>(irSPOff());
  return Location::Stack { sbRel };
}

Optional<Location> FrameStateMgr::optStk(IRSPRelOffset off) const {
  return optStk(off.to<SBInvOffset>(irSPOff()));
}
Optional<Location> FrameStateMgr::optStk(SBInvOffset off) const {
  if (off.offset < 1) return std::nullopt;
  return Location::Stack { off };
}

LocalState& FrameStateMgr::localState(uint32_t id) {
  assertx(id < cur().curFunc->numLocals());
  return cur().locals[id];
}

LocalState& FrameStateMgr::localState(Location l) {
  assertx(l.tag() == LTag::Local);
  return localState(l.localId());
}

StackState& FrameStateMgr::stackState(IRSPRelOffset spRel) {
  auto const sbRel = spRel.to<SBInvOffset>(irSPOff());
  return stackState(sbRel);
}

StackState& FrameStateMgr::stackState(SBInvOffset sbRel) {
  always_assert_flog(
    sbRel.offset >= 1,
    "stack sbRel.offset went below 1: irSPOff: {}, sbRel: {}\n",
    cur().irSPOff.offset,
    sbRel.offset
  );
  return cur().stack[sbRel];
}

StackState& FrameStateMgr::stackState(Location l) {
  assertx(l.tag() == LTag::Stack);
  return stackState(l.stackIdx());
}

MBaseState& FrameStateMgr::mbaseState() {
  auto& mbase = cur().mbase;
  if (!mbase) mbase.emplace();
  return *mbase;
}

LocalState FrameStateMgr::local(uint32_t id) const {
  auto const& locals = cur().locals;
  auto const it = locals.find(id);
  return it != locals.end() ? it->second : LocalState{};
}

bool FrameStateMgr::tracked(Location l) const {
  switch (l.tag()) {
    case LTag::Local:
      return cur().locals.contains(l.localId());
    case LTag::Stack: {
      auto const sbRel = l.stackIdx();
      return cur().stack.contains(sbRel);
    }
    case LTag::MBase:
      return cur().mbase.has_value();
  }
  not_reached();
}

bool FrameStateMgr::validStackOffset(IRSPRelOffset off) const {
  return optStk(off).has_value();
}

void FrameStateMgr::recoverLocal(const std::vector<Block*>& from, uint32_t id) {
  assertx(!from.empty());

  Type t = TBottom;

  for (auto const block : from) {
    auto const sit = m_states.find(block);
    assertx(sit != m_states.end());
    auto const& inState = sit->second.in.back();

    // If we don't know anything about the local in one of the paths,
    // it might be anything.
    auto const lit = inState.locals.find(id);
    if (lit == inState.locals.end()) return;

    t |= lit->second.type;
  }

  // At least one path from the immediate dominator to the current block had to
  // be reachable to emit the current block.
  assertx(t != TBottom);

  ITRACE(2, "recovering local {}: {}\n", id, t);
  refineType(loc(id), t, std::nullopt);
}

/*
 * We consider it logically const to extend with default-constructed stack
 * values.
 */
StackState FrameStateMgr::stack(IRSPRelOffset offset) const {
  auto const sbRel = offset.to<SBInvOffset>(irSPOff());
  return stack(sbRel);
}

StackState FrameStateMgr::stack(SBInvOffset offset) const {
  auto const& curStack = cur().stack;
  auto const it = curStack.find(offset);
  return it != curStack.end() ? it->second : StackState{};
}

MBaseState FrameStateMgr::mbase() const {
  return cur().mbase.value_or(MBaseState{});
}

#define IMPL_MEMBER_OF(type_t, name)                           \
  type_t FrameStateMgr::name##Of(Location l) const {           \
    return [&]() -> type_t {                                   \
      switch (l.tag()) {                                       \
        case LTag::Local:    return local(l.localId()).name;   \
        case LTag::Stack:    return stack(l.stackIdx()).name;  \
        case LTag::MBase:    return mbase().name;              \
      }                                                        \
      not_reached();                                           \
    }();                                                       \
  }

IMPL_MEMBER_OF(SSATmp*, value)
IMPL_MEMBER_OF(Type, type)
IMPL_MEMBER_OF(TypeSourceSet, typeSrcs)

#undef IMPL_MEMBER_AT

///////////////////////////////////////////////////////////////////////////////

/*
 * Conservatively calculate the type of a location solely from the
 * AliasClass.
 */
Type FrameStateMgr::typeFromAliasClass(const AliasClass& acls) const {
  // If it's a single one of our tracked locations, we can use what we
  // know of that location.
  if (auto const l = acls.is_local()) {
    if (l->ids.hasSingleValue()) return typeOf(loc(l->ids.singleValue()));
  } else if (auto const s = acls.is_stack()) {
    if (s->size() == 1) {
      if (auto const l = optStk(s->low)) return typeOf(*l);
    }
  } else if (acls <= AMIStateTempBase) {
    return cur().mTempBase.type;
  }
  // Otherwise we don't know
  return TCell;
}

/*
 * Calculate the type off a pointer's pointee using it's
 * definitions. If the calculated type exceeds `limit`, stop and
 * return the type so far.
 */
Type FrameStateMgr::typeOfPointeeFromDefs(SSATmp* ptr, Type limit) const {
  assertx(ptr->isA(TMem));

  // Visit every defining instruction, and use what we know of that
  // particular instruction to calculate the type. They all get
  // unioned together.
  auto t = TBottom;
  auto const visit = [&] (const IRInstruction* inst, const SSATmp*) {
    t |= [&] {
      switch (inst->op()) {
      // Use our tracked state for these:
      case LdLocAddr:
        return typeOf(loc(inst->extra<LdLocAddr>()->locId));
      case LdStkAddr:
        return typeOf(stk(inst->extra<LdStkAddr>()->offset));
      case LdMIStateTempBaseAddr:
        return cur().mTempBase.type;
      case LdMBase:
        // Laundering the pointer through the MBR destroys our def
        // information. Be conservative and use what we know from the
        // alias classes.
        return typeFromAliasClass(inst->extra<LdMBase>()->acls);
      case DefConst: {
        auto const constTy = inst->typeParam();
        assertx(constTy.hasConstVal(TMem));
        auto const p = constTy.ptrVal();
        // It's tempting to just dereference the constant ptr and get
        // it's type. However there's no guarantee it's type is
        // constant, nor even that the pointer is even
        // dereferencable. Instead compare the address with known
        // constants.
        if (p == &immutable_null_base)   return TInitNull;
        if (p == &immutable_uninit_base) return TUninit;
        return TCell;
      }
      case LdPropAddr:
      case LdInitPropAddr:
      case LdClsPropAddrOrNull:
      case LdClsPropAddrOrRaise:
        // These types are invariant and calculated when the IR op is
        // emitted.
        assertx(inst->typeParam() <= TCell);
        return inst->typeParam();
      case LdRDSAddr:
      case LdInitRDSAddr:
        // These too are calculated when emitted (and won't change).
        return inst->extra<RDSHandleAndType>()->type;
      case ElemDictD:
      case ElemDictU:
      case BespokeElem: {
        // These can be calculated depending on the inputs. The type
        // param tells us the known type of the base (which is
        // provided via pointer).
        assertx(inst->typeParam() <= TArrLike);
        auto elem = arrLikeElemType(
          inst->typeParam(),
          inst->src(1)->type(),
          inst->ctx()
        );
        if (!elem.second) {
          if (inst->is(ElemDictU) ||
              (inst->is(BespokeElem) && !inst->src(2)->boolVal())) {
            elem.first |= TInitNull;
          }
        }
        assertx(elem.first != TBottom);
        return elem.first;
      }
      case ElemDictK:
        // These require no type params as there's no pointers
        // involved.
        return arrLikePosType(
          inst->src(3)->type(),
          inst->src(2)->type(),
          false,
          inst->ctx()
        );
      case LdVecElemAddr:
        return arrLikeElemType(
          inst->src(2)->type(),
          inst->src(1)->type(),
          inst->ctx()
        ).first;
      case StructDictElemAddr: {
        auto const arrType = inst->src(3)->type();
        auto elem = arrLikeElemType(
          arrType,
          inst->src(1)->type(),
          inst->ctx()
        );
        auto const& layout = arrType.arrSpec().layout();
        if (!elem.second && !layout.slotAlwaysPresent(inst->src(2)->type())) {
          elem.first |= TUninit;
        }
        return elem.first;
      }
      default:
        // Otherwise something we can't say anything about.
        return TCell;
      }
    }();
    return t < limit;
  };
  visitEveryDefiningInst(ptr, visit);
  return t;
}

Type FrameStateMgr::typeOfPointee(SSATmp* ptr, Type limit) const {
  assertx(ptr->isA(TMem));
  auto const acls = canonicalize(pointee(ptr));
  if (isMBase(ptr, acls) == TriBool::Yes) return mbase().type;
  return typeOfPointeeFromDefs(ptr, limit);
}

/*
 * Return any SSATmp representing the value of the given pointer's
 * pointee.
 */
SSATmp* FrameStateMgr::valueOfPointee(SSATmp* ptr) const {
  assertx(ptr->isA(TMem));
  auto const acls = canonicalize(pointee(ptr));
  if (isMBase(ptr, acls) == TriBool::Yes) return mbase().value;

  if (!acls.isSingleLocation()) return nullptr;
  if (auto const l = acls.is_local()) {
    assertx(l->ids.hasSingleValue());
    return valueOf(loc(l->ids.singleValue()));
  } else if (auto const s = acls.is_stack()) {
    assertx(s->size() == 1);
    if (auto const l = optStk(s->low)) return valueOf(*l);
  } else if (acls <= AMIStateTempBase) {
    return cur().mTempBase.value;
  }
  return nullptr;
}

///////////////////////////////////////////////////////////////////////////////

AliasClass FrameStateMgr::locationToAliasClass(Location l) const {
  switch (l.tag()) {
    case LTag::Local:
      return ALocal{canonical(fp()), l.localId()};
    case LTag::Stack:
      return AStack::at(l.stackIdx().to<IRSPRelOffset>(irSPOff()));
    case LTag::MBase:
      return AMIStateTempBase;
  }
  not_reached();
}

// Determine if the given TMem/AliasClass might be the current
// MBase. Ptr is optional. If given we can do more precise (and
// faster) checks.
TriBool FrameStateMgr::isMBase(SSATmp* ptr, const AliasClass& acls) const {
  if (ptr) {
    // If the ptr is the known mbr, then it obviously is the mbase. If
    // the two pointers aren't even compatible, it can't be.
    assertx(ptr->isA(TMem));
    if (canonical(ptr) == canonical(mbr().ptr)) return TriBool::Yes;
    // NB: One could be a Ptr and the other a Lval, so don't use type
    // comparison. Compare the locations directly.
    if (!ptr_location_t(ptr->type().ptrLocation() &
                        mbr().ptrType.ptrLocation())) {
      return TriBool::No;
    }
  }
  // If their known alias classes don't even overlap, they can't be
  // the same.
  if (!acls.maybe(mbr().pointee)) return TriBool::No;

  // If the known types at both locations are disjoint, they can't be
  // the same.
  auto const knownType = [&] {
    if (ptr) return typeOfPointeeFromDefs(ptr, TCell);
    return typeFromAliasClass(acls);
  }();
  auto const mbrType = [&] {
    if (acls <= (ALocalAny|AStackAny|AMIStateTempBase)) {
      auto ty = TBottom;
      if (acls.maybe(ALocalAny))        ty |= cur().mbaseLocalType;
      if (acls.maybe(AStackAny))        ty |= cur().mbaseStackType;
      if (acls.maybe(AMIStateTempBase)) ty |= cur().mbaseTempType;
      return ty;
    }
    return mbase().type;
  }();
  if (!knownType.maybe(mbrType)) return TriBool::No;

  // If either represents multiple locations, we can't say for sure.
  if (!acls.isSingleLocation() || !mbr().pointee.isSingleLocation()) {
    return TriBool::Maybe;
  }
  // The alias classes are the same, and they're both single
  // locations, so it's a definite match.
  if (acls == mbr().pointee) return TriBool::Yes;
  // If not, however, they still might be. There's alias classes which
  // can be single locations, and not equal to each other, but still
  // might be each other (props, for example).
  return TriBool::Maybe;
}

TriBool FrameStateMgr::isMBase(SSATmp* ptr) const {
  assertx(ptr->isA(TMem));
  return isMBase(ptr, canonicalize(pointee(ptr)));
}

///////////////////////////////////////////////////////////////////////////////

// Update tracked state to reflect a load (into dst) through the given
// pointer.
void FrameStateMgr::pointerLoad(SSATmp* ptr, SSATmp* dst) {
  assertx(ptr->isA(TMem));
  auto const acls = canonicalize(pointee(ptr));
  // These updates are optional, since they are only used to give us a
  // known SSATmp for the location. We can only update them if we know
  // for sure what location is being affected.
  if (isMBase(ptr, acls) == TriBool::Yes) {
    setValue(Location::MBase{}, dst);
    if (acls.maybe(ALocalAny))        cur().mbaseLocalType = dst->type();
    if (acls.maybe(AStackAny))        cur().mbaseStackType = dst->type();
    if (acls.maybe(AMIStateTempBase)) cur().mbaseTempType  = dst->type();
  }
  if (acls.isSingleLocation()) {
    auto const oldModified = cur().stackModified;
    setValue(acls, dst);
    cur().stackModified = oldModified;
  }
}

// Update tracked state to reflect a store of the given value through
// the given pointer.
void FrameStateMgr::pointerStore(SSATmp* ptr, SSATmp* value) {
  assertx(ptr->isA(TMem));

  auto const acls = canonicalize(pointee(ptr));

  // First sync mbase state. If the pointer might point at the mbase,
  // update it's known value/type.
  auto prevLocalType = TCell;
  auto prevStackType = TCell;
  auto prevTempType = TCell;
  switch (isMBase(ptr, acls)) {
    case TriBool::Yes: {
      prevLocalType = cur().mbaseLocalType;
      prevStackType = cur().mbaseStackType;
      prevTempType  = cur().mbaseTempType;
      setValue(Location::MBase{}, value);
      auto const type = value ? value->type() : TCell;
      if (acls.maybe(ALocalAny))        cur().mbaseLocalType = type;
      if (acls.maybe(AStackAny))        cur().mbaseStackType = type;
      if (acls.maybe(AMIStateTempBase)) cur().mbaseTempType  = type;
      break;
    }
    case TriBool::Maybe:
      prevLocalType = prevStackType = prevTempType =
        typeOfPointeeFromDefs(ptr, TCell);
      if (value) {
        widenType(Location::MBase{}, value->type());
        if (acls.maybe(ALocalAny))        cur().mbaseLocalType |= value->type();
        if (acls.maybe(AStackAny))        cur().mbaseStackType |= value->type();
        if (acls.maybe(AMIStateTempBase)) cur().mbaseTempType  |= value->type();
      } else {
        setValue(Location::MBase{}, nullptr);
        if (acls.maybe(ALocalAny))        cur().mbaseLocalType = TCell;
        if (acls.maybe(AStackAny))        cur().mbaseStackType = TCell;
        if (acls.maybe(AMIStateTempBase)) cur().mbaseTempType  = TCell;
      }
      break;
    case TriBool::No:
      prevLocalType = prevStackType = prevTempType =
        typeOfPointeeFromDefs(ptr, TCell);
      break;
  }

  // If the alias class is a single location, we can precisely change
  // the state.
  if (acls.isSingleLocation()) {
    setValue(acls, value);
  } else {
    // Otherwise union in the state with anything it might be
    if (acls.maybe(ALocalAny)) {
      for (auto const& local : cur().locals) {
        if (!prevLocalType.maybe(local.second.type)) continue;
        if (!acls.maybe(ALocal{fp(), local.first})) continue;
        value
          ? widenType(loc(local.first), value->type())
          : setValue(loc(local.first), nullptr);
      }
      // This instruction could also affect locals that aren't being
      // tracked. Be conservative and assume that they could be
      // affected.
      cur().localsCleared = true;
    }
    if (acls.maybe(AStackAny)) {
      for (auto const& stack : cur().stack) {
        if (!prevStackType.maybe(stack.second.type)) continue;
        auto const offset = stack.first.to<IRSPRelOffset>(irSPOff());
        if (!acls.maybe(AStack::at(offset))) continue;
        value
          ? widenType(stk(stack.first), value->type())
          : setValue(stk(stack.first), nullptr);
      }
    }
    if (acls.maybe(AMIStateTempBase)) {
      if (prevTempType.maybe(cur().mTempBase.type)) {
        value ? widenMTempBase(value->type()) : setMTempBase(nullptr);
      }
    }
  }
}

void FrameStateMgr::pointerRefine(SSATmp* ptr, Type type) {
  assertx(ptr->isA(TMem));
  auto const acls = canonicalize(pointee(ptr));
  if (isMBase(ptr, acls) == TriBool::Yes) {
    refineType(Location::MBase{}, type, std::nullopt);
    cur().mbaseStackType &= type;
    cur().mbaseLocalType &= type;
    cur().mbaseTempType  &= type;
  }
  if (acls.isSingleLocation()) refineType(acls, type, std::nullopt);
}

///////////////////////////////////////////////////////////////////////////////

template<LTag tag>
static void setValueImpl(Location l,
                         LocationState<tag>& state,
                         SSATmp* value,
                         bool forLoad) {
  ITRACE(2, "{} := {}\n", jit::show(l), value ? value->toString() : "<>");
  state.value = value;
  state.type = value ? value->type() : LocationState<tag>::default_type();
  if (!forLoad) state.maybeChanged = true;

  state.typeSrcs.clear();
  if (value) {
    state.typeSrcs.insert(TypeSource::makeValue(value));
  }
}

/*
 * Update the value (and type) for `l'.
 */
void FrameStateMgr::setValue(Location l, SSATmp* value, bool forLoad) {
  switch (l.tag()) {
    case LTag::Local:
      return setValueImpl(l, localState(l), value, forLoad);
    case LTag::Stack:
      cur().stackModified = true;
      return setValueImpl(l, stackState(l), value, forLoad);
    case LTag::MBase:
      return setValueImpl(l, mbaseState(), value, forLoad);
  }
  not_reached();
}

void FrameStateMgr::setValue(const AliasClass& pointee, SSATmp* value) {
  assertx(pointee.isSingleLocation());

  if (auto const l = pointee.is_local()) {
    setValue(loc(l->ids.singleValue()), value);
  } else if (auto const s = pointee.is_stack()) {
    assertx(s->size() == 1);
    if (auto const l = optStk(s->low)) setValue(*l, value);
  } else if (pointee <= AMIStateTempBase) {
    setMTempBase(value);
  }
}

/*
 * Like setValue, but also updates the MBase state appropriately if
 * the MBase might be the given location.
 */
void FrameStateMgr::setValueAndSyncMBase(Location l,
                                         SSATmp* value,
                                         bool forLoad) {
  assertx(l.tag() != LTag::MBase);
  switch (isMBase(nullptr, locationToAliasClass(l))) {
    case TriBool::No:
      break;
    case TriBool::Yes:
      setValue(Location::MBase{}, value);
      if (l.tag() == LTag::Stack) {
        cur().mbaseStackType = value ? value->type() : TCell;
      } else if (l.tag() == LTag::Local) {
        cur().mbaseLocalType = value ? value->type() : TCell;
      }
      break;
    case TriBool::Maybe:
      if (forLoad) break;
      if (value) {
        widenType(Location::MBase{}, value->type());
      } else {
        setValue(Location::MBase{}, nullptr);
      }
      if (l.tag() == LTag::Stack) {
        cur().mbaseStackType |= value ? value->type() : TCell;
      } else if (l.tag() == LTag::Local) {
        cur().mbaseLocalType |= value ? value->type() : TCell;
      }
      break;
  }

  // NB: Do this after the mbase check, since it might consult the
  // type in the location being modified.
  setValue(l, value, forLoad);
}

template<LTag tag>
static void setTypeImpl(Location l, LocationState<tag>& state, Type type) {
  ITRACE(2, "{} :: {} -> {}\n", jit::show(l), state.type, type);
  state.value = nullptr;
  state.type = type;
  state.maybeChanged = true;
  state.typeSrcs.clear();
}

/*
 * Update the type for `l' to reflect a possible change in the value---but when
 * we don't have that value.
 *
 * Setting the type clears the typeSrcs, so the new type may not be derived
 * from the old type in any way.
 */
void FrameStateMgr::setType(Location l, Type type) {
  switch (l.tag()) {
    case LTag::Local:
      return setTypeImpl(l, localState(l), type);
    case LTag::Stack:
      cur().stackModified = true;
      return setTypeImpl(l, stackState(l), type);
    case LTag::MBase:
      return setTypeImpl(l, mbaseState(), type);
  }
  not_reached();
}

template<LTag tag>
static void widenTypeImpl(Location l, LocationState<tag>& state, Type type) {
  ITRACE(2, "{} :: {} -> {}\n", jit::show(l), state.type, state.type | type);
  state.value = nullptr;
  state.type |= type;
  state.maybeChanged = true;
}

/*
 * Update the type for `l' as a result of an operation that might change the
 * value.
 *
 * This is just like setType() except that typeSrcs are preserved, so the new
 * type may be derived from the old type.
 */
void FrameStateMgr::widenType(Location l, Type type) {
  switch (l.tag()) {
    case LTag::Local:
      return widenTypeImpl(l, localState(l), type);
    case LTag::Stack:
      cur().stackModified = true;
      return widenTypeImpl(l, stackState(l), type);
    case LTag::MBase:
      return widenTypeImpl(l, mbaseState(), type);
  }
  not_reached();
}

template<LTag tag>
static void refineTypeImpl(Location l, LocationState<tag>& state,
                           Type type, Optional<TypeSource> typeSrc) {
  auto const refined = state.type.refine(type);
  ITRACE(2, "{} :: {} -> {} (via {})\n",
         jit::show(l), state.type, refined, type);

  state.type = refined;
  state.typeSrcs.clear();
  if (typeSrc) state.typeSrcs.insert(*typeSrc);
}

/*
 * Update the type for `l' to reflect new information that we've obtained from
 * guards, assertions, or the like.
 *
 * A type refinement does /not/ indicate a change in value, so the various
 * changed flags are not touched.
 */
void FrameStateMgr::refineType(Location l,
                               Type type,
                               Optional<TypeSource> typeSrc) {
  switch (l.tag()) {
    case LTag::Local: return refineTypeImpl(l, localState(l), type, typeSrc);
    case LTag::Stack: return refineTypeImpl(l, stackState(l), type, typeSrc);
    case LTag::MBase: return refineTypeImpl(l, mbaseState(), type, typeSrc);
  }
  not_reached();
}

void FrameStateMgr::refineType(const AliasClass& pointee, Type type,
                               Optional<TypeSource> typeSrc) {
  assertx(pointee.isSingleLocation());

  if (auto const l = pointee.is_local()) {
    refineType(loc(l->ids.singleValue()), type, typeSrc);
  } else if (auto const s = pointee.is_stack()) {
    assertx(s->size() == 1);
    if (auto const l = optStk(s->low)) refineType(*l, type, typeSrc);
  } else if (pointee <= AMIStateTempBase) {
    refineMTempBase(type);
  }
}

/*
 * Like refineType, but also keeps state synced between the MBase and
 * the location it might represent.
 */
void FrameStateMgr::refineTypeAndSyncMBase(Location l,
                                           Type type,
                                           TypeSource typeSrc) {
  if (l.tag() == LTag::MBase) {
    // We're updating the MBase. Try to find what tracked locations
    // the MBase might be and update those too. We only do this if we
    // definitely know the location.
    auto const& p = cur().mbr.pointee;
    if (auto const local = p.is_local()) {
      if (local->ids.hasSingleValue()) {
        refineType(loc(local->ids.singleValue()), type, typeSrc);
      }
    } else if (auto const stack = p.is_stack()) {
      if (stack->size() == 1) {
        if (auto const os = optStk(stack->low)) refineType(*os, type, typeSrc);
      }
    } else if (p <= AMIStateTempBase) {
      refineMTempBase(type);
    }

    cur().mbaseStackType = cur().mbaseStackType.refine(type);
    cur().mbaseLocalType = cur().mbaseLocalType.refine(type);
    cur().mbaseTempType  = cur().mbaseTempType.refine(type);
  } else {
    // Only refine the MBase if it's definitely this location
    auto const acls = locationToAliasClass(l);
    if (isMBase(nullptr, acls) == TriBool::Yes) {
      refineType(Location::MBase{}, type, typeSrc);
      if (l.tag() == LTag::Stack) {
        cur().mbaseStackType = cur().mbaseStackType.refine(type);
      } else if (l.tag() == LTag::Local) {
        cur().mbaseLocalType = cur().mbaseLocalType.refine(type);
      }
    }
  }

  refineType(l, type, typeSrc);
}

/*
 * Refine the value for `state' to `newVal' if it was set to `oldVal'.
 */
template<LTag tag>
bool FrameStateMgr::refineValue(LocationState<tag>& state,
                                SSATmp* oldVal, SSATmp* newVal) {
  if (!state.value || canonical(state.value) != canonical(oldVal)) {
    return false;
  }
  ITRACE(2, "refining value: {} -> {}\n", *state.value, *newVal);

  state.value = newVal;
  state.type = newVal->type();
  state.typeSrcs.clear();
  state.typeSrcs.insert(TypeSource::makeValue(newVal));
  return true;
}

void FrameStateMgr::refineMBaseValue(SSATmp* oldVal, SSATmp* newVal) {
  if (!cur().mbase) return;
  if (!refineValue(*cur().mbase, oldVal, newVal)) return;
  cur().mbaseStackType &= newVal->type();
  cur().mbaseLocalType &= newVal->type();
  cur().mbaseTempType  &= newVal->type();
}

///////////////////////////////////////////////////////////////////////////////

void FrameStateMgr::clearCtx() {
  ITRACE(2, "clearCtx\n");
  cur().ctx = nullptr;
  cur().ctxType = cur().origCtxType;
}

void FrameStateMgr::clearLocals() {
  ITRACE(2, "clearLocals\n");
  for (auto& it : cur().locals) {
    auto const id = it.first;
    setValue(loc(id), nullptr);
  }
  cur().localsCleared = true;

  if (mbr().pointee.maybe(ALocalAny)) {
    setValue(Location::MBase{}, nullptr);
    cur().mbaseLocalType = TCell;
  }
}

void FrameStateMgr::clearMInstr() {
  ITRACE(2, "clearMInstr\n");
  auto& c = cur();
  c.mbr = MBRState{};
  c.mbase.emplace();  // keep tracking the MBase
  c.mTempBase = MTempBaseState{};
  c.mROProp = TriBool::Maybe;
  c.mbaseStackType = TCell;
  c.mbaseLocalType = TCell;
  c.mbaseTempType = TCell;
}

///////////////////////////////////////////////////////////////////////////////

void FrameStateMgr::setMBR(SSATmp* mbr, bool forLoad) {
  assertx(!mbr || mbr->isA(TMem));
  if (mbr && mbr == cur().mbr.ptr) return;

  auto const mbrPointee = mbr ? canonicalize(pointee(mbr)) : AUnknownTV;
  assertx(mbrPointee <= AUnknownTV);
  assertx(!mbr || mbrPointee <= canonicalize(pointee(mbr->type())));

  ITRACE(2, "MBR := {}\n", mbr ? mbr->toString() : "<>");
  cur().mbr.ptr = mbr;
  cur().mbr.ptrType = mbr ? mbr->type() : TLval;
  cur().mbr.pointee = mbrPointee;

  if (forLoad) return;

  // Since we changed the MBR, the MBase might be a different location
  // now. Try to find the new location and sync the MBase state to
  // match it. We only do this if we definitely know the location.

  auto const set = [&] (SSATmp* val, Type type) {
    ITRACE(2, "MBase := {} ({})\n", val ? val->toString() : "<>", type);
    auto& mbase = mbaseState();
    mbase.value = val;
    mbase.type = type;
    mbase.maybeChanged = true;
    mbase.typeSrcs.clear();
  };

  cur().mbaseLocalType = TBottom;
  cur().mbaseStackType = TBottom;
  cur().mbaseTempType = TBottom;

  if (auto const l = mbrPointee.is_local()) {
    if (l->ids.hasSingleValue()) {
      auto const& state = localState(l->ids.singleValue());
      cur().mbaseLocalType = state.type;
      return set(state.value, state.type);
    }
  } else if (auto const s = mbrPointee.is_stack()) {
    if (s->size() == 1) {
      if (auto const l = optStk(s->low)) {
        auto const& state = stackState(*l);
        cur().mbaseStackType = state.type;
        return set(state.value, state.type);
      }
    }
  } else if (mbrPointee <= AMIStateTempBase) {
    cur().mbaseTempType = cur().mTempBase.type;
    return set(cur().mTempBase.value, cur().mTempBase.type);
  }

  // It isn't known to be one of our tracked locations. Try to
  // calculate the type from it's definitions.
  auto const type = mbr ? typeOfPointeeFromDefs(mbr, TCell) : TCell;
  if (mbrPointee.maybe(ALocalAny))        cur().mbaseLocalType = type;
  if (mbrPointee.maybe(AStackAny))        cur().mbaseStackType = type;
  if (mbrPointee.maybe(AMIStateTempBase)) cur().mbaseTempType  = type;
  set(nullptr, type);
}

///////////////////////////////////////////////////////////////////////////////

void FrameStateMgr::setMTempBase(SSATmp* value) {
  ITRACE(2, "MTempBase := {}\n", value ? value->toString() : "<>");
  cur().mTempBase.value = value;
  cur().mTempBase.type = value ? value->type() : TCell;
}

void FrameStateMgr::setMTempBaseType(Type type) {
  ITRACE(2, "MTempBase :: {} -> {}\n", cur().mTempBase.type, type);
  cur().mTempBase.value = nullptr;
  cur().mTempBase.type = type;
}

void FrameStateMgr::widenMTempBase(Type type) {
  ITRACE(2, "MTempBase :: {} -> {}\n",
         cur().mTempBase.type, cur().mTempBase.type | type);
  cur().mTempBase.value = nullptr;
  cur().mTempBase.type |= type;
}

void FrameStateMgr::refineMTempBase(Type type) {
  auto const refined = cur().mTempBase.type & type;
  ITRACE(2, "MTempBase :: {} -> {} (via {})\n",
         cur().mTempBase.type, refined, type);
  cur().mTempBase.type = refined;
}

///////////////////////////////////////////////////////////////////////////////

std::string FrameState::show() const {
  auto const optTmp = [] (SSATmp* t) -> std::string {
    if (t) return t->toString();
    return "<>";
  };

  std::string out;

  fmt::format_to(std::back_inserter(out), "Func: {}\n"
    "  fp: {}, sp: {}, ctx: {}, fpFixup: {}\n"
    "  irSPOff: {}, bcSPOff: {}, stublogue: {}\n"
    "  stack modified: {}, locals cleared: {}\n"
    "  read-only prop: {}\n",
    curFunc->fullName()->slice(),
    optTmp(fpValue),
    optTmp(spValue),
    optTmp(ctx),
    optTmp(fixupFPValue),
    irSPOff.offset,
    bcSPOff.offset,
    stublogue ? "yes" : "no",
    stackModified ? "yes" : "no",
    localsCleared ? "yes" : "no",
    HPHP::show(mROProp)
  );
  if (!locals.empty()) {
    fmt::format_to(std::back_inserter(out), "{:-^70}\n", "");
    for (auto const& local : locals) {
      fmt::format_to(std::back_inserter(out), "  Local #{}: {} {}{}\n",
        local.first,
        optTmp(local.second.value),
        local.second.type,
        local.second.maybeChanged ? " (changed)" : ""
      );
    }
  }
  if (!stack.empty()) {
    fmt::format_to(std::back_inserter(out), "{:-^70}\n", "");
    for (auto const& stk : stack) {
      fmt::format_to(std::back_inserter(out), "  Stack #{}: {} {}{}\n",
        stk.first.offset,
        optTmp(stk.second.value),
        stk.second.type,
        stk.second.maybeChanged ? " (changed)" : ""
      );
    }
  }
  if (mTempBase.value || mTempBase.type < TCell) {
    fmt::format_to(std::back_inserter(out), "{:-^70}\n  MTempBase: {} {}\n",
      "",
      optTmp(mTempBase.value),
      mTempBase.type
    );
  }
  if (mbr.ptr || mbr.ptrType < TLval ||
      mbr.pointee < AUnknownTV) {
    fmt::format_to(std::back_inserter(out), "{:-^70}\n  MBR: {} {} {}\n",
      "",
      optTmp(mbr.ptr),
      mbr.ptrType,
      jit::show(mbr.pointee)
    );
  }
  if (mbase ||
      mbaseStackType < TCell ||
      mbaseLocalType < TCell ||
      mbaseTempType < TCell) {
    fmt::format_to(std::back_inserter(out), "{:-^70}\n  MBase: {} {}{} [L:{}, S:{}, T:{}]\n",
      "",
      optTmp(mbase ? mbase->value : nullptr),
      mbase ? mbase->type : TCell,
      mbase && mbase->maybeChanged ? " (changed)" : "",
      mbaseLocalType,
      mbaseStackType,
      mbaseTempType
    );
  }

  return out;
}

std::string FrameStateMgr::show() const {
  std::string out;
  for (auto const& state : m_stack) {
    fmt::format_to(std::back_inserter(out), "{:=^70}\n{}", "", state.show());
  }
  fmt::format_to(std::back_inserter(out), "{:=^70}\n", "");
  return out;
}

///////////////////////////////////////////////////////////////////////////////

}
