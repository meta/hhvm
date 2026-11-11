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

#include "hphp/runtime/vm/jit/normalized-instruction.h"

#include "hphp/runtime/vm/class-meth-data-ref.h"
#include "hphp/runtime/vm/jit/cls-cns-profile.h"
#include "hphp/runtime/vm/jit/irgen-call.h"
#include "hphp/runtime/vm/jit/irgen-exit.h"
#include "hphp/runtime/vm/jit/irgen-internal.h"
#include "hphp/runtime/vm/jit/irgen-interpone.h"
#include "hphp/runtime/vm/jit/target-profile.h"

#include "hphp/util/configs/eval.h"
#include "hphp/util/configs/repo.h"
#include "hphp/util/configs/sandbox.h"

namespace HPHP::jit::irgen {

///////////////////////////////////////////////////////////////////////////////

void emitCnsE(IRGS& env, const StringData* name) {
  if (auto const tv = Constant::lookupPersistent(name)) {
    if (tv->m_type == KindOfUninit) {
      // KindOfUninit is a dynamic system constant. always a slow
      // lookup.
      push(env, gen(env, LookupCnsE, cns(env, name)));
      return;
    }

    if (auto const val = Type::tryCns(*tv)) {
      push(env, cns(env, *val));
      return;
    }
  }

  // In RepoAuthoritative mode every constant is persistent, but its handle is
  // only bound once the defining unit is merged. LdCns would bind a normal
  // handle for a constant whose unit hasn't been loaded yet, making the later
  // persistent bind in Constant::def fail and leaving the constant undefined,
  // so emit a handle-less lookup instead.
  if (Cfg::Repo::Authoritative &&
      !rds::isHandleBound(lookupCnsHandle(name))) {
    push(env, gen(env, LookupCnsE, cns(env, name)));
    return;
  }

  auto const v = cond(
    env,
    [&] (Block* taken) {
      return gen(env, LdCns, taken, cns(env, name));
    },
    [&] (SSATmp* v) {
      gen(env, IncRef, v);
      return v;
    },
    [&] {
      hint(env, Block::Hint::Unlikely);
      return gen(env, LookupCnsE, cns(env, name));
    }
  );
  push(env, v);
}

namespace {

//////////////////////////////////////////////////////////////////////

void exactClsCns(IRGS& env,
                const Class* cls,
                const StringData* cnsNameStr,
                const StringData* clsNameStr) {
  auto const clsCnsName = ClsCnsName { clsNameStr, cnsNameStr };
  // If we passed in a Class*, this means we have better information
  // about what the Class* must be since this information is local and
  // comes from inspecting the top of the stack to see if we have an
  // exact type.
  auto const lookup = [&]() {
    if (cls != nullptr) {
      return Class::ClassLookup { Class::ClassLookupResult::Exact, cls };
    } else {
      return lookupKnownMaybe(env, clsNameStr);
    }
  }();

  if (lookup.cls &&
      will_symbol_raise_module_boundary_violation(lookup.cls, curFunc(env))) {
    auto const cns = gen(env, InitClsCns, TInitCell, clsCnsName);
    pushIncRef(env, cns);
    return;
  }

  // If we can't prove we know the Class* at jit-time, we need to
  // load the constant out of out RDS.
  auto const getCnsWithType = [&](jit::Type ty) {
    auto const cns = cond(
      env,
      [&] (Block* taken) {
        return gen(env, LdClsCns, ty, clsCnsName, taken);
      },
      [&] (SSATmp* cns) { return cns; },
      [&] {
        hint(env, Block::Hint::Unlikely);
        return gen(env, InitClsCns, ty, clsCnsName);
      }
    );
    pushIncRef(env, cns);
  };

  auto const data = [&](uint32_t id, bool success) {
    return LoggingSpeculateData {
      clsNameStr,
      curClass(env) ? curClass(env)->name() : nullptr,
      cnsNameStr,
      Op::ClsCnsD,
      id,
      success,
    };
  };

  switch (lookup.tag) {
    case Class::ClassLookupResult::Exact: {
      Slot ignore;
      auto cnsType = TInitCell;
      if (auto const tv = lookup.cls->cnsNameToTV(cnsNameStr, ignore)) {
        if (type(tv) != KindOfUninit) {
          // If the class is known and this constant is a scalar constant,
          // we can just compile it to a literal.
          if (auto const val = Type::tryCns(*tv)) {
            push(env, cns(env, *val));
            return;
          }
          cnsType = typeFromTV(tv, curClass(env));
        }
      }
      getCnsWithType(TInitCell);
      return;
    }
    case Class::ClassLookupResult::None: {
      if (Cfg::Sandbox::Speculate) {
        if (Cfg::Eval::LogClsSpeculation) {
          gen(env, LogClsSpeculation, data(ClassId::Invalid, false));
        }
        gen(env, LdClsCached, LdClsFallbackData::Fatal(), cns(env, clsNameStr));
        gen(env, Jmp, makeExit(env));
        return;
      } else {
        getCnsWithType(TInitCell);
        return;
      }
    }
    case Class::ClassLookupResult::Maybe: {
      // TODO: Remove after fixing modules
      // I believe the above case where we push a literal is broken, in
      // sandbox mode since when we push a constant, we just trust
      // that the class doesn't violate the module boundary.
      // This was probably fine so far, since in sandbox mode, we usually
      // don't know the exact Class*, especially one that is in a different
      // file/module. Therefore, in practice we probably always do a InitClsCns
      // before accessing a constant. That is about to change as sandbox
      // mode becomes more performant, and we emit cheap runtime-checks
      // and if our runtime Class* matches the jit-time Class*, we may
      // be able to push the literal directly, without doing the boundary
      // check in InitClsCns.
      if (Cfg::Eval::EnforceModules) return getCnsWithType(TInitCell);

      // If we have a Class* at jit-time, but cannot trust it, we can emit
      // a check to confirm the runtime Class* matches and then use the
      // constant we burned in at jit-time.
      Slot ignore;
      if (auto const tv = lookup.cls->cnsNameToTV(cnsNameStr, ignore)) {
        if (type(tv) != KindOfUninit) {
          if (auto const val = Type::tryCns(*tv)) {
            gen(env, LdClsCached, LdClsFallbackData::Fatal(), cns(env, clsNameStr));
            auto const isEqual = gen(env, EqClassId, ClassIdData(lookup.cls));
            ifThenElse(
              env,
              [&] (Block* taken) {
                gen(env, JmpZero, taken, isEqual);
              },
              [&] {
                if (Cfg::Eval::LogClsSpeculation) {
                  gen(env, LogClsSpeculation, data(lookup.cls->classId().id(), true));
                }
                push(env, cns(env, *val));
              },
              [&] {
                hint(env, Block::Hint::Unlikely);
                if (Cfg::Eval::LogClsSpeculation) {
                  gen(env, LogClsSpeculation, data(lookup.cls->classId().id(), false));
                }
                getCnsWithType(TInitCell);
                gen(env, Jmp, makeExit(env, nextSrcKey(env)));
              }
            );
            return;
          }
        }
      }
      getCnsWithType(TInitCell);
      break;
    }
    not_reached();
  };

}

StaticString clsCnsProfileKey { "ClsCnsProfile" };

//////////////////////////////////////////////////////////////////////

} // namespace

void emitClsCnsD(IRGS& env,
                 const StringData* cnsNameStr,
                 const StringData* clsNameStr) {
  exactClsCns(env, nullptr, cnsNameStr, clsNameStr);
}

void emitClsCns(IRGS& env, const StringData* cnsNameStr) {
  auto const clsTmp = topC(env);
  auto const clsTy = clsTmp->type();

  if (!(clsTy <= TCls)) PUNT(ClsCns-NotClass);

  auto const loadSlot = [&] (Slot slot) {
    auto const data = ClsCnsSlotData { cnsNameStr, slot };
    auto const cns = cond(
      env,
      [&] (Block* taken) {
        auto const slotVal = gen(env, LdSubClsCns, data, clsTmp);
        return gen(env, CheckType, TUncountedInit, taken, slotVal);
      },
      [&] (SSATmp* val) { return val; },
      [&] {
        hint(env, Block::Hint::Unlikely);
        auto const cns = gen(env, InitSubClsCns, data, clsTmp);
        // The value may be ref-counted if we take the slow path
        gen(env, IncRef, cns);
        return cns;
      }
    );
    discard(env);
    push(env, cns);
  };

  if (!clsTy.clsSpec() || !isNormalClass(clsTy.clsSpec().cls())) {
    TargetProfile<ClsCnsProfile> profile(env.context,
                                         env.irb->curMarker(),
                                         clsCnsProfileKey.get());
    if (profile.profiling()) {
      auto const data = ProfileSubClsCnsData { cnsNameStr, profile.handle() };
      auto const cns = gen(
        env,
        CheckType,
        TInitCell,
        makeExitSlow(env),
        gen(env, ProfileSubClsCns, data, clsTmp)
      );
      discard(env);
      pushIncRef(env, cns);
      return;
    }

    if (profile.optimizing()) {
      auto const slot = profile.data().getSlot();
      if (slot != kInvalidSlot) {
        auto const exit = makeExitSlow(env);
        auto const len = gen(env, LdClsCnsVecLen, clsTmp);
        auto const cmp = gen(env, LteInt, len, cns(env, slot));
        gen(env, JmpNZero, exit, cmp);
        auto const data = ClsCnsSlotData { cnsNameStr, slot };
        gen(env, CheckSubClsCns, data, exit, clsTmp);
        return loadSlot(slot);
      }
    }

    return interpOne(env);
  }

  auto const cls = clsTy.clsSpec().cls();
  if (clsTy.clsSpec().exact()) {
    discard(env);
    exactClsCns(env, cls, cnsNameStr, cls->name());
  } else {
    auto const slot =
      cls->clsCnsSlot(cnsNameStr, ConstModifierFlags::Kind::Value, true);
    if (slot == kInvalidSlot) return interpOne(env);
    loadSlot(slot);
  }
}

void emitClsCnsL(IRGS& env, int32_t id) {
  auto const cls = topC(env);
  if (!cls->isA(TCls)) PUNT(ClsCns-NotClass);
  auto const cnsName = ldLoc(env, id, DataTypeSpecific);
  if (!cnsName->isA(TStr)) PUNT(ClsCns-NotStr);
  if (cnsName->hasConstVal(TStr)) {
    emitClsCns(env, cnsName->strVal());
  } else {
    auto const cns = gen(env, LookupClsCns, cls, cnsName);
    popDecRef(env);
    pushIncRef(env, cns);
  }
}

//////////////////////////////////////////////////////////////////////

}
