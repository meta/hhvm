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

#include "hphp/runtime/base/concurrent-shared-store.h"

#include "hphp/runtime/base/apc-object.h"
#include "hphp/runtime/base/apc-stats.h"
#include "hphp/runtime/base/request-id.h"
#include "hphp/runtime/base/variable-serializer.h"
#include "hphp/runtime/ext/apc/ext_apc.h"

#include "hphp/util/logger.h"
#include "hphp/util/trace.h"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <folly/ScopeGuard.h>
#include <folly/Random.h>

using folly::SharedMutex;

namespace HPHP {

TRACE_SET_MOD(apc)

//////////////////////////////////////////////////////////////////////

namespace {

//////////////////////////////////////////////////////////////////////

#ifdef HPHP_TRACE
std::string show(const StoreValue& sval) {
  return fmt::format("size {} kind {}", sval.dataSize, (int)sval.getKind());
}
#endif

//////////////////////////////////////////////////////////////////////

}

//////////////////////////////////////////////////////////////////////

void StoreValue::set(APCHandle* v, int64_t expire_ttl, int64_t max_ttl, int64_t bump_ttl) {
  setHandle(v);
  uint32_t mtime = time(nullptr);
  if (c_time == 0)  c_time = mtime;
  expireRequestId.store(RequestId(), std::memory_order_release);
  expireTime.store(expire_ttl ? mtime + expire_ttl : 0, std::memory_order_release);
  bumpTTL.store(bump_ttl, std::memory_order_release);
  maxExpireTime.store(max_ttl ? mtime + max_ttl : 0, std::memory_order_release);
}

bool StoreValue::expired() const {
  // For primed values, 'expire' is not valid to read.
  if (c_time == 0) return false;
  auto const e = rawExpire();
  return e && time(nullptr) >= e;
}

uint32_t StoreValue::queueExpire() const {
  assertx(c_time != 0); // Should not call queueExpire for primed values

  auto expire = expireTime.load(std::memory_order_acquire);
  if (!expire) {
    return expire;
  }

  auto bump = bumpTTL.load(std::memory_order_acquire);
  if (bump == 0) {
    return expire;
  }

  auto now = time(nullptr);
  auto halftimeExpire = expire - (bump / 2);
  if (halftimeExpire > now) {
    return halftimeExpire;
  }
  return expire;
}

//////////////////////////////////////////////////////////////////////

EntryInfo::Type EntryInfo::getAPCType(const APCHandle* handle) {
  switch (handle->kind()) {
    case APCKind::Uninit:
    case APCKind::Null:
    case APCKind::Bool:
    case APCKind::Int:
    case APCKind::Double:
    case APCKind::PersistentClass:
    case APCKind::LazyClass:
    case APCKind::PersistentFunc:
    case APCKind::PersistentClsMeth:
    case APCKind::FuncEntity:
    case APCKind::ClsMeth:
    case APCKind::StaticArray:
    case APCKind::StaticBespoke:
    case APCKind::StaticString:
      return EntryInfo::Type::Shared;
    case APCKind::SharedString:
      return EntryInfo::Type::SharedString;
    case APCKind::SharedArray:
    case APCKind::SharedBespoke:
      switch (handle->type()) {
        case KindOfPersistentVec:    return EntryInfo::Type::SharedVec;
        case KindOfPersistentDict:   return EntryInfo::Type::SharedDict;
        case KindOfPersistentKeyset: return EntryInfo::Type::SharedKeyset;
        default: always_assert(false);
      }
    case APCKind::SerializedVec:
      return EntryInfo::Type::SerializedVec;
    case APCKind::SerializedDict:
      return EntryInfo::Type::SerializedDict;
    case APCKind::SerializedKeyset:
      return EntryInfo::Type::SerializedKeyset;
    case APCKind::CopiedVec:
    case APCKind::CopiedLegacyVec:
      return EntryInfo::Type::APCVec;
    case APCKind::CopiedDict:
    case APCKind::CopiedLegacyDict:
      return EntryInfo::Type::APCDict;
    case APCKind::CopiedKeyset:
      return EntryInfo::Type::APCKeyset;
    case APCKind::SerializedObject:
      return EntryInfo::Type::SerializedObject;
    case APCKind::CopiedObject:
    case APCKind::CopiedCollection:
      return EntryInfo::Type::APCObject;
    case APCKind::RFunc:
      return EntryInfo::Type::APCRFunc;
    case APCKind::RClsMeth:
      return EntryInfo::Type::APCRClsMeth;
  }
  not_reached();
}

size_t EntryInfo::totalSize() const {
  return size + key.size() + 1 /* null terminator in string */ +
    ConcurrentTableSharedStore::NodeSize + sizeof(StoreValue);
}

//////////////////////////////////////////////////////////////////////

/*
 * Read cache layer for frequently accessed but rarely updated APC entries.
 * Doesn't support APC values that require refcounting, i.e., it supports
 * the 'singletons' null, true, false and the uncounted array/string types.
 * Designed to be used to replicate a subset of ConcurrentTableSharedStore.
 * Keys can be filtered based on prefixes (apcExtension::HotKeyPrefix).
 * Lock-free; if a stored entry is cleared, its value must be kept alive for
 * duration of any requests in flight at that time (treadmill).
 *
 * Capacity is fixed at initialization. Does not check expiration on 'get',
 * so best used with periodic purge enabled (apcExtension::ExpireOnSets).
 *
 * TODO(11228222): Upper-bound the time between periodic purges.
 */
struct HotCache {
  /*
   * Indices are never invalidated once assigned to a key (clearing an entry
   * does not actually erase the key from underlying array, just hides it).
   */
  using Idx = StoreValue::HotCacheIdx;

