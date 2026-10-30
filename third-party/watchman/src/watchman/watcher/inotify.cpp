/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/String.h>
#include <folly/Synchronized.h>
#include <atomic>
#include "eden/common/utils/FSDetect.h"
#include "watchman/Constants.h"
#include "watchman/Errors.h"
#include "watchman/FlagMap.h"
#include "watchman/InMemoryView.h"
#include "watchman/Poison.h"
#include "watchman/RingBuffer.h"
#include "watchman/fs/FSDetect.h"
#include "watchman/fs/FileDescriptor.h"
#include "watchman/fs/Pipe.h"
#include "watchman/root/Root.h"
#include "watchman/watcher/Watcher.h"
#include "watchman/watcher/WatcherRegistry.h"

#ifdef HAVE_INOTIFY_INIT

using namespace watchman;

#ifndef IN_EXCL_UNLINK
/* defined in <linux/inotify.h> but we can't include that without
 * breaking userspace */
#define WATCHMAN_IN_EXCL_UNLINK 0x04000000
#else
#define WATCHMAN_IN_EXCL_UNLINK IN_EXCL_UNLINK
#endif

#define WATCHMAN_INOTIFY_MASK                                       \
  IN_ATTRIB | IN_CREATE | IN_DELETE | IN_DELETE_SELF | IN_MODIFY |  \
      IN_MOVE_SELF | IN_MOVED_FROM | IN_MOVED_TO | IN_DONT_FOLLOW | \
      IN_ONLYDIR | WATCHMAN_IN_EXCL_UNLINK

namespace {

const struct flag_map inflags[] = {
    {IN_ACCESS, "IN_ACCESS"},
    {IN_MODIFY, "IN_MODIFY"},
    {IN_ATTRIB, "IN_ATTRIB"},
    {IN_CLOSE_WRITE, "IN_CLOSE_WRITE"},
    {IN_CLOSE_NOWRITE, "IN_CLOSE_NOWRITE"},
    {IN_OPEN, "IN_OPEN"},
    {IN_MOVED_FROM, "IN_MOVED_FROM"},
    {IN_MOVED_TO, "IN_MOVED_TO"},
    {IN_CREATE, "IN_CREATE"},
    {IN_DELETE, "IN_DELETE"},
    {IN_DELETE_SELF, "IN_DELETE_SELF"},
    {IN_MOVE_SELF, "IN_MOVE_SELF"},
    {IN_UNMOUNT, "IN_UNMOUNT"},
    {IN_Q_OVERFLOW, "IN_Q_OVERFLOW"},
    {IN_IGNORED, "IN_IGNORED"},
    {IN_ISDIR, "IN_ISDIR"},
    {0, nullptr},
};

struct pending_move {
  std::chrono::system_clock::time_point created;
  w_string name;

  pending_move(
      std::chrono::system_clock::time_point created,
      const w_string& name)
      : created(created), name(name) {}
};

/**
 * Memory-efficient records for debugging inotify events after the fact.
 */
struct InotifyLogEntry {
  // 52 should cover many filenames.
  static constexpr size_t kNameLength = 52;

  InotifyLogEntry() = default;

  explicit InotifyLogEntry(inotify_event* evt) noexcept {
    wd = evt->wd;
    mask = evt->mask;
    cookie = evt->cookie;

    // If evt->len is nonzero, evt->name is a null-terminated string.
    bool has_name = evt->len > 0;
    if (has_name) {
      auto piece = w_string_piece{evt->name}; // Evaluate strlen here.
      storeTruncatedHead(name, piece);
    } else {
      name[0] = 0;
    }
  }

  json_ref asJsonValue() {
    size_t length = strnlen(name, kNameLength);
    auto namePiece = w_string_piece{name, length};

    return json_object({
        {"wd", json_integer(wd)},
        {"mask", json_integer(mask)},
        {"cookie", json_integer(cookie)},
        {"name", typed_string_to_json(namePiece.asWString())},
    });
  }

  w_string_piece namePiece() const {
    size_t length = strnlen(name, kNameLength);
    return w_string_piece{name, length};
  }

  int wd;
  uint32_t mask;
  uint32_t cookie;
  // Will end with ... if truncated. May not be null-terminated.
  char name[kNameLength];
};
static_assert(64 == sizeof(InotifyLogEntry));

} // namespace

struct InotifyWatcher : public Watcher {
  /* we use one inotify instance per watched root dir */
  FileDescriptor infd;
  Pipe terminatePipe_;

