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

#include "hphp/runtime/vm/unit-parser.h"

#include <memory>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <folly/compression/Zstd.h>
#include <folly/json/DynamicConverter.h>
#include <folly/FileUtil.h>
#include <folly/system/ThreadName.h>

#include "hphp/hack/src/hackc/ffi_bridge/compiler_ffi.rs.h"
#include "hphp/hack/src/hackc/hhbc-unit.h"
#include "hphp/hack/src/parser/ffi_bridge/parser_ffi.rs.h"
#include "hphp/runtime/base/autoload-map.h"
#include "hphp/runtime/base/file-stream-wrapper.h"
#include "hphp/runtime/base/stream-wrapper-registry.h"
#include "hphp/runtime/base/unit-cache.h"
#include "hphp/runtime/vm/decl-provider.h"
#include "hphp/runtime/vm/hackc-translator.h"
#include "hphp/runtime/vm/unit-emitter.h"
#include "hphp/runtime/vm/unit-gen-helpers.h"
#include "hphp/util/configs/eval.h"
#include "hphp/util/match.h"
#include "hphp/util/sha1.h"
#include "hphp/zend/zend-string.h"

namespace HPHP {

TRACE_SET_MOD(unit_parse)

UnitEmitterCacheHook g_unit_emitter_cache_hook = nullptr;

///////////////////////////////////////////////////////////////////////////////
// Cache layer dispatcher

namespace {

struct CacheLayer {
  UnitEmitterCacheHookPriority priority;
  std::string name;
  UnitEmitterCacheLayer hook;
};

std::vector<CacheLayer> s_cache_layers;

// True after initUnitEmitterCacheDispatcher(); registration is forbidden after this.
static bool s_layers_frozen = false;

// Forward declaration for recursive dispatch.
std::unique_ptr<UnitEmitter> invokeLayer(
  size_t depth,
  const char* filename,
  const SHA1& sha1,
  folly::StringPiece::size_type fileLen,
  HhvmDeclProvider* provider,
  const std::function<std::unique_ptr<UnitEmitter>(bool)>& hackc);

// The dispatcher set as g_unit_emitter_cache_hook once any layer is registered.
// Layers are sorted by priority ascending; lowest priority is called first (outermost).
std::unique_ptr<UnitEmitter> dispatchUnitEmitterCacheLayers(
  const char* filename,
  const SHA1& sha1,
  folly::StringPiece::size_type fileLen,
  HhvmDeclProvider* provider,
  const std::function<std::unique_ptr<UnitEmitter>(bool)>& hackc) {
  if (s_cache_layers.empty()) return hackc(true);
  return invokeLayer(0, filename, sha1, fileLen, provider, hackc);
}

std::unique_ptr<UnitEmitter> invokeLayer(
  size_t depth,
  const char* filename,
  const SHA1& sha1,
  folly::StringPiece::size_type fileLen,
  HhvmDeclProvider* provider,
  const std::function<std::unique_ptr<UnitEmitter>(bool)>& hackc) {
  auto const n = s_cache_layers.size();
  assertx(depth < n);
  auto const& layer = s_cache_layers[depth];
  return layer.hook(
    filename, sha1, fileLen, provider,
    [&](bool wantsICE) -> std::unique_ptr<UnitEmitter> {
      if (depth + 1 >= n) return hackc(wantsICE);
      return invokeLayer(depth + 1, filename, sha1, fileLen, provider, hackc);
    }
  );
}

} // namespace

void registerUnitEmitterCacheLayer(
    std::string_view name,
    UnitEmitterCacheHookPriority priority,
    UnitEmitterCacheLayer layer) {
  always_assert(layer != nullptr);
  always_assert(!s_layers_frozen);
  for (auto const& l : s_cache_layers) {
    always_assert_flog(
      l.name != name,
      "HHVM fatal: duplicate unit emitter cache layer '{}'", name);
  }
  // Insert sorted by priority ascending (lower = outermost = called first).
  // Use upper_bound so that layers with equal priority preserve registration order.
  auto it = std::upper_bound(
    s_cache_layers.begin(), s_cache_layers.end(), priority,
    [](UnitEmitterCacheHookPriority p, const CacheLayer& l) {
      return p < l.priority;
    });
  s_cache_layers.insert(it, {priority, std::string(name), layer});
}

void initUnitEmitterCacheDispatcher() {
  always_assert(!g_unit_emitter_cache_hook);
  g_unit_emitter_cache_hook = &dispatchUnitEmitterCacheLayers;
  s_layers_frozen = true;
}

namespace {

struct CompileException : Exception {
  template<class... A>
  explicit CompileException(fmt::format_string<A...> fmt, A&&... args)
    : Exception(fmt::format(fmt, std::forward<A>(args)...))
  {}
};

[[noreturn]] void throwErrno(const char* what) {
  throw CompileException("{}: {}", what, folly::errnoStr(errno));
}

CompilerResult unitEmitterFromHackCUnitHandleErrors(const hackc::hhbc::Unit& unit,
                                                    const char* filename,
	                                                  const SHA1& sha1,
	                                                  const SHA1& bcSha1,
                                                    const Extension* extension,
                                                    bool& internal_error,
                                                    CompileAbortMode mode,
                                                    const PackageInfo& packageInfo,
                                                    UnitEmitterAttributes attributes) {
  try {
    return unitEmitterFromHackCUnit(unit, filename, sha1, bcSha1,
                                    extension, false, packageInfo, attributes);
  } catch (const FatalErrorException&) {
    throw;
  } catch (const TranslationFatal& ex) {
    if (mode >= CompileAbortMode::VerifyErrors) internal_error = true;
    return ex.what();
  } catch (const std::exception& ex) {
    internal_error = true;
    return ex.what();
  }
}

////////////////////////////////////////////////////////////////////////////////

CompilerResult hackc_compile(
  folly::StringPiece code,
  CodeSource codeSource,
  const char* filename,
  const SHA1& sha1,
  const Extension* extension,
  bool isSystemLib,
  bool forDebuggerEval,
  bool& internal_error,
  const RepoOptionsFlags& options,
  UnitEmitterAttributes attributes,
  CompileAbortMode mode,
  hackc::DeclProvider* provider
) {
  hackc::NativeEnv native_env{
    .decl_provider = reinterpret_cast<uint64_t>(provider),
    .filepath = filename,
    .parser_flags = hackc::ParserFlags {
    },
    .flags = hackc::EnvFlags {
      .is_systemlib = isSystemLib,
      .for_debugger_eval = forDebuggerEval,
      .disable_toplevel_elaboration = false,
      .enable_ir = false,
    }
  };
  options.initAliasedNamespaces(native_env);
  options.initExperimentalFeatures(native_env);
  options.initHhbcFlags(native_env.hhbc_flags);
  options.initParserFlags(native_env.parser_flags);
  if (Cfg::Eval::HackCompilerInheritConfig) {
    for (auto& [k, v] : RO::IncludeRoots) {
      native_env.include_roots.emplace_back(hackc::StringMapEntry{k, v});
    }
  }

  try {
    rust::Box<hackc::UnitWrapper> unit_wrapped = [&] {
      tracing::Block _{
        "hackc_translator",
        [&] {
          return tracing::Props{}
            .add("filename", filename ? filename : "")
            .add("code_size", code.size());
        }
      };
      return hackc::compile_unit_from_text(
          native_env,
          {(const uint8_t*)code.data(), code.size()}
      );
    }();

    auto const bcSha1 = SHA1(hash_unit(*unit_wrapped));
    const hackc::hhbc::Unit* unit = hackCUnitRaw(unit_wrapped);

    auto hackCResult = unitEmitterFromHackCUnitHandleErrors(
      *unit, filename, sha1, bcSha1, extension,
      internal_error, mode, options.packageInfo(), attributes
    );
    return hackCResult;
  } catch (const std::exception& ex) {
    // Report Rust errors from compile_unit_from_text().
    return ex.what();
  }
}

/// A simple UnitCompiler that invokes hackc in-process.
struct HackcUnitCompiler final : UnitCompiler {
  using UnitCompiler::UnitCompiler;

