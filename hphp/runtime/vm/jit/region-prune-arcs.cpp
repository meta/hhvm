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
#include "hphp/runtime/vm/jit/region-prune-arcs.h"

#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "hphp/util/assertions.h"
#include "hphp/util/dataflow-worklist.h"
#include "hphp/util/trace.h"
#include "hphp/runtime/vm/jit/prof-data.h"
#include "hphp/runtime/vm/jit/region-selection.h"
#include "hphp/runtime/vm/jit/trans-cfg.h"
#include "hphp/runtime/vm/jit/type.h"

namespace HPHP::jit {

TRACE_SET_MOD(pgo)

namespace {

//////////////////////////////////////////////////////////////////////

struct State {
  bool initialized{false};
  std::vector<Type> locals;
  Type mbase{TCell};
};

template<typename NodeId>
struct NodeInfo {
  NodeId nodeId;
  const GuardedLocations* preConds;
  const PostConditions* postConds;
  State in;
  State out;
};

//////////////////////////////////////////////////////////////////////

std::string DEBUG_ONLY show(const State& state) {
  if (!state.initialized) return "<uninit>\n";
  auto ret = std::string{};

  for (auto locID = uint32_t{0}; locID < state.locals.size(); ++locID) {
    auto const ty = state.locals[locID];
    if (ty < TCell) fmt::format_to(std::back_inserter(ret), "  L{}: {}\n", locID, ty.toString());
  }
  auto const ty = state.mbase;
  if (ty < TCell) fmt::format_to(std::back_inserter(ret), "  M{{}}: {}\n", ty.toString());
  return ret;
}

State entry_state(const Func* func, std::vector<Type>* input = nullptr) {
  auto ret = State{};
  ret.initialized = true;

  if (input) ret.locals = *input;
  ret.locals.resize(func->numLocals(), TCell);

  return ret;
}

bool merge_type(Type& lhs, const Type& rhs) {
  auto const old = lhs;
  lhs |= rhs;
  return lhs != old;
}

bool merge_into(State& dst, const State& src) {
  if (!dst.initialized) {
    dst = src;
    return true;
  }
  auto changed = false;

  always_assert(dst.locals.size() == src.locals.size());
  for (auto i = uint32_t{0}; i < src.locals.size(); ++i) {
    changed |= merge_type(dst.locals[i], src.locals[i]);
  }

  changed |= merge_type(dst.mbase, src.mbase);

  return changed;
}

//////////////////////////////////////////////////////////////////////

bool is_tracked(Location l) {
  switch (l.tag()) {
    case LTag::Local:
    case LTag::MBase:
      return true;
    case LTag::Stack:
      return false;
  }
  not_reached();
}

Type& type_of(State& state, Location l) {
  switch (l.tag()) {
    case LTag::Local: {
      auto const locID = l.localId();
      assertx(locID < state.locals.size());
      return state.locals[locID];
    }
    case LTag::Stack:
    case LTag::MBase:
      return state.mbase;
  }
  not_reached();
}

Type type_of(const State& state, Location l) {
  return type_of(const_cast<State&>(state), l);
}

template<typename NodeId>
bool preconds_may_pass(char nodeSpec, const NodeInfo<NodeId>& binfo,
                       const State& state) {
  // Bail if any type is Bottom, which can only happen in unreachable paths.
  for (auto const& ty : state.locals) {
    if (ty == TBottom) return false;
  }

  for (auto const& p : *binfo.preConds) {
    if (!is_tracked(p.location)) continue;
    auto const ty = type_of(state, p.location);
    if (!ty.maybe(p.type)) {
      FTRACE(6, "  x {}{}'s precond {} fails ({} is {})\n",
             nodeSpec, binfo.nodeId, show(p), show(p.location), ty);
      return false;
    }
  }
  return true;
}

/*
 * PostConditions of a node are our local transfer functions.
 *
 * Changed types are overwritten, while refined types are intersected
 * with the current type.
 */
void apply_transfer_function(State& dst, const PostConditions& postConds) {
  for (auto& p : postConds.refined) {
    if (!is_tracked(p.location)) continue;
    auto& ty = type_of(dst, p.location);
    ty &= p.type;
  }
  for (auto& p : postConds.changed) {
    if (!is_tracked(p.location)) continue;
    auto& ty = type_of(dst, p.location);
    ty = p.type;
  }
}

//////////////////////////////////////////////////////////////////////

template<typename NodeId>
struct PruneArcs {
  using Arc = std::pair<NodeId, NodeId>;

