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

void cgCheckType(IRLS& env, const IRInstruction* inst) {
  // Note: If you add new supported type checks, you should update
  // negativeCheckType() to indicate whether it is precise or not.
  auto const src = inst->src(0);
  auto const dst = inst->dst();
  auto const srcData = srcLoc(env, inst, 0).reg(0);
  auto const srcType = srcLoc(env, inst, 0).reg(1);
  auto known = src->type();

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

  if (known <= typeParam) {
    // src is the target type or better. Just define our dst.
    doMov();
    return;
  } else if (!known.maybe(typeParam)) {
    // src is definitely not the target type. Always jump.
    v << jmp{label(env, inst->taken())};
    return;
  }

  always_assert(typeParam <= TCls || !typeParam.hasConstVal());
  if (Cfg::Jit::ProfileGuardTypes) {
    emitProfileGuardType(v, typeParam);
  }

  // TUncounted + TUncountedInit are special, handle them separately.
  if (typeParam == TUncounted || typeParam == TUncountedInit) {
    if (known.maybe(TCounted)) {
      if (srcType == InvalidReg) {
        // Known DataType, but the outcome was not statically determined,
        // must be a countable. Do the exact persistence check.
        assertx(known <= (TCounted | TPersistent));
        assertx(known.maybe(TCounted));
        assertx(known.maybe(TPersistent));
        auto const sf = emitCmpRefCount(v, 0, srcData);
        doJcc(CC_L, sf);
      } else {
        // A persistent tag proves countness. A counted tag can still hold a
        // persistent value, so this check may have false negatives.
        auto const sf = v.makeReg();
        emitTestTVType(v, sf, kRefCountedBit, srcType);
        doJcc(CC_Z, sf);
      }
      known -= TCounted;
    }
    if (typeParam == TUncountedInit && known.maybe(TUninit)) {
      assertx(!(known <= TUninit));
      assertx(srcType != InvalidReg);
      auto const sf = v.makeReg();
      emitCmpTVType(v, sf, KindOfUninit, srcType);
      doJcc(CC_NE, sf);
      known -= TUninit;
    }
    doMov();
    return;
  }

  // Check DataType if needed. Accept both persistence flavors.
  auto const base = typeParam.unspecialize();
  auto const tagType = [&] {
    auto t = base;
    for (auto const dt : {TStr, TVec, TDict, TKeyset}) {
      if (t.maybe(dt)) t |= dt;
    }
    return t;
  }();
  if (!(known <= tagType)) {
    assertx(srcType != InvalidReg);
    auto const sf = v.makeReg();
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
      if (tagType.isKnownDataType()) return test(tagType.toDataType());
      if (tagType == TArrLike)       return cmp(KindOfKeyset, CC_BE);
      if (tagType == (TVec|TDict))   return cmp(KindOfVec, CC_BE);
      if (tagType == TNull)          return cmp(KindOfUninit, CC_AE);
      if (tagType == TNonNull)       return cmp(KindOfUninit, CC_B);
      if (tagType == TInitCell)      return cmp(KindOfUninit, CC_NE);
      always_assert_flog(false, "Uncheckable type: {}", typeParam);
    }();
    doJcc(cc, sf);
    known &= tagType;
  }

  // Check countness if needed.
  if (!(known <= base)) {
    assertx(known <= (TCounted | TPersistent));
    if (base <= TStatic) {
      auto const sf = emitCmpRefCount(v, StaticValue, srcData);
      doJcc(CC_E, sf);
      known &= TStatic;
    } else if (base <= TPersistent) {
      auto const sf = emitCmpRefCount(v, 0, srcData);
      doJcc(CC_L, sf);
      known &= TPersistent;
    } else if (base <= TCounted) {
      auto const sf = emitCmpRefCount(v, 0, srcData);
      doJcc(CC_GE, sf);
      known &= TCounted;
    }

    // Triggers if none of the conditions above hit, or we tried to check for
    // something strange and unsupported, like:
    //   CheckType<SharedVec>
    //   CheckType<StaticVec | PersistentDict>
    always_assert_flog(known <= base, "Uncheckable type: {}", typeParam);
  }

  // Check specialization if needed.
  if (!(known <= typeParam)) {
    assertx(typeParam.isSpecialized());
    if (typeParam < TObj || typeParam < TCls) {
      auto const spec = typeParam.clsSpec();
      assertx(spec);
      assertx(spec.exact() || spec.cls()->attrs() & AttrNoOverride);

      auto const sf = v.makeReg();
      if (typeParam < TObj) {
        emitCmpPackedPtr(v, sf, spec.cls(),
                         srcData[ObjectData::getVMClassOffset()]);
      } else {
        v << cmpq{v.cns(spec.cls()), srcData, sf};
      }
      doJcc(CC_E, sf);
    } else {
      auto const spec = typeParam.arrSpec();
      assertx(typeParam <= TArrLike);
      assertx(allowBespokeArrayLikes());
      assertx(!spec.type());

      auto const layout = spec.layout();
      if (layout == ArrayLayout::Vanilla() ||
          layout == ArrayLayout::Bespoke()) {
        auto const index = srcData[HeaderKindOffset];
        auto const sf = v.makeReg();
        v << testbim{ArrayData::kBespokeKindMask, index, sf};
        doJcc(layout == ArrayLayout::Vanilla() ? CC_Z : CC_NZ, sf);
      } else {
        auto const test = layout.bespokeLayoutTest();
        auto const imm = static_cast<int16_t>(test.imm);
        auto const index = srcData[ArrayData::offsetOfBespokeIndex()];
        auto const sf = v.makeReg();
        switch (test.mode) {
          case LayoutTest::And1Byte: v << testbim{imm, index + 1, sf}; break;
          case LayoutTest::And2Byte: v << testwim{imm, index, sf}; break;
          case LayoutTest::Cmp1Byte: v << cmpbim{imm, index + 1, sf}; break;
          case LayoutTest::Cmp2Byte: v << cmpwim{imm, index, sf}; break;
        }
        doJcc(CC_Z, sf);
      }
    }
  }

  doMov();
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
