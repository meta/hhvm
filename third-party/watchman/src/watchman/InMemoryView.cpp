/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/InMemoryView.h"
#include <fmt/core.h>
#include <folly/ScopeGuard.h>
#include <algorithm>
#include <chrono>
#include <memory>
#include <thread>
#include "watchman/Errors.h"
#include "watchman/ThreadPool.h"
#include "watchman/query/GlobTree.h"
#include "watchman/query/Query.h"
#include "watchman/query/QueryContext.h"
#include "watchman/query/eval.h"
#include "watchman/root/Root.h"
#include "watchman/thirdparty/wildmatch/wildmatch.h"
#include "watchman/watcher/Watcher.h"
#include "watchman/watchman_file.h"

// Each root gets a number that uniquely identifies it within the process. This
// helps avoid confusion if a root is removed and then added again.
static std::atomic<watchman::ClockRoot> next_root_number{1};

namespace watchman {

namespace {
/** Concatenate dir_name and name around a unix style directory
 * separator.
 * dir_name may be NULL in which case this returns a copy of name.
 */
inline std::string make_path_name(
    const char* dir_name,
    uint32_t dlen,
    const char* name,
    uint32_t nlen) {
  std::string result;
  result.reserve(dlen + nlen + 1);

  if (dlen) {
    result.append(dir_name, dlen);
    // wildmatch wants unix separators
    result.push_back('/');
  }
  result.append(name, nlen);
  return result;
}
} // namespace

InMemoryViewCaches::InMemoryViewCaches(
    const w_string& rootPath,
    size_t maxHashes,
    size_t maxSymlinks,
    std::chrono::milliseconds errorTTL)
    : contentHashCache(rootPath, maxHashes, errorTTL),
      symlinkTargetCache(rootPath, maxSymlinks, errorTTL) {}

InMemoryFileResult::InMemoryFileResult(
    const watchman_file* file,
    InMemoryViewCaches& caches)
    : file_(file), caches_(caches) {}

void InMemoryFileResult::batchFetchProperties(
    const std::vector<std::unique_ptr<FileResult>>& files) {
  std::vector<folly::Future<folly::Unit>> readlinkFutures;
  std::vector<folly::Future<folly::Unit>> sha1Futures;

  // Since we may initiate some async work in the body of the function
  // below, we need to ensure that we wait for it to complete before
  // we return from this scope, even if we are throwing an exception.
  // If we fail to do so, the continuation on the futures that we
  // schedule will access invalid memory and we'll all feel bad.
  SCOPE_EXIT {
    if (!readlinkFutures.empty()) {
      folly::collectAll(readlinkFutures.begin(), readlinkFutures.end()).wait();
    }
    if (!sha1Futures.empty()) {
      folly::collectAll(sha1Futures.begin(), sha1Futures.end()).wait();
    }
  };

  for (auto& f : files) {
    auto* file = dynamic_cast<InMemoryFileResult*>(f.get());

    if (file->neededProperties() & FileResult::Property::SymlinkTarget) {
      if (!file->file_->stat.isSymlink()) {
        // If this file is not a symlink then we immediately yield
        // a nullptr w_string instance rather than propagating an error.
        // This behavior is relied upon by the field rendering code and
        // checked in test_symlink.py.
        file->symlinkTarget_ = w_string();
      } else {
        auto dir = file->dirName();
        dir.advance(file->caches_.symlinkTargetCache.rootPath().size());

        // If dirName is the root, dir.size() will now be zero
        if (dir.size() > 0) {
          // if not at the root, skip the slash character at the
          // front of dir
          dir.advance(1);
        }

        SymlinkTargetCacheKey key{
            w_string::pathCat({dir, file->baseName()}), file->file_->otime};

        readlinkFutures.emplace_back(
            caches_.symlinkTargetCache.get(key).thenTry(
                [file](
                    folly::Try<std::shared_ptr<
                        const SymlinkTargetCache::Node>>&& result) {
                  if (result.hasValue()) {
                    file->symlinkTarget_ = result.value()->value();
                  } else {
                    // we don't have a way to report the error for readlink
                    // due to legacy requirements in the interface, so we
                    // just set it to empty.
                    file->symlinkTarget_ = w_string();
                  }
                }));
      }
    }

    if (file->neededProperties() & FileResult::Property::ContentSha1) {
      auto dir = file->dirName();
      dir.advance(file->caches_.contentHashCache.rootPath().size());

      // If dirName is the root, dir.size() will now be zero
      if (dir.size() > 0) {
        // if not at the root, skip the slash character at the
        // front of dir
        dir.advance(1);
      }

      ContentHashCacheKey key{
          w_string::pathCat({dir, file->baseName()}),
          size_t(file->file_->stat.size),
          file->file_->stat.mtime};

      sha1Futures.emplace_back(caches_.contentHashCache.get(key).thenTry(
          [file](
              folly::Try<std::shared_ptr<const ContentHashCache::Node>>&&
                  result) {
            file->contentSha1_ =
                makeResultWith([&] { return result.value()->value(); });
          }));
    }

    file->clearNeededProperties();
  }
}

std::optional<FileInformation> InMemoryFileResult::stat() {
  return file_->stat;
}

std::optional<size_t> InMemoryFileResult::size() {
  return file_->stat.size;
}

std::optional<struct timespec> InMemoryFileResult::accessedTime() {
  return file_->stat.atime;
}

std::optional<struct timespec> InMemoryFileResult::modifiedTime() {
  return file_->stat.mtime;
}

std::optional<struct timespec> InMemoryFileResult::changedTime() {
  return file_->stat.ctime;
}

w_string_piece InMemoryFileResult::baseName() {
  return file_->getName();
}

w_string_piece InMemoryFileResult::dirName() {
  if (!dirName_) {
    dirName_ = file_->parent->getFullPath();
  }
  return *dirName_;
}

std::optional<bool> InMemoryFileResult::exists() {
  return file_->exists;
}

std::optional<ClockStamp> InMemoryFileResult::ctime() {
  return file_->ctime;
}

std::optional<ClockStamp> InMemoryFileResult::otime() {
  return file_->otime;
}

std::optional<ResolvedSymlink> InMemoryFileResult::readLink() {
  if (!symlinkTarget_.has_value()) {
    if (!file_->stat.isSymlink()) {
      // We already know it's not a symlink, so there is no need to fetch
      // properties.
      symlinkTarget_ = NotSymlink{};
      return symlinkTarget_;
    }
    // Need to load the symlink target; batch that up
    accessorNeedsProperties(FileResult::Property::SymlinkTarget);
    return std::nullopt;
  }
  return symlinkTarget_;
}

std::optional<FileResult::ContentHash> InMemoryFileResult::getContentSha1() {
  if (!file_->exists) {
    // Don't return hashes for files that we believe to be deleted.
    throw std::system_error(
        std::make_error_code(std::errc::no_such_file_or_directory));
  }

  if (!file_->stat.isFile()) {
    // We only want to compute the hash for regular files
    throw std::system_error(std::make_error_code(std::errc::is_a_directory));
  }

  if (contentSha1_.empty()) {
    accessorNeedsProperties(FileResult::Property::ContentSha1);
    return std::nullopt;
  }
  return contentSha1_.value();
}

ViewDatabase::ViewDatabase(const w_string& root_path)
    : rootPath_{root_path},
      rootDir_{std::make_unique<watchman_dir>(root_path, nullptr)} {}

watchman_dir* ViewDatabase::resolveDir(const w_string& dir_name, bool create) {
  if (dir_name == rootPath_) {
    return rootDir_.get();
  }

  const char* dir_component = dir_name.data();
  const char* dir_end = dir_component + dir_name.size();

  watchman_dir* dir = rootDir_.get();
  dir_component += rootPath_.size() + 1; // Skip root path prefix

  w_assert(dir_component <= dir_end, "impossible file name");

  watchman_dir* parent;
  while (true) {
    auto sep = (const char*)memchr(dir_component, '/', dir_end - dir_component);
    // Note: if sep is NULL it means that we're looking at the basename
    // component of the input directory name, which is the terminal
    // iteration of this search.

    w_string_piece component(
        dir_component, sep ? (sep - dir_component) : (dir_end - dir_component));

    auto child = dir->getChildDir(component);

    if (!child && !create) {
      return nullptr;
    }
    if (!child && sep && create) {
      // A component in the middle wasn't present.  Since we're in create
      // mode, we know that the leaf must exist.  The assumption is that
      // we have another pending item for the parent.  We'll create the
      // parent dir now and our other machinery will populate its contents
      // later.
      w_string child_name(dir_component, (uint32_t)(sep - dir_component));

      // Careful! dir->dirs is keyed by non-owning string pieces so the
      // child_name MUST be stored or otherwise kept alive by the watchman_dir
      // instance constructed below!
      auto& new_child = dir->dirs[child_name];
      new_child.reset(new watchman_dir(child_name, dir));

      child = new_child.get();
    }

    parent = dir;
    dir = child;

    if (!sep) {
      // We reached the end of the string
      if (dir) {
        // We found the dir
        return dir;
      }
      // We need to create the dir
      break;
    }

    // Skip to the next component for the next iteration
    dir_component = sep + 1;
  }

  w_string child_name(dir_component, (uint32_t)(dir_end - dir_component));
  // Careful! parent->dirs is keyed by non-owning string pieces so the
  // child_name MUST be stored or otherwise kept alive by the watchman_dir
  // instance constructed below!
  auto& new_child = parent->dirs[child_name];
  new_child.reset(new watchman_dir(child_name, parent));
  return new_child.get();
}

const watchman_dir* ViewDatabase::resolveDir(const w_string& dir_name) const {
  if (dir_name == rootPath_) {
    return rootDir_.get();
  }

  const char* dir_component = dir_name.data();
  const char* dir_end = dir_component + dir_name.size();

  watchman_dir* dir = rootDir_.get();
  dir_component += rootPath_.size() + 1; // Skip root path prefix

  w_assert(dir_component <= dir_end, "impossible file name");

  while (true) {
    auto sep = (const char*)memchr(dir_component, '/', dir_end - dir_component);
    // Note: if sep is NULL it means that we're looking at the basename
    // component of the input directory name, which is the terminal
    // iteration of this search.

    w_string_piece component(
        dir_component, sep ? (sep - dir_component) : (dir_end - dir_component));

    auto child = dir->getChildDir(component);
    if (!child) {
      return nullptr;
    }

    dir = child;

    if (!sep) {
      // We reached the end of the string
      if (dir) {
        // We found the dir
        return dir;
      }
      // Does not exist
      return nullptr;
    }

    // Skip to the next component for the next iteration
    dir_component = sep + 1;
  }
}

watchman_file* ViewDatabase::getOrCreateChildFile(
    watchman_dir* dir,
    const w_string& file_name,
    ClockStamp ctime) {
  // file_name is typically a baseName slice; let's use it as-is
  // to look up a child...
  auto it = dir->files.find(file_name);
  if (it != dir->files.end()) {
    return it->second.get();
  }

  // ... but take the shorter string from inside the file that
  // we create as the key.
  auto file = watchman_file::make(file_name, dir);
  auto& file_ptr = dir->files[file->getName()];
  file_ptr = std::move(file);

  file_ptr->ctime = ctime;

  return file_ptr.get();
}

void ViewDatabase::markFileChanged(watchman_file* file, ClockStamp otime) {
  file->otime = otime;

  if (latestFile_ != file) {
    // unlink from list
    file->removeFromFileList();

    // and move to the head
    insertAtHeadOfFileList(file);
  }
}

void ViewDatabase::markDirDeleted(
    watchman_dir* dir,
    ClockStamp otime,
    bool recursive) {
  if (!dir->last_check_existed) {
    // If we know that it doesn't exist, return early
    return;
  }
  dir->last_check_existed = false;

  for (auto& it : dir->files) {
    auto file = it.second.get();

    if (file->exists) {
      auto full_name = dir->getFullPathToChild(file->getName());
      logf(DBG, "mark_deleted: {}\n", full_name);
      file->exists = false;
      markFileChanged(file, otime);
    }
  }

  if (recursive) {
    for (auto& it : dir->dirs) {
      auto child = it.second.get();

      markDirDeleted(child, otime, true);
    }
  }
}

void ViewDatabase::insertAtHeadOfFileList(struct watchman_file* file) {
  file->next = latestFile_;
  if (file->next) {
    file->next->prev = &file->next;
  }
  latestFile_ = file;
  file->prev = &latestFile_;
}

InMemoryView::PendingChangeLogEntry::PendingChangeLogEntry(
    const PendingChange& pc,
    std::error_code errcode,
    const FileInformation& st) noexcept {
  this->now = pc.now;
  this->pending_flags = pc.flags.asRaw();
  storeTruncatedTail(this->path_tail, pc.path);

  this->errcode = errcode.value();
  this->mode = st.mode;
  this->size = st.size;
  this->mtime = st.mtime.tv_sec;
}

json_ref InMemoryView::PendingChangeLogEntry::asJsonValue() const {
  return json_object({
      {"now", json_integer(now.time_since_epoch().count())},
      {"pending_flags",
       typed_string_to_json(PendingFlags::raw(pending_flags).format())},
      {"path",
       w_string_to_json(w_string{path_tail, strnlen(path_tail, kPathLength)})},
      {"errcode", json_integer(errcode)},
      {"mode", json_integer(mode)},
      {"size", json_integer(size)},
      {"mtime", json_integer(mtime)},
  });
}

InMemoryView::InMemoryView(
    FileSystem& fileSystem,
    const w_string& root_path,
    Configuration config,
    std::shared_ptr<Watcher> watcher)
    : QueryableView{root_path, /*requiresCrawl=*/true},
      fileSystem_{fileSystem},
      config_(std::move(config)),
      view_(std::in_place, root_path),
      rootNumber_(next_root_number++),
      rootPath_(root_path),
      watcher_(std::move(watcher)),
      caches_(
          root_path,
          config_.getInt("content_hash_max_items", 128 * 1024),
          config_.getInt("symlink_target_max_items", 32 * 1024),
          std::chrono::milliseconds(
              config_.getInt("content_hash_negative_cache_ttl_ms", 2000))),
      enableContentCacheWarming_(
          config_.getBool("content_hash_warming", false)),
      maxFilesToWarmInContentCache_(
          size_t(config_.getInt("content_hash_max_warm_per_settle", 1024))),
      maxFileSizeToWarmInContentCache_(int64_t(config_.getInt(
          "content_hash_max_file_size_to_warm",
          10 * 1024 * 1024))),
      syncContentCacheWarming_(
          config_.getBool("content_hash_warm_wait_before_settle", false)) {
  json_int_t in_memory_view_ring_log_size =
      config_.getInt("in_memory_view_ring_log_size", 0);
  if (in_memory_view_ring_log_size) {
    this->processedPaths_ = std::make_unique<RingBuffer<PendingChangeLogEntry>>(
        in_memory_view_ring_log_size);
  }
}

InMemoryView::~InMemoryView() = default;

ClockStamp InMemoryView::ageOutFile(
    std::unordered_set<w_string>& dirs_to_erase,
    watchman_file* file) {
  auto parent = file->parent;

  auto full_name = parent->getFullPathToChild(file->getName());
  logf(DBG, "age_out file={}\n", full_name);

  auto ageOutOtime = file->otime;

  // If we have a corresponding dir, we want to arrange to remove it, but only
  // after we have unlinked all of the associated file nodes.
  dirs_to_erase.insert(full_name);

  // Remove the entry from the containing file hash; this will free it.
  // We don't need to stop watching it, because we already stopped watching it
  // when we marked it as !exists.
  parent->files.erase(file->getName());

  return ageOutOtime;
}

void InMemoryView::ageOut(
    int64_t& walked,
    int64_t& files,
    int64_t& dirs,
    std::chrono::seconds minAge) {
  files = 0;
  walked = 0;
  std::unordered_set<w_string> dirs_to_erase;

  auto now = std::chrono::system_clock::now();
  lastAgeOutTimestamp_ = now;
  auto view = view_.wlock();

  watchman_file* file = view->getLatestFile();
  watchman_file* prior = nullptr;
  while (file) {
    ++walked;
    if (file->exists ||
        std::chrono::system_clock::from_time_t(file->otime.timestamp) + minAge >
            now) {
      prior = file;
      file = file->next;
      continue;
    }

    auto agedOtime = ageOutFile(dirs_to_erase, file);

    // Revise tick for fresh instance reporting
    lastAgeOutTick_ = std::max(lastAgeOutTick_, agedOtime.ticks);

    files++;

    // Go back to last good file node; we can't trust that the
    // value of file->next saved before age_out_file is a valid
    // file node as anything past that point may have also been
    // aged out along with it.
    file = prior;
  }

  for (auto& name : dirs_to_erase) {
    auto parent = view->resolveDir(name.dirName(), false);
    if (parent) {
      parent->dirs.erase(name.baseName());
    }
  }

  if (files + dirs_to_erase.size()) {
    logf(ERR, "aged {} files, {} dirs\n", files, dirs_to_erase.size());
  }

  dirs = dirs_to_erase.size();
}

void InMemoryView::timeGenerator(const Query* query, QueryContext* ctx) const {
  // Walk back in time until we hit the boundary
  auto view = view_.rlock();
  ctx->generationStarted();

  for (watchman_file* f = view->getLatestFile(); f; f = f->next) {
    ctx->bumpNumWalked();
    // Note that we use <= for the time comparisons in here so that we
    // report the things that changed inclusive of the boundary presented.
    // This is especially important for clients using the coarse unix
    // timestamp as the since basis, as they would be much more
    // likely to miss out on changes if we didn't.
    if (auto* since_ts = std::get_if<QuerySince::Timestamp>(&ctx->since.since);
        since_ts && f->otime.timestamp <= since_ts->time) {
      break;
    }
    if (auto* since_clock = std::get_if<QuerySince::Clock>(&ctx->since.since);
        since_clock && f->otime.ticks <= since_clock->ticks) {
      break;
    }

    if (!ctx->fileMatchesRelativeRoot(f)) {
      continue;
    }

    w_query_process_file(
        query, ctx, std::make_unique<InMemoryFileResult>(f, caches_));
  }
}

void InMemoryView::pathGenerator(const Query* query, QueryContext* ctx) const {
  w_string_piece relative_root;
  struct watchman_file* f;

  if (query->relative_root) {
    relative_root = *query->relative_root;
  } else {
    relative_root = rootPath_;
  }

  auto view = view_.rlock();
  ctx->generationStarted();

  for (const auto& path : *query->paths) {
    const watchman_dir* dir;
    w_string dir_name;

    // Compose path with root
    auto full_name = w_string::pathCat({relative_root, path.name});

    // special case of root dir itself
    if (rootPath_ == full_name) {
      // dirname on the root is outside the root, which is useless
      dir = view->resolveDir(full_name);
      goto is_dir;
    }

    // Ideally, we'd just resolve it directly as a dir and be done.
    // It's not quite so simple though, because we may resolve a dir
    // that had been deleted and replaced by a file.
    // We prefer to resolve the parent and walk down.
    dir_name = full_name.dirName();
    if (dir_name.empty()) {
      continue;
    }

    dir = view->resolveDir(dir_name);

    if (!dir) {
      // Doesn't exist, and never has
      continue;
    }

    if (!dir->files.empty()) {
      auto file_name = path.name.baseName();
      f = dir->getChildFile(file_name);

      // If it's a file (but not an existent dir)
      if (f && (!f->exists || !f->stat.isDir())) {
        ctx->bumpNumWalked();
        w_query_process_file(
            query, ctx, std::make_unique<InMemoryFileResult>(f, caches_));
        continue;
      }
    }

    // Is it a dir?
    if (dir->dirs.empty()) {
      continue;
    }

    dir = dir->getChildDir(full_name.baseName());
  is_dir:
    // We got a dir; process recursively to specified depth
    if (dir) {
      dirGenerator(query, ctx, dir, path.depth);
    }
  }
}

void InMemoryView::dirGenerator(
    const Query* query,
    QueryContext* ctx,
    const watchman_dir* dir,
    uint32_t depth) const {
  for (auto& it : dir->files) {
    auto file = it.second.get();
    ctx->bumpNumWalked();

    w_query_process_file(
        query, ctx, std::make_unique<InMemoryFileResult>(file, caches_));
  }

  if (depth > 0) {
    for (auto& it : dir->dirs) {
      const auto child = it.second.get();

      dirGenerator(query, ctx, child, depth - 1);
    }
  }
}

/** This is our specialized handler for the ** recursive glob pattern.
 * This is the unhappy path because we have no choice but to recursively
 * walk the tree; we have no way to prune portions that won't match.
 * We do coalesce recursive matches together that might generate multiple
 * results.
 * For example: */
// globs: ["foo/**/*.h", "foo/**/**/*.h"]
/* effectively runs the same query multiple times.  By combining the
 * doublestar walk for both into a single walk, we can then match each
 * file against the list of patterns, terminating that match as soon
 * as any one of them matches the file node.
 */
void InMemoryView::globGeneratorDoublestar(
    QueryContext* ctx,
    const struct watchman_dir* dir,
    const GlobTree* node,
    const char* dir_name,
    uint32_t dir_name_len) const {
  bool matched;

  // First step is to walk the set of files contained in this node
  for (auto& it : dir->files) {
    auto file = it.second.get();
    auto file_name = file->getName();

    ctx->bumpNumWalked();

    if (!file->exists) {
      // Globs can only match files that exist
      continue;
    }

    auto subject = make_path_name(
        dir_name, dir_name_len, file_name.data(), file_name.size());

    // Now that we have computed the name of this candidate file node,
    // attempt to match against each of the possible doublestar patterns
    // in turn.  As soon as any one of them matches we can stop this loop
    // as it doesn't make a lot of sense to yield multiple results for
    // the same file.
    for (const auto& child_node : node->doublestar_children) {
      matched =
          wildmatch(
              child_node->pattern.c_str(),
              subject.c_str(),
              ctx->query->glob_flags | WM_PATHNAME |
                  (ctx->query->case_sensitive == CaseSensitivity::CaseSensitive
                       ? 0
                       : WM_CASEFOLD),
              0) == WM_MATCH;

      if (matched) {
        w_query_process_file(
            ctx->query,
            ctx,
            std::make_unique<InMemoryFileResult>(file, caches_));
        // No sense running multiple matches for this same file node
        // if this one succeeded.
        break;
      }
    }
  }

  // And now walk down to any dirs; all dirs are eligible
  for (auto& it : dir->dirs) {
    const auto child = it.second.get();

    if (!child->last_check_existed) {
      // Globs can only match files in dirs that exist
      continue;
    }

    auto subject = make_path_name(
        dir_name, dir_name_len, child->name.data(), child->name.size());
    globGeneratorDoublestar(ctx, child, node, subject.data(), subject.size());
  }
}

/* Match each child of node against the children of dir */
void InMemoryView::globGeneratorTree(
    QueryContext* ctx,
    const GlobTree* node,
    const struct watchman_dir* dir) const {
  if (!node->doublestar_children.empty()) {
    globGeneratorDoublestar(ctx, dir, node, nullptr, 0);
  }

  for (const auto& child_node : node->children) {
    w_assert(!child_node->is_doublestar, "should not get here with ** glob");

    // If there are child dirs, consider them for recursion.
    // Note that we don't restrict this to !leaf because the user may have
    // set their globs list to something like ["some_dir", "some_dir/file"]
    // and we don't want to preclude matching the latter.
    if (!dir->dirs.empty()) {
      // Attempt direct lookup if possible
      if (!child_node->had_specials &&
          ctx->query->case_sensitive == CaseSensitivity::CaseSensitive) {
        w_string_piece component(
            child_node->pattern.data(), child_node->pattern.size());
        const auto child_dir = dir->getChildDir(component);

        if (child_dir) {
          globGeneratorTree(ctx, child_node.get(), child_dir);
        }
      } else {
        // Otherwise we have to walk and match
        for (auto& it : dir->dirs) {
          const auto child_dir = it.second.get();

          if (!child_dir->last_check_existed) {
            // Globs can only match files in dirs that exist
            continue;
          }

          if (wildmatch(
                  child_node->pattern.c_str(),
                  child_dir->name.c_str(),
                  ctx->query->glob_flags |
                      (ctx->query->case_sensitive ==
                               CaseSensitivity::CaseSensitive
                           ? 0
                           : WM_CASEFOLD),
                  0) == WM_MATCH) {
            globGeneratorTree(ctx, child_node.get(), child_dir);
          }
        }
      }
    }

    // If the node is a leaf we are in a position to match files.
    if (child_node->is_leaf && !dir->files.empty()) {
      // Attempt direct lookup if possible
      if (!child_node->had_specials &&
          ctx->query->case_sensitive == CaseSensitivity::CaseSensitive) {
        w_string_piece component(
            child_node->pattern.data(), child_node->pattern.size());
        auto file = dir->getChildFile(component);

        if (file) {
          ctx->bumpNumWalked();
          if (file->exists) {
            // Globs can only match files that exist
            w_query_process_file(
                ctx->query,
                ctx,
                std::make_unique<InMemoryFileResult>(file, caches_));
          }
        }
      } else {
        for (auto& it : dir->files) {
          // Otherwise we have to walk and match
          auto file = it.second.get();
          auto file_name = file->getName();
          ctx->bumpNumWalked();

          if (!file->exists) {
            // Globs can only match files that exist
            continue;
          }

          if (wildmatch(
                  child_node->pattern.c_str(),
                  file_name.data(),
                  ctx->query->glob_flags |
                      (ctx->query->case_sensitive ==
                               CaseSensitivity::CaseSensitive
                           ? 0
                           : WM_CASEFOLD),
                  0) == WM_MATCH) {
            w_query_process_file(
                ctx->query,
                ctx,
                std::make_unique<InMemoryFileResult>(file, caches_));
          }
        }
      }
    }
  }
}

void InMemoryView::globGenerator(const Query* query, QueryContext* ctx) const {
  w_string relative_root;

  if (query->relative_root) {
    relative_root = *query->relative_root;
  } else {
    relative_root = rootPath_;
  }

  auto view = view_.rlock();

  const auto dir = view->resolveDir(relative_root);
  if (!dir) {
    QueryExecError::throwf(
        "glob_generator could not resolve {}, check your relative_root parameter!",
        relative_root);
  }

  globGeneratorTree(ctx, query->glob_tree.get(), dir);
}

void InMemoryView::allFilesGenerator(const Query* query, QueryContext* ctx)
    const {
  struct watchman_file* f;
  auto view = view_.rlock();
  ctx->generationStarted();

  for (f = view->getLatestFile(); f; f = f->next) {
    ctx->bumpNumWalked();
    if (!ctx->fileMatchesRelativeRoot(f)) {
      continue;
    }

    w_query_process_file(
        query, ctx, std::make_unique<InMemoryFileResult>(f, caches_));
  }
}

ClockPosition InMemoryView::getMostRecentRootNumberAndTickValue() const {
  return ClockPosition(rootNumber_, mostRecentTick_);
}

w_string InMemoryView::getCurrentClockString() const {
  char clockbuf[128];
  if (!clock_id_string(
          rootNumber_, mostRecentTick_, clockbuf, sizeof(clockbuf))) {
    throw std::runtime_error("clock string exceeded clockbuf size");
  }
  return w_string(clockbuf, W_STRING_UNICODE);
}

ClockTicks InMemoryView::getLastAgeOutTickValue() const {
  return lastAgeOutTick_;
}

std::chrono::system_clock::time_point InMemoryView::getLastAgeOutTimeStamp()
    const {
  return lastAgeOutTimestamp_;
}

void InMemoryView::startThreads(const std::shared_ptr<Root>& root) {
  // Start a thread to call into the watcher API for filesystem notifications
  auto self = std::static_pointer_cast<InMemoryView>(shared_from_this());
  logf(DBG, "starting threads for {} {}\n", fmt::ptr(this), rootPath_);
  std::thread notifyThreadInstance([self, root]() {
    w_set_thread_name(
        "notify ", uintptr_t(self.get()), " ", self->rootPath_.view());
    try {
      self->notifyThread(root);
    } catch (const std::exception& e) {
      log(ERR, "Exception: ", e.what(), " cancel root\n");
      root->cancel(fmt::format("notifyThread failed: {}", e.what()));
    }
    log(DBG, "out of loop\n");
  });
  notifyThreadInstance.detach();

  // Wait for it to signal that the watcher has been initialized
  pendingFromWatcher_.lockAndWait(std::chrono::milliseconds(-1) /* infinite */);

  // And now start the IO thread
  std::thread ioThreadInstance([self, root]() {
    w_set_thread_name(
        "io ", uintptr_t(self.get()), " ", self->rootPath_.view());
    try {
      self->ioThread(root);
    } catch (const std::exception& e) {
      log(ERR, "Exception: ", e.what(), " cancel root\n");
      root->cancel(fmt::format("ioThread failed: {}", e.what()));
    }
    log(DBG, "out of loop\n");
  });
  ioThreadInstance.detach();
}

void InMemoryView::stopThreads(std::string_view reason) {
  logf(
      DBG,
      "signalThreads! {} {} because ... {}\n",
      fmt::ptr(this),
      rootPath_,
      reason);
  stopThreads_.store(true, std::memory_order_release);
  watcher_->stopThreads();
  {
    auto pending = pendingFromWatcher_.lock();
    // we need this to make sure that watch does not hang
    for (auto& sync : pending->stealSyncs()) {
      sync.setException(
          std::runtime_error(
              fmt::format("Watch shutting down because ... {}", reason)));
    }
    pending->startRefusingSyncs(reason);
    pending->ping();
  }
}

void InMemoryView::wakeThreads() {
  pendingFromWatcher_.lock()->ping();
}

folly::SemiFuture<folly::Unit> InMemoryView::waitForSettle(
    std::chrono::milliseconds settle_period) {
  auto [p, f] = folly::makePromiseContract<folly::Unit>();
  pendingSettles_.withWLock(
      [&, p = std::move(p)](auto& pendingSettles) mutable {
        pendingSettles.insert(std::make_pair(settle_period, std::move(p)));
      });

  // iothread might be waiting, so wake it so it sees the new entry in
  // pendingSettles_.
  pendingFromWatcher_.lock()->ping();

  return std::move(f);
}

/* Ensure that we're synchronized with the state of the
 * filesystem at the current time.
 * We do this by touching a cookie file and waiting to
 * observe it via inotify.  When we see it we know that
 * we've seen everything up to the point in time at which
 * we're asking questions.
 * Throws a std::system_error with an ETIMEDOUT error if
 * the timeout expires before we observe the change, or
 * a runtime_error if the root has been deleted or rendered
 * inaccessible. */
CookieSync::SyncResult InMemoryView::syncToNow(
    const std::shared_ptr<Root>& root,
    std::chrono::milliseconds timeout) {
  auto syncResult = syncToNowCookies(root, timeout);

  // Some watcher implementations (notably, FSEvents) reorder change events
  // before they're reported, and cookie files are not sufficient. Instead, the
  // watcher supports direct synchronization. Once a cookie file has been
  // observed, ensure that all pending events have been flushed and wait until
  // the pending event queue is fully crawled.
  auto result = watcher_->flushPendingEvents();
  if (result.valid()) {
    // The watcher has made all pending events available and inserted a promise
    // into its PendingCollection. Wait for InMemoryView to observe it and
    // everything prior.
    //
    // Would be nice to use a deadline rather than a timeout here.

    try {
      std::move(result).get(timeout);
    } catch (folly::FutureTimeout&) {
      auto why = fmt::format(
          "syncToNow: timed out waiting for pending watcher events to be flushed within {} milliseconds",
          timeout.count());
      log(ERR, why, "\n");
      throw std::system_error(ETIMEDOUT, std::generic_category(), why);
    }
  }

  return syncResult;
}

folly::SemiFuture<CookieSync::SyncResult> InMemoryView::sync(
    const std::shared_ptr<Root>& root) {
  return root->cookies.sync();
}

CookieSync::SyncResult InMemoryView::syncToNowCookies(
    const std::shared_ptr<Root>& root,
    std::chrono::milliseconds timeout) {
  try {
    return root->cookies.syncToNow(timeout);
  } catch (const std::system_error& exc) {
    auto cookieDirs = root->cookies.cookieDirs();

    if (exc.code() == error_code::no_such_file_or_directory ||
        exc.code() == error_code::permission_denied ||
        exc.code() == error_code::not_a_directory) {
      // A key path was removed; this is either the vcs dir (.hg, .git, .svn)
      // or possibly the root of the watch itself.
      if (!(watcher_->flags & WATCHER_HAS_SPLIT_WATCH)) {
        w_assert(
            cookieDirs.size() == 1,
            "Non split watchers cannot have multiple cookie directories");
        if (cookieDirs.count(rootPath_) == 1) {
          // If the root was removed then we need to cancel the watch.
          // We may have already observed the removal via the notifythread,
          // but in some cases (eg: btrfs subvolume deletion) no notification
          // is received.
          root->cancel("root directory was removed or is inaccessible");
          throw std::runtime_error("root dir was removed or is inaccessible");
        } else {
          // The cookie dir was a VCS subdir and it got deleted.  Let's
          // focus instead on the parent dir and recursively retry.
          root->cookies.setCookieDir(rootPath_);
          return root->cookies.syncToNow(timeout);
        }
      } else {
        // Split watchers have one watch on the root and watches for nested
        // directories, and syncToNow will only throw if no cookies were
        // created, ie: if all the nested watched directories are no longer
        // present and the root directory has been removed.
        root->cancel("root dir was removed or is inaccessible");
        throw std::runtime_error("root dir was removed or is inaccessible");
      }
    }

    // Let's augment the error reason with the current recrawl state,
    // if any.
    {
      auto info = root->recrawlInfo.rlock();

      if (!root->inner.done_initial || info->shouldRecrawl) {
        std::string extra = (info->recrawlCount > 0)
            ? fmt::format("(re-crawling, count={})", info->recrawlCount)
            : "(performing initial crawl)";

        throw std::system_error(
            exc.code(), fmt::format("{}. {}", exc.what(), extra));
      }
    }

    // On BTRFS we're not guaranteed to get notified about all classes
    // of replacement so we make a best effort attempt to do something
    // reasonable.   Let's pretend that we got notified about the cookie
    // dir changing and schedule the IO thread to look at it.
    // If it observes a change it will do the right thing.
    {
      auto now = std::chrono::system_clock::now();

      // TODO: pass this PendingCollection in as a parameter
      auto lock = pendingFromWatcher_.lock();
      for (const auto& dir : cookieDirs) {
        lock->add(dir, now, W_PENDING_CRAWL_ONLY);
      }
      lock->ping();
    }

    // We didn't have any useful additional contextual information
    // to add so let's just bubble up the exception.
    throw;
  }
}

bool InMemoryView::doAnyOfTheseFilesExist(
    const std::vector<w_string>& fileNames) const {
  auto view = view_.rlock();
  for (auto& name : fileNames) {
    auto fullName = w_string::pathCat({rootPath_, name});
    const auto dir = view->resolveDir(fullName.dirName());
    if (!dir) {
      continue;
    }

    auto file = dir->getChildFile(fullName.baseName());
    if (!file) {
      continue;
    }
    if (file->exists) {
      return true;
    }
  }
  return false;
}

const w_string& InMemoryView::getName() const {
  return watcher_->name;
}

const std::shared_ptr<Watcher>& InMemoryView::getWatcher() const {
  return watcher_;
}

json_ref InMemoryView::getWatcherDebugInfo() const {
  return json_object({
      {"watcher", watcher_->getDebugInfo()},
      {"view", getViewDebugInfo()},
  });
}

void InMemoryView::clearWatcherDebugInfo() {
  watcher_->clearDebugInfo();
  clearViewDebugInfo();
}

json_ref InMemoryView::getViewDebugInfo() const {
  auto processedPathsResult = json_null();
  if (processedPaths_) {
    std::vector<json_ref> paths;
    for (auto& entry : processedPaths_->readAll()) {
      paths.push_back(entry.asJsonValue());
    }
    processedPathsResult = json_array(std::move(paths));
  }
  return json_object({
      {"processed_paths", processedPathsResult},
  });
}

void InMemoryView::clearViewDebugInfo() {
  if (processedPaths_) {
    processedPaths_->clear();
  }
}

void InMemoryView::warmContentCache() {
  if (!enableContentCacheWarming_) {
    return;
  }

  log(DBG, "considering files for content hash cache warming\n");

  size_t n = 0;
  std::deque<folly::Future<std::shared_ptr<const ContentHashCache::Node>>>
      futures;

  {
    // Walk back in time until we hit the boundary, or hit the limit
    // on the number of files we should warm up.
    auto view = view_.rlock();
    struct watchman_file* f;
    for (f = view->getLatestFile(); f && n < maxFilesToWarmInContentCache_;
         f = f->next) {
      if (f->otime.ticks <= lastWarmedTick_) {
        log(DBG,
            "warmContentCache: stop because file ticks ",
            f->otime.ticks,
            " is <= lastWarmedTick_ ",
            lastWarmedTick_,
            "\n");
        break;
      }

      if (f->exists && f->stat.isFile() &&
          (maxFileSizeToWarmInContentCache_ <= 0 ||
           f->stat.size <=
               static_cast<uint64_t>(maxFileSizeToWarmInContentCache_))) {
        // Note: we could also add an expression to further constrain
        // the things we warm up here.  Let's see if we need it before
        // going ahead and adding.

        auto dirStr = f->parent->getFullPath();
        w_string_piece dir(dirStr);
        dir.advance(caches_.contentHashCache.rootPath().size());

        // If dirName is the root, dir.size() will now be zero
        if (dir.size() > 0) {
          // if not at the root, skip the slash character at the
          // front of dir
          dir.advance(1);
        }
        ContentHashCacheKey key{
            w_string::pathCat({dir, f->getName()}),
            size_t(f->stat.size),
            f->stat.mtime};

        log(DBG, "warmContentCache: lookup ", key.relativePath, "\n");
        auto f_2 = caches_.contentHashCache.get(key);
        if (syncContentCacheWarming_) {
          futures.emplace_back(std::move(f_2));
        }
        ++n;
      }
    }

    lastWarmedTick_ = mostRecentTick_;
  }

  log(DBG,
      "warmContentCache, lastWarmedTick_ now ",
      lastWarmedTick_,
      " scheduled ",
      n,
      " files for hashing, will wait for ",
      futures.size(),
      " lookups to finish\n");

  if (syncContentCacheWarming_) {
    // Wait for them to finish, but don't use get() because we don't
    // care about any errors that may have occurred.
    folly::collectAll(futures.begin(), futures.end()).wait();
    log(DBG, "warmContentCache: hashing complete\n");
  }
}

} // namespace watchman
