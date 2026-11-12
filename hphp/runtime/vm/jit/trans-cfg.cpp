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

#include "hphp/runtime/vm/jit/trans-cfg.h"
#include <algorithm>

#include <fmt/format.h>
#include <folly/MapUtil.h>

#include "hphp/runtime/vm/jit/prof-data.h"
#include "hphp/runtime/vm/jit/region-selection.h"

namespace HPHP::jit {

TRACE_SET_MOD(pgo)

TransIDSet findPredTrans(const RegionDesc& rd, const ProfData* profData) {
  // In the case of profile serialization, the profile translations we
  // need to build the TransCFG are not in the SrcDB since they were collected
  // before we restarted. So instead, we call this function at serialization time,
  // and serialize the results to store them in the RegionDesc on deserialization.
  auto const incoming = rd.incoming();
  TransIDSet predSet;
  if (incoming) {
    for (auto id : *incoming) {
      predSet.insert(id);
    }
    return predSet;
  }

  // Otherwise, we compute with the SrcDB.
  auto const dstSK = rd.start();
  const SrcRec* dstSR = tc::findSrcRec(dstSK);
  assertx(dstSR);

  auto srLock = dstSR->readlock();
  for (auto& inBr : dstSR->incomingBranches()) {
    // Function calls are not tracked by TransCFG.
    if (inBr.type() == IncomingBranch::Tag::CALL) continue;

    auto const srcID = profData->jmpTransID(inBr.toSmash());
    if (srcID == kInvalidTransID) continue;

    auto const srcRec = profData->transRec(srcID);
    if (!srcRec || !srcRec->isProfile()) continue;

    FTRACE(5, "findPredTrans: toSmash = {}   srcID = {}\n",
           inBr.toSmash(), srcID);
    auto srcSuccSrcKeys = srcRec->lastSrcKey().succSrcKeys();
    if (srcSuccSrcKeys.contains(dstSK)) {
      predSet.insert(srcID);
    } else {
      FTRACE(5, "findPredTrans: WARNING: incoming branch with impossible "
             "control flow between translations: {} -> {}"
             "(probably due to side exit)\n", srcID, rd.entry()->id());
    }
  }

  return predSet;
}

/**
 * This function tries to infer the weight of any arc in the arcVec given the
 * weights of other arcs in the list and totalWeight, which is the
 * known sum of all their weights.
 * Returns whether or not the weight of any arc was inferred and, in case of
 * success, the weight of such arc is updated.
 */
static bool inferredArcWeight(const TransCFG::ArcPtrVec& arcVec,
                              int64_t totalWeight) {
  int64_t arcWeight = totalWeight;
  TransCFG::Arc* unknownArc = nullptr;
  for (auto arc : arcVec) {
    if (arc->weight() == TransCFG::Arc::kUnknownWeight) {
      if (unknownArc != nullptr) {
        // More than one arc with unknown weight, so can't infer
        return false;
      }
      unknownArc = arc;
    } else {
      arcWeight -= arc->weight();
    }
  }
  if (unknownArc == nullptr) return false;
  // Avoid creating negative-weight arcs. Node weights are not required to be
  // accurate and, since arc weights are derived from nodes' weights, they
  // aren't accurate either.  This can result in arcWeight to be negative here.
  if (arcWeight < 0) arcWeight = 0;
  unknownArc->setWeight(arcWeight);
  return true;
}

TransCFG::TransCFG(const Func* func,
                   const ProfData* profData,
                   bool inlining /* = false */) {
  assertx(profData);

  // add nodes
  for (auto const tid : profData->funcProfTransIDs(func)) {
    auto const rec = profData->transRec(tid);
    assertx(rec->region() != nullptr);
    // This will skip DV Funclets if they were already
    // retranslated w/ the prologues:
    if (inlining || !profData->optimized(rec->srcKey())) {
      int64_t weight = profData->transCounter(tid);
      addNode(tid, weight);
    }
  }

  // add arcs
  for (auto const dstId : nodes()) {
    auto const rec = profData->transRec(dstId);
    auto const dstSK = rec->srcKey();

    // Make sure we start a new translation at each FuncEntry.
    if (dstSK.anyFuncEntry()) continue;

    auto const dstBlock = rec->region()->entry();
    FTRACE(5, "TransCFG: adding incoming arcs in dstId = {}\n", dstId);
    TransIDSet predIDs = findPredTrans(*rec->region(), profData);
    for (auto predId : predIDs) {
      if (hasNode(predId)) {
        auto const predRec = profData->transRec(predId);
        auto const predBlock = predRec->region()->blocks().back();
        auto const& postConds = predBlock->postConds();
        auto predPostConds = postConds.changed;
        predPostConds.insert(predPostConds.end(), postConds.refined.begin(),
                             postConds.refined.end());
        auto const predSK = predRec->srcKey();
        if (preCondsAreSatisfied(dstBlock, predPostConds) &&
            predSK.resumeMode() == dstSK.resumeMode()) {
          FTRACE(5, "TransCFG: adding arc {} -> {} ({} -> {})\n",
                 predId, dstId, showShort(predSK), showShort(dstSK));
          addArc(predId, dstId, TransCFG::Arc::kUnknownWeight);
        }
      }
    }
  }

  // infer arc weights
  bool changed;
  do {
    changed = false;
    for (auto const tid : nodes()) {
      auto const nodeWeight = weight(tid);
      if (inferredArcWeight(inArcs(tid),  nodeWeight)) changed = true;
      if (inferredArcWeight(outArcs(tid), nodeWeight)) changed = true;
    }
  } while (changed);

  // guess weight for non-inferred arcs
  for (auto const tid : nodes()) {
    for (auto arc : outArcs(tid)) {
      if (arc->weight() == Arc::kUnknownWeight) {
        arc->setGuessed();
        auto arcWgt = std::min(weight(arc->src()), weight(arc->dst())) / 2;
        arc->setWeight(arcWgt);
      }
    }
  }
}

int64_t TransCFG::weight(TransID id) const {
  assertx(hasNode(id));
  auto const idx = folly::get_default(m_idToIdx, id);
  return m_nodeInfo[idx].weight();
}

const TransCFG::ArcPtrVec& TransCFG::inArcs(TransID id) const {
  assertx(hasNode(id));
  auto const idx = folly::get_default(m_idToIdx, id);
  return m_nodeInfo[idx].inArcs();
}

const TransCFG::ArcPtrVec& TransCFG::outArcs(TransID id) const {
  assertx(hasNode(id));
  auto const idx = folly::get_default(m_idToIdx, id);
  return m_nodeInfo[idx].outArcs();
}

TransCFG::Node::~Node() {
  for (auto arc : m_outArcs) {
    delete arc;
  }
}

void TransCFG::Node::removeInArc(Arc* arc) {
  auto pos = std::find(m_inArcs.begin(), m_inArcs.end(), arc);
  assertx(pos != m_inArcs.end());
  *pos = m_inArcs.back();
  m_inArcs.pop_back();
}

void TransCFG::Node::removeOutArc(Arc* arc) {
  auto pos = std::find(m_outArcs.begin(), m_outArcs.end(), arc);
  assertx(pos != m_outArcs.end());
  *pos = m_outArcs.back();
  m_outArcs.pop_back();
}

void TransCFG::addNode(TransID id, int64_t weight) {
  auto const idx = m_transIds.size();
  m_transIds.push_back(id);
  m_idToIdx[id] = idx;
  m_nodeInfo.push_back(Node(id, weight));
}

bool TransCFG::hasNode(TransID id) const {
  return m_idToIdx.find(id) != m_idToIdx.end();
}

TransCFG::ArcPtrVec TransCFG::arcs() const {
  ArcPtrVec arcs;
  for (auto node : nodes()) {
    const ArcPtrVec& nodeOutArcs = outArcs(node);
    arcs.insert(arcs.end(), nodeOutArcs.begin(), nodeOutArcs.end());
  }
  return arcs;
}

void TransCFG::addArc(TransID srcId, TransID dstId, int64_t weight) {
  assertx(hasNode(srcId));
  assertx(hasNode(dstId));
  auto const srcIdx = m_idToIdx[srcId];
  auto const dstIdx = m_idToIdx[dstId];
  Arc* arc = new Arc(srcId, dstId, weight);
  m_nodeInfo[srcIdx].addOutArc(arc);
  m_nodeInfo[dstIdx].addInArc(arc);
}

void TransCFG::removeArc(TransID srcId, TransID dstId) {
  assertx(hasNode(srcId));
  assertx(hasNode(dstId));
  auto const srcIdx = m_idToIdx[srcId];
  auto const dstIdx = m_idToIdx[dstId];

  Arc* toRemove = nullptr;
  for (auto arc : m_nodeInfo[srcIdx].outArcs()) {
    if (arc->dst() == dstId) {
      toRemove = arc;
      break;
    }
  }

  assertx(toRemove != nullptr);
  m_nodeInfo[srcIdx].removeOutArc(toRemove);
  m_nodeInfo[dstIdx].removeInArc(toRemove);
  delete toRemove;
}

bool TransCFG::hasArc(TransID srcId, TransID dstId) const {
  assertx(hasNode(srcId));
  assertx(hasNode(dstId));
  for (auto arc : outArcs(srcId)) {
    if (arc->dst() == dstId) return true;
  }
  return false;
}

void TransCFG::print(std::ostream& out, FuncId funcId,
                     const ProfData* profData) const {
  out << "digraph TransCFG {\ncomment=\"function: \""
      << Func::fromFuncId(funcId)->fullName()->data() << "\n";

  // find max node weight
  int64_t maxWeight = 1; // 1 to avoid div by 0
  for (auto tid : nodes()) {
    auto w = weight(tid);
    if (w > maxWeight) maxWeight = w;
  }

  // print nodes
  for (auto const tid : nodes()) {
    auto const w = weight(tid);
    uint32_t coldness  = 255 - (255 * w / maxWeight);
    auto const rec = profData->transRec(tid);
    auto const bcStart = rec->srcKey().printableOffset();
    auto const bcStop  = rec->lastSrcKey().printableOffset();
    auto const shape = "box";
    out << fmt::format(
      "t{} [shape={},label=\"T: {}\\np: {}\\n"
      "bc: [{}-{})\",style=filled,fillcolor=\"#ff{:02x}{:02x}\"];\n",
      tid, shape, tid, w, bcStart, bcStop, coldness, coldness);
  }

  // print arcs
  for (auto srcId : nodes()) {
    for (auto arc : outArcs(srcId)) {
      auto const w = arc->weight();
      out << fmt::format("t{} -> t{} [color=\"{}\",label=\"{}\"] ;\n",
                            srcId,
                            arc->dst(),
                            arc->guessed() ? "red" : "green4",
                            w);
    }
  }

  out << "}\n";
}

}
