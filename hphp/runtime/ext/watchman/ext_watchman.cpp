/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-2015 Facebook, Inc. (http://www.facebook.com)     |
   | Copyright (c) 1997-2010 The PHP Group                                |
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

#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <utility>

#include <folly/json/dynamic.h>
#include <fmt/core.h>
#include <folly/ExceptionWrapper.h>
#include <folly/Executor.h>
#include <folly/futures/Future.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/ScopedEventBaseThread.h>
#include <folly/json/json.h>

#include <watchman/cppclient/WatchmanClient.h>

#include <hphp/runtime/base/datatype.h>
#include <hphp/runtime/base/array-iterator.h>
#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/base/unit-cache.h"
#include "hphp/runtime/ext/asio/asio-external-thread-event.h"
#include "hphp/runtime/ext/extension.h"

/* Threading, Locking, and Data-Sharing Notes
 *
 * As there is a bunch of data shared between all these contexts we follow a few
 * rules to keep things sane:
 *
 * - All shared data are covered by a single mutex: s_sharedDataMutex.
 *
 * - Unless explicitly noted in comments, entry points from asynchronous sources
 *   (HHVM calls, callbacks, etc.) immediately lock the global mutex and hold it
 *   for their lifetime. Therefore all other functions/methods assume by default
 *   the lock is held apart from some explicit exceptions.
 *
 * - All IO/Watchman operations are run asynchronously and for this reason most
 *   of the HHVM visible API returns Awaitables.
 *
 * - All IO/Watchman asynchronous call-backs are handled on a single thread +
 *   event-base managed by a singleton of WatchmanThreadEventBase. This
 *   automatically locks the global mutex around all ASYNC callbacks and
 *   guarantees serial execution in FIFO order.
 *
 * - The user specified PHP-callback on subsription update is executed in a
 *   fresh AsyncFunc thread. This avoids running this potentially slow operation
 *   in the IO/Watchamn thread.
 *
 * Code in this module may be executed in one of 4 possible thread contexts:
 *
 * - Code called from arbitrary PHP-user requests (PHP).
 *
 * - In an AsyncFunc thread (PHP-CALLBACK).
 *
 * - In a folly event base thread managed by WatchmanThreadEventBase (ASYNC).
 *
 * - HHVM initialization (INIT) - musn't use PHP exception throwing!
 *
 * All methods, functions, and lambdas indicate which of these contexts they are
 * executed from and whether they are an entry-point from that context. If code
 * is an entry-point it should either grab a lock immediately or explain why
 * not. Non entry-points assume they have a lock unless explicitly stated.
 *
 * All data is worked on using STL/folly types. Arguments coming in from PHP are
 * immediately converted to their STL equivelants. All data going out are
 * converted from STL types as late as possible.
 */

namespace HPHP {
namespace {

// Lock covering all Watchman global data - this needs to be grabbed in
// any direct entries from HHVM.
std::mutex s_sharedDataMutex;

// Class to execute short-run Watchman related callbacks in serial order. Long-
// running Watchman subscription callbacks are run as AsyncFunc threads
// initiated from this thread.
struct WatchmanThreadEventBase : folly::Executor {
  // (PHP)
  WatchmanThreadEventBase() :
    m_eventThread([this] {
        // We don't have/need a lock here as we don't use any shared data.
        m_eventBase.loopForever();
      })
  { }

  // (INIT)
  ~WatchmanThreadEventBase() override {
    drain();
    m_eventBase.terminateLoopSoon();
    m_eventThread.join();
  }

    // (PHP)
  void add(folly::Func f) override {
    m_eventBase.add([f = std::move(f)] () mutable { // (ASYNC entry-point)
      std::lock_guard<std::mutex> g(s_sharedDataMutex);
      f();
    });
  }

  // (INIT)
  void drain() {
    // assume EventBases execute queue in FIFO order
    getEventBase().runInEventBaseThreadAndWait([] {});
  }

  // (PHP)
  folly::EventBase& getEventBase() {
    return m_eventBase;
  }

  // (PHP / INIT) Methods for singleton functionality. Folly's Singleton
  // implementation doesn't seem to fit here as it's built around shared_ptrs,
  // but there is no "shared ownership" of the instance. It's instead owned by
  // the whole process but only after it's created.
  static WatchmanThreadEventBase* Get() {
    if (!s_wmTEB) {
      s_wmTEB = new WatchmanThreadEventBase();
    }
    return s_wmTEB;
  }

  // (INIT)
  static void Free() {
    delete s_wmTEB;
    s_wmTEB = nullptr;
  }

  // (INIT)
  static bool Exists() {
    return !!s_wmTEB;
  }

 private:
  folly::EventBase m_eventBase;
  std::thread m_eventThread;

  static WatchmanThreadEventBase* s_wmTEB;
};

WatchmanThreadEventBase* WatchmanThreadEventBase::s_wmTEB{nullptr};

/**
 * Convert a `folly::dynamic` into an `HPHP::TypedValue`.
 */
TypedValue makeTV(const folly::dynamic& data) {
  switch (data.type()) {
    case folly::dynamic::NULLT:
      return make_tv<KindOfNull>();
    case folly::dynamic::BOOL:
      return make_tv<KindOfBoolean>(data.getBool());
    case folly::dynamic::DOUBLE:
      return make_tv<KindOfDouble>(data.getDouble());
    case folly::dynamic::INT64:
      return make_tv<KindOfInt64>(data.getInt());
    case folly::dynamic::STRING:
      return make_tv<KindOfString>(StringData::Make(data.getString()));
    case folly::dynamic::ARRAY:
    {
      VecInit res{data.size()};
      for (auto const& v : data) {
        res.append(makeTV(v));
      }
      return make_tv<KindOfVec>(res.create());
    }
    case folly::dynamic::OBJECT:
    {
      DictInit res{data.size()};
      for (auto const& [k, v] : data.items()) {
        switch (k.type()) {
          case folly::dynamic::INT64:
            res.set(k.getInt(), makeTV(v));
            break;
          case folly::dynamic::STRING:
            res.set(OptString{k.getString()}, makeTV(v));
            break;
          default:
            SystemLib::throwInvalidOperationExceptionObject(fmt::format(
              "folly::dynamic to HPHP::Variant conversion failed! An object's "
              "key was neither an int nor a string. It was {}", k.typeName()));
        }
      }
      return make_tv<KindOfDict>(res.create());
    }
  }
  not_reached();
}

/**
 * Convert an `HPHP::TypedValue` into a `folly::dynamic`.
 */
folly::dynamic makeDynamic(const HPHP::TypedValue& data) {
  switch (data.type()) {
    case KindOfNull:
      return folly::dynamic{nullptr};
    case KindOfBoolean:
      return folly::dynamic{static_cast<bool>(data.val().num)};
    case KindOfDouble:
      return folly::dynamic{data.val().dbl};
    case KindOfInt64:
      return folly::dynamic{data.val().num};
    case KindOfString:
    case KindOfPersistentString:
      return folly::dynamic{data.val().pstr->slice()};
    case KindOfVec:
    case KindOfPersistentVec:
    case KindOfKeyset:
    case KindOfPersistentKeyset:
    {
      auto res = folly::dynamic::array();
      IterateV(data.val().parr, [&](auto const& v) {
        res.push_back(makeDynamic(v));
      });
      return res;
    }
    case KindOfDict:
    case KindOfPersistentDict:
    {
      folly::dynamic res = folly::dynamic::object;
      IterateKV(data.val().parr, [&](auto const& k, auto const& v) {
        res[makeDynamic(k)] = makeDynamic(v);
      });
      return res;
    }
    default:
      SystemLib::throwInvalidOperationExceptionObject(fmt::format(
        "HPHP::Variant to folly::dynamic conversion failed! Got an "
        "unconvertible Variant of type {}...", tname(data.type())));
  }
  not_reached();
}

template <typename T> struct FutureEvent : AsioExternalThreadEvent {
  // (PHP)
  explicit FutureEvent(folly::Future<T>&& future)
  {
    std::move(future).thenTry([this] (folly::Try<T> result) { // (ASYNC)
      if (result.hasException()) {
        m_exception = result.exception();
      } else {
        m_result = result.value();
      }
      markAsFinished();
    });
  }

 protected:
  // (PHP entry-point) we do not get a lock here as this should only be called
  // after the markAsFinished() above which is the only place mutating the state
  // of the data used here. markAsFinished() can only be called once as it is
  // only called (indirectly) from construction of this object instance.
  void unserialize(TypedValue& result) override {
    if (m_exception) {
      SystemLib::throwInvalidOperationExceptionObject(
        m_exception.what().c_str());
    } else {
      unserializeImpl(result);
    }
  }

 private:
  // (PHP) no lock
  template<typename U = T>
  typename std::enable_if<std::is_same<U, std::string>::value>::type
    unserializeImpl(TypedValue& result)
  {
    tvCopy(make_tv<KindOfString>(StringData::Make(m_result)), result);
  }

  // (PHP) no lock
  template<typename U = T>
  typename std::enable_if<std::is_same<U, folly::Unit>::value>::type
    unserializeImpl(TypedValue& result)
  {
    tvCopy(make_tv<KindOfNull>(), result);
  }

  // (PHP) no lock
  template<typename U = T>
  typename std::enable_if<std::is_same<U, bool>::value>::type
    unserializeImpl(TypedValue& result)
  {
    tvCopy(make_tv<KindOfBoolean>(m_result), result);
  }

  // (PHP) no lock
  template<typename U = T>
  typename std::enable_if<std::is_same<U, folly::dynamic>::value>::type
    unserializeImpl(TypedValue& result)
  {
    tvCopy(makeTV(m_result), result);
  }

  T m_result;
  folly::exception_wrapper m_exception;
};

// (PHP) Makes a new WatchmanClient.
folly::Future<std::shared_ptr<watchman::WatchmanClient>>
getWatchmanClientForSocket(const std::string& socket_path) {
  auto socket =
    socket_path.size() ? socket_path : folly::Optional<std::string>();
  auto client = std::make_shared<watchman::WatchmanClient>(
    &(WatchmanThreadEventBase::Get()->getEventBase()),
    std::move(socket),
    WatchmanThreadEventBase::Get(),
    [](folly::exception_wrapper& /*ex*/) { /* (ASYNC) error handler */ }
  );
  return client->connect()
    .via(WatchmanThreadEventBase::Get())
    .thenValue([client](const folly::dynamic& /*connect_info*/) {
      // (ASYNC)
      return client;
    });
}

// (PHP entry-point)
Object HHVM_FUNCTION(HH_watchman_run,
  const OptString& _json_query,
  const Variant& _socket_path
) {
  std::lock_guard<std::mutex> g(s_sharedDataMutex);

  auto json_query = _json_query.toCppString();
  auto socket_path = _socket_path.isNull() ?
    "" : _socket_path.toString().toCppString();

  auto dynamic_query = folly::parseJson(json_query);
  auto res_future = getWatchmanClientForSocket(socket_path)
    .thenValue([dynamic_query] (std::shared_ptr<watchman::WatchmanClient> client) {
      // (ASYNC)
      return client->run(dynamic_query)
        .via(WatchmanThreadEventBase::Get())
        // pass client shared_ptr through to keep client alive
        .thenValue([client] (const folly::dynamic& result) {
          return std::string(toJson(result).data());
        });
    });
  return Object{
    (new FutureEvent<std::string>(std::move(res_future)))->getWaitHandle()
  };
}

struct WatchmanExtension final : Extension {
  // See ext_watchman.php for details of version bumps.
  // (INIT entry-point) no need for a lock
  WatchmanExtension() : Extension("watchman", "1", "scm_client_infra") {}

  // (INIT entry-point) no need for a lock
  void moduleRegisterNative() override {
    HHVM_FALIAS(HH\\watchman_run, HH_watchman_run);
  }

  // (INIT entry-point) this needs more fine grained control over locking as
  // described inline.
  void moduleShutdown() override {
    WatchmanThreadEventBase::Free();
  }

  // (INIT entry-point) no need for lock
  bool moduleEnabled() const override {
    return m_enabled;
  }

  // (INIT entry-point) no need for lock
  void moduleLoad(const IniSetting::Map& ini, Hdf config) override {
    m_enabled = Config::GetBool(ini, config, "watchman.enable", m_enabled);
  }

 private:
  // Disabled by default to ensure we don't pay requestInit/shutdown cost of
  // getting a lock unless we need to.
  bool m_enabled{false};
};

WatchmanExtension s_watchman;

} // namespace
} // namespace HPHP
