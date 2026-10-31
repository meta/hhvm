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

#include "hphp/runtime/server/http-server.h"

#include "hphp/runtime/base/apc-stats.h"
#include "hphp/runtime/base/backtrace.h"
#include "hphp/runtime/base/file-util.h"
#include "hphp/runtime/base/init-fini-node.h"
#include "hphp/runtime/base/memory-manager.h"
#include "hphp/runtime/base/runtime-option.h"
#include "hphp/runtime/debugger/debugger.h"
#include "hphp/runtime/ext/apc/ext_apc.h"
#include "hphp/runtime/ext/server/ext_server.h"
#include "hphp/runtime/vm/jit/mcgen-translate.h"
#include "hphp/runtime/server/admin-request-handler.h"
#include "hphp/runtime/server/http-request-handler.h"
#include "hphp/runtime/server/memory-stats.h"
#include "hphp/runtime/server/replay-transport.h"
#include "hphp/runtime/server/server-stats.h"
#include "hphp/runtime/server/warmup-request-handler.h"

#include "hphp/util/boot-stats.h"
#include "hphp/util/bump-mapper.h"
#include "hphp/util/configs/adminserver.h"
#include "hphp/util/configs/debugger.h"
#include "hphp/util/configs/eval.h"
#include "hphp/util/configs/server.h"
#include "hphp/util/logger.h"
#include "hphp/util/process.h"
#include "hphp/util/roar.h"
#include "hphp/util/struct-log.h"
#include "hphp/util/sync-signal.h"
#include "hphp/util/user-info.h"

#include <fmt/format.h>
#include <folly/Conv.h>
#include <folly/FileUtil.h>
#include <folly/ScopeGuard.h>
#include <folly/portability/Unistd.h>

#include <signal.h>
#include <fstream>

void DisableFork() __attribute__((__weak__));
void EnableForkLogging() __attribute__((__weak__));
// GCC GCOV API
extern "C" void __gcov_flush() __attribute__((__weak__));
// LLVM/clang API. See llvm-project/compiler-rt/lib/profile/InstrProfiling.h
extern "C" void __llvm_profile_write_file() __attribute__((__weak__));
extern "C" void __llvm_profile_set_filename(const char* filename) __attribute__((__weak__));

