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

#include "hphp/runtime/vm/treadmill.h"

#include <atomic>
#include <vector>
#include <memory>
#include <algorithm>

#include <pthread.h>
#include <stdio.h>
#include <signal.h>

#include <fmt/format.h>
#include <folly/portability/SysTime.h>
#include <folly/Likely.h>

#include "hphp/runtime/base/request-info.h"
#include "hphp/util/logger.h"
#include "hphp/util/process.h"
#include "hphp/util/trace.h"
#include "hphp/util/rank.h"
#include "hphp/util/rds-local.h"
#include "hphp/util/service-data.h"

namespace HPHP {  namespace Treadmill {

TRACE_SET_MOD(treadmill)

namespace {

//////////////////////////////////////////////////////////////////////

/*
 * We assign local, unique indexes to each thread, with hopes that
 * they are densely packed.
 *
 * The plan here is that each thread starts with rl_thisThreadIdx as
 * kInvalidRequestIdx.  And the first time a thread starts using the Treadmill
 * it allocates a new thread id from s_nextThreadIdx with fetch_add.
 */
std::atomic<int64_t> g_nextThreadIdx{0};
constexpr int64_t kInvalidRequestIdx = -1;
RDS_LOCAL_NO_CHECK(int64_t, rl_thisRequestIdx){kInvalidRequestIdx};

struct TreadmillRequestInfo {
  Clock::time_point startTime;
  pthread_t pthreadId;
  RequestInfo* requestInfo;
  SessionKind sessionKind;
};

pthread_mutex_t s_stateLock = PTHREAD_MUTEX_INITIALIZER;
std::vector<TreadmillRequestInfo> s_inflightRequests;

// Start time of the oldest request in flight. All writes happen under the
// s_stateLock.
std::atomic<Clock::time_point> s_oldestRequestInFlight{kNoStartTime};

std::string s_killMessage;

//////////////////////////////////////////////////////////////////////

struct StateGuard {
  StateGuard() {
    checkRank(RankTreadmill);
    pthread_mutex_lock(&s_stateLock);
    pushRank(RankTreadmill);
  }

  struct Locked {};
  explicit StateGuard(Locked) { pushRank(RankTreadmill); }

