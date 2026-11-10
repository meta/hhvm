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

#include <deque>
#include <atomic>
#include <chrono>
#include <memory>
#include <set>
#include <vector>

#include <boost/range/adaptors.hpp>
#include <folly/Memory.h>
#include <folly/small_vector.h>

#include "hphp/util/alloc.h"
#include "hphp/util/assertions.h"
#include "hphp/util/async-func.h"
#include "hphp/util/atomic.h"
#include "hphp/util/compatibility.h"
#include "hphp/util/exception.h"
#include "hphp/util/health-monitor-types.h"
#include "hphp/util/lock.h"
#include "hphp/util/logger.h"
#include "hphp/util/numa.h"
#include "hphp/util/synchronizable-multi.h"

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

/**
 * A queue-based multi-threaded job processing facility. Internally, we have a
 * queue of jobs and a list of workers, each of which runs in its own thread.
 * Job queue can take new jobs on the fly and workers will continue to pull
 * jobs off the queue and work on it.
 *
 * To use it, simply define your own job and worker class like this,
 *
 *   struct MyJob {
 *     // storing job data
 *   };
 *
 *   struct MyWorker : JobQueueWorker<MyJob*> {
 *     virtual void doJob(MyJob *job) {
 *       // process the job
 *       delete job; // if it was new-ed
 *     }
 *   };
 *
 * Now, use JobQueueDispatcher to start the whole thing,
 *
 *   JobQueueDispatcher<MyJob*, MyWorker> dispatcher(40, NULL); // 40 threads
 *   dispatcher.start();
 *   ...
 *   dispatcher.enqueue(new MyJob(...));
 *   ...
 *   dispatcher.stop();
 *
 * Note that a job queue is normally empty initially and new jobs are pushed
 * into the queue over time. Workers can be stopped individually.
 *
 * Job process ordering
 * ====================
 * By default, requests are processed in FIFO order.
 *
 * In addition, we support an option where the request processing order can flip
 * between FIFO or LIFO based on the length of the queue. This can be enabled by
 * setting the 'lifoSwitchThreshold' parameter. If the job queue is configured
 * to be in FIFO mode, and the current queue length exceeds lifoSwitchThreshold,
 * then the workers will begin work on requests in LIFO order until the queue
 * size is below the threshold in which case we resume in FIFO order. Setting
 * the queue to be in LIFO mode initially will have the opposite behavior. This
 * is useful when we are in a loaded situation and we want to prioritize the
 * newest requests.
 *
 * You can configure a LIFO ordered queue by setting lifoSwitchThreshold to 0.
 */

///////////////////////////////////////////////////////////////////////////////

namespace detail {
struct NoDropCachePolicy { static void dropCache() {} };
}

struct DispatcherStats {
  // Maximum number of threads that can be used by the dispatcher
  size_t maxThreads = 0;
  // Number of workers that are actively processing jobs
  size_t activeThreads = 0;
  // Number of jobs that are currently in the queue
  size_t queuedJobCount = 0;
  // Instantiated pool workers, including idle workers.
  size_t liveThreads = 0;
};

/**
 * A job queue that's suitable for multiple threads to work on.
 */
template<typename TJob,
         bool waitable = false,
         class DropCachePolicy = detail::NoDropCachePolicy>
struct JobQueue : SynchronizableMulti {
  // trivial class for signaling queue stop
  struct StopSignal {};

public:
  /**
   * Constructor.
   */
  JobQueue(int maxQueueCount, std::chrono::nanoseconds dropCacheTimeout,
           bool dropStack, int lifoSwitchThreshold,
           std::chrono::nanoseconds maxJobQueuing, int numPriorities)
      : SynchronizableMulti(maxQueueCount + 1) // reaper added
      , m_dropCacheTimeout(dropCacheTimeout)
      , m_dropStack(dropStack)
      , m_lifoSwitchThreshold(lifoSwitchThreshold)
      , m_maxJobQueuing(maxJobQueuing)
      , m_jobReaperId(maxQueueCount) {
    assertx(maxQueueCount > 0);
    m_jobQueues.resize(numPriorities);
  }

