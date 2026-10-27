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

#include "hphp/runtime/base/object-data.h"
#include "hphp/runtime/vm/act-rec.h"
#include "hphp/runtime/vm/resumable.h"

#include "hphp/runtime/vm/jit/types.h"
#include "hphp/runtime/vm/jit/abi.h"
#include "hphp/runtime/vm/jit/code-gen-cf.h"
#include "hphp/runtime/vm/jit/code-gen-helpers.h"
#include "hphp/runtime/vm/jit/extra-data.h"
#include "hphp/runtime/vm/jit/ir-instruction.h"
#include "hphp/runtime/vm/jit/ir-opcode.h"
#include "hphp/runtime/vm/jit/ssa-tmp.h"
#include "hphp/runtime/vm/jit/translator-inline.h"
#include "hphp/runtime/vm/jit/type.h"
#include "hphp/runtime/vm/jit/vasm-gen.h"
#include "hphp/runtime/vm/jit/vasm-instr.h"
#include "hphp/runtime/vm/jit/vasm-reg.h"

#include "hphp/runtime/ext/asio/asio-blockable.h"
#include "hphp/runtime/ext/asio/ext_concurrent-wait-handle.h"
#include "hphp/runtime/ext/asio/ext_async-function-wait-handle.h"
#include "hphp/runtime/ext/asio/ext_async-generator.h"
#include "hphp/runtime/ext/asio/ext_static-wait-handle.h"
#include "hphp/runtime/ext/asio/ext_wait-handle.h"
#include "hphp/runtime/ext/asio/ext_waitable-wait-handle.h"
#include "hphp/runtime/ext/generator/ext_generator.h"

#include "hphp/util/asm-x64.h"
#include "hphp/util/trace.h"

