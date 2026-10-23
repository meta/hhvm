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
#include "hphp/runtime/vm/jit/containers.h"
#include "hphp/runtime/vm/jit/pass-tracer.h"
#include "hphp/runtime/vm/jit/phys-reg.h"
#include "hphp/runtime/vm/jit/reg-alloc.h"
#include "hphp/runtime/vm/jit/timer.h"
#include "hphp/runtime/vm/jit/vasm-instr.h"
#include "hphp/runtime/vm/jit/vasm-print.h"
#include "hphp/runtime/vm/jit/vasm-reg.h"
#include "hphp/runtime/vm/jit/vasm-unit.h"
#include "hphp/runtime/vm/jit/vasm-visit.h"

#include "hphp/util/dataflow-worklist.h"
#include "hphp/util/trace.h"

#include <fmt/format.h>

#include <algorithm>
#include <limits>
#include <string>
#include <type_traits>

namespace HPHP::jit {

namespace {

TRACE_SET_MOD(vasm_copy)

///////////////////////////////////////////////////////////////////////////////

using RpoID = size_t;

constexpr auto kInvalidDisp = std::numeric_limits<int32_t>::max();

/*
 * The value of a physical register at a program point, relative to the values
 * physical registers had on entry to the program.
 *
 * Currently, we only track expressions of the form: %base + disp.
 */
struct PhysExpr {
  PhysExpr() : base(InvalidReg), disp(kInvalidDisp) {}
  PhysExpr(PhysReg base, int32_t disp) : base(base), disp(disp) {}

  PhysExpr(const PhysExpr&) = default;
  PhysExpr& operator=(const PhysExpr&) = default;

  bool operator==(const PhysExpr& o) const {
    return base == o.base && disp == o.disp;
  }
  bool operator!=(const PhysExpr& o) const { return !(*this == o); }

  PhysReg base;
  int32_t disp;
};

/*
 * Information about the definition of a virtual register.
 *
 * The `base' register may be a physical register, in which case `expr' is its
 * abstract value at the def (if known).
 */
struct DefInfo {
  bool operator==(const DefInfo& o) const {
    return base == o.base && disp == o.disp && expr == o.expr && copy == o.copy;
  }
  bool operator!=(const DefInfo& o) const { return !(*this == o); }

  Vreg base;
  int32_t disp;
  Vreg copy; // This Vreg is identical to "copy" (which might be
             // different from "base" if the displacement is
             // non-zero).
  PhysExpr expr;
};

/*
 * State of registers at a program point.
 */
struct RegState {
  PhysReg::Map<PhysExpr> phys;

  /*
   * Whether a Vreg def has been seen, or invalidated due to a dataflow
   * conflict, at this program point.
   *
   * The virtual `defs' metadata vector in Env is mutated during our
   * flow-sensitive abstract evaluation pass.  Since virtual Vregs are SSA, and
   * since we don't "chase" through physical register metadata when setting
   * their DefInfo, we don't need to flow `defs'.
   *
   * However, the converse is not true.  When we populate `phys', we do chase
   * through the information in `defs'.  This means we need to track changes to
   * that information in our dataflow analysis.  Fortunately, SSA gives us the
   * convenient invariant that the DefInfo for a Vreg can only change up to
   * twice: once when we first see the def, and possibly a second time if the
   * def was relative to a physical register and we see a different PhysExpr
   * for that register at the def.
   *
   * This is an optimization to avoid per-block tracking of all virtual
   * register defs.
   */
  boost::dynamic_bitset<> virt_seen;
  boost::dynamic_bitset<> virt_invalid;

  bool init{false};
};

/*
 * Inputs to, and results of, the analysis passes.
 */
struct Env {
  explicit Env(Vunit& unit, const Abi& abi)
    : unit(unit)
    , abi(abi)
    , rpo_blocks(sortBlocks(unit))
    , block_to_rpo(unit.blocks.size())
    , defs(unit.next_vr)
  {
    for (size_t i = 0, n = rpo_blocks.size(); i < n; ++i) {
      block_to_rpo[rpo_blocks[i]] = i;
    }
  }

  Vunit& unit;
  const Abi& abi;
  jit::vector<Vlabel> rpo_blocks;
  jit::vector<RpoID> block_to_rpo;

  /*
   * RegState at the entry of each block.
   *
   * Computed by a flow-sensitive abstract evaluation pass.
   */
  jit::vector<RegState> block_states;

