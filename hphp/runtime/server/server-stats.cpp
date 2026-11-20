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

#include "hphp/runtime/server/server-stats.h"

#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/base/datetime.h"
#include "hphp/runtime/base/memory-manager.h"
#include "hphp/runtime/base/runtime-option.h"
#include "hphp/runtime/server/http-server.h"
#include "hphp/runtime/server/thread-hint.h"
#include "hphp/runtime/server/writer.h"
#include "hphp/util/build-info.h"
#include "hphp/util/configs/stats.h"
#include "hphp/util/hardware-counter.h"
#include "hphp/util/process.h"
#include "hphp/util/text-util.h"
#include "hphp/util/timer.h"

#include <fmt/format.h>
#include <folly/Conv.h>
#include <folly/String.h>

#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace HPHP {

void ServerStats::GetLogger() {
  s_logger.getCheck();
}

void ServerStats::Merge(CounterMap& dest, const CounterMap& src,
                        const KeyMap& wanted) {
  for (auto const& iter : src) {
    auto const& key = iter.first;
    if (!wanted.empty() && !wanted.count(key)) continue;
    dest[key] += iter.second;
  }
}

void ServerStats::Merge(TimeSlot& dest, const TimeSlot& src,
                        const KeyMap& wanted) {
  dest.m_hits += src.m_hits;
  Merge(dest.m_values, src.m_values, wanted);
}

ServerStats::KeyMap ServerStats::CompileKeys(
  const std::vector<std::string>& rules) {

  KeyMap res;
  if (rules.empty()) return res;
  for (auto const& rule : rules) {
    assertx(!rule.empty());
    auto len = rule.length();
    std::string suffix;
    if (len > 4) {
      len -= 4;
      suffix = rule.substr(len);
    }
    if (suffix == "/hit") {
      res[rule.substr(0, len)] |= UDF_HIT;
    } else if (suffix == "/sec") {
      res[rule.substr(0, len)] |= UDF_SEC;
    } else {
      res[rule] |= UDF_NONE;
    }
  }
  return res;
}

///////////////////////////////////////////////////////////////////////////////
// static

Mutex ServerStats::s_lock;
std::vector<ServerStats*> ServerStats::s_loggers;
THREAD_LOCAL_NO_CHECK(ServerStats, ServerStats::s_logger);

void ServerStats::LogPage(const std::string& url, int code) {
  if (Cfg::Stats::Web && Cfg::Stats::Enable) {
    ServerStats::s_logger->logPage(url, code);
  }
}

void ServerStats::Log(const std::string& name, int64_t value) {
  if (Cfg::Stats::Web && Cfg::Stats::Enable) {
    ServerStats::s_logger->log(name, value);
  }
}

void ServerStats::LogBytes(int64_t bytes) {
  if (Cfg::Stats::Web && Cfg::Stats::Enable) {
    ServerStats::s_logger->logBytes(bytes);
  }
}

void ServerStats::StartRequest(const char* url, const char* clientIP,
                               const char* vhost) {
  if (Cfg::Stats::Web && Cfg::Stats::Enable) {
    ServerStats::s_logger->startRequest(url, clientIP, vhost);
  }
}

void ServerStats::SetEndpoint(const char* endpoint) {
  if (Cfg::Stats::Web && Cfg::Stats::Enable) {
    ServerStats::s_logger->setEndpoint(endpoint);
  }
}

void ServerStats::SetThreadMode(ThreadMode mode) {
  ServerStats::s_logger->setThreadMode(mode);
}

ServerStats::ThreadMode ServerStats::GetThreadMode() {
  return ServerStats::s_logger->m_threadStatus.m_mode;
}

const char* ServerStats::ThreadModeString(ThreadMode mode) {
  switch (mode) {
    case ThreadMode::Idling:
      return "Idling";
    case ThreadMode::Processing:
      return "Processing";
    case ThreadMode::Writing:
      return "Writing";
    case ThreadMode::PostProcessing:
      return "PostProcessing";
  }
  not_reached();
}

