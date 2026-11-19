/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fmt/chrono.h>
#include <chrono>

#include "watchman/Errors.h"
#include "watchman/InMemoryView.h"
#include "watchman/PerfSample.h"
#include "watchman/fs/ParallelWalk.h"
#include "watchman/root/Root.h"
#include "watchman/root/warnerr.h"
#include "watchman/telemetry/LogEvent.h"
#include "watchman/telemetry/WatchmanStructuredLogger.h"
#include "watchman/watcher/Watcher.h"
#include "watchman/watchman_dir.h"
#include "watchman/watchman_file.h"

namespace watchman {

folly::SemiFuture<folly::Unit> InMemoryView::waitUntilReadyToQuery() {
  auto [p, f] = folly::makePromiseContract<folly::Unit>();
  auto pending = pendingFromWatcher_.lock();
  pending->addSync(std::move(p));
  pending->ping();
  return std::move(f);
}

void InMemoryView::fullCrawl(
    const std::shared_ptr<Root>& root,
    PendingCollection& pendingFromWatcher,
    PendingChanges& localPending) {
  root->recrawlInfo.wlock()->crawlStart = std::chrono::steady_clock::now();

  PerfSample sample("full-crawl");

  auto view = view_.wlock();
  // Ensure that we observe these files with a new, distinct clock,
  // otherwise a fresh subscription established immediately after a watch
  // can get stuck with an empty view until another change is observed
  mostRecentTick_.fetch_add(1, std::memory_order_acq_rel);

  fullCrawlStatCount_ = std::make_shared<std::atomic<size_t>>(0);
  root->recrawlInfo.wlock()->statCount = fullCrawlStatCount_;

  // A stored root inode of 0 lets the crawl accept a new directory at the
  // root path.
  view->setRootInode(0);

  auto start = std::chrono::system_clock::now();
  pendingFromWatcher.lock()->add(root->root_path, start, W_PENDING_RECURSIVE);
  while (true) {
    // There is the potential for a subtle race condition here.  Since we now
    // coalesce overlaps we must consume our outstanding set before we merge
    // in any new kernel notification information or we risk missing out on
    // observing changes that happen during the initial crawl.  This
    // translates to a two level loop; the outer loop sweeps in data from
    // inotify, then the inner loop processes it and any dirs that we pick up
    // from recursive processing.
    {
      auto lock = pendingFromWatcher.lock();
      localPending.append(lock->stealItems(), lock->stealSyncs());
    }
    if (localPending.empty()) {
      break;
    }

    (void)processAllPending(root, *view, localPending);
  }

  auto recrawlInfo = root->recrawlInfo.wlock();
  recrawlInfo->shouldRecrawl = false;
  recrawlInfo->crawlFinish = std::chrono::steady_clock::now();
  recrawlInfo->statCount = nullptr;
  fullCrawlStatCount_ = nullptr;
  root->inner.done_initial.store(true, std::memory_order_release);

  // There is no need to hold locks while logging, and abortAllCookies resolves
  // a Promise which can run arbitrary code, so locks must be released here.
  auto recrawlCount = recrawlInfo->recrawlCount;
  recrawlInfo.unlock();
  view.unlock();

  root->cookies.abortAllCookies();

  auto root_metadata = root->getRootMetadata();
  sample.add_root_metadata(root_metadata);
  sample.finish();
  sample.force_log();
  sample.log();

  FullCrawl fullCrawl;
  fullCrawl.root = root_metadata.root_path.string();
  fullCrawl.recrawl = root_metadata.recrawl_count;
  fullCrawl.case_sensitive = root_metadata.case_sensitive;
  fullCrawl.watcher = root_metadata.watcher.string();
  getLogger()->logEvent(fullCrawl);

  logf(ERR, "{}crawl complete\n", recrawlCount ? "re" : "");
}

InMemoryView::Continue InMemoryView::doSettleThings(
    Root& root,
    IoThreadState& state) {
  // No new pending items were given to us, so consider that
  // we may now be settled.

  std::chrono::milliseconds sinceUnsettle = state.lastUnsettle
      ? std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - *state.lastUnsettle)
      : std::chrono::milliseconds{0};

  warmContentCache();

  root.unilateralResponses->enqueue(json_object({{"settled", json_true()}}));

  if (root.considerReap()) {
    root.stopWatch("Watch was idle for too long");
    return Continue::Stop;
  }

  std::optional<std::chrono::milliseconds> nextPendingSettle;

