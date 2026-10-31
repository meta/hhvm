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

#include <fmt/format.h>
#include <memory>
#include <utility>
#include <vector>
#include <string>

#include <boost/container/flat_map.hpp>

#include <folly/Hash.h>

#include "hphp/runtime/vm/jit/containers.h"
#include "hphp/runtime/vm/jit/location.h"
#include "hphp/runtime/vm/jit/stack-offsets.h"
#include "hphp/runtime/vm/jit/type.h"
#include "hphp/runtime/vm/jit/types.h"

#include "hphp/runtime/vm/func.h"
#include "hphp/runtime/vm/resumable.h"
#include "hphp/runtime/vm/srckey.h"

namespace HPHP::jit {

struct ProfData;
struct TransCFG;

//////////////////////////////////////////////////////////////////////

enum class PGORegionMode {
  Hottrace, // Select a long region, using profile counters to guide the trace
  Hotblock, // Select a single block
  HotCFG,   // Select arbitrary CFG using profile counters to prune cold paths
  WholeCFG, // Select the entire CFG that has been profiled
};

//////////////////////////////////////////////////////////////////////

/*
 * RegionDesc is a description of a code region.  This includes the
 * list of blocks in the region, and also the list of control-flow
 * arcs within the region.
 *
 * It consists of a list of unique SrcKey ranges, with type
 * annotations that may come from profiling or other sources.
 *
 * The first block is the entry point, and the remaining blocks must
 * be sorted in a reverse post order.
 */
struct RegionDesc {
  struct Block;
  struct Arc;
  struct TypedLocation;
  struct GuardedLocation;
  using BlockPtr = std::shared_ptr<Block>;
  using BlockId = TransID;
  // BlockId Encoding:
  //   - Non-negative numbers are blocks that correspond
  //     to the start of a TransProfile translation, and therefore can
  //     be used to index into ProfData.
  //   - Negative numbers are used for other blocks, which correspond
  //     to blocks created by inlining and which don't correspond to
  //     the beginning of a profiling translation.
  using BlockIdSet = boost::container::flat_set<BlockId>;
  using BlockIdVec = std::vector<BlockId>;
  using BlockVec = std::vector<BlockPtr>;

  bool              empty() const;
  SrcKey            start() const;
  BlockPtr          entry() const;
  const BlockVec&   blocks() const;
  BlockPtr          block(BlockId id) const;
  const BlockIdSet& succs(BlockId id) const;
  const BlockIdSet& preds(BlockId id) const;
  bool              isExit(BlockId id) const;

  /*
   * Returns the set of blocks that got merged into block `id' by
   * guard relaxation.
   */
  const BlockIdSet& merged(BlockId id) const;

  const BlockIdSet* incoming() const;
  void incoming(BlockIdSet&& ids);

  /*
   * Modify this RegionDesc so that its list of blocks is sorted in a reverse
   * post order.
   */
  void sortBlocks();

  /*
   * Initialize reverse post order indices.
   */
  void initRpoIds();

  /*
   * Compute a dominating block for each block and save this information into
   * this RegionDesc.
   *
   * Requires blocks to be sorted in a reverse post order (see sortBlocks()).
   */
  void findDominators();

  /*
   * Returns the last BC offset in the region that corresponds to the
   * function where the region starts.  This will normally be the offset
   * of the last instruction in the last block, except if the function
   * ends with an inlined call.  In this case, the offset of the
   * corresponding FCall* in the function that starts the region is
   * returned.
   *
   * Note that the notion of "last BC offset" only makes sense for
   * regions that are linear traces.
   */
  SrcKey            lastSrcKey() const;

  /*
   * Returns the profile count associated with block `bid'.  In case other
   * blocks have been merged into this block, the returned count includes the
   * counts of these other blocks as well.
   */
  int64_t           blockProfCount(BlockId bid) const;
  double            blockProfCountScale(BlockId bid) const;

  Block*            addBlock(SrcKey sk, int length, SBInvOffset spOffset);
  void              addBlock(BlockPtr newBlock);
  bool              hasBlock(BlockId id) const;
  void              replaceBlock(BlockId bid, BlockPtr newBlock);
  void              deleteBlock(BlockId bid);
  BlockVec::iterator deleteBlock(RegionDesc::BlockVec::iterator it);
  void              renumberBlock(BlockId oldId, BlockId newId);
  void              addArc(BlockId src, BlockId dst);
  void              removeArc(BlockId src, BlockId dst);
  void              addMerged(BlockId fromId, BlockId intoId);
  Optional<BlockId> idom(BlockId id) const;
  uint32_t          rpoId(BlockId id) const;
  Optional<BlockId> prevRetrans(BlockId id) const;
  Optional<BlockId> nextRetrans(BlockId id) const;
  void              clearPrevRetrans(BlockId id);
  void              clearNextRetrans(BlockId id);
  void              setNextRetrans(BlockId id, BlockId next);
  void              append(const RegionDesc&  other);
  void              prepend(const RegionDesc& other);
  void              chainRetransBlocks();
  void              setBlockProfCountScale(BlockId, double);
  uint32_t          instrSize() const;
  std::string       toString() const;