void ServerStats::SetThreadIOStatus(const char* name, const char* addr,
                                    int64_t usWallTime /* = -1 */) {
  ServerStats::s_logger->setThreadIOStatus(name, addr, usWallTime);
}

Array ServerStats::GetThreadIOStatuses() {
  return ServerStats::s_logger->getThreadIOStatuses();
}

int64_t ServerStats::Get(const std::string& name) {
  return ServerStats::s_logger->get(name);
}

void ServerStats::Reset() {
  ServerStats::s_logger->reset();
}

void ServerStats::Clear() {
  VisitAllSlots([](TimeSlot& slot) { slot.clear(); } );
}

std::string ServerStats::GetKeys() {
  // special keys that don't come from slots
  std::set<std::string> allKeys {
    "hit", "load", "load-pct", "idle", "queued"
  };

  VisitAllSlots(
    [&allKeys](TimeSlot& slot) {
      const auto& values = slot.m_values;
      for (const auto& kvp : values) {
        auto const& key = kvp.first;
        allKeys.insert(key);
      }
    }
  );

  std::string out;
  out.reserve(16u * allKeys.size());
  for (auto const& iter : allKeys) {
    out += iter;
    out += "\n";
  }
  return out;
}

hphp_fast_string_map<int64_t> ServerStats::Report(
  const std::vector<std::string>& keys, const std::string& prefix) {

  auto report = ReportImpl(CompileKeys(keys));
  if (prefix.empty()) return report;

  hphp_fast_string_map<int64_t> result(report.size());
  for (auto& val: report) {
    result.emplace(prefix + "." + val.first, std::move(val.second));
  }
  return result;
}

std::string ServerStats::ReportString(const std::string& keys,
                                      const std::string& prefix) {
  std::vector<std::string> rules;
  folly::split(',', keys.c_str(), rules, true);

  auto const result = ReportImpl(CompileKeys(rules));

  std::ostringstream out;
  out << "{";
  std::string key = prefix;
  if (!key.empty()) {
    key += ".";
  }

  // Sort keys in alphabetical order before printing to the string.
  std::vector<std::pair<std::string, int64_t>> kvpVec;
  kvpVec.reserve(result.size());
  for (auto const& kvpair : result) {
    kvpVec.push_back(kvpair);
  }
  std::sort(kvpVec.begin(), kvpVec.end());

  bool firstKey = true;
  for (auto const& kvpair : kvpVec) {
    if (firstKey) {
      firstKey = false;
    } else {
      out << ", ";
    }
    out << Writer::escape_for_json((key + kvpair.first).c_str())
        << ": " << kvpair.second;
  }
  out << "}\n";

  return out.str();
}

