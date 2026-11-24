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

#include <string>

#include <folly/Conv.h>
#include <fmt/format.h>

#include "hphp/runtime/base/timestamp.h"
#include "hphp/runtime/server/access-log.h"
#include "hphp/runtime/server/http-server.h"
#include "hphp/runtime/server/request-uri.h"
#include "hphp/runtime/server/server-note.h"
#include "hphp/runtime/server/server-stats.h"
#include "hphp/runtime/server/server.h"
#include "hphp/runtime/server/transport.h"
#include "hphp/runtime/server/virtual-host.h"
#include "hphp/util/hardware-counter.h"
#include "hphp/util/timer.h"
#include "hphp/util/service-data.h"

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

struct ClassicWriter final : LogWriter {
  ClassicWriter(const AccessLogFileData& alfd, LogChannel chan)
    : LogWriter(chan)
    , m_logdata(alfd)
  {}
  ~ClassicWriter() override;
  void init(const std::string& username,
            AccessLog::GetThreadDataFunc fn) override;
  void write(Transport* transport, const VirtualHost* vhost) override;
  const static std::string handle;
private:
  const AccessLogFileData m_logdata;
  static bool parseConditions(const char*& fmt, int code);
  static std::string parseArgument(const char*& fmt);
  static void skipField(const char*& fmt);
};

struct FieldGenerator {
  FieldGenerator(Transport* t, const VirtualHost* vh,
                 AccessLog::ThreadData* tdata)
    : transport(t)
    , vhost(vh)
    , threadData(tdata)
  {}
  template<typename T>
  bool gen(char field, const std::string& arg, T& out);
private:
  static std::string escapeData(const char* s, int len);
  Transport* transport;
  const VirtualHost* vhost;
  AccessLog::ThreadData* threadData;
};

