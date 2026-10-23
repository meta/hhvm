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

#include "hphp/runtime/base/runtime-error.h"
#include "hphp/runtime/base/builtin-functions.h"
#include "hphp/runtime/base/execution-context.h"
#include "hphp/runtime/base/runtime-option.h"
#include "hphp/runtime/base/request-info.h"
#include "hphp/runtime/vm/vm-regs.h"
#include "hphp/util/configs/errorhandling.h"
#include "hphp/util/configs/eval.h"
#include "hphp/util/configs/php7.h"
#include "hphp/util/random.h"
#include "hphp/util/string-vsnprintf.h"

#include <fmt/format.h>
#include <folly/Random.h>
#include <folly/logging/RateLimiter.h>
#include <folly/Range.h>

#ifdef ERROR
# undef ERROR
#endif
#ifdef STRICT
# undef STRICT
#endif

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

/*
 * Careful in these functions: they can be called when regState() is
 * DIRTY.  ExecutionContext::handleError is dirty-reg safe, but
 * evaluate other functions that you might need here.
 */

void raise_error(const std::string &msg) {
  g_context->handleError(msg, static_cast<int>(ErrorMode::ERROR), false,
                         ExecutionContext::ErrorThrowMode::Always,
                         "\nFatal error: ",
                         false);
  always_assert(0);
}

void raise_error(const char *fmt, ...) {
  std::string msg;
  va_list ap;
  va_start(ap, fmt);
  string_vsnprintf(msg, fmt, ap);
  va_end(ap);
  raise_error(msg);
}

void raise_error_without_first_frame(const std::string &msg) {
  g_context->handleError(msg, static_cast<int>(ErrorMode::ERROR), false,
                         ExecutionContext::ErrorThrowMode::Always,
                         "\nFatal error: ",
                         true);
  always_assert(0);
}

void raise_recoverable_error(const std::string &msg) {
  g_context->handleError(
    msg, static_cast<int>(ErrorMode::RECOVERABLE_ERROR), true,
    ExecutionContext::ErrorThrowMode::IfUnhandled,
    "\nCatchable Fatal error: ",
    false);
}

void raise_recoverable_error_without_first_frame(const std::string &msg) {
  g_context->handleError(
    msg, static_cast<int>(ErrorMode::RECOVERABLE_ERROR), true,
    ExecutionContext::ErrorThrowMode::IfUnhandled,
    "\nCatchable Fatal error: ",
    true);
}

void raise_typehint_error(const std::string& msg) {
  if (Cfg::PHP7::EngineExceptions) {
    VMRegAnchor _;
    SystemLib::throwTypeErrorObject(msg);
  }
  raise_recoverable_error(msg);
  raise_error("Error handler tried to recover from typehint violation");
}

void raise_typehint_error_without_first_frame(const std::string& msg) {
  if (Cfg::PHP7::EngineExceptions) {
    VMRegAnchor _;
    SystemLib::throwTypeErrorObject(msg);
  }
  raise_recoverable_error_without_first_frame(msg);
  raise_error("Error handler tried to recover from typehint violation");
}

void raise_reified_typehint_error(const std::string& msg, bool warn) {
  if (!warn) return raise_typehint_error_without_first_frame(msg);
  raise_warning_unsampled(msg);
}

void raise_inline_typehint_error(std::string& givenType, std::string& expectedType, std::string& errorKey) {
  std::string msg;
  if (errorKey.empty()) {
    msg = fmt::format("Runtime type-check: Expected {}, got {}", expectedType, givenType);
  } else {
    msg = fmt::format("Runtime type-check: Expected {} at {}, got {}",
      expectedType, errorKey, givenType);
  }
  if (Cfg::ErrorHandling::ThrowExceptionOnInlineTypeError) return raise_typehint_error(msg);
  raise_warning_unsampled(msg);
}

void raise_return_typehint_error(const std::string& msg) {
  if (Cfg::PHP7::EngineExceptions) {
    VMRegAnchor _;
    SystemLib::throwTypeErrorObject(msg);
  }
  raise_recoverable_error(msg);
  raise_error("Error handler tried to recover from a return typehint "
              "violation");
}

void raise_property_typehint_error(const std::string& msg,
                                   bool isSoft, bool isUB) {
  assertx(Cfg::Eval::CheckPropTypeHints > 0);

  if (Cfg::Eval::CheckPropTypeHints == 1 || isSoft) {
    raise_warning_unsampled(msg);
    return;
  }

  raise_recoverable_error(msg);
  if (Cfg::Eval::CheckPropTypeHints >= 3) {
    raise_error("Error handler tried to recover from a property typehint "
                "violation");
  }
}

