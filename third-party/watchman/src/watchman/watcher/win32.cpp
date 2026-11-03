/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/Synchronized.h>
#include "watchman/InMemoryView.h"
#include "watchman/fs/FileDescriptor.h"
#include "watchman/portability/WinError.h"
#include "watchman/root/Root.h"
#include "watchman/watcher/Watcher.h"
#include "watchman/watcher/WatcherRegistry.h"

#include <algorithm>
#include <condition_variable>
#include <iterator>
#include <list>
#include <mutex>
#include <tuple>

using namespace watchman;

#ifdef _WIN32

namespace {

constexpr DWORD kNetworkBufSize = 64 * 1024;

struct Item {
  w_string path;
  PendingFlags flags;

  Item(w_string&& path, PendingFlags flags)
      : path(std::move(path)), flags(flags) {}
};

} // namespace

struct WinWatcher : public Watcher {
  HANDLE ping{INVALID_HANDLE_VALUE};
  HANDLE olapEvent{INVALID_HANDLE_VALUE};
  FileDescriptor dir_handle;

  std::condition_variable cond;
  folly::Synchronized<std::list<Item>, std::mutex> changedItems;

  explicit WinWatcher(const w_string& root_path, const Configuration& config);
  ~WinWatcher();

  std::unique_ptr<DirHandle> startWatchDir(
      const std::shared_ptr<Root>& root,
      const char* path) override;

  Watcher::ConsumeNotifyRet consumeNotify(
      const std::shared_ptr<Root>& root,
      PendingChanges& coll) override;

  WaitNotifyResult waitNotify(int timeoutms) override;
  bool start(const std::shared_ptr<Root>& root) override;
  void stopThreads() override;
  void readChangesThread(const std::shared_ptr<Root>& root);
};

WinWatcher::WinWatcher(const w_string& root_path, const Configuration& config)
    : Watcher("win32", WATCHER_HAS_PER_FILE_NOTIFICATIONS) {
  auto wpath = root_path.piece().asWideUNC();

  // Create an overlapped handle so that we can avoid blocking forever
  // in ReadDirectoryChangesW
  dir_handle = FileDescriptor(
      intptr_t(CreateFileW(
          wpath.c_str(),
          GENERIC_READ,
          FILE_SHARE_READ | FILE_SHARE_DELETE | FILE_SHARE_WRITE,
          nullptr,
          OPEN_EXISTING,
          FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
          nullptr)),
      FileDescriptor::FDType::Generic);

  if (!dir_handle) {
    throw std::runtime_error(
        std::string("failed to open dir ") + root_path.c_str() + ": " +
        win32_strerror(GetLastError()));
  }

  ping = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  if (!ping) {
    throw std::runtime_error(
        std::string("failed to create event: ") +
        win32_strerror(GetLastError()));
  }
  olapEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  if (!olapEvent) {
    throw std::runtime_error(
        std::string("failed to create event: ") +
        win32_strerror(GetLastError()));
  }
}

WinWatcher::~WinWatcher() {
  if (ping != INVALID_HANDLE_VALUE) {
    CloseHandle(ping);
  }
  if (olapEvent != INVALID_HANDLE_VALUE) {
    CloseHandle(olapEvent);
  }
}

void WinWatcher::stopThreads() {
  SetEvent(ping);
}