namespace HPHP {

///////////////////////////////////////////////////////////////////////////////
// statics

std::shared_ptr<HttpServer> HttpServer::Server;
time_t HttpServer::StartTime;
std::atomic<double> HttpServer::LoadFactor{1.0};
std::atomic_int HttpServer::QueueDiscount{0};
std::atomic_int HttpServer::SignalReceived{0};
std::atomic_int_fast64_t HttpServer::PrepareToStopTime{0};
time_t HttpServer::OldServerStopTime;
std::vector<ShutdownStat> HttpServer::ShutdownStats;
folly::MicroSpinLock HttpServer::StatsLock;

// signals upon which the server shuts down gracefully
static const int kTermSignals[] = { SIGHUP, SIGINT, SIGTERM, SIGUSR1 };

void log_signal(int sig, siginfo_t* info) {
  if (!Cfg::Eval::LogHttpServerSignalSource) return;

  auto const sname = strsignal(sig);
  std::string pname = "<unknown>";
  std::string uname = "<unknown>";

  folly::readFile(
    folly::to<std::string>("/proc/", info->si_pid, "/cmdline").data(),
    pname
  );

  try {
    UserInfo user{info->si_uid};
    uname = user.pw->pw_name;
  } catch (const Exception&) {
  }

  Logger::Error(
    "Received signal `%s' from %s via `%s' (UID: %i, PID: %i)",
    sname,
    uname.data(),
    pname.data(),
    info->si_uid,
    info->si_pid
  );
}

// runs in signal handler thread.
static void on_kill_server(int sig, siginfo_t* info) {
  static std::atomic_flag flag = ATOMIC_FLAG_INIT;
  if (flag.test_and_set()) return;      // it was already called once.

  if (HttpServer::Server) {
    // Stop handling signals, because the
    // server is already shutting down, and we don't want to use the default
    // handlers which may immediately terminate the process.
    ignore_sync_signals();

    // Write information to the error log to make determining the sender of the
    // signal easier.
    log_signal(sig, info);

    int zero = 0;
    HttpServer::SignalReceived.compare_exchange_strong(zero, sig);
    // SignalReceived may possibly be set in HttpServer::stopOnSignal().
    HttpServer::Server->stop();
  } else {
    reset_sync_signals();
    raise(sig);                         // invoke default handler
  }
  pthread_exit(nullptr);                // terminate signal handler thread
}

static void exit_on_timeout(int sig) {
  // Must only call async-signal-safe functions.
  kill(getpid(), SIGKILL);
  // we really shouldn't get here, but who knows.
  // abort so we catch it as a crash.
  abort();
}

///////////////////////////////////////////////////////////////////////////////

HttpServer::HttpServer() {
  LoadFactor = Cfg::Eval::InitialLoadFactor;

  // enabling mutex profiling, but it's not turned on
  LockProfiler::s_pfunc_profile = server_stats_log_mutex;

  int startingThreadCount = Cfg::Server::ThreadCount;
  if (Cfg::Server::WarmupThrottleRequestCount > 0) {
    startingThreadCount =
      std::min(startingThreadCount,
               Cfg::Server::WarmupThrottleThreadCount);
  }
  // The queue count is the ceiling setMaxThreadCount() clamps to, so it has to
  // be the largest worker count anything can later ask for, not the count we
  // boot with. ThreadTune and `hhvmctl set-server-thread-count` both raise
  // threads above Server.ThreadCount at runtime; sizing the queue from the same
  // baseline is what leaves them room to do it.
  const int maxQueueCount =
    static_cast<int>(serverThreadTuneMaxThreadCount(Cfg::Server::ThreadCount));
  auto serverFactory = ServerFactoryRegistry::getInstance()->getFactory
      (Cfg::Server::Type);
  const std::string address = Cfg::Server::FileSocket.empty()
    ? Cfg::Server::IP : Cfg::Server::FileSocket;
  ServerOptions options(address, Cfg::Server::Port,
    Cfg::Server::ThreadCount, startingThreadCount,
    maxQueueCount);
  options.m_useFileSocket = !Cfg::Server::FileSocket.empty();
  options.m_serverFD = Cfg::Server::PortFd;
  options.m_sslFD = Cfg::Server::SSLPortFd;
  options.m_hugeThreads = Cfg::Server::HugeThreadCount;
  options.m_hugeStackKb = Cfg::Server::HugeStackSizeKb;
  options.m_loop_sample_rate = Cfg::Server::LoopSampleRate;
  m_pageServer = serverFactory->createServer(options);
  m_pageServer->addServerEventListener(this);

  if (startingThreadCount != Cfg::Server::ThreadCount) {
    auto handlerFactory = std::make_shared<WarmupRequestHandlerFactory>(
      m_pageServer.get(),
      Cfg::Server::WarmupThrottleRequestCount,
      Cfg::Server::RequestTimeoutSeconds);
    m_pageServer->setRequestHandlerFactory([handlerFactory] {
      return handlerFactory->createHandler();
    });
  } else {
    m_pageServer->setRequestHandlerFactory<HttpRequestHandler>(
      Cfg::Server::RequestTimeoutSeconds);
  }

  if (Cfg::Server::EnableSSL) {
    m_pageServer->enableSSL(Cfg::Server::SSLPort);
  }

  if (!Cfg::Server::SecondaryType.empty()) {
    if (Cfg::Eval::AuthoritativeMode) {
      startupFailure("Secondary server is not supported in RepoAuthoritative (production) mode");
    } else {
      auto secondaryServerFactory = ServerFactoryRegistry::getInstance()->getFactory
        (Cfg::Server::SecondaryType);
      ServerOptions secondaryServerOptions(Cfg::Server::IP, Cfg::Server::SecondaryPort,
        Cfg::Server::ThreadCount, startingThreadCount,
        maxQueueCount);
      secondaryServerOptions.m_useFileSocket = false;
      secondaryServerOptions.m_serverFD = Cfg::Server::PortFd;
      secondaryServerOptions.m_sslFD = Cfg::Server::SSLPortFd;
      secondaryServerOptions.m_hugeThreads = Cfg::Server::HugeThreadCount;
      secondaryServerOptions.m_hugeStackKb = Cfg::Server::HugeStackSizeKb;
      secondaryServerOptions.m_loop_sample_rate = Cfg::Server::LoopSampleRate;
      m_secondaryPageServer = secondaryServerFactory->createServer(secondaryServerOptions);
      m_secondaryPageServer->addServerEventListener(this);

      if (startingThreadCount != Cfg::Server::ThreadCount) {
        auto secondaryHandlerFactory = std::make_shared<WarmupRequestHandlerFactory>(
          m_secondaryPageServer.get(),
          Cfg::Server::WarmupThrottleRequestCount,
          Cfg::Server::RequestTimeoutSeconds);
        m_secondaryPageServer->setRequestHandlerFactory([secondaryHandlerFactory] {
          return secondaryHandlerFactory->createHandler();
        });
      } else {
        m_secondaryPageServer->setRequestHandlerFactory<HttpRequestHandler>(
          Cfg::Server::RequestTimeoutSeconds);
      }
    }
  }

  ServerOptions admin_options(Cfg::AdminServer::IP,
                              Cfg::AdminServer::Port,
                              Cfg::AdminServer::ThreadCount);
  m_adminServer = serverFactory->createServer(admin_options);
  m_adminServer->setRequestHandlerFactory<AdminRequestHandler>(
    Cfg::Server::RequestTimeoutSeconds);

  m_counterCallback.init(
    [this](ServiceData::CounterMap& counters) {
      counters["ev_connections"] = m_pageServer->getLibEventConnectionCount();
      auto const uptime = HHVM_FN(server_uptime)();
      counters["uptime"] = uptime;
      counters["stopping_soon"] = HHVM_FN(server_is_prepared_to_stop)();

      auto const dispatcherStats = m_pageServer->getDispatcherStats();
      counters["inflight_requests"] = dispatcherStats.activeThreads;
      counters["max_threads"] = dispatcherStats.maxThreads;
      auto queued_requests = dispatcherStats.queuedJobCount;
      counters["queued_requests"] = queued_requests;
      counters["queued_requests_high"] =
        queued_requests > Cfg::Server::HighQueueingThreshold;

      // Temporary counter that is available only during a short uptime window.
      if (uptime > Cfg::GC::MemTrackStart && uptime < Cfg::GC::MemTrackEnd) {
        counters["windowed_rss"] = ProcStatus::adjustedRssKb();
        counters["windowed_low_mem"] = alloc::getLowMapped();
        counters["windowed_mid_mem"] = alloc::getMidMapped();
        counters["windowed_units"] = MemoryStats::Count(AllocKind::Unit);
        counters["windowed_classes"] = MemoryStats::Count(AllocKind::Class);
        counters["windowed_funcs"] = MemoryStats::Count(AllocKind::Func);
        counters["windowed_unit_size"] =
          MemoryStats::TotalSize(AllocKind::Unit);
        counters["windowed_class_size"] =
          MemoryStats::TotalSize(AllocKind::Class);
        counters["windowed_func_size"] =
          MemoryStats::TotalSize(AllocKind::Func);
        auto const& apc = APCStats::getAPCStats();
        counters["windowed_apc_entries"] = apc.totalEntries();
        counters["windowed_apc_key_size"] = apc.totalKeySize();
        counters["windowed_apc_value_size"] = apc.totalValueSize();
      }
    }
  );

  for (auto const sig : kTermSignals) {
    sync_signal_info(sig, on_kill_server);
  }
}

void HttpServer::onServerShutdown() {
  // Avoid running this multiple times
  static std::atomic_flag flag = ATOMIC_FLAG_INIT;
  if (flag.test_and_set()) return;

  SparseHeap::PrepareToStop();
#ifdef USE_JEMALLOC
  shutdown_slab_managers();
#if USE_JEMALLOC
  if (HPHP::alloc::BumpEmergencyMapper::
      s_emergencyFlag.load(std::memory_order_acquire)) {
    // Server is shutting down when it almost exhausted low memory.
    Logger::Error("emergency restart: terminating due to lowptr exhaustion!");
    if (StructuredLog::enabled()) {
      auto record = StructuredLogEntry{};
      record.setInt("uptime", HHVM_FN(server_uptime)());
      record.setInt("low_mapped", alloc::getLowMapped());
      record.setInt("mid_mapped", alloc::getMidMapped());
      StructuredLog::log("hhvm_emergency_restart", record);
    }
  }
#endif
#endif
  InitFiniNode::ServerFini();

  Eval::Debugger::Stop();
  if (Cfg::Debugger::EnableServer) {
    Logger::Info("debugger server stopped");
  }

  // When a new instance of HPHP has taken over our page server socket, stop our
  // admin server so it can acquire those ports.
  if (Cfg::AdminServer::Port) {
    m_adminServer->stop();
    m_adminServer->waitForEnd();
    Logger::Info("admin server stopped");
  }
}

void HttpServer::serverStopped(HPHP::Server* server) {
  Logger::Info("Page server stopped");
  assertx(server == m_pageServer.get() || (m_secondaryPageServer && server == m_secondaryPageServer.get()));
  removePid();

  auto sockFile = Cfg::Server::FileSocket;
  if (!sockFile.empty()) {
    unlink(sockFile.c_str());
  }

  LogShutdownStats();
}

void HttpServer::playShutdownRequest(const std::string& fileName) {
  if (fileName.empty()) return;
  Logger::Info("playing request upon shutdown %s", fileName.c_str());
  try {
    ReplayTransport rt;
    rt.replayInput(fileName.c_str());
    HttpRequestHandler handler(0);
    handler.run(&rt);
    if (rt.getResponseCode() == 200) {
      Logger::Info("successfully finished request: %s", rt.getUrl());
    } else {
      Logger::Error("request unsuccessful: %s", rt.getUrl());
    }
    Logger::FlushAll();
    HttpRequestHandler::GetAccessLog().flushAllWriters();
  } catch (...) {
    Logger::Error("got exception when playing request: %s",
                  fileName.c_str());
  }
}

HttpServer::~HttpServer() {
  m_counterCallback.deinit();
  stop();
}

void HttpServer::startupFailure(const std::string& msg) {
  Logger::Error(msg);
  Logger::Error("Shutting down due to failure(s) to bind in "
                "HttpServer::runAndExitProcess");
  // Logger flushes itself---we don't need to run any atexit handlers
  // (historically we've mostly just SEGV'd while trying) ...
  _Exit(HPHP_EXIT_FAILURE);
}


void HttpServer::runAdminServerOrExitProcess() {
  if (Cfg::AdminServer::Port) {
    if (!startServer(false)) {
      startupFailure("Unable to start admin server");
      not_reached();
    }
    Logger::Info("admin server started");
  }
}

void HttpServer::runOrExitProcess() {
  if (!RuntimeOption::InstanceId.empty()) {
    std::string msg = "Starting instance " + RuntimeOption::InstanceId;
    if (!RuntimeOption::DeploymentId.empty()) {
      msg += " from deployment " + RuntimeOption::DeploymentId;
    }
    Logger::Info(msg);
  }

  block_sync_signals_and_start_handler_thread();

  ServerStats::Clear();                 // Clear stats from warmup requests

  if (Cfg::Server::Port) {
    if (!startServer(true)) {
      startupFailure("Unable to start page server");
      not_reached();
    }
    Logger::Info("page server started");
  }

  StartTime = time(nullptr);

  // If we haven't already, start the admin server.
  // We can't start the admin server early when hotswap is enabled because
  // it might result in killing ourself instead of the old server.
  if (Cfg::Server::StopOld) {
    runAdminServerOrExitProcess();
  }

  if (!Eval::Debugger::StartServer()) {
    startupFailure("Unable to start debugger server");
    not_reached();
  } else if (Cfg::Debugger::EnableServer) {
    Logger::Info("debugger server started");
  }

  BootStats::markStart("servers started");
  try {
    InitFiniNode::ServerInit();
  } catch (std::exception& e) {
    startupFailure(
      fmt::format("Exception in InitFiniNode::ServerInit(): {}",
                     e.what()));
  }

  {
    BootStats::mark("servers started");
    Logger::Info("all servers started");
    if (!Cfg::Server::ForkingEnabled) {
      if (DisableFork) {
        // We should not fork from the server process.  Use light process
        // instead.  This will intercept subsequent fork() calls and make them
        // fail immediately.
        DisableFork();
      } else {
        // Otherwise, the binary we are building is not HHVM.  It is probably
        // some tests, don't intercept fork().
        Logger::Warning("ignored runtime option Server.Forking.Enabled=false");
      }
    } else {
#if FOLLY_HAVE_PTHREAD_ATFORK
      pthread_atfork(nullptr, nullptr,
                     [] {
                       Process::OOMScoreAdj(1000);
                     });
#endif
    }
    if (Cfg::Server::ForkingLogForkAttempts) {
      if (EnableForkLogging) {
        EnableForkLogging();
      } else {
        // the binary we are building is not HHVM.  It is probably
        // some tests, don't intercept fork().
        Logger::Warning("ignored runtime option Server.Forking.Log=true");
      }
    }
    if (Cfg::Eval::ServerOOMAdj < 0) {
      // Avoid HHVM getting killed when a forked process uses too much memory.
      // A positive adjustment makes it more likely for the server to be killed,
      // and that's not what we want.
      Process::OOMScoreAdj(Cfg::Eval::ServerOOMAdj);
    }
    createPid();
    Lock lock(this);
    BootStats::markFromStart("start_server");
    if (!jit::mcgen::retranslateAllPending()) {
      BootStats::done(Cfg::Server::LogBootStats);
    } // else we log after retranslateAll finishes

    // Play extended warmup requests after server starts running. This works on
    // jumpstart seeders, and on sandboxes.
    if (isJitSerializing() || !Cfg::Repo::Authoritative) {
      replayExtendedWarmupRequests();
    }
    // continuously running until /stop is received on admin server
    while (!m_stopped) {
      wait();
    }
    Logger::Info("Server is stopping");

    if (m_stopReason) {
      Logger::Warning("Server stopping with reason: %s", m_stopReason);
    } else if (auto signo = SignalReceived.load(std::memory_order_acquire)) {
      Logger::Warning("Server stopping with reason: %s", strsignal(signo));
    }
  }

  if (Cfg::Server::Port) {
    Logger::Info("stopping page server");
    m_pageServer->stop();
  }
  if (m_secondaryPageServer) {
    Logger::Info("stopping secondary page server");
    m_secondaryPageServer->stop();
  }
  onServerShutdown();

  EvictFileCache();

  waitForServers();
  // Log APC samples after all requests finish.
  apc_sample_by_size();
  // Log static string samples after all requests finish.
  log_static_strings();
  playShutdownRequest(Cfg::Server::CleanupRequest);
}

void HttpServer::waitForServers() {
  if (Cfg::Server::Port) {
    m_pageServer->waitForEnd();
  }
  if (Cfg::AdminServer::Port) {
    m_adminServer->waitForEnd();
  }
  if (m_secondaryPageServer) {
    m_secondaryPageServer->waitForEnd();
  }
  // all other servers invoke waitForEnd on stop
}

void HttpServer::ProfileFlush() {
  if (__gcov_flush) {
    Logger::Info("Flushing profile");
    __gcov_flush();
  }
  // If running with ROAR, this calls into the profiling runtime included with
  // ROAR. ROAR expects to be in full control of the runtime, so this has to be
  // disabled.
  if (!use_roar && __llvm_profile_write_file) {
    Logger::Info("Flushing profile");
    __llvm_profile_write_file();
    __llvm_profile_set_filename("/dev/null");
  }
}

void HttpServer::stop(const char* stopReason) {
  if (m_stopping.exchange(true)) return;

  ProfileFlush();
  // Let all worker threads know that the server is shutting down. If some
  // request installed a PHP-level signal handler through `pcntl_signal(SIGTERM,
  // handler_func)`, `handler_func()` will run in the corresponding request
  // context, to perform cleanup tasks. If no handler is registered, it is
  // ignored.
  RequestInfo::BroadcastSignal(SIGTERM);

  // we're shutting down flush http logs
  Logger::Info("Flushing http logs");
  Logger::FlushAll();
  HttpRequestHandler::GetAccessLog().flushAllWriters();
  Process::OOMScoreAdj(1000);
  MarkShutdownStat(ShutdownEvent::SHUTDOWN_INITIATED);

  if (Cfg::Server::KillOnTimeout) {
    int totalWait =
      Cfg::Server::PreShutdownWait +
      Cfg::Server::ShutdownListenWait +
      Cfg::Server::GracefulShutdownWait;

    if (totalWait > 0) {
      // Use a killer thread to _Exit() after totalWait seconds.  If
      // the main thread returns before that, the killer thread will
      // exit.  So don't do join() on this thread.
      auto killer = std::thread([totalWait] {
          sched_param param{};
          param.sched_priority = 5;
          pthread_setschedparam(pthread_self(), SCHED_RR, &param);
          /* sleep override */
          std::this_thread::sleep_for(std::chrono::seconds{totalWait});
          _Exit(HPHP_SHUTDOWN_TIMEOUT);
        });
      killer.detach();
    }
  }

  Lock lock(this);
  m_stopped = true;
  m_stopReason = stopReason;
  Logger::Info("Waking up server thread to stop");
  notify();
}

void HttpServer::stopOnSignal(int sig) {
  int zero = 0;
  if (SignalReceived.compare_exchange_strong(zero, sig) &&
      Cfg::Server::KillOnTimeout) {
    auto const totalWait =
      Cfg::Server::PreShutdownWait +
      Cfg::Server::ShutdownListenWait +
      Cfg::Server::GracefulShutdownWait;
    // In case HttpServer::stop() isn't invoked in the synchronous signal
    // handler.
    if (totalWait > 0) {
        signal(SIGALRM, exit_on_timeout);
        sigset_t s;
        sigemptyset(&s);
        sigaddset(&s, SIGALRM);
        pthread_sigmask(SIG_UNBLOCK, &s, nullptr);
        alarm(totalWait);
    }
  }
  // Invoke on_kill_server() in the synchronous signal handler thread. We cannot
  // directly call HttpServer::stop() here directly, because this function must
  // be asynchronous-signal safe.
  kill(getpid(), SIGTERM);
}

void HttpServer::EvictFileCache() {
  // In theory, the kernel should do just as well even if we don't
  // explicitly advise files out.  But we can do it anyway when we
  // need more free memory, e.g., when a new instance of the server is
  // about to start.
  advise_out(Cfg::Repo::Path);
  advise_out(Cfg::Server::FileCache);
}

void HttpServer::PrepareToStop() {
  MarkShutdownStat(ShutdownEvent::SHUTDOWN_PREPARE);
  PrepareToStopTime.store(time(nullptr), std::memory_order_release);
  EvictFileCache();
  SparseHeap::PrepareToStop();
#ifdef USE_JEMALLOC
  shutdown_slab_managers();
#endif
}

void HttpServer::createPid() {
  if (!RuntimeOption::PidFile.empty()) {
    FILE * f = fopen(RuntimeOption::PidFile.c_str(), "w");
    if (f) {
      auto const pid = getpid();
      char buf[64];
      snprintf(buf, sizeof(buf), "%" PRId64, (int64_t)pid);
      fwrite(buf, strlen(buf), 1, f);
      fclose(f);
    } else {
      Logger::Error("Unable to open pid file %s for write",
                    RuntimeOption::PidFile.c_str());
    }
  }
}

void HttpServer::removePid() {
  if (!RuntimeOption::PidFile.empty()) {
    unlink(RuntimeOption::PidFile.c_str());
  }
}

void HttpServer::killPid() {
  if (!RuntimeOption::PidFile.empty()) {
    std::ifstream in(RuntimeOption::PidFile);
    int64_t pid;
    if ((in >> pid) && pid > 0) {
      in.close();
      kill((pid_t)pid, SIGKILL);
      return;
    }
    Logger::Error("Unable to read pid file %s for any meaningful pid",
                  RuntimeOption::PidFile.c_str());
  }
}

static bool sendAdminCommand(const char* cmd) {
  if (Cfg::AdminServer::Port <= 0) return false;
  std::string host = Cfg::AdminServer::IP;
  if (host.empty()) host = "localhost";
  auto passwords = Cfg::AdminServer::Passwords;
  if (passwords.empty() && !Cfg::AdminServer::Password.empty()) {
    passwords.insert(Cfg::AdminServer::Password);
  }
  auto passwordIter = passwords.begin();
  do {
    std::string url;
    if (passwordIter != passwords.end()) {
      url = fmt::format("http://{}:{}/{}?auth={}", host,
                           Cfg::AdminServer::Port,
                           cmd, *passwordIter);
      ++passwordIter;
    } else {
      url = fmt::format("http://{}:{}/{}", host,
                           Cfg::AdminServer::Port, cmd);
    }
    if (CURL* curl = curl_easy_init()) {
      curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
      curl_easy_setopt(curl, CURLOPT_NOBODY, 1);
      curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1);
      curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3);
      curl_easy_setopt(curl, CURLOPT_USERAGENT, "hhvm-internal/1.0");
      auto code = curl_easy_perform(curl);
      curl_easy_cleanup(curl);
      if (code == CURLE_OK) {
        Logger::Info("sent %s via admin port", cmd);
        return true;
      }
    }
  } while (passwordIter != passwords.end());
  return false;
}

