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

#pragma once

#include "hphp/runtime/vm/jit/block.h"
#include "hphp/runtime/vm/jit/cfg.h"
#include "hphp/runtime/vm/jit/containers.h"
#include "hphp/runtime/vm/jit/frame-state.h"
#include "hphp/runtime/vm/jit/guard-constraint.h"
#include "hphp/runtime/vm/jit/guard-constraints.h"
#include "hphp/runtime/vm/jit/ir-opcode.h"
#include "hphp/runtime/vm/jit/ir-unit.h"
#include "hphp/runtime/vm/jit/region-selection.h"
#include "hphp/runtime/vm/jit/simplify.h"
#include "hphp/runtime/vm/jit/state-vector.h"
#include "hphp/runtime/vm/jit/type.h"

#include <folly/ScopeGuard.h>

#include <functional>

namespace HPHP::jit::irgen {

///////////////////////////////////////////////////////////////////////////////

struct ExnStackState {
  SBInvOffset syncedSpLevel{0};
};

/*
 * This module provides the basic utilities for generating the IR instructions
 * in a trace and emitting control flow. It also performs some optimizations
 * while generating IR, and may be reinvoked for a second optimization pass.
 *
 * This module is also responsible for organizing a few types of
 * gen-time optimizations:
 *
 *   - preOptimize pass
 *
 *      Before an instruction is linked into the trace, IRBuilder
 *      internally runs preOptimize() on it, which can do some
 *      tracelet-state related modifications to the instruction.  For
 *      example, it can eliminate redundant guards.
 *
 *   - simplification pass
 *
 *      After the preOptimize pass, IRBuilder calls out to
 *      Simplifier to perform state-independent optimizations, like
 *      copy propagation and strength reduction.  (See simplify.h.)
 */
struct IRBuilder {
  IRBuilder(IRUnit&, const Func*);

  /*
   * Accessors.
   */
  IRUnit& unit() const { return m_unit; }
  FrameStateMgr& fs() { return m_state; }
  const BCMarker& curMarker() const { return m_curBCContext.marker; }

  /*
   * Get the current BCContext, incrementing its `iroff'.
   */
  BCContext nextBCContext() {
    return BCContext { m_curBCContext.marker, m_curBCContext.iroff++ };
  }

  /*
   * Update the current BCContext.
   */
  void setCurMarker(const BCMarker&);
  void resetCurIROff(uint16_t off = 0) { m_curBCContext.iroff = off; }

  /*
   * Exception handling and IRBuilder.
   *
   * Normally HHBC opcodes that throw don't have any effects before they throw.
   * By default, when you gen() instructions that could throw, IRBuilder
   * automatically creates catch blocks that take the current frame-state
   * information, except spill the stack as if the instruction has not yet
   * started.
   *
   * There are some exceptions, and so there are two ways to modify this
   * behavior.  If an HHBC opcode should have some effects on the stack prior
   * to throwing, the lowering function can call exceptionStackBoundary after
   * doing this to inform IRBuilder that it's not a bug---in this case the
   * automatically created catch blocks will spill the stack as of the last
   * boundary.
   *
   * The other way is to set a custom catch creator function.  This is
   * basically for the minstr instructions, which has various temporary stack
   * state to clean up during unwinding.
   */
  void exceptionStackBoundary();
  const ExnStackState& exceptionStackState() const { return m_exnStack; }

  /*
   * Tracked state for bytecode locations.
   *
   * These simply constrain the location, then delegate to fs().
   */
  LocalState local(uint32_t id, GuardConstraint gc);
  StackState stack(IRSPRelOffset offset, GuardConstraint gc);
  SSATmp* valueOf(Location l, GuardConstraint gc);
  Type     typeOf(Location l, GuardConstraint gc);

  /////////////////////////////////////////////////////////////////////////////
  /*
   * Guard relaxation.
   *
   * Whenever the semantics of an HHIR instruction depends on the type of one
   * of its input values, that value's type must be constrained using one of
   * these functions. This happens automatically for most values, when obtained
   * through irgen-internal functions like popC (and friends).
   */

  /*
   * Enable guard constraining for this IRBuilder. This may disable some
   * optimizations.
   */
  void enableConstrainGuards();

  /*
   * Emit and register a tracelet guard assumption for an untracked location.
   * TCell requires no guard. Only used while constraining guards for region
   * selection; the emitted assertion does not perform a runtime check.
   */
  void guardType(Location, Type);

  /*
   * All the guards in the managed IRUnit.
   */
  const GuardConstraints* guards() const { return &m_constraints; }