  void setHotWeight(uint64_t weight) { m_hotWeight = weight; }
  Optional<uint64_t> getHotWeight() const { return m_hotWeight; }

  const std::vector<Type>& inlineInputTypes() const {
    return m_inlineInputTypes;
  }
  Type inlineCtxType() const { return m_inlineCtxType; }

  void setInlineContext(Type ctx, const std::vector<Type>& args) {
    m_inlineCtxType = ctx;
    m_inlineInputTypes = args;
  }

  template<class Work>
  void              forEachArc(Work w) const;

private:
  struct BlockData {
    BlockPtr                 block;
    BlockIdSet               preds;
    BlockIdSet               succs;
    BlockIdSet               merged; // other blocks that got merged into this
    BlockId                  prevRetransId{kInvalidTransID};
    BlockId                  nextRetransId{kInvalidTransID};
    BlockId                  idom{kInvalidTransID};
    uint32_t                 rpoId{std::numeric_limits<uint32_t>::max()};
    double                   profCountScale{1.0};
    bool                     hasIncoming{false};
    explicit BlockData(BlockPtr b = nullptr) : block(b) {}
  };

  BlockData& data(BlockId id);
  const BlockData& data(BlockId id) const {
    return const_cast<RegionDesc*>(this)->data(id);
  }
  void       copyBlocksFrom(const RegionDesc& other,
                            BlockVec::iterator where);
  void       copyArcsFrom(const RegionDesc& other);
  void       postOrderSort(RegionDesc::BlockId     bid,
                           RegionDesc::BlockIdSet& visited,
                           RegionDesc::BlockIdVec& outVec);

  std::vector<BlockPtr>             m_blocks;
  hphp_hash_map<BlockId, BlockData> m_data;

  // When optimizing, we may know what a "hot weight" for this region would be
  // relative to other regions. Pass this information down to vasm-layout.
  Optional<uint64_t> m_hotWeight;

  // For regions selected for inlining, track the types of input arguments
  std::vector<Type> m_inlineInputTypes;
  Type m_inlineCtxType;
};

using RegionDescPtr = std::shared_ptr<RegionDesc>;
using RegionVec = std::vector<RegionDescPtr>;
using RegionSet = hphp_hash_set<
  RegionDescPtr,
  smart_pointer_hash<RegionDescPtr>
>;

struct RegionDesc::Arc {
  BlockId src;
  BlockId dst;
};

/*
 * A type for somewhere in the middle of or start of a region.
 *
 * All types annotated in the RegionDesc are things that need to be guarded on
 * or checked and then side-exited on. Types from ahead-of-time static analysis
 * are encoded in the bytecode stream.
 */
struct RegionDesc::TypedLocation {
  Location location;
  Type type;
};

inline bool operator==(const RegionDesc::TypedLocation& a,
                       const RegionDesc::TypedLocation& b) {
  return a.location == b.location && a.type == b.type;
}

/*
 * Information about a location that is guarded in a RegionDesc.
 * Includes the type and the DataTypeCategory.
 */
struct RegionDesc::GuardedLocation {
  Location location;
  Type type;
  DataTypeCategory category;
};

inline bool operator==(const RegionDesc::GuardedLocation& a,
                       const RegionDesc::GuardedLocation& b) {
  return a.location == b.location &&
         a.type     == b.type     &&
         a.category == b.category;
}

/*
 * PostConditions are known type information for locals and stack
 * locations at the end of profiling translation.
 *
 * These are kept for blocks that end a profiling translation in order
 * to enable better region selection.  This information is used to
 * prevent profiling translations with incompatible types from being
 * stitched together in a larger, optimizing translation.
 *
 * The PostConditions are kept in two distinct sets:
 *
 *   - the 'changed' set includes locations that may have been
 *     modified by the corresponding translation;
 *
 *   - the 'refined' set includes locations that are not modified but
 *     that some information is learned about them during the
 *     corresponding translation, typically due to type checks or
 *     asserts.
 */
using TypedLocations   = jit::vector<RegionDesc::TypedLocation>;
using GuardedLocations = jit::vector<RegionDesc::GuardedLocation>;

struct PostConditions {
  TypedLocations changed;
  TypedLocations refined;
  PostConditions& operator|=(const PostConditions& other);
};

/*
 * A basic block in the region.
 */
struct RegionDesc::Block {

