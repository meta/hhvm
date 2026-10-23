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

#ifndef incl_HPHP_VM_CLASS_INL_H_
#error "class-inl.h should only be included by class.h"
#endif

#include <folly/Random.h>

#include "hphp/runtime/base/runtime-error.h"
#include "hphp/runtime/base/strings.h"
#include "hphp/runtime/vm/named-entity.h"

#include "hphp/util/configs/repo.h"
#include "hphp/util/configs/sandbox.h"
#include "hphp/util/random.h"

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

inline bool Class::isZombie() const {
  return !m_cachedClass.bound();
}


inline bool Class::validate() const {
#ifndef NDEBUG
  assertx(m_magic == kMagic);
#endif
  assertx(name()->checkSane());
  return true;
}

inline const ClassId Class::classId() const {
  assertx(!m_classId.isInvalid());
  return m_classId;
}

///////////////////////////////////////////////////////////////////////////////
// Class::PropInitVec.

template <bool is_const>
Class::PropInitVec::iterator_impl<is_const>::iterator_impl(
  tv_iter_t tv,
  bit_iter_t bit
  ) : m_val(tv), m_bit(bit) {}

template <bool is_const>
bool Class::PropInitVec::iterator_impl<is_const>::operator==(
  const Class::PropInitVec::iterator_impl<is_const>& o
) const {
  return m_val == o.m_val;
}

template <bool is_const>
bool Class::PropInitVec::iterator_impl<is_const>::operator!=(
  const Class::PropInitVec::iterator_impl<is_const>& o
  ) const {
  return !(*this == o);
}

template <bool is_const>
Class::PropInitVec::iterator_impl<is_const>&
Class::PropInitVec::iterator_impl<is_const>::operator++() {
  ++m_bit;
  ++m_val;
  return *this;
}

template <bool is_const>
Class::PropInitVec::iterator_impl<is_const>
Class::PropInitVec::iterator_impl<is_const>::operator++(int) {
  auto const ret = *this;
  ++(*this);
  return ret;
}

template <bool is_const>
Class::PropInitVec::Entry<is_const>
Class::PropInitVec::iterator_impl<is_const>::operator*() const {
  return Entry<is_const>{m_val, *m_bit};
}

template <bool is_const>
Class::PropInitVec::Entry<is_const>
Class::PropInitVec::iterator_impl<is_const>::operator->() const {
  return *(*this);
}

inline Class::PropInitVec::PropInitVec() : m_data(nullptr),
                                           m_size(0),
                                           m_capacity(0) {}

template <typename T>
inline Class::PropInitVec::Entry<false>
Class::PropInitVec::operator[](T i) {
  auto lval = m_data->at(i);
  auto deepInit = deepInitBits()[i];
  return Entry<false>{lval, deepInit};
}

template <typename T>
inline Class::PropInitVec::Entry<true>
Class::PropInitVec::operator[](T i) const {
  auto lval = m_data->at(i);
  auto deepInit = deepInitBits()[i];
  return Entry<true>{lval, deepInit};
}

inline Class::PropInitVec::iterator Class::PropInitVec::begin() {
  return iterator{m_data->iteratorAt(0),
                  deepInitBits().iteratorAt(0)};
}

inline Class::PropInitVec::iterator Class::PropInitVec::end() {
  return iterator{m_data->iteratorAt(m_size),
                  deepInitBits().iteratorAt(m_size)};
}

inline Class::PropInitVec::const_iterator Class::PropInitVec::cbegin() const {
  return const_iterator{
    const_cast<const ObjectProps*>(m_data)->iteratorAt(0),
      deepInitBits().iteratorAt(0)};
}

inline Class::PropInitVec::const_iterator Class::PropInitVec::cend() const {
  return const_iterator{
    const_cast<const ObjectProps*>(m_data)->iteratorAt(m_size),
      deepInitBits().iteratorAt(m_size)};
}

inline size_t Class::PropInitVec::size() const {
  return m_size;
}

