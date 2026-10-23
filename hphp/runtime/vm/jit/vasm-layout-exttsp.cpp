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

#include "hphp/util/configs/jit.h"
#include "hphp/util/debug-format.h"
#include "hphp/util/trace.h"

#include <algorithm>
#include <limits>

/*
 * This module implements a code layout strategy for sorting a Vunit's blocks:
 *
 * The algorithm is a greedy heuristic that works with chains (ordered lists)
 * of basic blocks. Initially all chains are isolated basic blocks. On every
 * iteration, we pick a pair of chains whose merging yields the biggest increase
 * in the ExtTSP value, which models how i-cache "friendly" a specific chain is.
 * A pair of chains giving the maximum gain is merged into a new chain. The
 * procedure stops when there is only one chain left, or when merging does not
 * increase ExtTSP. In the latter case, the remaining chains are sorted by
 * density in decreasing order.
 *
 * Reference:
 *    A. Newell and S. Pupyrev, Improved Basic Block Reordering,
 *    IEEE Transactions on Computers, 2020
 */

namespace HPHP::jit::layout {
////////////////////////////////////////////////////////////////////////////////

namespace {
////////////////////////////////////////////////////////////////////////////////

TRACE_SET_MOD(layout)

// Algorithm-specific constants for the ext-tsp model.
const double kForwardWeightCond = 0.1;
const double kForwardWeightUncond = 0.1;
const double kBackwardWeightCond = 0.1;
const double kBackwardWeightUncond = 0.1;
const double kFallthroughWeightCond = 1.0;
const double kFallthroughWeightUncond = 1.05;
const size_t kForwardDistance = 1024;
const size_t kBackwardDistance = 640;

// The maximum size of a chain created by the algorithm. The size is bounded
// so that the algorithm can efficiently process extremely large instance.
const size_t kMaxChainSize = 4096;
// The maximum size of a chain for splitting. Larger values of the threshold
// may yield better quality at the cost of worsen run-time.
const size_t kChainSplitThreshold = 128;

// Epsilon for comparison of doubles.
constexpr double EPS = 1e-8;

/*
 * Compute the Ext-TSP score for a given jump.
 */
double jumpExtTSPScore(uint64_t jumpDist, uint64_t jumpMaxDist, uint64_t count,
                       double weight) {
  if (jumpDist > jumpMaxDist) return 0;
  double prob = 1.0 - static_cast<double>(jumpDist) / jumpMaxDist;
  return weight * prob * count;
}

/*
 * Compute the Ext-TSP score for a jump between a given pair of blocks,
 * using their sizes, (estimated) addresses and the jump execution count.
 */
double extTSPScore(uint64_t srcAddr, uint64_t srcSize, uint64_t dstAddr,
                   uint64_t count, bool isConditional) {
  // Fallthrough
  if (srcAddr + srcSize == dstAddr) {
    return jumpExtTSPScore(0, 1, count,
                           isConditional ? kFallthroughWeightCond
                                         : kFallthroughWeightUncond);
  }
  // Forward
  if (srcAddr + srcSize < dstAddr) {
    const uint64_t dist = dstAddr - (srcAddr + srcSize);
    return jumpExtTSPScore(dist, kForwardDistance, count,
                           isConditional ? kForwardWeightCond
                                         : kForwardWeightUncond);
  }
  // Backward
  const uint64_t dist = srcAddr + srcSize - dstAddr;
  return jumpExtTSPScore(dist, kBackwardDistance, count,
                         isConditional ? kBackwardWeightCond
                                       : kBackwardWeightUncond);
}

/*
 * A type of merging two chains, X and Y. The former chain is split into
 * X1 and X2 and then concatenated with Y in the order specified by the type.
 */
enum class MergeTypeTy : int { X_Y, X1_Y_X2, Y_X2_X1, X2_X1_Y };

/*
 * The gain of merging two chains, that is, the Ext-TSP score of the merge
 * together with the corresponding merge 'type' and 'offset'.
 */
struct MergeGainTy {
  explicit MergeGainTy() = default;
  explicit MergeGainTy(double score, size_t mergeOffset, MergeTypeTy mergeType)
      : score(score), mergeOffset(mergeOffset), mergeType(mergeType) {}

  // Returns 'true' iff Other is preferred over this.
  bool operator<(const MergeGainTy& other) const {
    return other.score > EPS && other.score > score + EPS;
  }

  // Update the current gain if Other is preferred over this.
  void updateIfLessThan(const MergeGainTy& other) {
    if (*this < other) *this = other;
  }