  Block(BlockId id, SrcKey start, int length, SBInvOffset initSpOff);

  Block& operator=(const Block&) = delete;

  /*
   * Accessors for the func, unit, length (in HHBC instructions), and
   * starting SrcKey of this Block.
   */
  BlockId     id()                const { return m_id; }
  const Unit* unit()              const { return m_start.unit(); }
  const Func* func()              const { return m_start.func(); }
  SrcKey      start()             const { return m_start; }
  SrcKey      last()              const { return m_last; }
  int         length()            const { return m_length; }
  bool        empty()             const { return length() == 0; }
  SBInvOffset initialSpOffset()   const { return m_initialSpOffset; }
  TransID     profTransID()       const { return m_profTransID; }

  void setID(BlockId id)                  { m_id = id; }
  void setProfTransID(TransID ptid)       { m_profTransID = ptid; }
  void setInitialSpOffset(SBInvOffset sp) { m_initialSpOffset = sp; }

  /*
   * Increase the length of the Block by 1.
   */
  void addInstruction();

  /*
   * Remove all instructions after sk from the block.
   */
  void truncateAfter(SrcKey sk);

  /*
   * Add a precondition type to this block. Preconditions have no effects on
   * correctness, but entering a block with a known type that violates a
   * precondition is likely to result in a side exit after little to no
   * forward progress.
   */
  void addPreCondition(const GuardedLocation&);

  /*
   * Set the post-conditions for this Block.
   */
  void setPostConds(const PostConditions&);

  /*
   * Clears the block's type pre-conditions.
   */
  void clearPreConditions();

  /*
   * The following getters return references to the metadata maps holding the
   * information added using the add* and set* methods above. The best way to
   * iterate over the information is using a MapWalker, since they're all
   * backed by a sorted map.
   */
  const GuardedLocations& typePreConditions() const {
    return m_typePreConditions;
  }
  const PostConditions& postConds() const {
    return m_postConds;
  }

private:
  void checkInstructions() const;
  void checkInstruction(SrcKey sk) const;
  void checkMetadata() const;

private:
  const SrcKey     m_start;
  SrcKey           m_last;
  BlockId          m_id;
  int              m_length;
  SBInvOffset      m_initialSpOffset;
  TransID          m_profTransID;
  GuardedLocations m_typePreConditions;
  PostConditions   m_postConds;
};

//////////////////////////////////////////////////////////////////////

/*
 * Information about the context in which we are selecting a region.
 *
 * Right now this is a source location, plus information about the live
 * types that we need to be compiling for. There is no implication that
 * the region selected will necessarily be specialized for those types.
 */

using LiveTypesVec = jit::small_vector<RegionDesc::TypedLocation, 10>;

struct RegionContext {
  RegionContext(SrcKey sk, SBInvOffset spOff)
    : sk(sk), spOffset(spOff) {}

  SrcKey sk;
  LiveTypesVec liveTypes;
  SBInvOffset spOffset;
};

//////////////////////////////////////////////////////////////////////

template<class Work> inline
void RegionDesc::forEachArc(Work w) const {
  for (auto& src : m_blocks) {
    auto srcId = src->id();
    for (auto dstId : succs(srcId)) {
      w(srcId, dstId);
    }
  }
}

//////////////////////////////////////////////////////////////////////

/**
 * A key struct that uniquely identifies a single translation that can be
 * published at any point in time.  This includes both the SrcKey for the start
 * of the translation and the list of its entry guards.
 */
struct RegionEntryKey {

  RegionEntryKey(SrcKey srcKey, const GuardedLocations& guards)
    : m_srcKey(srcKey)
    , m_guards(guards)
  { }

  explicit RegionEntryKey(const RegionDesc& region)
    : m_srcKey(region.entry()->start())
    , m_guards(region.entry()->typePreConditions())
  { }

  SrcKey                  srcKey() const { return m_srcKey; }
  const GuardedLocations& guards() const { return m_guards; }

  struct Eq {
    bool operator()(const RegionEntryKey& key1,
                    const RegionEntryKey& key2) const {
      return key1.m_srcKey == key2.m_srcKey &&
             key1.m_guards == key2.m_guards;
    }
  };

