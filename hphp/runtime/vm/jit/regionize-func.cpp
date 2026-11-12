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

#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>

#include "hphp/util/assertions.h"
#include "hphp/util/configs/jit.h"
#include "hphp/runtime/vm/jit/mcgen.h"
#include "hphp/runtime/vm/jit/prof-data.h"
#include "hphp/runtime/vm/jit/region-prune-arcs.h"
#include "hphp/runtime/vm/jit/region-selection.h"
#include "hphp/runtime/vm/jit/timer.h"
#include "hphp/runtime/vm/jit/trans-cfg.h"

namespace HPHP::jit {

TRACE_SET_MOD(pgo)

//////////////////////////////////////////////////////////////////////

namespace {

using TransIDToRegionMap = hphp_hash_map<TransID, RegionDescPtr>;
using RegionToTransIDsMap = hphp_hash_map<
  RegionDescPtr,
  TransIDVec,
  smart_pointer_hash<RegionDescPtr>
>;

/**
 * Returns the set of of TransIDs that are in `bid's retranslation
 * chain in `region'.
 */
TransIDSet findRetransSet(const RegionDesc& region, RegionDesc::BlockId bid) {
  TransIDSet set;
  auto insert = [&](RegionDesc::BlockId id) {
    set.insert(id);
    auto& merged = region.merged(id);
    set.insert(merged.begin(), merged.end());
  };
  insert(bid);
  while (auto next = region.nextRetrans(bid)) {
    bid = next.value();
    insert(bid);
  }
  return set;
}

/**
 * Add to `coveredArcs' all `cfg's arcs going from `src' to 'dst' or
 * any of `dst's retranslations in `dstRegion'.
 */
void markCoveredArc(TransID src,
                    TransID dst,
                    const TransCFG& cfg,
                    const RegionDesc& dstRegion,
                    TransCFG::ArcPtrSet& coveredArcs) {
  auto dstRetransSet = findRetransSet(dstRegion, dst);
  for (auto outArc : cfg.outArcs(src)) {
    if (dstRetransSet.contains(outArc->dst())) {
      coveredArcs.insert(outArc);
    }
  }
}

/**
 * Add to sets coveredNodes and coveredArcs the cfg arcs that are now
 * covered given the new region containing the translations in `region'.
 */
void markCovered(const TransCFG& cfg, const RegionDescPtr region,
                 TransIDSet& heads,
                 TransIDToRegionMap& headToRegion,
                 TransIDSet& coveredNodes,
                 TransCFG::ArcPtrSet& coveredArcs) {
  assertx(!region->empty());
  const auto entryId = region->entry()->id();

  // Mark all region's nodes as covered.
  for (auto& b : region->blocks()) {
    coveredNodes.insert(b->id());
    const auto& merged = region->merged(b->id());
    coveredNodes.insert(merged.begin(), merged.end());
  }

  // Mark as covered all incoming arcs from already covered nodes into the entry
  // of the region or one of its retranslations/merged blocks.
  for (auto newHead : findRetransSet(*region, entryId)) {
    heads.insert(newHead);
    headToRegion[newHead] = region;
    for (auto arc : cfg.inArcs(newHead)) {
      const auto src = arc->src();
      if (coveredNodes.contains(src)) {
        markCoveredArc(src, entryId, cfg, *region, coveredArcs);
      }
    }
  }

  // Mark all CFG arcs within the region as covered.
  region->forEachArc([&](RegionDesc::BlockId src, RegionDesc::BlockId dst) {
    auto const srcIds = findRetransSet(*region, src);
    auto const dstIds = findRetransSet(*region, dst);

    for (auto srcId : srcIds) {
      for (auto arc : cfg.outArcs(srcId)) {
        if (dstIds.contains(arc->dst())) {
          coveredArcs.insert(arc);
        }
      }
    }
  });

  // Mark all outgoing arcs from the region to a head node as covered.
  for (auto& b : region->blocks()) {
    for (auto srcId : findRetransSet(*region, b->id())) {
      for (auto arc : cfg.outArcs(srcId)) {
        if (heads.contains(arc->dst())) {
          auto dstRegionEntryId = headToRegion[arc->dst()]->entry()->id();
          markCoveredArc(arc->src(), dstRegionEntryId, cfg,
                         *headToRegion[arc->dst()], coveredArcs);
        }
      }
    }
  }
}

/**
 * Mark `node' and all of its incoming and outgoing arcs as covered without
 * forming a region for it.
 */
void markNodeAndArcsCovered(const TransCFG& cfg, TransID node,
                            TransIDSet& coveredNodes,
                            TransCFG::ArcPtrSet& coveredArcs) {
  coveredNodes.insert(node);
  auto const& inArcs = cfg.inArcs(node);
  coveredArcs.insert(inArcs.begin(), inArcs.end());
  auto const& outArcs = cfg.outArcs(node);
  coveredArcs.insert(outArcs.begin(), outArcs.end());
}

/**
 * Returns the sum of the weights of the arcs going from srcs to dst.
 */
int64_t interRegionWeight(const TransIDVec& srcs,
                          TransID           dst,
                          const TransCFG&   cfg) {
  int64_t totalWeight = 0;
  for (auto src : srcs) {
    for (auto arc : cfg.outArcs(src)) {
      if (arc->dst() == dst) {
        totalWeight += arc->weight();
      }
    }
  }
  return totalWeight;
}

const TransIDVec& getRegionTransIDVec(const RegionToTransIDsMap& map,
                                      RegionDescPtr region) {
  auto it = map.find(region);
  assertx(it != map.end());
  return it->second;
}

/**
 * Sorts the regions vector in a linear order to be used for
 * translation.  The goal is to obtain an order that improves locality
 * when the function is executed.  Each region is translated separately.
 */
void sortRegions(RegionVec& regions, const Func* /*func*/, const TransCFG& cfg,
                 const ProfData* profData,
                 const TransIDToRegionMap& /*headToRegion*/,
                 const RegionToTransIDsMap& regionToTransIds) {
  if (regions.empty()) return;

  RegionVec sorted;
  RegionSet selected;

  // First, add all trivial DV func entries. They are already sorted by
  // the number of passed arguments in the ascending order.
  // This assumes that non-trivial DV func entries are uncommon.
  for (auto r : regions) {
    auto const firstTid = getRegionTransIDVec(regionToTransIds, r)[0];
    auto const firstSk = profData->transRec(firstTid)->srcKey();
    if (firstSk.trivialDVFuncEntry()) {
      sorted.push_back(r);
      selected.insert(r);
    }
  }

  if (sorted.size() == regions.size()) {
    regions = sorted;
    return;
  }

  // Second, try to pick the func entry region with the highest profile weight,
  // as that's the most commonly used entry point. If there is none, pick the
  // lowest bytecode offset, as that's where we are most likely to recover from
  // interpreting. Tie them by largest profile weight.
  RegionDescPtr entryRegion = nullptr;
  int64_t maxEntryWeight = -1;
  auto lowestSk = SrcKey {};
  for (const auto& pair : regionToTransIds) {
    auto r = pair.first;
    if (selected.contains(r)) continue;
    auto& tids = pair.second;
    auto const firstTid = tids[0];
    auto const firstSk = profData->transRec(firstTid)->srcKey();
    auto const weight = cfg.weight(firstTid);
    auto const isBetter = [&]{
      if (!lowestSk.valid()) return true;
      if (firstSk == lowestSk) return weight > maxEntryWeight;
      if (firstSk.anyFuncEntry() && !lowestSk.anyFuncEntry()) return true;
      if (!firstSk.anyFuncEntry() && lowestSk.anyFuncEntry()) return false;
      if (!firstSk.anyFuncEntry()) return firstSk.offset() < lowestSk.offset();
      if (weight != maxEntryWeight) return weight > maxEntryWeight;
      return firstSk.entryOffset() < lowestSk.entryOffset();
    }();
    if (isBetter) {
      entryRegion = r;
      maxEntryWeight = weight;
      lowestSk = firstSk;
    }
  }

  assertx(entryRegion);
  sorted.push_back(entryRegion);
  selected.insert(entryRegion);

  RegionDescPtr region = entryRegion;
  // Select the remaining regions, iteratively picking the most likely
  // region to execute next.
  for (auto i = sorted.size(); i < regions.size(); i++) {
    int64_t      maxWeight = -1;
    int64_t  maxHeadWeight = -1;
    RegionDescPtr bestNext = nullptr;
    auto    regionTransIds = getRegionTransIDVec(regionToTransIds, region);
    for (auto next : regions) {
      if (selected.contains(next)) continue;
      auto nextTransIds = getRegionTransIDVec(regionToTransIds, next);
      int64_t weight = interRegionWeight(regionTransIds, nextTransIds[0], cfg);
      int64_t headWeight = cfg.weight(nextTransIds[0]);
      if ((weight >  maxWeight) ||
          (weight == maxWeight && headWeight > maxHeadWeight)) {
        maxWeight     = weight;
        maxHeadWeight = headWeight;
        bestNext      = next;
      }
    }
    assertx(bestNext);
    sorted.push_back(bestNext);
    selected.insert(bestNext);
    region = bestNext;
  }

  assertx(sorted.size() == regions.size());
  regions = sorted;

  if (debug && Trace::moduleEnabled(HPHP::Trace::pgo, 5)) {
    for (size_t i = 0; i < regions.size(); i++) {
      auto r = regions[i];
      auto tids = getRegionTransIDVec(regionToTransIds, r);
      std::string transIds = folly::join(", ", tids);
      FTRACE(6, "sortRegions: region[{}]: {}\n", i, transIds);
    }
  }
}

bool allArcsCovered(const TransCFG::ArcPtrVec& arcs,
                    const TransCFG::ArcPtrSet& coveredArcs) {
  for (auto arc : arcs) {
    if (!coveredArcs.contains(arc)) {
      return false;
    }
  }
  return true;
}

}

//////////////////////////////////////////////////////////////////////

/**
 * Regionize a func, so that each node and each arc in its TransCFG is
 * "covered".  A node is covered if any region contains it.  An arc T1->T2
 * is covered if either:
 *
 *   a) T1 and T2 are in the same region R and R contains arc T1->T2.
 *   b) T2 is the head (first translation) of a region.
 *
 * Basic algorithm:
 *
 *   1) sort nodes in decreasing weight order
 *   2) for each node N:
 *      2.1) if N and all its incoming arcs are covered, then continue
 *      2.2) select a region starting at this node and mark nodes/arcs as
 *           covered appropriately
 */
RegionVec regionizeFunc(const Func* func, std::string& transCFGAnnot) {
  const Timer rf_timer(Timer::regionizeFunc, nullptr);
  assertx(profData());

  tracing::Block _{"regionize-func", [&] { return traceProps(func); }};

  auto regionMode = pgoRegionMode(*func);

  auto const funcId = func->getFuncId();
  auto const profData = jit::profData();
  TransCFG cfg(func, profData);

  if (Trace::moduleEnabled(HPHP::Trace::pgo, 5)) {
    auto dotFileName = folly::to<std::string>(
      "/tmp/func-cfg-", funcId.toInt(), ".dot");
    std::ofstream outFile(dotFileName);
    if (outFile.is_open()) {
      cfg.print(outFile, funcId, profData);
      FTRACE(5, "regionizeFunc: initial CFG for func {}({}) saved to file {}\n",
        func->fullName()->data(), funcId, dotFileName);
      outFile.close();
    }
  }
  if (mcgen::dumpTCAnnotation(TransKind::Optimize) &&
      Cfg::Eval::DumpRegion >= 2) {
    std::ostringstream cfgStream;
    cfg.print(cfgStream, funcId, profData);
    transCFGAnnot = cfgStream.str();
  }

  auto nodes = cfg.nodes();
  std::sort(
    nodes.begin(),
    nodes.end(),
    [&](TransID tid1, TransID tid2) -> bool {
      if (regionMode == PGORegionMode::WholeCFG ||
          regionMode == PGORegionMode::HotCFG) {
        auto sk1 = profData->transRec(tid1)->srcKey();
        auto sk2 = profData->transRec(tid2)->srcKey();
        if (sk1 != sk2) {
          if (sk1.anyFuncEntry() != sk2.anyFuncEntry()) return sk1.anyFuncEntry();
          if (sk1.funcEntry() && sk2.funcEntry()) {
            return sk1.numEntryArgs() < sk2.numEntryArgs();
          }
          if (sk1.namedParamsFuncEntry()) return !sk2.funcEntry();
          if (sk2.namedParamsFuncEntry()) return !sk1.funcEntry();
          return sk1.offset() < sk2.offset();
        }
      }
      if (cfg.weight(tid1) != cfg.weight(tid2)) {
        return cfg.weight(tid1) > cfg.weight(tid2);
      }
      // In case of ties, pick older translations first, in an attempt to start
      // loops at their headers.
      return tid1 < tid2;
    }
  );

  trans_cfg_prune_arcs(cfg, nodes);

  auto arcs = cfg.arcs();

  TransCFG::ArcPtrSet coveredArcs;
  TransIDSet coveredNodes;
  TransIDSet heads;
  TransIDToRegionMap headToRegion;
  RegionToTransIDsMap regionToTransIds;
  RegionVec regions;

  for (auto node : nodes) {
    if (!coveredNodes.count(node) ||
        !allArcsCovered(cfg.inArcs(node),  coveredArcs)) {
      auto newHead = node;
      // If the weight of node is too low, we mark it and its incoming arcs as
      // covered but skip generating a region starting at it to reduce code
      // size.  This node will probably trigger a live translation instead.
      auto const minBlkPerc = Cfg::Jit::PGOMinBlockCountPercent;
      if (cfg.weight(node) < cfg.weight(nodes[0]) * minBlkPerc / 100) {
        FTRACE(3, "regionizeFunc: skipping forming a region to cover node {}\n",
               newHead);
        markNodeAndArcsCovered(cfg, node, coveredNodes, coveredArcs);
        continue;
      }
      FTRACE(6, "regionizeFunc: selecting trace to cover node {}\n", newHead);
      RegionDescPtr region;
      HotTransContext ctx;
      ctx.cfg = &cfg;
      ctx.profData = profData;
      ctx.entries = {newHead};
      ctx.maxBCInstrs = Cfg::Jit::MaxRegionInstrs;
      switch (regionMode) {
        case PGORegionMode::Hottrace:
          region = selectHotTrace(ctx);
          break;

        case PGORegionMode::WholeCFG:
        case PGORegionMode::HotCFG:
          region = selectHotCFG(ctx);
          break;

        case PGORegionMode::Hotblock:
          always_assert(0 && "Invalid value for Cfg::Jit::PGORegionSelector");
      }
      if (!region) {
        // selectHotCFG() returns null when it ends up with an empty region.
        // Treat the node like the low-weight case above — mark it and its arcs
        // covered and move on — rather than dereferencing null below. Offline
        // retranslateAll over a prod profile reaches this.
        FTRACE(3, "regionizeFunc: no region selected to cover node {}\n",
               newHead);
        markNodeAndArcsCovered(cfg, node, coveredNodes, coveredArcs);
        continue;
      }
      FTRACE(6, "regionizeFunc: selected region to cover node {}\n{}\n",
             newHead, show(*region));
      profData->setOptimized(profData->transRec(newHead)->srcKey());

      for (auto& b : region->blocks()) {
        const auto bid = b->id();
        assertx(hasTransID(bid) &&
                bid == getTransID(bid) &&
                bid == b->profTransID());
        regionToTransIds[region].push_back(bid);
      }

      regions.emplace_back(region);
      markCovered(cfg, region, heads, headToRegion, coveredNodes, coveredArcs);
    }
  }

  assertx(coveredNodes.size() == cfg.nodes().size());
  assertx(coveredArcs.size() == arcs.size());

  sortRegions(regions, func, cfg, profData, headToRegion, regionToTransIds);

  if (debug && Trace::moduleEnabled(HPHP::Trace::pgo, 5)) {
    FTRACE(5, "\n--------------------------------------------\n"
           "regionizeFunc({}): computed regions:\n", funcId);
    for (auto region : regions) {
      FTRACE(5, "{}\n\n", show(*region));
    }
  }

  return regions;
}

}
