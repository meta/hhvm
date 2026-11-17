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

#include "hphp/runtime/base/datatype.h"
#include "hphp/runtime/base/string-data.h"
#include "hphp/runtime/base/typed-value.h"
#include "hphp/runtime/vm/act-rec.h"
#include "hphp/runtime/vm/bytecode.h"
#include "hphp/runtime/vm/class.h"
#include "hphp/runtime/vm/func.h"
#include "hphp/runtime/vm/minstr-state.h"
#include "hphp/runtime/vm/prologue-flags.h"
#include "hphp/runtime/vm/srckey.h"
#include "hphp/runtime/vm/unit.h"

#include "hphp/runtime/vm/jit/types.h"
#include "hphp/runtime/vm/jit/abi.h"
#include "hphp/runtime/vm/jit/arg-group.h"
#include "hphp/runtime/vm/jit/bc-marker.h"
#include "hphp/runtime/vm/jit/call-spec.h"
#include "hphp/runtime/vm/jit/call-target-profile.h"
#include "hphp/runtime/vm/jit/code-gen-cf.h"
#include "hphp/runtime/vm/jit/code-gen-helpers.h"
#include "hphp/runtime/vm/jit/extra-data.h"
#include "hphp/runtime/vm/jit/fixup.h"
#include "hphp/runtime/vm/jit/ir-instruction.h"
#include "hphp/runtime/vm/jit/ir-opcode.h"
#include "hphp/runtime/vm/jit/mcgen.h"
#include "hphp/runtime/vm/jit/ssa-tmp.h"
#include "hphp/runtime/vm/jit/target-profile.h"
#include "hphp/runtime/vm/jit/tc.h"
#include "hphp/runtime/vm/jit/translator-inline.h"
#include "hphp/runtime/vm/jit/type.h"
#include "hphp/runtime/vm/jit/unique-stubs.h"
#include "hphp/runtime/vm/jit/vasm-gen.h"
#include "hphp/runtime/vm/jit/vasm-instr.h"
#include "hphp/runtime/vm/jit/vasm-reg.h"

#include "hphp/util/configs/hhir.h"
#include "hphp/util/trace.h"