  {
    auto pendingSettles = pendingSettles_.wlock();
    auto iter = pendingSettles->begin();
    while (iter != pendingSettles->end() && iter->first <= sinceUnsettle) {
      auto node = pendingSettles->extract(iter++);
      node.mapped().setValue();
    }

    if (iter != pendingSettles->end()) {
      nextPendingSettle = iter->first;
    }
  }

  if (nextPendingSettle) {
    // There is another settle pending, so only wait until then.
    state.currentTimeout = *nextPendingSettle - sinceUnsettle;
  } else {
    state.currentTimeout =
        std::min(state.biggestTimeout, state.currentTimeout * 2);
  }

  root.considerAgeOut();
  return Continue::Continue;
}

void InMemoryView::clientModeCrawl(const std::shared_ptr<Root>& root) {
  PendingChanges pending;
  fullCrawl(root, pendingFromWatcher_, pending);
}

namespace {

std::chrono::milliseconds getBiggestTimeout(const Root& root) {
  std::chrono::milliseconds biggest_timeout = root.gc_interval;

  if (biggest_timeout.count() == 0 ||
      (root.idle_reap_age.count() != 0 &&
       root.idle_reap_age < biggest_timeout)) {
    biggest_timeout = root.idle_reap_age;
  }
  if (biggest_timeout.count() == 0) {
    biggest_timeout = std::chrono::hours(24);
  }
  return biggest_timeout;
}

} // namespace

void InMemoryView::ioThread(const std::shared_ptr<Root>& root) {
  IoThreadState state{getBiggestTimeout(*root)};
  state.currentTimeout = root->trigger_settle;
  // Injects a temporary blocks, only in test code. This is to
  // force the iothread to loose a race with the notify thread.
  // TODO: Support something like EdenFS FaultInjector so that we can do
  // something less hacky.
  if (config_.getBool("inject_block_in_io_thread_start", false)) {
    sleep(10);
  }
  while (Continue::Continue == stepIoThread(root, state, pendingFromWatcher_)) {
  }
}

InMemoryView::Continue InMemoryView::stepIoThread(
    const std::shared_ptr<Root>& root,
    IoThreadState& state,
    PendingCollection& pendingFromWatcher) {
  if (stopThreads_.load(std::memory_order_acquire)) {
    return Continue::Stop;
  }

  auto markUnsettled = [&](IoThreadState& state) {
    state.lastUnsettle = std::chrono::steady_clock::now();
    // Reduce sleep timeout to the settle duration ready for the next loop
    // through.
    state.currentTimeout = root->trigger_settle;
  };

  if (!root->inner.done_initial.load(std::memory_order_acquire)) {
    /* first order of business is to find all the files under our root */
    fullCrawl(root, pendingFromWatcher, state.localPending);

    markUnsettled(state);
    return Continue::Continue;
  }

  // Wait for the notify thread to give us pending items, or for
  // the settle period to expire
  {
    logf(DBG, "poll_events timeout={}ms\n", state.currentTimeout);
    auto targetPendingLock =
        pendingFromWatcher.lockAndWait(state.currentTimeout);
    logf(DBG, " ... wake up\n");
    state.localPending.append(
        targetPendingLock->stealItems(), targetPendingLock->stealSyncs());
  }

  if (root->inner.cancelled.load(std::memory_order_acquire)) {
    // The root was cancelled. Root::cancel will call stopThreads() soon, so
    // just exit early.
    return Continue::Stop;
  }

  // Has a Watcher indicated this root needs a recrawl?
  // TODO: scheduleRecrawl should be replaced with a regular event published in
  // the PendingCollection.
  if (root->recrawlInfo.rlock()->shouldRecrawl) {
    auto info = root->recrawlInfo.wlock();
    info->recrawlCount++;
    root->inner.done_initial.store(false, std::memory_order_release);
    // Now that done_initial is false, the next pass will recrawl.
    return Continue::Continue;
  }

  // fullCrawl unconditionally sets done_initial to true and if
  // handleShouldRecrawl set it false, execution wouldn't reach this part of
  // the loop.
  w_check(
      root->inner.done_initial.load(std::memory_order_acquire),
      "A full crawl should not be pending at this point in the loop.");

  // Waiting for an event timed out or we were woken with a ping, so still
  // consider the root settled.
  if (state.localPending.empty()) {
    return doSettleThings(*root, state);
  }

  // Otherwise we have pending items to stat and crawl

  // Some Linux kernels between 5.3 and 5.6 will report inotify events before
  // the file has been evicted from the cache, causing Watchman to incorrectly
  // think the file is still on disk after it's unlinked. If configured, allow
  // a brief sleep to mitigate.
  //
  // Careful with this knob: it adds latency to every query by delaying cookie
  // processing.
  auto notify_sleep_ms = config_.getInt("notify_sleep_ms", 0);
  if (notify_sleep_ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(notify_sleep_ms));
  }

  auto view = view_.wlock();

  mostRecentTick_.fetch_add(1, std::memory_order_acq_rel);

  auto isDesynced = processAllPending(root, *view, state.localPending);
  if (isDesynced == IsDesynced::Yes) {
    logf(ERR, "recrawl complete, aborting all pending cookies\n");
    root->cookies.abortAllCookies();
  }

  // Always mark unsettled after processing events because settle durations
  // should only include idle time, not time spent processing events.
  markUnsettled(state);
  return Continue::Continue;
}