  PruneArcs(char nodeSpec, uint32_t size)
    : m_nodeSpec(nodeSpec), m_size(size), m_nodeInfos(size), m_workQ(size) {
  }

  void registerNode(uint32_t idx, NodeId id, const GuardedLocations& preConds,
                    const PostConditions& postConds) {
    m_nodeInfos[idx].nodeId = id;
    m_nodeInfos[idx].preConds = &preConds;
    m_nodeInfos[idx].postConds = &postConds;
    m_nodeIdToIdx[id] = idx;
  }

  void addEntryNode(uint32_t idx, State entryState) {
    m_nodeInfos[idx].in = std::move(entryState);
    m_workQ.push(idx);
  }

  template<typename Succs>
  void iterate(Succs succs) {
    FTRACE(4, "Iterating:\n");
    while (!m_workQ.empty()) {
      auto const idx = m_workQ.pop();
      auto& binfo = m_nodeInfos[idx];
      FTRACE(4, "{}{}\n", m_nodeSpec, binfo.nodeId);

      binfo.out = binfo.in;
      apply_transfer_function(binfo.out, *binfo.postConds);

      for (auto& succ : succs(binfo.nodeId)) {
        auto const succIdx = nodeIdx(succ);
        auto& succInfo = m_nodeInfos[succIdx];
        if (preconds_may_pass(m_nodeSpec, succInfo, binfo.out)) {
          if (merge_into(succInfo.in, binfo.out)) {
            FTRACE(5, "  -> {}{}\n", m_nodeSpec, succInfo.nodeId);
            m_workQ.push(succIdx);
          }
        }
      }
    }

    FTRACE(2, "\nPostConds fixed point:\n{}\n",
      [&] () -> std::string {
        auto ret = std::string{};
        for (auto& s : m_nodeInfos) {
          fmt::format_to(std::back_inserter(ret), "{}{}:\n{}", m_nodeSpec, s.nodeId, show(s.in));
        }
        return ret;
      }()
    );
  }

  template<typename Succs>
  std::vector<Arc> removableArcs(Succs succs) const {
    // Return all edges that look like they will unconditionally fail type
    // predictions.
    auto removable = std::vector<Arc>{};

    for (auto const& binfo : m_nodeInfos) {
      for (auto& succ : succs(binfo.nodeId)) {
        auto const& succInfo = m_nodeInfos[nodeIdx(succ)];
        if (!binfo.in.initialized ||
            !succInfo.in.initialized ||
            !preconds_may_pass(m_nodeSpec, succInfo, binfo.out)) {
          removable.emplace_back(binfo.nodeId, succInfo.nodeId);
        }
      }
    }

    return removable;
  }

  bool isUnreachable(uint32_t idx) const {
    assertx(idx < m_size);
    return !m_nodeInfos[idx].in.initialized;
  }

  NodeId nodeId(uint32_t idx) const {
    assertx(idx < m_size);
    return m_nodeInfos[idx].nodeId;
  }

  uint32_t nodeIdx(NodeId nodeId) const {
    auto const iter = m_nodeIdToIdx.find(nodeId);
    assertx(iter != end(m_nodeIdToIdx));
    return iter->second;
  }

private:
  char m_nodeSpec;
  uint32_t m_size;
  std::vector<NodeInfo<NodeId>> m_nodeInfos;
  dataflow_worklist<uint32_t> m_workQ;

