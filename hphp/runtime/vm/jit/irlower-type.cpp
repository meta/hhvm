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

#include "hphp/runtime/vm/jit/abi.h"
#include "hphp/runtime/vm/jit/code-gen-cf.h"
#include "hphp/runtime/vm/jit/extra-data.h"
#include "hphp/runtime/vm/jit/ir-instruction.h"
#include "hphp/runtime/vm/jit/ssa-tmp.h"
#include "hphp/runtime/vm/jit/translator-inline.h"
#include "hphp/runtime/vm/jit/type-profile.h"
#include "hphp/runtime/vm/jit/type.h"
#include "hphp/runtime/vm/jit/types.h"
#include "hphp/runtime/vm/jit/vasm-gen.h"
#include "hphp/runtime/vm/jit/vasm-instr.h"
#include "hphp/runtime/vm/jit/vasm-reg.h"

#include "hphp/util/asm-x64.h"
#include "hphp/util/trace.h"

namespace HPHP::jit::irlower {

TRACE_SET_MOD(irlower)

///////////////////////////////////////////////////////////////////////////////

void cgCheckType(IRLS& env, const IRInstruction* inst) {
  // Note: If you add new supported type checks, you should update
  // negativeCheckType() to indicate whether it is precise or not.
  auto const src = inst->src(0);
  auto const dst = inst->dst();
  auto const srcData = srcLoc(env, inst, 0).reg(0);
  auto const srcType = srcLoc(env, inst, 0).reg(1);

  auto& v = vmain(env);

  auto const doJcc = [&] (ConditionCode cc, Vreg sf) {
    fwdJcc(v, env, ccNegate(cc), sf, inst->taken());
  };

  auto const doMov = [&] {
    auto const dstData = dstLoc(env, inst, 0).reg(0);
    auto const dstType = dstLoc(env, inst, 0).reg(1);

    if (dst->isA(TBool) && !src->isA(TBool)) {
      v << movtqb{srcData, dstData};
    } else {
      v << copy{srcData, dstData};
    }
    if (dstType == InvalidReg) return;
    if (srcType != InvalidReg) {
      v << copy{srcType, dstType};
    } else {
      v << ldimmb{static_cast<data_type_t>(src->type().toDataType()), dstType};
    }
  };

  auto const typeParam = inst->typeParam();

  if (src->isA(typeParam)) {
    // src is the target type or better. Just define our dst.
    doMov();
    return;
  } else if (!src->type().maybe(typeParam)) {
    // src is definitely not the target type. Always jump.
    v << jmp{label(env, inst->taken())};
    return;
  }

  /*
   * See if we're just checking the array specialization or the object class
   * of a value with a mostly-known type.
   *
   * NOTE: we must support cases like CheckType<ArrLike=Vanilla> t1:Vec in
   * this check, which is why we check here that src->type() matches the
   * unspecialized type before skipping the DataType component of the test.
   */
  if (typeParam.isSpecialized() &&
      typeParam.unspecialize() >= src->type()) {
    detail::emitSpecializedTypeTest(v, env, typeParam, srcData, doJcc);
    doMov();
    return;
  }

  if (srcType != InvalidReg) {
    // CheckType<UncountedInit> requires two tests: Uncounted, and not Uninit.
    // If we already know the value is an InitCell, skip the second.
    auto const relax = typeParam == TUncountedInit && src->isA(TInitCell);
    auto const typeToCheck = relax ? TUncounted : typeParam;
    emitTypeTest(v, env, typeToCheck, srcType, srcData, v.makeReg(), doJcc);
    doMov();
    return;
  }

  // We know that it is a string and we are checking for static str
  if (!typeParam.isSpecialized() && typeParam <= TStaticStr) {
    if (!src->isA(TStr)) {
      // This could be a false negative but that's okay based on CheckType's
      // contract
      v << jmp{label(env, inst->taken())};
      return;
    }
    detail::emitSpecializedTypeTest(v, env, typeParam, srcData, doJcc);
    doMov();
    return;
  }

  /*
   * Since not all of our unions carry a type register, there are some
   * situations with strings and arrays that are neither constantly-foldable
   * nor in the emitTypeTest() code path.
   *
   * We currently actually check their persistent bit here, which will let
   * both static and uncounted strings through.  Also note that
   * CheckType<Uncounted> t1:{Null|Str} doesn't get this treatment currently---
   * the emitTypeTest() path above will only check the type register.
   */
  if (!typeParam.isSpecialized() &&
      typeParam <= TUncounted &&
      src->type().subtypeOfAny(TStr, TArrLike) &&
      src->type().maybe(typeParam)) {
    assertx(src->type().maybe(TPersistent));

    auto const sf = emitCmpRefCount(v, 0, srcData);
    doJcc(CC_L, sf);
    doMov();
    return;
  }

  always_assert_flog(
    false,
    "Bad src: {} and dst: {} types in '{}'",
    src->type(), typeParam, *inst
  );
}

///////////////////////////////////////////////////////////////////////////////

void cgAssertType(IRLS& env, const IRInstruction* inst) {
  auto& v = vmain(env);
  auto const& dtype = inst->dst()->type();
  if (dtype == TBottom) {
    v << trap{TRAP_REASON, Fixup::none()};
    v = v.makeBlock();
    return;
  }

  auto const dst = dstLoc(env, inst, 0);
  auto const src = srcLoc(env, inst, 0);
  copyTV(v, src, dst, dtype);
}

void cgAssertLoc(IRLS&, const IRInstruction*) {}
void cgAssertStk(IRLS&, const IRInstruction*) {}
void cgAssertMBase(IRLS&, const IRInstruction*) {}

void cgProfileType(IRLS& env, const IRInstruction* inst) {
  auto const extra = inst->extra<RDSHandleData>();

  auto const args = argGroup(env, inst)
    .addr(rvmtl(), safe_cast<int32_t>(extra->handle))
    .typedValue(0);

  cgCallHelper(vmain(env), env, CallSpec::method(&TypeProfile::report),
               kVoidDest, SyncOptions::None, args);
}

///////////////////////////////////////////////////////////////////////////////

IMPL_OPCODE_CALL(RecordReifiedGenericsAndGetTSList)

void cgResolveTypeStruct(IRLS& env, const IRInstruction* inst) {
  auto const sp = srcLoc(env, inst, 0).reg();
  auto const extra = inst->extra<ResolveTypeStructData>();
  auto& v = vmain(env);

  auto const args = argGroup(env, inst)
    .imm(extra->size)      // num
    .addr(sp, cellsToBytes(extra->offset.offset)) // values
    .imm(extra->cls)       // declaring cls
    .ssa(1)                // called cls
    .imm(extra->suppress)  // suppress
    .imm(extra->isOrAsOp); // isOrAsOp

  cgCallHelper(v, env, CallSpec::direct(resolveTypeStructHelper),
               callDest(env, inst), SyncOptions::Sync, args);
}

///////////////////////////////////////////////////////////////////////////////

}
