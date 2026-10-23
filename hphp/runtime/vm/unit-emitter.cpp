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

#include "hphp/runtime/vm/unit-emitter.h"

#include "hphp/runtime/base/array-data.h"
#include "hphp/runtime/base/location.h"
#include "hphp/runtime/base/repo-auth-type.h"
#include "hphp/runtime/base/runtime-option.h"
#include "hphp/runtime/base/static-string-table.h"
#include "hphp/runtime/base/typed-value.h"
#include "hphp/runtime/base/unit-cache.h"

#include "hphp/runtime/ext/extension.h"
#include "hphp/runtime/ext/extension-registry.h"

#include "hphp/runtime/vm/disas.h"
#include "hphp/runtime/vm/func.h"
#include "hphp/runtime/vm/func-emitter.h"
#include "hphp/runtime/vm/jit/perf-counters.h"
#include "hphp/runtime/vm/preclass.h"
#include "hphp/runtime/vm/preclass-emitter.h"
#include "hphp/runtime/vm/repo-autoload-map-builder.h"
#include "hphp/runtime/vm/type-alias-emitter.h"
#include "hphp/runtime/vm/unit.h"
#include "hphp/runtime/vm/verifier/check.h"

#include "hphp/util/blob-encoder.h"
#include "hphp/util/configs/debugger.h"
#include "hphp/util/configs/eval.h"
#include "hphp/util/logger.h"
#include "hphp/util/sha1.h"
#include "hphp/util/trace.h"

#include <boost/algorithm/string/predicate.hpp>

#include <folly/Memory.h>
#include <folly/FileUtil.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <fmt/format.h>

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

TRACE_SET_MOD(hhbc)

///////////////////////////////////////////////////////////////////////////////

UnitEmitterAttributes UnitEmitterAttributes::defaults() {
  return {};
}

UnitEmitterAttributes UnitEmitterAttributes::forAbsolutePath(
    const std::filesystem::path& path,
    const RepoOptions& options) {
  assertx(path.is_absolute());
  auto const policy = options.packageInfo().strictDynamicReferencePolicyForPath(
    path, options.dir()
  );
  return {
    policy.strictIsolation,
    policy.raiseDynamicClassLoadError,
  };
}

UnitEmitterAttributes UnitEmitterAttributes::forRepoRelativePath(
    const std::filesystem::path& path,
    const RepoOptionsFlags& options) {
  assertx(path.is_relative());
  auto const policy = options.packageInfo().strictDynamicReferencePolicyForPath(
    path, {}
  );
  return {
    policy.strictIsolation,
    policy.raiseDynamicClassLoadError,
  };
}

std::string UnitEmitterAttributes::mangle() const {
  return {
    strictPackage ? '1' : '0',
    '|',
    raiseDynamicClassLoadError ? '1' : '0'
  };
}

///////////////////////////////////////////////////////////////////////////////

UnitEmitter::UnitEmitter(const SHA1& sha1,
                         const SHA1& bcSha1,
                         const PackageInfo& packageInfo,
                         UnitEmitterAttributes attributes)
  : m_attributes(attributes)
  , m_packageInfo(packageInfo)
  , m_sha1(sha1)
  , m_bcSha1(bcSha1)
  , m_nextFuncSn(0)
{}

std::unique_ptr<UnitEmitter> UnitEmitter::makeForSerde(
    const SHA1& sha1,
    const SHA1& bcSha1,
    const PackageInfo& packageInfo) {
  return std::make_unique<UnitEmitter>(
    sha1, bcSha1, packageInfo, UnitEmitterAttributes::defaults()
  );
}

UnitEmitter::~UnitEmitter() {
  for (auto& pce : m_pceVec) delete pce;
}

///////////////////////////////////////////////////////////////////////////////
// Litstrs and Arrays.

const StringData* UnitEmitter::lookupLitstrId(Id id) const {
  assertx(id >= 0 && id < m_litstrs.size());
  auto& elem = m_litstrs[id];
  auto wrapper = elem.copy();
  if (wrapper.isPtr()) {
    assertx(!wrapper.ptr() || wrapper.ptr()->isStatic());
    return wrapper.ptr();
  }
  auto lock = elem.lock_for_update();
  wrapper = elem.copy();
  if (wrapper.isPtr()) {
    assertx(!wrapper.ptr() || wrapper.ptr()->isStatic());
    return wrapper.ptr();
  }
  auto const str = loadLitstrFromRepo(m_sn, wrapper.token(), true);
  assertx(!str || str->isStatic());
  lock.update(StringOrToken::FromPtr(str));
  return str;
}