  // The value of the merge gain.
  double score{-1.0};
  // The offset of the merge gain.
  size_t mergeOffset{0};
  // The type of the merge gain.
  MergeTypeTy mergeType{MergeTypeTy::X_Y};
};

struct Jump;
struct Chain;
struct ChainEdge;

/*
 * A node in the graph, typically corresponding to a basic block in CFG.
 */
struct Block {
  Block(const Block&) = delete;
  Block(Block&&) = default;
  Block& operator=(const Block&) = delete;
  Block& operator=(Block&&) = default;

  explicit Block(Vlabel label, uint64_t index, uint64_t size, uint64_t count)
    : label(label)
    , index(index)
    , size(size)
    , executionCount(count)
  {}

  bool isAdjacent(const Block* other) const;

  bool isEntry() const { return index == 0; }

  // Corresponding label of the basic block.
  const Vlabel label;
  // The original index of the block in CFG.
  size_t index{0};
  // The index of the block in the current chain.
  size_t curIndex{0};
  // (Estimated) size of the block in the binary.
  uint64_t size{0};
  // Execution count of the block in the profile data.
  uint64_t executionCount{0};
  // Current chain of the block.
  Chain* curChain{nullptr};
  // An offset of the block in the current chain.
  mutable uint64_t estimatedAddr{0};
  // Forced successor of the block in CFG.
  Block* forcedSucc{nullptr};
  // Forced predecessor of the block in CFG.
  Block* forcedPred{nullptr};
  // Outgoing jumps from the block.
  jit::vector<Jump *> outJumps;
  // Incoming jumps to the block.
  jit::vector<Jump *> inJumps;
};

/*
 * An arc in the graph, typically corresponding to a jump between two blocks.
 */
struct Jump {
  Jump(const Jump&) = delete;
  Jump(Jump&&) = default;
  Jump& operator=(const Jump&) = delete;
  Jump& operator=(Jump&&) = delete;

  explicit Jump(Block* source, Block* target, uint64_t executionCount)
      : source(source), target(target), executionCount(executionCount) {}

  // Source block of the jump.
  Block *source;
  // Target block of the jump.
  Block *target;
  // Execution count of the arc in the profile data.
  uint64_t executionCount{0};
  // Whether the jump corresponds to a conditional branch.
  bool isConditional{false};
};

bool Block::isAdjacent(const Block* other) const {
  for (Jump* jump : outJumps) {
    if (jump->target == other) {
      return true;
    }
  }
  for (Jump* jump : inJumps) {
    if (jump->source == other) {
      return true;
    }
  }
  return false;
}

/*
 * A chain (ordered sequence) of basic blocks.
 */
struct Chain {
  Chain(const Chain&) = delete;
  Chain(Chain&&) = default;
  Chain& operator=(const Chain&) = delete;
  Chain& operator=(Chain&&) = default;

  explicit Chain(uint64_t id, Block* block)
    : id(id)
    , score(0)
    , blocks(1, block)
    , executionCount(block->executionCount)
    , size(block->size)
  {}

  double density() const {
    return static_cast<double>(executionCount) / size;
  }

  bool isEntry() const { return blocks[0]->index == 0; }

  ChainEdge* getEdge(Chain* other) const {
    for (auto it : edges) {
      if (it.first == other) {
        return it.second;
      }
    }
    return nullptr;
  }

  void removeEdge(Chain* other) {
    auto it = edges.begin();
    while (it != edges.end()) {
      if (it->first == other) {
        edges.erase(it);
        return;
      }
      it++;
    }
  }

  void addEdge(Chain* other, ChainEdge* edge) {
    edges.push_back(std::make_pair(other, edge));
  }

  void merge(Chain* other, const jit::vector<Block*>& mergedBlocks) {
    executionCount += other->executionCount;
    size += other->size;
    blocks = mergedBlocks;
    // Update the block's chains
    for (size_t idx = 0; idx < blocks.size(); idx++) {
      blocks[idx]->curChain = this;
      blocks[idx]->curIndex = idx;
    }
  }

  void mergeEdges(Chain* other);

  void clear() {
    blocks.clear();
    blocks.shrink_to_fit();
    edges.clear();
    edges.shrink_to_fit();
  }

  // Unique chain identifier.
  size_t id;
  // Cached ext-tsp score for the chain.
  double score;
  // Blocks of the chain
  jit::vector<Block*> blocks;
  // Adjacent chains and corresponding edges (lists of jumps)
  jit::vector<std::pair<Chain*, ChainEdge*>> edges;
  // The total execution count of the chain.
  uint64_t executionCount;
  // The total size of the chain.
  uint64_t size;
};

/*
 * An edge in CFG representing jumps between two chains.
 * When blocks are merged into chains, the edges are combined too so that
 * there is always at most one edge between a pair of chains.
 */
struct ChainEdge {
  ChainEdge(const ChainEdge&) = delete;
  ChainEdge(ChainEdge&&) = default;
  ChainEdge& operator=(const ChainEdge&) = delete;
  ChainEdge& operator=(ChainEdge&&) = delete;

