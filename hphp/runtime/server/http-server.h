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

#include "hphp/runtime/server/server.h"
#include "hphp/runtime/server/shutdown-stats.h"

#include "hphp/util/async-func.h"
#include "hphp/util/service-data.h"

#include <atomic>
#include <folly/MicroSpinLock.h>

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

struct HttpServer : Synchronizable, Server::ServerEventListener {
 public:
  static std::shared_ptr<HttpServer> Server;
  static time_t StartTime;
  static std::atomic<double> LoadFactor;
  static std::atomic_int QueueDiscount;
  static std::atomic_int SignalReceived;

 private:
  static std::atomic_int_fast64_t PrepareToStopTime;
  static time_t OldServerStopTime;
  static std::vector<ShutdownStat> ShutdownStats;
  static folly::MicroSpinLock StatsLock; // for ShutdownStats

 public:
  explicit HttpServer();
  ~HttpServer() override;

  /*
   * Try to run the various servers that this class controls.
   *
   * If any of them can't bind their appropriate port (or otherwise
   * fail their initialization steps), shut down the entire process
   * without running any atexit handlers.
   */
  void runOrExitProcess();
  /*
   * A separate method to do the same thing for admin server, which can
   * be started sooner than the other servers.
   */
  void runAdminServerOrExitProcess();

  // stop() cannot be called from a signal handler, use stopOnSignal() in that
  // case.
  void stop(const char* reason = nullptr);
  void stopOnSignal(int sig);

  bool isStopped() const { return m_stopped;}

  void flushLog();

  void serverStopped(HPHP::Server* server) override;

  HPHP::Server* getPageServer() { return m_pageServer.get(); }

  static uint64_t GetIOGateThreadCount();

  static void MarkShutdownStat(ShutdownEvent event);
  static void LogShutdownStats();
  static void ProfileFlush();

  static int64_t GetPrepareToStopTime() {
    // Make sure changes are seen right away after PrepareToStop().
    return PrepareToStopTime.load(std::memory_order_acquire);
  }
  /*
   * Tell the old server instance to (prepare to) stop.  Return true
   * if the old server acknowledges, or a previous such attempt to
   * stop it was made.  This function doesn't wait until the previous
   * server exits.  Nothing bad happens if the old server isn't there,
   * or is already in the process of stopping.  These functions are
   * designed to work when Cfg::Server::StopOld is set.
   *
   * Currently they are implemented through commands on admin port.
   * So they will not work if admin server is not present, or if the
   * new server and old server disagree on port and password for admin
   * server.
   */
  static bool ReduceOldServerLoad();
  static bool StopOldServer();

  /*
   * When running with Cfg::Server::StopOld, given a target
   * memory needed (Cfg::Server::RSSNeededMb), check memory
   * status, stop the old server when necessary, and wait for at most
   * RuntimeOption::OldServerWait seconds after trying to stop the old
   * server, before proceeding regardless of available memory. `final`
   * indicates whether this is the final wait (and thus must wait till
   * all the required memory is available).
   */
  static void CheckMemAndWait(bool final = false);

  // Helpers to decide whether it is safe to proceed till next check
  // point, or continue indefinitely.
  static bool CanStep(const MemInfo& mem, int64_t rss,
                      int64_t rssNeeded, int cacheFreeFactor);
  static bool CanContinue(const MemInfo& mem, int64_t rss,
                          int64_t rssNeeded, int cacheFreeFactor);

  static void EvictFileCache();
  static void PrepareToStop();

private:
  bool startServer(bool pageServer);
  void onServerShutdown();
  void waitForServers();
  void startupFailure(const std::string& msg);

  // pid file functions
  void createPid();
  void removePid();
  void killPid();

  // Allow cleanups (e.g., flush cached values into a database) using
  // PHP code when server stops.
  void playShutdownRequest(const std::string& fileName);

private:
  std::atomic<bool> m_stopping{false};
  bool m_stopped{false};
  const char* m_stopReason{nullptr};

  ServerPtr m_pageServer;
  ServerPtr m_secondaryPageServer;
  ServerPtr m_adminServer;
  ServiceData::CounterCallback m_counterCallback;
};

///////////////////////////////////////////////////////////////////////////////
}