const ArrayData* UnitEmitter::lookupArrayId(Id id) const {
  assertx(id >= 0 && id < m_arrays.size());
  auto& elem = m_arrays[id];
  auto wrapper = elem.copy();
  if (wrapper.isPtr()) {
    assertx(wrapper.ptr());
    assertx(wrapper.ptr()->isStatic());
    return wrapper.ptr();
  }
  auto lock = elem.lock_for_update();
  wrapper = elem.copy();
  if (wrapper.isPtr()) {
    assertx(wrapper.ptr());
    assertx(wrapper.ptr()->isStatic());
    return wrapper.ptr();
  }

  auto const oldStrEmitter =
    BlobEncoderHelper<const StringData*>::tl_unitEmitter;
  auto const oldArrEmitter =
    BlobEncoderHelper<const ArrayData*>::tl_unitEmitter;

  BlobEncoderHelper<const StringData*>::tl_unitEmitter =
    const_cast<UnitEmitter*>(this);
  BlobEncoderHelper<const ArrayData*>::tl_unitEmitter =
    const_cast<UnitEmitter*>(this);
  SCOPE_EXIT {
    assertx(BlobEncoderHelper<const StringData*>::tl_unitEmitter == this);
    assertx(BlobEncoderHelper<const ArrayData*>::tl_unitEmitter == this);
    BlobEncoderHelper<const StringData*>::tl_unitEmitter = oldStrEmitter;
    BlobEncoderHelper<const ArrayData*>::tl_unitEmitter = oldArrEmitter;
  };

  auto const array = [&] {
    // Check if we're eagerly loading arrays during the initial
    // UnitEmitter deserialization.
    if (m_litarrayBuffer) {
      auto const offset = wrapper.token();
      assertx(offset <= m_litarrayBufferSize);
      return loadLitarrayFromPtr(
        m_litarrayBuffer + offset,
        m_litarrayBufferSize - offset
      );
    }
    return loadLitarrayFromRepo(m_sn, wrapper.token(), true);
  }();
  assertx(array);
  assertx(array->isStatic());
  lock.update(ArrayOrToken::FromPtr(array));
  return array;
}

const RepoAuthType::Array* UnitEmitter::lookupRATArray(Id id) const {
  assertx(id >= 0 && id < m_rats.size());
  auto& elem = m_rats[id];
  auto wrapper = elem.copy();
  if (wrapper.isPtr()) {
    assertx(wrapper.ptr());
    return wrapper.ptr();
  }
  auto lock = elem.lock_for_update();
  wrapper = elem.copy();
  if (wrapper.isPtr()) {
    assertx(wrapper.ptr());
    return wrapper.ptr();
  }
  auto const array = loadRATArrayFromRepo(m_sn, wrapper.token());
  assertx(array);
  lock.update(RATArrayOrToken::FromPtr(array));
  return array;
}

OptString UnitEmitter::lookupLitstrIdCopy(Id id) const {
  assertx(id >= 0 && id < m_litstrs.size());
  auto& elem = m_litstrs[id];
  auto wrapper = elem.copy();
  if (wrapper.isPtr()) {
    assertx(!wrapper.ptr() || wrapper.ptr()->isStatic());
    if (!wrapper.ptr()) return OptString{};
    return OptString::attach(const_cast<StringData*>(wrapper.ptr()));
  }
  auto const str = loadLitstrFromRepo(m_sn, wrapper.token(), false);
  if (!str) return OptString{};
  return OptString::attach(const_cast<StringData*>(str));
}

Array UnitEmitter::lookupArrayIdCopy(Id id) const {
  assertx(id >= 0 && id < m_arrays.size());
  auto& elem = m_arrays[id];
  auto wrapper = elem.copy();
  if (wrapper.isPtr()) {
    assertx(wrapper.ptr());
    assertx(wrapper.ptr()->isStatic());
    return Array::attach(const_cast<ArrayData*>(wrapper.ptr()));
  }

  auto const oldStrEmitter =
    BlobEncoderHelper<const StringData*>::tl_unitEmitter;
  auto const oldArrEmitter =
    BlobEncoderHelper<const ArrayData*>::tl_unitEmitter;

  BlobEncoderHelper<const StringData*>::tl_unitEmitter =
    const_cast<UnitEmitter*>(this);
  BlobEncoderHelper<const ArrayData*>::tl_unitEmitter =
    const_cast<UnitEmitter*>(this);
  SCOPE_EXIT {
    assertx(BlobEncoderHelper<const StringData*>::tl_unitEmitter == this);
    assertx(BlobEncoderHelper<const ArrayData*>::tl_unitEmitter == this);
    BlobEncoderHelper<const StringData*>::tl_unitEmitter = oldStrEmitter;
    BlobEncoderHelper<const ArrayData*>::tl_unitEmitter = oldArrEmitter;
  };

  auto const array = loadLitarrayFromRepo(m_sn, wrapper.token(), false);
  assertx(array);
  return Array::attach(const_cast<ArrayData*>(array));
}

Id UnitEmitter::mergeLitstr(const StringData* litstr) {
  assertx(!litstr || litstr->isStatic());
  auto const it = m_litstr2id.find(litstr);
  if (it == m_litstr2id.end()) {
    auto const id = m_litstrs.size();
    m_litstrs.emplace_back(StringOrToken::FromPtr(litstr));
    auto const DEBUG_ONLY insert = m_litstr2id.emplace(litstr, id);
    assertx(insert.second);
    return id;
  } else {
    return it->second;
  }
}

Id UnitEmitter::mergeArray(const ArrayData* a) {
  assertx(a);
  assertx(a->isStatic());
  auto const it = m_array2id.find(a);
  if (it == m_array2id.end()) {
    auto const id = m_arrays.size();
    m_arrays.emplace_back(ArrayOrToken::FromPtr(a));
    auto const DEBUG_ONLY insert = m_array2id.emplace(a, id);
    assertx(insert.second);
    return id;
  } else {
    return it->second;
  }
}

