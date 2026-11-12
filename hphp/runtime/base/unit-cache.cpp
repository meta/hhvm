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
#include <atomic>
#include <cstdint>
#include "hphp/runtime/base/unit-cache.h"

#include "hphp/runtime/base/autoload-handler.h"
#include "hphp/runtime/base/builtin-functions.h"
#include "hphp/runtime/base/coeffects-config.h"
#include "hphp/runtime/base/configs/unit-cache-generated.h"
#include "hphp/runtime/base/execution-context.h"
#include "hphp/runtime/base/file-stream-wrapper.h"
#include "hphp/runtime/base/file-util.h"
#include "hphp/runtime/base/init-fini-node.h"
#include "hphp/runtime/base/plain-file.h"
#include "hphp/runtime/base/program-functions.h"
#include "hphp/runtime/base/stat-cache.h"
#include "hphp/runtime/base/stream-wrapper-registry.h"
#include "hphp/runtime/base/string-util.h"
#include "hphp/runtime/base/vm-worker.h"
#include "hphp/runtime/base/zend-string.h"
#include "hphp/runtime/server/cli-server.h"
#include "hphp/runtime/server/source-root-info.h"
#include "hphp/runtime/vm/debugger-hook.h"
#include "hphp/runtime/vm/repo-file.h"
#include "hphp/runtime/vm/runtime-compiler.h"
#include "hphp/runtime/vm/treadmill.h"
#include "hphp/runtime/vm/unit-emitter.h"
#include "hphp/runtime/vm/unit-parser.h"

#include "hphp/util/assertions.h"
#include "hphp/util/build-info.h"
#include "hphp/util/configs/debugger.h"
#include "hphp/util/configs/eval.h"
#ifdef USE_JEMALLOC
#include "hphp/util/configs/stats.h"
#include "hphp/runtime/vm/native.h"
#endif
#include "hphp/util/mutex.h"
#include "hphp/util/rank.h"
#include "hphp/util/struct-log.h"
#include "hphp/util/timer.h"

#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#include <boost/algorithm/string/predicate.hpp>
#include <folly/FileUtil.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/executors/task_queue/PriorityUnboundedBlockingQueue.h>
#include <folly/portability/Fcntl.h>
#include <folly/portability/SysStat.h>
#include <folly/synchronization/AtomicNotification.h>

#include <sys/xattr.h>

namespace HPHP {

//////////////////////////////////////////////////////////////////////

namespace {

//////////////////////////////////////////////////////////////////////

using OptLog = Optional<StructuredLogEntry>;

struct LogTimer {
  LogTimer(const char* name, OptLog& ent)
    : m_name(name)
    , m_ent(ent)
    , m_start(m_ent ? Timer::GetThreadCPUTimeNanos() : -1)
  {}
  ~LogTimer() { stop(); }

  void stop() {
    if (m_start == -1) return;

    auto const elapsed = Timer::GetThreadCPUTimeNanos() - m_start;
    m_ent->setInt(m_name, elapsed / 1000);
    m_start = -1;
  }

private:
  const char* m_name;
  OptLog& m_ent;
  int64_t m_start;
};

struct CachedUnit {
  Unit* unit{};
  size_t rdsBitId{-1uL};
};

struct CachedUnitInternal {
  CachedUnitInternal() = default;
  CachedUnitInternal(const CachedUnitInternal& src) :
      unit{src.unit.copy()},
      rdsBitId{src.rdsBitId} {}
  CachedUnitInternal& operator=(const CachedUnitInternal&) = delete;

  static Unit* const Uninit;

  CachedUnit cachedUnit() const {
    return CachedUnit { unit.get(), rdsBitId };
  }

  // nullptr if there is no Unit for this path, Uninit if the CachedUnit
  // hasn't been initialized yet.
  mutable LockFreePtrWrapper<Unit*> unit{Uninit};
  // id of the RDS bit for whether the Unit is included
  mutable size_t rdsBitId{-1u};
};

Unit* const CachedUnitInternal::Uninit = reinterpret_cast<Unit*>(-8);

//////////////////////////////////////////////////////////////////////
// RepoAuthoritative mode unit caching

/*
 * In RepoAuthoritative mode, loaded units are never unloaded, we
 * don't support symlink chasing, you can't include urls, and files
 * are never changed, which makes the code here significantly simpler.
 * Because of this it pays to keep it separate from the other cases so
 * they don't need to be littered with RepoAuthoritative checks.
 */
using RepoUnitCache = RankedCHM<
  const StringData*,     // must be static
  CachedUnitInternal,
  StringDataHashCompare,
  RankUnitCache
>;
RepoUnitCache s_repoUnitCache;
#ifdef USE_JEMALLOC
std::atomic<uint64_t> s_unitLoadOrder = 0;
#endif
CachedUnit lookupUnitRepoAuth(const StringData* path,
                              const RepoUnitInfo* info) {
  assertx(!info || info->path == path);
  tracing::BlockNoTrace _{"lookup-unit-repo-auth"};

  path = makeStaticString(path);

  RepoUnitCache::const_accessor acc;
  s_repoUnitCache.insert(acc, path);
  auto const& cu = acc->second;

  if (cu.unit.copy() != CachedUnitInternal::Uninit) return cu.cachedUnit();

  auto lock = cu.unit.lock_for_update();
  if (cu.unit.copy() != CachedUnitInternal::Uninit) {
    // Someone else updated the unit while we were waiting on the lock
    return cu.cachedUnit();
  }

  auto const create = [] (std::unique_ptr<UnitEmitter> ue) {
#ifdef USE_JEMALLOC
    if (Cfg::Stats::TrackPerUnitMemory) {
      size_t len = sizeof(uint64_t*);
      uint64_t* alloc;
      uint64_t* del;
      mallctl("thread.allocatedp", static_cast<void*>(&alloc), &len, nullptr, 0);
      mallctl("thread.deallocatedp", static_cast<void*>(&del), &len, nullptr, 0);
      auto before = *alloc;
      auto debefore = *del;
      auto result = ue->create();
      auto after = *alloc;
      auto deafter = *del;
      auto change = (after - deafter) - (before - debefore);

      StructuredLogEntry ent;
      ent.setStr("filepath", ue->m_filepath->toCppString());
      ent.setInt("mem_change", change);
      ent.setInt("order", ++s_unitLoadOrder);
      StructuredLog::log("hhvm_unit_loads", ent);

      return result;
    }
#endif
    return ue->create();
  };

  try {
    /*
     * We got the lock, so we're responsible for updating the entry.
     */
    auto ue = RepoFile::loadUnitEmitter(path, info, true);
    if (ue) {
      auto unit = create(std::move(ue));
      cu.rdsBitId = rds::allocBit();
      lock.update(unit.release());
    } else {
      lock.update(nullptr);
    }
  } catch (...) {
    lock.release();
    s_repoUnitCache.erase(acc);
    throw;
  }
  return cu.cachedUnit();
}

//////////////////////////////////////////////////////////////////////
// Non-repo mode unit caching

void releaseFromHashCache(Unit*);

struct CachedFile {
  CachedFile() = delete;
  explicit CachedFile(const CachedFile&) = delete;
  CachedFile& operator=(const CachedFile&) = delete;

  CachedFile(const CachedUnit& src,
             const struct stat& statInfo,
             const RepoOptions& options)
    : cu(src)
    , dirty(false)
    , repoOptionsHash(options.flags().cacheKeySha1())
  {
    mtime      = statInfo.st_mtim;
    ctime      = statInfo.st_ctim;
    ino        = statInfo.st_ino;
    devId      = statInfo.st_dev;
    if (cu.unit) cu.unit->acquireCacheRefCount();
  }

  // Create a new CachedFile entry, sharing a Unit with another one,
  // but with a new stat info.
  CachedFile(const CachedFile& o, const struct stat& statInfo)
    : cu{o.cu}
    , dirty(false)
    , repoOptionsHash(o.repoOptionsHash)
  {
    mtime      = statInfo.st_mtim;
    ctime      = statInfo.st_ctim;
    ino        = statInfo.st_ino;
    devId      = statInfo.st_dev;
    if (cu.unit) {
      // Since this is sharing it, we should already have a ref.
      assertx(cu.unit->hasCacheRef());
      cu.unit->acquireCacheRefCount();
    }
  }

  ~CachedFile() {
    if (!cu.unit) return;
    if (cu.unit->hasPerRequestFilepath()) {
      // Units with per-request filepaths are backed by the per-hash
      // cache and need special handling while deleting.
      releaseFromHashCache(cu.unit);
    } else if (cu.unit->releaseCacheRefCount()) {
      // Otherwise, only delete the Unit if this is the last reference
      // to it.
      cu.unit->startDestroy();
    }
  }

  CachedUnit cu;

  mutable struct timespec mtime;
  mutable struct timespec ctime;
  mutable ino_t ino;
  mutable dev_t devId;

  // We set the dirty flag for files that are eagerly invalidated via a call
  // to unitCacheSyncRepo() to indicate they may require recompilation. We
  // don't remove them from the cache entirely as we may find that they have
  // not meaningfully changed and can be reused.
  mutable std::atomic<bool> dirty;

  SHA1 repoOptionsHash;
};

using CachedFilePtr = copy_ptr<CachedFile>;

struct CachedUnitNonRepo {
  CachedUnitNonRepo() = default;
  CachedUnitNonRepo(const CachedUnitNonRepo& other) :
      cachedUnit{other.cachedUnit.copy()} {}
  CachedUnitNonRepo& operator=(const CachedUnitNonRepo&) = delete;

  mutable LockFreePtrWrapper<CachedFilePtr> cachedUnit;
};

using NonRepoUnitCache = RankedCHM<
  const StringData*,     // must be static
  CachedUnitNonRepo,
  StringDataHashCompare,
  RankUnitCache
>;
NonRepoUnitCache s_nonRepoUnitCache;

struct CachedByHashUnit {
  CachedByHashUnit() = default;
  CachedByHashUnit(const CachedByHashUnit& other) : unit{other.unit.copy()} {}
  CachedByHashUnit& operator=(const CachedByHashUnit&) = delete;