  explicit ChainEdge(Jump* jump)
    : srcChain(jump->source->curChain)
    , dstChain(jump->target->curChain)
    , jumps(1, jump)
  {}

  void changeEndpoint(Chain* from, Chain* to) {
    if (from == srcChain) {
      srcChain = to;
    }
    if (from == dstChain) {
      dstChain = to;
    }
  }

  void appendJump(Jump* jump) { jumps.push_back(jump); }

  void moveJumps(ChainEdge* other) {
    jumps.insert(jumps.end(), other->jumps.begin(), other->jumps.end());
    other->jumps.clear();
    other->jumps.shrink_to_fit();
  }

  bool hasCachedMergeGain(Chain* src, Chain* /*dst*/) const {
    return src == srcChain ? cacheValidForward : cacheValidBackward;
  }

  MergeGainTy getCachedMergeGain(Chain* src, Chain* /*dst*/) const {
    return src == srcChain ? cachedGainForward : cachedGainBackward;
  }

  void setCachedMergeGain(Chain* src, Chain* /*dst*/, MergeGainTy mergeGain) {
    if (src == srcChain) {
      cachedGainForward = mergeGain;
      cacheValidForward = true;
    } else {
      cachedGainBackward = mergeGain;
      cacheValidBackward = true;
    }
  }

  void invalidateCache() {
    cacheValidForward = false;
    cacheValidBackward = false;
  }

 private:
  // Source chain.
  Chain* srcChain{nullptr};
  // Destination chain.
  Chain* dstChain{nullptr};

 public:
  // Original jumps in the binary with corresponding execution counts.
  std::vector<Jump*> jumps;
  // Cached ext-tsp value for merging the pair of chains
  // Since the gain of merging (src, dst) and (dst, src) might be different,
  // we store both values here.
  MergeGainTy cachedGainForward;
  MergeGainTy cachedGainBackward;
  // Whether the cached value must be recomputed.
  bool cacheValidForward{false};
  bool cacheValidBackward{false};
};

void Chain::mergeEdges(Chain* other) {
  assert(this != other && "cannot merge a chain with itself");

  // Update edges adjacent to chain other
  for (auto edgeIt : other->edges) {
    Chain* dstChain = edgeIt.first;
    ChainEdge* dstEdge = edgeIt.second;
    Chain* targetChain = dstChain == other ? this : dstChain;

    ChainEdge* curEdge = getEdge(targetChain);
    if (curEdge == nullptr) {
      dstEdge->changeEndpoint(other, this);
      this->addEdge(targetChain, dstEdge);
      if (dstChain != this && dstChain != other) {
        dstChain->addEdge(this, dstEdge);
      }
    } else {
      curEdge->moveJumps(dstEdge);
    }
    // Cleanup leftover edge
    if (dstChain != other) {
      dstChain->removeEdge(other);
    }
  }
}

using BlockIter = std::vector<Block*>::const_iterator;

/*
 * A wrapper around three chains of blocks; it is used to avoid extra
 * instantiation of the vectors.
 */
struct MergedChain {
  MergedChain(BlockIter begin1,
              BlockIter end1,
              BlockIter begin2 = BlockIter(),
              BlockIter end2 = BlockIter(),
              BlockIter begin3 = BlockIter(),
              BlockIter end3 = BlockIter())
    : begin1(begin1)
    , end1(end1)
    , begin2(begin2)
    , end2(end2)
    , begin3(begin3)
    , end3(end3)
  {}

  template <typename F>
  void forEach(const F& func) const {
    for (auto it = begin1; it != end1; it++) {
      func(*it);
    }
    for (auto it = begin2; it != end2; it++) {
      func(*it);
    }
    for (auto it = begin3; it != end3; it++) {
      func(*it);
    }
  }

  jit::vector<Block*> getBlocks() const {
    jit::vector<Block*> result;
    result.reserve(
        std::distance(begin1, end1) +
        std::distance(begin2, end2) +
        std::distance(begin3, end3));
    result.insert(result.end(), begin1, end1);
    result.insert(result.end(), begin2, end2);
    result.insert(result.end(), begin3, end3);
    return result;
  }

  const Block* getFirstBlock() const {
    return *begin1;
  }

