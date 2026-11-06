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

#include <algorithm>

#include "hphp/runtime/base/type-string.h"
#include "hphp/runtime/base/array-provenance.h"

#include "hphp/hhbbc/analyze.h"
#include "hphp/hhbbc/bc.h"
#include "hphp/hhbbc/class-util.h"
#include "hphp/hhbbc/context.h"
#include "hphp/hhbbc/func-util.h"
#include "hphp/hhbbc/index.h"
#include "hphp/hhbbc/interp-state.h"
#include "hphp/hhbbc/interp.h"
#include "hphp/hhbbc/options.h"
#include "hphp/hhbbc/representation.h"
#include "hphp/hhbbc/type-structure.h"
#include "hphp/hhbbc/type-system.h"

namespace HPHP::HHBBC {

struct LocalRange;

//////////////////////////////////////////////////////////////////////

TRACE_SET_MOD(hhbbc)

//////////////////////////////////////////////////////////////////////

struct TrackedElemInfo {
  TrackedElemInfo(uint32_t d, uint32_t i) : depth{d}, idx{i} {}
  // stack depth of the AddElem we're tracking
  uint32_t depth;
  // bytecode index of the previous AddElem
  uint32_t idx;
};

/*
 * Interpreter Step State.
 *
 * This struct gives interpreter functions access to shared state.  It's not in
 * interp-state.h because it's part of the internal implementation of
 * interpreter routines.  The publicized state as results of interpretation are
 * in that header and interp.h.
 */
struct ISS {
  ISS(Interp& bag, PropagateFn propagate)
    : index(bag.index)
    , ctx(bag.ctx)
    , collect(bag.collect)
    , bid(bag.bid)
    , blk(*bag.blk)
    , state(bag.state)
    , undo(bag.undo)
    , propagate(std::move(propagate))
    , fallthrough(bag.blk->fallthrough)
    , analyzeDepth(0)
  {}

  const IIndex& index;
  const AnalysisContext ctx;
  CollectedInfo& collect;
  const BlockId bid;
  const php::Block& blk;
  State& state;
  StateMutationUndo* undo;
  StepFlags flags;
  PropagateFn propagate;

  Optional<State> stateBefore;

  // Current fallthrough of the block being analyzed. This can be
  // changed multiple times during analysis, so use this instead of
  // blk.fallthrough (which is only changed during index update).
  BlockId fallthrough;

  // If we're inside an impl (as opposed to reduce) this will be > 0
  uint32_t analyzeDepth{0};
  int32_t srcLoc{-1};
  bool reprocess{false};
  // As we process the block, we keep track of the optimized bytecode
  // stream. We expect that in steady state, there will be no changes;
  // so as we process the block, if the initial bytecodes are the
  // same, we just keep track of how many are the same in
  // unchangedBcs. Once things diverge, the replacements are stored in
  // replacedBcs.

