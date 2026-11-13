/*
 * Copyright (c) 2014 Tim Starling
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <atomic>
#include <mutex>
#include <new>
#include <thread>
#include <vector>
#include <folly/system/HardwareConcurrency.h>
#include <tbb/concurrent_hash_map.h>

namespace HPHP {

/**
 * ConcurrentLRUCache is a thread-safe hashtable with a limited size. When
 * it is full, insert() evicts the least recently used item from the cache.
 *
 * The find() operation fills a ConstAccessor object, which is a smart pointer
 * similar to TBB's const_accessor. After eviction, destruction of the value is
 * deferred until all ConstAccessor objects are destroyed.
 *
 * The implementation is generally conservative, relying on the documented
 * behaviour of tbb::concurrent_hash_map. LRU list transactions are protected
 * with a single mutex. Having our own doubly-linked list implementation helps
 * to ensure that list transactions are sufficiently brief, consisting of only
 * a few loads and stores. User code is not executed while the lock is held.
 *
 * The acquisition of the list mutex during find() is non-blocking (try_lock),
 * so under heavy lookup load, the container will not stall, instead some LRU
 * update operations will be omitted.
 *
 * Insert performance was observed to degrade rapidly when there is a heavy
 * concurrent insert/evict load, mostly due to locks in the underlying
 * TBB::CHM. So if that is a possibility for your workload,
 * ConcurrentScalableCache is recommended instead.
 */
template <class TKey, class TValue, class THash = tbb::tbb_hash_compare<TKey>>
struct ConcurrentLRUCache {
private:
  /**
   * The LRU list node.
   *
   * We make a copy of the key in the list node, allowing us to find the
   * TBB::CHM element from the list node. TBB::CHM invalidates iterators
   * on most operations, even find(), ruling out more efficient
   * implementations.
   */
  struct ListNode {
    ListNode()
      : m_prev(OutOfListMarker), m_next(nullptr)
    {}

    explicit ListNode(const TKey& key)
      : m_key(key), m_prev(OutOfListMarker), m_next(nullptr)
    {}

    TKey m_key;
    ListNode* m_prev;
    ListNode* m_next;

    bool isInList() const {
      return m_prev != OutOfListMarker;
    }
  };

  static ListNode* const OutOfListMarker;

  /**
   * The value that we store in the hashtable. The list node is allocated from
   * an internal object_pool. The ListNode* is owned by the list.
   */
  struct HashMapValue {
    HashMapValue()
      : m_listNode(nullptr)
    {}

    HashMapValue(const TValue& value, ListNode* node)
      : m_value(value), m_listNode(node)
    {}

    TValue m_value;
    ListNode* m_listNode;
  };

  typedef tbb::concurrent_hash_map<TKey, HashMapValue, THash> HashMap;
  typedef typename HashMap::const_accessor HashMapConstAccessor;
  typedef typename HashMap::accessor HashMapAccessor;
  typedef typename HashMap::value_type HashMapValuePair;
  typedef std::pair<const TKey, TValue> SnapshotValue;

public:
  /**
   * The proxy object for TBB::CHM::const_accessor. Provides direct access to
   * the user's value by dereferencing, thus hiding our implementation
   * details.
   */
  struct ConstAccessor {
    ConstAccessor() {}

    const TValue& operator*() const {
      return *get();
    }

    const TValue* operator->() const {
      return get();
    }

    const TValue* get() const {
      return &m_hashAccessor->second.m_value;
    }

    bool empty() const {
      return m_hashAccessor.empty();
    }

  private:
    friend struct ConcurrentLRUCache;
    HashMapConstAccessor m_hashAccessor;
  };

  /**
   * Create a container with a given maximum size
   */
  explicit ConcurrentLRUCache(size_t maxSize);

  ConcurrentLRUCache(const ConcurrentLRUCache& other) = delete;
  ConcurrentLRUCache& operator=(const ConcurrentLRUCache&) = delete;