void raise_property_typehint_unset_error(const Class* declCls,
                                         const StringData* propName,
                                         bool isSoft, bool isUB) {
  raise_property_typehint_error(
    fmt::format(
      "Unsetting property '{}::{}' with type annotation",
      declCls->name()->slice(),
      propName->slice()
    ),
    isSoft,
    isUB
  );
}

void raise_convert_object_to_string(const char* cls_name) {
  raise_error("Cannot convert object to string (got instance of %s)", cls_name);
}

void throw_convert_rfunc_to_type(const char* typeName) {
  SystemLib::throwInvalidOperationExceptionObject(
    fmt::format("Cannot convert reified function to {}", typeName)
  );
}

void throw_convert_rcls_meth_to_type(const char* typeName) {
  SystemLib::throwInvalidOperationExceptionObject(
    fmt::format("Cannot convert reified class method to {}", typeName)
  );
}

void throw_convert_ecl_to_type(const char* typeName) {
  SystemLib::throwInvalidOperationExceptionObject(
    fmt::format("Cannot convert enum class label to {}", typeName)
  );
}

void raise_use_of_specialized_array() {
  raise_error(Strings::DATATYPE_SPECIALIZED_DVARR);
}

void raise_hackarr_compat_notice(const std::string& msg) {
  raise_notice("Hack Array Compat: %s", msg.c_str());
}

void raise_hack_arr_compat_serialize_notice(const ArrayData* arr) {
  auto const type = [&]{
    if (arr->isVecType()) return "vec";
    if (arr->isDictType())     return "dict";
    if (arr->isKeysetType())   return "keyset";
    return "array";
  }();
  raise_notice("Hack Array Compat: Serializing %s", type);
}

namespace {

[[noreturn]]
void raise_func_undefined(const char* prefix, const StringData* name,
                          const Class* cls) {
  if (cls) {
    raise_error("%s undefined method %s::%s()", prefix, cls->name()->data(),
                name->data());
  }
  raise_error("%s undefined function %s()", prefix, name->data());
}

}

void raise_hackarr_compat_is_operator(const char* source, const char* target) {
  raise_notice(
    "Hack Array Compat: is/as operator used with %s and %s",
    source,
    target
  );
}

void raise_resolve_func_undefined(const StringData* name, const Class* cls) {
  raise_func_undefined("Failure to resolve", name, cls);
}

void raise_call_to_undefined(const StringData* name, const Class* cls) {
  raise_func_undefined("Call to", name, cls);
}

void raise_resolve_class_undefined(const StringData* name) {
  raise_error(Strings::FAILED_RESOLVE_CLASS, name->data());
}

void raise_recoverable_error(const char *fmt, ...) {
  std::string msg;
  va_list ap;
  va_start(ap, fmt);
  string_vsnprintf(msg, fmt, ap);
  va_end(ap);
  raise_recoverable_error(msg);
}

static int64_t g_notice_counter = 0;

static bool notice_freq_check(ErrorMode mode) {
  if (!g_context->getThrowAllErrors() &&
      (Cfg::ErrorHandling::NoticeFrequency <= 0 ||
       g_notice_counter++ % Cfg::ErrorHandling::NoticeFrequency != 0)) {
    return false;
  }
  return g_context->errorNeedsHandling(
    static_cast<int>(mode), true, ExecutionContext::ErrorThrowMode::Never);
}

#define HANDLE_ERROR(userHandle, throwMode, str, skip)                  \
  g_context->handleError(msg, static_cast<int>(mode), userHandle,       \
                         ExecutionContext::ErrorThrowMode::throwMode,   \
                         str,                                           \
                         skip);

static void raise_notice_helper(ErrorMode mode, bool skipTop,
                                const std::string& msg) {
  switch (mode) {
    case ErrorMode::STRICT:
      HANDLE_ERROR(true, Never, "\nStrict Warning: ", skipTop);
      break;
    case ErrorMode::NOTICE:
      HANDLE_ERROR(true, Never, "\nNotice: ", skipTop);
      break;
    case ErrorMode::PHP_DEPRECATED:
      HANDLE_ERROR(true, Never, "\nDeprecated: ", skipTop);
      break;
    default:
      always_assert(false && "Unhandled type of error");
  }
}

void raise_strict_warning(const std::string &msg) {
  if (notice_freq_check(ErrorMode::STRICT)) {
    raise_notice_helper(ErrorMode::STRICT, false, msg);
  }
}

void raise_strict_warning_without_first_frame(const std::string &msg) {
  if (notice_freq_check(ErrorMode::STRICT)) {
    raise_notice_helper(ErrorMode::STRICT, true, msg);
  }
}

void raise_strict_warning(const char *fmt, ...) {
  if (!notice_freq_check(ErrorMode::STRICT)) return;

  std::string msg;
  va_list ap;
  va_start(ap, fmt);
  string_vsnprintf(msg, fmt, ap);
  va_end(ap);
  raise_notice_helper(ErrorMode::STRICT, false, msg);
}

