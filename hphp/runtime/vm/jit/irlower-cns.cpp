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
#include "hphp/runtime/base/execution-context.h"
#include "hphp/runtime/base/rds-util.h"
#include "hphp/runtime/base/runtime-error.h"
#include "hphp/runtime/base/static-string-table.h"
#include "hphp/runtime/base/string-data.h"
#include "hphp/runtime/base/tv-mutate.h"
#include "hphp/runtime/base/tv-variant.h"
#include "hphp/runtime/base/type-variant.h"
#include "hphp/runtime/vm/named-entity.h"

#include "hphp/runtime/vm/jit/types.h"
#include "hphp/runtime/vm/jit/abi.h"
#include "hphp/runtime/vm/jit/arg-group.h"
#include "hphp/runtime/vm/jit/call-spec.h"
#include "hphp/runtime/vm/jit/cls-cns-profile.h"
#include "hphp/runtime/vm/jit/code-gen-cf.h"
#include "hphp/runtime/vm/jit/extra-data.h"
#include "hphp/runtime/vm/jit/ir-instruction.h"
#include "hphp/runtime/vm/jit/ir-opcode.h"
#include "hphp/runtime/vm/jit/ssa-tmp.h"
#include "hphp/runtime/vm/jit/type.h"
#include "hphp/runtime/vm/jit/vasm-gen.h"
#include "hphp/runtime/vm/jit/vasm-instr.h"
#include "hphp/runtime/vm/jit/vasm-reg.h"

#include "hphp/util/asm-x64.h"
#include "hphp/util/configs/repo.h"
#include "hphp/util/trace.h"

