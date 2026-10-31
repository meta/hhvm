/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/Traits.h>
#include <folly/container/F14Map.h>
#include <list>

#include "squangle/mysql_client/DbResult.h"
#include "squangle/mysql_client/PoolKey.h"
#include "squangle/mysql_client/TwoLevelCache.h"

namespace facebook::common::mysql_client {

template <typename Client>
class ConnectionPool;
template <typename Client>
class ConnectPoolOperation;
template <typename Client>
class MysqlPooledHolder;

// Auxiliary class to isolate the queue code. Clean ups also happen in this
// class, it mainly manages the ConnectPoolOperation and
// MysqlPooledHolder containers.
template <typename Client>
class PoolStorageData {
 public:
  using PoolOpList = std::list<std::weak_ptr<ConnectPoolOperation<Client>>>;

  PoolStorageData(size_t conn_limit, Duration max_idle_time)
      : conn_limit_(conn_limit), max_idle_time_(max_idle_time) {}

  ~PoolStorageData() = default;

  // Shouldn't need the copy constructor or assignment operator
  PoolStorageData(const PoolStorageData& other) = delete;
  PoolStorageData& operator=(const PoolStorageData& other) = delete;

  // Default implementations for move constructor and assignment operator
  PoolStorageData(PoolStorageData&& other) = default;
  PoolStorageData& operator=(PoolStorageData&& other) = default;

  // Returns an shared pointer of the oldest valid operation in the queue
  // for the given PoolKey. The returned operation is removed from the
  // queue. We return shared to avoid the instance dying (for any reason)
  // before the connection is given to it.
  std::shared_ptr<ConnectPoolOperation<Client>> popOperation(
      const PoolKey& pool_key) {
    auto& list = waitList_[pool_key];
    while (!list.empty()) {
      std::weak_ptr<ConnectPoolOperation<Client>> weak_op = list.front();
      list.pop_front();
      auto ret = weak_op.lock();
      if (ret && !ret->done()) {
        return ret;
      }
    }

    return nullptr;
  }

  // Puts the new operation in the end of the list
  void queueOperation(
      const PoolKey& pool_key,
      std::shared_ptr<ConnectPoolOperation<Client>> pool_op) {
    auto& list = waitList_[pool_key];
    std::weak_ptr<ConnectPoolOperation<Client>> weak_op = std::move(pool_op);
    list.push_back(std::move(weak_op));
  }

  // Searches for an operation in the queue and removes it. Returns whether it
  // was still queued, which callers use to decide whether they may act on it:
  // false means some other thread has already taken it out to fulfill it.
  bool dequeueOperation(
      const PoolKey& pool_key,
      const ConnectPoolOperation<Client>& pool_op) {
    auto& list = waitList_[pool_key];
    auto it = std::find_if(list.begin(), list.end(), [&](const auto& weak_op) {
      auto locked_op = weak_op.lock();
      return locked_op && locked_op.get() == &pool_op;
    });

    if (it == list.end()) {
      return false;
    }

    list.erase(it);
    return true;
  }

  // Calls failureCallback with the error description and removed all
  // the operations for conn_key from the queue.
  void failOperations(
      const PoolKey& pool_key,
      OperationResult op_result,
      unsigned int mysql_errno,
      const std::string& mysql_error) {
    auto& list = waitList_[pool_key];
    while (!list.empty()) {
      std::weak_ptr<ConnectPoolOperation<Client>> weak_op = list.front();
      list.pop_front();
      auto lock_op = weak_op.lock();
      if (lock_op && !lock_op->done()) {
        lock_op->failureCallback(op_result, mysql_errno, mysql_error);
      }
    }
  }

  // Returns a connection for the given ConnectionKey. The connection will
  // be removed from the queue. Depending on the policy, it will give the
  // oldest inserted connection (fifo) or the most recent inserted (lifo).
  std::unique_ptr<MysqlPooledHolder<Client>> popConnection(
      const PoolKey& pool_key) {
    return stock_.popLevel1(pool_key);
  }

  // Returns a connection for the given ConnectionKey, ignoring db name.
  // In order to reuse this connection, we need to run COM_CHANGE_USER.
  std::unique_ptr<MysqlPooledHolder<Client>> popInstanceConnection(
      const PoolKey& pool_key) {
    size_t tries = 0;
    size_t bestSize = 0;
    // Find a pool key from level2_ cache, which has the largest number of
    // connections in level1_ cache. This function checks at most
    // "kMaxKeysToCheckInLevel2" number of keys in level2_ cache.
    return stock_.popLevel2(
        pool_key,
        [&](const auto& key1, const auto& key2) mutable -> std::optional<bool> {
          if (++tries == kMaxKeysToCheckInLevel2) {
            return std::nullopt;
          }

          if (bestSize == 0) {
            bestSize = stock_.level1Size(key1);
          }
          auto testSize = stock_.level1Size(key2);
          if (testSize > bestSize) {
            bestSize = testSize;
            return true;
          }
          return false;
        });
  }

