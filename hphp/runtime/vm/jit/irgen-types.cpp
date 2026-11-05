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
#include "hphp/runtime/vm/jit/irgen-types.h"

#include "hphp/runtime/base/array-iterator.h"
#include "hphp/runtime/base/type-structure.h"
#include "hphp/runtime/base/type-structure-helpers.h"
#include "hphp/runtime/base/type-structure-helpers-defs.h"

#include "hphp/runtime/vm/hhbc-shared.h"

#include "hphp/runtime/vm/jit/is-type-struct-profile.h"
#include "hphp/runtime/vm/jit/guard-constraint.h"
#include "hphp/runtime/vm/jit/target-profile.h"
#include "hphp/runtime/vm/jit/decref-profile.h"
#include "hphp/runtime/vm/jit/type.h"

#include "hphp/runtime/vm/jit/ir-opcode.h"
#include "hphp/runtime/vm/jit/irgen-exit.h"
#include "hphp/runtime/vm/jit/irgen-interpone.h"
#include "hphp/runtime/vm/jit/irgen-builtin.h"

#include "hphp/runtime/vm/jit/irgen-internal.h"

#include "hphp/util/configs/eval.h"
#include "hphp/util/text-util.h"

#include <fmt/format.h>

namespace HPHP::jit::irgen {

namespace {

//////////////////////////////////////////////////////////////////////

const StaticString
  s_StringishObject("StringishObject"),
  s_Awaitable("HH\\Awaitable"),
  s_CLASS_TO_CLASSNAME(Strings::CLASS_TO_CLASSNAME),
  s_CLASS_TO_CLASSNAME_INHERITED(Strings::CLASS_TO_CLASSNAME_INHERITED),
  s_STRING_TO_CLASS(Strings::STRING_TO_CLASS),
  s_STRING_TO_CLASS_INHERITED(Strings::STRING_TO_CLASS_INHERITED);

//////////////////////////////////////////////////////////////////////

/*
 * Returns a Bool value indicating if src (which must be <= TObj) is an
 * instance of the class given in className, or nullptr if we don't have an
 * efficient translation of the required check. checkCls must be the TCls for
 * className (but it doesn't have to be constant).
 */
SSATmp* implInstanceCheck(IRGS& env, SSATmp* src, const StringData* className,
                          SSATmp* checkCls) {
  assertx(src->isA(TObj));
  if (s_Awaitable.get()->tsame(className)) {
    return gen(env, IsWaitHandle, src);
  }
  if (s_StringishObject.get()->tsame(className)) {
    return gen(env, HasToString, src);
  }

  auto knownCls = checkCls->hasConstVal(TCls) ? checkCls->clsVal() : nullptr;
  assertx(IMPLIES(knownCls, lookupKnown(env, knownCls)));
  assertx(IMPLIES(knownCls, knownCls->name()->tsame(className)));

  auto const srcType = src->type();

  /*
   * If the value is a specialized object type and we don't have to constrain a
   * guard to get it, we can avoid emitting runtime checks if we know the
   * result is true. If we don't know, we still have to emit a runtime check
   * because src might be a subtype of the specialized type.
   */
  if (srcType < TObj && srcType.clsSpec()) {
    auto const cls = srcType.clsSpec().cls();
    if (!env.irb->constrainValue(src, GuardConstraint(cls).setWeak()) &&
        ((knownCls && cls->classof(knownCls)) ||
         cls->name()->tsame(className))) {
      return cns(env, true);
    }
  }

  // Every case after this point requires knowing things about knownCls.
  if (knownCls == nullptr) return nullptr;

  auto const ssaClassName = cns(env, className);
  auto const objClass     = gen(env, LdObjClass, src);

  if (env.context.kind == TransKind::Profile && !InstanceBits::initted()) {
    gen(env, ProfileInstanceCheck, cns(env, knownCls));
  } else if (env.context.kind == TransKind::Optimize ||
             InstanceBits::initted()) {
    InstanceBits::init();
    if (InstanceBits::lookup(className) != 0) {
      return gen(env, InstanceOfBitmask, objClass, ssaClassName);
    }
  }

  // If the class is an interface, we can just hit the class's vtable or
  // interface map and call it a day.
  if (isInterface(knownCls)) {
    auto const slot = knownCls->preClass()->ifaceVtableSlot();
    if (slot != kInvalidSlot) {
      assertx(Cfg::Repo::Authoritative);
      return gen(env,
                 InstanceOfIfaceVtable,
                 InstanceOfIfaceVtableData{knownCls, true},
                 objClass);
    }

    auto const extra = InstanceOfData { true };
    return gen(env, InstanceOfIface, extra, objClass, ssaClassName);
  }

  // If knownCls isn't a normal class, our caller may want to do something
  // different.
  return isNormalClass(knownCls) ?
    gen(env, ExtendsClass, ExtendsClassData{ knownCls }, objClass) : nullptr;
}

constexpr size_t kNumDataTypes = 18;
constexpr std::array<DataType, kNumDataTypes> computeDataTypes() {
  std::array<DataType, kNumDataTypes> result = {};
  size_t index = 0;
#define DT(name, value, ...) {                                 \
    auto constexpr dt = KindOf##name;                          \
    if (dt == dt_modulo_persistence(dt)) result[index++] = dt; \
  }
  DATATYPES
#undef DT
#ifdef __clang__
  always_assert(index == kNumDataTypes);
#endif
  return result;
}

constexpr std::array<DataType, kNumDataTypes> kDataTypes = computeDataTypes();

/*
 * Emit a type-check for the given type-constraint. Since the details can vary
 * quite a bit depending on what the type-constraint represents, this function
 * is heavily templatized.
 *
 * Returns the potentially updated value after parameter type check
 * and any coersions
 *
 * The lambda parameters are as follows:
 *
 * - GetTcInfo:  Return a string describing the origin of the type-constraint
 * - GetThisCls: Return the SSATmp of the the class of `this'
 * - Fail:       Emit code to deal with the type check failing.
 * - Callable:   Emit code to verify that the given value is callable.
 * - VerifyCls:  Emit code to verify that the given value is an instance of the
 *               given Class.
 * - Fallback:   Called when the type check cannot be resolved statically.
 *               Call a runtime helper to do the check.
 */
template <typename TGetTcInfo,
  typename TGetThisCls,
  typename TFail,
  typename TCallable,
  typename TVerifyCls,
  typename TFallback>
SSATmp* verifyTypeImpl(IRGS& env,
  const TypeConstraint& tc,
  bool onlyCheckNullability,
  SSATmp* inputVal,
  TGetTcInfo getTcInfo,
  TGetThisCls getThisCls,
  TFail fail,
  TCallable callable,
  TVerifyCls verifyCls,
  TFallback fallback) {

  // Ensure that we should bother checking the type at all. If it's uncheckable
  // (because it's an unenforcible type) then just return.
  if (!tc.isCheckable()) return inputVal;

  auto const genThisCls = [&]() {
    return tc.isThis() ? getThisCls() : cns(env, nullptr);
  };

  auto const genFail = [&](SSATmp* val, SSATmp* thisCls = nullptr) {
    if (thisCls == nullptr) thisCls = genThisCls();

    auto const failHard = Cfg::Repo::Authoritative
      && !tc.isSoft()
      && !tc.isThis();
    fail(val, thisCls, failHard);
  };

  // Check `val` against `tc` using `result` as a rule.
  auto const checkOneType = [&](SSATmp* val, AnnotAction result) -> SSATmp* {
    assertx(val->type().isKnownDataType());

    auto const skipInheritedConversion = tc.isInherited() &&
      (tc.isSoft() || !Cfg::Eval::ConvertClassToStringForInheritedConstraints);

    auto const emitClassToStringNotice = [&] {
      std::string msg;
      string_printf(
        msg,
        skipInheritedConversion
          ? Strings::CLASS_TO_STRING_IMPLICIT_INHERITED
          : Strings::CLASS_TO_STRING_IMPLICIT,
        getTcInfo().c_str()
      );
      gen(
        env,
        RaiseNotice,
        SampleRateData { Cfg::Eval::ClassStringHintNoticesSampleRate },
        cns(env, makeStaticString(msg))
      );
    };

    switch (result) {
      case AnnotAction::Pass:
        return val;

      case AnnotAction::Fail:
        genFail(val);
        return val;

      case AnnotAction::Fallback:
        fallback(val, genThisCls(), false);
        return val;

      case AnnotAction::FallbackCoerce:
        return fallback(val, genThisCls(), true);

      case AnnotAction::CallableCheck:
        if (tc.isUnion()) not_implemented(); // TODO(T151885113)
        callable(val);
        return val;

      case AnnotAction::ObjectCheck:
        // We'll check objects next.
        break;

      case AnnotAction::WarnClassToString: {
        assertx(val->type() <= TCls);
        emitClassToStringNotice();
        [[fallthrough]];
      }
      case AnnotAction::ConvertClassToString:
        assertx(val->type() <= TCls);
        if (skipInheritedConversion) return val;
        return gen(env, LdClsName, val);

      case AnnotAction::WarnLazyClassToString: {
        assertx(val->type() <= TLazyCls);
        emitClassToStringNotice();
        [[fallthrough]];
      }
      case AnnotAction::ConvertLazyClassToString:
        assertx(val->type() <= TLazyCls);
        if (skipInheritedConversion) return val;
        return gen(env, LdLazyClsName, val);

      case AnnotAction::WarnClassname: {
        assertx(val->type() <= TCls || val->type() <= TLazyCls);
        gen(env,
          RaiseNotice,
          SampleRateData { Cfg::Eval::ClassnameNoticesSampleRate },
          cns(env, tc.isInherited()
            ? s_CLASS_TO_CLASSNAME_INHERITED.get()
            : s_CLASS_TO_CLASSNAME.get())
        );
        return val;
      }
      case AnnotAction::WarnClass: {
        assertx(val->type() <= TStr);
        gen(env,
          RaiseNotice,
          SampleRateData { Cfg::Eval::ClassNoticesSampleRate },
          cns(env, tc.isInherited()
            ? s_STRING_TO_CLASS_INHERITED.get()
            : s_STRING_TO_CLASS.get())
        );
        return val;
      }
    }
    assertx(result == AnnotAction::ObjectCheck);
    assertx(val->type() <= TObj);
    assertx(!onlyCheckNullability);

    SSATmp* thisCls = nullptr;
    auto genCheckThis = [&](Block* taken) {
      // For 'this' type checks, the class needs to be an exact match.
      thisCls = getThisCls();
      auto const objClass = gen(env, LdObjClass, val);
      return gen(env, JmpZero, taken, gen(env, EqCls, thisCls, objClass));
    };

    // At this point, we know that val is a TObj.
    if (tc.isThis()) {
      ifThen(
        env,
        genCheckThis,
        [&] {
          hint(env, Block::Hint::Unlikely);
          genFail(val, thisCls);
        }
      );
      return val;
    }

    // At this point, we know that val is a TObj and that tc is an Object.
    if (tc.isUnion()) {
      MultiCond mc{env};
      if (tc.unionHasThis()) {
        // Add a check for `this` as a union member.
        mc.ifThen(
          genCheckThis,
          [&](SSATmp*) {
            return val;
          });
      }
      // Iterate through the matching classes.
      for (auto& tc : eachClassTypeConstraintInUnion(tc)) {
        assertx(tc.isSubObject() || tc.isUnresolved());
        mc.ifThen(
          [&](Block* taken) -> SSATmp* {
            auto const clsName = tc.isSubObject()
              ? tc.clsName() : tc.typeName();
            auto const isInstance = implInstanceOfD(env, val, clsName);
            return gen(env, JmpZero, taken, isInstance);
          },
          [&](SSATmp*) {
            return val;
          });
      }

      return mc.elseDo([&] {
        genFail(val);
        return val;
      });
    } else {
      // Non-union:
      assertx(tc.isSubObject() || tc.isUnresolved());
      auto const clsName = tc.isSubObject() ? tc.clsName() : tc.typeName();
      auto const checkCls = lookupCls(env, clsName);
      auto const fastIsInstance = implInstanceCheck(env, val, clsName, checkCls);
      if (fastIsInstance) {
        return cond(
          env,
          [&] (Block* taken) {
            gen(env, JmpZero, taken, fastIsInstance);
          },
          [&] {
            if (checkCls->hasConstVal(TCls)) {
              // we know that val is an instance of checkCls, use that refine val's type
              return gen(env, AssertType, Type::SubObj(checkCls->clsVal()), val);
            } else {
              return val;
            }
          },
          [&] {
            hint(env, Block::Hint::Unlikely);
            genFail(val);
            return val;
          }
        );
      }
      verifyCls(val, checkCls);
    }
    return val;
  }; // end checkOneType

  assertx(inputVal->type() <= TCell);
  auto const valType = inputVal->type();

  // Given a DataType compute what AnnotAction to take.
  auto const computeAction = [&](DataType dt, const TypeConstraint& tc) -> AnnotAction {
    assertx(!tc.isUnion());
    if (dt == KindOfNull && tc.isNullable()) return AnnotAction::Pass;
    auto const name = tc.isSubObject() ? tc.clsName() : tc.typeName();
    auto const action = annotCompat(dt, tc.type(), name);
    if (action != AnnotAction::ObjectCheck) return action;

    if (onlyCheckNullability) return AnnotAction::Pass;
    if (tc.isThis()) return action;
    assertx(tc.isSubObject() || tc.isUnresolved());

    if (!valType.clsSpec()) return action;
    auto const cls = valType.clsSpec().cls();

    if (env.irb->constrainValue(inputVal, GuardConstraint(cls).setWeak())) {
      // We would have to constrain a guard to get the `cls` value.
      return action;
    }

    // Exact name match -- the type check will always pass.
    auto const clsName = tc.isSubObject() ? tc.clsName() : tc.typeName();
    if (cls->name()->same(clsName)) return AnnotAction::Pass;

    if (auto const knownCls = lookupKnown(env, clsName)) {
      // Subclass of a unique class.
      if (cls->classof(knownCls)) return AnnotAction::Pass;
    }

    return action;
  };

  auto const computeUnionAction = [&computeAction](DataType dt, const TypeConstraint& tuc) -> AnnotAction {
    if (tuc.isUnion()) {
      AnnotAction fallbackAction = AnnotAction::Fail;
      for (auto& tc : eachTypeConstraintInUnion(tuc)) {
        auto const action = computeAction(dt, tc);
        switch (action) {
          case AnnotAction::Fail:
            break;

          case AnnotAction::Pass:
            return AnnotAction::Pass;

          case AnnotAction::FallbackCoerce:
            // We never coerce a union - so always just treat it like a Fallback.
          case AnnotAction::Fallback:
            // We can't compute this statically - so we need to fall back to
            // runtime evaluation.
            fallbackAction = AnnotAction::Fallback;
            // Continue to loop just in case we see a Pass later on.
            break;

          case AnnotAction::CallableCheck:
          case AnnotAction::ObjectCheck:
          case AnnotAction::WarnClassToString:
          case AnnotAction::ConvertClassToString:
          case AnnotAction::WarnClassname:
          case AnnotAction::WarnClass:
          case AnnotAction::WarnLazyClassToString:
          case AnnotAction::ConvertLazyClassToString:
            if (fallbackAction == AnnotAction::Fail) {
              fallbackAction = action;
            } else {
              fallbackAction = AnnotAction::Fallback;
            }
            break;
        }
      }

      return fallbackAction;
    } else {
      return computeAction(dt, tuc);
    }
  };

  if (valType.isKnownDataType()) {
    // The type is well-known statically. Compute and then perform an action
    // based on the static type.
    auto const dt = valType.toDataType();
    auto action = computeUnionAction(dt, tc);
    return checkOneType(inputVal, action);
  }

  // Order matters here. When merging we always take the "largest" value.
  enum { None, Fail, Fallback, FallbackCoerce } fallbackAction = None;

  auto const options = [&]{
    TinyVector<std::pair<DataType, AnnotAction>, kNumDataTypes> result;
    // Go through the basic datatypes and for each one that the TC could be add
    // an entry to `result` saying how to handle it.
    for (auto const dt : kDataTypes) {
      auto const type = Type(dt);
      if (!valType.maybe(type)) continue;
      auto const action = computeUnionAction(dt, tc);
      switch (action) {
        case AnnotAction::Fail:
          // We should never get this - we already determined that the types
          // could overlap so when would we ever get 'Fail'?
          fallbackAction = std::max(fallbackAction, Fail);
          break;
        case AnnotAction::Fallback:
          // We can't compute this statically - so we need to fall back to
          // runtime evaluation.
          fallbackAction = std::max(fallbackAction, Fallback);
          break;
        case AnnotAction::FallbackCoerce:
          // We can't compute this statically - so we need to fall back to
          // runtime evaluation and then we'll write the coerced value back to
          // the variable (this happens for Class or LazyClass).
          fallbackAction = std::max(fallbackAction, FallbackCoerce);
          break;
        case AnnotAction::CallableCheck:
        case AnnotAction::ConvertClassToString:
        case AnnotAction::ConvertLazyClassToString:
        case AnnotAction::ObjectCheck:
        case AnnotAction::Pass:
        case AnnotAction::WarnClassToString:
        case AnnotAction::WarnClassname:
        case AnnotAction::WarnClass:
        case AnnotAction::WarnLazyClassToString:
          result.emplace_back(dt, action);
          break;
      }
    }
    // We had no options to try and no action just do nothing. Note that this is
    // different behavior than an empty union (which is default fail)!
    return result;
  }();

  if (fallbackAction == None &&
      std::all_of(options.begin(), options.end(), [] (auto const& pair) {
        return pair.second == AnnotAction::Pass;
      })) {
    return inputVal;
  }

  // TODO(kshaunak): If we were a bit more sophisticated here, we could
  // merge the cases for certain types, like TVec|TDict, or TArrLike.

  // At runtime do a type-switch and figure out which (if any) of our options
  // actually matches and perform that action. In the case that nothing matches
  // then perform the fallbackAction.
  MultiCond mc{env};
  for (auto const& pair : options) {
    mc.ifTypeThen(inputVal, Type(pair.first), [&](SSATmp* val) -> SSATmp* {
      return checkOneType(val, pair.second);
    });
  }
  return mc.elseDo([&]{
    switch (fallbackAction) {
      case None:
        gen(env, Unreachable, ASSERT_REASON);
        break;
      case Fail:
        hint(env, Block::Hint::Unlikely);
        genFail(inputVal);
        return inputVal;
      case Fallback:
        fallback(inputVal, genThisCls(), false);
        return inputVal;
      case FallbackCoerce:
        return fallback(inputVal, genThisCls(), true);
    }
    return cns(env, TBottom);
  });
}

Type typeOpToType(IsTypeOp op) {
  switch (op) {
  case IsTypeOp::Null:    return TInitNull;
  case IsTypeOp::Int:     return TInt;
  case IsTypeOp::Dbl:     return TDbl;
  case IsTypeOp::Bool:    return TBool;
  case IsTypeOp::Str:     return TStr;
  case IsTypeOp::Keyset:  return TKeyset;
  case IsTypeOp::Obj:     return TObj;
  case IsTypeOp::Res:     return TRes;
  case IsTypeOp::ClsMeth:
  case IsTypeOp::Func:
  case IsTypeOp::Class:
  case IsTypeOp::Vec:
  case IsTypeOp::Dict:
  case IsTypeOp::ArrLike:
  case IsTypeOp::LegacyArrLike:
  case IsTypeOp::Scalar: not_reached();
  }
  not_reached();
}

SSATmp* isScalarImpl(IRGS& env, SSATmp* val) {
  // The simplifier works fine when val has a known DataType, but do some
  // checks first in case val has a type like {Int|Str}.
  auto const scalar = TBool | TInt | TDbl | TStr | TFunc | TCls | TLazyCls | TEnumClassLabel;
  if (val->isA(scalar)) return cns(env, true);
  if (!val->type().maybe(scalar)) return cns(env, false);

  MultiCond mc{env};
  for (auto t : {TBool, TInt, TDbl, TStr, TFunc, TCls, TLazyCls, TEnumClassLabel}) {
    mc.ifTypeThen(val, t, [&](SSATmp*) { return cns(env, true); });
  }
  return mc.elseDo([&]{ return cns(env, false); });
}

const StaticString s_FUNC_CONVERSION(Strings::FUNC_TO_STRING);
const StaticString s_FUNC_IS_STRING("Func used in is_string");
const StaticString s_CLASS_IS_STRING("Class used in is_string");
const StaticString s_TYPE_STRUCT_NOT_DARR("Type-structure is not a darray");

SSATmp* isStrImpl(IRGS& env, SSATmp* src) {
  MultiCond mc{env};

  mc.ifTypeThen(src, TStr, [&](SSATmp*) { return cns(env, true); });

  mc.ifTypeThen(src, TLazyCls, [&](SSATmp*) {
    if (Cfg::Eval::ClassIsStringNotices) {
      gen(env, RaiseNotice, SampleRateData {},
          cns(env, s_CLASS_IS_STRING.get()));
    }
    return cns(env, true);
  });

  mc.ifTypeThen(src, TCls, [&](SSATmp*) {
    if (Cfg::Eval::ClassIsStringNotices) {
      gen(env, RaiseNotice, SampleRateData {},
          cns(env, s_CLASS_IS_STRING.get()));
    }
    return cns(env, true);
  });

  return mc.elseDo([&]{ return cns(env, false); });
}

SSATmp* isClassImpl(IRGS& env, SSATmp* src) {
  MultiCond mc{env};
  mc.ifTypeThen(src, TLazyCls, [&](SSATmp*) { return cns(env, true); });
  mc.ifTypeThen(src, TCls, [&](SSATmp*) { return cns(env, true); });
  return mc.elseDo([&]{ return cns(env, false); });
}

SSATmp* isFuncImpl(IRGS& env, SSATmp* src) {
  MultiCond mc{env};
  mc.ifTypeThen(src, TFunc, [&](SSATmp* func) {
    auto const attr = AttrData { AttrIsMethCaller };
    auto const isMC = gen(env, FuncHasAttr, attr, func);
    return gen(env, EqBool, isMC, cns(env, false));
  });
  mc.ifTypeThen(src, TRFunc, [&](SSATmp*) { return cns(env, true); });
  return mc.elseDo([&]{ return cns(env, false); });
}

SSATmp* isClsMethImpl(IRGS& env, SSATmp* src) {
  MultiCond mc{env};
  mc.ifTypeThen(src, TClsMeth, [&](SSATmp*) { return cns(env, true); });
  mc.ifTypeThen(src, TRClsMeth, [&](SSATmp*) { return cns(env, true); });
  return mc.elseDo([&]{ return cns(env, false); });
}

SSATmp* isVecImpl(IRGS& env, SSATmp* src) {
  return isType(env, TVec, src);
}

SSATmp* isDictImpl(IRGS& env, SSATmp* src) {
  return isType(env, TDict, src);
}

SSATmp* isArrLikeImpl(IRGS& env, SSATmp* src) {
  return isType(env, TArrLike, src);
}

SSATmp* isLegacyArrLikeImpl(IRGS& env, SSATmp* src) {
  MultiCond mc{env};

  mc.ifTypeThen(src, TVec|TDict, [&](SSATmp* src) {
    return gen(env, IsLegacyArrLike, src);
  });

  return mc.elseDo([&]{ return cns(env, false); });
}

//////////////////////////////////////////////////////////////////////

}