  // number of unchanged bcs to take from blk.hhbcs
  uint32_t unchangedBcs{0};
  // new bytecodes
  BytecodeVec replacedBcs;
  CompactVector<TrackedElemInfo> trackedElems;
};

void impl_vec(ISS& env, bool reduce, BytecodeVec&& bcs);
void rewind(ISS& env, const Bytecode&);
void rewind(ISS& env, int);
const Bytecode* last_op(ISS& env, int idx = 0);
const Bytecode* op_from_slot(ISS& env, int, int prev = 0);

//////////////////////////////////////////////////////////////////////

namespace interp_step {

/*
 * An interp_step::in(ISS&, const bc::op&) function exists for every
 * bytecode. Most are defined in interp.cpp, but some (like FCallBuiltin and
 * member instructions) are defined elsewhere.
 */
#define O(opcode, ...) void in(ISS&, const bc::opcode&);
OPCODES
#undef O

}

/*
 * Find a contiguous local range which is equivalent to the given range and has
 * a smaller starting id. Only returns the equivalent first local because the
 * size doesn't change.
 */
LocalId equivLocalRange(ISS& env, const LocalRange& range);

namespace {

Type peekLocRaw(ISS& env, LocalId l);
bool peekLocCouldBeUninit(ISS& env, LocalId l);

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#endif

/*
 * impl(...)
 *
 * Utility for chaining one bytecode implementation to a series of a few
 * others.  Use reduce() if you also want to enable strength reduction
 * (i.e. the bytecode can be replaced by some other bytecode as an
 * optimization).
 *
 * The chained-to bytecodes should not take branches.  For impl, the
 * canConstProp flag will only be set if it was set for all the
 * bytecodes.
 */

template<class... Ts>
void impl(ISS& env, Ts&&... ts) {
  impl_vec(env, false, { std::forward<Ts>(ts)... });
}

/*
 * Reduce means that (given some situation in the execution state),
 * a given bytecode could be replaced by some other bytecode
 * sequence.  Ensure that if you call reduce(), it is before any
 * state-affecting operations (like popC()).
 */
void reduce(ISS& env, BytecodeVec&& bcs) {
  impl_vec(env, true, std::move(bcs));
}

template<class... Bytecodes>
void reduce(ISS& env, Bytecodes&&... hhbc) {
  reduce(env, { std::forward<Bytecodes>(hhbc)... });
}

bool will_reduce(ISS& env) { return env.analyzeDepth == 0; }

bool can_rewind(ISS& env) {
  return !any(env.collect.opts & CollectionOpts::Speculating);
}

// If we're in a context where constant propagation is allowed. For
// the most part, the infra takes care of this automatically. However,
// if you wish to do constprop manually (via reduce), you must check
// this first.
bool can_constprop(ISS& env) {
  return !any(
    env.collect.opts &
    (CollectionOpts::Inlining | CollectionOpts::Speculating)
  );
}

void nothrow(ISS& env) {
  ITRACE(2, "    nothrow\n");
  env.flags.wasPEI = false;
}

void unreachable(ISS& env) {
  ITRACE(2, "    unreachable\n");
  env.state.unreachable = true;
}

void constprop(ISS& env) {
  ITRACE(2, "    constprop\n");
  env.flags.canConstProp = true;
}

void effect_free(ISS& env) {
  ITRACE(2, "    effect_free\n");
  nothrow(env);
  env.flags.effectFree = true;
}

void reanalyze_on_update(ISS& env) {
  ITRACE(2, "    reanalyze_on_update\n");
  env.collect.reanalyzeOnUpdate = true;
}

/*
 * Mark the current block as unconditionally jumping to target. The
 * caller must arrange for env.state to reflect the state that needs
 * to be propagated to the target, but it should not propagate that
 * state.
 */
void jmp_setdest(ISS& env, BlockId target) {
  env.flags.jmpDest = target;
}
void jmp_nevertaken(ISS& env) {
  jmp_setdest(env, env.fallthrough);
}

void readUnknownParams(ISS& env) {
  for (LocalId p = 0; p < env.ctx.func->params.size(); p++) {
    if (p == env.flags.mayReadLocalSet.size()) break;
    env.flags.mayReadLocalSet.set(p);
  }
  env.flags.usedParams.set();
}

void readUnknownLocals(ISS& env) {
  env.flags.mayReadLocalSet.set();
  env.flags.usedParams.set();
}

void readAllLocals(ISS& env) {
  env.flags.mayReadLocalSet.set();
  env.flags.usedParams.set();
}

void doRet(ISS& env, Type t, bool hasEffects) {
  assertx(env.state.stack.empty());
  assertx(!t.is(BBottom) || hasEffects);
  env.flags.mayReadLocalSet.set();
  env.flags.retParam = NoLocalId;
  env.flags.returned = t;
  if (t.is(BBottom)) unreachable(env);
  if (!hasEffects) effect_free(env);
}

void hasInvariantIterBase(ISS& env) {
  env.collect.hasInvariantIterBase = true;
}

//////////////////////////////////////////////////////////////////////
// eval stack

Type popT(ISS& env) {
  assertx(!env.state.stack.empty());
  auto const ret = env.state.stack.back().type;
  ITRACE(2, "    pop:  {}\n", show(ret));
  assertx(ret.subtypeOf(BCell));
  env.state.stack.pop_elem();
  env.state.topStkIterKeyEquiv = NoIterId;
  if (env.undo) env.undo->onPop(ret);
  return ret;
}

Type popC(ISS& env) {
  auto const v = popT(env);
  assertx(v.subtypeOf(BInitCell));
  return v;
}

Type popU(ISS& env) {
  auto const v = popT(env);
  assertx(v.subtypeOf(BUninit));
  return v;
}

Type popCU(ISS& env) {
  auto const v = popT(env);
  assertx(v.subtypeOf(BCell));
  return v;
}

Type popCV(ISS& env) { return popT(env); }

void discard(ISS& env, int n) {
  for (auto i = 0; i < n; ++i) popT(env);
}

const Type& topT(ISS& env, uint32_t idx = 0) {
  assertx(idx < env.state.stack.size());
  return env.state.stack[env.state.stack.size() - idx - 1].type;
}

const Type& topC(ISS& env, uint32_t i = 0) {
  assertx(topT(env, i).subtypeOf(BInitCell));
  return topT(env, i);
}

const Type& topCV(ISS& env, uint32_t i = 0) { return topT(env, i); }

void push(ISS& env, Type t) {
  ITRACE(2, "    push: {}\n", show(t));
  env.state.stack.push_elem(std::move(t), NoLocalId,
                            env.unchangedBcs + env.replacedBcs.size());
  env.state.topStkIterKeyEquiv = NoIterId;
  if (env.undo) env.undo->onPush();
}

void push(ISS& env, Type t, LocalId l) {
  if (l == NoLocalId) return push(env, t);
  if (l <= MaxLocalId && is_volatile_local(env.ctx.func, l)) {
    return push(env, t);
  }
  ITRACE(2, "    push: {} (={})\n", show(t), local_string(*env.ctx.func, l));
  env.state.stack.push_elem(std::move(t), l,
                            env.unchangedBcs + env.replacedBcs.size());
  env.state.topStkIterKeyEquiv = NoIterId;
  if (env.undo) env.undo->onPush();
}


//////////////////////////////////////////////////////////////////////
// $this

void setThisAvailable(ISS& env) {
  ITRACE(2, "    setThisAvailable\n");
  if (!env.ctx.cls || is_unused_trait(*env.ctx.cls) ||
      (env.ctx.func->attrs & AttrStatic)) {
    return unreachable(env);
  }
  if (!env.state.thisType.couldBe(BObj) ||
      !env.state.thisType.subtypeOf(BOptObj)) {
    return unreachable(env);
  }
  if (env.state.thisType.couldBe(BInitNull)) {
    env.state.thisType = unopt(std::move(env.state.thisType));
  }
}

bool thisAvailable(ISS& env) {
  return
    env.state.thisType.subtypeOf(BObj) &&
    !env.state.thisType.is(BBottom);
}

Type thisType(ISS& env) {
  return env.state.thisType;
}

Type thisTypeNonNull(ISS& env) {
  if (!env.state.thisType.couldBe(TObj)) return TBottom;
  if (env.state.thisType.couldBe(BInitNull)) return unopt(env.state.thisType);
  return env.state.thisType;
}

//////////////////////////////////////////////////////////////////////
// self

inline Optional<Type> selfCls(ISS& env) {
  return selfCls(env.index, env.ctx);
}
inline Optional<Type> selfClsExact(ISS& env) {
  return selfClsExact(env.index, env.ctx);
}

inline Optional<Type> parentCls(ISS& env) {
  return parentCls(env.index, env.ctx);
}
inline Optional<Type> parentClsExact(ISS& env) {
  return parentClsExact(env.index, env.ctx);
}

// Like selfClsExact, but if the func is non-static, use an object
// type instead.
inline Type selfExact(ISS& env) {
  assertx(env.ctx.func);
  auto ty = selfClsExact(env);
  if (env.ctx.func->attrs & AttrStatic) {
    return ty ? *ty : TCls;
  }
  return ty ? toobj(*ty) : TObj;
}

//////////////////////////////////////////////////////////////////////
// class constants

inline ClsConstLookupResult lookupClsConstant(const IIndex& index,
                                              const Context& ctx,
                                              const CollectedInfo* collect,
                                              const Type& cls,
                                              const Type& name) {
  // Check if the constant's class is definitely the current context.
  auto const isClsCtx = [&] {
    if (!collect || !collect->clsCns) return false;
    if (!is_specialized_cls(cls)) return false;
    auto const& dcls = dcls_of(cls);
    if (!dcls.isExact()) return false;
    auto const self = selfClsExact(index, ctx);
    if (!self || !is_specialized_cls(*self)) return false;
    return dcls.cls().same(dcls_of(*self).cls());
  }();

  if (isClsCtx && is_specialized_string(name)) {
    auto lookup = collect->clsCns->lookup(sval_of(name));
    if (lookup.found == TriBool::Yes) return lookup;
  }
  return index.lookup_class_constant(ctx, cls, name);
}

inline ClsConstLookupResult lookupClsConstant(ISS& env,
                                              const Type& cls,
                                              const Type& name) {
  return lookupClsConstant(env.index, env.ctx, &env.collect, cls, name);
}

//////////////////////////////////////////////////////////////////////
// folding

const StaticString s___NEVER_INLINE("__NEVER_INLINE");

bool shouldAttemptToFold(ISS& env, const php::Func* func, const FCallArgs& fca,
                         Type context, bool maybeDynamic) {
  if (!func ||
      fca.hasUnpack() ||
      fca.hasGenerics() ||
      fca.numRets() != 1 ||
      !will_reduce(env) ||
      any(env.collect.opts & CollectionOpts::Optimizing)) {
    return false;
  }

  if (maybeDynamic && (
      (Cfg::Eval::NoticeOnBuiltinDynamicCalls &&
       (func->attrs & AttrBuiltin)) ||
      (dyn_call_error_level(func) > 0))) {
    return false;
  }

  // A class from a string is loaded by name, which checks its module and
  // package and may raise a notice whose sample rate HHBBC doesn't know.
  if (maybeDynamic && context.couldBe(BCls)) return false;

  if (func->userAttributes.count(s___NEVER_INLINE.get())) {
    return false;
  }

  // TODO(named_params) we can be smarter about folding things if the named args
  // match the named parameters.
  if (fca.namedArgNames() != nullptr) return false;

  // Reified functions may have a mismatch of arity or reified generics
  // so we cannot fold them
  // TODO(T31677864): Detect the arity mismatch at HHBBC and enable them to
  // be foldable
  if (func->isReified) return false;

  // Coeffect violation may raise warning or throw an exception
  if (!fca.skipCoeffectsCheck()) return false;

  // Readonly violation may raise warning or throw an exception
  if (fca.enforceReadonly() ||
      fca.enforceMutableReturn() ||
      fca.enforceReadonlyThis()) {
    return false;
  }

  auto const funcUnit = env.index.lookup_func_unit(*func);

  // Internal functions may raise module boundary violations
  if ((func->attrs & AttrInternal) &&
      env.index.lookup_func_unit(*env.ctx.func)->moduleName !=
      funcUnit->moduleName) {
    return false;
  }

  // We only fold functions when numRets == 1
  if (func->hasInOutArgs) return false;

  // Can't fold if we get the wrong amount of arguments
  if (!check_nargs_in_range(func, fca.numArgs())) return false;

  // Don't try to fold functions which aren't guaranteed to be accessible at
  // this call site.
  if (func->attrs & AttrPrivate) {
    if (env.ctx.cls != func->cls) return false;
  } else if (func->attrs & AttrProtected) {
    assertx(func->cls);
    if (env.ctx.cls != func->cls) {
      if (!env.ctx.cls) return false;
      auto const rcls1 = env.index.resolve_class(env.ctx.cls->name);
      auto const rcls2 = env.index.resolve_class(func->cls->name);
      if (!rcls1 || !rcls2) return false;
      if (!rcls1->exactSubtypeOf(*rcls2, true, true) &&
          !rcls2->exactSubtypeOf(*rcls1, true, true)) {
        return false;
      }
    }
  }

  // Foldable builtins are always worth trying
  if (func->attrs & AttrIsFoldable) return true;

  // Any native functions at this point are known to be
  // non-foldable, but other builtins might be, even if they
  // don't have the __Foldable attribute.
  if (func->isNative) return false;

  // TODO(named_params) In the future, we can fold when named params
  // match named arguments correctly.
  for (auto& param: func->params) {
    if (param.isNamed) return false;
  }

  if (func->params.size()) return true;

  auto const rfunc = env.index.resolve_func_or_method(*func);

  // The function has no args. Check if it's effect free and returns
  // a literal.
  auto [retTy, effectFree, _] = env.index.lookup_return_type(
    env.ctx,
    &env.collect.methods,
    rfunc,
    Dep::InlineDepthLimit
  );
  auto const isScalar = is_scalar(retTy);
  if (effectFree && isScalar) return true;

  if (!(func->attrs & AttrStatic) && func->cls) {
    // May be worth trying to fold if the method returns a scalar,
    // assuming its only "effect" is checking for existence of $this.
    if (isScalar) return true;

    // The method may be foldable if we know more about $this.
    if (is_specialized_obj(context)) {
      auto const& dobj = dobj_of(context);
      if (dobj.isExact() ||
          (dobj.isSub() && dobj.cls().cls() != func->cls) ||
          (dobj.isIsectAndExact() &&
           dobj.isectAndExact().first.cls() != func->cls)) {
        return true;
      }
    }
  }

  return false;
}

//////////////////////////////////////////////////////////////////////
// locals

void mayReadLocal(ISS& env, uint32_t id, bool isUse = true) {
  if (id < env.flags.mayReadLocalSet.size()) {
    env.flags.mayReadLocalSet.set(id);
  }
  if (isUse &&
      id < env.flags.usedParams.size() &&
      id < env.ctx.func->params.size()) {
    env.flags.usedParams.set(id);
  }
}

// Find a local which is equivalent to the given local
LocalId findLocEquiv(State& state, const php::Func* func, LocalId l) {
  if (l >= state.equivLocals.size()) return NoLocalId;
  assertx(state.equivLocals[l] == NoLocalId || !is_volatile_local(func, l));
  return state.equivLocals[l];
}
LocalId findLocEquiv(ISS& env, LocalId l) {
  return findLocEquiv(env.state, env.ctx.func, l);
}

// Given an iterator base local, find an equivalent local that is possibly
// better. IterInit/IterNext often uses an unnamed local that came from
// a regular local, which would be a better choice if that local was not
// manipulated in an unsafe way. Regular locals have lower ids.
LocalId findIterBaseLoc(State& state, const php::Func* func, LocalId l) {
  assertx(l != NoLocalId);
  auto const locEquiv = findLocEquiv(state, func, l);
  if (locEquiv == NoLocalId) return l;
  return std::min(l, locEquiv);
}
LocalId findIterBaseLoc(ISS& env, LocalId l) {
  return findIterBaseLoc(env.state, env.ctx.func, l);
}

// Find an equivalent local with minimum id
LocalId findMinLocEquiv(ISS& env, LocalId l, bool allowUninit) {
  if (l >= env.state.equivLocals.size() ||
      env.state.equivLocals[l] == NoLocalId) {
    return NoLocalId;
  }

  auto min = l;
  auto cur = env.state.equivLocals[l];
  while (cur != l) {
    if (cur < min && (allowUninit || !peekLocCouldBeUninit(env, cur))) {
      min = cur;
    }
    cur = env.state.equivLocals[cur];
  }
  return min != l ? min : NoLocalId;
}

// Determine whether two locals are equivalent
bool locsAreEquiv(ISS& env, LocalId l1, LocalId l2) {
  if (l1 >= env.state.equivLocals.size() ||
      l2 >= env.state.equivLocals.size() ||
      env.state.equivLocals[l1] == NoLocalId ||
      env.state.equivLocals[l2] == NoLocalId) {
    return false;
  }

  auto l = l1;
  while ((l = env.state.equivLocals[l]) != l1) {
    if (l == l2) return true;
  }
  return false;
}

bool locIsThis(ISS& env, LocalId l) {
  assertx(l <= MaxLocalId);
  return l == env.state.thisLoc ||
    (env.state.thisLoc <= MaxLocalId &&
     locsAreEquiv(env, l, env.state.thisLoc));
}

void killLocEquiv(State& state, LocalId l) {
  if (l >= state.equivLocals.size()) return;
  if (state.equivLocals[l] == NoLocalId) return;
  auto loc = l;
  do {
    loc = state.equivLocals[loc];
  } while (state.equivLocals[loc] != l);
  assertx(loc != l);
  if (state.equivLocals[l] == loc) {
    state.equivLocals[loc] = NoLocalId;
  } else {
    state.equivLocals[loc] = state.equivLocals[l];
  }
  state.equivLocals[l] = NoLocalId;
}

void killLocEquiv(ISS& env, LocalId l) {
  killLocEquiv(env.state, l);
}

void killAllLocEquiv(ISS& env) {
  env.state.equivLocals.clear();
}

// Add from to to's equivalency set.
void addLocEquiv(ISS& env,
                 LocalId from,
                 LocalId to) {
  always_assert(peekLocRaw(env, from).subtypeOf(BCell));
  always_assert(!is_volatile_local(env.ctx.func, to));
  always_assert(from != to && findLocEquiv(env, from) == NoLocalId);

  auto m = std::max(to, from);
  if (env.state.equivLocals.size() <= m) {
    env.state.equivLocals.resize(m + 1, NoLocalId);
  }

  if (env.state.equivLocals[to] == NoLocalId) {
    env.state.equivLocals[from] = to;
    env.state.equivLocals[to] = from;
  } else {
    env.state.equivLocals[from] = env.state.equivLocals[to];
    env.state.equivLocals[to] = from;
  }
}

// Obtain a local which is equivalent to the given stack value
LocalId topStkLocal(const State& state, uint32_t idx = 0) {
  assertx(idx < state.stack.size());
  auto const equiv = state.stack[state.stack.size() - idx - 1].equivLoc;
  return equiv > MaxLocalId ? NoLocalId : equiv;
}
LocalId topStkLocal(ISS& env, uint32_t idx = 0) {
  return topStkLocal(env.state, idx);
}

// Obtain a location which is equivalent to the given stack value
LocalId topStkEquiv(ISS& env, uint32_t idx = 0) {
  assertx(idx < env.state.stack.size());
  return env.state.stack[env.state.stack.size() - idx - 1].equivLoc;
}

void setStkLocal(ISS& env, LocalId loc, uint32_t idx = 0) {
  assertx(loc <= MaxLocalId);
  always_assert(peekLocRaw(env, loc).subtypeOf(BCell));
  auto const equiv = [&] {
    while (true) {
      auto const e = topStkEquiv(env, idx);
      if (e != StackDupId) return e;
      idx++;
    }
  }();

  if (equiv <= MaxLocalId) {
    if (loc == equiv || locsAreEquiv(env, loc, equiv)) return;
    addLocEquiv(env, loc, equiv);
    return;
  }
  env.state.stack[env.state.stack.size() - idx - 1].equivLoc = loc;
}

void killThisLoc(ISS& env, LocalId l) {
  if (l != NoLocalId ?
      env.state.thisLoc == l : env.state.thisLoc != NoLocalId) {
    ITRACE(2, "Killing thisLoc: {}\n", env.state.thisLoc);
    env.state.thisLoc = NoLocalId;
  }
}

// Kill all equivalencies involving the given local to stack values
void killStkEquiv(ISS& env, LocalId l) {
  for (auto& e : env.state.stack) {
    if (e.equivLoc != l) continue;
    e.equivLoc = findLocEquiv(env, l);
    assertx(e.equivLoc != l);
  }
}

void killAllStkEquiv(ISS& env) {
  for (auto& e : env.state.stack) {
    if (e.equivLoc <= MaxLocalId) e.equivLoc = NoLocalId;
  }
}

void killIterEquivs(ISS& env, LocalId l, LocalId key = NoLocalId) {
  for (auto& i : env.state.iters) {
    match(
      i,
      []  (DeadIter) {},
      [&] (LiveIter& iter) {
        if (iter.setValueBase == l) {
          iter.setValueBase = NoLocalId;
          iter.setValueType = TBottom;
        }
        if (iter.keyLocal == l) iter.keyLocal = NoLocalId;
        if (iter.baseLocal == l) {
          iter.baseUpdated = true;
          if (key == NoLocalId || key != iter.keyLocal) {
            iter.baseLocal = NoLocalId;
          }
        }
      }
    );
  }
}

void killAllIterEquivs(ISS& env) {
  for (auto& i : env.state.iters) {
    match(
      i,
      [] (DeadIter) {},
      [] (LiveIter& iter) {
        iter.baseUpdated = true;
        iter.baseLocal = NoLocalId;
        iter.keyLocal = NoLocalId;
        iter.setValueBase = NoLocalId;
        iter.setValueType = TBottom;
      }
    );
  }
}

void setIterKey(ISS& env, IterId id, LocalId key) {
  match(
    env.state.iters[id],
    []  (DeadIter) {},
    [&] (LiveIter& iter) { iter.keyLocal = key; }
  );
}

Type peekLocRaw(ISS& env, LocalId l) {
  auto ret = env.state.locals[l];
  if (is_volatile_local(env.ctx.func, l)) {
    always_assert_flog(ret.is(BCell), "volatile local was not TCell");
  }
  return ret;
}

Type locRaw(ISS& env, LocalId l) {
  mayReadLocal(env, l);
  return peekLocRaw(env, l);
}

void setLocRaw(ISS& env, LocalId l, Type t) {
  mayReadLocal(env, l);
  killStkEquiv(env, l);
  killLocEquiv(env, l);
  killIterEquivs(env, l);
  killThisLoc(env, l);
  if (is_volatile_local(env.ctx.func, l)) {
    auto current = env.state.locals[l];
    always_assert_flog(current.is(BCell), "volatile local was not TCell");
    return;
  }
  if (env.undo) env.undo->onLocalWrite(l, std::move(env.state.locals[l]));
  env.state.locals[l] = std::move(t);
}

// Read a local type in the sense of CGetL. (TUninits turn into
// TInitNull)
Type locAsCell(ISS& env, LocalId l) {
  return to_cell(locRaw(env, l));
}

bool peekLocCouldBeUninit(ISS& env, LocalId l) {
  return peekLocRaw(env, l).couldBe(BUninit);
}

bool locCouldBeUninit(ISS& env, LocalId l) {
  return locRaw(env, l).couldBe(BUninit);
}

/*
 * Update the known type of a local, based on assertions (e.g. IsType/JmpCC),
 * rather than an actual modification to the local.
 */
void refineLocHelper(ISS& env, LocalId l, Type t) {
  auto v = peekLocRaw(env, l);
  assertx(v.subtypeOf(BCell));
  if (!is_volatile_local(env.ctx.func, l)) {
    if (env.undo) env.undo->onLocalWrite(l, std::move(env.state.locals[l]));
    env.state.locals[l] = std::move(t);
  }
}

/*
 * Refine all locals in an equivalence class using fun. Returns false if refined
 * local is unreachable.
 */
template<typename F>
bool refineLocation(ISS& env, LocalId l, F fun) {
  bool ok = true;
  auto refine = [&] (Type t) {
    always_assert(t.subtypeOf(BCell));
    auto i = intersection_of(fun(t), t);
    if (i.subtypeOf(BBottom)) ok = false;
    return i;
  };
  if (l == StackDupId) {
    auto stkIdx = env.state.stack.size();
    while (true) {
      --stkIdx;
      auto& stk = env.state.stack[stkIdx];
      if (env.undo) env.undo->onStackWrite(stkIdx, stk.type);
      stk.type = refine(std::move(stk.type));
      if (stk.equivLoc != StackDupId) break;
      assertx(stkIdx > 0);
    }
    l = env.state.stack[stkIdx].equivLoc;
  }
  if (l == StackThisId) {
    if (env.state.thisLoc != NoLocalId) {
      l = env.state.thisLoc;
    }
    return ok;
  }
  if (l == NoLocalId) return ok;
  assertx(l <= MaxLocalId);
  auto fixThis = false;
  auto equiv = findLocEquiv(env, l);
  if (equiv != NoLocalId) {
    do {
      if (equiv == env.state.thisLoc) fixThis = true;
      refineLocHelper(env, equiv, refine(peekLocRaw(env, equiv)));
      equiv = findLocEquiv(env, equiv);
    } while (equiv != l);
  }
  if (fixThis || l == env.state.thisLoc) {
    env.state.thisType = refine(env.state.thisType);
  }
  refineLocHelper(env, l, refine(peekLocRaw(env, l)));
  return ok;
}

/*
 * Refine locals along taken and fallthrough edges.
 */
template<typename Taken, typename Fallthrough>
void refineLocation(ISS& env, LocalId l,
                    Taken taken, BlockId target, Fallthrough fallthrough) {
  auto state = env.state;
  auto const target_reachable = refineLocation(env, l, taken);
  if (!target_reachable) jmp_nevertaken(env);
  // swap, so we can restore this state if the branch is always taken.
  env.state.swap(state);
  if (!refineLocation(env, l, fallthrough)) { // fallthrough unreachable.
    jmp_setdest(env, target);
    env.state.copy_from(std::move(state));
  } else if (target_reachable) {
    env.propagate(target, &state);
  }
}

/*
 * Set a local type in the sense of tvSet.  If the local is boxed or
 * not known to be not boxed, we can't change the type.  May be used
 * to set locals to types that include Uninit.
 */
void setLoc(ISS& env, LocalId l, Type t, LocalId key = NoLocalId) {
  killStkEquiv(env, l);
  killLocEquiv(env, l);
  killIterEquivs(env, l, key);
  killThisLoc(env, l);
  mayReadLocal(env, l);
  refineLocHelper(env, l, std::move(t));
}

LocalId findLocal(ISS& env, SString name) {
  for (auto& l : env.ctx.func->locals) {
    if (l.name->same(name)) {
      mayReadLocal(env, l.id);
      return l.id;
    }
  }
  return NoLocalId;
}

void killLocals(ISS& env) {
  ITRACE(2, "    killLocals\n");
  readUnknownLocals(env);
  for (size_t l = 0; l < env.state.locals.size(); ++l) {
    if (env.undo) env.undo->onLocalWrite(l, std::move(env.state.locals[l]));
    env.state.locals[l] = TCell;
  }
  killAllLocEquiv(env);
  killAllStkEquiv(env);
  killAllIterEquivs(env);
  killThisLoc(env, NoLocalId);
}

//////////////////////////////////////////////////////////////////////
// iterators

void setIter(ISS& env, IterId iter, Iter iterState) {
  env.state.iters[iter] = std::move(iterState);
}
void freeIter(ISS& env, IterId iter) {
  env.state.iters[iter] = DeadIter {};
}

bool iterIsDead(ISS& env, IterId iter) {
  return match<bool>(
    env.state.iters[iter],
    [] (DeadIter) { return true; },
    [] (const LiveIter&) { return false; }
  );
}

//////////////////////////////////////////////////////////////////////
// properties on $this

/*
 * Note: we are only tracking control-flow insensitive types for
 * object properties, because it can be pretty rough to try to track
 * all cases that could re-enter the VM, run arbitrary code, and
 * potentially change the type of a property.
 *
 * Because of this, the various "setter" functions for thisProps
 * here actually just union the new type into what we already had.
 */

Optional<Type> thisPropType(ISS& env, SString name) {
  if (auto const elem = env.collect.props.readPrivateProp(name)) {
    return elem->ty;
  }
  return std::nullopt;
}

bool isMaybeThisPropAttr(ISS& env, SString name, Attr attr) {
  if (attr & AttrInitialSatisfiesTC) {
    if (env.collect.props.privatePropertySatisfiesTC(name)) return true;
    attr = (Attr)(attr & ~AttrInitialSatisfiesTC);
  }
  if (!attr) return false;
  auto const& raw = env.collect.props.privatePropertiesRaw();
  auto const it = raw.find(name);
  // Prop either doesn't exist, or is on an unflattened trait. Be
  // conservative.
  if (it == raw.end()) return true;
  return it->second.attrs & attr;
}

bool isDefinitelyThisPropAttr(ISS& env, SString name, Attr attr) {
  if (attr & AttrInitialSatisfiesTC) {
    if (env.collect.props.privatePropertySatisfiesTC(name)) return true;
    attr = (Attr)(attr & ~AttrInitialSatisfiesTC);
  }
  if (!attr) return false;
  auto const& raw = env.collect.props.privatePropertiesRaw();
  auto const it = raw.find(name);
  // Prop either doesn't exist, or is on an unflattened trait. Be
  // conservative.
  if (it == raw.end()) return false;
  return it->second.attrs & attr;
}

void killThisProps(ISS& env) {
  ITRACE(2, "    killThisProps\n");
  env.collect.props.mergeInAllPrivateProps(env.index, TCell);
}

/*
 * This function returns a type that includes all the possible types
 * that could result from reading a property $this->name.
 */
Optional<Type> thisPropAsCell(ISS& env, SString name) {
  auto const ty = thisPropType(env, name);
  if (!ty) return std::nullopt;
  return to_cell(ty.value());
}

/*
 * Merge a type into the tracked property types on $this, in the sense
 * of tvSet.
 *
 * Note that all types we see that could go into an object property have to
 * loosen_all.  This is because the object could be serialized and then
 * deserialized, losing the static-ness of a string or array member, and we
 * don't guarantee deserialization would preserve a constant value object
 * property type.
 */
void mergeThisProp(ISS& env, SString name, Type type) {
  env.collect.props.mergeInPrivateProp(
    env.index,
    name,
    loosen_this_prop_for_serialization(*env.ctx.cls, name, std::move(type))
  );
}

/*
 * Merge something into each this prop.  Usually MapFn will be a
 * predicate that returns TBottom when some condition doesn't hold.
 *
 * The types given to the map function are the raw tracked types
 * (i.e. could be TUninit).
 */
template<typename MapFn>
void mergeEachThisPropRaw(ISS& env, MapFn fn) {
  for (auto const& kv : env.collect.props.privatePropertiesRaw()) {
    auto const ty = thisPropType(env, kv.first);
    assertx(ty.has_value());
    mergeThisProp(env, kv.first, fn(*ty));
  }
}

//////////////////////////////////////////////////////////////////////
// properties on self::

// Similar to $this properties above, we only track control-flow
// insensitive types for these.

void killPrivateStatics(ISS& env) {
  ITRACE(2, "    killPrivateStatics\n");
  env.collect.props.mergeInAllPrivateStatics(env.index, TInitCell, true, false);
}

//////////////////////////////////////////////////////////////////////
// misc

inline void propInitialValue(ISS& env,
                             const php::Prop& prop,
                             TypedValue val,
                             bool satisfies,
                             bool deepInit) {
  ITRACE(2, "    propInitialValue \"{}\" -> {}{}{}\n",
         prop.name, show(from_cell(val)),
         satisfies ? " (initial satisfies TC)" : "",
         deepInit ? " (deep init)" : "");
  env.collect.props.setInitialValue(prop, val, satisfies, deepInit);
}

inline PropMergeResult mergeStaticProp(ISS& env,
                                       const Type& self,
                                       const Type& name,
                                       const Type& val,
                                       bool checkUB = false,
                                       bool ignoreConst = false,
                                       bool mustBeReadOnly = false) {
  ITRACE(2, "    mergeStaticProp {}::{} -> {}\n",
         show(self), show(name), show(val));
  return env.index.merge_static_type(
    env.ctx,
    env.collect.publicSPropMutations,
    env.collect.props,
    self,
    name,
    val,
    checkUB,
    ignoreConst,
    mustBeReadOnly
  );
}

inline MemoSets memoGet(ISS& env) {
  env.collect.allMemoGets.emplace(env.bid);
  return env.collect.allMemoSets;
}

inline void memoSet(ISS& env, Type t, Type wh, bool effectFree) {
  auto reflow = false;

  t |= env.collect.allMemoSets.retTy;
  if (env.collect.allMemoSets.retTy.strictSubtypeOf(t)) {
    env.collect.allMemoSets.retTy = std::move(t);
    reflow = true;
  }
  wh |= env.collect.allMemoSets.waitHandleRetTy;
  if (env.collect.allMemoSets.waitHandleRetTy.strictSubtypeOf(wh)) {
    env.collect.allMemoSets.waitHandleRetTy = std::move(wh);
    reflow = true;
  }
  if (!effectFree && env.collect.allMemoSets.effectFree) {
    env.collect.allMemoSets.effectFree = false;
    reflow = true;
  }
  if (reflow) {
    for (auto const bid : env.collect.allMemoGets) {
      env.propagate(bid, nullptr);
    }
  }
}

//////////////////////////////////////////////////////////////////////

#ifdef __clang__
#pragma clang diagnostic pop
#endif
}

//////////////////////////////////////////////////////////////////////

}
