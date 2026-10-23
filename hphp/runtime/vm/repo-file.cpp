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

#include "hphp/runtime/vm/repo-file.h"

#include "hphp/runtime/base/autoload-handler.h"

#include "hphp/runtime/vm/func-emitter.h"
#include "hphp/runtime/vm/repo-autoload-map-builder.h"
#include "hphp/runtime/vm/repo-global-data.h"
#include "hphp/runtime/vm/unit-emitter.h"

#include "hphp/util/blob-writer.h"

#include <fmt/format.h>
#include <folly/String.h>
#include <folly/container/F14Map.h>
#include <folly/container/F14Set.h>
#include <magic_enum/magic_enum.hpp>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string_view>

TRACE_SET_MOD(repo_file)

namespace HPHP {

////////////////////////////////////////////////////////////////////////////////

namespace {

enum class RepoFileChunks {
  UNIT_EMITTERS,
  GLOBAL_DATA,
  PACKAGE_INFO,
};

enum class RepoFileIndexes {
  UNIT_SYMBOLS,
  UNIT_METH_CALLERS,
  UNIT_INFOS,
  AUTOLOAD_TYPES,
  AUTOLOAD_FUNCS,
  AUTOLOAD_CONSTANTS,
  AUTOLOAD_TYPEALIASES,
  AUTOLOAD_MODULES,
  PATH_TO_UNIT_INFO,
};

////////////////////////////////////////////////////////////////////////////////
// File Format:

/*
 * Fixed Header:
 *
 * - Magic   (4 bytes): "HHBC"
 * - Version (2 bytes)
 *
 * Everything after the fixed header depends on the version number.
 *
 * Version 1 header:
 *
 * - Repo schema (up to 256 bytes)
 * (Everything after this depends on the repo schema)
 */

////////////////////////////////////////////////////////////////////////////////

constexpr Blob::Magic kMagic{ 'H', 'H', 'B', 'C' };

constexpr Blob::Version kCurrentVersion = 1;

// Arbitrary limits on the size of various sections. The sizes are all
// 64-bits, but we don't allow the full range so that a corrupted file
// won't cause us to try to pre-allocate huge amounts of memory. These
// limits were sized so that we should never exceed them, but if we
// ever do, we can just raise them.
constexpr size_t kUnitEmitterSizeLimit = 1ull << 33;
constexpr size_t kGlobalDataSizeLimit  = 1ull << 28;
constexpr size_t kPackageInfoSizeLimit = 1ull << 28;
constexpr size_t kIndexSizeLimit       = 1ull << 28;

// If you hit this limit you also need to change Blob::HashMapIndex::Bucket.
constexpr size_t kIndexDataSizeLimit   = std::numeric_limits<uint32_t>::max();

////////////////////////////////////////////////////////////////////////////////

const StringData* relativePathToSourceRoot(const StringData* path) {
  assertx(!path->empty());
  if (path->data()[0] == '/' && !Cfg::Server::SourceRoot.empty() &&
    !strncmp(Cfg::Server::SourceRoot.c_str(), path->data(), Cfg::Server::SourceRoot.size())) {
    return makeStaticString(path->data() + Cfg::Server::SourceRoot.size(),
                            path->size() - Cfg::Server::SourceRoot.size());
  }
  return path;
}

std::string relativePathToSourceRoot(std::string_view path) {
  if (path.empty()) return {};
  if (path[0] == '/' && !Cfg::Server::SourceRoot.empty() &&
      path.starts_with(Cfg::Server::SourceRoot)) {
    path.remove_prefix(Cfg::Server::SourceRoot.size());
  }
  return std::string{path};
}

bool containsPathOrAncestor(
    const std::vector<std::string>& sortedPaths,
    std::string_view path) {
  for (;;) {
    auto const it = std::lower_bound(
      sortedPaths.begin(),
      sortedPaths.end(),
      path,
      [](const std::string& candidate, std::string_view value) {
        return std::string_view{candidate} < value;
      }
    );
    if (it != sortedPaths.end() && std::string_view{*it} == path) {
      return true;
    }

    auto const slash = path.rfind('/');
    if (slash == std::string_view::npos) return false;
    path = path.substr(0, slash);
  }
}

using UnitMethCallers = std::vector<const StringData*>;

////////////////////////////////////////////////////////////////////////////////

}

////////////////////////////////////////////////////////////////////////////////

void RepoUnitInfo::serde(BlobEncoder& sd) const {
  std::string spath = path->toCppString();
  auto const pathData = spath.c_str();
  assertx(!spath.empty());
  if (pathData[0] == '/' && !Cfg::Server::SourceRoot.empty() &&
      !strncmp(Cfg::Server::SourceRoot.c_str(), pathData, Cfg::Server::SourceRoot.size())) {
    spath = spath.substr(Cfg::Server::SourceRoot.size());
  }

  sd(unitSn)(spath)(emitterLocation)(symbolsLocation);
}

void RepoUnitInfo::serde(BlobDecoder& sd) {
  std::string spath;

  sd(unitSn)(spath)(emitterLocation)(symbolsLocation);

  if (!Cfg::Server::SourceRoot.empty() && !spath.empty() && spath.c_str()[0] != '/') {
    spath = Cfg::Server::SourceRoot + spath;
  }
  path = makeStaticString(spath);
}

////////////////////////////////////////////////////////////////////////////////

// Builder state
struct RepoFileBuilder::Data : Blob::Writer<RepoFileChunks, RepoFileIndexes> {
  struct UnitEmitterIndex {
    const StringData* path;
    int64_t sn;
    Blob::Bounds location;
    UnitMethCallers methCallers;
  };
  std::vector<UnitEmitterIndex> unitEmittersIndex;
  bool enableUnitEmitterReuse{false};