void WinWatcher::readChangesThread(const std::shared_ptr<Root>& root) {
  std::vector<uint8_t> buf;
  auto olap = OVERLAPPED();
  BOOL initiate_read = true;
  HANDLE handles[2] = {olapEvent, ping};
  DWORD bytes;

  w_set_thread_name("readchange ", root->root_path.view());
  watchman::log(watchman::DBG, "initializing\n");

  // Artificial extra latency to impose around processing changes.
  // This is needed to avoid trying to access files and dirs too
  // soon after a change is noticed, as this can cause recursive
  // deletes to fail.
  auto extraLatency = root->config.getInt("win32_batch_latency_ms", 30);

  DWORD size = root->config.getInt("win32_rdcw_buf_size", 16384);

  DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
      FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE |
      FILE_NOTIFY_CHANGE_LAST_WRITE;

  // Block until winmatch_root_st is waiting for our initialization
  {
    auto wlock = changedItems.lock();

    olap.hEvent = olapEvent;

    buf.resize(size);

    if (!ReadDirectoryChangesW(
            (HANDLE)dir_handle.handle(),
            &buf[0],
            size,
            TRUE,
            filter,
            nullptr,
            &olap,
            nullptr)) {
      DWORD err = GetLastError();
      logf(
          ERR,
          "ReadDirectoryChangesW: failed, cancel watch. {}\n",
          win32_strerror(err));
      root->cancel(
          fmt::format("ReadDirectoryChangesW failed: {}", win32_strerror(err)));
      return;
    }
    // Signal that we are done with init.  We MUST do this AFTER our first
    // successful ReadDirectoryChangesW, otherwise there is a race condition
    // where we'll miss observing the cookie for a query that comes in
    // after we've crawled but before the watch is established.
    logf(DBG, "ReadDirectoryChangesW signalling as init done\n");
    cond.notify_one();
  }
  initiate_read = false;

  std::list<Item> items;

  // The mutex must not be held when we enter the loop
  while (!root->inner.cancelled) {
    if (initiate_read) {
      if (!ReadDirectoryChangesW(
              (HANDLE)dir_handle.handle(),
              &buf[0],
              size,
              TRUE,
              filter,
              nullptr,
              &olap,
              nullptr)) {
        DWORD err = GetLastError();
        logf(
            ERR,
            "ReadDirectoryChangesW: failed, cancel watch. {}\n",
            win32_strerror(err));
        root->cancel(
            fmt::format(
                "ReadDirectoryChangesW failed: {}", win32_strerror(err)));
        break;
      } else {
        initiate_read = false;
      }
    }

    watchman::log(watchman::DBG, "waiting for change notifications\n");
    DWORD status = WaitForMultipleObjects(
        2,
        handles,
        FALSE,
        // We use a 10 second timeout by default until we start accumulating a
        // batch.  Once we have a batch we prefer to add more to it than notify
        // immediately, so we introduce a 30ms latency.  Without this artificial
        // latency we'll wake up and start trying to look at a directory that
        // may be in the process of being recursively deleted and that act can
        // block the recursive delete.
        items.empty() ? 10000 : extraLatency);
    watchman::log(watchman::DBG, "wait returned with status ", status, "\n");

    if (status == WAIT_OBJECT_0) {
      bytes = 0;
      if (!GetOverlappedResult(
              (HANDLE)dir_handle.handle(), &olap, &bytes, FALSE)) {
        DWORD err = GetLastError();
        logf(
            ERR,
            "overlapped ReadDirectoryChangesW({}): {:x} {}\n",
            root->root_path,
            err,
            win32_strerror(err));

        if (err == ERROR_INVALID_PARAMETER && size > kNetworkBufSize) {
          // May be a network buffer related size issue; the docs say that
          // we can hit this when watching a UNC path. Let's downsize and
          // retry the read just one time
          logf(
              ERR,
              "retrying watch for possible network location {} "
              "with smaller buffer\n",
              root->root_path);
          size = kNetworkBufSize;
          initiate_read = true;
          continue;
        }

        if (err == ERROR_NOTIFY_ENUM_DIR) {
          root->recrawlTriggered(
              "GetOverlappedResult failed with ERROR_NOTIFY_ENUM_DIR");
          items.emplace_back(
              w_string{root->root_path},
              W_PENDING_IS_DESYNCED | W_PENDING_RECURSIVE);
        } else {
          logf(ERR, "Cancelling watch for {}\n", root->root_path);
          root->cancel(
              fmt::format(
                  "unexpected error from GetOverlappedResult: {}",
                  win32_strerror(err)));
          break;
        }
      } else {
        if (bytes == 0) {
          root->recrawlTriggered("ReadDirectoryChangesW overflowed");
          items.emplace_back(
              w_string{root->root_path},
              W_PENDING_IS_DESYNCED | W_PENDING_RECURSIVE);
        } else {
          PFILE_NOTIFY_INFORMATION notify =
              (PFILE_NOTIFY_INFORMATION)buf.data();

          while (true) {
            // FileNameLength is in BYTES, but FileName is WCHAR
            DWORD n_chars =
                notify->FileNameLength / sizeof(notify->FileName[0]);
            w_string name(notify->FileName, n_chars);

            auto full = w_string::pathCat({root->root_path, name});

            if (!root->ignore.isIgnored(full.data(), full.size())) {
              // If we have a delete or rename-away it may be part of
              // a recursive tree remove or rename.  In that situation
              // the notifications that we'll receive from the OS will
              // be from the leaves and bubble up to the root of the
              // delete/rename.  We want to flag those paths for recursive
              // analysis so that we can prune children from the trie
              // that is built when we pass this to the pending list
              // later.  We don't do that here in this thread because
              // we're trying to minimize latency in this context.
              items.emplace_back(
                  w_string{full},
                  (notify->Action == FILE_ACTION_REMOVED ||
                   notify->Action == FILE_ACTION_RENAMED_OLD_NAME)
                      ? W_PENDING_RECURSIVE
                      : 0);

              if (!name.empty() &&
                  (notify->Action == FILE_ACTION_ADDED ||
                   notify->Action == FILE_ACTION_REMOVED ||
                   notify->Action == FILE_ACTION_RENAMED_OLD_NAME ||
                   notify->Action == FILE_ACTION_RENAMED_NEW_NAME)) {
                // ReadDirectoryChangesW provides change events when the child
                // entry list changes, but may not provide a notification for
                // the parent when its mtime changes. It should be rescanned, so
                // synthesize an event for the IO thread here.
                items.emplace_back(full.dirName(), PendingFlags{});
              }
            }

            // Advance to next item
            if (notify->NextEntryOffset == 0) {
              break;
            }
            notify = (PFILE_NOTIFY_INFORMATION)(notify->NextEntryOffset +
                                                (char*)notify);
          }
        }

        ResetEvent(olapEvent);
        initiate_read = true;
      }
    } else if (status == WAIT_OBJECT_0 + 1) {
      logf(ERR, "signalled\n");
      break;
    } else if (status == WAIT_TIMEOUT) {
      if (!items.empty()) {
        watchman::log(
            watchman::DBG,
            "timed out waiting for changes, and we have ",
            items.size(),
            " items; move and notify\n");
        auto wlock = changedItems.lock();
        wlock->splice(wlock->end(), items);
        cond.notify_one();
      }
    } else {
      logf(ERR, "impossible wait status={}\n", status);
      break;
    }
  }

  logf(DBG, "done\n");
}

