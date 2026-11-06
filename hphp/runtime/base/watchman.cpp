/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
   +----------------------------------------------------------------------+
   | This source path is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the path LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include "hphp/runtime/base/watchman.h"

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <folly/Singleton.h>
#include <folly/Synchronized.h>
#include <folly/concurrency/ConcurrentHashMap.h>
#include <folly/executors/GlobalExecutor.h>
#include <folly/executors/IOThreadPoolExecutor.h>
#include <folly/futures/FutureSplitter.h>
#include <folly/json/json.h>

#include "hphp/util/assertions.h"
#include "hphp/util/hash-map.h"
#include "hphp/util/optional.h"
#include "hphp/util/trace.h"

namespace HPHP {
namespace {

TRACE_SET_MOD(watchman)

WatchmanProfiler s_profiler = nullptr;

/**
 * Result of connecting and watching
 */
struct WatchData {
  std::shared_ptr<watchman::WatchmanClient> m_client;
  watchman::WatchPathPtr m_watchPath;
};

class WatchmanImpl final : public Watchman,
                           public std::enable_shared_from_this<WatchmanImpl> {
public:
  WatchmanImpl(std::filesystem::path path, Optional<std::string> sockPath)
      : m_path{std::move(path)}, m_sockPath{std::move(sockPath)} {
  }

  ~WatchmanImpl() override = default;

  folly::SemiFuture<folly::dynamic> query(folly::dynamic queryObj) override {
    return query(std::move(queryObj), 1);
  }

  folly::SemiFuture<watchman::Clock> getClock() override {
    return getClock(1);
  }

  std::unique_ptr<Subscription> subscribe(
      const folly::dynamic& queryObj,
      watchman::SubscriptionCallback&& callback) override {
    FTRACE(
        3,
        "Subscribing to root {} query {}\n",
        m_path.native(),
        folly::toJson(queryObj));
    auto data = m_data.lock();
    auto const id = data->m_nextCallbackId++;
    auto const [it, inserted] = data->m_callbacks.try_emplace(queryObj);
    it->second.emplace(id, std::move(callback));
    // A query keeps its entry, and its subscription on the current
    // connection, until the next reconnect even after its last callback is
    // released, so only a new entry needs a new subscription.
    if (inserted) {
      data->m_watchFuture.getFuture().thenValue(
          [weakThis = weak_from_this(), queryObj](const WatchData& watchData) {
            auto sharedThis = weakThis.lock();
            if (!sharedThis) {
              return;
            }
            sharedThis->clientSubscribe(
                *watchData.m_client, watchData.m_watchPath, queryObj);
          });
    }
    return std::make_unique<SubscriptionImpl>(weak_from_this(), queryObj, id);
  }

  folly::Future<WatchData> reconnect() {
    folly::Optional<std::string> sockPath;
    if (m_sockPath) {
      sockPath = *m_sockPath;
    }
    auto client = std::make_shared<watchman::WatchmanClient>(
        m_exec.getEventBase(),
        std::move(sockPath),
        folly::getCPUExecutor().get());

    auto connectFuture = client->connect();
    auto watchFuture =
        std::move(connectFuture)
            .via(&m_exec)
            .thenValue([weakThis = weak_from_this(),
                        client = std::move(client)](folly::dynamic&&) mutable {
              auto sharedThis = weakThis.lock();
              if (!sharedThis) {
                throw std::bad_weak_ptr();
              }
              auto watchPathFuture = client->watch(sharedThis->m_path.native());
              return std::move(watchPathFuture)
                  .via(&sharedThis->m_exec)
                  .thenValue([client = std::move(client)](
                                 watchman::WatchPathPtr&& watchPath) mutable {
                    return WatchData{std::move(client), std::move(watchPath)};
                  });
            })
            .thenValue([weakThis = weak_from_this()](WatchData&& watchData) {
              auto sharedThis = weakThis.lock();
              if (!sharedThis) {
                throw std::bad_weak_ptr();
              }
              auto data = sharedThis->m_data.lock();
              folly::erase_if(data->m_callbacks, [](auto const& entry) {
                return entry.second.empty();
              });
              for (auto const& [queryObj, _] : data->m_callbacks) {
                sharedThis->clientSubscribe(
                    *watchData.m_client, watchData.m_watchPath, queryObj);
              }
              return std::move(watchData);
            });

    auto data = m_data.lock();
    data->m_watchFuture = folly::splitFuture(std::move(watchFuture));
    return data->m_watchFuture.getFuture();
  }

private:
  struct SubscriptionImpl final : Subscription {
    SubscriptionImpl(
        std::weak_ptr<WatchmanImpl> watchman,
        folly::dynamic queryObj,
        uint64_t id)
        : m_watchman{std::move(watchman)},
          m_queryObj{std::move(queryObj)},
          m_id{id} {}

    ~SubscriptionImpl() override {
      if (auto watchman = m_watchman.lock()) {
        watchman->unsubscribe(m_queryObj, m_id);
      }
    }

    const std::weak_ptr<WatchmanImpl> m_watchman;
    const folly::dynamic m_queryObj;
    const uint64_t m_id;
  };

  void unsubscribe(const folly::dynamic& queryObj, uint64_t id) {
    FTRACE(
        3,
        "Unsubscribing from root {} query {}\n",
        m_path.native(),
        folly::toJson(queryObj));
    // Destroy the callback after unlocking, since destroying what it owns can
    // re-enter this object.
    auto callback = m_data.withLock(
        [&](auto& data) { return data.m_callbacks.at(queryObj).extract(id); });
  }

  folly::SemiFuture<folly::dynamic>
  query(folly::dynamic queryObj, int nReconnects) {
    auto const preLockTime = std::chrono::steady_clock::now();
    auto data = m_data.lock();
    auto const preExecTime = std::chrono::steady_clock::now();
    return data->m_watchFuture.getFuture()
        .via(&m_exec)
        .thenValue([queryObj, preLockTime, preExecTime](WatchData&& watchData) {
          auto const execTime = std::chrono::steady_clock::now();
          return watchData.m_client->query(
            queryObj,
            std::move(watchData.m_watchPath)
            ).deferValue(
              [queryObj, preLockTime, preExecTime, execTime] (
                watchman::QueryResult&& res
              ) {
                if (s_profiler) {
                  (s_profiler)(
                    res, queryObj, preLockTime, preExecTime, execTime
                  );
                }
                return std::move(res.raw_);
              }
            );
        })
        .thenError([weakThis = weak_from_this(), queryObj, nReconnects](
                       const folly::exception_wrapper& e) mutable {
          auto sharedThis = weakThis.lock();
          if (!sharedThis) {
            throw std::bad_weak_ptr();
          }
          if (nReconnects <= 0) {
            e.throw_exception();
          }
          return sharedThis->reconnect().thenTry(
              [sharedThis, queryObj = std::move(queryObj), nReconnects](
                  folly::Try<WatchData>&&) mutable {
                return sharedThis->query(std::move(queryObj), nReconnects - 1);
              });
        })
        .semi();
  }

  folly::SemiFuture<watchman::Clock> getClock(int nReconnects) {
    auto data = m_data.lock();
    return data->m_watchFuture.getFuture()
        .via(&m_exec)
        .thenValue([](WatchData&& watchData) {
          return watchData.m_client->getClock(std::move(watchData.m_watchPath));
        })
        .thenError([weakThis = weak_from_this(),
                    nReconnects](const folly::exception_wrapper& e) mutable {
          auto sharedThis = weakThis.lock();
          if (!sharedThis) {
            throw std::bad_weak_ptr();
          }
          if (nReconnects <= 0) {
            e.throw_exception();
          }
          return sharedThis->reconnect().thenTry(
              [sharedThis, nReconnects](folly::Try<WatchData>&&) mutable {
                return sharedThis->getClock(nReconnects - 1);
              });
        })
        .semi();
  }

  void runCallbacks(
      const folly::dynamic& queryObj, folly::Try<folly::dynamic>&& results) {
    std::vector<watchman::SubscriptionCallback> callbacks =
        m_data.withLock([&](auto& data) {
          std::vector<watchman::SubscriptionCallback> result;
          auto const& callbacksIt = data.m_callbacks.find(queryObj);
          if (callbacksIt != data.m_callbacks.end()) {
            for (auto const& [_, callback] : callbacksIt->second) {
              result.push_back(callback);
            }
          }
          return result;
        });
    if (callbacks.empty()) {
      return;
    }

    auto it = callbacks.begin();
    for (; std::next(it) != callbacks.end(); ++it) {
      auto cb = *it;
      cb(folly::copy(results));
    }
    // Move the results instead of copying on the last iteration
    if (it != callbacks.end()) {
      assertx(std::next(it) == callbacks.end());
      auto cb = *it;
      cb(std::move(results));
    }
  }

  folly::SemiFuture<watchman::SubscriptionPtr> clientSubscribe(
      watchman::WatchmanClient& client,
      watchman::WatchPathPtr path,
      const folly::dynamic& queryObj) {
    return client.subscribe(
        queryObj,
        std::move(path),
        &m_exec,
        [weakThis = weak_from_this(),
         queryObj](folly::Try<folly::dynamic>&& results) {
          auto sharedThis = weakThis.lock();
          if (!sharedThis) {
            throw std::bad_weak_ptr();
          }
          // If we got an exception from Watchman, attempt to reconnect
          // once. This callback will not run again until we've
          // successfully reconnected.
          if (results.hasException()) {
            results.exception().handle(
                [&sharedThis](const std::system_error& e) {
                  FTRACE(2, "Subscription error: {}\n", e.what());
                  sharedThis->reconnect();
                },
                [&sharedThis](const folly::AsyncSocketException& e) {
                  FTRACE(2, "Subscription error: {}\n", e.what());
                  sharedThis->reconnect();
                });
          }
          sharedThis->runCallbacks(queryObj, std::move(results));
        }, fmt::format("hphp-watchman-subscription-{}", m_nextSubscriptionId++));
  }

  folly::IOThreadPoolExecutor m_exec{1};

  const std::filesystem::path m_path;
  const Optional<std::string> m_sockPath;

  std::atomic<uint64_t> m_nextSubscriptionId{0};

  struct Data {
    // This future must complete before you can perform query() or subscribe()
    // operations on the given repo.
    folly::FutureSplitter<WatchData> m_watchFuture;
    // Each query shares one watchman subscription between its callbacks.
    hphp_hash_map<
        folly::dynamic,
        std::map<uint64_t, watchman::SubscriptionCallback>>
        m_callbacks;
    uint64_t m_nextCallbackId{0};
  };
  folly::Synchronized<Data, std::mutex> m_data;
};

} // namespace

Watchman::~Watchman() = default;

std::shared_ptr<Watchman> Watchman::get(
    const std::filesystem::path& path, const Optional<std::string>& sockPath) {
  assertx(path.is_absolute());
  auto watchman = std::make_shared<WatchmanImpl>(path, sockPath);
  watchman->reconnect();
  return watchman;
}

void Watchman::setProfiler(WatchmanProfiler&& prof) {
  s_profiler = std::move(prof);
}

} // namespace HPHP