  /**
   * If not null, holds a fixed-size ring of the last `inotify_ring_log_size`
   * inotify events.
   */
  std::unique_ptr<RingBuffer<InotifyLogEntry>> ringBuffer_;

  /**
   * Published from consumeNotify so getDebugInfo can read a recent value.
   */
  std::atomic<uint64_t> totalEventsSeen_ = 0;

  struct maps {
    /* map of active watch descriptor to name of the corresponding dir */
    std::unordered_map<int, w_string> wd_to_name;
    /* map of inotify cookie to corresponding name */
    std::unordered_map<uint32_t, pending_move> move_map;
  };

  folly::Synchronized<maps> maps;

  // Make the buffer big enough for 16k entries, which
  // happens to be the default fs.inotify.max_queued_events
  char ibuf
      [WATCHMAN_BATCH_LIMIT * (sizeof(struct inotify_event) + (NAME_MAX + 1))];

  explicit InotifyWatcher(const Configuration& config);

  std::unique_ptr<DirHandle> startWatchDir(
      const std::shared_ptr<Root>& root,
      const char* path) override;

  Watcher::ConsumeNotifyRet consumeNotify(
      const std::shared_ptr<Root>& root,
      PendingChanges& coll) override;

  WaitNotifyResult waitNotify(int timeoutms) override;

  // Process a single inotify event and add it to the pending collection if
  // needed. Returns true if the root directory was removed and the watch needs
  // to be cancelled.
  bool process_inotify_event(
      const std::shared_ptr<Root>& root,
      PendingChanges& coll,
      struct inotify_event* ine,
      std::chrono::system_clock::time_point now);

  void stopThreads() override;

  json_ref getDebugInfo() override;
  void clearDebugInfo() override;
};

InotifyWatcher::InotifyWatcher(const Configuration& config)
    : Watcher("inotify", WATCHER_HAS_PER_FILE_NOTIFICATIONS) {
#ifdef HAVE_INOTIFY_INIT1
  infd = FileDescriptor(
      inotify_init1(IN_CLOEXEC), FileDescriptor::FDType::Generic);
#else
  infd = FileDescriptor(inotify_init(), FileDescriptor::FDType::Generic);
#endif
  if (infd.fd() == -1) {
    throw std::system_error(errno, inotify_category(), "inotify_init");
  }
  infd.setCloExec();

  {
    auto wlock = maps.wlock();
    wlock->wd_to_name.reserve(config.getInt(CFG_HINT_NUM_DIRS, HINT_NUM_DIRS));
  }

  json_int_t inotify_ring_log_size = config.getInt("inotify_ring_log_size", 0);
  if (inotify_ring_log_size) {
    ringBuffer_ =
        std::make_unique<RingBuffer<InotifyLogEntry>>(inotify_ring_log_size);
  }
}

std::unique_ptr<DirHandle> InotifyWatcher::startWatchDir(
    const std::shared_ptr<Root>&,
    const char* path) {
  // Carry out our very strict opendir first to ensure that we're not
  // traversing symlinks in the context of this root
  auto osdir = openDir(path);

  w_string dir_name(path, W_STRING_BYTE);

  // The directory might be different since the last time we looked at it, so
  // call inotify_add_watch unconditionally.
  int newwd = inotify_add_watch(infd.fd(), path, WATCHMAN_INOTIFY_MASK);
  if (newwd == -1) {
    int err = errno;
    throw std::system_error(err, inotify_category(), "inotify_add_watch");
  }

  // record mapping
  {
    auto wlock = maps.wlock();
    wlock->wd_to_name[newwd] = dir_name;
  }
  logf(DBG, "adding {} -> {} mapping\n", newwd, path);

  // Btrfs can snapshot directory entries when the handle is opened.
  osdir.reset();
  return openDir(path);
}

