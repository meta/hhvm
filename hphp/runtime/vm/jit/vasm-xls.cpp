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

#include "hphp/runtime/vm/jit/vasm.h"

#include "hphp/runtime/vm/jit/abi.h"
#include "hphp/runtime/vm/jit/punt.h"
#include "hphp/runtime/vm/jit/reg-algorithms.h"
#include "hphp/runtime/vm/jit/timer.h"
#include "hphp/runtime/vm/jit/vasm-instr.h"
#include "hphp/runtime/vm/jit/vasm-print.h"
#include "hphp/runtime/vm/jit/vasm-reg.h"
#include "hphp/runtime/vm/jit/vasm-unit.h"
#include "hphp/runtime/vm/jit/vasm-util.h"
#include "hphp/runtime/vm/jit/vasm-visit.h"

#include "hphp/util/arch.h"
#include "hphp/util/assertions.h"
#include "hphp/util/configs/hhir.h"
#include "hphp/util/dataflow-worklist.h"
#include "hphp/util/trace.h"

#include <boost/range/adaptor/reversed.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <random>

// future work
//  - #3098509 streamline code, vectors vs linked lists, etc
//  - #3098685 Optimize lifetime splitting
//  - #3098739 new features now possible with XLS

TRACE_SET_MOD(xls)

namespace HPHP::jit {
///////////////////////////////////////////////////////////////////////////////

namespace {
///////////////////////////////////////////////////////////////////////////////

std::atomic<size_t> s_counter;

DEBUG_ONLY constexpr auto kHintLevel = 5;

/*
 * Vreg discriminator.
 */
enum class Constraint { Any, CopySrc, Gpr, Simd, Sf };

Constraint constraint(const Vreg&) { return Constraint::Any; }
Constraint constraint(const Vreg64&) { return Constraint::Gpr; }
Constraint constraint(const Vreg32&) { return Constraint::Gpr; }
Constraint constraint(const Vreg16&) { return Constraint::Gpr; }
Constraint constraint(const Vreg8&) { return Constraint::Gpr; }
Constraint constraint(const VregDbl&) { return Constraint::Simd; }
Constraint constraint(const Vreg128&) { return Constraint::Simd; }
Constraint constraint(const VregSF&) { return Constraint::Sf; }

bool is_wide(const Vreg128&) { return true; }
template<class T> bool is_wide(const T&) { return false; }

/*
 * Sentinel constants.
 */
constexpr int kInvalidPhiGroup = -1;
constexpr int kInvalidSpillSlot = -1;
const unsigned kMaxPos = UINT_MAX; // "infinity" use position

/*
 * A Use refers to the position where an interval is used or defined.
 */
struct Use {
  Use() {}
  Use(Constraint kind, unsigned pos, Vreg hint)
    : kind(kind)
    , pos(pos)
    , hint(hint)
  {}

  /*
   * Constraint imposed on the Vreg at this use.
   */
  Constraint kind{Constraint::Any};
  /*
   * Position of this use or def.
   */
  unsigned pos{kMaxPos};
  /*
   * If valid, try to assign the same physical register here as `hint' was
   * assigned at `pos'.
   */
  Vreg hint;

  /*
   * Index of phi group metadata.
   */
  int phi_group{kInvalidPhiGroup};
};

/*
 * A LiveRange is an closed-open range of positions where an interval is live.
 *
 * Specifically, for the LiveRange [start, end), start is in the range and
 * end is not.
 */
struct LiveRange {
  bool contains(unsigned pos) const { return pos >= start && pos < end; }
  bool intersects(LiveRange r) const { return r.start < end && start < r.end; }
  bool contains(LiveRange r) const { return r.start >= start && r.end <= end; }
public:
  unsigned start, end;
};

struct Variable;

/*
 * An Interval represents the lifetime of a Vreg as a sorted list of disjoint
 * ranges and a sorted list of use positions.  It is the unit of register
 * allocation.
 *
 * Intervals may be split---e.g., because the Vreg needed to be spilled in some
 * subrange.  All (sub-)Intervals for a given Vreg are connected as a singly
 * linked list, sorted by start position.
 *
 * Every use position must be inside one of the ranges, or exactly at the end
 * of the last range.  Allowing a use exactly at the end facilitates lifetime
 * splitting when the use at the position of an instruction clobbers registers
 * as a side effect, e.g. a call.
 *
 * The intuition for allowing uses at the end of an Interval is that, in truth,
 * the picture at a given position looks like this:
 *
 *          | [s]
 *          |
 *    +-----|-------------+ copy{s, d}  <-+
 *    |     v             |               |
 *    + - - - - - - - - - +               +--- position n
 *    |             |     |               |
 *    +-------------|-----+             <-+
 *                  |
 *              [d] v
 *
 * We represent an instruction with a single position `n'.  All the use(s) and
 * def(s) of that instruction are live at some point within it, but their
 * lifetimes nonetheless do not overlap.  Since we don't represent instructions
 * using two position numbers, instead, we allow uses on the open end side of
 * Intervals, because they don't actually conflict with, e.g., a def of another
 * Interval that starts at the same position.
 */
struct Interval {
  explicit Interval(Variable* var) : var(var) {}

  std::string toString() const;

  /*
   * Endpoints.  Intervals are [closed, open), just like LiveRanges.
   */
  unsigned start() const { return ranges.front().start; }
  unsigned end() const { return ranges.back().end; }

  /*
   * Head of this Interval chain.
   */
  const Interval* leader() const;
        Interval* leader();

  /*
   * Register allocation state.
   */
  bool live() const;
  bool constant() const;
  bool spilled() const;
  bool fixed() const;

  /*
   * Split this interval at `pos', returning the new `this->next'.
   *
   * If `keep_uses' is set, uses exactly at the end of the first interval will
   * stay with the first split (rather than the second).
   *
   * @requires: pos > start() && pos < end(); this ensures that both
   *            subintervals are nonempty.
   */
  Interval* split(unsigned pos, bool keep_uses = false);

  /////////////////////////////////////////////////////////////////////////////
  // Queries.
  //
  // These operate only on `this' and not its children.

  /*
   * Get the index of the first range or use that is not strictly lower than
   * `pos' (i.e., which contains/is at `pos' or is strictly higher than `pos').
   */
  unsigned findRange(unsigned pos) const;
  unsigned findUse(unsigned pos) const;

  /*
   * Whether there is a range that includes `pos', or a use at `pos'.
   */
  bool covers(unsigned pos) const;
  bool usedAt(unsigned pos) const;

  /*
   * The position of a use [relative to `pos'] that requires a register (i.e.,
   * CopySrc uses are ignored).
   *
   * firstUseAfter: The first use >= `pos', kMaxPos if there are no more uses.
   * lastUseBefore: The first use <= `pos'; 0 if the first use is after `pos'.
   * firstUse:      The first use in `this'.
   */
  unsigned firstUseAfter(unsigned pos) const;
  unsigned lastUseBefore(unsigned pos) const;
  unsigned firstUse() const;

public:
  // The Variable whose (sub-)lifetime this (sub-)interval represents.
  Variable* const var;

  // Pointer to the next subinterval.
  Interval* next{nullptr};

  // Live ranges and use positions.
  jit::vector<LiveRange> ranges;
  jit::vector<Use> uses;

  // The physical register assigned to this subinterval.
  PhysReg reg;
};

/*
 * Metadata about a variable which is the object of register allocation.
 *
 * Each Variable represents a Vreg in the Vunit we're performing register
 * allocation on---including lifetime ranges, uses, def information, etc.
 */
struct Variable {
  explicit Variable(Vreg r) : vreg(r) {}

  static Variable* create(Vreg r);
  static void destroy(Variable* var);

  /*
   * Accessors.
   */
  Interval* ivl() const { return (Interval*)(this + 1); }
  bool fixed() const { return vreg.isPhys(); }

  /*
   * Return the subinterval which covers or has a use at `pos', else nullptr if
   * no subinterval does.
   */
  Interval* ivlAt(unsigned pos);
  Interval* ivlAtUse(unsigned pos);

public:
  // The Vreg this value represents.
  const Vreg vreg;

  // Whether this is a SIMD value.
  bool wide{false};

  // Whether this variable is a constant, and its constant value.
  bool constant{false};
  Vconst val;

  // The spill slot assigned to this value.  Since we don't spill physical
  // registers, and Vregs are SSA, a value's assigned slot never changes.
  int slot{kInvalidSpillSlot};

  // Position of, and block containing, the variable's single def instruction;
  // invalid for physical registers.
  unsigned def_pos{kMaxPos};
  Vlabel def_block;

  // List of recordbasenativesps this vreg is live across.
  jit::vector<unsigned> recordbasenativesps{};

  // Copy of the Vreg's def instruction.
  Vinstr def_inst;
};

using LiveSet = boost::dynamic_bitset<>; // Bitset of Vreg numbers.

template<class Fn> void forEach(const LiveSet& bits, Fn fn) {
  for (auto i = bits.find_first(); i != bits.npos; i = bits.find_next(i)) {
    fn(Vreg(i));
  }
}

/*
 * Sack of inputs and pre-computed data used by the main XLS algorithm.
 */
struct VxlsContext {
  explicit VxlsContext(const Abi& abi)
    : abi(abi)
    , sp(rsp())
  {
    tmp = ARCH_MATCH(
      [](arch::X64) { return reg::xmm15; },
      [](arch::ARM) { return vixl::d31; }
    );
    this->abi.simdUnreserved -= tmp;
    this->abi.simdReserved |= tmp;
    assertx(!abi.gpUnreserved.contains(sp));
    assertx(!abi.gpUnreserved.contains(tmp));
  }

public:
  Abi abi;
  // Arch-dependent stack pointer.
  PhysReg sp;
  // Temp register used only for breaking cycles.
  PhysReg tmp;
  // Debug-only run identifier.
  size_t counter;

