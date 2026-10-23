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

#include "hphp/runtime/vm/jit/ir-unit.h"

#include "hphp/runtime/vm/jit/block.h"
#include "hphp/runtime/vm/jit/cfg.h"
#include "hphp/runtime/vm/jit/frame-state.h"
#include "hphp/util/configs/jit.h"
#include "hphp/util/timer.h"

#include <fmt/format.h>

namespace HPHP::jit {
///////////////////////////////////////////////////////////////////////////////

TRACE_SET_MOD(hhir)

///////////////////////////////////////////////////////////////////////////////

IRUnit::IRUnit(TransContext context,
               std::unique_ptr<AnnotationData> annotationData)
  : annotationData(std::move(annotationData))
  , m_context(context)
{
  // Setup m_entry after property initialization, since it depends on
  // the value of m_defHint.
  // For Optimize translations, the entry block's profCount is
  // adjusted later in translateRegion.
  m_entry = defBlock();
  m_startNanos = HPHP::Timer::GetThreadCPUTimeNanos();
}

const PackageInfo& IRUnit::packageInfo() {
  assertx(m_context.packageInfo);
  return *m_context.packageInfo;
}

void IRUnit::initLogEntry(const Func* func) {
  if (func ? func->shouldSampleJit() :
      StructuredLog::coinflip(Cfg::Jit::SampleRate)) {
    m_logEntry.emplace();
  }
}

IRInstruction* IRUnit::defLabel(unsigned numDst, Block* b,
                                const BCContext& bcctx) {
  IRInstruction inst(DefLabel, bcctx);
  auto const label = clone(&inst);
  if (numDst > 0) {
    auto const dstsPtr = new (m_arena) SSATmp*[numDst];
    for (unsigned i = 0; i < numDst; ++i) {
      dstsPtr[i] = newSSATmp(label);
    }
    label->setDsts(numDst, dstsPtr);
  }
  b->prepend(label);
  return label;
}

void IRUnit::expandLabel(IRInstruction* label, unsigned extraDst) {
  assertx(label->is(DefLabel));
  assertx(extraDst > 0);
  auto const dstsPtr = new (m_arena) SSATmp*[extraDst + label->numDsts()];
  unsigned i = 0;
  for (auto dst : label->dsts()) {
    dstsPtr[i++] = dst;
  }
  for (unsigned j = 0; j < extraDst; j++) {
    dstsPtr[i++] = newSSATmp(label);
  }
  label->setDsts(i, dstsPtr);
}

void IRUnit::expandJmp(IRInstruction* jmp, SSATmp* value) {
  assertx(jmp->is(Jmp));
  auto const newSrcs = new (m_arena) SSATmp*[jmp->numSrcs() + 1];
  size_t i = 0;
  for (auto src : jmp->srcs()) {
    newSrcs[i++] = src;
  }
  newSrcs[i++] = value;
  jmp->setSrcs(i, newSrcs);
}

Block* IRUnit::defBlock(uint64_t profCount /* =1 */,
                        Block::Hint hint   /* =Neither */ ) {
  FTRACE(2, "IRUnit defining B{}\n", m_nextBlockId);
  if (hint == Block::Hint::Neither) {
    hint = m_defHint;
  }
  auto const block = new (m_arena) Block(m_nextBlockId++, profCount, hint);
  assertx(!block->isEntry() || block->hint() >= Block::Hint::Neither);

  return block;
}

SSATmp* IRUnit::cns(Type type) {
  assertx(type.hasConstVal() ||
         type.subtypeOfAny(TUninit, TInitNull, TNullptr));
  IRInstruction inst(DefConst, BCContext{});
  inst.setTypeParam(type);
  if (SSATmp* tmp = m_constTable.lookup(&inst)) {
    assertx(tmp->type() == type);
    return tmp;
  }
  return m_constTable.insert(clone(&inst)->dst());
}

///////////////////////////////////////////////////////////////////////////////

/*
 * Returns true iff `block' ends the IR unit after successfully finishing
 * execution of the bytecode instruction at `sk', without throwing an exception.
 */
static bool endsUnitAtSrcKey(const Block* block, SrcKey sk) {
  if (!block->isExitNoThrow()) return false;

  const auto& inst = block->back();
  const auto  instSk = inst.marker().sk();

  switch (inst.op()) {
    // These instructions end a unit after executing the bytecode
    // instruction they correspond to.
    case InterpOneCF:
    case JmpExit:
    case Unreachable:
    case EndBlock:
      return instSk == sk;

    // The RetCtrl is generally ending a bytecode instruction, with the
    // exception being in an Await bytecode instruction, where we consider the
    // end of the bytecode instruction to be the non-suspending path.
    case RetCtrl: {
      auto const op = inst.marker().sk().op();
      return op != Op::Await && op != Op::AwaitAll;
    }

    case AsyncFuncRet:
    case AsyncFuncRetSlow:
    case AsyncGenRetR:
    case AsyncGenYieldR:
      return true;

    // A ReqBindJmp ends a unit and it jumps to the next instruction to
    // execute.
    case ReqBindJmp: {
      auto destSk = inst.extra<ReqBindJmp>()->target;
      return sk.succSrcKeys().contains(destSk);
    }

    default:
      return false;
  }
}

jit::vector<Block*> findMainExitBlocks(const IRUnit& unit, SrcKey lastSk) {
  jit::vector<Block*> mainExits;

  FTRACE(5, "findMainExitBlocks: looking for exit at {} in unit:\n{}\n",
         showShort(lastSk), show(unit));

  for (auto block : rpoSortCfg(unit)) {
    if (block->hint() != Block::Hint::Unused &&
        endsUnitAtSrcKey(block, lastSk)) {
      mainExits.push_back(block);
    }
  }

  FTRACE(5, "findMainExitBlocks: mainExits = {}\n",
         [&]{
           std::string ret;
           for (auto& me : mainExits) {
             fmt::format_to(std::back_inserter(ret), "B{}, ", me->id());
           }
           return ret;
         }());

  return mainExits;
}

///////////////////////////////////////////////////////////////////////////////

}