  // Puts the new connection in the back of the list.
  void queueConnection(std::unique_ptr<MysqlPooledHolder<Client>> newConn) {
    const auto& key = newConn->getPoolKey();
    stock_.push(key, std::move(newConn), conn_limit_);
  }

  // Checks and removes the connection that reached their idle time or age
  // limit.
  auto cleanupConnections() {
    Timepoint now = std::chrono::steady_clock::now();
    return stock_.cleanup([&](const auto& conn) {
      return (conn->getLifeDuration() != Duration::zero() &&
              (conn->getCreationTime() + conn->getLifeDuration() < now)) ||
          conn->getLastActivityTime() + max_idle_time_ < now;
    });
  }

  // Checks and removes the weak ptrs that already expired, so we have a
  // better approximation of the number of operations really waiting.
  void cleanupOperations() {
    for (auto mapIt = waitList_.begin(); mapIt != waitList_.end();) {
      auto& list = mapIt->second;
      for (auto it = list.begin(); it != list.end();) {
        // check if weak pointer expired
        if (auto op = it->lock(); !op || op->done()) {
          it = list.erase(it);
          VLOG(11) << "Operation being erased during clean up";
        } else {
          ++it;
        }
      }

      if (list.empty()) {
        mapIt = waitList_.erase(mapIt);
      } else {
        ++mapIt;
      }
    }
  }

  // Cancels all operations that are still in the queue and clears all the
  // storage
  void clearAll() {
    // Clearing all operations in the queue
    for (auto& [_, list] : waitList_) {
      for (auto& wptr : list) {
        // check if weak pointer expired
        if (auto locked_op = wptr.lock(); locked_op) {
          locked_op->cancel();
          VLOG(2) << "Cancelling operation in the pool during clean up";
        }
      }
    }

    waitList_.clear();
    // For the connections we don't need to close one by one, we can just
    // clear the list and leave the destructor to handle it.
    stock_.clear();
  }

  // for debugging
  void displayPoolStatus() {
    LOG(INFO) << "*** level1 cache";
    stock_.iterateLevel1([](const auto& key, const auto& value) {
      LOG(INFO) << key.connKey.getDisplayString()
                << ", # conn = " << value.size();
    });
    LOG(INFO) << "*** level2 cache";
    stock_.iterateLevel2([](const auto& key, const auto& value) {
      LOG(INFO) << key.connKey.getDisplayString(true)
                << ", # key = " << value.size();
    });
  }

  [[nodiscard]] size_t numQueuedOperations(const PoolKey& pool_key) const {
    auto it = waitList_.find(pool_key);
    return it == waitList_.end() ? 0 : it->second.size();
  }

  [[nodiscard]] Duration maxIdleTime() const noexcept {
    return max_idle_time_;
  }

  [[nodiscard]] size_t getNumKey() const noexcept {
    return stock_.level1NumKey();
  }

 private:
  // At most this number of keys will be compared in the level2 cache to find
  // a key having the largest number of connections
  static constexpr int kMaxKeysToCheckInLevel2 = 3;

  // This pool holds weak_ptr to the operation in wait list to avoid holding
  // async client in the draining process in case the operation has already
  // been discarded by the creator before got a connection. This also serves
  // to avoid giving them connections

  TwoLevelCache<
      Client,
      PoolKey,
      std::unique_ptr<MysqlPooledHolder<Client>>,
      PoolKeyHash,
      PoolKeyPartialHash>
      stock_;

  folly::F14NodeMap<PoolKey, PoolOpList, PoolKeyHash> waitList_;

  size_t conn_limit_;
  Duration max_idle_time_;
};

namespace detail {
template <typename T>
using uses_one_thread_ = typename T::uses_one_thread;
} // namespace detail

template <typename T>
constexpr bool uses_one_thread_v =
    folly::is_detected_v<detail::uses_one_thread_, T>;
template <typename T>
struct uses_one_thread_ : std::bool_constant<uses_one_thread_v<T>> {};

// Auxiliary class to isolate the queue code. Clean ups also happen in this
// class, it mainly manages the ConnectPoolOperation and
// MysqlPooledHolder containers.
template <typename Client>
class PoolStorage {
 public:
  // At most this number of keys will be compared in the level2 cache to find
  // a key having the largest number of connections
  PoolStorage(size_t conn_limit, Duration max_idle_time)
      : data_(PoolStorageData<Client>(conn_limit, max_idle_time)) {}

  ~PoolStorage() = default;