  void finish(const RepoGlobalData&,
              std::vector<RepoUnitSymbols>,
              std::vector<UnitMethCallers>,
              const PackageInfo&,
              const RepoAutoloadMapBuilder::TypeNameMap& types,
              const RepoAutoloadMapBuilder::FuncNameMap& funcs,
              const RepoAutoloadMapBuilder::CaseSensitiveMap& constants,
              const RepoAutoloadMapBuilder::TypeNameMap& typeAliases,
              const RepoAutoloadMapBuilder::CaseSensitiveMap& modules);
};

////////////////////////////////////////////////////////////////////////////////

RepoFileBuilder::RepoFileBuilder(const std::string& path,
                                 bool enableUnitEmitterReuse)
  : m_data{std::make_unique<Data>()}
{
  if (enableUnitEmitterReuse &&
      (Cfg::Eval::UseHHBBC || Cfg::Eval::EnableDecl)) {
    throw std::runtime_error(
      "Current build configuration cannot reuse raw unit emitters"
    );
  }
  m_data->enableUnitEmitterReuse = enableUnitEmitterReuse;
  m_data->header(path, kMagic, kCurrentVersion);
}

RepoFileBuilder::~RepoFileBuilder() {
}

void RepoFileBuilder::add(const EncodedUE& ue) {
  assertx(m_data);
  assertx(ue.path->isStatic());
  assertx(ue.sn >= 0);

  FTRACE(2, "RepoFileBuilder::add {}\n", ue.path);

  const StringData* path = ue.path;
  assertx(!path->empty());
  path = relativePathToSourceRoot(path);

  auto const size = ue.blob.size();
  always_assert(size <= kUnitEmitterSizeLimit);

  m_data->unitEmittersIndex.emplace_back(
    RepoFileBuilder::Data::UnitEmitterIndex{
      path,
      ue.sn,
      { m_data->sizes.get(RepoFileChunks::UNIT_EMITTERS), size },
      ue.methCallers
    }
  );

  m_data->write(RepoFileChunks::UNIT_EMITTERS, ue.blob.data(), size);
}

void RepoFileBuilder::add(const UnitEmitter& ue) {
  assertx(m_data);
  add(EncodedUE{ue, m_data->enableUnitEmitterReuse});
}

void RepoFileBuilder::finish(const RepoGlobalData& global,
                             const RepoAutoloadMapBuilder& autoloadMap,
                             const PackageInfo& packageInfo) {
  assertx(m_data);

  std::vector<RepoUnitSymbols> unitSymbols(
    m_data->unitEmittersIndex.size());
  auto add_symbols = [&](auto const& symbols, auto type) {
    for (auto const& info : symbols) {
      unitSymbols[info.second].push_back(std::make_pair(info.first, type));
    }
  };

  add_symbols(autoloadMap.getTypes(), RepoSymbolType::TYPE);
  add_symbols(autoloadMap.getFuncs(), RepoSymbolType::FUNC);
  add_symbols(autoloadMap.getConstants(), RepoSymbolType::CONSTANT);
  add_symbols(autoloadMap.getTypeAliases(), RepoSymbolType::TYPE_ALIAS);
  add_symbols(autoloadMap.getModules(), RepoSymbolType::MODULE);

  std::vector<UnitMethCallers> unitMethCallers;
  if (m_data->enableUnitEmitterReuse) {
    unitMethCallers.resize(m_data->unitEmittersIndex.size());
    for (auto const& unit : m_data->unitEmittersIndex) {
      unitMethCallers[unit.sn] = unit.methCallers;
    }
  }

  auto data = std::move(m_data);
  data->finish(
    global,
    std::move(unitSymbols),
    std::move(unitMethCallers),
    packageInfo,
    autoloadMap.getTypes(),
    autoloadMap.getFuncs(),
    autoloadMap.getConstants(),
    autoloadMap.getTypeAliases(),
    autoloadMap.getModules()
  );
}

void RepoFileBuilder::Data::finish(
    const RepoGlobalData& global,
    std::vector<RepoUnitSymbols> unitSymbols,
    std::vector<UnitMethCallers> unitMethCallers,
    const PackageInfo& packageInfo,
    const RepoAutoloadMapBuilder::TypeNameMap& types,
    const RepoAutoloadMapBuilder::FuncNameMap& funcs,
    const RepoAutoloadMapBuilder::CaseSensitiveMap& constants,
    const RepoAutoloadMapBuilder::TypeNameMap& typeAliases,
    const RepoAutoloadMapBuilder::CaseSensitiveMap& modules) {
  auto outputGlobal = global;
  outputGlobal.RepoFileCapabilities = enableUnitEmitterReuse
    ? static_cast<uint32_t>(RepoFileCapability::RAW_UNIT_EMITTER_REUSE)
    : 0;

  // Global data
  {
    BlobEncoder encoder;
    encoder(outputGlobal);
    write(RepoFileChunks::GLOBAL_DATA, encoder.data(), encoder.size());
    always_assert(
      sizes.get(RepoFileChunks::GLOBAL_DATA) <= kGlobalDataSizeLimit);
  }

  // Package Info
  {
    BlobEncoder encoder;
    encoder(packageInfo);
    write(RepoFileChunks::PACKAGE_INFO, encoder.data(), encoder.size());
    always_assert(
      sizes.get(RepoFileChunks::PACKAGE_INFO) <= kPackageInfoSizeLimit);
  }

  // Unit Symbols
  auto const unitSymbolsBounds = listIndex(
    RepoFileIndexes::UNIT_SYMBOLS, std::move(unitSymbols));

  // Unit Meth Callers
  if (outputGlobal.hasRepoFileCapability(
        RepoFileCapability::RAW_UNIT_EMITTER_REUSE)) {
    listIndex(
      RepoFileIndexes::UNIT_METH_CALLERS, std::move(unitMethCallers));
  } else {
    listIndex(
      RepoFileIndexes::UNIT_METH_CALLERS,
      std::vector<UnitMethCallers>{});
  }

  // Unit Infos
  std::vector<Blob::Bounds> unitInfosBounds;
  {
    std::vector<RepoUnitInfo> list(unitEmittersIndex.size());
    for (auto const& unit : unitEmittersIndex) {
      list[unit.sn] = RepoUnitInfo {
        unit.sn,
        unit.path,
        unit.location,
        unitSymbolsBounds[unit.sn]
      };
    }

    unitInfosBounds = listIndex(RepoFileIndexes::UNIT_INFOS, list);
  }

  // Repo Autoload Map
  // Symbol to Blob::Bounds for the UnitInfo
  {
    auto key_lambda = [](auto const& it) { return it.first->toCppString(); };
    auto value_lambda = [&](auto const& it) {
      return &unitInfosBounds[it.second];
    };

    hashMapIndex<Blob::Bounds, TypeNameCompare>(
      RepoFileIndexes::AUTOLOAD_TYPES, types,
      key_lambda, value_lambda);
    hashMapIndex<Blob::Bounds, FuncNameCompare>(
      RepoFileIndexes::AUTOLOAD_FUNCS, funcs,
      key_lambda, value_lambda);
    hashMapIndex<Blob::Bounds, Blob::CaseSensitiveCompare>(
      RepoFileIndexes::AUTOLOAD_CONSTANTS, constants,
      key_lambda, value_lambda);
    hashMapIndex<Blob::Bounds, TypeNameCompare>(
      RepoFileIndexes::AUTOLOAD_TYPEALIASES, typeAliases,
      key_lambda, value_lambda);
    hashMapIndex<Blob::Bounds, Blob::CaseSensitiveCompare>(
      RepoFileIndexes::AUTOLOAD_MODULES, modules,
      key_lambda, value_lambda);
  }

  // Path to Blob::Bounds for the UnitInfo
  hashMapIndex<Blob::Bounds, Blob::CaseSensitiveCompare>(
    RepoFileIndexes::PATH_TO_UNIT_INFO, unitEmittersIndex,
    [](auto const& unit) { return unit.path->toCppString(); },
    [&](auto const& unit) { return &unitInfosBounds[unit.sn]; });

  Blob::Writer<RepoFileChunks, RepoFileIndexes>::finish();
}

////////////////////////////////////////////////////////////////////////////////

RepoFileBuilder::EncodedUE::EncodedUE(
    const UnitEmitter& ue,
    bool includeUnitMethCallers)
  : path{ue.m_filepath}
  , sn{ue.m_sn}
{
  if (includeUnitMethCallers) {
    for (auto const& fe : ue.fevec()) {
      if (!fe->isMethod() && (fe->attrs & AttrIsMethCaller) &&
          !fe->name->slice().endsWith("$memoize_impl")) {
        methCallers.push_back(fe->name);
      }
    }
  }

  BlobEncoder encoder;
  const_cast<UnitEmitter&>(ue).serde(encoder, false);
  blob = encoder.take();
}

////////////////////////////////////////////////////////////////////////////////

namespace {

void appendRange(std::vector<Blob::Bounds>& ranges,
                 const Blob::Bounds& location) {
  if (!ranges.empty() &&
      ranges.back().offset + ranges.back().size == location.offset) {
    ranges.back().size += location.size;
    return;
  }
  ranges.push_back(location);
}

bool boundsWithin(size_t size, const Blob::Bounds& bounds) {
  return bounds.offset <= size && bounds.size <= size - bounds.offset;
}

////////////////////////////////////////////////////////////////////////////////

}

// Reader state
struct RepoFileData::Data : Blob::Reader<RepoFileChunks, RepoFileIndexes> {
  struct IncrementalBaseUnit {
    RepoUnitInfo info;
    RepoUnitSymbols symbols;
    UnitMethCallers methCallers;
  };