Id UnitEmitter::mergeRATArray(const RepoAuthType::Array* a) {
  assertx(a);
  auto const it = m_rat2id.find(a);
  if (it == m_rat2id.end()) {
    auto const id = m_rats.size();
    m_rats.emplace_back(RATArrayOrToken::FromPtr(a));
    auto const DEBUG_ONLY insert = m_rat2id.emplace(a, id);
    assertx(insert.second);
    return id;
  } else {
    return it->second;
  }
}

const StringData* UnitEmitter::loadLitstrFromRepo(int64_t unitSn,
                                                  RepoFile::Token token,
                                                  bool makeStatic) {
  assertx(Cfg::Repo::Authoritative);

  // Reset tl_unitEmitter or tl_unit (if set). We're loading a string
  // for the unit's string table, so the encoder shouldn't access the
  // string table.
  auto const oldEmitter = BlobEncoderHelper<const StringData*>::tl_unitEmitter;
  auto const oldUnit = BlobEncoderHelper<const StringData*>::tl_unit;
  BlobEncoderHelper<const StringData*>::tl_unitEmitter = nullptr;
  BlobEncoderHelper<const StringData*>::tl_unit = nullptr;
  SCOPE_EXIT {
    assertx(!BlobEncoderHelper<const StringData*>::tl_unitEmitter);
    assertx(!BlobEncoderHelper<const StringData*>::tl_unit);
    BlobEncoderHelper<const StringData*>::tl_unitEmitter = oldEmitter;
    BlobEncoderHelper<const StringData*>::tl_unit = oldUnit;
  };

  auto const remaining = RepoFile::remainingSizeOfUnit(unitSn, token);

  size_t actualSize;
  {
    auto const size = std::min<size_t>(remaining, 64);
    auto const data = std::make_unique<unsigned char[]>(size);
    RepoFile::readRawFromUnit(unitSn, token, data.get(), size);
    BlobDecoder decoder{data.get(), size};
    actualSize = BlobEncoderHelper<const StringData*>::peekSize(decoder);
    if (actualSize <= decoder.remaining()) {
      const StringData* s;
      decoder(s, makeStatic);
      assertx(IMPLIES(makeStatic, s->isStatic()));
      return s;
    }
  }

  always_assert(actualSize <= remaining);
  always_assert(actualSize <= std::numeric_limits<uint32_t>::max());

  auto const data = std::make_unique<unsigned char[]>(actualSize);
  RepoFile::readRawFromUnit(unitSn, token, data.get(), actualSize);
  BlobDecoder decoder{data.get(), actualSize};
  const StringData* s;
  decoder(s, makeStatic);
  assertx(IMPLIES(makeStatic, s->isStatic()));
  decoder.assertDone();
  return s;
}

const ArrayData* UnitEmitter::loadLitarrayFromRepo(int64_t unitSn,
                                                   RepoFile::Token token,
                                                   bool makeStatic) {
  assertx(Cfg::Repo::Authoritative);

  auto const oldDefer = BlobEncoderHelper<const ArrayData*>::tl_defer;
  BlobEncoderHelper<const ArrayData*>::tl_defer = true;
  SCOPE_EXIT {
    assertx(BlobEncoderHelper<const ArrayData*>::tl_defer);
    BlobEncoderHelper<const ArrayData*>::tl_defer = oldDefer;
  };

  auto const remaining = RepoFile::remainingSizeOfUnit(unitSn, token);
  size_t actualSize;
  {
    auto const size = std::min<size_t>(remaining, 64);
    auto const data = std::make_unique<unsigned char[]>(size);
    RepoFile::readRawFromUnit(unitSn, token, data.get(), size);

    BlobDecoder decoder{data.get(), size};
    actualSize = decoder.peekSize32();
    if (actualSize <= decoder.remaining()) {
      const ArrayData* ad;
      decoder.withSize32([&] { decoder(ad, makeStatic); });
      assertx(ad);
      assertx(IMPLIES(makeStatic, ad->isStatic()));
      return ad;
    }
  }

  always_assert(actualSize <= remaining);
  always_assert(actualSize <= std::numeric_limits<uint32_t>::max());

  auto const data = std::make_unique<unsigned char[]>(actualSize);
  RepoFile::readRawFromUnit(unitSn, token, data.get(), actualSize);

  BlobDecoder decoder{data.get(), actualSize};
  const ArrayData* ad;
  decoder.withSize32([&] { decoder(ad, makeStatic); });
  assertx(ad);
  assertx(IMPLIES(makeStatic, ad->isStatic()));
  decoder.assertDone();
  return ad;
}

const ArrayData* UnitEmitter::loadLitarrayFromPtr(const char* ptr,
                                                  size_t size) {
  auto const oldDefer = BlobEncoderHelper<const ArrayData*>::tl_defer;
  BlobEncoderHelper<const ArrayData*>::tl_defer = true;
  SCOPE_EXIT {
    assertx(BlobEncoderHelper<const ArrayData*>::tl_defer);
    BlobEncoderHelper<const ArrayData*>::tl_defer = oldDefer;
  };

  BlobDecoder decoder{ptr, size};
  const ArrayData* ad;
  decoder.withSize32([&] { decoder(ad); });
  assertx(ad && ad->isStatic());
  return ad;
}

