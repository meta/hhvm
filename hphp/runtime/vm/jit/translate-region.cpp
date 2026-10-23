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

#include "hphp/runtime/vm/jit/translate-region.h"

#include "hphp/util/configs/debugger.h"
#include "hphp/util/configs/hhir.h"
#include "hphp/util/ringbuffer.h"
#include "hphp/util/trace.h"

#include "hphp/runtime/base/tracing.h"

#include "hphp/runtime/vm/bc-pattern.h"

#include "hphp/runtime/vm/jit/inline-stitching.h"
#include "hphp/runtime/vm/jit/inlining-decider.h"
#include "hphp/runtime/vm/jit/irgen.h"
#include "hphp/runtime/vm/jit/irgen-control.h"
#include "hphp/runtime/vm/jit/irgen-exit.h"
#include "hphp/runtime/vm/jit/irgen-func-prologue.h"
#include "hphp/runtime/vm/jit/irgen-inlining.h"
#include "hphp/runtime/vm/jit/irgen-internal.h"
#include "hphp/runtime/vm/jit/ir-unit.h"
#include "hphp/runtime/vm/jit/location.h"
#include "hphp/runtime/vm/jit/mcgen.h"
#include "hphp/runtime/vm/jit/mcgen-translate.h"
#include "hphp/runtime/vm/jit/normalized-instruction.h"
#include "hphp/runtime/vm/jit/opt.h"
#include "hphp/runtime/vm/jit/print.h"
#include "hphp/runtime/vm/jit/prof-data.h"
#include "hphp/runtime/vm/jit/translation-stats.h"
#include "hphp/runtime/vm/jit/punt.h"
#include "hphp/runtime/vm/jit/region-selection.h"
#include "hphp/runtime/vm/jit/timer.h"
#include "hphp/runtime/vm/jit/type.h"

#include <fmt/format.h>

TRACE_SET_MOD(trans)

namespace HPHP::jit {

/*
 * Data used by irGenRegion() and friends to pass information between retries.
 */
struct TranslateRetryContext {
  // Bytecode instructions that must be interpreted.
  ProfSrcKeySet toInterp;

