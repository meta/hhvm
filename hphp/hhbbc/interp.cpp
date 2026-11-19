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
#include "hphp/hhbbc/interp.h"

#include <algorithm>
#include <vector>
#include <string>

#include <folly/gen/Base.h>
#include <fmt/core.h>
#include <folly/gen/String.h>

#include "hphp/util/configs/eval.h"
#include "hphp/util/hash-set.h"
#include "hphp/util/trace.h"
#include "hphp/runtime/base/collections.h"
#include "hphp/runtime/base/static-string-table.h"
#include "hphp/runtime/base/tv-arith.h"
#include "hphp/runtime/base/tv-comparisons.h"
#include "hphp/runtime/base/tv-conversions.h"
#include "hphp/runtime/base/type-structure.h"
#include "hphp/runtime/base/type-structure-helpers.h"
#include "hphp/runtime/base/type-structure-helpers-defs.h"
#include "hphp/runtime/vm/runtime.h"

#include "hphp/runtime/ext/hh/ext_hh.h"

#include "hphp/hhbbc/analyze.h"
#include "hphp/hhbbc/bc.h"
#include "hphp/hhbbc/cfg.h"
#include "hphp/hhbbc/class-util.h"
#include "hphp/hhbbc/eval-cell.h"
#include "hphp/hhbbc/index.h"
#include "hphp/hhbbc/interp-state.h"
#include "hphp/hhbbc/optimize.h"
#include "hphp/hhbbc/representation.h"
#include "hphp/hhbbc/type-ops.h"
#include "hphp/hhbbc/type-structure.h"
#include "hphp/hhbbc/type-system.h"
#include "hphp/hhbbc/wide-func.h"

#include "hphp/hhbbc/interp-internal.h"