InMemoryView::IsDesynced InMemoryView::processAllPending(
    const std::shared_ptr<Root>& root,
    ViewDatabase& view,
    PendingChanges& coll) {
  auto desyncState = IsDesynced::No;

  // Don't resolve any of these until any recursive crawls are done.
  std::vector<std::vector<folly::Promise<folly::Unit>>> allSyncs;

  // Don't resolve cookies -- that is, unblock queries -- until all
  // pending paths are processed.  There is no inherent order in
  // events from the watcher. They might trigger recrawls or require
  // subsequent enumeration and discovery. So we cannot unblock any
  // pending queries until we're sure the internal view is caught up
  // to all pending change events.
  std::vector<w_string> pendingCookies;

  while (!coll.empty()) {
    logf(
        DBG,
        "processing {} events in {}\n",
        coll.getPendingItemCount(),
        rootPath_);

    auto pending = coll.stealItems();
    auto syncs = coll.stealSyncs();
    if (syncs.empty()) {
      w_check(
          pending != nullptr,
          "coll.stealItems() and coll.size() did not agree about its size");
    } else {
      allSyncs.push_back(std::move(syncs));
    }

    while (pending) {
      if (!stopThreads_.load(std::memory_order_acquire)) {
        if (pending->flags & W_PENDING_IS_DESYNCED) {
          // The watcher is desynced but some cookies might be written to disk
          // while the recursive crawl is ongoing. We are going to specifically
          // ignore these cookies during that recursive crawl to avoid a race
          // condition where cookies might be seen before some files have been
          // observed as changed on disk. Due to this, and the fact that cookies
          // notifications might simply have been dropped by the watcher, we
          // need to abort the pending cookies to force them to be recreated on
          // disk, and thus re-seen.
          if (pending->flags & W_PENDING_CRAWL_ONLY) {
            desyncState = IsDesynced::Yes;
          }
        }

        // processPath may insert new pending items into `coll`
        processPath(root, view, coll, *pending, nullptr, pendingCookies);
      }

      // TODO: Document that continuing to run this loop when stopThreads_ is
      // true fixes a stack overflow when pending is long.
      pending = std::move(pending->next);
    }
  }

  for (auto& pendingCookie : pendingCookies) {
    if (processedPaths_) {
      // Record a fake entry to indicate when we unblocked the cookie in the
      // processed paths log.
      processedPaths_->write(
          PendingChangeLogEntry{
              PendingChange{
                  pendingCookie,
                  std::chrono::system_clock::now(),
                  W_PENDING_CRAWL_ONLY},
              // This error code is unlikely to ever collide with reality.
              make_error_code(error_code::too_many_symbolic_link_levels),
              FileInformation{}});
    }
    root->cookies.notifyCookie(pendingCookie);
  }

  for (auto& outer : allSyncs) {
    for (auto& sync : outer) {
      sync.setValue();
    }
  }

  return desyncState;
}

