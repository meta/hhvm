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

#include "hphp/runtime/vm/jit/vasm-layout.h"

#include "hphp/runtime/vm/jit/containers.h"
#include "hphp/runtime/vm/jit/timer.h"
#include "hphp/runtime/vm/jit/vasm.h"
#include "hphp/runtime/vm/jit/vasm-instr.h"
#include "hphp/runtime/vm/jit/vasm-print.h"
#include "hphp/runtime/vm/jit/vasm-unit.h"
#include "hphp/runtime/vm/jit/vasm-visit.h"

#include "hphp/util/configs/eval.h"
#include "hphp/util/configs/jit.h"
#include "hphp/util/trace.h"

#include <folly/MapUtil.h>

#include <algorithm>
#include <sstream>
#include <fmt/format.h>

/*
 * This module implements two code layout strategies for sorting a Vunit's
 * blocks:
 *
 *  1) rpoLayout() implements a simple layout that sorts the blocks in reverse
 *     post-order.  The final list of blocks is also partitioned so that any
 *     blocks assigned to the Main code area appear before the blocks assigned
 *     to the Cold area, which in turn appear before all blocks assigned to the
 *     Frozen area.  This method is used when no profiling information is
 *     available.
 *
 *  2) pgoLayout() is enabled for Optimize, PGO-based regions and OptPrologues.
 *     This implements the algorithm described in "Profile Guided Code Positioning"
 *     (PLDI'1990) by Pettis & Hansen (more specifically, Algo2, from section 4.2.1).
 *     This implementation uses estimated arc weights derived from a combination of
 *     profile counters inserted at the bytecode-level blocks (in Profile
 *     translations) and the JIT-time Likely/Unlikely/Unused hints (encoded in
 *     the "area" field of Vblocks).
 */

