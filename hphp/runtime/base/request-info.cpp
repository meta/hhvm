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

#include "hphp/runtime/base/request-info.h"

#include <map>
#include <set>

#include <fmt/format.h>

#include "hphp/util/alloc.h"
#include "hphp/util/lock.h"
#include "hphp/util/perf-event.h"
#include "hphp/util/service-data.h"

#include "hphp/runtime/base/backtrace.h"
#include "hphp/runtime/base/builtin-functions.h"
#include "hphp/runtime/base/code-coverage.h"
#include "hphp/runtime/base/perf-mem-event.h"
#include "hphp/runtime/base/surprise-flags.h"
#include "hphp/runtime/server/cli-server.h"

#include "hphp/runtime/ext/process/ext_process.h"

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

namespace {
///////////////////////////////////////////////////////////////////////////////

/*
 * Set of all RequestInfos for the running process.
 */
std::set<RequestInfo*> s_request_infos;
Mutex s_request_info_mutex;

std::atomic<int> s_pendingOOMs{0};

///////////////////////////////////////////////////////////////////////////////
}

// Threshold for aborting when host is low on memory.
std::atomic_size_t
RequestInfo::s_OOMKillThreshold{std::numeric_limits<int64_t>::max()};

thread_local size_t RequestInfo::s_stackLimitWithSlack;

RDS_LOCAL_NO_CHECK(RequestInfo, RequestInfo::s_requestInfo);

RequestInfo::RequestInfo() {
}

RequestInfo::~RequestInfo() {
  Lock lock(s_request_info_mutex);
  s_request_infos.erase(this);
}

void RequestInfo::threadInit() {
  s_stackLimitWithSlack = s_stackLimit + StackSlack;
  assertx(m_id.unallocated());
  m_reqInjectionData.threadInit();
  Lock lock(s_request_info_mutex);
  s_request_infos.insert(this);
}

bool RequestInfo::valid(RequestInfo* info) {
  Lock lock(s_request_info_mutex);
  return s_request_infos.find(info) != s_request_infos.end();
}

void RequestInfo::GetExecutionSamples(std::map<Executing, int>& counts) {
  Lock lock(s_request_info_mutex);
  for (auto const info : s_request_infos) {
    ++counts[info->m_executing];
  }
}

void RequestInfo::ExecutePerRequest(std::function<void(RequestInfo*)> f) {
  Lock lock(s_request_info_mutex);
  for (auto request_info : s_request_infos) {
    f(request_info);
  }
}

void RequestInfo::BroadcastSignal(int signo) {
  ExecutePerRequest([signo] (RequestInfo* t) {
      t->m_reqInjectionData.sendSignal(signo);
    }
  );
}

int RequestInfo::SetPendingGCForAllOnRequest() {
  int cnt = 0;
  ExecutePerRequest( [&cnt](RequestInfo* t) {
    if ( t->changeGlobalGCStatus(OnRequestWithNoPendingExecution,
                                 OnRequestWithPendingExecution)) {
      t->m_reqInjectionData.setFlag(PendingGCFlag);
      cnt++;
    }
  } );
  return cnt;
}

void RequestInfo::InvokeOOMKiller(int maxToKill) {
  auto pendingOOMs = s_pendingOOMs.load(std::memory_order_acquire);
  while (!s_pendingOOMs.compare_exchange_weak(pendingOOMs,
                                              std::max(pendingOOMs, maxToKill),
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire));
  ExecutePerRequest(
    [] (RequestInfo* t) {
      t->m_reqInjectionData.setHostOOMFlag();
    }
  );
  Logger::FError("Invoking OOM killer on requests using more than {} bytes, "
                 "current RSS = {}MB",
                 OOMKillThreshold(), Process::GetMemUsageMb());
  static auto OOMKillerInvokeCounter = ServiceData::createTimeSeries(
    "hhvm_oom_killer_invoke", {ServiceData::StatsType::COUNT}
  );
  OOMKillerInvokeCounter->addValue(1);
}

void RequestInfo::onSessionInit(RequestId id, RequestId root_req_id) {
  assertx(!id.unallocated());
  assertx(m_id.unallocated());
  m_id = id;
  // If the root_req_id is not set, it means the current request is the
  // first request in the session. In this case, we set the root_req_id to
  // the current request id.
  m_root_req_id = root_req_id.unallocated() ? id : root_req_id;
  m_reqInjectionData.onSessionInit();
  m_coverage.onSessionInit();
}

bool RequestInfo::changeGlobalGCStatus(GlobalGCStatus from, GlobalGCStatus to) {
  return m_globalGCStatus.compare_exchange_strong(from, to);
}