void InMemoryView::processPath(
    const std::shared_ptr<Root>& root,
    ViewDatabase& view,
    PendingChanges& coll,
    const PendingChange& pending,
    const FileInformation* pre_stat,
    std::vector<w_string>& pendingCookies) {
  w_check(
      pending.path.size() >= rootPath_.size(),
      "full_path must be a descendant of the root directory\n",
      "rootPath_:    ",
      rootPath_,
      "\n",
      "pending.path: ",
      pending.path,
      "\n");

  /* From a particular query's point of view, there are four sorts of cookies we
   * can observe:
   * 1. Cookies that this query has created. This marks the end of this query's
   *    sync_to_now, so we hide it from the results.
   * 2. Cookies that another query on the same watch by the same process has
   *    created. This marks the end of that other query's sync_to_now, so from
   *    the point of view of this query we turn a blind eye to it.
   * 3. Cookies created by another process on the same watch. We're independent
   *    of other processes, so we report these.
   * 4. Cookies created by a nested watch by the same or a different process.
   *    We're independent of other watches, so we report these.
   *
   * The below condition is true for cases 1 and 2 and false for 3 and 4.
   */
  if (root->cookies.isCookiePrefix(pending.path)) {
    bool consider_cookie;
    if (watcher_->flags & WATCHER_HAS_PER_FILE_NOTIFICATIONS) {
      // The watcher gives us file level notification, thus only consider
      // cookies if this path is coming directly from the watcher, not from a
      // recursive crawl.
      consider_cookie = (pending.flags & W_PENDING_VIA_NOTIFY) ||
          !root->inner.done_initial.load(std::memory_order_acquire);
    } else {
      // If we are de-synced, we shouldn't consider cookies as we are currently
      // walking directories recursively and we need to wait for after the
      // directory is fully re-crawled before notifying the cookie. At the end
      // of the crawl, cookies will be cancelled and re-created.
      consider_cookie =
          (pending.flags & W_PENDING_IS_DESYNCED) != W_PENDING_IS_DESYNCED;
    }

    if (consider_cookie) {
      pendingCookies.push_back(pending.path);
    }

    // Never allow cookie files to show up in the tree
    return;
  }

  if (pending.path == rootPath_ || (pending.flags & W_PENDING_CRAWL_ONLY)) {
    crawler(root, view, coll, pending, pendingCookies);
  } else {
    statPath(*root, root->cookies, view, coll, pending, pre_stat);
  }
}

namespace {

void apply_dir_size_hint(watchman_dir* dir, uint32_t ndirs, uint32_t nfiles) {
  if (dir->files.empty() && nfiles > 0) {
    dir->files.reserve(nfiles);
  }
  if (dir->dirs.empty() && ndirs > 0) {
    dir->dirs.reserve(ndirs);
  }
}

} // namespace