bool HttpServer::ReduceOldServerLoad() {
  if (!Cfg::Server::StopOld) return false;
  if (OldServerStopTime > 0) return true;
  Logger::Info("trying to reduce load of the old HPHP server");
  return sendAdminCommand("prepare-to-stop");
}

bool HttpServer::StopOldServer() {
  if (OldServerStopTime > 0) return true;
  SCOPE_EXIT { OldServerStopTime = time(nullptr); };
  Logger::Info("trying to shut down old HPHP server by /stop command");
  return sendAdminCommand("stop");
}

// Return the estimated amount of memory that can be safely taken into
// the current process RSS.
static inline int64_t availableMemory(const MemInfo& mem, int64_t rss,
                                      int factor) {
  // Estimation of page cache that are readily evictable without
  // causing big memory pressure.  We consider it safe to take
  // `pageFreeFactor` percent of cached pages (excluding those used by
  // the current process, estimated to be equal to the current RSS)
  auto const otherCacheMb = std::max((int64_t)0,  mem.cachedMb - rss);
  auto const availableMb = mem.freeMb + otherCacheMb * factor / 100;
  return availableMb;
}

bool HttpServer::CanContinue(const MemInfo& mem, int64_t rssMb,
                             int64_t rssNeeded, int cacheFreeFactor) {
  if (!mem.valid()) return false;
  if (mem.freeMb < Cfg::Server::CriticalFreeMb) return false;
  auto const availableMb = availableMemory(mem, rssMb, cacheFreeFactor);
  auto const result = (rssMb + availableMb >= rssNeeded);
  if (result) assertx(CanStep(mem, rssMb, rssNeeded, cacheFreeFactor));
  return result;
}

