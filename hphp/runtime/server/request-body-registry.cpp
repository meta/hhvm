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
#include "hphp/runtime/server/request-body-registry.h"

#include "hphp/runtime/base/config.h"
#include "hphp/runtime/base/runtime-option.h"
#include "hphp/runtime/server/virtual-host.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>
#include <unicode/utf8.h>

namespace HPHP {
namespace {

char lowerAscii(char c) {
  return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

std::string lowerAscii(std::string_view value) {
  std::string result(value);
  std::transform(result.begin(), result.end(), result.begin(),
                 [](char c) { return lowerAscii(c); });
  return result;
}

bool equalAscii(std::string_view a, std::string_view b) {
  return a.size() == b.size() &&
    std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
      return lowerAscii(x) == lowerAscii(y);
    });
}

bool startsWith(std::string_view value, std::string_view prefix,
                bool ignoreCase = false) {
  return value.size() >= prefix.size() &&
    (ignoreCase ? equalAscii(value.substr(0, prefix.size()), prefix)
                : value.substr(0, prefix.size()) == prefix);
}

bool endsWith(std::string_view value, std::string_view suffix,
              bool ignoreCase = false) {
  return value.size() >= suffix.size() &&
    (ignoreCase ? equalAscii(value.substr(value.size() - suffix.size()), suffix)
                : value.substr(value.size() - suffix.size()) == suffix);
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }

bool isLowerAlnum(char c) { return isDigit(c) || (c >= 'a' && c <= 'z'); }

bool isAsciiAlnum(char c) { return isLowerAlnum(lowerAscii(c)); }

std::string normalizedHost(std::string_view host) {
  auto const colon = host.rfind(':');
  if (colon != host.npos && colon + 1 < host.size() &&
      std::all_of(host.begin() + colon + 1, host.end(), isDigit)) {
    host = host.substr(0, colon);
  }
  return lowerAscii(host);
}

bool validHostLabels(std::string_view value) {
  bool inLabel = false;
  for (auto const c : value) {
    if (c == '.') {
      if (!inLabel) return false;
      inLabel = false;
    } else {
      if (!isLowerAlnum(c) && c != '-') return false;
      inLabel = true;
    }
  }
  return inLabel;
}

bool hostMatches(std::string_view pattern, std::string_view host) {
  auto const star = pattern.find('*');
  if (star == pattern.npos) return pattern == host;
  auto const prefix = pattern.substr(0, star);
  auto const suffix = pattern.substr(star + 1);
  return host.size() > prefix.size() + suffix.size() &&
    startsWith(host, prefix) && endsWith(host, suffix) &&
    validHostLabels(host.substr(prefix.size(),
      host.size() - prefix.size() - suffix.size()));
}

bool hostPatternsOverlap(std::string_view a, std::string_view b) {
  auto const ai = a.find('*');
  auto const bi = b.find('*');
  if (ai == a.npos) return hostMatches(b, a);
  if (bi == b.npos) return hostMatches(a, b);
  auto const ap = a.substr(0, ai);
  auto const bp = b.substr(0, bi);
  auto const as = a.substr(ai + 1);
  auto const bs = b.substr(bi + 1);
  return (startsWith(ap, bp) || startsWith(bp, ap)) &&
    (endsWith(as, bs) || endsWith(bs, as));
}

void validateHostPattern(std::string_view value) {
  if (value.empty() || value.front() == '.' || value.back() == '.' ||
      value.find("..") != value.npos) {
    throw std::invalid_argument("Invalid request-body host pattern");
  }
  auto const star = value.find('*');
  if (star != value.npos &&
      (value.find('*', star + 1) != value.npos ||
       (star != 0 && value[star - 1] != '.') ||
       (star + 1 != value.size() && value[star + 1] != '.'))) {
    throw std::invalid_argument("Host wildcard must occupy one whole label position");
  }
  for (auto const c : value) {
    if (!isLowerAlnum(c) && c != '-' && c != '.' && c != '*') {
      throw std::invalid_argument("Invalid request-body host pattern");
    }
  }
}

// Absolute-target extraction follows WWW RequestUrl: the first slash after
// the scheme can be inside the query. The caller supplies the effective Host.
std::string_view requestPath(std::string_view target, bool www) {
  auto const scheme = target.find("://");
  if (scheme != target.npos && scheme != 0 &&
      std::all_of(target.begin(), target.begin() + scheme, [](char c) {
        return isAsciiAlnum(c) || c == '+';
      })) {
    auto const slash = target.find('/', scheme + 3);
    if (slash == target.npos) return {};
    target.remove_prefix(slash);
  }
  auto const end = www ? target.find('?') : target.find_first_of("?#");
  return target.substr(0, end);
}

int hexDigit(char c) {
  if (isDigit(c)) return c - '0';
  c = lowerAscii(c);
  return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

// rawurldecode preserves '+' and malformed escapes, including embedded NULs.
std::string rawDecode(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '%' && i + 2 < value.size()) {
      auto const hi = hexDigit(value[i + 1]);
      auto const lo = hexDigit(value[i + 2]);
      if (hi >= 0 && lo >= 0) {
        result.push_back(static_cast<char>(hi * 16 + lo));
        i += 2;
        continue;
      }
    }
    result.push_back(value[i]);
  }
  return result;
}