  // Sorted blocks.
  jit::vector<Vlabel> blocks;
  // [start,end) position of each block.
  jit::vector<LiveRange> block_ranges;
  // Per-block sp[offset] to spill-slots.  std::nullopt in cases where we
  // have still not reached the recordbasenativesp instruction.
  jit::vector<Optional<int>> spill_offsets;
  // Per-block live-in sets.
  jit::vector<LiveSet> livein;
};

///////////////////////////////////////////////////////////////////////////////
// Interval.

const Interval* Interval::leader() const { return var->ivl(); }
      Interval* Interval::leader()       { return var->ivl(); }

bool Interval::live() const { return reg != InvalidReg; }
bool Interval::constant() const { return var->constant; }
bool Interval::spilled() const { return !live() && var->slot >= 0; }
bool Interval::fixed() const { return var->fixed(); }

unsigned Interval::findRange(unsigned pos) const {
  unsigned lo = 0;
  for (unsigned hi = ranges.size(); lo < hi;) {
    auto mid = (lo + hi) / 2;
    auto r = ranges[mid];
    if (pos < r.start) {
      hi = mid;
    } else if (r.end <= pos) {
      lo = mid + 1;
    } else {
      return mid;
    }
  }
  assertx(lo == ranges.size() || pos < ranges[lo].start);
  return lo;
}

unsigned Interval::findUse(unsigned pos) const {
  unsigned lo = 0, hi = uses.size();
  while (lo < hi) {
    auto mid = (lo + hi) / 2;
    auto u = uses[mid].pos;
    if (pos < u) {
      hi = mid;
    } else if (u < pos) {
      lo = mid + 1;
    } else {
      return mid;
    }
  }
  assertx(lo == uses.size() || pos < uses[lo].pos);
  return lo;
}

bool Interval::covers(unsigned pos) const {
  if (pos < start() || pos >= end()) return false;
  auto i = ranges.begin() + findRange(pos);
  return i != ranges.end() && i->contains(pos);
}

bool Interval::usedAt(unsigned pos) const {
  if (pos < start() || pos > end()) return false;
  auto i = uses.begin() + findUse(pos);
  return i != uses.end() && pos == i->pos;
}

unsigned Interval::firstUseAfter(unsigned pos) const {
  for (auto& u : uses) {
    if (u.kind == Constraint::CopySrc) continue;
    if (u.pos >= pos) return u.pos;
  }
  return kMaxPos;
}

unsigned Interval::lastUseBefore(unsigned pos) const {
  auto prev = 0;
  for (auto& u : uses) {
    if (u.kind == Constraint::CopySrc) continue;
    if (u.pos > pos) return prev;
    prev = u.pos;
  }
  return prev;
}

unsigned Interval::firstUse() const {
  for (auto& u : uses) {
    if (u.kind != Constraint::CopySrc) return u.pos;
  }
  return kMaxPos;
}

Interval* Interval::split(unsigned pos, bool keep_uses) {
  assertx(pos > start() && pos < end()); // both parts will be non-empty

  auto child = jit::make<Interval>(var);
  child->next = next;
  next = child;

  // advance r1 to the first range we want in child; maybe split a range.
  auto r1 = ranges.begin() + findRange(pos);
  if (pos > r1->start) { // split r at pos
    child->ranges.push_back({pos, r1->end});
    r1->end = pos;
    r1++;
  }
  child->ranges.insert(child->ranges.end(), r1, ranges.end());
  ranges.erase(r1, ranges.end());

  // advance u1 to the first use position in child, then copy u1..end to child.
  auto u1 = uses.begin() + findUse(end());
  auto u2 = uses.end();
  if (keep_uses) {
    while (u1 != u2 && u1->pos <= end()) u1++;
  } else {
    while (u1 != u2 && u1->pos < child->start()) u1++;
  }
  child->uses.insert(child->uses.end(), u1, u2);
  uses.erase(u1, u2);

  return child;
}

///////////////////////////////////////////////////////////////////////////////
// Variable.

Variable* Variable::create(Vreg r) {
  auto const mem = malloc(sizeof(Variable) + sizeof(Interval));
  auto const var = new (mem) Variable(r);
  new (reinterpret_cast<void*>(var + 1)) Interval(var);
  return var;
}

void Variable::destroy(Variable* var) {
  var->~Variable();
  auto ivl = reinterpret_cast<Interval*>(var + 1);
  ivl->~Interval();
  free(var);
}

Interval* Variable::ivlAt(unsigned pos) {
  for (auto ivl = this->ivl(); ivl; ivl = ivl->next) {
    if (pos < ivl->start()) return nullptr;
    if (ivl->covers(pos)) return ivl;
  }
  return nullptr;
}

Interval* Variable::ivlAtUse(unsigned pos) {
  for (auto ivl = this->ivl(); ivl; ivl = ivl->next) {
    if (pos < ivl->start()) return nullptr;
    if (ivl->usedAt(pos)) return ivl;
  }
  return nullptr;
}

///////////////////////////////////////////////////////////////////////////////

/*
 * Extended Linear Scan is based on Wimmer & Franz "Linear Scan Register
 * Allocation on SSA Form". As currently written, it also works on non-ssa
 * input.
 *
 * 1. Sort blocks such that all predecessors of B come before B, except
 * loop-edge predecessors. If the input IR is in SSA form, this also
 * implies the definition of each SSATmp comes before all uses.
 *
 * 2. Assign an even numbered position to every instruction. Positions
 * between instructions are used to insert copies and spills. Each block
 * starts with an extra position number that corresponds to an imaginary
 * "label" instruction that is not physically in the vasm IR.
 *
 * 3. Create one interval I for each Vreg R that requires register allocation,
 * by iterating blocks and instructions in reverse order, computing live
 * registers as we go. Each interval consists of a sorted list of disjoint,
 * live ranges covering the positions where R must be in a physical register
 * or spill slot. Vregs that are constants or have forced registers
 * (e.g. VmSp) are skipped. If the input is SSA, the start position of each
 * interval dominates every live range and use position in the interval.
 *
 * 4. Process intervals in order of start position, maintaining the set of
 * active (live) and inactive (not live, but with live ranges that start
 * after the current interval). When choosing a register, prefer the one
 * available furthest into the future. If necessary, split the current
 * interval so the first part gets a register, and enqueue the rest.
 * When no registers are available, choose either the current interval or
 * another one to spill, trying to free up the longest-available register.
 *
 * Split positions must be after an interval's start position, and on or before
 * the chosen split point. We're free try to choose a good position inbetween,
 * for example block boundaries and cold blocks.
 *
 * 5. Once intervals have been walked and split, every interval has an assigned
 * operand (register or spill location) for all positions where its alive.
 * Visit every instruction and modify its Vreg operands to the physical
 * register that was assigned.
 *
 * 6. Splitting creates sub-intervals that are assigned to different registers
 * or spill locations, so insert resolving copies at the split positions
 * between intervals that were split in a block, and copies on control-flow
 * edges connecting different sub-intervals. When more than one copy occurs
 * in a position, they are parallel-copies (all sources read before any dest
 * is written).
 *
 * If any sub-interval was spilled, a single store is generated after each
 * definition point.
 *
 * When analyzing instructions that use or define a virtual SF register
 * (VregSF), eagerly rename it to the singleton PhysReg RegSF{0}, under the
 * assumption that there can only be one live SF at each position. This
 * reduces the number of intervals we need to process, facilitates inserting
 * ldimm{0} (as xor), and is checked by checkSF().
 */

///////////////////////////////////////////////////////////////////////////////

/*
 * Printing utilities.
 */
void printVariables(const char* caption,
                    const Vunit& unit, const VxlsContext& ctx,
                    const jit::vector<Variable*>& variables);
void dumpVariables(const jit::vector<Variable*>& variables,
                   unsigned num_spills);

/*
 * The ID of the block enclosing `pos'.
 */
Vlabel blockFor(const VxlsContext& ctx, unsigned pos) {
  for (unsigned lo = 0, hi = ctx.blocks.size(); lo < hi;) {
    auto mid = (lo + hi) / 2;
    auto r = ctx.block_ranges[ctx.blocks[mid]];
    if (pos < r.start) {
      hi = mid;
    } else if (pos >= r.end) {
      lo = mid + 1;
    } else {
      return ctx.blocks[mid];
    }
  }
  always_assert(false);
  return Vlabel{0xffffffff};
}

/*
 * Insert `src' into `dst' before dst[j], corresponding to XLS logical
 * position `pos'.
 *
 * Updates `j' to refer to the same instruction after the code insertions.
 */
void insertCodeAt(jit::vector<Vinstr>& dst, unsigned& j,
                  const jit::vector<Vinstr>& src, unsigned pos) {
  auto const irctx = dst[j].irctx();
  dst.insert(dst.begin() + j, src.size(), trap{TRAP_REASON, Fixup::none()});
  for (auto const& inst : src) {
    dst[j] = inst;
    dst[j].set_irctx(irctx);
    dst[j++].id = pos;
  }
}

///////////////////////////////////////////////////////////////////////////////
// Pre-analysis passes.

/*
 * Compute the linear position range of each block.
 *
 * This modifies the Vinstrs in `unit' by setting their `pos' members, in
 * addition to producing the block-to-range map.
 */
jit::vector<LiveRange> computePositions(Vunit& unit,
                                        const jit::vector<Vlabel>& blocks) {
  auto block_ranges = jit::vector<LiveRange>{unit.blocks.size()};
  unsigned pos = 0;

  for (auto const b : blocks) {
    auto& code = unit.blocks[b].code;

    bool front_uses{false};
    visitUses(unit, code.front(), [&](Vreg /*r*/) { front_uses = true; });
    if (front_uses) {
      auto irctx = code.front().irctx();
      code.emplace(code.begin(), nop{}, irctx);
    }
    auto const start = pos;

    for (auto& inst : unit.blocks[b].code) {
      inst.id = pos;
      pos += 2;
    }
    block_ranges[b] = { start, pos };
  }
  return block_ranges;
}

/*
 * Return the effect this instruction has on the value of `sp'.
 *
 * Asserts (if `do_assert' is set) if an instruction mutates `sp' in an
 * untrackable way.
 */
int spEffect(const Vunit& unit, const Vinstr& inst, PhysReg sp,
             bool do_assert = debug) {
  switch (inst.op) {
    case Vinstr::push:
    case Vinstr::pushf:
    case Vinstr::pushm:
      return -8;
    case Vinstr::pushp:
    case Vinstr::pushpm:
      return -16;
    case Vinstr::pop:
    case Vinstr::popf:
    case Vinstr::popm:
      return 8;
    case Vinstr::popp:
    case Vinstr::poppm:
      return 16;
    case Vinstr::lea: {
      auto& i = inst.lea_;
      if (i.d == Vreg64(sp)) {
        assertx(i.s.base == i.d && !i.s.index.isValid());
        return i.s.disp;
      }
      return 0;
    }
    default:
      if (do_assert) {
        visitDefs(unit, inst, [&] (Vreg r) { assertx(r != sp); });
      }
      return 0;
  }
}

/*
 * Compute the offset from `sp' to the spill area at each block start.
 */
jit::vector<Optional<int>> analyzeSP(const Vunit& unit,
                                            const jit::vector<Vlabel>& blocks,
                                            PhysReg sp) {
  auto visited = boost::dynamic_bitset<>(unit.blocks.size());
  auto spill_offsets = jit::vector<Optional<int>>(unit.blocks.size());

  for (auto const b : blocks) {
    auto offset = visited.test(b) ? spill_offsets[b] : std::nullopt;

    for (unsigned j = 0; j < unit.blocks[b].code.size(); j++) {
      auto const& inst = unit.blocks[b].code[j];
      if (inst.op == Vinstr::recordbasenativesp) {
        assert_flog(!offset, "Block B{} Instr {} initiailizes native SP, but "
                    "already initialized.", size_t(b), j);
        offset = 0;
      } else if (inst.op == Vinstr::unrecordbasenativesp) {
        assert_flog(offset, "Block B{} Instr {} uninitiailizes native SP, but "
                    "already uninitialized.", size_t(b), j);
        assert_flog(*offset == 0, "Block B{} Instr {} uninitiailizes native SP, "
                    "but SPOffset is nonzero.", size_t(b), j);
        offset = std::nullopt;
      } else if (offset) {
        *offset -= spEffect(unit, inst, sp);
      }
    }
    for (auto const s : succs(unit.blocks[b])) {
      if (visited.test(s)) {
        assert_flog(offset == spill_offsets[s],
                    "sp mismatch on edge B{}->B{}, expected {} got {}",
                    size_t(b), size_t(s),
                    spill_offsets[s] ? std::to_string(*spill_offsets[s])
                                     : "none",
                    offset           ? std::to_string(*offset)
                                     : "none");
      } else {
        spill_offsets[s] = offset;
        visited.set(s);
      }
    }
  }
  return spill_offsets;
}

jit::vector<LiveSet> computeLiveness(const Vunit& unit,
                                     const Abi& abi,
                                     const jit::vector<Vlabel>& blocks) {
  auto livein = jit::vector<LiveSet>{unit.blocks.size()};
  auto const preds = computePreds(unit);

  auto blockPO = jit::vector<uint32_t>(unit.blocks.size());
  auto revBlocks = blocks;
  std::reverse(begin(revBlocks), end(revBlocks));

  auto wl = dataflow_worklist<uint32_t>(revBlocks.size());

  for (unsigned po = 0; po < revBlocks.size(); po++) {
    wl.push(po);
    blockPO[revBlocks[po]] = po;
  }

  while (!wl.empty()) {
    auto b = revBlocks[wl.pop()];
    auto& block = unit.blocks[b];

    // start with the union of the successor blocks
    LiveSet live(unit.next_vr);
    for (auto s : succs(block)) {
      if (!livein[s].empty()) live |= livein[s];
    }

    // and now go through the instructions in the block in reverse order
    for (auto const& inst : boost::adaptors::reverse(block.code)) {
      RegSet implicit_uses, implicit_across, implicit_defs;
      getEffects(abi, inst, implicit_uses, implicit_across, implicit_defs);

      auto const vsf = Vreg{RegSF{0}};

      auto const dvisit = [&] (Vreg r, Width w) {
        live.reset(w == Width::Flags ? vsf : r);
      };
      auto const uvisit = [&] (Vreg r, Width w) {
        live.set(w == Width::Flags ? vsf : r);
      };

      visitDefs(unit, inst, dvisit);
      visit(unit, implicit_defs, dvisit);

      visitUses(unit, inst, uvisit);
      visit(unit, implicit_uses, uvisit);
      visit(unit, implicit_across, uvisit);
    }

    if (live != livein[b]) {
      livein[b] = live;
      for (auto p : preds[b]) {
        wl.push(blockPO[p]);
      }
    }
  }

  return livein;
}

///////////////////////////////////////////////////////////////////////////////
// Lifetime intervals.

/*
 * Add `r' to `ivl'.
 *
 * This assumes that the ranges of `ivl' are in reverse order, and that `r'
 * precedes or overlaps with ivl->ranges.first().
 */
void addRange(Interval* ivl, LiveRange r) {
  while (!ivl->ranges.empty() && r.contains(ivl->ranges.back())) {
    ivl->ranges.pop_back();
  }
  if (ivl->ranges.empty()) {
    return ivl->ranges.push_back(r);
  }
  auto& first = ivl->ranges.back();
  if (first.contains(r)) return;
  if (r.end >= first.start) {
    first.start = r.start;
  } else {
    ivl->ranges.push_back(r);
  }
}

/*
 * Visits defs of an instruction, updates their liveness, adds live ranges, and
 * adds Uses with appropriate hints.
 */
struct DefVisitor {
  DefVisitor(const Vunit& unit, jit::vector<Variable*>& variables,
             LiveSet& live, Vlabel b, const Vinstr& inst, unsigned pos)
    : m_unit(unit)
    , m_variables(variables)
    , m_live(live)
    , m_block(b)
    , m_inst(inst)
    , m_pos(pos)
  {}

  // Skip immediates and uses.
  template<class F> void imm(const F&) {}
  template<class R> void use(R) {}
  template<class S, class H> void useHint(S, H) {}
  template<class R> void across(R) {}