const RepoAuthType::Array*
UnitEmitter::loadRATArrayFromRepo(int64_t unitSn,
                                  RepoFile::Token token) {
  assertx(Cfg::Repo::Authoritative);
  assertx(BlobEncoderHelper<const StringData*>::tl_unit ||
          BlobEncoderHelper<const StringData*>::tl_unitEmitter);

  auto const remaining = RepoFile::remainingSizeOfUnit(unitSn, token);

  size_t actualSize;
  {
    auto const size = std::min<size_t>(remaining, 64);
    auto const data = std::make_unique<unsigned char[]>(size);
    RepoFile::readRawFromUnit(unitSn, token, data.get(), size);
    BlobDecoder decoder{data.get(), size};
    actualSize = decoder.peekSize();
    if (actualSize <= decoder.remaining()) {
      const RepoAuthType::Array* array = nullptr;
      decoder.withSize(
        [&] { array = RepoAuthType::Array::deserialize(decoder); }
      );
      assertx(array);
      return array;
    }
  }

  always_assert(actualSize <= remaining);
  always_assert(actualSize <= std::numeric_limits<uint32_t>::max());

  auto const data = std::make_unique<unsigned char[]>(actualSize);
  RepoFile::readRawFromUnit(unitSn, token, data.get(), actualSize);
  BlobDecoder decoder{data.get(), actualSize};
  const RepoAuthType::Array* array = nullptr;
  decoder.withSize(
    [&] { array = RepoAuthType::Array::deserialize(decoder); }
  );
  decoder.assertDone();
  assertx(array);
  return array;
}

///////////////////////////////////////////////////////////////////////////////
// FuncEmitters.

FuncEmitter* UnitEmitter::newFuncEmitter(const StringData* name, int64_t sn) {
  if (sn == -1) {
    sn = m_nextFuncSn++;
  }
  auto fe = std::make_unique<FuncEmitter>(sn, m_fes.size(), name);
  m_fes.push_back(std::move(fe));
  return m_fes.back().get();
}

FuncEmitter* UnitEmitter::newMethodEmitter(const StringData* name,
                                           PreClassEmitter* pce, int64_t sn) {
  if (sn == -1) {
    sn = m_nextFuncSn++;
  }
  return new FuncEmitter(sn, name, pce);
}

///////////////////////////////////////////////////////////////////////////////
// PreClassEmitters.

PreClassEmitter* UnitEmitter::newPreClassEmitter(
  const std::string& name
) {
  auto n = makeStaticString(name);
  auto pce = new PreClassEmitter(n);
  m_pceVec.emplace_back(pce);
  return pce;
}

///////////////////////////////////////////////////////////////////////////////
// Type aliases.

TypeAliasEmitter* UnitEmitter::newTypeAliasEmitter(const std::string& name) {
  auto te = std::make_unique<TypeAliasEmitter>(*this, m_typeAliases.size(), name);
  m_typeAliases.push_back(std::move(te));
  return m_typeAliases.back().get();
}

///////////////////////////////////////////////////////////////////////////////
// Constants.

Id UnitEmitter::addConstant(const Constant& c) {
  // A KindOfUninit constant is a sentinel that the runtime should invoke
  // 86cinit_<cnsName> by calling Constant::get(). hackc emits these
  // initializer functions for constants that could not be folded.
  Id id = m_constants.size();
  TRACE(1, "Add Constant %d %s %d\n", id, c.name->data(), c.attrs);
  m_constants.push_back(c);
  return id;
}

///////////////////////////////////////////////////////////////////////////////
// Modules.

Id UnitEmitter::addModule(const Module& m) {
  Id id = m_modules.size();
  TRACE(1, "Add Module %d %s %d\n", id, m.name->data(), m.attrs);
  m_modules.push_back(m);
  return id;
}

///////////////////////////////////////////////////////////////////////////////
// Logging.

void UnitEmitter::logDeclInfo() const {
  if ((m_errorSyms.empty() && m_missingSyms.empty()) ||
      !StructuredLog::coinflip(Cfg::Eval::LogDeclErrors)) return;

  StructuredLogEntry ent;
  ent.setInt("sample_rate", Cfg::Eval::LogDeclErrors);
  ent.setInt("num_deps", m_deps.size());

  std::vector<std::string> errors;
  std::vector<std::string> missing;
  for (auto sym : m_errorSyms) errors.emplace_back(sym->toCppString());
  for (auto sym : m_missingSyms) missing.emplace_back(sym->toCppString());

  std::sort(errors.begin(), errors.end());
  std::sort(missing.begin(), missing.end());

  errors.erase(std::unique(errors.begin(), errors.end()), errors.end());
  missing.erase(std::unique(missing.begin(), missing.end()), missing.end());

  ent.setInt("num_errors", errors.size());
  ent.setInt("num_missing", missing.size());

  auto const logVec = [&] (auto name, auto& vec) {
    std::vector<folly::StringPiece> v;
    v.reserve(vec.size());
    for (auto& s : vec) v.emplace_back(s);
    ent.setVec(name, v);
  };

  logVec("error_symbols", errors);
  logVec("missing_symbols", missing);

  StructuredLog::log("hhvm_decl_error_logging", ent);
}

///////////////////////////////////////////////////////////////////////////////
// EntryPoint calculation.

const StaticString s___EntryPoint("__EntryPoint");

