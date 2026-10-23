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
#include "hphp/runtime/vm/jit/opt.h"

#include "hphp/runtime/base/perf-warning.h"

#include "hphp/runtime/vm/jit/analysis.h"
#include "hphp/runtime/vm/jit/block.h"
#include "hphp/runtime/vm/jit/cfg.h"
#include "hphp/runtime/vm/jit/containers.h"
#include "hphp/runtime/vm/jit/ir-instruction.h"
#include "hphp/runtime/vm/jit/mutation.h"
#include "hphp/runtime/vm/jit/ssa-tmp.h"
#include "hphp/runtime/vm/jit/state-vector.h"
#include "hphp/runtime/vm/jit/pass-tracer.h"
#include "hphp/runtime/vm/jit/timer.h"

#include "hphp/util/dataflow-worklist.h"

#include <sstream>
#include <fmt/format.h>
#include <folly/small_vector.h>

namespace HPHP::jit {

TRACE_SET_MOD(hhir_gvn)

//////////////////////////////////////////////////////////////////////

namespace {

struct ValueNumberMetadata {
  SSATmp* key;
  SSATmp* value;
};

using ValueNumberTable = StateVector<SSATmp, ValueNumberMetadata>;
using DstIndex = int32_t;

struct CongruenceHasher {
  using KeyType = std::pair<IRInstruction*, DstIndex>;

  explicit CongruenceHasher(const ValueNumberTable& globalTable)
    : m_globalTable(&globalTable)
  {
  }

  size_t hashSharedImpl(IRInstruction* inst, size_t result) const {
    if (inst->hasExtra()) {
      result = folly::hash::hash_128_to_64(
        result,
        hashExtra(inst->op(), inst->rawExtra())
      );
    }

    if (inst->hasTypeParam()) {
      result = folly::hash::hash_128_to_64(result, inst->typeParam().hash());
    }

    return result;
  }

  size_t hashSrcs(KeyType key, size_t result) const {
    auto& table = *m_globalTable;
    auto inst = key.first;
    for (uint32_t i = 0; i < inst->numSrcs(); ++i) {
      auto src = canonical(inst->src(i));
      assertx(table[src].value);
      result = folly::hash::hash_128_to_64(
        result,
        reinterpret_cast<size_t>(table[src].value)
      );
    }
    return result;
  }

  size_t operator()(KeyType key) const {
    auto inst = key.first;

    // Note: this doesn't take commutativity or associativity into account, but
    // it might be nice to do so for the opcodes where it makes sense.
    size_t result = static_cast<size_t>(inst->op());
    result = hashSrcs(key, result);
    return hashSharedImpl(inst, result);
  }

private:
  const ValueNumberTable* m_globalTable;
};

struct CongruenceComparator {
  using KeyType = std::pair<IRInstruction*, DstIndex>;

  explicit CongruenceComparator(const ValueNumberTable& globalTable)
    : m_globalTable(&globalTable)
  {
  }

  bool compareSrcs(KeyType keyA, KeyType keyB) const {
    auto& table = *m_globalTable;
    auto instA = keyA.first;
    auto instB = keyB.first;

    for (uint32_t i = 0; i < instA->numSrcs(); ++i) {
      auto srcA = canonical(instA->src(i));
      auto srcB = canonical(instB->src(i));
      assertx(table[srcA].value);
      assertx(table[srcB].value);
      if (table[srcA].value != table[srcB].value) return false;
    }

    return true;
  }