inline bool Class::PropInitVec::reqAllocated() const {
  return m_capacity < 0;
}

inline const ObjectProps* Class::PropInitVec::data() const {
  return m_data;
}

inline BitsetView<false> Class::PropInitVec::deepInitBits() {
  auto const cap = m_capacity < 0 ? ~m_capacity : m_capacity;
  return BitsetView<false>{reinterpret_cast<unsigned char*>(m_data) +
                           ObjectProps::sizeFor(cap)};
}

inline BitsetView<true>
Class::PropInitVec::deepInitBits() const {
  auto const cap = m_capacity < 0 ? ~m_capacity : m_capacity;
  return BitsetView<true>{reinterpret_cast<const unsigned char*>(m_data) +
                          ObjectProps::sizeFor(cap)};
}


///////////////////////////////////////////////////////////////////////////////
// Pre- and post-allocations.

inline PackedPtr<Func>* Class::funcVec() const {
  return reinterpret_cast<PackedPtr<Func>*>(
    reinterpret_cast<uintptr_t>(this) -
    m_funcVecLen * sizeof(PackedPtr<Func>)
  );
}

inline void* Class::mallocPtr() const {
  return reinterpret_cast<void*>(
    reinterpret_cast<uintptr_t>(funcVec()) & ~(alignof(Class) - 1)
  );
}

inline const void* Class::mallocEnd() const {
  return reinterpret_cast<const char*>(this)
         + Class::classVecOff()
         + classVecLen() * sizeof(*classVec());
}

inline const PackedPtr<Class>* Class::classVec() const {
  return m_classVec;
}

inline Class::classVecLen_t Class::classVecLen() const {
  return m_classVecLen;
}

///////////////////////////////////////////////////////////////////////////////
// Ancestry.

namespace {
inline bool isConcreteNormalClass(const Class* cls) {
  auto constexpr disallowed =
    AttrAbstract | AttrEnum | AttrEnumClass | AttrInterface | AttrTrait;
  return !(cls->attrs() & disallowed);
}
}

inline bool Class::classofNonIFace(const Class* cls) const {
  assertx(!(cls->attrs() & AttrInterface));
  if (m_classVecLen >= cls->m_classVecLen) {
    return (m_classVec[cls->m_classVecLen-1] == cls);
  }
  return false;
}

inline bool Class::classof(const Class* cls) const {
  auto const bit = cls->m_instanceBitsIndex.load(std::memory_order_acquire);
  assertx(bit == kNoInstanceBit || kProfileInstanceBit || bit > 0);
  if (bit > 0) {
    return m_instanceBits.test(bit);
  }

  if (this == cls) return true;

  // If `cls' is an interface, we can simply check to see if cls is in
  // this->m_interfaces.  Otherwise, if `this' is not an interface, the
  // classVec check will determine whether it's an instance of cls (including
  // the case where this and cls are the same trait).  Otherwise, `this' is an
  // interface, and `cls' is not, so we need to return false.  But the classVec
  // check can never return true in that case (cls's classVec contains only
  // non-interfaces, while this->classVec is either empty, or contains
  // interfaces).
  if (UNLIKELY(isInterface(cls))) {
    auto const slot = cls->preClass()->ifaceVtableSlot();
    if (slot != kInvalidSlot && isConcreteNormalClass(this)) {
      assertx(Cfg::Repo::Authoritative);
      auto const ok = slot < m_vtableVecLen && m_vtableVec[slot].iface == cls;
      assertx(ok == (m_interfaces.lookupDefault(cls->name(), nullptr) == cls));
      return ok;
    }
    return m_interfaces.lookupDefault(cls->name(), nullptr) == cls;
  }
  return classofNonIFace(cls);
}

inline bool Class::ifaceofDirect(const StringData* name) const {
  return m_interfaces.contains(name);
}

///////////////////////////////////////////////////////////////////////////////
// Basic info.

