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

#pragma once

#include <unordered_map>
#include <algorithm>
#include <vector>
#include <string>
#include <map>
#include <set>
#include <boost/container/flat_set.hpp>
#include <memory>
#include <sys/stat.h>
#include <folly/json/dynamic.h>

#include "hphp/runtime/base/config.h"
#include "hphp/runtime/base/configs/repo-options-flags-generated.h"
#include "hphp/runtime/base/package.h"
#include "hphp/runtime/base/typed-value.h"
#include "hphp/runtime/base/types.h"

#include "hphp/util/compilation-flags.h"
#include "hphp/util/configs/autoload.h"
#include "hphp/util/configs/eval.h"
#include "hphp/util/configs/gc.h"
#include "hphp/util/configs/hacklang.h"
#include "hphp/util/configs/jit.h"
#include "hphp/util/configs/php7.h"
#include "hphp/util/configs/server.h"
#include "hphp/util/functional.h"
#include "hphp/util/hash-map.h"
#include "hphp/util/sha1.h"

#include "hphp/hack/src/parser/ffi_bridge/parser_ffi.rs.h"

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

struct AccessLogFileData;
struct ErrorLogFileData;
struct VirtualHost;
struct IpBlockMap;
struct SatelliteServerInfo;
struct FilesMatch;
struct Hdf;
struct IniSettingMap;

using StringToIntMap = std::unordered_map<std::string, int>;

enum class JitSerdesMode {
  // NB: if changing the encoding here, make sure to update isJitSerializing()
  // and isJitDeserializing() as needed.
  //
  // Bit 0: serialize
  // Bit 1: deserialize
  Off                   = 0x0,
  Serialize             = 0x1,          // 00001
  SerializeAndExit      = 0x5,          // 00101
  Deserialize           = 0x2,          // 00010
  DeserializeOrFail     = 0x6,          // 00110
  DeserializeOrGenerate = 0xa,          // 01010
  DeserializeAndDelete  = 0xe,          // 01110
  DeserializeAndExit    = 0x12,         // 10010
  DeserializeForPreload = 0x16,         // 10110
};

namespace hackc {
  struct NativeEnv;
  struct HhbcFlags;
  struct ParserFlags;
  struct DeclParserConfig;
}

namespace Facts {
// Version number representing the DB's schema.  This number is determined
// randomly.  We use this when we make a change that invalidates the cache,
// such as adding a new table which would otherwise be unpopulated without a
// cache rebuild.
constexpr size_t kSchemaVersion = 2035084036;
}

/*
 * The bare RepoOptions information that the parser cares about.
 */
struct RepoOptionsFlags {

#define S(name) friend Cfg::name;
SECTIONS_FOR_REPOOPTIONSFLAGS()
#undef S

  const PackageInfo& packageInfo() const { return m_packageInfo; }
  const SHA1& cacheKeySha1() const { return m_sha1; }

  ParserEnv getParserEnvironment() const;
  void initDeclConfig(hackc::DeclParserConfig&) const;
  void initHhbcFlags(hackc::HhbcFlags&) const;
  void initParserFlags(hackc::ParserFlags&) const;
  void initAliasedNamespaces(hackc::NativeEnv&) const;
  void initExperimentalFeatures(hackc::NativeEnv&) const;

  std::string autoloadQuery() const { return Query; }
  std::vector<std::string> autoloadIncludePaths() const { return IncludePaths; }
  std::vector<std::string> autoloadExcludePaths() const { return ExcludePaths; }
  std::vector<std::string> autoloadFileExtensions() const { return FileExtensions; }
  bool autoloadUseEden() const { return UseEden; }
  folly::dynamic autoloadQueryObj() const { return m_cachedQuery; }
  std::string trustedDBPath() const { return TrustedDBPath; }
  const std::vector<std::string>& autoloadRepoBuildSearchDirs() const {
    return RepoBuildSearchDirs;
  }

  /**
   * Allowlist consisting of the attributes, marking methods, which Facts
   * should index
   */
  const Cfg::StringVector& indexedMethodAttributes() const {
    return IndexedMethodAttributes;
  }

  /**
   * Allowlist consisting of the attributes, marking functions, which Facts
   * should index
   */
  const Cfg::StringVector& indexedFunctionAttributes() const {
    return IndexedFunctionAttributes;
  }

  // NB: Everything serialized here affects the cache for RE. Do not
  // put anything unnecessary or that changes spuriously.
  template <typename SerDe> void serde(SerDe& sd) {
    #define C(t, n) sd(n);
    CONFIGS_FOR_REPOOPTIONSFLAGS()
    #undef C

    sd(m_packageInfo);
    sd(m_sha1);
    sd(m_factsCacheBreaker);

    if constexpr (SerDe::deserializing) calcCachedQuery();
  }

  template <typename SerDe>
  static RepoOptionsFlags makeForSerde(SerDe& sd) {
    RepoOptionsFlags f;
    sd(f);
    return f;
  }

