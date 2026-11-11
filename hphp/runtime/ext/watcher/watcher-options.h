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

#pragma once

#include <string>
#include <vector>

#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include <folly/json/dynamic.h>

#include "hphp/runtime/base/type-variant.h"
#include "hphp/runtime/ext/watcher/watcher-clock.h"

// #include <folly/ExceptionWrapper.h>
// #include <folly/Executor.h>
// #include <fmt/format.h>
// #include <folly/futures/Future.h>
// #include <folly/io/async/EventBase.h>
// #include <folly/io/async/ScopedEventBaseThread.h>
// #include <folly/json/json.h>

// #include <watchman/cppclient/WatchmanClient.h>

// #include <hphp/runtime/base/datatype.h>
// #include <hphp/runtime/base/array-iterator.h>
// #include "hphp/runtime/base/array-init.h"
// #include "hphp/runtime/base/unit-cache.h"
// #include "hphp/runtime/ext/asio/asio-external-thread-event.h"
// #include "hphp/runtime/ext/extension.h"

namespace HPHP {
namespace Watcher {

struct WatcherOptions {

public:
  WatcherOptions() {};

  WatcherOptions(const Variant& options, const Variant& clock);
  folly::dynamic watchmanQuery();

  Optional<std::string> root;
  Optional<std::vector<std::string>> include_paths;
  Optional<std::vector<std::string>> include_extensions;
  Optional<std::vector<std::string>> exclude_paths;
  Optional<std::vector<std::string>> exclude_extensions;
  Optional<std::string> relative_root;
  Optional<std::vector<std::string>> fields;
  Optional<bool> empty_on_fresh_instance;
  Optional<bool> defer_vcs;
  Optional<int> sync_timeout;
  WatcherClock clock;

  // Watchman specific options
  std::string socket_path;
};

} // namespace Watcher
} // namespace HPHP