void UnitEmitter::calculateEntryPointId() {
  if (m_entryPointIdCalculated) return;

  for (auto& fe : m_fes) {
    auto hasEntryPointAttr = fe->userAttributes.contains(s___EntryPoint.get());
    if (hasEntryPointAttr) {
      // Hack already enforce that there is only one func marked with entryPoint.
      always_assert(m_entryPointId == kInvalidId);
      m_entryPointId = fe->id();
    }
  }

  m_entryPointIdCalculated = true;
}

void UnitEmitter::setEntryPointIdCalculated() {
  m_entryPointIdCalculated = true;
  if (!debug) return;

  for (auto& fe : m_fes) {
    auto isEntryPoint = m_entryPointId == fe->id();
    auto hasEntryPointAttr = fe->userAttributes.contains(s___EntryPoint.get());
    always_assert(isEntryPoint == hasEntryPointAttr);
  }
}

Id UnitEmitter::getEntryPointId() const {
  always_assert(m_entryPointIdCalculated);
  return m_entryPointId;
}

void UnitEmitter::finish() {
  calculateEntryPointId();
  if (m_attributes.strictPackage) {
    for (auto const pce : preclasses()) {
      pce->setAttrs(pce->attrs() | AttrInStrictPackage);
    }
  }
  assertx(m_fatalUnit || (isSystemLib() == (m_extension != nullptr)));
}

///////////////////////////////////////////////////////////////////////////////
// Package Info.

const PackageInfo& UnitEmitter::getPackageInfo() const {
  return m_packageInfo;
}

const StaticString
  s_PackageOverride("__PackageOverride");

const StringData* UnitEmitter::getPackageOverride() const {
  auto const attr = m_fileAttributes.find(s_PackageOverride.get());
  if (attr != m_fileAttributes.end()) {
    assertx(tvIsVec(attr->second) && val(attr->second).parr->size() == 1);
    return tvAssertStringLike(val(attr->second).parr->at(int64_t(0)));
  }
  return nullptr;
}

///////////////////////////////////////////////////////////////////////////////
// Initialization and execution.

ServiceData::ExportedCounter* g_hhbc_size =
  ServiceData::createCounter("vm.hhbc-size");

bool UnitEmitter::check(bool verbose) const {
  return Verifier::checkUnit(
    this,
    verbose ? Verifier::kVerbose : Verifier::kStderr
  );
}

bool needs_extended_line_table() {
  return Cfg::Repo::DebugInfo &&
    (Cfg::Eval::DumpHhas ||
     Cfg::Debugger::EnableHphpd ||
     Cfg::Debugger::EnableVSDebugger ||
     Cfg::Debugger::EnableServer);
}