  /**
   * Put a job into the queue and notify a worker to pick it up if requested.
   */
  void enqueue(TJob job, int priority = 0, bool eagerNotify = true) {
    assertx(priority >= 0);
    auto const enqueueTime = std::chrono::steady_clock::now();
    Lock lock(this);
    m_jobQueues[priority].emplace_back(job, enqueueTime);
    ++m_jobCount;
    if (eagerNotify) {
      notify();
    }
  }

  /**
   * Grab a job from the queue for processing. Since the job was not created
   * by this queue class, it's up to a worker class on whether to deallocate
   * the job object correctly.
   */
  TJob dequeueMaybeExpired(int id, int q, bool inc, bool* expired,
                           bool highpri = false, bool* workerStop = nullptr) {
    if (id == m_jobReaperId) {
      *expired = true;
      return dequeueOnlyExpiredImpl(id, q, inc);
    }
    return dequeueMaybeExpiredImpl(id, q, inc, expired,
                                   highpri, workerStop);
  }

  /**
   * Purely for making sure no new jobs are queued when we are stopping.
   */
  void stop() {
    Lock lock(this);
    m_stopped = true;
    notifyAll(); // so all waiting threads can find out queue is stopped
  }

  void waitEmpty() {}
  void signalEmpty() {}

  /**
   * Keeps track of how many active workers are working on the queue.
   */
  void incActiveWorker() {
    ++m_workerCount;
  }
  int decActiveWorker() {
    return --m_workerCount;
  }
  int getActiveWorker() {
    return m_workerCount;
  }

  /**
   * Keep track of how many jobs are queued, but not yet been serviced.
   */
  int getQueuedJobs() {
    return m_jobCount;
  }

  void updateMaxActiveWorkers(int num) {
    m_maxActiveWorkers.exchange(num, std::memory_order_acq_rel);
  }

  int releaseQueuedJobs(int target = 0) {
    if (m_jobCount) {
      Lock lock(this);
      const int active = getActiveWorker();
      auto const toRelease =
        std::max(1, std::min(target - active, m_jobCount));
      for (int i = 0; i < toRelease; ++i) {
        notify();
      }
      return toRelease;
    }
    return 0;
  }

 private:
  friend class JobQueue_Expiration_Test;

  TJob dequeueMaybeExpiredImpl(int id, int q, bool inc,
                               bool* expired, bool highPri = false,
                               bool* workerStop = nullptr) {
    *expired = false;
    Lock lock(this);
    bool flushed = false;

    bool ableToDequeue = getActiveWorker() <
                         m_maxActiveWorkers.load(std::memory_order_acquire);
    while (m_jobCount == 0 || !ableToDequeue) {
      uint32_t kNumPriority = m_jobQueues.size();
      if (m_jobQueues[kNumPriority - 1].size() > 0) {
        break;
      }

      // Stop trying to dequeue, if the queue itself has stopped, or if the
      // worker has been asked to stop due to thread count adjustment.
      if (m_stopped || (workerStop && *workerStop)) {
        throw StopSignal();
      }

      if (flushed) {
        // Flushed worker threads gets lower priority.  But a flushed worker
        // with huge stack is still more preferable than a non-flushed worker
        // without huge stack.
        wait(id, q, highPri ? Priority::High : Priority::Low);
      } else if (m_dropCacheTimeout > std::chrono::nanoseconds::zero()) {
        // When we can't dequeue due to the max active worker limit, we flush
        // the thread immediately to reduce resource pressure.
        if (!ableToDequeue ||
            !wait(id, q, (highPri ? Priority::Highest : Priority::Normal),
                  std::chrono::steady_clock::now() + m_dropCacheTimeout)) {
          // since we timed out, maybe we can turn idle without holding memory
          if (m_jobCount == 0 || !ableToDequeue) {
            ScopedUnlock unlock(this);
            flush_thread_caches();
            if (m_dropStack && s_stackLimit) {
              flush_thread_stack();
            }
            DropCachePolicy::dropCache();
            flushed = true;
          }
        }
      } else {
        // m_dropCacheTimeout <= 0s, a thread that starts waiting more recently
        // should be given a task first (LIFO), same as unflushed threads.
        wait(id, q, highPri ? Priority::Highest : Priority::Normal);
      }
      ableToDequeue = getActiveWorker() <
                      m_maxActiveWorkers.load(std::memory_order_acquire);
    }
    // Stop immediately if the worker has been asked to stop, due to thread
    // count adjustments.
    if (workerStop && *workerStop) {
      throw StopSignal();
    }
    if (inc) incActiveWorker();
    --m_jobCount;

    // look across all our queues from highest priority to lowest.
    auto const now = std::chrono::steady_clock::now();
    for (auto& jobs : boost::adaptors::reverse(m_jobQueues)) {
      if (jobs.empty()) {
        continue;
      }

      // peek at the beginning of the queue to see if the request has already
      // timed out.
      if (m_maxJobQueuing > std::chrono::nanoseconds::zero() &&
          now - jobs.front().second > m_maxJobQueuing) {
        *expired = true;
        TJob job = jobs.front().first;
        jobs.pop_front();
        return job;
      }

      if (m_jobCount >= m_lifoSwitchThreshold) {
        TJob job = jobs.back().first;
        jobs.pop_back();
        return job;
      }
      TJob job = jobs.front().first;
      jobs.pop_front();
      return job;
    }
    not_reached();
  }

