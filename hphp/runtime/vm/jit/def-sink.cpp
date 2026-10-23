/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-2015 Facebook, Inc. (http://www.facebook.com)     |
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
#include "hphp/runtime/vm/jit/opt.h"

#include "hphp/util/bitset-utils.h"
#include "hphp/util/dataflow-worklist.h"
#include "hphp/util/match.h"
#include "hphp/util/trace.h"
#include "hphp/util/tribool.h"

#include "hphp/runtime/vm/jit/block.h"
#include "hphp/runtime/vm/jit/cfg.h"
#include "hphp/runtime/vm/jit/dce.h"
#include "hphp/runtime/vm/jit/ir-instruction.h"
#include "hphp/runtime/vm/jit/ir-opcode.h"
#include "hphp/runtime/vm/jit/ir-unit.h"
#include "hphp/runtime/vm/jit/memory-effects.h"
#include "hphp/runtime/vm/jit/mutation.h"
#include "hphp/runtime/vm/jit/pass-tracer.h"
#include "hphp/runtime/vm/jit/simple-propagation.h"
#include "hphp/runtime/vm/jit/state-vector.h"
#include "hphp/runtime/vm/jit/timer.h"

#include <fmt/format.h>

/*
  Implement a pass which attempts to sink definitions as close to
  their first usage as possible.

  This is useful because it might sink calculations into side-exits or
  other branches where they are only needed. It also shortens
  lifetimes which reduces register pressure. Certain instructions will
  be aggressively re-materialized before every usage.

  This pass is based on an algorithm presented in the paper "Partial
  Dead Code Elimination" by Knoop, Ruthing, and Steffen, but with a
  few modifications. Namely, restoring SSA form after the sinking (the
  original paper did not consider SSA), handling arbitrary memory
  effects, an optimization dealing with the last instruction in a
  block, and "trivial" rematerializations.

  This algorithm is quite aggressive, as it will sink any definition
  as far as possible, even duplicating the definition across
  branches. It has the nice property, however, that its guaranteed to
  never duplicate work along any particular path. It can, however,
  increase static code size. It will also never sink a definition into
  a loop body deeper than where it originally was. Definitions will
  only be moved across blocks, the algorithm does not consider
  inter-block instruction movement. This can be handled in a separate
  phase, but is not currently implemented.

  This pass implements a form of partial dead code elimination. If a
  definition is duplicated across a branch, it will only be sunk where
  the definition's tmp is live.

  Certain instructions are (usually) free, in the sense that they
  calculate memory addresses and are usually folded into whatever
  instruction consumes the address. These instructions benefit from
  being aggressively rematerialized before each usage. They'll be
  folded into the (immediately following) usage and we'll avoid
  keeping their associated SSATmps live for any longer than
  necessary. This occurs most often when we sink a store to the mbase
  into a side-exit. We want to move the associated address
  calculations into the side-exit as well.

  "Memory conflict" refers to the inability to sink the definition of
  a tmp past a particular instruction because both the instruction
  defining that tmp and the other instruction conflict. Instructions
  can conflict if they potentially have a read/write or write/write
  conflict on the same piece of memory, or more generally, have
  side-effects which cannot be re-sequenced between each other.

  The algorithm is a forward dataflow analysis using the two sets
  BEGIN-DELAYED(B) and END-DELAYED(B), for each block B. If a SSATmp
  is present in BEGIN-DELAYED(B), it means it is safe to sink that tmp
  to the *beginning* of block B. If a SSATmp is present in
  END-DELAYED(B), it is safe to sink that tmp to immediately before
  the *last* instruction in block B. Since every block ends in a
  control flow or terminal instruction, it is never possible to sink a
  SSATmp to the end of the block.

  In addition, define the additional sets:

  VALID(T) = The set of tmps which are considered by the pass. These are
             tmps corresponding to instructions which are safe to sink.
  TRIVIAL(T) = The subset of VALID which corresponds to "trivial" instructions
               (those which will be rematerialized before every usage).

  (It is assumed from now on that all tmps are a member of VALID)

  BEGIN-ANTICIPATED(B) = All tmps used in B, or (transitively) by any
                         successors of B, and that are defined.
  END-ANTICIPATED(B) = All tmps used (transitively) by any successors of B,
                       or by the last instruction in B, and that are defined.

  The anticipated sets are used to determine which SSATmps are needed
  at a given point and are calculated using the typical backwards
  dataflow for liveness. Defs kill anticipated bits, which is
  important in loops.

  ALL-DEFS(B) = All tmps defined within B
  UNBLOCKED-DEFS(B) = All tmps defined within B, which do not have a
                      subsequent use or memory conflict within B.

  These are strictly per-block and require no dataflow. They are used
  to determine which tmps to add to the delayed sets during dataflow.

  BEGIN-USES(B) = All tmps used within B, except by the final instruction
  END-USES(B) = All tmps used by the final instruction in B

  BEGIN-UNBLOCKED(B) = All tmps which aren't used within B (except by last
                       instruction), or are trivial
  END-UNBLOCKED(B) = All tmps which aren't used by the last instruction within
                     B, or are trivial.

  BEGIN-CONFLICT(B) = All tmps which have some kind of memory conflict with an
                      instruction in B, not including the last instruction.
  END-CONFLICT(B) = All tmps which have some kind of memory conflict with the
                    last instruction in B.

  The conflict sets here are strictly conceptual. The implementation
  calculates conflicts on the fly.

  The update equations are:

  BEGIN-DELAYED(B) = if B is the entry block -> {}
                     otherwise -> intersection of (END-DELAYED(M) &
                                                   ~END-UNBLOCKED(M) &
                                                   ~END-CONFLICT(M))
                              for each M in PREDS(B)

  (It is safe to sink a tmp to the beginning of a block only if its
  safe to sink that tmp to the end of all its predecessors, and that
  tmp is not used by the last instruction of any predecessors, or has
  a memory conflict with the last instruction).

  END-DELAYED(B) = (BEGIN-DELAYED(B) & BEGIN-UNBLOCKED(B) & ~BEGIN-CONFLICT(B))
                   | UNBLOCKED-DEF(B)

  (It is safe to sink a tmp to the end of a block only if that tmp is
  defined in B (and does not have a subsequent use/conflict in B), or
  if that tmp is safe to sink to the beginning of B. The tmp must also
  not be used within B or have a memory conflict within B).

  BEGIN-DELAYED(B) and END-DELAYED(B) originally contain all tmps which are
  eligible for sinking in the entire unit, and the fixed-point
  solution of the sets is obtained. Then, for each block B, define the
  two sets BEGIN-INSERT(B) and END-INSERT(B). If a tmp is present in
  BEGIN-INSERT(B), than that tmp should be inserted at the beginning of
  B. If a tmp is present in END-INSERT(B), than that tmp should be
  inserted in front of the last instruction in B.

  BEGIN-INSERT(B) = (BEGIN-DELAYED(B) & BEGIN-ANTICIPATED(B) & ~ALL-DEFS(B)
                     & (BEGIN-CONFLICT(B) | ~BEGIN-UNBLOCKED(B)))
                     | (BEGIN-USES(B) & TRIVIAL(B))

  (Insert a tmp at the beginning of a block if its safe to sink it to
  the beginning of that block, it's anticipated (live), and that it
  has a usage within the block, or has a memory conflict within the
  block (this means it cannot be sunk any further). Regardless of the
  above condition, include any trivial tmps used within the block).

  END-INSERT(B) = (END-DELAYED(B) & END-ANTICIPATED(B)
                  & ~ALL-DEFS(B) & ~BEGIN-INSERT(B)
                  & (union of ~BEGIN-DELAYED(B) for each M in SUCC(B))
                  | (END-USES(B) & TRIVIAL(B)))

  (Insert a tmp at the end of a block if its safe to sink it to the
  end of that block, and that tmp is anticipated (live), and that tmp
  isn't defined in that block, and its not safe to sink it to all of
  the block's successors. Ignore any tmps already inserted at the
  beginning of the block. Regardless of the above condition, include
  any trivial tmps used within the block).

  Note that BEGIN-INSERT and END-INSERT are not recursively defined, and
  thus don't need dataflow to solve.

  Once BEGIN-INSERT and END-INSERT are solved for every block, each
  definition is cloned into the blocks given by BEGIN-INSERT and END-INSERT
  at the appropriate points. The original definition is left in place,
  and each cloned definition defines a new tmp. Each use of the
  original definition must be replaced with one of the newly defined
  tmps. However, this transformation can destroy SSA form, as a usage
  of the original definition may now be dominated by multiple cloned
  definitions. Therefore, phis in the form of new Jmp/DefLabel pairs
  may need to be inserted.

  For each new tmp which needs to be inserted, we insert the
  definition, and then walk the CFG starting from that block (in RPO
  order). Any usage of the old tmp is replaced by the new
  one. Successors dominated by the new defining block are processed as
  normal. If a successor is not dominated, however, we insert a
  DefLabel/PhiJmp on that block's predecessors. This defines a new
  tmp, so the process repeats recursively from that block.

  The entire process is repeated until we reach a fixed point.

  We do not remove the original tmp (it may not necessarily be
  dead). Some of the inserted DefLabels can also be optimized away. We
  leave it to DCE to clean these up. Since this pass will never insert
  a new definition into the block of an existing definition, it will
  always reach a fixed point.
 */

namespace HPHP::jit {

TRACE_SET_MOD(hhir_sinkdefs)

namespace {

/*
 * It is much more efficient if the bitsets we use are fixed size. Of
 * course, you don't know at compile time the biggest unit you'll
 * process. Luckily, there's no interaction between SSATmps, so we can
 * perform the pass in blocks. We use fixed size bitsets, with a
 * "start" index. If the bitset isn't sufficient to contain all
 * possible SSATmps, we'll run the pass more than once.
 */
struct SSATmpSet {
  explicit SSATmpSet(size_t start) : start{start} {}

  void set() { bits.set(); }
  void set(size_t i) {
    assertx(i >= start && i < start+bits.size());
    bits.set(i - start);
  }

  void reset() { bits.reset(); }
  void reset(size_t i) {
    assertx(i >= start && i < start+bits.size());
    bits.reset(i - start);
  }

  SSATmpSet operator~() const {
    auto copy = *this;
    copy.bits.flip();
    return copy;
  }

  SSATmpSet& operator|=(const SSATmpSet& o) {
    assertx(start == o.start);
    bits |= o.bits;
    return *this;
  }
  SSATmpSet& operator&=(const SSATmpSet& o) {
    assertx(start == o.start);
    bits &= o.bits;
    return *this;
  }
  SSATmpSet& operator-=(const SSATmpSet& o) {
    assertx(start == o.start);
    bits &= ~o.bits;
    return *this;
  }

  SSATmpSet operator|(const SSATmpSet& o) const {
    auto copy = *this;
    copy |= o;
    return copy;
  }
  SSATmpSet operator&(const SSATmpSet& o) const {
    auto copy = *this;
    copy &= o;
    return copy;
  }
  SSATmpSet operator-(const SSATmpSet& o) const {
    auto copy = *this;
    copy -= o;
    return copy;
  }

  bool operator==(const SSATmpSet& o) const {
    assertx(start == o.start);
    return bits == o.bits;
  }
  bool operator!=(const SSATmpSet& o) const {
    assertx(start == o.start);
    return bits != o.bits;
  }

  bool operator[](size_t i) const {
    if (i < start || i >= start+bits.size()) return false;
    return bits[i - start];
  }

  bool none() const { return bits.none(); }

  template<typename F> void forEach(F f) const {
    bitset_for_each_set(bits, [&] (size_t b) { f(b + start); });
  }

  static constexpr size_t kNumBits = 640;
  std::bitset<kNumBits> bits;
  size_t start;
};

DEBUG_ONLY std::string show(const SSATmpSet& s) {
  std::string out;
  auto first = true;
  s.forEach(
    [&] (size_t bit) {
      fmt::format_to(std::back_inserter(out), "{}t{}", first ? "" : ", ", bit);
      first = false;
    }
  );
  return out;
}

/*
  Data-flow state for each block. We keep track of the various sets
  using bitsets, indexed by the SSATmp id defined (we don't attempt to
  sink multi-def instructions currently). If these sets prove to be
  sufficiently sparse, a different represention may be called for. The
  conflict sets aren't stored, as they are calculated on demand.
*/
struct BlockState {
  explicit BlockState(size_t ssa_tmp_start)
    : begin_delayed{ssa_tmp_start}
    , end_delayed{ssa_tmp_start}
    , begin_anticipated{ssa_tmp_start}
    , end_anticipated{ssa_tmp_start}
    , unblocked_defs{ssa_tmp_start}
    , all_defs{ssa_tmp_start}
    , begin_uses{ssa_tmp_start}
    , end_uses{ssa_tmp_start}
    , begin_unblocked{ssa_tmp_start}
    , end_unblocked{ssa_tmp_start} {}

  uint32_t rpo_order;

  // Whether any instructions in this block will definitely or
  // definitely not cause a memory conflict.
  TriBool begin_will_conflict{TriBool::No};
  TriBool end_will_conflict{TriBool::No};

  SSATmpSet begin_delayed;
  SSATmpSet end_delayed;

  SSATmpSet begin_anticipated;
  SSATmpSet end_anticipated;

  SSATmpSet unblocked_defs;
  SSATmpSet all_defs;

  SSATmpSet begin_uses;
  SSATmpSet end_uses;

  SSATmpSet begin_unblocked;
  SSATmpSet end_unblocked;
};

struct State {
  State(IRUnit& unit,
        const BlockList& rpo_blocks,
        Optional<IdomVector>& idoms,
        size_t ssa_tmp_start)
    : unit{unit}
    , rpo_blocks{rpo_blocks}
    , idoms{idoms}
    , ssa_tmp_start{ssa_tmp_start}
    , valid{ssa_tmp_start}
    , trivial{ssa_tmp_start}
    , no_conflict{unit.numInsts()}
    , block_state{unit, BlockState{ssa_tmp_start}}
    , sunk{ssa_tmp_start} {}

  IRUnit& unit;
  const BlockList& rpo_blocks;
  Optional<IdomVector>& idoms;
  size_t ssa_tmp_start;

  SSATmpSet valid;
  SSATmpSet trivial;
  // Instructions (not SSATmps) which will never cause a memory
  // conflict.
  boost::dynamic_bitset<> no_conflict;

  StateVector<Block, BlockState> block_state;

  SSATmpSet sunk;
};

// Insertion points for each SSATmp.
struct Insertion {
  BlockSet front;
  BlockSet back;
};
using InsertionMap = jit::fast_map<SSATmp*, Insertion>;

// Keeps track of inserted Phis
using PhiMap = jit::fast_map<Block*, SSATmp*>;

/*
  Memory Conflicts:

  As stated in the algorithm description, a memory conflict between
  two instructions is when they have a read/write or write/write
  conflict on the same memory address, or have non-trivial
  side-effects that cannot be reordered amongst themselves.

  1) Instructions with non-trivial side-effects are never candidates
  for sinking and are ignored by this pass. canDCE() is currently used
  as the proxy for this.

  2) If an instruction consumes a reference of a tmp, and another
  instruction potentially has the same tmp as a source, those
  instructions cannot be sunk across each other. The instruction which
  consumes a reference may release that value, thus its not safe to
  read it afterwards. This prevents a comparison of two strings, for
  example, from being sunk past the DecRef of the strings. The only
  exception is DecRefNZ, which consumes a reference, but we know will
  never release the value. We have to be pessimistic for any types
  which can trigger recursive releases (like arrays).

  5) Only instructions with memory effects of IrrelevantEffects,
  PureLoad, or GeneralEffects can be sunk. GeneralEffects is only
  allowed if it doesn't store anything. No instructions can be sunk
  across ReturnEffects, ExitEffects, or UnknownEffects. For the
  remaining combinations, the precise affected locations are checked
  for a read/write conflict.

  Note that an instruction which defines a tmp and an instruction
  which has that tmp as a source do *not* conflict with each
  other. This seems counter-intuitive, but the def/use constraints are
  handled with the data-flow directly and don't need to be duplicated
  here.
*/

bool conflicts(const IRInstruction& instr,
               const IRInstruction& sinkee,
               const IrrelevantEffects&) {
  auto const effects = canonicalize(memory_effects(instr));
  return match<bool>(
    effects,
    [&] (const IrrelevantEffects&)   { return false; },
    [&] (const ReturnEffects&)       { return true; },
    [&] (const CallEffects&)         {
      // A Call can potentially dec-ref any counted value, so it's not
      // safe to sink an instruction which uses a counted value past
      // it.
      for (auto const src : sinkee.srcs()) {
        if (src->type().maybe(TCounted)) return true;
      }
      return false;
    },
    [&] (const GeneralEffects&)      { return false; },
    [&] (const PureLoad&)            { return false; },
    [&] (const PureStore&)           { return false; },
    [&] (const PureInlineCall&)      { return false; },
    [&] (const ExitEffects&)         { return true; },
    [&] (const UnknownEffects&)      { return true; }
  );
}

bool conflicts(const IRInstruction& instr,
               const IRInstruction& sinkee,
               const PureLoad& load) {
  auto const effects = canonicalize(memory_effects(instr));
  return match<bool>(
    effects,
    [&] (const IrrelevantEffects&)   { return false; },
    [&] (const ReturnEffects&)       { return true; },
    [&] (const CallEffects& call)    {
      // A Call can potentially dec-ref any counted value, so it's not
      // safe to sink an instruction which uses a counted value past
      // it.
      for (auto const src : sinkee.srcs()) {
        if (src->type().maybe(TCounted)) return true;
      }
      return
        load.src.maybe(call.kills) ||
        load.src.maybe(call.uninits) ||
        load.src.maybe(call.actrec) ||
        load.src.maybe(call.outputs) ||
        load.src.maybe(AHeapAny) ||
        load.src.maybe(ARdsAny);
    },
    [&] (const GeneralEffects& g)    {
      return
        load.src.maybe(g.stores) ||
        load.src.maybe(g.kills) ||
        load.src.maybe(g.inout);
    },
    [&] (const PureLoad&)            { return false; },
    [&] (const PureStore& store)     { return load.src.maybe(store.dst); },
    [&] (const PureInlineCall& call) { return load.src.maybe(call.base); },
    [&] (const ExitEffects&)         { return true; },
    [&] (const UnknownEffects&)      { return true; }
  );
}

bool conflicts(const IRInstruction& instr,
               const IRInstruction& sinkee,
               const GeneralEffects& g) {
  assertx(g.stores == AEmpty && g.kills == AEmpty && g.inout == AEmpty);

  auto const test_reads = [&] (const AliasClass& acls) {
    if (g.loads.maybe(acls)) return true;
    for (auto const& frame : g.backtrace) {
      if (frame.maybe(acls)) return true;
    }
    return false;
  };

  auto const effects = canonicalize(memory_effects(instr));
   return match<bool>(
    effects,
    [&] (const IrrelevantEffects&)   { return false; },
    [&] (const ReturnEffects&)       { return true; },
    [&] (const CallEffects& call)    {
      // A Call can potentially dec-ref any counted value, so it's not
      // safe to sink an instruction which uses a counted value past
      // it.
      for (auto const src : sinkee.srcs()) {
        if (src->type().maybe(TCounted)) return true;
      }
      return
        test_reads(call.kills) ||
        test_reads(call.uninits) ||
        test_reads(call.actrec) ||
        test_reads(call.outputs) ||
        test_reads(AHeapAny) ||
        test_reads(ARdsAny);
    },
    [&] (const GeneralEffects& g2)   {
      // Two general effects instructions where one may not be DCEd must
      // conflict.  Non DCE able instructions might have effects not
      // enumerated by memory effects.  Notably today we omit certain locations
      // from being analyzed by memory effects.  Assuming no conlicting effects
      // means the instructions do not conflict omits the fact they might
      // conflict on a non tracked location.  Today such locations are read and
      // written to using may_load_store(AEmpty, AEmpty).
      if (!canDCE(instr) || !canDCE(sinkee)) return true;

      return
        test_reads(g2.stores) ||
        test_reads(g2.kills) ||
        test_reads(g2.inout);
    },
    [&] (const PureLoad&)            { return false; },
    [&] (const PureStore& store)     { return test_reads(store.dst); },
    [&] (const PureInlineCall& call) { return test_reads(call.base); },
    [&] (const ExitEffects&)         { return true; },
    [&] (const UnknownEffects&)      { return true; }
  );
}

bool conflicts(const IRInstruction& sinkee, const IRInstruction& barrier) {
  // An instruction never conflicts with itself.
  if (&sinkee == &barrier) return false;

  // Technically okay, but tends to pointlessly sink a LdLoc/LdStk
  // across both sides of the check.
  if (barrier.is(CheckSurpriseFlags)) {
    ITRACE(4, "{} conflicts with {} (surprise flag check)()\n",
           sinkee, barrier);
    return true;
  }

  if (!barrier.is(DecRefNZ)) {
    // We need to check if the barrier can potentially trigger a
    // DecRef which would release sinkee's def.
    for (auto const src : sinkee.srcs()) {
      if (!src->type().maybe(TCounted)) continue;
      for (int j = 0; j < barrier.numSrcs(); ++j) {
        if (!barrier.consumesReference(j)) continue;
        auto const consumed = barrier.src(j);
        if (consumed->type().maybe(TArrLike | TObj) ||
            src->type().maybe(consumed->type())) {
          ITRACE(4, "{} conflicts with {} (consumes ref)\n", sinkee, barrier);
          return true;
        }
      }
    }
  }

  auto const effects = canonicalize(memory_effects(sinkee));
  auto const memory_effect_conflict = match<bool>(
    effects,
    [&] (const IrrelevantEffects& x) { return conflicts(barrier, sinkee, x); },
    [&] (const GeneralEffects& x)    { return conflicts(barrier, sinkee, x); },
    [&] (const PureLoad& x)          { return conflicts(barrier, sinkee, x); },
    [&] (const ReturnEffects&)       { always_assert(false); return true; },
    [&] (const CallEffects&)         { always_assert(false); return true; },
    [&] (const PureStore&)           { always_assert(false); return true; },
    [&] (const PureInlineCall&)      { always_assert(false); return true; },
    [&] (const ExitEffects&)         { always_assert(false); return true; },
    [&] (const UnknownEffects&)      { always_assert(false); return true; }
  );

  if (memory_effect_conflict) {
    ITRACE(4, "{} conflicts with {} (memory effects)\n", sinkee, barrier);
    return true;
  }

  ITRACE(4, "{} does not conflict with {}\n", sinkee, barrier);
  return false;
}

/*
 * Check if an instruction will definitely (or definitely not) cause a
 * memory conflict, regardless of any other instructions. This is just
 * used by optimization. It's always safe to return Maybe.
 */
TriBool will_conflict(const IRInstruction& inst) {
  if (inst.is(CheckSurpriseFlags)) return TriBool::Yes;

  // Be conservative for anything ref-counted
  for (auto const src : inst.srcs()) {
    if (src->type().maybe(TCounted)) return TriBool::Maybe;
  }

  auto const effects = canonicalize(memory_effects(inst));
  return match<TriBool>(
    effects,
    [&] (const IrrelevantEffects&)   { return TriBool::No; },
    [&] (const GeneralEffects& g)    {
      if (g.loads != AEmpty || g.stores != AEmpty || g.kills != AEmpty ||
          g.inout != AEmpty) {
        return TriBool::Maybe;
      }
      return TriBool::No;
    },
    [&] (const PureLoad& x)          { return TriBool::Maybe; },
    [&] (const ReturnEffects&)       { return TriBool::Yes; },
    [&] (const CallEffects&)         { return TriBool::Maybe; },
    [&] (const PureStore&)           { return TriBool::Maybe; },
    [&] (const PureInlineCall&)      { return TriBool::Maybe; },
    [&] (const ExitEffects&)         { return TriBool::Yes; },
    [&] (const UnknownEffects&)      { return TriBool::Yes; }
  );
}

/*
 * These instructions are essentially free and are profitable to be
 * placed before each usage.
 */
bool is_trivially_sinkable(const IRInstruction& inst) {
  return inst.is(LdLocAddr, LdStkAddr, LdMIStateTempBaseAddr,
                 LdRDSAddr, ConvPtrToLval);
}

/*
 * Given a set of tmps, and an instruction, return a set with any tmps which
 * conflict with this instruction removed.
 */
SSATmpSet remove_conflicts(const State& state,
                           SSATmpSet tmps,
                           const IRInstruction& instr,
                           TriBool will_conflict) {
  switch (will_conflict) {
    case TriBool::No:
      return tmps;
    case TriBool::Yes:
      tmps.reset();
      return tmps;
    case TriBool::Maybe:
      break;
  }
  if (state.no_conflict[instr.id()]) return tmps;

  tmps.forEach(
    [&] (size_t bit) {
      auto const tmp_instr = state.unit.findSSATmp(bit)->inst();
      if (state.no_conflict[tmp_instr->id()]) return;
      if (!conflicts(*tmp_instr, instr)) return;
      tmps.reset(bit);
    }
  );
  return tmps;
}

/*
 * Given a set of tmps, and a block, return a set with any temps which conflict
 * with any instructions in this block removed. Ignore the last instruction.
 */
SSATmpSet remove_conflicts(const State& state,
                           SSATmpSet tmps,
                           const Block& block) {
  // We can short-circuit the entire block if we statically know it
  // will/won't conflict.
  switch (state.block_state[block].begin_will_conflict) {
    case TriBool::No:
      return tmps;
    case TriBool::Yes:
      tmps.reset();
      return tmps;
    case TriBool::Maybe:
      break;
  }

  // Otherwise check each block:
  for (auto const& block_instr : block) {
    if (block_instr.isBlockEnd()) continue;
    if (state.no_conflict[block_instr.id()]) continue;
    tmps.forEach(
      [&] (size_t bit) {
        auto const tmp_instr = state.unit.findSSATmp(bit)->inst();
        if (state.no_conflict[tmp_instr->id()]) return;
        if (!conflicts(*tmp_instr, block_instr)) return;
        tmps.reset(bit);
      }
    );
  }
  return tmps;
}

/*
 * Like remove_conflicts, but with the sense reversed. Return tmps
 * will *will* conflict.
 */
SSATmpSet remove_non_conflicts(const State& state,
                               SSATmpSet tmps,
                               const Block& block) {
  switch (state.block_state[block].begin_will_conflict) {
    case TriBool::No:
      tmps.reset();
      return tmps;
    case TriBool::Yes:
      return tmps;
    case TriBool::Maybe:
      break;
  }

  tmps.forEach(
    [&] (size_t bit) {
      auto const tmp_instr = state.unit.findSSATmp(bit)->inst();
      if (!state.no_conflict[tmp_instr->id()]) {
        for (auto const& block_instr : block) {
          if (block_instr.isBlockEnd()) continue;
          if (!state.no_conflict[block_instr.id()] &&
              conflicts(*tmp_instr, block_instr)) {
            return;
          }
        }
      }
      tmps.reset(bit);
    }
  );
  return tmps;
}

/*
 * Starting at the given block, rewrite all usages of the old tmp to
 * use the new tmp instead.
 */
void rewrite_uses(State& state,
                  Block& start,
                  SSATmp* old_tmp,
                  SSATmp* new_tmp,
                  const Insertion& insertions,
                  PhiMap& phis,
                  bool start_at_front) {
  ITRACE(3, "Rewriting t{} to t{} starting at {} of block {}:\n",
         old_tmp->id(), new_tmp->id(),
         start_at_front ? "front" : "back", start.id());
  Trace::Indent indenter;

  // We should only be replacing valid tmps
  assertx(state.valid[old_tmp->id()]);
  assertx(state.sunk[old_tmp->id()]);
  assertx(!state.valid[new_tmp->id()]);
  assertx(!state.sunk[new_tmp->id()]);

  // Use a worklist for the block processing:
  BlockSet visited;
  jit::stack<Block*> worklist;

  visited.emplace(&start);
  worklist.push(&start);

  auto const rewrite = [&] (IRInstruction& inst) {
    for (int i = 0; i < inst.numSrcs(); ++i) {
      if (inst.src(i) != old_tmp) continue;
      ITRACE(4, "Rewriting src #{} of {}\n", i, inst);
      inst.setSrc(i, new_tmp);
    }
  };

  // Rewrite all usages in the block. Return false if processing along
  // this path should stop.
  auto const rewrite_block = [&] (Block* block) {
    if (start_at_front) {
      // If we're starting at the front of the block, check if this
      // block is handled by a different insertion. If it is, we
      // should stop. That insertion will handle rewrites.
      assertx(state.block_state[block].begin_anticipated[old_tmp->id()]);
      if (block != &start && insertions.front.count(block)) {
        ITRACE(4, "Front of block {} has a different insertion\n", block->id());
        return false;
      }
      // Only deal with instructions if there's a usage here.
      if (state.block_state[block].begin_uses[old_tmp->id()]) {
        for (auto& inst : *block) {
          if (!inst.isBlockEnd()) rewrite(inst);
        }
      }
      // We handled everything but the last instruction. Do the same
      // check for a different insertion for that.
      if (insertions.back.contains(block)) {
        ITRACE(4, "Back of block{} has a different insertion\n", block->id());
        return false;
      }
    } else {
      // We're starting at the end of the block. If there's an
      // insertion for the back of this block, stop processing. The
      // other insertion will handle this.
      assertx(state.block_state[block].end_anticipated[old_tmp->id()]);
      // From now on we'll start at the the front in all successors
      start_at_front = true;
      if (block != &start && insertions.back.count(block)) {
        ITRACE(4, "Back of block {} has a different insertion\n", block->id());
        return false;
      }
    }
    // We either started at the end of the block, or we did the front
    // first. Either way, handle the last instruction now.
    if (state.block_state[block].end_uses[old_tmp->id()]) {
      rewrite(block->back());
    }
    return true;
  };

  do {
    auto block = worklist.top();
    worklist.pop();

    ITRACE(
      4, "Processing block {} starting at {}\n",
      block->id(), start_at_front ? "front" : "back"
    );
    if (!rewrite_block(block)) continue;

    // Enqueue any successors for further processing:
    block->forEachSucc(
      [&] (Block* succ) {
        // Already visited, so no need to enqueue
        if (visited.contains(succ)) {
          ITRACE(4, "Skipping enqueue of successor {} since already visited\n",
                 succ->id());
          return;
        }

        // Old SSATmp isn't alive in successor, so there can't be any
        // usages to replace.
        if (!state.block_state[succ].begin_anticipated[old_tmp->id()]) {
          ITRACE(
            4, "Skipping enqueue of successor {} since t{} isn't live there\n",
            succ->id(), old_tmp->id()
          );
          return;
        }

        // If this successor has more than one predecessor, it might
        // be on a dominance frontier (remember critical edges are
        // split).
        if (succ->numPreds() > 1) {
          // We already inserted a phi here. We would have already
          // rewritten the Jmp source above, so nothing further to do.
          if (phis.contains(succ)) {
            ITRACE(
              4,
              "Skipping enqueue of successor {} since a phi has been "
              "placed there\n",
              succ->id()
            );
            return;
          }

          // We need dominance information now. Calculate it if we
          // haven't already.
          if (!state.idoms) {
            ITRACE(4, "Calculating dominators\n");
            state.idoms = findDominators(
              state.unit,
              state.rpo_blocks,
              numberBlocks(state.unit, state.rpo_blocks)
            );
          }

          // Successor is not dominated by the start block (where the
          // new definition will go). We need to insert a Phi.
          if (!dominates(&start, succ, *state.idoms)) {
            ITRACE(
              4, "Successor {} is not dominated by {}. Adding phi\n",
              succ->id(), start.id()
            );
            // The Jmps originally all take the old SSATmp as their
            // input. They'll be rewritten as necessary by later
            // rewrite_uses calls.
            jit::hash_map<Block*, SSATmp*> inputs;
            succ->forEachPred(
              [&] (Block* pred) {
                assertx(state.block_state[pred].end_anticipated[old_tmp->id()]);
                if (pred != block) {
                  state.block_state[pred].end_uses.set(old_tmp->id());
                }
                inputs.emplace(pred, old_tmp);
              }
            );
            inputs[block] = new_tmp;
            auto const def_tmp = insertPhi(state.unit, succ, inputs);
            phis.emplace(succ, def_tmp);
            ITRACE(4, "Added phi {}\n", *def_tmp->inst());
            visited.emplace(succ);
            // The DefLabel defined a new SSATnp, so repeat the
            // process from the successor block.
            rewrite_uses(
              state,
              *succ,
              old_tmp,
              def_tmp,
              insertions,
              phis,
              true
            );
            return;
          }
        }

        ITRACE(4, "Enqueuing successor {}\n", succ->id());
        visited.emplace(succ);
        worklist.push(succ);
      }
    );
  } while (!worklist.empty());
}

/*
 * If we sink an instruction, we may extend the lifetimes of any of
 * that instruction's sources. As a result, those SSATmps may be
 * anticipated now where they weren't before. Since we rely on the
 * accuracy of the anticipated bits when sinking SSATmps, we need to
 * fix them up.
 *
 * Start at the given block and proceed to all predecessors. If the
 * current block is already anticipated, we can stop. Otherwise, mark
 * it as anticipated and process to predecessors.
 */
void fixup_anticipated(State& state, SSATmp* tmp, Block& start) {
  if (state.block_state[start].begin_anticipated[tmp->id()]) return;

  ITRACE(3, "Fixing up anticipated bits for t{}, starting at {}\n",
         tmp->id(), start.id());
  assertx(state.sunk[tmp->id()]);

  state.block_state[start].begin_anticipated.set(tmp->id());

  BlockSet visited;
  jit::stack<Block*> worklist;
  start.forEachPred(
    [&] (Block* pred) {
      visited.emplace(pred);
      worklist.push(pred);
    }
  );

  do {
    auto block = worklist.top();
    worklist.pop();

    auto& block_state = state.block_state[block];
    auto& begin = block_state.begin_anticipated;
    auto& end = block_state.end_anticipated;
    if (!end[tmp->id()]) {
      ITRACE(4, "Adding t{} to anticipated end of {}\n",
             tmp->id(), block->id());
      end.set(tmp->id());
      if (!begin[tmp->id()] && !block_state.all_defs[tmp->id()]) {
        ITRACE(4, "Adding t{} to anticipated begin of {}\n",
               tmp->id(), block->id());
        begin.set(tmp->id());
        block->forEachPred(
          [&] (Block* pred) {
            if (visited.contains(pred)) return;
            visited.emplace(pred);
            worklist.push(pred);
          }
        );
      }
    }
  } while (!worklist.empty());
}

/*
 * Perform the actual insertions given by the InsertionMap, and
 * rewrite any usages as necessary.
 */
void sink_tmps(State& state, const InsertionMap& insertions) {
  ITRACE(1, "sink_tmps:vvvvvvvvvvvvvvvvvvvv\n");
  SCOPE_EXIT { ITRACE(1, "sink_tmps:^^^^^^^^^^^^^^^^^^^^\n"); };
  Trace::Indent indenter;

  // For each insertion (front or back), clone the original
  // instruction, rewrite all usages, then prepend/append the
  // instruction in the new block.
  for (auto const& insertion : insertions) {
    auto const orig_tmp = insertion.first;
    auto const orig_inst = orig_tmp->inst();
    auto const& front_inserts = insertion.second.front;
    auto const& back_inserts = insertion.second.back;

    ITRACE(2, "Sinking {}\n", *orig_inst);
    Trace::Indent indenter2;

    // Keep track of inserted phis for this insertion.
    PhiMap phis;
    for (auto& block : front_inserts) {
      auto const new_inst = state.unit.clone(orig_inst);
      ITRACE(2, "Cloned to {}\n", *new_inst);
      rewrite_uses(
        state,
        *block,
        orig_tmp,
        new_inst->dst(),
        insertion.second,
        phis,
        true
      );
      ITRACE(2, "Inserting {} in front of {}\n", *new_inst, block->id());
      block->prepend(new_inst);
      // Sinking this instruction may have extended lifetimes of any
      // of its sources. We need to potentially fixup anticipated
      // state if any of them will be sunk as well.
      for (auto const src : new_inst->srcs()) {
        if (!state.sunk[src->id()]) continue;
        state.block_state[block].begin_uses.set(src->id());
        fixup_anticipated(state, src, *block);
      }
    }
    for (auto& block : back_inserts) {
      auto const new_inst = state.unit.clone(orig_inst);
      ITRACE(2, "Cloned to {}\n", *new_inst);
      rewrite_uses(
        state,
        *block,
        orig_tmp,
        new_inst->dst(),
        insertion.second,
        phis,
        false
      );
      ITRACE(2, "Inserting {} in back of {}\n", *new_inst, block->id());
      block->append(new_inst);
      // As above, we need to fixup anticipated state if any of the
      // sources are also sunk.
      for (auto const src : new_inst->srcs()) {
        if (!state.sunk[src->id()]) continue;
        state.block_state[block].begin_uses.set(src->id());
        fixup_anticipated(state, src, *block);
      }
    }
  }
}

/*
 * Set up the initial state for the pass. This is the global SSATmp
 * sets, and the initial per-block state.
 */
State make_state(IRUnit& unit,
                 const BlockList& rpo_blocks,
                 const boost::dynamic_bitset<>& reachable,
                 Optional<IdomVector>& opt_idoms,
                 size_t ssa_tmp_start) {
  ITRACE(1, "make_state:vvvvvvvvvvvvvvvvvvvv\n");
  SCOPE_EXIT { ITRACE(1, "make_state:^^^^^^^^^^^^^^^^^^^^\n"); };
  Trace::Indent indenter;

  State state{unit, rpo_blocks, opt_idoms, ssa_tmp_start};

  auto const last_tmp =
    std::min<size_t>(unit.numTmps(), ssa_tmp_start + SSATmpSet::kNumBits);
  // Set up valid SSATmps. Only consider the fixed block we're
  // currently on.
  for (uint32_t i = ssa_tmp_start; i < last_tmp; ++i) {
    auto const tmp = unit.findSSATmp(i);
    auto const instr = tmp->inst();

    // Instructions in an unreachable block can be weird. Just ignore
    // them. They're not relevant.
    if (!instr->block() || !reachable[instr->block()->id()]) continue;

    /*
     * Instructions with multiple defs could potentialy be dealt with, but not
     * worth the effort now. Assume that if an instruction cannot be DCE'd, it
     * also cannot be sunk.
     */
    if (!canDCE(*instr)) {
      ITRACE(5, "{} not eligible (not DCE-able)\n", *instr);
      continue;
    }
    if (instr->naryDst()) {
      ITRACE(5, "{} not eligible (nary dst)\n", *instr);
      continue;
    }

    // We don't want to move anything with non-trivial memory effects.
    auto const effects = canonicalize(memory_effects(*instr));
    auto const memory_effects_okay = match<bool>(
      effects,
      [&] (const IrrelevantEffects&)   { return true; },
      [&] (const GeneralEffects& g)    {
        // GeneralEffects is okay as long as it doesn't store anything.
        return g.stores == AEmpty && g.kills == AEmpty && g.inout == AEmpty;
      },
      [&] (const PureLoad&)            { return true; },
      [&] (const ReturnEffects&)       { return false; },
      [&] (const CallEffects&)         { return false; },
      [&] (const PureStore&)           { return false; },
      [&] (const PureInlineCall&)      { return false; },
      [&] (const ExitEffects&)         { return false; },
      [&] (const UnknownEffects&)      { return false; }
    );
    if (!memory_effects_okay) {
      ITRACE(5, "{} not eligible (bad memory effects)\n", *instr);
      continue;
    }

    // This SSATmp is valid. Check if it's trivial as well.
    auto const trivial = is_trivially_sinkable(*instr);
    state.valid.set(i);
    if (trivial) state.trivial.set(i);
    ITRACE(5, "{} eligible for sinking{}\n",
           *instr, trivial ? " (trivial)" : "");

    assertx(!instr->isBlockEnd());
  }
  ITRACE(2, "valid_tmps = {}\n", show(state.valid));
  ITRACE(2, "trivially_sinkable = {}\n", show(state.trivial));

  if (state.valid.none()) return state;

  // Now set up per-block initial state.
  for (uint32_t i = 0; i < rpo_blocks.size(); ++i) {
    auto& block = *rpo_blocks[i];
    auto& block_state = state.block_state[block];
    block_state.rpo_order = i;

    // END-DELAYED always starts as all sinkable tmps. This ensures a
    // proper fixed-point solution.
    block_state.end_delayed = state.valid;

    for (auto& instr : block) {
      // Remove pass-through instructions, which simplifies everything else.
      constProp(unit, &instr);
      copyProp(&instr);

      // If this instruction will always/never conflict, adjust the
      // per-block state as necessary.
      auto const conflict = will_conflict(instr);
      if (!instr.isBlockEnd()) {
        block_state.begin_will_conflict &= conflict;
      } else {
        block_state.end_will_conflict &= conflict;
      }

      // Likewise, adjust the set of defs in this block which aren't
      // blocked (they're blocked if there's a conflict).
      if (conflict == TriBool::Yes) {
        ITRACE(4, "{} always conflicts with everything\n", instr);
        if (!instr.isBlockEnd()) block_state.unblocked_defs.reset();
      } else if (conflict == TriBool::No) {
        ITRACE(4, "{} never conflicts with anything\n", instr);
        state.no_conflict.set(instr.id());
      } else if (!instr.isBlockEnd()) {
        // Likewise, remove any conflicts. We do this on an instruction
        // by instruction basis instead once at the end because we don't
        // want to catch conflicts that occur with instructions before
        // the tmp is even defined.
        block_state.unblocked_defs = remove_conflicts(
          state, block_state.unblocked_defs, instr, TriBool::Maybe
        );
      }

      // Add any sources for this instruction (ignoring known
      // unsinkable ones). Add it to END-USE instead of BEGIN-USE if
      // this instruction is the last in the block.
      for (auto const src : instr.srcs()) {
        auto const srcId = src->id();
        if (!state.valid[srcId]) continue;
        if (!instr.isBlockEnd()) {
          block_state.begin_uses.set(srcId);
          if (!state.trivial[srcId]) block_state.unblocked_defs.reset(srcId);
        } else {
          block_state.end_uses.set(src->id());
        }
      }

      // Add any definitions for this instruction (ignoring known unsinkable
      // ones).
      for (auto const dest : instr.dsts()) {
        if (!state.valid[dest->id()]) continue;
        assertx(!instr.isBlockEnd());
        block_state.unblocked_defs.set(dest->id());
        block_state.all_defs.set(dest->id());
      }
    }

    if (block_state.begin_will_conflict != TriBool::Yes) {
      block_state.begin_unblocked =
        (~block_state.begin_uses | state.trivial) & state.valid;
    }
    if (block_state.end_will_conflict != TriBool::Yes) {
      block_state.end_unblocked =
        (~block_state.end_uses | state.trivial) & state.valid;
    }

    ITRACE(2, "Initial state of block {}:\n", block.id());
    ITRACE(2, "  unblocked_defs = {}\n", show(block_state.unblocked_defs));
    ITRACE(2, "  all_defs = {}\n", show(block_state.all_defs));
    ITRACE(2, "  begin_uses = {}\n", show(block_state.begin_uses));
    ITRACE(2, "  end_uses = {}\n", show(block_state.end_uses));
    ITRACE(2, "  begin_unblocked = {}\n", show(block_state.begin_unblocked));
    ITRACE(2, "  end_unblocked = {}\n", show(block_state.end_unblocked));
    ITRACE(2, "  begin_will_conflict = {}\n",
           show(block_state.begin_will_conflict));
    ITRACE(2, "  end_will_conflict = {}\n",
           show(block_state.end_will_conflict));
  }

  ONTRACE(2,
    std::string out;
    auto first = true;
    for (auto bit = state.no_conflict.find_first();
         bit != boost::dynamic_bitset<>::npos;
         bit = state.no_conflict.find_next(bit)) {
      fmt::format_to(std::back_inserter(out), "{}{}", first ? "" : ", ", bit);
      first = false;
    }
    ITRACE(2, "no_conflict = {}\n", out);
  );

  return state;
}

/*
 * Solve the dataflow equations to obtain the BEGIN-DELAYED and
 * END-DELAYED sets for the given set of blocks.
 */
void find_delayed(State& state) {
  ITRACE(1, "find_delayed:vvvvvvvvvvvvvvvvvvvv\n");
  SCOPE_EXIT { ITRACE(1, "find_delayed:^^^^^^^^^^^^^^^^^^^^\n"); };
  Trace::Indent indenter;

  auto const& blocks = state.rpo_blocks;
  dataflow_worklist<size_t> worklist{blocks.size()};
  dataflow_worklist<size_t, std::less<size_t>> rworklist{blocks.size()};

  // Set up worklists. Anticipated flows backwards and delayed flow
  // forward.
  for (uint32_t i = 0; i < blocks.size(); ++i) {
    worklist.push(i);
    rworklist.push(i);
  }

  do {
    auto const block = blocks[rworklist.pop()];

    auto& block_state = state.block_state[block];
    auto& begin_anticipated = block_state.begin_anticipated;
    auto& end_anticipated = block_state.end_anticipated;
    auto const& all_defs = block_state.all_defs;
    auto const& begin_uses = block_state.begin_uses;
    auto const& end_uses = block_state.end_uses;

    ITRACE(3, "Processing block {}:\n", block->id());
    ITRACE(3, "  begin_anticipated = {}\n", show(begin_anticipated));
    ITRACE(3, "  end_anticipated = {}\n", show(end_anticipated));

    end_anticipated.reset();
    block->forEachSucc(
      [&] (Block* succ) {
        auto const& succ_state = state.block_state[succ];
        end_anticipated |= succ_state.begin_anticipated;
      }
    );
    end_anticipated |= end_uses;

    auto new_begin_anticipated = (end_anticipated | begin_uses) - all_defs;

    ITRACE(3, "Processed block {}:\n", block->id());
    ITRACE(3, "  begin_anticipated = {}\n", show(new_begin_anticipated));
    ITRACE(3, "  end_anticipated = {}\n", show(end_anticipated));

    if (new_begin_anticipated != begin_anticipated) {
      block->forEachPred(
        [&] (Block* pred) {
          if (rworklist.push(state.block_state[pred].rpo_order)) {
            ITRACE(3, "Enqueing block {} for re-processing\n", pred->id());
          }
        }
      );
      begin_anticipated = std::move(new_begin_anticipated);
    }
  } while (!rworklist.empty());

  // Find the fixed-point solution of BEGIN-DELAYED and END-DELAYED
  // for each block using a typical worklist approach.
  do {
    auto const block = blocks[worklist.pop()];

    auto& block_state = state.block_state[block];
    auto& begin_delayed = block_state.begin_delayed;
    auto& end_delayed = block_state.end_delayed;

    auto const& begin_unblocked = block_state.begin_unblocked;
    auto const& unblocked_defs = block_state.unblocked_defs;

    ITRACE(3, "Processing block {}:\n", block->id());
    ITRACE(3, "  begin_delayed = {}\n", show(begin_delayed));
    ITRACE(3, "  end_delayed = {}\n", show(end_delayed));

    /*
     * remove_conflicts is used rather than maintaining an explicit set because
     * its faster. Only a few tmps are present in the set at each call to
     * remove_conflicts, so we only have to do a few conflict checks each
     * time. If we tried to pre-compute all the conflict information, we'd have
     * to do the conflict check #tmps * #instructions.
     */

    if (block->isEntry()) {
      begin_delayed.reset();
    } else {
      begin_delayed.set();
      block->forEachPred(
        [&] (Block* pred) {
          auto const& pred_state = state.block_state[pred];
          begin_delayed &= remove_conflicts(
            state,
            pred_state.end_delayed & pred_state.end_unblocked,
            pred->back(),
            pred_state.end_will_conflict
          );
        }
      );
    }

    auto new_end_delayed =
      remove_conflicts(state, begin_delayed & begin_unblocked, *block) |
      unblocked_defs;

    ITRACE(3, "Processed block {}:\n", block->id());
    ITRACE(3, "  begin_delayed = {}\n", show(begin_delayed));
    ITRACE(3, "  end_delayed = {}\n", show(new_end_delayed));

    // Information from block to block is only ever transmitted via a
    // block's END-DELAYED set, so if that has changed from the
    // previous value, re-queue all of the block's successors to be
    // re-processed.
    if (new_end_delayed != end_delayed) {
      block->forEachSucc(
        [&] (Block* succ) {
          if (worklist.push(state.block_state[succ].rpo_order)) {
            ITRACE(3, "Enqueing block {} for re-processing\n", succ->id());
          }
        }
      );
      end_delayed = std::move(new_end_delayed);
    }
  } while (!worklist.empty());
}

/*
 * Given the solved BEGIN-DELAYED and END-DELAYED sets, solve for
 * BEGIN-INSERT and END-INSERT and tabulate the results into an
 * InsertionMap.
 */
InsertionMap find_insertions(State& state)  {
  ITRACE(1, "find_insertions:vvvvvvvvvvvvvvvvvvvv\n");
  SCOPE_EXIT { ITRACE(1, "find_insertions:^^^^^^^^^^^^^^^^^^^^\n"); };
  Trace::Indent indenter;

  InsertionMap insertions;

  auto const& trivial = state.trivial;

  for (auto const block : state.rpo_blocks) {
    auto const& block_state = state.block_state[block];
    auto const& begin_delayed = block_state.begin_delayed;
    auto const& begin_unblocked = block_state.begin_unblocked;
    auto const& begin_uses = block_state.begin_uses;
    auto const& end_uses = block_state.end_uses;
    auto const& end_delayed = block_state.end_delayed;
    auto const& begin_anticipated = block_state.begin_anticipated;
    auto const& end_anticipated = block_state.end_anticipated;
    auto const& all_defs = block_state.all_defs;

    auto begin_insert = begin_delayed & begin_anticipated;
    begin_insert &=
      remove_non_conflicts(state, begin_insert, *block) | ~begin_unblocked;
    begin_insert |= (begin_uses & trivial);

    SSATmpSet end_insert{state.ssa_tmp_start};
    if (block->isExit()) {
      end_insert.set();
    } else {
      end_insert |= end_uses & trivial;
      block->forEachSucc(
        [&] (Block* succ) {
          end_insert |= ~state.block_state[succ].begin_delayed;
        }
      );
    }
    end_insert -= begin_insert;
    end_insert &= end_delayed & end_anticipated;

    begin_insert -= all_defs;
    end_insert -= all_defs;

    ITRACE(2, "Block {}:\n", block->id());
    ITRACE(2, "  begin_insert = {}\n", show(begin_insert));
    ITRACE(2, "  end_insert = {}\n", show(end_insert));

    begin_insert.forEach(
      [&] (size_t bit) {
        auto const s = state.unit.findSSATmp(bit);
        insertions[s].front.insert(block);
        state.sunk.set(s->id());
      }
    );
    end_insert.forEach(
      [&] (size_t bit) {
        auto const s = state.unit.findSSATmp(bit);
        insertions[s].back.insert(block);
        state.sunk.set(s->id());
      }
    );
  }
  ITRACE(2, "sunk = {}\n", show(state.sunk));

  ONTRACE(1,
    for (auto const& p : insertions) {
      for (auto const block : p.second.front) {
        ITRACE(1, "Sink {} to front of block {}\n",
               *p.first->inst(), block->id());
      }
      for (auto const block : p.second.back) {
        ITRACE(1, "Sink {} to end of block {}\n",
               *p.first->inst(), block->id());
      }
    }
  );

  return insertions;
}

/*
 * Main pass loop. Given a list of blocks, find all definitions which are valid
 * candidates for sinking, sink them, and then restore valid SSA form. Return
 * true if any definitions were sunk, false otherwise.
 */
bool sink_pass_for_bits(IRUnit& unit,
                        const BlockList& rpo_blocks,
                        const boost::dynamic_bitset<>& reachable,
                        Optional<IdomVector>& opt_idoms,
                        size_t pass_count,
                        size_t ssa_tmp_start) {
  ITRACE(
    1,
    "sink_pass #{} ({}-{}):vvvvvvvvvvvvvvvvvvvv\n",
    pass_count+1,
    ssa_tmp_start,
    ssa_tmp_start + SSATmpSet::kNumBits
  );
  SCOPE_EXIT {
    ITRACE(
      1,
      "sink_pass #{} ({}-{}):^^^^^^^^^^^^^^^^^^^^\n",
      pass_count+1,
      ssa_tmp_start,
      ssa_tmp_start + SSATmpSet::kNumBits
    );
  };
  Trace::Indent indenter;

  // Calculate initial state
  auto state =
    make_state(unit, rpo_blocks, reachable, opt_idoms, ssa_tmp_start);
  if (state.valid.none()) return false;

  // Calculate the delayed sets
  find_delayed(state);
  // The fixed-point of BEGIN-DELAYED and BEGIN-DELAYED has been
  // found, so use it to compute BEGIN-INSERT and BEGIN-INSERT which
  // will tell us where to sink the definitions to. This information
  // is pulled out of BEGIN-INSERT and BEGIN-INSERT into the easier to
  // process InsertionMap.
  auto const insertions = find_insertions(state);

  // Nothing to sink so done.
  if (insertions.empty()) return false;

  // Otherwise we're definitely going to insert something, so perform
  // the insertions.
  sink_tmps(state, insertions);
  return true;
}

// Run one instance of the pass. Depending on the number of SSATmps,
// we might need to perform the sinking more than once.
bool sink_pass(IRUnit& unit,
               const BlockList& rpo_blocks,
               const boost::dynamic_bitset<>& reachable,
               Optional<IdomVector>& opt_idoms,
               size_t pass_count) {
  // Do the actual pass, for each block of SSATmps. In the common
  // case, this will only be once.
  auto changed = false;
  for (size_t start_bits = 0;
       start_bits < unit.numTmps();
       start_bits += SSATmpSet::kNumBits) {
    changed |= sink_pass_for_bits(
      unit,
      rpo_blocks,
      reachable,
      opt_idoms,
      pass_count,
      start_bits
    );
  }
  return changed;
}

}

// Pass entry point.
void sinkDefs(IRUnit& unit) {
  PassTracer tracer{&unit, Trace::hhir_sinkdefs, "sinkDefs"};
  Timer timer{Timer::optimize_sinkDefs, unit.logEntry().get_pointer()};

  // Algorithm relies on the lack of critical edges, so remove them.
  splitCriticalEdges(unit);

  // This pass doesn't modify the CFG, so we can calculate the RPO
  // order and reachable blocks once.
  auto const rpo_blocks = rpoSortCfg(unit);
  if (rpo_blocks.empty()) return;

  boost::dynamic_bitset<> reachable{unit.numBlocks()};
  for (auto const b : rpo_blocks) reachable.set(b->id());

  // Defer the dominator calculation until we actually need it. This
  // only needs to be done once.
  Optional<IdomVector> opt_idoms;

  // Keep sinking definitions until nothing will sink any farther.
  size_t pass_count = 0;
  while (sink_pass(unit, rpo_blocks, reachable, opt_idoms, pass_count)) {
    ++pass_count;
  }
}

//////////////////////////////////////////////////////////////////////

}
