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

#include <fmt/format.h>

#include "hphp/runtime/server/pagelet-server.h"

#include "hphp/runtime/server/transport.h"
#include "hphp/runtime/server/http-request-handler.h"
#include "hphp/runtime/server/upload.h"
#include "hphp/runtime/server/job-queue-vm-stack.h"
#include "hphp/runtime/server/host-health-monitor.h"
#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/base/array-iterator.h"
#include "hphp/runtime/base/builtin-functions.h"
#include "hphp/runtime/base/string-buffer.h"
#include "hphp/runtime/ext/server/ext_server.h"
#include "hphp/util/boot-stats.h"
#include "hphp/util/compatibility.h"
#include "hphp/util/configs/pageletserver.h"
#include "hphp/util/job-queue.h"
#include "hphp/util/lock.h"
#include "hphp/util/logger.h"
#include "hphp/util/service-data.h"
#include "hphp/util/timer.h"

using std::set;

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

PageletTransport::PageletTransport(
    const OptString& url, const Array& headers, const OptString& postData,
    const OptString& remoteHost, const set<std::string> &rfc1867UploadedFiles,
    const Array& files, int timeoutSeconds)
    : m_refCount(0),
      m_timeoutSeconds(timeoutSeconds),
      m_done(false),
      m_code(0),
      m_event(nullptr) {

  Timer::GetMonotonicTime(m_queueTime);
  m_threadType = ThreadType::PageletThread;

  m_url.append(url.data(), url.size());
  m_remoteHost.append(remoteHost.data(), remoteHost.size());

  for (ArrayIter iter(headers); iter; ++iter) {
    auto const key = iter.first();
    auto const header = iter.second().toString();
    if (key.isString() && !key.toString().empty()) {
      m_requestHeaders[key.toString().data()].push_back(header.data());
    } else {
      if (Cfg::PageletServer::HeaderCheck == 1) {
        raise_warning(
          "Specifying Pagelet headers using \"key: value\" syntax "
          "is deprecated: %s",
          header.data()
        );
      } else if (Cfg::PageletServer::HeaderCheck == 2) {
        SystemLib::throwInvalidArgumentExceptionObject(fmt::format(
          "Specifying Pagelet headers using \"key: value\" syntax "
          "is disabled: {}",
          header.data()
        ));
      }
      int pos = header.find(": ");
      if (pos >= 0) {
        std::string name = header.substr(0, pos).data();
        std::string value = header.substr(pos + 2).data();

        if (Cfg::PageletServer::HeaderCollide > 0 &&
            headers->exists(OptString(name))) {
          auto const msg = fmt::format(
            "Detected Pagelet header specified using both \"key: value\" "
            "and key => \"value\" syntax: {}",
            name
          );
          if (Cfg::PageletServer::HeaderCollide == 3) {
            SystemLib::throwInvalidArgumentExceptionObject(msg);
          }

          raise_warning("%s", msg.data());
          if (Cfg::PageletServer::HeaderCollide == 2) continue;
        }

        m_requestHeaders[name].push_back(value);
      } else {
        Logger::Error("throwing away bad header: %s", header.data());
      }
    }
  }

  if (postData.empty()) {
    m_get = true;
  } else {
    m_get = false;
    m_postData.append(postData.data(), postData.size());
  }

  disableCompression(); // so we don't have to decompress during sendImpl()
  m_rfc1867UploadedFiles = rfc1867UploadedFiles;
  m_files = (std::string) HHVM_FN(serialize)(files);
}

const char *PageletTransport::getUrl() {
  return m_url.c_str();
}

const char *PageletTransport::getRemoteHost() {
  return m_remoteHost.c_str();
}

uint16_t PageletTransport::getRemotePort() {
  return 0;
}

const void *PageletTransport::getPostData(size_t &size) {
  size = m_postData.size();
  return m_postData.data();
}

Transport::Method PageletTransport::getMethod() {
  return m_get ? Transport::Method::GET : Transport::Method::POST;
}

std::string PageletTransport::getHeader(const char *name) {
  assertx(name && *name);
  HeaderMap::const_iterator iter = m_requestHeaders.find(name);
  if (iter != m_requestHeaders.end()) {
    return iter->second[0];
  }
  return "";
}