ServerStats::CounterMap ServerStats::ReportImpl(const KeyMap& wantedKeys) {

  auto const beginTime = static_cast<uint64_t>(time(nullptr));
  auto const SlotDuration = Cfg::Stats::SlotDuration;
  auto const now = beginTime / SlotDuration;

  TimeSlot ts{};
  // Aggregate every TimeSlot with in the range [now + 2 - MaxSlot, now], The
  // slot (now + 1 - MaxSlot) actually contains valid data, but it is at risk of
  // being cleared if time advances to the next slot (now + 1) while we are
  // collecting here. So exclude those. Hopefully the following aggregation
  // process takes less than StatsSlotDuration seconds.
  VisitAllSlots(
    [&wantedKeys, now, &ts] (TimeSlot& slot) {
      if (slot.m_time <= now + 1u - Cfg::Stats::MaxSlot) {
        // We are not going to need the old data in future.
        slot.clear();
        return;
      }
      if (slot.m_time > now) return;
      Merge(ts, slot, wantedKeys);
    }
  );

  // Total amount of time over which the stats were written.  We try to estimate
  // the time between starting and ending time of the collection process by
  // taking some averages.
  uint32_t sec = SlotDuration * (Cfg::Stats::MaxSlot - 2u);
  auto const endTime = static_cast<uint64_t>(time(nullptr));
  if (endTime / Cfg::Stats::SlotDuration != now) {
    sec += (beginTime % SlotDuration + SlotDuration + 1) / 2;
  } else {
    sec += ((beginTime + endTime + 1) % (2 * SlotDuration)) / 2;
  }

  auto& values = ts.m_values;

  // Derive keys with suffix /hit or /sec
  for (auto const& iter : wantedKeys) {
    auto const& key = iter.first;
    auto const udf = iter.second;
    auto viter = values.find(key);
    if (viter == values.end()) continue;
    auto const value = viter->second;
    if ((udf & UDF_HIT) && ts.m_hits) {
      values[key + "/hit"] = value * PRECISION / ts.m_hits;
    }
    if ((udf & UDF_SEC) && sec) {
      values[key + "/sec"] = value * PRECISION / sec;
    }
  }

  auto const wantAll = wantedKeys.empty();
  // write special keys
  if (wantAll || wantedKeys.find("hit") != wantedKeys.end()) {
    values["hit"] = ts.m_hits;
    // hit/sec is probably more meaningful than hit itself.
    if (sec) values["hit/sec"] = ts.m_hits * PRECISION / sec;
  }
  if (auto const server = HttpServer::Server->getPageServer()) {
    auto const load = server->getActiveWorker();
    if (wantAll || wantedKeys.find("load") != wantedKeys.end()) {
      values["load"] = load;
    }
    if (wantAll || wantedKeys.find("load-pct") != wantedKeys.end()) {
      values["load-pct"] = load * 100 / server->getMaxThreadCount();
    }
    if (wantAll || wantedKeys.find("idle") != wantedKeys.end()) {
      values["idle"] = server->getMaxThreadCount() - load;
    }
    if (wantAll || wantedKeys.find("queued") != wantedKeys.end()) {
      values["queued"] = server->getQueuedJobs();
    }
  }

  return ts.m_values;
}

static std::string format_duration(timeval& duration) {
  std::string ret;
  if (duration.tv_sec > 0 || duration.tv_usec > 0) {
    int milliseconds = duration.tv_usec / 1000;
    double seconds = duration.tv_sec % 60 + milliseconds * .001;
    int minutes = duration.tv_sec / 60;
    int hours = minutes / 60;
    minutes = minutes % 60;
    if (hours) {
      ret += folly::to<std::string>(hours) + " hour";
      ret += (hours == 1) ? " " : "s ";
    }
    if (minutes || (hours && seconds)) {
      ret += folly::to<std::string>(minutes) + " minute";
      ret += (minutes == 1) ? " " : "s ";
    }
    if (seconds || minutes || hours) {
      ret += fmt::format("{:.3f}", seconds);
      ret += (seconds == 1) ? "" : "s";
    }
  } else {
   ret = "0 second";
  }
  return ret;
}

