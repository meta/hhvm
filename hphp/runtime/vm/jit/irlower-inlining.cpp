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

#include "hphp/runtime/vm/jit/irlower-internal.h"

#include "hphp/runtime/vm/act-rec.h"
#include "hphp/runtime/vm/func.h"

#include "hphp/runtime/vm/jit/types.h"
#include "hphp/runtime/vm/jit/abi.h"
#include "hphp/runtime/vm/jit/analysis.h"
#include "hphp/runtime/vm/jit/code-gen-helpers.h"
#include "hphp/runtime/vm/jit/extra-data.h"
#include "hphp/runtime/vm/jit/ir-instruction.h"
#include "hphp/runtime/vm/jit/ir-opcode.h"
#include "hphp/runtime/vm/jit/ssa-tmp.h"
#include "hphp/runtime/vm/jit/translator-inline.h"
#include "hphp/runtime/vm/jit/vasm-gen.h"
#include "hphp/runtime/vm/jit/vasm-instr.h"
#include "hphp/runtime/vm/jit/vasm-reg.h"

#include "hphp/util/configs/hhir.h"
#include "hphp/util/trace.h"

namespace HPHP::jit::irlower {

TRACE_SET_MOD(irlower)

///////////////////////////////////////////////////////////////////////////////

void cgDefCalleeFP(IRLS& env, const IRInstruction* inst) {
  auto const callerSP = srcLoc(env, inst, 0).reg();
  auto const calleeFP = dstLoc(env, inst, 0).reg();
  auto const extra = inst->extra<DefCalleeFP>();
  auto& v = vmain(env);

  // We could use callerFP in a non-resumed context but vasm-copy should clean
  // this up for us since callerSP was computed as an lea{} from rvmfp.
  v << lea{callerSP[cellsToBytes(extra->spOffset.offset)], calleeFP};
  if (!inst->marker().sk().prologue()) v.unit().allocFrame(inst);
}

void cgEnterInlineFrame(IRLS& env, const IRInstruction* inst) {
  auto& v = vmain(env);
  if (!inst->marker().sk().prologue()) {
    auto const it = v.unit().fpToFrame.find(inst->src(0));
    assertx(it != v.unit().fpToFrame.end());
    v.unit().frames[it->second].entry_weight = vmain(env).weight();
  }

  v << inlinestart{};
}

void cgInlineCall(IRLS& env, const IRInstruction* inst) {
  auto const calleeFP = srcLoc(env, inst, 0).reg();
  auto const callerFP = srcLoc(env, inst, 1).reg();
  auto const calleeFPInst = inst->src(0)->inst();
  assertx(calleeFPInst->is(DefCalleeFP));
  auto const extra = calleeFPInst->extra<DefCalleeFP>();
  auto& v = vmain(env);

  auto const off = [&] () -> int32_t {
    auto const callerFPOff = offsetOfFrame(inst->src(1));
    if (!callerFPOff) return 0;

    auto const calleeFPOff = extra->spOffset;
    return *callerFPOff - calleeFPOff;
  }();

  // Do roughly the same work as an HHIR Call.
  v << store{callerFP, calleeFP[AROFF(m_sfp)]};

  auto const retAddr = v.makeReg();
  v << ldbindretaddr{extra->returnSk, extra->returnSPOff, retAddr};
  v << store{retAddr, calleeFP[AROFF(m_savedRip)]};
  v << pushvmfp{calleeFP, cellsToBytes(off)};
}

void cgLeaveInlineFrame(IRLS& env, const IRInstruction* inst) {
  auto& v = vmain(env);
  auto const fp = srcLoc(env, inst, 0).reg();

  if (Cfg::HHIR::GenerateAsserts) {
    if (env.unit.context().initSrcKey.resumeMode() == ResumeMode::None) {
      for (auto i = 0; i < kNumActRecCells; ++i) {
        trashFullTV(v, fp[i], kTVTrashJITFrame);
      }
    }
  }

  v << inlineend{};
}

void cgInlineSideExit(IRLS& env, const IRInstruction* inst) {
  auto const calleeFP = srcLoc(env, inst, 1).reg();
  auto const target = srcLoc(env, inst, 3).reg();
  auto const extra = inst->extra<InlineSideExit>();
  auto const callee = extra->callee;

  auto& v = vmain(env);
  v << inlineend{};

  auto const coaf = ActRec::encodeCallOffsetAndFlags(
    extra->callBCOff,
    1 << ActRec::IsInlined
  );

  v << copy{target, rarg(0)};
  v << copy{v.cns(callee->getFuncId().toInt()), rarg(1)};
  v << copy{v.cns(coaf), rarg(2)};
  v << syncvmsp{calleeFP};

  auto const done = v.makeBlock();
  v << inlinesideexit{vm_regs_with_sp() | arg_regs(3)};

  // The callee is responsible for unwinding the whole frame, which was already
  // popped from the marker. However, this needs to be adjusted for the empty
  // space reserved for inouts, as we optimize away the uninits.
  auto const marker = inst->marker();
  auto const fixupBcOff = marker.fixupBcOff();
  auto const fixupSpOff = marker.fixupBcSPOff() - callee->numInOutParams();
  v << syncpoint{Fixup::direct(fixupBcOff, fixupSpOff)};
  v << unwind{done, label(env, inst->taken())};
  v = done;

  auto const dst = dstLoc(env, inst, 0);
  auto const type = inst->dst()->type();
  if (!type.admitsSingleVal()) {
    v << defvmretdata{dst.reg(0)};
  }
  if (type.needsReg()) {
    v << defvmrettype{dst.reg(1)};
  }
}

void cgInlineSideExitSyncStack(IRLS&, const IRInstruction*) {
  // Nothing to do here.
}

///////////////////////////////////////////////////////////////////////////////

void cgConjure(IRLS& env, const IRInstruction* inst) {
  auto const dst = dstLoc(env, inst, 0);
  auto& v = vmain(env);
  if (dst.hasReg(0)) {
    v << conjure{dst.reg(0)};
  }
  if (dst.hasReg(1)) {
    v << conjure{dst.reg(1)};
  }
}

///////////////////////////////////////////////////////////////////////////////

}