SSATmp* implInstanceOfD(IRGS& env, SSATmp* src, const StringData* className) {
  /*
   * InstanceOfD is always false if it's not an object.
   *
   * We're prepared to generate translations for known non-object types, but if
   * it's Gen/Cell we're going to PUNT because it's natural to translate that
   * case with control flow TODO(#16781576)
   */
  if (TObj < src->type()) {
    PUNT(InstanceOfD_MaybeObj);
  }
  if (!src->isA(TObj)) {
    if (src->isA(TCls | TLazyCls)) {
      if (!interface_supports_string(className)) return cns(env, false);
      if (Cfg::Eval::ClassIsStringNotices && src->isA(TCls)) {
        gen(
          env,
          RaiseNotice,
          SampleRateData {},
          cns(env, s_CLASS_IS_STRING.get())
        );
      }
      return cns(env, true);
    }

    auto const res =
      (src->isA(TArrLike) && interface_supports_arrlike(className)) ||
      (src->isA(TStr) && interface_supports_string(className)) ||
      (src->isA(TInt) && interface_supports_int(className)) ||
      (src->isA(TDbl) && interface_supports_double(className));
    return cns(env, res);
  }

  auto const checkCls = lookupCls(env, className);
  if (auto isInstance = implInstanceCheck(env, src, className, checkCls)) {
    return isInstance;
  }

  return gen(env,
             InstanceOf,
             InstanceOfData { true },
             gen(env, LdObjClass, src),
             checkCls);
}