  void def(Vtuple defs) {
    for (auto r : m_unit.tuples[defs]) def(r);
  }
  void defHint(Vtuple def_tuple, Vtuple hint_tuple) {
    auto& defs = m_unit.tuples[def_tuple];
    auto& hints = m_unit.tuples[hint_tuple];
    for (int i = 0; i < defs.size(); i++) {
      def(defs[i], Constraint::Any, hints[i]);
    }
  }
  template<class R> void def(R r) {
    def(r, constraint(r), Vreg{}, is_wide(r));
  }
  template<class D, class H> void defHint(D dst, H hint) {
    def(dst, constraint(dst), hint, is_wide(dst));
  }
  void def(Vreg r) { def(r, Constraint::Any); }
  void defHint(Vreg d, Vreg hint) { def(d, Constraint::Any, hint); }
  void def(RegSet rs) { rs.forEach([&](Vreg r) { def(r); }); }
  void def(VregSF r) {
    r = RegSF{0}; // eagerly rename all SFs
    def(r, constraint(r));
  }

private:
  void def(Vreg r, Constraint kind, Vreg hint = Vreg{}, bool wide = false) {
    auto var = m_variables[r];
    if (m_live.test(r)) {
      m_live.reset(r);
      var->ivl()->ranges.back().start = m_pos;
    } else {
      if (!var) {
        var = m_variables[r] = Variable::create(r);
      }
      addRange(var->ivl(), {m_pos, m_pos + 1});
    }
    if (!var->fixed()) {
      var->ivl()->uses.push_back(Use{kind, m_pos, hint});
      var->wide |= wide;
      var->def_pos = m_pos;
      var->def_block = m_block;
      var->def_inst = m_inst;
    }
  }

private:
  const Vunit& m_unit;
  jit::vector<Variable*>& m_variables;
  LiveSet& m_live;
  Vlabel m_block;
  const Vinstr& m_inst;
  unsigned m_pos;
};

struct UseVisitor {
  UseVisitor(const Vunit& unit, jit::vector<Variable*>& variables,
             LiveSet& live, const Vinstr& inst, LiveRange range)
    : m_unit(unit)
    , m_variables(variables)
    , m_live(live)
    , m_inst(inst)
    , m_range(range)
  {}

  // Skip immediates and defs.
  template<class F> void imm(const F&) {}
  template<class R> void def(R) {}
  template<class D, class H> void defHint(D, H) {}

  template<class R> void use(R r) { use(r, constraint(r), m_range.end); }
  template<class S, class H> void useHint(S src, H hint) {
    use(src, constraint(src), m_range.end, hint);
  }
  void use(VregSF r) {
    r = RegSF{0}; // eagerly rename all SFs
    use(r, constraint(r), m_range.end);
  }
  void use(RegSet regs) { regs.forEach([&](Vreg r) { use(r); }); }
  void use(Vtuple uses) { for (auto r : m_unit.tuples[uses]) use(r); }
  void useHint(Vtuple src_tuple, Vtuple hint_tuple) {
    auto& uses = m_unit.tuples[src_tuple];
    auto& hints = m_unit.tuples[hint_tuple];
    for (int i = 0, n = uses.size(); i < n; i++) {
      useHint(uses[i], hints[i]);
    }
  }
  void use(Vptr m) {
    if (m.base.isValid()) use(m.base);
    if (m.index.isValid()) use(m.index);
  }
  template<Width w> void use(Vp<w> m) { use(static_cast<Vptr>(m)); }

  void use(VcallArgsId /*id*/) {
    always_assert(false && "vcall unsupported in vxls");
  }

  /*
   * An operand marked as UA means use-across.  Mark it live across the
   * instruction so its lifetime conflicts with the destination, which ensures
   * it will be assigned a different register than the destination.  This isn't
   * necessary if *both* operands of a binary instruction are the same virtual
   * register, but is still correct.
   */
  template<class R> void across(R r) { use(r, constraint(r), m_range.end + 1); }
  void across(Vtuple uses) { for (auto r : m_unit.tuples[uses]) across(r); }
  void across(RegSet regs) { regs.forEach([&](Vreg r) { across(r); }); }
  void across(Vptr m) {
    if (m.base.isValid()) across(m.base);
    if (m.index.isValid()) across(m.index);
  }
  template<Width w> void across(Vp<w> m) { across(static_cast<Vptr>(m)); }

private:
  void use(Vreg r, Constraint kind, unsigned end, Vreg hint = Vreg{}) {
    m_live.set(r);

    auto var = m_variables[r];
    if (!var) var = m_variables[r] = Variable::create(r);
    addRange(var->ivl(), {m_range.start, end});

    if (!var->fixed()) {
      if (m_inst.op == Vinstr::copyargs ||
          m_inst.op == Vinstr::copy2 ||
          m_inst.op == Vinstr::copy ||
          m_inst.op == Vinstr::phijmp) {
        // all these instructions lower to parallel copyplans, which know
        // how to load directly from constants or spilled locations
        kind = Constraint::CopySrc;
      }
      var->ivl()->uses.push_back({kind, m_range.end, hint});
    }
  }

private:
  const Vunit& m_unit;
  jit::vector<Variable*>& m_variables;
  LiveSet& m_live;
  const Vinstr& m_inst;
  const LiveRange m_range;
};

/*
 * Compute lifetime intervals and use positions of all Vregs by walking the
 * code bottom-up once.
 */
jit::vector<Variable*> buildIntervals(const Vunit& unit,
                                      const VxlsContext& ctx) {
  ONTRACE(kVasmRegAllocDetailLevel, printCfg(unit, ctx.blocks));

  auto variables = jit::vector<Variable*>{unit.next_vr};

  for (auto b : boost::adaptors::reverse(ctx.blocks)) {
    auto& block = unit.blocks[b];

    // initial live set is the union of successor live sets.
    LiveSet live(unit.next_vr);
    for (auto s : succs(block)) {
      always_assert(!ctx.livein[s].empty());
      live |= ctx.livein[s];
    }

    // add a range covering the whole block to every live interval
    auto& block_range = ctx.block_ranges[b];
    forEach(live, [&](Vreg r) {
      if (!variables[r]) variables[r] = Variable::create(r);
      addRange(variables[r]->ivl(), block_range);
    });

    // visit instructions bottom-up, adding uses & ranges
    auto pos = block_range.end;
    for (auto const& inst : boost::adaptors::reverse(block.code)) {
      pos -= 2;
      RegSet implicit_uses, implicit_across, implicit_defs;
      getEffects(ctx.abi, inst, implicit_uses, implicit_across, implicit_defs);

      DefVisitor dv(unit, variables, live, b, inst, pos);
      visitOperands(inst, dv);
      dv.def(implicit_defs);

      UseVisitor uv(unit, variables, live, inst, {block_range.start, pos});
      visitOperands(inst, uv);
      uv.use(implicit_uses);
      uv.across(implicit_across);
      if (inst.op == Vinstr::recordbasenativesp) {
        forEach(live, [&](Vreg r) {
          if (!unit.regToConst.contains(r)) {
            // We mark the instruction as a use so no spills span the
            // instruction  unless they have to.
            uv.use(r);
            if (!variables[r]) variables[r] = Variable::create(r);
            variables[r]->recordbasenativesps.push_back(pos);
          }
        });
      } else if (inst.op == Vinstr::unrecordbasenativesp) {
        forEach(live, [&](Vreg r) {
          if (!unit.regToConst.contains(r)) {
            // We mark the instruction as a use so no spills span the
            // instruction  unless they have to.
            uv.use(r);
          }
        });
      }
    }

    // sanity check liveness computation
    always_assert(live == ctx.livein[b]);
  }

  // finish processing live ranges for constants
  for (auto& c : unit.constToReg) {
    if (auto var = variables[c.second]) {
      var->ivl()->ranges.back().start = 0;
      var->def_pos = 0;
      var->constant = true;
      var->val = c.first;
    }
  }

  // Ranges and uses were generated in reverse order.  Unreverse them now.
  for (auto var : variables) {
    if (!var) continue;
    auto ivl = var->ivl();
    assertx(!ivl->ranges.empty()); // no empty intervals
    std::reverse(ivl->uses.begin(), ivl->uses.end());
    std::reverse(ivl->ranges.begin(), ivl->ranges.end());
  }
  ONTRACE(kVasmRegAllocDetailLevel,
    printVariables("after building intervals", unit, ctx, variables);
  );

  if (debug) {
    // only constants and physical registers can be live-into the entry block.
    forEach(ctx.livein[unit.entry], [&](Vreg r) {
      UNUSED auto var = variables[r];
      assertx(var->constant || var->fixed());
    });
    for (auto var : variables) {
      if (!var) continue;
      auto const ivl = var->ivl();
      for (unsigned i = 1; i < ivl->uses.size(); i++) {
        assertx(ivl->uses[i].pos >= ivl->uses[i-1].pos); // monotonic
      }
      for (unsigned i = 1; i < ivl->ranges.size(); i++) {
        assertx(ivl->ranges[i].end > ivl->ranges[i].start); // no empty ranges
        assertx(ivl->ranges[i].start > ivl->ranges[i-1].end); // no empty gaps
      }
    }
  }
  return variables;
}

///////////////////////////////////////////////////////////////////////////////

/*
 * A map from PhysReg number to position.
 */
using PosVec = PhysReg::Map<unsigned>;

/*
 * Between `r1' and `r2', choose the one whose position in `pos_vec' is closest
 * to but still after (or at) `pos'.
 *
 * If neither has a position after `pos', choose the higher one.  If both have
 * the same position, choose `r1'.
 */
PhysReg choose_closest_after(unsigned pos, const PosVec& pos_vec,
                             PhysReg r1, PhysReg r2) {
  if (r1 == InvalidReg) return r2;
  if (r2 == InvalidReg) return r1;

  if (pos_vec[r1] >= pos) {
    if (pos <= pos_vec[r2] && pos_vec[r2] < pos_vec[r1]) {
      return r2;
    }
  } else {
    if (pos_vec[r2] > pos_vec[r1]) {
      return r2;
    }
  }
  return r1;
}

/*
 * Like choose_closest_after(), but iterates through `pos_vec'.
 */
PhysReg find_closest_after(unsigned pos, const PosVec& pos_vec) {
  auto ret = InvalidReg;
  for (auto const r : pos_vec) {
    ret = choose_closest_after(pos, pos_vec, ret, r);
  }
  return ret;
}

///////////////////////////////////////////////////////////////////////////////
// Hints.

/*
 * A member of a "phi group"---i.e., the set of all variables which flow into
 * one another via a set of corresponding phijmp's and phidef's.
 */
struct PhiVar {
  Vreg r;       // the Vreg that is used or def'd in the phi
  unsigned pos; // position of the phijmp or phidef
};

/*
 * Hint metadata that doesn't fit into the `variables' vector.
 */
struct HintInfo {
  jit::vector<jit::vector<PhiVar>> phi_groups;
};

/*
 * Add the phi use of `r' at `pos' to the phi group given by `pgid'.
 */
void addPhiGroupMember(const jit::vector<Variable*>& variables,
                       jit::vector<jit::vector<PhiVar>>& phi_groups,
                       Vreg r, unsigned pos, int pgid,
                       Vreg hint = Vreg{}) {
  assertx(phi_groups.size() > pgid);
  assertx(!phi_groups[pgid].empty());

  auto const var = variables[r];
  assertx(var != nullptr);
  assertx(!var->fixed());

  auto const ivl = var->ivl();
  auto& u = pos == var->def_pos
    ? ivl->uses.front()
    : ivl->uses[ivl->findUse(pos)];
  assertx(u.pos == pos);

  if (hint.isValid()) u.hint = hint;

  u.phi_group = pgid;
  phi_groups[pgid].push_back({r, u.pos});
}

/*
 * Collect hint metadata for phis.
 *
 * The return value of this pass represents an equivalence relation over phi
 * uses and defs.  The relation is defined as follows:
 *
 * Consider a phidef F with arity n (i.e., it defines n variables).  F gives
 * rise to n equivalence classes, each of which consists in the i-th use or def
 * variable from each phi instruction in the transitive closure of the predicate
 *
 *    P(f) := { every phijmp which targets f, and every other phidef
 *              targeted by those phijmp's }
 *
 * applied to F.
 *
 * Or, taking a pictoral example:
 *     _________________________________________________________
 *    |                                                         |
 *    |                                           +---------+   |
 *    |                                           |         |   |
 *    |                                           v         |   |
 *    | phijmp(x1, y1)    phijmp(x2, y2)    phijmp(x3, y3)  .   |
 *    |       |                 |                 |         .   |
 *    |       +-----------------+   +-------------+         .   |
 *    |       |                     |                       |   |
 *    |       v                     v                       |   |
 *    | phidef(x4, y4)    phidef(x5, y5) -------------------+   |
 *    |       |                                                 |
 *    |______ . ________________________________________________|
 *            .
 *            .
 *     ______ | __________________________
 *    |       v                           |
 *    | phijmp(x6, y6)--->phidef(x7, y7)  |
 *    |___________________________________|
 *
 * The equivalence classes in this example are {x1, x2, x4}, {y1, y2, y4},
 * {x3, x5}, {y3, y5}, {x6, x7}, and {y6, y7}.
 *
 * We use these equivalence classes during register allocation to try to assign
 * the same register to all the variables in a phi group (at least at those
 * positions where the phi instructions occur).
 *
 * Note that this equivalence relation does /not/ capture the idea that we
 * probably want the same register for x4 as we do for x6 and x7.  To track that
 * information, we set the `hint' on phi uses to the corresponding phidef
 * variable, then let the hint back propagation pass handle it.  (Note that we
 * do /not/ set the `hint' field at phi defs, nor do we try to use it at any
 * point.)
 */
jit::vector<jit::vector<PhiVar>>
analyzePhiHints(const Vunit& unit, const VxlsContext& ctx,
                const jit::vector<Variable*>& variables) {
  auto phi_groups = jit::vector<jit::vector<PhiVar>>{};

  /*
   * Get the phi group for r's def, optionally allocating a new phi group if
   * necessary.
   */
  auto const def_phi_group = [&] (Vreg r, bool alloc) {
    auto const var = variables[r];
    assertx(var != nullptr);
    assertx(var->def_inst.op == Vinstr::phidef);

    auto& u = var->ivl()->uses.front();

    if (u.phi_group == kInvalidPhiGroup && alloc) {
      u.phi_group = phi_groups.size();
      phi_groups.emplace_back(jit::vector<PhiVar>{{r, u.pos}});
    }
    return u.phi_group;
  };

  auto const is_fixed = [&] (Vreg r) { return variables[r]->fixed(); };

  for (auto const b : ctx.blocks) {
    auto const& code = unit.blocks[b].code;
    if (code.empty()) continue;

    auto const& first = code.front();
    auto const& last = code.back();

    if (first.op == Vinstr::phidef) {
      // A phidef'd variable just need to be assigned a phi group if it doesn't
      // already have one (i.e., from handling a phijmp).
      auto const& defs = unit.tuples[first.phidef_.defs];
      for (auto const r : defs) {
        if (is_fixed(r)) continue;
        def_phi_group(r, true);
      }
    }

    if (last.op == Vinstr::phijmp) {
      auto const target = last.phijmp_.target;
      auto const& def_inst = unit.blocks[target].code.front();
      assertx(def_inst.op == Vinstr::phidef);

      auto const& uses = unit.tuples[last.phijmp_.uses];
      auto const& defs = unit.tuples[def_inst.phidef_.defs];
      assertx(uses.size() == defs.size());

      // Set the phi group for each phi use variable to that of the
      // corresponding phi def variable (allocating a new group if the def
      // variable has not already been assigned one).
      for (size_t i = 0, n = uses.size(); i < n; ++i) {
        if (is_fixed(defs[i]) || is_fixed(uses[i])) continue;

        auto const pgid = def_phi_group(defs[i], true);
        addPhiGroupMember(variables, phi_groups,
                          uses[i], last.id, pgid, defs[i]);
      }
    }
  }
  return phi_groups;
}

HintInfo analyzeHints(const Vunit& unit, const VxlsContext& ctx,
                      const jit::vector<Variable*>& variables) {
  HintInfo hint_info;
  hint_info.phi_groups = analyzePhiHints(unit, ctx, variables);

  return hint_info;
}

/*
 * Try to return the physical register that would coalesce `ivl' with its
 * hinted source.
 */
PhysReg tryCoalesce(const jit::vector<Variable*>& variables,
                    const Interval* ivl) {
  assertx(ivl == ivl->leader());
  assertx(!ivl->uses.empty());

  auto const& def = ivl->uses.front();
  assertx(def.pos == ivl->var->def_pos);

  if (!def.hint.isValid()) return InvalidReg;
  auto const hint_var = variables[def.hint];

  for (auto hvl = hint_var->ivl(); hvl; hvl = hvl->next) {
    if (def.pos == hvl->end()) return hvl->reg;
  }
  return InvalidReg;
}

/*
 * Scan the uses of `ivl' for phi nodes, and return a pair of hints for the
 * first one we find.
 *
 * For a given phi node F, the first of the two hints we return is derived from
 * only those phijmps targeting F (if F is a phidef), or from the phidefs F
 * targets (if F is a phijmp).  The second hint accounts for F's entire
 * equivalence class---see analyzePhiHints() for more details.
 */
Optional<std::pair<PhysReg,PhysReg>>
tryPhiHint(const jit::vector<Variable*>& variables,
           const Interval* ivl, const HintInfo& hint_info,
           const PosVec& free_until) {
  auto const choose = [&] (PhysReg h1, PhysReg h2) {
    return choose_closest_after(ivl->end(), free_until, h1, h2);
  };

  for (auto const& u : ivl->uses) {
    // Look for the first phi hint.
    if (u.phi_group == kInvalidPhiGroup) continue;

    auto preferred = InvalidReg;
    auto group_hint = InvalidReg;

    // Choose a register to be the hint.
    for (auto const& phiv : hint_info.phi_groups[u.phi_group]) {
      auto const var = variables[phiv.r];
      assertx(var);

      auto const phivl = var->ivlAtUse(phiv.pos);
      if (phivl->reg == InvalidReg) continue;

      auto const& phu = phivl->uses[phivl->findUse(phiv.pos)];
      assertx(phu.pos == phiv.pos);

      // Prefer to use a hint from a corresponding phi node.
      if (u.hint == phiv.r || phu.hint == ivl->var->vreg) {
        preferred = choose(preferred, phivl->reg);
      }
      // In the end, though, we're okay with any hint from the group.
      group_hint = choose(group_hint, phivl->reg);
    }
    return std::make_pair(preferred, group_hint);
  }
  return std::nullopt;
}

/*
 * Choose an appropriate hint for the (sub-)interval `ivl'.
 *
 * The register allocator passes us constraints via `free_until' and `allow'.
 * We use the `free_until' vector to choose a hint that most tightly covers the
 * lifetime of `ivl'; we use `allow' to ensure that our hint satisfies
 * constraints.
 */
PhysReg chooseHint(const jit::vector<Variable*>& variables,
                   const Interval* ivl, const HintInfo& hint_info,
                   const PosVec& free_until, RegSet allow) {
  if (!Cfg::HHIR::EnablePreColoring &&
      !Cfg::HHIR::EnableCoalescing) return InvalidReg;

  auto const choose = [&] (PhysReg h1, PhysReg h2) {
    return choose_closest_after(ivl->end(), free_until, h1, h2);
  };
  auto hint = InvalidReg;

  // If `ivl' contains its def, try a coalescing hint.
  if (ivl == ivl->leader()) {
    hint = tryCoalesce(variables, ivl);
  }

  // Return true if we should return `h'.
  auto const check_and_trace = [&] (PhysReg h, const char* prefix) {
    if (h == InvalidReg) return false;
    if (allow.contains(h)) {
      FTRACE(kHintLevel, "{}hinting {} for %{} @ {}\n",
             prefix, show(h), size_t(ivl->var->vreg), ivl->start());
      return true;
    }
    FTRACE(kHintLevel, "  found mismatched hint for %{} @ {}\n",
           size_t(ivl->var->vreg), ivl->uses.front().pos);
    return false;
  };

  auto const is_phi_use = !ivl->uses.empty() &&
                          ivl->uses.front().phi_group != kInvalidPhiGroup;

  // If we have either a backwards or a forwards hint, return it---if we have
  // both, return whichever is free the longest.  For phi uses, however, we
  // want to consider the whole phi group's allocations first.
  if (!is_phi_use && check_and_trace(hint, "")) return hint;

  // Try to determine a hint via the first phi use in the interval.
  auto const phi_hints = tryPhiHint(variables, ivl, hint_info, free_until);
  bool is_phi_hint = false;

  if (phi_hints) {
    hint = choose(phi_hints->first, hint);
    if (hint != phi_hints->first) {
      hint = choose(hint, phi_hints->second);
    }
    is_phi_hint = hint == phi_hints->first ||
                  hint == phi_hints->second;
  }
  if (phi_hints || is_phi_use) {
    if (check_and_trace(hint, debug && is_phi_hint ? "phi-" : "")) {
      return hint;
    }
  }

  // Crawl through our uses and look for a physical hint.
  for (auto const& u : ivl->uses) {
    if (!u.hint.isValid() ||
        !u.hint.isPhys() ||
        !allow.contains(u.hint)) continue;
    hint = choose(hint, u.hint);
  }

  if (hint != InvalidReg) {
    FTRACE(kHintLevel, "physical hint {} for %{} @ {}\n",
           show(hint), size_t(ivl->var->vreg), ivl->start());
  }
  return hint;
}

///////////////////////////////////////////////////////////////////////////////
// Register allocation.

/*
 * Information about spills generated by register allocation.
 *
 * Used for the allocateSpillSpace() pass which inserts the instructions that
 * create spill space on the stack.
 */
struct SpillInfo {
  // Number of intervals spilled.
  unsigned num_spills{0};
  // Number of spill slots used.
  size_t used_spill_slots{0};
};

/*
 * Extended Linear Scan register allocator over vasm virtual registers (Vregs).
 *
 * This encapsulates the intermediate data structures used during the
 * allocation phase of the algorithm so we don't have to pass them around
 * everywhere.
 */
struct Vxls {
  Vxls(const VxlsContext& ctx,
       const jit::vector<Variable*>& variables,
       const HintInfo& hint_info)
    : ctx(ctx)
    , variables(variables)
    , hint_info(hint_info)
  {}

