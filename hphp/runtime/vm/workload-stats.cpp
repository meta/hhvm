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

#include "hphp/runtime/vm/workload-stats.h"

#include "hphp/runtime/base/init-fini-node.h"
#include "hphp/runtime/server/server-stats.h"

#include "hphp/util/compilation-flags.h"
#include "hphp/util/service-data.h"
#include "hphp/util/thread-local.h"
#include "hphp/util/timer.h"

#include <mutex>
#include <vector>

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////
namespace {

using Counter = int64_t;
constexpr Counter kInvalidCounter = -1;

Counter getCurrentCounter() {
  return Counter {
    Timer::GetThreadCPUTimeNanos()
  };
}

struct DeltaCounter {
  Counter start = kInvalidCounter;
  Counter acc;
};

void start(DeltaCounter& dc) {
  assertx(dc.start == kInvalidCounter);
  dc.start = getCurrentCounter();
}

void end(DeltaCounter& dc) {
  dc.acc += getCurrentCounter() - dc.start;
  assertx(dc.start != kInvalidCounter);
  if (debug) dc.start = kInvalidCounter;
}

///////////////////////////////////////////////////////////////////////////////
}

// Request workload stats needs external linkage to appease type scanners.
struct RequestWorkloadStats final {
  using State = WorkloadStats::State;

  static void LoggingInit();

  void requestInit();
  void requestShutdown();

  void transition(State from, State to);

  void enter(State to);
  void leave();

private:
  // PHP time is time in a request not spent JITing.
  // Interp time is time spent interpreting PHP.  This discounts time spent
  // interpreting due to interpOnes.  This is because they are in JITed code,
  // and it doesn't make sense to count them against the JIT being well adapted
  // to the current workload.
  DeltaCounter m_php;
  DeltaCounter m_interp;

  uint64_t m_transitionCounts[3] = {};

  std::vector<State> m_s;

  // Counters initialized at process start.
  static ServiceData::ExportedTimeSeries* s_interpNanos;
  static ServiceData::ExportedTimeSeries* s_requestNanos;
  static ServiceData::ExportedTimeSeries* s_interpVMRatio;
  static ServiceData::ExportedTimeSeries* s_trans;
  static ServiceData::ExportedTimeSeries* s_interps;

public:
  static int64_t s_relativeInterp;
  static int64_t s_requests;
  static std::mutex s_mutex;
};
ServiceData::ExportedTimeSeries* RequestWorkloadStats::s_interpNanos;
ServiceData::ExportedTimeSeries* RequestWorkloadStats::s_requestNanos;
ServiceData::ExportedTimeSeries* RequestWorkloadStats::s_interpVMRatio;
ServiceData::ExportedTimeSeries* RequestWorkloadStats::s_trans;
ServiceData::ExportedTimeSeries* RequestWorkloadStats::s_interps;
int64_t RequestWorkloadStats::s_relativeInterp = 0;
int64_t RequestWorkloadStats::s_requests = 0;
std::mutex RequestWorkloadStats::s_mutex;

// Setup RequestWorkloadStats to have init and request callbacks.
THREAD_LOCAL_NO_CHECK(RequestWorkloadStats, s_request_workload_stats);
InitFiniNode init(RequestWorkloadStats::LoggingInit,
                  InitFiniNode::When::ProcessInit);
InitFiniNode t_init([] () { s_request_workload_stats.getCheck(); },
                    InitFiniNode::When::ThreadInit);
InitFiniNode r_init([] () { s_request_workload_stats->requestInit(); },
                    InitFiniNode::When::RequestStart);
InitFiniNode r_shutdown([] () { s_request_workload_stats->requestShutdown(); },
                        InitFiniNode::When::RequestFini);