  /*
   * Trace back to the guard that provided the type of `val', if any, then
   * constrain it so that its type will not be relaxed beyond `gc'.
   *
   * Returns true iff a guard needs a tighter constraint, and does not change
   * constraints if `gc.weak' is true.
   */
  bool constrainValue(SSATmp* const val, GuardConstraint gc);

  /*
   * Constrain the type sources of the given bytecode location.
   */
  bool constrainLocation(Location l, GuardConstraint gc);
  bool constrainLocal(uint32_t id, GuardConstraint gc, const std::string& why);
  bool constrainStack(IRSPRelOffset offset, GuardConstraint gc);

  /*
   * Returns the number of registered guards with non-generic type constraints.
   */
  uint32_t numGuards() const;

  /////////////////////////////////////////////////////////////////////////////
  // Bytecode-level control flow helpers.

  /*
   * The block that we're currently emitting code to.
   */
  Block* curBlock() { return m_curBlock; }

  /*
   * Return whether we have state saved for `block'---which indicates that it's
   * currently reachable from the unit's entry block.
   */
  bool canStartBlock(Block* block) const;

  /*
   * Start `block', returning success.
   *
   * We fail if `canStartBlock(block)' is false.
   */
  bool startBlock(Block* block, bool hasUnprocessedPred);

  /*
   * Create a new block corresponding to bytecode control flow.
   */
  Block* makeBlock(SrcKey sk, uint64_t profCount);

  /*
   * Check or set the block corresponding to `sk'.
   */
  bool hasBlock(SrcKey sk) const;
  void setBlock(SrcKey sk, Block* block);

  /*
   * Append `block' to the unit.
   *
   * This method is used to implement IR-level control-flow helpers. When doing
   * control flow, we may simplify branching instructions into direct jumps to
   * next or taken, in which case some blocks that we append are unreachable.
   *
   * We can detect this case by checking whether `block` has any predecessors -
   * all incoming edges should be created before calling this method.
   */
  void appendBlock(Block* block);

  /*
   * If the final instruction in the current block is a block end and no next
   * block has been assigned yet, create one.
   */
  void ensureBlockAppendable();

  /*
   * Clear the SrcKey-to-block map.
   */
  void resetOffsetMapping();

  /*
   * Save and restore the SrcKey-to-block map.
   */
  using SkToBlockMap = jit::fast_map<SrcKey, Block*, SrcKey::Hasher>;
  SkToBlockMap saveAndClearOffsetMapping();
  void restoreOffsetMapping(SkToBlockMap&& offsetMapping);

  /*
   * Get or set an exception handling block for a given EH at SrcKey assuming
   * an empty stack, or a parent block, plus a value on top of the stack.
   */
  Block* getEHBlock(SrcKey) const;
  Block* getEHDecRefBlock(Block*, SSATmp*) const;
  void setEHBlock(SrcKey, Block*);
  void setEHDecRefBlock(Block*, SSATmp*, Block*);

  /*
   * Save and restore the m_skToEHBlockMap.
   */
  SkToBlockMap saveAndClearEHBlockMapping();
  void restoreEHBlockMapping(SkToBlockMap&& skToEHBlockMap);


  /*
   * To emit code to a block other than the current block, call pushBlock(),
   * emit instructions as usual with gen(...), then call popBlock(). This is
   * best done using the BlockPusher struct:
   *
   * gen(CodeForMainBlock, ...);
   * {
   *   BlockPusher<PauseExit> bp(m_irb, marker, exitBlock);
   *   gen(CodeForExitBlock, ...);
   * }
   * gen(CodeForMainBlock, ...);
   */
  void pushBlock(const BCMarker& marker, Block* b);
  void popBlock();

  /*
   * Conditionally append a new instruction to the current Block, depending on
   * what some optimizations have to say about it.
   */
  enum class CloneFlag { Yes, No };
  SSATmp* optimizeInst(IRInstruction* inst,
                       CloneFlag doClone,
                       Block* srcBlock);

  /*
   * `inUnreachableState` will return true if IRBuilder has detected that our
   * current position is unreachable from the unit entry.
   */
  bool inUnreachableState() const;

  /////////////////////////////////////////////////////////////////////////////
  // Internal API.

private:
  template<class... Args>
  SSATmp* gen(Opcode op, Args&&... args) {
    return makeInstruction(
      [this] (IRInstruction* inst) {
        return optimizeInst(inst, CloneFlag::Yes, nullptr);
      },
      op,
      nextBCContext(),
      std::forward<Args>(args)...
    );
  }