  // Regions to not inline
  jit::fast_set<ProfSrcKey, ProfSrcKey::Hasher> inlineBlacklist;
};

//////////////////////////////////////////////////////////////////////

namespace {

enum class TranslateResult {
  Retry,
  Success
};

/*
 * Create a map from RegionDesc::BlockId -> IR Block* for all region blocks.
 */
BlockIdToIRBlockMap createBlockMap(irgen::IRGS& irgs,
                                   const RegionDesc& region) {
  auto ret = BlockIdToIRBlockMap{};

  auto& irb = *irgs.irb;
  auto const& blocks = region.blocks();
  for (unsigned i = 0; i < blocks.size(); i++) {
    auto const rBlock = blocks[i];
    auto const id = rBlock->id();

    // NB: This maps the region entry block to a new IR block, even though
    // we've already constructed an IR entry block. We'll make the IR entry
    // block jump to this block.
    assertx(!hasTransID(id) || profData());
    auto transCount = hasTransID(id)
      ? region.blockProfCount(id)
      : 1;
    uint64_t profCount = transCount * irgs.profFactor;
    auto const iBlock = irb.unit().defBlock(profCount);

    ret[id] = iBlock;
    FTRACE(1,
           "createBlockMaps: RegionBlock {} => IRBlock {} (BC offset = {})\n",
           id, iBlock->id(), rBlock->start().printableOffset());
  }

  return ret;
}

/*
 * Set IRBuilder's Block associated to blockId's block according to
 * the mapping in blockIdToIRBlock.
 */
void setIRBlock(irgen::IRGS& irgs,
                RegionDesc::BlockId blockId,
                const RegionDesc& region,
                const BlockIdToIRBlockMap& blockIdToIRBlock) {
  auto& irb = *irgs.irb;
  auto rBlock = region.block(blockId);
  auto sk = rBlock->start();

  auto iit = blockIdToIRBlock.find(blockId);
  assertx(iit != blockIdToIRBlock.end());

  assertx(!irb.hasBlock(sk));
  FTRACE(3, "  setIRBlock: blockId {}, offset {} => IR Block {}\n",
         blockId, sk.printableOffset(), iit->second->id());
  irb.setBlock(sk, iit->second);
}

/*
 * Set IRBuilder's Blocks for srcBlockId's successors' offsets within
 * the region.  It also returns the guard-failure block, if any.
 */
Block* setSuccIRBlocks(irgen::IRGS& irgs,
                       const RegionDesc& region,
                       RegionDesc::BlockId srcBlockId,
                       const BlockIdToIRBlockMap& blockIdToIRBlock) {
  FTRACE(3, "setSuccIRBlocks: srcBlockId = {}\n", srcBlockId);
  auto& irb = *irgs.irb;
  irb.resetOffsetMapping();
  for (auto dstBlockId : region.succs(srcBlockId)) {
    setIRBlock(irgs, dstBlockId, region, blockIdToIRBlock);
  }
  if (auto nextRetrans = region.nextRetrans(srcBlockId)) {
    auto it = blockIdToIRBlock.find(nextRetrans.value());
    assertx(it != blockIdToIRBlock.end());
    return it->second;
  }
  return nullptr;
}

/*
 * If this region's entry block is at an entry point for a function (DV init or
 * main entry), we can assert that all non-parameter locals are Uninit under
 * some conditions.  This function checks the necessary conditions and, if they
 * hold, emits such type assertions.
 */
void emitEntryAssertions(irgen::IRGS& irgs, const Func* func, SrcKey sk) {
  // TODO(named_params) we may be able to emit better assertions here in the
  // future.
  if (sk.namedParamsFuncEntry()) return;
  assertx(sk.funcEntry());

  uint32_t loc = 0;

  // Set types of passed arguments. They were already type checked.
  auto const numArgs = sk.numEntryArgs() + func->numNamedParams();
  for (; loc < numArgs; ++loc) {
    auto const t = typeFromFuncParam(func, loc);
    irgen::assertTypeLocation(irgs, Location::Local { loc }, t);
  }

  // Non-passed positional parameters contain uninitialized garbage.
  auto const numNonVariadicParams = func->numNonVariadicParams();
  loc = numNonVariadicParams;

  // Set the type of ...$args parameter.
  if (func->hasVariadicCaptureParam()) {
    assertx(func->numNonVariadicParams() == loc);
    auto const t = numArgs == numNonVariadicParams
      ? TVec : Type::cns(staticEmptyVec());
    irgen::assertTypeLocation(irgs, Location::Local { loc }, t);
    loc++;
  }

  assertx(loc == func->numParams());

  if (func->isClosureBody()) {
    // In a closure, non-parameter locals can have types other than Uninit
    // after the prologue runs, as use vars gets unpacked into them. We rely
    // on hhbbc to assert these types.
    return;
  }

  if (func->hasReifiedGenerics()) {
    // The next non-parameter local contains the reified generics.
    assertx(func->reifiedGenericsLocalId() == loc);
    irgen::assertTypeLocation(irgs, Location::Local { loc }, TVec);
    loc++;
  }
  if (func->hasCoeffectsLocal()) {
    // The next non-parameter local contains the coeffects.
    assertx(func->coeffectsLocalId() == loc);
    irgen::assertTypeLocation(irgs, Location::Local { loc }, TInt);
    loc++;
  }
}

//////////////////////////////////////////////////////////////////////

namespace {

struct ArrayReachInfo {
  Location loc;
  uint32_t guardIdx;
};

}

//////////////////////////////////////////////////////////////////////

/*
 * Emit type guards.
 */
void emitGuards(irgen::IRGS& irgs,
                const RegionDesc::Block& block,
                bool isEntry,
                Block* guardFailBlock) {
  auto const sk = block.start();
  if (isEntry) {
    irgen::ringbufferEntry(irgs, Trace::RBTypeTraceletGuards, sk);
  }

  if (sk.nonTrivialFuncEntry()) {
    emitEntryAssertions(irgs, block.func(), sk);
  }
  if (Cfg::Jit::CollectTranslationStats) {
    auto transStats = globalTransStats();
    assertx(transStats != nullptr);
    TransID transStatsID = transStats->initTransStats(irgs.context.kind, sk);
    assertx(transStatsID != kInvalidTransID);
    gen(irgs, IncStatCounter, TransIDData{transStatsID});
  }

  // Emit type guards/preconditions.
  assertx(IMPLIES(sk.trivialDVFuncEntry(), block.typePreConditions().empty()));
  for (auto const& preCond : block.typePreConditions()) {
    auto const type = preCond.type;
    auto const loc  = preCond.location;
    assertx(IMPLIES(type.arrSpec(), irgs.context.kind == TransKind::Live));
    if (guardFailBlock == nullptr) guardFailBlock = irgen::makeExit(irgs, sk);
    irgen::checkType(irgs, loc, type, guardFailBlock);
  }

  // Finish emitting guards, and emit profiling counters.
  if (isEntry) {
    if (RI().m_coverage.m_should_use_per_file_coverage &&
        !sk.trivialDVFuncEntry()) {
      irgen::checkCoverage(irgs);
    }

    if (Cfg::Debugger::EnableVSDebugger && Cfg::Eval::EmitDebuggerIntrCheck &&
        !sk.trivialDVFuncEntry()) {
      irgen::checkDebuggerIntr(irgs, curSrcKey(irgs));
    }

    if (irgs.context.kind == TransKind::Profile) {
      assertx(irgs.context.transIDs.size() == 1);
      auto const transID = *irgs.context.transIDs.begin();
      if (sk.nonTrivialFuncEntry() && !mcgen::retranslateAllEnabled()) {
        irgen::checkCold(irgs, transID);
      } else {
        irgen::incProfCounter(irgs, transID);
      }
    }

    // Increment the count for the latest call for optimized translations if we're
    // going to serialize the profile data.
    if (irgs.context.kind == TransKind::Optimize && isJitSerializing() &&
        sk.nonTrivialFuncEntry() && Cfg::Jit::PGOOptCodeCallGraph) {
      irgen::gen(irgs, IncCallCounter, FuncData { curFunc(irgs) }, irgen::fp(irgs));
    }

    irgen::ringbufferEntry(irgs, Trace::RBTypeTraceletBody, sk);

    // In the entry block, hhbc-translator gets a chance to emit some code
    // immediately after the initial checks on the first instruction.
    irgen::prepareEntry(irgs);
  }
}

struct BlockWorkQueue {
  explicit BlockWorkQueue(const RegionDesc& region)
    : m_region(region)
    , m_numUnprocPreds(jit::vector<int>(region.blocks().size()))
  {
    for (auto rpoId = 0; rpoId < m_region.blocks().size(); ++rpoId) {
      auto const& bid = m_region.blocks()[rpoId]->id();
      m_numUnprocPreds[rpoId] =
        m_region.preds(bid).size() + (m_region.prevRetrans(bid) ? 1 : 0);
      if (m_numUnprocPreds[rpoId] == 0) m_queue.push(rpoId);
    }
  }

