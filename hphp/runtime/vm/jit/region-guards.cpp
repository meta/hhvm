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

#include <string>
#include <vector>

#include <fmt/format.h>
#include <folly/Hash.h>

#include "hphp/util/assertions.h"
#include "hphp/util/configs/jit.h"
#include "hphp/util/trace.h"

#include "hphp/runtime/vm/jit/containers.h"
#include "hphp/runtime/vm/jit/guard-constraint.h"
#include "hphp/runtime/vm/jit/location.h"
#include "hphp/runtime/vm/jit/prof-data.h"
#include "hphp/runtime/vm/jit/region-selection.h"
#include "hphp/runtime/vm/jit/type.h"

namespace HPHP { namespace jit {

TRACE_SET_MOD(pgo)

//////////////////////////////////////////////////////////////////////

namespace {

struct BlockData {
  RegionDesc::BlockId blockId;
  GuardedLocations    guards;
  jit::vector<Type>   origTypes;
  bool                relaxed; // block had some type guard relaxed
  bool                deleted; // block became unnecessary
  bool                merged; // another block was merged into this
  int64_t             weight;
};

using BlockDataVec = jit::vector<BlockData>;

/*
 * Find the retranslation-chain roots by selecting the blocks that are
 * part of a retranslation chain (determined by looking at the blocks
 * that are incident to retranslation arcs) that have a successor but
 * not a predecessor in the retranslation chain.
 */
RegionDesc::BlockIdVec findRetransChainRoots(const RegionDesc& region) {
  RegionDesc::BlockIdVec roots;

  for (auto block : region.blocks()) {
    auto bid = block->id();
    auto chainPrev = region.prevRetrans(bid);
    auto chainNext = region.nextRetrans(bid);
    if (!chainPrev && chainNext) {
      roots.push_back(bid);
    }
  }

  FTRACE(3, "findRetransChainRoots: {}\n", folly::join(", ", roots));
  return roots;
}

BlockDataVec createBlockData(const RegionDesc& region,
                             RegionDesc::BlockId rootId,
                             const ProfData& profData) {
  BlockDataVec data;
  auto bid = rootId;
  while (true) {
    const auto block = region.block(bid);
    auto weight = profData.transCounter(bid);
    data.push_back({ bid, block->typePreConditions(), jit::vector<Type>(),
                     false, false, false, weight });
    std::sort(data.back().guards.begin(), data.back().guards.end(),
              [&](const RegionDesc::GuardedLocation& gl1,
                  const RegionDesc::GuardedLocation& gl2) {
                return gl1.location < gl2.location;
              });
    for (auto& guard : data.back().guards) {
      data.back().origTypes.push_back(guard.type);
    }
    if (auto next = region.nextRetrans(bid)) {
      bid = next.value();
    } else {
      break;
    }
  }
  return data;
}

using LocationTypeWeights = jit::hash_map<
  Location,
  jit::hash_map<Type,int64_t>,
  Location::Hash
>;

LocationTypeWeights findLocationTypes(const BlockDataVec& blockData) {
  LocationTypeWeights map;
  for (auto& bd : blockData) {
    for (auto& guard : bd.guards) {
      map[guard.location][guard.type] += bd.weight;
    }
  }
  return map;
}

/*
 * We consider relaxation profitable if there's not a single dominating type
 * that accounts for Cfg::Jit::PGORelaxPercent or more of the time
 * during profiling.
 */
bool relaxIsProfitable(const jit::hash_map<Type,int64_t>& typeWeights,
                       Type                               guardType,
                       DataTypeCategory&                  guardCategory) {
  assertx(guardType <= TCell);
  auto relaxedType = relaxType(guardType, guardCategory);

  DEBUG_ONLY int64_t totalWgt   = 0; // sum of all the block weights
  int64_t relaxWgt   = 0; // total weight if we relax guardType w/ guardCategory
  int64_t noRelaxWgt = 0; // total weight if we don't relax guardType at all
  for (auto& typeWgt : typeWeights) {
    auto type = typeWgt.first;
    auto weight = typeWgt.second;
    assertx(type <= TCell);
    const bool fitsConstraint = guardCategory == DataTypeSpecialized
      ? type.isSpecialized()
      : typeFitsConstraint(type, guardCategory);
    totalWgt += weight;
    if (!fitsConstraint) continue;
    if (type <= guardType) noRelaxWgt += weight;
    if (relaxType(type, guardCategory) == relaxedType) relaxWgt += weight;
  }

  // Consider relaxing to the input guardCategory.
  auto const profitable =
    (noRelaxWgt * 100 < relaxWgt * Cfg::Jit::PGORelaxPercent);
  FTRACE(3,
         "relaxIsProfitable({}, {}): noRelaxWgt={} ; relaxWgt={} ; totalWgt={} "
         "=> {}\n",
         guardType, guardCategory, noRelaxWgt, relaxWgt, totalWgt, profitable);
  return profitable;
}

/*
 * This is the function that actually relaxes the guards of all the
 * blocks in `blockData' in case that's considered profitable
 * according to `relaxIsProfitable'.
 */
void relaxGuards(BlockDataVec& blockData) {
  auto locTypes = findLocationTypes(blockData);

  for (auto& bd : blockData) {
    for (auto& guard : bd.guards) {

      DataTypeCategory UNUSED origCategory = guard.category;
      if (!relaxIsProfitable(locTypes[guard.location],
                             guard.type, guard.category)) {
        FTRACE(2, "relaxGuards(Block {}): skipping {}: {} -- not profitable\n",
               bd.blockId, show(guard.location), guard.type,
               typeCategoryName(guard.category));
        continue;
      }

      auto oldType = guard.type;
      always_assert_flog(oldType <= TCell, "oldType = {}", oldType);
      guard.type = relaxType(guard.type, guard.category);
      if (oldType != guard.type) {
        bd.relaxed = true;
        FTRACE(2, "relaxGuards(Block {}): {}: {} => {} ({} => {})\n",
               bd.blockId, show(guard.location), oldType, guard.type,
               typeCategoryName(origCategory),
               typeCategoryName(guard.category));
      }
    }
  }
}

/*
 * Determine whether two blocks are equivalent, which entails:
 *
 *  1) Their type guards are identical after relaxation;
 *  2) Their sets of successor region blocks are identical.
 */
bool equivalent(const BlockData&  bd1,
                const BlockData&  bd2,
                const RegionDesc& region) {

  // 1) Compare type guards
  const auto& guards1 = bd1.guards;
  const auto& guards2 = bd2.guards;
  if (guards1 != guards2) return false;

  auto const block1 = region.block(bd1.blockId);
  auto const block2 = region.block(bd2.blockId);

  // 2) Compare the sets of successor blocks
  if (region.succs(bd1.blockId) != region.succs(bd2.blockId)) return false;

  return true;
}

/*
 * This function goes through all the blocks' `data' and marks as
 * deleted the ones that are equivalent (per the `equivalent' function
 * above) to another block after relaxing the guards.  During this
 * process, the blocks that are deleted get "merged" with their
 * equivalent ones in the `region'.
 */
void removeDuplicates(BlockDataVec& data, RegionDesc& region) {
  const auto entryId = region.entry()->id();
  for (size_t i = 0; i < data.size(); i++) {
    for (size_t j = 0; j < i; j++) {
      if (!data[j].deleted && equivalent(data[i], data[j], region)) {
        // Be careful not to delete the region's entry block.
        const auto d = data[i].blockId == entryId ? j : i;
        const auto m = i + j - d;
        assertx(data[d].blockId != entryId);
        assertx(!data[m].deleted);
        data[d].deleted = true;
        data[m].merged  = true;
        region.addMerged(data[d].blockId, data[m].blockId);
        if (data[d].merged) {
          for (auto mid : region.merged(data[d].blockId)) {
            region.addMerged(mid, data[m].blockId);
          }
        }
        FTRACE(2, "removeDuplicates(): merging Block {} into Block {}\n",
               data[d].blockId, data[m].blockId);
        break;
      }
    }
  }
}

/*
 * This step unrelaxes guards that were relaxed but whose block
 * couldn't be merged with any other block.
 */
void unrelaxGuards(BlockDataVec& blockData) {
  for (auto& bd : blockData) {
    if (bd.merged || bd.deleted) continue;
    for (size_t g = 0; g < bd.guards.size(); g++) {
      const auto currType = bd.guards[g].type;
      const auto origType = bd.origTypes[g];
      if (currType != origType) {
        FTRACE(2, "unrelaxGuards(Block {}): {}: {} => {} ({})\n",
               bd.blockId, show(bd.guards[g].location), currType, origType,
               typeCategoryName(bd.guards[g].category));
        assertx(origType < currType);
        bd.guards[g].type = origType;
      }
    }
  }
}

/*
 * Returns whether all the type guards for `block' are satisfied by 'bd' and
 * its corresponding block.
 */
bool passesGuards(const RegionDesc& region, const RegionDesc::Block& block,
                  const BlockData& bd) {
  if (bd.blockId == block.id()) return true;

  // Check the type guards.  For simplicity, for now we return `false'
  // if the sets of guarded locations are different.
  const auto& preConds  = block.typePreConditions();
  const auto& newGuards = bd.guards;
  FTRACE(5, "passesGuards():\n   preConds = {}\n   newGuards = {}\n",
         show(preConds), show(newGuards));
  if (preConds.size() != newGuards.size()) {
    FTRACE(5, "passesGuards(): No, different sizes\n");
    return false;
  }

  for (size_t p = 0; p < preConds.size(); p++) {
    auto const& location = preConds[p].location;
    size_t g = 0;
    for (; g < newGuards.size(); g++) {
      if (location == newGuards[g].location) break;
    }
    if (g == newGuards.size()) {
      FTRACE(5, "passesGuards(): No, {} not in newGuards\n", show(location));
      return false;
    }
    if (!(preConds[p].type <= newGuards[g].type)) {
      FTRACE(5, "passesGuards(): No, {} not a subtype of {}\n",
             preConds[p].type, newGuards[g].type);
      return false;
    }
  }

  FTRACE(5, "passesGuards(): OK\n");
  return true;
}

/*
 * For each block in the retranslation chain, add its profiling
 * weight to each of the BlockData that satisfies its pre-conditions
 * (except for its original BlockData, since that weight was already
 * included during the creation of the BlockData).
 */
void updateWeights(const RegionDesc&   region,
                   RegionDesc::BlockId rootId,
                   BlockDataVec&       blockData,
                   const ProfData&     profData) {
  auto bid = rootId;
  while (true) {
    auto blockWeight = profData.transCounter(bid);
    for (auto& bd : blockData) {
      if (!bd.deleted && bd.blockId != bid &&
          passesGuards(region, *region.block(bid), bd)) {
        bd.weight += blockWeight;
      }
    }

    if (auto next = region.nextRetrans(bid)) {
      bid = next.value();
    } else {
      break;
    }
  }
}

/*
 * This function sorts the `blockData' vector in decreasing order or weight,
 * while keeping the region entry first and the deleted blocks at the end.
 */
void sortBlockData(BlockDataVec& blockData, RegionDesc::BlockId entryId) {
  std::sort(blockData.begin(), blockData.end(),
            [&](const BlockData& bd1, const BlockData& bd2) {
              const auto isEntry1 = bd1.blockId == entryId;
              const auto isEntry2 = bd2.blockId == entryId;
              if (isEntry1 != isEntry2) return isEntry1;
              if (bd1.deleted != bd2.deleted) return bd1.deleted < bd2.deleted;
              return bd1.weight > bd2.weight;
            });
}

/*
 * Update `region' based on the optimization result in `blockData'.
 * This encompasses:
 *
 *  1) Updating the type guards on the affected blocks.
 *  2) Updating the retranslation successor blocks to rechain them in the new
 *     order.
 *  3) Updating the any predecessor block in the region to go to the new chain
 *     root.
 *  4) Deleting the blocks that became unnecessary (unreachable) after
 *     rechaining.
 */
void updateRegion(RegionDesc&         region,
                  const BlockDataVec& blockData,
                  RegionDesc::BlockId rootId) {

  // 1) Update type guards.
  for (auto& bd : blockData) {
    if (!bd.relaxed) continue;

    // Note that we can't update the original block because it's part
    // of the profiling data -- so it may be used again later as part
    // of another region, and the decision to relax it or not might be
    // different depending on the region.  Therefore, we copy the
    // original block, update it, and replace it in the region.
    auto oldBlock = region.block(bd.blockId);
    auto newBlock = std::make_shared<RegionDesc::Block>(*oldBlock);
    region.replaceBlock(bd.blockId, newBlock);

    // Actually update the type guards.
    newBlock->clearPreConditions();
    for (auto& guard : bd.guards) {
      if (guard.type < TCell) {
        newBlock->addPreCondition(guard);
      }
    }
  }

  // 2) Update the retranslation successors, first cleaning the
  //    original pointers.
  RegionDesc::BlockId bid = rootId;
  while (true) {
    auto nextId = region.nextRetrans(bid);
    region.clearPrevRetrans(bid);
    region.clearNextRetrans(bid);
    if (nextId) bid = nextId.value();
    else break;
  }
  for (size_t i = 0; i < blockData.size() - 1; i++) {
    if (blockData[i + 1].deleted) break;
    auto thisId = blockData[i].blockId;
    auto nextId = blockData[i + 1].blockId;
    region.setNextRetrans(thisId, nextId);
  }

  // Compute allChainBlockIds, allChainPredIds, and newRootId to be
  // used in 3) below.
  RegionDesc::BlockIdSet allChainBlockIds;
  RegionDesc::BlockIdSet allChainPredIds;
  RegionDesc::BlockId    newRootId = kInvalidTransID;
  for (auto& bd : blockData) {
    allChainBlockIds.insert(bd.blockId);

    auto& preds = region.preds(bd.blockId);
    allChainPredIds.insert(preds.begin(), preds.end());

    if (!bd.deleted && newRootId == kInvalidTransID) {
      newRootId = bd.blockId;
    }
  }
  FTRACE(3, "newRootId = {}\n", newRootId);
  FTRACE(3, "allChainBlockIds = {}\n", folly::join(", ", allChainBlockIds));
  FTRACE(3, "allChainPredIds  = {}\n", folly::join(", ", allChainPredIds));

  // 3) For any block that had a successor in the retranslation chain,
  //    remove all the chain blocks from its successors' set and add only
  //    the new root.
  assertx(newRootId != kInvalidTransID);
  for (auto predId : allChainPredIds) {
    const auto& succs = region.succs(predId);
    for (auto succId : succs) {
      if (succId != newRootId && allChainBlockIds.count(succId)) {
        FTRACE(3, "updateRegion: removing arc {} -> {}\n", predId, succId);
        region.removeArc(predId, succId);
      }
    }

    FTRACE(3, "updateRegion: adding arc {} -> {}\n", predId, newRootId);
    region.addArc(predId, newRootId);
  }

  // 4) Delete unnecessary blocks from the region.
  for (auto& bd : blockData) {
    if (bd.deleted) {
      for (auto succId : region.succs(bd.blockId)) {
        FTRACE(3, "updateRegion: removing arc {}[deleted] -> {}\n",
               bd.blockId, succId);
        region.removeArc(bd.blockId, succId);
      }
      for (auto predId : region.preds(bd.blockId)) {
        FTRACE(3, "updateRegion: removing arc {} -> {}[deleted]\n",
               predId, bd.blockId);
        region.removeArc(predId, bd.blockId);
      }
      FTRACE(3, "updateRegion: deleting block {}\n", bd.blockId);
      region.deleteBlock(bd.blockId);
    }
  }

  // Re-order the blocks to make sure they're in topological order if possible.
  region.sortBlocks();

  // Check that the update region is still well-formed.
  DEBUG_ONLY std::string errorMsg;
  assert_flog(check(region, errorMsg), "{}\n{}", errorMsg, show(region));
}

DEBUG_ONLY std::string show(const BlockDataVec& blockData) {
  std::string ret;
  for (auto& bd : blockData) {
    fmt::format_to(std::back_inserter(ret), "   - Block {} (w: {}): relaxed:{} merged:{} deleted:{}\n",
                  bd.blockId, bd.weight, bd.relaxed, bd.merged, bd.deleted);
    for (auto& guard : bd.guards) {
      fmt::format_to(std::back_inserter(ret), "      - {}: {} ({})\n", show(guard.location),
                    guard.type, typeCategoryName(guard.category));
    }
  }
  return ret;
}

/*
 * Optimize `region's retranslation chain rooted at block `rootId'.
 */
void optimizeChain(RegionDesc&         region,
                   RegionDesc::BlockId rootId,
                   const ProfData&     profData) {
  auto blockData = createBlockData(region, rootId, profData);
  FTRACE(2, "optimizeChain(rootId {}): before relaxGuards:\n{}\n",
         rootId, show(blockData));
  relaxGuards(blockData);

  FTRACE(2, "optimizeChain(rootId {}): before removeDuplicates:\n{}\n",
         rootId, show(blockData));
  removeDuplicates(blockData, region);

  FTRACE(2, "optimizeChain(rootId {}): before unrelaxGuards:\n{}\n",
         rootId, show(blockData));
  unrelaxGuards(blockData);

  FTRACE(2, "optimizeChain(rootId {}): before updateWeights:\n{}\n",
         rootId, show(blockData));
  updateWeights(region, rootId, blockData, profData);

  FTRACE(2, "optimizeChain(rootId {}): before sortBlockData:\n{}\n",
         rootId, show(blockData));
  sortBlockData(blockData, region.entry()->id());

  FTRACE(2, "optimizeChain(rootId {}): before updateRegion:\n{}\n",
         rootId, show(blockData));
  updateRegion(region, blockData, rootId);
}

}

//////////////////////////////////////////////////////////////////////

/*
 * Optimize the `region's guards, operating on one retranslation chain
 * at a time.
 */
void optimizeProfiledGuards(RegionDesc& region, const ProfData& profData) {
  auto chainRoots = findRetransChainRoots(region);
  for (auto rootId : chainRoots) {
    optimizeChain(region, rootId, profData);
  }
}

//////////////////////////////////////////////////////////////////////

} }
