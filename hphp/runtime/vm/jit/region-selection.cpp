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
#include "hphp/runtime/vm/jit/region-selection.h"

#include <algorithm>
#include <fstream>
#include <exception>
#include <utility>

#include <fmt/format.h>
#include <folly/Memory.h>
#include <folly/Conv.h>
#include <folly/String.h>

#include "hphp/util/assertions.h"
#include "hphp/util/configs/jit.h"
#include "hphp/runtime/vm/resumable.h"
#include "hphp/runtime/vm/jit/guard-constraint.h"
#include "hphp/runtime/vm/jit/prof-data.h"
#include "hphp/runtime/vm/jit/punt.h"
#include "hphp/runtime/vm/jit/trans-cfg.h"

namespace HPHP::jit {

TRACE_SET_MOD(region)

//////////////////////////////////////////////////////////////////////

extern RegionDescPtr selectMethod(const RegionContext&);

//////////////////////////////////////////////////////////////////////

namespace {

enum class RegionMode {
  None,      // empty region

  // Modes that create a region by inspecting live VM state
  Method,    // region with a whole method
  Tracelet,  // single-entry, multiple-exits region that ends on conditional
             // branches or when an instruction consumes a value of unknown type
};

RegionMode regionMode() {
  auto& s = Cfg::Jit::RegionSelector;
  if (s == ""        ) return RegionMode::None;
  if (s == "method"  ) return RegionMode::Method;
  if (s == "tracelet") return RegionMode::Tracelet;
  FTRACE(1, "unknown region mode {}: using none\n", s);
  assertx(false);
  return RegionMode::None;
}

std::string show(RegionMode mode) {
  switch (mode) {
    case RegionMode::None:     return "none";
    case RegionMode::Method:   return "method";
    case RegionMode::Tracelet: return "tracelet";
  }
}

}

//////////////////////////////////////////////////////////////////////

PGORegionMode pgoRegionMode(const Func& /*func*/) {
  auto& s = Cfg::Jit::PGORegionSelector;
  if (s == "hottrace") return PGORegionMode::Hottrace;
  if (s == "hotblock") return PGORegionMode::Hotblock;
  if (s == "hotcfg")   return PGORegionMode::HotCFG;
  if (s == "wholecfg") return PGORegionMode::WholeCFG;
  FTRACE(1, "unknown pgo region mode {}: using hottrace\n", s);
  assertx(false);
  return PGORegionMode::Hottrace;
}

//////////////////////////////////////////////////////////////////////

PostConditions& PostConditions::operator|=(const PostConditions& other) {
  using TypedLocation = RegionDesc::TypedLocation;
  struct State {
    Type type;
    bool changed;
  };
  jit::fast_map<Location,State,Location::Hash> locState;

  auto add = [&] (const TypedLocation& tl, bool isChanged) {
    State state{tl.type, isChanged};
    auto ins = locState.insert(std::pair<Location,State>(tl.location, state));
    if (ins.second) return;
    auto& it = ins.first;
    it->second.type    |= tl.type;
    it->second.changed |= isChanged;
  };

  for (auto& c : changed) add(c, true);
  for (auto& r : refined) add(r, false);
  for (auto& c : other.changed) add(c, true);
  for (auto& r : other.refined) add(r, false);

  changed.clear();
  refined.clear();
  for (auto& l : locState) {
    auto const& loc      = l.first;
    auto const type      = l.second.type;
    auto const isChanged = l.second.changed;
    TypedLocation typedLoc{loc, type};
    if (isChanged) changed.push_back(typedLoc);
    else           refined.push_back(typedLoc);
  }

  return *this;
}

//////////////////////////////////////////////////////////////////////

bool RegionDesc::empty() const {
  return m_blocks.empty();
}

RegionDesc::BlockPtr RegionDesc::entry() const {
  assertx(!empty());
  return m_blocks[0];
}

bool RegionDesc::isExit(BlockId bid) const {
  return succs(bid).empty();
}

SrcKey RegionDesc::start() const {
  assertx(!empty());
  return m_blocks[0]->start();
}

uint32_t RegionDesc::instrSize() const {
  uint32_t size = 0;
  for (auto& b : m_blocks) {
    size += b->length();
  }
  return size;
}

SrcKey RegionDesc::lastSrcKey() const {
  assertx(!empty());
  FuncId startFuncId = start().funcID();
  for (int i = m_blocks.size() - 1; i >= 0; i--) {
    SrcKey sk = m_blocks[i]->last();
    if (sk.funcID() == startFuncId) {
      return sk;
    }
  }
  always_assert(0);
}


RegionDesc::Block* RegionDesc::addBlock(SrcKey      sk,
                                        int         length,
                                        SBInvOffset spOffset) {
  m_blocks.push_back(
    std::make_shared<Block>(kInvalidTransID, sk, length, spOffset));
  BlockPtr block = m_blocks.back();
  m_data[block->id()] = BlockData(block);
  return block.get();
}

void RegionDesc::addBlock(BlockPtr newBlock) {
  m_blocks.push_back(newBlock);
  m_data[newBlock->id()] = BlockData{newBlock};
}

void RegionDesc::replaceBlock(BlockId bid, BlockPtr newBlock) {
  auto oldBlock = block(bid);
  m_data[bid].block = newBlock;
  auto it = std::find(m_blocks.begin(), m_blocks.end(), oldBlock);
  assertx(it != m_blocks.end());
  *it = newBlock;
}

void RegionDesc::deleteBlock(BlockId bid) {
  auto it = std::find_if(m_blocks.begin(), m_blocks.end(),
                         [&](const BlockPtr b) { return b->id() == bid; });
  if (it != m_blocks.end()) deleteBlock(it);
}

RegionDesc::BlockVec::iterator
RegionDesc::deleteBlock(RegionDesc::BlockVec::iterator it) {
  const auto bid = (*it)->id();
  for (auto pid : preds(bid)) data(pid).succs.erase(bid);
  for (auto sid : succs(bid)) data(sid).preds.erase(bid);

  auto const prevRetr = data(bid).prevRetransId;
  auto const nextRetr = data(bid).nextRetransId;
  if (prevRetr != kInvalidTransID) data(prevRetr).nextRetransId = nextRetr;
  if (nextRetr != kInvalidTransID) data(nextRetr).prevRetransId = prevRetr;

  m_data.erase(bid);
  return m_blocks.erase(it);
}

const RegionDesc::BlockVec& RegionDesc::blocks() const {
  return m_blocks;
}

RegionDesc::BlockData& RegionDesc::data(BlockId id) {
  auto it = m_data.find(id);
  always_assert_flog(it != m_data.end(),
                     "BlockId {} doesn't exist in m_data", id);
  return it->second;
}

bool RegionDesc::hasBlock(BlockId id) const {
  return m_data.contains(id);
}

RegionDesc::BlockPtr RegionDesc::block(BlockId id) const {
  return data(id).block;
}

const RegionDesc::BlockIdSet& RegionDesc::succs(BlockId id) const {
  return data(id).succs;
}

const RegionDesc::BlockIdSet& RegionDesc::preds(BlockId id) const {
  auto const& data = this->data(id);
  if (data.hasIncoming) {
    assertx(data.succs.empty());
    return data.succs;
  }
  return data.preds;
}

const RegionDesc::BlockIdSet* RegionDesc::incoming() const {
  auto const& data = this->data(entry()->id());
  return data.hasIncoming ? &data.preds : nullptr;
}

void RegionDesc::incoming(RegionDesc::BlockIdSet&& ids) {
  auto& data = this->data(entry()->id());
  assertx(data.succs.empty());
  assertx(data.preds.empty());
  assertx(!data.hasIncoming);
  data.hasIncoming = true;
  data.preds = std::move(ids);
}

const RegionDesc::BlockIdSet& RegionDesc::merged(BlockId id) const {
  return data(id).merged;
}

Optional<RegionDesc::BlockId> RegionDesc::idom(BlockId id) const {
  auto const idom = data(id).idom;
  if (idom == kInvalidTransID) return std::nullopt;
  return idom;
}

uint32_t RegionDesc::rpoId(BlockId id) const {
  assertx(data(id).rpoId != std::numeric_limits<uint32_t>::max());
  return data(id).rpoId;
}

Optional<RegionDesc::BlockId> RegionDesc::prevRetrans(BlockId id) const {
  auto const prev = data(id).prevRetransId;
  if (prev == kInvalidTransID) return std::nullopt;
  return prev;
}

Optional<RegionDesc::BlockId> RegionDesc::nextRetrans(BlockId id) const {
  auto const next = data(id).nextRetransId;
  if (next == kInvalidTransID) return std::nullopt;
  return next;
}

void RegionDesc::setNextRetrans(BlockId id, BlockId next) {
  assertx(data(id).nextRetransId == kInvalidTransID);
  assertx(data(next).prevRetransId == kInvalidTransID);
  data(id).nextRetransId = next;
  data(next).prevRetransId = id;
}

void RegionDesc::clearNextRetrans(BlockId id) {
  data(id).nextRetransId = kInvalidTransID;
}

void RegionDesc::clearPrevRetrans(BlockId id) {
  data(id).prevRetransId = kInvalidTransID;
}

void RegionDesc::addArc(BlockId srcId, BlockId dstId) {
  data(srcId).succs.insert(dstId);
  data(dstId).preds.insert(srcId);
}

void RegionDesc::removeArc(BlockId srcID, BlockId dstID) {
  data(srcID).succs.erase(dstID);
  data(dstID).preds.erase(srcID);
}

void RegionDesc::addMerged(BlockId fromId, BlockId intoId) {
  data(intoId).merged.insert(fromId);
}

int64_t RegionDesc::blockProfCount(RegionDesc::BlockId bid) const {
  const auto pd = profData();
  if (pd == nullptr) return 1;
  if (bid < 0) return 1;
  assertx(bid < pd->numTransRecs());
  const auto tr = pd->transRec(bid);
  if (tr->kind() != TransKind::Profile) return 1;
  int64_t total = pd->transCounter(bid);
  for (auto mid : merged(bid)) {
    total += pd->transCounter(mid);
  }
  return int64_t(total * blockProfCountScale(bid));
}

double RegionDesc::blockProfCountScale(RegionDesc::BlockId bid) const {
  return data(bid).profCountScale;
}

void RegionDesc::setBlockProfCountScale(RegionDesc::BlockId bid, double scale) {
  data(bid).profCountScale = scale;
}

void RegionDesc::renumberBlock(BlockId oldId, BlockId newId) {
  assertx( hasBlock(oldId));
  assertx(!hasBlock(newId));

  block(oldId)->setID(newId);
  m_data[newId] = m_data[oldId];
  m_data.erase(oldId);

  // Fix predecessor sets for the successors.
  for (auto succId : m_data[newId].succs) {
    BlockIdSet& succPreds = m_data[succId].preds;
    assertx(succPreds.contains(oldId));
    succPreds.erase(oldId);
    succPreds.insert(newId);
  }

  // Fix successor sets for the predecessors.
  for (auto predId : preds(newId)) {
    BlockIdSet& predSuccs = m_data[predId].succs;
    assertx(predSuccs.contains(oldId));
    predSuccs.erase(oldId);
    predSuccs.insert(newId);
  }
}

void RegionDesc::copyArcsFrom(const RegionDesc& srcRegion) {
  for (auto const& b : srcRegion.m_blocks) {
    auto bid = b->id();
    for (auto succId : srcRegion.succs(bid)) {
      addArc(bid, succId);
    }
  }
}

void RegionDesc::copyBlocksFrom(const RegionDesc&  other,
                                BlockVec::iterator where) {
  auto otherBlocks = other.blocks();
  m_blocks.insert(where, otherBlocks.begin(), otherBlocks.end());
  for (auto b : otherBlocks) {
    m_data[b->id()] = BlockData(b);
  }
}

void RegionDesc::append(const RegionDesc& other) {
  copyBlocksFrom(other, m_blocks.end());
  copyArcsFrom(other);
}

void RegionDesc::prepend(const RegionDesc& other) {
  copyBlocksFrom(other, m_blocks.begin());
  copyArcsFrom(other);
}

/*
 * Perform a DFS starting at block `bid', storing the post-order in
 * `outVec'.
 */
void RegionDesc::postOrderSort(RegionDesc::BlockId     bid,
                               RegionDesc::BlockIdSet& visited,
                               RegionDesc::BlockIdVec& outVec) {
  if (visited.contains(bid)) return;
  visited.insert(bid);

  if (auto nextRetr = nextRetrans(bid)) {
    postOrderSort(nextRetr.value(), visited, outVec);
  }
  for (auto succ : succs(bid)) {
    postOrderSort(succ, visited, outVec);
  }
  outVec.push_back(bid);
}

/**
 * Sort the m_blocks vector in reverse post order.  This enforces that
 * m_blocks will be a topological order in case the region is acyclic.
 * All region arcs are taken into account, including retranslation arcs.
 */
void RegionDesc::sortBlocks() {
  RegionDesc::BlockIdSet visited;
  RegionDesc::BlockIdVec reverse;

  auto const entryId = entry()->id();
  for (auto& block : m_blocks) {
    if (block->start() == start()) {
      postOrderSort(block->id(), visited, reverse);
    }
  }
  assertx(m_blocks.size() >= reverse.size());

  // Remove unreachable blocks from `m_data'.
  for (auto it = m_blocks.begin(); it != m_blocks.end();) {
    auto bid = (*it)->id();
    if (!visited.contains(bid)) {
      it = deleteBlock(it);
    } else {
      it++;
    }
  }

  // Update `m_blocks' vector, making sure that entryId remains the first one.
  m_blocks.clear();
  m_blocks.push_back(block(entryId));
  data(entryId).rpoId = 0;
  auto size = reverse.size();
  for (size_t i = 0; i < size; i++) {
    auto const id = reverse[size - i - 1];
    if (id == entryId) continue;
    data(id).rpoId = m_blocks.size();
    m_blocks.push_back(block(id));
  }
  always_assert_flog(
    entryId == entry()->id(),
    "sortBlocks() changed region entry: entryId ({}) != entry()->id() ({})",
    entryId, entry()->id()
  );
}

void RegionDesc::initRpoIds() {
  for (size_t i = 0; i < m_blocks.size(); ++i) {
    data(m_blocks[i]->id()).rpoId = i;
  }
}

void RegionDesc::findDominators() {
  assertx(!m_blocks.empty());

  auto constexpr kUnknown = -2;
  auto constexpr kEntry = -1;

  // Entry block has a special "outside of the region" kEntry dominator.
  // All other blocks are unknown.
  auto idoms = std::vector<int32_t>(m_blocks.size(), kUnknown);
  idoms[0] = kEntry;

  for (auto changed = true; changed; ) {
    changed = false;

    for (auto i = 1; i < idoms.size(); ++i) {
      auto const bid = m_blocks[i]->id();
      assertx(entry() != m_blocks[i]);

      auto idom = kUnknown;
      auto const update = [&](BlockId pred) {
        auto rpoId = data(pred).rpoId;
        if (idoms[rpoId] == kUnknown) return;
        if (idom == kUnknown) {
          idom = rpoId;
          return;
        }
        while (rpoId != idom) {
          while (rpoId < idom) idom = idoms[idom];
          while (rpoId > idom) rpoId = idoms[rpoId];
        }
      };

      if (auto const prevRetr = prevRetrans(bid)) update(*prevRetr);
      for (auto pred : preds(bid)) update(pred);
      assertx(idom != kUnknown);

      if (idoms[i] != idom) {
        idoms[i] = idom;
        changed = true;
      }
    }
  }

  for (auto i = 0; i < idoms.size(); ++i) {
    data(m_blocks[i]->id()).idom = idoms[i] == kEntry
      ? kInvalidTransID
      : m_blocks[idoms[i]]->id();
  }
}

namespace {

struct Chain {
  size_t id;
  jit::vector<RegionDesc::BlockId> blocks;
};

using BlockToChainMap = hphp_hash_map<RegionDesc::BlockId, size_t>;

void mergeChains(Chain& dst, Chain& src, BlockToChainMap& b2c) {
  if (dst.id == src.id) return;
  dst.blocks.insert(dst.blocks.end(), src.blocks.begin(), src.blocks.end());
  for (auto bid : src.blocks) {
    b2c[bid] = dst.id;
  }
  src.blocks.clear();
}

RegionDesc::BlockId findFirstInSet(const Chain& c, RegionDesc::BlockIdSet s) {
  for (auto bid : c.blocks) {
    if (s.contains(bid)) return bid;
  }
  always_assert(0);
}

/**
 * Returns true iff all the block ids in `chains' are in the `validBlockIds'
 * set.
 */
bool chainsAreValid(const jit::vector<Chain>& chains,
                    const jit::hash_set<RegionDesc::BlockId>& validBlockIds) {
  for (auto& chain : chains) {
    for (auto bid : chain.blocks) {
      if (validBlockIds.find(bid) == validBlockIds.end()) {
        return false;
      }
    }
  }
  return true;
}

std::string show(const jit::vector<Chain>& chains) {
  std::string ret;
  for (auto& chain : chains) {
    fmt::format_to(std::back_inserter(ret), "[{}]: {}\n", chain.id, folly::join(",", chain.blocks));
  }
  return ret;
}

}

/**
 * Chain the retranslation blocks.  This method enforces that, for
 * each region block, all its successor have distinct SrcKeys.
 */
void RegionDesc::chainRetransBlocks() {
  jit::vector<Chain> chains;
  BlockToChainMap block2chain;
  SCOPE_ASSERT_DETAIL("RegionDesc::chainRetransBlocks") { return show(*this); };
  jit::hash_set<RegionDesc::BlockId> blockIds;
  jit::hash_map<RegionDesc::BlockId, int64_t> blockWgt;

  auto profData = jit::profData();

  // 1. Initially assign each region block to its own chain.
  const auto entryBid = entry()->id();
  RegionDesc::BlockId entryCid = -1;
  for (auto b : blocks()) {
    auto bid = b->id();
    auto cid = chains.size();
    chains.push_back({cid, {bid}});
    block2chain[bid] = cid;
    blockIds.insert(bid);
    always_assert(hasTransID(bid));
    blockWgt[bid] = profData->transCounter(bid);
    if (bid == entryBid) entryCid = cid;
  }
  always_assert_flog(chainsAreValid(chains, blockIds), "{}", show(chains));

  // 2. Merge all blocks with the same SrcKey as the entry into the entry's
  //    chain.
  for (auto b : blocks()) {
    if (b->id() == entryBid) continue;
    if (b->start() == entry()->start()) {
      const auto cid = block2chain[b->id()];
      mergeChains(chains[entryCid], chains[cid], block2chain);
    }
  }

  // 3. For each block, if it has 2 successors with the same SrcKey,
  //    then merge the successors' chains into one.
  for (auto b : blocks()) {
    auto bid = b->id();
    const auto& succSet = succs(bid);
    for (auto it1 = succSet.begin(); it1 != succSet.end(); it1++) {
      auto bid1 = *it1;
      auto cid1 = block2chain[bid1];
      for (auto it2 = it1 + 1; it2 != succSet.end(); it2++) {
        auto bid2 = *it2;
        auto cid2 = block2chain[bid2];
        if (data(bid1).block->start() == data(bid2).block->start()) {
          mergeChains(chains[cid1], chains[cid2], block2chain);
        }
      }
    }
  }
  always_assert_flog(chainsAreValid(chains, blockIds), "{}", show(chains));

  // 4. Sort each chain.  In general, we want to sort each chain in
  //    decreasing order of profile weights.  However, note that this
  //    transformation can turn acyclic graphs into cyclic ones (see
  //    example below).
  //
  //    Here's an example showing how an acyclic CFG can become cyclic
  //    by chaining its retranslation blocks:
  //
  //      - Region before chaining retranslation blocks, where B2' and B2"
  //        are retranslations starting at the same SrcKey:
  //          B1  -> B2'
  //          B1  -> B2"
  //          B2' -> B3
  //          B3  -> B2"
  //
  //      - Region after sorting the chain as B2" -R-> B2':
  //          B1  ->   B2"
  //          B2" -R-> B2'
  //          B2' ->   B3
  //          B3  ->   B2"
  //        Note the cycle: B2" -R-> B2' -> B3 -> B2".
  //
  auto cmpBlocks = [&](RegionDesc::BlockId bid1, RegionDesc::BlockId bid2) {
    const auto isEntry1 = bid1 == entryBid;
    const auto isEntry2 = bid2 == entryBid;
    if (isEntry1 != isEntry2) return isEntry1;
    return blockWgt[bid1] > blockWgt[bid2];
  };

  TRACE(1, "chainRetransBlocks: computed chains:\n");
  for (auto& c : chains) {
    std::sort(c.blocks.begin(), c.blocks.end(), cmpBlocks);

    if (Trace::moduleEnabled(Trace::region, 1) && c.blocks.size() > 0) {
      FTRACE(1, "  -> {} (w={})", c.blocks[0], blockWgt[c.blocks[0]]);
      for (size_t i = 1; i < c.blocks.size(); i++) {
        FTRACE(1, ", {} (w={})", c.blocks[i], blockWgt[c.blocks[i]]);
      }
      FTRACE(1, "\n");
    }
  }
  always_assert_flog(chainsAreValid(chains, blockIds), "{}", show(chains));

  // 5. Set the nextRetrans blocks according to the computed chains.
  for (auto& c : chains) {
    if (c.blocks.size() == 0) continue;
    for (size_t i = 0; i < c.blocks.size() - 1; i++) {
      setNextRetrans(c.blocks[i], c.blocks[i + 1]);
    }
  }

  // 6. For each block with multiple successors in the same chain,
  //    only keep the successor that first appears in the chain.
  BlockIdSet erased_ids;
  for (auto b : blocks()) {
    auto& succSet = data(b->id()).succs;
    for (auto s : succSet) {
      if (erased_ids.contains(s)) continue;
      auto& c = chains[block2chain[s]];
      auto selectedSucc = findFirstInSet(c, succSet);
      for (auto other : c.blocks) {
        if (other == selectedSucc) continue;
        // You can't erase from a flat_set while iterating it, so track
        // the ids we erased here.
        if (erased_ids.insert(other).second) {
          data(other).preds.erase(b->id());
        }
      }
    }
    for (auto id : erased_ids) succSet.erase(id);
    erased_ids.clear();
  }

  ITRACE(
    3,
    "selectHotCFG: before sortBlocks at the end of chainRetransBlocks:\n{}\n",
    show(*this)
  );

  // 7. Reorder the blocks in the region in topological order (if
  //    region is acyclic), since the previous steps may break it.
  sortBlocks();
}

std::string RegionDesc::toString() const {
  auto ret = show(*this);
  ret += "data:\n";
  for (auto d : m_data) {
    ret += fmt::format("  block id: {}\n", d.first);
  }
  return ret;
}

//////////////////////////////////////////////////////////////////////

/*
 * We assign unique negative ID's to all new blocks---these correspond to
 * invalid TransIDs.  To maintain this property, we have to start one past
 * the sentinel kInvalidTransID, which is -1.
 */
static std::atomic<RegionDesc::BlockId> s_nextId{-2};

TransID getTransID(RegionDesc::BlockId blockId) {
  assertx(hasTransID(blockId));
  return TransID(blockId);
}

bool hasTransID(RegionDesc::BlockId blockId) {
  return blockId >= 0;
}

RegionDesc::Block::Block(BlockId     id,
                         SrcKey      start,
                         int         length,
                         SBInvOffset initSpOff)
  : m_start(start)
  , m_last(SrcKey{})
  , m_length(length)
  , m_initialSpOffset(initSpOff)
  , m_profTransID(kInvalidTransID)
{
  if (id == kInvalidTransID) {
    m_id = s_nextId.fetch_sub(1, std::memory_order_acq_rel);
  } else {
    m_id = id;
    while (true) {
      auto expected = s_nextId.load(std::memory_order_acquire);
      if (id > expected) break;
      if (s_nextId.compare_exchange_weak(expected, id - 1,
                                         std::memory_order_acq_rel)) {
        break;
      }
    }
  }

  assertx(length >= 0);
  if (length > 0) {
    SrcKey sk = start;
    for (unsigned i = 1; i < length; ++i) sk.advance();
    m_last = sk;
  }
  checkInstructions();
  checkMetadata();
}

void RegionDesc::Block::addInstruction() {
  if (m_length > 0) checkInstruction(last());
  assertx((m_last.valid()) == (m_length != 0));

  ++m_length;
  if (m_length == 1) {
    m_last = m_start;
  } else {
    m_last.advance();
  }
}

void RegionDesc::Block::truncateAfter(SrcKey final) {
  if (final == m_last) return;
  auto skIter = start();
  int newLen = -1;
  for (int i = 0; i < m_length; ++i, skIter.advance(func())) {
    if (skIter == final) {
      newLen = i + 1;
      break;
    }
  }
  assertx(newLen != -1);
  FTRACE(1, "selectTracelet truncating block after offset {}:\n{}\n",
          final.offset(), show(*this));
  m_length = newLen;
  m_last = final;

  checkInstructions();
  checkMetadata();
}

void RegionDesc::Block::addPreCondition(const GuardedLocation& locGuard) {
  FTRACE(2, "Block::addPreCondition({})\n", show(locGuard));
  assertx(locGuard.type != TBottom);
  assertx(locGuard.type <= TCell);
  assertx(locGuard.type.isSpecialized() ||
          typeFitsConstraint(locGuard.type, locGuard.category));
  m_typePreConditions.push_back(locGuard);
}

void RegionDesc::Block::setPostConds(const PostConditions& conds) {
  m_postConds = conds;
}

void RegionDesc::Block::clearPreConditions() {
  m_typePreConditions.clear();
}

/*
 * Check invariants about the bytecode instructions in this Block.
 *
 * 1. Single entry, single exit (aside from exceptions).  I.e. no
 *    non-fallthrough instructions mid-block and no control flow (not
 *    counting calls as control flow).
 *
 */
void RegionDesc::Block::checkInstructions() const {
  if (!debug || length() == 0) return;

  auto sk = start();

  for (int i = 1; i < length(); ++i) {
    if (i != length() - 1) checkInstruction(sk);
    sk.advance(func());
  }
  assertx(sk == m_last);
}

void RegionDesc::Block::checkInstruction(SrcKey sk) const {
  if (sk.anyFuncEntry()) return;

  if (instrFlags(sk.op()) & TF) {
    FTRACE(1, "Bad block: {}\n", show(*this));
    assertx(false && "Block may not contain non-fallthrough instruction unless "
                     "they are last");
  }
  if (instrIsNonCallControlFlow(sk.op())) {
    FTRACE(1, "Bad block: {}\n", show(*this));
    assertx(false && "Block may not contain control flow instructions unless "
                     "they are last");
  }
}

/*
 * Check invariants about the metadata for this Block.
 *
 * 1. Each SrcKey in m_preConditions is within the bounds of the block.
 *
 * 2. Each local id referred to in the type pre-conditions list is valid.
 *
 * 3. (Unchecked) each stack offset in the type pre-conditions list is valid.
*/
void RegionDesc::Block::checkMetadata() const {
  auto checkTypedLocations = [&](const char* /*msg*/,
                                 const TypedLocations& vec) {
    for (auto& typedLoc : vec) {
      auto& loc = typedLoc.location;
      switch (loc.tag()) {
        case LTag::Local:
          assertx(loc.localId() < func()->numLocals());
          break;
        case LTag::Stack:
        case LTag::MBase:
          break;
      }
    }
  };

  auto checkGuardedLocations = [&](const char* /*msg*/,
                                   const GuardedLocations& vec) {
    for (auto& guardedLoc : vec) {
      assertx(guardedLoc.type.isSpecialized() ||
              typeFitsConstraint(guardedLoc.type, guardedLoc.category));
      auto& loc = guardedLoc.location;
      switch (loc.tag()) {
        case LTag::Local:
          assertx(loc.localId() < func()->numLocals());
          break;
        case LTag::Stack:
        case LTag::MBase:
          break;
      }
    }
  };

  checkTypedLocations("changed post-conditions", m_postConds.changed);
  checkTypedLocations("refined post-conditions", m_postConds.refined);
  checkGuardedLocations("type precondition", m_typePreConditions);
}

RegionDescPtr selectRegion(const RegionContext& context,
                           TransKind kind) {
  auto const mode = regionMode();

  FTRACE(1,
    "Select region: mode={} context:\n{}",
    show(mode), show(context)
  );

  auto region = [&]{
    try {
      switch (mode) {
        case RegionMode::None:
          return RegionDescPtr{nullptr};
        case RegionMode::Method:
          return selectMethod(context);
        case RegionMode::Tracelet: {
          auto const maxBCInstrs = kind == TransKind::Live
            ? Cfg::Jit::MaxLiveRegionInstrs
            : Cfg::Jit::MaxRegionInstrs;
          return selectTracelet(context, kind, maxBCInstrs);
        }
      }
      not_reached();
    } catch ([[maybe_unused]] const FailedIRGen& e) {
      FTRACE(1, "region selector threw: {}\n", e.what());
      return RegionDescPtr{nullptr};
    } catch ([[maybe_unused]] const ResourceExceededException& e) {
      FTRACE(1, "region selector fataled: {}\n", e.what());
      return RegionDescPtr{nullptr};
    } catch (const std::exception& e) {
      always_assert_flog(
        false, "region selector threw unexpected: {}\n", e.what()
      );
    }
  }();

  if (region) {
    FTRACE(3, "{}", show(*region));
    always_assert(
      region->instrSize() <= std::max(Cfg::Jit::MaxRegionInstrs,
                                      Cfg::Jit::MaxLiveRegionInstrs)
    );
  } else {
    FTRACE(1, "no region selectable; using tracelet compiler\n");
  }

  return region;
}

//////////////////////////////////////////////////////////////////////

static bool postCondMismatch(const RegionDesc::TypedLocation& postCond,
                             const RegionDesc::GuardedLocation& preCond) {
  return postCond.location == preCond.location &&
         !preCond.type.maybe(postCond.type);
}

bool preCondsAreSatisfied(const RegionDesc::BlockPtr& block,
                          const TypedLocations& prevPostConds) {
  const auto& preConds = block->typePreConditions();
  for (const auto& preCond : preConds) {
    for (const auto& post : prevPostConds) {
      if (postCondMismatch(post, preCond)) {
        FTRACE(6, "preCondsAreSatisfied: postcondition check failed!\n"
               "  postcondition was {}, precondition was {}\n",
               show(post), show(preCond));
        return false;
      }
    }
  }
  return true;
}

bool breaksRegion(SrcKey sk) {
  if (sk.anyFuncEntry()) return false;
  switch (sk.op()) {
    case Op::SSwitch:
    case Op::CreateCont:
    case Op::Yield:
    case Op::YieldK:
    case Op::RetC:
    case Op::RetM:
    case Op::RetCSuspended:
    case Op::Exit:
    case Op::Fatal:
    case Op::Throw:
    case Op::Eval:
    case Op::NativeImpl:
    case Op::ThrowNonExhaustiveSwitch:
    case Op::StaticAnalysisError:
      return true;

    case Op::Await:
    case Op::AwaitAll:
    case Op::AwaitLowPri:
      // We break regions at resumed Await instructions, to avoid
      // duplicating the translation of the resumed SrcKey after the
      // Await.
      return sk.resumeMode() == ResumeMode::Async;

    default:
      return false;
  }
}

//////////////////////////////////////////////////////////////////////

namespace {

struct DFSWalker {
  explicit DFSWalker(const RegionDesc& region)
    : m_region(region) { }