  void initialize();

  /*
   * If 'key' has an associated value in the cache, returns true and
   * sets 'value' (but leaves 'idx' in an undefined state).
   * Else, returns false, leaves 'value' unchanged, but fills 'idx' with
   * information about the failure to be passed to 'store'.
   */
  bool get(const StringData* key, Variant& value, Idx& idx, bool pure) const;

  /*
   * Try to add or update an entry (key, svar), if they both are eligible
   * and there is still room. 'idx' must be the result of a failed call
   * to 'get' for the same key. On success, updates sval->hotIndex to reference
   * the added/updated entry and returns true.
   */
  bool store(Idx idx, const StringData* key,
             APCHandle* svar, const StoreValue* sval);

  /*
   * Clear the entry referenced by 'sval' (set by a call to 'store') if any.
   * Return false and do nothing if 'sval' does not reference any entry.
   * Safe to call concurrently and repeatedly (idempotent). The caller must
   * ensure the effects of any previous call to 'store' for this key are visible
   * before this methods reads from 'sval' (e.g., by only calling these methods
   * under read/write locks like ConcurrentTableSharedStore::get/store/erase).
   */
  bool clearValue(const StoreValue& sval) {
    return clearValueIdx(sval.hotIndex.load(std::memory_order_acquire));
  }

  /*
   * Consistent with 'get', only less output.
   */
  bool hasValue(const StringData* key) const;

 private:
  // Keys stored are char*, but lookup/insert always use StringData*.
  struct EqualityTester {
    bool operator()(const char* a, const StringData* b) const {
      // AtomicHashArray magic keys are < 0; valid pointers are > 0 and aligned.
#if !FOLLY_SANITIZE
      return reinterpret_cast<intptr_t>(a) > 0 &&
        wordsame(a, b->data(), b->size() + 1);
#else
      return reinterpret_cast<intptr_t>(a) > 0 && !strcmp(a, b->data());
#endif
    }
  };
  struct Hasher {
    size_t operator()(const StringData* a) const { return a->hash(); }
  };

  // Allocates keys on successful insertion. They are never erased/freed.
  struct KeyConverter {
    const char* operator()(const StringData* sd) const {
      if (sd->isStatic()) return sd->data();
      auto const nbytes = sd->size() + 1;
      auto const dst = apc_malloc(nbytes);
      assertx((reinterpret_cast<uintptr_t>(dst) & 7) == 0);
      memcpy(dst, sd->data(), nbytes);
      return reinterpret_cast<const char*>(dst);
    }
  };
  using HotMap = folly::AtomicHashArray<const char*, std::atomic<APCHandle*>,
                                        Hasher, EqualityTester,
                                        APCAllocator<char>,
                                        folly::AtomicHashArrayLinearProbeFcn,
                                        KeyConverter>;

  static bool supportedKind(const APCHandle* h) {
    return h->isSingletonKind() || h->isShared();
  }

  bool clearValueIdx(Idx idx);

  // True iff the given key string is eligible to be considered hot.
  bool maybeHot(const char* key) const {
    for (auto p : apcExtension::HotPrefix) {
      if (strncmp(key, p.data(), p.size()) == 0) {
        return true;
      }
    }
    return false;
  }

  // True if the key *might* be eligible (with false positives).
  bool maybeHotFast(const StringData* sd) const {
    // TODO(11227362): Avoid size check if we can assume zero-padding.
    return sd->size() >= sizeof(uint64_t) &&
      (readPrefix64(sd->data()) & m_hotPrefixMask) == 0;
  }

  // Return the bits that were *not* set in any of the prefixes' first 8 chars.
  static uint64_t computeFastPrefixMask(std::vector<std::string> prefs) {
    uint64_t any = 0;
    for (auto& p : prefs) {
      while (p.size() < sizeof(uint64_t)) p += '\xff';
      any |= readPrefix64(p.data());
    }
    return ~any;
  }

  static uint64_t readPrefix64(const char* s) {
    uint64_t result;
    memcpy(&result, s, sizeof(result));
    return result;
  }

