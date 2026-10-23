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

#include "hphp/runtime/vm/jit/abi.h"
#include "hphp/runtime/vm/jit/memory-effects.h"
#include "hphp/runtime/vm/jit/timer.h"
#include "hphp/runtime/vm/jit/vasm.h"
#include "hphp/runtime/vm/jit/vasm-info.h"
#include "hphp/runtime/vm/jit/vasm-print.h"
#include "hphp/runtime/vm/jit/vasm-unit.h"
#include "hphp/runtime/vm/jit/vasm-util.h"

#include "hphp/util/copy-ptr.h"
#include "hphp/util/dataflow-worklist.h"
#include "hphp/util/match.h"
#include "hphp/util/non-invalidating-vector.h"
#include "hphp/util/optional.h"
#include "hphp/util/tribool.h"

#include <boost/range/adaptor/reversed.hpp>
#include <fmt/format.h>

TRACE_SET_MOD(vasm_graph_color)

namespace HPHP::jit {

//////////////////////////////////////////////////////////////////////

/*
 * Register allocation using SSA graph-coloring:
 *
 * This register allocator is based on the theory of graph coloring on SSA
 * programs. Programs in strict SSA form have the unique property that their
 * interference graphs are chordal. This implies that the chromatic number of
 * the graph is equal to the size of its largest clique. This means that the
 * points at which spilling is required can be calculated easily (separate from
 * the coloring decisions), and once those spills are inserted, the program can
 * be colored trivially.
 *
 * This is more efficient than traditional graph-coloring. The interference
 * graph never needs to be materialized and we never need to "restart" from the
 * beginning. Moreover, spilling and coloring are divorced and handled in their
 * own separate passes. This makes for a more modular and easier to understand
 * algorithm. It does, however, require the unit to be completely in SSA form,
 * which adds overhead.
 *
 * The allocation is done in the following (broad) steps:
 *
 * - The unit is prepared by materializing any constants into actual Vregs.
 *
 * - Register classes are inferred. This involves analysing the uses and defs of
 *   each Vreg to determine which register class they should be assigned to (GP
 *   or SIMD). The algorithm requires every register to be in a single register
 *   class and there can be no mixing of the two).
 *
 * - Spills and reloads are inserted. The unit is walked, keeping track of all
 *   live Vregs. At any point where the number of live Vregs exceeds the number
 *   of physical registers, spills are inserted to bring the number of live
 *   Vregs down. A reload is inserted where-ever an instruction uses a Vreg
 *   which is currently spilled.
 *
 * - Colors are assigned. Now that spills and reloads have been inserted, it's
 *   guaranteed that every Vreg will always have at least one color available
 *   for it. Walk the unit and assign registers for every Vreg using a
 *   heuristic to try to minimize copies. A Vreg can be in different physical
 *   registers at different points in the unit. Spill slots are assigned
 *   using a separate algorithm.
 *
 * - Lower out of SSA. Lower phis into register and spill slot moves. Optimize
 *   away no-op copies. Assign stack pointer adjustments as necessary for spill
 *   slots. At this point the unit no longer has any virtual registers and is
 *   fully register allocated.
 *
 * See the comments for each pass for more detailed information.
 *
 * This allocator is largely built on the ideas in the following:
 *
 * - "Register Allocation for Programs in SSA Form" by Sebastian Hack
 *
 * - "Register Spilling and Live-Range Splitting for SSA-Form Programs" by
 *    Matthias Braun and Sebastian Hack
 *
 * - "Preference-Guided Register Assignment" by Matthias Braun, Christoph
 *    Mallon, and Sebastian Hack (Compiler Construction 2010)
 *
 * - "Revisiting Out-of-SSA Translation for Correctness, Code Quality, and
 *    Efficiency" by Benoit Boissinot, Alain Darte, Fabrice Rastello, Benoit
 *    Dupont de Dinechin, Christophe Guillon
 */

namespace {

//////////////////////////////////////////////////////////////////////
// State

// RegClass determine what set of things a Vreg can be colored to. This includes
// whether the Vreg represents a spill slot, and whether it's a wide value or
// not. It is inferred by looking at how a Vreg is used and defined.
enum RegClass {
  Any,      // Completely unconstrained. This is the default, but is not valid
            // for a used register.
  AnyNarrow,// This means the Vreg can be safely given a GP or SIMD register and
            // is non-wide. However, the graph coloring algorithm cannot handle
            // this kind of constraint (union of two other constraints), so
            // right now it's effectively a synonym for GP. TODO (T37587676) to
            // try to deal with this better.

  GP,       // Generic purpose register
  SIMD,     // SIMD register
  SIMDWide, // 128-bit SIMD register
  SF,       // Singleton flags register
  Spill,    // Spill slot
  SpillWide // 128-bit spill slot
};

// Color is a discriminated union representing an unassigned color, a spill
// slot, or a physical register. Which subset is valid depends on the Vreg's
// RegClass.
struct None {};
struct SpillSlot { size_t slot; };
struct SpillSlotWide { size_t slot; };
using Color = std::variant<None, PhysReg, SpillSlot, SpillSlotWide>;

// Rematerialization info for a particular Vreg
struct RematInfo {
  // The instruction which potentially can rematerialize it. May be a
  // reload to indicate there's no rematerialization available. The
  // instruction may not necessarily be usuable. A context sensitive
  // check is required.
  Vinstr instr;
  // The block where the instruction where the instruction came
  // from. This may not necessarily be where the Vreg was defined, as
  // we chain through copy-ish instructions. This is only meaningful
  // for instructions which have physical register sources.
  Vlabel block;
};

// State about each Vreg. Instead of separate data-structures, all Vreg
// information is concentrated in this one data-structure.
struct RegInfo {
  RegClass regClass = RegClass::Any;
  // Color assigned to this Vreg at this particular moment (during
  // coloring, Vregs can be assigned different colors at different
  // points in the unit). Spills will always keep the same color.
  Color color;
  // Index into the penalty vector table
  size_t penaltyIdx = 0;
  // Can this Vreg be potentially rematerialized (instead of reloaded)
  // by this instruction? This field is calculated lazily and then
  // cached. Even if there's information here, we may still need to do
  // context sensitive checks to see if it's usuable.
  Optional<RematInfo> cachedRemat;
};

using BlockVector = jit::vector<Vlabel>;
using BlockSet = boost::dynamic_bitset<>;
using WeightMap = jit::fast_map<Vreg, uint64_t>;
using PhiWeightVector = jit::vector<Optional<uint64_t>>;
using PenaltyVector = PhysReg::Map<int64_t>;

// Information about each inferred loop. A loop is represented by its header
// block.
struct LoopInfo {
  VregSet uses;            // Vregs used inside
  jit::vector<Vlabel> blocks; // Blocks composing this loop
  size_t depth = 0;        // Nesting depth (this will always be at least one
                           // once the information is initialized).
};

// Cached def/use/across operands for an instruction.
struct CachedOperands {
  CachedOperands() = default;
  CachedOperands(VregSet&& defs, VregSet&& uses, VregSet&& acrosses,
                 RegSet&& physDefs, RegSet&& physUses) noexcept:
    defs{std::move(defs)},
    uses{std::move(uses)},
    acrosses{std::move(acrosses)},
    physDefs{std::move(physDefs)},
    physUses{std::move(physUses)} {}
  VregSet defs;
  VregSet uses;
  VregSet acrosses;
  RegSet physDefs;
  RegSet physUses;
};

using CachedOperandsTable = NonInvalidatingVector<CachedOperands>;

// Global allocator state.
struct State {
  Vunit& unit;
  const Abi& abi;

  // CFG information
  BlockVector rpo;
  jit::vector<size_t> rpoOrder;
  PredVector preds;

  // Registers available for selection by the allocator.
  const RegSet gpUnreserved;
  const RegSet simdUnreserved;
  // Registers not available for selection. These will be left untouched if
  // present.
  const RegSet reservedRegs;
  // SIMD scratch register for resolving shuffles.
  const PhysReg scratch;

  // Liveness information
  jit::vector<VregSet> liveIn;
  jit::vector<VregSet> liveOut;
  jit::vector<VregSet> defs;
  jit::vector<VregSet> uses;
  jit::vector<VregSet> gens;

  // Tracks if the stack pointer has been recorded upon entry to the block.
  // Built during initial calculate_liveness, and used by the spiller to
  // establish if spilling is allowed.
  BlockSet spRecordedIn{};
  BlockSet spRecordedOut{};

  // Whether each block contain an unrecordbasenativesp
  BlockSet hasUnrecordbasenativesp{};

  // All physical registers (including ignored), which have a def in a
  // block
  jit::vector<RegSet> physDefs;
  // If a physical register (including ignored) is in this set for a
  // block, it means that that register is modified on some path from
  // the entry of the unit to the *end* of the block. If not, the
  // register is guaranteed to have the same value it did on entry to
  // the unit.
  jit::vector<RegSet> physChangedBefore;
  // All physical registers (including ignored), which have a def
  // anywhere in the unit
  RegSet physChanged;
  // Cached information for resolving whether a particular physical
  // register has potentially changed in value (in a known way)
  // between a block and where some Vreg is defined.
  jit::fast_map<std::tuple<PhysReg, Vreg, Vlabel>,
                std::pair<Optional<int64_t>, bool>>
  physRecoverableCache;

  // Cached information about the memory locations written to in a
  // particular block. This vector is resized on first use and the
  // per-block information is calculated on demand.
  jit::vector<Optional<AliasClass>> memWrites;

  // Mapping of block/Vreg pairs to whether the Vreg (representing a
  // spilled but potentially rematerializable read) is available to be
  // rematerialized at that block. TriBool::Maybe is used during
  // calculation and is used to mark blocks currently being visited. See
  jit::fast_map<std::pair<Vlabel, Vreg>, TriBool> memReadAvailableCache;

  // Loop information. A loop is represented by its header block.
  jit::fast_map<Vlabel, LoopInfo> loopInfo;
  // Map of block to the inner-most loop it belongs to. Blocks not contained
  // within a loop will not be present.
  jit::fast_map<Vlabel, Vlabel> blockToLoop;
  // Map of a loop to all of the loops it's contained within.
  std::unordered_multimap<Vlabel, Vlabel> loopMembership;

  // Cached operands. The "id" field in a Vinstr is an index into this
  // table. Since 0 means no cached information, the first entry is
  // never used.
  CachedOperandsTable cachedOperands;

  // Calculate penalty vectors. Different Vregs may share the same
  // penalty vector.
  jit::vector<PenaltyVector> penalties;

  // Vreg state
  jit::vector<Optional<RegInfo>> regInfo;

  // All Vregs belonging to particular RegClasses. Used to quickly
  // filter VregSets.
  VregSet gps;
  VregSet simds;
  VregSet flags;

  // Pre-calculated mapping of spill Vregs to spill slots
  jit::fast_map<Vreg, Color> spillColors;

  // The number of non-wide and wide spill slots allocated. This determines how
  // much space to reserve in the stack.
  size_t numSpillSlots = 0;
  size_t numWideSpillSlots = 0;

  // If we spilled anything in this unit
  bool spilled = false;
};

//////////////////////////////////////////////////////////////////////
// Pretty-printers

std::string show(RegClass r) {
  switch (r) {
    case RegClass::Any:       return "any";
    case RegClass::AnyNarrow: return "any-narrow";
    case RegClass::GP:        return "gp";
    case RegClass::SIMD:      return "simd";
    case RegClass::SIMDWide:  return "simd-wide";
    case RegClass::SF:        return "sf";
    case RegClass::Spill:     return "spill";
    case RegClass::SpillWide: return "spill-wide";
  }
  always_assert(false);
}

std::string show(Color c) {
  return match<std::string>(
    c,
    [] (None)            { return "-"; },
    [] (PhysReg r)       { return show(r); },
    [] (SpillSlot s)     { return fmt::format("S{}", s.slot); },
    [] (SpillSlotWide s) { return fmt::format("SW{}", s.slot); }
  );
}

std::string show(const BlockVector& v) {
  using namespace folly::gen;
  return fmt::format(
    "[{}]",
    from(v)
      | map([] (Vlabel b) { return fmt::format("{}", b); })
      | unsplit<std::string>(", ")
  );
}

std::string show(const WeightMap& w) {
  std::vector<std::pair<Vreg, uint64_t>> sorted{w.begin(), w.end()};
  std::sort(sorted.begin(), sorted.end());

  using namespace folly::gen;
  return fmt::format(
    "{{{}}}",
    from(sorted)
      | map([] (const std::pair<Vreg, uint64_t>& p) {
          return fmt::format("{} -> {}", show(p.first), p.second);
        })
      | unsplit<std::string>(", ")
  );
}

std::string show(const PhiWeightVector& v) {
  using namespace folly::gen;
  return fmt::format(
    "[{}]",
    from(v)
      | map([] (const Optional<uint64_t>& w) -> std::string {
          if (!w) return "*";
          return std::to_string(*w);
        })
      | unsplit<std::string>(", ")
  );
}

std::string show(const PenaltyVector& v) {
  std::string out;
  auto first = true;
  for (auto const r : v) {
    if (!first) out += ", ";
    first = false;
    out += fmt::format("{}: {}", show(r), v[r]);
  }
  return fmt::format("[{}]", out);
}

std::string show(const Vunit& unit, const RematInfo& remat) {
  return fmt::format("{} [{}]", show(unit, remat.instr), remat.block);
}

std::string show(const Vunit& unit, const RegInfo& info) {
  return fmt::format(
    "Class: {:10}, Color: {:6}, Penalty: {:3}, Mat: ({})",
    show(info.regClass),
    show(info.color),
    info.penaltyIdx,
    info.cachedRemat ? show(unit, *info.cachedRemat) : "-"
  );
}

std::string show(const LoopInfo& info) {
  using namespace folly::gen;
  return fmt::format(
    "Depth: {:2}, Uses: {}, Blocks: {}",
    info.depth,
    show(info.uses),
    from(info.blocks)
      | map([] (Vlabel b) { return fmt::format("{}", b); })
      | unsplit<std::string>(", ")
  );
}

std::string show(const State& state) {
  auto const dumpBlockInfo = [&] (auto const& info) -> std::string {
    if (info.empty()) return "";
    std::string str;
    for (auto const b : state.rpo) {
      if (b >= info.size() || info[b].empty()) continue;
      str += fmt::format(
        "  {:5} -> {}\n",
        b, show(info[b])
      );
    }
    return str;
  };

  return fmt::format(
    "GP Unreserved:        {}\n"
    "SIMD Unreserved:      {}\n"
    "Reserved Regs:        {}\n"
    "Scratch Reg:          {}\n"
    "Num Spill Slots:      {}\n"
    "Num Wide Spill Slots: {}\n"
    "RPO:                  {}\n"
    "Spill Colors:         {}\n"
    "Phys Changed:         {}\n"
    "Mem-Writes:\n{}"
    "Reg Info:\n{}"
    "Live In:\n{}"
    "Live Out:\n{}"
    "Uses:\n{}"
    "Defs:\n{}"
    "Gens:\n{}"
    "Phys Defs:\n{}"
    "Phys Changed Before:\n{}"
    "Loop Info:\n{}"
    "Penalties:\n{}",
    show(state.gpUnreserved),
    show(state.simdUnreserved),
    show(state.reservedRegs),
    show(state.scratch),
    state.numSpillSlots,
    state.numWideSpillSlots,
    show(state.rpo),
    [&]{
      using namespace folly::gen;
      return fmt::format(
        "{{{}}}",
        from(state.spillColors)
        | map([] (std::pair<Vreg, Color> p) {
            return fmt::format("{}: {}", show(p.first), show(p.second));
          })
        | orderBy([] (const std::string& p) {
            return p;
          })
        | unsplit<std::string>(", ")
      );
    }(),
    show(state.physChanged),
    [&] () -> std::string {
      if (state.memWrites.empty()) return "";
      std::string str;
      for (auto const b : state.rpo) {
        if (b >= state.memWrites.size() || !state.memWrites[b]) continue;
        str += fmt::format(
          "  {:5} -> {}\n",
          b, show(*state.memWrites[b])
        );
      }
      return str;
     }(),
    [&]{
      std::string str;
      for (size_t i = 0; i < state.regInfo.size(); ++i) {
        auto const& info = state.regInfo[i];
        if (!info) continue;
        str += fmt::format(
          "  {:6} -> {}\n",
          show(Vreg{i}),
          show(state.unit, *info)
        );
      }
      return str;
    }(),
    dumpBlockInfo(state.liveIn),
    dumpBlockInfo(state.liveOut),
    dumpBlockInfo(state.uses),
    dumpBlockInfo(state.defs),
    dumpBlockInfo(state.gens),
    dumpBlockInfo(state.physDefs),
    dumpBlockInfo(state.physChangedBefore),
    [&]{
      using namespace folly::gen;
      return from(state.loopInfo)
        | map([] (std::pair<Vlabel, LoopInfo> kv) {
            return fmt::format(
              "  {:5} -> {}\n",
              kv.first,
              show(kv.second)
            );
          })
        | orderBy([] (const std::string& p) {
            return p;
          })
        | unsplit<std::string>("");
    }(),
    [&]{
      std::string str;
      for (size_t i = 1; i < state.penalties.size(); ++i) {
        auto const& v = state.penalties[i];
        str += fmt::format("  {:3} -> {}\n", i, show(v));
      }
      return str;
    }()
  );
}

//////////////////////////////////////////////////////////////////////
// RegInfo accessors

// Access the RegInfo for the given Vreg (the info should exist).
RegInfo& reg_info(State& state, Vreg r) {
  assertx(r.isValid());
  assertx(!r.isPhys());
  assertx(r < state.regInfo.size());
  auto& info = state.regInfo[r];
  assertx(info);
  return *info;
}

// Access the const RegInfo for the given Vreg (the info should exist).
const RegInfo& reg_info(const State& state, Vreg r) {
  assertx(r.isValid());
  assertx(!r.isPhys());
  assertx(r < state.regInfo.size());
  auto const& info = state.regInfo[r];
  assertx(info);
  return *info;
}

// Access the RegInfo for the given Vreg, create new state if not present.
RegInfo& reg_info_create(State& state, Vreg r) {
  assertx(r.isValid());
  assertx(!r.isPhys());
  if (state.regInfo.size() <= r) state.regInfo.resize(size_t{r}+1);
  auto& info = state.regInfo[r];
  if (!info) info.emplace();
  return *info;
}

// Add RegInfo for the given Vreg (the Vreg should not already have info).
RegInfo& reg_info_insert(State& state, Vreg r, RegInfo info) {
  assertx(r.isValid());
  assertx(!r.isPhys());
  if (state.regInfo.size() <= r) state.regInfo.resize(size_t{r}+1);
  auto& newInfo = state.regInfo[r];
  assertx(!newInfo);
  newInfo.emplace(std::move(info));
  return *newInfo;
}

// Returns true if this Vreg is a physical register which is
// reserved. Such a register is generally ignored by the allocator.
bool is_ignored(const State& state, Vreg r) {
  return r.isPhys() && state.reservedRegs.contains(r.physReg());
}

// Retrieve the reg-class for this Vreg. This works for physical
// registers as well.
RegClass reg_class(const State& state, Vreg r) {
  assertx(!is_ignored(state, r));
  if (!r.isPhys()) return reg_info(state, r).regClass;
  switch (r.physReg().type()) {
    case PhysReg::GP:   return RegClass::GP;
    case PhysReg::SIMD: return RegClass::SIMD;
    case PhysReg::SF:   return RegClass::SF;
  }
  always_assert(false);
}

// Retrieve the assigned color and reg-class for this Vreg. This works
// for physical registers as well (their color will always be
// themselves).
struct RegColorInfo {
  Color color;
  RegClass regClass;
};

RegColorInfo reg_color_info(const State& state, Vreg r) {
  if (r.isPhys()) return { r.physReg(), reg_class(state, r) };
  auto const& info = reg_info(state, r);
  return { info.color, info.regClass };
}

// Is this reg-class a spill?
bool is_spill(RegClass cls) {
  return cls == RegClass::Spill || cls == RegClass::SpillWide;
}

// Does this reg-class require coloring? Both registers and spills
// need to be colored, but status flags do not.
bool is_colorable(RegClass cls) {
  switch (cls) {
    case RegClass::AnyNarrow:
    case RegClass::GP:
    case RegClass::SIMD:
    case RegClass::SIMDWide:
    case RegClass::Spill:
    case RegClass::SpillWide:
      return true;
    case RegClass::SF:
      return false;
    case RegClass::Any:
      break;
  }
  always_assert(false);
}

// Does this reg-class require coloring, and is not a spill?
bool is_colorable_reg(RegClass cls) {
  switch (cls) {
    case RegClass::AnyNarrow:
    case RegClass::GP:
    case RegClass::SIMD:
    case RegClass::SIMDWide:
      return true;
    case RegClass::SF:
    case RegClass::Spill:
    case RegClass::SpillWide:
      return false;
    case RegClass::Any:
      break;
  }
  always_assert(false);
}

// Compatible in the sense they map to the same set of physical registers.
bool compatible_reg_classes(RegClass cls1, RegClass cls2) {
  switch (cls1) {
    case RegClass::AnyNarrow:
    case RegClass::GP:
      return cls2 == RegClass::AnyNarrow || cls2 == RegClass::GP;
    case RegClass::SIMD:
    case RegClass::SIMDWide:
      return cls2 == RegClass::SIMD || cls2 == RegClass::SIMDWide;
    case RegClass::SF:
    case RegClass::Spill:
    case RegClass::SpillWide:
      return cls1 == cls2;
    case RegClass::Any:
      break;
  }
  always_assert(false);
}

// Populate the reg-class VregSets in state as appropriate.
void set_reg_class_bits(State& state, Vreg r, RegClass cls) {
  switch (cls) {
    case RegClass::AnyNarrow:
    case RegClass::GP:
      state.gps.add(r);
      break;
    case RegClass::SIMD:
    case RegClass::SIMDWide:
      state.simds.add(r);
      break;
    case RegClass::SF:
      state.flags.add(r);
      break;
    case RegClass::Any:
    case RegClass::Spill:
    case RegClass::SpillWide:
      break;
  }
}

// Wrappers around std::get<>. Will assert if you try to retrieve a value from
// the color that isn't present (so check before calling).
bool is_color_none(Color c) { return std::get_if<None>(&c); }
bool is_color_reg(Color c) { return std::get_if<PhysReg>(&c); }
bool is_color_spill_slot(Color c) { return std::get_if<SpillSlot>(&c); }
bool is_color_spill_slot_wide(Color c) { return std::get_if<SpillSlotWide>(&c); }

PhysReg color_reg(Color c) {
  auto const r = std::get_if<PhysReg>(&c);
  assertx(r);
  return *r;
}

SpillSlot color_spill_slot(Color c) {
  auto const s = std::get_if<SpillSlot>(&c);
  assertx(s);
  return *s;
}

SpillSlotWide color_spill_slot_wide(Color c) {
  auto const s = std::get_if<SpillSlotWide>(&c);
  assertx(s);
  return *s;
}

// Does this instruction represent a spill?
bool is_spill_inst(const Vinstr& inst) {
  switch (inst.op) {
    case Vinstr::spill:
    case Vinstr::spillbi:
    case Vinstr::spillli:
    case Vinstr::spillqi:
    case Vinstr::spillundefq:
      return true;
    default:
      return false;
  }
}

// Generically obtain the destination Vreg from a spill instruction.
Vreg spill_inst_dest(const Vinstr& inst) {
  switch (inst.op) {
    case Vinstr::spill:       return inst.spill_.d;
    case Vinstr::spillbi:     return inst.spillbi_.d;
    case Vinstr::spillli:     return inst.spillli_.d;
    case Vinstr::spillqi:     return inst.spillqi_.d;
    case Vinstr::spillundefq: return inst.spillundefq_.d;
    default: always_assert(false);
  }
}

//////////////////////////////////////////////////////////////////////
// Operand visitors

/*
 * We need to retrieve the operands for instructions many times. Since
 * the operands are often the same, we can cache them. We store the
 * cached operands in State, and use the "id" field in Vinstr as an
 * index into the cached information. If "id" is 0, it means nothing
 * is cached and we need to visit the operands.
 *
 * If the operands are changed, we need to call
 * invalidate_cached_operands() to reset "id" back to 0, which will
 * force re-caching of the information.
 *
 * The VregSets already have ignored registers removed from them.
 *
 * Note: we never remove information from the cached operands table,
 * so it's always safe to retain references to the VregSets, even after
 * an invalidation.
 */

struct CacheOperandsVisitor {
  CacheOperandsVisitor(VregSet& defs,
                       VregSet& uses,
                       VregSet& acrosses,
                       RegSet& physDefs,
                       RegSet& physUses,
                       const State& state)
    : defs{defs}
    , uses{uses}
    , acrosses{acrosses}
    , physDefs{physDefs}
    , physUses{physUses}
    , state{state} {}

  template<class T> void imm(const T&) {}

  template <typename T> void def(T t) {
    visit(state.unit, t, [this] (Vreg r) { addDef(r); });
  }
  template <typename T, typename U> void defHint(T t, U) { def(t); }

  template <typename T> void across(T t) {
    visit(state.unit, t, [this] (Vreg r) { addAcross(r); });
  }

  template <typename T> void use(T t) {
    visit(state.unit, t, [this] (Vreg r) { addUse(r); });
  }
  template <typename T, typename U> void useHint(T t, U) { use(t); }

  void addDef(Vreg r) {
    if (r.isPhys()) physDefs |= r.physReg();
    if (is_ignored(state, r)) return;
    defs.add(r);
  }
  void addUse(Vreg r) {
    if (r.isPhys()) physUses |= r.physReg();
    if (is_ignored(state, r)) return;
    uses.add(r);
  }
  void addAcross(Vreg r) {
    if (r.isPhys()) physUses |= r.physReg();
    if (is_ignored(state, r)) return;
    acrosses.add(r);
    uses.add(r);
  }

  VregSet& defs;
  VregSet& uses;
  VregSet& acrosses;
  RegSet& physDefs;
  RegSet& physUses;
  const State& state;
};

// Do the work of actually building the cached information.
NEVER_INLINE
void cache_operands(State& state, Vinstr& inst) {
  VregSet defs, uses, acrosses;
  RegSet physDefs;
  RegSet physUses;

  // First get the normal operands
  CacheOperandsVisitor v{defs, uses, acrosses, physDefs, physUses, state};
  visitOperands(inst, v);

  // Then add in any implicit ones
  RegSet implicitDefs, implicitUses, implicitAcrosses;
  getEffects(state.abi, inst, implicitUses, implicitAcrosses, implicitDefs);

  physDefs |= implicitDefs;
  physUses |= implicitUses;
  physUses |= implicitAcrosses;

  implicitDefs.forEach(
    [&] (PhysReg r) {
      if (state.reservedRegs.contains(r)) return;
      defs.add(r);
    }
  );
  implicitUses.forEach(
    [&] (PhysReg r) {
      if (state.reservedRegs.contains(r)) return;
      uses.add(r);
    }
  );
  implicitAcrosses.forEach(
    [&] (PhysReg r) {
      if (state.reservedRegs.contains(r)) return;
      acrosses.add(r);
      uses.add(r);
    }
  );

  // We never remove any information from the table, but the index is
  // 32-bits so we should never run out of space.
  always_assert(
    state.cachedOperands.size() <
    std::numeric_limits<decltype(inst.id)>::max()
  );

  inst.id = state.cachedOperands.size();
  assertx(inst.id > 0);
  state.cachedOperands.emplace_back(
    std::move(defs),
    std::move(uses),
    std::move(acrosses),
    std::move(physDefs),
    std::move(physUses)
  );
}

// Invalidate any cached operands for an instruction.
void invalidate_cached_operands(Vinstr& inst) {
  inst.id = 0;
}

// Cached operand getters. Check if we need to cache the operands and
// then return a reference to the appropriate set. These references
// are safe to hold onto indefinitely, as they'll never be removed (we
// only add information to the table).

const VregSet& defs_set_cached(State& state, Vinstr& inst) {
  if (!inst.id) cache_operands(state, inst);
  return state.cachedOperands[inst.id].defs;
}

const VregSet& uses_set_cached(State& state, Vinstr& inst) {
  if (!inst.id) cache_operands(state, inst);
  return state.cachedOperands[inst.id].uses;
}

const VregSet& acrosses_set_cached(State& state, Vinstr& inst) {
  if (!inst.id) cache_operands(state, inst);
  return state.cachedOperands[inst.id].acrosses;
}

// Unlike defs_set_cached(), this contains all physical registers,
// including ignored ones.
const RegSet& phys_defs_set_cached(State& state, Vinstr& inst) {
  if (!inst.id) cache_operands(state, inst);
  return state.cachedOperands[inst.id].physDefs;
}

// Unlike uses_set_cached(), this contains all physical registers,
// including ignored ones.
const RegSet& phys_uses_set_cached(State& state, Vinstr& inst) {
  if (!inst.id) cache_operands(state, inst);
  return state.cachedOperands[inst.id].physUses;
}

//////////////////////////////////////////////////////////////////////
// State utilities

void compute_rpo(State& state) {
  auto rpo = sortBlocks(state.unit);
  jit::vector<size_t> rpoOrder(state.unit.blocks.size());
  for (size_t i = 0; i < rpo.size(); ++i) rpoOrder[rpo[i]] = i;
  state.rpo = std::move(rpo);
  state.rpoOrder = std::move(rpoOrder);
}

// Return true if the given Vreg is live in (that is, before the instruction
// executes) at the instruction given by the block and index. This merely looks
// for a usage of that Vreg in the future, so it may return true if you give it
// a Vreg which isn't yet defined.
bool live_in_at(State& state, Vreg reg, Vlabel b, size_t i) {
  assertx(b < state.unit.blocks.size());
  assertx(i <= state.unit.blocks[b].code.size());
  assertx(!is_ignored(state, reg));

  // If the position is the beginning of the block, we can just use the
  // pre-calculated liveness information.
  if (i == 0) return state.liveIn[b][reg];

  // Otherwise walk through the block from the specified position and look for a
  // use of that Vreg. If we find one, it must be live (assuming it's defined at
  // the specified position).
  auto& unit = state.unit;
  auto& block = unit.blocks[b];
  for (; i < block.code.size(); ++i) {
    auto& inst = block.code[i];
    if (uses_set_cached(state, inst)[reg]) return true;
    if (reg.isPhys()) {
      if (defs_set_cached(state, inst)[reg]) return false;
    }
  }
  // If we reach the end of the block without finding a usage, it may still be
  // used in a successor, so use the pre-calculated live-out information.
  return state.liveOut[b][reg];
}

// Calculate liveness in the traditional dataflow way. There's more
// efficient algorithms leveraging SSA but they require tracking use
// and def positions and it doesn't seem worth it (this is fast enough
// in practice). If `changed' isn't provided (indicating this is the
// first calculation), then also calculate metadata related to
// physical register defs.
void calculate_liveness(State& state, const BlockSet* changed = nullptr) {
  auto& unit = state.unit;

  if (changed) {
    assertx(state.liveIn.size() == unit.blocks.size());
    assertx(state.liveOut.size() == unit.blocks.size());
    assertx(state.uses.size() == unit.blocks.size());
    assertx(state.defs.size() == unit.blocks.size());
    assertx(state.gens.size() == unit.blocks.size());
    assertx(state.physDefs.size() == unit.blocks.size());
    assertx(state.physChangedBefore.size() == unit.blocks.size());
    assertx(state.spRecordedIn.size() == unit.blocks.size());
    assertx(state.spRecordedOut.size() == unit.blocks.size());
    assertx(state.hasUnrecordbasenativesp.size() == unit.blocks.size());
    assertx(changed->size() == unit.blocks.size());
  } else {
    state.liveIn.resize(unit.blocks.size());
    state.liveOut.resize(unit.blocks.size());
    state.uses.resize(unit.blocks.size());
    state.defs.resize(unit.blocks.size());
    state.gens.resize(unit.blocks.size());
    state.physDefs.resize(unit.blocks.size());
    state.physChangedBefore.resize(unit.blocks.size());
    state.spRecordedIn.resize(unit.blocks.size());
    state.spRecordedOut.resize(unit.blocks.size());
    state.hasUnrecordbasenativesp.resize(unit.blocks.size());
  }

  auto const processBlock = [&] (Vlabel b,
                                 VregSet& g,
                                 VregSet& k,
                                 VregSet& u,
                                 RegSet& r,
                                 bool& spRecorded,
                                 bool& hasUnrecordbasenativesp) {
    auto& block = unit.blocks[b];
    for (auto& inst : boost::adaptors::reverse(block.code)) {
      auto const& defs = defs_set_cached(state, inst);
      auto const& uses = uses_set_cached(state, inst);
      r |= phys_defs_set_cached(state, inst);
      k |= defs;
      g -= defs;
      g |= uses;
      u |= uses;
      if (inst.op == Vinstr::recordbasenativesp) {
        assert_flog(!spRecorded, "Block B{} {} initiailizes native SP, "
                    "but already initialized.", b, show(unit, inst));
        spRecorded = true;
      }
      if (inst.op == Vinstr::unrecordbasenativesp) {
        hasUnrecordbasenativesp = true;
      }
    }
    g -= state.flags;
    k -= state.flags;
    u -= state.flags;
  };

  dataflow_worklist<size_t, std::less<size_t>> worklist(state.rpo.size());
  dataflow_worklist<size_t> physWorklist(!changed ? state.rpo.size() : 0);
  for (size_t i = 0; i < state.rpo.size(); ++i) {
    auto const b = state.rpo[i];

    if (!changed || (*changed)[b]) {
      auto& g = state.gens[b];
      auto& k = state.defs[b];
      auto& u = state.uses[b];
      auto& r = state.physDefs[b];
      bool spRecorded = state.spRecordedIn[b];
      bool hasUnrecordbasenativesp = false;
      g.reset();
      k.reset();
      u.reset();
      r = RegSet{};
      processBlock(b, g, k, u, r, spRecorded, hasUnrecordbasenativesp);
      state.spRecordedOut[b] = spRecorded;
      state.hasUnrecordbasenativesp[b] = hasUnrecordbasenativesp;
      for (auto const succ : succs(unit.blocks[b])) {
        state.spRecordedIn[succ] = spRecorded;
      }
    } else if (debug) {
      VregSet g;
      VregSet k;
      VregSet u;
      RegSet r;
      bool spRecorded = state.spRecordedIn[b];
      bool hasUnrecordbasenativesp = false;
      processBlock(b, g, k, u, r, spRecorded, hasUnrecordbasenativesp);
      always_assert(g == state.gens[b]);
      always_assert(k == state.defs[b]);
      always_assert(u == state.uses[b]);
      always_assert(r == state.physDefs[b]);
      always_assert(state.spRecordedOut[b] == spRecorded);
      for (auto const succ : succs(unit.blocks[b])) {
        always_assert(state.spRecordedIn[succ] == spRecorded);
      }
      always_assert(state.hasUnrecordbasenativesp[b] ==
                    hasUnrecordbasenativesp);
    }

    state.liveIn[b].reset();
    state.liveOut[b].reset();

    worklist.push(i);
    if (!changed) {
      // If we're calculating physical register metadata, also prepare
      // the worklist for that.
      state.physChangedBefore[b] = RegSet{};
      physWorklist.push(i);
    }
  }

  while (!worklist.empty()) {
    auto const b = state.rpo[worklist.pop()];
    auto const& block = unit.blocks[b];

    auto& out = state.liveOut[b];
    out.reset();
    for (auto const s : succs(block)) out |= state.liveIn[s];
    auto transfer = out;
    transfer -= state.defs[b];
    transfer |= state.gens[b];

    if (transfer != state.liveIn[b]) {
      for (auto const pred : state.preds[b]) {
        worklist.push(state.rpoOrder[pred]);
      }
      state.liveIn[b] = std::move(transfer);
    }
  }

  if (!changed) {
    // Calculate the physical register metadata. We can't use the
    // above dataflow, because this is a forward dataflow, and the
    // previous is a backwards dataflow.
    while (!physWorklist.empty()) {
      auto const b = state.rpo[physWorklist.pop()];
      auto const& block = unit.blocks[b];

      // Flow any physical registers which have been changed into the
      // block's successors. Also record which registers have been
      // modified anywhere.
      state.physChanged |= state.physDefs[b];
      auto out = state.physDefs[b];
      for (auto const pred : state.preds[b]) {
        out |= state.physChangedBefore[pred];
      }

      if (out != state.physChangedBefore[b]) {
        for (auto const s : succs(block)) {
          physWorklist.push(state.rpoOrder[s]);
        }
        state.physChangedBefore[b] = out;
      }
    }
  }

  // Only physical registers should be live-in to the entry block.
  assertx(
    (state.liveIn[state.unit.entry] -
     VregSet{state.gpUnreserved | state.simdUnreserved}).none()
  );
}

// Helper function to retrieve loop info (and asserting if it doesn't
// exist). Avoid using [] because it will create new entries.
const LoopInfo& loop_info(const State& state, Vlabel loop) {
  auto const it = state.loopInfo.find(loop);
  assertx(it != state.loopInfo.end());
  return it->second;
}
LoopInfo& loop_info(State& state, Vlabel loop) {
  auto const it = state.loopInfo.find(loop);
  assertx(it != state.loopInfo.end());
  return it->second;
}

// A block is a loop header if we have loop info for it (since loops are keyed
// by their header).
bool is_loop_header(const State& state, Vlabel b) {
  return state.loopInfo.find(b) != state.loopInfo.end();
}

// Whether the given block is a member of the given loop
bool block_in_loop(const State& state, Vlabel b, Vlabel loop) {
  assertx(is_loop_header(state, loop));
  auto const it = state.blockToLoop.find(b);
  if (it == state.blockToLoop.end()) return false;
  if (it->second == loop) return true;
  auto p = state.loopMembership.equal_range(it->second);
  while (p.first != p.second) {
    if (p.first->second == loop) return true;
    ++p.first;
  }
  return false;
}

// Identify the loops within an unit (only their relationship to each
// other).
void find_loops(State& state) {
  auto& unit = state.unit;

  // Find the loops
  auto const backEdges =
    findBackEdges(unit, state.rpo, findDominators(unit, state.rpo));
  if (backEdges.empty()) return;

  auto loopBlocks = findLoopBlocks(unit, state.preds, backEdges);

  // First create entries in loopInfo for each block. After this, we'll never
  // create new entries, just modify the existing ones.
  state.loopInfo.reserve(loopBlocks.size());
  for (auto const& p : loopBlocks) {
    auto const DEBUG_ONLY result = state.loopInfo.emplace(p.first, LoopInfo{});
    assertx(result.second);
  }

  // Now calculate the loop nesting depths by bumping it for every loop
  // contained within another. Also track which loops are members of other loops
  // for the register pressure calculation.
  for (auto const& p : loopBlocks) {
    for (auto const b : p.second) {
      auto const it = state.loopInfo.find(b);
      if (it == state.loopInfo.end()) continue; // Not a loop header
      ++it->second.depth;
      state.loopMembership.emplace(b, p.first);
    }
  }

  // Create the block to loop mapping. A block will be assigned to the loop with
  // the highest depth (the inner-most nested one).
  for (auto const& p : loopBlocks) {
    auto const newLoopInfo = state.loopInfo.find(p.first);
    assertx(newLoopInfo != state.loopInfo.end());

    for (auto const b : p.second) {
      auto& loop = state.blockToLoop[b];
      if (!loop.isValid()) { // First assignment
        loop = p.first;
        continue;
      }
      // It's already assigned a loop. Use the new one if it has a higher depth.
      auto const oldLoopInfo = state.loopInfo.find(loop);
      assertx(oldLoopInfo != state.loopInfo.end());
      if (newLoopInfo->second.depth > oldLoopInfo->second.depth) {
        loop = p.first;
      }
    }
  }

  for (auto& p : loopBlocks) {
    auto const it = state.loopInfo.find(p.first);
    assertx(it != state.loopInfo.end());
    it->second.blocks = std::move(p.second);
    // Keep the blocks in RPO order
    std::sort(
      it->second.blocks.begin(),
      it->second.blocks.end(),
      [&] (Vlabel a, Vlabel b) { return state.rpoOrder[a] < state.rpoOrder[b]; }
    );
  }
}

// Calculate the Vregs used within a loop. We don't do this while
// calculating the other loop metadata because it's more expensive and
// we only need it when/if spilling.
void calculate_loop_uses(State& state) {
  // If there's no loops, nothing to do.
  if (state.loopInfo.empty()) return;

  auto& unit = state.unit;

  // Record that a Vreg is used within a loop, returning true if this
  // is new information or not.
  auto const markUse = [&] (Vreg r, Vlabel loop) {
    if (r.isPhys()) return false;
    if (reg_class(state, r) == RegClass::SF) return false;
    auto& info = loop_info(state, loop);
    auto const oldSize = info.uses.size();
    info.uses.add(r);
    return info.uses.size() != oldSize;
  };

  // Deal with a copy between two Vregs for a set of loops. The source
  // is used only if the destination is.
  auto const copyish = [&] (Vreg s, Vreg d, auto loops) {
    auto changed = false;
    for (auto l = loops.first; l != loops.second; ++l) {
      if (!d.isPhys() && !loop_info(state, l->second).uses[d]) continue;
      changed |= markUse(s, l->second);
    }
    return changed;
  };

  // Iterate over the unit, and record all usages within loops. For
  // the first iteration, we look at all instructions, and only copies
  // for the rest. Copies make this analysis flow sensitive, so we
  // only need to iterate those to a fixed point. Return true if
  // anything new was learned.
  auto const process = [&] (bool copiesOnly) {
    auto changed = false;
    for (auto const b : boost::adaptors::reverse(state.rpo)) {
      // Find the loops that this block belongs to (as an iterator range).
      auto const loops = [&] {
        auto const it = state.blockToLoop.find(b);
        if (it == state.blockToLoop.end()) {
          return std::make_pair(
            state.loopMembership.end(),
            state.loopMembership.end()
          );
        }
        auto const range = state.loopMembership.equal_range(it->second);
        assertx(range.first != range.second);
        return range;
      }();
      // Not in a loop, skip
      if (loops.first == loops.second) continue;

      // Information flows backwards from dst to src in copies, so
      // walk backwards.
      for (auto& inst : boost::adaptors::reverse(unit.blocks[b].code)) {
        switch (inst.op) {
          case Vinstr::copy:
            changed |= copyish(inst.copy_.s, inst.copy_.d, loops);
            break;
          case Vinstr::spill:
            changed |= copyish(inst.spill_.s, inst.spill_.d, loops);
            break;
          case Vinstr::reload:
            changed |= copyish(inst.reload_.s, inst.reload_.d, loops);
            break;
          case Vinstr::ssaalias:
            changed |= copyish(inst.ssaalias_.s, inst.ssaalias_.d, loops);
            break;
          case Vinstr::copyargs: {
            auto const& s = unit.tuples[inst.copyargs_.s];
            auto const& d = unit.tuples[inst.copyargs_.d];
            assertx(s.size() == d.size());
            for (size_t i = 0; i < s.size(); ++i) {
              changed |= copyish(s[i], d[i], loops);
            }
            break;
          }
          case Vinstr::phijmp: {
            auto const& s = unit.tuples[inst.phijmp_.uses];
            auto const& target = unit.blocks[inst.phijmp_.target].code.front();
            assertx(target.op == Vinstr::phidef);
            auto const& d = unit.tuples[target.phidef_.defs];
            assertx(s.size() == d.size());
            for (size_t i = 0; i < s.size(); ++i) {
              changed |= copyish(s[i], d[i], loops);
            }
            break;
          }
          default:
            if (copiesOnly) break;
            for (auto const r : uses_set_cached(state, inst)) {
              for (auto l = loops.first; l != loops.second; ++l) {
                changed |= markUse(r, l->second);
              }
            }
            break;
        }
      }
    }
    return changed;
  };

  // Loop while things change, only processing all instructions the
  // first time.
  auto notFirst = false;
  while (process(notFirst)) notFirst = true;
}

// Map an arbitrary block to the depth of the loop it belongs to (if any).
size_t block_loop_depth(const State& state, Vlabel b) {
  auto const it = state.blockToLoop.find(b);
  if (it == state.blockToLoop.end()) return 0; // Not in a loop
  auto const& info = loop_info(state, it->second);
  // The depth includes itself, so must always be at least 1.
  assertx(info.depth > 0);
  return info.depth;
}

// Return "weight" of a block, an approximation of how many times it will be
// executed.
int64_t block_weight(const State& state, Vlabel b) {
  // Weight cannot be zero, as that means doing anything in it is
  // "free" (leads to nonesense decisions).
  return std::max<int64_t>(state.unit.blocks[b].weight, 1);
}

State make_state(Vunit& unit, const Abi& abi) {
  assertx(!abi.unreserved().contains(rsp()));

  // Reserve a SIMD scratch register to resolve register shuffles.
  auto const scratch = [&] () -> PhysReg {
    return ARCH_MATCH(
      [](arch::X64) { return reg::xmm15; },
      [](arch::ARM) { return vixl::d31; }
    );
  }();
  assertx(scratch.isSIMD());

  State state{
    unit,
    abi,
    {},
    {},
    computePreds(unit),
    abi.gpUnreserved,
    abi.simdUnreserved - scratch,
    (abi.all() - (abi.gpUnreserved | abi.simdUnreserved)) | scratch,
    scratch
  };

  // Pre-size the table to avoid excessive resizing.
  state.regInfo.reserve(unit.next_vr * 2);

  compute_rpo(state);

  size_t instrCount = 0;
  for (auto const b : state.rpo) {
    instrCount += state.unit.blocks[b].code.size();
  }
  state.cachedOperands.reserve(instrCount * 2);

  // Insert dummy cached operand entry since 0 is not a valid index.
  state.cachedOperands.emplace_back();

  return state;
}

//////////////////////////////////////////////////////////////////////
// Pre-allocation preparation

/*
 * Constant placement
 *
 * Vregs representing constants need to be materialized and placed in
 * the unit before register allocation. Each constant needs to be
 * materialized before it's first usage. The easiest thing would be to
 * just place constants in the entry block, but that's not optimal.
 *
 * Ideally a constant should be (1) placed as close to possible to its uses (to
 * minimize register pressure), (2) as few times as possible (to minimize code
 * size), and should not be (3) placed on a path where it may not be used (to
 * minimize useless work).
 *
 * (1) can be dealt with by starting at the entry block and then sinking the
 * definitions as far as possible using standard dataflow. This will prevent us
 * from sinking constant definitions inside loops, which is desirable, and
 * satisfies (3), but tends to violate (2). With no other constraints, the
 * definitions will tend to be sunk to immediately before all their usages,
 * leading to code duplication.
 *
 * To prevent this (and satisfy (2)), we first calculate "sink stops". These
 * indicate that at a particular block, a particular Vreg (representing the
 * constant) should not be sunk further.
 *
 * The sink stops for a constant are the post-dominator frontier of all usages
 * of that constant. That is, the blocks where the constant is used on all
 * successors from that block (or in the block itself), but where the
 * predecessor is not a sink stop for that constant. This captures (2) and (3)
 * as we will not place the constant where there's a path where it might be
 * used, but if the constant is used on all paths, we won't duplicate it along
 * those paths.
 *
 * Once sink stops are calculated, we start at the entry block and attempt to
 * sink each constant as far as possible. We use typical partial redundancy
 * dataflow for this and stop sinking at a sink stop, or if we encounter a use
 * of that constant. In addition, we use liveness of the constants to avoid
 * sinking down paths where the constant isn't used at all.
 *
 * Once we stop sinking, we insert the appropriate "ldimm" vasm instruction
 * (writing to its associated Vreg) at that point to materialize the constant. A
 * given constant (which has a single Vreg) may be placed at multiple points,
 * which means that Vreg now has multiple definitions. This is fine, as the unit
 * will be rewritten into SSA form anyways, which will fix this.
 *
 * As an optimization, we look for "trivial" constants. These are
 * constants used exactly one place (not within a loop). Such
 * constants do not require the above mentioned dataflow and can be
 * placed immediately before their use.
 */

struct PlaceConstantsBlockInfo {
  VregSet stopSink;
  VregSet liveIn;
};

// Calculate sink stop and liveness information for each constant for
// every block, returning an empty vector if there's no
// constants. Also return the set of trivial constants in each block
// (if any), and a set of blocks which have non-trivial constant uses.

struct ComputePlaceConstantsBlockInfoRet {
  jit::vector<PlaceConstantsBlockInfo> blockInfo;
  jit::vector<VregSet> trivial;
  BlockSet withConsts;
};

ComputePlaceConstantsBlockInfoRet
compute_place_constants_block_info(State& state) {
  auto& unit = state.unit;
  auto const& rpo = state.rpo;
  auto const& rpoOrder = state.rpoOrder;
  auto const& preds = state.preds;

  using Ret = ComputePlaceConstantsBlockInfoRet;
  if (unit.regToConst.empty()) return Ret{{}, {}, BlockSet{}};

  /* Per-block dataflow state:
   *
   * We keep track of two things for dataflow:
   *
   * definitelyUsed - True if a register is used in the block, or if
   *                  definitelyUsed for all successors.
   *
   * nearestUse - This block if the register is used by this block, or
   *              if there's different blocks in the uses state among
   *              the successors. Otherwise it's the same as all the
   *              successors' uses block.
   */
  struct BlockState {
    // Mapping of a constant to its nearest usage (or union of multiple usages)
    // at this block
    jit::fast_map<Vreg, Vlabel> nearestUse;
    // Set of constants which are used along all paths from this block
    VregSet definitelyUsed;
    bool operator==(const BlockState& o) const {
      return std::tie(nearestUse, definitelyUsed) ==
        std::tie(o.nearestUse, o.definitelyUsed);
    }
    bool operator!=(const BlockState& o) const { return !(*this == o); }
  };

  // Dataflow bock state. As an optimization, we store the state in a copy_ptr,
  // which lets us share it between blocks cheaply. We use an empty copy_ptr to
  // represent the empty state.
  jit::vector<copy_ptr<BlockState>> blockIn(unit.blocks.size());

  // Mapping of blocks to the constants used in that block
  jit::vector<VregSet> blockUses(unit.blocks.size());

  dataflow_worklist<size_t, std::less<size_t>> worklist(rpo.size());

  BlockSet visited(unit.blocks.size());
  visited.flip();

  // All constants with a use
  VregSet allUsed;
  // All constants with more than one use, or a use within a loop
  VregSet nonTrivial;
  // Per-block trivial uses
  jit::vector<VregSet> trivialUses(unit.blocks.size());

  // Populate the constant usages for each block. Since this is a backwards
  // dataflow, start the worklist with successor-less blocks (the exits).
  for (size_t i = 0; i < rpo.size(); ++i) {
    auto const b = rpo[i];
    auto& block = unit.blocks[b];
    auto& uses = blockUses[b];

    for (auto& inst : block.code) {
      // Special case: If we have a copy moving a constant into an
      // ignored register, turn it into the appropriate ldimm
      // instruction with that register as the destination. Otherwise
      // we'll materialize the constant into an available register and
      // then immediately copy it into the ignored register, which is
      // inefficient. This happens a lot in stubs.
      if (inst.op == Vinstr::copy &&
          is_ignored(state, inst.copy_.d) &&
          unit.regToConst.count(inst.copy_.s)) {
        auto const d = inst.copy_.d;
        auto const it = unit.regToConst.find(inst.copy_.s);
        assertx(it != unit.regToConst.end());
        auto const& vconst = it->second;
        switch (vconst.kind) {
          case Vconst::Quad:
          case Vconst::Double:
            if (vconst.isUndef) {
              inst.op = Vinstr::ldundefq;
              inst.ldundefq_.d = d;
            } else {
              inst.op = Vinstr::ldimmq;
              inst.ldimmq_.s = uint64_t(vconst.val);
              inst.ldimmq_.d = d;
            }
            break;
          case Vconst::Long:
            assertx(!vconst.isUndef);
            inst.op = Vinstr::ldimml;
            inst.ldimml_.s = int32_t(vconst.val);
            inst.ldimml_.d = d;
            break;
          case Vconst::Byte:
            assertx(!vconst.isUndef);
            inst.op = Vinstr::ldimmb;
            inst.ldimmb_.s = uint8_t(vconst.val);
            inst.ldimmb_.d = d;
            break;
        }
        invalidate_cached_operands(inst);
      } else {
        for (auto const r : uses_set_cached(state, inst)) {
          if (unit.regToConst.contains(r)) uses.add(r);
        }
      }

      // A constant should never have a definition (before we place them).
      if (debug) {
        for (auto const r : defs_set_cached(state, inst)) {
          always_assert(!unit.regToConst.contains(r));
        }
      }
    }

    // Update triviality information. Mark a constant as trivial if we
    // haven't seen any previous uses. If we detect a second use, mark
    // it as non-trivial. Since we don't update previous block
    // information, it may be incorrect after this. We'll fix it up
    // later by subtracting "nonTrivial" from it.
    auto const inLoop = block_loop_depth(state, b) > 0;
    for (auto const r : uses) {
      if (inLoop || allUsed[r]) {
        allUsed.add(r);
        nonTrivial.add(r);
        trivialUses[b].remove(r);
        continue;
      }
      allUsed.add(r);
      trivialUses[b].add(r);
    }

    visited[b] = false;
    if (succs(block).size() == 0) worklist.push(i);
  }

  // We only need the dataflow if there's any non-trivial usages.
  if (nonTrivial.none()) {
    // Either the unit has no usages at all, or all usages are
    // trivial.
    if (allUsed.none()) return Ret{{}, {}, BlockSet{}};
    return Ret{{}, std::move(trivialUses), BlockSet{}};
  }

  BlockSet blocksWithUses(unit.blocks.size());

  while (!worklist.empty()) {
    auto const b = rpo[worklist.pop()];
    auto const& block = unit.blocks[b];

    // Fixup the per-block trivial information by removing constants
    // known to be non-trivial.
    trivialUses[b] -= nonTrivial;

    auto const successors = succs(block);

    // All successors are identical if there's no adjacent successors which are
    // different.
    auto const allSuccsIdentical =
      std::adjacent_find(
        successors.begin(),
        successors.end(),
        [&] (Vlabel s1, Vlabel s2) {
          // NB: We're checking if the successors are different, so the return
          // values are inverted.
          if (!visited[s1] || !visited[s2]) return true;
          auto const& succState1 = blockIn[s1];
          auto const& succState2 = blockIn[s2];
          // Short-cut if they're the same pointer
          if (succState1.get() == succState2.get()) return false;
          if (!succState1 || !succState2) return true;
          return *succState1 != *succState2;
        }
      ) == successors.end();

    // Schedule predecessors if the state has changed. Return true if the state
    // changed, false otherwise.
    auto const schedulePreds = [&] (const BlockState* newState) {
      auto const& oldState = blockIn[b];
      assertx(!oldState || newState);
      if (!visited[b] || (!oldState && newState) ||
          (oldState.get() != newState && *oldState != *newState)) {
        for (auto const pred : preds[b]) worklist.push(rpoOrder[pred]);
        return true;
      }
      return false;
    };

    auto const uses = blockUses[b] & nonTrivial;
    if (allSuccsIdentical) {
      // All the successors have identical state. We can just cheaply copy one
      // of the successors state for our own (because they're in copy_ptrs).
      assertx(successors.empty() || visited[successors[0]]);

      // Copy arbitrary successor
      auto newState = decltype(blockIn)::value_type{};
      if (!successors.empty()) newState = blockIn[successors[0]];

      if (uses.any()) {
        // If this block has any usages, we have to mutate the state (and make a
        // deep copy). Since this block uses the constant, that constant is
        // definitely used at this point.
        if (!newState) newState.emplace();
        auto mutated = newState.mutate();
        mutated->definitelyUsed |= uses;
        for (auto const r : uses) mutated->nearestUse.insert_or_assign(r, b);
        blocksWithUses[b] = true;
      }
      // Update this block's state and schedule any predecessors
      if (schedulePreds(newState.get())) blockIn[b] = newState;
    } else {
      // Successors have differing states. We can't copy it cheaply but need to
      // compute the union among them.
      assertx(!successors.empty());

      BlockState newState;
      auto first = true;
      for (auto const& succ : successors) {
        if (!visited[succ]) continue;

        auto const& succState = blockIn[succ];
        if (!succState) {
          // Successor doesn't use anything, so nothing from the successors is
          // definitely used.
          newState.definitelyUsed.reset();
          first = false;
          continue;
        }
        // Merge together usages from this successor. If we already have a usage
        // for this constant and that usage isn't the same block, we're at a
        // join point between distinct usages. Treat the current block as the
        // usage point.
        for (auto const& p : succState->nearestUse) {
          auto const result = newState.nearestUse.emplace(p.first, p.second);
          if (!result.second && result.first->second != p.second) {
            result.first->second = b;
          }
        }
        // Constants are definitely used only if they're definitely used by all
        // of their successors.
        if (first) {
          newState.definitelyUsed = succState->definitelyUsed;
          first = false;
        } else {
          newState.definitelyUsed &= succState->definitelyUsed;
        }
      }
      assertx(!first);

      // If this block uses a constant, that constant is definitely used at this
      // point.
      newState.definitelyUsed |= uses;
      for (auto const r : uses) newState.nearestUse.insert_or_assign(r, b);
      if (uses.any()) blocksWithUses[b] = true;

      // Update this block's state and schedule any predecessors
      if (schedulePreds(&newState)) blockIn[b].emplace(std::move(newState));
    }

    visited[b] = true;
  };

  always_assert(visited.all());

  jit::vector<PlaceConstantsBlockInfo> outInfo(unit.blocks.size());
  for (auto const b : rpo) {
    auto const& state = blockIn[b];
    if (!state) continue;

    auto& out = outInfo[b];

    for (auto const& p : state->nearestUse) {
      // A constant is live if there's any usage from this block.
      out.liveIn.add(p.first);
      // We want to stop sinking at this block only if it's definitely used from
      // this point, and the nearest usage is this block. The latter condition
      // implies that either the constant is used in this block, or there's at
      // least two distinct use sites from this block. Since the constant is
      // definitely used, it's not profitable to sink any farther.
      if (state->definitelyUsed[p.first] && p.second == b) {
        out.stopSink.add(p.first);
      }
    }
  }

  return Ret{
    std::move(outInfo),
    std::move(trivialUses),
    std::move(blocksWithUses)
  };
}

// Find the last index in the block where it's not legal to insert the
// ldimm. IE, avoid the special instructions which end blocks which we
// cannot insert anything after.
size_t find_first_invalid_block_index(const Vblock& block) {
  auto const valid = [] (Vinstr::Opcode op) {
    return op != Vinstr::unwind &&
      op != Vinstr::syncpoint &&
      op != Vinstr::nothrow &&
      op != Vinstr::fallthru;
  };
  auto i = block.code.size();
  while (i > 0 && !valid(block.code[i-1].op)) --i;
  return i;
}

// Look for any blocks where all the successors of the block define
// the same constant at the front. Hoist those definitions up into the
// end of the block. Constant materialization can sometimes cause such
// definitions, and it's difficult to modify the dataflow to not do
// so. Instead just fix it up after the fact.
void hoist_constants(State& state, BlockSet& changed) {
  auto& unit = state.unit;

  // NB: Be careful with references to things inside the unit here,
  // since they can be invalidated whenever we call vmodify.
  for (auto const b : state.rpo) {
    auto successors = succs(unit.blocks[b]);
    // Can only happen if we have more than one successor
    if (successors.size() < 2) continue;

    // Avoid self-loops or blocks without all successors changed
    if (std::any_of(successors.begin(),
                    successors.end(),
                    [&] (Vlabel s) { return !changed[s] || s == b; })) {
      continue;
    }

    // Search a block for a constant defining the given Vreg. We stop
    // looking if we encounter a non-constant materialization
    // instruction.
    auto const findVreg = [&] (Vreg r, Vlabel succ) -> Optional<size_t> {
      size_t idx = 0;
      auto const& code = unit.blocks[succ].code;
      auto const limit = code.size();
      while (idx < limit) {
        auto const& inst = code[idx];
        switch (inst.op) {
          case Vinstr::ldimmq:
            if (inst.ldimmq_.d == r) return idx;
            break;
          case Vinstr::ldimml:
            if (inst.ldimml_.d == r) return idx;
            break;
          case Vinstr::ldimmb:
            if (inst.ldimmb_.d == r) return idx;
            break;
          default:
            return std::nullopt;
        }
        ++idx;
      }
      return std::nullopt;
    };

    VregSet hoisted;

    // Attempt to hoist a Vreg (defined by the given instruction from
    // one of the successors) into the parent block.
    auto const hoist = [&] (Vreg r, const Vinstr& inst) {
      // Check all the other successors and make sure it's also defined there.
      for (size_t succIndex = 1; succIndex < successors.size(); ++succIndex) {
        if (!findVreg(r, successors[succIndex])) return;
      }

      // It is, so copy the instruction into the end of the parent
      // block.
      vmodify(
        unit, b, find_first_invalid_block_index(unit.blocks[b]) - 1,
        [&] (Vout& v) { v << inst; return 0; }
      );
      // vmodify could have shift data, so re-calculate successors
      // (which is a pointer).
      successors = succs(unit.blocks[b]);
      hoisted.add(r);
      changed[b] = true;
    };

    // For every constant defined in the first successor (an arbitrary
    // choise), attempt to hoist into the parent. We only examine
    // constants which are defined before any other instruction.
    //
    // hoist() can call vmodify, which can invalidate references in
    // the unit, so use indices.
    for (size_t idx = 0; idx < unit.blocks[successors[0]].code.size(); ++idx) {
      auto const& inst = unit.blocks[successors[0]].code[idx];
      switch (inst.op) {
        case Vinstr::ldimmq:
          hoist(inst.ldimmq_.d, inst);
          continue;
        case Vinstr::ldimml:
          hoist(inst.ldimml_.d, inst);
          continue;
        case Vinstr::ldimmb:
          hoist(inst.ldimmb_.d, inst);
          continue;
        default:
          break;
      }
      break;
    }

    // Didn't hoist anything, so we're done
    if (hoisted.none()) continue;

    // Otherwise we need to remove the now unneeded definitions from
    // the successors.
    for (size_t idx = 0; idx < successors.size(); ++idx) {
      auto const succ = successors[idx];
      for (auto const r : hoisted) {
        auto const constIdx = findVreg(r, succ);
        assertx(constIdx); // Should be there
        vmodify(unit, succ, *constIdx, [&] (Vout&) { return 1; });
      }
      // vmodify() might have invalidated successors (which is a
      // pointer), so re-acquire it.
      successors = succs(unit.blocks[b]);
    }
  }
}

// Materialize constants by placing instructions to create them at the
// appropriate points. The set of constants placed multiple times
// (thus needing SSA restoration) is returned, along with a block set
// containing the blocks containing those constants.
std::pair<VregSet, BlockSet> place_constants(State& state) {
  auto& unit = state.unit;

  auto info = compute_place_constants_block_info(state);
  auto const& blockInfo = info.blockInfo;
  auto const& trivial = info.trivial;
  auto& withConsts = info.withConsts;
  if (blockInfo.empty() && trivial.empty()) return {{}, BlockSet{}};

  assertx(trivial.size() == unit.blocks.size());

  auto const& rpo = state.rpo;
  auto const& rpoOrder = state.rpoOrder;
  auto const& preds = state.preds;

  // The set of constants in each block which can be sunk to the beginning of
  // that block.
  jit::vector<VregSet> inStates;
  // The set of constants in each block which can be sunk to the end of that
  // block.
  jit::vector<VregSet> outStates;

  // If we don't have block information, all uses are trivial so we
  // don't need dataflow.
  if (!blockInfo.empty()) {
    assertx(blockInfo.size() == unit.blocks.size());

    inStates.resize(unit.blocks.size());
    outStates.resize(unit.blocks.size());
    BlockSet visited(unit.blocks.size());

    dataflow_worklist<size_t> worklist(rpo.size());

    assertx(!rpo.empty() && rpo[0] == unit.entry);
    worklist.push(0);

    /*
     * Dataflow:
     *
     * IN(B) = intersection of OUT(P) for each P in PREDS(B)
     *
     * (It is safe to sink a constant to the beginning of B if it can be sunk to
     * the end of every predecessor).
     *
     * OUT(B) = (IN(B) - STOP(B)) & LIVE(B)
     *
     * (It is safe to sink a constant to the end of B if it can be sunk to the
     * beginning of B, does not have a stop sink, and is live from B.
     *
     * PLACE(B) = (IN(B) & LIVE(B) & STOP(B)) | (OUT(B) - (intersection of IN(S)
     *             for each S in SUCCS(B)))
     *
     * (A constant should be placed in B if it can be sunk to the beginning of
     * B, is live in B, and is stopped in B. It should also be placed in B if it
     * can be sunk to the end of B, and can not be sunk to the beginning of all
     * of B's successors).
     */
    do {
      auto const b = rpo[worklist.pop()];
      auto const& info = blockInfo[b];
      auto const& predList = preds[b];
      auto& in = inStates[b];
      auto& out = outStates[b];

      if (b == unit.entry) {
        assertx(predList.empty());
        assertx(!visited[b]);
        assertx(in.none());
        assertx(out.none());
        // A constant can be trivially "sunk" to the beginning of the entry
        // block if it's live.
        in = info.liveIn;
      } else {
        assertx(!predList.empty());

        // Find all the constants which can be sunk to the beginning of this
        // block. A constant can be sunk to the beginning of this block if it
        // can be sunk to the end of all of its predecessors.

        in.reset();
        // Find the first visited predecessor. There has to be at least one
        // because we're going in RPO order.
        auto predIt = std::find_if(
          predList.begin(),
          predList.end(),
          [&] (Vlabel p) { return visited[p]; }
        );
        assertx(predIt != predList.end());

        in = outStates[*predIt];
        for (++predIt; predIt != predList.end(); ++predIt) {
          if (visited[*predIt]) in &= outStates[*predIt];
        }
      }

      // From the constants which can be sunk to the beginning of the block,
      // find the subset which can be sunk to the end. A constant can be sunk
      // through the block if there's not a sink stop and if it's still live in
      // the block.
      auto newOut = in;
      newOut -= info.stopSink;
      newOut &= info.liveIn;

      // Schedule successors if the out state has changed.
      if (!visited[b] || newOut != out) {
        out = std::move(newOut);
        for (auto const succ : succs(unit.blocks[b])) {
          worklist.push(rpoOrder[succ]);
        }
      }

      visited[b] = true;
    } while (!worklist.empty());
  }

  VregSet allPlaced;
  VregSet multiplePlaced;
  VregSet placeThisInst;
  for (auto const b : rpo) {
    auto place = [&] {
      // If we have no in states, it's because we didn't perform
      // dataflow above, which only happens if we only have trivial
      // uses. Therefore the trivial set is everything we might place.
      if (inStates.empty()) return trivial[b];
      assertx(inStates.size() == unit.blocks.size());
      assertx(outStates.size() == unit.blocks.size());

      // Find the constants which should be materialized in this block. The
      // constant should be materialized if it can be sunk to the beginning of
      // the block, is live in that block, and we have a sink stop.
      auto const& info = blockInfo[b];
      auto place = inStates[b];
      place &= info.liveIn;
      place &= info.stopSink;

      auto const& succList = succs(unit.blocks[b]);
      assertx(!succList.empty() || outStates[b].none());
      if (!succList.empty()) {
        // If a constant could be sunk to the end of the block, but not sunk to
        // the beginning of all of its predecessors, we also need to place it
        // here.
        auto sunk = inStates[succList[0]];
        for (size_t i = 1; i < succList.size(); ++i) {
          sunk &= inStates[succList[i]];
        }
        auto didntSink = outStates[b];
        didntSink -= sunk;
        place |= didntSink;
      }
      return place | trivial[b];
    }();
    if (place.none()) continue;

    multiplePlaced |= (allPlaced & place);
    allPlaced |= place;
    // We can ignore trivial constants because those are automatically
    // in SSA form
    if ((place - trivial[b]).any()) {
      assertx(withConsts.size() == unit.blocks.size());
      withConsts[b] = true;
    }

    auto block = &unit.blocks[b];
    assertx(!block->code.empty());

    auto stopIdx = find_first_invalid_block_index(*block);

    // Walk through this block, inserting ldimms immediately before a constant's
    // use for the constants we decided to materialize here.
    for (size_t i = 0; i < stopIdx; ++i) {
      placeThisInst.reset();
      if (i == stopIdx - 1) {
        // We reached the end of the block. Any constants remaining are ones we
        // couldn't sink to all predecessors. Materialize them here at the end.
        placeThisInst |= place;
      } else {
        placeThisInst |= (place & uses_set_cached(state, block->code[i]));
      }

      if (placeThisInst.none()) continue;

      auto const materialize = [&] (Vout& v, Vreg r) {
        auto const it = unit.regToConst.find(r);
        assertx(it != unit.regToConst.end());
        auto const& vconst = it->second;
        switch (vconst.kind) {
          case Vconst::Quad:
          case Vconst::Double:
            if (vconst.isUndef) {
              v << ldundefq{r};
            } else {
              v << ldimmq{uint64_t(vconst.val), r};
            }
            break;
          case Vconst::Long:
            assertx(!vconst.isUndef);
            v << ldimml{int32_t(vconst.val), r};
            break;
          case Vconst::Byte:
            assertx(!vconst.isUndef);
            v << ldimmb{uint8_t(vconst.val), r};
            break;
        }
        ++i;
        ++stopIdx;
      };

      vmodify(
        unit, b, i,
        [&] (Vout& v) {
          for (auto const r : placeThisInst) materialize(v, r);
          return 0;
        }
      );
      block = &unit.blocks[b];
      place -= placeThisInst;
      if (place.none()) break;
    }

    assertx(place.none());
  }

  // Constant registers no longer exist!
  unit.regToConst.clear();
  unit.constToReg.clear();

  // Attempt to hoist constants
  if (withConsts.any()) hoist_constants(state, withConsts);

  return {std::move(multiplePlaced), std::move(withConsts)};
}

//////////////////////////////////////////////////////////////////////

// Make every phijmp operand unique so phi lowering and penalty biasing
// can reason about each edge independently.
void split_phijmp_duplicates(State& state) {
  auto& unit = state.unit;

  for (auto const b : state.rpo) {
    auto& block = unit.blocks[b];
    if (block.code.empty()) continue;
    auto& inst = block.code.back();
    if (inst.op != Vinstr::phijmp) continue;

    auto& uses = unit.tuples[inst.phijmp_.uses];
    if (uses.size() < 2) continue;

    VregList copySrcs;
    VregList copyDsts;
    jit::fast_set<Vreg> seen;

    for (size_t i = 0; i < uses.size(); ++i) {
      auto const src = uses[i];
      if (seen.insert(src).second) continue;

      auto const tmp = unit.makeReg();
      uses[i] = tmp;
      copySrcs.emplace_back(src);
      copyDsts.emplace_back(tmp);

      if (!src.isPhys() && src < state.regInfo.size()) {
        auto const& infoPtr = state.regInfo[src];
        if (infoPtr) reg_info_create(state, tmp) = *infoPtr;
      }
    }

    if (copySrcs.empty()) continue;
    invalidate_cached_operands(inst);
    vmodify(
      unit, b, block.code.size() - 1,
      [&] (Vout& v) {
        if (copySrcs.size() == 1) {
          v << copy{copySrcs[0], copyDsts[0]};
        } else {
          v << copyargs{
            unit.makeTuple(std::move(copySrcs)),
            unit.makeTuple(std::move(copyDsts))
          };
        }
        return 0;
      }
    );
  }
}

void prepare_unit(State& state) {
  // Constant materialization requires knowing where any loops are.
  find_loops(state);

  // Materialize constants, which might result in a non-SSA unit.
  auto results = place_constants(state);
  auto const& toSSA = results.first;
  auto& withConsts = results.second;

  if (toSSA.none()) return;
  assertx(withConsts.size() == state.unit.blocks.size());

  // Restore SSA if necessary and create RegInfo for the new Vregs,
  // mirroring the info of the original Vregs (which have been
  // rewritten).
  auto const mappings =
    restoreSSA(state.unit, toSSA, withConsts, state.rpo, state.preds, 0);
  for (auto const& map : mappings) {
    if (!map.second.isPhys()) continue;
    reg_info_create(state, map.first);
  }

  split_phijmp_duplicates(state);
}

//////////////////////////////////////////////////////////////////////
// Register class inference

/*
 * Vregs themselves don't inherently have a class. That is, they're not
 * inherently GP or SIMD. Instead the set of physical registers they can be
 * allocated depends solely on their usages and defs. The instructions put
 * constraints on their operands which is used to infer the proper register
 * class.
 *
 * We need to infer such information here. We start by assuming that all Vregs
 * are unconstrained (RegClass::Any). We then examine all uses and defs of that
 * Vreg, constraining the RegClass as appropriate. If the constraints are
 * incompatible (for example, RegClass::GP and RegClass::SIMD), we insert a copy
 * between the incompatible points. Copies never constrain their sources or
 * dests, and it's assumed that a copy can deal with moving values between
 * register classes, so this is always safe.
 *
 * A register is wide if its source or dest is a Vreg128. Unlike other
 * constraints, wideness is propagated across copies (because otherwise the
 * source would be RegClass::SIMDWide, and the dest would be
 * RegClass::AnyNarrow, which is 64-bits, so we'd drop data). So a copy which
 * has a source or dest of RegClass::SIMDWide has the other operand as
 * RegClass::SIMDWide as well. Other than this wrinkle, the inference is flow
 * insensitive.
 *
 * RegClass::SF is meant to mark registers as being the flags register. We don't
 * attempt to allocate the flags register (there's only one), so it mainly
 * serves as a marker to ignore that register (it's compatible with nothing).
 *
 * RegClass::AnyNarrow means the Vreg is unconstrained (but is not wide). In
 * theory this means we're completely free to assign it either a GP or SIMD
 * register. Unfortunately the graph coloring algorithm cannot deal with a
 * constraint like this. Every Vreg has to either be selected from GPs or SIMDs,
 * not both. Therefore RegClass::AnyNarrow is a synonym for RegClass::GP right
 * now. We can do better than that. TODO (T37587676)
 *
 * Finally, we insert copies to resolve a few tricky cases. We ensure
 * that there's no copyargs between ignored and non-ignored
 * registers. We ensure that a copyargs only has all physical or all
 * virtual dests, not a mix of both. We ensure that a phidef/phijmp
 * pair doesn't copy between register classes, nor does it write to a
 * physical register. We resolve these cases by adding new virtual
 * registers as the dests, and then copying from those new registers
 * to the old dests. These restrictions make the spilling and phi
 * lowering logic simpler. We don't have to do this for copy2 because
 * unlike other copies it imposes a constraint and does not get
 * special treatment from the spiller.
 */

namespace detail {

// Turn Vreg constraint into RegClass generically
RegClass reg_class(Vreg)      { return RegClass::AnyNarrow; }
RegClass reg_class(Vreg64)    { return RegClass::GP; }
RegClass reg_class(Vreg32)    { return RegClass::GP; }
RegClass reg_class(Vreg16)    { return RegClass::GP; }
RegClass reg_class(Vreg8)     { return RegClass::GP; }
RegClass reg_class(VregDbl)   { return RegClass::SIMD; }
RegClass reg_class(Vreg128)   { return RegClass::SIMDWide; }
RegClass reg_class(VregSF)    { return RegClass::SF; }
RegClass reg_class(PhysReg r) {
  switch (r.type()) {
    case PhysReg::GP:   return RegClass::GP;
    case PhysReg::SIMD: return RegClass::SIMD;
    case PhysReg::SF:   return RegClass::SF;
  }
  always_assert(false);
}

}

void infer_register_classes(State& state) {
  auto& unit = state.unit;

  // Vregs which have incompatible uses/defs. Copies must be inserted to resolve
  // this.
  VregSet incompatible;

  // Returns the common RegClass between c1 and c2, or std::nullopt if none
  // exists.
  auto const merge = [] (RegClass c1,
                         RegClass c2) -> Optional<RegClass> {
    // We shouldn't be mixing SF with anything
    assertx((c1 == RegClass::SF) == (c2 == RegClass::SF));
    if (c1 == RegClass::Any) return c2;
    if (c2 == RegClass::Any) return c1;
    if (c1 == c2) return c1;
    if (c1 == RegClass::AnyNarrow &&
        (c2 == RegClass::GP || c2 == RegClass::SIMD || c2 == RegClass::SIMDWide)) {
      return c2;
    }
    if (c2 == RegClass::AnyNarrow &&
        (c1 == RegClass::GP || c1 == RegClass::SIMD || c1 == RegClass::SIMDWide)) {
      return c1;
    }
    return std::nullopt;
  };

  auto haveWide = false;

  auto const updateDefs = [&] (auto r) {
    if (r.isPhys()) return;
    auto& info = reg_info_create(state, r);
    assertx(info.regClass == RegClass::Any);
    info.regClass = detail::reg_class(r);
    haveWide |= (info.regClass == RegClass::SIMDWide);
  };

  auto const updateUses = [&] (auto r) {
    if (r.isPhys()) return;
    auto& info = reg_info_create(state, r);
    auto const newClass = detail::reg_class(r);
    if (auto const c = merge(info.regClass, newClass)) {
      info.regClass = *c;
      haveWide |= (info.regClass == RegClass::SIMDWide);
    } else {
      incompatible.add(r);
    }
  };

  auto const processInst = [&] (const Vinstr& inst, bool skipDefs = false) {
    // NB: We can't use the cached operands here because we need to
    // know the specific kind of Vreg.
    visitUses(unit, inst, updateUses);
    if (!skipDefs) visitDefs(unit, inst, updateDefs);
  };

  // Emit a set of copies as either a single copy or a copyargs
  // instruction. It's guaranteed to only ever emit a single
  // instruction.
  auto const addCopies = [&] (const jit::vector<std::pair<Vreg, Vreg>>& copies,
                              Vlabel b,
                              size_t i) {
    vmodify(
      unit, b, i,
      [&] (Vout& v) {
        if (copies.size() == 1) {
          v << copy{copies[0].first, copies[0].second};
        } else {
          VregList src;
          VregList dst;
          for (auto const& p : copies) {
            src.emplace_back(p.first);
            dst.emplace_back(p.second);
          }
          v << copyargs{
            unit.makeTuple(std::move(src)),
            unit.makeTuple(std::move(dst))
          };
        }
        return 0;
      }
    );
  };

  // Check that a phi or a copyargs instruction has distinct dst registers.
  // We allow rvmfp() as a special case because we sometimes phi FramePtrs but
  // we always assign them to the PhysReg rvmfp().
  DEBUG_ONLY auto const checkDistinctDsts = [] (const VregList& dsts) -> bool {
    VregSet defs;
    for (auto i = 0; i < dsts.size(); ++i) {
      if (dsts[i] == rvmfp()) continue;
      always_assert(!defs[dsts[i]]);
      defs.add(dsts[i]);
    }
    return true;
  };

  // Insert copies as necessary to ensure that a copyargs instruction
  // does not copy between a non-ignored register and an ignored
  // one. Also ensure that a copyargs only has all physical registers
  // as dests, or only all virtual ones. This makes the spilling logic
  // easier. Return true if anything was changed.
  auto const canonicalizeCopyArgs = [&] (Vlabel b, size_t instIdx) {
    auto& inst = unit.blocks[b].code[instIdx];
    if (inst.op != Vinstr::copyargs) return false;

    // If the source is an ignored register, we insert a copy before.
    jit::fast_map<Vreg, Vreg> precopy;
    // If the dest is an ignored register, we insert a copy after.
    jit::fast_map<Vreg, Vreg> postcopy;

    auto& srcs = unit.tuples[inst.copyargs_.s];
    auto& dsts = unit.tuples[inst.copyargs_.d];
    assertx(srcs.size() == dsts.size());
    assertx(checkDistinctDsts(dsts));

    for (size_t i = 0; i < srcs.size(); ++i) {
      auto const s = srcs[i];
      auto const d = dsts[i];
      if (is_ignored(state, s) == is_ignored(state, d)) continue;
      if (is_ignored(state, s)) {
        auto it = precopy.find(s);
        // The copyargs can take the same register multiple times as a source,
        // so re-use the same copied register for that.
        if (it == precopy.end()) it = precopy.emplace(s, unit.makeReg()).first;
        srcs[i] = it->second;
        invalidate_cached_operands(inst);
      }
      if (is_ignored(state, d)) {
        // Dests should be unique, however.
        auto const result = postcopy.emplace(d, unit.makeReg());
        assertx(result.second);
        dsts[i] = result.first->second;
        invalidate_cached_operands(inst);
      }
    }

    // Insert the copies at the appropriate places.
    if (!precopy.empty()) {
      vmodify(
        unit, b, instIdx,
        [&] (Vout& v) {
          for (auto const& p : precopy) v << copy{p.first, p.second};
          return 0;
        }
      );
    }

    if (!postcopy.empty()) {
      vmodify(
        unit, b, instIdx + precopy.size() + 1,
        [&] (Vout& v) {
          for (auto const& p : postcopy) v << copy{p.second, p.first};
          return 0;
        }
      );
    }

    // If we added copies, return true. This will get called again and
    // we might handle the below case if necessary.
    if (!precopy.empty() || !postcopy.empty()) return true;

    // Ensure the dests are all physical registers or all virtual
    // ones. If not, add new virtual registers as dests, then do a
    // copy from those to the physical registers.
    auto foundPhys = false;
    auto foundNonPhys = false;
    for (size_t i = 0; i < srcs.size(); ++i) {
      auto const s = srcs[i];
      auto const d = dsts[i];
      assertx(is_ignored(state, s) == is_ignored(state, d));
      if (is_ignored(state, s)) continue;
      if (d.isPhys()) {
        foundPhys = true;
      } else {
        foundNonPhys = true;
      }
    }
    if (!foundPhys || !foundNonPhys) return false;

    jit::vector<std::pair<Vreg, Vreg>> physCopies;
    for (size_t i = 0; i < srcs.size(); ++i) {
      auto const d = dsts[i];
      if (!d.isPhys()) continue;
      auto const newReg = unit.makeReg();
      physCopies.emplace_back(newReg, d);
      dsts[i] = newReg;
      invalidate_cached_operands(inst);
    }
    assertx(!physCopies.empty());
    addCopies(physCopies, b, instIdx + 1);
    return true;
  };

  // First the flow insensitive part. Visit every instruction, using the uses
  // and defs to determine the register class.
  for (auto const b : state.rpo) {
    for (size_t i = 0; i < unit.blocks[b].code.size();) {
      // Deal with strange copyargs. If we changed anything, loop
      // without advancing i, so we'll process this copyargs again
      // (until we stop needing to change it).
      if (canonicalizeCopyArgs(b, i)) continue;
      // Analyze the use and defs. If there's no incompatibilities we're done.
      processInst(unit.blocks[b].code[i]);
      if (incompatible.none()) {
        ++i;
        continue;
      }

      // Otherwise we need to insert a copy before the use.
      jit::vector<std::pair<Vreg, Vreg>> copies;
      for (auto const r : incompatible) {
        assertx(!is_ignored(state, r));
        auto const newReg = unit.makeReg();
        // Clone the register, except for the RegClass, which is automatically
        // Any because it's coming from a copy. We'll reprocess this
        // instruction to set the RegClass appropriately.
        auto& newInfo = reg_info_create(state, newReg);
        newInfo = reg_info(state, r);
        newInfo.regClass = RegClass::Any;
        copies.emplace_back(r, newReg);
      }
      assertx(!copies.empty());
      addCopies(copies, b, i);
      // We've inserted a copy, so adjust the instruction index.
      ++i;

      // Rewrite the instruction to take the new (from copy) register.
      visitRegsMutable(
        unit, unit.blocks[b].code[i],
        [&] (Vreg r) {
          if (!incompatible[r]) return r;
          for (auto const& p : copies) {
            if (p.first == r) return p.second;
          }
          always_assert(false);
        },
        [&] (Vreg r) { return r; }
      );
      invalidate_cached_operands(unit.blocks[b].code[i]);

      // Now process the copy and reprocess the instruction. This will set the
      // RegClass of the new (from copy) registers properly.
      incompatible.reset();
      processInst(unit.blocks[b].code[i-1]);
      processInst(unit.blocks[b].code[i], true);
      // Reprocessing shouldn't create new incompatibilities.
      assertx(incompatible.none());
      ++i;
    }
  }

  // Now for the flow sensitive part. If either side of a copy is a wide
  // register, the other side must be forced to be wide as well. Only wideness
  // is propagated across copies in this way. We only propagate wideness if the
  // other register is RegClass::AnyNarrow (unconstrained non-wide). If there's
  // an actual constraint, we want to honor that (it means the code is
  // attempting to move a 128-bit register to a 64-bit one or vice-versa). If we
  // didn't see any wide RegClass already, we can skip this. Since this is rare,
  // we simply loop to a fixed point rather than try a worklist.
  auto changed = haveWide;
  while (changed) {
    changed = false;

    auto const propagateWide = [&] (Vreg s, Vreg d) {
      if (s.isPhys() || d.isPhys()) return false;
      auto& sInfo = reg_info(state, s);
      auto& dInfo = reg_info(state, d);
      if (sInfo.regClass == RegClass::SIMDWide &&
          dInfo.regClass == RegClass::AnyNarrow) {
        dInfo.regClass = RegClass::SIMDWide;
        return true;
      } else if (sInfo.regClass == RegClass::AnyNarrow &&
                 dInfo.regClass == RegClass::SIMDWide) {
        sInfo.regClass = RegClass::SIMDWide;
        return true;
      }
      return false;
    };

    for (auto const b : state.rpo) {
      auto const& block = unit.blocks[b];
      for (auto const& inst : block.code) {
        if (inst.op == Vinstr::copy) {
          changed |= propagateWide(inst.copy_.s, inst.copy_.d);
        } else if (inst.op == Vinstr::copyargs) {
          auto const& s = unit.tuples[inst.copyargs_.s];
          auto const& d = unit.tuples[inst.copyargs_.d];
          assertx(s.size() == d.size());
          for (size_t i = 0; i < s.size(); ++i) {
            changed |= propagateWide(s[i], d[i]);
          }
        } else if (inst.op == Vinstr::phidef) {
          // Phis are treated as copies in this context
          auto const& d = unit.tuples[inst.phidef_.defs];
          for (auto const pred : state.preds[b]) {
            auto const& phijmp = unit.blocks[pred].code.back();
            assertx(phijmp.op == Vinstr::phijmp);
            auto const& s = unit.tuples[phijmp.phijmp_.uses];
            assertx(s.size() == d.size());
            for (size_t i = 0; i < s.size(); ++i) {
              changed |= propagateWide(s[i], d[i]);
            }
          }
        }
      }
    }
  }

  // Finally we have to ensure that the src/dest pairs of a phi have
  // the same register class. If not, we'll ensure they do by (yet
  // again) inserting a copy before the phi of the old src to a new
  // Vreg which has the same register class of the dest. The new Vreg
  // will then be the src in the phi. We also disallow a phi from
  // having a physical register as dest. We need to do all of this
  // because having to deal with moves between register classes during
  // a parallel phi copy greatly complicates the spilling logic and
  // the phi lowering logic.
  for (auto const b : state.rpo) {
    auto& firstInst = unit.blocks[b].code.front();
    if (firstInst.op == Vinstr::phidef) {
      auto& d = unit.tuples[firstInst.phidef_.defs];
      assertx(checkDistinctDsts(d));

      // Check for any physical registers as dests. If any, set a new
      // virtual register as the dest, and emit a copy from it to the
      // physical register.
      jit::vector<std::pair<Vreg, Vreg>> copies;
      for (size_t i = 0; i < d.size(); ++i) {
        if (is_ignored(state, d[i])) continue;
        if (!d[i].isPhys()) continue;
        auto const newReg = unit.makeReg();
        auto const dCls = reg_class(state, d[i]);
        assertx(dCls != RegClass::SF);
        reg_info_insert(state, newReg, RegInfo{dCls});
        copies.emplace_back(newReg, d[i]);
        d[i] = newReg;
        invalidate_cached_operands(firstInst);
      }

      if (!copies.empty()) addCopies(copies, b, 1);
    }

    auto& lastInst = unit.blocks[b].code.back();
    if (lastInst.op == Vinstr::phijmp) {
      auto& s = unit.tuples[lastInst.phijmp_.uses];
      auto const successorList = succs(unit.blocks[b]);
      assertx(successorList.size() == 1);
      auto const& phidef = unit.blocks[successorList[0]].code.front();
      assertx(phidef.op == Vinstr::phidef);
      auto const& d = unit.tuples[phidef.phidef_.defs];
      assertx(s.size() == d.size());

      jit::vector<std::pair<Vreg, Vreg>> copies;
      jit::vector<std::pair<Vreg, Vreg>> ignoredCopies;
      for (size_t i = 0; i < s.size(); ++i) {
        // We don't handle phis between an ignored and non-ignored
        // register right now. We could, but it complicates things. If
        // a phi takes an ignored register as a source, but the dest
        // is non-ignored, insert a copy before the phi to make both
        // sides match.
        if (is_ignored(state, s[i]) && !is_ignored(state, d[i])) {
          auto const dCls = reg_class(state, d[i]);
          assertx(dCls != RegClass::SF);
          auto const newReg = unit.makeReg();
          reg_info_insert(state, newReg, RegInfo{dCls});
          ignoredCopies.emplace_back(s[i], newReg);
          s[i] = newReg;
          invalidate_cached_operands(lastInst);
          continue;
        }
        // However, don't allow the opposite case (non-ignored to
        // ignored) since it doesn't occur right now
        assertx(is_ignored(state, s[i]) == is_ignored(state, d[i]));
        // Since we allow rvmfp() to appear multiple times in a
        // phidef, it's not really clear how to properly emit code for
        // them. This isn't an issue if the source and dests are
        // always the same, which always happens now, so assert that
        // this remains the case.
        assertx((s[i] == rvmfp()) == (d[i] == rvmfp()));
        if (is_ignored(state, s[i])) continue;

        auto const sCls = reg_class(state, s[i]);
        auto const dCls = reg_class(state, d[i]);
        if (sCls == dCls && !s[i].isPhys()) continue;
        assertx(sCls != RegClass::SF && dCls != RegClass::SF);

        // Create a new Vreg in the same RegClass as the dest and use that in
        // the phi instead. Add a copy between the old src and the new
        // Vreg. This lets the move between register classes be done outside of
        // the phi, instead of during it.
        auto const newReg = unit.makeReg();
        reg_info_insert(state, newReg, RegInfo{dCls});

        copies.emplace_back(s[i], newReg);
        s[i] = newReg;
        invalidate_cached_operands(lastInst);
      }

      // The copies between ignored and non-ignored registers need to
      // each happen separately (we don't want them in a copyargs), so
      // do them one by one.
      for (auto const& copy : ignoredCopies) {
        addCopies({copy}, b, unit.blocks[b].code.size() - 1);
      }
      if (!copies.empty()) {
        addCopies(copies, b, unit.blocks[b].code.size() - 1);
      }
    }
  }

  // Now that we've inferred reg-classes, populate the VregSets in
  // state as necessary.
  state.gps.add(state.gpUnreserved);
  state.simds.add(state.simdUnreserved);
  state.flags.add(state.abi.sf);
  for (size_t i = 0; i < state.regInfo.size(); ++i) {
    auto const& info = state.regInfo[i];
    if (!info) continue;
    set_reg_class_bits(state, Vreg{i}, info->regClass);
  }
}

//////////////////////////////////////////////////////////////////////
// Spilling

/*
 * Spilling is by far the most complicated part of the register allocator. It is
 * responsible for inserting Vinstr::spill and Vinstr::reload instructions to
 * move Vregs to/from spill slots in memory. Non-copy instructions have to have
 * their inputs in registers (not in memory) at the point they're executed, but
 * we can never have more Vregs in registers than there are actually physical
 * registers.
 *
 * Once spilling is accomplished then the unit is guaranteed to be trivially
 * colorable. It's guaranteed that at all points in the unit there's never more
 * Vregs in registers than there are physical registers, and therefore there'll
 * always be a free physical register to select. This property is completely
 * independent of the colors you choose and thus means the actual color
 * selection is arbitrary (ignoring physical register constraints).
 *
 * The key behind this algorithm is that in a strict SSA program, the number of
 * needed physical registers at any point is equal to the number of live values
 * (Vregs here). This is just a restatement of the property that the chromatic
 * number of chordal interference graphs is equal to the largest clique. Each
 * clique of size N in the interference graph represents a point where N values
 * are simultaneously alive.
 *
 * Since we have a fixed number of physical registers, it follows that the unit
 * can only be colored if we can reduce (at every point) the number of live
 * Vregs to below that number. Of course we cannot make Vregs be not live, since
 * that affects program semantics, but we can spill them, which means they don't
 * occupy a register. This logic does *not* apply for non-SSA programs.
 *
 * The spilling is accomplished by walking the unit block by block. We keep
 * per-block state of which Vregs are in registers (not-spilled), and which are
 * in memory (spilled). At the entry of each block we first calculate the
 * initial state. That is, we decide which live-in Vregs should be in registers
 * and which in memory initially. For loop headers we apply extra logic to avoid
 * spilling Vregs used within the loop. Every block is free to make its own
 * independent decision in this regard. It can lead to blocks disagreeing about
 * where a Vreg should be, which will be fixed up afterwards.
 *
 * Once the block state is initialized, the block is walked instruction by
 * instruction. We force all the Vregs an instruction uses out of memory (if
 * necessary), generating reload instructions. This might increase the number of
 * Vregs in registers past the limit, so it may trigger spilling other Vregs
 * into memory, restoring the balance (generating spill instructions). We then
 * purge any uses which are now dead out of the state. We then record the defs
 * of the instruction as being in registers (since the instruction has to write
 * to registers). This may generate yet more spills. Finally we purge any defs
 * which are now dead (because they're unused). The decision of which Vregs to
 * actually spill is performed by a heuristic which uses a calculated "spill
 * weight" for each Vreg.
 *
 * This process is repeated for every instruction, generating reload and spill
 * instructions along the way. When we reach the end of the block, we record the
 * out state. If the block ends in a phijmp, we also record the state of the
 * Vregs used by the phi.
 *
 * Copies get special treatment. Copy instructions are special in that they can
 * handle both spilled and non-spilled Vregs (no reload required). This is
 * actually required because we need to be able to copy spilled Vregs around
 * without adding new register pressure. We do, however, require that the source
 * and dest of the copy agree with regards to whether the Vreg is spilled or
 * not. We may have to introduce extra instructions to guarantee this.
 *
 * Once this is all done, each block is self-consistent, but the blocks may not
 * agree amongst themselves about which Vregs are spilled or not. IE, one block
 * may have decided that it would assume that a live-in Vreg is in a register,
 * but its predecessor may have decided to spill this. We resolve this by
 * inserting spills or reloads at the end of the predecessor as necessary. A
 * similar situation may happen with phijmp/phidef pairs.
 *
 * The actual spill and reload instructions are kept until after coloring and
 * will be removed as part of SSA lowering.
 *
 * We can use register pressure calculations to determine which blocks can
 * actually trigger a spill. If the register pressure within a block never
 * exceeds the number of available registers, we know a spill will never be
 * generated. For these blocks, we can update the spilling state particularly
 * efficiently (it's just the live-out Vregs all in registers). In the (common)
 * case where no blocks will cause a spill, we can skip the spiller entirely.
 */

/*
 * Materialization:
 *
 * When attempting to reload a spilled Vreg, we can instead
 * rematerialize the value in some cases. We look backwards from the
 * current Vreg use to its definition(s). We chain through copyish
 * instructions (including phis, which is how we can have multiple
 * definitions). If the definition is pure, and any sources in the
 * definition are still available, we can re-emit the definition
 * instead of a reload. If there are multiple definitions, they must
 * be all equivalent (same op and sources). If we successfully
 * rematerialize a Vreg, we should (hopefully) have a spill with no
 * associated reloads, which will be cleaned up by dead code
 * elimination. To keep the lookups efficient, we lazily cache the
 * definitions as we encounter them.
 *
 * This lookup is somewhat complicated by mutually recursive
 * definitions. That is, a Vreg defined by a phi whose Vreg input is
 * defined (ultimately) by the original Vreg. When doing a lookup, it's
 * safe to ignore such recursive definitions (as long as there's at
 * least one non-recursive definition). However, it's not safe to cache
 * the definitions of intermediate Vregs in such cases. This is
 * because their definition might change in later lookups (because
 * you're at a different program point). Since we use the cached
 * definitions to avoid redundant work (otherwise we could have an
 * exponential blow up in paths visited), we cannot just not cache
 * them. Instead we cache them, but clear them at the end of the
 * lookup (it's safe to use them during the same particular lookup). We
 * store a special nop instruction in the cache to indicate we're
 * currently doing a lookup for that Vreg, which is how we detect
 * recursion.
 */

// Is this instruction a (simple) memory read?
bool is_mem_read(const Vinstr& inst) {
  // We're looking for instructions which read from memory and nothing
  // else.
  return
    !isPure(inst) &&
    !isCall(inst) &&
    touchesMemory(inst.op) &&
    !writesMemory(inst.op);
}

// Comparators for instruction source types
namespace detail {

// Make these all types explicit (no default case) so that we get compile errors
// if we have a new source type.
bool src_cmp(const Vunit&, Vreg r1, Vreg r2)     { return r1 == r2; }
bool src_cmp(const Vunit&, Vptr r1, Vptr r2)     { return r1 == r2; }
bool src_cmp(const Vunit&, RegSet r1, RegSet r2) { return r1 == r2; }
bool src_cmp(const Vunit& unit, Vtuple t1, Vtuple t2) {
  return unit.tuples[t1] == unit.tuples[t2];
}
bool src_cmp(const Vunit& unit, VcallArgsId a1, VcallArgsId a2) {
  return unit.vcallArgs[a1] == unit.vcallArgs[a2];
}

}

// Check if two instructions are equal (modulo the def Vreg). Used to possibly
// merge together different rematerialization candidates at join points.
bool compare_remat_insts(const Vunit& unit,
                         const Vinstr& inst1,
                         const Vinstr& inst2) {
  if (&inst1 == &inst2) return true;
  if (inst1.op != inst2.op) return false;

  switch (inst1.op) {
#define O(name, imms, uses, ...)                       \
    case Vinstr::name: {                               \
      auto const& i1 = inst1.name##_; (void)i1;        \
      auto const& i2 = inst2.name##_; (void)i2;        \
      imms                                             \
      uses                                             \
      break;                                           \
    }
#define I(f)    if (i1.f != i2.f) return false;
#define U(s)    if (!detail::src_cmp(unit, i1.s, i2.s)) return false;
#define UA(s)   if (!detail::src_cmp(unit, i1.s, i2.s)) return false;
#define UH(s,h) if (!detail::src_cmp(unit, i1.s, i2.s)) return false;
#define UM(s)   if (!detail::src_cmp(unit, i1.s, i2.s)) return false;
#define UW(s)   if (!detail::src_cmp(unit, i1.s, i2.s)) return false;
#define Inone
#define Un
    VASM_OPCODES
#undef Un
#undef Inone
#undef UW
#undef UM
#undef UH
#undef UA
#undef U
#undef I
#undef O
  }

  // Special case: If these instructions (which now know are
  // identical) represent a memory read, they must have an identical
  // origin, or at least the same PureLoad memory effects. It's
  // possible for two vasm instructions to be identical, but represent
  // different memory effects. Once we canonicalize to a single
  // defining instruction, we'll lose one of the memory effects or the
  // other, and we won't be able to calculate potential write
  // conflicts properly. So, if this is a memory read, extend the
  // notion of equality to their memory effects as well.
  if (inst1.origin != inst2.origin && is_mem_read(inst1)) {
    if (!inst1.origin || !inst2.origin) return false;
    auto const effects1 = memory_effects(*inst1.origin);
    auto const effects2 = memory_effects(*inst2.origin);
    auto const load1 = std::get_if<PureLoad>(&effects1);
    auto const load2 = std::get_if<PureLoad>(&effects2);
    if (!load1 || !load2) return false;
    if (load1->src != load2->src) return false;
  }

  return true;
}

// Emit an instruction for a reload or rematerialization, setting the
// appropriate origin. For normal reload instruction, we use the
// current origin in the Vout. For rematerialization instructions we
// want to preserve the origin of the remat instruction (this is
// important to keep memory effects correct).
void emit_reload(Vout& v, const Vinstr& inst) {
  if (inst.op == Vinstr::reload) {
    v << inst;
    return;
  }
  auto const old = v.getOrigin();
  v.setOrigin(inst.origin);
  v << inst;
  v.setOrigin(old);
}

// The result of a defining instruction for rematerialization lookup:
struct RematLookup {
  // The defining instruction. Might be reload if no single one can be
  // found, or nop if recursion is detected. This instruction might
  // not necessarily be suitable for reloading at the current program
  // point (or ever).
  Vinstr inst;
  // Whether any recursion definitions were detected while looking up
  // this instruction. Any (non-reload) instructions obtained this way
  // should not be cached across different lookups, as their result
  // may change at different program points.
  bool recursive;
  // The block where the defining instruction was sourced from. This
  // is only meaningful for non-reload instructions which have
  // physical register sources.
  Vlabel block;
};

RematLookup find_defining_inst_for_remat_cached(State&,
                                                Vreg,
                                                Vlabel,
                                                size_t,
                                                VregSet&);

// Given a Vreg, search backwards in the unit to find its defining
// instruction. The search is started at the specified block and
// instruction index. Copyish instructions (including phis) are
// "transparent" to this search. If the Vreg has multiple definitions,
// and they are all equivalent, return an arbitrary one. If they are
// not equivalent, or if the definition cannot be found, return a
// reload instruction. A nop instruction may be returned if all of the
// definitions are mutually recursive. The VregSet is used to mark
// which Vregs in the cache have a definition involving mutual
// recursion. Such cached entries will be removed after the lookup is
// finished.
RematLookup find_defining_inst_for_remat(State& state,
                                         Vlabel b,
                                         size_t instIdx,
                                         const Vreg startR,
                                         VregSet& recursives) {
  auto& unit = state.unit;

  auto const compatible = [&] (Vreg r1, Vreg r2) {
    return compatible_reg_classes(
      reg_info(state, r1).regClass,
      reg_info(state, r2).regClass
    );
  };

  auto const fail = [&] {
    return RematLookup{ reload{startR, startR}, false, b };
  };

  // Record whether we passed through a copyish instruction where the
  // source and dest were not compatible (different register
  // classes). If so, we'll only report the defining instruction if
  // it's an immediate load. The copyish instruction may actually
  // change the value if they're incompatible (it could truncate or
  // zero-extend). The reason we still allow immediate loads is
  // because we often materialize constants into a different register
  // class than how it's ultimately used.
  auto incompatibility = false;

  auto const if_compatible = [&] (const Vinstr& inst) {
    return incompatibility ? fail() : RematLookup{inst, false, b};
  };

  auto r = startR;
  while (true) {
    assertx(!r.isPhys());

    // First find the correct block:
    //
    // From the current block, walk backwards through the predecessors
    // until we encounter a block where the desired Vreg is *not*
    // live-in. Since it's live-in for its successors, this means it
    // must be defined in that block. This lets us skip over most
    // blocks without examining their instructions.
    while (state.liveIn[b][r]) {
      auto const& preds = state.preds[b];
      assertx(!preds.empty());

      // If a block has multiple predecessors, choose the one with the
      // lowest RPO order. Not only does this prevent us from getting
      // loops, but it helps us find the def faster.
      auto bestOrder = state.rpoOrder[preds[0]];
      auto bestPred = preds[0];
      assertx(state.liveOut[preds[0]][r]);
      for (size_t i = 1; i < preds.size(); ++i) {
        auto const pred = preds[i];
        assertx(state.liveOut[pred][r]);
        if (state.rpoOrder[pred] < bestOrder) {
          bestOrder = state.rpoOrder[pred];
          bestPred = pred;
        }
      }
      // Should always decrease
      assertx(state.rpoOrder[bestPred] < state.rpoOrder[b]);
      b = bestPred;
      assertx(!unit.blocks[b].code.empty());
      instIdx = unit.blocks[b].code.size();
    }

    // Now that we have the correct block, find the correct
    // instruction within it.
    auto& block = unit.blocks[b];
    while (true) {
      always_assert_flog(
        instIdx > 0,
        "Cannot find a def for {} in {}",
        show(r), b
      );
      --instIdx;
      auto& inst = block.code[instIdx];

      // Quick check if this instruction defs the Vreg
      if (!defs_set_cached(state, inst)[r]) continue;

      // Process each instruction. If it's copyish, change the tracked
      // Vreg to be the source. We treat reload/spill/ssaalias as
      // copyish here because they don't change the actual value in
      // the Vreg.

      switch (inst.op) {
        case Vinstr::copy:
          assertx(inst.copy_.d == r);
          // A copy from a physical register is not considered copyish
          // (we cannot continue because physical registers are not in
          // SSA form, thus this algorithm won't work to find its next
          // def). So, use this as the defining instruction.
          if (inst.copy_.s.isPhys()) return if_compatible(inst);
          if (!compatible(inst.copy_.s, r)) incompatibility = true;
          r = inst.copy_.s;
          break;
        case Vinstr::reload:
          assertx(inst.reload_.d == r);
          r = inst.reload_.s;
          break;
        case Vinstr::spill:
          if (inst.spill_.s.isPhys()) {
            // We cannot return a spill here as the rematerialization
            // instruction. We can just copy from the source of the
            // spill.
            return if_compatible(copy{inst.spill_.s, inst.spill_.d});
          }
          assertx(inst.spill_.d == r);
          r = inst.spill_.s;
          break;
        case Vinstr::ssaalias:
          assertx(inst.ssaalias_.d == r);
          r = inst.ssaalias_.s;
          break;
        case Vinstr::copyargs: {
          auto const& dsts = unit.tuples[inst.copyargs_.d];
          auto const& srcs = unit.tuples[inst.copyargs_.s];
          assertx(srcs.size() == dsts.size());
          auto DEBUG_ONLY found = false;
          for (size_t i = 0; i < dsts.size(); ++i) {
            if (dsts[i] != r) continue;
            if (srcs[i].isPhys()) return fail();
            if (!compatible(srcs[i], r)) incompatibility = true;
            r = srcs[i];
            if (debug) found = true;
            break;
          }
          // We should always find something because of the defs set
          // check above.
          assertx(found);
          break;
        }
        case Vinstr::phidef: {
          // A phidef is copyish, but we may have to examine more than
          // one source, hence the need for recursion.

          // Phidef can only appear at the beginning of the block.
          assertx(instIdx == 0);

          auto const& dsts = unit.tuples[inst.phidef_.defs];
          auto const& preds = state.preds[b];

          for (size_t i = 0; i < dsts.size(); ++i) {
            if (dsts[i] != r) continue;

            // Found a def. Recursively find the defining instruction
            // for each predecessor Vreg.
            Optional<Vinstr> common;
            Vlabel defBlock;
            auto anyRecursive = false;
            for (auto const p : preds) {
              auto const& pred = unit.blocks[p];
              auto const& phijmp = pred.code.back();
              assertx(phijmp.op == Vinstr::phijmp);
              auto const s = unit.tuples[phijmp.phijmp_.uses][i];
              // Can't track phi-ing with a physical register
              if (s.isPhys()) return fail();
              auto const result = find_defining_inst_for_remat_cached(
                state,
                s,
                p,
                pred.code.size(),
                recursives
              );
              anyRecursive |= result.recursive;
              if (result.inst.op == Vinstr::reload) return fail();
              // Skip over recursive definitions
              if (result.inst.op == Vinstr::nop) {
                anyRecursive = true;
                continue;
              }
              if (!compatible(s, r) &&
                  result.inst.op != Vinstr::ldimmq &&
                  result.inst.op != Vinstr::ldimml &&
                  result.inst.op != Vinstr::ldimmb &&
                  result.inst.op != Vinstr::ldundefq) {
                return fail();
              }
              // All defining instructions should be compatible.
              if (!common) {
                common = result.inst;
                defBlock = result.block;
              } else if (!compare_remat_insts(unit, *common, result.inst)) {
                return fail();
              } else if (!phys_uses_set_cached(state, *common).empty()) {
                // The instructions are identical, but we require a
                // stricter condition for instructions which use
                // physical registers. Any rematerialization
                // instruction which uses physical registers must have
                // a single location. Otherwise we cannot guarantee
                // that the involved physical registers have the same
                // value(s) for all instructions. Non-physical Vregs
                // do not have this problem because they're in SSA and
                // thus only have one definition. Its possible to
                // arrive at the same instruction even when looking
                // through phis, so check that here.
                if (defBlock != result.block) return fail();
              }
            }
            // This can only happen if all of the sources were
            // recursively defined.
            if (!common) return {nop{}, true, b};
            return {*common, anyRecursive, defBlock};
          }

          // We should always find a matching def because of the defs
          // set check above.
          always_assert(false);
        }
        case Vinstr::ldimmq:
        case Vinstr::ldimml:
        case Vinstr::ldimmb:
        case Vinstr::ldundefq:
          // These can be used regardless of the incompatibility flag.
          return {inst, false, b};
        // Treat the spill immediate instructions as being their
        // equivalent ldimm instructions.
        case Vinstr::spillbi:
          return {ldimmb{inst.spillbi_.s, inst.spillbi_.d}, false, b};
        case Vinstr::spillli:
          return {ldimml{inst.spillli_.s, inst.spillli_.d}, false, b};
        case Vinstr::spillqi:
          return {ldimmq{inst.spillqi_.s.q(), inst.spillqi_.d}, false, b};
        case Vinstr::spillundefq:
          return {ldundefq{inst.spillundefq_.d}, false, b};
        default:
          // The rest can only be used if we didn't encounter any
          // incompatible copies.
          return if_compatible(inst);
      }

      // If we reach here, we encountered a copyish instruction which
      // changed what Vreg we're looking for. We need to find the
      // block which defines the new Vreg, so break out of the
      // instruction loop and restart the block search above.
      break;
    }
  }
}

// Similar to find_defining_inst_for_remat, but first checks if a
// cached entry for the Vreg exists. If not, it will call
// find_defining_inst_for_remat to obtain one, then cache it and
// return it. This function is mutually recursive with
// find_defining_inst_for_remat since they call each other.
RematLookup find_defining_inst_for_remat_cached(State& state,
                                                Vreg r,
                                                Vlabel b,
                                                size_t instIdx,
                                                VregSet& recursives) {
  // We cache rematerialization resolutions to avoid redundant
  // lookups. If there's no cached information for this Vreg,
  // calculate it by searching for a defining instruction. If the
  // instruction isn't found, or if it's always unusable (if it's not
  // pure, for example), we'll just store a reload instead.
  assertx(!r.isPhys());

  // Lookup the cached entry. If one exists, return it (along with
  // whether that entry came from a mutually recursive definition).
  auto& info = reg_info(state, r);
  if (info.cachedRemat) {
    return {info.cachedRemat->instr, recursives[r], info.cachedRemat->block};
  }
  assertx(!recursives[r]);

  // Otherwise we need to look it up. Store a nop in the cached entry
  // to catch mutual recursion, then look it up.
  info.cachedRemat = RematInfo{nop{}, b};
  auto result = find_defining_inst_for_remat(state, b, instIdx, r, recursives);
  // Nothing should have touched the cached entry.
  assertx(info.cachedRemat && info.cachedRemat->instr.op == Vinstr::nop);
  assertx(!recursives[r]);

  // Store the information in the cache, along with its recursive
  // status. Return the same data.
  auto const cache = [&] (const Vinstr& inst,
                          bool recursive,
                          Vlabel defBlock) -> RematLookup {
    info.cachedRemat = RematInfo{inst, defBlock};
    if (recursive) recursives.add(r);
    return {inst, recursive, defBlock};
  };

  // No single defining instruction found:
  if (result.inst.op == Vinstr::reload) return cache(reload{r, r}, false, b);
  // Entirely mutually recursive:
  if (result.inst.op == Vinstr::nop) return cache(nop{}, true, b);

  // Is this instruction at least possibly rematerializable?
  auto const acceptable = [&] {
    if (!isPure(result.inst)) {
      // Most non-pure instructions cannot be rematerialized. The
      // exception are "simple" mem reads which have the appropriate
      // associated memory effects.
      if (!is_mem_read(result.inst)) return false;
      // If there's no origin, we can't calculate memory effects and
      // must be conservative.
      if (!result.inst.origin) return false;
      // We can only (possibly) rematerialize loads which are pure and
      // read from a single (abstract) location. Anything more
      // complicated is beyond our abilities to track safely.
      auto const effects = memory_effects(*result.inst.origin);
      auto const load = std::get_if<PureLoad>(&effects);
      if (!load || !load->src.isSingleLocation()) return false;
    }
    // Can't rematerialization instructions which define any physical
    // registers or define more than one Vreg.
    if (!phys_defs_set_cached(state, result.inst).empty()) return false;
    if (defs_set_cached(state, result.inst).size() != 1) return false;
    return true;
  }();
  if (!acceptable) return cache(reload{r, r}, false, b);

  // Otherwise cache it and return it
  return cache(result.inst, result.recursive, result.block);
}

// Entry point for finding the defining instruction for
// rematerialization (the above two functions are implementation
// details). Given a Vreg, finding the defining instruction for that
// Vreg (at the given block/instruction) and return it. If a single
// defining instruction cannot be found, return reload instead.
RematInfo find_defining_inst_for_remat_enter(State& state,
                                             Vreg r,
                                             Vlabel b,
                                             size_t instIdx) {
  VregSet recursives;
  // Do the lookup
  auto const result =
    find_defining_inst_for_remat_cached(state, r, b, instIdx, recursives);

  // Remove any ephemeral entries in the cache from mutually recursive
  // definitions. They're not safe to keep across lookups, because their
  // results are incomplete. We still want to store them during a single
  // lookup to avoid exponential path behavior. The final result for `r`
  // is complete though, so keep it. This avoids quadratic lookup behavior
  // for large cycles.
  for (auto const recur : recursives) {
    if (recur == r) continue;
    assertx(!recur.isPhys());
    auto& info = reg_info(state, recur);
    assertx(info.cachedRemat);
    info.cachedRemat.reset();
  }

  // Treat an entirely mutually recursive definition as a reload,
  // which is safe.
  return result.inst.op == Vinstr::nop
    ? RematInfo{reload{r, r}, b}
    : RematInfo{result.inst, result.block};
}

// Return true if the given Vptr is "simple". That is, if it's nothing
// but a base plus offset without any scale or index.
bool is_simple_vptr(const Vptr& ptr) {
  if (ptr.seg != Segment::DS) return false;
  if (ptr.index != InvalidReg) return false;
  if (ptr.scale != 1) return false;
  return true;
}

// Given an instruction which writes to the given PhysReg, if the
// instruction modifies the PhysReg in a tracked way, return the
// inverse of that change. Return std::nullopt if the register is
// changed in an unpredictable way.
Optional<int64_t> tracked_physical_register_change(const State& state,
                                                   PhysReg r,
                                                   const Vinstr& inst,
                                                   Vlabel b) {
  auto const& unit = state.unit;

  // Right now we only support "simple" leas and copyish instructions
  // (which are modeled like leas of 0) between the same register.

  switch (inst.op) {
    case Vinstr::lea: {
      auto const& i = inst.lea_;
      assertx(i.d == r);
      if (i.s.base != r || !is_simple_vptr(i.s)) return std::nullopt;
      return -i.s.disp;
    }
    case Vinstr::copy:
      assertx(inst.copy_.d == r);
      if (inst.copy_.s != r) return std::nullopt;
      return 0;
    case Vinstr::copyargs: {
      auto const& srcs = unit.tuples[inst.copyargs_.s];
      auto const& dsts = unit.tuples[inst.copyargs_.d];
      assertx(srcs.size() == dsts.size());
      DEBUG_ONLY size_t defCount = 0;
      for (size_t i = 0; i < dsts.size(); ++i) {
        if (dsts[i] != r) continue;
        if (srcs[i] != r) return std::nullopt;
        ++defCount;
      }
      assertx(defCount > 0);
      return 0;
    }
    case Vinstr::phidef: {
      DEBUG_ONLY size_t defCount = 0;
      auto const& dsts = unit.tuples[inst.phidef_.defs];
      for (size_t i = 0; i < dsts.size(); ++i) {
        if (dsts[i] != r) continue;
        ++defCount;
        auto const& preds = state.preds[b];
        assertx(!preds.empty());
        for (auto const pred : preds) {
          auto const& phijmp = unit.blocks[pred].code.back();
          assertx(phijmp.op == Vinstr::phijmp);
          auto const& srcs = unit.tuples[phijmp.phijmp_.uses];
          assertx(srcs.size() == dsts.size());
          if (srcs[i] != r) return std::nullopt;
        }
      }
      assertx(defCount > 0);
      return 0;
    }
    default:
      return std::nullopt;
  }
}

// Determine if the physical register `phys' is changed in only
// recoverable ways on *all* paths from `currentBlock' and
// `startInstIdx' backwards to `targetInst' (which must define
// `target') in `targetBlock'. `startOffset' is the cumulative
// modifications to `phys' already seen along this path. `inOffsets'
// is used for store metadata during the calculation. `cleanup' will
// be populated with the cache entries (indicated by the Vlabel) which
// must be removed after the calculation (cannot be kept across
// calculations).

struct PhysRecoverableResult {
  // The statically known offset of the value of `phys' at the current
  // point to its value when 'target' was defined. std::nullopt if it's
  // changed in an unrecoverable way.
  Optional<int64_t> offset;
  // If the calculation of `offset' involved information from a
  // loop. This implies the result is ephemeral (can change across
  // calculations), and thus any caching of it cannot be kept across
  // calculations.
  bool recursive;
};

PhysRecoverableResult physical_register_is_recoverable(
    State& state,
    PhysReg phys,
    Vreg target,
    Vlabel targetBlock,
    Vinstr& targetInst,
    Vlabel currentBlock,
    size_t startInstIdx,
    int64_t startOffset,
    jit::vector<Optional<int64_t>>& inOffsets,
    jit::vector<Vlabel>& cleanup) {
  /*
   * Recursively explore every path backwards from `currentBlock' at
   * `startInstIdx' until we find our `targetInst'. If we encounter a
   * change of `phys' before hitting the instruction, we see if the
   * change modifies `phys' in a recoverable way. If so, we continue
   * onwards, accumulating the changes. Otherwise if the change is
   * unrecoverable, we return that `phys' is not recoverable. If we do
   * not encounter any such unrecoverable changes (or any changes at
   * all) along all paths, the register's original value can be
   * reconstructed. We track the accumulated offset (starting with 0
   * at the starting block), and push it to each predecessor. Once we
   * hit the target block, that accumulated offset is the
   * result. Unwinding the recursion is just returning that ultimate
   * result from the target block, and making sure the result is the
   * same at merge points.
   *
   * This is potentially expensive, so we imploy a number of
   * optimizations. First we utilize some pre-calculated metadata
   * about physical register definitions to avoid having to process
   * every instruction in a block, or even cut the search short
   * entirely. Second we cache the results at every block to speed up
   * subsequent lookups of the same information. Note: the cache
   * prevents us from a potential exponential path explosion (consider
   * a series of diamonds).
   *
   * It's possible to hit a loop and attempt to process a block we
   * already visited. We use the `inOffsets' vector to detect
   * this. When we enter a block, the `inOffsets' vector is checked to
   * see if an offset has been stored there. We record the
   * `startOffset' value in the `inOffsets' vector if not, and process
   * the block. If we loop around and attempt to process the block
   * again, we'll see an offset in the `inOffsets' vector. If our
   * `currentOffset' does not match what's stored in the vector, we
   * know the final result is std::nullopt (no recoverable value). This
   * mismatch means that there's two paths going into the block where
   * `phys' has a different offset, which means the offset isn't
   * consistent and thus not recoverable. If the offset matches the
   * one in `inOffsets', we return a std::nullopt offset with the
   * recursion flag set.
   *
   * We cannot return an offset in this case since the block hasn't
   * been done processing (because we know we've looped while
   * processing). Any attempt to continue processing the block will
   * just result in looping again. Instead we return the special
   * recursion value to indicate that this path should be ignored when
   * merging paths. When we return from processing a block fully, we
   * remove the `inOffsets' vector (and add a result to the cache).
   *
   * When merging results for multiple predecessors, we ignore any
   * that don't have an offset and have a recursion flag set (if they
   * don't have the flag set, no offset means a failure and we just
   * return failure). If all predecessors are marked as such, we
   * return the same. If some predecessors are recursive, and some
   * not, we'll return the merged not recursive results, but also set
   * the recursive flag. This helps us mark which cached entries must
   * be cleaned up after.
   *
   * When each block gets its result, we cache it so that another
   * lookup on that block can return the result immediately. Caching
   * results with the recursive flag set take special care. Such
   * results can be used during the calculation, but must be removed
   * afterwards. Why? Since the recursive flag means one of the paths
   * looped, we don't know if that path would have returned a failure
   * or not. We simply ignored that path during the calculation, but
   * if it *would* have returned failure, it would have made other
   * paths return failure as well. Since we ignored it, we cached
   * values instead of failures. This is fine for a single
   * calculation, but we cannot keep the cached values for future
   * calculations. We don't care about the recursive flag for failures
   * because a failure will always be a failure. The `cleanup' vector
   * is populated with the list of cache entires which must be removed
   * afterwards.
   */

  assertx(!target.isPhys());
  assertx(targetBlock.isValid());
  assertx(startInstIdx <= state.unit.blocks[currentBlock].code.size());
  assertx(phys_defs_set_cached(state, targetInst).empty());
  assertx(inOffsets.empty() || inOffsets.size() == state.unit.blocks.size());
  assertx(startOffset == 0 ||
          startInstIdx == state.unit.blocks[currentBlock].code.size());

  // Don't bother calling this if we trivially know if `phys' isn't
  // changed anywhere in the unit.
  assertx(state.physChanged.contains(phys));

  // Short-cut: if we've pre-calculated there's no change to `phys'
  // from this block back to the entry, we don't have to go any
  // further.
  if (!state.physChangedBefore[currentBlock].contains(phys)) {
    return {startOffset, false};
  }

  auto& unit = state.unit;
  auto const endIdx = unit.blocks[currentBlock].code.size();

  if (startInstIdx == endIdx) {
    // The cache and `inOffsets' only apply when we're starting at the
    // end of the block (which implies this is not the starting
    // block). If we're not at the end, we won't process all the
    // block, so the information won't be complete. It's fine to not
    // do this for the starting block. It just means we might
    // potentially process it one extra time if we encounter a loop.

    // See if we have an existing cache entry. The cached entry is the
    // offset from *this* block to the target, so we need to add any
    // accumulated offset up to this point.
    auto const it =
      state.physRecoverableCache.find({phys, target, currentBlock});
    if (it != state.physRecoverableCache.end()) {
      if (!it->second.first) return {std::nullopt, it->second.second};
      return {*it->second.first + startOffset, it->second.second};
    }

    // Check for looping. If the offsets match, we'll return the
    // recursive flag and ignore this result. If they don't we won't
    // set the flag, and the lack of an offset indicates a failure.
    if (!inOffsets.empty() && inOffsets[currentBlock]) {
      return {std::nullopt, startOffset == *inOffsets[currentBlock]};
    }

    // No loop. Record our start offset to catch a later loop.
    if (inOffsets.empty()) inOffsets.resize(unit.blocks.size());
    inOffsets[currentBlock] = startOffset;
  }

  auto const cache = [&] (Optional<int64_t> offset,
                          bool recursive = false)
    -> PhysRecoverableResult {

    if (startInstIdx == endIdx) {
      // The cache is implicitly for result starting at the end of the
      // block. Don't cache this information if we started in the middle
      // of the block.

      // We should have our start offset recorded at this point. Since
      // we're done with this block, remove it, and set a cache entry
      // instead.
      assertx(inOffsets[currentBlock]);
      inOffsets[currentBlock].reset();

      // Store our result in the cache in case we try to lookup this
      // block again. The offset here is the final offset returned
      // from the target block, which incorporates all the accumulated
      // offsets so far. We want the cache entries to be independent
      // of where we entered the block, so remove our start offset
      // from the result.
      DEBUG_ONLY auto const result = state.physRecoverableCache.emplace(
        std::make_tuple(phys, target, currentBlock),
        std::make_pair(
          offset ? make_optional(*offset - startOffset) : std::nullopt,
          recursive
        )
      );
      // We shouldn't have something cached already.
      assertx(result.second);

      // If recursive, we need to remove this entry once all the
      // recursion backs out.
      if (recursive) cleanup.emplace_back(currentBlock);
    }
    return {offset, recursive};
  };

  auto currentOffset = startOffset;

  // Process this block, unless if we're starting at the first
  // instruction (in which case we jump right to the predecessors).
  if (startInstIdx > 0) {
    // We only need to examine each instruction if we know there's a
    // definition of `phys' in it.
    if (state.physDefs[currentBlock].contains(phys)) {
      // This block defs `phys' somewhere. Examine every
      // instruction. Look for a def of `phys' before we hit the
      // target instruction (if in the target block).
      for (size_t idx = startInstIdx; idx > 0; --idx) {
        auto& inst = unit.blocks[currentBlock].code[idx - 1];
        if (phys_defs_set_cached(state, inst).contains(phys)) {
          // `phys' is modified here, and we haven't hit the target
          // instruction yet. Check if `phys' is modified in a
          // recoverable way. If so, we can accumulate the change and
          // continue onwards. Otherwise, give up.
          auto const off =
            tracked_physical_register_change(state, phys, inst, currentBlock);
          if (!off) return cache(std::nullopt);
          currentOffset += *off;
          continue;
        }
        // If we're not in the target block, there's no point in
        // looking for the target instruction.
        if (currentBlock != targetBlock) continue;
        // Otherwise we're in the target block, so check if we hit the
        // target instruction. If we do, we're done.
        if (!defs_set_cached(state, inst)[target]) continue;
        if (!compare_remat_insts(unit, inst, targetInst)) continue;
        return cache(currentOffset);
      }
      // If this is the target block, we should have seen the target
      // instruction and returned by now.
      assertx(currentBlock != targetBlock);
    } else if (currentBlock == targetBlock) {
      // This block does not def `phys'. If it's our target block, we
      // know `phys' is unchanged without examining any instructions.
      return cache(currentOffset);
    }
  }

  // Recurse into all the predecessors of this block and combine the
  // results:
  auto const& preds = state.preds[currentBlock];
  // If we're in the entry block, we should have either seen a def of
  // 'phys' or the target instruction by now.
  assertx(!preds.empty());

  Optional<int64_t> predOffset;
  auto anyRecursive = false;
  for (auto const pred : preds) {
    auto const result = physical_register_is_recoverable(
      state,
      phys,
      target,
      targetBlock,
      targetInst,
      pred,
      unit.blocks[pred].code.size(),
      currentOffset,
      inOffsets,
      cleanup
    );

    anyRecursive |= result.recursive;

    if (!result.offset) {
      // No offset. If the recursive flag is set, just ignore
      // it. Otherwise, we failed (note we don't care about recursive
      // flag if we fail).
      if (result.recursive) continue;
      return cache(std::nullopt);
    }

    // Otherwise make sure the results from all predecessors are the
    // same. If we not, fail.
    if (!predOffset) {
      predOffset = *result.offset;
    } else if (*predOffset != *result.offset) {
      return cache(std::nullopt);
    }
  }
  // If we don't have an offset, it means every path was recursive
  // with no offset. If so, the recursive flag should be set.
  assertx(predOffset || anyRecursive);
  return cache(predOffset, anyRecursive);
}

// Check if all the physical registers used by the given
// rematerialization instruction have recoverable values. If there's
// only one physical register with a non-zero recoverable offset,
// return that. Note that this will return an offset of zero if the
// instruction uses no physical registers.
Optional<int64_t> used_physical_registers_recoverable(State& state,
                                                      RematInfo& remat,
                                                      Vlabel b,
                                                      size_t instIdx) {
  // Remove any physical registers which are not modified anywhere in
  // the unit.  There's no need to check those.
  auto const physUses =
    phys_uses_set_cached(state, remat.instr) & state.physChanged;

  Optional<int64_t> offset{0};
  size_t count = 0;
  physUses.forEach(
    [&] (PhysReg phys) {
      if (!offset) return;

      auto const& defs = defs_set_cached(state, remat.instr);
      assertx(defs.size() == 1);
      auto const target = *defs.begin();

      jit::vector<Optional<int64_t>> inOffsets;
      jit::vector<Vlabel> cleanup;
      auto const result = physical_register_is_recoverable(
        state,
        phys,
        target,
        remat.block,
        remat.instr,
        b,
        instIdx,
        0,
        inOffsets,
        cleanup
      );

      // Remove any ephemeral entries in the cache which aren't safe
      // to keep.
      for (auto const b : cleanup) {
        DEBUG_ONLY auto const removed =
          state.physRecoverableCache.erase({phys, target, b});
        assertx(removed == 1);
      }

      if (!result.offset) {
        // No offset and the recursive flag means every path was
        // recursive, which shouldn't happen.
        assertx(!result.recursive);
        offset.reset();
        return;
      }

      // Don't return an offset if we have more than one physical
      // register and at least one of them has a non-zero recoverable
      // offset. We don't deal with multiple offsets right now, so
      // that case is ambiguous. Moreover it's not a concern as we
      // only deal with leas with non-zero offsets right now.
      if ((*offset != 0 || *result.offset != 0) && count > 0) {
        offset.reset();
        return;
      }
      offset = *result.offset;
      ++count;
    }
  );
  return offset;
}

// Check if the two given displacements can be added together and used
// as the displacement for a Vptr.  Namely, make sure the sum is still
// within the allowed range for the architecture.
bool can_merge_disps(int64_t disp1, int64_t disp2) {
  auto const total = disp1 + disp2;
  if (arch::any<arch::ARM>()) {
    return total >= -256 && total <= 255;
  } else {
    using DispType = decltype(std::declval<Vptr>().disp);
    return
      total >= std::numeric_limits<DispType>::min() &&
      total <= std::numeric_limits<DispType>::max();
  }
}

namespace detail {

// Attempt to add an offset to an instruction's Vptr
struct VptrVisitor {
  explicit VptrVisitor(int64_t offset) : offset{offset} {}

  template<typename T> void imm(const T&) {}
  template<typename T> void across(const T&) {}
  template<typename T> void def(const T&) {}
  template<typename T, typename H> void defHint(const T&, const H&) {}

  template<typename T, typename H>
  void useHint(const T& t, const H&) { use(t); }

  template<typename T>
  typename std::enable_if<
    std::is_same<Vreg,T>::value ||
    std::is_same<Vreg8,T>::value ||
    std::is_same<Vreg16,T>::value ||
    std::is_same<Vreg32,T>::value ||
    std::is_same<Vreg64,T>::value ||
    std::is_same<Vreg128,T>::value ||
    std::is_same<VregSF,T>::value ||
    std::is_same<VregDbl,T>::value
  >::type use(T) {}
  void use(RegSet) {}
  void use(VcallArgsId) {}
  void use(Vtuple) {}

  void use(Vptr& ptr) {
    // We only want to modify one Vptr. If an instruction has more
    // than one, it's something odd and we don't want to touch it.
    if (count > 0) {
      success = false;
      return;
    }
    ++count;
    if (!is_simple_vptr(ptr)) return;
    if (!can_merge_disps(ptr.disp, offset)) return;
    ptr.disp += offset;
    success = true;
  }

  int64_t offset;
  size_t count = 0;
  bool success = false;
};

}

// Attempt to fold the given offset into the given instruction,
// returning true if successful (with the instruction modified). If
// false, the instruction is not modified.
bool fold_offset_into_instr(Vinstr& instr, int64_t offset) {
  // A zero offset requires no changes, so always succeeds
  if (offset == 0) return true;

  if (instr.op == Vinstr::copy) {
    // copys can be turned into leas with the offset as the
    // displacement
    if (!can_merge_disps(0, offset)) return false;
    auto const src = instr.copy_.s;
    auto const dst = instr.copy_.d;
    instr.op = Vinstr::lea;
    instr.lea_.s = Vptr{};
    instr.lea_.s.base = src;
    instr.lea_.s.disp = offset;
    instr.lea_.d = dst;
    return true;
  } else {
    detail::VptrVisitor visitor{offset};
    visitOperands(instr, visitor);
    return visitor.success;
  }
}

// Aggregate all of the abstract locations that may be written to
// within a block.
AliasClass mem_writes_for_block(State& state, Vlabel b) {
  // If we've already calculated for this block, just return it.
  if (b < state.memWrites.size()) {
    if (state.memWrites[b]) return *state.memWrites[b];
  }

  // Otherwise calculate it by unioning together everything in the
  // block
  auto writes = AEmpty;
  for (auto const& inst : state.unit.blocks[b].code) {
    writes |= mem_writes_for_inst(inst);
  }

  // And save it in the cache
  if (b >= state.memWrites.size()) {
    state.memWrites.resize(state.unit.blocks.size());
  }
  state.memWrites[b] = writes;
  return writes;
}

/*
 * Implementation of mem_read_available. This walks backwards through
 * the unit from the starting point (following copy/spill/reload
 * chains) until it either (1) finds the original memory load, or (2)
 * finds a write which conflicts with the load's abstract location. If
 * we find no such write on any path before finding the load, this
 * means the original value is still present in the original location,
 * and the load can be safely re-performed.
 *
 * Since this can potentially do a look of duplicate work, we cache
 * the results in memReadAvailableCache. Before processing a block, we
 * consult memReadAvailableCache and if there's an entry, we return
 * immediately. As we unwind the blocks we visited, we update their
 * entries to short cut future lookups. The cache is also used to
 * detect cyclic lookups. When we start processing a block, we set the
 * cache entry to TriBool::Maybe, which indicates it's being
 * processed. If we hit a cache entry with TriBool::Maybe, we've hit a
 * loop. In that case, we treat it (optimistically) as a true entry
 * (TriBool::Yes). When we eventually update the TriBool::Maybe block
 * with its real value, if its true, we have to do nothing (as any
 * loops simply assumed that). However, if its false, we need to
 * change any entries involved in the loop to false. We do this with
 * fixup_mem_read_available_cache. It just walks all of the block's
 * successors, looking for matching TriBool::Yes entries and resets
 * them to TriBool::No.
 */

void fixup_mem_read_available_cache(State& state,
                                    Vlabel b,
                                    VregSet regs,
                                    bool end) {
  // For every block reachable (via successors) from b and have a
  // cached entry for a Vreg in regs, set that entry to
  // TriBool::No. If a Vreg in regs is copied, add it to regs. We stop
  // when none of the regs are live anymore, or if we no longer find
  // any appropriate cache entries. If end is true, we'll only process
  // the phijmp at the end of the block (if present), but not any
  // other instructions (used for starting the recursion).
  assertx(!regs.empty());
  assertx(regs.physRegs().empty());

  // Filter out Vregs which do not have a TriBool::Yes entry in the
  // cache. If there's no entry, or if the entry is TriBool::No or
  // TriBool::Maybe the fixup can terminate here. Such entries mean
  // the block did not participate in a loop.
  auto const finalize = [&] (VregSet& toFilter) {
    for (auto const r : toFilter) {
      auto it = state.memReadAvailableCache.find(std::make_pair(b, r));
      if (it == state.memReadAvailableCache.end() ||
          it->second != TriBool::Yes) {
        toFilter.remove(r);
        continue;
      }
      // Otherwise it did, so mark it properly
      it->second = TriBool::No;
    }
  };

  // Recurse into the given successor with the given Vregs. Ignore
  // successors with TriBool::Maybe entries. This indicates they're
  // currently being processed by mem_read_available_recurse. They'll
  // be fixed up correctly by that once done being processed.
  auto const recurse = [&] (Vlabel succ, VregSet succRegs) {
    for (auto const r : succRegs) {
      auto it = state.memReadAvailableCache.find(std::make_pair(succ, r));
      if (it != state.memReadAvailableCache.end() &&
          it->second == TriBool::Maybe) {
        succRegs.remove(r);
        continue;
      }
    }
    if (succRegs.empty()) return;
    fixup_mem_read_available_cache(state, succ, std::move(succRegs), false);
  };

  auto const& block = state.unit.blocks[b];
  if (!end) {
    // Look for any copies of something in regs. If so, add them to
    // regs as well.
    for (auto const& inst : block.code) {
      switch (inst.op) {
        case Vinstr::copy:
          if (regs[inst.copy_.s] && !inst.copy_.d.isPhys()) {
            regs.add(inst.copy_.d);
          }
          break;
        case Vinstr::reload:
          if (regs[inst.reload_.s] && !inst.reload_.d.isPhys()) {
            regs.add(inst.reload_.d);
          }
          break;
        case Vinstr::spill:
          if (regs[inst.spill_.s] && !inst.spill_.d.isPhys()) {
            regs.add(inst.spill_.d);
          }
          break;
        case Vinstr::ssaalias:
          if (regs[inst.ssaalias_.s] && !inst.ssaalias_.d.isPhys()) {
            regs.add(inst.ssaalias_.d);
          }
          break;
        case Vinstr::copyargs: {
          auto const& dsts = state.unit.tuples[inst.copyargs_.d];
          auto const& srcs = state.unit.tuples[inst.copyargs_.s];
          assertx(srcs.size() == dsts.size());
          for (size_t i = 0; i < dsts.size(); ++i) {
            if (!regs[srcs[i]]) continue;
            if (dsts[i].isPhys()) continue;
            regs.add(dsts[i]);
          }
          break;
        }
        default:
          break;
      }
    }
  }

  // Process phijmp separately, since we want to deal with it
  // regardless of whether end is true or not.
  if (block.code.back().op == Vinstr::phijmp) {
    auto const& inst = block.code.back();
    auto const successors = succs(block);
    assertx(successors.size() == 1);
    auto const& phidef = state.unit.blocks[successors[0]].code.front();
    assertx(phidef.op == Vinstr::phidef);
    auto const& srcs = state.unit.tuples[inst.phijmp_.uses];
    auto const& dsts = state.unit.tuples[phidef.phidef_.defs];
    assertx(srcs.size() == dsts.size());

    // Add the Vregs defined by the phi after doing the normal regs
    // processing. Logically they are defined in the successor, so if
    // we add them early, they'll just get filtered out.
    VregSet phiRegs;
    for (size_t i = 0; i < srcs.size(); ++i) {
      if (!regs[srcs[i]]) continue;
      if (dsts[i].isPhys() || regs[dsts[i]]) continue;
      phiRegs.add(dsts[i]);
    }
    // If there's no actual phi Vregs, we can just take the normal
    // path.
    if (!phiRegs.empty()) {
      // Otherwise, remove the regs which do not have an appropriate
      // cached entry for this block.
      finalize(regs);
      // Now add in the phi regs. Also filter out the regs which
      // aren't even live in the successor.
      auto newRegs = (regs & state.liveIn[successors[0]]) | phiRegs;
      // phiRegs is not empty, so the union must not be.
      assertx(!newRegs.empty());
      // Recurse into the (single) successor
      return recurse(successors[0], std::move(newRegs));
    }
  }

  // Remove the regs which do not have an appropriate cached entry for
  // this block.
  finalize(regs);
  // If there's none left, nothing left to do.
  if (regs.empty()) return;
  // Otherwise recurse into each successor. We can narrow the Vreg
  // list down by only those Vregs live into that successor.
  for (auto const succ : succs(block)) recurse(succ, regs & state.liveIn[succ]);
}

bool mem_read_available_recurse(State& state,
                                Vlabel b,
                                size_t instIdx,
                                const AliasClass& load,
                                Vreg r) {
  assertx(!r.isPhys());
  assertx(instIdx <= state.unit.blocks[b].code.size());

  // If the block has a single predecessor, we'll use iteration instead
  // of recursion (because it's more efficient). However we need to
  // update cache entries on the way out. So store the block/Vreg
  // pairs (the Vreg can change too) here for updates on the exit.
  TinyVector<std::pair<Vlabel, Vreg>, 16> cacheUpdates;

  // Called when "entering" a new block. If there's a cached entry,
  // return it. Otherwise mark this block as being processed.
  auto const enter = [&] () -> Optional<bool> {
    // We only cache entries for the full block
    if (instIdx == state.unit.blocks[b].code.size()) {
      auto const insert = state.memReadAvailableCache.emplace(
        std::make_pair(b, r), TriBool::Maybe
      );
      if (!insert.second) return insert.first->second != TriBool::No;
      cacheUpdates.emplace_back(std::make_pair(b, r));
    }
    return std::nullopt;
  };

  // Called when we have a result and are "exiting". Update all of the
  // blocks we visited in this function with the cached value. If the
  // result is false, make sure we fixup any cached entries which
  // reached the blocks from a loop.
  auto const exit = [&] (bool result) {
    for (size_t i = 0; i < cacheUpdates.size(); ++i) {
      auto const& update = cacheUpdates[i];
      auto it = state.memReadAvailableCache.find(update);
      // Nothing should have modified the entries for this block:
      assertx(it != state.memReadAvailableCache.end());
      assertx(it->second == TriBool::Maybe);

      // If we're setting this entry to false, then we need to perform the
      // fixup. We can skip the fixup if the block has only one
      // successor as it would have been processed in the previous iteration of
      // this loop, except when it is the very first block in cacheUpdates.
      bool fixupNow = !result
        && (i == 0 || succs(state.unit.blocks[update.first]).size() > 1);

      if (fixupNow) {
        // fixup_mem_read_available_cache assumes any processably
        // block has a TriBool::Yes entry, so temporarily change it to
        // that.
        it->second = TriBool::Yes;
        fixup_mem_read_available_cache(
          state,
          update.first,
          VregSet{update.second},
          true
        );
        // Fixup should have changed this for us
        assertx(it->second == TriBool::No);
      } else {
        // No need for fixups
        it->second = yesOrNo(result);
      }
    }

    return result;
  };

  while (true) {
    assertx(!r.isPhys());

    while (state.liveIn[b][r]) {
      if (auto const cached = enter()) return exit(*cached);

      // The current Vreg is live-in to this block, which means it is
      // not def'ed here. We only need to check this block for
      // conflicting writes.
      auto& block = state.unit.blocks[b];

      // Check if any the aggregated writes in this block interfer
      // with the load. If they do, do a per-instruction check. This
      // is for two reasons. The first is that unioning together the
      // locations can sometimes create larger than desired
      // unions. The second is that we might only be processing *some*
      // of this block (if instIdx is not at the end), so the
      // per-block writes is an over-estimate.
      if (mem_writes_for_block(state, b).maybe(load)) {
        for (size_t i = 0; i < instIdx; ++i) {
          auto const& inst = block.code[i];
          if (mem_writes_for_inst(inst).maybe(load)) return exit(false);
        }
      }

      // No conflicts. This block is clean. Recurse into any
      // predecessors. If there's only one predecessor, we can just
      // loop.
      auto const& preds = state.preds[b];
      assertx(!preds.empty());

      if (preds.size() > 1) {
        for (auto const pred : preds) {
          auto const available = mem_read_available_recurse(
            state,
            pred,
            state.unit.blocks[pred].code.size(),
            load,
            r
          );
          if (!available) return exit(false);
        }
        return exit(true);
      }

      b = preds[0];
      instIdx = state.unit.blocks[b].code.size();
    }

    // The current Vreg has a def in this block. This could either be
    // the original load, or a copy/spill/reload.

    if (auto const cached = enter()) return exit(*cached);

    // We're going to check for per-instruction memory location
    // conflicts below, but we can skip it if there's no conflicts for
    // the entire block.
    auto const checkWrites = mem_writes_for_block(state, b).maybe(load);

    auto& block = state.unit.blocks[b];
    while (true) {
      // We shouldn't reach the beginning of the block since we should
      // hit the def first.
      always_assert_flog(
        instIdx > 0,
        "Cannot find a def for {} in {}",
        show(r), b
      );
      --instIdx;
      auto& inst = block.code[instIdx];

      // Do a quick check against the cached defs set. If there's no
      // match, we can move onto the next instruction. Before we do
      // that, check for write conflicts if we're checking.
      if (!defs_set_cached(state, inst)[r]) {
        if (checkWrites && mem_writes_for_inst(inst).maybe(load)) {
          return exit(false);
        }
        continue;
      }

      // We found our def. These are all copyish, or the original
      // memory load. For the copyish ones, switch to the new Vreg to
      // track and repeat the process. We don't need to check for
      // write conflicts for any of these because none of them write
      // to memory.
      switch (inst.op) {
        case Vinstr::copy:
          assertx(inst.copy_.d == r);
          r = inst.copy_.s;
          break;
        case Vinstr::reload:
          assertx(inst.reload_.d == r);
          r = inst.reload_.s;
          break;
        case Vinstr::spill:
          assertx(inst.spill_.d == r);
          r = inst.spill_.s;
          break;
        case Vinstr::ssaalias:
          assertx(inst.ssaalias_.d == r);
          r = inst.ssaalias_.s;
          break;
        case Vinstr::copyargs: {
          auto const& dsts = state.unit.tuples[inst.copyargs_.d];
          auto const& srcs = state.unit.tuples[inst.copyargs_.s];
          assertx(srcs.size() == dsts.size());
          auto DEBUG_ONLY found = false;
          for (size_t i = 0; i < dsts.size(); ++i) {
            if (dsts[i] != r) continue;
            r = srcs[i];
            if (debug) found = true;
            break;
          }
          assertx(found);
          break;
        }
        case Vinstr::phidef: {
          assertx(instIdx == 0);
          auto const& dsts = state.unit.tuples[inst.phidef_.defs];
          auto const& preds = state.preds[b];

          // Recurse into every predecessor corresponding to the phi
          // srcs. Once we return, we have our answer.
          for (size_t i = 0; i < dsts.size(); ++i) {
            if (dsts[i] != r) continue;

            for (auto const p : preds) {
              auto const& pred = state.unit.blocks[p];
              auto const& phijmp = pred.code.back();
              assertx(phijmp.op == Vinstr::phijmp);
              auto const s = state.unit.tuples[phijmp.phijmp_.uses][i];
              auto const available = mem_read_available_recurse(
                state,
                p,
                pred.code.size(),
                load,
                s
              );
              if (!available) return exit(false);
            }
            return exit(true);
          }
          always_assert(false);
        }
        default:
          // This should be the original memory load. Do some sanity
          // checking.
          assertx(is_mem_read(inst));
          assertx(inst.origin);
          if (debug) {
            auto const DEBUG_ONLY effects = memory_effects(*inst.origin);
            auto const DEBUG_ONLY otherLoad = std::get_if<PureLoad>(&effects);
            always_assert(otherLoad && otherLoad->src.isSingleLocation());
            always_assert(load.maybe(otherLoad->src));
          }
          return exit(true);
      }

      // If we reach here, we hit a copyish instruction and updated
      // our current Vreg. Break out of this loop and into the outer
      // loop, which will restart the logic.
      break;
    }
  }

  not_reached();
}

// Return true if the memory read given by the "instr" instruction can
// be safely rematerialized at the program point given by "b" and
// "instIdx". The value that was loaded is currently in "r".
bool mem_read_available(State& state,
                        Vlabel b,
                        size_t instIdx,
                        const Vinstr& instr,
                        Vreg r) {
  assertx(is_mem_read(instr));
  assertx(instr.origin);
  auto const effects = memory_effects(*instr.origin);
  auto const pureLoad = std::get_if<PureLoad>(&effects);
  assertx(pureLoad && pureLoad->src.isSingleLocation());

  return mem_read_available_recurse(
    state,
    b,
    instIdx,
    pureLoad->src,
    r
  );
}

// Return an instruction to reload a spilled Vreg src into Vreg dst,
// possibly using rematerialization. If rematerialization is not
// possible, then a normal reload instruction is returned. The search
// for the rematerialized instruction is started at the specified
// block and instruction offset. The "inReg" VregSet determines which
// Vregs are known to be in registers (and thus available) at the
// current program point. An instruction can only be rematerialized if
// all of its sources are available in physical registers. The
// rematerialization logic will consider sufficiently simple memory
// reads, unless if "allowMemReads" is false.
Vinstr reload_with_remat(State& state,
                         Vlabel b,
                         size_t instIdx,
                         const VregSet& inReg,
                         Vreg src,
                         Vreg dst,
                         bool allowMemReads = true) {
  assertx(!src.isPhys());

  auto remat =
    find_defining_inst_for_remat_enter(state, src, b, instIdx);

  // If the rematerialization instruction is a reload, we'll just
  // return it, so no further checks are needed. Otherwise we can
  // cache an instruction which may be situationally usable, so we
  // need to do these checks every time (if the instruction is never
  // useful, we should have a reload stored here already).
  if (remat.instr.op == Vinstr::reload) return reload{src, dst};

  if ((uses_set_cached(state, remat.instr) - inReg).any()) {
    // One of the sources is unavailable. Return a reload.
    return reload{src, dst};
  }

  // If we don't want to consider memory reads here, bail out right
  // away.
  auto const memRead = is_mem_read(remat.instr);
  if (memRead && !allowMemReads) return reload{src, dst};

  // Check if the physical registers this rematerialization
  // instruction uses (if any) are recoverable. If not, we cannot use
  // it.
  auto const offset =
    used_physical_registers_recoverable(state, remat, b, instIdx);
  if (!offset) return reload{src, dst};

  // Attempt to fold the offset into the instruction (if offset is
  // zero, this will always succeed). If we can't, the instruction
  // cannot be used.
  if (!fold_offset_into_instr(remat.instr, *offset)) return reload{src, dst};

  // If this is a memory read, check if we can safely rematerialize it
  if (memRead && !mem_read_available(state, b, instIdx, remat.instr, src)) {
    return reload{src, dst};
  }

  // Change the output of the rematerialized instruction (there should
  // be only one) to the requested Vreg and return it.
  visitRegsMutable(
    state.unit,
    remat.instr,
    []  (Vreg r) { return r; },
    [&] (Vreg)   { return dst; }
  );
  invalidate_cached_operands(remat.instr);
  return remat.instr;
}

// A Vreg is a trivial rematerialization if it can always be
// rematerialized at any position in the unit. This usually means it
// represents an immediate and thus has no sources to depend on, or it
// only uses ignored physical registers (which haven't been modified
// in non-recoverable ways since). This function determines if a Vreg
// is a trivial rematerialization. The block/instruction position is
// used as a starting point to resolve the defining instruction
// only. If the rematerialization is trivial, it is returned,
// std::nullopt otherwise.
Optional<Vinstr> is_trivial_remat(State& state,
                                  Vreg r,
                                  Vlabel b,
                                  size_t instIdx) {
  auto remat = find_defining_inst_for_remat_enter(state, r, b, instIdx);
  switch (remat.instr.op) {
    case Vinstr::ldimmq:
    case Vinstr::ldimml:
    case Vinstr::ldimmb:
    case Vinstr::ldundefq:
      // Immediates can always be rematerialized (this is the usual
      // case).
      return remat.instr;
    case Vinstr::reload:
      // If we ever have to reload it, it's obviously not trivial.
      return std::nullopt;
    default: {
      // Memory rematerialization is never trivial
      if (!isPure(remat.instr)) return std::nullopt;
      // If the instruction uses any non-ignored registers, we cannot
      // treat it as trivial, as we do not know if they'll be
      // available or not.
      if (uses_set_cached(state, remat.instr).any()) return std::nullopt;
      // However we can treat it as trivial if it uses only ignored
      // physical registers, whose values are recoverable at this
      // point.
      auto const offset =
        used_physical_registers_recoverable(state, remat, b, instIdx);
      // If the physical register's value is not recoverable, we
      // cannot rematerialize.
      if (!offset) return std::nullopt;
      // Otherwise we can only use it if we can successfully fold the
      // offset into the instruction (if the offset is zero, this will
      // always succeed).
      if (fold_offset_into_instr(remat.instr, *offset)) return remat.instr;
      return std::nullopt;
    }
  }
}

namespace detail {

// Visitor to access a Vinstr's Vptr and Vreg operands in a generic
// way.
template <typename OnUse, typename OnVptr>
struct FoldRematWithUseVisit {
  template<typename T> void imm(const T&) {}
  template<typename T> void def(const T&) {}
  template <typename T, typename H> void defHint(const T&, const H&) {}

  template<typename T> void across(T& t) { use(t); }
  template<typename T, typename H> void useHint(T& t, const H&) { use(t); }

  void use(RegSet) {}
  void use(VcallArgsId) { always_assert(false); }

  void use(Vtuple t) { for (auto const r : unit.tuples[t]) onUse(r); }
  void use(Vptr& ptr) { onVptr(ptr); }
  void use(Vreg r) { onUse(r); }

  OnUse onUse;
  OnVptr onVptr;
  const Vunit& unit;
};

// Provides template param deduction....
template <typename OnUse, typename OnVptr>
FoldRematWithUseVisit<OnUse, OnVptr>
make_fold_remat_with_use_visit(const Vunit& unit,
                               OnUse onUse,
                               OnVptr onVptr) {
  return FoldRematWithUseVisit<OnUse, OnVptr>{
    std::move(onUse),
    std::move(onVptr),
    unit
  };
}

}

// Returns true if the given rematerialization instruction `remat'
// (defining `r') can be folded completely into the given instruction
// `use'. `use' must actually use `r'.
bool can_fold_remat_with_use(const State& state,
                             const Vinstr& remat,
                             Vinstr& use,
                             Vreg r) {
  assertx(!r.isPhys());

  int64_t rematDisp = 0;
  if (remat.op == Vinstr::lea) {
    if (!is_simple_vptr(remat.lea_.s)) return false;
    rematDisp = remat.lea_.s.disp;
  } else if (remat.op != Vinstr::copy) {
    return false;
  }

  // If the use is a copy, we can turn it into a lea as long as the
  // displacement isn't too large.
  if (use.op == Vinstr::copy) return can_merge_disps(0, rematDisp);

  auto bad = false;
  DEBUG_ONLY auto found = false;
  // Iterate over the operands of `use'. `r' cannot be used as a
  // normal operand (we cannot fold that) and can only be used as the
  // base of a Vptr.
  auto visit = detail::make_fold_remat_with_use_visit(
    state.unit,
    [&] (Vreg u) {
      if (u != r) return;
      bad = true;
      found = true;
    },
    [&] (const Vptr& p) {
      if (!is_simple_vptr(p)) {
        // The Vptr isn't simple, so we cannot fold into it. However
        // if the Vptr does not use `r' at all, we can ignore those,
        // as its not relevant anyways.
        if (p.base == r || p.index == r) {
          bad = true;
          found = true;
        }
        return;
      }
      // The Vptr is simple, but we still need to check if it uses
      // `r'. If it does, we can fold as long as the displacements
      // will fit.
      if (p.base != r) return;
      found = true;
      if (!can_merge_disps(p.disp, rematDisp)) bad = true;
    }
  );
  visitOperands(use, visit);
  // We should have found something because its a pre-condition that
  // `use' uses `r'.
  assertx(found);
  return !bad;
}

// Given the rematerialization instruction `remat' defining `r', fold
// it into the instruction `use'. The ability to do this should have
// already been checked.
void fold_remat_with_use(const State& state,
                         const Vinstr& remat,
                         Vinstr& use,
                         Vreg r) {
  assertx(can_fold_remat_with_use(state, remat, use, r));

  Vreg rematBase;
  int64_t rematDisp = 0;
  if (remat.op == Vinstr::lea) {
    assertx(is_simple_vptr(remat.lea_.s));
    rematBase = remat.lea_.s.base;
    rematDisp = remat.lea_.s.disp;
  } else {
    assertx(remat.op == Vinstr::copy);
    rematBase = remat.copy_.s;
  }

  // If the use is a copy, we can just turn it into a lea
  if (use.op == Vinstr::copy) {
    assertx(can_merge_disps(0, rematDisp));
    if (rematDisp == 0) {
      use.copy_.s = rematBase;
    } else {
      auto const dst = use.copy_.d;
      use.op = Vinstr::lea;
      use.lea_.s = Vptr{};
      use.lea_.s.base = rematBase;
      use.lea_.s.disp = rematDisp;
      use.lea_.d = dst;
    }
    invalidate_cached_operands(use);
    return;
  }

  // Otherwise see if we can fold it into any Vptrs
  DEBUG_ONLY auto changed = false;
  auto visit = detail::make_fold_remat_with_use_visit(
    state.unit,
    [&] (Vreg u) { assertx(u != r); },
    [&] (Vptr& p) {
      if (p.base != r) return;
      assertx(is_simple_vptr(p));
      assertx(can_merge_disps(p.disp, rematDisp));
      // Do the actual folding.
      p.base = rematBase;
      p.disp += rematDisp;
      changed = true;
    }
  );
  visitOperands(use, visit);
  assertx(changed);
  invalidate_cached_operands(use);
}

struct SpillWithRematResult {
  // How many instructions were added
  size_t added;
  // Whether rematerialization was performed
  bool rematerialized;
};

namespace detail {

// Helper function for spill_with_remat below. Try to emit the
// appropriate spill[b,l,q]i instruction. Otherwise use a spill
// instruction (if allowed).
template <typename T, typename I>
SpillWithRematResult try_immed_spill(I i,
                                     Vout& v,
                                     Vreg src,
                                     Vreg dst,
                                     const Vinstr& remat,
                                     bool useDst,
                                     bool useSrc) {
  if (i.fits(sz::dword)) {
    v << T{i.l(), dst};
    return {1, true};
  }
  if (useDst) {
    emit_reload(v, remat);
    v << spill{dst, dst};
    return {2, true};
  }
  if (useSrc) {
    v << spill{src, dst};
    return {1, false};
  }
  return {0, false};
}

}

// Generate a spill for the given Vreg `src' into the given Vreg
// `dst'. If `src' is rematerializable with an immediate at the given
// program point (given by the block/instruction position), then
// attempt to spill the immediate directly. Otherwise emit a spill
// instruction. The instructions are inserted into the given Vout&.
//
// `inReg' is the set of Vregs currently in registers (not spilled),
// which is used to determine what rematerializations can be done.
// Vout.
//
// If `useDst' is true, than the dst Vreg can be used as a target for
// a rematerialization instruction. dst is then spilled to itself. If
// false, the only rematerialization instructions allowed are the ones
// which spill directly to the slot (without using an intermediate
// register). If this isn't possible, a vanilla spill will be emitted.
//
// If `useSrc' is true, the src Vreg can be used as (non-spilled)
// register. This is required to emit a spill instruction. If `useSrc'
// is false, *only* rematerialization is allowed. If rematerialization
// is not possible, than no instruction will be emitted (this is the
// only time this function will fail to emit an instruction).
//
// The number of instructions thus inserted is returned, along with a
// flag indicating if rematerialization happened. If `useSrc' is
// false, and no spill instruction could be generated, 0 is returned.

SpillWithRematResult spill_with_remat(State& state,
                                      Vout& v,
                                      Vlabel b,
                                      size_t instIdx,
                                      const VregSet& inReg,
                                      Vreg src,
                                      Vreg dst,
                                      bool useDst,
                                      bool useSrc) {
  // A physical register has no RegInfo and no defining instruction to
  // rematerialize, so spill directly if needed.
  if (src.isPhys()) {
    if (!useSrc) return {0, false};
    v << spill{src, dst};
    return {1, false};
  }

  // Determine if this Vreg can be rematerialized at this current
  // program point.
  auto const remat =
    reload_with_remat(state, b, instIdx, inReg, src, dst, false);

  // We can't use the spill immediate instructions on non-X86 because
  // they'd have to be lowered (and that has to be done before
  // register allocation).
  if (!arch::any<arch::X64>()) {
    if (remat.op == Vinstr::reload || !useDst) {
      if (!useSrc) return {0, false};
      v << spill{src, dst};
      return {1, false};
    }
    emit_reload(v, remat);
    v << spill{dst, dst};
    return {2, true};
  }

  switch (remat.op) {
    // Rematerializable as an immediate. Try to use a spill immediate
    // instruction.
    case Vinstr::ldimmb:
      return detail::try_immed_spill<spillbi>(
        remat.ldimmb_.s, v, src, dst, remat, useDst, useSrc
      );
    case Vinstr::ldimml:
      return detail::try_immed_spill<spillli>(
        remat.ldimml_.s, v, src, dst, remat, useDst, useSrc
      );
    case Vinstr::ldimmq:
      return detail::try_immed_spill<spillqi>(
        remat.ldimmq_.s, v, src, dst, remat, useDst, useSrc
      );
    case Vinstr::ldundefq:
      v << spillundefq{dst};
      return {1, true};
    case Vinstr::copy:
      // Special case: if the Vreg comes from a copy, we can spill
      // directly from the copy's source, saving an intermediate
      // Vreg.
      v << spill{remat.copy_.s, dst};
      return {1, true};
    case Vinstr::reload:
      // Not rematerializable. Just emit a spill (if we can, fail
      // otherwise).
      if (!useSrc) return {0, false};
      v << spill{src, dst};
      return {1, false};
    default:
      // Some other rematerializable instruction. We can't write it
      // directly to the spill slot, but we can rematerialize it into
      // dst, and then spill dst to itself. We can only do this if
      // we're allowed to write to dst as a register (as opposed to
      // just as a spill destination).
      assertx(isPure(remat));
      if (useDst) {
        emit_reload(v, remat);
        v << spill{dst, dst};
        return {2, true};
      }
      if (useSrc) {
        v << spill{src, dst};
        return {1, false};
      }
      return {0, false};
  }
}

/*
 * Spill Weight:
 *
 * The spill weight is what drives the spilling heuristic. When we
 * need to spill a Vreg, the spill weights of all (eligible) Vregs are
 * calculated and sorted. The Vregs with the largest spill weights are
 * chosen. Therefore we want higher spill weights to reflect that a
 * Vreg is more profitable to spill.
 *
 * The spill weight is composed of two values, the usage frequency and
 * the distance. We use profiling information to drive the usage
 * frequency calculation. From the current location, we search forward
 * and find all the next uses of the Vreg. That is, all the uses
 * reachable from the current location which do not already have a use
 * on the path. The usage frequency is the sum of the block weights of
 * each of those uses. Since we will reload the Vreg before each of
 * these uses, it reflects an estimation of how many reloads we'll
 * have to execute at runtime. Since copyish instructions do not
 * (usually) trigger a reload, they are not considered a use. Instead
 * their dests are added to the set of Vregs we are looking for.
 *
 * The second component of the spill weight is the distance. This uses
 * the same Vreg uses we found for the usage frequency. It is the
 * minimum of the distances (in terms of instructions) between the
 * current location and those uses. All other things being equal, we
 * want to choose the Vreg which is used farthest away. The distance
 * is only used to break ties when the frequency is the same (which
 * can happen if we don't have profiling information).
 *
 * We calculate the spill weights on demand rather than
 * pre-calculating them, as many Vregs will never need to be spilled.
 *
 * In addition to the base spill weight, we apply a multiplier
 * depending if the Vreg cannot be rematerialized at a particular
 * point. This is to reflect the fact that rematerializing a Vreg is
 * cheaper than loading it from a spill slot. This biases the spiller
 * towards trying to "spill" things like immediates, which can be
 * rematerialized cheaply. We also take into account that if the Vreg
 * is trivially rematerializable, its spill slot will probably become
 * dead, which means even the spill is "free".
 */

struct SpillWeight {
  int64_t usage = 0;
  int64_t distance = std::numeric_limits<int32_t>::max();

  bool operator==(const SpillWeight& o) const {
    return usage == o.usage && distance == o.distance;
  }
  bool operator!=(const SpillWeight& o) const {
    return usage != o.usage || distance != o.distance;
  }
  // Higher usage is less (since a higher weight means more profitable
  // to spill, which means less usage).
  bool operator<(const SpillWeight& o) const {
    if (usage != o.usage) return usage > o.usage;
    return distance < o.distance;
  }
  bool operator>(const SpillWeight& o) const {
    if (usage != o.usage) return usage < o.usage;
    return distance > o.distance;
  }
};

// The relative weight of a spill/reload instruction versus
// rematerializing a Vreg. The higher this value, the more profitable
// it is to rematerialize a Vreg versus spilling or reloading it. You
// don't necessarily want to always rematerialize, as a spill/reload
// outside of a loop might be preferable to a rematerialization within
// a loop. Ideally this would be set to the actual runtime cost of
// writing/reading from the stack versus the rematerialization
// instruction, but this is hard to determine in practice because the
// actual cost is determined by many factors. The weight here was
// determined empirically to give the best results.
//
// Note: setting this too high can cause overflows during spill weight
// calculations, which will lead to crashes,
const constexpr int64_t kSpillReloadMultiplier = 25;

struct SpillerResults;

// Result of a spill weight lookup at a given program point:
struct SpillWeightAt {
  // Calculated spill weight
  SpillWeight weight;
  // Whether all immediate uses from this point are trivially
  // rematerializable.
  bool allTrivial = true;
};

SpillWeightAt spill_weight_at(State&,
                              const SpillerResults&,
                              Vreg,
                              Vlabel,
                              size_t);

// The state of which live Vregs are currently spilled or not-spilled. This
// encapsulates that state, as well as the logic to decide which registers
// should be spilled or reloaded.

struct SpillerState {
  // Default state is no Vregs are live
  SpillerState(State& state, bool spRecordedIn)
    : gp{size_t(state.gpUnreserved.size())}
    , simd{size_t(state.simdUnreserved.size())}
    , state{&state}
    , spRecorded{spRecordedIn}
  {}

  // We track the Vreg state separately for each register class.
  struct PerClass {
    size_t numRegs;
    VregSet inReg;
    VregSet inMem;
  };
  PerClass gp;
  PerClass simd;
  State* state;
  bool spRecorded;

  // Given a Vreg, return the per-class state appropriate for that Vreg (or
  // nullptr if untracked). This lets code manipulate the state generically
  // without having to switch on register class constantly.
  const PerClass* forReg(Vreg r) const {
    if (is_ignored(*state, r)) return nullptr;
    switch (reg_class(*state, r)) {
      case RegClass::AnyNarrow:
      case RegClass::GP:
        return &gp;
      case RegClass::SIMD:
      case RegClass::SIMDWide:
        return &simd;
      case RegClass::SF:
        return nullptr;
      case RegClass::Any:
      case RegClass::Spill:
      case RegClass::SpillWide:
        break;
    }
    always_assert(false);
  }
  PerClass* forReg(Vreg r) {
    return const_cast<PerClass*>(
      const_cast<const SpillerState*>(this)->forReg(r)
    );
  }

  /*
   * Using the spill weights at the given position, move any Vregs which need to
   * be moved from "inReg" to "inMem" to bring the number of Vregs in "inReg"
   * back below the size of the register class. If we cannot move enough Vregs,
   * then we'll assert (which means a bug). The set of Vregs thus moved is
   * returned. Spill instructions will need to be generated for these Vregs.
   *
   * The "forbidden" set is the set of Vregs which are not eligible
   * for spilling (typically the set of Vregs which are operands to the
   * instruction and thus cannot be spilled).
   *
   * The "cost" callback takes a Vreg, and returns the estimated cost
   * of spilling that Vreg. This is incorporated into the total spill
   * weight for that Vreg. If the Vreg is rematerializable at all of
   * its immediate uses, this spill cost is ignored (as it's assumed
   * the spill will be dead).
   *
   * The "penalties" set is a set of manual adjustments which will be
   * made to the calculated spill weights before they are sorted. This
   * can be used to bias decisions based on some special knowledge.
   *
   * This function is typically used after some Vregs are moved into the "inReg"
   * set (perhaps because of reloading). You then call this to spill as
   * necessary (using the Vregs just reloaded as the forbidden set).
   */
  template <typename Cost>
  VregSet spill(Vlabel b,
                size_t instIdx,
                const SpillerResults& results,
                Cost&& cost,
                const VregSet& forbidden = {}) {
    // We'll calculate spill for GP and SIMD separately and then combine the
    // results (since they do not interact).
    auto const impl = [&] (PerClass& per) {
      // If we're already below the size, there's nothing to do (this is the
      // common case).
      if (per.inReg.size() <= per.numRegs) return VregSet{};

      // We know we have to spill. If the set of Vregs we can't spill
      // (that aren't spilled) is equal to, or larger, than the
      // register limit, we know we have to spill all of them. There's
      // no need to invoke any of the heuristics below. This can
      // happen if any Vregs are live across calls which clobber
      // everything.
      if ((forbidden & per.inReg).size() >= per.numRegs) {
        auto spilled = per.inReg - forbidden;
        spilled.removePhys();

        // Make sure that spilling everything that we can is
        // sufficient.
        auto const toRemove = per.inReg.size() - per.numRegs;
        always_assert_flog(
          toRemove <= spilled.size(),
          "Need to spill {} Vregs, but only {} are available at location {} {}",
          toRemove,
          spilled.size(),
          b,
          instIdx
        );

        // We know we have to spill everything, so just move
        // everything into memory.
        per.inReg -= spilled;
        per.inMem |= spilled;
        return spilled;
      }

      // Otherwise we really have to spill. Gather up the candidates with their
      // spill weights at this point.
      jit::vector<std::pair<Vreg, SpillWeight>> candidates;
      candidates.reserve(per.inReg.size());
      for (auto const r : per.inReg) {
        // We never can spill physical registers or ones which have
        // been forbidden.
        if (r.isPhys() || forbidden[r]) continue;
        // Calculate the spill weights
        auto weight = spill_weight_at(*state, results, r, b, instIdx);
        // If this candidate is not rematerializable at all of its
        // immediate uses, add on any penalty for spilling it at the
        // current location.
        if (!weight.allTrivial) weight.weight.usage += cost(r);
        candidates.emplace_back(r, weight.weight);
      }

      // If we have less candidates than we need to spill, we're in trouble.
      auto const toRemove = per.inReg.size() - per.numRegs;
      always_assert_flog(
        toRemove <= candidates.size(),
        "Need to spill {} Vregs, but only {} are available at location {} {}",
        toRemove,
        candidates.size(),
        b,
        instIdx
      );

      // Sort them according to their spill weights. Higher weights (more
      // profitable to spill) come first. Use the Vreg number to break ties.
      std::sort(
        candidates.begin(),
        candidates.end(),
        [](const std::pair<Vreg, SpillWeight>& a,
           const std::pair<Vreg, SpillWeight>& b) {
          if (a.second != b.second) return a.second > b.second;
          return a.first < b.first;
        }
      );

      // Spill the first N, remove them from the inReg state and put them in
      // inMem to reflect they're spilled.
      VregSet spilled;
      for (size_t i = 0; i < toRemove; ++i) spilled.add(candidates[i].first);
      per.inReg -= spilled;
      per.inMem |= spilled;

      return spilled;
    };
    return impl(gp) | impl(simd);
  }

  // Remove any Vregs (in the candidates set) from being tracked which
  // are not live after the current position, or that belong to the
  // optional kills set.
  void dropDead(const VregSet& candidates,
                const VregSet* kills,
                Vlabel b,
                size_t instIdx) {
    for (auto const r : candidates) {
      auto s = forReg(r);
      if (!s) continue;
      assertx(s->inReg[r] || s->inMem[r]);
      if (live_in_at(*state, r, b, instIdx + 1) &&
          (!kills || !r.isPhys() || !(*kills)[r])) {
        continue;
      }
      s->inReg.remove(r);
      s->inMem.remove(r);
    }
  }

  // Go back to initial state
  void reset() {
    gp.inReg.reset();
    gp.inMem.reset();
    simd.inReg.reset();
    simd.inMem.reset();
  }

  // Return all Vregs in memory
  VregSet inMem() const { return gp.inMem | simd.inMem; }

  // Whether nothing is currently spilled
  bool allRegs() const {
    return
      gp.inMem.none() &&
      simd.inMem.none();
  }

  // Whether everything live is a physical register
  bool allPhys() const {
    return allRegs() && gp.inReg.allPhys() && simd.inReg.allPhys();
  }

  std::string toString() const {
    return fmt::format(
      "GP: [Reg: {} Mem: {}] SIMD: [Reg: {} Mem: {}]",
      show(gp.inReg), show(gp.inMem),
      show(simd.inReg), show(simd.inMem)
    );
  }

  // Sanity checking
  bool checkInvariants(Vlabel b, size_t instIdx) const {
    auto const impl = [&] (const PerClass& per) {
      // If we can't spill, we should never have a Vreg in memory.
      always_assert(IMPLIES(!spRecorded, per.inMem.none()));
      // We should never have more un-spilled Vregs than there are physical
      // registers for the class (except before a call to spill()).
      always_assert(per.inReg.size() <= per.numRegs);
      // A Vreg is either spilled or not.
      always_assert((per.inReg & per.inMem).none());
      for (auto const r : (per.inReg | per.inMem)) {
        // A Vreg should only be tracked if it's actually live at this
        // position, and a Vreg should be in the per-class state appropriate
        // for it.
        always_assert(!is_ignored(*state, r));
        always_assert(forReg(r) == &per);
        always_assert(!b.isValid() || live_in_at(*state, r, b, instIdx));
      }
      // A physical register can never be in memory
      for (auto const r : per.inMem) always_assert(!r.isPhys());
    };
    impl(gp);
    impl(simd);
    return true;
  }
};

// SpillerState at block boundaries, along with created Vregs which need SSA
// restoration.
struct SpillerResults {
  explicit SpillerResults(const State& state)
    : perBlock(state.unit.blocks.size())
    , changed{state.unit.blocks.size()} {}

  struct PerBlock {
    Optional<SpillerState> in;
    Optional<SpillerState> out;
    // Phi inputs/outputs. If a bit is set at index N, it means the phi
    // input/output at position N of the phi instruction is non-spilled.
    Optional<boost::dynamic_bitset<>> inPhi;
    Optional<boost::dynamic_bitset<>> outPhi;
  };
  jit::vector<PerBlock> perBlock;
  VregSet ssaize;
  // Which Vregs were rematerialized (they may also have been
  // reloaded).
  VregSet rematerialized;
  BlockSet changed;

  std::string toString(const State& state) const {
    auto ret = fmt::format(
      "SSAize: {}\nRematerialized: {}\n",
      show(ssaize),
      show(rematerialized)
    );
    for (auto const b : state.rpo) {
      auto const& per = perBlock[b];
      std::string inPhi;
      std::string outPhi;
      if (per.inPhi) {
        auto first = true;
        for (size_t i = 0; i < per.inPhi->size(); ++i) {
          if (!first) inPhi += ", ";
          inPhi += fmt::format("{}:{}", i, (*per.inPhi)[i] ? "reg" : "mem");
          first = false;
        }
      } else {
        inPhi = "*";
      }
      if (per.outPhi) {
        auto first = true;
        for (size_t i = 0; i < per.outPhi->size(); ++i) {
          if (!first) outPhi += ", ";
          outPhi +=
            fmt::format("{}:{}", i, (*per.outPhi)[i] ? "reg" : "mem");
          first = false;
        }
      } else {
        outPhi = "*";
      }
      ret += fmt::format(
        "  {:5}:{}\n"
        "    In      -> {}\n"
        "    Out     -> {}\n"
        "    In-Phi  -> {}\n"
        "    Out-Phi -> {}\n",
        b,
        changed[b] ? " (Changed)" : "",
        per.in ? per.in->toString() : "*",
        per.out ? per.out->toString() : "*",
        inPhi,
        outPhi
      );
    }
    return ret;
  }
};

// Recursive implementation for spill_weight_at (see below).
SpillWeightAt
spill_weight_at_impl(State& state,
                     const SpillerResults& spillerResults,
                     BlockSet& processed,
                     Vreg reg,
                     Vlabel startBlock,
                     size_t startRpo,
                     Vlabel b,
                     size_t instIdx) {
  auto& unit = state.unit;

  // We should never get here with a block that is before the
  // start block (such blocks have already been processed by the
  // spiller, thus they don't matter to future decisions).
  assertx(state.rpoOrder[b] >= startRpo);

  if (processed[b]) return SpillWeightAt{};
  processed[b] = true;

  // Check if the Vreg is no longer live. Don't do this for the start
  // block because the Vreg may not be live in to that block (it may
  // be defined within it).
  if (b != startBlock) {
    assertx(instIdx == 0);
    auto& firstInst = unit.blocks[b].code[0];
    if (firstInst.op == Vinstr::phidef) {
      // If the block begins with a phidef, it may define the current
      // Vreg.
      if (!state.liveIn[b][reg] && !defs_set_cached(state, firstInst)[reg]) {
        return SpillWeightAt{};
      }
    } else if (!state.liveIn[b][reg]) {
      return SpillWeightAt{};
    }
  }

  // Record a use of the current Vreg at the given instruction
  // index. If we can trivially rematerialize the Vreg at this point
  // and fold it into the instruction, do not consider it as a real
  // use and instead continue (returning std::nullopt). Otherwise,
  // returns the spill weight resulting from that usage.
  auto const recordUse = [&] (size_t idx) -> Optional<SpillWeightAt> {
    auto weight = block_weight(state, b);
    auto allTrivial = true;
    if (auto const trivial = is_trivial_remat(state, reg, b, idx)) {
      // We can rematerialize it trivially, check if we can fold the
      // rematerialization into this using instruction. Only attempt
      // this if we're not at the end of the block (which means
      // there's no actual instruction).
      if (idx < unit.blocks[b].code.size()) {
        auto& inst = unit.blocks[b].code[idx];
        if (can_fold_remat_with_use(state, *trivial, inst, reg)) {
          // We can fold it, so continue on.
          return std::nullopt;
        }
      }
    } else {
      // If this Vreg isn't trivially rematerializable, scale this
      // weight by the relative cost of a reload.
      weight *= kSpillReloadMultiplier;
      allTrivial = false;
    }

    assertx(idx >= instIdx);
    return SpillWeightAt{
      SpillWeight{weight, int64_t(idx - instIdx)},
      allTrivial
    };
  };

  // recordUse, but for situations where we shouldn't be folding
  // anything (for example with phis), so assert that.
  auto const recordUseNoFold = [&] (size_t idx) {
    auto const w = recordUse(idx);
    assertx(w);
    return *w;
  };

  // Record a use of the current Vreg by a copy instruction. These may
  // or may not result in an actual usage. If it does, return the
  // spill weight. Otherwise return std::nullopt.
  auto const recordCopyUse =
    [&] (Vreg dst, size_t idx, bool copy) -> Optional<SpillWeightAt> {
    // Copies are special. A copy to a physical register is treated
    // like a use. If the src is live after the copy, treat it like a
    // use as well. Otherwise we just start tracking the destination.
    if (dst.isPhys() || live_in_at(state, reg, b, idx + 1)) {
      return copy ? recordUse(idx) : recordUseNoFold(idx);
    } else {
      reg = dst;
      return std::nullopt;
    }
  };

  // Special case when dealing with loop headers for a loop that the
  // current block belongs to. Such blocks will always be already
  // processed (because it's a back edge). If the Vreg in question is
  // not used within the loop, we ignore this edge for the sake of
  // spill weight. Why? We assume that if we spill such Vregs (not
  // used within the loop), we'll later hoist the spill out of the
  // loop, meaning we won't have to do anything on the back
  // edge. This helps bias the spilling heuristic towards selecting
  // non-used within the loop Vregs to help facilitate this.
  auto const ignoreBackEdge = [&] (Vreg r, Vlabel succ) {
    if (!is_loop_header(state, succ)) return false;
    if (!block_in_loop(state, b, succ)) return false;
    auto const& loopInfo = loop_info(state, succ);
    return !loopInfo.uses[r];
  };

  // Examine each instruction in the current block, looking for
  // uses. For copyish instructions, start tracking the destination
  // Vreg. However, if the destination is a physical register, or if
  // the source is live after the copy, treat that as a use.
  auto& block = unit.blocks[b];
  if (state.uses[b][reg]) {
    for (size_t i = instIdx; i < block.code.size(); ++i) {
      auto& inst = block.code[i];

      switch (inst.op) {
        case Vinstr::copy:
          if (inst.copy_.s != reg) continue;
          if (auto const w = recordCopyUse(inst.copy_.d, i, true)) return *w;
          break;
        case Vinstr::copyargs: {
          auto const& srcs = unit.tuples[inst.copyargs_.s];
          auto const& dsts = unit.tuples[inst.copyargs_.d];
          assertx(srcs.size() == dsts.size());
          for (size_t j = 0; j < srcs.size(); ++j) {
            if (srcs[j] != reg) continue;
            if (auto const w = recordCopyUse(dsts[j], i, false)) return *w;
            break;
          }
          break;
        }
        case Vinstr::phijmp: {
          auto const successors = succs(block);
          assertx(successors.size() == 1);
          auto const succ = successors[0];
          auto const& phidef = unit.blocks[succ].code.front();
          assertx(phidef.op == Vinstr::phidef);
          auto const& srcs = unit.tuples[inst.phijmp_.uses];
          auto const& dsts = unit.tuples[phidef.phidef_.defs];
          assertx(srcs.size() == dsts.size());

          if (state.rpoOrder[succ] < startRpo) {
            // Special case: if this jump is a back edge to a block
            // which the spiller has already processed. We don't want to
            // examine this block like the others, but we can use the
            // already calculated spiller state. If the spiller has
            // determined that the phi def is non-spilled, we treat that
            // as a use as we'll have to insert a reload on the back
            // edge if the Vreg is spilled. If the spiller has
            // determined that the phi def is spilled, we remove some of
            // the spill weight corresponding to the weight at that
            // block. This is because by spilling this Vreg, we'll avoid
            // a spill on the back edge (so it removes some of the
            // penalty).
            auto const& spillerState = spillerResults.perBlock[succ].inPhi;
            assertx(spillerState.has_value());
            assertx(spillerState->size() == srcs.size());

            for (size_t j = 0; j < srcs.size(); ++j) {
              if (srcs[j] != reg) continue;
              if (dsts[j].isPhys()) {
                return recordUseNoFold(i);
              } else if ((*spillerState)[j]) {
                if (!ignoreBackEdge(srcs[j], succ)) return recordUseNoFold(i);
              } else {
                SpillWeightAt weight;
                weight.weight.usage -=
                  block_weight(state, b) * kSpillReloadMultiplier;
                return weight;
              }
            }
          } else {
            // Otherwise treat it like a copyargs
            for (size_t j = 0; j < srcs.size(); ++j) {
              if (srcs[j] != reg) continue;
              if (auto const w = recordCopyUse(dsts[j], i, false)) return *w;
              break;
            }
          }
          break;
        }
        default: {
          // A normal instruction. Shouldn't be a spill, reload, or
          // ssaalias because we should only examine blocks that the
          // spiller hasn't visited yet.
          assertx(!is_spill_inst(inst) &&
                  inst.op != Vinstr::reload &&
                  inst.op != Vinstr::ssaalias);
          if (!uses_set_cached(state, inst)[reg]) continue;
          // If this use "counts", return the subsequent
          // weight. Otherwise, if it doesn't (because we can fold the
          // use), ignore it and continue on.
          if (auto const w = recordUse(i)) return *w;
          break;
        }
      }
    }
  }

  // We didn't find any actual uses in this block. We need to now
  // process the successors.
  auto const successors = succs(block);
  if (successors.empty()) return SpillWeightAt{};

  // If there's more than one successor we have to recurse and combine
  // the results among the successors.
  if (successors.size() > 1) {
    SpillWeightAt weight;
    for (auto const succ : successors) {
      // Since critical edges are split, we cannot have any
      // back-edges here.
      assertx(state.rpoOrder[succ] >= state.rpoOrder[b]);
      auto const result = spill_weight_at_impl(
        state,
        spillerResults,
        processed,
        reg,
        startBlock,
        startRpo,
        succ,
        0
      );
      weight.allTrivial &= result.allTrivial;
      weight.weight.distance = std::min<int64_t>(
        weight.weight.distance,
        result.weight.distance + (unit.blocks[b].code.size() - instIdx)
      );
      weight.weight.usage += result.weight.usage;
    }
    return weight;
  }

  // Otherwise there's a single successor.

  auto const succ = successors[0];
  if (state.rpoOrder[succ] < startRpo) {
    // Special case: if the successor is a jump backwards to a block
    // which the spiller has already processed, we can examine the
    // spiller state to see what it has already decided there. If it
    // has decided the Vreg is not-spilled, that counts as a use
    // (we'll have to reload it on the back-edge). If it has decided
    // the Vreg is spilled, we remove some of the spill weight
    // corresponding to the weight at that block. This is because by
    // spilling this Vreg, we avoid a spill at the back edge. This
    // can all only happen (jumping to an already processed block)
    // if there's a single successor because we've split critical
    // edges.
    auto const& spillerState = spillerResults.perBlock[succ].in;
    assertx(spillerState.has_value());

    auto const s = spillerState->forReg(reg);
    assertx(s);
    if (s->inReg[reg]) {
      if (!ignoreBackEdge(reg, succ)) return recordUseNoFold(block.code.size());
    } else if (s->inMem[reg]) {
      SpillWeightAt weight;
      weight.weight.usage -= block_weight(state, b) * kSpillReloadMultiplier;
      return weight;
    }

    // We don't actually examine the successor in this case so
    // we're done.
    return SpillWeightAt{};
  }

  // Otherwise tail call to process the single successor.
  return spill_weight_at_impl(
    state,
    spillerResults,
    processed,
    reg,
    startBlock,
    startRpo,
    succ,
    0
  );
}

// Calculate the spill weight for the given Vreg at the given block
// and index into that block. Returns the spill weight, and whether
// all immediate uses of the given Vreg are trivially
// rematerializable.
SpillWeightAt
spill_weight_at(State& state,
                const SpillerResults& spillerResults,
                Vreg reg,
                Vlabel startBlock,
                size_t startInstIdx) {
  assertx(startBlock < state.unit.blocks.size());
  assertx(startInstIdx <= state.unit.blocks[startBlock].code.size());

  assertx(!reg.isPhys());
  assertx(reg_info(state, reg).regClass != RegClass::SF);

  /*
   * From the specified starting block, we walk the CFG. We look for a
   * usage of the specified Vreg. If found, we calculate the spill
   * weight as follows: The weight usage is set to the block weight
   * where the use happens. The weight distance is set to the number
   * of instructions between the starting point and the usage. The
   * "all trivial" flag is set to true if the Vreg is trivially
   * rematerializable at the point of the usage, false otherwise.
   *
   * Copyish instructions have some special treatment. Since they
   * generally preserve spill-ness, they are not usually considered as
   * uses. Instead we switch to looking for usages of their dest Vreg
   * instead. There are two exceptions. The first is when the copy
   * writes to a physical register. That is treated like a normal use
   * (since we'll be forced to emit a reload for that case). The
   * second is if the src Vreg is live after the copy. In that case,
   * we treat it like a normal use as well. The reasoning is that
   * we'll most likely emit a reload for that case (to avoid an
   * expensive memory to memory copy).
   *
   * If we reach the end of the block without a use, we continue into
   * the successor blocks. If the block has a single successor, we
   * tail call into processing it. If the block has multiple
   * successors, we recursively calculate the spill weights for each
   * successor, and then combine the results.
   *
   * We only examine blocks which the spiller hasn't processed
   * yet. This is because the choice of whether a Vreg is spilled or
   * not has already been fixed for those blocks, so it shouldn't
   * change the heuristic. Instead we check the spiller state to see
   * what has been decided for that Vreg. If it has been decided to
   * not be spilled in that block, we'll treat that as a use (because
   * we'll have to eventually emit a reload on the back edge to it).
   * Since the spiller workers in RPO order, we can determine whether
   * a block has been processed by just looking at the RPO index of
   * the blocks (is it before our first block?).
   *
   * Each block is only processed once (we keep a set of processed
   * blocks).
   */

  auto const startRpo = state.rpoOrder[startBlock];

  // Avoid infinite loops
  BlockSet processed(state.unit.blocks.size());
  return spill_weight_at_impl(
    state,
    spillerResults,
    processed,
    reg,
    startBlock,
    startRpo,
    startBlock,
    startInstIdx
  );
}

// Calculate information indicating which instructions within a block
// may actually generate spills. Such instructions can be processed
// more efficiently. If none of a block's instructions can spill, we
// can process the block more efficiently without processing
// individual instructions. If no blocks can spill, the spiller logic
// can be skipped entirely.

// Information about spilling needed on a per-instruction basis
struct SpillingNeededPerBlock {
  // We store per-instruction for the first 128 instructions. Beyond
  // that, we just use the block information.
  std::bitset<128> perInst;
  // True if any instruction in the block can spill. For instructions
  // beyond 128, this is all we know.
  bool any = false;

  bool operator()(size_t idx) const {
    if (idx < perInst.size()) return perInst[idx];
    return any;
  }
};
// Per-block spilling information
struct SpillingNeeded {
  jit::vector<SpillingNeededPerBlock> perBlock;
  // True if spilling is possible in any block
  bool any = false;
};

SpillingNeeded determine_spilling_needed(State& state) {
  auto& unit = state.unit;
  auto const gpLimit = state.gpUnreserved.size();
  auto const simdLimit = state.simdUnreserved.size();
  auto const commonLimit = std::min(gpLimit, simdLimit);

  auto const validRegs = state.gps | state.simds;

  // We can determine if a instruction might trigger a spill by
  // calculating the maximum register pressure within the
  // instruction. If it exceeds the number of physical registers, we
  // might need a spill. This calculation is conservative because we
  // assume all live Vregs will be in registers (in reality, some
  // might already be spilled). Thus, at worst, we overestimate the
  // register pressure, thinking we might have a spill when we won't.

  SpillingNeeded needsSpilling;
  needsSpilling.perBlock.resize(unit.blocks.size());

  for (auto const b : state.rpo) {
    // Extra coarse and quick check. If the sum of live-in Vregs and
    // Vregs defined with the block is still below the limit of both
    // reg classes, we know no spills can happen here, so skip the
    // block.
    if (((state.liveIn[b] | state.defs[b]) & validRegs).size() <= commonLimit) {
      continue;
    }

    // Start with the already stored liveness information. We track
    // GPs and SIMDs together in the same set, as this is more
    // efficient. If necessary we'll break them out into different
    // reg-classes.
    auto live = state.liveOut[b] & validRegs;

    // Check if the live Vregs exceed any of our limits
    auto const update = [&] (size_t idx) {
      // If the aggregate Vregs are all below the common limit, we
      // know it can't spill, even without splitting them out into
      // their reg-classes.
      if (live.size() <= commonLimit) return;

      // Quick check failed. Do per reg-class checks and mark this
      // instruction as potentially spilling.
      if ((live & state.gps).size() > gpLimit ||
          (live & state.simds).size() > simdLimit) {
        needsSpilling.any = true;
        auto& perBlock = needsSpilling.perBlock[b];
        perBlock.any = true;
        if (idx < perBlock.perInst.size()) perBlock.perInst[idx] = true;
      }
    };

    // Walk backwards modifying the liveness information on the fly.
    for (auto instIdx = unit.blocks[b].code.size(); instIdx > 0; --instIdx) {
      auto& inst = unit.blocks[b].code[instIdx - 1];

      auto const& defs = defs_set_cached(state, inst);
      auto const& uses = uses_set_cached(state, inst);
      auto const& acrosses = acrosses_set_cached(state, inst);

      live |= defs;
      live |= acrosses;

      // Deal with register pressure unfaithful instructions like the
      // spiller.
      if (defs.containsPhys() && uses.containsPhys()) {
        live.add(uses.physRegs());
      }

      live &= validRegs;
      update(instIdx - 1);

      live -= defs;
      live |= uses;

      live &= validRegs;
      update(instIdx - 1);
    }

    // What we've calculated for liveness should match the already stored
    // per-block liveness information at this point.
    assertx(live == state.liveIn[b]);
  }

  return needsSpilling;
}

// Update the phi spiller state for block b to account for which Vregs have been
// spilled or not.
void record_phi_spill_state_helper(Vlabel b,
                                   const VregList& defs,
                                   SpillerState& spillerState,
                                   SpillerResults& results) {
  auto& in = results.perBlock[b].inPhi.emplace();
  in.resize(defs.size(), true);
  for (size_t i = 0; i < defs.size(); ++i) {
    auto const r = defs[i];
    auto const s = spillerState.forReg(r);
    if (!s) continue;
    assertx(s->inReg[r] || s->inMem[r]);
    assertx(!r.isPhys() || s->inReg[r]);
    in[i] = s->inReg[r];
  }

  // Some of the phi outputs might be immediately dead, so drop them now.
  spillerState.dropDead(
    spillerState.gp.inReg | spillerState.gp.inMem |
    spillerState.simd.inReg | spillerState.simd.inMem,
    nullptr,
    b,
    0
  );
}

// Initialize the spiller state for entry into a block (populating the
// block's in state). This decides which Vregs should be considered
// spilled or non-spilled upon entry to the block. We take into
// account which Vregs have been spilled in predecessors, but we're
// not required to, since we'll fix up any mismatches later. If the
// spilling parameter indicates the first instruction doesn't need
// spilling, we can attempt a faster initialization. The starting
// instruction index is returned (to skip over any phi).
size_t setup_initial_spiller_state(State& state,
                                   Vlabel b,
                                   SpillerResults& results,
                                   const SpillingNeededPerBlock& spilling) {
  auto& unit = state.unit;
  SpillerState initial{state, state.spRecordedIn[b]};

  const VregList* phiDefs = nullptr;
  if (unit.blocks[b].code.front().op == Vinstr::phidef) {
    auto const& phidef = unit.blocks[b].code.front().phidef_;
    phiDefs = &unit.tuples[phidef.defs];
  }

  // If all of the predecessors have the same state for every Vreg
  // live into the block, we can perform a faster initialization,
  // since we can skip calculating spill weights to decide what to
  // spill. The initial state will just be what all the predecessors
  // have agreed upon.
  auto const fastInit = [&] {
    if (spilling(0) || state.preds[b].size() == 0) return false;
    // A single predecessor trivially agrees
    if (state.preds[b].size() == 1) return true;

    // There's multiple predecessors. For every live-in Vreg, check if
    // it's always in registers, or always in memory for all
    // predecessors.
    for (auto const r : state.liveIn[b]) {
      Optional<bool> inReg;
      for (auto const pred : state.preds[b]) {
        auto const& out = results.perBlock[pred].out;
        if (!out) continue;
        auto const inRegInPred = out->gp.inReg[r] || out->simd.inReg[r];
        if (!inReg) {
          inReg = inRegInPred;
        } else if (*inReg != inRegInPred) {
          return false;
        }
      }
    }

    if (!phiDefs) return true;

    // Same for Vregs defined by a phidef
    auto const size = phiDefs->size();
    for (size_t i = 0; i < size; ++i) {
      Optional<bool> inReg;
      for (auto const pred : state.preds[b]) {
        auto const& outPhi = results.perBlock[pred].outPhi;
        if (!outPhi) continue;
        if (!inReg) {
          inReg = (*outPhi)[i];
        } else if (*inReg != (*outPhi)[i]) {
          return false;
        }
      }
    }

    return true;
  }();

  if (fastInit) {
    // We can use the fast initialization. Since all the predecessors
    // agree about all Vregs, there's no decisions we have to make. We
    // can just copy their state as our initial state.

    // We can use any arbitrary pred to copy the state from (they're
    // all the same). We only have to skip over ones without any out
    // state, which means they were not processed yet (we ignored
    // those above when deciding to use fast init).
    size_t predIdx = 0;
    auto const& preds = state.preds[b];
    while (predIdx < preds.size() && !results.perBlock[preds[predIdx]].out) {
      ++predIdx;
    }
    assertx(predIdx < preds.size());
    // Copy the out state, removing any Vregs which aren't live-in.
    auto const& out = results.perBlock[preds[predIdx]].out;
    initial.gp.inReg = out->gp.inReg & state.liveIn[b];
    initial.simd.inReg = out->simd.inReg & state.liveIn[b];
    initial.gp.inMem = out->gp.inMem & state.liveIn[b];
    initial.simd.inMem = out->simd.inMem & state.liveIn[b];

    if (phiDefs) {
      // Do the same for any Vregs defined by a phidef
      auto const& outPhi = results.perBlock[preds[predIdx]].outPhi;
      assertx(outPhi);
      assertx(outPhi->size() == phiDefs->size());
      auto const& defs = *phiDefs;
      for (size_t i = 0; i < outPhi->size(); ++i) {
        auto s = initial.forReg(defs[i]);
        if (!s) continue;
        if ((*outPhi)[i]) {
          s->inReg.add(defs[i]);
        } else {
          s->inMem.add(defs[i]);
        }
      }
    }

    // And we're done. No calculating spill weights or invoking the
    // spiller.
  } else {
    // Otherwise we need to use the normal init since there might be a
    // disagreement among the predecessors.

    // Remember if this block is a loop header
    auto const loopInfo = is_loop_header(state, b)
      ? &loop_info(state, b)
      : nullptr;

    // First iterate over all (already processed) predecessors. For each
    // Vreg which is live-in to this block, we'll examine its state in
    // the predecessors, and sum the total block weights of the
    // predecessors where it's spilled and where it's not spilled. We'll
    // initially consider the Vreg to be whatever has the higher total
    // weight. We'll record the weight of the non-spilled preds so we
    // can bias the spilling decision below.
    jit::vector<std::pair<Vreg, int64_t>> spillCosts;
    if (b == state.unit.entry) {
      for (auto const r : state.liveIn[b]) {
        // Only physical registers should be live-in for the entry
        // block. They'll always be non-spilled.
        assertx(r.isPhys() && !is_ignored(state, r));
        auto const s = initial.forReg(r);
        if (!s) continue;
        s->inReg.add(r);
      }
    } else {
      for (auto const r : state.liveIn[b]) {
        auto initialState = initial.forReg(r);
        if (!initialState) continue;
        if (r.isPhys()) {
          initialState->inReg.add(r);
          continue;
        }

        // The penalty for assuming this Vreg is in a physical register
        // versus the penalty for assuming it is spilled.
        int64_t inRegPenalty = 0;
        int64_t inMemPenalty = 0;
        // The cost of spilling this Vreg. It is similar to inMemPenalty
        // except it does not take into account rematerialization (for
        // spill cost calculation we assume the spill will have to be
        // materialized).
        int64_t spillCost = 0;
        for (auto const pred : state.preds[b]) {
          auto const& out = results.perBlock[pred].out;
          if (!out) continue;

          // There shouldn't be anything live which isn't tracked.
          assertx(
            (state.liveIn[b] -
             (out->gp.inReg | out->simd.inReg | out->gp.inMem | out->simd.inMem)
            ).none()
          );

          // Sum up the block weights for each case
          auto const s = out->forReg(r);
          assertx(s);

          if (s->inReg[r]) {
            // If the Vreg is trivially rematerializable, there's no
            // penalty for considering it spilled, as we assume all
            // spills will be dead.
            //
            // Note that we're checking in b, not pred here. If r is
            // trivially rematerializable in b, then any uses should be
            // rematerialized, and thus the spills should become
            // dead. This isn't the case if it's trivially
            // rematerializable in pred.
            auto const trivial =
              !r.isPhys() && is_trivial_remat(state, r, b, !!phiDefs);
            auto const cost =
              block_weight(state, pred) * kSpillReloadMultiplier;
            if (!trivial) inMemPenalty += cost;
            spillCost += cost;
          } else {
            assertx(!r.isPhys());
            assertx(s->inMem[r]);
            // If the Vreg is spilled in the predecessor, then we'll
            // have to reload it if we want it to be non-spilled in this
            // block. The cost of the reload depends if it's trivially
            // rematerializable or not.
            auto const trivial =
              is_trivial_remat(state, r, pred, unit.blocks[pred].code.size());
            inRegPenalty +=
              block_weight(state, pred) *
              (trivial ? 1 : kSpillReloadMultiplier);
          }
        }

        // Special case: If this is a loop header, and the Vreg is used
        // within the loop, bias the spill penalty taking into account
        // future usages. For example, if a Vreg is spilled on all
        // predecessors coming into the loop header, we might still want
        // to reload it.
        if (loopInfo && loopInfo->uses[r]) {
          inMemPenalty +=
            spill_weight_at(state, results, r, b, !!phiDefs).weight.usage;
        }

        // Pick the majority case
        if (inMemPenalty >= inRegPenalty) {
          initialState->inReg.add(r);
          // We might have to spill this anyways below, so record the
          // penalty of doing so (the weight of all the predecessors
          // where it's not spilled).
          if (!r.isPhys()) spillCosts.emplace_back(r, spillCost);
        } else {
          assertx(!r.isPhys());
          initialState->inMem.add(r);
        }
      }
    }

    // Now that we've processed Vregs which were live-in to the block,
    // we need to consider phidef outputs. We can use similar logic to
    // the live-in Vregs, except examining the phi state of the
    // predecessors.
    if (phiDefs) {
      assertx(b != unit.entry);
      assertx(
        !defs_set_cached(state, unit.blocks[b].code.front()).containsPhys()
      );
      assertx(uses_set_cached(state, unit.blocks[b].code.front()).none());

      auto const size = phiDefs->size();
      for (size_t i = 0; i < size; ++i) {
        auto const def = (*phiDefs)[i];

        auto initialState = initial.forReg(def);
        if (!initialState) continue;

        int64_t inRegPenalty = 0;
        int64_t inMemPenalty = 0;
        int64_t spillCost = 0;
        for (auto const pred : state.preds[b]) {
          auto const& out = results.perBlock[pred].outPhi;
          if (!out) continue;

          assertx(out->size() == size);

          assertx(unit.blocks[pred].code.back().op == Vinstr::phijmp);
          auto const& phijmp = unit.blocks[pred].code.back().phijmp_;
          auto const& uses = unit.tuples[phijmp.uses];
          assertx(uses.size() == size);

          if ((*out)[i]) {
            // Note that for the same reasons as above, we're checking
            // in b, not in pred. We can only save on the penalty if it's
            // trivially rematerializable at this point.
            auto const trivial =
              !def.isPhys() && is_trivial_remat(state, def, b, 1);
            auto const cost =
              block_weight(state, pred) * kSpillReloadMultiplier;
            if (!trivial) inMemPenalty += cost;
            spillCost += cost;
          } else {
            assertx(!def.isPhys());
            auto const trivial = is_trivial_remat(
              state,
              uses[i],
              pred,
              unit.blocks[pred].code.size()
            );
            inRegPenalty +=
              block_weight(state, pred) *
              (trivial ? 1 : kSpillReloadMultiplier);
          }
        }

        // Same special case as above. Bias Vregs used within the loop
        // towards being in registers.
        if (loopInfo && loopInfo->uses[def]) {
          inMemPenalty +=
            spill_weight_at(state, results, def, b, 1).weight.usage;
        }

        if (inMemPenalty >= inRegPenalty) {
          initialState->inReg.add(def);
          if (!def.isPhys()) spillCosts.emplace_back(def, spillCost);
        } else {
          assertx(!def.isPhys());
          initialState->inMem.add(def);
        }
      }
    }

    // We might have more non-spilled Vregs than we can fit into
    // registers. Spill as necessary. Use the recorded weights above to
    // bias the decision appropriately.
    initial.spill(
      b, !!phiDefs, results,
      [&] (Vreg r) {
        auto const it = std::find_if(
          spillCosts.begin(), spillCosts.end(),
          [&] (const std::pair<Vreg, int64_t>& p) { return p.first == r; }
        );
        return (it != spillCosts.end()) ? it->second : 0;
      }
    );
  }

  // If this block has a phidef, we need to record which outputs of the phi were
  // considered to be spilled or not.
  assertx(!results.perBlock[b].inPhi);
  if (phiDefs) {
    record_phi_spill_state_helper(
      b,
      *phiDefs,
      initial,
      results
    );
  }

  // Record block state
  assertx(!results.perBlock[b].in);
  assertx(initial.checkInvariants(b, !!phiDefs));
  results.perBlock[b].in = std::move(initial);

  return !!phiDefs;
}

// Run spiller logic for a phijmp instruction at the given block and
// instruction index. Phis can handle spilled Vregs, so we don't
// necessarily need to reload the phi's inputs. However it might be
// profitable to do so in some situations. Namely, this function tries
// to minimize any memory to memory copies and to shorten spill slot
// lifetimes even if it cannot kill the spill entirely. Returns the
// number of instructions inserted, if any.
size_t process_phijmp_spills(State& state,
                             Vlabel b,
                             size_t instIdx,
                             Vinstr& phijmp,
                             SpillerState& spiller,
                             SpillerResults& results) {
  assertx(phijmp.op == Vinstr::phijmp);
  auto& unit = state.unit;

  // This should end a block
  assertx(instIdx == unit.blocks[b].code.size() - 1);
  assertx(defs_set_cached(state, phijmp).none());
  assertx(acrosses_set_cached(state, phijmp).none());

  assertx(unit.blocks[phijmp.phijmp_.target].code.front().op == Vinstr::phidef);
  auto const& phidef = unit.blocks[phijmp.phijmp_.target].code.front().phidef_;

  // The Vregs available for use in rematerialization
  // instructions. This is the set of Vregs not spilled (and will be
  // modified as we spill/reload Vregs).
  auto rematAvail = spiller.gp.inReg | spiller.simd.inReg;

  auto& uses = unit.tuples[phijmp.phijmp_.uses];

  // A "triviality boundary" is a phijmp for which the source is
  // trivially rematerializable, but the def is not. If the def is
  // used, we won't be able to rematerialize (because we've determined
  // it's not rematerializable). Therefore the inputs to the phi will
  // be kept alive (even if the inputs are trivially
  // rematerializable), which means we might keep a spill slot
  // containing an immediate live. To remedy this, we want to reload
  // (with rematerialization) such Vregs at the phijmp side of the
  // boundary. This kills the lifetime of the spill slot going into
  // the phi.
  auto const trivialityBoundary = [&] (Vreg r, size_t i) {
    if (!is_trivial_remat(state, r, b, instIdx)) return false;
    auto const def = unit.tuples[phidef.defs][i];
    return !is_trivial_remat(state, def, phijmp.phijmp_.target, 1);
  };

  // A promoted Vreg is a spilled Vreg used by the phi, but which we
  // want to reload anyways. We don't have to do this, because phis
  // can handle spilled Vregs fine. There are two reasons why we would
  // want to. The first is the triviality boundary case described
  // above. We want to shorten the lifetime of the input spill slot
  // (because we know it won't be killed despite being trivially
  // rematerializable). The second is if the input is live across the
  // phi. In that case, the phi is *always* going to cause a memory to
  // memory copy because we'll never be able to coalesce the phi src
  // and dest. So, we'll attempt to reload (with rematerialization)
  // the src. Even if we can't reload it, we might be able to spill to
  // dst without doing a memory to memory copy.
  //
  // The promotion map is a map of input Vregs to new Vregs created
  // which will become the new inputs to the phi. We'll
  // reload/rematerialize into the new Vregs and have the phi take
  // those instead.
  jit::flat_map<Vreg, Vreg> promotions;
  for (size_t i = 0; i < uses.size(); ++i) {
    auto const r = uses[i];
    auto const s = spiller.forReg(r);
    if (!s) continue;
    assertx(s->inReg[r] || s->inMem[r]);
    assertx(!r.isPhys() || s->inReg[r]);
    if (!s->inMem[r]) continue; // Not spilled, leave it be
    if (!state.liveOut[b][r] && !trivialityBoundary(r, i)) continue;
    // If it's one of the above cases, allocate a new Vreg (if not
    // already done), and record it. Assume this Vreg is not spilled
    // (we'll spill it as necessary below).
    auto const it = promotions.find(r);
    if (it != promotions.end()) continue;
    auto const d = unit.makeReg();
    auto const& info = reg_info(state, r);
    set_reg_class_bits(state, d, info.regClass);
    reg_info_insert(state, d, info);
    promotions.emplace(r, d);
    s->inReg.add(d);
  }

  // Before the promotions, we shouldn't have needed to spill
  // anything. However the addition of them might have caused us to
  // exceed the number of physical registers, we have to spill
  // here. Note that we might spill Vregs which were originally
  // non-spilled, not just the promoted ones. This is fine, as the
  // heuristic has determined it's more profitable to do so.
  auto spills = spiller.spill(
    b, instIdx + 1, results,
    [&] (Vreg) { return block_weight(state, b) * kSpillReloadMultiplier; }
  );
  // We shouldn't spill if there were no promotions
  assertx(IMPLIES(spills.any(), !promotions.empty()));

  // Now that we've spilled any Vregs as necessary, we need to check
  // the Vregs we created as part of the promotions above. If the
  // created Vreg was not spilled, we'll need to reload (possibly with
  // rematerialization) from the original Vreg into the created Vreg
  // (the original Vreg was spilled or else we wouldn't have done a
  // promotion with it). Otherwise, if the created Vreg was spilled,
  // we don't actually have to do anything (because the original Vreg
  // was already spilled). However, we want to try to spill an
  // immediate into the created Vreg if possible (avoiding a memory to
  // memory copy).

  jit::vector<std::pair<Vreg, Vreg>> reloads; // Emit reload p1 -> p2
  jit::vector<std::pair<Vreg, Vreg>> rematSpills; // Emit spill[b,l,q]i * -> p2
  for (auto const& p : promotions) {
    auto const u = p.first;
    auto const d = p.second;
    auto const s = spiller.forReg(u);
    assertx(s);
    assertx(spiller.forReg(d) == s);

    assertx(s->inMem[u]);
    assertx(!spills[u]);

    if (s->inReg[d]) {
      assertx(!spills[d]);
      // Promotion target is not spilled, we'll need to reload
      reloads.emplace_back(u, d);
    } else {
      assertx(s->inMem[d]);
      assertx(spills[d]);
      // Otherwise it got spilled. Remove it from "spills" because we
      // don't actually need to spill it, but try to rematerialize it
      // directly into the destination spill slot.
      spills.remove(d);
      rematSpills.emplace_back(u, d);
    }
  }

  // We have to spill. Make sure we can.
  always_assert_flog(
    IMPLIES(!spiller.spRecorded, spills.none()),
    "Trying to spill for {} in {} when not allowed (sp not recorded) "
    "(Spills: {})",
    show(state.unit, phijmp),
    b,
    show(spills)
  );

  if (debug) {
    for (auto const r : spills) always_assert(spiller.forReg(r));
  }

  results.ssaize |= spills;

  // Materialize the actual spills and reloads. The order here is
  // important.
  size_t added = 0;
  if (spills.any() || !reloads.empty() || !rematSpills.empty()) {
    vmodify(
      unit, b, instIdx,
      [&] (Vout& v) {
        // First try to rematerialize spills directly. This can
        // conditionally succeed. If it fails (because we cannot
        // rematerialize), do nothing. The phi will just handle the
        // copy like it did originally.
        //
        // We can do this first because it should neither increase or
        // decrease register pressure.
        for (auto const& p : rematSpills) {
          assertx(!rematAvail[p.first]);
          assertx(!rematAvail[p.second]);

          auto const s = spiller.forReg(p.second);
          assertx(s);
          assertx(s->inMem[p.second]);
          auto const spillResults = spill_with_remat(
            state,
            v,
            b,
            instIdx,
            rematAvail,
            p.first,
            p.second,
            false, // Dst is a spilled, so it can't be used as a remat target
            false // Src is spilled, so don't emit a spill instruction
          );
          if (spillResults.rematerialized) results.rematerialized.add(p.first);
          assertx(state.spilled);
          if (spillResults.added > 0) {
            // Success, record the added instruction
            added += spillResults.added;
          } else {
            // Failure. We're going to leave this phi input as is, so
            // forget about the promotion target.
            s->inMem.remove(p.second);
          }
        }

        // Now emit the spills to reduce register pressure
        for (auto const r : spills) {
          auto const spillResults = spill_with_remat(
            state,
            v,
            b,
            instIdx,
            rematAvail,
            r,
            r,
            true,
            true
          );
          added += spillResults.added;
          if (spillResults.rematerialized) results.rematerialized.add(r);
          // No longer available for rematerialization
          rematAvail.remove(r);
          state.spilled = true;
        }

        // Finally perform the reloads (which increases register
        // pressure).
        for (auto const& p : reloads) {
          auto const inst = reload_with_remat(
            state,
            b,
            instIdx,
            rematAvail,
            p.first,
            p.second
          );
          if (inst.op != Vinstr::reload) results.rematerialized.add(p.first);
          emit_reload(v, inst);
          ++added;
          // The destination is now available for rematerialization.
          rematAvail.add(p.second);
        }
        return 0;
      }
    );

    results.changed[b] = true;
  }
  // NB: Can't access phijmp anymore, since the vmodify may have
  // invalidated it.

  // Examine which Vregs are currently non-spilled and record them in
  // the phi state. Also fixup the phi inputs for any surviving
  // promotions.
  assertx(!results.perBlock[b].outPhi);
  auto& outPhi = results.perBlock[b].outPhi.emplace();
  outPhi.resize(uses.size(), true);
  for (size_t i = 0; i < uses.size(); ++i) {
    auto const r = uses[i];
    auto const s = spiller.forReg(r);
    if (!s) continue;

    auto const it = promotions.find(r);
    if (it != promotions.end()) {
      // This phi input got promoted. We'll instead look at the state
      // of the promotion target.
      auto const d = it->second;
      assertx(spiller.forReg(d) == s);
      assertx(!spills[d]);
      assertx(!s->inReg[r]);

      // The promotion target can be inReg, inMem, or neither here. If
      // inReg it got reloaded, if inMem we rematerialized it into the
      // spill slot directly, and if neither we couldn't do anything
      // with it. The first case is the only time the phi state should
      // be marked as being non-spilled.
      outPhi[i] = s->inReg[d];

      if (s->inReg[d] || s->inMem[d]) {
        // The promotion target was reloaded or rematerialized into a
        // spill slot. In either case, rewrite the phi input at this
        // index to take the target instead.
        if (!state.liveOut[b][r]) {
          // Manually remove a dead phi input because it will no
          // longer be part of the uses VregList, so dropDead won't
          // remove it below.
          s->inMem.remove(r);
        } else {
          assertx(s->inMem[r]);
        }

        assertx(uses[i] == r);
        uses[i] = d;
        invalidate_cached_operands(unit.blocks[b].code[instIdx + added]);
        results.changed[b] = true;
      } else {
        // Otherwise we didn't do anything with the target. The phi
        // will be untouched at this index.
        assertx(s->inMem[r]);
      }
    } else {
      // Not part of a promotion, just record its final state.
      assertx(s->inReg[r] || s->inMem[r]);
      assertx(!r.isPhys() || s->inReg[r]);
      outPhi[i] = s->inReg[r];
    }
  }

  // Remove any Vregs which aren't used after the phi so they won't be part of
  // the block's live-out.
  spiller.dropDead(
    uses_set_cached(state, unit.blocks[b].code[instIdx + added]),
    nullptr,
    b,
    instIdx + added
  );

  if (debug) {
    // Promotion targets should always be dead at this point, as they
    // exist only to be inputs for the phi.
    for (auto const& p : promotions) {
      auto const d = p.second;
      auto const s = spiller.forReg(d);
      always_assert(!s->inReg[d] && !s->inMem[d]);
    }
  }

  return added;
}

// Run spiller logic for a copy-ish instruction with no physical
// dests. Copies need to be handled specially because unlike normal
// instructions, they can handle both spilled and non-spilled Vregs
// (therefore we don't need to reload the inputs before the
// instruction). We only need to ensure that a copy doesn't attempt to
// move between a spilled or non-spilled Vreg (or vice-versa). The
// copy can trigger reloads in some circumstances (to avoid memory to
// memory copies), but will never add to register pressure. Return the
// total number of instructions inserted, and the total number of
// instructions inserted *before* the copy.

struct ProcessCopyResults {
  size_t added;
  size_t addedBefore;
};
ProcessCopyResults process_copy_spills(State& state,
                                       VregList& uses,
                                       VregList& defs,
                                       Vlabel b,
                                       size_t instIdx,
                                       SpillerState& spiller,
                                       SpillerResults& results) {
  assertx(uses.size() == defs.size());

  auto& unit = state.unit;

  // Record which uses of the copy are currently spilled or not before
  // the copy. We can't just use the spiller state later because we'll
  // drop dead uses from it.
  VregSet initialInReg;
  VregSet initialInMem;
  for (size_t i = 0; i < uses.size(); ++i) {
    auto const u = uses[i];
    auto const s = spiller.forReg(u);
    if (!s) {
      assertx(!spiller.forReg(defs[i]));
      continue;
    }
    assertx(s->inReg[u] || s->inMem[u]);
    assertx(spiller.forReg(defs[i]));
    assertx(!u.isPhys() || s->inReg[u]);
    assertx(!defs[i].isPhys());
    assertx(!initialInReg[defs[i]] && !initialInMem[defs[i]]);
    if (s->inReg[u]) {
      initialInReg.add(u);
    } else {
      initialInMem.add(u);
    }
  }

  // Keep track of which Vregs aren't spilled, so we know which ones
  // are available for rematerialization.
  auto rematAvail = spiller.gp.inReg | spiller.simd.inReg;

  // Remove any Vregs which aren't used after the copy from being tracked.
  VregSet defsSet{defs};
  spiller.dropDead(VregSet{uses}, &defsSet, b, instIdx);

  /*
   * Now setup our initial assumptions for the def Vreg states, using
   * the current state of the src Vregs. There's several cases:
   *
   * #1 Src not spilled -> Dst not spilled
   *
   * #2 Src spilled, dead after copy -> Dst spilled
   *
   * #3 Src spilled, live after copy -> Dst not spilled
   *
   * The rational for #3 is as follows: If the src and dst are both
   * spilled, and the src is live across the copy, we'll *have* to
   * lower this to a memory to memory copy (we'll never be able to
   * coalesce the two slots together as they naturally interfer). By
   * initially assuming the dst is non-spilled, we have to chance to
   * load it into a register (avoiding the store), or possibly
   * rematerialize it directly into the destination spill slot
   * (avoiding the load). At worst, the spiller will decide it needs to
   * be in memory anyways, and we'll be no worse off.
   */
  for (size_t i = 0; i < defs.size(); ++i) {
    auto const d = defs[i];
    auto const u = uses[i];
    auto const s = spiller.forReg(d);
    if (!s) {
      assertx(!initialInReg[u] && !initialInMem[u]);
      assertx(!spiller.forReg(u));
      continue;
    }
    // Since this is being defined here, we shouldn't already be tracking it.
    assertx(!d.isPhys());
    assertx(!s->inReg[d] && !s->inMem[d]);
    assertx(spiller.forReg(u));
    if (initialInReg[u]) {
      s->inReg.add(d);
    } else {
      assertx(initialInMem[u]);
      auto const usesState = spiller.forReg(u);
      assertx(usesState);
      assertx(!usesState->inReg[u]);
      // Easy check if the src is live across the copy. It can't be
      // inReg here, so if it's not inMem, it's dead.
      if (usesState->inMem[u]) {
        s->inReg.add(d);
      } else {
        s->inMem.add(d);
      }
    }
  }

  // Adding the defs may have exceeded the available registers, so spill as
  // necessary.
  auto const spills = spiller.spill(
    b, instIdx + 1, results,
    [&] (Vreg) { return block_weight(state, b) * kSpillReloadMultiplier; }
  );

  // We have to spill. Make sure we can.
  always_assert_flog(
    IMPLIES(!spiller.spRecorded, spills.none()),
    "Trying to spill for {} in {} when not allowed (sp not recorded) "
    "(Spills: {})",
    show(state.unit, unit.blocks[b].code[instIdx]),
    b,
    show(spills)
  );

  if (debug) {
    for (auto const r : spills) always_assert(spiller.forReg(r));
  }

  // We know which Vregs we want to spill. Copys can handle copying between
  // spilled Vregs on both sides and non-spilled Vregs on both sides (but not
  // mixing). Moving between spilled and non-spilled Vregs is the job of the
  // spill and reload instructions. So, if we need to spill one side of the copy
  // and not the other (and vice-versa), we need to insert other instructions to
  // keep this invariant.

  jit::vector<std::pair<Vreg, Vreg>> aliasPairs; // Emit ssaalias p1 -> p2
  jit::vector<std::pair<Vreg, Vreg>> spillPairs; // Emit spill p1 -> p2
  jit::vector<std::pair<Vreg, Vreg>> reloads; // Emit reload p1 -> p2
  // Attempt store[b,l,q]i * -> p2
  jit::vector<std::tuple<Vreg, Vreg, size_t>> rematSpills;
  // Rewrite uses[p1] = p2
  jit::vector<std::pair<size_t, Vreg>> useRewrites;
  // Rewrite defs[p1] = p2
  jit::vector<std::pair<size_t, Vreg>> defRewrites;
  auto selfSpills = spills; // Emit spill p -> p
  for (size_t i = 0; i < uses.size(); ++i) {
    auto const u = uses[i];
    auto const d = defs[i];

    if (initialInReg[u]) {
      // Src started out in a physical register. It might have been
      // spilled.
      if (spills[u]) {
        assertx(!u.isPhys());
        auto const s = spiller.forReg(d);
        if (!s) continue;
        if (s->inReg[d]) {
          /*
           * The src has been spilled, but dst has not. We need to
           * make the input and output of the copy agree with
           * regards to the spillness. We can't reload src because
           * we've already decided we don't have the available
           * registers to do that. Instead we'll spill src (because
           * it's in selfSpills). We'll emit a ssaalias instruction,
           * aliasing src to dst, rewrite the copy to take dst
           * instead, and mark dst as needing SSA restoration.
           *
           * For example:
           *   copy src -> dst
           *   conjureuse dst
           *
           * Becomes:
           *   ssaalias src -> dst
           *   spill src -> src
           *   copy dst -> dst
           *   conjureuse dst
           *
           * After SSA:
           *   spill src -> src2
           *   copy src -> dst
           *   conjureuse dst
           */
          assertx(!spills[d]);
          aliasPairs.emplace_back(u, d);
          results.ssaize.add(d);
          useRewrites.emplace_back(i, d);
        } else {
          /*
           * Both the src and dst have been spilled. The copy can
           * handle spilled Vregs just fine. We'll spill src both to
           * dst and itself (in that order), rewrite the copy to take
           * dst instead, and mark dst as needing SSA
           * restoration. Instead we could spill src and then copy src
           * to dst. However this would be inefficient, because the
           * two spilled Vregs are simultaneously live, thus the copy
           * would always need a memory to memory transfer.
           *
           * For example:
           *   copy src -> dst
           *   conjureuse dst
           *
           * Becomes:
           *   spill src -> dst
           *   spill src -> src
           *   copy dst -> dst
           *   conjureuse dst
           *
           * After SSA:
           *   spill src -> dst
           *   spill src -> src2
           *   copy dst -> dst2
           *   conjureuse dst2
           */
          assertx(s->inMem[d]);
          assertx(!d.isPhys());
          spillPairs.emplace_back(u, d);
          selfSpills.remove(d);
          results.ssaize.add(d);
          useRewrites.emplace_back(i, d);
        }
      } else {
        auto const s = spiller.forReg(d);
        if (s && s->inMem[d]) {
          /*
           * The src isn't spilled, but dst has been. The copy can
           * handle spilled Vregs just fine, so we'll emit a spill
           * instruction before the copy, spilling the src into the
           * dst. We'll rewrite the copy to take dst as the src. We'll
           * mark dst as needing SSA conversion, so the copy will get
           * a new Vreg as its output and all down stream users will
           * be rewritten. This is near identical to the case of both
           * src and dst being spilled, except we don't need to spill
           * src to itself.
           *
           * For example:
           *   copy src -> dst
           *   conjureuse dst
           *
           * Becomes:
           *   spill src -> dst
           *   copy dst -> dst
           *   conjureuse dst
           *
           * After SSA:
           *   spill src -> dst
           *   copy dst -> dst2
           *   conjureuse dst2
           */
          assertx(!d.isPhys());
          spillPairs.emplace_back(u, d);
          selfSpills.remove(d);
          results.ssaize.add(d);
          useRewrites.emplace_back(i, d);
        }
      }
    } else {
      // Src started out in memory. It can't have been spilled.
      assertx(initialInMem[u]);
      assertx(!u.isPhys());
      assertx(!spills[u]);
      auto const s = spiller.forReg(d);
      if (!s) continue;
      if (s->inReg[d]) {
        /*
         * Src started out in memory, but dst is not spilled. This
         * can happen if src is live across the copy and we decided
         * it's profitable for the dst to be in a physical
         * register. We'll rewrite the copy to write to src instead
         * of dst (so the copy becomes a nop), and emit a reload
         * from src to dst after the copy. We cannot reload into dst
         * before the copy as register pressure might not allow that
         * (we need for any of the copy srcs to become dead).
         *
         * For example:
         *   copy src -> dst
         *   conjureuse src
         *   conjureuse dst
         *
         * Becomes:
         *   copy src -> src
         *   reload src -> dst
         *   conjureuse src
         *   conjureuse dst
         *
         * After SSA:
         *   copy src1 -> src2
         *   reload src2 -> dst
         *   conjureuse src2
         *   conjureuse dst
         *
         * (with src1 and src2 being later coalesced together, removing
         * the copy).
         */
        assertx(!spills[d]);
        reloads.emplace_back(u, d);
        results.ssaize.add(u);
        defRewrites.emplace_back(i, u);
      } else {
        /*
         * Src started out in memory, and dst is spilled. This is
         * expected if src is not live across the copy (and does not
         * generate anything). However, if src is live across the
         * copy, we want to try to rematerialize directly into the
         * spill slot to avoid an expensive memory to memory
         * copy. If we cannot, we do nothing and let the phi handle
         * it as normal. If successful, we change the copy to take
         * the dst as both its input and output, making it a no-op.
         *
         * For example:
         *   ldimmq 1 -> src1
         *   spill src1 -> src2
         *   copy src2 -> dst
         *   conjureuse dst
         *
         * Becomes (if successful):
         *   ldimmq 1 -> src1
         *   spill src1 -> src2
         *   spillqi 1 -> dst
         *   copy dst -> dst
         *   conjureuse dst
         *
         * After SSA (if successful):
         *   ldimmq 1 -> src1
         *   spill src1 -> src2
         *   spillqi 1 -> dst1
         *   copy dst1 -> dst2
         *   conjureuse dst2
         *
         * (with dst1 and dst2 being later coalesced together,
         * removing the copy).
         */
        assertx(s->inMem[d]);
        selfSpills.remove(d);
        auto const usesState = spiller.forReg(u);
        assertx(usesState);
        assertx(!usesState->inReg[u]);
        // If the use is still in memory, it's live across the copy.
        if (usesState->inMem[u]) rematSpills.emplace_back(u, d, i);
      }
    }
  }

  // Anything self spilled needs to be SSA restored because we use the same Vreg
  // as the source and dest.
  results.ssaize |= selfSpills;

  // Now that we've calculated everything, emit the actual instructions. We need
  // to emit them in this particular order for correctness.

  // First the instructions which come before the copy instruction
  size_t addedBefore = 0;

  if (!aliasPairs.empty() ||
      !spillPairs.empty() ||
      !rematSpills.empty() ||
      selfSpills.any()) {
    vmodify(
      unit, b, instIdx,
      [&] (Vout& v) {
        // First alias pairs, which don't change register pressure.
        VregSet usedByAlias;
        for (auto const& p : aliasPairs) {
          v << ssaalias{p.first, p.second};
          usedByAlias.add(p.first);
          ++addedBefore;
        }

        // Then the spills which decrease register pressure.
        for (auto const& p : spillPairs) {
          auto const spillResults = spill_with_remat(
            state,
            v,
            b,
            instIdx,
            rematAvail,
            p.first,
            p.second,
            false, // Dst has been spilled, so cannot be used as a
                   // remat target without increasing register
                   // pressure.
            true
          );
          addedBefore += spillResults.added;
          if (spillResults.rematerialized) results.rematerialized.add(p.first);
          // The dest of the spill is no longer available for
          // rematerialization.
          rematAvail.remove(p.second);
          state.spilled = true;
        }

        // Now attempt rematerializing directly into spill slots,
        // which might fail. This doesn't change register pressure.
        for (auto const& p : rematSpills) {
          auto const src = std::get<0>(p);
          auto const dst = std::get<1>(p);
          auto const spillResults = spill_with_remat(
            state,
            v,
            b,
            instIdx,
            rematAvail,
            src,
            dst,
            false, // dst is spilled, so cannot be used as a remat target
            false // src is spilled, so don't emit a spill instruction
          );
          if (spillResults.rematerialized) results.rematerialized.add(src);
          assertx(state.spilled);
          assertx(!rematAvail[src]);
          assertx(!rematAvail[dst]);
          if (spillResults.added > 0) {
            // Success. Rewrite the use of the copy to be the dst.
            auto const idx = std::get<2>(p);
            useRewrites.emplace_back(idx, dst);
            results.ssaize.add(dst);
            addedBefore += spillResults.added;
          }
          // Otherwise do nothing, the copy will handle it as normal
        }

        // Then the self spills which reduces register pressure.
        for (auto const r : selfSpills) {
          auto const spillResults = spill_with_remat(
            state,
            v,
            b,
            instIdx,
            rematAvail,
            r,
            r,
            !usedByAlias[r], // r can only be used as a remat target if
                             // it isn't used by a ssaalias (otherwise
                             // it needs to be kept unchanged across the
                             // spill)
            true
          );
          addedBefore += spillResults.added;
          if (spillResults.rematerialized) results.rematerialized.add(r);
          // Spilled Vregs are no longer available for
          // rematerialization.
          rematAvail.remove(r);
          state.spilled = true;
        }
        return 0;
      }
    );

    results.changed[b] = true;
  }

  // Then the reloads which must come after the copy (because of
  // register pressure).
  size_t addedAfter = 0;
  if (!reloads.empty()) {
    vmodify(
      unit, b, instIdx + addedBefore + 1,
      [&] (Vout& v) {
        for (auto const& p : reloads) {
          auto const inst = reload_with_remat(
            state,
            b,
            instIdx,
            rematAvail,
            p.first,
            p.second
          );
          if (inst.op != Vinstr::reload) results.rematerialized.add(p.first);
          emit_reload(v, inst);
          ++addedAfter;
          // Anything reloaded is now available for rematerialization.
          rematAvail.add(p.second);
        }
        return 0;
      }
    );

    results.changed[b] = true;
  }

  // Now that all instructions have been rewritten, update the uses
  // and defs VregLists for the copy. We defer this until the end
  // because the spill/reload rematerialization logic may try to look
  // through this same copy instruction, and we want the uses/defs to
  // reflect their original values, not some partial changes.
  if (!useRewrites.empty() || !defRewrites.empty()) {
    for (auto const& p : useRewrites) {
      assertx(p.first < uses.size());
      uses[p.first] = p.second;
    }
    for (auto const& p : defRewrites) {
      assertx(p.first < defs.size());
      defs[p.first] = p.second;
    }
    invalidate_cached_operands(unit.blocks[b].code[instIdx + addedBefore]);
    results.changed[b] = true;
  }

  // Remove any Vregs which the copy defined and are immediately dead.
  spiller.dropDead(defsSet, nullptr, b, instIdx + addedBefore + addedAfter);

  return {addedBefore + addedAfter, addedBefore};
}

size_t process_unrecordbasenativesp_spills(State& state,
                                           Vlabel b,
                                           size_t instIdx,
                                           SpillerState& spiller,
                                           SpillerResults& results) {
  auto rematAvail = spiller.gp.inReg | spiller.simd.inReg;
  auto const reloads = spiller.inMem();
  size_t added = 0;
  results.ssaize |= reloads;
  spiller.dropDead(reloads, nullptr, b, instIdx);
  for (auto const r : reloads) {
    auto s = spiller.forReg(r);
    s->inMem.remove(r);
    s->inReg.add(r);
  }
  vmodify(
    state.unit, b, instIdx,
    [&] (Vout& v) {
      for (auto const r : reloads) {
        assertx(!r.isPhys());
        auto const inst = reload_with_remat(
          state,
          b,
          instIdx,
          rematAvail,
          r,
          r
        );
        if (inst.op != Vinstr::reload) results.rematerialized.add(r);
        emit_reload(v, inst);
        ++added;
        rematAvail.add(r);
      }
      return 0;
    }
  );
  results.changed[b] = true;
  return added;
}

// Run spiller logic for normal instructions (not phis or copies). Return the
// number of instructions inserted.
size_t process_inst_spills(State& state,
                           Vlabel b,
                           size_t instIdx,
                           Vinstr& inst,
                           SpillerState& spiller,
                           SpillerResults& results) {
  // We allow Vinstr::copy if the src/dest is a physical register. In that case,
  // we treat it like a normal instruction (not giving it the special copy
  // logic).
  assertx(inst.op != Vinstr::phijmp && inst.op != Vinstr::phidef);
  assertx(
    inst.op != Vinstr::copyargs || defs_set_cached(state, inst).allPhys()
  );

  auto& unit = state.unit;

  // Keep track of which Vregs are not spilled so they can be used for
  // rematerialization.
  auto rematAvail = spiller.gp.inReg | spiller.simd.inReg;

  VregSet reloads; // Vregs we'll have to reload
  VregSet spills;  // Vregs we'll have to spill
  VregSet folded; // Vregs which originally needed to be reloaded,
                  // but we rematerialized and folded into the
                  // instruction.

  // First process the uses (including acrosses). If any use is
  // currently spilled, check if we can rematerialize it and fold it
  // into the instruction. If not, make it non-spilled and record that
  // it needs a reload.
  for (auto const r : uses_set_cached(state, inst)) {
    auto s = spiller.forReg(r);
    if (!s) continue;
    assertx(s->inReg[r] || s->inMem[r]);
    if (!s->inMem[r]) continue;
    assertx(!r.isPhys());
    auto const remat = reload_with_remat(
      state,
      b,
      instIdx,
      rematAvail,
      r,
      r
    );
    if (can_fold_remat_with_use(state, remat, inst, r)) {
      // We can rematerialize this Vreg and fold its definition into
      // the instruction itself. Keep the Vreg in inMem because we're
      // not going to actually reload it.
      fold_remat_with_use(state, remat, inst, r);
      folded.add(r);
      results.changed[b] = true;
    } else {
      // Otherwise we're going to reload it, so move it into inReg.
      reloads.add(r);
      s->inMem.remove(r);
      s->inReg.add(r);
    }
  }
  results.rematerialized |= folded;

  // Defer getting the cached operands until now, as folding
  // rematerializations above may have changed them.
  //
  // NB: acrosses is a subset of uses at this point
  auto uses = uses_set_cached(state, inst);
  auto acrosses = acrosses_set_cached(state, inst);
  auto const& defs = defs_set_cached(state, inst);

  // Anything we folded away shouldn't be showing up as any of the
  // operands anymore.
  assertx(!uses.intersects(folded));
  assertx(!defs.intersects(folded));
  // Some of the Vregs which we folded away instead of reloading may
  // be dead after this instruction, so remove them. This won't happen
  // below because they're no longer part of the use set.
  spiller.dropDead(folded, nullptr, b, instIdx);

  // Moving the uses into registers may require us to spill other Vregs (except
  // the ones we're about to use).
  spills |= spiller.spill(
    b, instIdx + 1, results,
    [&] (Vreg) { return block_weight(state, b) * kSpillReloadMultiplier; },
    uses
  );

  /*
   * Consider the instruction "Op R1 -> R2" where R1 and R2 are
   * virtual, and R1 is not used after Op. This instruction clearly
   * only requires one physical register because R1 is dead after Op,
   * and therefore R2 can always use the same register that R1
   * had. However, suppose that R1 is %eax, and R2 is %ecx. The
   * instruction actually requires two physical registers because we
   * no longer have the freedom to assign R1 and R2 the same physical
   * register. Sebastian Hack calls such instructions "register
   * pressure unfaithful".
   *
   * This is problematic for the spiller because it only considers the
   * number of live Vregs when determining whether spilling is
   * needed. It only sees the first situation. Fix this conservatively
   * by forcing any physical register uses which don't have a matching
   * physical register def to be across.
   */
  if (defs.containsPhys() && uses.containsPhys()) {
    acrosses.add(uses.physRegs() - defs.physRegs());
  }
  uses -= acrosses; // Make uses and acrosses disjoint

  // If any of the uses aren't used after this, drop them from being tracked to
  // make room for the defs. Since we removed the acrosses from the uses, we'll
  // keep those alive.
  spiller.dropDead(uses, &defs, b, instIdx);

  // Process the defs. The defs always start in registers because the
  // instruction has to write them there.
  for (auto const r : defs) {
    auto s = spiller.forReg(r);
    if (!s) continue;
    // We shouldn't already be tracking this
    assertx(!s->inReg[r] && !s->inMem[r]);
    s->inReg.add(r);
  }

  // Adding the defs may have caused us to need to spill even more We
  // cannot spill any defs, because they're going to be in physical
  // registers regardless, and acrosses need to be kept alive across
  // the defs. However, we can spill the uses here if necessary. It
  // will require some special handling below.
  spills |= spiller.spill(
    b, instIdx + 1, results,
    [&] (Vreg r) {
      // "Spilling" for a reloaded Vreg is free, because we can simply
      // keep its spill slot alive after the reload.
      if (reloads[r]) return int64_t{0};
      return block_weight(state, b) * kSpillReloadMultiplier;
    },
    defs | acrosses
  );

  // Make sure we can spill.
  always_assert_flog(
    IMPLIES(!spiller.spRecorded, spills.none() && reloads.none()),
    "Trying to spill/reload for {} in {} when not allowed (sp not recorded) "
    "(Spills: {}, Reloads: {})",
    show(state.unit, inst),
    b,
    show(spills),
    show(reloads)
  );

  // Some of the defs or acrosses may not be live (if the defs are never used),
  // so remove them.
  spiller.dropDead(defs | acrosses, nullptr, b, instIdx);

  // Bail out if we don't need to emit anything (hopefully the common case).
  if (spills.none() && reloads.none()) return 0;

  // Okay to spill uses, but not defs or acrosses.
  assertx((spills & (defs | acrosses)).none());

  // We always spill/reload a Vreg to itself, so they'll need to be
  // re-SSAized.
  results.ssaize |= spills;
  results.ssaize |= reloads;

  jit::fast_map<Vreg, Vreg> useRewrites;

  size_t added = 0;
  vmodify(
    unit, b, instIdx,
    [&] (Vout& v) {
      // First emit the spills (which decreases register pressure).
      for (auto const r : spills) {
        assertx(!r.isPhys());

        // If the Vreg is reloaded, we already have a perfectly valid
        // spill slot for it, so nothing needs to be done.
        if (reloads[r]) continue;

        /*
         * Special case: if we're spilling a Vreg that's used by the
         * instruction, we need to take special care. We cannot simply
         * spill it, as it will then not by in a physical register
         * when the instruction operates. Instead we'll use ssaalias
         * to make a "copy" of the Vreg, spill the original, and
         * rewrite the instruction to use the "copy". Copy is in
         * quotes because ssaalias uses the SSA rewriting to pass to
         * ensure no actual copy is created.
         *
         * For example:
         *   conjureuse src (where src needs to be spilled)
         *
         * Becomes:
         *   ssaalias src -> src2
         *   spill src -> src
         *   conjureuse src2
         *
         * After SSA:
         *   spill src -> src3
         *   conjureuse src
         *
         * At first glance this might appear to be ineffective because
         * it keeps the original Vreg alive after the spill, and the
         * whole point is to decrease register pressure. It works
         * because we'll only spill uses if the register pressure is
         * exceeded because of defs. In this scheme, the Vreg always
         * becomes dead immediately after its used (before the defs),
         * so we'll still lower register pressure where we need to.
         */
        if (uses[r]) {
          auto const d = state.unit.makeReg();
          auto const& info = reg_info(state, r);
          set_reg_class_bits(state, d, info.regClass);
          reg_info_insert(state, d, info);
          v << ssaalias{r, d};
          results.ssaize.add(d);
          useRewrites.emplace(r, d);
          ++added;
        }

        auto const spillResults = spill_with_remat(
          state,
          v,
          b,
          instIdx,
          rematAvail,
          r,
          r,
          !uses[r],
          true
        );
        added += spillResults.added;
        if (spillResults.rematerialized) results.rematerialized.add(r);
        rematAvail.remove(r);
        state.spilled = true;
      }

      // Now emit the reloads (which increases register pressure),
      // possibly with rematerialization.
      for (auto const r : reloads) {
        assertx(!r.isPhys());

        // Destination of the reload. Normally this is the same
        // Vreg. However if we're both reloading and spilling the
        // Vreg, we reload it into a fresh Vreg to keep the spill slot
        // alive (this makes the actual spill a no-op).
        auto const d = [&] {
          if (!spills[r]) return r;
          auto const d = state.unit.makeReg();
          auto const& info = reg_info(state, r);
          set_reg_class_bits(state, d, info.regClass);
          reg_info_insert(state, d, info);
          useRewrites.emplace(r, d);
          return d;
        }();

        auto const inst = reload_with_remat(
          state,
          b,
          instIdx,
          rematAvail,
          r,
          d
        );
        if (inst.op != Vinstr::reload) results.rematerialized.add(d);
        emit_reload(v, inst);
        ++added;
        if (r == d) rematAvail.add(r);
      }

      return 0;
    }
  );

  // Rewrites any uses of the instruction to match new Vregs created
  // above. Note: we cannot use "inst" here because the vmodify above
  // has invalidated the reference.
  if (!useRewrites.empty()) {
    visitRegsMutable(
      unit, unit.blocks[b].code[instIdx + added],
      [&] (Vreg r) {
        auto const it = useRewrites.find(r);
        return (it == useRewrites.end()) ? r : it->second;
      },
      [&] (Vreg r) { return r; }
    );
    invalidate_cached_operands(unit.blocks[b].code[instIdx + added]);
  }
  results.changed[b] = true;

  return added;
}

// Run the spiller logic for an entire block which we've
// pre-determined will not contain a spill. The block must not have
// any uses of spilled registers.
void process_spills_skip(const State& state,
                         Vlabel b,
                         SpillerState& spiller,
                         SpillerResults& results) {
  auto const inMem = spiller.inMem();

  // We know this block will not use any spilled registers and we know
  // no instruction within the block can cause a spill. The state
  // after processing this block should just be all the live-out
  // Vregs. If the Vreg was live-in in memory, it stays in memory,
  // otherwise its in a register.
  auto const& unit = state.unit;
  auto const& block = unit.blocks[b];
  auto const& lastInst = block.code.back();
  if (lastInst.op == Vinstr::phijmp) {
    // Remember to record phi spill state (no spills).
    auto const& uses = unit.tuples[lastInst.phijmp_.uses];
    assertx(!results.perBlock[b].outPhi);
    auto& outPhi = results.perBlock[b].outPhi.emplace();
    outPhi.resize(uses.size(), true);
    for (size_t i = 0; i < uses.size(); ++i) {
      auto const r = uses[i];
      if (!spiller.forReg(r)) continue;
      outPhi[i] = !inMem[r];
    }
  }

  spiller.reset();
  for (auto const r : state.liveOut[b]) {
    auto s = spiller.forReg(r);
    if (!s) continue;
    // If it was in memory (which can only happen if it was live-in in
    // memory), it stays in memory. In register otherwise.
    if (inMem[r]) {
      s->inMem.add(r);
    } else {
      s->inReg.add(r);
    }
  }

  // Since we jumped to the end of the block without processing update the
  // spRecorded state to the block out state.
  spiller.spRecorded = state.spRecordedOut[b];
}

// Run the spiller logic on an instruction which we've determined
// cannot cause a spill and which does not use a spilled register. For
// such an instruction, we simply add its defs to the spiller state
// and remove any dead Vregs.
void process_inst_spills_fast(State& state,
                              Vinstr& inst,
                              Vlabel b,
                              size_t instIdx,
                              SpillerState& spiller,
                              SpillerResults& results) {
  if (inst.op == Vinstr::phijmp) {
    // For phijmps, just mark all of its inputs as being non-spilled
    // in the spiller out state. Since we already know this
    // instruction does not use any spilled Vregs, they must all be
    // non-spilled.
    auto const& uses = state.unit.tuples[inst.phijmp_.uses];
    assertx(!results.perBlock[b].outPhi);
    auto& outPhi = results.perBlock[b].outPhi.emplace();
    outPhi.resize(uses.size(), true);
  }

  // Add any defs to the inReg state (since we know this instruction
  // cannot cause a spill, we don't have to touch inMem). Then remove
  // any dead Vregs.
  auto const& uses = uses_set_cached(state, inst);
  auto const& defs = defs_set_cached(state, inst);
  spiller.gp.inReg |= defs & state.gps;
  spiller.simd.inReg |= defs & state.simds;
  spiller.dropDead(uses | defs, nullptr, b, instIdx);
}

// Run the spiller logic over the entire unit. Returning the spiller
// state at block boundaries (and Vregs which need SSA
// conversion). Each block is allowed to determine which of its in/out
// Vregs should be spilled or not, independently of all others. We'll
// later pass over all the state and insert spills/reloads as needed
// to make each block compatible. The spilling information lets us
// process blocks/instructions more efficiently if we know they cannot
// cause a spill.
SpillerResults process_spills(State& state,
                              const SpillingNeeded& needsSpilling) {
  auto& unit = state.unit;

  // We're going to need Vreg use information about loops, so actually
  // calculate it.
  calculate_loop_uses(state);

  SpillerResults results{state};
  SCOPE_ASSERT_DETAIL("Spiller State") { return results.toString(state); };

  for (auto const b : state.rpo) {
    // Initialize the state for this block and get the initial (in) state.
    auto instIdx =
      setup_initial_spiller_state(state, b, results, needsSpilling.perBlock[b]);
    auto spiller = *results.perBlock[b].in;
    SCOPE_ASSERT_DETAIL("Current Spiller") {
      return fmt::format(
        "Block: {}\n{}",
        b, spiller.toString()
      );
    };

    if (!needsSpilling.perBlock[b].any &&
        !state.hasUnrecordbasenativesp[b] &&
        !state.uses[b].intersects(spiller.inMem())) {
      // Be efficient if there's no potential spills to worry about.
      process_spills_skip(state, b, spiller, results);
    } else {
      // Iterate over each instruction and run the logic for each one. We need
      // to use indices because we'll modify the unit as part of processing it
      // (which means we need to shift the indices).

      // The spilling information uses the original instruction
      // indices, so we need to subtract out the number of
      // instructions we've inserted.
      size_t addedInsts = 0;
      for (; instIdx < unit.blocks[b].code.size(); ++instIdx) {
        assertx(spiller.checkInvariants(b, instIdx));

        auto& inst = unit.blocks[b].code[instIdx];

        if (inst.op == Vinstr::recordbasenativesp) {
          spiller.spRecorded = true;
        }

        // If this instruction cannot cause any spills, and it does
        // not use any spilled registers, we can process it more
        // efficiently.
        if (!needsSpilling.perBlock[b](instIdx - addedInsts) &&
            !uses_set_cached(state, inst).intersects(spiller.inMem()) &&
            inst.op != Vinstr::unrecordbasenativesp) {
          process_inst_spills_fast(state, inst, b, instIdx, spiller, results);
          continue;
        }

        switch (inst.op) {
          case Vinstr::unrecordbasenativesp: {
            auto const added =
              process_unrecordbasenativesp_spills(state, b, instIdx,
                                                  spiller, results);
            addedInsts += added;
            instIdx += added;
            break;
          }
          case Vinstr::phijmp: {
            auto const added =
              process_phijmp_spills(state, b, instIdx, inst, spiller, results);
            addedInsts += added;
            instIdx += added;
            break;
          }
          case Vinstr::copyargs:
            assertx(acrosses_set_cached(state, inst).none());
            if (defs_set_cached(state, inst).allPhys()) {
              auto const added = process_inst_spills(
                state,
                b,
                instIdx,
                inst,
                spiller,
                results
              );
              addedInsts += added;
              instIdx += added;
            } else {
              assertx(!defs_set_cached(state, inst).containsPhys());
              auto const added = process_copy_spills(
                state,
                unit.tuples[inst.copyargs_.s],
                unit.tuples[inst.copyargs_.d],
                b,
                instIdx,
                spiller,
                results
              ).added;
              addedInsts += added;
              instIdx += added;
            }
            break;
          case Vinstr::copy: {
            assertx(acrosses_set_cached(state, inst).none());

            // Copies which have physical sources or dests are treated like
            // normal instructions.
            if (is_ignored(state, inst.copy_.s) || inst.copy_.d.isPhys()) {
              auto const added = process_inst_spills(
                state,
                b,
                instIdx,
                inst,
                spiller,
                results
              );
              addedInsts += added;
              instIdx += added;
              break;
            }

            // process_copy_spills takes its uses and defs as a VregList, so
            // use a temporary one to satisfy the interface.
            VregList uses{inst.copy_.s};
            VregList defs{inst.copy_.d};
            auto const copyResults = process_copy_spills(
              state,
              uses,
              defs,
              b,
              instIdx,
              spiller,
              results
            );
            auto& copy = unit.blocks[b].code[instIdx + copyResults.addedBefore];
            assertx(copy.op == Vinstr::copy);
            copy.copy_.s = uses[0];
            copy.copy_.d = defs[0];
            addedInsts += copyResults.added;
            instIdx += copyResults.added;
            break;
          }
          case Vinstr::phidef:
            // Should be handled as part of setting up the initial block state.
            always_assert(false);
            break;
          default: {
            auto const added = process_inst_spills(
              state,
              b,
              instIdx,
              inst,
              spiller,
              results
            );
            addedInsts += added;
            instIdx += added;
            break;
          }
        }
      }
    }

    assertx(spiller.checkInvariants(b, unit.blocks[b].code.size()));
    results.perBlock[b].out = std::move(spiller);
  }

  return results;
}

// Attempt to hoist spills within the specified loop.
void hoist_spills_in_loop(State& state,
                          SpillerResults& results,
                          Vlabel loop,
                          const LoopInfo& loopInfo) {
  auto& unit = state.unit;

  // First find any Vregs which are in registers at the loop header
  // block, but not used within the loop.
  auto candidates = [&] {
    auto const& in = results.perBlock[loop].in;
    assertx(in);
    return (in->gp.inReg | in->simd.inReg) - loopInfo.uses;
  }();
  if (candidates.none()) return;

  // Vregs can be copied within a loop, but that doesn't count as a
  // use. We want to consider all such linked Vregs as one. Use
  // union-find to find the sets of all Vregs linked by copies. In
  // addition, record which Vregs are spilled.

  VregSet spilled;
  jit::fast_map<Vreg, Vreg> mappings;

  auto const canonicalize = [&] (Vreg r) {
    auto const impl = [&] (Vreg r, auto const& self) {
      auto const it = mappings.find(r);
      if (it == mappings.end() || it->second == r) return r;
      auto& r2 = it->second;
      r2 = self(r2, self);
      return r2;
    };
    return impl(r, impl);
  };

  // Only link Vregs which aren't used within the loop and are
  // candidates (transitively).
  auto const unify = [&] (Vreg from, Vreg to) {
    if (from == to) return false;
    if (from.isPhys() || to.isPhys()) return false;

    if (loopInfo.uses[to]) {
      // If to wasn't used, then from shouldn't be as well (because
      // usage is transitive).
      assertx(loopInfo.uses[from]);
      assertx(!candidates[canonicalize(from)]);
      assertx(!candidates[canonicalize(to)]);
      return false;
    }
    // However from can be used while to is not.
    if (loopInfo.uses[from]) return false;

    from = canonicalize(from);
    to = canonicalize(to);
    if (from == to) return false;

    if (!candidates[from]) return false;
    if (candidates[to]) {
      auto const src = (from < to) ? to : from;
      auto const dst = (from < to) ? from : to;
      mappings[src] = dst;
      if (spilled[src]) spilled.add(dst);
    } else {
      mappings[to] = from;
      if (spilled[to]) spilled.add(from);
    }

    return true;
  };

  // Build up the equivalence sets, looping until we don't add any new
  // information.
  for (auto changed = true; changed;) {
    changed = false;
    for (auto const b : loopInfo.blocks) {
      for (auto const& inst : unit.blocks[b].code) {
        switch (inst.op) {
          case Vinstr::reload:
            assertx(!inst.reload_.s.isPhys() && !inst.reload_.d.isPhys());
            changed |= unify(inst.reload_.s, inst.reload_.d);
            break;
          case Vinstr::spill: {
            // We can sometimes spill directly from a physical
            // register (if the spilled Vreg came from a copy from
            // that physical register). We only do that if the spill
            // would have been from/to the same Vreg, so we can use
            // the dst in that case.
            assertx(!inst.spill_.d.isPhys());
            if (!inst.spill_.s.isPhys()) {
              changed |= unify(inst.spill_.s, inst.spill_.d);
              spilled.add(canonicalize(inst.spill_.s));
            } else {
              spilled.add(canonicalize(inst.spill_.d));
            }
            break;
          }
          case Vinstr::ssaalias:
            assertx(!inst.ssaalias_.s.isPhys() && !inst.ssaalias_.d.isPhys());
            changed |= unify(inst.ssaalias_.s, inst.ssaalias_.d);
            break;
          case Vinstr::copy:
            changed |= unify(inst.copy_.s, inst.copy_.d);
            break;
          case Vinstr::copyargs: {
            auto const& srcs = unit.tuples[inst.copyargs_.s];
            auto const& dsts = unit.tuples[inst.copyargs_.d];
            assertx(srcs.size() == dsts.size());
            for (size_t i = 0; i < srcs.size(); ++i) {
              changed |= unify(srcs[i], dsts[i]);
            }
            break;
          }
          case Vinstr::phijmp: {
            auto const succ = succs(unit.blocks[b]);
            assertx(succ.size() == 1);
            assertx(unit.blocks[succ[0]].code.front().op == Vinstr::phidef);
            auto const& phidef = unit.blocks[succ[0]].code.front().phidef_;
            auto const& srcs = unit.tuples[inst.phijmp_.uses];
            auto const& dsts = unit.tuples[phidef.defs];
            assertx(srcs.size() == dsts.size());
            for (size_t i = 0; i < srcs.size(); ++i) {
              changed |= unify(srcs[i], dsts[i]);
            }
            break;
          }
          default:
            break;
        }
      }
    }
  }

  // We only want to consider canonical Vregs which are spilled within
  // the loop.
  candidates &= spilled;
  if (candidates.none()) return;

  // Now that we have the set of Vregs which aren't used within the
  // loop, spilled, but in physical registers at the entry of the
  // loop, we want to estimate the gain we can get from hoisting them.

  jit::fast_map<Vreg, int64_t> gains;

  // Hoisting the given Vreg will cause a penalty. The penalty depends
  // on whether the Vreg is trivially rematerializable at the given
  // point, and whether trivial means there's any penalty at all
  // (which varies on context).
  auto const penalty = [&] (Vreg r, Vlabel b,
                            bool trivial, bool useTrivial) {
    assertx(candidates[r]);
    auto const amount =
      block_weight(state, b) *
      (trivial ? (useTrivial ? 1 : 0) : kSpillReloadMultiplier);
    gains[r] -= amount;
  };
  // Likewise, hoisting the given Vreg will cause a win. The win
  // depends on whether the Vreg is trivially rematerializable at the
  // given point, and whether trivial means there's any gain at all
  // (which varies on context).
  auto const credit = [&] (Vreg r, Vlabel b,
                           bool trivial, bool useTrivial) {
    assertx(candidates[r]);
    auto const amount =
      block_weight(state, b) *
      (trivial ? (useTrivial ? 1 : 0) : kSpillReloadMultiplier);
    gains[r] += amount;
  };

  // Loop over the blocks in the loop, assessing gains/penalties
  // depending on what spill/reloads will be generated if we hoist the
  // Vreg spills out of the loop.
  for (auto const b : loopInfo.blocks) {
    if (b == loop) {
      // Special case: the loop header. We want to look at
      // predecessors (not within the same block) and see the state of
      // each Vreg there. If we hoist the Vreg spills out, we'll
      // potentially have to insert spills in them.
      for (auto const pred : state.preds[b]) {
        // Ignore predecessors within the same loop. Those are
        // back-edges and if we do hoist the Vreg, will always match
        // the header block.
        if (block_in_loop(state, pred, loop)) continue;

        auto const& out = results.perBlock[pred].out;
        auto const& outPhi = results.perBlock[pred].outPhi;
        assertx(out);

        // If the Vreg is in memory in the predecessor, it's a gain
        // because we'll no longer have to reload the Vreg (remember
        // the candidate Vregs are in physical registers in the loop
        // header entry).
        for (auto const r : (out->gp.inMem | out->simd.inMem)) {
          auto const canon = canonicalize(r);
          if (!candidates[canon]) continue;
          auto const trivial = !!is_trivial_remat(state, r, b, !!outPhi);
          credit(canon, pred, trivial, true);
        }

        // On the other hand, if the Vreg is in a physical register in
        // the predecessor, it's a penalty because we'll have to insert
        // a new spill there.
        for (auto const r : (out->gp.inReg | out->simd.inReg)) {
          auto const canon = canonicalize(r);
          if (!candidates[canon]) continue;
          auto const trivial = !!is_trivial_remat(state, r, b, !!outPhi);
          penalty(canon, pred, trivial, false);
        }

        if (!outPhi) continue;

        // Do the same logic, but for phis.
        assertx(unit.blocks[b].code.front().op == Vinstr::phidef);
        auto const& phidef = unit.blocks[b].code.front().phidef_;
        auto const& defs = unit.tuples[phidef.defs];

        assertx(defs.size() == outPhi->size());
        for (size_t i = 0; i < defs.size(); ++i) {
          auto const r = defs[i];
          auto const canon = canonicalize(r);
          if (!candidates[canon]) continue;
          auto const trivial = !!is_trivial_remat(state, r, b, 1);
          if ((*outPhi)[i]) {
            penalty(canon, pred, trivial, false);
          } else {
            credit(canon, pred, trivial, true);
          }
        }
      }
    }

    // Examine the instructions within the block itself. If we hoist
    // the spill out of the loop, we won't need any spills within the
    // loop, so it's a gain since we can get rid of them.
    for (size_t i = 0; i < unit.blocks[b].code.size(); ++i) {
      auto& inst = unit.blocks[b].code[i];

      auto const process = [&] (Vreg r1, Vreg r2, bool spill, bool reload) {
        auto const c1 = canonicalize(r1);
        auto const c2 = canonicalize(r2);
        if (c1 != c2) return;
        if (!candidates[c1]) return;

        if (r1 != r2 && live_in_at(state, r1, b, i+1)) {
          // If the instruction writes to a different Vreg than it
          // reads from, but the src is live across the instruction,
          // assess a penalty instead of a credit. The instruction
          // will be turned into a copy, where both the src and dst
          // are spilled. Since src interferes with dst, the (mem to
          // mem) copy cannot be coalesced away. Instead it will be
          // lowered into a load/store pair. This is actually more
          // expensive than the previous spill/reload/copy/
          penalty(c1, b, false, false);
        } else if (spill || reload) {
          // Otherwise the copy will either be trivial (same src and
          // dst), or we optimistically assume it will be coalesced
          // away. In that case, we're removing the instruction
          // entirely, so its a credit. This is only applicable to
          // spills or reloads.
          credit(
            c1, b,
            !!is_trivial_remat(state, r1, b, i),
            reload
          );
        }
      };

      switch (inst.op) {
        case Vinstr::spill:
          process(inst.spill_.s, inst.spill_.d, true, false);
          break;
        case Vinstr::reload:
          process(inst.reload_.s, inst.reload_.d, false, true);
          break;
        case Vinstr::copy:
          process(inst.copy_.s, inst.copy_.d, false, false);
          break;
        case Vinstr::copyargs: {
          auto const& srcs = unit.tuples[inst.copyargs_.s];
          auto const& dsts = unit.tuples[inst.copyargs_.d];
          assertx(srcs.size() == dsts.size());
          for (size_t i = 0; i < srcs.size(); ++i) {
            process(srcs[i], dsts[i], false, false);
          }
          break;
        }
        case Vinstr::ssaalias:
        case Vinstr::phidef:
        case Vinstr::phijmp:
          // These will not be removed
          break;
        default: {
          // Pure instructions which def a candidate will be removed,
          // so provide the proper credit.
          if (!isPure(inst)) break;
          auto const& defs = defs_set_cached(state, inst);
          if (defs.size() != 1) break;
          auto const canon = canonicalize(*defs.begin());
          if (candidates[canon]) credit(canon, b, true, true);
          break;
        }
      }
    }

    // Now do the opposite of what we did for the entry block. Look at
    // successors of the current block which aren't in the same
    // loop. Assess a penalty or gain depending on the state of the
    // Vreg in that successor. If the successor is in the current
    // block, we know both sides will have the Vreg as being spilled,
    // so we can remove disagreements between the blocks and get a
    // gain.
    auto const successors = succs(unit.blocks[b]);
    for (auto const succ : successors) {
      auto const& out = results.perBlock[b].out;
      auto const& outPhi = results.perBlock[b].outPhi;
      assertx(out);

      auto const& in = results.perBlock[succ].in;
      auto const& inPhi = results.perBlock[succ].inPhi;
      assertx(in);

      if (block_in_loop(state, succ, loop)) {
        // The successor is in the same loop. Check for disagreements
        // between the blocks' in/out states for each Vreg. If there
        // is, we know we would have inserted a spill/reload that we
        // now won't need, so assess a gain.
        auto const process = [&] (const VregSet& from,
                                  const VregSet& to,
                                  bool useTrivial) {
          for (auto const r : from) {
            auto const canon = canonicalize(r);
            if (!candidates[canon]) continue;
            if (!to[r]) continue;
            auto const trivial =
              !!is_trivial_remat(state, r, b, unit.blocks[b].code.size());
            credit(canon, b, trivial, useTrivial);
          }
        };
        process(out->gp.inReg, in->gp.inMem, false);
        process(out->simd.inReg, in->simd.inMem, false);
        process(out->gp.inMem, in->gp.inReg, true);
        process(out->simd.inMem, in->simd.inReg, true);

        if (!outPhi) continue;
        assertx(successors.size() == 1);
        assertx(inPhi);

        assertx(unit.blocks[b].code.back().op == Vinstr::phijmp);
        assertx(unit.blocks[succ].code.front().op == Vinstr::phidef);
        auto const& phijmp = unit.blocks[b].code.back().phijmp_;
        auto const& phidef = unit.blocks[succ].code.front().phidef_;
        auto const& srcs = unit.tuples[phijmp.uses];
        auto const& defs = unit.tuples[phidef.defs];

        // Now do the same for any phis between these blocks. We only
        // consider the input/output pair if they map to the same
        // canonical set. Otherwise there's no guarantee we'll hoist
        // both.
        assertx(srcs.size() == defs.size());
        assertx(srcs.size() == outPhi->size());
        for (size_t i = 0; i < srcs.size(); ++i) {
          auto const r1 = srcs[i];
          auto const r2 = defs[i];
          auto const canon1 = canonicalize(r1);
          auto const canon2 = canonicalize(r2);
          auto const inRegOut = (*outPhi)[i];
          auto const inRegIn = (*inPhi)[i];

          if (canon1 == canon2) {
            if (!candidates[canon1]) continue;

            if (inRegOut == inRegIn) continue;

            auto const trivial =
              !!is_trivial_remat(state, r1, b, unit.blocks[b].code.size());
            credit(canon1, b, trivial, !inRegOut || inRegIn);
          } else {
            if (candidates[canon1] && inRegOut) {
              auto const trivial =
                !!is_trivial_remat(state, r1, b, unit.blocks[b].code.size());
              if (inRegIn) {
                penalty(canon1, b, trivial, true);
              } else {
                credit(canon1, b, trivial, false);
              }
            }

            if (candidates[canon2] && inRegIn) {
              auto const trivial = !!is_trivial_remat(state, r2, succ, 1);
              if (inRegOut) {
                penalty(canon2, b, trivial, false);
              } else {
                credit(canon2, b, trivial, true);
              }
            }
          }
        }
      } else {
        assertx(successors.size() > 1);
        assertx(!outPhi);
        assertx(!inPhi);
        assertx(unit.blocks[b].code.back().op != Vinstr::phijmp);
        assertx(unit.blocks[succ].code.front().op != Vinstr::phidef);

        // The successor is not in the same loop as the current
        // block. Examine the in-state of the successor. If it
        // indicates the Vreg was spilled in the successor but the
        // Vreg was not originally spilled in the current block, we
        // have a gain (we needed a spill but now we don't). On the
        // other hand, if the successor expects the Vreg to be
        // non-spilled and it was non-spilled in the current block, we
        // have a penalty (will have to insert a reload).
        auto const process = [&] (const VregSet& from,
                                  const VregSet& toReg,
                                  const VregSet& toMem) {
          for (auto const r : from) {
            auto const canon = canonicalize(r);
            if (!candidates[canon]) continue;
            auto const trivial =
              !!is_trivial_remat(state, r, b, unit.blocks[b].code.size());
            if (toReg[r]) {
              penalty(canon, succ, trivial, true);
            } else if (toMem[r]) {
              credit(canon, succ, trivial, false);
            }
          }
        };
        process(out->gp.inReg, in->gp.inReg, in->gp.inMem);
        process(out->simd.inReg, in->simd.inReg, in->simd.inMem);
      }
    }
  }

  // Remove any candidates with negative gains. This indicates it's a
  // net loss to hoist this Vreg's spill out of the loop.
  for (auto const& kv : gains) {
    assertx(candidates[kv.first]);
    if (kv.second <= 0) candidates.remove(kv.first);
  }

  if (candidates.none()) return;

  // Now we know which Vregs we're going to hoist. Loop over all the
  // blocks, changing the spiller state to indicate this (and removing
  // now redundant spill instructions).

  for (auto const b : loopInfo.blocks) {
    // First update spiller state. Move any candidates from inReg to
    // inMem.
    auto const spillize = [&] (SpillerState::PerClass& p) {
      auto changed = false;
      for (auto const r : p.inReg) {
        auto const canon = canonicalize(r);
        if (!candidates[canon]) continue;
        p.inMem.add(r);
        changed = true;
      }
      if (changed) p.inReg -= p.inMem;
    };

    // Same for phi spiller state.
    auto const spillizePhi = [&] (const VregList& regs,
                                  boost::dynamic_bitset<>& bits) {
      assertx(regs.size() == bits.size());
      for (size_t i = 0; i < regs.size(); ++i) {
        if (!candidates[canonicalize(regs[i])]) continue;
        bits[i] = false;
      }
    };

    auto& in = results.perBlock[b].in;
    auto& out = results.perBlock[b].out;
    auto& inPhi = results.perBlock[b].inPhi;
    auto& outPhi = results.perBlock[b].outPhi;
    assertx(in);
    assertx(out);

    spillize(in->gp);
    spillize(in->simd);
    spillize(out->gp);
    spillize(out->simd);

    if (inPhi) {
      assertx(unit.blocks[b].code.front().op == Vinstr::phidef);
      auto const& phidef = unit.blocks[b].code.front().phidef_;
      auto const& defs = unit.tuples[phidef.defs];
      spillizePhi(defs, *inPhi);
    }

    if (outPhi) {
      assertx(unit.blocks[b].code.back().op == Vinstr::phijmp);
      auto const& phijmp = unit.blocks[b].code.back().phijmp_;
      auto const& uses = unit.tuples[phijmp.uses];
      spillizePhi(uses, *outPhi);
    }

    // Remove any instructions which are now unneeded because the Vreg
    // is always spilled within the loop.
    for (auto& inst : unit.blocks[b].code) {
      switch (inst.op) {
        case Vinstr::copy:
          // Copies can handle spilled Vregs fine, but a copy from a
          // physical register has to be removed.
          if (!candidates[canonicalize(inst.copy_.d)]) break;
          if (inst.copy_.s.isPhys() || inst.copy_.s == inst.copy_.d) {
            // The dest of the copy is a candidate, which means it's
            // live-in to the loop. However, if the source is a
            // physical register, we're defining dest (inside the
            // loop). This can only happen if we've rematerialized the
            // dest. The only other case is if this is a nop copy,
            // which always can be removed.
            assertx(!inst.copy_.s.isPhys() ||
                    results.rematerialized[inst.copy_.d]);
            inst.nop_ = nop{};
            inst.op = Vinstr::nop;
            results.changed[b] = true;
            invalidate_cached_operands(inst);
          }
          break;
        case Vinstr::copyargs:
        case Vinstr::ssaalias:
        case Vinstr::phidef:
        case Vinstr::phijmp:
          // These handle spilled Vregs fine, they need to be kept as
          // is.
          break;
        case Vinstr::spill: {
          // Spills should be removed
          assertx(!inst.spill_.d.isPhys());
          // If we rematerialized directly into a spill, the source
          // can be a physical register. In that case, dest will have
          // the original Vreg.
          auto const r = inst.spill_.s.isPhys() ? inst.spill_.d : inst.spill_.s;
          if (candidates[canonicalize(r)]) {
            if (inst.spill_.s.isPhys() || inst.spill_.s == inst.spill_.d) {
              inst.nop_ = nop{};
              inst.op = Vinstr::nop;
            } else {
              inst.copy_ = copy{inst.spill_.s, inst.spill_.d};
              inst.op = Vinstr::copy;
            }
            results.changed[b] = true;
            invalidate_cached_operands(inst);
          }
          break;
        }
        case Vinstr::reload:
          // Reloads should be removed
          if (candidates[canonicalize(inst.reload_.s)]) {
            assertx(candidates[canonicalize(inst.reload_.d)]);
            if (inst.reload_.s == inst.reload_.d) {
              inst.nop_ = nop{};
              inst.op = Vinstr::nop;
            } else {
              inst.copy_ = copy{inst.reload_.s, inst.reload_.d};
              inst.op = Vinstr::copy;
            }
            results.changed[b] = true;
            invalidate_cached_operands(inst);
          }
          break;
        default:
          // Never change non-pure instructions
          if (!debug && !isPure(inst)) break;
          auto const& defs = defs_set_cached(state, inst);
          if (defs.size() != 1) break;
          auto const r = *defs.begin();
          // Hoisted Vregs should only be written by single def,
          // pure instructions.
          if (candidates[canonicalize(r)]) {
            assertx(results.rematerialized[r]);
            assertx(isPure(inst));
            inst.nop_ = nop{};
            inst.op = Vinstr::nop;
            results.changed[b] = true;
            invalidate_cached_operands(inst);
          }
          break;
      }
    }

    // Can't do liveness invariant checking here because
    // rematerialization may have created dead Vregs.
    assertx(in->checkInvariants(Vlabel{}, 0));
    assertx(out->checkInvariants(b, unit.blocks[b].code.size()));
  }
}

// Try to find Vregs which are in a physical register at a loop
// header, spilled somewhere with in the loop, and not used within the
// loop. Such spills are particularly bad because we'll have to reload
// on the loop's back edge. If found, try to hoist the spill out of
// the loop entirely to avoid the back edge reload penalty.
void hoist_loop_spills(State& state, SpillerResults& results) {
  if (state.loopInfo.empty()) return;

  // The spiller may have created new Vregs, so re-calculate the loop
  // uses.
  calculate_loop_uses(state);

  // Process each loop individually. Go from lower depth loops to
  // higher depths.
  using LoopPair = std::pair<Vlabel, const LoopInfo*>;
  jit::vector<LoopPair> loops;
  loops.reserve(state.loopInfo.size());
  for (auto const& kv : state.loopInfo) {
    loops.emplace_back(kv.first, &kv.second);
  }
  std::sort(
    loops.begin(),
    loops.end(),
    [] (const LoopPair& a, const LoopPair& b) {
      if (a.second->depth != b.second->depth) {
        return a.second->depth < b.second->depth;
      }
      auto const size1 = a.second->blocks.size();
      auto const size2 = b.second->blocks.size();
      if (size1 != size2) return size1 > size2;
      return a.first < b.first;
    }
  );

  for (auto const& kv : loops) {
    hoist_spills_in_loop(state, results, kv.first, *kv.second);
  }
}

// Mismatches between a block and its successors. This indicates what should be
// inserted in the block to resolve the mismatch.
struct SpillMismatchState {
  VregSet spills;  // Vregs to spill
  VregSet reloads; // Vregs to reload

  // Map a spilled Vreg to the spill destination. If not present, the Vreg is
  // spilled to itself.
  jit::flat_map<Vreg, Vreg> spillDests;
  // Map a reloaded Vreg to the reload destination. If not present, the Vreg is
  // reloaded to itself.
  jit::flat_map<Vreg, Vreg> reloadDests;
  // ssaalias instructions to insert
  jit::flat_map<Vreg, Vreg> aliases;

  // Index p1 of phijmp should be written to p2 (after all other
  // instructions have been inserted).
  jit::vector<std::pair<size_t, Vreg>> useRewrites;
};

// Calculate the spill/reload mismatches between a block and one of
// its successors.
SpillMismatchState find_spill_mismatches(State& state,
                                         Vlabel predBlock,
                                         Vlabel succBlock,
                                         SpillerResults& results) {
  auto& unit = state.unit;

  auto& pred = results.perBlock[predBlock].out;
  auto& predPhi = results.perBlock[predBlock].outPhi;
  // We should have out state for everything at this point (but only predPhi if
  // this has a phijmp).
  assertx(pred);
  assertx(pred->checkInvariants(predBlock, unit.blocks[predBlock].code.size()));

  SpillMismatchState mismatch;
  boost::dynamic_bitset<> phiSpills;
  boost::dynamic_bitset<> phiReloads;

  // Calculate where the states disagree:
  auto& succ = results.perBlock[succBlock].in;
  auto& succPhi = results.perBlock[succBlock].inPhi;
  assertx(succ);
  assertx(succ->checkInvariants(Vlabel{}, 0));
  assertx((bool)predPhi == (bool)succPhi);

  auto const modifySucc = succs(unit.blocks[predBlock]).size() > 1;
  assertx(IMPLIES(modifySucc, !predPhi));

  // If the Vreg is in a register in the predecessor but spilled in the
  // successor, we need to spill it. Likewise, if the Vreg is spilled in the
  // predecessor but in a register in the successor, we need to reload it.
  auto const add = [&] (const SpillerState::PerClass& s1,
                        const SpillerState::PerClass& s2) {
    mismatch.spills |= (s1.inReg & s2.inMem);
    mismatch.reloads |= (s1.inMem & s2.inReg);
  };
  add(pred->gp, succ->gp);
  add(pred->simd, succ->simd);

  if (predPhi) {
    // Do the same thing, but for phi input/outputs.
    assertx(predPhi->size() == succPhi->size());
    auto const diff = *predPhi ^ *succPhi;
    phiSpills = *predPhi & diff;
    phiReloads = *succPhi & diff;
  }

  assertx((mismatch.spills & mismatch.reloads).none());
  assertx(phiSpills.size() == phiReloads.size());
  assertx((phiSpills & phiReloads).none());

  // Since we split critical edges, there's only two situations we
  // have to deal with if we have any mismatches. The first is the
  // pred has a single successor. This implies the successor has
  // multiple predecessors (we wouldn't have mismatches
  // otherwise). We'll add the fixups in the predecessor. The second
  // is the pred has multiple successors. This means the successor has
  // a single predecessor (because of split critical edges). We'll add
  // the fixups in the successor.
  assertx(
    IMPLIES(
      mismatch.spills.any() || mismatch.reloads.any() ||
      phiSpills.any() || phiReloads.any(),
      (succs(unit.blocks[predBlock]).size() == 1 &&
       state.preds[succBlock].size() > 1) ||
      (succs(unit.blocks[predBlock]).size() > 1 &&
       state.preds[succBlock].size() == 1)
    )
  );

  // Update the block's out state to match the changes we need to
  // make.
  if (modifySucc) {
    assertx(state.preds[succBlock].size() == 1);
    auto const update = [&] (SpillerState::PerClass& s) {
      s.inReg |= (mismatch.spills & s.inMem);
      s.inMem |= (mismatch.reloads & s.inReg);
      s.inReg -= mismatch.reloads;
      s.inMem -= mismatch.spills;
    };
    update(succ->gp);
    update(succ->simd);
  } else {
    auto const update = [&] (SpillerState::PerClass& s) {
      s.inReg |= (mismatch.reloads & s.inMem);
      s.inMem |= (mismatch.spills & s.inReg);
      s.inReg -= mismatch.spills;
      s.inMem -= mismatch.reloads;
    };
    update(pred->gp);
    update(pred->simd);
  }
  assertx(pred->checkInvariants(predBlock, unit.blocks[predBlock].code.size()));
  assertx(succ->checkInvariants(Vlabel{}, 0));

  assertx((pred->gp.inReg & succ->gp.inMem).none());
  assertx((pred->gp.inMem & succ->gp.inReg).none());
  assertx((pred->simd.inReg & succ->simd.inMem).none());
  assertx((pred->simd.inMem & succ->simd.inReg).none());

  // If there's no phi, we're done.
  if (!predPhi) {
    assertx(phiReloads.empty());
    assertx(phiSpills.empty());
    return mismatch;
  }
  assertx(!modifySucc);

  // Modify this block's phi state to match the successors.
  assertx(phiSpills.size() == predPhi->size());
  *predPhi |= phiReloads;
  *predPhi -= phiSpills;
  assertx(*predPhi == *succPhi);

  assertx(unit.blocks[predBlock].code.back().op == Vinstr::phijmp);
  auto const& phijmp = unit.blocks[predBlock].code.back();
  auto const& uses = unit.tuples[phijmp.phijmp_.uses];

  // If there's a phi, we might need to add additional instructions. The reason
  // is because we might need to spill or reload Vregs to match the successor,
  // but the phijmp might take one of those Vregs as an input. We cannot defer
  // the spill or reloads after the phijmp because that's always the end of the
  // block. So, we might need to insert additional fixup instructions to make
  // sure the inputs of the phijmp matches the spilled/non-spilled state of the
  // phidef in the successor. Just like copies, phijmp/phidefs can handle Vregs
  // with the same spilled/non-spilled state on each side, but not a mix.
  VregSet newSpills;
  VregSet newReloads;
  for (size_t i = 0; i < uses.size(); ++i) {
    auto const r = uses[i];
    if (!pred->forReg(r)) {
      assertx(!phiSpills[i] && !phiReloads[i]);
      continue;
    }

    if (phiSpills[i])  newSpills.add(r);
    if (phiReloads[i]) newReloads.add(r);

    if (mismatch.spills[r] && !phiSpills[i]) {
      /*
       * A Vreg that's an input to the phijmp needs to be spilled, but the
       * successor expects it to be in a register when output by the
       * phidef. We'll make a temporary Vreg and alias it to the original input
       * (before the spill). The phijmp will then take the temporary as input
       * instead.
       *
       * Before:
       *   phijmp src
       *
       * After:
       *   ssaalias src -> src2
       *   spill src -> src
       *   phijmp src2
       *
       * After SSA:
       *   spill src -> src3
       *   phijmp src
       */

      // Re-use an existing alias Vreg if one exists.
      auto const it = mismatch.aliases.find(r);
      if (it != mismatch.aliases.end()) {
        mismatch.useRewrites.emplace_back(i, it->second);
      } else {
        auto const r2 = unit.makeReg();
        mismatch.aliases.emplace(r, r2);
        mismatch.useRewrites.emplace_back(i, r2);
      }
    } else if (!mismatch.spills[r] && phiSpills[i]) {
      /*
       * A Vreg that's an input to the phijmp doesn't need to be spilled, but
       * the successor expects it to be spilled when output by the phidef. This
       * case is easy to handle. Just spill to a temporary Vreg, and then have
       * the phijmp use that temporary as an input instead.
       *
       * Before:
       *  phijmp src
       *
       * After:
       *  spill src -> src2
       *  phijmp src2
       *
       * After SSA:
       *  spill src -> src2
       *  phijmp src2
       */
      auto const it = mismatch.spillDests.find(r);
      if (it != mismatch.spillDests.end()) {
        mismatch.useRewrites.emplace_back(i, it->second);
      } else {
        auto const r2 = unit.makeReg();
        mismatch.spillDests.emplace(r, r2);
        mismatch.useRewrites.emplace_back(i, r2);
      }
    }

    if (mismatch.reloads[r] && !phiReloads[i]) {
      /*
       * A Vreg that's an input to the phijmp needs to be reloaded, but the
       * successor expects it to be spilled when output by the phidef.  We'll
       * make a temporary Vreg and alias it to the original input (before the
       * reload). The phijmp will then take the temporary as input instead.
       *
       * Before
       *   phijmp src
       *
       * After:
       *  ssaalias src -> src2
       *  reload src -> src
       *  phijmp src2
       *
       * After SSA:
       *   reload src -> src3
       *   phijmp src
       */

      // Re-use an existing alias Vreg if one exists.
      auto const it = mismatch.aliases.find(r);
      if (it != mismatch.aliases.end()) {
        mismatch.useRewrites.emplace_back(i, it->second);
      } else {
        auto const r2 = unit.makeReg();
        mismatch.aliases.emplace(r, r2);
        mismatch.useRewrites.emplace_back(i, r2);
      }
    } else if (!mismatch.reloads[r] && phiReloads[i]) {
      /*
       * A Vreg that's an input to the phijmp doesn't need to be reloaded, but
       * the successor expects it to be in a register when output by the
       * phidef. This case is easy to handle. Just reload to a temporary Vreg,
       * and then have the phijmp use that temporary as an input instead.
       *
       * Before:
       *  phijmp src
       *
       * After:
       *  reload src -> src2
       *  phijmp src2
       *
       * After SSA:
       *  reload src -> src2
       *  phijmp src2
       */
      auto const it = mismatch.reloadDests.find(r);
      if (it != mismatch.reloadDests.end()) {
        mismatch.useRewrites.emplace_back(i, it->second);
      } else {
        auto const r2 = unit.makeReg();
        mismatch.reloadDests.emplace(r, r2);
        mismatch.useRewrites.emplace_back(i, r2);
      }
    }
  }

  mismatch.spills |= newSpills;
  mismatch.reloads |= newReloads;
  assertx((mismatch.spills & mismatch.reloads).none());

  return mismatch;
}

// Find all mismatches between blocks in the unit, and insert spill/reloads as
// necessary to resolve them.
void fixup_spill_mismatches(State& state, SpillerResults& results) {
  auto& unit = state.unit;

  // For each block, find the mismatches and then materialize them.
  for (auto const b : state.rpo) {
    // Look for mismatches between pred and succ, and materialize the
    // fixups in succ.
    auto const fixup = [&] (Vlabel pred, Vlabel succ) {
      assertx(pred != succ);
      assertx(pred == b || succ == b);

      auto const mismatch = find_spill_mismatches(state, pred, succ, results);

      always_assert(
        IMPLIES(
          !state.spRecordedIn[succ],
          mismatch.spills.none() && mismatch.reloads.none()
        )
      );

      if (mismatch.spills.none() && mismatch.reloads.none()) return;

      results.ssaize |= mismatch.spills;
      results.ssaize |= mismatch.reloads;

      if (succ == b) {
        assertx(mismatch.spillDests.empty());
        assertx(mismatch.reloadDests.empty());
        assertx(mismatch.aliases.empty());
      }

      // Any new Vreg being used as a destination of a spill/reload/ssaalias
      // should have the same RegInfo as its source.
      auto const processMap = [&] (const jit::flat_map<Vreg, Vreg>& m) {
        for (auto const& p : m) {
          results.ssaize.add(p.first);
          results.ssaize.add(p.second);
          auto const& info = reg_info(state, p.first);
          set_reg_class_bits(state, p.second, info.regClass);
          reg_info_insert(state, p.second, info);
        }
      };
      processMap(mismatch.spillDests);
      processMap(mismatch.reloadDests);
      processMap(mismatch.aliases);

      // We can only rematerialize instructions which have available
      // sources. These are the Vregs which are currently in
      // registers and haven't been reloaded (if they were reloaded
      // they may not be in registers when we need them).
      auto availForRemat = [&] {
        if (succ == b) {
          auto const& in = results.perBlock[b].in;
          return in->gp.inReg | in->simd.inReg;
        }

        auto const& out = results.perBlock[b].out;
        auto avail = out->gp.inReg | out->simd.inReg;

        for (auto const r : mismatch.reloads) {
          auto const it = mismatch.reloadDests.find(r);
          auto const r2 =
            it == mismatch.reloadDests.end() ? r : it->second;
          avail.remove(r2);
        };

        for (auto const r : mismatch.spills) {
          auto const it = mismatch.spillDests.find(r);
          auto const r2 =
            it == mismatch.spillDests.end() ? r : it->second;
          avail.add(r2);
        }

        return avail;
      }();

      assertx(
        IMPLIES(
          succ == b,
          unit.blocks[b].code.front().op != Vinstr::phidef
        )
      );

      // Materialize the actual instructions. The order of these is
      // important. Whether we insert the instructions at the
      // beginning of the block or the end depends on whether the
      // successor (where we're inserting) is the current block or
      // not.
      auto const insertIdx = (succ == b)
        ? 0
        : (unit.blocks[b].code.size() - 1);

      vmodify(
        unit, b, insertIdx,
        [&] (Vout& v) {
          // First the ssaaliases, which must come before anything else.
          VregSet usedByAlias;
          for (auto const& p : mismatch.aliases) {
            v << ssaalias{p.first, p.second};
            usedByAlias.add(p.first);
          }

          // Then the spills, which reduce register pressure.
          for (auto const r : mismatch.spills) {
            assertx(!r.isPhys());
            auto const it = mismatch.spillDests.find(r);
            auto const r2 =
              (it == mismatch.spillDests.end()) ? r : it->second;
            auto const spillResults = spill_with_remat(
              state,
              v,
              b,
              insertIdx,
              availForRemat,
              r,
              r2,
              // Dst can only be used by remats if its the same as the
              // src (otherwise we increase register pressure), and if
              // it hasn't been used by a ssaalias (in which case we
              // have to preserve the src).
              r == r2 && !usedByAlias[r],
              true
            );
            if (spillResults.rematerialized) results.rematerialized.add(r);
            state.spilled = true;
            availForRemat.remove(r2);
          }

          // Then the reloads, which increases register pressure.
          for (auto const r : mismatch.reloads) {
            assertx(!r.isPhys());
            auto const it = mismatch.reloadDests.find(r);
            auto const r2 =
              (it == mismatch.reloadDests.end()) ? r : it->second;
            auto const inst = reload_with_remat(
              state,
              b,
              insertIdx,
              availForRemat,
              r,
              r2
            );
            if (inst.op != Vinstr::reload) results.rematerialized.add(r);
            emit_reload(v, inst);
            availForRemat.add(r2);
          }

          return 0;
        }
      );
      results.changed[b] = true;

      // Now actually change any of the uses of the phijmp. We defer
      // this until the end because the rematerialization logic may
      // try to look through the same phijmp/phidef and will become
      // confused if it's partially modified. We're done with all that
      // now, so we can change it.
      if (!mismatch.useRewrites.empty()) {
        assertx(unit.blocks[pred].code.back().op == Vinstr::phijmp);
        auto& phijmp = unit.blocks[pred].code.back();
        auto& uses = unit.tuples[phijmp.phijmp_.uses];
        for (auto const& p : mismatch.useRewrites) {
          assertx(p.first < uses.size());
          uses[p.first] = p.second;
        }
        invalidate_cached_operands(phijmp);
      }
    };

    // If the block has a single predecessor, and that predecessor has
    // multiple successors, we want to insert the fixups in the
    // current block. If the current block has a single successor,
    // then we want to insert the fixups in the successor. Since we
    // split critical edges, exactly one of these cases applies for
    // every block.

    auto const& preds = state.preds[b];
    if (preds.size() == 1 && succs(unit.blocks[preds[0]]).size() > 1) {
      fixup(preds[0], b);
    }

    auto const successors = succs(unit.blocks[b]);
    if (successors.size() == 1) {
      fixup(b, successors[0]);
    }
  }

  if (debug) {
    for (auto const b : state.rpo) {
      // Vregs that were originally live-in to the block may now be
      // dead because of materialization. This means that
      // checkInvariants can't check liveness correctness like
      // normal. This outdated information is fine though, because
      // nothing will use the in-states after this.
      always_assert(results.perBlock[b].in->checkInvariants(Vlabel{}, 0));
      always_assert(
        results.perBlock[b].out->checkInvariants(
          b, unit.blocks[b].code.size()
        )
      );
      for (auto const p : state.preds[b]) {
        always_assert(results.perBlock[p].outPhi == results.perBlock[b].inPhi);
      }
      for (auto const s : succs(unit.blocks[b])) {
        always_assert(results.perBlock[b].outPhi == results.perBlock[s].inPhi);
      }
    }
  }
}

// Now that all spills and reloads have been inserted, we need to update the
// RegClass of spilled Vregs to indicate they represent spills. A Vreg
// represents a spill if its def is a spill instruction. The "spillness" is also
// propagated across copies and phis (if the source is a spill slot, the dest
// must be as well). Since this runs after SSA restoration, the mappings is used
// to map Vregs back to their original Vregs when necessary.
void set_spill_reg_classes(State& state,
                           const SpillerResults& results,
                           const jit::fast_map<Vreg, Vreg>& mappings) {
  auto& unit = state.unit;

  // Mark that the given Vreg is a spill of the appropriate width.
  auto const spillize = [&] (Vreg r) {
    assertx(!r.isPhys());
    auto& info = reg_info(state, r);
    auto const cls = info.regClass;
    if (is_spill(cls)) return;
    assertx(cls != RegClass::SF);
    if (cls == RegClass::SIMDWide) {
      info.regClass = RegClass::SpillWide;
    } else {
      info.regClass = RegClass::Spill;
    }
  };

  for (auto const b : state.rpo) {
    auto const& in = results.perBlock[b].in;
    auto const& phi = results.perBlock[b].inPhi;
    assertx(in);

    for (auto const& inst : unit.blocks[b].code) {
      switch (inst.op) {
        case Vinstr::phidef: {
          // Propagate "spillness" across the phi. If the block's phi state
          // indicates that the phi output is spilled, the corresponding dest
          // Vreg must be spillized.

          // This is a bit tricky because the SSA restoration pass that we run
          // before this may have created new phis, or expanded existing
          // ones. Therefore we cannot trust the phi state blindly. If the index
          // exists in the phi state, we can use it. Otherwise we need to map
          // the Vreg back to its pre-SSAize value and use the block state.
          auto const& defs = unit.tuples[inst.phidef_.defs];
          for (size_t i = 0; i < defs.size(); ++i) {
            auto const r = defs[i];

            auto const s = in->forReg(r);
            if (!s) continue;
            if (phi && i < phi->size()) {
              // This phi and index existed before the SSA pass, so we can just
              // use the phi state.
              if (!(*phi)[i]) spillize(r);
              continue;
            }

            // Otherwise this wasn't part of a phi originally but was turned
            // into one. Consult the mapping table to learn what the Vreg was
            // originally and use the block state to see if it was spilled.
            auto const it = mappings.find(r);
            auto const oldR = (it != mappings.end()) ? it->second : r;
            if (s->inMem[oldR]) {
              spillize(r);
            } else {
              assertx(s->inReg[oldR]);
            }
          }
          break;
        }
        case Vinstr::spill:
        case Vinstr::spillbi:
        case Vinstr::spillli:
        case Vinstr::spillqi:
        case Vinstr::spillundefq:
          // The dest of a spill instruction is a spill.
          spillize(spill_inst_dest(inst));
          break;
        case Vinstr::copy: {
          // Propagate "spillness" across copy
          auto const s = inst.copy_.s;
          auto const d = inst.copy_.d;
          if (is_ignored(state, s)) break;
          auto const cls = reg_class(state, s);
          if (is_spill(cls)) spillize(d);
          break;
        }
        case Vinstr::copyargs: {
          // Propagate "spillness" across copy
          auto const& uses = unit.tuples[inst.copyargs_.s];
          auto const& defs = unit.tuples[inst.copyargs_.d];
          assertx(uses.size() == defs.size());
          for (size_t i = 0; i < uses.size(); ++i) {
            assertx(is_ignored(state, uses[i]) == is_ignored(state, defs[i]));
            if (is_ignored(state, uses[i])) continue;
            auto const cls = reg_class(state, uses[i]);
            if (is_spill(cls)) spillize(defs[i]);
          }
          break;
        }
        default:
          break;
      }
    }
  }

  // Everything after this is sanity checking.
  if (!debug) return;

  // Do a pass over the unit making sure that the RegClasses of everything is
  // sane.
  auto const appropriate = [] (RegClass spill, RegClass reg) {
    switch (reg) {
      case RegClass::GP:
      case RegClass::SIMD:
      case RegClass::AnyNarrow:
        return spill == RegClass::Spill;
      case RegClass::SIMDWide:
        return spill == RegClass::SpillWide;
      case RegClass::Any:
      case RegClass::SF:
      case RegClass::Spill:
      case RegClass::SpillWide:
        break;
    }
    always_assert(false);
  };

  for (auto const b : state.rpo) {
    for (auto& inst : unit.blocks[b].code) {
      switch (inst.op) {
        case Vinstr::spill: {
          // The dest of a spill should be a spill slot, and the source should
          // not be.
          always_assert(!inst.spill_.d.isPhys());
          // We can have spills from physical registers
          if (!is_ignored(state, inst.spill_.s)) {
            auto const dCls = reg_class(state, inst.spill_.d);
            auto const sCls = reg_class(state, inst.spill_.s);
            always_assert(is_spill(dCls));
            always_assert(!is_spill(sCls));
            always_assert(appropriate(dCls, sCls));
          }
          break;
        }
        case Vinstr::spillbi:
        case Vinstr::spillli:
        case Vinstr::spillqi:
        case Vinstr::spillundefq: {
          // The dest of a spill immediate should be a spill slot.
          auto const d = spill_inst_dest(inst);
          always_assert(!d.isPhys());
          auto const dCls = reg_class(state, d);
          always_assert(is_spill(dCls));
          break;
        }
        case Vinstr::reload: {
          // The source of a reload should be a spill slot, and the dest should
          // not be.
          always_assert(!inst.reload_.s.isPhys());
          auto const dCls = reg_class(state, inst.reload_.d);
          auto const sCls = reg_class(state, inst.reload_.s);
          always_assert(!is_spill(dCls));
          always_assert(is_spill(sCls));
          always_assert(appropriate(sCls, dCls));
          break;
        }
        case Vinstr::phidef: {
          // The source/dest pairs of a phijmp/phidef should always be the same
          // RegClass. There shouldn't be any copying between physical registers
          // and Vregs.
          auto const& defs = unit.tuples[inst.phidef_.defs];
          for (auto const& pred : state.preds[b]) {
            always_assert(unit.blocks[pred].code.back().op == Vinstr::phijmp);
            auto const& uses = unit.tuples[
              unit.blocks[pred].code.back().phijmp_.uses
            ];
            always_assert(defs.size() == uses.size());
            for (size_t i = 0; i < defs.size(); ++i) {
              always_assert(
                is_ignored(state, defs[i]) == is_ignored(state, uses[i])
              );
              if (is_ignored(state, defs[i])) continue;
              auto const dCls = reg_class(state, defs[i]);
              auto const uCls = reg_class(state, uses[i]);
              always_assert(dCls == uCls);
            }
          }
          break;
        }
        case Vinstr::phijmp:
          // Handled as part of phidef
          break;
        case Vinstr::copy: {
          // The source and dest of a copy should both be spill slots or both
          // should not be, never a mix. The widths of the spill slots should be
          // the same.
          auto const s = inst.copy_.s;
          auto const d = inst.copy_.d;
          if (is_ignored(state, s)) {
            always_assert(
              is_ignored(state, d) || !is_spill(reg_class(state, d))
            );
            break;
          }
          if (is_ignored(state, d)) {
            always_assert(!is_spill(reg_class(state, s)));
            break;
          }
          auto const dCls = reg_class(state, d);
          auto const uCls = reg_class(state, s);
          always_assert(is_spill(dCls) == is_spill(uCls));
          always_assert(IMPLIES(is_spill(dCls), dCls == uCls));
          break;
        }
        case Vinstr::copyargs: {
          // Same as Vinstr::copy, but with a tuple
          auto const& uses = unit.tuples[inst.copyargs_.s];
          auto const& defs = unit.tuples[inst.copyargs_.d];
          always_assert(uses.size() == defs.size());
          for (size_t i = 0; i < uses.size(); ++i) {
            always_assert(
              is_ignored(state, uses[i]) == is_ignored(state, defs[i])
            );
            if (is_ignored(state, uses[i])) break;
            auto const dCls = reg_class(state, defs[i]);
            auto const uCls = reg_class(state, uses[i]);
            always_assert(is_spill(dCls) == is_spill(uCls));
            always_assert(IMPLIES(is_spill(dCls), dCls == uCls));
          }
          break;
        }
        default:
          // For most instructions, both the sources and dests should not be
          // spill slots.
          for (auto const r : uses_set_cached(state, inst)) {
            always_assert(!is_spill(reg_class(state, r)));
          }
          for (auto const r : defs_set_cached(state, inst)) {
            always_assert(!is_spill(reg_class(state, r)));
          }
          break;
      }
    }
  }
}

// Remove dead spills (and any other Vregs made dead by removing
// spills). This logically does the same thing as removeDeadCode(),
// but is optimized to take advantage of the fact we only want to
// remove spills, and can leverage our block changed set.
void remove_dead_spills(State& state, SpillerResults& results) {
  auto& unit = state.unit;

  // The set of Vregs which are being considered by DCE. Initially
  // this is any Vregs which have been rematerialized (which implies
  // we may have created dead spills). We'll extend it with any Vregs
  // which are used by those spills, and other interesting Vregs
  // transitively. Only instructions using interesting Vregs are
  // eligible for removal.
  VregSet interesting{results.rematerialized};
  // The set of Vregs which are maybe used. Instructions defining used
  // Vregs cannot be removed.
  VregSet used;

  // Check if an instruction is removable (modulo any of its
  // defs/uses). An instruction is removable if it doesn't have
  // side-effects and doesn't write to a physical register.
  auto const removable = [&] (const Vinstr& inst) {
    if (effectful(inst)) return false;
    auto hasPhys = false;
    visitDefs(unit, inst, [&] (Vreg r) { hasPhys |= r.isPhys(); });
    return !hasPhys;
  };

  /*
   * Analysis loop:
   *
   * The analysis is done in two passes. For the first pass, we
   * calculate which Vregs are interesting and which Vregs are
   * used. For the second pass, we only calculate which Vregs are
   * used. This needs to be done because which Vregs are used is
   * influenced by which are interesting. Each pass processes the unit
   * until we reach a fixed-point. So, we calculate the interesting
   * Vregs first, then calculate the used in the second pass. Since we
   * should have picked up most of the used Vregs in the first pass,
   * the second pass should be quick.
   */

  auto const analyze = [&] (bool secondPass) {
    auto again = true;
    while (again) {
      again = false;

      // Check if r is used. A physical register is always considered
      // used. If we're in the second pass, a non-interesting register
      // is considered used (because we won't remove it).
      auto const isUsed = [&] (Vreg r) {
        return used[r] ||
          r.isPhys() ||
          (secondPass && !interesting[r]);
      };

      // If r is interesting, mark it as used.
      auto const use = [&] (Vreg r) {
        if (!interesting[r] || used[r]) return;
        again = true;
        used.add(r);
      };

      // If d is interesting, mark s as interesting as well. We'll
      // only do this during the first pass.
      auto const link = [&] (Vreg s, Vreg d) {
        if (!secondPass) {
          if (interesting[d] && !s.isPhys() && !interesting[s]) {
            again = true;
            interesting.add(s);
          }
        }
        if (isUsed(d)) use(s);
      };

      for (auto const b : boost::adaptors::reverse(state.rpo)) {
        // If this block doesn't contain anything interesting, skip
        // it.
        if (!results.changed[b] &&
            !state.uses[b].intersects(interesting) &&
            !state.defs[b].intersects(interesting)) {
          continue;
        }

        auto& block = unit.blocks[b];
        for (auto& inst : boost::adaptors::reverse(block.code)) {
          switch (inst.op) {
            case Vinstr::copy:
              // For the copy-ish instructions, link the src and dest
              // pairs. The src is used if the dest is.
              link(inst.copy_.s, inst.copy_.d);
              break;
            case Vinstr::copyargs: {
              auto const& srcs = unit.tuples[inst.copyargs_.s];
              auto const& dsts = unit.tuples[inst.copyargs_.d];
              assertx(srcs.size() == dsts.size());
              for (size_t i = 0; i < srcs.size(); ++i) link(srcs[i], dsts[i]);
              break;
            }
            case Vinstr::phidef: {
              auto const& defs = unit.tuples[inst.phidef_.defs];
              for (auto const p : state.preds[b]) {
                auto const& pred = unit.blocks[p];
                assertx(pred.code.back().op == Vinstr::phijmp);
                auto const& uses = unit.tuples[pred.code.back().phijmp_.uses];
                assertx(uses.size() == defs.size());
                for (size_t i = 0; i < defs.size(); ++i) link(uses[i], defs[i]);
              }
              break;
            }
            case Vinstr::phijmp: {
              if (!secondPass) break;
              auto const& uses = unit.tuples[inst.phijmp_.uses];
              for (auto const s : succs(unit.blocks[b])) {
                auto const& succ = unit.blocks[s];
                assertx(succ.code.front().op == Vinstr::phidef);
                auto const& defs = unit.tuples[succ.code.front().phidef_.defs];
                assertx(uses.size() == defs.size());
                for (size_t i = 0; i < uses.size(); ++i) {
                  if (isUsed(defs[i])) use(uses[i]);
                }
              }
              break;
            }
            case Vinstr::spill:
              // Spill/reloads are treated like copies.
              link(inst.spill_.s, inst.spill_.d);
              break;
            case Vinstr::reload:
              link(inst.reload_.s, inst.reload_.d);
              break;
            default: {
              if (removable(inst)) {
                auto const& defs = defs_set_cached(state, inst);
                if (!secondPass) {
                  // In the first pass, if all of the defs are
                  // interesting, then any src is interesting.
                  auto const allInteresting = std::all_of(
                    defs.begin(),
                    defs.end(),
                    [&] (Vreg r) { return interesting[r]; }
                  );
                  if (allInteresting) {
                    for (auto const r : uses_set_cached(state, inst)) {
                      if (r.isPhys() || interesting[r]) continue;
                      again = true;
                      interesting.add(r);
                    }
                  }
                }

                // For generic instructions, if any def is used, then
                // any interesting srcs are used.
                auto const anyUsed =
                  std::any_of(defs.begin(), defs.end(), isUsed);
                if (anyUsed) {
                  auto const& uses = uses_set_cached(state, inst);
                  if (uses.intersects(interesting)) {
                    for (auto const r : uses) use(r);
                  }
                }
              } else if (!secondPass) {
                // The instruction isn't removable. Any interesting
                // srcs are unconditionally used.
                auto const& uses = uses_set_cached(state, inst);
                if (uses.intersects(interesting)) {
                  for (auto const r : uses) use(r);
                }
              }

              break;
            }
          }
        }
      }
    }
  };

  // Run the analysis passes
  analyze(false);
  analyze(true);

  // If there are not any interesting non-used Vregs, nothing to do.
  auto const toRemove = interesting - used;
  if (toRemove.none()) return;

  // Removal pass
  for (auto const b : state.rpo) {
    // If this block does not have any interesting Vregs, we can skip
    // it.
    if (!results.changed[b] &&
        !state.uses[b].intersects(toRemove) &&
        !state.defs[b].intersects(toRemove)) {
      continue;
    }

    size_t instIdx = 0;

    // Check if a Vreg is interesting and unused
    auto const unused = [&] (Vreg r) {
      return interesting[r] && !used[r];
    };

    // If the given Vreg is unused, remove the current instruction
    // (and mark that we've changed this block). Return true if we
    // removed the instruction, false otherwise.
    auto const removeIfUnused = [&] (Vreg r) {
      if (!unused(r)) return false;
      vmodify(unit, b, instIdx, [] (Vout&) { return 1; });
      results.changed[b] = true;
      return true;
    };

    while (instIdx < unit.blocks[b].code.size()) {
      auto& inst = unit.blocks[b].code[instIdx];
      switch (inst.op) {
        case Vinstr::copy:
          if (removeIfUnused(inst.copy_.d)) continue;
          break;
        case Vinstr::copyargs: {
          auto const& s = unit.tuples[inst.copyargs_.s];
          auto const& d = unit.tuples[inst.copyargs_.d];
          assertx(s.size() == d.size());

          // If none of the defs are used, don't change anything. If
          // all are, remove the copy entirely. If only some are,
          // shrink the copyargs list.
          if (std::none_of(d.begin(), d.end(), unused)) break;
          if (std::all_of(d.begin(), d.end(), unused)) {
            vmodify(unit, b, instIdx, [] (Vout& v) { return 1; });
            results.changed[b] = true;
            continue;
          }

          VregList newSrcs;
          VregList newDsts;
          for (size_t i = 0; i < s.size(); ++i) {
            if (unused(d[i])) continue;
            newSrcs.emplace_back(s[i]);
            newDsts.emplace_back(d[i]);
          }

          assertx(!newSrcs.empty());
          assertx(newSrcs.size() < s.size());

          if (newSrcs.size() == 1) {
            inst.op = Vinstr::copy;
            inst.copy_ = copy{newSrcs[0], newDsts[0]};
          } else {
            inst.copyargs_.s = unit.makeTuple(std::move(newSrcs));
            inst.copyargs_.d = unit.makeTuple(std::move(newDsts));
          }
          invalidate_cached_operands(inst);
          results.changed[b] = true;
          break;
        }
        case Vinstr::phidef: {
          auto d = &unit.tuples[inst.phidef_.defs];
          // Treat phidef similarily to copyargs
          if (std::none_of(d->begin(), d->end(), unused)) break;

          // One extra complication is that if we remove the phidef,
          // we need to turn the phijmps in the predecessors to jmps.
          if (std::all_of(d->begin(), d->end(), unused)) {
            for (auto const pred : state.preds[b]) {
              auto& phijmp = unit.blocks[pred].code.back();
              assertx(phijmp.op == Vinstr::phijmp);
              assertx(phijmp.phijmp_.target == b);
              phijmp.op = Vinstr::jmp;
              phijmp.jmp_.target = b;
              invalidate_cached_operands(phijmp);
              results.changed[pred] = true;
            }
            vmodify(unit, b, instIdx, [] (Vout& v) { return 1; });
            results.changed[b] = true;
            continue;
          }

          // Otherwise selectively remove Vregs in the predecessor
          // phijmps.
          for (auto const pred : state.preds[b]) {
            auto& phijmp = unit.blocks[pred].code.back();
            assertx(phijmp.op == Vinstr::phijmp);
            auto const& s = unit.tuples[phijmp.phijmp_.uses];
            assertx(s.size() == d->size());

            VregList newSrcs;
            for (size_t i = 0; i < d->size(); ++i) {
              if (unused((*d)[i])) continue;
              newSrcs.emplace_back(s[i]);
            }
            assertx(!newSrcs.empty());
            assertx(newSrcs.size() < s.size());

            phijmp.phijmp_.uses = unit.makeTuple(std::move(newSrcs));
            d = &unit.tuples[inst.phidef_.defs];
            invalidate_cached_operands(phijmp);
            results.changed[pred] = true;
          }

          VregList newDsts;
          for (size_t i = 0; i < d->size(); ++i) {
            if (unused((*d)[i])) continue;
            newDsts.emplace_back((*d)[i]);
          }
          assertx(!newDsts.empty());
          assertx(newDsts.size() < d->size());

          inst.phidef_.defs = unit.makeTuple(std::move(newDsts));
          invalidate_cached_operands(inst);
          results.changed[b] = true;
          break;
        }
        case Vinstr::phijmp:
          // Should be dealt with when processing phidefs.
          break;
        case Vinstr::spill:
          if (removeIfUnused(inst.spill_.d)) continue;
          break;
        case Vinstr::reload:
          if (removeIfUnused(inst.reload_.d)) continue;
          break;
        default: {
          if (!removable(inst)) break;
          auto const& d = defs_set_cached(state, inst);
          if (!std::all_of(d.begin(), d.end(), unused)) break;
          vmodify(unit, b, instIdx, [] (Vout& v) { return 1; });
          results.changed[b] = true;
          continue;
        }
      }

      ++instIdx;
    }
  }
}

void insert_spills(State& state) {
  // Calculate liveness. This is needed to determine if spilling is
  // necessary.
  calculate_liveness(state);

  auto const needsSpilling = determine_spilling_needed(state);
  // If no block can possibly spill, the spiller can be skipped
  // entirely.
  if (!needsSpilling.any) return;

  // Generate spills and fixup mismatches between blocks.
  auto results = process_spills(state, needsSpilling);
  SCOPE_ASSERT_DETAIL("Spiller State") { return results.toString(state); };
  hoist_loop_spills(state, results);
  fixup_spill_mismatches(state, results);

  // Account for Vregs we marked as needing re-SSAization. Any block
  // which uses or defines them requires attention from restoreSSA().
  for (auto const b : state.rpo) {
    if (!results.changed[b] &&
        (state.defs[b].intersects(results.ssaize) ||
         state.uses[b].intersects(results.ssaize))) {
      results.changed[b] = true;
    }
  }

  // Spilling may break SSA form because we use the same Vreg when
  // both spilled and reloaded, so fix that here.
  auto const mappings = [&] {
    auto const mappings = restoreSSA(
      state.unit,
      results.ssaize,
      results.changed,
      state.rpo,
      state.preds,
      0
    );
    for (auto const& map : mappings) {
      auto const& info = reg_info(state, map.second);
      set_reg_class_bits(state, map.first, info.regClass);
      reg_info_insert(state, map.first, info);
      if (results.rematerialized[map.second]) {
        results.rematerialized.add(map.first);
      }
    }
    return mappings;
  }();
  assertx(check(state.unit));

  // Update the reg-classes of spilled Vregs to mark that they are
  // spills.
  set_spill_reg_classes(state, results, mappings);

  // If we rematerialized anything, we may have created dead spills,
  // so remove them.
  if (results.rematerialized.any()) remove_dead_spills(state, results);

  // Do this check before re-calculating liveness. If we've broken SSA
  // form, this will report a better diagnostic rather than having
  // liveness calculation fail.
  assertx(check(state.unit));

  // Re-calculate liveness since we changed Vregs
  calculate_liveness(state, &results.changed);
}

//////////////////////////////////////////////////////////////////////
// Coloring

/*
 * As already mentioned, now that the spiller has lowered the register
 * pressure everywhere to below the number of physical registers, the
 * unit is guaranteed to be colorable.
 *
 * For instructions which do not write to physical registers, the
 * coloring is easy. Simply assign the defs the next available
 * physical registers. Once a Vreg becomes dead, the register is
 * released.
 *
 * For instructions which do write to physical registers, it is more
 * complex, however. An instruction could write to a physical register
 * which has already been assigned to a Vreg which is live across the
 * instruction. This means we need to migrate that Vreg to a new
 * (free) physical register (inserting a copy). For this reason, a
 * Vreg can potentially be assigned different physical registers at
 * different points in the unit.
 *
 * Spill slots are assigned to spill in a different manner using
 * global analysis. The "coloring" just looks up the already assigned
 * slot. Spills never change their color once assigned.
 *
 * While technically the colorer has the freedom to choose arbitrary
 * physical registers for Vregs, in practice most colorings are
 * terrible. They leave a lot of copies, and because of the
 * aforementioned physical def constraints, causes a lot of
 * shuffling. We use a heuristic using "penalty vectors" to try to
 * find a good coloring on the fly.
 *
 * Once a physical register has been assigned to a Vreg at a
 * particular instruction, the instruction will be rewritten to
 * operate on that physical register directly. This is easier because,
 * as mentioned above, a Vreg can be in different physical registers
 * at different points, and therefore we avoid having a complicated
 * per-instruction mapping. It is also more efficient to do the
 * rewriting as we color, instead of a separate pass. Spills are
 * rewritten separately.
 */

/*
 * Spill Coloring:
 *
 * We use a global analysis to assign spills to slots. This generates
 * very good results, but is potentially expensive. However it is not
 * an issue in practice because units tend to have a very small number
 * of spills.
 *
 * First we generate spill affinities. Two spills have an affinity
 * with each other if they do not interfere and have a copy between
 * them. The weight of the affinity is the block weight of the copy.
 *
 * We build up chunks from the affinities. Initially each spill is
 * assigned its own chunk. We then walk the affinities (sorted by
 * descending weight), and try to merge together the two spills'
 * chunks. We merge the chunks if all the spills in the merged chunk
 * don't interfere with each other (and have a compatible reg-class).
 *
 * Once we've built up the chunks this way, each chunk corresponds to
 * a single slot. Since every spill in the chunk doesn't interfere with
 * any other, they can be assigned the same slot. Indeed, since
 * they'll (tend to) share affinities, it's profitable for them to
 * share the same slot.
 *
 * These assignments are stored away, where the colorer will look them
 * up while walking the unit.
 */

struct SpillAffinity {
  Vreg r1;
  Vreg r2;
  size_t weight;
};

void assign_spill_colors(State& state,
                         const VregSet& spills,
                         const jit::fast_map<Vreg, VregSet>& interferences,
                         jit::vector<SpillAffinity> affinities) {
  assertx(spills.any());

  // Sort affinities by weight. Spills with higher weighted affinities
  // are more profitable to put into the same chunk (where they'll be
  // assigned the same slot).
  std::sort(
    affinities.begin(),
    affinities.end(),
    [&] (const SpillAffinity& a1, const SpillAffinity& a2) {
      if (a1.weight != a2.weight) return a1.weight > a2.weight;
      if (a1.r1 != a2.r1) return a1.r1 < a2.r1;
      return a1.r2 < a2.r2;
    }
  );

  // Set of active chunks.
  jit::vector<VregSet> chunks;
  // Map a chunk to its canonical representation.
  jit::vector<size_t> chunkMappings;
  // Mapping of a spill to the chunk it's currently assigned to.
  jit::fast_map<Vreg, size_t> spillToChunk;

  // Assign each spill its own chunk. Every chunk maps to itself.
  for (auto const r : spills) {
    spillToChunk[r] = chunks.size();
    chunks.emplace_back(r);
    chunkMappings.emplace_back(chunkMappings.size());
  }

  // Canonicalize a chunk index to the index of the chunk it's been
  // merged with. This can update the mappings as it goes (to shorten
  // lookup chains).
  auto const canonicalize = [&] (size_t chunk) {
    auto const impl = [&] (size_t chunk, auto const& self) {
      auto& chunk2 = chunkMappings[chunk];
      if (chunk == chunk2) return chunk;
      assertx(chunk2 < chunk);
      chunk2 = self(chunk2, self);
      return chunk2;
    };
    return impl(chunk, impl);
  };

  // Map a spill to the chunk it currently occupies.
  auto const lookup = [&] (Vreg r) {
    auto const it = spillToChunk.find(r);
    assertx(it != spillToChunk.end());
    auto const chunk = canonicalize(it->second);
    it->second = chunk;
    return chunk;
  };

  // Merge two chunks together
  auto const combine = [&] (size_t chunk1, size_t chunk2) {
    // Find their canonical forms
    chunk1 = canonicalize(chunk1);
    chunk2 = canonicalize(chunk2);
    assertx(chunk1 != chunk2);
    // Merge the one with the higher index into the lesser. Move the
    // contents of the source into the dest, empty the source, and
    // update the mappings so that the source now canonicalizes to the
    // dest.
    if (chunk1 > chunk2) std::swap(chunk1, chunk2);
    chunks[chunk1] |= chunks[chunk2];
    chunks[chunk2].reset();
    chunkMappings[chunk2] = chunk1;
    return chunk1;
  };

  // Check if any Vreg in c1 interferes with any Vreg in c2. This is a
  // naive N^2 check, but it's fine for the small number of spills we
  // encounter.
  auto const interferes = [&] (const VregSet& c1, const VregSet& c2) {
    // We only need to check the first reg as all registers in the
    // chunk must have the same reg class.
    assertx(!c1.empty());
    assertx(!c2.empty());
    if (reg_info(state, *c1.begin()).regClass !=
        reg_info(state, *c2.begin()).regClass) {
      return true;
    }

    for (auto const r1 : c1) {
      auto const it = interferences.find(r1);
      if (it == interferences.end()) continue;
      for (auto const r2 : c2) {
        if (it->second[r2]) return true;
      }
    }

    return false;
  };

  // Loop over the affinities (which have been sorted by
  // weight). Attempt to merge non-interfering chunks.
  for (auto const& affinity : affinities) {
    auto const r1 = affinity.r1;
    auto const r2 = affinity.r2;
    auto const chunk1 = lookup(r1);
    auto const chunk2 = lookup(r2);
    if (chunk1 == chunk2) continue;
    if (interferes(chunks[chunk1], chunks[chunk2])) continue;
    combine(chunk1, chunk2);
  }

  // We can still have non-interfering chunks which do not share any
  // affinities. Attempt to merge these which just minimizes the total
  // number of slots needed.
  for (size_t i = 0; i < chunks.size(); ++i) {
    if (chunks[i].none()) continue;
    for (size_t j = i+1; j < chunks.size(); ++j) {
      if (chunks[j].none()) continue;
      if (interferes(chunks[i], chunks[j])) continue;
      combine(i, j);
    }
  }

  assertx(state.numSpillSlots == 0);
  assertx(state.numWideSpillSlots == 0);

  // Now assign an unique slot for each remaining non-empty chunk.
  for (auto const& chunk : chunks) {
    if (chunk.none()) continue;

    Optional<bool> isWide;
    for (auto const r : chunk) {
      auto const& info = reg_info(state, r);
      assertx(is_spill(info.regClass));
      assertx(!isWide || *isWide == (info.regClass == RegClass::SpillWide));
      isWide = info.regClass == RegClass::SpillWide;

      assertx(state.spillColors.find(r) == state.spillColors.end());
      auto const color = *isWide
        ? Color { SpillSlotWide { state.numWideSpillSlots } }
        : Color { SpillSlot { state.numSpillSlots } };
      auto const DEBUG_ONLY result = state.spillColors.emplace(r, color);
      assertx(result.second);
    }

    if (*isWide) {
      ++state.numWideSpillSlots;
    } else {
      ++state.numSpillSlots;
    }
  }

  // Make sure the spill slot count is round (for alignment).
  if ((state.numSpillSlots % 2) == 1) ++state.numSpillSlots;
}

/*
 * Penalty Vectors:
 *
 * When assigning physical registers for Vregs, we want to choose a
 * register which will minimize total copies in the unit. However this
 * requires forward looking information. The colorer works an
 * instruction at a time and doesn't necessarily know how the Vreg
 * will be used. Penalty vectors are used to represent this
 * information.
 *
 * The vectors are a map of physical registers to penalty weights,
 * where the weight is the number of (dynamic) copies we'll have to
 * pay if you assign the Vreg that physical register. When assigning a
 * Vreg a physical register, we want to assign it the physical
 * register with the lowest weight.
 *
 * Two things affect the penalty vector for a Vreg:
 *
 * - If any physical register is used as a def in an instruction while
 *   the Vreg is alive. This induces a penalty on that physical
 *   register, since if the Vreg is assigned that register, we'll be
 *   forced to emit a copy to a different register. The penalty is the
 *   block weight where the instruction is.
 *
 * - If the Vreg is a hinted use of an instruction with a physical
 *   register def. This induces a penalty on every register *except*
 *   that physical register. This is because if we assign the Vreg any
 *   register except the def, we won't be able to satisfy the hint
 *   (this includes copy instructions).
 *
 * In addition, we want to satisfy as many hints as possible, even if
 * a physical register is not involved. If two Vregs are connected
 * with a hint, and do not interfere (if they interfere the hint can
 * never be satisfied so we ignore it), we merge their penalty vectors
 * together and have them share the same vector. Therefore both Vregs
 * will tend to be assigned the same register and we'll likely satisfy
 * the hint.
 *
 * So, we build up initial penalty vectors for all Vregs using the
 * above criteria. We iterate over the unit backwards, assessing
 * penalties, and merging hinted Vregs.
 *
 * While coloring, we remove penalties as we process
 * instructions. That is, once we process an instruction with one of
 * the above cases, we undo its effect on the penalty vector. The idea
 * being, we've already colored that instruction and therefore it
 * shouldn't influence any future decisions. This means that once
 * we're done coloring, all penalty vectors should contain zeros.
 *
 * Note that the penalty vector doesn't reflect a hint from a physical
 * register use to a non-physical register def (just the
 * opposite). That is dealt with by the colorer directly.
 *
 * For phis, it's possible to color the destination of the phi before
 * one of the sources. In that case, we modify the penalty vector on
 * the fly like the first case above. This helps ensure the phi
 * sources will be colored like the def.
 */

// Calculate penalty vectors for all (non-spill) Vregs (and calculate
// spill colors as well).
void calculate_penalties(State& state) {
  auto& unit = state.unit;

  // A penalty index of zero means not set, so have a dummy initial
  // entry.
  state.penalties.emplace_back();

  // While merging penalty vectors, every Vreg will have a "canonical"
  // Vreg. The canonical Vreg will point at the penalty vector that
  // the original Vreg should use. This allows for fast merging.
  jit::vector<Vreg> mappings;
  mappings.resize(unit.next_vr);

  auto const canonicalize = [&] (Vreg r) {
    auto const impl = [&] (Vreg r, auto const& self) {
      auto& r2 = mappings[r];
      if (!r2.isValid() || r == r2) return r;
      assertx(r2 < r);
      r2 = self(r2, self);
      return r2;
    };
    return impl(r, impl);
  };

  jit::vector<SpillAffinity> spillAffinities;

  // Mark that two Vregs are connected by a hint (with the given
  // weight). If the Vregs are spills, this records a spill
  // affinity. Otherwise it attempts to merge their penalty vectors
  // together and marks them as shared.
  auto const unify = [&] (Vreg use, Vreg def, size_t weight) {
    if (use == def) return;
    if (use.isPhys() || is_ignored(state, def)) return;

    // Hints with incompatible reg-classes can never be satisfied, so
    // ignore them.
    auto const cls1 = reg_class(state, use);
    auto const cls2 = reg_class(state, def);
    if (!compatible_reg_classes(cls1, cls2)) return;

    // Special case, the hint def is a physical register. We don't
    // unify with physical registers (they don't have penalty
    // vectors), but we can modify the use's penalty vector to bias
    // toward this physical register.
    if (def.isPhys()) {
      // Get the canonical penalty vector
      auto const canon = canonicalize(use);
      auto& info = reg_info(state, canon);
      if (!info.penaltyIdx) {
        info.penaltyIdx = state.penalties.size();
        state.penalties.emplace_back();
      }
      // Assess a penalty for any register, except for the def.
      auto& v = state.penalties[info.penaltyIdx];
      auto const phys = def.physReg();
      for (auto const r : v) {
        if (r != phys) v[r] += weight;
      }
      return;
    }

    // Spills. Just record the affinity.
    if (is_spill(cls1)) {
      assertx(cls1 == cls2);
      spillAffinities.emplace_back(SpillAffinity{use, def, weight});
      return;
    }

    // Attempt to merge their penalty vectors
    use = canonicalize(use);
    def = canonicalize(def);
    // Already merged
    if (use == def) return;

    // We merge the penalty vector for the higher Vreg into the lesser
    // Vreg.
    auto const src = (use < def) ? def : use;
    auto const dst = (use < def) ? use : def;
    mappings[src] = dst;

    // The source penalty vector hasn't been allocated yet, so nothing
    // to merge.
    auto const srcIdx = reg_info(state, src).penaltyIdx;
    if (!srcIdx) return;

    // The dest penalty vector hasn't been allocated yet, so do so
    // now.
    auto& dstInfo = reg_info(state, dst);
    if (!dstInfo.penaltyIdx) {
      dstInfo.penaltyIdx = state.penalties.size();
      state.penalties.emplace_back();
    }

    // Merge the penalty vectors, zeroing out the source vector.
    auto& srcPenalties = state.penalties[srcIdx];
    auto& dstPenalties = state.penalties[dstInfo.penaltyIdx];
    for (auto const r : srcPenalties) {
      dstPenalties[r] += srcPenalties[r];
      srcPenalties[r] = 0;
    }
  };

  jit::fast_map<Vreg, VregSet> spillInterferences;
  VregSet spills;
  VregSet nonSpills;

  // Build up penalty vectors and spill information
  for (auto const b : boost::adaptors::reverse(state.rpo)) {
    auto& block = unit.blocks[b];
    auto const weight = block_weight(state, b);

    // Deal with phijmps specially, since they do not have hinted
    // operands, but we want to treat them as such with the phidef.
    if (block.code.back().op == Vinstr::phijmp) {
      auto const& phijmp = block.code.back().phijmp_;
      for (auto const succ : succs(block)) {
        auto const& succBlock = unit.blocks[succ];
        assertx(succBlock.code.front().op == Vinstr::phidef);
        auto const& phidef = succBlock.code.front().phidef_;
        auto const& liveIn = state.liveIn[succ];
        auto const& uses = unit.tuples[phijmp.uses];
        auto const& defs = unit.tuples[phidef.defs];
        assertx(uses.size() == defs.size());
        for (size_t i = 0; i < uses.size(); ++i) {
          if (liveIn[uses[i]]) continue;
          unify(uses[i], defs[i], weight);
        }
      }
    }

    // We need liveness information to know which hints are
    // unsatisfiable, so walk backwards modifying it on the fly.
    auto live = state.liveOut[b];

    for (auto& inst : boost::adaptors::reverse(block.code)) {
      // Attempt to unify hinted src/dest pairs. Each hinted src can
      // be used only once (more than one is not satisfiable), so
      // ignore multiples.
      VregSet hints;
      auto const processDef = [&] (Vreg r, Vreg hint) {
        if (!r.isPhys() && is_colorable_reg(reg_class(state, r))) {
          nonSpills.add(r);
        }
        if (!hint.isValid() || live[hint] || hints[hint]) return;
        hints.add(hint);
        unify(hint, r, weight);
      };
      visitDefsWithHints(unit, inst, processDef);

      auto const& defs = defs_set_cached(state, inst);

      // If we don't have spills and there's no physical registers in
      // the defs, we can skip the below logic since the penalty
      // vectors can't change. We only have to update the liveness
      // information.
      if (state.spilled || defs.containsPhys()) {
        // Add acrosses to the live set, since they interfer with the
        // defs. The defs will be checked directly when needed rather
        // than being added to the live set.
        live |= acrosses_set_cached(state, inst);
        // Keep physical registers out of the live set since we don't
        // care about them below and don't want to waste time
        // iterating over them.
        live.removePhys();

        for (auto const defReg : defs) {
          // If a physical register is defined, assess a penalty for
          // it to any live Vreg penalty vectors.
          if (defReg.isPhys()) {
            auto const phys = defReg.physReg();
            for (auto const liveReg : live) {
              // Physical registers should have been removed above.
              assertx(!liveReg.isPhys());
              if (defs[liveReg]) continue;
              if (!is_colorable_reg(reg_class(state, liveReg))) continue;

              // Assess a penalty for the def physical register since
              // any Vreg with that register at this point will need
              // to be moved.
              auto const canon = canonicalize(liveReg);
              auto& info = reg_info(state, canon);
              if (!info.penaltyIdx) {
                info.penaltyIdx = state.penalties.size();
                state.penalties.emplace_back();
              }
              state.penalties[info.penaltyIdx][phys] += weight;
            }
            continue;
          }

          // If the def is a spill, remember it, and mark any other
          // live spills as interfering with it.
          auto const& info = reg_info(state, defReg);
          if (!is_spill(info.regClass)) continue;

          spills.add(defReg);
          for (auto const liveReg : live) {
            if (defReg == liveReg) continue;
            auto const cls = reg_class(state, liveReg);
            if (info.regClass != cls) continue;
            spillInterferences[defReg].add(liveReg);
            spillInterferences[liveReg].add(defReg);
          }
          for (auto const liveReg : defs) {
            if (defReg == liveReg) continue;
            auto const cls = reg_class(state, liveReg);
            if (info.regClass != cls) continue;
            spillInterferences[defReg].add(liveReg);
            spillInterferences[liveReg].add(defReg);
          }
        }
      }

      // This is sufficient for liveness if we don't have physical
      // register defs.
      live -= defs;
      live |= uses_set_cached(state, inst);
    }

    // Assert that our liveness information matches what liveness
    // analysis calculated originally (modulo physical registers).
    assertx(
      [&] {
        for (auto const r : state.liveIn[b]) {
          always_assert(r.isPhys() || live[r]);
        }
        for (auto const r : live) {
          if (state.liveIn[b][r]) continue;
          always_assert(!r.isPhys());
          always_assert(reg_info(state, r).regClass == RegClass::SF);
        }
        return true;
      }()
    );
  }

  // Update the penalty vector index for each Vreg to point directly
  // at its penalty vector, rather than going through its canonical
  // mapping. Make sure every non-spill Vreg has a penalty vector
  // assigned. If they don't have one at this point, it means there's
  // no penalties, so give them a default one. Keep track of the
  // highest penalty vector index seen, so we can discard unused ones.
  size_t maxPenaltyIdx = 0;
  for (auto const r : nonSpills) {
    auto& info = reg_info(state, r);
    auto const canon = canonicalize(r);
    info.penaltyIdx = reg_info(state, canon).penaltyIdx;
    if (!info.penaltyIdx) {
      info.penaltyIdx = state.penalties.size();
      state.penalties.emplace_back();
    }
    maxPenaltyIdx = std::max(maxPenaltyIdx, info.penaltyIdx);
  }
  state.penalties.resize(maxPenaltyIdx + 1);

  // If we found any spills, used the interference and affinity
  // information to calculate slots for them.
  if (spills.any()) {
    assertx(state.spilled);
    assign_spill_colors(
      state,
      spills,
      spillInterferences,
      std::move(spillAffinities)
    );
  }
}

// The unit can be colored in any order thats dominance preserving
// (this includes RPO). However, we can get better coloring by walking
// the CFG in an order that prioritizes higher weight blocks. This
// function calculates such a block order.
BlockVector calculate_block_color_order(const State& state) {
  // With 1 or 2 block units, the visit order is fixed, so the result
  // will always be the same as RPO.
  if (state.rpo.size() <= 2) return state.rpo;

  // Build up traces for each block. The trace for a block is its own
  // weight plus the maximum of the traces of its predecessors. It
  // reflects the maximum possible execution count to get to this
  // block.
  jit::vector<size_t> trace;
  trace.resize(state.unit.blocks.size());

  for (auto const b : state.rpo) {
    size_t t = 0;
    for (auto const pred : state.preds[b]) t = std::max(t, trace[pred]);
    trace[b] = t + block_weight(state, b);
    assertx(trace[b] > 0);
  }

  // Sort the blocks by trace. Bigger traces first. Use RPO order to
  // break ties.
  auto blocks = state.rpo;
  std::sort(
    blocks.begin(),
    blocks.end(),
    [&] (Vlabel a, Vlabel b) {
      if (trace[a] != trace[b]) return trace[a] > trace[b];
      return state.rpoOrder[a] < state.rpoOrder[b];
    }
  );

  /*
   * Now use the traces to find a good block ordering. We want to
   * color the blocks with the higher traces first (they'll be
   * executed most often). However, since we need to preserve
   * dominance order, we cannot color a block until we've colored at
   * least one of its predecessors.
   *
   * So, select the unprocessed block with the highest trace. Select
   * the block's predecessor with the highest weight (if any) and
   * recurse. Once the recursion returns, mark the current block as
   * processed and add it to the order. Repeat this until no
   * unprocessed blocks remain.
   *
   * NB: In RPO order if the unit has no loops, all predecessors for a
   * block will be ordered before that block. This is *not* the case
   * for this ordering.
   */
  BlockVector order;
  BlockSet processed(state.unit.blocks.size());
  order.reserve(blocks.size());

  auto const addTrace = [&] (Vlabel b, auto const& self) {
    if (processed[b]) return;
    size_t bestTrace = 0;
    Vlabel bestPred;
    for (auto const pred : state.preds[b]) {
      if (state.rpoOrder[pred] >= state.rpoOrder[b]) continue;
      if (bestTrace >= trace[pred]) continue;
      bestTrace = trace[pred];
      bestPred = pred;
    }
    if (bestPred.isValid()) self(bestPred, self);
    order.emplace_back(b);
    processed[b] = true;
  };
  for (auto const b : blocks) addTrace(b, addTrace);

  return order;
}

// On x86, r8-r15 and xmm8-15 require a REX prefix on any instruction
// which uses them. This makes instructions larger, so we want to
// prefer registers (all else being equal) which do not require them.
bool isREXRegister(PhysReg r) {
  if (!arch::any<arch::X64>()) return false;
  return
    Vreg{r} >= Vreg{reg::r8} &&
    (Vreg{r} < Vreg{reg::xmm0} || Vreg{r} > Vreg{reg::xmm7});
}

// FreeRegs is responsible for tracking which physical registers are free and
// assigning them.

struct FreeRegs {
  explicit FreeRegs(State& state)
    : state{&state}
    , regs{state.gpUnreserved | state.simdUnreserved} {}

  // Choose an appropriate coloring for the given Vreg. This merely
  // chooses the color, and does not mark it as taken or modify any
  // other internal state. If the Vreg represents a spill, it will be
  // chosen from the pre-computed spill colors. Otherwise, a physical
  // register is chosen to try to maximize satisfied hints.
  //
  // For the register case, the heuristic may decide to migrate a
  // (already taken) physical register to a different (free) physical
  // register. In that case, the "moveDest" field of the coloring will
  // be set to the new physical register. One can use assignedTo() on
  // the returned color to get the source physical register.
  //
  // "moveWeight" is a lambda, which given two physical registers,
  // returns the estimated weight of moving between them.
  //
  // If the given Vreg is a physical register, that physical register
  // will always be chosen, if available. Otherwise the function will
  // consider moving the old Vreg from that physical register.

  struct Coloring {
    /* implicit */ Coloring(PhysReg r) : color{r} { assertx(r != InvalidReg); }
    /* implicit */ Coloring(Color c) : color{c} {}
    Coloring(PhysReg r, PhysReg m) : color{r}, moveDest{m}
    { assertx(r != InvalidReg); }
    Color color;
    PhysReg moveDest = InvalidReg;
  };

  template <typename W>
  Coloring choose(Vreg r, W&& moveWeight) const {
    // Find best physical register for a Vreg, choosing from the set
    // of registers in "mask".
    auto const findBest = [&] (RegSet mask) -> Coloring {
      // Comparator for two physical registers
      auto const better = [&] (PhysReg r1,
                               PhysReg r2,
                               int64_t w1,
                               int64_t w2,
                               Vreg seed) {
        // The weight is the most important factor. A lesser weight
        // is always better.
        if (w1 != w2) return w1 < w2;

        // If they have the same weight, prefer a register which is
        // available to one that is not (which would require a
        // displacement).
        auto const avail1 = regs.contains(r1);
        auto const avail2 = regs.contains(r2);
        if (avail1 != avail2) return avail1 > avail2;

        // If they have the same availability, prefer the register
        // which does not require a REX prefix to one that does. On
        // x86_64, this will generate slightly smaller code.
        auto const isREX1 = isREXRegister(r1);
        auto const isREX2 = isREXRegister(r2);
        if (isREX1 != isREX2) return isREX1 < isREX2;

        // If they are both equal otherwise, then break the tie using
        // a pseudo-random function. Hash the physical register and
        // the Vreg we're selecting for. This is preferred to just
        // using the lesser register, for example, because it helps
        // provide a wider dispersion of register uses (which
        // potentially prevents conflicts later).
        auto const perturb1 = hash_int64_pair(seed, Vreg{r1});
        auto const perturb2 = hash_int64_pair(seed, Vreg{r2});
        if (perturb1 != perturb2) return perturb1 < perturb2;

        // In the extremely rare case everything is identical, prefer
        // the lesser register
        return Vreg{r1} < Vreg{r2};
      };

      if (r.isPhys()) {
        // If the Vreg is physical, we can only ever assign it its
        // associated physical register. Check if it's free. If not,
        // we have to displace something which we'll do below.
        auto const p = r.physReg();
        if (regs.contains(p)) return p;
      } else {
        // Otherwise iterate over all the available physical registers
        // and select the best one.
        auto const& info = reg_info(*state, r);
        assertx(info.penaltyIdx > 0 &&
                info.penaltyIdx < state->penalties.size());
        auto const& penalties = state->penalties[info.penaltyIdx];

        PhysReg best = InvalidReg;
        mask.forEach(
          [&] (PhysReg r2) {
            if (best == InvalidReg ||
                better(r2, best, penalties[r2], penalties[best], r)) {
              best = r2;
              return;
            }
          }
        );

        // If we found a best one, and it's free, use that.
        if (best != InvalidReg && regs.contains(best)) return best;
      }

      struct PhysWeight {
        PhysReg reg;
        int64_t weight;
      };
      struct PhysWeightArray {
        std::array<PhysWeight, PhysReg::kMaxRegs> pairs;
        size_t size = 0;
      };

      // Build a list of candidate physical registers from
      // 'candidates', sorted by their weight ascending (calculated by
      // "calc").
      auto const buildPhysWeights = [&] (Vreg seed,
                                         RegSet candidates,
                                         auto&& calc) {
        PhysWeightArray weights;
        // Build the weights
        candidates.forEach(
          [&] (PhysReg r) {
            weights.pairs[weights.size++] = PhysWeight{r, calc(r)};
          }
        );
        // And sort them. Physical registers with lower weights are
        // always preferred. With equal weight, we prefer available
        // registers over unavailable ones. Finally, to break any
        // remaining ties, we hash the associated Vreg and the
        // physical register to come up with a pseudo-random
        // order. This helps avoid pathologies caused by lots of
        // physical registers with the same weight.
        std::sort(
          weights.pairs.begin(), weights.pairs.begin() + weights.size,
          [&] (const PhysWeight& p1, const PhysWeight& p2) {
            return better(p1.reg, p2.reg, p1.weight, p2.weight, seed);
          }
        );
        return weights;
      };

      auto const weights = [&] {
        // If the Vreg is a physical register, there's only one
        // candidate (itself) the weight is irrelevant here.
        if (r.isPhys()) {
          PhysWeightArray weights;
          weights.pairs[0] = PhysWeight{r.physReg(), 0};
          weights.size = 1;
          return weights;
        }

        // Otherwise build up the full list, using the penalty vectors
        // to order them.
        auto const& info = reg_info(*state, r);
        assertx(info.penaltyIdx > 0 &&
                info.penaltyIdx < state->penalties.size());
        auto const& penalties = state->penalties[info.penaltyIdx];
        return buildPhysWeights(
          r, mask, [&] (PhysReg r) { return penalties[r]; }
        );
      }();

      // Examine each candidate starting from the best.
      for (size_t i = 0; i < weights.size; ++i) {
        auto const current = weights.pairs[i].reg;
        auto const weight = weights.pairs[i].weight;

        // This candidate is available, just use it.
        if (regs.contains(current)) return current;

        // Otherwise, if we don't want moves here, try the next
        // one. If this is the last candidate, we're done since the
        // heuristic looks at the next possible candidate.
        if (i+1 == weights.size && !r.isPhys()) continue;

        // We support moves, so see if it's profitable to generate one.
        auto const assignedTo = occupied[current];
        assertx(assignedTo.isValid());
        // Can never move physical registers
        if (assignedTo.isPhys()) continue;

        // Get the penalty vector of the Vreg which might be moved.
        auto const& movePenalties = [&] {
          auto const idx = reg_info(*state, assignedTo).penaltyIdx;
          assertx(idx > 0 && idx < state->penalties.size());
          return state->penalties[idx];
        }();

        // Build a candidate list to move the Vreg to. We only support
        // moving to a free physical register.
        auto const moveWeights = buildPhysWeights(
          assignedTo, (mask & regs),
          [&] (PhysReg r) { return movePenalties[r]; }
        );

        // No free physical register to move to, this candidate is
        // done.
        if (!moveWeights.size) continue;

        // If we're trying to select for a physical register, we
        // *have* to move. So just go with the best one, regardless of
        // its weights.
        if (r.isPhys()) {
          assertx(current == r.physReg());
          return {current, moveWeights.pairs[0].reg};
        }

        // Otherwise check if the penalty for the move is less than
        // the win from moving it. Get the delta between the weight
        // for the current candidate and the next best one. The
        // penalty for the move is the difference between the moved
        // Vreg's current register to its new one, plus the weight of
        // the move itself. If the penalty for the move is less than
        // the candidate's penalty delta, this is an overall win and
        // we should do it.
        auto const nextWeight = weights.pairs[i+1].weight;
        auto const moveWin = nextWeight - weight;
        auto const movePenalty =
          (moveWeights.pairs[0].weight - movePenalties[current]) +
          moveWeight(current, moveWeights.pairs[0].reg);
        if (moveWin > movePenalty) {
          return {current, moveWeights.pairs[0].reg};
        }
      }

      return Coloring{None{}};
    };

    // Lookup the pre-computed spill slot for a spill. This never
    // changes.
    auto const findSlot = [&] {
      auto const it = state->spillColors.find(r);
      assertx(it != state->spillColors.end());
      return it->second;
    };

    switch (reg_class(*state, r)) {
      case RegClass::GP:
      case RegClass::AnyNarrow: return findBest(state->gpUnreserved);
      case RegClass::SIMD:
      case RegClass::SIMDWide: return findBest(state->simdUnreserved);
      case RegClass::Spill: {
        auto const c = findSlot();
        assertx(is_color_spill_slot(c));
        return c;
      }
      case RegClass::SpillWide: {
        auto const c = findSlot();
        assertx(is_color_spill_slot_wide(c));
        return c;
      }
      case RegClass::Any:
      case RegClass::SF:
        break;
    }
    always_assert(false);
  }

  // Mark the given physical register as taken. The register should not already
  // be reserved. The RegClass should be appropriate for that register.
  void reserve(Vreg r, Color c, RegClass cls) {
    switch (cls) {
      case RegClass::GP:
      case RegClass::AnyNarrow:
      case RegClass::SIMD:
      case RegClass::SIMDWide: {
        auto const reg = color_reg(c);
        assertx(regs.contains(reg));
        assertx(!occupied[reg].isValid());
        regs -= reg;
        occupied[reg] = r;
        return;
      }
      case RegClass::Spill:
        assertx(is_color_spill_slot(c));
        return;
      case RegClass::SpillWide:
        assertx(is_color_spill_slot_wide(c));
        return;
      case RegClass::Any:
      case RegClass::SF:
        break;
    }
    always_assert(false);
  }

  // Mark the given physical register as available. The register should be
  // already marked as taken. The RegClass should be appropriate for that
  // register.
  void release(Color c, RegClass cls) {
    switch (cls) {
      case RegClass::GP:
      case RegClass::AnyNarrow:
      case RegClass::SIMD:
      case RegClass::SIMDWide: {
        auto const r = color_reg(c);
        assertx(!regs.contains(r));
        assertx(occupied[r].isValid());
        regs |= r;
        occupied[r] = Vreg{};
        return;
      }
      case RegClass::Spill:
        assertx(is_color_spill_slot(c));
        return;
      case RegClass::SpillWide:
        assertx(is_color_spill_slot_wide(c));
        return;
      case RegClass::Any:
      case RegClass::SF:
        break;
    }
    always_assert(false);
  }

  // Check if the given physical register (with appropriate RegClass) is taken.
  bool available(Color c, RegClass cls) const {
    switch (cls) {
      case RegClass::GP:
      case RegClass::AnyNarrow:
      case RegClass::SIMD:
      case RegClass::SIMDWide:
        return regs.contains(color_reg(c));
      case RegClass::Spill:
        assertx(is_color_spill_slot(c));
        return true;
      case RegClass::SpillWide:
        assertx(is_color_spill_slot_wide(c));
        return true;
      case RegClass::Any:
      case RegClass::SF:
        break;
    }
    always_assert(false);
  }

  // Express a move between two physical registers. The destination
  // should not be assigned a Vreg, while the source should be. The
  // Vreg will now be assigned the destination register.
  void move(PhysReg src, PhysReg dst) {
    assertx(src != dst);
    assertx(!regs.contains(src));
    assertx(regs.contains(dst));
    auto const v = occupied[src];
    assertx(v.isValid());
    assertx(!occupied[dst].isValid());
    occupied[dst] = v;
    occupied[src] = Vreg{};
    regs -= dst;
    regs |= src;
  }

  // Return all Vregs which are currently assigned a physical register
  // (so ignoring spills).
  VregSet live() const {
    VregSet out;
    for (auto const r : occupied) {
      if (!occupied[r].isValid()) continue;
      assertx(!regs.contains(r));
      out.add(occupied[r]);
    }
    return out;
  }

  // Return the Vreg that a physical register is currently assigned
  // to. The physical register should be currently used.
  Vreg assignedTo(PhysReg r) const {
     assertx(!regs.contains(r));
     assertx(occupied[r].isValid());
     return occupied[r];
  }

  // Return physical registers not currently allocated.
  const RegSet& freeRegs() const { return regs; }

  std::string toString() const {
    auto str = fmt::format("Free: {}, Occupied: [", show(regs));
    auto first = true;
    for (auto const r : occupied) {
      if (!occupied[r].isValid()) continue;
      str += fmt::format(
        "{}{} -> {}",
        first ? "" : ", ",
        show(r),
        show(occupied[r])
      );
      first = false;
    }
    str += "]";
    return str;
  }

private:
  State* state;
  RegSet regs;
  PhysReg::Map<Vreg> occupied;
};

// Helper function to assert that a Color is not None (which means we couldn't
// find a free one, which is a bug).
void assert_found_color(Vreg r, Color c) {
  always_assert_flog(
    !is_color_none(c),
    "Unable to find free color for {}",
    show(r)
  );
}

// Release the allocated colors from any Vregs (from the given
// candidate set) which are dead at the given position, or in the
// optional kills set. If "released" is provided, then fill it with
// the physical registers freed.
void release_dead_regs(State& state,
                       FreeRegs& free,
                       const VregSet& candidates,
                       const VregSet* kills,
                       Vlabel block,
                       size_t instIdx,
                       RegSet* released = nullptr) {
  for (auto const r : candidates) {
    auto const info = reg_color_info(state, r);
    if (!is_colorable(info.regClass)) continue;
    // Since we walk the unit in a dominance preserving order, every Vreg
    // defined at this point should have a color assigned.
    assertx(!is_color_none(info.color));
    assertx(is_spill(info.regClass) ||
            !free.available(info.color, info.regClass));
    if (live_in_at(state, r, block, instIdx + 1) &&
        (!kills || !r.isPhys() || !(*kills)[r])) {
      continue;
    }
    if (released && is_color_reg(info.color)) {
      *released |= color_reg(info.color);
    }
    free.release(info.color, info.regClass);
    if (debug && !is_spill(info.regClass) && !r.isPhys()) {
      // In debug builds set the color back to None so we'll catch
      // errors more quickly.
      reg_info(state, r).color = Color{};
    }
  }
}

// Whether two physical registers can be exchanged with a single
// instruction.
bool can_single_swap(PhysReg r1, PhysReg r2) {
  return ARCH_MATCH(
    [&](arch::X64) { return r1.isGP() && r2.isGP(); },
    [](arch::ARM) { return false; }
  );
}

// Run the coloring logic for a single instruction. Returns the number
// of instructions added.
size_t color_inst(State& state,
                  FreeRegs& free,
                  Vinstr& inst,
                  Vlabel block,
                  size_t instIdx,
                  BlockSet& processed,
                  const jit::vector<BlockSet>& phiAdjustments) {
  auto const& acrosses = acrosses_set_cached(state, inst);
  auto const& uses = uses_set_cached(state, inst) - acrosses;
  auto const& defs = defs_set_cached(state, inst);

  // Phidefs should be done as part of block initialization.
  assertx(inst.op != Vinstr::phidef);

  auto& unit = state.unit;

  auto const weight = block_weight(state, block);

  // If this is a phijmp, we're at the end of the block. If our
  // successor (there should only be one) is already processed, it
  // means we've biased the penalty vectors for the phijmp sources (to
  // try to make their assignments match the ones in the
  // successor). Since we're done with this block, we need to undo
  // those penalty vector adjustments now. We use the phi adjustment
  // metadata to know which ones to undo (it's hard to determine
  // otherwise).
  if (inst.op == Vinstr::phijmp) {
    assertx(defs.none());
    assertx(instIdx == unit.blocks[block].code.size() - 1);
    auto const successors = succs(unit.blocks[block]);
    assertx(successors.size() == 1);
    auto const successor = successors[0];
    if (processed[successor]) {
      auto const& phidef = unit.blocks[successor].code.front();
      assertx(phidef.op == Vinstr::phidef);
      auto const& uses = unit.tuples[inst.phijmp_.uses];
      auto const& defs = unit.tuples[phidef.phidef_.defs];
      assertx(uses.size() == defs.size());

      auto const& adjustments = phiAdjustments[block];
      assertx(adjustments.empty() || adjustments.size() == uses.size());

      // If adjustments[i] is true, we it means we adjusted the
      // penalty vector for the Vreg at uses[i] by the weight of this
      // block towards defs[i]. Undo this.
      for (size_t i = 0; i < adjustments.size(); ++i) {
        if (!adjustments[i]) continue;
        assertx(!is_ignored(state, defs[i]));
        assertx(!is_ignored(state, uses[i]));
        assertx(defs[i].isPhys());
        assertx(!uses[i].isPhys());

        auto& info = reg_info(state, uses[i]);
        assertx(is_colorable_reg(info.regClass));
        auto const idx = info.penaltyIdx;
        assertx(idx > 0 && idx < state.penalties.size());
        auto& v = state.penalties[idx];

        auto const phys = defs[i].physReg();
        for (auto const r : v) {
          if (r != phys) {
            assertx(v[r] >= weight);
            v[r] -= weight;
          }
        }
      }
    }
  }
  // For a phijmp, most of the below logic will be skipped (because it
  // has no defs).

  // If this instruction has a hint from Vreg sources to physical
  // registers, we'll have incorporated this fact into the penalty
  // vectors of those Vregs (bias it towards that physical
  // register). We want to undo these adjustments, but we have to do
  // it after selection below. Record the adjustments to be undone
  // afterwards.
  struct Adjustment {
    Adjustment(PenaltyVector* p, PhysReg s) : penalties{p}, skip{s} {}
    PenaltyVector* penalties;
    PhysReg skip;
  };
  jit::vector<Adjustment> penaltyAdjustments;
  if (defs.containsPhys()) {
    VregSet hints;
    visitDefsWithHints(
      unit, inst,
      [&] (Vreg def, Vreg hint) {
        // This matches the logic for biasing or not when we processed
        // the phidef.
        if (!hint.isValid() || !def.isPhys() || hint.isPhys() || hints[hint]) {
          return;
        }
        hints.add(hint);

        auto const& info = reg_info(state, hint);
        if (!is_colorable_reg(info.regClass)) return;

        auto const cls1 = reg_class(state, hint);
        auto const cls2 = reg_class(state, def);
        if (!compatible_reg_classes(cls1, cls2)) return;

        if (acrosses[hint] || live_in_at(state, hint, block, instIdx + 1)) {
          return;
        }

        auto const idx = info.penaltyIdx;
        assertx(idx > 0 && idx < state.penalties.size());
        penaltyAdjustments.emplace_back(&state.penalties[idx], def.physReg());
      }
    );
  }

  // Rewrite the sources of this instruction to become the physical
  // registers the Vregs are already assigned to. Record the physical
  // registers corresponding to "across" Vregs.
  RegSet acrossPhys;
  visitRegsMutable(
    unit,
    inst,
    [&] (Vreg r) -> Vreg {
      if (r.isPhys()) return r;
      auto const& info = reg_info(state, r);
      if (is_spill(info.regClass)) return r;
      if (info.regClass == RegClass::SF) return state.abi.sf.choose();
      auto const c = color_reg(info.color);
      assertx(c != state.scratch);
      if (acrosses[r]) acrossPhys |= c;
      return c;
    },
    [] (Vreg r) { return r; }
  );

  // Release any colors held by now dead uses. This doesn't include
  // acrosses, which we removed from the uses set. Record those
  // physical registers which were released.
  RegSet usesNowDead;
  release_dead_regs(state, free, uses, &defs, block, instIdx, &usesNowDead);

  // Snapshot the free physical registers before we begin coloring.
  auto const freeRegsBeforeDefs = free.freeRegs();

  // The sources (and thus all the hints) of this instruction have
  // been rewritten to be physical registers. If the corresponding def
  // is not a physical register, we want to bias that def's penalty
  // vector towards the hinted source (to satisfy the hint). Do that
  // here, and record the adjustment to be undone afterwards.
  visitDefsWithHints(
    unit, inst,
    [&] (Vreg def, Vreg hint) {
      if (!hint.isValid() || !hint.isPhys() || def.isPhys()) return;

      // If the hint isn't free at the def, it's not satisfiable.
      auto const phys = hint.physReg();
      if (!freeRegsBeforeDefs.contains(phys)) return;

      auto const& info = reg_info(state, def);
      if (!is_colorable_reg(info.regClass)) return;

      assertx(info.penaltyIdx > 0 && info.penaltyIdx < state.penalties.size());
      auto& v = state.penalties[info.penaltyIdx];
      for (auto const r : v) {
        if (r != phys) v[r] += weight;
      }
      penaltyAdjustments.emplace_back(&v, phys);
    }
  );

  // Perform the actual physical register selection. For Vregs
  // representing non-physical registers, we try to find the best
  // physical register for it, possibly moving another Vreg. For Vregs
  // representing physical registers, we don't have any choice in the
  // matter, but we still might have to migrate a Vreg out of the
  // desired physical register.
  VregList copySrcs;
  VregList copyDsts;
  for (auto const r : defs) {
    auto const cls = reg_class(state, r);
    if (!is_colorable(cls)) continue;

    auto info = r.isPhys() ? nullptr : &reg_info(state, r);

    // This Vreg is being defined here, so it better not have a
    // color already.
    assertx(!info || is_color_none(info->color));

    // Pick a color, assert we found something (which we always
    // should), handle any possible moves, and then reserve it. If the
    // Vreg is a physical register, it will always return itself, but
    // will move a Vreg if it resides in the destination.
    auto const coloring = free.choose(
      r,
      [&] (Vreg from, Vreg to) -> int64_t {
        // Move weight calculation. This matches the logic below for
        // determining whether a copy is actually needed.
        if (freeRegsBeforeDefs.contains(from)) return 0;
        auto const it = std::find(copySrcs.begin(), copySrcs.end(), from);
        if (it != copySrcs.end()) return 0;
        if (!usesNowDead.contains(to)) return weight;
        // If we can't emit a swap, assume it will take three
        // instructions.
        return can_single_swap(from, to) ? weight : (weight * 3);
      }
    );
    assert_found_color(r, coloring.color);

    // Some other Vreg has to be moved to make way for this physical
    // register.
    if (coloring.moveDest != InvalidReg) {
      // Update metadata to track the move.
      auto const movedFrom = color_reg(coloring.color);
      auto const vreg = free.assignedTo(movedFrom);
      free.move(movedFrom, coloring.moveDest);
      reg_info(state, vreg).color = coloring.moveDest;

      // Add to the copy list so we'll generate a copy before this
      // instruction. If the copy source is a destination of another
      // copy, just update the original copy. If the copy source is
      // dead when the instruction executes, no copy is actually
      // needed. Finally, if the copy source is already being moved to
      // another register, no copy is needed (it's already being
      // saved).
      std::replace(
        copyDsts.begin(), copyDsts.end(), movedFrom, coloring.moveDest
      );
      if (!freeRegsBeforeDefs.contains(movedFrom)) {
        if (std::find(copySrcs.begin(), copySrcs.end(), movedFrom) ==
            copySrcs.end()) {
          copySrcs.emplace_back(movedFrom);
          copyDsts.emplace_back(coloring.moveDest);
        }
      }
    }

    if (info) info->color = coloring.color;
    free.reserve(r, coloring.color, cls);
  }

  // Undo all the penalty adjustments we did before coloring.
  for (auto const p : penaltyAdjustments) {
    auto& v = *p.penalties;
    for (auto const r : v) {
      if (r == p.skip) continue;
      assertx(v[r] >= weight);
      v[r] -= weight;
    }
  }

  // Rewrite the sources and the defs of this instruction to take into
  // account the physical register selections and any moves we need to
  // generate.
  visitRegsMutable(
    unit,
    inst,
    [&] (Vreg r) -> Vreg {
      assertx(r.isPhys() || is_spill(reg_class(state, r)));
      if (inst.op == Vinstr::copy || inst.op == Vinstr::copyargs) {
        // If the current instruction is a copy, we'll fold the new
        // copies into it below, so nothing needs to be changed.
        return r;
      }
      if (r.isPhys()) {
        // Rewrite already colored source to take into account moves
        // we need to generate.
        auto const phys = r.physReg();
        assertx(copySrcs.size() == copyDsts.size());
        for (size_t i = 0; i < copyDsts.size(); ++i) {
          if (!usesNowDead.contains(copyDsts[i]) &&
              !acrossPhys.contains(copySrcs[i])) {
            // If the copy destination isn't used by the instruction,
            // and the copy source isn't across, then the source and
            // destination will contain the same value when the
            // instruction executes, so we don't actually have to
            // change anything.
            continue;
          }
          // Otherwise rewrite as appropriate. We need to check both
          // copySrcs and copyDsts because we might be swapping them
          // and both can appear as uses in the current instruction.
          if (copyDsts[i] == phys) return copySrcs[i];
          if (copySrcs[i] == phys) return copyDsts[i];
        }
      }
      return r;
    },
    [&] (Vreg r) -> Vreg {
      if (r.isPhys()) return r;
      auto const& info = reg_info(state, r);
      if (is_spill(info.regClass)) return r;
      if (info.regClass == RegClass::SF) return state.abi.sf.choose();
      auto const c = color_reg(info.color);
      assertx(c != state.scratch);
      return c;
    }
  );
  invalidate_cached_operands(inst);

  // If this instruction defined any physical registers, we'll have
  // incorporated this information into the initial penalty vectors
  // for any live Vreg. Since this instruction is now processed, we
  // want to undo this adjustment.
  if (defs.containsPhys()) {
    for (auto const r : free.live()) {
      // VregSet always provides the physical registers first, so if
      // we see a non-physical Vreg, we've seen all the physical
      // registers.
      if (r.isPhys()) continue;
      if (!is_colorable_reg(reg_class(state, r))) continue;
      auto const idx = reg_info(state, r).penaltyIdx;
      assertx(idx > 0 && idx < state.penalties.size());
      auto& v = state.penalties[idx];
      for (auto const d : defs) {
        if (!d.isPhys()) break;
        auto const phys = d.physReg();
        assertx(v[phys] >= weight);
        v[phys] -= weight;
      }
    }
  }

  // Release any now dead defs or acrosses.
  release_dead_regs(state, free, acrosses, nullptr, block, instIdx);
  release_dead_regs(state, free, defs, nullptr, block, instIdx);

  assertx(copySrcs.size() == copyDsts.size());
  if (copySrcs.empty()) return 0;

  // Emit any needed copies:

  // Special case. If the previous instruction to this one defines the
  // copy source, we possibly just modify it to write to the copy
  // destination instead.
  if (instIdx > 0) {
    auto& last = unit.blocks[block].code[instIdx - 1];
    // Rewriting phidefs is tricky because of the penalty vector
    // biasing, so skip for now.
    if (last.op != Vinstr::phidef) {
      auto const& lastDefs = defs_set_cached(state, last);
      auto const& lastAcrosses = acrosses_set_cached(state, last);
      size_t copyIdx = 0;
      for (size_t i = 0; i < copySrcs.size(); ++i) {
        // We can safely perform this optimization if the destination
        // is not used by the current instruction (can't clobber
        // otherwise), if the source is actually defined by the
        // previous instruction, if the destination isn't also defined
        // by the previous instruction, and if the destination isn't
        // an across operand for the previous instruction.
        if (!usesNowDead.contains(copyDsts[i]) &&
            lastDefs[copySrcs[i]] &&
            !lastDefs[copyDsts[i]] &&
            !lastAcrosses[copyDsts[i]]) {
          // Rewrite the previous instruction to write to the
          // destination.
          visitRegsMutable(
            unit, last,
            [&] (Vreg r) { return r; },
            [&] (Vreg r) -> Vreg {
              return (copySrcs[i] == r) ? copyDsts[i] : r;
            }
          );
          // And rewrite the current instruction to take the
          // destination as the source.
          visitRegsMutable(
            unit, inst,
            [&] (Vreg r) { return (copySrcs[i] == r) ? copyDsts[i] : r; },
            [&] (Vreg r) { return r; }
          );
        } else {
          // Otherwise, no rewrite, keep this pair in the list.
          copySrcs[copyIdx] = copySrcs[i];
          copyDsts[copyIdx] = copyDsts[i];
          ++copyIdx;
        }
      }
      // Did we shrink the copy list (and hence changed something)?
      if (copyIdx < copySrcs.size()) {
        invalidate_cached_operands(last);
        invalidate_cached_operands(inst);
      }
      // If we eliminated all copies, we're done
      if (copyIdx == 0) return 0;
      copySrcs.resize(copyIdx);
      copyDsts.resize(copyIdx);
    }
  }

  // More special cases: If this instruction is a copy, or copyargs,
  // we can fold the copies into the current instruction (this works
  // because copyargs is a parallel copy).
  if (inst.op == Vinstr::copy) {
    copySrcs.emplace_back(inst.copy_.s);
    copyDsts.emplace_back(inst.copy_.d);
    inst.op = Vinstr::copyargs;
    inst.copyargs_.s = unit.makeTuple(std::move(copySrcs));
    inst.copyargs_.d = unit.makeTuple(std::move(copyDsts));
    invalidate_cached_operands(inst);
    return 0;
  } else if (inst.op == Vinstr::copyargs) {
    auto& srcs = unit.tuples[inst.copyargs_.s];
    auto& dsts = unit.tuples[inst.copyargs_.d];
    for (size_t i = 0; i < copySrcs.size(); ++i) {
      srcs.emplace_back(copySrcs[i]);
      dsts.emplace_back(copyDsts[i]);
    }
    invalidate_cached_operands(inst);
    return 0;
  }

  // If the destination of the copy is used as a source of this
  // instruction, we can't just emit a copy overwriting it before the
  // instruction. Instead we swap the source and destination. This is
  // fine because the destination is guaranteed to be dead before the
  // instruction writes to its defs. We do this by inserting an
  // opposite order of the copy in the list, which the phi lowering
  // logic will turn into a swap.
  auto const origSize = copySrcs.size();
  for (size_t i = 0; i < origSize; ++i) {
    if (!usesNowDead.contains(copyDsts[i])) continue;
    auto const src = copySrcs[i];
    auto const dst = copyDsts[i];
    copySrcs.emplace_back(dst);
    copyDsts.emplace_back(src);
  }

  // Emit the copies
  vmodify(
    unit, block, instIdx,
    [&] (Vout& v) {
      if (copySrcs.size() == 1) {
        v << copy{copySrcs[0], copyDsts[0]};
      } else {
        v << copyargs{
          unit.makeTuple(std::move(copySrcs)),
          unit.makeTuple(std::move(copyDsts))
        };
      }
      return 0;
    }
  );

  return 1;
}

// Flat mapping of Vreg to a physical register it's been assigned
// to. This is used to record Vreg to physical register assignments at
// block entry/exit. The block will have an entry for each
// live-in/live-out Vreg (in that order), except for Vregs
// representing physical registers. Registers will always be present
// in ascending order.
using AssignmentVector = jit::vector<std::pair<Vreg, PhysReg>>;

// Initialize the initial set of Vreg to physical register assignments
// for a block, taking into account predecessors. If the block has one
// predecessor, we just copy over the assignments from that. If it has
// multiple predecessors, we invoke the register selection logic, but
// biasing (via the penalty vectors) towards assignments which match
// the predecessors. In extreme cases, we may choose a register that
// none of the predecessors selected. This all means the block can
// have register assignments which don't match their predecessors or
// successors, but that will be resolved afterwards.
size_t color_block_initialize(
    State& state,
    FreeRegs& free,
    Vlabel b,
    const jit::vector<AssignmentVector>& assignments,
    const BlockSet& processed,
    jit::vector<boost::dynamic_bitset<>>& phiAdjustments
) {
  auto const reserve = [&] (Vreg r, PhysReg phys) {
    auto const cls = reg_class(state, phys);
    always_assert(free.available(phys, cls));
    free.reserve(r, phys, cls);
    if (r.isPhys()) return;
    auto& info = reg_info(state, r);
    assertx(is_color_none(info.color));
    info.color = phys;
  };

  auto const& preds = state.preds[b];
  if (preds.empty()) {
    assertx(b == state.unit.entry);
    assertx(state.liveIn[b].allPhys());
    for (auto const r : state.liveIn[b]) reserve(r, r.physReg());
    return 0;
  }

  if (preds.size() == 1) {
    // If this block has exactly one predecessor, it must already be
    // processed (dominance preserving order guarantees this). We can
    // simply use the same assignments the predecessor used for Vregs.
    assertx(processed[preds[0]]);

    auto const& predOut = assignments[preds[0]];
    size_t predOutIdx = 0;
    for (auto const r : state.liveIn[b]) {
      auto const cls = reg_class(state, r);
      if (!is_colorable_reg(cls)) continue;

      if (r.isPhys()) {
        reserve(r, r.physReg());
        continue;
      }

      // The predecessor can have a Vreg which is live-out (and thus
      // in the assignment vector), but isn't live-in to this
      // block. Skip over such Vregs. This is only possible if the
      // predecessor has multiple successors.
      assertx(predOutIdx < predOut.size());
      while (r != predOut[predOutIdx].first) {
        ++predOutIdx;
        assertx(predOutIdx < predOut.size());
      }

      reserve(r, predOut[predOutIdx].second);
    }

    // If the block doesn't begin with a phidef, we're done.
    auto& inst = state.unit.blocks[b].code.front();
    if (inst.op != Vinstr::phidef) return 0;

    auto const phijmp = state.unit.blocks[preds[0]].code.back();
    assertx(phijmp.op == Vinstr::phijmp);
    auto const& uses = state.unit.tuples[phijmp.phijmp_.uses];
    auto& defs = state.unit.tuples[inst.phidef_.defs];
    assertx(uses.size() == defs.size());

    // The uses of the associated phijmp should already be colored, so
    // just use that to initialize the phidef defs.
    auto const& defsSet = defs_set_cached(state, inst);
    for (size_t i = 0; i < uses.size(); ++i) {
      if (is_ignored(state, defs[i])) {
        assertx(is_ignored(state, uses[i]));
        continue;
      }
      assertx(!defs[i].isPhys());
      auto const& info = reg_info(state, defs[i]);
      if (is_spill(info.regClass)) continue;
      if (info.regClass == RegClass::SF) {
        defs[i] = state.abi.sf.choose();
        continue;
      }
      // Rewrite the instruction to take the physical registers.
      assertx(uses[i].isPhys());
      auto const phys = uses[i].physReg();
      assertx(phys != state.scratch);
      reserve(defs[i], phys);
      defs[i] = phys;
    }
    invalidate_cached_operands(inst);
    // The phidef can define registers which are immediately dead, so
    // release those here.
    release_dead_regs(state, free, defsSet, nullptr, b, 0);
  }

  // Since we've split critical edges, if we have multiple
  // predecessors, all those predecessors must have one successor
  // (this block), and therefore the live-out information should
  // always match the live-in.
  if (debug) {
    for (auto const pred : preds) {
      always_assert(state.liveOut[pred] == state.liveIn[b]);
    }
  }

  // We have multiple predecessors. The different predecessors might
  // have different Vreg to physical register assignments. So, instead
  // of just taking it from one predecessor or another, instead we
  // invoke the full "choose" logic for each Vreg, which will attempt
  // to chose the physical register which is optimal (according to
  // penalty vectors). However, we bias the penalty vectors by what
  // the predecessors have already chosen, to reflect that certain
  // choices will avoid copies on the edge. Since these biases are
  // only for this one instruction, we need to undo the adjustments
  // after selection.

  struct Adjustment {
    Adjustment(PenaltyVector* p, PhysReg s, int64_t a)
      : penalties{p}, skip{s}, amount{a} {}
    PenaltyVector* penalties;
    PhysReg skip;
    int64_t amount;
  };
  jit::vector<Adjustment> penaltyAdjustments;

  size_t assignmentIdx = 0;
  for (auto const r : state.liveIn[b]) {
    if (r.isPhys()) continue;

    auto& info = reg_info(state, r);
    if (!is_colorable_reg(info.regClass)) continue;

    assertx(info.penaltyIdx > 0 && info.penaltyIdx < state.penalties.size());
    auto& penalties = state.penalties[info.penaltyIdx];

    // Bias the penalty vector for this Vreg by what processed
    // predecessors have already selected for it. Record the
    // adjustment so we can undo it.
    for (auto const pred : preds) {
      if (!processed[pred]) continue;

      auto const& predAssignment = assignments[pred];
      assertx(assignmentIdx < predAssignment.size());
      assertx(predAssignment[assignmentIdx].first == r);

      // The copy will be done on the predecessor, so that's the
      // weight to use.
      auto const weight = block_weight(state, pred);

      for (auto const phys : penalties) {
        if (phys == predAssignment[assignmentIdx].second) continue;
        penalties[phys] += weight;
      }
      penaltyAdjustments.emplace_back(
        &penalties,
        predAssignment[assignmentIdx].second,
        weight
      );
    }

    ++assignmentIdx;
  }

  // Same as above, but for a phidef/phijmp pair, if any.
  auto& phidef = state.unit.blocks[b].code.front();
  if (phidef.op == Vinstr::phidef) {
    auto const& defs = state.unit.tuples[phidef.phidef_.defs];
    for (size_t i = 0; i < defs.size(); ++i) {
      if (is_ignored(state, defs[i])) continue;
      assertx(!defs[i].isPhys());
      auto& info = reg_info(state, defs[i]);
      if (!is_colorable_reg(info.regClass)) continue;

      assertx(info.penaltyIdx > 0 && info.penaltyIdx < state.penalties.size());
      auto& penalties = state.penalties[info.penaltyIdx];

      for (auto const pred : preds) {
        auto const phijmp = state.unit.blocks[pred].code.back();
        assertx(phijmp.op == Vinstr::phijmp);
        auto const& uses = state.unit.tuples[phijmp.phijmp_.uses];
        assertx(uses.size() == defs.size());

        if (!processed[pred]) continue;

        assertx(uses[i].isPhys());
        assertx(!is_ignored(state, uses[i]));
        auto const physUse = uses[i].physReg();
        auto const weight = block_weight(state, pred);
        for (auto const phys : penalties) {
          if (phys == physUse) continue;
          penalties[phys] += weight;
        }
        penaltyAdjustments.emplace_back(&penalties, physUse, weight);
      }
    }
  }

  // Now that we've biased things appropriately, do the actual
  // selection.
  for (auto const r : state.liveIn[b]) {
    // NB: Physical register always come before virtual ones in a
    // VregSet, so we'll reserve all the physical registers first.
    if (r.isPhys()) {
      reserve(r, r.physReg());
      continue;
    }

    auto& info = reg_info(state, r);
    if (!is_colorable_reg(info.regClass)) continue;

    // Choose a physical register for this Vreg. "Moving" here is
    // free, because it just results in a change to a previous
    // assignment.
    auto const coloring = free.choose(r, [] (Vreg, Vreg) { return 0; });
    assert_found_color(r, coloring.color);

    auto const selected = color_reg(coloring.color);
    if (coloring.moveDest != InvalidReg) {
      auto const vreg = free.assignedTo(selected);
      free.move(selected, coloring.moveDest);
      reg_info(state, vreg).color = coloring.moveDest;
    }

    reserve(r, selected);
  }

  // Do the same, but for phidef/phijmp pair if any.
  if (phidef.op == Vinstr::phidef) {
    auto const& defs = state.unit.tuples[phidef.phidef_.defs];
    for (size_t i = 0; i < defs.size(); ++i) {
      if (is_ignored(state, defs[i])) continue;
      assertx(!defs[i].isPhys());
      auto& info = reg_info(state, defs[i]);
      if (!is_colorable(info.regClass)) continue;
      assertx(is_color_none(info.color));

      auto const coloring = free.choose(defs[i], [] (Vreg, Vreg) { return 0; });
      assert_found_color(defs[i], coloring.color);
      info.color = coloring.color;

      if (coloring.moveDest != InvalidReg) {
        auto const selected = color_reg(info.color);
        auto const vreg = free.assignedTo(selected);
        free.move(selected, coloring.moveDest);
        reg_info(state, vreg).color = coloring.moveDest;
      }

      // We don't use reserve() here because defs[i] might be a spill.
      free.reserve(defs[i], info.color, info.regClass);
    }
  }

  // Now that we've selected registers for all the Vregs, we need to
  // bias any unprocessed predecessors. That is, we want to make it
  // more likely that when coloring the predecessor, we select
  // registers for the same Vregs there, that match our selections
  // here. This will eliminate any necessary copies between the
  // blocks. Assess a penalty for each Vreg to every physical register
  // which isn't the one we selected. The weight of the penalty is the
  // weight of the predecessor. These adjustments will be undone when
  // the predecessor is colored.
  for (auto const r : state.liveIn[b]) {
    if (r.isPhys()) continue;

    auto& info = reg_info(state, r);
    if (!is_colorable_reg(info.regClass)) continue;

    auto const selection = color_reg(info.color);
    for (auto const pred : preds) {
      if (processed[pred]) continue;

      assertx(info.penaltyIdx > 0 && info.penaltyIdx < state.penalties.size());
      auto& penalties = state.penalties[info.penaltyIdx];

      auto const weight = block_weight(state, pred);
      for (auto const phys : penalties) {
        if (phys == selection) continue;
        penalties[phys] += weight;
      }
    }
  }

  // Same idea as above, but for phijmp/phidef pairs. We need to
  // record which operands were adjusted, as it's difficult to
  // determine after the fact.
  if (phidef.op == Vinstr::phidef) {
    auto const& defs = state.unit.tuples[phidef.phidef_.defs];
    for (size_t i = 0; i < defs.size(); ++i) {
      if (is_ignored(state, defs[i])) continue;
      assertx(!defs[i].isPhys());
      auto& info = reg_info(state, defs[i]);
      if (!is_colorable_reg(info.regClass)) continue;

      auto const physDef = color_reg(info.color);
      for (auto const pred : preds) {
        if (processed[pred]) continue;
        auto const& phijmp = state.unit.blocks[pred].code.back();
        auto const& uses = state.unit.tuples[phijmp.phijmp_.uses];
        assertx(uses.size() == defs.size());
        assertx(!is_ignored(state, uses[i]));

        auto& adjustments = phiAdjustments[pred];
        assertx(adjustments.empty() || adjustments.size() == uses.size());
        if (adjustments.empty()) adjustments.resize(uses.size());

        auto const penaltyIdx = reg_info(state, uses[i]).penaltyIdx;
        assertx(penaltyIdx > 0 && penaltyIdx < state.penalties.size());
        // If the source and dest don't have the same penalty vector,
        // there's no benefit to assigning them the same physical
        // register (they probably interfere anyways), so skip the
        // adjustment.
        if (penaltyIdx != info.penaltyIdx) continue;

        adjustments[i] = true;

        auto& v = state.penalties[penaltyIdx];
        auto const weight = block_weight(state, pred);
        for (auto const r : v) {
          if (r != physDef) v[r] += weight;
        }
      }
    }
  }

  // Undo the penalty vector adjustments we did early before the
  // selections were made.
  for (auto const p : penaltyAdjustments) {
    auto& penalties = *p.penalties;
    for (auto const r : penalties) {
      if (r == p.skip) continue;
      assertx(penalties[r] >= p.amount);
      penalties[r] -= p.amount;
    }
  }

  // Rewrite the phidef (if present) to take the physical registers
  // selected for it.
  if (phidef.op == Vinstr::phidef) {
    // Capture this before rewriting the operands.
    auto const& defsSet = defs_set_cached(state, phidef);
    auto& defs = state.unit.tuples[phidef.phidef_.defs];
    for (size_t i = 0; i < defs.size(); ++i) {
      if (is_ignored(state, defs[i])) continue;
      assertx(!defs[i].isPhys());
      auto const& info = reg_info(state, defs[i]);
      if (is_spill(info.regClass)) continue;
      if (info.regClass == RegClass::SF) {
        defs[i] = state.abi.sf.choose();
        continue;
      }
      defs[i] = color_reg(info.color);
      assertx(defs[i] != state.scratch);
    }
    invalidate_cached_operands(phidef);
    // The phidef can define registers which are immediately dead, so
    // release those here.
    release_dead_regs(state, free, defsSet, nullptr, b, 0);
    return 1;
  }

  return 0;
}

void assign_colors(State& state) {
  // Calculate (initial) penalty vectors and find a good coloring
  // order.
  calculate_penalties(state);
  auto const order = calculate_block_color_order(state);

  auto& unit = state.unit;

  // Vreg to physical register assignments to the entry and exit of
  // blocks. Used to initialize assignments when starting a block, and
  // for resolving mismatches between blocks.
  jit::vector<AssignmentVector> assignmentsIn;
  jit::vector<AssignmentVector> assignmentsOut;
  assignmentsIn.resize(unit.blocks.size());
  assignmentsOut.resize(unit.blocks.size());

  // When we color a phidef, not all of the phijmp blocks may have
  // been colored already. We adjust the penalty vectors for those
  // uncolored phijmp sources to express a preference for the phidef
  // def assigned registers. However, not all Vregs may be adjusted
  // (because interference may mean we don't try to satisfy the
  // hint). It's difficult to back out what got adjusted and what
  // didn't, so explicitly encode it in this data-structure. The
  // vector is keyed by block, and each bit in the bitset represents
  // an index into the phijmp source.
  jit::vector<boost::dynamic_bitset<>> phiAdjustments;
  phiAdjustments.resize(unit.blocks.size());

  // Populate register assignments from a set of live Vregs.
  auto const liveToAssignment = [&] (const VregSet& live,
                                     AssignmentVector& assignment) {
    assignment.reserve(live.size());
    for (auto const r : live) {
      if (r.isPhys()) continue;
      auto const& info = reg_info(state, r);
      if (!is_colorable_reg(info.regClass)) continue;
      assertx(!is_color_none(info.color));
      assignment.emplace_back(r, color_reg(info.color));
    }
  };

  BlockSet processed(state.unit.blocks.size());

  // Since the block order is dominance preserving, we'll always encouter a
  // Vreg's def before any of its usages. This means we can color in a single
  // pass over the unit.
  for (auto const b : order) {
    assertx(!processed[b]);

    // Initialize the initial assignments according to the information
    // from predecessors and phidefs.
    FreeRegs free{state};
    SCOPE_ASSERT_DETAIL("Colorer State") { return free.toString(); };
    auto const startIdx = color_block_initialize(
      state, free, b, assignmentsOut,
      processed, phiAdjustments
    );

    // Record the initial assignment for mismatch resolution. This is
    // only necessary if the block has multiple predecessors (if
    // there's only one predecessor, it must be already processed and
    // there can be no mismatches).
    if (state.preds[b].size() > 1) {
      liveToAssignment(state.liveIn[b], assignmentsIn[b]);
    } else if (state.preds[b].size() > 0) {
      assertx(processed[state.preds[b][0]]);
    }

    // Walk the instructions, running the coloring logic on them.
    for (size_t i = startIdx; i < unit.blocks[b].code.size(); ++i) {
      auto& inst = unit.blocks[b].code[i];
      // Make sure the uses of the instruction are all colored already (which
      // should be the case because we walk the unit in dominance preserving
      // order).
      if (debug) {
        for (auto const r : uses_set_cached(state, inst)) {
          if (r.isPhys()) continue;
          auto const& info = reg_info(state, r);
          if (!is_colorable(info.regClass)) continue;
          always_assert(!is_color_none(info.color));
        };
      }

      i += color_inst(state, free, inst, b, i, processed, phiAdjustments);
    }

    auto const successors = succs(unit.blocks[b]);
    if (!successors.empty()) {
      // If this block has one successor, but that successor is
      // already processed, it means that successor has multiple
      // predecessors (because we split critical edges), and one of
      // them has a higher weight than us.  When the predecessor was
      // colored, we modified the penalty vectors for live-in Vregs to
      // reflect a preference for whatever they were colored to. Since
      // we've now processed this block, we must now undo these
      // preferences.
      if (successors.size() == 1 && processed[successors[0]]) {
        auto const weight = block_weight(state, b);

        for (auto const& assignment : assignmentsIn[successors[0]]) {
          auto& info = reg_info(state, assignment.first);
          assertx(is_colorable_reg(info.regClass));

          auto const penaltyIdx = info.penaltyIdx;
          assertx(penaltyIdx > 0 && penaltyIdx < state.penalties.size());
          auto& v = state.penalties[penaltyIdx];

          // Undo the preference (really a penalty for everything else
          // except one Vreg).
          for (auto const r : v) {
            if (r != assignment.second) {
              assertx(v[r] >= weight);
              v[r] -= weight;
            }
          }
        }
      }

      // Record assignments at the end of the block for mismatch
      // resolution.
      liveToAssignment(state.liveOut[b], assignmentsOut[b]);
    }

    processed[b] = true;

    // Clear color information from the Vregs in debug builds, to
    // catch uses before assignment.
    if (debug) {
      for (auto const r : state.liveOut[b]) {
        if (r.isPhys()) continue;
        auto& info = reg_info(state, r);
        if (!is_spill(info.regClass)) info.color = Color{};
      }
    }
  }

  // At this point all penalties should be zero, as we should have
  // undone every assessment as we colored the unit.
  if (debug) {
    for (auto const& v : state.penalties) {
      for (auto const r : v) always_assert(v[r] == 0);
    }
  }

  // Now compare the assignment information between predecessors and
  // successors and look for mismatches. This means the blocks made
  // different coloring decisions. If so, we need to insert phis
  // between the blocks to make everything consistent.
  for (auto const b : state.rpo) {
    // If a block only has one predecessor, it should always be
    // consistent, so we can skip.
    auto const& preds = state.preds[b];
    if (preds.size() <= 1) continue;

    // Since we've split critical edges, a block with multiple
    // predecessors should have the same live-in Vregs as all the
    // predecessors' live-out.
    if (debug) {
      for (auto const pred : preds) {
        always_assert(state.liveOut[pred] == state.liveIn[b]);
      }
    }

    auto const& in = assignmentsIn[b];

    VregSet conflicts;
    for (size_t i = 0; i < in.size(); ++i) {
      assertx(!in[i].first.isPhys());
      auto const virt = in[i].first;
      auto const phys = in[i].second;
      for (auto const pred : preds) {
        assertx(assignmentsOut[pred].size() == in.size());
        assertx(assignmentsOut[pred][i].first == virt);
        if (assignmentsOut[pred][i].second != phys) conflicts.add(virt);
      }
    }
    // Hopefully the common case
    if (conflicts.none()) continue;

    // Either create a new phidef/phijmp or expand an existing one.
    auto const addToPhiDef = [&] (PhysReg reg) {
      auto& first = unit.blocks[b].code.front();
      if (first.op == Vinstr::phidef) {
        unit.tuples[first.phidef_.defs].push_back(reg);
        invalidate_cached_operands(first);
      } else {
        vmodify(
          unit, b, 0,
          [&] (Vout& v) { v << phidef{unit.makeTuple({reg})}; return 0; }
        );
      }
    };

    auto const addToPhiJmp = [&] (Vlabel pred, PhysReg reg) {
      auto& last = unit.blocks[pred].code.back();
      if (last.op == Vinstr::phijmp) {
        unit.tuples[last.phijmp_.uses].push_back(reg);
      } else {
        assertx(last.op == Vinstr::jmp);
        auto const jmp = last.jmp_;
        assertx(jmp.target == b);
        last.op = Vinstr::phijmp;
        last.phijmp_ = phijmp{jmp.target, unit.makeTuple({reg})};
      }
      invalidate_cached_operands(last);
    };

    // Resolve the mismatches with phis
    for (size_t i = 0; i < in.size(); ++i) {
      if (!conflicts[in[i].first]) continue;
      addToPhiDef(in[i].second);
      for (auto const pred : preds) {
        addToPhiJmp(pred, assignmentsOut[pred][i].second);
      }
    }
  }

  assertx(check(state.unit));
}

//////////////////////////////////////////////////////////////////////
// SSA lowering

/*
 * A move plan is a list of sequential moves or swaps needed to lower a set of
 * parallel moves. This is tricky because the original move might take a
 * register as both a source and a dest, and since the move is parallel, it must
 * read from the source before overwriting it.
 *
 * For example:
 *  copyargs r1, r2, r3 -> r2, r1, r4
 *
 * Becomes:
 *  Swap r1, r2
 *  Move r3, r4
 *
 * To build this, we build the transfer graph. This is a directed graph composed
 * of vertices representing registers. If there's a move from register R1 to
 * register R2, then there's an edge from the vertex representing R2 to the
 * vertex representing R1. Suppose we have an edge V1 -> V2 where V1 has an
 * in-degree of 0. V1 represents the register R3 and V2 represents R4. This
 * implies that R3 is not used as a source by any move. We emit a Move R4 -> R3
 * and remove V1 from the graph (and decrease any relevant in-degrees). This
 * process is repeated until there are no more vertices with in-degree of 0.
 *
 * At this point, the graph is now composed solely of loops. We walk each loop,
 * emitting swaps for adjacent vertices (until we get back to the beginning of
 * the loop).
 */

// This is a single component of a move plan. It's templatized to work with both
// physical registers or spill slots.
template <typename T>
struct MoveInfo {
  // Move: Move src -> dst
  // Xchg: Swap src <-> dst
  enum class Kind { Move, Xchg };
  Kind kind;
  T src;
  T dst;
};

// Generate a move plan from a set of copies from src to dest (specified
// backwards).
template <typename T>
jit::vector<MoveInfo<T>> make_move_plan(const jit::fast_map<T,T>& dstToSrc) {
  // Calculate in-degrees
  jit::fast_map<T, int> degree;
  jit::vector<std::pair<T,T>> dstToSrcVec;
  degree.reserve(dstToSrc.size());
  dstToSrcVec.reserve(dstToSrc.size());
  for (auto const& kv : dstToSrc) {
    dstToSrcVec.push_back(kv);
    auto const src = kv.second;
    assertx(kv.first != src);
    ++degree[src];
  }
  std::sort(dstToSrcVec.begin(), dstToSrcVec.end(),
            [](const std::pair<T,T>& a, const std::pair<T,T>& b) {
              return (size_t)Vreg{a.first} < (size_t)Vreg{b.first};
            });

  // Compute the list of non-swap moves. Keep removing nodes from the graph that
  // have an in-degree of 0 (by setting their degree to -1, which makes it as
  // processed). Emit a move for each one.
  auto moveInfo = [&]{
    jit::vector<MoveInfo<T>> info;

    for (auto const& kv : dstToSrcVec) {
      auto dst = kv.first;
      auto src = kv.second;

      // Check if it's either already processed (-1) or has an incoming edge (>
      // 0).
      if (degree[dst] != 0) continue;
      // It has no incoming edges, so we can remove it. Mark it as processed.
      degree[dst] = -1;

      // Walk the dst -> src chain as long as we keep lowering in-degrees to
      // zero.
      while (true) {
        info.push_back({MoveInfo<T>::Kind::Move, src, dst});
        // Remove the edge from dst to src and decrease the in-degree. If it's
        // now zero, follow the chain and repeat the process.
        assertx(degree[src] > 0);
        if (--degree[src] != 0) break;
        degree[src] = -1;

        auto const it = dstToSrc.find(src);
        if (it == dstToSrc.end()) break;
        dst = src;
        src = it->second;
      }
    }

    return info;
  }();

  // At this point the transfer graph is nothing but loops. We can walk each
  // loop, emitting swaps for adjacent nodes.

  for (auto const& kv : dstToSrcVec) {
    auto const begin = kv.first;

    // Ignore nodes already processed above.
    if (degree[begin] < 0) continue;
    // We shouldn't have any zero nodes at this point.
    assertx(degree[begin] > 0);
    degree[begin] = -1;

    // Walk the loop
    auto prev = begin;
    auto current = kv.second;
    while (current != begin) {
      assertx(degree[current] > 0);
      degree[current] = -1;
      moveInfo.push_back({MoveInfo<T>::Kind::Xchg, prev, current});
      prev = current;
      auto const it = dstToSrc.find(current);
      assertx(it != dstToSrc.end());
      current = it->second;
    }
  }

  return moveInfo;
}

// Lower the instruction at the given position down to a set of copy and copy2
// (swap) instructions. The copies may be optimized away if their source(s) and
// dests(s) are the same register. Remove the input instruction and replace it
// with the new instructions. This assumes that the instructions have already
// been rewritten to take physical registers.
void lower_copies(const State& state,
                  Vlabel block,
                  size_t instIdx) {
  // Map of individual physical register moves (in reverse order, dest to src).
  jit::fast_map<PhysReg, PhysReg> regDstToSrc;
  // Map of individual spill slot moves (in reverse order, dest to src).
  jit::fast_map<size_t, size_t> spillDstToSrc;
  // Map of spill slot to the Vreg representing it in a src.
  jit::fast_map<size_t, Vreg> spillSrcSlotToReg;
  // Map of spill slot to the Vreg representing it in a dest.
  jit::fast_map<size_t, Vreg> spillDstSlotToReg;
  // If this is a phijmp, the target of the jmp.
  Vlabel phiTarget;

  auto const addSrcDstPair = [&] (Vreg src, Vreg dst) {
    // We've already converted Vregs to physical registers everywhere except
    // with Vregs representing spill slots. Therefore the only way the source
    // and dest can vary with regards to physical-ness is if we're copying
    // to/from a spill slot to a non-spill slot, which isn't allowed. Only spill
    // and reload instructions can move values to/from physical registers
    // to/from spill slots.
    assertx(src.isPhys() == dst.isPhys());

    if (src.isPhys()) {
      // Reg to reg copy. If the source and dest is the same, the copy is
      // trivially a no-op, so ignore it.
      if (src == dst) return;
      // Otherwise record the copy (backwards).
      auto const DEBUG_ONLY result =
        regDstToSrc.emplace(dst.physReg(), src.physReg());
      assertx(result.second);
      return;
    }

    // Spill to spill copy.
    auto const sInfo = reg_info(state, src);
    auto const dInfo = reg_info(state, dst);
    assertx(is_spill(sInfo.regClass));
    assertx(is_spill(dInfo.regClass));

    // Lookup the colored slot and record the meta-data.
    auto const sOffset = (sInfo.regClass == RegClass::Spill)
      ? color_spill_slot(sInfo.color).slot
      : color_spill_slot_wide(sInfo.color).slot + state.numSpillSlots;
    auto const dOffset = (dInfo.regClass == RegClass::Spill)
      ? color_spill_slot(dInfo.color).slot
      : color_spill_slot_wide(dInfo.color).slot + state.numSpillSlots;

    auto const DEBUG_ONLY result1 = spillSrcSlotToReg.emplace(sOffset, src);
    assertx(IMPLIES(!result1.second, result1.first->second == src));

    auto const DEBUG_ONLY result2 = spillDstSlotToReg.emplace(dOffset, dst);
    assertx(result2.second);

    // Trivial useless copy
    if (sOffset == dOffset) return;

    auto const DEBUG_ONLY result3 = spillDstToSrc.emplace(dOffset, sOffset);
    assertx(result3.second);
  };

  // Build up the list of moves from the instruction.
  auto const& inst = state.unit.blocks[block].code[instIdx];
  switch (inst.op) {
    // copy2 needs to be treated specially. We don't lower it to a
    // move plan (because it has special semantics). If it's a no-op,
    // we can remove it, but otherwise leave it as is.
    case Vinstr::copy2: {
      if (inst.copy2_.s0 == inst.copy2_.d0 &&
          inst.copy2_.s1 == inst.copy2_.d1) {
        vmodify(state.unit, block, instIdx, [] (Vout&) { return 1; });
      }
      return;
    }
    case Vinstr::copy:
      if (inst.copy_.s == inst.copy_.d) {
        vmodify(state.unit, block, instIdx, [] (Vout&) { return 1; });
        return;
      } else if (inst.copy_.s.isPhys() && inst.copy_.d.isPhys()) {
        // Single argument copies of physical registers don't need
        // lowering.
        return;
      } else {
        addSrcDstPair(inst.copy_.s, inst.copy_.d);
        break;
      }
    case Vinstr::copyargs: {
      auto const& srcs = state.unit.tuples[inst.copyargs_.s];
      auto const& dsts = state.unit.tuples[inst.copyargs_.d];
      assertx(srcs.size() == dsts.size());
      auto noop = true;
      for (size_t i = 0; i < srcs.size(); ++i) {
        if (srcs[i] == dsts[i]) continue;
        noop = false;
        addSrcDstPair(srcs[i], dsts[i]);
      }
      if (noop) {
        // Trivial copy, just remove it
        vmodify(state.unit, block, instIdx, [] (Vout&) { return 1; });
        return;
      }
      break;
    }
    case Vinstr::phijmp: {
      auto const& srcs = state.unit.tuples[inst.phijmp_.uses];
      auto const succ = succs(state.unit.blocks[block]);
      assertx(succ.size() == 1);
      auto const& succBlock = state.unit.blocks[succ[0]];
      auto const& phidef = succBlock.code.front();
      assertx(phidef.op == Vinstr::phidef);
      auto const& dsts = state.unit.tuples[phidef.phidef_.defs];
      assertx(srcs.size() == dsts.size());
      auto noop = true;
      for (size_t i = 0; i < srcs.size(); ++i) {
        if (srcs[i] == dsts[i]) continue;
        noop = false;
        addSrcDstPair(srcs[i], dsts[i]);
      }
      phiTarget = inst.phijmp_.target;
      if (noop) {
        // Trivial phi, just turn the phijmp into a normal jmp.
        vmodify(
          state.unit, block, instIdx,
          [&] (Vout& v) {
            v << jmp{phiTarget};
            return 1;
          }
        );
        return;
      }
      break;
    }
    default:
      always_assert(false);
  }

  using RegMoveInfo = MoveInfo<PhysReg>;
  using SpillMoveInfo = MoveInfo<size_t>;
  jit::vector<RegMoveInfo> regMoves;
  jit::vector<SpillMoveInfo> spillMoves;

  // Turn the register moves and spill moves into move plans.
  if (!regDstToSrc.empty()) {
    regMoves = make_move_plan(regDstToSrc);
  }
  if (!spillDstToSrc.empty()) {
    spillMoves = make_move_plan(spillDstToSrc);
  }

  vmodify(
    state.unit, block, instIdx,
    [&] (Vout& v) {
      // Turn the reg move plan into copies.
      for (auto const& move : regMoves) {
        assertx(move.src != move.dst);
        assertx(move.src != state.scratch);
        assertx(move.dst != state.scratch);

        switch (move.kind) {
          case RegMoveInfo::Kind::Move:
            v << copy{move.src, move.dst};
            break;
          case RegMoveInfo::Kind::Xchg:
            // Emit a swap if both operands are GP registers. We can't swap
            // SIMDs, so use the scratch register instead.
            if (move.src.isGP() && move.dst.isGP()) {
              v << copy2{move.src, move.dst, move.dst, move.src};
            } else {
              v << copy{move.src, state.scratch};
              v << copy{move.dst, move.src};
              v << copy{state.scratch, move.dst};
            }
            break;
        }
      }

      auto const srcSlotToReg = [&] (size_t offset) {
        auto const it = spillSrcSlotToReg.find(offset);
        assertx(it != spillSrcSlotToReg.end());
        return it->second;
      };
      auto const dstSlotToReg = [&] (size_t offset) {
        auto const it = spillDstSlotToReg.find(offset);
        assertx(it != spillDstSlotToReg.end());
        return it->second;
      };

      // We cannot lower spill moves into copies (obviously). Instead we emit a
      // reload into the scratch register and then a spill from the scratch
      // register. This accomplishes a mem-to-mem move. The actual spill/reload
      // instructions will be lowered to from/to mem moves later on.
      for (auto const& move : spillMoves) {
        assertx(move.src != move.dst);

        switch (move.kind) {
          case SpillMoveInfo::Kind::Move: {
            auto const s = srcSlotToReg(move.src);
            auto const d = dstSlotToReg(move.dst);
            v << reload{s, state.scratch};
            v << spill{state.scratch, d};
            break;
          }
          case SpillMoveInfo::Kind::Xchg: {
            // Swaps for spills are a pain. We need to use both the scratch
            // register and a temporary spill slot on the stack.
            auto const s1 = srcSlotToReg(move.src);
            auto const s2 = srcSlotToReg(move.dst);
            auto const d1 = dstSlotToReg(move.src);
            auto const d2 = dstSlotToReg(move.dst);
            v << reload{s1, state.scratch};
            if (reg_info(state, s1).regClass == RegClass::SpillWide) {
              v << storeups{state.scratch, rsp()[-16]};
            } else {
              v << store{state.scratch, rsp()[-16]};
            }
            v << reload{s2, state.scratch};
            v << spill{state.scratch, d1};
            if (reg_info(state, d2).regClass == RegClass::SpillWide) {
              v << loadups{rsp()[-16], state.scratch};
            } else {
              v << load{rsp()[-16], state.scratch};
            }
            v << spill{state.scratch, d2};
            break;
          }
        }
      }

      // Keep the jmp part of the phijmp
      if (phiTarget.isValid()) v << jmp{phiTarget};

      // Delete the original instruction
      return 1;
    }
  );
}

/*
 * Spill materialization:
 *
 * The materialization of spill and reload instructions can be complicated. The
 * actual transformation of the instructions is easy, but we need to account for
 * the stack space the spill slots use.
 *
 * Spill slots occupy space on the stack, so the stack pointer needs to be
 * adjusted appropriately. We cannot simply access beyond the stack pointer
 * because it might be clobbered by a call.
 *
 * There are two complications: (1) We want to defer the stack pointer
 * adjustment until it's actually needed (the first spill). (2) Other
 * instructions may manipulate the stack pointer. We want to insert the stack
 * pointer adjustments right before a spill slot is used (requiring the space),
 * or when the stack pointer is changed by another instruction.
 *
 * First we split any side exiting blocks. These are blocks which contain
 * instructions which implicitly exit the unit (without being reflected in
 * control flow). The problem is, we might want to insert stack pointer
 * adjustment code along such exits, and we need a place to do so. We replace
 * these instructions with conditional jumps to blocks which use the
 * unconditional version of the side-exit.
 *
 * Next we calculate the live regions for the spill slots. This is just the
 * blocks where there's a live spilled value. Inside these regions, we need to
 * have allocated the spill space on the stack (so the stack pointer should be
 * adjusted).
 *
 * We then calculate the live regions for the stack pointer. This is the blocks
 * where the stack pointer has been adjusted (by other instructions) away from
 * its position when we entered the unit.
 *
 * Normally we insert the stack pointer adjustments when we enter or exit the
 * spill slots live region. However, if we're already in a stack pointer live
 * region when we enter or exit, we need to instead move the adjustment outward
 * to the entry/exit of that stack pointer region. This is because we need to
 * perform our adjustment before/after any stack pointer adjustment done by
 * other instructions.
 *
 * So, if any spill slots live region is adjacent to a stack pointer live
 * region, we expand the spill slots region to include that stack pointer live
 * region. We repeat this process until the spill slots live region won't expand
 * any further.
 *
 * Once this is done, we insert the actual stack pointer adjustments at the
 * frontier of the spill slots live regions. Now that the stack is set up
 * correctly, we lower the spill/reload instructions into stores or loads, using
 * the stack pointer offset at that point.
 *
 * If we split any side-exiting blocks above, we then run some vasm optimization
 * passes to attempt to recollapse them to side-exit instructions. We may not
 * have had to insert any adjustment code there, so they can go back to their
 * original form.
 */

// Return the amount an instruction will move the stack pointer. Negative is
// moving away from the frame pointer, and positive is moving towards it. This
// only supports the type of instructions we expect to see, and not abitrary
// ones.
int sp_change(const State& state, const Vinstr& inst) {
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
      auto const& lea = inst.lea_;
      if (lea.d == rsp()) {
        assertx(lea.s.base == lea.d && !lea.s.index.isValid());
        return lea.s.disp;
      }
      return 0;
    }
    default:
      // No other instruction should be writing to the stack pointer.
      if (debug) {
        visitDefs(
          state.unit, inst,
          [&] (Vreg r) { always_assert(r != rsp()); }
        );
      }
      return 0;
  }
}

// Per-block information about the offset of the stack pointer (relative to its
// position at the start of the unit).
struct SPOffset {
  Optional<int> in;
  Optional<int> out;
};
using SPOffsets = jit::vector<SPOffset>;

// Calculate stack pointer offset information
SPOffsets calculate_sp_offsets(const State& state) {
  auto const& unit = state.unit;

  SPOffsets spOffsets(unit.blocks.size());

  // We don't need dataflow for this because we require that the stack pointer
  // offset is always statically known. This implies that it cannot have
  // different offsets at join points (we'll assert otherwise), so one pass is
  // sufficient.
  for (auto const b : state.rpo) {
    auto spOffset = spOffsets[b].in;

    // Calculate the block's instruction effects on the offset.
    auto const& block = unit.blocks[b];
    for (size_t i = 0; i < block.code.size(); i++) {
      auto const& inst = block.code[i];
      if (inst.op == Vinstr::recordbasenativesp) {
        assertx(!spOffset);
        spOffset = 0;
      } else if (inst.op == Vinstr::unrecordbasenativesp) {
        assert_flog(spOffset, "Block B{} Instr {} uninitiailizes native SP, "
                    "but already uninitialized.", b, i);
        spOffset = std::nullopt;
      } else if (spOffset) {
        *spOffset += sp_change(state, inst);
        // Don't support moving the stack pointer before where it started
        // originally.
        assertx(*spOffset <= 0);
      }
    }

    // Propagate state to successors.
    auto const successorList = succs(block);
    // If there's no successors, we're exiting the unit, so make sure the stack
    // pointer has been returned to its entry position.
    assertx(IMPLIES(successorList.empty(), !spOffset || *spOffset == 0));
    for (auto const succ : successorList) {
      // Join point shouldn't have differing offsets.
      assertx(!spOffsets[succ].in || (spOffset &&
                                      *spOffsets[succ].in == *spOffset));
      spOffsets[succ].in = spOffset;
    }

    spOffsets[b].out = spOffset;
  }

  return spOffsets;
}

// If the given instruction is a spill or reload, turn it into the
// appropriate load/store instruction to/from memory. Use the given
// stack pointer offset to calculate the location of the spill
// slot. Return true if the transformation happened, false
// otherwise. If additional instructions are needed to be generated,
// advance instIdx as appropriate.
//
// `skip' is any additional space that has been allocated beyond the
// spill slots before the stack pointer (and therefore must be skipped
// over when accessing the spill slots). See diagram below:

/*
 * (Stack grows downward)
 *
 *                 | Prev Frame    |
 * prev %rsp --->  -----------------   <-----------------------
 *                 | Spill Slot #0 |     |                    |
 *                 -----------------     |                    |
 *                 | Spill Slot #1 |     |--- spillSpace (24) |
 *                 -----------------     |                    |
 *                 | Spill Slot #2 |     |                    |-- spOffset (-40)
 *                 -----------------   <--                    |
 *                 |               |     |                    |
 *                 -----------------     |--- skip (16)       |
 *                 |               |     |                    |
 *      %rsp --->  -----------------   <-----------------------
 *
 */
bool materialize_spill(const State& state,
                       Vlabel b,
                       Vinstr& inst,
                       size_t& instIdx,
                       Optional<int> skip) {
  auto const getSkip = [&skip] {
    // We have already asserted there is no spilling prior to
    // recordbasenativesp
    assertx(skip);
    return *skip;
  };

  auto const to_offset = [&] (SpillSlot slot) {
    return slot.slot*8 + getSkip();
  };
  auto const to_offset_wide = [&] (SpillSlotWide slot) {
    return state.numSpillSlots*8 + slot.slot*16 + getSkip();
  };

  // Materialize a spill of an immediate.
  auto const spillImmed = [&] (Vreg d,
                               Immed i,
                               auto const& immedToReg) {
    assertx(getSkip() >= 0);
    auto const& info = reg_info(state, d);
    assertx(is_spill(info.regClass));

    if (info.regClass == RegClass::Spill) {
      // If it's a normal spill slot, just write it using a storeqi
      // (note that despite the size of the immediate, we always write
      // to the full spill slot).
      auto const color = color_spill_slot(info.color);
      inst.storeqi_ = storeqi{i, rsp()[to_offset(color)]};
      inst.op = Vinstr::storeqi;
      invalidate_cached_operands(inst);
    } else {
      // Otherwise it's a wide spill slot. We can't write an immediate
      // directly to a wide spill slot (no 128-bit store immediate
      // instruction). Instead we materialize it into a wide register,
      // and then write that to memory. We can use the scratch
      // register for this (and have to, because there's no guarantee
      // any other register is free).
      assertx(info.regClass == RegClass::SpillWide);
      auto const color = color_spill_slot_wide(info.color);
      vmodify(
        state.unit, b, instIdx,
        [&] (Vout& v) {
          v << immedToReg(i, state.scratch);
          v << storeups{
            state.scratch,
            rsp()[to_offset_wide(color)]
          };
          return 1;
        }
      );
      ++instIdx;
    }

    return true;
  };

  switch (inst.op) {
    case Vinstr::spill: {
      assertx(getSkip() >= 0);
      assertx(inst.spill_.s.isPhys());
      auto const& info = reg_info(state, inst.spill_.d);
      assertx(is_spill(info.regClass));

      if (info.regClass == RegClass::Spill) {
        auto const color = color_spill_slot(info.color);
        inst.store_ = store{
          inst.spill_.s,
          rsp()[to_offset(color)]
        };
        inst.op = Vinstr::store;
      } else {
        assertx(info.regClass == RegClass::SpillWide);
        assertx(inst.spill_.s.isSIMD());
        auto const color = color_spill_slot_wide(info.color);
        inst.storeups_ = storeups{
          inst.spill_.s,
          rsp()[to_offset_wide(color)]
        };
        inst.op = Vinstr::storeups;
      }
      invalidate_cached_operands(inst);

      return true;
    }
    case Vinstr::reload: {
      assertx(getSkip() >= 0);
      assertx(inst.reload_.d.isPhys());
      auto const& info = reg_info(state, inst.reload_.s);
      assertx(is_spill(info.regClass));

      if (info.regClass == RegClass::Spill) {
        auto const color = color_spill_slot(info.color);
        inst.load_ = load{
          rsp()[to_offset(color)],
          inst.reload_.d
        };
        inst.op = Vinstr::load;
      } else {
        assertx(info.regClass == RegClass::SpillWide);
        assertx(inst.reload_.d.isSIMD());
        auto const color = color_spill_slot_wide(info.color);
        inst.loadups_ = loadups{
          rsp()[to_offset_wide(color)],
          inst.reload_.d
        };
        inst.op = Vinstr::loadups;
      }
      invalidate_cached_operands(inst);

      return true;
    }
    case Vinstr::spillbi:
      return spillImmed(
        inst.spillbi_.d,
        inst.spillbi_.s,
        [&] (Immed i, Vreg r) { return ldimmb{i, r}; }
      );
    case Vinstr::spillli:
      return spillImmed(
        inst.spillli_.d,
        inst.spillli_.s,
        [&] (Immed i, Vreg r) { return ldimml{i, r}; }
      );
    case Vinstr::spillqi:
      return spillImmed(
        inst.spillqi_.d,
        inst.spillqi_.s,
        [&] (Immed i, Vreg r) { return ldimmq{i.q(), r}; }
      );
    case Vinstr::spillundefq:
      assertx(getSkip() >= 0);
      assertx(is_spill(reg_info(state, inst.spillundefq_.d).regClass));
      inst.nop_ = nop{};
      inst.op = Vinstr::nop;
      invalidate_cached_operands(inst);
      return true;
    default:
      return false;
  }
}

// Turn all spill/reload instructions in the unit into appropriate load/store
// instructions to/from memory. spillSpace is the total amount of space
// allocated for spills.
void materialize_spills(const State& state, size_t spillSpace) {
  // The exact offset to use for a stack slot differs depending on the current
  // stack pointer offset (because spill slots are addressed off the stack
  // pointer). Therefore we need the offsets for every block.
  auto const spOffsets = calculate_sp_offsets(state);
  assertx(spOffsets.size() == state.unit.blocks.size());

  for (auto const b : state.rpo) {
    auto spOffset = spOffsets[b].in;
    assertx(IMPLIES(b == state.unit.entry, !spOffset));
    assertx(!spOffset || *spOffset <= 0);

    for (size_t i = 0; i < state.unit.blocks[b].code.size(); ++i) {
      auto& inst = state.unit.blocks[b].code[i];
      Optional<int> skip;
      if (spOffset) skip = -*spOffset - spillSpace;
      if (materialize_spill(state, b, inst, i, skip)) {
        assertx(sp_change(state, inst) == 0);
      } else {
        if (inst.op == Vinstr::recordbasenativesp) {
          assertx(!spOffset);
          spOffset = 0;
        } else if (inst.op == Vinstr::unrecordbasenativesp) {
          assertx(spOffset);
          spOffset = std::nullopt;
        } else if (spOffset) {
          *spOffset += sp_change(state, inst);
          assertx(*spOffset <= 0);
        }
      }
    }

    // Do a sanity check that our offset calculation for the block matches what
    // was calculated by calculate_sp_offsets().
    if (debug) {
      auto const successorList = succs(state.unit.blocks[b]);
      always_assert(IMPLIES(successorList.empty(),
                            !spOffset || *spOffset == 0));
      for (auto const succ : successorList) {
        always_assert(spOffsets[succ].in == spOffset);
      }
    }
  }
}

// Per-block liveness information for either spill slots or the stack pointer
// offset. We call the stack pointer offset alive if it's different than its
// entry block position (non-zero).
struct SPAdjustLiveness {
  bool in = false; // If true, the spill slots/stack pointer is live-in
  bool out = false; // If true, the spill slots/stack pointer is live-out
  // If set, the index of the first instruction in the block which goes from
  // dead to alive.
  Optional<size_t> begin;
  // If set, the index of the last instruction in the block which goes from
  // alive to dead.
  Optional<size_t> end;
  // Whether this block contains an indirect fixup
  bool hasIndirectFixup;
};

// Implementation of spill liveness algorithm, templated to allow for
// different set representations for the slots.
template <typename T, typename Resize, typename Subtract>
jit::vector<SPAdjustLiveness> find_spill_liveness_impl(const State& state,
                                                       const SPOffsets& spOff,
                                                       Resize resize,
                                                       Subtract subtract) {
  auto const& unit = state.unit;

  // First calculate the spill slot liveness in the conventional
  // manner using dataflow.
  struct SlotLiveness {
    T in;
    T out;
    T gen;
    T kill;
  };
  jit::vector<SlotLiveness> slotLiveness(unit.blocks.size());

  dataflow_worklist<size_t, std::less<size_t>> worklist(state.rpo.size());
  for (size_t i = 0; i < state.rpo.size(); ++i) worklist.push(i);

  auto const offset = [&] (Vreg r) {
    auto const& info = reg_info(state, r);
    if (info.regClass == RegClass::Spill) {
      return color_spill_slot(info.color).slot;
    } else {
      assertx(info.regClass == RegClass::SpillWide);
      return color_spill_slot_wide(info.color).slot + state.numSpillSlots;
    }
  };

  jit::vector<SPAdjustLiveness> liveness(unit.blocks.size());

  for (auto const b : state.rpo) {
    auto& live = slotLiveness[b];
    resize(live.in);
    resize(live.out);
    resize(live.gen);
    resize(live.kill);

    auto const& block = unit.blocks[b];
    for (auto const& inst : boost::adaptors::reverse(block.code)) {
      if (is_spill_inst(inst)) {
        auto const o = offset(spill_inst_dest(inst));
        live.kill[o] = true;
        live.gen[o] = false;
      } else if (inst.op == Vinstr::reload) {
        auto const o = offset(inst.reload_.s);
        live.kill[o] = false;
        live.gen[o] = true;
      }
      if (instrHasIndirectFixup(inst)) liveness[b].hasIndirectFixup = true;
    }

    live.in = live.gen;
  }

  while (!worklist.empty()) {
    auto const b = state.rpo[worklist.pop()];
    auto& live = slotLiveness[b];

    live.out.reset();
    auto const& block = unit.blocks[b];
    for (auto const succ : succs(block)) {
      live.out |= slotLiveness[succ].in;
    }

    auto in = subtract(live.out, live.kill) | live.gen;
    if (in != live.in) {
      for (auto const pred : state.preds[b]) {
        worklist.push(state.rpoOrder[pred]);
      }
      live.in = std::move(in);
    }
  }

  for (auto const b : state.rpo) {
    auto& live = liveness[b];
    bool spRecorded = spOff[b].in.has_value();

    // The spill slots are alive if any of the individual ones were calculated
    // to be alive (and vice-versa).
    live.in = slotLiveness[b].in.any();
    live.out = slotLiveness[b].out.any();

    // If there are no spill slots alive going into the block, look for any
    // instruction which make them alive.
    if (!live.in) {
      auto& block = unit.blocks[b];
      for (size_t i = 0; i < block.code.size(); ++i) {
        auto const& inst = block.code[i];
        assertx(inst.op != Vinstr::reload);
        if (inst.op == Vinstr::recordbasenativesp) spRecorded = true;
        else if (inst.op == Vinstr::unrecordbasenativesp) spRecorded = false;
        if (!is_spill_inst(inst)) continue;
        live.begin = i;
        // This should be caught earlier when the spill occurs.
        assert_flog(spRecorded, "Trying to spill before allowed. "
                    "Spill space is being allocated due to a spill at "
                    "B{} Instr {}, but the base native sp is not yet "
                    "recorded.", b, i);
        break;
      }
    } else {
      always_assert_flog(spRecorded, "Trying to spill before or after it is "
                         "allowed. Spill slots are live at the start of B{}, "
                         "but the base native sp is not yet recorded. There "
                         "is a spill somewhere in a loop that is not "
                         "dominated by the recording of the base native sp.",
                         b);
    }

    // If there are no spill slots alive going out of the block and there's any
    // spill slot alive within it, look backwards for any instruction which
    // kills them.
    if (!live.out && (live.in || live.begin)) {
      auto& block = unit.blocks[b];
      for (size_t i = block.code.size(); i > 0; --i) {
        auto const& inst = block.code[i-1];
        // A spill slot could be completely dead, in which case its live-range
        // is just the single spill instruction defining it (without a reload).
        if (inst.op != Vinstr::reload && !is_spill_inst(inst)) continue;
        live.end = i - 1;
        break;
      }
    }
  }

  return liveness;
}

// Calculate liveness information for the spill slots.
jit::vector<SPAdjustLiveness> find_spill_liveness(const State& state,
                                                  const SPOffsets& spOffsets) {
  auto const numSlots = state.numSpillSlots + state.numWideSpillSlots;

  static constexpr size_t kSmallLimit = 128;
  if (numSlots <= kSmallLimit) {
    // In the common case we have a small number of spills and we can
    // use a fixed size bitset to represent them, which is much
    // faster.
    using Bitset = std::bitset<kSmallLimit>;
    return find_spill_liveness_impl<Bitset>(
      state,
      spOffsets,
      [] (const Bitset&) {},
      [] (const Bitset& a, const Bitset& b) { return a & ~b; }
    );
  } else {
    // Otherwise use a dynamic bitset, which is slower but can handle
    // an arbitrary number of spills.
    using Bitset = boost::dynamic_bitset<>;
    return find_spill_liveness_impl<Bitset>(
      state,
      spOffsets,
      [&] (Bitset& s) { s.resize(numSlots); },
      [] (const Bitset& a, const Bitset& b) { return a - b; }
    );
  }
}

// Calculate liveness information for the stack pointer offset
jit::vector<SPAdjustLiveness> find_sp_liveness(const State& state,
                                               const SPOffsets& spOffsets,
                                               bool& found) {
  auto const& unit = state.unit;
  jit::vector<SPAdjustLiveness> liveness(unit.blocks.size());

  for (auto const b : state.rpo) {
    auto in = spOffsets[b].in;
    auto out = spOffsets[b].out;

    auto& live = liveness[b];

    if (!in || *in == 0) {
      // The in-offset of the block is 0, so the stack pointer offset is dead
      // coming into the block. Look for any instruction which changes it to
      // make the stack pointer offset alive.
      auto& block = unit.blocks[b];
      for (size_t i = 0; i < block.code.size(); ++i) {
        auto const& inst = block.code[i];
        if (inst.op == Vinstr::recordbasenativesp) {
          assertx(!in);
          in = 0;
          continue;
        }
        if (inst.op == Vinstr::unrecordbasenativesp) {
          assertx(in);
          assert_flog(*in == 0, "Base native sp unrecorded when spill space "
                      "is live.");
          in = std::nullopt;
          continue;
        }
        if (!in || sp_change(state, inst) == 0) continue;
        live.begin = i;
        found = true;
        break;
      }
    } else {
      // Otherwise it's live-in to the block.
      live.in = true;
      found = true;
    }

    if (!out || *out == 0) {
      // The out-offset of the block is 0, so the stack pointer offset is dead
      // going out of the block. Look backwards for any instruction which
      // changes it to make the stack pointer offset alive.
      auto& block = unit.blocks[b];
      for (size_t i = block.code.size(); i > 0; --i) {
        auto const& inst = block.code[i-1];
        if (inst.op == Vinstr::recordbasenativesp) {
          assertx(out);
          assert_flog(*out == 0, "Base native sp unrecorded when spill space "
                      "is live.");
          out = std::nullopt;
          continue;
        }
        if (inst.op == Vinstr::unrecordbasenativesp) {
          assertx(!out);
          out = 0;
          continue;
        }
        if (!out || sp_change(state, inst) == 0) continue;
        live.end = i - 1;
        found = true;
        break;
      }
    } else {
      // Otherwise it's live-out to the block.
      live.out = true;
      found = true;
    }
  }

  return liveness;
}

// Expand the spill slot liveness information to also include the stack pointer
// offset liveness if they are adjacent anywhere. This will give us the region
// where we have to make stack pointer adjustments.
void expand_spill_liveness(const State& state,
                           jit::vector<SPAdjustLiveness>& spillLiveness,
                           const jit::vector<SPAdjustLiveness>& spLiveness) {
  // First find the blocks where the spill slots and stack pointer offset are
  // both alive and expand the begin or end data appropriately.
  for (auto const b : state.rpo) {
    auto& spill = spillLiveness[b];
    auto const& sp = spLiveness[b];

    if (!spill.in && !spill.out && !spill.begin && !spill.end) continue;

    if (!spill.in && sp.in) {
      spill.in = true;
      spill.begin.reset();
    }

    if (!spill.out && sp.out) {
      spill.out = true;
      spill.end.reset();
    }

    if (spill.begin && sp.begin) {
      spill.begin = std::min(*spill.begin, *sp.begin);
    }

    if (spill.end && sp.end) {
      spill.end = std::max(*spill.end, *sp.end);
    }
  }

  // Then expand the spill slot live region to include any stack pointer offset
  // live regions that are adjacent to it. We repeat this process until we can't
  // expand it any further.
  auto changed = true;
  while (changed) {
    changed = false;

    for (auto const b : state.rpo) {
      auto& spill = spillLiveness[b];
      auto const& sp = spLiveness[b];

      // If the spill slot liveness range already covers this block, no need to
      // extend it.
      if (spill.in || spill.out || spill.begin || spill.end) continue;

      if (sp.out) {
        for (auto const succ : succs(state.unit.blocks[b])) {
          if (!spillLiveness[succ].in) continue;
          spill = sp;
          changed = true;
          break;
        }
      }

      if (spill.out) continue;

      if (sp.in) {
        for (auto const pred : state.preds[b]) {
          if (!spillLiveness[pred].out) continue;
          spill = sp;
          changed = true;
          break;
        }
      }
    }
  }

  // The spill slot live region(s) now incorporates any stack pointer offset
  // live region(s).
}

// Insert stack pointer adjustments at the appropriate places
void insert_sp_adjustments(State& state, size_t spillSpace) {
  assertx(spillSpace != 0);
  // We make use of the stack pointer offset info to determine liveness of the
  // stack pointer, and establish if the base sp has been recorded.
  auto const spOffsets = calculate_sp_offsets(state);
  assertx(spOffsets.size() == state.unit.blocks.size());

  // Calculate where spill slots are alive, then where the stack pointer is
  // alive (non zero offset), and expand the spill slot liveness to match the
  // stack pointer liveness.
  auto spillLiveness = find_spill_liveness(state, spOffsets);

  auto found = false;
  auto const spLiveness = find_sp_liveness(state, spOffsets, found);
  if (found) expand_spill_liveness(state, spillLiveness, spLiveness);

  auto& unit = state.unit;
  // Insert a lea to adjust the stack pointer (either forwards or backwards)
  // before the instruction at the given index.
  auto const modify = [&] (Vlabel b, size_t check, size_t i, int adjustment) {
    // If there's already a lea there, we can just modify that.
    if (unit.blocks[b].code[check].op == Vinstr::lea) {
      auto& lea = unit.blocks[b].code[check].lea_;
      if (lea.d == rsp() && lea.s.base == rsp() && !lea.s.index.isValid()) {
        lea.s.disp += adjustment;
        return 0;
      }
    }
    vmodify(
      unit, b, i,
      [&] (Vout& v) {
        v << lea{rsp()[adjustment], rsp()};
        return 0;
      }
    );
    return 1;
  };

  auto const adjust_rip = [&](Vlabel b, size_t begin, size_t end) {
    assertx(begin < state.unit.blocks[b].code.size());
    assertx(end <= state.unit.blocks[b].code.size());
    for (size_t i = begin; i < end; ++i) {
      auto& inst = state.unit.blocks[b].code[i];
      if (instrHasIndirectFixup(inst)) {
        updateIndirectFixupBySpill(inst, spillSpace);
      }
    }
  };

  auto const prologue = isPrologue(state.unit.context->kind);
  for (auto const b : state.rpo) {
    auto const& spill = spillLiveness[b];

    if (prologue && spill.hasIndirectFixup) {
      if (spill.begin) {
        // Spill begins in this block
        assertx(!spill.in);
        assertx(IMPLIES(!spill.end, spill.out));
        assertx(IMPLIES(spill.end, !spill.out));
        auto const end =
          spill.end ? *spill.end + 1 : state.unit.blocks[b].code.size();
        adjust_rip(b, *spill.begin, end);
      } else if (spill.end) {
        // Spill ends in this block
        assertx(spill.in);
        assertx(!spill.out);
        adjust_rip(b, 0, *spill.end + 1);
      } else if (spill.in && spill.out) {
        // Spill persists in this block
        adjust_rip(b, 0, state.unit.blocks[b].code.size());
      } else {
        assertx(!spill.in && !spill.out);
      }
    }

    size_t added = 0;
    if (spill.begin) {
      // The spill slot liveness begins here at an instruction, so insert a
      // stack pointer adjustment before that instruction.
      added += modify(b, *spill.begin, *spill.begin, -spillSpace);
    }
    if (spill.end) {
      // The spill slot liveness ends here at an instruction, so insert a stack
      // pointer adjustment back before that instruction.
      added += modify(
        b, *spill.end + added, *spill.end + added + 1, spillSpace
      );
    }

    if (!spill.out) continue;
    for (auto const succ : succs(unit.blocks[b])) {
      if (spillLiveness[succ].in) continue;
      // The spill slot liveness is live out of the block, but not live into
      // this successor. This can only happen if the block has multiple
      // successors (which implies the successor has only one predecessor, since
      // we've split critical edges). We need to insert a stack pointer
      // adjustment at the beginning of the successor.
      assertx(succs(unit.blocks[b]).size() > 1);
      assertx(state.preds[succ].size() == 1);

      auto& succSpill = spillLiveness[succ];
      if (succSpill.begin) {
        // If we're going to need a stack pointer adjustment later in the
        // successor, we can just elide it by forgoing the adjustment at the
        // entry of the successor.
        succSpill.in = true;
        succSpill.begin.reset();
      } else {
        // Otherwise insert the adjustment at the beginning of the successor,
        // skipping over a landingpad instruction if present. Since this can add
        // instructions, we need to fix up the instruction offsets in the
        // successor's state (we already know that succSpill.begin isn't set).
        auto const idx
          = size_t{unit.blocks[succ].code[0].op == Vinstr::landingpad};
        auto const succAdded = modify(succ, idx, idx, spillSpace);
        if (succSpill.end) *succSpill.end += succAdded;
      }
    }
  }
}

// Turn spills into load/store instructions, adjusting the stack pointer where
// appropriate.
void lower_spills(State& state) {
  assertx((state.numSpillSlots % 2) == 0);

  // Calculate total space on the stack used by spills.
  auto const spillSpace =
    state.numSpillSlots * 8 + state.numWideSpillSlots * 16;

  assertx((spillSpace % 16) == 0);

  // Common case, no spills.
  if (spillSpace == 0) return;

  // Insert any stack pointer adjustments
  insert_sp_adjustments(state, spillSpace);
  // Actually transform the spills
  materialize_spills(state, spillSpace);

  // Do a second stack pointer offset calculation which will check if the
  // adjustment code kept the stack pointer in a consistent state.
  if (debug) calculate_sp_offsets(state);
}

void lower_ssa(State& state) {
  auto& unit = state.unit;

  /*
   * First pass:
   *
   * Suppose we have:
   *
   * B1: phijmp R1 -> B4
   * B2: phijmp R1 -> B4
   * B3: phijmp R1 -> B4
   * B4: phidef R2
   *
   * Rewrite this to:
   *
   * .....
   * B4: phidef R1
   *     copy R1 -> R2
   *
   * That is, if a phi has all the same register at the phijmps, but a
   * different register at the phidef, then change the phidef to the
   * register at the phijmps, and then copy to the actual dest. This
   * lets us make the phi trivial, and emit a single copy at the
   * def. Otherwise we'll emit a copy at every jmp, which increases
   * code size.
   */
  for (auto const b : state.rpo) {
    auto const& preds = state.preds[b];
    // Not an issue if we only have a single pred
    if (preds.size() < 2) continue;

    auto& phidef = unit.blocks[b].code.front();
    if (phidef.op != Vinstr::phidef) continue;

    auto& defs = unit.tuples[phidef.phidef_.defs];

    VregList copySrcs;
    VregList copyDsts;
    for (size_t i = 0; i < defs.size(); ++i) {
      // Any Vreg which isn't a physical register at this point should
      // be a spill.
      if (!defs[i].isPhys()) continue;

      // Iterate over the phijmps, looking for the same register.
      PhysReg same;
      for (auto const pred : preds) {
        auto const& phijmp = unit.blocks[pred].code.back();
        assertx(phijmp.op == Vinstr::phijmp);
        auto const& uses = unit.tuples[phijmp.phijmp_.uses];
        assertx(uses.size() == defs.size());

        assertx(uses[i].isPhys());
        if (uses[i] == defs[i]) {
          same = InvalidReg;
          break;
        }

        auto const phys = uses[i].physReg();
        if (same == InvalidReg) {
          same = phys;
        } else if (same != phys) {
          same = InvalidReg;
          break;
        }
      }

      if (same == InvalidReg) continue;
      // This optimization is invalid if the def writes to the same
      // register the jmps use.
      if (defs_set_cached(state, phidef)[same]) continue;

      copySrcs.emplace_back(same);
      copyDsts.emplace_back(defs[i]);
      defs[i] = same;
      invalidate_cached_operands(phidef);
    }

    assertx(copySrcs.size() == copyDsts.size());
    if (copySrcs.size() == 0) continue;

    vmodify(
      unit, b, 1,
      [&] (Vout& v) {
        if (copySrcs.size() == 1) {
          v << copy{copySrcs[0], copyDsts[0]};
        } else {
          v << copyargs{
            unit.makeTuple(std::move(copySrcs)),
            unit.makeTuple(std::move(copyDsts))
          };
        }
        return 0;
      }
    );
  }

  // Second pass: lower copy-ish instructions (including phis) to copy and copy2
  // instructions, and optimize them away if they're no-ops.
  for (auto const b : state.rpo) {
    for (auto i = unit.blocks[b].code.size(); i > 0; --i) {
      auto const& inst = unit.blocks[b].code[i - 1];
      switch (inst.op) {
        case Vinstr::copy:
        case Vinstr::copy2:
        case Vinstr::copyargs:
        case Vinstr::phijmp:
          lower_copies(state, b, i - 1);
          break;
        default:
          break;
      }
    }
  }

  // Third pass: Remove (now useless) phidef instructions
  for (auto const b : state.rpo) {
    auto const& inst = unit.blocks[b].code.front();
    if (inst.op != Vinstr::phidef) continue;
    vmodify(unit, b, 0, [] (Vout&) { return 1; });
  }

  lower_spills(state);

  assertx(check(unit));
}

//////////////////////////////////////////////////////////////////////

}

//////////////////////////////////////////////////////////////////////

void allocateRegistersWithGraphColor(Vunit& unit, const Abi& abi) {
  Timer timer(Timer::vasm_reg_alloc, unit.log_entry);

  if (Trace::moduleEnabled(Trace::vasm_graph_color, 2)) {
    printUnit(0, "before vasm-graph-color", unit);
  }

  assertx(check(unit));
  // We don't want to mutate the cfg at all (it can interfere with
  // vasm-block-counts), so we assert that the unit has had critical
  // edges pre-split, and that we don't have side exiting
  // instructions. No side exiting instructions means that any place
  // we leave the unit, there's an explicit exit block.
  assertx(checkNoCriticalEdges(unit));
  assertx(checkNoSideExits(unit));

  auto state = make_state(unit, abi);
  SCOPE_ASSERT_DETAIL("Graph color state") { return show(state); };

  prepare_unit(state);
  infer_register_classes(state);
  insert_spills(state);
  assign_colors(state);
  lower_ssa(state);

  if (Trace::moduleEnabled(Trace::vasm_graph_color, 1) ||
      Trace::moduleEnabled(Trace::vasm, kVasmRegAllocLevel)) {
    printUnit(0, "after vasm-graph-color", unit);
  }
}

//////////////////////////////////////////////////////////////////////

}