 private:
  BlockIter begin1;
  BlockIter end1;
  BlockIter begin2;
  BlockIter end2;
  BlockIter begin3;
  BlockIter end3;
};

/*
 * The implementation of the ExtTSP algorithm.
 */
struct ExtTSPImpl {
  ExtTSPImpl(const Vunit& unit,
         const Scale& scale,
         const jit::vector<Vlabel>& blocks)
    : unit(unit)
    , scale(scale)
	, numNodes(blocks.size())
  {
    initialize(blocks);
  }

  /*
   * Run the algorithm and return an optimized ordering of blocks.
   */
  void run(jit::vector<jit::vector<Vlabel>>& clusters) {
    // Pass 1: Merge blocks with their mutually forced successors
    mergeForcedPairs();

    // Pass 2: Merge pairs of chains while improving the ExtTSP objective
    mergeChainPairs();

    // Collect resulting clusters
    clusters.clear();
    clusters.reserve(allChains.size());
    for (Chain& chain : allChains) {
      if (chain.blocks.size() > 0) {
        jit::vector<Vlabel> cluster;
        for (Block* b : chain.blocks) {
          cluster.push_back(b->label);
        }
        clusters.push_back(cluster);
      }
    }
  }

 private:
  /*
   * Initialize the algorithm's data structures.
   */
  void initialize(const jit::vector<Vlabel>& blocks) {
    // Initialize blocks
    labelIndex.resize(unit.blocks.size(), 0);
    allBlocks.reserve(numNodes);
    for (uint64_t i = 0; i < numNodes; i++) {
      // Estimate the average size of an instruction (in bytes)
      uint64_t numInsts = unit.blocks[blocks[i]].code.size();
      uint64_t size = std::max<uint64_t>(4 * numInsts, 1);
      uint64_t executionCount = scale.weight(blocks[i]);
      // The execution count of the entry block is set to at least 1
      if (i == 0 && executionCount == 0)
        executionCount = 1;
      allBlocks.emplace_back(blocks[i], i, size, executionCount);
      labelIndex[blocks[i]] = i;
    }

    // Initialize jumps between the blocks
	  succNodes.resize(numNodes);
    predNodes.resize(numNodes);
    std::vector<uint64_t> outDegree(numNodes, 0);
    std::vector<uint64_t> inDegree(numNodes, 0);
    size_t numJumps = 0;
    for (Block& block : allBlocks) {
      auto succSet = succs(unit.blocks[block.label]);
      for (Vlabel succLabel : succSet) {
        uint64_t pred = block.index;
        uint64_t succ = labelIndex[succLabel];
        outDegree[pred]++;
        inDegree[succ]++;
        uint64_t executionCount = scale.weight(block.label, succLabel);
        if (pred != succ && executionCount > 0)
          numJumps++;
        allBlocks[pred].executionCount = std::max(allBlocks[pred].executionCount, executionCount);
        allBlocks[succ].executionCount = std::max(allBlocks[succ].executionCount, executionCount);
      }
    }
    allJumps.reserve(numJumps);

    // Reserve space for succNodes, predNodes, outJumps and inJumps for each block
    for (Block& block : allBlocks) {
      auto idx = block.index;
      block.outJumps.reserve(outDegree[idx]);
      block.inJumps.reserve(inDegree[idx]);
      succNodes[idx].reserve(outDegree[idx]);
      predNodes[idx].reserve(inDegree[idx]);
    }

    // Initialize edges for the blocks and compute their total in/out weights
    for (Block& block : allBlocks) {
      auto succSet = succs(unit.blocks[block.label]);
      for (Vlabel succLabel : succSet) {
        uint64_t pred = block.index;
        uint64_t succ = labelIndex[succLabel];
        // Ignore self-edges
        if (pred == succ) continue;

        succNodes[pred].push_back(succ);
        predNodes[succ].push_back(pred);
        uint64_t executionCount = scale.weight(block.label, succLabel);
        if (executionCount > 0) {
          Block& predBlock = allBlocks[pred];
          Block& succBlock = allBlocks[succ];
          allJumps.emplace_back(&predBlock, &succBlock, executionCount);
          succBlock.inJumps.push_back(&allJumps.back());
          predBlock.outJumps.push_back(&allJumps.back());
        }
      }
    }
    // Initialize "isConditional" flag for the jumps
    for (Jump& jump : allJumps) {
      assert(outDegree[jump.source->index] > 0 &&
             "incorrectly computed out-degree of the block");
      jump.isConditional = outDegree[jump.source->index] > 1;
    }

    // Initialize chains
    allChains.reserve(numNodes);
    hotChains.reserve(numNodes);
    for (Block& block : allBlocks) {
      allChains.emplace_back(block.index, &block);
      block.curChain = &allChains.back();
      if (block.executionCount > 0) {
        hotChains.push_back(&allChains.back());
      }
    }

    // Initialize chain edges
    allEdges.reserve(allJumps.size());
    for (Block& curBlock : allBlocks) {
      for (Jump* outJump : curBlock.outJumps) {
        Block* succBlock = outJump->target;
        ChainEdge* curEdge = curBlock.curChain->getEdge(succBlock->curChain);
        // This edge is already present in the graph
        if (curEdge != nullptr) {
          assert(succBlock->curChain->getEdge(curBlock.curChain) != nullptr);
          curEdge->appendJump(outJump);
          continue;
        }
        // This is a new edge
        allEdges.emplace_back(outJump);
        curBlock.curChain->addEdge(succBlock->curChain, &allEdges.back());
        succBlock->curChain->addEdge(curBlock.curChain, &allEdges.back());
      }
    }
    assert(allEdges.size() <= allJumps.size() && "Incorrect number of created edges");
  }