namespace HPHP::jit::irlower {

TRACE_SET_MOD(irlower)

///////////////////////////////////////////////////////////////////////////////

void cgStArResumeAddr(IRLS& env, const IRInstruction* inst) {
  auto const ar = srcLoc(env, inst, 0).reg();
  auto& v = vmain(env);

  auto const addrOff = Resumable::resumeAddrOff() - Resumable::arOff();
  auto const offsetOff = Resumable::suspendOffsetOff() - Resumable::arOff();
  v << store{srcLoc(env, inst, 1).reg(), ar[addrOff]};
  v << storeli{inst->extra<SuspendOffset>()->off, ar[offsetOff]};
}

///////////////////////////////////////////////////////////////////////////////

namespace {

ptrdiff_t genOffset(bool isAsync) {
  return isAsync ? AsyncGenerator::objectOff() : Generator::objectOff();
}

}

IMPL_OPCODE_CALL(CreateGen)
IMPL_OPCODE_CALL(CreateAGen)

void cgContEnter(IRLS& env, const IRInstruction* inst) {
  auto const sp = srcLoc(env, inst, 0).reg();
  auto const fp = srcLoc(env, inst, 1).reg();
  auto const genFP = srcLoc(env, inst, 2).reg();
  auto const target = srcLoc(env, inst, 3).reg();
  auto const sendVal = srcLoc(env, inst, 4);

  auto const extra = inst->extra<ContEnter>();
  auto const spOff = extra->spOffset;
  auto const callOffAndFlags = safe_cast<int32_t>(
    ActRec::encodeCallOffsetAndFlags(extra->callBCOffset, 0));

  auto& v = vmain(env);

  auto const next = v.makeBlock();

  v << store{fp, genFP[AROFF(m_sfp)]};
  v << storeli{callOffAndFlags, genFP[AROFF(m_callOffAndFlags)]};

  v << pushvmfp{genFP};
  auto const sync_sp = v.makeReg();
  v << lea{sp[cellsToBytes(spOff.offset - 1)], sync_sp};
  v << syncvmsp{sync_sp};

  storeTV(v, sync_sp[0], sendVal, inst->src(4));

  v << contenter{genFP, target, cross_trace_regs_resumed(),
                 {next, label(env, inst->taken())}};
  v = next;

  auto const dst = dstLoc(env, inst, 0);
  auto const type = inst->dst()->type();
  if (!type.admitsSingleVal()) {
    v << defvmretdata{dst.reg(0)};
  }
  if (type.needsReg()) {
    v << defvmrettype{dst.reg(1)};
  }
}

void cgContCheckNext(IRLS& env, const IRInstruction* inst) {
  auto const cont = srcLoc(env, inst, 0).reg();
  auto const checkStarted = inst->src(1)->boolVal();
  auto const isAsync = inst->extra<IsAsyncData>()->isAsync;
  auto& v = vmain(env);

  auto const sf = v.makeReg();

  // These asserts make sure that the startedCheck work.
  static_assert(uint8_t(BaseGenerator::State::Created) == 0, "used below");
  static_assert(uint8_t(BaseGenerator::State::Started) == 1, "used below");

  // Take exit if state != 1 (checkStarted) or if state > 1 (!checkStarted).
  auto const cc = checkStarted ? CC_NE : CC_A;
  auto const stateOff = BaseGenerator::stateOff() - genOffset(isAsync);
  v << cmpbim{int8_t(BaseGenerator::State::Started), cont[stateOff], sf};
  v << jcc{cc, sf, {label(env, inst->next()), label(env, inst->taken())}};
}

///////////////////////////////////////////////////////////////////////////////

void cgContValid(IRLS& env, const IRInstruction* inst) {
  auto const dst = dstLoc(env, inst, 0).reg();
  auto const cont = srcLoc(env, inst, 0).reg();
  auto const isAsync = inst->extra<IsAsyncData>()->isAsync;
  auto& v = vmain(env);

  // Return true if generator state is not Done.
  auto const sf = v.makeReg();
  auto const stateOff = BaseGenerator::stateOff() - genOffset(isAsync);
  v << cmpbim{int8_t(BaseGenerator::State::Done), cont[stateOff], sf};
  v << setcc{CC_NE, sf, dst};
}

void cgStContArState(IRLS& env, const IRInstruction* inst) {
  auto const fp = srcLoc(env, inst, 0).reg();
  auto& v = vmain(env);
  v << storebi{
    static_cast<int8_t>(inst->extra<StContArState>()->state),
    fp[BaseGenerator::stateOff() - BaseGenerator::arOff()]
  };
}

///////////////////////////////////////////////////////////////////////////////

void cgLdContActRec(IRLS& env, const IRInstruction* inst) {
  auto const dst = dstLoc(env, inst, 0).reg();
  auto const cont = srcLoc(env, inst, 0).reg();
  auto const isAsync = inst->extra<IsAsyncData>()->isAsync;
  auto const arOff = BaseGenerator::arOff() - genOffset(isAsync);
  vmain(env) << lea{cont[arOff], dst};
}

void cgLdContArValue(IRLS& env, const IRInstruction* inst) {
  auto const contAR = srcLoc(env, inst, 0).reg();
  auto const valueOff = GENDATAOFF(m_value) - Generator::arOff();
  loadTV(vmain(env), inst->dst(), dstLoc(env, inst, 0), contAR[valueOff]);
}

void cgStContArValue(IRLS& env, const IRInstruction* inst) {
  auto const contAR = srcLoc(env, inst, 0).reg();
  auto const valueOff = GENDATAOFF(m_value) - Generator::arOff();
  storeTV(vmain(env), contAR[valueOff], srcLoc(env, inst, 1), inst->src(1));
}

void cgLdContResumeAddr(IRLS& env, const IRInstruction* inst) {
  auto const dst = dstLoc(env, inst, 0).reg();
  auto const cont = srcLoc(env, inst, 0).reg();
  auto const isAsync = inst->extra<IsAsyncData>()->isAsync;
  auto const addrOff = BaseGenerator::resumeAddrOff() - genOffset(isAsync);
  vmain(env) << load{cont[addrOff], dst};
}

void cgLdContArKey(IRLS& env, const IRInstruction* inst) {
  auto const contAR = srcLoc(env, inst, 0).reg();
  auto const keyOff = GENDATAOFF(m_key) - Generator::arOff();
  loadTV(vmain(env), inst->dst(), dstLoc(env, inst, 0), contAR[keyOff]);
}

void cgStContArKey(IRLS& env, const IRInstruction* inst) {
  auto const contAR = srcLoc(env, inst, 0).reg();
  auto const keyOff = GENDATAOFF(m_key) - Generator::arOff();
  storeTV(vmain(env), contAR[keyOff], srcLoc(env, inst, 1), inst->src(1));
}

void cgContArIncKey(IRLS& env, const IRInstruction* inst) {
  auto const contAR = srcLoc(env, inst, 0).reg();
  auto& v = vmain(env);

  auto const keyOff = GENDATAOFF(m_key) - Generator::arOff();
  v << incqm{contAR[keyOff + TVOFF(m_data)], v.makeReg()};
}

void cgContArIncIdx(IRLS& env, const IRInstruction* inst) {
  auto const dst = dstLoc(env, inst, 0).reg();
  auto const contAR = srcLoc(env, inst, 0).reg();
  auto& v = vmain(env);

  auto const tmp = v.makeReg();
  auto const idxOff = GENDATAOFF(m_index) - Generator::arOff();
  v << load{contAR[idxOff], tmp};
  v << incq{tmp, dst, v.makeReg()};
  v << store{dst, contAR[idxOff]};
}

void cgContArUpdateIdx(IRLS& env, const IRInstruction* inst) {
  auto contAR = srcLoc(env, inst, 0).reg();
  auto newIdx = srcLoc(env, inst, 1).reg();
  auto& v = vmain(env);

  auto mem_index = v.makeReg();
  auto const idxOff = GENDATAOFF(m_index) - Generator::arOff();
  v << load{contAR[idxOff], mem_index};

  auto const sf = v.makeReg();
  auto res = v.makeReg();
  v << cmpq{mem_index, newIdx, sf};
  v << cmovq{CC_G, sf, mem_index, newIdx, res};
  v << store{res, contAR[idxOff]};
}

///////////////////////////////////////////////////////////////////////////////

using WH = c_Awaitable;
using AFWH = c_AsyncFunctionWaitHandle;

namespace {

constexpr ptrdiff_t ar_rel(ptrdiff_t off) {
  return off - AFWH::arOff();
}

/*
 * Check if obj is an Awaitable. Passes CC_BE on sf if it is.
 */
void emitIsAwaitable(Vout& v, Vreg obj, Vreg sf) {
  auto constexpr minwh = (int)HeaderKind::WaitHandle;
  auto constexpr maxwh = (int)HeaderKind::ConcurrentWH;
  static_assert(maxwh - minwh == 3, "WH range check needs updating");

  auto const kind = v.makeReg();
  auto const wh_index = v.makeReg();
  v << loadzbl{obj[HeaderKindOffset], kind};
  v << subli{minwh, kind, wh_index, v.makeReg()};
  v << cmpli{maxwh - minwh, wh_index, sf};
}

}

IMPL_OPCODE_CALL(CreateAFWH)
IMPL_OPCODE_CALL(CreateAFWHL)
IMPL_OPCODE_CALL(CreateAGWH)
IMPL_OPCODE_CALL(CreateFSWH)
IMPL_OPCODE_CALL(AFWHPrepareChild)

void cgCreateSSWH(IRLS& env, const IRInstruction* inst) {
  auto& v = vmain(env);
  auto const val = srcLoc(env, inst, 0).reg();
  auto const dst = dstLoc(env, inst, 0).reg();

  if (inst->src(0)->type() <= TNull) {
    auto const handle = c_StaticWaitHandle::NullHandle.handle();
    markRDSAccess(v, handle);
    v << load{rvmtl()[handle], dst};
    emitIncRef(v, dst, TRAP_REASON);
    return;
  }

  if (inst->src(0)->type() <= TBool) {
    auto const trueHandle = c_StaticWaitHandle::TrueHandle.handle();
    auto const falseHandle = c_StaticWaitHandle::FalseHandle.handle();

    if (inst->src(0)->hasConstVal(TBool)) {
      auto const handle = inst->src(0)->boolVal() ? trueHandle : falseHandle;
      markRDSAccess(v, handle);
      v << load{rvmtl()[handle], dst};
      emitIncRef(v, dst, TRAP_REASON);
      return;
    }

    auto const sf = v.makeReg();
    auto const hreg = v.makeReg();
    v << testb{val, val, sf};
    v << cmovq{CC_NZ, sf, v.cns(falseHandle), v.cns(trueHandle), hreg};
    markRDSAccess(v, hreg);
    v << load{hreg[rvmtl()], dst};
    emitIncRef(v, dst, TRAP_REASON);
    return;
  }

  cgCallHelper(
    v,
    env,
    CallSpec::direct(c_StaticWaitHandle::CreateSucceeded),
    callDest(env, inst),
    SyncOptions::None,
    argGroup(env, inst).typedValue(0)
  );
}

void cgCreateCCWH(IRLS& env, const IRInstruction* inst) {
  auto const extra = inst->extra<CreateCCWHData>();

  cgCallHelper(
    vmain(env),
    env,
    CallSpec::direct(c_ConcurrentWaitHandle::fromFrameNoCheck),
    callDest(env, inst),
    SyncOptions::Sync,
    argGroup(env, inst)
      .ssa(0)
      .imm(extra->first)
      .imm(extra->first + extra->count)
      .ssa(1)
  );
}

void cgCountWHNotDone(IRLS& env, const IRInstruction* inst) {
  auto const fp = srcLoc(env, inst, 0).reg();
  auto const extra = inst->extra<CountWHNotDone>();

  auto& v = vmain(env);

  assertx(extra->count > 0);
  auto const start_type_ptr = v.makeReg();
  auto const start_data_ptr = v.makeReg();
  auto const end_ptr = v.makeReg();
  auto const cnt = v.cns(0);
  v << lea{ptrToLocalType(fp, extra->first), start_type_ptr};
  v << lea{ptrToLocalData(fp, extra->first), start_data_ptr};
  v << lea{ptrToLocalType(fp, extra->first + extra->count), end_ptr};

  auto out = doWhile(v, CC_NE, {start_type_ptr, start_data_ptr, cnt},
    [&] (const VregList& in, const VregList& out) {
      auto const type_ptr_in = in[0];
      auto const data_ptr_in = in[1];
      auto const cnt_in = in[2];
      auto const type_ptr_out = out[0];
      auto const data_ptr_out = out[1];
      auto const cnt_out = out[2];
      auto const sf_is_wh = v.makeReg();
      auto const sf_is_finished = v.makeReg();
      auto const sf_cont = v.makeReg();
      auto const obj = v.makeReg();
      auto const cnt_new = v.makeReg();
      auto const loop_cont = v.makeBlock();

      // Skip nulls.
      auto const sf_is_null = v.makeReg();
      emitCmpTVType(v, sf_is_null, KindOfNull, *type_ptr_in);
      ifThen(v, CC_E, sf_is_null, [&] (Vout& v) {
        v << phijmp{loop_cont, v.makeTuple({cnt_in})};
      });

      // Take exit on non-objects.
      auto const sf_is_obj = v.makeReg();
      emitCmpTVType(v, sf_is_obj, KindOfObject, *type_ptr_in);
      fwdJcc(v, env, CC_NE, sf_is_obj, inst->taken());

      // Take exit on non-Awaitables.
      v << load{*data_ptr_in, obj};
      emitIsAwaitable(v, obj, sf_is_wh);
      fwdJcc(v, env, CC_A, sf_is_wh, inst->taken());

      // We depend on this in the test with 0x0E below.
      static_assert(c_Awaitable::STATE_SUCCEEDED == 0, "");
      static_assert(c_Awaitable::STATE_FAILED == 1, "");

      v << testbim{0x0E, obj[WH::stateOff()], sf_is_finished};
      cond(v, CC_NZ, sf_is_finished, cnt_new,
        [&] (Vout& v) {
          auto ret = v.makeReg();
          v << incq{cnt_in, ret, v.makeReg()};
          return ret;
        },
        [&] (Vout& v) { return cnt_in; }
      );

      v << phijmp{loop_cont, v.makeTuple({cnt_new})};

      v = loop_cont;
      v << phidef{v.makeTuple({cnt_out})};

      nextLocal(v, type_ptr_in, data_ptr_in, type_ptr_out, data_ptr_out);
      v << cmpq{type_ptr_out, end_ptr, sf_cont};
      return sf_cont;
    },
    extra->count
  );

  v << copy{out[2], dstLoc(env, inst, 0).reg()};
}

void cgAFWHBlockOn(IRLS& env, const IRInstruction* inst) {
  auto parentAR = srcLoc(env, inst, 0).reg();
  auto child = srcLoc(env, inst, 1).reg();
  auto& v = vmain(env);

  // parent->setState(STATE_BLOCKED);
  v << storebi{
    AFWH::toKindState(WH::Kind::AsyncFunction, AFWH::STATE_BLOCKED),
    parentAR[ar_rel(AFWH::stateOff())]
  };

  auto const blockableOff = AFWH::childrenOff() + AFWH::Node::blockableOff();

  // parent->m_blockable.m_bits = child->m_parentChain.m_lastParent|Kind::AFWH;
  static_assert(
    uint8_t(AsioBlockable::Kind::AsyncFunctionWaitHandleNode) == 0,
    "AFWH kind must be 0."
  );
  auto const lastParentOff = c_WaitableWaitHandle::parentChainOff() +
                              AsioBlockableChain::lastParentOff();
  auto const prevParentOff = blockableOff + AsioBlockable::bitsOff();

  auto const lastParent = v.makeReg();
  v << load{child[lastParentOff], lastParent};
  v << store{lastParent, parentAR[ar_rel(prevParentOff)]};

  // child->m_parentChain.m_lastParent = &parent->m_blockable;
  auto objToAR = v.makeReg();
  v << lea{parentAR[ar_rel(blockableOff)], objToAR};
  v << store{objToAR, child[lastParentOff]};

  // parent->m_child = child;
  auto const childOff = AFWH::childrenOff() + AFWH::Node::childOff();
  v << store{child, parentAR[ar_rel(childOff)]};
}

void cgAFWHPushTailFrame(IRLS& env, const IRInstruction* inst) {
  auto const wh = srcLoc(env, inst, 0).reg();
  auto const id = inst->src(1)->intVal();
  auto const taken = label(env, inst->taken());
  auto& v = vmain(env);

  // We "own" this AFWH if it has exactly two references: one that we hold,
  // and one that its child holds. This IR op is only used on blocked AFWHs.
  auto constexpr kAFWHHeaderKind = (int32_t)HeaderKind::AsyncFuncWH;
  auto constexpr kAFWHOwnedCount = int32_t{2};

  auto const sf1 = v.makeReg();
  auto const sf2 = v.makeReg();
  v << cmpbim{kAFWHHeaderKind, wh[HeapObject::kind_offset()], sf1};
  ifThen(v, CC_NE, sf1, taken);
  v << cmplim{kAFWHOwnedCount, wh[HeapObject::count_offset()], sf2};
  ifThen(v, CC_NE, sf2, taken);

  // Check that we have room for another ID in m_tailFrameIds by testing its
  // highest bit. See comments in async-function-wait-handle.h for details.
  static_assert(AFWH::kNumTailFrames == 4, "");
  static_assert(sizeof(AsyncFrameId) == 2, "");
  always_assert(0 < id && id <= kMaxAsyncFrameId);
  auto const sf3 = v.makeReg();
  v << testbim{-0b10000000, wh[AFWH::tailFramesOff() + 7], sf3};
  ifThen(v, CC_Z, sf3, taken);

  // Push the new ID into the least-significant bits of m_tailFrameIds.
  auto const old_val = v.makeReg();
  auto const shifted = v.makeReg();
  auto const new_val = v.makeReg();
  auto const shift = safe_cast<int32_t>(8 * sizeof(AsyncFrameId));
  v << load{wh[AFWH::tailFramesOff()], old_val};
  v << shlqi{shift, old_val, shifted, v.makeReg()};
  v << orqi{safe_cast<int32_t>(id), shifted, new_val, v.makeReg()};
  v << store{new_val, wh[AFWH::tailFramesOff()]};
}

///////////////////////////////////////////////////////////////////////////////

void cgIsWaitHandle(IRLS& env, const IRInstruction* inst) {
  auto const dst = dstLoc(env, inst, 0).reg();
  auto const obj = srcLoc(env, inst, 0).reg();
  auto& v = vmain(env);
  auto const sf = v.makeReg();

  emitIsAwaitable(v, obj, sf);
  v << setcc{CC_BE, sf, dst};
}

void cgLdWHState(IRLS& env, const IRInstruction* inst) {
  auto const dst = dstLoc(env, inst, 0).reg();
  auto const obj = srcLoc(env, inst, 0).reg();
  auto& v = vmain(env);

  auto const state = v.makeReg();
  v << loadzbq{obj[WH::stateOff()], state};
  v << andqi{0x0F, state, dst, v.makeReg()};
}

void cgLdWHNotDone(IRLS& env, const IRInstruction* inst) {
  auto const dst = dstLoc(env, inst, 0).reg();
  auto const obj = srcLoc(env, inst, 0).reg();
  auto& v = vmain(env);

  // We depend on this in the test with 0x0E below.
  static_assert(c_Awaitable::STATE_SUCCEEDED == 0, "");
  static_assert(c_Awaitable::STATE_FAILED == 1, "");

  auto const sf = v.makeReg();
  auto const result = v.makeReg();
  v << testbim{0x0E, obj[WH::stateOff()], sf};
  v << setcc{CC_NZ, sf, result};
  v << movzbq{result, dst};
}

void cgLdWHResult(IRLS& env, const IRInstruction* inst) {
  auto const obj = srcLoc(env, inst, 0).reg();
  loadTV(vmain(env), inst->dst(), dstLoc(env, inst, 0), obj[WH::resultOff()]);
}

void cgLdAFWHActRec(IRLS& env, const IRInstruction* inst) {
  auto const dst = dstLoc(env, inst, 0).reg();
  auto const obj = srcLoc(env, inst, 0).reg();
  auto& v = vmain(env);
  v << lea{obj[AFWH::arOff()], dst};
}

///////////////////////////////////////////////////////////////////////////////

}