  /*
   * One worker can be designated as the job reaper. The id of the job reaper
   * equals maxQueueCount of the dispatcher. The job reaper checks if the
   * oldest job on the queue has expired and if so, terminate that job without
   * processing it.  When the job reaper calls dequeueMaybeExpired(), it goes to
   * dequeueOnlyExpiredImpl(), which only returns the oldest job and only if
   * it's expired. Otherwise dequeueMaybeExpired() will block until a job
   * expires.
   */
  TJob dequeueOnlyExpiredImpl(int id, int q, bool inc) {
    assertx(id == m_jobReaperId);
    assertx(m_maxJobQueuing > std::chrono::nanoseconds::zero());
    Lock lock(this);
    while (!m_stopped) {
      auto const now = std::chrono::steady_clock::now();
      auto deadline = now + m_maxJobQueuing;

      for (auto& jobs : boost::adaptors::reverse(m_jobQueues)) {
        if (!jobs.empty()) {
          auto const expiry = jobs.front().second + m_maxJobQueuing;
          if (now >= expiry) {
            if (inc) incActiveWorker();
            --m_jobCount;

            TJob job = jobs.front().first;
            jobs.pop_front();
            return job;
          }
          // oldest job hasn't expired yet. wake us up when it will.
          deadline = std::min(deadline, expiry);
        }
      }
      if (wait(id, q, Priority::Low, deadline)) {
        // We got woken up by somebody calling notify (as opposed to timeout),
        // then some work might be on the queue. We only expire things here,
        // so let's notify somebody else as well.
        notify();
      }
    }
    throw StopSignal();
  }

  int m_jobCount{0};
  folly::small_vector<
    std::deque<std::pair<TJob, std::chrono::steady_clock::time_point>>,
    2
  > m_jobQueues;
  bool m_stopped{false};
  std::atomic<int> m_workerCount{0};
  std::atomic<int> m_maxActiveWorkers{INT_MAX};
  const std::chrono::nanoseconds m_dropCacheTimeout;
  const bool m_dropStack;
  const int m_lifoSwitchThreshold;
  const std::chrono::nanoseconds m_maxJobQueuing;
  const int m_jobReaperId;              // equals max worker thread count
};

template<class TJob, class Policy>
struct JobQueue<TJob,true,Policy> : JobQueue<TJob,false,Policy> {
  JobQueue(int threadCount, std::chrono::nanoseconds dropCacheTimeout,
           bool dropStack, int lifoSwitchThreshold,
           std::chrono::nanoseconds maxJobQueuing, int numPriorities) :
    JobQueue<TJob,false,Policy>(threadCount,
                                dropCacheTimeout,
                                dropStack,
                                lifoSwitchThreshold,
                                maxJobQueuing,
                                numPriorities) {}
  void waitEmpty() {
    Lock lock(this);
    while (this->getActiveWorker() || this->getQueuedJobs()) {
      m_cond.wait(this->getMutex());
    }
  }
  bool pollEmpty() {
    Lock lock(this);
    return !(this->getActiveWorker() || this->getQueuedJobs());
  }
  void signalEmpty() {
    m_cond.notify_one();
  }
private:
  std::condition_variable_any m_cond;
};