  explicit Data(const std::string& path,
                Blob::ReadMode mode = Blob::ReadMode::PReadOnly)
    : inputPath{path} {
    init(path, kMagic, kCurrentVersion, mode);

    check(RepoFileChunks::UNIT_EMITTERS, 0);
    check(RepoFileChunks::GLOBAL_DATA, kGlobalDataSizeLimit);
    check(RepoFileChunks::PACKAGE_INFO, kPackageInfoSizeLimit);
    for (auto index : magic_enum::enum_values<RepoFileIndexes>()) {
      check(index, kIndexSizeLimit, kIndexDataSizeLimit);
    }

    globalData = readChunk<RepoGlobalData>(RepoFileChunks::GLOBAL_DATA);
    packageInfo = readChunk<PackageInfo>(RepoFileChunks::PACKAGE_INFO);
  }

  bool canReuseUnitEmitters() const {
    return globalData.hasRepoFileCapability(
      RepoFileCapability::RAW_UNIT_EMITTER_REUSE);
  }

  std::vector<IncrementalBaseUnit> loadUnitsForIncrementalBuild() {
    always_assert(canReuseUnitEmitters());
    auto const infosIndex = listIndex(RepoFileIndexes::UNIT_INFOS);
      always_assert(
        infosIndex.size == listIndex(RepoFileIndexes::UNIT_SYMBOLS).size);
      always_assert(
        infosIndex.size == listIndex(RepoFileIndexes::UNIT_METH_CALLERS).size);

    std::vector<IncrementalBaseUnit> units;
    units.reserve(infosIndex.size);
    DEBUG_ONLY folly::F14FastSet<std::string> paths;
    DEBUG_ONLY auto const infosRead = readList<RepoUnitInfo>(
      RepoFileIndexes::UNIT_INFOS,
      [&](size_t i, RepoUnitInfo info) {
        assertx(info.unitSn == i);
        assertx(info.path && !info.path->empty());
        assertx(info.emitterLocation.size != 0);
        assertx(boundsWithin(
          sizes.get(RepoFileChunks::UNIT_EMITTERS),
          info.emitterLocation));
        assertx(info.symbolsLocation.size != 0);
        assertx(boundsWithin(
          listIndex(RepoFileIndexes::UNIT_SYMBOLS).dataBounds.size,
          info.symbolsLocation));

        info.path = relativePathToSourceRoot(info.path);
        assertx(paths.emplace(info.path->toCppString()).second);

        units.push_back(IncrementalBaseUnit{std::move(info), {}, {}});
      }
    );
    assertx(infosRead == infosIndex.size);

    DEBUG_ONLY auto const symbolsRead = readList<RepoUnitSymbols>(
      RepoFileIndexes::UNIT_SYMBOLS,
      [&](size_t i, RepoUnitSymbols symbols) {
        assertx(i < units.size());
        units[i].symbols = std::move(symbols);
      }
    );
      assertx(symbolsRead == units.size());

    DEBUG_ONLY auto const methCallersRead = readList<UnitMethCallers>(
      RepoFileIndexes::UNIT_METH_CALLERS,
      [&](size_t i, UnitMethCallers methCallers) {
        assertx(i < units.size());
        units[i].methCallers = std::move(methCallers);
      }
    );
    assertx(methCallersRead == units.size());

    if constexpr (debug) {
      std::vector<Blob::Bounds> emitterLocations;
      emitterLocations.reserve(units.size());
      for (auto const& unit : units) {
        emitterLocations.push_back(unit.info.emitterLocation);
      }
      std::sort(
        emitterLocations.begin(),
        emitterLocations.end(),
        [](auto const& left, auto const& right) {
          return left.offset < right.offset;
        }
      );
      for (size_t i = 1; i < emitterLocations.size(); ++i) {
        assertx(
          emitterLocations[i].offset >=
          emitterLocations[i - 1].offset + emitterLocations[i - 1].size);
      }
    }
    return units;
  }

