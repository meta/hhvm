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

#include "hphp/runtime/base/runtime-option.h"

#include "hphp/hack/src/hackc/compile/options_gen.h"
#include "hphp/hack/src/hackc/ffi_bridge/compiler_ffi.rs.h"
#include "hphp/runtime/base/autoload-handler.h"
#include "hphp/runtime/base/bespoke-array.h"
#include "hphp/runtime/base/builtin-functions.h"
#include "hphp/runtime/base/coeffects-config.h"
#include "hphp/runtime/base/config.h"
#include "hphp/runtime/base/configs/server-loader.h"
#include "hphp/runtime/base/crash-reporter.h"
#include "hphp/runtime/base/execution-context.h"
#include "hphp/runtime/base/extended-logger.h"
#include "hphp/runtime/base/file-util-defs.h"
#include "hphp/runtime/base/file-util.h"
#include "hphp/runtime/base/hphp-system.h"
#include "hphp/runtime/base/ini-setting.h"
#include "hphp/runtime/base/init-fini-node.h"
#include "hphp/runtime/base/memory-manager.h"
#include "hphp/runtime/base/plain-file.h"
#include "hphp/runtime/base/req-heap-sanitizer.h"
#include "hphp/runtime/base/static-string-table.h"
#include "hphp/runtime/base/tv-refcount.h"
#include "hphp/runtime/base/zend-url.h"
#include "hphp/runtime/ext/extension-registry.h"
#include "hphp/runtime/ext/hash/hash_murmur.h"
#include "hphp/runtime/server/access-log.h"
#include "hphp/runtime/server/cli-server.h"
#include "hphp/runtime/server/files-match.h"
#include "hphp/runtime/server/virtual-host.h"
#include "hphp/runtime/vm/jit/mcgen-translate.h"
#include "hphp/util/arch.h"
#include "hphp/util/build-info.h"
#include "hphp/util/bump-mapper.h"
#include "hphp/util/configs/adminserver.h"
#include "hphp/util/configs/debug.h"
#include "hphp/util/configs/eval.h"
#include "hphp/util/configs/gc.h"
#include "hphp/util/configs/mail.h"
#include "hphp/util/configs/repo.h"
#include "hphp/util/configs/sandbox.h"
#include "hphp/util/configs/server.h"
#include "hphp/util/configs/log.h"
#include "hphp/util/current-executable.h" // @donotremove
#include "hphp/util/hardware-counter.h"
#include "hphp/util/hdf.h"
#include "hphp/util/hdf-extract.h"
#include "hphp/util/log-file-flusher.h"
#include "hphp/util/logger.h"
#include "hphp/util/network.h"
#include "hphp/util/process.h"
#include "hphp/util/roar.h"
#include "hphp/util/service-data.h"
#include "hphp/util/stack-trace.h"
#include "hphp/util/struct-log.h"
#include "hphp/zend/zend-string.h"

#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

#include <boost/algorithm/string/predicate.hpp>
#include <fmt/format.h>
#include <folly/Conv.h>
#include <folly/json/DynamicConverter.h>
#include <folly/FileUtil.h>
#include <folly/String.h>
#include <folly/portability/GFlags.h>
#include <folly/portability/SysResource.h>
#include <folly/portability/SysTime.h>
#include <folly/portability/Unistd.h>

#ifdef __aarch64__
#include <sys/auxv.h>
#include <asm/hwcap.h>
#endif

#include "hphp/runtime/base/datetime.h"

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

RepoOptions RepoOptions::s_defaults;
RepoOptions RepoOptions::s_defaultsForSystemlib;

namespace {

std::vector<std::string> s_RelativeConfigs;

////////////////////////////////////////////////////////////////////////////////

void mangleForKey(bool b, std::string& s) { s += (b ? '1' : '0'); }
void mangleForKey(const Cfg::StringStringMap& map, std::string& s) {
  s += folly::to<std::string>(map.size());
  s += '\0';
  for (auto const& par : map) {
    s += par.first;
    s += '\0';
    s += par.second;
    s += '\0';
  }
}
void mangleForKey(const Cfg::StringVector& vec, std::string& s) {
  s += folly::to<std::string>(vec.size());
  s += '\0';
  for (auto const& val : vec) {
    s += val;
    s += '\0';
  }
}
void mangleForKey(const std::string& s1, std::string& s2) { s2 += s1; }

folly::dynamic toIniValue(bool b) {
  return b ? "1" : "0";
}

folly::dynamic toIniValue(const Cfg::StringStringMap& map) {
  folly::dynamic obj = folly::dynamic::object();
  for (auto& kv : map) {
    obj[kv.first] = kv.second;
  }
  return obj;
}

folly::dynamic toIniValue(const Cfg::StringVector& vec) {
  folly::dynamic obj = folly::dynamic::array();
  for (auto& val : vec) {
    obj.push_back(val);
  }
  return obj;
}

folly::dynamic toIniValue(const std::string& str) {
  return str;
}

struct CachedRepoOptions {
  CachedRepoOptions() = default;
  explicit CachedRepoOptions(RepoOptions&& opts)
    : options(new RepoOptions(std::move(opts)))
  {}
  CachedRepoOptions(const CachedRepoOptions& opts)
    : options(nullptr)
  {
    if (auto o = opts.options.load(std::memory_order_acquire)) {
      options.store(new RepoOptions(*o), std::memory_order_release);
    }
  }
  ~CachedRepoOptions() {
    Treadmill::enqueue([opt = options.exchange(nullptr)] { delete opt; });
  }

  CachedRepoOptions& operator=(const CachedRepoOptions& opts) {
    auto const o = opts.options.load(std::memory_order_acquire);
    auto const old = options.exchange(o ? new RepoOptions(*o) : nullptr);
    if (old) Treadmill::enqueue([old] { delete old; });
    return *this;
  }

  static bool isChanged(const RepoOptions* opts, const RepoOptionStats& st) {
    auto const compare = [&] (Optional<struct stat> o,
                              Optional<struct stat> s) {
      if (!o.has_value() && !s.has_value()) return false;
      if (o.has_value() != s.has_value()) return true;
      return
        s->st_mtim.tv_sec  != o->st_mtim.tv_sec ||
        s->st_mtim.tv_nsec != o->st_mtim.tv_nsec ||
        s->st_ctim.tv_sec  != o->st_ctim.tv_sec ||
        s->st_ctim.tv_nsec != o->st_ctim.tv_nsec ||
        s->st_dev != o->st_dev ||
        s->st_ino != o->st_ino;
    };
    return compare(opts->stat().m_configStat, st.m_configStat)
           || compare(opts->stat().m_packageStat, st.m_packageStat);
  }

  const RepoOptions* update(RepoOptions&& opts) const {
    auto const val = new RepoOptions(std::move(opts));
    auto const old = options.exchange(val);
    if (old) Treadmill::enqueue([old] { delete old; });
    return val;
  }

  const RepoOptions* fetch(const RepoOptionStats& st) const {
    auto const opts = options.load(std::memory_order_acquire);
    return opts && !isChanged(opts, st) ? opts : nullptr;
  }

  mutable std::atomic<RepoOptions*> options{nullptr};
};

struct RepoOptionCacheKey {
  Stream::Wrapper* wrapper;
  std::string filename;
};
struct RepoOptionCacheKeyCompare {
  bool equal(const RepoOptionCacheKey& k1, const RepoOptionCacheKey& k2) const {
    return k1.wrapper == k2.wrapper && k1.filename == k2.filename;
  }
  size_t hash(const RepoOptionCacheKey& k) const {
    return std::hash<Stream::Wrapper*>()(k.wrapper) ^
           hash_string_cs_unsafe(k.filename.c_str(), k.filename.size());
  }
};
using RepoOptionCache = tbb::concurrent_hash_map<
  RepoOptionCacheKey,
  CachedRepoOptions,
  RepoOptionCacheKeyCompare
>;
RepoOptionCache s_repoOptionCache;

constexpr const char* kHhvmConfigHdf = ".hhvmconfig.hdf";

template<class F>
bool walkDirTree(std::string fpath, F func) {
  do {
    auto const off = fpath.rfind('/');
    if (off == std::string::npos) return false;
    fpath.resize(off);
    fpath += '/';
    fpath += kHhvmConfigHdf;

    if (func(fpath)) return true;

    fpath.resize(off);
  } while (!fpath.empty() && fpath != "/");
  return false;
}

RDS_LOCAL(std::string, s_lastSeenRepoConfig);

}