  mutable LockFreePtrWrapper<Unit*> unit;
};

struct SHA1HashCompare {
  bool equal(const SHA1& a, const SHA1& b) const { return a == b; }
  size_t hash(const SHA1& a) const { return a.hash(); }
};

using UnitByHashCache = RankedCHM<
  SHA1,
  CachedByHashUnit,
  SHA1HashCompare,
  RankUnitHashCache
>;
UnitByHashCache s_unitByHashCache;

RDS_LOCAL(bool, rl_trustAutoloadedUnits);

//////////////////////////////////////////////////////////////////////

namespace {

ServiceData::ExportedCounter* s_unitCompileAttempts =
  ServiceData::createCounter("vm.unit-compile-attempts");
ServiceData::ExportedCounter* s_unitActualCompiles =
  ServiceData::createCounter("vm.unit-actual-compiles");
ServiceData::ExportedCounter* s_unitsPrefetched =
  ServiceData::createCounter("vm.units-prefetched");
ServiceData::ExportedCounter* s_unitsPathReaped =
  ServiceData::createCounter("vm.units-path-reaped");
ServiceData::ExportedCounter* s_unitsEvalReaped =
  ServiceData::createCounter("vm.units-eval-reaped");
ServiceData::ExportedCounter* s_unitCompileFileLoads =
  ServiceData::createCounter("vm.unit-compile-file-loads");
ServiceData::ExportedCounter* s_unitEdenInconsistencies =
  ServiceData::createCounter("vm.unit-eden-inconsistencies");
ServiceData::ExportedCounter* s_unitReuseBytecode =
  ServiceData::createCounter("vm.unit-reuse-bytecode");

}

using HashCache = tbb::concurrent_hash_map<
  std::string,
  std::pair<struct stat, SHA1>
>;

using PerRepoHC = tbb::concurrent_hash_map<
  std::string,
  std::unique_ptr<HashCache>
>;

PerRepoHC s_hashCaches;

HashCache& getHashCache(const std::string& repo) {
  PerRepoHC::const_accessor cns_acc;
  if (s_hashCaches.find(cns_acc, repo)) return *cns_acc->second;

  PerRepoHC::accessor acc;
  if (!s_hashCaches.insert(acc, repo)) return *acc->second;
  acc->second = std::make_unique<HashCache>();
  return *acc->second;
}

Optional<OptString> loadFileContents(const char* path,
                                     Stream::Wrapper* wrapper) {
  std::string contents;
  assertx(path);

  tracing::Block _{
    "read-file", [&] { return tracing::Props{}.add("path", path); }
  };
  // If the file is too large it may OOM the request
  MemoryManager::SuppressOOM so(*tl_heap);
  if (wrapper) {
    // We only allow normal file streams, which cannot re-enter
    assertx(wrapper->isNormalFileStream());
    if (auto const f = wrapper->open(OptString{path}, "r", 0, nullptr)) {
      return f->read();
    }
    return {};
  }

  auto const fd = open(path, O_RDONLY);
  if (fd < 0) return {};
  auto file = req::make<PlainFile>(fd);
  return file->read();
}

int64_t timespecCompare(const struct timespec& l,
                        const struct timespec& r) {
  if (l.tv_sec != r.tv_sec) return l.tv_sec - r.tv_sec;
  return l.tv_nsec - r.tv_nsec;
}

bool isChanged(const struct stat* old_st, const struct stat* new_st) {
  return timespecCompare(old_st->st_mtim, new_st->st_mtim) < 0 ||
         timespecCompare(old_st->st_ctim, new_st->st_ctim) < 0 ||
         old_st->st_ino != new_st->st_ino ||
         old_st->st_dev != new_st->st_dev;
}

Optional<SHA1> getHashForFile(const std::string& relPath,
                              Stream::Wrapper* wrapper,
                              const std::filesystem::path& root) {
  if (Cfg::Eval::UseEdenFS) {
    if (auto const h = getHashFromEden(relPath.data(), wrapper)) return SHA1{*h};
  }

  auto& cache = getHashCache(root.string());

  struct stat st;
  auto const fullpath = root / relPath;
  if (statSyscall(fullpath.string(), &st) != 0) return {};

  {
    HashCache::const_accessor acc;
    if (cache.find(acc, relPath)) {
      if (!isChanged(&acc->second.first, &st)) return acc->second.second;
    }
  }

  HashCache::accessor acc;
  if (!cache.insert(acc, relPath)) {
    if (!isChanged(&acc->second.first, &st)) return acc->second.second;
  }

  if (auto const c = loadFileContents(fullpath.c_str(), wrapper)) {
    auto const ret = SHA1{string_sha1(c->slice())};
    acc->second = std::make_pair(st, ret);
    return ret;
  }

  return {};
}

//////////////////////////////////////////////////////////////////////

uint64_t g_units_seen_count = 0;

bool stressUnitCache() {
  if (Cfg::Eval::StressUnitCacheFreq <= 0) return false;
  if (Cfg::Eval::StressUnitCacheFreq == 1) return true;
  return ++g_units_seen_count % Cfg::Eval::StressUnitCacheFreq == 0;
}

/*
 * Represents whether a unit may require recompilation and for what reason.
 * NOTE: This enum is used in an order dependent manner. Earlier items are
 * considered "less severe" than later items: we may not consider a file changed
 * if just the stat data for the underlying file is different (`STAT`) but we
 * will always consider a file changed if the current repo options are different
 * from the options in the cached unit (`REPO_OPTION_DIFF`).
 */
enum class UnitChange {
  /* This unit does not require re-compilation: the file on disk has not
   * changed **and** none of it's dependencies have changed.
   */
  NONE,
  /* This unit's backing file has potentially been modified according to the
   * stat data. The dependencies **may** also have changed.
   */
  STAT,
  /* At least one dependency file hash has changed. */
  DEPS,
  /* We have decided to stress test the unit cache: *always* consider this
   * file changed.
   */
  STRESS_TEST,
  /* The repo options between this request and the cached unit differ, so
   * we should never use the cached bytecode for this unit.
   */
  REPO_OPTION_DIFF,
  /* The cached file pointer or the underlying unit are null: we must
   * recompile this file.
   */
  EMPTY_UNIT
};

/*
 * Iterate over the dependencies of `u` and check if the hash of any of those
 * dependencies on disk differs from the hash recorded in the unit.
 * See `changeReason` for more details. This functions asserts that either:
 * - `Cfg::Eval::EnableDecl` is false
 * - `u->deps()` is empty.
 * If `Cfg::Eval::EnableDecl` is true and `u->deps()` is non-empty, then we are in
 * an invalid state: tracking declarations despite not having decl driven
 * bytecode enabled.
 *
 * @precondition `u != nullptr`
 */
bool anyDepsChanged(
  const Unit* u,
  const RepoOptions& options,
  Stream::Wrapper* wrapper
) {
  assertx(u);
  assertx(Cfg::Eval::EnableDecl || u->deps().empty());
  for (auto& [file, hash] : u->deps()) {
    if (getHashForFile(file, wrapper, options.dir()) != hash) {
      return true;
    }
  }
  return false;
}

/*
 * Determine whether or not cachedUnit can be used from the cache or requires
 * recompilation from disk.
 *
 * When Cfg::Eval::EnableDecl is false this a pure function of whether or not the
 * source text of the file has been modified. In this function we check to see
 * if the stat() information has changed on disk as a shortcut.
 *
 * When decls are enabled we must also know if any files that the requested
 * source file depended on have been modified. For this purpose each unit
 * stores a vector of dependent files and their SHA-1 hashes. We check each
 * file in this list to see if a dependency has changed.
 *
 * To speed up this operation on non-eden filesystems we store a separate cache
 * of SHA-1 sums for each repository, again leveraging stat to determine when
 * entries require recomputation.
 *
 * Notice that this cache is lazily invalidated both with and without decl
 * information. Entries are not preemptively evicted when changed on disk or
 * when dependencies are modified. This allows us to avoid maintaining a reverse
 * dependency map and can save substantial time when files with significant fan-
 * out are modified.
 *
 * While systems that require a "complete" compilation of the world (e.g. a type
 * checker) benefit from eager recomputation, hhvm seldom requires a substantial
 * working set when running in sandbox mode and thus a lazy approach is well
 * suited to appear more responsive even after changes that ultimately cause
 * large invalidation fan-out.
 */
UnitChange changeReason(
  const CachedFilePtr& cachedUnit,
  const struct stat& s,
  const RepoOptions& options,
  Stream::Wrapper* wrapper,
  bool forAutoloadHit
) {
  auto const checkStat = [&] {
    if (forAutoloadHit) {
      return cachedUnit->dirty.load(std::memory_order_acquire);
    }
    return
      timespecCompare(cachedUnit->mtime, s.st_mtim) < 0 ||
      timespecCompare(cachedUnit->ctime, s.st_ctim) < 0 ||
      cachedUnit->ino != s.st_ino ||
      cachedUnit->devId != s.st_dev;
  };
  // If the cached unit is null, we always need to consider it out of date (in
  // case someone created the file).  This case should only happen if something
  // successfully stat'd the file, but then it was gone by the time we tried to
  // open() it.
  if (cachedUnit.isNull() || cachedUnit->cu.unit == nullptr) {
    return UnitChange::EMPTY_UNIT;
  } else if (checkStat()) {
    return UnitChange::STAT;
  } else if (anyDepsChanged(cachedUnit->cu.unit, options, wrapper)) {
    return UnitChange::DEPS;
  } else if (cachedUnit->repoOptionsHash != options.flags().cacheKeySha1()) {
    return UnitChange::REPO_OPTION_DIFF;
  } else if (stressUnitCache()) {
    return UnitChange::STRESS_TEST;
  }
  return UnitChange::NONE;
}

// Returns true if the given unit has no bound path, or it has already
// been bound with the given path.
bool canBeBoundToPath(const Unit* unit, const StringData* path) {
  assertx(unit);
  assertx(path->isStatic());
  if (!Cfg::Eval::ReuseUnitsByHash) return true;
  auto const p = unit->perRequestFilepath();
  if (!p) return true;
  assertx(p->isStatic());
  return p == path;
}

bool canBeBoundToPath(const CachedFilePtr& cachedUnit,
                      const StringData* path) {
  assertx(path->isStatic());
  if (!Cfg::Eval::ReuseUnitsByHash) return true;
  if (!cachedUnit || !cachedUnit->cu.unit) return true;
  return canBeBoundToPath(cachedUnit->cu.unit, path);
}

/*
 * When a Unit is backed by the per-hash cache (which is equivalent to
 * it having a per-request filepath), we cannot just treadmill it
 * immediately when its cache ref-count drops to zero (like
 * normal). We need to remove it from the per-hash cache first.
 *
 * However, this has a race:
 *
 * - This thread decrements the cache ref-count on the Unit and drops
 *   it to zero. It enters the function below and prepares to remove
 *   the Unit from the per-hash cache.
 *
 * - Simultaneously another thread is accessing the same Unit in the
 *   per-hash cache. That thread increments the ref-count (bringing it
 *   from 0 back to 1). The Unit is now alive again.
 *
 * - This thread removes the Unit from the cache (not a big deal) and
 *   puts it on the treadmill (more problematic).
 *
 * We can work around this by the following:
 *
 * - We require that all inc-refs (but not dec-refs) of the cache
 *   ref-count occur while holding the per-hash cache accessor. When
 *   we go to remove the Unit from the cache, we use an exclusive
 *   (non-const) accessor which guarantees no other thread is holding
 *   an accessor on that key. Since inc-refs can only happen while
 *   holding an accessor, we are guarded against a dead Unit
 *   (ref-count of zero) resurrecting.
 *
 * - Once we hold the exclusive accessor, we check the ref-count
 *   again. If its still zero, its safe to remove (since at this point
 *   it cannot go back up).
 *
 * - Multiple threads can still see the ref-count go to zero (if
 *   there's been inc-refs in between), so multiple threads can
 *   attempt to acquire the accessor and free the entry. Therefore,
 *   once we acquire the accessor, we confirm that the Unit still
 *   exists in the cache (along with its ref-count). The first thread
 *   which gets the accessor will remove it, and the rest will do
 *   nothing.
 *
 * - Note: there's still technically a hazard here where the cache
 *   entry could be replaced with another Unit at an identical memory
 *   address. This is exceedingly unlikely, and harmless. If that
 *   happens and the new Unit's ref-count happens to be zero, this
 *   thread will just free that Unit instead on behalf of some other
 *   thread.
 *
 * NB: We never erase the cache entries, just null them out. This
 * makes it safe to iterate over the cache.
 */

void releaseFromHashCache(Unit* unit) {
  assertx(unit);
  assertx(!Cfg::Repo::Authoritative);
  assertx(Cfg::Eval::ReuseUnitsByHash);
  assertx(unit->hasPerRequestFilepath());

  // Capture the SHA1 first from the Unit. If the release drops the
  // ref-count to zero, another thread may delete it, so we can't
  // access the Unit after this.
  auto const sha1 = unit->sha1();
  // If this isn't the last reference, nothing more to do.
  if (!unit->releaseCacheRefCount()) return;
  // NB: unit may be already freed at this point, so don't access it
  // until we acquire the cache accessor.

  tracing::BlockNoTrace _{"unit-hash-cache-erase"};

  // Note the non-const accessor, which guarantees exclusivity.
  UnitByHashCache::accessor acc;
  // We never remove keys from the cache and since this Unit existed
  // in the cache, the key should always be there.
  always_assert(s_unitByHashCache.find(acc, sha1));

  auto& cached = acc->second.unit;

  // While we're holding the exclusive accessor, the ref-count cannot
  // go back up. The Unit may have been freed while we were waiting
  // for the accessor. Check if the entry for the hash is the same
  // Unit. If it is, the Unit still exists and we can access it
  // safely.
  if (cached.copy() != unit) {
    // See if the unit is anywhere in the linked list of cached units
    for (auto u = cached.copy(); u; u = u->nextCachedByHash()) {
      if (u->nextCachedByHash() == unit) {
        if (unit->hasCacheRef()) return;
        u->setNextCachedByHash(unit->nextCachedByHash());
        unit->startDestroy();
      }
    }
    return;
  }
  // Check if the ref-count went back up since we decremented it. If
  // so, freeing it will happen later and be some other thread's
  // responsibility.
  if (unit->hasCacheRef()) return;

  // Acquire the entry lock. This is probably pedantic since nobody
  // else should have an accessor on this entry.
  auto lock = cached.lock_for_update();
  // Since we have an exclusive accessor, the ref-count should be
  // unchanged.
  assertx(!unit->hasCacheRef());

  // Null out the entry in the cache. The "old" Unit should be our
  // Unit since we already checked that. Treadmill the Unit and we're
  // done. Once we release the accessor, any other thread waiting to
  // delete this Unit will see that the entry is gone.
  auto const DEBUG_ONLY old = lock.update(unit->nextCachedByHash());
  assertx(old == unit);
  unit->startDestroy();
}

CachedFilePtr createUnitFromFile(const StringData* const path,
                                 CodeSource codeSource,
                                 Stream::Wrapper* wrapper,
                                 Unit** releaseUnit,
                                 OptLog& ent,
                                 const Extension* extension,
                                 AutoloadMap* map,
                                 const RepoOptions& options,
                                 FileLoadFlags& flags,
                                 const struct stat& statInfo,
                                 CachedFilePtr orig,
                                 bool forPrefetch,
                                 UnitChange reason) {
  auto const impl = [&] (bool tryLazy) {
    tracing::BlockNoTrace _{"create-unit-from-file"};

    LazyUnitContentsLoader loader{
      path->data(),
      wrapper,
      options,
      (size_t)statInfo.st_size,
      !tryLazy
    };
    SCOPE_EXIT {
      if (loader.didLoad()) {
        tracing::updateName("create-unit-from-file-load");
        s_unitCompileFileLoads->increment();
      }
    };
    s_unitCompileAttempts->increment();

    // The stat may have indicated that the file was touched, but the
    // contents may not have actually changed. In that case, the hash we
    // just calculated may be the same as the pre-existing Unit's
    // hash. In that case, we just use the old unit.
    if (
      orig &&
      orig->cu.unit &&
      loader.sha1() == orig->cu.unit->sha1() &&
      // Earlier we checked if we think this unit has changed: if we've already
      // checked the dependencies and they **have** changed, then there's no
      // need to do so again. If the reason for change was not due to the
      // dependencies and doesn't *further* require we recompile the unit, then
      // we check the dependencies here and re-use the original unit if no
      // dependencies have changed.
      reason < UnitChange::DEPS &&
      !anyDepsChanged(orig->cu.unit, options, wrapper)
    ) {
      return CachedFilePtr{*orig, statInfo};
    }

    // Compile a new Unit from contents
    auto const compileNew = [&] {
      s_unitActualCompiles->increment();
      LogTimer compileTimer("compile_ms", ent);
      rqtrace::EventGuard trace{"COMPILE_UNIT"};
      trace.annotate("file_size", folly::to<std::string>(loader.fileLength()));
      flags = FileLoadFlags::kCompiled;
      return compile_file(loader, codeSource, path->data(), extension, map);
    };

    // If orig is provided, check if the given Unit has the same bcSha1
    // as it.
    auto const sameBC = [&] (Unit* unit) {
      auto const reuse =
        orig && orig->cu.unit && unit &&
        unit->bcSha1() == orig->cu.unit->bcSha1();
      if (reuse) s_unitReuseBytecode->increment();
      return reuse;
    };

    auto const makeCachedFilePtr = [&] (Unit* unit) {
      return CachedFilePtr{
        CachedUnit { unit, unit ? rds::allocBit() : -1 },
        statInfo,
        options
      };
    };

    if (Cfg::Eval::ReuseUnitsByHash) {
      // We're re-using Units according to their hash:

      auto const cachedFilePtr = [&] {
        tracing::BlockNoTrace _{"unit-hash-cache"};

        UnitByHashCache::const_accessor acc;
        s_unitByHashCache.insert(acc, loader.sha1());

        auto& cached = acc->second.unit;

        auto const hit = [&] (Unit* unit) {
          assertx(unit->sha1() == loader.sha1());
          assertx(unit->hasPerRequestFilepath());
          // Try to re-use the old Unit if we can:
          if (sameBC(unit)) return CachedFilePtr{orig->cu, statInfo, options};
          if (forPrefetch || canBeBoundToPath(unit, path)) {
            // We can bind the path so it can be used.
            return makeCachedFilePtr(unit);
          }
          // Otherwise the Unit has an already bound (and incompatible)
          // filepath. We'll just create a new Unit instead.
          return CachedFilePtr{};
        };

        auto const checkDeps = [&] (Unit* unit) -> Unit* {
          for (; unit; unit = unit->nextCachedByHash()) {
            if (!anyDepsChanged(unit, options, wrapper)) return unit;
          }
          return nullptr;
        };

        // First check before acquiring the lock
        if (auto const unit = checkDeps(cached.copy())) return hit(unit);

        // No entry, so acquire the lock:
        auto lock = cached.try_lock_for_update();
        if (!lock) {
          tracing::BlockNoTrace _{"unit-hash-cache-lock-acquire"};
          lock.emplace(cached.lock_for_update());
        }

        // Try again now that we have the lock
        if (auto const unit = checkDeps(cached.copy())) {
          // NB: Its safe to unlock first. The Unit can only be freed by
          // releaseFromHashCache() which acquires an exclusive lock on
          // this table slot first (so cannot happen concurrently).
          lock->release();
          return hit(unit);
        }

        // There's no Unit, compile a new one and store it in the cache.
        auto unit = compileNew();
        if (!unit) {
          lock->release();
          return makeCachedFilePtr(nullptr);
        }
        assertx(unit->sha1() == loader.sha1());
        assertx(!unit->hasCacheRef());
        assertx(!unit->hasPerRequestFilepath());

        // Try to re-use the original Unit if possible
        if (sameBC(unit)) {
          lock->release();
          if (releaseUnit) *releaseUnit = unit;
          else delete unit;
          return CachedFilePtr{orig->cu, statInfo, options};
        }

        // For things like HHAS files, the filepath in the Unit may not
        // match what we requested during compilation. In that case we
        // can't use per-request filepaths because the filepath of the
        // Unit has no relation to anything else. Units without
        // per-request filepaths cannot be stored in this cache.
        assertx(unit->origFilepath()->isStatic());
        if (unit->origFilepath() != path) {
          lock->release();
          return makeCachedFilePtr(unit);
        }
        unit->makeFilepathPerRequest();
        unit->setNextCachedByHash(cached.copy());

        // We make a copy of the ptr so we can move *that* into the cache. If,
        // in the future, `unit` became a smart pointer of some kind, we'd
        // (hopefully) get an error on this line, or avoid an error later when
        // moving the ptr.
        auto unit_ptr_copy = unit;
        // Store the Unit in the cache.
        auto const DEBUG_ONLY old = lock->update(std::move(unit_ptr_copy));
        assertx(!old || !checkDeps(old));
        assertx(old == unit->nextCachedByHash());
        return makeCachedFilePtr(unit);
      }();
      if (cachedFilePtr) {
        if (!orig || cachedFilePtr->cu.unit != orig->cu.unit) {
          flags = FileLoadFlags::kEvicted;
        }
        return cachedFilePtr;
      }
    }

    // We're not reusing Units by hash, or one existed but already had a
    // bound path. Compile a new Unit.
    auto const unit = compileNew();
    if (sameBC(unit)) {
      // If the new Unit has the same bcSha1 as the old one, just re-use
      // the old one. This saves any JIT work we've already done on the
      // orig Unit.
      if (releaseUnit) *releaseUnit = unit;
      else delete unit;
      return CachedFilePtr{orig->cu, statInfo, options};
    }
    flags = FileLoadFlags::kEvicted;
    assertx(!unit || !unit->hasCacheRef());
    assertx(!unit || !unit->hasPerRequestFilepath());
    return makeCachedFilePtr(unit);
  };

  // Loading the contents lazily can fail (if the contents of the file
  // changes after obtaining the hash). So, try loading lazily a fixed
  // number of times. If we exceed it, give up and load the file
  // contents eagerly (this should basically never happen).
  auto attempts = 0;
  while (true) {
    try {
      return impl(attempts < LazyUnitContentsLoader::kMaxLazyAttempts);
    } catch (const LazyUnitContentsLoader::LoadError&) {
      return CachedFilePtr{};
    } catch (const LazyUnitContentsLoader::Inconsistency&) {
      assertx(attempts < LazyUnitContentsLoader::kMaxLazyAttempts);
      s_unitEdenInconsistencies->increment();
    }
    ++attempts;
  }
}

// When running via the CLI server special access checks may need to be
// performed, and in the event that the server is unable to load the file an
// alternative per client cache may be used.
std::pair<NonRepoUnitCache*, Stream::Wrapper*>
getNonRepoCacheWithWrapper(const StringData* rpath) {
  // If this isn't a CLI server request, this is a normal file access
  if (!is_cli_server_mode()) {
    return std::make_pair(&s_nonRepoUnitCache, nullptr);
  }

  // If the server cannot access rpath attempt to open the unit on the client
  if (access(rpath->data(), R_OK) == -1) {
    auto wrapper = Stream::getWrapperFromURI(StrNR{rpath});
    if (!wrapper || !wrapper->isNormalFileStream()) {
      return std::make_pair(nullptr, nullptr);
    }
    return std::make_pair(&s_nonRepoUnitCache, wrapper);
  }

  // When the server is able to read the file prefer to access it that way,
  // in all modes units loaded by the server are cached for all clients.
  return std::make_pair(&s_nonRepoUnitCache, nullptr);
}

FOLLY_NOINLINE const StringData* resolveRequestedPath(const StringData* requestedPath,
                                       bool alreadyRealpath) {
  auto const rpath = [&] (const StringData* p) {
    if (!Cfg::Eval::CheckSymLink || alreadyRealpath) {
      return makeStaticString(p);
    }
    std::string rp;
    rp = realpathLibc(p->data());
    return (rp.size() != 0 &&
            (rp.size() != p->size() || memcmp(rp.data(), p->data(), rp.size())))
      ? makeStaticString(rp)
      : makeStaticString(p);
  };

  // XXX: it seems weird we have to do this even though we already ran
  // resolveVmInclude.
  if (FileUtil::isAbsolutePath(requestedPath->slice())) {
    return rpath(requestedPath);
  }
  return rpath(
    (OptString{SourceRootInfo::GetCurrentSourceRoot()} +
     StrNR{requestedPath}).get()
  );
}

//////////////////////////////////////////////////////////////////////
// Unit prefetching

struct StaticStringCompare {
  bool equal(const StringData* s1, const StringData* s2) const {
    assertx(s1);
    assertx(s2);
    assertx(s1->isStatic());
    assertx(s2->isStatic());
    return s1 == s2;
  }
  size_t hash(const StringData* s) const {
    assertx(s);
    assertx(s->isStatic());
    return hash_int64(reinterpret_cast<uintptr_t>(s));
  }
};

// To avoid enqueueing multiple identical prefetch requests, we
// maintain a global table of the timestamp of the last prefetch
// request for each path. We'll only enqueue a new prefetch request
// for that path if more than some amount has passed since the last
// attempt. Note that we store paths (without normalization), and a
// given unit might be referred to by multiple paths. This is fine
// because it just means we'll do a bit of extra work.
//
// Note that even if the request goes through, we'd still realize the
// prefetch is unnecessary and drop it eventually, but this avoids a
// lot of extra work to get to that point. The downside is we might we
// miss an opportunity to prefetch a unit that changed shortly after
// the last attempt, but this should be uncommon.

tbb::concurrent_hash_map<
  const StringData*,
  std::chrono::steady_clock::time_point,
  StaticStringCompare
> s_prefetchTimestamps;

folly::CPUThreadPoolExecutor& getPrefetchExecutor() {
  assertx(!Cfg::Repo::Authoritative);
  assertx(unitPrefetchingEnabled());

  // Executor compatible thread factory which sets up the HPHP thread
  // state.
  struct PrefetcherThreadFactory : folly::NamedThreadFactory {
    using folly::NamedThreadFactory::NamedThreadFactory;

    std::thread newThread(folly::Func&& func) override {
      return folly::NamedThreadFactory::newThread(
        [func = std::move(func)] () mutable {
          hphp_thread_init();
          g_context.getCheck();
          SCOPE_EXIT { hphp_thread_exit(); };
          try {
            func();
          } catch (const std::exception& exn) {
            Logger::FError("Unit prefetching thread threw: {}", exn.what());
          } catch (...) {
            Logger::Error("Unit prefetching thread threw unknown exception");
          }
        }
      );
    }
  };

  // This will maintain a thread pool containing between
  // EvalUnitPrefetcherMinThreads and EvalUnitPrefetcherMaxThreads
  // threads. New threads are spun up to the max as long as there's
  // available work. Idle threads for longer than
  // EvalUnitPrefetcherIdleThreadTimeoutSecs will be reaped. The work
  // queue is unbounded, so it will always accept new work.
  struct PrefetcherExecutor : folly::CPUThreadPoolExecutor {
    PrefetcherExecutor()
      : folly::CPUThreadPoolExecutor(
          {Cfg::Eval::UnitPrefetcherMaxThreads,
           std::min(Cfg::Eval::UnitPrefetcherMinThreads,
                    Cfg::Eval::UnitPrefetcherMaxThreads)},
          std::make_unique<
            folly::PriorityUnboundedBlockingQueue<
              folly::CPUThreadPoolExecutor::CPUTask
            >
          >(3),
          std::make_shared<PrefetcherThreadFactory>("UnitPrefetchPool")
        )
    {
      setThreadDeathTimeout(
        std::chrono::seconds{Cfg::Eval::UnitPrefetcherIdleThreadTimeoutSecs}
      );
    }
  };
  static PrefetcherExecutor e;
  return e;
}

// Given a set of symbols, attempt to prefetch any units which are
// known to define those symbols (determined by the autoloader). You
// can optionally provide any Unit which is currently being loaded,
// which will ignore any symbols defined in that unit.
void prefetchSymbolRefs(SymbolRefs symbols, const Unit* loadingUnit) {
  if (!speculativeUnitPrefetchingEnabled()) return;
  assertx(!Cfg::Repo::Authoritative);

  // If there's no autoloader associated with this request, we can't
  // resolve the symbols, so there's nothing to do.
  if (!AutoloadHandler::s_instance->getAutoloadMap()) return;

  tracing::BlockNoTrace _{"prefetch-symbol-refs"};

  // Map all the symbols into paths from the autoloader. Note that
  // these paths may not be canonical. prefetchUnit() will deal with
  // that. The paths are static so we can use pointer equality.
  hphp_fast_set<StringData*> paths;

  auto const resolve = [&]
    (auto const& names, AutoloadMap::KindOf k) {
    for (auto const& name : names) {
      // Lookup the path in the maps that the autoloader
      // provides. Note that this won't succeed if the autoloader
      // defines the symbol via its "failure" function.
      if (auto const pathRes =
          AutoloadHandler::s_instance->getFile(StrNR{name}, k)) {
        paths.insert(makeStaticString(pathRes->m_path));
      }
    }
  };

  for (auto const& sym : symbols) {
    switch (sym.first) {
      case SymbolRef::Class:
        resolve(sym.second, AutoloadMap::KindOf::Type);
        break;
      case SymbolRef::Function:
        resolve(sym.second, AutoloadMap::KindOf::Function);
        break;
      case SymbolRef::Constant:
        resolve(sym.second, AutoloadMap::KindOf::Constant);
        break;
      case SymbolRef::Include:
        break;
    }
  }

  for (auto const& p : paths) prefetchUnit(p, nullptr, loadingUnit);
}

//////////////////////////////////////////////////////////////////////

void logTearing(const CachedFilePtr& ptr) {
  if (g_context && g_context->m_requestStartForTearing &&
      ptr->cu.unit && !ptr->cu.unit->isSystemLib()) {
    auto const t = ptr->mtime;
    auto const s = *g_context->m_requestStartForTearing;
    constexpr uint64_t sec_to_ns = 1000000000;

    auto const skew = Cfg::Eval::RequestTearingSkewMicros * 1000;
    auto const skew_ns = skew % sec_to_ns;
    auto const skew_s = skew / sec_to_ns;

    if (s.tv_sec < t.tv_sec + skew_s ||
        (s.tv_sec == t.tv_sec + skew_s && s.tv_nsec < t.tv_nsec + skew_ns)) {
      auto const diff =
        (t.tv_sec - s.tv_sec) * sec_to_ns + (t.tv_nsec - s.tv_nsec);
      ptr->cu.unit->logTearing(diff);
    }
  }
}

CachedUnit loadUnitNonRepoAuth(const StringData* rpath,
                               NonRepoUnitCache& cache,
                               Stream::Wrapper* wrapper,
                               const struct stat& statInfo,
                               OptLog& ent,
                               const Extension* extension,
                               const RepoOptions& options,
                               FileLoadFlags& flags,
                               bool forPrefetch) {
  tracing::BlockNoTrace _{"load-unit-non-repo-auth"};

  LogTimer loadTime("load_ms", ent);

  rqtrace::EventGuard trace{"WRITE_UNIT"};

  // Freeing a unit while holding the tbb lock would cause a rank violation when
  // recycle-tc is enabled as reclaiming dead functions requires that the code
  // and metadata locks be acquired.
  Unit* releaseUnit = nullptr;
  SCOPE_EXIT { if (releaseUnit) delete releaseUnit; };

  NonRepoUnitCache::const_accessor rpathAcc;

  cache.insert(rpathAcc, rpath);
  auto& cachedUnit = rpathAcc->second.cachedUnit;

  // We've already checked the cache before calling this function,
  // so don't bother again before grabbing the lock.
  auto lock = cachedUnit.try_lock_for_update();
  if (!lock) {
    tracing::BlockNoTrace _{"unit-cache-lock-acquire"};
    lock.emplace(cachedUnit.lock_for_update());
  }

  auto forceNewUnit = false;
  auto changeRes = UnitChange::NONE;

  if (auto const tmp = cachedUnit.copy()) {
    changeRes = changeReason(tmp, statInfo, options, wrapper, false);
    if (changeRes == UnitChange::NONE) {
      if (forPrefetch || canBeBoundToPath(tmp, rpath)) {
        lock->release();
        flags = FileLoadFlags::kWaited;
        if (ent) ent->setStr("type", "cache_hit_writelock");
        logTearing(tmp);
        return tmp->cu;
      } else {
        // An unit exists, but is already bound to a different
        // path. We need to compile a new one.
        forceNewUnit = true;
      }
    }
    if (ent) ent->setStr("type", "cache_stale");
  } else {
    if (ent) ent->setStr("type", "cache_miss");
  }

  trace.finish();
  auto ptr = [&] {
    auto const map = [] () -> AutoloadMap* {
      if (!AutoloadHandler::s_instance) {
        // It is not safe to autoinit AutoloadHandler outside a normal
        // request.
        return nullptr;
      }
      return AutoloadHandler::s_instance->getAutoloadMap();
    }();
    auto orig = (Cfg::Eval::CheckUnitSHA1 && !forceNewUnit)
      ? cachedUnit.copy()
      : CachedFilePtr{};
    return createUnitFromFile(rpath, CodeSource::User,
                              wrapper, &releaseUnit, ent,
                              extension, map, options, flags,
                              statInfo, std::move(orig), forPrefetch,
                              changeRes);
  }();
  if (UNLIKELY(!ptr)) return CachedUnit{};

  // Don't cache the unit if it was created in response to an
  // internal error in ExternCompiler. Such units represent
  // transient events. The copy_ptr dtor will ensure the unit is
  // automatically treadmilled the Unit after the request ends.
  // Also don't cache the unit if we only created it due to the path
  // binding check above. We want to keep the original unit in the
  // cache for that case.
  if (UNLIKELY(forceNewUnit || !ptr->cu.unit || ptr->cu.unit->isICE())) {
    return ptr->cu;
  }

  assertx(cachedUnit.copy() != ptr);
  logTearing(ptr);

  // Otherwise update the entry. Defer the destruction of the
  // old copy_ptr using the Treadmill. Other threads may be
  // reading the entry simultaneously so the ref-count cannot
  // drop to zero here.
  auto const cu = ptr->cu;
  if (auto old = lock->update(std::move(ptr))) {
    // We don't need to do anything explicitly; the copy_ptr
    // destructor will take care of it.
    Treadmill::enqueue([o = std::move(old)] {});
  }
  return cu;
}

FOLLY_NOINLINE int resolveStat(const std::string& path, struct stat* buf) {
  return statSyscall(path, buf);
}

CachedUnit lookupUnitNonRepoAuth(const StringData* requestedPath,
                                 struct stat& statInfo,
                                 OptLog& ent,
                                 const Extension* extension,
                                 FileLoadFlags& flags,
                                 bool alreadyRealpath,
                                 bool forPrefetch,
                                 bool forAutoload) {
  tracing::BlockNoTrace _{"lookup-unit-non-repo-auth"};

  // Shouldn't be getting systemlib units here
  assertx(strncmp(requestedPath->data(), "/:", 2));

  auto const rpath = resolveRequestedPath(requestedPath, alreadyRealpath);
  assertx(rpath->isStatic());

  auto const& options = RepoOptions::forFile(rpath->data());
  if (UNLIKELY(!Cfg::Eval::LoadFilepathFromUnitCache)) {
    g_context->onLoadWithOptions(requestedPath->data(), options);
  }

  if (Cfg::Eval::EnableDecl || Cfg::Eval::AutoloadInitEarly) {
    // Initialize AutoloadHandler before we parse the file so HhvmDeclProvider
    // can respond to queries from HackC.
    AutoloadHandler::s_instance.getCheck();
  }

  auto [cache, wrapper] = getNonRepoCacheWithWrapper(rpath);
  if (!cache) return CachedUnit{};
  assertx(!wrapper || wrapper->isNormalFileStream());

  auto cu = [&, cache = cache, wrapper = wrapper] {
    {
      // Steady state, its probably already in the cache. Try that first
      rqtrace::EventGuard trace{"READ_UNIT"};
      NonRepoUnitCache::const_accessor acc;
      if (cache->find(acc, rpath)) {
        auto const cachedUnit = acc->second.cachedUnit.copy();
        if (changeReason(cachedUnit, statInfo, options, wrapper, forAutoload) ==
            UnitChange::NONE) {
          if (forPrefetch || canBeBoundToPath(cachedUnit, rpath)) {
            if (ent) ent->setStr("type", "cache_hit_readlock");
            flags = FileLoadFlags::kHitMem;
            logTearing(cachedUnit);

            // When we store the unit in ExecutionContext we will want to know
            // its mtime which we haven't gotten from stat() yet.
            if (forAutoload) {
              statInfo.st_mtim = cachedUnit->mtime;
            }
            return cachedUnit->cu;
          }
        }
      }
    }

    // If we missed in the cache it may be because the unit was marked dirty,
    // we need to recheck it, and repopulate the cache with the most current
    // hash and stat() data.
    if (forAutoload) {
      if (resolveStat(rpath->data(), &statInfo) == -1 ||
          S_ISDIR(statInfo.st_mode)) {
        return CachedUnit{};
      }
    }

    // Not in the cache, attempt to load it
    return loadUnitNonRepoAuth(
      rpath, *cache, wrapper, statInfo,
      ent, extension,
      options, flags, forPrefetch
    );
  }();

  if (UNLIKELY(Cfg::Eval::LoadFilepathFromUnitCache && cu.unit)) {
    auto const origFileoptions = RepoOptions::forFile(
      cu.unit->origFilepath()->data()
    );
    g_context->onLoadWithOptions(requestedPath->data(), origFileoptions);
  }

  if (cu.unit) {
    if (Cfg::Eval::IdleUnitTimeoutSecs > 0 &&
        !forPrefetch) {
      // Mark this Unit as being used by this request. This will keep
      // the Unit from being reaped until this request ends. We defer
      // updating the timestamp on the Unit until this request ends
      // (the expiration time should be measured from the end of the
      // last request which used it).
      cu.unit->setLastTouchRequest(Treadmill::getRequestStartTime());
      g_context->m_touchedUnits.emplace(cu.unit);
    }

    if (Cfg::Eval::ReuseUnitsByHash &&
        !forPrefetch &&
        cu.unit->hasPerRequestFilepath()) {
      // If this isn't for a prefetch, we're going to actively use
      // this Unit, so bind it to the requested path. If there's a
      // path already bound, it had better be the requested one (we
      // should have created a new Unit otherwise).
      if (auto const DEBUG_ONLY p = cu.unit->perRequestFilepath()) {
        assertx(p == rpath);
      } else {
        cu.unit->bindPerRequestFilepath(rpath);
      }
    }

    // Check if this unit has any symbol refs. If so, atomically claim
    // them and attempt to prefetch units using the symbols. Only one
    // thread will claim the refs, so this will only be done once per
    // unit.
    if (auto symbols = cu.unit->claimSymbolRefsForPrefetch()) {
      prefetchSymbolRefs(std::move(*symbols), cu.unit);
    }
  }

  return cu;
}

//////////////////////////////////////////////////////////////////////
// resolveVmInclude callbacks

struct ResolveIncludeContext {
  struct stat* s; // stat for the file
  bool allow_dir; // return true for dirs?
  OptString path;    // translated path of the file
};

bool findFile(const StringData* path, struct stat* s, bool allow_dir,
              Stream::Wrapper* w) {
  // We rely on this side-effect in RepoAuthoritative mode right now, since the
  // stat information is an output-param of resolveVmInclude, but we aren't
  // really going to call stat.
  s->st_mode = 0;

  if (Cfg::Repo::Authoritative) {
    return lookupUnitRepoAuth(path, nullptr).unit != nullptr;
  }

  if (statSyscall(path->data(), s) == 0) {
    // The call explicitly populates the struct for dirs, but returns
    // false for them because it is geared toward file includes.
    return allow_dir || !S_ISDIR(s->st_mode);
  }

  if (w && (w != &s_file_stream_wrapper)) {
    // We only allow normal file streams, which cannot re-enter.
    assertx(w->isNormalFileStream());
    if (w->stat(StrNR(path), s) == 0) return allow_dir || !S_ISDIR(s->st_mode);
  }

  return false;
}

bool findFileWrapper(const OptString& file, void* ctx) {
  auto const context = static_cast<ResolveIncludeContext*>(ctx);
  assertx(context->path.isNull());

  auto wrapper = Stream::getWrapperFromURI(file);
  if (!wrapper || !wrapper->isNormalFileStream()) return false;

  // TranslatePath() will canonicalize the path and also check
  // whether the file is in an allowed directory.
  OptString translatedPath = File::TranslatePathKeepRelative(file);
  if (!FileUtil::isAbsolutePath(file.toCppString())) {
    if (findFile(translatedPath.get(), context->s, context->allow_dir,
                 wrapper)) {
      context->path = translatedPath;
      return true;
    }
    return false;
  }
  if (Cfg::Sandbox::Mode || !Cfg::Server::AlwaysUseRelativePath) {
    if (findFile(translatedPath.get(), context->s, context->allow_dir,
                 wrapper)) {
      context->path = translatedPath;
      return true;
    }
  }
  std::string server_root(SourceRootInfo::GetCurrentSourceRoot());
  if (server_root.empty()) {
    server_root = std::string(g_context->getCwd().data());
    if (server_root.empty() ||
        FileUtil::isDirSeparator(server_root[server_root.size() - 1])) {
      server_root += FileUtil::getDirSeparator();
    }
  }
  OptString rel_path(FileUtil::relativePath(server_root, translatedPath.data()));
  if (findFile(rel_path.get(), context->s, context->allow_dir,
               wrapper)) {
    context->path = rel_path;
    return true;
  }
  return false;
}

void logLoad(
  StructuredLogEntry& ent,
  const StringData* path,
  const char* cwd,
  OptString rpath,
  const CachedUnit& cu
) {
  ent.setStr("include_path", path->data());
  ent.setStr("current_dir", cwd);
  ent.setStr("resolved_path", rpath.data());
  if (auto const u = cu.unit) {
    if (auto const info = u->getFatalInfo()) {
      auto const parse = info->m_fatalOp == FatalOp::Parse;
      ent.setStr("result", parse ? "parse_fatal" : "compile_fatal");
      ent.setStr("error", info->m_fatalMsg);
    } else {
      ent.setStr("result", "success");
    }
    ent.setStr("sha1", u->sha1().toString());
    ent.setStr("repo_sn", folly::to<std::string>(u->sn()));

    int bclen = 0;
    u->forEachFunc([&](auto const& func) {
      bclen += func->bclen();
      return false;
    });

    ent.setInt("bc_len", bclen);
    ent.setInt("num_litstrs", u->numLitstrs());
    ent.setInt("num_funcs", u->funcs().size());
    ent.setInt("num_classes", u->preclasses().size());
    ent.setInt("num_type_aliases", u->typeAliases().size());
  } else {
    ent.setStr("result", "file_not_found");
  }

  switch (rl_typeProfileLocals->requestKind) {
  case RequestKind::Warmup: ent.setStr("request_kind", "warmup"); break;
  case RequestKind::Standard: ent.setStr("request_kind", "standard"); break;
  case RequestKind::NonVM: ent.setStr("request_kind", "nonVM"); break;
  }
  ent.setInt("request_count", requestCount());

  StructuredLog::log("hhvm_unit_cache", ent);
}

//////////////////////////////////////////////////////////////////////

char mangleExtension(const folly::StringPiece fileName) {
  if (fileName.endsWith(".hack")) return '0';
  if (fileName.endsWith(".hackpartial")) return '1';
  if (fileName.endsWith(".php")) return '2';
  if (fileName.endsWith(".hhas")) return '3';
  return '4'; // other files
}

//////////////////////////////////////////////////////////////////////

} // end empty namespace

//////////////////////////////////////////////////////////////////////

std::string mangleUnitSha1(const folly::StringPiece fileSha1,
                           const folly::StringPiece fileName,
                           const RepoOptionsFlags& opts,
                           const UnitEmitterAttributes& attributes) {
  return string_sha1(
    folly::to<std::string>(
      fileSha1, '|',
      repoSchemaId().toString(),
#define C(Config, ...) Config, '|',
      CONFIGS_FOR_UNITCACHEFLAGS()
#undef C
      CoeffectsConfig::mangle(),
      opts.cacheKeySha1().toString(),
      '|', attributes.mangle(),
      '|',
      mangleExtension(fileName)
    )
  );
}

Optional<SHA1> getHashForFile(const std::string& path,
                              const std::filesystem::path& root) {
  auto wrapper = Stream::getWrapperFromURI(OptString{root.string()});
  return getHashForFile(path, wrapper, root);
}

Optional<std::string> getHashFromEden(const char* path,
                                      Stream::Wrapper* wrapper) {
  assertx(Cfg::Eval::UseEdenFS);
  assertx(path);
  if (wrapper) {
    // We only allow normal file streams, which cannot re-enter
    assertx(wrapper->isNormalFileStream());
    auto const xattr = wrapper->getxattr(path, "user.sha1");
    if (!xattr || xattr->size() != SHA1::kStrLen) return std::nullopt;
    return xattr;
  }
  char xattr_buf[SHA1::kStrLen];
  auto const ret = getxattr(path, "user.sha1", xattr_buf, sizeof(xattr_buf));
  if (ret != sizeof(xattr_buf)) return std::nullopt;
  return std::string{xattr_buf, sizeof(xattr_buf)};
}

Optional<std::string> getDigestFromEden(const char* path,
                                        Stream::Wrapper* wrapper) {
  assertx(Cfg::Eval::UseEdenFS);
  assertx(path);
  if (wrapper) {
    // We only allow normal file streams, which cannot re-enter
    assertx(wrapper->isNormalFileStream());
    auto const xattr = wrapper->getxattr(path, "user.digesthash");
    if (!xattr || xattr->size() != SHA1::kStrLen) return std::nullopt;
    return xattr;
  }
  // That's 64 bytes for the hash for you decimal peasants
  char xattr_buf[0x40];
  auto const ret = getxattr(path, "user.digesthash", xattr_buf, sizeof(xattr_buf));
  if (ret != sizeof(xattr_buf)) return std::nullopt;
  return std::string{xattr_buf, sizeof(xattr_buf)};
}


size_t numLoadedUnits() {
  if (Cfg::Repo::Authoritative) return s_repoUnitCache.size();
  return s_nonRepoUnitCache.size();
}

Unit* getLoadedUnit(StringData* path) {
  if (!Cfg::Repo::Authoritative) {
    auto const rpath = resolveRequestedPath(path, false);
    NonRepoUnitCache::const_accessor accessor;
    if (s_nonRepoUnitCache.find(accessor, rpath) ) {
      auto cachedUnit = accessor->second.cachedUnit.copy();
      return cachedUnit ? cachedUnit->cu.unit : nullptr;
    }
  }
  return nullptr;
}

std::vector<Unit*> loadedUnitsRepoAuth() {
  always_assert(Cfg::Repo::Authoritative);
  std::vector<Unit*> units;
  units.reserve(s_repoUnitCache.size());
  for (auto const& elm : s_repoUnitCache) {
    if (auto const unit = elm.second.unit.copy()) {
      if (unit != CachedUnitInternal::Uninit) {
        units.push_back(unit);
      }
    }
  }
  return units;
}

std::vector<Unit*> loadedUnitsNonRepoAuth() {
  always_assert(!Cfg::Repo::Authoritative);
  std::vector<Unit*> units;
  units.reserve(s_nonRepoUnitCache.size());
  for (auto const& elm : s_nonRepoUnitCache) {
    auto const cachedUnit = elm.second.cachedUnit.copy();
    if (cachedUnit) {
      if (auto const unit = cachedUnit->cu.unit) {
        units.push_back(unit);
      }
    }
  }
  return units;
}

void invalidateUnit(StringData* path) {
  always_assert(!Cfg::Repo::Authoritative);

  path = makeStaticString(path);

  auto const erase = [&] (NonRepoUnitCache& cache) {
    NonRepoUnitCache::accessor acc;
    if (cache.find(acc, path)) {
      // We can't just erase the entry here... there's a race where
      // another thread could have copied the copy_ptr out of the map,
      // but not yet inc-ref'd it. If we just erase the entry, we
      // could dec-ref it (and free it) before the other thread has a
      // chance to inc-ref it. We need to defer the dec-ref using the
      // treadmill. Manually move the copy_ptr onto the treadmill, and
      // then replace it with a null copy_ptr.
      auto& cached = acc->second.cachedUnit;
      auto lock = cached.lock_for_update();
      if (auto old = lock.update({})) {
        Treadmill::enqueue([o = std::move(old)] {});
      }
    }
  };
  erase(s_nonRepoUnitCache);
}

FOLLY_NOINLINE OptString resolveVmInclude(const StringData* path,
                        const char* currentDir,
                        struct stat* s,
                        bool allow_dir /* = false */) {
  ResolveIncludeContext ctx{s, allow_dir};
  resolve_include(StrNR{path}, currentDir, findFileWrapper, &ctx);
  // If resolve_include() could not find the file, return NULL
  return ctx.path;
}

Unit* lookupUnit(const StringData* path, const char* currentDir,
                 bool* initial_opt, const Extension* extension,
                 bool alreadyRealpath, bool forPrefetch, bool forAutoload) {
  return lookupUnit(path, nullptr, currentDir, initial_opt, extension,
                    alreadyRealpath, forPrefetch, forAutoload);
}

Unit* lookupUnit(const StringData* path, const RepoUnitInfo* info,
                 const char* currentDir, bool* initial_opt,
                 const Extension* extension, bool alreadyRealpath,
                 bool forPrefetch, bool forAutoload) {
  bool init;
  bool& initial = initial_opt ? *initial_opt : init;
  initial = true;

  tracing::BlockNoTrace _{"lookup-unit"};

  OptLog ent;
  if (!Cfg::Repo::Authoritative &&
      StructuredLog::coinflip(Cfg::Eval::LogUnitLoadRate)) {
    ent.emplace();
  }

  rqtrace::ScopeGuard trace{"LOOKUP_UNIT"};
  trace.annotate("path", path->data());
  trace.annotate("pwd", currentDir);

  LogTimer lookupTimer("lookup_ms", ent);

  // We can only trust hits without stat() if we know that the autoloader was
  // able to successfully sync this request.
  forAutoload &= *rl_trustAutoloadedUnits;

  struct stat s;
  OptString spath;
  if (info) {
    assertx(!path->empty());
    assertx(path->data()[0] == '/');
    assertx(path->isStatic());
    spath = StrNR(path);
  } else if (forAutoload) {
    spath = makeStaticString(path);
  } else {
    spath = resolveVmInclude(path, currentDir, &s);
    if (spath.isNull()) return nullptr;
  }

  auto const eContext = g_context.getNoCheck();

  if (!eContext->m_visitedFiles.isNull()) {
    eContext->m_visitedFiles.append(spath.asTypedValue());
  }

  // Check if this file has already been included.
  if (!forPrefetch) {
    auto it = eContext->m_evaledFiles.find(spath.get());
    if (it != eContext->m_evaledFiles.end()) {
      // In RepoAuthoritative mode we assume that the files are unchanged.
      initial = false;
      if (Cfg::Repo::Authoritative ||
          forAutoload ||
          (it->second.ts_sec > s.st_mtime) ||
          ((it->second.ts_sec == s.st_mtime) &&
           (it->second.ts_nsec >= s.st_mtim.tv_nsec))) {
        return it->second.unit;
      }
    }
  }

  FileLoadFlags flags = FileLoadFlags::kHitMem;

  // This file hasn't been included yet, so we need to parse the file
  auto const cunit = Cfg::Repo::Authoritative
    ? lookupUnitRepoAuth(spath.get(), info)
    : lookupUnitNonRepoAuth(spath.get(), s, ent, extension, flags,
                            alreadyRealpath, forPrefetch, forAutoload);

  if (cunit.unit && initial_opt) {
    // if initial_opt is not set, this shouldn't be recorded as a
    // per request fetch of the file.
    if (rds::testAndSetBit(cunit.rdsBitId)) {
      initial = false;
    }
    // if parsing was successful, update the mappings for spath and
    // rpath (if it exists).
    if (!forPrefetch) {
      eContext->m_evaledFilesOrder.push_back(cunit.unit->filepath());
      eContext->m_evaledFiles[spath.get()] =
        {cunit.unit, s.st_mtime, static_cast<unsigned long>(s.st_mtim.tv_nsec),
         flags};
      spath.get()->incRefCount();
      if (!cunit.unit->filepath()->same(spath.get())) {
        eContext->m_evaledFiles[cunit.unit->filepath()] =
          {cunit.unit, s.st_mtime, static_cast<unsigned long>(s.st_mtim.tv_nsec),
           FileLoadFlags::kDup};
      }
    }
    if (Cfg::Debugger::EnableVSDebugger || Cfg::Debugger::EnableHphpd) {
      eContext->m_loadedUnits.emplace(cunit.unit->filepath(), cunit.unit);
      DEBUGGER_ATTACHED_ONLY(phpDebuggerFileLoadHook(cunit.unit));
    }
  }

  lookupTimer.stop();
  if (ent) logLoad(*ent, path, currentDir, spath, cunit);
  return cunit.unit;
}

Unit* lookupSyslibUnit(StringData* path) {
  assertx(Cfg::Repo::Authoritative);
  return lookupUnitRepoAuth(path, nullptr).unit;
}

//////////////////////////////////////////////////////////////////////

void prefetchUnit(StringData* requestedPath,
                  std::shared_ptr<std::atomic<std::uint32_t>> gate,
                  const Unit* loadingUnit) {
  assertx(!Cfg::Repo::Authoritative);
  assertx(unitPrefetchingEnabled());
  assertx(requestedPath->isStatic());
  assertx(!loadingUnit || loadingUnit->filepath()->isStatic());

  // If the requested path is trivially identical to a Unit being
  // loaded (without normalization), we can just skip this.
  if (loadingUnit && requestedPath == loadingUnit->filepath()) return;

  tracing::BlockNoTrace _{"prefetch-unit"};

  // Otherwise check if we've prefetched this path already
  // lately. This is just an optimization.
  auto const prefetchedAlready = [&] {
    // Assume that if we have a gate, this is an explicit request
    // for prefetching (not done from the Unit loader), so skip this
    // optimization.
    if (gate) return false;

    // Grab the timestamp for the last prefetch from the map. If none
    // exists, we'll atomically insert it. In that case, the path has
    // never been prefetched.
    auto const now = std::chrono::steady_clock::now();
    decltype(s_prefetchTimestamps)::accessor acc;
    if (s_prefetchTimestamps.insert(acc, {requestedPath, now})) {
      return false;
    }
    // The path has been prefetched before. We need to check if the
    // last prefetch was more than 15 seconds in the past. If so,
    // we'll try again (and update the timestamp). Otherwise, we'll
    // forgo this attempt. The 15 second constant was chosen somewhat
    // arbitrarily.
    if (now >= acc->second + std::chrono::seconds{15}) {
      acc->second = now;
      return false;
    }
    return true;
  }();
  if (prefetchedAlready) return;

  // Perform all the work that needs request context. The worker
  // threads aren't a request, so this must be done here before it
  // gets queued into the worker pool:

  // Normally we need to run resolveVmInclude() in a request context,
  // as it might access per-request state. However this is relatively
  // expensive. If the below criteria are true, then
  // resolveVmInclude() will not require any request state and it can
  // be deferred to the worker thread.
  auto const deferResolveVmInclude =
    FileUtil::isAbsolutePath(requestedPath->slice()) &&
    !RID().hasSafeFileAccess() &&
    (Cfg::Sandbox::Mode || !Cfg::Server::AlwaysUseRelativePath);

  Optional<struct stat> fileStat;
  const StringData* path = nullptr;
  if (!deferResolveVmInclude) {
    // We can't safely defer resolveVmInclude(). Do it now.
    fileStat.emplace();
    auto const spath = resolveVmInclude(
      requestedPath,
      "",
      fileStat.get_pointer()
    );
    // File doesn't exist. Nothing to do.
    if (spath.isNull()) return;

    // Do the second round of path normalization. Resolving symlinks
    // is relatively expensive, but always can be deferred until the
    // work thread. Lie and say realpath has already been done to get
    // only the path canonicalization without symlink resolution.
    path = resolveRequestedPath(spath.get(), true);
    assertx(path->isStatic());
  } else {
    // Keep the path as is. We'll do all the normalization in the
    // worker thread.
    path = requestedPath;
  }

  // Now that the paths might be normalized, compare them again
  // against any loading unit, to see if we can short-circuit.
  if (loadingUnit && path == loadingUnit->filepath()) return;

  // We can only do prefetching if the file is accessible normally. We
  // can't support CLI wrappers because the request that its
  // associated with will be gone.
  auto [cache, wrapper] = getNonRepoCacheWithWrapper(path);
  if (!cache || (wrapper && !wrapper->isNormalFileStream())) return;

  // We're definitely going to enqueue this request. Bump the gate if
  // provided.
  if (gate) gate->fetch_add(1);

  tracing::BlockNoTrace _2{"prefetch-unit-enqueue"};

  // We can't share raw AutoloadMap* pointers across threads, some of them are
  // request local while others may be treadmilled. To ensure we hold a valid
  // reference to the map we construct a holder which may in some cases wrap
  // a std::shared_ptr.
  auto holder = [] () -> AutoloadMap::Holder {
    if (!AutoloadHandler::s_instance) {
      // It is not safe to autoinit AutoloadHandler outside a normal request.
      return {};
    }
    if (auto const ahm = AutoloadHandler::s_instance->getAutoloadMap()) {
      return ahm->getNativeHolder();
    }
    return {};
  }();

  // The rest of the work can be done in the worker thread. Enqueue
  // it.
  getPrefetchExecutor().addWithPriority(
    // NB: This lambda is executed at some later point in another
    // thread, so you need to be careful about the lifetime of what
    // you capture here.
    [path, fileStat, cache = cache,
     loadingUnitPath = loadingUnit ? loadingUnit->filepath() : nullptr,
     gate = std::move(gate), w = wrapper, map = std::move(holder)] () mutable {
      SCOPE_EXIT {
        // Decrement the gate whenever we're done. If the gate hits
        // zero, do a notification to wake up any thread waiting on
        // it.
        if (!gate) return;
        auto const count = gate->fetch_sub(1) - 1;
        if (count == 0) folly::atomic_notify_one(gate.get());
      };

      // We cannot delete Units at all points in the loading path (due
      // to potential lock rank violations). If necessary, we defer
      // the deletion until when we exit this function.
      Unit* releaseUnit = nullptr;
      SCOPE_EXIT { if (releaseUnit) delete releaseUnit; };

      // If we deferred resolveVmInclude(), do it now.
      if (!fileStat) {
        fileStat.emplace();
        auto const spath = resolveVmInclude(
          path,
          "",
          fileStat.get_pointer()
        );
        // File doesn't exist. Nothing to do.
        if (spath.isNull()) return;

        // We don't need resolveRequestedPath() here as the conditions
        // for deferring resolveVmInclude() mean it would be a nop.
        assertx(FileUtil::isAbsolutePath(spath.get()->slice()));
        path = makeStaticString(spath.get());
      }

      // Now do any required symlink resolution:
      auto const rpath = resolveRequestedPath(path, false);
      assertx(rpath->isStatic());

      // Now that the paths are fully normalized, compare them against
      // any loading unit path, to see if we can return without doing
      // anything.
      if (rpath == loadingUnitPath) return;

      auto const& options = RepoOptions::forFile(rpath->data());

      NonRepoUnitCache::const_accessor rpathAcc;

      cache->insert(rpathAcc, rpath);
      auto& cachedUnit = rpathAcc->second.cachedUnit;

      // NB: It might be tempting to check if the file has changed
      // before acquiring the lock. This opens up a race where the
      // Unit can be deleted out from under us. Once we acquire the
      // lock, no other thread can delete the Unit (until we release
      // it). This isn't an issue for the request threads because
      // the Unit is freed by the treadmill (so cannot go away
      // during the request). This thread is *not* a request thread
      // and therefore the treadmill can't save us.

      // Try to acquire the update lock for this unit. Don't
      // block. If we fail to acquire the lock, its because another
      // prefetcher thread, or a request thread is currently loading
      // the unit. In either case, we don't want to do anything with
      // it, and just move onto another request.
      auto lock = cachedUnit.try_lock_for_update();
      if (!lock) return;

      // Now that we have the lock, check if the path has a Unit
      // already, and if so, has the file has changed since that
      // Unit was created. If not, there's nothing to do.
      auto changeRes = UnitChange::NONE;
      if (auto const tmp = cachedUnit.copy()) {
        changeRes = changeReason(tmp, *fileStat, options, w, false);
        if (changeRes == UnitChange::NONE) return;
      }

      // The Unit doesn't already exist, or the file has
      // changed. Either way, we need to create a new Unit.
      auto ptr = [&] {
        auto orig = Cfg::Eval::CheckUnitSHA1
          ? cachedUnit.copy()
          : CachedFilePtr{};
        FileLoadFlags flags;
        OptLog optLog;
        return createUnitFromFile(rpath, CodeSource::User, nullptr, &releaseUnit,
                                  optLog, nullptr,
                                  map.get(),
                                  options, flags, *fileStat, std::move(orig),
                                  true, changeRes);
      }();

      // We don't want to prefetch ICE units (they can be
      // transient), so if we encounter one, just drop it and leave
      // the cached entry as is.
      if (!ptr || !ptr->cu.unit || ptr->cu.unit->isICE()) return;

      // The new Unit is good. Atomically update the cache entry
      // with it while releasing the lock.

      assertx(cachedUnit.copy() != ptr);

      // Otherwise update the entry. Defer the destruction of the
      // old copy_ptr using the Treadmill. Other threads may be
      // reading the entry simultaneously so the ref-count cannot
      // drop to zero here.
      if (auto old = lock->update(std::move(ptr))) {
        // We don't need to do anything explicitly; the copy_ptr
        // destructor will take care of it.
        Treadmill::enqueue([o = std::move(old)] {});
      }

      s_unitsPrefetched->increment();
    },
    // Use high priority for prefetch requests. Medium priority is
    // used for drain blocks. Low priority is used internally by the
    // executor to drain the queue during shutdown.
    folly::Executor::HI_PRI
  );
}

void unitCacheSetSync() {
  *rl_trustAutoloadedUnits = true;
}

void unitCacheClearSync() {
  *rl_trustAutoloadedUnits = false;
}

namespace {
bool dirtyUnit(StringData* path) {
  always_assert(!Cfg::Repo::Authoritative);
  assertx(path->isStatic());

  NonRepoUnitCache::const_accessor acc;
  if (s_nonRepoUnitCache.find(acc, path)) {
    if (auto const cu = acc->second.cachedUnit.copy()) {
      cu->dirty.store(true, std::memory_order_release);
      return true;
    }
  }
  return false;
}
}

void unitCacheSyncRepo(AutoloadMap* map,
                       std::filesystem::path& root,
                       std::vector<Facts::PathAndOptionalHash>& changed,
                       std::vector<std::filesystem::path>& deleted) {
  size_t removed = 0;
  std::vector<StringData*> updated;

  for (auto& path : deleted) {
    if (dirtyUnit(makeStaticString((root / path).native()))) removed++;
  }

  for (auto& info : changed) {
    auto const absPath = makeStaticString((root / info.m_path).native());
    if (dirtyUnit(absPath)) updated.emplace_back(absPath);
  }

  if (Cfg::Server::Mode &&
      (!deleted.empty() || !changed.empty() || !updated.empty())) {
    Logger::FInfo(
      "Synced {}: {} deleted; {} modified; {} evicted; {} to prefetch",
      root.native(),
      deleted.size(),
      changed.size(),
      updated.size() + removed,
      updated.size()
    );
  }

  if (updated.empty() ||
      !Cfg::Eval::AutoloadEagerReloadUnitCache ||
      !unitPrefetchingEnabled()) {
    return;
  }

  getPrefetchExecutor().addWithPriority(
    // NB: This lambda is executed at some later point in another
    // thread, so you need to be careful about the lifetime of what
    // you capture here.
    [updated = std::move(updated), map = map->getNativeHolder()] () mutable {
      // We cannot delete Units at all points in the loading path (due
      // to potential lock rank violations). If necessary, we defer
      // the deletion until when we exit this function.
      std::vector<Unit*> releaseUnits;
      std::vector<CachedFilePtr> oldUnits;
      size_t count = 0;
      SCOPE_EXIT {
        for (auto u : releaseUnits) delete u;

        // We don't need to do anything explicitly; the copy_ptr
        // destructor will take care of it.
        Treadmill::enqueue([v = std::move(oldUnits)] {});

        if (Cfg::Server::Mode) {
          Logger::FInfo("Prefetched {} modified units", count);
        }
      };

      const RepoOptions* options = nullptr;
      for (auto rpath : updated) {
        struct stat fileStat;
        if (statSyscall(rpath->data(), &fileStat) == -1) continue;
        if (S_ISDIR(fileStat.st_mode)) continue;
        if (!options) options = &RepoOptions::forFile(rpath->data());

        NonRepoUnitCache::const_accessor rpathAcc;

        s_nonRepoUnitCache.insert(rpathAcc, rpath);
        auto& cachedUnit = rpathAcc->second.cachedUnit;

        // NB: It might be tempting to check if the file has changed
        // before acquiring the lock. This opens up a race where the
        // Unit can be deleted out from under us. Once we acquire the
        // lock, no other thread can delete the Unit (until we release
        // it). This isn't an issue for the request threads because
        // the Unit is freed by the treadmill (so cannot go away
        // during the request). This thread is *not* a request thread
        // and therefore the treadmill can't save us.

        // Try to acquire the update lock for this unit. Don't
        // block. If we fail to acquire the lock, its because another
        // prefetcher thread, or a request thread is currently loading
        // the unit. In either case, we don't want to do anything with
        // it, and just move onto another request.
        auto lock = cachedUnit.try_lock_for_update();
        if (!lock) continue;

        // Now that we have the lock, check if the path has a Unit
        // already, and if so, has the file has changed since that
        // Unit was created. If not, there's nothing to do.
        auto changeRes = UnitChange::NONE;
        if (auto const tmp = cachedUnit.copy()) {
          changeRes = changeReason(tmp, fileStat, *options, nullptr, false);
          if (changeRes == UnitChange::NONE) continue;
        }

        // The Unit doesn't already exist, or the file has
        // changed. Either way, we need to create a new Unit.
        Unit* releaseUnit = nullptr;
        auto ptr = [&] {
          auto orig = Cfg::Eval::CheckUnitSHA1
            ? cachedUnit.copy()
            : CachedFilePtr{};
          FileLoadFlags flags;
          OptLog optLog;
          return createUnitFromFile(rpath, CodeSource::User, nullptr,
                                    &releaseUnit, optLog, nullptr,
                                    map.get(),
                                    *options, flags, fileStat, std::move(orig),
                                    true, changeRes);
        }();

        if (releaseUnit) releaseUnits.emplace_back(releaseUnit);

        // We don't want to prefetch ICE units (they can be
        // transient), so if we encounter one, just drop it and leave
        // the cached entry as is.
        if (!ptr || !ptr->cu.unit || ptr->cu.unit->isICE()) continue;

        // The new Unit is good. Atomically update the cache entry
        // with it while releasing the lock.

        assertx(cachedUnit.copy() != ptr);
        ++count;

        // Otherwise update the entry. Defer the destruction of the
        // old copy_ptr using the Treadmill. Other threads may be
        // reading the entry simultaneously so the ref-count cannot
        // drop to zero here.
        if (auto old = lock->update(std::move(ptr))) {
          oldUnits.emplace_back(std::move(old));
        }
      }
    },
    // Use high priority for prefetch requests. Medium priority is
    // used for drain blocks. Low priority is used internally by the
    // executor to drain the queue during shutdown.
    folly::Executor::HI_PRI
  );
}

void drainUnitPrefetcher() {
  // Enqueue a medium priority task which simply posts the
  // baton. Since prefetch requests are always enqueued with high
  // priority, this task will not run until there's no queued prefetch
  // requests (the executor always processes available higher priority
  // tasks before lower priority ones).
  folly::Baton baton;
  getPrefetchExecutor().addWithPriority(
    [&baton] { baton.post(); },
    folly::Executor::MID_PRI
  );
  baton.wait();
}

//////////////////////////////////////////////////////////////////////

namespace {

// Evaled units have a footprint in the TC and translation
// metadata. The applications we care about tend to have few, short,
// stereotyped evals, where the same code keeps getting eval'ed over
// and over again; so we keep around units for each eval'ed string, so
// that the TC space isn't wasted on each eval.

using EvaledUnitsMap = RankedCHM<
  const StringData*, // Must be static
  Unit*,
  StringDataHashCompare,
  RankEvaledUnits
>;

static EvaledUnitsMap s_evaledUnits;

}

Unit* compileEvalString(const StringData* code, const char* evalFilename) {
  auto const scode = makeStaticString(code);
  EvaledUnitsMap::accessor acc;
  if (s_evaledUnits.insert(acc, scode) || !acc->second) {
    auto const map = [] () -> AutoloadMap* {
      if (!AutoloadHandler::s_instance) {
        // It is not safe to autoinit AutoloadHandler outside a normal request.
        return nullptr;
      }
      return AutoloadHandler::s_instance->getAutoloadMap();
    }();
    acc->second = compile_string(
      scode->slice(),
      CodeSource::Eval,
      evalFilename,
      nullptr,
      map,
      g_context->getRepoOptionsForCurrentFrame()
    );
  }
  if (Cfg::Eval::IdleUnitTimeoutSecs > 0 && !Cfg::Repo::Authoritative) {
    // Mark this Unit like we do for normal Units
    acc->second->setLastTouchRequest(Treadmill::getRequestStartTime());
    g_context->m_touchedUnits.emplace(acc->second);
  }
  return acc->second;
}

//////////////////////////////////////////////////////////////////////

namespace {

/*
 * Unit Reaper
 *
 * This thread is responsible for removing "expired" Units from caches
 * (after which they will get deleted by the treadmill). This helps
 * long running server processes avoid wasting memory on old Units
 * which will never be used any longer. This is especially useful when
 * using symlinks and every change is considered a "new" Unit.
 *
 * For every Unit we keep two pieces of information. The first is the
 * newest request which used the Unit. The second is the latest
 * timestamp of when the Unit was last used. The timestamp is actually
 * updated only at the point when a using request ends (this keeps
 * from biasing against long running requests).
 *
 * The Eval.IdleUnitTimeoutSecs determines when a Unit is expired. If
 * a Unit has not been used by any currently running threads, and its
 * timestamp is more than Eval.IdleUnitTimeoutSecs in the past, it is
 * considered expired and will be removed.
 *
 * There is an additional config called Eval.IdleUnitMinThreshold. If
 * set, the Unit reaper will not reap more Units than needed to bring
 * the current set of Units below that threshold. IE: If the threshold
 * is 1000, and there's 500 non-expired units, and 700 expired units,
 * it will only reap 200 (instead of the full 700). This is useful to
 * avoid an idle server from reaping every Unit and keeping a base
 * "working set" in memory.
 *
 * The reaper runs at regular intervals and scans all the Unit caches
 * for expired units. The regular interval is either the Unit
 * expiration timeout, or 5 minutes (whichever is shorter). Running at
 * regular intervals is easier than trying to keep track of the "next
 * expiration" and avoids pathologies with wakingup to often and doing
 * little work. Since we expect the expiration timeout to be
 * configured long in practice, 5 minutes was a good upper limit to
 * allow for somewhat timely reclamation, without doing a lot of
 * needless work.
 */
struct UnitReaper {
  UnitReaper()
    : m_timeout{Cfg::Eval::IdleUnitTimeoutSecs}
    , m_interval{std::min<std::chrono::seconds>(m_timeout, kMaxInterval)}
    , m_thread{&UnitReaper::run, this}
  {}

