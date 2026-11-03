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

#include "hphp/runtime/base/program-functions.h"

#include <atomic>
#include <exception>

#include "hphp/runtime/base/apc-typed-value.h"
#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/base/backtrace.h"
#include "hphp/runtime/base/bespoke-array.h"
#include "hphp/runtime/base/builtin-functions.h"
#include "hphp/runtime/base/code-coverage.h"
#include "hphp/runtime/base/config.h"
#include "hphp/runtime/base/exceptions.h"
#include "hphp/runtime/base/execution-context.h"
#include "hphp/runtime/base/extended-logger.h"
#include "hphp/runtime/base/file-util.h"
#include "hphp/runtime/base/file-util-defs.h"
#include "hphp/runtime/base/implicit-context.h"
#include "hphp/runtime/base/ini-setting.h"
#include "hphp/runtime/base/init-fini-node.h"
#include "hphp/runtime/base/member-reflection.h"
#include "hphp/runtime/base/memory-manager.h"
#include "hphp/runtime/base/perf-mem-event.h"
#include "hphp/runtime/base/php-globals.h"
#include "hphp/runtime/base/plain-file.h"
#include "hphp/runtime/base/request-id.h"
#include "hphp/runtime/base/runtime-error.h"
#include "hphp/runtime/base/runtime-option.h"
#include "hphp/runtime/base/stream-wrapper-registry.h"
#include "hphp/runtime/base/surprise-flags.h"
#include "hphp/runtime/base/thread-safe-setlocale.h"
#include "hphp/runtime/base/tracing.h"
#include "hphp/runtime/base/unit-cache.h"
#include "hphp/runtime/base/variable-serializer.h"
#include "hphp/runtime/base/verify-types.h"
#include "hphp/runtime/debugger/debugger.h"
#include "hphp/runtime/debugger/debugger_hook_handler.h"
#include "hphp/runtime/ext/extension-registry.h"
#include "hphp/runtime/ext/string/ext_string.h"
#include "hphp/runtime/ext/strobelight/ext_strobelight.h"
#include "hphp/runtime/ext/xenon/ext_xenon.h"
#include "hphp/runtime/server/admin-request-handler.h"
#include "hphp/runtime/server/cli-server.h"
#include "hphp/runtime/server/http-request-handler.h"
#include "hphp/runtime/server/http-server.h"
#include "hphp/runtime/server/pagelet-server.h"
#include "hphp/runtime/server/replay-transport.h"
#include "hphp/runtime/server/xbox-request-handler.h"
#include "hphp/runtime/server/server-note.h"
#include "hphp/runtime/server/server-stats.h"
#include "hphp/runtime/server/static-content-cache.h"
#include "hphp/runtime/server/warmup-request-handler.h"
#include "hphp/runtime/server/xbox-server.h"
#include "hphp/runtime/vm/debug/debug.h"
#include "hphp/runtime/vm/jit/cprof-consume.h"
#include "hphp/runtime/vm/jit/mcgen-async.h"
#include "hphp/runtime/vm/jit/mcgen-translate.h"
#include "hphp/runtime/vm/jit/mcgen.h"
#include "hphp/runtime/vm/jit/prof-data-serialize.h"
#include "hphp/runtime/vm/jit/prof-data.h"
#include "hphp/runtime/vm/jit/tc.h"
#include "hphp/runtime/vm/jit/translation-stats.h"
#include "hphp/runtime/vm/jit/translator.h"
#include "hphp/runtime/vm/repo-file.h"
#include "hphp/runtime/vm/repo-global-data.h"
#include "hphp/runtime/vm/runtime-compiler.h"
#include "hphp/runtime/vm/source-location.h"
#include "hphp/runtime/vm/treadmill.h"
#include "hphp/runtime/vm/unit-parser.h"

#include "hphp/util/alloc.h"
#include "hphp/util/assertions.h"
#include "hphp/util/arch.h"
#include "hphp/util/boot-stats.h"
#include "hphp/util/build-info.h"
#include "hphp/util/capability.h"
#include "hphp/util/configs/adminserver.h"
#include "hphp/util/configs/debug.h"
#include "hphp/util/configs/debugger.h"
#include "hphp/util/configs/errorhandling.h"
#include "hphp/util/configs/eval.h"
#include "hphp/util/configs/jit.h"
#include "hphp/util/configs/log.h"
#include "hphp/util/configs/server.h"
#include "hphp/util/configs/xenon.h"
#include "hphp/util/embedded-data.h"
#include "hphp/util/exception.h"
#include "hphp/util/hardware-counter.h"
#include "hphp/util/hugetlb.h"
#include "hphp/util/kernel-version.h"
#include "hphp/util/light-process.h"
#include "hphp/util/managed-arena.h"
#include "hphp/util/maphuge.h"
#include "hphp/util/perf-event.h"
#include "hphp/util/portability.h"
#include "hphp/util/process-exec.h"
#include "hphp/util/process.h"
#include "hphp/util/rds-local.h"
#include "hphp/util/roar.h"
#include "hphp/util/service-data.h"
#include "hphp/util/shm-counter.h"
#include "hphp/util/stack-trace.h"
#include "hphp/util/sync-signal.h"
#include "hphp/util/timer.h"
#include "hphp/util/type-scan.h"

#include "hphp/zend/zend-math.h"
#include "hphp/zend/zend-string.h"
#include "hphp/zend/zend-strtod.h"

#ifdef HHVM_FACEBOOK
#include "roarjit/Init.h"
#endif

#include <folly/CPortability.h>
#include <folly/json/json.h>
#include <folly/Portability.h>
#include <folly/Random.h>
#include <folly/Range.h>
#include <folly/Singleton.h>
#include <folly/portability/Fcntl.h>
#include <folly/portability/Libgen.h>
#include <folly/portability/Stdlib.h>
#include <folly/portability/Unistd.h>

#include <boost/program_options/options_description.hpp>
#include <boost/program_options/positional_options.hpp>
#include <boost/program_options/variables_map.hpp>

#ifndef HHVM_FACEBOOK
// Needed on libevent2
#include <event2/thread.h>
#endif

#include <oniguruma.h>
// Oniguruma defines UChar to unsigned char, but ICU4C defines it to signed
// 16-bit int. This is supposed to be resolved by ONIG_ESCAPE_UCHAR_COLLISION,
// however this isn't fully supported in 6.8.0 or 6.8.1.
//
// As of 2018-03-21, not in any release; hopefully will be in 6.8.2 - it's
// resolved by this commit:
//
// https://github.com/kkos/oniguruma/commit/e79406479b6be4a56e40ede6c1a87b51fba073a2
#undef UChar
#include <signal.h>

#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <fmt/format.h>

using namespace boost::program_options;
using std::cout;

constexpr auto MAX_INPUT_NESTING_LEVEL = 64;

