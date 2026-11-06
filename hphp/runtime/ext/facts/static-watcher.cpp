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

#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <folly/futures/Future.h>

#include "hphp/runtime/ext/facts/static-watcher.h"
#include "hphp/runtime/ext/facts/watcher.h"
#include "hphp/util/optional.h"

namespace HPHP {
namespace Facts {
namespace {

struct StaticWatcher : public Watcher {
  virtual ~StaticWatcher() = default;

  explicit StaticWatcher(std::vector<std::filesystem::path> paths)
      : m_paths{std::move(paths)} {}

  /**
   * Return all of our files as changed files.
   */
  folly::SemiFuture<Delta> getChanges(Clock lastClock) override {
    std::vector<FileDelta> fileDeltas;
    fileDeltas.reserve(m_paths.size());
    for (auto const& path : m_paths) {
      fileDeltas.push_back(
          FileDelta{.m_path = path, .m_exists = true, .m_watcher_hash = {}});
    }
    auto newClock = [](const auto& lastClockStr) -> Clock {
      if (lastClockStr.empty()) {
        return Clock{.m_clock = "1"};
      }
      return Clock{.m_clock = std::to_string(std::stoll(lastClockStr) + 1)};
    }(lastClock.m_clock);
    return folly::makeSemiFuture(
        Delta{
            .m_lastClock = {std::move(lastClock)},
            .m_newClock = std::move(newClock),
            .m_fresh = true,
            .m_files = std::move(fileDeltas),
        });
  }

  std::unique_ptr<Subscription> subscribe(
      std::function<void(Delta&&)> _callback) override {
    return nullptr;
  }

  const std::vector<std::filesystem::path> m_paths;
};

} // namespace

std::shared_ptr<Watcher> make_static_watcher(
    std::vector<std::filesystem::path> paths) {
  return std::make_shared<StaticWatcher>(std::move(paths));
}

} // namespace Facts
} // namespace HPHP
