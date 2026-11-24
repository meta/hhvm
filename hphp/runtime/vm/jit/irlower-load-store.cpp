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

#include "hphp/runtime/base/implicit-context.h"
#include "hphp/runtime/base/memory-manager.h"

#include "hphp/runtime/vm/jit/abi.h"
#include "hphp/runtime/vm/jit/arg-group.h"
#include "hphp/runtime/vm/jit/call-spec.h"
#include "hphp/runtime/vm/jit/code-gen-cf.h"
#include "hphp/runtime/vm/jit/code-gen-helpers.h"
#include "hphp/runtime/vm/jit/extra-data.h"
#include "hphp/runtime/vm/jit/ir-instruction.h"
#include "hphp/runtime/vm/jit/ir-opcode.h"
#include "hphp/runtime/vm/jit/ssa-tmp.h"
#include "hphp/runtime/vm/jit/translator-inline.h"
#include "hphp/runtime/vm/jit/types.h"
#include "hphp/runtime/vm/jit/vasm-gen.h"
#include "hphp/runtime/vm/jit/vasm-instr.h"
#include "hphp/runtime/vm/jit/vasm-reg.h"

#include "hphp/util/asm-x64.h"
#include "hphp/util/configs/hhir.h"
#include "hphp/util/trace.h"

