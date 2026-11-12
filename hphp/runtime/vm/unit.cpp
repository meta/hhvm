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

#include "hphp/runtime/vm/unit.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <sstream>
#include <vector>

#include <boost/algorithm/string/predicate.hpp>
#include <fmt/format.h>

#include <folly/MapUtil.h>
#include <folly/String.h>

#include "hphp/system/systemlib.h"
#include "hphp/util/alloc.h"
#include "hphp/util/assertions.h"
#include "hphp/util/configs/debugger.h"
#include "hphp/util/configs/eval.h"
#include "hphp/util/configs/jit.h"
#include "hphp/util/configs/repo.h"
#include "hphp/util/configs/server.h"
#include "hphp/util/lock.h"
#include "hphp/util/mutex.h"
#include "hphp/util/struct-log.h"

#include "hphp/runtime/base/attr.h"
#include "hphp/runtime/base/autoload-handler.h"
#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/base/array-iterator.h"
#include "hphp/runtime/base/execution-context.h"
#include "hphp/runtime/base/file-util.h"
#include "hphp/runtime/base/stats.h"
#include "hphp/runtime/base/string-data.h"
#include "hphp/runtime/base/type-array.h"
#include "hphp/runtime/base/type-string.h"
#include "hphp/runtime/base/type-variant.h"
#include "hphp/runtime/base/typed-value.h"
#include "hphp/runtime/base/unit-cache.h"
#include "hphp/runtime/base/vanilla-vec.h"

#include "hphp/runtime/debugger/debugger.h"

#include "hphp/runtime/vm/bytecode.h"
#include "hphp/runtime/vm/class.h"
#include "hphp/runtime/vm/func.h"
#include "hphp/runtime/vm/hh-utils.h"
#include "hphp/runtime/vm/named-entity.h"
#include "hphp/runtime/vm/named-entity-defs.h"
#include "hphp/runtime/vm/preclass.h"
#include "hphp/runtime/vm/preclass-emitter.h"
#include "hphp/runtime/vm/reverse-data-map.h"
#include "hphp/runtime/vm/type-alias.h"
#include "hphp/runtime/vm/unit-emitter.h"
#include "hphp/runtime/vm/unit-token.h"
#include "hphp/runtime/vm/unit-util.h"

#include "hphp/runtime/server/memory-stats.h"

#include "hphp/runtime/ext/server/ext_server.h"
#include "hphp/runtime/ext/vsdebug/debugger.h"
#include "hphp/runtime/ext/vsdebug/ext_vsdebug.h"

#include "hphp/runtime/base/program-functions.h"