//////////////////////////////////////////////////////////////////////

void emitInstanceOfD(IRGS& env, const StringData* className) {
  auto const src = popC(env);
  push(env, implInstanceOfD(env, src, className));
  decRef(env, src);
}

void emitInstanceOf(IRGS& env) {
  auto const t1 = popC(env);
  auto const t2 = popC(env); // t2 instanceof t1

  if (t1->isA(TObj) && t2->isA(TObj)) {
    auto const c2 = gen(env, LdObjClass, t2);
    auto const c1 = gen(env, LdObjClass, t1);
    auto const extra = InstanceOfData { true };
    push(env, gen(env, InstanceOf, extra, c2, c1));
    decRef(env, t2, DecRefProfileId::InstanceOfSrc2);
    decRef(env, t1, DecRefProfileId::InstanceOfSrc1);
    return;
  }

  if (!t1->isA(TStr)) PUNT(InstanceOf-NotStr);

  if (t2->isA(TObj)) {
    auto const c1 = gen(env, LookupCls, t1);
    auto const c2  = gen(env, LdObjClass, t2);
    auto const extra = InstanceOfData { true };
    push(env, gen(env, InstanceOf, extra, c2, c1));
    decRef(env, t2, DecRefProfileId::InstanceOfSrc2);
    decRef(env, t1, DecRefProfileId::InstanceOfSrc1);
    return;
  }

  auto const res = [&]() -> SSATmp* {
    if (t2->isA(TArrLike)) return gen(env, InterfaceSupportsArrLike, t1);
    if (t2->isA(TInt))     return gen(env, InterfaceSupportsInt, t1);
    if (t2->isA(TStr))     return gen(env, InterfaceSupportsStr, t1);
    if (t2->isA(TDbl))     return gen(env, InterfaceSupportsDbl, t1);
    if (t2->isA(TCls)) {
      if (Cfg::Eval::RaiseClassConversionNoticeSampleRate == 0) {
        return gen(env, InterfaceSupportsStr, t1);
      }
      return cond(
        env,
        [&] (Block* taken) {
          gen(env, JmpZero, taken, gen(env, InterfaceSupportsStr, t1));
        },
        [&] {
          std::string msg;
          string_printf(msg, Strings::CLASS_TO_STRING_IMPLICIT, "instanceof");
          gen(env,
            RaiseNotice,
            SampleRateData { Cfg::Eval::RaiseClassConversionNoticeSampleRate },
            cns(env, makeStaticString(msg)));
          return cns(env, true);
        },
        [&] { return cns(env, false); }
      );
    }
    if (!t2->type().maybe(TObj|TArrLike|TInt|TStr|TDbl)) return cns(env, false);
    return nullptr;
  }();

  if (!res) PUNT(InstanceOf-Unknown);

  push(env, res);
  decRef(env, t2, DecRefProfileId::InstanceOfSrc2);
  decRef(env, t1, DecRefProfileId::InstanceOfSrc1);
}