RepoOptionStats::RepoOptionStats(const std::string& configPath,
                                 Stream::Wrapper* wrapper)
  : m_configStat(std::nullopt)
  , m_packageStat(std::nullopt)
{
  auto const wrapped_stat = [&](const char* path, struct stat* st) {
    if (wrapper) return wrapper->stat(path, st);
    return ::stat(path, st);
  };
  struct stat config;
  if (wrapped_stat(configPath.data(), &config) == 0) m_configStat = config;
  // Same directory
  auto const repo =
    configPath.empty() ? "" : std::filesystem::path(configPath).parent_path();
  auto const packagePath = repo / Cfg::Eval::PackagesTomlFileName;
  if (std::filesystem::exists(packagePath)) {
    struct stat package;
    if (wrapped_stat(packagePath.string().data(), &package) == 0) {
      m_packageStat = package;
    }
  }
}

ParserEnv RepoOptionsFlags::getParserEnvironment() const {
  return ParserEnv {
      true // codegen
    , true  // hhvm_compat_mode
    , true  // php5_compat_mode
    , EnableXHPClassModifier
    , DisableXHPElementMangling
    , false // disable_xhp_children_declarations
    , true  // interpret_soft_types_as_like_types
    };
}

void RepoOptionsFlags::initAliasedNamespaces(hackc::NativeEnv& env) const {
  for (auto& [k, v] : AliasedNamespaces) {
    env.aliased_namespaces.emplace_back(hackc::StringMapEntry{k, v});
  }
}

void RepoOptionsFlags::initExperimentalFeatures(hackc::NativeEnv& env) const {
  for (auto& [k, v] : ExperimentalFeatures) {
    env.experimental_features.emplace_back(hackc::StringMapEntry{k, v});
  }
  env.use_legacy_experimental_feature_config = UseLegacyExperimentalFeatureConfig;
  env.consider_unspecified_experimental_features_released = ConsiderUnspecifiedExperimentalFeaturesReleased;
}

void RepoOptionsFlags::initDeclConfig(hackc::DeclParserConfig& config) const {
  for (auto& [k, v] : AliasedNamespaces) {
    config.aliased_namespaces.emplace_back(hackc::StringMapEntry{k, v});
  }
  config.disable_xhp_element_mangling = DisableXHPElementMangling;
  config.interpret_soft_types_as_like_types = true;
  config.enable_xhp_class_modifier = EnableXHPClassModifier;
  config.php5_compat_mode = true;
  config.hhvm_compat_mode = true;
  config.use_obr_decls = UseObrDecls;
}

void RepoOptionsFlags::initHhbcFlags(hackc::HhbcFlags& flags) const {
  Cfg::InitHackcHHBCFlags(*this, flags);
}

void RepoOptionsFlags::initParserFlags(hackc::ParserFlags& flags) const {
  Cfg::InitHackcParserFlags(*this, flags);
}

void RepoOptionsFlags::calcCachedQuery() {
  if (Query.empty()) return;
  try {
    m_cachedQuery = folly::parseJson(Query);
  } catch (const folly::json::parse_error& ) { /* swallow error */ }
}

const RepoOptions& RepoOptions::forFile(const std::string& path) {
  tracing::BlockNoTrace _{"repo-options"};

  if (boost::starts_with(path, "/:")) return defaults();

  // Fast path: we have an active request and it has cached a RepoOptions
  // which has not been modified. It can cause us to miss out on
  // configs that were added between the current directory and the source file.
  // (Loading these configs would result in a fatal anyway with this option)
  if (!g_context.isNull()) {
    if (auto const opts = g_context->getRepoOptionsForRequest()) {
      // If path() is empty we have the default() options, which means we have
      // negatively cached the existence of a .hhvmconfig.hdf for this request.
      if (opts->path().empty()) return *opts;

      // Don't bother checking if the file is changed. This cache is request
      // local and within any given request we want to use a consistent version
      // of the RepoOptions anyway.
      if (boost::starts_with(std::filesystem::path{path}, opts->dir())) {
        return *opts;
      }
    }
  }

  auto const isParentOf = [] (const std::filesystem::path& p1, const std::string& p2) {
    return boost::starts_with(std::filesystem::path{p2}, p1.parent_path());
  };

  // Wrap filesystem accesses if needed to proxy info from cli server client.
  Stream::Wrapper* wrapper = nullptr;
  if (is_cli_server_mode()) {
    wrapper = Stream::getWrapperFromURI(path, nullptr);
    if (wrapper && !wrapper->isNormalFileStream()) wrapper = nullptr;
  }
  auto const wrapped_open = [&](const char* path) -> Optional<OptString> {
    if (wrapper) {
      if (auto const file = wrapper->open(path, "r", 0, nullptr)) {
        return file->read();
      }
      return std::nullopt;
    }

    auto const fd = open(path, O_RDONLY);
    if (fd < 0) return std::nullopt;
    auto file = req::make<PlainFile>(fd);
    return file->read();
  };

  auto const set = [&] (
    RepoOptionCache::const_accessor& rpathAcc,
    const std::string& path,
    const RepoOptionStats& st
  ) -> const RepoOptions* {
    *s_lastSeenRepoConfig = path;
    if (auto const opts = rpathAcc->second.fetch(st)) {
      return opts;
    }
    auto const contents = wrapped_open(path.data());
    if (!contents) return nullptr;
    RepoOptions newOpts{ contents->data(), path.data()};
    newOpts.m_stat = st;
    return rpathAcc->second.update(std::move(newOpts));
  };

  auto const test = [&] (const std::string& path) -> const RepoOptions* {
    RepoOptionCache::const_accessor rpathAcc;
    const RepoOptionCacheKey key {wrapper, path};
    if (!s_repoOptionCache.find(rpathAcc, key)) return nullptr;
    RepoOptionStats st(path, wrapper);
    if (st.missing()) {
      s_repoOptionCache.erase(rpathAcc);
      return nullptr;
    }
    return set(rpathAcc, path, st);
  };

  const RepoOptions* ret{nullptr};

  // WARNING: when Eval.CachePerRepoOptionsPath we cache the last used path for
  //          RepoOptions per thread, and while we will detect changes to this
  //          file, and do a rescan in the event that it is deleted or doesn't
  //          match the current file being loaded, we will miss out on new
  //          configs that may be added. Since we expect to see a single config
  //          per repository we expect that this will be a reasonably safe,
  //          optimization.
  if (Cfg::Eval::CachePerRepoOptionsPath) {
    if (!s_lastSeenRepoConfig->empty() &&
        isParentOf(*s_lastSeenRepoConfig, path)) {
      if (auto const r = test(*s_lastSeenRepoConfig)) return *r;
      s_lastSeenRepoConfig->clear();
    }

    // If the last seen path isn't set yet or is no longer accurate try checking
    // other cached paths before falling back to the filesystem.
    walkDirTree(path, [&] (const std::string& path) {
      return (ret = test(path)) != nullptr;
    });
  }

  if (ret) return *ret;

  walkDirTree(path, [&] (const std::string& path) {
    RepoOptionStats st(path, wrapper);
    if (st.missing()) return false;
    RepoOptionCache::const_accessor rpathAcc;
    const RepoOptionCacheKey key {wrapper, path};
    s_repoOptionCache.insert(rpathAcc, key);
    ret = set(rpathAcc, path, st);
    return true;
  });

  return ret ? *ret : defaults();
}

void RepoOptions::calcCacheKey() {
  std::string raw;
#define C(_, n) mangleForKey(m_flags.n, raw);
CONFIGS_FOR_REPOOPTIONSFLAGS()
#undef C

  mangleForKey(packageInfo().mangleForCacheKey(), raw);
  m_flags.m_sha1 = SHA1{string_sha1(raw)};
}

namespace {
std::string getCacheBreakerSchemaHash(std::string_view root,
                                      const RepoOptionsFlags& flags) {
  std::string optsHash = Cfg::Eval::IncludeReopOptionsInFactsCacheBreaker
      ? flags.cacheKeySha1().toString()
      : flags.getFactsCacheBreaker();

  constexpr std::string_view kCacheBreakerFmt =
    "Native Facts DB cache breaker:\n"
    " Version: {}\n"
    " Root: {}\n"
    " RepoOpts hash: {}";
  if (Cfg::Server::Mode && !is_cli_server_mode()) {
    Logger::FInfo(kCacheBreakerFmt, Facts::kSchemaVersion, root, optsHash);
  } else {
    Logger::FVerbose(kCacheBreakerFmt, Facts::kSchemaVersion, root, optsHash);
  }

  std::string rootHash = string_sha1(root);
  optsHash.resize(10);
  rootHash.resize(10);
  return folly::to<std::string>(Facts::kSchemaVersion, '_', optsHash, '_',
                                rootHash);
}
}