bool HttpServer::CanStep(const MemInfo& mem, int64_t rssMb,
                         int64_t rssNeeded, int cacheFreeFactor) {
  if (!mem.valid()) return false;
  if (mem.freeMb < Cfg::Server::CriticalFreeMb) return false;
  auto const availableMb = availableMemory(mem, rssMb, cacheFreeFactor);
  // Estimation of the memory needed till the next check point.  Since
  // the current check point is not the last one, we try to be more
  // optimistic, by assuming that memory requirement won't grow
  // drastically between successive check points, and that it won't
  // grow over our estimate.
  auto const neededToStep = std::min(rssNeeded / 4, rssNeeded - rssMb);
  return (availableMb >= neededToStep);
}

void HttpServer::CheckMemAndWait(bool final) {
  if (!Cfg::Server::StopOld) return;
  if (Cfg::Server::StopOldWait <= 0) return;

  auto const rssNeeded = Cfg::Server::RSSNeededMb;
  auto const factor = Cfg::Server::CacheFreeFactor;
  do {
    // Don't wait too long
    if (OldServerStopTime > 0 &&
        time(nullptr) - OldServerStopTime >= Cfg::Server::StopOldWait) {
      return;
    }

    auto const rssMb = Process::GetMemUsageMb();
    MemInfo memInfo;
    if (!Process::GetMemoryInfo(memInfo)) {
      Logger::Error("Failed to obtain memory information");
      HttpServer::StopOldServer();
      return;
    }
    Logger::FInfo("Memory pressure check: free/cached/buffers {}/{}/{} "
                  "currentRss {}Mb, required {}Mb.",
                  memInfo.freeMb, memInfo.cachedMb, memInfo.buffersMb,
                  rssMb, rssNeeded);

    if (!final) {
      if (CanStep(memInfo, rssMb, rssNeeded, factor)) return;
    } else {
      if (CanContinue(memInfo, rssMb, rssNeeded, factor)) return;
    }

    // OK, we don't have enough memory, let's do something.
    HttpServer::StopOldServer();  // nop if already called before
    /* sleep override */ sleep(1);
  } while (true); // Guaranteed to return in the loop, at least upon timeout.
  not_reached();
}