  void walk(RegionDesc::BlockId id) {
    if (m_visited.contains(id)) return;
    m_visited.insert(id);

    if (auto nextRetrans = m_region.nextRetrans(id)) {
      walk(nextRetrans.value());
    }
    for (auto succ : m_region.succs(id)) {
      walk(succ);
    }
  }

  size_t numVisited() const { return m_visited.size(); }

 private:
  const RegionDesc&      m_region;
  RegionDesc::BlockIdSet m_visited;
};

}

/*
 * Checks if the given region is well-formed, which entails the
 * following properties:
 *
 *   1) The region has at least one block.
 *
 *   2) Each block in the region has a different id.
 *
 *   3) All arcs involve blocks within the region.
 *
 *   4) For each arc, the bytecode offset of the dst block must
 *      possibly follow the execution of the src block.
 *
 *   5) Each block contains at most one successor corresponding to a
 *      given SrcKey.
 *
 *   7) All blocks are reachable from the entry block.
 *
 *   8) For each block, there must be a path from the entry to it that
 *      includes only earlier blocks in the region.
 *
 *  10) The block-retranslation chains cannot have cycles.
 *
 *  11) All successors and predecessors sets are consistent (i.e., if
 *      B is a successor of A, then A is a predecessor of B).  This
 *      also applies to nextRetrans and prevRetrans.
 */
bool check(const RegionDesc& region, std::string& error) {

  auto bad = [&](const std::string& errorMsg) {
    error = errorMsg;
    return false;
  };

  // 1) The region has at least one block.
  if (region.empty()) return bad("empty region");

  RegionDesc::BlockIdSet blockSet;
  for (auto b : region.blocks()) {
    auto bid = b->id();
    // 2) Each block in the region has a different id.
    if (blockSet.contains(bid)) {
      return bad(fmt::format("many blocks with id {}", bid));
    }
    blockSet.insert(bid);
  }

  for (auto b : region.blocks()) {
    auto bid = b->id();
    SrcKey    lastSk = region.block(bid)->last();
    SrcKey::Set validSuccSrcKeys = lastSk.succSrcKeys();
    SrcKey::Set succSrcKeys;

    for (auto succ : region.succs(bid)) {
      SrcKey succSk = region.block(succ)->start();

      // 3) All arcs involve blocks within the region.
      if (!blockSet.contains(succ)) {
        return bad(fmt::format("arc with dst not in the region: {} -> {}",
                                  bid, succ));
      }

      // 11) Successors and predecessors sets are consistent.
      if (!region.preds(succ).contains(bid)) {
        return bad(fmt::format("arc missing in succ's pred set: {} -> {}",
                                  bid, succ));
      }

      // Checks 4) and 5) below don't make sense for arcs corresponding
      // to inlined calls and returns, so skip them in such cases.
      // This won't be possible once task #4076399 is done.
      if (lastSk.func() != succSk.func()) continue;

      // 4) For each arc, the bytecode offset of the dst block must
      //    possibly follow the execution of the src block.
      if (!validSuccSrcKeys.contains(succSk)) {
        return bad(fmt::format("arc with impossible control flow: {} -> {}",
                                  bid, succ));
      }

      // 5) Each block contains at most one successor corresponding to a
      //    given SrcKey.
      if (succSrcKeys.contains(succSk)) {
        return bad(fmt::format("block {} has multiple successors with SK {}",
                                  bid, show(succSk)));
      }
      succSrcKeys.insert(succSk);
    }
    for (auto pred : region.preds(bid)) {
      if (!blockSet.contains(pred)) {
        return bad(fmt::format("arc with src not in the region: {} -> {}",
                                  pred, bid));
      }
      // 11) Successors and predecessors sets are consistent.
      if (!region.succs(pred).contains(bid)) {
        return bad(fmt::format("arc missing in pred's succ set: {} -> {}",
                                  pred, bid));
      }
    }
  }

  // 7) All blocks are reachable from the entry (first) block.
  DFSWalker dfsWalk(region);
  dfsWalk.walk(region.entry()->id());
  if (dfsWalk.numVisited() != blockSet.size()) {
    return bad("region has unreachable blocks");
  }

  // 8) is checked below.
  RegionDesc::BlockIdSet visited;
  auto& blocks = region.blocks();
  for (unsigned i = 0; i < blocks.size(); i++) {
    auto bid = blocks[i]->id();
    unsigned nVisited = 0;
    if (auto prevRetrans = region.prevRetrans(bid)) {
      nVisited += visited.count(prevRetrans.value());
    }
    for (auto pred : region.preds(bid)) {
      nVisited += visited.count(pred);
    }
    // 8) For each block, there must be a path from the entry to it that
    //    includes only earlier blocks in the region.
    if (nVisited == 0 && i != 0) {
      return bad(fmt::format("block {} appears before all its predecessors",
                                bid));
    }
    visited.insert(bid);
  }

  // 11) nextRetrans and prevRetrans are consistent.
  for (unsigned i = 0; i < blocks.size(); i++) {
    auto bid = blocks[i]->id();
    auto nextRetrans = region.nextRetrans(bid);
    if (nextRetrans) {
      auto nextRetransId = nextRetrans.value();
      auto nextPrevId = region.prevRetrans(nextRetransId);
      if (!nextPrevId || nextPrevId.value() != bid) {
        return bad(fmt::format("block {}'s nextRetrans (block {}) has non-"
                                  "matching prevRetrans", bid, nextRetransId));
      }
    }
    auto prevRetrans = region.prevRetrans(bid);
    if (prevRetrans) {
      auto prevRetransId = prevRetrans.value();
      auto prevNextId = region.nextRetrans(prevRetransId);
      if (!prevNextId || prevNextId.value() != bid) {
        return bad(fmt::format("block {}'s prevRetrans (block {}) has non-"
                                  "matching nextRetrans", bid, prevRetransId));
      }
    }
  }

  // 10) The block-retranslation chains cannot have cycles.
  for (auto b : blocks) {
    auto bid = b->id();
    RegionDesc::BlockIdSet chainSet;
    chainSet.insert(bid);
    while (auto next = region.nextRetrans(bid)) {
      auto nextId = next.value();
      if (chainSet.contains(nextId)) {
        return bad(fmt::format("cyclic retranslation chain for block {}",
                                  bid));
      }
      chainSet.insert(nextId);
      bid = nextId;
    }
  }

  return true;
}

//////////////////////////////////////////////////////////////////////

std::string show(RegionDesc::TypedLocation ta) {
  return fmt::format(
    "{} :: {}",
    show(ta.location),
    ta.type.toString()
  );
}

std::string show(const RegionDesc::GuardedLocation& guardedLoc) {
  return fmt::format(
    "{} :: {} ({})",
    show(guardedLoc.location),
    guardedLoc.type.toString(),
    typeCategoryName(guardedLoc.category)
  );
}

std::string show(const GuardedLocations& guardedLocVec) {
  std::string ret;
  for (auto& guardedLoc : guardedLocVec) {
    fmt::format_to(std::back_inserter(ret), "{} ; ", show(guardedLoc));
  }
  return ret;
}

std::string show(const PostConditions& pconds) {
  std::string ret;
  for (const auto& postCond : pconds.changed) {
    folly::toAppend("  changed postcondition: ", show(postCond), "\n", &ret);
  }
  for (const auto& postCond : pconds.refined) {
    folly::toAppend("  refined postcondition: ", show(postCond), "\n", &ret);
  }
  return ret;
}

std::string show(const RegionContext& ctx) {
  std::string ret;
  folly::toAppend(show(ctx.sk), "\n", &ret);
  for (auto& t : ctx.liveTypes) folly::toAppend(" ", show(t), "\n", &ret);

  return ret;
}

std::string show(const RegionDesc::Block& b) {
  std::string ret{"Block "};
  folly::toAppend(b.id(), ' ', showShort(b.start()),
                  " length ", b.length(),
                  " initSpOff ", b.initialSpOffset().offset,
                  " profTransID ", b.profTransID(),
                  '\n',
                  &ret
                 );

  auto& preconditions = b.typePreConditions();
  auto  skIter        = b.start();

  for (auto const& p : preconditions) {
    folly::toAppend("  precondition: ", show(p), "\n", &ret);
  }

  for (int i = 0; i < b.length(); ++i) {
    folly::toAppend(
      "    ",
      skIter.printableOffset(),
      "  ",
      skIter.showInst(),
      "\n",
      &ret
    );
    skIter.advance(b.func());
  }

  folly::toAppend(show(b.postConds()), &ret);

  return ret;
}

std::string show(const RegionDesc& region) {
  std::string ret{fmt::format("Region ({} blocks):\n",
                                 region.blocks().size())};

  auto profData = jit::profData();

  auto weight = [&] (RegionDesc::BlockPtr b) -> int64_t {
    if (!profData) return 0;
    auto tid = b->profTransID();
    if (tid == kInvalidTransID) return 0;
    return region.blockProfCount(tid);
  };

  uint64_t maxBlockWgt = 1; // avoid div by 0

  // Print contents of all blocks in pure text format.
  for (auto& b : region.blocks()) {
    folly::toAppend(show(*b), &ret);
    auto w = weight(b);
    if (w > maxBlockWgt) maxBlockWgt = w;
  }

  // Print CFG in dot format, coloring the blocks based on hotness.
  // Print all the blocks first.
  folly::toAppend("\ndigraph RegionCFG {\n node[shape=box,style=filled]\n",
                  &ret);
  for (auto& b : region.blocks()) {
    auto const id = b->id();
    auto const& mergedSet = region.merged(id);
    std::string mergedStr = mergedSet.empty() ? "" :
                            (" (" + folly::join(",", mergedSet) + ")");
    uint32_t coldness = 255 - (255 * weight(b) / maxBlockWgt);
    fmt::format_to(std::back_inserter(ret), " \"B{}\" [label=\"B {}{}\\np: {}\","
                  "fillcolor=\"#ff{:02x}{:02x}\"]\n",
                  id, id, mergedStr, weight(b), coldness, coldness);
  }

  // Print arcs in dot format.
  for (auto& b : region.blocks()) {
    if (auto r = region.nextRetrans(b->id())) {
      folly::toAppend(fmt::format(" \"B{}\" -> \"B{}\" [label=R,color=red]\n",
                                    b->id(), r.value()), &ret);
    }
    for (auto s : region.succs(b->id())) {
      folly::toAppend(fmt::format(" \"B{}\" -> \"B{}\"\n", b->id(), s),
                      &ret);
    }
  }

  ret += "}\n";

  return ret;
}

//////////////////////////////////////////////////////////////////////

}
