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
#include "hphp/runtime/base/program-functions.h"
#include "hphp/runtime/base/runtime-option.h"
#include "hphp/runtime/base/static-string-table.h"
#include "hphp/runtime/base/unit-cache.h"
#include "hphp/runtime/vm/unit-emitter.h"

#include "hphp/util/blob-encoder.h"
#include "hphp/util/configs/eval.h"
#include "hphp/util/configs/repo.h"
#include "hphp/util/configs/server.h"

#include <folly/ScopeGuard.h>
#include <folly/testing/TestUtil.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>
#include <tuple>
#include <utility>

namespace HPHP {
namespace {

struct ActiveDeploymentConfig {
  bool serverMode = std::exchange(Cfg::Server::Mode, false);
  bool repoAuthoritative = std::exchange(Cfg::Repo::Authoritative, false);
  std::string name = std::exchange(Cfg::Eval::ActiveDeployment, "production");

  ~ActiveDeploymentConfig() {
    Cfg::Server::Mode = serverMode;
    Cfg::Repo::Authoritative = repoAuthoritative;
    Cfg::Eval::ActiveDeployment = std::move(name);
  }
};

PackageInfo deploymentPackageInfo(const char* package) {
  PackageInfo info;
  info.m_deployments.emplace(
    "production", PackageInfo::Deployment{{package}, {}}
  );
  return info;
}

TEST(PackageInfoTest, ClearsActiveDeploymentBetweenRequests) {
  ActiveDeploymentConfig config;
  Cfg::Server::Mode = true;
  std::thread{[] {
    hphp_thread_init();
    SCOPE_EXIT { hphp_thread_exit(); };
    auto const first = deploymentPackageInfo("prod");
    auto const second = deploymentPackageInfo("intern");
    {
      HphpSession request{Treadmill::SessionKind::UnitTests};
      EXPECT_EQ(&first.deployments().at("production"), first.getActiveDeployment());
    }
    {
      HphpSession request{Treadmill::SessionKind::UnitTests};
      EXPECT_EQ(&second.deployments().at("production"), second.getActiveDeployment());
    }
  }}.join();
}

TEST(PackageInfoTest, ClearsMissingActiveDeploymentBetweenRequests) {
  ActiveDeploymentConfig config;
  Cfg::Server::Mode = true;
  std::thread{[] {
    hphp_thread_init();
    SCOPE_EXIT { hphp_thread_exit(); };
    PackageInfo info;
    {
      HphpSession request{Treadmill::SessionKind::UnitTests};
      EXPECT_EQ(info.getActiveDeployment(), nullptr);
    }
    info.m_deployments.emplace(
      "production", PackageInfo::Deployment{{"intern"}, {}}
    );
    {
      HphpSession request{Treadmill::SessionKind::UnitTests};
      EXPECT_EQ(&info.deployments().at("production"), info.getActiveDeployment());
    }
  }}.join();
}

// Checks which names may be asked about at runtime.
TEST(PackageInfoTest, AllowsDeployedPackagesChecking) {
  PackageInfo info;
  info.m_packages.emplace("opted_in", PackageInfo::Package{});
  info.m_packages.at("opted_in").m_allow_deployed_packages_checking = true;
  info.m_packages.emplace("closed", PackageInfo::Package{});
  info.m_packages.emplace("contradictory", PackageInfo::Package{});
  info.m_packages.at("contradictory").m_enable_strict_isolation = true;
  info.m_packages.at("contradictory").m_allow_deployed_packages_checking = true;
  info.m_implicitPackageFamilies.emplace(
    "prototypes",
    PackageInfo::ImplicitPackageFamily{"families/", {}, {}}
  );

  auto const allows = [&] (const char* name) {
    return info.resolvePackagePolicy(name).allowDeployedPackagesChecking;
  };
  EXPECT_TRUE(allows("opted_in"));
  EXPECT_FALSE(allows("closed"));
  // Strict isolation wins over an opt-in the config parser rejected.
  EXPECT_FALSE(allows("contradictory"));
  // Implicit packages are strict-isolation, so neither the family nor a
  // member of it may be checked for.
  EXPECT_FALSE(allows("prototypes"));
  EXPECT_FALSE(allows("prototypes.example"));
  // An undeclared name stays checkable so a typo reports absent.
  EXPECT_TRUE(allows("neverdeclared"));
}

// Checks that config loading gates implicit families and reports strict status.
TEST(PackageInfoTest, LoadsImplicitPackageFamiliesOnlyWhenEnabled) {
  folly::test::TemporaryDirectory temp{"package-info"};
  auto const config = temp.path() / "PACKAGES.toml";
  std::ofstream out{config};
  out << R"(
[packages]
[packages.intern]
include_paths = ["//alpha/", "//www/"]
[packages.soft]

[packages.strict]
enable_strict_isolation = true

[packages.notice]
enable_strict_isolation = true
raise_dynamic_class_load_error = false

[packages.explicit_error]
enable_strict_isolation = true
raise_dynamic_class_load_error = true

[implicit_packages.prototypes]
path = "//www/prototypes/"
includes = ["intern"]
soft_includes = ["soft"]

[implicit_packages.notices]
path = "//www/notices/"
raise_dynamic_class_load_error = false

[implicit_packages.explicit_errors]
path = "//www/explicit-errors/"
raise_dynamic_class_load_error = true
)";
  out.close();