constexpr std::string_view kEUIDPlaceholder = "%{euid}";
constexpr std::string_view kSchemaPlaceholder = "%{schema}";
void RepoOptions::calcAutoloadDB() {
  namespace fs = std::filesystem;

  if (Cfg::Autoload::DBPath.empty()) return;
  std::string pathTemplate{Cfg::Autoload::DBPath};

  auto const euidIdx = pathTemplate.find(kEUIDPlaceholder);
  if (euidIdx != std::string::npos) {
    pathTemplate.replace(
        euidIdx, kEUIDPlaceholder.size(), folly::to<std::string>(geteuid()));
  }

  auto const schemaIdx = pathTemplate.find(kSchemaPlaceholder);
  if (schemaIdx != std::string::npos) {
    pathTemplate.replace(
        schemaIdx,
        kSchemaPlaceholder.size(),
        getCacheBreakerSchemaHash(m_repo.native(), m_flags));
  }

  fs::path dbPath = pathTemplate;
  if (dbPath.is_relative()) dbPath = m_repo / dbPath;
  m_autoloadDB = fs::absolute(dbPath);
}

const RepoOptions& RepoOptions::defaults() {
  always_assert(s_defaults.m_init);
  return s_defaults;
}

const RepoOptions& RepoOptions::defaultsForSystemlib() {
  if (!s_defaultsForSystemlib.m_init) {
    s_defaultsForSystemlib.initDefaultsForSystemlib();
  }
  return s_defaultsForSystemlib;
}

void RepoOptions::filterNamespaces() {
  for (auto it = m_flags.AliasedNamespaces.begin();
       it != m_flags.AliasedNamespaces.end(); ) {
    if (!is_valid_class_name(it->second)) {
      Logger::Warning("Skipping invalid AliasedNamespace %s\n",
                      it->second.c_str());
      it = m_flags.AliasedNamespaces.erase(it);
      continue;
    }

    while (it->second.size() && it->second[0] == '\\') {
      it->second = it->second.substr(1);
    }

    ++it;
  }
}

RepoOptions::RepoOptions(const char* str, const char* file) : m_path(file), m_init(true) {
  always_assert(s_defaults.m_init);
  Hdf config{};
  config.fromString(str);

  Cfg::GetRepoOptionsFlagsFromConfig(m_flags, config, s_defaults.m_flags);

  hdfExtract(config, "Autoload.CacheBreaker", m_flags.m_factsCacheBreaker,
             "d6ede7d391");

  filterNamespaces();
  if (!m_path.empty()) {
      m_repo = std::filesystem::canonical(m_path.parent_path());
  }
  m_flags.m_packageInfo = PackageInfo::fromFile(
    m_repo / Cfg::Eval::PackagesTomlFileName,
    m_flags.EnableImplicitPackages
  );
  calcCacheKey();
  calcAutoloadDB();
  m_flags.calcCachedQuery();
}

void RepoOptions::initDefaults(const Hdf& hdf, const IniSettingMap& ini) {
  Cfg::GetRepoOptionsFlags(m_flags, ini, hdf);

  filterNamespaces();
  m_path.clear();
  m_flags.m_packageInfo = PackageInfo::defaults();
  calcCacheKey();
  m_init = true;
}

void RepoOptions::initDefaultsForSystemlib() {
  Cfg::GetRepoOptionsFlagsForSystemlib(m_flags);

  filterNamespaces();
  m_path.clear();
  m_flags.m_packageInfo = PackageInfo::defaults();
  calcCacheKey();
  m_init = true;
}

void RepoOptions::setDefaults(const Hdf& hdf, const IniSettingMap& ini) {
  always_assert(!s_defaults.m_init);
  s_defaults.initDefaults(hdf, ini);
}

///////////////////////////////////////////////////////////////////////////////

std::map<std::string, std::string> RuntimeOption::TierOverwriteInputs;

std::string RuntimeOption::BuildId;
std::string RuntimeOption::InstanceId;
std::string RuntimeOption::DeploymentId;
int64_t RuntimeOption::ConfigId = 0;
std::string RuntimeOption::PidFile = "www.pid";

bool RuntimeOption::EnableZendIniCompat = true;
bool RuntimeOption::DumpPreciseProfData = true;

JitSerdesMode RuntimeOption::EvalJitSerdesMode{};

std::string RuntimeOption::EvalSBSerdesFile;

std::map<std::string, ErrorLogFileData> RuntimeOption::ErrorLogs = {
  {Logger::DEFAULT, ErrorLogFileData()},
};

int RuntimeOption::RuntimeErrorReportingLevel =
  static_cast<int>(ErrorMode::HPHP_ALL);

std::vector<std::string> RuntimeOption::TzdataSearchPaths;
std::map<std::string, std::string> RuntimeOption::GFlags;
hphp_fast_string_set RuntimeOption::ActiveExperiments;
hphp_fast_string_set RuntimeOption::InactiveExperiments;

int RuntimeOption::RaiseDebuggingFrequency = 1;
int64_t RuntimeOption::SerializationSizeLimit = StringData::MaxSize;

std::string RuntimeOption::AccessLogDefaultFormat = "%h %l %u %t \"%r\" %>s %b";
std::map<std::string, AccessLogFileData> RuntimeOption::AccessLogs;

std::map<std::string, AccessLogFileData> RuntimeOption::RPCLogs;

std::vector<std::shared_ptr<VirtualHost>> RuntimeOption::VirtualHosts;
std::shared_ptr<IpBlockMap> RuntimeOption::IpBlocks;

std::map<std::string, std::string> RuntimeOption::IncludeRoots;

hphp_string_imap<std::string> RuntimeOption::StaticFileExtensions;
hphp_string_imap<std::string> RuntimeOption::PhpFileExtensions;
std::vector<std::shared_ptr<FilesMatch>> RuntimeOption::FilesMatches;

std::set<std::string, stdltistr> RuntimeOption::TraceFunctions;

int64_t RuntimeOption::MaxSQLRowCount = 0;
int64_t RuntimeOption::SocketDefaultTimeout = 60;

std::map<std::string, std::string> RuntimeOption::ServerVariables;
std::map<std::string, std::string> RuntimeOption::EnvVariables;

std::map<std::string, std::string>& RuntimeOption::GetMetadata() {
  static std::map<std::string, std::string> Metadata;
  return Metadata;
}

const std::string& RuntimeOption::GetServerPrimaryIPv4() {
   static std::string serverPrimaryIPv4 = GetPrimaryIPv4();
   return serverPrimaryIPv4;
}

const std::string& RuntimeOption::GetServerPrimaryIPv6() {
   static std::string serverPrimaryIPv6 = GetPrimaryIPv6();
   return serverPrimaryIPv6;
}

static inline std::string regionSelectorDefault() {
  return "tracelet";
}

static inline bool pgoDefault() {
#ifdef HHVM_NO_DEFAULT_PGO
  return false;
#else
  return true;
#endif
}

static inline uint64_t pgoThresholdDefault() {
  return debug ? 2 : 2000;
}

static inline bool hugePagesSoundNice() {
  return Cfg::Server::Mode;
}

static inline uint32_t hotTextHugePagesDefault() {
  if (!hugePagesSoundNice()) return 0;
  return ARCH_MATCH(
    [](arch::X64) { return 8; },
    [](arch::ARM) { return 12; }
  );
}

static inline std::string reorderPropsDefault() {
  if (isJitDeserializing()) {
    return "countedness-hotness";
  }
  return debug ? "alphabetical" : "countedness";
}

Optional<std::filesystem::path> RuntimeOption::GetHomePath(
    const folly::StringPiece user) {
  namespace fs = std::filesystem;

  auto homePath = fs::path{Cfg::Sandbox::Home} / fs::path{std::string{user}};
  if (fs::is_directory(homePath)) {
    return make_optional(std::move(homePath));
  }

  if (!Cfg::Sandbox::Fallback.empty()) {
    homePath = fs::path{Cfg::Sandbox::Fallback} / fs::path{std::string{user}};
    if (fs::is_directory(homePath)) {
      return make_optional(std::move(homePath));
    }
  }

  return std::nullopt;
}