inline const StringData* Class::name() const {
  return m_preClass->name();
}

inline const PreClass* Class::preClass() const {
  return m_preClass.get();
}

inline Class* Class::parent() const {
  return m_parent.get();
}

inline StrNR Class::nameStr() const {
  return m_preClass->nameStr();
}

inline StrNR Class::parentStr() const {
  return m_preClass->parentStr();
}

inline Attr Class::attrs() const {
  assertx(Attr(m_attrCopy) == m_preClass->attrs());
  return Attr(m_attrCopy);
}

inline bool Class::rtAttribute(RuntimeAttribute a) const {
  return m_RTAttrs & a;
}

inline void Class::initRTAttributes(uint8_t a) {
  m_RTAttrs |= a;
}

inline bool Class::isPersistent() const {
  return attrs() & AttrPersistent;
}

inline bool Class::isInternal() const {
  return attrs() & AttrInternal;
}

inline bool Class::isDynamicallyConstructible() const {
  return attrs() & AttrDynamicallyConstructible;
}

inline bool Class::isDynamicallyReferenced() const {
  return attrs() & AttrDynamicallyReferenced;
}

inline bool Class::isInStrictPackage() const {
  return attrs() & AttrInStrictPackage;
}

inline Optional<int64_t> Class::dynConstructSampleRate() const {
  auto const rate = preClass()->dynConstructSampleRate();
  if (rate < 0) return {};
  return rate;
}


///////////////////////////////////////////////////////////////////////////////
// Magic methods.

inline const Func* Class::getCtor() const {
  return m_ctor;
}

inline const Func* Class::getToString() const {
  return m_toString;
}

inline const Func* Class::get86pinit() const {
  return m_pinitVec.back();
}

inline const Func* Class::get86sinit() const {
  return m_sinitVec.back();
}

inline const Func* Class::get86linit() const {
  return m_linitVec.back();
}

///////////////////////////////////////////////////////////////////////////////
// Builtin classes.

inline bool Class::isBuiltin() const {
  return attrs() & AttrBuiltin;
}

template <bool Unlocked>
inline BuiltinCtorFunction Class::instanceCtor() const {
  return Unlocked ? m_extra->m_instanceCtorUnlocked : m_extra->m_instanceCtor;
}

inline BuiltinDtorFunction Class::instanceDtor() const {
  return m_extra->m_instanceDtor;
}

///////////////////////////////////////////////////////////////////////////////
// Object release.

inline ObjReleaseFunc Class::releaseFunc() const {
  return m_releaseFunc;
}

inline uint32_t Class::memoSize() const {
  return m_memoSize;
}

inline uint8_t Class::sizeIdx() const {
  return m_sizeIdx;
}

///////////////////////////////////////////////////////////////////////////////
// Methods.

inline size_t Class::numMethods() const {
  return m_methods.size();
}

inline Func* Class::getMethod(Slot idx) const {
  assertx(idx < numMethods());
  auto funcVec = (PackedPtr<Func>*)this;
  return funcVec[-((int32_t)idx + 1)];
}

inline void Class::setMethod(Slot idx, Func* func) {
  assertx(idx < numMethods());
  auto funcVec = (PackedPtr<Func>*)this;
  funcVec[-((int32_t)idx + 1)] = func;
}

inline Func* Class::getMethodSafe(Slot idx) const {
  if (idx >= numMethods()) return nullptr;
  return getMethod(idx);
}

inline Func* Class::getIfaceMethodSafe(Slot vtableIdx, Slot methodIdx) const {
  if (vtableIdx >= m_vtableVecLen) return nullptr;
  auto const& vtable = m_vtableVec[vtableIdx];
  if (vtable.iface == nullptr) return nullptr;
  if (methodIdx >= vtable.iface->numMethods()) return nullptr;
  return vtable.vtable[methodIdx];
}

inline Func* Class::lookupMethod(const StringData* methName) const {
  Slot* idx = m_methods.find(methName);
  if (!idx) return nullptr;
  return getMethod(*idx);
}