ServerStatusReport ServerStats::ReportStatus() {
  ServerStatusReport status{};

  status.process.id = (int64_t)getpid();
  status.process.build = RuntimeOption::BuildId;
  status.process.compiler = compilerId().begin();

#ifdef NDEBUG
  status.process.debug = false;
#else
  status.process.debug = true;
#endif

  time_t now = time(nullptr);
  status.process.now = (int64_t)now;

  // Match behavior of HHVM_FN(server_uptime) and return -1 if the server has
  // not started yet.
  status.process.start = HttpServer::StartTime == 0 ? -1  : HttpServer::StartTime;
  status.process.up = HttpServer::StartTime == 0 ? -1 : (now - HttpServer::StartTime);

  Lock lock(s_lock, false);
  timeval current;
  gettimeofday(&current, 0);

  for (unsigned int i = 0; i < s_loggers.size(); i++) {
    ThreadStatus& ts = s_loggers[i]->m_threadStatus;

    timeval duration;
    if (ts.m_start.tv_sec > 0 && ts.m_done.tv_sec > 0) {
      timersub(&ts.m_done, &ts.m_start, &duration);
    } else if (ts.m_start.tv_sec > 0) {
      timersub(&current, &ts.m_start, &duration);
    } else {
      memset(&duration, 0, sizeof(duration));
    }

    ServerStatusThreadReport thread{};
    thread.id = (int64_t)ts.m_threadId;
    thread.tid = (int64_t)ts.m_threadPid;
    thread.req = ts.m_requestCount;
    thread.bytes = ts.m_writeBytes;
    thread.mode = ThreadModeString(ts.m_mode);

    if (ts.m_requestCount && (ts.m_mode != ThreadMode::Idling)) {
      ServerStatusRequestReport request{};
      request.vhost = ts.m_vhost;
      request.url = ts.m_url;
      request.endpoint = ts.m_endpoint;
      request.client = ts.m_clientIP;
      request.start = ts.m_start.tv_sec;
      request.duration_ms = (duration.tv_sec * 1000) + (duration.tv_usec / 1000);

      auto const stats = ts.m_mm->getStatsCopy();
      ServerStatusRequestMemoryReport memory{};

      memory.currentUsage = stats.usage();
      memory.currentAlloc = stats.capacity();
      memory.peakUsage = stats.peakUsage;
      memory.peakAlloc = stats.peakCap;
      memory.limit = ts.m_mm->getMemoryLimit();
      memory.currentMmUsage = stats.mmUsage();

      request.memory = std::move(memory);

      // Only in the event that we are currently in the process of an io, will
      // we output the iostatus, and ioInProcessDuationMicros
      if (ts.m_ioInProcess) {
        timespec now_ts;
        Timer::GetMonotonicTime(now_ts);
        ServerStatusRequestIOReport io;

        io.status = std::string(ts.m_ioName) + " " + ts.m_ioAddr;
        io.duration = gettime_diff_us(ts.m_ioStart, now_ts);

        request.io = std::move(io);
      }
      thread.request = std::move(request);
    }
    status.threads.emplace_back(thread);
  }

  return status;
}

std::string ServerStats::ReportStatus(Writer::Format format) {
  std::ostringstream out;
  std::unique_ptr<Writer> w;
  switch (format) {
    case Writer::Format::XML:
      w.reset(new XMLWriter(out));
      break;
    case Writer::Format::JSON:
      w.reset(new JSONWriter(out));
      break;
    case Writer::Format::HTML:
      w.reset(new HTMLWriter(out));
      break;
    default:
      return "";
  }

  ServerStatusReport status = ReportStatus();

  w->writeFileHeader();
  w->beginObject("status");

  w->beginObject("process");
  w->writeEntry("id", status.process.id);
  w->writeEntry("build", status.process.build);
  w->writeEntry("compiler", status.process.compiler);

  if (status.process.debug) {
    w->writeEntry("debug", "yes");
  }

  timeval up;
  up.tv_sec = status.process.up;
  up.tv_usec = 0;
  w->writeEntry("now", req::make<DateTime>(status.process.now)->
                       toString(DateTime::DateFormatCookie).data());
  if (status.process.up == -1) {
    w->writeEntry("start", "server not started");
  } else {
    w->writeEntry("start", req::make<DateTime>(status.process.start)->
                          toString(DateTime::DateFormatCookie).data());
  }

  // TODO(jtwarren): Deprecate this field (up) once we confirm there are no
  // usages and replace with "uptime" which is more deterministic and more
  // easily parsed.
  w->writeEntry("up", format_duration(up));
  w->writeEntry("uptime", status.process.up);

  w->endObject("process");

  w->beginList("threads");

  for (ServerStatusThreadReport thread : status.threads) {
    w->beginObject("thread");
    w->writeEntry("id", thread.id);
    w->writeEntry("tid", thread.tid);
    w->writeEntry("req", thread.req);
    w->writeEntry("bytes", thread.bytes);
    w->writeEntry("mode", thread.mode);

    if (thread.request.has_value()) {
      ServerStatusRequestReport request = thread.request.value();
      w->writeEntry("vhost", request.vhost);
      w->writeEntry("url", request.url);
      w->writeEntry("endpoint", request.endpoint);
      w->writeEntry("client", request.client);
      w->writeEntry("start", req::make<DateTime>(request.start)->
                    toString(DateTime::DateFormatCookie).data());
      int64_t duration_ms = request.duration_ms;
      timeval duration;
      duration.tv_sec = duration_ms / 1000;
      duration.tv_usec = (duration_ms % 1000) * 1000;
      w->writeEntry("duration", format_duration(duration));

      w->beginObject("memory");
      w->writeEntry("current usage", request.memory.currentUsage);
      w->writeEntry("current alloc", request.memory.currentAlloc);
      w->writeEntry("peak usage", request.memory.peakUsage);
      w->writeEntry("peak alloc", request.memory.peakAlloc);
      w->writeEntry("limit", request.memory.limit);
      w->writeEntry("current mm usage", request.memory.currentMmUsage);
      w->endObject("memory");

      // Only in the event that we are currently in the process of an io, will
      // we output the iostatus, and ioInProcessDuationMicros
      if (request.io.has_value()) {
        ServerStatusRequestIOReport io = request.io.value();
        w->writeEntry("iostatus", io.status);
        w->writeEntry("ioduration", io.duration);
      }
    }
    w->endObject("thread");
  }
  w->endList("threads");
  w->endObject("status");
  w->writeFileFooter();

  return out.str();
}