std::unique_ptr<Unit> UnitEmitter::create() const {
  INC_TPC(unit_load);

  tracing::BlockNoTrace _{"unit-create"};

  static const bool kVerify = debug || Cfg::Eval::Verify ||
    Cfg::Eval::VerifyOnly;
  static const bool kVerifyVerboseSystem =
    getenv("HHVM_VERIFY_VERBOSE_SYSTEM");
  static const bool kVerifyVerbose =
    kVerifyVerboseSystem || getenv("HHVM_VERIFY_VERBOSE");

  const bool isSystemLib_ = isSystemLib();
  const bool doVerify = kVerify || boost::ends_with(m_filepath->data(), ".hhas");

  // Systemlib is verified during build so we don't need to do it here.
  if (!isSystemLib_ && doVerify) {
    // The verifier needs the bytecode available, but we don't want to
    // necessarily force it to load (otherwise it would defeat the
    // point of lazy loading when we're using the verifier). So, load
    // the bytecode for all functions, but reset it back to the tokens
    // when we're done.
    std::lock_guard lock{m_verifyLock};
    std::vector<std::pair<FuncEmitter*, Func::BCPtr::Token>> tokens;
    SCOPE_EXIT {
      for (auto& p : tokens) {
        p.first->setBcToken(p.second, p.first->bcPos());
      }
    };
    if (Cfg::Repo::Authoritative) {
      for (auto& fe : m_fes) {
        auto const token = fe->loadBc(m_sn);
        if (!token) continue;
        tokens.emplace_back(std::make_pair(fe.get(), *token));
      }
      for (auto& pce : m_pceVec) {
        for (auto& fe : pce->methods()) {
          auto const token = fe->loadBc(m_sn);
          if (!token) continue;
          tokens.emplace_back(std::make_pair(fe, *token));
        }
      }
    }

    auto const verbose = kVerifyVerbose;
    if (!check(verbose)) {
      if (!verbose) {
        std::cerr << fmt::format(
          "Verification failed for unit {}. Re-run with "
          "HHVM_VERIFY_VERBOSE{}=1 to see more details.\n",
          m_filepath->data(), isSystemLib_ ? "_SYSTEM" : ""
        );
      }
      if (Cfg::Eval::VerifyOnly) {
        if (!isSystemLib_) {
          std::fflush(stdout);
          _Exit(1);
        }
      } else if (Cfg::Eval::FatalOnVerifyError) {
        return createFatalUnit(
          const_cast<StringData*>(m_filepath),
          m_sha1,
          FatalOp::Parse,
          "A bytecode verification error was detected"
        )->create();
      }
    }
    if (!isSystemLib_ && Cfg::Eval::VerifyOnly) {
      std::fflush(stdout);
      _Exit(0);
    }
  }

  std::unique_ptr<Unit> u {
    Cfg::Repo::Authoritative && !Cfg::Sandbox::Mode ?
      new Unit : new UnitExtended
  };

  if (m_fatalUnit) {
    FatalInfo info{m_fatalLoc, m_fatalOp, m_fatalMsg};
    u->m_fatalInfo = std::make_unique<FatalInfo>(info);
  }

  u->m_sn = m_sn;
  u->m_origFilepath = m_filepath;
  u->m_moduleName = m_moduleName;
  u->m_sha1 = m_sha1;
  u->m_bcSha1 = m_bcSha1;
  u->m_extension = m_extension;
  for (auto const& pce : m_pceVec) {
    auto const preCls = pce->create(*u);
    u->m_preClasses.emplace_back(PreClassPtr{preCls});
    auto const DEBUG_ONLY emplaced =
      u->m_nameToPreClass.emplace(preCls->name(), PreClassPtr{preCls});
    assertx(emplaced.second);
  }
  for (auto const& te : m_typeAliases) {
    u->m_typeAliases.push_back(te->create(*u));
  }
  u->m_constants = m_constants;
  u->m_metaData = m_metaData;
  u->m_fileAttributes = m_fileAttributes;
  u->m_softDeployedRepoOnly = m_softDeployedRepoOnly;
  u->m_raiseStrictPackageDynamicClassLoadError =
    m_attributes.raiseDynamicClassLoadError;
  u->m_ICE = m_ICE;
  u->m_deps = m_deps;

  u->m_modules.reserve(m_modules.size());
  for (auto const& m : m_modules) u->m_modules.emplace_back(m);

  u->m_litstrs.reserve(m_litstrs.size());
  for (auto const& s : m_litstrs) {
    assertx(s->isToken() || !s->ptr() || s->ptr()->isStatic());
    assertx(IMPLIES(s->isToken(), Cfg::Repo::Authoritative));
    u->m_litstrs.emplace_back(s);
  }
  u->m_arrays.reserve(m_arrays.size());
  for (auto const& a : m_arrays) {
    assertx(a->isToken() || a->ptr()->isStatic());
    assertx(IMPLIES(a->isToken(), Cfg::Repo::Authoritative));
    u->m_arrays.emplace_back(a);
  }
  u->m_rats.reserve(m_rats.size());
  for (auto const& a : m_rats) {
    assertx(IMPLIES(a->isToken(), Cfg::Repo::Authoritative));
    u->m_rats.emplace_back(a);
  }

  DEBUG_ONLY size_t ix = 0;
  for (auto& fe : m_fes) {
    auto const func = fe->create(*u, nullptr);
    assertx(ix == fe->id());
    u->m_funcs.push_back(func);
    ix++;
  }

  u->m_entryPointId = getEntryPointId();

  if (u->m_extended) {
    auto ux = u->getExtended();

    // If prefetching is enabled, store the symbol refs in the Unit so
    // the prefetcher can claim them. Reset the atomic flag to mark
    // them available. Otherwise set the atomic flag was already
    // claimed as a shortcut.
    if (!Cfg::Repo::Authoritative && speculativeUnitPrefetchingEnabled()) {
      ux->m_symbolRefsForPrefetch = m_symbol_refs;
      ux->m_symbolRefsPrefetched.clear();
    } else {
      ux->m_symbolRefsPrefetched.test_and_set();
    }
  }

  if (Cfg::Eval::DumpHhas > 1 ||
    (!isSystemLib_ && Cfg::Eval::DumpHhas == 1)) {
    auto const& hhaspath = Cfg::Eval::DumpHhasToFile;
    if (!hhaspath.empty()) {
      static std::atomic<bool> first_unit{true};
      auto const flags = O_WRONLY | O_CREAT | (first_unit ? O_TRUNC : O_APPEND);
      if (!folly::writeFile(disassemble(u.get()), hhaspath.c_str(), flags)) {
        Logger::Error("Failed to write hhas to %s", hhaspath.c_str());
        _Exit(HPHP_EXIT_FAILURE);
      }
      first_unit = false;
    } else {
      std::printf("%s", disassemble(u.get()).c_str());
      std::fflush(stdout);
    }
    if (!isSystemLib_) {
      _Exit(0);
    }
  }

  if (Cfg::Eval::DumpBytecode) {
    // Dump human-readable bytecode.
    Trace::traceRelease("%s", u->toString().c_str());
  }

  return u;
}

///////////////////////////////////////////////////////////////////////////////

