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

#include "hphp/runtime/base/array-data.h"
#include "hphp/runtime/base/attr.h"
#include "hphp/runtime/base/bespoke-array.h"
#include "hphp/runtime/base/datatype.h"
#include "hphp/runtime/base/header-kind.h"
#include "hphp/runtime/base/object-data.h"

#include "hphp/runtime/vm/jit/abi.h"
#include "hphp/runtime/vm/jit/code-gen-cf.h"
#include "hphp/runtime/vm/jit/extra-data.h"
#include "hphp/runtime/vm/jit/guard-type-profile.h"
#include "hphp/runtime/vm/jit/ir-instruction.h"
#include "hphp/runtime/vm/jit/ssa-tmp.h"
#include "hphp/runtime/vm/jit/translator-inline.h"
#include "hphp/runtime/vm/jit/type-profile.h"
#include "hphp/runtime/vm/jit/type-specialization.h"
#include "hphp/runtime/vm/jit/type.h"
#include "hphp/runtime/vm/jit/types.h"
#include "hphp/runtime/vm/jit/vasm-gen.h"
#include "hphp/runtime/vm/jit/vasm-instr.h"
#include "hphp/runtime/vm/jit/vasm-reg.h"

#include "hphp/util/asm-x64.h"
#include "hphp/util/configs/jit.h"
#include "hphp/util/trace.h"

namespace HPHP::jit::irlower {

TRACE_SET_MOD(irlower)

///////////////////////////////////////////////////////////////////////////////

namespace {

template <class JmpFn>
void emitBespokeLayoutTest(Vout& v, ArrayLayout layout, Vreg r, JmpFn doJcc) {
  auto const test = layout.bespokeLayoutTest();
  auto const imm = static_cast<int16_t>(test.imm);
  auto const sf = v.makeReg();

  switch (test.mode) {
    case LayoutTest::And1Byte:
      v << testbim{imm, r[ArrayData::offsetOfBespokeIndex() + 1], sf};
      break;
    case LayoutTest::And2Byte:
      v << testwim{imm, r[ArrayData::offsetOfBespokeIndex()], sf};
      break;
    case LayoutTest::Cmp1Byte:
      v << cmpbim{imm, r[ArrayData::offsetOfBespokeIndex() + 1], sf};
      break;
    case LayoutTest::Cmp2Byte:
      v << cmpwim{imm, r[ArrayData::offsetOfBespokeIndex()], sf};
      break;
  }

  doJcc(CC_Z, sf);
  auto const doneBlock = v.makeBlock();
  v << jmp{doneBlock};
  v = doneBlock;
}

/*
 * Test whether the value given by `dataSrc' has the same type specialization
 * as `type' does.
 *
 * Assumes that the DataType corresponding to `dataSrc' already matches `type'.
 */
template <class JmpFn>
void emitSpecializedTypeTest(Vout& v, Type type, Vreg dataSrc, JmpFn doJcc) {
  if (type < TRes) {
    // No cls field in Resource.
    always_assert(false && "unexpected guard on specialized Resource");
  }

  if (type == TStaticStr) {
    auto const sf = emitCmpRefCount(v, StaticValue, dataSrc);
    doJcc(CC_E, sf);
    return;
  }

  if (type < TObj || type < TCls) {
    // Emit the specific class test.
    assertx(type.clsSpec());
    assertx(type.clsSpec().exact() ||
            type.clsSpec().cls()->attrs() & AttrNoOverride);

    auto const sf = v.makeReg();
    if (type < TObj) {
      emitCmpPackedPtr(v, sf, type.clsSpec().cls(),
                       dataSrc[ObjectData::getVMClassOffset()]);
    } else {
      v << cmpq{v.cns(type.clsSpec().cls()), dataSrc, sf};
    }
    doJcc(CC_E, sf);
    return;
  }

  auto const spec = type.arrSpec();
  assertx(allowBespokeArrayLikes());
  assertx(!spec.type());

  auto const layout = spec.layout();
  if (layout == ArrayLayout::Vanilla() || layout == ArrayLayout::Bespoke()) {
    auto const sf = v.makeReg();
    auto const cc = layout == ArrayLayout::Vanilla() ? CC_Z : CC_NZ;
    v << testbim{ArrayData::kBespokeKindMask, dataSrc[HeaderKindOffset], sf};
    doJcc(cc, sf);
  } else {
    emitBespokeLayoutTest(v, layout, dataSrc, doJcc);
  }
}

}

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
    emitSpecializedTypeTest(v, typeParam, srcData, doJcc);
    doMov();
    return;
  }

  if (srcType != InvalidReg) {
    // CheckType<UncountedInit> requires two tests: Uncounted, and not Uninit.
    // If we already know the value is an InitCell, skip the second.
    auto const relax = typeParam == TUncountedInit && src->isA(TInitCell);
    auto const type = relax ? TUncounted : typeParam;
    auto const sf = v.makeReg();
    always_assert(type <= TCls || !type.hasConstVal());
    always_assert_flog(
      !type.subtypeOfAny(TCountedStr, TPersistentArrLike),
      "Unsupported type in cgCheckType(): {}", type
    );

    // Profile the type being guarded. TUncounted is handled by
    // emitIsTVTypeRefCounted below.
    if (Cfg::Jit::ProfileGuardTypes && type != TUncounted) {
      emitProfileGuardType(v, type);
    }

    auto const cc = [&] {
      auto const test = [&] (DataType kind) {
        assertx(kind == dt_modulo_persistence(kind));
        auto const mask = ~static_cast<data_type_t>(kind);
        emitTestTVType(v, sf, mask, srcType);
        return CC_E;
      };

      auto const cmp = [&] (DataType kind, ConditionCode cc) {
        emitCmpTVType(v, sf, kind, srcType);
        return cc;
      };

      auto const base = type.unspecialize();
      if (type == TStaticStr)     return test(KindOfString);
      if (type.isKnownDataType()) return test(type.toDataType());
      if (base == TArrLike)       return cmp(KindOfKeyset, CC_BE);
      if (type == (TVec|TDict))   return cmp(KindOfVec, CC_BE);
      if (type == TNull)          return cmp(KindOfUninit, CC_AE);
      if (type == TNonNull)       return cmp(KindOfUninit, CC_B);

      if (type == TUncountedInit) {
        auto const sf2 = v.makeReg();
        emitTestTVType(v, sf2, kRefCountedBit, srcType);
        doJcc(CC_Z, sf2);
        return cmp(KindOfUninit, CC_NZ);
      }

      if (type == TUncounted) {
        return ccNegate(emitIsTVTypeRefCounted(v, sf, srcType));
      }

      if (type == TInitCell) return cmp(KindOfUninit, CC_NZ);

      // All other valid types must not be unions.
      always_assert_flog(false, "Uncheckable type: {}", type);
    }();

    doJcc(cc, sf);

    if (type.isSpecialized() || type == TStaticStr) {
      emitSpecializedTypeTest(v, type, srcData, doJcc);
    }
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
    emitSpecializedTypeTest(v, typeParam, srcData, doJcc);
    doMov();
    return;
  }

  /*
   * Since not all of our unions carry a type register, there are some
   * situations with strings and arrays that are neither constantly-foldable
   * nor in the type-register code path above.
   *
   * We currently actually check their persistent bit here, which will let
   * both static and uncounted strings through.  Also note that
   * CheckType<Uncounted> t1:{Null|Str} doesn't get this treatment currently---
   * the path above will only check the type register.
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
