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
 | If you did not receive a copy of the PHP license and are unable to   |
 | obtain it through the world-wide-web, please send a note to          |
 | license@php.net so we can mail you a copy immediately.               |
 +----------------------------------------------------------------------+
*/

#include "hphp/runtime/base/package.h"

#include "hphp/runtime/base/configs/eval-loader.h"
#include "hphp/runtime/base/execution-context.h"
#include "hphp/runtime/base/init-fini-node.h"
#include "hphp/runtime/base/string-data.h"
#include "hphp/runtime/server/cli-server.h"
#include "hphp/runtime/vm/func.h"

#include "hphp/util/configs/eval.h"

#include <re2/re2.h>
#include "hphp/util/rds-local.h"

#include "hphp/hack/src/packages/ffi_bridge/packages_ffi.rs.h"

#include <folly/json/dynamic.h>
#include <folly/json/json.h>
#include <fstream>

namespace HPHP {

// Most patterns are the same, so de-dup them and avoid creating
// re2::RE2 instances.

namespace {

hphp_fast_map<std::string, std::unique_ptr<const re2::RE2>> s_patternCache;
folly::SharedMutex s_patternCacheLock;

}

PackageInfo::PackageInfo(const PackageMap& packages,
                         const DeploymentMap& deployments,
                         const ImplicitPackageFamilyMap& implicitPackageFamilies,
                         PackageAndImplicitFamilyPathsInLookupOrder
                           packageAndImplicitFamilyPathsInLookupOrder)
  : m_packages(packages)
  , m_deployments(deployments)
  , m_implicitPackageFamilies(implicitPackageFamilies)
  , m_packageAndImplicitFamilyPathsInLookupOrder(
      std::move(packageAndImplicitFamilyPathsInLookupOrder)
    ) {}

PackageInfo PackageInfo::fromFile(const std::filesystem::path& path,
                                  bool enableImplicitPackages) {
  PackageMap packages;
  DeploymentMap deployments;
  ImplicitPackageFamilyMap implicitPackageFamilies;
  PackageAndImplicitFamilyPathsInLookupOrder
    packageAndImplicitFamilyPathsInLookupOrder;

  try {
    if (!std::filesystem::exists(path)) {
      if (Cfg::Eval::PackagesTomlFileName != Cfg::EvalLoader::PackagesTomlFileNameDefault()) {
        Logger::Warning(
          "Could not open the package specification: %s. Continuing with the empty package specification.",
          path.string().c_str());
      }
      return defaults();
    }

    auto info = package::package_info(path.string(), enableImplicitPackages);

    auto const convert = [&] (auto const& v) {
      hphp_vector_string_set result;
      // hphp_vector_string_set inserts to the beginning instead of to the end,
      // insert in reverse order to make up for this.
      // rust::Vec does not define rbegin and rend.
      for (size_t i = v.size(); i > 0; --i) {
        result.insert(std::string(v[i-1]));
      }
      return result;
    };

    for (auto& p : info.packages) {
      packages.emplace(std::string(p.name),
                       Package {
                         convert(p.package.includes),
                         convert(p.package.soft_includes),
                         convert(p.package.include_paths),
                         p.package.enable_strict_isolation,
                         p.package.raise_dynamic_class_load_error,
                         p.package.allow_deployed_packages_checking
                       });
    }

    for (auto& d : info.deployments) {
      deployments.emplace(std::string(d.name),
                          Deployment {
                            convert(d.deployment.packages),
                            convert(d.deployment.soft_packages),
                          });
    }
    for (auto& f : info.implicit_package_families) {
      implicitPackageFamilies.emplace(
        std::string(f.name),
        ImplicitPackageFamily {
          std::string(f.family.path),
          convert(f.family.includes),
          convert(f.family.soft_includes),
          f.family.raise_dynamic_class_load_error,
        }
      );
    }
    auto const& paths =
      info.package_and_implicit_family_paths_in_lookup_order;
    packageAndImplicitFamilyPathsInLookupOrder.reserve(paths.size());
    for (auto& entry : paths) {
      packageAndImplicitFamilyPathsInLookupOrder.push_back({
        std::string(entry.path),
        std::string(entry.package.name),
        entry.package.package.is_implicit,
      });
    }
    if (info.errors.size() > 0) {
      std::vector<folly::StringPiece> packageConfigErrors;
      for (auto& error : info.errors) {
        packageConfigErrors.push_back(error.c_str());
      }
      Logger::FError("Error parsing {}: {}", path.c_str(), folly::join("\n", packageConfigErrors));
    }

    return PackageInfo(
      packages,
      deployments,
      implicitPackageFamilies,
      std::move(packageAndImplicitFamilyPathsInLookupOrder)
    );
  } catch (const std::exception& e) {
    Logger::Warning(
      "Exception %s when reading: %s. Continuing with the empty package specification.",
      e.what(), path.c_str());
    return defaults();
  }
}

PackageInfo PackageInfo::defaults() {
  // Empty package for now
  return {};
}

namespace {

std::optional<std::filesystem::path> repoRelativePath(
    std::filesystem::path path,
    const std::filesystem::path& repoRoot) {
  path = path.lexically_normal();
  if (path.is_absolute()) {
    assertx(!repoRoot.empty());
    auto const root = std::filesystem::absolute(repoRoot).lexically_normal();
    path = path.lexically_relative(root);
  }

  if (path.empty() || path.is_absolute() || path == ".") {
    return std::nullopt;
  }
  if (*path.begin() == "..") return std::nullopt;
  return path;
}

} // namespace

PackageInfo::ResolvedPackagePolicy PackageInfo::resolvePackagePolicy(
    const std::string& package) const {
  auto const explicitPackage = packages().find(package);
  if (explicitPackage != packages().end()) {
    auto const& p = explicitPackage->second;
    auto const strict = p.m_enable_strict_isolation;
    return {
      .strictIsolation = strict,
      .raiseDynamicClassLoadError = strict && p.m_raiseDynamicClassLoadError,
      // Strict wins: HHVM only logs the parser's contradictory-flags error.
      .allowDeployedPackagesChecking =
        !strict && p.m_allow_deployed_packages_checking,
    };
  }

  auto const separator = package.find('.');
  auto const family = package.substr(0, separator);
  auto const implicitPackage = implicitPackageFamilies().find(family);
  if (implicitPackage == implicitPackageFamilies().end()) return {};
  return {
    .strictIsolation = true,
    .raiseDynamicClassLoadError =
      implicitPackage->second.m_raiseDynamicClassLoadError,
    .allowDeployedPackagesChecking = false,
  };
}

PackageInfo::ResolvedPackagePolicy
PackageInfo::strictDynamicReferencePolicyForPath(
    std::filesystem::path path,
    const std::filesystem::path& repoRoot) const {
  if (packageAndImplicitFamilyPathsInLookupOrder().empty()) return {};
  auto const relative = repoRelativePath(std::move(path), repoRoot);
  if (!relative) return {};
  auto const package = pathToPackageName(relative->generic_string());
  if (!package) return {};
  return resolvePackagePolicy(*package);
}
bool PackageInfo::canReportStrictDynamicReference(
    std::string_view activeDeployment) const {
  auto const isStrictPackage = [&] (const std::string& package) {
    return resolvePackagePolicy(package).strictIsolation;
  };

  if (!activeDeployment.empty()) {
    auto const deployment = deployments().find(std::string{activeDeployment});
    if (deployment == deployments().end()) return false;
    return std::any_of(
             deployment->second.m_packages.begin(),
             deployment->second.m_packages.end(),
             isStrictPackage
           ) ||
      std::any_of(
        deployment->second.m_soft_packages.begin(),
        deployment->second.m_soft_packages.end(),
        isStrictPackage
      );
  }

  return std::any_of(
           packages().begin(), packages().end(),
           [&] (auto const& entry) { return isStrictPackage(entry.first); }
         ) ||
    std::any_of(
      implicitPackageFamilies().begin(), implicitPackageFamilies().end(),
      [&] (auto const& entry) { return isStrictPackage(entry.first); }
    );
}
namespace {
folly::dynamic mangleVecForCacheKey(const hphp_vector_string_set& data) {
  folly::dynamic result = folly::dynamic::array();
  for (auto& s : data) result.push_back(s);
  return result;
}

folly::dynamic mangleVecForCacheKey(
  const TinyVector<const re2::RE2*>& data
) {
  folly::dynamic result = folly::dynamic::array();
  for (auto& r : data) result.push_back(r->pattern());
  return result;
}
} // namespace

std::string PackageInfo::mangleForCacheKey() const {
  folly::dynamic packagesAndDeployments = folly::dynamic::object();

  for (auto& [name, package] : packages()) {
    folly::dynamic entry = folly::dynamic::object();
    entry["include_paths"] = mangleVecForCacheKey(package.m_include_paths);
    entry["includes"] = mangleVecForCacheKey(package.m_includes);
    entry["soft_includes"] = mangleVecForCacheKey(package.m_soft_includes);
    entry["enable_strict_isolation"] = package.m_enable_strict_isolation;
    entry["raise_dynamic_class_load_error"] =
      package.m_raiseDynamicClassLoadError;
    entry["allow_deployed_packages_checking"] =
      package.m_allow_deployed_packages_checking;
    packagesAndDeployments[name] = entry;
  }

  for (auto& [name, deployment] : deployments()) {
    folly::dynamic entry = folly::dynamic::object();
    entry["packages"] = mangleVecForCacheKey(deployment.m_packages);
    entry["soft_packages"] = mangleVecForCacheKey(deployment.m_soft_packages);
    packagesAndDeployments[name] = entry;
  }

  folly::dynamic families = folly::dynamic::object();
  for (auto& [name, family] : implicitPackageFamilies()) {
    folly::dynamic entry = folly::dynamic::object();
    entry["path"] = family.m_path;
    entry["includes"] = mangleVecForCacheKey(family.m_includes);
    entry["soft_includes"] = mangleVecForCacheKey(family.m_soft_includes);
    entry["raise_dynamic_class_load_error"] =
      family.m_raiseDynamicClassLoadError;
    families[name] = entry;
  }
  auto result = folly::dynamic::array(
    std::move(packagesAndDeployments),
    std::move(families)
  );

  // By default the ordering of keys in dynamic objects is unspecified, and
  // in dbg builds we randomize the order to ensure no one is depending on it.
  folly::json::serialization_opts opts;
  opts.sort_keys = true;
  return folly::json::serialize(std::move(result), std::move(opts));
}

static RDS_LOCAL_NO_CHECK(const PackageInfo::Deployment*, s_requestActiveDeployment);

static InitFiniNode s_clearRequestActiveDeployment(
  [] { s_requestActiveDeployment.destroy(); },
  InitFiniNode::When::RequestFini
);

const PackageInfo::Deployment* PackageInfo::getActiveDeployment() const {
  auto const findDeploymentByName = [&](const std::string& name) -> const PackageInfo::Deployment* {
    auto const it = deployments().find(name);
    if (it == end(deployments())) return nullptr;
    return &it->second;
  };

  if (Cfg::Repo::Authoritative || !Cfg::Server::Mode) {
    return findDeploymentByName(Cfg::Eval::ActiveDeployment);
  }
  // If unset, set the cached active deployment to null by default.
  if (s_requestActiveDeployment.isNull()) {
    auto const activeDeployment = [&]() -> const PackageInfo::Deployment* {
        // If we're in the CLI server, get the active deployment from cli.hdf
        // Otherwise, read the active deployment from config.hdf
        return is_cli_server_mode() ? findDeploymentByName(cli_get_active_deployment())
                                    : findDeploymentByName(Cfg::Eval::ActiveDeployment);
    }();
    s_requestActiveDeployment.emplace(activeDeployment);
  }
  assertx(!s_requestActiveDeployment.isNull());
  return *s_requestActiveDeployment;
}

bool PackageInfo::implPackageExists(const StringData* package) const {
  assertx(package);
  if (package->empty()) return false;
  auto const activeDeployment = getActiveDeployment();
  // If there's no active deployment, return whether package exists at all
  if (!activeDeployment) return packages().contains(package->toCppString());
  switch (activeDeployment->getDeployKind(package->toCppString())) {
    case DeployKind::Hard:
      return true;
    case DeployKind::Soft:
    case DeployKind::NotDeployed:
      return false;
  }
}

std::optional<std::string>
PackageInfo::implicitPackageNameToPathPrefix(std::string_view name) const {
  auto const separator = name.find('.');
  if (separator == std::string_view::npos) return std::nullopt;

  auto const familyName = name.substr(0, separator);
  auto const member = name.substr(separator + 1);
  if (familyName.empty() || member.empty()) return std::nullopt;

  auto const& families = implicitPackageFamilies();
  auto const family = families.find(familyName);
  if (family == families.end()) return std::nullopt;

  // Package config parsing normalizes family roots to end in `/`.
  assertx(family->second.m_path.empty() || family->second.m_path.ends_with('/'));
  std::string includePath;
  includePath.reserve(family->second.m_path.size() + member.size() + 1);
  includePath.append(family->second.m_path);
  includePath.append(member);
  includePath.push_back('/');
  return includePath;
}

std::optional<std::string>
PackageInfo::pathToPackageName(std::string_view path) const {
  for (auto const& entry : packageAndImplicitFamilyPathsInLookupOrder()) {
    if (!path.starts_with(entry.m_path)) continue;
    if (!entry.m_isImplicit) {
      return entry.m_package;
    }

    auto const remainder = path.substr(entry.m_path.size());
    auto const separator = remainder.find('/');
    if (separator == std::string_view::npos || separator == 0) {
      return std::nullopt;
    }
    auto const member = remainder.substr(0, separator);
    std::string package;
    package.reserve(entry.m_package.size() + member.size() + 1);
    package.append(entry.m_package);
    package.push_back('.');
    package.append(member);
    return package;
  }
  return std::nullopt;
}

} // namespace HPHP
