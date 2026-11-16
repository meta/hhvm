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

#include "hphp/runtime/vm/runtime-compiler.h"

#include "hphp/runtime/base/autoload-handler.h"
#include "hphp/runtime/base/program-functions.h"
#include "hphp/runtime/base/repo-autoload-map.h"
#include "hphp/runtime/base/request-info.h"
#include "hphp/runtime/base/runtime-option.h"
#include "hphp/runtime/base/static-string-table.h"
#include "hphp/runtime/base/unit-cache.h"

#include "hphp/runtime/vm/as.h"
#include "hphp/runtime/vm/repo-autoload-map-builder.h"
#include "hphp/runtime/vm/repo-file.h"
#include "hphp/runtime/vm/repo-global-data.h"
#include "hphp/runtime/vm/unit-parser.h"
#include "hphp/runtime/vm/jit/tc.h"

#include "hphp/util/assertions.h"
#include "hphp/util/configs/eval.h"
#include "hphp/util/embedded-data-reader.h"
#include "hphp/util/sha1.h"

#include "hphp/zend/zend-string.h"

#include <stdexcept>
#include <folly/Likely.h>
#include <folly/Range.h>

namespace HPHP {

TRACE_SET_MOD(runtime)

namespace {

///////////////////////////////////////////////////////////////////////////////

std::mutex s_encodedUELock;
std::vector<RepoFileBuilder::EncodedUE> s_encodedUEs;
RepoAutoloadMapBuilder s_autoload;

void onLoadUE(std::unique_ptr<UnitEmitter>& ue) {
  if (Cfg::Repo::Authoritative || !jit::tc::dumpEnabled()) return;

  std::lock_guard lock{s_encodedUELock};
  ue->m_sn = s_encodedUEs.size();
  s_autoload.addUnit(*ue);
  s_encodedUEs.emplace_back(RepoFileBuilder::EncodedUE{*ue, false});
}

std::unique_ptr<UnitEmitter> parse(LazyUnitContentsLoader& loader,
                                   CodeSource codeSource,
                                   const char* filename,
                                   const Extension* extension,
                                   AutoloadMap* map,
                                   bool isSystemLib,
                                   bool forDebuggerEval) {
  if (!filename) filename = "";

  tracing::Block _{
    "parse",
    [&] {
      return tracing::Props{}
        .add("filename", filename ? filename : "")
        .add("code_size", loader.fileLength());
    }
  };
  auto const wasLoaded = loader.didLoad();
  SCOPE_EXIT {
    if (!wasLoaded && loader.didLoad()) {
      tracing::updateName("parse-load");
    }
  };

  // Do not count memory used during parsing/emitting towards OOM.
  MemoryManager::SuppressOOM so(*tl_heap);

  SCOPE_ASSERT_DETAIL("parsing file") { return filename; };

  // We don't want to invoke the JIT when trying to run PHP code.
  auto const prevFolding = RID().getJitFolding();
  RID().setJitFolding(true);
  SCOPE_EXIT { RID().setJitFolding(prevFolding); };

  std::unique_ptr<UnitEmitter> ue;
  // Check if this file contains raw HHAS instead of Hack source code.
  // This is dictated by a special file extension.
  if (Cfg::Eval::AllowHhas &&
      folly::StringPiece(filename).endsWith(".hhas")) {
    auto const& contents = loader.contents();
    ue = assemble_string(
      contents,
      filename,
      loader.sha1(),
      extension,
      RepoOptions::forFile(filename).packageInfo(),
      loader.unitEmitterAttributes()
    );
  }

  // If ue != nullptr then we assembled it above, so don't feed it into
  // the extern compiler
  if (!ue) {
    auto uc = UnitCompiler::create(
      loader,
      codeSource,
      filename,
      extension,
      map,
      isSystemLib,
      forDebuggerEval
    );
    assertx(uc);
    tracing::BlockNoTrace _2{"unit-compiler-run"};
    SCOPE_EXIT {
      if (!wasLoaded && loader.didLoad()) {
        tracing::updateName("unit-compiler-run-load");
      }
    };
    try {
      bool cache_hit;
      ue = uc->compile(cache_hit);
    } catch (const CompilerAbort& exn) {
      fprintf(stderr, "%s", exn.what());
      _Exit(HPHP_EXIT_FAILURE);
    }
  }

  assertx(ue);
  ue->logDeclInfo();

  onLoadUE(ue);
  return ue;
}

RepoGlobalData getGlobalData() {
  auto const now = std::chrono::high_resolution_clock::now();
  auto const nanos =
    std::chrono::duration_cast<std::chrono::nanoseconds>(
      now.time_since_epoch()
    );

  auto gd      = RepoGlobalData{};
  gd.Signature = nanos.count();

  Cfg::StoreToGlobalData(gd);

  gd.EvalCoeffectEnforcementLevels = RO::EvalCoeffectEnforcementLevels;

  return gd;
}

///////////////////////////////////////////////////////////////////////////////

}

void dump_compiled_units(const std::string& path) {
  RepoFileBuilder repo{path};
  auto const gd  = getGlobalData();
  auto const pkg = RepoOptions::defaults().packageInfo();

  std::lock_guard lock{s_encodedUELock};
  for (auto const& ue : s_encodedUEs) repo.add(ue);
  repo.finish(gd, s_autoload, pkg);
}

Unit* compile_file(LazyUnitContentsLoader& loader,
                   CodeSource codeSource,
                   const char* filename,
                   const Extension* extension,
                   AutoloadMap* map) {
  assertx(!filename || filename[0] != '/' || filename[1] != ':');
  return parse(
    loader,
    codeSource,
    filename,
    extension,
    map,
    false,
    false
  )->create().release();
}

Unit* compile_string(folly::StringPiece s,
                     CodeSource codeSource,
                     const char* fname,
                     const Extension* extension,
                     AutoloadMap* map,
                     const RepoOptions& options,
                     bool isSystemLib,
                     bool forDebuggerEval) {
  // If the file is too large it may OOM the request
  MemoryManager::SuppressOOM so{*tl_heap};

  auto const name = fname ? fname : "";
  auto const attributes = isSystemLib || !fname
    ? UnitEmitterAttributes::defaults()
    : UnitEmitterAttributes::forAbsolutePath(name, options);
  auto const sha1 = SHA1{mangleUnitSha1(
    string_sha1(s), name, options.flags(), attributes
  )};
  LazyUnitContentsLoader loader{
    sha1, s, options.flags(), options.dir(), attributes
  };
  return parse(
    loader,
    codeSource,
    fname,
    extension,
    map,
    isSystemLib,
    forDebuggerEval
  )->create().release();
}

Unit* get_systemlib(const EmbeddedDataReader& reader,
                    const std::string& path,
                    const Extension* extension) {
  assertx(path[0] == '/' && path[1] == ':');

  if (Cfg::Repo::Authoritative) {
    if (auto u = lookupSyslibUnit(makeStaticString(path))) {
      return u;
    }
  }

  auto buffer = reader.readSectionForPath(path+".ue");

  if (buffer.empty()) {
    throw std::invalid_argument("Missing systemlib unit emitter for path: " + path);
  }

  UnitEmitterSerdeWrapper uew;
  BlobDecoder decoder(buffer.data(), buffer.size());
  uew.serde(decoder);

  auto ue = std::move(uew.m_ue);
  ue->m_extension = extension;
  assertx(IMPLIES(ue->isSystemLib(), extension));

  onLoadUE(ue);

  auto u = ue->create();
  SystemLib::registerUnitEmitter(std::move(ue));
  return u.release();
}

std::unique_ptr<UnitEmitter> compile_systemlib_string_to_ue(
    const char* s, size_t sz, const char* fname,
    const Extension* extension) {
  auto const& defaults = RepoOptions::defaultsForSystemlib();
  auto const attributes = UnitEmitterAttributes::defaults();
  auto const sha1 = SHA1{mangleUnitSha1(
    string_sha1(folly::StringPiece{s, sz}),
    fname,
    defaults.flags(),
    attributes
  )};
  LazyUnitContentsLoader loader{
    sha1,
    {s, sz},
    defaults.flags(),
    defaults.dir(),
    attributes
  };
  // We provide the empty autoload map here: there are no symbols that are going
  // to be provided outside of systemlib itself. The created decl provider will
  // hook into `s_builtin_symbols` to provide the relevant decls.
  RepoAutoloadMap empty_map({}, {}, {}, {}, {});
  auto ue = parse(
    loader,
    CodeSource::Systemlib,
    fname,
    extension,
    Cfg::Eval::EnableDecl ? &empty_map : nullptr,
    true,
    false
  );
  onLoadUE(ue);
  always_assert(ue);
  return ue;
}

Unit* compile_debugger_string(
  folly::StringPiece s, const RepoOptions& options
) {
  auto const map = [] () -> AutoloadMap* {
    if (!AutoloadHandler::s_instance) {
      // It is not safe to autoinit AutoloadHandler outside a normal request.
      return nullptr;
    }
    return AutoloadHandler::s_instance->getAutoloadMap();
  }();

  return compile_string(
    s,
    CodeSource::Debugger,
    nullptr,
    nullptr,
    map,
    options,
    false,
    true
  );
}

}
