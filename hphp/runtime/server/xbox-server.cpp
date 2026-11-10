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

#include "hphp/runtime/server/xbox-server.h"

#include <folly/ScopeGuard.h>
#include "hphp/runtime/base/builtin-functions.h"
#include "hphp/runtime/base/request-fanout-limit-wrapper.h"
#include "hphp/runtime/ext/server/ext_server.h"
#include "hphp/runtime/server/xbox-request-handler.h"
#include "hphp/runtime/server/satellite-server.h"
#include "hphp/runtime/server/job-queue-vm-stack.h"
#include "hphp/runtime/server/server-task-event.h"
#include "hphp/util/configs/server.h"
#include "hphp/util/configs/xbox.h"
#include "hphp/util/job-queue.h"
#include "hphp/util/lock.h"
#include "hphp/util/logger.h"
#include "hphp/util/timer.h"
#include "hphp/system/systemlib.h"

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

const StaticString s_xbox("xbox");

///////////////////////////////////////////////////////////////////////////////

using std::string;

XboxTransport::XboxTransport(const folly::StringPiece message,
                             RequestId root_req_id,
                             const folly::StringPiece reqInitDoc /* = "" */)
    : m_refCount(0), m_done(false), m_code(0), m_event(nullptr) {
  Timer::GetMonotonicTime(m_queueTime);

  m_message.append(message.data(), message.size());
  m_reqInitDoc.append(reqInitDoc.data(), reqInitDoc.size());
  setRootRequestId(root_req_id);
  disableCompression(); // so we don't have to decompress during sendImpl()
}

const char *XboxTransport::getUrl() {
  if (!m_reqInitDoc.empty()) {
    return "xbox_process_call_message";
  }
  return Cfg::Xbox::ProcessMessageFunc.c_str();
}

std::string XboxTransport::getHeader(const char *name) {
  if (!strcasecmp(name, "Host")) return m_host;
  if (!strcasecmp(name, "ReqInitDoc")) return m_reqInitDoc;
  return "";
}

void XboxTransport::sendImpl(const void* data, int size, int code,
                             bool /*chunked*/, bool eom) {
  m_response.append((const char*)data, size);
  if (code) {
    m_code = code;
  }
  if (eom) {
    onSendEndImpl();
  }
}

void XboxTransport::onSendEndImpl() {
  Lock lock(this);
  if (m_done) {
    return;
  }
  m_done = true;
  if (m_event) {
    m_event->finish();
  }
  notify();
}

OptString XboxTransport::getResults(int &code, int timeout_ms /* = 0 */) {
  {
    Lock lock(this);
    while (!m_done) {
      if (timeout_ms > 0) {
        long long seconds = timeout_ms / 1000;
        long long nanosecs = (timeout_ms - seconds * 1000) * 1000;
        if (!wait(seconds, nanosecs)) {
          code = -1;
          return String::Empty();
        }
      } else {
        wait();
      }
    }
  }

  OptString response(m_response.c_str(), m_response.size(), CopyString);
  code = m_code;

  return response;
}

///////////////////////////////////////////////////////////////////////////////

static THREAD_LOCAL(std::shared_ptr<XboxServerInfo>, s_xbox_server_info);
static THREAD_LOCAL(std::string, s_xbox_prev_req_init_doc);
static THREAD_LOCAL(XboxRequestHandler, s_xbox_request_handler);

///////////////////////////////////////////////////////////////////////////////

struct XboxWorker
  : JobQueueWorker<XboxTransport*,Server*,true,false,JobQueueDropVMStack>
{
  void abortJob(XboxTransport *job) override {
    Logger::Warning("Job dropped by JobQueueDispatcher because of timeout in xbox.");
    auto const handler = createRequestHandler();
    handler->abortRequest(job);
    destroyRequestHandler();
    job->decRefCount();
  }

  void doJob(XboxTransport *job) override {
    try {
      // If this job or the previous job that ran on this thread have
      // a custom initial document, make sure we do a reset
      string reqInitDoc = job->getHeader("ReqInitDoc");
      *s_xbox_prev_req_init_doc = reqInitDoc;

      job->onRequestStart(job->getStartTimer());

      auto const handler = createRequestHandler();
      if (auto ctx = job->detachCliContext()) {
        handler->setCliContext(std::move(ctx).value());
      }
      handler->run(job);
      destroyRequestHandler();
      job->decRefCount();
    } catch (...) {
      Logger::Error("RpcRequestHandler leaked exceptions");
    }
  }
private:
  RequestHandler *createRequestHandler() {
    if (!*s_xbox_server_info) {
      *s_xbox_server_info = std::make_shared<XboxServerInfo>();
    }

    s_xbox_request_handler->setLogInfo(Cfg::Xbox::ServerInfoLogInfo);
    s_xbox_request_handler->setServerInfo(*s_xbox_server_info);
    return s_xbox_request_handler.get();
  }

  void destroyRequestHandler() {
    if (!s_xbox_request_handler.isNull()) {
      s_xbox_request_handler.destroy();
    }
  }

  void onThreadExit() override {
    if (!s_xbox_request_handler.isNull()) {
      s_xbox_request_handler.destroy();
    }
  }
};