static int64_t g_warning_counter = 0;

bool warning_freq_check() {
  if (!g_context->getThrowAllErrors() &&
      (Cfg::ErrorHandling::WarningFrequency <= 0 ||
       g_warning_counter++ % Cfg::ErrorHandling::WarningFrequency != 0)) {
    return false;
  }
  return g_context->errorNeedsHandling(
    static_cast<int>(ErrorMode::WARNING), true,
    ExecutionContext::ErrorThrowMode::Never);
}

void raise_warning_helper(bool skipTop, const std::string& msg) {
  auto mode = ErrorMode::WARNING;
  HANDLE_ERROR(true, Never, "\nWarning: ", skipTop);
}

void raise_warning(const std::string &msg) {
  if (warning_freq_check()) {
    raise_warning_helper(false, msg);
  }
}

void raise_warning_without_first_frame(const std::string &msg) {
  if (warning_freq_check()) {
    raise_warning_helper(true, msg);
  }
}

void raise_warning(const char *fmt, ...) {
  if (!warning_freq_check()) return;
  std::string msg;
  va_list ap;
  va_start(ap, fmt);
  string_vsnprintf(msg, fmt, ap);
  va_end(ap);
  raise_warning_helper(false, msg);
}

/**
 * Warnings are currently sampled. raise_warning_unsampled can help when
 * migrating warnings to errors.
 *
 * In general, RaiseDebuggingFrequency should be kept at 1.
 */
static int64_t g_raise_warning_unsampled_counter = 0;

void raise_warning_unsampled(const std::string &msg) {
  if (RuntimeOption::RaiseDebuggingFrequency <= 0 ||
      (g_raise_warning_unsampled_counter++) %
      RuntimeOption::RaiseDebuggingFrequency != 0) {
    return;
  }
  if (g_context->errorNeedsHandling(
        static_cast<int>(ErrorMode::WARNING), true,
        ExecutionContext::ErrorThrowMode::Never)) {
    raise_warning_helper(false, msg);
  }
}

void raise_warning_unsampled(const char *fmt, ...) {
  std::string msg;
  va_list ap;
  va_start(ap, fmt);
  string_vsnprintf(msg, fmt, ap);
  va_end(ap);
  raise_warning_unsampled(msg);
}

void raise_notice(const std::string &msg) {
  if (notice_freq_check(ErrorMode::NOTICE)) {
    raise_notice_helper(ErrorMode::NOTICE, false, msg);
  }
}

void raise_notice_without_first_frame(const std::string &msg) {
  if (notice_freq_check(ErrorMode::NOTICE)) {
    raise_notice_helper(ErrorMode::NOTICE, true, msg);
  }
}

void raise_notice(const char *fmt, ...) {
  if (!notice_freq_check(ErrorMode::NOTICE)) return;
  std::string msg;
  va_list ap;
  va_start(ap, fmt);
  string_vsnprintf(msg, fmt, ap);
  va_end(ap);
  raise_notice_helper(ErrorMode::NOTICE, false, msg);
}

void raise_deprecated(const std::string &msg) {
  if (notice_freq_check(ErrorMode::PHP_DEPRECATED)) {
    raise_notice_helper(ErrorMode::PHP_DEPRECATED, false, msg);
  }
}

void raise_deprecated_without_first_frame(const std::string &msg) {
  if (notice_freq_check(ErrorMode::PHP_DEPRECATED)) {
    raise_notice_helper(ErrorMode::PHP_DEPRECATED, true, msg);
  }
}

void raise_deprecated(const char *fmt, ...) {
  if (!notice_freq_check(ErrorMode::PHP_DEPRECATED)) return;
  std::string msg;
  va_list ap;
  va_start(ap, fmt);
  string_vsnprintf(msg, fmt, ap);
  va_end(ap);
  raise_notice_helper(ErrorMode::PHP_DEPRECATED, false, msg);
}

std::string param_type_error_message(
    const char* func_name,
    int param_num,
    const char* expected_type,
    TypedValue actual_value) {

  // slice off fg1_
  if (strncmp(func_name, "fg1_", 4) == 0) {
    func_name += 4;
  } else if (strncmp(func_name, "tg1_", 4) == 0) {
    func_name += 4;
  }
  assertx(param_num > 0);

  auto const isLegacy =
    isArrayLikeType(type(actual_value)) &&
    val(actual_value).parr->isLegacyArray();
  return fmt::format(
    "{}() expects parameter {} to be {}, {} given",
    func_name,
    param_num,
    expected_type,
    getDataTypeString(type(actual_value), isLegacy).data());
}

