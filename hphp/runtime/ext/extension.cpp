/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
   | Copyright (c) 1997-2010 The PHP Group                                |
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
#include "hphp/runtime/ext/extension.h"
#include "hphp/runtime/ext/extension-registry.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

#include <folly/Range.h>
#include <folly/Varint.h>

#include "hphp/util/assertions.h"
#include "hphp/util/blob-encoder.h"
#include "hphp/util/trace.h"
#include "hphp/runtime/ext/apache/ext_apache.h"
#include "hphp/runtime/ext/apc/ext_apc.h"
#include "hphp/runtime/base/program-functions.h"
#include "hphp/runtime/base/tv-shared.h"
#include "hphp/runtime/vm/builtin-symbol-map.h"
#include "hphp/runtime/vm/native-func-table.h"
#include "hphp/runtime/vm/runtime-compiler.h"
#include "hphp/runtime/vm/unit.h"
#include "hphp/system/systemlib.h"

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

Extension::Extension(const char* name, const char* version, const char* oncall)
    : m_name(name)
    , m_version(version)
    , m_oncall(oncall) {
  ExtensionRegistry::registerExtension(this);
}

bool Extension::IsSystemlibPath(const std::string& name) {
  return !name.compare(0, 2, "/:");
}

/**
 * Loads a named systemlib section from the main binary (or DSO)
 * using the label "ext.{hash(name)}"
 *
 * If {name} is not passed, then {m_name} is assumed.
 */
void Extension::loadSystemlib(const std::string& name) {
  auto const moduleName = std::string("/:"+name);
  auto const unit = get_systemlib(moduleName, this);
  always_assert_flog(unit, "No unit created for systemlib `{}'", moduleName);
  if (auto const info = unit->getFatalInfo()) {
    std::fprintf(stderr, "Systemlib `%s' contains a fataling unit: %s, %d\n",
                 moduleName.c_str(),
                 info->m_fatalMsg.c_str(),
                 info->m_fatalLoc.line1);
    _Exit(HPHP_EXIT_FAILURE);
  }

  unit->merge();
  SystemLib::addPersistentUnit(unit);
}

void Extension::moduleLoad(const IniSetting::Map& /*ini*/, Hdf /*hdf*/)
{}

void Extension::moduleInit()
{}

void Extension::moduleRegisterNative()
{}

void Extension::moduleInfo(Array &info) {
  info.set(OptString(m_name), true);
}

void Extension::cliClientInit()
{}

void Extension::moduleShutdown()
{}

void Extension::threadInit()
{}

void Extension::threadShutdown()
{}

void Extension::requestInit()
{}

void Extension::requestShutdown()
{}

// override this to control extension_loaded() return value
bool Extension::moduleEnabled() const {
  return true;
}

const Extension::DependencySet Extension::getDeps() const {
  // No dependencies by default
  return DependencySet();
}

void Extension::registerExtensionFunction(const OptString& name) {
  assertx(name.get()->isStatic());
  m_functions.push_back(name.get());
}

const std::vector<StringData*>& Extension::getExtensionFunctions() const {
  return m_functions;
}

std::vector<std::string> Extension::hackFiles() const {
  return {"ext_" + toLower(std::string(m_name) + ".php")};
}

void Extension::loadEmitters() {
  for (auto const& name : hackFiles()) {
    loadSystemlib(name);
  }
}

void Extension::loadDecls() {
  // Look for "ext.{namehash}" in the binary and grab its decls
  for (auto const& name : hackFiles()) {
    loadDeclsFrom(name);
  }
}

void Extension::loadDeclsFrom(const std::string& name) {
  auto serialized_decls = get_embedded_section("/:" + name + ".decls");
  always_assert(serialized_decls.size() > 0);
  Native::registerBuiltinSymbols(serialized_decls);
}

void Extension::deserialize(BlobDecoder& sd) {
  auto bytes = folly::ByteRange{sd.data(), sd.remaining()};
  auto const valid = [&] {
    if (bytes.size() < sizeof(uint32_t)) return false;
    uint32_t count;
    std::memcpy(&count, bytes.data(), sizeof(count));
    bytes.advance(sizeof(count));
    if (count > bytes.size()) return false;
    for (uint32_t i = 0; i < count; ++i) {
      auto const size = folly::tryDecodeVarint(bytes);
      if (!size || *size > bytes.size()) return false;
      bytes.advance(*size);
    }
    return bytes.empty();
  }();
  if (!valid) throw std::runtime_error("Invalid extension warmup data");

  auto init = sd.readArrayInitWithLazyCount<VecInit>(
    [&](VecInit& v) {
      std::string_view sv;
      sd(sv);
      if (Trace::moduleEnabledRelease(Trace::apc_warmup, 1)) {
        Trace::ftraceRelease("Extension::deserialize [{}]: key '{}'\n", m_name, sv);
      }
      auto sd = StringData::MakeShared(sv);
      v.append(make_tv<KindOfPersistentString>(sd));
    });
  auto d = init.toArray();
  if (Trace::moduleEnabledRelease(Trace::apc_warmup, 1)) {
    Trace::ftraceRelease("Extension::deserialize [{}]: {} warmup keys\n", m_name, d.size());
  }
  MakeSharedEnv env {nullptr, nullptr};
  setWarmupData(MakeSharedArray(d.get(), env));
}

/////////////////////////////////////////////////////////////////////////////
} // namespace HPHP