  ~UnitReaper() {
    // Signal the thread to wake up and then wait for it to exit
    m_done = 1;
    folly::atomic_notify_one(&m_done);
    m_thread.join();
  }

private:

  using Clock = Unit::TouchClock;

  static constexpr const std::chrono::minutes kMaxInterval{5};

  // A Unit is considered expired if it has not been touched by any
  // running request (IE, its touched request is older than the oldest
  // running request), and its touch timestamp plus the expiration
  // time is before now.
  bool isExpired(Clock::time_point now,
                 Treadmill::Clock::time_point oldestRequest,
                 Treadmill::Clock::time_point lastRequest,
                 Clock::time_point lastTime) const {
    assertx(oldestRequest != Treadmill::kNoStartTime);
    if (lastRequest >= oldestRequest) return false;
    if (now < lastTime + m_timeout) return false;
    return true;
  }

  void reapPathCaches(Clock::time_point now,
                      Treadmill::Clock::time_point oldestRequest) {
    auto const threshold = Cfg::Eval::IdleUnitMinThreshold;

    // Do a quick check if the total size of the caches is below the
    // threshold. If so, we know there's nothing to do.
    if (s_nonRepoUnitCache.size() <= threshold) return;

    // We might need to reap something. Loop over all of the caches
    // and look for expired Units. Record any that we find.
    struct Expired {
      NonRepoUnitCache* cache;
      const StringData* path;
      Clock::time_point time;
    };
    std::vector<Expired> expired;
    size_t nonExpired = 0;

    [&](NonRepoUnitCache& cache) {
      // Iterating over the caches is safe because we never remove any
      // entries. We might, however, skip entries or visit an entry
      // more than once. Skipping is fine, we'll just miss a
      // potentially expired Unit (we'll catch it again next
      // time). Visiting more than once will be dealt with below.
      for (auto const& p : cache) {
        assertx(p.first->isStatic());
        auto const cached = p.second.cachedUnit.copy();
        if (!cached || !cached->cu.unit) continue;
        auto const [lastRequest, lastTime] = cached->cu.unit->getLastTouch();
        if (!isExpired(now, oldestRequest, lastRequest, lastTime)) {
          ++nonExpired;
          continue;
        }
        expired.emplace_back(Expired{&cache, p.first, lastTime});
      }
    }(s_nonRepoUnitCache);

    // Nothing is expired. We're done.
    if (expired.empty()) return;

    // As mentioned above, we might have visited an entry more than
    // once. Remove any duplicates.
    std::sort(
      expired.begin(), expired.end(),
      [] (const Expired& a, const Expired& b) { return a.path < b.path; }
    );
    expired.erase(
      std::unique(
        expired.begin(), expired.end(),
        [] (const Expired& a, const Expired& b) { return a.path == b.path; }
      ),
      expired.end()
    );

    // Check how many we want to actually reap. Non-expired Units
    // consume the threshold first, and only then expired Units.
    auto const toReap =
      expired.size() -
      std::min<size_t>(
        threshold - std::min<uint32_t>(nonExpired, threshold),
        expired.size()
      );
    // We can keep everything.
    if (!toReap) return;

    // If we don't have to reap everything, we need to decide which
    // ones we want to actually keep. Sort the expired Units by their
    // timestamp (we want to preferentially reap older Units). Since
    // ties can be common with timestamps (since they are set at the
    // end of the request), use path to break ties.
    if (toReap < expired.size()) {
      std::sort(
        expired.begin(), expired.end(),
        [] (const Expired& a, const Expired& b) {
          if (a.time < b.time) return true;
          if (a.time > b.time) return false;
          return a.path->compare(b.path) < 0;
        }
      );
      // Only keep the oldest ones.
      expired.erase(expired.begin() + toReap, expired.end());
    }

    // Do the actual reaping:
    for (auto const& e : expired) {
      assertx(e.path->isStatic());

      // The entry better be here since we never remove them and we
      // saw it before.
      NonRepoUnitCache::const_accessor accessor;
      always_assert(e.cache->find(accessor, e.path));

      // Lock the entry. NB: this entry may have been changed since we
      // iterated over it. It may not even have the same Unit in it.
      auto& cachedUnit = accessor->second.cachedUnit;
      auto lock = cachedUnit.lock_for_update();

      // The Unit could have been touched (or there could be a
      // different Unit in it), so redo the expiration check. If its
      // not actually expired now, skip it.
      auto const cached = cachedUnit.copy();
      if (!cached || !cached->cu.unit) continue;

      auto const [lastRequest, lastTime] = cached->cu.unit->getLastTouch();
      if (!isExpired(now, oldestRequest, lastRequest, lastTime)) continue;

      // Still expired. Replace the entry with a nullptr and put the
      // Unit on the treadmill to be deleted.
      if (auto old = lock.update({})) {
        Treadmill::enqueue([o = std::move(old)] {});
        s_unitsPathReaped->increment();
      }
    }
  }