  bool operator()(KeyType keyA, KeyType keyB) const {
    auto instA = keyA.first;
    auto instB = keyB.first;

    // Note: this doesn't take commutativity or associativity into account, but
    // it might be nice to do so for the opcodes where it makes sense.
    if (instA->op() != instB->op()) return false;
    if (instA->numSrcs() != instB->numSrcs()) return false;
    if (instA->hasTypeParam() != instB->hasTypeParam()) return false;
    if (instA->hasTypeParam() && instA->typeParam() != instB->typeParam()) {
      return false;
    }

    if (instA->hasExtra()) {
      assertx(instB->hasExtra());
      if (!equalsExtra(instA->op(), instA->rawExtra(), instB->rawExtra())) {
        return false;
      }
    }

    if (!compareSrcs(keyA, keyB)) {
      return false;
    }

    return true;
  }

private:
  const ValueNumberTable* m_globalTable;
};

using NameTable = jit::hash_map<
  std::pair<IRInstruction*, DstIndex>, SSATmp*,
  CongruenceHasher,
  CongruenceComparator
>;

struct GVNState {
  ValueNumberTable* localTable{nullptr};
  ValueNumberTable* globalTable{nullptr};
  NameTable* nameTable{nullptr};
};

bool supportsGVN(const IRInstruction* inst) {
  switch (inst->op()) {
  case AssertType:
  case AbsDbl:
  case AddInt:
  case SubInt:
  case MulInt:
  case AndInt:
  case AddDbl:
  case SubDbl:
  case MulDbl:
  case DivDbl:
  case DivInt:
  case Mod:
  case Sqrt:
  case OrInt:
  case XorInt:
  case Shl:
  case Shr:
  case Floor:
  case Ceil:
  case MulIntO:
  case XorBool:
  case ConvDblToBool:
  case ConvIntToBool:
  case ConvBoolToDbl:
  case ConvIntToDbl:
  case ConvBoolToInt:
  case ConvDblToInt:
  case ConvFuncPrologueFlagsToARFlags:
  case DblAsBits:
  case IntAsPtrToElem:
  case PtrToElemAsInt:
  case GtInt:
  case GteInt:
  case LtInt:
  case LteInt:
  case EqInt:
  case EqClassId:
  case NeqInt:
  case CmpInt:
  case GtDbl:
  case GteDbl:
  case LtDbl:
  case LteDbl:
  case EqDbl:
  case NeqDbl:
  case CmpDbl:
  case GtStr:
  case GteStr:
  case LtStr:
  case LteStr:
  case EqStr:
  case NeqStr:
  case SameStr:
  case NSameStr:
  case CmpStr:
  case GtBool:
  case GteBool:
  case LtBool:
  case LteBool:
  case EqBool:
  case NeqBool:
  case CmpBool:
  case SameObj:
  case NSameObj:
  case SameArrLike:
  case NSameArrLike:
  case GtRes:
  case GteRes:
  case LtRes:
  case LteRes:
  case EqRes:
  case NeqRes:
  case CmpRes:
  case EqCls:
  case EqLazyCls:
  case EqFunc:
  case EqFuncId:
  case EqStrPtr:
  case InstanceOf:
  case InstanceOfIface:
  case InstanceOfIfaceVtable:
  case ExtendsClass:
  case InstanceOfBitmask:
  case NInstanceOfBitmask:
  case InterfaceSupportsArrLike:
  case InterfaceSupportsStr:
  case InterfaceSupportsInt:
  case InterfaceSupportsDbl:
  case HasToString:
  case IsLegacyArrLike:
  case IsWaitHandle:
  case IsCol:
  case LdRDSAddr:
  case LdFrameThis:
  case LdFrameCls:
  case LdClsCtor:
  case DefConst:
  case LdCls:
  case LdClsCached:
  case LdClsInitData:
  case LdClsFromClsMeth:
  case LdSmashableFunc:
  case LdFuncFromClsMeth:
  case LdFuncInOutBits:
  case LdFuncVecLen:
  case LdClsMethod:
  case LdIfaceMethod:
  case LdPropAddr:
  case LdPublicFunc:
  case LdObjClass:
  case LdClsName:
  case LdLazyClsName:
  case LdEnumClassLabelName:
  case Mov:
  case LdContActRec:
  case LdAFWHActRec:
  case LdVecElemAddr:
  case OrdStr:
  case ChrInt:
  case CheckRange:
  case CountVec:
  case CountDict:
  case CountKeyset:
  case BespokeGet:
  case BespokeGetThrow:
  case BespokeIterEnd:
  case LdMonotypeDictTombstones:
  case LdMonotypeDictKey:
  case LdMonotypeDictVal:
  case LdMonotypeVecElem:
  case LdTypeStructureVal:
  case LdTypeStructureValCns:
  case Select:
  case StrictlyIntegerConv:
  case LookupSPropSlot:
  case ConvPtrToLval:
  case RaiseErrorOnInvalidIsAsExpressionType:
  case LdUnitPerRequestFilepath:
  case DirFromFilepath:
  case AKExistsDict:
  case AKExistsKeyset:
  case DictGet:
  case DictGetK:
  case DictGetQuiet:
  case DictIdx:
  case DictIsset:
  case DictIterEnd:
  case KeysetGet:
  case KeysetGetK:
  case KeysetGetQuiet:
  case KeysetIdx:
  case KeysetIsset:
  case KeysetIterEnd:
  case CheckDictOffset:
  case CheckKeysetOffset:
  case CheckDictKeys:
  case CheckArrayCOW:
  case CheckMissingKeyInArrLike:
  case LdFuncRequiredCoeffects:
  case FuncHasAttr:
  case ClassHasAttr:
  case StructDictSlot:
  case StructDictElemAddr:
  case StructDictSlotInPos:
  case LdStructDictKey:
  case LdStructDictVal:
  case LdClsPropAddrOrNull:
  case LdClsPropAddrOrRaise:
  case LdPtrIterKey:
  case LdPtrIterVal:
  case GetDictPtrIter:
  case GetKeysetPtrIter:
  case BespokeIterGetKey:
  case BespokeIterGetVal:
    return true;

  case EqArrLike:
  case NeqArrLike:
    // Keyset equality comparisons never re-enter or throw
    return inst->src(0)->type() <= TKeyset && inst->src(1)->type() <= TKeyset;

  case IsTypeStruct:
    // Resources can change type without generating a new SSATmp,
    // so its not safe to GVN
    return !opcodeMayRaise(IsTypeStruct) && !inst->src(1)->type().maybe(TRes);

  case LdFuncCached: {
    // Do not need to reload a function within a translation except if it
    // could have been renamed.
    auto const funcName = inst->extra<FuncNameData>()->name;
    return !RO::funcIsRenamable(funcName);
  }
  default:
    return false;
  }
}

void initWithInstruction(IRInstruction* inst, ValueNumberTable& table) {
  // Each SSATmp starts out as the canonical name for itself.
  for (auto dst : inst->dsts()) {
    table[dst] = ValueNumberMetadata { dst, dst };
  }

  for (auto src : inst->srcs()) {
    table[src] = ValueNumberMetadata { src, src };
  }
}

bool visitInstruction(
  GVNState& env,
  IRInstruction* inst
) {
  auto& globalTable = *env.globalTable;
  auto& localTable = *env.localTable;
  auto& nameTable = *env.nameTable;

  if (!supportsGVN(inst)) return false;

  bool changed = false;
  for (auto dstIdx = 0; dstIdx < inst->numDsts(); ++dstIdx) {
    auto dst = inst->dst(dstIdx);
    assertx(dst);

    auto result = nameTable.emplace(std::make_pair(inst, dstIdx), dst);
    SSATmp* temp = result.second ? dst : result.first->second;
    assertx(temp);

    assertx(globalTable[dst].value);
    if (temp != globalTable[dst].value) {
      localTable[dst] = ValueNumberMetadata { dst, temp };
      FTRACE(1,
        "instruction {}\n"
        "updated value number for dst to dst of {}\n",
        *inst,
        *temp->inst()
      );
      changed = true;
    }
  }
  return changed;
}

bool visitBlock(
  GVNState& env,
  Block* block
) {
  bool changed = false;
  for (auto& inst : *block) {
    changed = visitInstruction(env, &inst) || changed;
  }
  return changed;
}

void applyLocalUpdates(ValueNumberTable& local, ValueNumberTable& global) {
  for (auto metadata : local) {
    if (!metadata.key) continue;
    global[metadata.key] = metadata;
  }
}

void runAnalysis(
  GVNState& env,
  const IRUnit& unit,
  const BlockList& blocks
) {
  for (auto block : blocks) {
    for (auto& inst : *block) {
      initWithInstruction(&inst, *env.globalTable);
    }
  }

  bool changed = true;
  while (changed) {
    // We need a temporary table of updates which we apply after running this
    // iteration of the fixed point. If we change the global ValueNumberTable
    // during the pass, the hash values of the SSATmps will change which is
    // apparently a no-no for unordered_map.
    ValueNumberTable localTable(unit, ValueNumberMetadata{});
    env.localTable = &localTable;
    SCOPE_EXIT { env.localTable = nullptr; };
    {
      CongruenceHasher hash(*env.globalTable);
      CongruenceComparator pred(*env.globalTable);
      NameTable nameTable(0, hash, pred);
      env.nameTable = &nameTable;
      SCOPE_EXIT { env.nameTable = nullptr; };

      changed = false;
      for (auto block : blocks) {
        changed = visitBlock(env, block) || changed;
      }
    }
    applyLocalUpdates(localTable, *env.globalTable);
  }
}

/*
 * When gvn replaces an instruction that produces a refcount, we need
 * to IncRef the replacement value. Its not enough to IncRef it where
 * the replacement occurs (where a refcount would originally have been
 * produced), because the replacement value might not survive that
 * long. For example:
 *
 * (1) t2:Str = CastIntToStr t1:Int
 * ....
 * (2) DecRef t2:Str
 * ....
 * (3) t3:Str = CastIntToStr t1:Int
 *
 * The DecRef in (2) might release t2. So, inserting an IncRef after
 * (3) isn't correct, as t2 will have already been released. Instead,
 * we need to IncRef it right after the canonical instruction
 * (1). However, in general there will be paths from the canonical
 * instruction that don't reach the replaced instruction, so we'll
 * need to insert DecRefs on those paths.
 *
 * As an example, given:
 *
 *        +------+
 *        | Orig |
 *        +------+
 *           |\
 *           | \
 *           |  \    +-----+
 *           |   --->|exit1|
 *           |       +-----+
 *           v
 *        +------+
 *        | Rep1 |
 *        +------+
 *           |\
 *           | \
 *           |  \    +-----+
 *           |   --->|exit2|
 *           |       +-----+
 *           v
 *        +------+
 *        | Rep2 |
 *        +------+
 *
 * Where Orig is a PRc instruction, and Rep1 and Rep2 are identical
 * instructions that are going to be replaced, we need to insert an
 * IncRef after Orig (to ensure its dst lives until Rep1), a DecRef at
 * exit1 (to keep the RefCounts balanced), an IncRef after Rep1 (to
 * ensure Orig's dst lives until Rep2), and a DecRef in exit2 (to keep
 * the RefCounts balanced).
 *
 * We do this by computing the Partial-Anticipability of the candidate
 * instructions (Orig, Rep1 and Rep2 in the example). After each
 * candidate instruction we insert an IncRef if a candidate is
 * Partially-Anticipated-out. If a candidateis
 * Partially-Anticipated-out in a predecessor, but not
 * Partially-Anticipated-in in the successor, then a DecRef is
 * inserted in the successor. Since critical edges are split, this
 * covers all of the cases.
 */
constexpr uint32_t kMaxTrackedPrcs = 64;

struct PrcState {
  using Bits = std::bitset<kMaxTrackedPrcs>;