///////////////////////////////////////////////////////////////////////////////
// Property metadata.

inline size_t Class::numDeclProperties() const {
  return m_declProperties.size();
}

inline size_t Class::numStaticProperties() const {
  return m_staticProperties.size();
}

inline uint32_t Class::declPropNumAccessible() const {
  return m_declPropNumAccessible;
}

inline folly::Range<const Class::Prop*> Class::declProperties() const {
  return m_declProperties.range();
}

inline folly::Range<const Class::SProp*> Class::staticProperties() const {
  return m_staticProperties.range();
}

inline Slot Class::lookupDeclProp(const StringData* propName) const {
  return m_declProperties.findIndex(propName);
}

inline Slot Class::lookupSProp(const StringData* sPropName) const {
  return m_staticProperties.findIndex(sPropName);
}

inline Slot Class::lookupReifiedInitProp() const {
  return m_declProperties.findIndex(s_86reified_prop.get());
}

inline bool Class::hasReifiedGenerics() const {
  return m_allFlags.m_hasReifiedGenerics;
}

inline bool Class::hasReifiedParent() const {
  return m_allFlags.m_hasReifiedParent;
}

inline RepoAuthType Class::declPropRepoAuthType(Slot index) const {
  return m_declProperties[index].repoAuthType;
}

inline RepoAuthType Class::staticPropRepoAuthType(Slot index) const {
  return m_staticProperties[index].repoAuthType;
}

inline const TypeIntersectionConstraint& Class::declPropTypeConstraint(
  Slot index) const {
  return m_declProperties[index].typeConstraints;
}

inline const TypeIntersectionConstraint& Class::staticPropTypeConstraint(
  Slot index) const {
  return m_staticProperties[index].typeConstraints;
}

inline bool Class::hasDeepInitProps() const {
  return m_allFlags.m_hasDeepInitProps;
}

inline bool Class::forbidsDynamicProps() const {
  return attrs() & AttrForbidDynamicProps;
}

///////////////////////////////////////////////////////////////////////////////
// Property initialization.

inline bool Class::needInitialization() const {
  return m_allFlags.m_needInitialization;
}

inline bool Class::maybeRedefinesPropTypes() const {
  return m_allFlags.m_maybeRedefsPropTy;
}

inline bool Class::needsPropInitialValueCheck() const {
  return m_allFlags.m_needsPropInitialCheck;
}

inline const Class::PropInitVec& Class::declPropInit() const {
  return m_declPropInit;
}

inline const VMFixedVector<const Func*>& Class::pinitVec() const {
  return m_pinitVec;
}

inline const VMFixedVector<const Func*>& Class::sinitVec() const {
  return m_sinitVec;
}

inline const VMFixedVector<const Func*>& Class::linitVec() const {
  return m_linitVec;
}

inline rds::Handle Class::checkedPropTypeRedefinesHandle() const {
  assertx(m_allFlags.m_maybeRedefsPropTy);
  m_extra->m_checkedPropTypeRedefs.bind(
    rds::Mode::Normal,
    rds::LinkName{"PropTypeRedefs", name()}
  );
  return m_extra->m_checkedPropTypeRedefs.handle();
}

inline rds::Handle Class::checkedPropInitialValuesHandle() const {
  assertx(m_allFlags.m_needsPropInitialCheck);
  m_extra->m_checkedPropInitialValues.bind(
    rds::Mode::Normal,
    rds::LinkName{"PropInitialValues", name()}
  );
  return m_extra->m_checkedPropInitialValues.handle();
}

///////////////////////////////////////////////////////////////////////////////
// Property storage.

inline void Class::initPropHandle() const {
  m_propDataCache.bind(
    rds::Mode::Normal,
    rds::LinkName{"PropDataCache", name()}
  );
}

inline rds::Handle Class::propHandle() const {
  return m_propDataCache.handle();
}

