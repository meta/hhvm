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

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string_view>
#include <thread>

#include <fmt/core.h>
#include <folly/Likely.h>
#include <folly/Synchronized.h>
#include <folly/Unit.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/futures/Future.h>
#include <folly/futures/FutureSplitter.h>
#include <folly/logging/xlog.h>

#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/base/array-iterator.h"
#include "hphp/runtime/base/request-info.h"
#include "hphp/runtime/base/type-array.h"
#include "hphp/runtime/base/type-string.h"
#include "hphp/runtime/base/type-variant.h"
#include "hphp/runtime/base/typed-value.h"
#include "hphp/runtime/base/unit-cache.h"
#include "hphp/runtime/ext/facts/exception.h"
#include "hphp/runtime/ext/facts/fact-extractor.h"
#include "hphp/runtime/ext/facts/facts-store.h"
#include "hphp/runtime/ext/facts/file-facts.h"
#include "hphp/runtime/ext/facts/log-perf.h"
#include "hphp/runtime/ext/facts/path-and-hash.h"
#include "hphp/runtime/ext/facts/string-ptr.h"
#include "hphp/runtime/ext/facts/symbol-map.h"
#include "hphp/runtime/ext/facts/symbol-types.h"
#include "hphp/runtime/ext/facts/thread-factory.h"
#include "hphp/util/assertions.h"
#include "hphp/util/configs/autoload.h"
#include "hphp/util/hash-set.h"
#include "hphp/util/optional.h"
#include "hphp/util/sandbox-events.h"
#include "hphp/util/sha1.h"

namespace fs = std::filesystem;

namespace HPHP {
namespace Facts {
namespace {

/**
 * Return a path relative to `root` with the same canonical location as `p`. If
 * `p` does not exist within `root`, return `std::nullopt`.
 */
Optional<fs::path> resolvePathRelativeToRoot(
    const fs::path& path,
    const fs::path& root) {
  if (path.is_relative())
    return path;
  // TODO: for snapshot semantics, this should not access the filesystem.
  if (!fs::exists(path))
    return std::nullopt;
  return fs::relative(path, root);
}

Optional<Path> ensureRelativePath(const OptString& path, const fs::path& root) {
  if (path.empty()) {
    return {};
  }

  if (path.slice().at(0) != '/') {
    return Path{*path.get()};
  }
  auto resolvedPath =
      resolvePathRelativeToRoot(fs::path{path.toCppString()}, root);
  if (!resolvedPath) {
    return {};
  }
  return Path{*resolvedPath};
}

constexpr std::string_view kKindFilterKey{"kind"};
constexpr std::string_view kDeriveKindFilterKey{"derive_kind"};
constexpr std::string_view kExtendsFilterKey{"extends"};
constexpr std::string_view kRequiresFilterKey{"require extends"};
constexpr std::string_view kTypeFlagFilterKey{"flags"};
constexpr std::string_view kTypeFlagFilterValEmpty{"empty"};
constexpr std::string_view kTypeFlagFilterValAbstract{"abstract"};
constexpr std::string_view kTypeFlagFilterValFinal{"final"};

// Filter on TypeKind: Class, Enum, Interface, or Trait
struct KindFilterData {
  bool m_removeClasses = false;
  bool m_removeEnums = false;
  bool m_removeInterfaces = false;
  bool m_removeTraits = false;

  TypeKindMask toMask() const {
    auto mask = kTypeKindAll;
    if (m_removeClasses) {
      mask &= ~static_cast<int>(TypeKind::Class);
    }
    if (m_removeEnums) {
      mask &= ~static_cast<int>(TypeKind::Enum);
    }
    if (m_removeInterfaces) {
      mask &= ~static_cast<int>(TypeKind::Interface);
    }
    if (m_removeTraits) {
      mask &= ~static_cast<int>(TypeKind::Trait);
    }
    return mask;
  }

  constexpr bool doesIncludeEverything() const noexcept {
    return !(
        m_removeClasses || m_removeEnums || m_removeInterfaces ||
        m_removeTraits);
  }

  static constexpr KindFilterData includeEverything() noexcept {
    return {
        .m_removeClasses = false,
        .m_removeEnums = false,
        .m_removeInterfaces = false,
        .m_removeTraits = false};
  }

  static constexpr KindFilterData removeEverything() noexcept {
    return {
        .m_removeClasses = true,
        .m_removeEnums = true,
        .m_removeInterfaces = true,
        .m_removeTraits = true};
  }

  static KindFilterData createFromKeyset(const ArrayData* kindFilter) {
    if (kindFilter == nullptr) {
      // We weren't presented with a kindFilter, so don't remove any kinds
      return KindFilterData::includeEverything();
    }
    // Remove the kinds that aren't mentioned in the filter
    auto data = KindFilterData::removeEverything();
    IterateKV(kindFilter, [&](TypedValue k, TypedValue UNUSED v) {
      if (!tvIsString(k)) {
        return;
      }
      StringPtr kind{k.m_data.pstr};
      if (kind == kTypeKindClass) {
        data.m_removeClasses = false;
      } else if (kind == kTypeKindEnum) {
        data.m_removeEnums = false;
      } else if (kind == kTypeKindInterface) {
        data.m_removeInterfaces = false;
      } else if (kind == kTypeKindTrait) {
        data.m_removeTraits = false;
      }
    });
    return data;
  }
};

// Filter on base/derived relationship (extends or requires)
struct DeriveKindFilterData {
  static constexpr DeriveKindFilterData includeEverything() noexcept {
    return {.m_removeExtends = false, .m_removeRequires = false};
  }

  static constexpr DeriveKindFilterData removeEverything() noexcept {
    return {.m_removeExtends = true, .m_removeRequires = true};
  }

  static DeriveKindFilterData createFromKeyset(
      const ArrayData* deriveKindFilter) {
    DeriveKindFilterData filters = DeriveKindFilterData::removeEverything();
    IterateKV(deriveKindFilter, [&](TypedValue k, UNUSED TypedValue v) {
      if (!tvIsString(k)) {
        return;
      }
      StringPtr key{k.m_data.pstr};
      if (key == kExtendsFilterKey) {
        filters.m_removeExtends = false;
      } else if (key == kRequiresFilterKey) {
        filters.m_removeRequires = false;
      }
    });
    return filters;
  }

  DeriveKindMask toMask() const {
    auto mask = kDeriveKindAll;
    if (m_removeExtends) {
      mask &= ~static_cast<int>(DeriveKind::Extends);
    }
    if (m_removeRequires) {
      mask &= ~static_cast<int>(DeriveKind::RequireThisAs);
      mask &= ~static_cast<int>(DeriveKind::RequireClass);
      mask &= ~static_cast<int>(DeriveKind::RequireExtends);
      mask &= ~static_cast<int>(DeriveKind::RequireImplements);
    }
    return mask;
  }

  bool m_removeExtends = false;
  bool m_removeRequires = false;
};

/**
 * Passed into FactsAPI queries to filter down to only classes that are
 * abstract, final, or neither (empty flag).
 */
struct TypeFlagFilterData {
  bool m_removeEmpty;
  bool m_removeAbstract;
  bool m_removeFinal;

  static constexpr TypeFlagFilterData includeEverything() noexcept {
    return {
        .m_removeEmpty = false,
        .m_removeAbstract = false,
        .m_removeFinal = false};
  }

  static constexpr TypeFlagFilterData removeEverything() noexcept {
    return {
        .m_removeEmpty = true, .m_removeAbstract = true, .m_removeFinal = true};
  }

  /**
   * We'll use this to check on equality to includeEverything() and
   * removeEverything()
   */
  bool operator==(const TypeFlagFilterData& other) const {
    return m_removeEmpty == other.m_removeEmpty &&
        m_removeAbstract == other.m_removeAbstract &&
        m_removeFinal == other.m_removeFinal;
  }