  const std::string& getFactsCacheBreaker() const { return m_factsCacheBreaker;}
  void calcCachedQuery();

private:
  RepoOptionsFlags() = default;

  #define C(t, n) t n;
  CONFIGS_FOR_REPOOPTIONSFLAGS()
  #undef C

  PackageInfo m_packageInfo;

  SHA1 m_sha1;
  std::string m_factsCacheBreaker;

  // The query to be used for autoloading
  folly::dynamic m_cachedQuery;

  friend struct RepoOptions;
};

namespace Stream {
struct Wrapper;
}

struct RepoOptionStats {
  RepoOptionStats() = default;
  RepoOptionStats(const std::string&, Stream::Wrapper*);
  bool missing() const {
    return !m_configStat.has_value() && !m_packageStat.has_value();
  }

  Optional<struct stat> m_configStat;
  Optional<struct stat> m_packageStat;
};

/*
 * RepoOptionsFlags plus extra state
 */
struct RepoOptions {
  RepoOptions(const RepoOptions&) = default;
  RepoOptions(RepoOptions&&) = default;

  const RepoOptionsFlags& flags() const { return m_flags; }
  const PackageInfo& packageInfo() const { return flags().packageInfo(); }
  const std::filesystem::path& path() const { return m_path; }
  const RepoOptionStats& stat() const { return m_stat; }

  const std::filesystem::path& dir() const { return m_repo; }
  const std::filesystem::path& autoloadDB() const { return m_autoloadDB; }

  bool operator==(const RepoOptions& o) const {
    // If we have hash collisions of unequal RepoOptions, we have
    // bigger problems.
    return m_flags.m_sha1 == o.m_flags.m_sha1;
  }
  bool operator!=(const RepoOptions& o) const { return !(*this == o); }

  static const RepoOptions& defaults();
  static const RepoOptions& defaultsForSystemlib();
  static void setDefaults(const Hdf& hdf, const IniSettingMap& ini);

  static const RepoOptions& forFile(const std::string& path);
private:
  RepoOptions() = default;
  RepoOptions(const char* str, const char* file);

  void filterNamespaces();
  void initDefaults(const Hdf& hdf, const IniSettingMap& ini);
  void initDefaultsForSystemlib();
  void calcCacheKey();
  void calcDynamic();
  void calcAutoloadDB();

  RepoOptionsFlags m_flags;

  // Path to .hhvmconfg.hdf
  std::filesystem::path m_path;
  RepoOptionStats m_stat;

  // Canonical path of repo root directory that contains .hhvmconfig.hdf
  std::filesystem::path m_repo;

  // The autoload DB specified by these repo options
  std::filesystem::path m_autoloadDB;

  bool m_init = false;

  static RepoOptions s_defaults;
  static RepoOptions s_defaultsForSystemlib;
};

/**
 * Configurable options set from command line or configurable file at startup
 * time.
 */
struct RuntimeOption {
  static void Load(
    IniSettingMap &ini, Hdf& config,
    const std::vector<std::string>& iniClis = std::vector<std::string>(),
    const std::vector<std::string>& hdfClis = std::vector<std::string>(),
    std::vector<std::string>* messages = nullptr,
    std::string cmd = "");

  static bool GcSamplingEnabled() {
    return Cfg::GC::SampleRate > 0;
  }

  static bool JitSamplingEnabled() {
    return Cfg::Jit::Enabled && Cfg::Jit::SampleRate > 0;
  }

  static void ReadSatelliteInfo(
    const IniSettingMap& ini,
    const Hdf& hdf,
    std::vector<std::shared_ptr<SatelliteServerInfo>>& infos
  );

  static Optional<std::filesystem::path> GetHomePath(
    const folly::StringPiece user);

  static std::string GetDefaultUser();

  /**
   * Find a config file corresponding to the given user and parse its
   * settings into the given ini and hdf objects.
   *
   * Return true on success and false on failure.
   */
  static bool ReadPerUserSettings(const std::filesystem::path& confFileName,
                                  IniSettingMap& ini, Hdf& config);

  static std::string getTraceOutputFile();

  // Store the list of input values used to calculate the tier overwrites
  static std::map<std::string, std::string> TierOverwriteInputs;
  static void StoreTierOverwriteInputs(const std::string &machine, const std::string &tier,
    const std::string &task, const std::string &cpu, const std::string &tiers, const std::string &tags);
  static std::map<std::string, std::string> getTierOverwriteInputs();

  // Store the Metadata about the config file read in.
  static std::map<std::string, std::string>& GetMetadata();

  static std::string BuildId;
  static std::string InstanceId;
  static std::string DeploymentId; // ID for set of instances deployed at once
  static int64_t ConfigId; // Queryable to verify a specific config was read
  static std::string PidFile;

  static std::map<std::string, ErrorLogFileData> ErrorLogs;

  static int RuntimeErrorReportingLevel;

  static int RaiseDebuggingFrequency;
  static int64_t SerializationSizeLimit;