inline rds::Handle Class::sPropInitHandle() const {
  return m_sPropCacheInit.handle();
}

inline rds::Handle Class::sPropHandle(Slot index) const {
  return sPropLink(index).handle();
}

inline rds::Link<StaticPropData, rds::Mode::NonNormal>
Class::sPropLink(Slot index) const {
  assertx(m_sPropCacheInit.bound());
  assertx(numStaticProperties() > index);
  return m_sPropCache[index];
}

inline bool Class::declaredOnThisClass(const SProp& sProp) const {
  return sProp.cls == this || (sProp.attrs & AttrLSB);
}

inline bool Class::inherited(const SProp& parentProp) const {
  return !(parentProp.attrs & AttrPrivate) ||
    (parentProp.attrs & AttrLSB);
}

///////////////////////////////////////////////////////////////////////////////
// Constants.

inline size_t Class::numConstants() const {
  return m_constants.size();
}

inline const Class::Const* Class::constants() const {
  return m_constants.accessList();
}

inline bool Class::hasConstant(const StringData* clsCnsName) const {
  // m_constants.contains(clsCnsName) returns abstract constants
  auto clsCnsInd = m_constants.findIndex(clsCnsName);
  return (clsCnsInd != kInvalidSlot) &&
    !m_constants[clsCnsInd].isAbstract() &&
    m_constants[clsCnsInd].kind() == ConstModifierFlags::Kind::Value;
}

inline bool Class::hasTypeConstant(const StringData* typeConstName,
                                   bool includeAbs) const {
  auto typeConstInd = m_constants.findIndex(typeConstName);
  return (typeConstInd != kInvalidSlot) &&
    (!m_constants[typeConstInd].isAbstractAndUninit() || includeAbs) &&
    m_constants[typeConstInd].kind() == ConstModifierFlags::Kind::Type;
}

///////////////////////////////////////////////////////////////////////////////
// Interfaces and traits.

inline folly::Range<const ClassPtr*> Class::declInterfaces() const {
  return folly::range(m_declInterfaces.begin(),
                      m_declInterfaces.end());
}

inline const Class::InterfaceMap& Class::allInterfaces() const {
  return m_interfaces;
}

inline const bool Class::hasIncludedEnums() const {
  return m_extra && (m_extra->m_includedEnums.size() != 0);
}

inline const Class::IncludedEnumMap& Class::allIncludedEnums() const {
  return m_extra->m_includedEnums;
}

inline Slot Class::traitsBeginIdx() const {
  return m_extra->m_traitsBeginIdx;
}

inline Slot Class::traitsEndIdx() const   {
  return m_extra->m_traitsEndIdx;
}

inline const VMCompactVector<ClassPtr>& Class::usedTraitClasses() const {
  return m_extra->m_usedTraits;
}

inline const Class::RequirementMap& Class::allRequirements() const {
  return m_requirements;
}

///////////////////////////////////////////////////////////////////////////////
// Instance bits.

inline bool Class::checkInstanceBit(unsigned int bit) const {
  assertx(bit > 0);
  return m_instanceBits[bit];
}

inline void Class::incInstanceCheckCount(uint64_t inc) const {
  if (inc & 1) ++inc;
  uint64_t curr = m_instanceCheckCount.load(std::memory_order_acquire);
  while (true) {
    if (curr & 1) return;               // no longer used as a counter
    auto updated = curr + inc;
    if (m_instanceCheckCount.compare_exchange_weak(
            curr, updated, std::memory_order_acq_rel)) {
      return;
    }
  }
}

inline uint64_t Class::getInstanceCheckCount() const {
  assertx(m_instanceBitsIndex.load() == kProfileInstanceBit);
  auto res = m_instanceCheckCount.load(std::memory_order_acquire);
  assertx((res & 1) == 0);
  return res;
}

///////////////////////////////////////////////////////////////////////////////
// Throwable initialization.