void emitIsLateBoundCls(IRGS& env) {
  auto const cls = curClass(env);
  if (!cls) PUNT(IsLateBoundCls-NoClassContext);
  if (isTrait(cls)) PUNT(IsLateBoundCls-Trait);
  auto const obj = popC(env);
  if (obj->isA(TObj)) {
    auto const rhs = ldCtxCls(env);
    auto const lhs  = gen(env, LdObjClass, obj);
    push(env, gen(env, InstanceOf, InstanceOfData { true }, lhs, rhs));
  } else if (!obj->type().maybe(TObj)) {
    push(env, cns(env, false));
  } else {
    PUNT(IsLateBoundCls-MaybeObject);
  }
  decRef(env, obj);
}

namespace {

template<typename F>
SSATmp* resolveTypeStructureAndCacheInRDS(
  IRGS& env,
  F resolveTypeStruct,
  bool typeStructureCouldBeNonStatic
) {
  if (typeStructureCouldBeNonStatic) return resolveTypeStruct();
  auto const handle = rds::alloc<TypedValue>().handle();
  auto const data = RDSHandleAndType { handle, TDict };
  auto const addr = gen(env, LdRDSAddr, data, TPtrToOther);
  ifThen(
    env,
    [&] (Block* taken) {
      gen(env, CheckRDSInitialized, taken, RDSHandleData { handle });
    },
    [&] {
      hint(env, Block::Hint::Unlikely);
      gen(env, StMem, addr, resolveTypeStruct());
      gen(env, MarkRDSInitialized, RDSHandleData { handle });
    }
  );
  return gen(env, LdMem, TDict, addr);
}

SSATmp* resolveTypeStructImpl(
  IRGS& env,
  bool typeStructureCouldBeNonStatic,
  bool suppress,
  uint32_t n,
  bool isOrAsOp
) {
  auto const declaringCls = curFunc(env) ? curClass(env) : nullptr;
  auto const calledCls =
    declaringCls && typeStructureCouldBeNonStatic
      ? ldCtxCls(env)
      : cns(env, nullptr);
  auto const result = resolveTypeStructureAndCacheInRDS(
    env,
    [&] {
      return gen(
        env,
        ResolveTypeStruct,
        ResolveTypeStructData {
          declaringCls,
          suppress,
          spOffBCFromIRSP(env),
          static_cast<uint32_t>(n),
          isOrAsOp
        },
        sp(env),
        calledCls
      );
    },
    typeStructureCouldBeNonStatic
  );
  popC(env);
  discard(env, n - 1);
  return result;
}

const ArrayData* staticallyResolveTypeStructure(
  IRGS& env,
  const ArrayData* ts,
  bool& partial,
  bool& invalidType
) {
  auto const declaringCls = curFunc(env) ? curClass(env) : nullptr;
  bool persistent = false;
  // This shouldn't do a difference, but does on GCC 8.3 on Ubuntu 19.04;
  // if we take the catch then return `ts`, it's a bogus value and we
  // segfault... sometimes...
  const ArrayData* ts_copy = ts;
  try {
    auto newTS = TypeStructure::resolvePartial(
      ArrNR(ts), nullptr, declaringCls, persistent, partial, invalidType);
    if (persistent) return ArrayData::GetScalarArray(std::move(newTS));
  } catch (Exception& ) {}
  // We are here because either we threw in the resolution or it wasn't
  // persistent resolution which means we didn't really resolve it
  partial = true;
  return ts_copy;
}

SSATmp* check_nullable(IRGS& env, SSATmp* res, SSATmp* var) {
  return cond(
    env,
    [&] (Block* taken) { gen(env, JmpNZero, taken, res); },
    [&] { return isType(env, TNull, var); },
    [&] { return cns(env, true); }
  );
}

template<typename... Types>
void chain_is_type(IRGS& env, SSATmp* c, bool nullable, Types... tys) {
  static_assert(sizeof...(tys) >= 2);
  MultiCond mc{env};
  for (auto const ty : {tys...}) {
    mc.ifTypeThen(c, ty, [&](SSATmp*) { return cns(env, true); });
  }
  if (nullable) {
    mc.ifTypeThen(c, TNull, [&](SSATmp*) { return cns(env, true); });
  }
  push(env, mc.elseDo([&] { return cns(env, false); }));
}

/*
 * This function tries to emit is type struct operations without resolving
 * the type structure when that's possible.
 * When it returns true, it has popped two values from the stack, namely the
 * type structure and the cell, and pushed one value back to stack, namely
 * true/false if it is an is-operation or the cell if it is an as operation.
 * This function does not modify the reference counts of these stack values,
 * leaving that responsibility to the caller.
 * When it returns false, it does not modify anything.
 */
bool emitIsTypeStructWithoutResolvingIfPossible(
  IRGS& env,
  const ArrayData* ts,
  TypeStructResolveOp op
) {
  // Top of the stack is the type structure, so the thing we are checking is
  // the next element
  auto const t = topC(env, BCSPRelOffset { 1 });
  auto const is_nullable_ts = is_ts_nullable(ts);

  auto const cnsResult = [&] (bool value) {
    popC(env); // pop the ts that's on the stack
    popC(env); // pop the cell
    push(env, cns(env, value));
    return true;
  };

  auto const success = [&] { return cnsResult(true); };
  auto const fail = [&] { return cnsResult(false); };

  auto const primitive = [&] (Type ty) {
    auto const nty = is_nullable_ts ? ty|TNull : ty;
    if (t->isA(nty)) return success();
    if (!t->type().maybe(nty)) return fail();
    popC(env); // pop the ts that's on the stack
    auto const c = popC(env);
    auto const res = isType(env, ty, c);
    push(env, is_nullable_ts ? check_nullable(env, res, c) : res);
    return true;
  };

  // We explicitly bind is_nullable_ts because failing to do so causes a
  // spurious compiler error on some g++ versions.
  auto const unionOf = [&,is_nullable_ts] (Type ty1, Type ty2,
                                           auto&&... rest) {
    auto const ty = Type::unionAll(ty1, ty2, rest...) |
                    (is_nullable_ts ? TNull : TBottom);
    if (t->isA(ty)) return success();
    if (!t->type().maybe(ty)) return fail();

    popC(env); // pop the ts that's on the stack
    auto const c = popC(env);
    chain_is_type(env, c, is_nullable_ts, ty1, ty2, rest...);
    return true;
  };

  auto const primitiveKindToType = [](TypeStructure::Kind kind) {
    switch (kind) {
      case TypeStructure::Kind::T_int:         return TInt;
      case TypeStructure::Kind::T_bool:        return TBool;
      case TypeStructure::Kind::T_float:       return TDbl;
      case TypeStructure::Kind::T_null:        return TNull;
      case TypeStructure::Kind::T_void:        return TNull;
      case TypeStructure::Kind::T_keyset:      return TKeyset;
      default: always_assert(false && "Not primitive");
    }
    not_reached();
  };

  auto const classnameForResolvedClass = [&](const ArrayData* arr) -> const StringData* {
    auto const clsname = get_ts_classname(arr);
    if (arr->exists(s_generic_types)) {
      auto cls = lookupKnown(env, clsname);
      if ((cls && cls->hasReifiedGenerics()) || !isTSAllWildcards(arr)) {
        // If it is a reified class or has non wildcard generics,
        // we need to bail
        return nullptr;
      }
    }
    return clsname;
  };

  if (t->isA(TNull) && is_nullable_ts) return success();

  auto kind = get_ts_kind(ts);
  switch (kind) {
    case TypeStructure::Kind::T_int:
    case TypeStructure::Kind::T_bool:
    case TypeStructure::Kind::T_float:
    case TypeStructure::Kind::T_null:
    case TypeStructure::Kind::T_void:
    case TypeStructure::Kind::T_keyset:
      return primitive(primitiveKindToType(kind));
    case TypeStructure::Kind::T_string: {
      if (t->type().maybe(TLazyCls) &&
          Cfg::Eval::ClassIsStringNotices) {
        ifElse(env,
          [&] (Block* taken) {
            gen(env, CheckType, TLazyCls, taken, t);
          },
          [&] {
            gen(env, RaiseNotice, SampleRateData {},
                cns(env, s_CLASS_IS_STRING.get()));
          }
        );
      }
      if (t->type().maybe(TCls) &&
          Cfg::Eval::ClassIsStringNotices) {
        ifElse(env,
          [&] (Block* taken) {
            gen(env, CheckType, TCls, taken, t);
          },
          [&] {
            gen(env, RaiseNotice, SampleRateData {},
                cns(env, s_CLASS_IS_STRING.get()));
          }
        );
      }
      return unionOf(TStr, TLazyCls, TCls);
    }
    case TypeStructure::Kind::T_nonnull:     return primitive(TNonNull);
    case TypeStructure::Kind::T_mixed:
    case TypeStructure::Kind::T_dynamic:
      return success();
    case TypeStructure::Kind::T_num:         return unionOf(TInt, TDbl);
    case TypeStructure::Kind::T_arraykey: {
      if (t->type().maybe(TLazyCls) &&
          Cfg::Eval::ClassIsStringNotices) {
        ifElse(env,
          [&] (Block* taken) {
            gen(env, CheckType, TLazyCls, taken, t);
          },
          [&] {
            gen(env, RaiseNotice, SampleRateData {},
                cns(env, s_CLASS_IS_STRING.get()));
          }
        );
      }
      if (t->type().maybe(TCls) &&
          Cfg::Eval::ClassIsStringNotices) {
        ifElse(env,
          [&] (Block* taken) {
            gen(env, CheckType, TCls, taken, t);
          },
          [&] {
            gen(env, RaiseNotice, SampleRateData {},
                cns(env, s_CLASS_IS_STRING.get()));
          }
        );
      }
      return unionOf(TInt, TStr, TLazyCls, TCls);
    }
    case TypeStructure::Kind::T_any_array:
      return unionOf(TVec, TDict, TKeyset);
    case TypeStructure::Kind::T_vec_or_dict:
    case TypeStructure::Kind::T_varray_or_darray:
    case TypeStructure::Kind::T_dict:
    case TypeStructure::Kind::T_vec:
    case TypeStructure::Kind::T_darray:
    case TypeStructure::Kind::T_varray: {
      popC(env); // pop the ts that's on the stack
      auto const c = popC(env);
      auto const res = [&]{
        if (kind == TypeStructure::Kind::T_dict ||
            kind == TypeStructure::Kind::T_darray) {
          return isDictImpl(env, c);
        } else if (kind == TypeStructure::Kind::T_vec ||
                   kind == TypeStructure::Kind::T_varray) {
          return isVecImpl(env, c);
        } else {
          assertx(kind == TypeStructure::Kind::T_vec_or_dict ||
                  kind == TypeStructure::Kind::T_varray_or_darray);
          return cond(
            env,
            [&](Block* taken) { gen(env, JmpZero, taken, isVecImpl(env, c)); },
            [&] { return cns(env, true); },
            [&] { return isDictImpl(env, c); }
          );
        }
      }();
      push(env, is_nullable_ts ? check_nullable(env, res, c) : res);
      return true;
    }
    case TypeStructure::Kind::T_class:
    case TypeStructure::Kind::T_interface:
    case TypeStructure::Kind::T_xhp: {
      auto const clsname = classnameForResolvedClass(ts);
      if (!clsname) return false;
      popC(env); // pop the ts that's on the stack
      auto const c = popC(env);
      auto const res = implInstanceOfD(env, c, clsname);
      push(env, is_nullable_ts ? check_nullable(env, res, c) : res);
      return true;
    }
    case TypeStructure::Kind::T_nothing:
    case TypeStructure::Kind::T_noreturn:
      return fail();
    case TypeStructure::Kind::T_union: {
      // Even if we can tell for sure that the type structure will or won't
      // permit a particular value, we still need to match the runtime behavior
      // of rejecting invalid type structures. In particular we may have a type
      // structure that would require recursion or that contains an invalid arm
      // of the union (even if it wasn't actually important for verifying the
      // value at hand).
      if (op != TypeStructResolveOp::DontResolve ||
          !errorOnIsAsExpressionInvalidTypes(ArrNR{ts}, true)) {
        return false;
      }
      hphp_fast_set<Type> primitives;
      hphp_fast_set<const StringData*> instances;
      bool fallback = false;
      if (is_nullable_ts) primitives.emplace(TNull);
      IterateV(
        get_ts_union_types(ts),
        [&](TypedValue ty) {
          assertx(isArrayLikeType(ty.m_type));
          auto const arr = ty.m_data.parr;
          if (is_ts_nullable(arr)) primitives.emplace(TNull);
          auto const arr_kind = get_ts_kind(arr);
          switch (arr_kind) {
            case TypeStructure::Kind::T_int:
            case TypeStructure::Kind::T_bool:
            case TypeStructure::Kind::T_float:
            case TypeStructure::Kind::T_null:
            case TypeStructure::Kind::T_void:
            case TypeStructure::Kind::T_keyset:
              primitives.emplace(primitiveKindToType(arr_kind));
              break;
            case TypeStructure::Kind::T_string:
              if (Cfg::Eval::ClassIsStringNotices) {
                // punt
                fallback = true;
                return true; // short-circuit
              }
              primitives.insert({TStr, TLazyCls, TCls});
              break;
            case TypeStructure::Kind::T_class:
            case TypeStructure::Kind::T_interface:
            case TypeStructure::Kind::T_xhp: {
              auto const clsname = classnameForResolvedClass(arr);
              if (!clsname) {
                // punt
                fallback = true;
                return true; // short-circuit
              }
              instances.emplace(clsname);
              break;
            }
            default:
              fallback = true;
              return true; // short-circuit
          }
          return false; // keep-going
        }
      );
      if (fallback) return false; // fallback to regular check
      assertx(!primitives.empty() || !instances.empty());
      popC(env); // pop the ts that's on the stack
      auto const c = popC(env);

      MultiCond mc{env};
      for (auto& ty : primitives) {
        mc.ifTypeThen(c, ty, [&](SSATmp*) { return cns(env, true); });
      }
      for (auto const clsname : instances) {
        mc.ifThen(
          [&] (Block* taken) {
            auto const success = implInstanceOfD(env, c, clsname);
            gen(env, JmpZero, taken, success);
            return c;
          },
          [&] (SSATmp*) { return cns(env, true); }
        );
      }
      push(env, mc.elseDo([&]{ return cns(env, false); }));
      return true;
    }
    case TypeStructure::Kind::T_typevar:
    case TypeStructure::Kind::T_fun:
    case TypeStructure::Kind::T_trait:
    case TypeStructure::Kind::T_recursiveUnion:
      // Not supported, will throw an error on these at the resolution phase
      return false;
    case TypeStructure::Kind::T_resource: {
      if (!t->type().maybe(TRes)) return fail();
      return false;
    }
    case TypeStructure::Kind::T_enum:
    case TypeStructure::Kind::T_tuple:
    case TypeStructure::Kind::T_shape:
    case TypeStructure::Kind::T_typeaccess:
    case TypeStructure::Kind::T_unresolved:
    case TypeStructure::Kind::T_reifiedtype:
      // TODO(T28423611): Implement these
      return false;
    case TypeStructure::Kind::T_class_ptr:
      // TODO(T199610905) Do nothing until we're ready to enforce inner type
      return false;
    case TypeStructure::Kind::T_class_or_classname:
      // Banned by typechecker as with above case, no plans to support
      return false;
  }
  not_reached();
}

/*
 * shouldDefRef is set iff the resulting SSATmp is a newly allocated type
 * structure
 * This function does not modify the reference count of its inputs, leaving that
 * to the caller
 */
SSATmp* handleIsResolutionAndCommonOpts(
  IRGS& env,
  TypeStructResolveOp op,
  bool& done,
  bool& shouldDecRef,
  bool& checkValid
) {
  auto const a = topC(env);
  if (!a->isA(TDict)) PUNT(IsTypeStructC-NotArrayTypeStruct);
  if (!a->hasConstVal(TDict)) {
    if (op == TypeStructResolveOp::Resolve) {
      return resolveTypeStructImpl(env, true, true, 1, true);
    }
    shouldDecRef = false;
    checkValid = true;
    return popC(env);
  }
  auto const ts = a->arrLikeVal();
  auto maybe_resolved = ts;
  bool partial = true;
  bool invalidType = true;
  if (op == TypeStructResolveOp::Resolve) {
    maybe_resolved =
      staticallyResolveTypeStructure(env, ts, partial, invalidType);
    shouldDecRef = maybe_resolved != ts;
  }
  if (op == TypeStructResolveOp::Resolve && invalidType) {
    shouldDecRef = typeStructureCouldBeNonStatic(ts);
    return resolveTypeStructImpl(
      env, typeStructureCouldBeNonStatic(ts), true, 1, true);
  }
  if (emitIsTypeStructWithoutResolvingIfPossible(env, maybe_resolved, op)) {
    done = true;
    return nullptr;
  }
  if (op == TypeStructResolveOp::Resolve && partial) {
    shouldDecRef = typeStructureCouldBeNonStatic(ts);
    return resolveTypeStructImpl(
      env, typeStructureCouldBeNonStatic(ts), true, 1, true);
  }
  popC(env);
  if (op == TypeStructResolveOp::DontResolve) checkValid = true;
  else {
    checkValid = errorOnIsAsExpressionInvalidTypes(ArrNR{maybe_resolved}, true);
  }
  return cns(env, maybe_resolved);
}

} // namespace