void RequestInfo::setPendingException(Exception* e) {
  m_reqInjectionData.setFlag(PendingExceptionFlag);

  auto tmp = m_pendingException;
  m_pendingException = e;
  delete tmp;
}

void RequestInfo::onSessionExit() {
  assertx(!m_id.unallocated());
  m_id = RequestId();

  // Clear any timeout handlers to they don't fire when the request has already
  // been destroyed.
  m_reqInjectionData.setTimeout(0);
  m_reqInjectionData.setCPUTimeout(0);
  m_reqInjectionData.setUserTimeout(0);

  m_reqInjectionData.reset();
  m_coverage.onSessionExit();

  if (auto tmp = m_pendingException) {
    m_pendingException = nullptr;
    // request memory has already been freed
    if (auto ee = dynamic_cast<ExtendedException*>(tmp)) {
      ee->leakBacktrace();
    }
    delete tmp;
  }

  rds::requestExit();
}

//////////////////////////////////////////////////////////////////////

void raise_infinite_recursion_error() {
  raise_error("infinite recursion detected");
}

NEVER_INLINE void* stack_top_ptr_conservative() {
  DECLARE_FRAME_POINTER(fp);
  return fp;
}

static Exception* generate_request_timeout_exception(c_WaitableWaitHandle* wh) {
  auto timeout = RID().getTimeout();
  auto exceptionMsg = is_any_cli_mode()
    ? fmt::format(
        "Maximum execution time of {} seconds exceeded",
        timeout)
    : fmt::format(
        "entire web request took longer than {} seconds and timed out",
        timeout);
  auto exceptionStack = createBacktrace(BacktraceArgs()
                                        .fromWaitHandle(wh)
                                        .withSelf()
                                        .withThis()
                                        .withMetadata());
  return new RequestTimeoutException(exceptionMsg, exceptionStack);
}

static Exception* generate_request_cpu_timeout_exception(
  c_WaitableWaitHandle* wh
) {
  auto exceptionMsg = fmt::format(
    "Maximum CPU time of {} seconds exceeded",
    RID().getCPUTimeout()
  );

  auto exceptionStack = createBacktrace(BacktraceArgs()
                                        .fromWaitHandle(wh)
                                        .withSelf()
                                        .withThis()
                                        .withMetadata());
  return new RequestCPUTimeoutException(exceptionMsg, exceptionStack);
}

static Exception* generate_memory_exceeded_exception(c_WaitableWaitHandle* wh) {
  auto exceptionMsg = fmt::format(
    "request has exceeded memory limit {} of {} used",
    tl_heap->getStatsCopy().usage(),
    RID().getMemoryLimitNumeric());

  auto exceptionStack = createBacktrace(BacktraceArgs()
                                        .fromWaitHandle(wh)
                                        .withSelf()
                                        .withThis()
                                        .withMetadata());
  return new RequestMemoryExceededException(exceptionMsg, exceptionStack);
}

static Exception* generate_cli_client_terminated_exception(
  c_WaitableWaitHandle* wh
) {
  auto exceptionStack = createBacktrace(BacktraceArgs()
                                        .fromWaitHandle(wh)
                                        .withSelf()
                                        .withThis()
                                        .withMetadata());
  return new FatalErrorException("CLI client terminated", exceptionStack);
}

// suppress certain callbacks when we're running a user error handler;
// to reduce the chances that a subsequent error occurs in the callback
// and obscures the effect that the first handler would have had.
static bool callbacksOk() {
  switch (g_context->getErrorState()) {
    case ExecutionContext::ErrorState::NoError:
    case ExecutionContext::ErrorState::ErrorRaised:
    case ExecutionContext::ErrorState::ErrorRaisedByUserHandler:
      return true;
    case ExecutionContext::ErrorState::ExecutingUserHandler:
      return false;
  }
  not_reached();
}