void HttpServer::MarkShutdownStat(ShutdownEvent event) {
  if (!Cfg::Eval::LogServerRestartStats) return;
  std::lock_guard<folly::MicroSpinLock> lock(StatsLock);
  MemInfo mem;
  Process::GetMemoryInfo(mem);
  auto const rss = Process::GetMemUsageMb();
  auto const requests = requestCount();
  if (event == ShutdownEvent::SHUTDOWN_PREPARE) {
    ShutdownStats.clear();
    ShutdownStats.reserve(ShutdownEvent::kNumEvents);
  }
#ifndef NDEBUG
  if (!ShutdownStats.empty()) {
    assertx(ShutdownStats.back().event <= event);
  }
#endif
  ShutdownStats.push_back({event, time(nullptr), mem, rss, requests});
}

void HttpServer::LogShutdownStats() {
  if (!Cfg::Eval::LogServerRestartStats) return;
  StructuredLogEntry entry;
  std::lock_guard<folly::MicroSpinLock> lock(StatsLock);
  if (ShutdownStats.empty()) return;
  for (size_t i = 0; i < ShutdownStats.size(); ++i) {
    const auto& stat = ShutdownStats[i];
    auto const eventName = stat.eventName();
    entry.setInt(fmt::format("{}_rss", eventName), stat.rss);
    entry.setInt(fmt::format("{}_free", eventName),
                 stat.memUsage.freeMb);
    entry.setInt(fmt::format("{}_cached", eventName),
                 stat.memUsage.cachedMb);
    entry.setInt(fmt::format("{}_buffers", eventName),
                 stat.memUsage.buffersMb);
    // Log the difference since last event, if available
    if (i > 0) {
      const auto& last = ShutdownStats[i - 1];
      auto const lastEvent = last.eventName();
      entry.setInt(fmt::format("{}_duration", lastEvent),
                   stat.time - last.time);
      entry.setInt(fmt::format("{}_requests", lastEvent),
                   stat.requestsServed - last.requestsServed);
      entry.setInt(fmt::format("{}_rss_delta", lastEvent),
                   stat.rss - last.rss);
    }
  }
  StructuredLog::log("webserver_shutdown_timing", entry);
  ShutdownStats.clear();
}