  // Reap the evaled Unit cache. Only the path Unit caches, we don't
  // keep any threshold here.
  void reapEvalCaches(Clock::time_point now,
                      Treadmill::Clock::time_point oldestRequest) {
    if (s_evaledUnits.empty()) return;

    // Iterate over the cache. This is safe because we never delete
    // anything from it. We may, however, get duplicates or skip
    // elements. These are both fine. Store all the keys we encounter
    // and process them below.
    hphp_fast_set<const StringData*> codes;
    for (auto const& p : s_evaledUnits) {
      if (!p.second) continue;
      assertx(p.first->isStatic());
      codes.emplace(p.first);
    }

    // For every key encountered, check if the Unit is expired and if
    // so, remove it.
    for (auto const& c : codes) {
      // We grab an exclusive accessor, preventing anyone else from
      // touching this entry.
      EvaledUnitsMap::accessor accessor;
      // We should always find this entry because we just got the key
      // from it (and we don't remove entries).
      always_assert(s_evaledUnits.find(accessor, c));
      // Check if the Unit is expired. If it is, null out the entry
      // (but don't remove it), and put the Unit on the treadmill to
      // be deleted.
      auto const unit = accessor->second;
      auto const [lastRequest, lastTime] = unit->getLastTouch();
      if (!isExpired(now, oldestRequest, lastRequest, lastTime)) continue;
      accessor->second = nullptr;
      unit->startDestroy();
      s_unitsEvalReaped->increment();
    }
  }