  /*
   * For a pair of blocks, A and B, block B is the forced successor of A,
   * if (i) all jumps (based on profile) from A goes to B and (ii) all jumps
   * to B are from A. Such blocks should be adjacent in the optimal ordering;
   * the method finds and merges such pairs of blocks.
   */
  void mergeForcedPairs() {
    // Find fallthroughs based on edge weights
    for (Block& block : allBlocks) {
      if (succNodes[block.index].size() == 1 &&
          predNodes[succNodes[block.index][0]].size() == 1 &&
          succNodes[block.index][0] != 0) {
        size_t SuccIndex = succNodes[block.index][0];
        block.forcedSucc = &allBlocks[SuccIndex];
        allBlocks[SuccIndex].forcedPred = &block;
      }
    }

    // There might be 'cycles' in the forced dependencies, since profile
    // data isn't 100% accurate. Typically this is observed in loops, when the
    // loop edges are the hottest successors for the basic blocks of the loop.
    // Break the cycles by choosing the block with the smallest index as the
    // head. This helps to keep the original order of the loops, which likely
    // have already been rotated in the optimized manner.
    for (Block& block : allBlocks) {
      if (block.forcedSucc == nullptr) continue;
      if (block.forcedPred == nullptr) continue;

      Block* succBlock = block.forcedSucc;
      while (succBlock != nullptr && succBlock != &block) {
        succBlock = succBlock->forcedSucc;
      }
      if (succBlock == nullptr) continue;
      // Break the cycle
      allBlocks[block.forcedPred->index].forcedSucc = nullptr;
      block.forcedPred = nullptr;
    }

    // Merge blocks with their fallthrough successors
    for (Block& block : allBlocks) {
      if (block.forcedPred == nullptr && block.forcedSucc != nullptr) {
        Block* curBlock = &block;
        while (curBlock->forcedSucc != nullptr) {
          const auto nextBlock = curBlock->forcedSucc;
          mergeChains(block.curChain, nextBlock->curChain, 0, MergeTypeTy::X_Y);
          curBlock = nextBlock;
        }
      }
    }
  }

  /*
   * Merge pairs of chains while improving the ExtTSP objective.
   */
  void mergeChainPairs() {
    /// Deterministically compare pairs of chains
    auto compareChainPairs = [](const Chain* a1, const Chain* b1,
                                const Chain* a2, const Chain* b2) {
      if (a1 != a2) {
        return a1->id < a2->id;
      }
      return b1->id < b2->id;
    };
    double prevScore = std::numeric_limits<double>::max();
    while (hotChains.size() > 1) {
      Chain* bestChainPred = nullptr;
      Chain* bestChainSucc = nullptr;
      MergeGainTy bestGain;
      // Iterate over all pairs of chains
      for (Chain* chainPred : hotChains) {
        // Since the score of merging doesn't increase, we can stop early when
        // the newly found merge is as good as the previous one
        if (bestGain.score == prevScore) break;

        // Get candidates for merging with the current chain
        for (auto edgeIter : chainPred->edges) {
          Chain* chainSucc = edgeIter.first;
          if (chainSucc == nullptr) continue;
          ChainEdge* chainEdge = edgeIter.second;
          // Ignore loop edges
          if (chainPred == chainSucc) continue;

          // Stop early if the combined chain violates the maximum allowed size
          if (chainPred->blocks.size() +
              chainSucc->blocks.size() >= kMaxChainSize) {
            continue;
          }

          // Don't merge the chains if they have vastly different densities.
          // We stop early if the ratio between the densities exceeds
          // JitLayoutMaxMergeRatio. Smaller values of the option result in
          // fewer merges (hence, more chains), which in turn typically yields
          // smaller size of the hot code section.
          double minDensity = std::min(chainPred->density(), chainSucc->density());
          double maxDensity = std::max(chainPred->density(), chainSucc->density());
          assert(minDensity > 0.0 && maxDensity > 0.0 &&
                 "incorrectly computed chain densities");
          const double ratio = maxDensity / minDensity;
          if (ratio > Cfg::Jit::LayoutExtTSPMaxMergeDensityRatio) continue;

          // Compute the gain of merging the two chains
          MergeGainTy curGain = getBestMergeGain(chainPred, chainSucc, chainEdge);
          if (curGain.score <= EPS) continue;

          if (bestGain < curGain  ||
              (std::abs(curGain.score - bestGain.score) < EPS &&
               (bestChainPred == nullptr ||
                compareChainPairs(chainPred, chainSucc, bestChainPred,
                                  bestChainSucc)))) {
            bestGain = curGain;
            bestChainPred = chainPred;
            bestChainSucc = chainSucc;
            // Stop early when the merge is as good as the previous one
            if (bestGain.score == prevScore) break;
          }
        }
      }

      // Stop merging when there is no improvement
      if (bestGain.score <= EPS) break;

      // Merge the best pair of chains
      prevScore = bestGain.score;
      mergeChains(
          bestChainPred,
          bestChainSucc,
          bestGain.mergeOffset,
          bestGain.mergeType);
    }
  }

