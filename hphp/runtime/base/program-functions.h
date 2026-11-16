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

#pragma once

#include "hphp/runtime/base/request-id.h"
#include "hphp/runtime/base/types.h"
#include "hphp/runtime/vm/treadmill.h"
#include "hphp/util/optional.h"

#include <boost/program_options/parsers.hpp>

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

struct Transport;
struct Unit;

/**
 * Main entry point of the entire program.
 */
int execute_program(int argc, char **argv);
void execute_command_line_begin(int argc, char **argv);
void execute_command_line_end(bool coverage, const char *program,
                              bool runCleanup = true);

void init_command_line_session(int arc, char** argv);
void init_command_line_globals(
  int argc, char** argv, char** envp,
  const std::map<std::string, std::string>& serverVariables,
  const std::map<std::string, std::string>& envVariables
);

/**
 * Set up REQUEST_TIME_NS field in $_SERVER.
 */
void init_server_request_time(Array& server);

/**
 * Setting up environment variables.
 */
void process_env_variables(Array& variables);

/**
 * Inserting a variable into specified symbol table.
 *
 * "overwrite" parameter is only for cookies:
 * According to rfc2965, more specific paths are listed above the less
 * specific ones. If we encounter a duplicate cookie name, we should
 * skip it, since it is not possible to have the same (plain text)
 * cookie name for the same path and we should not overwrite more
 * specific cookies with the less specific ones.
 */
void register_variable(Array& variables,
                       char* name,
                       const Variant& value,
                       bool overwrite = true);

OptString canonicalize_path(const OptString& path, const char* root, int rootLen);

/**
 * Translate hex encode stack into both C++ and PHP frames.
 */
std::string translate_stack(const char *hexencoded,
                            bool with_frame_numbers = true);

time_t start_time();

///////////////////////////////////////////////////////////////////////////////

struct ExecutionContext;

/*
 * If `initAsWorker` is set, JIT data structure and extensions will not be
 * initialized as they're expensive to set up and unused by workers.
 */
void hphp_process_init(bool initForWorkerProcess = false,
                       bool skipRDSInit = false);
void cli_client_init();
void cli_client_thread_init();
void cli_client_thread_exit();
void hphp_session_init(Treadmill::SessionKind session_kind,
                       Transport* transport = nullptr,
                       RequestId id = RequestId(),
                       RequestId root_req_id = RequestId());

void invoke_prelude_script(
     const char* currentDir,
     const std::string& document,
     const std::string& prelude);
bool hphp_invoke_simple(const std::string& filename, bool warmupOnly);
bool hphp_invoke(ExecutionContext *context,
                 const std::string &cmd,
                 bool func,
                 const Array& funcParams,
                 Variant* funcRet,
                 const std::string &reqInitFunc,
                 const std::string &reqInitDoc,
                 bool &error,
                 std::string &errorMsg,
                 bool once,
                 bool warmupOnly,
                 bool richErrorMsg,
                 const std::string& prelude,
                 bool allowDynCallNoPointer = false);
void hphp_context_exit();

void hphp_thread_init(bool skipExtensions = false,
                      bool skipRDSInit = false);
void hphp_thread_exit(bool skipExtensions = false);

void init_current_pthread_stack_limits();

void hphp_memory_cleanup();
/*
 * Tear down various internal state at the very end of a session.
 */
void hphp_session_exit();
void hphp_process_exit() noexcept;
bool is_hphp_session_initialized();

// Helper function for stats tracking with exceptions.
void bump_counter_and_rethrow(bool isPsp, ExecutionContext* context);

std::vector<int> get_executable_lines(const Unit*);

struct HphpSession {
  explicit HphpSession(Treadmill::SessionKind sk) {
    hphp_session_init(sk);
  }
  ~HphpSession() {
    hphp_context_exit();
    hphp_session_exit();
  }
  HphpSession(const HphpSession&) = delete;
  HphpSession& operator=(const HphpSession&) = delete;
};

struct HphpSessionAndThread {
  explicit HphpSessionAndThread(Treadmill::SessionKind sk) {
    hphp_thread_init();
    session.emplace(sk);
  }
  ~HphpSessionAndThread() {
    session.reset();
    hphp_thread_exit();
  }
  HphpSessionAndThread(const HphpSessionAndThread&) = delete;
  HphpSessionAndThread& operator=(const HphpSessionAndThread&) = delete;
 private:
  Optional<HphpSession> session;
};

///////////////////////////////////////////////////////////////////////////////
}

extern "C" {
void __attribute__((__weak__)) __hot_start();
void __attribute__((__weak__)) __hot_end();
}
