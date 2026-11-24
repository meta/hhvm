/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/watchman_cmd.h" // @donotremove

#if HAVE_FSEVENTS && defined(HAVE_KQUEUE)

#include <folly/Synchronized.h>
#include <condition_variable>
#include <mutex>
#include "watchman/Client.h"
#include "watchman/InMemoryView.h"
#include "watchman/root/Root.h"
#include "watchman/watcher/WatcherRegistry.h"
#include "watchman/watcher/fsevents.h"
#include "watchman/watcher/kqueue.h"
#include "watchman/watchman_file.h"

namespace watchman {

class PendingEventsCond {
 public:
  /**
   * Notify that some events are pending.
   *
   * Return true if this thread should stop, false otherwise.
   */
  bool notifyOneOrStop() {
    auto lock = stop_.lock();
    if (lock->shouldStop) {
      return true;
    }
    lock->hasPending = true;
    cond_.notify_one();
    return false;
  }

  /**
   * Whether this thread should stop.
   */
  bool shouldStop() {
    return stop_.lock()->shouldStop;
  }

  /**
   * Wait for a change from a nested watcher.
   */
  Watcher::WaitNotifyResult waitAndClear(int timeoutms) {
    auto lock = stop_.lock();
    cond_.wait_until(
        lock.as_lock(),
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutms),
        [&] { return lock->hasPending || lock->shouldStop; });
    if (lock->shouldStop) {
      return Watcher::WaitNotifyResult::Terminate;
    }
    if (lock->hasPending) {
      lock->hasPending = false;
      return Watcher::WaitNotifyResult::Ready;
    }
    return Watcher::WaitNotifyResult::Timeout;
  }

  /**
   * Notify all the waiting threads to stop.
   */
  void stopAll() {
    auto lock = stop_.lock();
    lock->shouldStop = true;
    cond_.notify_all();
  }

 private:
  struct Inner {
    bool shouldStop = false;
    bool hasPending = false;
  };

  folly::Synchronized<Inner, std::mutex> stop_;
  std::condition_variable cond_;
};

/**
 * Watcher that uses both kqueue and fsevents to watch a hierarchy.
 *
 * The kqueue watches are used on the root directory and all the files at the
 * root, while the fsevents one is used on the subdirectories.
 */
class KQueueAndFSEventsWatcher : public Watcher {
 public:
  explicit KQueueAndFSEventsWatcher(
      const w_string& root_path,
      const Configuration& config);

  bool start(const std::shared_ptr<Root>& root) override;

  folly::SemiFuture<folly::Unit> flushPendingEvents() override;

  std::unique_ptr<DirHandle> startWatchDir(
      const std::shared_ptr<Root>& root,
      const char* path) override;

  bool startWatchFile(struct watchman_file* file) override;

  Watcher::ConsumeNotifyRet consumeNotify(
      const std::shared_ptr<Root>& root,
      PendingChanges& coll) override;

  WaitNotifyResult waitNotify(int timeoutms) override;
  void stopThreads() override;

  /**
   * Force a recrawl to be injected in the stream. Used in the
   * 'debug-kqueue-and-fsevents-recrawl' command.
   */
  void injectRecrawl(w_string path);

 private:
  folly::Synchronized<
      std::unordered_map<w_string, std::shared_ptr<FSEventsWatcher>>>
      fseventWatchers_;
  std::shared_ptr<KQueueWatcher> kqueueWatcher_;

  std::shared_ptr<PendingEventsCond> pendingCondition_;

  folly::Synchronized<std::optional<w_string>> injectedRecrawl_;
};

KQueueAndFSEventsWatcher::KQueueAndFSEventsWatcher(
    const w_string& root_path,
    const Configuration& config)
    : Watcher(
          "kqueue+fsevents",
          WATCHER_HAS_SPLIT_WATCH |
              // The top-level directories' FSEventsWatchers read this too.
              (config.getBool("fsevents_watch_files", true)
                   ? WATCHER_HAS_PER_FILE_NOTIFICATIONS_BELOW_ROOT
                   : 0)),
      kqueueWatcher_(std::make_shared<KQueueWatcher>(root_path, config, false)),
      pendingCondition_(std::make_shared<PendingEventsCond>()) {}