  RepoGlobalData globalData;

  PackageInfo packageInfo;

  std::string inputPath;

  CaseSensitiveHashMapIndex pathToUnitInfoBoundsIndex;
  Blob::ListIndex unitInfosIndex;
  Blob::ListIndex unitSymbolsIndex;

  std::atomic<bool> loadedGlobalTables{false};

  using UnitSnToUnitInfoMap = folly_concurrent_hash_map_simd<int64_t,
                                                            RepoUnitInfo>;
  mutable UnitSnToUnitInfoMap snToUnitInfo{};

  using PathToSymbolsMap = folly_concurrent_hash_map_simd<
    const StringData*, RepoUnitSymbols, string_data_hash, string_data_same>;
  mutable PathToSymbolsMap pathToSymbols{};

private:
  template <typename T>
  T readChunk(RepoFileChunks chunk) {
    auto blob = fd.readBlob(offsets.get(chunk), sizes.get(chunk));
    T value;
    blob.decoder(value);
    blob.decoder.assertDone();
    return value;
  }

  template <typename T, typename F>
  size_t readList(RepoFileIndexes index, F&& consume) {
    auto const list = listIndex(index);
    auto blob = fd.readBlob(list.dataBounds.offset, list.dataBounds.size);
    size_t i = 0;
    while (blob.decoder.remaining() > 0) {
      T value;
      blob.decoder(value);
      consume(i++, std::move(value));
    }
    return i;
  }
};

namespace {

template <typename Writer>
void copyUnitEmitters(
    Writer& output,
    const RepoFileData::Data& input,
    const std::vector<Blob::Bounds>& ranges) {
  constexpr size_t kCopyBufferSize = 1 << 20;
  std::vector<char> buffer(kCopyBufferSize);

  auto const chunkOffset = input.offsets.get(RepoFileChunks::UNIT_EMITTERS);
  for (auto const& range : ranges) {
    size_t copied = 0;
    while (copied < range.size) {
      auto const size = std::min(buffer.size(), range.size - copied);
      input.fd.pread(
        buffer.data(), size, chunkOffset + range.offset + copied);
      output.write(RepoFileChunks::UNIT_EMITTERS, buffer.data(), size);
      copied += size;
    }
  }
}

std::unique_ptr<RepoFileData::Data> s_repoFileData{};

const RepoUnitInfo& getUnitInfoFromUnitSn(const RepoFileData::Data& data,
                                          int64_t unitSn) {
  auto index = data.unitInfosIndex;
  assertx(unitSn >= 0 && unitSn < index.size);

  auto acc = data.snToUnitInfo.find(unitSn);
  if (acc != data.snToUnitInfo.cend()) {
    return acc->second;
  }

  auto res = data.getFromIndex<RepoUnitInfo>(index, unitSn);
  assertx(res);
  auto insertRes = data.snToUnitInfo.insert(unitSn, *res);
  return insertRes.first->second;
}

const RepoUnitInfo& getUnitInfoFromBounds(const RepoFileData::Data& data,
                                          const Blob::Bounds& bounds) {
  FTRACE(1, "getUnitInfoFromBounds {} {} {} {}\n", bounds.offset, bounds.size,
         data.unitInfosIndex.dataBounds.offset,
         data.unitInfosIndex.dataBounds.size);
  assertx(bounds.size > 0);
  assertx(bounds.offset < data.unitInfosIndex.dataBounds.size);
  assertx(bounds.offset + bounds.size
          <= data.unitInfosIndex.dataBounds.size);

  auto blob = data.fd.readBlob(
    data.unitInfosIndex.dataBounds.offset + bounds.offset, bounds.size);
  RepoUnitInfo info;
  blob.decoder(info);
  blob.decoder.assertDone();

  auto insertRes = data.snToUnitInfo.insert(info.unitSn, info);
  return insertRes.first->second;
}

RepoUnitSymbols getUnitSymbolsFromBounds(const RepoFileData::Data& data,
                                         const Blob::Bounds& bounds) {
  FTRACE(1, "getUnitSymbolsFromBounds {} {} {} {}\n", bounds.offset,
         bounds.size, data.unitSymbolsIndex.dataBounds.offset,
         data.unitSymbolsIndex.dataBounds.size);
  assertx(bounds.size > 0);
  assertx(bounds.offset < data.unitSymbolsIndex.dataBounds.size);
  assertx(bounds.offset + bounds.size
          <= data.unitSymbolsIndex.dataBounds.size);

  auto blob = data.fd.readBlob(
    data.unitSymbolsIndex.dataBounds.offset + bounds.offset, bounds.size);
  RepoUnitSymbols symbols;
  blob.decoder(symbols);
  blob.decoder.assertDone();

  return symbols;
}

template <typename KeyCompare>
const RepoUnitInfo* findUnitInfoFromKey(
    const RepoFileData::Data& data,
    const Blob::HashMapIndex<KeyCompare>& map,
    const StringData* key) {
  auto bounds = data.getFromIndex<Blob::Bounds>(map, key->slice());
  if (!bounds) {
    return nullptr;
  }
  return &getUnitInfoFromBounds(data, *bounds);
}

const RepoUnitInfo* findUnitInfoFromPath(const RepoFileData::Data& data,
                                         const StringData* path) {
  auto searchPath = path;
  if (!path->empty()) {
    searchPath = relativePathToSourceRoot(path);
  }

  FTRACE(1, "findUnitInfoFromPath {} {}\n", searchPath->data(), path->data());

  return findUnitInfoFromKey(data, data.pathToUnitInfoBoundsIndex, searchPath);
}

}

////////////////////////////////////////////////////////////////////////////////

namespace {

void validateIncrementalPackageInfo(
    const PackageInfo& basePackageInfo,
    const PackageInfo& packageInfo) {
  auto const sameMap = [](auto const& left,
                          auto const& right,
                          auto const& sameValue) {
    if (left.size() != right.size()) return false;
    for (auto const& [name, value] : left) {
      auto const it = right.find(name);
      if (it == right.end() || !sameValue(value, it->second)) return false;
    }
    return true;
  };
  auto const samePackage = [](const PackageInfo::Package& left,
                              const PackageInfo::Package& right) {
    return left.m_includes == right.m_includes &&
      left.m_soft_includes == right.m_soft_includes &&
      left.m_include_paths == right.m_include_paths &&
      left.m_enable_strict_isolation == right.m_enable_strict_isolation &&
      left.m_raiseDynamicClassLoadError ==
        right.m_raiseDynamicClassLoadError &&
      left.m_allow_deployed_packages_checking ==
        right.m_allow_deployed_packages_checking;
  };
  auto const sameDeployment = [](const PackageInfo::Deployment& left,
                                 const PackageInfo::Deployment& right) {
    return left.m_packages == right.m_packages &&
      left.m_soft_packages == right.m_soft_packages;
  };
  auto const sameImplicitPackageFamily = [](
      const PackageInfo::ImplicitPackageFamily& left,
      const PackageInfo::ImplicitPackageFamily& right) {
    return left.m_path == right.m_path &&
      left.m_includes == right.m_includes &&
      left.m_soft_includes == right.m_soft_includes &&
      left.m_raiseDynamicClassLoadError ==
        right.m_raiseDynamicClassLoadError;
  };

  if (!sameMap(
        basePackageInfo.packages(),
        packageInfo.packages(),
        samePackage
      ) ||
      !sameMap(
        basePackageInfo.deployments(),
        packageInfo.deployments(),
        sameDeployment
      ) ||
      !sameMap(
        basePackageInfo.implicitPackageFamilies(),
        packageInfo.implicitPackageFamilies(),
        sameImplicitPackageFamily
      )) {
    throw std::runtime_error(
      "Incremental RepoFile base PackageInfo does not match the current build"
    );
  }
}

}

RepoFileData::RepoFileData(const std::string& path)
  : m_data{std::make_unique<Data>(path)}
{}

RepoFileData::~RepoFileData() = default;

void RepoFileBuilder::addFrom(
    RepoFileData& base,
    RepoAutoloadMapBuilder& autoload,
    std::vector<std::string> exclude,
    const PackageInfo& packageInfo) {
  assertx(m_data);
  assertx(base.m_data);
  auto& baseData = *base.m_data;

  auto const aliasesBase = [&](const std::string& candidate) {
    if (candidate == baseData.inputPath) return true;
    std::error_code error;
    return std::filesystem::equivalent(candidate, baseData.inputPath, error);
  };
  if (aliasesBase(m_data->destFilename) ||
      aliasesBase(m_data->sourceFilename)) {
    throw std::runtime_error(fmt::format(
      "Incremental RepoFile output '{}' aliases its base '{}'",
      m_data->destFilename,
      baseData.inputPath
    ));
  }
  if (!m_data->enableUnitEmitterReuse) {
    throw std::runtime_error(
      "Incremental RepoFile output must enable raw unit emitter reuse"
    );
  }
  if (!baseData.canReuseUnitEmitters()) {
    throw std::runtime_error(fmt::format(
      "RepoFile '{}' cannot be used as an incremental base",
      baseData.inputPath
    ));
  }
  validateIncrementalPackageInfo(baseData.packageInfo, packageInfo);

  std::sort(exclude.begin(), exclude.end());

  auto const addedUnitCount = m_data->unitEmittersIndex.size();
  // Units are encoded concurrently, so they are not necessarily added in
  // serial-number order. The added units own exactly the SNs
  // [0, addedUnitCount), which is what lets the retained base units below take
  // their SN from their position in the index without colliding.
  folly::F14FastMap<int64_t, size_t> addedUnitIndexBySn;
  addedUnitIndexBySn.reserve(addedUnitCount);
  for (size_t i = 0; i < addedUnitCount; ++i) {
    auto const sn = m_data->unitEmittersIndex[i].sn;
    always_assert(sn >= 0 && static_cast<size_t>(sn) < addedUnitCount);
    always_assert(addedUnitIndexBySn.emplace(sn, i).second);
  }
  auto const unitIndexBySn = [&](int64_t sn) {
    auto const it = addedUnitIndexBySn.find(sn);
    auto const index =
      it != addedUnitIndexBySn.end() ? it->second : static_cast<size_t>(sn);
    always_assert(sn >= 0 && index < m_data->unitEmittersIndex.size());
    return index;
  };

  auto baseUnits = baseData.loadUnitsForIncrementalBuild();

  std::vector<RepoFileData::Data::IncrementalBaseUnit*> retainedBaseUnits;
  retainedBaseUnits.reserve(baseUnits.size());
  for (auto& unit : baseUnits) {
    auto const path = unit.info.path->toCppString();
    if (!containsPathOrAncestor(exclude, path)) {
      retainedBaseUnits.push_back(&unit);
    }
  }

  auto nextEmitterOffset =
    m_data->sizes.get(RepoFileChunks::UNIT_EMITTERS);
  std::vector<Blob::Bounds> baseRanges;
  for (auto unit : retainedBaseUnits) {
    auto const baseLocation = unit->info.emitterLocation;
    unit->info.emitterLocation = {
      nextEmitterOffset, baseLocation.size};
    nextEmitterOffset += baseLocation.size;
    appendRange(baseRanges, baseLocation);
  }
  copyUnitEmitters(*m_data, baseData, baseRanges);

  auto const failSymbolCollision = [&](const char* kind,
                                       const StringData* symbol,
                                       int64_t existingUnitSn,
                                       int64_t incomingUnitSn) {
    throw std::runtime_error(fmt::format(
      "More than one {} with the name {}. In {} and {}",
      kind,
      symbol->data(),
      m_data->unitEmittersIndex[unitIndexBySn(existingUnitSn)].path->data(),
      m_data->unitEmittersIndex[unitIndexBySn(incomingUnitSn)].path->data()
    ));
  };
  auto const addSymbol = [&](auto& map,
                             const StringData* symbol,
                             int64_t unitSn,
                             const char* kind) {
    auto const result = map.emplace(symbol, unitSn);
    if (!result.second) {
      failSymbolCollision(kind, symbol, result.first->second, unitSn);
    }
  };
  auto const checkNoSymbol = [&](const auto& map,
                                 const StringData* symbol,
                                 int64_t unitSn) {
    auto const it = map.find(symbol);
    if (it != map.cend()) {
      failSymbolCollision("symbol", symbol, it->second, unitSn);
    }
  };
  auto const isMethCaller = [](const UnitMethCallers& methCallers,
                               const StringData* symbol) {
    return std::find(methCallers.begin(), methCallers.end(), symbol) !=
      methCallers.end();
  };
  // The same meth caller may be defined by several units, so an ordinary
  // function claiming a name already held by an added unit's meth caller is a
  // precedence question rather than a duplicate definition.
  auto const definedByAddedMethCaller = [&](const StringData* symbol,
                                            int64_t unitSn) {
    auto const it = addedUnitIndexBySn.find(unitSn);
    return it != addedUnitIndexBySn.end() &&
      isMethCaller(m_data->unitEmittersIndex[it->second].methCallers, symbol);
  };

  m_data->unitEmittersIndex.reserve(
    addedUnitCount + retainedBaseUnits.size());
  for (auto unit : retainedBaseUnits) {
    auto const unitSn =
      static_cast<int64_t>(m_data->unitEmittersIndex.size());
    m_data->unitEmittersIndex.push_back(Data::UnitEmitterIndex{
      unit->info.path,
      unitSn,
      unit->info.emitterLocation,
      std::move(unit->methCallers),
    });
    auto const& methCallers =
      m_data->unitEmittersIndex.back().methCallers;

    for (auto const& [symbol, type] : unit->symbols) {
      if (type == RepoSymbolType::TYPE) {
        checkNoSymbol(autoload.m_typeAliases, symbol, unitSn);
        addSymbol(autoload.m_types, symbol, unitSn, "type");
      } else if (type == RepoSymbolType::FUNC) {
        // This unit's own meth callers are added by the loop below, once every
        // ordinary function has had a chance to claim the name.
        if (isMethCaller(methCallers, symbol)) continue;
        auto const result = autoload.m_funcs.emplace(symbol, unitSn);
        if (!result.second) {
          if (!definedByAddedMethCaller(symbol, result.first->second)) {
            failSymbolCollision(
              "function", symbol, result.first->second, unitSn);
          }
          autoload.m_funcs.assign(symbol, unitSn);
        }
      } else if (type == RepoSymbolType::CONSTANT) {
        addSymbol(autoload.m_constants, symbol, unitSn, "constant");
      } else if (type == RepoSymbolType::TYPE_ALIAS) {
        checkNoSymbol(autoload.m_types, symbol, unitSn);
        addSymbol(autoload.m_typeAliases, symbol, unitSn, "type alias");
      } else {
        assertx(type == RepoSymbolType::MODULE);
        addSymbol(autoload.m_modules, symbol, unitSn, "module");
      }
    }
    for (auto const symbol : methCallers) {
      autoload.m_funcs.emplace(symbol, unitSn);
    }
  }
}

////////////////////////////////////////////////////////////////////////////////

void RepoFile::init(const std::string& path) {
  assertx(!s_repoFileData);
  s_repoFileData = std::make_unique<RepoFileData::Data>(path);
}

void RepoFile::destroy() {
  if (!s_repoFileData) return;
  s_repoFileData.reset();
}

const RepoGlobalData& RepoFile::globalData() {
  assertx(s_repoFileData);
  return s_repoFileData->globalData;
}

void RepoFile::loadGlobalTables(bool loadAutoloadMap) {
  assertx(s_repoFileData);
  assertx(!s_repoFileData->loadedGlobalTables.load());
  auto& data = *s_repoFileData;

  data.unitSymbolsIndex = data.listIndex(RepoFileIndexes::UNIT_SYMBOLS);
  data.unitInfosIndex = data.listIndex(RepoFileIndexes::UNIT_INFOS);

  data.pathToUnitInfoBoundsIndex = data.hashMapIndex<Blob::CaseSensitiveCompare>(
    RepoFileIndexes::PATH_TO_UNIT_INFO);

  // Repo autoload map
  if (loadAutoloadMap) {
    AutoloadHandler::setRepoAutoloadMap(
      std::make_unique<RepoAutoloadMap>(
        data.hashMapIndex<TypeNameCompare>(RepoFileIndexes::AUTOLOAD_TYPES),
        data.hashMapIndex<FuncNameCompare>(RepoFileIndexes::AUTOLOAD_FUNCS),
        data.hashMapIndex<Blob::CaseSensitiveCompare>(RepoFileIndexes::AUTOLOAD_CONSTANTS),
        data.hashMapIndex<TypeNameCompare>(RepoFileIndexes::AUTOLOAD_TYPEALIASES),
        data.hashMapIndex<Blob::CaseSensitiveCompare>(RepoFileIndexes::AUTOLOAD_MODULES)
      )
    );
  }

  data.loadedGlobalTables.store(true);
}

const PackageInfo& RepoFile::packageInfo() {
  assertx(s_repoFileData);
  return s_repoFileData->packageInfo;
}

std::unique_ptr<UnitEmitter>
RepoFile::loadUnitEmitter(const StringData* path,
                          const RepoUnitInfo* info,
                          bool lazy) {
  assertx(s_repoFileData);
  assertx(s_repoFileData->loadedGlobalTables.load());
  auto& data = *s_repoFileData;

  if (info == nullptr) {
    assertx(path->isStatic());
    info = findUnitInfoFromPath(data, path);
    if (!info) return nullptr;
    assertx(info->path == path);
  }

  auto blob = data.fd.readBlob(
    data.offsets.get(RepoFileChunks::UNIT_EMITTERS) +
      info->emitterLocation.offset,
    info->emitterLocation.size
  );

  auto ue = UnitEmitter::makeForSerde(
    SHA1{(uint64_t)info->unitSn},
    SHA1{},
    RepoOptions::defaults().packageInfo()
  );
  ue->m_filepath = info->path;
  ue->m_sn = info->unitSn;
  ue->serde(blob.decoder, lazy);
  ue->finish();
  blob.decoder.assertDone();
  return ue;
}

size_t RepoFile::remainingSizeOfUnit(int64_t unitSn, Token token) {
  assertx(s_repoFileData);
  assertx(s_repoFileData->loadedGlobalTables.load());
  auto const& data = *s_repoFileData;
  auto info = getUnitInfoFromUnitSn(data, unitSn);
  assertx(token <= info.emitterLocation.size);
  return info.emitterLocation.size - token;
}

void RepoFile::readRawFromUnit(int64_t unitSn, Token token,
                               unsigned char* ptr, size_t len) {
  assertx(s_repoFileData);
  assertx(s_repoFileData->loadedGlobalTables.load());
  assertx(unitSn >= 0);
  auto const& data = *s_repoFileData;
  auto info = getUnitInfoFromUnitSn(data, unitSn);
  assertx(token <= info.emitterLocation.size);
  always_assert(token + len <= info.emitterLocation.size);
  data.fd.pread(ptr, len,
    data.offsets.get(RepoFileChunks::UNIT_EMITTERS) +
      info.emitterLocation.offset + token
  );
}

const StringData* RepoFile::findUnitPath(int64_t unitSn) {
  assertx(s_repoFileData);
  assertx(s_repoFileData->loadedGlobalTables.load());
  assertx(unitSn >= 0);
  auto& data = *s_repoFileData;
  if (unitSn >= data.unitInfosIndex.size) return nullptr;
  auto info = getUnitInfoFromUnitSn(data, unitSn);
  return info.path;
}

const StringData* RepoFile::findUnitPath(const SHA1& sha1) {
  // SHA1s aren't a thing for RepoFile. When we create a UnitEmitter,
  // we just set the SHA1 to be the SN. So, if you ask for an
  // UnitEmitter by SHA1, we'll only have it if all the words are zero
  // except the last one (which we do a normal SN search for). We only
  // have this function at all for compatibility for tc-print.
  always_assert(sha1.q.size() == 5);
  if (sha1.q[0] != 0 ||
      sha1.q[1] != 0 ||
      sha1.q[2] != 0 ||
      sha1.q[3] != 0) {
    return nullptr;
  }
  return findUnitPath(sha1.q[4]);
}

template <typename KeyCompare>
const RepoUnitInfo* RepoFile::findUnitInfo(
    const Blob::HashMapIndex<KeyCompare>& map, const StringData* key) {
  // We need to check here because sometime people call this before RepoFileData
  // has been inited
  if (map.size == 0) {
    return nullptr;
  }

  assertx(s_repoFileData);
  assertx(s_repoFileData->loadedGlobalTables.load());
  auto& data = *s_repoFileData;
  return findUnitInfoFromKey(data, map, key);
}

template
const RepoUnitInfo* RepoFile::findUnitInfo(
  const CaseSensitiveHashMapIndex& map, const StringData* key);

template
const RepoUnitInfo* RepoFile::findUnitInfo(
  const HashMapTypeIndex& map, const StringData* key);


const RepoUnitSymbols* RepoFile::findUnitSymbols(const StringData* path) {
  assertx(s_repoFileData);
  assertx(s_repoFileData->loadedGlobalTables.load());
  auto& data = *s_repoFileData;

  // Convert to relative path before looking in the cache because that is what
  // is used when setting the cache
  if (!path->empty()) {
    path = relativePathToSourceRoot(path);
  }

  auto acc = data.pathToSymbols.find(path);
  if (acc != data.pathToSymbols.cend()) {
    return &acc->second;
  }

  auto info = findUnitInfoFromPath(data, path);
  if (!info) {
    return nullptr;
  }

  auto res = getUnitSymbolsFromBounds(data, info->symbolsLocation);
  auto insertRes = data.pathToSymbols.insert(info->path, res);
  return &insertRes.first->second;
}

std::vector<const StringData*> RepoFile::enumerateUnits() {
  assertx(s_repoFileData);
  assertx(s_repoFileData->loadedGlobalTables.load());
  auto& data = *s_repoFileData;

  std::vector<const StringData*> ret;
  ret.reserve(data.unitInfosIndex.size);

  auto dataBlob = data.fd.readBlob(data.unitInfosIndex.dataBounds.offset,
                                   data.unitInfosIndex.dataBounds.size);
  while (dataBlob.decoder.remaining() > 0) {
    RepoUnitInfo info;
    dataBlob.decoder(info);
    ret.push_back(info.path);
  }

  // Maintain deterministic order
  std::sort(
    ret.begin(), ret.end(),
    [] (const StringData* a, const StringData* b) {
      return strcmp(a->data(), b->data()) < 0;
    }
  );
  return ret;
}

std::size_t RepoFile::numUnits() {
  assertx(s_repoFileData);
  assertx(s_repoFileData->loadedGlobalTables.load());
  return s_repoFileData->unitInfosIndex.size;
}

////////////////////////////////////////////////////////////////////////////////
}