bool InotifyWatcher::process_inotify_event(
    const std::shared_ptr<Root>& root,
    PendingChanges& coll,
    struct inotify_event* ine,
    std::chrono::system_clock::time_point now) {
  char flags_label[128];
  w_expand_flags(inflags, ine->mask, flags_label, sizeof(flags_label));

  logf(
      DBG,
      "notify: wd={} mask={:x} {} {}\n",
      ine->wd,
      ine->mask,
      flags_label,
      ine->len > 0 ? ine->name : "");

  if (ringBuffer_) {
    ringBuffer_->write(InotifyLogEntry{ine});
  }

  if (ine->wd == -1 && (ine->mask & IN_Q_OVERFLOW)) {
    /* we missed something, will need to re-crawl */
    root->scheduleRecrawl("IN_Q_OVERFLOW");
  } else if (ine->wd != -1) {
    w_string name;
    char buf[WATCHMAN_NAME_MAX];
    PendingFlags pending_flags = W_PENDING_VIA_NOTIFY;
    std::optional<w_string> dir_name;

    {
      auto rlock = maps.rlock();
      auto it = rlock->wd_to_name.find(ine->wd);
      if (it != rlock->wd_to_name.end()) {
        dir_name = it->second;
      }
    }

    if (dir_name) {
      if (ine->len > 0) {
        // TODO: What if this truncates?
        snprintf(
            buf,
            sizeof(buf),
            "%.*s/%s",
            int(dir_name->size()),
            dir_name->data(),
            ine->name);
        name = w_string(buf, W_STRING_BYTE);
      } else {
        name = *dir_name;
      }
    }

    if (ine->len > 0 &&
        (ine->mask & (IN_MOVED_FROM | IN_ISDIR)) ==
            (IN_MOVED_FROM | IN_ISDIR)) {
      // record this as a pending move, so that we can automatically
      // watch the target when we get the other side of it.
      {
        auto wlock = maps.wlock();
        wlock->move_map.emplace(ine->cookie, pending_move(now, name));
      }

      log(DBG, "recording move_from ", ine->cookie, " ", name, "\n");
    }

    if (ine->len > 0 &&
        (ine->mask & (IN_MOVED_TO | IN_ISDIR)) == (IN_MOVED_FROM | IN_ISDIR)) {
      auto wlock = maps.wlock();
      auto it = wlock->move_map.find(ine->cookie);
      if (it != wlock->move_map.end()) {
        auto& old = it->second;
        int wd =
            inotify_add_watch(infd.fd(), name.c_str(), WATCHMAN_INOTIFY_MASK);
        if (wd == -1) {
          if (errno == ENOSPC || errno == ENOMEM) {
            // Limits exceeded, no recovery from our perspective
            set_poison_state(
                name,
                now,
                "inotify-add-watch",
                std::error_code(errno, inotify_category()));
          } else {
            watchman::log(
                watchman::DBG,
                "add_watch: ",
                name,
                " ",
                inotify_category().message(errno),
                "\n");
          }
        } else {
          logf(DBG, "moved {} -> {}\n", old.name.c_str(), name.c_str());
          // TODO: assert that there is no entry in wd_to_name
          wlock->wd_to_name[wd] = name;
        }
      } else {
        logf(
            DBG,
            "move: cookie={:x} not found in move map {}\n",
            ine->cookie,
            name);
      }
    }

    if (dir_name) {
      if ((ine->mask &
           (IN_UNMOUNT | IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF))) {
        if (root->root_path == name) {
          logf(
              ERR,
              "root dir {} has been (re)moved, canceling watch\n",
              root->root_path);
          return true;
        }

        // We need to examine the parent and potentially crawl down
        auto pname = name.dirName();
        logf(DBG, "mask={:x}, focus on parent: {}\n", ine->mask, pname);
        name = pname;
      }

      if (ine->mask & (IN_CREATE | IN_DELETE)) {
        pending_flags.set(W_PENDING_RECURSIVE);
      }

      logf(
          DBG,
          "add_pending for inotify mask={:x} {}\n",
          ine->mask,
          name.c_str());
      coll.add(name, now, pending_flags);

      if (ine->mask & (IN_CREATE | IN_DELETE)) {
        // When a directory's child is created or unlinked, inotify does not
        // tell us its parent has also changed. It should be rescanned, so
        // synthesize an event for the IO thread here.
        coll.add(name.dirName(), now, W_PENDING_VIA_NOTIFY);
      }

      // The kernel removed the wd -> name mapping, so let's update
      // our state here also
      if (ine->mask & IN_IGNORED) {
        logf(
            DBG,
            "mask={:x}: remove watch {} {}\n",
            ine->mask,
            ine->wd,
            dir_name.value());
        auto wlock = maps.wlock();
        wlock->wd_to_name.erase(ine->wd);
      }

    } else if ((ine->mask & (IN_MOVE_SELF | IN_IGNORED)) == 0) {
      // If we can't resolve the dir, and this isn't notification
      // that it has gone away, then we want to recrawl to fix
      // up our state.
      logf(
          ERR,
          "wanted dir {} for mask {:x} but not found {}\n",
          ine->wd,
          ine->mask,
          ine->name);
      root->scheduleRecrawl("dir missing from internal state");
    }
  }
  return false;
}