namespace HPHP::jit::irlower {

TRACE_SET_MOD(irlower)

///////////////////////////////////////////////////////////////////////////////

void cgLdCns(IRLS& env, const IRInstruction* inst) {
  auto const cnsName = inst->src(0)->strVal();
  auto const ch = makeCnsHandle(cnsName);
  assertx(rds::isHandleBound(ch));
  auto const dst = dstLoc(env, inst, 0);
  auto& v = vmain(env);
  assertx(inst->taken());

  auto const checkUninit = [&] {
    auto const sf = v.makeReg();
    emitCmpTVType(v, sf, KindOfUninit, dst.reg(1));
    fwdJcc(v, env, CC_E, sf, inst->taken());
  };

  if (rds::isNormalHandle(ch)) {
    auto const sf = checkRDSHandleInitialized(v, ch);
    fwdJcc(v, env, CC_NE, sf, inst->taken());
    markRDSAccess(v, ch);
    loadTV(v, inst->dst(), dst, rvmtl()[ch]);
    checkUninit();
    return;
  }

  auto const pcns = rds::handleToPtr<TypedValue, rds::Mode::Persistent>(ch);

  if (pcns->m_type == KindOfUninit) {
    markRDSAccess(v, ch);
    loadTV(v, inst->dst(), dst, *v.cns(pcns));
    checkUninit();
  } else {
    // Statically known constant.
    assertx(!dst.isFullSIMD());
    switch (pcns->m_type) {
      case KindOfNull:
        v << copy{v.cns(nullptr), dst.reg(0)};
        break;
      case KindOfBoolean:
        v << copy{v.cns(!!pcns->m_data.num), dst.reg(0)};
        break;
      case KindOfInt64:
      case KindOfPersistentString:
      case KindOfPersistentVec:
      case KindOfPersistentDict:
      case KindOfPersistentKeyset:
      case KindOfString:
      case KindOfVec:
      case KindOfDict:
      case KindOfKeyset:
      case KindOfObject:
      case KindOfResource:
      case KindOfRFunc:
      case KindOfFunc:
      case KindOfClass:
      case KindOfLazyClass:
      case KindOfClsMeth:
      case KindOfRClsMeth:
      case KindOfEnumClassLabel:
        v << copy{v.cns(pcns->m_data.num), dst.reg(0)};
        break;
      case KindOfDouble:
        v << copy{v.cns(pcns->m_data.dbl), dst.reg(0)};
        break;
      case KindOfUninit:
        not_reached();
    }
    v << copy{v.cns(pcns->m_type), dst.reg(1)};
  }
}

///////////////////////////////////////////////////////////////////////////////

namespace {

TypedValue lookupCnsEHelper(StringData* nm) {
  auto const cns = Constant::load(nm);
  if (LIKELY(type(cns) != KindOfUninit)) {
    return cns;
  }
  raise_error("Undefined constant '%s'", nm->data());
}

}

static TypedValue lookupCnsEHelperNormal(rds::Handle tv_handle,
                                         StringData* nm) {
  assertx(rds::isNormalHandle(tv_handle));
  if (UNLIKELY(rds::isHandleInit(tv_handle))) {
    UNUSED auto const tv =
      rds::handleToPtr<TypedValue, rds::Mode::Normal>(tv_handle);
    assertx(type(tv) == KindOfUninit);
    Variant v = Constant::get(nm);
    const TypedValue cns = v.detach();
    assertx(tvIsPlausible(cns));
    assertx(tvAsCVarRef(&cns).isAllowedAsConstantValue() ==
            Variant::AllowedAsConstantValue::Allowed);
    tvIncRefGen(cns);
    rds::handleToRef<TypedValue, rds::Mode::Normal>(tv_handle) = cns;
    return cns;
  }
  return lookupCnsEHelper(nm);
}

static TypedValue lookupCnsEHelperPersistent(rds::Handle tv_handle,
                                             StringData* nm) {
  assertx(rds::isPersistentHandle(tv_handle));

  UNUSED auto const tv =
    rds::handleToPtr<TypedValue, rds::Mode::Persistent>(tv_handle);
  assertx(type(tv) == KindOfUninit);
  Variant v = Constant::get(nm);
  const TypedValue cns = v.detach();
  assertx(tvIsPlausible(cns));
  assertx(tvAsCVarRef(&cns).isAllowedAsConstantValue() ==
          Variant::AllowedAsConstantValue::Allowed);
  return cns;
}

///////////////////////////////////////////////////////////////////////////////

void cgLookupCnsE(IRLS& env, const IRInstruction* inst) {
  auto const cnsName = inst->src(0)->strVal();

  // In RepoAuthoritative mode, irgen emits LookupCnsE for constants whose
  // persistent handle isn't bound yet (see emitCnsE). Don't bind a normal
  // handle for them here; go through Constant::load, which autoloads the
  // defining unit. By the time we get here another thread may have bound the
  // persistent handle with a concrete value, which the persistent helper below
  // doesn't handle, so use the generic helper for that case too.
  if (Cfg::Repo::Authoritative) {
    auto const existing = lookupCnsHandle(cnsName);
    if (!rds::isHandleBound(existing) ||
        (rds::isPersistentHandle(existing) &&
         rds::handleToPtr<TypedValue, rds::Mode::Persistent>(existing)
           ->m_type != KindOfUninit)) {
      cgCallHelper(
        vmain(env), env,
        CallSpec::direct(lookupCnsEHelper),
        callDestTV(env, inst),
        SyncOptions::Sync,
        argGroup(env, inst).immPtr(cnsName)
      );
      return;
    }
  }

  auto const ch = makeCnsHandle(cnsName);
  assertx(rds::isHandleBound(ch));

  auto const args = argGroup(env, inst)
    .imm(ch)
    .immPtr(cnsName);

  cgCallHelper(
    vmain(env), env,
    rds::isNormalHandle(ch)
      ? CallSpec::direct(lookupCnsEHelperNormal)
      : CallSpec::direct(lookupCnsEHelperPersistent),
    callDestTV(env, inst),
    SyncOptions::Sync,
    args
  );
}

///////////////////////////////////////////////////////////////////////////////

void cgLdClsCns(IRLS& env, const IRInstruction* inst) {
  auto const extra = inst->extra<LdClsCns>();
  auto const dst = dstLoc(env, inst, 0);
  auto& v = vmain(env);

  auto const link = rds::bindClassConstant(extra->clsName, extra->cnsName);
  assertx(link.isNormal());

  auto const sf = checkRDSHandleInitialized(v, link.handle());
  fwdJcc(v, env, CC_NE, sf, inst->taken());
  markRDSAccess(v, link.handle());
  loadTV(v, inst->dst(), dst, rvmtl()[link.handle()]);
}

void cgLdSubClsCns(IRLS& env, const IRInstruction* inst) {
  auto const extra = inst->extra<LdSubClsCns>();
  auto const dst = dstLoc(env, inst, 0);
  auto& v = vmain(env);

  auto const slot = extra->slot;
  auto const tmp = v.makeReg();
  v << load{srcLoc(env, inst, 0).reg()[Class::constantsVecOff()], tmp};
  loadTV(
    v,
    inst->dst(),
    dst,
    tmp[slot * sizeof(Class::Const) + offsetof(Class::Const, val)]
  );
}

void cgCheckSubClsCns(IRLS& env, const IRInstruction* inst) {
  auto const extra = inst->extra<CheckSubClsCns>();
  auto& v = vmain(env);

  auto const slot = extra->slot;
  auto const tmp = v.makeReg();
  auto const sf = v.makeReg();
  v << load{srcLoc(env, inst, 0).reg()[Class::constantsVecOff()], tmp};

  auto const constOffset = slot * sizeof(Class::Const);

  emitCmpPackedPtr(
    v, sf, extra->cnsName,
    tmp[constOffset + offsetof(Class::Const, name)]
  );
  fwdJcc(v, env, CC_NE, sf, inst->taken());

  auto const sf2 = v.makeReg();
  auto const kindOffset =
    constOffset +
    offsetof(Class::Const, val) +
    offsetof(TypedValueAux, m_auxFlags) +
    offsetof(AuxFlagsUnion, u_constModifierFlags) +
    offsetof(ConstModifierFlags, kind);

  static_assert((int)ConstModifierFlags::Kind::Value == 0);

  v << testbim{
    safe_cast<int8_t>(-1),
    tmp[kindOffset],
    sf2
  };
  fwdJcc(v, env, CC_NZ, sf2, inst->taken());
}

void cgLdClsCnsVecLen(IRLS& env, const IRInstruction* inst) {
  auto const dst = dstLoc(env, inst, 0).reg();
  auto const cls = srcLoc(env, inst, 0).reg();
  auto& v = vmain(env);

  auto const off = Class::constantsVecLenOff();

  static_assert(
    Class::constantsVecLenSize() == 4,
    "Class::constantsVecLenSize() must be 4 bytes "
    "(if you changed it, fix the following code)");

  v << loadzlq{cls[off], dst};
}

void cgProfileSubClsCns(IRLS& env, const IRInstruction* inst) {
  auto const extra = inst->extra<ProfileSubClsCns>();

  auto const args = argGroup(env, inst)
    .addr(rvmtl(), safe_cast<int32_t>(extra->handle))
    .ssa(0)
    .immPtr(extra->cnsName);

  cgCallHelper(
    vmain(env),
    env,
    CallSpec::method(&ClsCnsProfile::reportClsCns),
    callDestTV(env, inst),
    SyncOptions::Sync,
    args
  );
}

static TypedValue initClsCnsHelper(TypedValue* cache,
                                   const NamedType* ne,
                                   const StringData* cls,
                                   const StringData* cns) {
  // We need anchor here since lookupClsCns might raise warning or throw an
  // exception
  VMRegAnchor _;
  auto const clsCns = g_context->lookupClsCns(ne, cls, cns);
  tvDup(clsCns, *cache);
  return clsCns;
}

void cgInitClsCns(IRLS& env, const IRInstruction* inst) {
  auto const extra = inst->extra<InitClsCns>();
  auto const link = rds::bindClassConstant(extra->clsName, extra->cnsName);
  assertx(link.isNormal());
  auto& v = vmain(env);

  markRDSAccess(v, link.handle());
  auto const args = argGroup(env, inst)
    .addr(rvmtl(), safe_cast<int32_t>(link.handle()))
    .immPtr(NamedType::getOrCreate(extra->clsName))
    .immPtr(extra->clsName)
    .immPtr(extra->cnsName);

  cgCallHelper(v, env, CallSpec::direct(initClsCnsHelper),
               callDestTV(env, inst), SyncOptions::Sync, args);

  markRDSHandleInitialized(v, link.handle());
}

static TypedValue initSubClsCnsHelper(const Class* cls,
                                      const StringData* cnsName) {
  auto const cns = cls->clsCnsGet(cnsName);
  if (UNLIKELY(cns.m_type == KindOfUninit)) {
    raise_error("Couldn't find constant %s::%s",
                cls->name()->data(), cnsName->data());
  }
  return cns;
}

void cgInitSubClsCns(IRLS& env, const IRInstruction* inst) {
  auto const extra = inst->extra<InitSubClsCns>();
  auto const args = argGroup(env, inst)
    .ssa(0)
    .immPtr(extra->cnsName);
  cgCallHelper(vmain(env), env, CallSpec::direct(initSubClsCnsHelper),
               callDestTV(env, inst), SyncOptions::Sync, args);
}

///////////////////////////////////////////////////////////////////////////////

namespace {

void ldResolvedTypeHelper(Vout& v, Slot slot, Vreg cls, Vreg cns) {
  auto const cnsVec = v.makeReg();
  v << load{cls[Class::constantsVecOff()], cnsVec};
  v << load{
    cnsVec[slot * sizeof(Class::Const) +
           offsetof(Class::Const, val) +
           TVOFF(m_data)],
    cns
  };
}

} // namespace

void cgLdResolvedTypeCns(IRLS& env, const IRInstruction* inst) {
  auto const cls = srcLoc(env, inst, 0).reg();
  auto const cns = dstLoc(env, inst, 0).reg();
  auto const slot = inst->extra<LdResolvedTypeCns>()->slot;
  auto& v = vmain(env);

  auto const masked = v.makeReg();
  ldResolvedTypeHelper(v, slot, cls, masked);

  auto const sf = v.makeReg();
  v << btrq{0, masked, cns, sf};
  fwdJcc(v, env, CC_AE, sf, inst->taken());
}

void cgLdResolvedTypeCnsNoCheck(IRLS& env, const IRInstruction* inst) {
  auto const cls = srcLoc(env, inst, 0).reg();
  auto const cns = dstLoc(env, inst, 0).reg();
  auto const slot = inst->extra<LdResolvedTypeCnsNoCheck>()->slot;
  auto& v = vmain(env);

  auto const masked = v.makeReg();
  ldResolvedTypeHelper(v, slot, cls, masked);

  if (debug) {
    // If we've asserted it's always pre-resolved, it had better be!
    auto const sf = v.makeReg();
    v << testqi{0x1, masked, sf};
    unlikelyIfThen(
      v, vcold(env), CC_Z, sf,
      [&] (Vout& v) { v << trap{TRAP_REASON, Fixup::none()}; }
    );
  }

  v << xorqi{0x1, masked, cns, v.makeReg()};
}

void cgLdTypeCns(IRLS& env, const IRInstruction* inst) {
  auto const args = argGroup(env, inst).ssa(0).ssa(1).imm(false);
  cgCallHelper(vmain(env), env, CallSpec::direct(loadClsTypeCnsHelper),
               callDest(env, inst), SyncOptions::Sync, args);
}
void cgLdTypeCnsNoThrow(IRLS& env, const IRInstruction* inst) {
  auto const args = argGroup(env, inst).ssa(0).ssa(1).imm(true);
  cgCallHelper(vmain(env), env, CallSpec::direct(loadClsTypeCnsHelper),
               callDest(env, inst), SyncOptions::Sync, args);
}

void cgLdResolvedTypeCnsClsName(IRLS& env, const IRInstruction* inst) {
  auto const extra = inst->extra<LdResolvedTypeCnsClsName>();
  auto const cls = srcLoc(env, inst, 0).reg();
  auto const dst = dstLoc(env, inst, 0).reg();
  auto& v = vmain(env);

  auto const slot = extra->slot;
  auto const cnsVec = v.makeReg();
  v << load{cls[Class::constantsVecOff()], cnsVec};
  auto const rawAddr = cnsVec[slot * sizeof(Class::Const) +
                              offsetof(Class::Const, val) +
                              offsetof(TypedValue, m_aux) +
                              offsetof(AuxUnion, u_constModifiers) +
                              offsetof(ConstModifiers, u_clsName)];
  emitLdPackedPtr(v, rawAddr, dst);
}

void cgLdTypeCnsClsName(IRLS& env, const IRInstruction* inst) {
  auto const args = argGroup(env, inst).ssa(0).ssa(1);
  cgCallHelper(vmain(env), env, CallSpec::direct(loadClsTypeCnsClsNameHelper),
               callDest(env, inst), SyncOptions::Sync, args);
}

void cgLdClsCtxCns(IRLS& env, const IRInstruction* inst) {
  auto const cls = srcLoc(env, inst, 0).reg();
  auto const dst = dstLoc(env, inst, 0).reg();
  auto const slot = inst->extra<LdClsCtxCns>()->slot;
  auto& v = vmain(env);

  auto const cnsVec = v.makeReg();
  v << load{cls[Class::constantsVecOff()], cnsVec};
  auto const rawAddr = cnsVec[slot * sizeof(Class::Const) +
                              offsetof(Class::Const, val) +
                              offsetof(TypedValue, m_aux) +
                              offsetof(AuxUnion, u_constModifiers) +
                              offsetof(ConstModifiers, u_coeffectsData)];
  v << loadzlq{rawAddr, dst};

  if (debug) {
    // Lets assert that the slot we are loading is context constant
    auto const sf = v.makeReg();
    auto const kindAddr = cnsVec[slot * sizeof(Class::Const) +
                                 offsetof(Class::Const, val) +
                                 offsetof(TypedValue, m_auxFlags) +
                                 offsetof(AuxFlagsUnion, u_constModifierFlags) +
                                 offsetof(ConstModifierFlags, kind)];
    v << cmpbim{static_cast<int8_t>(ConstModifierFlags::Kind::Context), kindAddr, sf};
    unlikelyIfThen(
      v, vcold(env), CC_NZ, sf,
      [&] (Vout& v) { v << trap{TRAP_REASON, Fixup::none()}; }
    );
    // Lets assert that the upper bits are zero
    auto const shifted = v.makeReg();
    auto const sf2 = v.makeReg();
    // 16 bits for coeffects
    v << shrqi{16, dst, shifted, v.makeReg()};
    v << testq{shifted, shifted, sf2};
    unlikelyIfThen(
      v, vcold(env), CC_NZ, sf2,
      [&] (Vout& v) { v << trap{TRAP_REASON, Fixup::none()}; }
    );
  }
}

///////////////////////////////////////////////////////////////////////////////

IMPL_OPCODE_CALL(LookupClsCns)
IMPL_OPCODE_CALL(LookupClsCtxCns)

///////////////////////////////////////////////////////////////////////////////

}