  std::unique_ptr<UnitEmitter> compile(
    bool& cacheHit,
    HhvmDeclProvider*,
    CompileAbortMode = CompileAbortMode::Never) override;

  const char* getName() const override { return "HackC"; }
};

// UnitCompiler which first tries to retrieve the UnitEmitter via the
// g_unit_emitter_cache_hook. If that fails, delegate to the
// UnitCompiler produced by the "makeFallback" lambda (this avoids
// having to create the fallback UnitEmitter until we need it). The
// lambda will only be called once. Its output is cached afterwards.
struct CacheUnitCompiler final : UnitCompiler {
  CacheUnitCompiler(LazyUnitContentsLoader& loader,
                    CodeSource codeSource,
                    const char* filename,
                    const Extension* extension,
                    AutoloadMap* map,
                    bool isSystemLib,
                    bool forDebuggerEval,
                    std::function<std::unique_ptr<UnitCompiler>()> makeFallback)
    : UnitCompiler{
        loader,
        codeSource,
        filename,
        extension,
        map,
        isSystemLib,
        forDebuggerEval
      }
    , m_makeFallback{std::move(makeFallback)} {}

  std::unique_ptr<UnitEmitter> compile(
    bool& cacheHit,
    HhvmDeclProvider*,
    CompileAbortMode = CompileAbortMode::Never) override;