bool WinWatcher::start(const std::shared_ptr<Root>& root) {
  // Spin up the changes reading thread; it owns a ref on the root

  try {
    // Acquire the mutex so thread initialization waits until we release it
    auto wlock = changedItems.lock();

    watchman::log(watchman::DBG, "starting readChangesThread\n");
    auto self = std::dynamic_pointer_cast<WinWatcher>(shared_from_this());
    std::thread thread([self, root]() noexcept {
      try {
        self->readChangesThread(root);
      } catch (const std::exception& e) {
        watchman::log(watchman::ERR, "uncaught exception: ", e.what());
        root->cancel(fmt::format("readChangesThread errored: {}", e.what()));
      }

      // Ensure that we signal the condition variable before we
      // finish this thread.  That ensures that don't get stuck
      // waiting in WinWatcher::start if something unexpected happens.
      auto wlock = self->changedItems.lock();
      self->cond.notify_one();
    });
    // We have to detach because the readChangesThread may wind up
    // being the last thread to reference the watcher state and
    // cannot join itself.
    thread.detach();

    // Allow thread init to proceed; wait for its signal
    if (cond.wait_for(wlock.as_lock(), std::chrono::seconds(10)) ==
        std::cv_status::timeout) {
      watchman::log(
          watchman::ERR, "timedout waiting for readChangesThread to start\n");
      root->cancel("timedout waiting for readChangesThread to start");
      return false;
    }

    if (root->failure_reason) {
      logf(
          ERR,
          "failed to start readchanges thread: {}\n",
          *root->failure_reason);
      return false;
    }
    return true;
  } catch (const std::exception& e) {
    logf(ERR, "failed to start readchanges thread: {}\n", e.what());
    return false;
  }
}

std::unique_ptr<DirHandle> WinWatcher::startWatchDir(
    const std::shared_ptr<Root>& root,
    const char* path) {
  return openDir(path);
}

Watcher::ConsumeNotifyRet WinWatcher::consumeNotify(
    const std::shared_ptr<Root>& root,
    PendingChanges& coll) {
  std::list<Item> items;

  {
    auto wlock = changedItems.lock();
    std::swap(items, *wlock);
  }

  auto now = std::chrono::system_clock::now();

  for (auto& item : items) {
    watchman::log(
        watchman::DBG,
        "readchanges: add pending ",
        item.path,
        " ",
        item.flags.format(),
        "\n");
    coll.add(item.path, now, W_PENDING_VIA_NOTIFY | item.flags);
  }

  // The readChangesThread cancels itself.
  return {false};
}

Watcher::WaitNotifyResult WinWatcher::waitNotify(int timeoutms) {
  auto wlock = changedItems.lock();
  if (!wlock->empty()) {
    return WaitNotifyResult::Ready;
  }
  cond.wait_for(wlock.as_lock(), std::chrono::milliseconds(timeoutms));
  return !wlock->empty() ? WaitNotifyResult::Ready : WaitNotifyResult::Timeout;
}

static RegisterWatcher<WinWatcher> reg("win32");

#endif // _WIN32

/* vim:ts=2:sw=2:et:
 */