const StaticString
  s_ct("ct"),
  s_wt("wt");


///////////////////////////////////////////////////////////////////////////////

ServerStats::ThreadStatus::ThreadStatus() {
  memset(this, 0, offsetof(ThreadStatus, m_ioStatuses));
  m_threadId = Process::GetThreadId();
  m_threadPid = Process::GetThreadPid();
}

ServerStats::ServerStats() {
  assertx(Cfg::Stats::MaxSlot >= 2);
  if (Cfg::Stats::MaxSlot < 2) {
    Cfg::Stats::MaxSlot = 2;
  }
  m_slots.resize(Cfg::Stats::MaxSlot);
  Lock lock(s_lock, false);
  s_loggers.push_back(this);
}

ServerStats::~ServerStats() {
  // Remove this from the s_loggers vector
  Lock lock(s_lock, false);
  // Scan the vector looking for this instance of ServerStats. Scanning
  // the vector is not terribly efficient, but this doesn't happen often
  // when the server is in a steady state.
  for (auto& logger : s_loggers) {
    if (logger == this) {
      auto back = s_loggers.back();
      if (this != back) {
        Lock l(back->m_lock, false);
        for (auto i = 0u; i < m_slots.size(); ++i) {
          if (m_slots[i].m_time == back->m_slots[i].m_time) {
            Merge(back->m_slots[i], m_slots[i], KeyMap{});
          } else if (m_slots[i].m_time > back->m_slots[i].m_time) {
            back->m_slots[i] = m_slots[i];
          } // else, discard old data in m_slots[i]
        }
        logger = back;
      }
      s_loggers.pop_back();
      break;
    }
  }
}

void ServerStats::log(const std::string& name, int64_t value) {
  m_values[name] += value;
}

int64_t ServerStats::get(const std::string& name) {
  auto const iter = m_values.find(name);
  if (iter != m_values.end()) {
    return iter->second;
  }
  return 0;
}

void ServerStats::logPage(const std::string& /*url*/, int /*code*/) {
  m_threadStatus.m_mode = ThreadMode::Idling;
  gettimeofday(&m_threadStatus.m_done, 0);
  Lock lock(m_lock, false);
  auto const now = curr();
  auto const slot = now % Cfg::Stats::MaxSlot;
  auto& ts = m_slots[slot];
  if (ts.m_time != now) {
    ts.clear();
    ts.m_time = now;
  }
  ts.m_hits++;
  Merge(ts.m_values, m_values, KeyMap{});
}

void ServerStats::reset() {
  m_values.clear();
}