  ~ConcurrentLRUCache() {
    clear();
  }

  /**
   * Find a value by key, and return it by filling the ConstAccessor, which
   * can be default-constructed. Returns true if the element was found, false
   * otherwise. Updates the eviction list, making the element the
   * most-recently used.
   */
  bool find(ConstAccessor& ac, const TKey& key);

  /**
   * Insert a value into the container. Both the key and value will be copied.
   * The new element will put into the eviction list as the most-recently
   * used.
   *
   * If there was already an element in the container with the same key, it
   * will not be updated, and false will be returned. Otherwise, true will be
   * returned.
   */
  bool insert(const TKey& key, const TValue& value);

  /**
   * Clear the container. NOT THREAD SAFE -- do not use while other threads
   * are accessing the container.
   */
  void clear();

  /**
   * Get a snapshot of the keys in the container by copying them into the
   * supplied vector. This will block inserts and prevent LRU updates while it
   * completes. The keys will be inserted in order from most-recently used to
   * least-recently used.
   */
  void snapshotKeys(std::vector<TKey>& keys);

  /**
   * Get the approximate size of the container. May be slightly too low when
   * insertion is in progress.
   */
  size_t size() const {
    return m_size.load();
  }

private:
  /**
   * Unlink a node from the list. The caller must lock the list mutex while
   * this is called.
   */
  void delink(ListNode* node);

  /**
   * Add a new node to the list in the most-recently used position. The caller
   * must lock the list mutex while this is called.
   */
  void pushFront(ListNode* node);

  /**
   * Evict the least-recently used item from the container. This function does
   * its own locking.
   */
  void evict();

  /**
   * The maximum number of elements in the container.
   */
  size_t m_maxSize;

  /**
   * This atomic variable is used to signal to all threads whether or not
   * eviction should be done on insert. It is approximately equal to the
   * number of elements in the container.
   */
  std::atomic<size_t> m_size;

  /**
   * The underlying TBB hash map.
   */
  HashMap m_map;

  /**
   * The linked list. The "head" is the most-recently used node, and the
   * "tail" is the least-recently used node. The list mutex must be held
   * during both read and write.
   */
  ListNode m_head;
  ListNode m_tail;
  typedef std::mutex ListMutex;
  ListMutex m_listMutex;
};

template <class TKey, class TValue, class THash>
typename ConcurrentLRUCache<TKey, TValue, THash>::ListNode* const
ConcurrentLRUCache<TKey, TValue, THash>::OutOfListMarker = (ListNode*)-1;

template <class TKey, class TValue, class THash>
ConcurrentLRUCache<TKey, TValue, THash>::
ConcurrentLRUCache(size_t maxSize)
  : m_maxSize(maxSize), m_size(0),
  m_map(folly::available_concurrency() * 4) // it will automatically grow
{
  m_head.m_prev = nullptr;
  m_head.m_next = &m_tail;
  m_tail.m_prev = &m_head;
}

template <class TKey, class TValue, class THash>
bool ConcurrentLRUCache<TKey, TValue, THash>::
find(ConstAccessor& ac, const TKey& key) {
  HashMapConstAccessor& hashAccessor = ac.m_hashAccessor;
  if (!m_map.find(hashAccessor, key)) {
    return false;
  }

  // Acquire the lock, but don't block if it is already held
  std::unique_lock<ListMutex> lock(m_listMutex, std::try_to_lock);
  if (lock) {
    ListNode* node = hashAccessor->second.m_listNode;
    // The list node may be out of the list if it is in the process of being
    // inserted or evicted. Doing this check allows us to lock the list for
    // shorter periods of time.
    if (node->isInList()) {
      delink(node);
      pushFront(node);
    }
    lock.unlock();
  }
  return true;
}