void InMemoryView::crawler(
    const std::shared_ptr<Root>& root,
    ViewDatabase& view,
    PendingChanges& coll,
    const PendingChange& pending,
    std::vector<w_string>& pendingCookies) {
  bool recursive = pending.flags.contains(W_PENDING_RECURSIVE);
  bool stat_all = pending.flags.contains(W_PENDING_NONRECURSIVE_SCAN);

  auto dir = view.resolveDir(pending.path, true);

  // Detect root directory replacement.
  // The inode number check is handled more generally by the sister code
  // in stat.cpp.  We need to special case it for the root because we never
  // generate a watchman_file node for the root and thus never call
  // InMemoryView::statPath (we'll fault if we do!).
  // Ideally the kernel would have given us a signal when we've been replaced
  // but some filesystems (eg: BTRFS) do not emit appropriate inotify events
  // for things like subvolume deletes.  We've seen situations where the
  // root has been replaced and we got no notifications at all and this has
  // left the cookie sync mechanism broken forever.
  if (pending.path == root->root_path) {
    try {
      auto st = fileSystem_.getFileInformation(
          pending.path.c_str(), root->case_sensitive);
      if (st.ino != view.getRootInode()) {
        // If it still exists and the inode doesn't match, then we need
        // to force recrawl to make sure we're in sync.
        // We're lazily initializing the rootInode to 0 here, so we don't
        // need to do this the first time through (we're already crawling
        // everything in that case).
        if (view.getRootInode() != 0) {
          root->scheduleRecrawl(
              "root was replaced and we didn't get notified by the kernel");
          return;
        }
        recursive = true;
        view.setRootInode(st.ino);
      }
    } catch (const std::system_error& err) {
      handle_open_errno(
          *root,
          dir->getFullPath(),
          pending.now,
          "getFileInformation",
          err.code());
      view.markDirDeleted(dir, getClock(pending.now), true);
      return;
    }
  }

  if (recursive &&
      root->enable_parallel_crawl.load(std::memory_order_acquire)) {
    return crawlerParallel(root, view, coll, pending, pendingCookies);
  }

  auto& path = pending.path;

  logf(
      DBG, "opendir({}) recursive={} stat_all={}\n", path, recursive, stat_all);

  /* Start watching and open the dir for crawling.
   * Whether we open the dir prior to watching or after is watcher specific,
   * so the operations are rolled together in our abstraction */
  std::unique_ptr<DirHandle> osdir;

  try {
    osdir = watcher_->startWatchDir(root, path.c_str());
  } catch (const std::system_error& err) {
    logf(DBG, "startWatchDir({}) threw {}\n", path, err.what());
    handle_open_errno(
        *root, dir->getFullPath(), pending.now, "opendir", err.code());
    view.markDirDeleted(dir, getClock(pending.now), true);
    return;
  }

  if (dir->files.empty()) {
    // Pre-size our hash(es) if we can, so that we can avoid collisions
    // and re-hashing during initial crawl
    uint32_t num_dirs = 0;
#ifndef _WIN32
    struct stat st;
    int dfd = osdir->getFd();
    if (dfd != -1 && fstat(dfd, &st) == 0) {
      num_dirs = (uint32_t)st.st_nlink;
    }
#endif
    // st.st_nlink is usually number of dirs + 2 (., ..).
    // If it is less than 2 then it doesn't follow that convention.
    // We just pass it through for the dir size hint and the hash
    // table implementation will round that up to the next power of 2
    apply_dir_size_hint(
        dir,
        num_dirs,
        uint32_t(root->config.getInt("hint_num_files_per_dir", 64)));
  }

  /* flag for delete detection */
  for (auto& it : dir->files) {
    auto file = it.second.get();
    if (file->exists) {
      file->maybe_deleted = true;
    }
  }

  try {
    while (const DirEntry* dirent = osdir->readDir()) {
      // Don't follow parent/self links
      if (dirent->d_name[0] == '.' &&
          (!strcmp(dirent->d_name, ".") || !strcmp(dirent->d_name, ".."))) {
        continue;
      }

      // Queue it up for analysis if the file is newly existing
      w_string name(dirent->d_name, W_STRING_BYTE);
      struct watchman_file* file = dir->getChildFile(name);
      if (file) {
        file->maybe_deleted = false;
      }
      if (!file || !file->exists || stat_all || recursive) {
        auto full_path = dir->getFullPathToChild(name);

        PendingFlags newFlags;
        if (recursive || !file || !file->exists) {
          newFlags.set(W_PENDING_RECURSIVE);
        }
        if (pending.flags & W_PENDING_IS_DESYNCED) {
          newFlags.set(W_PENDING_IS_DESYNCED);
        }

        logf(
            DBG,
            "in crawler calling processPath on {} oldflags={} newflags={}\n",
            full_path,
            pending.flags.asRaw(),
            newFlags.asRaw());

        PendingChange full_pending{std::move(full_path), pending.now, newFlags};
        processPath(
            root,
            view,
            coll,
            full_pending,
            dirent->has_stat ? &dirent->stat : nullptr,
            pendingCookies);
      }
    }
  } catch (const std::system_error& exc) {
    log(ERR,
        "Error while reading dir ",
        path,
        ": ",
        exc.what(),
        ", re-adding to pending list to re-assess\n");
    coll.add(path, pending.now, {});
  }
  osdir.reset();

  // Anything still in maybe_deleted is actually deleted.
  // Arrange to re-process it shortly
  for (auto& it : dir->files) {
    auto file = it.second.get();
    if (file->exists &&
        (file->maybe_deleted || (file->stat.isDir() && recursive))) {
      coll.add(
          dir,
          file->getName().data(),
          pending.now,
          recursive ? W_PENDING_RECURSIVE : PendingFlags{});
    }
  }
}

namespace {

// Handle ignore and startWatchDir on openDir.
class CrawlerFileSystem : public FileSystem {
 public:
  std::unique_ptr<DirHandle> openDir(const char* path, bool strict) override {
    // startWatchDir implementations use default openDir, which has
    // strict=true.
    w_assert(strict, "CrawlerFileSystem::openDir requires strict=true");
    (void)strict;
    // Match ignore handling in statPath().
    w_string fullPath{path};
    if (root_->ignore.isIgnoreDir(fullPath)) {
      return nullptr;
    }
    if ((root_->root_path != fullPath &&
         root_->ignore.isIgnoreVCS(fullPath.dirName())) &&
        !root_->cookies.isCookieDir(fullPath)) {
      return nullptr;
    }
    // Use watcher->startWatchDir to ensure side effects are applied
    // in the right order (ex. inotify_add_watch before opendir).
    // This requires startWatchDir to be thread-safe.
    return watcher_->startWatchDir(root_, path);
  }

  FileInformation getFileInformation(
      const char* path,
      CaseSensitivity caseSensitive) override {
    return fileSystem_.getFileInformation(path, caseSensitive);
  }

  void touch(const char* path) override {
    fileSystem_.touch(path);
  }

  CrawlerFileSystem(
      FileSystem& fileSystem,
      std::shared_ptr<Root> root,
      std::shared_ptr<Watcher> watcher)
      : fileSystem_{fileSystem},
        root_{std::move(root)},
        watcher_{std::move(watcher)} {}