void emitIsTypeStructC(IRGS& env, TypeStructResolveOp op, TypeStructEnforceKind kind) {
  auto const a = topC(env);
  auto const c = topC(env, BCSPRelOffset { 1 });
  bool done = false, shouldDecRef = true, checkValid = false;
  SSATmp* tc =
    handleIsResolutionAndCommonOpts(env, op, done, shouldDecRef, checkValid);
  if (done) {
    decRef(env, c, DecRefProfileId::IsTypeStructCc);
    decRef(env, a, DecRefProfileId::IsTypeStructCa);
    return;
  }
  popC(env);
  auto block = opcodeMayRaise(IsTypeStruct) && shouldDecRef
    ? makeCatchBlock(env, [&]{
        decRef(env, tc, DecRefProfileId::IsTypeStructCTc);
      })
    : nullptr;
  auto const data = RDSHandleData { rds::bindTSCache(curFunc(env)).handle() };

  static const StaticString s_IsTypeStruct{"IsTypeStruct"};
  auto const profile = TargetProfile<IsTypeStructProfile> {
    env.context,
    env.irb->curMarker(),
    s_IsTypeStruct.get()
  };

  auto const generic = [&] {
    if (checkValid) gen(env, RaiseErrorOnInvalidIsAsExpressionType, tc);
    return gen(env, IsTypeStruct, block, data, tc, c);
  };

  auto const finish = [&] (SSATmp* result) {
    push(env, result);
    decRef(env, c, DecRefProfileId::IsTypeStructCc);
    decRef(env, a, DecRefProfileId::IsTypeStructCa);
    if (shouldDecRef) decRef(env, tc, DecRefProfileId::IsTypeStructCTc);
  };

  if (kind == TypeStructEnforceKind::Shallow) {
    if (checkValid) gen(env, RaiseErrorOnInvalidIsAsExpressionType, tc);
    finish(gen(env, IsTypeStructShallow, block, data, tc, c));
    return;
  }

  if (profile.profiling()) {
    gen(env, ProfileIsTypeStruct, RDSHandleData { profile.handle() }, a);
    finish(generic());
    return;
  }

  if (!profile.optimizing() || !profile.data().shouldOptimize()) {
    finish(generic());
    return;
  }

  finish(cond(
    env,
    [&] (Block* taken) {
      return gen(env, IsTypeStructCached, taken, a, c);
    },
    [&] (SSATmp* result) { // next
      return result;
    },
    [&] { // taken
      hint(env, Block::Hint::Unlikely);
      return generic();
    }
  ));
}