template <typename SerDe>
void UnitEmitter::serde(SerDe& sd, bool lazy) {
  assertx(IMPLIES(lazy, Cfg::Repo::Authoritative));
  assertx(IMPLIES(!SerDe::deserializing, !lazy));

  MemoryManager::SuppressOOM so{*tl_heap};

  auto is_systemlib = isSystemLib();
  if (is_systemlib) lazy = false;

  // Have SerDe use this unit's string/array table for encoding or decoding.
  assertx(!BlobEncoderHelper<const StringData*>::tl_unitEmitter);
  assertx(!BlobEncoderHelper<const ArrayData*>::tl_unitEmitter);
  BlobEncoderHelper<const StringData*>::tl_unitEmitter = this;
  BlobEncoderHelper<const ArrayData*>::tl_unitEmitter = this;
  SCOPE_EXIT {
    assertx(BlobEncoderHelper<const StringData*>::tl_unitEmitter == this);
    assertx(BlobEncoderHelper<const ArrayData*>::tl_unitEmitter == this);
    BlobEncoderHelper<const StringData*>::tl_unitEmitter = nullptr;
    BlobEncoderHelper<const ArrayData*>::tl_unitEmitter = nullptr;
  };

  auto const seq = [&] (auto const& c, auto const& r, auto const& w) {
    if constexpr (SerDe::deserializing) {
      size_t size;
      sd(size);
      for (size_t i = 0; i < size; ++i) r(sd, i);
    } else {
      sd(c.size());
      for (auto const& x : c) w(sd, x);
    }
  };

  auto const serdeFuncEmitters = [&] (auto& funcs, auto create) {
    seq(
      funcs,
      [&] (auto& sd, size_t i) {
        int sn;
        PackedStringPtr name;
        sd(sn);
        sd(name);

        auto fe = create(name, sn, i);
        fe->serde(sd, lazy);
        fe->setEHTabIsSorted();
        fe->finish();
      },
      [&] (auto& sd, auto& fe) {
        sd(fe->sn());
        sd(fe->name);
        fe->serde(sd, false);
      }
    );
  };

  auto const serdeMethods = [&] (PreClassEmitter* pce) {
    serdeFuncEmitters(
      pce->methods(),
      [&](auto& name, auto sn, auto /* i */) {
        auto fe = newMethodEmitter(name, pce, sn);
        auto const added UNUSED = pce->addMethod(fe);
        assertx(added);
        return fe;
      }
    );
  };

  // Using the unit's string table adds an ordering dependency on
  // serialization. When encoding, we need to encode the string table
  // last. This is because encoding everything else may add to the
  // string table. However, when decoding, we need to decode the
  // string table first. This is because the rest of the decoding will
  // query the string table. Use BlobEncoder/BlobDecoder's alternate
  // mechanism for this. When encoding, the second lambda will be
  // called last. When decoding, the second lambda will be called
  // first.
  sd.alternate(
    [&] {
      sd(m_metaData)
        (m_fileAttributes)
        (m_moduleName)
        (m_packageInfo)
        (m_attributes)
        (m_symbol_refs)
        (m_bcSha1)
        (m_fatalUnit)
        (m_entryPointId)
        (m_deps)
        (m_softDeployedRepoOnly)
        (m_ICE)
        (m_missingSyms)
        (m_errorSyms);

      if constexpr (SerDe::deserializing) {
        std::string ext_name;
        sd(ext_name);
        m_extension = [&]() -> Extension* {
          if (ext_name.empty()) return nullptr;
          auto const ext = ExtensionRegistry::get(ext_name.c_str(), false);
          always_assert(ext);
          return ext;
        }();
      } else {
        auto const ext_name = m_extension
          ? std::string{m_extension->getName()}
          : std::string{};
        sd(ext_name);
      }

      if (m_fatalUnit) {
        sd(m_fatalLoc)
          (m_fatalOp)
          (m_fatalMsg);
      }

      if (!is_systemlib && Cfg::Eval::LoadFilepathFromUnitCache) {
        assertx(!Cfg::Repo::Authoritative);
        /* May be different than the unit origin: e.g. for hhas files. */
        sd(m_filepath);
      }

      if constexpr (SerDe::deserializing) {
        setEntryPointIdCalculated();
      } else {
        assertx(m_entryPointIdCalculated);
      }

      // RAT arrays
      seq(
        m_rats,
        [&] (auto& sd, size_t i) {
          if (lazy && Cfg::Repo::LitstrLazyLoad) {
            assertx(m_rats.size() == i);
            m_rats.emplace_back(RATArrayOrToken::FromToken(sd.advanced()));
            sd.skipWithSize();
          } else {
            const RepoAuthType::Array* array = nullptr;
            sd.withSize(
              [&] { array = RepoAuthType::Array::deserialize(sd); }
            );
            auto const id UNUSED = mergeRATArray(array);
            assertx(id == i);
          }
        },
        [&] (auto& sd, auto const& wrapper) {
          sd.withSize([&] { wrapper->ptr()->serialize(sd); });
        }
      );

      serdeFuncEmitters(
        m_fes,
        [&](auto& name, auto sn, auto i) {
          auto fe = newFuncEmitter(name, sn);
          assertx(fe->id() == i);
          return fe;
        }
      );

      // Pre-class emitters
      seq(
        m_pceVec,
        [&] (auto& sd, size_t i) {
          std::string name;
          sd(name);
          auto pce = newPreClassEmitter(name);
          pce->serdeMetaData(sd);
          serdeMethods(pce);
        },
        [&] (auto& sd, PreClassEmitter* pce) {
          sd(pce->name()->toCppString());
          pce->serdeMetaData(sd);
          serdeMethods(pce);
        }
      );

      // Type aliases
      seq(
        m_typeAliases,
        [&] (auto& sd, size_t i) {
          std::string name;
          sd(name);
          auto te = newTypeAliasEmitter(name);
          te->serdeMetaData(sd);
          assertx(te->id() == i);
        },
        [&] (auto& sd, const std::unique_ptr<TypeAliasEmitter>& te) {
          sd(te->name()->toCppString());
          te->serdeMetaData(sd);
        }
      );

      // Constants
      seq(
        m_constants,
        [&] (auto& sd, size_t i) {
          Constant cns;
          sd(cns.name);
          sd(cns);
          auto const id UNUSED = addConstant(cns);
          assertx(id == i);
        },
        [&] (auto& sd, const Constant& cns) {
          sd(cns.name);
          sd(cns);
        }
      );

      // Modules
      seq(
        m_modules,
        [&] (auto& sd, size_t i) {
          Module m;
          sd(m.name);
          sd(m);
          auto const id UNUSED = addModule(m);
          assertx(id == i);
        },
        [&] (auto& sd, const Module& m) {
          sd(m.name);
          sd(m);
        }
      );
    },
    [&] {
      // Serializing arrays can add new things to the literal string
      // table, so the string table needs to be encoded last. However,
      // when deserializing, we need to load the string table first.
      sd.alternate(
        [&] {
          // Arrays
          assertx(BlobEncoderHelper<const ArrayData*>::tl_unitEmitter == this);
          assertx(!BlobEncoderHelper<const ArrayData*>::tl_defer);
          BlobEncoderHelper<const ArrayData*>::tl_defer = true;
          SCOPE_EXIT {
            assertx(BlobEncoderHelper<const ArrayData*>::tl_defer);
            BlobEncoderHelper<const ArrayData*>::tl_defer = false;
          };

          if constexpr (SerDe::deserializing) {
            size_t count = 0;
            // Start by encoding each array as if it would be lazy
            // loaded.
            sd.readWithLazyCount(
              [&] {
                assertx(m_arrays.size() == count);
                m_arrays.emplace_back(ArrayOrToken::FromToken(sd.advanced()));
                sd.skipWithSize32();
                ++count;
              }
            );

            if (!lazy || !Cfg::Repo::LitstrLazyLoad) {
              // If we're not using lazy loading, call lookupArray to
              // triger the load of every array. We set
              // m_litarrayBuffer to ensure lookupArray knows where to
              // find the raw array data (without it, it would try to
              // use the repo).
              assertx(!m_litarrayBuffer);
              assertx(!m_litarrayBufferSize);
              m_litarrayBuffer = reinterpret_cast<const char*>(sd.start());
              m_litarrayBufferSize = sd.end() - sd.start();
              SCOPE_EXIT {
                m_litarrayBuffer = nullptr;
                m_litarrayBufferSize = 0;
              };
              for (size_t i = 0; i < count; ++i) lookupArrayId(i);
            }
          } else {
            sd.lazyCount(
              [&] {
                for (size_t i = 0; i < m_arrays.size(); ++i) {
                  // NB: Serializing here may increase the size of
                  // m_arrays.
                  sd.withSize32([&] { sd(m_arrays[i]->ptr()); });
                }
                return m_arrays.size();
              }
            );
          }
        },
        [&] {
          // Literal strings

          // Don't access the string table here since we're populating
          // it.
          assertx(BlobEncoderHelper<const StringData*>::tl_unitEmitter == this);
          BlobEncoderHelper<const StringData*>::tl_unitEmitter = nullptr;
          SCOPE_EXIT {
            assertx(!BlobEncoderHelper<const StringData*>::tl_unitEmitter);
            BlobEncoderHelper<const StringData*>::tl_unitEmitter = this;
          };

          seq(
            m_litstrs,
            [&] (auto& sd, size_t i) {
              if (lazy && Cfg::Repo::LitstrLazyLoad) {
                assertx(m_litstrs.size() == i);
                // When lazy loading, check if the string corresponds to
                // an already existing static string. If so, put that in
                // the table directly. This avoids redundant loads later
                // from the repo at very little cost, especially for very
                // common strings which are almost certainly going to be
                // loaded already. If not, just record the offset for
                // later loading (if necessary).
                auto const offset = sd.advanced();
                auto const sp =
                  BlobEncoderHelper<const StringData*>::asStringPiece(sd);
                if (!sp.data()) {
                  m_litstrs.emplace_back(StringOrToken::FromPtr(nullptr));
                } else if (sp.size() == 0) {
                  m_litstrs.emplace_back(StringOrToken::FromPtr(staticEmptyString()));
                } else if (auto const sd = lookupStaticString(sp)) {
                  m_litstrs.emplace_back(StringOrToken::FromPtr(sd));
                } else {
                  m_litstrs.emplace_back(StringOrToken::FromToken(offset));
                }
              } else {
                const StringData* s;
                sd(s);
                auto const id UNUSED = mergeLitstr(s);
                assertx(id == i);
              }
            },
            [&] (auto& sd, auto const& wrapper) { sd(wrapper->ptr()); }
          );
        }
      );
    }
  );
}

template void UnitEmitter::serde<>(BlobDecoder&, bool);
template void UnitEmitter::serde<>(BlobEncoder&, bool);

std::unique_ptr<UnitEmitter>
createFatalUnit(const StringData* filename, const SHA1& sha1, FatalOp op,
                std::string err, Location::Range loc) {
  auto ue = std::make_unique<UnitEmitter>(
    sha1,
    SHA1{},
    RepoOptions::defaults().packageInfo(),
    UnitEmitterAttributes::defaults()
  );
  ue->m_filepath = filename;

  ue->m_fatalUnit = true;
  ue->m_fatalLoc = loc;
  ue->m_fatalOp = op;
  ue->m_fatalMsg = err;

  ue->finish();
  return ue;
}

///////////////////////////////////////////////////////////////////////////////
}