void RequestWorkloadStats::LoggingInit() {
  const std::vector<ServiceData::StatsType> exportTypes{
    ServiceData::StatsType::AVG,
  };
  const std::vector<std::chrono::seconds> levels{
    std::chrono::seconds(60),
    std::chrono::seconds(0),
  };

  s_interpNanos = ServiceData::createTimeSeries(
    "vm.nanos_interp", exportTypes, levels);
  s_requestNanos = ServiceData::createTimeSeries(
    "vm.nanos_php", exportTypes, levels);
  s_interpVMRatio = ServiceData::createTimeSeries(
    "vm.relative_nanos_interp", exportTypes, levels);
  s_trans = ServiceData::createTimeSeries(
    "vm.enter_trans", exportTypes, levels);
  s_interps = ServiceData::createTimeSeries(
    "vm.enter_interp", exportTypes, levels);
}

void RequestWorkloadStats::requestInit() {
  assertx(m_s.empty());
  m_php.acc = 0;
  m_interp.acc = 0;
  m_s.emplace_back(State::InRequest);
  start(m_php);

  for (auto& count : m_transitionCounts) {
    count = 0;
  }
}

void RequestWorkloadStats::requestShutdown() {
  if (m_s.empty()) return;
  end(m_php);
  assertx(m_s.back() == State::InRequest);
  m_s.pop_back();
  assertx(m_s.empty());

  const auto php = m_php.acc;
  const auto interp = m_interp.acc;

  s_interpNanos->addValue(interp);
  s_requestNanos->addValue(php);
  ServerStats::Log("nanos_interp", interp);
  ServerStats::Log("nanos_php", php);

  if (php > 0) {
    auto const value = interp * 10000 / php;
    s_interpVMRatio->addValue(value);
    ServerStats::Log("interp_vm_ratio", value);

    {
      std::unique_lock<std::mutex> guard(s_mutex);
      s_relativeInterp += value;
      s_requests++;
    }
  }

  // Log state change counts.
  s_trans->addValue(m_transitionCounts[State::InTrans]);
  s_interps->addValue(m_transitionCounts[State::InInterp]);
}

ALWAYS_INLINE void RequestWorkloadStats::transition(State from, State to) {
// Disable this logic in OSS as these timing measurements
// are uneconomical without kernel-level nanosecond timing.
#ifdef HHVM_FACEBOOK
  switch (from) {
  case State::InRequest:
    switch (to) {
    case State::InRequest:
      always_assert(false);
      break;
    case State::InInterp:
      start(m_interp);
      break;
    case State::InTrans:
      end(m_php);
      break;
    }
    break;

  case State::InInterp:
    switch (to) {
    case State::InInterp:
      break;
    case State::InRequest:
      end(m_interp);
      break;
    case State::InTrans:
      end(m_interp);
      end(m_php);
      break;
    }
    break;

  case State::InTrans:
    switch (to) {
    case State::InTrans:
      always_assert(false);
      break;
    case State::InRequest:
      start(m_php);
      break;
    case State::InInterp:
      start(m_php);
      start(m_interp);
      break;
    }
    break;
  }
#endif // HHVM_FACEBOOK
}

void RequestWorkloadStats::enter(State to) {
  if (m_s.empty()) return;
  transition(m_s.back(), to);
  m_s.emplace_back(to);
  m_transitionCounts[to]++;
}

void RequestWorkloadStats::leave() {
  if (m_s.empty()) return;
  auto s = m_s.back();
  m_s.pop_back();
  transition(s, m_s.back());
}

WorkloadStats::WorkloadStats(State guardedState) {
  s_request_workload_stats->enter(guardedState);
}

WorkloadStats::~WorkloadStats() {
  s_request_workload_stats->leave();
}

int64_t WorkloadStats::GetAndResetAvgRelativeInterp() {
  int64_t relativeInterp;
  int64_t requests;
  {
    std::unique_lock<std::mutex> guard(RequestWorkloadStats::s_mutex);
    relativeInterp = RequestWorkloadStats::s_relativeInterp;
    requests = RequestWorkloadStats::s_requests;
    RequestWorkloadStats::s_relativeInterp = 0;
    RequestWorkloadStats::s_requests = 0;
  }
  return requests ? (relativeInterp / requests) : relativeInterp;
}

void WorkloadStats::EnsureInit() {
  s_request_workload_stats.getCheck();
}

///////////////////////////////////////////////////////////////////////////////
}