  SpillInfo go();

private:
  void assignSpill(Interval* ivl);
  void spill(Interval*);
  void assignReg(Interval*, PhysReg);

  unsigned nearestSplitBefore(unsigned pos);
  unsigned constrain(Interval*, RegSet&);

  void update(Interval*);
  void allocate(Interval*);
  void allocBlocked(Interval*);
  void spillOthers(Interval* current, PhysReg r);

private:
  /*
   * Comparison function for pending priority queue.
   *
   * std::priority_queue requires a less operation, but sorts the heap
   * highest-first; we need the opposite (lowest-first), so use greater-than.
   */
  struct Compare {
    bool operator()(const Interval* i1, const Interval* i2) {
      return i1->start() > i2->start();
    }
  };

private:
  const VxlsContext& ctx;

  // Variables, null if unused.
  const jit::vector<Variable*>& variables;
  // Hint metadata.
  const HintInfo& hint_info;
  // Subintervals sorted by Interval start.
  jit::priority_queue<Interval*,Compare> pending;
  // Intervals that overlap.
  jit::vector<Interval*> active, inactive;
  // Last position each spill slot was owned; kMaxPos means currently used.
  jit::array<unsigned, kMaxSpillSlots> spill_slots{{0}};
  // Stats on spills.
  SpillInfo spill_info;
};

SpillInfo Vxls::go() {
  for (auto var : variables) {
    if (!var) continue;
    if (var->fixed()) {
      assignReg(var->ivl(), var->vreg);
    } else if (var->constant) {
      spill(var->ivl());
    } else {
      pending.push(var->ivl());
    }
  }
  while (!pending.empty()) {
    auto current = pending.top();
    pending.pop();
    update(current);
    allocate(current);
  }
  return spill_info;
}

/*
 * Assign the next available spill slot to `ivl'.
 */
void Vxls::assignSpill(Interval* ivl) {
  assertx(!ivl->fixed() && ivl != ivl->leader());

  if (ivl->var->slot != kInvalidSpillSlot) return;

  auto& used_spill_slots = spill_info.used_spill_slots;

  auto const assign_slot = [&] (size_t slot) {
    ivl->var->slot = slot;
    ++spill_info.num_spills;

    spill_slots[slot] = kMaxPos;
    if (!ivl->var->wide) {
      used_spill_slots = std::max(used_spill_slots, slot + 1);
    } else {
      used_spill_slots = std::max(used_spill_slots, slot + 2);
      spill_slots[slot + 1] = kMaxPos;
    }
  };

  // Assign spill slots.  We track the highest position at which a spill slot
  // was owned, and only reassign it to a Vreg if its lifetime interval
  // (including all splits) is strictly above that high water mark.
  if (!ivl->var->wide) {
    for (size_t slot = 0, n = spill_slots.size(); slot < n; ++slot) {
      if (ivl->leader()->start() >= spill_slots[slot]) {
        return assign_slot(slot);
      }
    }
  } else {
    for (size_t slot = 0, n = spill_slots.size() - 1; slot < n; slot += 2) {
      if (ivl->leader()->start() >= spill_slots[slot] &&
          ivl->leader()->start() >= spill_slots[slot + 1]) {
        return assign_slot(slot);
      }
    }
  }

  // Ran out of spill slots.
  ONTRACE(kVasmRegAllocDetailLevel,
          dumpVariables(variables, spill_info.num_spills));
  TRACE(1, "vxls-punt TooManySpills\n");
  TRACE_PUNT("LinearScan_TooManySpills");
}

/*
 * Assign `r' to `ivl'.
 */
void Vxls::assignReg(Interval* ivl, PhysReg r) {
  if (!ivl->fixed() && ivl->uses.empty()) {
    ivl->reg = InvalidReg;
    if (!ivl->constant()) assignSpill(ivl);
  } else {
    ivl->reg = r;
    active.push_back(ivl);
  }
}

/*
 * Spill `ivl' from its start until its first register use.
 *
 * Spill `ivl' if there is no use; otherwise split the interval just before the
 * use, and enqueue the second part.
 */
void Vxls::spill(Interval* ivl) {
  unsigned first_use = ivl->firstUse();
  if (first_use <= ivl->end()) {
    auto split_pos = nearestSplitBefore(first_use);
    if (split_pos <= ivl->start()) {
      // This only can happen if we need more than the available registers
      // at a single position.  It can happen in phijmp or callargs.
      TRACE(1, "vxls-punt RegSpill\n");
      TRACE_PUNT("RegSpill"); // cannot split before first_use
    }
    pending.push(ivl->split(split_pos));
  }
  ivl->reg = InvalidReg;
  if (!ivl->constant()) assignSpill(ivl);
}

/*
 * Update the active and inactive lists for the start of `current'.
 */
void Vxls::update(Interval* current) {
  auto const pos = current->start();

  auto const free_spill_slot = [this] (Interval* ivl) {
    assertx(!ivl->next);
    auto slot = ivl->var->slot;

    if (slot != kInvalidSpillSlot) {
      if (ivl->var->wide) {
        assertx(spill_slots[slot + 1]);
        spill_slots[slot + 1] = ivl->end();
      }
      assertx(spill_slots[slot]);
      spill_slots[slot] = ivl->end();
    }
  };

  // Check for active/inactive intervals that have expired or which need their
  // polarity flipped.
  auto const update_list = [&] (jit::vector<Interval*>& target,
                                jit::vector<Interval*>& other,
                                bool is_active) {
    auto end = target.end();
    for (auto i = target.begin(); i != end;) {
      auto ivl = *i;
      if (pos >= ivl->end()) {
        *i = *--end;
        if (!ivl->next) free_spill_slot(ivl);
      } else if (is_active ? !ivl->covers(pos) : ivl->covers(pos)) {
        *i = *--end;
        other.push_back(ivl);
      } else {
        i++;
      }
    }
    target.erase(end, target.end());
  };
  update_list(active, inactive, true);
  update_list(inactive, active, false);
}

/*
 * Return the closest split position on or before `pos'.
 *
 * The result might be exactly on an edge, or in-between instruction positions.
 */
unsigned Vxls::nearestSplitBefore(unsigned pos) {
  auto b = blockFor(ctx, pos);
  auto range = ctx.block_ranges[b];
  if (pos == range.start) return pos;
  return (pos - 1) | 1;
}

/*
 * Constrain the allowable registers for `ivl' by inspecting uses.
 *
 * Returns the latest position for which `allow' (which we populate) is valid.
 * We use this return value to fill the `free_until' PosVec in allocate()
 * below.  That data structure tracks the first position at which a register is
 * /unavailable/, so it would appear that constrain()'s return value is
 * off-by-one.
 *
 * In fact, it is not; we actually /need/ this position offsetting because of
 * our leniency towards having uses at an Interval's end() position.  If we
 * fail to constrain on an end-position use, we must still split and spill.
 * (In contrast, if we intersect with another Interval on an end position use,
 * it's okay because SSA tells us that the conflict must be the other
 * Interval's def position, and a use and a def at the same position don't
 * actually conflict; see the fun ASCII diagram that adorns the definition of
 * Interval).
 */
unsigned Vxls::constrain(Interval* ivl, RegSet& allow) {
  auto const any = ctx.abi.unreserved() - ctx.abi.sf; // Any but not flags.
  allow = ctx.abi.unreserved();
  for (auto& u : ivl->uses) {
    auto need = u.kind == Constraint::Simd ? ctx.abi.simdUnreserved :
                u.kind == Constraint::Gpr ? ctx.abi.gpUnreserved :
                u.kind == Constraint::Sf ? ctx.abi.sf :
                any; // Any or CopySrc
    if ((allow & need).empty()) {
      // cannot satisfy constraints; must split before u.pos
      return u.pos - 1;
    }
    allow &= need;
  }
  return kMaxPos;
}

/*
 * Return the next intersection point between `current' and `other', or kMaxPos
 * if they never intersect.
 *
 * We assume that `other', if it represents an SSA variable, is not live at the
 * start of `current'.
 *
 * Note that if two intervals intersect, the first point of intersection will
 * always be the start of one of the intervals, because SSA ensures that a def
 * dominates all uses, and hence all live ranges as well.
 */
unsigned nextIntersect(const Interval* current, const Interval* other) {
  assertx(!current->fixed());

  if (current == current->leader() &&
      other == other->leader() &&
      !other->fixed()) {
    // Since `other' is an inactive Vreg interval, it cannot cover current's
    // start, and `current' cannot cover other's start, since `other' started
    // earlier.  Therefore, SSA guarantees no intersection.
    return kMaxPos;
  }
  if (current->end() <= other->start() ||
      other->end() <= current->start()) {
    // One ends before the other starts.
    return kMaxPos;
  }
  // r1,e1 span all of current
  auto r1 = current->ranges.begin();
  auto e1 = current->ranges.end();
  // r2,e2 span the tail of other that might intersect current
  auto r2 = other->ranges.begin() + other->findRange(current->start());
  auto e2 = other->ranges.end();
  // search for the lowest position covered by current and other
  for (;;) {
    if (r1->start < r2->start) {
      if (r2->start < r1->end) return r2->start;
      if (++r1 == e1) return kMaxPos;
    } else {
      if (r1->start < r2->end) return r1->start;
      if (++r2 == e2) return kMaxPos;
    }
  }
  return kMaxPos;
}

void Vxls::allocate(Interval* current) {
  // Map from PhysReg until the first position at which it is /not/ available.
  PosVec free_until; // 0 by default

  RegSet allow;
  auto const conflict = constrain(current, allow);

  // Mark regs that fit our constraints as free up until the point of conflict,
  // unless they're owned by active intervals---then mark them used.
  allow.forEach([&](PhysReg r) {
    free_until[r] = conflict;
  });
  for (auto ivl : active) {
    free_until[ivl->reg] = 0;
  }

  // Mark each reg assigned to an inactive interval as only free until the
  // first position at which `current' intersects that interval.
  for (auto ivl : inactive) {
    auto r = ivl->reg;
    if (free_until[r] == 0) continue;
    auto until = nextIntersect(current, ivl);
    free_until[r] = std::min(until, free_until[r]);
  }

  if (current->ranges.size() > 1) {
    auto const b = blockFor(ctx, current->start());
    auto const blk_range = ctx.block_ranges[b];
    if (blk_range.end > current->ranges[0].end) {
      // We're assigning a register to an interval with
      // multiple ranges, but the vreg isn't live out
      // of the first range. This means there's no
      // connection between this range and any subsequent
      // one, so we can safely break the interval
      // after the first range without making things worse.
      // On the other hand, it can make things better, by
      // eg not assigning a constant to a register in an
      // unlikely exit block, and then holding it in a callee save
      // reg across lots of unrelated code until its used
      // again in another unlikely exit block.
      auto second = current->split(blk_range.end, false);
      pending.push(second);
    } else if (current->constant() &&
               current->uses.size() &&
               current->uses[0].pos >= blk_range.end) {
      // we probably don't want to load a constant into a register
      // at the start of a block where its not used.
      return spill(current);
    }
  }

  // Choose a register to allocate to `current', either via a hint or by
  // various heuristics.
  auto const choose_reg = [&] {
    auto const hint = chooseHint(variables, current,
                                 hint_info, free_until, allow);
    if (hint != InvalidReg &&
        free_until[hint] >= current->end()) {
      return hint;
    }

    auto ret = InvalidReg;
    for (auto const r : free_until) {
      ret = choose_closest_after(current->end(), free_until, ret, r);
    }
    return ret;
  };

  auto const r = choose_reg();
  auto const pos = free_until[r];
  if (pos >= current->end()) {
    return assignReg(current, r);
  }

  if (pos > current->start()) {
    // `r' is free for the first part of current.
    auto const prev_use = current->lastUseBefore(pos);

    DEBUG_ONLY auto min_split = std::max(prev_use, current->start() + 1);
    assertx(min_split <= pos);

    auto split_pos = nearestSplitBefore(pos);
    if (split_pos > current->start()) {
      if (prev_use && prev_use < split_pos) {
        // If there are uses in previous blocks, but no uses between the start
        // of the block containing `split_pos' and `split_pos' itself, we
        // should split earlier; otherwise we'll need to insert moves/loads on
        // the edge(s) into this block, which clearly can't be used since we're
        // spilling before the first use.  Might as well spill on a block
        // boundary, as early as possible.
        auto prev_range_idx = current->findRange(prev_use);
        auto prev_range = &current->ranges[prev_range_idx];
        if (prev_range->start <= prev_use && prev_range->end < split_pos) {
          prev_range++;
        }
        if (prev_range->start > prev_use && prev_range->start < split_pos) {
          split_pos = prev_range->start;
        }
      }

      // Split `current'.  We keep uses at the end of the first split because
      // we know that `r' is free up to /and including/ that position.
      auto second = current->split(split_pos, true /* keep_uses */);
      pending.push(second);

      // Try to find a register again.  Since `current' is shorter, we might
      // have a different hint, or a different heuristically-determined
      // best-reg.
      auto const r = choose_reg();
      assertx(free_until[r] >= current->end());
      return assignReg(current, r);
    }
  }

  // Must spill `current' or another victim.
  allocBlocked(current);
}

/*
 * When all registers are in use, find a good interval (possibly `current') to
 * split and spill.
 *
 * When an interval is split and the second part is spilled, possibly split the
 * second part again before the next use-pos that requires a register, and
 * enqueue the third part.
 */
void Vxls::allocBlocked(Interval* current) {
  auto const cur_start = current->start();

  RegSet allow;
  auto const conflict = constrain(current, allow); // repeated from allocate

  // Track the positions (a) at which each PhysReg is next used by any lifetime
  // interval to which it's assigned (`used'), and (b) at which each PhysReg is
  // next assigned to a value whose lifetime intersects `current' (`blocked').
  PosVec used, blocked;
  allow.forEach([&](PhysReg r) { used[r] = blocked[r] = conflict; });

  // compute next use of active registers, so we can pick the furthest one
  for (auto ivl : active) {
    if (ivl->fixed()) {
      blocked[ivl->reg] = used[ivl->reg] = 0;
    } else {
      auto use_pos = ivl->firstUseAfter(cur_start);
      used[ivl->reg] = std::min(use_pos, used[ivl->reg]);
    }
  }

  // compute next intersection/use of inactive regs to find what's free longest
  for (auto ivl : inactive) {
    auto const r = ivl->reg;
    if (blocked[r] == 0) continue;

    auto intersect_pos = nextIntersect(current, ivl);
    if (intersect_pos == kMaxPos) continue;

    if (ivl->fixed()) {
      blocked[r] = std::min(intersect_pos, blocked[r]);
      used[r] = std::min(blocked[r], used[r]);
    } else {
      auto use_pos = ivl->firstUseAfter(cur_start);
      used[r] = std::min(use_pos, used[r]);
    }
  }

  // Choose the best victim register(s) to spill---the one with first-use
  // after, but closest to, the lifetime of `current'; or the one with farthest
  // first-use if no such register exists.
  auto r = find_closest_after(current->end(), used);

  // If all other registers are used by their owning intervals before the first
  // register-use of `current', then we have to spill `current'.
  if (used[r] < current->firstUse()) {
    return spill(current);
  }

  auto const block_pos = blocked[r];
  if (block_pos < current->end()) {
    // If /every/ usable register is assigned to a lifetime interval which
    // intersects with `current', we have to split current before that point.
    auto prev_use = current->lastUseBefore(block_pos);

    DEBUG_ONLY auto min_split = std::max(prev_use, cur_start + 1);
    auto max_split = block_pos;
    assertx(cur_start < min_split && min_split <= max_split);

    auto split_pos = nearestSplitBefore(max_split);
    if (split_pos > current->start()) {
      auto second = current->split(split_pos, true /* keep_uses */);
      pending.push(second);
    }
  }
  spillOthers(current, r);
  assignReg(current, r);
}

/*
 * Split and spill other intervals that conflict with `current' for register r,
 * at current->start().
 *
 * If necessary, split the victims again before their first use position that
 * requires a register.
 */
void Vxls::spillOthers(Interval* current, PhysReg r) {
  auto const cur_start = current->start();

  // Split `ivl' at `cur_start' and spill the second part.  If `cur_start' is
  // too close to ivl->start(), spill all of `ivl' instead.
  auto const spill_after = [&] (Interval* ivl) {
    auto const split_pos = nearestSplitBefore(cur_start);
    auto const tail = split_pos <= ivl->start() ? ivl : ivl->split(split_pos);
    spill(tail);
  };

  // Split and spill other active intervals after `cur_start'.
  auto end = active.end();
  for (auto i = active.begin(); i != end;) {
    auto other = *i;
    if (other->fixed() || r != other->reg) {
      i++; continue;
    }
    *i = *--end;
    spill_after(other);
  }
  active.erase(end, active.end());

  // Split and spill any inactive intervals after `cur_start' if they intersect
  // with `current'.
  end = inactive.end();
  for (auto i = inactive.begin(); i != end;) {
    auto other = *i;
    if (other->fixed() || r != other->reg) {
      i++; continue;
    }
    auto intersect = nextIntersect(current, other);
    if (intersect >= current->end()) {
      i++; continue;
    }
    *i = *--end;
    spill_after(other);
  }
  inactive.erase(end, inactive.end());
}

SpillInfo assignRegisters(const VxlsContext& ctx,
                          const jit::vector<Variable*>& variables,
                          const HintInfo& hint_info) {
  return Vxls(ctx, variables, hint_info).go();
}

///////////////////////////////////////////////////////////////////////////////
// Lifetime continuity resolution.

/*
 * A pair of source block number and successor index, used to identify an
 * out-edge.
 */
using EdgeKey = std::pair<Vlabel,unsigned>;

struct EdgeHasher {
  size_t operator()(EdgeKey k) const {
    return size_t(k.first) ^ k.second;
  }
};

/*
 * Copies that are required at a given position or edge.
 *
 * The keys into the PhysReg::Map are the dests; the Interval*'s are the
 * sources (nullptr if no copy is needed).
 */
using CopyPlan = PhysReg::Map<Interval*>;

/*
 * Copy and spill points for resolving split lifetime intervals.
 *
 * After register allocation, some lifetime intervals may have been split, and
 * their Vregs assigned to different physical registers or spill locations.  We
 * use this struct to track where we need to add moves to maintain continuity.
 * (We also use it to resolve phis.)
 */
struct ResolutionPlan {
  // Where to insert spills.
  jit::hash_map<unsigned,CopyPlan> spills;
  // Where to insert reg-reg moves, or spill and constant loads, between
  // instructions (or in place of copy{} and friends).
  jit::hash_map<unsigned,CopyPlan> copies;