namespace HPHP {

///////////////////////////////////////////////////////////////////////////////
// Forward declarations.

/*
 * XXX: VM process initialization is handled through a function
 * pointer so libhphp_runtime.a can be linked into programs that don't
 * actually initialize the VM.
 */
void (*g_vmProcessInit)();

void timezone_init();

void pcre_init();
void pcre_reinit();

// Overrides the weak definition in hphp/util/alloc.cpp, which cannot reach
// Logger from below it in the dependency graph. Outside server mode there is
// no error log yet, so leave those on stderr.
void alloc_warn(const std::string& msg) {
  if (Cfg::Server::Mode) {
    Logger::Warning(msg);
  } else {
    fprintf(stderr, "%s\n", msg.c_str());
  }
}

///////////////////////////////////////////////////////////////////////////////
// helpers

enum class ExecutionMode {
  RUN,
  DEBUG,
  NOTEBOOK,
  EVAL,
  TRANSLATE,
  SERVER,
  REPLAY,
  DAEMON,
  DUMPHHAS,
  VERIFY,
  VSDEBUG,
  GETOPTION,
  DUMPCOVERAGE,
  LIVE_TRANSLATE
};

void validate(boost::any& v, const std::vector<std::string>& values, ExecutionMode*, int) {
    // Make sure no previous assignment to 'v' was made.
    validators::check_first_occurrence(v);
    const std::string& s = validators::get_single_string(values);

    if (s == "run") {
        v = boost::any(ExecutionMode::RUN);
    } else if (s == "debug" || s == "d") {
        v = boost::any(ExecutionMode::DEBUG);
    } else if (s == "notebook") {
        v = boost::any(ExecutionMode::NOTEBOOK);
    } else if (s == "eval") {
        v = boost::any(ExecutionMode::EVAL);
    } else if (s == "translate" || s == "t") {
        v = boost::any(ExecutionMode::TRANSLATE);
    } else if (s == "server" || s == "s") {
        v = boost::any(ExecutionMode::SERVER);
    } else if (s == "replay") {
        v = boost::any(ExecutionMode::REPLAY);
    } else if (s == "daemon") {
        v = boost::any(ExecutionMode::DAEMON);
    } else if (s == "dumphhas") {
        v = boost::any(ExecutionMode::DUMPHHAS);
    } else if (s == "verify") {
        v = boost::any(ExecutionMode::VERIFY);
    } else if (s == "vsdebug") {
        v = boost::any(ExecutionMode::VSDEBUG);
    } else if (s == "getoption") {
        v = boost::any(ExecutionMode::GETOPTION);
    } else if (s == "dumpcoverage") {
        v = boost::any(ExecutionMode::DUMPCOVERAGE);
    } else if (s == "live") {
        v = boost::any(ExecutionMode::LIVE_TRANSLATE);
    } else {
        throw validation_error(validation_error::invalid_option_value);
    }
}

struct ProgramOptions {
  ExecutionMode mode;
  std::vector<std::string> config;
  std::vector<std::string> confStrings;
  std::vector<std::string> iniStrings;
  int port;
  int portfd;
  int sslportfd;
  int admin_port;
  std::string user;
  std::string file;
  std::string lint;
  bool isTempFile;
  int count;
  bool noSafeAccessCheck;
  std::vector<std::string> args;
  std::string buildId;
  std::string instanceId;
  std::string show;
  std::string parse;
  int vsDebugPort;
  std::string vsDebugDomainSocket;
  bool vsDebugNoWait;
  Eval::DebuggerClientOptions debugger_options;
};

struct StartTime {
  StartTime() : startTime(time(nullptr)) {}
  time_t startTime;
};

static bool registrationComplete = false;
static StartTime s_startTime;
static std::string tempFile;
std::vector<std::string> s_config_files;
std::vector<std::string> s_ini_strings;

time_t start_time() {
  return s_startTime.startTime;
}

const StaticString
  s_HPHP("HPHP"),
  s_HHVM("HHVM"),
  s_HHVM_JIT("HHVM_JIT"),
  s_HHVM_ARCH("HHVM_ARCH"),
  s_REQUEST_TIME_NS("REQUEST_TIME_NS"),
  s_REQUEST_TIME_MONOTONIC_NS("REQUEST_TIME_MONOTONIC_NS"),
  s_DOCUMENT_ROOT("DOCUMENT_ROOT"),
  s_SCRIPT_FILENAME("SCRIPT_FILENAME"),
  s_SCRIPT_NAME("SCRIPT_NAME"),
  s_PHP_SELF("PHP_SELF"),
  s_argc("argc"),
  s_argv("argv"),
  s_PWD("PWD"),
  s_HOSTNAME("HOSTNAME"),
  s__SERVER("_SERVER"),
  s__ENV("_ENV"),
  s_toStringErr("(unable to call toString())");

static RDS_LOCAL(bool, s_sessionInitialized);

static void process_cmd_arguments(int argc, char **argv) {
  php_global_set(s_argc, Variant(argc));
  VecInit argvArray(argc);
  for (int i = 0; i < argc; i++) {
    argvArray.append(OptString(argv[i]));
  }
  php_global_set(s_argv, argvArray.toArray());
}

static void process_env_variables(
  Array& variables,
  char** envp,
  const std::map<std::string, std::string>& envVariables
) {
  for (auto const& kv : envVariables) {
    OptString idx(kv.first);
    auto const arrkey = variables.convertKey<IntishCast::Cast>(idx);
    OptString str(kv.second);
    variables.set(arrkey, make_tv<KindOfString>(str.get()));
  }
  for (char **env = envp; env && *env; env++) {
    char *p = strchr(*env, '=');
    if (p) {
      OptString name(*env, p - *env, CopyString);
      register_variable(variables, (char*)name.data(),
                        OptString(p + 1, CopyString));
    }
  }
}

void process_env_variables(Array& variables) {
  process_env_variables(variables, environ, RuntimeOption::EnvVariables);
}

const StaticString
  s_HPHP_INPUT_PARSE_ERROR("HPHP_INPUT_PARSE_ERROR"),
  s_failed_append("failed_append");

// Handle adding a variable to an array, supporting keys that look
// like array expressions (like 'FOO[][key1][k2]').
void register_variable(Array& variables, char *name, const Variant& value,
                       bool overwrite /* = true */) {
  // ignore leading spaces in the variable name
  char *var = name;
  while (*var && *var == ' ') {
    var++;
  }

  // ensure that we don't have spaces or dots in the variable name
  // (not binary safe)
  bool is_array = false;
  char *ip = nullptr; // index pointer
  char *p = var;
  for (; *p; p++) {
    if (*p == ' ' || *p == '.') {
      *p = '_';
    } else if (*p == '[') {
      is_array = true;
      ip = p;
      *p = 0;
      break;
    }
  }
  int var_len = p - var;
  if (var_len == 0) {
    // empty variable name, or variable name with a space in it
    return;
  }

  // The array pointer we're currently adding to.  If we're doing a
  // multi-dimensional set. Note that as we're essentially using this as an
  // interior array pointer it's not safe to allow reentry here.
  Array* symtable = &variables;

  char* index = var;
  int index_len = var_len;

  if (is_array) {
    int nest_level = 0;
    while (true) {
      if (++nest_level > MAX_INPUT_NESTING_LEVEL) {
        Logger::Warning("Input variable nesting level exceeded");
        return;
      }

      ip++;
      char *index_s = ip;
      int new_idx_len = 0;
      if (isspace(*ip)) {
        ip++;
      }
      if (*ip == ']') {
        index_s = nullptr;
      } else {
        ip = strchr(ip, ']');
        if (!ip) {
          // PHP variables cannot contain '[' in their names,
          // so we replace the character with a '_'
          *(index_s - 1) = '_';

          index_len = 0;
          if (index) {
            index_len = strlen(index);
          }
          goto plain_var;
        }
        *ip = 0;
        new_idx_len = strlen(index_s);
      }

      if (!index) {
        auto const size = symtable->size();
        symtable->append(make_persistent_array_like_tv(staticEmptyDictArray()));
        if (symtable->size() == size) {
          ServerNote::Add(s_HPHP_INPUT_PARSE_ERROR, s_failed_append);
          return;
        }
        auto const key = symtable->get()->getKey(symtable->get()->iter_last());
        symtable = &asArrRef(symtable->lval(key));
      } else {
        OptString key_str(index, index_len, CopyString);
        auto const key =
          symtable->convertKey<IntishCast::Cast>(key_str.asTypedValue());
        auto const v = symtable->lookup(key);
        if (isNullType(v.type()) || !isArrayLikeType(v.type())) {
          symtable->set(key, make_persistent_array_like_tv(staticEmptyDictArray()));
        }
        symtable = &asArrRef(symtable->lval(key));
      }
      /* ip pointed to the '[' character, now obtain the key */
      index = index_s;
      index_len = new_idx_len;

      ip++;
      if (*ip == '[') {
        is_array = true;
        *ip = 0;
      } else {
        goto plain_var;
      }
    }
  } else {
  plain_var:
    if (!index) {
      symtable->append(value);
    } else {
      OptString key_str(index, index_len, CopyString);
      auto key =
        symtable->convertKey<IntishCast::Cast>(key_str.asTypedValue());
      if (overwrite || !symtable->exists(key)) {
        symtable->set(key, *value.asTypedValue(), true);
      }
    }
  }
}

enum class ContextOfException {
  ReqInit = 1,
  Invoke,
  Handler,
};

static void handle_exception_append_bt(std::string& errorMsg,
                                       const ExtendedException& e) {
  Array bt = e.getBacktrace();
  if (!bt.empty()) {
    errorMsg += ExtendedLogger::StringOfStackTrace(bt);
  }
}

void bump_counter_and_rethrow(bool isPsp, ExecutionContext* context) {
  try {
    throw;
  } catch (const RequestTimeoutException&) {
    if (isPsp) {
      static auto requestTimeoutPSPCounter = ServiceData::createTimeSeries(
        "requests_timed_out_psp", {ServiceData::StatsType::COUNT});
      requestTimeoutPSPCounter->addValue(1);
      ServerStats::Log("request.timed_out.psp", 1);
    } else {
      static auto requestTimeoutCounter = ServiceData::createTimeSeries(
        "requests_timed_out_non_psp", {ServiceData::StatsType::COUNT});
      requestTimeoutCounter->addValue(1);
      ServerStats::Log("request.timed_out.non_psp", 1);
    }
    context->markTimedOut();
    throw;
  } catch (const RequestCPUTimeoutException&) {
    if (isPsp) {
      static auto requestCPUTimeoutPSPCounter = ServiceData::createTimeSeries(
        "requests_cpu_timed_out_psp", {ServiceData::StatsType::COUNT});
      requestCPUTimeoutPSPCounter->addValue(1);
      ServerStats::Log("request.cpu_timed_out.psp", 1);
    } else {
      static auto requestCPUTimeoutCounter = ServiceData::createTimeSeries(
        "requests_cpu_timed_out_non_psp", {ServiceData::StatsType::COUNT});
      requestCPUTimeoutCounter->addValue(1);
      ServerStats::Log("request.cpu_timed_out.non_psp", 1);
    }
    context->markTimedOut();
    throw;
  } catch (const RequestMemoryExceededException&) {
    if (isPsp) {
      static auto requestMemoryExceededPSPCounter =
        ServiceData::createTimeSeries(
          "requests_memory_exceeded_psp", {ServiceData::StatsType::COUNT});
      requestMemoryExceededPSPCounter->addValue(1);
      ServerStats::Log("request.memory_exceeded.psp", 1);
    } else {
      static auto requestMemoryExceededCounter = ServiceData::createTimeSeries(
        "requests_memory_exceeded_non_psp", {ServiceData::StatsType::COUNT});
      requestMemoryExceededCounter->addValue(1);
      ServerStats::Log("request.memory_exceeded.non_psp", 1);
    }
    throw;
  } catch (const RequestOOMKilledException& e) {
    if (isPsp) {
      static auto requestHostOOMPSPCounter = ServiceData::createTimeSeries(
        "requests_oom_killed_psp", {ServiceData::StatsType::COUNT});
      requestHostOOMPSPCounter->addValue(1);
      ServerStats::Log("request.oom_killed.psp", 1);
    } else {
      static auto requestHostOOMCounter = ServiceData::createTimeSeries(
        "requests_oom_killed_non_psp", {ServiceData::StatsType::COUNT});
      requestHostOOMCounter->addValue(1);
      ServerStats::Log("request.oom_killed.non_psp", 1);
    }
    if (Cfg::GC::LogKilledRequests && StructuredLog::enabled()) {
      StructuredLogEntry entry;
      entry.setInt("mem_used", e.m_usedBytes);
      entry.setInt("is_psp", static_cast<int>(isPsp));
      entry.setStr("url", context->getRequestUrl());
      StructuredLog::log("hhvm_oom_killed", entry);
    }
    context->markOOMKilled();
    throw;
  }
}

static void handle_exception_helper(bool& ret,
                                    ExecutionContext* context,
                                    std::string& errorMsg,
                                    ContextOfException where,
                                    bool& error,
                                    bool richErrorMsg) {
  // Clear oom/timeout while handling exception and restore them afterwards.
  StackLimitAndSurpriseFlags::FlagSaver saver(
    stackLimitAndSurprise(),
    ResourceFlags
  );

  try {
    bump_counter_and_rethrow(false /* isPsp */, context);
  } catch (const Eval::DebuggerException&) {
    throw;
  } catch (const ExitException& e) {
    if (where == ContextOfException::ReqInit) {
      ret = false;
    } else if (where != ContextOfException::Handler &&
        !context->getExitCallback().isNull() &&
        is_callable(context->getExitCallback())) {
      Array stack = e.getBacktrace();
      Array argv = make_vec_array(*rl_exit_code, stack);
      vm_call_user_func(context->getExitCallback(), argv);
    }
  } catch (const PhpFileDoesNotExistException& e) {
    ret = false;
    if (where != ContextOfException::Handler) {
      raise_notice(e.getMessage());
    } else {
      Logger::Error(e.getMessage());
    }
    if (richErrorMsg) {
      handle_exception_append_bt(errorMsg, e);
    }
  } catch (const Exception& e) {
    bool oldRet = ret;
    bool origError = error;
    std::string origErrorMsg = errorMsg;
    ret = false;
    error = true;
    errorMsg = "";
    if (where == ContextOfException::Handler) {
      errorMsg = "Exception handler threw an exception: ";
    }
    errorMsg += e.what();
    if (where == ContextOfException::Invoke) {
      bool handlerRet = context->onFatalError(e);
      if (handlerRet) {
        ret = oldRet;
        error = origError;
        errorMsg = origErrorMsg;
      }
    } else {
      Logger::Error(errorMsg);
    }
    if (richErrorMsg) {
      auto const ee = dynamic_cast<const ExtendedException*>(&e);
      if (ee) {
        handle_exception_append_bt(errorMsg, *ee);
      }
    }
  } catch (const Object& e) {
    bool oldRet = ret;
    bool origError = error;
    auto const origErrorMsg = errorMsg;
    ret = false;
    error = true;
    errorMsg = "";
    if (where == ContextOfException::Handler) {
      errorMsg = "Exception handler threw an object exception: ";
    }
    try {
      errorMsg += throwable_to_string(e.get()).data();
    } catch (...) {
      errorMsg += s_toStringErr.data();
    }
    if (where == ContextOfException::Invoke) {
      bool handlerRet = context->onUnhandledException(e);
      if (handlerRet) {
        ret = oldRet;
        error = origError;
        errorMsg = origErrorMsg;
      }
    } else {
      Logger::Error(errorMsg);
    }
  } catch (const std::exception& e) {
    ret = false;
    error = true;
    errorMsg = "C++ exception: ";
    errorMsg += e.what();
    Logger::Error(errorMsg);
  } catch (...) {
    ret = false;
    error = true;
    errorMsg = "(non-standard exception \"";
    errorMsg += current_exception_name();
    errorMsg += "\" was thrown)";
    Logger::Error(errorMsg);
  }
}

static bool hphp_chdir_file(const std::string& filename) {
  bool ret = false;
  OptString s = File::TranslatePath(filename);
  char *buf = strndup(s.data(), s.size());
  char *dir = dirname(buf);
  assertx(dir);
  if (dir) {
    if (File::IsVirtualDirectory(dir)) {
      g_context->setCwd(OptString(dir, CopyString));
      ret = true;
    } else {
      struct stat sb;
      stat(dir, &sb);
      if ((sb.st_mode & S_IFMT) == S_IFDIR) {
        ret = true;
        if (*dir != '.') {
          g_context->setCwd(OptString(dir, CopyString));
        }
      }
    }
  }
  free(buf);
  return ret;
}

static void handle_resource_exceeded_exception() {
  try {
    throw;
  } catch (RequestTimeoutException&) {
    RID().triggerTimeout(TimeoutTime);
  } catch (RequestCPUTimeoutException&) {
    RID().triggerTimeout(TimeoutCPUTime);
  } catch (RequestMemoryExceededException&) {
    stackLimitAndSurprise().setFlag(MemExceededFlag);
    RID().setRequestOOMFlag();
  } catch (...) {}
}

void handle_destructor_exception(const char* situation) {
  std::string errorMsg;

  try {
    throw;
  } catch (ExitException& e) {
    // ExitException is fine, no need to show a warning.
    RI().setPendingException(e.clone());
    return;
  } catch (Object &e) {
    // For user exceptions, invoke the user exception handler
    errorMsg = situation;
    errorMsg += " threw an object exception: ";
    try {
      errorMsg += throwable_to_string(e.get()).data();
    } catch (...) {
      handle_resource_exceeded_exception();
      errorMsg += "(unable to call toString())";
    }
  } catch (Exception& e) {
    RI().setPendingException(e.clone());
    errorMsg = situation;
    errorMsg += " raised a fatal error: ";
    errorMsg += e.what();
  } catch (...) {
    errorMsg = situation;
    errorMsg += " threw an unknown exception";
  }
  // For fatal errors and unknown exceptions, we raise a warning.
  // If there is a user error handler it will be invoked, otherwise
  // the default error handler will be invoked.
  try {
    raise_warning_unsampled(errorMsg);
  } catch (...) {
    handle_resource_exceeded_exception();

    // The user error handler fataled or threw an exception,
    // print out the error message directly to the log
    Logger::Warning(errorMsg);
  }
}

void init_server_request_time(Array& server) {
  auto const nowRealtimeNs = gettime_ns(CLOCK_REALTIME);
  auto const nowMonotonicNs = gettime_ns(CLOCK_MONOTONIC);
  server.set(s_REQUEST_TIME_NS, nowRealtimeNs);
  server.set(s_REQUEST_TIME_MONOTONIC_NS, nowMonotonicNs);
}

static RDS_LOCAL(rqtrace::Trace, tl_cmdTrace);

void init_command_line_session(int argc, char** argv) {
  StackTraceNoHeap::AddExtraLogging("ThreadType", "CLI");
  std::string args;
  for (int i = 0; i < argc; i++) {
    if (i) args += " ";
    args += argv[i];
  }
  StackTraceNoHeap::AddExtraLogging("Arguments", args.c_str());

  hphp_session_init(Treadmill::SessionKind::CLISession);
  auto const context = g_context.getNoCheck();
  context->obSetImplicitFlush(true);
  if (Cfg::Eval::TraceCommandLineRequest) {
    tl_cmdTrace.destroy();
    context->setRequestTrace(tl_cmdTrace.getCheck());
  }
}

void
init_command_line_globals(
  int argc, char** argv, char** envp,
  const std::map<std::string, std::string>& serverVariables,
  const std::map<std::string, std::string>& envVariables
) {
  // Env
  {
    auto envArr = Array::CreateDict();
    process_env_variables(envArr, envp, envVariables);
    envArr.set(s_HPHP, 1);
    envArr.set(s_HHVM, 1);
    if (Cfg::Jit::Enabled) {
      envArr.set(s_HHVM_JIT, 1);
    }
    ARCH_MATCH(
      [&](arch::X64) {
        envArr.set(s_HHVM_ARCH, "x64");
      },
      [&](arch::ARM) {
        envArr.set(s_HHVM_ARCH, "arm");
      }
    );
    php_global_set(s__ENV, std::move(envArr));
  }

  process_cmd_arguments(argc, argv);

  // Server
  {
    auto serverArr = Array::CreateDict();
    process_env_variables(serverArr, envp, envVariables);
    OptString file = String::Empty();
    if (argc > 0) {
      file = OptString::attach(StringData::Make(argv[0], CopyString));
    }
    init_server_request_time(serverArr);
    serverArr.set(s_DOCUMENT_ROOT, empty_string_tv());
    serverArr.set(s_SCRIPT_FILENAME, file);
    serverArr.set(s_SCRIPT_NAME, file);
    serverArr.set(s_PHP_SELF, file);
    serverArr.set(s_argv, php_global(s_argv));
    serverArr.set(s_argc, php_global(s_argc));
    serverArr.set(s_PWD, g_context->getCwd());
    char hostname[1024];
    if (!is_any_cli_mode() &&
        !gethostname(hostname, sizeof(hostname))) {
      // gethostname may not null-terminate
      hostname[sizeof(hostname) - 1] = '\0';
      serverArr.set(s_HOSTNAME, OptString(hostname, CopyString));
    }

    for (auto const& kv : serverVariables) {
      serverArr.set(OptString{kv.first}, OptString{kv.second});
    }

    php_global_set(s__SERVER, std::move(serverArr));
  }

  if (Cfg::Server::RequestTimeoutSeconds) {
    RID().setTimeout(Cfg::Server::RequestTimeoutSeconds);
  }

  if (Cfg::Xenon::ForceAlwaysOn) {
    Xenon::getInstance().surpriseAll();
  }

  // Initialize the debugger
  DEBUGGER_ATTACHED_ONLY(phpDebuggerRequestInitHook());
}

void execute_command_line_begin(int argc, char **argv) {
  init_command_line_session(argc, argv);
  init_command_line_globals(argc, argv, environ,
                            RuntimeOption::ServerVariables,
                            RuntimeOption::EnvVariables);
}

void execute_command_line_end(bool coverage, const char *program,
                              bool runCleanup) {
  auto& ti = RI();
  if (coverage && ti.m_reqInjectionData.getCoverage() &&
      !Cfg::Eval::CodeCoverageOutputFile.empty()) {
    ti.m_coverage.dumpOnExit();
  }
  g_context->onShutdownPostSend(); // runs more php
  Eval::Debugger::InterruptPSPEnded(program);
  if (runCleanup) {
    hphp_context_exit();
    hphp_session_exit();
  }
}

#define AT_END_OF_TEXT    __attribute__((__section__(".never_hugify")))

#define ALIGN_HUGE_PAGE   __attribute__((__aligned__(2 * 1024 * 1024)))

#if defined(__has_attribute) && __has_attribute(__no_builtin__)
#define NO_BUILTIN_MEMCPY __attribute__((__no_builtin__("memcpy")))
#else
#define NO_BUILTIN_MEMCPY
#endif

#if defined(__has_attribute) && __has_attribute(__optimize__)
#define OPTIMIZE_2        __attribute__((__optimize__("2")))
#else
#define OPTIMIZE_2
#endif

static void
NEVER_INLINE AT_END_OF_TEXT NO_BUILTIN_MEMCPY OPTIMIZE_2
hugifyMemcpy(uint64_t* dst, uint64_t* src, size_t sz) {
  for (size_t cnt = 0; cnt < sz; cnt += sizeof(uint64_t)) {
    *dst++ = *src++;
  }
}

static bool
NEVER_INLINE AT_END_OF_TEXT ALIGN_HUGE_PAGE OPTIMIZE_2
EXTERNALLY_VISIBLE
hugifyText(char* from, char* to) {
#if !FOLLY_SANITIZE && defined MADV_HUGEPAGE
  if (from > to || (to - from) < sizeof(uint64_t)) {
    return false;
  }
  size_t sz = to - from;

#ifdef __x86_64__
  auto const hasKernelSupport = [] () -> bool {
    KernelVersion version;
    if (version.m_major < 5) return false;
    if (version.m_major > 5) return true;
    if (version.m_minor > 2) return true;
      return false;
  };
  if (Cfg::Eval::NewTHPHotText && hasKernelSupport()) {
    // The new way doesn't work if the region is locked. Note that this means
    // Server.LockCodeMemory won't be applied to the region--there is no
    // guarantee that the region would stay in memory, especially if the kernel
    // fails to find huge pages for us.
    munlock(from, sz);
    madvise(from, sz, MADV_HUGEPAGE);
    Logger::FInfo("Mapped text section onto THP pagecache from {} to {}.",
                  fmt::ptr(from), fmt::ptr(to));
    return true;
  }
#endif

  // We will try to get the hot text in [from, to) on 2M hugetlb pages in 3
  // steps.
  // Step 1: get some 2M hugetlb pages, but don't overwrite the hot text yet.
  // Step 2: copy content of hot text to the hugetlb pages.
  // Step 3: use `mremap()` to replace [from, to) with the hugetlb pages that
  // already contain the original content.
  //
  // This approach has the advantage that the hot range always contains the
  // right sequences of instructions (except when the kernel is doing mremap,
  // which isn't visible in user space as we only have a single thread running
  // here), so we don't have to worry about problems such as unavailability of
  // memcpy.
  HugePageInfo info = get_huge2m_info();
  if (info.free_hugepages * size2m < sz) {
    Logger::Warning("Insufficient hugetlb reservation for hot text.");
    return false;
  }
  auto tempPages = mmap(nullptr,
                        sz + size2m,   // extra size for alignment
                        PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (tempPages == MAP_FAILED) {
    perror("Failed to create temporary mapping");
    return false;
  }
  // An extra copy of the hot text, just in case the kernel messes things up
  // when manipulating the mappings.
  auto backup = static_cast<char*>(malloc(sz));
  memcpy(backup, from, sz);
  SCOPE_EXIT {
    munmap(tempPages, sz);
    free(backup);
    Debug::DebugInfo::setPidMapOverlay(from, to);
  };
  auto aligned = static_cast<char*>(ru<size2m>(tempPages));
  auto const requestedPages = sz / size2m;
  // Now map some hugetlb pages starting at `aligned`.
  auto const actualPages = remap_interleaved_2m_pages(aligned, requestedPages);
  if (actualPages < requestedPages) {
    Logger::Warning("Insufficient hugetlb reservation for hot text.");
    return false;
  }
  memcpy(aligned, from, sz);
  mprotect(aligned, sz, PROT_READ | PROT_EXEC);
  // We might have done mlock() when obtaining hugetlb pages, so unlock before
  // moving the mapping.
  munlock(aligned, sz);
  // Replace the original mapping one page at a time using mremap(), which has
  // the limitation of only handling one mapping at a time.
  for (size_t offset = 0; offset < sz; offset += size2m) {
    if (MAP_FAILED == mremap(aligned + offset, size2m, size2m,
                             MREMAP_MAYMOVE | MREMAP_FIXED,
                             from + offset)) {
      perror("mremap() failed");
      // I don't know why mremap() would fail here. If it does fail, the kernel
      // should not change existing mapping. However, we don't want to rely on
      // the kernel doing the right thing. Check and restore the pages from
      // backup if needed.
      bool needsRestoration = false;
      unsigned char status[size2m / size4k];
      if (mincore(from + offset, size2m, status) == -1) {
        // The range now contains unmapped memory, overwrite the mapping and
        // restore from backup.
        needsRestoration = true;
      } else {
        needsRestoration = memcmp(from + offset, backup + offset, size2m);
      }
      if (needsRestoration) {
        void* restored = mmap(from + offset, size2m,
                              PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                              -1, 0);
        if (restored != from + offset) {
          // Either MAP_FAILED, or maybe MAP_FIXED doesn't work. At this point,
          // there is not much we can do. The process will likely crash soon.
          perror("mmap() failed when recovering hot text mapping");
          return false;
        }
        hugifyMemcpy((uint64_t*)(from + offset), (uint64_t*)(backup + offset), size2m);
        mprotect(from + offset, size2m, PROT_READ | PROT_EXEC);
      }
      if (offset > size2m) {
        Logger::FWarning("Mapped text section onto hugetlb pages from {} to {},"
                         "which is smaller than the intended size of {} bytes",
                         fmt::ptr(from),
                         fmt::ptr(from + offset - size2m),
                         sz);
      } else {
        Logger::FWarning("Unable to get any page on hugetlb pages");
      }
      return false;
    }
  }
  Logger::FInfo("Mapped text section onto hugetlb pages from {} to {}",
                fmt::ptr(from), fmt::ptr(to));
  return true;
#endif
  return false;
}

static void pagein_self(void) {
#if defined(USE_JEMALLOC)
  bool background_threads = false;
  if (mallctlRead<bool, true>("background_thread", &background_threads)) {
    background_threads = false;
    Logger::Warning("Failed to determine jemalloc background thread state");
  }
  if (background_threads &&
      mallctlWrite<bool, true>("background_thread", false)) {
    Logger::Warning("Failed to disable jemalloc background threads");
  }
  SCOPE_EXIT {
    if (background_threads &&
        mallctlWrite<bool, true>("background_thread", true)) {
      Logger::Warning("Failed to enable jemalloc background threads");
    }
  };
#endif

  // Other than the jemalloc background threads, which should've been stopped by
  // now, the only thread allowed here is the current one.  Check that and alarm
  // people when they accidentally created threads before this point.
  int numTry = 0;
  while (Process::GetNumThreads() > 1) {
    if (numTry < 3) {
      numTry++;
      usleep(1000);
    } else {
      break;
    }
  }
  auto nThreads = Process::GetNumThreads();
  if (nThreads > 1) {
    Logger::Error("%d threads running, cannot hugify text!", nThreads);
    fprintf(stderr,
            "HHVM is broken: %u threads running in hugifyText()!\n",
            nThreads);
    if (debug) {
      throw std::runtime_error{
        "you cannot create threads before pagein_self"
      };
    }
  }

  if (Cfg::Server::SchedPolicy >= 0 && Cfg::Server::SchedPolicy <= SCHED_BATCH) {
    sched_param param{};
    if (Cfg::Server::SchedPolicy == SCHED_RR) {
      param.sched_priority =
        std::max(0, sched_get_priority_min(Cfg::Server::SchedPolicy));
    }
    auto const ret = sched_setscheduler(0, Cfg::Server::SchedPolicy, &param);
    if (ret) {
      Logger::Error("failed to adjust scheduling priority: " +
                    folly::errnoStr(errno));
    } else {
      Logger::Info("successfully adjusted scheduling priority to %d",
                   Cfg::Server::SchedPolicy);
    }
  }
  if (Cfg::Server::SchedPriority) {
    auto const ret = setpriority(PRIO_PROCESS, 0, Cfg::Server::SchedPriority);
    if (ret) {
      Logger::FError("failed to setpriority to {}: {}", Cfg::Server::SchedPriority,
                     folly::errnoStr(errno));
    }
  }

  auto mapped_huge = false;
  auto const try_map_huge =
    hugePagesSupported() &&
    Cfg::Eval::MaxHotTextHugePages > 0 &&
    (char*)__hot_start != nullptr && (char*)__hot_end != nullptr &&
    nThreads <= 1;

  SCOPE_EXIT {
    if (try_map_huge != mapped_huge) {
      Logger::Warning("Failed to hugify the .text section");
    }
  };

  if (auto fp = fopen("/proc/self/maps", "r")) {
    SCOPE_EXIT {
      fclose(fp);
    };
    while (!feof(fp)) {
      char line[PATH_MAX + 256];
      if (fgets(line, sizeof(line), fp) == 0)
        break;
      unsigned long begin, end, inode, pgoff;
      char mapname[PATH_MAX];
      char perm[5];
      char dev[11];
      int r = sscanf(line, "%lx-%lx %4s %lx %10s %ld %s",
                     &begin, &end, perm, &pgoff, dev, &inode, mapname);

      // Look for private, read/executable segments that correspond to a file on
      // disk.
      if (r != 7 ||
          memcmp(perm, "r-xp", 4) ||
          access(mapname, F_OK) != 0) {
        continue;
      }
      // Do it for the main binary only.
      if (Process::Argv[0] &&
          strcmp(basename(mapname), basename(Process::Argv[0]))) {
        continue;
      }

      auto beginPtr = (char*)begin;
      auto endPtr = (char*)end;
      auto hotStart = (char*)__hot_start;
      auto hotEnd = (char*)__hot_end;

      if (try_map_huge && beginPtr <= hotStart && hotEnd <= endPtr) {
        char* from = rd<size2m>(hotStart);
        if (from < beginPtr) from += size2m;
        char* to = ru<size2m>(hotEnd);
        if (to > endPtr) to -= size2m;
        if (to <= from) continue;
        const size_t maxHugeHotTextBytes =
          Cfg::Eval::MaxHotTextHugePages * size2m;
        if (to - from >  maxHugeHotTextBytes) {
          to = from + maxHugeHotTextBytes;
        }
        // Check that hugifyText() does not start in hot text.
        if (to <= (void*)hugifyText || from > (void*)hugifyText) {
          mapped_huge |= hugifyText(from, to);
        }
      }
      if (Cfg::Server::LockCodeMemory) {
        mlock(beginPtr, end - begin);
      }
    }
  }
}

/* Sets RuntimeOption::ExecutionMode according to commandline options prior to
 * config load.
 */
static void set_execution_mode(ExecutionMode mode) {
    switch (mode) {
        case ExecutionMode::DAEMON:
        case ExecutionMode::SERVER:
        case ExecutionMode::REPLAY:
            Cfg::Server::Mode = true;
            Logger::Escape = true;
            return;
        case ExecutionMode::RUN:
        case ExecutionMode::DEBUG:
        case ExecutionMode::NOTEBOOK:
        case ExecutionMode::TRANSLATE:
        case ExecutionMode::DUMPHHAS:
        case ExecutionMode::VERIFY:
        case ExecutionMode::VSDEBUG:
        case ExecutionMode::GETOPTION:
        case ExecutionMode::EVAL:
        case ExecutionMode::DUMPCOVERAGE:
        case ExecutionMode::LIVE_TRANSLATE:
        // We don't run PHP in "translate" mode, so just treat it like cli mode.
            Cfg::Server::Mode = false;
            Logger::Escape = false;
            return;
    }
}

static void init_repo_file() {
  if (!Cfg::Repo::Authoritative) return;
  assertx(!Cfg::Repo::Path.empty());
  RepoFile::init(Cfg::Repo::Path);
}

static int start_server(const std::string &username) {
  if (!registrationComplete) {
    folly::SingletonVault::singleton()->registrationComplete();
    registrationComplete = true;
  }
  BootStats::start();
  BootStats::set("config_id", RuntimeOption::ConfigId);
  HttpServer::CheckMemAndWait();
  InitFiniNode::ServerPreInit();

  if (!Cfg::Eval::UnixServerPath.empty()) {
    init_cli_server(Cfg::Eval::UnixServerPath.c_str());
  }

  // Before we start the webserver, make sure the entire
  // binary is paged into memory.
  BootStats::markStart("pagein_self");
  pagein_self();
  BootStats::mark("pagein_self");

  set_execution_mode(ExecutionMode::SERVER);

  // Initialize ROAR logging
#ifdef __roar__
  facebook::roar::initROARLogging();
#endif

#if !defined(SKIP_USER_CHANGE)
  if (!username.empty()) {
    if (Logger::UseCronolog) {
      for (const auto& el : RuntimeOption::ErrorLogs) {
        Cronolog::changeOwner(username, el.second.symLink);
      }
    }
    if (!Capability::ChangeUnixUser(username, Cfg::Server::AllowRunAsRoot)) {
      _exit(HPHP_EXIT_FAILURE);
    }
    LightProcess::ChangeUser(username);
  } else if (getuid() == 0 && !Cfg::Server::AllowRunAsRoot) {
    Logger::Error("hhvm not allowed to run as root unless "
                  "-vServer.AllowRunAsRoot=1 is used.");
    _exit(HPHP_EXIT_FAILURE);
  }
  Capability::SetDumpable();
#endif
  // Include hugetlb pages in core dumps.
  Process::SetCoreDumpHugePages();

  BootStats::markStart("init_repo_file");
  init_repo_file();
  BootStats::mark("init_repo_file");

  hphp_process_init();
  SCOPE_EXIT {
    hphp_process_exit();
    Logger::Info("all servers stopped");
    Logger::FlushAll();
  };

  HttpRequestHandler::GetAccessLog().init
    (RuntimeOption::AccessLogDefaultFormat, RuntimeOption::AccessLogs,
     username);
  AdminRequestHandler::GetAccessLog().init
    (Cfg::Log::AdminLogFormat, Cfg::Log::BaseDirectory, Cfg::Log::AdminLogSymLink,
     Cfg::Log::AdminLogFile,
     username);
  XboxRequestHandler::GetAccessLog().init
    (RuntimeOption::AccessLogDefaultFormat, RuntimeOption::RPCLogs,
     username);
  SCOPE_EXIT {
    Logger::FlushAll();
    HttpRequestHandler::GetAccessLog().fini();
    AdminRequestHandler::GetAccessLog().fini();
    XboxRequestHandler::GetAccessLog().fini();
  };

  if (Cfg::Server::InternalWarmupThreads > 0) {
    HttpServer::CheckMemAndWait();
    InitFiniNode::WarmupConcurrentStart(
      Cfg::Server::InternalWarmupThreads);
  }

  HttpServer::CheckMemAndWait();
  // Create the HttpServer before any warmup requests to properly
  // initialize the process
  HttpServer::Server = std::make_shared<HttpServer>();

  // Run the admin server before warmup requests. This allows us to get
  // data from fbagent/dynologd during warmup, which is helpful for debugging.
  // We cannot do this if hotswap is enabled, because it might result in
  // killing ourself instead of the old server!
  if (!Cfg::Server::StopOld) {
    HttpServer::Server->runAdminServerOrExitProcess();
  }

  if (Cfg::Server::InternalWarmupThreads > 0) {
    BootStats::Block timer("concurrentWaitForEnd", true);
    InitFiniNode::WarmupConcurrentWaitForEnd();
  }

  // If we have any warmup requests, replay them before listening for
  // real connections
  BootStats::markStart("warmup");
  {
    Logger::Info("Warming up");
    if (!Cfg::Jit::ProfileWarmupRequests) profileWarmupStart();
    SCOPE_EXIT { profileWarmupEnd(); };
    InternalWarmupRequestPlayer(Cfg::Server::WarmupThreadCount,
                                Cfg::Server::DedupeWarmupRequests)
      .runAfterDelay(Cfg::Server::WarmupRequests);
  }
  BootStats::mark("warmup");

  if (Cfg::Server::StopOld) HttpServer::StopOldServer();

  if (Cfg::Eval::EnableNuma) {
    BootStats::markStart("enable_numa");
    purge_all();
    enable_numa();
    BootStats::mark("enable_numa");
  }
  HttpServer::CheckMemAndWait(true); // Final wait

  if (!Cfg::Eval::UnixServerPath.empty()) {
    start_cli_server();
  }

  if (jit::mcgen::retranslateAllScheduled()) {
    // We ran retranslateAll from deserialized profile.
    BootStats::Block timer("waitForRetranslateAll", true);
    jit::mcgen::joinWorkerThreads();
  }

#ifdef USE_JEMALLOC
  auto const reqHeapSpec = PageSpec{
    Cfg::Eval::Num1GPagesForReqHeap,
    Cfg::Eval::Num2MPagesForReqHeap
  };
  unsigned nSlabs = Cfg::Eval::NumReservedMBForSlabs * (1ull << 20) / kSlabSize;
  setup_local_arenas(reqHeapSpec, nSlabs);

  if (Cfg::Repo::Authoritative) {
    setup_swappable_readonly_arena(Cfg::Eval::HHBCArenaChunkSize);
  }
#endif

  HttpServer::Server->runOrExitProcess();
  HttpServer::Server.reset();

  return 0;
}

static void logSettings() {
  if (Cfg::Server::LogSettingsOnStartup) {
    Logger::Info("Settings: %s\n",
                 folly::toJson(IniSetting::GetAllAsDynamic()).c_str());
  }
}

static InitFiniNode s_logSettings(logSettings, InitFiniNode::When::ServerInit);

std::string translate_stack(const char *hexencoded, bool with_frame_numbers) {
  if (!hexencoded || !*hexencoded) {
    return "";
  }

  StackTrace st(hexencoded);
  std::vector<std::shared_ptr<StackFrameExtra>> frames;
  st.get(frames);

  std::ostringstream out;
  for (size_t i = 0; i < frames.size(); i++) {
    auto f = frames[i];
    if (with_frame_numbers) {
      out << "# " << (i < 10 ? " " : "") << i << ' ';
    }
    out << f->toString();
    out << '\n';
  }
  return out.str();
}

///////////////////////////////////////////////////////////////////////////////

static void prepare_args(int &argc,
                         char **&argv,
                         const std::vector<std::string> &args,
                         const char *file) {
  argv = (char **)malloc((args.size() + 2) * sizeof(char*));
  argc = 0;
  if (file && *file) {
    argv[argc++] = (char*)file;
  }
  for (int i = 0; i < (int)args.size(); i++) {
    argv[argc++] = (char*)args[i].c_str();
  }
  argv[argc] = nullptr;
}

static int execute_program_impl(int argc, char **argv);
int execute_program(int argc, char **argv) {
  int ret_code = HPHP_EXIT_FAILURE;
  try {
    try {
      ret_code = execute_program_impl(argc, argv);
    } catch (const Exception& e) {
      Logger::Error("Uncaught exception: %s", e.what());
      throw;
    } catch (const std::exception& e) {
      Logger::Error("Uncaught exception: %s", e.what());
      throw;
    } catch (...) {
      Logger::Error("Uncaught exception: (unknown)");
      throw;
    }
    if (tempFile.length() && std::filesystem::exists(tempFile)) {
      std::filesystem::remove(tempFile);
    }
  } catch (...) {
    if (HttpServer::Server ||
        folly::SingletonVault::singleton()->livingSingletonCount()) {
      // an exception was thrown that prevented proper shutdown. Its not
      // safe to destroy the globals, or run atexit handlers.
      // abort() so it shows up as a crash, and we can diagnose/fix the
      // exception
      always_assert(false);
    }
  }

  return ret_code;
}

static bool open_server_log_files() {
  bool openedLog = false;
  for (const auto& el : RuntimeOption::ErrorLogs) {
    bool ok = true;
    const auto& name    = el.first;
    const auto& errlog = el.second;
    if (!errlog.logFile.empty()) {
      if (errlog.isPipeOutput()) {
        auto output = popen(errlog.logFile.substr(1).c_str(), "w");
        ok = (output != nullptr);
        Logger::SetOutput(name, output, true);
      } else if (Logger::UseCronolog && errlog.hasTemplate()) {
        auto cronoLog = Logger::CronoOutput(name);
        always_assert(cronoLog);
        cronoLog->m_template = errlog.logFile;
        cronoLog->setPeriodicity();
        if (errlog.periodMultiplier) {
          cronoLog->m_periodMultiple = errlog.periodMultiplier;
        }
        cronoLog->m_linkName = errlog.symLink;
      } else {
        auto output = fopen(errlog.logFile.c_str(), "a");
        ok = (output != nullptr);
        Logger::SetOutput(name, output, false);
      }
      if (!ok) Logger::Error("Can't open log file: %s", errlog.logFile.c_str());
      openedLog |= ok;
    }
  }
  return openedLog;
}

static int compute_hhvm_argc(const options_description& desc,
                             int argc, char** argv) {
  enum ArgCode {
    NO_ARG = 0,
    ARG_REQUIRED = 1,
    ARG_OPTIONAL = 2
  };
  const auto& vec = desc.options();
  std::map<std::string,ArgCode> long_options;
  std::map<std::string,ArgCode> short_options;
  // Build lookup maps for the short options and the long options
  for (unsigned i = 0; i < vec.size(); ++i) {
    auto opt = vec[i];
    auto long_name = opt->long_name();
    ArgCode code = NO_ARG;
    if (opt->semantic()->max_tokens() == 1) {
      if (opt->semantic()->min_tokens() == 1) {
        code = ARG_REQUIRED;
      } else {
        code = ARG_OPTIONAL;
      }
    }
    long_options[long_name] = code;
    auto format_name = opt->format_name();
    if (format_name.size() >= 2 && format_name[0] == '-' &&
        format_name[1] != '-') {
      auto short_name = format_name.substr(1,1);
      short_options[short_name] = code;
    }
  }
  // Loop over the args
  int pos = 1;
  while (pos < argc) {
    const char* str = argv[pos];
    int len = strlen(str);
    if (len == 2 && memcmp(str, "--", 2) == 0) {
      // We found "--". All args after this are intended for the
      // PHP application
      ++pos;
      break;
    }
    if (len >= 3 && str[0] == '-' && str[1] == '-') {
      // Handle long options
      ++pos;
      std::string s(str+2);
      auto it = long_options.find(s);
      if (it != long_options.end() && it->second != NO_ARG && pos < argc &&
          (it->second == ARG_REQUIRED || argv[pos][0] != '-')) {
        ++pos;
      }
    } else if (len >= 2 && str[0] == '-') {
      // Handle short options
      ++pos;
      std::string s;
      s.append(1, str[1]);
      auto it = short_options.find(s);
      if (it != short_options.end() && it->second != 0 && len == 2 &&
          pos < argc && (it->second == ARG_REQUIRED || argv[pos][0] != '-')) {
        ++pos;
      }
    } else {
      // We've found a non-option argument. This arg and all args
      // that follow are intended for the PHP application
      break;
    }
  }
  return pos;
}

/*
 * alloc.h defines a minimum C++ stack size but that only applies to threads we
 * manually create.  When the main thread will be executing PHP rather than just
 * managing a server, make sure its stack is big enough.
 */
static void set_stack_size() {
  struct rlimit rlim;
  if (getrlimit(RLIMIT_STACK, &rlim) != 0) return;

  if (rlim.rlim_cur < kStackSizeMinimum || rlim.rlim_cur == RLIM_INFINITY) {
    rlim.rlim_cur = kStackSizeMinimum;
    if (setrlimit(RLIMIT_STACK, &rlim)) {
      Logger::Error("failed to set stack limit to %zd\n", kStackSizeMinimum);
    }
  }
}

std::vector<int> get_executable_lines(const Unit* compiled) {
  hphp_fast_set<int> lines_set;

  compiled->forEachFunc([&](const Func* func) {
    if(func->isGenerated() || func->isAbstract()) {
      // Both generated and abstract methods have some underlying code
      // generated by the compiler. Considering this generated code as
      // "executable lines" is confusing (interface methods and classes'
      // closing curly braces show as executable)
      return false;
    }

    auto start = func->entry();
    auto pc = start;

    // Do not include default value initialization in generated lines
    auto endOff = [&]() {
      for (auto const& param : func->params()) {
        if (param.funcletOff != kInvalidOffset) {
          return param.funcletOff;
        }
      }
      return func->bclen();
    }();
    auto end = pc + endOff;

    auto const lineTable = func->getOrLoadLineTableCopy();
    while (pc < end) {
      auto op = peek_op(pc);
      if (CodeCoverage::isCoverable(op)) {
        auto line = SourceLocation::getLineInfo(lineTable, pc - start).second;
        if (line > 0) {
          lines_set.insert(line);
        }
      }

      pc += instrLen(pc);
    }

    return false;
  });

  std::vector<int> lines_vector(lines_set.begin(), lines_set.end());
  std::sort(lines_vector.begin(), lines_vector.end());
  return lines_vector;
}

static int execute_program_impl(int argc, char** argv) {
  std::string usage = "Usage:\n\n   ";
  usage += argv[0];
  usage += " [-m <mode>] [<options>] [<arg1>] [<arg2>] ...\n\nOptions";

  ProgramOptions po;
  options_description desc(usage.c_str());
  desc.add_options()
    ("help", "display this message")
    ("version", "display version number")
    ("modules", "display modules")
    ("list-extensions",
     "display loaded extensions as JSON (name, version, oncall)")
    ("info", "PHP information")
    ("php", "emulate the standard php command line")
    ("compiler-id", "display the git hash for the compiler")
    ("repo-schema", "display the repository schema id")
    ("repo-option-hash", "print the repo-options hash for the specified file "
     "and exit")
    ("mode,m", value<ExecutionMode>(&po.mode)->default_value(ExecutionMode::RUN, "run"),
     "run | debug (d) | notebook | vsdebug | server (s) | daemon | replay | "
     "translate (t) | verify | getoption | eval")
    ("interactive,a", "Shortcut for --mode debug") // -a is from PHP5
    ("config,c", value<std::vector<std::string>>(&po.config)->composing(),
     "load specified config file")
    ("config-value,v",
     value<std::vector<std::string>>(&po.confStrings)->composing(),
     "individual configuration string in a format of name=value, where "
     "name can be any valid configuration for a config file")
    ("define,d", value<std::vector<std::string>>(&po.iniStrings)->composing(),
     "define an ini setting in the same format ( foo[=bar] ) as provided in a "
     ".ini file")
    ("no-config", "don't use the default php.ini")
    ("port,p", value<int>(&po.port)->default_value(-1),
     "start an HTTP server at specified port")
    ("port-fd", value<int>(&po.portfd)->default_value(-1),
     "use specified fd instead of creating a socket")
    ("ssl-port-fd", value<int>(&po.sslportfd)->default_value(-1),
     "use specified fd for SSL instead of creating a socket")
    ("admin-port", value<int>(&po.admin_port)->default_value(-1),
     "start admin listener at specified port")
    ("debug-config", value<std::string>(&po.debugger_options.configFName),
      "load specified debugger config file")
    ("debug-host,h",
     value<std::string>(&po.debugger_options.host)->implicit_value("localhost"),
     "connect to debugger server at specified address")
    ("debug-port", value<int>(&po.debugger_options.port)->default_value(-1),
     "connect to debugger server at specified port")
    ("debug-extension", value<std::string>(&po.debugger_options.extension),
     "PHP file that extends command 'arg'")
    ("debug-cmd", value<std::vector<std::string>>(
     &po.debugger_options.cmds)->composing(),
     "executes this debugger command and returns its output in stdout")
    ("debug-sandbox",
     value<std::string>(&po.debugger_options.sandbox)->default_value("default"),
     "initial sandbox to attach to when debugger is started")
    ("from", value<std::string>(&po.debugger_options.from),
     "caller identifier for debugger usage logging")
    ("user,u", value<std::string>(&po.user),
     "run server under this user account")
    ("file,f", value<std::string>(&po.file),
     "execute specified file")
    ("lint,l", value<std::string>(&po.lint),
     "lint specified file")
    ("show,w", value<std::string>(&po.show),
     "output specified file and do nothing else")
    ("check-repo", "attempt to load repo and then exit")
    ("verify-resolutions", "check that resolved type constraints and constants "
                           "in an hhbc file match a source repo on disk")
    ("temp-file",
     "file specified is temporary and removed after execution")
    ("count", value<int>(&po.count)->default_value(1),
     "how many times to repeat execution")
    ("no-safe-access-check",
      value<bool>(&po.noSafeAccessCheck)->default_value(false),
     "whether to ignore safe file access check")
    ("arg", value<std::vector<std::string>>(&po.args)->composing(),
     "arguments")
    ("extra-header", value<std::string>(&Logger::ExtraHeader),
     "extra-header to add to log lines")
    ("build-id", value<std::string>(&po.buildId),
     "unique identifier of compiled server code")
    ("instance-id", value<std::string>(&po.instanceId),
     "unique identifier of server instance")
    ("vsDebugPort", value<int>(&po.vsDebugPort)->default_value(-1),
      "Debugger TCP port to listen on for the VS Code debugger extension")
    ("vsDebugDomainSocketPath",
      value<std::string>(&po.vsDebugDomainSocket)->default_value(""),
      "Debugger port to listen on for the VS Code debugger extension")
    ("vsDebugNoWait", value<bool>(&po.vsDebugNoWait)->default_value(false),
      "Indicates the debugger should not block script startup waiting for "
      "a debugger client to attach. Only applies if vsDebugPort or "
        "vsDebugDomainSocketPath is specified.")
    ;

  positional_options_description p;
  p.add("arg", -1);
  variables_map vm;

  // Before invoking the boost command line parser, we do a manual pass
  // to find the first occurrence of either "--" or a non-option argument
  // in order to determine which arguments should be consumed by HHVM and
  // which arguments should be passed along to the PHP application. This
  // is necessary so that the boost command line parser doesn't choke on
  // args intended for the PHP application.
  int hhvm_argc = compute_hhvm_argc(desc, argc, argv);
  // Need to have a parent try for opts so I can use opts in the catch of
  // one of the sub-tries below.
  try {
    // Invoke the boost command line parser to parse the args for HHVM.
    auto opts = command_line_parser(hhvm_argc, argv)
      .options(desc)
      .positional(p)
      // If these style options are changed, compute_hhvm_argc() will
      // need to be updated appropriately
      .style(command_line_style::default_style &
             ~command_line_style::allow_guessing &
             ~command_line_style::allow_sticky &
             ~command_line_style::long_allow_adjacent)
      .run();
    try {
      // Manually append the args for the PHP application.
      int pos = 0;
      for (unsigned m = 0; m < opts.options.size(); ++m) {
        const auto& bo = opts.options[m];
        if (bo.string_key == "arg") {
          ++pos;
        }
      }
      for (unsigned m = hhvm_argc; m < argc; ++m) {
        std::string str = argv[m];
        basic_option<char> bo;
        bo.string_key = "arg";
        bo.position_key = pos++;
        bo.value.push_back(str);
        bo.original_tokens.push_back(str);
        bo.unregistered = false;
        bo.case_insensitive = false;
        opts.options.push_back(bo);
      }
      // Process the options
      store(opts, vm);
      notify(vm);
      if (vm.contains("interactive") /* or -a */) {
        po.mode = ExecutionMode::DEBUG;
      }

      set_execution_mode(po.mode);

      if (po.config.empty() && !vm.count("no-config")
          && ::getenv("HHVM_NO_DEFAULT_CONFIGS") == nullptr) {
        auto file_callback = [&po] (const char *filename) {
          Logger::Verbose("Using default config file: %s", filename);
          po.config.push_back(filename);
        };
        add_default_config_files_globbed(DEFAULT_CONFIG_DIR "/php*.ini",
                                         file_callback);
        add_default_config_files_globbed(DEFAULT_CONFIG_DIR "/config*.hdf",
                                         file_callback);
      }
      const auto env_config = ::getenv("HHVM_CONFIG_FILE");
      if (env_config != nullptr) {
        add_default_config_files_globbed(
          env_config,
          [&po](const char* filename) {
            Logger::Verbose("Using config file from environment: %s", filename);
            po.config.push_back(filename);
          }
        );
      }
    } catch (const error &e) {
      Logger::Error("Error in command line: %s", e.what());
      cout << desc << "\n";
      return HPHP_EXIT_FAILURE;
    } catch (...) {
      Logger::Error("Error in command line.");
      cout << desc << "\n";
      return HPHP_EXIT_FAILURE;
    }
  } catch (const error &e) {
    Logger::Error("Error in command line: %s", e.what());
    cout << desc << "\n";
    return HPHP_EXIT_FAILURE;
  } catch (...) {
    Logger::Error("Error in command line parsing.");
    cout << desc << "\n";
    return HPHP_EXIT_FAILURE;
  }

#ifdef ENABLE_SYSTEM_LOCALE_ARCHIVE
  // Use system locales as the default LOCALE_ARCHIVE for nix patched glibc
  if (::getenv("LOCALE_ARCHIVE") == nullptr) {
    ::setenv("LOCALE_ARCHIVE", "/usr/lib/locale/locale-archive", true);
  }
#endif

  // reuse -h for help command if possible
  if (vm.count("help") || (vm.count("debug-host") && (po.mode != ExecutionMode::DEBUG && po.mode != ExecutionMode::NOTEBOOK))) {
    cout << desc << "\n";
    return 0;
  }
  if (vm.contains("version")) {
    cout << "HipHop VM";
    cout << " " << HHVM_VERSION;
    cout << " (" << (debug ? "dbg" : "rel") << ")";
    if (use_roar) cout << " (roar)";
    cout << " (packedptr)\n";
    cout << "Compiler: " << compilerId() << "\n";
    cout << "Repo schema: " << repoSchemaId() << "\n";
    return 0;
  }
  if (vm.contains("modules")) {
    rds::local::init();
    SCOPE_EXIT { rds::local::fini(); };
    tl_heap.getCheck();
    Array exts = ExtensionRegistry::getLoaded();
    cout << "[PHP Modules]" << "\n";
    for (ArrayIter iter(exts); iter; ++iter) {
      cout << iter.second().toString().toCppString() << "\n";
    }
    return 0;
  }
  if (vm.contains("list-extensions")) {
    auto exts = ExtensionRegistry::getExtensions();
    folly::dynamic arr = folly::dynamic::array;
    for (auto const* ext : exts) {
      folly::dynamic obj = folly::dynamic::object
        ("name", ext->getName())
        ("version", ext->getVersion())
        ("oncall", ext->getOncall());
      arr.push_back(std::move(obj));
    }
    cout << folly::toPrettyJson(arr) << "\n";
    return 0;
  }
  if (vm.contains("compiler-id")) {
    cout << compilerId() << "\n";
    return 0;
  }

  if (vm.contains("repo-schema")) {
    cout << repoSchemaId() << "\n";
    return 0;
  }

  if (!po.show.empty()) {
    hphp_thread_init(true);
    g_context.getCheck();
    SCOPE_EXIT { hphp_thread_exit(true); };

    auto f = req::make<PlainFile>();
    f->open(po.show, "r");
    if (!f->valid()) {
      Logger::Error("Unable to open file %s", po.show.c_str());
      return HPHP_EXIT_FAILURE;
    }
    f->print();
    f->close();
    return 0;
  }

  po.isTempFile = vm.count("temp-file");

  // forget the source for systemlib.php unless we are debugging
  if (po.mode != ExecutionMode::DEBUG && po.mode != ExecutionMode::VSDEBUG && po.mode != ExecutionMode::NOTEBOOK) {
    SystemLib::s_source = "";
  }
  if (po.mode == ExecutionMode::VSDEBUG) {
    Cfg::Debugger::EnableVSDebugger = true;
    Cfg::Debugger::VSDebuggerListenPort = po.vsDebugPort;
    Cfg::Debugger::VSDebuggerDomainSocketPath = po.vsDebugDomainSocket;
    Cfg::Debugger::VSDebuggerNoWait = po.vsDebugNoWait;
  }

  // we need to to initialize these very early
  pcre_init();
  // this is needed for libevent2 to be thread-safe, which backs Hack ASIO.
  #ifndef HHVM_FACEBOOK
  // FB uses a custom libevent 1
  evthread_use_pthreads();
  #endif

  rds::local::init();
  SCOPE_EXIT { rds::local::fini(); };
  tl_heap.getCheck();
  if (Cfg::Server::Mode) {
    // Create the hardware counter before reading options,
    // so that the main thread never has inherit set in server
    // mode
    HardwareCounter::s_counter.getCheck();
  }
  std::vector<std::string> messages;
  // We want the ini map to be freed after processing and loading the options
  // So put this in its own block
  {
    IniSettingMap ini = IniSettingMap();
    Hdf config;
    s_config_files = po.config;
    // Start with .hdf and .ini files
    for (auto& filename : s_config_files) {
      if (std::filesystem::exists(filename)) {
        Config::ParseConfigFile(filename, ini, config);
      } else {
        Logger::Warning(
          "The configuration file %s does not exist",
          filename.c_str()
        );
      }
    }

    auto const scriptFilePath =
      !po.file.empty() ? po.file :
      !po.args.empty() ? po.args[0] :
      std::string("");

    // Now, take care of CLI options and then officially load and bind things
    s_ini_strings = po.iniStrings;
    RuntimeOption::Load(
      ini,
      config,
      po.iniStrings,
      po.confStrings,
      &messages,
      scriptFilePath
    );
    std::vector<std::string> badnodes;
    config.lint(badnodes);
    for (const auto& badnode : badnodes) {
      const auto msg = "Possible bad config node: " + badnode;
      fprintf(stderr, "%s\n", msg.c_str());
      messages.push_back(msg);
    }

    if (po.mode == ExecutionMode::GETOPTION) {
      if (po.args.size() < 1) {
        fprintf(stderr, "Must specify an option to load\n");
        return HPHP_EXIT_FAILURE;
      }
      Variant value;
      bool ret = IniSetting::Get(po.args[0], value);
      if (!ret) {
        fprintf(stderr, "No such option: %s\n", po.args[0].data());
        return HPHP_EXIT_FAILURE;
      }
      if (!value.isString()) {
        VariableSerializer vs{VariableSerializer::Type::JSON};
        value = vs.serializeValue(value, false);
      }
      printf("%s\n", value.toString().data());
      return 0;
    }
    if (vm.contains("repo-option-hash")) {
      rds::local::init();
      SCOPE_EXIT { rds::local::fini(); };

      if (scriptFilePath.empty()) {
        std::cerr << "Must specify of file with --repo-option-hash\n";
        return HPHP_EXIT_FAILURE;
      }
      if (access(scriptFilePath.data(), F_OK) != 0) {
        std::cerr << "Cannot access file " << scriptFilePath << "\n";
        return HPHP_EXIT_FAILURE;
      }
      auto const& opts = RepoOptions::forFile(scriptFilePath);
      cout << opts.path().string() << ": "
           << opts.flags().cacheKeySha1().toString() << "\n";
      return 0;
    }
  }

  std::vector<int> inherited_fds;
  RuntimeOption::BuildId = po.buildId;
  RuntimeOption::InstanceId = po.instanceId;

  // Do this as early as possible to avoid creating temp files and spawning
  // light processes. Correct compilation still requires loading all of the
  // ini/hdf/cli options.
  if (po.mode == ExecutionMode::DUMPHHAS ||
      po.mode == ExecutionMode::VERIFY ||
      po.mode == ExecutionMode::DUMPCOVERAGE) {
    if (po.file.empty() && po.args.empty()) {
      std::cerr << "Nothing to do. Pass a hack file to compile.\n";
      return HPHP_EXIT_FAILURE;
    }

    auto const file = [] (std::string file) -> std::string {
      if (!FileUtil::isAbsolutePath(file)) {
        return (
          SourceRootInfo::GetCurrentSourceRoot() / std::move(file)
        ).native();
      }
      return file;
    }(po.file.empty() ? po.args[0] : po.file);

    std::fstream fs(file, std::ios::in);
    if (!fs) {
      std::cerr << "Unable to open \"" << file << "\"\n";
      return HPHP_EXIT_FAILURE;
    }
    std::stringstream contents;
    contents << fs.rdbuf();

    auto const& defaults = RepoOptions::defaults();
    auto const attributes = UnitEmitterAttributes::defaults();
    auto const str = contents.str();
    auto const sha1 = SHA1{
      mangleUnitSha1(
        string_sha1(str), file, defaults.flags(), attributes
      )
    };

    if (!registrationComplete) {
      folly::SingletonVault::singleton()->registrationComplete();
      registrationComplete = true;
    }

    hphp_thread_init();
    g_context.getCheck();
    SCOPE_EXIT { hphp_thread_exit(); };

    if (po.mode == ExecutionMode::DUMPHHAS) {
      Cfg::Eval::DumpHhas = true;
    } else if (po.mode != ExecutionMode::DUMPCOVERAGE) {
      Cfg::Eval::VerifyOnly = true;
    }

    LazyUnitContentsLoader loader{
      sha1,
      str,
      defaults.flags(),
      defaults.dir(),
      attributes
    };
    auto compiled =
      compile_file(loader, CodeSource::User, file.c_str(), nullptr, nullptr);

    if (po.mode == ExecutionMode::VERIFY) {
      return 0;
    }

    // This will dump the hhas for file as EvalDumpHhas was set
    if (!compiled) {
      std::cerr << "Unable to compile \"" << file << "\"\n";
      return HPHP_EXIT_FAILURE;
    }

    if (po.mode ==  ExecutionMode::DUMPCOVERAGE) {
      std::cout << "[" << folly::join(", ", get_executable_lines(compiled))
                << "]" << std::endl;
      return 0;
    }

    return 0;
  }

  if (po.port != -1) {
    Cfg::Server::Port = po.port;
  }
  if (po.portfd != -1) {
    Cfg::Server::PortFd = po.portfd;
    inherited_fds.push_back(po.portfd);
  }
  if (po.sslportfd != -1) {
    Cfg::Server::SSLPortFd = po.sslportfd;
    inherited_fds.push_back(po.sslportfd);
  }
  if (po.admin_port != -1) {
    Cfg::AdminServer::Port = po.admin_port;
  }
  if (po.noSafeAccessCheck) {
    Cfg::Server::SafeFileAccess = false;
  }
  IniSetting::s_system_settings_are_set = true;
  if (debug) tl_heap->checkHeap("resetRuntimeOptions");
  tl_heap.destroy();
  rds::local::fini();
  // From this point on there is no tl_heap until hphp_process_init is called.

  auto opened_logs = open_server_log_files();
  if (po.mode == ExecutionMode::DAEMON) {
    if (!opened_logs) {
      Logger::Error("Log file not specified under daemon mode.\n\n");
    }
    proc::daemonize();
  }

  if (Cfg::Server::Mode) {
    for (auto const& m : messages) {
      Logger::Info(m);
    }
  }

  // Defer the initialization of light processes until the log file handle is
  // created, so that light processes can log to the right place. If we ever
  // lose a light process, stop the server instead of proceeding in an
  // uncertain state. Don't start them in DumpHhas mode because
  // it _Exit()s after loading the first non-systemlib unit.
  if (!Cfg::Eval::DumpHhas) {
    LightProcess::SetLostChildHandler([](pid_t /*child*/) {
      if (!HttpServer::Server) return;
      if (!HttpServer::Server->isStopped()) {
        Logger::Error(
          "Received SIGCHLD for light process, shutting down server"
        );
        HttpServer::Server->stopOnSignal(SIGCHLD);
      }
    });
    LightProcess::Initialize(Cfg::Server::LightProcessFilePrefix,
                             Cfg::Server::LightProcessCount,
                             Cfg::Eval::RecordSubprocessTimes,
                             inherited_fds);
  }

#if USE_JEMALLOC
  if (Cfg::Server::Mode) {
    purge_all();
    setup_auto_arenas({Cfg::Eval::Num1GPagesForA0, Cfg::Eval::Num2MPagesForA0});
  }
  if (Cfg::Eval::FileBackedColdArena) {
    set_cold_file_dir(Cfg::Eval::ColdArenaFileDir.c_str());
    enable_high_cold_file();
  }
#endif

  auto const addTypeToEmbeddedPath = [&](std::string path, const char* type) {
    auto const typePlaceholder = "%{type}";
    assertx(strstr(type, typePlaceholder) == nullptr);
    size_t idx;
    if ((idx = path.find(typePlaceholder)) != std::string::npos) {
      path.replace(idx, strlen(typePlaceholder), type);
    }
    return path;
  };

  // We want to initialize the type-scanners as early as possible
  // because any allocations before-hand will get a generic unknown
  // type type-index.
  SCOPE_EXIT {
    // this would be handled by hphp_process_exit, but some paths
    // short circuit before getting there.
    embedded_data_cleanup();
  };
  try {
    type_scan::init(
      addTypeToEmbeddedPath(
        Cfg::Eval::EmbeddedDataExtractPath,
        "type_scanners"
      ),
      addTypeToEmbeddedPath(
        Cfg::Eval::EmbeddedDataFallbackPath,
        "type_scanners"
      ),
      Cfg::Eval::EmbeddedDataTrustExtract
    );
  } catch (const type_scan::InitException& exn) {
    Logger::Error("Unable to initialize GC type-scanners: %s", exn.what());
    exit(HPHP_EXIT_FAILURE);
  }
  ThreadLocalManager::GetManager().initTypeIndices();

  // It's okay if this fails.
  init_member_reflection(
    addTypeToEmbeddedPath(
      Cfg::Eval::EmbeddedDataExtractPath,
      "member_reflection"
    ),
    addTypeToEmbeddedPath(
      Cfg::Eval::EmbeddedDataFallbackPath,
      "member_reflection"
    ),
    Cfg::Eval::EmbeddedDataTrustExtract
  );

  if (!ShmCounters::initialize(true, Logger::Error)) {
    exit(HPHP_EXIT_FAILURE);
  }

  if (vm.contains("check-repo")) {
    hphp_thread_init();
    always_assert(Cfg::Repo::Authoritative);
    init_repo_file();
    RepoFile::loadGlobalTables();
    RepoFile::globalData().load();
    return 0;
  }

  if (vm.contains("verify-resolutions")) {
    if (po.args.size() < 2) {
      fprintf(stderr,
              "Usage: %s --verify-resolutions [hhbc-repo] [src-root]\n",
              argv[0]);
      exit(HPHP_EXIT_FAILURE);
    }
    std::vector<std::string> paths;
    for (size_t i = 2; i < po.args.size(); ++i) paths.emplace_back(po.args[i]);
    compare_resolved_types(po.args[0], po.args[1], paths);
    return 0;
  }

  if (!po.lint.empty()) {
    Logger::LogHeader = false;
    Logger::LogLevel = Logger::LogInfo;
    Logger::UseCronolog = false;
    Logger::UseLogFile = true;
    // we're linting, reset whatever logger settings and write once to stdout
    Logger::ClearThreadLog();
    for (auto& el : RuntimeOption::ErrorLogs) {
      const auto& name = el.first;
      Logger::SetTheLogger(name, nullptr);
    }
    Logger::SetTheLogger(Logger::DEFAULT, std::make_unique<Logger>());

    if (po.isTempFile) {
      tempFile = po.lint;
    }

    if (!registrationComplete) {
      folly::SingletonVault::singleton()->registrationComplete();
      registrationComplete = true;
    }

    hphp_thread_init();
    g_context.getCheck();
    SCOPE_EXIT { hphp_thread_exit(); };

    try {
      auto const file = [&] {
        if (!FileUtil::isAbsolutePath(po.lint)) {
          return (SourceRootInfo::GetCurrentSourceRoot() / po.lint).native();
        }
        return po.lint;
      }();

      std::fstream fs{file, std::ios::in};
      if (!fs) throw FileOpenException(po.lint);
      std::stringstream contents;
      contents << fs.rdbuf();

      auto const repoOptions = RepoOptions::forFile(file.c_str());
      auto const attributes = UnitEmitterAttributes::forAbsolutePath(
        file, repoOptions
      );

      auto const str = contents.str();
      auto const sha1 = SHA1{mangleUnitSha1(
        string_sha1(str), file, repoOptions.flags(), attributes
      )};

      // Disable any cache hooks because they're generally not useful
      // if we're just going to lint (and they might be expensive).
      g_unit_emitter_cache_hook = nullptr;

      LazyUnitContentsLoader loader{
        sha1,
        str,
        repoOptions.flags(),
        repoOptions.dir(),
        attributes
      };
      auto const unit =
        compile_file(loader, CodeSource::User, file.c_str(), nullptr, nullptr);
      if (!unit) {
        std::cerr << "Unable to compile \"" << file << "\"\n";
        return HPHP_EXIT_FAILURE;
      }
      if (auto const info = unit->getFatalInfo()) {
        raise_parse_error(unit->filepath(), info->m_fatalMsg.c_str(),
                          info->m_fatalLoc);
      }
    } catch (const FatalErrorException& e) {
      Cfg::ErrorHandling::CallUserHandlerOnFatals = false;
      Cfg::Log::AlwaysLogUnhandledExceptions = false;
      g_context->onFatalError(e);
      return HPHP_EXIT_FAILURE;
    }
    Logger::Info("No syntax errors detected in %s", po.lint.c_str());
    return 0;
  }

  if (po.mode == ExecutionMode::LIVE_TRANSLATE) {
    Cfg::Jit::Enabled = true;
    Cfg::Jit::PGO = false;

    init_repo_file();
    hphp_process_init();
    SCOPE_EXIT { hphp_process_exit(); };

    hphp_session_init(Treadmill::SessionKind::CLISession);
    SCOPE_EXIT {
      hphp_context_exit();
      hphp_session_exit();
    };

    auto const fname = Cfg::Eval::LiveTranslateFunction;

    auto const f = [&] () -> Func* {
      std::vector<std::string> parts;
      folly::split("::", fname, parts);
      if (parts.size() != 2) return Func::load(makeStaticString(fname));

      auto const c = Class::load(makeStaticString(parts[0]));
      if (!c) return nullptr;
      return c->lookupMethod(makeStaticString(parts[1]));
    }();

    if (!f) {
      std::cerr << "Unable to load function: "
                << Cfg::Eval::LiveTranslateFunction
                << std::endl;
      return -1;
    }

    auto const off = Cfg::Eval::LiveTranslateOffset;
    if (off >= f->bclen()) {
      std::cerr << "Bytecode offset past function end: " << off << std::endl;
      return -1;
    }


    auto msg = jit::mcgen::debug_translate_live(
      SrcKey{f, off, ResumeMode::None},
      Cfg::Eval::LiveTranslateGuards
    );
    if (!msg.empty()) {
      std::cerr << msg << std::endl;
      return -1;
    }

    return 0;
  }

  if (argc <= 1 ||
    po.mode == ExecutionMode::RUN ||
    po.mode == ExecutionMode::DEBUG ||
    po.mode == ExecutionMode::NOTEBOOK ||
    po.mode == ExecutionMode::VSDEBUG ||
    po.mode == ExecutionMode::EVAL) {
    set_stack_size();

    if (po.isTempFile) {
      tempFile = po.file;
    }

    set_execution_mode(ExecutionMode::RUN);
    /* recreate the hardware counters for the main thread now that we know
     * whether to include subprocess times */
    HardwareCounter::s_counter.destroy();
    HardwareCounter::s_counter.getCheck();

    int new_argc;
    char **new_argv;
    prepare_args(new_argc, new_argv, po.args, po.file.c_str());

    std::string const cliFile = !po.file.empty() ? po.file :
                                 new_argv[0] ? new_argv[0] : "";
    if (po.mode != ExecutionMode::DEBUG && po.mode != ExecutionMode::NOTEBOOK && po.mode != ExecutionMode::EVAL && cliFile.empty()) {
      std::cerr << "Nothing to do. Either pass a hack file to run, or "
        "use -m server\n";
      return HPHP_EXIT_FAILURE;
    }

    if (po.mode == ExecutionMode::EVAL && po.args.empty()) {
      std::cerr << "Nothing to do. Pass a command to run with mode eval\n";
      return HPHP_EXIT_FAILURE;
    }

    if (Cfg::Eval::UseRemoteUnixServer != "no" &&
        !Cfg::Eval::UnixServerPath.empty() &&
        (!po.file.empty() || !po.args.empty()) && po.mode != ExecutionMode::EVAL) {
      // CLI server clients use a wacky delayed initialization scheme for
      // RDS, and therefore requires RDS_LOCALS be outside RDS.
      rds::local::init();
      SCOPE_EXIT { rds::local::fini(); };
      std::vector<std::string> args;
      if (!po.file.empty()) {
        args.emplace_back(po.file);
      }
      args.insert(args.end(), po.args.begin(), po.args.end());
      run_command_on_cli_server(
        Cfg::Eval::UnixServerPath.c_str(), args, po.count
      );
      if (Cfg::Eval::UseRemoteUnixServer == "only") {
        Logger::Error("Failed to connect to unix server, make sure HHVM is "
                      "running and the \"%s\" socket exists.",
                      Cfg::Eval::UnixServerPath.c_str());
        exit(HPHP_EXIT_FAILURE);
      }
    }

    int ret = 0;
    init_repo_file();
    hphp_process_init();
    SCOPE_EXIT { hphp_process_exit(); };

    block_sync_signals_and_start_handler_thread();

    std::string file;
    if (new_argc > 0) {
      file = new_argv[0];
    }

    if (po.mode == ExecutionMode::DEBUG || po.mode == ExecutionMode::NOTEBOOK) {
      StackTraceNoHeap::AddExtraLogging("IsDebugger", "True");
      Cfg::Debugger::EnableHphpd = true;
      po.debugger_options.isNotebook = po.mode == ExecutionMode::NOTEBOOK;
      po.debugger_options.fileName = file;
      po.debugger_options.user = po.user;
      Eval::DebuggerProxyPtr localProxy =
        Eval::Debugger::StartClient(po.debugger_options);
      if (!localProxy) {
        Logger::Error("Failed to start debugger client\n\n");
        return HPHP_EXIT_FAILURE;
      }
      Eval::Debugger::RegisterSandbox(localProxy->getDummyInfo());
      std::shared_ptr<std::vector<std::string>> client_args;
      bool restart = false;
      ret = 0;
      while (true) {
        try {
          assertx(po.debugger_options.fileName == file);
          execute_command_line_begin(new_argc, new_argv);
          // Set the proxy for this thread to be the localProxy we just
          // created. If we're script debugging, this will be the proxy that
          // does all of our work. If we're remote debugging, this proxy will
          // go unused until we finally stop it when the user quits the
          // debugger.
          g_context->setSandboxId(localProxy->getDummyInfo().id());
          if (restart) {
            // Systemlib.php is not loaded again, so we need this if we
            // are to hit any breakpoints in systemlib.
            proxySetBreakPoints(localProxy.get());
          }
          Eval::Debugger::DebuggerSession(po.debugger_options, restart);
          restart = false;
          execute_command_line_end(true, file.c_str());
        } catch (const Eval::DebuggerRestartException& e) {
          execute_command_line_end(false, nullptr);

          if (!e.m_args->empty()) {
            file = e.m_args->at(0);
            po.debugger_options.fileName = file;
            client_args = e.m_args;
            free(new_argv);
            prepare_args(new_argc, new_argv, *client_args, nullptr);
          }
          restart = true;
        } catch (const Eval::DebuggerClientExitException&) {
          execute_command_line_end(false, nullptr);
          break; // end user quitting debugger
        }
      }

    } else {

      tracing::Request _{
        "cli-request",
        file,
        [&] { return tracing::Props{}.add("file", file); }
      };

      ret = 0;

      for (int i = 0; i < po.count; i++) {
        execute_command_line_begin(new_argc, new_argv);
        ret = 1;
        if (po.mode == ExecutionMode::EVAL) {
          OptString code{"<?hh " + file};
          auto const r = g_context->evalPHPDebugger(code.get(), 0);
          if (!r.failed) ret = 0;
        } else if (hphp_invoke_simple(file, false /* warmup only */)) {
          ret = *rl_exit_code;
        }
        const bool last = i == po.count - 1;
        if (last && jit::tc::dumpEnabled()) {
          jit::mcgen::joinWorkerThreads();
          jit::tc::dump();
        }
        execute_command_line_end(true, file.c_str());

        if (!last) {
          // If we're running an unit test with multiple runs, provide
          // a separator between the runs.
          if (auto const sep = getenv("HHVM_MULTI_COUNT_SEP")) {
            fflush(stderr);
            printf("%s", sep);
            fflush(stdout);
          }
        }
        if (Cfg::Eval::FreeRandomUnits) {
          auto units = loadedUnitsNonRepoAuth();
          for (auto const unit : units) {
            if (unit->isSystemLib()) continue;
            invalidateUnit(const_cast<StringData*>(unit->origFilepath()));
          }
          low_malloc(sizeof(Func) + (sizeof(jit::AtomicLowTCA) * (i + 1)));
        }
      }
    }

    free(new_argv);

    return ret;
  }

  if (po.mode == ExecutionMode::DAEMON || po.mode == ExecutionMode::SERVER) {
    if (!po.user.empty()) Cfg::Server::User = po.user;
    return start_server(Cfg::Server::User);
  }

  if (po.mode == ExecutionMode::REPLAY && !po.args.empty()) {
      Cfg::Debug::RecordInput = false;
      set_execution_mode(ExecutionMode::SERVER);

      HttpServer server; // so we initialize runtime properly
      HttpRequestHandler handler(0);

      for (int i = 0; i < po.count; i++) {
          for (unsigned int j = 0; j < po.args.size(); j++) {
              ReplayTransport rt;
              rt.replayInput(po.args[j].c_str());
              handler.run(&rt);
              printf("%s\n", rt.getResponse().c_str());
          }
      }
      return 0;
  }

  if (po.mode == ExecutionMode::TRANSLATE && !po.args.empty()) {
      printf("%s", translate_stack(po.args[0].c_str()).c_str());
      return 0;
  }

  cout << desc << "\n";
  return HPHP_EXIT_FAILURE;
}

OptString canonicalize_path(const OptString& p, const char* root, int rootLen) {
  OptString path = FileUtil::canonicalize(p);
  if (path.charAt(0) == '/') {
    auto const& sourceRoot = Cfg::Server::SourceRoot;
    int len = sourceRoot.size();
    if (len && strncmp(path.data(), sourceRoot.c_str(), len) == 0) {
      return path.substr(len);
    }
    if (root && rootLen && strncmp(path.data(), root, rootLen) == 0) {
      return path.substr(rootLen);
    }
  }
  return path;
}

namespace {
  std::string get_section_name(const std::string& path) {
    std::string section("ext.");
    return section + HHVM_FN(md5)(std::string(path), false).substr(0, 12).data();
  }
}

// Retrieve a embed section of the HHVM binary
std::string get_embedded_section(const std::string& path) {
  auto section_name = get_section_name(path);

  embedded_data desc;
  if (!get_embedded_data(section_name.c_str(), &desc)) return "";
  return read_embedded_data(desc);
}

///////////////////////////////////////////////////////////////////////////////
// C++ ffi

namespace {

DEBUG_ONLY std::atomic<bool> s_process_exited = false;

void on_timeout(int sig, siginfo_t* info, void* /*context*/) {
  if (sig == SIGVTALRM && info && info->si_code == SI_TIMER) {
    auto data = (RequestTimer*)info->si_value.sival_ptr;
    if (data) {
      data->onTimeout();
    }
  }
}

}

/*
 * Update constants to their real values and sync some runtime options
 */
static void update_constants_and_options() {
  assertx(ExtensionRegistry::modulesInitialised());
  // If extension constants were used in the ini files (e.g., E_ALL) they
  // would have come out as 0 in the previous pass until we load and
  // initialize our extensions, which we do in RuntimeOption::Load() via
  // ExtensionRegistry::ModuleLoad() and in ExtensionRegistry::ModuleInit()
  // in hphp_process_init(). We will re-import and set only the constants that
  // have been now bound to their proper value.
  IniSettingMap ini = IniSettingMap();
  for (auto& filename: s_config_files) {
    Config::ParseIniFile(filename, ini, true);
  }
  // Reset the INI settings from the CLI.
  for (auto& iniStr: s_ini_strings) {
    Config::ParseIniString(iniStr, ini, true);
  }

  // Reset, possibly, some request dependent runtime options based on certain
  // setting values. Do this here so we ensure the constants have been loaded
  // correctly (e.g., error_reporting E_ALL, etc.)
  Variant sys;
  if (IniSetting::GetSystem("error_reporting", sys)) {
    RuntimeOption::RuntimeErrorReportingLevel = sys.toInt64();
    RID().setErrorReportingLevel(RuntimeOption::RuntimeErrorReportingLevel);
  }
  if (IniSetting::GetSystem("memory_limit", sys)) {
    RID().setMemoryLimit(sys.toString().toCppString());
    Cfg::Server::RequestMemoryMaxBytes = RID().getMemoryLimitNumeric();
  }
}

void hphp_thread_init(bool skipExtensions /* = false */,
                      bool skipRDSInit /* = false */) {
  init_current_pthread_stack_limits();
#if USE_JEMALLOC
  arenas_thread_init();
#endif
  if (!skipRDSInit) rds::threadInit();
  ServerStats::GetLogger();
  zend_get_bigint_data();
  zend_rand_init();
  get_server_note();
  tl_heap.getCheck()->init();

  assertx(RequestInfo::s_requestInfo.isNull());
  RequestInfo::s_requestInfo.getCheck()->threadInit();

  HardwareCounter::s_counter.getCheck();
  if (!skipExtensions) ExtensionRegistry::threadInit();
  InitFiniNode::ThreadInit();

  // Ensure that there's no request-allocated memory. This call must happen at
  // least once after RDS has been initialized to ensure
  // MemoryManager::resetGC() sets a proper trigger threshold.
  hphp_memory_cleanup();
}

void hphp_thread_exit(bool skipExtensions /* = false */) {
  // All threads should have already exited before process exit
  assertx(!s_process_exited);

  InitFiniNode::ThreadFini();
  if (!skipExtensions) ExtensionRegistry::threadShutdown();
  if (!g_context.isNull()) g_context.destroy();
  rds::threadExit();
#if USE_JEMALLOC
  arenas_thread_exit();
#endif
}

void cli_client_init() {
  if (*s_sessionInitialized) return;
  Process::InitProcessStatics();
  rds::processInit();
  rds::threadInit();
  ServerStats::GetLogger();
  zend_rand_init();
  get_server_note();
  assertx(RequestInfo::s_requestInfo.isNull());
  RequestInfo::s_requestInfo.getCheck()->threadInit();
  HardwareCounter::s_counter.getCheck();
  InitFiniNode::ThreadInit();
  hphp_memory_cleanup();
  g_context.getCheck();
  if (!registrationComplete) {
    folly::SingletonVault::singleton()->registrationComplete();
    registrationComplete = true;
  }
  AsioSession::Init();
  Socket::clearLastError();
  RI().onSessionInit(RequestId::allocate());
  tl_heap->resetExternalStats();
  g_thread_safe_locale_handler->reset();
  Treadmill::startRequest(Treadmill::SessionKind::CLIServer);
  *s_sessionInitialized = true;
}

void cli_client_thread_init() {
  if (*s_sessionInitialized) return;
  g_context.getCheck();
  AsioSession::Init();
  Socket::clearLastError();
  RI().onSessionInit(RequestId::allocate());
  tl_heap->resetExternalStats();
  g_thread_safe_locale_handler->reset();
  Treadmill::startRequest(Treadmill::SessionKind::CLIServer);
  *s_sessionInitialized = true;
}

void init_current_pthread_stack_limits() {
  pthread_attr_t attr;
// Linux+GNU extension
#if defined(_GNU_SOURCE)
  if (pthread_getattr_np(pthread_self(), &attr) != 0 ) {
    Logger::Error("pthread_getattr_np failed before checking stack limits");
    _exit(HPHP_EXIT_FAILURE);
  }
#else
  if (pthread_attr_init(&attr) != 0 ) {
    Logger::Error("pthread_attr_init failed before checking stack limits");
    _exit(HPHP_EXIT_FAILURE);
  }
#endif
  init_stack_limits(&attr);
  if (pthread_attr_destroy(&attr) != 0 ) {
    Logger::Error("pthread_attr_destroy failed after checking stack limits");
    _exit(HPHP_EXIT_FAILURE);
  }
}

void hphp_process_init(bool initForWorkerProcess /* = false */,
                       bool skipRDSInit /* = false */) {
  BootStats::markStart("pthread_init");
  init_current_pthread_stack_limits();
  BootStats::mark("pthread_init");

  BootStats::markStart("Process::InitProcessStatics");
  // Freeze cache-layer registration and install the dispatcher.
  // Extensions register layers during moduleLoad() (called from
  // RuntimeOption::Load); this call finalizes the dispatch chain.
  initUnitEmitterCacheDispatcher();

  Process::InitProcessStatics();
  BootStats::mark("Process::InitProcessStatics");

  // initialize the tzinfo cache.
  BootStats::markStart("timezone_init");
  timezone_init();
  BootStats::mark("timezone_init");

  BootStats::markStart("xenon");
  if (!skipRDSInit) rds::processInit();

  hphp_thread_init(/* skipExtensions= */ initForWorkerProcess, skipRDSInit);

  struct sigaction action = {};
  action.sa_sigaction = on_timeout;
  action.sa_flags = SA_SIGINFO | SA_NODEFER | SA_RESTART;
  sigaction(SIGVTALRM, &action, nullptr);

  // initialize Xenon profiler
  Xenon::getInstance().start();
  BootStats::mark("xenon");

  // set up strobelight signal handling
  BootStats::markStart("strobelight");
  Strobelight::getInstance().init();
  BootStats::mark("strobelight");

  // reinitialize pcre table
  BootStats::markStart("pcre_reinit");
  pcre_reinit();
  BootStats::mark("pcre_reinit");

  // the liboniguruma docs say this isnt needed,
  // but the implementation of init is not
  // thread safe due to bugs
  BootStats::markStart("onig_init");
  onig_init();
  BootStats::mark("onig_init");

  g_context.getCheck();
  // Some event handlers are registered during the startup process.
  g_context->acceptRequestEventHandlers(true);
  if (!registrationComplete) {
    folly::SingletonVault::singleton()->registrationComplete();
    registrationComplete = true;
  }
  InitFiniNode::ProcessPreInit();
  // TODO(9795696): Race in thread map may trigger spurious logging at
  // thread exit, so for now, only spawn threads if we're a server.
  const uint32_t maxWorkers = Cfg::Server::Mode ? 3 : 0;
  InitFiniNode::ProcessInitConcurrentStart(maxWorkers);
  SCOPE_EXIT {
    BootStats::markStart("extra_process_init_concurrent_wait");
    InitFiniNode::ProcessInitConcurrentWaitForEnd();
    BootStats::mark("extra_process_init_concurrent_wait");
  };

  ImplicitContext::activeCtx
    .bind(rds::Mode::Normal, rds::LinkID{"ImplicitContext::activeCtx"});
  ImplicitContext::emptyCtx
    .bind(rds::Mode::Normal, rds::LinkID{"ImplicitContext::emptyCtx"});

  if (Cfg::Debugger::EnableVSDebugger && Cfg::Eval::EmitDebuggerIntrCheck) {
    DebuggerHook::s_exceptionBreakpointIntr
      .bind(rds::Mode::Normal, rds::LinkID{"ExceptionBreakpointIntr"});
  }

  if (!initForWorkerProcess) {
    jit::mcgen::processInit();
    jit::processInitProfData();
    jit::processInitTransStats();
  }
  if (Cfg::Eval::EnableDecl) {
    BootStats::markStart("s_builtin_symbols_populated");
    if (!initForWorkerProcess) {
      ExtensionRegistry::moduleDeclInit();
    }
    BootStats::mark("s_builtin_symbols_populated");
  }
  BootStats::markStart("g_vmProcessInit");
  g_vmProcessInit();
  BootStats::mark("g_vmProcessInit");

  BootStats::markStart("PageletServer::Restart");
  PageletServer::Restart();
  BootStats::mark("PageletServer::Restart");
  BootStats::markStart("XboxServer::Restart");
  XboxServer::Restart();
  BootStats::mark("XboxServer::Restart");
  BootStats::markStart("Stream::RegisterCoreWrappers");
  Stream::RegisterCoreWrappers();
  BootStats::mark("Stream::RegisterCoreWrappers");
  if (!initForWorkerProcess) {
    BootStats::markStart("ExtensionRegistry::moduleInit");
    ExtensionRegistry::moduleInit();
    BootStats::mark("ExtensionRegistry::moduleInit");
  }

  if (!RuntimeOption::DeploymentId.empty()) {
    StackTraceNoHeap::AddExtraLogging(
      "DeploymentId", RuntimeOption::DeploymentId);
  }

  if (!initForWorkerProcess) {
    // Now that constants have been bound we can update options using constants
    // in ini files (e.g., E_ALL) and sync some other options
    update_constants_and_options();
  }

  BootStats::markStart("extra_process_init");
  InitFiniNode::ProcessInit();
  BootStats::mark("extra_process_init");

  StaticContentCache::load();

  if (!initForWorkerProcess && jit::cprof::contProfStartupActive()) {
    InitFiniNode::ProcessInitConcurrentWaitForEnd();
    jit::cprof::consumeContProfAtStartup();
  }

  if (Cfg::Repo::Authoritative &&
      !Cfg::Jit::SerdesFile.empty()) {
    auto const mode = RuntimeOption::EvalJitSerdesMode;
    auto const numWorkers = Cfg::Jit::WorkerThreadsForSerdes ?
        Cfg::Jit::WorkerThreadsForSerdes : Process::GetCPUCount();

    auto const deserialize = [&] (auto const& f) {
#if USE_JEMALLOC
      auto const numArenas =
        std::min(Cfg::Jit::WorkerArenas,
                 std::max(Cfg::Jit::WorkerThreads, numWorkers));
      setup_extra_arenas(numArenas);
#endif
      return f(
        Cfg::Jit::SerdesFile,
        Cfg::Jit::ParallelDeserialize ? numWorkers : 1,
        false
      );
    };

    auto const rta = [&] (const std::string& mark, bool skipSerialize) {
      BootStats::mark(mark);
      BootStats::set("prof_data_source_host",
                     jit::ProfData::buildHost()->toCppString());
      BootStats::set("prof_data_timestamp", jit::ProfData::buildTime());
      Cfg::Jit::ProfileBCSize = 0;
      Cfg::Jit::WorkerThreads = numWorkers;
      // Run retranslateAll asynchronously, without waiting for it to finish
      // here.
      jit::mcgen::checkRetranslateAll(true, skipSerialize);
    };

    auto const tryPartialDeserialize = [&] {
      if (!isJitSerializing()) return;
      if (!jit::serializeOptProfEnabled()) return;
      if (!Cfg::Jit::SerializeOptProfRestart) return;

      if (Cfg::Server::Mode) {
        Logger::FInfo("Attempting to deserialize partial profile-data file: {}",
                      Cfg::Jit::SerdesFile);
      }

      auto const success = deserialize(jit::tryDeserializePartialProfData);
      if (success) {
        if (Cfg::Server::Mode) {
          Logger::FInfo("Successfully deserialized partial profile-data file. "
                        "Loaded {} units with {} workers",
                        numLoadedUnits(), numWorkers);
        }
        rta("jit::tryDeserializePartialProfData", true);
      } else if (Cfg::Server::Mode) {
        Logger::FInfo("Failed deserializing partial profile-data file. "
                      "Proceeding normally");
      }
    };

    if (isJitDeserializing()) {
      if (Cfg::Server::Mode) {
        Logger::FInfo("JitDeserializeFrom: {}",
                      Cfg::Jit::SerdesFile);
      }

      auto const errMsg = deserialize(jit::deserializeProfData);

      if (mode == JitSerdesMode::DeserializeAndDelete) {
        // Delete the serialized profile data when we finish reading
        if (Cfg::Server::Mode) {
          Logger::FInfo("Deleting serialized profile-data file: {}",
                        Cfg::Jit::SerdesFile);
        }
        unlink(Cfg::Jit::SerdesFile.c_str());
      }

      if (errMsg.empty()) {
        if (Cfg::Server::Mode) {
          Logger::FInfo("JitDeserialize: Loaded {} Units with {} workers",
                        numLoadedUnits(), numWorkers);
        }
        if (mode != JitSerdesMode::DeserializeForPreload) {
          rta("jit::deserializeProfData", false);
        }
        if (mode == JitSerdesMode::DeserializeAndExit) {
          if (Cfg::Server::Mode) {
            Logger::Info("JitDeserialize finished; exiting");
          }
          if (jit::tc::dumpEnabled()) {
            jit::mcgen::joinWorkerThreads();
            jit::tc::dump();
          }
          hphp_process_exit();
          exit(0);
        }
      } else {                          // failed to deserialize
        if (mode == JitSerdesMode::DeserializeOrFail ||
            mode == JitSerdesMode::DeserializeAndExit) {
          Logger::Error(errMsg);
          hphp_process_exit();
          exit(HPHP_EXIT_FAILURE);
        }
        if (mode == JitSerdesMode::DeserializeOrGenerate) {
          Logger::Info(errMsg +
                       ", scheduling one time serialization and restart");
          RuntimeOption::EvalJitSerdesMode = JitSerdesMode::SerializeAndExit;
          tryPartialDeserialize();
        } else {
          Logger::Info(errMsg + ", will profile then retranslateAll");
        }
      }
    } else {
      // We're not deserializing. Check if we're serializing and if we
      // have a partial file. If so, deserialize it and do a RTA.
      tryPartialDeserialize();
    }
  }

  rds::requestExit();
  BootStats::mark("rds::requestExit");
  // Reset the preloaded g_context
  ExecutionContext *context = g_context.getNoCheck();
  context->onRequestShutdown(); // TODO T20898959 kill early REH usage.
  context->~ExecutionContext();
  new (context) ExecutionContext();
  BootStats::mark("ExecutionContext");
}

static void handle_exception(bool& ret, ExecutionContext* context,
                             std::string& errorMsg, ContextOfException where,
                             bool& error, bool richErrorMsg) {
  assertx(where == ContextOfException::Invoke ||
         where == ContextOfException::ReqInit);
  try {
    handle_exception_helper(ret, context, errorMsg, where, error, richErrorMsg);
  } catch (const ExitException&) {
    // Got an ExitException during exception handling, handle
    // similarly to the case below but don't call obEndAll().
  } catch (...) {
    handle_exception_helper(ret, context, errorMsg, ContextOfException::Handler,
                            error, richErrorMsg);
    context->obEndAll();
  }
}

static void handle_reqinit_exception(bool &ret, ExecutionContext *context,
                                     std::string &errorMsg, bool &error) {
  handle_exception(ret, context, errorMsg, ContextOfException::ReqInit, error,
                   false);
}

static void handle_invoke_exception(bool &ret, ExecutionContext *context,
                                    std::string &errorMsg, bool &error,
                                    bool richErrorMsg) {
  handle_exception(ret, context, errorMsg, ContextOfException::Invoke, error,
                   richErrorMsg);
}

const std::string s_phpRootVar("${SOURCE_ROOT}/");

void invoke_prelude_script(
  const char* currentDir,
  const std::string& document,
  const std::string& prelude
) {
  const auto run_file = [currentDir] (const OptString& f) {
    auto const w = Stream::getWrapperFromURI(f, nullptr, false);
    if (w->access(f, R_OK) == 0) {
      include_impl_invoke(f, true, currentDir, true);
      return true;
    }
    return false;
  };

  if (UNLIKELY(!prelude.starts_with(s_phpRootVar))) {
    if(!FileUtil::runRelative(
      prelude,
      OptString(document, CopyString),
      currentDir,
      run_file
    )) {
      Logger::Error("Failed to read prelude file at %s.", prelude.data());
    };
    return;
  }

  const auto repo_path = RepoOptions::forFile(document.c_str()).dir();
  const auto path = repo_path / prelude.substr(s_phpRootVar.length());
  if (!run_file(OptString(path.native()))) {
    Logger::Error(
      "Failed to read prelude file at %s. "
      "Ensure that %s is relative to the folder containing .hhvmconfig.hdf",
      path.c_str(), s_phpRootVar.data());
  }
}

static bool hphp_warmup(ExecutionContext *context,
                        const std::string& cmd,
                        const std::string &reqInitFunc,
                        const std::string &reqInitDoc,
                        const std::string &prelude,
                        bool &error,
                        bool runEntryPoint) {
  tracing::Block _{
    "warmup",
    [&] {
      return tracing::Props{}
        .add("cmd", cmd)
        .add("req_init_doc", reqInitDoc)
        .add("req_init_func", reqInitFunc)
        .add("prelude", prelude);
    }
  };

  bool ret = true;
  error = false;
  std::string errorMsg;

  ServerStatsHelper ssh("reqinit");
  try {
    if (!prelude.empty() && (!cmd.empty() || !reqInitDoc.empty())) {
      auto const currentDir = context->getCwd();
      auto const& document = !reqInitDoc.empty() ? reqInitDoc : cmd;
      invoke_prelude_script(currentDir.data(), document, prelude);
    }

    if (!reqInitDoc.empty()) {
      include_impl_invoke(reqInitDoc, true, "", runEntryPoint);
    }
    if (!reqInitFunc.empty()) {
      invoke(reqInitFunc, Array());
    }
    context->backupSession();
  } catch (...) {
    handle_reqinit_exception(ret, context, errorMsg, error);
  }

  return ret;
}

void hphp_session_init(Treadmill::SessionKind session_kind,
                       Transport* transport,
                       RequestId id,
                       RequestId root_req_id) {
  if (id.unallocated()) id = RequestId::allocate();
  assertx(!*s_sessionInitialized);
  MemoryManager::requestInit();
  g_context.getCheck();
  AsioSession::Init();
  Socket::clearLastError();
  RI().onSessionInit(id, root_req_id);
  tl_heap->resetExternalStats();
  unitCacheClearSync();

  g_thread_safe_locale_handler->reset();
  Treadmill::startRequest(session_kind);

  // Allow request event handlers to be created now that a new request has
  // started.
  g_context->acceptRequestEventHandlers(true);

  g_context->requestInit();             // must happen after treadmill start
  if (transport != nullptr) g_context->setTransport(transport);
  *s_sessionInitialized = true;

  ExtensionRegistry::requestInit();

  // Sample function calls for this request
  if (RID().logFunctionCalls()) {
    EventHook::Enable();
  }

  auto const pme_freq = Cfg::Eval::PerfMemEventRequestFreq;
  if (pme_freq > 0 && folly::Random::rand32(pme_freq) == 0) {
    // Enable memory access sampling for this request.
    perf_event_enable(
      Cfg::Eval::PerfMemEventSampleFreq,
      [] (PerfEvent) { stackLimitAndSurprise().setFlag(PendingPerfEventFlag); }
    );
  }
}

bool hphp_invoke_simple(const std::string& filename, bool warmupOnly) {
  bool error;
  std::string errorMsg;
  return hphp_invoke(g_context.getNoCheck(), filename, false, null_array,
                     nullptr, "", "", error, errorMsg,
                     true /* once */,
                     warmupOnly,
                     false /* richErrorMsg */,
                     Cfg::Eval::PreludePath);
}

bool hphp_invoke(ExecutionContext *context, const std::string &cmd,
                 bool func, const Array& funcParams, Variant* funcRet,
                 const std::string &reqInitFunc, const std::string &reqInitDoc,
                 bool &error, std::string &errorMsg,
                 bool once, bool warmupOnly,
                 bool richErrorMsg, const std::string& prelude,
                 bool allowDynCallNoPointer /* = false */) {
  tracing::Block _{"invoke", [&] { return tracing::Props{}.add("cmd", cmd); }};

  bool isServer = !is_any_cli_mode();
  error = false;

  // Make sure we have the right current working directory within the repo
  // based on what server.source_root was set to (current process directory
  // being the default)
  if (Cfg::Repo::Authoritative) {
    context->setCwd(Cfg::Server::SourceRoot);
  }

  OptString oldCwd;
  if (isServer) {
    oldCwd = context->getCwd();
  }
  if (!hphp_warmup(context, cmd, reqInitFunc, reqInitDoc, prelude, error,
                   func)) {
    if (isServer) context->setCwd(oldCwd);
    return false;
  }

  tl_heap->resetCouldOOM(isStandardRequest());
  RID().resetTimers();

  bool ret = true;
  if (!warmupOnly) {
    try {
      ServerStatsHelper ssh("invoke");
      if (func) {
        auto const ret = invoke(cmd, funcParams, allowDynCallNoPointer);
        if (funcRet) *funcRet = ret;
      } else {
        if (isServer) hphp_chdir_file(cmd);
        include_impl_invoke(cmd.c_str(), once, "", true);
      }
    } catch (...) {
      handle_invoke_exception(ret, context, errorMsg, error, richErrorMsg);
    }
  }

  try {
    context->onShutdownPreSend();
  } catch (...) {
    handle_invoke_exception(ret, context, errorMsg, error, richErrorMsg);
  }

  if (isServer) context->setCwd(oldCwd);
  return ret;
}

void hphp_context_exit() {
  // Run shutdown handlers. This may cause user code to run.
  g_thread_safe_locale_handler->reset();

  // Reset the surprise flag checks of all the translations of all the
  // functions intercepted by the current request.  Must be performed
  // before shutting down the context as the list of intercepted functions
  // of the request is stored in the RDS.
  reset_all_intercepted_functions();

  auto const context = g_context.getNoCheck();
  context->onRequestShutdown();

  // Extensions could have shutdown handlers
  ExtensionRegistry::requestShutdown();
  InitFiniNode::RequestFini();

  // Extension shutdown could have re-initialized some
  // request locals
  context->onRequestShutdown();

  // This causes request event handler registration to fail until the next
  // request starts.
  context->acceptRequestEventHandlers(false);

  // Clean up a bunch of request state. No user code after this point.
  MemoryManager::setExiting();
  context->requestExit();
  context->obProtect(false);
  context->obEndAll();
}

void hphp_memory_cleanup() {
  MemoryManager::requestShutdown();
  auto& mm = *tl_heap;
  // sweep functions are allowed to access g_context,
  // so we can't destroy it yet
  mm.sweep();

  // Freeing hazard pointers can enqueue APCHandles into g_context.
  APCTypedValue::FreeHazardPointers();

  // We should never have any registered RequestEventHandlers. If we do
  // something after onRequestShutdown registered a RequestEventHandler.
  // Its now too late to run the requestShutdown functions, but if we carry
  // on, requestInit and requestShutdown will never be called again.
  // I considered just clearing the inited flags; which works for some
  // RequestEventHandlers - but its a disaster for others. So just fail hard
  // here.
  always_assert(g_context.isNull() || !g_context->hasRequestEventHandlers());

  // g_context is request allocated, and has some members that need
  // cleanup, so destroy it before its too late
  g_context.destroy();

  weakref_cleanup();
  mm.resetAllocator();
  mm.resetCouldOOM();
}

void cli_client_thread_exit() {
  assertx(*s_sessionInitialized);

  g_thread_safe_locale_handler->reset();
  Treadmill::finishRequest();
  RI().onSessionExit();
  hphp_memory_cleanup();
  assertx(tl_heap->empty());

  *s_sessionInitialized = false;
}

void hphp_session_exit() {
  assertx(*s_sessionInitialized);
  // Server note and INI have to live long enough for the access log to fire.
  // RequestLocal is too early.
  ServerNote::Reset();
  IniSetting::ResetSavedDefaults();

  if (Cfg::Jit::WaitForAsyncJITBetweenRequests &&
      Treadmill::sessionKind() == Treadmill::SessionKind::CLISession) {
    jit::mcgen::waitForAsyncTranslationWorkerThreadsToEmpty();
  }

  // In JitPGO mode, check if it's time to schedule the retranslation of all
  // profiled functions and, if so, schedule it.
  jit::mcgen::checkRetranslateAll();
  jit::mcgen::checkSerializeOptProf();
  jit::tc::requestExit();

  if (Cfg::Eval::IdleUnitTimeoutSecs > 0 && !Cfg::Repo::Authoritative) {
    // Update the timestamp of any Unit we touched in this request. We
    // defer this until the end of the request to prevent Units from
    // being considered "expired" while a request is still using it.
    auto const now = Unit::TouchClock::now();
    for (auto const u : g_context->m_touchedUnits) u->setLastTouchTime(now);
  } else {
    assertx(g_context->m_touchedUnits.empty());
  }

  // Similarly, apc strings could be in the ServerNote array, and
  // it's possible they are scheduled to be destroyed after this request
  // finishes.
  Treadmill::finishRequest();

  RI().onSessionExit();

  // We might have events from after the final surprise flag check of the
  // request, so consume them here.
  perf_event_consume(record_perf_mem_event);
  perf_event_disable();

  hphp_memory_cleanup();
  assertx(tl_heap->empty());

  *s_sessionInitialized = false;
  s_extra_request_nanoseconds = 0;
}

void hphp_process_exit() noexcept {
  // We want to do clean up on a best-effort basis: don't skip later steps if
  // an earlier step fails, and don't propagate exceptions ouf of this function
#define LOG_AND_IGNORE(voidexpr) try { voidexpr; } catch (...) { \
    Logger::Error("got exception in cleanup step: " #voidexpr); }
  LOG_AND_IGNORE(jit::logGlobalTransStats())
  LOG_AND_IGNORE(teardown_cli_server())
  LOG_AND_IGNORE(Xenon::getInstance().stop())
  LOG_AND_IGNORE(jit::mcgen::joinWorkerThreads())
  LOG_AND_IGNORE(jit::tc::processExit())
  LOG_AND_IGNORE(bespoke::waitOnExportProfiles())
  LOG_AND_IGNORE(PageletServer::Stop())
  LOG_AND_IGNORE(XboxServer::Stop())
  // Debugger::Stop() needs an execution context
  LOG_AND_IGNORE(g_context.getCheck())
  LOG_AND_IGNORE(Eval::Debugger::Stop())
  LOG_AND_IGNORE(jit::mcgen::joinAsyncTranslationWorkerThreads())
  LOG_AND_IGNORE(g_context.destroy())
  LOG_AND_IGNORE(shutdownUnitPrefetcher());
  LOG_AND_IGNORE(shutdownUnitReaper());
  LOG_AND_IGNORE(Strobelight::shutdown())
  LOG_AND_IGNORE(ExtensionRegistry::moduleShutdown())
  LOG_AND_IGNORE(LightProcess::Close())
  LOG_AND_IGNORE(InitFiniNode::ProcessFini())
  LOG_AND_IGNORE(folly::SingletonVault::singleton()->destroyInstances())
  LOG_AND_IGNORE(embedded_data_cleanup())
  LOG_AND_IGNORE(Debug::destroyDebugInfo())
  LOG_AND_IGNORE(clearUnitCacheForExit())
#undef LOG_AND_IGNORE

#ifndef NDEBUG
  s_process_exited = true;
#endif
}

bool is_hphp_session_initialized() {
  return *s_sessionInitialized;
}

static struct SetThreadInitFini {
  template<class ThreadT> static typename std::enable_if<
    std::is_integral<ThreadT>::value || std::is_pointer<ThreadT>::value>::type
  recordThreadAddr(ThreadT threadId, char* stackAddr, size_t stackSize) {
    // In the current glibc implementation, pthread_t is a 64-bit unsigned
    // integer, whose value equals the address of the thread control block
    // (TCB).  In x64_64, this is right above the TLS block.  In addition,
    // TLS and TCB sits at the high end of the stack, i.e.,
    //
    // stackAddr + stackSize ----> +---------------+
    //                             | TCB           |
    //              threadId ----> +---------------+
    //                             | TLS           |
    //                             +---------------+
    //                             | Stack         |
    //                             .               .
    //                             .               .
    //             stackAddr ----> +---------------+
    auto const tcbBase = reinterpret_cast<char*>(threadId);
    auto stackEnd = stackAddr + stackSize;
    if (tcbBase > stackAddr && tcbBase < stackEnd) { // the expected layout
      // TCB
      Debug::DebugInfo::recordDataMap(
        tcbBase, stackEnd,
        fmt::format("Thread-{}", static_cast<void*>(tcbBase)));
      // TLS
      auto const tlsRange = getCppTdata();
      auto const tlsSize = (tlsRange.second + 15) / 16 * 16;
      stackEnd = tcbBase - tlsSize;
      if (tlsSize) {
        Debug::DebugInfo::recordDataMap(
          stackEnd, tcbBase,
          fmt::format("TLS-{}", static_cast<void*>(tcbBase)));
      }
    }
    Debug::DebugInfo::recordDataMap(
      stackAddr, stackEnd,
      fmt::format("Stack-{}", static_cast<void*>(tcbBase)));
  }
  template <class ThreadT>
  static typename std::enable_if<!std::is_integral<ThreadT>::value &&
                                 !std::is_pointer<ThreadT>::value>::type
  recordThreadAddr(ThreadT /*threadId*/, char* stackAddr, size_t stackSize) {
    // pthread_t is not an integer or pointer to TCB in this pthread
    // implementation.  But we can still figure out where TLS is.
    auto const tlsRange = getCppTdata();
    auto const tlsSize = (tlsRange.second + 15) / 16 * 16;
    auto const tlsBaseAddr = reinterpret_cast<char*>(tlsBase());
    Debug::DebugInfo::recordDataMap(
      tlsBaseAddr, tlsBaseAddr + tlsSize,
      fmt::format("TLS-{}", static_cast<void*>(stackAddr)));
    Debug::DebugInfo::recordDataMap(
      stackAddr, stackAddr + stackSize,
      fmt::format("Stack-{}", static_cast<void*>(stackAddr)));
  }

  SetThreadInitFini() {
    AsyncFuncImpl::SetThreadInitFunc(
      [] (void*) {
#if defined(_GNU_SOURCE)
        if (Cfg::Eval::PerfDataMap) {
          pthread_t threadId = pthread_self();
          pthread_attr_t attr;
          pthread_getattr_np(threadId, &attr);
          void* stackAddr{nullptr};
          size_t stackSize{0};
          pthread_attr_getstack(&attr, &stackAddr, &stackSize);
          pthread_attr_destroy(&attr);
          recordThreadAddr(threadId, static_cast<char*>(stackAddr), stackSize);
        }
#endif
        hphp_thread_init();
      },
      nullptr);
    AsyncFuncImpl::SetThreadFiniFunc([](void*) { hphp_thread_exit(); },
                                     nullptr);
  }
} s_SetThreadInitFini;

///////////////////////////////////////////////////////////////////////////////
}