namespace {
bool startThread(
    const std::shared_ptr<Root>& root,
    const std::shared_ptr<Watcher>& watcher,
    const std::shared_ptr<PendingEventsCond>& cond) {
  std::weak_ptr<Watcher> weakWatcher(watcher);
  std::thread thr([weakWatcher, root, cond]() {
    while (true) {
      auto watcher = weakWatcher.lock();
      if (!watcher) {
        break;
      }
      auto waitResult = watcher->waitNotify(86400);
      if (waitResult == Watcher::WaitNotifyResult::Ready) {
        if (cond->notifyOneOrStop()) {
          return;
        }
      } else if (
          waitResult == Watcher::WaitNotifyResult::Terminate ||
          cond->shouldStop()) {
        return;
      }
    }
  });
  thr.detach();
  return true;
}
} // namespace

bool KQueueAndFSEventsWatcher::start(const std::shared_ptr<Root>& root) {
  root->cookies.addCookieDir(root->root_path);
  return startThread(root, kqueueWatcher_, pendingCondition_);
}

folly::SemiFuture<folly::Unit> KQueueAndFSEventsWatcher::flushPendingEvents() {
  // Flush the kqueue watcher outside of the lock, because it may need to
  // change the set of watchers.
  auto kqueueFlush = kqueueWatcher_->flushPendingEvents();
  // But we know KQueueWatcher doesn't implement flushPendingEvents, so to
  // avoid having to chain the futures here, just assert.
  w_check(
      !kqueueFlush.valid(),
      "This code needs to be updated to handle KQueueWatcher implementing flushPendingEvents");

  auto fseventsWatchers = *fseventWatchers_.rlock();

  std::vector<folly::SemiFuture<folly::Unit>> futures;
  futures.reserve(fseventsWatchers.size());
  for (auto& [name, watcher] : fseventsWatchers) {
    auto future = watcher->flushPendingEvents();
    if (future.valid()) {
      futures.push_back(std::move(future));
    }
  }
  return folly::collect(futures).unit();
}

std::unique_ptr<DirHandle> KQueueAndFSEventsWatcher::startWatchDir(
    const std::shared_ptr<Root>& root,
    const char* path) {
  // Open the directory first to validate that the path is the canonical one.
  // This will throw an exception if it's not.
  auto ret = openDir(path);

  if (root->root_path == path) {
    logf(DBG, "Watching root directory with kqueue\n");
    // This is the root, let's watch it with kqueue.
    kqueueWatcher_->startWatchDir(root, path);
  } else {
    w_string fullPath{path};
    if (root->root_path == fullPath.dirName()) {
      auto wlock = fseventWatchers_.wlock();
      if (wlock->find(fullPath) == wlock->end()) {
        logf(
            DBG,
            "Creating a new FSEventsWatcher for top-level directory {}\n",
            fullPath);
        auto [it, _] = wlock->emplace(
            fullPath,
            std::make_shared<FSEventsWatcher>(
                root->root_path, root->config, std::optional(fullPath)));
        const auto& watcher = it->second;
        if (!watcher->start(root)) {
          throw std::runtime_error("couldn't start fsEvent");
        }
        if (!startThread(root, watcher, pendingCondition_)) {
          throw std::runtime_error("couldn't start fsEvent");
        }
        // FSEvents doesn't report a cookie written before its stream started.
        root->cookies.addCookieDir(fullPath);
      }
    }
  }

  return ret;
}

bool KQueueAndFSEventsWatcher::startWatchFile(struct watchman_file* file) {
  if (file->parent->parent == nullptr && !file->stat.isDir()) {
    // File at the root, watch it with kqueue.
    return kqueueWatcher_->startWatchFile(file);
  }

  // FSEvent by default watches all the files recursively, we don't need to do
  // anything.
  return true;
}