  // Maps node ids to their indices.
  jit::fast_map<NodeId, uint32_t> m_nodeIdToIdx;
};

}

//////////////////////////////////////////////////////////////////////

void region_prune_arcs(RegionDesc& region, std::vector<Type>* input) {
  FTRACE(4, "region_prune_arcs\n");

  region.sortBlocks();
  auto const& blocks = region.blocks();
  auto const numBlocks = blocks.size();
  auto pruneArcs = PruneArcs<RegionDesc::BlockId>('B', numBlocks);

  for (auto idx = uint32_t{0}; idx < numBlocks; ++idx) {
    auto const& b = blocks[idx];
    pruneArcs.registerNode(
      idx, b->id(), b->typePreConditions(), b->postConds());
    if (b->start() == region.start()) {
      pruneArcs.addEntryNode(idx, entry_state(region.start().func(), input));
    }
  }

  auto const succs = [&](RegionDesc::BlockId id) { return region.succs(id); };

  pruneArcs.iterate(succs);

  for (auto& arc : pruneArcs.removableArcs(succs)) {
    FTRACE(2, "Pruning arc: B{} -> B{}\n", arc.first, arc.second);
    region.removeArc(arc.first, arc.second);
  }

  for (auto idx = uint32_t{0}; idx < numBlocks; ++idx) {
    if (pruneArcs.isUnreachable(idx)) {
      auto const blockId = pruneArcs.nodeId(idx);
      FTRACE(2, "Pruning block: B{}\n", blockId);
      region.deleteBlock(blockId);
    }
  }

  FTRACE(2, "\n");
}

//////////////////////////////////////////////////////////////////////

void trans_cfg_prune_arcs(TransCFG& cfg, const std::vector<TransID>& nodes) {
  FTRACE(4, "trans_cfg_prune_arcs\n");

  if (nodes.size() == 0) return;

  auto const numNodes = nodes.size();
  auto pruneArcs = PruneArcs<TransID>('t', numNodes);

  for (auto idx = uint32_t{0}; idx < numNodes; ++idx) {
    auto const tid = nodes[idx];
    auto const rec = profData()->transRec(tid);
    assertx(rec->region()->blocks().size() == 1);
    auto const& b = rec->region()->blocks()[0];
    assertx(b->id() == tid);
    pruneArcs.registerNode(idx, tid, b->typePreConditions(), b->postConds());
  }

  auto const anyState = entry_state(profData()->transRec(nodes[0])->func());

  // Mark the node where the first translation will start as entry node.
  pruneArcs.addEntryNode(0, anyState);

  for (auto idx = uint32_t{0}; idx < numNodes; ++idx) {
    auto const tid = nodes[idx];

    // Mark all nodes without predecessors as entry nodes.
    if (cfg.inArcs(tid).empty()) {
      pruneArcs.addEntryNode(idx, anyState);
    }

    // Mark all successors of nodes that break region as entry nodes.
    if (breaksRegion(profData()->transRec(tid)->lastSrcKey())) {
      for (auto arc : cfg.outArcs(tid)) {
        pruneArcs.addEntryNode(pruneArcs.nodeIdx(arc->dst()), anyState);
      }
    }
  }

  auto const succs = [&](TransID tid) {
    std::vector<TransID> s;
    for (auto arc : cfg.outArcs(tid)) s.emplace_back(arc->dst());
    return s;
  };

  pruneArcs.iterate(succs);

  // Keep marking unreachable nodes as entry nodes and continue iteration. These
  // nodes were obviously reached during profiling, perhaps we are missing some
  // info due to interpreting. These are the nodes where regionizeFunc() is
  // likely to start new translations.
  for (auto idx = uint32_t{0}; idx < numNodes; ++idx) {
    if (pruneArcs.isUnreachable(idx)) {
      pruneArcs.addEntryNode(idx, anyState);
      pruneArcs.iterate(succs);
      assertx(!pruneArcs.isUnreachable(idx));
    }
  }

  for (auto& arc : pruneArcs.removableArcs(succs)) {
    FTRACE(2, "Pruning arc: t{} -> t{}\n", arc.first, arc.second);
    cfg.removeArc(arc.first, arc.second);
  }

  FTRACE(2, "\n");
}

}