bool RuntimeOption::funcIsRenamable(const StringData* name) {
  if (HPHP::is_generated(name)) return false;
  if (Cfg::Jit::EnableRenameFunction == 0) return false;
  if (Cfg::Jit::EnableRenameFunction == 2) {
    return Cfg::Eval::RenamableFunctions.find(name->data()) !=
      Cfg::Eval::RenamableFunctions.end();
  } else {
    return true;
  }
}

std::string RuntimeOption::GetDefaultUser() {
  if (Cfg::Sandbox::DefaultUserFile.empty()) return {};

  std::filesystem::path file{Cfg::Sandbox::DefaultUserFile};
  if (!std::filesystem::is_regular_file(file)) return {};

  std::string user;
  if (!folly::readFile(file.c_str(), user) || user.empty()) return {};

  return user;
}

bool RuntimeOption::ReadPerUserSettings(const std::filesystem::path& confFileName,
                                        IniSettingMap& ini, Hdf& config) {
  try {
    Config::ParseConfigFile(confFileName.native(), ini, config, false);
    return true;
  } catch (HdfException& e) {
    Logger::Error("%s ignored: %s", confFileName.native().c_str(),
                  e.getMessage().c_str());
    return false;
  }
}

std::string RuntimeOption::getTraceOutputFile() {
  return fmt::format("{}/hphp.{}.log",
                        Cfg::Debug::RemoteTraceOutputDir, (int64_t)getpid());
}

using std::string;
#define F(type, name, def) \
  type RuntimeOption::Eval ## name = type(def);
EVALFLAGS()
#undef F
hphp_string_map<TypedValue> RuntimeOption::ConstantFunctions;

#ifdef HHVM_FACEBOOK

int RuntimeOption::ThriftFBServerThriftServerIOWorkerThreads = 1;
int RuntimeOption::ThriftFBServerThriftServerCPUWorkerThreads = 1;
std::set<std::string> RuntimeOption::ThriftFBServerHighPriorityEndPoints;

bool RuntimeOption::EnableFb303Server = false;
int RuntimeOption::Fb303ServerPort = 0;
std::string RuntimeOption::Fb303ServerIP;
int RuntimeOption::Fb303ServerWorkerThreads = 1;
int RuntimeOption::Fb303ServerPoolThreads = 1;
bool RuntimeOption::Fb303ServerExposeSensitiveMethods = false;
#endif

size_t serverThreadTuneMaxThreadCount(size_t baselineThreadCount) {
#ifdef HHVM_FACEBOOK
  const size_t adjustmentFactor =
      baselineThreadCount * Cfg::Server::ThreadTuneAdjustmentPct / 100;

  // Should never happen, but protect against integer overflow
  if (baselineThreadCount >
      std::numeric_limits<size_t>::max() - adjustmentFactor) {
    return std::numeric_limits<size_t>::max();
  }

  return baselineThreadCount + adjustmentFactor;
#else
  return baselineThreadCount;
#endif
}

std::map<std::string, std::string> RuntimeOption::CustomSettings;

#ifdef NDEBUG
  #ifdef ALWAYS_ASSERT
    const StaticString s_hhvm_build_type("Release with asserts");
  #else
    const StaticString s_hhvm_build_type("Release");
  #endif
#else
  const StaticString s_hhvm_build_type("Debug");
#endif

///////////////////////////////////////////////////////////////////////////////

static void setResourceLimit(int resource, const IniSetting::Map& ini,
                             const Hdf& rlimit, const char* nodeName) {
  if (!Config::GetString(ini, rlimit, nodeName).empty()) {
    struct rlimit rl;
    getrlimit(resource, &rl);
    rl.rlim_cur = Config::GetInt64(ini, rlimit, nodeName);
    if (rl.rlim_max < rl.rlim_cur) {
      rl.rlim_max = rl.rlim_cur;
    }
    int ret = setrlimit(resource, &rl);
    if (ret) {
      Logger::Error("Unable to set %s to %" PRId64 ": %s (%d)",
                    nodeName, (int64_t)rl.rlim_cur,
                    folly::errnoStr(errno).c_str(), errno);
    }
  }
}

static void normalizePath(std::string &path) {
  if (!path.empty()) {
    if (path[path.length() - 1] == '/') {
      path = path.substr(0, path.length() - 1);
    }
    if (path[0] != '/') {
      path = std::string("/") + path;
    }
  }
}

static bool matchShard(
  bool enableShards,
  const std::string& hostname,
  const IniSetting::Map& ini, Hdf hdfPattern,
  std::vector<std::string>& messages
) {
  if (!hdfPattern.exists("Shard")) return true;

  if (!enableShards) {
    hdfPattern["Shard"].setVisited();
    hdfPattern["ShardCount"].setVisited();
    hdfPattern["ShardSalt"].setVisited();
    return false;
  }

  auto const shard = Config::GetInt64(ini, hdfPattern, "Shard", -1, false);

  auto const nshards =
    Config::GetInt64(ini, hdfPattern, "ShardCount", 100, false);

  if (shard < 0 || shard >= nshards) {
    messages.push_back(fmt::format("Invalid value for Shard: {}", shard));
    return true;
  }

  auto input = hostname;
  if (hdfPattern.exists("ShardSalt")) {
    input += Config::GetString(ini, hdfPattern, "ShardSalt", "", false);
  }

  auto const md5 = Md5Digest(input.data(), input.size());
  uint32_t seed{0};
  memcpy(&seed, &md5.digest[0], 4);

  // This shift is to match the behavior of sharding in chef which appears to
  // have an off-by-one bug:
  //   seed = Digest::MD5.hexdigest(seed_input)[0...7].to_i(16)
  seed = ntohl(seed) >> 4;

  messages.push_back(fmt::format(
    "Checking Shard = {}; Input = {}; Seed = {}; ShardCount = {}; Value = {}",
    shard, input, seed, nshards, seed % nshards
  ));

  return seed % nshards <= shard;
}

static Optional<uint64_t> hostNumber(const std::string& hostname) {
  auto const pos = hostname.find_first_of("0123456789");
  if (pos == std::string::npos) return {};

  try {
    return std::stoll(hostname.substr(pos));
  } catch (const std::invalid_argument&) {
  } catch (const std::out_of_range&) {
  }

  return {};
}

static bool matchExperiment(
  bool enableShards,
  const std::string& hostname,
  const IniSetting::Map& ini, Hdf& config,
  std::vector<std::string>& messages
) {
  if (!config.exists("Experiment")) return true;
  if (!config.exists("Experiment.name")) {
    config["Experiment"].setVisited();
    messages.push_back(
      "Detected misconfigured experiment with no name, skipping"
    );
    return false;
  }

  if (!enableShards) {
    config["Experiment"].setVisited();
    return false;
  }

  auto const rate = Config::GetInt64(ini, config, "Experiment.rate", 2);
  auto const flip = Config::GetBool(ini, config, "Experiment.flip", false);
  auto const name = Config::GetString(ini, config, "Experiment.name");

  if (rate == 0) return flip;
  if (rate == 1) return !flip;

  auto const num  = hostNumber(hostname).value_or(12345678);
  auto const hash = murmur_hash_64A(name.data(), name.size(), num);
  auto const res  = hash % rate == 0;

  messages.push_back(fmt::format(
    "Checking Experiment `{}'; hostname = {}; Seed = {}; Hash = {}; Flip = {}",
    name, hostname, num, hash, flip ? "true" : "false"
  ));

  return flip ? !res : res;
}