namespace HPHP {

//////////////////////////////////////////////////////////////////////

TRACE_SET_MOD(hhbc)

//////////////////////////////////////////////////////////////////////

std::atomic<size_t> Unit::s_createdUnits{0};
std::atomic<size_t> Unit::s_liveUnits{0};

///////////////////////////////////////////////////////////////////////////////
// Construction and destruction.

Unit::Unit()
  : m_interpretOnly(false)
  , m_extended(false)
  , m_ICE(false)
  , m_raiseStrictPackageDynamicClassLoadError(false)
{
  ++s_createdUnits;
  ++s_liveUnits;
}

Unit::~Unit() {
  if (Cfg::Eval::EnableReverseDataMap &&
      m_mergeState.load(std::memory_order_acquire) != MergeState::Unmerged) {
    // Units are registered to data_map in Unit::initialMerge().
    data_map::deregister(this);
  }

  // A Unit is only destroyed without startDestroy() if it was never used, so
  // no UnitToken can have been handed out for it.
  UnitToken::assertNoValidToken(this);

  for (auto const func : funcs()) func->finishDestroy();

  // ExecutionContext and the TC may retain references to Class'es, so
  // it is possible for Class'es to outlive their Unit.
  for (auto const& pcls : m_preClasses) {
    Class* cls = pcls->namedType()->clsList();
    while (cls) {
      Class* cur = cls;
      cls = cls->m_next;
      if (cur->preClass() == pcls.get()) {
        cur->destroy();
      }
    }
  }

  --s_liveUnits;
}

void* Unit::operator new(size_t sz) {
  MemoryStats::LogAlloc(AllocKind::Unit, sz);
  return vm_malloc(sz);
}

void Unit::operator delete(void* p, size_t /*sz*/) {
  vm_free(p);
}

void Unit::operator delete(Unit* p, std::destroying_delete_t) {
  if (p->m_extended) {
    static_cast<UnitExtended*>(p)->~UnitExtended();
  } else {
    p->~Unit();
  }
  vm_free(p);
}

void Unit::startDestroy() {
  m_zombie.store(true, std::memory_order_release);
  UnitToken::setInvalid(this);

  for (auto const func : funcs()) func->startDestroy();
  for (auto const& pcls : m_preClasses) {
    Class* cls = pcls->namedType()->clsList();
    while (cls) {
      Class* cur = cls;
      cls = cls->m_next;
      if (cur->preClass() == pcls.get()) {
        for (size_t i = 0; i < cur->numMethods(); i++) {
          if (auto meth = cur->getMethod(i)) {
            if (meth->cls() == cur) {
              meth->startDestroy();
            }
          }
        }
      }
    }
  }
  Treadmill::enqueue(
    [this] {
      delete this;
    }
  );
}

std::shared_ptr<UnitToken> Unit::getUnitToken() const {
  return UnitToken::get(this);
}

///////////////////////////////////////////////////////////////////////////////
// Code locations.

bool Unit::getOffsetRanges(int line, OffsetFuncRangeVec& offsets) const {
  assertx(offsets.size() == 0);
  forEachFunc([&](const Func* func) {
    // Quickly ignore functions that can't have that line in them
    if (line < func->line1() || line > func->line2()) return false;

    auto map = func->getLineToOffsetRangeVecMap();
    auto it = map.find(line);
    if (it != map.end()) {
      offsets.push_back(std::pair(func, it->second));
    }
    return false;
  });
  return offsets.size() != 0;
}

bool Unit::isCoverageEnabled() const {
  return m_coverage.bound() && m_coverage.isInit();
}
void Unit::enableCoverage() {
  if (!m_coverage.bound()) {
    assertx(!Cfg::Repo::Authoritative && Cfg::Eval::EnablePerFileCoverage);
    m_coverage.bind(
      rds::Mode::Normal,
      rds::LinkName{"UnitCoverage", origFilepath()}
    );
  }
  if (m_coverage.isInit()) return;
  new (m_coverage.get()) req::unique_ptr<req::dynamic_bitset>{};
  m_coverage.markInit();
}
void Unit::disableCoverage() {
  if (!isCoverageEnabled()) return;

  m_coverage.markUninit();
  m_coverage->~unique_ptr();
}
void Unit::clearCoverage() {
  assertx(isCoverageEnabled());
  auto& ptr = *m_coverage;
  if (ptr) ptr->reset();
}
void Unit::recordCoverage(int line) {
  assertx(isCoverageEnabled());

  if (line == -1) return;

  auto& ptr = *m_coverage;
  if (!ptr) ptr = req::make_unique<req::dynamic_bitset>();
  if (ptr->size() <= line) ptr->resize(line + 1);
  ptr->set(line);
}
Array Unit::reportCoverage() const {
  assertx(isCoverageEnabled());

  auto& ptr = *m_coverage;
  if (!ptr) return empty_vec_array();

  auto const& c = *ptr;
  auto const end = req::dynamic_bitset::npos;
  VecInit init{ptr->count()};
  for (auto i = c.find_first(); i != end; i = c.find_next(i)) {
    init.append(i);
  }

  return init.toArray();
}
rds::Handle Unit::coverageDataHandle() const {
  if (!m_coverage.bound()) {
    assertx(!Cfg::Repo::Authoritative && Cfg::Eval::EnablePerFileCoverage);
    m_coverage.bind(
      rds::Mode::Normal,
      rds::LinkName{"UnitCoverage", origFilepath()}
    );
  }
  return m_coverage.handle();
}

///////////////////////////////////////////////////////////////////////////////
// Funcs and PreClasses.

Func* Unit::getEntryPoint() const {
  if (m_entryPointId == kInvalidId) {
    return nullptr;
  }
  return lookupFuncId(m_entryPointId);
}

///////////////////////////////////////////////////////////////////////////////
// Literal strings.

StringData* Unit::lookupLitstrId(Id id) const {
  assertx(id >= 0 && id < m_litstrs.size());

  auto& elem = m_litstrs[id];
  auto wrapper = elem.copy();
  if (wrapper.isPtr()) {
    assertx(!wrapper.ptr() || wrapper.ptr()->isStatic());
    return const_cast<StringData*>(wrapper.ptr());
  }
  auto lock = elem.lock_for_update();
  wrapper = elem.copy();
  if (wrapper.isPtr()) {
    assertx(!wrapper.ptr() || wrapper.ptr()->isStatic());
    return const_cast<StringData*>(wrapper.ptr());
  }
  auto const str = UnitEmitter::loadLitstrFromRepo(m_sn, wrapper.token(), true);
  assertx(!str || str->isStatic());
  lock.update(StringOrToken::FromPtr(str));
  return const_cast<StringData*>(str);
}

///////////////////////////////////////////////////////////////////////////////
// Literal arrays.

const ArrayData* Unit::lookupArrayId(Id id) const {
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

  auto const oldStrUnit = BlobEncoderHelper<const StringData*>::tl_unit;
  auto const oldArrUnit = BlobEncoderHelper<const ArrayData*>::tl_unit;

  BlobEncoderHelper<const StringData*>::tl_unit = const_cast<Unit*>(this);
  BlobEncoderHelper<const ArrayData*>::tl_unit = const_cast<Unit*>(this);
  SCOPE_EXIT {
    assertx(BlobEncoderHelper<const StringData*>::tl_unit == this);
    assertx(BlobEncoderHelper<const ArrayData*>::tl_unit == this);
    BlobEncoderHelper<const StringData*>::tl_unit = oldStrUnit;
    BlobEncoderHelper<const ArrayData*>::tl_unit = oldArrUnit;
  };

  auto const array =
    UnitEmitter::loadLitarrayFromRepo(m_sn, wrapper.token(), true);
  assertx(array);
  assertx(array->isStatic());
  lock.update(ArrayOrToken::FromPtr(array));
  return array;
}

///////////////////////////////////////////////////////////////////////////////
// RAT arrays.

const RepoAuthType::Array* Unit::lookupRATArray(Id id) const {
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

  assertx(!BlobEncoderHelper<const StringData*>::tl_unit);
  BlobEncoderHelper<const StringData*>::tl_unit = const_cast<Unit*>(this);
  SCOPE_EXIT {
    assertx(BlobEncoderHelper<const StringData*>::tl_unit == this);
    BlobEncoderHelper<const StringData*>::tl_unit = nullptr;
  };

  auto const array = UnitEmitter::loadRATArrayFromRepo(m_sn, wrapper.token());
  assertx(array);
  lock.update(RATArrayOrToken::FromPtr(array));
  return array;
}

///////////////////////////////////////////////////////////////////////////////
// Merge.

namespace {
///////////////////////////////////////////////////////////////////////////////

SimpleMutex unitInitLock(false /* reentrant */, RankUnitInit);
std::atomic<uint64_t> s_loadedUnits{0};

bool isEvalName(const StringData* name) {
  return name->empty() || boost::ends_with(name->slice(), EVAL_FILENAME_SUFFIX);
}

///////////////////////////////////////////////////////////////////////////////
}

void Unit::initialMerge() {
  unitInitLock.assertOwnedBySelf();
  if (m_mergeState.load(std::memory_order_acquire) != MergeState::Unmerged) {
    return;
  }

  auto const nrecord = Cfg::Eval::RecordFirstUnits;
  if (s_loadedUnits.load(std::memory_order_acquire) < nrecord) {
    auto const index = s_loadedUnits.fetch_add(1, std::memory_order_acq_rel);
    if (index < nrecord) {
      StructuredLogEntry ent;
      ent.setStr("path", m_origFilepath->data());
      ent.setInt("index", index);
      if (Cfg::Server::Mode) {
        ent.setInt("uptime", HHVM_FN(server_uptime)());
      }
      StructuredLog::log("hhvm_first_units", ent);
    }
  }

  if (Cfg::Eval::EnableReverseDataMap) {
    data_map::register_start(this);
  }

  // Promote eligible constants to persistent RDS while still under the
  // initial-merge lock (which every request must pass through before it can
  // finish merging this unit). Doing it here -- rather than at per-request merge
  // time -- means the persistent handle is bound and AttrPersistent is recorded
  // together, exactly once, so a constant's attributes always match its handle's
  // mode and there is no race on the shared Constant.
  if (Cfg::Eval::ForceAllPersistent && Cfg::Eval::ConstantPersistence) {
    for (auto& constant : m_constants) {
      if (!(constant.attrs & AttrPersistent)) {
        Constant::tryPromotePersistent(&constant);
      }
    }
  }

  m_mergeState.store(MergeState::InitialMerged, std::memory_order_release);
}

namespace {

using PType = std::variant<PreClass*, PreTypeAlias*>;
using SymCache = hphp_fast_map<
  const StringData*,
  PType,
  string_data_hash,
  string_data_tsame
>;

struct LoadContext {
  SymCache syms;
  bool isSystem{false};
  AutoloadMap* am{nullptr};
  std::vector<Unit*> unmerged;
  hphp_fast_set<PType> loaded;
};

////////////////////////////////////////////////////////////////////////////////

Optional<PType> addUnit(LoadContext& ctx, Unit* d, const StringData* name) {
  Optional<PType> ret;
  for (auto& pcls : d->preclasses()) {
    if (!PreClassEmitter::IsAnonymousClassName(pcls->name()->slice())) {
      ctx.syms.emplace(pcls->name(), pcls.get());
      if (name && pcls->name()->tsame(name)) ret = pcls.get();
    }
  }

  for (auto& pty : d->typeAliases()) {
    ctx.syms.emplace(pty.name, &pty);
    if (name && pty.name->tsame(name)) ret = &pty;
  }

  // We will need to finish fulling loading this unit now that we're merging a
  // symbol from it.
  ctx.unmerged.emplace_back(d);
  return ret;
}

Optional<PType> findPreType(LoadContext& ctx, const StringData* name) {
  auto const symIt = ctx.syms.find(name);
  if (symIt != ctx.syms.end()) return symIt->second;

  // SystemLib should have no external dependencies so ignore AutoloadMap init.
  // Initializing the autoload map before we the first unit in the repo can
  // cause us to cache a null map for the duration of the request.
  if (ctx.isSystem) return {};

  // Lazily compute the autoload map- if all symbols are local then avoid a
  // potentially expensive facts initialization.
  if (!ctx.am) {
    ctx.am = AutoloadHandler::s_instance->getAutoloadMap();
    if (!ctx.am) return {};
  }

  auto const p = ctx.am->getTypeOrTypeAliasFile(StrNR{name});
  if (!p) return {};

  bool initial = true;
  auto const d = lookupUnit(p->m_path.get(), g_context->getCwd().data(),
                            &initial, nullptr, true, false, true);

  // If the unit was already loaded the symbol should have been defined already,
  // this implies we don't know where to find the symbol.
  if (!d || !initial) return {};
  return addUnit(ctx, d, name);
}

void loadTypeSafe(LoadContext& ctx, const StringData* name, PType initial) {
  std::vector<PType> stack;
  hphp_fast_set<PType> visited;

  auto const exists = [&] (const StringData* p) {
    if (p->isSymbol() && p->getCachedClass()) return true;
    auto ne = NamedType::getNoCreate(p);
    return ne && (ne->getCachedClass() || ne->getCachedTypeAlias());
  };

  auto const def = [&] (PType p) {
    if (!ctx.loaded.emplace(p).second) return;
    match<void>(
      p,
      [&] (PreClass* pc)      { return Class::def(pc, true); },
      [&] (PreTypeAlias* pta) { return TypeAlias::def(pta, true); }
    );
  };

  auto const load = [&] (const StringData* p) {
    if (!p || p->empty() || exists(p)) return;

    // If the symbol isn't available or a cycle is detected continue on and
    // allow the error to be encountered later when def() is called with
    // failIsFatal.
    auto ty = findPreType(ctx, p);
    if (ty && !visited.count(*ty)) stack.emplace_back(*ty);
  };

  // If the symbol already exists trigger an error unless we were the ones who
  // loaded it.
  if (exists(name)) def(initial);
  stack.emplace_back(initial);

  while (!stack.empty()) {
    auto p = stack.back();
    if (visited.emplace(p).second) {
      match<void>(
        p,
        [&] (PreClass* pc) {
          if (auto const parent = pc->parent())    load(parent);
          for (auto const i : pc->interfaces())    load(i);
          for (auto const e : pc->includedEnums()) load(e);
          for (auto const t : pc->usedTraits())    load(t);
          if (pc->enumBaseTy().isUnresolved()) {
            load(pc->enumBaseTy().typeName());
          }
        },
        [&] (PreTypeAlias* pta) {
          if (pta->value.isUnresolved()) {
            for (auto& tc : eachTypeConstraintInUnion(pta->value)) {
              if (tc.isUnresolved()) load(tc.typeName());
            }
          }
        }
      );
    } else {
      stack.pop_back();
      def(p);
    }
  }
}

void mergeUnitSafe(LoadContext& ctx, Unit* u) {
  if (!u->preMerge()) return;

  for (auto const& pc : u->preclasses()) {
    if (!PreClassEmitter::IsAnonymousClassName(pc->name()->slice())) {
      loadTypeSafe(ctx, pc->name(), pc.get());
    }
  }
  for (auto& pta : u->typeAliases()) loadTypeSafe(ctx, pta.name, &pta);

  u->finalMerge();
}

void mergeUnitSafe(Unit* u) {
  LoadContext ctx;
  addUnit(ctx, u, nullptr);
  ctx.isSystem = u->isSystemLib();

  // Disallow further autoloading while performing a safe merge. The merge
  // algorithm is responsible for querying the autoload map for symbols while
  // in this state. Calling into the autoloader could trigger reentry into the
  // mergeUnitSafe() stack.
  AutoloadHandler::Inhibit _;

  for (size_t i = 0; i < ctx.unmerged.size(); ++i) {
    mergeUnitSafe(ctx, ctx.unmerged[i]);
  }
}

}

bool Unit::preMerge() {
  auto mergeState = m_mergeState.load(std::memory_order_acquire);
  if (mergeState == MergeState::Merged) return false;

  if (m_fatalInfo) {
    raise_parse_error(filepath(),
                      m_fatalInfo->m_fatalMsg.c_str(),
                      m_fatalInfo->m_fatalLoc);
  }

  if (m_softDeployedRepoOnly) {
    raise_warning("File %s is soft-deployed, but its unit was merged", filepath()->data());
  }

  if (mergeState == MergeState::Unmerged) {
    SimpleLock lock(unitInitLock);
    initialMerge();
    mergeState = m_mergeState.load(std::memory_order_acquire);
    assertx(mergeState >= MergeState::InitialMerged);
  }

  if (mergeState == MergeState::InitialMerged) {
    mergeImpl<false, true>();
  }

  if (mergeState == MergeState::NeedsNonPersistentMerged) {
    if (isSystemLib()) {
      mergeImpl<true /* mergeOnlyNonPersistentFuncs */>();
      assertx(!Cfg::Repo::Authoritative && Cfg::Jit::EnableRenameFunction);
    } else {
      mergeImpl<false, true>();
      assertx(!Cfg::Repo::Authoritative);
    }
  }

  return mergeState != MergeState::Merged;
}

void Unit::finalMerge() {
  assertx(m_mergeState.load(std::memory_order_acquire) != MergeState::Unmerged);

  if (Cfg::Eval::LogDeclDeps) {
    logDeclInfo();

    auto const thisPath = filepath();
    auto const& options = RepoOptions::forFile(thisPath->data());
    auto const dir = options.dir();
    auto const relPath = std::filesystem::relative(
      std::filesystem::path(thisPath->data()),
      dir
    );

    for (auto& [path, sha] : m_deps) {
      auto const attributes = UnitEmitterAttributes::forRepoRelativePath(
        path, options.flags()
      );
      auto hash = SHA1{
        mangleUnitSha1(sha.toString(), path, options.flags(), attributes)
      };
      auto fpath = makeStaticString(dir / path);
      g_context->m_loadedRdepMap[fpath].emplace_back(relPath, hash);
    }
  }

  if (Cfg::Repo::Authoritative ||
      (isSystemLib() && !Cfg::Jit::EnableRenameFunction)) {
    m_mergeState.store(MergeState::Merged, std::memory_order_release);
  } else {
    m_mergeState.store(MergeState::NeedsNonPersistentMerged, std::memory_order_release);
  }
}

void Unit::merge() {
  auto mergeState = m_mergeState.load(std::memory_order_acquire);
  if (mergeState == MergeState::Merged) return;

  if (Cfg::Eval::UseTopologicalMerge &&
      (!isSystemLib() || mergeState != MergeState::NeedsNonPersistentMerged)) {
    return mergeUnitSafe(this);
  }

  if (m_fatalInfo) {
    raise_parse_error(filepath(),
                      m_fatalInfo->m_fatalMsg.c_str(),
                      m_fatalInfo->m_fatalLoc);
  }

  if (m_softDeployedRepoOnly) {
    raise_warning("File %s is soft-deployed, but its unit was merged", filepath()->data());
  }

  if (mergeState == MergeState::Unmerged) {
    SimpleLock lock(unitInitLock);
    initialMerge();
    mergeState = m_mergeState.load(std::memory_order_acquire);
    assertx(mergeState >= MergeState::InitialMerged);
  }

  if (mergeState == MergeState::InitialMerged) {
    mergeImpl<false>();
  }

  if (mergeState == MergeState::NeedsNonPersistentMerged) {
    if (isSystemLib()) {
      mergeImpl<true /* mergeOnlyNonPersistentFuncs */>();
      assertx(!Cfg::Repo::Authoritative && Cfg::Jit::EnableRenameFunction);
    } else {
      mergeImpl<false>();
      assertx(!Cfg::Repo::Authoritative);
    }
  }

  if (Cfg::Eval::LogDeclDeps) {
    logDeclInfo();

    auto const thisPath = filepath();
    auto const& options = RepoOptions::forFile(thisPath->data());
    auto const dir = options.dir();
    auto const relPath = std::filesystem::relative(
      std::filesystem::path(thisPath->data()),
      dir
    );

    for (auto& [path, sha] : m_deps) {
      auto const attributes = UnitEmitterAttributes::forRepoRelativePath(
        path, options.flags()
      );
      auto hash = SHA1{
        mangleUnitSha1(sha.toString(), path, options.flags(), attributes)
      };
      auto fpath = makeStaticString(dir / path);
      g_context->m_loadedRdepMap[fpath].emplace_back(relPath, hash);
    }
  }

  if (Cfg::Repo::Authoritative ||
    (isSystemLib() && !Cfg::Jit::EnableRenameFunction)) {
    m_mergeState.store(MergeState::Merged, std::memory_order_release);
  } else {
    m_mergeState.store(MergeState::NeedsNonPersistentMerged, std::memory_order_release);
  }
}

void Unit::logTearing(int64_t nsecs) {
  assertx(!Cfg::Repo::Authoritative);
  assertx(Cfg::Eval::SampleRequestTearing);

  auto const repoOptions = g_context->getRepoOptionsForRequest();
  auto repoRoot = repoOptions->dir();

  assertx(!isSystemLib());
  StructuredLogEntry ent;

  auto const tpath = [&] () -> std::string {
    auto const orig = std::filesystem::path{origFilepath()->data()};
    auto const origNative = orig.native();
    if (repoRoot.native().size() > origNative.size() || repoRoot.empty()) {
      return origNative;
    }
    if (!std::equal(repoRoot.begin(), repoRoot.end(), orig.begin())) {
      return origNative;
    }
    return orig.lexically_relative(repoRoot).native();
  }();

  auto const debuggerCount = [&] {
    if (Cfg::Debugger::EnableHphpd) {
      return Eval::Debugger::CountConnectedProxy();
    }

    HPHP::VSDEBUG::Debugger* vspDebugger =
      HPHP::VSDEBUG::VSDebugExtension::getDebugger();
    if (vspDebugger != nullptr && vspDebugger->clientConnected()) {
      return 1;
    }

    return 0;
  }();

  auto const hash = sha1().toString();
  auto const bchash = bcSha1().toString();
  ent.setStr("filepath", tpath);
  ent.setStr("sha1", hash);
  ent.setStr("bc_sha1", bchash);
  ent.setInt("time_diff_ns", nsecs);
  ent.setInt("debuggers", debuggerCount);

  // always generate logs
  ent.force_init = Cfg::Eval::SampleRequestTearingForce;

  FTRACE(2, "Tearing in {} ({} ns)\n", tpath.data(), nsecs);

  StructuredLog::log("hhvm_sandbox_file_tearing", ent);
}

void Unit::logDeclInfo() const {
  if (!Cfg::Eval::LogAllDeclTearing &&
      !StructuredLog::coinflip(Cfg::Eval::LogDeclDeps)) return;

  auto const thisPath = filepath();
  auto const& options = RepoOptions::forFile(thisPath->data());
  auto const dir = options.dir();
  auto const& map = g_context->m_evaledFiles;
  std::vector<std::string> rev_tears;
  std::vector<std::string> not_loaded;
  std::vector<std::string> loaded;

  std::vector<std::tuple<std::string, std::string, SHA1>> deps;
  for (auto const& [path, sha] : m_deps) {
    auto const attributes = UnitEmitterAttributes::forRepoRelativePath(
      path, options.flags()
    );
    auto hash = SHA1{
      mangleUnitSha1(sha.toString(), path, options.flags(), attributes)
    };
    deps.emplace_back(path, dir / path, std::move(hash));
  }

  for (auto const& [path, fullpath, sha] : deps) {
    auto const full = makeStaticString(fullpath);
    if (auto const info = folly::get_ptr(map, full)) {
      if (info->unit->sha1() != sha) rev_tears.emplace_back(path);
      else loaded.emplace_back(path);
    } else {
      not_loaded.emplace_back(path);
    }
  }

  StructuredLogEntry ent;

  ent.setInt("num_loaded_deps", loaded.size() + rev_tears.size());
  ent.setInt("num_torn_deps", rev_tears.size());
  ent.setInt("num_not_loaded_deps", not_loaded.size());
  ent.setInt("num_deps", deps.size());

  auto const logVec = [&] (auto name, auto& vec) {
    std::vector<folly::StringPiece> v;
    v.reserve(vec.size());
    for (auto& s : vec) v.emplace_back(s);
    ent.setVec(name, v);
  };

  logVec("loaded_deps", loaded);
  logVec("torn_deps", rev_tears);
  logVec("not_loaded_deps", not_loaded);

  std::vector<std::string> tears;
  std::vector<std::string> non_tears;
  for (auto const& [path, sha] : g_context->m_loadedRdepMap[thisPath]) {
    if (sha != m_sha1) tears.emplace_back(path);
    else non_tears.emplace_back(path);
  }

  ent.setInt("num_loaded_rdeps", tears.size() + non_tears.size());
  ent.setInt("num_torn_rdeps", tears.size());

  logVec("torn_rdeps", tears);
  logVec("loaded_rdeps", non_tears);

  if ((Cfg::Eval::LogAllDeclTearing && (!rev_tears.empty() || !tears.empty()))) {
    ent.setInt("sample_rate", 1);
    StructuredLog::log("hhvm_decl_logging", ent);
  } else if(!Cfg::Eval::LogAllDeclTearing ||
            StructuredLog::coinflip(Cfg::Eval::LogDeclDeps)) {
    ent.setInt("sample_rate", Cfg::Eval::LogDeclDeps);
    StructuredLog::log("hhvm_decl_logging", ent);
  }
}

template <typename T, typename I>
static bool defineSymbols(
    const T& symbols,
    boost::dynamic_bitset<>& define,
    Stats::StatCounter counter,
    I lambda
) {
  bool madeProgress = false;
  for (auto i = define.find_first(); i != define.npos; i = define.find_next(i)) {
    auto& symbol = symbols[i];
    Stats::inc(Stats::UnitMerge_mergeable);
    Stats::inc(counter);
    auto res = lambda(symbol);
    if (res) {
      define.reset(i);
      madeProgress = true;
    }
  }
  return madeProgress;
}

template<bool mergeOnlyNonPersistentFuncs, bool skipClassesAndAliases>
void Unit::mergeImpl() {
  assertx(m_mergeState.load(std::memory_order_acquire) >=
          MergeState::InitialMerged);
  autoTypecheck(this);

  FTRACE(1, "Merging unit {} ({} funcs, {} constants, {} typealiases, {} classes, {} modules)\n",
         this->m_origFilepath->data(), m_funcs.size(), m_constants.size(),
         m_typeAliases.size(), m_preClasses.size(), m_modules.size());

  for (auto func : funcs()) {
    Stats::inc(Stats::UnitMerge_mergeable_function);
    if (mergeOnlyNonPersistentFuncs && func->isPersistent()) continue;
    Stats::inc(Stats::UnitMerge_mergeable_function_define);
    Func::def(func);
  }

  if (mergeOnlyNonPersistentFuncs) return;

  for (auto& constant : m_constants) {
    Stats::inc(Stats::UnitMerge_mergeable);
    Stats::inc(Stats::UnitMerge_mergeable_define);

    // sys/repo constants are persistent at compile time. Under
    // Eval.ForceAllPersistent + Eval.ConstantPersistence a concrete constant may
    // also have been promoted to persistent during initialMerge; a dynamic
    // (KindOfUninit-valued) constant never is. So a non-sys/repo persistent
    // constant must be a concrete one promoted in that mode.
    assertx(IMPLIES(this->isSystemLib() || Cfg::Repo::Authoritative,
                    constant.attrs & AttrPersistent));
    assertx(IMPLIES((constant.attrs & AttrPersistent) &&
                    !this->isSystemLib() && !Cfg::Repo::Authoritative,
                    Cfg::Eval::ForceAllPersistent &&
                    Cfg::Eval::ConstantPersistence &&
                    constant.val.m_type != KindOfUninit));
    Constant::def(&constant);
  }

  for (auto& module : m_modules) {
    Stats::inc(Stats::UnitMerge_mergeable);
    Stats::inc(Stats::UnitMerge_mergeable_define);

    assertx(IMPLIES(Cfg::Repo::Authoritative, module.attrs & AttrPersistent));
    Module::def(&module);
  }

  if (skipClassesAndAliases) return;

  boost::dynamic_bitset<> preClasses(m_preClasses.size());
  preClasses.set();
  boost::dynamic_bitset<> typeAliases(m_typeAliases.size());
  typeAliases.set();

  bool failIsFatal = false;
  do {
    bool madeProgress = defineSymbols(
      m_preClasses, preClasses, Stats::UnitMerge_mergeable_class,
      [&](const PreClassPtr& preClass) {
        // Anonymous classes doesn't need to be defined because they will be
        // defined when used
        auto pcName = preClass->name();
        if (PreClassEmitter::IsAnonymousClassName(pcName->slice())) {
          return true;
        }
        assertx(preClass->isPersistent() ==
                (Cfg::Eval::ForceAllPersistent ||
                 this->isSystemLib() || Cfg::Repo::Authoritative));
        return Class::def(preClass.get(), failIsFatal) != nullptr;
      });

    // We do type alias last because they may depend on classes that needs to
    // be defined first
    madeProgress |= defineSymbols(m_typeAliases, typeAliases,
      Stats::UnitMerge_mergeable_typealias,
      [&](const PreTypeAlias& typeAlias) {
        assertx(typeAlias.isPersistent() ==
              ((Cfg::Eval::ForceAllPersistent &&
                Cfg::Eval::TypeAliasPersistence) ||
               this->isSystemLib() || Cfg::Repo::Authoritative));
        return TypeAlias::def(&typeAlias, failIsFatal);
      });

    // If we did not make progress then there is an undefined symbol or a
    // class definition cycle that spans files.
    if (!madeProgress) failIsFatal = true;
  } while (typeAliases.any() || preClasses.any());
}

bool Unit::isSystemLib() const {
  return FileUtil::isSystemName(m_origFilepath->slice());
}

bool Unit::isPersistent() const {
  return Cfg::Repo::Authoritative || isSystemLib();
}

///////////////////////////////////////////////////////////////////////////////
// Info arrays.

namespace {

Array getClassesWithAttrInfo(Attr attrs, bool inverse = false) {
  auto builtins = Array::CreateVec();
  auto non_builtins = Array::CreateVec();
  NamedType::foreach_cached_class([&](Class* c) {
    if ((c->attrs() & attrs) ? !inverse : inverse) {
      if (c->isBuiltin()) {
        builtins.append(make_tv<KindOfPersistentString>(c->name()));
      } else {
        non_builtins.append(make_tv<KindOfPersistentString>(c->name()));
      }
    }
  });
  if (builtins.empty()) return non_builtins;
  auto all = VecInit(builtins.size() + non_builtins.size());
  for (auto i = builtins.size(); i > 0; i--) {
    all.append(builtins.lookup(safe_cast<int64_t>(i - 1)));
  }
  IterateV(non_builtins.get(), [&](auto name) { all.append(name); });
  return all.toArray();
}

template<bool system>
Array getFunctions() {
  // Return an array of all defined functions.  This method is used
  // to support get_defined_functions().
  Array a = Array::CreateVec();
  NamedFunc::foreach_cached_func([&](Func* func) {
    if ((system ^ func->isBuiltin()) || func->isGenerated()) return; //continue
    a.append(Variant(func->nameStr()));
  });
  return a;
}

///////////////////////////////////////////////////////////////////////////////
}

Array Unit::getClassesInfo() {
  return getClassesWithAttrInfo(AttrInterface | AttrTrait,
                                 /* inverse = */ true);
}

Array Unit::getInterfacesInfo() {
  return getClassesWithAttrInfo(AttrInterface);
}

Array Unit::getTraitsInfo() {
  return getClassesWithAttrInfo(AttrTrait);
}

Array Unit::getUserFunctions() {
  return getFunctions<false>();
}

Array Unit::getSystemFunctions() {
  return getFunctions<true>();
}


///////////////////////////////////////////////////////////////////////////////
// Pretty printer.

std::string Unit::toString() const {
  std::ostringstream ss;
  for (auto& pc : m_preClasses) {
    pc->prettyPrint(ss);
  }
  for (auto& func : funcs()) {
    func->prettyPrint(ss);
  }
  for (auto& cns : constants()) {
    cns.prettyPrint(ss);
  }
  for (auto& m : modules()) {
    m.prettyPrint(ss);
  }
  return ss.str();
}


///////////////////////////////////////////////////////////////////////////////
// Other methods.

std::string mangleReifiedGenericsName(const ArrayData* tsList) {
  std::vector<std::string> l;
  IterateV(
    tsList,
    [&](TypedValue v) {
      assertx(tvIsDict(v));
      auto str =
        TypeStructure::toString(ArrNR(v.m_data.parr),
          TypeStructure::TSDisplayType::TSDisplayTypeInternal).toCppString();
      str.erase(remove_if(str.begin(), str.end(), isspace), str.end());
      l.emplace_back(str);
    }
  );
  return fmt::format("<{}>", folly::join(",", l));
}

}