  /*
   * Compute the Ext-TSP score for a given block order and a list of jumps.
   */
  double extTSPScore(const MergedChain& mergedBlocks,
                     const std::vector<Jump*>& jumps) const {
    if (jumps.empty()) {
      return 0.0;
    }
    uint64_t curAddr = 0;
    mergedBlocks.forEach([&](const Block* b) {
      b->estimatedAddr = curAddr;
      curAddr += b->size;
    });

    double score = 0;
    for (Jump* jump : jumps) {
      const Block* srcBlock = jump->source;
      const Block* dstBlock = jump->target;
      score += ::HPHP::jit::layout::extTSPScore(
          srcBlock->estimatedAddr,
          srcBlock->size,
          dstBlock->estimatedAddr,
          jump->executionCount,
		      jump->isConditional);
    }
    return score;
  }

  /*
   * Compute the gain of merging two chains
   *
   * The function considers all possible ways of merging two chains and
   * computes the one having the largest increase in ExtTSP objective. The result
   * is a pair with the first element being the gain and the second element
   * being the corresponding merging type.
   */
  MergeGainTy getBestMergeGain(Chain* chainPred,
                               Chain* chainSucc,
                               ChainEdge* edge) const {
    if (edge->hasCachedMergeGain(chainPred, chainSucc)) {
      return edge->getCachedMergeGain(chainPred, chainSucc);
    }

    // Precompute jumps between chainPred and chainSucc
    auto jumps = edge->jumps;
    ChainEdge* selfEdge = chainPred->getEdge(chainPred);
    if (selfEdge != nullptr) {
      jumps.insert(jumps.end(), selfEdge->jumps.begin(), selfEdge->jumps.end());
    }
    assert(jumps.size() > 0 && "trying to merge chains w/o jumps");

    // The object holds the best currently chosen gain of merging the two chains
    MergeGainTy gain;

    /// Given a merge offset and a list of merge types, try to merge two chains
    /// and update Gain with a better alternative
    auto tryChainMerging = [&](size_t Offset,
                               const std::vector<MergeTypeTy>& MergeTypes) {
      // Skip merging corresponding to concatenation w/o splitting
      if (Offset == 0 || Offset == chainPred->blocks.size()) return;
      // Skip merging if it breaks Forced successors
      Block* prevBlock = chainPred->blocks[Offset - 1];
      if (prevBlock->forcedSucc != nullptr) return;
      // Apply the merge, compute the corresponding gain, and update the best
      // value, if the merge is beneficial
      for (const auto& MergeType : MergeTypes) {
        gain.updateIfLessThan(
            computeMergeGain(chainPred, chainSucc, jumps, Offset, MergeType));
      }
    };

    // Try to concatenate two chains w/o splitting
    gain.updateIfLessThan(
        computeMergeGain(chainPred, chainSucc, jumps, 0, MergeTypeTy::X_Y));

    // Attach (a part of) ChainPred before the first block of ChainSucc
    for (Jump* jump : chainSucc->blocks.front()->inJumps) {
      const Block* srcBlock = jump->source;
      if (srcBlock->curChain != chainPred) continue;
      size_t offset = srcBlock->curIndex + 1;
      tryChainMerging(offset, {MergeTypeTy::X1_Y_X2, MergeTypeTy::X2_X1_Y});
    }

    // Attach (a part of) ChainPred after the last block of ChainSucc
    for (Jump* jump : chainSucc->blocks.back()->outJumps) {
      const Block* dstBlock = jump->target;
      if (dstBlock->curChain != chainPred) continue;
      size_t offset = dstBlock->curIndex;
      tryChainMerging(offset, {MergeTypeTy::X1_Y_X2, MergeTypeTy::Y_X2_X1});
    }

    // Try to break ChainPred in various ways and concatenate with ChainSucc
    if (chainPred->blocks.size() <= kChainSplitThreshold) {
      for (size_t offset = 1; offset < chainPred->blocks.size(); offset++) {
        const Block* BB = chainPred->blocks[offset - 1];
        const Block* BB2 = chainPred->blocks[offset];
        if (BB->isAdjacent(BB2)) continue;

        // Try to split the chain in different ways. In practice, applying
        // X2_Y_X1 merging is almost never provides benefits; thus, we exclude
        // it from consideration to reduce the search space
        tryChainMerging(offset, {MergeTypeTy::X1_Y_X2, MergeTypeTy::Y_X2_X1,
                                 MergeTypeTy::X2_X1_Y});
      }
    }
    edge->setCachedMergeGain(chainPred, chainSucc, gain);
    return gain;
  }