void emitThrowAsTypeStructException(IRGS& env, AsTypeStructExceptionKind kind) {
  auto const arr = topC(env);
  auto const c = topC(env, BCSPRelOffset { 1 });
  auto const tsAndBlock = [&]() -> std::pair<SSATmp*, Block*> {
    if (arr->hasConstVal(TDict)) {
      auto const ts = arr->arrLikeVal();
      bool partial = true, invalidType = true;
      auto maybe_resolved =
        staticallyResolveTypeStructure(env, ts, partial, invalidType);
      if (!ts->same(maybe_resolved)) {
        auto const inputTS = cns(env, maybe_resolved);
        return {inputTS, makeCatchBlock(env, [&]{ decRef(env, inputTS); })};
      }
    }
    auto const ts = resolveTypeStructImpl(env, true, false, 1, true);
    return {ts, makeCatchBlock(env, [&]{ decRef(env, ts); })};
  }();
  // No need to decref inputs as this instruction will throw
  if (kind == AsTypeStructExceptionKind::Error) {
    gen(env, ThrowAsTypeStructError, tsAndBlock.second, tsAndBlock.first, c);
  } else {
    gen(env, ThrowAsTypeStructException, tsAndBlock.second, tsAndBlock.first, c);
  }
}

void emitRecordReifiedGeneric(IRGS& env) {
  auto const ts = popC(env);
  if (!ts->isA(TVec)) {
    PUNT(RecordReifiedGeneric-InvalidTS);
  }
  // RecordReifiedGenericsAndGetTSList decrefs the ts
  auto const result = gen(env, RecordReifiedGenericsAndGetTSList, ts);
  push(env, result);
}