const HeaderMap& PageletTransport::getHeaders() {
  return m_requestHeaders;
}

void PageletTransport::addHeaderImpl(const char *name, const char *value) {
  assertx(name && *name);
  assertx(value);
  m_responseHeaders[name].push_back(value);
}

void PageletTransport::removeHeaderImpl(const char *name) {
  assertx(name && *name);
  m_responseHeaders.erase(name);
}

void PageletTransport::sendImpl(const void* data, int size, int code,
                                bool /*chunked*/, bool eom) {
  m_response.append((const char*)data, size);
  if (code) {
    m_code = code;
  }
  if (eom) {
    onSendEndImpl();
  }
}

void PageletTransport::onSendEndImpl() {
  Lock lock(this);
  m_done = true;
  if (m_event) {
    constexpr uintptr_t kTrashedEvent = 0xfeeefeeef001f001;
    m_event->finish();
    m_event = reinterpret_cast<PageletServerTaskEvent*>(kTrashedEvent);
  }
  notify();
}

bool PageletTransport::isUploadedFile(const OptString& filename) {
  return m_rfc1867UploadedFiles.find(filename.c_str()) !=
         m_rfc1867UploadedFiles.end();
}

bool PageletTransport::getFiles(std::string &files) {
  files = m_files;
  return true;
}

bool PageletTransport::isDone() {
  return m_done;
}

void PageletTransport::addToPipeline(const std::string &s) {
  // the output buffer is already closed; nothing to do
  if (m_done) return;

  Lock lock(this);
  m_pipeline.push_back(s);
  if (m_event) {
    m_event->finish();
    m_event = nullptr;
  }
  notify();
}

bool PageletTransport::isPipelineEmpty() {
  Lock lock(this);
  return m_pipeline.empty();
}

Array PageletTransport::getAsyncResults(bool allow_empty) {
  auto results = Array::CreateVec();
  PageletServerTaskEvent* next_event = nullptr;
  int code = 0;

  {
    Lock lock(this);
    assertx(m_done || !m_pipeline.empty() || allow_empty);
    while (!m_pipeline.empty()) {
      std::string &str = m_pipeline.front();
      OptString response(str.c_str(), str.size(), CopyString);
      results.append(response);
      m_pipeline.pop_front();
    }


    if (m_done) {
      code = m_code;
      OptString response(m_response.c_str(), m_response.size(), CopyString);
      results.append(response);
    } else {
      next_event = new PageletServerTaskEvent();
      m_event = next_event;
      m_event->setJob(this);
    }
  }

  return VecInit(3)
    .append(results)
    .append(next_event
      ? make_tv<KindOfObject>(next_event->getWaitHandle())
      : make_tv<KindOfNull>())
    .append(make_tv<KindOfInt64>(code))
    .toArray();
}

OptString PageletTransport::getResults(
  Array &headers,
  int &code,
  int64_t timeout_ms
) {
  // Make sure that we only ever return a vec or null.
  if (!headers.isNull() && !headers.isVec()) {
    headers = Array::CreateVec();
  }

  {
    Lock lock(this);
    while (!m_done && m_pipeline.empty()) {
      if (timeout_ms > 0) {
        long seconds = timeout_ms / 1000;
        long long nanosecs = (timeout_ms % 1000) * 1000000;
        if (!wait(seconds, nanosecs)) {
          code = -1;
          return String::Empty();
        }
      } else {
        wait();
      }
    }

    if (!m_pipeline.empty()) {
      // intermediate results do not have headers and code
      std::string ret = m_pipeline.front();
      m_pipeline.pop_front();
      code = 0;
      return ret;
    }
  }

  OptString response(m_response.c_str(), m_response.size(), CopyString);
  headers = Array::CreateVec();
  for (HeaderMap::const_iterator iter = m_responseHeaders.begin();
       iter != m_responseHeaders.end(); ++iter) {
    for (unsigned int i = 0; i < iter->second.size(); i++) {
      StringBuffer sb;
      sb.append(iter->first);
      sb.append(": ");
      sb.append(iter->second[i]);
      headers.append(sb.detach());
    }
  }
  code = m_code;
  return response;
}