void ServerStats::logBytes(int64_t bytes) {
  m_threadStatus.m_writeBytes += bytes;
}

static void safe_copy(char* dest, const char* src, int max) {
  auto const len = strlen(src) + 1;
  dest[--max] = '\0';
  memcpy(dest, src, len > max ? max : len);
}

void ServerStats::startRequest(const char* url, const char* clientIP,
                               const char* vhost) {
  ++m_threadStatus.m_requestCount;

  m_threadStatus.m_mm = tl_heap.get();
  gettimeofday(&m_threadStatus.m_start, 0);
  memset(&m_threadStatus.m_done, 0, sizeof(m_threadStatus.m_done));
  m_threadStatus.m_mode = ThreadMode::Processing;
  m_threadStatus.m_ioStatuses.clear();

  safe_copy(m_threadStatus.m_url, url, sizeof(m_threadStatus.m_url));
  safe_copy(m_threadStatus.m_endpoint, "", sizeof(m_threadStatus.m_endpoint));
  safe_copy(m_threadStatus.m_clientIP, clientIP,
            sizeof(m_threadStatus.m_clientIP));
  safe_copy(m_threadStatus.m_vhost, vhost, sizeof(m_threadStatus.m_vhost));
}

void ServerStats::setEndpoint(const char* endpoint) {
  safe_copy(m_threadStatus.m_endpoint, endpoint,
            sizeof(m_threadStatus.m_endpoint));
}

void ServerStats::setThreadIOStatus(const char* name, const char* addr,
                                    int64_t usWallTime /* = -1 */) {
  bool starting = ((name && *name) || (addr && *addr));

  if (starting) {
    if (name) {
      safe_copy(m_threadStatus.m_ioName, name,
                sizeof(m_threadStatus.m_ioName));
    }
    if (addr) {
      safe_copy(m_threadStatus.m_ioAddr, addr,
                sizeof(m_threadStatus.m_ioAddr));
    }

    // Mark the current thread as being in the process of completing
    // an io, and record the time that the io started.
    m_threadStatus.m_ioInProcess = true;
    if (usWallTime < 0) {
      Timer::GetMonotonicTime(m_threadStatus.m_ioStart);
    }
  }

  if (!starting || usWallTime >= 0) {
    m_threadStatus.m_ioInProcess = false;

    if (Cfg::Stats::NetworkIO) {
      int64_t wt = usWallTime;
      if (wt < 0) {
        timespec now;
        Timer::GetMonotonicTime(now);
        wt = gettime_diff_us(m_threadStatus.m_ioStart, now);
      }

      const char* ioName = m_threadStatus.m_ioName;
      const char* ioAddr = m_threadStatus.m_ioAddr;

      if (Cfg::Stats::NetworkIO) {
        std::string key = ioName;
        if (*ioAddr) {
          key += ' '; key += ioAddr;
        }
        IOStatus& io = m_threadStatus.m_ioStatuses[key];
        ++io.count;
        io.wall_time += wt;
      }

    }
  }
}

Array ServerStats::getThreadIOStatuses() {
  IOStatusMap& status = m_threadStatus.m_ioStatuses;
  DictInit ret(status.size());
  for (auto const& iter : status) {
    ret.set(OptString(iter.first),
            make_dict_array(s_ct, iter.second.count,
                           s_wt, iter.second.wall_time));
  }
  status.clear();
  return ret.toArray();
}

///////////////////////////////////////////////////////////////////////////////

ServerStatsHelper::ServerStatsHelper(const char* section,
                                     uint32_t track /* = false */)
  : m_section(section), m_instStart(0), m_track(track) {
  if (Cfg::Stats::Web && Cfg::Stats::Enable) {
    Timer::GetMonotonicTime(m_wallStart);
#ifdef CLOCK_THREAD_CPUTIME_ID
    gettime(CLOCK_THREAD_CPUTIME_ID, &m_cpuStart);
#endif
    if (m_track & TRACK_HWINST) {
      m_instStart = HardwareCounter::GetInstructionCount();
    }
  }
}

