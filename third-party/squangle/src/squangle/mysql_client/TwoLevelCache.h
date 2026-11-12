/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/container/F14Map.h>
#include <folly/container/F14Set.h>
#include <glog/logging.h>
#include <list>

namespace {

// Find the best match based on the results of pred.  `pred` returns a
// std::optional<bool>, and returning std::nullopt indicates that the loop
// should be terminated an the current best returned.  Otherwise true indicates
// that the current value (`*it`) is better than `*best` and false means it is
// not.
template <typename Container, typename Compare>
inline auto findBest(Container& container, Compare comp) {
  auto it = container.begin();
  if (it == container.end()) {
    return container.end();
  }

  auto best = it;
  while (++it != container.end()) {
    if (auto res = comp(*best, *it); !res) {
      break;
    } else if (*res) {
      best = it;
    }
  }

  return best;
}

} // namespace

namespace facebook::common::mysql_client {

// level1_ map: Key = PoolKey, Value = list of MysqlPooledHolder
// level2_ map: Key = PoolKey (dbname is ignored), Value = set of PoolKey
//
// Pool keys will be present in level2_ set as long as its level1_ list include
// non-zero connections.
template <
    typename Client,
    typename Key,
    typename Value,
    typename FullKeyHash,
    typename PartialKeyHash>
class TwoLevelCache {
 public:
  TwoLevelCache() {}

  void push(const Key& key, Value value, size_t max) {
    auto& list = level1_[key];
    if (list.empty()) {
      // The key is new in level1_, let's add it to level2_.
      level2_[key].insert(key);
    }

    list.push_back(std::move(value));
    if (list.size() > max) {
      list.pop_front();
    }
  }

  Value popLevel1(const Key& key) {
    if (auto it = level1_.find(key);
        it != level1_.end() && it->second.size() > 0) {
      auto ret = std::move(it->second.front());
      it->second.pop_front();

      if (it->second.empty()) {
        // The key does not exist in level1_, let's remove it from level2_.
        eraseFromLevel2(key);
        level1_.erase(it);
      }
      return ret;
    }
    return Value();
  }

  // Finds a key from level2_ cache, with a predicate comp, and pops its
  // corresponding connection from level1_ cache.
  // Predicate comp returns true, if the best key needs to be updated, false, if
  // the best key remains unchanged, or folly::none, if we need to stop
  // traversing keys in level2_.
  template <typename Pred>
  Value popLevel2(const Key& key, Pred comp) {
    if (auto it = level2_.find(key); it != level2_.end()) {
      auto best = findBest(it->second, std::move(comp));
      DCHECK(best != it->second.end());
      return popLevel1(*best);
    }
    return Value();
  }

  // Cleans up connections in level2_ (and level1_) cache, which meet the pred.
  template <typename Pred>
  std::vector<Value> cleanup(Pred pred) {
    std::vector<Value> toBeReleased;
    for (auto it1 = level1_.begin(); it1 != level1_.end();) {
      auto& list = it1->second;
      DCHECK(!list.empty());

      for (auto it2 = list.begin(); it2 != list.end();) {
        if (pred(*it2)) {
          // Add the value to the toBeReleased vector so all of them get
          // destructed after the loop completes.
          toBeReleased.push_back(std::move(*it2));
          it2 = list.erase(it2);
        } else {
          ++it2;
        }
      }

      if (list.empty()) {
        // The key does not exist in level1_, let's remove it from level2_
        eraseFromLevel2(it1->first);
        it1 = level1_.erase(it1);
      } else {
        ++it1;
      }
    }

    return toBeReleased;
  }

  void clear() {
    level1_.clear();
    level2_.clear();
  }

  template <typename Func>
  void iterateLevel1(Func func) {
    for (const auto& [key, value] : level1_) {
      func(key, value);
    }
  }

  template <typename Func>
  void iterateLevel2(Func func) {
    for (const auto& [key, value] : level2_) {
      func(key, value);
    }
  }

  size_t level1Size(const Key& key) const {
    if (auto it = level1_.find(key); it != level1_.end()) {
      return it->second.size();
    }
    return 0;
  }

  size_t level2Size(const Key& key) const {
    if (auto it = level2_.find(key); it != level2_.end()) {
      return it->second.size();
    }
    return 0;
  }

  size_t level1NumKey() const noexcept {
    return level1_.size();
  }

 private:
  using Level2Value = folly::F14FastSet<Key, FullKeyHash>;
  using Level1Map = folly::F14FastMap<Key, std::list<Value>, FullKeyHash>;
  using Level2Map =
      folly::F14FastMap<Key, Level2Value, PartialKeyHash, PartialKeyHash>;

  // This should be called before erasing the empty list from level1
  void eraseFromLevel2(const Key& key) {
    DCHECK(level1_.contains(key));
    DCHECK(level1_.at(key).empty());
    auto it = level2_.find(key);
    DCHECK(it != level2_.end());
    it->second.erase(key);
    if (it->second.empty()) {
      level2_.erase(it);
    }
  }

  Level1Map level1_;
  Level2Map level2_;
};

} // namespace facebook::common::mysql_client