  ~StateGuard() {
    popRank(RankTreadmill);
    pthread_mutex_unlock(&s_stateLock);
  }
};

//////////////////////////////////////////////////////////////////////
char const* getSessionKindName(SessionKind value) {
  switch(value) {
    case SessionKind::None: return "None";
    case SessionKind::DebuggerClient: return "DebuggerClient";
    case SessionKind::PreloadRepo: return "PreloadRepo";
    case SessionKind::Watchman: return "Watchman";
    case SessionKind::Vsdebug: return "VSDebug";
    case SessionKind::FactsWorker: return "FactsWorker";
    case SessionKind::CLIServer: return "CLIServer";
    case SessionKind::AdminPort: return "AdminRequest";
    case SessionKind::HttpRequest: return "HttpRequest";
    case SessionKind::RpcRequest: return "RpcRequest";
    case SessionKind::TranslateWorker: return "TranslateWorker";
    case SessionKind::Retranslate: return "Retranslate";
    case SessionKind::RetranslateAll: return "RetranslateAll";
    case SessionKind::ProfData: return "ProfData";
    case SessionKind::UnitTests: return "UnitTests";
    case SessionKind::CompileRepo: return "CompileRepo";
    case SessionKind::HHBBC: return "HHBBC";
    case SessionKind::CompilerEmit: return "CompilerEmit";
    case SessionKind::CompilerAnalysis: return "CompilerAnalysis";
    case SessionKind::CLISession: return "CLISession";
    case SessionKind::UnitReaper: return "UnitReaper";
  }
  return "";
}

//////////////////////////////////////////////////////////////////////

/*
 * Returns how long (wall time) the oldest request in flight has been running.
 */
Clock::duration getOldestRequestAge() {
  auto const oldestStartTime = getOldestRequestStartTime();
  return oldestStartTime != kNoStartTime
    ? Clock::now() - oldestStartTime
    : Clock::duration::zero();
}

/*
 * Get the ID of the thread to abort in case the treadmill gets stuck for too
 * long.  In general, this is the oldest thread, but special treatment is used
 * for retranslate-all.  When the main retranslate-all thread gets stuck, it's
 * often because it's waiting for one of the JIT worker threads, which do the
 * bulk of the JITing.  In such cases, we want to abort the oldest JIT worker,
 * to capture its backtrace, instead of the main retranslate-all thread.
 */
const TreadmillRequestInfo* getRequestToAbort() {
  const TreadmillRequestInfo* oldest = nullptr;
  const TreadmillRequestInfo* oldestWorker = nullptr;

  for (auto const& req : s_inflightRequests) {
    if (req.startTime == kNoStartTime) continue;
    auto const timeout = req.requestInfo
      ? req.requestInfo->m_reqInjectionData.getTimeout()
      : 0;
    auto const limit = std::chrono::seconds(std::max(
      Cfg::Server::MaxRequestAgeFactor * Cfg::Server::RequestTimeoutSeconds,
      Cfg::Server::MaxRequestAgeFactor * timeout
    ));
    if (!limit.count()) continue;
    if (req.sessionKind == SessionKind::TranslateWorker &&
        (oldestWorker == nullptr || req.startTime < oldestWorker->startTime)) {
      oldestWorker = &req;
    }
    auto const age = Clock::now() - req.startTime;
    if (age <= limit) continue;
    oldest = &req;
  }

  if (!oldest) return nullptr;
  if (oldest->sessionKind != SessionKind::RetranslateAll) return oldest;
  if (oldestWorker) return oldestWorker;
  return oldest;
}

void checkOldest(Optional<StateGuard>& guard, bool force) {
  if (debug && !force) return;

  auto const limit =
    Cfg::Server::MaxRequestAgeFactor *
      std::chrono::seconds(Cfg::Server::RequestTimeoutSeconds);
  if (limit.count() == 0) return;

  auto const oldestAge = getOldestRequestAge();
  if (oldestAge <= limit) return;

  auto const request = getRequestToAbort();
  if (!request) return;

  auto const msg = fmt::format(
    "Oldest request ({}, {}, {}) has been running for {} "
    "seconds. Aborting the server.",
    request->requestInfo ? request->requestInfo->m_id.toString() : "none",
    request->startTime.time_since_epoch().count(),
    getSessionKindName(request->sessionKind),
    std::chrono::duration_cast<std::chrono::seconds>(
      Clock::now() - request->startTime
    ).count()
  );
  Logger::Error(msg);
  s_killMessage = msg;
  auto const threadId = request->pthreadId;
  // Drop the lock since the SIGABRT we're about to send will try to
  // acquire it.
  guard.reset();
  pthread_kill(threadId, SIGABRT);
  // We're going to die, wait for it and don't bother proceeding.
  ::pause();
  always_assert(false);
}

void refreshStats() {
  static ServiceData::ExportedCounter* s_oldestRequestAgeStat =
    ServiceData::createCounter("treadmill.age");
  auto const oldestAge = getOldestRequestAge();
  s_oldestRequestAgeStat->setValue(
    std::chrono::duration_cast<std::chrono::seconds>(oldestAge).count()
  );
}
}

/*
 * Return the current thread's index.
 */
int64_t requestIdx() {
  if (UNLIKELY(*rl_thisRequestIdx == kInvalidRequestIdx)) {
    *rl_thisRequestIdx =
      g_nextThreadIdx.fetch_add(1, std::memory_order_acq_rel);
  }
  return *rl_thisRequestIdx;
}

struct PendingTriggers {
  ConcurrentSList<WorkItem> list;
  ~PendingTriggers() {
    s_destroyed = true;
    // Delete any work items still in the list at shutdown.
    auto it = list.begin();
    while (it.valid()) {
      auto* item = &it.data();
      it.advance(true);
      delete item;
    }
  }
  static bool s_destroyed;
};

static PendingTriggers s_tq;
bool PendingTriggers::s_destroyed = false;
void enqueueInternal(std::unique_ptr<WorkItem> gt) {
  if (PendingTriggers::s_destroyed) {
    return;
  }

  // Timestamp is grabbed without synchronization so the list may not be
  // strictly ordered, but that's okay — timestamps will be close enough
  // and work items will most likely be processed together.
  gt->m_timestamp = Clock::now();
  s_tq.list.insert_head(*gt.release());
}

void startRequest(SessionKind session_kind) {
  auto const requestIdx = Treadmill::requestIdx();

  Optional<StateGuard> g;
  g.emplace();

  refreshStats();
  checkOldest(g, false);
  auto const startTime = Clock::now();
  if (requestIdx >= s_inflightRequests.size()) {
    s_inflightRequests.resize(
      requestIdx + 1, {kNoStartTime, 0, 0, SessionKind::None});
  } else {
    assertx(s_inflightRequests[requestIdx].startTime == kNoStartTime);
  }
  s_inflightRequests[requestIdx].startTime = startTime;
  s_inflightRequests[requestIdx].pthreadId = Process::GetThreadId();
  s_inflightRequests[requestIdx].requestInfo =
    RequestInfo::s_requestInfo.isNull() ? nullptr : &RI();
  s_inflightRequests[requestIdx].sessionKind = session_kind;
  FTRACE(1, "requestIdx {} pthreadId {} start @{}\n", requestIdx,
         s_inflightRequests[requestIdx].pthreadId,
         startTime.time_since_epoch().count());

  // Set the oldest request in flight if there is none set yet. We obtained
  // the monotonic timestamp under the state guard, so there can't be a later
  // one set.
  auto const oldest = s_oldestRequestInFlight.load(std::memory_order_acquire);
  assertx(oldest <= startTime);
  if (oldest == kNoStartTime) {
    s_oldestRequestInFlight.store(startTime, std::memory_order_release);
  }

  if (!RequestInfo::s_requestInfo.isNull()) {
    RI().changeGlobalGCStatus(RequestInfo::Idle,
                              RequestInfo::OnRequestWithNoPendingExecution);
  }
}

void finishRequest() {
  auto const requestIdx = Treadmill::requestIdx();
  assertx(requestIdx != -1);
  FTRACE(1, "tid {} finish\n", requestIdx);
  std::vector<std::unique_ptr<WorkItem>> toFire;
  {
    StateGuard g;
    assertx(s_inflightRequests[requestIdx].startTime != kNoStartTime);
    auto const startTime = s_inflightRequests[requestIdx].startTime;
    s_inflightRequests[requestIdx].startTime = kNoStartTime;

    // After finishing a request, check to see if we've allowed any triggers
    // to fire and update the time of the oldest request in flight.
    // However if the request just finished is not the current oldest we
    // don't need to check anything as there cannot be any WorkItem to run.
    if (s_oldestRequestInFlight.load(std::memory_order_acquire) == startTime) {
      auto limit = Clock::time_point::max();
      for (auto& val : s_inflightRequests) {
        if (val.startTime != kNoStartTime && val.startTime < limit) {
          limit = val.startTime;
        }
      }
      // update "oldest in flight" or kill it if there are no running requests
      s_oldestRequestInFlight.store(
        limit == Clock::time_point::max() ? kNoStartTime : limit,
        std::memory_order_release
      );

      // Collect WorkItems to run. The list is in reverse insertion order
      // (newest at head), so we must walk the entire list to find all
      // items older than the new oldest request.
      auto it = s_tq.list.begin();
      while (it.valid()) {
        auto& item = it.data();
        if (item.m_timestamp >= limit) {
          it.advance();
          continue;
        }
        TRACE(2, "delendum %lld\n",
              (long long)item.m_timestamp.time_since_epoch().count());
        it.advance(true);
        toFire.emplace_back(&item);
      }
    }
    constexpr int limit = 100;
    if (!RequestInfo::s_requestInfo.isNull()) {
      // If somehow we excessed the limit, GlobalGCTrigger will stay on
      // "Triggering" stage forever. No more global GC can be triggered.
      // But it should have no effect on APC GC -- The data will be freed
      // by treadmill's calling
      int i;
      for (i = 0; i < limit; ++i) {
        if (RI().changeGlobalGCStatus(
              RequestInfo::OnRequestWithPendingExecution,
              RequestInfo::Idle)) {
          // Call globalGCTrigger to Run the pending execution
          // TODO(20074509)
          FTRACE(2, "treadmill executes pending global GC callbacks\n");
          break;
        }
        if (RI().changeGlobalGCStatus(
              RequestInfo::OnRequestWithNoPendingExecution,
              RequestInfo::Idle)) {
          break;
        }
      }
      assertx(i < limit);
      if (i == limit) {
        Logger::Warning("Treadmill fails to set global GC status into Idle");
      }
    }
  }
  for (unsigned i = 0; i < toFire.size(); ++i) {
    toFire[i]->run();
  }
}

Clock::time_point getOldestRequestStartTime() {
  return s_oldestRequestInFlight.load(std::memory_order_acquire);
}

Clock::time_point getRequestStartTime() {
  auto const requestIdx = Treadmill::requestIdx();
  assertx(requestIdx != kInvalidRequestIdx);
  return s_inflightRequests[requestIdx].startTime;
}

SessionKind sessionKind() {
  if (*rl_thisRequestIdx == kInvalidRequestIdx) return SessionKind::None;
  return s_inflightRequests[*rl_thisRequestIdx].sessionKind;
}

void deferredFree(void* p) {
  enqueue([p] { free(p); });
}

void checkForStuckTreadmill() {
  Optional<StateGuard> g;
  g.emplace();
  refreshStats();
  checkOldest(g, true);
}

std::string dumpActiveRequestInfo() {
  std::string out;
  out += "\nActive Requests:\n";
  for (auto& req : s_inflightRequests) {
    if (req.startTime != kNoStartTime) {
      fmt::format_to(std::back_inserter(out), "  {} {} {} (age {}ms){} {}{}\n",
        req.pthreadId,
        req.requestInfo ? req.requestInfo->m_id.toString() : "none",
        req.startTime.time_since_epoch().count(),
        std::chrono::duration_cast<std::chrono::milliseconds>(
          Clock::now() - req.startTime
        ).count(),
        req.requestInfo
          ? fmt::format(
            " (timeout {}s)",
            req.requestInfo->m_reqInjectionData.getTimeout()
          )
          : "",
        getSessionKindName(req.sessionKind),
        req.startTime ==  getOldestRequestStartTime() ? " OLDEST" : ""
      );
    }
  }

  return out;
}

unsigned long long getNumInflightRequests() {
    return s_inflightRequests.size();
}

std::string dumpTreadmillInfo(bool forCrash) {
  std::string out;
  Optional<StateGuard> g;

  if (!forCrash) {
    g.emplace();
  } else {
    // Make an attempt to grab the lock even if we're dumping for a
    // crash. Only wait one second and then give up. If we don't grab
    // the lock, then proceed anyways and hope for the best.
    struct timespec timeout;
    clock_gettime(CLOCK_REALTIME, &timeout);
    ++timeout.tv_sec;
    if (pthread_mutex_timedlock(&s_stateLock, &timeout) != 0) {
      out += "Attempting to dump treadmill without acquiring StateGuard\n";
    } else {
      g.emplace(StateGuard::Locked{});
    }
  }

  if (!s_killMessage.empty()) {
    fmt::format_to(std::back_inserter(out), "{}\n\n", s_killMessage);
  }

  auto const oldestStart = getOldestRequestStartTime();

  fmt::format_to(std::back_inserter(out), "Now: {}\n",
    Clock::now().time_since_epoch().count()
  );

  fmt::format_to(std::back_inserter(out), "OldestStartTime: {}\n",
    oldestStart.time_since_epoch().count()
  );

  fmt::format_to(std::back_inserter(out), "InflightRequestsSize: {}\n",
    s_inflightRequests.size()
  );

  out += dumpActiveRequestInfo();

  return out;
}

}}