  struct Hash {
    size_t operator()(const RegionEntryKey& key) const {
      size_t h = key.m_srcKey.toAtomicInt();
      Location::Hash lochash;
      using data_category_t = std::underlying_type<DataTypeCategory>::type;
      for (auto& guard : key.m_guards) {
        h = folly::hash::hash_combine(
          h, lochash(guard.location),
          guard.type.hash(),
          static_cast<data_category_t>(guard.category)
        );
      }
      return h;
    }
  };

 private:
  SrcKey           m_srcKey;
  GuardedLocations m_guards;
};

//////////////////////////////////////////////////////////////////////

/*
 * Select a compilation region corresponding to the given context.
 * The shape of the region selected is controlled by
 * Cfg::Jit::RegionSelector.
 *
 * This function may return nullptr.
 *
 * For now this is hooked up in mcgen::translateWork, and
 * returning nullptr causes it to use the current level 0 tracelet
 * analyzer.  Eventually we'd like this to completely replace analyze.
 */
RegionDescPtr selectRegion(const RegionContext& context, TransKind kind);


/*
 * Select a compilation region as long as possible using the given context.
 * The region will be broken before the first instruction that attempts to
 * consume an input with an insufficiently precise type, or after most control
 * flow instructions.  This uses roughly the same heuristics as the old
 * analyze() framework.
 *
 * May return a null region if the given RegionContext doesn't have enough
 * information to translate at least one instruction.
 *
 * The `allowInlining' flag should be disabled when we are selecting a tracelet
 * whose shape will be analyzed by the inlining decider.
 */
RegionDescPtr selectTracelet(const RegionContext& ctx, TransKind kind,
                             int32_t maxBCInstrs, bool inlining = false);

struct HotTransContext {
  TransIDSet entries;
  TransCFG* cfg;
  const ProfData* profData;
  int32_t maxBCInstrs;
  bool inlining{false};
  std::vector<Type>* inputTypes{nullptr};
};

/*
 * Select the hottest trace with the given context (starting at ctx.tid).
 */
RegionDescPtr selectHotTrace(HotTransContext& ctx);

/*
 * Create a region with the given context ctx (starting at ctx.tid) that
 * includes as much of the TransCFG as possible (in "wholecfg" mode), but that
 * can be pruned to eliminate cold/unlikely code as well (in "hotcfg" mode).
 * Returns in *truncated whether or not the region had to be truncated due to
 * the maximum bytecode limit.
 */
RegionDescPtr selectHotCFG(HotTransContext& ctx, bool* truncated = nullptr);

/*
 * Checks whether the type pre-conditions at the beginning of block
 * satisfy the post-conditions in prevPostConds.
 */
bool preCondsAreSatisfied(const RegionDesc::BlockPtr& block,
                          const TypedLocations& prevPostConds);

/*
 * This function returns true for control-flow bytecode instructions that
 * are not supported in the middle of a region yet.
 */
bool breaksRegion(SrcKey sk);

/*
 * Creates regions covering all existing profile translations for func.
 */
RegionVec regionizeFunc(const Func* func, std::string& transCFGAnnot);

/*
 * Optimize the guards of `region' based on profiling data.
 *
 * The `region' must have its retranslation blocks already chained.
 */
void optimizeProfiledGuards(RegionDesc& region, const ProfData& profData);

/*
 * Returns the PGO region selector to be used for the given `func'.
 * This depends on the value of Cfg::Jit::PGORegionSelector
 * and the given `func'.
 */
PGORegionMode pgoRegionMode(const Func& func);

/*
 * Functions to map BlockIds to the TransIDs used when the block was
 * profiled.
 */
bool    hasTransID(RegionDesc::BlockId blockId);
TransID getTransID(RegionDesc::BlockId blockId);

/*
 * Checks if the given region is well-formed.
 */
bool check(const RegionDesc& region, std::string& error);

/*
 * Debug stringification for various things.
 */
std::string show(RegionDesc::TypedLocation);
std::string show(const RegionDesc::GuardedLocation&);
std::string show(const GuardedLocations&);
std::string show(const PostConditions&);
std::string show(const RegionContext&);
std::string show(const RegionDesc::Block&);
std::string show(const RegionDesc&);

//////////////////////////////////////////////////////////////////////

}

template <>
struct fmt::formatter<HPHP::jit::RegionDesc> : fmt::formatter<std::string_view> {
  auto format(const HPHP::jit::RegionDesc& v, fmt::format_context& ctx) const {
    return fmt::formatter<std::string_view>::format(v.toString(), ctx);
  }
};