namespace HPHP::jit::irlower {

TRACE_SET_MOD(irlower)

///////////////////////////////////////////////////////////////////////////////

namespace {

void loadLocOrStk(IRLS& env, const IRInstruction* inst, Vptr src) {
  auto& v = vmain(env);
  auto const dst = dstLoc(env, inst, 0);
  if (!arch::any<arch::ARM>() || !inst->dst()->type().needsReg() ||
      dst.isFullSIMD()) {
    loadTV(v, inst->dst(), dst, src);
    return;
  }

  assertx(dst.hasReg(1));
  auto const type = v.makeReg();
  loadTV(v, inst->dst(), Vloc{dst.reg(0), type}, src, true);
  // The wide type load can pair with the value load, but includes non-type
  // bytes. ARM byte comparisons use W registers, so bits 8..31 must be zero.
  // movtqb emits UXTB to extract the type tag; auxiliary bits are unused
  // in local and stack slots.
  v << movtqb{type, dst.reg(1)};
}

}

///////////////////////////////////////////////////////////////////////////////

void cgKillActRec(IRLS& env, const IRInstruction* inst) {
  auto& v = vmain(env);
  v << killeffects{};
  if (!emitDebugCode()) return;

  auto const fp = srcLoc(env, inst, 0).reg();
  trashBytes(v, fp[0], sizeof(ActRec), kActRecTrashFill);
}

void cgKillLoc(IRLS& env, const IRInstruction* inst) {
  auto& v = vmain(env);
  v << killeffects{};
  if (!emitDebugCode()) return;

  auto const fp = srcLoc(env, inst, 0).reg();
  auto const offset = localOffset(inst->extra<KillLoc>()->locId);
  trashBytes(v, fp[offset], sizeof(TypedValue), kLocalTrashFill);
}

void cgLdLoc(IRLS& env, const IRInstruction* inst) {
  auto const fp = srcLoc(env, inst, 0).reg();
  auto const off = localOffset(inst->extra<LdLoc>()->locId);

  loadLocOrStk(env, inst, fp[off]);
}

void cgLdLocForeign(IRLS& env, const IRInstruction* inst) {
  auto const fp = srcLoc(env, inst, 0).reg();
  auto const locId = srcLoc(env, inst, 1).reg();
  auto& v = vmain(env);
  auto const off = v.makeReg();
  auto const shifted = v.makeReg();
  auto const negated = v.makeReg();
  // calculate offset: (locId + 1) * sizeof(TypedValue)
  v << addqi{1, locId, off, v.makeReg()};
  static_assert(sizeof(TypedValue) == 16, "");
  v << shlqi{4, off, shifted, v.makeReg()};
  v << neg{shifted, negated, v.makeReg()};
  loadTV(vmain(env), inst->dst(), dstLoc(env, inst, 0), fp[negated]);
}

void cgLdLocAddr(IRLS& env, const IRInstruction* inst) {
  auto const fp = srcLoc(env, inst, 0).reg();
  auto const off = localOffset(inst->extra<LdLocAddr>()->locId);
  if (dstLoc(env, inst, 0).hasReg()) {
    vmain(env) << lea{fp[off], dstLoc(env, inst, 0).reg()};
  }
}

void cgStLoc(IRLS& env, const IRInstruction* inst) {
  auto const fp = srcLoc(env, inst, 0).reg();
  auto const off = localOffset(inst->extra<StLoc>()->locId);

  // For ARM, we request the `aux' field to be stored too.  That's unused, but
  // having a wider store for the type allows it to be merged with the store of
  // the value into a single storepair instruction.
  const bool storeAux = arch::any<arch::ARM>();

  storeTV(vmain(env), fp[off], srcLoc(env, inst, 1), inst->src(1),
          TBottom, storeAux);
}

void cgStLocMeta(IRLS&, const IRInstruction*) {}

void cgStLocRange(IRLS& env, const IRInstruction* inst) {
  auto const range = inst->extra<StLocRange>();

  if (range->start >= range->end) return;

  auto const fp = srcLoc(env, inst, 0).reg();
  auto const loc = srcLoc(env, inst, 1);
  auto const val = inst->src(1);
  auto& v = vmain(env);

  auto const typePtr = v.makeReg();
  auto const dataPtr = v.makeReg();
  v << lea{ptrToLocalType(fp, range->start), typePtr};
  v << lea{ptrToLocalData(fp, range->start), dataPtr};

  forLoopUnroll(v, range->end - range->start, {typePtr, dataPtr},
    [&] (Vout& vo, const VregList& in, int iter) {
      auto const offset = -safe_cast<int32_t>(iter * sizeof(TypedValue));
      auto const typeIn = in[0];
      auto const dataIn = in[1];
      storeTV(vo, val->type(), loc, typeIn[offset], dataIn[offset]);
    },
    [&] (Vout& vo, const VregList& in, const VregList& out,
         unsigned iterations) {
      auto const typeIn = in[0];
      auto const dataIn = in[1];
      auto const typeOut = out[0];
      auto const dataOut = out[1];
      nextLocal(vo, typeIn, dataIn, typeOut, dataOut, iterations);
    }
  );
}

void cgDbgTrashFrame(IRLS& env, const IRInstruction* inst) {
  auto const fp = srcLoc(env, inst, 0).reg();
  auto const off = cellsToBytes(inst->extra<DbgTrashFrame>()->offset.offset);
  for (auto i = 0; i < kNumActRecCells; ++i) {
    trashFullTV(vmain(env), fp[off + cellsToBytes(i)], kTVTrashJITFrame);
  }
}

///////////////////////////////////////////////////////////////////////////////

void cgLdStk(IRLS& env, const IRInstruction* inst) {
  auto const sp = srcLoc(env, inst, 0).reg();
  auto const off = cellsToBytes(inst->extra<LdStk>()->offset.offset);

  loadLocOrStk(env, inst, sp[off]);
}

void cgLdStkAddr(IRLS& env, const IRInstruction* inst) {
  auto const sp = srcLoc(env, inst, 0).reg();
  auto const off = cellsToBytes(inst->extra<LdStkAddr>()->offset.offset);
  vmain(env) << lea{sp[off], dstLoc(env, inst, 0).reg()};
}

void cgStStk(IRLS& env, const IRInstruction* inst) {
  auto const sp = srcLoc(env, inst, 0).reg();
  auto const off = cellsToBytes(inst->extra<StStk>()->offset.offset);

  auto const type = inst->hasTypeParam()
    ? inst->typeParam()
    : inst->src(1)->type();

  // For ARM, we request the `aux' field to be stored too.  That's unused, but
  // having a wider store for the type allows it to be merged with the store of
  // the value into a single storepair instruction.
  const bool storeAux = arch::any<arch::ARM>();

  storeTV(vmain(env), sp[off], srcLoc(env, inst, 1), inst->src(1), type,
          storeAux);
}

void cgStStkMeta(IRLS&, const IRInstruction*) {}

void cgStStkRange(IRLS& env, const IRInstruction* inst) {
  auto const range = inst->extra<StackRange>();
  auto const count = range->count;
  if (count == 0) return;

  auto const startOff = range->start.offset;
  auto const sp = srcLoc(env, inst, 0).reg();
  auto const val = inst->src(1);
  auto const valType = val->type();
  auto const valSrcLoc = srcLoc(env, inst, 1);
  auto& v = vmain(env);

  auto const typePtr = v.makeReg();
  auto const dataPtr = v.makeReg();
  v << lea{sp[cellsToBytes(startOff) + TVOFF(m_type)], typePtr};
  v << lea{sp[cellsToBytes(startOff) + TVOFF(m_data)], dataPtr};

  auto const tvSize = safe_cast<int32_t>(sizeof(TypedValue));

  forLoopUnroll(v, count, {typePtr, dataPtr},
    [&] (Vout& vo, const VregList& in, int iter) {
      auto const offset = safe_cast<int32_t>(iter * tvSize);
      auto const typeIn = in[0];
      auto const dataIn = in[1];
      storeTV(vo, valType, valSrcLoc, typeIn[offset], dataIn[offset]);
    },
    [&] (Vout& vo, const VregList& in, const VregList& out,
         unsigned iterations) {
      auto const typeIn = in[0];
      auto const dataIn = in[1];
      auto const typeOut = out[0];
      auto const dataOut = out[1];
      auto const increment = safe_cast<int32_t>(tvSize * iterations);
      vo << addqi{increment, typeIn, typeOut, vo.makeReg()};
      vo << addqi{increment, dataIn, dataOut, vo.makeReg()};
    }
  );
}

void cgStOutValue(IRLS& env, const IRInstruction* inst) {
  auto const fp = srcLoc(env, inst, 0).reg();
  auto const off = cellsToBytes(
    inst->extra<StOutValue>()->index + kNumActRecCells
  );
  storeTV(vmain(env), fp[off], srcLoc(env, inst, 1), inst->src(1));
}

void cgLdOutAddr(IRLS& env, const IRInstruction* inst) {
  auto const fp = srcLoc(env, inst, 0).reg();
  auto const off = cellsToBytes(
    inst->extra<LdOutAddr>()->index + kNumActRecCells
  );
  vmain(env) << lea{fp[off], dstLoc(env, inst, 0).reg()};
}

void cgDbgTrashStk(IRLS& env, const IRInstruction* inst) {
  auto const sp = srcLoc(env, inst, 0).reg();
  auto const off = cellsToBytes(inst->extra<DbgTrashStk>()->offset.offset);
  trashFullTV(vmain(env), sp[off],  kTVTrashJITStk);
}

void cgLdMem(IRLS& env, const IRInstruction* inst) {
  auto const ptr    = inst->src(0);
  auto const ptrLoc = tmpLoc(env, ptr);
  auto const dstLoc = tmpLoc(env, inst->dst());

  loadTV(vmain(env), inst->dst()->type(), dstLoc,
         memTVTypePtr(ptr, ptrLoc), memTVValPtr(ptr, ptrLoc));
}

void cgStMem(IRLS& env, const IRInstruction* inst) {
  auto const ptr    = inst->src(0);
  auto const ptrLoc = tmpLoc(env, ptr);
  auto const src    = inst->src(1);
  auto const srcLoc = tmpLoc(env, src);

  auto const type   = inst->hasTypeParam() ? inst->typeParam() : src->type();

  storeTV(vmain(env), type, srcLoc,
          memTVTypePtr(ptr, ptrLoc), memTVValPtr(ptr, ptrLoc));
}

void cgStMemMeta(IRLS&, const IRInstruction*) {}

void cgLdImplicitContext(IRLS& env, const IRInstruction* inst) {
  auto& v = vmain(env);
  markRDSAccess(v, ImplicitContext::activeCtx.handle());

  auto const dst = dstLoc(env, inst, 0);
  v << load{rvmtl()[ImplicitContext::activeCtx.handle()], dst.reg(0)};
}

void cgLdImplicitContextMemoKey(IRLS& env, const IRInstruction* inst) {
  auto const obj = srcLoc(env, inst, 0).reg();
  auto const dst = dstLoc(env, inst, 0).reg();

  auto& v = vmain(env);
  v << load{
    obj[Native::dataOffset<ImplicitContext>() + ImplicitContext::memoKeyOffset()],
    dst
  };
}

void cgLdMemoAgnosticIC(IRLS& env, const IRInstruction* inst) {
  auto const obj = srcLoc(env, inst, 0).reg();
  auto const dst = dstLoc(env, inst, 0).reg();
  auto& v = vmain(env);
  v << load{
    obj[Native::dataOffset<ImplicitContext>() + ImplicitContext::memoAgnosticOffset()],
    dst
  };
}

void cgStImplicitContext(IRLS& env, const IRInstruction* inst) {
  auto& v = vmain(env);
  assertx(inst->src(0)->isA(TObj));

  auto const data = srcLoc(env, inst, 0).reg(0);
  markRDSAccess(v, ImplicitContext::activeCtx.handle());

  v << store{data, rvmtl()[ImplicitContext::activeCtx.handle()]};
}

void cgDbgTrashMem(IRLS& env, const IRInstruction* inst) {
  auto const ptr    = inst->src(0);
  auto const ptrLoc = tmpLoc(env, ptr);
  trashTV(vmain(env), memTVTypePtr(ptr, ptrLoc), memTVValPtr(ptr, ptrLoc),
          kTVTrashJITHeap);
}

///////////////////////////////////////////////////////////////////////////////

void cgLdClsInitElem(IRLS& env, const IRInstruction* inst) {
  auto const base = srcLoc(env, inst, 0).reg();
  auto const idx = inst->extra<IndexData>()->index;
  auto& v = vmain(env);

  auto const lval_offset = ObjectProps::offsetOf(idx);

  loadTV(v, inst->dst()->type(), dstLoc(env, inst, 0),
         base[lval_offset.typeOffset()],
         base[lval_offset.dataOffset()]);
}

void cgStClsInitElem(IRLS& env, const IRInstruction* inst) {
  auto const base = srcLoc(env, inst, 0).reg();
  auto const idx = inst->extra<IndexData>()->index;
  auto& v = vmain(env);

  auto const lval_offset = ObjectProps::offsetOf(idx);

  storeTV(v, inst->src(1)->type(),
          srcLoc(env, inst, 1),
          base[lval_offset.typeOffset()],
          base[lval_offset.dataOffset()]);
}

///////////////////////////////////////////////////////////////////////////////

void cgLdMIStateTempBaseAddr(IRLS& env, const IRInstruction* inst) {
  auto const off = rds::kVmMInstrStateOff + offsetof(MInstrState, tvTempBase);
  vmain(env) << lea{rvmtl()[off], dstLoc(env, inst, 0).reg()};
}

void cgLdMBase(IRLS& env, const IRInstruction* inst) {
  auto const off = rds::kVmMInstrStateOff + offsetof(MInstrState, base);
  auto const dstLoc = irlower::dstLoc(env, inst, 0);
  vmain(env) << load{rvmtl()[off], dstLoc.reg(0)};
  vmain(env) << load{rvmtl()[off + sizeof(intptr_t)], dstLoc.reg(1)};
}

void cgStMBase(IRLS& env, const IRInstruction* inst) {
  auto const off = rds::kVmMInstrStateOff + offsetof(MInstrState, base);
  auto const srcLoc = irlower::srcLoc(env, inst, 0);
  vmain(env) << store{srcLoc.reg(0), rvmtl()[off]};
  vmain(env) << store{srcLoc.reg(1), rvmtl()[off + sizeof(intptr_t)]};
}

void cgCheckMROProp(IRLS& env, const IRInstruction* inst) {
  auto const off = rds::kVmMInstrStateOff + offsetof(MInstrState, roProp);
  auto& v = vmain(env);
  auto const sf = v.makeReg();
  v << cmpbim{0, rvmtl()[off], sf};
  v << jcc{CC_Z, sf, {label(env, inst->next()), label(env, inst->taken())}};
}

void cgStMROProp(IRLS& env, const IRInstruction* inst) {
  auto const off = rds::kVmMInstrStateOff + offsetof(MInstrState, roProp);
  auto const srcLoc = irlower::srcLoc(env, inst, 0);
  vmain(env) << storeb{srcLoc.reg(0), rvmtl()[off]};
}

///////////////////////////////////////////////////////////////////////////////

static tv_lval ldGblAddrHelper(const StringData* name) {
  return g_context->m_globalNVTable->lookup(name);
}

void cgLdGblAddr(IRLS& env, const IRInstruction* inst) {
  auto& v = vmain(env);
  cgCallHelper(v, env, CallSpec::direct(ldGblAddrHelper),
               callDest(env, inst),
               SyncOptions::Sync, argGroup(env, inst).ssa(0));
}

void cgProfileGlobal(IRLS& env, const IRInstruction* inst) {
  auto& v = vmain(env);
  cgCallHelper(v, env, CallSpec::direct(profileGlobal),
               kVoidDest, SyncOptions::None,
               argGroup(env, inst).ssa(0));
}

IMPL_OPCODE_CALL(LdGblAddrDef)

///////////////////////////////////////////////////////////////////////////////

void cgDeserializeLazyProp(IRLS& env, const IRInstruction* inst) {
  auto const src = srcLoc(env, inst, 0).reg();
  auto const val = Immed(static_cast<int8_t>(kInvalidDataType));
  auto const index = inst->extra<DeserializeLazyProp>()->index;
  auto const offset = ObjectProps::offsetOf(index).shift(sizeof(ObjectData));

  auto& v = vmain(env);
  auto const sf = v.makeReg();
  v << cmpbim{val, src[offset.typeOffset()], sf};

  ifThen(v, CC_Z, sf, [&](Vout& v) {
    auto const type = v.makeReg();
    auto const data = v.makeReg();
    v << lea{src[offset.typeOffset()], type};
    v << lea{src[offset.dataOffset()], data};
    auto const args = argGroup(env, inst).reg(type).reg(data);
    cgCallHelper(v, env, CallSpec::direct(ObjectData::deserializeLazyProp),
                 kVoidDest, SyncOptions::None, args);
  });
}

void cgLdClosureArg(IRLS& env, const IRInstruction* inst) {
  auto const src = srcLoc(env, inst, 0).reg();
  auto const offs = ObjectProps::offsetOf(inst->extra<LdClosureArg>()->index)
    .shift(sizeof(ObjectData));
  loadTV(vmain(env), 
         inst->dst()->type(), 
         dstLoc(env, inst, 0), 
         src[offs.typeOffset()], 
         src[offs.dataOffset()]);
}


void cgLdPropAddr(IRLS& env, const IRInstruction* inst) {
  auto& v = vmain(env);
  auto const dstLoc = irlower::dstLoc(env, inst, 0);
  auto const valReg = dstLoc.reg(tv_lval::val_idx);

  auto const src = srcLoc(env, inst, 0).reg();
  auto const offs = ObjectProps::offsetOf(inst->extra<LdPropAddr>()->index)
    .shift(sizeof(ObjectData));

  v << lea{src[offs.dataOffset()], valReg};
  static_assert(TVOFF(m_data) == 0, "");
  v << lea{src[offs.typeOffset()], dstLoc.reg(tv_lval::type_idx)};
}

void cgLdInitPropAddr(IRLS& env, const IRInstruction* inst) {
  auto const dstLoc = irlower::dstLoc(env, inst, 0);
  auto const obj = srcLoc(env, inst, 0).reg();
  auto& v = vmain(env);

  auto const offs = ObjectProps::offsetOf(inst->extra<LdInitPropAddr>()->index)
    .shift(sizeof(ObjectData));

  if (dstLoc.hasReg(tv_lval::val_idx)) {
    v << lea{obj[offs.dataOffset()], dstLoc.reg(tv_lval::val_idx)};
  }
  if (dstLoc.hasReg(tv_lval::type_idx)) {
    static_assert(TVOFF(m_data) == 0, "");
    v << lea{obj[offs.typeOffset()], dstLoc.reg(tv_lval::type_idx)};
  }

  auto const sf = v.makeReg();
  emitCmpTVType(v, sf, KindOfUninit, obj[offs.typeOffset()]);
  v << jcc{CC_Z, sf, {label(env, inst->next()), label(env, inst->taken())}};
}

IMPL_OPCODE_CALL(LdClsPropAddrOrNull)
IMPL_OPCODE_CALL(LdClsPropAddrOrRaise)

///////////////////////////////////////////////////////////////////////////////

void cgCheckRDSInitialized(IRLS& env, const IRInstruction* inst) {
  auto const handle = inst->extra<CheckRDSInitialized>()->handle;
  auto& v = vmain(env);

  if (rds::isNormalHandle(handle)) {
    auto const sf = checkRDSHandleInitialized(v, handle);
    v << jcc{CC_NE, sf, {label(env, inst->next()), label(env, inst->taken())}};
  } else {
    // Always initialized; just fall through to inst->next().
    assertx(rds::isPersistentHandle(handle));
    DEBUG_ONLY bool initialized =
      rds::handleToRef<bool, rds::Mode::Persistent>(handle);
    assertx(initialized);
  }
}

void cgMarkRDSInitialized(IRLS& env, const IRInstruction* inst) {
  auto const handle = inst->extra<MarkRDSInitialized>()->handle;
  if (rds::isNormalHandle(handle)) markRDSHandleInitialized(vmain(env), handle);
}

void cgMarkRDSAccess(IRLS& env, const IRInstruction* inst) {
  assertx(rds::shouldProfileAccesses());
  markRDSAccess(vmain(env), inst->extra<MarkRDSAccess>()->handle);
}

///////////////////////////////////////////////////////////////////////////////

namespace {

void ldRDSAddrImpl(Vout& v, rds::Handle handle, Vreg dst) {
  if (rds::isPersistentHandle(handle)) {
    // Persistent RDS handles are not relative to RDS base.
    auto const addr = rds::handleToPtr<void, rds::Mode::Persistent>(handle);
    v << copy{v.cns(addr), dst};
  } else {
    v << lea{rvmtl()[handle], dst};
  }
}

}

void cgLdRDSAddr(IRLS& env, const IRInstruction* inst) {
  ldRDSAddrImpl(
    vmain(env),
    inst->extra<LdRDSAddr>()->handle,
    dstLoc(env, inst, 0).reg()
  );
}

void cgLdInitRDSAddr(IRLS& env, const IRInstruction* inst) {
  auto const dst = dstLoc(env, inst, 0).reg();
  auto& v = vmain(env);

  ldRDSAddrImpl(v, inst->extra<LdInitRDSAddr>()->handle, dst);

  auto const sf = v.makeReg();
  emitCmpTVType(v, sf, KindOfUninit, dst[TVOFF(m_type)]);
  v << jcc{CC_Z, sf, {label(env, inst->next()), label(env, inst->taken())}};
}

void cgLdTVFromRDS(IRLS& env, const IRInstruction* inst) {
  auto& v = vmain(env);
  auto const extra = inst->extra<LdTVFromRDS>();
  loadTV(
    v,
    inst->dst(),
    dstLoc(env, inst, 0),
    rvmtl()[extra->handle],
    extra->includeAux
  );
}

void cgStTVInRDS(IRLS& env, const IRInstruction* inst) {
  auto& v = vmain(env);
  auto const extra = inst->extra<StTVInRDS>();
  storeTV(
    v,
    rvmtl()[extra->handle],
    srcLoc(env, inst, 0),
    inst->src(0),
    TBottom,
    extra->includeAux
  );
}

void cgCheckFuncNeedsCoverage(IRLS& env, const IRInstruction* inst) {
  auto& v = vmain(env);

  auto const indexHandle = Func::GetCoverageIndex();
  auto const flagHandle = inst->extra<FuncData>()->func->getCoverageHandle();

  auto const index = v.makeReg();
  auto const flag = v.makeReg();
  auto const sf1 = v.makeReg();
  auto const sf2 = v.makeReg();

  v << loadw{rvmtl()[indexHandle], index};
  v << testw{index, index, sf1};
  ifThen(v, CC_NZ, sf1, [&] (Vout& v) {
    v << loadw{rvmtl()[flagHandle], flag};
    v << cmpw{index, flag, sf2};
    v << jcc{CC_NE, sf2, {label(env, inst->next()), label(env, inst->taken())}};
  });
}

void cgRecordFuncCall(IRLS& env, const IRInstruction* inst) {
  auto const indexHandle = Func::GetCoverageIndex();
  auto const flagHandle = inst->extra<FuncData>()->func->getCoverageHandle();

  auto& v = vmain(env);
  auto const index = v.makeReg();

  v << loadw{rvmtl()[indexHandle], index};
  v << storew{index, rvmtl()[flagHandle]};

  cgCallHelper(v, env, CallSpec::method(&Func::recordCallNoCheck), kVoidDest,
               SyncOptions::None,
               argGroup(env, inst).immPtr(inst->extra<FuncData>()->func));
}

///////////////////////////////////////////////////////////////////////////////

void cgLdTVAux(IRLS& env, const IRInstruction* inst) {
  auto const dst = dstLoc(env, inst, 0).reg();

  auto const tv = srcLoc(env, inst, 0);
  assertx(tv.hasReg(1));
  auto const type = tv.reg(1);

  auto& v = vmain(env);
  v << shrqi{32, type, dst, v.makeReg()};

  if (Cfg::HHIR::GenerateAsserts) {
    auto const extra = inst->extra<LdTVAux>();
    auto const mask = -extra->valid - 1;

    if (mask) {
      auto const sf = v.makeReg();
      v << testqi{mask, dst, sf};
      ifThen(v, CC_NZ, sf, [](Vout& v) {
        v << trap{TRAP_REASON, Fixup::none()};
      });
    }
  }
}

///////////////////////////////////////////////////////////////////////////////

}