  void reap(Unit::TouchClock::time_point now) {
    // The reaper runs as a request, to lock the treadmill and keep
    // things from being deleted out under us.
    HphpSession _{Treadmill::SessionKind::UnitReaper};

    auto const oldestRequest = Treadmill::getOldestRequestStartTime();
    // Since this is a request, we should always have an oldest
    // request (maybe ourself).
    assertx(oldestRequest != Treadmill::kNoStartTime);

    reapPathCaches(now, oldestRequest);
    reapEvalCaches(now, oldestRequest);
  }

  void run() {
    assertx(Cfg::Eval::IdleUnitTimeoutSecs > 0);
    assertx(!Cfg::Repo::Authoritative);

    hphp_thread_init();
    SCOPE_EXIT { hphp_thread_exit(); };
    folly::setThreadName("unit-reaper");

    // Since no Units should exist when we start up, we can't have any
    // expired Units until at least the full timeout period.
    auto nextWakeup = Clock::now() + m_timeout + m_interval;
    while (!m_done) {
      folly::atomic_wait_until(&m_done, 1u, nextWakeup);
      if (m_done) break; // Shut down thread
      auto const now = Clock::now();
      // Check for spurious wakeups:
      if (now < nextWakeup) continue;
      // We timed out, so run the reaper. Schedule to run it again at
      // the next interval.
      reap(now);
      nextWakeup = now + m_interval;
    }
  }

