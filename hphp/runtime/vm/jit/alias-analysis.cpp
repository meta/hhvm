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
#include <fmt/format.h>

#include "hphp/runtime/vm/jit/alias-analysis.h"

#include <utility>
#include <sstream>

#include "hphp/util/bitset-utils.h"
#include "hphp/util/match.h"
#include "hphp/runtime/base/perf-warning.h"
#include "hphp/runtime/vm/jit/ir-unit.h"
#include "hphp/runtime/vm/jit/memory-effects.h"
#include "hphp/runtime/vm/jit/alias-class.h"

namespace HPHP::jit {

TRACE_SET_MOD(hhir_alias)

namespace {

//////////////////////////////////////////////////////////////////////

// Locations that refer to ranges of the eval stack or multiple frame locals
// are expanded into individual locations only if smaller than this threshold.
constexpr int kMaxExpandedSize = 16;

template<class Visit>
void visit_locations(const BlockList& blocks, Visit visit) {
  for (auto& blk : blocks) {
    FTRACE(1, "B{}:\n", blk->id());
    for (auto& inst : *blk) {
      auto const effects = canonicalize(memory_effects(inst));
      FTRACE(1, "  {: <30} -- {}\n", show(effects), inst.toString());
      match(
        effects,
        [&] (const IrrelevantEffects&)   {},
        [&] (const UnknownEffects&)      {},
        [&] (const ReturnEffects& x)     { visit(x.kills); },
        [&] (const CallEffects& x) {
          visit(x.kills);
          visit(x.uninits);
          visit(x.inputs);
          visit(x.actrec);
          visit(x.outputs);
          for (auto const& frame : x.backtrace) {
            visit(frame);
          }
        },
        [&] (const GeneralEffects& x) {
          visit(x.loads);
          visit(x.stores);
          visit(x.inout);
          visit(x.moves);
          visit(x.kills);
          for (auto const& frame : x.backtrace) {
            visit(frame);
          }
        },
        [&] (const PureLoad& x)          { visit(x.src); },
        [&] (const PureStore& x)         { visit(x.dst); },
        [&] (const ExitEffects& x)       { visit(x.live);
                                           visit(x.kills);
                                           visit(x.uninits); },
        [&] (const PureInlineCall& x)    { visit(x.base);
                                           visit(x.actrec); }
      );
    }
  }
}

Optional<uint32_t> add_class(AliasAnalysis& ret, AliasClass acls) {
  assertx(acls.isSingleLocation());
  auto const ins = ret.locations.insert(std::make_pair(acls, ALocMeta{}));
  if (!ins.second) return ins.first->second.index;
  if (ret.locations.size() > kMaxTrackedALocs) {
    always_assert(ret.locations.size() == kMaxTrackedALocs + 1);
    ret.locations.erase(acls);
    return std::nullopt;
  }
  FTRACE(1, "    new: {}\n", show(acls));
  auto& meta = ins.first->second;
  meta.index = ret.locations.size() - 1;
  always_assert(meta.index < kMaxTrackedALocs);
  return meta.index;
}

// Expand a location into a set of locations that may alias it. This
// is for locals where the location may contain a discrete set of
// locals.
template<class T>
ALocBits may_alias_component(const AliasAnalysis& aa,
                             AliasClass acls,
                             Optional<T> proj,
                             const AliasAnalysis::LocationMap& sets,
                             AliasClass any,
                             ALocBits pessimistic) {
  if (proj) {
    auto ret = ALocBits{};
    if (proj->ids.hasSingleValue()) {
      if (auto const slot = aa.find(*proj)) {
        ret.set(slot->index);
      }
      // Otherwise the location is untracked, and cannot interfere with any
      // tracked location.
    } else {
      auto const it = sets.find(*proj);
      if (it != end(sets)) {
        ret |= it->second;
      } else {
        ret |= pessimistic;
      }
    }
    return ret;
  }
  return acls.maybe(any) ? pessimistic : ALocBits{};
}

template<class T>
ALocBits may_alias_part(const AliasAnalysis& aa,
                        AliasClass acls,
                        Optional<T> proj,
                        AliasClass any,
                        ALocBits pessimistic) {
  if (proj) {
    if (auto meta = aa.find(*proj)) {
      return ALocBits{meta->conflicts}.set(meta->index);
    }
    assertx(acls.maybe(any));
    return pessimistic;
  }
  return acls.maybe(any) ? pessimistic : ALocBits{};
}

// Expand a location into a set of locations which definitely contain
// it. This is for locals where the location may contain a discrete
// set of locals.
template<class T>
ALocBits expand_component(const AliasAnalysis& aa,
                          AliasClass acls,
                          Optional<T> loc,
                          const AliasAnalysis::LocationMap& sets,
                          AliasClass any,
                          ALocBits all) {
  if (loc) {
    auto ret = ALocBits{};
    if (loc->ids.hasSingleValue()) {
      if (auto const meta = aa.find(*loc)) {
        ret.set(meta->index);
      }
    } else {
      auto const it = sets.find(*loc);
      if (it != end(sets)) {
        ret |= it->second;
      }
      // We could iterate over everything and set corresponding bits, but that
      // seldom adds value.
    }
    return ret;
  }
  return any <= acls ? all : ALocBits{};
}

template<class T>
ALocBits expand_part(const AliasAnalysis& aa,
                     AliasClass acls,
                     Optional<T> proj,
                     AliasClass any,
                     ALocBits all) {
  auto ret = ALocBits{};
  if (proj) {
    if (auto const meta = aa.find(*proj)) {
      return ret.set(meta->index);      // A single tracked location.
    }
    assertx(acls.maybe(any));
    return ret;
  }
  return any <= acls ? all : ret;
}

template<class T>
bool collect_component(AliasAnalysis& aa,
                       Optional<T> loc,
                       AliasAnalysis::LocationMap& map) {
  if (!loc) return false;

  assertx(!loc->ids.empty());
  if (loc->ids.hasSingleValue()) {
    add_class(aa, *loc);
  } else {
    auto const inserted = map.insert({*loc, {}}).second;
    if (!inserted && loc->ids.size() <= kMaxExpandedSize) {
      for (uint32_t id = 0; id < AliasIdSet::BitsetMax; ++id) {
        if (!loc->ids.test(id)) continue;
        add_class(aa, T { loc->frameIdx, id });
      }
    }
  }
  return true;
}

bool collect_component(AliasAnalysis& aa,
                       Optional<AStack> stk,
                       AliasAnalysis::LocationMap& map) {
  if (!stk) return false;

  if (stk->size() == 1) {
    add_class(aa, *stk);
  } else {
    auto const inserted = map.insert({*stk, {}}).second;
    if (!inserted && stk->size() <= kMaxExpandedSize) {
      for (auto stkidx = stk->low; stkidx < stk->high; ++stkidx) {
        add_class(aa, AStack::at(stkidx));
      }
    }
  }
  return true;
}

//////////////////////////////////////////////////////////////////////

}

AliasAnalysis::AliasAnalysis(const IRUnit& /*unit*/) {}

Optional<ALocMeta> AliasAnalysis::find(AliasClass acls) const {
  auto const it = locations.find(acls);
  if (it == end(locations)) return std::nullopt;
  return it->second;
}

#define SIMPLE_ALIAS_CLASSES(X)       \
  X(Rds, rds, all_rds)                \
  X(FContext, fcontext, all_fcontext) \
  X(FFunc, ffunc, all_ffunc)          \
  X(FMeta, fmeta, all_fmeta)          \
/**/

#define ALIAS_CLASSES(X)      \
  X(ClosureArg, closureArg, all_closureArg) \
  X(Prop, prop, all_props)    \
  X(ElemI, elemI, all_elemIs) \
  X(ElemS, elemS, all_elemSs) \
  SIMPLE_ALIAS_CLASSES(X)     \
/**/

ALocBits AliasAnalysis::may_alias(AliasClass acls) const {
  if (auto meta = find(acls)) {
    return ALocBits{meta->conflicts}.set(meta->index);
  }

  auto ret = ALocBits{};

  // Handle stacks specially to be less pessimistic.  We can always use the
  // expand map to find stack locations that may alias our class.
  auto const stk = acls.stack();
  if (stk && stk->size() > 1) {
    auto const it = stk_expand_map.find(*stk);
    ret |= it != end(stk_expand_map) ? it->second : all_stack;
  } else {
    ret |= may_alias_part(*this, acls, acls.stack(), AStackAny, all_stack);
  }

  ret |= may_alias_component(*this, acls, acls.local(), loc_expand_map,
                             ALocalAny, all_local);
  ret |= may_alias_component(*this, acls, acls.iter(), iter_expand_map,
                             AIterAny, all_iter);

  auto const add_single = [&] (auto single, AliasClass cls) {
    assertx(cls.isSingleLocation());
    if (cls <= *single) {
      if (auto meta = find(cls)) {
        ret |= ALocBits{meta->conflicts}.set(meta->index);
      }
      // The location is untracked.
    }
  };

  if (auto const mis = acls.mis()) {
    add_single(mis, AMIStateTempBase);
    add_single(mis, AMIStateBase);
    add_single(mis, AMIStateROProp);
  }

  if (auto const fbase = acls.frame_base()) {
    add_single(fbase, AFBasePtr);
  }

  if (auto const vm_reg_state = acls.vm_reg_state()) {
    add_single(vm_reg_state, AVMRegState);
  }

  if (auto const vm_reg = acls.vm_reg()) {
    add_single(vm_reg, AVMFP);
    add_single(vm_reg, AVMSP);
    add_single(vm_reg, AVMPC);
    add_single(vm_reg, AVMRetAddr);
  }

#define MAY_ALIAS(What, what, all) \
  ret |= may_alias_part(*this, acls, acls.what(), A##What##Any, all);

  ALIAS_CLASSES(MAY_ALIAS);

#undef MAY_ALIAS

  return ret;
}

ALocBits AliasAnalysis::expand(AliasClass acls) const {
  if (auto const info = find(acls)) return ALocBits{}.set(info->index);

  auto ret = ALocBits{};

  // We want to handle stacks partially specially, because they can be expanded
  // in some situations even if they don't have an ALocMeta.
  if (auto const stk = acls.stack()) {
    auto const it = stk->size() > 1 ? stk_expand_map.find(*stk)
                                    : end(stk_expand_map);
    if (it != end(stk_expand_map)) {
      ret |= it->second;
    } else {
      ret |= expand_part(*this, acls, stk, AStackAny, all_stack);
    }
  } else if (AStackAny <= acls) {
    ret |= all_stack;
  }

  ret |= expand_component(*this, acls, acls.local(), loc_expand_map,
                          ALocalAny, all_local);
  ret |= expand_component(*this, acls, acls.iter(), iter_expand_map,
                          AIterAny, all_iter);

  auto const add_single = [&] (auto single, AliasClass cls) {
    assertx(cls.isSingleLocation());
    if (cls <= *single) {
      if (auto const meta = find(cls)) {
        ret.set(meta->index);
      }
    }
  };

  if (auto const mis = acls.mis()) {
    auto const add_mis = [&] (AliasClass cls) { add_single(mis, cls); };
    add_mis(AMIStateTempBase);
    add_mis(AMIStateBase);
    add_mis(AMIStateROProp);
  }

  if (auto const fbase = acls.frame_base()) {
    add_single(fbase, AFBasePtr);
  }

  if (auto const vm_reg_state = acls.vm_reg_state()) {
    add_single(vm_reg_state, AVMRegState);
  }

  if (auto const vm_reg = acls.vm_reg()) {
    add_single(vm_reg, AVMFP);
    add_single(vm_reg, AVMSP);
    add_single(vm_reg, AVMPC);
    add_single(vm_reg, AVMRetAddr);
  }

#define EXPAND(What, what, all) \
  ret |= expand_part(*this, acls, acls.what(), A##What##Any, all);

  ALIAS_CLASSES(EXPAND);

#undef EXPAND

  return ret;
}

AliasAnalysis collect_aliases(const IRUnit& unit, const BlockList& blocks) {
  FTRACE(1, "collect_aliases:vvvvvvvvvvvvvvvvvvvv\n");
  SCOPE_EXIT { FTRACE(1, "collect_aliases:^^^^^^^^^^^^^^^^^^^^\n"); };

  auto ret = AliasAnalysis{unit};

  /*
   * Right now we compute the conflict sets for object properties based only on
   * object property offsets, and for arrays based only on index.  Everything
   * colliding in that regard is assumed to possibly alias.
   */
  
  auto conflict_closure_arg_offset = jit::fast_map<uint32_t,ALocBits>{};
  auto conflict_prop_offset = jit::fast_map<uint32_t,ALocBits>{};
  auto conflict_array_index = jit::fast_map<int64_t,ALocBits>{};
  auto conflict_array_key = jit::fast_map<const StringData*,ALocBits>{};
  auto prop_array_map = jit::fast_map<int64_t,AliasClass>{};

  visit_locations(blocks, [&] (AliasClass acls) {
    if (auto const closure_arg = acls.is_closureArg()) {
      if (auto const index = add_class(ret, acls)) {
        conflict_closure_arg_offset[closure_arg->offset].set(*index);
        prop_array_map.emplace(*index, acls);
      }
      return;
    }

    if (auto const prop = acls.is_prop()) {
      if (auto const index = add_class(ret, acls)) {
        conflict_prop_offset[prop->offset].set(*index);
        prop_array_map.emplace(*index, acls);
      }
      return;
    }

    if (auto const elemI = acls.is_elemI()) {
      if (auto const index = add_class(ret, acls)) {
        conflict_array_index[elemI->idx].set(*index);
        prop_array_map.emplace(*index, acls);
      }
      return;
    }

    if (auto const elemS = acls.is_elemS()) {
      if (auto const index = add_class(ret, acls)) {
        conflict_array_key[elemS->key].set(*index);
        prop_array_map.emplace(*index, acls);
      }
      return;
    }

    if (acls.is_rds()) {
      add_class(ret, acls);
      return;
    }

    if (acls.is_mis() && acls.isSingleLocation()) {
      add_class(ret, acls);
      return;
    }

    if (auto const ar = acls.actrec()) {
      auto const idx = ar->frameIdx;
      if (acls.maybe(AFContext { idx })) add_class(ret, AFContext { idx });
      if (acls.maybe(AFFunc { idx }))    add_class(ret, AFFunc { idx });
      if (acls.maybe(AFMeta { idx }))    add_class(ret, AFMeta { idx });
      // Fallthrough here: it's possible to share these specializations with
      // ALocal and AIter specializations.
    }

    if (acls.is_frame_base() && acls.isSingleLocation()) {
      add_class(ret, acls);
      return;
    }

    if (acls.is_vm_reg_state() && acls.isSingleLocation()) {
      add_class(ret, acls);
      return;
    }

    if (auto const vm_reg = acls.vm_reg()) {
      if (acls.maybe(AVMFP))      add_class(ret, AVMFP);
      if (acls.maybe(AVMSP))      add_class(ret, AVMSP);
      if (acls.maybe(AVMPC))      add_class(ret, AVMPC);
      if (acls.maybe(AVMRetAddr)) add_class(ret, AVMRetAddr);
    }

    // Note that we're using iter() instead of is_iter(), etc. here. That's
    // because we often union these types with other, unspecialized AliasClass
    // types for pessimistic operations (e.g. a may-throw IR op will typically
    // read (specialized) 86metadata locals, plus (unspecialized) heap data).
    if (collect_component(ret, acls.iter(), ret.iter_expand_map)) return;
    if (collect_component(ret, acls.local(), ret.loc_expand_map)) return;
    if (collect_component(ret, acls.stack(), ret.stk_expand_map)) return;
  });

  always_assert(ret.locations.size() <= kMaxTrackedALocs);
  if (ret.locations.size() == kMaxTrackedALocs) {
    logLowPriPerfWarning(
      "alias-analysis kMaxTrackedALocs",
      25000,
      [&](StructuredLogEntry& cols) {
        auto const func = unit.context().initSrcKey.func();
        cols.setStr("func", func->fullName()->slice());
        cols.setStr("filename", func->unit()->origFilepath()->slice());
        cols.setStr("hhir_unit", show(unit));
      }
    );
    FTRACE(1, "max locations limit was reached\n");
  }

  auto const maybe_set_conflicts =
    [&] (ALocBits conflicts, ALocMeta& meta, const AliasClass& acls) {
      bitset_for_each_set(
        conflicts,
        [&] (size_t i) {
          auto const other_acls = folly::get_ptr(prop_array_map, i);
          assertx(other_acls);
          if (i != meta.index && acls.maybe(*other_acls)) {
            meta.conflicts.set(i);
          }
        }
      );
    };

  auto make_conflict_set = [&] (AliasClass acls, ALocMeta& meta) {
    if (auto const closure_arg = acls.is_closureArg()) {
      maybe_set_conflicts(conflict_closure_arg_offset[closure_arg->offset], meta, acls);
      ret.all_closureArg.set(meta.index);
      return;
    }
    if (auto const prop = acls.is_prop()) {
      maybe_set_conflicts(conflict_prop_offset[prop->offset], meta, acls);
      ret.all_props.set(meta.index);
      return;
    }

    if (auto const elemI = acls.is_elemI()) {
      maybe_set_conflicts(conflict_array_index[elemI->idx], meta, acls);
      ret.all_elemIs.set(meta.index);
      return;
    }

    if (auto const elemS = acls.is_elemS()) {
      maybe_set_conflicts(conflict_array_key[elemS->key], meta, acls);
      ret.all_elemSs.set(meta.index);
      return;
    }

    if (auto const frame = acls.is_local()) {
      ret.all_local.set(meta.index);
      return;
    }

    if (acls.is_stack()) {
      ret.all_stack.set(meta.index);
      return;
    }

    if (acls.is_iter()) {
      ret.all_iter.set(meta.index);
      return;
    }

#define SET(What, what, all) \
  if (acls.is_##what()) {    \
    ret.all.set(meta.index); \
    return;                  \
  }

    SIMPLE_ALIAS_CLASSES(SET)

#undef SET

    if (acls.is_mis()) {
      // We don't maintain an all_mistate set so there's nothing to do here but
      // avoid hitting the assert below.
      return;
    }

    if (acls.is_frame_base()) {
      // There's a single frame base register and it cannot conflict with any
      // other locations.
      return;
    }

    if (acls.is_vm_reg_state()) {
      return;
    }

    if (acls.is_vm_reg()) {
      return;
    }

    always_assert_flog(0, "AliasAnalysis assigned an AliasClass an id "
      "but it didn't match a situation we understood: {}\n", show(acls));
  };

  ret.locations_inv.resize(ret.locations.size());
  for (auto& kv : ret.locations) {
    assertx(kv.first.isSingleLocation());
    make_conflict_set(kv.first, kv.second);
    ret.locations_inv[kv.second.index] = kv.second;

    /*
     * Note: this is probably more complex than it needs to be, because we're
     * iterating the stk_expand_map for each location.  Since kMaxTrackedALocs
     * is bounded by a constant, it's kinda O(stk_expand_map), but not in a
     * good way.  The number of locations is currently generally small, so this
     * is probably ok for now---but if we remove the limit we may need to
     * revisit this so it can't blow up.
     */
    if (kv.first.is_stack()) {
      for (auto& ent : ret.stk_expand_map) {
        if (kv.first <= ent.first) {
          FTRACE(2, "  ({}) {} <= {}\n",
            kv.second.index,
            show(kv.first),
            show(ent.first));
          ent.second.set(kv.second.index);
        }
      }
    } else if (kv.first.is_local()) {
      for (auto& ent : ret.loc_expand_map) {
        if (kv.first <= ent.first) {
          FTRACE(2, "  ({}) {} <= {}\n",
            kv.second.index,
            show(kv.first),
            show(ent.first));
          ent.second.set(kv.second.index);
        }
      }
    } else if (kv.first.is_iter()) {
      for (auto& ent : ret.iter_expand_map) {
        if (kv.first <= ent.first) {
          FTRACE(2, "  ({}) {} <= {}\n",
            kv.second.index,
            show(kv.first),
            show(ent.first));
          ent.second.set(kv.second.index);
        }
      }
    }
  }

  return ret;
}

//////////////////////////////////////////////////////////////////////

std::string show(ALocBits bits) {
  std::ostringstream out;
  if (bits.none()) {
    return "0";
  }
  if (bits.all()) {
    return "-1";
  }
  out << bits;
  return out.str();
}

std::string show(const AliasAnalysis& ainfo) {
  auto ret = std::string{};
  std::vector<const decltype(ainfo.locations)::value_type*> sorted;
  sorted.reserve(ainfo.locations.size());
  for (auto& kv : ainfo.locations) {
    sorted.push_back(&kv);
  }
  std::sort(sorted.begin(), sorted.end(),
            [](auto const* a, auto const* b) {
              return a->second.index < b->second.index;
            });

  for (auto const* kv : sorted) {
    auto conf = kv->second.conflicts;
    conf.set(kv->second.index);
    fmt::format_to(std::back_inserter(ret), " {: <20} = {: >3} : {}\n",
      show(kv->first),
      kv->second.index,
      show(conf));
  }

#define FMT(...)  " {: <20}       : {}\n"
#define SHOW(What, what, all) "all "#what, show(ainfo.all),

  fmt::format_to(
      std::back_inserter(ret),
      ALIAS_CLASSES(FMT)
      " {: <20}       : {}\n",

      ALIAS_CLASSES(SHOW)
      "all local",          show(ainfo.all_local)
  );
  std::vector<std::string> tmp;
  for (auto& kv : ainfo.loc_expand_map) {
    tmp.push_back(fmt::format(" ex {: <17}       : {}\n",
                                 show(kv.first),
                                 show(kv.second)));
  }
  std::sort(tmp.begin(), tmp.end());
  for (auto& s : tmp) ret += s;
  tmp.clear();
  fmt::format_to(std::back_inserter(ret), " {: <20}       : {}\n",
     "all iter",  show(ainfo.all_iter));
  for (auto& kv : ainfo.iter_expand_map) {
    tmp.push_back(fmt::format(" ex {: <17}       : {}\n",
                                 show(kv.first),
                                 show(kv.second)));
  }
  std::sort(tmp.begin(), tmp.end());
  for (auto& s : tmp) ret += s;
  tmp.clear();
  fmt::format_to(std::back_inserter(ret), " {: <20}       : {}\n",
     "all stack",  show(ainfo.all_stack));
  for (auto& kv : ainfo.stk_expand_map) {
    tmp.push_back(fmt::format(" ex {: <17}       : {}\n",
                                 show(kv.first),
                                 show(kv.second)));
  }
  std::sort(tmp.begin(), tmp.end());
  for (auto& s : tmp) ret += s;
  tmp.clear();
  return ret;
}

//////////////////////////////////////////////////////////////////////

}