inline bool Class::needsInitThrowable() const {
  return m_allFlags.m_needsInitThrowable;
}

///////////////////////////////////////////////////////////////////////////////
// JIT data.

inline rds::Handle Class::classHandle() const {
  return m_cachedClass.handle();
}

inline rds::Link<ClassId, rds::Mode::Normal> Class::classIdLink() const {
  auto const sym = rds::LinkName{"ClassId", name()};
  return rds::bind<ClassId, rds::Mode::Normal>(sym);
}

inline rds::Handle Class::classIdHandle() const {
  return classIdLink().handle();
}

inline void Class::setClassHandle(rds::Link<PackedPtr<Class>,
                                  rds::Mode::NonLocal> link) const {
  assertx(!m_cachedClass.bound());
  m_cachedClass = link;

}

inline Class* Class::getCached() const {
  return m_cachedClass.isInit() ? *m_cachedClass : nullptr;
}

inline void Class::setCached() {
  m_cachedClass.initWith(this);
  if (isPersistent()) return;
  classIdLink().initWith(classId());
}

///////////////////////////////////////////////////////////////////////////////
// Native data.

inline const Native::NativeDataInfo* Class::getNativeDataInfo() const {
  return m_extra->m_nativeDataInfo;
}

///////////////////////////////////////////////////////////////////////////////
// Closure subclasses.

inline bool Class::isScopedClosure() const {
  return m_scoped;
}

inline const Class::ScopedClonesMap& Class::scopedClones() const {
  return m_extra->m_scopedClones;
}

/////////////////////////////////////////////////////////////////////////////
// Memoization

inline size_t Class::numMemoSlots() const {
  return m_extra->m_nextMemoSlot;
}

inline bool Class::hasMemoSlots() const {
  return numMemoSlots() > 0;
}

inline std::pair<Slot, bool> Class::memoSlotForFunc(FuncId func) const {
  assertx(hasMemoSlots());
  auto const it = m_extra->m_memoMappings.find(func);
  if (it != m_extra->m_memoMappings.end()) return it->second;
  // Each mapping is only stored in the class which defines it, so recurse up to
  // the parent. We should only be calling this with functions which have a memo
  // slot, so assert if we reach the end without finding a slot.
  if (m_parent) return m_parent->memoSlotForFunc(func);
  always_assert(false);
}

inline FuncId Class::funcForMemoSlot(Slot slot) const {
  if (!hasMemoSlots()) return FuncId::Invalid;
  if (slot >= numMemoSlots()) return FuncId::Invalid;
  Slot parentNextSlot = (m_parent && m_parent->hasMemoSlots())
                        ? m_parent->numMemoSlots()
                        : 0;
  if (slot < parentNextSlot) {
    return m_parent->funcForMemoSlot(slot);
  }
  for (const auto& mapping : m_extra->m_memoMappings) {
    if (mapping.second.first == slot) {
      return mapping.first;
    }
  }

  return FuncId::Invalid;
}

///////////////////////////////////////////////////////////////////////////////
// Other methods.

inline TypeConstraint Class::enumBaseTy() const {
  return m_extra->m_enumBaseTy;
}

inline EnumValues* Class::getEnumValues() const {
  return m_extra->m_enumValues.load(std::memory_order_acquire);
}

///////////////////////////////////////////////////////////////////////////////
// ExtraData.

inline void Class::allocExtraData() const {
  m_extra.ensureAllocated();
}

inline bool Class::hasExtraData() const {
  return static_cast<bool>(m_extra);
}

///////////////////////////////////////////////////////////////////////////////
// Non-member functions.

inline Attr classKindAsAttr(ClassKind kind) {
  return static_cast<Attr>(kind);
}

inline bool isTrait(const Class* cls) {
  return cls->attrs() & AttrTrait;
}

inline bool isEnum(const Class* cls) {
  return cls->attrs() & AttrEnum;
}

inline bool isEnumClass(const Class* cls) {
  return cls->attrs() & AttrEnumClass;
}