///////////////////////////////////////////////////////////////////////////////

static ReadWriteMutex s_dispatchMutex;
static JobQueueDispatcher<XboxWorker> *s_dispatcher;
static size_t s_drainingThreadCount{0};
static ServiceData::CounterCallback* s_counters;

void XboxServer::Restart() {
  Stop();

  if (Cfg::Xbox::ServerInfoThreadCount > 0) {
    {
      WriteLock l(s_dispatchMutex);
      s_dispatcher = new JobQueueDispatcher<XboxWorker>
        (Cfg::Xbox::ServerInfoThreadCount,
         Cfg::Xbox::ServerInfoThreadCount,
         Cfg::Server::ThreadDropCacheTimeoutSeconds,
         Cfg::Server::ThreadDropStack,
         nullptr,
         INT_MAX,
         Cfg::Xbox::ServerInfoMaxJobQueuingMs,
         1,    // numPriorities default
         0,    // hugeCount default
         -1,   // initThreadCount default
         0,    // hugeStackKb default
         0,    // extraKb default
         Cfg::Xbox::ServerInfoThreadGroupSuffix
        );
      s_counters = new ServiceData::CounterCallback(
          [](ServiceData::CounterMap& counters) {
            // For the entire duration when the counters are registered, we make
            // sure `s_dispatcher` is initialized and won't be stopped. See
            // `XboxServer::Stop()` where we deregister the counters before
            // destroying `s_counters`.
            assertx(s_dispatcher);
            auto const& stats = s_dispatcher->getDispatcherStats();
            counters["xbox_inflight_requests"] = stats.activeThreads;
            counters["xbox_queued_requests"] = stats.queuedJobCount;
          },
          "xbox_"
      );
      if (Cfg::Server::TrackRequestFanout) {
        requestFanoutLimitInit(Cfg::Server::RequestFanoutLimit, Cfg::Server::ThreadCount);
      }

    }
    if (Cfg::Xbox::ServerInfoLogInfo) {
      Logger::Info("xbox server started");
    }
    s_dispatcher->start();
  }
}

void XboxServer::Stop() {
  if (!s_dispatcher) return;

  delete s_counters;
  s_counters = nullptr;
  JobQueueDispatcher<XboxWorker>* dispatcher = nullptr;
  size_t drainingCount = 0;
  {
    WriteLock l(s_dispatchMutex);
    if (!s_dispatcher) return;

    dispatcher = s_dispatcher;
    drainingCount = dispatcher->getLiveThreadCount();
    s_drainingThreadCount += drainingCount;
    s_dispatcher = nullptr;
  }

  SCOPE_EXIT {
    delete dispatcher;
    WriteLock l(s_dispatchMutex);
    s_drainingThreadCount -= drainingCount;
  };
  dispatcher->stop();
}

bool XboxServer::Enabled() {
  return s_dispatcher;
}

///////////////////////////////////////////////////////////////////////////////

const StaticString
  s_code("code"),
  s_response("response"),
  s_error("error"),
  s_localhost("localhost"),
  s_127_0_0_1("127.0.0.1");

static bool isLocalHost(const OptString& host) {
  return host.empty() || host == s_localhost || host == s_127_0_0_1;
}

///////////////////////////////////////////////////////////////////////////////

struct XboxTask : SweepableResourceData {
  DECLARE_RESOURCE_ALLOCATION(XboxTask)

  XboxTask(const OptString& message, const OptString& reqInitDoc = "", const RequestId root_req_id = RequestId()) {
    m_job = new XboxTransport(message.toCppString(), root_req_id, reqInitDoc.toCppString());
    m_job->incRefCount();
    if (cli_supports_clone()) {
      m_job->setCliContext(cli_clone_context());
    }
  }

