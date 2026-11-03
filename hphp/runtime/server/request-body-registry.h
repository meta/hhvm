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
   +----------------------------------------------------------------------+
*/
#pragma once

#include "hphp/runtime/server/request-body-reader.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace HPHP {

struct Hdf;
struct IniSettingMap;

// Startup-immutable routes. All aliases of one profile share its admission
// count; distinct profiles have independent capacities.
struct RequestBodyRegistry final {
  enum class PathMode { Raw, WWW };

  struct Route {
    std::vector<std::string> virtualHosts;
    // A '*' occupies one whole label position and matches one or more labels.
    std::vector<std::string> hosts;
    std::string path;
    PathMode mode{PathMode::Raw};
  };

  struct Profile {
    std::string name;
    size_t highWaterMark{65536};
    uint32_t capacity{1};
    std::vector<Route> routes;
  };

  struct Match {
    // Shared by all matching route, host and virtual-host aliases of a profile.
    std::shared_ptr<RequestBodyProfile> profile;
    // Configured route template, not the concrete request path.
    std::string path;
    // Distinct candidate values for one capture, not multiple captures. Raw
    // mode returns the raw value; WWW tries UTF8-normalized raw input first,
    // then decoded input. Capture-free routes leave this empty. For WWW,
    // authorization must use its resolved parameter, not a candidate selected
    // here before WWW's decode policy runs.
    std::vector<std::string> segments;
  };

  explicit RequestBodyRegistry(std::vector<Profile>);
  static std::shared_ptr<const RequestBodyRegistry> load(
    const IniSettingMap&, const Hdf&);

  // The caller supplies the effective Host (first post-codec value, with its
  // vhost fallback) and the raw request target, not a parsed URL path/authority.
  // Only literal POST is selectable. utf8izeReplace controls replacement vs
  // removal for WWW candidates only; Raw does not UTF8-repair or percent-decode.
  std::optional<Match> select(
    std::string_view virtualHost, std::string_view host,
    std::string_view method, std::string_view target,
    bool utf8izeReplace) const;

  bool empty() const noexcept { return m_routes.empty(); }

 private:
  struct CompiledRoute {
    Route route;
    std::shared_ptr<RequestBodyProfile> profile;
    std::vector<std::string> tokens;
    std::optional<size_t> capture;
    // Original-case literals; overlap folds them if either route is WWW.
    std::string prefix;
    std::string suffix;
  };

  std::vector<CompiledRoute> m_routes;
};

}