  // not copyable or movable
  PoolStorage(const PoolStorage&) = delete;
  PoolStorage& operator=(const PoolStorage&) = delete;

  PoolStorage(PoolStorage&& other) = default;
  PoolStorage& operator=(PoolStorage&& other) = default;

  std::shared_ptr<ConnectPoolOperation<Client>> popOperation(
      const PoolKey& pool_key) {
    if constexpr (uses_one_thread_v<Client>) {
      return data_.popOperation(pool_key);
    } else {
      return data_.wlock()->popOperation(pool_key);
    }
  }

  void queueOperation(
      const PoolKey& pool_key,
      std::shared_ptr<ConnectPoolOperation<Client>> pool_op) {
    if constexpr (uses_one_thread_v<Client>) {
      data_.queueOperation(pool_key, std::move(pool_op));
    } else {
      data_.wlock()->queueOperation(pool_key, std::move(pool_op));
    }
  }

  bool dequeueOperation(
      const PoolKey& pool_key,
      const ConnectPoolOperation<Client>& pool_op) {
    if constexpr (uses_one_thread_v<Client>) {
      return data_.dequeueOperation(pool_key, pool_op);
    } else {
      return data_.wlock()->dequeueOperation(pool_key, pool_op);
    }
  }

  // Calls failureCallback with the error description and removed all
  // the operations for conn_key from the queue.
  void failOperations(
      const PoolKey& pool_key,
      OperationResult op_result,
      unsigned int mysql_errno,
      const std::string& mysql_error) {
    if constexpr (uses_one_thread_v<Client>) {
      data_.failOperations(pool_key, op_result, mysql_errno, mysql_error);
    } else {
      data_.wlock()->failOperations(
          pool_key, op_result, mysql_errno, mysql_error);
    }
  }

  std::unique_ptr<MysqlPooledHolder<Client>> popConnection(
      const PoolKey& pool_key) {
    if constexpr (uses_one_thread_v<Client>) {
      return data_.popConnection(pool_key);
    } else {
      return data_.wlock()->popConnection(pool_key);
    }
  }

  std::unique_ptr<MysqlPooledHolder<Client>> popInstanceConnection(
      const PoolKey& pool_key) {
    if constexpr (uses_one_thread_v<Client>) {
      return data_.popInstanceConnection(pool_key);
    } else {
      return data_.wlock()->popInstanceConnection(pool_key);
    }
  }

  void queueConnection(std::unique_ptr<MysqlPooledHolder<Client>> newConn) {
    if constexpr (uses_one_thread_v<Client>) {
      data_.queueConnection(std::move(newConn));
    } else {
      data_.wlock()->queueConnection(std::move(newConn));
    }
  }

  void cleanupConnections() {
    if constexpr (uses_one_thread_v<Client>) {
      data_.cleanupConnections();
    } else {
      // Return the connections to be cleaned up all the way out to here to
      // avoid the destructor calling back into data_ while we have it locked.
      auto res = data_.wlock()->cleanupConnections();
    }
  }

  void cleanupOperations() {
    if constexpr (uses_one_thread_v<Client>) {
      data_.cleanupOperations();
    } else {
      data_.wlock()->cleanupOperations();
    }
  }

  void clearAll() {
    if constexpr (uses_one_thread_v<Client>) {
      data_.clearAll();
    } else {
      data_.wlock()->clearAll();
    }
  }

  void displayPoolStatus() {
    if constexpr (uses_one_thread_v<Client>) {
      data_.displayPoolStatus();
    } else {
      data_.wlock()->displayPoolStatus();
    }
  }

  [[nodiscard]] size_t numQueuedOperations(const PoolKey& pool_key) const {
    if constexpr (uses_one_thread_v<Client>) {
      return data_.numQueuedOperations(pool_key);
    } else {
      return data_.rlock()->numQueuedOperations(pool_key);
    }
  }

  [[nodiscard]] Duration maxIdleTime() const noexcept {
    if constexpr (uses_one_thread_v<Client>) {
      return data_.maxIdleTime();
    } else {
      return data_.rlock()->maxIdleTime();
    }
  }

  [[nodiscard]] size_t getNumKey() const noexcept {
    if constexpr (uses_one_thread_v<Client>) {
      return data_.getNumKey();
    } else {
      return data_.rlock()->getNumKey();
    }
  }

 private:
  // If uses_one_thread_v<Client> is true we don't need to synchronize access
  // to the pool data, so use PoolStorgeData directly.  Otherwise use the
  // synchronized version.
  using PoolStorageType = typename std::conditional<
      uses_one_thread_v<Client>,
      PoolStorageData<Client>,
      folly::Synchronized<PoolStorageData<Client>>>::type;
  PoolStorageType data_;
};

} // namespace facebook::common::mysql_client