namespace HPHP::jit::irlower {

TRACE_SET_MOD(irlower)

///////////////////////////////////////////////////////////////////////////////

namespace {

bool setCtxReg(Vout& v, const Func* callee, SSATmp* ctx, Vreg src, Vreg dst) {
  if (ctx->isA(TObj) || ctx->isA(TCls)) {
    assertx(!callee || callee->isClosureBody() || callee->cls());
    v << copy{src, dst};
    return true;
  }

  assertx(ctx->isA(TNullptr));
  assertx(!callee || (!callee->isClosureBody() && !callee->cls()));
  if (Cfg::HHIR::GenerateAsserts) {
    v << copy{v.cns(ActRec::kTrashedThisSlot), dst};
    return true;
  }

  return false;
}

}

void cgCall(IRLS& env, const IRInstruction* inst) {
  auto const sp = srcLoc(env, inst, 0).reg();
  auto const callee = srcLoc(env, inst, 2).reg();
  auto const ctx = srcLoc(env, inst, 3).reg();
  auto const coeffects = srcLoc(env, inst, 4).reg();
  auto const extra = inst->extra<Call>();
  auto const numPositionalsInclUnpack =
    extra->numPosArgs + (extra->hasUnpack ? 1 : 0);
  auto const func = inst->src(2)->hasConstVal(TFunc)
    ? inst->src(2)->funcVal() : nullptr;
  // Upgrade skipRepack if HHIR opts inferred the callee. We can't do this for
  // unpack, as it is not guaranteed to be a varray.
  // We can also special-case calls with 0 args as never needing a repack since there's
  // no unpack argument and param counts are non-negative.
  auto const skipRepack = extra->skipRepack || (
    func && !extra->hasUnpack && numPositionalsInclUnpack <= func->numPositionalParams()
  ) || numPositionalsInclUnpack == 0;
  auto const coeffectsVal = inst->src(4)->hasConstVal(TInt)
    ? RuntimeCoeffects::fromValue(inst->src(4)->intVal())
    : RuntimeCoeffects::none();

  auto& v = vmain(env);

  auto const prologueFlags = PrologueFlags(
    extra->hasGenerics,
    extra->dynamicCall,
    extra->namedArgNames != nullptr,
    extra->asyncEagerReturn,
    extra->callOffset,
    extra->genericsBitmap,
    coeffectsVal
  );

  if (!inst->src(4)->hasConstVal(TInt)) {
    auto const coeffectsShifted = v.makeReg();
    v << shlqi{
      PrologueFlags::CoeffectsStart,
      coeffects,
      coeffectsShifted,
      v.makeReg()
    };
    v << orq{
      coeffectsShifted,
      v.cns(prologueFlags.value()),
      r_func_prologue_flags(),
      v.makeReg()
    };
  } else {
    v << copy{v.cns(prologueFlags.value()), r_func_prologue_flags()};
  }

  v << copy{callee, r_func_prologue_callee()};
  v << copy{v.cns(numPositionalsInclUnpack), r_func_prologue_num_args()};
  auto const withCtx =
    setCtxReg(v, func, inst->src(3), ctx, r_func_prologue_ctx());

  // Make vmsp() point to the future vmfp().
  auto const ssp = v.makeReg();
  v << lea{sp[cellsToBytes(extra->spOffset.offset + extra->numInputs())], ssp};
  v << syncvmsp{ssp};

  auto const done = v.makeBlock();
  auto withNamedArgNames = extra->namedArgNames != nullptr;
  if (withNamedArgNames) {
    v << copy{v.cns(extra->namedArgNames), r_func_prologue_named_args()};
  }
  if (skipRepack && func) {
    // Emit a smashable call that initially calls a recyclable service request
    // stub.  The stub and the eventual targets take rvmfp() as an argument,
    // pointing to the callee ActRec.
    assertx(
      (!extra->hasUnpack && extra->numPosArgs <= func->numPositionalParams()) ||
      (extra->hasUnpack && extra->numPosArgs == func->numPositionalParams()));
    v << callphps{tc::ustubs().immutableBindCallStub,
                  func_prologue_regs(withCtx, withNamedArgNames),
                  func, numPositionalsInclUnpack};
  } else if (skipRepack) {
    // If we've statically determined the provided number of arguments
    // doesn't exceed what the target expects, we can skip the stub
    // and call the prologue directly.
    auto const pTabOff = safe_cast<int32_t>(Func::prologueTableOff());
    auto const ptrSize = safe_cast<int32_t>(sizeof(LowTCA));
    auto const dest = v.makeReg();
    v << loadzlq{r_func_prologue_callee()[numPositionalsInclUnpack * ptrSize + pTabOff], dest};
    v << callphpr{dest, func_prologue_regs(withCtx, withNamedArgNames)};
  } else {
    // It was not statically determined that the arguments are passed in a way
    // the callee expects. Use the redispatch stub to repack them as needed and
    // transfer control to the appropriate prologue. This can happen due to:
    // - the callee not being statically known
    // - the callee inferred later in HHIR opts, but arguments mispacked
    // - unpack used in a different position than callee's variadic param
    auto const stub = !extra->hasUnpack
      ? (extra->namedArgNames == nullptr
         ? tc::ustubs().funcPrologueRedispatch
         : tc::ustubs().funcPrologueRedispatchNamedArgs)
      : tc::ustubs().funcPrologueRedispatchUnpack;
    v << callphp{stub, func_prologue_regs(withCtx, withNamedArgNames)};
  }

  // The prologue is responsible for unwinding all inputs. We could have
  // optimized away Uninit stores for ActRec and inouts, so skip them as well.
  auto const marker = inst->marker();
  auto const fixupBcOff = marker.fixupBcOff();
  auto const fixupSpOff = marker.fixupBcSPOff()
    - extra->numInputs() - kNumActRecCells - extra->numOut;
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

void cgCallFuncEntry(IRLS& env, const IRInstruction* inst) {
  auto const calleeFP = srcLoc(env, inst, 0).reg();
  auto const callee = srcLoc(env, inst, 2).reg();
  auto const ctx = srcLoc(env, inst, 3).reg();
  auto const extra = inst->extra<CallFuncEntry>();
  auto const calleePrototype = extra->calleePrototype;
  auto const func = inst->src(2)->hasConstVal(TFunc)
    ? inst->src(2)->funcVal() : nullptr;

  auto& v = vmain(env);

  // Initialize func entry registers. `callee_id` is initialized later
  // as it may be copied over from `callee` for virtual func call entries,
  // and doing so after loading the func entry improves register allocator
  // behavior.
  v << copy{v.cns(extra->arFlags), r_func_entry_ar_flags()};
  auto const withCtx =
    setCtxReg(v, calleePrototype, inst->src(3), ctx, r_func_entry_ctx());

  // Make vmsp() point to the future vmfp().
  v << syncvmsp{calleeFP};

  auto const done = v.makeBlock();
  auto const numPosArgs = std::min(extra->numInitPositionals,
                                   calleePrototype->numPositionalParams());
  if (func != nullptr) {
    v << copy{v.cns(func->getFuncId().toInt()), r_func_entry_callee_id()};
    // When we statically know the callee, emit a smashable call that initially
    // calls a recyclable service request stub. The stub and the eventual targets
    // take rvmfp() as an argument, pointing to the callee ActRec.
    auto entry = SrcKey {
      func, numPosArgs, extra->addedUninitNamedParams, SrcKey::FuncEntryTag {}
    };
    v << callphpfe{entry, func_entry_regs(withCtx)};
  } else {
    // We're doing a virtual dispatch to the call func entry. Read the `m_funcEntry`
    // field from the function and call it directly. The field will be initialized to
    // a stub that assumes `nonNonVariadicParams` are passed in and translates the func entry
    // with the appropriate SrcKey.
    assertx(numPosArgs == calleePrototype->numPositionalParams());
    assertx(!extra->addedUninitNamedParams);
    // Load the FuncEntry address dynamically from the function.
    auto dest = v.makeReg();
    auto const funcEntryOff = safe_cast<int32_t>(Func::funcEntryOff());
    v << loadzlq{callee[funcEntryOff], dest};
    emitEncodePackedPtr(v, callee, r_func_entry_callee_id());
    v << callphpr{dest, func_entry_regs(withCtx)};
  }

  // The callee is responsible for unwinding the whole frame, which includes
  // all inputs, ActRec and empty space reserved for inouts.
  auto const marker = inst->marker();
  auto const fixupBcOff = marker.fixupBcOff();
  auto const fixupSpOff = marker.fixupBcSPOff() - calleePrototype->numInOutParams();
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

void cgCallBuiltin(IRLS& env, const IRInstruction* inst) {
  auto const extra = inst->extra<CallBuiltin>();
  auto const callee = extra->callee;
  auto const returnByValue = callee->isReturnByValue();

  auto& v = vmain(env);

  // We don't write to the true dst registers until the very end of the
  // instruction sequence, in case we have to perform option-dependent fixups.
  auto const dstData = dstLoc(env, inst, 0).reg(0);
  auto const dstType = dstLoc(env, inst, 0).reg(1);
  auto const tmpData = v.makeReg();
  auto const tmpType = v.makeReg();

  auto returnType = inst->dst()->type();

  if (returnType.maybe(TNull) && !(returnType <= TNull)) {
    if ((returnType - TNull).isSingularReferenceType()) {
      returnType -= TNull;
    }
  }

  int returnOffset = rds::kVmMInstrStateOff +
                     offsetof(MInstrState, tvBuiltinReturn);
  auto args = argGroup(env, inst);

  if (!returnByValue) {
    if (!returnType.isSimpleType()) {
      if (returnType.isSingularReferenceType()) {
        returnOffset += TVOFF(m_data);
      }
      // Pass the address of tvBuiltinReturn to the native function as the
      // location where it can construct the return Array, String, Object, or
      // Variant.
      args.indRet(rvmtl(), returnOffset);
    }
  }

  // The srcs past the first two (sp and fp) are the arguments to the callee.
  auto srcNum = uint32_t{2};

  // Add the this_ or self_ argument for HNI builtins.
  if (callee->isMethod()) {
    if (callee->isStatic()) {
      args.ssa(srcNum);
      ++srcNum;
    } else {
      // Note that we don't support objects with vtables here (if they may need
      // a $this pointer adjustment).  This should be filtered out during irgen
      // or before.
      args.ssa(srcNum);
      ++srcNum;
    }
  }

  // Add the positional arguments.
  for (uint32_t i = 0; i < callee->numParams(); ++i, ++srcNum) {
    auto const& pi = callee->params()[i];

    switch (pi.builtinAbi) {
      case Func::ParamInfo::BuiltinAbi::Value:
        assertx(inst->src(srcNum)->isA(TCell));
        args.ssa(srcNum);
        break;
      case Func::ParamInfo::BuiltinAbi::FPValue:
        assertx(inst->src(srcNum)->isA(TCell));
        args.fp(srcNum);
        break;
      case Func::ParamInfo::BuiltinAbi::TypedValue:
        assertx(inst->src(srcNum)->isA(TCell));
        args.typedValue(srcNum);
        break;
      case Func::ParamInfo::BuiltinAbi::ValueByRef:
      case Func::ParamInfo::BuiltinAbi::TypedValueByRef:
      case Func::ParamInfo::BuiltinAbi::InOutByRef:
        static_assert(TVOFF(m_data) == 0, "");
        assertx(inst->src(srcNum)->isA(TPtr));
        args.ssa(srcNum);
        break;
    }
  }

  auto dest = [&] () -> CallDest {
    if (!returnType.isSimpleType()) {
      if (!returnByValue) return kVoidDest; // indirect return
      return returnType.isSingularReferenceType()
        ? callDest(dstData) // String, Array, or Object
        : callDest(dstData, dstType); // Variant
    }
    return callDest(env, inst);
  }();
  if (dest.reg0.isValid()) dest.reg0 = tmpData;
  if (dest.reg1.isValid()) dest.reg1 = tmpType;

  // Call epilogue: handle builtin return types and inlining accounting.
  auto const end = [&] (Vout& v) {
    v << copy{tmpData, dstData};
    if (dstType.isValid()) v << copy{tmpType, dstType};
  };

  cgCallHelper(v, env, CallSpec::direct(callee->nativeFuncPtr(), nullptr),
               dest, SyncOptions::Sync, args);

  // For primitive return types (int, bool, double) and returnByValue, the
  // return value is already in dstData/dstType.
  if (returnType.isSimpleType() || returnByValue) return end(v);

  // For return by reference (String, Object, Array, Variant), the builtin
  // writes the return value into MInstrState::tvBuiltinReturn, from where it
  // has to be tested and copied.

  if (returnType.isSingularReferenceType()) {
    // The return type is String, Array, or Object; fold nullptr to KindOfNull.

    v << load{rvmtl()[returnOffset], tmpData};

    if (dstType.isValid()) {
      auto const sf = v.makeReg();
      auto const rtype = v.cns(returnType.toDataType());
      auto const nulltype = v.cns(KindOfNull);
      v << testq{tmpData, tmpData, sf};
      v << cmovb{CC_Z, sf, rtype, nulltype, tmpType};
    }
    return end(v);
  }

  if (returnType <= TCell) {
    // The return type is Variant; fold KindOfUninit to KindOfNull.

    v << load{rvmtl()[returnOffset + TVOFF(m_data)], tmpData};

    if (dstType.isValid()) {
      v << loadb{rvmtl()[returnOffset + TVOFF(m_type)], tmpType};
      if (debug) {
        auto const sf = v.makeReg();
        v << cmpbi{static_cast<data_type_t>(KindOfUninit), tmpType, sf};
        ifThen(
          v, CC_E, sf,
          [&] (Vout& v) { v << trap{TRAP_REASON, Fixup::none()}; }
        );
      }
    }
    return end(v);
  }

  not_reached();
}

void cgNativeImpl(IRLS& env, const IRInstruction* inst) {
  auto fp = srcLoc(env, inst, 0).reg();
  auto& v = vmain(env);

  auto const func = inst->marker().func();

  v << vinvoke{
    CallSpec::direct(func->arFuncPtr(), nullptr),
    v.makeVcallArgs({{fp}}),
    v.makeTuple({}),
    {label(env, inst->next()),
     label(env, inst->taken())},
    makeFixup(inst->marker(), SyncOptions::Sync)
  };
}

static void traceCallback(ActRec* fp, TypedValue* sp, SrcKey::AtomicInt skInt) {
  auto const sk = SrcKey::fromAtomicInt(skInt);
  if (Trace::moduleEnabled(Trace::hhirTracelets)) {
    FTRACE(0, "{} {} {} {}\n",
           showShort(sk), fp, sp,
           __builtin_return_address(0));
  }
  // Func entries do not have all locals set up.
  checkFrame(fp, sp, !sk.anyFuncEntry() /* fullCheck */);
}

void cgDbgTraceCall(IRLS& env, const IRInstruction* inst) {
  auto const spOff = inst->extra<DbgTraceCall>()->offset;

  auto const args = argGroup(env, inst)
    .ssa(0)
    .addr(srcLoc(env, inst, 1).reg(), cellsToBytes(spOff.offset))
    .imm(inst->marker().sk().toAtomicInt());

  cgCallHelper(vmain(env), env, CallSpec::direct(traceCallback),
               callDest(env, inst), SyncOptions::None, args);
}

///////////////////////////////////////////////////////////////////////////////

void cgProfileCall(IRLS& env, const IRInstruction* inst) {
  auto const extra = inst->extra<ProfileCallTargetData>();

  auto const args = argGroup(env, inst)
    .addr(rvmtl(), safe_cast<int32_t>(extra->handle))
    .ssa(0);

  cgCallHelper(vmain(env), env, CallSpec::method(&CallTargetProfile::report),
               kVoidDest, SyncOptions::None, args);
}

///////////////////////////////////////////////////////////////////////////////

void cgEnterPrologue(IRLS& env, const IRInstruction*) {
  vmain(env) << stublogue{false};
  vmain(env) << recordbasenativesp{};
}

void cgExitPrologue(IRLS& env, const IRInstruction*) {
  vmain(env) << unrecordbasenativesp{};
  vmain(env) << unstublogue{};
}

void cgEnterTranslation(IRLS& env, const IRInstruction*) {
  vmain(env) << recordbasenativesp{};
}

///////////////////////////////////////////////////////////////////////////////

IMPL_OPCODE_CALL(NewRFunc)

void cgFuncHasReifiedGenerics(IRLS& env, const IRInstruction* inst) {
  auto const dst = dstLoc(env, inst, 0).reg();
  auto const func = srcLoc(env, inst, 0).reg();

  auto& v = vmain(env);
  auto const shared = v.makeReg();
  auto const sf = v.makeReg();

  v << load{func[Func::sharedOff()], shared};
  v << testlim{(int32_t)Func::reifiedGenericsMask(),
               shared[Func::sharedAllFlags()], sf};

  v << setcc{CC_NZ, sf, dst};
}

void cgLdFuncFromRFunc(IRLS& env, const IRInstruction* inst) {
  auto const rfuncRef = srcLoc(env, inst, 0).reg();
  auto const dst = dstLoc(env, inst, 0).reg();
  auto& v = vmain(env);
  v << load{rfuncRef[RFuncData::funcOffset()], dst};
}

void cgLdGenericsFromRFunc(IRLS& env, const IRInstruction* inst) {
  auto const rfuncRef = srcLoc(env, inst, 0).reg();
  auto const dst = dstLoc(env, inst, 0).reg();
  auto& v = vmain(env);
  v << load{rfuncRef[RFuncData::genericsOffset()], dst};
}

///////////////////////////////////////////////////////////////////////////////

IMPL_OPCODE_CALL(NewRClsMeth)

namespace {

void ldFromRClsMethCommon(IRLS& env, const IRInstruction* inst, ptrdiff_t offset) {
  auto const rclsMethRef = srcLoc(env, inst, 0).reg();
  auto const dst = dstLoc(env, inst, 0).reg();
  auto& v = vmain(env);
  v << load{rclsMethRef[offset], dst};
}

}// namespace

void cgLdClsFromRClsMeth(IRLS& env, const IRInstruction* inst) {
  ldFromRClsMethCommon(env, inst, RClsMethData::clsOffset());
}

void cgLdFuncFromRClsMeth(IRLS& env, const IRInstruction* inst) {
  ldFromRClsMethCommon(env, inst, RClsMethData::funcOffset());
}

void cgLdGenericsFromRClsMeth(IRLS& env, const IRInstruction* inst) {
  ldFromRClsMethCommon(env, inst, RClsMethData::genericsOffset());
}

///////////////////////////////////////////////////////////////////////////////

namespace {
  Vreg checkModulesEquality(Vout& v, IRLS& env, const IRInstruction* inst) {
    auto const sf = v.makeReg();
    auto const callerModuleName = inst->extra<FuncData>()->func->moduleName();

    if (inst->src(0)->isA(TFunc)) {
      auto const func = srcLoc(env, inst, 0).reg();

      if (Cfg::Repo::Authoritative) {
        auto const calleeModuleName = v.makeReg();
        auto const shared = v.makeReg();
        auto const sfExtra = v.makeReg();
        v << load{func[Func::sharedOff()], shared};
        v << testlim{(int32_t)Func::hasExtendedSharedDataMask(),
                     shared[Func::sharedAllFlags()], sfExtra};
        cond(v, CC_Z, sfExtra, calleeModuleName,
          [&] (Vout& v) {
            auto const ret = v.makeReg();
            auto const unit = v.makeReg();
            v << load{func[Func::unitOff()], unit};
            emitLdPackedPtr(v, unit[Unit::moduleNameOff()], ret);
            return ret;
          },
          [&] (Vout& v) {
            auto const ret = v.makeReg();
            emitLdPackedPtr(v, shared[Func::extendedSharedOriginalModuleName()], ret);
            return ret;
          });
        emitCmpPackedPtr(v, sf, callerModuleName, calleeModuleName);
      } else {
        auto const unit = v.makeReg();
        v << load{func[Func::unitOff()], unit};
        emitCmpPackedPtr(v, sf, callerModuleName, unit[Unit::moduleNameOff()]);
      };
    } else {
      assertx(inst->src(0)->isA(TCls));
      auto const unit = v.makeReg();
      auto const cls = srcLoc(env, inst, 0).reg();
      auto const preclass = v.makeReg();
      v << load{cls[Class::preClassOff()], preclass};
      v << load{preclass[PreClass::unitOffset()], unit};
      emitCmpPackedPtr(v, sf, callerModuleName, unit[Unit::moduleNameOff()]);
    }

    return sf;
  }
} // namespace

void cgCallViolatesModuleBoundary(IRLS& env, const IRInstruction* inst) {
  auto const dst = dstLoc(env, inst, 0).reg();
  auto& v = vmain(env);
  auto const sf = checkModulesEquality(v, env, inst);
  v << setcc{CC_NZ, sf, dst};
}

}