  /*
   * Per-Vreg analysis info.
   *
   * This is computed alongside `block_states', but we can merge as we go since
   * we only keep DefInfo for virtual Vregs, which are SSA.
   *
   * The information in the DefInfo is "usable" if it is also virtual (and thus
   * SSA), or if it makes references to a physical register whose value at both
   * the use and def site are a known displacement from one another.
   */
  jit::vector<DefInfo> defs;
};

///////////////////////////////////////////////////////////////////////////////

DEBUG_ONLY std::string show(PhysExpr x) {
  return fmt::format("{} + {}", show(x.base), x.disp);
}

DEBUG_ONLY std::string show(const DefInfo& x) {
  return fmt::format(
    "{} + {}{}{}{}",
    show(x.base), x.disp,
    x.copy.isValid() ? " [" : "",
    x.copy.isValid() ? show(x.copy) : "",
    x.copy.isValid() ? "]" : ""
  );
}

///////////////////////////////////////////////////////////////////////////////

/*
 * Call `f' on every Vreg def'd by `inst'.
 */
template<class F>
void for_all_defs(Env& env, const Vinstr& inst, F f) {
  visitDefs(env.unit, inst, f);
  auto uses   = RegSet{};
  auto across = RegSet{};
  auto defs   = RegSet{};
  getEffects(env.abi, inst, uses, across, defs);
  defs.forEach(f);
}

/*
 * Initialize the block-in RegState vector in `env'.
 *
 * We invalidate all the physical register states except for in the entry
 * block, whose state is initialized to {r, 0} for each reserved PhysReg `r'.
 */
void initialize_reg_states(Env& env) {
  env.block_states.resize(env.unit.blocks.size());
  if (env.unit.blocks.empty()) return;

  auto& state = env.block_states[env.unit.entry];

  for (auto const r : state.phys) {
    state.phys[r] = PhysExpr { r, 0 };
  }
  state.virt_seen = boost::dynamic_bitset<>(env.unit.next_vr);
  state.virt_invalid = boost::dynamic_bitset<>(env.unit.next_vr);

  state.init = true;
}

/*
 * Merge `src' into `dst', returning whether we updated `dst'.
 */
bool merge_into(RegState& dst, const RegState& src) {
  assertx(src.init);

  if (!dst.init) {
    dst = src;
    return true;
  }

  auto changed = false;

  for (auto const r : dst.phys) {
    if (dst.phys[r] == src.phys[r]) continue;

    // Any two different PhysExprs are incompatible.
    dst.phys[r] = PhysExpr{};
    changed = true;
  }

  changed |= (dst.virt_seen != src.virt_seen) ||
             (dst.virt_invalid != src.virt_invalid);
  dst.virt_seen |= src.virt_seen;
  dst.virt_invalid |= src.virt_invalid;

  return changed;
}

/*
 * Stringify the analysis state for the beginning of each block.
 */
DEBUG_ONLY std::string show_fixed_point(Env& env) {
  auto ret = std::string{};

  for (auto b : env.rpo_blocks) {
    auto const& state = env.block_states[b];
    fmt::format_to(std::back_inserter(ret), "{: <4}:\n", b);

    for (auto const r : state.phys) {
      auto const expr = state.phys[r];
      if (expr == PhysExpr{}) continue;

      fmt::format_to(std::back_inserter(ret), "  {} := {}\n", show(r), show(expr));
    }
  }

  fmt::format_to(std::back_inserter(ret), "virtuals:\n");
  for (unsigned i = 0, n = env.defs.size(); i < n; ++i) {
    auto const& def = env.defs[i];
    if (!def.base.isValid()) continue;

    fmt::format_to(std::back_inserter(ret), "  {} := {}", show(Vreg{i}), show(def));
    if (def.expr != PhysExpr{}) {
      fmt::format_to(std::back_inserter(ret), " ({} := {})", show(def.base), show(def.expr));
    }
    fmt::format_to(std::back_inserter(ret), "\n");
  }

  return ret;
}

///////////////////////////////////////////////////////////////////////////////

/*
 * "Chase" the def metadata for `r' through its sources until we arrive at a
 * physical source, then compute a PhysExpr.
 *
 * If no physical source is found, return an invalid PhysExpr.
 */
PhysExpr chase_thru(const Env& env, Vreg r) {
  if (!r.isVirt()) return PhysExpr{};

  auto const& def = env.defs[r];
  if (!def.base.isValid()) return PhysExpr{};

  if (def.base.isPhys()) {
    if (def.expr == PhysExpr{}) {
      return PhysExpr{};
    }
    auto expr = def.expr;
    expr.disp += def.disp;
    return expr;
  }
  assertx(def.base.isVirt());

  auto expr = chase_thru(env, def.base);
  if (expr != PhysExpr{}) {
    expr.disp += def.disp;
    return expr;
  }
  return PhysExpr{};
}

/*
 * Get or compute a PhysExpr for `s', else return an invalid PhysExpr.
 */
PhysExpr expr_for(const Env& env, RegState& state, Vreg s) {
  return s.isPhys()
    ? state.phys[s]
    : chase_thru(env, s);
}

///////////////////////////////////////////////////////////////////////////////

/*
 * Analyze a copy from `s' to `d'.
 *
 * Returns true if no further analysis is needed for the def to `d'.
 */
bool analyze_phys_copy(const Env& env, RegState& state, Vreg d, Vreg s) {
  if (!d.isPhys()) return true;

  auto const expr = expr_for(env, state, s);
  if (expr == PhysExpr{}) return false;

  state.phys[d] = expr;
  FTRACE(3, "      {} = {}\n", show(d), show(state.phys[d]));
  return true;
}

/*
 * Analyze an instruction which performs `d := s + disp'.
 *
 * Returns true if no further analysis is needed for the def to `d'.
 */
bool analyze_phys_disp(const Env& env, RegState& state,
                       Vreg d, Vreg s, int32_t disp) {
  if (!d.isPhys()) return true;

  auto const expr = expr_for(env, state, s);
  if (expr == PhysExpr{}) return false;

  state.phys[d] = expr;
  state.phys[d].disp += disp;
  FTRACE(3, "      {} = {}\n", show(d), show(state.phys[d]));
  return true;
}

/*
 * Analyze a def that can't be tracked as a copy or displacement.
 *
 * Always returns true, for easy chaining of analysis routines.
 */
bool analyze_phys_def(const Env& env, RegState& state, Vreg d) {
  if (!d.isPhys()) return true;

  FTRACE(3, "      kill {}\n", show(d));
  state.phys[d] = PhysExpr{};

  return true;
}

/*
 * Merge `src' into the DefInfo for `d'.
 *
 * This just sets `env.defs[d]' to `src' if it was uninitialized, else checks
 * if it matches, and invalidates it if not.
 *
 * Since virtual Vregs are SSA, two DefInfos should always match /unless/ they
 * differ in the def-time PhysExpr.  For this reason, since we track the
 * PhysExpr explicitly as part of our dataflow loop, we don't need to track
 * whether a Vreg's DefInfo has changed.
 */
void merge_def_info(Env& env, RegState& state, Vreg d, const DefInfo& src) {
  auto& def = env.defs[d];

  auto const s = src.base;
  auto const expr = s.isPhys() ? state.phys[s] : PhysExpr{};

  if (!def.base.isValid()) {
    def = src;
    def.expr = expr;
    state.virt_seen[d] = true;

    FTRACE(3, "      {} = {}\n", show(d), show(def));
    return;
  }
  assertx(def.base == src.base &&
          def.disp == src.disp);

  if (def != src) {
    def = DefInfo{};
    state.virt_invalid[d] = true;
    FTRACE(3, "      kill {}\n", show(d));
  }
}

/*
 * Analyze an instruction which performs `d := s [+ disp]'.
 */
void analyze_virt_copy(Env& env, RegState& state, Vreg d, Vreg s) {
  if (!d.isVirt()) return;
  merge_def_info(env, state, d, DefInfo { s, 0, s.isVirt() ? s : Vreg{} });
}
void analyze_virt_disp(Env& env, RegState& state,
                       Vreg d, Vreg s, int32_t disp) {
  if (!d.isVirt()) return;
  merge_def_info(env, state, d, DefInfo { s, disp });
}

#define VASM_ADDS \
      V(addli)    \
      V(addqi)
#define VASM_SUBS \
      V(subli)    \
      V(subqi)

/*
 * Analyze the virtual defs of `inst'.
 */
void analyze_inst_virtual(Env& env, RegState& state, const Vinstr& inst) {
  switch (inst.op) {
    case Vinstr::copy:
      return analyze_virt_copy(env, state, inst.copy_.d, inst.copy_.s);

    case Vinstr::copyargs: {
      auto const& s = env.unit.tuples[inst.copyargs_.s];
      auto const& d = env.unit.tuples[inst.copyargs_.d];
      assertx(s.size() == d.size());
      for (size_t i = 0; i < s.size(); ++i) {
        analyze_virt_copy(env, state, d[i], s[i]);
      }
      return;
    }

    case Vinstr::lea:
    {
      auto const& i = inst.lea_;
      if (i.s.seg == Segment::DS && i.s.index == InvalidReg) {
        analyze_virt_disp(env, state, i.d, i.s.base, i.s.disp);
      }
      return;
    }

#define V(add)          \
      case Vinstr::add: \
        return analyze_virt_disp(env, state, inst.add##_.d, \
                                 inst.add##_.s1, inst.add##_.s0.l());
      VASM_ADDS
#undef V
#define V(sub)          \
      case Vinstr::sub: \
        return analyze_virt_disp(env, state, inst.sub##_.d, \
                                 inst.sub##_.s1, -inst.sub##_.s0.l());
      VASM_SUBS
#undef V

    default: break;
  }
}

/*
 * Analyze the physical defs of `inst'.
 */
void analyze_inst_physical(Env& env, RegState& state,
                           const Vinstr& inst, const Vinstr* next) {
  auto const done = [&] {
    switch (inst.op) {
      case Vinstr::copy:
        return analyze_phys_copy(env, state, inst.copy_.d, inst.copy_.s);

      case Vinstr::lea:
      {
        auto const& i = inst.lea_;
        return i.s.seg == Segment::DS &&
               i.s.index == InvalidReg &&
               analyze_phys_disp(env, state, i.d, i.s.base, i.s.disp);
      }

#define V(add)          \
      case Vinstr::add: \
        return analyze_phys_disp(env, state, inst.add##_.d, \
                                 inst.add##_.s1, inst.add##_.s0.l());
      VASM_ADDS
#undef V
#define V(sub)          \
      case Vinstr::sub: \
        return analyze_phys_disp(env, state, inst.sub##_.d, \
                                 inst.sub##_.s1, -inst.sub##_.s0.l());
      VASM_SUBS
#undef V

      default: break;
    }
    return false;
  }();
  if (done) return;

  for_all_defs(env, inst, [&] (Vreg d) {
    return analyze_phys_def(env, state, d);
  });
}

#undef VASM_SUBS
#undef VASM_ADDS

/*
 * Toplevel def analysis pass.
 */
void analyze_defs(Env& env) {
  FTRACE(1, "analyze_defs -----------------------------------------\n");

  initialize_reg_states(env);

  auto workQ = dataflow_worklist<RpoID>(env.unit.blocks.size());
  workQ.push(RpoID{0});

  do {
    auto const b = env.rpo_blocks[workQ.pop()];
    FTRACE(1, "{}:\n", b);

    auto& code = env.unit.blocks[b].code;
    auto state = env.block_states[b];

    for (size_t i = 0, n = code.size(); i < n; ++i) {
      auto& inst = code[i];
      FTRACE(2, "    {}\n", show(env.unit, inst));

      auto const next_inst = i != n - 1 ? &code[i + 1] : nullptr;
      analyze_inst_virtual(env, state, inst);
      analyze_inst_physical(env, state, inst, next_inst);
    }

    for (auto const s : succs(code.back())) {
      FTRACE(4, "  -> {}\n", s);
      auto& succ_state = env.block_states[s];
      if (merge_into(succ_state, state)) workQ.push(env.block_to_rpo[s]);
    }
  } while (!workQ.empty());

  FTRACE(5, "\nfixed point:\n{}\n", show_fixed_point(env));
}

///////////////////////////////////////////////////////////////////////////////

/*
 * Chase the sources of `def', recursively folding the "root" DefInfos into
 * their dependent defs.
 */
void flatten_impl(Env& env, DefInfo& def) {
  auto const s = def.base;
  if (!s.isVirt()) return;

  auto& src = env.defs[s];
  flatten_impl(env, src);

  if (def.copy.isValid()) {
    assertx(def.copy.isVirt());
    while (true) {
      auto const& other = env.defs[def.copy];
      if (!other.copy.isValid() || other.copy.isPhys()) break;
      def.copy = other.copy;
    }
  }

  if (!src.base.isVirt()) return;
  def.base = src.base;
  def.disp += src.disp;
}

/*
 * Chase sources of Vregs in `defs', recursively folding the "root" DefInfos
 * into their dependent defs.
 *
 * This routine does not fold physical defs, in case they end up being unusable
 * due to mismatched PhysExprs.
 */
void flatten_def_infos(Env& env) {
  for (auto& def : env.defs) flatten_impl(env, def);
}

///////////////////////////////////////////////////////////////////////////////

/*
 * Call `f(def)' if `r' can be rewritten as `def' at the program point given by
 * `state'.
 */
template<class F>
void if_rewritable(const Env& env, const RegState& state, Vreg r, F f) {
  if (!r.isVirt()) return;

  auto const& definition = env.defs[r];
  if (!definition.base.isValid()) return;

  auto const try_phys_rewrite = [&] (DefInfo def) {
    // We can't fold defs relative to unreserved physical registers.
    if (!def.base.isPhys()) return false;

    // If we don't know anything about the physical register's value, we can't
    // do any rewriting.
    if (def.expr == PhysExpr{}) return false;

    // At this point, we know that `r' is defined relative to some physical
    // register `def.base' which is statically derivable from the value of some
    // physical register `def.expr.base' at entry to the program.  We want to
    // find another register that is defined relative to that same on-entry
    // value at the current program point, given by `state'.
    //
    // We could in theory use any physical register or any virtual register
    // whose def dominates the current point.  Instead, we only try the two we
    // mentioned: `def.base' and `def.expr.base', based on the assumption that
    // physical registers whose values are known relative to on-entry values
    // stay relative to those values.
    auto const try_rewrite = [&] (PhysReg s) {
      auto const& cur = state.phys[s];
      if (def.expr.base != cur.base) {
        if (cur == PhysExpr{}) {
          FTRACE(4, "      incompatible: {} =/> {} + ?? (base unknown)\n",
                 show(r), show(s));
        } else {
          FTRACE(4, "      incompatible: {} =/> {} + {} "
                    "(at def: {}, currently: {})\n",
                 show(r), show(s), def.disp - (cur.disp - def.expr.disp),
                 show(def.expr), show(cur));
        }
        return false;
      }

      // We need to subtract out the change in displacement of `s' relative to
      // `cur.base' from the site of the def until now.  Or, algebraically:
      //
      //    r := s_def + def.disp
      //    s_def := cur.base + def.expr.disp
      //    s_cur := cur.base + cur.disp
      //
      //    s_def = (cur.base + cur.disp) - cur.disp + def.expr.disp
      //    r = s_cur - (cur.disp - def.expr.disp) + def.disp
      def.base = s;
      def.disp -= (cur.disp - def.expr.disp);
      f(def);
      return true;
    };

    return try_rewrite(def.base) ||
           try_rewrite(def.expr.base);
  };

  if (definition.base.isPhys()) {
    try_phys_rewrite(definition);
    return;
  }
  assertx(definition.base.isVirt());

  auto const& src = env.defs[definition.base];
  // The flatten_def_infos() pass should have folded chains of virtual defs.
  assertx(!src.base.isVirt());

  if (src.base.isPhys()) {
    auto folded = src;
    folded.disp += definition.disp;

    // Try rewriting to the physical `src`; but even if we can't, we can still
    // just rewrite to `definition`.
    if (try_phys_rewrite(folded)) return;
  }

  f(definition);
}

/*
 * Visitor for rewriting Vreg uses, replacing them with the expressions for
 * their defs.
 */
struct OptVisit {
  const Env& env;
  const RegState& state;

  template<class T> void imm(T&) {}
  template<class T> void across(T& t) { use(t); }
  template<class T, class H> void useHint(T& t, H&) { use(t); }
  template<class T, class H> void defHint(T& t, H&) { def(t); }
  template<class T> void def(T&) {}

  void use(RegSet) {}
  void use(VregSF) {}
  void use(VcallArgsId) {}
  void use(Vreg128) {}

  void use(Vtuple t) { for (auto& reg : env.unit.tuples[t]) use(reg); }

  void use(Vptr& ptr) {
    // Rewrite memory operands that are based on registers we've copied or
    // lea'd off of other registers.
    if (ptr.seg != Segment::DS) return;
    if_rewritable(env, state, ptr.base, [&] (const DefInfo& def) {
      if (arch::any<arch::ARM>()) {
        // After lowering, only [base, index lsl #scale] and [base, #imm]
        // are allowed where the range of #imm is [-256 .. 255]
        assertx(ptr.base.isValid());
        auto disp = ptr.disp + def.disp;
        if (ptr.index.isValid()) {
          if (disp != 0) return;
        } else {
          if (disp < -256 || disp > 255) return;
        }
      }
      FTRACE(2, "      rewrite: {} => {}\n", show(ptr.base), show(def));
      ptr.base = def.base;
      ptr.disp += def.disp;
    });
    if_rewritable(env, state, ptr.index, [&] (const DefInfo& def) {
      if (arch::any<arch::ARM>()) return;
      auto const newDisp =
        static_cast<int64_t>(ptr.disp) + ptr.scale * def.disp;
      if (!deltaFits(newDisp, sz::dword)) return;
      FTRACE(2, "      rewrite: {} => {}\n", show(ptr.index), show(def));
      ptr.index = def.base;
      ptr.disp = newDisp;
    });
  }

  template<class T>
  typename std::enable_if<
    std::is_same<Vreg,T>::value ||
    std::is_same<Vreg8,T>::value ||
    std::is_same<Vreg16,T>::value ||
    std::is_same<Vreg32,T>::value ||
    std::is_same<Vreg64,T>::value ||
    std::is_same<VregDbl,T>::value
  >::type use(T& reg) {
    // Rewrite to another register if it's just a copy.
    if_rewritable(env, state, reg, [&] (const DefInfo& def) {
      // If the displacement is zero, just use the base
      // register. Otherwise, we might a copy (which isn't necessarily
      // as good as base). Use that instead.
      auto const d = (def.disp != 0) ? def.copy : def.base;
      if (!d.isValid()) return;
      FTRACE(2, "      rewrite: {} => {}\n", show(reg), show(d));
      reg = d;
    });
  }
};

/*
 * Rewrite a copy{} as an lea{} if possible.
 *
 * Note that if the copy could have been rewritten as a different copy, the
 * above visitor would have taken care of it.
 */
void optimize_copy(const Env& env, const RegState& state, Vinstr& inst) {
  auto& copy = inst.copy_;
  if_rewritable(env, state, copy.s, [&] (const DefInfo& def) {
    if (def.disp == 0) return;
    FTRACE(2, "      copy => lea {}\n", show(def));
    inst = lea{def.base[def.disp], copy.d};
  });
}

/*
 * Rewrite the srcs of `inst' as the expressions used to def them.
 */
void optimize_inst(const Env& env, const RegState& state, Vinstr& inst) {
  // For specialized iterators, we'd like to use a physical single register for
  // the position. On exit traces, we intentionally recompute the old position
  // in order to avoid extending Vreg lifetimes. Don't overoptimize this case.
  if (inst.origin != nullptr && inst.origin->is(StIterPos)) return;

  auto visit = OptVisit { env, state };
  visitOperands(inst, visit);

  switch (inst.op) {
    case Vinstr::copy:
      optimize_copy(env, state, inst);
      break;
    default: break;
  }
}

/*
 * Post-analysis expression-rewriting pass.
 */
void optimize(Env& env) {
  FTRACE(1, "\noptimize ---------------------------------------------\n");

  for (auto const& b : env.rpo_blocks) {
    FTRACE(1, "{}:\n", b);
    auto& code = env.unit.blocks[b].code;

    auto state = env.block_states[b];

    for (auto it = code.begin(); it != code.end(); ++it) {
      auto& inst = *it;
      FTRACE(2, "    {}\n", show(env.unit, inst));

      auto const next_it = std::next(it);
      auto const next_inst = next_it != code.end() ? &*next_it : nullptr;

      optimize_inst(env, state, inst);
      analyze_inst_physical(env, state, inst, next_inst);
    }
  }
}

///////////////////////////////////////////////////////////////////////////////

}

/*
 * This pass performs straight-forward copy propagation, along with stateful
 * copy propagation of values through physical registers.  (Tracking the values
 * of physical registers requires dataflow analysis, because they do not have
 * single definitions.)
 *
 * The pass also tracks registers defined via lea instructions, and it knows
 * when a register holds a value that is the same as another register plus some
 * offset.  It then folds offsets in memory operands to try to require fewer
 * registers.  The main motivation for this is to generally eliminate the need
 * for a separate stack pointer (the result of HHIR's DefFrameRelSP instruction,
 * which will just be an lea off of the rvmfp() physical register).
 */
void optimizeCopies(Vunit& unit, const Abi& abi) {
  Timer timer(Timer::vasm_copy, unit.log_entry);
  VpassTracer tracer{&unit, Trace::vasm_copy, "vasm-copy"};
  Env env { unit, abi };
  analyze_defs(env);
  flatten_def_infos(env);
  optimize(env);
}

///////////////////////////////////////////////////////////////////////////////

}