  // Copies and loads on edges (between blocks).
  jit::hash_map<EdgeKey,CopyPlan,EdgeHasher> edge_copies;
};

template<class CopyPlanT>
using void_req = typename std::enable_if<
  std::is_same<CopyPlanT, CopyPlan>::value ||
  std::is_same<CopyPlanT, const CopyPlan>::value
>::type;

/*
 * Iterators for reg-reg copies and for {const,spill}-reg loads.
 */
template<class CopyPlanT, class F>
void_req<CopyPlanT> for_each_copy(CopyPlanT& plan, F f) {
  for (auto dst : plan) {
    auto& ivl = plan[dst];
    if (!ivl || !ivl->live()) continue;
    f(dst, ivl);
  }
}
template<class CopyPlanT, class F>
void_req<CopyPlanT> for_each_load(CopyPlanT& plan, F f) {
  for (auto dst : plan) {
    auto& ivl = plan[dst];
    if (!ivl || ivl->live()) continue;
    assertx(ivl->constant() || ivl->spilled());
    f(dst, ivl);
  }
}

/*
 * Insert a spill after the def-position in `ivl'.
 *
 * There's only one such position, because of SSA.
 */
void insertSpill(const VxlsContext& ctx,
                 ResolutionPlan& resolution, Interval* ivl) {
  auto DEBUG_ONLY checkPos = [&](unsigned pos) {
    assertx(pos % 2 == 1);
    DEBUG_ONLY auto const b = blockFor(ctx, pos);
    DEBUG_ONLY auto const& range = ctx.block_ranges[b];
    assertx(pos - 1 >= range.start && pos + 1 < range.end);
    return true;
  };
  auto const spill = [&] (unsigned pos) {
    assertx(checkPos(pos));
    resolution.spills[pos][ivl->reg] = ivl; // store ivl->reg => ivl->slot
  };
  if (ivl->var->recordbasenativesps.empty()) {
    spill(ivl->var->def_pos + 1);
  } else {
    for (auto const pos : ivl->var->recordbasenativesps) {
      always_assert_flog(ivl->covers(pos),
                         "Spilling required before native sp set");
      spill(pos + 1);
    }
  }
}

/*
 * Insert spills and copies that connect subintervals that were split
 * between instructions.
 */
void resolveSplits(const VxlsContext& ctx,
                   const jit::vector<Variable*>& variables,
                   ResolutionPlan& resolution) {
  for (auto var : variables) {
    if (!var) continue;
    auto i1 = var->ivl();
    if (var->slot >= 0) insertSpill(ctx, resolution, i1);

    for (auto i2 = i1->next; i2; i1 = i2, i2 = i2->next) {
      auto const pos = i2->start();
      if (i1->end() != pos) continue; // spans lifetime hole
      if (i2->reg == InvalidReg) continue; // no load necessary
      if (i2->reg == i1->reg) continue; // no copy necessary

      auto const b = blockFor(ctx, pos);
      auto const range = ctx.block_ranges[b];

      if (pos % 2 == 0) {
        // even position requiring a copy must be on edge
        assertx(range.start == pos);
      } else {
        // odd position
        assertx(pos > range.start); // implicit label position per block
        if (pos + 1 == range.end) continue; // copy belongs on successor edge

        assertx(!resolution.copies[pos][i2->reg]);
        resolution.copies[pos][i2->reg] = i1;
      }
    }
  }
}

/*
 * Lower copyargs{} and copy{} into moveplans at the same position.
 */
void lowerCopies(Vunit& unit, const VxlsContext& ctx,
                 const jit::vector<Variable*>& variables,
                 ResolutionPlan& resolution) {
  // Add a lifetime-resolving copy from `s' to `d'---without touching the
  // instruction stream.
  auto const lower = [&] (unsigned pos, Vreg s, Vreg d) {
    auto v1 = variables[s];
    auto v2 = variables[d];
    assertx(v1 && v2);
    assertx(v2->fixed() || v2->def_pos == pos); // ssa

    auto i1 = v1->fixed() ? v1->ivl() : v1->ivlAtUse(pos);
    auto i2 = v2->ivl();

    if (i2->reg != i1->reg) {
      assertx(!resolution.copies[pos][i2->reg]);
      resolution.copies[pos][i2->reg] = i1;
    }
  };

  for (auto b : ctx.blocks) {
    auto pos = ctx.block_ranges[b].start;

    for (auto& inst : unit.blocks[b].code) {
      if (inst.op == Vinstr::copyargs) {
        auto const& uses = unit.tuples[inst.copyargs_.s];
        auto const& defs = unit.tuples[inst.copyargs_.d];
        for (unsigned i = 0, n = uses.size(); i < n; ++i) {
          lower(pos, uses[i], defs[i]);
        }
        inst = nop{};
      } else if (inst.op == Vinstr::copy2) {
        lower(pos, inst.copy2_.s0, inst.copy2_.d0);
        lower(pos, inst.copy2_.s1, inst.copy2_.d1);
        inst = nop{};
      } else if (inst.op == Vinstr::copy) {
        lower(pos, inst.copy_.s, inst.copy_.d);
        inst = nop{};
      }
      pos += 2;
    }
  }
}

/*
 * Search for the phidef in block `b', then return its dest tuple.
 */
Vtuple findPhiDefs(const Vunit& unit, Vlabel b) {
  assertx(!unit.blocks[b].code.empty() &&
          unit.blocks[b].code.front().op == Vinstr::phidef);
  return unit.blocks[b].code.front().phidef_.defs;
}

/*
 * Register copy resolutions for livein sets and phis.
 */
void resolveEdges(Vunit& unit, const VxlsContext& ctx,
                  const jit::vector<Variable*>& variables,
                  ResolutionPlan& resolution) {
  auto const addPhiEdgeCopies = [&] (Vlabel block, Vlabel target,
                                     uint32_t targetIndex,
                                     const VregList& uses) {
    auto const p1 = ctx.block_ranges[block].end - 2;
    auto const& defs = unit.tuples[findPhiDefs(unit, target)];

    for (unsigned i = 0, n = uses.size(); i < n; ++i) {
      auto v1 = variables[uses[i]];
      auto v2 = variables[defs[i]];
      assertx(v1 && v2);

      auto i1 = v1->fixed() ? v1->ivl() : v1->ivlAtUse(p1);
      auto i2 = v2->ivl();

      if (i2->reg != i1->reg) {
        EdgeKey edge { block, targetIndex };
        assertx(!resolution.edge_copies[edge][i2->reg]);
        resolution.edge_copies[edge][i2->reg] = i1;
      }
    }
  };

  for (auto b1 : ctx.blocks) {
    auto const p1 = ctx.block_ranges[b1].end - 2;
    auto& block1 = unit.blocks[b1];
    auto& inst1 = block1.code.back();

    // Add resolutions for phis.
    if (inst1.op == Vinstr::phijmp) {
      auto const& phijmp = inst1.phijmp_;
      auto const target = phijmp.target;
      auto const& uses = unit.tuples[phijmp.uses];
      addPhiEdgeCopies(b1, target, 0, uses);
      inst1 = jmp{target};
    }

    auto const succlist = succs(block1);

    // Add resolutions for livein sets.
    for (unsigned i = 0, n = succlist.size(); i < n; i++) {
      auto const b2 = succlist[i];
      auto const p2 = ctx.block_ranges[b2].start;

      forEach(ctx.livein[b2], [&] (Vreg vr) {
        auto var = variables[vr];
        if (var->fixed()) return;
        Interval* i1 = nullptr;
        Interval* i2 = nullptr;

        for (auto ivl = var->ivl(); ivl && !(i1 && i2); ivl = ivl->next) {
          if (ivl->covers(p1)) i1 = ivl;
          if (ivl->covers(p2)) i2 = ivl;
        }

        // i2 can be unallocated if the tmp is a constant or is spilled.
        if (i2->reg != InvalidReg && i2->reg != i1->reg) {
          assertx((resolution.edge_copies[{b1,i}][i2->reg] == nullptr));
          resolution.edge_copies[{b1,i}][i2->reg] = i1;
        }
      });
    }
  }
}

/*
 * Walk through the variables list and account for all points where copies or
 * spills need to be made.
 */
ResolutionPlan resolveLifetimes(Vunit& unit, const VxlsContext& ctx,
                                const jit::vector<Variable*>& variables) {
  ResolutionPlan resolution;

  resolveSplits(ctx, variables, resolution);
  lowerCopies(unit, ctx, variables, resolution);
  resolveEdges(unit, ctx, variables, resolution);

  return resolution;
}

///////////////////////////////////////////////////////////////////////////////
// Operand renaming.

/*
 * Visitor class for renaming registers.
 */
struct Renamer {
  Renamer(const jit::vector<Variable*>& variables, unsigned pos)
    : variables(variables)
    , pos(pos)
  {}

