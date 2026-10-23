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
#include "hphp/runtime/vm/jit/block.h"
#include "hphp/runtime/vm/jit/cfg.h"
#include "hphp/runtime/vm/jit/ir-unit.h"
#include "hphp/runtime/vm/jit/mutation.h"
#include "hphp/runtime/vm/jit/pass-tracer.h"
#include "hphp/runtime/vm/jit/timer.h"

namespace HPHP::jit {

TRACE_SET_MOD(hhir_cfg)

namespace {

// Different from simplify(), this works even when `block' is unreachable.
// Returns true iff changes are made.
bool convertCondBranchToJmp(IRUnit& unit, Block* block) {
  auto takenBlk = block->taken();
  if (takenBlk == nullptr) return false;
  auto nextBlk = block->next();
  if (nextBlk == nullptr) return false;

  auto& term = block->back();
  // Only look at some conditional branches with no side effects.
  if (!term.is(JmpZero,
               JmpNZero,
               CheckRDSInitialized)) {
    return false;
  }

  // Case 1: both ways lead to the same block.
  bool isUnconditional = (nextBlk == takenBlk);

  // Case 2: branch constantly goes one way.
  if (!isUnconditional && term.numSrcs() == 1) {
    auto const src = term.src(0);
    if (src->hasConstVal() && term.is(JmpZero, JmpNZero)) {
      auto const v = src->isA(TBool) ? src->boolVal()
                                     : static_cast<bool>(src->intVal());
      if (v == term.is(JmpZero)) {
        // Branch never taken.
        takenBlk = block->next();
      }
      isUnconditional = true;
    }
  }

  if (isUnconditional) {
    assertx(takenBlk);
    auto const bcctx = term.bcctx();
    term.convertToNop();                // Removes edges to original dests.
    block->push_back(unit.gen(Jmp, bcctx, takenBlk));
    FTRACE(1, "Replaced conditional branch in B{} with Jmp to B{}.\n",
           block->id(), takenBlk->id());
    return true;
  }
  return false;
}

// If `block' only goes to `takenBlk' and `takenBlk' can only be reached
// directly from `block', merge them and leave `takenBlk' unreachable.
// Returns true iff merging happend.
bool absorbDstBlock(IRUnit& unit, Block* block) {
  auto& term = block->back();
  if (!term.is(Jmp)) return false;

  auto takenBlk = block->taken();
  if (takenBlk == block) return false;
  if (takenBlk->numPreds() != 1) return false;

  // Replace DefLabel with Mov's
  if (takenBlk->begin()->is(DefLabel)) {
    auto& defLabel = *takenBlk->begin();
    for (auto i = 0; i < defLabel.numDsts(); ++i) {
      auto mov = unit.gen(Mov, defLabel.bcctx(), term.src(i));
      mov->setDst(defLabel.dst(i));
      mov->dst()->setInstruction(mov);
      block->push_back(mov);
    }
    defLabel.convertToNop();
  }

  term.convertToNop();
  block->splice(block->end(), takenBlk, takenBlk->skipHeader(),
                takenBlk->end());

  // takenBlk is in a zombie now, it will be removed later.
  assertx(takenBlk->numPreds() == 0);
  takenBlk->instrs().clear();
  FTRACE(1, "Merged B{} into B{}\n", takenBlk->id(), block->id());
  return true;
}

// Jmp->(Jmp2|Jcc)->...   ==> (Jmp2|Jcc)->...
// Jcc->(non-phi)Jmp->... ==> Jcc->...
// Returns true iff changes were made.
bool foldJmp(IRUnit& unit, Block* block) {
  auto& term = block->back();

  if (term.is(Jmp)) {
    auto takenBlk = block->taken();
    if (takenBlk == block) return false;
    // If we reach here, takenBlk cannot be merged into block as it has other
    // predecessors. We could duplicate whatever code in takenBlk in block, but
    // we only do it if takenBlk contains only a cheap instruction.
    if (takenBlk->begin()->is(Jmp, JmpZero, JmpNZero)) {
      block->back().become(unit, &*takenBlk->begin());
      FTRACE(1, "Duplicated B{} into B{}\n", takenBlk->id(), block->id());
      // takenBlk is probably still reachable from other blocks.
      return true;
    }
    return false;
  }

  if (auto next = term.next()) {
    auto jmp = next->begin();
    if (jmp->is(Jmp) && jmp->numSrcs() == 0) {
      assertx(jmp->taken());
      FTRACE(1, "Setting {} next to skip {}\n", term, *jmp);
      term.setNext(jmp->taken());
      return true;
    }
  }

  if (auto taken = term.taken()) {
    auto jmp = taken->begin();
    if (jmp->is(Jmp) && jmp->numSrcs() == 0) {
      assertx(jmp->taken());
      FTRACE(1, "Setting {} taken to skip {}\n", term, *jmp);
      term.setTaken(jmp->taken());
      return true;
    }
  }

  return false;
}

/*
 * Convert a diamond where each branch only forwards SSATmps to the join point
 * into some number of Select instructions. The instructions are inserted at the
 * head of the diamond, along with a jump to the join point. The diamond
 * branches will be later dead code eliminated if they are now dead. If this is
 * the only predecessor of the join point, it will be merged into the diamond
 * head.
 */
bool collapseDiamond(IRUnit& unit, Block* block) {
  auto& term = block->back();

  // Is this block the head of a suitable diamond?

  if (!term.is(JmpZero, JmpNZero)) return false;

  auto const next = block->next();
  auto const taken = block->taken();
  if (next == taken) return false;

  // Both sides of the diamond must consist of nothing but a Jmp to a common
  // join point and must forward at least one value to the join point.
  auto const nextJmp = next->begin();
  if (!nextJmp->is(Jmp) || !nextJmp->numSrcs()) return false;
  auto const takenJmp = taken->begin();
  if (!takenJmp->is(Jmp) || !takenJmp->numSrcs()) return false;

  if (next->taken() != taken->taken()) return false;

  assertx(nextJmp->numSrcs() == takenJmp->numSrcs());

  auto const join = next->taken();
  assertx(join->begin()->is(DefLabel));
  assertx(join->begin()->numDsts() == nextJmp->numSrcs());

  // For every value forwarded to the join point, generate a Select
  // at the head of the diamond. Each Select chooses between the
  // value provided on the left or right side of the diamond.
  std::vector<SSATmp*> newSrcs{nextJmp->numSrcs()};
  for (uint32_t i = 0; i < nextJmp->numSrcs(); ++i) {
    auto const t = term.is(JmpZero) ? nextJmp->src(i) : takenJmp->src(i);
    auto const f = term.is(JmpZero) ? takenJmp->src(i) : nextJmp->src(i);
    auto select = unit.gen(Select, term.bcctx(), term.src(0), t, f);
    block->insert(block->backIter(), select);
    newSrcs[i] = select->dst();
  }

  // Instead of conditionally jumping into either branch in the diamond, now
  // forward the selected values into the join point.
  auto newJmp = unit.gen(Jmp, term.bcctx(), join,
                         std::make_pair(newSrcs.size(), newSrcs.data()));
  term.convertToNop();
  block->push_back(newJmp);

  FTRACE(1, "Collapsed B{} -> B{}/B{} -> B{} diamond.\n",
         block->id(), next->id(), taken->id(), join->id());

  return true;
}

}

/*
 * This pass tries to merge blocks and cleanup the CFG.
 *
 * In each pass, it visits blocks in reverse post order and tries to
 * (1) convert the conditional branch at the end of the block into a Jmp;
 * (2) merge the block with its unique successor block, if it is the unique
 *     predecessor of its successor;
 * (3) fold Jmps, if it fits the Jmp -> Jmp|Jcc or Jcc -> Jmp pattern.
 * (4) convert control flow diamonds to Select instructions
 *
 * The reverse post order is not essential to the transformation; in the current
 * implementation it helps skipping some blocks after a change happens.
 */
void cleanCfg(IRUnit& unit) {
  PassTracer tracer { &unit, Trace::hhir_cfg, "cleanCfg" };
  Timer timer(Timer::optimize_cleancfg, unit.logEntry().get_pointer());
  bool changed = false;
  do {
    auto const blocks = rpoSortCfg(unit);
    for (auto block : blocks) {
      // Skip malformed unreachable blocks that can appear transiently.
      if (block->empty()) continue;
      if (block->numPreds() == 0 && block != unit.entry()) continue;

      // Keep working on the current block until no further changes are made.
      for ( ; ; changed = true) {
        if (convertCondBranchToJmp(unit, block)) continue;
        if (absorbDstBlock(unit, block)) continue;
        if (foldJmp(unit, block)) continue;
        if (collapseDiamond(unit, block)) continue;
        break;
      }
    }
  } while (removeUnreachable(unit));

  // If any block is removed, reflow all types.
  if (changed) reflowTypes(unit);
}

}