size_t handle_request_surprise(c_WaitableWaitHandle* wh, size_t mask) {
  NoHandleSurpriseScope::AssertNone(static_cast<SurpriseFlag>(mask));
  auto& info = RI();
  auto& p = info.m_reqInjectionData;

  auto flags = stackLimitAndSurprise().fetchFlags() & mask;
  auto const debugging = p.getDebuggerAttached();

  // Start with any pending exception that might be on the request.
  stackLimitAndSurprise().clearFlag(PendingExceptionFlag);
  auto pendingException = info.m_pendingException;
  info.m_pendingException = nullptr;

  if (auto cbFlags =
      flags & (MemThresholdFlag | IntervalTimerFlag)) {
    if (!callbacksOk()) {
      stackLimitAndSurprise().setFlag(static_cast<SurpriseFlag>(cbFlags));
      flags -= cbFlags;
    }
  }

  if (flags & TimedOutFlag) {
    // Never send it back to callers unless we want soft timeout callback
    flags -= TimedOutFlag;

    if (!debugging) {
      if (p.checkTimeoutKind(TimeoutTime)) {
        p.setCPUTimeout(0);  // Stop CPU timer so we won't time out twice.
        if (pendingException) {
          stackLimitAndSurprise().setFlag(TimedOutFlag);
        } else {
          pendingException = generate_request_timeout_exception(wh);
        }
      } else if (p.checkTimeoutKind(TimeoutCPUTime)) {
        // Don't bother with the CPU timeout if we're already handling a wall
        // timeout.
        p.setTimeout(0);  // Stop wall timer so we won't time out twice.
        if (pendingException) {
          stackLimitAndSurprise().setFlag(TimedOutFlag);
        } else {
          pendingException = generate_request_cpu_timeout_exception(wh);
        }
      } else if (p.checkTimeoutKind(TimeoutSoft)) {
        p.setUserTimeout(0); // Stop wall timer so we won't time out twice.
        if (!callbacksOk()) {
          stackLimitAndSurprise().setFlag(TimedOutFlag);
        } else {
          flags += TimedOutFlag;
        }
      }
    }
  }

  if (flags & PendingGCFlag) {
    stackLimitAndSurprise().clearFlag(PendingGCFlag);
    if (tl_heap->isGCEnabled()) {
      tl_heap->collect("surprise");
      tl_heap->enforceMemoryLimit();
    } else {
      tl_heap->checkHeap("surprise");
    }
    // flags was snapshotted before the collection ran, so it does not yet
    // carry a MemExceededFlag that enforceMemoryLimit() just raised.
    flags |= stackLimitAndSurprise().fetchFlags() & mask & MemExceededFlag;
  }

  if (flags & MemExceededFlag) {
    if (p.hostOOMFlag() && !pendingException) {
      // When the host is running out of memory, don't abort all requests.
      // Instead, only kill a request if it uses a nontrivial amount of memory.
      auto const currUsage = tl_heap->currentUsage();
      // Once a request has the OOM abort flag set, it is never unset through
      // the lifetime of the request.
      if (currUsage > (RequestInfo::OOMKillThreshold() * info.m_OOMMultiplier) &&
          !p.shouldOOMAbort() &&
          s_pendingOOMs.fetch_sub(1, std::memory_order_acq_rel) > 0) {
        p.setRequestOOMAbort();
      }
      if (p.shouldOOMAbort()) {
        pendingException =
          new RequestOOMKilledException(static_cast<size_t>(currUsage));
        // In case this exception doesn't stop other pieces of code from
        // running, keep aborting them until all are dead.
        p.setHostOOMFlag();
      } else {
        // Let this request survive. If the OOM killer comes back again, we will
        // check again then.
        p.clearHostOOMFlag();
      }
    }
    if (p.requestOOMFlag() && !pendingException) {
      // Request exceeded memory limit, but the host is fine.
      pendingException = generate_memory_exceeded_exception(wh);
    }
  }
  if (debugging && p.getDebuggerStepIntr()) {
    // we'll disable specific flags to make sure Xenon (or other)
    // re-entrant code doesn't interfere with debugging.
    if (flags & XenonSignalFlag) {
      stackLimitAndSurprise().clearFlag(XenonSignalFlag);
    }
    if (flags & HeapSamplingFlag) {
      stackLimitAndSurprise().clearFlag(HeapSamplingFlag);
    }
    if (flags & IntervalTimerFlag) {
      stackLimitAndSurprise().clearFlag(IntervalTimerFlag);
    }
  }
  if (flags & CLIClientTerminated) {
    if (pendingException) {
      stackLimitAndSurprise().setFlag(CLIClientTerminated);
    } else {
      pendingException = generate_cli_client_terminated_exception(wh);
    }
  }
  if (flags & SignaledFlag) {
    stackLimitAndSurprise().clearFlag(SignaledFlag);
    HHVM_FN(pcntl_signal_dispatch)();
  }

  if (flags & PendingPerfEventFlag) {
    stackLimitAndSurprise().clearFlag(PendingPerfEventFlag);
    perf_event_consume(record_perf_mem_event);
  }

  if (pendingException) {
    pendingException->throwException();
  }
  return flags;
}

}