  const char* getName() const override { return "Cache"; }
private:
  std::function<std::unique_ptr<UnitCompiler>()> m_makeFallback;
  std::unique_ptr<UnitCompiler> m_fallback;
};

////////////////////////////////////////////////////////////////////////////////
}

CompilerAbort::CompilerAbort(const std::string& filename,
                             const std::string& error)
  : std::runtime_error{
      fmt::format(
        "Encountered an internal error while processing HHAS for {}, "
        "bailing because Eval.AbortBuildOnCompilerError is set\n\n{}",
        filename, error
      )
    }
{
}

ParseFactsResult extract_facts(
  const std::string& filename,
  const RepoOptionsFlags& options,
  folly::StringPiece expect_sha1
) {
  auto w = Stream::getWrapperFromURI(StrNR(filename));
  if (!(w && dynamic_cast<FileStreamWrapper*>(w))) {
    throwErrno("Failed to extract facts: Could not get FileStreamWrapper.");
  }
  const auto f = w->open(StrNR(filename), "r", 0, nullptr);
  if (!f) { 
    throwErrno(
      fmt::format(
        "Failed to extract facts: Could not read source code for {}", 
        filename
      ).c_str());
  }
  auto const str = f->read();
  auto const source_text = str.get()->slice();
  auto actual_sha1 = string_sha1(source_text);
  if (!expect_sha1.empty()) {
    if (actual_sha1 != expect_sha1) {
      return fmt::format(
          "Unexpected SHA1: {} != {}", actual_sha1, expect_sha1
      );
    }
  }
  try {
    hackc::DeclParserConfig config;
    options.initDeclConfig(config);
    auto const decls = hackc::parse_decls(
      config,
      filename,
      {(const uint8_t*)source_text.data(), source_text.size()}
    );
    auto blob = hackc::decls_to_facts_binary(*decls, actual_sha1);
    return FactsBinaryString { std::string{(char*)blob.data(), blob.size()} };
  } catch (const std::exception& ) {
    return FactsBinaryString { "" }; // Swallow errors from HackC
  }
}

FfpResult ffp_parse_file(
  const std::string& contents,
  const RepoOptionsFlags& options
) {
  auto const env = options.getParserEnvironment();
  auto const json = hackc_parse_positioned_full_trivia(contents, env);
  return FfpJSONString { std::string(json.begin(), json.end()) };
}

std::unique_ptr<UnitCompiler>
UnitCompiler::create(LazyUnitContentsLoader& loader,
                     CodeSource codeSource,
                     const char* filename,
                     const Extension* extension,
                     AutoloadMap* map,
                     bool isSystemLib,
                     bool forDebuggerEval) {
  auto make = [&loader, codeSource, extension, filename, isSystemLib, forDebuggerEval,
               map] {
    return std::make_unique<HackcUnitCompiler>(
      loader,
      codeSource,
      filename,
      extension,
      map,
      isSystemLib,
      forDebuggerEval
    );
  };

  if (g_unit_emitter_cache_hook && !forDebuggerEval) {
    return std::make_unique<CacheUnitCompiler>(
      loader,
      codeSource,
      filename,
      extension,
      map,
      isSystemLib,
      false,
      std::move(make)
    );
  } else {
    return make();
  }
}

std::unique_ptr<UnitEmitter> UnitCompiler::compile(
    bool& cacheHit,
    CompileAbortMode mode) {
  auto provider = HhvmDeclProvider::create(
    m_map,
    m_loader.options(),
    m_loader.repoRoot()
  );
  return compile(cacheHit, provider.get(), mode);
}

std::unique_ptr<UnitEmitter> compile_unit(
  folly::StringPiece code,
  CodeSource codeSource,
  const char* filename,
  const SHA1& sha1,
  const Extension* extension,
  bool isSystemLib,
  bool forDebuggerEval,
  const RepoOptionsFlags& options,
  UnitEmitterAttributes attributes,
  CompileAbortMode mode,
  hackc::DeclProvider* provider
) {
  bool ice = false;
  auto res = hackc_compile(code, codeSource, filename, sha1, extension, isSystemLib,
      forDebuggerEval, ice, options, attributes, mode, provider);
  auto unitEmitter = match<std::unique_ptr<UnitEmitter>>(res,
    [&] (std::unique_ptr<UnitEmitter>& ue) {
      ue->finish();
      return std::move(ue);
    },
    [&] (std::string& err) {
      switch (mode) {
      case CompileAbortMode::Never:
        break;
      case CompileAbortMode::AllErrorsNull: {
        return std::unique_ptr<UnitEmitter>{};
      }
      case CompileAbortMode::OnlyICE:
      case CompileAbortMode::VerifyErrors:
      case CompileAbortMode::AllErrors:
        // Abort on internal compile errors as appropriate based on mode
        if (ice) throw CompilerAbort{filename, err};
      }
      return createFatalUnit(makeStaticString(filename), sha1,
                             FatalOp::Runtime, err);
    }
  );

  if (unitEmitter) unitEmitter->m_ICE = ice;
  return unitEmitter;
}

std::unique_ptr<UnitEmitter> HackcUnitCompiler::compile(
    bool& cacheHit,
    HhvmDeclProvider* provider,
    CompileAbortMode mode) {
  cacheHit = false;
  auto unitEmitter = compile_unit(m_loader.contents(),
                                  m_codeSource,
                                  m_filename,
                                  m_loader.sha1(),
                                  m_extension,
                                  m_isSystemLib,
                                  m_forDebuggerEval,
                                  m_loader.options(),
                                  m_loader.unitEmitterAttributes(),
                                  mode,
                                  provider);
  if (unitEmitter && provider) unitEmitter->m_deps = provider->getFlatDeps();
  return unitEmitter;
}

std::unique_ptr<UnitEmitter>
CacheUnitCompiler::compile(bool& cacheHit,
                           HhvmDeclProvider* provider,
                           CompileAbortMode mode) {
  assertx(g_unit_emitter_cache_hook);
  cacheHit = true;
  return g_unit_emitter_cache_hook(
    m_filename,
    m_loader.sha1(),
    m_loader.fileLength(),
    provider,
    [&] (bool wantsICE) {
      if (!m_fallback) m_fallback = m_makeFallback();
      assertx(m_fallback);
      return m_fallback->compile(
        cacheHit,
        provider,
        wantsICE ? mode : CompileAbortMode::AllErrorsNull
      );
    }
  );
}

////////////////////////////////////////////////////////////////////////////////
}