void emitCombineAndResolveTypeStruct(IRGS& env, uint32_t n) {
  push(env, resolveTypeStructImpl(env, true, false, n, false));
}

void raiseClsmethCompatTypeHint(
  IRGS& env, int32_t id, const Func* func, const TypeConstraint& tc) {
  auto name = tc.displayName(func->cls());
  if (id == TypeConstraint::ReturnId) {
    gen(env, RaiseNotice, SampleRateData {}, cns(env, makeStaticString(
      fmt::format("class_meth Compat: Value returned from function {}() "
      "must be of type {}, clsmeth given",
        func->fullName()->slice(), name))));
  } else {
    gen(env, RaiseNotice, SampleRateData {}, cns(env, makeStaticString(
      fmt::format("class_meth Compat: Argument {} passed to {}() "
      "must be of type {}, clsmeth given",
        id + 1, func->fullName()->slice(), name))));
  }
}

void verifyRetType(
  IRGS& env,
  int32_t id,
  BCSPRelOffset stackOffset,
  HPHP::VerifyRetKind kind
) {
  auto const func = curFunc(env);
  auto const verifyFunc = [&] (
    const TypeConstraint& tc,
    SSATmp* inputVal,
    bool onlyCheckNullability
  ) -> SSATmp* {
    return verifyTypeImpl(
      env,
      tc,
      onlyCheckNullability,
      inputVal,
      [&] {
        if (id == TypeConstraint::ReturnId) {
          return fmt::format("return of {}()", func->fullName()->slice());
        } else {
          return fmt::format(
            "argument {} returned from {}() as an inout parameter",
            id+1, func->fullName()->slice());
        }
      },
      [&] { // Get the class representing `this' type
        return ldCtxCls(env);
      },
      [&] (SSATmp* val, SSATmp* thisCls, bool hard) { // Check failure
        updateStackOffsetAndExceptionBoundary(env);
        gen(
          env,
          hard ? VerifyRetFailHard : VerifyRetFail,
          FuncParamWithTCData { func, id, &tc },
          val,
          thisCls
        );
      },
      [&] (SSATmp* val) { // Callable check
        gen(
          env,
          VerifyRetCallable,
          FuncParamWithTCData { func, id, &tc },
          val
        );
      },
      [&] (SSATmp* val, SSATmp* checkCls) {
        // Class/type-alias check
        gen(
          env,
          VerifyRetCls,
          FuncParamWithTCData { func, id, &tc },
          val,
          gen(env, LdObjClass, val),
          checkCls
        );
      },
      [&] (SSATmp* val, SSATmp* thisCls, bool mayCoerce) { // Fallback
        return gen(
          env,
          mayCoerce ? VerifyRetCoerce : VerifyRet,
          FuncParamWithTCData { func, id, &tc },
          val,
          thisCls
        );
      }
    );
  };

  auto const val = topC(env, stackOffset, DataTypeGeneric);
  assertx(stackOffset.offset >= 0);
  auto const& tic = (id == TypeConstraint::ReturnId)
    ? func->returnTypeConstraints()
    : func->params()[id].typeConstraints;

  auto updatedVal = val;
  if (Cfg::Eval::EnforceOverriddenReturnTypes
      && id == TypeConstraint::ReturnId) {
    for (auto const& tc : tic.range()) {
      if (!tc.isInherited() && kind == VerifyRetKind::None) continue;
      updatedVal = verifyFunc(
        tc,
        updatedVal,
        !tc.isInherited() && kind == VerifyRetKind::NonNull
      );
    }
  } else {
    if (kind == VerifyRetKind::None) return;
    for (auto const& tc : tic.range()) {
      if (tc.isInherited()) continue;
      updatedVal = verifyFunc(tc, updatedVal, kind == VerifyRetKind::NonNull);
    }
  }

  if (updatedVal != val) {
    auto const offset = offsetFromIRSP(env, stackOffset);
    gen(env, StStk, IRSPRelOffsetData{offset}, sp(env), updatedVal);
    env.irb->exceptionStackBoundary();
  }
}

void verifyParamType(IRGS& env, const Func* func, int32_t id,
                     BCSPRelOffset offset, SSATmp* prologueCtx) {
  if (func->params()[id].isOutOnly()) return;

  auto const verifyFunc = [&](const TypeConstraint& tc, SSATmp* inputVal) -> SSATmp* {
    return verifyTypeImpl(
      env,
      tc,
      false,  // onlyCheckNullability
      inputVal,
      [&] {
        return fmt::format(
          "argument {} passed to {}()", id + 1, func->fullName()->slice());
      },
      [&] { // Get the class representing `this' type
        if (prologueCtx == nullptr) return ldCtxCls(env);

        if (!func->cls()) return cns(env, nullptr);
        if (func->isClosureBody()) {
          auto const closureTy = Type::ExactObj(func->implCls());
          auto const closure = gen(env, AssertType, closureTy, prologueCtx);
          if (func->isStatic()) {
            return gen(env, LdClosureCls, Type::SubCls(func->cls()), closure);
          }
          auto const closureThis =
            gen(env, LdClosureThis, Type::SubObj(func->cls()), closure);
          return gen(env, LdObjClass, closureThis);
        }

        if (func->isStatic()) {
          return gen(env, AssertType, Type::SubCls(func->cls()), prologueCtx);
        }
        auto const thiz =
          gen(env, AssertType, Type::SubObj(func->cls()), prologueCtx);
        return gen(env, LdObjClass, thiz);
      },
      [&] (SSATmp* val, SSATmp* thisCls, bool hard) { // Check failure
        gen(
          env,
          hard ? VerifyParamFailHard : VerifyParamFail,
          FuncParamWithTCData { func, id, &tc },
          val,
          thisCls
        );
      },
      [&] (SSATmp* val) { // Callable check
        gen(
          env,
          VerifyParamCallable,
          FuncParamWithTCData { func, id, &tc },
          val
        );
      },
      [&] (SSATmp* val, SSATmp* checkCls) {
        // Class/type-alias check
        gen(
          env,
          VerifyParamCls,
          FuncParamWithTCData { func, id, &tc },
          val,
          gen(env, LdObjClass, val),
          checkCls
        );
      },
      [&] (SSATmp* val, SSATmp* thisCls, bool mayCoerce) { // Fallback
        return gen(
          env,
          mayCoerce ? VerifyParamCoerce : VerifyParam,
          FuncParamWithTCData { func, id, &tc },
          val,
          thisCls
        );
      }
    );
  };
  auto const val = topC(env, offset, DataTypeGeneric);
  auto updatedVal = val;
  for (auto const& tc : func->params()[id].typeConstraints.range()) {
    updatedVal = verifyFunc(tc, updatedVal);
  }

  if (updatedVal != val) {
    auto const irspRelOffset = offsetFromIRSP(env, offset);
    gen(env, StStk, IRSPRelOffsetData{irspRelOffset}, sp(env), updatedVal);
    updateStackOffsetAndExceptionBoundary(env);
  }
}

