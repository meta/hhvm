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

#include <fmt/format.h>

#include "hphp/runtime/vm/jit/translator-runtime.h"

#include "hphp/runtime/base/array-common.h"
#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/base/builtin-functions.h"
#include "hphp/runtime/base/collections.h"
#include "hphp/runtime/base/datatype.h"
#include "hphp/runtime/base/execution-context.h"
#include "hphp/runtime/base/object-data.h"
#include "hphp/runtime/base/string-data.h"
#include "hphp/runtime/base/tv-refcount.h"
#include "hphp/runtime/base/tv-type.h"
#include "hphp/runtime/base/tv-variant.h"
#include "hphp/runtime/base/typed-value.h"
#include "hphp/runtime/base/type-structure-helpers-defs.h"
#include "hphp/runtime/base/type-structure-helpers.h"
#include "hphp/runtime/base/vanilla-dict.h"
#include "hphp/runtime/base/vanilla-keyset.h"
#include "hphp/runtime/base/vanilla-vec.h"

#include "hphp/runtime/ext/collections/ext_collections-pair.h"
#include "hphp/runtime/ext/collections/ext_collections-vector.h"

#include "hphp/runtime/vm/act-rec.h"
#include "hphp/runtime/vm/class.h"
#include "hphp/runtime/vm/class-meth-data-ref.h"
#include "hphp/runtime/vm/func.h"
#include "hphp/runtime/vm/member-operations.h"
#include "hphp/runtime/vm/method-lookup.h"
#include "hphp/runtime/vm/reified-generics.h"
#include "hphp/runtime/vm/named-params.h"
#include "hphp/runtime/vm/resumable.h"
#include "hphp/runtime/vm/type-constraint.h"
#include "hphp/runtime/vm/unit-util.h"

#include "hphp/runtime/vm/jit/coeffect-fun-param-profile.h"
#include "hphp/runtime/vm/jit/target-cache.h"
#include "hphp/runtime/vm/jit/target-profile.h"
#include "hphp/runtime/vm/jit/translator-inline.h"
#include "hphp/runtime/vm/jit/unwind-itanium.h"

#include "hphp/system/systemlib.h"

#include "hphp/util/portability.h"
#include "hphp/util/random.h"
#include "hphp/util/service-data.h"
#include "hphp/util/text-util.h"