///////////////////////////////////////////////////////////////////////////////

/**
 * Base class for a customized worker.
 *
 * DropCachePolicy is an extra callback for specific actions to take
 * when we decide to drop stack/caches.
 */
template<typename TJob,
         typename TContext = void*,
         bool countActive = false,
         bool waitable = false,
         class Policy = detail::NoDropCachePolicy>
struct JobQueueWorker {
  typedef TJob JobType;
  typedef TContext ContextType;
  typedef JobQueue<TJob, waitable, Policy> QueueType;
  typedef Policy DropCachePolicy;

  static const bool Waitable = waitable;
  static const bool CountActive = countActive;

  virtual ~JobQueueWorker() {}

  /**
   * Two-phase object creation for easier derivation and for JobQueueDispatcher
   * to easily create a vector of workers.
   */
  void create(int id, QueueType* queue, void* func, ContextType context) {
    assertx(queue);
    m_id = id;
    m_queue = queue;
    m_func = func;
    m_context = context;
  }

  /**
   * The only functions a subclass needs to implement.
   */
  virtual void doJob(TJob job) = 0;
  virtual void abortJob(TJob /*job*/) {
    Logger::Warning("Job dropped by JobQueueDispatcher because of timeout.");
  }
  virtual void onThreadEnter() {}
  virtual void onThreadExit() {}

  /**
   * Start this worker thread.
   */
  void start() {
    assertx(m_queue);
    onThreadEnter();
    bool highPri = (s_hugeRange.ptr != nullptr);
    while (!m_stopped) {
      try {
        bool expired = false;
        TJob job = m_queue->dequeueMaybeExpired(m_id, s_numaNode, countActive,
                                                &expired, highPri, &m_stopped);
        if (expired) {
          abortJob(job);
        } else {
          doJob(job);
        }
        if (countActive) {
          if (!m_queue->decActiveWorker() && waitable) {
            Lock lock(m_queue);
            if (!m_queue->getActiveWorker() &&
                !m_queue->getQueuedJobs()) {
              m_queue->signalEmpty();
            }
          }
        }
      } catch (const typename QueueType::StopSignal&) {
        // Either queue is empty and stopped, or we've been asked to stop.
        m_stopped = true;
      }
    }
    onThreadExit();
  }

  /**
   * Stop this worker thread.
   */
  void stop() {
    m_stopped = true;
  }

  int id() { return m_id; }
  void* func() { return m_func; }
  bool stopped() { return m_stopped; }

protected:
  int m_id{-1};
  void* m_func{nullptr};
  ContextType m_context{};
  bool m_stopped{false};

private:
  QueueType* m_queue{nullptr};
};

///////////////////////////////////////////////////////////////////////////////

/**
 * Driver class to push through the whole thing.
 */
template<class TWorker>
struct JobQueueDispatcher : IHostHealthObserver {
  /**
   * Constructor.
   */
  JobQueueDispatcher(int maxThreadCount, int maxQueueCountConfig,
                     int dropCacheTimeoutSec, bool dropStack,
                     typename TWorker::ContextType context,
                     int lifoSwitchThreshold = INT_MAX,
                     int maxJobQueuingMs = 0, int numPriorities = 1,
                     int hugeCount = 0,
                     int initThreadCount = -1,
                     unsigned hugeStackKb = 0,
                     unsigned extraKb = 0,
                     const std::string& threadGroupSuffix = "")
      : m_startReaperThread(maxJobQueuingMs > 0)
      , m_context(context)
      , m_maxThreadCount(maxThreadCount)
      , m_maxQueueCount(std::max(maxQueueCountConfig, maxThreadCount))
      , m_currThreadCountLimit(initThreadCount)
      , m_hugeThreadCount(hugeCount)
      , m_hugeStackKb(hugeStackKb)
      , m_tlExtraKb(extraKb)
      , m_threadGroupSuffix(threadGroupSuffix)
      , m_queue(m_maxQueueCount, std::chrono::seconds(dropCacheTimeoutSec),
                dropStack, lifoSwitchThreshold,
                std::chrono::milliseconds(maxJobQueuingMs), numPriorities) {
    assertx(maxThreadCount >= 1);
    assertx(m_maxQueueCount >= maxThreadCount);
    if (maxQueueCountConfig < maxThreadCount) {
      Logger::Warning(
        "JobQueueDispatcher: QueueCount %d < ThreadCount %d, using %d\n",
        maxQueueCountConfig,
        maxThreadCount,
        maxThreadCount);
    }
    if (initThreadCount < 0 || initThreadCount > maxThreadCount) {
      m_currThreadCountLimit = maxThreadCount;
    }
    if (!TWorker::CountActive) {
      // If TWorker does not support counting the number of
      // active workers, just start all of the workers eagerly
      for (int i = 0; i < maxThreadCount; i++) {
        addWorkerImpl(false);
      }
    }
  }