  CrawlerFileSystem() = delete;
  CrawlerFileSystem(CrawlerFileSystem&&) = delete;
  CrawlerFileSystem(const CrawlerFileSystem&) = delete;
  CrawlerFileSystem& operator=(CrawlerFileSystem&&) = delete;
  CrawlerFileSystem& operator=(const CrawlerFileSystem&) = delete;

 private:
  FileSystem& fileSystem_;
  std::shared_ptr<Root> root_;
  std::shared_ptr<Watcher> watcher_;
};

} // namespace

void InMemoryView::crawlerParallel(
    const std::shared_ptr<Root>& root,
    ViewDatabase& view,
    PendingChanges& coll,
    const PendingChange& pending,
    std::vector<w_string>& pendingCookies) {
  w_assert(
      pending.flags.contains(W_PENDING_RECURSIVE),
      "crawlerParallel requires W_PENDING_RECURSIVE");

  // Flags to pass down to statPath().
  const PendingFlags inheritFlags =
      (pending.flags & W_PENDING_IS_DESYNCED) | W_PENDING_VIA_PWALK;

  AbsolutePath path{pending.path.c_str()};
  logf(DBG, "crawlerParallel({})\n", path);

  // Use ParallelWalker to get directory entries and stats recursively.
  // Unlike crawler(), do not call the crawler function recursively
  // (via W_PENDING_RECURSIVE), and avoid extra syscalls.

  std::shared_ptr<CrawlerFileSystem> fs =
      std::make_shared<CrawlerFileSystem>(fileSystem_, root, watcher_);
  size_t threadCountHint = config_.getInt("parallel_crawl_thread_count", 0);
  ParallelWalker walker{
      std::move(fs),
      path,
      root->allow_crawling_other_mounts ? std::nullopt
                                        : std::optional{root->stat},
      threadCountHint};

  // Step 1: Process readDir results.
  while (true) {
    auto result = walker.nextResult();
    if (!result.has_value()) {
      break;
    }
    ReadDirResult& dirResult = result.value();

    // Step 1a: Prepare the dirView.
    w_string dirPath{dirResult.dirFullPath.c_str()};
    auto dirView = view.resolveDir(dirPath, true);
    if (dirView->files.empty()) {
      dirView->files.reserve(dirResult.entries.size());
      dirView->dirs.reserve(dirResult.subdirCount);
    }
    for (auto& it : dirView->files) {
      auto fileView = it.second.get();
      if (fileView->exists) {
        fileView->maybe_deleted = true;
      }
    }

    // Step 1b: Update files in the dirView via statPath().
    // Prepare the stat so statPath can avoid syscall.
    for (auto& entry : dirResult.entries) {
      w_string name{entry.name.c_str(), W_STRING_BYTE};
      watchman_file* fileView = dirView->getChildFile(name);
      if (fileView) {
        fileView->maybe_deleted = false;
      }
      auto fullPath = dirView->getFullPathToChild(name);
      processPath(
          root,
          view,
          coll,
          PendingChange{
              std::move(fullPath),
              pending.now,
              inheritFlags,
          },
          &entry.stat,
          pendingCookies);
    }

    // Step 1c: Mark for deletion.
    for (auto& it : dirView->files) {
      auto fileView = it.second.get();
      if (fileView->exists && fileView->maybe_deleted) {
        auto fullPath = dirView->getFullPathToChild(fileView->getName());
        processPath(
            root,
            view,
            coll,
            PendingChange{
                std::move(fullPath),
                pending.now,
                inheritFlags,
            },
            nullptr,
            pendingCookies);
      }
    }
  }

  // Step 2: Handle errors.
  while (true) {
    auto maybe_error = walker.nextError();
    if (!maybe_error) {
      break;
    }
    auto error = maybe_error.value();
    const std::system_error* exc =
        error.error.get_exception<std::system_error>();
    if (exc) {
      auto code = exc->code();
      if (code == error_code::no_such_file_or_directory ||
          code == error_code::not_a_directory) {
        // Handled by step 1c.
      } else {
        // Report error. Affect query response.
        handle_open_errno(
            *root, error.fullPath, pending.now, error.operationName, code);
      }
    }
  }
}