  /*
   * Location wrapper helpers.
   */
  Location loc(uint32_t) const;
  Location stk(IRSPRelOffset) const;

  /*
   * preOptimize() and helpers.
   */
  SSATmp* preOptimizeAssertTypeOp(IRInstruction* inst,
                                  Type oldType,
                                  SSATmp* oldVal,
                                  const IRInstruction* typeSrc);
  SSATmp* preOptimizeAssertType(IRInstruction*);
  SSATmp* preOptimizeAssertLocation(IRInstruction*, Location);
  SSATmp* preOptimizeAssertLoc(IRInstruction*);
  SSATmp* preOptimizeAssertStk(IRInstruction*);
  SSATmp* preOptimizeAssertMBase(IRInstruction*);
  SSATmp* preOptimizeLdLocation(IRInstruction*, Location);
  SSATmp* preOptimizeLdLoc(IRInstruction*);
  SSATmp* preOptimizeLdStk(IRInstruction*);
  SSATmp* preOptimizeLdMem(IRInstruction*);
  SSATmp* preOptimizeLdMBase(IRInstruction*);
  SSATmp* preOptimizeLdClosureCtx(IRInstruction*);
  SSATmp* preOptimizeLdClosureThis(IRInstruction*);
  SSATmp* preOptimizeLdClosureCls(IRInstruction*);
  SSATmp* preOptimizeLdFrameCtx(IRInstruction*);
  SSATmp* preOptimizeLdFrameThis(IRInstruction*);
  SSATmp* preOptimizeLdFrameCls(IRInstruction*);
  SSATmp* preOptimizeStMem(IRInstruction*);
  SSATmp* preOptimizeStMemMeta(IRInstruction*);
  SSATmp* preOptimizeStMROProp(IRInstruction*);
  SSATmp* preOptimizeCheckMROProp(IRInstruction*);
  SSATmp* preOptimizeBaseTypeParam(IRInstruction*);
  SSATmp* preOptimizeElemDictD(IRInstruction*);
  SSATmp* preOptimizeElemDictU(IRInstruction*);
  SSATmp* preOptimizeBespokeElem(IRInstruction*);
  SSATmp* preOptimizeSetElem(IRInstruction*);
  SSATmp* preOptimize(IRInstruction*);

  void appendInstruction(IRInstruction* inst);

  /*
   * Type constraint helpers.
   */
  bool constrainLocation(Location l, GuardConstraint gc,
                         const std::string& why);
  bool constrainTypeSrc(TypeSource typeSrc, GuardConstraint gc);
  bool constrainRefinement(const IRInstruction* inst,
                           GuardConstraint gc, Type srcType);
  bool constrainGuard(const IRInstruction* inst, GuardConstraint gc,
                      Type srcType);
  bool shouldConstrainGuards() const;

  bool isMBaseLoad(const IRInstruction*) const;

  /////////////////////////////////////////////////////////////////////////////

private:
  struct BlockState {
    Block* block;
    BCContext bcctx;
    ExnStackState exnStack;
    std::function<Block* ()> catchCreator;
  };

private:
  IRUnit& m_unit;
  BCContext m_curBCContext;
  FrameStateMgr m_state;

  /*
   * m_savedBlocks will be nonempty iff we're emitting code to a block other
   * than the main block. m_curBCContext, and m_curBlock are all set from the
   * most recent call to pushBlock() or popBlock().
   */
  jit::vector<BlockState> m_savedBlocks;
  Block* m_curBlock;
  ExnStackState m_exnStack;

  bool m_enableSimplification{false};

  GuardConstraints m_constraints;
  bool m_constrainGuards{false};

  // Keep track of blocks created to support bytecode control flow.
  SkToBlockMap m_skToBlockMap;

  // A map from exception handler SKs to blocks implementing their handlers.
  SkToBlockMap m_skToEHBlockMap;

  // A map from a pair of EH block id and top of the stack value id to another
  // EH block that is responsible for popping and decreffing that value.
  jit::fast_map<std::pair<uint32_t, uint32_t>, Block*> m_skToEHDecRefBlockMap;
};

///////////////////////////////////////////////////////////////////////////////

/*
 * RAII helper for emitting code to exit traces. See IRBuilder::pushBlock
 * for usage.
 */
struct BlockPusher {
  BlockPusher(IRBuilder& irb, const BCMarker& marker, Block* block)
    : m_irb(irb)
  {
    irb.pushBlock(marker, block);
  }

  ~BlockPusher() {
    m_irb.popBlock();
  }

 private:
  IRBuilder& m_irb;
};

///////////////////////////////////////////////////////////////////////////////

}