  ~JobQueueDispatcher() override {
    stop();
    for (auto func : m_funcs) delete func;
    for (auto worker : m_stoppedWorkers) delete worker;
    for (auto worker : m_workers) delete worker;
  }

  size_t getMaxThreadCount() const {
    return m_maxThreadCount.load(std::memory_order_acquire);
  }

  size_t getLiveThreadCount() const {
    return m_threadCount.load(std::memory_order_relaxed);
  }

  int getActiveWorker() {
    return m_queue.getActiveWorker();
  }

  int getQueuedJobs() {
    return m_queue.getQueuedJobs();
  }

  int getTargetNumWorkers() {
    if (TWorker::CountActive) {
      int target = getActiveWorker() +  getQueuedJobs();
      if (target > m_currThreadCountLimit) return m_currThreadCountLimit;
      return target;
    } else {
      return m_currThreadCountLimit;
    }
  }

  /**
   * Creates worker threads and start running them. This is non-blocking.
   */
  void start() {
    Lock lock(m_mutex);
    m_queue.setNumGroups(num_numa_nodes());
    // Spin up more worker threads if appropriate
    int target = getTargetNumWorkers();
    for (int n = numActiveThreads(); n < target; ++n) {
      addWorkerImpl(false);
    }
    for (auto worker : m_funcs) {
      worker->start();
      ++m_threadCount;
    }
    m_stopped = false;

    if (m_startReaperThread) {
      addReaper();
    }
  }

  /**
   * Enqueue a new job.
   */
  void enqueue(typename TWorker::JobType job, int priority = 0) {
    auto const level = getHealthLevel();
    auto const eagerNotify =
      (level < HealthLevel::Cautious) ||
      ((level == HealthLevel::Cautious) &&
        (m_queue.getActiveWorker() * 2 <= m_currThreadCountLimit));
    m_queue.enqueue(job, priority, eagerNotify);

    // Spin up another worker thread if appropriate.
    auto const target = getTargetNumWorkers();
    size_t actives;
    {
      Lock lock(m_mutex);
      if (m_stopped) return;
      actives = numActiveThreads();
    }
    if (actives < target) addWorker();
  }

  /**
   * Add a worker thread on the fly.
   */
  void addWorker() {
    Lock lock(m_mutex);
    if (!m_stopped) {
      addWorkerImpl(true);
    }
  }

  /*
   * Increase the limit on the number of workers to the maximum.
   */
  void saturateWorkers() {
    Lock lock(m_mutex);
    if (m_stopped) return;

    auto const mtc = m_maxThreadCount.load(std::memory_order_acquire);
    auto const n = mtc - m_currThreadCountLimit;
    m_currThreadCountLimit = mtc;

    if (!TWorker::CountActive) {
      for (int i = 0; i < n; ++i) {
        addWorkerImpl(true);
      }
    } else {
      while (numActiveThreads() < getTargetNumWorkers()) {
        addWorkerImpl(true);
      }
    }
  }

  std::vector<TWorker*> getWorkers() {
    Lock lock(m_mutex);
    std::vector<TWorker*> ret{m_workers};
    return ret;
  }