// ref counting
void PageletTransport::incRefCount() {
  ++m_refCount;
}

void PageletTransport::decRefCount() {
  assertx(m_refCount.load() > 0);
  if (--m_refCount == 0) {
    delete this;
  }
}

const timespec& PageletTransport::getStartTimer() const {
  return m_queueTime;
}

int PageletTransport::getTimeoutSeconds() const {
  return m_timeoutSeconds;
}

void PageletTransport::setAsioEvent(PageletServerTaskEvent *event) {
  m_event = event;
}

///////////////////////////////////////////////////////////////////////////////

static int64_t to_ms(const timespec& ts) {
  return ts.tv_sec * 1000 + (ts.tv_nsec / 1000000);
}

struct PageletWorker
  : JobQueueWorker<PageletTransport*,Server*,true,false,JobQueueDropVMStack>
{
  void doJob(PageletTransport *job) override {
    static auto pageletTimeInQueueCounter =
      ServiceData::createTimeSeries("pagelet_time_in_queue",
        { ServiceData::StatsType::AVG, ServiceData::StatsType::SUM });
    try {
      int64_t job_created_time = to_ms(job->getStartTimer());
      job->onRequestStart(job->getStartTimer());
      int timeout = job->getTimeoutSeconds();
      if (timeout > 0) {
        timespec ts;
        Timer::GetMonotonicTime(ts);
        int64_t delta_ms =
          job_created_time + timeout * 1000 - to_ms(ts);
        if (delta_ms > 500) {
          timeout = (delta_ms + 500) / 1000;
        } else {
          timeout = 1;
        }
      } else {
        timeout = 0;
      }
      timespec handler_start_time_ts;
      Timer::GetMonotonicTime(handler_start_time_ts);
      auto time_spent_in_queue = to_ms(handler_start_time_ts) - job_created_time;
      pageletTimeInQueueCounter->addValue(time_spent_in_queue);
      HttpRequestHandler(timeout).run(job);
      job->decRefCount();
    } catch (...) {
      Logger::Error("HttpRequestHandler leaked exceptions");
    }
  }
};

///////////////////////////////////////////////////////////////////////////////

struct PageletTask : SweepableResourceData {
  DECLARE_RESOURCE_ALLOCATION(PageletTask)

  PageletTask(const OptString& url, const Array& headers, const OptString& post_data,
              const OptString& remote_host,
              const std::set<std::string> &rfc1867UploadedFiles,
              const Array& files, int timeoutSeconds) {
    m_job = new PageletTransport(url, headers, remote_host, post_data,
                                 rfc1867UploadedFiles, files, timeoutSeconds);
    m_job->incRefCount();
  }

  ~PageletTask() override {
    m_job->decRefCount();
  }

  PageletTransport *getJob() { return m_job;}

  CLASSNAME_IS("PageletTask")
  // overriding ResourceData
  const OptString& o_getClassNameHook() const override { return classnameof(); }

private:
  PageletTransport *m_job;
};
IMPLEMENT_RESOURCE_ALLOCATION(PageletTask)

///////////////////////////////////////////////////////////////////////////////
// implementing PageletServer

static JobQueueDispatcher<PageletWorker> *s_dispatcher;
static ReadWriteMutex s_dispatchMutex;
static ServiceData::CounterCallback* s_counters;

bool PageletServer::Enabled() {
  return s_dispatcher;
}

void PageletServer::Restart() {
  Stop();
  if (Cfg::PageletServer::ThreadCount > 0) {
    {
      WriteLock l(s_dispatchMutex);
      s_dispatcher = new JobQueueDispatcher<PageletWorker>
        (Cfg::PageletServer::ThreadCount,
         Cfg::PageletServer::ThreadCount,
         Cfg::PageletServer::ThreadDropCacheTimeoutSeconds,
         Cfg::PageletServer::ThreadDropStack,
         nullptr);
      s_dispatcher->setHugePageConfig(
        Cfg::PageletServer::HugeThreadCount,
        Cfg::Server::HugeStackSizeKb);
      auto monitor = getSingleton<HostHealthMonitor>();
      monitor->subscribe(s_dispatcher);
    }
    s_dispatcher->start();
    BootStats::mark("pagelet server started");
    s_counters = new ServiceData::CounterCallback(
        [](ServiceData::CounterMap& counters) {
          counters["pagelet_inflight_requests"] =
            PageletServer::GetActiveWorker();
          counters["pagelet_queued_requests"] =
            PageletServer::GetQueuedJobs();
        },
        "pagelet_"
    );
  }
}

