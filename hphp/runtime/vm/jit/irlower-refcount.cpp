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
#include "hphp/runtime/base/countable.h"
#include "hphp/runtime/base/header-kind.h"
#include "hphp/runtime/base/static-string-table.h"
#include "hphp/runtime/base/stats.h"

#include "hphp/runtime/vm/class-meth-data-ref.h"
#include "hphp/runtime/vm/jit/types.h"
#include "hphp/runtime/vm/jit/abi.h"
#include "hphp/runtime/vm/jit/arg-group.h"
#include "hphp/runtime/vm/jit/bc-marker.h"
#include "hphp/runtime/vm/jit/call-spec.h"
#include "hphp/runtime/vm/jit/code-gen-cf.h"
#include "hphp/runtime/vm/jit/code-gen-helpers.h"
#include "hphp/runtime/vm/jit/decref-profile.h"
#include "hphp/runtime/vm/jit/extra-data.h"
#include "hphp/runtime/vm/jit/incref-profile.h"
#include "hphp/runtime/vm/jit/ir-instruction.h"
#include "hphp/runtime/vm/jit/ir-opcode.h"
#include "hphp/runtime/vm/jit/irlower-bespoke.h"
#include "hphp/runtime/vm/jit/ssa-tmp.h"
#include "hphp/runtime/vm/jit/tc.h"
#include "hphp/runtime/vm/jit/target-profile.h"
#include "hphp/runtime/vm/jit/type.h"
#include "hphp/runtime/vm/jit/type-specialization.h"
#include "hphp/runtime/vm/jit/vasm-gen.h"
#include "hphp/runtime/vm/jit/vasm-instr.h"
#include "hphp/runtime/vm/jit/vasm-reg.h"

#include "hphp/runtime/ext/core/ext_core_closure.h"

#include "hphp/util/asm-x64.h"
#include "hphp/util/configs/hhir.h"
#include "hphp/util/configs/jit.h"
#include "hphp/util/trace.h"