  void waitEmpty(bool stop = true) {
    if (m_stopped) return;
    m_queue.waitEmpty();
    if (stop) this->stop();
  }

  bool pollEmpty() {
    if (m_stopped) return true;
    return m_queue.pollEmpty();
  }

  /**
   * Stop all workers after all jobs are processed. No new jobs should be
   * enqueued at this moment, or this call may block for longer time.
   */
  void stop() {
    // TODO(t5572120): If stop has already been called when the destructor
    // runs, we'd bail out here and potentially start destroying AsyncFuncs
    // that are still running.
    {
      Lock lock(m_mutex);
      if (m_stopped) return;
      m_stopped = true;
    }

    m_queue.stop();
    bool exceptioned = false;
    Exception exception;

    while (true) {
      AsyncFunc<TWorker> *func = nullptr;
      {
        Lock lock(m_mutex);
        if (!m_funcs.empty()) {
          func = *m_funcs.begin();
        } else if (m_reaperFunc) {
          func = m_reaperFunc.release();
        }
      }
      if (func == nullptr) {
        break;
      }
      try {
        func->waitForEnd();
      } catch (Exception& e) {
        exceptioned = true;
        exception = e;
      }
      {
        Lock lock(m_mutex);
        if (m_funcs.erase(func)) --m_threadCount;
      }
      delete func;
    }
    if (exceptioned) {
      throw exception;
    }
  }

  void run() {
    start();
    stop();
  }

  void notifyNewStatus(HealthLevel newStatus) override {
    if (m_healthStatus >= HealthLevel::NoMore &&
        newStatus < HealthLevel::NoMore) {
      m_healthStatus = newStatus;
      // release blocked requests in queue if any
      m_queue.updateMaxActiveWorkers(INT_MAX);
      m_queue.releaseQueuedJobs(m_currThreadCountLimit / 4);
    } else if (newStatus >= HealthLevel::NoMore &&
                m_healthStatus < HealthLevel::NoMore) {
      m_healthStatus = newStatus;
      m_queue.updateMaxActiveWorkers(0);
    } else {
      m_healthStatus = newStatus;
    }
  }

  HealthLevel getHealthLevel() override {
    return m_healthStatus;
  }

  void setHugePageConfig(int count, unsigned stackKb, unsigned rangeKb = 0) {
    m_hugeThreadCount = count;
    m_hugeStackKb = stackKb;
  }

  void updateMaxActiveWorkers(int num) {
    m_queue.updateMaxActiveWorkers(num);
  }

  /*
   * Change the maximum thread count.
   *
   * If we have more workers than the new maximum, we stop worker threads until
   * we drop below the threshold.  While those threads finish execution, we
   * park their workers in the m_stoppedWorkers set, for later destruction.
   */
  void setMaxThreadCount(int mtc) {
    if (mtc > m_maxQueueCount) mtc = m_maxQueueCount;

    Lock lock(m_mutex);

    // The purpose of m_currThreadCountLimit is to enable us to start with a
    // lower thread limit than the true maximum, and eventually bump up to that
    // maximum via saturateWorkers().  Here, we assume that if the two are
    // equal, saturation was intended and we should maintain it; otherwise, we
    // should not.
    //
    // This assumption is only incorrect if the initial thread count and the
    // maximum thread count happened to be the same, under conditions where we
    // don't want saturation (except by chance).  We rely on the client to
    // avoid this situation.
    m_currThreadCountLimit =
      m_maxThreadCount.load(std::memory_order_acquire) == m_currThreadCountLimit
        ? mtc
        : std::min(mtc, m_currThreadCountLimit);

    m_maxThreadCount.store(mtc, std::memory_order_release);

    // If we have fewer active threads than the new maximum, we're done.
    if (m_workers.size() <= mtc) return;

    // We have too many active threads; we need to stop the excess.
    for (auto it = m_workers.begin() + mtc; it != m_workers.end(); ++it) {
      auto worker = *it;
      worker->stop();
      m_queue.notifySpecific(worker->id()); // wake it up, if it is waiting.
    }
    m_stoppedWorkers.insert(m_workers.begin() + mtc, m_workers.end());
    m_workers.resize(mtc);
  }