// Use fb_utf8ize's ICU decoder and NUL treatment without request-heap objects.
std::string utf8ize(std::string_view value, bool replace) {
  if (value.size() > std::numeric_limits<int32_t>::max()) {
    return std::string(value);
  }
  std::string result;
  result.reserve(value.size());
  auto const length = static_cast<int32_t>(value.size());
  int32_t offset = 0;
  while (offset < length) {
    auto const start = offset;
    UChar32 point;
    U8_NEXT(value.data(), offset, length, point);
    if (point > 0) {
      result.append(value.substr(start, offset - start));
    } else if (replace) {
      result.append("\xef\xbf\xbd");
    }
  }
  return result;
}

std::vector<std::string_view> tokens(std::string_view path, bool www) {
  if (startsWith(path, "/")) path.remove_prefix(1);
  if (www && endsWith(path, "/")) path.remove_suffix(1);
  std::vector<std::string_view> result;
  while (true) {
    auto const end = path.find('/');
    result.push_back(path.substr(0, end));
    if (end == path.npos) return result;
    path.remove_prefix(end + 1);
  }
}

bool commonEntry(const std::vector<std::string>& a,
                 const std::vector<std::string>& b) {
  return std::any_of(a.begin(), a.end(), [&](auto const& value) {
    return std::find(b.begin(), b.end(), value) != b.end();
  });
}

}

