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

#include "hphp/runtime/base/type-string.h"
#include "hphp/runtime/base/repo-auth-type.h"
#include "hphp/runtime/vm/hhbc-codec.h"
#include "hphp/runtime/vm/unit-util.h"

#include "hphp/util/configs/eval.h"

namespace HPHP {

///////////////////////////////////////////////////////////////////////////////
// Basic accessors.

inline UnitExtended* Unit::getExtended() {
  assertx(m_extended);
  return static_cast<UnitExtended*>(this);
}

inline SymbolRefs* Unit::claimSymbolRefsForPrefetch() {
  if (!m_extended) return nullptr; // No symbol refs
  auto extended = getExtended();
  // Atomically mark them as taken and check if they were already
  // taken
  if (extended->m_symbolRefsPrefetched.test_and_set()) return nullptr;
  return &extended->m_symbolRefsForPrefetch;
}

inline const UnitExtended* Unit::getExtended() const {
  assertx(m_extended);
  return static_cast<const UnitExtended*>(this);
}

inline const FatalInfo* Unit::getFatalInfo() const {
  return m_fatalInfo.get();
}

inline int64_t Unit::sn() const {
  return m_sn;
}

inline SHA1 Unit::sha1() const {
  return m_sha1;
}

inline SHA1 Unit::bcSha1() const {
  return m_bcSha1;
}

inline bool Unit::isICE() const {
  return m_ICE;
}

inline bool Unit::isZombie() const {
  return m_zombie.load(std::memory_order_acquire);
}

inline bool Unit::isSoftDeployedRepoOnly() const {
  assertx(Cfg::Repo::Authoritative);
  return m_softDeployedRepoOnly;
}

inline bool Unit::shouldRaiseStrictPackageDynamicClassLoadError() const {
  return m_raiseStrictPackageDynamicClassLoadError;
}

///////////////////////////////////////////////////////////////////////////////
// File paths.

inline const StringData* Unit::origFilepath() const {
  assertx(m_origFilepath);
  return m_origFilepath;
}

inline const StringData* Unit::perRequestFilepath() const {
  if (!m_extended) return nullptr;
  auto const u = getExtended();
  if (!u->m_perRequestFilepath.bound()) return nullptr;
  assertx(!Cfg::Repo::Authoritative);
  assertx(Cfg::Eval::ReuseUnitsByHash);
  if (!u->m_perRequestFilepath.isInit()) return nullptr;
  return *u->m_perRequestFilepath;
}

inline const StringData* Unit::filepath() const {
  if (auto const p = perRequestFilepath()) return p;
  return origFilepath();
}

inline rds::Handle Unit::perRequestFilepathHandle() const {
  if (!m_extended) return rds::kUninitHandle;
  return getExtended()->m_perRequestFilepath.maybeHandle();
}

inline bool Unit::hasPerRequestFilepath() const {
  return perRequestFilepathHandle() != rds::kUninitHandle;
}

inline void Unit::bindPerRequestFilepath(const StringData* p) {
  assertx(p);
  assertx(p->isStatic());
  assertx(!Cfg::Repo::Authoritative);
  assertx(Cfg::Eval::ReuseUnitsByHash);
  assertx(m_extended);
  auto u = getExtended();
  assertx(u->m_perRequestFilepath.bound());
  assertx(!u->m_perRequestFilepath.isInit());
  u->m_perRequestFilepath.initWith(p);
}

inline void Unit::makeFilepathPerRequest() {
  assertx(!Cfg::Repo::Authoritative);
  assertx(Cfg::Eval::ReuseUnitsByHash);
  assertx(m_extended);
  auto u = getExtended();
  assertx(!u->m_perRequestFilepath.bound());
  u->m_perRequestFilepath = rds::alloc<PackedStringPtr>();
}

/////////////////////////////////////////////////////////////////////////////
// Unit cache ref-counting

inline void Unit::acquireCacheRefCount() {
  assertx(!Cfg::Repo::Authoritative);
  assertx(m_extended);
  ++getExtended()->m_cacheRefCount;
}

inline bool Unit::releaseCacheRefCount() {
  assertx(!Cfg::Repo::Authoritative);
  assertx(m_extended);
  assertx(getExtended()->m_cacheRefCount > 0);
  return !(--getExtended()->m_cacheRefCount);
}

inline bool Unit::hasCacheRef() const {
  assertx(!Cfg::Repo::Authoritative);
  assertx(m_extended);
  return getExtended()->m_cacheRefCount > 0;
}

inline Unit* Unit::nextCachedByHash() const {
  assertx(!Cfg::Repo::Authoritative);
  assertx(m_extended);
  return getExtended()->m_nextCachedByHash.load(std::memory_order_acquire);
}

inline void Unit::setNextCachedByHash(Unit* u) {
  assertx(!Cfg::Repo::Authoritative);
  assertx(m_extended);
  assertx(!u || u->sha1() == sha1());
  return getExtended()->m_nextCachedByHash.store(u, std::memory_order_release);
}

///////////////////////////////////////////////////////////////////////////////
// Idle unit reaping

inline void
Unit::setLastTouchRequest(Treadmill::Clock::time_point requestStartTime) {
  assertx(!Cfg::Repo::Authoritative);
  assertx(Cfg::Eval::IdleUnitTimeoutSecs > 0);
  assertx(requestStartTime != Treadmill::kNoStartTime);
  assertx(m_extended);
  auto u = getExtended();
  auto old = u->m_lastTouchRequestStartTime.load();
  while (old < requestStartTime) {
    if (u->m_lastTouchRequestStartTime
      .compare_exchange_weak(old, requestStartTime)
    ) {
      return;
    }
  }
}

inline void Unit::setLastTouchTime(TouchClock::time_point now) {
  assertx(!Cfg::Repo::Authoritative);
  assertx(Cfg::Eval::IdleUnitTimeoutSecs > 0);
  assertx(m_extended);
  auto u = getExtended();
  auto old = u->m_lastTouchTime.load();
  while (old < now) {
    if (u->m_lastTouchTime.compare_exchange_weak(old, now)) return;
  }
}

inline std::pair<Treadmill::Clock::time_point, Unit::TouchClock::time_point>
Unit::getLastTouch() const {
  assertx(!Cfg::Repo::Authoritative);
  assertx(Cfg::Eval::IdleUnitTimeoutSecs > 0);
  assertx(m_extended);
  auto const u = getExtended();
  return std::make_pair(
    u->m_lastTouchRequestStartTime.load(),
    u->m_lastTouchTime.load()
  );
}

///////////////////////////////////////////////////////////////////////////////
// Litstrs, NamedTypes, and NamedFuncs.

inline size_t Unit::numLitstrs() const {
  return m_litstrs.size();
}

inline const NamedType* Unit::lookupNamedTypeId(Id id) const {
  return lookupNamedTypePairId(id).second;
}

inline NamedTypePair Unit::lookupNamedTypePairId(Id id) const {
  auto const name = lookupLitstrId(id);
  assertx(name);
  assertx(name->data()[0] != '\\');
  return { name, NamedType::getOrCreate(name) };
}

inline NamedFuncPair Unit::lookupNamedFuncPairId(Id id) const {
  auto const name = lookupLitstrId(id);
  assertx(name);
  assertx(name->data()[0] != '\\');
  return { name, NamedFunc::getOrCreate(name) };
}

///////////////////////////////////////////////////////////////////////////////
// Arrays.

inline size_t Unit::numArrays() const {
  return m_arrays.size();
}

///////////////////////////////////////////////////////////////////////////////
// PreClasses

inline PreClass* Unit::lookupPreClass(const StringData* name) const {
  auto const it = m_nameToPreClass.find(name);
  return it == m_nameToPreClass.end() ? nullptr : it->second.get();
}

inline folly::Range<PreClassPtr*> Unit::preclasses() {
  return { m_preClasses.data(), m_preClasses.size() };
}

inline folly::Range<const PreClassPtr*> Unit::preclasses() const {
  return { m_preClasses.data(), m_preClasses.size() };
}

///////////////////////////////////////////////////////////////////////////////
// Metadata

inline const Constant* Unit::lookupConstantId(Id id) const {
  assertx(id < Id(m_constants.size()));
  return &m_constants[id];
}

inline const Module* Unit::lookupModuleId(Id id) const {
  assertx(id < Id(m_modules.size()));
  return &m_modules[id];
}

inline const PreTypeAlias* Unit::lookupTypeAliasId(Id id) const {
  assertx(id < Id(m_typeAliases.size()));
  return &m_typeAliases[id];
}

///////////////////////////////////////////////////////////////////////////////
// Funcs

inline Func* Unit::lookupFuncId(Id id) const {
  assertx(id < Id(m_funcs.size()));
  return m_funcs[id];
}

inline folly::Range<Func**> Unit::funcs() {
  return { m_funcs.begin(), m_funcs.end() };
}

inline folly::Range<Func* const*> Unit::funcs() const {
  return { m_funcs.begin(), m_funcs.end() };
}

template<class Fn> void Unit::forEachFunc(Fn fn) const {
  for (auto& func : funcs()) {
    if (fn(func)) {
      return;
    }
  }
  for (auto& c : preclasses()) {
    for (auto& method : c->allMethods()) {
      if (fn(method)) {
        return;
      }
    }
  }
}

///////////////////////////////////////////////////////////////////////////////
// Type aliases

inline folly::Range<PreTypeAlias*> Unit::typeAliases() {
  return { m_typeAliases.begin(), m_typeAliases.end() };
}

inline folly::Range<const PreTypeAlias*> Unit::typeAliases() const {
  return { m_typeAliases.begin(), m_typeAliases.end() };
}

///////////////////////////////////////////////////////////////////////////////
// Constants

inline folly::Range<Constant*> Unit::constants() {
  return { m_constants.begin(), m_constants.end() };
}

inline folly::Range<const Constant*> Unit::constants() const {
  return { m_constants.begin(), m_constants.end() };
}

///////////////////////////////////////////////////////////////////////////////
// Modules

inline folly::Range<Module*> Unit::modules() {
  return { m_modules.begin(), m_modules.end() };
}

inline folly::Range<const Module*> Unit::modules() const {
  return { m_modules.begin(), m_modules.end() };
}

///////////////////////////////////////////////////////////////////////////////
// File attributes

inline const UserAttributeMap& Unit::fileAttributes() const {
  return m_fileAttributes;
}

///////////////////////////////////////////////////////////////////////////////
// Modules

inline const StringData* Unit::moduleName() const {
  return m_moduleName;
}

///////////////////////////////////////////////////////////////////////////////
// Merge.

inline bool Unit::isEmpty() const {
  return m_mergeState.load(std::memory_order_acquire) == MergeState::Merged;
}

///////////////////////////////////////////////////////////////////////////////
// Other methods.

inline bool Unit::isInterpretOnly() const {
  return m_interpretOnly;
}

inline void Unit::setInterpretOnly() {
  m_interpretOnly = true;
}

inline UserAttributeMap Unit::metaData() const {
  return m_metaData;
}

///////////////////////////////////////////////////////////////////////////////
}
