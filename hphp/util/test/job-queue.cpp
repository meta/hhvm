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
#include "hphp/util/job-queue.h"

#include <chrono>
#include <functional>
#include <future>
#include <thread>
#include <gtest/gtest.h>

namespace HPHP {

namespace {
template<bool CountActive = true>
struct CountWorker
  : JobQueueWorker<std::function<void()>, std::function<void()>*, CountActive, true> {
  void doJob(std::function<void()> job) override { job(); }
  void onThreadExit() override { if (this->m_context) (*this->m_context)(); }
};
}

TEST(JobQueueDispatcher, InstantiatedWorkers) {
  JobQueueDispatcher<CountWorker<>> dispatcher(128, 50000, 0, false, nullptr);
  std::promise<void> started;
  std::promise<void> release;
  auto ready = release.get_future();
  dispatcher.enqueue([&] { started.set_value(); ready.wait(); });
  EXPECT_EQ(0, dispatcher.getLiveThreadCount());
  dispatcher.start();
  started.get_future().wait();
  EXPECT_EQ(1, dispatcher.getLiveThreadCount());
  dispatcher.setMaxThreadCount(50000);
  EXPECT_EQ(1, dispatcher.getLiveThreadCount());
  release.set_value();
  dispatcher.waitEmpty(false);
  EXPECT_EQ(0, dispatcher.getActiveWorker());
  EXPECT_EQ(1, dispatcher.getLiveThreadCount());
  dispatcher.stop();
  EXPECT_EQ(0, dispatcher.getLiveThreadCount());
}

TEST(JobQueueDispatcher, CountRetiredWorkerDuringStop) {
  std::promise<void> exiting;
  std::promise<void> release;
  auto ready = release.get_future();
  std::function<void()> onExit = [&] { exiting.set_value(); ready.wait(); };
  JobQueueDispatcher<CountWorker<>> dispatcher(1, 1, 0, false, &onExit);
  dispatcher.enqueue([] {});
  dispatcher.start();
  dispatcher.waitEmpty(false);
  dispatcher.setMaxThreadCount(0);
  std::thread stopper([&] { dispatcher.stop(); });
  exiting.get_future().wait();
  EXPECT_EQ(1, dispatcher.getLiveThreadCount());
  release.set_value();
  stopper.join();
  EXPECT_EQ(0, dispatcher.getLiveThreadCount());
}

TEST(JobQueueDispatcher, CountBeforeAndAfterResize) {
  JobQueueDispatcher<CountWorker<false>> dispatcher(3, 3, 0, false, nullptr);
  EXPECT_EQ(0, dispatcher.getLiveThreadCount());
  dispatcher.setMaxThreadCount(2);
  dispatcher.start();
  EXPECT_EQ(2, dispatcher.getLiveThreadCount());
  auto workers = dispatcher.getWorkers();
  dispatcher.setMaxThreadCount(1);
  static_cast<AsyncFunc<CountWorker<false>>*>(workers.back()->func())->waitForEnd();
  dispatcher.enqueue([] {});
  EXPECT_EQ(1, dispatcher.getLiveThreadCount());
  dispatcher.stop();
  EXPECT_EQ(0, dispatcher.getLiveThreadCount());
}

TEST(JobQueue, Ordering) {
  {
    // FIFO only.
    JobQueue<int> job_queue(1, std::chrono::nanoseconds::zero(), false, INT_MAX,
                            std::chrono::nanoseconds::zero(), 1);
    for (int i = 0; i < 100; ++i) {
      job_queue.enqueue(i);
    }

    EXPECT_EQ(100, job_queue.getQueuedJobs());

    bool expired;
    for (int i = 0; i < 100; ++i) {
      EXPECT_EQ(i, job_queue.dequeueMaybeExpired(0, 0, false, &expired));
    }
  }

  {
    // LIFO only.
    JobQueue<int> job_queue(1, std::chrono::nanoseconds::zero(), false, 0,
                            std::chrono::nanoseconds::zero(), 1);
    for (int i = 0; i < 100; ++i) {
      job_queue.enqueue(i);
    }

    EXPECT_EQ(100, job_queue.getQueuedJobs());

    bool expired;
    for (int i = 0; i < 100; ++i) {
      EXPECT_EQ(100 - i - 1, job_queue.dequeueMaybeExpired(0, 0,
                                                           false, &expired));
    }
  }

  {
    // Hybrid. First do 50 LIFO, then 50 FIFO.
    JobQueue<int> job_queue(1, std::chrono::nanoseconds(0), false, 50,
                            std::chrono::nanoseconds::zero(), 1);
    for (int i = 0; i < 100; ++i) {
      job_queue.enqueue(i);
    }

    EXPECT_EQ(100, job_queue.getQueuedJobs());

    bool expired;
    for (int i = 0; i < 50; ++i) {
      EXPECT_EQ(100 - i - 1, job_queue.dequeueMaybeExpired(0, 0,
                                                           false, &expired));
    }
    for (int i = 0; i < 50; ++i) {
      EXPECT_EQ(i, job_queue.dequeueMaybeExpired(0, 0, false, &expired));
    }
  }
}

TEST(JobQueue, Expiration) {
  {
    JobQueue<int> fifo_queue(1, std::chrono::nanoseconds::zero(), false,
                             INT_MAX, std::chrono::seconds(30), 1);
    fifo_queue.enqueue(1);
    fifo_queue.enqueue(2);
    fifo_queue.enqueue(3);

    bool expired = false;
    EXPECT_EQ(1, fifo_queue.dequeueMaybeExpiredImpl(0, 0, true, &expired));
    EXPECT_FALSE(expired);
    fifo_queue.m_jobQueues[0].front().second -= std::chrono::seconds(31);
    EXPECT_EQ(2, fifo_queue.dequeueMaybeExpiredImpl(0, 0, true, &expired));
    EXPECT_TRUE(expired);
    EXPECT_EQ(3, fifo_queue.dequeueMaybeExpiredImpl(0, 0, true, &expired));
    EXPECT_FALSE(expired);
  }

  {
    JobQueue<int> lifo_queue(1, std::chrono::nanoseconds::zero(), false, 0,
                             std::chrono::seconds(30), 1);
    lifo_queue.enqueue(1);
    lifo_queue.enqueue(2);
    lifo_queue.enqueue(3);

    bool expired = false;
    EXPECT_EQ(3, lifo_queue.dequeueMaybeExpiredImpl(0, 0, true, &expired));
    EXPECT_FALSE(expired);
    // now we should get a job from the beginning of the queue even though we
    // are in lifo mode before request expiration is enabled.
    lifo_queue.m_jobQueues[0].front().second -= std::chrono::seconds(31);
    EXPECT_EQ(1, lifo_queue.dequeueMaybeExpiredImpl(0, 0, true, &expired));
    EXPECT_TRUE(expired);
    EXPECT_EQ(2, lifo_queue.dequeueMaybeExpiredImpl(0, 0, true, &expired));
    EXPECT_FALSE(expired);
  }

  {
    // job reaper.
    JobQueue<int> lifo_queue(1, std::chrono::nanoseconds::zero(), false, 0,
                             std::chrono::seconds(30), 1);
    lifo_queue.enqueue(1);
    lifo_queue.enqueue(2);
    lifo_queue.enqueue(3);
    lifo_queue.enqueue(4);
    lifo_queue.enqueue(5);

    // manipulate m_jobs timestamp to simulate time passing.
    lifo_queue.m_jobQueues[0][0].second -= std::chrono::seconds(32);
    lifo_queue.m_jobQueues[0][1].second -= std::chrono::seconds(31);

    // having job reaper should not affect anything other threads are doing.
    {
      bool expired = false;
      EXPECT_EQ(1, lifo_queue.dequeueMaybeExpired(0, 0, true, &expired));
      EXPECT_TRUE(expired);
      EXPECT_EQ(2, lifo_queue.dequeueMaybeExpired(1, 0, true, &expired));
      EXPECT_TRUE(expired);
    }

    // now no more jobs are expired. job reaper would block.
    std::atomic<int> value(-1);
    std::thread thread([&]() {
        bool expired;
        value.store(lifo_queue.dequeueMaybeExpired(1, 0, true, &expired));
      });
    EXPECT_EQ(-1, value.load());
    // even if you notify it.
    lifo_queue.notify();
    EXPECT_EQ(-1, value.load());

    {
      // but normal workers should proceed.
      bool expired = false;
      EXPECT_EQ(5, lifo_queue.dequeueMaybeExpired(0, 0, true, &expired));
      EXPECT_FALSE(expired);
    }

    // now set the first job to be expired.
    lifo_queue.m_jobQueues[0][0].second -= std::chrono::seconds(32);
    lifo_queue.notify();

    // busy wait until value is updated.
    thread.join();
    EXPECT_EQ(3, value.load());

    // stop case
    bool exceptionCaught = false;
    std::thread thread1([&]() {
        bool expired;
        try {
          lifo_queue.dequeueMaybeExpired(1, 0, true, &expired);
        } catch (const JobQueue<int>::StopSignal&) {
          exceptionCaught = true;
        }
      });
    lifo_queue.stop();
    thread1.join();
    EXPECT_TRUE(exceptionCaught);
  }
}

TEST(JobQueue, Priority) {
  JobQueue<int> fifo_queue(1, std::chrono::nanoseconds::zero(), false, INT_MAX,
                           std::chrono::milliseconds(30), 3);
  fifo_queue.enqueue(1);
  fifo_queue.enqueue(2);
  fifo_queue.enqueue(3, 2);
  fifo_queue.enqueue(4, 0);
  fifo_queue.enqueue(5, 1);
  fifo_queue.enqueue(6, 2);

  bool expired;
  EXPECT_EQ(3, fifo_queue.dequeueMaybeExpired(0, 0, true, &expired));
  EXPECT_EQ(6, fifo_queue.dequeueMaybeExpired(0, 0, true, &expired));
  EXPECT_EQ(5, fifo_queue.dequeueMaybeExpired(0, 0, true, &expired));
  EXPECT_EQ(1, fifo_queue.dequeueMaybeExpired(0, 0, true, &expired));
  EXPECT_EQ(2, fifo_queue.dequeueMaybeExpired(0, 0, true, &expired));
  EXPECT_EQ(4, fifo_queue.dequeueMaybeExpired(0, 0, true, &expired));
}

}