// A machine can belong to a tier, which can overwrite
// various settings, even if they are set in the same
// hdf file. However, CLI overrides still win the day over
// everything.
static std::vector<std::string> getTierOverwrites(
  IniSetting::Map& ini,
  Hdf& config,
  hphp_fast_string_set& active_exps,
  hphp_fast_string_set& inactive_exps
) {

  // Machine metrics
  string hostname, tier, task, cpu, tiers, tags;
  {
    hostname = Config::GetString(ini, config, "Machine.name");
    if (hostname.empty()) {
      hostname = Process::GetHostName();
    }

    tier = Config::GetString(ini, config, "Machine.tier");

    task = Config::GetString(ini, config, "Machine.task");

    cpu = Config::GetString(ini, config, "Machine.cpu");
    if (cpu.empty()) {
      cpu = Process::GetCPUModel();
    }

    tiers = Config::GetString(ini, config, "Machine.tiers");
    if (!tiers.empty()) {
      if (!folly::readFile(tiers.c_str(), tiers)) {
        tiers.clear();
      }
    }

    tags = Config::GetString(ini, config, "Machine.tags");
    if (!tags.empty()) {
      if (!folly::readFile(tags.c_str(), tags)) {
        tags.clear();
      }
    }
  }

  auto const checkPatterns = [&] (Hdf hdf) {
    // Check the patterns one by one so they all get evaluated; otherwise, when
    // using "&&" in a single expression with multiple patterns, if an earlier
    // one fails to match, the later one would be reported as unused.
    auto matched = true;
    matched &= Config::matchHdfPattern(hostname, ini, hdf, "machine");
    matched &= Config::matchHdfPattern(tier, ini, hdf, "tier");
    matched &= Config::matchHdfPattern(task, ini, hdf, "task");
    matched &= Config::matchHdfPattern(tiers, ini, hdf, "tiers", "m");
    matched &= Config::matchHdfPattern(tags, ini, hdf, "tags", "m");
    matched &= Config::matchHdfPatternSet(tags, ini, hdf, "tagset");
    matched &= Config::matchHdfPattern(cpu, ini, hdf, "cpu");
    return matched;
  };

  std::vector<std::string> messages;
  auto enableShards = !config["DisableShards"].configGetBool();

  auto const matchesTier = [&] (Hdf hdf) {
    // Check the patterns one by one so they all get evaluated; otherwise, when
    // using "&&" in a single expression with multiple patterns, if an earlier
    // one fails to match, the later one would be reported as unused.
    auto matched = true;
    matched &= checkPatterns(hdf);
    matched &= !hdf.exists("exclude") || !checkPatterns(hdf["exclude"]);
    matched &= matchShard(enableShards, hostname, ini, hdf, messages);
    return matched;
  };

  // Tier overwrites
  {
    // Required to get log entries actually written to scuba
    for (Hdf hdf = config["Tiers"].firstChild(); hdf.exists();
         hdf = hdf.next()) {
      if (messages.empty()) {
        messages.emplace_back(fmt::format(
                                "Matching tiers using: "
                                "machine='{}', tier='{}', task='{}', "
                                "cpu='{}', tiers='{}', tags='{}'",
                                hostname, tier, task, cpu, tiers, tags));

        RuntimeOption::StoreTierOverwriteInputs(hostname, tier, task, cpu, tiers, tags);
      }
      auto const matches = matchesTier(hdf);
      auto const matches_exp = matchExperiment(
        enableShards, hostname, ini, hdf, messages
      );
      if (matches && matches_exp) {
        messages.emplace_back(fmt::format(
                                "Matched tier: {}", hdf.getName()));
        if (enableShards && hdf["DisableShards"].configGetBool()) {
          messages.emplace_back("Sharding is disabled.");
          enableShards = false;
        }
        if (hdf.exists("clear")) {
          std::vector<std::string> list;
          hdf["clear"].configGet(list);
          for (auto const& s : list) {
            config.remove(s);
          }
        }
        if (hdf.exists("Experiment.name")) {
          active_exps.emplace(Config::GetString(ini, hdf, "Experiment.name"));
        }
        config.copy(hdf["overwrite"]);
        // no break here, so we can continue to match more overwrites
      } else if (matches && !matches_exp && hdf.exists("Experiment.name")) {
        inactive_exps.emplace(Config::GetString(ini, hdf, "Experiment.name"));
      }
      // Avoid lint errors about unvisited nodes when the tier does not match.
      hdf["DisableShards"].setVisited();
      hdf["clear"].setVisited();
      hdf["overwrite"].setVisited();
    }
  }
  return messages;
}

// Log the inputs used for calculating tier overwrites for this machine
void RuntimeOption::StoreTierOverwriteInputs(const std::string &machine, const std::string &tier,
  const std::string &task, const std::string &cpu, const std::string &tiers, const std::string &tags) {
    // Store the tier inputs as part of the RuntimeOption singleton, so that we can access it later
    RuntimeOption::TierOverwriteInputs["machine"] = machine;
    RuntimeOption::TierOverwriteInputs["tier"] = tier;
    RuntimeOption::TierOverwriteInputs["task"] = task;
    RuntimeOption::TierOverwriteInputs["cpu"] = cpu;
    RuntimeOption::TierOverwriteInputs["tiers"] = tiers;
    RuntimeOption::TierOverwriteInputs["tags"] = tags;
}

void logTierOverwriteInputs() {
  if(!Cfg::Server::LogTierOverwriteInputs) return;

  StructuredLogEntry log_entry;
  log_entry.force_init = true;
  log_entry.setStr("machine", RuntimeOption::TierOverwriteInputs["machine"]);
  log_entry.setStr("tier", RuntimeOption::TierOverwriteInputs["tier"]);
  log_entry.setStr("task", RuntimeOption::TierOverwriteInputs["task"]);
  log_entry.setStr("cpu", RuntimeOption::TierOverwriteInputs["cpu"]);
  log_entry.setStr("tiers", RuntimeOption::TierOverwriteInputs["tiers"]);
  log_entry.setStr("tags", RuntimeOption::TierOverwriteInputs["tags"]);
  log_entry.setStr("config_file_name", RuntimeOption::GetMetadata()["ConfigFileName"]);
  StructuredLog::log("hhvm_config_hdf_logs", log_entry);
}

InitFiniNode s_logTierOverwrites(logTierOverwriteInputs, InitFiniNode::When::ServerInit);