  auto const configPath = std::filesystem::path{config.native()};
  auto const enabled = PackageInfo::fromFile(configPath, true);
  ASSERT_EQ(enabled.implicitPackageFamilies().size(), 3);
  auto const& family = enabled.implicitPackageFamilies().at("prototypes");
  EXPECT_EQ(family.m_path, "www/prototypes/");
  EXPECT_TRUE(family.m_includes.contains("intern"));
  EXPECT_TRUE(family.m_soft_includes.contains("soft"));
  EXPECT_TRUE(family.m_raiseDynamicClassLoadError);
  EXPECT_FALSE(enabled.packages().contains("prototypes"));

  auto const disabled = PackageInfo::fromFile(configPath, false);
  EXPECT_TRUE(disabled.implicitPackageFamilies().empty());
  EXPECT_TRUE(disabled.packages().contains("intern"));
  EXPECT_TRUE(disabled.packages().contains("soft"));
  auto const strict = enabled.resolvePackagePolicy("strict");
  EXPECT_TRUE(strict.strictIsolation);
  EXPECT_TRUE(strict.raiseDynamicClassLoadError);

  auto const notice = enabled.resolvePackagePolicy("notice");
  EXPECT_TRUE(notice.strictIsolation);
  EXPECT_FALSE(notice.raiseDynamicClassLoadError);

  auto const explicitError =
    enabled.resolvePackagePolicy("explicit_error");
  EXPECT_TRUE(explicitError.strictIsolation);
  EXPECT_TRUE(explicitError.raiseDynamicClassLoadError);

  auto const intern = enabled.resolvePackagePolicy("intern");
  EXPECT_FALSE(intern.strictIsolation);
  EXPECT_FALSE(intern.raiseDynamicClassLoadError);

  auto const prototypes = enabled.resolvePackagePolicy("prototypes");
  EXPECT_TRUE(prototypes.strictIsolation);
  EXPECT_TRUE(prototypes.raiseDynamicClassLoadError);

  auto const prototypeMember =
    enabled.resolvePackagePolicy("prototypes.example");
  EXPECT_TRUE(prototypeMember.strictIsolation);
  EXPECT_TRUE(prototypeMember.raiseDynamicClassLoadError);

  auto const noticeMember = enabled.resolvePackagePolicy("notices.example");
  EXPECT_TRUE(noticeMember.strictIsolation);
  EXPECT_FALSE(noticeMember.raiseDynamicClassLoadError);

  auto const explicitErrorMember =
    enabled.resolvePackagePolicy("explicit_errors.example");
  EXPECT_TRUE(explicitErrorMember.strictIsolation);
  EXPECT_TRUE(explicitErrorMember.raiseDynamicClassLoadError);

  auto const unknown = enabled.resolvePackagePolicy("unknown");
  EXPECT_FALSE(unknown.strictIsolation);
  EXPECT_FALSE(unknown.raiseDynamicClassLoadError);