Watcher::ConsumeNotifyRet KQueueAndFSEventsWatcher::consumeNotify(
    const std::shared_ptr<Root>& root,
    PendingChanges& coll) {
  {
    auto guard = injectedRecrawl_.wlock();
    if (guard->has_value()) {
      const auto& injectedDir = guard->value();

      auto now = std::chrono::system_clock::now();
      coll.add(
          injectedDir,
          now,
          W_PENDING_VIA_NOTIFY | W_PENDING_RECURSIVE | W_PENDING_IS_DESYNCED);

      guard->reset();
    }
  }

  {
    auto fseventWatches = fseventWatchers_.wlock();
    auto it = fseventWatches->begin();
    while (it != fseventWatches->end()) {
      auto& [watchpath, fsevent] = *it;
      auto [cancelSelf] = fsevent->consumeNotify(root, coll);
      if (cancelSelf) {
        fsevent->stopThreads();
        root->cookies.removeCookieDir(watchpath);
        // The crawl of the root does not look at this path if something is
        // there again, so nothing else finds out what it holds now.
        coll.add(
            watchpath,
            std::chrono::system_clock::now(),
            W_PENDING_VIA_NOTIFY | W_PENDING_RECURSIVE);
        it = fseventWatches->erase(it);
        continue;
      } else {
        ++it;
      }
    }
  }

  return kqueueWatcher_->consumeNotify(root, coll);
}

Watcher::WaitNotifyResult KQueueAndFSEventsWatcher::waitNotify(int timeoutms) {
  return pendingCondition_->waitAndClear(timeoutms);
}

void KQueueAndFSEventsWatcher::stopThreads() {
  pendingCondition_->stopAll();
  {
    auto fseventWatches = fseventWatchers_.rlock();
    for (auto& [_, fsevent] : *fseventWatches) {
      fsevent->stopThreads();
    }
  }
  kqueueWatcher_->stopThreads();
}

void KQueueAndFSEventsWatcher::injectRecrawl(w_string path) {
  *injectedRecrawl_.wlock() = path;
  pendingCondition_->notifyOneOrStop();
}

namespace {
std::shared_ptr<InMemoryView> makeKQueueAndFSEventsWatcher(
    const w_string& root_path,
    const w_string& /*fstype*/,
    const Configuration& config) {
  if (config.getBool("prefer_split_fsevents_watcher", false)) {
    return std::make_shared<InMemoryView>(
        realFileSystem,
        root_path,
        config,
        std::make_shared<KQueueAndFSEventsWatcher>(root_path, config));
  } else {
    throw std::runtime_error(
        "Not using the kqueue+fsevents watcher as the \"prefer_split_fsevents_watcher\" config isn't set");
  }
}
} // namespace

static WatcherRegistry reg("kqueue+fsevents", makeKQueueAndFSEventsWatcher, 5);

namespace {

std::shared_ptr<KQueueAndFSEventsWatcher> watcherFromRoot(
    const std::shared_ptr<Root>& root) {
  auto view = std::dynamic_pointer_cast<watchman::InMemoryView>(root->view());
  if (!view) {
    return nullptr;
  }

  return std::dynamic_pointer_cast<KQueueAndFSEventsWatcher>(
      view->getWatcher());
}

static UntypedResponse cmd_debug_kqueue_and_fsevents_recrawl(
    Client* client,
    const json_ref& args) {
  /* resolve the root */
  if (json_array_size(args) != 3) {
    throw ErrorResponse(
        "wrong number of arguments for 'debug-kqueue-and-fsevents-recrawl'");
  }

  auto root = resolveRoot(client, args);

  auto watcher = watcherFromRoot(root);
  if (!watcher) {
    throw ErrorResponse("root is not using the kqueue+fsevents watcher");
  }

  /* Get the path that the recrawl should be triggered on */
  const auto& json_path = args.at(2);
  auto path = json_string_value(json_path);
  if (!path) {
    throw ErrorResponse(
        "invalid value for argument 2, expected a string naming the path to trigger a recrawl on");
  }

  watcher->injectRecrawl(path);

  return UntypedResponse{};
}

} // namespace

W_CMD_REG(
    "debug-kqueue-and-fsevents-recrawl",
    cmd_debug_kqueue_and_fsevents_recrawl,
    CMD_DAEMON,
    w_cmd_realpath_root);

} // namespace watchman

#endif