inline bool isAnyEnum(const Class* cls) {
  return isEnum(cls) || isEnumClass(cls);
}

inline bool isInterface(const Class* cls) {
  return cls->attrs() & AttrInterface;
}

inline bool isNormalClass(const Class* cls) {
  return !(cls->attrs() & (AttrTrait | AttrInterface | AttrEnum |
                           AttrEnumClass));
}

inline bool isAbstract(const Class* cls) {
  return cls->attrs() & AttrAbstract;
}

inline bool classHasPersistentRDS(const Class* cls) {
  auto const persistent = cls != nullptr &&
    rds::isPersistentHandle(cls->classHandle());
  assertx(!cls || persistent == cls->isPersistent());
  return persistent;
}

inline const StringData* classToStringHelper(const Class* cls,
                                             const char* source) {
  if (folly::Random::oneIn(Cfg::Eval::RaiseClassConversionNoticeSampleRate, threadLocalRng64())) {
    raise_class_to_string_conversion_notice(source);
 }
 return cls->name();
}

///////////////////////////////////////////////////////////////////////////////
// Lookup.

inline Class* Class::lookup(const StringData* name) {
  return get(name, false);
}

 /*
 * Check whether a Class* can be trusted to not change in the given context.
 * We can use a Class* if:
 * (1) Its persistent. It will never change.
 * (2) We are currently jitting a translation that will be invalidated
 *     whenever the Class* changes.
 *     For method translations, we need to check that loading class ctx
 *         will also load cls. see Class::avail()
 *     For function translations, these are only invalidated when the unit
 *         they are defined in changes. Therefore, we'd need to ensure that
 *         all classes loaded as a result of loading cls are contained inside
 *         the unit.
 *     As a conservative approximation of this, we are currently just checking
 *     if ctx is a subtype of cls, but we can certainly do better.
 */

inline const Class* Class::lookupKnown(const Class* cls, const Class* ctx) {
  auto const res = lookupKnownMaybe(cls, ctx);
  switch (res.tag) {
    case Class::ClassLookupResult::None:
    case Class::ClassLookupResult::Maybe:
      return nullptr;
    case Class::ClassLookupResult::Exact:
      return res.cls;
  }
}

inline Class::ClassLookup Class::lookupKnownMaybe(const Class* cls,
                                                  const Class* ctx) {
  auto const tag = [&]() {
    if (UNLIKELY(cls == nullptr)) return Class::ClassLookupResult::None;
    if (cls->attrs() & AttrPersistent) return Class::ClassLookupResult::Exact;
    if (ctx && ctx->classof(cls)) return Class::ClassLookupResult::Exact;
    if (Cfg::Sandbox::Speculate) return ClassLookupResult::Maybe;
    return Class::ClassLookupResult::None;
  }();
  return Class::ClassLookup { tag, cls };
}

inline const Class* Class::lookupKnown(const NamedType* ne,
                                       const Class* ctx) {
  Class* cls = ne->clsList();
  return lookupKnown(cls, ctx);
}

inline const Class* Class::lookupKnown(const StringData* name,
                                       const Class* ctx) {
  return lookupKnown(NamedType::getOrCreate(name), ctx);
}

inline Class* Class::load(const StringData* name) {
  return get(name, true);
}

inline Class* Class::get(const StringData* name, bool tryAutoload) {
  if (name->isSymbol()) {
    if (auto const result = name->getCachedClass()) return result;
  }
  auto const orig = name;
  OptString normStr;
  auto ne = NamedType::getNoCreate(name, &normStr);

  if (!ne && !tryAutoload) return nullptr;
  if (normStr) name = normStr.get();

  auto const result = get(ne, name, tryAutoload);
  if (orig->isSymbol() && result && classHasPersistentRDS(result)) {
    const_cast<StringData*>(orig)->setCachedClass(result);
  }
  return result;
}

///////////////////////////////////////////////////////////////////////////////
}