SSATmp* verifyPropType(IRGS& env,
                       SSATmp* cls,
                       const TypeIntersectionConstraint* tcs,
                       Slot slot,
                       SSATmp* val,
                       SSATmp* name,
                       bool isSProp) {
  assertx(cls->isA(TCls));
  assertx(val->isA(TCell));

  if (Cfg::Eval::CheckPropTypeHints <= 0) return val;

  auto const verifyFunc = [&](const TypeConstraint* tc, SSATmp* inputVal) -> SSATmp* {
    if (!tc || !tc->isCheckable()) return inputVal;
    assertx(tc->validForProp());

    auto const fallback = [&](SSATmp* val, SSATmp*, bool mayCoerce) -> SSATmp* {
      return gen(
        env,
        mayCoerce ? VerifyPropCoerce : VerifyProp,
        TypeConstraintData { tc },
        cls,
        cns(env, slot),
        val,
        cns(env, isSProp)
      );
    };

    if (tc->mayCoerce()) env.irb->constrainValue(inputVal, DataTypeSpecific);

    return verifyTypeImpl(
      env,
      *tc,
      false,  // onlyCheckNullability
      inputVal,
      [&] {
        if (name->hasConstVal(TStaticStr)) {
          return fmt::format("property {}", name->strVal()->slice());
        } else {
          return std::string("property");
        }
      },
      [&] { // Get the class representing `this' type
        return cls;
      },
      [&] (SSATmp* val, SSATmp*, bool hard) { // Check failure
        auto const failHard =
          hard && Cfg::Eval::CheckPropTypeHints >= 3;
        gen(
          env,
          failHard ? VerifyPropFailHard : VerifyPropFail,
          TypeConstraintData{ tc },
          cls,
          cns(env, slot),
          val,
          cns(env, isSProp)
        );
      },
      // We don't allow callable as a property type-hint, so we should never
      // need to check callability.
      [&] (SSATmp*) { always_assert(false); },
      [&] (SSATmp* val, SSATmp* checkCls) { // Class/type-alias check
        gen(
          env,
          VerifyPropCls,
          TypeConstraintData{ tc },
          cls,
          cns(env, slot),
          checkCls,
          val,
          cns(env, isSProp)
        );
      },
      fallback
    );
  };
  auto updatedVal = val;
  for (auto const& tc : tcs->range()) {
    updatedVal = verifyFunc(&tc, updatedVal);
  }

  return updatedVal;
}

void emitVerifyRetTypeTS(IRGS& env) {
  auto const ts = popC(env);
  auto const cell = topC(env);
  auto const reified = tcCouldBeReified(curFunc(env), TypeConstraint::ReturnId);
  if (reified || cell->isA(TObj)) {
    auto const funcData = FuncData { curFunc(env) };
    gen(env, VerifyReifiedReturnType, funcData, cell, ts, ldCtxCls(env));
  } else if (cell->type().maybe(TObj) && !reified) {
    // Meaning we did not not guard on the stack input correctly
    PUNT(VerifyRetTypeTS-UnguardedObj);
  }
  decRef(env, ts);
}

void emitVerifyTypeTS(IRGS& env) {
  // Don't emit code if sample rate is zero
  if (Cfg::Eval::CheckedUnsafeCastSampleRate == 0) {
    popDecRef(env);
    return;
  }
  if (!topC(env)->isA(TDict)) return interpOne(env);
  auto const ts = topC(env);
  auto const cell = topC(env, BCSPRelOffset{1});
  auto const funcData = FuncData { curFunc(env) };
  gen(env, VerifyType, funcData, cell, ts, ldCtxCls(env));
  popDecRef(env);
}

void emitVerifyOutType(IRGS& env, int32_t paramId) {
  verifyRetType(env, paramId, BCSPRelOffset {0}, HPHP::VerifyRetKind::All);
}

void emitVerifyParamType(IRGS& env, int32_t paramId) {
  verifyParamType(env, curFunc(env), paramId, BCSPRelOffset{0}, nullptr);
}

void emitVerifyParamTypeTS(IRGS& env, int32_t paramId) {
  auto const ts = popC(env);
  auto const cell = ldLoc(env, paramId, DataTypeSpecific);
  auto const reified = tcCouldBeReified(curFunc(env), paramId);
  if (cell->isA(TObj) || reified) {
    cond(
      env,
      [&] (Block* taken) {
        return gen(env, CheckType, TDict, taken, ts);
      },
      [&] (SSATmp* dts) {
        auto const fpData = FuncParamData { curFunc(env), paramId };
        gen(env, VerifyReifiedLocalType, fpData, cell, dts, ldCtxCls(env));
        return nullptr;
      },
      [&] {
        gen(env, RaiseError, cns(env, s_TYPE_STRUCT_NOT_DARR.get()));
        return nullptr;
      }
    );
  } else if (cell->type().maybe(TObj)) {
    // Meaning we did not not guard on the stack input correctly
    PUNT(VerifyReifiedLocalType-UnguardedObj);
  }
  decRef(env, ts);
}

void emitOODeclExists(IRGS& env, OODeclExistsOp subop) {
  auto const tAutoload = topC(env);
  auto const tCls = topC(env, BCSPRelOffset{1});

  if (!tCls->isA(TStr) || !tAutoload->isA(TBool)){ // result of Cast
    PUNT(OODeclExists-BadTypes);
  }

  ClassKind kind;
  switch (subop) {
  case OODeclExistsOp::Class:     kind = ClassKind::Class; break;
  case OODeclExistsOp::Trait:     kind = ClassKind::Trait; break;
  case OODeclExistsOp::Interface: kind = ClassKind::Interface; break;
  }

  auto const val = gen(
    env,
    OODeclExists,
    ClassKindData { kind },
    tCls,
    tAutoload
  );
  discard(env, 2);
  push(env, val);
  decRef(env, tCls);
}

void emitIssetL(IRGS& env, int32_t id) {
  auto const ld = ldLoc(env, id, DataTypeSpecific);
  push(env, isType(env, TNonNull, ld));
}

void emitIsUnsetL(IRGS& env, int32_t id) {
  auto const ld = ldLoc(env, id, DataTypeSpecific);
  push(env, isType(env, TUninit, ld));
}

SSATmp* isTypeHelper(IRGS& env, IsTypeOp subop, SSATmp* val) {
  switch (subop) {
    case IsTypeOp::Vec:           return isVecImpl(env, val);
    case IsTypeOp::Dict:          return isDictImpl(env, val);
    case IsTypeOp::Scalar:        return isScalarImpl(env, val);
    case IsTypeOp::Str:           return isStrImpl(env, val);
    case IsTypeOp::ArrLike:       return isArrLikeImpl(env, val);
    case IsTypeOp::LegacyArrLike: return isLegacyArrLikeImpl(env, val);
    case IsTypeOp::Class:         return isClassImpl(env, val);
    case IsTypeOp::Func:          return isFuncImpl(env, val);
    case IsTypeOp::ClsMeth:       return isClsMethImpl(env, val);
    default: break;
  }

  auto const t = typeOpToType(subop);
  return t <= TObj ? optimizedCallIsObject(env, val) : isType(env, t, val);
}

void emitIsTypeC(IRGS& env, IsTypeOp subop) {
  auto const val = popC(env, DataTypeSpecific);
  push(env, isTypeHelper(env, subop, val));
  decRef(env, val);
}

void emitIsTypeL(IRGS& env, NamedLocal loc, IsTypeOp subop) {
  auto const val = ldLocWarn(env, loc, DataTypeSpecific);
  push(env, isTypeHelper(env, subop, val));
}

//////////////////////////////////////////////////////////////////////

void emitAssertRATL(IRGS& env, int32_t loc, RepoAuthType rat) {
  assertTypeLocal(env, loc, typeFromRAT(rat, curClass(env)));
}

void emitAssertRATStk(IRGS& env, uint32_t offset, RepoAuthType rat) {
  assertTypeStack(
    env,
    BCSPRelOffset{safe_cast<int32_t>(offset)},
    typeFromRAT(rat, curClass(env))
  );
}

//////////////////////////////////////////////////////////////////////

}
