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

#include <exception>
#include <string>
#include <string_view>

#include "hphp/runtime/base/watchman-connection.h"

#include "hphp/runtime/base/config.h"
#include "hphp/runtime/base/ini-setting.h"
#include "hphp/util/configs/sandbox.h"
#include "hphp/util/configs/watchman.h"
#include "hphp/util/trace.h"
#include "hphp/util/user-info.h"

#include <fmt/format.h>
#include <folly/concurrency/ConcurrentHashMap.h>
#include <folly/ScopeGuard.h>
#include <folly/Singleton.h>

TRACE_SET_MOD(watchman)

namespace HPHP {

namespace {

constexpr std::string_view kUserTemplateTag = "%{user}";

/**
 * Resolve the given template to return a Watchman socket owned by the given
 * user.
 *
 * Watchman sockets are often scoped by the user who runs the server process.
 * If Wez has a Watchman server, maybe we can communicate with Wez's server by
 * going to `/var/watchman/wez-state/sock`. The user may pass a template
 * string like `/var/watchman/%{user}-state/sock`. This function resolves the
 * template tag `"%{user}"` into "wez".
 *
 * We're resolving a UID into a Linux username on the machine. Linux usernames
 * already have to play nicely with the directory system. You can't have a user
 * named `"../"`, or they wouldn't be able to have a homedir.
 */
std::string format_watchman_socket(
    std::string_view watchmanSocketTemplate, Optional<std::string_view> user) {
  std::string watchmanSocket{watchmanSocketTemplate};
  size_t userTagPos = watchmanSocket.find(kUserTemplateTag);
  if (userTagPos == std::string::npos) {
    return watchmanSocket;
  }

  if (!user) {
    throw std::runtime_error{fmt::format(
      "We could not find a valid Unix username to expand {} to a path",
      watchmanSocket)};
  }

  // Replace each occurrence of "%{user}" with the username we found
  assertx(user->find(kUserTemplateTag) == std::string_view::npos);
  do {
    watchmanSocket.replace(userTagPos, kUserTemplateTag.size(), *user);
    userTagPos = watchmanSocket.find(kUserTemplateTag);
  } while (userTagPos != std::string::npos);

  return watchmanSocket;
}

using WatchmanCache = folly::ConcurrentHashMap<
  std::string,
  std::shared_ptr<Watchman>
>;

folly::Singleton<WatchmanCache> s_watchmanClients;

////////////////////////////////////////////////////////////////////////////////
}

/**
 * Discover who owns the given repo and return the Watchman socket
 * corresponding to that user.
 */
Optional<std::string> find_user_socket(const std::filesystem::path& repoRoot) {
  auto def = [] (Optional<std::string_view> user) -> Optional<std::string> {
    FTRACE(3, "Using watchman.socket.default = {}.\n", Cfg::Watchman::SocketDefault);
    if (Cfg::Watchman::SocketDefault.empty()) return {};
    return format_watchman_socket(Cfg::Watchman::SocketDefault, user);
  };
  int repoRootFD = ::open(repoRoot.native().c_str(), O_DIRECTORY | O_RDONLY);
  if (repoRootFD == -1) return def(std::nullopt);
  SCOPE_EXIT { ::close(repoRootFD); };

  struct ::stat hstat {};
  if (::fstat(repoRootFD, &hstat) != 0) return def(std::nullopt);

  // The repo is owned by root, so use a special root socket
  if (hstat.st_uid == 0) {
    auto const& rootSock = Cfg::Watchman::SocketRoot;
    FTRACE(
        3,
        "{} is owned by root, looking for socket at {}\n",
        repoRoot.native(),
        rootSock.empty() ? "<none>" : rootSock);
    if (rootSock.empty()) {
      std::string root{"root"};
      return def(root);
    } else {
      return rootSock;
    }
  }

  // Find the `watchman.socket` setting in the repo owner's
  // SandboxConfFile (usually a .hphp file somewhere in their home
  // directory).
  UserInfo info{hstat.st_uid};
  auto user = std::string{info.pw->pw_name};
  auto homePath = RO::GetHomePath(user);
  if (!homePath) return def(user);

  IniSetting::Map ini = IniSetting::Map::object;
  Hdf config;

  auto confFileName = (*homePath) / Cfg::Sandbox::ConfFile;
  if (!RO::ReadPerUserSettings(confFileName, ini, config)) {
    return def(user);
  }

  auto sock = Config::GetString(ini, config, "watchman.socket.default");
  return sock.empty() ? def(user) : sock;
}

std::shared_ptr<Watchman>
get_watchman_client(const std::filesystem::path& root) {
  auto const cache = s_watchmanClients.try_get();
  always_assert(cache);

  auto it = cache->find(root.native());
  if (it != cache->end()) return it->second;

  return cache->insert(
    root.native(), Watchman::get(root, find_user_socket(root))
  ).first->second;
}

}