void raise_param_type_warning(
    const char* func_name,
    int param_num,
    const char* expected_type,
    TypedValue actual_value) {

  // its ok to do this before munging, because it only looks at the
  // end of the string
  auto is_constructor = is_constructor_name(func_name);
  if (!is_constructor && !warning_freq_check()) return;

  auto msg = param_type_error_message(func_name,
                                      param_num,
                                      expected_type,
                                      actual_value);

  if (is_constructor) {
    SystemLib::throwExceptionObject(msg);
  }

  raise_warning_helper(false, msg);
}

void raise_message(ErrorMode mode,
                   const char *fmt,
                   va_list ap) {
  std::string msg;
  string_vsnprintf(msg, fmt, ap);
  raise_message(mode, false, msg);
}

void raise_message(ErrorMode mode,
                   const char *fmt,
                   ...) {
  std::string msg;
  va_list ap;
  va_start(ap, fmt);
  string_vsnprintf(msg, fmt, ap);
  va_end(ap);
  raise_message(mode, false, msg);
}

void raise_message(ErrorMode mode,
                   bool skipTop,
                   const std::string &msg) {
  if (mode == ErrorMode::ERROR) {
    HANDLE_ERROR(false, Always, "\nFatal error: ", skipTop);
    not_reached();
  }

  if (mode == ErrorMode::RECOVERABLE_ERROR) {
    HANDLE_ERROR(true, IfUnhandled, "\nCatchable fatal error: ", skipTop);
    return;
  }

  if (!g_context->errorNeedsHandling(static_cast<int>(mode), true,
                                     ExecutionContext::ErrorThrowMode::Never)) {
    return;
  }

  if (mode == ErrorMode::WARNING) {
    if (Cfg::ErrorHandling::WarningFrequency <= 0 ||
        (g_warning_counter++) % Cfg::ErrorHandling::WarningFrequency != 0) {
      return;
    }
    HANDLE_ERROR(true, Never, "\nWarning: ", skipTop);
    return;
  }

  if (Cfg::ErrorHandling::NoticeFrequency <= 0 ||
      (g_notice_counter++) % Cfg::ErrorHandling::NoticeFrequency != 0) {
    return;
  }

  raise_notice_helper(mode, skipTop, msg);
}

void raise_str_to_class_notice(const StringData* name, jit::StrToClassKind kind) {
  auto const source = [&] {
    switch (kind) {
      case jit::StrToClassKind::Expression:
        return "expression";
      case jit::StrToClassKind::StaticMethod:
        return "static method call";
      case jit::StrToClassKind::TypeStructure:
        return "type_structure()";
      case jit::StrToClassKind::DynamicClassMeth:
        return "dynamic_class_meth()";
    }
  }();
  if (folly::Random::oneIn(Cfg::Eval::RaiseStrToClsConversionNoticeSampleRate, threadLocalRng64())) {
    raise_notice("Implicit string to Class conversion for classname %s for %s",
                 name->data(), source);
  }
}

void raise_clsmeth_compat_type_hint(
  const Func* func, const std::string& displayName,
  Optional<int> param) {
  if (param) {
    raise_notice(
      "class_meth Compat: Argument %d passed to %s()"
      " must be of type %s, clsmeth given",
      *param + 1, func->fullName()->data(), displayName.c_str());
  } else {
    raise_notice(
      "class_meth Compat: Value returned from function %s()"
      " must be of type %s, clsmeth given",
      func->fullName()->data(), displayName.c_str());
  }
}

void raise_clsmeth_compat_type_hint_outparam_notice(
  const Func* func, const std::string& displayName, int paramNum) {
  raise_notice(
    "class_meth Compat: Argument %d returned from %s()"
    " must be of type %s, clsmeth given",
    paramNum + 1, func->fullName()->data(), displayName.c_str());
}

void raise_clsmeth_compat_type_hint_property_notice(
  const Class* declCls, const StringData* propName,
  const std::string& displayName, bool isStatic) {
  raise_notice(
    "class_meth Compat: %s '%s::%s' declared as type %s, clsmeth assigned",
    isStatic ? "Static property" : "Property",
    declCls->name()->data(), propName->data(), displayName.c_str());
}

///////////////////////////////////////////////////////////////////////////////

void raise_class_to_string_conversion_notice(const char* source) {
  if (LIKELY(!RID().getSuppressClassConversionNotices())) {
    raise_notice(Strings::CLASS_TO_STRING_IMPLICIT, source);
  }
}

SuppressClassConversionNotice::SuppressClassConversionNotice()
  : old(RID().getSuppressClassConversionNotices()) {
  RID().setSuppressClassConversionNotices(true);
}
SuppressClassConversionNotice::~SuppressClassConversionNotice() {
  RID().setSuppressClassConversionNotices(old);
}

void raise_class_to_memokey_conversion_warning() {
  raise_warning(Strings::CLASS_TO_MEMOKEY);
}

}