  /**
   * The ArrayData* is how the data comes in from hack.  It corresponds
   * to the contents of the 'flags' field on the DeriveFilters shape. If that
   * key isn't provided, this will never be called and so we will include
   * everything.
   */
  static TypeFlagFilterData createFromKeyset(const ArrayData* typeFlagFilter) {
    TypeFlagFilterData out = TypeFlagFilterData::removeEverything();
    IterateKV(typeFlagFilter, [&](TypedValue k, UNUSED TypedValue v) {
      if (!tvIsString(k)) {
        return;
      }
      StringPtr key{k.m_data.pstr};
      if (key == kTypeFlagFilterValEmpty) {
        out.m_removeEmpty = false;
      } else if (key == kTypeFlagFilterValAbstract) {
        out.m_removeAbstract = false;
      } else if (key == kTypeFlagFilterValFinal) {
        out.m_removeFinal = false;
      } else {
        HPHP::SystemLib::throwRuntimeExceptionObject(
            fmt::format("Passed invalid key {}", key->toCppString()));
      }
    });
    return out;
  }
};

struct TypeFilterData {
  KindFilterData m_kindFilters;
  DeriveKindFilterData m_deriveKindFilters;
  TypeFlagFilterData m_typeFlagFilters;

  static TypeFilterData includeEverything() noexcept {
    return {
        .m_kindFilters = KindFilterData::includeEverything(),
        .m_deriveKindFilters = DeriveKindFilterData::includeEverything(),
        .m_typeFlagFilters = TypeFlagFilterData::includeEverything()};
  }