RequestBodyRegistry::RequestBodyRegistry(std::vector<Profile> profiles) {
  std::set<std::string> names;
  for (auto& config : profiles) {
    if (config.name.empty() || !names.insert(config.name).second ||
        config.highWaterMark == 0 ||
        config.highWaterMark > std::numeric_limits<int32_t>::max() ||
        config.capacity == 0 || config.routes.empty()) {
      throw std::invalid_argument("Invalid request-body profile");
    }
    auto profile = std::make_shared<RequestBodyProfile>(
      std::move(config.name), config.highWaterMark, config.capacity);
    for (auto& route : config.routes) {
      if (route.virtualHosts.empty() || route.hosts.empty() ||
          !startsWith(route.path, "/") || route.path.size() < 2 ||
          endsWith(route.path, "/") ||
          route.path.find_first_of("?#%\\") != route.path.npos) {
        throw std::invalid_argument("Invalid request-body route");
      }
      for (auto& host : route.hosts) {
        host = lowerAscii(host);
        validateHostPattern(host);
      }
      CompiledRoute compiled;
      compiled.profile = profile;
      compiled.route = std::move(route);
      auto const parts = tokens(compiled.route.path, false);
      for (size_t i = 0; i < parts.size(); ++i) {
        auto const part = parts[i];
        if (startsWith(part, "{") && endsWith(part, "}") &&
            part.size() > 2) {
          if (compiled.capture) {
            throw std::invalid_argument("Only one route capture is supported");
          }
          auto const label = part.substr(1, part.size() - 2);
          if (!std::all_of(label.begin(), label.end(), [](char c) {
                return isAsciiAlnum(c) || c == '_';
              })) {
            throw std::invalid_argument("Invalid route capture name");
          }
          compiled.capture = i;
          auto const start = compiled.route.path.find('{');
          auto const end = compiled.route.path.find('}');
          compiled.prefix = compiled.route.path.substr(0, start);
          compiled.suffix = compiled.route.path.substr(end + 1);
        } else if (part.empty() || part.find_first_of("{}") != part.npos ||
                   !std::all_of(part.begin(), part.end(), [](char c) {
                     return c > 32 && static_cast<unsigned char>(c) < 127;
                   })) {
          throw std::invalid_argument("Invalid literal route segment");
        }
        compiled.tokens.emplace_back(part);
      }
      if (!compiled.capture) compiled.prefix = compiled.route.path;
      for (auto const& previous : m_routes) {
        if (!commonEntry(previous.route.virtualHosts,
                         compiled.route.virtualHosts)) continue;
        auto const hostsOverlap = std::any_of(
          previous.route.hosts.begin(), previous.route.hosts.end(),
          [&](auto const& a) {
            return std::any_of(compiled.route.hosts.begin(),
                               compiled.route.hosts.end(), [&](auto const& b) {
              return hostPatternsOverlap(a, b);
            });
          });
        if (!hostsOverlap) continue;
        // Captures can expand separators under WWW percent decoding. Raw and
        // WWW modes can also disagree about where a fragment ends the path.
        auto const mixedModes = previous.route.mode != compiled.route.mode;
        auto const ignoreCase = mixedModes || compiled.route.mode == PathMode::WWW;
        auto const prefixOverlap =
          startsWith(previous.prefix, compiled.prefix, ignoreCase) ||
          startsWith(compiled.prefix, previous.prefix, ignoreCase);
        auto const suffixOverlap =
          endsWith(previous.suffix, compiled.suffix, ignoreCase) ||
          endsWith(compiled.suffix, previous.suffix, ignoreCase);
        // Distinct capture-free templates in one mode cannot match one path.
        if (!mixedModes && !previous.capture && !compiled.capture &&
            (compiled.route.mode == PathMode::WWW
              ? !equalAscii(previous.route.path, compiled.route.path)
              : previous.route.path != compiled.route.path)) continue;
        if (prefixOverlap && (suffixOverlap || mixedModes)) {
          throw std::invalid_argument("Overlapping request-body routes");
        }
      }
      m_routes.push_back(std::move(compiled));
    }
  }
}

