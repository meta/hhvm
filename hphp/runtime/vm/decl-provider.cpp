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

#include "hphp/runtime/base/autoload-handler.h"
#include "hphp/runtime/base/zend-string.h"
#include "hphp/runtime/vm/builtin-symbol-map.h"
#include "hphp/runtime/vm/decl-provider.h"

#include "hphp/util/configs/eval.h"
#include "hphp/util/sha1.h"

#include <boost/algorithm/string/predicate.hpp>

#include <fstream>

namespace HPHP {

TRACE_SET_MOD(unit_parse)

std::unique_ptr<HhvmDeclProvider>
HhvmDeclProvider::create(AutoloadMap* map,
                         const RepoOptionsFlags& options,
                         const std::filesystem::path& repoRoot) {
  if (!Cfg::Eval::EnableDecl) {
    return {nullptr};
  }
  if (!map) {
    // Compiling systemlib.php with no autoload map initialized yet.
    return {nullptr};
  }

  // Create the DeclProvider.
  // This will only cache entries it sees during this invocation of
  // the compiler. Hackc should have a per-session cache indexed by symbol,
  // and this file-to-decls cache should span sessions.
  hackc::DeclParserConfig decl_config;
  options.initDeclConfig(decl_config);
  return std::make_unique<HhvmDeclProvider>(decl_config, map, repoRoot);
}

HhvmDeclProvider::HhvmDeclProvider(
    hackc::DeclParserConfig decl_config,
    AutoloadMap* map,
    const std::filesystem::path& repo
) : m_config{decl_config}, m_map{map}, m_repo{repo}
{}

// Called by hackc.

hackc::ExternalDeclProviderResult HhvmDeclProvider::getType(
  std::string_view symbol,
  uint64_t depth
) noexcept {
  return getDecls(symbol, depth, AutoloadMap::KindOf::TypeOrTypeAlias);
}

hackc::ExternalDeclProviderResult HhvmDeclProvider::getFunc(
  std::string_view symbol
) noexcept {
  return getDecls(symbol, 0, AutoloadMap::KindOf::Function);
}

hackc::ExternalDeclProviderResult HhvmDeclProvider::getConst(
  std::string_view symbol
) noexcept {
  return getDecls(symbol, 0, AutoloadMap::KindOf::Constant);
}

hackc::ExternalDeclProviderResult HhvmDeclProvider::getModule(
  std::string_view symbol
) noexcept {
  return getDecls(symbol, 0, AutoloadMap::KindOf::Module);
}

hackc::ExternalDeclProviderResult HhvmDeclProvider::getDecls(
  std::string_view symbol,
  uint64_t depth,
  AutoloadMap::KindOf kind
) noexcept {
  // TODO(T110866581): symbol should be normalized by hackc
  std::string_view sym(normalizeNS(symbol));
  ITRACE(3, "DP lookup {}\n", sym);

  // Lookup the file where sym is defined in the autoload map.
  auto filename_opt = m_map->getFile(kind, sym);
  if (filename_opt) {
    auto filename = filename_opt->native();
    auto result = m_cache.find(filename);

    if (result != m_cache.end()) {
      ITRACE(3, "DP found cached decls for {} in {}\n", sym, filename);
      return hackc::ExternalDeclProviderResult::from_decls(*result->second);
    }

    // Nothing cached: Load file, parse decls.
    std::ifstream s(filename);
    std::string text {
      std::istreambuf_iterator<char>(s), std::istreambuf_iterator<char>()
    };

    try {
      auto holder = hackc::parse_decls(
          m_config,
          filename,
          {(const uint8_t*)text.data(), text.size()}
      );
      ITRACE(3, "DP parsed {} in {}\n", sym, filename);

      auto const norm_filename =
        std::filesystem::relative(*filename_opt, m_repo);

      auto const hash = SHA1{string_sha1(text)};
      m_deps.emplace(DeclSym{kind, symbol}, DepInfo{norm_filename, depth, hash});

      // Insert decl_result into the cache, return DeclsAndBlob::decls,
      // a pointer to rust decls in m_cache.
      auto [it, _] = m_cache.insert({filename, std::move(holder)});
      return hackc::ExternalDeclProviderResult::from_decls(*it->second);
    } catch ([[maybe_unused]] const std::exception& ex) {
      // Decl parser error - don't cache anything, and don't fall through.
      ITRACE(4, "DP {}: decl parse error: {}", sym, ex.what());
      return hackc::ExternalDeclProviderResult::missing();
    }
  }

  ITRACE(4, "DP {}: getFile() returned None\n", sym);

  auto const res = Native::getBuiltinDecls(makeStaticString(sym), kind);
  if (res) {
    ITRACE(4, "DP {}: found in extensions or systemlib\n", sym);
    return *res;
  }
  ITRACE(4, "DP {}: symbol not found in native decl registry\n", sym);
  m_sawMissing = true;
  return hackc::ExternalDeclProviderResult::missing();
}

std::vector<DeclDep> HhvmDeclProvider::getFlatDeps() const {
  hphp_hash_map<std::string, SHA1> deps;
  for (auto& [sym, info] : m_deps) deps.emplace(info.file, info.hash);

  std::vector<DeclDep> flat;
  for (auto [file, hash] : deps) flat.emplace_back(DeclDep{file, hash});
  return flat;
}

std::vector<std::vector<DeclLoc>> HhvmDeclProvider::getLocsByDepth() const {
  uint64_t max_depth = 0;

  hphp_hash_map<std::string, std::pair<DeclSym, DepInfo>> deps;
  for (auto& [sym, info] : m_deps) {
    auto [it, inserted] = deps.emplace(info.file, std::make_pair(sym, info));
    // smaller depth replaces entry with bigger depth, for equal depths,
    // first one wins.
    if (!inserted && info.depth < it->second.second.depth) {
      it->second = std::make_pair(sym, info);
    }
    max_depth = std::max(info.depth, max_depth);
  }

  std::vector<std::vector<DeclLoc>> locs_by_depth(max_depth + 1);
  for (auto& [file, sym_info] : deps) {
    auto& [sym, info] = sym_info;
    locs_by_depth[info.depth].emplace_back(
      std::make_pair(sym, DeclDep{file, info.hash})
    );
  }

  // Now that we've collapsed files in multiple dep lists to the list with the
  // lowest level there may be empty lists. Our clients care about an ordering
  // of dependencies by depth, not the specific levels themselves,
  // so as a convenience guarantee that each dependency list is non-empty.
  locs_by_depth.erase(
    std::remove_if(locs_by_depth.begin(), locs_by_depth.end(), [] (auto& locs) {
      return locs.empty();
    }),
    locs_by_depth.end()
  );

  return locs_by_depth;
}
}//namespace HPHP