  static TypeFilterData createFromShape(const ArrayData* filters) {
    if (filters == nullptr) {
      return TypeFilterData::includeEverything();
    }
    // Default to including everything. If a keyset is provided, include only
    // the members of that keyset.
    TypeFilterData typeFilters = TypeFilterData::includeEverything();
    IterateKV(filters, [&](TypedValue k, TypedValue v) {
      if (!tvIsString(k)) {
        return;
      }
      StringPtr key{k.m_data.pstr};

      if (key == kKindFilterKey) {
        if (tvIsArrayLike(v)) {
          typeFilters.m_kindFilters =
              KindFilterData::createFromKeyset(v.m_data.parr);
        }
      } else if (key == kDeriveKindFilterKey) {
        if (tvIsArrayLike(v)) {
          typeFilters.m_deriveKindFilters =
              DeriveKindFilterData::createFromKeyset(v.m_data.parr);
        }
      } else if (key == kTypeFlagFilterKey) {
        if (tvIsArrayLike(v)) {
          typeFilters.m_typeFlagFilters =
              TypeFlagFilterData::createFromKeyset(v.m_data.parr);
        }
      }
    });
    return typeFilters;
  }
};

// SHA1 hash of an empty file
constexpr folly::StringPiece kEmptyFileSha1Hash =
    "da39a3ee5e6b4b0d3255bfef95601890afd80709";

std::string curthread() {
  std::stringstream s;
  s << std::this_thread::get_id();
  return s.str();
}

std::vector<fs::path> removeHashes(
    std::vector<PathAndOptionalHash>&& pathsWithHashes) {
  std::vector<fs::path> paths;
  paths.reserve(pathsWithHashes.size());
  for (auto&& pathAndHash : std::move(pathsWithHashes)) {
    paths.push_back(std::move(pathAndHash.m_path));
  }
  return paths;
}

/**
 * Produce a lazy class `vec<class<mixed>>` for Facts functions
 * that return known classes. NOTE: there is a possibility of
 * conflating some aliases with classes here due to the lack of
 * distinction on the Type symbol kind, but it should be OK on
 * functions that inspect class hierarchies.
 */
Array makeVecOfLazyClass(const std::vector<Symbol<SymKind::Type>>& vector) {
  auto ret = VecInit{vector.size()};
  for (auto const& str : vector) {
    ret.append(make_tv<KindOfLazyClass>(LazyClassData::create(str.get())));
  }
  auto arr = ret.toArray();
  arr.get()->sort(0, true);
  return arr;
}

/**
 * Convert a C++ StringData structure into a Hack `vec<string>`.
 */
template <typename StringPtrIterable>
Array makeVecOfString(StringPtrIterable&& vector) {
  auto ret = VecInit{vector.size()};
  for (auto&& str : std::forward<StringPtrIterable>(vector)) {
    ret.append(VarNR{StrNR{str.get()}}.tv());
  }
  return ret.toArray();
}

/**
 * Convert a C++ StringData structure into a Hack `vec<(string, string)>`.
 */
Array makeVecOfStringString(const std::vector<MethodDecl>& vector) {
  auto ret = VecInit{vector.size()};
  for (auto const& [typeDecl, method] : vector) {
    auto stringStringTuple = VecInit{2};
    stringStringTuple.append(
        make_tv<KindOfPersistentString>(typeDecl.m_name.get()));
    stringStringTuple.append(make_tv<KindOfPersistentString>(method.get()));

    ret.append(stringStringTuple.toArray());
  }
  return ret.toArray();
}

Array makeVecOfFunctionString(const std::vector<FunctionDecl>& vector) {
  auto ret = VecInit{vector.size()};
  for (auto const& function : vector) {
    ret.append(make_tv<KindOfPersistentString>(function.m_name.get()));
  }
  return ret.toArray();
}

TypedValue tvFromDynamic(folly::dynamic&& dy) {
  if (dy.isBool()) {
    return make_tv<KindOfBoolean>(std::move(dy).getBool());
  }
  if (dy.isInt()) {
    return make_tv<KindOfInt64>(std::move(dy).getInt());
  }
  if (dy.isDouble()) {
    return make_tv<KindOfDouble>(std::move(dy).getInt());
  }
  if (dy.isString()) {
    return make_tv<KindOfPersistentString>(
        makeStaticString(std::move(dy).getString()));
  }
  if (dy.isArray()) {
    VecInit ret{dy.size()};
    for (auto&& v : std::move(dy)) {
      ret.append(tvFromDynamic(std::move(v)));
    }
  }
  if (dy.isObject()) {
    DictInit ret{dy.size()};
    for (auto [k, v] : dy.items()) {
      if (k.isInt()) {
        ret.set(k.getInt(), tvFromDynamic(std::move(v)));
      } else if (k.isString()) {
        ret.set(k.getString(), tvFromDynamic(std::move(v)));
      }
    }
  }
  return make_tv<KindOfNull>();
}

const StaticString s_TypeKindClass(kTypeKindClass);
const StaticString s_TypeKindEnum(kTypeKindEnum);
const StaticString s_TypeKindInterface(kTypeKindInterface);
const StaticString s_TypeKindTrait(kTypeKindTrait);
const StaticString s_TypeKindTypeAlias(kTypeKindTypeAlias);

/**
 * Return TypeKind as a string literal suitable for use in TypeDetails
 */
StaticString typeKindToString(TypeKind kind) {
  switch (kind) {
    case TypeKind::Class:
      return s_TypeKindClass;
    case TypeKind::Enum:
      return s_TypeKindEnum;
    case TypeKind::Interface:
      return s_TypeKindInterface;
    case TypeKind::Trait:
      return s_TypeKindTrait;
    case TypeKind::TypeAlias:
      return s_TypeKindTypeAlias;
    case TypeKind::Unknown:
      return empty_string_ref;
  }
  not_reached();
}

/**
 * Convert a C++ folly::dynamic structure into a Hack `vec<dynamic>`.
 */
template <typename DynamicIterable>
Array makeVecOfDynamic(DynamicIterable&& vector) {
  auto ret = VecInit{vector.size()};
  for (auto&& dy : std::forward<DynamicIterable>(vector)) {
    ret.append(tvFromDynamic(std::move(dy)));
  }
  return ret.toArray();
}

/**
 * Convert (method, attribute) pairs into a Hack `dict<string, vec<string>>`,
 * grouping attribute names by method name.
 */
Array makeDictOfStringVecString(
    const std::vector<
        std::pair<Symbol<SymKind::Method>, Symbol<SymKind::Type>>>& pairs) {
  std::map<std::string_view, std::vector<const StringData*>> grouped;
  std::map<std::string_view, const StringData*> methodKeys;
  for (auto const& [method, attr] : pairs) {
    auto sv = method.slice();
    grouped[sv].push_back(attr.get());
    methodKeys[sv] = method.get();
  }
  auto ret = DictInit{grouped.size()};
  for (auto const& [sv, attrs] : grouped) {
    auto attrVec = VecInit{attrs.size()};
    for (auto const* attr : attrs) {
      attrVec.append(VarNR{StrNR{attr}}.tv());
    }
    ret.set(StrNR{methodKeys[sv]}, attrVec.toArray());
  }
  return ret.toArray();
}

/**
 * Converts a vector of FileAttrVals (each of which is one path and one
 * attr val for a particular file attr) into a vector of two-item vectors
 * each of which will be represented as a hack dynamic.
 *
 * If there is no attr arg for this attr, the second value will be null.
 */
Array makeVecOfDynamicDynamic(std::vector<FileAttrVal>& vector) {
  auto ret = VecInit{vector.size()};
  for (auto&& [path, val] : vector) {
    auto dynDynTuple = VecInit{2};
    dynDynTuple.append(tvFromDynamic(*path.get()));
    if (val) {
      dynDynTuple.append(tvFromDynamic(std::move(*val)));
    } else {
      dynDynTuple.append(tvFromDynamic(folly::dynamic{}));
    }
    ret.append(dynDynTuple.toArray());
  }
  return ret.toArray();
}

/**
 * The actual AutoloadMap. Stores one SymbolMap for each kind of symbol.
 */
struct FactsStoreImpl final
    : public FactsStore,
      public std::enable_shared_from_this<FactsStoreImpl> {
  // when the filesystem can change, set up with a watcher
  FactsStoreImpl(
      fs::path root,
      AutoloadDB::Opener dbOpener,
      std::shared_ptr<Watcher> watcher,
      Optional<std::filesystem::path> suppressionFilePath,
      hphp_vector_set<Symbol<SymKind::Type>> indexedMethodAttributes,
      hphp_vector_set<Symbol<SymKind::Type>> indexedFunctionAttributes)
      : m_updateExec{std::make_unique<folly::CPUThreadPoolExecutor>(
            1,
            make_thread_factory("Autoload update"))},
        m_root{std::move(root)},
        m_symbolMap{
            m_root,
            std::move(dbOpener),
            std::move(indexedMethodAttributes),
            std::move(indexedFunctionAttributes),
            Cfg::Autoload::DBEnableBlockingDbWait,
            Cfg::Autoload::DBUseSymbolMapForGetFilesWithAttributeAndAnyVal,
            std::chrono::milliseconds(Cfg::Autoload::DBBlockingDbWaitTimeoutMs),
        },
        m_watcher{std::move(watcher)},
        m_suppressionFilePath{std::move(suppressionFilePath)} {}

  // when the filesystem is readonly and we have a trusted db
  FactsStoreImpl(
      fs::path root,
      AutoloadDB::Opener dbOpener,
      hphp_vector_set<Symbol<SymKind::Type>> indexedMethodAttributes,
      hphp_vector_set<Symbol<SymKind::Type>> indexedFunctionAttributes)
      : m_root{std::move(root)},
        m_symbolMap{
            m_root,
            std::move(dbOpener),
            std::move(indexedMethodAttributes),
            std::move(indexedFunctionAttributes),
            Cfg::Autoload::DBEnableBlockingDbWait,
            Cfg::Autoload::DBUseSymbolMapForGetFilesWithAttributeAndAnyVal,
            std::chrono::milliseconds(
                Cfg::Autoload::DBBlockingDbWaitTimeoutMs)} {}

  ~FactsStoreImpl() override {
    try {
      close();
    } catch (...) {
      // Can't throw an exception outside of a destructor
    }
  }

  FactsStoreImpl(const FactsStoreImpl&) = delete;
  FactsStoreImpl(FactsStoreImpl&&) noexcept = delete;
  FactsStoreImpl& operator=(const FactsStoreImpl&) = delete;
  FactsStoreImpl& operator=(FactsStoreImpl&&) noexcept = delete;

  void close() override {
    m_closing = true;
    auto updateFut = m_updateFuture.wlock();
    try {
      updateFut->getFuture().wait();
    } catch (...) {
      // folly::Future::wait() might throw a FutureInvalid exception, but we're
      // shutting down.
    }
    *updateFut = {};
    m_updateExec = {};
  }

  Holder getNativeHolder() noexcept override {
    return Holder{
        this, [sptr = shared_from_this()]() mutable { sptr.reset(); }};
  }

  /**
   * Return an object representing the last time this store was updated
   */
  Clock getClock() const {
    auto clock = m_symbolMap.getClock();
    return clock.isInitial() ? m_symbolMap.dbClock() : clock;
  }

  void validate(const std::set<std::string>& types_to_ignore) override {
    m_symbolMap.validate(types_to_ignore);
  }

  /**
   * Guarantee the map is at least as up-to-date as the codebase was
   * when update() was called.
   *
   * throws SQLiteExc or FactsStoreExc.
   */
  void ensureUpdated() override {
    TimeoutSuspender suspendTimeoutsDuringUpdate;

    folly::SemiFuture<folly::Unit> updateFuture = update();
    updateFuture.wait();
    auto const& res = std::move(updateFuture).result();
    if (res.hasException()) {
      res.throwUnlessValue();
    }
    if (Cfg::Eval::AutoloadEagerSyncUnitCache && m_watcher) {
      unitCacheSetSync();
    }
  }

  OptString getTypeName(const OptString& type) override {
    auto name = m_symbolMap.getTypeName(*type.get());
    if (!name) {
      return OptString{};
    } else {
      return OptString{const_cast<StringData*>(name->get())};
    }
  }

  Variant getKind(const OptString& type) override {
    auto const kind = m_symbolMap.getKind(Symbol<SymKind::Type>{*type.get()});
    auto const kindStr = typeKindToString(kind).get();

    if (kindStr == nullptr || kindStr->empty()) {
      return Variant{Variant::NullInit{}};
    } else {
      return Variant{kindStr, Variant::PersistentStrInit{}};
    }
  }

  bool isTypeAbstract(const OptString& type) override {
    return m_symbolMap.isTypeAbstract(*type.get());
  }

  bool isTypeFinal(const OptString& type) override {
    return m_symbolMap.isTypeFinal(*type.get());
  }

  Optional<AutoloadMap::FileResult> getTypeOrTypeAliasFile(
      const OptString& type) override {
    return getSymbolFile<SymKind::Type>(
        type, [](SymbolMap& m, Symbol<SymKind::Type> s) {
          return m.getTypeOrTypeAliasFile(s);
        });
  }

  Optional<AutoloadMap::FileResult> getTypeOrTypeAliasFileRelative(
      const OptString& type) override {
    return getSymbolFileRelative<SymKind::Type>(
        type, [](SymbolMap& m, Symbol<SymKind::Type> s) {
          return m.getTypeOrTypeAliasFile(s);
        });
  }

  Optional<AutoloadMap::FileResult> getTypeFile(
      const OptString& type) override {
    return getSymbolFile<SymKind::Type>(
        type,
        [](SymbolMap& m, Symbol<SymKind::Type> s) { return m.getTypeFile(s); });
  }

  Optional<AutoloadMap::FileResult> getTypeFileRelative(
      const OptString& type) override {
    return getSymbolFileRelative<SymKind::Type>(
        type,
        [](SymbolMap& m, Symbol<SymKind::Type> s) { return m.getTypeFile(s); });
  }

  Optional<AutoloadMap::FileResult> getFunctionFile(
      const OptString& function) override {
    return getSymbolFile<SymKind::Function>(
        function, [](SymbolMap& m, Symbol<SymKind::Function> s) {
          return m.getFunctionFile(s);
        });
  }

  Optional<AutoloadMap::FileResult> getFunctionFileRelative(
      const OptString& function) override {
    return getSymbolFileRelative<SymKind::Function>(
        function, [](SymbolMap& m, Symbol<SymKind::Function> s) {
          return m.getFunctionFile(s);
        });
  }

  Optional<AutoloadMap::FileResult> getConstantFile(
      const OptString& constant) override {
    return getSymbolFile<SymKind::Constant>(
        constant, [](SymbolMap& m, Symbol<SymKind::Constant> s) {
          return m.getConstantFile(s);
        });
  }

  Optional<AutoloadMap::FileResult> getConstantFileRelative(
      const OptString& constant) override {
    return getSymbolFileRelative<SymKind::Constant>(
        constant, [](SymbolMap& m, Symbol<SymKind::Constant> s) {
          return m.getConstantFile(s);
        });
  }

  Optional<AutoloadMap::FileResult> getTypeAliasFile(
      const OptString& typeAlias) override {
    return getSymbolFile<SymKind::Type>(
        typeAlias, [](SymbolMap& m, Symbol<SymKind::Type> s) {
          return m.getTypeAliasFile(s);
        });
  }

  Optional<AutoloadMap::FileResult> getTypeAliasFileRelative(
      const OptString& typeAlias) override {
    return getSymbolFileRelative<SymKind::Type>(
        typeAlias, [](SymbolMap& m, Symbol<SymKind::Type> s) {
          return m.getTypeAliasFile(s);
        });
  }

  Optional<AutoloadMap::FileResult> getModuleFile(
      const OptString& module) override {
    return getSymbolFile<SymKind::Module>(
        module, [](SymbolMap& m, Symbol<SymKind::Module> s) {
          return m.getModuleFile(s);
        });
  }

  Optional<AutoloadMap::FileResult> getModuleFileRelative(
      const OptString& module) override {
    return getSymbolFileRelative<SymKind::Module>(
        module, [](SymbolMap& m, Symbol<SymKind::Module> s) {
          return m.getModuleFile(s);
        });
  }

  Optional<fs::path> getTypeOrTypeAliasFile(std::string_view type) override {
    return getSymbolFile<SymKind::Type>(
        type, [](SymbolMap& m, Symbol<SymKind::Type> s) {
          return m.getTypeOrTypeAliasFile(s);
        });
  }

  Optional<fs::path> getTypeOrTypeAliasFileRelative(
      std::string_view type) override {
    return getSymbolFileRelative<SymKind::Type>(
        type, [](SymbolMap& m, Symbol<SymKind::Type> s) {
          return m.getTypeOrTypeAliasFile(s);
        });
  }

  Optional<fs::path> getTypeFile(std::string_view type) override {
    return getSymbolFile<SymKind::Type>(
        type,
        [](SymbolMap& m, Symbol<SymKind::Type> s) { return m.getTypeFile(s); });
  }

  Optional<fs::path> getTypeFileRelative(std::string_view type) override {
    return getSymbolFileRelative<SymKind::Type>(
        type,
        [](SymbolMap& m, Symbol<SymKind::Type> s) { return m.getTypeFile(s); });
  }

  Optional<fs::path> getFunctionFile(std::string_view func) override {
    return getSymbolFile<SymKind::Function>(
        func, [](SymbolMap& m, Symbol<SymKind::Function> s) {
          return m.getFunctionFile(s);
        });
  }

  Optional<fs::path> getFunctionFileRelative(std::string_view func) override {
    return getSymbolFileRelative<SymKind::Function>(
        func, [](SymbolMap& m, Symbol<SymKind::Function> s) {
          return m.getFunctionFile(s);
        });
  }

  Optional<fs::path> getConstantFile(std::string_view name) override {
    return getSymbolFile<SymKind::Constant>(
        name, [](SymbolMap& m, Symbol<SymKind::Constant> s) {
          return m.getConstantFile(s);
        });
  }

  Optional<fs::path> getConstantFileRelative(std::string_view name) override {
    return getSymbolFileRelative<SymKind::Constant>(
        name, [](SymbolMap& m, Symbol<SymKind::Constant> s) {
          return m.getConstantFile(s);
        });
  }

  Optional<fs::path> getTypeAliasFile(std::string_view name) override {
    return getSymbolFile<SymKind::Type>(
        name, [](SymbolMap& m, Symbol<SymKind::Type> s) {
          return m.getTypeAliasFile(s);
        });
  }

  Optional<fs::path> getTypeAliasFileRelative(std::string_view name) override {
    return getSymbolFileRelative<SymKind::Type>(
        name, [](SymbolMap& m, Symbol<SymKind::Type> s) {
          return m.getTypeAliasFile(s);
        });
  }

  Optional<fs::path> getModuleFile(std::string_view name) override {
    return getSymbolFile<SymKind::Module>(
        name, [](SymbolMap& m, Symbol<SymKind::Module> s) {
          return m.getModuleFile(s);
        });
  }

  Optional<fs::path> getModuleFileRelative(std::string_view name) override {
    return getSymbolFileRelative<SymKind::Module>(
        name, [](SymbolMap& m, Symbol<SymKind::Module> s) {
          return m.getModuleFile(s);
        });
  }

  Array getFileTypes(const OptString& path) override {
    return getFileSymbols<SymKind::Type>(
        path, [](SymbolMap& m, Path s) { return m.getFileTypes(s); });
  }

  Array getFileFunctions(const OptString& path) override {
    return getFileSymbols<SymKind::Function>(
        path, [](SymbolMap& m, Path s) { return m.getFileFunctions(s); });
  }

  Array getFileConstants(const OptString& path) override {
    return getFileSymbols<SymKind::Constant>(
        path, [](SymbolMap& m, Path s) { return m.getFileConstants(s); });
  }

  Array getFileTypeAliases(const OptString& path) override {
    return getFileSymbols<SymKind::Type>(
        path, [](SymbolMap& m, Path s) { return m.getFileTypeAliases(s); });
  }

  Array getFileModules(const OptString& path) override {
    return getFileSymbols<SymKind::Module>(
        path, [](SymbolMap& m, Path s) { return m.getFileModules(s); });
  }

  Array getAllModules() override {
    auto modules = m_symbolMap.getAllModules();
    VecInit symbols{modules.size()};
    for (auto const& module : modules) {
      symbols.append(make_tv<KindOfPersistentString>(module.get()));
    }
    return symbols.toArray();
  }

  Optional<OptString> getFileModuleMembership(const OptString& path) override {
    auto relativePath = ensureRelativePath(path, m_root);
    if (!relativePath.has_value()) {
      return std::nullopt;
    }
    auto result = m_symbolMap.getFileModuleMembership(*relativePath);
    if (result.has_value()) {
      return StrNR{result->get()};
    } else {
      return std::nullopt;
    }
  }

  Optional<OptString> getFilePackageMembership(const OptString& path) override {
    auto relativePath = ensureRelativePath(path, m_root);
    if (!relativePath.has_value()) {
      return std::nullopt;
    }

    // Check if there's a package override first - this is actually the only
    // package information we store in Facts.
    auto result = m_symbolMap.getFilePackageMembership(*relativePath);
    if (result.has_value()) {
      return StrNR{result->get()};
    }

    // If there's no package override, first make sure the file even exists,
    // then use the package configuration to determine its membership.
    if (!m_symbolMap.getFileExists(relativePath.value())) {
      return std::nullopt;
    }

    auto requestOptions = g_context->getRepoOptionsForRequest();
    if (!requestOptions) {
      return std::nullopt;
    }

    auto const package = requestOptions->packageInfo().pathToPackageName(
        relativePath.value().slice());
    if (!package)
      return std::nullopt;
    return OptString{*package};
  }

  Array getBaseTypes(const OptString& derivedType, const Variant& filters)
      override {
    return filterBaseTypes(
        derivedType,
        TypeFilterData::createFromShape(
            filters.isArray() ? filters.getArrayData() : nullptr));
  }

  Array getDerivedTypes(const OptString& baseType, const Variant& filters)
      override {
    return filterDerivedTypes(
        baseType,
        TypeFilterData::createFromShape(
            filters.isArray() ? filters.getArrayData() : nullptr));
  }

  Array getTransitiveDerivedTypes(
      const OptString& baseType,
      const Variant& filters,
      bool includeInterfaceRequireExtends) override {
    return filterTransitiveDerivedTypes(
        baseType,
        includeInterfaceRequireExtends,
        TypeFilterData::createFromShape(
            filters.isArray() ? filters.getArrayData() : nullptr));
  }

  Array getTypesWithAttribute(const OptString& attr) override {
    return makeVecOfLazyClass(m_symbolMap.getTypesWithAttribute(*attr.get()));
  }

  Array getTypeAliasesWithAttribute(const OptString& attr) override {
    return makeVecOfString(
        m_symbolMap.getTypeAliasesWithAttribute(*attr.get()));
  }

  Array getMethodsWithAttribute(const OptString& attr) override {
    if (UNLIKELY(!m_symbolMap.isAttrIndexed(*attr.get()))) {
      HPHP::SystemLib::throwRuntimeExceptionObject(
          fmt::format(
              "Queried attribute {} not found in IndexedMethodAttributes={}",
              attr.get()->data(),
              m_symbolMap.debugIndexedAttrs()));
    }
    return makeVecOfStringString(
        m_symbolMap.getMethodsWithAttribute(*attr.get()));
  }

  Array getFunctionsWithAttribute(const OptString& attr) override {
    if (UNLIKELY(!m_symbolMap.isFunctionAttrIndexed(*attr.get()))) {
      HPHP::SystemLib::throwRuntimeExceptionObject(
          fmt::format(
              "Queried attribute {} not found in IndexedFunctionAttributes={}",
              attr.get()->data(),
              m_symbolMap.debugIndexedFunctionAttrs()));
    }
    return makeVecOfFunctionString(
        m_symbolMap.getFunctionsWithAttribute(*attr.get()));
  }

  Array getTypeMethodAttributes(const OptString& type) override {
    return makeDictOfStringVecString(
        m_symbolMap.getTypeMethodAttributes(*type.get()));
  }

  Array getFilesWithAttribute(const OptString& attr) override {
    return makeVecOfString(m_symbolMap.getFilesWithAttribute(*attr.get()));
  }

  Array getFilesWithAttributeAndAnyValue(
      const OptString& attr,
      const folly::dynamic& value) override {
    return makeVecOfString(
        m_symbolMap.getFilesWithAttributeAndAnyValue(*attr.get(), value));
  }
  /**
   * Given a the name of a file attribute, returns an array of two item
   * arrays of hack dynamics. The first item in each array is  well typed
   * string but made dynamic so the return vector has a single type.
   * The second arg has strange behavior.
   * It is a folly::dynamic in the symbol map, and a string in the db but we use
   * parseJson on the way out.  So I originally expected values that were
   * ints in the original file attribute to come back as ints. But they do
   * come out as strings.  This behavior is the same with getFilesAttrArgs,
   * so this is not a new bug with this method.  Because of this, we
   * actually represent the output as a vec<(string, ?string)> in hack.
   * The second item is nullable because not every attr has values.
   */
  Array getFilesAndAttrValsWithAttribute(const OptString& attr) override {
    auto toConvert = m_symbolMap.getFilesAndAttrValsWithAttribute(*attr.get());
    return makeVecOfDynamicDynamic(toConvert);
  }

  Array getTypeAttributes(const OptString& type) override {
    return makeVecOfString(m_symbolMap.getAttributesOfType(*type.get()));
  }

  Array getTypeAliasAttributes(const OptString& typeAlias) override {
    return makeVecOfString(
        m_symbolMap.getAttributesOfTypeAlias(*typeAlias.get()));
  }

  Array getMethodAttributes(const OptString& type, const OptString& method)
      override {
    return makeVecOfString(
        m_symbolMap.getAttributesOfMethod(*type.get(), *method.get()));
  }

  Array getFunctionAttributes(const OptString& function) override {
    return makeVecOfString(
        m_symbolMap.getAttributesOfFunction(*function.get()));
  }

  Array getFileAttributes(const OptString& file) override {
    return makeVecOfString(m_symbolMap.getAttributesOfFile(Path{*file.get()}));
  }

  Array getTypeAttrArgs(const OptString& type, const OptString& attribute)
      override {
    return makeVecOfDynamic(
        m_symbolMap.getTypeAttributeArgs(*type.get(), *attribute.get()));
  }

  Array getTypeAliasAttrArgs(
      const OptString& typeAlias,
      const OptString& attribute) override {
    return makeVecOfDynamic(m_symbolMap.getTypeAliasAttributeArgs(
        *typeAlias.get(), *attribute.get()));
  }

  Array getMethodAttrArgs(
      const OptString& type,
      const OptString& method,
      const OptString& attribute) override {
    return makeVecOfDynamic(m_symbolMap.getMethodAttributeArgs(
        *type.get(), *method.get(), *attribute.get()));
  }

  Array getFunctionAttrArgs(
      const OptString& function,
      const OptString& attribute) override {
    return makeVecOfDynamic(m_symbolMap.getFunctionAttributeArgs(
        *function.get(), *attribute.get()));
  }

  Array getFileAttrArgs(const OptString& file, const OptString& attribute)
      override {
    return makeVecOfDynamic(
        m_symbolMap.getFileAttributeArgs(Path{*file.get()}, *attribute.get()));
  }

  Optional<std::string> getSha1(const OptString& path) {
    return m_symbolMap.getSha1(Path{*path.get()});
  }

  /**
   * Update whenever a file in the filesystem changes.
   */
  void subscribe() {
    if (!m_watcher) {
      return;
    }
    auto clock = getClock();
    m_watcher->subscribe(
        clock, [weakThis = weak_from_this()](Watcher::Delta&& delta) {
          XLOGF(
              INFO,
              "Subscription result: {} paths received.",
              delta.m_files.size());
          auto sharedThis = weakThis.lock();
          if (!sharedThis) {
            rareSboxEvent("facts-store", "subscribe weakThis==null", "");
            return;
          }
          try {
            timeSboxEvent(
                Cfg::Autoload::PerfSampleRate,
                "facts-store",
                "subscribe update",
                "",
                [&] { sharedThis->update(); });
          } catch (const UpdateExc& e) {
            // The store closed after watchman sent this notification. Don't
            // let that stop the notification from reaching the other stores
            // that share this subscription.
            XLOGF(INFO, "Ignoring subscription result: {}", e.what());
          }
        });
  }

  /**
   * Query our Watcher to see changed files, update our internal data
   * structures, and resolve the returned future once the map is at least as
   * up-to-date as the time update() was called.
   */
  folly::SemiFuture<folly::Unit> update() {
    if (!m_watcher) {
      return {};
    }
    if (m_closing) {
      throw UpdateExc{
          "Attempted to call FactsStoreImpl::update() while shutting down"};
    }
    // Check if a suppression sentinel file exists
    if (m_suppressionFilePath) {
      auto suppressionFilePath = *m_suppressionFilePath;
      if (suppressionFilePath.is_relative()) {
        suppressionFilePath = m_root / std::move(suppressionFilePath);
      }
      if (std::filesystem::exists(suppressionFilePath)) {
        XLOGF(
            INFO,
            "Suppression file found at {}, not updating native Facts.",
            suppressionFilePath.native());
        rareSboxEvent(
            "facts-store", "update supressed", suppressionFilePath.native());
        return {};
      }
    }
    auto updateFuture = [&] {
      tracing::Block _{"autoload-update-acquire-lock"};
      return m_updateFuture.wlock();
    }();
    auto updateStart = std::chrono::steady_clock::now();
    // We need at least one Watchman query to start and complete after
    // `update()` is called. We model our updater and Watchman querier as a
    // chain of futures, which can be thought of as a queue. If we have
    // multiple simultaneous updates, then there is already a Watchman query
    // which hasn't started yet. In that case, we don't need to schedule
    // another Watchman query. One of the pending queries will start and
    // finish after this call to update().
    if (!m_queryingWatchman) {
      m_queryingWatchman = true;
      *updateFuture = folly::splitFuture(
          updateFuture->getFuture()
              .via(m_updateExec.get())
              .thenTry([this](const folly::Try<folly::Unit>&) {
                m_queryingWatchman = false;
                m_lastWatchmanQueryStart = std::chrono::steady_clock::now();
                return m_watcher->getChanges(getClock());
              })
              .thenTry(
                  [this](folly::Try<Watcher::Delta>&& delta)
                      -> folly::SemiFuture<Watcher::Delta> {
                    // On watchman failure or fresh instance, check
                    // whether an external tool updated the DB behind
                    // our back. If the DB clock differs from our
                    // in-memory clock, reset all in-memory state and
                    // re-query watchman with the DB clock.
                    auto inMemClock = m_symbolMap.getClock();
                    bool shouldValidate = !inMemClock.isInitial() &&
                        (delta.hasException() || delta.value().m_fresh);
                    if (shouldValidate) {
                      try {
                        // Drain any in-flight async DB writes so dbClock()
                        // reflects the authoritative on-disk state. Without
                        // this, a pending writeback can leave dbClock stale
                        // and trigger a spurious reset.
                        m_symbolMap.waitForDBUpdate();
                        auto dbClock = m_symbolMap.dbClock();
                        if (dbClock != inMemClock) {
                          XLOGF(
                              INFO,
                              "Stale in-memory state detected. "
                              "DB clock differs from in-memory "
                              "clock (in-memory: {}, DB: {}). "
                              "Resetting and re-querying.",
                              inMemClock,
                              dbClock);
                          m_symbolMap.resetInMemoryState();
                          return m_watcher->getChanges(dbClock);
                        }
                      } catch (const std::exception& e) {
                        XLOGF(
                            WARN,
                            "Failed to read DB clock for stale "
                            "detection: {}",
                            e.what());
                      }
                    }
                    delta.throwUnlessValue();
                    return folly::makeSemiFuture(std::move(delta.value()));
                  })
              .thenTry([this](folly::Try<Watcher::Delta>&& delta) {
                if (delta.hasException()) {
                  auto msg = fmt::format(
                      "Exception while querying watcher: {}",
                      delta.exception().what());
                  XLOG(ERR) << msg;
                  rareSboxEvent("facts-store", "update watcher exception", msg);
                  throw UpdateExc{msg};
                }
                return updateWithDelta(std::move(delta.value()));
              }));
    }
    return updateFuture->getFuture()
        .thenTry([this, updateStart](folly::Try<folly::Unit> res) {
          // Verify that we have kicked off at least one Watchman query between
          // this call to update() and the future chain's completion. We always
          // log if this invariant is violated, and we also assert when we're in
          // debug mode.
          auto lastWatchmanQueryStart = m_lastWatchmanQueryStart.load();
          if (UNLIKELY(lastWatchmanQueryStart < updateStart)) {
            auto currentTime = std::chrono::steady_clock::now();
            auto msg = fmt::format(
                "Stale update. Last Watchman query was {}ms ago, but update was only {}ms ago.",
                std::chrono::duration_cast<
                    std::chrono::duration<double, std::milli>>(
                    currentTime - lastWatchmanQueryStart)
                    .count(),
                std::chrono::duration_cast<
                    std::chrono::duration<double, std::milli>>(
                    currentTime - updateStart)
                    .count());
            XLOG(ERR, msg);
            rareSboxEvent("facts-store", msg, "");
          }
          assertx(updateStart < lastWatchmanQueryStart);
          auto queryMicros = std::chrono::duration_cast<std::chrono::duration<
              double,
              std::chrono::microseconds::period>>(
                                 lastWatchmanQueryStart - updateStart)
                                 .count();
          sampleSboxEvent(
              Cfg::Autoload::PerfSampleRate,
              "facts-store",
              "watchman query",
              m_root.native(),
              queryMicros,
              std::nullopt);
          XLOGF(
              DBG0,
              "update waited {}ms for a Watchman query to return.",
              queryMicros / 1000);
          return res;
        })
        .semi();
  }

  folly::SemiFuture<folly::Unit> updateWithDelta(Watcher::Delta&& delta) {
    if (m_closing) {
      throw UpdateExc{"Shutting down"};
    }

    tracing::Block _{"autoload-update-parse-results", [&] {
                       return tracing::Props{}
                           .add("fresh", delta.m_fresh)
                           .add("delta_size", delta.m_files.size());
                     }};
    bool isFresh = delta.m_fresh;
    Clock lastClock = *delta.m_lastClock;
    Clock newClock = delta.m_newClock;

    auto [alteredPathsAndHashes, deletedPaths] = isFresh
        ? getFreshDelta(std::move(delta))
        : getIncrementalDelta(std::move(delta));

    if (Cfg::Eval::AutoloadEagerSyncUnitCache) {
      unitCacheSyncRepo(this, m_root, alteredPathsAndHashes, deletedPaths);
    }

    // An empty result can still advance the clock past files excluded by the
    // Watchman query, so skip the update only when the clock is unchanged.
    //
    // Note that we intentionally still perform a no-op update if the watchman
    // clock has not been stored in the symbol map yet as queries using the
    // merge base can be more expensive than clock based queries.
    if (!isFresh && alteredPathsAndHashes.empty() && deletedPaths.empty() &&
        lastClock == newClock && !lastClock.m_clock.empty()) {
      XLOG(INFO) << "Finished: it's a no-op incremental update.";
      return {};
    }

    XLOGF(
        INFO,
        "{} update: {} paths altered, {} paths deleted.",
        isFresh ? "Fresh" : "Incremental",
        alteredPathsAndHashes.size(),
        deletedPaths.size());

    auto alteredPathFacts = LIKELY(alteredPathsAndHashes.empty())
        ? std::vector<folly::Try<FileFacts>>{}
        : facts_from_paths(m_root, alteredPathsAndHashes);

    std::vector<FileFacts> facts;
    facts.reserve(alteredPathFacts.size());
    for (auto i = 0; i < alteredPathFacts.size(); i++) {
      try {
        facts.push_back(std::move(alteredPathFacts[i]).value());
      } catch (FactsExtractionExc& e) {
        XLOGF(
            CRITICAL,
            "Error extracting facts from {}: {}",
            alteredPathsAndHashes.at(i).m_path.c_str(),
            e.what());
        // Treat a parse error as if we had deleted the path
        deletedPaths.push_back(std::move(alteredPathsAndHashes.at(i).m_path));
        alteredPathsAndHashes.at(i) = {fs::path{}, {}};
      }
    }

    // Compact empty paths out of alteredPathsAndHashes
    if (facts.size() < alteredPathsAndHashes.size()) {
      alteredPathsAndHashes.erase(
          std::remove_if(
              alteredPathsAndHashes.begin(),
              alteredPathsAndHashes.end(),
              [](const PathAndOptionalHash& pathAndHash) {
                return pathAndHash.m_path.empty();
              }),
          alteredPathsAndHashes.end());
    }

    XLOGF(
        INFO,
        "Facts size: {}. Altered paths size: {}",
        facts.size(),
        alteredPathsAndHashes.size());
    assertx(facts.size() == alteredPathsAndHashes.size());

    // Check for files with the SHA1 hash corresponding to an empty
    // file, but with nonempty facts
    for (auto i = 0; i < alteredPathsAndHashes.size(); ++i) {
      auto const& fileFacts = facts.at(i);
      // We cannot trust pathAndHash.m_hash, the authoritative hash for these
      // facts is fileFacts.m_sha1hex which was computed from the file contents
      // that were used to derive these facts.
      //
      // If a race occurs between watchman notifying us of changes to disk and
      // further writes occurring the hash we have from watchman may be stale.
      // The hash written to memcache and used to key the in memory facts map
      // will be fileFacts.m_sha1hex.
      if (as_slice(fileFacts.sha1sum) == kEmptyFileSha1Hash &&
          !isEmpty(fileFacts)) {
        throw FactsExtractionExc{fmt::format(
            "{} has a SHA1 hash corresponding to an empty file ('{}'), "
            "but we are attempting to assign it non-empty facts: `{}`",
            alteredPathsAndHashes[i].m_path.native(),
            kEmptyFileSha1Hash,
            as_slice(hackc::facts_debug(fileFacts)))};
      }
    }

    XLOGF(
        INFO,
        "SymbolMap.update(since={}, clock={}, "
        "alteredPathsAndHashes.size()={}, deletedPaths.size()={})",
        lastClock,
        newClock,
        alteredPathsAndHashes.size(),
        deletedPaths.size());
    m_symbolMap.update(
        lastClock,
        newClock,
        // See comment above for why we no longer trust the hashes from this set
        // of paths.
        removeHashes(std::move(alteredPathsAndHashes)),
        std::move(deletedPaths),
        std::move(facts));

    XLOGF(INFO, "Thread {} has finished updating.", curthread());
    return {};
  }

  /**
   * Calculate the paths which have changed since the last Watchman
   * update when Watchman gives us the full state of the world.
   */

  std::tuple<std::vector<PathAndOptionalHash>, std::vector<fs::path>>
  getFreshDelta(Watcher::Delta&& delta) const {
    auto allPaths = m_symbolMap.getAllPathsWithHashes();
    XLOGF(
        INFO,
        "Received fresh query, checking against our list of {} paths.",
        allPaths.size());

    std::vector<PathAndOptionalHash> alteredPaths;
    alteredPaths.reserve(delta.m_files.size());
    for (auto& pathData : std::move(delta.m_files)) {
      assertx(pathData.m_exists);

      auto& path = pathData.m_path;
      auto sha1hex = pathData.m_watcher_hash;

      // Watchman is sending us all the files in the repo, regardless of
      // whether we've seen the same file before. Watchman is also
      // sending us SHA1 hashes, so compare these to determine which
      // files have actually changed.
      bool sha1HexMatches = [&]() {
        if (!sha1hex.has_value()) {
          return false;
        }
        auto const it = allPaths.find(path.native());
        if (it == allPaths.end()) {
          return false;
        }
        return SHA1{*sha1hex} == it->second;
      }();

      // Remove the path from the map to mark that we've seen
      // it. Watchman doesn't know about files which have been deleted
      // while it was down, so we have to figure out which files were
      // deleted ourselves.
      allPaths.erase(path.native());

      if (sha1HexMatches) {
        continue;
      }

      alteredPaths.push_back({std::move(path), std::move(sha1hex)});
    }

    // All remaining paths are ones which Watchman didn't see. These
    // have been deleted.
    std::vector<fs::path> deletedPaths;
    deletedPaths.reserve(allPaths.size());
    for (auto const& [pathStr, _] : allPaths) {
      deletedPaths.emplace_back(pathStr);
    }

    return {std::move(alteredPaths), std::move(deletedPaths)};
  }

  /**
   * Calculate the paths which have changed since the last Watchman
   * update when Watchman gives us an incremental update.
   */
  std::tuple<std::vector<PathAndOptionalHash>, std::vector<fs::path>>
  getIncrementalDelta(Watcher::Delta&& delta) const {
    std::vector<PathAndOptionalHash> alteredPaths;
    std::vector<fs::path> deletedPaths;
    for (auto& pathData : delta.m_files) {
      if (!pathData.m_exists) {
        deletedPaths.push_back(std::move(pathData.m_path));
      } else {
        alteredPaths.push_back(
            {std::move(pathData.m_path), std::move(pathData.m_watcher_hash)});
      }
    }
    return {std::move(alteredPaths), std::move(deletedPaths)};
  }

  Array filterBaseTypes(
      const OptString& derivedType,
      const TypeFilterData& filters) {
    std::vector<Symbol<SymKind::Type>> baseTypes;

    auto addBaseTypes = [&](DeriveKind kind) {
      auto newBaseTypes = m_symbolMap.getBaseTypes(
          Symbol<SymKind::Type>{*derivedType.get()}, kind);
      baseTypes.reserve(baseTypes.size() + newBaseTypes.size());
      std::move(
          std::begin(newBaseTypes),
          std::end(newBaseTypes),
          std::back_inserter(baseTypes));
    };

    if (!filters.m_deriveKindFilters.m_removeExtends) {
      addBaseTypes(DeriveKind::Extends);
    }
    if (!filters.m_deriveKindFilters.m_removeRequires) {
      addBaseTypes(DeriveKind::RequireThisAs);
      addBaseTypes(DeriveKind::RequireClass);
      addBaseTypes(DeriveKind::RequireExtends);
      addBaseTypes(DeriveKind::RequireImplements);
    }

    return makeVecOfLazyClass(filterByFlags(
        filterByKind(std::move(baseTypes), filters.m_kindFilters),
        filters.m_typeFlagFilters));
  }

  Array filterDerivedTypes(
      const OptString& baseType,
      const TypeFilterData& filters) {
    std::vector<Symbol<SymKind::Type>> derivedTypes;

    auto addDerivedTypes = [&](DeriveKind kind) {
      auto newDerivedTypes = m_symbolMap.getDerivedTypes(
          Symbol<SymKind::Type>{*baseType.get()}, kind);
      derivedTypes.reserve(derivedTypes.size() + newDerivedTypes.size());
      std::move(
          std::begin(newDerivedTypes),
          std::end(newDerivedTypes),
          std::back_inserter(derivedTypes));
    };

    if (!filters.m_deriveKindFilters.m_removeExtends) {
      addDerivedTypes(DeriveKind::Extends);
    }
    if (!filters.m_deriveKindFilters.m_removeRequires) {
      addDerivedTypes(DeriveKind::RequireThisAs);
      addDerivedTypes(DeriveKind::RequireClass);
      addDerivedTypes(DeriveKind::RequireExtends);
      addDerivedTypes(DeriveKind::RequireImplements);
    }

    return makeVecOfLazyClass(filterByFlags(
        filterByKind(std::move(derivedTypes), filters.m_kindFilters),
        filters.m_typeFlagFilters));
  }

  Array filterTransitiveDerivedTypes(
      const OptString& baseType,
      bool includeInterfaceRequireExtends,
      const TypeFilterData& filters) {
    auto derivedTypes = m_symbolMap.getTransitiveDerivedTypes(
        Symbol<SymKind::Type>{*baseType.get()}, includeInterfaceRequireExtends);

    return makeVecOfLazyClass(filterByFlags(
        filterByKind(std::move(derivedTypes), filters.m_kindFilters),
        filters.m_typeFlagFilters));
  }

  std::atomic<std::chrono::steady_clock::time_point> m_lastWatchmanQueryStart{
      std::chrono::steady_clock::now()};
  std::atomic<bool> m_queryingWatchman{false};
  std::unique_ptr<folly::CPUThreadPoolExecutor> m_updateExec;

  folly::Synchronized<folly::FutureSplitter<folly::Unit>> m_updateFuture{
      folly::splitFuture(folly::makeFuture())};
  std::atomic<bool> m_closing{false};
  fs::path m_root;
  SymbolMap m_symbolMap;
  std::shared_ptr<Watcher> m_watcher;
  Optional<std::filesystem::path> m_suppressionFilePath;

  template <SymKind k, typename TLambda>
  Array getFileSymbols(const OptString& path, TLambda lambda) {
    auto relativePath = ensureRelativePath(path, m_root);
    if (!relativePath) {
      return Array::CreateVec();
    }

    auto symbols = lambda(m_symbolMap, *relativePath);
    VecInit ret{symbols.size()};
    for (auto s : std::move(symbols)) {
      ret.append(make_tv<KindOfPersistentString>(s.get()));
    }
    return ret.toArray();
  }

  template <SymKind K, class T>
  Optional<AutoloadMap::FileResult> getSymbolFile(
      const OptString& symbol,
      T lambda) {
    auto path = getSymbolFile<K>(std::string_view{symbol.slice()}, lambda);
    if (UNLIKELY(!path)) {
      return {};
    }
    return AutoloadMap::FileResult(OptString{path->native()});
  }

  template <SymKind K, class T>
  Optional<fs::path> getSymbolFile(std::string_view symbol, T lambda) {
    const StringData* fileStr = lambda(m_symbolMap, Symbol<K>{symbol}).get();
    if (UNLIKELY(!fileStr))
      return std::nullopt;
    fs::path p{fileStr->toCppString()};
    assertx(p.is_relative());
    return m_root / p;
  }

  template <SymKind K, class T>
  Optional<AutoloadMap::FileResult> getSymbolFileRelative(
      const OptString& symbol,
      T lambda) {
    const StringData* fileStr =
        lambda(m_symbolMap, Symbol<K>{std::string_view{symbol.slice()}}).get();
    if (UNLIKELY(!fileStr)) {
      return {};
    }
    assertx(fileStr->isStatic());
    assertx(fileStr->data()[0] != '/');
    return AutoloadMap::FileResult(StrNR{fileStr});
  }

  template <SymKind K, class T>
  Optional<fs::path> getSymbolFileRelative(std::string_view symbol, T lambda) {
    const StringData* fileStr = lambda(m_symbolMap, Symbol<K>{symbol}).get();
    if (UNLIKELY(!fileStr))
      return std::nullopt;
    fs::path p{fileStr->toCppString()};
    assertx(p.is_relative());
    return p;
  }

  /**
   * Filter the given `types` down to only those of the given `kinds`, such as
   * class, interface, enum, or trait.
   *
   * The Hack type of `kinds` is `keyset<HH\Facts\TypeKind>`.
   */
  std::vector<Symbol<SymKind::Type>> filterByKind(
      std::vector<Symbol<SymKind::Type>> types,
      const KindFilterData& kinds) {
    // Bail out if we aren't filtering on kind
    if (kinds.doesIncludeEverything()) {
      return types;
    }
    types.erase(
        std::remove_if(
            types.begin(),
            types.end(),
            [&](const Symbol<SymKind::Type>& type) {
              auto kind = m_symbolMap.getKind(type);
              switch (kind) {
                case Facts::TypeKind::Class:
                  return kinds.m_removeClasses;
                case Facts::TypeKind::Enum:
                  return kinds.m_removeEnums;
                case Facts::TypeKind::Interface:
                  return kinds.m_removeInterfaces;
                case Facts::TypeKind::Trait:
                  return kinds.m_removeTraits;
                default:
                  return false;
              }
            }),
        types.end());
    return types;
  }
  /**
   * Filter down to the just the type flags that we want, such as final,
   * abstract, or empty. This is probably less performant than
   * if we rolled this logic into filterByKind, but it is easier to follow
   * this way, as two consecutive filters, rather than one complex one.
   */
  std::vector<Symbol<SymKind::Type>> filterByFlags(
      std::vector<Symbol<SymKind::Type>> types,
      const TypeFlagFilterData& flags) {
    // If returning everything, no need to iterate
    if (flags == TypeFlagFilterData::includeEverything()) {
      return types;
    }
    types.erase(
        std::remove_if(
            types.begin(),
            types.end(),
            [&](const Symbol<SymKind::Type>& type) {
              bool isFinal = m_symbolMap.isTypeFinal(type);
              bool isAbstract = m_symbolMap.isTypeAbstract(type);
              if (flags.m_removeEmpty && (!isFinal && !isAbstract)) {
                return true;
              }
              if (isFinal && flags.m_removeFinal) {
                return true;
              }
              if (isAbstract && flags.m_removeAbstract) {
                return true;
              }
              return false;
            }),
        types.end());
    return types;
  }
};

} // namespace

std::shared_ptr<FactsStore> make_watcher_facts(
    fs::path root,
    AutoloadDB::Opener dbOpener,
    std::shared_ptr<Watcher> watcher,
    bool shouldSubscribe,
    Optional<std::filesystem::path> suppressionFilePath,
    const std::vector<std::string>& indexedMethodAttrsVec,
    const std::vector<std::string>& indexedFunctionAttrsVec) {
  hphp_vector_set<Symbol<SymKind::Type>> indexedMethodAttrs;
  indexedMethodAttrs.reserve(indexedMethodAttrsVec.size());
  for (auto& v : indexedMethodAttrsVec) {
    indexedMethodAttrs.insert(Symbol<SymKind::Type>{std::move(v)});
  }
  hphp_vector_set<Symbol<SymKind::Type>> indexedFunctionAttrs;
  indexedFunctionAttrs.reserve(indexedFunctionAttrsVec.size());
  for (auto& v : indexedFunctionAttrsVec) {
    indexedFunctionAttrs.insert(Symbol<SymKind::Type>{std::move(v)});
  }
  auto inner = std::make_shared<FactsStoreImpl>(
      std::move(root),
      std::move(dbOpener),
      std::move(watcher),
      std::move(suppressionFilePath),
      std::move(indexedMethodAttrs),
      std::move(indexedFunctionAttrs));
  if (shouldSubscribe) {
    inner->subscribe();
  }
  return FactsLogger::wrap(
      std::move(inner),
      "ext_facts watcher",
      Cfg::Autoload::PerfSampleRate,
      Cfg::Autoload::ErrorSampleRate);
}

std::shared_ptr<FactsStore> make_trusted_facts(
    fs::path root,
    AutoloadDB::Opener dbOpener,
    const std::vector<std::string>& indexedMethodAttrsVec,
    const std::vector<std::string>& indexedFunctionAttrsVec) {
  hphp_vector_set<Symbol<SymKind::Type>> indexedMethodAttrs;
  indexedMethodAttrs.reserve(indexedMethodAttrsVec.size());
  for (auto& v : indexedMethodAttrsVec) {
    indexedMethodAttrs.insert(Symbol<SymKind::Type>{std::move(v)});
  }
  hphp_vector_set<Symbol<SymKind::Type>> indexedFunctionAttrs;
  indexedFunctionAttrs.reserve(indexedFunctionAttrsVec.size());
  for (auto& v : indexedFunctionAttrsVec) {
    indexedFunctionAttrs.insert(Symbol<SymKind::Type>{std::move(v)});
  }
  auto inner = std::make_unique<FactsStoreImpl>(
      std::move(root),
      std::move(dbOpener),
      std::move(indexedMethodAttrs),
      std::move(indexedFunctionAttrs));
  return FactsLogger::wrap(
      std::move(inner),
      "ext_facts trusted",
      Cfg::Autoload::PerfSampleRate,
      Cfg::Autoload::ErrorSampleRate);
}

} // namespace Facts
} // namespace HPHP
