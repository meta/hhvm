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
#include "hphp/hhbbc/analyze.h"

#include <cstdint>
#include <cstdio>
#include <set>
#include <algorithm>
#include <string>
#include <vector>
#include <fmt/core.h>

#include "hphp/runtime/base/type-structure-helpers-defs.h"

#include "hphp/util/configs/eval.h"
#include "hphp/util/dataflow-worklist.h"
#include "hphp/util/trace.h"

#include "hphp/hhbbc/interp-state.h"
#include "hphp/hhbbc/interp.h"
#include "hphp/hhbbc/index.h"
#include "hphp/hhbbc/options.h"
#include "hphp/hhbbc/representation.h"
#include "hphp/hhbbc/cfg.h"
#include "hphp/hhbbc/class-util.h"
#include "hphp/hhbbc/func-util.h"
#include "hphp/hhbbc/options-util.h"

namespace HPHP::HHBBC {

namespace {

TRACE_SET_MOD(hhbbc)

struct KnownArgs {
  Type context;
  const CompactVector<Type>& args;
  const NamedArgNameVec& argNames;
};

//////////////////////////////////////////////////////////////////////

const StaticString s_Closure("Closure");
const StaticString s_AsyncGenerator("HH\\AsyncGenerator");
const StaticString s_Generator("Generator");

const StaticString s_invoke("__invoke");

//////////////////////////////////////////////////////////////////////

/*
 * Short-hand to get the rpoId of a block in a given FuncAnalysis.  (The RPO
 * ids are re-assigned per analysis.)
 */
uint32_t rpoId(const FuncAnalysis& ai, BlockId blk) {
  return ai.bdata[blk].rpoId;
}

uint32_t firstPositionalId(const php::Func* func) {
  auto const numParams = func->params.size();
  auto firstPositionalId = 0;
  for (; firstPositionalId < numParams; ++firstPositionalId) {
    if (!func->params[firstPositionalId].isNamed) {
      break;
    }
  }
  return firstPositionalId;
}

const StaticString s_reified_generics_var("0ReifiedGenerics");
const StaticString s_coeffects_var("0Coeffects");

Optional<State> entry_state(const IIndex& index, CollectedInfo& collect,
                            const Context& ctx, const KnownArgs* knownArgs) {
  auto ret = State{};
  ret.initialized = true;
  ret.thisType = [&] {
    if (!ctx.cls) return TNull;
    if (knownArgs && !knownArgs->context.subtypeOf(BBottom)) {
      if (knownArgs->context.subtypeOf(BOptObj)) {
        return setctx(knownArgs->context);
      }
      if (knownArgs->context.subtypeOf(BCls) &&
          is_specialized_cls(knownArgs->context)) {
        return setctx(toobj(knownArgs->context));
      }
    }
    auto const maybeSelfType = selfCls(index, ctx);
    auto thisType = maybeSelfType ? setctx(toobj(*maybeSelfType)) : TObj;
    if (ctx.func->cls &&
        !(ctx.func->cls->attrs & AttrTrait) &&
        !(ctx.func->attrs & AttrStatic)) {
      return thisType;
    }
    return opt(std::move(thisType));
  }();
  ret.locals.resize(ctx.func->locals.size());
  ret.iters.resize(ctx.func->numIters);

  /*
   * TODO(named_params) the known-args vector is indexed positionally: argument
   * i is taken to be parameter i.  A call that leaves out an optional named
   * argument shifts everything after it, so the types land on the wrong
   * parameters, and verifying one against a hint it cannot satisfy yields
   * Bottom.  A Bottom local is not merely imprecise: it reaches the rest of
   * the analysis and trips Type::checkInvariants once something builds a vec
   * out of it.
   *
   * Matching the arguments up needs argNames, and it is not as simple as
   * splitting the vector: hackc reorders a function's parameters so the named
   * ones come first, lexicographically, so there is no contiguous positional
   * region to keep.  Until that is threaded through, fall back to the
   * unknown-args treatment for a callee with named parameters.  That costs
   * precision when inlining such a call and is always sound.
   */
  auto const positionalArgs =
    (knownArgs && !ctx.func->hasNamedParams) ? knownArgs : nullptr;

  auto locId = uint32_t{0};
  for (; locId < ctx.func->params.size(); ++locId) {
    if (positionalArgs) {
      if (locId < positionalArgs->args.size()) {
        if (ctx.func->params[locId].isVariadic) {
          std::vector<Type> pack(positionalArgs->args.begin() + locId,
                                 positionalArgs->args.end());
          for (auto& p : pack) p = unctx(std::move(p));
          ret.locals[locId] = vec(std::move(pack));
        } else {
          auto [ty, _, effectFree] =
            verify_param_type(index, ctx, locId,
                              unctx(positionalArgs->args[locId]));

          if (ty.subtypeOf(BBottom)) {
            ret.unreachable = true;
            if (ctx.func->params[locId].dvEntryPoint == NoBlockId) {
              return std::nullopt;
            }
          }

          ret.locals[locId] = std::move(ty);
          collect.effectFree &= effectFree;
        }
      } else {
        ret.locals[locId] = ctx.func->params[locId].isVariadic ? TVec : TUninit;
      }
      continue;
    }

    if (ctx.func->params[locId].isVariadic) {
      ret.locals[locId] = TVec;
      continue;
    }

    // Because we throw a non-recoverable error for having fewer than the
    // required number of args, all function parameters must be initialized.
    auto [ty, _, effectFree] = verify_param_type(index, ctx, locId, TInitCell);

    if (ty.subtypeOf(BBottom)) {
      ret.unreachable = true;
      if (ctx.func->params[locId].dvEntryPoint == NoBlockId) {
        return std::nullopt;
      }
    }

    ret.locals[locId] = std::move(ty);
    collect.effectFree &= effectFree;
  }

  // Closures have use vars, we need to look up their types from the index.
  auto const useVars = [&] {
    if (!ctx.func->isClosureBody) return CompactVector<Type>{};
    return collect.closureUseVars.initial(*ctx.func);
  }();

  /*
   * Reified functions have a hidden local that's always the first
   * (non-parameter) local, which stores reified generics.
   */
  if (ctx.func->isReified) {
    // Currently closures cannot be reified
    assertx(!ctx.func->isClosureBody);
    assertx(locId < ret.locals.size());
    assertx(ctx.func->locals[locId].name->same(s_reified_generics_var.get()));
    ret.locals[locId++] = get_type_of_reified_list(ctx.func->userAttributes);
  }

  /*
   * Functions with coeffect rules have a hidden local that's always the first
   * (non-parameter) local (after reified generics, if exists),
   * which stores the ambient coeffects.
   */
  if (has_coeffects_local(ctx.func)) {
    assertx(locId < ret.locals.size());
    assertx(ctx.func->locals[locId].name->same(s_coeffects_var.get()));
    ret.locals[locId++] = TInt;
  }

  auto afterParamsLocId = uint32_t{0};
  for (; locId < ctx.func->locals.size(); ++locId, ++afterParamsLocId) {
    /*
     * Some of the closure locals are mapped to used variables. The types of
     * use vars are looked up from the index.
     */
    if (ctx.func->isClosureBody) {
      if (afterParamsLocId < useVars.size()) {
        ret.locals[locId] = useVars[afterParamsLocId];
        continue;
      }
    }

    // Otherwise the local will start uninitialized, like normal.
    ret.locals[locId] = TUninit;
  }

  // Finally, make sure any volatile locals are set to Gen, even if they are
  // parameters.
  for (auto locId = uint32_t{0}; locId < ctx.func->locals.size(); ++locId) {
    if (is_volatile_local(ctx.func, locId)) {
      ret.locals[locId] = TCell;
    }
  }

  return ret;
}

/*
 * Helper for do_analyze to initialize the states for all function entries
 * (i.e. each dv init and the main entry), and all of them count as places the
 * function could be entered, so they all must be visited at least once.
 *
 * If we're entering at a DV-init, all higher parameter locals must be
 * Uninit, with the possible exception of a final variadic param
 * (which will be an array), and all optional named parameters may be uninit.
 * It is also possible that the DV-init is reachable from within the function
 * with these parameter locals already initialized (although the normal php
 * emitter can't do this), but that case will be discovered when iterating.
 */
dataflow_worklist<uint32_t>
prepare_incompleteQ(const IIndex& index,
                    FuncAnalysis& ai,
                    CollectedInfo& collect,
                    const KnownArgs* knownArgs) {
  auto incompleteQ     = dataflow_worklist<uint32_t>(ai.rpoBlocks.size());
  auto const ctx       = ai.ctx;
  auto const numParams = ctx.func->params.size();

  auto const entryState = entry_state(index, collect, ctx, knownArgs);
  if (!entryState) return incompleteQ;

  if (knownArgs) {
    // When we have known args, we only need to add one of the entry points to
    // the initial state, since we know how many arguments were passed.
    auto const useDvInit = [&] {
      if (knownArgs->args.size() >= numParams) return false;
      for (auto i = knownArgs->args.size(); i < numParams; ++i) {
        auto const dv = ctx.func->params[i].dvEntryPoint;
        if (dv != NoBlockId) {
          ai.bdata[dv].stateIn.copy_from(*entryState);
          ai.bdata[dv].stateIn.unreachable = false;
          incompleteQ.push(rpoId(ai, dv));
          return true;
        }
      }
      return false;
    }();

    if (!useDvInit && !entryState->unreachable) {
      ai.bdata[ctx.func->mainEntry].stateIn.copy_from(*entryState);
      incompleteQ.push(rpoId(ai, ctx.func->mainEntry));
    }

    return incompleteQ;
  }

  auto const firstPos = firstPositionalId(ctx.func);
  State optionalEntryState{*entryState};
  for (auto locId = 0; locId < firstPos; ++locId) {
    if (ctx.func->params[locId].isNamed &&
        ctx.func->params[locId].dvEntryPoint != NoBlockId) {
      optionalEntryState.locals[locId] |= TUninit;
    }
  }
  for (auto paramId = firstPos; paramId < numParams; ++paramId) {
    auto const dv = ctx.func->params[paramId].dvEntryPoint;
    if (dv != NoBlockId) {
      ai.bdata[dv].stateIn.copy_from(optionalEntryState);
      ai.bdata[dv].stateIn.unreachable = false;
      incompleteQ.push(rpoId(ai, dv));
      for (auto locId = paramId; locId < numParams; ++locId) {
        ai.bdata[dv].stateIn.locals[locId] =
          ctx.func->params[locId].isVariadic ? TVec : TUninit;
      }
    }
    // If a DV-init's param has an entry state of Bottom, then none of
    // the following DV-inits are reachable.
    if (optionalEntryState.locals[paramId].is(BBottom)) break;
  }
  auto const namedDV = namedParamFuncEntry(*ctx.func);
  if (namedDV != NoBlockId) {
    ai.bdata[namedDV].stateIn.copy_from(optionalEntryState);
    ai.bdata[namedDV].stateIn.unreachable = false;
    incompleteQ.push(rpoId(ai, namedDV));
  }

  if (!entryState->unreachable) {
    ai.bdata[ctx.func->mainEntry].stateIn.copy_from(*entryState);
    incompleteQ.push(rpoId(ai, ctx.func->mainEntry));
  }

  return incompleteQ;
}

/*
 * Closures inside of classes are analyzed in the context they are
 * created in (this affects accessibility rules, access to privates,
 * etc).
 *
 * Note that in the interpreter code, ctx.func->cls is not
 * necessarily the same as ctx.cls because of closures.
 */
AnalysisContext adjust_closure_context(const IIndex& index,
                                       AnalysisContext ctx) {
  if (ctx.cls) ctx.cls = index.lookup_closure_context(*ctx.cls);
  return ctx;
}

Type fixup_return_type(const IIndex& index,
                       const php::Func& func,
                       Type ty,
                       bool canSuspend) {
  if (func.isGenerator) {
    if (func.isAsync) {
      // Async generators always return AsyncGenerator object.
      return objExact(builtin_class(index, s_AsyncGenerator.get()));
    } else {
      // Non-async generators always return Generator object.
      return objExact(builtin_class(index, s_Generator.get()));
    }
  } else if (func.isAsync) {
    // Async functions always return WaitH<T>, where T is the type returned
    // internally.
    return wait_handle(std::move(ty), canSuspend);
  } else {
    return ty;
  }
}

FuncAnalysis do_analyze_collect(const IIndex& index,
                                const AnalysisContext& ctx,
                                CollectedInfo& collect,
                                const KnownArgs* knownArgs) {
  assertx(ctx.cls == adjust_closure_context(index, ctx).cls);
  auto ai = FuncAnalysis { ctx };

  SCOPE_ASSERT_DETAIL("do-analyze-collect-2") {
    std::string ret;
    for (auto bid : ctx.func.blockRange()) {
      ret += fmt::format("block #{}\nin-{}\n{}",
                    bid,
                    state_string(*ctx.func, ai.bdata[bid].stateIn, collect),
                    show(*ctx.func, *ctx.func.blocks()[bid]));
    }

    return ret;
  };

  SCOPE_ASSERT_DETAIL("do-analyze-collect-1") {
    return "Analyzing: " + show(ctx);
  };

  Optional<std::array<Trace::Bump, 3>> bump;

  if (knownArgs) {
    using namespace folly::gen;
    FTRACE(
      2,
      "{:.^70}\n",
      fmt::format(
        "Inline Interp (context: {}, args: {}, arg names: {})",
        show(knownArgs->context),
        from(knownArgs->args)
          | map([] (const Type& t) { return show(t); })
          | unsplit<std::string>(","),
        from(knownArgs->argNames)
          | map([] (SString s) { return s->toCppString(); })
          | unsplit<std::string>(",")
      )
    );
  } else {
    // Only bump if we're not doing inline analysis. Otherwise we want
    // to keep the bump from the caller.
    bump.emplace(
      trace_bump(ctx, Trace::hhbbc, Trace::hhbbc_cfg, Trace::hhbbc_index)
    );
  }

  SCOPE_EXIT {
    if (knownArgs) {
      FTRACE(2, "{:.^70}\n", "End Inline Interp");
    }
  };

  FTRACE(
    2, "{:-^70}\n-- {}\n",
    index.frozen() ? "Analyze (frozen)" : "Analyze",
    show(ctx)
  );

  /*
   * Set of RPO ids that still need to be visited.
   *
   * Initially, we need each entry block in this list.  As we visit
   * blocks, we propagate states to their successors and across their
   * back edges---when state merges cause a change to the block
   * stateIn, we will add it to this queue so it gets visited again.
   */
  auto incompleteQ = prepare_incompleteQ(index, ai, collect, knownArgs);

  /*
   * There are potentially infinitely growing types when we're using union_of to
   * merge states, so occasionally we need to apply a widening operator.
   *
   * Currently this is done by having a straight-forward hueristic: if you visit
   * a block too many times, we'll start doing all the merges with the widening
   * operator. We must then continue iterating in case the actual fixed point is
   * higher than the result of widening. Likewise if we loop too much because of
   * local static types changing, we'll widen those.
   *
   * Termination is guaranteed because the widening operator has only finite
   * chains in the type lattice.
   */
  auto totalVisits = std::vector<uint32_t>(ctx.func.blocks().size());

  // For debugging, count how many times basic blocks get interpreted.
  DEBUG_ONLY auto interp_counter = uint32_t{0};

  hphp_fast_map<BlockId, BlockUpdateInfo> blockUpdates;
  ai.canSuspend = false;

  /*
   * Iterate until a fixed point.
   *
   * Each time a stateIn for a block changes, we re-insert the block's
   * rpo ID in incompleteQ.  Since incompleteQ is ordered, we'll
   * always visit blocks with earlier RPO ids first, which hopefully
   * means less iterations.
   */
  do {
    while (!incompleteQ.empty()) {
      auto const bid = ai.rpoBlocks[incompleteQ.pop()];

      totalVisits[bid]++;

      FTRACE(2, "block #{}\nin {}{}", bid,
             state_string(*ctx.func, ai.bdata[bid].stateIn, collect),
             property_state_string(collect.props));
      ++interp_counter;

      // Vector for consistent iteration order.
      hphp_vector_map<BlockId, State> rollbackStates;
      hphp_fast_set<BlockId> scheduled;

      auto const propagate = [&] (BlockId target, const State* st) {
        if (!st) {
          FTRACE(2, "     Force reprocess: {}\n", target);
          incompleteQ.push(rpoId(ai, target));
          return;
        }

        auto const needsWiden =
          totalVisits[target] >= options.analyzeFuncWideningLimit;

        FTRACE(2, "     {}-> {}\n", needsWiden ? "widening " : "", target);
        FTRACE(4, "\ntarget old {}\n",
               state_string(*ctx.func, ai.bdata[target].stateIn, collect));

        // Save the before start of the target (if we don't have one
        // already). If we need to rollback this block, we'll restore
        // all targets back to their original state.
        rollbackStates.try_emplace(target, ai.bdata[target].stateIn);

        auto const changed =
          needsWiden ? widen_into(ai.bdata[target].stateIn, *st)
                     : merge_into(ai.bdata[target].stateIn, *st);
        if (changed) scheduled.emplace(target);
        FTRACE(4, "target new {}",
               state_string(*ctx.func, ai.bdata[target].stateIn, collect));
        if (changed) FTRACE(2, "    changed: {}\n", target);
      };

      auto const rollback = [&] {
        for (auto& [b, st] : rollbackStates) {
          FTRACE(4, "\nrollback block #{}:\n", b);
          FTRACE(4, "target old {}\n",
                 state_string(*ctx.func, ai.bdata[b].stateIn, collect));
          ai.bdata[b].stateIn.copy_from(std::move(st));
          FTRACE(4, "target new {}\n",
                 state_string(*ctx.func, ai.bdata[b].stateIn, collect));
        }
        rollbackStates.clear();
        scheduled.clear();
      };

      auto const blk = ctx.func.blocks()[bid].get();
      auto stateOut = ai.bdata[bid].stateIn;
      auto interp   = Interp { index, ctx, collect, bid, blk, stateOut };
      auto flags    = run(interp, ai.bdata[bid].stateIn, propagate, rollback);

      for (auto const target : scheduled) incompleteQ.push(rpoId(ai, target));

      if (any(collect.opts & CollectionOpts::EffectFreeOnly) &&
          !collect.effectFree) {
        break;
      }
      if (!flags.updateInfo.replacedBcs.empty() ||
          flags.updateInfo.unchangedBcs != blk->hhbcs.size() ||
          flags.updateInfo.fallthrough != blk->fallthrough) {
        blockUpdates[bid] = std::move(flags.updateInfo);
      } else {
        blockUpdates.erase(bid);
      }

      if (flags.returned) {
        ai.inferredReturn |= std::move(*flags.returned);
        if (flags.retParam == NoLocalId) {
          ai.retParam = NoLocalId;
        } else if (ai.retParam != flags.retParam) {
          if (ai.retParam != MaxLocalId) {
            ai.retParam = NoLocalId;
          } else {
            ai.retParam = flags.retParam;
          }
        }
      }

      ai.usedParams |= flags.usedParams;
      ai.canSuspend |= flags.canSuspend;
      ai.bdata[bid].noThrow = flags.noThrow;
    }

    if (any(collect.opts & CollectionOpts::EffectFreeOnly) &&
        !collect.effectFree) {
      break;
    }
  } while (!incompleteQ.empty());

  ai.closureUseTypes = collect.closureUseVars.merged();
  ai.effectFree = collect.effectFree;
  ai.reanalyzeOnUpdate = collect.reanalyzeOnUpdate;
  ai.hasInvariantIterBase = collect.hasInvariantIterBase;
  ai.unfoldableFuncs = collect.unfoldableFuncs;
  ai.publicSPropMutations = std::move(collect.publicSPropMutations);
  for (auto& elm : blockUpdates) {
    ai.blockUpdates.emplace_back(elm.first, std::move(elm.second));
  }
  ai.inferredReturn =
    fixup_return_type(
      index,
      *ctx.func,
      std::move(ai.inferredReturn),
      ai.canSuspend
    );

  if (collect.props.hasInitialValues()) {
    for (size_t i = 0, size = ctx.cls->properties.size(); i < size; ++i) {
      auto const& prop = ctx.cls->properties[i];
      auto initial = collect.props.getInitialValue(prop);
      if (!initial) continue;
      if (ai.resolvedInitializers.isNull()) {
        ai.resolvedInitializers = decltype(ai.resolvedInitializers)::makeR();
      }
      ai.resolvedInitializers.right()->emplace_back(i, std::move(*initial));
    }
  }

  /*
   * If inferredReturn is TBottom, the callee didn't execute a return
   * at all.  (E.g. it unconditionally throws, or is an abstract
   * function body.)
   *
   * In this case, we leave the return type as TBottom, to indicate
   * the same to callers.
   */
  assertx(ai.inferredReturn.subtypeOf(BCell));

  // For debugging, print the final input states for each block.
  FTRACE(2, "{}", [&] {
    auto const bsep = std::string(60, '=') + "\n";
    auto const sep = std::string(60, '-') + "\n";
    auto ret = fmt::format(
      "{}function {} ({} block interps):\n{}",
      bsep,
      show(ctx),
      interp_counter,
      bsep
    );
    for (auto& bd : ai.bdata) {
      ret += fmt::format("{}block {}{}:\nin {}",
        sep,
        ai.rpoBlocks[bd.rpoId],
        bd.noThrow ? " (no throw)" : "",
        state_string(*ctx.func, bd.stateIn, collect));
    }
    ret += sep + bsep;
    ret += fmt::format("{}{}: Inferred return type: {}{} (can-suspend: {})\n",
      show(ctx),
      [&] () -> std::string {
        using namespace folly::gen;
        if (!knownArgs) return "";
        return fmt::format(
          " (context: {}, args: {}, arg names: {})",
          show(knownArgs->context),
          from(knownArgs->args)
            | map([] (const Type& t) { return show(t); })
            | unsplit<std::string>(","),
          from(knownArgs->argNames)
            | map([] (SString s) { return s->toCppString(); })
            | unsplit<std::string>(",")
        );
      }(),
      show(ai.inferredReturn),
      ai.effectFree ? " (effect-free)" : "",
      ai.canSuspend ? "yes" : "no");
    ret += bsep;
    return ret;
  }());
  return ai;
}

FuncAnalysis do_analyze(const IIndex& index,
                        const AnalysisContext& inputCtx,
                        ClassAnalysis* clsAnalysis,
                        ClsConstantWork* clsCnsWork = nullptr,
                        const KnownArgs* knownArgs = nullptr,
                        CollectionOpts opts = CollectionOpts{}) {
  auto const ctx = adjust_closure_context(index, inputCtx);
  ContextPusher _{index, ctx};

  // If this isn't an 86cinit, or if we're inline interping, just do a
  // normal analyze.
  if (ctx.func->name != s_86cinit.get() || knownArgs) {
    auto collect = CollectedInfo { index, ctx, clsAnalysis, opts, clsCnsWork };
    return do_analyze_collect(index, ctx, collect, knownArgs);
  }

  // Otherwise this is an 86cinit. We want to inline interp it for
  // each constant to resolve them. Since a constant can be defined in
  // terms of another, we want to repeat this until we reach a fixed
  // point.

  // Use a scratch ClsConstantWork if one hasn't been provided (this
  // is the case when we're the analyzing constants phase).
  Optional<ClsConstantWork> temp;
  if (!clsCnsWork) {
    temp.emplace(index, *ctx.cls);
    clsCnsWork = temp.get_pointer();
  }
  assertx(!clsCnsWork->next());

  // Schedule initial work
  for (auto const& cns : ctx.cls->constants) {
    if (cns.kind != ConstModifierFlags::Kind::Value) continue;
    if (!cns.val) continue;
    if (cns.val->m_type != KindOfUninit) continue;
    clsCnsWork->add(cns.name);
  }

  // Inline interp for this constant, and reschedule interp for any
  // constants which rely on this constant. Continue until there's no
  // more work.
  while (auto const cns = clsCnsWork->next()) {
    clsCnsWork->setCurrent(cns);
    SCOPE_EXIT { clsCnsWork->clearCurrent(cns); };
    auto const UNUSED bump =
      trace_bump(ctx, Trace::hhbbc, Trace::hhbbc_cfg, Trace::hhbbc_index);
    auto fa = analyze_func_inline(
      index,
      ctx,
      TCls,
      { sval(cns) },
      {},
      clsCnsWork
    );
    clsCnsWork->update(cns, unctx(std::move(fa.inferredReturn)));
  }

  // Analyze the 86cinit as a normal functions. We do this last so it
  // can take advantage of any resolved constants in clsCnsWork.
  auto collect = CollectedInfo { index, ctx, clsAnalysis, opts, clsCnsWork };
  auto ret = do_analyze_collect(index, ctx, collect, nullptr);

  // Propagate out the resolved constants so they can be reflected
  // into the Index.
  for (size_t i = 0, size = ctx.cls->constants.size(); i < size; ++i) {
    auto const& cns = ctx.cls->constants[i];
    if (cns.kind != ConstModifierFlags::Kind::Value) continue;
    if (!cns.val) continue;
    if (cns.val->m_type != KindOfUninit) continue;
    auto& info = clsCnsWork->constants.at(cns.name);
    if (ret.resolvedInitializers.isNull()) {
      ret.resolvedInitializers = decltype(ret.resolvedInitializers)::makeL();
    }
    assertx(ret.resolvedInitializers.left());
    ret.resolvedInitializers.left()->emplace_back(i, info);
  }
  return ret;
}

//////////////////////////////////////////////////////////////////////

/*
 * In the case of HNI builtin classes, private properties are
 * allowed to be mutated by native code, so we may not see all the
 * modifications.
 *
 * We are allowed to assume the type annotation on the property is
 * accurate, although nothing is currently checking that this is the
 * case.  We handle this right now by doing inference as if it
 * couldn't be affected by native code, then assert the inferred
 * type is at least a subtype of the annotated type, and expanding
 * it to be the annotated type if it is bigger.
 */
void expand_hni_prop_types(ClassAnalysis& clsAnalysis) {
  auto relax_prop = [&] (const php::Prop& prop, PropState& propState) {
    auto it = propState.find(prop.name);
    if (it == end(propState)) return;

    it->second.everModified = true;

    /*
     * When any functions are interceptable, we don't require the constraints to
     * actually match, and relax all the HNI types to Gen.
     *
     * This is because with any interceptable functions, it's quite
     * possible that some function calls in systemlib might not be
     * known to return things matching the property type hints for
     * some properties, or not to take their arguments by reference.
     */
    auto const hniTy = from_hni_constraint(prop.userType);
    if (it->second.ty.subtypeOf(hniTy)) {
      it->second.ty = hniTy;
      return;
    }

    always_assert_flog(
      false,
      "HNI class {}::{} inferred property type ({}) doesn't "
        "match annotation ({})\n",
      clsAnalysis.ctx.cls->name,
      prop.name,
      show(it->second.ty),
      show(hniTy)
    );
  };

  for (auto& prop : clsAnalysis.ctx.cls->properties) {
    relax_prop(prop, clsAnalysis.privateProperties);
    relax_prop(prop, clsAnalysis.privateStatics);
  }
}

//////////////////////////////////////////////////////////////////////

using Invariance = php::Const::Invariance;

// Try to determine the appropriate invariance field for a (resolved)
// type class constant. This requires looking at all of the subclasses
// and comparing how they resolve the same type class constant.
Invariance determine_invariance(const IIndex& index,
                                const Context& ctx,
                                SString name,
                                SArray resolved) {
  auto const rcls = index.resolve_class(*ctx.cls);
  assertx(rcls);

  auto const check = tvIsString(resolved->get(s_classname));
  auto invariance = Invariance::Same;

  auto const full = rcls->forEachSubclass(
    [&] (SString subn, Attr) {
      if (subn->tsame(ctx.cls->name)) return;

      // Subclass isn't present, be conservative.
      auto const srcls = index.resolve_class(subn);
      if (!srcls) {
        invariance = Invariance::None;
        return;
      }

      auto const lookup =
        index.lookup_class_type_constant(clsExact(*srcls, true), sval(name));
      auto const sresolved = lookup.resolution.sarray();
      if (!sresolved) {
        // We can't resolve it, so we can't assume anything.
        invariance = Invariance::None;
        return;
      }

      if (sresolved == resolved) return;

      // The resolved type structure in this subclass is not the
      // same. We might still be able to assert that a classname is
      // always present, or a resolved type structure at least exists.
      if (invariance == Invariance::Same ||
          invariance == Invariance::ClassnamePresent) {
        invariance = (check && tvIsString(sresolved->get(s_classname)))
          ? Invariance::ClassnamePresent
          : Invariance::Present;
      }
    }
  );
  // If we don't have complete children here, the only reason is
  // because this is a conservative class, so be conservative.
  return full ? invariance : Invariance::None;
}

void resolve_type_constants(const IIndex& index,
                            const Context& ctx,
                            ClassAnalysis& analysis) {
  auto const UNUSED bump =
    trace_bump(ctx, Trace::hhbbc, Trace::hhbbc_cfg, Trace::hhbbc_index);

  InTypeCns _{index};

  for (auto const& pair :
         index.lookup_flattened_class_type_constants(*ctx.cls)) {
    auto const name = pair.first;
    auto const idx = pair.second;

    SCOPE_ASSERT_DETAIL("resolve-type-constants") {
      return fmt::format(
        "Resolving {}::{} -> {}",
        idx.cls->data(), name->data(), show(ctx)
      );
    };

    if (idx.cls->tsame(ctx.cls->name)) {
      assertx(idx.idx < ctx.cls->constants.size());
      auto const& cns = ctx.cls->constants[idx.idx];
      if (cns.resolvedTypeStructure) {
        if (cns.invariance != Invariance::None) continue;
        auto const invariance = determine_invariance(
          index,
          ctx,
          name,
          cns.resolvedTypeStructure
        );
        if (invariance != Invariance::None) {
          analysis.resolvedTypeConsts.emplace_back(
            ResolvedClsTypeConst{
              cns.resolvedTypeStructure,
              cns.contextInsensitive,
              idx,
              invariance
            }
          );
        }
        continue;
      }
    }

    FTRACE(2, "{:-^70}\n-- ({}) {}::{}\n", "Resolve", show(ctx), idx.cls, name);

    auto const lookup = index.lookup_class_type_constant(*ctx.cls, name, idx);
    auto const ts = lookup.resolution.sarray();
    if (!ts) {
      FTRACE(2, "  no resolution\n");
      continue;
    }

    FTRACE(
      2, "  resolved to {}{}\n",
      staticArrayStreamer(ts),
      lookup.resolution.contextSensitive ? " (context sensitive)" : ""
    );

    auto const invariance = determine_invariance(index, ctx, name, ts);
    analysis.resolvedTypeConsts.emplace_back(
      ResolvedClsTypeConst{
        ts,
        !lookup.resolution.contextSensitive,
        idx,
        invariance
      }
    );
  }
}

//////////////////////////////////////////////////////////////////////

}

//////////////////////////////////////////////////////////////////////

const php::Func* ClassAnalysisWorklist::next() {
  if (worklist.empty()) return nullptr;
  auto f = worklist.front();
  inWorklist.erase(f);
  worklist.pop_front();
  return f;
}

bool ClassAnalysisWorklist::schedule(const php::Func& f) {
  auto const insert = inWorklist.emplace(&f);
  if (!insert.second) return false;
  worklist.emplace_back(&f);
  return true;
}

void ClassAnalysisWorklist::scheduleForProp(SString name) {
  auto const it = propDeps.find(name);
  if (it == propDeps.end()) return;
  for (auto const f : it->second) schedule(*f);
}

void ClassAnalysisWorklist::scheduleForPropMutate(SString name) {
  auto const it = propMutateDeps.find(name);
  if (it == propMutateDeps.end()) return;
  for (auto const f : it->second) schedule(*f);
}

void ClassAnalysisWorklist::scheduleForReturnType(const php::Func& callee) {
  auto const it = returnTypeDeps.find(&callee);
  if (it == returnTypeDeps.end()) return;
  for (auto const f : it->second) schedule(*f);
}

void ClassAnalysisWorklist::scheduleForUseVars(const php::Func& invoke) {
  auto const it = useVarDeps.find(&invoke);
  if (it == useVarDeps.end()) return;
  for (auto const f : it->second) schedule(*f);
}

//////////////////////////////////////////////////////////////////////

ClsConstantWork::ClsConstantWork(const IIndex& index,
                                 const php::Class& cls): cls{cls} {
  auto initial = index.lookup_class_constants(cls);
  for (auto& [name, info] : initial) constants.emplace(name, std::move(info));
}

ClsConstLookupResult ClsConstantWork::lookup(SString name) {
  auto const it = constants.find(name);
  if (it == end(constants)) {
    return ClsConstLookupResult{ TBottom, TriBool::No, true };
  }
  if (current) deps[name].emplace(current);
  return ClsConstLookupResult{
    it->second.type,
    TriBool::Yes,
    !is_scalar(it->second.type) || bool(cls.attrs & AttrInternal)
  };
}

void ClsConstantWork::update(SString name, Type t) {
  auto& old = constants.at(name);
  if (t.strictlyMoreRefined(old.type)) {
    if (old.refinements < options.returnTypeRefineLimit) {
      old.type = std::move(t);
      ++old.refinements;
      schedule(name);
    } else {
      FTRACE(
        1, "maxed out refinements for class constant {}::{}\n",
        cls.name, name
      );
    }
  } else {
    always_assert_flog(
      t.moreRefined(old.type),
      "Class constant type invariant violated for {}::{}.\n"
      "   {} is not at least as refined as {}\n",
      cls.name,
      name,
      show(t),
      show(old.type)
    );
  }
}

void ClsConstantWork::add(SString name) {
  auto const emplaced = inWorklist.emplace(name);
  if (!emplaced.second) return;
  worklist.emplace_back(name);
}

void ClsConstantWork::schedule(SString name) {
  auto const it = deps.find(name);
  if (it == end(deps)) return;
  for (auto const d : it->second) {
    FTRACE(2, "Scheduling {}::{} because of {}::{}\n",
           cls.name, d, cls.name, name);
    add(d);
  }
}

SString ClsConstantWork::next() {
  if (worklist.empty()) return nullptr;
  auto n = worklist.front();
  inWorklist.erase(n);
  worklist.pop_front();
  return n;
}

//////////////////////////////////////////////////////////////////////

FuncAnalysisResult::FuncAnalysisResult(AnalysisContext ctx)
  : ctx(ctx)
  , inferredReturn(TBottom)
{
}

FuncAnalysis::FuncAnalysis(AnalysisContext ctx)
  : FuncAnalysisResult{ctx}
  , rpoBlocks{rpoSortAddDVs(ctx.func)}
  , bdata{ctx.func.blocks().size()}
{
  for (auto rpoId = size_t{0}; rpoId < rpoBlocks.size(); ++rpoId) {
    bdata[rpoBlocks[rpoId]].rpoId = rpoId;
  }
}

FuncAnalysis analyze_func(const IIndex& index, const AnalysisContext& ctx,
                          CollectionOpts opts) {
  return do_analyze(index, ctx, nullptr, nullptr, nullptr, opts);
}

FuncAnalysis analyze_func_inline(const IIndex& index,
                                 const AnalysisContext& ctx,
                                 const Type& thisType,
                                 const CompactVector<Type>& args,
                                 const NamedArgNameVec& argNames,
                                 ClsConstantWork* clsCnsWork,
                                 CollectionOpts opts) {
  auto const knownArgs = KnownArgs {
    ctx.func->isClosureBody ? TBottom : thisType,
    args,
    argNames
  };

  return do_analyze(index, ctx, nullptr, clsCnsWork, &knownArgs,
                    opts | CollectionOpts::Inlining);
}

ClassAnalysis analyze_class_separate(const AnalysisIndex& index,
                                     const Context& ctx) {
  assertx(ctx.cls);
  assertx(!ctx.func);
  assertx(!is_closure(*ctx.cls));

  {
    auto const UNUSED bump =
      trace_bump(ctx, Trace::hhbbc, Trace::hhbbc_cfg, Trace::hhbbc_index);
    FTRACE(
      2, "{:#^70}\n-- {}\n",
      index.frozen() ? " Class (Frozen) " : " Class ",
      ctx.cls->name
    );
  }

  AnalysisIndexAdaptor adaptor{index};
  ContextPusher _{adaptor, ctx};
  ClassAnalysis analysis{ctx};

  if (!is_used_trait(*ctx.cls)) {
    resolve_type_constants(adaptor, ctx, analysis);
  }

  auto const associatedClosures = [&] {
    CompactVector<const php::Class*> out;
    out.reserve(ctx.cls->closures.size());
    for (auto& clo : ctx.cls->closures) out.emplace_back(clo.get());
    return out;
  }();

  for (auto const c : associatedClosures) {
    auto const f = c->methods[0].get();
    auto const UNUSED bump =
      trace_bump(*f, Trace::hhbbc, Trace::hhbbc_cfg, Trace::hhbbc_index);
    auto const wf = php::WideFunc::cns(f);
    analysis.closures.emplace_back(
      analyze_func(
        adaptor,
        AnalysisContext { ctx.unit, wf, ctx.cls },
        CollectionOpts{}
      )
    );
  }

  for (auto const& m : ctx.cls->methods) {
    auto const UNUSED bump =
      trace_bump(*m, Trace::hhbbc, Trace::hhbbc_cfg, Trace::hhbbc_index);
    auto const wf = php::WideFunc::cns(m.get());
    analysis.methods.emplace_back(
      analyze_func(
        adaptor,
        AnalysisContext { ctx.unit, wf, ctx.cls },
        CollectionOpts{}
      )
    );
  }

  return analysis;
}

ClassAnalysis analyze_class(const IIndex& index, const Context& ctx) {
  assertx(ctx.cls);
  assertx(!ctx.func);
  assertx(!is_used_trait(*ctx.cls));
  // Uncomment when local execution is gone
  //assertx(!is_closure(*ctx.cls));

  {
    auto const UNUSED bump =
      trace_bump(ctx, Trace::hhbbc, Trace::hhbbc_cfg, Trace::hhbbc_index);
    FTRACE(
      2, "{:#^70}\n-- {}\n",
      index.frozen() ? " Class (Frozen) " : " Class ",
      ctx.cls->name
    );
  }

  ContextPusher _{index, ctx};
  ClassAnalysis clsAnalysis(ctx);

  resolve_type_constants(index, ctx, clsAnalysis);

  auto const associatedClosures = [&] {
    CompactVector<const php::Class*> out;
    out.reserve(ctx.cls->closures.size());
    for (auto& clo : ctx.cls->closures) out.emplace_back(clo.get());
    return out;
  }();
  auto const associatedMethods  = index.lookup_extra_methods(*ctx.cls);
  auto const isHNIBuiltin       = ctx.cls->attrs & AttrBuiltin;

  /*
   * Initialize inferred private property types to their in-class
   * initializers.
   *
   * We need to loosen_all on instance properties, because the class could be
   * unserialized, which we don't guarantee preserves those aspects of the
   * type.
   *
   * Also, set Uninit properties to TBottom, so that analysis
   * of 86pinit methods sets them to the correct type.
   */

  auto const initialMightBeBad = [&] (const php::Prop& prop,
                                      const Type& initial) {
    if (is_closure(*ctx.cls)) return false;
    if (prop.attrs & (AttrSystemInitialValue | AttrLateInit)) return false;
    return std::any_of(
      prop.typeConstraints.range().begin(), prop.typeConstraints.range().end(),
      [&] (const TypeConstraint& tc) {
        return !initial.moreRefined(
          lookup_constraint(index, ctx, tc, initial).lower
        );
      }
    );
  };

  for (size_t propIdx = 0, size = ctx.cls->properties.size();
       propIdx < size; ++propIdx) {
    auto const UNUSED bump =
      trace_bump(ctx, Trace::hhbbc, Trace::hhbbc_cfg, Trace::hhbbc_index);

    auto const& prop = ctx.cls->properties[propIdx];
    auto const cellTy = from_cell(prop.val);

    if (!(prop.attrs & AttrInitialSatisfiesTC) &&
        !cellTy.subtypeOf(BUninit)) {
      if (!initialMightBeBad(prop, cellTy)) {
        clsAnalysis.resolvedProps.emplace_back(
          propIdx, PropInitInfo{prop.val, true, false}
        );
      }
    }

    if (!(prop.attrs & AttrPrivate)) continue;

    if (isHNIBuiltin) {
      auto const hniTy = from_hni_constraint(prop.userType);
      always_assert_flog(
        cellTy.subtypeOf(hniTy),
        "hni {}::{} has impossible type. "
        "The annotation says it is type ({}) "
        "but the default value is type ({}).\n",
        ctx.cls->name,
        prop.name,
        show(hniTy),
        show(cellTy)
      );
    }

    if (!(prop.attrs & AttrStatic)) {
      auto t = loosen_this_prop_for_serialization(*ctx.cls, prop.name, cellTy);

      if (!is_closure(*ctx.cls) && t.subtypeOf(BUninit)) {
        /*
         * For non-closure classes, a property of type KindOfUninit
         * means that it has non-scalar initializer which will be set
         * by a 86pinit method.  For these classes, we want the
         * initial type of the property to be the type set by the
         * 86pinit method, so we set the type to TBottom.
         *
         * Closures will not have an 86pinit body, but still may have
         * properties of kind KindOfUninit (they will later contain
         * used variables).  We don't want to touch those.
         */
        t = TBottom;
      } else if (!(prop.attrs & AttrSystemInitialValue)) {
        t = adjust_type_for_prop(
          index, *ctx.cls, &prop.typeConstraints, t
        );
      }
      auto& elem = clsAnalysis.privateProperties[prop.name];
      elem.ty = std::move(t);
      elem.tc = &prop.typeConstraints;
      elem.attrs = prop.attrs;
      elem.everModified = false;
    } else {
      // Same thing as the above regarding TUninit and TBottom.
      // Static properties don't need to exclude closures for this,
      // though---we use instance properties for the closure use vars.
      auto t = cellTy.subtypeOf(BUninit)
        ? TBottom
        : (prop.attrs & AttrSystemInitialValue)
          ? cellTy
          : adjust_type_for_prop(
            index, *ctx.cls, &prop.typeConstraints, cellTy
          );
      auto& elem = clsAnalysis.privateStatics[prop.name];
      elem.ty = std::move(t);
      elem.tc = &prop.typeConstraints;
      elem.attrs = prop.attrs;
      elem.everModified = false;
    }
  }

  /*
   * For builtins, we assume the runtime can write to the properties
   * in un-analyzable ways (but won't violate their type-hint). So,
   * expand the analyzed types to at least include the type-hint.
   */
  if (isHNIBuiltin) expand_hni_prop_types(clsAnalysis);

  ClsConstantWork clsCnsWork{index, *ctx.cls};

  /*
   * For classes with non-scalar initializers, the 86pinit, 86sinit,
   * 86linit, 86cinit, and 86reifiedinit methods are guaranteed to run
   * before any other method, and are never called afterwards. Thus,
   * we can analyze these methods first to determine the initial types
   * of properties with non-scalar initializers, and these need not be
   * be run again as part of the fixedpoint computation.
   */
  CompactVector<FuncAnalysis> initResults;
  for (auto const& f : ctx.cls->methods) {
    if (!is_86init_func(*f)) continue;
    auto const wf = php::WideFunc::cns(f.get());
    auto const context = AnalysisContext { ctx.unit, wf, ctx.cls };
    initResults.emplace_back(
      do_analyze(index, context, &clsAnalysis, &clsCnsWork)
    );
  }

  ClassAnalysisWork work;
  clsAnalysis.work = &work;

  for (auto const& fa : initResults) {
    auto const resolved = fa.resolvedInitializers.right();
    if (!resolved) continue;
    for (auto const& [idx, init] : *resolved) {
      if (!init.satisfies) continue;
      assertx(idx < ctx.cls->properties.size());
      work.privatePropsSatisfiesTC.emplace(ctx.cls->properties[idx].name);
    }
  }

  // NB: Properties can still be TBottom at this point if their initial values
  // cannot possibly satisfy their type-constraints. The classes of such
  // properties cannot be instantiated.

  /*
   * Similar to the function case in do_analyze, we have to handle the
   * fact that there are infinitely growing chains in our type lattice
   * under union_of.
   *
   * So if we've visited a func some number of times and still aren't
   * at a fixed point, we'll set the property state to the result of
   * widening the old state with the new state, and then reset the
   * counter.  This guarantees eventual termination.
   */

  clsAnalysis.methods.reserve(initResults.size() + ctx.cls->methods.size());
  for (auto& m : initResults) {
    clsAnalysis.methods.emplace_back(std::move(m));
  }
  clsAnalysis.closures.reserve(associatedClosures.size());

  auto const startPrivateProperties = clsAnalysis.privateProperties;
  auto const startPrivateStatics = clsAnalysis.privateStatics;

  struct FuncMeta {
    SString unit;
    const php::Class* cls;
    CompactVector<FuncAnalysisResult>* output;
    size_t startReturnRefinements;
    size_t localReturnRefinements = 0;
    int outputIdx = -1;
    size_t visits = 0;
  };
  hphp_fast_map<const php::Func*, FuncMeta> funcMeta;

  auto const getMeta = [&] (const php::Func& f) -> FuncMeta& {
    auto metaIt = funcMeta.find(&f);
    always_assert(metaIt != funcMeta.end());
    return metaIt->second;
  };

  // Build up the initial worklist:
  for (auto const& f : ctx.cls->methods) {
    if (is_86init_func(*f)) continue;
    auto const UNUSED bump =
      trace_bump(*f, Trace::hhbbc, Trace::hhbbc_cfg, Trace::hhbbc_index);
    auto const DEBUG_ONLY inserted = work.worklist.schedule(*f);
    assertx(inserted);
    auto [type, refinements] = index.lookup_return_type_raw(f.get());
    work.returnTypes.emplace(f.get(), std::move(type));
    funcMeta.emplace(
      f.get(),
      FuncMeta{ctx.unit, ctx.cls, &clsAnalysis.methods, refinements}
    );
  }

  for (auto const c : associatedClosures) {
    auto const f = c->methods[0].get();
    auto const UNUSED bump =
      trace_bump(*f, Trace::hhbbc, Trace::hhbbc_cfg, Trace::hhbbc_index);
    auto const DEBUG_ONLY inserted = work.worklist.schedule(*f);
    assertx(inserted);
    auto [type, refinements] = index.lookup_return_type_raw(f);
    work.returnTypes.emplace(f, std::move(type));
    work.useVars.emplace(f, index.lookup_closure_use_vars_raw(*f));
    funcMeta.emplace(
      f, FuncMeta{ctx.unit, c, &clsAnalysis.closures, refinements}
    );
  }
  for (auto const m : associatedMethods) {
    auto const DEBUG_ONLY inserted = work.worklist.schedule(*m);
    assertx(inserted);
    funcMeta.emplace(
      m,
      FuncMeta{m->unit, ctx.cls, nullptr, 0, 0}
    );
  }

  // Suppose we first analyze a function which has a CreateCl and
  // record use-var types, then later analysis (within analyze_class)
  // determines the CreateCl was dead. In that case, we'll overwrite
  // the original FuncAnalysisResult (containing the use-var types)
  // with the new one (containing no use-var types because we didn't
  // see a CreateCl). The lack of info in the FuncAnalysisResult will
  // prevent the index from being updated, even though our analysis
  // might depend on some of those initial use-var types. This can
  // cause monotonicity violations. To prevent this, when overwriting
  // a FuncAnalysisResult entry, always make sure to copy over any
  // use-var type into the new one. This ensures that if ever see a
  // CreateCl, the FuncAnalysisResult will always have a record of
  // that.
  auto const persistUseTypes = [&] (const FuncAnalysisResult& from,
                                    FuncAnalysisResult& to) {
    for (auto const& [clo, types] : from.closureUseTypes) {
      // Will only add closure entries which don't already exist.
      to.closureUseTypes.emplace(clo, types);
    }
  };

  // Keep analyzing until we have more no functions scheduled (the
  // fixed point).
  while (!work.worklist.empty()) {
    hphp_fast_set<const php::Func*> changed;

    // First analyze funcs until we hit a fixed point for the
    // properties. Until we reach that, the return types are *not*
    // guaranteed to be correct.
    while (auto const f = work.worklist.next()) {
      auto& meta = getMeta(*f);

      auto const wf = php::WideFunc::cns(f);
      auto const context = AnalysisContext { meta.unit, wf, meta.cls };
      auto results = do_analyze(index, context, &clsAnalysis, &clsCnsWork);

      if (meta.output) {
        if (meta.outputIdx < 0) {
          meta.outputIdx = meta.output->size();
          meta.output->emplace_back(std::move(results));
        } else {
          auto& output = (*meta.output)[meta.outputIdx];
          persistUseTypes(output, results);
          output = std::move(results);
        }
      }

      if (meta.visits++ >= options.analyzeClassWideningLimit) {
        for (auto& prop : clsAnalysis.privateProperties) {
          auto wide = widen_type(prop.second.ty);
          if (prop.second.ty.strictlyMoreRefined(wide)) {
            prop.second.ty = std::move(wide);
            work.worklist.scheduleForProp(prop.first);
          }
        }
        for (auto& prop : clsAnalysis.privateStatics) {
          auto wide = widen_type(prop.second.ty);
          if (prop.second.ty.strictlyMoreRefined(wide)) {
            prop.second.ty = std::move(wide);
            work.worklist.scheduleForProp(prop.first);
          }
        }
      }
    }

    // We've hit a fixed point for the properties. Other local
    // information (such as return type information) is now correct
    // (but might not be optimal).

    // Reflect any improved return types into the results. This will
    // make them available for local analysis and they'll eventually
    // be written back into the Index.
    for (auto& kv : funcMeta) {
      auto const f = kv.first;
      auto& meta = kv.second;
      if (!meta.output) continue;
      assertx(meta.outputIdx >= 0);
      auto& results = (*meta.output)[meta.outputIdx];

      auto const UNUSED bump =
        trace_bump(*f, Trace::hhbbc, Trace::hhbbc_cfg, Trace::hhbbc_index);

      auto const oldTypeIt = work.returnTypes.find(f);
      assertx(oldTypeIt != work.returnTypes.end());
      auto& oldInfo = oldTypeIt->second;
      auto& oldType = oldInfo.t;
      auto& oldEffectFree = oldInfo.effectFree;

      // Heed the return type refinement limit
      if (results.inferredReturn.strictlyMoreRefined(oldType)) {
        if (meta.startReturnRefinements + meta.localReturnRefinements
            < options.returnTypeRefineLimit) {
          oldType = results.inferredReturn;
          work.worklist.scheduleForReturnType(*f);
          changed.emplace(f);
        } else if (meta.localReturnRefinements > 0) {
          results.inferredReturn = oldType;
          results.effectFree = oldEffectFree;
        }
        ++meta.localReturnRefinements;
      } else {
        always_assert_flog(
          results.inferredReturn.moreRefined(oldType),
          "Index return type invariant violated in {} {}.\n"
          "   {} is not at least as refined as {}\n",
          f->unit,
          func_fullname(*f),
          show(results.inferredReturn),
          show(oldType)
        );
      }

      if (results.effectFree) {
        if (!oldEffectFree) {
          oldEffectFree = true;
          work.worklist.scheduleForReturnType(*f);
          changed.emplace(f);
        }
      } else {
        always_assert_flog(
          !oldEffectFree,
          "Index effect-free invariant violated in {} {}.\n"
          "    Went from true to false\n",
          f->unit,
          func_fullname(*f)
        );
      }

      if (results.canSuspend != oldInfo.canSuspend) {
        always_assert_flog(
          canSuspendUpdateAllowed(results.canSuspend, oldInfo.canSuspend),
          "Index canSuspend invariant violated in {} {}.\n"
          "    Went from {} to {}\n",
          f->unit,
          func_fullname(*f),
          oldInfo.canSuspend ? "yes" : "no",
          results.canSuspend ? "yes" : "no"
        );
        oldInfo.canSuspend = results.canSuspend;
        work.worklist.scheduleForReturnType(*f);
        changed.emplace(f);
      }

      for (auto const& [clo, useVars] : results.closureUseTypes) {
        assertx(is_closure(*clo));
        auto invoke = find_method(clo, s_invoke.get());
        always_assert(invoke);

        auto const old = folly::get_ptr(work.useVars, invoke);
        if (!old) continue;

        auto anyChanged = false;
        for (size_t i = 0, size = useVars.size(); i < size; ++i) {
          auto u = useVars.get_default(i, TCell);
          auto const o = old->get_default(i, TCell);

          if (u.strictlyMoreRefined(o)) {
            old->ensure(i, TCell) = std::move(u);
            anyChanged = true;
          } else {
            always_assert_flog(
              u.moreRefined(o),
              "Index closure use-var invariant violated for {}.\n"
              "   {} is not at least as refined as {}\n",
              clo->name,
              show(u),
              show(o)
            );
          }
        }

        if (anyChanged) {
          work.worklist.scheduleForUseVars(*invoke);
          changed.emplace(invoke);
        }
      }

      results.localReturnRefinements = meta.localReturnRefinements;
      if (results.localReturnRefinements > 0) --results.localReturnRefinements;
    }

    // We've made the return types available for local analysis. Now
    // iterate again and see if we can improve them.
    while (auto const f = work.worklist.next()) {
      auto& meta = getMeta(*f);

      auto const wf = php::WideFunc::cns(f);
      auto const context = AnalysisContext { meta.unit, wf, meta.cls };

      work.noPropRefine = true;
      auto results = do_analyze(index, context, &clsAnalysis, &clsCnsWork);
      work.noPropRefine = false;

      if (!meta.output) continue;

      auto const UNUSED bump =
        trace_bump(context, Trace::hhbbc, Trace::hhbbc_cfg, Trace::hhbbc_index);

      auto returnTypeIt = work.returnTypes.find(f);
      assertx(returnTypeIt != work.returnTypes.end());

      auto& oldInfo = returnTypeIt->second;
      auto& oldReturn = oldInfo.t;
      auto& oldEffectFree = oldInfo.effectFree;

      // Heed the return type refinement limit
      if (results.inferredReturn.strictlyMoreRefined(oldReturn)) {
        if (meta.startReturnRefinements + meta.localReturnRefinements
            < options.returnTypeRefineLimit) {
          oldReturn = results.inferredReturn;
          work.worklist.scheduleForReturnType(*f);
          changed.emplace(f);
        } else if (meta.localReturnRefinements > 0) {
          results.inferredReturn = oldReturn;
          results.effectFree = oldEffectFree;
        }
        ++meta.localReturnRefinements;
      } else {
        always_assert_flog(
          results.inferredReturn.moreRefined(oldReturn),
          "Index return type invariant violated in {} {}.\n"
          "   {} is not at least as refined as {}\n",
          f->unit,
          func_fullname(*f),
          show(results.inferredReturn),
          show(oldReturn)
        );
      }

      if (results.effectFree) {
        if (!oldEffectFree) {
          oldEffectFree = true;
          work.worklist.scheduleForReturnType(*f);
          changed.emplace(f);
        }
      } else {
        always_assert_flog(
          !oldEffectFree,
          "Index effect-free invariant violated in {} {}.\n"
          "    Went from true to false\n",
          f->unit,
          func_fullname(*f)
        );
      }

      if (results.canSuspend != oldInfo.canSuspend) {
        always_assert_flog(
          canSuspendUpdateAllowed(results.canSuspend, oldInfo.canSuspend),
          "Index canSuspend invariant violated in {} {}.\n"
          "    Went from {} to {}\n",
          f->unit,
          func_fullname(*f),
          oldInfo.canSuspend ? "yes" : "no",
          results.canSuspend ? "yes" : "no"
        );
        oldInfo.canSuspend = results.canSuspend;
        work.worklist.scheduleForReturnType(*f);
        changed.emplace(f);
      }

      for (auto const& [clo, useVars] : results.closureUseTypes) {
        assertx(is_closure(*clo));
        auto invoke = find_method(clo, s_invoke.get());
        always_assert(invoke);

        auto const old = folly::get_ptr(work.useVars, invoke);
        if (!old) continue;

        auto anyChanged = false;
        for (size_t i = 0, size = useVars.size(); i < size; ++i) {
          auto u = useVars.get_default(i, TCell);
          auto const o = old->get_default(i, TCell);

          if (u.strictlyMoreRefined(o)) {
            old->ensure(i, TCell) = std::move(u);
            anyChanged = true;
          } else {
            always_assert_flog(
              u.moreRefined(o),
              "Index closure use-var invariant violated for {}.\n"
              "   {} is not at least as refined as {}\n",
              clo->name,
              show(u),
              show(o)
            );
          }
        }

        if (anyChanged) {
          work.worklist.scheduleForUseVars(*invoke);
          changed.emplace(invoke);
        }
      }

      results.localReturnRefinements = meta.localReturnRefinements;
      if (results.localReturnRefinements > 0) --results.localReturnRefinements;

      assertx(meta.outputIdx >= 0);
      auto& output = (*meta.output)[meta.outputIdx];
      persistUseTypes(output, results);
      output = std::move(results);
    }

    // Return types have reached a fixed point. However, this means
    // that we might be able to further improve property types. So, if
    // a method has an improved return return, examine the methods
    // which depend on that return type. Drop any property info for
    // properties those methods write to. Reschedule any methods which
    // or write to those properties. The idea is we want to re-analyze
    // all mutations of those properties again, since the refined
    // returned types may result in better property types. This
    // process may repeat multiple times, but will eventually reach a
    // fixed point.

    if (!work.propMutators.empty()) {
      auto const resetProp = [&] (SString name,
                                  const PropState& src,
                                  PropState& dst) {
        auto dstIt = dst.find(name);
        auto const srcIt = src.find(name);
        if (dstIt == dst.end()) {
          assertx(srcIt == src.end());
          return;
        }
        assertx(srcIt != src.end());
        dstIt->second.ty = srcIt->second.ty;
        dstIt->second.everModified = srcIt->second.everModified;
      };

      hphp_fast_set<SString> retryProps;
      auto const addProps = [&] (const hphp_fast_set<const php::Func*>& deps) {
        for (auto const dep : deps) {
          auto const propsIt = work.propMutators.find(dep);
          if (propsIt == work.propMutators.end()) continue;
          for (auto const prop : propsIt->second) retryProps.emplace(prop);
        }
      };

      for (auto const f : changed) {
        if (auto const deps = work.worklist.depsForReturnType(*f)) {
          addProps(*deps);
        }
        if (auto const deps = work.worklist.depsForUseVars(*f)) {
          addProps(*deps);
        }
      }

      // Schedule the funcs which mutate the props before the ones
      // that read them.
      for (auto const prop : retryProps) {
        resetProp(prop, startPrivateProperties,
                  clsAnalysis.privateProperties);
        resetProp(prop, startPrivateStatics,
                  clsAnalysis.privateStatics);
        work.worklist.scheduleForPropMutate(prop);
      }
      for (auto const prop : retryProps) {
        work.worklist.scheduleForProp(prop);
      }
    }

    // This entire loop will eventually terminate when we cannot
    // improve properties nor return types.
  }

  auto const UNUSED bump =
    trace_bump(ctx, Trace::hhbbc, Trace::hhbbc_cfg, Trace::hhbbc_index);

  // For debugging, print the final state of the class analysis.
  FTRACE(2, "{}", [&] {
    auto const bsep = std::string(60, '+') + "\n";
    auto ret = fmt::format(
      "{}class {}:\n{}",
      bsep,
      ctx.cls->name->data(),
      bsep
    );
    for (auto& kv : clsAnalysis.privateProperties) {
      ret += fmt::format(
        "private ${: <14} :: {}\n",
        kv.first->data(),
        show(kv.second.ty)
      );
    }
    for (auto& kv : clsAnalysis.privateStatics) {
      ret += fmt::format(
        "private static ${: <14} :: {}\n",
        kv.first->data(),
        show(kv.second.ty)
      );
    }
    ret += bsep;
    return ret;
  }());

  clsAnalysis.work = nullptr;
  return clsAnalysis;
}

//////////////////////////////////////////////////////////////////////

PropagatedStates::PropagatedStates(State&& state, StateMutationUndo undos)
  : m_locals{std::move(state.locals)}
  , m_undos{std::move(undos)}
{
  for (size_t i = 0; i < state.stack.size(); ++i) {
    m_stack.emplace_back(std::move(state.stack[i].type));
  }
}

void PropagatedStates::next() {
  // The undo log shouldn't be empty, and we should be at a mark
  // (which marks instruction boundary).
  assertx(!m_undos.events.empty());
  assertx(std::get_if<StateMutationUndo::Mark>(&m_undos.events.back()));

  m_lastPush.reset();
  m_lastSet.reset();
  m_afterLocals.clear();
  m_undos.events.pop_back();

  // Use the undo log to "unwind" the current state.
  while (true) {
    assertx(!m_undos.events.empty());
    auto const stop = match<bool>(
      m_undos.events.back(),
      [] (const StateMutationUndo::Mark&) { return true; },
      [this] (StateMutationUndo::Push) {
        assertx(!m_stack.empty());
        if (!m_lastPush) m_lastPush.emplace(std::move(m_stack.back()));
        m_stack.pop_back();
        return false;
      },
      [this] (StateMutationUndo::Pop& p) {
        m_stack.emplace_back(std::move(p.t));
        return false;
      },
      [this] (StateMutationUndo::Stack& s) {
        assertx(s.idx < m_stack.size());
        m_stack[s.idx] = std::move(s.t);
        return false;
      },
      [this] (StateMutationUndo::Local& l) {
        assertx(l.id < m_locals.size());
        auto& old = m_locals[l.id];
        m_afterLocals.emplace_back(std::make_pair(l.id, std::move(old)));
        old = std::move(l.t);
        return false;
      },
      [this] (StateMutationUndo::UndoableArraySet& s) {
        // Instead of holding onto a reference to the pre-set array
        // (which can cause COW to trigger), store enough information
        // to undo the set. This can only be done under certain
        // circumstances. If not applicable, a normal stack-write undo
        // is issued instead.
        assertx(!m_stack.empty());
        // Store information about the last undoable array set without
        // referencing the array itself, to try to keep array
        // ref-count at 1.
        if (!m_lastSet) m_lastSet.emplace(s);
        auto arr = std::move(m_stack.back());
        m_stack.back() = undo_array_like_set(std::move(arr), s.key);
        return false;
      }
    );
    if (stop) break;
    m_undos.events.pop_back();
  }
}

PropagatedStates
locally_propagated_states(const IIndex& index,
                          const AnalysisContext& ctx,
                          CollectedInfo& collect,
                          BlockId bid,
                          State state) {
  Trace::Bump bumper{Trace::hhbbc, 10};

  auto const blk = ctx.func.blocks()[bid].get();

  // Set up the undo log for the interp. We reserve it using this size
  // heuristic which captures typical undo log sizes.
  StateMutationUndo undos;
  undos.events.reserve((blk->hhbcs.size() + 1) * 4);

  auto interp = Interp { index, ctx, collect, bid, blk, state, &undos };

  for (auto const& op : blk->hhbcs) {
    auto const markIdx = undos.events.size();
    // Record instruction boundary
    undos.events.emplace_back(StateMutationUndo::Mark{});
    // Interpret it. This appends more info to the undo log.
    auto const stepFlags = step(interp, op);
    // Store the step flags in the mark we recorded before the
    // interpret.
    auto& mark = std::get<StateMutationUndo::Mark>(undos.events[markIdx]);
    mark.wasPEI = stepFlags.wasPEI;
    mark.mayReadLocalSet = stepFlags.mayReadLocalSet;
    mark.unreachable = state.unreachable;
    state.stack.compact();
  }
  // Add a final mark to maintain invariants (this will be popped
  // immediately when the first next() is called).
  undos.events.emplace_back(StateMutationUndo::Mark{});
  return PropagatedStates{std::move(state), std::move(undos)};
}

State locally_propagated_bid_state(const IIndex& index,
                                   const AnalysisContext& ctx,
                                   CollectedInfo& collect,
                                   BlockId bid,
                                   State state,
                                   BlockId targetBid) {
  Trace::Bump bumper{Trace::hhbbc, 10};
  if (!state.initialized) return {};

  auto const originalState = state;
  auto const blk = ctx.func.blocks()[bid].get();
  auto interp = Interp { index, ctx, collect, bid, blk, state };

  State ret{};
  auto const propagate = [&] (BlockId target, const State* st) {
    if (target == targetBid) merge_into(ret, *st);
  };
  run(interp, originalState, propagate, []{});

  ret.stack.compact();
  return ret;
}

//////////////////////////////////////////////////////////////////////

UnitAnalysis analyze_unit(const AnalysisIndex& index, const Context& ctx) {
  assertx(ctx.unit);
  assertx(!ctx.cls);
  assertx(!ctx.func);

  auto const UNUSED bump =
    trace_bump(ctx, Trace::hhbbc, Trace::hhbbc_cfg, Trace::hhbbc_index);

  FTRACE(
    2, "{:#^70}\n-- {}\n",
    index.frozen() ? " Unit (Frozen) " : " Unit ",
    ctx.unit
  );

  AnalysisIndexAdaptor adaptor{ index };
  ContextPusher _{adaptor, ctx};

  UnitAnalysis analysis{ctx};

  InTypeCns _2{adaptor};

  auto const& unit = index.lookup_unit(ctx.unit);
  for (size_t i = 0, size = unit.typeAliases.size(); i < size; ++i) {
    auto const& ta = *unit.typeAliases[i];
    if (ta.resolvedTypeStructure) continue;

    SCOPE_ASSERT_DETAIL("resolve-type-alias") {
      return fmt::format("Resolving {} {}", show(ctx), ta.name->data());
    };
    FTRACE(2, "{:-^70}\n-- ({}) {}\n", "Resolve", show(ctx), ta.name);

    auto const resolution =
      resolve_type_structure(AnalysisIndexAdaptor { index }, nullptr, ta);
    auto const ts = resolution.sarray();
    assertx(!resolution.contextSensitive);
    if (!ts) {
      FTRACE(2, "  no resolution\n");
      continue;
    }

    FTRACE(2, "  resolved to {}\n", staticArrayStreamer(ts));
    analysis.resolvedTypeAliases.emplace_back(ResolvedTypeAlias{i, ts});
  }

  return analysis;
}

//////////////////////////////////////////////////////////////////////

namespace {

template <typename Resolve, typename Self>
ConstraintType type_from_constraint_impl(const TypeConstraint& tc,
                                         const Type& candidate,
                                         const Resolve& resolve,
                                         const Self& self) {
  using C = ConstraintType;

  assertx(IMPLIES(!tc.isCheckable(), tc.isMixed()));

  auto ty = [&] {
    auto const exact = [&] (const Type& t) {
      return C{ t, t };
    };

    if (tc.isUnion()) {
      auto range = eachTypeConstraintInUnion(tc);
      auto it = range.begin();
      auto c = type_from_constraint_impl(*it, candidate, resolve, self);
      ++it;

      for(; it != range.end(); ++it) {
        auto c2 = type_from_constraint_impl(*it, candidate, resolve, self);
        c = union_constraint(c, c2);
      }
      return c;
    }

    switch (getAnnotMetaType(tc.type())) {
      case AnnotMetaType::Precise: {
        switch (getAnnotDataType(tc.type())) {
          case KindOfNull:         return exact(TInitNull);
          case KindOfBoolean:      return exact(TBool);
          case KindOfInt64:        return exact(TInt);
          case KindOfDouble:       return exact(TDbl);
          case KindOfVec:          return exact(TVec);
          case KindOfDict:         return exact(TDict);
          case KindOfKeyset:       return exact(TKeyset);
          case KindOfResource:     return exact(TRes);
          case KindOfClsMeth:      return exact(TClsMeth);
          case KindOfEnumClassLabel: return exact(TEnumClassLabel);
          case KindOfObject:       return exact(TObj);
          case KindOfString:
            return C{
              TStr,
              union_of(TStr, TCls, TLazyCls),
              TriBool::Yes
            };
          case KindOfUninit:
          case KindOfRFunc:
          case KindOfFunc:
          case KindOfRClsMeth:
          case KindOfClass:
          case KindOfLazyClass:    break;
        }
        always_assert(false);
      }
      case AnnotMetaType::Mixed:
        return C{ TInitCell, TInitCell, TriBool::No, true };
      case AnnotMetaType::Nothing:
      case AnnotMetaType::NoReturn:   return exact(TBottom);
      case AnnotMetaType::Nonnull:    return exact(TNonNull);
      case AnnotMetaType::Number:     return exact(TNum);
      case AnnotMetaType::VecOrDict:  return exact(TKVish);
      case AnnotMetaType::ArrayLike:  return exact(TArrLike);
      case AnnotMetaType::SubObject: {
        auto const cls = resolve(tc.clsName());
        auto lower = cls ? subObj(*cls) : TBottom;
        auto upper = lower;

        // The "magic" interfaces cannot be represented with a single
        // type. Obj=Foo|Str, for example, is not a valid type. It is
        // safe to only provide a subset as the lower bound. We can
        // use any provided candidate type to refine the lower bound
        // and supply the subset which would allow us to optimize away
        // the check.
        if (interface_supports_arrlike(tc.clsName())) {
          if (candidate.subtypeOf(BArrLike)) lower = TArrLike;
          upper |= TArrLike;
        }
        if (interface_supports_int(tc.clsName())) {
          if (candidate.subtypeOf(BInt)) lower = TInt;
          upper |= TInt;
        }
        if (interface_supports_double(tc.clsName())) {
          if (candidate.subtypeOf(BDbl)) lower = TDbl;
          upper |= TDbl;
        }

        if (interface_supports_string(tc.clsName())) {
          if (candidate.subtypeOf(BStr)) lower = TStr;
          upper |= union_of(TStr, TCls, TLazyCls);
          return C{
            std::move(lower),
            std::move(upper),
            TriBool::Yes
          };
        }

        return C{ std::move(lower), std::move(upper) };
      }
      case AnnotMetaType::Unresolved:
        return C{ TBottom, TBottom };
      case AnnotMetaType::This:
        if (auto const s = self()) {
          auto const obj = toobj(*s);
          if (!is_specialized_cls(*s) || dcls_of(*s).cls().couldBeMocked()) {
            return C { setctx(obj), obj };
          }
          return exact(setctx(obj));
        }
        return C{ TBottom, TObj };
      case AnnotMetaType::Callable:
        return C{
          TBottom,
          union_of(TStr, TVec, TDict, TFunc, TRFunc, TObj, TClsMeth, TRClsMeth)
        };
      case AnnotMetaType::ArrayKey:
        return C{
          TArrKey,
          union_of(TArrKey, TCls, TLazyCls),
          TriBool::Yes
        };
      // NB: when we're logging, only string has no side effects going through
      // classname<T>, and only class pointers have no side effects going
      // through class<T>
      case AnnotMetaType::Classname:
        if (!Cfg::Eval::ClassPassesClassname) {
          return exact(TStr);
        }
        return C{
          TStr,
          union_of(TStr, TCls, TLazyCls)
        };
      case AnnotMetaType::Class:
        // TODO(T199611023) do similar checks to SubObject for inner class type
        if (Cfg::Eval::ClassTypeLevel > 0) {
          return exact(union_of(TCls, TLazyCls));
        }
        return C{
          union_of(TCls, TLazyCls),
          union_of(TStr, TCls, TLazyCls)
        };
      case AnnotMetaType::ClassOrClassname:
        return C{
          union_of(TStr, TCls, TLazyCls),
          union_of(TStr, TCls, TLazyCls)
        };
    }

    always_assert(false);
  }();

  // Nullable constraint always includes TInitNull.
  if (tc.isNullable()) {
    ty.lower = opt(std::move(ty.lower));
    ty.upper = opt(std::move(ty.upper));
  }
  // Soft type-constraints can potentially allow anything, so its
  // upper-bound is maximal. We might still be able to optimize away
  // the check if the lower bound is satisfied.
  if (tc.isSoft()) {
    ty.upper = TInitCell;
    ty.maybeMixed = true;
  }
  return ty;
}

}

ConstraintType type_from_constraint(
  const TypeConstraint& tc,
  const Type& candidate,
  const std::function<Optional<res::Class>(SString)>& resolve,
  const std::function<Optional<Type>()>& self)
{
  return type_from_constraint_impl(tc, candidate, resolve, self);
}

ConstraintType
lookup_constraint(const IIndex& index,
                  const Context& ctx,
                  const TypeConstraint& tc,
                  const Type& candidate) {
  return type_from_constraint_impl(
    tc, candidate,
    [&] (SString name) { return index.resolve_class(name); },
    [&] { return selfCls(index, ctx); }
  );
}

std::tuple<Type, bool, bool> verify_param_type(const IIndex& index,
                                               const Context& ctx,
                                               uint32_t paramId,
                                               const Type& t) {
  // Builtins verify parameter types differently.
  if (ctx.func->isNative) return { t, true, true };

  assertx(paramId < ctx.func->params.size());
  auto const& pinfo = ctx.func->params[paramId];

  // Inputs of out-only parameters are not enforced.
  if (pinfo.outOnly) return { t, true, true };

  auto refined = TInitCell;
  auto noop = true;
  auto effectFree = true;
  for (auto const& tc : pinfo.typeConstraints.range()) {
    auto const lookup = lookup_constraint(index, ctx, tc, t);
    if (t.moreRefined(lookup.lower)) {
      refined = intersection_of(std::move(refined), t);
      continue;
    }

    if (!t.couldBe(lookup.upper)) return { TBottom, false, false };

    noop = false;

    auto result = intersection_of(t, lookup.upper);
    if (lookup.coerceClassToString == TriBool::Yes) {
      assertx(!lookup.lower.couldBe(BCls | BLazyCls));
      assertx(lookup.upper.couldBe(BStr | BCls | BLazyCls));
      if (result.couldBe(BCls | BLazyCls)) {
        result = promote_classish(std::move(result));
        if (effectFree && (Cfg::Eval::ClassStringHintNoticesSampleRate > 0 ||
                           !promote_classish(t).moreRefined(lookup.lower))) {
          effectFree = false;
        }
      } else {
        effectFree = false;
      }
    } else if (lookup.coerceClassToString == TriBool::Maybe) {
      if (result.couldBe(BCls | BLazyCls)) result |= TSStr;
      effectFree = false;
    } else {
      effectFree = false;
    }

    refined = intersection_of(std::move(refined), std::move(result));
    if (refined.is(BBottom)) return { TBottom, false, false };
  }

  return { std::move(refined), noop, effectFree };
}

Type adjust_type_for_prop(const IIndex& index,
                          const php::Class& propCls,
                          const TypeIntersectionConstraint* tcs,
                          const Type& ty) {
  if (!tcs) return ty;
  auto result = ty;
  for (auto const& tc : tcs->range()) {
    result = adjust_type_for_prop(index, propCls, tc, result);
    if (result.is(BBottom)) return result;
  }
  return result;
}

Type adjust_type_for_prop(const IIndex& index,
                          const php::Class& propCls,
                          const TypeConstraint& tc,
                          const Type& ty) {
  assertx(tc.validForProp());
  if (Cfg::Eval::CheckPropTypeHints <= 2) return ty;
  auto lookup = lookup_constraint(
    index,
    Context { nullptr, nullptr, &propCls },
    tc,
    ty
  );
  auto upper = unctx(lookup.upper);
  // A property with a mixed type-hint can be unset and therefore by
  // Uninit. Any other type-hint forbids unsetting.
  if (lookup.maybeMixed) upper |= TUninit;
  auto ret = intersection_of(std::move(upper), ty);
  if (lookup.coerceClassToString == TriBool::Yes) {
    assertx(!lookup.lower.couldBe(BCls | BLazyCls));
    assertx(lookup.upper.couldBe(BStr | BCls | BLazyCls));
    ret = promote_classish(std::move(ret));
  } else if (lookup.coerceClassToString == TriBool::Maybe) {
    if (ret.couldBe(BCls | BLazyCls)) ret |= TSStr;
  }
  return ret;
}

ConstraintType union_constraint(const ConstraintType& a,
                                const ConstraintType& b) {
  return ConstraintType {
    intersection_of(a.lower, b.lower),
    union_of(a.upper, b.upper),
    a.coerceClassToString | b.coerceClassToString,
    a.maybeMixed || b.maybeMixed,
  };
}

//////////////////////////////////////////////////////////////////////

Optional<Type> selfCls(const IIndex& index, const Context& ctx) {
  if (!ctx.cls || is_used_trait(*ctx.cls)) {
    return std::nullopt;
  }
  if (auto const c = index.resolve_class(ctx.cls->name)) {
    return subCls(*c, true);
  }
  return std::nullopt;
}

Optional<Type> selfClsExact(const IIndex& index, const Context& ctx) {
  if (!ctx.cls || is_used_trait(*ctx.cls)) {
    return std::nullopt;
  }
  if (auto const c = index.resolve_class(ctx.cls->name)) {
    return clsExact(*c, true);
  }
  return std::nullopt;
}

Optional<Type> parentCls(const IIndex& index, const Context& ctx) {
  if (!ctx.cls || is_used_trait(*ctx.cls) || !ctx.cls->parentName) {
    return std::nullopt;
  }
  if (auto const c = index.resolve_class(ctx.cls->parentName)) {
    return subCls(*c, true);
  }
  return std::nullopt;
}

Optional<Type> parentClsExact(const IIndex& index, const Context& ctx) {
  if (!ctx.cls || is_used_trait(*ctx.cls) || !ctx.cls->parentName) {
    return std::nullopt;
  }
  if (auto const c = index.resolve_class(ctx.cls->parentName)) {
    return clsExact(*c, true);
  }
  return std::nullopt;
}

//////////////////////////////////////////////////////////////////////

res::Class builtin_class(const IIndex& index, SString name) {
  // A builtin class may not necessarily be resolved, but it must
  // exist, and if it is resolved, must be AttrBuiltin.
  auto const rcls = index.resolve_class(name);
  always_assert_flog(
    rcls.has_value(),
    "A builtin class ({}) does not exist",
    name
  );
  always_assert_flog(
    !rcls->cls() || rcls->cls()->attrs & AttrBuiltin,
    "A builtin class ({}) resolved to non-builtin",
    name
  );
  return *rcls;
}

Type return_type_from_constraints(
  const php::Func& f,
  const std::function<Optional<res::Class>(SString)>& resolve,
  const std::function<Optional<Type>()>& self
) {
  if (f.isGenerator) {
    if (f.isAsync) {
      // Async generators always return AsyncGenerator object.
      return objExact(res::Class::get(s_AsyncGenerator.get()));
    }
    // Non-async generators always return Generator object.
    return objExact(res::Class::get(s_Generator.get()));
  }

  auto const make_type = [&] (const TypeConstraint& tc) {
    auto lookup = type_from_constraint(tc, TInitCell, resolve, self);
    if (lookup.coerceClassToString == TriBool::Yes) {
      lookup.upper = promote_classish(std::move(lookup.upper));
    } else if (lookup.coerceClassToString == TriBool::Maybe) {
      lookup.upper |= TSStr;
    }

    if (f.isNative &&
      lookup.upper.subtypeOf(BStr | BObj | BRes | BArrLike)
    ) {
      // TODO: T220184756 Remove implicit assumption that non-simple
      // types (ones that are represented by pointers) can always
      // possibly be null
      lookup.upper = opt(std::move(lookup.upper));
    }
    return unctx(std::move(lookup.upper));
  };

  auto const process = [&] (const TypeIntersectionConstraint& tcs) {
    auto ret = TInitCell;
    for (auto const& tc : tcs.range()) {
      ret = intersection_of(std::move(ret), make_type(tc));
    }
    return ret;
  };

  auto ret = process(f.retTypeConstraints);
  if (f.hasInOutArgs && !ret.is(BBottom)) {
    std::vector<Type> types;
    types.reserve(f.params.size() + 1);
    types.emplace_back(std::move(ret));
    for (auto const& p : f.params) {
      if (!p.inout) continue;
      auto t = process(p.typeConstraints);
      if (t.is(BBottom)) return TBottom;
      types.emplace_back(std::move(t));
    }
    std::reverse(begin(types)+1, end(types));
    ret = vec(std::move(types));
  }

  if (f.isAsync) {
    // Async functions always return WaitH<T>, where T is the type
    // returned internally.
    return wait_handle(std::move(ret));
  }
  return ret;
}

//////////////////////////////////////////////////////////////////////

}