namespace {
bool did_file_change(
    const FileInformation* saved,
    const FileInformation* fresh) {
  /* we have to compare this way because the stat structure
   * may contain fields that vary and that don't impact our
   * understanding of the file */

#define FIELD_CHG(name)             \
  if (saved->name != fresh->name) { \
    return true;                    \
  }

  // Can't compare with memcmp due to padding and garbage in the struct
  // on OpenBSD, which has a 32-bit tv_sec + 64-bit tv_nsec
#define TIMESPEC_FIELD_CHG(wat)                           \
  {                                                       \
    struct timespec a = saved->wat##time;                 \
    struct timespec b = fresh->wat##time;                 \
    if (a.tv_sec != b.tv_sec || a.tv_nsec != b.tv_nsec) { \
      return true;                                        \
    }                                                     \
  }

  FIELD_CHG(mode);

  if (!saved->isDir()) {
    FIELD_CHG(size);
    FIELD_CHG(nlink);
  }
  FIELD_CHG(dev);
  FIELD_CHG(ino);
  FIELD_CHG(uid);
  FIELD_CHG(gid);
  // Don't care about st_blocks
  // Don't care about st_blksize
  // Don't care about st_atimespec
  TIMESPEC_FIELD_CHG(m);
  TIMESPEC_FIELD_CHG(c);

  return false;
}
} // namespace