  DispatcherStats getDispatcherStats() {
    DispatcherStats stats;

    stats.maxThreads = getMaxThreadCount();
    stats.activeThreads = getActiveWorker();
    stats.queuedJobCount = getQueuedJobs();
    stats.liveThreads = getLiveThreadCount();

    return stats;
  }

  /////////////////////////////////////////////////////////////////////////////

private:
  bool m_stopped{true};
  const bool m_startReaperThread;
  HealthLevel m_healthStatus{HealthLevel::Bold};
  typename TWorker::ContextType m_context;
  std::atomic<int> m_maxThreadCount;
  std::atomic<size_t> m_threadCount{0};
  const int m_maxQueueCount;    // not including the possible reaper
  int m_currThreadCountLimit;   // initial limit can be lower than max
  std::atomic<uint32_t> m_prevNode{0};   // the NUMA node for last worker
  int m_hugeThreadCount{0};
  unsigned m_hugeStackKb;
  unsigned m_tlExtraKb;
  std::string m_threadGroupSuffix;
  JobQueue<typename TWorker::JobType,
           TWorker::Waitable,
           typename TWorker::DropCachePolicy> m_queue;

  Mutex m_mutex;
  std::vector<TWorker*> m_workers;
  std::unordered_set<TWorker*> m_stoppedWorkers;
  std::set<AsyncFunc<TWorker>*> m_funcs;
  std::unique_ptr<TWorker> m_reaper;
  std::unique_ptr<AsyncFunc<TWorker>> m_reaperFunc;

  /*
   * Helper to extract the AsyncFunc stored in a TWorker.
   */
  static AsyncFunc<TWorker>* funcFrom(TWorker* worker) {
    return reinterpret_cast<AsyncFunc<TWorker>*>(worker->func());
  }

  /*
   * Total number of workers that might be active.
   *
   * This includes workers that are in the process of being stopped because we
   * shrunk the max thread count.
   *
   * This function cannot be called concurrently.
   */
  size_t numActiveThreads() {
    // Delete any stopped workers and funcs that have completely finished since
    // the last time we shrunk m_maxThreadCount.  The steady state is that this
    // set will be empty, so this should be cheap when amortized across a
    // lengthy run.
    for (auto it = m_stoppedWorkers.begin();
         it != m_stoppedWorkers.end(); ) {
      auto const worker = *it;
      auto const func = funcFrom(worker);

      if (func->waitForEnd(-1)) {
        it = m_stoppedWorkers.erase(it);
        m_funcs.erase(func);
        if (!m_stopped) --m_threadCount;
        delete func;
        delete worker;
      } else {
        ++it;
      }
    }
    return m_workers.size() + m_stoppedWorkers.size();
  }

  int addReaper() {
    m_reaper = std::make_unique<TWorker>();
    m_reaperFunc = std::make_unique<AsyncFunc<TWorker>>(m_reaper.get(),
                                                        &TWorker::start);
    m_reaper->create(m_maxThreadCount, &m_queue, m_reaperFunc.get(), m_context);
    m_reaperFunc->start();
    return m_maxThreadCount;
  }

  // Cannot be called concurrently (callers should hold m_mutex, or
  // otherwise ensure that no other threads are calling this).
  void addWorkerImpl(bool start) {
    if (numActiveThreads() >= m_maxThreadCount) {
      // another thread raced with us to add a worker.
      return;
    }
    auto worker = new TWorker();
    unsigned hugeStackKb =
      m_workers.size() < m_hugeThreadCount ? m_hugeStackKb : 0;
    auto func = new AsyncFunc<TWorker>(worker,
                                       &TWorker::start,
                                       next_numa_node(m_prevNode),
                                       hugeStackKb,
                                       m_tlExtraKb,
                                       m_threadGroupSuffix);
    int id = m_workers.size();
    m_workers.push_back(worker);
    m_funcs.insert(func);
    worker->create(id, &m_queue, func, m_context);

    if (start) {
      func->start();
      ++m_threadCount;
    }
  }

};

///////////////////////////////////////////////////////////////////////////////
}
