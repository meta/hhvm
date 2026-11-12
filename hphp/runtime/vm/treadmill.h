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

#include <stdint.h>
#include <chrono>
#include <memory>

namespace HPHP::Treadmill {

//////////////////////////////////////////////////////////////////////

/*
 * List of potentiall users of the treadmill, useful for debugging halts
 */
enum class SessionKind {
  None,
  DebuggerClient,
  PreloadRepo,
  Watchman,
  Vsdebug,
  FactsWorker,
  CLIServer,
  AdminPort,
  HttpRequest,
  RpcRequest,
  TranslateWorker,
  Retranslate,
  RetranslateAll,
  ProfData,
  UnitTests,
  CompileRepo,
  HHBBC,
  CompilerEmit,
  CompilerAnalysis,
  CLISession,
  UnitReaper
};

/*
 * Treadmill uses steady clock to track the oldest running request.
 */
using Clock = std::chrono::steady_clock;

/*
 * A special value of start time indicating either an idle request in request
 * context, or no request in flight in global context.
 */
constexpr Clock::time_point kNoStartTime = Clock::time_point{};

/*
 * Return the current thread's session kind.
 */
SessionKind sessionKind();

/*
 * Return the current thread's request index.
 */
int64_t requestIdx();

/*
 * The Treadmill allows us to defer work until all currently
 * outstanding requests have finished.  We hook request start and
 * finish to know when these events happen.
 */
void startRequest(SessionKind session_kind);
void finishRequest();

/*
 * Returns the start time of the oldest request in flight, or kNoStartTime
 * if there is none.
 */
Clock::time_point getOldestRequestStartTime();

/*
 * Returns the start time of this request.
 */
Clock::time_point getRequestStartTime();

/*
 * Ask for memory to be freed (as in free, not delete) by the next
 * appropriate treadmill round.
 */
void deferredFree(void*);

/*
 * Check if the treadmill is "stuck" by a request running far longer
 * than it should. If stuck, the process will send a SIGABRT to itself
 * to die. This check is normally done by startRequest(), but can be
 * triggered manually with this.
 */
void checkForStuckTreadmill();

/*
 * Used to get debug information about the treadmill. If forCrash is true then
 * an attempt will be made to acquire the treadmill mutex but a result will be
 * returned regardless of whether the lock was acquired.
 */
std::string dumpTreadmillInfo(bool forCrash = false);

/* TODO: Add docs for these requests */
std::string dumpActiveRequestInfo();
unsigned long long getNumInflightRequests();

/*
 * Schedule a function to run on the next appropriate treadmill round.
 *
 * The function will be called from the base mutex rank.
 *
 * Note: the function passed here escapes (naturally).  If you use a
 * lambda here, copy things into the lambda by value.
 *
 * f must be an rvalue: the treadmill takes ownership of it, so std::move
 * named callbacks in.
 *
 * Important: f() must not throw an exception.
 */
template<class F> void enqueue(F&& f);

struct Session final {
  Session(SessionKind session_kind) { startRequest(session_kind); }
  ~Session() { finishRequest(); }
  Session(Session&&) = delete;
  Session& operator=(Session&&) = delete;
};

//////////////////////////////////////////////////////////////////////

}

#include "hphp/runtime/vm/treadmill-inl.h"