namespace HPHP::HHBBC {

//////////////////////////////////////////////////////////////////////

namespace {

const StaticString s_MethCallerHelper("__SystemLib\\MethCallerHelper");
const StaticString s_PHP_Incomplete_Class("__PHP_Incomplete_Class");
const StaticString s_ConstMap("ConstMap");
const StaticString s_ConstSet("ConstSet");
const StaticString s_ConstVector("ConstVector");
const StaticString s_Iterator("HH\\Iterator");
const StaticString s_IMemoizeParam("HH\\IMemoizeParam");
const StaticString s_getInstanceKey("getInstanceKey");
const StaticString s_Closure("Closure");
const StaticString s_this(annotTypeName(AnnotType::This));
const StaticString s_AsyncGenerator("HH\\AsyncGenerator");
const StaticString s_Generator("Generator");

bool poppable(Op op) {
  switch (op) {
    case Op::Dup:
    case Op::Null:
    case Op::False:
    case Op::True:
    case Op::Int:
    case Op::Double:
    case Op::String:
    case Op::Vec:
    case Op::Dict:
    case Op::Keyset:
    case Op::NewDictArray:
    case Op::NewCol:
    case Op::LazyClass:
    case Op::EnumClassLabel:
      return true;
    default:
      return false;
  }
}

bool pushes_immediate(Op op) {
  switch (op) {
    case Op::Null:
    case Op::False:
    case Op::True:
    case Op::Int:
    case Op::Double:
    case Op::String:
    case Op::Vec:
    case Op::Dict:
    case Op::Keyset:
    case Op::LazyClass:
    case Op::EnumClassLabel:
      return true;
    default:
      return false;
  }
}

void interpStep(ISS& env, const Bytecode& bc);

void record(ISS& env, const Bytecode& bc) {
  if (bc.srcLoc != env.srcLoc) {
    Bytecode tmp = bc;
    tmp.srcLoc = env.srcLoc;
    return record(env, tmp);
  }

  if (env.replacedBcs.empty() &&
      env.unchangedBcs < env.blk.hhbcs.size() &&
      bc == env.blk.hhbcs[env.unchangedBcs]) {
    env.unchangedBcs++;
    return;
  }

  ITRACE(2, "  => {}\n", show(*env.ctx.func, bc));
  env.replacedBcs.push_back(bc);
}

// The number of pops as seen by interp.
uint32_t numPop(const Bytecode& bc) {
  if (bc.op == Op::CGetL2) return 1;
  return bc.numPop();
}

// The number of pushes as seen by interp.
uint32_t numPush(const Bytecode& bc) {
  if (bc.op == Op::CGetL2) return 2;
  return bc.numPush();
}

void reprocess(ISS& env) {
  FTRACE(2, "    reprocess\n");
  env.reprocess = true;
}

ArrayData** add_elem_array(ISS& env) {
  auto const idx = env.trackedElems.back().idx;
  if (idx < env.unchangedBcs) {
    auto const DEBUG_ONLY& bc = env.blk.hhbcs[idx];
    assertx(bc.op == Op::Concat);
    return nullptr;
  }
  assertx(idx >= env.unchangedBcs);
  auto& bc = env.replacedBcs[idx - env.unchangedBcs];
  auto arr = [&] () -> const ArrayData** {
    switch (bc.op) {
      case Op::Vec:     return &bc.Vec.arr1;
      case Op::Dict:    return &bc.Dict.arr1;
      case Op::Keyset:  return &bc.Keyset.arr1;
      case Op::Concat:  return nullptr;
      default:          not_reached();
    }
  }();
  return const_cast<ArrayData**>(arr);
}

bool start_add_elem(ISS& env, Type& ty, Op op) {
  auto value = tvNonStatic(ty);
  if (!value || !isArrayLikeType(value->m_type)) return false;

  if (op == Op::AddElemC) {
    reduce(env, bc::PopC {}, bc::PopC {}, bc::PopC {});
  } else {
    reduce(env, bc::PopC {}, bc::PopC {});
  }
  env.trackedElems.emplace_back(
    env.state.stack.size(),
    env.unchangedBcs + env.replacedBcs.size()
  );

  auto const arr = value->m_data.parr;
  env.replacedBcs.push_back(
    [&] () -> Bytecode {
      if (arr->isVecType())    return bc::Vec { arr };
      if (arr->isDictType())   return bc::Dict { arr };
      if (arr->isKeysetType()) return bc::Keyset { arr };
      always_assert(false);
    }()
  );
  env.replacedBcs.back().srcLoc = env.srcLoc;
  ITRACE(2, "(addelem* -> {}\n",
         show(*env.ctx.func, env.replacedBcs.back()));
  push(env, std::move(ty));
  effect_free(env);
  return true;
}

/*
 * Alter the saved add_elem array in a way that preserves its provenance tag
 * or adds a new one if applicable (i.e. the array is a vec or dict)
 *
 * The `mutate` parameter should be callable with an ArrayData** pointing to the
 * add_elem array cached in the interp state and should write to it directly.
 */
template <typename Fn>
bool mutate_add_elem_array(ISS& env, Fn&& mutate) {
  auto const arr = add_elem_array(env);
  if (!arr) return false;
  mutate(arr);
  return true;
}

void finish_tracked_elem(ISS& env) {
  auto const arr = add_elem_array(env);
  env.trackedElems.pop_back();
  if (arr) {
    ArrayData::GetScalarArray(arr);
    reprocess(env);
  }
}

void finish_tracked_elems(ISS& env, size_t depth) {
  while (!env.trackedElems.empty() && env.trackedElems.back().depth >= depth) {
    finish_tracked_elem(env);
  }
}

uint32_t id_from_slot(ISS& env, int slot) {
  auto const id = (env.state.stack.end() - (slot + 1))->id;
  assertx(id == StackElem::NoId ||
          id < env.unchangedBcs + env.replacedBcs.size());
  return id;
}

const Bytecode* op_from_id(ISS& env, uint32_t id) {
  if (id == StackElem::NoId) return nullptr;
  if (id < env.unchangedBcs) return &env.blk.hhbcs[id];
  auto const off = id - env.unchangedBcs;
  assertx(off < env.replacedBcs.size());
  return &env.replacedBcs[off];
}

void ensure_mutable(ISS& env, uint32_t id) {
  if (id < env.unchangedBcs) {
    auto const delta = env.unchangedBcs - id;
    env.replacedBcs.resize(env.replacedBcs.size() + delta);
    for (auto i = env.replacedBcs.size(); i-- > delta; ) {
      env.replacedBcs[i] = std::move(env.replacedBcs[i - delta]);
    }
    for (auto i = 0; i < delta; i++) {
      env.replacedBcs[i] = env.blk.hhbcs[id + i];
    }
    env.unchangedBcs = id;
  }
}

/*
 * Turn the instruction that wrote the slot'th element from the top of
 * the stack into a Nop, adjusting the stack appropriately. If its the
 * previous instruction, just rewind.
 */
int kill_by_slot(ISS& env, int slot) {
  assertx(!env.undo);
  assertx(can_rewind(env));
  auto const id = id_from_slot(env, slot);
  assertx(id != StackElem::NoId);
  auto const sz = env.state.stack.size();
  // if its the last bytecode we processed, we can rewind and avoid
  // the reprocess overhead.
  if (id == env.unchangedBcs + env.replacedBcs.size() - 1) {
    rewind(env, 1);
    return env.state.stack.size() - sz;
  }
  ensure_mutable(env, id);
  auto& bc = env.replacedBcs[id - env.unchangedBcs];
  auto const pop = numPop(bc);
  auto const push = numPush(bc);
  ITRACE(2, "kill_by_slot: slot={}, id={}, was {}\n",
         slot, id, show(*env.ctx.func, bc));
  bc = bc_with_loc(bc.srcLoc, bc::Nop {});
  env.state.stack.kill(pop, push, id);
  reprocess(env);
  return env.state.stack.size() - sz;
}

/*
 * Check whether an instruction can be inserted immediately after the
 * slot'th stack entry was written. This is only possible if slot was
 * the last thing written by the instruction that wrote it (ie some
 * bytecodes push more than one value - there's no way to insert a
 * bytecode that will write *between* those values on the stack).
 */
bool can_insert_after_slot(ISS& env, int slot) {
  auto const it = env.state.stack.end() - (slot + 1);
  if (it->id == StackElem::NoId) return false;
  if (auto const next = it.next_elem(1)) {
    return next->id != it->id;
  }
  return true;
}

/*
 * Insert a sequence of bytecodes after the instruction that wrote the
 * slot'th element from the top of the stack.
 *
 * The entire sequence pops numPop, and pushes numPush stack
 * elements. Only the last bytecode can push anything onto the stack,
 * and the types it pushes are pointed to by types (if you have more
 * than one bytecode that pushes, call this more than once).
 */
void insert_after_slot(ISS& env, int slot,
                       int numPop, int numPush, const Type* types,
                       const BytecodeVec& bcs) {
  assertx(can_insert_after_slot(env, slot));
  assertx(!env.undo);
  auto const id = id_from_slot(env, slot);
  assertx(id != StackElem::NoId);
  ensure_mutable(env, id + 1);
  env.state.stack.insert_after(numPop, numPush, types, bcs.size(), id);
  env.replacedBcs.insert(env.replacedBcs.begin() + (id + 1 - env.unchangedBcs),
                         bcs.begin(), bcs.end());
  using namespace folly::gen;
  ITRACE(2, "insert_after_slot: slot={}, id={}  [{}]\n",
         slot, id,
         from(bcs) |
         map([&] (const Bytecode& bc) { return show(*env.ctx.func, bc); }) |
         unsplit<std::string>(", "));
}

Bytecode& mutate_last_op(ISS& env) {
  assertx(will_reduce(env));

  if (!env.replacedBcs.size()) {
    assertx(env.unchangedBcs);
    env.replacedBcs.push_back(env.blk.hhbcs[--env.unchangedBcs]);
  }
  return env.replacedBcs.back();
}

/*
 * Can be used to replace one op with another when rewind/reduce isn't
 * safe (eg to change a SetL to a PopL - its not safe to rewind/reduce
 * because the SetL changed both the Type and the equiv of its local).
 */
void replace_last_op(ISS& env, Bytecode&& bc) {
  auto& last = mutate_last_op(env);
  auto const newPush = numPush(bc);
  auto const oldPush = numPush(last);
  auto const newPops = numPop(bc);
  auto const oldPops = numPop(last);

  assertx(newPush <= oldPush);
  assertx(newPops <= oldPops);

  if (newPush != oldPush || newPops != oldPops) {
    assertx(!env.undo);
    env.state.stack.rewind(oldPops - newPops, oldPush - newPush);
  }
  ITRACE(2, "(replace: {}->{}\n",
         show(*env.ctx.func, last), show(*env.ctx.func, bc));
  last = bc_with_loc(last.srcLoc, bc);
}

template<typename TOp>
bool reduceRet(ISS& env, const TOp& op, HPHP::VerifyRetKind kind) {
  static_assert(false);
}

template<>
bool reduceRet(ISS& env, const bc::RetC&, HPHP::VerifyRetKind kind) {
  reduce(env, bc::RetC {kind});
  return true;
}

template<>
bool reduceRet(ISS& env, const bc::RetM& op, HPHP::VerifyRetKind kind) {
  reduce(env, bc::RetM {op.arg1, kind});
  return true;
}

template<>
bool reduceRet(ISS& env, const bc::VerifyOutType&, HPHP::VerifyRetKind kind) {
  assertx(kind == VerifyRetKind::None);
  reduce(env);
  return true;
}

struct VerifyResult {
  Type type;
  bool effectFree;
  bool coerced;
};

template<typename TOp>
Optional<VerifyResult> verifyRetNonNullImpl(ISS& env, const TOp& op, const Type& retTy) {
  if (!retTy.couldBe(BInitNull)) {
    reduceRet(env, op, VerifyRetKind::None);
    return std::nullopt;
  }
  auto const& constraints = env.ctx.func->retTypeConstraints;
  auto hasNonNullableType = std::any_of(
    constraints.range().begin(),
    constraints.range().end(),
    [&](const TypeConstraint& tc) {
      return !tc.isSoft() && !tc.isNullable();
    }
  );

  auto refined = hasNonNullableType
    ? (retTy.subtypeOf(BInitNull) ? TBottom : unopt(retTy))
    : retTy;
  return VerifyResult(std::move(refined), false, false);
}

template<typename TOp>
Optional<VerifyResult> verifyRetAllImpl(ISS& env, const TypeIntersectionConstraint& tcs,
                                   bool reduce_nullonly, const TOp& op, const Type& stackT) {
  assertx(!tcs.isTop());

  auto refined = TInitCell;
  auto remove = true;
  auto effectFree = true;
  auto coerced = false;
  auto nullonly = reduce_nullonly &&
    stackT.couldBe(BInitNull) &&
    !stackT.subtypeOf(BInitNull);

  for (auto const& tc : tcs.range()) {
    auto const type = lookup_constraint(env.index, env.ctx, tc, stackT);
    if (stackT.moreRefined(type.lower)) {
      refined = intersection_of(std::move(refined), stackT);
      continue;
    }

    if (!stackT.couldBe(type.upper)) {
      return VerifyResult(TBottom, false, false);
    }

    remove = false;
    if (nullonly) {
      nullonly = unopt(stackT).moreRefined(type.lower);
    }

    auto result = intersection_of(stackT, type.upper);
    if (type.coerceClassToString == TriBool::Yes) {
      assertx(!type.lower.couldBe(BCls | BLazyCls));
      assertx(type.upper.couldBe(BStr | BCls | BLazyCls));
      if (result.couldBe(BCls | BLazyCls)) {
        result = promote_classish(std::move(result));
        coerced = true;
        if (effectFree && (Cfg::Eval::ClassStringHintNoticesSampleRate > 0 ||
                           !promote_classish(stackT).moreRefined(type.lower))) {
          effectFree = false;
        }
      } else {
        effectFree = false;
      }
    } else if (type.coerceClassToString == TriBool::Maybe) {
      if (result.couldBe(BCls | BLazyCls)) result |= TSStr;
      effectFree = false;
    } else {
      effectFree = false;
    }

    refined = intersection_of(std::move(refined), result);
    if (refined.is(BBottom)) {
      return VerifyResult(TBottom, false, coerced);
    }
  }

  if (remove) {
    assertx(effectFree);
    assertx(!coerced);
    reduceRet(env, op, VerifyRetKind::None);
    return std::nullopt;
  }

  // In cases where stackT includes InitNull, but would pass the
  // type-constraint if it was not InitNull, we can lower to a
  // non-null check.
  if (nullonly) {
    reduceRet(env, op, VerifyRetKind::NonNull);
    return std::nullopt;
  }

  return VerifyResult(std::move(refined), effectFree, coerced);
}

template <typename Op>
Optional<VerifyResult> verifyRetImpl(ISS& env, HPHP::VerifyRetKind kind, const Op& op, const Type& type) {
  switch (kind) {
    case HPHP::VerifyRetKind::All:
      return verifyRetAllImpl(env, env.ctx.func->retTypeConstraints, true, op, type);
    case HPHP::VerifyRetKind::NonNull:
      return verifyRetNonNullImpl(env, op, type);
    case HPHP::VerifyRetKind::None:
      return VerifyResult(type, true, false);
  }
}

}

//////////////////////////////////////////////////////////////////////

const Bytecode* op_from_slot(ISS& env, int slot, int prev /* = 0 */) {
  if (!will_reduce(env)) return nullptr;
  auto const id = id_from_slot(env, slot);
  if (id == StackElem::NoId) return nullptr;
  if (id < prev) return nullptr;
  return op_from_id(env, id - prev);
}

const Bytecode* last_op(ISS& env, int idx /* = 0 */) {
  if (!will_reduce(env)) return nullptr;

  if (env.replacedBcs.size() > idx) {
    return &env.replacedBcs[env.replacedBcs.size() - idx - 1];
  }

  idx -= env.replacedBcs.size();
  if (env.unchangedBcs > idx) {
    return &env.blk.hhbcs[env.unchangedBcs - idx - 1];
  }
  return nullptr;
}

/*
 * Assuming bc was just interped, rewind to the state immediately
 * before it was interped.
 *
 * This is rarely what you want. Its used for constprop, where the
 * bytecode has been interped, but not yet committed to the bytecode
 * stream. We want to undo its effects, the spit out pops for its
 * inputs, and commit a constant-generating bytecode.
 */
void rewind(ISS& env, const Bytecode& bc) {
  assertx(!env.undo);
  assertx(can_rewind(env));
  ITRACE(2, "(rewind: {}\n", show(*env.ctx.func, bc));
  env.state.stack.rewind(numPop(bc), numPush(bc));
  env.flags.usedParams.reset();
  env.flags.canSuspend = false;
}

/*
 * Used for peephole opts. Will undo the *stack* effects of the last n
 * committed byte codes, and remove them from the bytecode stream, in
 * preparation for writing out an optimized replacement sequence.
 *
 * WARNING: Does not undo other changes to state, such as local types,
 * local equivalency, and thisType. Take care when rewinding such
 * things.
 */
void rewind(ISS& env, int n) {
  assertx(n);
  assertx(!env.undo);
  assertx(can_rewind(env));
  while (env.replacedBcs.size()) {
    rewind(env, env.replacedBcs.back());
    env.replacedBcs.pop_back();
    if (!--n) return;
  }
  while (n--) {
    rewind(env, env.blk.hhbcs[--env.unchangedBcs]);
  }
}

void impl_vec(ISS& env, bool reduce, BytecodeVec&& bcs) {
  if (!will_reduce(env)) reduce = false;

  if (reduce) {
    using namespace folly::gen;
    ITRACE(2, "(reduce: {}\n",
           from(bcs) |
           map([&] (const Bytecode& bc) { return show(*env.ctx.func, bc); }) |
           unsplit<std::string>(", "));
    if (bcs.size()) {
      auto ef = !env.flags.reduced || env.flags.effectFree;
      auto usedParams = env.flags.usedParams;
      auto canSuspend = env.flags.canSuspend;
      Trace::Indent _;
      for (auto const& bc : bcs) {
        assertx(
          env.flags.jmpDest == NoBlockId &&
          "you can't use impl with branching opcodes before last position"
        );
        interpStep(env, bc);
        usedParams |= env.flags.usedParams;
        canSuspend |= env.flags.canSuspend;
        if (!env.flags.effectFree) ef = false;
        if (env.state.unreachable || env.flags.jmpDest != NoBlockId) break;
      }
      env.flags.effectFree = ef;
      env.flags.usedParams = usedParams;
      env.flags.canSuspend = canSuspend;
    } else if (!env.flags.reduced) {
      effect_free(env);
    }
    env.flags.reduced = true;
    return;
  }

  env.analyzeDepth++;
  SCOPE_EXIT { env.analyzeDepth--; };

  // We should be at the start of a bytecode.
  assertx(env.flags.wasPEI);
  assertx(!env.flags.canConstProp);
  assertx(!env.flags.effectFree);
  assertx(env.flags.usedParams.none());
  assertx(!env.flags.canSuspend);

  env.flags.wasPEI          = false;
  env.flags.canConstProp    = true;
  env.flags.effectFree      = true;
  env.flags.usedParams.reset();
  env.flags.canSuspend      = false;

  for (auto const& bc : bcs) {
    assertx(env.flags.jmpDest == NoBlockId &&
           "you can't use impl with branching opcodes before last position");

    auto const wasPEI = env.flags.wasPEI;
    auto const canConstProp = env.flags.canConstProp;
    auto const effectFree = env.flags.effectFree;
    auto const usedParams = env.flags.usedParams;
    auto const canSuspend = env.flags.canSuspend;

    ITRACE(3, "    (impl {}\n", show(*env.ctx.func, bc));
    env.flags.wasPEI          = true;
    env.flags.canConstProp    = false;
    env.flags.effectFree      = false;
    env.flags.usedParams.reset();
    env.flags.canSuspend      = false;
    default_dispatch(env, bc);

    if (env.flags.canConstProp) {
      [&] {
        if (env.flags.effectFree && !env.flags.wasPEI) return;
        auto stk = env.state.stack.end();
        for (auto i = bc.numPush(); i--; ) {
          --stk;
          if (!is_scalar(stk->type)) return;
        }
        env.flags.effectFree = true;
        env.flags.wasPEI = false;
      }();
    }

    // If any of the opcodes in the impl list said they could throw,
    // then the whole thing could throw.
    env.flags.wasPEI |= wasPEI;
    env.flags.canConstProp &= canConstProp;
    env.flags.effectFree &= effectFree;
    env.flags.usedParams |= usedParams;
    env.flags.canSuspend |= canSuspend;
    if (env.state.unreachable || env.flags.jmpDest != NoBlockId) break;
  }
}

LocalId equivLocalRange(ISS& env, const LocalRange& range) {
  auto bestRange = range.first;
  auto equivFirst = findLocEquiv(env, range.first);
  if (equivFirst == NoLocalId) return bestRange;
  do {
    if (equivFirst < bestRange) {
      auto equivRange = [&] {
        // local equivalency includes differing by Uninit, so we need
        // to check the types.
        if (!equal(peekLocRaw(env, equivFirst), peekLocRaw(env, range.first))) {
          return false;
        }

        for (uint32_t i = 1; i < range.count; ++i) {
          if (!locsAreEquiv(env, equivFirst + i, range.first + i) ||
              !equal(peekLocRaw(env, equivFirst + i),
                     peekLocRaw(env, range.first + i))) {
            return false;
          }
        }

        return true;
      }();

      if (equivRange) {
        bestRange = equivFirst;
      }
    }
    equivFirst = findLocEquiv(env, equivFirst);
    assertx(equivFirst != NoLocalId);
  } while (equivFirst != range.first);

  return bestRange;
}

SString getNameFromType(const Type& t) {
  if (!t.subtypeOf(BStr) && !t.subtypeOf(BLazyCls)) return nullptr;
  if (is_specialized_string(t)) return sval_of(t);
  if (is_specialized_lazycls(t)) return lazyclsval_of(t);
  return nullptr;
}

//////////////////////////////////////////////////////////////////////

namespace {

/*
 * Very simple check to see if the top level class is reified or not
 */
bool shouldRemoveVerifyTypeTS(ISS& env, SArray ts) {
  if (get_ts_kind(ts) != TypeStructure::Kind::T_unresolved) return false;
  auto const clsName = get_ts_classname(ts);
  auto const lookup = env.index.lookup_class_or_type_alias(clsName);
  if (lookup.cls) {
    return !env.index.resolve_class(*lookup.cls)->couldHaveReifiedGenerics();
  }
  // Type aliases cannot have reified generics
  return lookup.typeAlias;
}

}

//////////////////////////////////////////////////////////////////////

namespace interp_step {

void in(ISS& env, const bc::Nop&)  { reduce(env); }

void in(ISS& env, const bc::PopC&) {
  if (auto const last = last_op(env)) {
    if (poppable(last->op) && can_rewind(env)) {
      rewind(env, 1);
      return reduce(env);
    }
    if (last->op == Op::This) {
      // can't rewind This because it removed null from thisType (so
      // CheckThis at this point is a no-op) - and note that it must
      // have *been* nullable, or we'd have turned it into a
      // `BareThis NeverNull`
      replace_last_op(env, bc::CheckThis {});
      return reduce(env);
    }
    if (last->op == Op::SetL) {
      // can't rewind a SetL because it changes local state
      replace_last_op(env, bc::PopL { last->SetL.loc1 });
      return reduce(env);
    }
    if (last->op == Op::CGetL2 && can_rewind(env)) {
      auto loc = last->CGetL2.nloc1;
      rewind(env, 1);
      return reduce(env, bc::PopC {}, bc::CGetL { loc });
    }
  }

  effect_free(env);
  popC(env);
}

void in(ISS& env, const bc::PopU&) {
  if (auto const last = last_op(env)) {
    if (last->op == Op::NullUninit && can_rewind(env)) {
      rewind(env, 1);
      return reduce(env);
    }
  }
  effect_free(env); popU(env);
}

void in(ISS& env, const bc::PopU2&) {
  effect_free(env);
  auto equiv = topStkEquiv(env);
  auto val = popC(env);
  popU(env);
  push(env, std::move(val), equiv != StackDupId ? equiv : NoLocalId);
}

void in(ISS& env, const bc::Dup& /*op*/) {
  effect_free(env);
  auto equiv = topStkEquiv(env);
  auto val = popC(env);
  push(env, val, equiv);
  push(env, std::move(val), StackDupId);
}

void in(ISS& env, const bc::AssertRATL& op) {
  mayReadLocal(env, op.loc1);
  effect_free(env);
}

void in(ISS& env, const bc::AssertRATStk&) {
  effect_free(env);
}

void in(ISS& env, const bc::CGetCUNop&) {
  effect_free(env);
  auto const t = popCU(env);
  push(env, remove_uninit(t));
}

void in(ISS& env, const bc::Null&) {
  effect_free(env);
  push(env, TInitNull);
}

void in(ISS& env, const bc::NullUninit&) {
  effect_free(env);
  push(env, TUninit);
}

void in(ISS& env, const bc::True&) {
  effect_free(env);
  push(env, TTrue);
}

void in(ISS& env, const bc::False&) {
  effect_free(env);
  push(env, TFalse);
}

void in(ISS& env, const bc::Int& op) {
  effect_free(env);
  push(env, ival(op.arg1));
}

void in(ISS& env, const bc::Double& op) {
  effect_free(env);
  push(env, dval(op.dbl1));
}

void in(ISS& env, const bc::String& op) {
  effect_free(env);
  push(env, sval(op.str1));
}

void in(ISS& env, const bc::Vec& op) {
  assertx(op.arr1->isVecType());
  effect_free(env);
  push(env, vec_val(op.arr1));
}

void in(ISS& env, const bc::Dict& op) {
  assertx(op.arr1->isDictType());
  effect_free(env);
  push(env, dict_val(op.arr1));
}

void in(ISS& env, const bc::Keyset& op) {
  assertx(op.arr1->isKeysetType());
  effect_free(env);
  push(env, keyset_val(op.arr1));
}

void in(ISS& env, const bc::NewDictArray& op) {
  effect_free(env);
  push(env, op.arg1 == 0 ? dict_empty() : some_dict_empty());
}

void in(ISS& env, const bc::NewStructDict& op) {
  auto map = MapElems{};
  for (auto it = op.keys.end(); it != op.keys.begin(); ) {
    map.emplace_front(
      make_tv<KindOfString>(*--it),
      MapElem::SStrKey(popC(env))
    );
  }
  push(env, dict_map(std::move(map)));
  effect_free(env);
  constprop(env);
}

void in(ISS& env, const bc::NewVec& op) {
  auto elems = std::vector<Type>{};
  elems.reserve(op.arg1);
  for (auto i = uint32_t{0}; i < op.arg1; ++i) {
    elems.push_back(std::move(topC(env, op.arg1 - i - 1)));
  }
  discard(env, op.arg1);
  effect_free(env);
  constprop(env);
  push(env, vec(std::move(elems)));
}

void in(ISS& env, const bc::NewKeysetArray& op) {
  assertx(op.arg1 > 0);
  auto map = MapElems{};
  auto ty = TBottom;
  auto useMap = true;
  auto bad = false;
  auto effectful = false;
  /*
   * Read from the bottom of the stack upwards, so the elements are seen in
   * the order they were written rather than reversed.  That order is what
   * decides where a repeated element ends up: a keyset keeps it where it
   * first appeared, and emplace_back leaves the existing entry alone, so the
   * two agree.
   */
  for (auto i = uint32_t{0}; i < op.arg1; ++i) {
    auto [key, promotion] =
      promote_classlike_to_key(topC(env, op.arg1 - i - 1));

    auto const keyValid = key.subtypeOf(BArrKey);
    if (!keyValid) key = intersection_of(std::move(key), TArrKey);
    if (key.is(BBottom)) {
      bad = true;
      useMap = false;
      effectful = true;
    }

    if (useMap) {
      if (auto const v = tv(key)) {
        map.emplace_back(*v, MapElem::KeyFromType(key, key));
      } else {
        useMap = false;
      }
    }

    ty |= std::move(key);
    effectful |= !keyValid || (promotion == Promotion::YesMightThrow);
  }
  discard(env, op.arg1);

  if (!effectful) {
    effect_free(env);
    constprop(env);
  }

  if (useMap) {
    push(env, keyset_map(std::move(map)));
  } else if (!bad) {
    push(env, keyset_n(ty));
  } else {
    assertx(effectful);
    unreachable(env);
    push(env, TBottom);
  }
}

void in(ISS& env, const bc::AddElemC&) {
  auto const v = topC(env, 0);
  auto const [k, promotion] = promote_classlike_to_key(topC(env, 1));
  auto const promoteMayThrow = (promotion == Promotion::YesMightThrow);

  auto inTy = (env.state.stack.end() - 3).unspecialize();
  auto canUndo = false;
  if (env.undo) {
    // Special case: can we undo this AddElemC without explicitly
    // storing the input array? This is desirable because we can (try
    // to) keep the array's ref-count at 1, which will avoid O(N^2)
    // blow-up in long AddElemC chains.
    if (array_like_set_can_be_undone(inTy, k)) {
      canUndo = true;
    } else {
      // Unspecialize modifies the stack location
      env.undo->onStackWrite(env.state.stack.size() - 3, inTy);
    }
  }

  auto outTy = [&] () -> Optional<Type> {
    if (!k.subtypeOf(BArrKey)) return std::nullopt;
    if (inTy.subtypeOf(BDict)) {
      auto const r = array_like_set(std::move(inTy), k, v);
      if (!r.second) return r.first;
    }
    return std::nullopt;
  }();
  // If a set is reversible, we must be able the model the set.
  assertx(IMPLIES(canUndo, outTy.has_value()));

  if (!canUndo && outTy && !promoteMayThrow && will_reduce(env)) {
    if (!env.trackedElems.empty() &&
        env.trackedElems.back().depth + 3 == env.state.stack.size()) {
      auto const handled = [&] {
        if (!k.subtypeOf(BArrKey)) return false;
        auto ktv = tv(k);
        if (!ktv) return false;
        auto vtv = tv(v);
        if (!vtv) return false;
        return mutate_add_elem_array(env, [&](ArrayData** arr) {
          *arr = (*arr)->setMove(*ktv, *vtv);
        });
      }();
      if (handled) {
        if (env.undo) {
          env.undo->onStackWrite(
            env.state.stack.size() - 3,
            std::move((env.state.stack.end() - 3)->type)
          );
        }
        (env.state.stack.end() - 3)->type = std::move(*outTy);
        reduce(env, bc::PopC {}, bc::PopC {});
        ITRACE(2, "(addelem* -> {}\n",
               show(*env.ctx.func,
                    env.replacedBcs[env.trackedElems.back().idx - env.unchangedBcs]));
        return;
      }
    } else {
      if (start_add_elem(env, *outTy, Op::AddElemC)) return;
    }
  }

  // Remove the key and value.
  popC(env);
  popC(env);

  // If this AddElemC is reversible, then suppress the normal undo log
  // recording. All subsequent stack actions will be replaced by a
  // single undo action.
  if (canUndo) env.undo->suppress();
  SCOPE_EXIT { if (canUndo) env.undo->unsuppress(); };

  popC(env);
  finish_tracked_elems(env, env.state.stack.size());

  if (!outTy) {
    assertx(!canUndo);
    return push(env, TInitCell);
  }

  if (outTy->subtypeOf(BBottom)) {
    unreachable(env);
  } else if (!promoteMayThrow) {
    effect_free(env);
    constprop(env);
  }
  push(env, std::move(*outTy));
  if (canUndo) {
    // Finally unsuppress the undo log and record the reversible array
    // set. If we can't undo the set, then the above stack pushes/pops
    // will record the set in the normal way.
    env.undo->unsuppress();
    env.undo->onUndoableArraySet(topC(env), k);
  }
}

void in(ISS& env, const bc::AddNewElemC&) {
  auto v = topC(env);
  auto inTy = (env.state.stack.end() - 2).unspecialize();
  // Unspecialize modifies the stack location
  if (env.undo) env.undo->onStackWrite(env.state.stack.size() - 2, inTy);

  auto outTy = [&] () -> Optional<Type> {
    if (inTy.subtypeOf(BVec | BKeyset)) {
      auto const r = array_like_newelem(std::move(inTy), v);
      if (!r.second) return r.first;
    }
    return std::nullopt;
  }();

  if (outTy && will_reduce(env)) {
    if (!env.trackedElems.empty() &&
        env.trackedElems.back().depth + 2 == env.state.stack.size()) {
      auto const handled = [&] {
        auto vtv = tv(v);
        if (!vtv) return false;
        return mutate_add_elem_array(env, [&](ArrayData** arr) {
          *arr = (*arr)->appendMove(*vtv);
        });
      }();
      if (handled) {
        (env.state.stack.end() - 2)->type = std::move(*outTy);
        reduce(env, bc::PopC {});
        ITRACE(2, "(addelem* -> {}\n",
               show(*env.ctx.func,
                    env.replacedBcs[env.trackedElems.back().idx - env.unchangedBcs]));
        return;
      }
    } else {
      if (start_add_elem(env, *outTy, Op::AddNewElemC)) {
        return;
      }
    }
  }

  discard(env, 2);
  finish_tracked_elems(env, env.state.stack.size());

  if (!outTy) return push(env, TInitCell);

  if (outTy->is(BBottom)) {
    unreachable(env);
  } else {
    constprop(env);
  }
  push(env, std::move(*outTy));
}

void in(ISS& env, const bc::NewCol& op) {
  auto const type = static_cast<CollectionType>(op.subop1);
  auto const name = collections::typeToString(type);
  push(env, objExact(builtin_class(env.index, name)));
  effect_free(env);
}

void in(ISS& env, const bc::NewPair& /*op*/) {
  popC(env); popC(env);
  auto const name = collections::typeToString(CollectionType::Pair);
  push(env, objExact(builtin_class(env.index, name)));
  effect_free(env);
}

void in(ISS& env, const bc::ColFromArray& op) {
  auto const src = popC(env);
  auto const type = static_cast<CollectionType>(op.subop1);
  assertx(type != CollectionType::Pair);
  if (type == CollectionType::Vector || type == CollectionType::ImmVector) {
    if (src.subtypeOf(TVec)) effect_free(env);
  } else {
    assertx(type == CollectionType::Map ||
            type == CollectionType::ImmMap ||
            type == CollectionType::Set ||
            type == CollectionType::ImmSet);
    if (src.subtypeOf(TDict)) effect_free(env);
  }
  auto const name = collections::typeToString(type);
  push(env, objExact(builtin_class(env.index, name)));
}

void in(ISS& env, const bc::CnsE& op) {
  auto t = env.index.lookup_constant(env.ctx, op.str1);
  if (t.subtypeOf(BBottom)) unreachable(env);
  constprop(env);
  push(env, std::move(t));
}

namespace {

void clsCnsImpl(ISS& env, const Type& cls, const Type& name) {
  if (!cls.couldBe(BCls) || !name.couldBe(BStr)) {
    push(env, TBottom);
    unreachable(env);
    return;
  }

  auto lookup = lookupClsConstant(env, cls, name);
  if (lookup.found == TriBool::No || lookup.ty.is(BBottom)) {
    push(env, TBottom);
    unreachable(env);
    return;
  }

  if (cls.subtypeOf(BCls) &&
      name.subtypeOf(BStr) &&
      lookup.found == TriBool::Yes &&
      !lookup.mightThrow) {
    constprop(env);
    effect_free(env);
  }

  push(env, std::move(lookup.ty));
}

}

void in(ISS& env, const bc::ClsCns& op) {
  auto const cls = topC(env);

  if (cls.subtypeOf(BCls) && is_specialized_cls(cls)) {
    auto const& dcls = dcls_of(cls);
    if (dcls.isExact()) {
      return reduce(
        env, bc::PopC {}, bc::ClsCnsD { op.str1, dcls.cls().name() }
      );
    }
  }

  popC(env);
  clsCnsImpl(env, cls, sval(op.str1));
}

void in(ISS& env, const bc::ClsCnsL& op) {
  auto const cls = topC(env);
  auto const name = locRaw(env, op.loc1);

  if (name.subtypeOf(BStr) && is_specialized_string(name)) {
    return reduce(env, bc::ClsCns { sval_of(name) });
  }

  popC(env);
  clsCnsImpl(env, cls, name);
}

void in(ISS& env, const bc::ClsCnsD& op) {
  auto const rcls = env.index.resolve_class(op.str2);
  if (!rcls) {
    push(env, TBottom);
    unreachable(env);
    return;
  }
  clsCnsImpl(env, clsExact(*rcls, true), sval(op.str1));
}

void in(ISS& env, const bc::File&) {
  if (!options.SourceRootForFileBC) {
    effect_free(env);
    return push(env, TSStr);
  }

  auto filename = env.ctx.func->originalUnit
    ? env.ctx.func->originalUnit
    : env.ctx.func->unit;
  if (!FileUtil::isAbsolutePath(filename->slice())) {
    filename = makeStaticString(
      *options.SourceRootForFileBC + filename->toCppString()
    );
  }
  constprop(env);
  push(env, sval(filename));
}

void in(ISS& env, const bc::Dir&) {
  if (!options.SourceRootForFileBC) {
    effect_free(env);
    return push(env, TSStr);
  }

  auto filename = env.ctx.func->originalUnit
    ? env.ctx.func->originalUnit
    : env.ctx.func->unit;
  if (!FileUtil::isAbsolutePath(filename->slice())) {
    filename = makeStaticString(
      *options.SourceRootForFileBC + filename->toCppString()
    );
  }
  constprop(env);
  push(env, sval(makeStaticString(FileUtil::dirname(StrNR{filename}))));
}

void in(ISS& env, const bc::Method&) {
  auto const fullName = [&] () -> const StringData* {
    if (!env.ctx.func->cls) return env.ctx.func->name;
    return makeStaticString(
      fmt::format("{}::{}", env.ctx.func->cls->name->data(), env.ctx.func->name->data())
    );
  }();
  constprop(env);
  push(env, sval(fullName));
}

void in(ISS& env, const bc::FuncCred&) { effect_free(env); push(env, TObj); }

void in(ISS& env, const bc::ClassName& op) {
  auto const ty = topC(env);
  if (ty.subtypeOf(BCls) && is_specialized_cls(ty)) {
    auto const& dcls = dcls_of(ty);
    if (dcls.isExact()) {
      return reduce(env,
                    bc::PopC {},
                    bc::String { dcls.cls().name() });
    }
    effect_free(env);
  }
  popC(env);
  push(env, TSStr);
}

void in(ISS& env, const bc::EnumClassLabelName& op) {
  auto const ty = topC(env);
  if (ty.subtypeOf(BEnumClassLabel) && is_specialized_ecl(ty)) {
    auto const& label = eclval_of(ty);
    return reduce(env,
                  bc::PopC {},
                  bc::String { label });
  }
  if (ty.subtypeOf(BEnumClassLabel)) effect_free(env);
  popC(env);
  push(env, TSStr);
}

void concatHelper(ISS& env, uint32_t n) {
  auto changed = false;
  auto side_effects = false;
  if (will_reduce(env)) {
    auto litstr = [&] (SString next, uint32_t i) -> SString {
      auto const t = topC(env, i);
      auto const v = tv(t);
      if (!v) return nullptr;
      if (!isStringType(v->m_type) && !isIntType(v->m_type)) return nullptr;
      auto const cell = eval_cell_value(
        [&] {
          auto const s = makeStaticString(
            next ?
            StringData::Make(tvAsCVarRef(&*v).toString().get(), next) :
            tvAsCVarRef(&*v).toString().get());
          return make_tv<KindOfString>(s);
        }
      );
      if (!cell) return nullptr;
      return cell->m_data.pstr;
    };

    auto fold = [&] (uint32_t slot, uint32_t num, SString result) {
      auto const cell = make_tv<KindOfString>(result);
      auto const ty = from_cell(cell);
      BytecodeVec bcs{num, bc::PopC {}};
      if (num > 1) bcs.push_back(gen_constant(cell));
      if (slot == 0) {
        reduce(env, std::move(bcs));
      } else {
        insert_after_slot(env, slot, num, num > 1 ? 1 : 0, &ty, bcs);
        reprocess(env);
      }
      n -= num - 1;
      changed = true;
    };

    for (auto i = 0; i < n; i++) {
      if (!topC(env, i).subtypeOf(BArrKey)) {
        side_effects = true;
        break;
      }
    }

    if (!side_effects && can_rewind(env)) {
      for (auto i = 0; i < n; i++) {
        auto const prev = op_from_slot(env, i);
        if (!prev) continue;
        if (prev->op == Op::Concat || prev->op == Op::ConcatN) {
          auto const extra = kill_by_slot(env, i);
          changed = true;
          n += extra;
          i += extra;
        }
      }
    }

    SString result = nullptr;
    uint32_t i = 0;
    uint32_t nlit = 0;
    while (i < n) {
      // In order to collapse literals, we need to be able to insert
      // pops, and a constant after the sequence that generated the
      // literals. We can always insert after the last instruction
      // though, and we only need to check the first slot of a
      // sequence.
      auto const next = !i || result || can_insert_after_slot(env, i) ?
        litstr(result, i) : nullptr;
      if (next == staticEmptyString()) {
        if (n == 1) break;
        // don't fold away empty strings if the concat could trigger exceptions
        if (i == 0 && !topC(env, 1).subtypeOf(BArrKey)) break;
        if (n == 2 && i == 1 && !topC(env, 0).subtypeOf(BArrKey)) break;
        assertx(nlit == 0);
        fold(i, 1, next);
        n--;
        continue;
      }
      if (!next) {
        if (nlit > 1) {
          fold(i - nlit, nlit, result);
          i -= nlit - 1;
        }
        nlit = 0;
      } else {
        nlit++;
      }
      result = next;
      i++;
    }
    if (nlit > 1) fold(i - nlit, nlit, result);
  }

  if (!changed) {
    discard(env, n);
    push(env, TStr);
    return;
  }

  if (n == 1) {
    if (!topC(env).subtypeOf(BStr)) {
      return reduce(env, bc::CastString {});
    }
    return reduce(env);
  }

  reduce(env);
  // We can't reduce the emitted concats, or we'll end up with
  // infinite recursion.
  env.flags.wasPEI = true;
  env.flags.effectFree = false;
  env.flags.canConstProp = false;

  auto concat = [&] (uint32_t num) {
    discard(env, num);
    push(env, TStr);
    if (num == 2) {
      record(env, bc::Concat {});
    } else {
      record(env, bc::ConcatN { num });
    }
  };

  while (n >= 4) {
    concat(4);
    n -= 3;
  }
  if (n > 1) concat(n);
}

void in(ISS& env, const bc::Concat& /*op*/) {
  concatHelper(env, 2);
}

void in(ISS& env, const bc::ConcatN& op) {
  if (op.arg1 == 2) return reduce(env, bc::Concat {});
  concatHelper(env, op.arg1);
}

template <class Op, class Fun>
void arithImpl(ISS& env, const Op& /*op*/, Fun fun) {
  constprop(env);
  auto const t1 = popC(env);
  auto const t2 = popC(env);
  auto r = fun(t2, t1);
  if (r.is(BBottom)) unreachable(env);
  push(env, std::move(r));
}

void in(ISS& env, const bc::Add& op)    { arithImpl(env, op, typeAdd); }
void in(ISS& env, const bc::Sub& op)    { arithImpl(env, op, typeSub); }
void in(ISS& env, const bc::Mul& op)    { arithImpl(env, op, typeMul); }
void in(ISS& env, const bc::Div& op)    { arithImpl(env, op, typeDiv); }
void in(ISS& env, const bc::Mod& op)    { arithImpl(env, op, typeMod); }
void in(ISS& env, const bc::Pow& op)    { arithImpl(env, op, typePow); }
void in(ISS& env, const bc::BitAnd& op) { arithImpl(env, op, typeBitAnd); }
void in(ISS& env, const bc::BitOr& op)  { arithImpl(env, op, typeBitOr); }
void in(ISS& env, const bc::BitXor& op) { arithImpl(env, op, typeBitXor); }
void in(ISS& env, const bc::Shl& op)    { arithImpl(env, op, typeShl); }
void in(ISS& env, const bc::Shr& op)    { arithImpl(env, op, typeShr); }

void in(ISS& env, const bc::BitNot& /*op*/) {
  auto const t = popC(env);
  auto const v = tv(t);
  if (!t.couldBe(BInt | BStr | BSStr | BLazyCls | BCls)) {
    unreachable(env);
    return push(env, TBottom);
  }

  // If it's a string we raise a warning or error if Eval.WarnOnBitwiseOpInvalidType is set
  if (v && !t.subtypeOf(BStr)) {
    constprop(env);
    auto cell = eval_cell([&] {
      auto c = *v;
      tvBitNot(c);
      return c;
    });
    if (cell) return push(env, std::move(*cell));
  }
  push(env, TInitCell);
}

namespace {

template<bool NSame>
std::pair<Type,bool> resolveSame(ISS& env) {
  auto const l1 = topStkEquiv(env, 0);
  auto const t1 = topC(env, 0);
  auto const l2 = topStkEquiv(env, 1);
  auto const t2 = topC(env, 1);

  auto warningsEnabled =
    (Cfg::Eval::EmitClsMethPointers ||
     Cfg::Eval::RaiseClassConversionNoticeSampleRate > 0);

  auto const result = [&] {
    auto const v1 = tv(t1);
    auto const v2 = tv(t2);

    if (l1 == StackDupId ||
        (l1 == l2 && l1 != NoLocalId) ||
        (l1 <= MaxLocalId && l2 <= MaxLocalId && locsAreEquiv(env, l1, l2))) {
      if (!t1.couldBe(BDbl) || !t2.couldBe(BDbl) ||
          (v1 && (v1->m_type != KindOfDouble || !std::isnan(v1->m_data.dbl))) ||
          (v2 && (v2->m_type != KindOfDouble || !std::isnan(v2->m_data.dbl)))) {
        return NSame ? TFalse : TTrue;
      }
    }

    if (v1 && v2) {
      if (auto r = eval_cell_value([&]{ return tvSame(*v2, *v1); })) {
        // we wouldn't get here if cellSame raised a warning
        warningsEnabled = false;
        return r != NSame ? TTrue : TFalse;
      }
    }

    return NSame ? typeNSame(t1, t2) : typeSame(t1, t2);
  }();

  if (warningsEnabled && result.is(NSame ? BFalse : BTrue)) {
    warningsEnabled = false;
  }
  return { result, warningsEnabled && compare_might_raise(t1, t2) };
}

template<bool Negate>
void sameImpl(ISS& env) {
  if (auto const last = last_op(env)) {
    if (last->op == Op::Null && can_rewind(env)) {
      rewind(env, 1);
      reduce(env, bc::IsTypeC { IsTypeOp::Null });
      if (Negate) reduce(env, bc::Not {});
      return;
    }
    if (auto const prev = last_op(env, 1)) {
      if (prev->op == Op::Null &&
          (last->op == Op::CGetL || last->op == Op::CGetL2 ||
           last->op == Op::CGetQuietL) && can_rewind(env)) {
        auto const loc = [&]() {
          if (last->op == Op::CGetL) {
            return last->CGetL.nloc1;
          } else if (last->op == Op::CGetL2) {
            return last->CGetL2.nloc1;
          } else if (last->op == Op::CGetQuietL) {
            return NamedLocal{kInvalidLocalName, last->CGetQuietL.loc1};
          }
          always_assert(false);
        }();
        rewind(env, 2);
        reduce(env, bc::IsTypeL { loc, IsTypeOp::Null });
        if (Negate) reduce(env, bc::Not {});
        return;
      }
    }
  }

  auto pair = resolveSame<Negate>(env);
  discard(env, 2);

  if (!pair.second) {
    nothrow(env);
    constprop(env);
  }

  push(env, std::move(pair.first));
}

/**
 * Returns true if the branch in the Jmp should always be taken. Used by
 * jmpImpl to reduce `if ($a === $b) { ... }`
 */
template<class JmpOp>
bool sameJmpImpl(ISS& env, Op sameOp, const JmpOp& jmp) {
  const StackElem* elems[2];
  env.state.stack.peek(2, elems, 1);

  auto const loc0 = elems[1]->equivLoc;
  auto const loc1 = elems[0]->equivLoc;
  // If loc0 == loc1, either they're both NoLocalId, so there's
  // nothing for us to deduce, or both stack elements are the same
  // value, so the only thing we could deduce is that they are or are
  // not NaN. But we don't track that, so just bail.
  if (loc0 == loc1 || loc0 == StackDupId) return false;

  auto const ty0 = elems[1]->type;
  auto const ty1 = elems[0]->type;
  auto const val0 = tv(ty0);
  auto const val1 = tv(ty1);

  // If both have values, we expect the interp for Same to have already
  // simplified so we shouldn't be here. Note: tv(Cls=C) still does not have a
  // value despite knowing the exact class.
  assertx(!val0 || !val1);
  if ((loc0 == NoLocalId && !val0 && ty1.subtypeOf(ty0)) ||
      (loc1 == NoLocalId && !val1 && ty0.subtypeOf(ty1))) {
    return false;
  }

  // Same currently lies about the distinction between Func/Cls/Str
  // TODO(T168044199) Unify this analysis with sameImpl
  // see also: couldBeStringish
  if (ty0.couldBe(BCls) && ty1.couldBe(BStr)) return false;
  if (ty1.couldBe(BCls) && ty0.couldBe(BStr)) return false;
  if (ty0.couldBe(BCls) && ty1.couldBe(BLazyCls)) return false;
  if (ty1.couldBe(BCls) && ty0.couldBe(BLazyCls)) return false;
  if (ty0.couldBe(BLazyCls) && ty1.couldBe(BStr)) return false;
  if (ty1.couldBe(BLazyCls) && ty0.couldBe(BStr)) return false;

  auto isect = intersection_of(ty0, ty1);

  // Unfortunately, floating point negative zero and positive zero are
  // different, but are identical using as far as Same is concerened. We should
  // avoid refining a value to 0.0 because it compares identically to 0.0
  if (isect.couldBe(dval(0.0)) || isect.couldBe(dval(-0.0))) {
    isect = union_of(isect, TDbl);
  }

  discard(env, 1);

  auto handle_same = [&] {
    // Currently dce uses equivalency to prove that something isn't
    // the last reference - so we can only assert equivalency here if
    // we know that won't be affected. Its irrelevant for uncounted
    // things, and for TObj and TRes, $x === $y iff $x and $y refer to
    // the same thing.
    if (loc0 <= MaxLocalId &&
        (ty0.subtypeOf(BObj | BRes | BPrim) ||
         ty1.subtypeOf(BObj | BRes | BPrim) ||
         (ty0.subtypeOf(BUnc) && ty1.subtypeOf(BUnc)))) {
      if (loc1 == StackDupId) {
        setStkLocal(env, loc0, 0);
      } else if (loc1 <= MaxLocalId && !locsAreEquiv(env, loc0, loc1)) {
        auto loc = loc0;
        while (true) {
          auto const other = findLocEquiv(env, loc);
          if (other == NoLocalId) break;
          killLocEquiv(env, loc);
          addLocEquiv(env, loc, loc1);
          loc = other;
        }
        addLocEquiv(env, loc, loc1);
      }
    }
    return refineLocation(env, loc1 != NoLocalId ? loc1 : loc0, [&] (Type ty) {
        auto const needsUninit =
          ty.couldBe(BUninit) &&
          !isect.couldBe(BUninit) &&
          isect.couldBe(BInitNull);
        auto ret = ty.subtypeOf(BUnc) ? isect : loosen_staticness(isect);
        if (needsUninit) ret = union_of(std::move(ret), TUninit);
        return ret;
      }
    );
  };

  auto handle_differ_side = [&] (LocalId location, const Type& ty) {
    if (!ty.subtypeOf(BInitNull) && !ty.strictSubtypeOf(TBool)) return true;
    return refineLocation(env, location, [&] (Type t) {
      if (ty.subtypeOf(BNull)) {
        t = remove_uninit(std::move(t));
        if (t.couldBe(BInitNull) && !t.subtypeOf(BInitNull)) {
          t = unopt(std::move(t));
        }
        return t;
      } else if (ty.strictSubtypeOf(TBool) && t.subtypeOf(BBool)) {
        return ty.is(BFalse) ? TTrue : TFalse;
      }
      return t;
    });
  };

  auto handle_differ = [&] {
    return
      (loc0 == NoLocalId || handle_differ_side(loc0, ty1)) &&
      (loc1 == NoLocalId || handle_differ_side(loc1, ty0));
  };

  auto const sameIsJmpTarget =
    (sameOp == Op::Same) == (JmpOp::op == Op::JmpNZ);

  auto save = env.state;
  auto const target_reachable = sameIsJmpTarget ?
    handle_same() : handle_differ();
  if (!target_reachable) jmp_nevertaken(env);
  // swap, so we can restore this state if the branch is always taken.
  env.state.swap(save);
  if (!(sameIsJmpTarget ? handle_differ() : handle_same())) {
    jmp_setdest(env, jmp.target1);
    env.state.copy_from(std::move(save));
  } else if (target_reachable) {
    env.propagate(jmp.target1, &save);
  }

  return true;
}

bc::JmpNZ invertJmp(const bc::JmpZ& jmp) { return bc::JmpNZ { jmp.target1 }; }
bc::JmpZ invertJmp(const bc::JmpNZ& jmp) { return bc::JmpZ { jmp.target1 }; }

}

void in(ISS& env, const bc::Same&)  { sameImpl<false>(env); }
void in(ISS& env, const bc::NSame&) { sameImpl<true>(env); }

template<class Fun>
void cmpImpl(ISS& env, Fun fun) {
  auto const t1 = popC(env);
  auto const t2 = popC(env);
  auto const v1 = tv(t1);
  auto const v2 = tv(t2);
  if (v1 && v2) {
    if (auto r = eval_cell_value([&]{ return fun(*v2, *v1); })) {
      constprop(env);
      return push(env, *r ? TTrue : TFalse);
    }
  }
  // TODO_4: evaluate when these can throw, non-constant type stuff.
  push(env, TBool);
}

namespace {

bool couldBeStringish(const Type& t) {
  return t.couldBe(BCls | BLazyCls | BStr);
}

bool everEq(const Type& t1, const Type& t2) {
  // for comparison purposes we need to be careful about these coercions
  if (couldBeStringish(t1) && couldBeStringish(t2)) return true;
  return loosen_all(t1).couldBe(loosen_all(t2));
}

bool cmpWillThrow(const Type& t1, const Type& t2) {
  // for comparison purposes we need to be careful about these coercions
  if (couldBeStringish(t1) && couldBeStringish(t2)) return false;

  auto couldBeIntAndDbl = [](const Type& t1, const Type& t2) {
    return t1.couldBe(BInt) && t2.couldBe(BDbl);
  };
   // relational comparisons allow for int v dbl
  if (couldBeIntAndDbl(t1, t2) || couldBeIntAndDbl(t2, t1)) return false;

  return !loosen_to_datatype(t1).couldBe(loosen_to_datatype(t2));
}

void eqImpl(ISS& env, bool eq) {
  auto rs = resolveSame<false>(env);
  if (rs.first.is(BTrue)) {
    if (!rs.second) constprop(env);
    discard(env, 2);
    return push(env, eq ? TTrue : TFalse);
  }

  if (!everEq(topC(env, 0), topC(env, 1))) {
    discard(env, 2);
    return push(env, eq ? TFalse : TTrue);
  }

  cmpImpl(env, [&] (TypedValue c1, TypedValue c2) {
    return tvEqual(c1, c2) == eq;
  });
}

bool cmpThrowCheck(ISS& env, const Type& t1, const Type& t2) {
  if (!cmpWillThrow(t1, t2)) return false;
  discard(env, 2);
  push(env, TBottom);
  unreachable(env);
  return true;
}

}

void in(ISS& env, const bc::Eq&) { eqImpl(env, true); }
void in(ISS& env, const bc::Neq&) { eqImpl(env, false); }

void in(ISS& env, const bc::Lt&) {
  if (cmpThrowCheck(env, topC(env, 0), topC(env, 1))) return;
  cmpImpl(env, static_cast<bool (*)(TypedValue, TypedValue)>(tvLess));
}
void in(ISS& env, const bc::Gt&) {
  if (cmpThrowCheck(env, topC(env, 0), topC(env, 1))) return;
  cmpImpl(env, static_cast<bool (*)(TypedValue, TypedValue)>(tvGreater));
}
void in(ISS& env, const bc::Lte&) {
  if (cmpThrowCheck(env, topC(env, 0), topC(env, 1))) return;
  cmpImpl(env, tvLessOrEqual);
}
void in(ISS& env, const bc::Gte&) {
  if (cmpThrowCheck(env, topC(env, 0), topC(env, 1))) return;
  cmpImpl(env, tvGreaterOrEqual);
}

void in(ISS& env, const bc::Cmp&) {
  auto const t1 = topC(env, 0);
  auto const t2 = topC(env, 1);
  if (cmpThrowCheck(env, t1, t2)) return;
  discard(env, 2);
  if (equal(t1, t2)) {
    auto const v1 = tv(t1);
    auto const v2 = tv(t2);
    if (v1 && v2) {
      if (auto r = eval_cell_value([&]{ return ival(tvCompare(*v2, *v1)); })) {
        constprop(env);
        return push(env, std::move(*r));
      }
    }
  }
  // TODO_4: evaluate when these can throw, non-constant type stuff.
  push(env, TInt);
}

void castBoolImpl(ISS& env, const Type& t, bool negate) {
  auto const [e, effectFree] = emptiness(t);
  if (effectFree) {
    effect_free(env);
    constprop(env);
  }

  switch (e) {
    case Emptiness::Empty:
    case Emptiness::NonEmpty:
      return push(env, (e == Emptiness::Empty) == negate ? TTrue : TFalse);
    case Emptiness::Maybe:
      break;
  }

  push(env, TBool);
}

void in(ISS& env, const bc::Not&) {
  castBoolImpl(env, popC(env), true);
}

void in(ISS& env, const bc::CastBool&) {
  auto const t = topC(env);
  if (t.subtypeOf(BBool)) return reduce(env);
  castBoolImpl(env, popC(env), false);
}

void in(ISS& env, const bc::CastInt&) {
  auto const t = topC(env);
  if (t.subtypeOf(BInt)) return reduce(env);
  constprop(env);
  popC(env);
  // Objects can raise a warning about converting to int, class-likes can
  // raise a notice, and funcs, cls-meths and enum class labels throw.
  if (t.subtypeOf(BPrim | BStr | BArrLike | BRes)) nothrow(env);
  if (auto const v = tv(t)) {
    auto cell = eval_cell([&] {
      return make_tv<KindOfInt64>(tvToInt(*v));
    });
    if (cell) return push(env, std::move(*cell));
  }
  push(env, TInt);
}

// Handle a casting operation, where "target" is the type being casted to. If
// "fn" is provided, it will be called to cast any constant inputs. If "elide"
// is set to true, if the source type is the same as the destination, the cast
// will be optimized away.
void castImpl(ISS& env, Type target, void(*fn)(TypedValue*)) {
  auto const t = topC(env);
  if (t.subtypeOf(target)) return reduce(env);
  popC(env);

  if (fn) {
    if (auto val = tv(t)) {
      if (auto result = eval_cell([&] { fn(&*val); return *val; })) {
        constprop(env);
        target = *result;
      }
    }
  }
  push(env, std::move(target));
}

void in(ISS& env, const bc::CastDouble&) {
  castImpl(env, TDbl, tvCastToDoubleInPlace);
}

void in(ISS& env, const bc::CastString&) {
  castImpl(env, TStr, tvCastToStringInPlace);
}

void in(ISS& env, const bc::CastDict&)   {
  castImpl(env, TDict, tvCastToDictInPlace);
}

void in(ISS& env, const bc::CastVec&)    {
  castImpl(env, TVec, tvCastToVecInPlace);
}

void in(ISS& env, const bc::CastKeyset&) {
  castImpl(env, TKeyset, tvCastToKeysetInPlace);
}

void in(ISS& env, const bc::DblAsBits&) {
  effect_free(env);
  constprop(env);

  auto const ty = popC(env);
  if (!ty.couldBe(BDbl)) return push(env, ival(0));

  if (auto val = tv(ty)) {
    assertx(isDoubleType(val->m_type));
    val->m_type = KindOfInt64;
    push(env, from_cell(*val));
    return;
  }

  push(env, TInt);
}

void in(ISS& env, const bc::Print& /*op*/) {
  popC(env);
  push(env, ival(1));
}

void in(ISS& env, const bc::Clone& /*op*/) {
  auto val = popC(env);
  if (!val.subtypeOf(BObj)) {
    val &= TObj;
    if (val.is(BBottom)) unreachable(env);
  }
  push(env, std::move(val));
}

void in(ISS& env, const bc::Exit&)  { popC(env); push(env, TInitNull); }
void in(ISS& env, const bc::Fatal&) { popC(env); }
void in(ISS& env, const bc::StaticAnalysisError&) {}

void in(ISS& env, const bc::Enter& op) {
  always_assert(op.target1 == env.ctx.func->mainEntry);
  env.propagate(env.ctx.func->mainEntry, &env.state);
}

void in(ISS& /*env*/, const bc::Jmp&) {
  always_assert(0 && "blocks should not contain Jmp instructions");
}

void in(ISS& env, const bc::Select& op) {
  auto const cond = topC(env);
  auto const t = topC(env, 1);
  auto const f = topC(env, 2);

  auto const [e, effectFree] = emptiness(cond);
  if (effectFree) {
    effect_free(env);
    constprop(env);
  }

  switch (e) {
    case Emptiness::Maybe:
      discard(env, 3);
      push(env, union_of(t, f));
      return;
    case Emptiness::NonEmpty:
      discard(env, 3);
      push(env, t);
      return;
    case Emptiness::Empty:
      return reduce(env, bc::PopC {}, bc::PopC {});
  }
  not_reached();
}

namespace {

template<class JmpOp>
bool isTypeHelper(ISS& env,
                  IsTypeOp typeOp,
                  LocalId location,
                  Op op,
                  const JmpOp& jmp) {
  if (typeOp == IsTypeOp::Scalar || typeOp == IsTypeOp::LegacyArrLike ||
      typeOp == IsTypeOp::Func) {
    return false;
  }

  auto const val = [&] {
    if (op != Op::IsTypeC) return locRaw(env, location);
    const StackElem* elem;
    env.state.stack.peek(1, &elem, 1);
    location = elem->equivLoc;
    return elem->type;
  }();

  if (location == NoLocalId || !val.subtypeOf(BCell)) return false;

  // If the type could be ClsMeth and Arr/Vec, skip location refining.
  // Otherwise, refine location based on the testType.
  auto testTy = type_of_istype(typeOp);

  assertx(val.couldBe(testTy) &&
          (!val.subtypeOf(testTy) || val.subtypeOf(BObj)));

  discard(env, 1);

  if (op == Op::IsTypeC) {
    if (!is_type_might_raise(testTy, val)) nothrow(env);
  } else if (op == Op::IssetL) {
    nothrow(env);
  } else if (!locCouldBeUninit(env, location) &&
             !is_type_might_raise(testTy, val)) {
    nothrow(env);
  }

  auto const negate = (jmp.op == Op::JmpNZ) == (op != Op::IssetL);
  auto const was_true = [&] (Type t) {
    if (testTy.subtypeOf(BNull)) return intersection_of(t, TNull);
    assertx(!testTy.couldBe(BNull));
    return intersection_of(t, testTy);
  };
  auto const was_false = [&] (Type t) {
    auto tinit = remove_uninit(t);
    if (testTy.subtypeOf(BNull)) {
      return (tinit.couldBe(BInitNull) && !tinit.subtypeOf(BInitNull))
        ? unopt(std::move(tinit)) : tinit;
    }
    if (t.couldBe(BInitNull) && !t.subtypeOf(BInitNull)) {
      assertx(!testTy.couldBe(BNull));
      if (unopt(tinit).subtypeOf(testTy)) return TNull;
    }
    return t;
  };

  auto const taken = [&] (Type t) {
    return negate ? was_true(std::move(t)) : was_false(std::move(t));
  };

  auto const fallthrough = [&] (Type t) {
    return negate ? was_false(std::move(t)) : was_true(std::move(t));
  };

  refineLocation(env, location, taken, jmp.target1, fallthrough);
  return true;
}

template<class JmpOp>
bool instanceOfJmpImpl(ISS& env,
                       const bc::InstanceOfD& inst,
                       const JmpOp& jmp) {

  const StackElem* elem;
  env.state.stack.peek(1, &elem, 1);

  auto const locId = elem->equivLoc;
  if (locId == NoLocalId || interface_supports_non_objects(inst.str1)) {
    return false;
  }
  auto const rcls = env.index.resolve_class(inst.str1);
  if (!rcls) return false;

  auto const val = elem->type;
  auto const instTy = subObj(*rcls);
  assertx(!val.subtypeOf(instTy) && val.couldBe(instTy));

  // If we have an optional type, whose unopt is guaranteed to pass
  // the instanceof check, then failing to pass implies it was null.
  auto const fail_implies_null =
    val.couldBe(BInitNull) &&
    !val.subtypeOf(BInitNull) &&
    unopt(val).subtypeOf(instTy);

  discard(env, 1);
  auto const negate = jmp.op == Op::JmpNZ;
  auto const result = [&] (Type t, bool pass) {
    return pass ? instTy : fail_implies_null ? TNull : t;
  };
  auto const taken  = [&] (Type t) { return result(t, negate); };
  auto const fallthrough = [&] (Type t) { return result(t, !negate); };
  refineLocation(env, locId, taken, jmp.target1, fallthrough);
  return true;
}

template<class JmpOp>
bool isTypeStructCJmpImpl(ISS& env,
                          const bc::IsTypeStructC& inst,
                          const JmpOp& jmp) {

  const StackElem* elems[2];
  env.state.stack.peek(2, elems, 1);

  auto const locId = elems[0]->equivLoc;
  if (locId == NoLocalId) return false;

  auto const a = tv(elems[1]->type);
  if (!a) return false;
  // if it wasn't valid, the JmpOp wouldn't be reachable
  assertx(isValidTSType(*a, false));

  auto const is_nullable_ts = is_ts_nullable(a->m_data.parr);
  auto const ts_kind = get_ts_kind(a->m_data.parr);
  // type_of_type_structure does not resolve these types.  It is important we
  // do resolve them here, or we may have issues when we reduce the checks to
  // InstanceOfD checks.  This logic performs the same exact refinement as
  // instanceOfD will.
  if (is_nullable_ts ||
      (ts_kind != TypeStructure::Kind::T_class &&
       ts_kind != TypeStructure::Kind::T_interface &&
       ts_kind != TypeStructure::Kind::T_xhp &&
       ts_kind != TypeStructure::Kind::T_unresolved)) {
    return false;
  }

  auto const clsName = get_ts_classname(a->m_data.parr);

  if (interface_supports_non_objects(clsName)) return false;

  auto const rcls = env.index.resolve_class(clsName);
  if (!rcls) return false;

  auto const val = elems[0]->type;
  auto const instTy = subObj(*rcls);
  if (val.subtypeOf(instTy) || !val.couldBe(instTy)) {
    return false;
  }

  auto const cls = rcls->cls();
  if (!cls || cls->attrs & AttrEnum) return false;

  // If we have an optional type, whose unopt is guaranteed to pass
  // the instanceof check, then failing to pass implies it was null.
  auto const fail_implies_null =
    val.couldBe(BInitNull) &&
    !val.subtypeOf(BInitNull) &&
    unopt(val).subtypeOf(instTy);

  discard(env, 1);

  auto const negate = jmp.op == Op::JmpNZ;
  auto const result = [&] (Type t, bool pass) {
    return pass ? instTy : fail_implies_null ? TNull : t;
  };
  auto const taken  = [&] (Type t) { return result(t, negate); };
  auto const fallthrough = [&] (Type t) { return result(t, !negate); };
  refineLocation(env, locId, taken, jmp.target1, fallthrough);
  return true;
}

template<class JmpOp>
void jmpImpl(ISS& env, const JmpOp& op) {
  auto const Negate = std::is_same<JmpOp, bc::JmpNZ>::value;
  auto const location = topStkEquiv(env);
  auto const t = topC(env);
  auto const [e, effectFree] = emptiness(t);
  if (e == (Negate ? Emptiness::NonEmpty : Emptiness::Empty)) {
    reduce(env, bc::PopC {});
    return jmp_setdest(env, op.target1);
  }

  if (e == (Negate ? Emptiness::Empty : Emptiness::NonEmpty) ||
      (next_real_block(env.ctx.func, env.fallthrough) ==
       next_real_block(env.ctx.func, op.target1))) {
    return reduce(env, bc::PopC{});
  }

  auto const fix = [&, effectFree=effectFree] {
    if (effectFree) effect_free(env);
    if (env.flags.jmpDest == NoBlockId || !can_rewind(env)) return;
    auto const jmpDest = env.flags.jmpDest;
    env.flags.jmpDest = NoBlockId;
    rewind(env, op);
    reduce(env, bc::PopC {});
    env.flags.jmpDest = jmpDest;
  };

  if (auto const last = last_op(env)) {
    if (last->op == Op::Not && can_rewind(env)) {
      rewind(env, 1);
      return reduce(env, invertJmp(op));
    }
    if (last->op == Op::Same || last->op == Op::NSame) {
      if (sameJmpImpl(env, last->op, op)) return fix();
    } else if (last->op == Op::IssetL) {
      if (isTypeHelper(env,
                       IsTypeOp::Null,
                       last->IssetL.loc1,
                       last->op,
                       op)) {
        return fix();
      }
    } else if (last->op == Op::IsTypeL) {
      if (isTypeHelper(env,
                       last->IsTypeL.subop2,
                       last->IsTypeL.nloc1.id,
                       last->op,
                       op)) {
        return fix();
      }
    } else if (last->op == Op::IsTypeC) {
      if (isTypeHelper(env,
                       last->IsTypeC.subop1,
                       NoLocalId,
                       last->op,
                       op)) {
        return fix();
      }
    } else if (last->op == Op::InstanceOfD) {
      if (instanceOfJmpImpl(env, last->InstanceOfD, op)) return fix();
    } else if (last->op == Op::IsTypeStructC) {
      if (isTypeStructCJmpImpl(env, last->IsTypeStructC, op)) return fix();
    }
  }

  popC(env);

  if (location == NoLocalId) {
    if (effectFree) effect_free(env);
    return env.propagate(op.target1, &env.state);
  }

  refineLocation(env, location,
                 Negate ? assert_nonemptiness : assert_emptiness,
                 op.target1,
                 Negate ? assert_emptiness : assert_nonemptiness);
  return fix();
}

} // namespace

void in(ISS& env, const bc::JmpNZ& op) { jmpImpl(env, op); }
void in(ISS& env, const bc::JmpZ& op)  { jmpImpl(env, op); }

void in(ISS& env, const bc::Switch& op) {
  const auto t = topC(env);
  const auto v = tv(t);

  auto bail = [&] {
    popC(env);
    forEachTakenEdge(op, [&] (BlockId id) {
        env.propagate(id, &env.state);
    });
  };

  auto go = [&] (BlockId blk) {
    reduce(env, bc::PopC {});
    return jmp_setdest(env, blk);
  };

  if (!t.couldBe(BInt)) {
    if (op.subop1 == SwitchKind::Unbounded) return bail();
    return go(op.targets.back());
  }

  if (!v) return bail();

  auto num_elems = op.targets.size();
  if (op.subop1 == SwitchKind::Unbounded) {
    if (v->m_data.num < 0 || v->m_data.num >= num_elems) return bail();
    return go(op.targets[v->m_data.num]);
  }

  assertx(num_elems > 2);
  num_elems -= 2;
  auto const i = v->m_data.num - op.arg2;
  return i >= 0 && i < num_elems ? go(op.targets[i]) : go(op.targets.back());
}

void in(ISS& env, const bc::SSwitch& op) {
  const auto t = topC(env);
  const auto v = tv(t);

  if (!couldBeStringish(t)) {
    reduce(env, bc::PopC {});
    return jmp_setdest(env, op.targets.back().second);
  }

  if (v) {
    for (auto& kv : op.targets) {
      auto match = eval_cell_value([&] {
        if (!kv.first) return true;
        return v->m_data.pstr->equal(kv.first);
      });

      if (!match) break;
      if (*match) {
        reduce(env, bc::PopC {});
        return jmp_setdest(env, kv.second);
      }
    }
  }

  popC(env);
  forEachTakenEdge(op, [&] (BlockId id) {
      env.propagate(id, &env.state);
  });
}

void in(ISS& env, const bc::RetC& op) {
  auto const locEquiv = topStkLocal(env);
  auto res = verifyRetImpl(env, op.subop1, op, topC(env));
  if (!res) return;
  popC(env);
  doRet(env, res->type, !res->effectFree);
  if (!res->coerced && locEquiv != NoLocalId && locEquiv < env.ctx.func->params.size()) {
    env.flags.retParam = locEquiv;
  }
}

void in(ISS& env, const bc::RetM& op) {
  auto res = verifyRetImpl(env, op.subop2, op, topC(env, op.arg1 - 1));
  if (!res) return;
  std::vector<Type> ret(op.arg1);
  for (int i = 0; i < op.arg1; ++i) {
    ret[op.arg1 - i - 1] = popC(env);
  }
  ret[0] = std::move(res->type);
  bool unreachable = std::any_of(
    ret.begin(),
    ret.end(),
    [&](const Type& t) { return t.is(BBottom);}
  );
  doRet(env, unreachable ? TBottom : vec(std::move(ret)), unreachable || !res->effectFree);
}

void in(ISS& env, const bc::RetCSuspended&) {
  always_assert(env.ctx.func->isAsync && !env.ctx.func->isGenerator);

  auto const type = popC(env);
  env.flags.canSuspend = true;
  auto const retTy = is_specialized_wait_handle(type) ? wait_handle_inner(type) : TInitCell;
  doRet(env, retTy, retTy.is(BBottom));
}

void in(ISS& env, const bc::Throw& /*op*/) {
  popC(env);
}

void in(ISS& env, const bc::ThrowNonExhaustiveSwitch& /*op*/) {}

void in(ISS& env, const bc::RaiseClassStringConversionNotice& /*op*/) {}

void in(ISS& env, const bc::ChainFaults&) {
  popC(env);
}

void in(ISS& env, const bc::NativeImpl&) {
  auto const& func = env.ctx.func;
  assertx(func->isNative);
  assertx(!func->isAsync);

  killLocals(env);
  return doRet(
    env,
    return_type_from_constraints(
      *func,
      [&] (SString name) { return env.index.resolve_class(name); },
      [&] () { return selfCls(env.index, env.ctx); }
    ),
    true
  );
}

void in(ISS& env, const bc::CGetL& op) {
  if (locIsThis(env, op.nloc1.id)) {
    auto const& ty = peekLocRaw(env, op.nloc1.id);
    if (!ty.subtypeOf(BInitNull)) {
      auto const subop = ty.couldBe(BUninit) ?
        BareThisOp::Notice : ty.couldBe(BNull) ?
        BareThisOp::NoNotice : BareThisOp::NeverNull;
      return reduce(env, bc::BareThis { subop });
    }
  }
  if (auto const last = last_op(env)) {
    if (last->op == Op::PopL &&
        op.nloc1.id == last->PopL.loc1 && can_rewind(env)) {
      reprocess(env);
      rewind(env, 1);
      setLocRaw(env, op.nloc1.id, TCell);
      return reduce(env, bc::SetL { op.nloc1.id });
    }
  }
  if (!peekLocCouldBeUninit(env, op.nloc1.id)) {
    auto const minLocEquiv = findMinLocEquiv(env, op.nloc1.id, false);
    auto const loc = minLocEquiv != NoLocalId ? minLocEquiv : op.nloc1.id;
    return reduce(env, bc::CGetQuietL { loc });
  }
  mayReadLocal(env, op.nloc1.id);
  push(env, locAsCell(env, op.nloc1.id), op.nloc1.id);
}

void in(ISS& env, const bc::CGetQuietL& op) {
  if (locIsThis(env, op.loc1)) {
    return reduce(env, bc::BareThis { BareThisOp::NoNotice });
  }
  if (auto const last = last_op(env)) {
    if (last->op == Op::PopL &&
        op.loc1 == last->PopL.loc1 &&
        can_rewind(env)) {
      reprocess(env);
      rewind(env, 1);
      setLocRaw(env, op.loc1, TCell);
      return reduce(env, bc::SetL { op.loc1 });
    }
  }
  auto const minLocEquiv = findMinLocEquiv(env, op.loc1, true);
  if (minLocEquiv != NoLocalId) {
    return reduce(env, bc::CGetQuietL { minLocEquiv });
  }

  effect_free(env);
  constprop(env);
  mayReadLocal(env, op.loc1);
  push(env, locAsCell(env, op.loc1), op.loc1);
}

void in(ISS& env, const bc::CUGetL& op) {
  auto ty = locRaw(env, op.loc1);
  effect_free(env);
  constprop(env);
  push(env, std::move(ty), op.loc1);
}

void in(ISS& env, const bc::PushL& op) {
  auto const minLocEquiv = findMinLocEquiv(env, op.loc1, false);
  if (minLocEquiv != NoLocalId) {
    return reduce(env, bc::CGetQuietL { minLocEquiv }, bc::UnsetL { op.loc1 });
  }

  if (auto const last = last_op(env)) {
    if (last->op == Op::PopL &&
        last->PopL.loc1 == op.loc1 && can_rewind(env)) {
      // rewind is ok, because we're just going to unset the local
      // (and note the unset can't be a no-op because the PopL set it
      // to an InitCell). But its possible that before the PopL, the
      // local *was* unset, so maybe would have killed the no-op. The
      // only way to fix that is to reprocess the block with the new
      // instruction sequence and see what happens.
      reprocess(env);
      rewind(env, 1);
      return reduce(env, bc::UnsetL { op.loc1 });
    }
  }

  auto const& ty = peekLocRaw(env, op.loc1);
  if (ty.subtypeOf(BUninit)) {
    // It's unsafe to ever perform a PushL on an uninit location, but we may
    // have generated a PushL in the HackC if we determined that a CGetL
    // could only be reached if the local it referenced was initialized, and
    // now that we know the local is uninitialized we know it must be
    // unreachable.
    //
    // This can happen because the liveness analysis in HackC is much more
    // primitive than HHBBC. If we see a CGetL instruction HackC will assume the
    // local must be initialized afterwards (or it would have thrown) but make
    // no attempt to detect cases where we would unconditionally throw.
    unreachable(env);
  } else if (can_constprop(env)) {
    if (auto val = tv(peekLocRaw(env, op.loc1))) {
      return reduce(env, bc::UnsetL { op.loc1 }, gen_constant(*val));
    }
  }

  impl(env, bc::CGetQuietL { op.loc1 }, bc::UnsetL { op.loc1 });
}

void in(ISS& env, const bc::CGetL2& op) {
  if (auto const last = last_op(env)) {
    if (can_rewind(env)) {
      if ((poppable(last->op) && !numPop(*last)) ||
          ((last->op == Op::CGetL || last->op == Op::CGetQuietL) &&
           !peekLocCouldBeUninit(env, op.nloc1.id))) {
        auto const other = *last;
        rewind(env, 1);
        return reduce(env, bc::CGetL { op.nloc1 }, other);
      }
    }
  }

  if (!peekLocCouldBeUninit(env, op.nloc1.id)) {
    auto const minLocEquiv = findMinLocEquiv(env, op.nloc1.id, false);
    if (minLocEquiv != NoLocalId) {
      return reduce(env, bc::CGetL2 { { kInvalidLocalName, minLocEquiv } });
    }
    effect_free(env);
  }
  mayReadLocal(env, op.nloc1.id);
  auto loc = locAsCell(env, op.nloc1.id);
  auto topEquiv = topStkLocal(env);
  auto top = popT(env);
  push(env, std::move(loc), op.nloc1.id);
  push(env, std::move(top), topEquiv);
}

void in(ISS& env, const bc::CGetG&) { popC(env); push(env, TInitCell); }

void in(ISS& env, const bc::CGetS& op) {
  auto const tcls  = popC(env);
  auto const tname = popC(env);

  auto const throws = [&] {
    unreachable(env);
    return push(env, TBottom);
  };

  if (!tcls.couldBe(BCls)) return throws();

  auto lookup = env.index.lookup_static(
    env.ctx,
    env.collect.props,
    tcls,
    tname
  );

  if (lookup.found == TriBool::No || lookup.ty.subtypeOf(BBottom)) {
    return throws();
  }

  auto const mustBeMutable = ReadonlyOp::Mutable == op.subop1;
  if (mustBeMutable && lookup.readOnly == TriBool::Yes) {
    return throws();
  }
  auto const mightReadOnlyThrow =
    mustBeMutable &&
    lookup.readOnly == TriBool::Maybe;

  if (lookup.found == TriBool::Yes &&
      lookup.lateInit == TriBool::No &&
      lookup.internal == TriBool::No &&
      !lookup.classInitMightRaise &&
      !mightReadOnlyThrow &&
      tcls.subtypeOf(BCls) &&
      tname.subtypeOf(BStr)) {
    effect_free(env);
    constprop(env);
  }

  push(env, std::move(lookup.ty));
}

namespace {

bool module_check_always_passes(ISS& env, const php::Class& cls) {
  auto const unit = env.index.lookup_class_unit(cls);
  if (!(cls.attrs & AttrInternal)) return true;
  return unit->moduleName == env.index.lookup_func_unit(*env.ctx.func)->moduleName;
}

bool module_check_always_passes(ISS& env, const res::Class& rcls) {
  if (auto const cls = rcls.cls()) {
    return module_check_always_passes(env, *cls);
  }
  return false;
}

bool module_check_always_passes(ISS& env, const DCls& dcls) {
  if (dcls.isExact() || dcls.isSub()) {
    return module_check_always_passes(env, dcls.cls());
  } else if (dcls.isIsect()) {
    for (auto const cls : dcls.isect()) {
      if (module_check_always_passes(env, cls)) return true;
    }
    return false;
  } else {
    assertx(dcls.isIsectAndExact());
    auto const [e, i] = dcls.isectAndExact();
    if (module_check_always_passes(env, e)) return true;
    for (auto const cls : *i) {
      if (module_check_always_passes(env, cls)) return true;
    }
    return false;
  }
}

} // namespace

void in(ISS& env, const bc::ClassGetC& op) {
  auto const kind = static_cast<ClassGetCMode>(op.subop1);
  /**
    * If this is triggered, HH\classname_to_class($foo, "cause_a_sev") was
    * committed to a path that was included in a repo build, which is not
    * allowed. This API is not exposed to Hack and requires HH_FIXME[4105]
    * to suppress the "Too many arguments" error.
    */
  always_assert_flog(
    kind != ClassGetCMode::UnsafeBackdoor,
    "HH\\classname_to_class() unsafe backdoor is not allowed in repo builds"
  );
  auto const t = topC(env);

  if (t.subtypeOf(BCls)) return reduce(env);
  popC(env);

  if (!t.couldBe(BObj | BCls | BStr | BLazyCls)) {
    unreachable(env);
    push(env, TBottom);
    return;
  }

  if (t.subtypeOf(BObj)) {
    switch (kind) {
      case ClassGetCMode::Normal:
        effect_free(env);
        push(env, objcls(t));
        return;
      case ClassGetCMode::ExplicitConversion:
      case ClassGetCMode::UnsafeBackdoor:
      case ClassGetCMode::StrictIsolationBackdoor:
        unreachable(env);
        push(env, TBottom);
        return;
    }
  }

  if (auto const clsname = getNameFromType(t)) {
    if (auto const rcls = env.index.resolve_class(clsname)) {
      auto const may_raise = t.subtypeOf(BStr) && [&] {
        switch (kind) {
          case ClassGetCMode::Normal:
            return true;
          case ClassGetCMode::ExplicitConversion:
            return
              rcls->mightCareAboutDynamicallyReferenced() ||
              rcls->mightBeInStrictPackage();
          case ClassGetCMode::UnsafeBackdoor:
          case ClassGetCMode::StrictIsolationBackdoor:
            return rcls->mightCareAboutDynamicallyReferenced();
        }
      }();

      if (!may_raise &&
          module_check_always_passes(env, *rcls)) {
        effect_free(env);
      }
      push(env, clsExact(*rcls, true));
      return;
    }
    push(env, TBottom);
    unreachable(env);
    return;
  }

  push(env, TCls);
}

void classGetTSImpl(ISS& env, bool pushGenerics) {
  auto const ts = popC(env);
  if (!ts.couldBe(BDict)) {
    push(env, TBottom);
    if (pushGenerics) push(env, TBottom);
    return;
  }

  auto const a = tv(ts);
  if (a && isValidTSType(*a, false)) {
    // HackC emits CombineAndResolveTypeStruct before this so assume resolved
    auto const ts_arr = a->m_data.parr;
    if (auto const name = type_structure_name(ts_arr)) {
      if (auto const rcls = env.index.resolve_class(name)) {
        push(env, clsExact(*rcls, true));
        if (pushGenerics) {
          if (auto const rg = get_ts_generic_types_opt(ts_arr)) {
            push(env, vec_val(rg));
          } else {
            push(env, vec_empty());
          }
        }
        return;
      }
    }
  }

  push(env, TCls);
  if (pushGenerics) push(env, TVec);
}

void in(ISS& env, const bc::ClassGetTS&) {
  classGetTSImpl(env, false);
}

void in(ISS& env, const bc::ClassGetTSWithGenerics&) {
  classGetTSImpl(env, true);
}

void in(ISS& env, const bc::AKExists&) {
  auto const base = popC(env);
  auto const [key, promotion] = promote_classlike_to_key(popC(env));

  auto result = TBottom;
  auto effectFree = promotion != Promotion::YesMightThrow;

  if (!base.subtypeOf(BObj | BArrLike)) {
    effectFree = false;
    result |= TFalse;
  }

  if (base.couldBe(BObj)) {
    effectFree = false;
    result |= TBool;
  }
  if (base.couldBe(BArrLike)) {
    auto const validKey = key.subtypeOf(BArrKey);
    if (!validKey) effectFree = false;
    if (key.couldBe(BArrKey)) {
      auto const elem =
        array_like_elem(base, validKey ? key : intersection_of(key, TArrKey));
      if (elem.first.is(BBottom)) {
        result |= TFalse;
      } else if (elem.second) {
        result |= TTrue;
      } else {
        result |= TBool;
      }
    }
  }

  if (result.is(BBottom)) {
    assertx(!effectFree);
    unreachable(env);
  }
  if (effectFree) {
    constprop(env);
    effect_free(env);
  }
  push(env, std::move(result));
}

void in(ISS& env, const bc::GetMemoKeyL& op) {
  auto const& func = env.ctx.func;
  always_assert(func->isMemoizeWrapper);

  auto const tyIMemoizeParam =
    subObj(builtin_class(env.index, s_IMemoizeParam.get()));

  auto const inTy = locAsCell(env, op.nloc1.id);

  // If the local could be uninit, we might raise a warning (as
  // usual). Converting an object to a memo key might invoke PHP code if it has
  // the IMemoizeParam interface, and if it doesn't, we'll throw.
  if (!locCouldBeUninit(env, op.nloc1.id) &&
      !inTy.couldBe(BObj | BVec | BDict)) {
    effect_free(env);
    constprop(env);
  }

  // If type constraints are being enforced and the local being turned into a
  // memo key is a parameter, then we can possibly using the type constraint to
  // infer a more efficient memo key mode.
  using MK = MemoKeyConstraint;
  bool isMemoizeParam = false;
  auto const mkc = [&] {
    if (op.nloc1.id >= env.ctx.func->params.size()) return MK::None;
    auto const& tcs = env.ctx.func->params[op.nloc1.id].typeConstraints;
    auto isTCMemoizeParam = [&](auto const& tc) -> bool {
      if (!tc.isSubObject()) return false;
      if (tc.isSoft()) return false;
      auto const rcls = env.index.resolve_class(tc.clsName());
      assertx(rcls.has_value());
      return subObj(*rcls).subtypeOf(tyIMemoizeParam);
    };
    isMemoizeParam = std::any_of(
      tcs.range().begin(),
      tcs.range().end(),
      isTCMemoizeParam
    );
    return tcs.getMemoKeyConstraint();
  }();

  // Use the type-constraint to reduce this operation to a more efficient memo
  // mode. Some of the modes can be reduced to simple bytecode operations
  // inline. Even with the type-constraints, we still need to check the inferred
  // type of the local. Something may have possibly clobbered the local between
  // the type-check and this op.
  switch (mkc) {
    case MK::Int:
      // Always an int, so the key is always an identity mapping
      if (inTy.subtypeOf(BInt)) return reduce(env, bc::CGetL { op.nloc1 });
      break;
    case MK::Bool:
      // Always a bool, so the key is the bool cast to an int
      if (inTy.subtypeOf(BBool)) {
        return reduce(env, bc::CGetL { op.nloc1 }, bc::CastInt {});
      }
      break;
    case MK::Str:
      // Always a string, so the key is always an identity mapping
      if (inTy.subtypeOf(BStr)) return reduce(env, bc::CGetL { op.nloc1 });
      break;
    case MK::IntOrStr:
      // Either an int or string, so the key can be an identity mapping
      if (inTy.subtypeOf(BArrKey)) return reduce(env, bc::CGetL { op.nloc1 });
      break;
    case MK::StrOrNull:
      // A nullable string. The key will either be the string or the integer
      // zero.
      if (inTy.subtypeOf(BOptStr)) {
        return reduce(
          env,
          bc::CGetL { op.nloc1 },
          bc::Int { 0 },
          bc::IsTypeL { op.nloc1, IsTypeOp::Null },
          bc::Select {}
        );
      }
      break;
    case MK::IntOrNull:
      // A nullable int. The key will either be the integer, or the static empty
      // string.
      if (inTy.subtypeOf(BOptInt)) {
        return reduce(
          env,
          bc::CGetL { op.nloc1 },
          bc::String { staticEmptyString() },
          bc::IsTypeL { op.nloc1, IsTypeOp::Null },
          bc::Select {}
        );
      }
      break;
    case MK::BoolOrNull:
      // A nullable bool. The key will either be 0, 1, or 2.
      if (inTy.subtypeOf(BOptBool)) {
        return reduce(
          env,
          bc::CGetL { op.nloc1 },
          bc::CastInt {},
          bc::Int { 2 },
          bc::IsTypeL { op.nloc1, IsTypeOp::Null },
          bc::Select {}
        );
      }
      break;
    case MK::Dbl:
      // The double will be converted (losslessly) to an integer.
      if (inTy.subtypeOf(BDbl)) {
        return reduce(env, bc::CGetL { op.nloc1 }, bc::DblAsBits {});
      }
      break;
    case MK::DblOrNull:
      // A nullable double. The key will be an integer, or the static empty
      // string.
      if (inTy.subtypeOf(BOptDbl)) {
        return reduce(
          env,
          bc::CGetL { op.nloc1 },
          bc::DblAsBits {},
          bc::String { staticEmptyString() },
          bc::IsTypeL { op.nloc1, IsTypeOp::Null },
          bc::Select {}
        );
      }
      break;
    case MK::Object:
      // An object. If the object is definitely known to implement IMemoizeParam
      // we can simply call that method, casting the output to ensure its always
      // a string (which is what the generic mode does). If not, it will use the
      // generic mode, which can handle collections or classes which don't
      // implement getInstanceKey.
      if (isMemoizeParam &&
          inTy.subtypeOf(tyIMemoizeParam)) {
        return reduce(
          env,
          bc::CGetL { op.nloc1 },
          bc::NullUninit {},
          bc::FCallObjMethodD {
            FCallArgs(0),
            staticEmptyString(),
            ObjMethodOp::NullThrows,
            s_getInstanceKey.get()
          },
          bc::CastString {}
        );
      }
      break;
    case MK::ObjectOrNull:
      // An object or null. We can use the null safe version of a function call
      // when invoking getInstanceKey and then select from the result of that,
      // or the integer 0. This might seem wasteful, but the JIT does a good job
      // inlining away the call in the null case.
      if (isMemoizeParam &&
          inTy.subtypeOf(opt(tyIMemoizeParam))) {
        return reduce(
          env,
          bc::CGetL { op.nloc1 },
          bc::NullUninit {},
          bc::FCallObjMethodD {
            FCallArgs(0),
            staticEmptyString(),
            ObjMethodOp::NullSafe,
            s_getInstanceKey.get()
          },
          bc::CastString {},
          bc::Int { 0 },
          bc::IsTypeL { op.nloc1, IsTypeOp::Null },
          bc::Select {}
        );
      }
      break;
    default:
      break;
  }

  // No type constraint, or one that isn't usuable. Use the generic memoization
  // scheme which can handle any type:

  if (auto const val = tv(inTy)) {
    auto const key = eval_cell(
      [&]{ return HHVM_FN(serialize_memoize_param)(*val); }
    );
    if (key) return push(env, *key);
  }

  // Integer keys are always mapped to themselves
  if (inTy.subtypeOf(BInt)) return reduce(env, bc::CGetL { op.nloc1 });
  if (inTy.subtypeOf(BOptInt)) {
    return reduce(
      env,
      bc::CGetL { op.nloc1 },
      bc::String { s_nullMemoKey.get() },
      bc::IsTypeL { op.nloc1, IsTypeOp::Null },
      bc::Select {}
    );
  }
  if (inTy.subtypeOf(BBool)) {
    return reduce(
      env,
      bc::String { s_falseMemoKey.get() },
      bc::String { s_trueMemoKey.get() },
      bc::CGetL { op.nloc1 },
      bc::Select {}
    );
  }

  // A memo key can be an integer if the input might be an integer, and is a
  // string otherwise. Booleans and nulls are always static strings.
  auto keyTy = [&]{
    if (inTy.subtypeOf(BOptBool)) return TSStr;
    if (inTy.couldBe(BInt))       return union_of(TInt, TStr);
    return TStr;
  }();
  push(env, std::move(keyTy));
}

void in(ISS& env, const bc::IssetL& op) {
  if (locIsThis(env, op.loc1)) {
    return reduce(env,
                  bc::BareThis { BareThisOp::NoNotice },
                  bc::IsTypeC { IsTypeOp::Null },
                  bc::Not {});
  }
  effect_free(env);
  constprop(env);
  auto const loc = locAsCell(env, op.loc1);
  if (loc.subtypeOf(BNull))  return push(env, TFalse);
  if (!loc.couldBe(BNull))   return push(env, TTrue);
  push(env, TBool);
}

void in(ISS& env, const bc::IsUnsetL& op) {
  effect_free(env);
  constprop(env);
  auto const loc = locRaw(env, op.loc1);
  if (loc.subtypeOf(BUninit))  return push(env, TTrue);
  if (!loc.couldBe(BUninit))   return push(env, TFalse);
  push(env, TBool);
}

void in(ISS& env, const bc::IssetS& op) {
  auto const tcls  = popC(env);
  auto const tname = popC(env);

  if (!tcls.couldBe(BCls)) {
    unreachable(env);
    return push(env, TBottom);
  }

  auto lookup = env.index.lookup_static(
    env.ctx,
    env.collect.props,
    tcls,
    tname
  );

  if (!lookup.classInitMightRaise &&
      lookup.internal == TriBool::No &&
      tcls.subtypeOf(BCls) &&
      tname.subtypeOf(BStr)) {
    effect_free(env);
    constprop(env);
  }

  if (lookup.ty.subtypeOf(BNull)) return push(env, TFalse);
  if (!lookup.ty.couldBe(BNull) && lookup.lateInit == TriBool::No) {
    return push(env, TTrue);
  }
  push(env, TBool);
}

void in(ISS& env, const bc::IssetG&) { popC(env); push(env, TBool); }

void isTypeObj(ISS& env, const Type& ty) {
  if (!ty.couldBe(BObj)) return push(env, TFalse);
  if (ty.subtypeOf(BObj)) {
    auto const incompl = objExact(
      builtin_class(env.index, s_PHP_Incomplete_Class.get()));
    if (Cfg::Eval::BuildMayNoticeOnMethCallerHelperIsObject) {
      auto const c =
        objExact(builtin_class(env.index, s_MethCallerHelper.get()));
      if (ty.couldBe(c)) return push(env, TBool);
    }
    if (!ty.couldBe(incompl))  return push(env, TTrue);
    if (ty.subtypeOf(incompl)) return push(env, TFalse);
  }
  push(env, TBool);
}

void isTypeImpl(ISS& env, const Type& locOrCell, IsTypeOp subop) {
  switch (subop) {
    case IsTypeOp::Scalar: return push(env, TBool);
    case IsTypeOp::LegacyArrLike: return push(env, TBool);
    case IsTypeOp::Obj: return isTypeObj(env, locOrCell);
    case IsTypeOp::Func:
      // If it is TFunc, it may still be meth_caller.
      if (locOrCell.couldBe(TFunc)) return push(env, TBool);
      break;
    default: break;
  }

  auto const test = type_of_istype(subop);
  if (locOrCell.subtypeOf(test))  return push(env, TTrue);
  if (!locOrCell.couldBe(test))   return push(env, TFalse);
  push(env, TBool);
}

template<class Op>
void isTypeLImpl(ISS& env, const Op& op) {
  auto const loc = locAsCell(env, op.nloc1.id);
  if (!locCouldBeUninit(env, op.nloc1.id) &&
      !is_type_might_raise(op.subop2, loc)) {
    constprop(env);
    effect_free(env);
  }

  isTypeImpl(env, loc, op.subop2);
}

template<class Op>
void isTypeCImpl(ISS& env, const Op& op) {
  auto const t1 = popC(env);
  if (!is_type_might_raise(op.subop1, t1)) {
    constprop(env);
    effect_free(env);
  }

  isTypeImpl(env, t1, op.subop1);
}

void in(ISS& env, const bc::IsTypeC& op) { isTypeCImpl(env, op); }
void in(ISS& env, const bc::IsTypeL& op) { isTypeLImpl(env, op); }

void in(ISS& env, const bc::InstanceOfD& op) {
  auto t1 = topC(env);
  // Note: InstanceOfD can do autoload if the type might be a type
  // alias, so it's not nothrow unless we know it's an object type.
  if (auto const rcls = env.index.resolve_class(op.str1)) {
    auto result = [&] (const Type& r) {
      nothrow(env);

      if (!r.is(BBool)) constprop(env);
      popC(env);
      push(env, r);
    };
    if (!interface_supports_non_objects(rcls->name())) {
      auto const testTy = subObj(*rcls);
      if (t1.subtypeOf(testTy)) return result(TTrue);
      if (!t1.couldBe(testTy)) return result(TFalse);
      if (t1.couldBe(BInitNull) && !t1.subtypeOf(BInitNull)) {
        t1 = unopt(std::move(t1));
        if (t1.subtypeOf(testTy)) {
          return reduce(env, bc::IsTypeC { IsTypeOp::Null }, bc::Not {});
        }
      }
      return result(TBool);
    }
  } else {
    // The class doesn't exist, so we can never have an instance of
    // it.
    popC(env);
    push(env, TFalse);
    return;
  }
  popC(env);
  push(env, TBool);
}

void in(ISS& env, const bc::InstanceOf& /*op*/) {
  auto const t1 = topC(env);
  auto const v1 = tv(t1);
  if (v1 && v1->m_type == KindOfString) {
    return reduce(env, bc::PopC {},
                       bc::InstanceOfD { v1->m_data.pstr });
  }

  if (t1.subtypeOf(BObj) && is_specialized_obj(t1)) {
    auto const& dobj = dobj_of(t1);
    if (dobj.isExact()) {
      return reduce(env, bc::PopC {},
                         bc::InstanceOfD { dobj.cls().name() });
    }
  }

  popC(env);
  popC(env);
  push(env, TBool);
}

void in(ISS& env, const bc::IsLateBoundCls& op) {
  auto const cls = env.ctx.cls;
  if (cls && !(cls->attrs & AttrTrait)) effect_free(env);
  popC(env);
  return push(env, TBool);
}

namespace {

bool isValidTypeOpForIsAs(const IsTypeOp& op) {
  switch (op) {
    case IsTypeOp::Null:
    case IsTypeOp::Bool:
    case IsTypeOp::Int:
    case IsTypeOp::Dbl:
    case IsTypeOp::Str:
    case IsTypeOp::Obj:
      return true;
    case IsTypeOp::Res:
    case IsTypeOp::Vec:
    case IsTypeOp::Dict:
    case IsTypeOp::Keyset:
    case IsTypeOp::ArrLike:
    case IsTypeOp::LegacyArrLike:
    case IsTypeOp::Scalar:
    case IsTypeOp::ClsMeth:
    case IsTypeOp::Func:
    case IsTypeOp::Class:
      return false;
  }
  not_reached();
}

void isTypeStructImpl(ISS& env, SArray inputTS) {
  auto const ts = inputTS;
  auto const t = loosen_likeness(topC(env, 1)); // operand to is/as

  bool may_raise = true;
  auto result = [&] (const Type& out) {
    popC(env); // type structure
    popC(env); // operand to is/as
    constprop(env);
    if (!may_raise) nothrow(env);
    return push(env, out);
  };

  auto check = [&] (
    const Optional<Type> type,
    const Optional<Type> deopt = std::nullopt
  ) {
    if (!type || is_type_might_raise(*type, t)) return result(TBool);
    auto test = type.value();
    if (t.subtypeOf(test)) return result(TTrue);
    if (!t.couldBe(test) && (!deopt || !t.couldBe(deopt.value()))) {
      return result(TFalse);
    }
    auto const op = type_to_istypeop(test);
    if (!op || !isValidTypeOpForIsAs(op.value())) return result(TBool);
    return reduce(env, bc::PopC {}, bc::IsTypeC { *op });
  };

  auto const is_nullable_ts = is_ts_nullable(ts);
  auto const is_definitely_null = t.subtypeOf(BNull);
  auto const is_definitely_not_null = !t.couldBe(BNull);

  if (is_nullable_ts && is_definitely_null) return result(TTrue);

  auto const ts_type = type_of_type_structure(env.index, env.ctx, ts);

  if (is_nullable_ts && !is_definitely_not_null && ts_type == std::nullopt) {
    // Ts is nullable and we know that t could be null but we dont know for sure
    // Also we didn't get a type out of the type structure
    return result(TBool);
  }

  if (ts_type && !is_type_might_raise(*ts_type, t)) may_raise = false;
  switch (get_ts_kind(ts)) {
    case TypeStructure::Kind::T_int:
    case TypeStructure::Kind::T_bool:
    case TypeStructure::Kind::T_float:
    case TypeStructure::Kind::T_string:
    case TypeStructure::Kind::T_num:
    case TypeStructure::Kind::T_arraykey:
    case TypeStructure::Kind::T_keyset:
    case TypeStructure::Kind::T_void:
    case TypeStructure::Kind::T_null:
      return check(ts_type);
    case TypeStructure::Kind::T_tuple:
      return check(ts_type, TVec);
    case TypeStructure::Kind::T_shape:
      return check(ts_type, TDict);
    case TypeStructure::Kind::T_dict:
      return check(ts_type);
    case TypeStructure::Kind::T_vec:
      return check(ts_type);
    case TypeStructure::Kind::T_nothing:
    case TypeStructure::Kind::T_noreturn:
      return result(TFalse);
    case TypeStructure::Kind::T_mixed:
    case TypeStructure::Kind::T_dynamic:
      return result(TTrue);
    case TypeStructure::Kind::T_nonnull:
      if (is_definitely_null) return result(TFalse);
      if (is_definitely_not_null) return result(TTrue);
      return reduce(env,
                    bc::PopC {},
                    bc::IsTypeC { IsTypeOp::Null },
                    bc::Not {});
    case TypeStructure::Kind::T_class:
    case TypeStructure::Kind::T_interface:
    case TypeStructure::Kind::T_xhp: {
      auto clsname = get_ts_classname(ts);
      auto const rcls = env.index.resolve_class(clsname);
      if (!rcls) return result(TBool);
      if (ts->exists(s_generic_types)) {
        if (!isTSAllWildcards(ts)) return result(TBool);
        if (rcls->couldHaveReifiedGenerics()) return result(TBool);
      }
      return reduce(env, bc::PopC {}, bc::InstanceOfD { clsname });
    }
    case TypeStructure::Kind::T_unresolved: {
      auto classname = get_ts_classname(ts);
      auto const has_generics = ts->exists(s_generic_types);
      if (!has_generics && classname->tsame(s_this.get())) {
        return reduce(env, bc::PopC {}, bc::IsLateBoundCls {});
      }
      auto const rcls = env.index.resolve_class(classname);
      // We can only reduce to instance of if we know for sure that this class
      // can be resolved since instanceof undefined class does not throw
      if (!rcls) return result(TBool);
      auto const cls = rcls->cls();
      if (!cls || cls->attrs & AttrEnum) return result(TBool);
      if (has_generics && (cls->hasReifiedGenerics || !isTSAllWildcards(ts))) {
        // If it is a reified class or has non wildcard generics, we
        // need to bail
        return result(TBool);
      }
      return reduce(env, bc::PopC {}, bc::InstanceOfD { rcls->name() });
    }
    case TypeStructure::Kind::T_resource: {
      if (!ts_type || is_type_might_raise(*ts_type, t)) return result(TBool);
      if (!t.couldBe(*ts_type)) return result(TFalse);
      return result(TBool);
    }
    case TypeStructure::Kind::T_enum:
    case TypeStructure::Kind::T_vec_or_dict:
    case TypeStructure::Kind::T_any_array:
    case TypeStructure::Kind::T_union:
    case TypeStructure::Kind::T_recursiveUnion:
      // TODO(T29232862): implement
      return result(TBool);
    case TypeStructure::Kind::T_typeaccess:
    case TypeStructure::Kind::T_darray:
    case TypeStructure::Kind::T_varray:
    case TypeStructure::Kind::T_varray_or_darray:
    case TypeStructure::Kind::T_reifiedtype:
      return result(TBool);
    case TypeStructure::Kind::T_fun:
    case TypeStructure::Kind::T_typevar:
    case TypeStructure::Kind::T_trait:
      // We will error on these at the JIT
      return result(TBool);
    case TypeStructure::Kind::T_class_ptr:
      // TODO(T199611023) Do nothing until we're ready to enforce the inner type
      return result(TBool);
    case TypeStructure::Kind::T_class_or_classname:
      return result(TBool);
  }

  not_reached();
}

const StaticString s_hh_type_structure_no_throw("HH\\type_structure_no_throw");

} // namespace

void in(ISS& env, const bc::IsTypeStructC& op) {
  if (!topC(env).couldBe(BDict)) {
    popC(env);
    popC(env);
    return unreachable(env);
  }
  auto const a = tv(topC(env));
  if (!a || !isValidTSType(*a, false)) {
    popC(env);
    popC(env);
    return push(env, TBool);
  }
  if (op.subop1 == TypeStructResolveOp::Resolve) {
    if (auto const ts = resolve_type_structure(env, a->m_data.parr).sarray()) {
      return reduce(
        env,
        bc::PopC {},
        bc::Dict { ts },
        bc::IsTypeStructC { TypeStructResolveOp::DontResolve, op.subop2 }
      );
    }
    if (auto const val = get_ts_this_type_access(a->m_data.parr)) {
      // Convert `$x is this::T` into
      // `$x is type_structure_no_throw(static::class, 'T')`
      // to take advantage of the caching that comes with the type_structure
      return reduce(
        env,
        bc::PopC {},
        bc::NullUninit {},
        bc::NullUninit {},
        bc::LateBoundCls {},
        bc::String {val},
        bc::FCallFuncD {FCallArgs(2), s_hh_type_structure_no_throw.get()},
        bc::IsTypeStructC { TypeStructResolveOp::DontResolve, op.subop2 }
      );
    }
  }
  isTypeStructImpl(env, a->m_data.parr);
}

void in(ISS& env, const bc::ThrowAsTypeStructException& op) {
  popC(env);
  popC(env);
  unreachable(env);
}

void in(ISS& env, const bc::CombineAndResolveTypeStruct& op) {
  assertx(op.arg1 > 0);
  auto valid = true;
  auto const first = tv(topC(env));
  if (first && isValidTSType(*first, false)) {
    auto const ts = first->m_data.parr;
    // Optimize single input that does not need any combination
    if (op.arg1 == 1) {
      if (auto const r = resolve_type_structure(env, ts).sarray()) {
        return reduce(
          env,
          bc::PopC {},
          bc::Dict { r }
        );
      }
    }

    // Optimize double input that needs a single combination and looks of the
    // form ?T or @T
    if (op.arg1 == 2 && get_ts_kind(ts) == TypeStructure::Kind::T_reifiedtype) {
      BytecodeVec instrs { bc::PopC {} };
      auto const tv_true = gen_constant(make_tv<KindOfBoolean>(true));
      if (ts->exists(s_nullable.get())) {
        instrs.push_back(gen_constant(make_tv<KindOfString>(s_nullable.get())));
        instrs.push_back(tv_true);
        instrs.push_back(bc::AddElemC {});
      }
      if (ts->exists(s_soft.get())) {
        instrs.push_back(gen_constant(make_tv<KindOfString>(s_soft.get())));
        instrs.push_back(tv_true);
        instrs.push_back(bc::AddElemC {});
      }
      return reduce(env, std::move(instrs));
    }
  }

  for (int i = 0; i < op.arg1; ++i) {
    auto const t = popC(env);
    valid &= t.couldBe(BDict);
  }
  if (!valid) return unreachable(env);
  nothrow(env);
  push(env, TDict);
}

void in(ISS& env, const bc::RecordReifiedGeneric& op) {
  // TODO(T31677864): implement real optimizations
  auto const t = popC(env);
  if (!t.couldBe(BVec)) return unreachable(env);
  if (t.subtypeOf(BVec)) nothrow(env);
  push(env, TSVec);
}

void in(ISS& env, const bc::CheckClsReifiedGenericMismatch& op) {
  auto const location = topStkEquiv(env, 0);
  popC(env);

  if (location == NoLocalId) return;
  auto const ok = refineLocation(
    env, location,
    [&] (Type) {
      return get_type_of_reified_list(env.ctx.cls->userAttributes);
    }
  );
  if (!ok) unreachable(env);
}

void in(ISS& env, const bc::ClassHasReifiedGenerics& op) {
  // TODO(T121050961) Optimize for lazy classes too
  auto const cls = popC(env);
  if (!cls.couldBe(BCls | BLazyCls)) {
    unreachable(env);
    return push(env, TBottom);
  }
  if (!cls.subtypeOf(BCls)) {
    push(env, TBool);
    return;
  }
  effect_free(env);
  constprop(env);
  auto const t = [&] {
    if (!is_specialized_cls(cls) || !dcls_of(cls).isExact()) {
      return TBool;
    }
    auto const& dcls = dcls_of(cls);
    if (!dcls.cls().couldHaveReifiedGenerics()) {
      return TFalse;
    }
    if (dcls.cls().mustHaveReifiedGenerics()) {
      return TTrue;
    }
    return TBool;
  }();
  push(env, t);
}

void in(ISS& env, const bc::GetClsRGProp& op) {
  // TODO(T121050961) Optimize for lazy classes too
  auto const cls = popC(env);
  if (!thisAvailable(env) || !cls.couldBe(BCls | BLazyCls)) {
    unreachable(env);
    return push(env, TBottom);
  }
  if (!cls.subtypeOf(BCls) ||
      !is_specialized_cls(cls) ||
      !dcls_of(cls).isExact()) {
    push(env, TVec);
    return;
  }
  auto const &dcls = dcls_of(cls);
  if (!dcls.cls().couldHaveReifiedGenerics()) {
    push(env, TInitNull);
    return;
  }
  push(env, TVec);
}

void in(ISS& env, const bc::HasReifiedParent& op) {
  // TODO(T121050961) Optimize for lazy classes too
  auto const cls = popC(env);
  if (!cls.couldBe(BCls | BLazyCls)) {
    unreachable(env);
    return push(env, TBottom);
  }
  if (!cls.subtypeOf(BCls)) {
    push(env, TBool);
    return;
  }
  effect_free(env);
  constprop(env);
  auto const t = [&] {
    if (!is_specialized_cls(cls) || !dcls_of(cls).isExact()) {
      return TBool;
    }
    auto const& dcls = dcls_of(cls);
    if (!dcls.cls().couldHaveReifiedParent()) {
      return TFalse;
    }
    if (dcls.cls().mustHaveReifiedParent()) {
      return TTrue;
    }
    return TBool;
  }();
  push(env, t);
}

void in(ISS& env, const bc::CheckClsRGSoft& op) {
  // TODO(T121050961) Optimize for lazy classes too
  auto const cls = popC(env);
  if (!cls.couldBe(BCls | BLazyCls)) {
    unreachable(env);
    return;
  }
  if (!cls.subtypeOf(BCls) ||
      !is_specialized_cls(cls) ||
      !dcls_of(cls).isExact()) {
    return;
  }
  auto const &dcls = dcls_of(cls);
  if (!dcls.cls().couldHaveReifiedGenerics()) {
    unreachable(env);
  }
}

void in(ISS& env, const bc::ReifiedInit& op) {
  auto const cls = topC(env);
  mayReadLocal(env, op.loc1);
  if (!cls.couldBe(BCls | BLazyCls)) {
    return reduce(env, bc::PopC {}, bc::PopC {});
  }
  if (cls.subtypeOf(BCls) &&
      is_specialized_cls(cls) &&
      dcls_of(cls).isExact()) {
    auto const& dcls = dcls_of(cls);
    if (!dcls.cls().couldHaveReifiedGenerics() &&
        !dcls.cls().couldHaveReifiedParent()) {
      // ReifiedInit is a no-op if the class isn't reified
      // and doesn't have a reified parent.
      return reduce(env, bc::PopC {}, bc::PopC {});
    }

    // if the class has reified generics, and if generics
    // were not specified at instantiation, then we must emit
    // CheckClsRgSoft to raise a warning or fatal.
    auto const cell = locAsCell(env, op.loc1);
    if (dcls.cls().mustHaveReifiedGenerics() && cell.subtypeOf(BVecE)) {
      return reduce(
        env,
        bc::CheckClsRGSoft {},
        bc::PopC {}
      );
    }

    if (dcls.cls().mustHaveReifiedGenerics() ||
        dcls.cls().mustHaveReifiedParent()) {
      return reduce(
        env,
        bc::PopC {}, // pop the class off the stack
        bc::NullUninit {},
        bc::CGetQuietL { op.loc1 }, // push generics onto stack
        bc::FCallObjMethodD {
          FCallArgs(1),
          staticEmptyString(),
          ObjMethodOp::NullThrows,
          s_86reifiedinit.get()
        },
        bc::PopC {}
      );
    }
  }
  popC(env);
  popC(env);
}

namespace {

/*
 * If the value on the top of the stack is known to be equivalent to the local
 * its being moved/copied to, return std::nullopt without modifying any
 * state. Otherwise, pop the stack value, perform the set, and return a pair
 * giving the value's type, and any other local its known to be equivalent to.
 */
template <typename Set>
Optional<std::pair<Type, LocalId>> moveToLocImpl(ISS& env,
                                                 const Set& op) {
  if (auto const prev = last_op(env, 1)) {
    if (prev->op == Op::CGetL2 &&
        prev->CGetL2.nloc1.id == op.loc1 &&
        last_op(env)->op == Op::Concat && can_rewind(env) &&
        // Concat converts its left operand to string first, ConcatEqual its
        // right, so the two only agree when converting the local cannot
        // throw. A double left operand throws InvalidOperationException.
        peekLocRaw(env, op.loc1).subtypeOf(BArrKey)) {
      rewind(env, 2);
      reduce(env, bc::SetOpL { op.loc1, SetOpOp::ConcatEqual });
      return std::nullopt;
    }
  }

  auto equivLoc = topStkEquiv(env);
  // If the local could be a Ref, don't record equality because the stack
  // element and the local won't actually have the same type.
  if (equivLoc == StackThisId && env.state.thisLoc != NoLocalId) {
    if (env.state.thisLoc == op.loc1 ||
               locsAreEquiv(env, env.state.thisLoc, op.loc1)) {
      return std::nullopt;
    } else {
      equivLoc = env.state.thisLoc;
    }
  }
  if (!is_volatile_local(env.ctx.func, op.loc1)) {
    if (equivLoc <= MaxLocalId) {
      if (equivLoc == op.loc1 ||
          locsAreEquiv(env, equivLoc, op.loc1)) {
        // We allow equivalency to ignore Uninit, so we need to check
        // the types here.
        if (equal(peekLocRaw(env, op.loc1), topC(env))) {
          return std::nullopt;
        }
      }
    } else if (equivLoc == NoLocalId) {
      equivLoc = op.loc1;
    }
    if (!any(env.collect.opts & CollectionOpts::Speculating)) {
      effect_free(env);
    }
  } else {
    equivLoc = NoLocalId;
  }
  nothrow(env);
  auto const iterKeyEquiv = env.state.topStkIterKeyEquiv;
  auto val = popC(env);
  setLoc(env, op.loc1, val);
  if (equivLoc == StackThisId) {
    assertx(env.state.thisLoc == NoLocalId);
    equivLoc = env.state.thisLoc = op.loc1;
  }
  if (equivLoc == StackDupId) {
    setStkLocal(env, op.loc1);
  } else if (equivLoc != op.loc1 && equivLoc != NoLocalId) {
    addLocEquiv(env, op.loc1, equivLoc);
  }
  if (iterKeyEquiv != NoIterId) {
    setIterKey(env, iterKeyEquiv, op.loc1);
  }
  return { std::make_pair(std::move(val), equivLoc) };
}

}

void in(ISS& env, const bc::PopL& op) {
  // If the same value is already in the local, do nothing but pop
  // it. Otherwise, the set has been done by moveToLocImpl.
  if (!moveToLocImpl(env, op)) return reduce(env, bc::PopC {});
}

void in(ISS& env, const bc::SetL& op) {
  // If the same value is already in the local, do nothing because SetL keeps
  // the value on the stack. If it isn't, we need to push it back onto the stack
  // because moveToLocImpl popped it.
  if (auto p = moveToLocImpl(env, op)) {
    push(env, std::move(p->first), p->second);
  } else {
    reduce(env);
  }
}

void in(ISS& env, const bc::PopG&) {
  popC(env);
  popC(env);
}

void in(ISS& env, const bc::SetS& op) {
  auto const val   = popC(env);
  auto const tcls  = popC(env);
  auto const tname = popC(env);

  auto const throws = [&] {
    unreachable(env);
    return push(env, TBottom);
  };

  if (!tcls.couldBe(BCls)) return throws();

  auto merge = mergeStaticProp(
    env, tcls, tname, val, true, false,
    op.subop1 == ReadonlyOp::Readonly
  );
  if (merge.throws == TriBool::Yes || merge.adjusted.subtypeOf(BBottom)) {
    return throws();
  }

  if (merge.throws == TriBool::No &&
      tcls.subtypeOf(BCls) &&
      tname.subtypeOf(BStr)) {
    nothrow(env);
  }

  push(env, std::move(merge.adjusted));
}

void in(ISS& env, const bc::SetOpL& op) {
  auto const t1     = popC(env);
  auto const loc    = locAsCell(env, op.loc1);

  auto resultTy = typeSetOp(op.subop2, loc, t1);
  setLoc(env, op.loc1, resultTy);
  if (resultTy.is(BBottom)) unreachable(env);
  push(env, std::move(resultTy));
}

void in(ISS& env, const bc::SetOpS& op) {
  auto const rhs   = popC(env);
  auto const tcls  = popC(env);
  auto const tname = popC(env);

  auto const throws = [&] {
    unreachable(env);
    return push(env, TBottom);
  };

  if (!tcls.couldBe(BCls)) return throws();

  auto const lookup = env.index.lookup_static(
    env.ctx,
    env.collect.props,
    tcls,
    tname
  );

  if (lookup.found == TriBool::No || lookup.ty.subtypeOf(BBottom)) {
    return throws();
  }

  auto const newTy = typeSetOp(op.subop1, lookup.ty, rhs);
  if (newTy.subtypeOf(BBottom)) return throws();

  auto merge = mergeStaticProp(env, tcls, tname, newTy);
  if (merge.throws == TriBool::Yes || merge.adjusted.subtypeOf(BBottom)) {
    return throws();
  }

  // NB: Unlike IncDecS, SetOpS pushes the post-TypeConstraint
  // adjustment value.
  push(env, std::move(merge.adjusted));
}

void in(ISS& env, const bc::IncDecL& op) {
  auto loc = locAsCell(env, op.nloc1.id);
  auto newT = typeIncDec(op.subop2, loc);

  if (newT.subtypeOf(BBottom)) {
    unreachable(env);
    return push(env, TBottom);
  }

  if (!locCouldBeUninit(env, op.nloc1.id) && loc.subtypeOf(BNum)) nothrow(env);

  auto const pre = isPre(op.subop2);
  if (!pre) push(env, std::move(loc));
  setLoc(env, op.nloc1.id, newT);
  if (pre)  push(env, std::move(newT));
}

void in(ISS& env, const bc::IncDecS& op) {
  auto const tcls  = popC(env);
  auto const tname = popC(env);
  auto const pre = isPre(op.subop1);

  auto const throws = [&] {
    unreachable(env);
    return push(env, TBottom);
  };

  if (!tcls.couldBe(BCls)) return throws();

  auto lookup = env.index.lookup_static(
    env.ctx,
    env.collect.props,
    tcls,
    tname
  );

  if (lookup.found == TriBool::No || lookup.ty.subtypeOf(BBottom)) {
    return throws();
  }

  auto newTy = typeIncDec(op.subop1, lookup.ty);
  if (newTy.subtypeOf(BBottom)) return throws();

  auto const merge = mergeStaticProp(env, tcls, tname, newTy);
  if (merge.throws == TriBool::Yes || merge.adjusted.subtypeOf(BBottom)) {
    return throws();
  }

  if (lookup.found == TriBool::Yes &&
      lookup.lateInit == TriBool::No &&
      lookup.internal == TriBool::No &&
      !lookup.classInitMightRaise &&
      merge.throws == TriBool::No &&
      tcls.subtypeOf(BCls) &&
      tname.subtypeOf(BStr) &&
      lookup.ty.subtypeOf(BNum)) {
    nothrow(env);
  }

  // NB: IncDecS pushes the value pre-TypeConstraint modification
  push(env, pre ? std::move(newTy) : std::move(lookup.ty));
}

void in(ISS& env, const bc::UnsetL& op) {
  // Peek so that we don't register a ready on the local if we're
  // going to optimize this away.
  if (peekLocRaw(env, op.loc1).subtypeOf(TUninit)) {
    return reduce(env);
  }

  if (auto const last = last_op(env)) {
    // No point in popping into the local if we're just going to
    // immediately unset it.
    if (last->op == Op::PopL &&
        last->PopL.loc1 == op.loc1 &&
        can_rewind(env)) {
      reprocess(env);
      rewind(env, 1);
      setLocRaw(env, op.loc1, TCell);
      return reduce(env, bc::PopC {}, bc::UnsetL { op.loc1 });
    }
  }

  if (any(env.collect.opts & CollectionOpts::Speculating)) {
    nothrow(env);
  } else {
    effect_free(env);
  }
  setLocRaw(env, op.loc1, TUninit);
}

void in(ISS& env, const bc::UnsetG& /*op*/) {
  auto const t1 = popC(env);
  if (!t1.couldBe(BObj | BRes)) nothrow(env);
}

bool fcallCanSkipRepack(ISS& env, const FCallArgs& fca, const res::Func& func) {
  // Can't skip repack if potentially calling a function with too many args.
  if (fca.numArgs() > func.minNonVariadicParams()) return false;
  // Repack not needed if not unpacking and not having too many arguments.
  if (!fca.hasUnpack()) return true;
  // Can't skip repack if unpack args are in a wrong position.
  if (fca.numArgs() != func.maxNonVariadicParams()) return false;

  // Repack not needed if unpack args have the correct type.
  auto const unpackArgs = topC(env, fca.hasGenerics() ? 1 : 0);
  return unpackArgs.subtypeOf(BVec);
}

bool coeffectRulesMatch(ISS& env,
                        const FCallArgs& fca,
                        const res::Func& func,
                        uint32_t numExtraInputs,
                        const CoeffectRule& caller,
                        const CoeffectRule& callee) {
  if (caller.m_type != callee.m_type) return false;
  switch (caller.m_type) {
    case CoeffectRule::Type::CCThis: {
      if (caller.m_name != callee.m_name ||
          caller.m_types != callee.m_types) {
        return false;
      }
      if (!thisAvailable(env)) return false;
      auto const loc = topStkEquiv(env, fca.numInputs() + numExtraInputs + 1);
      return loc == StackThisId || (loc <= MaxLocalId && locIsThis(env, loc));
    }
    case CoeffectRule::Type::CCParam:
      if (caller.m_name != callee.m_name) return false;
      [[fallthrough]];
    case CoeffectRule::Type::FunParam: {
      if (fca.hasUnpack()) return false;
      if (fca.numArgs() <= callee.m_index) return false;
      auto const l1 = caller.m_index;
      auto const l2 = topStkEquiv(env, fca.numInputs() - callee.m_index - 1);
      return l1 == l2 ||
             (l1 <= MaxLocalId &&
              l2 <= MaxLocalId &&
              locsAreEquiv(env, l1, l2));
    }
    case CoeffectRule::Type::CCReified:
      // TODO: optimize these
      return false;
    case CoeffectRule::Type::ClosureParentScope:
    case CoeffectRule::Type::GeneratorThis:
    case CoeffectRule::Type::Caller:
    case CoeffectRule::Type::Invalid:
      return false;
  }
  not_reached();
}

bool fcallCanSkipCoeffectsCheck(ISS& env,
                                const FCallArgs& fca,
                                const res::Func& func,
                                uint32_t numExtraInputs) {
  auto const requiredCoeffectsOpt = func.requiredCoeffects();
  if (!requiredCoeffectsOpt) return false;
  auto const required = *requiredCoeffectsOpt;
  auto const provided =
    RuntimeCoeffects::fromValue(env.ctx.func->requiredCoeffects.value() |
                                env.ctx.func->coeffectEscapes.value());
  if (!provided.canCall(required)) return false;
  auto const calleeRules = func.coeffectRules();
  // If we couldn't tell whether callee has rules or not, punt.
  if (!calleeRules) return false;
  if (calleeRules->empty()) return true;
  if (calleeRules->size() == 1 && (*calleeRules)[0].isCaller()) return true;
  auto const callerRules = env.ctx.func->coeffectRules;
  return std::is_permutation(callerRules.begin(), callerRules.end(),
                             calleeRules->begin(), calleeRules->end(),
                             [&] (const CoeffectRule& a,
                                  const CoeffectRule& b) {
                               return coeffectRulesMatch(env, fca, func,
                                                         numExtraInputs,
                                                         a, b);
                              });
}

template<typename FCallWithFCA>
bool fcallOptimizeChecks(
  ISS& env,
  const FCallArgs& fca,
  const res::Func& func,
  FCallWithFCA fcallWithFCA,
  Optional<uint32_t> inOutNum,
  bool maybeNullsafe,
  uint32_t numExtraInputs
) {
  // TODO(named_params) we don't optimize away checks if named params
  // or args exists right now and can do better.
  if (fca.namedArgNames() != nullptr || func.mightHaveNamedParams()) {
    return false;
  }
  // Don't optimize away in-out checks if we might use the null safe
  // operator. If we do so, we need the in-out bits to shuffle the
  // stack properly.
  if (!maybeNullsafe && fca.enforceInOut()) {
    if (inOutNum == fca.numRets() - 1) {
      bool match = true;
      for (auto i = 0; i < fca.numArgs(); ++i) {
        auto const kind = func.lookupParamPrep(i);
        if (kind.inOut == TriBool::Maybe) {
          match = false;
          break;
        }

        if (yesOrNo(fca.isInOut(i)) != kind.inOut) {
          // The function/method may not exist, in which case we should raise a
          // different error. Just defer the checks to the runtime.
          auto const exact = func.exactFunc();
          if (!exact) return false;

          // inout mismatch
          auto const exCls = makeStaticString("InvalidArgumentException");
          auto const err = makeStaticString(
            formatParamInOutMismatch(
              exact->name->data(),
              i,
              !fca.isInOut(i)
            )
          );

          reduce(
            env,
            bc::NewObjD { exCls },
            bc::Dup {},
            bc::NullUninit {},
            bc::String { err },
            bc::FCallCtor { FCallArgs(1), staticEmptyString() },
            bc::PopC {},
            bc::LockObj {},
            bc::Throw {}
          );
          return true;
        }
      }

      if (match) {
        // Optimize away the runtime inout-ness check.
        reduce(env, fcallWithFCA(fca.withoutInOut()));
        return true;
      }
    }
  }

  if (fca.enforceReadonly()) {
    bool match = true;
    for (auto i = 0; i < fca.numArgs(); ++i) {
      if (!fca.isReadonly(i)) continue;
      auto const kind = func.lookupParamPrep(i);
      if (kind.readonly == TriBool::Maybe) {
        match = false;
        break;
      }

      if (kind.readonly != TriBool::Yes) {
        // The function/method may not exist, in which case we should raise a
        // different error. Just defer the checks to the runtime.
        if (!func.exactFunc()) return false;
        match = false;
        break;
      }
    }

    if (match) {
      // Optimize away the runtime readonly-ness check.
      reduce(env, fcallWithFCA(fca.withoutReadonly()));
      return true;
    }
  }

  if (fca.enforceMutableReturn()) {
    if (func.lookupReturnReadonly() == TriBool::No) {
      reduce(env, fcallWithFCA(fca.withoutEnforceMutableReturn()));
      return true;
    }
  }

  if (fca.enforceReadonlyThis()) {
    if (func.lookupReadonlyThis() == TriBool::Yes) {
      reduce(env, fcallWithFCA(fca.withoutEnforceReadonlyThis()));
      return true;
    }
  }

  // Infer whether the callee supports async eager return.
  if (fca.asyncEagerTarget() != NoBlockId) {
    if (func.supportsAsyncEagerReturn() == TriBool::No) {
      reduce(env, fcallWithFCA(fca.withoutAsyncEagerTarget()));
      return true;
    }
  }

  if (!fca.skipRepack() && fcallCanSkipRepack(env, fca, func)) {
    reduce(env, fcallWithFCA(fca.withoutRepack()));
    return true;
  }

  if (!fca.skipCoeffectsCheck() &&
      fcallCanSkipCoeffectsCheck(env, fca, func, numExtraInputs)) {
    reduce(env, fcallWithFCA(fca.withoutCoeffectsCheck()));
    return true;
  }

  return false;
}

//////////////////////////////////////////////////////////////////////

namespace {

//////////////////////////////////////////////////////////////////////

struct Folded {};
struct NotFolded {};
using FoldResult = std::variant<TypedValue, Folded, NotFolded>;

//////////////////////////////////////////////////////////////////////

}

//////////////////////////////////////////////////////////////////////

FoldResult fcallTryFold(
  ISS& env,
  const FCallArgs& fca,
  const res::Func& func,
  Type context,
  bool maybeDynamic,
  uint32_t numExtraInputs
) {
  auto const foldableFunc = func.exactFunc();
  if (!foldableFunc) return NotFolded{};
  if (!shouldAttemptToFold(env, foldableFunc, fca, context, maybeDynamic)) {
    return NotFolded{};
  }

  assertx(!fca.hasUnpack() && !fca.hasGenerics() && fca.numRets() == 1);

  auto const finish = [&] (Type ty, bool effectFree) -> FoldResult {
    auto const v = tv(ty);
    if (!effectFree || !v) return NotFolded{};
    // We have an effect-free call which results in a
    // constant. However if we can't actually const prop in this
    // context (for example, if we're speculating), we won't fold, but
    // will report the constant. We can push the return type that the
    // folding would have resulted in, maintaining consistency between
    // speculation and non-speculation.
    if (!can_constprop(env)) return *v;
    BytecodeVec repl;
    for (uint32_t i = 0; i < numExtraInputs; ++i) repl.push_back(bc::PopC {});
    for (uint32_t i = 0; i < fca.numArgs(); ++i) repl.push_back(bc::PopC {});
    repl.push_back(bc::PopU {});
    if (topT(env, fca.numArgs() + 1 + numExtraInputs).subtypeOf(TInitCell)) {
      repl.push_back(bc::PopC {});
    } else {
      assertx(topT(env, fca.numArgs() + 1 + numExtraInputs).subtypeOf(TUninit));
      repl.push_back(bc::PopU {});
    }
    repl.push_back(gen_constant(*v));
    reduce(env, std::move(repl));
    return Folded{};
  };

  if (foldableFunc->attrs & AttrBuiltin &&
      foldableFunc->attrs & AttrIsFoldable) {
    auto ret = const_fold(env, fca.numArgs(), numExtraInputs, *foldableFunc,
                          false);
    if (!ret) return NotFolded{};
    return finish(std::move(*ret), true);
  }

  CompactVector<Type> args(fca.numArgs());
  auto const firstArgPos = numExtraInputs + fca.numInputs() - 1;
  for (auto i = uint32_t{0}; i < fca.numArgs(); ++i) {
    auto const& arg = topT(env, firstArgPos - i);
    auto const isScalar = is_scalar(arg);
    if (!isScalar &&
        (env.index.func_depends_on_arg(foldableFunc, i) ||
         !arg.subtypeOf(BInitCell))) {
      return NotFolded{};
    }
    args[i] = isScalar ? scalarize(arg) : arg;
  }

  auto calleeCtx = CallContext {
    foldableFunc,
    std::move(args),
    copy_arg_names(fca.namedArgNames()),
    std::move(context)
  };
  if (env.collect.unfoldableFuncs.contains(calleeCtx)) return NotFolded{};

  auto [foldableReturnType, effectFree, _] =
    env.index.lookup_foldable_return_type(
    env.ctx,
    calleeCtx
  );
  auto folded = finish(std::move(foldableReturnType), effectFree);
  if (std::holds_alternative<NotFolded>(folded)) {
    env.collect.unfoldableFuncs.emplace(std::move(calleeCtx));
  }
  return folded;
}

Type typeFromWH(Type t) {
  if (!t.couldBe(BObj)) {
    // Exceptions will be thrown if a non-object is awaited.
    return TBottom;
  }

  // Throw away non-obj component.
  t &= TObj;

  // If we aren't even sure this is a wait handle, there's nothing we can
  // infer here.
  if (!is_specialized_wait_handle(t)) {
    return TInitCell;
  }

  return wait_handle_inner(t);
}

void pushCallReturnType(ISS& env,
                        Type ty,
                        bool canSuspend,
                        const FCallArgs& fca,
                        bool nullsafe,
                        const std::vector<Type>& inOuts,
                        FoldResult folded) {
  auto const numRets = fca.numRets();
  if (numRets != 1) {
    assertx(fca.asyncEagerTarget() == NoBlockId);
    assertx(IMPLIES(nullsafe, inOuts.size() == numRets - 1));

    for (auto i = uint32_t{0}; i < numRets - 1; ++i) popU(env);
    if (!ty.couldBe(BVecN)) {
      // Function cannot have an in-out args match, so call will
      // always fail.
      if (!nullsafe) {
        for (int32_t i = 0; i < numRets; i++) push(env, TBottom);
        return unreachable(env);
      }
      // We'll only hit the nullsafe null case, so the outputs are the
      // inout inputs.
      for (auto& t : inOuts) push(env, std::move(t));
      push(env, TInitNull);
      return;
    }

    // If we might use the nullsafe operator, we need to union in the
    // null case (which means the inout args are unchanged).
    if (is_specialized_array_like(ty)) {
      for (int32_t i = 1; i < numRets; i++) {
        auto elem = array_like_elem(ty, ival(i)).first;
        if (nullsafe) elem |= inOuts[i-1];
        push(env, std::move(elem));
      }
      push(
        env,
        nullsafe
          ? opt(array_like_elem(ty, ival(0)).first)
          : array_like_elem(ty, ival(0)).first
      );
    } else {
      for (int32_t i = 0; i < numRets; ++i) push(env, TInitCell);
    }
    return;
  }
  if (fca.asyncEagerTarget() != NoBlockId) {
    env.flags.canSuspend |= canSuspend;
    assertx(!ty.is(BBottom));
    push(env, typeFromWH(ty));
    assertx(!topC(env).subtypeOf(BBottom));
    env.propagate(fca.asyncEagerTarget(), &env.state);
    popC(env);
  }

  if (nullsafe) ty = opt(std::move(ty));
  if (ty.is(BBottom)) {
    // The callee function never returns.  It might throw, or loop
    // forever.
    push(env, TBottom);
    return unreachable(env);
  }

  // We would have folded this call, but we're in some context where
  // we can't const-prop. To maintain monotonicity, push the constant
  // that would have resulted from the folding. This ensures
  // consistency between speculating and non-speculating.
  if (auto const v = std::get_if<TypedValue>(&folded)) {
    effect_free(env);
    push(env, from_cell(*v));
  } else {
    push(env, std::move(ty));
  }
}

const StaticString s_defined { "defined" };
const StaticString s_function_exists { "function_exists" };

Optional<bool> awaitAllCanSuspend(const res::Func& func,
                                  const CompactVector<Type>& args) {
  if (args.empty()) return std::nullopt;
  auto const name = func.name();
  if (name != "HH\\AwaitAllWaitHandle::fromVec" &&
      name != "HH\\AwaitAllWaitHandle::fromDict") {
    return std::nullopt;
  }
  return wait_handle_can_suspend(iter_types(args[0]).value);
}

template<typename FCallWithFCA>
void fcallKnownImpl(
  ISS& env,
  const FCallArgs& fca,
  const res::Func& func,
  Type context,
  bool nullsafe,
  uint32_t numExtraInputs,
  FCallWithFCA fcallWithFCA,
  Optional<uint32_t> inOutNum,
  FoldResult folded
) {
  assertx(!std::holds_alternative<Folded>(folded));

  auto const numArgs = fca.numArgs();
  auto returnInfo = [&] {
    CompactVector<Type> args(numArgs);
    auto const firstArgPos = numExtraInputs + fca.numInputs() - 1;
    for (auto i = uint32_t{0}; i < numArgs; ++i) {
      args[i] = topCV(env, firstArgPos - i);
    }

    auto const* argNames = fca.namedArgNames();

    auto ret = fca.hasUnpack()
      ? env.index.lookup_return_type(env.ctx, &env.collect.methods, func)
      : env.index.lookup_return_type(
          env.ctx, &env.collect.methods, args, argNames, context, func
        );
    if (func.supportsAsyncEagerReturn() == TriBool::Yes &&
        !ret.canSuspend) {
      ret.t = set_wait_handle_can_suspend(
        std::move(ret.t),
        false
      );
    }
    if (!fca.hasUnpack()) {
      if (auto const canSuspend = awaitAllCanSuspend(func, args)) {
        ret.t = wait_handle(TInitNull, *canSuspend);
      }
    }
    return ret;
  }();
  auto returnType = std::move(returnInfo.t);

  // If there's a caller/callee inout mismatch, then the call will
  // always fail.
  if (fca.enforceInOut()) {
    if (inOutNum && (*inOutNum + 1 != fca.numRets())) {
      returnType = TBottom;
    }
  }

  if (fca.asyncEagerTarget() != NoBlockId &&
      typeFromWH(returnType).is(BBottom)) {
    // Kill the async eager target if the function never returns.
    reduce(env, fcallWithFCA(std::move(fca.withoutAsyncEagerTarget())));
    return;
  }

  // For dynamic methods such as $c::$foo(), $o->$foo(), and similar,
  // numExtraInputs describes the number of stack values representing the callee
  // that need to be discarded e.g. when folding by emitting PopC or just for
  // type analysis.
  // See also: extraInput bools, extraStk in the interpreter
  for (auto i = uint32_t{0}; i < numExtraInputs; ++i) popC(env);
  if (fca.hasGenerics()) popC(env);
  if (fca.hasUnpack()) popC(env);
  std::vector<Type> inOuts;
  for (auto i = uint32_t{0}; i < numArgs; ++i) {
    if (nullsafe && fca.enforceInOut() && fca.isInOut(numArgs - i - 1)) {
      inOuts.emplace_back(popCV(env));
    } else {
      popCV(env);
    }
  }
  popU(env);
  popCU(env);
  pushCallReturnType(
    env,
    std::move(returnType),
    returnInfo.canSuspend,
    fca,
    nullsafe,
    inOuts,
    std::move(folded)
  );
}

void fcallUnknownImpl(ISS& env,
                      const FCallArgs& fca,
                      const Type& retTy = TInitCell) {
  if (fca.hasGenerics()) popC(env);
  if (fca.hasUnpack()) popC(env);
  auto const numArgs = fca.numArgs();
  auto const numRets = fca.numRets();
  for (auto i = uint32_t{0}; i < numArgs; ++i) popCV(env);
  popU(env);
  popCU(env);
  if (fca.asyncEagerTarget() != NoBlockId) {
    env.flags.canSuspend = true;
    assertx(numRets == 1);
    assertx(!retTy.is(BBottom));
    push(env, retTy);
    env.propagate(fca.asyncEagerTarget(), &env.state);
    popC(env);
  }
  for (auto i = uint32_t{0}; i < numRets - 1; ++i) popU(env);
  for (auto i = uint32_t{0}; i < numRets; ++i) push(env, retTy);
}

void in(ISS& env, const bc::FCallFuncD& op) {
  auto const rfunc = env.index.resolve_func(op.str2);

  if (auto const wrapped = rfunc.triviallyWrappedFunc()) {
    return reduce(env, bc::FCallFuncD { op.fca, *wrapped });
  }

  if (op.fca.hasGenerics()) {
    auto const tsList = topC(env);
    if (!tsList.couldBe(BVec)) {
      return unreachable(env);
    }

    if (!rfunc.couldHaveReifiedGenerics()) {
      return reduce(
        env,
        bc::PopC {},
        bc::FCallFuncD { op.fca.withoutGenerics(), op.str2 }
      );
    }
  }

  auto const updateBC = [&] (FCallArgs fca) {
    return bc::FCallFuncD { std::move(fca), op.str2 };
  };

  auto const numInOut = op.fca.enforceInOut()
    ? rfunc.lookupNumInoutParams()
    : std::nullopt;

  if (fcallOptimizeChecks(env, op.fca, rfunc, updateBC, numInOut, false, 0)) {
    return;
  }
  auto folded = fcallTryFold(env, op.fca, rfunc, TBottom, false, 0);
  if (std::holds_alternative<Folded>(folded)) return;

  if (auto const func = rfunc.exactFunc()) {
    if (optimize_builtin(env, func, op.fca)) return;
  }

  fcallKnownImpl(
    env,
    op.fca,
    rfunc,
    TBottom,
    false,
    0,
    updateBC,
    numInOut,
    std::move(folded)
  );
}

namespace {

const StaticString s_invoke("__invoke");
const StaticString
  s_DynamicContextOverrideUnsafe("__SystemLib\\DynamicContextOverrideUnsafe");

bool isBadContext(const FCallArgs& fca) {
  return fca.context() &&
    fca.context()->tsame(s_DynamicContextOverrideUnsafe.get());
}

Context getCallContext(const ISS& env, const FCallArgs& fca) {
  if (auto const name = fca.context()) {
    auto const rcls = env.index.resolve_class(name);
    if (rcls && rcls->cls()) {
      return Context { env.ctx.unit, env.ctx.func, rcls->cls() };
    }
    return Context { env.ctx.unit, env.ctx.func, nullptr };
  }
  return env.ctx;
}

void fcallObjMethodNullsafeNoFold(ISS& env,
                                  const FCallArgs& fca,
                                  bool extraInput) {
  assertx(fca.asyncEagerTarget() == NoBlockId);
  if (extraInput) popC(env);
  if (fca.hasGenerics()) popC(env);
  if (fca.hasUnpack()) popC(env);
  auto const numArgs = fca.numArgs();
  auto const numRets = fca.numRets();
  std::vector<Type> inOuts;
  for (auto i = uint32_t{0}; i < numArgs; ++i) {
    if (fca.enforceInOut() && fca.isInOut(numArgs - i - 1)) {
      inOuts.emplace_back(popCV(env));
    } else {
      popCV(env);
    }
  }
  popU(env);
  popCU(env);
  for (auto i = uint32_t{0}; i < numRets - 1; ++i) popU(env);
  assertx(inOuts.size() == numRets - 1);
  for (auto& t : inOuts) push(env, std::move(t));
  push(env, TInitNull);
}

void fcallObjMethodNullsafe(ISS& env, const FCallArgs& fca, bool extraInput) {
  // Don't fold if there's inout arguments. We could, in principal,
  // fold away the inout case like we do below, but we don't have the
  // bytecodes necessary to shuffle the stack.
  if (fca.enforceInOut()) {
    for (uint32_t i = 0; i < fca.numArgs(); ++i) {
      if (fca.isInOut(i)) {
        return fcallObjMethodNullsafeNoFold(env, fca, extraInput);
      }
    }
  }

  BytecodeVec repl;
  if (extraInput) repl.push_back(bc::PopC {});
  if (fca.hasGenerics()) repl.push_back(bc::PopC {});
  if (fca.hasUnpack()) repl.push_back(bc::PopC {});

  auto const numArgs = fca.numArgs();
  for (uint32_t i = 0; i < numArgs; ++i) {
    assertx(topC(env, repl.size()).subtypeOf(BInitCell));
    repl.push_back(bc::PopC {});
  }
  repl.push_back(bc::PopU {});
  repl.push_back(bc::PopC {});
  assertx(fca.numRets() == 1);
  repl.push_back(bc::Null {});

  reduce(env, std::move(repl));
}

template <typename UpdateBC>
void fcallObjMethodImpl(ISS& env, const FCallArgs& fca, SString methName,
                        bool nullThrows, bool dynamic, bool extraInput,
                        uint32_t inputPos, SString clsHint,
                        UpdateBC updateBC) {
  auto const input = topC(env, inputPos);
  auto const location = topStkEquiv(env, inputPos);
  auto const mayCallMethod = input.couldBe(BObj);
  auto const mayUseNullsafe = !nullThrows && input.couldBe(BNull);
  auto const mayThrowNonObj = !input.subtypeOf(nullThrows ? BObj : BOptObj);

  auto const refineLoc = [&] {
    if (location == NoLocalId) return;
    if (!refineLocation(env, location, [&] (Type t) {
      if (nullThrows) return intersection_of(t, TObj);
      if (!t.couldBe(BUninit)) return intersection_of(t, TOptObj);
      if (!t.couldBe(BObj)) return intersection_of(t, TNull);
      return t;
    })) {
      unreachable(env);
    }
  };

  auto const throws = [&] {
    if (fca.asyncEagerTarget() != NoBlockId) {
      // Kill the async eager target if the function never returns.
      return reduce(env, updateBC(fca.withoutAsyncEagerTarget()));
    }
    if (extraInput) popC(env);
    fcallUnknownImpl(env, fca, TBottom);
    unreachable(env);
  };

  if (!mayCallMethod && !mayUseNullsafe) {
    // This FCallObjMethodD may only throw
    return throws();
  }

  if (!mayCallMethod && !mayThrowNonObj) {
    // Null input, this may only return null, so do that.
    return fcallObjMethodNullsafe(env, fca, extraInput);
  }

  if (!mayCallMethod) {
    // May only return null, but can't fold as we may still throw.
    assertx(mayUseNullsafe && mayThrowNonObj);
    if (fca.asyncEagerTarget() != NoBlockId) {
      return reduce(env, updateBC(fca.withoutAsyncEagerTarget()));
    }
    return fcallObjMethodNullsafeNoFold(env, fca, extraInput);
  }

  if (isBadContext(fca)) return throws();

  auto const ctx = getCallContext(env, fca);
  auto const ctxTy = input.couldBe(BObj)
    ? intersection_of(input, TObj)
    : TObj;
  auto const rfunc = env.index.resolve_method(ctx, ctxTy, methName);

  auto const numInOut = fca.enforceInOut()
    ? rfunc.lookupNumInoutParams()
    : std::nullopt;

  auto const canFold = !mayUseNullsafe && !mayThrowNonObj;
  auto const numExtraInputs = extraInput ? 1 : 0;
  if (fcallOptimizeChecks(env, fca, rfunc, updateBC,
                          numInOut, mayUseNullsafe, numExtraInputs)) {
    return;
  }

  FoldResult folded = NotFolded{};
  if (canFold) {
    folded = fcallTryFold(env, fca, rfunc, ctxTy, dynamic, numExtraInputs);
    if (std::holds_alternative<Folded>(folded)) return;
  }

  if (clsHint && clsHint->empty() && rfunc.exactFunc()) {
    return reduce(env, updateBC(fca, rfunc.exactFunc()->cls->name));
  }

  fcallKnownImpl(env, fca, rfunc, ctxTy, mayUseNullsafe, extraInput ? 1 : 0,
                 updateBC, numInOut, std::move(folded));
  refineLoc();
}

void fcallFuncUnknown(ISS& env, const bc::FCallFunc& op) {
  popC(env);
  fcallUnknownImpl(env, op.fca);
}

void fcallFuncClsMeth(ISS& env, const bc::FCallFunc& op) {
  assertx(topC(env).subtypeOf(BClsMeth));

  // TODO: optimize me
  fcallFuncUnknown(env, op);
}

void fcallFuncFunc(ISS& env, const bc::FCallFunc& op) {
  assertx(topC(env).subtypeOf(BFunc));

  // TODO: optimize me
  fcallFuncUnknown(env, op);
}

void fcallFuncObj(ISS& env, const bc::FCallFunc& op) {
  assertx(topC(env).subtypeOf(BOptObj));

  auto const updateBC = [&] (FCallArgs fca, SString clsHint = nullptr) {
    assertx(!clsHint);
    return bc::FCallFunc { std::move(fca) };
  };
  fcallObjMethodImpl(
    env, op.fca, s_invoke.get(),
    true, false, true, 0, nullptr,
    updateBC
  );
}

void fcallFuncStr(ISS& env, const bc::FCallFunc& op) {
  assertx(topC(env).subtypeOf(BStr));
  auto funcName = getNameFromType(topC(env));
  if (!funcName) return fcallFuncUnknown(env, op);

  funcName = normalizeNS(funcName);
  if (!isNSNormalized(funcName) || !notClassMethodPair(funcName)) {
    return fcallFuncUnknown(env, op);
  }

  auto const rfunc = env.index.resolve_func(funcName);
  if (!rfunc.mightCareAboutDynCalls()) {
    return reduce(env, bc::PopC {}, bc::FCallFuncD { op.fca, funcName });
  }

  auto const updateBC = [&] (FCallArgs fca) {
    return bc::FCallFunc { std::move(fca) };
  };

  auto const numInOut = op.fca.enforceInOut()
    ? rfunc.lookupNumInoutParams()
    : std::nullopt;

  if (fcallOptimizeChecks(env, op.fca, rfunc, updateBC, numInOut, false, 1)) {
    return;
  }
  fcallKnownImpl(
    env,
    op.fca,
    rfunc,
    TBottom,
    false,
    1,
    updateBC,
    numInOut,
    NotFolded{}
  );
}

} // namespace

void in(ISS& env, const bc::FCallFunc& op) {
  auto const callable = topC(env);
  if (!callable.couldBe(BObj | BArrLike | BStr | BFunc |
                        BRFunc | BClsMeth | BRClsMeth)) {
    if (op.fca.asyncEagerTarget() != NoBlockId) {
      return reduce(env, bc::FCallFunc { op.fca.withoutAsyncEagerTarget() });
    }
    popC(env);
    fcallUnknownImpl(env, op.fca, TBottom);
    return unreachable(env);
  }
  if (callable.subtypeOf(BOptObj)) return fcallFuncObj(env, op);
  if (callable.subtypeOf(BFunc)) return fcallFuncFunc(env, op);
  if (callable.subtypeOf(BClsMeth)) return fcallFuncClsMeth(env, op);
  if (callable.subtypeOf(BStr)) return fcallFuncStr(env, op);
  fcallFuncUnknown(env, op);
}

void in(ISS& env, const bc::ResolveFunc& op) {
  push(env, TFunc);
}

void in(ISS& env, const bc::ResolveMethCaller& op) {
  // TODO (T29639296)
  push(env, TFunc);
}

void in(ISS& env, const bc::ResolveRFunc& op) {
  popC(env);
  push(env, union_of(TFunc, TRFunc));
}

namespace {

Type ctxCls(ISS& env) {
  auto const s = selfCls(env);
  return setctx(s ? *s : TCls);
}

Type specialClsRefToCls(ISS& env, SpecialClsRef ref) {
  if (!env.ctx.cls) return TCls;
  auto const op = [&]()-> Optional<Type> {
    switch (ref) {
      case SpecialClsRef::LateBoundCls: return ctxCls(env);
      case SpecialClsRef::SelfCls:      return selfClsExact(env);
      case SpecialClsRef::ParentCls:    return parentClsExact(env);
    }
    always_assert(false);
  }();
  return op ? *op : TCls;
}

template<bool reifiedVersion = false>
void resolveClsMethodSImpl(ISS& env, SpecialClsRef ref, LSString meth_name) {
  auto const clsTy = specialClsRefToCls(env, ref);
  auto const rfunc = env.index.resolve_method(env.ctx, clsTy, meth_name);
  if (is_specialized_cls(clsTy) && dcls_of(clsTy).isExact() &&
      !rfunc.couldHaveReifiedGenerics()) {
    auto const clsName = dcls_of(clsTy).cls().name();
    if (reifiedVersion) {
      return reduce(
        env,
        bc::PopC {},
        bc::ResolveClsMethodD { clsName, meth_name }
      );
    } else {
      return reduce(env, bc::ResolveClsMethodD { clsName, meth_name });
    }
  }
  if (reifiedVersion) popC(env);
  if (!reifiedVersion || !rfunc.couldHaveReifiedGenerics()) {
    push(env, TClsMeth);
  } else {
    push(env, union_of(TClsMeth, TRClsMeth));
  }
}

} // namespace

void in(ISS& env, const bc::ResolveClsMethod& op) {
  popC(env);
  push(env, TClsMeth);
}

void in(ISS& env, const bc::ResolveClsMethodD& op) {
  push(env, TClsMeth);
}

void in(ISS& env, const bc::ResolveClsMethodS& op) {
  resolveClsMethodSImpl<false>(env, op.subop1, op.str2);
}

void in(ISS& env, const bc::ResolveRClsMethod&) {
  popC(env);
  popC(env);
  push(env, union_of(TClsMeth, TRClsMeth));
}

void in(ISS& env, const bc::ResolveRClsMethodD&) {
  popC(env);
  push(env, union_of(TClsMeth, TRClsMeth));
}

void in(ISS& env, const bc::ResolveRClsMethodS& op) {
  resolveClsMethodSImpl<true>(env, op.subop1, op.str2);
}

void in(ISS& env, const bc::ResolveClass& op) {
  auto cls = env.index.resolve_class(op.str1);
  if (!cls) {
    push(env, TBottom);
    unreachable(env);
    return;
  }
  if (module_check_always_passes(env, *cls)) {
    effect_free(env);
  }
  push(env, clsExact(*cls, true));
}

void in(ISS& env, const bc::LazyClass& op) {
  effect_free(env);
  push(env, lazyclsval(op.str1));
}

void in(ISS& env, const bc::EnumClassLabel& op) {
  effect_free(env);
  push(env, enumclasslabelval(op.str1));
}

void in(ISS& env, const bc::FCallObjMethodD& op) {
  if (op.fca.hasGenerics()) {
    auto const tsList = topC(env);
    if (!tsList.couldBe(BVec)) {
      return unreachable(env);
    }

    auto const input = topC(env, op.fca.numInputs() + 1);
    auto const ctxTy = input.couldBe(BObj)
      ? intersection_of(input, TObj)
      : TObj;
    auto const rfunc = env.index.resolve_method(env.ctx, ctxTy, op.str4);
    if (!rfunc.couldHaveReifiedGenerics()) {
      return reduce(
        env,
        bc::PopC {},
        bc::FCallObjMethodD {
          op.fca.withoutGenerics(), op.str2, op.subop3, op.str4 }
      );
    }
  }

  auto const updateBC = [&] (FCallArgs fca, SString clsHint = nullptr) {
    if (!clsHint) clsHint = op.str2;
    return bc::FCallObjMethodD { std::move(fca), clsHint, op.subop3, op.str4 };
  };
  fcallObjMethodImpl(
    env, op.fca, op.str4,
    op.subop3 == ObjMethodOp::NullThrows,
    false, false, op.fca.numInputs() + 1,
    op.str2, updateBC
  );
}

void in(ISS& env, const bc::FCallObjMethod& op) {
  auto const methName = getNameFromType(topC(env));
  if (!methName) {
    popC(env);
    fcallUnknownImpl(env, op.fca);
    return;
  }

  auto const input = topC(env, op.fca.numInputs() + 2);
  auto const ctxTy = input.couldBe(BObj)
    ? intersection_of(input, TObj)
    : TObj;
  auto const rfunc = env.index.resolve_method(env.ctx, ctxTy, methName);
  if (!rfunc.mightCareAboutDynCalls()) {
    return reduce(
      env,
      bc::PopC {},
      bc::FCallObjMethodD { op.fca, op.str2, op.subop3, methName }
    );
  }

  auto const updateBC = [&] (FCallArgs fca, SString clsHint = nullptr) {
    if (!clsHint) clsHint = op.str2;
    return bc::FCallObjMethod { std::move(fca), clsHint, op.subop3 };
  };
  fcallObjMethodImpl(
    env, op.fca, methName,
    op.subop3 == ObjMethodOp::NullThrows,
    true, true, op.fca.numInputs() + 2,
    op.str2, updateBC
  );
}

namespace {

template <typename Op, class UpdateBC>
void fcallClsMethodImpl(ISS& env, const Op& op, Type clsTy, SString methName,
                        bool dynamic, uint32_t numExtraInputs, SString clsHint,
                        UpdateBC updateBC) {
  if (isBadContext(op.fca)) {
    if (op.fca.asyncEagerTarget() != NoBlockId) {
      return reduce(env, updateBC(op.fca.withoutAsyncEagerTarget()));
    }
    for (auto i = uint32_t{0}; i < numExtraInputs; ++i) popC(env);
    fcallUnknownImpl(env, op.fca, TBottom);
    unreachable(env);
    return;
  }

  auto const ctx = getCallContext(env, op.fca);
  auto const rfunc = env.index.resolve_method(ctx, clsTy, methName);

  auto const numInOut = op.fca.enforceInOut()
    ? rfunc.lookupNumInoutParams()
    : std::nullopt;

  if (fcallOptimizeChecks(env, op.fca, rfunc, updateBC, numInOut, false,
                          numExtraInputs)) {
    return;
  }
  auto folded = fcallTryFold(env, op.fca, rfunc, clsTy, dynamic, numExtraInputs);
  if (std::holds_alternative<Folded>(folded)) return;

  if (clsHint && rfunc.exactFunc() && clsHint->empty()) {
    return reduce(env, updateBC(op.fca, rfunc.exactFunc()->cls->name));
  }

  fcallKnownImpl(env, op.fca, rfunc, clsTy, false /* nullsafe */,
                 numExtraInputs, updateBC, numInOut, std::move(folded));
}

} // namespace

void in(ISS& env, const bc::FCallClsMethodD& op) {
  auto const updateBC = [&] (FCallArgs fca, SString clsHint = nullptr) {
    return bc::FCallClsMethodD { std::move(fca), op.str2, op.str3 };
  };

  auto const rcls = env.index.resolve_class(op.str2);
  if (!rcls) {
    if (op.fca.asyncEagerTarget() != NoBlockId) {
      return reduce(env, updateBC(op.fca.withoutAsyncEagerTarget()));
    }
    fcallUnknownImpl(env, op.fca, TBottom);
    unreachable(env);
    return;
  }

  auto const clsTy = clsExact(*rcls, true);
  auto const rfunc = env.index.resolve_method(env.ctx, clsTy, op.str3);

  if (op.fca.hasGenerics() && !rfunc.couldHaveReifiedGenerics()) {
    return reduce(
      env,
      bc::PopC {},
      bc::FCallClsMethodD {
        op.fca.withoutGenerics(), op.str2, op.str3 }
    );
  }

  if (auto const func = rfunc.exactFunc()) {
    assertx(func->cls != nullptr);
    if (func->cls->name->same(op.str2) &&
        optimize_builtin(env, func, op.fca)) {
      // When we use FCallBuiltin to call a static method, the litstr method
      // name will be a fully qualified cls::fn (e.g. "HH\Map::fromItems").
      //
      // As a result, we can only do this optimization if the name of the
      // builtin function's class matches this op's class name immediate.
      return;
    }
  }

  fcallClsMethodImpl(env, op, clsTy, op.str3, false, 0, nullptr, updateBC);
}

void in(ISS& env, const bc::FCallClsMethodM& op) {
  auto const throws = [&] {
    if (op.fca.asyncEagerTarget() != NoBlockId) {
      // Kill the async eager target if the function never returns.
      return reduce(
        env,
        bc::FCallClsMethodM {
          op.fca.withoutAsyncEagerTarget(),
          op.str2,
          op.str3
        }
      );
    }
    popC(env);
    fcallUnknownImpl(env, op.fca, TBottom);
    unreachable(env);
  };

  auto const t = topC(env);
  if (!t.couldBe(BObj | BCls | BStr | BLazyCls)) return throws();

  auto const clsTy = [&] {
    if (t.subtypeOf(BCls)) return t;
    if (t.subtypeOf(BObj)) return objcls(t);
    if (auto const clsname = getNameFromType(t)) {
      if (auto const rcls = env.index.resolve_class(clsname)) {
        return clsExact(*rcls, true);
      } else {
        return TBottom;
      }
    }
    return TCls;
  }();
  if (clsTy.is(BBottom)) return throws();

  auto const methName = op.str3;
  auto const rfunc = env.index.resolve_method(env.ctx, clsTy, methName);
  auto const maybeDynamicCall = t.couldBe(TStr);
  if (is_specialized_cls(clsTy) && dcls_of(clsTy).isExact() &&
      module_check_always_passes(env, dcls_of(clsTy)) &&
      !maybeDynamicCall &&
      (!rfunc.mightCareAboutDynCalls() ||
        !maybeDynamicCall
      )
  ) {
    auto const clsName = dcls_of(clsTy).cls().name();
    return reduce(
      env,
      bc::PopC {},
      bc::FCallClsMethodD { op.fca, clsName, methName }
    );
  }

  auto const updateBC = [&] (FCallArgs fca, SString clsHint = nullptr) {
    if (!clsHint) clsHint = op.str2;
    return bc::FCallClsMethodM { std::move(fca), clsHint, methName};
  };
  fcallClsMethodImpl(env, op, clsTy, methName, maybeDynamicCall, 1, op.str2, updateBC);
}

namespace {

template <typename Op, class UpdateBC>
void fcallClsMethodSImpl(ISS& env, const Op& op, SString methName, bool dynamic,
                         bool extraInput, UpdateBC updateBC) {
  auto const clsTy = specialClsRefToCls(env, op.subop3);
  if (is_specialized_cls(clsTy) && dcls_of(clsTy).isExact() &&
      !dynamic && op.subop3 == SpecialClsRef::LateBoundCls) {
    auto const clsName = dcls_of(clsTy).cls().name();
    reduce(env, bc::FCallClsMethodD { op.fca, clsName, methName });
    return;
  }

  auto const rfunc = env.index.resolve_method(env.ctx, clsTy, methName);

  auto const numInOut = op.fca.enforceInOut()
    ? rfunc.lookupNumInoutParams()
    : std::nullopt;

  auto const numExtraInputs = extraInput ? 1 : 0;
  if (fcallOptimizeChecks(env, op.fca, rfunc, updateBC, numInOut, false,
                          numExtraInputs)) {
    return;
  }

  auto folded = fcallTryFold(env, op.fca, rfunc, ctxCls(env), dynamic,
                             numExtraInputs);
  if (std::holds_alternative<Folded>(folded)) return;

  auto moduleCheck = [&] {
    auto const func = rfunc.exactFunc();
    assertx(func);
    return module_check_always_passes(env, *(func->cls));
  };

  if (rfunc.exactFunc() && op.str2->empty() && moduleCheck()) {
    return reduce(env, updateBC(op.fca, rfunc.exactFunc()->cls->name));
  }

  fcallKnownImpl(env, op.fca, rfunc, ctxCls(env), false /* nullsafe */,
                 extraInput ? 1 : 0, updateBC, numInOut, std::move(folded));
}

} // namespace

void in(ISS& env, const bc::FCallClsMethodSD& op) {
  if (op.fca.hasGenerics()) {
    auto const clsTy = specialClsRefToCls(env, op.subop3);
    auto const rfunc = env.index.resolve_method(env.ctx, clsTy, op.str4);
    if (!rfunc.couldHaveReifiedGenerics()) {
      return reduce(
        env,
        bc::PopC {},
        bc::FCallClsMethodSD {
          op.fca.withoutGenerics(), op.str2, op.subop3, op.str4  }
      );
    }
  }

  auto const updateBC = [&] (FCallArgs fca, SString clsHint = nullptr) {
    if (!clsHint) clsHint = op.str2;
    return bc::FCallClsMethodSD { std::move(fca), clsHint, op.subop3, op.str4 };
  };
  fcallClsMethodSImpl(env, op, op.str4, false, false, updateBC);
}

void in(ISS& env, const bc::NewObjD& op)  {
  auto const rcls = env.index.resolve_class(op.str1);
  if (!rcls) {
    push(env, TBottom);
    unreachable(env);
    return;
  }

  auto obj = objExact(*rcls);
  if (obj.subtypeOf(BBottom)) {
    unreachable(env);
    return push(env, TBottom);
  }

  auto const isCtx = [&] {
    if (!env.ctx.cls) return false;
    if (rcls->couldBeOverriddenByRegular()) return false;
    auto const r = env.index.resolve_class(*env.ctx.cls);
    if (!r) return false;
    return equal(obj, objExact(*r));
  }();
  push(env, setctx(std::move(obj), isCtx));
}

void in(ISS& env, const bc::NewObjS& op) {
  auto const cls = specialClsRefToCls(env, op.subop1);
  if (!is_specialized_cls(cls)) {
    push(env, TObj);
    return;
  }

  auto const& dcls = dcls_of(cls);
  if (dcls.isExact() && !dcls.cls().couldHaveReifiedGenerics() &&
      module_check_always_passes(env, dcls) &&
      (!dcls.cls().couldBeOverridden() ||
       equal(cls, unctx(cls)))) {
    return reduce(env, bc::NewObjD { dcls.cls().name() });
  }

  auto obj = toobj(cls);
  if (obj.subtypeOf(BBottom)) unreachable(env);
  push(env, std::move(obj));
}

void in(ISS& env, const bc::NewObj& op) {
  auto const cls = topC(env);
  if (!cls.subtypeOf(BCls) || !is_specialized_cls(cls)) {
    popC(env);
    push(env, TObj);
    return;
  }

  auto const& dcls = dcls_of(cls);
  if (dcls.isExact() && !dcls.cls().mightCareAboutDynConstructs() &&
      module_check_always_passes(env, dcls)) {
    return reduce(
      env,
      bc::PopC {},
      bc::NewObjD { dcls.cls().name() }
    );
  }

  popC(env);
  auto obj = toobj(cls);
  if (obj.subtypeOf(BBottom)) unreachable(env);
  push(env, std::move(obj));
}

namespace {

bool objMightHaveConstProps(const Type& t) {
  assertx(t.subtypeOf(BObj));
  if (!is_specialized_obj(t)) return true;
  auto const& dobj = dobj_of(t);
  if (dobj.isExact()) return dobj.cls().couldHaveConstProp();
  if (dobj.isSub()) return dobj.cls().subCouldHaveConstProp();
  if (dobj.isIsect()) {
    for (auto const cls : dobj.isect()) {
      if (!cls.subCouldHaveConstProp()) return false;
    }
    return true;
  }
  assertx(dobj.isIsectAndExact());
  auto const [e, i] = dobj.isectAndExact();
  if (!e.subCouldHaveConstProp()) return false;
  for (auto const cls : *i) {
    if (!cls.subCouldHaveConstProp()) return false;
  }
  return true;
}

}

void in(ISS& env, const bc::FCallCtor& op) {
  auto const obj = topC(env, op.fca.numInputs() + 1);
  assertx(op.fca.numRets() == 1);

  if (!obj.subtypeOf(BObj)) return fcallUnknownImpl(env, op.fca);

  if (op.fca.lockWhileUnwinding() && !objMightHaveConstProps(obj)) {
    return reduce(
      env, bc::FCallCtor { op.fca.withoutLockWhileUnwinding(), op.str2 }
    );
  }

  auto const rfunc = env.index.resolve_ctor(obj);

  auto const updateFCA = [&] (FCallArgs&& fca) {
    return bc::FCallCtor { std::move(fca), op.str2 };
  };

  auto const numInOut = op.fca.enforceInOut()
    ? rfunc.lookupNumInoutParams()
    : std::nullopt;

  if (fcallOptimizeChecks(env, op.fca, rfunc, updateFCA, numInOut, false, 0)) {
    return;
  }

  auto const canFold = obj.subtypeOf(BObj);
  FoldResult folded = NotFolded{};
  if (canFold) {
    folded = fcallTryFold(env, op.fca, rfunc, obj, false /* dynamic */, 0);
    if (std::holds_alternative<Folded>(folded)) return;
  }

  if (rfunc.exactFunc() && op.str2->empty()) {
    // We've found the exact func that will be called, set the hint.
    return reduce(env, bc::FCallCtor { op.fca, rfunc.exactFunc()->cls->name });
  }

  fcallKnownImpl(env, op.fca, rfunc, obj, false /* nullsafe */, 0,
                 updateFCA, numInOut, std::move(folded));
}

void in(ISS& env, const bc::LockObj& op) {
  auto const t = topC(env);
  auto bail = [&]() {
    discard(env, 1);
    return push(env, t);
  };
  if (!t.subtypeOf(BObj)) return bail();
  if (!is_specialized_obj(t) || objMightHaveConstProps(t)) {
    nothrow(env);
    return bail();
  }
  reduce(env);
}

void in(ISS& env, const bc::IterBase&) {
  auto const t = topC(env);
  if (t.subtypeOf(BArrLike)) return reduce(env);

  auto const iterator = subObj(builtin_class(env.index, s_Iterator.get()));
  if (t.subtypeOf(iterator)) return reduce(env);

  auto tOut = TBottom;
  auto hasEffects = !t.subtypeOf(BArrLike | BObj);
  if (t.couldBe(BArrLike)) tOut |= intersection_of(t, TArrLike);
  if (t.couldBe(BObj)) {
    auto const tObj = intersection_of(t, TObj);
    auto const map = subObj(builtin_class(env.index, s_ConstMap.get()));
    auto const set = subObj(builtin_class(env.index, s_ConstSet.get()));
    auto const vector = subObj(builtin_class(env.index, s_ConstVector.get()));
    if (tObj.subtypeOf(iterator)) {
      tOut |= tObj;
    } else if (tObj.subtypeOf(map)) {
      tOut |= TDict;
    } else if (tObj.subtypeOf(set)) {
      tOut |= TDict;  // Sets are still backed by dicts rather than keysets
    } else if (tObj.subtypeOf(vector)) {
      tOut |= TVec;  // Note: this includes Pair
    } else {
      tOut |= union_of(TArrLike, TObj);
      hasEffects = true;
    }
  }

  if (!hasEffects) effect_free(env);
  popC(env);
  push(env, tOut);
}

void in(ISS& env, const bc::IterGetKey& op) {
  assertx(!iterIsDead(env, op.ita.iterId));
  auto const& li = std::get<LiveIter>(env.state.iters[op.ita.iterId]);
  if (!li.types.mayThrowOnGetOrNext) effect_free(env);
  mayReadLocal(env, op.loc2);
  push(env, li.types.key);
  env.state.topStkIterKeyEquiv = op.ita.iterId;
}

void in(ISS& env, const bc::IterGetValue& op) {
  assertx(!iterIsDead(env, op.ita.iterId));
  auto const& li = std::get<LiveIter>(env.state.iters[op.ita.iterId]);
  if (!li.types.mayThrowOnGetOrNext) effect_free(env);
  mayReadLocal(env, op.loc2);
  push(env, li.types.value);
}

void in(ISS& env, const bc::IterSetValue& op) {
  assertx(!iterIsDead(env, op.ita.iterId));
  auto const& li = std::get<LiveIter>(env.state.iters[op.ita.iterId]);
  auto const eligible = li.setValueBase == op.loc2;
  auto const base = locAsCell(env, op.loc2);
  auto const key = intersection_of(li.types.key, TArrKey);
  auto value = popC(env);
  auto const set = array_like_set(base, key, value);
  // Safe to ignore set.second, as IterSetValue is guaranteed to never throw.
  nothrow(env);
  // This will kill base local iter equivalence and stop BaseConst optimization,
  // but we don't care, as all IterSetValue loops are already optimized.
  setLoc(env, op.loc2, set.first, NoLocalId);
  auto& updated = std::get<LiveIter>(env.state.iters[op.ita.iterId]);
  if (eligible) {
    updated.setValueBase = op.loc2;
    updated.setValueType = std::move(value);
  } else {
    updated.setValueBase = NoLocalId;
    updated.setValueType = TBottom;
  }
}

void in(ISS& env, const bc::IterInit& op) {
  auto const ita = op.ita;
  auto const baseLoc = op.loc2;
  auto const sourceLoc = [&] {
    auto const loc = findIterBaseLoc(env, baseLoc);
    if (loc == baseLoc && has_flag(ita.flags, IterArgsFlags::BaseConst)) {
      // Can't improve this iterator further.
      return NoLocalId;
    }
    return loc;
  }();
  auto const base = locAsCell(env, baseLoc);
  auto ity = iter_types(base);
  auto const setValueBase =
    base.subtypeOf(BVec) || base.subtypeOf(BDict) ? baseLoc : NoLocalId;

  auto const fallthrough = [&] {
    setIter(env, ita.iterId, LiveIter { ity, sourceLoc, NoLocalId, env.bid,
                                        false, setValueBase });
  };

  assertx(iterIsDead(env, ita.iterId));

  if (!ity.mayThrowOnInit) {
    if (ity.count == IterTypes::Count::Empty && will_reduce(env)) {
      reduce(env);
      return jmp_setdest(env, op.target3);
    }
    nothrow(env);
  }

  switch (ity.count) {
    case IterTypes::Count::Empty:
      jmp_setdest(env, op.target3);
      return;
    case IterTypes::Count::Single:
    case IterTypes::Count::NonEmpty:
      fallthrough();
      return jmp_nevertaken(env);
    case IterTypes::Count::ZeroOrOne:
    case IterTypes::Count::Any:
      // Take the branch before setting locals if the iter is already
      // empty, but after popping.  Similar for the other IterInits
      // below.
      env.propagate(op.target3, &env.state);
      fallthrough();
      return;
  }
  always_assert(false);
}

void in(ISS& env, const bc::IterNext& op) {
  auto const ita = op.ita;

  auto noThrow = false;
  auto setValueBase = NoLocalId;
  auto setValueType = TBottom;
  auto setValueKey = TBottom;
  auto const noTaken = match<bool>(
    env.state.iters[ita.iterId],
    [&] (DeadIter)           {
      always_assert(false && "IterNext on dead iter");
      return false;
    },
    [&] (const LiveIter& ti) {
      if (!ti.types.mayThrowOnGetOrNext) noThrow = true;
      if (ti.baseLocal != NoLocalId) hasInvariantIterBase(env);
      setValueBase = ti.setValueBase;
      setValueType = ti.setValueType;
      setValueKey = ti.types.key;
      switch (ti.types.count) {
        case IterTypes::Count::Single:
        case IterTypes::Count::ZeroOrOne:
          return true;
        case IterTypes::Count::NonEmpty:
        case IterTypes::Count::Any:
          return false;
        case IterTypes::Count::Empty:
          always_assert(false);
      }
      not_reached();
    }
  );

  if (noTaken && noThrow && setValueType.is(BBottom) && will_reduce(env)) {
    auto const iterId = safe_cast<IterId>(ita.iterId);
    reduce(env, bc::IterFree { iterId });
  }

  mayReadLocal(env, op.loc2);

  if (noThrow) nothrow(env);

  auto const refineSetValues = [&] {
    if (setValueType.is(BBottom) || setValueBase != op.loc2) return;
    auto const base = peekLocRaw(env, setValueBase);
    refineLocHelper(
      env,
      setValueBase,
      array_like_set_values(
        base,
        intersection_of(setValueKey, TArrKey),
        setValueType
      )
    );
  };

  if (noTaken) {
    jmp_nevertaken(env);
    refineSetValues();
    freeIter(env, ita.iterId);
    return;
  }

  auto taken = env.state;
  auto& takenIter = std::get<LiveIter>(taken.iters[ita.iterId]);
  if (takenIter.setValueBase != op.loc2 ||
      takenIter.setValueType.is(BBottom)) {
    takenIter.setValueBase = NoLocalId;
  }
  takenIter.setValueType = TBottom;
  env.propagate(op.target3, &taken);

  refineSetValues();
  freeIter(env, ita.iterId);
}

void in(ISS& env, const bc::IterFree& op) {
  nothrow(env);
  freeIter(env, op.iter1);
}

/*
 * Any include/require (or eval) op kills all locals, and private properties.
 */
void inclOpImpl(ISS& env) {
  popC(env);
  killLocals(env);
  killThisProps(env);
  killPrivateStatics(env);
  push(env, TInitCell);
}

void in(ISS& env, const bc::Incl&)      { inclOpImpl(env); }
void in(ISS& env, const bc::InclOnce&)  { inclOpImpl(env); }
void in(ISS& env, const bc::Req&)       { inclOpImpl(env); }
void in(ISS& env, const bc::ReqOnce&)   { inclOpImpl(env); }
void in(ISS& env, const bc::ReqDoc&)    { inclOpImpl(env); }
void in(ISS& env, const bc::Eval&)      { inclOpImpl(env); }

void in(ISS& env, const bc::This&) {
  if (thisAvailable(env)) {
    return reduce(env, bc::BareThis { BareThisOp::NeverNull });
  }
  auto const ty = thisTypeNonNull(env);
  push(env, ty, StackThisId);
  setThisAvailable(env);
  if (ty.subtypeOf(BBottom)) unreachable(env);
}

void in(ISS& env, const bc::LateBoundCls& op) {
  if (env.ctx.cls) effect_free(env);
  auto const ty = selfCls(env);
  push(env, setctx(ty ? *ty : TCls));
}

void in(ISS& env, const bc::CheckThis&) {
  if (thisAvailable(env)) {
    return reduce(env);
  }
  setThisAvailable(env);
}

void in(ISS& env, const bc::BareThis& op) {
  if (thisAvailable(env)) {
    if (op.subop1 != BareThisOp::NeverNull) {
      return reduce(env, bc::BareThis { BareThisOp::NeverNull });
    }
  }

  auto const ty = thisType(env);
  if (ty.subtypeOf(BBottom)) {
    unreachable(env);
    return push(env, TBottom);
  }

  switch (op.subop1) {
    case BareThisOp::Notice:
      break;
    case BareThisOp::NoNotice:
      effect_free(env);
      break;
    case BareThisOp::NeverNull:
      setThisAvailable(env);
      if (!env.state.unreachable) effect_free(env);
      return push(env, ty, StackThisId);
  }

  push(env, ty, StackThisId);
}

/*
 * Amongst other things, we use this to mark units non-persistent.
 */
void in(ISS& env, const bc::OODeclExists& op) {
  auto flag = popC(env);
  auto name = popC(env);
  if (!flag.couldBe(BBool) || !name.couldBe(BStr)) {
    unreachable(env);
    push(env, TBottom);
    return;
  }

  if (flag.subtypeOf(BBool) && name.subtypeOf(BStr)) {
    constprop(env);
  }

  auto const v = tv(name);
  if (!v) return push(env, TBool);

  assertx(isStringType(v->m_type));
  auto const rcls = env.index.resolve_class(v->m_data.pstr);
  if (!rcls) return push(env, TFalse);
  auto const cls = rcls->cls();

  // We know the Class* exists, but not its type.
  if (!cls) return push(env, TBool);

  auto const exist = [&] () -> bool {
    switch (op.subop1) {
      case OODeclExistsOp::Class:
        return !(cls->attrs & (AttrInterface | AttrTrait));
      case OODeclExistsOp::Interface:
        return cls->attrs & AttrInterface;
      case OODeclExistsOp::Trait:
        return cls->attrs & AttrTrait;
    }
    not_reached();
  }();

  push(env, exist ? TTrue : TFalse);
}

namespace {

bool couldBeMocked(const Type& t) {
  auto const dcls = [&] () -> const DCls* {
    if (is_specialized_cls(t)) {
      return &dcls_of(t);
    } else if (is_specialized_obj(t)) {
      return &dobj_of(t);
    }
    return nullptr;
  }();
  // In practice this should not occur since this is used mostly on
  // the result of looked up type constraints.
  if (!dcls) return true;
  if (dcls->isExact() || dcls->isSub()) return dcls->cls().couldBeMocked();
  if (dcls->isIsect()) {
    for (auto const cls : dcls->isect()) {
      if (!cls.couldBeMocked()) return false;
    }
    return true;
  }
  assertx(dcls->isIsectAndExact());
  auto const [e, i] = dcls->isectAndExact();
  if (!e.couldBeMocked()) return false;
  for (auto const cls : *i) {
    if (!cls.couldBeMocked()) return false;
  }
  return true;
}

bool couldHaveReifiedType(
    const ISS& env,
    const TypeIntersectionConstraint& tcs
) {
  if (env.ctx.func->isClosureBody) {
    for (auto i = env.ctx.func->params.size();
         i < env.ctx.func->locals.size();
         ++i) {
      auto const name = env.ctx.func->locals[i].name;
      if (!name) return false; // named locals do not appear after unnamed local
      if (isMangledReifiedGenericInClosure(name)) return true;
    }
    return false;
  }
  auto couldHaveReifiedGenerics = [&](const TypeConstraint& tc) {
    if (!tc.isSubObject()) return false;
    auto const cls = env.index.resolve_class(tc.clsName());
    assertx(cls.has_value());
    return cls->couldHaveReifiedGenerics();
  };
  return std::any_of(
    tcs.range().begin(),
    tcs.range().end(),
    couldHaveReifiedGenerics
  );
}

}

void in(ISS& env, const bc::VerifyParamType& op) {
  auto [newTy, remove, effectFree] =
    verify_param_type(env.index, env.ctx, op.loc1, topC(env));

  if (remove) return reduce(env);
  if (newTy.subtypeOf(BBottom)) unreachable(env);

  if (effectFree) {
    effect_free(env);
    constprop(env);
  }

  popC(env);
  push(env, std::move(newTy));
}

void in(ISS& env, const bc::VerifyParamTypeTS& op) {
  auto const a = topC(env);
  if (!a.couldBe(BDict)) {
    unreachable(env);
    popC(env);
    return;
  }
  auto const& constraints = env.ctx.func->params[op.loc1].typeConstraints;
  // TODO(T31677864): We are being extremely pessimistic here, relax it
  if (!env.ctx.func->isReified &&
      (!env.ctx.cls || !env.ctx.cls->hasReifiedGenerics) &&
      !couldHaveReifiedType(env, constraints)) {
    return reduce(env, bc::PopC {});
  }

  if (auto const inputTS = tv(a)) {
    if (!isValidTSType(*inputTS, false)) {
      unreachable(env);
      popC(env);
      return;
    }
    auto const resolvedTS =
      resolve_type_structure(env, inputTS->m_data.parr).sarray();
    if (resolvedTS && resolvedTS != inputTS->m_data.parr) {
      reduce(env, bc::PopC {});
      reduce(env, bc::Dict { resolvedTS });
      reduce(env, bc::VerifyParamTypeTS { op.loc1 });
      return;
    }
    if (shouldRemoveVerifyTypeTS(env, inputTS->m_data.parr)) {
      return reduce(env, bc::PopC {});
    }
  }
  if (auto const last = last_op(env)) {
    if (last->op == Op::CombineAndResolveTypeStruct) {
      if (auto const last2 = last_op(env, 1)) {
        if (last2->op == Op::Dict &&
            shouldRemoveVerifyTypeTS(env, last2->Dict.arr1)) {
          return reduce(env, bc::PopC {});
        }
      }
    }
  }
  mayReadLocal(env, op.loc1, false);
  popC(env);
}

void in(ISS& env, const bc::VerifyOutType& op) {
  auto const& pinfo = env.ctx.func->params[op.loc1];
  auto res = verifyRetAllImpl(env, pinfo.typeConstraints, false, op, topC(env));
  if (!res) return;
  if (res->effectFree) effect_free(env);
  popC(env);
  push(env, std::move(res->type));
}

void in(ISS& env, const bc::VerifyRetTypeTS& /*op*/) {
  auto const a = topC(env);
  if (!a.couldBe(BDict)) {
    unreachable(env);
    popC(env);
    return;
  }
  auto const& retTypeConstraints = env.ctx.func->retTypeConstraints;

  // TODO(T31677864): We are being extremely pessimistic here, relax it
  if (!env.ctx.func->isReified &&
      (!env.ctx.cls || !env.ctx.cls->hasReifiedGenerics) &&
      !couldHaveReifiedType(env, retTypeConstraints)) {
    return reduce(env, bc::PopC {});
  }
  if (auto const inputTS = tv(a)) {
    if (!isValidTSType(*inputTS, false)) {
      unreachable(env);
      popC(env);
      return;
    }
    auto const resolvedTS =
      resolve_type_structure(env, inputTS->m_data.parr).sarray();
    if (resolvedTS && resolvedTS != inputTS->m_data.parr) {
      reduce(env, bc::PopC {});
      reduce(env, bc::Dict { resolvedTS });
      reduce(env, bc::VerifyRetTypeTS {});
      return;
    }
    if (shouldRemoveVerifyTypeTS(env, inputTS->m_data.parr)) {
      return reduce(env, bc::PopC {});
    }
  }
  if (auto const last = last_op(env)) {
    if (last->op == Op::CombineAndResolveTypeStruct) {
      if (auto const last2 = last_op(env, 1)) {
        if (last2->op == Op::Dict &&
            shouldRemoveVerifyTypeTS(env, last2->Dict.arr1)) {
          return reduce(env, bc::PopC {});
        }
      }
    }
  }
  popC(env);
}

void in(ISS& env, const bc::VerifyTypeTS& /*op*/) {
  // Don't even generate code if flag isn't set
  if (!Cfg::Eval::CheckedUnsafeCast) {
    reduce(env, bc::PopC {});
    return;
  }

  auto const a = topC(env);
  if (!a.couldBe(BDict)) {
    unreachable(env);
    popC(env);
    return;
  }

  if (auto const inputTS = tv(a)) {
    if (!isValidTSType(*inputTS, false)) {
      unreachable(env);
      popC(env);
      return;
    }
    auto const resolvedTS =
      resolve_type_structure(env, inputTS->m_data.parr).sarray();
    if (resolvedTS && resolvedTS != inputTS->m_data.parr) {
      reduce(env, bc::PopC {});
      reduce(env, bc::Dict { resolvedTS });
      reduce(env, bc::VerifyTypeTS {});
      return;
    }
  }
  auto stackT = topC(env, 1);
  auto const stackEquiv = topStkEquiv(env, 1);
  popC(env);
  popC(env);
  push(env, std::move(stackT), stackEquiv);
}

void in(ISS& env, const bc::SelfCls&) {
  auto const self = selfClsExact(env);
  if (self) {
    effect_free(env);
    push(env, *self);
  } else {
    push(env, TCls);
  }
}

void in(ISS& env, const bc::ParentCls&) {
  auto const parent = parentClsExact(env);
  if (parent) {
    effect_free(env);
    push(env, *parent);
  } else {
    push(env, TCls);
  }
}

void in(ISS& env, const bc::CreateCl& op) {
  auto const nargs   = op.arg1;

  auto const rcls = env.index.resolve_class(op.str2);
  if (!rcls) {
    discard(env, nargs);
    unreachable(env);
    return push(env, TBottom);
  }

  auto const cls = rcls->cls();
  always_assert_flog(
    cls,
    "A closure class ({}) failed to resolve",
    op.str2
  );
  assertx(cls->unit == env.ctx.unit);
  assertx(is_closure(*cls));

  /*
   * Every closure should have a unique allocation site, but we may see it
   * multiple times in a given round of analyzing this function.  Each time we
   * may have more information about the used variables; the types should only
   * possibly grow.  If it's already there we need to merge the used vars in
   * with what we saw last time.
   */
  if (nargs) {
    CompactVector<Type> usedVars(nargs);
    for (auto i = uint32_t{0}; i < nargs; ++i) {
      usedVars[nargs - i - 1] = unctx(popCU(env));
    }
    env.collect.closureUseVars.merge(*cls, std::move(usedVars));
  }

  effect_free(env);

  if (env.ctx.cls && is_used_trait(*env.ctx.cls)) {
    // Be pessimistic if we're within a trait. The closure will get
    // rescoped potentially multiple times at runtime.
    push(
      env,
      subObj(builtin_class(env.index, s_Closure.get()))
    );
  } else {
    push(env, objExact(*rcls));
  }
}

void in(ISS& env, const bc::CreateCont& /*op*/) {
  // First resume is always next() which pushes null.
  push(env, TInitNull);
}

namespace {
void contEnterImpl(ISS& env) {
  popC(env);
  auto const cls = env.ctx.func->cls;
  assertx(
    cls && (
      cls->name->tsame(s_AsyncGenerator.get()) ||
      cls->name->tsame(s_Generator.get())
    )
  );
  push(
    env,
    cls->name == s_AsyncGenerator.get()
      ? wait_handle(TInitCell)
      : TInitNull
  );
}
}

void in(ISS& env, const bc::ContEnter&) {
  contEnterImpl(env);
}

void in(ISS& env, const bc::ContRaise&) {
  contEnterImpl(env);
}

void in(ISS& env, const bc::Yield&) {
  popC(env);
  push(env, TInitCell);
}

void in(ISS& env, const bc::YieldK&) {
  popC(env);
  popC(env);
  push(env, TInitCell);
}

void in(ISS& /*env*/, const bc::ContCheck&) {}
void in(ISS& env, const bc::ContValid&)   { push(env, TBool); }
void in(ISS& env, const bc::ContKey&)     { push(env, TInitCell); }
void in(ISS& env, const bc::ContCurrent&) { push(env, TInitCell); }

void pushTypeFromWH(ISS& env, Type t) {
  auto inner = typeFromWH(t);
  // The next opcode is unreachable if awaiting a non-object or WaitH<Bottom>.
  if (inner.subtypeOf(BBottom)) unreachable(env);
  push(env, std::move(inner));
}

void in(ISS& env, const bc::WHResult&) {
  pushTypeFromWH(env, popC(env));
}

void in(ISS& env, const bc::Await&) {
  auto const& wh = topC(env);
  if (wh.subtypeOf(BObj) &&
      is_specialized_wait_handle(wh) &&
      !wait_handle_can_suspend(wh)) {
    return reduce(env, bc::WHResult {});
  }
  auto value = popC(env);
  env.flags.canSuspend |= wait_handle_can_suspend(value);
  pushTypeFromWH(env, std::move(value));
}

void in(ISS& env, const bc::AwaitAll& op) {
  auto const equiv = equivLocalRange(env, op.locrange);
  if (equiv != op.locrange.first) {
    return reduce(
      env,
      bc::AwaitAll {LocalRange {equiv, op.locrange.count}}
    );
  }

  auto const end = op.locrange.first + op.locrange.count;

  CompactVector<bool> eager;
  eager.reserve(op.locrange.count);
  auto anyEager = false;
  for (auto loc = op.locrange.first; loc < end; ++loc) {
    auto const ty = to_cell(peekLocRaw(env, loc));
    auto const isWaitHandle =
      ty.subtypeOf(BOptObj) && is_specialized_wait_handle(ty);
    auto const isEager =
      ty.subtypeOf(BInitNull) ||
      (isWaitHandle && !wait_handle_can_suspend(ty));
    eager.emplace_back(isEager);
    anyEager |= isEager;
  }

  if (anyEager) {
    auto const nonEagerCount = std::count(eager.begin(), eager.end(), false);
    if (!nonEagerCount) return reduce(env, bc::Null {});

    // AwaitAll uses a contiguous local range, so remove eager members at the
    // ends without changing local identity across a suspension point. Eager
    // members within the remaining range are already excluded from the
    // ConcurrentWaitHandle by the runtime.
    uint32_t first = 0;
    while (eager[first]) ++first;
    auto last = safe_cast<uint32_t>(eager.size());
    while (eager[last - 1]) --last;
    if (first || last != eager.size()) {
      return reduce(env, bc::AwaitAll { LocalRange {
        op.locrange.first + first,
        last - first
      }});
    }
  }

  for (uint32_t i = 0; i < op.locrange.count; ++i) {
    auto const loc = op.locrange.first + i;
    mayReadLocal(env, loc);
    env.flags.canSuspend |= wait_handle_can_suspend(locAsCell(env, loc));
  }

  push(env, TInitNull);
}

void in(ISS& env, const bc::AwaitLowPri& /*op*/) {
  env.flags.canSuspend = true;
  push(env, TInitNull);
}

void in(ISS& env, const bc::SetImplicitContextByValue&) {
  nothrow(env);
  popC(env);
  push(env, TObj);
}

const StaticString
  s_Memoize("__Memoize"),
  s_MemoizeLSB("__MemoizeLSB");

void in(ISS& env, const bc::GetMemoAgnosticImplicitContext&) {
  effect_free(env);
  return push(env, TObj);
}

void in(ISS& env, const bc::GetWholeImplicitContext&) {
  effect_free(env);
  return push(env, TObj);
}

void in(ISS& env, const bc::Idx&) {
  auto const def = popC(env);
  auto const [key, promotion] = promote_classlike_to_key(popC(env));
  auto const base = popC(env);

  assertx(!def.is(BBottom));

  auto effectFree = promotion != Promotion::YesMightThrow;
  auto result = TBottom;

  auto const finish = [&] {
    if (result.is(BBottom)) {
      assertx(!effectFree);
      unreachable(env);
    }
    if (effectFree) {
      constprop(env);
      effect_free(env);
    }
    push(env, std::move(result));
  };

  if (key.couldBe(BNull)) result |= def;
  if (key.subtypeOf(BNull)) return finish();

  if (!base.subtypeOf(BArrLike | BObj | BStr)) result |= def;

  if (base.couldBe(BArrLike)) {
    if (!key.subtypeOf(BOptArrKey)) effectFree = false;
    if (key.couldBe(BArrKey)) {
      auto elem = array_like_elem(
        base,
        key.subtypeOf(BArrKey) ? key : intersection_of(key, TArrKey)
      );
      result |= std::move(elem.first);
      if (!elem.second) result |= def;
    }
  }
  if (base.couldBe(BObj)) {
    result |= TInitCell;
    effectFree = false;
  }
  if (base.couldBe(BStr)) {
    result |= TSStr;
    result |= def;
    if (!key.subtypeOf(BOptArrKey)) effectFree = false;
  }

  finish();
}

void in(ISS& env, const bc::ArrayIdx&) {
  auto def = popC(env);
  auto const [key, promotion] = promote_classlike_to_key(popC(env));
  auto const base = popC(env);

  assertx(!def.is(BBottom));

  auto effectFree = promotion != Promotion::YesMightThrow;
  auto result = TBottom;

  auto const finish = [&] {
    if (result.is(BBottom)) {
      assertx(!effectFree);
      unreachable(env);
    }
    if (effectFree) {
      constprop(env);
      effect_free(env);
    }
    push(env, std::move(result));
  };

  if (key.couldBe(BNull)) result |= def;
  if (key.subtypeOf(BNull)) return finish();

  if (!base.subtypeOf(BArrLike)) effectFree = false;
  if (!base.couldBe(BArrLike)) return finish();

  if (!key.subtypeOf(BOptArrKey)) effectFree = false;
  if (!key.couldBe(BArrKey)) return finish();

  auto elem = array_like_elem(
    base,
    key.subtypeOf(BArrKey) ? key : intersection_of(key, TArrKey)
  );
  result |= std::move(elem.first);
  if (!elem.second) result |= std::move(def);
  finish();
}

namespace {
void implArrayMarkLegacy(ISS& env, bool legacy) {
  auto const recursive = popC(env);
  auto const value = popC(env);

  if (auto const tv_recursive = tv(recursive)) {
    if (auto const tv_value = tv(value)) {
      if (tvIsBool(*tv_recursive)) {
        auto const result = eval_cell([&]{
          return val(*tv_recursive).num
            ? arrprov::markTvRecursively(*tv_value, legacy)
            : arrprov::markTvShallow(*tv_value, legacy);
        });
        if (result) {
          push(env, *result);
          effect_free(env);
          constprop(env);
          return;
        }
      }
    }
  }

  // Changing the legacy mark can change array staticness and, recursively,
  // nested array types. Preserve everything which cannot be affected, but
  // conservatively drop specialization for vecs and dicts.
  auto result = value;
  if (value.couldBe(BVec)) result |= TVec;
  if (value.couldBe(BDict)) result |= TDict;
  push(env, std::move(result));
}
}

void in(ISS& env, const bc::ArrayMarkLegacy&) {
  implArrayMarkLegacy(env, true);
}

void in(ISS& env, const bc::ArrayUnmarkLegacy&) {
  implArrayMarkLegacy(env, false);
}

void in(ISS& env, const bc::CheckProp&) {
  if (env.ctx.cls->attrs & AttrNoOverride) {
    return reduce(env, bc::False {});
  }
  effect_free(env);
  push(env, TBool);
}

void in(ISS& env, const bc::InitProp& op) {
  auto const t = topC(env);
  switch (op.subop2) {
    case InitPropOp::Static: {
      auto const rcls = env.index.resolve_class(env.ctx.cls->name);
      // If class isn't instantiable, this bytecode isn't reachable
      // anyways.
      if (!rcls) break;
      mergeStaticProp(
        env,
        clsExact(*rcls, true),
        sval(op.str1),
        t,
        false,
        true
      );
      break;
    }
    case InitPropOp::NonStatic:
      mergeThisProp(env, op.str1, t);
      break;
  }

  for (auto const& prop : env.ctx.func->cls->properties) {
    if (prop.name != op.str1) continue;

    ITRACE(1, "InitProp: {} = {}\n", op.str1, show(t));

    auto const refine =
      [&] (const TypeConstraint& tc) -> std::pair<Type, bool> {
      assertx(tc.validForProp());
      if (Cfg::Eval::CheckPropTypeHints == 0) return { t, true };
      auto const lookup = lookup_constraint(env.index, env.ctx, tc, t);
      if (t.moreRefined(lookup.lower)) return { t, true };
      if (Cfg::Eval::ClassStringHintNoticesSampleRate > 0) return { t, false };
      if (!t.couldBe(lookup.upper)) return { t, false };
      if (lookup.coerceClassToString != TriBool::Yes) return { t, false };
      auto promoted = promote_classish(t);
      if (!promoted.moreRefined(lookup.lower)) return { t, false };
      return { std::move(promoted), true };
    };

    auto const [refined, effectFree] = [&] () -> std::pair<Type, bool> {
      auto refined1 = TCell;
      auto effectFree1 = true;
      for (auto const& tc : prop.typeConstraints.range()) {
        if (!effectFree1) break;
        auto [refined2, effectFree2] = refine(tc);
        refined1 &= refined2;
        if (refined1.is(BBottom)) effectFree1 = false;
        effectFree1 &= effectFree2;
      }
      return {std::move(refined1), effectFree1};
    }();

    auto const val = [effectFree = effectFree] (const Type& t) {
      if (!effectFree) return make_tv<KindOfUninit>();
      if (auto const v = tv(t)) return *v;
      return make_tv<KindOfUninit>();
    }(refined);

    auto const deepInit =
      (prop.attrs & AttrDeepInit) &&
      (type(val) == KindOfUninit) &&
      could_contain_objects(refined);
    propInitialValue(
      env,
      prop,
      val,
      effectFree,
      deepInit
    );
    if (type(val) == KindOfUninit) break;
    return reduce(env, bc::PopC {});
  }

  popC(env);
}

namespace {

template <typename Op, typename Rebind>
void memoGetImpl(ISS& env,
                 const Op& op,
                 BlockId noValueBlk,
                 Optional<BlockId> whBlk,
                 Rebind&& rebind) {
  always_assert(env.ctx.func->isMemoizeWrapper);
  always_assert(op.locrange.first + op.locrange.count
                <= env.ctx.func->locals.size());

  if (will_reduce(env)) {
    // If we can use an equivalent, earlier range, then use that instead.
    auto const equiv = equivLocalRange(env, op.locrange);
    if (equiv != op.locrange.first) {
      reduce(env, rebind(LocalRange { equiv, op.locrange.count }));
      return;
    }
  }

  auto memoSets = memoGet(env);

  // MemoGet can raise if we give a non arr-key local, or if we're in a method
  // and $this isn't available.
  auto allArrKey = true;
  for (uint32_t i = 0; i < op.locrange.count; ++i) {
    // Peek here, because if we decide to reduce the bytecode, we
    // don't want to mark the locals as being read.
    allArrKey &= peekLocRaw(env, op.locrange.first + i).subtypeOf(BArrKey);
  }
  if (allArrKey &&
      (!env.ctx.func->cls ||
       (env.ctx.func->attrs & AttrStatic) ||
       thisAvailable(env))) {
    if (will_reduce(env)) {
      if (memoSets.retTy.subtypeOf(BBottom) &&
          (!whBlk || memoSets.waitHandleRetTy.is(BBottom))) {
        reduce(env);
        jmp_setdest(env, noValueBlk);
        return;
      }
      // deal with constprop manually; otherwise we will propagate the
      // taken edge and *then* replace the MemoGet with a constant.
      if (memoSets.effectFree) {
        auto const v = [&] () -> Optional<TypedValue> {
          if (!can_constprop(env)) return std::nullopt;
          if (auto const v = tv(memoSets.retTy)) return v;
          if (!whBlk) return std::nullopt;
          if (auto const v = tv(memoSets.waitHandleRetTy)) return v;
          return std::nullopt;
        }();
        if (v) {
          // If we are here, we have already processed the block with MemoSet,
          // which is not effect free, so the whole analysis pass is already
          // marked as non effect free.
          //
          // The propagation of information from MemoSet to MemoGet violates
          // monotonicity (one block suddenly becomes unreachable and another
          // becomes reachable).
          //
          // Force reanalaysis of this function once MemoGet gets replaced
          // with a constant to properly infer the function became effect free.
          reanalyze_on_update(env);
          reduce(env, gen_constant(*v));
          return;
        }
      }
    }
    effect_free(env);
  }

  // We don't remove the op, so mark the locals as being read.
  for (uint32_t i = 0; i < op.locrange.count; ++i) {
    mayReadLocal(env, op.locrange.first + i);
  }

  if (memoSets.retTy.is(BBottom)) {
    if (!whBlk || memoSets.waitHandleRetTy.is(BBottom)) {
      // No sets for either cases, we'll always branch to the no value
      // case.
      jmp_setdest(env, noValueBlk);
    } else {
      // Only sets for the non-eager case. The fallthrough case
      // is unreachable.
      env.propagate(noValueBlk, &env.state);
      push(env, std::move(memoSets.waitHandleRetTy));
      env.propagate(*whBlk, &env.state);
      popC(env);
      unreachable(env);
      push(env, TBottom);
    }
    return;
  }

  env.propagate(noValueBlk, &env.state);
  if (whBlk && !memoSets.waitHandleRetTy.is(BBottom)) {
    push(env, std::move(memoSets.waitHandleRetTy));
    env.propagate(*whBlk, &env.state);
    popC(env);
  }
  push(env, std::move(memoSets.retTy));
}

}

void in(ISS& env, const bc::MemoGet& op) {
  memoGetImpl(
    env, op, op.target1, std::nullopt,
    [&] (const LocalRange& l) { return bc::MemoGet { op.target1, l }; }
  );
}

void in(ISS& env, const bc::MemoGetEager& op) {
  always_assert(env.ctx.func->isAsync && !env.ctx.func->isGenerator);
  memoGetImpl(
    env, op, op.target1, op.target2,
    [&] (const LocalRange& l) {
      return bc::MemoGetEager { op.target1, op.target2, l };
    }
  );
}

namespace {

template <typename Op>
void memoSetImpl(ISS& env, const Op& op, bool eager) {
  always_assert(env.ctx.func->isMemoizeWrapper);
  always_assert(op.locrange.first + op.locrange.count
                <= env.ctx.func->locals.size());

  // If we can use an equivalent, earlier range, then use that instead.
  auto const equiv = equivLocalRange(env, op.locrange);
  if (equiv != op.locrange.first) {
    return reduce(
      env,
      Op { LocalRange { equiv, op.locrange.count } }
    );
  }

  // If the call to the memoize implementation was optimized away to
  // an immediate instruction, record that fact so that we can
  // optimize away the corresponding MemoGet.
  auto effectFree = [&] {
    auto const last = op_from_slot(env, 0);
    return last && pushes_immediate(last->op);
  }();

  // MemoSet can raise if we give a non arr-key local, or if we're in a method
  // and $this isn't available.
  auto allArrKey = true;
  for (uint32_t i = 0; i < op.locrange.count; ++i) {
    allArrKey &= locRaw(env, op.locrange.first + i).subtypeOf(BArrKey);
  }
  if (allArrKey &&
      (!env.ctx.func->cls ||
       (env.ctx.func->attrs & AttrStatic) ||
       thisAvailable(env))) {
    nothrow(env);
  } else {
    effectFree = false;
  }

  auto t = popC(env);
  memoSet(
    env,
    (!env.ctx.func->isAsync || eager) ? t : TBottom,
    (!env.ctx.func->isAsync || eager) ? TBottom : t,
    effectFree
  );
  if (effectFree) effect_free(env);
  push(env, std::move(t));
}

}

void in(ISS& env, const bc::MemoSet& op) {
  memoSetImpl(env, op, false);
}

void in(ISS& env, const bc::MemoSetEager& op) {
  always_assert(env.ctx.func->isAsync && !env.ctx.func->isGenerator);
  memoSetImpl(env, op, true);
}

}

namespace {

//////////////////////////////////////////////////////////////////////

void dispatch(ISS& env, const Bytecode& op) {
#define O(opcode, ...) case Op::opcode: interp_step::in(env, op.opcode); return;
  switch (op.op) { OPCODES }
#undef O
  not_reached();
}

//////////////////////////////////////////////////////////////////////

void interpStep(ISS& env, const Bytecode& bc) {
  ITRACE(2, "  {} ({})\n",
         show(*env.ctx.func, bc),
         env.unchangedBcs + env.replacedBcs.size());
  Trace::Indent _;

  // If there are throw exit edges, make a copy of the state (except
  // stacks) in case we need to propagate across throw exits (if
  // it's a PEI).
  if (!env.stateBefore && env.blk.throwExit != NoBlockId) {
    env.stateBefore.emplace(with_throwable_only(env.index, env.state));
  }

  env.flags = {};

  default_dispatch(env, bc);

  if (env.flags.reduced) return;

  auto const_prop = [&] {
    if (!env.flags.canConstProp) return false;

    auto const numPushed   = bc.numPush();
    TinyVector<TypedValue> cells;

    auto i = size_t{0};
    while (i < numPushed) {
      auto const v = tv(topT(env, i));
      if (!v) return false;
      cells.push_back(*v);
      ++i;
    }

    if (env.flags.wasPEI) {
      ITRACE(2, "   nothrow (due to constprop)\n");
      env.flags.wasPEI = false;
    }
    if (!env.flags.effectFree) {
      ITRACE(2, "   effect_free (due to constprop)\n");
      env.flags.effectFree = true;
    }

    // If we're doing inline interp, don't actually perform the
    // constprop. If we do, we can infer static types that won't
    // actually exist at runtime.
    if (any(env.collect.opts & CollectionOpts::Inlining)) {
      ITRACE(2, "   inlining, skipping actual constprop\n");
      return false;
    }

    // If we're speculating, it's not safe to rewind since it messes
    // up stack tracking.
    if (any(env.collect.opts & CollectionOpts::Speculating)) {
      ITRACE(2, "   speculating, skipping actual constprop\n");
      return false;
    }

    assertx(can_constprop(env));

    rewind(env, bc);

    auto const numPop = bc.numPop();
    for (auto j = 0; j < numPop; j++) {
      auto const flavor = bc.popFlavor(j);
      if (flavor == Flavor::C) {
        interpStep(env, bc::PopC {});
      } else if (flavor == Flavor::U) {
        interpStep(env, bc::PopU {});
      } else {
        assertx(flavor == Flavor::CU);
        auto const& popped = topT(env);
        if (popped.subtypeOf(BUninit)) {
          interpStep(env, bc::PopU {});
        } else {
          assertx(popped.subtypeOf(BInitCell));
          interpStep(env, bc::PopC {});
        }
      }
    }

    while (i--) {
      push(env, from_cell(cells[i]));
      record(env, gen_constant(cells[i]));
    }
    return true;
  };

  if (const_prop()) return;

  assertx(!env.flags.effectFree || !env.flags.wasPEI);
  if (env.flags.wasPEI) {
    ITRACE(2, "   PEI.\n");
    if (env.stateBefore) {
      env.propagate(env.blk.throwExit, &*env.stateBefore);
    }
  }
  env.stateBefore.reset();

  record(env, bc);
}

void interpOne(ISS& env, const Bytecode& bc) {
  env.srcLoc = bc.srcLoc;
  interpStep(env, bc);
}

BlockId speculate(Interp& interp) {
  auto low_water = interp.state.stack.size();

  interp.collect.opts = interp.collect.opts | CollectionOpts::Speculating;
  SCOPE_EXIT {
    interp.collect.opts = interp.collect.opts - CollectionOpts::Speculating;
  };

  auto failed = false;
  ISS env { interp, [&] (BlockId, const State*) { failed = true; } };

  assertx(env.collect.mInstrState.empty());

  FTRACE(4, "  Speculate B{}\n", interp.bid);
  for (auto const& bc : interp.blk->hhbcs) {
    assertx(!interp.state.unreachable);
    auto const numPop = bc.numPop() + (bc.op == Op::Dup ? -1 : 0);
    if (interp.state.stack.size() - numPop < low_water) {
      low_water = interp.state.stack.size() - numPop;
    }

    interpOne(env, bc);
    if (failed) {
      env.collect.mInstrState.clear();
      FTRACE(3, "  Bailing from speculate because propagate was called\n");
      return NoBlockId;
    }

    auto const& flags = env.flags;
    if (!flags.effectFree) {
      env.collect.mInstrState.clear();
      FTRACE(3, "  Bailing from speculate because not effect free\n");
      return NoBlockId;
    }

    assertx(!flags.returned);

    if (flags.jmpDest != NoBlockId && interp.state.stack.size() == low_water) {
      assertx(env.collect.mInstrState.empty());
      FTRACE(2, "  Speculate found target block {}\n", flags.jmpDest);
      return flags.jmpDest;
    }
  }
  assertx(env.collect.mInstrState.empty());

  if (interp.state.stack.size() != low_water) {
    FTRACE(3,
           "  Bailing from speculate because the speculated block "
           "left items on the stack\n");
    return NoBlockId;
  }

  if (interp.blk->fallthrough == NoBlockId) {
    FTRACE(3,
           "  Bailing from speculate because there was no fallthrough");
    return NoBlockId;
  }

  FTRACE(2, "  Speculate found fallthrough block {}\n",
         interp.blk->fallthrough);

  return interp.blk->fallthrough;
}

BlockId speculateHelper(ISS& env, BlockId orig, bool updateTaken) {
  assertx(orig != NoBlockId);

  if (!will_reduce(env)) return orig;

  auto const last = last_op(env);
  bool endsInControlFlow = last && instrIsNonCallControlFlow(last->op);
  auto target = orig;
  auto pops = 0;

  auto const& func = env.ctx.func;

  hphp_fast_set<BlockId> seen;

  State temp{env.state, State::Compact{}};
  do {
    auto const targetBlk = func.blocks()[target].get();
    if (!targetBlk->multiPred) break;
    auto const ok = [&] {
        switch (targetBlk->hhbcs.back().op) {
          case Op::JmpZ:
          case Op::JmpNZ:
          case Op::SSwitch:
          case Op::Switch:
            return true;
          default:
            return false;
        }
      }();

    if (!ok) break;

    Interp interp {
      env.index, env.ctx, env.collect, target, targetBlk, temp
    };

    auto const old_size = temp.stack.size();
    auto const new_target = speculate(interp);
    if (new_target == NoBlockId) break;

    const ssize_t delta = old_size - temp.stack.size();
    assertx(delta >= 0);
    if (delta && endsInControlFlow) break;

    pops += delta;
    target = new_target;
    temp.stack.compact();
  } while (seen.emplace(target).second);

  if (endsInControlFlow && updateTaken) {
    assertx(!pops);
    auto needsUpdate = target != orig;
    if (!needsUpdate) {
      forEachTakenEdge(
        *last,
        [&] (BlockId bid) {
          if (bid == orig) return;
          if (bid == NoBlockId || !is_fatal_block(*func.blocks()[bid])) {
            // Don't set the edge to NoBlockId if it's already pointing
            // to a fatal block.
            needsUpdate = true;
          }
        }
      );
    }
    if (needsUpdate) {
      auto& bc = mutate_last_op(env);
      assertx(bc.op != Op::Enter);
      forEachTakenEdge(
        bc,
        [&] (BlockId& bid) {
          if (bid == orig) {
            bid = target;
            return;
          }
          if (bid == NoBlockId) return;
          // Don't set the edge to NoBlockId if it's already pointing
          // to a fatal block.
          if (!is_fatal_block(*func.blocks()[bid])) bid = NoBlockId;
        }
      );
    }
  }

  while (pops--) {
    auto const& popped = topT(env);
    if (popped.subtypeOf(BInitCell)) {
      interpStep(env, bc::PopC {});
    } else {
      assertx(popped.subtypeOf(BUninit));
      interpStep(env, bc::PopU {});
    }
  }

  return target;
}

}

//////////////////////////////////////////////////////////////////////

RunFlags run(Interp& interp, const State& in,
             const PropagateFn& propagate,
             const RollbackFn& rollback) {
  SCOPE_EXIT {
    FTRACE(2, "\nout {}{}\n",
           state_string(*interp.ctx.func, interp.state, interp.collect),
           property_state_string(interp.collect.props));
  };

  auto env = ISS { interp, propagate };
  auto ret = RunFlags {};
  auto finish = [&] (BlockId fallthrough) {
    // Don't change the fallthrough to NoBlockId if the fallthrough is
    // already to a fatal block.
    if (fallthrough != NoBlockId ||
        interp.blk->fallthrough == NoBlockId ||
        !is_fatal_block(*interp.ctx.func.blocks()[interp.blk->fallthrough])) {
      ret.updateInfo.fallthrough = fallthrough;
    } else {
      ret.updateInfo.fallthrough = interp.blk->fallthrough;
    }
    ret.updateInfo.fallthrough = fallthrough;
    ret.updateInfo.unchangedBcs = env.unchangedBcs;
    ret.updateInfo.replacedBcs = std::move(env.replacedBcs);
    return ret;
  };

  BytecodeVec retryBcs;
  auto retryOffset = interp.blk->hhbcs.size();
  auto size = retryOffset;
  size_t idx = 0;

  assertx(env.collect.mInstrState.empty());
  while (true) {
    if (idx == size) {
      assertx(env.collect.mInstrState.empty());
      finish_tracked_elems(env, 0);
      if (!env.reprocess) {
        FTRACE(2, "  <end block>\n");
        if (env.fallthrough == NoBlockId) return finish(NoBlockId);
        auto const old = env.fallthrough;
        env.fallthrough = speculateHelper(env, env.fallthrough, false);
        if (old == env.fallthrough) {
          propagate(env.fallthrough, &interp.state);
          return finish(env.fallthrough);
        }
        FTRACE(2, "  reprocessing due to successful speculation\n");
      } else {
        assertx(env.unchangedBcs < retryOffset || env.replacedBcs.size());
      }
      FTRACE(2, "  Reprocess mutated block {}\n", interp.bid);
      assertx(!env.undo);
      retryOffset = env.unchangedBcs;
      retryBcs = std::move(env.replacedBcs);
      env.unchangedBcs = 0;
      env.state.copy_from(in);
      env.reprocess = false;
      env.replacedBcs.clear();
      env.stateBefore.reset();
      size = retryOffset + retryBcs.size();
      idx = 0;
      ret.usedParams.reset();
      ret.canSuspend = false;
      rollback();
      continue;
    }

    auto const& bc = idx < retryOffset ?
      interp.blk->hhbcs[idx] : retryBcs[idx - retryOffset];
    ++idx;

    interpOne(env, bc);
    auto const& flags = env.flags;

    ret.usedParams |= flags.usedParams;
    ret.canSuspend |= flags.canSuspend;

    if (flags.wasPEI) ret.noThrow = false;

    if (interp.collect.effectFree && !flags.effectFree) {
      interp.collect.effectFree = false;
      if (any(interp.collect.opts & CollectionOpts::EffectFreeOnly)) {
        env.collect.mInstrState.clear();
        FTRACE(2, "  Bailing because not effect free\n");
        return finish(NoBlockId);
      }
    }

    if (flags.returned) {
      always_assert(idx == size);
      assertx(env.collect.mInstrState.empty());
      if (env.reprocess) continue;

      always_assert(interp.blk->fallthrough == NoBlockId);
      assertx(!ret.returned);
      FTRACE(2, "  returned {}\n", show(*flags.returned));
      ret.retParam = flags.retParam;
      ret.returned = flags.returned;
      return finish(NoBlockId);
    }

    if (flags.jmpDest != NoBlockId) {
      always_assert(idx == size);
      assertx(env.collect.mInstrState.empty());
      auto const hasFallthrough = [&] {
        if (flags.jmpDest != interp.blk->fallthrough) {
          FTRACE(2, "  <took branch; no fallthrough>\n");
          auto const last = last_op(env);
          return !last || !instrIsNonCallControlFlow(last->op);
        } else {
          FTRACE(2, "  <branch never taken>\n");
          return true;
        }
      }();
      if (hasFallthrough) env.fallthrough = flags.jmpDest;
      if (env.reprocess) continue;
      finish_tracked_elems(env, 0);
      auto const newDest = speculateHelper(env, flags.jmpDest, true);
      if (newDest != flags.jmpDest) {
        FTRACE(2, "  reprocessing due to successful speculation\n");
        if (hasFallthrough) env.fallthrough = newDest;
        env.reprocess = true;
        continue;
      }
      propagate(newDest, &interp.state);
      return finish(hasFallthrough ? newDest : NoBlockId);
    }

    if (interp.state.unreachable) {
      env.collect.mInstrState.clear();
      if (env.reprocess) {
        idx = size;
        continue;
      }
      FTRACE(2, "  <bytecode fallthrough is unreachable>\n");
      finish_tracked_elems(env, 0);
      return finish(NoBlockId);
    }
  }
}

StepFlags step(Interp& interp, const Bytecode& op) {
  ISS env { interp, [] (BlockId, const State*) {} };
  env.analyzeDepth++;
  default_dispatch(env, op);
  if (env.state.unreachable) {
    env.collect.mInstrState.clear();
  }
  assertx(env.trackedElems.empty());
  return env.flags;
}

void default_dispatch(ISS& env, const Bytecode& op) {
  if (!env.trackedElems.empty()) {
    auto const pops = [&] () -> uint32_t {
      switch (op.op) {
        case Op::AddElemC:
        case Op::AddNewElemC:
          return numPop(op) - 1;
        case Op::Concat:
        case Op::ConcatN:
          return 0;
        default:
          return numPop(op);
      }
    }();

    finish_tracked_elems(env, env.state.stack.size() - pops);
  }
  dispatch(env, op);
  if (instrFlags(op.op) & TF && env.flags.jmpDest == NoBlockId) {
    unreachable(env);
  }
}

//////////////////////////////////////////////////////////////////////

}