  /*
   * Compute the score gain of merging two chains, respecting a given
   * merge 'type' and 'offset'.
   *
   * The two chains are not modified in the method.
   */
  MergeGainTy computeMergeGain(const Chain* chainPred, const Chain* chainSucc,
                               const std::vector<Jump*>& jumps,
                               size_t mergeOffset,
                               MergeTypeTy mergeType) const {
    MergedChain mergedBlocks = mergeBlocks(chainPred->blocks, chainSucc->blocks,
                                    mergeOffset, mergeType);

    // Do not allow a merge that does not preserve the original entry block
    if ((chainPred->isEntry() || chainSucc->isEntry()) &&
        !mergedBlocks.getFirstBlock()->isEntry()) {
      return MergeGainTy();
    }

    // The gain for the new chain
    double newGainScore = extTSPScore(mergedBlocks, jumps) - chainPred->score;
    return MergeGainTy(newGainScore, mergeOffset, mergeType);
  }

  /*
   * Merge two chains of blocks respecting a given merge 'type' and 'offset'.
   *
   * If mergeType == 0, then the result is a concatenation of two chains.
   * Otherwise, the first chain is cut into two sub-chains at the offset,
   * and merged using all possible ways of concatenating three chains.
   */
  MergedChain mergeBlocks(
      const jit::vector<Block*>& x,
      const jit::vector<Block*>& y,
      size_t mergeOffset,
      MergeTypeTy mergeType) const {
    // Split the first chain, X, into X1 and X2
    BlockIter beginX1 = x.begin();
    BlockIter endX1 = x.begin() + mergeOffset;
    BlockIter beginX2 = x.begin() + mergeOffset;
    BlockIter endX2 = x.end();
    BlockIter beginY = y.begin();
    BlockIter endY = y.end();

    // Construct a new chain from the three existing ones
    switch (mergeType) {
      case MergeTypeTy::X_Y:
        return MergedChain(beginX1, endX2, beginY, endY);
      case MergeTypeTy::X1_Y_X2:
        return MergedChain(beginX1, endX1, beginY, endY, beginX2, endX2);
      case MergeTypeTy::Y_X2_X1:
        return MergedChain(beginY, endY, beginX2, endX2, beginX1, endX1);
      case MergeTypeTy::X2_X1_Y:
        return MergedChain(beginX2, endX2, beginX1, endX1, beginY, endY);
    }
    always_assert(false);
  }

  /*
   * Merge chain from into chain into, update the list of active chains,
   * adjacency information, and the corresponding cached values.
   */
  void mergeChains(
      Chain* into,
      Chain* from,
      size_t mergeOffset,
      MergeTypeTy mergeType) {
    // Merge the blocks
    MergedChain mergedBlocks =
        mergeBlocks(into->blocks, from->blocks, mergeOffset, mergeType);
    into->merge(from, mergedBlocks.getBlocks());
    into->mergeEdges(from);
    from->clear();

    // Update cached ext-tsp score for the new chain
    ChainEdge* selfEdge = into->getEdge(into);
    if (selfEdge != nullptr) {
      mergedBlocks = MergedChain(into->blocks.begin(), into->blocks.end());
      into->score = extTSPScore(mergedBlocks, selfEdge->jumps);
    }

    // Remove the chain from the list of active chains
    auto it = std::remove(hotChains.begin(), hotChains.end(), from);
    hotChains.erase(it, hotChains.end());

    // Invalidate caches
    for (auto edgeIter : into->edges) {
      edgeIter.second->invalidateCache();
    }
  }