  XboxTask(const XboxTask&) = delete;
  XboxTask& operator=(const XboxTask&) = delete;

  ~XboxTask() override {
    m_job->decRefCount();
  }

  XboxTransport *getJob() { return m_job;}

  CLASSNAME_IS("XboxTask")
  // overriding ResourceData
  const OptString& o_getClassNameHook() const override { return classnameof(); }

private:
  XboxTransport *m_job{nullptr};
};
IMPLEMENT_RESOURCE_ALLOCATION(XboxTask)

///////////////////////////////////////////////////////////////////////////////

OptResource XboxServer::TaskStart(
  const OptString& msg,
  const OptString& reqInitDoc /* = "" */,
  ServerTaskEvent<XboxServer, XboxTransport> *event /* = nullptr */,
  RequestId root_req_id /* = RequestId() */) {
  static auto xboxOverflowCounter =
    ServiceData::createTimeSeries("xbox_overflow",
                                  { ServiceData::StatsType::COUNT });
  static auto xboxQueuedCounter =
    ServiceData::createTimeSeries("xbox_queued",
                                  { ServiceData::StatsType::COUNT });
  {
    WriteLock l(s_dispatchMutex);
    if (s_dispatcher &&
        (s_dispatcher->getActiveWorker() <
         Cfg::Xbox::ServerInfoThreadCount ||
         s_dispatcher->getQueuedJobs() <
         Cfg::Xbox::ServerInfoMaxQueueLength)) {
      auto task = req::make<XboxTask>(msg, reqInitDoc, root_req_id);
      XboxTransport *job = task->getJob();
      job->incRefCount(); // paired with worker's decRefCount()
      xboxQueuedCounter->addValue(1);

      Transport *transport = g_context->getTransport();
      if (transport) {
        job->setHost(transport->getHeader("Host"));
      }

      if (event) {
        job->setAsioEvent(event);
        event->setJob(job);
      }

      // If server_uptime() is -1, it means we are not yet running in server mode
      // and are serving warmup requests. Excempt warmup requests from fanout enforcement.
      if (Cfg::Server::TrackRequestFanout && HHVM_FN(server_uptime)() >= 0) {
        requestFanoutLimitIncrement(root_req_id);
      }

      assertx(s_dispatcher);
      s_dispatcher->enqueue(job);
      g_context->incrXboxTasksStarted();

      return OptResource(std::move(task));
    }
  }

  auto hasXbox = Cfg::Xbox::ServerInfoThreadCount > 0;
  const char* errMsg;
  if (hasXbox && !s_dispatcher) {
    errMsg = "Cannot create Xbox task because Xbox server is shut down";
  } else if (hasXbox) {
    errMsg = "Cannot create new Xbox task because the Xbox queue has "
     "reached maximum capacity";
  } else {
     errMsg = "Cannot create new Xbox task because the Xbox is not enabled";
  }
  if (hasXbox) {
    xboxOverflowCounter->addValue(1);
  }

  throw_exception(SystemLib::AllocExceptionObject(errMsg));
  return OptResource();
}

bool XboxServer::TaskStatus(const OptResource& task) {
  return cast<XboxTask>(task)->getJob()->isDone();
}

int XboxServer::TaskResult(const OptResource& task, int timeout_ms, Variant *ret) {
  return TaskResult(cast<XboxTask>(task)->getJob(), timeout_ms, ret);
}

int XboxServer::TaskResult(XboxTransport *job, int timeout_ms, Variant *ret) {
  int code = 0;
  OptString response = job->getResults(code, timeout_ms);
  if (ret) {
    if (code == 200) {
      *ret =
        unserialize_from_string(response, VariableUnserializer::Type::Internal);
    } else {
      *ret = response;
    }
  }
  return code;
}

int XboxServer::GetActiveWorkers() {
  ReadLock l(s_dispatchMutex);
  return s_dispatcher ? s_dispatcher->getActiveWorker() : 0;
}

size_t XboxServer::GetLiveThreadCount() {
  ReadLock l(s_dispatchMutex);
  return s_drainingThreadCount +
    (s_dispatcher ? s_dispatcher->getLiveThreadCount() : 0);
}

int XboxServer::GetQueuedJobs() {
  ReadLock l(s_dispatchMutex);
  return s_dispatcher ? s_dispatcher->getQueuedJobs() : 0;
}

///////////////////////////////////////////////////////////////////////////////
}
