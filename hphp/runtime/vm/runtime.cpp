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
#include "hphp/runtime/vm/runtime.h"
#include "hphp/runtime/vm/method-lookup.h"
#include "hphp/runtime/base/backtrace.h"
#include "hphp/runtime/base/execution-context.h"
#include "hphp/runtime/base/coeffects-config.h"
#include "hphp/runtime/base/builtin-functions.h"
#include "hphp/runtime/base/request-info.h"
#include "hphp/runtime/base/unit-cache.h"

#include "hphp/util/configs/eval.h"
#include "hphp/util/conv-10.h"
#include "hphp/util/random.h"
#include "hphp/util/trace.h"
#include "hphp/util/text-util.h"

#include <fmt/format.h>
#include <usdt/usdt.h>
#include <folly/Random.h>

namespace HPHP {

TRACE_SET_MOD(runtime)

/**
 * print_string will decRef the string
 */
void print_string(StringData* s) {
  g_context->write(s->data(), s->size());
  TRACE(2, "t-x64 output(str): (%p) %43s\n", s->data(),
        escapeStringForCPP(s->data(), s->size()).data());
  decRefStr(s);
}

void print_int(int64_t i) {
  char intbuf[21];
  auto const s = conv_10(i, intbuf + sizeof(intbuf));

  g_context->write(s.data(), s.size());
}

void print_boolean(bool val) {
  if (val) {
    g_context->write("1", 1);
  }
}

/**
 * concat_ss will will incRef the output string
 * and decref its first argument
 */
StringData* concat_ss(StringData* v1, StringData* v2) {
  if (v1->cowCheck()) {
    USDT(hhvm, hhvm_cow_concat, v1->size(), v2->size());
    StringData* ret = StringData::Make(v1, v2);
    // Because v1 was shared, we know this won't release the string.
    v1->decRefCount();
    return ret;
  }

  auto const rhs = v2->slice();
  UNUSED auto const lsize = v1->size();
  USDT(hhvm, hhvm_mut_concat, lsize, rhs.size());
  auto const ret = v1->append(rhs);
  if (UNLIKELY(ret != v1)) {
    // had to realloc even though count==1
    assertx(v1->hasExactlyOneRef());
    v1->release();
  }
  return ret;
}

/**
 * concat_is will incRef the output string
 */
StringData* concat_is(int64_t v1, StringData* v2) {
  char intbuf[21];
  // Convert the int to a string
  auto const s1 = conv_10(v1, intbuf + sizeof(intbuf));
  auto const s2 = v2->slice();
  return StringData::Make(s1, s2);
}

/**
 * concat_si will incRef the output string
 * and decref its first argument
 */
StringData* concat_si(StringData* v1, int64_t v2) {
  char intbuf[21];
  auto const s2 = conv_10(v2, intbuf + sizeof(intbuf));
  if (v1->cowCheck()) {
    auto const s1 = v1->slice();
    USDT(hhvm, hhvm_cow_concat, s1.size(), s2.size());
    auto const ret = StringData::Make(s1, s2);
    // Because v1 was shared, we know this won't release it.
    v1->decRefCount();
    return ret;
  }

  UNUSED auto const lsize = v1->size();
  USDT(hhvm, hhvm_mut_concat, lsize, s2.size());
  auto const ret = v1->append(s2);
  if (UNLIKELY(ret != v1)) {
    // had to realloc even though count==1
    assertx(v1->hasExactlyOneRef());
    v1->release();
  }
  return ret;
}

StringData* concat_s3(StringData* v1, StringData* v2, StringData* v3) {
  if (v1->cowCheck()) {
    auto s1 = v1->slice();
    auto s2 = v2->slice();
    auto s3 = v3->slice();
    USDT(hhvm, hhvm_cow_concat, s1.size(), s2.size() + s3.size());
    StringData* ret = StringData::Make(s1, s2, s3);
    // Because v1 was shared, we know this won't release it.
    v1->decRefCount();
    return ret;
  }

  UNUSED auto const lsize = v1->size();
  USDT(hhvm, hhvm_mut_concat, lsize, v2->size() + v3->size());
  auto const ret = v1->append(v2->slice(), v3->slice());
  if (UNLIKELY(ret != v1)) {
    // had to realloc even though count==1
    assertx(v1->hasExactlyOneRef());
    v1->release();
  }
  return ret;
}

StringData* concat_s4(StringData* v1, StringData* v2,
                      StringData* v3, StringData* v4) {
  if (v1->cowCheck()) {
    auto s1 = v1->slice();
    auto s2 = v2->slice();
    auto s3 = v3->slice();
    auto s4 = v4->slice();
    USDT(hhvm, hhvm_cow_concat, s1.size(), s2.size() + s3.size() + s4.size());
    StringData* ret = StringData::Make(s1, s2, s3, s4);
    // Because v1 was shared, we know this won't release it.
    v1->decRefCount();
    return ret;
  }

  UNUSED auto const lsize = v1->size();
  USDT(hhvm, hhvm_mut_concat, lsize, v2->size() + v3->size() + v4->size());
  auto const ret = v1->append(v2->slice(), v3->slice(), v4->slice());
  if (UNLIKELY(ret != v1)) {
    // had to realloc even though count==1
    assertx(v1->hasExactlyOneRef());
    v1->release();
  }
  return ret;
}

void raiseWarning(const StringData* sd) {
  raise_warning("%s", sd->data());
}

void raiseNotice(uint32_t sampleRate, const StringData* sd) {
  if (folly::Random::oneIn(sampleRate, threadLocalRng64())) {
    raise_notice("%s", sd->data());
  }
}

void throwArrayIndexException(const ArrayData* ad, const int64_t index) {
  throwOOBArrayKeyException(index, ad);
}

void throwArrayKeyException(const ArrayData* ad, const StringData* key) {
  assertx(ad->isDictType());
  throwOOBArrayKeyException(key, ad);
}

void throwMustBeEnclosedInReadonlyException(const Class* cls, const StringData* propName) {
  throw_must_be_enclosed_in_readonly(cls->name()->data(), propName->data());
}

void throwMustBeReadonlyException(const Class* cls, const StringData* propName) {
  throw_must_be_readonly(cls->name()->data(), propName->data());
}

void throwMustBeMutableException(const Class* cls, const StringData* propName) {
  throw_must_be_mutable(cls->name()->data(), propName->data());
}

void throwOrWarnLocalMustBeValueTypeException(const StringData* locName) {
  throw_local_must_be_value_type(locName->data());
}

void throwMustBeValueTypeException(const Class* cls, const StringData* propName) {
  throw_must_be_value_type(cls->name()->data(), propName->data());
}

std::string formatParamInOutMismatch(const char* fname, uint32_t index,
                                   bool funcByRef) {
  if (funcByRef) {
    return fmt::format(
      "{}() expects parameter {} to be inout, but the call was "
      "not annotated with 'inout'", fname, index + 1
    );
  } else {
    return fmt::format(
      "{}() does not expect parameter {} to be inout, but the call was "
      "annotated with 'inout'", fname, index + 1
    );
  }
}

void throwParamInOutMismatch(const Func* func, uint32_t index) {
  SystemLib::throwInvalidArgumentExceptionObject(formatParamInOutMismatch(
    func->fullName()->data(), index, func->isInOut(index)));
}

void checkInOutMismatch(const Func* func, uint32_t numArgs,
                        const uint8_t* inoutArgs) {
  assertx(numArgs == 0 || inoutArgs != nullptr);
  uint8_t tmp = 0;
  for (auto i = 0; i < numArgs; ++i) {
    tmp = (i % 8) == 0 ? *(inoutArgs++) : tmp >> 1;
    if (func->isInOut(i) != (tmp & 1)) throwParamInOutMismatch(func, i);
  }
}

std::string formatParamReadonlyMismatch(const char* fname, uint32_t index) {
  return fmt::format(
    "{}() does not allow for parameter {} to be passed readonly, but the argument "
    "was annotated with 'readonly'", fname, index + 1
  );
}

std::string formatReturnReadonlyMismatch(const char* fname) {
  return fmt::format(
    "{}() returns readonly, but the caller expects a mutable value",
    fname
  );
}

std::string formatReadonlyThisMismatch(const char* fname) {
  return fmt::format(
    "{}() modifies the instance, but the caller has a readonly instance",
    fname
  );
}

void throwReadonlyMismatch(const Func* func, int32_t index) {
  auto const msg = [&] {
    if (index == kReadonlyReturnId) {
      return formatReturnReadonlyMismatch(func->fullName()->data());
    }
    if (index == kReadonlyThisId) {
      return formatReadonlyThisMismatch(func->fullName()->data());
    }
    return formatParamReadonlyMismatch(func->fullName()->data(), index);
  }();
  SystemLib::throwReadonlyViolationExceptionObject(msg);
}

Optional<uint32_t> readonlyMismatch(const Func* func, uint32_t numArgs,
                                    const uint8_t* readonlyArgs,
                                    const ArrayData* namedArgNames) {
  assertx(numArgs == 0 || readonlyArgs != nullptr);
  auto const numNamedArgs = namedArgNames ? namedArgNames->size() : 0;
  uint8_t tmp = 0;
  for (auto i = 0u; i < numArgs; ++i) {
    tmp = (i % 8) == 0 ? *(readonlyArgs++) : tmp >> 1;
    if (!(tmp & 1)) continue;
    uint32_t param;
    if (i < numNamedArgs) {
      param = func->lookupVarId(namedArgNames->at(i).val().pstr);
      // Invalid names are rejected by the named-argument checks.
      if (param >= func->numNamedParams()) continue;
    } else {
      param = func->numNamedParams() + i - numNamedArgs;
    }
    if (!func->isReadonly(param)) return i;
  }
  return std::nullopt;
}

void checkReadonlyMismatch(const Func* func, uint32_t numArgs,
                           const uint8_t* readonlyArgs,
                           const ArrayData* namedArgNames) {
  if (auto const mismatch =
        readonlyMismatch(func, numArgs, readonlyArgs, namedArgNames)) {
    throwReadonlyMismatch(func, *mismatch);
  }
}

void throwInvalidUnpackArgs() {
  SystemLib::throwInvalidArgumentExceptionObject(
    "Only containers may be unpacked");
}

namespace {

std::string formatArgumentErrMsg(const Func* func, const char* amount,
                                 uint32_t expected, uint32_t got) {
  return fmt::format(
    "{}() expects {} {} positional parameter{}, {} given",
    func->fullNameWithClosureName(),
    amount,
    expected,
    expected == 1 ? "" : "s",
    got
  );
}

}

void throwMissingPositionalArgument(const Func* func, int got) {
  auto const expected = func->numRequiredPositionalParams();
  assertx(got < expected);
  auto const amount = expected < func->numParams() - func->numNamedParams() ? "at least" : "exactly";
  auto const errMsg = formatArgumentErrMsg(func, amount, expected, got);
  SystemLib::throwRuntimeExceptionObject(Variant(errMsg));
}

void throwMissingNamedParam(const Func* func, const StringData* paramName) {
  auto const errMsg = fmt::format(
    "Call to {}() has no argument matching required named parameter {}",
    func->fullNameWithClosureName(),
    paramName->slice()
  );

  SystemLib::throwRuntimeExceptionObject(Variant(errMsg));
}

void throwUnexpectedNamedArguments(const Func* func) {
  assertx(func->numNamedParams() == 0);
  auto const errMsg = fmt::format(
    "{}() has no named parameters but was called with named arguments.",
    func->fullNameWithClosureName());

  SystemLib::throwRuntimeExceptionObject(Variant(errMsg));
}

void throwNamedArgumentNameMismatch(const Func* func, const StringData* argName) {
  auto const errMsg = fmt::format(
    "Named argument {} does not match any named parameter in call to {}",
    argName->toCppString(),
    func->fullNameWithClosureName()
  );
  SystemLib::throwRuntimeExceptionObject(Variant(errMsg));
}

void raiseTooManyArguments(const Func* func, int got) {
  assertx(!func->hasVariadicCaptureParam());

  if (!Cfg::Eval::WarnOnTooManyArguments && !func->isCPPBuiltin()) {
    return;
  }
  auto const total = func->numPositionalParams();
  assertx(got > total);
  auto const amount = func->numRequiredPositionalParams() < total ? "at most" : "exactly";
  auto const errMsg = formatArgumentErrMsg(func, amount, total, got);

  if (Cfg::Eval::WarnOnTooManyArguments > 1 || func->isCPPBuiltin()) {
    // TODO(named_params): Once we have named param support, this should always
    // throw.
    SystemLib::throwRuntimeExceptionObject(Variant(errMsg));
  } else {
    raise_warning(errMsg);
  }
}

void raiseTooManyArgumentsPrologue(const Func* func, ArrayData* unpackArgs) {
  SCOPE_EXIT { decRefArr(unpackArgs); };
  if (unpackArgs->empty()) return;
  auto const got = func->numPositionalParams() + unpackArgs->size();
  raiseTooManyArguments(func, got);
}

//////////////////////////////////////////////////////////////////////

RDS_LOCAL_NO_CHECK(uint64_t, rl_num_coeffect_violations){0};

void raiseCoeffectsCallViolation(const Func* callee,
                                 RuntimeCoeffects provided,
                                 RuntimeCoeffects required) {
  assertx(CoeffectsConfig::enabled());
  auto const callerName = [&] {
    VMRegAnchor _;
    if (!vmfp()) {
      // VM is entering to the first frame
      return OptString{makeStaticString("[vm-entry]")};
    }
    OptString result;
    walkStack([&] (const BTFrame& frm) {
      assertx(frm);
      auto const func = frm.func();
      assertx(func);
      if (func->hasCoeffectRules() &&
          func->getCoeffectRules().size() == 1 &&
          func->getCoeffectRules()[0].isCaller()) {
        return false; // keep going
      }
      result = func->fullNameWithClosureName();
      return true;
    });
    assertx(!result.isNull());
    return result;
  };

  auto const errMsg = [&] {
    return fmt::format(
      "Call to {}() requires [{}] coeffects but {}() provided [{}]",
      callee->fullNameWithClosureName(),
      required.toString(),
      callerName(),
      provided.toString()
    );
  };

  FTRACE_MOD(Trace::coeffects, 1, "{}\n {:016b} -> {:016b}\n",
             errMsg(), provided.value(), required.value());

  assertx(!provided.canCall(required));
  if (provided.canCallWithWarning(required)) {
    if (*rl_num_coeffect_violations >= Cfg::Eval::CoeffectViolationWarningMax) {
      return;
    }
    (*rl_num_coeffect_violations)++;
    raise_warning(errMsg());
  } else {
    SystemLib::throwCoeffectViolationExceptionObject(errMsg());
  }
}

void raiseCoeffectsFunParamTypeViolation(TypedValue tv,
                                         int32_t paramIdx) {
  auto const errMsg =
    fmt::format("Coeffect rule requires parameter at position {} to be a "
                   "closure object, function/method pointer or null but "
                   "{} given",
                   paramIdx + 1, describe_actual_type(&tv));
  if (CoeffectsConfig::throws()) {
    SystemLib::throwInvalidArgumentExceptionObject(errMsg);
  }
  raise_warning(errMsg);
}

void raiseCoeffectsFunParamCoeffectRulesViolation(const Func* f) {
  assertx(f);
  auto const errMsg =
    fmt::format("Function/method pointer to {}() contains polymorphic "
                   "coeffects but is used as a coeffect rule",
                   f->fullNameWithClosureName());
  if (CoeffectsConfig::throws()) {
    SystemLib::throwInvalidArgumentExceptionObject(errMsg);
  }
  raise_warning(errMsg);
}

namespace {

void moduleBoundaryViolationImpl(
  std::string symbol,
  const StringData* symbolModule,
  const StringData* fromModule,
  bool soft
) {
  assertx(Cfg::Eval::EnforceModules);
  // Internal symbols must always have a module
  assertx(symbolModule != nullptr);
  assertx(fromModule != nullptr);
  assertx(symbolModule != fromModule);
  assertx(!symbol.empty());
  // Don't throw error in debugger when bypassCheck is on
  if (!g_context.isNull() && g_context->debuggerSettings.bypassCheck) return;
  auto const errMsg = fmt::format(
    "Accessing {}internal {} in module {} from {} is not allowed",
    soft ? "soft " : "",
    symbol,
    symbolModule->slice(),
    Module::isDefault(fromModule)
      ? "the default module"
      : fmt::format("module {}", fromModule->slice())
  );
  if (Cfg::Eval::EnforceModules > 1 && !soft) {
    SystemLib::throwModuleBoundaryViolationExceptionObject(errMsg);
  }
  raise_warning(errMsg);
}

} // namespace

void raiseModulePropertyViolation(
  const Class* cls,
  const StringData* prop,
  const StringData* callerModule,
  bool is_static = false
) {
  if (!Cfg::Eval::EnforceModules) return;
  assertx(cls);
  assertx(prop);
  auto const attrs = [&] {
    if (is_static) {
      auto const propSlot = cls->lookupSProp(prop);
      return cls->staticProperties()[propSlot].attrs;
    }
    auto const propSlot = cls->lookupDeclProp(prop);
    return cls->declProperties()[propSlot].attrs;
  }();
  assertx(attrs & AttrInternal);
  auto const soft = attrs & AttrInternalSoft;

  return moduleBoundaryViolationImpl(
    is_static
      ? fmt::format(
          "static property {}::${}", cls->name()->slice(), prop->data())
      : fmt::format("property {}::${}", cls->name()->slice(), prop->data()),
    cls->moduleName(),
    callerModule,
    soft
  );
}

void raiseModuleBoundaryViolation(const Class* ctx,
                                  const Func* callee,
                                  const StringData* callerModule) {
  if (!Cfg::Eval::EnforceModules) return;

  assertx(callee);
  assertx(IMPLIES(callee->isMethod(), ctx));
  assertx(callee->isInternal());
  return moduleBoundaryViolationImpl(
    ctx && callee->isMethod()
        ? fmt::format("method {}::{}", ctx->name()->slice(), callee->name()->slice())
        : fmt::format("function {}", callee->name()->slice()),
    callee->moduleName(),
    callerModule,
    callee->attrs() & AttrInternalSoft
  );
}

void raiseModuleBoundaryViolation(const Class* cls,
                                  const StringData* callerModule,
                                  const Class* callerCls) {
  if (!Cfg::Eval::EnforceModules) return;

  assertx(cls);
  assertx(cls->isInternal());
  if (canTestsBypassVisibility(cls->preClass()->userAttributes(),
                               callerCls)) return;
  auto const symbolType =
    isEnum(cls) ? "enum" : (isEnumClass(cls) ? "enum class" : "class");
  return moduleBoundaryViolationImpl(
    fmt::format("{} {}", symbolType, cls->name()->slice()),
    cls->moduleName(),
    callerModule,
    cls->attrs() & AttrInternalSoft
  );
}

const StaticString s___DynamicallyReferenced("__DynamicallyReferenced");

void raiseMissingDynamicallyReferenced(const Class* cls) {
  if (folly::Random::oneIn(Cfg::Eval::DynamicallyReferencedNoticeSampleRate, threadLocalRng64())) {
    auto const& attrs = cls->preClass()->userAttributes();
    auto const it = attrs.find(s___DynamicallyReferenced.get());
    if (it != attrs.end()) {
      assertx(tvIsVec(it->second));
      auto const args = val(it->second).parr;
      assertx(args->size() == 1);
      auto const rate = tvAssertInt(args->at(int64_t{0}));
      if (folly::Random::oneIn(rate, threadLocalRng64())) {
        raise_notice(Strings::SOFT_MISSING_DYNAMICALLY_REFERENCED,
                     cls->name()->data(),
                     rate);
      }
    } else {
      raise_notice(Strings::MISSING_DYNAMICALLY_REFERENCED, cls->name()->data());
    }
  }
}

NEVER_INLINE void checkStrictPackageDynamicReference(const Class* cls) {
  if (Cfg::Repo::Authoritative &&
      !Cfg::Eval::CanReportStrictDynamicReference) {
    return;
  }
  if (!cls->isInStrictPackage()) return;

  if (cls->preClass()->unit()
        ->shouldRaiseStrictPackageDynamicClassLoadError()) {
    std::string msg;
    string_printf(
      msg,
      Strings::STRICT_PACKAGE_DYNAMIC_REFERENCE,
      cls->name()->data()
    );
    throw_invalid_operation_exception(makeStaticString(msg));
  }

  if (Cfg::Eval::StrictPackageDynamicReferenceNoticeSampleRate > 0 &&
      folly::Random::oneIn(
        Cfg::Eval::StrictPackageDynamicReferenceNoticeSampleRate,
        threadLocalRng64()
      )) {
    raise_notice(
      Strings::STRICT_PACKAGE_DYNAMIC_REFERENCE,
      cls->name()->data()
    );
  }
}

//////////////////////////////////////////////////////////////////////

}