 private:
  // The binary function.
  const Vunit& unit;
  // Provider of block/edge counts in Vunit.
  const Scale& scale;
  // An index of each label in the original order.
  jit::vector<uint64_t> labelIndex;

  // The number of nodes in the graph.
  const size_t numNodes;

  // Successors of each node.
  std::vector<std::vector<uint64_t>> succNodes;

  // Predecessors of each node.
  std::vector<std::vector<uint64_t>> predNodes;

  // All basic blocks.
  jit::vector<Block> allBlocks;

  // All jumps between blocks.
  jit::vector<Jump> allJumps;

  // All chains of basic blocks.
  jit::vector<Chain> allChains;

  // All edges between chains.
  jit::vector<ChainEdge> allEdges;

  // Active chains. The vector gets updated at runtime when chains are merged.
  jit::vector<Chain*> hotChains;
};

/*
 * Computes the "tsp-score" for a given ordering of basic blocks, that is, the
 * total weight of the jumps that are fallthrough in the order.
 * The method returns a pair (tsp score, total weight of the jumps).
 */
std::pair<uint64_t, uint64_t> tspScore(const Vunit& unit,
                                       const Scale& scale,
                                       const jit::vector<Vlabel>& labels) {
  // Initialize sizes of the blocks
  std::unordered_map<Vlabel, uint64_t> size;
  for (size_t i = 0; i < labels.size(); i++) {
    uint64_t numInsts = unit.blocks[labels[i]].code.size();
    size[labels[i]] = std::max<uint64_t>(4 * numInsts, 1);
  }
  // Initialize addresses of the blocks in memory for a given ordering
  std::unordered_map<Vlabel, uint64_t> addr;
  uint64_t curAddr = 0;
  for (size_t i = 0; i < labels.size(); i++) {
    addr[labels[i]] = curAddr;
    curAddr += size[labels[i]];
  }

  // Initialize edges for the blocks and compute their total in/out weights
  uint64_t tspScore = 0;
  uint64_t totalWeight = 0;
  for (size_t i = 0; i < labels.size(); i++) {
    uint64_t blockScore = 0;
    auto succSet = succs(unit.blocks[labels[i]]);
    for (Vlabel succLabel : succSet) {
      // Ignore self-loops
      if (succLabel == labels[i]) continue;
      uint64_t executionCount = scale.weight(labels[i], succLabel);
      if (addr[labels[i]] + size[labels[i]] == addr[succLabel]) {
        blockScore += executionCount;
      }
      totalWeight += executionCount;
    }
    tspScore += blockScore;
  }

  return std::make_pair(tspScore, totalWeight);
}

////////////////////////////////////////////////////////////////////////////////
} // anonymous namespace

void Clusterizer::clusterizeExtTSP() {
  auto blocks = m_scale.blocks();
  assert(!blocks.empty());
  assert(blocks[0] == m_unit.entry);

  DEBUG_ONLY auto const orgScore = (debug || Trace::enabled)
                                   ? tspScore(m_unit, m_scale, blocks)
                                   : std::make_pair(uint64_t(0), uint64_t(0));
  FTRACE(1, "[vasm-layout] Orig TSP score = {} ({:.2f}% of all counts)\n",
            groupDigits(orgScore.first),
            100.0 * orgScore.first / std::max(orgScore.second, uint64_t(1)));

  // Apply the algorithm
  ExtTSPImpl(m_unit, m_scale, blocks).run(m_clusters);

  // Assign the computed clusters
  jit::vector<Vlabel> newOrder;
  for (size_t i = 0; i < m_clusters.size(); i++) {
    for (auto b : m_clusters[i]) {
      m_blockCluster[b] = (Vlabel)i;
      newOrder.push_back(b);
    }
  }
  always_assert(newOrder.size() == blocks.size());

  DEBUG_ONLY auto const newScore = (debug || Trace::enabled)
                                   ? tspScore(m_unit, m_scale, newOrder)
                                   : std::make_pair(uint64_t(0), uint64_t(0));
  assert(orgScore.second == newScore.second && "incorrect tsp score");
  FTRACE(1, "[vasm-layout] New  TSP score = {} ({:.2f}% of all counts)\n",
            groupDigits(newScore.first),
            100.0 * newScore.first / std::max(newScore.second, uint64_t(1)));
}

////////////////////////////////////////////////////////////////////////////////
}
