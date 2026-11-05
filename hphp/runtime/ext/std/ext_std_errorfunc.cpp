/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
   | Copyright (c) 1997-2010 The PHP Group                                |
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
#include "hphp/runtime/ext/std/ext_std_errorfunc.h"

#include <iostream>

#include <fmt/core.h>
#include <folly/Likely.h>
#include <folly/Random.h>

#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/base/array-iterator.h"
#include "hphp/runtime/base/backtrace.h"
#include "hphp/runtime/base/bespoke-runtime.h"
#include "hphp/runtime/base/builtin-functions.h"
#include "hphp/runtime/base/exceptions.h"
#include "hphp/runtime/base/execution-context.h"
#include "hphp/runtime/base/string-buffer.h"
#include "hphp/runtime/base/request-info.h"
#include "hphp/runtime/ext/std/ext_std_file.h"
#include "hphp/util/logger.h"
#include "hphp/util/random.h"

namespace HPHP {

///////////////////////////////////////////////////////////////////////////////

const StaticString
  s_file("file"),
  s_line("line"),
  s_function("function"),
  s_class("class"),
  s_type("type"),
  s_args("args"),
  s_message("message"),
  s_call_user_func("call_user_func"),
  s_call_user_func_array("call_user_func_array"),
  s_hphp_debug_caller_info("hphp_debug_caller_info");

///////////////////////////////////////////////////////////////////////////////

const int64_t k_DEBUG_BACKTRACE_PROVIDE_OBJECT = (1 << 0);
const int64_t k_DEBUG_BACKTRACE_IGNORE_ARGS = (1 << 1);
const int64_t k_DEBUG_BACKTRACE_PROVIDE_METADATA = (1 << 16);
const int64_t k_DEBUG_BACKTRACE_ONLY_METADATA_FRAMES = (1 << 17);
const int64_t k_DEBUG_BACKTRACE_SKIP_INLINED = (1 << 18);
const int64_t k_DEBUG_BACKTRACE_INCLUDE_ATTRIBUTES = (1 << 19);

const int64_t k_DEBUG_BACKTRACE_HASH_CONSIDER_METADATA = (1 << 0);


Array HHVM_FUNCTION(debug_backtrace, int64_t options /* = 1 */,
                                     int64_t limit /* = 0 */) {
  bool provide_object = options & k_DEBUG_BACKTRACE_PROVIDE_OBJECT;
  bool provide_metadata = options & k_DEBUG_BACKTRACE_PROVIDE_METADATA;
  bool only_metadata_frames = options & k_DEBUG_BACKTRACE_ONLY_METADATA_FRAMES;
  bool ignore_args = options & k_DEBUG_BACKTRACE_IGNORE_ARGS;
  bool skip_inlined = options & k_DEBUG_BACKTRACE_SKIP_INLINED;
  bool incl_attrs = options & k_DEBUG_BACKTRACE_INCLUDE_ATTRIBUTES;
  return createBacktrace(BacktraceArgs()
                         .withThis(provide_object)
                         .withMetadata(provide_metadata)
                         .skipTop(skip_inlined)
                         .skipInlined(skip_inlined)
                         .onlyMetadataFrames(only_metadata_frames)
                         .ignoreArgs(ignore_args)
                         .setLimit(limit)
                         .includeAttrs(incl_attrs));
}

ArrayData* debug_backtrace_jit(int64_t options) {
  return HHVM_FN(debug_backtrace)(options).detach();
}

namespace {

constexpr auto s_file_idx = 0;
constexpr auto s_line_idx = 1;
constexpr auto s_class_idx = 2;
constexpr auto s_function_idx = 3;

RuntimeStruct* caller_info_struct() {
  static const RuntimeStruct::FieldIndexVector s_fields = {
    {s_file_idx, s_file},
    {s_line_idx, s_line},
    {s_class_idx, s_class},
    {s_function_idx, s_function},
  };
  static auto const s_struct =
    RuntimeStruct::registerRuntimeStruct(s_hphp_debug_caller_info, s_fields);
  return s_struct;
}

// Returned when the callee is the bottom frame, so the result always has
// 'file', 'function' and 'line'.
Array caller_info_placeholder() {
  StructDictInit init(caller_info_struct(), 3);
  init.set(s_file_idx, s_file, empty_string_variant());
  init.set(s_function_idx, s_function, empty_string_variant());
  init.set(s_line_idx, s_line, 0);
  return init.toArray();
}

}

bool hphp_debug_caller_info_impl(
    Array& result, bool& skipped, const Func* func, Offset offset) {
  if (!skipped && func->isSkipFrame()) return false;
  if (!skipped) {
    skipped = true;
    return false;
  }

  if (func->name() == s_call_user_func.get()) return false;
  if (func->name() == s_call_user_func_array.get()) return false;

  auto const line = func->getLineNumber(offset);
  if (line == -1) return false;

  auto const s_struct = caller_info_struct();

  auto const cls = func->cls();
  auto const path = func->originalUnit() ?
    func->originalUnit() : func->unit()->filepath();

  auto const has_cls = cls && !func->isClosureBody();
  StructDictInit init(s_struct, has_cls ? 4 : 3);
  if (has_cls) {
    init.set(s_class_idx, s_class,
             Variant(VarNR(const_cast<StringData*>(cls->name()))));
  }
  init.set(s_file_idx, s_file,
           Variant(VarNR(const_cast<StringData*>(path))));
  init.set(s_function_idx, s_function, func->nameWithClosureName());
  init.set(s_line_idx, s_line, line);
  result = init.toArray();
  return true;
}

/**
 * hphp_debug_caller_info - returns an array of info about the "caller"
 *
 * For clarity, we refer to the function that called hphp_debug_caller_info()
 * as the "callee", and we refer to the function that called the callee as
 * the "caller".
 *
 * This function returns an array containing keys "file", "function", "line" and
 * optionally "class" which indicate the filename, function, line number and
 * class name (if in class context) where the "caller" called the "callee".
 * If the callee is the bottom frame (a sync entry point, a shutdown function,
 * an exception handler), there is no caller: "file" and "function" are empty
 * and "line" is 0.
 */
ArrayRet HHVM_FUNCTION(hphp_debug_caller_info) {
  Array result;
  bool skipped = false;
  walkStack([&] (const BTFrame& frm) {
    return hphp_debug_caller_info_impl(
        result, skipped, frm.func(), frm.bcOff());
  });
  return result.isNull() ? caller_info_placeholder() : result;
}


bool hphp_debug_caller_identifier_impl(
    OptString& result, bool& skipped, const Func* func) {
  if (!skipped && func->isSkipFrame()) return false;
  if (!skipped) {
    skipped = true;
    return false;
  }

  if (func->name()->fsame(s_call_user_func.get())) return false;
  if (func->name()->fsame(s_call_user_func_array.get())) return false;

  result = func->fullNameWithClosureName();
  return true;
}

/**
 * hphp_debug_caller_identifier - returns the full function name of
 * the "caller"
 *
 * For clarity, we refer to the function that called
 * hphp_debug_caller_identifier() as the "callee", and we refer to the
 * function that called the callee as the "caller".
 */
OptString HHVM_FUNCTION(hphp_debug_caller_identifier) {
  OptString result = String::Empty();
  bool skipped = false;
  walkStack([&] (const BTFrame& frm) {
    return hphp_debug_caller_identifier_impl(
        result, skipped, frm.func());
  });
  return result;
}

int64_t HHVM_FUNCTION(hphp_debug_backtrace_hash, int64_t options /* = 0 */) {
  return createBacktraceHash(
    options & k_DEBUG_BACKTRACE_HASH_CONSIDER_METADATA
  );
}

void HHVM_FUNCTION(debug_print_backtrace, int64_t options /* = 0 */,
                                          int64_t limit /* = 0 */) {
  bool ignore_args = options & k_DEBUG_BACKTRACE_IGNORE_ARGS;
  g_context->write(debug_string_backtrace(false, ignore_args, limit));
}

OptString stringify_backtrace(const Array& bt, bool ignore_args) {
  StringBuffer buf;
  int i = 0;
  for (ArrayIter it = bt.begin(); !it.end(); it.next(), i++) {
    Array frame = it.second().toArray();
    buf.append('#');
    buf.append(i);
    if (i < 10) buf.append(' ');
    buf.append(' ');
    if (frame.exists(s_class)) {
      buf.append(tvCastToString(frame->get(s_class)));
      buf.append(tvCastToString(frame->get(s_type)));
    }
    buf.append(tvCastToString(frame->get(s_function)));
    buf.append("(");
    if (!ignore_args) {
      bool first = true;
      for (ArrayIter argsIt(tvCastToArrayLike(frame->get(s_args)));
          !argsIt.end();
          argsIt.next()) {
        if (!first) {
          buf.append(", ");
        } else {
          first = false;
        }
        try {
          buf.append(argsIt.second().toString());
        } catch (FatalErrorException& fe) {
          buf.append(fe.getMessage());
        }
      }
    }
    buf.append(")");
    if (frame.exists(s_file)) {
      buf.append(" called at [");
      buf.append(tvCastToString(frame->get(s_file)));
      buf.append(':');
      buf.append(tvCastToString(frame->get(s_line)));
      buf.append(']');
    }
    buf.append('\n');
  }
  return buf.detach();
}

OptString debug_string_backtrace(bool skip, bool ignore_args /* = false */,
                                 int64_t limit /* = 0 */) {
  Array bt;
  bt = createBacktrace(BacktraceArgs()
                       .skipTop(skip)
                       .ignoreArgs(ignore_args)
                       .setLimit(limit));
  return stringify_backtrace(bt, ignore_args);
}

Array HHVM_FUNCTION(error_get_last) {
  OptString lastError = g_context->getLastError();
  if (lastError.isNull()) {
    return null_array;
  }
  return make_dict_array(
    s_type, g_context->getLastErrorNumber(),
    s_message, g_context->getLastError(),
    s_file, g_context->getLastErrorPath(),
    s_line, g_context->getLastErrorLine()
  );
}

bool HHVM_FUNCTION(error_log, const OptString& message, int64_t message_type /* = 0 */,
                   const Variant& destination /* = null */,
                   const Variant& /*extra_headers*/ /* = null */) {
  // error_log() should not invoke the user error handler,
  // so we use Logger::Error() instead of raise_warning() or raise_error()
  switch (message_type) {
  case 0:
  {
    std::string line(message.data(),
                     // Truncate to 512k
                     message.size() > (1<<19) ? (1<<19) : message.size());
    Logger::Error(line);
    return true;
  }
  case 3:
  {
    // open for append only
    auto outfile = HHVM_FN(fopen)(destination.toString(), "a");
    if (outfile.isNull()) {
      Logger::Error("can't open error_log file!\n");
      return false;
    }
    HHVM_FN(fwrite)(outfile.toResource(), message);
    HHVM_FN(fclose)(outfile.toResource());
    return true;
  }
  case 2: // not used per PHP
  default:
    Logger::Error("error_log does not support message_type %ld!", message_type);
    break;
  }
  return false;
}

int64_t HHVM_FUNCTION(error_reporting, const Variant& level /* = null */) {
  auto& id = RequestInfo::s_requestInfo.getNoCheck()->m_reqInjectionData;
  int oldErrorReportingLevel = id.getErrorReportingLevel();
  if (!level.isNull()) {
    id.setErrorReportingLevel((int)level.toInt64());
  }
  return oldErrorReportingLevel;
}

bool HHVM_FUNCTION(restore_error_handler) {
  g_context->popUserErrorHandler();
  return true;
}

bool HHVM_FUNCTION(restore_exception_handler) {
  g_context->popUserExceptionHandler();
  return true;
}

Variant HHVM_FUNCTION(set_error_handler, const Variant& error_handler,
                      int64_t error_types /* = ErrorMode::PHP_ALL | STRICT */) {
  if (!is_null(error_handler.asTypedValue())) {
    return g_context->pushUserErrorHandler(error_handler, error_types);
  } else {
    g_context->clearUserErrorHandlers();
    return init_null_variant;
  }
}

Variant HHVM_FUNCTION(set_exception_handler, const Variant& exception_handler) {
  return g_context->pushUserExceptionHandler(exception_handler);
}

void HHVM_FUNCTION(hphp_set_error_page, const OptString& page) {
  g_context->setErrorPage(page);
}

void HHVM_FUNCTION(hphp_throw_fatal_error, const OptString& error_msg) {
  std::string msg = error_msg.data();
  raise_error(msg);
}

void HHVM_FUNCTION(hphp_clear_unflushed) {
  g_context->obEndAll();
  g_context->obStart();
  g_context->obProtect(true);
}

bool HHVM_FUNCTION(trigger_error, const OptString& error_msg,
                   int64_t error_type /* = ErrorMode::USER_NOTICE */) {
  std::string msg = error_msg.data(); // not toCppString()
  if (UNLIKELY(g_context->getThrowAllErrors())) {
    throw Exception(fmt::format("throwAllErrors: {}", error_type));
  }
  if (error_type == (int)ErrorMode::USER_ERROR) {
    g_context->handleError(msg, error_type, true,
                           ExecutionContext::ErrorThrowMode::IfUnhandled,
                           "\nFatal error: ");
    return true;
  }
  if (error_type == (int)ErrorMode::USER_WARNING) {
    g_context->handleError(msg, error_type, true,
                           ExecutionContext::ErrorThrowMode::Never,
                           "\nWarning: ");
    return true;
  }
  if (error_type == (int)ErrorMode::USER_NOTICE) {
    g_context->handleError(msg, error_type, true,
                           ExecutionContext::ErrorThrowMode::Never,
                           "\nNotice: ");
    return true;
  }
  if (error_type == (int)ErrorMode::USER_DEPRECATED) {
    g_context->handleError(msg, error_type, true,
                           ExecutionContext::ErrorThrowMode::Never,
                           "\nDeprecated: ");
    return true;
  }
  if (error_type == (int)ErrorMode::STRICT) {
    // So that we can raise strict warnings for mismatched
    // params in FCallBuiltin
    raise_strict_warning(msg);
    return true;
  }

  auto const f = fromCaller(
    [] (const BTFrame& frm) { return frm.func(); }
  );

  if (f && f->isBuiltin()) {
    if (error_type == (int)ErrorMode::ERROR) {
      raise_error_without_first_frame(msg);
      return true;
    }
    if (error_type == (int)ErrorMode::WARNING) {
      raise_warning_without_first_frame(msg);
      return true;
    }
    if (error_type == (int)ErrorMode::NOTICE) {
      raise_notice_without_first_frame(msg);
      return true;
    }
    if (error_type == (int)ErrorMode::PHP_DEPRECATED) {
      raise_deprecated_without_first_frame(msg);
      return true;
    }
    if (error_type == (int)ErrorMode::RECOVERABLE_ERROR) {
      raise_recoverable_error_without_first_frame(msg);
      return true;
    }
  }
  raise_warning("Invalid error type specified");
  return false;
}

bool HHVM_FUNCTION(trigger_sampled_error, const OptString& error_msg,
                   int64_t sample_rate,
                   int64_t error_type /* = (int)ErrorMode::USER_NOTICE */) {
  if (!folly::Random::oneIn(sample_rate, threadLocalRng64())) {
    return true;
  }
  return HHVM_FN(trigger_error)(error_msg, error_type);
}

bool HHVM_FUNCTION(user_error, const OptString& error_msg,
                   int64_t error_type /* = (int)ErrorMode::USER_NOTICE */) {
  return HHVM_FN(trigger_error)(error_msg, error_type);
}

///////////////////////////////////////////////////////////////////////////////

Array HHVM_FUNCTION(HH_deferred_errors) {
  return g_context->releaseDeferredErrors();
}

///////////////////////////////////////////////////////////////////////////////

void StandardExtension::registerNativeErrorFunc() {
  HHVM_FE(debug_backtrace);
  HHVM_FE(hphp_debug_caller_info);
  HHVM_FE(hphp_debug_caller_identifier);
  HHVM_FE(hphp_debug_backtrace_hash);
  HHVM_FE(debug_print_backtrace);
  HHVM_FE(error_get_last);
  HHVM_FE(error_log);
  HHVM_FE(error_reporting);
  HHVM_FE(restore_error_handler);
  HHVM_FE(restore_exception_handler);
  HHVM_FE(set_error_handler);
  HHVM_FE(set_exception_handler);
  HHVM_FE(hphp_set_error_page);
  HHVM_FE(hphp_throw_fatal_error);
  HHVM_FE(hphp_clear_unflushed);
  HHVM_FE(trigger_error);
  HHVM_FE(trigger_sampled_error);
  HHVM_FE(user_error);
  HHVM_FALIAS(HH\\deferred_errors, HH_deferred_errors);
  HHVM_RC_INT(DEBUG_BACKTRACE_PROVIDE_OBJECT, k_DEBUG_BACKTRACE_PROVIDE_OBJECT);
  HHVM_RC_INT(DEBUG_BACKTRACE_IGNORE_ARGS, k_DEBUG_BACKTRACE_IGNORE_ARGS);
  HHVM_RC_INT(DEBUG_BACKTRACE_PROVIDE_METADATA,
              k_DEBUG_BACKTRACE_PROVIDE_METADATA);
  HHVM_RC_INT(DEBUG_BACKTRACE_ONLY_METADATA_FRAMES,
              k_DEBUG_BACKTRACE_ONLY_METADATA_FRAMES);
  HHVM_RC_INT(DEBUG_BACKTRACE_HASH_CONSIDER_METADATA,
              k_DEBUG_BACKTRACE_HASH_CONSIDER_METADATA);
  HHVM_RC_INT(DEBUG_BACKTRACE_SKIP_INLINED,
              k_DEBUG_BACKTRACE_SKIP_INLINED);
  HHVM_RC_INT(DEBUG_BACKTRACE_INCLUDE_ATTRIBUTES,
              k_DEBUG_BACKTRACE_INCLUDE_ATTRIBUTES);
  HHVM_RC_INT(E_ERROR, (int)ErrorMode::ERROR);
  HHVM_RC_INT(E_WARNING, (int)ErrorMode::WARNING);
  HHVM_RC_INT(E_PARSE, (int)ErrorMode::PARSE);
  HHVM_RC_INT(E_NOTICE, (int)ErrorMode::NOTICE);
  HHVM_RC_INT(E_CORE_ERROR, (int)ErrorMode::CORE_ERROR);
  HHVM_RC_INT(E_CORE_WARNING, (int)ErrorMode::CORE_WARNING);
  HHVM_RC_INT(E_COMPILE_ERROR, (int)ErrorMode::COMPILE_ERROR);
  HHVM_RC_INT(E_COMPILE_WARNING, (int)ErrorMode::COMPILE_WARNING);
  HHVM_RC_INT(E_USER_ERROR, (int)ErrorMode::USER_ERROR);
  HHVM_RC_INT(E_USER_WARNING, (int)ErrorMode::USER_WARNING);
  HHVM_RC_INT(E_USER_NOTICE, (int)ErrorMode::USER_NOTICE);
  HHVM_RC_INT(E_STRICT, (int)ErrorMode::STRICT);
  HHVM_RC_INT(E_RECOVERABLE_ERROR, (int)ErrorMode::RECOVERABLE_ERROR);
  HHVM_RC_INT(E_DEPRECATED, (int)ErrorMode::PHP_DEPRECATED);
  HHVM_RC_INT(E_USER_DEPRECATED, (int)ErrorMode::USER_DEPRECATED);
  HHVM_RC_INT(E_ALL, (int)ErrorMode::PHP_ALL | (int)ErrorMode::STRICT);

  HHVM_RC_INT(E_HHVM_FATAL_ERROR, (int)ErrorMode::FATAL_ERROR);
}

}