  HotMap::SmartPtr m_hotMap;
  uint64_t m_hotPrefixMask{~0ull};
  std::atomic<bool> m_isFull{false};
};
HotCache s_hotCache;

void HotCache::initialize() {
  if (!m_hotMap) {
    HotMap::Config config;
    config.maxLoadFactor = apcExtension::HotLoadFactor;
    /*
     * Ensure we detect a fully loaded map in time. (AHA's default of
     * 1000 * hundreds of homogeneous writers => O(100,000) entries before
     * first real check.)
     *
     * TODO(11227990): Use maxSize/(actual thread count), or make AHA dynamic.
     */
    config.entryCountThreadCacheSize = 100;
    auto const maxSize = apcExtension::HotSize;
    m_hotMap = HotMap::create(maxSize, config);

    auto const& prefs = apcExtension::HotPrefix;
    m_hotPrefixMask = computeFastPrefixMask(prefs);
  }
}

bool HotCache::hasValue(const StringData* key) const {
  if (!maybeHotFast(key)) return false;
  HotMap::const_iterator it = m_hotMap->find(key);
  return it != m_hotMap->end() &&
         it->second.load(std::memory_order_acquire) != nullptr;
}

bool HotCache::get(const StringData* key, Variant& value, Idx& idx, bool pure) const {
  if (!maybeHotFast(key)) {
    idx = StoreValue::kHotCacheKnownIneligible;
    return false;
  }
  auto it = m_hotMap->find(key);
  if (it == m_hotMap->end()) {
    idx = StoreValue::kHotCacheUnknown;
    return false;
  }
  if (auto const handle = it->second.load(std::memory_order_acquire)) {
    value = handle->toLocal(pure);
    return true;
  }
  idx = it.getIndex();
  return false;
}

bool HotCache::store(Idx idx, const StringData* key,
                     APCHandle* svar, const StoreValue* sval) {
  if (idx == StoreValue::kHotCacheKnownIneligible) return false;
  if (!svar || !supportedKind(svar)) return false;
  if (idx == StoreValue::kHotCacheUnknown) {
    if (!maybeHot(key->data())) return false;
    if (m_isFull.load(std::memory_order_acquire)) return false;
    auto p = m_hotMap->emplace(key, svar);
    if (p.first == m_hotMap->end()) {
      m_isFull.store(true, std::memory_order_release);
      return false;
    }
    idx = p.first.getIndex();
  }
  assertx(idx >= 0);
  sval->hotIndex.store(idx, std::memory_order_release);
  m_hotMap->findAt(idx)->second.store(svar, std::memory_order_release);
  return true;
}

bool HotCache::clearValueIdx(Idx idx) {
  if (idx == StoreValue::kHotCacheUnknown) return false;
  assertx(idx >= 0);
  auto it = m_hotMap->findAt(idx);
  it->second.store(nullptr, std::memory_order_release);
  return true;
}

//////////////////////////////////////////////////////////////////////

bool ConcurrentTableSharedStore::eraseKey(const OptString& key) {
  assertx(!key.isNull());
  return eraseImpl(tagStringData(key.get()), false, nullptr);
}

/*
 * The Map::accessor here establishes a write lock, which means that other
 * threads, protected by read locks through Map::const_accessor, will not
 * read erased values from APC.
 */
bool ConcurrentTableSharedStore::eraseImpl(const char* key,
                                           bool expired,
                                           ExpSet::accessor* expAcc) {
  assertx(key);

  std::shared_lock l(m_lock);
  Map::accessor acc;
  if (!m_vars.find(acc, key)) {
    return false;
  }
  auto& storeVal = acc->second;
  if (expired && !storeVal.expired()) {
    auto expiry = storeVal.queueExpire();
    if (expiry) {
      s_hotCache.clearValue(storeVal);

      // If we got an expAcc it means that we are iterating over m_expQueue and
      // expiring things. But if the item is not expired yet we need to put it back
      // into the queue with the correct expire time. It happens if expire time was
      // updated either by apc_extend_ttl or by setting a new value with a TTL on
      // an existing key.
      if (expAcc) {
        auto ikey = intptr_t(acc->first);
        m_expQueue.push({ ikey, expiry });
      }
    }
    return false;
  }

  s_hotCache.clearValue(storeVal);

  FTRACE(2, "Remove {} {}\n", acc->first, show(acc->second));
  auto const var = storeVal.data();
  APCStats::getAPCStats().removeAPCValue(storeVal.dataSize, var, expired);
  var->unreferenceRoot(storeVal.dataSize);

  APCStats::getAPCStats().removeKey(strlen(acc->first));
  const void* vpkey = acc->first;
  /*
   * Note that we have a delicate situation here; purgeExpired obtains
   * the ExpSet accessor, and then the Map accessor, while eraseImpl
   * (called from other sites) apparently obtains the Map accessor
   * followed by the ExpSet accessor.
   *
   * This does not result in deadlock, because the Map accessor is
   * released by m_vars.erase. But we need this ordering to ensure
   * that as long as you hold an accessor to m_expSet, its key
   * converted to a char* will be a valid c-string.
   */
  m_vars.erase(acc);
  if (expAcc) {
    m_expSet.erase(*expAcc);
  } else {
    /*
     * Note that we can't just call m_expSet.erase(intptr_t(vpkey))
     * here. That will remove the element and not block, even if
     * we hold an ExpSet::accessor to the element in another thread,
     * which would allow us to proceed and free vpkey.
     */
    ExpSet::accessor eAcc;
    if (m_expSet.find(eAcc, intptr_t(vpkey))) {
      m_expSet.erase(eAcc);
    }
  }
  free(const_cast<void*>(vpkey));
  return true;
}

// Should be called outside m_lock
void ConcurrentTableSharedStore::purgeExpired() {
  auto last = m_lastPurgeTime.load(std::memory_order_acquire);
  time_t now = time(nullptr);
  if (now < last + apcExtension::PurgeInterval) return;
  if (!m_lastPurgeTime.compare_exchange_strong(last, now,
                                               std::memory_order_acq_rel)) {
    return;                             // someone beat us
  }
  ExpirationPair tmp;
  DEBUG_ONLY int i = 0;
  DEBUG_ONLY int j = 0;
  while (m_expQueue.try_pop(tmp)) {
    if (tmp.second > now) {
      m_expQueue.push(tmp);
      break;
    }
    ExpSet::accessor acc;
    if (m_expSet.find(acc, tmp.first)) {
      FTRACE(3, "Expiring {}...", (char*)tmp.first);
      if (eraseImpl((char*)tmp.first, true, &acc)) {
        FTRACE(3, "succeeded\n");
        ++i;
        continue;
      }
      FTRACE(3, "failed\n");
    }
    ++j;
  }
  FTRACE(1, "Expired {} entries and ignored {}\n", i, j);
}

void ConcurrentTableSharedStore::purgeDeferred(req::vector<StringData*>&& keys) {
  for (auto const& key : keys) {
    if (eraseImpl(tagStringData(key), true, nullptr)) {
      FTRACE(3, "purgeDeferred: {}\n", key);
    }
  }
  keys.clear();
}

bool ConcurrentTableSharedStore::handlePromoteObj(const OptString& key,
                                                  APCHandle* svar,
                                                  const Variant& value,
                                                  bool pure) {
  auto const pair = APCObject::MakeAPCObject(svar, value, pure);
  if (!pair.handle) return false;
  auto const converted = pair.handle;
  auto const size = pair.size;

  Map::accessor acc;
  if (!m_vars.find(acc, tagStringData(key.get()))) {
    // There is a chance another thread deletes the key when this thread is
    // converting the object. In that case, we just bail
    converted->unreferenceRoot(size);
    return false;
  }

  // Our handle may not be same as `svar' here because some other thread may
  // have updated it already, check before updating.
  auto& sval = acc->second;
  auto const handle = sval.data();
  if (handle == svar && handle->kind() == APCKind::SerializedObject) {
    sval.setHandle(converted);
    APCStats::getAPCStats().updateAPCValue(
      converted, size, handle, sval.dataSize, false);
    handle->unreferenceRoot(sval.dataSize);
    sval.dataSize = size;
    return true;
  }

  converted->unreferenceRoot(size);
  return false;
}

bool ConcurrentTableSharedStore::checkExpire(const OptString& keyStr,
                                                Map::const_accessor& acc) {
  auto const tag = tagStringData(keyStr.get());
  if (!m_vars.find(acc, tag)) return true;
  auto const sval = &acc->second;
  if (sval->c_time == 0) return false;
  /*
   * To reduce thundering herds, expiration on apc_fetch is "deferred":
   * First fetch that sees entry is too old sets expireTime to 1, stores its
   * thread id, and returns false. Subsequent apc_fetch calls by *other*
   * threads will treat that entry as unexpired, but all other code will
   * treat it as expired, in particular, the periodic purging.
   */
  auto const e = sval->expireTime.load(std::memory_order_acquire);
  if (e == 0) return false;

  if (e == 1) {
    // Treat as expired iff this thread was the one setting the 1.
    if (sval->expireRequestId.load(std::memory_order_acquire) ==
        RI().m_id) {
      FTRACE(3, "Previously expired by us: {}\n", show(*sval));
      return true;
    }
    FTRACE(5, "Expired by {}, we are {}\n",
           sval->expireRequestId.load(std::memory_order_acquire).toString(),
           RI().m_id.toString());
    return false;
  }

  auto now = time(nullptr);
  if (now >= e) {
    // Try to mark entry as expired.
    auto expected = RequestId();
    auto const desired = RI().m_id;
    if (!sval->expireRequestId.compare_exchange_strong(expected, desired)) {
      // Another thread raced us and won, so not expired.
      return false;
    }

    FTRACE(3, "Deferred expire: {}\n", show(*sval));
    sval->expireTime.store(1, std::memory_order_release);
    auto const key = intptr_t(acc->first);
    // release acc so the m_expSet.erase won't deadlock with a
    // concurrent purgeExpired.
    acc.release();
    // make sure purgeExpired doesn't kill it before we have a
    // chance to refill it.
    m_expSet.erase(key);
    g_context->enqueueAPCDeferredExpire(keyStr);
    return true;
  }

  auto diff = e - now;
  auto bumpTTL = sval->bumpTTL.load(std::memory_order_acquire);
  if (bumpTTL != 0 && diff <= (bumpTTL / 2)) {
    auto maxExpireTime = sval->maxExpireTime.load(std::memory_order_acquire);
    if (e < maxExpireTime) {
      uint32_t newExpire = now + bumpTTL;
      newExpire = maxExpireTime == 0
        ? newExpire : std::min(newExpire, maxExpireTime);
      FTRACE(3, "Expire soon so extend to {}: {}\n", newExpire, show(*sval));
      auto expected = e;
      sval->expireTime.compare_exchange_strong(
        expected, newExpire, std::memory_order_release);
    }
  }
  return false;
}

bool ConcurrentTableSharedStore::get(const OptString& keyStr, Variant& value, bool pure) {
  FTRACE(3, "Get {}\n", keyStr.get()->data());
  HotCache::Idx hotIdx;
  if (s_hotCache.get(keyStr.get(), value, hotIdx, pure)) return true;
  const StoreValue *sval;
  APCHandle *svar = nullptr;
  std::shared_lock l(m_lock);
  bool promoteObj = false;
  bool needsToLocal = false;
  {
    Map::const_accessor acc;
    if (checkExpire(keyStr, acc)) {
      return false;
    }
    sval = &acc->second;
    svar = sval->data();
    APCKind kind = sval->getKind();
    if (apcExtension::AllowObj &&
        (kind == APCKind::SerializedObject ||
         kind == APCKind::CopiedObject ||
         kind == APCKind::CopiedCollection) &&
        !svar->objAttempted()) {
      // Hold ref here for later promoting the object
      svar->referenceNonRoot();
      needsToLocal = promoteObj = true;
    } else if (svar->isTypedValue()) {
      value = svar->toLocal(pure);
    } else {
      svar->referenceNonRoot();
      needsToLocal = true;
    }
    if (!promoteObj) {
      /*
       * Successful slow-case lookup => add value to cache (if key and kind
       * are eligible and there is still room for it). Another thread may be
       * updating the same key concurrently, but ConcurrentTableSharedStore's
       * per-entry lock ensures it will agree on the value.
       */
      s_hotCache.store(hotIdx, keyStr.get(), svar, sval);
    }
  }

  if (needsToLocal) {
    SCOPE_EXIT { svar->unreferenceNonRoot(); };

    l.unlock(); // toLocal() may reenter the autoloader
    value = svar->toLocal(pure);
    if (promoteObj) handlePromoteObj(keyStr, svar, value, pure);
  }

  return true;
}

int64_t ConcurrentTableSharedStore::inc(const OptString& key, int64_t step,
                                        bool& found) {
  found = false;
  std::shared_lock l(m_lock);

  Map::accessor acc;
  if (!m_vars.find(acc, tagStringData(key.get()))) {
    return 0;
  }
  auto& sval = acc->second;
  if (sval.expired()) return 0;
  /*
   * Inc only works on KindOfDouble or KindOfInt64, which are never kept in
   * file-backed storage from priming.  So we don't need to try to deserialize
   * anything or handle the case that sval.data is file-backed.
   */
  auto const oldHandle = sval.data();
  if (oldHandle == nullptr) return 0;
  if (oldHandle->kind() != APCKind::Int &&
      oldHandle->kind() != APCKind::Double) {
    return 0;
  }

  // Currently a no-op, since HotCache doesn't store int/double.
  assertx(sval.hotIndex == StoreValue::kHotCacheUnknown);
  s_hotCache.clearValue(sval);

  auto const ret = oldHandle->toLocal(true /* pure irrelevant for int */).toInt64() + step;
  auto const pair = APCHandle::Create(VarNR{ret},
                                      APCHandleLevel::Outer,
                                      false,
                                      true /* pure doesn't matter for ints */);
  APCStats::getAPCStats().updateAPCValue(pair.handle, pair.size,
                                         oldHandle, sval.dataSize,
                                         false);
  oldHandle->unreferenceRoot(sval.dataSize);
  sval.setHandle(pair.handle);
  sval.dataSize = pair.size;
  found = true;
  return ret;
}

bool ConcurrentTableSharedStore::cas(const OptString& key, int64_t old,
                                     int64_t val) {
  std::shared_lock l(m_lock);

  Map::accessor acc;
  if (!m_vars.find(acc, tagStringData(key.get()))) {
    return false;
  }

  auto& sval = acc->second;
  if (sval.expired()) return false;
  s_hotCache.clearValue(sval);
  auto const oldHandle = sval.data();
  if ((oldHandle->kind() != APCKind::Int &&
       oldHandle->kind() != APCKind::Double) ||
      oldHandle->toLocal(false /* no pure cas variant */).toInt64() != old) {
    return false;
  }

  auto const pair = APCHandle::Create(VarNR{val}, APCHandleLevel::Outer,
                                      false, false /* no pure cas variant */);
  APCStats::getAPCStats().updateAPCValue(pair.handle, pair.size,
                                         oldHandle, sval.dataSize,
                                         false);
  oldHandle->unreferenceRoot(sval.dataSize);
  sval.setHandle(pair.handle);
  sval.dataSize = pair.size;
  return true;
}

bool ConcurrentTableSharedStore::exists(const OptString& keyStr) {
  if (s_hotCache.hasValue(keyStr.get())) return true;
  std::shared_lock l(m_lock);
  {
    Map::const_accessor acc;
    if (checkExpire(keyStr, acc)) {
      return false;
    }
  }
  return true;
}

int64_t ConcurrentTableSharedStore::size(const OptString& key, bool& found) {
  found = false;
  std::shared_lock l(m_lock);

  Map::accessor acc;
  if (!m_vars.find(acc, tagStringData(key.get()))) {
    return 0;
  }
  auto* sval = &acc->second;
  if (sval->expired()) return 0;

  // We no longer need to worry about expiration: we are reading the already
  // computed dataSize field on APCHandle svals
  found = true;
  return sval->dataSize;
}

// Apply APC.TTLLimit, if it is configured.
static int64_t apply_ttl_limit(int64_t ttl) {
  if (apcExtension::TTLLimit > 0) {
    if (ttl == 0 || ttl > apcExtension::TTLLimit) {
      return apcExtension::TTLLimit;
    }
  }
  return ttl;
}

bool ConcurrentTableSharedStore::extendTTL(const OptString& key, int64_t new_ttl) {
  std::shared_lock l(m_lock);
  Map::accessor acc;
  if (!m_vars.find(acc, tagStringData(key.get()))) {
    return false;
  }
  auto& sval = acc->second;
  if (sval.expired()) return false; // This can't resurrect a value
  if (sval.c_time == 0) return false; // Time has no meaning for primed values
  auto old_expire = sval.rawExpire();
  if (!old_expire) return false; // Already infinite TTL

  new_ttl = apply_ttl_limit(new_ttl);
  // This API can't be used to breach the ttl cap.
  if (new_ttl == 0) {
    sval.expireTime.store(0, std::memory_order_release);
    sval.bumpTTL.store(0, std::memory_order_release);
    sval.maxExpireTime.store(0, std::memory_order_release);
    return true;
  }
  auto new_expire = time(nullptr) + new_ttl;
  if (new_expire > old_expire) {
    sval.expireTime.store(new_expire, std::memory_order_release);
    sval.bumpTTL.store(0, std::memory_order_release);
    sval.maxExpireTime.store(new_expire, std::memory_order_release);
    return true;
  }
  return false;
}


bool ConcurrentTableSharedStore::add(const OptString& key,
                                     const Variant& val,
                                     int64_t max_ttl,
                                     int64_t bump_ttl,
                                     APCHandleLevel level,
                                     bool pure) {
  return storeImpl(key, val, max_ttl, bump_ttl, false, level, pure);
}

void ConcurrentTableSharedStore::set(const OptString& key,
                                     const Variant& val,
                                     int64_t max_ttl,
                                     int64_t bump_ttl,
                                     APCHandleLevel level,
                                     bool pure) {
  storeImpl(key, val, max_ttl, bump_ttl, true, level, pure);
}

bool ConcurrentTableSharedStore::storeImpl(const OptString& key,
                                           const Variant& value,
                                           int64_t max_ttl,
                                           int64_t bump_ttl,
                                           bool overwrite,
                                           APCHandleLevel level,
                                           bool pure) {
  StoreValue *sval;
  auto keyLen = key.size();

  // We need to do this before we acquire any locks. Serializing objects can
  // reenter the VM (__sleep) and certain operations may cause us to throw for
  // types that cannot be serialized to APC.
  auto svar = APCHandle::Create(value, level, false, pure);

  char* const kcp = strdup(key.data());

  {
  std::shared_lock l(m_lock);
  bool present;
  time_t expiry = 0;
  {
    Map::accessor acc;
    APCHandle* current = nullptr;
    present = !m_vars.insert(acc, kcp);
    sval = &acc->second;
    if (present) {
      free(kcp);
      if (!overwrite && !sval->expired()) {
        svar.handle->unreferenceRoot();
        return false;
      }
      /*
       * Simply clear the entry --- if it's truly hot, a successful non-cache
       * 'get' will soon update the entry with the new value.
       */
      s_hotCache.clearValue(*sval);
      current = sval->data();
      FTRACE(2, "Update {} {}\n", acc->first, show(acc->second));
    } else {
      FTRACE(2, "Add {} {}\n", acc->first, show(acc->second));
      APCStats::getAPCStats().addKey(keyLen);
    }

    int64_t adjustedMaxTTL = apply_ttl_limit(max_ttl);
    if (adjustedMaxTTL > apcExtension::TTLMaxFinite) {
      adjustedMaxTTL = 0;
    }

    if (bump_ttl < 0 || (adjustedMaxTTL != 0 && bump_ttl >= adjustedMaxTTL)) {
      bump_ttl = 0;
    }
    bump_ttl = std::min(bump_ttl, (int64_t)std::numeric_limits<uint16_t>::max());

    auto expire_ttl = bump_ttl;
    if (expire_ttl <= 0 || (adjustedMaxTTL > 0 && expire_ttl >= adjustedMaxTTL)) {
      expire_ttl = adjustedMaxTTL;
    }

    if (current) {
      if (sval->rawExpire() == 0 && adjustedMaxTTL != 0) {
        APCStats::getAPCStats().removeAPCValue(
          sval->dataSize, current, sval->expired());
        APCStats::getAPCStats().addAPCValue(svar.handle, svar.size);
      } else {
        APCStats::getAPCStats().updateAPCValue(
          svar.handle, svar.size, current, sval->dataSize,
          sval->expired());
      }
      current->unreferenceRoot(sval->dataSize);
    } else {
      APCStats::getAPCStats().addAPCValue(svar.handle, svar.size);
    }

    sval->set(svar.handle, expire_ttl, adjustedMaxTTL, bump_ttl);
    sval->dataSize = svar.size;
    expiry = sval->queueExpire();
    if (expiry) {
      auto ikey = intptr_t(acc->first);
      if (m_expSet.insert({ ikey, ExpNil{} })) {
        m_expQueue.push({ ikey, expiry });
      }
    }
  }
  }  // m_lock
  if (!apcExtension::UseUncounted && apcExtension::ExpireOnSets) {
    purgeExpired();
  }

  return true;
}

///////////////////////////////////////////////////////////////////////////////
// init

void ConcurrentTableSharedStore::init() {
  s_hotCache.initialize();
}

///////////////////////////////////////////////////////////////////////////////
// debugging and info/stats support

EntryInfo ConcurrentTableSharedStore::makeEntryInfo(const char* key,
                                                    StoreValue* sval,
                                                    int64_t curr_time) {
  auto const handle = sval->data();
  uint32_t size = sval->dataSize;
  auto type = EntryInfo::getAPCType(handle);
  auto calcTTL = [&](uint32_t expire) -> int64_t {
    if (expire) {
      return expire - curr_time;
    }
    return 0;
  };

  auto ttl = calcTTL(sval->rawExpire());
  auto maxTTL = calcTTL(sval->maxExpireTime.load(std::memory_order_acquire));
  return EntryInfo(key, size, ttl, maxTTL, sval->bumpTTL.load(std::memory_order_acquire),
                   type, sval->c_time, s_hotCache.hasValue(OptString(key).get()));
}

std::vector<EntryInfo> ConcurrentTableSharedStore::getEntriesInfo() {
  auto entries = std::vector<EntryInfo>{};

  int64_t curr_time = time(nullptr);
  entries.reserve(m_vars.size() + 1000);

  {
    std::unique_lock l(m_lock);
    for (Map::iterator iter = m_vars.begin(); iter != m_vars.end(); ++iter) {
      entries.push_back(
          makeEntryInfo(iter->first, &iter->second, curr_time));
    }
  }
  std::sort(entries.begin(), entries.end(),
            [] (const EntryInfo& e1, const EntryInfo& e2) {
              return e1.key < e2.key; });
  return entries;
}

/**
 * Dumps a single key and value from APC to `out`. This is used for debug
 * commands only.
 * This function generally needs to be called under m_lock, unless you know that
 * sval won't be invalidated while this is called.
 */
static void dumpOneKeyAndValue(std::ostream &out,
                               const std::string& key,
                               const HPHP::Optional<std::string>& value) {
    out << key;
    out << " #### ";
    if (value) {
      out << "INMEMORY " << *value;
    }
    out << std::endl;
}

static void dumpEntriesInfo(std::vector<EntryInfo> entries, std::ostream& out) {
  out << "key inmem size ttl type maxttl bumpttl inhotcache\n";
  for (auto entry : entries) {
    out << entry.key << " "
        << "1 " // If the value is in memory (before apc_prime could make it live on disk)
        << entry.size << " "
        << entry.ttl << " "
        << static_cast<int32_t>(entry.type) << " "
        << entry.maxTTL << " "
        << entry.bumpTTL << " "
        << (entry.inHotCache ? "1" : "0") << '\n';
  }
}

void ConcurrentTableSharedStore::dumpKeyAndValue(std::ostream & out) {
  auto const entries = debugGetEntries(std::nullopt, std::nullopt);
  out << "Total " << entries.size() << std::endl;
  for (auto const& e : entries) {
    dumpOneKeyAndValue(out, e.first, e.second);
  }
}

hphp_fast_string_set ConcurrentTableSharedStore::debugGetKeys() {
  hphp_fast_string_set keys;
  for (auto const& e: getEntriesInfo()) {
    keys.emplace(e.key);
  }

  return keys;
}

hphp_fast_string_map<HPHP::Optional<std::string>>
ConcurrentTableSharedStore::debugGetEntries(const HPHP::Optional<std::string>& prefix,
                                                  HPHP::Optional<uint32_t> count,
                                                  HPHP::Optional<uint32_t> minSize) {
  hphp_fast_string_map<HPHP::Optional<std::string>> entries;
  std::unique_lock l(m_lock);
  for (auto const& iter : m_vars) {
    if (prefix && strncmp(iter.first, prefix->c_str(), prefix->size()) != 0) {
      // if we asked for a prefix filter, and it does not match, continue
      continue;
    }

    if (minSize && iter.second.dataSize < *minSize) {
      continue;
    }

    if (iter.second.expired()) {
      entries.emplace(iter.first, std::nullopt);
    } else {
      auto const value = iter.second.data()->toLocal(false);
      try {
        auto valS = internal_serialize(value);
        entries.emplace(iter.first, valS.toCppString());
      } catch (const Exception& e) {
        entries.emplace(iter.first, std::string{"Exception: ", e.what()});
      }
    }

    if (count && entries.size() >= *count) {
      break;
    }
  }

  return entries;
}

void ConcurrentTableSharedStore::dump(std::ostream& out, DumpMode dumpMode) {
  Logger::Info("dumping apc");

  switch (dumpMode) {
  case DumpMode::KeyAndValue:
    dumpKeyAndValue(out);
    break;

  case DumpMode::KeyOnly:
    for (auto& key : debugGetKeys()) {
      out << key << '\n';
    }
    break;

  case DumpMode::KeyAndMeta:
    dumpEntriesInfo(getEntriesInfo(), out);
    break;
  }

  Logger::Info("dumping apc done");
}

void ConcurrentTableSharedStore::dumpPrefix(std::ostream& out,
                                            const std::string &prefix,
                                            uint32_t count) {
  Logger::Info("dumping apc prefix %s", prefix.c_str());

  auto const entries = debugGetEntries(prefix, count);

  for (auto const &iter : entries) {
    dumpOneKeyAndValue(out, iter.first, iter.second);
  }

  Logger::Info("dumping apc prefix done");
}

void ConcurrentTableSharedStore::dumpRandomKeys(std::ostream& out,
                                                uint32_t count) {
  dumpEntriesInfo(sampleEntriesInfo(count), out);
}

void ConcurrentTableSharedStore::dumpKeysWithPrefixes(
  BlobEncoder& sd,
  const std::vector<std::string>& prefixes) {
  if (prefixes.empty()) return;
  std::unique_lock l(m_lock);
  sd.lazyCount([&]() {
    uint32_t count = 0;
    for (auto const& iter : m_vars) {
      const StoreValue& value = iter.second;
      if (value.c_time == 0) continue;
      if (value.expired()) continue;
      auto const key = iter.first;
      bool match = std::any_of(
        prefixes.begin(), prefixes.end(),
        [key] (const std::string& prefix) {
          return !strncmp(key, prefix.c_str(), prefix.size());
        });
      if (!match) continue;
      sd(std::string_view(key));
      count++;
    }
    return count;
  });
}

std::vector<EntryInfo>
ConcurrentTableSharedStore::sampleEntriesInfo(uint32_t count) {
  std::unique_lock l(m_lock);
  if (m_vars.empty()) {
    Logger::Warning("No APC entries sampled (empty store)");
    return std::vector<EntryInfo>();
  }
  std::vector<EntryInfo> samples;
  for (; count > 0; count--) {
    if (!m_vars.getRandomAPCEntry(samples)) {
      Logger::Warning("No APC entries sampled (incompatible TBB library)");
      return std::vector<EntryInfo>();
    }
  }
  return samples;
}

std::vector<std::tuple<EntryInfo, uint32_t>>
ConcurrentTableSharedStore::sampleEntriesInfoBySize(uint32_t bytes) {
  std::vector<std::tuple<EntryInfo, uint32_t>> samples;
  if (bytes == 0) return samples;
  int64_t next = folly::Random::rand32(bytes);
  int64_t curr_time = time(nullptr);
  samples.reserve(m_vars.size() / bytes * 128);
  std::unique_lock l(m_lock);
  for (auto& iter : m_vars) {
    auto const key = iter.first;
    StoreValue& value = iter.second;
    if (value.expired()) continue;
    int size = NodeSize + sizeof(StoreValue) + strlen(key) + 1 + value.dataSize;

    if (size > next) {
      uint32_t left = size - next;
      uint32_t weight = ((left - 1) / bytes) + 1;

      samples.push_back(std::make_tuple(makeEntryInfo(key, &value, curr_time),
                                        weight));

      next = weight * bytes - left;
    } else {
      next -= size;
    }
  }
  return samples;
}

template<typename Key, typename T, typename HashCompare>
bool ConcurrentTableSharedStore
      ::APCMap<Key,T,HashCompare>
      ::getRandomAPCEntry(std::vector<EntryInfo>& entries) {
  assertx(!this->empty());
#if TBB_VERSION_MAJOR >= 4
  auto current = this->range();
  for (auto rnd = folly::Random::rand32();
       rnd != 0 && current.is_divisible(); rnd >>= 1) {
    // Split the range 'current' into two halves: 'current' and 'otherHalf'.
    decltype(current) otherHalf(current, tbb::split());
    // Randomly choose which half to keep.
    if (rnd & 1) {
      current = otherHalf;
    }
  }
  auto apcPair = *current.begin();
  int64_t curr_time = time(nullptr);
  entries.push_back(makeEntryInfo(apcPair.first, &apcPair.second, curr_time));
  return true;
#else
  return false;
#endif
}

//////////////////////////////////////////////////////////////////////
}