  static std::string AccessLogDefaultFormat;
  static std::map<std::string, AccessLogFileData> AccessLogs;

  static std::map<std::string, AccessLogFileData> RPCLogs;

  static const std::string& GetServerPrimaryIPv4();
  static const std::string& GetServerPrimaryIPv6();
  static std::vector<std::shared_ptr<VirtualHost>> VirtualHosts;
  static std::shared_ptr<IpBlockMap> IpBlocks;
  static std::vector<std::shared_ptr<SatelliteServerInfo>>
         SatelliteServerInfos;

  /**
   * Legal root directory expressions in an include expression. For example,
   *
   *   include_once $PHP_ROOT . '/lib.php';
   *
   * Here, "$PHP_ROOT" is a legal include root. Stores what it resolves to.
   *
   *   RuntimeOption::IncludeRoots["$PHP_ROOT"] = "";
   *   RuntimeOption::IncludeRoots["$LIB_ROOT"] = "lib";
   */
  static std::map<std::string, std::string> IncludeRoots;

  static hphp_string_imap<std::string> StaticFileExtensions;
  static hphp_string_imap<std::string> PhpFileExtensions;
  static std::set<std::string> StaticFileGenerators;
  static std::vector<std::shared_ptr<FilesMatch>> FilesMatches;

  static std::set<std::string, stdltistr> TraceFunctions;

  static int64_t MaxSQLRowCount;
  static int64_t SocketDefaultTimeout;

  static std::map<std::string, std::string> ServerVariables;

  static std::map<std::string, std::string> EnvVariables;

  // Eval options
  static bool EnableZendIniCompat;
  static JitSerdesMode EvalJitSerdesMode;
  static bool DumpPreciseProfData;

  static std::string EvalSBSerdesFile;

  static hphp_string_map<TypedValue> ConstantFunctions;

  static std::vector<std::string> TzdataSearchPaths;
  static std::map<std::string, std::string> GFlags;

  static hphp_fast_string_set ActiveExperiments;
  static hphp_fast_string_set InactiveExperiments;

#define EVALFLAGS()                                                     \
  /* F(type, name, defaultVal) */                                       \
  F(string, ReorderProps,              reorderPropsDefault())           \
  /*                                                                    \
   * Map from coeffect name to enforcement level                        \
   * e.g. {'pure' => 2, 'rx' => 1}                                      \
   */                                                                   \
  F(StringToIntMap, CoeffectEnforcementLevels, coeffectEnforcementLevelsDefaults()) \
  /* */

private:
  using string = std::string;

  // Custom settings. This should be accessed via the GetServerCustomSetting
  // APIs.
  static std::map<std::string, std::string> CustomSettings;

public:
#define F(type, name, unused) \
  static type Eval ## name;
  EVALFLAGS()
#undef F

#ifdef HHVM_FACEBOOK
  // ThriftFBServer
  static int ThriftFBServerThriftServerIOWorkerThreads;
  static int ThriftFBServerThriftServerCPUWorkerThreads;
  static std::set<std::string> ThriftFBServerHighPriorityEndPoints;

  // fb303 server
  static bool EnableFb303Server;
  static int Fb303ServerPort;
  static std::string Fb303ServerIP;
  static int Fb303ServerWorkerThreads;
  static int Fb303ServerPoolThreads;
  static bool Fb303ServerExposeSensitiveMethods;
#endif

  static bool funcIsRenamable(const StringData* name);
};
static_assert(sizeof(RuntimeOption) == 1, "no instance variables");

using RO = RuntimeOption;

inline bool isJitDeserializing() {
  auto const m = RuntimeOption::EvalJitSerdesMode;
  return static_cast<std::underlying_type<JitSerdesMode>::type>(m) & 0x2;
}

inline bool isJitSerializing() {
  auto const m = RuntimeOption::EvalJitSerdesMode;
  return static_cast<std::underlying_type<JitSerdesMode>::type>(m) & 0x1;
}

inline bool unitPrefetchingEnabled() {
  return Cfg::Eval::UnitPrefetcherMaxThreads > 0;
}

inline bool speculativeUnitPrefetchingEnabled() {
  return Cfg::Eval::UnitPrefetcherSpeculative && Cfg::Eval::UnitPrefetcherMaxThreads > 0;
}


/*
 * Ceiling on the worker count that ThreadTune may scale up to from a given
 * baseline, per Server.ThreadTune.AdjustmentPct.
 *
 * Two callers must agree on this: ThreadController, which never recommends
 * more than this, and HttpServer, which sizes the dispatcher's queue ceiling
 * from it. If the latter came out lower, setMaxThreadCount() would clamp and
 * tuning would silently top out early.
 */
size_t serverThreadTuneMaxThreadCount(size_t baselineThreadCount);

inline StringToIntMap coeffectEnforcementLevelsDefaults() {
#ifdef HHVM_FACEBOOK
  return {{"zoned", 2}};
#else
  return {};
#endif
}

///////////////////////////////////////////////////////////////////////////////
}