  // How long until a Unit is considered expired
  const std::chrono::seconds m_timeout;
  // How often do we run the reaper? This is typically less than the
  // full expiration period.
  const std::chrono::seconds m_interval;

  // Flag to mark that the thread should shutdown
  std::atomic<std::uint32_t> m_done{0};
  std::thread m_thread;
};

UnitReaper* s_unit_reaper = nullptr;

InitFiniNode s_unit_reaper_init(
  [] {
    assertx(!s_unit_reaper);
    if (Cfg::Eval::IdleUnitTimeoutSecs > 0 && !Cfg::Repo::Authoritative) {
      s_unit_reaper = new UnitReaper();
    }
  },
  InitFiniNode::When::ProcessInit
);

}

// This could be done with an InitFiniNode, but it has to be done
// before moduleShutdown().
void shutdownUnitReaper() {
  if (!s_unit_reaper) return;
  assertx(Cfg::Eval::IdleUnitTimeoutSecs > 0);
  assertx(!Cfg::Repo::Authoritative);
  delete s_unit_reaper;
  s_unit_reaper = nullptr;
}

//////////////////////////////////////////////////////////////////////

namespace {

ServiceData::CounterCallback s_counters(
  [](ServiceData::CounterMap& counters) {
    counters["vm.path-unit-cache-size"] = numLoadedUnits();
    counters["vm.hash-unit-cache-size"] = s_unitByHashCache.size();
    counters["vm.eval-unit-cache-size"] = s_evaledUnits.size();
    counters["vm.live-units"] = Unit::liveUnitCount();
    counters["vm.created-units"] = Unit::createdUnitCount();
  },
  "vm."
);

}

//////////////////////////////////////////////////////////////////////

void clearUnitCacheForExit() {
  s_nonRepoUnitCache.clear();
  s_repoUnitCache.clear();
}

void shutdownUnitPrefetcher() {
  if (Cfg::Repo::Authoritative || !unitPrefetchingEnabled()) return;
  getPrefetchExecutor().join();
}

//////////////////////////////////////////////////////////////////////

LazyUnitContentsLoader::LazyUnitContentsLoader(const char* path,
                                               Stream::Wrapper* wrapper,
                                               const RepoOptions& options,
                                               size_t fileLength,
                                               bool forceEager)
  : m_path{path}
  , m_wrapper{wrapper}
  , m_options{options.flags()}
  , m_file_length{fileLength}
  , m_repo{options.dir()}
  , m_loaded{false}
{
  assertx(m_path);
  m_unitEmitterAttributes =
    UnitEmitterAttributes::forAbsolutePath(m_path, options);

  auto const file_hash_str = [&] {
    // If there's no emitter cache hook, we're always going to have to
    // read the file contents, so there's no point in deferring.
    if (!forceEager && g_unit_emitter_cache_hook && Cfg::Eval::UseEdenFS) {
      if (auto const h = getHashFromEden(m_path, m_wrapper)) return *h;
    }
    load();
    return string_sha1(m_contents.slice());
  }();

  m_file_hash = SHA1{file_hash_str};
  m_hash = SHA1{mangleUnitSha1(
    file_hash_str,
    m_path,
    m_options,
    m_unitEmitterAttributes
  )};
}

LazyUnitContentsLoader::LazyUnitContentsLoader(SHA1 sha,
                                               folly::StringPiece contents,
                                               const RepoOptionsFlags& options,
                                               std::filesystem::path repoRoot,
                                               UnitEmitterAttributes attributes)
  : m_path{nullptr}
  , m_wrapper{nullptr}
  , m_options{options}
  , m_unitEmitterAttributes{attributes}
  , m_hash{sha}
  , m_file_length{contents.size()}
  , m_repo{std::move(repoRoot)}
  , m_contents_ptr{contents}
  , m_loaded{true}
{
}

folly::StringPiece LazyUnitContentsLoader::contents() {
  if (!m_loaded) {
    auto const oldSize = m_file_length;
    load();
    // The file might have changed after we read the hash from the
    // xattr. So, calculate the hash from the file contents. If
    // there's a mismatch, throw Inconsistency to let the caller know
    // and deal with it (usually by restarting the whole loading
    // process).
    auto const read_file_hash = SHA1{string_sha1(m_contents.slice())};
    if (read_file_hash != m_file_hash) {
      m_contents.reset();
      m_file_length = oldSize;
      m_contents_ptr = {};
      m_loaded = false;
      throw Inconsistency{};
    }
  }
  return m_contents_ptr;
}

void LazyUnitContentsLoader::load() {
  assertx(!m_loaded);

  if (auto c = loadFileContents(m_path, m_wrapper)) {
    m_contents = std::move(*c);
    m_file_length = m_contents.size();
    m_contents_ptr = m_contents.slice();
    m_loaded = true;
  } else {
    throw LoadError{};
  }
}

//////////////////////////////////////////////////////////////////////

}