std::shared_ptr<const RequestBodyRegistry> RequestBodyRegistry::load(
    const IniSettingMap& ini, const Hdf& hdf) {
  std::vector<Profile> profiles;
  Config::Iterate([&](auto const& profileIni, auto const& profileHdf,
                      auto const&) {
    Profile profile;
    profile.name = Config::GetString(profileIni, profileHdf, "Name", "", false);
    auto const water = Config::GetInt64(
      profileIni, profileHdf, "QueueHighWaterMark",
      static_cast<int64_t>(profile.highWaterMark), false);
    auto const capacity = Config::GetInt64(
      profileIni, profileHdf, "Capacity",
      static_cast<int64_t>(profile.capacity), false);
    if (water <= 0 || water > std::numeric_limits<int32_t>::max() ||
        capacity <= 0 || capacity > std::numeric_limits<uint32_t>::max()) {
      throw std::invalid_argument("Invalid request-body profile limits");
    }
    profile.highWaterMark = water;
    profile.capacity = capacity;
    Config::Iterate([&](auto const& routeIni, auto const& routeHdf, auto const&) {
      Route route;
      route.virtualHosts = Config::GetStrVector(
        routeIni, routeHdf, "VirtualHosts", {}, false);
      for (auto const& name : route.virtualHosts) {
        if (name != "default" && !std::any_of(
              RuntimeOption::VirtualHosts.begin(), RuntimeOption::VirtualHosts.end(),
              [&](auto const& host) { return host->getName() == name; })) {
          throw std::invalid_argument("Unknown request-body virtual host");
        }
      }
      route.hosts = Config::GetStrVector(routeIni, routeHdf, "Hosts", {}, false);
      route.path = Config::GetString(routeIni, routeHdf, "Path", "", false);
      auto const mode = Config::GetString(
        routeIni, routeHdf, "PathMode", "raw", false);
      if (mode != "raw" && mode != "www") {
        throw std::invalid_argument("Unknown request-body path mode");
      }
      route.mode = mode == "www" ? PathMode::WWW : PathMode::Raw;
      profile.routes.push_back(std::move(route));
    }, profileIni, profileHdf, "Routes", false);
    profiles.push_back(std::move(profile));
  }, ini, hdf, "Server.RequestBodyReaders");
  return std::make_shared<const RequestBodyRegistry>(std::move(profiles));
}

std::optional<RequestBodyRegistry::Match> RequestBodyRegistry::select(
    std::string_view virtualHost, std::string_view host,
    std::string_view method, std::string_view target,
    bool utf8izeReplace) const {
  if (method != "POST" || m_routes.empty()) return std::nullopt;
  auto const authority = normalizedHost(host);
  using CandidateParts = std::vector<std::vector<std::string_view>>;
  std::vector<std::string> wwwCandidates;
  std::optional<CandidateParts> rawParts;
  std::optional<CandidateParts> wwwParts;
  for (auto const& compiled : m_routes) {
    auto const& route = compiled.route;
    if (std::find(route.virtualHosts.begin(), route.virtualHosts.end(),
                  virtualHost) == route.virtualHosts.end() ||
        !std::any_of(route.hosts.begin(), route.hosts.end(), [&](auto const& p) {
          return hostMatches(p, authority);
        })) continue;
    auto const www = route.mode == PathMode::WWW;
    auto& candidateParts = www ? wwwParts : rawParts;
    if (!candidateParts) {
      candidateParts.emplace();
      auto const path = requestPath(target, www);
      if (path.empty()) continue;
      if (www) {
        wwwCandidates.push_back(utf8ize(path, utf8izeReplace));
        auto decoded = utf8ize(rawDecode(path), utf8izeReplace);
        if (decoded != wwwCandidates.front()) {
          wwwCandidates.push_back(std::move(decoded));
        }
        // Finish owning all strings before forming views; never move them again.
        for (auto const& candidate : wwwCandidates) {
          candidateParts->push_back(tokens(candidate, true));
        }
      } else if (startsWith(path, "/")) {
        candidateParts->push_back(tokens(path, false));
      }
    }
    std::optional<Match> result;
    for (auto const& parts : *candidateParts) {
      if (parts.size() != compiled.tokens.size()) continue;
      bool same = true;
      for (size_t i = 0; i < parts.size(); ++i) {
        if (compiled.capture == i) {
          if (!www && parts[i].empty()) {
            same = false;
            break;
          }
          continue;
        }
        if (www ? !equalAscii(parts[i], compiled.tokens[i])
                : parts[i] != compiled.tokens[i]) {
          same = false;
          break;
        }
      }
      if (!same) continue;
      if (!result) result.emplace(Match{compiled.profile, route.path, {}});
      if (compiled.capture) {
        auto const segment = parts[*compiled.capture];
        if (!std::any_of(result->segments.begin(), result->segments.end(),
                        [&](auto const& value) {
                          return std::string_view(value) == segment;
                        })) result->segments.emplace_back(segment);
      }
    }
    if (result) return result;
  }
  return std::nullopt;
}

}