namespace HPHP::jit {

///////////////////////////////////////////////////////////////////////////////

namespace layout {

TRACE_SET_MOD(layout)

///////////////////////////////////////////////////////////////////////////////

jit::vector<Vlabel> rpoLayout(Vunit& unit) {
  auto labels = sortBlocks(unit);

  auto const blk = [&] (Vlabel b) -> const Vblock& { return unit.blocks[b]; };

  // Partition into main/cold/frozen areas without changing relative order, and
  // the fallthru{} block will be last if there is one.
  auto coldIt = std::stable_partition(labels.begin(), labels.end(),
    [&] (Vlabel b) {
      return blk(b).area_idx == AreaIndex::Main &&
             blk(b).code.back().op != Vinstr::fallthru;
    });
  std::stable_partition(coldIt, labels.end(),
    [&] (Vlabel b) {
      return blk(b).area_idx == AreaIndex::Cold &&
             blk(b).code.back().op != Vinstr::fallthru;
    });

  if (unit.context) {
    auto const kind = unit.context->kind;
    if ((isPrologue(kind)  && !Cfg::Jit::LayoutPrologueSplitHotCold) ||
        (isProfiling(kind) && !Cfg::Jit::LayoutProfileSplitHotCold)  ||
        (isLive(kind)      && !Cfg::Jit::LayoutLiveSplitHotCold)) {
      for (auto b : labels) {
        if (unit.blocks[b].area_idx == AreaIndex::Cold ||
            (unit.blocks[b].area_idx == AreaIndex::Frozen &&
             !Cfg::Jit::LayoutSplitFrozen)) {
          unit.blocks[b].area_idx = AreaIndex::Main;
        }
      }
    }
  }

  // We put fallthru{} blocks at the end, but we also need to make sure it's
  // still partitioned with those blocks that share a code area.  This should
  // always be true, so just assert it.
  DEBUG_ONLY auto const n = labels.size();
  assertx(n < 2 ||
    IMPLIES(
      blk(labels.back()).code.back().op == Vinstr::fallthru,
      blk(labels.back()).area_idx == blk(labels[n - 2]).area_idx
    )
  );

  return labels;
}

///////////////////////////////////////////////////////////////////////////////

Scale::Scale(const Vunit& unit)
    : m_unit(unit)
    , m_blocks(sortBlocks(unit))
    , m_preds(computePreds(unit)) {
  computeArcWeights();
}

Scale::Scale(const Vunit& unit, const jit::vector<Vlabel>& blockOrder)
    : m_unit(unit)
    , m_blocks(blockOrder)
    , m_preds(computePreds(unit)) {
  computeArcWeights();
}

uint64_t Scale::predSize(Vlabel blk) const {
  return m_preds[blk].size();
}

const jit::vector<Vlabel>& Scale::blocks() const {
  return m_blocks;
}

int64_t Scale::weight(Vlabel blk) const {
  return m_unit.blocks[blk].weight;
}

int64_t Scale::weight(Vlabel src, Vlabel dst) const {
  return folly::get_default(m_arcWgts, arcId(src, dst), 0);
}

TransIDSet Scale::findProfTransIDs(Vlabel blk) const {
  for (auto& i : m_unit.blocks[blk].code) {
    if (!i.origin) continue;
    auto profTransIDs = i.origin->marker().profTransIDs();
    if (profTransIDs.empty()) continue;
    return profTransIDs;
  }
  return TransIDSet{};
}

int64_t Scale::findProfCount(Vlabel blk) const {
  for (auto& i : m_unit.blocks[blk].code) {
    if (i.origin) {
      return i.origin->block()->profCount();
    }
  }
  return 1;
}

static bool is_catch(const Vblock& block) {
  return block.code.size() >= 1 && block.code[0].op == Vinstr::landingpad;
}

void Scale::computeArcWeights() {
  FTRACE(3, "[vasm-layout] computeArcWeights:\n");

  // First, we can easily compute the weight of the non-crititical arcs by
  // looking at its incident blocks.
  for (auto b : m_blocks) {
    auto succSet = succs(m_unit.blocks[b]);
    for (auto s : succSet) {
      if (Cfg::Jit::LayoutPruneCatchArcs &&
          is_catch(m_unit.blocks[s])) {
        continue;
      }
      auto arcid = arcId(b, s);
      m_arcWgts[arcid] = succSet.size()    == 1 ? weight(b)
                       : m_preds[s].size() == 1 ? weight(s)
                       : kUnknownWeight;
      assertx(m_arcWgts[arcid] >= 0);
      if (m_arcWgts[arcid] != kUnknownWeight) {
        FTRACE(3, "  - arc({} -> {}) [non-critical] => weight = {}  "
               "[|succs(b)| = {} ; |preds(s)| = {}] "
               "[weight(b) = {} ; weight(s) = {}]\n", b, s, m_arcWgts[arcid],
               succSet.size(), m_preds[s].size(), weight(b), weight(s));
      }
    }
  }

  // Next, do an iterative pass trying to infer the remaining arcs using the
  // fact that some arc weights are already known and the invariant that arc
  // weights incoming / outgoing a specific block should add up to that block's
  // weight.
  bool inferred = true;
  do {
    inferred = false;

    for (auto b : m_blocks) {
      const auto total = weight(b);

      // If b has a single successor with unknown weight, infer its weight.
      auto succSet = succs(m_unit.blocks[b]);
      unsigned numUnknown = 0;
      uint64_t unknownArcId = 0;
      uint64_t knownTotal = 0;
      for (auto s : succSet) {
        auto arcid = arcId(b, s);
        if (m_arcWgts[arcid] == kUnknownWeight) {
          numUnknown++;
          unknownArcId = arcid;
        } else {
          knownTotal += m_arcWgts[arcid];
        }
      }
      if (numUnknown == 1) {
        m_arcWgts[unknownArcId] = total > knownTotal ? total - knownTotal : 0;
        inferred = true;
        FTRACE(3, "  - arc({} -> {}) [inferred-succs] => weight = {}\n",
               b, unknownArcId & 0xffffffff, m_arcWgts[unknownArcId]);
      }

      // If b has a single predecessor with unknown weight, infer its weight.
      numUnknown = 0;
      unknownArcId = 0;
      knownTotal = 0;
      for (auto p : m_preds[b]) {
        auto arcid = arcId(p, b);
        if (m_arcWgts[arcid] == kUnknownWeight) {
          numUnknown++;
          unknownArcId = arcid;
        } else {
          knownTotal += m_arcWgts[arcid];
        }
      }
      if (numUnknown == 1) {
        m_arcWgts[unknownArcId] = total > knownTotal ? total - knownTotal : 0;
        inferred = true;
        FTRACE(3, "  - arc({} -> {}) [inferred-preds] => weight = {}\n",
               unknownArcId >> 32, b, m_arcWgts[unknownArcId]);
      }
    }
  } while (inferred);

  // Finally, for each arc whose weight is still unknown at this point, we
  // currently just approximate it as half of the smallest weight of its
  // incident blocks.
  for (auto b : m_blocks) {
    auto succSet = succs(m_unit.blocks[b]);
    for (auto s : succSet) {
      auto arcid = arcId(b, s);
      if (m_arcWgts[arcid] == kUnknownWeight) {
        m_arcWgts[arcid] = std::min(weight(b), weight(s)) / 2;
        FTRACE(3, "  - arc({} -> {}) [guessed] => weight = {}\n",
               b, s, m_arcWgts[arcid]);
      }
    }
  }
}

std::string Scale::toString() const {
  std::ostringstream out;
  out << "digraph LayoutCFG {\n";
  int64_t maxWgt = 1;
  for (auto b : m_blocks) {
    maxWgt = std::max(maxWgt, weight(b));
  }
  for (auto b : m_blocks) {
    auto const& block = m_unit.blocks[b];
    unsigned coldness = 255 - (255 * weight(b) / maxWgt);
    out << fmt::format(
      "{} [label=\"{}\\nw: {}\\nptid: {}\\narea: {}\\nprof: {}\\ninsts: {}\","
      "shape=box,style=filled,fillcolor=\"#ff{:02x}{:02x}\"]\n",
      b, b, weight(b), folly::join(',', findProfTransIDs(b)),
      unsigned(block.area_idx),
      findProfCount(b), block.code.size(), coldness, coldness);
    for (auto s : succs(block)) {
      out << fmt::format("{} -> {} [label={}];\n", b, s, weight(b, s));
    }
  }
  out << "}\n";
  return out.str();
}

///////////////////////////////////////////////////////////////////////////////

Clusterizer::Clusterizer(Vunit& unit, const Scale& scale)
      : m_unit(unit)
    , m_scale(scale)
    , m_blocks(sortBlocks(unit)) {
  initClusters();
  auto const isProlog = unit.context && isPrologue(unit.context->kind);
  if ((Cfg::Jit::LayoutExtTSP && !isProlog) ||
      (Cfg::Jit::LayoutExtTSPForPrologues && isProlog)) {
    clusterizeExtTSP();
  } else {
    clusterizeGreedy();
  }
  sortClusters();
  if (Cfg::Jit::PGOLayoutSplitHotCold) {
    splitHotColdClusters();
  } else {
    for (auto b : m_blocks) {
      if (unit.blocks[b].area_idx != AreaIndex::Frozen ||
          Cfg::Jit::PGOLayoutResplitFrozen) {
        unit.blocks[b].area_idx = AreaIndex::Main;
      }
    }
  }

  FTRACE(1, "{}", toString());
}

jit::vector<Vlabel> Clusterizer::getBlockList() const {
  jit::vector<Vlabel> list;
  for (auto cid : m_clusterOrder) {
    for (auto b : m_clusters[cid]) {
      list.push_back(b);
    }
  }
  return list;
}

std::string Clusterizer::toString() const {
  std::ostringstream out;
  out << "clusterize: final clusters:\n";
  for (auto cid : m_clusterOrder) {
    out << fmt::format("  - cluster {}: ", cid);
    for (auto b : m_clusters[cid]) {
      out << fmt::format("{}, ", b);
    }
    out << "\n";
  }
  return out.str();
}

void Clusterizer::initClusters() {
  m_clusters.resize(m_unit.blocks.size());
  m_blockCluster.resize(m_unit.blocks.size());
  for (auto b : m_blocks) {
    m_clusters[b].push_back(b);
    m_blockCluster[b] = b;
  }
}

void Clusterizer::clusterizeGreedy() {
  struct ArcInfo {
    Vlabel  src;
    Vlabel  dst;
    int64_t wgt;
  };
  jit::vector<ArcInfo> arcInfos;
  for (auto b : m_blocks) {
    for (auto s : succs(m_unit.blocks[b])) {
      arcInfos.push_back({b, s, m_scale.weight(b, s)});
    }
  }

  // sort arcs in decreasing weight order
  std::sort(arcInfos.begin(), arcInfos.end(),
            [&](const ArcInfo& a1, const ArcInfo& a2) {
              return a1.wgt > a2.wgt;
            });

  for (auto& arcInfo : arcInfos) {
    auto src = arcInfo.src;
    auto dst = arcInfo.dst;

    // Only merge blocks in the same area.
    if (m_unit.blocks[src].area_idx != m_unit.blocks[dst].area_idx) continue;

    auto srcCid = m_blockCluster[src];
    auto dstCid = m_blockCluster[dst];
    if (srcCid == dstCid) continue;
    auto& srcC = m_clusters[srcCid];
    auto& dstC = m_clusters[dstCid];

    // src must be the last in its cluster
    if (srcC.back() != src) continue;
    // dst must be the first in its cluster
    if (dstC.front() != dst) continue;

    // Don't merge blocks if their weights are beyond JitLayoutMaxMergeRatio.
    // Avoiding to create clusters with block with very different weights can
    // hurt cache locality. NB: We add 1 to the weights to avoid division by 0.
    auto const srcWgt = m_unit.blocks[src].weight;
    auto const dstWgt = m_unit.blocks[dst].weight;
    const double ratio = (1.0 + std::max(srcWgt, dstWgt)) /
                         (1.0 + std::min(srcWgt, dstWgt));
    if (ratio > Cfg::Jit::LayoutMaxMergeRatio) continue;

    // Don't merge zero and non-zero weight blocks that go in different areas.
    if (Cfg::Jit::LayoutSeparateZeroWeightBlocks) {
      auto const srcZero = m_unit.blocks[src].weight == 0;
      auto const dstZero = m_unit.blocks[dst].weight == 0;
      if (srcZero != dstZero) continue;
    }

    // merge the clusters by append the blocks in dstC to srcC
    for (auto d : dstC) {
      srcC.push_back(d);
      m_blockCluster[d] = srcCid;
    }
    dstC.clear();
  }
}

using SuccInfos = jit::hash_map<uint32_t, int64_t>; // cluster id => weight

struct DFSSortClusters {
  DFSSortClusters(jit::vector<SuccInfos>&& succInfos, const Vunit& unit)
    : m_clusterSuccs(std::move(succInfos))
    , m_visited(unit.blocks.size()) { }