namespace HPHP {

TRACE_SET_MOD(runtime)

//////////////////////////////////////////////////////////////////////

namespace jit {

//////////////////////////////////////////////////////////////////////

void setNewElem(tv_lval base, TypedValue val) {
  HPHP::SetNewElem<false>(base, &val);
}

//////////////////////////////////////////////////////////////////////

ArrayData* addNewElemVec(ArrayData* vec, TypedValue v) {
  assertx(vec->isVanillaVec());
  return VanillaVec::AppendMove(vec, v);
}

//////////////////////////////////////////////////////////////////////

ArrayData* convArrLikeToVecHelper(ArrayData* adIn) {
  auto a = adIn->toVec(adIn->cowCheck());
  assertx(a->isVecType());
  if (a != adIn) decRefArr(adIn);
  return a;
}

ArrayData* convObjToVecHelper(ObjectData* obj) {
  auto a = castObjToVec(obj);
  assertx(a->isVecType());
  decRefObj(obj);
  return a;
}

ArrayData* convArrLikeToDictHelper(ArrayData* adIn) {
  auto a = adIn->toDict(adIn->cowCheck());
  assertx(a->isDictType());
  if (a != adIn) decRefArr(adIn);
  return a;
}

ArrayData* convObjToDictHelper(ObjectData* obj) {
  auto a = castObjToDict(obj);
  assertx(a->isDictType());
  decRefObj(obj);
  return a;
}

ArrayData* convArrLikeToKeysetHelper(ArrayData* adIn) {
  auto a = adIn->toKeyset(adIn->cowCheck());
  assertx(a->isKeysetType());
  if (a != adIn) decRefArr(adIn);
  return a;
}

ArrayData* convObjToKeysetHelper(ObjectData* obj) {
  auto a = castObjToKeyset(obj);
  assertx(a->isKeysetType());
  decRefObj(obj);
  return a;
}

double convObjToDblHelper(const ObjectData* o) {
  return o->toDouble();
}

double convStrToDblHelper(const StringData* s) {
  return s->toDouble();
}

double convResToDblHelper(const ResourceHdr* r) {
  return r->getId();
}

double convTVToDblHelper(TypedValue tv) {
  return tvCastToDouble(tv);
}

StringData* convDblToStrHelper(double d) {
  return buildStringData(d);
}

StringData* convIntToStrHelper(int64_t i) {
  return buildStringData(i);
}

StringData* convObjToStrHelper(ObjectData* o) {
  // toString() returns a counted String; detach() it to move ownership
  // of the count to the caller
  return o->invokeToString().detach();
}

void throwUndefPropException(ObjectData* base, const StringData* name) {
  base->throwUndefPropException(name);
}

void throwUndefVariable(StringData* nm) {
  SCOPE_EXIT { decRefStr(nm); };
  SystemLib::throwUndefinedVariableExceptionObject(
    fmt::format("Undefined variable: {}", nm->data()));
}

void raise_error_sd(const StringData *msg) {
  raise_error("%s", msg->data());
}

ALWAYS_INLINE
static bool VerifyTypeClsImpl(const Class* cls,
                              const Class* constraint,
                              const TypeConstraint* expected) {
  // This helper should only be called for the Object and Unresolved cases
  assertx(expected->isSubObject() || expected->isUnresolved());
  // If we have a resolved class for the constraint, all we have to do is
  // check if the value's class is compatible with it
  if (LIKELY(constraint != nullptr)) {
    return cls->classof(constraint);
  }
  // Handle the case where the constraint is a type alias if it could be
  return expected->isUnresolved() && expected->checkTypeAliasObj(cls);
}

void VerifyParamTypeCls(ObjectData* obj,
                        const Class* constraint,
                        const Func* func,
                        int32_t paramId,
                        const TypeConstraint* expected) {
  if (!VerifyTypeClsImpl(obj->getVMClass(), constraint, expected)) {
    assertx(expected->isSubObject() || expected->isUnresolved());
    VerifyParamTypeFail(
      make_tv<KindOfObject>(obj), nullptr, func, paramId, expected);
  }
}

void VerifyParamTypeCallable(TypedValue value, const Func* func,
                             int32_t paramId, const TypeConstraint* tc) {
  if (UNLIKELY(!is_callable(tvAsCVarRef(&value)))) {
    assertx(tc->isCallable());
    VerifyParamTypeFail(value, nullptr, func, paramId, tc);
  }
}

TypedValue VerifyParamType(TypedValue value, const Class* ctx,
                           const Func* func, int32_t paramId,
                           const TypeConstraint* tc) {
  assertx(tvIsPlausible(value));
  assertx(tc->isCheckable());
  tc->verifyParam(&value, ctx, func, paramId);
  assertx(tvIsPlausible(value));
  return value;
}

void VerifyParamTypeFail(TypedValue value, const Class* ctx,
                         const Func* func, int32_t paramId,
                         const TypeConstraint* tc) {
  DEBUG_ONLY auto const origType = value.type();
  assertx(tvIsPlausible(value));
  assertx(!tc->check(&value, ctx));
  tc->verifyParamFail(&value, ctx, func, paramId);
  assertx(value.type() == origType);
}

void VerifyRetTypeCls(ObjectData* obj,
                      const Class* constraint,
                      const Func* func,
                      int32_t retId,
                      const TypeConstraint* expected) {
  if (!VerifyTypeClsImpl(obj->getVMClass(), constraint, expected)) {
    assertx(expected->isSubObject() || expected->isUnresolved());
    VerifyRetTypeFail(
      make_tv<KindOfObject>(obj), nullptr, func, retId, expected);
  }
}

void VerifyRetTypeCallable(TypedValue value, const Func* func,
  int32_t retId, const TypeConstraint* tc) {
  if (UNLIKELY(!is_callable(tvAsCVarRef(&value)))) {
    assertx(tc->isCallable());
    VerifyRetTypeFail(value, nullptr, func, retId, tc);
  }
}

TypedValue VerifyRetType(TypedValue value, const Class* ctx,
                         const Func* func, int32_t retId,
                         const TypeConstraint* tc) {
  assertx(tvIsPlausible(value));
  assertx(tc->isCheckable());
  if (retId == TypeConstraint::ReturnId) {
    tc->verifyReturn(&value, ctx, func);
  } else {
    tc->verifyOutParam(&value, ctx, func, retId);
  }
  assertx(tvIsPlausible(value));
  return value;
}

void VerifyRetTypeFail(TypedValue value, const Class* ctx,
                       const Func* func, int32_t retId,
                       const TypeConstraint* tc) {
  DEBUG_ONLY auto const origType = value.type();
  if (retId == TypeConstraint::ReturnId) {
    assertx(!tc->check(&value, ctx));
    tc->verifyReturnFail(&value, ctx, func);
  } else {
    assertx(!tc->check(&value, ctx));
    tc->verifyOutParamFail(&value, ctx, func, retId);
  }
  assertx(value.type() == origType);
}

void VerifyReifiedLocalTypeImpl(TypedValue value, ArrayData* ts,
                                const Class* ctx, const Func* func,
                                int32_t paramId) {
  auto const couldBeReified = tcCouldBeReified(func, paramId);
  bool warn = false;
  if (verifyReifiedLocalType(&value, ts, ctx, func, couldBeReified, warn)) {
    return;
  }
  raise_reified_typehint_error(
    fmt::format(
      "Argument {} passed to {}() must be an instance of {}, {} given",
      paramId + 1,
      func->fullName()->data(),
      TypeStructure::toString(ArrNR(ts),
        TypeStructure::TSDisplayType::TSDisplayTypeUser).c_str(),
      describe_actual_type(&value)
    ), warn
  );
}

void VerifyReifiedReturnTypeImpl(TypedValue value, ArrayData* ts,
                                 const Class* ctx, const Func* func) {
  auto const couldBeReified = tcCouldBeReified(func, TypeConstraint::ReturnId);
  bool warn = false;
  if (verifyReifiedLocalType(&value, ts, ctx, func, couldBeReified, warn)) {
    return;
  }
  raise_reified_typehint_error(
    fmt::format(
      "Value returned from function {}() must be of type {}, {} given",
      func->fullName()->data(),
      TypeStructure::toString(ArrNR(ts),
        TypeStructure::TSDisplayType::TSDisplayTypeUser).c_str(),
      describe_actual_type(&value)
    ), warn
  );
}

void VerifyTypeImpl(TypedValue value, ArrayData* ts,
                                 const Class* ctx, const Func* func) {
  // We shouldn't even have generated code if flag isn't set
  assertx(Cfg::Eval::CheckedUnsafeCast);
  if (folly::Random::oneIn(Cfg::Eval::CheckedUnsafeCastSampleRate, threadLocalRng64())) {
    req::vector<Array> tsList;
    std::string givenType, expectedType, errorKey;
    auto resolved_ts = resolveAndVerifyTypeStructure<false>(
      ArrNR(ts), func->cls(), ctx, tsList, true);
    if (!checkForVerifyTypeStructureMatchesTV(resolved_ts, value, givenType, expectedType, errorKey)) {
      raise_inline_typehint_error(givenType, expectedType, errorKey);
    }
  }
}

namespace {

ALWAYS_INLINE
TypedValue getDefaultIfMissing(TypedValue tv, TypedValue def) {
  return tv.is_init() ? tv : def;
}

ALWAYS_INLINE
TypedValue doScan(const VanillaDict* arr, StringData* key, TypedValue def) {
  assertx(key->isStatic());
  assertx(arr->keyTypes().mustBeStaticStrs());
  auto used = arr->iterLimit();
  for (auto elm = arr->data(); used; used--, elm++) {
    assertx(elm->hasStrKey());
    assertx(elm->strKey()->isStatic());
    if (key == elm->strKey()) return *elm->datatv();
  }
  return def;
}

}

// This helper may also be used when we know we have a VanillaDict in the JIT.
TypedValue dictIdxI(ArrayData* a, int64_t key, TypedValue def) {
  assertx(a->isVanillaDict());
  return getDefaultIfMissing(VanillaDict::NvGetInt(a, key), def);
}

// This helper is also used for VanillaDicts.
NEVER_INLINE
TypedValue dictIdxS(ArrayData* a, StringData* key, TypedValue def) {
  assertx(a->isVanillaDict());
  return getDefaultIfMissing(VanillaDict::NvGetStr(a, key), def);
}

// This helper is also used for VanillaDicts.
NEVER_INLINE
TypedValue dictIdxScan(ArrayData* a, StringData* key, TypedValue def) {
  assertx(a->isVanillaDict());
  auto const ad = VanillaDict::as(a);
  if (!ad->keyTypes().mustBeStaticStrs()) return dictIdxS(a, key, def);
  return doScan(ad, key, def);
}

TypedValue keysetIdxI(ArrayData* a, int64_t key, TypedValue def) {
  assertx(a->isVanillaKeyset());
  return getDefaultIfMissing(VanillaKeyset::NvGetInt(a, key), def);
}

TypedValue keysetIdxS(ArrayData* a, StringData* key, TypedValue def) {
  assertx(a->isVanillaKeyset());
  return getDefaultIfMissing(VanillaKeyset::NvGetStr(a, key), def);
}

template <bool isFirst>
TypedValue vecFirstLast(ArrayData* a) {
  assertx(a->isVanillaVec());
  auto const size = a->size();
  if (UNLIKELY(size == 0)) return make_tv<KindOfNull>();
  return VanillaVec::NvGetInt(a, isFirst ? 0 : size - 1);
}

template TypedValue vecFirstLast<true>(ArrayData*);
template TypedValue vecFirstLast<false>(ArrayData*);

template <bool isFirst, bool isKey>
TypedValue arrFirstLast(ArrayData* a) {
  if (UNLIKELY(a->empty())) {
    return make_tv<KindOfNull>();
  }
  auto pos = isFirst ? a->iter_begin() : a->iter_last();
  return isKey ? a->nvGetKey(pos) : a->nvGetVal(pos);
}

template TypedValue arrFirstLast<true, false>(ArrayData*);
template TypedValue arrFirstLast<false, false>(ArrayData*);
template TypedValue arrFirstLast<true, true>(ArrayData*);
template TypedValue arrFirstLast<false, true>(ArrayData*);

TypedValue* getSPropOrNull(ReadonlyOp op,
                           const Class* cls,
                           const StringData* name,
                           const Func* ctx,
                           bool ignoreLateInit,
                           bool writeMode) {
  auto const propCtx = MemberLookupContext(ctx->cls(), ctx->moduleName());
  auto const lookup = ignoreLateInit
    ? cls->getSPropIgnoreLateInit(propCtx, name)
    : cls->getSProp(propCtx, name);
  if (writeMode && UNLIKELY(lookup.constant)) {
    throw_cannot_modify_static_const_prop(cls->name()->data(), name->data());
  }
  if (lookup.internal) {
    auto const slot = cls->lookupSProp(name);
    auto const prop = cls->staticProperties()[slot];
    if (will_symbol_raise_module_boundary_violation(&prop, ctx)) {
      raiseModulePropertyViolation(cls, name, ctx->moduleName(), true);
    }
  }
  checkReadonly(lookup.val, cls, name, lookup.readonly, op, writeMode);
  if (UNLIKELY(!lookup.val || !lookup.accessible)) return nullptr;

  return lookup.val;
}

TypedValue* getSPropOrRaise(ReadonlyOp op,
                            const Class* cls,
                            const StringData* name,
                            const Func* ctx,
                            bool ignoreLateInit,
                            bool writeMode) {
  auto sprop = getSPropOrNull(op, cls, name, ctx, ignoreLateInit, writeMode);
  if (UNLIKELY(!sprop)) {
    raise_error("Invalid static property access: %s::%s",
                cls->name()->data(), name->data());
  }
  return sprop;
}

tv_lval ldGblAddrDefHelper(StringData* name) {
  return g_context->m_globalNVTable->lookupAdd(name);
}

//////////////////////////////////////////////////////////////////////

void checkFrame(ActRec* fp, TypedValue* sp, bool fullCheck) {
  const Func* func = fp->func();
  func->validate();
  if (func->cls()) {
    assertx(!func->cls()->isZombie());
  }
  int numLocals = func->numLocals();
  assertx(sp <= (TypedValue*)fp - func->numSlotsInFrame() || isResumed(fp));

  if (!fullCheck) return;

  int numParams = func->numParams();
  for (int i = 0; i < numLocals; i++) {
    if (i >= numParams && isResumed(fp) && i < func->numNamedLocals() &&
        func->localVarName(i)) {
      continue;
    }
    assertx(tvIsPlausible(*frame_local(fp, i)));
  }

  visitStackElems(
    fp, sp,
    [](const TypedValue* tv) {
      assertx(tvIsPlausible(*tv));
    }
  );
}

const Func* loadPublicFunction() {
  const Func* func = nullptr;
  walkStack([&] (const BTFrame& f) {
    if (!func) func = f.func();
    if (f.func()->isNoInjection()) return false;
    func = f.func();
    return true;
  });

  assertx(func);
  return func;
}

const Func* loadClassCtor(Class* cls, Func* ctxFunc) {
  const Func* f = cls->getCtor();
  if (UNLIKELY(!(f->attrs() & AttrPublic))) {
    auto const callCtx = MemberLookupContext(ctxFunc->cls(), ctxFunc);
    UNUSED auto func =
      lookupMethodCtx(cls, nullptr, callCtx,
                      CallType::CtorMethod,
                      MethodLookupErrorOptions::RaiseOnNotFound);
    assertx(func == f);
  }
  return f;
}

const Func* lookupClsMethodHelper(const Class* cls, const StringData* methName,
                                  ObjectData* obj, const Func* ctxFunc) {
  const Func* f;
  auto const callCtx = MemberLookupContext(ctxFunc->cls(), ctxFunc);
  auto const res = lookupClsMethod(f, cls, methName, obj, callCtx,
                                   MethodLookupErrorOptions::RaiseOnNotFound);

  if (res == LookupResult::MethodFoundWithThis) {
    // Handled by interpreter.
    return nullptr;
  }

  assertx(res == LookupResult::MethodFoundNoThis);
  if (!f->isStaticInPrologue()) {
    throw_missing_this(f);
  }

  return f;
}

//////////////////////////////////////////////////////////////////////

TypedValue lookupClsCns(const Class* cls, const StringData* cnsName) {
  return cls->clsCnsGet(cnsName);
}

int lookupClsCtxCns(const Class* cls, const StringData* cnsName) {
  return cls->clsCtxCnsGet(cnsName, true)->value();
}

bool methodExistsHelper(Class* cls, StringData* meth) {
  assertx(isNormalClass(cls) && !isAbstract(cls));
  return cls->lookupMethod(meth) != nullptr;
}

ArrayData* resolveTypeStructHelper(
  uint32_t n,
  const TypedValue* values,
  const Class* declaringCls,
  const Class* calledCls,
  bool suppress,
  bool isOrAsOp
) {
  assertx(n != 0);
  auto const v = *values;
  isValidTSType(v, true);
  auto const ts = v.m_data.parr;
  req::vector<Array> tsList;
  for (int i = 0; i < n - 1; ++i) {
    auto const a = values[n - i - 1];
    isValidTSType(a, true);
    tsList.emplace_back(Array::attach(a.m_data.parr));
  }
  auto resolved = [&] {
    if (isOrAsOp) {
      return resolveAndVerifyTypeStructure<true>(
               ArrNR(ts), declaringCls, calledCls, tsList, suppress);
    }
    return resolveAndVerifyTypeStructure<false>(
             ArrNR(ts), declaringCls, calledCls, tsList, suppress);
  }();
  return resolved.detach();
}

bool isTypeStructHelper(ArrayData* a, TypedValue c, rds::Handle h) {
  auto const cls = TSClassCache::write(h, a);
  if (!cls) return checkTypeStructureMatchesTV(ArrNR(a), c);
  if (!tvIsObject(c)) return false;
  return c.m_data.pobj->getVMClass()->classofNonIFace(cls);
}

bool isTypeStructShallowHelper(ArrayData* a, TypedValue c, rds::Handle h) {
  auto const cls = TSClassCache::write(h, a);
  // The checkTypeStructureMatchesTV overload which accepts a warn flag runs
  // checkTypeStructureMatchesTVImpl with isOrAsOp=false, giving the shallow
  // checking behavior used for param verification and which we want here.
  bool warn = false;
  if (!cls) return checkTypeStructureMatchesTV(ArrNR(a), c, warn);
  if (!tvIsObject(c)) return false;
  return c.m_data.pobj->getVMClass()->classofNonIFace(cls);
}

void profileIsTypeStructHelper(ArrayData* a, IsTypeStructProfile* prof) {
  prof->update(a);
}

void profileCoeffectFunParamHelper(TypedValue tv,
                                   CoeffectFunParamProfile* prof) {
  prof->update(&tv);
}

void throwAsTypeStructExceptionHelper(ArrayData* a, TypedValue c) {
  std::string givenType, expectedType, errorKey;
  auto const ts = ArrNR(a);
  if (!checkTypeStructureMatchesTV(ts, c, givenType, expectedType,
                                     errorKey)) {
    throwTypeStructureDoesNotMatchTVException(
      givenType, expectedType, errorKey, false);
  }
  always_assert(false && "Invalid bytecode sequence: Instruction must throw");
}

void throwAsTypeStructErrorHelper(ArrayData* a, TypedValue c) {
  std::string givenType, expectedType, errorKey;
  auto const ts = ArrNR(a);
  if (!checkTypeStructureMatchesTV(ts, c, givenType, expectedType,
                                     errorKey)) {
    throwTypeStructureDoesNotMatchTVException(
      givenType, expectedType, errorKey, true);
  }
  always_assert(false && "Invalid bytecode sequence: Instruction must throw");
}

ArrayData* errorOnIsAsExpressionInvalidTypesHelper(ArrayData* a) {
  errorOnIsAsExpressionInvalidTypes(ArrNR(a), false);
  return a;
}

ArrayData* recordReifiedGenericsAndGetTSList(ArrayData* tsList) {
  auto const mangledName = makeStaticString(mangleReifiedGenericsName(tsList));
  auto result = addToTypeReifiedGenericsTable(mangledName, tsList);
  return result;
}

ArrayData* loadClsTypeCnsHelper(
  const Class* cls, const StringData* name, bool no_throw_on_undefined
) {
  auto const getFake = [] {
    auto array = make_dict_array(
      s_kind,
      typeStructureKindToVariant(TypeStructure::Kind::T_class),
      s_classname,
      Variant(s_type_structure_non_existant_class)
    );
    array.setEvalScalar();
    return array.get();
  };
  TypedValue typeCns;
  if (no_throw_on_undefined) {
    try {
      typeCns = cls->clsCnsGet(name, ConstModifierFlags::Kind::Type);
    } catch (Exception&) {
      return getFake();
    } catch (Object&) {
      return getFake();
    }
  } else {
    typeCns = cls->clsCnsGet(name, ConstModifierFlags::Kind::Type);
  }
  if (typeCns.m_type == KindOfUninit) {
    if (no_throw_on_undefined) {
      return getFake();
    } else {
      if (cls->hasTypeConstant(name, true)) {
        raise_error("Type constant %s::%s is abstract",
                    cls->name()->data(), name->data());
      } else {
        raise_error("Non-existent type constant %s::%s",
                    cls->name()->data(), name->data());
      }
    }
  }

  assertx(isArrayLikeType(typeCns.m_type));
  assertx(typeCns.m_data.parr->isDictType());
  assertx(typeCns.m_data.parr->isStatic());
  return typeCns.m_data.parr;
}

StringData* loadClsTypeCnsClsNameHelper(const Class* cls,
                                        const StringData* name) {
  auto const ts = loadClsTypeCnsHelper(cls, name, false);
  auto const classname = ts->get(s_classname.get(), true);
  assertx(isStringType(type(classname)));
  assertx(val(classname).pstr->isStatic());
  return val(classname).pstr;
}

void raiseCoeffectsCallViolationHelper(const Func* callee,
                                       uint64_t providedCoeffects,
                                       uint64_t requiredCoeffects) {
  raiseCoeffectsCallViolation(callee,
                              RuntimeCoeffects::fromValue(providedCoeffects),
                              RuntimeCoeffects::fromValue(requiredCoeffects));
}

void initFuncNamedParamDefaults(const Func* callee,
                                uint32_t posArgc,
                                const ArrayData* namedArgNames,
                                TypedValue* stackTop) {
  
  int32_t numNamedArgs = namedArgNames ? namedArgNames->size() : 0;
  // As noted in irlower-exception.cpp, rvmsp() will not account for the named arg
  // names that were passed in since that information isn't encoded in the prologues.
  // We need to manually adjust the stack top to account for the named args.
  // Since the stack grows down, we subtract.
  stackTop -= numNamedArgs;
  if (callee->hasVariadicNamedParam()) {
    // Arguments may be dropped here, but the final layout is the same either
    // way, so the prologue's static stack accounting is unaffected.
    initVariadicNamedParamValues(callee, posArgc, namedArgNames, stackTop);
    return;
  }
  initFuncNamedParamDefaultValues(callee, posArgc, namedArgNames, stackTop);
}

void checkFunNamedArgsMismatch(const Func* callee, uint32_t posArgc,
                               const ArrayData* namedArgNames, TypedValue* stackTop) {

  assertx(callee->hasNamedParams());
  checkNamedArgMismatch(
    callee, namedArgNames, throwUnexpectedNamedArguments,
    throwNamedArgumentNameMismatch, throwMissingNamedParam
  );
  initFuncNamedParamDefaults(callee, posArgc, namedArgNames, stackTop);
}

void checkFunReifiedGenericsWithNamedArgs(const Func* callee,
                                          const ArrayData* namedArgNames,
                                          TypedValue* stackTop) {
  assertx(callee->hasReifiedGenerics());
  auto numNamedArgs = namedArgNames ? namedArgNames->size() : 0;
  stackTop -= numNamedArgs;
  TypedValue generics = *stackTop;
  assertx(tvIsVec(generics));
  checkFunReifiedGenericMismatch(callee, val(generics).parr);
}

void throwOOBException(TypedValue base, TypedValue key) {
  if (isArrayLikeType(base.m_type)) {
    throwOOBArrayKeyException(key, base.m_data.parr);
  } else if (base.m_type == KindOfObject) {
    assertx(isIntType(key.m_type));
    collections::throwOOB(key.m_data.num);
  }
  not_reached();
}

void invalidArrayKeyHelper(const ArrayData* ad, TypedValue key) {
  throwInvalidArrayKeyException(&key, ad);
}

static ServiceData::ExportedCounter* s_staticAnalysisErrors =
  ServiceData::createCounter("vm.static_analysis_errors");

void raiseStaticAnalysisError() {
  s_staticAnalysisErrors->increment();
  raise_error("static analysis error: supposedly unreachable code was reached");
}

namespace MInstrHelpers {

TypedValue setOpElem(tv_lval base, TypedValue key,
                     TypedValue val, SetOpOp op) {
  auto const result = HPHP::SetOpElem(op, base, key, &val);
  tvIncRefGen(result);
  return result;
}

StringData* stringGetI(StringData* base, uint64_t x) {
  if (LIKELY(x < base->size())) {
    return base->getChar(x);
  }
  raise_notice("Uninitialized string offset: %" PRId64,
               static_cast<int64_t>(x));
  return staticEmptyString();
}

uint64_t pairIsset(c_Pair* pair, int64_t index) {
  auto result = pair->get(index);
  return result ? !tvIsNull(result) : false;
}

uint64_t vectorIsset(c_Vector* vec, int64_t index) {
  auto result = vec->get(index);
  return result ? !tvIsNull(*result) : false;
}

TypedValue incDecElem(tv_lval base, TypedValue key, IncDecOp op) {
  auto const result = HPHP::IncDecElem(op, base, key);
  return result;
}

tv_lval elemVecIU(tv_lval base, int64_t key) {
  assertx(tvIsVec(base));
  return ElemUVec<KeyType::Int>(base, key);
}

}

//////////////////////////////////////////////////////////////////////

}}