  template <class T>
  void imm(const T& /*r*/) {}
  template<class R> void def(R& r) { rename(r); }
  template<class D, class H> void defHint(D& dst, H) { rename(dst); }
  template<class R> void use(R& r) { rename(r); }
  template<class S, class H> void useHint(S& src, H) { rename(src); }
  template<class R> void across(R& r) { rename(r); }
  void across(Vptr& m) {
    if (m.base.isValid()) rename(m.base);
    if (m.index.isValid()) rename(m.index);
  }
  template<Width w> void across(Vp<w>& m) { across(static_cast<Vptr&>(m)); }

  void def(RegSet) {}
  void use(RegSet /*r*/) {}
  void use(Vptr& m) {
    if (m.base.isValid()) rename(m.base);
    if (m.index.isValid()) rename(m.index);
  }
  template<Width w> void use(Vp<w>& m) { use(static_cast<Vptr&>(m)); }

  void use(VcallArgsId) { always_assert(false && "vcall unsupported in vxls"); }

private:
  void rename(Vreg8& r) { r = lookup(r, Constraint::Gpr); }
  void rename(Vreg16& r) { r = lookup(r, Constraint::Gpr); }
  void rename(Vreg32& r) { r = lookup(r, Constraint::Gpr); }
  void rename(Vreg64& r) { r = lookup(r, Constraint::Gpr); }
  void rename(VregDbl& r) { r = lookup(r, Constraint::Simd); }
  void rename(Vreg128& r) { r = lookup(r, Constraint::Simd); }
  void rename(VregSF& r) { r = RegSF{0}; }
  void rename(Vreg& r) { r = lookup(r, Constraint::Any); }
  void rename(Vtuple /*t*/) { /* phijmp+phidef handled by resolveEdges */
  }