  uint32_t rpoId;

  /* original definition of candidates (kills pant) */
  Bits orig;
  /* local is the set of candidates in this block */
  Bits local;
  /* pantIn = (local | pantOut) - orig */
  Bits pantIn;
  /* pantOut = Union<succs>(pantIn) */
  Bits pantOut;
};

std::string show(const PrcState::Bits& bits) {
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

std::string show(const PrcState& state) {
  return fmt::format(
    "   pantIn  : {}\n"
    "   orig    : {}\n"
    "   local   : {}\n"
    "   pantOut : {}\n",
    show(state.pantIn),
    show(state.orig),
    show(state.local),
    show(state.pantOut)
  );
}

struct PrcEnv {
  PrcEnv(IRUnit& unit, const BlockList& rpoBlocks) :
      unit{unit}, rpoBlocks{rpoBlocks} {}

  IRUnit& unit;
  const BlockList& rpoBlocks;
  // The first element of each inner vector is the canonical SSATmp
  // and the remainder are the SSATmps that will be replaced.
  std::vector<std::vector<SSATmp*>> insertMap;
  std::vector<PrcState> states;
};

void insertIncRefs(PrcEnv& env) {
  auto antQ =
    dataflow_worklist<uint32_t, std::less<uint32_t>>(env.rpoBlocks.size());

  env.states.resize(env.unit.numBlocks());
  for (uint32_t i = 0; i < env.rpoBlocks.size(); i++) {
    auto blk = env.rpoBlocks[i];
    auto& state = env.states[blk->id()];
    state.rpoId = i;
    antQ.push(i);
  }

  auto id = 0;
  for (auto const& v : env.insertMap) {
    assertx(!v.empty());
    for (auto const tmp : v) {
      auto const blk = tmp->inst()->block();
      auto& state = env.states[blk->id()];
      if (!state.local.test(id)) {
        state.local.set(id);
        continue;
      }
    }
    env.states[v[0]->inst()->block()->id()].orig.set(id);
    id++;
  }

  // compute anticipated
  do {
    auto const blk = env.rpoBlocks[antQ.pop()];
    auto& state = env.states[blk->id()];
    state.pantIn = (state.pantOut | state.local) & ~state.orig;
    blk->forEachPred(
      [&] (Block* b) {
        auto& s = env.states[b->id()];
        auto const pantOut = s.pantOut | state.pantIn;
        if (pantOut != s.pantOut) {
          s.pantOut = pantOut;
          antQ.push(s.rpoId);
        }
      }
    );
  } while (!antQ.empty());

  for (auto blk : env.rpoBlocks) {
    auto& state = env.states[blk->id()];
    FTRACE(4,
           "InsertIncDecs: Blk(B{}) <- {}\n"
           "{}"
           "  ->{}\n",
           blk->id(),
           [&] {
             std::string ret;
             blk->forEachPred([&] (Block* pred) {
                 fmt::format_to(std::back_inserter(ret), " B{}", pred->id());
               });
             return ret;
           }(),
           show(state),
           [&] {
             std::string ret;
             blk->forEachSucc([&] (Block* succ) {
                 fmt::format_to(std::back_inserter(ret), " B{}", succ->id());
               });
             return ret;
           }());

    auto inc = state.local;
    for (auto inc_id = 0; inc.any(); inc >>= 1, inc_id++) {
      if (inc.test(0)) {
        auto const& tmps = env.insertMap[inc_id];
        auto insert = [&] (IRInstruction* inst) {
          FTRACE(3, "Inserting IncRef into B{} for t{}\n",
                 blk->id(), tmps[0]->id());
          auto const iter = std::next(blk->iteratorTo(inst));
          blk->insert(iter, env.unit.gen(IncRef, inst->bcctx(), tmps[0]));
        };
        SSATmp* last = nullptr;
        // Insert an IncRef after every candidate in this block except
        // the last one (since we know for all but the last that its
        // successor is anticipated). Note that entries in tmps from
        // the same block are guaranteed to be in program order.
        for (auto const tmp : tmps) {
          if (tmp->inst()->block() != blk) continue;
          if (last) insert(last->inst());
          last = tmp;
        }
        // If it's partially anticipated out, insert an inc after the
        // last one too.
        always_assert(last);
        if (state.pantOut.test(inc_id)) insert(last->inst());
      }
    }

    if (state.pantOut.any()) {
      blk->forEachSucc(
        [&] (Block* succ) {
          auto dec = state.pantOut & ~env.states[succ->id()].pantIn;
          if (!dec.any()) return;
          for (auto dec_id = 0; dec.any(); dec >>= 1, dec_id++) {
            if (!dec.test(0)) continue;
            auto const tmp = env.insertMap[dec_id][0];
            FTRACE(3, "Inserting DecRef into B{} for t{}\n",
                   blk->id(), tmp->id());
            succ->prepend(env.unit.gen(DecRef, tmp->inst()->bcctx(),
                                       DecRefData{}, tmp));
          }
        }
      );
    }
  }
}

using ActionMap = jit::fast_map<SSATmp*, std::vector<SSATmp*>>;

bool tryReplaceInstruction(
  IRUnit& unit,
  const IdomVector& idoms,
  IRInstruction* inst,
  ValueNumberTable& table,
  ActionMap& actionMap
) {
  auto changed = false;
  if (inst->hasDst()) {
    auto const dst = inst->dst();
    auto const valueNumber = table[dst].value;
    if (valueNumber &&
        valueNumber != dst &&
        is_tmp_usable(idoms, valueNumber, inst->block())) {
      changed = true;
      if (inst->producesReference()) {
        auto& v = actionMap[valueNumber];
        if (!v.size()) v.push_back(valueNumber);
        v.push_back(dst);
      }
      if (inst->isBlockEnd()) {
        // Because the output of an equivalent instruction is available, we
        // know that this instruction must take its next branch.
        FTRACE(1,
               "eliminating throwing instruction {}\n",
               inst->toString()
              );
        assertx(inst->next());
        inst->block()->push_back(unit.gen(Jmp, inst->bcctx(), inst->next()));
      }
      if (!(valueNumber->type() <= dst->type())) {
        FTRACE(1,
               "replacing {} with AssertType({})\n",
               inst->toString(),
               dst->type().toString()
              );
        unit.replace(inst, AssertType, dst->type(), valueNumber);
      } else {
        FTRACE(1,
               "replacing {} with Mov({})\n",
               inst->toString(),
               *valueNumber
              );
        unit.replace(inst, Mov, valueNumber);
      }
    }
  }

  for (uint32_t i = 0; i < inst->numSrcs(); ++i) {
    auto s = inst->src(i);
    auto valueNumber = table[s].value;
    if (valueNumber == s) continue;
    if (!valueNumber) continue;
    if (!is_tmp_usable(idoms, valueNumber, inst->block())) continue;

    // If the replacement is at least as refined as the source type,
    // just substitute it directly
    if (valueNumber->type() <= s->type()) {
      FTRACE(1,
             "instruction {}\n"
             "replacing src {} with dst of {}\n",
             *inst,
             i,
             *valueNumber->inst()
            );
      inst->setSrc(i, valueNumber);
      changed = true;
    }
  }

  return changed;
}

bool replaceRedundantComputations(
  IRUnit& unit,
  const IdomVector& idoms,
  const BlockList& blocks,
  ValueNumberTable& table
) {
  ActionMap actionMap;
  auto changed = false;
  for (auto block : blocks) {
    for (auto& inst : *block) {
      changed |= tryReplaceInstruction(unit, idoms, &inst, table, actionMap);
    }
  }
  if (actionMap.empty()) return changed;
  PrcEnv env(unit, blocks);
  for (auto& elm : actionMap) {
    if (env.insertMap.size() == kMaxTrackedPrcs) {
      // This pretty much doesn't happen; when it does, we might be
      // over-increffing here - but thats not a big deal.
      auto const newTmp = elm.second[0];
      auto const block = newTmp->inst()->block();
      auto const iter = std::next(block->iteratorTo(newTmp->inst()));
      for (auto i = 1; i < elm.second.size(); i++) {
        block->insert(iter, unit.gen(IncRef, newTmp->inst()->bcctx(), newTmp));
      }
      continue;
    }
    env.insertMap.push_back(std::move(elm.second));
    if (env.insertMap.size() >= kMaxTrackedPrcs) {
      logPerfWarning(
        "gvnMaxPrcs",
        [&](StructuredLogEntry& cols) {
          auto const func = unit.context().initSrcKey.func();
          cols.setStr("func", func->fullName()->slice());
          cols.setStr("filename", func->unit()->origFilepath()->slice());
          cols.setStr("hhir_unit", show(unit));
        }
      );
    }
  }
  insertIncRefs(env);
  return changed;
}

} // namespace

/////////////////////////////////////////////////////////////////////////

bool gvn(IRUnit& unit) {
  PassTracer tracer{&unit, Trace::hhir_gvn, "gvn"};
  Timer t(Timer::optimize_gvn, unit.logEntry().get_pointer());
  splitCriticalEdges(unit);

  GVNState state;
  auto const rpoBlocks = rpoSortCfg(unit);
  auto const idoms = findDominators(
    unit,
    rpoBlocks,
    numberBlocks(unit, rpoBlocks)
  );

  ValueNumberTable globalTable(unit, ValueNumberMetadata{});
  state.globalTable = &globalTable;

  // This is an implementation of the RPO version of the global value numbering
  // algorithm presented in the 1996 paper "SCC-based Value Numbering" by
  // Cooper and Simpson.
  runAnalysis(state, unit, rpoBlocks);
  auto const changed =
    replaceRedundantComputations(unit, idoms, rpoBlocks, globalTable);
  state.globalTable = nullptr;
  // We might have added a new use of a SSATmp past a CheckType or
  // AssertType on it, so refine if necessary.
  return changed;
}

}