  /*
   * Returns the id of the next reachable region block to process and a flag
   * indicating whether this block has any still potentially reachable
   * unprocessed predecessors, or std::nullopt if no such block exists.
   */
  Optional<std::pair<RegionDesc::BlockId, bool>> nextReachable(
    const irgen::IRBuilder& irb,
    const BlockIdToIRBlockMap& blockIdToIRBlock
  ) {
    // Process block's successor edges. Decrements the number of unprocessed
    // predecessors of each successor and enqueues them if there are none left.
    auto const processSuccs = [&](RegionDesc::BlockId bid) {
      auto const handleSucc = [&](RegionDesc::BlockId succId) {
        auto const succRpoId = m_region.rpoId(succId);
        if (--m_numUnprocPreds[succRpoId] == 0) {
          m_queue.push(succRpoId);
        }
      };

      for (auto succId : m_region.succs(bid)) handleSucc(succId);
      if (auto succId = m_region.nextRetrans(bid)) handleSucc(*succId);
    };

    // Check if the IR block corresponding to the region block is reachable.
    auto const reachable = [&](RegionDesc::BlockId bid) {
      auto const it = blockIdToIRBlock.find(bid);
      assertx(it != blockIdToIRBlock.end());
      auto irBlock = it->second;
      return irb.canStartBlock(irBlock);
    };

    // Check if the dominator of the block was already processed.
    DEBUG_ONLY auto const processedDominator = [&](RegionDesc::BlockId bid) {
      auto const idom = m_region.idom(bid);
      if (!idom) return true;

      auto const idomRpoId = m_region.rpoId(*idom);
      return m_numUnprocPreds[idomRpoId] <= 0;
    };

    // Find any block with no pending unprocessed predecessors and process it.
    // If this block is unreachable, it can't possibly get reachable again.
    // We still need to process its successors to unblock their processing.
    while (!m_queue.empty()) {
      auto const rpoId = m_queue.front();
      m_queue.pop();

      auto const bid = m_region.blocks()[rpoId]->id();
      processSuccs(bid);
      if (reachable(bid)) return std::make_pair(bid, false);
    }

    // If we are here, all remaining blocks are either unreachable, or depend
    // on a loop. Find the first reachable block in RPO order and process it.
    // This is a good heuristics to reduce the number of blocks processed
    // with unprocessed predecessors. Loops are rare, so use a linear scan.
    for (auto rpoId = 0; rpoId < m_region.blocks().size(); ++rpoId) {
      // Skip already processed blocks.
      if (m_numUnprocPreds[rpoId] <= 0) continue;

      auto const bid = m_region.blocks()[rpoId]->id();

      // Skip unreachable blocks.
      if (!reachable(bid)) continue;

      // Dominator block must have been processed, otherwise `bid' would not
      // be reachable. We assert it here and assume it at recoverLocalState().
      assertx(processedDominator(bid));

      // Clear the number of unprocessed preds so that we don't get processed
      // again.
      m_numUnprocPreds[rpoId] = 0;
      processSuccs(bid);
      return std::make_pair(bid, true);
    }

    // No reachable blocks found. The end.
    return std::nullopt;
  }

private:
  const RegionDesc& m_region;
  jit::vector<int> m_numUnprocPreds;
  jit::queue<int> m_queue;
};

/*
 * Returns whether or not block `bid' is in the retranslation chain
 * for `region's entry block.
 */
bool inEntryRetransChain(RegionDesc::BlockId bid, const RegionDesc& region) {
  auto block = region.entry();
  if (block->start() != region.block(bid)->start()) return false;
  while (true) {
    if (block->id() == bid) return true;
    auto nextRetrans = region.nextRetrans(block->id());
    if (!nextRetrans) return false;
    block = region.block(nextRetrans.value());
  }
  not_reached();
}

/*
 * If `psk' is not an FCall with inlinable `callee', return nullptr.
 *
 * Otherwise, select a region for `callee' if one is not already present in
 * `retry'.  Update `inl' and return the region if it's inlinable.
 */
irgen::RegionAndLazyUnit getInlinableCalleeRegionAndLazyUnit(
  const irgen::IRGS& irgs,
  SrcKey entry,
  Type ctxType,
  const ProfSrcKey& psk,
  int& calleeCost,
  bool addedUninitNamed,
  const std::vector<Type>& inputTypes
) {
  assertx(entry.anyFuncEntry());
  if (isProfiling(irgs.context.kind) || irgs.inlineState.conjure) {
    return {psk.srcKey, nullptr};
  }
  if (!irgs.region || !irgs.retryContext) {
    return {psk.srcKey, nullptr};
  }
  auto annotationsPtr = mcgen::dumpTCAnnotation(irgs.context.kind) ?
                        irgs.unit.annotationData.get() : nullptr;
  if (!canInlineAt(psk.srcKey, entry, annotationsPtr)) {
    return {psk.srcKey, nullptr};
  }


  auto const& inlineBlacklist = irgs.retryContext->inlineBlacklist;
  if (inlineBlacklist.find(psk) != inlineBlacklist.end()) {
    return {psk.srcKey, nullptr};
  }

  auto regionAndLazyUnit = selectCalleeRegion(irgs, entry, ctxType, psk.srcKey,
                                              addedUninitNamed, inputTypes);
  if (!regionAndLazyUnit.region()) {
    return {psk.srcKey, nullptr};
  }
  if (!shouldInline(irgs, psk.srcKey, entry.func(), regionAndLazyUnit,
                    calleeCost)) {
    return {psk.srcKey, nullptr};
  }
  return regionAndLazyUnit;
}

static bool needsSurpriseCheck(Op op) {
  return op == Op::JmpZ || op == Op::JmpNZ || op == Op::Jmp;
}

static bool isSimpleOp(Op op) {
  return op == Op::Not ||
      op == Op::Same ||
      op == Op::NSame ||
      op == Op::Eq ||
      op == Op::Neq ||
      op == Op::Lt ||
      op == Op::Lte ||
      op == Op::Gt ||
      op == Op::Gte ||
      op == Op::Cmp ||
      op == Op::IsTypeL ||
      op == Op::IsTypeC ||
      op == Op::Add ||
      op == Op::Sub ||
      op == Op::Mul ||
      op == Op::Div ||
      op == Op::Mod ||
      op == Op::Pow ||
      op == Op::BitAnd ||
      op == Op::BitOr ||
      op == Op::BitXor ||
      op == Op::BitNot ||
      op == Op::CastBool ||
      op == Op::CastInt;
}

Optional<unsigned> scheduleSurprise(const RegionDesc::Block& block) {
  Optional<unsigned> checkIdx;
  auto sk = block.start();
  for (unsigned i = 0; i < block.length(); ++i, sk.advance(block.func())) {
    if (sk.anyFuncEntry()) continue;

    auto const backwards = [&]{
      auto const offset = sk.offset();
      auto const offsets = instrJumpTargets(sk.func()->entry(), offset);
      return std::any_of(
        offsets.begin(), offsets.end(), [=] (Offset o) { return o < offset; }
      );
    };
    if (i == block.length() - 1 && needsSurpriseCheck(sk.op()) && backwards()) {
      return checkIdx;
    }
    if (isSimpleOp(sk.op())) {
      if (!checkIdx) checkIdx = i;
      continue;
    }
    checkIdx.reset();
  }
  return {};
}

TransID canonTransID(const TransIDSet& tids) {
  return tids.size() == 0 ? kInvalidTransID : *tids.begin();
}

/*
 * Find a set of locals that might be modified on any paths between `from' and
 * `to', where `from' is a dominator of `to'.
 */
boost::dynamic_bitset<> findModifiedLocals(
  const RegionDesc& region,
  RegionDesc::BlockId from,
  RegionDesc::BlockId to
) {
  FTRACE(2, "findModifiedLocals B{} -> B{}\n", from, to);
  auto const numLocals = region.block(to)->func()->numLocals();
  auto modified = boost::dynamic_bitset<>(numLocals);
  auto seen = RegionDesc::BlockIdSet{from};
  auto queue = RegionDesc::BlockIdVec{};

  if (auto const pr = region.prevRetrans(to)) queue.push_back(*pr);
  for (auto const pred : region.preds(to)) queue.push_back(pred);

  while (!queue.empty()) {
    auto const bid = queue.back();
    queue.pop_back();

    if (!seen.insert(bid).second) continue;

    if (auto const pr = region.prevRetrans(bid)) queue.push_back(*pr);
    for (auto const pred : region.preds(bid)) queue.push_back(pred);

    auto const& b = *region.block(bid);

    auto sk = b.start();
    for (uint32_t i = 0; i < b.length(); ++i, sk.advance(b.func())) {
      if (sk.anyFuncEntry()) continue;
      auto const& ii = getInstrInfo(sk.op());
      if (ii.out & InstrFlags::Local) {
        modified[getLocalOperand(sk)] = true;
      }
      if (sk.op() == Op::BaseL) {
        // While BaseL does not directly modify a local, it is a starting
        // instruction of a linear sequence of MInstrs that will end up
        // modifying it.
        auto const mode = MOpMode(getImm(sk.pc(), 1).u_OA);
        switch (mode) {
          case MOpMode::None:
          case MOpMode::Warn:
            break;
          case MOpMode::Define:
          case MOpMode::Unset:
          case MOpMode::InOut:
            modified[getLocalOperand(sk)] = true;
            break;
        }
      }
    }
  }

  return modified;
}

/*
 * Given a block with unprocessed preds with cleared local state, attempt
 * to recover the state of locals using an immediate dominator of the block.
 *
 * Find all locals that are guaranteed to not be modified and recover the state
 * from successors of the immediate dominator. Since every path to the current
 * block must go through one of these blocks and the local is not modified
 * afterwards, the state can be reused.
 */
void recoverLocalState(
  irgen::IRGS& env,
  RegionDesc::BlockId bid,
  const BlockIdToIRBlockMap& blockIdToIRBlock
) {
  auto const idom = env.region->idom(bid);
  if (!idom) return;

  // Dominator block must have been processed, otherwise `bid' would not
  // be reachable. This is asserted by BlockWorkQueue::nextReachable().
  auto const modifiedLocals = findModifiedLocals(*env.region, *idom, bid);

  std::vector<Block*> idomSuccs;
  auto const add = [&](RegionDesc::BlockId bid) {
    auto const it = blockIdToIRBlock.find(bid);
    assertx(it != blockIdToIRBlock.end());
    auto const irBlock = it->second;

    // If we don't have a state for a successor, it is not directly reachable
    // from the immediate dominator. It is safe to ignore even if it becomes
    // reachable later, as all paths to this successor from the idom must go
    // via other successors.
    if (!env.irb->fs().hasStateFor(irBlock)) return;

    idomSuccs.push_back(irBlock);
  };
  if (auto const nr = env.region->nextRetrans(*idom)) add(*nr);
  for (auto const succ : env.region->succs(*idom)) add(succ);

  for (auto i = 0; i < modifiedLocals.size(); ++i) {
    if (!modifiedLocals[i]) {
      env.irb->fs().recoverLocal(idomSuccs, i);
    }
  }
}

TranslateResult irGenRegionImpl(irgen::IRGS& irgs,
                                const RegionDesc& region,
                                double profFactor,
                                bool ignoresBCSize = false) {
  const Timer irGenTimer(
    Timer::irGenRegionAttempt, irgs.unit.logEntry().get_pointer());
  auto& irb = *irgs.irb;
  auto prevRegion      = irgs.region;      irgs.region      = &region;
  auto prevProfFactor  = irgs.profFactor;  irgs.profFactor  = profFactor;
  auto prevProfTransIDs = irgs.profTransIDs; irgs.profTransIDs = TransIDSet{};
  auto prevOffsetMapping = irb.saveAndClearOffsetMapping();
  auto prevEHBlockMapping = irb.saveAndClearEHBlockMapping();
  // Note: no need to save/restore m_skToEHDecRefBlockMap, it is uniquely keyed.
  SCOPE_EXIT {
    irgs.region      = prevRegion;
    irgs.profFactor  = prevProfFactor;
    irgs.profTransIDs = prevProfTransIDs;
    irb.restoreOffsetMapping(std::move(prevOffsetMapping));
    irb.restoreEHBlockMapping(std::move(prevEHBlockMapping));
  };

  FTRACE(1, "translateRegion (mode={}, profFactor={:.2}) starting with:\n{}\n",
         show(irgs.context.kind), profFactor, show(region));

  if (Cfg::Eval::DumpRegion &&
      mcgen::dumpTCAnnotation(irgs.context.kind)) {
    irgs.unit.annotationData->add("RegionDesc", show(region));
  }

  std::string errorMsg;
  always_assert_flog(check(region, errorMsg), "{}", errorMsg);

  if (!ignoresBCSize) {
    auto regionSize = region.instrSize();
    always_assert(regionSize <= irgs.budgetBCInstrs);
    irgs.budgetBCInstrs -= regionSize;
  }

  // Create a map from region blocks to their corresponding initial IR blocks.
  auto const inlining = isInlining(irgs);
  auto blockIdToIRBlock = createBlockMap(irgs, region);

  if (!inlining) {
    // Prepare to start translation of the first region block.
    auto const entry = irb.unit().entry();
    irb.startBlock(entry, false /* hasUnprocPred */);

    // Make the IR entry block jump to the IR block we mapped the region entry
    // block to (they are not the same!).
    {
      auto const irBlock = blockIdToIRBlock[region.entry()->id()];
      always_assert(irBlock != entry);
      irgen::gen(irgs, Jmp, irBlock);
    }
  } else {
    // Set the first callee block as a successor to the FCall's block and
    // "fallthrough" from the caller into the callee's first block.
    setIRBlock(irgs, region.entry()->id(), region, blockIdToIRBlock);
    irgen::endBlock(irgs, region.start());
  }

  auto workQ = BlockWorkQueue(region);

  while (auto optBlockId = workQ.nextReachable(irb, blockIdToIRBlock)) {
    auto const [blockId, hasUnprocPred] = *optBlockId;
    auto const& block  = *region.block(blockId);
    auto sk            = block.start();
    bool emitedSurpriseCheck = false;
    auto const surprise = scheduleSurprise(block);

    SCOPE_ASSERT_DETAIL("IRGS") { return show(irgs); };

    irgs.profTransIDs.clear();
    if (hasTransID(blockId)) {
      irgs.profTransIDs.insert(blockId);
      auto& mergedBlocks = irgs.region->merged(blockId);
      irgs.profTransIDs.insert(mergedBlocks.begin(), mergedBlocks.end());
    }
    irgs.firstBcInst = inEntryRetransChain(blockId, region) && !inlining;
    irgen::prepareForNextHHBC(irgs, sk);

    // Prepare to start translating this region block.  This loads the
    // FrameState for the IR block corresponding to the start of this
    // region block, and it also sets the map from BC offsets to IR
    // blocks for the successors of this block in the region.
    auto const irBlock = blockIdToIRBlock[blockId];
    // Note: a block can have an unprocessed predecessor even if the
    // region is acyclic, e.g. if the IR was able to prove a path was
    // unfeasible due to incompatible types.
    if (!irb.startBlock(irBlock, hasUnprocPred)) {
      // This can't happen because we picked a reachable block from the workQ.
      always_assert_flog(
        0, "translateRegion: tried to startBlock on unreachable block {}\n",
        blockId
      );
    }
    // If we cleared local state due to unprocessed preds, try to restore it
    // from its immediate dominator's output state.
    if (hasUnprocPred) recoverLocalState(irgs, blockId, blockIdToIRBlock);

    auto const guardFailBlock =
      setSuccIRBlocks(irgs, region, blockId, blockIdToIRBlock);

    // Emit the type predictions for this region block. If this is the first
    // instruction in the region, we check inner type eagerly, and generate
    // profiling code in profiling translations.
    auto const isEntry = &block == region.entry().get() && !inlining;
    emitGuards(irgs, block, isEntry, guardFailBlock);

    if (irb.inUnreachableState()) {
      FTRACE(1, "translateRegion: skipping unreachable block: {}\n", blockId);
      continue;
    }

    // Generate IR for each bytecode instruction in this block.
    for (unsigned i = 0; i < block.length(); ++i, sk.advance(block.func())) {
      ProfSrcKey psk { canonTransID(irgs.profTransIDs), sk };
      auto const lastInstr = i == block.length() - 1;

      // Update bcOff here so any guards or assertions from metadata are
      // attributed to this instruction.
      irgen::prepareForNextHHBC(irgs, sk);

      // Create and initialize the instruction.
      NormalizedInstruction inst(sk, block.unit());
      inst.interp = irgs.retryContext->toInterp.count(psk);

      if (surprise && *surprise == i) {
        emitedSurpriseCheck = true;
        inst.forceSurpriseCheck = true;
      }

      // Emit IR for the body of the instruction.
      try {
        irgs.skipSurpriseCheck = emitedSurpriseCheck;
        translateInstr(irgs, inst);
      } catch (const RetryIRGen&) {
        return TranslateResult::Retry;
      } catch (const FailedIRGen& exn) {
        ProfSrcKey psk2{canonTransID(irgs.profTransIDs), sk};
        always_assert_flog(!irgs.retryContext->toInterp.contains(psk2),
                           "IR generation failed with {}\n",
                           exn.what());
        FTRACE(1, "ir generation for {} failed with {}\n",
          inst.toString(), exn.what());
        irgs.retryContext->toInterp.insert(psk2);
        return TranslateResult::Retry;
      }

      irgen::finishHHBC(irgs);

      if (!lastInstr && irgs.irb->inUnreachableState()) {
        FTRACE(1, "Breaking region at unreachable state at B{}\n", block.id());
        break;
      }

      // If this is the last instruction, handle block transitions.
      // If the block ends the region, then call irgen::endRegion to
      // sync the state and make a REQ_BIND_JMP service request.
      // Otherwise, if the instruction has a fall-through, then insert
      // a jump to the next offset, since it may not be the next block
      // to be translated.
      if (lastInstr) {
        if (region.isExit(blockId)) {
          if (!inlining || inst.source.anyFuncEntry() || !isReturnish(inst.op())) {
            irgen::endRegion(irgs);
          }
        } else if (inst.source.anyFuncEntry() || instrAllowsFallThru(inst.op())) {
          irgen::endBlock(irgs, inst.nextSk());
        }
      }
    }
  }

  if (!inlining) {
    irgen::sealUnit(irgs);
  }

  return TranslateResult::Success;
}

}

//////////////////////////////////////////////////////////////////////

std::unique_ptr<IRUnit> irGenRegion(const RegionDesc& region,
                                    const TransContext& context,
                                    PostConditions& pConds) noexcept {
  Timer irGenTimer(Timer::irGenRegion, nullptr);
  SCOPE_ASSERT_DETAIL("RegionDesc") { return show(region); };

  tracing::Block _{"hhir-gen", [&] { return traceProps(context); }};

  std::unique_ptr<IRUnit> unit;
  SCOPE_ASSERT_DETAIL("IRUnit") { return unit ? show(*unit) : "<null>"; };
  TranslateRetryContext retryContext;

  rqtrace::EventGuard trace{"IRGEN"};
  uint32_t tries = 0;

  while (true) {
    int32_t budgetBCInstrs = context.kind == TransKind::Live
      ? Cfg::Jit::MaxLiveRegionInstrs
      : Cfg::Jit::MaxRegionInstrs;
    unit = std::make_unique<IRUnit>(context,
                                    std::make_unique<AnnotationData>());
    unit->initLogEntry(context.initSrcKey.func());
    irgen::IRGS irgs{*unit, &region, budgetBCInstrs, &retryContext};
    irgen::defineFrameAndStack(irgs, region.entry()->initialSpOffset());
    tries++;

    // Set the profCount of the IRUnit's entry block, which is created a
    // priori.
    if (context.kind == TransKind::Optimize) {
      assertx(profData());
      auto entryBID = region.entry()->id();
      assertx(hasTransID(entryBID));
      auto entryTID = getTransID(entryBID);
      auto entryProfCount = region.blockProfCount(entryTID);
      irgs.unit.entry()->setProfCount(entryProfCount);
    }

    try {
      auto const result = irGenRegionImpl(irgs, region, 1);
      if (result == TranslateResult::Retry) {
        tracing::addPoint("translate-retry");
        continue;
      }
      assertx(result == TranslateResult::Success);
    } catch (const FailedTraceGen& e) {
      always_assert_flog(false, "irGenRegion failed with {}\n", e.what());
      continue;
    }

    assertx(budgetBCInstrs >= 0);
    FTRACE(1, "translateRegion: final budgetBCInstrs = {}\n", budgetBCInstrs);

    assertx(!isInlining(irgs));
    // For profiling translations, grab the postconditions to be used for
    // region selection whenever we decide to retranslate.
    assertx(pConds.changed.empty() && pConds.refined.empty());
    if (context.kind == TransKind::Profile &&
        Cfg::Jit::PGOUsePostConditions) {
      auto const lastSrcKey = region.lastSrcKey();
      auto const mainExits = findMainExitBlocks(irgs.unit, lastSrcKey);
      if (mainExits.size() > 0) {

        FTRACE(2, "translateRegion: mainExits: {}\nUnit: {}\n",
               [&]{
                 std::string ret;
                 for (auto& me : mainExits) {
                   fmt::format_to(std::back_inserter(ret), "B{}, ", me->id());
                 }
                 return ret;
               }(),
               show(irgs.unit));

        // Union the post-conditions of all the main-exit blocks.
        for (auto& me : mainExits) {
          FTRACE(2, "  - processing exit B{}\n", me->id());
          pConds |= irgs.irb->fs().postConds(me);
        }
      } else {
        FTRACE(2, "translateRegion: no main exit\n");
      }
    }

    break;
  }

  trace.annotate("tries", folly::to<std::string>(tries));
  trace.finish();
  irGenTimer.stop();

  auto finishPass = [&](const char* msg, int level) {
    printUnit(level, *unit, msg, nullptr, nullptr);
    assertx(checkCfg(*unit));
  };

  finishPass(" after initial translation ", kIRLevel);
  optimize(*unit, context.kind);
  finishPass(" after optimizing ", kOptLevel);

  return unit;
}

bool irGenTryInlineFCall(irgen::IRGS& irgs, SrcKey entry, SSATmp* ctx,
                         Offset asyncEagerOffset, SSATmp* calleeFP,
                         uint32_t posArgc, bool addedUninitNamed,
                         const std::vector<Type>& inputTypes) {
  assertx(entry.anyFuncEntry());
  ctx = ctx ? ctx : cns(irgs, nullptr);

  auto const psk = ProfSrcKey { canonTransID(irgs.profTransIDs), curSrcKey(irgs) };
  int calleeCost{0};

  // See if we have a callee region we can inline.
  auto calleeRegionAndUnit = getInlinableCalleeRegionAndLazyUnit(
    irgs, entry, ctx->type(), psk, calleeCost, addedUninitNamed, inputTypes);
  auto const calleeRegion = calleeRegionAndUnit.region();
  if (!calleeRegion) return false;

  // We shouldn't be inlining profiling translations.
  assertx(irgs.context.kind != TransKind::Profile);
  assertx(calleeRegion->instrSize() <= irgs.budgetBCInstrs || calleeCost <= Cfg::HHIR::AlwaysInlineVasmCostLimit);
  assert_flog(calleeRegion->start().func() == entry.func() &&
              calleeRegion->start().anyFuncEntry() && entry.anyFuncEntry() &&
              calleeRegion->start().numEntryArgs() >= entry.numEntryArgs(),
              "{} != {}", show(calleeRegion->start()), show(entry));

  FTRACE(1, "\nstarting inlined call from {} to {} with {} args "
         "and stack:\n{}\n",
         curFunc(irgs)->fullName()->data(),
         entry.func()->fullName()->data(),
         entry.numEntryArgs(),
         show(irgs));

  if (Cfg::HHIR::EnableInliningPass) {
    auto stitchContext = irgen::InlineStitchingContext {
      irgs.unit, *calleeRegionAndUnit.unit(), irgs.irb.get(), psk.srcKey, entry,
      fp(irgs), sp(irgs), ctx, irgs,
    };
    irgen::stitchCalleeUnit(stitchContext);
    return true;
  }

  emitInitFuncInputsInline(irgs, entry.func(), posArgc, calleeFP);
  beginInlining(irgs, entry, ctx, asyncEagerOffset, calleeCost, calleeFP,
                addedUninitNamed);

  SCOPE_ASSERT_DETAIL("Inlined-RegionDesc")
    { return show(*calleeRegion); };

  // Calculate the profFactor for the callee as the weight of
  // the caller block over the weight of the entry block of
  // the callee region.
  double calleeProfFactor = irgen::curProfCount(irgs);
  auto const calleeEntryBID = calleeRegion->entry()->id();
  if (hasTransID(calleeEntryBID)) {
    assertx(profData());
    auto const calleeTID = getTransID(calleeEntryBID);
    auto calleeProfCount = calleeRegion->blockProfCount(calleeTID);
    if (calleeProfCount == 0) calleeProfCount = 1; // avoid div by zero
    calleeProfFactor /= calleeProfCount;
    assert_flog(calleeProfFactor >= 0, "calleeProfFactor = {:.5}\n",
                calleeProfFactor);
  }

  auto const ignoreBCSize = calleeCost <= Cfg::HHIR::AlwaysInlineVasmCostLimit;
  auto result = irGenRegionImpl(irgs, *calleeRegion, calleeProfFactor,
                                ignoreBCSize);
  assertx(irgs.budgetBCInstrs >= 0);

  if (result != TranslateResult::Success) {
    assertx(result == TranslateResult::Retry);
    throw RetryIRGen("inline-propagate-retry");
  }

  // If the inlined region failed to contain any returns then the
  // rest of this block is dead- we could continue but there's no
  // benefit to inlining this call if it ends in a ReqRetranslate or
  // ReqBind* so instead we mark it as uninlinable and retry.
  if (!irgen::endInlining(irgs, *calleeRegion)) {
    irgs.retryContext->inlineBlacklist.insert(psk);
    throw RetryIRGen("inline-failed");
  }

  // Success.
  return true;
}

std::unique_ptr<IRUnit> irGenInlineRegion(const TransContext& ctx,
                                          const RegionDesc& region) {
  SCOPE_ASSERT_DETAIL("Inline-RegionDesc") { return show(region); };

  std::unique_ptr<IRUnit> unit;
  TranslateRetryContext retryContext;
  auto const entryBID = region.entry()->id();

  while (true) {
    const int32_t budgetBCInstrs = ctx.kind == TransKind::Live
      ? Cfg::Jit::MaxLiveRegionInstrs
      : Cfg::Jit::MaxRegionInstrs;
    // TODO: ctx contains caller info, make inlining cost calc caller agnostic
    unit = std::make_unique<IRUnit>(ctx, std::make_unique<AnnotationData>());
    irgen::IRGS irgs{*unit, &region, budgetBCInstrs, &retryContext};
    irgen::defineFrameAndStack(irgs, SBInvOffset{0});
    irgs.inlineState.conjure = true;
    if (hasTransID(entryBID)) {
      irgs.profTransIDs.clear();
      irgs.profTransIDs.insert(entryBID);
      auto const& mergedBlocks = region.merged(entryBID);
      irgs.profTransIDs.insert(mergedBlocks.begin(), mergedBlocks.end());
    }

    auto const& inputTypes = region.inlineInputTypes();
    auto const ctxType = region.inlineCtxType();

    auto const func = region.entry()->func();

    // Set the profCount of the entry block we just created.
    unit->entry()->setProfCount(curProfCount(irgs));

    SCOPE_ASSERT_DETAIL("Inline-IRUnit") { return show(*unit); };
    irgs.irb->startBlock(unit->entry(), false /* hasUnprocPred */);
    irgen::conjureBeginInlining(irgs, region.start(), ctxType, inputTypes);

    try {
      auto const result = irGenRegionImpl(irgs, region, 1 /* profFactor */,
                                          true /* ignore bc size */);
      if (result == TranslateResult::Retry) continue;
      assertx(result == TranslateResult::Success);
    } catch (const FailedTraceGen& e) {
      FTRACE(2, "irGenInlineRegion failed with {}\n", e.what());
      always_assert_flog(false, "irGenInlineRegion failed with {}\n", e.what());
      continue;
    }

    if (!irgen::conjureEndInlining(irgs, region, func->isCPPBuiltin())) {
      return nullptr;
    }

    irgen::sealUnit(irgs);
    optimize(*unit, TransKind::Optimize);
    return unit;
  }
}

}