  PhysReg lookup(Vreg vreg, Constraint kind) {
    auto var = variables[vreg];
    if (!var || vreg.isPhys()) return vreg;
    PhysReg reg = var->ivlAtUse(pos)->reg;
    assertx((kind == Constraint::Gpr && reg.isGP()) ||
            (kind == Constraint::Simd && reg.isSIMD()) ||
            (kind == Constraint::Sf && reg.isSF()) ||
            (kind == Constraint::Any && reg != InvalidReg));
    return reg;
  }
private:
  const jit::vector<Variable*>& variables;
  unsigned pos;
};

/*
 * Visit every virtual-register typed operand in `unit', and rename it to its
 * assigned physical register.
 */
void renameOperands(Vunit& unit, const VxlsContext& ctx,
                    const jit::vector<Variable*>& variables) {
  for (auto b : ctx.blocks) {
    auto pos = ctx.block_ranges[b].start;
    for (auto& inst : unit.blocks[b].code) {
      Renamer renamer(variables, pos);
      visitOperands(inst, renamer);
      pos += 2;
    }
  }
  ONTRACE(
    kVasmRegAllocDetailLevel,
    printVariables("after renaming operands", unit, ctx, variables);
  );
}

///////////////////////////////////////////////////////////////////////////////
// Copy insertion.

/*
 * Insert stores for `spills' (with spill space starting at `slots') into
 * `code' before code[j], corresponding to XLS logical position `pos'.
 *
 * Updates `j' to refer to the same instruction after the code insertions.
 */
void insertSpillsAt(jit::vector<Vinstr>& code, unsigned& j,
                    const CopyPlan& spills, Optional<MemoryRef> slots,
                    unsigned pos) {
  jit::vector<Vinstr> stores;

  for (auto src : spills) {
    auto ivl = spills[src];
    if (!ivl) continue;

    always_assert_flog(slots, "Spilling before native sp is set (pos {})",
                       pos);
    auto slot = ivl->var->slot;
    assertx(slot >= 0 && src == ivl->reg);
    MemoryRef ptr{slots->r + slotOffset(slot)};

    if (!ivl->var->wide) {
      always_assert_flog(!src.isSF(), "Tried to spill %flags");
      stores.emplace_back(store{src, ptr});
    } else {
      assertx(src.isSIMD());
      stores.emplace_back(storeups{src, ptr});
    }
  }
  insertCodeAt(code, j, stores, pos);
}

/*
 * Insert reg-reg moves for `copies' into `code' before code[j], corresponding
 * to XLS logical position `pos'.
 *
 * Updates `j' to refer to the same instruction after the code insertions.
 */
void insertCopiesAt(const VxlsContext& ctx,
                    jit::vector<Vinstr>& code, unsigned& j,
                    const CopyPlan& plan, unsigned pos) {
  MovePlan moves;
  jit::vector<Vinstr> copies;

  for_each_copy(plan, [&] (PhysReg dst, const Interval* ivl) {
    moves[dst] = ivl->reg;
  });
  auto const hows = doRegMoves(moves, ctx.tmp);

  for (auto const& how : hows) {
    if (how.m_kind == MoveInfo::Kind::Xchg) {
      copies.emplace_back(copy2{how.m_src, how.m_dst, how.m_dst, how.m_src});
    } else {
      copies.emplace_back(copy{how.m_src, how.m_dst});
    }
  }
  insertCodeAt(code, j, copies, pos);
}

/*
 * Insert constant loads or loads from spill space---with spill space starting
 * at `slots'---for `loads' into `code' before code[j], corresponding to XLS
 * logical position `pos'.
 *
 * Updates `j' to refer to the same instruction after the code insertions.
 */
void insertLoadsAt(jit::vector<Vinstr>& code, unsigned& j,
                   const CopyPlan& plan, Optional<MemoryRef> slots,
                   unsigned pos) {
  jit::vector<Vinstr> loads;

  for_each_load(plan, [&] (PhysReg dst, const Interval* ivl) {
    if (ivl->constant()) {
      if (ivl->var->val.isUndef) return;
      loads.push_back([&]() -> Vinstr {
        switch (ivl->var->val.kind) {
          case Vconst::Quad:
          case Vconst::Double:
            return ldimmq{uint64_t(ivl->var->val.val), dst};
          case Vconst::Long:
            return ldimml{int32_t(ivl->var->val.val), dst};
          case Vconst::Byte:
            return ldimmb{uint8_t(ivl->var->val.val), dst};
        }
        not_reached();
      }());
    } else if (ivl->spilled()) {
      always_assert_flog(slots, "Reloading before native sp is set (pos {})",
                         pos);
      MemoryRef ptr{slots->r + slotOffset(ivl->var->slot)};
      if (!ivl->var->wide) {
        loads.emplace_back(load{ptr, dst});
      } else {
        assertx(dst.isSIMD());
        loads.emplace_back(loadups{ptr, dst});
      }
    }
  });
  insertCodeAt(code, j, loads, pos);
}

/*
 * Mutate the Vinstr stream by inserting copies.
 *
 * This destroys the position numbering, so we can't use interval positions
 * after this.
 */
void insertCopies(Vunit& unit, const VxlsContext& ctx,
                  const jit::vector<Variable*>& /*variables*/,
                  const ResolutionPlan& resolution) {
  auto const getSlots = [&] (Optional<int> offset) {
      Optional<MemoryRef> slots;
      if (offset) slots = ctx.sp[*offset];
      return slots;
  };

  // Insert copies inside blocks.
  for (auto const b : ctx.blocks) {
    auto& block = unit.blocks[b];
    auto& code = block.code;
    auto pos = ctx.block_ranges[b].start;
    auto offset = ctx.spill_offsets[b];

    for (unsigned j = 0; j < code.size(); j++, pos += 2) {
      auto const slots = getSlots(offset);

      // Spills, reg-reg moves, and loads of constant values or spill space all
      // occur between instruction.  Insert them in order.
      auto s = resolution.spills.find(pos - 1);
      if (s != resolution.spills.end()) {
        insertSpillsAt(code, j, s->second, slots, pos - 1);
      }
      auto c = resolution.copies.find(pos - 1);
      if (c != resolution.copies.end()) {
        insertCopiesAt(ctx, code, j, c->second, pos - 1);
        insertLoadsAt(code, j, c->second, slots, pos - 1);
      }

      // Insert copies and loads at instructions.
      c = resolution.copies.find(pos);
      if (c != resolution.copies.end()) {
        insertCopiesAt(ctx, code, j, c->second, pos);
        insertLoadsAt(code, j, c->second, slots, pos);
      }
      assertx(!resolution.spills.contains(pos));
      if (code[j].op == Vinstr::recordbasenativesp) {
        assert_flog(!offset, "Block B{} Instr {} initiailizes native SP, but "
                    "already initialized.", size_t(b), j);
        offset = 0;
      } else if (code[j].op == Vinstr::unrecordbasenativesp) {
        assert_flog(offset, "Block B{} Instr {} uninitiailizes native SP, but "
                    "already uninitialized.", size_t(b), j);
        assert_flog(*offset == 0, "Block B{} Instr {} uninitiailizes native "
                    "SP, but SP offset is non zero.", size_t(b), j);
        offset = std::nullopt;
      } else if (offset) {
        *offset -= spEffect(unit, code[j], ctx.sp);
      }
    }
  }

  // insert copies on edges
  for (auto const b : ctx.blocks) {
    auto& block = unit.blocks[b];
    auto succlist = succs(block);

    if (succlist.size() == 1) {
      // copies will go at end of b
      auto const c = resolution.edge_copies.find({b, 0});
      if (c != resolution.edge_copies.end()) {
        auto& code = block.code;
        unsigned j = code.size() - 1;
        auto const pos = ctx.block_ranges[b].end - 1;
        auto const offset = ctx.spill_offsets[succlist[0]];
        auto const slots = getSlots(offset);

        // We interleave copies and loads in `edge_copies', so here and below
        // we process them separately (and pass `true' to avoid asserting).
        insertCopiesAt(ctx, code, j, c->second, pos);
        insertLoadsAt(code, j, c->second, slots, pos);
      }
    } else {
      // copies will go at start of successor
      for (int i = 0, n = succlist.size(); i < n; i++) {
        auto s = succlist[i];
        auto const c = resolution.edge_copies.find({b, i});
        if (c != resolution.edge_copies.end()) {
          auto& code = unit.blocks[s].code;
          unsigned j = 0;
          auto const pos = ctx.block_ranges[s].start;
          auto const offset = ctx.spill_offsets[s];
          auto const slots = getSlots(offset);

          insertCopiesAt(ctx, code, j, c->second, pos);
          insertLoadsAt(code, j, c->second, slots, pos);
        }
      }
    }
  }
}

///////////////////////////////////////////////////////////////////////////////

/*
 * Peephole cleanup pass.
 *
 * Remove no-op copy sequences before allocating spill space, since doing so
 * might modify the CFG.
 */
void peephole(Vunit& unit, const VxlsContext& ctx) {
  // Whether a Vinstr is a register swap.
  auto const match_xchg = [] (Vinstr& i, Vreg& r0, Vreg& r1) {
    if (i.op != Vinstr::copy2) return false;
    r0 = i.copy2_.s0;
    r1 = i.copy2_.s1;
    return r0 == i.copy2_.d1 && r1 == i.copy2_.d0;
  };

  for (auto b : ctx.blocks) {
    auto& code = unit.blocks[b].code;
    for (int i = 0, n = code.size(); i + 1 < n; i++) {
      Vreg r0, r1, r2, r3;
      if (match_xchg(code[i], r0, r1) &&
          match_xchg(code[i + 1], r2, r3) &&
          ((r0 == r2 && r1 == r3) || (r0 == r3 && r1 == r2))) {
        // matched xchg+xchg that cancel each other
        code[i] = nop{};
        code[i + 1] = nop{};
        i++;
      }
    }
    auto end = std::remove_if(code.begin(), code.end(), [&](Vinstr& inst) {
      return is_trivial_nop(inst) ||
             inst.op == Vinstr::phidef; // we lowered it
    });
    code.erase(end, code.end());
  }
}

///////////////////////////////////////////////////////////////////////////////
// Spill space allocation.

/*
 * SpillState is used by allocateSpillSpace() to decide where to allocate/free
 * spill space. It represents the state of the spill space as a whole and is
 * computed before each individual instruction.
 *
 * Order is important in this enum: it's only legal to transition to states
 * with higher values, and states are merged using std::max().
 */
enum SpillState : uint8_t {
  // State is uninitialized. All block in-states start here.
  Uninit,

  // Spill space is not currently possible; we must allocate spill space after
  // this point.
  NoSpillPossible,

  // Spill space is not currently needed; it's safe to allocate spill space
  // after this point.
  NoSpill,