  jit::vector<Vlabel> sort(uint32_t initialCid);

 private:
  void dfs(uint32_t cid);

  jit::vector<SuccInfos>  m_clusterSuccs;
  boost::dynamic_bitset<> m_visited;
  jit::vector<Vlabel>     m_list;
};

void DFSSortClusters::dfs(uint32_t cid) {

  if (m_visited.test(cid)) return;
  m_visited.set(cid);
  m_list.push_back(Vlabel(cid));

  // find the best successor, which is the one to which cid has the
  // highest weight among the ones that haven't been visited yet
  int64_t  maxWgt = 0;
  uint32_t bestSucc = uint32_t(-1);
  for (auto& sInfo : m_clusterSuccs[cid]) {
    auto succId = sInfo.first;
    if (m_visited.test(succId)) continue;
    auto wgt = sInfo.second;
    if (wgt >= maxWgt) {
      maxWgt   = wgt;
      bestSucc = succId;
    }
  }

  if (bestSucc == uint32_t(-1)) return;

  // visit bestSucc first
  dfs(bestSucc);

  // now visit the remaining ones
  for (auto& sInfo : m_clusterSuccs[cid]) {
    if (sInfo.first != bestSucc) {
      dfs(sInfo.first);
    }
  }
}

jit::vector<Vlabel> DFSSortClusters::sort(uint32_t initialCid) {
  dfs(initialCid);
  return std::move(m_list);
}

/*
 * This method creates a weighted graph of the clusters, and sorts
 * them according to a DFS pre-order that prioritizes the arcs with
 * heaviest weights, so as to try to have a cluster be followed by its
 * mostly likely successor cluster.
 */
void Clusterizer::sortClusters() {
  jit::vector<SuccInfos> clusterGraph;
  clusterGraph.resize(m_unit.blocks.size());

  for (auto b : m_blocks) {
    for (auto s : succs(m_unit.blocks[b])) {
      auto srcCid = m_blockCluster[b];
      auto dstCid = m_blockCluster[s];
      if (srcCid == dstCid) continue;
      auto wgt = m_scale.weight(b, s);
      clusterGraph[srcCid][dstCid] += wgt;
    }
  }

  DFSSortClusters dfsSort(std::move(clusterGraph), m_unit);
  m_clusterOrder = dfsSort.sort(m_blockCluster[m_unit.entry]);
}

void Clusterizer::splitHotColdClusters() {
  // compute the average weight of each cluster
  jit::vector<uint64_t> clusterAvgWgt(m_clusters.size());
  for (size_t c = 0; c < m_clusters.size(); c++) {
    uint64_t totalWeight = 0;
    uint64_t totalSize   = 0;
    for (auto b : m_clusters[c]) {
      const auto numInsts = m_unit.blocks[b].code.size();
      totalSize   += numInsts;
      totalWeight += numInsts * m_scale.weight(b);
    }
    clusterAvgWgt[c] = totalSize == 0 ? 0 : totalWeight / totalSize;
  }

  // The "hot weight" is a measure of how hot this function's entry is that
  // incorporates information from multiple translations. If we have (say)
  // two translations and the first is much hotter than the second, we can
  // use this hint to put the entire second translation into Cold or Frozen.
  //
  // If we always use this hint, we end up moving a lot of code across areas.
  // To let us smoothly interpolate between using and not using this hint,
  // we introduce a "multiplier" here: if it's set to 0, we'll always use this
  // translation's entry weight, and if it's set to 1, we'll always the entry
  // weight of the hottest translation for this function.
  auto const entryAvgWgt = clusterAvgWgt[m_blockCluster[m_unit.entry]];
  auto baseWgt = entryAvgWgt;
  if (m_unit.context && m_unit.context->region) {
    if (auto const hotWeight = m_unit.context->region->getHotWeight()) {
      auto const multiplier =
        Cfg::Jit::PGOVasmBlockCountersHotWeightMultiplier;
      baseWgt = std::max(entryAvgWgt, uint64_t(multiplier * (*hotWeight)));
      FTRACE(3, "baseWgt:{} = max(entryAvgWgt:{}, multiplier:{} * hotWeight:{})\n",
             baseWgt, entryAvgWgt, multiplier, *hotWeight);
    }
  }

  // An alternative way to penalize cold translations is to have an absolute
  // weight threshold in addition to a entry-relative one.
  uint64_t hotThreshold  = baseWgt * Cfg::Jit::LayoutHotThreshold;
  uint64_t coldThreshold = baseWgt * Cfg::Jit::LayoutColdThreshold;
  if (Cfg::Jit::LayoutMinHotThreshold) {
    hotThreshold = std::max(hotThreshold, Cfg::Jit::LayoutMinHotThreshold);
  }
  if (Cfg::Jit::LayoutMinColdThreshold) {
    coldThreshold = std::max(coldThreshold, Cfg::Jit::LayoutMinColdThreshold);
  }

  // Finally, for correctness, we can't allow any cluster to be in a hotter
  // region than the entry cluster. Adjust thresholds so that's the case.
  assertx(!m_clusterOrder.empty());
  auto maxAvgWgt = entryAvgWgt;
  for (auto cid : m_clusterOrder) {
    maxAvgWgt = std::max(maxAvgWgt, clusterAvgWgt[cid]);
  }
  if (maxAvgWgt >= hotThreshold && entryAvgWgt < hotThreshold) {
    FTRACE(3, "(maxAvgWgt = {}) >= (hotThreshold = {}) > (entryAvgWgt = {}) "
           "=> lower hotThreshold\n", maxAvgWgt, hotThreshold, entryAvgWgt);
    hotThreshold = entryAvgWgt;
  }
  if (maxAvgWgt >= coldThreshold && entryAvgWgt < coldThreshold) {
    FTRACE(3, "(maxAvgWgt = {}) >= (coldThreshold = {}) > (entryAvgWgt = {}) "
           "=> lower coldThreshold\n", maxAvgWgt, coldThreshold, entryAvgWgt);
    coldThreshold = entryAvgWgt;
  }

  // Also, for correctness, if we're padding the TC, put the entry in main.
  if (Cfg::Eval::ReusableTCPadding && entryAvgWgt < hotThreshold) {
    FTRACE(3, "TC includes {} padding bytes => put entry in main\n",
           Cfg::Eval::ReusableTCPadding);
    hotThreshold  = std::min(hotThreshold, entryAvgWgt);
    coldThreshold = std::min(coldThreshold, entryAvgWgt);
  }

  FTRACE(3, "splitHotColdClusters: baseWgt = {} ; hotThreshold = {} "
         "coldThreshold = {}\n", baseWgt, hotThreshold, coldThreshold);

  for (auto cid : m_clusterOrder) {
    if (m_clusters[cid].size() == 0) continue;
    const AreaIndex area =
      clusterAvgWgt[cid] >= hotThreshold  ? AreaIndex::Main :
      clusterAvgWgt[cid] >= coldThreshold ? AreaIndex::Cold :
                                            AreaIndex::Frozen;
    FTRACE(3, "  -> C{}: {} (avg wgt = {}): ",
           cid, area_names[unsigned(area)], clusterAvgWgt[cid]);
    for (auto b : m_clusters[cid]) {
      // If JitPGOLayoutResplitFrozen is false, don't change the area of blocks
      // initially assigned to frozen.
      if (m_unit.blocks[b].area_idx != AreaIndex::Frozen ||
          Cfg::Jit::PGOLayoutResplitFrozen) {
        m_unit.blocks[b].area_idx = area;
      }
      FTRACE(3, "{}, ", b);
    }
    FTRACE(3, "\n");
  }
}


///////////////////////////////////////////////////////////////////////////////

jit::vector<Vlabel> pgoLayout(Vunit& unit) {
  // Compute arc weights.
  Scale scale(unit);
  FTRACE(1, "profileGuidedLayout: Weighted CFG:\n{}\n", scale.toString());

  // Cluster the blocks based on weights and sort the clusters.
  Clusterizer clusterizer(unit, scale);
  auto labels = clusterizer.getBlockList();

  // Partition by actual code area without changing relative order.
  auto cold_iter = std::stable_partition(labels.begin(), labels.end(),
    [&] (Vlabel b) {
      return unit.blocks[b].area_idx == AreaIndex::Main;
    });
  if (cold_iter != labels.end()) {
    std::stable_partition(cold_iter, labels.end(),
      [&] (Vlabel b) {
        return unit.blocks[b].area_idx == AreaIndex::Cold;
      });
  }

  // Our relocation logic requires that no block be in a hotter area than the
  // entry block's area (e.g. if the entry is Cold, no block is in Main).
  // In particular, we use this invariant to identify the entry given the list
  // of code ranges for a translation (see TransLoc::entry()).
  //
  // We could lift this requirement with work, but it's probably not useful to
  // do so - if some blocks are in Main, we should put path from the entry to
  // those blocks in Main too. Instead, we assert if we break the invariant.
  //
  // If we're padding TC entries, we require that entries are in Main.
  assertx(!labels.empty());
  assertx(labels[0] == unit.entry);
  assertx(!Cfg::Eval::ReusableTCPadding ||
          unit.blocks[unit.entry].area_idx == AreaIndex::Main);

  if (Trace::moduleEnabled(Trace::layout, 1)) {
    FTRACE(1, "pgoLayout: final block list: ");
    for (DEBUG_ONLY auto b : labels) {
      FTRACE(1, "{}, ", b);
    }
    FTRACE(1, "\n");
  }

  if (Cfg::Eval::DumpLayoutCFG) {
    unit.annotations.emplace_back("LayoutCFG", Scale(unit, labels).toString());
  }

  return labels;
}

}

///////////////////////////////////////////////////////////////////////////////

jit::vector<Vlabel> layoutBlocks(Vunit& unit) {
  Timer timer(Timer::vasm_layout, unit.log_entry);
  auto const optimizePrologue = unit.context &&
    unit.context->kind == TransKind::OptPrologue &&
    Cfg::Jit::PGOVasmBlockCountersOptPrologue;

  return unit.context &&
    (unit.context->kind == TransKind::Optimize || optimizePrologue)
    ? layout::pgoLayout(unit)
    : layout::rpoLayout(unit);
}

///////////////////////////////////////////////////////////////////////////////

}