  std::vector<std::tuple<std::string, std::string, bool>>
    packageAndImplicitFamilyPathsInLookupOrder;
  for (auto const& entry :
       enabled.packageAndImplicitFamilyPathsInLookupOrder()) {
    packageAndImplicitFamilyPathsInLookupOrder.emplace_back(
      entry.m_path,
      entry.m_package,
      entry.m_isImplicit
    );
  }
  EXPECT_EQ(
    packageAndImplicitFamilyPathsInLookupOrder,
    (std::vector<std::tuple<std::string, std::string, bool>>{
      {"www/prototypes/", "prototypes", true},
      {"www/notices/", "notices", true},
      {"www/explicit-errors/", "explicit_errors", true},
      {"www/", "intern", false},
      {"alpha/", "intern", false},
    })
  );
}

TEST(PackageInfoTest, NonStrictPackageCannotRaiseDynamicReferenceError) {
  PackageInfo info;
  auto& package = info.m_packages["example"];
  package.m_raiseDynamicClassLoadError = true;

  auto const policy = info.resolvePackagePolicy("example");
  EXPECT_FALSE(policy.strictIsolation);
  EXPECT_FALSE(policy.raiseDynamicClassLoadError);
}

// Checks that strict-isolation metadata contributes to the package cache key.
TEST(PackageInfoTest, StrictIsolationChangesCacheMangle) {
  PackageInfo base;
  base.m_packages.emplace("example", PackageInfo::Package{});

  auto strict = base;
  strict.m_packages.at("example").m_enable_strict_isolation = true;

  EXPECT_NE(base.mangleForCacheKey(), strict.mangleForCacheKey());
}

TEST(PackageInfoTest, DynamicClassLoadErrorChangesCacheMangle) {
  PackageInfo base;
  base.m_packages.emplace("example", PackageInfo::Package{});
  base.m_packages.at("example").m_enable_strict_isolation = true;

  auto raises = base;
  raises.m_packages.at("example").m_raiseDynamicClassLoadError =
    true;

  EXPECT_NE(base.mangleForCacheKey(), raises.mangleForCacheKey());
}

// Checks that the cache key includes an empty implicit package family map.
TEST(PackageInfoTest, MangleIncludesEmptyImplicitFamilies) {
  PackageInfo info;
  info.m_packages.emplace("example", PackageInfo::Package{});

  EXPECT_EQ(
    info.mangleForCacheKey(),
    R"([{"example":{"allow_deployed_packages_checking":false,"enable_strict_isolation":false,"include_paths":[],"includes":[],"raise_dynamic_class_load_error":false,"soft_includes":[]}},{}])"
  );
}

// Checks that cache keys store explicit packages and implicit families separately.
TEST(PackageInfoTest, SeparatesImplicitFamiliesInCacheMangle) {
  PackageInfo info;
  info.m_packages.emplace("implicit_families", PackageInfo::Package{});
  info.m_implicitPackageFamilies.emplace(
    "prototypes",
    PackageInfo::ImplicitPackageFamily{"families/", {}, {}}
  );

  EXPECT_EQ(
    info.mangleForCacheKey(),
    R"([{"implicit_families":{"allow_deployed_packages_checking":false,"enable_strict_isolation":false,"include_paths":[],"includes":[],"raise_dynamic_class_load_error":false,"soft_includes":[]}},{"prototypes":{"includes":[],"path":"families/","raise_dynamic_class_load_error":false,"soft_includes":[]}}])"
  );
}

// Checks that changing any implicit package family field changes the cache key.
TEST(PackageInfoTest, EveryImplicitFamilyFieldChangesCacheMangle) {
  PackageInfo base;
  PackageInfo::ImplicitPackageFamily family;
  family.m_path = "www/prototypes/";
  family.m_includes.emplace("intern");
  family.m_soft_includes.emplace("soft");
  base.m_implicitPackageFamilies.emplace(
    "prototypes",
    std::move(family)
  );

  auto pathChanged = base;
  pathChanged.m_implicitPackageFamilies.at("prototypes").m_path =
    "www/other/";

  auto includesChanged = base;
  includesChanged.m_implicitPackageFamilies.at("prototypes")
    .m_includes.emplace("other");

  auto softIncludesChanged = base;
  softIncludesChanged.m_implicitPackageFamilies.at("prototypes")
    .m_soft_includes.emplace("other-soft");

  auto policyChanged = base;
  policyChanged.m_implicitPackageFamilies.at("prototypes")
    .m_raiseDynamicClassLoadError = true;

  auto const baseMangle = base.mangleForCacheKey();
  EXPECT_NE(baseMangle, pathChanged.mangleForCacheKey());
  EXPECT_NE(baseMangle, includesChanged.mangleForCacheKey());
  EXPECT_NE(baseMangle, softIncludesChanged.mangleForCacheKey());
  EXPECT_NE(baseMangle, policyChanged.mangleForCacheKey());
}

PackageInfo implicitPackageInfo() {
  PackageInfo info;
  PackageInfo::ImplicitPackageFamily family;
  family.m_path = "www/prototypes/";
  info.m_implicitPackageFamilies.emplace("prototypes", std::move(family));
  return info;
}

// Checks that an implicit package name resolves to its path prefix.
TEST(PackageInfoTest, ResolvesImplicitPackageNamesToPathPrefixes) {
  auto const info = implicitPackageInfo();
  EXPECT_EQ(
    info.implicitPackageNameToPathPrefix("prototypes.alpha"),
    std::optional<std::string>{"www/prototypes/alpha/"}
  );
}

// Checks that names without a declared implicit family do not resolve.
TEST(PackageInfoTest, RejectsUnknownImplicitPackageNamesToPathPrefixes) {
  auto const info = implicitPackageInfo();
  for (auto const name : {
         "prototypes",
         ".alpha",
         "prototypes.",
         "unknown.alpha",
       }) {
    EXPECT_FALSE(info.implicitPackageNameToPathPrefix(name).has_value()) << name;
  }
}

TEST(UnitEmitterAttributesTest, ResolvesPhysicalAndRepoRelativePaths) {
  folly::test::TemporaryDirectory temp{"unit-emitter-attributes"};
  auto const root = std::filesystem::path{temp.path().native()};
  std::filesystem::create_directories(root / "strict");
  std::filesystem::create_directories(root / "notice");
  std::filesystem::create_directories(root / "loose");

  std::ofstream config{root / ".hhvmconfig.hdf"};
  config << "Autoload {\n}\n";
  config.close();
  std::ofstream packages{root / "PACKAGES.toml"};
  packages << R"(
[packages]
[packages.strict]
include_paths = ["//strict/"]
enable_strict_isolation = true

[packages.notice]
include_paths = ["//notice/"]
enable_strict_isolation = true
raise_dynamic_class_load_error = false

[packages.loose]
include_paths = ["//loose/"]
)";
  packages.close();