ServerStatsHelper::~ServerStatsHelper() {
  if (Cfg::Stats::Web && Cfg::Stats::Enable) {
    timespec wallEnd;
    Timer::GetMonotonicTime(wallEnd);
#ifdef CLOCK_THREAD_CPUTIME_ID
    timespec cpuEnd;
    gettime(CLOCK_THREAD_CPUTIME_ID, &cpuEnd);
#endif

    logTime("page.wall.", m_wallStart, wallEnd);
#ifdef CLOCK_THREAD_CPUTIME_ID
    logTime("page.cpu.", m_cpuStart, cpuEnd);
#endif

    if (m_track & TRACK_MEMORY) {
      auto const stats = tl_heap->getStatsCopy();
      ServerStats::Log(std::string("mem.") + m_section, stats.peakUsage);
      ServerStats::Log(std::string("mem.allocated.") + m_section,
                       stats.peakCap);
      ServerStats::Log(std::string("mem.cumulative.") + m_section,
                       stats.totalAlloc);
    }

    if (m_track & TRACK_HWINST) {
      int64_t instEnd = HardwareCounter::GetInstructionCount();
      logTime("page.inst.", m_instStart, instEnd);
    }
  }
}

void ServerStatsHelper::logTime(const std::string& prefix,
                                const timespec& start, const timespec& end) {
  ServerStats::Log(prefix + m_section, gettime_diff_us(start, end));
}

void ServerStatsHelper::logTime(const std::string& prefix,
                                const int64_t& start, const int64_t& end) {
  assert(start >= 0 && end >= 0 && end >= start);
  ServerStats::Log(prefix + m_section, end - start);
}

///////////////////////////////////////////////////////////////////////////////

IOStatusHelper::IOStatusHelper(const char* name,
                               const char* address /* = NULL */,
                               int port /* = 0 */)
  : m_exeProfiler(RequestInfo::NetworkIO) {
  assertx(name && *name);
  if (Cfg::Stats::Web) {
    std::string msg;
    if (address) {
      msg = address;
    }
    if (port) {
      msg += ":";
      msg += folly::to<std::string>(port);
    }
    ServerStats::SetThreadIOStatus(name, msg.c_str());
  }
}

IOStatusHelper::~IOStatusHelper() {
  if (Cfg::Stats::Web) {
    ServerStats::SetThreadIOStatus(nullptr, nullptr);
  }
}

///////////////////////////////////////////////////////////////////////////////

static void set_curl_status(CURL* cp, CURLINFO info, const char* name,
                            const char* url) {
  double option;
  curl_easy_getinfo(cp, info, &option);
  if (option >= 0) {
    ServerStats::SetThreadIOStatus(name, url, option * 1000000);
  }
}

void set_curl_statuses(CURL* cp, const char* url) {
  set_curl_status(cp, CURLINFO_NAMELOOKUP_TIME,    "curl-namelookup",    url);
  set_curl_status(cp, CURLINFO_CONNECT_TIME,       "curl-connect",       url);
  set_curl_status(cp, CURLINFO_STARTTRANSFER_TIME, "curl-starttransfer", url);
  set_curl_status(cp, CURLINFO_PRETRANSFER_TIME,   "curl-pretransfer",   url);
}

///////////////////////////////////////////////////////////////////////////////

void server_stats_log_mutex(const std::string& stack, int64_t elapsed_us) {
  auto const prefix = "mutex." + stack;
  ServerStats::Log(prefix + ".hit", 1);
  ServerStats::Log(prefix + ".time", elapsed_us);
}

///////////////////////////////////////////////////////////////////////////////

namespace {
ServiceData::ExpensiveCounterCallback s_counters(
  [](ServiceData::CounterMap& counters) {
    const auto stats =
      ServerStats::Report(Cfg::Stats::TrackedKeys, "admin.server_stats");
    counters.insert(begin(stats), end(stats));
  },
  "admin.server_stats."
);
}

///////////////////////////////////////////////////////////////////////////////
}