Watcher::ConsumeNotifyRet InotifyWatcher::consumeNotify(
    const std::shared_ptr<Root>& root,
    PendingChanges& coll) {
  int n = read(infd.fd(), &ibuf, sizeof(ibuf));
  if (n == -1) {
    if (errno == EINTR) {
      return {false};
    }
    logf(
        FATAL,
        "read({}, {}): error {}\n",
        infd.fd(),
        sizeof(ibuf),
        folly::errnoStr(errno));
  }

  logf(DBG, "inotify read: returned {}.\n", n);
  auto now = std::chrono::system_clock::now();

  struct inotify_event* ine;
  bool cancel = false;
  size_t eventsSeen = 0;
  for (char* iptr = ibuf; iptr < ibuf + n; iptr += sizeof(*ine) + ine->len) {
    ine = (struct inotify_event*)iptr;

    cancel |= process_inotify_event(root, coll, ine, now);
    ++eventsSeen;
  }

  // Relaxed because we don't really care exactly when the value is visible.
  totalEventsSeen_.fetch_add(eventsSeen, std::memory_order_relaxed);

  // It is possible that we can accumulate a set of pending_move
  // structs in move_map.  This happens when a directory is moved
  // outside of the watched tree; we get the MOVE_FROM but never
  // get the MOVE_TO with the same cookie.  To avoid leaking these,
  // we'll age out the move_map after processing a full set of
  // inotify events.   We age out rather than delete all because
  // the MOVE_TO may yet be waiting to read in another go around.
  // We allow a somewhat arbitrary but practical grace period to
  // observe the corresponding MOVE_TO.
  {
    auto wlock = maps.wlock();
    auto it = wlock->move_map.begin();
    while (it != wlock->move_map.end()) {
      auto& pending = it->second;
      if (now - pending.created > std::chrono::seconds{5}) {
        logf(
            DBG,
            "deleting pending move {} (moved outside of watch?)\n",
            pending.name);
        it = wlock->move_map.erase(it);
      } else {
        ++it;
      }
    }
  }

  return {cancel};
}

Watcher::WaitNotifyResult InotifyWatcher::waitNotify(int timeoutms) {
  struct pollfd pfd[2];
  pfd[0].fd = infd.fd();
  pfd[0].events = POLLIN;
  pfd[1].fd = terminatePipe_.read.fd();
  pfd[1].events = POLLIN;

  int n = poll(pfd, std::size(pfd), timeoutms);

  if (n > 0) {
    if (pfd[1].revents) {
      // We were signalled via stopThreads
      return WaitNotifyResult::Terminate;
    }
    if (pfd[0].revents) {
      return WaitNotifyResult::Ready;
    }
  }
  return WaitNotifyResult::Timeout;
}

void InotifyWatcher::stopThreads() {
  ignore_result(write(terminatePipe_.write.fd(), "X", 1));
}

json_ref InotifyWatcher::getDebugInfo() {
  json_ref events = json_null();
  if (ringBuffer_) {
    std::vector<json_ref> arr;
    for (auto& entry : ringBuffer_->readAll()) {
      arr.push_back(entry.asJsonValue());
    }
    events = json_array(std::move(arr));
  }
  return json_object({
      {"events", events},
      {"total_event_count", json_integer(totalEventsSeen_.load())},
  });
}

void InotifyWatcher::clearDebugInfo() {
  // This is just debug info so small races are not problematic. To avoid races,
  // totalEventsSeen_ could be stored directly if ringBuffer_ is null, or as the
  // difference between currentHead() - lastClear_ if not null.
  totalEventsSeen_.store(0, std::memory_order_release);
  if (ringBuffer_) {
    ringBuffer_->clear();
  }
}

namespace {
std::shared_ptr<QueryableView> detectInotify(
    const w_string& root_path,
    const w_string& fstype,
    const Configuration& config) {
  if (facebook::eden::is_edenfs_fs_type(fstype.string())) {
    // inotify is effectively O(repo) and we know that that access
    // pattern is undesirable when running on top of EdenFS
    throw std::runtime_error("cannot watch EdenFS file systems with inotify");
  }
  return std::make_shared<InMemoryView>(
      realFileSystem,
      root_path,
      config,
      std::make_shared<InotifyWatcher>(config));
}
} // namespace

static WatcherRegistry reg("inotify", detectInotify);

#endif // HAVE_INOTIFY_INIT

/* vim:ts=2:sw=2:et:
 */