extern void initialize_apc();
void RuntimeOption::Load(
  IniSetting::Map& ini, Hdf& config,
  const std::vector<std::string>& iniClis /* = std::vector<std::string>() */,
  const std::vector<std::string>& hdfClis /* = std::vector<std::string>() */,
  std::vector<std::string>* messages /* = nullptr */,
  std::string cmd /* = "" */) {

  // Intialize the memory manager here because various settings and
  // initializations that we do here need it
  tl_heap.getCheck();

  // Store the config file name for logging.
  Config::Bind(GetMetadata(), ini, config, "Metadata");

  // Get the ini (-d) and hdf (-v) strings, which may override some
  // of options that were set from config files. We also do these
  // now so we can override Tier.*.[machine | tier | cpu] on the
  // command line, along with any fields within a Tier (e.g.,
  // CoreFileSize)
  for (auto& istr : iniClis) {
    Config::ParseIniString(istr, ini);
  }
  for (auto& hstr : hdfClis) {
    Config::ParseHdfString(hstr, config);
  }

  // See if there are any Tier-based overrides
  auto m = getTierOverwrites(ini, config, ActiveExperiments,
                             InactiveExperiments);
  if (messages) *messages = std::move(m);

  // RelativeConfigs can be set by commandline flags and tier overwrites, they
  // may also contain tier overwrites. They are, however, only included once, so
  // relative configs may not specify other relative configs which must to be
  // loaded. If RelativeConfigs is modified while loading configs an error is
  // raised, but we defer doing so until the logger is initialized below. If a
  // relative config cannot be found it is silently skipped (this is to allow
  // configs to be conditionally applied to scripts based on their location). By
  // reading the "hhvm.relative_configs" ini setting at runtime it is possible
  // to determine which configs were actually loaded.
  std::string relConfigsError;
  Config::Bind(s_RelativeConfigs, ini, config, "RelativeConfigs");
  if (!cmd.empty() && !s_RelativeConfigs.empty()) {
    OptString strcmd(cmd, CopyString);
    Process::InitProcessStatics();
    auto const currentDir = Process::CurrentWorkingDirectory.data();
    std::vector<std::string> newConfigs;
    auto const original = s_RelativeConfigs;
    for (auto& str : original) {
      if (str.empty()) continue;

      std::string fullpath;
      auto const found = FileUtil::runRelative(
        str, strcmd, currentDir,
        [&] (const OptString& f) {
          if (access(f.data(), R_OK) == 0) {
            fullpath = f.toCppString();
            FTRACE_MOD(Trace::facts, 3, "Parsing {}\n", fullpath);
            Config::ParseConfigFile(fullpath, ini, config);
            return true;
          }
          return false;
        }
      );
      if (found) newConfigs.emplace_back(std::move(fullpath));
    }
    if (!newConfigs.empty()) {
      auto m2 = getTierOverwrites(ini, config, ActiveExperiments,
                                  InactiveExperiments);
      if (messages) *messages = std::move(m2);
      if (s_RelativeConfigs != original) {
        relConfigsError = fmt::format(
          "RelativeConfigs node was modified while loading configs from [{}] "
          "to [{}]",
          folly::join(", ", original),
          folly::join(", ", s_RelativeConfigs)
        );
      }
    }
    s_RelativeConfigs.swap(newConfigs);
  } else {
    s_RelativeConfigs.clear();
  }

  // Then get the ini and hdf cli strings again, in case the tier overwrites
  // overrode any non-tier based command line option we set. The tier-based
  // command line overwrites will already have been set in the call above.
  // This extra call is for the other command line options that may have been
  // overridden by a tier, but shouldn't have been.
  for (auto& istr : iniClis) {
    Config::ParseIniString(istr, ini);
  }
  for (auto& hstr : hdfClis) {
    Config::ParseHdfString(hstr, config);
  }

  Config::Bind(PidFile, ini, config, "PidFile", "www.pid");
  Config::Bind(DeploymentId, ini, config, "DeploymentId");

  {
    static std::string deploymentIdOverride;
    Config::Bind(deploymentIdOverride, ini, config, "DeploymentIdOverride");
    if (!deploymentIdOverride.empty()) {
      RuntimeOption::DeploymentId = deploymentIdOverride;
    }
  }

  {
    // Config ID
    Config::Bind(ConfigId, ini, config, "ConfigId", 0);
    auto configIdCounter = ServiceData::createCounter("vm.config.id");
    configIdCounter->setValue(ConfigId);
  }

  Cfg::Load(ini, config);

  {
    // Logging
    auto setLogLevel = [](const std::string& value) {
      // ini parsing treats "None" as ""
      if (value == "None" || value == "") {
        Logger::LogLevel = Logger::LogNone;
      } else if (value == "Error") {
        Logger::LogLevel = Logger::LogError;
      } else if (value == "Warning") {
        Logger::LogLevel = Logger::LogWarning;
      } else if (value == "Info") {
        Logger::LogLevel = Logger::LogInfo;
      } else if (value == "Verbose") {
        Logger::LogLevel = Logger::LogVerbose;
      } else {
        return false;
      }
      return true;
    };
    auto str = Config::GetString(ini, config, "Log.Level");
    if (!str.empty()) {
      setLogLevel(str);
    }
    IniSetting::Bind(IniSetting::CORE, IniSetting::Mode::Config,
                     "hhvm.log.level", IniSetting::SetAndGet<std::string>(
      setLogLevel,
      []() {
        switch (Logger::LogLevel) {
          case Logger::LogNone:
            return "None";
          case Logger::LogError:
            return "Error";
          case Logger::LogWarning:
            return "Warning";
          case Logger::LogInfo:
            return "Info";
          case Logger::LogVerbose:
            return "Verbose";
        }
        return "";
      }
    ));

    Config::Bind(Logger::UseLogFile, ini, config, "Log.UseLogFile", true);
    if (Logger::UseLogFile && Cfg::Server::Mode) {
      RuntimeOption::ErrorLogs[Logger::DEFAULT] =
        ErrorLogFileData(Cfg::Log::BaseDirectory, Cfg::Log::File, Cfg::Log::FileSymLink, Cfg::Log::FilePeriodMultiplier);
    }
    if (Config::GetBool(ini, config, "Log.AlwaysPrintStackTraces")) {
      Logger::SetTheLogger(Logger::DEFAULT, std::make_unique<ExtendedLogger>());
      ExtendedLogger::EnabledByDefault = true;
    }

    Config::Bind(Logger::LogHeader, ini, config, "Log.Header");
    Config::Bind(Logger::LogNativeStackTrace, ini, config,
                 "Log.NativeStackTrace", true);
    Config::Bind(Logger::UseSyslog, ini, config, "Log.UseSyslog", false);
    Config::Bind(Logger::UseRequestLog, ini, config, "Log.UseRequestLog",
                 false);
    Config::Bind(Logger::AlwaysEscapeLog, ini, config, "Log.AlwaysEscapeLog",
                 true);
    Config::Bind(Logger::UseCronolog, ini, config, "Log.UseCronolog", false);
    Config::Bind(Logger::MaxMessagesPerRequest, ini,
                 config, "Log.MaxMessagesPerRequest", -1);
    Config::Bind(LogFileFlusher::DropCacheChunkSize, ini,
                 config, "Log.DropCacheChunkSize", 1 << 20);
    Config::Bind(RuntimeErrorReportingLevel, ini,
                 config, "Log.RuntimeErrorReportingLevel",
                 static_cast<int>(ErrorMode::HPHP_ALL));

    auto parseLogs = [] (const Hdf &config, const IniSetting::Map& ini,
                         const std::string &name,
                         std::map<std::string, AccessLogFileData> &logs) {
      auto parse_logs_callback = [&] (const IniSetting::Map &ini_pl,
                                      const Hdf &hdf_pl,
                                      const std::string &ini_pl_key) {
        string logName = hdf_pl.exists() && !hdf_pl.isEmpty()
                       ? hdf_pl.getName()
                       : ini_pl_key;
        string fname = Config::GetString(ini_pl, hdf_pl, "File", "", false);
        if (!fname.empty()) {
          string symlink = Config::GetString(ini_pl, hdf_pl, "SymLink", "",
                                             false);
          string format = Config::GetString(ini_pl, hdf_pl, "Format",
                                            AccessLogDefaultFormat, false);
          auto periodMultiplier = Config::GetUInt16(ini_pl, hdf_pl,
                                                    "PeriodMultiplier",
                                                    0, false);
          logs[logName] = AccessLogFileData(Cfg::Log::BaseDirectory, fname, symlink,
                                            format, periodMultiplier);


        }
      };
      Config::Iterate(parse_logs_callback, ini, config, name);
    };

    parseLogs(config, ini, "Log.Access", AccessLogs);
    RPCLogs = AccessLogs;
    parseLogs(config, ini, "Log.RPC", RPCLogs);
  }

  // If we generated errors while loading RelativeConfigs report those now that
  // error reporting is initialized
  if (!relConfigsError.empty()) Logger::Error(relConfigsError);

  {
    if (Config::GetInt64(ini, config, "ResourceLimit.CoreFileSizeOverride")) {
      setResourceLimit(RLIMIT_CORE, ini,  config,
                       "ResourceLimit.CoreFileSizeOverride");
    } else {
      setResourceLimit(RLIMIT_CORE, ini, config, "ResourceLimit.CoreFileSize");
    }
    setResourceLimit(RLIMIT_NOFILE, ini, config, "ResourceLimit.MaxSocket");
    setResourceLimit(RLIMIT_DATA, ini, config, "ResourceLimit.RSS");
    // These don't have RuntimeOption::xxx bindings, but we still want to be
    // able to use ini_xxx functionality on them; so directly bind to a local
    // static via Config::Bind.
    static int64_t s_core_file_size_override, s_core_file_size, s_rss = 0;
    static int32_t s_max_socket = 0;
    Config::Bind(s_core_file_size_override, ini, config,
                 "ResourceLimit.CoreFileSizeOverride", 0);
    Config::Bind(s_core_file_size, ini, config, "ResourceLimit.CoreFileSize",
                 0);
    Config::Bind(s_max_socket, ini, config, "ResourceLimit.MaxSocket", 0);
    Config::Bind(s_rss, ini, config, "ResourceLimit.RSS", 0);

    Config::Bind(SocketDefaultTimeout, ini, config,
                 "ResourceLimit.SocketDefaultTimeout", 60);
    Config::Bind(MaxSQLRowCount, ini, config, "ResourceLimit.MaxSQLRowCount",
                 0);
    Config::Bind(SerializationSizeLimit, ini, config,
                 "ResourceLimit.SerializationSizeLimit", StringData::MaxSize);
  }
  {
    // Repo
    if (Cfg::Repo::Path.empty()) {
      always_assert_flog(
        !Cfg::Repo::Authoritative,
        "Either Repo.Path, Repo.LocalPath, or Repo.CentralPath "
        "must be set in RepoAuthoritative mode"
      );
    }
  }

  {
    // Eval
    static std::string jitSerdesMode;
    Config::Bind(jitSerdesMode, ini, config, "Eval.JitSerdesMode", "Off");

    EvalJitSerdesMode = [&] {
      #define X(x) if (jitSerdesMode == #x) return JitSerdesMode::x
      X(Serialize);
      X(SerializeAndExit);
      X(Deserialize);
      X(DeserializeOrFail);
      X(DeserializeOrGenerate);
      X(DeserializeAndDelete);
      X(DeserializeAndExit);
      #undef X
      return JitSerdesMode::Off;
    }();

    // DumpPreciseProfileData defaults to true only when we can possibly write
    // profile data to disk.  It can be set to false to avoid the performance
    // penalty of only running the interpreter during retranslateAll.  We will
    // assume that DumpPreciseProfileData implies (JitSerdesMode::Serialize ||
    // JitSerdesMode::SerializeAndExit), to avoid checking too many flags in a
    // few places.  The config file should never need to explicitly set
    // DumpPreciseProfileData to true.
    auto const couldDump = !Cfg::Jit::SerdesFile.empty() &&
      (isJitSerializing() ||
       (EvalJitSerdesMode == JitSerdesMode::DeserializeOrGenerate));
    Config::Bind(DumpPreciseProfData, ini, config,
                 "Eval.DumpPreciseProfData", couldDump);

    Config::Bind(EvalSBSerdesFile, ini, config,
                 "Eval.SBSerdesFile", EvalSBSerdesFile);

#define F(type, name, defaultVal) \
    Config::Bind(Eval ## name, ini, config, "Eval."#name, defaultVal);
    EVALFLAGS()
#undef F

    if (Cfg::Jit::SerdesModeForceOff) EvalJitSerdesMode = JitSerdesMode::Off;

    if (!Cfg::Server::ForkingEnabled && Cfg::Server::Mode) {
      // Only use hugetlb pages when we don't fork().
      low_2m_pages(Cfg::Eval::MaxLowMemHugePages);
      high_2m_pages(Cfg::Eval::MaxHighArenaHugePages);
    }
#if USE_JEMALLOC
    g_useTHPUponHugeTLBFailure =
      Config::GetBool(ini, config, "Eval.UseTHPUponHugeTLBFailure",
                      g_useTHPUponHugeTLBFailure);
#endif

    if (!jit::mcgen::retranslateAllEnabled()) {
      Cfg::Jit::WorkerThreads = 0;
      if (EvalJitSerdesMode != JitSerdesMode::Off) {
        if (Cfg::Server::Mode) {
          Logger::Warning("Eval.JitSerdesMode reset from " + jitSerdesMode +
                          " to off, becasue JitRetranslateAll isn't enabled.");
        }
        EvalJitSerdesMode = JitSerdesMode::Off;
      }
      Cfg::Jit::SerdesFile.clear();
      DumpPreciseProfData = false;
    }
    if (Cfg::GC::SanitizeReqHeap) {
      HeapObjectSanitizer::install_signal_handler();
    }

    HardwareCounter::Init(Cfg::Eval::ProfileHWEnable,
                          url_decode(Cfg::Eval::ProfileHWEvents.data(),
                                     Cfg::Eval::ProfileHWEvents.size()).toCppString(),
                          false,
                          Cfg::Eval::ProfileHWExcludeKernel,
                          Cfg::Eval::ProfileHWFastReads,
                          Cfg::Eval::ProfileHWExportInterval);

    if (Cfg::Jit::Enabled && Cfg::Eval::RecordCodeCoverage) {
      throw std::runtime_error("Code coverage is not supported with "
        "Eval.Jit=true");
    }
    SetArenaSlabAllocBypass(Cfg::Eval::DisableSmallAllocator);
  }
  {
    // Server
    if (GetServerPrimaryIPv4().empty() && GetServerPrimaryIPv6().empty()) {
      throw std::runtime_error("Unable to resolve the server's IPv4 or IPv6 address");
    }

    // These things should be in Cfg PostProcess methods. But because they use
    // normalizeDir they can't
    {
      Cfg::Server::SourceRoot = FileUtil::normalizeDir(Cfg::Server::SourceRoot);
      if (Cfg::Server::SourceRoot.empty()) {
        Cfg::Server::SourceRoot = Cfg::ServerLoader::SourceRootDefault();
      }

      for (unsigned int i = 0; i < Cfg::Server::IncludeSearchPaths.size(); i++) {
        Cfg::Server::IncludeSearchPaths[i] = FileUtil::normalizeDir(Cfg::Server::IncludeSearchPaths[i]);
      }
      Cfg::Server::IncludeSearchPaths.insert(Cfg::Server::IncludeSearchPaths.begin(), ".");

      Cfg::Server::FontPath = FileUtil::normalizeDir(Cfg::Server::FontPath);
    }
  }

  VirtualHost::SortAllowedDirectories(Cfg::Server::AllowedDirectories);
  {
    auto vh_callback = [] (const IniSettingMap &ini_vh, const Hdf &hdf_vh,
                           const std::string &ini_vh_key) {
      if (VirtualHost::IsDefault(ini_vh, hdf_vh, ini_vh_key)) {
        VirtualHost::GetDefault().init(ini_vh, hdf_vh, ini_vh_key);
        VirtualHost::GetDefault().addAllowedDirectories(Cfg::Server::AllowedDirectories);
      } else {
        auto host = std::make_shared<VirtualHost>(ini_vh, hdf_vh, ini_vh_key);
        host->addAllowedDirectories(Cfg::Server::AllowedDirectories);
        VirtualHosts.push_back(host);
      }
    };
    // Virtual Hosts have to be iterated in order. Because only the first
    // one that matches in the VirtualHosts vector gets applied and used.
    // Hdf's and ini (via Variant arrays) internal storage handles ordering
    // naturally (as specified top to bottom in the file and left to right on
    // the command line.
    Config::Iterate(vh_callback, ini, config, "VirtualHost");
    Cfg::Server::LowestMaxPostSize = VirtualHost::GetLowestMaxPostSize();
  }
  {
    // IpBlocks
    IpBlocks = std::make_shared<IpBlockMap>(ini, config);
  }

  {
    // Static File

    hphp_string_imap<std::string> staticFileDefault;
    staticFileDefault["css"] = "text/css";
    staticFileDefault["gif"] = "image/gif";
    staticFileDefault["html"] = "text/html";
    staticFileDefault["jpeg"] = "image/jpeg";
    staticFileDefault["jpg"] = "image/jpeg";
    staticFileDefault["mp3"] = "audio/mpeg";
    staticFileDefault["png"] = "image/png";
    staticFileDefault["tif"] = "image/tiff";
    staticFileDefault["tiff"] = "image/tiff";
    staticFileDefault["txt"] = "text/plain";
    staticFileDefault["zip"] = "application/zip";

    Config::Bind(StaticFileExtensions, ini, config, "StaticFile.Extensions",
                 staticFileDefault);

    auto matches_callback = [](const IniSettingMap& ini_m, const Hdf& hdf_m,
                               const std::string& /*ini_m_key*/) {
      FilesMatches.push_back(std::make_shared<FilesMatch>(ini_m, hdf_m));
    };
    Config::Iterate(matches_callback, ini, config, "StaticFile.FilesMatch");
  }
  {
    // PhpFile
    Config::Bind(PhpFileExtensions, ini, config, "PhpFile.Extensions");
  }
  {
    // Admin Server
    // Until we support these get things we leave it here
    // And the reason these are doing get is that we don't want these values to be able
    // to be fetched using ini_get
    Cfg::AdminServer::Password = Config::GetString(ini, config, "AdminServer.Password");
    Cfg::AdminServer::Passwords = Config::GetSet(ini, config, "AdminServer.Passwords");
    Cfg::AdminServer::HashedPasswords = Config::GetSet(ini, config, "AdminServer.HashedPasswords");
  }
  {
    // Debug
    StackTrace::Enabled = Cfg::Debug::NativeStackTrace;

    if (Cfg::Debug::CoreDumpReport) {
      install_crash_reporter();
    }

    Config::Bind(TraceFunctions, ini, config,
                 "Debug.TraceFunctions", TraceFunctions);
  }
  {
    Config::Bind(ServerVariables, ini, config, "ServerVariables");
    Config::Bind(EnvVariables, ini, config, "EnvVariables");
  }
#ifdef HHVM_FACEBOOK
  {
    // ThriftFBServer
    Config::Bind(ThriftFBServerThriftServerIOWorkerThreads, ini, config,
                 "ThriftFBServer.ThriftServerIOThreads", 1);
    Config::Bind(ThriftFBServerThriftServerCPUWorkerThreads, ini, config, "ThriftFBServer.ThriftServerCPUWorkerThreads",
                 1);
    Config::Bind(ThriftFBServerHighPriorityEndPoints, ini, config, "ThriftFBServer.HighPriorityEndPoints");

    // Fb303Server
    Config::Bind(EnableFb303Server, ini, config, "Fb303Server.Enable",
                 EnableFb303Server);
    Config::Bind(Fb303ServerPort, ini, config, "Fb303Server.Port", 0);
    Config::Bind(Fb303ServerIP, ini, config, "Fb303Server.IP");
    Config::Bind(Fb303ServerWorkerThreads, ini, config,
                 "Fb303Server.WorkerThreads", 1);
    Config::Bind(Fb303ServerPoolThreads, ini, config, "Fb303Server.PoolThreads",
                 1);
    Config::Bind(Fb303ServerExposeSensitiveMethods, ini, config,
                 "Fb303Server.ExposeSensitiveMethods", Fb303ServerExposeSensitiveMethods);

  }
#endif

  Config::Bind(TzdataSearchPaths, ini, config, "TzdataSearchPaths");

  Config::Bind(CustomSettings, ini, config, "CustomSettings");

  Config::Bind(GFlags, ini, config, "GFlags");
  for (auto const& gflag : GFlags) {
    auto result = folly::gflags::SetCommandLineOption(gflag.first.c_str(),
                                                      gflag.second.c_str());
    if (result.empty()) {
      // result is empty under error conditions, or if the flag does not exist
      Logger::Error("Unrecognised GFlags '%s'", gflag.first.c_str());
    } else {
      Logger::Verbose("Set GFlags '%s' = '%s'", gflag.first.c_str(),
                      gflag.second.c_str());
    }
  }


  // Run initializers dependent on options, e.g., resizing atomic maps/vectors.
  refineStaticStringTableSize();
  InitFiniNode::ProcessPostRuntimeOptions();

  // **************************************************************************
  //                                  DANGER
  //
  // Only bind Mode::Config here!
  // These settings are process-wide, while those need to be thread-local since
  // they are per-request. They should go into RequestInjectionData. Getting
  // this wrong will cause subtle breakage -- in particular, it probably will
  // not show up in CLI mode, since everything there tends to be single
  // theaded.
  // **************************************************************************
  // Language and Misc Configuration Options
  IniSetting::Bind(IniSetting::CORE, IniSetting::Mode::Config, "expose_php",
                   &Cfg::Server::ExposeHPHP);

  // Data Handling
  IniSetting::Bind(IniSetting::CORE, IniSetting::Mode::Config,
                   "post_max_size",
                   IniSetting::SetAndGet<int64_t>(
                     nullptr,
                     []() {
                       return VirtualHost::GetMaxPostSize();
                     },
                     &Cfg::Server::MaxPostSize
                   ));
  IniSetting::Bind(IniSetting::CORE, IniSetting::Mode::Config,
                   "always_populate_raw_post_data",
                   &Cfg::Server::AlwaysPopulateRawPostData);

  // Paths and Directories
  IniSetting::Bind(IniSetting::CORE, IniSetting::Mode::Config,
                   "doc_root", &Cfg::Server::SourceRoot);
  IniSetting::Bind(IniSetting::CORE, IniSetting::Mode::Config,
                   "sendmail_path", &Cfg::Mail::SendmailPath);

  // FastCGI
  IniSetting::Bind(IniSetting::CORE, IniSetting::Mode::Config,
                   "pid", &RuntimeOption::PidFile);

  // File Uploads
  IniSetting::Bind(IniSetting::CORE, IniSetting::Mode::Config,
                   "file_uploads", "true",
                   &Cfg::Server::UploadEnableFileUploads);
  IniSetting::Bind(IniSetting::CORE, IniSetting::Mode::Config,
                   "upload_tmp_dir", &Cfg::Server::UploadTmpDir);
  IniSetting::Bind(IniSetting::CORE, IniSetting::Mode::Config,
                   "upload_max_filesize",
                   IniSetting::SetAndGet<std::string>(
                     [](const std::string& value) {
                       return ini_on_update(
                         value, Cfg::Server::UploadMaxFileSize);
                     },
                     []() {
                       return convert_long_to_bytes(
                         VirtualHost::GetUploadMaxFileSize());
                     }
                   ));
  // Filesystem and Streams Configuration Options
  IniSetting::Bind(IniSetting::CORE, IniSetting::Mode::Config,
                   "allow_url_fopen",
                   IniSetting::SetAndGet<std::string>(
                     [](const std::string& /*value*/) { return false; },
                     []() { return "1"; }));

  // HPHP specific
  IniSetting::Bind(IniSetting::CORE, IniSetting::Mode::Constant,
                   "hphp.compiler_id",
                   IniSetting::SetAndGet<std::string>(
                     [](const std::string& /*value*/) { return false; },
                     []() { return compilerId().begin(); }));
  IniSetting::Bind(IniSetting::CORE, IniSetting::Mode::Constant,
                   "hphp.compiler_timestamp",
                   IniSetting::SetAndGet<int64_t>(
                     [](const int64_t& /*value*/) { return false; },
                     []() { return compilerTimestamp(); }));
  IniSetting::Bind(IniSetting::CORE, IniSetting::Mode::Constant,
                   "hphp.compiler_version",
                   IniSetting::SetAndGet<std::string>(
                     [](const std::string& /*value*/) { return false; },
                     []() { return getHphpCompilerVersion(); }));
  IniSetting::Bind(IniSetting::CORE, IniSetting::Mode::Constant,
                   "hphp.cli_server_api_version",
                   IniSetting::SetAndGet<uint64_t>(
                     [](const uint64_t /*value*/) { return false; },
                     []() { return cli_server_api_version(); }));
  IniSetting::Bind(
    IniSetting::CORE, IniSetting::Mode::Constant, "hphp.build_id",
    IniSetting::SetAndGet<std::string>(
      [](const std::string& /*value*/) { return false; },
      nullptr,
      &RuntimeOption::BuildId)
  );
  IniSetting::Bind(IniSetting::CORE, IniSetting::Mode::Config,
                   "notice_frequency",
                   &Cfg::ErrorHandling::NoticeFrequency);
  IniSetting::Bind(IniSetting::CORE, IniSetting::Mode::Config,
                   "warning_frequency",
                   &Cfg::ErrorHandling::WarningFrequency);
  IniSetting::Bind(IniSetting::CORE, IniSetting::Mode::Constant,
                   "hhvm.build_type",
                   IniSetting::SetAndGet<std::string>(
    [](const std::string&) {
      return false;
    },
    []() {
      return s_hhvm_build_type.c_str();
    }
  ));

  ExtensionRegistry::moduleLoad(ini, config);
  initialize_apc();


  if (TraceFunctions.size()) Trace::ensureInit(getTraceOutputFile());

  if (Cfg::Jit::EnableRenameFunction && Cfg::Repo::Authoritative) {
      throw std::runtime_error("Can't use Eval.JitEnableRenameFunction if "
                               " Repo.Authoritative is turned on");
  }

  // Bespoke array-likes
  auto const allow_bespoke_array_likes = allowBespokeArrayLikes();
  if (!allow_bespoke_array_likes) {
    // We can't do this in PostProcess because allowBespokeArrayLikes uses configs from both
    // Eval and Jit sections
    Cfg::Eval::EmitBespokeTypeStructures = false;
    Cfg::Eval::EmitBespokeMonotypes = false;
    Cfg::Eval::EmitBespokeStructDicts = false;
  }
  bespoke::setLoggingEnabled(allow_bespoke_array_likes);
  specializeVanillaDestructors();

  // Coeffects
  CoeffectsConfig::init(RO::EvalCoeffectEnforcementLevels);

  // Initialize defaults for repo-specific parser configuration options.
  RepoOptions::setDefaults(config, ini);

}

///////////////////////////////////////////////////////////////////////////////

namespace {

ServiceData::CounterCallback s_build_info([](ServiceData::CounterMap& counters) {
  if (RuntimeOption::BuildId.empty()) return;

  std::vector<std::string> pieces;
  pieces.reserve(4);
  // ignore-job-rev-timestamp
  folly::split('-', RuntimeOption::BuildId, pieces, true);

  counters.emplace("admin.build.rev", folly::to<int64_t>(pieces.at(2)));

  std::tm t{};
  t.tm_isdst = -1;
  std::istringstream ss{std::move(pieces.at(3))};
  ss >> std::get_time(&t, "%Y%m%d%H%M%S");
  if (!ss.fail()) {
    counters.emplace("admin.build.age", std::time(nullptr) - mktime(&t));
  }
}, "admin.build.");

}

///////////////////////////////////////////////////////////////////////////////

}