template<typename T>
bool FieldGenerator::gen(char field, const std::string& arg, T& out) {
  int responseSize = transport->getResponseSize();
  int code = transport->getResponseCode();

  switch (field) {
  case 'b':
    if (responseSize == 0) return false;
    [[fallthrough]];
  case 'B':
    out = folly::to<T>(responseSize);
    break;
  case 'c':
    {
      if (arg.empty()) return false;
      std::string config = IniSetting::Get(arg);
      if (config.empty()) return false;
      out = folly::to<T>(config);
    }
    break;
  case 'C':
    {
      if (arg.empty()) return false;
      std::string cookie = transport->getCookie(arg);
      if (cookie.empty()) return false;
      out = folly::to<T>(escapeData(cookie.c_str(), cookie.size()));
    }
    break;
  case 'D':
    {
      struct timespec now;
      Timer::GetMonotonicTime(now);
      out = folly::to<T>(gettime_diff_us(transport->getWallTime(), now));
    }
    break;
  case 'd':
    {
#ifdef CLOCK_THREAD_CPUTIME_ID
      struct timespec now;
      gettime(CLOCK_THREAD_CPUTIME_ID, &now);
      out = folly::to<T>(gettime_diff_us(transport->getCpuTime(), now));
#else
      return false;
#endif
    }
    break;
  case 'h':
    {
       std::string host = transport->getRemoteHost();
       if (host.empty()) host = transport->getRemoteAddr();
       out = folly::to<T>(host);
    }
    break;
  case 'H':
    if (arg.empty()) return false;
    out = folly::to<T>(ServerStats::Get(arg));
    break;
  case 'i':
    {
      if (arg.empty()) return false;
      std::string header = transport->getHeader(arg.c_str());
      if (header.empty()) return false;
      if (vhost && vhost->hasLogFilter() &&
          strcasecmp(arg.c_str(), "Referer") == 0) {
        out = folly::to<T>(vhost->filterUrl(header));
      } else {
        out = folly::to<T>(header);
      }
    }
    break;
  case 'I':
    out = folly::to<T>(transport->getRequestSize());
    break;
  case 'j':
    {
      auto static jitMaturityCounter =
        ServiceData::createCounter("jit.maturity");
      if (jitMaturityCounter) {
        out = folly::to<T>(jitMaturityCounter->getValue());
      }
    }
    break;
  case 'm':
    {
      const char *method = transport->getMethodName();
      if (!method || !method[0]) return false;
      out = folly::to<T>(method);
    }
    break;
  case 'M':
    // current Unix timestamp in microseconds
    out = folly::to<T>(Timer::GetCurrentTimeMicros());
    break;
  case 'n':
    if (arg.empty()) return false;
    {
      OptString note = ServerNote::Get(arg);
      if (note.isNull()) return false;
      out = folly::to<T>(note.data());
    }
    break;
  case 'o':
    out = folly::to<T>(ServerStats::Get("request.memory_exceeded.non_psp"));
    break;
  case 'O':
    out = folly::to<T>(ServerStats::Get("request.memory_exceeded.psp"));
    break;
  case 'p':
    out = folly::to<T>(ServerStats::Get("request.timed_out.non_psp"));
    break;
  case 'P':
    out = folly::to<T>(ServerStats::Get("request.timed_out.psp"));
    break;
  case 'q':
    {
      String b, q;
      RequestURI::splitURL(transport->getUrl(), b, q);
      if (q.isNull() || q.empty()) return false;
      out = folly::to<T>(q.c_str());
    }
    break;
  case 'r':
    {
      const char *method = transport->getMethodName();
      if (!method || !method[0]) return false;
      const char *url = transport->getUrl();
      out = folly::to<T>(fmt::format(
        "{} {} HTTP/{}", method,
        (vhost && vhost->hasLogFilter()) ?  vhost->filterUrl(url) : url,
        transport->getHTTPVersion()
      ));
    }
    break;
  case 's':
    out = folly::to<T>(code);
    break;
  case 'S':
    // %S is not defined in Apache, we grab it here
    {
      const std::string& info(transport->getResponseInfo());
      if (info.empty()) return false;
      out = folly::to<T>(info);
    }
    break;
  case 't':
    {
      const char *fmt = arg.empty() ? "[%d/%b/%Y:%H:%M:%S %z]" : arg.c_str();
      char buf[256];
      time_t rawtime;
      struct tm timeinfo;
      time(&rawtime);
      localtime_r(&rawtime, &timeinfo);
      strftime(buf, 256, fmt, &timeinfo);
      out = folly::to<T>(buf);
    }
    break;
  case 'T':
    out = folly::to<T>(threadData ?
                       TimeStamp::Current() - threadData->startTime : 0);
    break;
  case 'U':
    {
      OptString b, q;
      RequestURI::splitURL(transport->getUrl(), b, q);
      out = folly::to<T>(b.c_str());
    }
    break;
  case 'w':
    // server uptime
    out = folly::to<T>(TimeStamp::Current() - HttpServer::StartTime);
    break;
  case 'v':
    {
      std::string host = transport->getHeader("Host");
      const std::string &sname = VirtualHost::GetCurrent()->serverName(host);
      if (sname.empty() || Cfg::Server::ForceServerNameToHeader) {
        out = folly::to<T>(host);
      } else {
        out = folly::to<T>(sname);
      }
    }
    break;
  case 'Y':
    {
      int64_t now = HardwareCounter::GetInstructionCount();
      out = folly::to<T>(now - transport->getInstructions());
    }
    break;
  case 'y':
    out = folly::to<T>(ServerStats::Get("page.inst.psp"));
    break;
  case 'Z':
     out = folly::to<T>(ServerStats::Get("page.wall.psp"));
     break;
  case 'z':
     out = folly::to<T>(ServerStats::Get("page.cpu.psp"));
     break;
  default:
    return false;
  }
  return true;
}

///////////////////////////////////////////////////////////////////////////////
}