  auto const& options = RepoOptions::forFile((root / "strict/C.php").c_str());
  auto const strict = UnitEmitterAttributes::forAbsolutePath(
    root / "loose/../strict/C.php", options
  );
  EXPECT_TRUE(strict.strictPackage);
  EXPECT_TRUE(strict.raiseDynamicClassLoadError);

#ifndef NDEBUG
  EXPECT_DEATH(
    UnitEmitterAttributes::forAbsolutePath("strict/C.php", options), ""
  );
#endif

  auto const notice = UnitEmitterAttributes::forRepoRelativePath(
    "notice/C.php", options.flags()
  );
  EXPECT_TRUE(notice.strictPackage);
  EXPECT_FALSE(notice.raiseDynamicClassLoadError);

  auto const loose = UnitEmitterAttributes::forAbsolutePath(
    root / "strict/../loose/C.php", options
  );
  EXPECT_FALSE(loose.strictPackage);
  EXPECT_FALSE(loose.raiseDynamicClassLoadError);
}

TEST(UnitEmitterAttributesTest, ParticipatesInUnitCacheKey) {
  auto const& options = RepoOptions::defaults().flags();
  auto const defaults = UnitEmitterAttributes::defaults();
  auto const strict = UnitEmitterAttributes{true, false};
  auto const enforced = UnitEmitterAttributes{true, true};

  auto const base = mangleUnitSha1("source", "file.php", options, defaults);
  EXPECT_NE(base, mangleUnitSha1("source", "file.php", options, strict));
  EXPECT_NE(
    mangleUnitSha1("source", "file.php", options, strict),
    mangleUnitSha1("source", "file.php", options, enforced)
  );
}

TEST(UnitEmitterAttributesTest, PreservedByUnitEmitterSerialization) {
  auto unit = std::make_unique<UnitEmitter>(
    SHA1{1},
    SHA1{2},
    RepoOptions::defaults().packageInfo(),
    UnitEmitterAttributes{true, true}
  );
  unit->m_filepath = makeStaticString("strict/C.php");
  unit->finish();

  UnitEmitterSerdeWrapper encoded{std::move(unit)};
  BlobEncoder encoder;
  encoded.serde(encoder);

  UnitEmitterSerdeWrapper decoded;
  BlobDecoder decoder{encoder.data(), encoder.size()};
  decoded.serde(decoder);
  decoder.assertDone();

  ASSERT_NE(decoded.m_ue, nullptr);
  EXPECT_TRUE(decoded.m_ue->m_attributes.strictPackage);
  EXPECT_TRUE(
    decoded.m_ue->m_attributes.raiseDynamicClassLoadError
  );
}

} // namespace
} // namespace HPHP