namespace HPHP::jit::irlower {

TRACE_SET_MOD(irlower)

///////////////////////////////////////////////////////////////////////////////

namespace {

///////////////////////////////////////////////////////////////////////////////

template<class Then>
void ifNonPersistent(Vout& v, Vout& vtaken, Type ty, Vloc loc, Then then) {
  if (!ty.maybe(TPersistent)) {
    then(v); // non-persistent check below will always succeed
    return;
  }

  auto const sf = emitCmpRefCount(v, 0, loc.reg());
  static_assert(SharedValue < 0 && StaticValue < 0, "");
  unlikelyIfThen(v, vtaken, CC_GE, sf, then);
}

template<class Then>
void ifRefCountedType(Vout& v, Vout& vtaken, Type ty, Vloc loc, Then then) {
  if (!ty.maybe(TCounted)) return;
  if (ty <= TCell && ty.isKnownDataType()) {
    if (isRefcountedType(ty.toDataType())) then(v);
    return;
  }
  auto const sf = v.makeReg();
  assertx(ty <= TCell);
  auto const cond = emitIsTVTypeRefCounted(v, sf, loc.reg(1));
  unlikelyIfThen(v, vtaken, cond, sf, then);
}

template<class Then>
void ifRefCountedNonPersistent(Vout& v, Type ty, Vloc loc, Then then) {
  ifRefCountedType(v, v, ty, loc, [&] (Vout& v) {
    ifNonPersistent(v, v, ty, loc, then);
  });
}

Vreg incrAmount(Vout& v, const TargetProfile<IncRefProfile>& profile) {
  if (!profile.profiling()) return Vreg{};
  auto const sf = v.makeReg();
  auto const offset = profile.handle() + offsetof(IncRefProfile, total);
  v << cmplim{-1, rvmtl()[offset], sf};
  auto const r1 = v.makeReg();
  v << setcc{CC_NE, sf, r1};
  auto const r2 = v.makeReg();
  v << movzbl{r1, r2};
  return r2;
}

void incrementProfile(Vout& v, const TargetProfile<IncRefProfile>& profile,
                      Vreg incr, size_t offset) {
  if (!profile.profiling()) return;
  v << addlm{incr, rvmtl()[profile.handle() + offset], v.makeReg()};
}

///////////////////////////////////////////////////////////////////////////////

}

static const StaticString s_IncRef("IncRefProfile");

///////////////////////////////////////////////////////////////////////////////

void cgIncRef(IRLS& env, const IRInstruction* inst) {
  // This is redundant with a check in ifRefCountedNonPersistent, but we check
  // anyway to avoid emitting profiling code in this case.
  auto const ty = inst->src(0)->type();
  if (!ty.maybe(TCounted)) return;

  auto const loc = srcLoc(env, inst, 0);
  auto& v = vmain(env);

  auto const profile = TargetProfile<IncRefProfile>(env.unit.context(),
                                                    inst->marker(),
                                                    s_IncRef.get());

  auto const incr = incrAmount(v, profile);
  incrementProfile(v, profile, incr, offsetof(IncRefProfile, total));

  bool unlikelyCounted = false;
  bool unlikelyIncrement = false;

  // We profile generic IncRefs to see which ones are unlikely to see
  // refcounted values.
  if (Cfg::HHIR::OutlineGenericIncDecRef && profile.optimizing()) {
    auto const data = profile.data();
    if (data.total > 0) {
      if (data.percent(data.refcounted) <
          Cfg::Jit::PGOUnlikelyIncRefCountedPercent
          && !(ty <= TCell && ty.isKnownDataType())) {
        unlikelyCounted = true;
        FTRACE(3, "irlower-inc-dec: Emitting cold counted check for {}, {}\n",
               data, *inst);
      }
      if (data.percent(data.incremented) <
          Cfg::Jit::PGOUnlikelyIncRefIncrementPercent) {
        unlikelyIncrement = true;
        FTRACE(3, "irlower-inc-dec: Emitting cold IncRef for {}, {}\n",
               data, *inst);
      }
    }
  }

  if (Cfg::Jit::PGOUseAddrCountedCheck &&
      !unlikelyCounted && unlikelyIncrement &&
      ty <= (TCounted | TPersistent) && ty.maybe(TPersistent)) {
    assertx(profile.optimizing());
    // We know the value is a pointer to a HeapObject
    auto const addr = loc.reg();
    // Look at upper bits of the pointer to decide if it is counted.
    auto const sf = v.makeReg();
    v << shrqi{(int)kSharedMaxShift, addr, v.makeReg(), sf};
    unlikelyIfThen(v, vcold(env), CC_NZ, sf,
                   [&] (Vout& v) {
                     emitIncRef(v, loc.reg(), TRAP_REASON);
                   });
    FTRACE(1, "irlower-inc: checking addr for incref {}\n", *inst);
    return;
  }

  auto& vtaken = unlikelyCounted ? vcold(env) : v;
  ifRefCountedType(
    v, vtaken, ty, loc,
    [&] (Vout& v) {
      incrementProfile(v, profile, incr, offsetof(IncRefProfile, refcounted));

      auto& vtaken = unlikelyIncrement ? vcold(env) : v;
      ifNonPersistent(
        v, vtaken, ty, loc,
        [&] (Vout& v) {
          incrementProfile(v, profile, incr,
                           offsetof(IncRefProfile, incremented));
          emitIncRef(v, loc.reg(), TRAP_REASON);
        }
      );
    }
  );
}

namespace {

/*
 * For Optimize translations, this function returns the percentage of the time
 * that the DecRef resulted in destruction during profiling.  For all other
 * kinds of translations, this functions returns 0.0, i.e. we assume that it's
 * unlikely.
 */
float decRefDestroyedPercent(Vout& v, IRLS& /*env*/,
                             const IRInstruction* /*inst*/,
                             const TargetProfile<DecRefProfile>& profile) {
  if (!profile.optimizing()) return 0.0;

  auto const data = profile.data();

  if (data.percent(data.destroyed()) == 0) {
    emitIncStat(v, Stats::TC_DecRef_Profiled_0);
  } else if (data.percent(data.destroyed()) == 100) {
    emitIncStat(v, Stats::TC_DecRef_Profiled_100);
  }

  return data.percent(data.destroyed());
}

CallSpec makeDtorCall(Vout& v, Type ty, Vloc loc, ArgGroup& args) {
  if (ty <= TArrLike) return destructorForArrayLike(ty);

  if (ty <= TObj) {
    if (auto const cls = ty.clsSpec().cls()) {
      if (ty.clsSpec().exact() || (cls->attrs() & AttrNoOverride)) {
        args.immPtr(cls);
        return CallSpec::direct(cls->releaseFunc().get());
      }
      // Call the release function if the base class is a real class and not
      // a builtin, as only builtins can override release method and builtins
      // never subclass non-builtins.
      if (!isInterface(cls) && !cls->isBuiltin()) {
        args.reg(emitLdObjClass(v, loc.reg(0), v.makeReg()));
        return CallSpec::direct(cls->releaseFunc().get());
      }
    }
    auto const cls = emitLdObjClass(v, loc.reg(0), v.makeReg());
    args.reg(cls);
    return CallSpec::objDestruct(cls);
  }

  return ty.isKnownDataType()
    ? CallSpec::direct(destructorForType(ty.toDataType()))
    : CallSpec::destruct(loc.reg(1));
}

namespace {
static void ProfileAndDecRef(DecRefProfile* profile, TypedValue tv) {
  profile->update(tv);
  tvDecRefGen(tv);
}
}

void implDecRefProf(Vout& v, IRLS& env, const IRInstruction* inst,
                    const TargetProfile<DecRefProfile>& profile,
                    bool profileOnly = false) {
  assertx(profile.profiling());
  auto const& type = inst->src(0)->type();
  if (!type.maybe(TCounted)) return;

  auto const args = argGroup(env, inst)
    .addr(rvmtl(), safe_cast<int32_t>(profile.handle()))
    .typedValue(0);
  auto const target = profileOnly
    ? CallSpec::method(&DecRefProfile::update)
    : CallSpec::direct(&ProfileAndDecRef);
  cgCallHelper(v, env, target, kVoidDest, SyncOptions::None, args);
}

/*
 * This function emits a DecRef optimized for the case where the value will be
 * destroyed/released.
 */
template<class Destroy>
void emitDecRefOptDestroy(Vout& v, Vout& vcold, Vreg data,
                          Destroy destroy, bool unlikelyPersist,
                          bool unlikelySurvive) {
  auto const sf = emitCmpRefCount(v, OneReference, data);

  ifThenElse(
    v, vcold, CC_NE, sf,
    [&] (Vout& v) {
      // If it's not static, actually reduce the reference count.  This does
      // another branch using the same status flags from the cmplim above.
      ifThen(v, vcold, CC_NL, sf,
             [&] (Vout& v) { emitDecRef(v, data, TRAP_REASON); },
             unlikelySurvive, tag_from_string("decref-is-static")
      );
    },
    destroy,
    unlikelyPersist && unlikelySurvive,
    tag_from_string("decref-is-one")
  );
}

/*
 * This function emits a DecRef optimized for the case where the value survives
 * the DecRef, i.e. it's count was greater than 1.
 */
template<class Destroy>
void emitDecRefOptSurvive(Vout& v, Vout& vcold, Vreg data,
                          Destroy destroy, bool unlikelyDestroy,
                          bool unlikelyPersist) {
  auto const sf = emitCmpRefCount(v, OneReference, data);

  ifThenElse(
    v, vcold, CC_LE, sf,
    [&] (Vout& v) {
      // If it's not static, call the release method.  This does another branch
      // using the same status flags from the cmplim above.
      ifThen(v, vcold, CC_E, sf, destroy, unlikelyDestroy,
             tag_from_string("decref-is-static"));
    },
    [&] (Vout& v) {
      emitDecRef(v, data, TRAP_REASON);
    },
    unlikelyDestroy && unlikelyPersist,
    tag_from_string("decref-is-one")
  );
}

/*
 * This function emits a DecRef optimized for the case where the value is
 * persistent.
 */
template<class Destroy>
void emitDecRefOptPersist(Vout& v, Vout& vcold, Vreg data,
                          Destroy destroy, bool unlikelyDestroy,
                          bool unlikelySurvive) {
  auto const sf = emitCmpRefCount(v, OneReference, data);

  ifThen(
    v, vcold, CC_GE, sf,
    [&] (Vout& v) {
      // If it's not one, call the release method; otherwise dec-ref the count.
      // This does another branch using the same status flags from the cmplim
      // above.
      ifThenElse(v, vcold, CC_E, sf,
                 destroy,
                 [&] (Vout& v) {
                   emitDecRef(v, data, TRAP_REASON);
                 },
                 unlikelyDestroy,
                 tag_from_string("decref-is-one"));
    },
    unlikelyDestroy && unlikelySurvive,
    tag_from_string("decref-is-static")
  );
}

/*
 * This function uses profile data to emit an optimized DecRef sequence for the
 * most common case (assuming the value is of a ref-counted type), and
 * potentially placing the code to handle the other cases in the cold code area.
 */
template<class Destroy>
void emitDecRefOpt(Vout& v, Vout& vcold, Vreg base,
                   const TargetProfile<DecRefProfile>& profile,
                   Destroy destroy) {
  const auto data = profile.data();
  const auto persistPct = data.percent(data.persistent());
  const auto destroyPct = data.percent(data.destroyed());
  const auto survivePct = data.percent(data.survived());

  const bool persistUnlikely = persistPct <
    Cfg::Jit::PGOUnlikelyDecRefPersistPercent;

  const bool destroyUnlikely = destroyPct <
    Cfg::Jit::PGOUnlikelyDecRefReleasePercent;

  const bool surviveUnlikely = survivePct <
    Cfg::Jit::PGOUnlikelyDecRefSurvivePercent;

  // Case 1: optimize for destruction
  if (destroyPct >= persistPct && destroyPct >= survivePct) {
    emitDecRefOptDestroy(v, vcold, base, destroy, persistUnlikely,
                         surviveUnlikely);
    return;
  }

  // Case 2: optimize for survival
  if (survivePct >= destroyPct && survivePct >= persistPct) {
    emitDecRefOptSurvive(v, vcold, base, destroy, destroyUnlikely,
                         persistUnlikely);
    return;
  }

  // Case 3: optimize for persistency
  emitDecRefOptPersist(v, vcold, base, destroy, destroyUnlikely,
                       surviveUnlikely);
}

void implDecRef(Vout& v, IRLS& env,
                const IRInstruction* inst, Type ty,
                const TargetProfile<DecRefProfile>& profile) {
  auto const base = srcLoc(env, inst, 0).reg(0);

  auto const destroy = [&] (Vout& v) {
    auto args = argGroup(env, inst).reg(base);
    auto const dtor = makeDtorCall(v, ty, srcLoc(env, inst, 0), args);
    cgCallHelper(v, env, dtor, kVoidDest, SyncOptions::None, args);
  };

  if (!ty.maybe(TPersistent)) {
    auto const destroyedPct = decRefDestroyedPercent(v, env, inst, profile);
    auto const unlikelyReleasePct =
      Cfg::Jit::PGOUnlikelyDecRefReleasePercent;
    auto const unlikelyDestroy = destroyedPct < unlikelyReleasePct;

    auto const sf = emitDecRef(v, base, TRAP_REASON);
    ifThen(v, vcold(env), CC_E, sf, destroy, unlikelyDestroy);
    return;
  }

  if (profile.optimizing()) {
    emitDecRefOpt(v, vcold(env), base, profile, destroy);
  } else {
    emitDecRefWork(v, vcold(env), base, destroy, true /* unlikelyDestroy */,
                   TRAP_REASON);
  }
}

void emitDecRefTypeStat(Vout& v, IRLS& env, const IRInstruction* inst) {
  if (!Trace::moduleEnabled(Trace::decreftype)) return;

  auto category = makeStaticString(inst->is(DecRef) ? "DecRef" : "DecRefNZ");
  auto key = makeStaticString(inst->src(0)->type().unspecialize().toString());

  auto const args = argGroup(env, inst)
    .immPtr(category)
    .immPtr(key)
    .imm(1);

  cgCallHelper(v, env, CallSpec::direct(Stats::incStatGrouped),
               kVoidDest, SyncOptions::None, args);
}

}

/*
 * This function emits a DecRef in a variety of different ways, and it leverages
 * profiling information in PGO mode.
 *
 * NO-PGO MODE:
 * ------------
 *
 * Without profiling information (i.e. for Live and Profile translations), we've
 * tried a variety of tweaks to this and found the current state of things to be
 * optimal, at least when measurements of the following factors were made:
 *
 * - whether to load the count into a register
 *
 * - whether to use `if (!--count) release();' if we don't need a static check
 *
 * - whether to skip using the register and just emit --count if we know
 *   its not static, and can't hit zero.
 *
 * The current scheme generates `if (!--count) release();' for types that
 * cannot possibly be static.  For types that might be static, it generates a
 * compare of the m_count field against 1, followed by two conditional branches
 * on the same flags.  We make use of the invariant that count fields are never
 * zero, and use a code sequence that looks like this:
 *
 *    cmpl $1, $FAST_REFCOUNT_OFFSET(%base)
 *    je do_release  // call the destructor, usually in cold code
 *    jl skip_dec    // count < 1 implies it's static
 *    decl $FAST_REFCOUNT_OFFSET(%base)
 *  skip_dec:
 *    // ....
 *
 *
 * PGO MODE:
 * ---------
 *
 * When PGO is enabled, we profile how many times the 4 possibilities for a
 * generic DecRef occur.  The profiling data is collected using the same DecRef
 * code as the no-PGO mode described above, except that a DecRefProfile is used
 * and its various counters are incremented accordingly.  For Optimize
 * translations, this data is used to enhance the no-PGO DecRef sequence in the
 * following possible ways:
 *
 *   1) Deciding to call a generic DecRef stub if the profile type is rarely
 *      ref-counted.
 *
 *   2) Deciding whether to put the code to release the object (count == 1 case)
 *      and/or the code to simply DecRef the count (count > 1 case) in the cold
 *      code area if they're unlikely.
 *
 *   3) Deciding whether to first check for a count of 1 (release), less than
 *      one (persistent), or greater than one (survive).  This allows us to use
 *      a single branch for the most common of these 3 cases, and two branches
 *      for the 2 least common ones.
 *
 */
void cgDecRef(IRLS& env, const IRInstruction *inst) {
  if (noop_decref) return;

  // This is redundant with a check in ifRefCounted, but we check anyway to
  // avoid emitting profiling code in this case.
  auto const ty = inst->src(0)->type();
  if (!ty.maybe(TCounted)) return;

  auto& v = vmain(env);

  emitDecRefTypeStat(v, env, inst);

  const auto profile = decRefProfile(env.unit.context(), inst);
  if (profile.profiling()) {
    implDecRefProf(v, env, inst, profile);
    return;
  }

  if (Trace::moduleEnabled(Trace::irlower, 3) && profile.optimizing()) {
    FTRACE(
        3, "irlower-refcount: Compiling:\nmarker: {}\ninstruction: "
        "{}\ndecRefProfileKey: {}\nDecRefProfile:\n {}\n",
        inst->marker().show(),
        *inst,
        decRefProfileKey(inst)->data(),
        profile.data());
  }

  if (Cfg::HHIR::OutlineGenericIncDecRef &&
      profile.optimizing() && !ty.isKnownDataType()) {
    auto const data = profile.data();
    auto const unlikelyCountedPct =
      Cfg::Jit::PGOUnlikelyDecRefCountedPercent;
    if (data.percent(data.refcounted) < unlikelyCountedPct) {
      // This DecRef rarely saw a refcounted type during profiling, so call the
      // stub in cold, keeping only the type check in main.
      FTRACE(3, "irlower-inc-dec: Emitting partially outlined DecRef "
                "for {}, {}\n", data, *inst);

      auto const data = srcLoc(env, inst, 0).reg(0);
      auto const type = srcLoc(env, inst, 0).reg(1);

      auto const sf = v.makeReg();
      auto const cc = emitIsTVTypeRefCounted(v, sf, type);

      unlikelyIfThen(v, vcold(env), cc, sf, [&] (Vout& v) {
        auto const stub = tc::ustubs().decRefGeneric;
        v << copyargs{
          v.makeTuple({data, type}),
          v.makeTuple({rarg(0), rarg(1)})
        };
        v << callfaststub{stub, arg_regs(2)};
      });
      return;
    }
  }

  auto impl = [&] (Vout& v, Type t) {
    implDecRef(v, env, inst, t, profile);
  };

  if (Cfg::Jit::PGOUseAddrCountedCheck && profile.optimizing() &&
      ty <= (TCounted | TPersistent) && ty.maybe(TPersistent)) {
    // Need to check countedness, and we do it by looking at the pointer.
    auto const data = profile.data();
    auto const unlikelyDecrement = data.total > 0 &&
      data.percent(data.survived() + data.destroyed()) <
      Cfg::Jit::PGOUnlikelyDecRefDecrementPercent;
    // If it's actually counted, we need to touch the cache line anyway.
    if (unlikelyDecrement) {
      auto sf = v.makeReg();
      auto const addr = srcLoc(env, inst, 0).reg();
      v << shrqi{(int)kSharedMaxShift, addr, v.makeReg(), sf};
      unlikelyIfThen(v, vcold(env), CC_NZ, sf,
                     [&] (Vout& v) {
                       impl(v, negativeCheckType(ty, TUncounted));
                     });
      return;
    }
  }

  ifRefCountedType(v, v, ty, srcLoc(env, inst, 0), [&] (Vout& v) {
    impl(v, negativeCheckType(ty, TUncounted));
  });
}


void cgReleaseShallow(IRLS& env, const IRInstruction* inst) {
  auto& v = vmain(env);
  auto const base = srcLoc(env, inst, 0).reg(0);
  auto const ty = inst->src(0)->type();
  auto args = argGroup(env, inst).reg(base);

  if (ty <= TArrLike) {
    auto const dtor = CallSpec::method(&ArrayData::releaseShallow);
    cgCallHelper(v, env, dtor, kVoidDest, SyncOptions::None, args);
  } else {
    assertx(ty.clsSpec().cls()->classof(c_Closure::classof()));
    auto const dtor = CallSpec::direct(&c_Closure::releaseShallow);
    cgCallHelper(v, env, dtor, kVoidDest, SyncOptions::None, args);
  }
}

void cgDecReleaseCheck(IRLS& env, const IRInstruction* inst) {
  auto& v = vmain(env);
  auto const loc = srcLoc(env, inst, 0);
  auto const ty = inst->src(0)->type();
  auto const taken = label(env, inst->taken());
  assertx(ty <= TCell);

  if (ty <= TUncounted) {
    v << jmp{taken};
    return;
  }

  if (!(ty <= (TCounted | TPersistent))) {
    auto const sf = v.makeReg();
    auto const cc = emitIsTVTypeRefCounted(v, sf, loc.reg(1));
    ifThen(v, ccNegate(cc), sf, taken,
           tag_from_string("decreleasecheck-is-dt-refcounted"));
  }

  auto const sf = emitCmpRefCount(v, OneReference, loc.reg(0));
  unlikelyIfThen(
    v, vcold(env), CC_NE, sf,
    [&](Vout& v) {
      if (ty.maybe(TPersistent)) {
        ifThen(v, CC_L, sf, taken,
               tag_from_string("decreleasecheck-is-persistent"));
      }
      emitDecRef(v, loc.reg(0), TRAP_REASON);
      v << jmp{taken};
    },
    tag_from_string("decreleasecheck-is-one")
  );
  v << jmp{label(env, inst->next())};
}

void cgDecRefNZ(IRLS& env, const IRInstruction* inst) {
  auto const ty = inst->src(0)->type();
  auto const loc = srcLoc(env, inst, 0);
  auto& v = vmain(env);

  auto const profile = decRefProfile(env.unit.context(), inst);
  if (profile.profiling()) {
    implDecRefProf(v, env, inst, profile);
    return;
  }

  if (Trace::moduleEnabled(Trace::irlower, 3) && profile.optimizing()) {
    FTRACE(
        3, "irlower-refcount: Compiling:\nmarker: {}\ninstruction: "
        "{}\ndecRefProfileKey: {}\nDecRefProfile:\n {}\n",
      inst->marker().show(),
      *inst,
      decRefProfileKey(inst)->data(),
      profile.data());
  }

  auto unlikelyCounted = false;
  auto unlikelyDecrement = false;
  if (Cfg::HHIR::OutlineGenericIncDecRef && profile.optimizing()) {
    auto const data = profile.data();
    if (data.total > 0) {
      if (data.percent(data.refcounted) <
          Cfg::Jit::PGOUnlikelyDecRefCountedPercent
          && !(ty <= TCell && ty.isKnownDataType())) {
        unlikelyCounted = true;
        FTRACE(3, "irlower-inc-dec: Emitting cold counted check for {}, {}\n",
               data, *inst);
      }
      if (data.percent(data.decremented) <
          Cfg::Jit::PGOUnlikelyDecRefDecrementPercent) {
        unlikelyDecrement = true;
        FTRACE(3, "irlower-inc-dec: Emitting cold DecRef for {}, {}\n",
               data, *inst);
      }
    }
  }

  emitIncStat(v, Stats::TC_DecRef_NZ);
  emitDecRefTypeStat(v, env, inst);

  if (Cfg::Jit::PGOUseAddrCountedCheck &&
      !unlikelyCounted && unlikelyDecrement &&
      ty <= (TCounted | TPersistent) && ty.maybe(TPersistent)) {
    assertx(profile.optimizing());
    auto sf = v.makeReg();
    v << shrqi{(int)kSharedMaxShift, loc.reg(), v.makeReg(), sf};
    unlikelyIfThen(v, vcold(env), CC_NZ, sf,
                   [&] (Vout& v) {
                     emitDecRef(v, loc.reg(), TRAP_REASON);
                   });
    return;
  }

  auto& vtaken = unlikelyCounted ? vcold(env) : v;
  ifRefCountedType(
    v, vtaken, ty, loc,
    [&] (Vout& v) {
      auto& vtaken = unlikelyDecrement ? vcold(env) : v;
      ifNonPersistent(
        v, vtaken, ty, loc,
        [&](Vout& v) {
          emitDecRef(v, loc.reg(), TRAP_REASON);
        }
      );
    }
  );
}

void cgProfileDecRef(IRLS& env, const IRInstruction* inst) {
  auto const profile = decRefProfile(env.unit.context(), inst);
  if (profile.profiling()) implDecRefProf(vmain(env), env, inst, profile, true);
}

///////////////////////////////////////////////////////////////////////////////

void cgDbgAssertRefCount(IRLS& env, const IRInstruction* inst) {
  auto const reason = inst->extra<AssertReason>()->reason;
  auto const src = srcLoc(env, inst, 0);
  auto& v = vmain(env);

  ifRefCountedType(v, v, inst->src(0)->type(), src, [&] (Vout& v) {
    emitAssertRefCount(v, src.reg(), reason);
  });
}

///////////////////////////////////////////////////////////////////////////////

}