  // Spill space is needed and must be allocated at or before this point.
  NeedSpill,
};

/*
 * SpillStates is used to hold in/out state for each block after the analysis
 * pass of allocateSpillSpace().
 */
struct SpillStates {
  SpillState in;
  SpillState out;
  bool changes;
  bool hasIndirectFixup;
};

/*
 * Returns true if spill space must be allocated before execution of this
 * instruction. In order to keep things simple, we return true for any
 * instruction that reads or writes sp.
 */
bool instrNeedsSpill(const Vunit& unit, const Vinstr& inst, PhysReg sp) {
  // Implicit sp input/output.
  if (spEffect(unit, inst, sp, false) != 0) return true;

  auto foundSp = false;
  visitDefs(unit, inst, [&] (Vreg r) { if (r == sp) foundSp = true; });
  if (foundSp) return true;

  visitUses(unit, inst, [&] (Vreg r) { if (r == sp) foundSp = true; });
  return foundSp;
}

/*
 * Return the required SpillState coming into inst. prevState must not be
 * Uninit.
 */
SpillState instrInState(const Vunit& unit, const Vinstr& inst,
                        SpillState prevState, PhysReg sp) {
  switch (prevState) {
    case Uninit: break;

    case NoSpillPossible:
      if (inst.op == Vinstr::recordbasenativesp) return NoSpill;
      return NoSpillPossible;

    case NoSpill:
      if (inst.op == Vinstr::unrecordbasenativesp) return NoSpillPossible;
      if (instrNeedsSpill(unit, inst, sp)) return NeedSpill;
      return NoSpill;

    case NeedSpill:
      if (inst.op == Vinstr::unrecordbasenativesp) return NoSpillPossible;
      return NeedSpill;
  }

  always_assert(false);
}

/*
 * Merge src into dst, returning true iff dst was changed.
 */
bool mergeSpillStates(SpillState& dst, SpillState src) {
  assertx(src != Uninit);
  if (dst == src) return false;

  // The only permitted merges result in a state with higher values.  This
  // holds because we can't merge a NoSpillPossible with a non NoSpillPossible
  // since that would mean a program point both can and can't have spills.
  assertx(IMPLIES(src != NoSpillPossible, dst != NoSpillPossible));
  auto const oldDst = dst;
  dst = std::max(dst, src);
  return dst != oldDst;
}

std::default_random_engine s_stressRand(0xfaceb00c);
std::uniform_int_distribution<int> s_stressDist(1,7);

/*
 * If the current unit used any spill slots, allocate and free spill
 * space where appropriate. Spill space is allocated right before it's
 * needed and freed before any instruction that exits the unit, which
 * is any block-ending instruction with no successors in the unit. The
 * algorithm uses two passes:
 *
 * Analysis:
 *   - For each block in RPO:
 *     - Load in-state, which has been populated by at least one predecessor
 *       (or manually set to NoSpillPossible for the entry block).
 *     - Analyze each instruction in the block, determining what state the
 *       spill space must be in before executing it.
 *     - Record out-state for the block and propagate to successors. If this
 *       changes the in-state for any of them, enqueue them for (re)processing.
 *
 * Mutation:
 *   - For each block (we use RPO to only visit reachable blocks but order
 *     doesn't matter):
 *     - Inspect the block's in-state and out-state:
 *       - NoSpill in: Walk the block to see if we need to allocate spill space
 *         before any instructions.
 *       - NoSpill out: Allocate spill space on any edges to successors with
 *         NeedSpill in-states.  Also walk block and check for deallocation of
 *         spill space.
 *       - NeedSpill out: If the block has no in-unit successors, free spill
 *         space before the block-end instruction.
 */
void allocateSpillSpace(Vunit& unit, const VxlsContext& ctx,
                        SpillInfo& spi) {
  if (spi.used_spill_slots == 0) return;
  Timer t(Timer::vasm_reg_alloc_spill, unit.log_entry);

  // Make sure we always allocate spill space in multiples of 16 bytes, to keep
  // alignment straightforward.
  if (spi.used_spill_slots % 2) spi.used_spill_slots++;
  FTRACE(1, "Allocating {} spill slots\n", spi.used_spill_slots);

  auto const spillSize = safe_cast<int32_t>(slotOffset(spi.used_spill_slots));
  // Pointer manipulation is traditionally done with lea, and it's safe to
  // insert even where flags might be live.
  Vinstr alloc = lea{ctx.sp[-spillSize], ctx.sp};
  Vinstr free  = lea{ctx.sp[spillSize], ctx.sp};

  jit::vector<uint32_t> rpoIds(unit.blocks.size());
  for (uint32_t i = 0; i < ctx.blocks.size(); ++i) rpoIds[ctx.blocks[i]] = i;

  jit::vector<SpillStates> states(unit.blocks.size(),
                                  {Uninit, Uninit, false, false});
  states[unit.entry].in = NoSpillPossible;
  dataflow_worklist<uint32_t> worklist(unit.blocks.size());
  worklist.push(0);

  // Walk the blocks in rpo. At the end of each block, propagate its out-state
  // to successors, adding them to the worklist if their in-state
  // changes. Blocks may be visited multiple times if loops are present.
  while (!worklist.empty()) {
    auto const label  = ctx.blocks[worklist.pop()];
    auto const& block = unit.blocks[label];
    auto const stateIn = states[label].in;
    auto state = stateIn;

    for (auto& inst : block.code) {
      state = instrInState(unit, inst, state, ctx.sp);
      if (state != stateIn) states[label].changes = true;
      if (instrHasIndirectFixup(inst)) states[label].hasIndirectFixup = true;
    }
    states[label].out = state;

    for (auto s : succs(block)) {
      if (mergeSpillStates(states[s].in, state)) {
        worklist.push(rpoIds[s]);
      }
    }
  }

  // Do a single mutation pass over the blocks.
  for (auto const label : ctx.blocks) {
    auto state = states[label];
    auto& block = unit.blocks[label];

    if (state.hasIndirectFixup && isPrologue(unit.context->kind)) {
      auto curState = state.in;
      for (auto it = block.code.begin(); it != block.code.end(); ++it) {
        // Note that if the instruction at the start or end of the spill
        // regions has fixup, this loop does not account for it.
        // This is not ideal but currently there are no instructions that
        // have fixups that can start/end spill regions, so it is fine.
        curState = instrInState(unit, *it, curState, ctx.sp);
        if (curState == NeedSpill && instrHasIndirectFixup(*it)) {
          updateIndirectFixupBySpill(*it, spillSize);
        }
      }
    }

    // Any block with a state change should be walked to check for allocation
    // or free of spill space.
    if (state.changes) {
      auto curState = state.in;
      for (auto it = block.code.begin(); it != block.code.end(); ++it) {
        auto const prevState = curState;
        curState = instrInState(unit, *it, curState, ctx.sp);
        if (curState == prevState) continue;
        if (prevState != NeedSpill && curState == NeedSpill) {
          assertx(prevState != NoSpillPossible);
          FTRACE(3, "alloc spill before {}: {}\n", label, show(unit, *it));
          alloc.set_irctx(it->irctx());
          it = block.code.insert(it, alloc);
          ++it;
        } else if (prevState == NeedSpill &&
                   curState == NoSpillPossible) {
          FTRACE(3, "free spill before {}: {}\n", label, show(unit, *it));
          free.set_irctx(it->irctx());
          it = block.code.insert(it, free);
          ++it;
        }
      }
    }

    // Allocate spill space on edges from a NoSpill out-state to a NeedSpill
    // in-state.
    auto const successors = succs(block);
    for (auto s : successors) {
      if (state.out != states[s].in) {
        assertx(state.out != NoSpillPossible && states[s].in != NoSpillPossible);
        auto const shouldAlloc = state.out == NoSpill;
        auto& op =  shouldAlloc ? alloc : free;
        FTRACE(3, "{} spill on edge from {} -> {}\n",
               shouldAlloc ? "alloc" : "free", label, s);
        auto it = std::prev(block.code.end());
        op.set_irctx(it->irctx());
        block.code.insert(it, op);
      }
    }

    // Any block with a NeedSpill out-state and no successors must free spill
    // space right before the block-end instruction. We ignore trap so spill
    // space is still allocated in core files.
    if (state.out == NeedSpill && successors.empty() &&
        block.code.back().op != Vinstr::trap) {
      auto it = std::prev(block.code.end());
      FTRACE(3, "free spill before {}: {}\n", label, show(unit, (*it)));
      free.set_irctx(it->irctx());
      block.code.insert(it, free);
    }
  }
}

///////////////////////////////////////////////////////////////////////////////
// Printing.

std::string Interval::toString() const {
  std::ostringstream out;
  auto delim = "";
  if (reg != InvalidReg) {
    out << show(reg);
    delim = " ";
  }
  if (constant()) {
    out << delim << fmt::format("#{:08x}", var->val.val);
  }
  if (var->slot >= 0) {
    out << delim << fmt::format("[%sp+{}]", slotOffset(var->slot));
  }
  delim = "";
  out << " [";
  for (auto const& r : ranges) {
    out << delim << fmt::format("{}-{}", r.start, r.end);
    delim = ",";
  }
  out << ") {";
  delim = "";
  for (auto const& u : uses) {
    if (u.pos == var->def_pos) {
      if (u.hint.isValid()) {
        out << delim << "@" << u.pos << "=" << show(u.hint);
      } else {
        out << delim << "@" << u.pos << "=";
      }
    } else {
      auto hint_delim = u.kind == Constraint::CopySrc ? "=?" : "=@";
      if (u.hint.isValid()) {
        out << delim << show(u.hint) << hint_delim << u.pos;
      } else {
        out << delim << hint_delim << u.pos;
      }
    }
    delim = ",";
  }
  out << "}";
  return out.str();
}

DEBUG_ONLY void dumpVariables(const jit::vector<Variable*>& variables,
                              unsigned num_spills) {
  Trace::traceRelease("Spills %u\n", num_spills);
  for (auto var : variables) {
    if (!var || var->fixed()) continue;
    Trace::traceRelease("%%%-2lu %s\n", size_t(var->vreg),
                        var->ivl()->toString().c_str());
    for (auto ivl = var->ivl()->next; ivl; ivl = ivl->next) {
      Trace::traceRelease("    %s\n", ivl->toString().c_str());
    }
  }
}

auto const ignore_reserved = !getenv("XLS_SHOW_RESERVED");
auto const collapse_fixed = !getenv("XLS_SHOW_FIXED");

enum Mode { Light, Heavy };

template<class Pred>
const char* draw(Variable* var, unsigned pos, Mode m, Pred covers) {
                                  // Light     Heavy
  static const char* top[]    = { (const char*)u8"\u2575", (const char*)u8"\u2579" };
  static const char* bottom[] = { (const char*)u8"\u2577", (const char*)u8"\u257B" };
  static const char* both[]   = { (const char*)u8"\u2502", (const char*)u8"\u2503" };
  static const char* empty[]  = { " ", " " };
  auto f = [&](unsigned position) {
    if (!var) return false;
    for (auto ivl = var->ivl(); ivl; ivl = ivl->next) {
      if (covers(ivl, position)) return true;
    }
    return false;
  };

  auto s = f(pos);
  auto d = pos % 2 == 1 ? s : f(pos + 1);
  return ( s && !d) ? top[m] :
         ( s &&  d) ? both[m] :
         (!s &&  d) ? bottom[m] :
         empty[m];
}

DEBUG_ONLY void printInstr(std::ostringstream& str,
                           const Vunit& unit, const VxlsContext& ctx,
                           const jit::vector<Variable*>& variables,
                           const Vinstr& inst, Vlabel b) {
  bool fixed_covers[2] = { false, false };
  Variable* fixed = nullptr;
  for (auto var : variables) {
    if (!var) continue;
    if (var->fixed()) {
      if (ignore_reserved && !ctx.abi.unreserved().contains(var->vreg)) {
        continue;
      }
      if (collapse_fixed) {
        fixed = var; // can be any.
        fixed_covers[0] |= var->ivl()->covers(inst.id);
        fixed_covers[1] |= var->ivl()->covers(inst.id + 1);
        continue;
      }
    }
    str << " ";
    str << draw(var, inst.id, Light, [&](Interval* child, unsigned p) {
      return child->covers(p);
    });
    str << draw(var, inst.id, Heavy, [&](Interval* child, unsigned p) {
      return child->usedAt(p);
    });
  }
  str << " " << draw(fixed, inst.id, Heavy, [&](Interval*, unsigned p) {
    assertx(p - inst.id < 2);
    return fixed_covers[p - inst.id];
  });
  if (inst.id == ctx.block_ranges[b].start) {
    str << fmt::format(" B{: <3}", size_t(b));
  } else {
    str << "     ";
  }
  str << fmt::format(" {: <3} ", inst.id) << show(unit, inst) << "\n";
}

DEBUG_ONLY void printVariables(const char* caption,
                               const Vunit& unit, const VxlsContext& ctx,
                               const jit::vector<Variable*>& variables) {
  std::ostringstream str;
  str << "Intervals " << caption << " " << ctx.counter << "\n";
  for (auto var : variables) {
    if (!var) continue;
    if (var->fixed()) {
      if (ignore_reserved && !ctx.abi.unreserved().contains(var->vreg)) {
        continue;
      }
      if (collapse_fixed) {
        continue;
      }
    }
    str << fmt::format(" {: <2}", size_t(var->vreg));
  }
  str << " FX\n";
  for (auto b : ctx.blocks) {
    for (auto& inst : unit.blocks[b].code) {
      printInstr(str, unit, ctx, variables, inst, b);
    }
  }
  HPHP::Trace::traceRelease("%s\n", str.str().c_str());
}

///////////////////////////////////////////////////////////////////////////////

struct XLSStats {
  size_t instrs{0}; // total instruction count after xls
  size_t moves{0}; // number of movs inserted
  size_t loads{0}; // number of loads inserted
  size_t spills{0}; // number of spill-stores inserted
  size_t consts{0}; // number of const-loads inserted
  size_t total_copies{0}; // moves+loads+spills
};

void dumpStats(const Vunit& unit, const ResolutionPlan& resolution) {
  XLSStats stats;

  for (auto const& blk : unit.blocks) {
    stats.instrs += blk.code.size();
  }
  for (auto const& kv : resolution.spills) {
    auto const& spills = kv.second;
    for (auto const r : spills) {
      if (spills[r]) ++stats.spills;
    }
  }

  auto const count_copies = [&] (const CopyPlan& plan) {
    for_each_copy(plan, [&] (PhysReg, const Interval*) {
      ++stats.moves;
    });
    for_each_load(plan, [&] (PhysReg, const Interval* ivl) {
      ++(ivl->spilled() ? stats.loads : stats.consts);
    });
  };
  for (auto const& kv : resolution.copies) count_copies(kv.second);
  for (auto const& kv : resolution.edge_copies) count_copies(kv.second);

  stats.total_copies = stats.moves + stats.loads + stats.spills;

  FTRACE_MOD(
    Trace::xls_stats, 1,
    "XLS stats:\n"
    "  instrs: {}\n"
    "  moves:  {}\n"
    "  loads:  {}\n"
    "  spills: {}\n"
    "  consts: {}\n"
    "  total copies: {}\n",
    stats.instrs, stats.moves, stats.loads, stats.spills,
    stats.consts, stats.total_copies
  );

  if (auto entry = unit.log_entry) {
    entry->setInt("xls_instrs", stats.instrs);
    entry->setInt("xls_moves", stats.moves);
    entry->setInt("xls_loads", stats.loads);
    entry->setInt("xls_spills", stats.spills);
    entry->setInt("xls_consts", stats.consts);
  }
}

///////////////////////////////////////////////////////////////////////////////
}

void allocateRegistersWithXLS(Vunit& unit, const Abi& abi) {
  Timer timer(Timer::vasm_reg_alloc, unit.log_entry);
  auto const counter = s_counter.fetch_add(1, std::memory_order_acq_rel);

  assertx(check(unit));
  assertx(checkNoCriticalEdges(unit));
  assertx(checkNoSideExits(unit));

  // Analysis passes.
  VxlsContext ctx{abi};
  ctx.blocks = sortBlocks(unit);
  ctx.block_ranges = computePositions(unit, ctx.blocks);
  ctx.spill_offsets = analyzeSP(unit, ctx.blocks, ctx.sp);
  ctx.livein = computeLiveness(unit, ctx.abi, ctx.blocks);
  ctx.counter = counter;

  // Build lifetime intervals and analyze hints.
  auto variables = buildIntervals(unit, ctx);
  auto const hint_info = analyzeHints(unit, ctx, variables);

  // Perform register allocation.
  auto spill_info = assignRegisters(ctx, variables, hint_info);

  ONTRACE(kVasmRegAllocDetailLevel,
          dumpVariables(variables, spill_info.num_spills));

  // Insert lifetime-resolving copies, spills, and rematerializations, and
  // replace the Vreg operands in the Vinstr stream with the assigned PhysRegs.
  auto const resolution = resolveLifetimes(unit, ctx, variables);
  renameOperands(unit, ctx, variables);
  insertCopies(unit, ctx, variables, resolution);

  ONTRACE(kVasmRegAllocDetailLevel,
    dumpVariables(variables, spill_info.num_spills);
    printVariables("after inserting copies", unit, ctx, variables);
  );

  peephole(unit, ctx);

  // Insert instructions for creating spill space.
  allocateSpillSpace(unit, ctx, spill_info);
  if (auto entry = unit.log_entry) {
    entry->setInt("num_spills", spill_info.num_spills);
    entry->setInt("used_spill_slots", spill_info.used_spill_slots);
  }

  printUnit(kVasmRegAllocLevel, "after vasm-xls", unit);
  if (HPHP::Trace::moduleEnabled(Trace::xls_stats, 1) || unit.log_entry) {
    dumpStats(unit, resolution);
  }

  // Free the variable metadata.
  for (auto var : variables) {
    if (!var) continue;
    for (Interval* ivl = var->ivl()->next, *next = nullptr; ivl; ivl = next) {
      next = ivl->next;
      jit::destroy(ivl);
    }
    Variable::destroy(var);
  }
}

///////////////////////////////////////////////////////////////////////////////
}
