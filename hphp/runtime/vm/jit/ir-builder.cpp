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

#include "hphp/runtime/vm/jit/ir-builder.h"

#include <utility>

#include <folly/ScopeGuard.h>

#include "hphp/util/assertions.h"
#include "hphp/util/configs/hhir.h"
#include "hphp/util/trace.h"

#include "hphp/runtime/vm/jit/analysis.h"
#include "hphp/runtime/vm/jit/guard-constraint.h"
#include "hphp/runtime/vm/jit/ir-unit.h"
#include "hphp/runtime/vm/jit/memory-effects.h"
#include "hphp/runtime/vm/jit/mutation.h"
#include "hphp/runtime/vm/jit/print.h"
#include "hphp/runtime/vm/jit/punt.h"
#include "hphp/runtime/vm/jit/simple-propagation.h"
#include "hphp/runtime/vm/jit/simplify.h"

namespace HPHP::jit::irgen {

namespace {

TRACE_SET_MOD(hhir)
using Trace::Indent;

///////////////////////////////////////////////////////////////////////////////

template<typename M>
const typename M::mapped_type& get_required(const M& m,
                                            typename M::key_type key) {
  auto it = m.find(key);
  always_assert(it != m.end());
  return it->second;
}

SSATmp* fwdGuardSource(IRInstruction* inst) {
  if (inst->is(AssertType, CheckType)) return inst->src(0);

  assertx(inst->is(AssertLoc, AssertStk, AssertMBase));
  inst->convertToNop();
  return nullptr;
}

///////////////////////////////////////////////////////////////////////////////

}

///////////////////////////////////////////////////////////////////////////////

IRBuilder::IRBuilder(IRUnit& unit, const Func* func)
  : m_unit(unit)
  , m_curBCContext{BCMarker{}, 0}
  , m_state(func)
  , m_curBlock(m_unit.entry())
{
  if (Cfg::HHIR::GenOpts) {
    m_enableSimplification = Cfg::HHIR::Simplification;
  }
  m_state.startBlock(m_curBlock, false);
}

void IRBuilder::enableConstrainGuards() {
  m_constrainGuards = true;
}

bool IRBuilder::shouldConstrainGuards() const {
  return m_constrainGuards;
}

bool IRBuilder::isMBaseLoad(const IRInstruction* inst) const {
  if (!inst->is(LdMem)) return false;
  return m_state.isMBase(inst->src(0)) == TriBool::Yes;
}

void IRBuilder::ensureBlockAppendable() {
  if (m_curBlock->empty()) return;

  auto& prev = m_curBlock->back();
  if (!prev.isBlockEnd()) return;
  assertx(!prev.isTerminal());

  m_curBlock = m_unit.defBlock(prev.block()->profCount());
  m_curBlock->setHint(prev.block()->hint());
  prev.setNext(m_curBlock);
  m_state.finishBlock(prev.block());
  FTRACE(2, "lazily appending B{}\n", m_curBlock->id());
  m_state.startBlock(m_curBlock, false);
}

void IRBuilder::appendInstruction(IRInstruction* inst) {
  FTRACE(1, "  append {}\n", inst->toString());
  assertx(inst->marker().valid());
  assertx(checkOperandTypes(inst));
  if (inst->is(Nop, DefConst)) return;

  if (shouldConstrainGuards()) {
    auto const l = [&]() -> Optional<Location> {
      switch (inst->op()) {
        case AssertLoc:
        case LdLoc:
          return loc(inst->extra<LocalId>()->locId);

        case AssertStk:
        case LdStk:
          return stk(inst->extra<IRSPRelOffsetData>()->offset);

        case AssertMBase:
          return make_optional<Location>(Location::MBase{});

        case LdMem:
          return isMBaseLoad(inst)
            ? make_optional<Location>(Location::MBase{})
            : std::nullopt;

        default:
          return std::nullopt;
      }
      not_reached();
    }();

    // If we're constraining guards, some instructions need certain information
    // to be recorded in side tables.
    if (l) {
      m_constraints.typeSrcs[inst] = m_state.typeSrcsOf(*l);
      if (!inst->is(LdLoc, LdStk) && !isMBaseLoad(inst)) {
        constrainLocation(*l, DataTypeGeneric, "appendInstruction");
        m_constraints.prevTypes[inst] = m_state.typeOf(*l);
      }
    }
  }

  // If the block isn't empty, check if we need to create a new block.
  ensureBlockAppendable();

  assertx((m_curBlock->empty() || !m_curBlock->back().isBlockEnd()) &&
          "Can't append an instruction after a BlockEnd instruction");
  m_curBlock->push_back(inst);
  m_state.update(inst);
  if (inst->isTerminal()) m_state.finishBlock(m_curBlock);

  // If the instruction is block ending and introduces a bottom type, we are
  // now in an unreachable state.
  if (inst->isNextEdgeUnreachable() && inst->isBlockEnd()) {
    assertx(!inst->isTerminal());
    m_curBlock = m_unit.defBlock(1);
    m_curBlock->setHint(Block::Hint::Unused);
    inst->setNext(m_curBlock);
    m_state.finishBlock(inst->block());
    m_state.startBlock(m_curBlock, false);
    gen(Unreachable, ASSERT_REASON);
  }
}

///////////////////////////////////////////////////////////////////////////////

SSATmp* IRBuilder::preOptimizeAssertTypeOp(IRInstruction* inst,
                                           const Type oldType,
                                           SSATmp* oldVal,
                                           const IRInstruction* srcInst) {
  ITRACE(3, "preOptimizeAssertTypeOp({}, {}, {}, {})\n",
         *inst, oldType,
         oldVal ? oldVal->toString() : "nullptr",
         srcInst ? srcInst->toString() : "nullptr");

  auto const newType = oldType.refine(inst->typeParam());

  // Eliminate this AssertTypeOp if:
  // 1) oldType is at least as good as newType and:
  //      a) typeParam == Cell
  //      b) oldVal is from a DefConst
  //      c) oldType.hasConstVal()
  //    The AssertType will never be useful for guard constraining in these
  //     situations.
  // 2) The source instruction is known to be another assert that's at least
  //    as good as this one.
  if ((oldType <= newType &&
       (inst->typeParam() == TCell ||
        (oldVal && oldVal->inst()->is(DefConst)) ||
        oldType.hasConstVal())) ||
      (srcInst &&
       srcInst->is(AssertType, AssertLoc, AssertStk, AssertMBase) &&
       srcInst->typeParam() <= inst->typeParam())) {
    return fwdGuardSource(inst);
  }

  // 3) We're not constraining guards, and the old type is at least as good as
  //    the new type.
  if (!shouldConstrainGuards()) {
    if (newType == TBottom) {
      gen(Unreachable, ASSERT_REASON);
      return m_unit.cns(TBottom);
    }
    if (oldType <= newType) return fwdGuardSource(inst);
    inst->setTypeParam(newType);
    retypeDests(inst, &m_unit);
  }

  return nullptr;
}

SSATmp* IRBuilder::preOptimizeAssertLocation(IRInstruction* inst,
                                             Location l) {
  auto const prevType = typeOf(l, DataTypeGeneric);
  if (auto const prevValue = valueOf(l, DataTypeGeneric)) {
    auto toAssert = inst->typeParam();
    if (!shouldConstrainGuards()) toAssert = prevType.refine(toAssert);
    gen(AssertType, toAssert, prevValue);
    inst->convertToNop();
    return nullptr;
  }

  return preOptimizeAssertTypeOp(
    inst,
    prevType,
    nullptr,
    nullptr
  );
}

SSATmp* IRBuilder::preOptimizeAssertType(IRInstruction* inst) {
  auto const src = inst->src(0);
  return preOptimizeAssertTypeOp(inst, src->type(), src, src->inst());
}

SSATmp* IRBuilder::preOptimizeAssertLoc(IRInstruction* inst) {
  return preOptimizeAssertLocation(inst, loc(inst->extra<AssertLoc>()->locId));
}

SSATmp* IRBuilder::preOptimizeAssertStk(IRInstruction* inst) {
  return preOptimizeAssertLocation(inst, stk(inst->extra<AssertStk>()->offset));
}

SSATmp* IRBuilder::preOptimizeAssertMBase(IRInstruction* inst) {
  return preOptimizeAssertLocation(inst, Location::MBase{});
}

SSATmp* IRBuilder::preOptimizeLdLocation(IRInstruction* inst, Location l) {
  auto const type = typeOf(l, DataTypeGeneric) & inst->typeParam();

  // The types may not be compatible in the presence of unreachable code.
  // Don't try to optimize the code in this case, and just let dead code
  // elimination take care of it later.
  if (type <= TBottom) {
    inst->setTypeParam(TBottom);
    retypeDests(inst, &m_unit);
    return nullptr;
  }

  if (auto const tmp = valueOf(l, DataTypeGeneric)) {
    assertx(type <= tmp->type());
    if (type < tmp->type()) return gen(AssertType, type, tmp);
    return tmp;
  }

  inst->setTypeParam(type);
  retypeDests(inst, &m_unit);
  return nullptr;
}

SSATmp* IRBuilder::preOptimizeLdLoc(IRInstruction* inst) {
  return preOptimizeLdLocation(inst, loc(inst->extra<LdLoc>()->locId));
}

SSATmp* IRBuilder::preOptimizeLdStk(IRInstruction* inst) {
  return preOptimizeLdLocation(inst, stk(inst->extra<LdStk>()->offset));
}

SSATmp* IRBuilder::preOptimizeLdMBase(IRInstruction* inst) {
  auto const type = m_state.mbr().ptrType & inst->typeParam();

  if (type <= TBottom) {
    inst->setTypeParam(TBottom);
    retypeDests(inst, &m_unit);
    return nullptr;
  }

  if (auto const ptr = m_state.mbr().ptr) {
    assertx(type <= ptr->type());
    if (type < ptr->type()) return gen(AssertType, type, ptr);
    return ptr;
  }

  inst->setTypeParam(type);
  retypeDests(inst, &m_unit);

  auto const& mbrPointee = m_state.mbr().pointee;
  auto& acls = inst->extra<LdMBase>()->acls;
  if (mbrPointee < acls) {
    acls = mbrPointee;
  } else {
    auto const fromPtrType = pointee(inst->typeParam());
    if (fromPtrType < acls) acls = fromPtrType;
  }

  // If we know the pointee of the MBR, we might be able to
  // rematerialize the MBase ptr directly without a load. We only do
  // this for trivial address calculations.
  auto const ptr = [&] () -> SSATmp* {
    if (!acls.isSingleLocation()) return nullptr;
    if (auto const l = acls.is_local()) {
      return gen(LdLocAddr, LocalId { l->ids.singleValue() }, m_state.fp());
    } else if (auto const s = acls.is_stack()) {
      if (m_state.validStackOffset(s->low)) {
        return gen(LdStkAddr, IRSPRelOffsetData { s->low }, m_state.sp());
      }
    } else if (auto const r = acls.is_rds()) {
      return gen(
        LdRDSAddr,
        RDSHandleAndType { r->handle, m_state.mbase().type },
        inst->typeParam().lvalToPtr()
      );
    } else if (acls <= AMIStateTempBase) {
      return gen(LdMIStateTempBaseAddr);
    }
    return nullptr;
  }();
  if (ptr) return gen(ConvPtrToLval, ptr);
  return nullptr;
}

SSATmp* IRBuilder::preOptimizeLdMem(IRInstruction* inst) {
  auto const ptr = inst->src(0);
  assertx(ptr->isA(TMem));

  auto const type =
    m_state.typeOfPointee(ptr, inst->typeParam()) & inst->typeParam();

  // The types may not be compatible in the presence of unreachable code.
  // Don't try to optimize the code in this case, and just let dead code
  // elimination take care of it later.
  if (type <= TBottom) {
    inst->setTypeParam(TBottom);
    retypeDests(inst, &m_unit);
    return nullptr;
  }

  if (auto const val = m_state.valueOfPointee(ptr)) {
    assertx(type <= val->type());
    if (type < val->type()) return gen(AssertType, type, val);
    return val;
  }

  // Try to convert the pointer load to a load directly from
  // locals/stack.
  auto const acls = canonicalize(pointee(ptr));
  if (acls.isSingleLocation()) {
    if (auto const l = acls.is_local()) {
      return gen(LdLoc, type, LocalId { l->ids.singleValue() }, m_state.fp());
    } else if (auto const s = acls.is_stack()) {
      if (m_state.validStackOffset(s->low)) {
        return gen(LdStk, type, IRSPRelOffsetData { s->low }, m_state.sp());
      }
    }
  }

  inst->setTypeParam(type);
  retypeDests(inst, &m_unit);
  return nullptr;
}

SSATmp* IRBuilder::preOptimizeStMem(IRInstruction* inst) {
  auto const ptr = inst->src(0);
  assertx(ptr->isA(TMem));

  // Try to convert the pointer store to a store directly to
  // locals/stack.
  auto const acls = canonicalize(pointee(ptr));
  if (acls.isSingleLocation()) {
    if (auto const l = acls.is_local()) {
      gen(
        StLoc, LocalId { l->ids.singleValue() }, m_state.fp(), inst->src(1)
      );
      inst->convertToNop();
    } else if (auto const s = acls.is_stack()) {
      if (m_state.validStackOffset(s->low)) {
        gen(StStk, IRSPRelOffsetData { s->low }, m_state.sp(), inst->src(1));
        inst->convertToNop();
      }
    }
  }

  return nullptr;
}

SSATmp* IRBuilder::preOptimizeStMemMeta(IRInstruction* inst) {
  auto const ptr = inst->src(0);
  assertx(ptr->isA(TMem));

  // Try to convert the pointer store to a store directly to
  // locals/stack.
  auto const acls = canonicalize(pointee(ptr));
  if (acls.isSingleLocation()) {
    if (auto const l = acls.is_local()) {
      gen(
        StLocMeta, LocalId { l->ids.singleValue() }, m_state.fp(), inst->src(1)
      );
      inst->convertToNop();
    } else if (auto const s = acls.is_stack()) {
      if (m_state.validStackOffset(s->low)) {
        gen(
          StStkMeta, IRSPRelOffsetData { s->low }, m_state.sp(), inst->src(1)
        );
        inst->convertToNop();
      }
    }
  }

  return nullptr;
}

SSATmp* IRBuilder::preOptimizeBaseTypeParam(IRInstruction* inst) {
  auto const ptr = inst->src(0);
  assertx(ptr->isA(TMem));
  auto const type = m_state.typeOfPointee(ptr);
  inst->setTypeParam(type & inst->typeParam());
  retypeDests(inst, &m_unit);
  return nullptr;
}
SSATmp* IRBuilder::preOptimizeElemDictD(IRInstruction* inst) {
  return preOptimizeBaseTypeParam(inst);
}
SSATmp* IRBuilder::preOptimizeElemDictU(IRInstruction* inst) {
  return preOptimizeBaseTypeParam(inst);
}
SSATmp* IRBuilder::preOptimizeBespokeElem(IRInstruction* inst) {
  return preOptimizeBaseTypeParam(inst);
}
SSATmp* IRBuilder::preOptimizeSetElem(IRInstruction* inst) {
  if (auto const i = preOptimizeBaseTypeParam(inst)) return i;
  if (!inst->is(SetElem)) return nullptr;

  auto const baseType = inst->typeParam();
  if (baseType <= TDict) {
    auto const basePtr = inst->src(0);
    auto const key = inst->src(1);
    auto const value = inst->src(2);
    if (!key->isA(TInt | TStr)) return nullptr;
    auto const base = gen(LdMem, baseType, basePtr);
    auto const newArr = gen(DictSet, inst->taken(), base, key, value);
    gen(StMem, basePtr, newArr);
    gen(IncRef, value);
    return value;
  }
  return nullptr;
}

SSATmp* IRBuilder::preOptimizeLdClosureCtx(IRInstruction* inst) {
  auto const closure = canonical(inst->src(0));
  if (!closure->inst()->is(ConstructClosure)) return nullptr;
  return gen(AssertType, inst->typeParam(), closure->inst()->src(0));
}
SSATmp* IRBuilder::preOptimizeLdClosureCls(IRInstruction* inst) {
  return preOptimizeLdClosureCtx(inst);
}
SSATmp* IRBuilder::preOptimizeLdClosureThis(IRInstruction* inst) {
  return preOptimizeLdClosureCtx(inst);
}

SSATmp* IRBuilder::preOptimizeLdFrameCtx(IRInstruction* inst) {
  assertx(inst->is(LdFrameCls, LdFrameThis));

  auto const func = inst->marker().func();
  assertx(func->cls() || func->isClosureBody());
  if (auto ctx = m_state.ctx()) {
    assertx(IMPLIES(inst->is(LdFrameCls), ctx->type() <= TCls));
    assertx(IMPLIES(inst->is(LdFrameThis), ctx->type() <= TObj));
    if (ctx->inst()->is(DefConst)) return ctx;
    if (ctx->type().admitsSingleVal()) {
      return m_unit.cns(ctx->type());
    }
    return ctx;
  }

  if (inst->is(LdFrameCls)) {
    assertx(func->cls());
    if (func->cls()->attrs() & AttrNoOverride) {
      return m_unit.cns(func->cls());
    }
  }

  return nullptr;
}

SSATmp* IRBuilder::preOptimizeLdFrameThis(IRInstruction* inst) {
  return preOptimizeLdFrameCtx(inst);
}
SSATmp* IRBuilder::preOptimizeLdFrameCls(IRInstruction* inst) {
  return preOptimizeLdFrameCtx(inst);
}

SSATmp* IRBuilder::preOptimizeStMROProp(IRInstruction* inst) {
  // Simple store-elim. If the store is redundant because it won't
  // change the known value, drop it.
  if (!inst->src(0)->hasConstVal(TBool)) return nullptr;
  auto const redundant = [&] {
    switch (m_state.mROProp()) {
      case TriBool::Yes:
        return inst->src(0)->boolVal();
      case TriBool::No:
        return !inst->src(0)->boolVal();
      case TriBool::Maybe:
        return false;
    }
    not_reached();
  }();
  if (redundant) inst->convertToNop();
  return nullptr;
}

SSATmp* IRBuilder::preOptimizeCheckMROProp(IRInstruction* inst) {
  switch (m_state.mROProp()) {
    case TriBool::Yes:
      inst->convertToNop();
      break;
    case TriBool::No:
      gen(Jmp, inst->taken());
      inst->convertToNop();
      break;
    case TriBool::Maybe:
      break;
  }
  return nullptr;
}

SSATmp* IRBuilder::preOptimize(IRInstruction* inst) {
#define X(op) case op: return preOptimize##op(inst);
  switch (inst->op()) {
  X(AssertType)
  X(AssertLoc)
  X(AssertStk)
  X(AssertMBase)
  X(LdLoc)
  X(LdStk)
  X(LdMBase)
  X(LdMem)
  X(LdClosureCls)
  X(LdClosureThis)
  X(LdFrameCls)
  X(LdFrameThis)
  X(StMem)
  X(StMemMeta)
  X(StMROProp)
  X(CheckMROProp)
  X(ElemDictD)
  X(ElemDictU)
  X(BespokeElem)
  X(SetElem)
  default: break;
  }
#undef X
  return nullptr;
}

///////////////////////////////////////////////////////////////////////////////

/*
 * Perform preoptimization and simplification on the input instruction.  If the
 * input instruction has a dest, this will return an SSATmp that represents the
 * same value as dst(0) of the input instruction.  If the input instruction has
 * no dest, this will return nullptr.
 *
 * The caller never needs to clone or append; all this has been done.
 */
SSATmp* IRBuilder::optimizeInst(IRInstruction* inst, CloneFlag doClone,
                                Block* /*srcBlock*/) {
  static DEBUG_ONLY __thread int instNest = 0;
  if (debug) ++instNest;
  SCOPE_EXIT { if (debug) --instNest; };
  DEBUG_ONLY auto indent = [&] { return std::string(instNest * 2, ' '); };

  FTRACE(1, "optimize: {}\n", inst->toString());

  auto cloneAndAppendOriginal = [&] () -> SSATmp* {
    if (inst->op() == Nop) return nullptr;
    if (doClone == CloneFlag::Yes) {
      inst = m_unit.clone(inst);
    }
    appendInstruction(inst);
    return inst->dst(0);
  };

  // Copy propagation on inst source operands. Only perform constant
  // propagation if we're not constraining guards, to avoid breaking the
  // use-def chains it uses to find guards.
  copyProp(inst);
  if (!shouldConstrainGuards()) constProp(m_unit, inst);

  // Since preOptimize can inspect tracked state, we don't
  // perform it on non-main traces.
  if (m_savedBlocks.size() == 0) {
    // First pass of IRBuilder optimizations try to replace an
    // instruction based on tracked state before we do anything else.
    // May mutate the IRInstruction in place (and return nullptr) or
    // return an SSATmp*.
    if (auto const preOpt = preOptimize(inst)) {
      FTRACE(1, "  {}preOptimize returned: {}\n",
             indent(), preOpt->inst()->toString());
      return inst->hasDst() ? preOpt : nullptr;
    }
    if (inst->op() == Nop) return cloneAndAppendOriginal();
  }

  // We skip simplification for AssertType when guard constraining is enabled
  // because information that appears to be redundant may allow us to avoid
  // constraining certain guards. preOptimizeAssertType() still eliminates some
  // subset of redundant AssertType instructions.
  if (!m_enableSimplification ||
      (shouldConstrainGuards() && inst->is(AssertType))) {
    return cloneAndAppendOriginal();
  }

  auto const simpResult = simplify(m_unit, inst);

  // These are the possible outputs:
  //
  // ([], nullptr): no optimization possible. Use original inst.
  //
  // ([], non-nullptr): passing through a src.
  //
  // ([X, ...], Y): throw away input instruction, append 'X, ...',
  //                return Y.

  if (!simpResult.instrs.empty()) {
    // New instructions were generated. Append the new ones, filtering out Nops.
    for (auto* newInst : simpResult.instrs) {
      assertx(!newInst->isTransient());
      if (newInst->op() == Nop) continue;
      appendInstruction(newInst);
    }
    return simpResult.dst;
  }

  // No new instructions were generated. Either simplification didn't do
  // anything, or we're using some other instruction's dst instead of our own.

  if (simpResult.dst) {
    // We're using some other instruction's output.  Don't append anything.
    assertx(simpResult.dst->inst() != inst);
    return simpResult.dst;
  }

  // No simplification happened.
  return cloneAndAppendOriginal();
}

void IRBuilder::exceptionStackBoundary() {
  /*
   * If this assert fires, we're trying to put things on the stack in a catch
   * trace that the unwinder won't be able to see.
   */
  FTRACE(2, "exceptionStackBoundary()\n");
  assertx(m_state.bcSPOff() == curMarker().bcSPOff());
  m_exnStack.syncedSpLevel = m_state.bcSPOff();
  m_state.resetStackModified();
}

void IRBuilder::setCurMarker(const BCMarker& newMarker) {
  if (newMarker == curMarker()) return;
  FTRACE(2, "IRBuilder::setCurMarker:\n  old: {}\n  new: {}\n",
         curMarker().valid() ? curMarker().show() : "<invalid>",
         newMarker.show());
  assertx(newMarker.valid());
  m_curBCContext.marker = newMarker;
}

///////////////////////////////////////////////////////////////////////////////
// Guard relaxation.

void IRBuilder::guardType(Location l, Type type) {
  assertx(shouldConstrainGuards());
  assertx(!m_state.tracked(l));
  assertx(type <= TCell);
  if (type == TCell) return;

  auto const inst = [&] {
    auto const bcctx = nextBCContext();
    switch (l.tag()) {
      case LTag::Local:
        return m_unit.gen(
          AssertLoc, bcctx, type, LocalId{l.localId()}, m_state.fp()
        );
      case LTag::Stack:
        return m_unit.gen(
          AssertStk, bcctx, type,
          IRSPRelOffsetData{l.stackIdx().to<IRSPRelOffset>(m_state.irSPOff())},
          m_state.sp()
        );
      case LTag::MBase:
        return m_unit.gen(AssertMBase, bcctx, type);
    }
    not_reached();
  }();

  // An untracked location has no value, so this cannot become an AssertType.
  // Its non-generic type also prevents the assertion from being eliminated.
  optimizeInst(inst, CloneFlag::No, nullptr);
  assertx(inst->is(AssertLoc, AssertStk, AssertMBase));
  assertx(inst->block());
  m_constraints.guards.emplace(inst, GuardConstraint{});
}

bool IRBuilder::constrainValue(SSATmp* const val, GuardConstraint gc) {
  if (!shouldConstrainGuards() || gc.empty()) return false;

  if (!val) {
    ITRACE(1, "attempted to constrain nullptr SSATmp*; bailing\n", gc);
    return false;
  }
  auto inst = val->inst();

  ITRACE(1, "constraining {} to {}\n", *inst, gc);
  Indent _i;

  if (inst->is(LdLoc, LdStk) || isMBaseLoad(inst)) {
    // If the value's type source is non-null and not a FramePtr, it's a real
    // value that was killed by a Call. The value won't be live but it's ok to
    // use it to track down the guard.

    always_assert_flog(m_constraints.typeSrcs.contains(inst),
                       "no typeSrcs found for {}", *inst);

    bool changed = false;
    auto const typeSrcs = get_required(m_constraints.typeSrcs, inst);

    for (auto typeSrc : typeSrcs) {
      if (typeSrc.isGuard()) {
        if (inst->is(LdLoc)) {
          ITRACE(1, "constraining guard for local[{}]\n",
                 inst->extra<LdLoc>()->locId);
        } else if (inst->is(LdStk)) {
          ITRACE(1, "constraining guard for stack[{}]\n",
                 inst->extra<LdStk>()->offset.offset);
        } else {
          assertx(isMBaseLoad(inst));
          ITRACE(1, "constraining guard for mbase\n");
        }
      }
      changed |= constrainTypeSrc(typeSrc, gc);
    }
    return changed;
  }

  if (inst->is(AssertType, CheckType)) {
    // Sometimes code in irgen asks for a value with DataTypeSpecific but can
    // tolerate a less specific value.  If that happens, there's nothing to
    // constrain.
    if (!typeFitsConstraint(val->type(), gc)) return false;

    return constrainRefinement(inst, gc, inst->src(0)->type());
  }

  if (inst->isPassthrough()) {
    return constrainValue(inst->getPassthroughValue(), gc);
  }

  if (inst->is(DefLabel)) {
    auto changed = false;
    auto dst = 0;
    for (; dst < inst->numDsts(); dst++) {
      if (val == inst->dst(dst)) break;
    }
    assertx(dst != inst->numDsts());
    for (auto& pred : inst->block()->preds()) {
      assertx(pred.inst()->is(Jmp));
      auto src = pred.inst()->src(dst);
      changed |= constrainValue(src, gc);
    }
    return changed;
  }

  // Any instructions not special cased above produce a new value, so there's
  // no guard for us to constrain.
  ITRACE(2, "value is new in this trace, bailing\n");
  return false;
}

bool IRBuilder::constrainLocation(Location l, GuardConstraint gc,
                                  const std::string& why) {
  if (!shouldConstrainGuards() || gc.empty()) return false;

  ITRACE(1, "constraining {} to {} (for {})\n", show(l), gc, why);
  Indent _i;

  bool changed = false;
  for (auto typeSrc : m_state.typeSrcsOf(l)) {
    changed |= constrainTypeSrc(typeSrc, gc);
  }
  return changed;
}

bool IRBuilder::constrainLocation(Location l, GuardConstraint gc) {
  return constrainLocation(l, gc, "");
}

bool IRBuilder::constrainLocal(uint32_t locID, GuardConstraint gc,
                               const std::string& why) {
  return constrainLocation(loc(locID), gc, why);
}

bool IRBuilder::constrainStack(IRSPRelOffset offset, GuardConstraint gc) {
  return constrainLocation(stk(offset), gc);
}

bool IRBuilder::constrainTypeSrc(TypeSource typeSrc, GuardConstraint gc) {
  if (!shouldConstrainGuards() || gc.empty()) return false;

  ITRACE(1, "constraining type source {} to {}\n", show(typeSrc), gc);
  Indent _i;

  if (typeSrc.isValue()) return constrainValue(typeSrc.value, gc);

  assertx(typeSrc.isGuard());
  auto const guard = typeSrc.guard;

  always_assert(guard->is(AssertLoc, AssertStk, AssertMBase));

  // If the dest of the Assert doesn't fit `gc', there's no point in
  // continuing.
  auto prevType = get_required(m_constraints.prevTypes, guard);
  if (!typeFitsConstraint(prevType.refine(guard->typeParam()), gc)) {
    return false;
  }

  if (m_constraints.guards.contains(guard)) {
    return constrainGuard(guard, gc, prevType);
  }

  return constrainRefinement(guard, gc, prevType);
}

/*
 * Constrain the sources of an ordinary assertion or check. Its typeParam is
 * not relaxed, so only constrain sources for information it doesn't provide.
 */
bool IRBuilder::constrainRefinement(const IRInstruction* inst,
                                    GuardConstraint gc, Type srcType) {
  assertx(shouldConstrainGuards());

  auto const knownType = inst->typeParam();

  // If the known type fits the constraint, we're done.
  if (typeFitsConstraint(knownType, gc)) return false;

  auto const newGC = relaxConstraint(gc, knownType, srcType);
  ITRACE(1, "tracing through {}, orig gc: {}, new gc: {}\n",
         *inst, gc, newGC);

  if (inst->is(AssertType, CheckType)) {
    return constrainValue(inst->src(0), newGC);
  }

  auto changed = false;
  auto const& typeSrcs = get_required(m_constraints.typeSrcs, inst);

  for (auto typeSrc : typeSrcs) {
    changed |= constrainTypeSrc(typeSrc, newGC);
  }
  return changed;
}

/*
 * Constrain a registered guard just enough to satisfy `gc', returning true
 * iff that requires a more specific constraint than its existing one.
 *
 * This does not necessarily constrain the guard, if `gc.weak' is true.
 */
bool IRBuilder::constrainGuard(const IRInstruction* inst, GuardConstraint gc,
                               Type srcType) {
  assertx(shouldConstrainGuards());

  // The location was untracked when this guard was emitted, so its incoming
  // type is TCell and there are no earlier type sources to constrain.
  // We keep the scaffolding around with the aim of eventual introduction
  // of entry assertions.
  assertx(srcType == TCell);

  auto& guard = m_constraints.guards.at(inst);

  auto const guardGC = relaxConstraint(gc, srcType, inst->typeParam());
  auto const newGC = applyConstraint(guard, guardGC);
  ITRACE(2, "constrainGuard({}, {}): {} -> {}\n", *inst, gc, guard, newGC);
  Indent _i;

  auto const changed = guard != newGC;
  if (changed && !gc.weak) guard = newGC;

  return changed;
}

uint32_t IRBuilder::numGuards() const {
  uint32_t count = 0;
  for (auto& g : m_constraints.guards) {
    if (g.second != DataTypeGeneric) count++;
  }
  return count;
}

///////////////////////////////////////////////////////////////////////////////

LocalState IRBuilder::local(uint32_t id, GuardConstraint gc) {
  constrainLocal(id, gc, "");
  return m_state.local(id);
}

StackState IRBuilder::stack(IRSPRelOffset offset, GuardConstraint gc) {
  constrainStack(offset, gc);
  return m_state.stack(offset);
}

SSATmp* IRBuilder::valueOf(Location l, GuardConstraint gc) {
  constrainLocation(l, gc, "");
  return m_state.valueOf(l);
}

Type IRBuilder::typeOf(Location l, GuardConstraint gc) {
  constrainLocation(l, gc, "");
  return m_state.typeOf(l);
}

/*
 * Wrap a local or stack ID into a Location.
 */
Location IRBuilder::loc(uint32_t id) const {
  return Location::Local { id };
}
Location IRBuilder::stk(IRSPRelOffset off) const {
  auto const fpRel = off.to<SBInvOffset>(m_state.irSPOff());
  return Location::Stack { fpRel };
}

///////////////////////////////////////////////////////////////////////////////
// Bytecode-level control flow.

bool IRBuilder::canStartBlock(Block* block) const {
  return m_state.hasStateFor(block);
}

bool IRBuilder::startBlock(Block* block, bool hasUnprocessedPred) {
  assertx(block);
  assertx(m_savedBlocks.empty());  // No bytecode control flow in exits.

  if (block == m_curBlock) return true;

  // Return false if we don't have a FrameState saved for `block' yet
  // -- meaning it isn't reachable from the entry block yet.
  if (!canStartBlock(block)) return false;

  // There's no reason for us to be starting on the entry block when it's not
  // our current block.
  always_assert(!block->isEntry());

  auto& lastInst = m_curBlock->back();
  always_assert(lastInst.isBlockEnd());
  always_assert(lastInst.isTerminal() || m_curBlock->next() != nullptr);

  m_state.finishBlock(m_curBlock);
  m_curBlock = block;

  m_state.startBlock(m_curBlock, hasUnprocessedPred);
  always_assert(m_state.sp() != nullptr);
  always_assert(m_state.fp() != nullptr);

  // After setting up the frame state for the block we are starting, ensure the
  // bytecode marker is as up to date as possible.  Normally starting another
  // bytecode will update the bytecode marker, but sometimes we will first
  // generate guard instructions, and they will have incorrect marker data
  // unless we restore it here.
  auto const& marker = curMarker();
  setCurMarker(BCMarker {
    marker.sk(),
    fs().bcSPOff(),
    fs().stublogue(),
    marker.profTransIDs(),
    fs().fp(),
    fs().fixupFP(),
    fs().sp()
  });
  FTRACE(2, "IRBuilder switching to block B{}:\n{}\n", block->id(),
         m_state.show());
  return true;
}

Block* IRBuilder::makeBlock(SrcKey sk, uint64_t profCount) {
  auto it = m_skToBlockMap.find(sk);
  if (it == m_skToBlockMap.end()) {
    auto const block = m_unit.defBlock(profCount);
    m_skToBlockMap.emplace(sk, block);
    return block;
  }
  return it->second;
}

bool IRBuilder::hasBlock(SrcKey sk) const {
  return m_skToBlockMap.contains(sk);
}

void IRBuilder::setBlock(SrcKey sk, Block* block) {
  assertx(!hasBlock(sk));
  m_skToBlockMap[sk] = block;
}

void IRBuilder::appendBlock(Block* block) {
  m_state.finishBlock(m_curBlock);
  FTRACE(2, "appending B{}\n", block->id());
  m_state.startBlock(block, false);
  m_curBlock = block;

  if (block->numPreds() == 0) {
    FTRACE(2, "Newly-appended B{} is unreachable!\n", block->id());
    gen(Unreachable, ASSERT_REASON);
  }
}

void IRBuilder::resetOffsetMapping() {
  m_skToBlockMap.clear();
}

IRBuilder::SkToBlockMap IRBuilder::saveAndClearOffsetMapping() {
  return std::move(m_skToBlockMap);
}

void IRBuilder::restoreOffsetMapping(SkToBlockMap&& offsetMapping) {
  m_skToBlockMap = std::move(offsetMapping);
}

Block* IRBuilder::getEHBlock(SrcKey sk) const {
  auto const it = m_skToEHBlockMap.find(sk);
  return it != m_skToEHBlockMap.end() ? it->second : nullptr;
}

Block* IRBuilder::getEHDecRefBlock(Block* prev, SSATmp* value) const {
  auto const valueId = value != nullptr
    ? value->id()
    : std::numeric_limits<uint32_t>::max();
  auto const key = std::make_pair(prev->id(), valueId);
  auto const it = m_skToEHDecRefBlockMap.find(key);
  return it != m_skToEHDecRefBlockMap.end() ? it->second : nullptr;
}

void IRBuilder::setEHBlock(SrcKey sk, Block* block) {
  assertx(getEHBlock(sk) == nullptr);
  m_skToEHBlockMap[sk] = block;
}

void IRBuilder::setEHDecRefBlock(Block* prev, SSATmp* value, Block* block) {
  assertx(getEHDecRefBlock(prev, value) == nullptr);
  auto const valueId = value != nullptr
    ? value->id()
    : std::numeric_limits<uint32_t>::max();
  auto const key = std::make_pair(prev->id(), valueId);
  m_skToEHDecRefBlockMap[key] = block;
}

IRBuilder::SkToBlockMap IRBuilder::saveAndClearEHBlockMapping() {
  return std::move(m_skToEHBlockMap);
}

void IRBuilder::restoreEHBlockMapping(SkToBlockMap&& ehBlockMapping) {
  m_skToEHBlockMap = std::move(ehBlockMapping);
}

void IRBuilder::pushBlock(const BCMarker& marker, Block* b) {
  FTRACE(2, "IRBuilder::pushBlock:\n  saved: B{} @ {}\n pushed: B{} @ {}\n",
         m_curBlock->id(), curMarker().show(), b->id(), marker.show());
  assertx(b);

  m_savedBlocks.push_back(
    BlockState { m_curBlock, m_curBCContext, m_exnStack }
  );
  m_state.pauseBlock(m_curBlock);
  m_state.startBlock(b, false);
  m_curBlock = b;
  m_curBCContext = BCContext { marker, 0 };

  if (debug) {
    for (UNUSED auto const& state : m_savedBlocks) {
      assertx(state.block != b &&
             "Can't push a block that's already in the saved stack");
    }
  }
}

void IRBuilder::popBlock() {
  assertx(!m_savedBlocks.empty());

  auto const& top = m_savedBlocks.back();
  FTRACE(2, "IRBuilder::popBlock:\n  popped: B{} @ {}\n restored: B{} @ {}\n",
         m_curBlock->id(), curMarker().show(),
         top.block->id(), top.bcctx.marker.show());
  m_state.finishBlock(m_curBlock);
  m_state.unpauseBlock(top.block);
  m_curBlock = top.block;
  m_curBCContext = top.bcctx;
  m_exnStack = top.exnStack;
  m_savedBlocks.pop_back();
}

bool IRBuilder::inUnreachableState() const {
  return !m_curBlock->empty() && m_curBlock->back().isTerminal();
}

///////////////////////////////////////////////////////////////////////////////

}