void InMemoryView::statPath(
    const Root& root,
    const CookieSync& cookies,
    ViewDatabase& view,
    PendingChanges& coll,
    const PendingChange& pending,
    const FileInformation* pre_stat) {
  bool recursive = pending.flags.contains(W_PENDING_RECURSIVE);
  const bool via_notify = pending.flags.contains(W_PENDING_VIA_NOTIFY);
  const PendingFlags desynced_flag = pending.flags & W_PENDING_IS_DESYNCED;

  // viaPwalk is true iff calling from crawlerParallel.
  bool viaPwalk = pending.flags.contains(W_PENDING_VIA_PWALK);

  if (root.ignore.isIgnoreDir(pending.path)) {
    logf(DBG, "{} matches ignore_dir rules\n", pending.path);
    return;
  }

  auto& path = pending.path;
  w_check(!path.empty(), "must have path");
  auto dir_name = pending.path.dirName();
  auto file_name = pending.path.baseName();
  w_check(!dir_name.empty(), "must have dir_name");
  auto parentDir = view.resolveDir(dir_name, true);

  auto file = parentDir->getChildFile(file_name);

  auto dir_ent = parentDir->getChildDir(file_name);

  FileInformation st;
  std::error_code errcode;
  if (pre_stat) {
    st = *pre_stat;
  } else if (viaPwalk) {
    // crawlerParallel always provides "pre_stat". If it doesn't, it means the
    // file is missing (see "Step 1c" in crawlerParallel). Treat as deleted
    // without an extra getFileInformation() call.
    errcode = make_error_code(error_code::no_such_file_or_directory);
  } else {
    try {
      st = fileSystem_.getFileInformation(path.c_str(), root.case_sensitive);
      log(DBG,
          "getFileInformation(",
          path,
          ") file=",
          fmt::ptr(file),
          " dir=",
          fmt::ptr(dir_ent),
          "\n");
    } catch (const std::system_error& exc) {
      errcode = exc.code();
      log(DBG,
          "getFileInformation(",
          path,
          ") file=",
          fmt::ptr(file),
          " dir=",
          fmt::ptr(dir_ent),
          " failed: ",
          exc.what(),
          "\n");
    }
  }

  if (processedPaths_) {
    processedPaths_->write(PendingChangeLogEntry{pending, errcode, st});
  }
  if (fullCrawlStatCount_) {
    // Not using loaded value - load can be relaxed - no need for acq_rel
    fullCrawlStatCount_->fetch_add(1, std::memory_order_release);
  }

  if (errcode == error_code::no_such_file_or_directory ||
      errcode == error_code::not_a_directory) {
    /* it's not there, update our state */
    if (dir_ent) {
      view.markDirDeleted(dir_ent, getClock(pending.now), true);
      log(DBG,
          "getFileInformation(",
          path,
          ") -> ",
          errcode.message(),
          " so stopping watch\n");
    }
    if (file) {
      if (file->exists) {
        log(DBG,
            "getFileInformation(",
            path,
            ") -> ",
            errcode.message(),
            " so marking ",
            file->getName(),
            " deleted\n");
        file->exists = false;
        view.markFileChanged(file, getClock(pending.now));
      }
    } else {
      // It was created and removed before we could ever observe it
      // in the filesystem.  We need to generate a deleted file
      // representation of it now, so that subscription clients can
      // be notified of this event
      file = view.getOrCreateChildFile(
          parentDir, file_name, getClock(pending.now));
      log(DBG,
          "getFileInformation(",
          path,
          ") -> ",
          errcode.message(),
          " and file node was NULL. "
          "Generating a deleted node.\n");
      file->exists = false;
      view.markFileChanged(file, getClock(pending.now));
    }

    if (!viaPwalk &&
        (root.case_sensitive == CaseSensitivity::CaseInSensitive &&
         dir_name != root.root_path && parentDir->last_check_existed)) {
      /* If we rejected the name because it wasn't canonical,
       * we need to ensure that we look in the parent dir to discover
       * the new item(s)
       *
       * Unless viaPwalk, in which case the siblings (items in parent dir)
       * will be visited by crawlerParallel. */
      logf(
          DBG,
          "we're case insensitive, and {} is ENOENT, "
          "speculatively look at parent dir {}\n",
          path,
          dir_name);
      coll.add(dir_name, pending.now, W_PENDING_CRAWL_ONLY);
    }

  } else if (errcode.value()) {
    log(ERR,
        "getFileInformation(",
        path,
        ") failed and not handled! -> ",
        errcode.message(),
        " value=",
        errcode.value(),
        " category=",
        errcode.category().name(),
        "\n");
  } else {
    if (!file) {
      file = view.getOrCreateChildFile(
          parentDir, file_name, getClock(pending.now));
    }

    if (!file->exists) {
      /* we're transitioning from deleted to existing,
       * so we're effectively new again */
      file->ctime.ticks = mostRecentTick_;
      file->ctime.timestamp = std::chrono::system_clock::to_time_t(pending.now);
      /* if a dir was deleted and now exists again, we want
       * to crawl it again */
      recursive = true;
    }
    if (!file->exists || via_notify || did_file_change(&file->stat, &st)) {
      logf(
          DBG,
          "file changed exists={} via_notify={} stat-changed={} isdir={} size={} {}\n",
          file->exists,
          via_notify,
          file->exists && !via_notify,
          st.isDir(),
          st.size,
          path);
      file->exists = true;
      view.markFileChanged(file, getClock(pending.now));

      // If the inode number changed then we definitely need to recursively
      // examine any children because we cannot assume that the kernel will
      // have given us the correct hints about this change.  BTRFS is one
      // example of a filesystem where this has been observed to happen.
      if (file->stat.ino != st.ino) {
        recursive = true;
      }
    }

    memcpy(&file->stat, &st, sizeof(file->stat));
    watcher_->startWatchFile(file);

    if (st.isDir()) {
      if (dir_ent == nullptr) {
        recursive = true;
      } else {
        // Ensure that we believe that this node exists
        dir_ent->last_check_existed = true;
      }

      if (!(root.allow_crawling_other_mounts ||
            isOnSameMount(root.stat, st, path.c_str()))) {
        logf(
            DBG,
            "Ignoring {} as it doesn't reside on the same mount point as the root directory\n",
            pending.path);
        return;
      }

      // Don't recurse if our parent is an ignore dir or via crawlerParallel
      // (already recursive)
      if (!viaPwalk &&
          (!root.ignore.isIgnoreVCS(dir_name) ||
           // but do if we're looking at the cookie dir (stat_path is never
           // called for the root itself)
           cookies.isCookieDir(pending.path))) {
        if (recursive) {
          /* we always need to crawl if we're recursive, this can happen when a
           * directory is created */
          coll.add(
              pending.path,
              pending.now,
              desynced_flag | W_PENDING_RECURSIVE | W_PENDING_CRAWL_ONLY);
        } else if (pending.flags & W_PENDING_NONRECURSIVE_SCAN) {
          /* on file changes, we receive a notification on the directory and
           * thus we just need to crawl this one directory to consider all
           * the pending files. */
          coll.add(
              pending.path,
              pending.now,
              desynced_flag | W_PENDING_NONRECURSIVE_SCAN |
                  W_PENDING_CRAWL_ONLY);
        } else {
          if (watcher_->flags & WATCHER_HAS_PER_FILE_NOTIFICATIONS) {
            /* we get told about changes on the child, so we don't need to do
             * anything */
          } else {
            /* in all the other cases, crawl */
            coll.add(
                pending.path,
                pending.now,
                desynced_flag | W_PENDING_CRAWL_ONLY);
          }
        }
      }
    } else if (dir_ent) {
      // We transitioned from dir to file (see fishy.php), so we should prune
      // our former tree here
      view.markDirDeleted(dir_ent, getClock(pending.now), true);
    }
  }
}

} // namespace watchman

/* vim:ts=2:sw=2:et:
 */