template <class TKey, class TValue, class THash>
bool ConcurrentLRUCache<TKey, TValue, THash>::
insert(const TKey& key, const TValue& value) {
  // Insert into the CHM
  ListNode* node = new ListNode(key);
  HashMapAccessor hashAccessor;
  HashMapValuePair hashMapValue(key, HashMapValue(value, node));
  if (!m_map.insert(hashAccessor, hashMapValue)) {
    delete node;
    return false;
  }
  hashAccessor.release();

  // Evict if necessary, now that we know the hashmap insertion was successful.
  size_t size = m_size.load();
  bool evictionDone = false;
  if (size >= m_maxSize) {
    // The container is at (or over) capacity, so eviction needs to be done.
    // Do not decrement m_size, since that would cause other threads to
    // inappropriately omit eviction during their own inserts.
    evict();
    evictionDone = true;
  }

  // Note that we have to update the LRU list before we increment m_size, so
  // that other threads don't attempt to evict list items before they even
  // exist.
  std::unique_lock<ListMutex> lock(m_listMutex);
  pushFront(node);
  lock.unlock();
  if (!evictionDone) {
    size = m_size++;
  }
  if (size > m_maxSize) {
    // It is possible for the size to temporarily exceed the maximum if there is
    // a heavy insert() load, once only as the cache fills. In this situation,
    // we have to be careful not to have every thread simultaneously attempt to
    // evict the extra entries, since we could end up underfilled. Instead we do
    // a compare-and-exchange to acquire an exclusive right to reduce the size
    // to a particular value.
    //
    // We could continue to evict in a loop, but if there are a lot of threads
    // here at the same time, that could lead to spinning. So we will just evict
    // one extra element per insert() until the overfill is rectified.
    if (m_size.compare_exchange_strong(size, size - 1)) {
      evict();
    }
  }
  return true;
}

template <class TKey, class TValue, class THash>
void ConcurrentLRUCache<TKey, TValue, THash>::
clear() {
  m_map.clear();
  ListNode* node = m_head.m_next;
  ListNode* next;
  while (node != &m_tail) {
    next = node->m_next;
    delete node;
    node = next;
  }
  m_head.m_next = &m_tail;
  m_tail.m_prev = &m_head;
  m_size = 0;
}

template <class TKey, class TValue, class THash>
void ConcurrentLRUCache<TKey, TValue, THash>::
snapshotKeys(std::vector<TKey>& keys) {
  keys.reserve(keys.size() + m_size.load());
  std::lock_guard<ListMutex> lock(m_listMutex);
  for (ListNode* node = m_head.m_next; node != &m_tail; node = node->m_next) {
    keys.push_back(node->m_key);
  }
}

template <class TKey, class TValue, class THash>
inline void ConcurrentLRUCache<TKey, TValue, THash>::
delink(ListNode* node) {
  ListNode* prev = node->m_prev;
  ListNode* next = node->m_next;
  prev->m_next = next;
  next->m_prev = prev;
  node->m_prev = OutOfListMarker;
}

template <class TKey, class TValue, class THash>
inline void ConcurrentLRUCache<TKey, TValue, THash>::
pushFront(ListNode* node) {
  ListNode* oldRealHead = m_head.m_next;
  node->m_prev = &m_head;
  node->m_next = oldRealHead;
  oldRealHead->m_prev = node;
  m_head.m_next = node;
}

template <class TKey, class TValue, class THash>
void ConcurrentLRUCache<TKey, TValue, THash>::
evict() {
  std::unique_lock<ListMutex> lock(m_listMutex);
  ListNode* moribund = m_tail.m_prev;
  if (moribund == &m_head) {
    // List is empty, can't evict
    return;
  }
  delink(moribund);
  lock.unlock();

  HashMapAccessor hashAccessor;
  if (!m_map.find(hashAccessor, moribund->m_key)) {
    // Presumably unreachable
    return;
  }
  m_map.erase(hashAccessor);
  delete moribund;
}

} // namespace HPHP