///////////////////////////////////////////////////////////////////////////////
// page server

bool HttpServer::startServer(bool pageServer) {
  int port = pageServer ?
    Cfg::Server::Port : Cfg::AdminServer::Port;

  // 1. try something nice
  for (unsigned int i = 0; i < 60; i++) {
    try {
      if (pageServer) {
        m_pageServer->start();
        if (m_secondaryPageServer) {
          m_secondaryPageServer->start();
        }
      } else {
        m_adminServer->start();
      }
      return true;
    } catch (FailedToListenException& e) {
      if (Cfg::Server::ExitOnBindFail) return false;

      StopOldServer();

      if (errno == EACCES) {
        Logger::Error("%s: Permission denied.", e.what());
        return false;
      }

      if (pageServer && !Cfg::Server::FileSocket.empty()) {
        if (i == 0) {
          Logger::Info("Unlinking unused socket at %s",
                       Cfg::Server::FileSocket.c_str());
        }
        struct stat stat_buf;
        if (stat(Cfg::Server::FileSocket.c_str(), &stat_buf) == 0
            && S_ISSOCK(stat_buf.st_mode)) {
          std::string cmd = "bash -c '! fuser ";
          cmd += Cfg::Server::FileSocket;
          cmd += "'";
          if (FileUtil::ssystem(cmd.c_str()) == 0) {
            unlink(Cfg::Server::FileSocket.c_str());
          }
        }
      }
      sleep(1);
    }
  }

  // 2. try something harsh
  if (Cfg::Server::HarshShutdown) {
    for (unsigned int i = 0; i < 5; i++) {
      try {
        if (pageServer) {
          m_pageServer->start();
          if (m_secondaryPageServer) {
            m_secondaryPageServer->start();
          }
        } else {
          m_adminServer->start();
        }
        return true;
      } catch (FailedToListenException& ) {
        if (i == 0) {
          Logger::Info("shutting down old HPHP server by pid file");
        }
        killPid();
        sleep(1);
      }
    }
  }

  // 3. try something evil
  if (Cfg::Server::EvilShutdown) {
    for (unsigned int i = 0; i < 60; i++) {
      try {
        if (pageServer) {
          m_pageServer->start();
          if (m_secondaryPageServer) {
            m_secondaryPageServer->start();
          }
        } else {
          m_adminServer->start();
        }
        return true;
      } catch (FailedToListenException& ) {
        if (pageServer && !Cfg::Server::FileSocket.empty()) {
          if (i == 0) {
            Logger::Info("unlinking socket at %s",
                         Cfg::Server::FileSocket.c_str());
          }

          struct stat stat_buf;
          if (stat(Cfg::Server::FileSocket.c_str(), &stat_buf) == 0
              && S_ISSOCK(stat_buf.st_mode)) {
            unlink(Cfg::Server::FileSocket.c_str());
          }
        } else {
          if (i == 0) {
            Logger::Info("killing anything listening on port %d", port);
          }

          std::string cmd = "lsof -t -i :";
          cmd += folly::to<std::string>(port);
          cmd += " | xargs kill -9";
          FileUtil::ssystem(cmd.c_str());
        }
        sleep(1);
      }
    }
  }

  return false;
}

///////////////////////////////////////////////////////////////////////////////
}