void PageletServer::Stop() {
  if (s_dispatcher) {
    auto monitor = getSingleton<HostHealthMonitor>();
    monitor->unsubscribe(s_dispatcher);
    s_dispatcher->stop();
    delete s_counters;
    s_counters = nullptr;
    WriteLock l(s_dispatchMutex);
    delete s_dispatcher;
    s_dispatcher = nullptr;
  }
}

OptResource PageletServer::TaskStart(
  const OptString& url, const Array& headers,
  const OptString& remote_host,
  const OptString& post_data /* = null_string */,
  const Array& files /* = null_array */,
  int timeoutSeconds /* = -1 */,
  PageletServerTaskEvent *event /* = nullptr*/
) {
  static auto pageletOverflowCounter =
    ServiceData::createTimeSeries("pagelet_overflow",
                                  { ServiceData::StatsType::COUNT });
  static auto pageletQueuedCounter =
    ServiceData::createTimeSeries("pagelet_queued",
                                  { ServiceData::StatsType::COUNT });
  {
    ReadLock l(s_dispatchMutex);
    if (!s_dispatcher) {
      return OptResource();
    }
    if (Cfg::PageletServer::QueueLimit > 0) {
      auto num_queued_jobs = s_dispatcher->getQueuedJobs();
      if (num_queued_jobs > Cfg::PageletServer::QueueLimit) {
        pageletOverflowCounter->addValue(1);
        return OptResource();
      }
      if (num_queued_jobs > 0) {
        pageletQueuedCounter->addValue(1);
      }
    }
  }
  auto task = req::make<PageletTask>(url, headers, remote_host, post_data,
                                        get_uploaded_files(), files,
                                        timeoutSeconds);
  PageletTransport *job = task->getJob();
  WriteLock l(s_dispatchMutex);
  if (s_dispatcher) {
    job->incRefCount(); // paired with worker's decRefCount()

    if (event) {
      job->setAsioEvent(event);
      event->setJob(job);
    }

    s_dispatcher->enqueue(job);
    g_context->incrPageletTasksStarted();
    return OptResource(std::move(task));
  }
  return OptResource();
}

int64_t PageletServer::TaskStatus(const OptResource& task) {
  PageletTransport *job = cast<PageletTask>(task)->getJob();
  if (!job->isPipelineEmpty()) {
    return PAGELET_READY;
  }
  if (job->isDone()) {
    return PAGELET_DONE;
  }
  return PAGELET_NOT_READY;
}

OptString PageletServer::TaskResult(const OptResource& task, Array &headers, int &code,
                                    int64_t timeout_ms) {
  auto ptask = cast<PageletTask>(task);
  return ptask->getJob()->getResults(headers, code, timeout_ms);
}

Array PageletServer::AsyncTaskResult(const OptResource& task) {
  auto ptask = cast<PageletTask>(task);
  return ptask->getJob()->getAsyncResults(true);
}

void PageletServer::AddToPipeline(const std::string &s) {
  assertx(!s.empty());
  PageletTransport *job =
    dynamic_cast<PageletTransport *>(g_context->getTransport());
  assertx(job);
  job->addToPipeline(s);
}

int PageletServer::GetActiveWorker() {
  return s_dispatcher ? s_dispatcher->getActiveWorker() : 0;
}

size_t PageletServer::GetLiveThreadCount() {
  ReadLock l(s_dispatchMutex);
  return s_dispatcher ? s_dispatcher->getLiveThreadCount() : 0;
}

int PageletServer::GetQueuedJobs() {
  return s_dispatcher ? s_dispatcher->getQueuedJobs() : 0;
}

///////////////////////////////////////////////////////////////////////////////
}
