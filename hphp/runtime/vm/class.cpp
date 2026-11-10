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

#include "hphp/runtime/base/attr.h"
#include "hphp/runtime/base/autoload-handler.h"
#include "hphp/runtime/base/bespoke/logging-profile.h"
#include "hphp/runtime/base/coeffects-config.h"
#include "hphp/runtime/base/collections.h"
#include "hphp/runtime/base/comparisons.h"
#include "hphp/runtime/base/enum-cache.h"
#include "hphp/runtime/base/strings.h"
#include "hphp/runtime/base/tv-refcount.h"
#include "hphp/runtime/base/typed-value.h"
#include "hphp/runtime/base/type-structure.h"
#include "hphp/runtime/base/vanilla-dict.h"
#include "hphp/runtime/server/memory-stats.h"
#include "hphp/runtime/vm/class.h"
#include "hphp/runtime/vm/frame-restore.h"
#include "hphp/runtime/vm/instance-bits.h"
#include "hphp/runtime/vm/jit/irgen-minstr.h"
#include "hphp/runtime/vm/memo-cache.h"
#include "hphp/runtime/vm/named-entity-defs.h"
#include "hphp/runtime/vm/named-entity.h"
#include "hphp/runtime/vm/native-data.h"
#include "hphp/runtime/vm/native-prop-handler.h"
#include "hphp/runtime/vm/property-profile.h"
#include "hphp/runtime/vm/method-lookup.h"
#include "hphp/runtime/vm/reified-generics.h"
#include "hphp/runtime/vm/reverse-data-map.h"
#include "hphp/runtime/vm/runtime.h"
#include "hphp/runtime/vm/trait-method-import-data.h"
#include "hphp/runtime/vm/treadmill.h"
#include "hphp/runtime/vm/vm-regs.h"

#include "hphp/runtime/ext/core/ext_core_closure.h"

#include "hphp/util/alloc.h"
#include "hphp/util/check-size.h"
#include "hphp/util/configs/eval.h"
#include "hphp/util/configs/server.h"
#include "hphp/util/logger.h"

#include <fmt/format.h>
#include <folly/Bits.h>
#include <folly/synchronization/EventCount.h>

#include <algorithm>

TRACE_SET_MOD(class_load)

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

const StaticString s_86cinit("86cinit");
const StaticString s_86pinit("86pinit");
const StaticString s_86sinit("86sinit");
const StaticString s_86linit("86linit");
const StaticString s_86ctor("86ctor");
const StaticString s_86metadata("86metadata");
const StaticString s_86productAttributionData("86productAttributionData");
const StaticString s_86reified_prop("86reified_prop");
const StaticString s_86reifiedinit("86reifiedinit");
const StaticString s___MockClass("__MockClass");
const StaticString s___Reified("__Reified");
const StaticString s___ModuleLevelTrait("__ModuleLevelTrait");
const StaticString s_Awaitable("HH\\Awaitable");

int64_t constexpr kInvalidIdx = std::numeric_limits<int64_t>::max();

namespace {
constexpr size_t kNumBuckets = 8192;

struct classBucket {
  std::atomic<int64_t> inProgress{kInvalidIdx};
  folly::EventCount ec;
  Mutex mutex;
};

std::array<classBucket, kNumBuckets> s_classes;
classBucket& getBucket(const StringData* s) {
  auto const idx = Cfg::Eval::SynchronizeClassLoading
    ? s->hash() % kNumBuckets : 0;
  return s_classes[idx];
}
Mutex& getMutex(const StringData* s) {
  return getBucket(s).mutex;
}
}

static std::atomic<ClassId::Id> s_nextClassId{1};
ReadWriteMutex g_scopeCacheMutex;

///////////////////////////////////////////////////////////////////////////////

void Class::setNewClassId() {
  assertx(m_classId.isInvalid());
  m_classId = ClassId(s_nextClassId.fetch_add(1, std::memory_order_acq_rel));
}

///////////////////////////////////////////////////////////////////////////////
// Class::PropInitVec.

Class::PropInitVec::~PropInitVec() {
  if (m_capacity > 0) {
    // allocated in low heap
    low_free(m_data);
  }
}

Class::PropInitVec*
Class::PropInitVec::allocWithReqAllocator(const PropInitVec& src) {
  uint32_t size = src.m_size;
  auto const props_size = ObjectProps::sizeFor(size);
  auto const bits_size = BitsetView<true>::sizeFor(size);

  PropInitVec* p = reinterpret_cast<PropInitVec*>(req::malloc(
    sizeof(PropInitVec) + props_size + bits_size,
    type_scan::getIndexForMalloc<
      PropInitVec,
      type_scan::Action::WithSuffix<char>
    >()
  ));

  p->m_size = size;
  p->m_capacity = ~size;
  p->m_data = reinterpret_cast<ObjectProps*>(p + 1);

  // These are two distinct memcpys because the props and deep init bits
  // aren't necessarily contiguous in src (because capacity >= size)
  // but will be in the destination (which is exactly sized).
  memcpy(p->m_data, src.m_data, props_size);
  memcpy(reinterpret_cast<char*>(p->m_data) + props_size,
         src.deepInitBits().buffer(),
         bits_size);

  return p;
}

const Class::PropInitVec&
Class::PropInitVec::operator=(const PropInitVec& piv) {
  assertx(!reqAllocated());
  if (this != &piv) {
    if (UNLIKELY(m_capacity)) {
      vm_free(m_data);
      m_data = nullptr;
    }
    m_size = m_capacity = piv.size();
    auto const props_size = ObjectProps::sizeFor(m_size);
    auto const bits_size = BitsetView<true>::sizeFor(m_size);
    if (m_size == 0) return *this;

    m_data = reinterpret_cast<ObjectProps*>(
      low_malloc(props_size + bits_size)
    );
    // These are two distinct memcpys because the props and deep init bits
    // aren't necessarily contiguous in src (because capacity >= size)
    // but will be in the destination (which is exactly sized).
    memcpy(m_data, piv.m_data, props_size);
    memcpy(deepInitBits().buffer(),
           piv.deepInitBits().buffer(),
           bits_size);

    assertx(m_data);
    assertx(m_data->checkInvariants(m_capacity));
  }
  return *this;
}

void Class::PropInitVec::push_back(const TypedValue& v) {
  assertx(!reqAllocated());
  if (m_size == m_capacity) {
    unsigned newCap = folly::nextPowTwo(m_size + 1);
    auto const props_size = ObjectProps::sizeFor(newCap);
    auto const bits_size = BitsetView<true>::sizeFor(newCap);

    auto newData = reinterpret_cast<char*>(
      low_malloc(props_size + bits_size)
    );

    auto const oldSize = ObjectProps::sizeFor(m_size);

    ObjectProps::setInvariantsAfterGrow(newData, oldSize, props_size);

    if (m_data) {
      // These two memcpys are separate because we're going from
      // contiguous memory (since the size == capacity) to noncontiguous memory
      // (because the structure has grown).
      memcpy(newData, m_data, oldSize);
      memcpy(newData + props_size,
             deepInitBits().buffer(),
             (m_size + 7) / 8);
      low_free(m_data);
    }

    m_data = reinterpret_cast<ObjectProps*>(newData);
    m_capacity = static_cast<int32_t>(newCap);

    assertx(m_data);
    assertx(m_data->checkInvariants(m_capacity));
  }
  auto const idx = m_size++;
  tvDup(v, m_data->at(idx));
  deepInitBits()[idx] = false;
}


///////////////////////////////////////////////////////////////////////////////
// Class.

namespace {

template <typename T, rds::Mode M>
void unbindLink(rds::Link<T, M>* link, rds::Symbol symbol) {
  if (!link->bound()) return;
  rds::unbind(symbol, link->handle());
  *link = rds::Link<T, M>();
}

bool shouldUsePersistentHandles(const Class* cls) {
  const auto isPersistent = cls->isPersistent();
  cls->verifyPersistence();
  return isPersistent;
}

/*
 * Load used traits of PreClass `preClass', and append the trait Class*'s to
 * 'usedTraits'.  Return an estimate of the method count of all used traits.
 */
unsigned loadUsedTraits(PreClass* preClass,
                        VMCompactVector<ClassPtr>& usedTraits) {
  unsigned methodCount = 0;
  hphp_fast_set<std::pair<const StringData*, PreClass*>> seenMethods;

  auto const traitsFlattened = !!(preClass->attrs() & AttrNoExpandTrait);
  for (auto const& traitName : preClass->usedTraits()) {
    Class* classPtr = Class::load(traitName);
    if (classPtr == nullptr) {
      raise_error(Strings::TRAITS_UNKNOWN_TRAIT, traitName->data());
    }
    if (!(classPtr->attrs() & AttrTrait)) {
      raise_error("%s cannot use %s - it is not a trait",
                  preClass->name()->data(),
                  classPtr->name()->data());
    }

    preClass->enforceInMaybeSealedParentWhitelist(classPtr->preClass());
    if (traitsFlattened) {
      // In RepoAuthoritative mode (with the WholeProgram compiler
      // optimizations), the contents of traits can be flattened into
      // the preClasses of "use"r classes. Continuing here allows us
      // to avoid unnecessarily attempting to re-import trait methods
      // and properties, only to fail due to (surprise surprise!) the
      // same method/property existing on m_preClass.
      continue;
    }

    // Do not include duplicate methods due to diamond traits in methodCount.
    auto numMethods = classPtr->numMethods();
    for (Slot i = 0; i < classPtr->numMethods(); ++i) {
      auto const meth = classPtr->getMethod(i);
      auto const origin = meth->preClass();
      if (origin && (origin->attrs() & AttrTrait) &&
          !seenMethods.emplace(meth->name(), origin).second) {
        numMethods--;
      }
    }

    usedTraits.push_back(ClassPtr(classPtr));
    methodCount += numMethods;

  }

  return methodCount;
}

/*
 * Class ends with a dynamically sized array, m_classVec. C++ doesn't allow
 * declaring empty arrays like C does, so we give it size 1 and use
 * m_classVec's offset as the true size of Class when allocating memory to
 * construct one.
 */
constexpr size_t sizeof_Class = Class::classVecOff();
constexpr size_t extra_builtin_ptrs =
#ifdef FULLPTR_FOR_BUILTINS
  8;
#else
  0;
#endif

static constexpr size_t kClassSize = (debug ? 280 : 276) + extra_builtin_ptrs;
static_assert(CheckSize<sizeof_Class, kClassSize>(), "");

/*
 * For a type-constant, store the ArrayData inside the type-structure with
 * the low bit set to indicate that the type-structure is resolved
 */
void setResolvedTypeConstVal(Class::Const& cns, ArrayData* ad);

Mutex s_priority_serialize_mutex;
std::set<std::pair<int64_t, Class*>> s_priority_serialize;

}

Class* Class::newClass(PreClass* preClass, Class* parent) {
  auto const classVecLen = parent != nullptr ? parent->m_classVecLen + 1 : 1;
  auto funcVecLen = (parent != nullptr ? parent->m_methods.size() : 0)
    + preClass->numMethods();
  if (parent == nullptr && !(preClass->attrs() & AttrNoReifiedInit)) {
      // We will need to add 86reifiedinit method
      funcVecLen++;
    }

  VMCompactVector<ClassPtr> usedTraits;
  auto numTraitMethodsEstimate = loadUsedTraits(preClass, usedTraits);
  funcVecLen += numTraitMethodsEstimate;

  // We need to pad this allocation so that the actual start of the Class is
  // 8-byte aligned.
  auto const mask = alignof(Class) - 1;
  auto const funcvec_sz = sizeof(PackedPtr<Func>) * funcVecLen;
  auto const prefix_sz = (funcvec_sz + mask) & ~mask;

  auto const size = sizeof_Class + prefix_sz
                    + sizeof(m_classVec[0]) * classVecLen;

  auto const mem = low_malloc(size);
  MemoryStats::LogAlloc(AllocKind::Class, size);
  auto const classPtr = reinterpret_cast<void*>(
    reinterpret_cast<uintptr_t>(mem) + prefix_sz
  );
  // ActRec::hasClass() and Closure::hasClass() decide that m_thisUnsafe holds a
  // Class* rather than an ObjectData* purely from the address being low.
  always_assert(is_low_mem(classPtr));
  try {
    return new (classPtr) Class(preClass, parent, std::move(usedTraits),
                                classVecLen, funcVecLen);
  } catch (...) {
    low_sized_free(mem, size);
    throw;
  }
}

Class* Class::rescope(Class* ctx) {
  assertx(parent() == c_Closure::classof());
  assertx(m_invoke);

  // Look up the generated template class for this particular subclass of
  // Closure.  This class maintains the table of scoped clones of itself, and
  // if we create a new scoped clone, we need to map it there.
  auto template_cls = this;
  auto const invoke = template_cls->m_invoke;

  auto const try_template = [&]() -> Class* {
    if (invoke->cls() != ctx) return nullptr;

    // The first scoping will never be for `template_cls' (since it's
    // impossible to define a closure in the context of its own Closure
    // subclass), so if this happens, it means that `template_cls' is in a
    // "de-scoped" state and we shouldn't use it.  (See Class::releaseRefs().)
    if (template_cls->m_scoped && invoke->cls() == template_cls) {
      return nullptr;
    }
    return template_cls;
  };

  // If the template class has already been scoped to `ctx', we're done.  This
  // is the common case in repo mode.
  if (auto cls = try_template()) return cls;

  template_cls->allocExtraData();
  auto& scopedClones = template_cls->m_extra.raw()->m_scopedClones;

  auto const key = ctx;

  auto const try_cache = [&] {
    auto it = scopedClones.find(key);
    return it != scopedClones.end() ? it->second.get() : nullptr;
  };

  { // Return the cached clone if we have one.
    ReadLock l(g_scopeCacheMutex);

    // If this succeeds, someone raced us to scoping the template.  We may have
    // unnecessarily allocated an ExtraData, but whatever.
    if (auto cls = try_template()) return cls;

    if (auto cls = try_cache()) return cls;
  }

  auto cloneClass = [&] {
    auto const cls = newClass(m_preClass.get(), m_parent.get());
    return cls;
  };

  // We use the French for closure because using the English crashes gcc in the
  // implicit lambda capture below.  (This is fixed in gcc 4.8.5.)
  auto fermeture = ClassPtr {
    template_cls->m_scoped ? cloneClass() : template_cls
  };

  WriteLock l(g_scopeCacheMutex);

  if (fermeture->m_scoped) {
    // We raced with someone who scoped the template_cls just as we were about
    // to, so make a new Class.  (Note that we don't want to do this with the
    // lock held, since it's very expensive.)
    //
    // This race should be far less likely than a race between two attempted
    // first-scopings for `template_cls', which is why we don't do an test-and-
    // set when we first check `m_scoped' before acquiring the lock.
    g_scopeCacheMutex.release();
    SCOPE_EXIT { g_scopeCacheMutex.acquireWrite(); };
    fermeture = ClassPtr { cloneClass() };
  }

  // Check the caches again.
  if (auto cls = try_template()) return cls;
  if (auto cls = try_cache()) return cls;

  fermeture->m_invoke->rescope(ctx);
  fermeture->m_invoke->setHasForeignThis(false);
  fermeture->m_scoped = true;

  if (ctx != nullptr &&
      !Cfg::Repo::Authoritative &&
      !classHasPersistentRDS(ctx)) {
    // If the context Class might be destroyed, we need to do extra accounting
    // so that we can drop all clones scoped to it at the time of destruction.
    ctx->allocExtraData();
    ctx->m_extra.raw()->m_clonesWithThisScope.push_back(
      ClassPtr(template_cls));
  }

  auto updateClones = [&] {
    if (template_cls != fermeture.get()) {
      scopedClones[key] = fermeture;
      fermeture.get()->setClassHandle(template_cls->m_cachedClass);
    }
  };

  InstanceBits::ifInitElse(
    [&] {
      fermeture->setInstanceBits();
      updateClones();
    },
    [&] {
      updateClones();
    }
  );

  return fermeture.get();
}

EnumValues* Class::setEnumValues(EnumValues* values) {
  auto extra = m_extra.ensureAllocated();
  EnumValues* expected = nullptr;
  if (!extra->m_enumValues.compare_exchange_strong(
        expected, values, std::memory_order_acq_rel)) {
    // Already set by someone else, use theirs.
    delete values;
    return expected;
  } else {
    return values;
  }
}

Class::ExtraData::~ExtraData() {
  delete m_enumValues.load(std::memory_order_acquire);
  if (m_lsbMemoExtra.m_handles) {
    for (auto const& kv : m_lsbMemoExtra.m_symbols) {
      rds::unbind(kv.first, kv.second);
    }
    vm_free(m_lsbMemoExtra.m_handles);
  }
}

void Class::destroy() {
  /*
   * If we were never put on NamedType::classList, or
   * we've already been destroy'd, there's nothing to do
   */
  if (!m_cachedClass.bound()) return;

  Lock l(getMutex(name()));
  // Need to recheck now we have the lock
  if (!m_cachedClass.bound()) return;
  // Only do this once.
  m_cachedClass = rds::Link<PackedPtr<Class>, rds::Mode::NonLocal>{};

  /*
   * Regardless of refCount, this Class is now unusable.  Remove it
   * from the class list.
   *
   * Needs to be under the lock, because multiple threads could call
   * destroy, or want to manipulate the class list.  (It's safe for
   * other threads to concurrently read the class list without the
   * lock.)
   */
  auto const pcls = m_preClass.get();
  pcls->namedType()->removeClass(this);

  releaseSProps();

  for (size_t i = 0; i < numMethods(); i++) {
    if (auto meth = getMethod(i)) {
      if (meth->cls() == this) meth->atomicFlags().set(Func::Flags::Zombie);
    }
  }

  Treadmill::enqueue(
    [this] {
      releaseRefs();
      if (!this->decAtomicCount()) this->atomicRelease();
    }
  );
}

void Class::atomicRelease() {
  assertx(!m_cachedClass.bound());
  assertx(!getCount());
  this->~Class();
  low_free(mallocPtr());
}

Class::~Class() {
  releaseRefs();   // must be called for Func-nulling side effects
  releaseSProps(); // must be called to avoid RDS Symbol collisions

  if (m_sPropCache) {
    auto const n = numStaticProperties();
    for (auto i = 0; i < n; ++i) {
      m_sPropCache[i].~Link();
    }
    using LinkT = std::remove_pointer<decltype(m_sPropCache)>::type;
    vm_sized_free(m_sPropCache, n * sizeof(LinkT));
  }

  for (auto i = size_t{}, n = numMethods(); i < n; i++) {
    if (auto meth = getMethod(i)) {
      if (meth->isPreFunc()) {
        meth->freeClone();
      } else {
        Func::destroy(meth);
      }
    }
  }

  if (m_extra) {
    delete m_extra.raw();
  }

  // clean enum cache
  EnumCache::deleteValues(this);
  if (m_vtableVec) vm_free(m_vtableVec);

#ifndef NDEBUG
  validate();
  m_magic = ~m_magic;
#endif
}

void Class::releaseRefs() {
  /*
   * We have to be careful here.
   * We want to free up as much as possible as early as possible, but
   * some of our methods may actually belong to our parent
   * This means we can't destroy *our* Funcs until our refCount
   * hits zero (ie when Class::~Class gets called), because there
   * could be a child class which hasn't yet been destroyed, which
   * will need to inspect them. Also, we need to inspect the Funcs
   * now (while we still have a references to parent) to determine
   * which ones we will eventually need to free.
   * Similarly, if any of our func's belong to a parent class, we
   * can't free the parent, because one of our children could also
   * have a reference to those func's (and its only reference to
   * our parent is via this class).
   */
  auto num = numMethods();
  bool okToReleaseParent = true;
  for (auto i = 0; i < num; i++) {
    Func* meth = getMethod(i);
    if (meth /* releaseRefs can be called more than once */ &&
        meth->cls() != this) {
      setMethod(i, nullptr);
      okToReleaseParent = false;
    }
  }

  if (okToReleaseParent) {
    m_parent.reset();
  }

  m_declInterfaces.clear();
  m_requirements.clear();

  if (m_extra) {
    auto xtra = m_extra.raw();
    xtra->m_usedTraits.clear();
    xtra->m_declIncludedEnums.clear();

    if (xtra->m_clonesWithThisScope.size() > 0) {
      WriteLock l(g_scopeCacheMutex);

      // Purge all references to scoped closure clones that are scoped to
      // `this'---there is no way anyone can find them at this point.
      for (auto const& template_cls : xtra->m_clonesWithThisScope) {
        auto const invoke = template_cls->m_invoke;

        if (invoke->cls() == this) {
          // We only hijack the `template_cls' as a clone for static rescopings
          // (which were signified by CloneAttr::None).  To undo this, we need
          // to make sure that /no/ scoping will match with that of
          // `template_cls'.  We can accomplish this by using `template_cls'
          // itself as the context.
          // Any instance of this closure will have its own scoped clone, and
          // we are de-scoping `template_cls' here, so the only way to obtain
          // it for dynamic binding is to reference it by name, which is
          // logically private (in PHP7, it's always just "Closure").  In HHVM,
          // this is possible by obtaining the Closure subclass's name, in
          // which case we'll just force a fresh clone via a special-case check
          // in Class::rescope().
          invoke->rescope(template_cls.get());
          // We explicitly decline to reset template_cls->m_scoped.  This lets
          // us simplify some assertions in rescope(), gives us a nice sanity
          // check for debugging, and avoids having to play around too much
          // with how Func::rescope() works, at the cost of preventing the
          // template from being scoped again.  This should only happen outside
          // of RepoAuthoritative mode while code is being modified, so the
          // extra memory usage is not a substantial concern.
        } else {
          assertx(template_cls->m_extra);
          auto& scopedClones = template_cls->m_extra.raw()->m_scopedClones;

          auto const it = scopedClones.find(this);
          assertx(it != scopedClones.end());
          it->second->m_cachedClass =
            rds::Link<PackedPtr<Class>, rds::Mode::NonLocal>{};
          scopedClones.erase(it);
        }
      }
    }
    xtra->m_clonesWithThisScope.clear();
  }
}

void Class::releaseSProps() {
  if (!m_sPropCache) return;

  auto init = &m_sPropCacheInit;

  if (!init->bound()) {
    if constexpr (debug) {
      for (Slot i = 0, n = numStaticProperties(); i < n; ++i) {
        auto const& sProp = m_staticProperties[i];
        if (declaredOnThisClass(sProp)) {
          always_assert(!m_sPropCache[i].bound());
        }
      }
    }
    return;
  }

  if (rds::isNormalHandle(init->handle())) {
    auto const symbol = rds::LinkName{"SPropCacheInit", name()};
    unbindLink(init, symbol);
    for (Slot i = 0, n = numStaticProperties(); i < n; ++i) {
      auto const& sProp = m_staticProperties[i];
      assertx(m_sPropCache[i].bound());
      if (m_sPropCache[i].isPersistent()) continue;
      if (declaredOnThisClass(sProp)) {
        unbindLink(&m_sPropCache[i], rds::SPropCache{this, i});
      }
    }
  } else {
    // init handle in rds::Persistent implies that all properties are persistent
    assertx(rds::isPersistentHandle(init->handle()));
    if constexpr (debug) {
      for (Slot i = 0, n = numStaticProperties(); i < n; ++i) {
        always_assert(m_sPropCache[i].bound());
        always_assert(m_sPropCache[i].isPersistent());
      }
    }
    return;
  }
}

Class::Avail Class::avail(Class*& parent,
                          bool tryAutoload /* = false */) const {
  if (Class *ourParent = m_parent.get()) {
    if (!parent) {
      PreClass *ppcls = ourParent->m_preClass.get();
      parent = Class::get(ppcls->namedType(),
                              m_preClass.get()->parent(), tryAutoload);
      if (!parent) {
        parent = ourParent;
        return Avail::Fail;
      }
    }
    if (parent != ourParent) {
      if (UNLIKELY(ourParent->isZombie())) {
        const_cast<Class*>(this)->destroy();
      }
      return Avail::False;
    }
  }

  if (m_preClass->attrs() & (AttrEnum|AttrEnumClass)) {
    auto const& ie = m_extra->m_declIncludedEnums;
    for (size_t i = 0; i < ie.size(); ++i) {
      auto const de = ie[i].get();
      auto const penum = ie[i]->m_preClass.get();
      auto const included_enum = Class::get(penum->namedType(),
                                            penum->name(), tryAutoload);
      if (included_enum != de) {
        if (!included_enum) {
          parent = de;
          return Avail::Fail;
        }
        if (UNLIKELY(de->isZombie())) {
          const_cast<Class*>(this)->destroy();
        }
        return Avail::False;
      }
    }
  }

  for (size_t i = 0; i < m_declInterfaces.size(); i++) {
    auto di = m_declInterfaces[i].get();
    const StringData* pdi = m_preClass.get()->interfaces()[i];
    assertx(pdi->tsame(di->name()));

    PreClass *pint = di->m_preClass.get();
    Class* interface = Class::get(pint->namedType(), pdi,
                                      tryAutoload);
    if (interface != di) {
      if (interface == nullptr) {
        parent = di;
        return Avail::Fail;
      }
      if (UNLIKELY(di->isZombie())) {
        const_cast<Class*>(this)->destroy();
      }
      return Avail::False;
    }
  }

  if (Cfg::Repo::Authoritative) {
    if (m_preClass->usedTraits().size()) {
      int numIfaces = m_interfaces.size();
      for (int i = 0; i < numIfaces; i++) {
        auto di = m_interfaces[i].get();

        PreClass *pint = di->m_preClass.get();
        Class* interface = Class::get(pint->namedType(), pint->name(),
                                          tryAutoload);
        if (interface != di) {
          if (interface == nullptr) {
            parent = di;
            return Avail::Fail;
          }
          if (UNLIKELY(di->isZombie())) {
            const_cast<Class*>(this)->destroy();
          }
          return Avail::False;
        }
      }
    }
  } else {
    for (size_t i = 0, n = m_extra->m_usedTraits.size(); i < n; ++i) {
      auto usedTrait = m_extra->m_usedTraits[i].get();
      const StringData* usedTraitName = m_preClass.get()->usedTraits()[i];
      PreClass* ptrait = usedTrait->m_preClass.get();
      Class* trait = Class::get(ptrait->namedType(), usedTraitName,
                                    tryAutoload);
      if (trait != usedTrait) {
        if (trait == nullptr) {
          parent = usedTrait;
          return Avail::Fail;
        }
        if (UNLIKELY(usedTrait->isZombie())) {
          const_cast<Class*>(this)->destroy();
        }
        return Avail::False;
      }
    }
  }

  return Avail::True;
}


///////////////////////////////////////////////////////////////////////////////
// Basic info.

size_t Class::stableHash() const {
  return folly::hash::hash_combine(
    name()->hashStatic(),
    preClass()->unit()->sn()
  );
}

///////////////////////////////////////////////////////////////////////////////
// Ancestry.

const Class* Class::commonAncestor(const Class* cls) const {
  assertx(isNormalClass(this) && isNormalClass(cls));

  // Walk up m_classVec for both classes to look for a common ancestor.
  auto vecIdx = std::min(m_classVecLen, cls->m_classVecLen) - 1;
  do {
    assertx(vecIdx < m_classVecLen && vecIdx < cls->m_classVecLen);
    if (m_classVec[vecIdx] == cls->m_classVec[vecIdx]) {
      return m_classVec[vecIdx];
    }
  } while (vecIdx--);

  return nullptr;
}

///////////////////////////////////////////////////////////////////////////////
// Magic methods.

const Func* Class::getDeclaredCtor() const {
  const Func* f = getCtor();
  return f != SystemLib::s_nullCtor ? f : nullptr;
}

const Func* Class::getRegularInvoke() const {
  assertx(IMPLIES(m_invoke, !m_invoke->isStaticInPrologue()));
  return m_invoke;
}

///////////////////////////////////////////////////////////////////////////////
// Builtin classes.

bool Class::isCppSerializable() const {
  assertx(instanceCtor()); // Only call this on CPP classes
  auto* ndi = m_extra ? m_extra.raw()->m_nativeDataInfo : nullptr;
  return ndi != nullptr && ndi->isSerializable();
}

bool Class::isCollectionClass() const {
  auto s = name();
  return collections::isTypeName(s);
}


///////////////////////////////////////////////////////////////////////////////
// Property initialization.

void Class::initialize() const {
  if (m_allFlags.m_maybeRedefsPropTy) checkPropTypeRedefinitions();
  if (m_allFlags.m_needsPropInitialCheck) checkPropInitialValues();

  if (m_pinitVec.size() > 0 && getPropData() == nullptr) {
    initProps();
  }
  if (numStaticProperties() > 0 && needsInitSProps()) {
    initSProps();
  }
}

bool Class::initialized() const {
  if (m_pinitVec.size() > 0 && getPropData() == nullptr) {
    return false;
  }
  if (numStaticProperties() > 0 && needsInitSProps()) {
    return false;
  }
  if (m_allFlags.m_maybeRedefsPropTy &&
      (!m_extra->m_checkedPropTypeRedefs.bound() ||
       !m_extra->m_checkedPropTypeRedefs.isInit())) {
    return false;
  }
  if (m_allFlags.m_needsPropInitialCheck &&
      (!m_extra->m_checkedPropInitialValues.bound() ||
       !m_extra->m_checkedPropInitialValues.isInit())) {
    return false;
  }
  return true;
}

void Class::initProps() const {
  assertx(m_pinitVec.size() > 0);
  assertx(getPropData() == nullptr);

  // Copy initial values for properties to a new vector that can be used to
  // complete initialization for non-scalar properties via the iterative
  // 86pinit() calls below. 86pinit() takes a reference to an array to populate
  // with initial property values; after it completes, we copy the values into
  // the new propVec.
  auto propVec = PropInitVec::allocWithReqAllocator(m_declPropInit);

  VMRegAnchor _;
  CoeffectsAutoGuard _2;

  initPropHandle();
  m_propDataCache.initWith(propVec);

  try {
    // Iteratively invoke 86pinit() methods upward
    // through the inheritance chain.
    for (auto it = m_pinitVec.rbegin(); it != m_pinitVec.rend(); ++it) {
      DEBUG_ONLY auto retval = g_context->invokeFunc(
        *it, init_null_variant, nullptr, nullptr, const_cast<Class*>(this),
        RuntimeCoeffects::pure(), false
      );
      assertx(retval.m_type == KindOfNull);
    }
  } catch (...) {
    // Undo the allocation of propVec
    req::destroy_raw(propVec);
    *m_propDataCache = nullptr;
    m_propDataCache.markUninit();
    throw;
  }

  // For properties that do not require deep initialization, promote strings
  // and arrays that came from 86pinit to static. This allows us to initialize
  // object properties very quickly because we can just memcpy and we don't
  // have to do any refcounting.
  // For properties that require "deep" initialization, we have to do a little
  // more work at object creation time.
  for (Slot slot = 0; slot < propVec->size(); slot++) {
    auto index = propSlotToIndex(slot);
    auto piv_entry = (*propVec)[index];
    assertx(!piv_entry.deepInit);
    // Set deepInit if the property requires "deep" initialization.
    if (m_declProperties[slot].attrs & AttrDeepInit) {
      piv_entry.deepInit = true;
    } else {
      TypedValue tv = piv_entry.val.tv();
      tvAsVariant(&tv).setEvalScalar();
      tvCopy(tv, piv_entry.val);
    }
  }
}

bool Class::needsInitSProps() const {
  return !m_sPropCacheInit.bound() || !m_sPropCacheInit.isInit();
}

void Class::initSProps() const {
  assertx(needsInitSProps() || m_sPropCacheInit.isPersistent());

  if (m_sPropCacheInit.bound() && m_sPropCacheInit.isPersistent()) return;

  const bool hasNonscalarInit = !m_sinitVec.empty() || !m_linitVec.empty();
  Optional<VMRegAnchor> _;
  if (hasNonscalarInit) {
    _.emplace();
  }

  // Initialize static props for parent.
  Class* parent = this->parent();
  if (parent && parent->needsInitSProps()) {
    parent->initSProps();
  }

  if (!numStaticProperties()) return;

  initSPropHandles();
  if (m_sPropCacheInit.isPersistent()) return;

  // Perform scalar inits.
  for (Slot slot = 0, n = m_staticProperties.size(); slot < n; ++slot) {
    auto const& sProp = m_staticProperties[slot];
    assertx(sProp.typeConstraints.validForProp());

    if (m_sPropCache[slot].isPersistent()) continue;

    // TODO(T61738946): We can remove the temporary here once we no longer
    // coerce class_meth types.
    auto val = sProp.val;

    if (declaredOnThisClass(sProp)) {
      sProp.typeConstraints.validForPropResolved(sProp.cls, sProp.name);
      if (Cfg::Eval::CheckPropTypeHints > 0 &&
          !(sProp.attrs & (AttrInitialSatisfiesTC|AttrSystemInitialValue)) &&
          sProp.val.m_type != KindOfUninit) {
        for (auto const& tc : sProp.typeConstraints.range()) {
          if (tc.isCheckable()) {
            tc.verifyStaticProperty(&val, this, sProp.cls, sProp.name);
          }
        }
      }
      m_sPropCache[slot]->val = val;
    }
  }

  bespoke::profileArrLikeStaticProps(this);

  // If there are non-scalar initializers (i.e. 86sinit or 86linit methods),
  // run them now.
  // They will override the KindOfUninit values set by scalar initialization.
  if (hasNonscalarInit) {
    for (unsigned i = 0, n = m_sinitVec.size(); i < n; i++) {
      DEBUG_ONLY auto retval = g_context->invokeFunc(
        m_sinitVec[i], init_null_variant, nullptr, nullptr, const_cast<Class*>(this),
        RuntimeCoeffects::pure(), false
      );
      assertx(retval.m_type == KindOfNull);
    }
    for (unsigned i = 0, n = m_linitVec.size(); i < n; i++) {
      DEBUG_ONLY auto retval = g_context->invokeFunc(
        m_linitVec[i], init_null_variant, nullptr, nullptr,
        const_cast<Class*>(this), RuntimeCoeffects::pure(), false
      );
      assertx(retval.m_type == KindOfNull);
    }
  }

  m_sPropCacheInit.initWith(true);
}

Slot Class::lsbMemoSlot(const Func* func, bool forValue) const {
  assertx(m_extra);
  if (forValue) {
    assertx(func->numKeysForMemoize() == 0);
  } else {
    assertx(func->numKeysForMemoize() > 0);
  }
  const auto& slots = m_extra->m_lsbMemoExtra.m_slots;
  auto it = slots.find(func->getFuncId());
  always_assert(it != slots.end());
  return it->second;
}

void Class::checkPropInitialValues() const {
  assertx(m_allFlags.m_needsPropInitialCheck);
  assertx(m_extra.get() != nullptr);

  auto extra = m_extra.get();
  checkedPropInitialValuesHandle(); // init handle
  if (extra->m_checkedPropInitialValues.isInit()) return;

  for (Slot slot = 0; slot < m_declProperties.size(); ++slot) {
    auto const& prop = m_declProperties[slot];
    for (auto const& tc : prop.typeConstraints.range()) {
      assertx(tc.validForProp());
      tc.validForPropResolved(prop.cls, prop.name);
    }

    if (Cfg::Eval::CheckPropTypeHints <= 0) continue;
    if (prop.attrs & (AttrInitialSatisfiesTC|AttrSystemInitialValue)) continue;
    auto const index = propSlotToIndex(slot);
    auto const rval = m_declPropInit[index].val;
    if (type(rval) == KindOfUninit) continue;
    auto tv = rval.tv();
    for (auto const& tc : prop.typeConstraints.range()) {
      if (tc.isCheckable()) {
        tc.verifyProperty(&tv, this, prop.cls, prop.name);
      }
    }

    // No coercion for statically initialized properties.
    // Coercing property values here is not thread-safe.
    assertx(type(tv) == type(rval));
    assertx(val(tv).num == val(rval).num);
  }

  extra->m_checkedPropInitialValues.initWith(true);
}

void Class::checkPropTypeRedefinitions() const {
  assertx(m_allFlags.m_maybeRedefsPropTy);
  assertx(Cfg::Eval::CheckPropTypeHints > 0);
  assertx(m_parent);
  assertx(m_extra.get() != nullptr);

  auto extra = m_extra.get();
  checkedPropTypeRedefinesHandle(); // init handle
  if (extra->m_checkedPropTypeRedefs.isInit()) return;

  if (m_parent->m_allFlags.m_maybeRedefsPropTy) m_parent->checkPropTypeRedefinitions();

  if (m_allFlags.m_selfMaybeRedefsPropTy) {
    for (Slot slot = 0; slot < m_declProperties.size(); slot++) {
      auto const& prop = m_declProperties[slot];
      if (prop.attrs & AttrNoBadRedeclare) continue;
      checkPropTypeRedefinition(slot);
    }
  }

  extra->m_checkedPropTypeRedefs.initWith(true);
}

void Class::checkPropTypeRedefinition(Slot slot) const {
  assertx(m_allFlags.m_maybeRedefsPropTy);
  assertx(Cfg::Eval::CheckPropTypeHints > 0);
  assertx(m_parent);
  assertx(slot != kInvalidSlot);
  assertx(slot < numDeclProperties());

  auto const& prop = m_declProperties[slot];
  assertx(!(prop.attrs & AttrNoBadRedeclare));

  auto const& oldProp = m_parent->m_declProperties[slot];

  auto const& oldTCs = oldProp.typeConstraints;
  auto const& newTCs = prop.typeConstraints;

  for (auto const& oldTC : oldTCs.range()) {
    if (oldTC.isMixed()) {
      continue;
    }
    if (std::none_of(
          newTCs.range().begin(),
          newTCs.range().end(),
          [&](const TypeConstraint& tc) {
            return oldTC.equivalentForProp(tc);
          }
        )
    ) {
      auto const oldTCName =
        oldTC.hasConstraint() ? oldTC.displayName() : "mixed";
      auto const msg = fmt::format(
        "{}::{} must have a type of {} as defined{} in {}",
        prop.cls->name()->slice(),
        prop.name,
        oldTCName,
        oldTC.isUpperBound()? " by the upper-bound" : "",
        oldProp.cls->name()->slice()
      );
      raise_property_typehint_error(
        msg,
        oldTC.isSoft(),
        oldTC.isUpperBound()
      );
    }
  }

  for (auto const& newTC : newTCs.range()) {
    if (std::none_of(oldTCs.range().begin(), oldTCs.range().end(),
          [&](const TypeConstraint& oldTC) {
            return oldTC.equivalentForProp(newTC);
          })) {
      auto const tcName = newTC.hasConstraint() ? newTC.displayName() : "mixed";
      auto const msg = fmt::format(
        "{}::{} includes a {} of {} but has no equivalent definition in {}",
        prop.cls->name()->slice(),
        prop.name,
        newTC.isUpperBound() ? "type upper-bound" : "type",
        tcName,
        oldProp.cls->name()->slice()
      );
      raise_property_typehint_error(msg, newTC.isSoft(), true);
    }
  }
}

///////////////////////////////////////////////////////////////////////////////
// Property storage.

void Class::initSPropHandles() const {
  if (m_sPropCacheInit.bound()) return;

  bool usePersistentHandles = m_cachedClass.isPersistent();
  bool allPersistentHandles = usePersistentHandles;

  // Propagate to parents so we can link inherited static props.
  Class* parent = this->parent();
  if (parent) {
    parent->initSPropHandles();
    if (!rds::isPersistentHandle(parent->sPropInitHandle())) {
      allPersistentHandles = false;
    }
  }

  // Bind all the static prop handles.
  for (Slot slot = 0, n = m_staticProperties.size(); slot < n; ++slot) {
    auto& propHandle = m_sPropCache[slot];
    auto const& sProp = m_staticProperties[slot];

    if (declaredOnThisClass(sProp)) {
      if (usePersistentHandles && (sProp.attrs & AttrPersistent)) {
        static_assert(sizeof(StaticPropData) == sizeof(sProp.val),
                      "StaticPropData must be a simple wrapper "
                      "around TypedValue");
        auto const tv = reinterpret_cast<const StaticPropData*>(&sProp.val);
        propHandle.bind(rds::Mode::Persistent, rds::SPropCache{this, slot}, tv);
      } else {
        allPersistentHandles = false;
        propHandle.bind(rds::Mode::Local, rds::SPropCache{this, slot});
      }
    } else {
      auto const realSlot = sProp.cls->lookupSProp(sProp.name);
      propHandle = sProp.cls->m_sPropCache[realSlot];
    }
    if (allPersistentHandles && !propHandle.isPersistent()) {
      allPersistentHandles = false;
    }
  }

  // Bind the init handle; this indicates that all handles are bound.
  if (allPersistentHandles) {
    // We must make sure the value stored at the handle is correct before
    // setting m_sPropCacheInit in case another thread tries to read it at just
    // the wrong time. And rather than giving each Class its own persistent
    // handle that always points to an immutable 'true', share one between all
    // of them.
    m_sPropCacheInit = rds::s_persistentTrue;
  } else {
    auto const symbol = rds::LinkName{"SPropCacheInit", name()};
    m_sPropCacheInit.bind(rds::Mode::Normal, symbol);
  }
  rds::recordRds(m_sPropCacheInit.handle(),
                 sizeof(bool), "SPropCacheInit", name()->slice());
}

Class::PropInitVec* Class::getPropData() const {
  return (m_propDataCache.bound() && m_propDataCache.isInit())
    ? *m_propDataCache
    : nullptr;
}

TypedValue* Class::getSPropData(Slot index) const {
  assertx(numStaticProperties() > index);
  return m_sPropCache[index].bound()
    ? &m_sPropCache[index].get()->val
    : nullptr;
}

///////////////////////////////////////////////////////////////////////////////
// Property lookup and accessibility.

MemberLookupContext::MemberLookupContext(const Class* cls,
                                         const Func* func)
  : MemberLookupContext(cls, func->moduleName())
  {}

MemberLookupContext::MemberLookupContext(const Class* cls,
                                         const StringData* moduleName) {
  m_class = cls;
  m_moduleName = moduleName;
}

const Class* MemberLookupContext::cls() const {
  return m_class;
}

const StringData* MemberLookupContext::moduleName() const {
  return m_moduleName;
}

Class::PropSlotLookup Class::getDeclPropSlot(
  const MemberLookupContext& propCtx,
  const StringData* key
) const {
  auto const propSlot = lookupDeclProp(key);

  auto const ctx = propCtx.cls();
  auto accessible = false;
  auto readonly = false;
  auto internal = false;

  if (propSlot != kInvalidSlot) {
    auto const attrs = m_declProperties[propSlot].attrs;
    readonly = bool(attrs & AttrIsReadonly);
    internal = bool(attrs & AttrInternal);
    if ((attrs & (AttrProtected|AttrPrivate)) &&
        (g_context.isNull() || !g_context->debuggerSettings.bypassCheck)) {
      // Fetch the class in the inheritance tree which first declared the
      // property
      auto const baseClass = m_declProperties[propSlot].cls;
      assertx(baseClass);

      // If ctx == baseClass, we have the right property and we can stop here.
      if (ctx == baseClass) return PropSlotLookup { propSlot, true, false, readonly, internal };

      // <<__TestsBypassVisibility>> check for property access from a test.
      if (canTestsBypassVisibility(
            m_declProperties[propSlot].preProp->userAttributes(), ctx)) {
        return PropSlotLookup { propSlot, true, false, readonly, internal };
      }

      // The anonymous context cannot access protected or private properties, so
      // we can fail fast here.
      if (ctx == nullptr) return PropSlotLookup { propSlot, false, false, readonly, internal };

      if (attrs & AttrPrivate) {
        // ctx != baseClass and the property is private, so it is not
        // accessible. We need to keep going because ctx may define a private
        // property with this name.
        accessible = false;
      }
      else {
        if (ctx == (Class*)-8 || ctx->classof(baseClass)) {
          // The special ctx (Class*)-8 is used by unserialization to
          // mean that protected properties are ok. Otherwise,
          // ctx is derived from baseClass, so we know this protected
          // property is accessible and we know ctx cannot have private
          // property with the same name, so we're done.
          return PropSlotLookup { propSlot, true, false, readonly, internal };
        }
        if (!baseClass->classof(ctx)) {
          // ctx is not the same, an ancestor, or a descendent of baseClass,
          // so the property is not accessible. Also, we know that ctx cannot
          // be the same or an ancestor of this, so we don't need to check if
          // ctx declares a private property with the same name and we can
          // fail fast here.
          return PropSlotLookup { propSlot, false, false, readonly, internal };
        }
        // We now know this protected property is accessible, but we need to
        // keep going because ctx may define a private property with the same
        // name.
        accessible = true;
        assertx(baseClass->classof(ctx));
      }
    } else {
      // The property is public (or we're in the debugger and we are bypassing
      // accessibility checks).
      accessible = true;
      // If ctx == this, we don't have to check if ctx defines a private
      // property with the same name and we can stop here.
      if (ctx == this) return PropSlotLookup { propSlot, true, false, readonly, internal };

      // We still need to check if ctx defines a private property with the same
      // name.
    }
  } else {
    // We didn't find a visible declared property in this's property map
    accessible = false;
  }

  // If ctx is an ancestor of this, check if ctx has a private property with the
  // same name.
  if (ctx && ctx != (Class*)-8 && classof(ctx)) {
    auto const ctxPropSlot = ctx->lookupDeclProp(key);

    if (ctxPropSlot != kInvalidSlot &&
        ctx->m_declProperties[ctxPropSlot].cls == ctx &&
        (ctx->m_declProperties[ctxPropSlot].attrs & AttrPrivate)) {
      // A private property from ctx trumps any other property we may
      // have found.
      readonly = bool(ctx->m_declProperties[ctxPropSlot].attrs & AttrIsReadonly);
      internal = bool(ctx->m_declProperties[ctxPropSlot].attrs & AttrInternal);
      return PropSlotLookup { ctxPropSlot, true, false, readonly, internal };
    }
  }

  if (!Cfg::Repo::Authoritative && propSlot == kInvalidSlot) {
    for (auto ancestor = m_parent; ancestor; ancestor = ancestor->m_parent) {
      auto const ancestorPropSlot = ancestor->lookupDeclProp(key);
      if (ancestorPropSlot == kInvalidSlot) continue;

      auto const& ancestorProp = ancestor->m_declProperties[ancestorPropSlot];
      if (
        ancestorProp.cls != ancestor.get() ||
        !(ancestorProp.attrs & AttrPrivate)
      ) {
        continue;
      }
      if (canTestsBypassVisibility(
            ancestorProp.preProp->userAttributes(), ctx)) {
        return PropSlotLookup {
          ancestorPropSlot,
          true,
          false,
          bool(ancestorProp.attrs & AttrIsReadonly),
          bool(ancestorProp.attrs & AttrInternal)
        };
      }
      break;
    }
  }

  if (propSlot == kInvalidSlot &&
      !g_context.isNull() &&
      g_context->debuggerSettings.bypassCheck &&
      m_parent) {
    // If the property could not be located on the current class, and this
    // class has a parent class, and the current evaluation is a debugger
    // eval with bypassCheck == true, search for the property as a member of
    // the parent class. The debugger access is not subject to visibilty checks.
    return m_parent->getDeclPropSlot(propCtx, key);
  }

  return PropSlotLookup { propSlot, accessible, false, readonly, internal };
}

Class::PropSlotLookup Class::findSProp(
  const MemberLookupContext& propCtx,
  const StringData* sPropName
) const {
  auto const sPropInd = lookupSProp(sPropName);
  auto const ctx = propCtx.cls();

  // Non-existent property.
  if (sPropInd == kInvalidSlot)
    return PropSlotLookup { kInvalidSlot, false, false, false, false };

  auto const& sProp = m_staticProperties[sPropInd];
  auto const sPropAttrs = sProp.attrs;
  auto const sPropConst = bool(sPropAttrs & AttrIsConst);
  auto const sPropReadOnly = bool(sPropAttrs & AttrIsReadonly);
  auto const sPropInternal = bool(sPropAttrs & AttrInternal);

  const Class* baseCls = this;
  if (sPropAttrs & AttrLSB) {
    // For an LSB static, accessibility attributes are relative to the class
    // that originally declared it.
    baseCls = sProp.cls;
  }
  // Property access within this Class's context.
  if (ctx == baseCls) return PropSlotLookup { sPropInd, true, sPropConst, sPropReadOnly, sPropInternal };

  // <<__TestsBypassVisibility>> check for static property access from a test.
  if (canTestsBypassVisibility(sProp.preProp->userAttributes(), ctx)) {
    return PropSlotLookup { sPropInd, true, sPropConst, sPropReadOnly, sPropInternal };
  }

  auto const accessible = [&] {
    switch (sPropAttrs & (AttrPublic | AttrProtected | AttrPrivate)) {
      // Public properties are always accessible.
      case AttrPublic:
        return true;

      // Property access is from within a parent class's method, which is
      // allowed for protected properties.
      case AttrProtected:
        return ctx != nullptr &&
               (baseCls->classof(ctx) || ctx->classof(baseCls));

      // Can only access private properties via the debugger.
      case AttrPrivate:
        return g_context->debuggerSettings.bypassCheck;

      default: break;
    }
    not_reached();
  }();
  // TODO(T130877659): Static property enforcement
  return PropSlotLookup { sPropInd, accessible, sPropConst, sPropReadOnly, sPropInternal };
}

Class::PropValLookup Class::getSPropIgnoreLateInit(
  const MemberLookupContext& ctx,
  const StringData* sPropName
) const {
  initialize();
  auto const lookup = findSProp(ctx, sPropName);
  if (lookup.slot == kInvalidSlot) {
    return PropValLookup { nullptr, kInvalidSlot, false, false, false, false };
  }

  auto const sProp = getSPropData(lookup.slot);

  if (debug) {
    auto const& decl = m_staticProperties[lookup.slot];
    auto const lateInit = bool(decl.attrs & AttrLateInit);
    always_assert_flog(
      sProp && (sProp->m_type != KindOfUninit || lateInit),
      "Static property initialization failed to initialize {}::{}",
      name(), sPropName
    );

    if (sProp->m_type != KindOfUninit) {
      if (Cfg::Repo::Authoritative) {
        auto const repoTy = staticPropRepoAuthType(lookup.slot);
        always_assert(tvMatchesRepoAuthType(*sProp, repoTy));
      }

      if (Cfg::Eval::CheckPropTypeHints > 2) {
        auto const typeOk = [&]{
          auto skipCheck = (sProp->m_type == KindOfNull &&
             !(decl.attrs & AttrNoImplicitNullable));

          auto res = true;
          if (skipCheck) return res;
          for (auto const& tc : decl.typeConstraints.range()) {
            if (tc.isCheckable() && !tc.isSoft()) {
              res = res && tc.assertCheck(sProp);
            }
          }
          return res;
        }();
        always_assert(typeOk);
      }
    }
  }

  return
    PropValLookup { sProp, lookup.slot, lookup.accessible, lookup.constant, lookup.readonly, lookup.internal };
}

Class::PropValLookup Class::getSProp(
  const MemberLookupContext& ctx,
  const StringData* sPropName
) const {
  auto const lookup = getSPropIgnoreLateInit(ctx, sPropName);
  if (lookup.val && UNLIKELY(lookup.val->m_type == KindOfUninit)) {
    auto const& decl = m_staticProperties[lookup.slot];
    if (decl.attrs & AttrLateInit) {
      throw_late_init_prop(decl.cls, sPropName, true);
    }
  }
  return lookup;
}

bool Class::IsPropAccessible(const Prop& prop, const MemberLookupContext& propCtx) {
  auto const ctx = propCtx.cls();
  if (prop.attrs & AttrPublic) return true;
  if (prop.attrs & AttrPrivate) return prop.cls == ctx;
  if (!ctx) return false;

  return prop.cls->classof(ctx) || ctx->classof(prop.cls);
}

Slot Class::propIndexToSlot(uint16_t index) const {
  auto const nprops = m_declProperties.size();
  for (Slot slot = 0; slot < nprops; slot++) {
    if (m_slotIndex[slot] == index) return slot;
  }
  always_assert_flog(0, "propIndexToSlot: no slot found for index = {}", index);
}

bool Class::isClosureClass() const {
  assertx(IMPLIES(attrs() & AttrIsClosureClass,
                  parent() == c_Closure::classof()));
  return attrs() & AttrIsClosureClass;
}

bool Class::hasClosureCoeffectsProp() const {
  assertx(isClosureClass());
  if (!(attrs() & AttrHasClosureCoeffectsProp)) return false;
  assertx(getRegularInvoke()->hasCoeffectRules());
  assertx(getRegularInvoke()->getCoeffectRules().size() == 1);
  assertx(getRegularInvoke()->getCoeffectRules()[0].isClosureParentScope());
  return true;
}

Slot Class::getCoeffectsProp() const {
  assertx(hasClosureCoeffectsProp());
  assertx(numDeclProperties() > 0);
  return 0;
}

///////////////////////////////////////////////////////////////////////////////
// Constants.

const StaticString s_classname("classname");

Optional<RuntimeCoeffects>
Class::clsCtxCnsGet(const StringData* name, bool failIsFatal) const {
  auto const slot = m_constants.findIndex(name);
  if (slot == kInvalidSlot) {
    if (!failIsFatal) return std::nullopt;
    if (CoeffectsConfig::throws()) {
      raise_error("Context constant %s does not exist", name->data());
    }
    raise_warning("Context constant %s does not exist", name->data());
    return RuntimeCoeffects::none();
  }
  auto const& cns = m_constants[slot];
  if (cns.kind() != ConstModifierFlags::Kind::Context) {
    if (!failIsFatal) return std::nullopt;
    raise_error("%s is a %s, looking for a context constant",
                name->data(), ConstModifierFlags::show(cns.kind()));
  }
  if (cns.isAbstractAndUninit()) {
    if (!failIsFatal) return std::nullopt;
    if (CoeffectsConfig::throws()) {
      raise_error("Context constant %s is abstract", name->data());
    }
    raise_warning("Context constant %s is abstract", name->data());
    return RuntimeCoeffects::none();
  }

  return cns.val.constModifiers().getCoeffects(cns.val.constModifierFlags()).toRequired();
}

ArrayData* Class::resolvedTypeCnsGet(ArrayData* ad) const {
  auto const rawData = reinterpret_cast<intptr_t>(ad);
  if (rawData & 0x1) {
    return reinterpret_cast<ArrayData*>(rawData ^ 0x1);
  }
  return nullptr;
}

TypedValue Class::clsCnsGet(const StringData* clsCnsName,
                            ConstModifierFlags::Kind what,
                            bool resolve) const {
  always_assert(what != ConstModifierFlags::Kind::Context);
  Slot clsCnsInd;
  auto cnsVal = cnsNameToTV(clsCnsName, clsCnsInd, what);
  if (!cnsVal) return make_tv<KindOfUninit>();

  auto& cns = m_constants[clsCnsInd];
  // When a child extends a parent, rather than the child having
  // distinct copies of the constants defined by their parent we prefer
  // those constants be shared. Aside from saving memeory and avoiding
  // multiple initializations of the same logical constant, this
  // establishes the property that a constant accessed through a child
  // class will compare equal to the same constant accessed through the
  // child's parent class.
  if (cns.cls != this
      && what == ConstModifierFlags::Kind::Value) {
    return cns.cls->clsCnsGet(clsCnsName, what, resolve);
  }

  ArrayData* typeCns = nullptr;

  if (cnsVal->m_type != KindOfUninit) {
    switch (cns.kind()) {
      case ConstModifierFlags::Kind::Value:
        return *cnsVal;
      case ConstModifierFlags::Kind::Type: {
        // Type constants with the low bit set are already resolved and can be
        // returned after masking out that bit.
        //
        // We can't check isDict here because we're using that low bit as a
        // marker; instead, check isArrayLike and do the stricter check below.
        assertx(isArrayLikeType(type(cnsVal)));
        assertx(!isRefcountedType(type(cnsVal)));
        typeCns = val(cnsVal).parr;
        auto const resolved = resolvedTypeCnsGet(typeCns);
        if (resolved != nullptr) {
          assertx(resolved->isDictType());
          auto tv = make_persistent_array_like_tv(resolved);
          if (resolved->isVanilla()) {
            bespoke::profileArrLikeClsCns(this, &tv, clsCnsInd);
          }
          return tv;
        }
        break;
      }
      case ConstModifierFlags::Kind::Context:
        not_reached();
    }
  }

  if (!resolve) return make_tv<KindOfUninit>();

  /*
   * We use a sentinel static array to mark constants that are being evaluated
   * during recursive resolution. This array is never exposed to the rest of
   * the runtime, so we can test for it by pointer equality.
   */
  static auto const s_sentinelVec = VanillaVec::CopyStatic(staticEmptyVec());
  assertx(s_sentinelVec != staticEmptyVec());

  /*
   * We have either a constant with a non-scalar initializer or an unresolved
   * type constant, meaning it will be potentially different in different
   * requests, which we store separately in an array in RDS.
   *
   * We need a special marker value in the non-scalar constant cache to indicate
   * that we're currently evaluating the value of a (type) constant. If we
   * attempt to evaluate the value of a (type) constant, and the special marker
   * is present, that means the (type) constant is recursively defined and
   * we'll raise an error. The globals array is never a valid value of a (type)
   * constant, so we use it as the marker.
   */
  m_nonScalarConstantCache.bind(
    rds::Mode::Normal,
    rds::LinkName{"ClassNonScalarCnsCache", name()}
  );
  auto& clsCnsData = *m_nonScalarConstantCache;
  if (m_nonScalarConstantCache.isInit()) {
    auto const cCns = clsCnsData->get(clsCnsName);
    if (cCns.is_init()) {
      // There's an entry in the cache for this (type) constant. If it's the
      // sentinel value, the constant is recursively defined - throw an error.
      // Otherwise, return the cached result.
      if (UNLIKELY(tvIsVec(cCns) && val(cCns).parr == s_sentinelVec)) {
        raise_error(
          fmt::format(
            "Cannot declare self-referencing {} '{}::{}'",
            ConstModifierFlags::show(cns.kind()),
            name()->slice(),
            clsCnsName->slice()
          )
        );
      }
      return cCns;
    }
  } else {
    // Because RDS uses a generation number scheme, when isInit was false we
    // may have a pointer to a (no-longer extant) ArrayData in our RDS entry.
    // Use detach to clear our Array so we don't attempt to decref the stale
    // ArrayData.
    clsCnsData.detach();
    clsCnsData = Array::attach(
      VanillaDict::MakeReserveDict(m_constants.size())
    );
    m_nonScalarConstantCache.markInit();
  }

  // We're going to run the 86cinit to get the constant's value, or try to
  // resolve the type constant. Store the sentinel value as this (type)
  // constant's cache entry // so that we can detect recursion.
  auto marker = make_array_like_tv(s_sentinelVec);
  clsCnsData.set(StrNR(clsCnsName), tvAsCVarRef(&marker), true /* isKey */);

  SCOPE_FAIL {
    auto const v = clsCnsData.lookup(StrNR{clsCnsName});
    if (tvIsVec(v) && val(v).parr == s_sentinelVec) {
      clsCnsData.remove(StrNR{clsCnsName});
    }
  };

  if (cns.kind() == ConstModifierFlags::Kind::Type) {
    assertx(typeCns);

    Array resolvedTS;
    bool persistent = true;
    try {
      resolvedTS = TypeStructure::resolve(typeCns, cns.name, cns.cls, this,
                                          persistent);
    } catch (const Exception& e) {
      raise_error(e.getMessage());
    }
    assertx(resolvedTS.isDict());

    auto ad = ArrayData::GetScalarArray(std::move(resolvedTS));
    if (persistent) {
      auto& cns_nc = const_cast<Const&>(cns);

      // Multiple threads might create and store the resolved type structure
      // here, but that's fine since they'll all store the same thing thanks to
      // GetScalarArray(). Ditto for the pointed class.
      // We could avoid a little duplicated work during warmup with more
      // complexity but it's not worth it.
      auto const classname_field = ad->get(s_classname.get());
      if (isStringType(classname_field.type())) {
        cns_nc.setPointedClsName(classname_field.val().pstr);
      }
      setResolvedTypeConstVal(cns_nc, ad);
      clsCnsData.remove(StrNR{clsCnsName});

      // Need to check this after storing the resolved array in the
      // type-constant. LoggingProfile has side effects and
      // requires this to generate a static array
      if (allowBespokeArrayLikes()) {
        auto const profile =
          getLoggingProfile(this, clsCnsInd, bespoke::LocationType::TypeConstant);
        if (profile) {
          auto layout = profile->getLayout();
          if (layout.bespoke()) {
            setResolvedTypeConstVal(cns_nc, ad = layout.apply(ad));
          }
        }
      }

      auto tv = make_persistent_array_like_tv(ad);
      bespoke::profileArrLikeClsCns(this, &tv, clsCnsInd);
      return tv;
    }

    auto tv = make_persistent_array_like_tv(ad);
    clsCnsData.set(StrNR(clsCnsName), tvAsCVarRef(&tv), true /* isKey */);
    return tv;
  }

  // The class constant has not been initialized yet; do so.
  auto const meth86cinit = cns.cls->lookupMethod(s_86cinit.get());
  TypedValue args[1] = {
    make_tv<KindOfPersistentString>(const_cast<StringData*>(cns.name.get()))
  };

  // Wrap the call to 'invokeFuncFew' and call 'raise_error' if an
  // 'Object' exception is encountered. The effect of this is to treat
  // constant intialization errors as terminal.
  auto invokeFuncFew = [&]() -> TypedValue {
    try {
      return g_context->invokeFuncFew(
               meth86cinit,
               const_cast<Class*>(this),
               1,
               nullptr,
               args,
               RuntimeCoeffects::fixme(),
               false,
               false
             );
    } catch(Object& e) {
      auto const msg = throwable_to_string(e.get());
      raise_error(
                  "'%s::%s' initialization failure: %s"
                  , m_preClass->name()->data(), clsCnsName->data(), msg.data());
    }
  };
  auto ret = invokeFuncFew();

  switch (tvAsCVarRef(&ret).isAllowedAsConstantValue()) {
  case Variant::AllowedAsConstantValue::Allowed:
    break;
  case Variant::AllowedAsConstantValue::NotAllowed: {
    always_assert(false);
  }
  case Variant::AllowedAsConstantValue::ContainsObject: {
    // Generally, objects are not allowed as class constants.
    if (!(attrs() & AttrEnumClass)) {
      raise_error("Value unsuitable as class constant");
    }
    break;
  }
  }

  clsCnsData.set(StrNR(clsCnsName), tvAsCVarRef(ret), true /* isKey */);

  // The caller will inc-ref the returned value, so undo the inc-ref caused by
  // storing it in the cache.
  tvDecRefGenNZ(&ret);
  return ret;
}

const TypedValue* Class::cnsNameToTV(const StringData* clsCnsName,
                                     Slot& clsCnsInd,
                                     ConstModifierFlags::Kind what) const {
  always_assert(what != ConstModifierFlags::Kind::Context);
  clsCnsInd = m_constants.findIndex(clsCnsName);
  if (clsCnsInd == kInvalidSlot) return nullptr;
  if (m_constants[clsCnsInd].isAbstractAndUninit()) return nullptr;

  auto const kind = m_constants[clsCnsInd].kind();
  if (kind != what) return nullptr;

  auto const ret = &m_constants[clsCnsInd].val;
  assertx(IMPLIES(kind == ConstModifierFlags::Kind::Value, tvIsPlausible(*ret)));
  return ret;
}

Slot Class::clsCnsSlot(
  const StringData* name, ConstModifierFlags::Kind want, bool allowAbstract
) const {
  auto slot = m_constants.findIndex(name);
  if (slot == kInvalidSlot) return slot;
  if (!allowAbstract && m_constants[slot].isAbstractAndUninit()) return kInvalidSlot;
  return m_constants[slot].kind() == want ? slot : kInvalidSlot;
}

///////////////////////////////////////////////////////////////////////////////
// Other methods.

void Class::verifyPersistence() const {
  if (!debug) return;
  if (!isPersistent()) return;
  assertx(Cfg::Eval::ForceAllPersistent ||
          preClass()->unit()->isSystemLib() || Cfg::Repo::Authoritative);
  assertx(!m_parent || classHasPersistentRDS(m_parent.get()));
  for (DEBUG_ONLY int i = 0; i < m_interfaces.size(); i++) {
    assertx(classHasPersistentRDS(m_interfaces[i]));
  }
  for (DEBUG_ONLY auto const& usedTrait : m_extra->m_usedTraits) {
    assertx(classHasPersistentRDS(usedTrait.get()));
  }
}

void Class::setInstanceBits() {
  setInstanceBitsImpl<false>();
}
void Class::setInstanceBitsAndParents() {
  setInstanceBitsImpl<true>();
}

template<bool setParents>
void Class::setInstanceBitsImpl() {
  // Bit 0 is reserved to indicate whether or not the rest of the bits
  // are initialized yet.
  if (m_instanceBits.test(0)) return;

  // Make sure profiling translations stop changing the profiling counter.
  m_instanceCheckCount.store(1, std::memory_order_release);

  InstanceBits::BitSet bits;
  bits.set(0);
  auto setBits = [&](Class* c) {
    if (setParents) c->setInstanceBitsAndParents();
    bits |= c->m_instanceBits;
  };
  if (m_parent.get()) setBits(m_parent.get());

  int numIfaces = m_interfaces.size();
  for (int i = 0; i < numIfaces; i++) setBits(m_interfaces[i]);

  // As soon as we store to m_instanceBitsIndex, we'll start using that index
  // for optimized classof checks. The "setParents" case is when we're setting
  // up bits for existing classes, post-PGO; in this case, we have to set up
  // all the m_instanceBits fields before storing any m_instanceBitsIndex.
  auto const bit = InstanceBits::lookup(name());
  if (!setParents) setInstanceBitsIndex(bit);
  if (bit) bits.set(bit);

  m_instanceBits = bits;
}

void Class::setInstanceBitsIndex(unsigned int bit) {
  assertx(m_instanceBitsIndex.load() == kProfileInstanceBit);
  auto const cast = bit ? safe_cast<int8_t>(bit) : kNoInstanceBit;
  m_instanceBitsIndex.store(cast, std::memory_order_release);
}

const GenericsInfo& Class::getGenericsInfo() const {
  assertx(m_extra);
  return m_extra.raw()->m_genericsInfo;
}

///////////////////////////////////////////////////////////////////////////////
// Private methods.
//
// These are mostly for the class creation path.

void Class::setParent() {
  // Cache m_preClass->attrs()
  m_attrCopy = m_preClass->attrs();

  // Validate the parent
  if (m_parent.get() != nullptr) {
    Attr parentAttrs = m_parent->attrs();
    if (UNLIKELY(parentAttrs &
                 (AttrFinal | AttrInterface | AttrTrait | AttrEnum |
                  AttrEnumClass))) {
      if (!(parentAttrs & AttrFinal) ||
          (parentAttrs & (AttrEnum|AttrEnumClass)) ||
          m_preClass->userAttributes().find(s___MockClass.get()) ==
          m_preClass->userAttributes().end() ||
          m_parent->isCollectionClass()) {
        raise_error("Class %s may not inherit from %s (%s)",
                    m_preClass->name()->data(),
                    ((parentAttrs & (AttrEnum|AttrEnumClass)) ? "enum" :
                     (parentAttrs & AttrFinal)     ? "final class" :
                     (parentAttrs & AttrInterface) ? "interface"   : "trait"),
                    m_parent->name()->data());
      }
      if ((parentAttrs & AttrAbstract) &&
          ((m_attrCopy & (AttrAbstract|AttrFinal)) != (AttrAbstract|AttrFinal))) {
        raise_error(
          "Class %s with %s inheriting 'abstract final' class %s"
          " must also be 'abstract final'",
          m_preClass->name()->data(),
          s___MockClass.data(),
          m_parent->name()->data()
        );
      }
    }
    if ((m_attrCopy & AttrIsConst) && !(parentAttrs & AttrIsConst)) {
      raise_error(
        "Const class %s cannot extend non-const parent %s",
        m_preClass->name()->data(),
        m_parent->name()->data()
      );
    }

    m_preClass->enforceInMaybeSealedParentWhitelist(m_parent->preClass());
    if (m_parent->m_allFlags.m_maybeRedefsPropTy) m_allFlags.m_maybeRedefsPropTy = true;
  }

  // Handle stuff specific to cppext classes
  if (m_parent.get() && m_parent->m_extra->m_instanceCtor) {
    allocExtraData();
    m_extra.raw()->m_instanceCtor = m_parent->m_extra->m_instanceCtor;
    m_extra.raw()->m_instanceCtorUnlocked =
      m_parent->m_extra->m_instanceCtorUnlocked;
    m_extra.raw()->m_instanceDtor = m_parent->m_extra->m_instanceDtor;
    assertx(m_parent->m_releaseFunc == m_parent->m_extra->m_instanceDtor);
    m_releaseFunc = m_parent->m_extra->m_instanceDtor;
    // XXX: should this be copying over the clsInfo also?  Might be broken...
  }
}

static Func* findSpecialMethod(Class* cls, const StringData* name) {
  if (!cls->preClass()->hasMethod(name)) return nullptr;
  Func* f = cls->preClass()->lookupMethod(name);
  f = f->clone(cls);
  f->setBaseCls(cls);
  f->setHasPrivateAncestor(false);
  return f;
}

const StaticString
  s_toString("__toString"),
  s_construct("__construct"),
  s_invoke("__invoke"),
  s_sleep("__sleep"),
  s_debugInfo("__debugInfo"),
  s_clone("__clone");

void Class::setSpecial() {
  m_toString = lookupMethod(s_toString.get());

  /*
   * The invoke method is only cached in the Class for a fast path JIT
   * translation.  If someone defines a weird __invoke (e.g. as a
   * static method), we don't bother caching it here so the translated
   * code won't have to check for that case.
   *
   * Note that AttrStatic on a closure's __invoke Func* means it is a
   * static closure---but the call to __invoke still works as if it
   * were a non-static method call---so they are excluded from that
   * here.  (The closure prologue uninstalls the $this and installs
   * the appropriate static context.)
   */
  m_invoke = lookupMethod(s_invoke.get());
  if (m_invoke && m_invoke->isStaticInPrologue()) {
    m_invoke = nullptr;
  }

  // Look for __construct() declared in either this class or a trait
  auto const func = lookupMethod(s_construct.get());
  if (func && func->cls() == this) {
    if (func->takesInOutParams()) {
      raise_error("Parameters may not be marked inout on constructors");
    }

    m_ctor = func;
    return;
  };

  // Look for parent constructor.
  if (m_parent.get() != nullptr && m_parent->m_ctor) {
    m_ctor = m_parent->m_ctor;
    return;
  }

  if (UNLIKELY(!SystemLib::s_nullCtor)) {
    SystemLib::setupNullCtor(this);
  }

  m_ctor = SystemLib::s_nullCtor;
}

namespace {

inline void raiseIncompat(const PreClass* implementor,
                          const Func* imeth) {
  const char* name = imeth->name()->data();
  raise_error("Declaration of %s::%s() must be compatible with "
              "that of %s::%s()",
              implementor->name()->data(), name,
              imeth->cls()->preClass()->name()->data(), name);
}

inline void checkRefCompat(const char* kind, const Func* self,
                           const Func* inherit) {
  // Shadowing is okay, if we inherit a private method we can't access it
  // anyway.
  if (inherit->attrs() & AttrPrivate) return;

  if (!self->takesInOutParams() && !inherit->takesInOutParams()) return;

  auto const selfPositionals = self->numPositionalParams();
  auto const inheritPositionals = inherit->numPositionalParams();
  auto const selfNamed = self->numNamedParams();
  auto const inheritNamed = inherit->numNamedParams();
  auto const max = std::max(selfPositionals, inheritPositionals);

  for (int i = 0; i < max; ++i) {
    // Since we're looking at ref wrappers of inout functions we need to check
    // inout, but if one of the functions isn't a wrapper we do actually have
    // a mismatch. Named parameters occupy a separate prefix and cannot be
    // inout, so compare positions within the positional suffix.
    auto const smode = i < selfPositionals && self->isInOut(selfNamed + i);
    auto const imode =
      i < inheritPositionals && inherit->isInOut(inheritNamed + i);
    if (smode != imode) {
      auto const msg = [&] {
        auto const sname = self->fullName()->data();
        auto const iname = inherit->fullName()->data();

        if (smode) {
          auto const idecl = i >= inheritPositionals ? "" : "inout ";
          return fmt::format(
            "Parameter {} on function {} was declared inout but is not "
            "declared {}on {} function {}", i + 1, sname, idecl,
            kind, iname);
        } else {
          auto const sdecl = i >= selfPositionals ? "" : "inout ";
          return fmt::format(
            "Parameter {} on function {} was not declared {}but is "
            "declared inout on {} function {}", i + 1, sname, sdecl,
            kind, iname);
        }
      }();
      raise_error(msg);
    }
  }
}

bool isRequiredParam(const Func::ParamInfo& param) {
  return !param.hasDefaultValue() && !param.isOptional();
}

bool namedParamsCompatible(const Func* implementation,
                           const Func* declaration) {
  auto const implementationNames = implementation->sortedNamedParamNames();
  auto const declarationNames = declaration->sortedNamedParamNames();
  auto const numImplementationNames = implementation->numNamedParams();
  auto const numDeclarationNames = declaration->numNamedParams();
  auto const implementationVariadic = implementation->hasVariadicNamedParam();
  auto const declarationVariadic = declaration->hasVariadicNamedParam();

  size_t implementationIdx = 0;
  size_t declarationIdx = 0;
  while (implementationIdx < numImplementationNames &&
         declarationIdx < numDeclarationNames) {
    auto const comparison = implementationNames[implementationIdx]
                              ->slice()
                              .compare(declarationNames[declarationIdx]->slice());
    if (comparison < 0) {
      if (isRequiredParam(implementation->params()[implementationIdx])) {
        return false;
      }
      ++implementationIdx;
    } else if (comparison > 0) {
      if (!implementationVariadic) return false;
      ++declarationIdx;
    } else {
      if (!isRequiredParam(declaration->params()[declarationIdx]) &&
          isRequiredParam(implementation->params()[implementationIdx])) {
        return false;
      }
      ++implementationIdx;
      ++declarationIdx;
    }
  }

  if (declarationIdx != numDeclarationNames && !implementationVariadic) {
    return false;
  }
  for (; implementationIdx < numImplementationNames; ++implementationIdx) {
    if (isRequiredParam(implementation->params()[implementationIdx])) {
      return false;
    }
  }
  return !declarationVariadic || implementationVariadic;
}

// Check compatibility vs interface and abstract declarations
void checkDeclarationCompat(const PreClass* preClass,
                            const Func* func, const Func* imeth) {
  const Func::ParamInfoVec& params = func->params();
  const Func::ParamInfoVec& iparams = imeth->params();

  // If interface has splat parameter then we can't tell without instantiating
  // generics whether the methods are compatible
  // e.g. consider foo(...T) overridden by foo(int,string)
  // hh will already have checked this anyhow
  auto const isplat = imeth->hasSplatParam();
  if (!isplat) {
    auto const ivariadic = imeth->hasVariadicCaptureParam();
    auto const variadic = func->hasVariadicCaptureParam();
    if (ivariadic && !variadic) {
      raiseIncompat(preClass, imeth);
    }

    if (!namedParamsCompatible(func, imeth)) {
      raiseIncompat(preClass, imeth);
    }

    // Verify that func has at least as many positional parameters as imeth.
    // If func is variadic, then the check isn't necessary because it can
    // take an arbitrary number of positional parameters.
    if (!variadic &&
        func->numPositionalParams() < imeth->numPositionalParams()) {
      raiseIncompat(preClass, imeth);
    }

    // Find the leftmost of imeth's contiguous trailing optional positional
    // parameters. Named parameters are matched by label above, not by slot.
    size_t firstOptional = 0;
    for (size_t i = 0; i < imeth->numPositionalParams(); ++i) {
      auto const param = i + imeth->numNamedParams();
      if (isRequiredParam(iparams[param])) firstOptional = i + 1;
    }
    assertx(!ivariadic || iparams[iparams.size() - 1].isVariadic());
    assertx(!ivariadic || params[params.size() - 1].isVariadic());

    // Verify that func provides defaults for every positional parameter after
    // imeth's leftmost trailing optional parameter. Variadic parameters don't
    // have default values and are excluded by numPositionalParams().
    for (auto i = firstOptional; i < func->numPositionalParams(); ++i) {
      auto const param = i + func->numNamedParams();
      if (isRequiredParam(params[param])) raiseIncompat(preClass, imeth);
    }
  }

  checkRefCompat(
    imeth->attrs() & AttrAbstract ? "abstract" : "interface",
    func,
    imeth
  );
}

} // namespace

Class::Class(PreClass* preClass, Class* parent,
             VMCompactVector<ClassPtr>&& usedTraits,
             unsigned classVecLen, unsigned funcVecLen)
  : m_releaseFunc{ObjectData::release}
  , m_preClass(PreClassPtr(preClass))
  , m_instanceBitsIndex{kProfileInstanceBit}
  , m_parent(parent)
#ifndef NDEBUG
  , m_magic{kMagic}
#endif
{
  if (std::numeric_limits<decltype(m_classVecLen)>::max() < classVecLen) {
    raise_error("Class %s has %d parents in the parent chain (maximum is %d)",
                name()->data(), classVecLen,
                std::numeric_limits<decltype(m_classVecLen)>::max());
  }
  m_classVecLen = always_safe_cast<decltype(m_classVecLen)>(classVecLen);

  if (std::numeric_limits<decltype(m_funcVecLen)>::max() < funcVecLen) {
    raise_error("Class %s has %d methods (maximum is %d)",
                name()->data(), funcVecLen,
                std::numeric_limits<decltype(m_funcVecLen)>::max());
  }
  m_funcVecLen = always_safe_cast<decltype(m_funcVecLen)>(funcVecLen);

  m_allFlags.m_maybeRedefsPropTy = false;
  m_allFlags.m_selfMaybeRedefsPropTy = false;
  m_allFlags.m_needsPropInitialCheck = false;
  m_allFlags.m_hasReifiedGenerics = false;
  m_allFlags.m_hasReifiedParent = false;

  SCOPE_FAIL { if (m_extra) delete m_extra.raw(); };

  if (usedTraits.size()) {
    allocExtraData();
    m_extra.raw()->m_usedTraits = std::move(usedTraits);
  }
  setParent();
  setMethods();
  setSpecial();       // must run before setRTAttributes
  setRTAttributes();
  setInterfaces();
  propagateReturnTypes(); // must be run after setting base class and interfaces.
  setEnumType();
  setIncludedEnums();
  setConstants();
  setProperties();    // must run before setInitializers
  setReifiedData();
  setInitializers();
  setClassVec();
  setRequirements();
  setNativeDataInfo();
  initClosure();
  setEnumType();
  setInstanceMemoCacheInfo();
  setLSBMemoCacheInfo();
  checkUserAttributes();

  // A class is allowed to implement two interfaces that share the same slot if
  // we'll fatal trying to define that class, so this has to happen after all
  // of those fatals could be thrown.
  setInterfaceVtables();

  // Calculates the base pointer offset and
  // the MemoryManager index of the class size.
  setReleaseData();

  auto handler = Native::getClassExtraDataHandler(name());
  if (handler) {
    handler(this);
  }
}

namespace {
  bool shouldInheritReturnTypes(const StringData* name) {
    return !(Func::isSpecial(name) ||
      s_toString.get()->same(name) ||
      s_construct.get()->same(name) ||
      s_invoke.get()->same(name) ||
      s_sleep.get()->same(name) ||
      s_debugInfo.get()->same(name) ||
      s_clone.get()->same(name));
  }

  bool shouldInheritTc(
    const Func* current,
    const Func* parent,
    const TypeConstraint& parentTc
  ) {
    if (!parentTc.isCheckable()) return false;

    // Don't pass down the Awaitable<x> type constraint from interfaces
    // and abstract classes to their async overrides.
    return !(current->isAsync()
      && !parent->isAsync()
      && parentTc.typeName()
      && parentTc.typeName()->tsame(s_Awaitable.get()));
  }

  hphp_fast_map<const StringData*, std::vector<const Func*>>
  collectReturnTypes(const Class* cls) {
    hphp_fast_map<const StringData*, std::vector<const Func*>> overrides;

    auto accumulate = [&](const Class* c) {
      for (Slot i = 0; i < c->numMethods(); ++i) {
        auto f = c->getMethod(i);
        if (f->attrs() & AttrPrivate) continue;
        overrides[f->name()].emplace_back(f);
      }
    };

    if (cls->parent() != nullptr) {
      accumulate(cls->parent());
    }
    for (auto const interface : cls->allInterfaces().range()) {
      accumulate(interface);
    }
    return overrides;
  }

  void inheritReturnTypesForMethod(
    Func* f,
    const std::vector<const Func*>& parents
  ) {
    constexpr auto kNormalizeFlags = TypeConstraintFlags::Inherited
      | TypeConstraintFlags::SingleTypeConstraint
      | TypeConstraintFlags::Soft
      | TypeConstraintFlags::Nullable
      | TypeConstraintFlags::DisplayNullable;

    // Flags to merge
    constexpr auto kFlagBitMask = TypeConstraintFlags::Soft
      | TypeConstraintFlags::Nullable;

    // Map from normalized TC to tracked flags.
    hphp_fast_map<TypeConstraint, TypeConstraintFlags, TypeConstraintHasher> seen;
    auto tcs = f->returnTypeConstraints().range();
    for (auto const& tc : tcs) {
      auto normTc = tc;
      normTc.removeFlags(kNormalizeFlags);
      auto flagBits = tc.flags() & kFlagBitMask;
      auto it = seen.find(normTc);
      if (it != seen.end()) {
        it->second = it->second & flagBits;
      } else {
        seen.emplace(std::move(normTc), flagBits);
      }
    }

    // Only collect inherited constraints that aren't already present or that
    // provide stricter types.
    std::vector<TypeConstraint> inherited;
    for (auto const& parent : parents) {
      for (auto const& tc : parent->returnTypeConstraints().range()) {
        if (!shouldInheritTc(f, parent, tc)) continue;

        auto normTc = tc;
        normTc.removeFlags(kNormalizeFlags);
        auto parentFlags = tc.flags() & kFlagBitMask;
        if (Cfg::Eval::EnforceOverriddenReturnTypes == 1) {
          parentFlags = parentFlags | TypeConstraintFlags::Soft;
        }

        bool shouldAdd = false;
        auto it = seen.find(normTc);
        if (it == seen.end()) {
          shouldAdd = true;
          seen.emplace(normTc, parentFlags);
        } else {
          // Inherit the parent TC only if it provides a stricter type,
          // i.e. the child has Soft or Nullable set but the parent does not.
          auto hasStricterFlags = it->second & ~parentFlags;
          if (hasStricterFlags != TypeConstraintFlags::NoFlags) {
            shouldAdd = true;
            it->second = it->second & parentFlags;
          }
        }

        if (shouldAdd) {
          auto copy = tc;
          copy.addFlags(TypeConstraintFlags::Inherited);
          if (Cfg::Eval::EnforceOverriddenReturnTypes == 1) {
            copy.addFlags(TypeConstraintFlags::Soft);
          }
          inherited.push_back(std::move(copy));
        }
      }
    }

    if (!inherited.empty()) {
      // Current return types first, then inherited types.
      std::vector<TypeConstraint> result;
      result.reserve(tcs.size() + inherited.size());
      for (auto const& tc : tcs) {
        result.push_back(tc);
      }
      for (auto& tc : inherited) {
        result.push_back(std::move(tc));
      }
      f->storeInheritedReturnTypes(
        TypeIntersectionConstraint(std::move(result))
      );
      assertx(f->hasInheritedReturnTypes());
    } else {
      assertx(!f->hasInheritedReturnTypes());
    }
  }
}

void Class::propagateReturnTypes() const {
  if (!Cfg::Eval::EnforceOverriddenReturnTypes) return;
  auto overrides = collectReturnTypes(this);
  if (overrides.empty()) return;

  // TODO: Recompute return types for methods present in the parent class
  // but don't have an override in the current class. Such methods may
  // provide an implementation for an interface method which could differ in
  // its return type.
  for (Slot i = 0, n = numMethods(); i < n; ++i) {
    auto const f = getMethod(i);
    // Compute return types for methods that are defined on this class
    // including trait methods imported at runtime.
    if (!f || f->cls() != this) continue;

    // Generators produce values via yield and not RetC. RetC only fires when
    // the generator body finishes, and the value on the stack at that point
    // is always null which indicates the end of the stream.
    if (f->isGenerator()) continue;

    auto const name = f->name();
    auto const it = overrides.find(name);
    if ((it == overrides.end()) || !shouldInheritReturnTypes(name)) continue;

    inheritReturnTypesForMethod(f, it->second);

    // If this is a memoize wrapper, propagate the inherited constraints
    // to the impl method which shares the same return type.
    if (f->hasInheritedReturnTypes() && f->isMemoizeWrapper()) {
      auto impl = lookupMethod(f->memoizeImplName());
      assertx(impl);
      impl->storeInheritedReturnTypes(
        TypeIntersectionConstraint(f->returnTypeConstraints().asVec())
      );
    }
  }
}

void Class::methodOverrideCheck(const Func* parentMethod, const Func* method) {
  // Skip special methods
  if (method->isGenerated()) return;

  if ((parentMethod->attrs() & AttrFinal)) {
    if (m_preClass->userAttributes().find(s___MockClass.get()) ==
        m_preClass->userAttributes().end()) {
      raise_error("Cannot override final method %s::%s()",
                  m_parent->name()->data(), parentMethod->name()->data());
    }
  }

  if (parentMethod->isAsync() && !method->isAsync()) {
    raise_notice(
      Strings::NON_ASYNC_OVERRIDES_ASYNC,
      parentMethod->fullName()->data(),
      m_preClass->name()->data()
    );
  }

  if ((method->attrs() & AttrAbstract) &&
      !(parentMethod->attrs() & AttrAbstract) &&
      !method->isFromTrait()) {
    raise_error("Cannot re-declare non-abstract method %s::%s() abstract in "
                "class %s",
                m_parent->m_preClass->name()->data(),
                parentMethod->name()->data(), m_preClass->name()->data());
  }

  if ((method->attrs()       & (AttrPublic | AttrProtected | AttrPrivate)) >
      (parentMethod->attrs() & (AttrPublic | AttrProtected | AttrPrivate))) {
    raise_error(
      "Access level to %s::%s() must be %s (as in class %s) or weaker",
      m_preClass->name()->data(), method->name()->data(),
      attrToVisibilityStr(parentMethod->attrs()),
      m_parent->name()->data());
  }

  if ((method->attrs() & AttrStatic) != (parentMethod->attrs() & AttrStatic)) {
    raise_error("Cannot change %sstatic method %s::%s() to %sstatic in %s",
                (parentMethod->attrs() & AttrStatic) ? "" : "non-",
                parentMethod->baseCls()->name()->data(),
                method->name()->data(),
                (method->attrs() & AttrStatic) ? "" : "non-",
                m_preClass->name()->data());
  }

  Func* baseMethod = parentMethod->baseCls()->lookupMethod(method->name());
  if (!(method->attrs() & AttrAbstract) &&
      (baseMethod->attrs() & AttrAbstract)) {
    checkDeclarationCompat(m_preClass.get(), method, baseMethod);
  } else {
    checkRefCompat("parent", method, parentMethod);
  }
}

void Class::setMethods() {
  MethodMapBuilder builder;

  ITRACE(5, "----------\nsetMethods() for {}:\n", this->name()->data());
  if (m_parent.get() != nullptr) {
    // Copy down the parent's method entries. These may be overridden below.
    for (Slot i = 0; i < m_parent->m_methods.size(); ++i) {
      Func* f = m_parent->getMethod(i);
      assertx(f);
      ITRACE(5, "  - adding parent method {}\n", f->name()->data());
      assertx(builder.size() == i);
      builder.add(f->name(), f);
    }
  }

  static_assert(AttrPublic < AttrProtected && AttrProtected < AttrPrivate, "");

  auto isPublicTrait = (attrs() & AttrTrait) && !(attrs() & AttrInternal);

  // Overlay/append this class's public/protected methods onto/to those of the
  // parent.
  for (size_t methI = 0; methI < m_preClass->numMethods(); ++methI) {
    Func* method = m_preClass->methods()[methI];
    ITRACE(5, "  - processing pre-class method {}\n", method->name()->data());
    if (Func::isSpecial(method->name())) {
      if (method->name() == s_86sinit.get() ||
          method->name() == s_86pinit.get()) {
        /*
         * we could also skip the cinit function here, but
         * that would mean storing it somewhere else.
         */
        continue;
      }
    }
    // Public traits cannot define internal methods, as the jit would not
    // handle correctly a call into an internal method, unless they have
    // the <<__ModuleLevelTrait>> attribute.
    // Remark: in repo mode the condition `method->isFromTrait()` ensures that
    // the method has been inlined from an internal or a module level trait.
    // Indeed in repo mode, an internal method must have passed the
    // well-formedness checks on the original trait, to be inlined in the current
    // trait from another trait. These ensured that the original trait was an
    // internal or module level trait. Otherwise, if the well-formedness checks
    // had failed, the method would not have been inlined in the current trait.
    if (isPublicTrait &&
        (!m_preClass->userAttributes().contains(s___ModuleLevelTrait.get()))
        && method->isInternal()
        && !(method->isFromTrait())) {
      raise_error(
        "Trait %s is public and therefore cannot define the internal method %s",
        m_preClass->name()->data(), method->name()->data());
    }

    MethodMapBuilder::iterator it2 = builder.find(method->name());
    if (it2 != builder.end()) {
      Func* parentMethod = builder[it2->second];
      // We should never have null func pointers to deal with
      assertx(parentMethod);
      // An abstract method that came from a trait doesn't override another
      // method.
      if (method->isFromTrait() && (method->attrs() & AttrAbstract)) continue;
      methodOverrideCheck(parentMethod, method);
      // Overlay.
      Func* f = method->clone(this);
      Class* baseClass;
      assertx(!(f->attrs() & AttrPrivate) ||
             (parentMethod->attrs() & AttrPrivate));
      if ((parentMethod->attrs() & AttrPrivate) || (f->attrs() & AttrPrivate)) {
        baseClass = this;
      } else {
        baseClass = parentMethod->baseCls();
      }
      f->setBaseCls(baseClass);
      f->setHasPrivateAncestor(
        parentMethod->hasPrivateAncestor() ||
        (parentMethod->attrs() & AttrPrivate));
      builder[it2->second] = f;
    } else {
      // This is the first class that declares the method
      Class* baseClass = this;
      // Append.
      Func* f = method->clone(this);
      f->setBaseCls(baseClass);
      f->setHasPrivateAncestor(false);
      builder.add(method->name(), f);
    }
  }

  // Add 86reifiedinit to base classes that do not have AttrNoReifiedInit set
  if (m_parent.get() == nullptr && !(attrs() & AttrNoReifiedInit)) {
    assertx(builder.find(s_86reifiedinit.get()) == builder.end());
    auto f = SystemLib::getNull86reifiedinit(this);
    builder.add(f->name(), f);
  }

  auto const traitsBeginIdx = builder.size();
  if (m_extra->m_usedTraits.size()) {
    importTraitMethods(builder);
  }
  auto const traitsEndIdx = builder.size();

  if (m_extra) {
    m_extra.raw()->m_traitsBeginIdx = traitsBeginIdx;
    m_extra.raw()->m_traitsEndIdx = traitsEndIdx;
  }

  // If class is not abstract, check that all abstract methods have been defined
  if (!(attrs() & (AttrTrait | AttrInterface | AttrAbstract))) {
    for (Slot i = 0; i < builder.size(); i++) {
      const Func* meth = builder[i];
      if (meth->attrs() & AttrAbstract) {
        raise_error("Class %s contains abstract method (%s) and "
                    "must therefore be declared abstract or implement "
                    "the remaining methods", m_preClass->name()->data(),
                    meth->name()->data());
      }
    }
  }

  // If class is abstract final, its static methods should not be abstract
  if ((attrs() & (AttrAbstract | AttrFinal)) == (AttrAbstract | AttrFinal)) {
    for (Slot i = 0; i < builder.size(); i++) {
      const Func* meth = builder[i];
      if ((meth->attrs() & (AttrAbstract | AttrStatic))
          == (AttrAbstract | AttrStatic)) {
        raise_error(
          "Class %s contains abstract static method (%s) and "
          "therefore cannot be declared 'abstract final'",
          m_preClass->name()->data(), meth->name()->data());
      }
    }
  }

  builder.create(m_methods);
  for (Slot i = 0; i < builder.size(); ++i) {
    builder[i]->setMethodSlot(i);
  }
  setFuncVec(builder);
}

/*
 * Initialize m_RTAttrs by inspecting the class methods and parents.
 */
void Class::setRTAttributes() {
  m_RTAttrs = 0;
  if (lookupMethod(s_sleep.get())) { m_RTAttrs |= Class::HasSleep; }
  if (lookupMethod(s_clone.get())) { m_RTAttrs |= Class::HasClone; }

  if ((isBuiltin() && Native::getNativePropHandler(name())) ||
      (m_parent && m_parent->hasNativePropHandler())) {
    m_RTAttrs |= Class::HasNativePropHandler;
  }
}

/*
 * Perform sanity checks on user attributes:
 * - <<__ModuleLevelTrait>> can only be specified on public traits
 * - a trait with the <<__MethodLevelTrait>> attribute can only use traits
 *   that have the <<__MethodLevelTrait>> attribute themselves
 */
void Class::checkUserAttributes() {
  if (m_preClass->userAttributes().contains(s___ModuleLevelTrait.get())) {
    if (!(attrs() & AttrTrait) || (attrs() & AttrInternal)) {
      raise_error("Attribute <<__ModuleLevelTrait>> can only be specified on public traits");
    }
  }
}

namespace {

// Return the Class which originally defined this constant. This is
// not necessarily the Class where the constant lives! HHBBC can copy
// and propagate constants across the hierarchy. We need to preserve
// the original defining class to make sure type-structure resolution
// behaves properly in this case.
Class* preConstDefiningCls(const PreClass::Const& preConst,
                           Class* thisCls) {
  // The common case is that preConst.cls() is null, which means the
  // defining class is the current class.
  if (!preConst.cls() || preConst.cls() == thisCls->name()) return thisCls;
  auto const cls = Class::lookup(preConst.cls());
  // The actual defining class should be higher up the type hierarchy,
  // so it had better be loaded already at this point.
  always_assert(cls);
  return cls;
}

void setResolvedTypeConstVal(Class::Const& cns, ArrayData* ad) {
  auto const rawData = reinterpret_cast<intptr_t>(ad);
  assertx((rawData & 0x7) == 0 && "ArrayData not 8-byte aligned");
  assertx(tvIsDict(cns.val));
  cns.val.m_data.parr = reinterpret_cast<ArrayData*>(rawData | 0x1);
}

// Initialize the val field of a constant. If this is a type-constant,
// we can store any pre-resolved type-structure in the val from the
// beginning and save a resolution at runtime. If a constant has a
// pre-resolved type-structure, the JIT is allowed to assume that this
// is done (and elide some checks).
void setConstVal(Class::Const& cns, const PreClass::Const& preConst) {
  // Start off with the normal value
  cns.val = preConst.val();

  // Do this check first, to avoid stomping on bits that other const
  // types might use.
  if (preConst.kind() != ConstModifierFlags::Kind::Type) return;
  cns.setPointedClsName(nullptr);

  auto const a = preConst.resolvedTypeStructure().get();
  if (!a) return;

  // We have a type-structure. Make sure we're the right constant type
  // to have one.
  assertx(a->isDictType());
  assertx(a->isStatic());
  assertx(!preConst.isAbstractAndUninit());

  // Initialize the value as if it had been resolved within
  // clsCnsGet(). This prevents the pre-resolved type-structure from
  // being resolved again (which is important, as type-structure
  // resolution is not idempotent).
  setResolvedTypeConstVal(cns, a);

  // Also cache any classname field.
  auto const classname = a->get(s_classname);
  if (tvIsString(classname)) cns.setPointedClsName(classname.val().pstr);
}

}

void Class::importTraitConsts(ConstMap::Builder& builder) {
  auto importConst = [&] (Const&& tConst, bool isFromInterface) {
    auto const declCls = tConst.cls;
    auto const existing = builder.find(tConst.name);

    // The declaring class for type constants can be used to resolve self:: and
    // parent:: references and must therefore refer to the importing class and
    // not the declaring trait.
    if (tConst.kind() == ConstModifierFlags::Kind::Type && !isFromInterface) {
      tConst.cls = this;
    }

    if (existing == builder.end()) {
      builder.add(tConst.name, tConst);
      return;
    }
    auto& existingConst = builder[existing->second];
    auto const existingConstName = existingConst.preConst->cls()
      ? existingConst.preConst->cls()
      : existingConst.cls->name();

    if (tConst.kind() != existingConst.kind()) {
      raise_error("%s cannot inherit the %s %s from %s, because it "
                  "was previously inherited as a %s from %s",
                  m_preClass->name()->data(),
                  ConstModifierFlags::show(tConst.kind()),
                  tConst.name->data(),
                  declCls->name()->data(),
                  ConstModifierFlags::show(existingConst.kind()),
                  existingConstName->data());
    }

    if (tConst.isAbstractAndUninit()) {
      return;
    }

    if (existingConst.isAbstractAndUninit()) {
      existingConst.cls = tConst.cls;
      existingConst.val = tConst.val;
      existingConst.preConst = tConst.preConst;
      return;
    }

    if (Cfg::Eval::TraitConstantInterfaceBehavior) {
      if (existingConst.isAbstract()) {
        // the case where the incoming constant is abstract without a default is covered above
        // there are two remaining cases:
        //   - the incoming constant is abstract with a default
        //   - the incoming constant is concrete
        // In both situations, the incoming constant should win, and separate bookkeeping will
        // cover situations where there are multiple competing defaults.
        existingConst.cls = tConst.cls;
        existingConst.val = tConst.val;
        existingConst.preConst = tConst.preConst;
        return;
      } else { // existing is concrete
        // the existing constant will win over any incoming abstracts and retain a fatal when two
        // concrete constants collide
        if (!tConst.isAbstract() && existingConst.cls != declCls) {
          raise_error("%s cannot inherit the %s %s from %s, because "
                      "it was previously inherited from %s",
                      m_preClass->name()->data(),
                      ConstModifierFlags::show(tConst.kind()),
                      tConst.name->data(),
                      declCls->name()->data(),
                      existingConstName->data());
        }
      }
    } else {
      // Constants in interfaces implemented by traits don't fatal with constants
      // in declInterfaces
      if (isFromInterface) return;

      // Type and Context constants in interfaces can be overridden.
      if (tConst.kind() == ConstModifierFlags::Kind::Type ||
          tConst.kind() == ConstModifierFlags::Kind::Context)  {
        return;
      }
      if (existingConst.cls != declCls) {

        // Constants in traits conflict with constants in declared interfaces
        if (existingConst.cls->attrs() & AttrInterface) {
          for (auto const& interface : m_declInterfaces) {
            auto iface = existingConst.cls;
            if (interface.get() == iface) {
              raise_error("%s cannot inherit the %s %s, because "
                          "it was previously inherited from %s",
                          m_preClass->name()->data(),
                          ConstModifierFlags::show(tConst.kind()),
                          tConst.name->data(),
                          existingConstName->data());
            }
          }
        }
      }
    }
  };

  if (attrs() & AttrNoExpandTrait) {
    Slot traitIdx = m_preClass->numConstants();
    while (traitIdx && m_preClass->constants()[traitIdx - 1].isFromTrait()) {
      traitIdx--;
    }
    for (Slot i = traitIdx; i < m_preClass->numConstants(); i++) {
        auto const* preConst = &m_preClass->constants()[i];
        Const tConst;
        tConst.cls = preConstDefiningCls(*preConst, this);
        tConst.name = preConst->name();
        tConst.preConst = preConst;
        setConstVal(tConst, *preConst);
        importConst(std::move(tConst), false);
    }
    // If we flatten, we need to check implemented interfaces for constants
    for (auto const& traitName : m_preClass->usedTraits()) {
      auto const trait = Class::lookup(traitName);
      assertx(trait->attrs() & AttrTrait);
      int numIfcs = trait->m_interfaces.size();

      for (int i = 0; i < numIfcs; i++) {
        auto interface = trait->m_interfaces[i];
        for (Slot slot = 0; slot < interface->m_constants.size(); ++slot) {
          auto tConst = interface->m_constants[slot];
          importConst(std::move(tConst), true);
        }
      }
    }
  } else if (m_extra) {
    for (auto const& t : m_extra->m_usedTraits) {
      auto trait = t.get();
      for (Slot slot = 0; slot < trait->m_constants.size(); ++slot) {
        auto tConst = trait->m_constants[slot];
        auto const isFromInterface = tConst.cls->attrs() & AttrInterface;
        importConst(std::move(tConst), isFromInterface);
      }
    }
  }
}

void Class::setConstants() {
  ConstMap::Builder builder;

  if (m_parent.get() != nullptr) {
    for (Slot i = 0; i < m_parent->m_constants.size(); ++i) {
      // Copy parent's constants.
      builder.add(m_parent->m_constants[i].name, m_parent->m_constants[i]);
    }
  }

  // Copy in constants off included enums
  if (m_extra) {
    for (int i = 0, size = m_extra->m_includedEnums.size(); i < size; ++i) {
      const Class* includedEnum = m_extra->m_includedEnums[i];

      for (Slot slot = 0; slot < includedEnum->m_constants.size(); ++slot) {
        auto const eConst = includedEnum->m_constants[slot];

        // If you're inheriting a constant with the same name as an existing
        // one, they must originate from the same place, unless the constant
        // was defined as abstract.
        auto const existing = builder.find(eConst.name);

        if (existing == builder.end()) {
          builder.add(eConst.name, eConst);
          continue;
        }

        auto& existingConst = builder[existing->second];

        if (eConst.kind() != existingConst.kind()) {
          raise_error("%s cannot inherit the %s %s from %s, because it "
                     "was previously inherited as a %s from %s",
                     m_preClass->name()->data(),
                     ConstModifierFlags::show(eConst.kind()),
                     eConst.name->data(),
                     eConst.cls->name()->data(),
                     ConstModifierFlags::show(existingConst.kind()),
                     existingConst.cls->name()->data());
        }

        if (eConst.isAbstractAndUninit()) {
          // note: enum class abstract constant cannot have default values
          continue;
        }

        if (existingConst.isAbstract()) {
          // The case where the incoming constant is abstract without a default is covered above.
          // For enum classes, we do not allow default values, so this case is impossible and
          // is a parse error.
          raise_error("In %s, abstract constant %s must not have a default value",
            m_preClass->name()->data(),
            eConst.name->data());
        } else { // existing is concrete
          if (!eConst.isAbstract() && existingConst.cls != eConst.cls) {
            raise_error("%s cannot inherit the %s %s from %s, because "
                      "it was previously inherited from %s",
                      m_preClass->name()->data(),
                      ConstModifierFlags::show(eConst.kind()),
                      eConst.name->data(),
                      eConst.cls->name()->data(),
                      existingConst.cls->name()->data());
          }
        }
      }
    }
  }

  // Copy in interface constants.
  for (int i = 0, size = m_declInterfaces.size(); i < size; ++i) {
    auto const iface = m_declInterfaces[i].get();

    for (Slot slot = 0; slot < iface->m_constants.size(); ++slot) {
      auto const iConst = iface->m_constants[slot];

      // If you're inheriting a constant with the same name as an existing
      // one, they must originate from the same place, unless the constant
      // was defined as abstract.
      auto const existing = builder.find(iConst.name);

      if (existing == builder.end()) {
        builder.add(iConst.name, iConst);
        continue;
      }
      auto& existingConst = builder[existing->second];

      if (iConst.kind() != existingConst.kind()) {
        raise_error("%s cannot inherit the %s %s from %s, because it "
                    "was previously inherited as a %s from %s",
                    m_preClass->name()->data(),
                    ConstModifierFlags::show(iConst.kind()),
                    iConst.name->data(),
                    iConst.cls->name()->data(),
                    ConstModifierFlags::show(existingConst.kind()),
                    existingConst.cls->name()->data());
      }

      if (iConst.isAbstractAndUninit()) {
        continue;
      }

      if (existingConst.isAbstract()) {
        // the case where the incoming constant is abstract without a default is covered above
        // there are two remaining cases:
        //   - the incoming constant is abstract with a default
        //   - the incoming constant is concrete
        // In both situations, the incoming constant should win, and separate bookkeeping will
        // cover situations where there are multiple competing defaults.
        existingConst.cls = iConst.cls;
        existingConst.val = iConst.val;
        existingConst.preConst = iConst.preConst;
      } else { // existing is concrete
        // the existing constant will win over any incoming abstracts and retain a fatal when two
        // concrete constants collide
        if (!iConst.isAbstract() && existingConst.cls != iConst.cls) {
          raise_error("%s cannot inherit the %s %s from %s, because "
                    "it was previously inherited from %s",
                    m_preClass->name()->data(),
                    ConstModifierFlags::show(iConst.kind()),
                    iConst.name->data(),
                    iConst.cls->name()->data(),
                    existingConst.cls->name()->data());
        }
      }
    }
  }

  // Copy in trait constants.
  importTraitConsts(builder);

  for (Slot i = 0, sz = m_preClass->numConstants(); i < sz; ++i) {
    const PreClass::Const* preConst = &m_preClass->constants()[i];

    ConstMap::Builder::iterator it2 = builder.find(preConst->name());
    if (it2 != builder.end()) {
      auto definingClass = builder[it2->second].cls;
      // Forbid redefining constants from interfaces, but not superclasses.
      // Constants from interfaces implemented by superclasses can be overridden.
      // Note that we don't do this check for type constants due to the
      // existence of abstract type constants with defaults, which we don't
      // currently have a way of tracking within HHVM.
      if (definingClass->attrs() & AttrInterface) {
        for (auto interface : m_declInterfaces) {
          if (interface->hasConstant(preConst->name())) {
            raise_error("Cannot override previously defined constant "
                        "%s::%s in %s",
                        builder[it2->second].cls->name()->data(),
                        preConst->name()->data(),
                        m_preClass->name()->data());
          }
        }
      }
      // Forbid redefining constants from included enums
      if ((definingClass->attrs() & (AttrEnum|AttrEnumClass)) && m_extra) {
        for (int j = 0, size = m_extra->m_includedEnums.size(); j < size; ++j) {
          const Class* includedEnum = m_extra->m_includedEnums[j];
          if (includedEnum->hasConstant(preConst->name())) {
            raise_error("Cannot override previously defined constant "
                        "%s::%s in %s",
                        builder[it2->second].cls->name()->data(),
                        preConst->name()->data(),
                        m_preClass->name()->data());
          }
        }
      }
      if (preConst->isAbstractAndUninit() &&
          !builder[it2->second].isAbstractAndUninit()) {
        raise_error("Cannot re-declare as abstract previously defined "
                    "%s %s::%s in %s",
                    ConstModifierFlags::show(builder[it2->second].kind()),
                    builder[it2->second].cls->name()->data(),
                    preConst->name()->data(),
                    m_preClass->name()->data());
      }

      if (preConst->kind() != builder[it2->second].kind()) {
        raise_error("Cannot re-declare as a %s previously defined "
                    "%s %s::%s in %s",
                    ConstModifierFlags::show(preConst->kind()),
                    ConstModifierFlags::show(builder[it2->second].kind()),
                    builder[it2->second].cls->name()->data(),
                    preConst->name()->data(),
                    m_preClass->name()->data());
      }
      builder[it2->second].cls = preConstDefiningCls(*preConst, this);
      builder[it2->second].preConst = preConst;
      setConstVal(builder[it2->second], *preConst);
    } else {
      // Append constant.
      Const constant;
      constant.cls = preConstDefiningCls(*preConst, this);
      constant.name = preConst->name();
      constant.preConst = preConst;
      setConstVal(constant, *preConst);
      builder.add(preConst->name(), constant);
    }
  }

  // If class is not abstract, all abstract constants should have been
  // defined
  if (!(attrs() & (AttrTrait | AttrInterface | AttrAbstract))) {
    for (Slot i = 0; i < builder.size(); i++) {
      const Const& constant = builder[i];
      if (constant.isAbstractAndUninit()) {
        raise_error("Class %s contains abstract %s (%s) and "
                    "must therefore be declared abstract or define "
                    "the remaining constants",
                    m_preClass->name()->data(),
                    ConstModifierFlags::show(constant.kind()),
                    constant.name->data());
      }
    }
  }

  // If class is abstract final, its constants should not be abstract
  else if (
    (attrs() & (AttrAbstract | AttrFinal)) == (AttrAbstract | AttrFinal)) {
    for (Slot i = 0; i < builder.size(); i++) {
      const Const& constant = builder[i];
      if (constant.isAbstractAndUninit()) {
        raise_error(
          "Class %s contains abstract %s (%s) and "
          "therefore cannot be declared 'abstract final'",
          m_preClass->name()->data(),
          ConstModifierFlags::show(constant.kind()),
          constant.name->data());
      }
    }
  }

  // For type constants, we have to use the value from the PreClass of the
  // declaring class, because the parent class or interface we got it from may
  // have replaced it with a resolved value.
  for (auto& pair : builder) {
    auto& cns = builder[pair.second];
    if (cns.kind() == ConstModifierFlags::Kind::Type) {
      setConstVal(cns, *cns.preConst);
    }

    // Concretize inherited abstract type constants with defaults
    if (isNormalClass(this) && !isAbstract(this)) {
      if (cns.isAbstract() && cns.val.is_init()) {
        cns.concretize();
      }
    }
  }

  m_constants.create(builder);

  // Classes with thrift specs require extra data.
  const static StaticString s_SPEC("SPEC");
  auto const clsCnsInd = m_constants.findIndex(s_SPEC.get());
  if (clsCnsInd != kInvalidSlot) {
    allocExtraData();
  }
}

namespace {

void copyDeepInitAttr(const PreClass::Prop* pclsProp, Class::Prop* clsProp) {
  if (pclsProp->attrs() & AttrDeepInit) {
    clsProp->attrs = (Attr)(clsProp->attrs | AttrDeepInit);
  } else {
    clsProp->attrs = (Attr)(clsProp->attrs & ~AttrDeepInit);
  }
}

/*
 * KeyFn should be a function that takes an index and returns a key to sort by.
 * To sort lexicographically by multiple fields, the key can be a tuple type.
 */
template<typename KeyFn>
void sortByKey(std::vector<uint32_t>& indices, KeyFn keyFn) {
  std::sort(indices.begin(), indices.end(), [&](uint32_t a, uint32_t b) {
    return keyFn(a) < keyFn(b);
  });
}

}

void Class::sortOwnProps(const PropMap::Builder& curPropMap,
                         uint32_t first,
                         uint32_t past,
                         std::vector<uint16_t>& slotIndex) {
  auto const serverMode = Cfg::Server::Mode;
  FTRACE(3, "Class::sortOwnProps: PreClass: {}\n", m_preClass->name()->data());
  if (serverMode && Cfg::Server::LogReorderProps) {
    Logger::FInfo("Class::sortOwnProps: PreClass: {}",
                  m_preClass->name()->data());
  }
  auto const size = past - first;
  if (size == 0) return; // no own props
  std::vector<uint32_t> order(size);
  for (uint32_t i = 0; i < size; i++) {
    order[i] = first + i;
  }
  // We don't change the order of the properties for closures.
  if (c_Closure::initialized() && parent() != c_Closure::classof()) {
    if (RuntimeOption::EvalReorderProps == "hotness") {
      /* Sort the properties in decreasing order of their profile counts,
       * according to the serialized JIT profile data.  In case of ties, we
       * preserve the logical order among the properties.  Note that, in the
       * absence of profile data (e.g. if jumpstart failed to deserialize the
       * JIT profile data), then the physical layout of the properties in memory
       * will match their logical order. */
      sortByKey(order, [&](uint32_t index) {
        auto const& prop = curPropMap[index];
        int64_t count = PropertyProfile::getCount(prop.cls->name(), prop.name);
        return std::make_tuple(-count, index);
      });
    } else if (RuntimeOption::EvalReorderProps == "alphabetical") {
      std::sort(order.begin(), order.end(),
                [&] (uint32_t a, uint32_t b) {
                  auto const& propa = curPropMap[a];
                  auto const& propb = curPropMap[b];
                  return strcmp(propa.name->data(), propb.name->data()) < 0;
                });
    } else if (RuntimeOption::EvalReorderProps == "countedness") {
      // Countable properties come earlier. Break ties by logical order.
      sortByKey(order, [&](uint32_t index) {
        auto const& prop = curPropMap[index];
        int64_t countable = jit::irgen::propertyMayBeCountable(prop);
        return std::make_tuple(-countable, index);
      });
    } else if (RuntimeOption::EvalReorderProps == "countedness-hotness") {
      // Countable properties come earlier. Break ties by profile counts
      // (assuming that we have them from jumpstart), then by logical order.
      sortByKey(order, [&](uint32_t index) {
        auto const& prop = curPropMap[index];
        int64_t count = PropertyProfile::getCount(prop.cls->name(), prop.name);
        int64_t countable = jit::irgen::propertyMayBeCountable(prop);
        return std::make_tuple(-countable, -count, index);
      });
    }
  }
  assertx(slotIndex.size() == past);
  for (uint32_t i = 0; i < size; i++) {
    auto slot = order[i];
    auto index = first + i;
    slotIndex[slot] = index;
    auto const& prop = curPropMap[slot];
    FTRACE(
      3, "  index={}: slot={}, prop={}, count={}, countable={}\n",
      index, slot, prop.name->data(),
      PropertyProfile::getCount(prop.cls->name(), prop.name),
      jit::irgen::propertyMayBeCountable(prop)
    );
    if (serverMode && Cfg::Server::LogReorderProps) {
      Logger::FInfo(
        "  index={}: slot={}, prop={}, count={}, countable={}",
        index, slot, prop.name->data(),
        PropertyProfile::getCount(prop.cls->name(), prop.name),
        jit::irgen::propertyMayBeCountable(prop)
      );
    }
  }
  sortOwnPropsInitVec(first, past, slotIndex);
}

void Class::sortOwnPropsInitVec(uint32_t first, uint32_t past,
                                const std::vector<uint16_t>& slotIndex) {
  auto const size = past - first;
  std::vector<TypedValue> tmpPropInitVec(size);
  std::vector<bool> tmpDeepInit(size);
  for (uint32_t i = first; i < past; i++) {
    tvCopy(m_declPropInit[i].val.tv(), tmpPropInitVec[i - first]);
    tmpDeepInit[i - first] = m_declPropInit[i].deepInit;
  }
  for (uint32_t slot = first; slot < past; slot++) {
    auto index = slotIndex[slot];
    FTRACE(3, "  - placing propInit for slot {} at index {}\n", slot, index);
    tvCopy(tmpPropInitVec[slot - first], m_declPropInit[index].val);
    m_declPropInit[index].deepInit = tmpDeepInit[slot - first];
  }
}

void Class::setupSProps() {
  auto const n = m_staticProperties.size();
  if (!n) return;

  using LinkT = std::remove_pointer<decltype(m_sPropCache)>::type;
  m_sPropCache = reinterpret_cast<LinkT*>(vm_malloc(n * sizeof(LinkT)));

  for (unsigned i = 0; i < n; ++i) {
    new (&m_sPropCache[i]) LinkT;
  }
}

std::atomic<void*>* Class::getThriftData() const {
  if (!m_extra) return nullptr;
  return &m_extra->m_thriftData;
}

void Class::setProperties() {
  int numInaccessible = 0;
  PropMap::Builder curPropMap;
  SPropMap::Builder curSPropMap;
  m_allFlags.m_hasDeepInitProps = false;
  std::vector<uint16_t> slotIndex;

  if (m_parent.get() != nullptr) {
    // m_hasDeepInitProps indicates if there are properties that require
    // deep initialization. Note there are cases where m_hasDeepInitProps is
    // true but none of the properties require deep initialization; this can
    // happen if a derived class redeclares a public or protected property
    // from an ancestor class. We still get correct behavior in these cases,
    // so it works out okay.
    m_allFlags.m_hasDeepInitProps = m_parent->m_allFlags.m_hasDeepInitProps;
    for (auto const& parentProp : m_parent->declProperties()) {
      // Copy parent's declared property.  Protected properties may be
      // weakened to public below, but otherwise, the parent's properties
      // will stay the same for this class.
      Prop prop;
      prop.preProp             = parentProp.preProp;
      prop.cls                 = parentProp.cls;
      prop.baseCls             = parentProp.baseCls;
      prop.attrs               = parentProp.attrs | AttrNoBadRedeclare;
      prop.typeConstraints     = parentProp.typeConstraints;
      prop.name                = parentProp.name;
      prop.repoAuthType        = parentProp.repoAuthType;

      if (!(parentProp.attrs & AttrPrivate)) {
        curPropMap.add(prop.name, prop);
      } else {
        ++numInaccessible;
        curPropMap.addUnnamed(prop);
      }
    }
    m_declPropInit = m_parent->m_declPropInit;
    m_allFlags.m_needsPropInitialCheck = m_parent->m_allFlags.m_needsPropInitialCheck;
    auto& parentSlotIndex = m_parent->m_slotIndex;
    slotIndex.insert(slotIndex.end(),
                     parentSlotIndex.begin(), parentSlotIndex.end());
    for (auto const& parentProp : m_parent->staticProperties()) {
      if (!inherited(parentProp)) continue;

      // Alias parent's static property.
      SProp sProp;
      sProp.preProp         = parentProp.preProp;
      sProp.name            = parentProp.name;
      sProp.attrs           = parentProp.attrs | AttrNoBadRedeclare;
      sProp.typeConstraints = parentProp.typeConstraints;
      sProp.cls             = parentProp.cls;
      sProp.repoAuthType    = parentProp.repoAuthType;
      tvWriteUninit(sProp.val);
      curSPropMap.add(sProp.name, sProp);
    }
  }

  Slot traitIdx = m_preClass->numProperties();
  if (m_preClass->attrs() & AttrNoExpandTrait) {
    while (traitIdx &&
           (m_preClass->properties()[traitIdx - 1].attrs() & AttrTrait)) {
      traitIdx--;
    }
  }

  Slot serializationIdx = 0;
  std::vector<bool> serializationVisited(curPropMap.size(), false);
  Slot staticSerializationIdx = 0;
  std::vector<bool> staticSerializationVisited(curSPropMap.size(), false);

  static_assert(AttrPublic < AttrProtected && AttrProtected < AttrPrivate, "");
  auto const firstOwnPropSlot = curPropMap.size();

  for (Slot slot = 0; slot < traitIdx; ++slot) {
    const PreClass::Prop* preProp = &m_preClass->properties()[slot];

    if (!(preProp->attrs() & AttrStatic)) {
      // Overlay/append this class's protected and public properties onto/to
      // those of the parent, and append this class's private properties.
      // Append order doesn't matter here (unlike in setMethods()).
      // Prohibit static-->non-static redeclaration.
      SPropMap::Builder::iterator it5 = curSPropMap.find(preProp->name());
      if (it5 != curSPropMap.end()) {
        raise_error("Cannot redeclare static %s::$%s as non-static %s::$%s",
                    curSPropMap[it5->second].cls->name()->data(),
                    preProp->name()->data(), m_preClass->name()->data(),
                    preProp->name()->data());
      }
      // Get parent's equivalent property, if one exists.
      const Prop* parentProp = nullptr;
      if (m_parent.get() != nullptr) {
        Slot id = m_parent->m_declProperties.findIndex(preProp->name());
        if (id != kInvalidSlot) {
          parentProp = &m_parent->m_declProperties[id];
        }
      }
      // Prohibit strengthening.
      if (parentProp
          && (preProp->attrs() & VisibilityAttrs)
             > (parentProp->attrs & VisibilityAttrs)) {
        raise_error(
          "Access level to %s::$%s must be %s (as in class %s) or weaker",
          m_preClass->name()->data(), preProp->name()->data(),
          attrToVisibilityStr(parentProp->attrs),
          m_parent->name()->data());
      }
      if (preProp->attrs() & AttrDeepInit) {
        m_allFlags.m_hasDeepInitProps = true;
      }

      auto isPublicTrait = isTrait(this) && !(isInternal());

      auto addNewProp = [&] {
        Prop prop;
        initProp(prop, preProp);

        // Public traits cannot define internal properties.  Enforcing this
        // is especially important for traits labelled <<__ModuleLevelTraits>>
        // (which are enforced to be public), as the jit would not handle correctly
        // accesses to internal properties of a <<__ModuleLevelTrait>>
        if (isPublicTrait && prop.isInternal()) {
          raise_error(
             "Trait %s is public and therefore cannot define the internal property %s",
             m_preClass->name()->data(), prop.name->data());
        }

        curPropMap.add(preProp->name(), prop);
        curPropMap[serializationIdx++].serializationIdx = curPropMap.size() - 1;
        serializationVisited.push_back(true);
        m_declPropInit.push_back(preProp->val());
        slotIndex.push_back(slotIndex.size());
      };

      auto const lateInitCheck = [&] (const Class::Prop& prop) {
        if ((prop.attrs ^ preProp->attrs()) & AttrLateInit) {
          raise_error(
            "Property %s::$%s must %sbe <<__LateInit>> (as in class %s)",
            m_preClass->name()->data(),
            preProp->name()->data(),
            prop.attrs & AttrLateInit ? "" : "not ",
            m_parent->name()->data()
          );
        }
      };

      auto const redeclareProp = [&] (const Slot slot) {
        // For duplicate property name, add the slot # defined in curPropMap,
        // and mark the property visited.
        assertx(serializationVisited.size() > slot);
        if (!serializationVisited[slot]) {
          curPropMap[serializationIdx++].serializationIdx = slot;
          serializationVisited[slot] = true;
        }

        auto& prop = curPropMap[slot];
        assertx((preProp->attrs() & VisibilityAttrs)
                <= (prop.attrs & VisibilityAttrs));
        assertx(!(prop.attrs & AttrNoImplicitNullable) ||
                (preProp->attrs() & AttrNoImplicitNullable));
        assertx(prop.attrs & AttrNoBadRedeclare);

        if ((preProp->attrs() & AttrIsConst) != (prop.attrs & AttrIsConst)) {
          raise_error("Cannot redeclare property %s of class %s with different "
                      "constness in class %s", preProp->name()->data(),
                      m_parent->name()->data(), m_preClass->name()->data());
        }

        lateInitCheck(prop);

        prop.preProp = preProp;
        prop.cls = this;
        if ((preProp->attrs() & VisibilityAttrs)
            < (prop.attrs & VisibilityAttrs)) {
          assertx((prop.attrs & VisibilityAttrs) == AttrProtected);
          assertx((preProp->attrs() & VisibilityAttrs) == AttrPublic);
          // Weaken protected property to public.
          prop.attrs = Attr(prop.attrs ^ (AttrProtected|AttrPublic));
        }

        auto const& tcs = preProp->typeConstraints();
        if (Cfg::Eval::CheckPropTypeHints > 0 &&
          !(preProp->attrs() & AttrNoBadRedeclare) &&
          tcs.maybeInequivalentForProp(prop.typeConstraints)
        ) {
          // If this property isn't obviously not redeclaring a property in
          // the parent, we need to check that when we initialize the class.
          prop.attrs = Attr(prop.attrs & ~AttrNoBadRedeclare);
          m_allFlags.m_selfMaybeRedefsPropTy = true;
          m_allFlags.m_maybeRedefsPropTy = true;
        }

        prop.typeConstraints = preProp->typeConstraints();
        prop.repoAuthType = preProp->repoAuthType();

        if (preProp->attrs() & AttrNoImplicitNullable) {
          prop.attrs |= AttrNoImplicitNullable;
        }
        attrSetter(prop.attrs,
                   preProp->attrs() & AttrSystemInitialValue,
                   AttrSystemInitialValue);
        attrSetter(prop.attrs,
                   preProp->attrs() & AttrInitialSatisfiesTC,
                   AttrInitialSatisfiesTC);
        attrSetter(prop.attrs,
                   preProp->attrs() & AttrPersistent,
                   AttrPersistent);

        checkPrePropVal(prop, preProp);
        auto index = slotIndex[slot];

        tvCopy(preProp->val(), m_declPropInit[index].val);
        copyDeepInitAttr(preProp, &prop);
      };

      switch (preProp->attrs() & VisibilityAttrs) {
        case AttrPrivate: {
          addNewProp();
          break;
        }
        case AttrProtected: {
          // Check whether a superclass has already declared this protected
          // property.
          PropMap::Builder::iterator it2 = curPropMap.find(preProp->name());
          if (it2 == curPropMap.end()) {
            addNewProp();
          } else {
            redeclareProp(it2->second);
          }
          break;
        }
        case AttrPublic: {
          // Check whether a superclass has already declared this as a
          // protected/public property.
          auto it2 = curPropMap.find(preProp->name());
          if (it2 == curPropMap.end()) {
            addNewProp();
          } else {
            redeclareProp(it2->second);
          }
          break;
        }
        default: always_assert(false);
      }
    } else { // Static property.
      // Prohibit non-static-->static redeclaration.
      auto const it2 = curPropMap.find(preProp->name());
      if (it2 != curPropMap.end()) {
        auto& prop = curPropMap[it2->second];
        raise_error("Cannot redeclare non-static %s::$%s as static %s::$%s",
                    prop.cls->name()->data(),
                    preProp->name()->data(),
                    m_preClass->name()->data(),
                    preProp->name()->data());
      }
      // Get parent's equivalent property, if one exists.
      auto const it3 = curSPropMap.find(preProp->name());
      Slot sPropInd = kInvalidSlot;
      // Prohibit strengthening.
      if (it3 != curSPropMap.end()) {
        const SProp& parentSProp = curSPropMap[it3->second];
        if ((preProp->attrs() & VisibilityAttrs)
            > (parentSProp.attrs & VisibilityAttrs)) {
          raise_error(
            "Access level to %s::$%s must be %s (as in class %s) or weaker",
            m_preClass->name()->data(), preProp->name()->data(),
            attrToVisibilityStr(parentSProp.attrs),
            m_parent->name()->data());
        }
        // Prohibit overlaying LSB static properties.
        if (parentSProp.attrs & AttrLSB) {
          raise_error(
            "Cannot redeclare LSB static %s::%s as %s::%s",
            parentSProp.cls->name()->data(),
            preProp->name()->data(),
            m_preClass->name()->data(),
            preProp->name()->data());
        }
        sPropInd = it3->second;
      }

      if (sPropInd == kInvalidSlot) {
        SProp sProp;
        initProp(sProp, preProp);
        curSPropMap.add(sProp.name, sProp);

        curSPropMap[staticSerializationIdx++].serializationIdx =
          curSPropMap.size() - 1;
        staticSerializationVisited.push_back(true);
      } else {
        // Overlay ancestor's property.
        auto& sProp = curSPropMap[sPropInd];
        initProp(sProp, preProp);

        assertx(staticSerializationVisited.size() > sPropInd);
        if (!staticSerializationVisited[sPropInd]) {
          staticSerializationVisited[sPropInd] = true;
          curSPropMap[staticSerializationIdx++].serializationIdx = sPropInd;
        }
      }
    }
  }

  importTraitProps(traitIdx, curPropMap, curSPropMap, slotIndex,
                   serializationIdx, serializationVisited,
                   staticSerializationIdx, staticSerializationVisited);

  auto const pastOwnPropSlot = curPropMap.size();
  sortOwnProps(curPropMap, firstOwnPropSlot, pastOwnPropSlot, slotIndex);
  assertx(slotIndex.size() == curPropMap.size());

  // set serialization index for parent properties at the end
  if (m_parent.get() != nullptr) {
    for (Slot i = 0; i < m_parent->declProperties().size(); ++i) {
      // For non-static properties, slot is always equal to parentSlot
      Slot slot = m_parent->declProperties()[i].serializationIdx;
      assertx(serializationVisited.size() > slot);
      if (!serializationVisited[slot]) {
        curPropMap[serializationIdx++].serializationIdx = slot;
      }
    }

    for (Slot i = 0; i < m_parent->staticProperties().size(); ++i) {
      Slot parentSlot = m_parent->staticProperties()[i].serializationIdx;
      auto const& prop = m_parent->staticProperties()[parentSlot];

      if (!inherited(prop)) continue;

      auto it = curSPropMap.find(prop.name);
      assertx(it != curSPropMap.end());

      Slot slot = it->second;
      assertx(staticSerializationVisited.size() > slot);
      if (!staticSerializationVisited[slot]) {
        curSPropMap[staticSerializationIdx++].serializationIdx = slot;
      }
    }

    assertx(serializationIdx == curPropMap.size());
    assertx(staticSerializationIdx == curSPropMap.size());
  }

  // LSB static properties that were inherited must be initialized separately.
  for (Slot slot = 0; slot < curSPropMap.size(); ++slot) {
    auto& sProp = curSPropMap[slot];
    if ((sProp.attrs & AttrLSB) && sProp.cls != this) {
      auto const& prevSProps = sProp.cls->m_staticProperties;
      auto prevInd = prevSProps.findIndex(sProp.name);
      assertx(prevInd != kInvalidSlot);
      sProp.val = prevSProps[prevInd].val;
    }
  }

  m_declProperties.create(curPropMap);
  m_staticProperties.create(curSPropMap);

  std::vector<ObjectProps::quick_index> slotQuickIndex;
  slotQuickIndex.reserve(slotIndex.size());
  for (auto i : slotIndex) {
    slotQuickIndex.push_back(ObjectProps::quickIndex(i));
  }
  m_slotIndex = slotQuickIndex;

  setupSProps();

  m_declPropNumAccessible = m_declProperties.size() - numInaccessible;

  // To speed up ObjectData::release, we only iterate over property indices
  // up to the last countable property index. Here, "index" refers to the
  // position of the property in memory, and "slot" to its logical slot.
  uint16_t countablePropsEnd = 0;
  for (Slot slot = 0; slot < m_declProperties.size(); ++slot) {
    if (jit::irgen::propertyMayBeCountable(m_declProperties[slot])) {
      auto const index =
        safe_cast<uint16_t>(propSlotToIndex(slot) + uint16_t{1});
      countablePropsEnd = std::max(countablePropsEnd, index);
    }
  }

  assertx(m_declProperties.size() <= ObjectProps::max_index + 1);
  assertx(countablePropsEnd <= ObjectProps::max_index);

  m_countablePropsEnd = ObjectProps::quickIndex(countablePropsEnd);
  FTRACE(3, "numDeclProperties = {}\n", m_declProperties.size());
  FTRACE(3, "countablePropsEnd = {}\n", countablePropsEnd);
}

bool Class::compatibleTraitPropInit(const TypedValue& tv1,
                                    const TypedValue& tv2) {
  if (tv1.m_type != tv2.m_type) return false;

  switch (tv1.m_type) {
    case KindOfNull:
      return true;

    case KindOfBoolean:
    case KindOfInt64:
    case KindOfDouble:
    case KindOfPersistentString:
    case KindOfString:
      return same(tvAsCVarRef(&tv1), tvAsCVarRef(&tv2));

    case KindOfUninit:
    case KindOfPersistentVec:
    case KindOfVec:
    case KindOfPersistentDict:
    case KindOfDict:
    case KindOfPersistentKeyset:
    case KindOfKeyset:
    case KindOfObject:
    case KindOfResource:
    case KindOfRFunc:
    case KindOfFunc:
    case KindOfClass:
    case KindOfLazyClass:
    case KindOfClsMeth:
    case KindOfRClsMeth:
    case KindOfEnumClassLabel:
      return false;
  }
  not_reached();
}

namespace {

const constexpr Attr kRedeclarePropAttrMask =
  Attr(
    ~(AttrNoBadRedeclare |
      AttrSystemInitialValue |
      AttrNoImplicitNullable |
      AttrInitialSatisfiesTC |
      AttrPersistent)
  );

}

void Class::importTraitInstanceProp(Prop& traitProp,
                                    TypedValue traitPropVal,
                                    PropMap::Builder& curPropMap,
                                    SPropMap::Builder& curSPropMap,
                                    std::vector<uint16_t>& slotIndex,
                                    Slot& serializationIdx,
                                    std::vector<bool>& serializationVisited) {
  // Check if prop already declared as static
  if (curSPropMap.find(traitProp.name) != curSPropMap.end()) {
    raise_error("trait declaration of property '%s' is incompatible with "
                "previous declaration", traitProp.name->data());
  }

  if ((attrs() & AttrIsConst) && !(traitProp.attrs & AttrIsConst)) {
    raise_error("trait's non-const declaration of property '%s' is "
                "incompatible with a const class", traitProp.name->data());
  }

  auto prevIt = curPropMap.find(traitProp.name);

  if (prevIt == curPropMap.end()) {
    // New prop, go ahead and add it
    auto prop = traitProp;
    prop.baseCls = this;
    if (prop.attrs & AttrDeepInit) {
      m_allFlags.m_hasDeepInitProps = true;
    }
    if (traitProp.cls != this) {
      // this was a non-flattened trait property.
      prop.cls = this;

      // Clear NoImplicitNullable on the property. HHBBC analyzed the property
      // in the context of the trait, not this class, so we cannot predict what
      // derived class' will do with it. Be conservative.
      prop.attrs = Attr(
        prop.attrs & ~AttrNoImplicitNullable) | AttrNoBadRedeclare;
    } else {
      assertx(prop.attrs & AttrNoBadRedeclare);
    }

    curPropMap.add(prop.name, prop);
    curPropMap[serializationIdx++].serializationIdx = curPropMap.size() - 1;
    serializationVisited.push_back(true);
    slotIndex.push_back(slotIndex.size());
    m_declPropInit.push_back(traitPropVal);
  } else {
    // Redeclared prop, make sure it matches previous declarations
    auto& prevProp    = curPropMap[prevIt->second];
    auto  prevPropIndex = slotIndex[prevIt->second];
    auto const prevPropVal = m_declPropInit[prevPropIndex].val.tv();
    if (((prevProp.attrs ^ traitProp.attrs) & kRedeclarePropAttrMask) ||
        (!(prevProp.attrs & AttrSystemInitialValue) &&
         !(traitProp.attrs & AttrSystemInitialValue) &&
         !compatibleTraitPropInit(prevPropVal, traitPropVal))) {
      raise_error("trait declaration of property '%s' is incompatible with "
                    "previous declaration", traitProp.name->data());
    }

    assertx(serializationVisited.size() > prevIt->second);
    if (!serializationVisited[prevIt->second]) {
      curPropMap[serializationIdx++].serializationIdx = prevIt->second;
      serializationVisited[prevIt->second] = true;
    }

    if (prevProp.cls != this) {
      prevProp.repoAuthType = traitProp.repoAuthType;
      attrSetter(
        prevProp.attrs,
        traitProp.attrs & AttrPersistent,
        AttrPersistent
      );
    }
  }
}

void Class::importTraitStaticProp(
  SProp& traitProp,
  PropMap::Builder& curPropMap,
  SPropMap::Builder& curSPropMap,
  Slot& staticSerializationIdx,
  std::vector<bool>& staticSerializationVisited) {
  // Check if prop already declared as non-static
  if (curPropMap.find(traitProp.name) != curPropMap.end()) {
    raise_error("trait declaration of property '%s' is incompatible with "
                "previous declaration", traitProp.name->data());
  }

  auto prevIt = curSPropMap.find(traitProp.name);
  if (prevIt == curSPropMap.end()) {
    // New prop, go ahead and add it
    auto prop = traitProp;
    prop.cls = this; // set current class as the first declaring prop
    prop.attrs |= AttrNoBadRedeclare;

    curSPropMap.add(prop.name, prop);
    curSPropMap[staticSerializationIdx++].serializationIdx =
      curSPropMap.size() - 1;
    staticSerializationVisited.push_back(true);
  } else {
    // Redeclared prop, make sure it matches previous declaration
    auto& prevProp = curSPropMap[prevIt->second];
    TypedValue prevPropVal;
    if (prevProp.attrs & AttrLSB) {
      raise_error("trait declaration of property '%s' would redeclare "
                  "LSB static", traitProp.name->data());
    }
    if (prevProp.cls == this) {
      // If this static property was declared by this class, we can get the
      // initial value directly from its value.
      prevPropVal = prevProp.val;
    } else {
      // If this static property was declared in a parent class, its value will
      // be KindOfUninit, and we'll need to consult the appropriate parent class
      // to get the initial value.
      auto const& prevSProps = prevProp.cls->m_staticProperties;

      auto prevPropInd = prevSProps.findIndex(prevProp.name);
      assertx(prevPropInd != kInvalidSlot);

      prevPropVal = prevSProps[prevPropInd].val;

      prevProp.repoAuthType = traitProp.repoAuthType;
      attrSetter(
        prevProp.attrs,
        traitProp.attrs & AttrPersistent,
        AttrPersistent
      );
    }
    if (((prevProp.attrs ^ traitProp.attrs) & kRedeclarePropAttrMask) ||
        (!(prevProp.attrs & AttrSystemInitialValue) &&
         !(traitProp.attrs & AttrSystemInitialValue) &&
         !compatibleTraitPropInit(traitProp.val, prevPropVal))) {
      raise_error("trait declaration of property '%s' is incompatible with "
                  "previous declaration", traitProp.name->data());
    }
    prevProp.cls = this;
    prevProp.val = prevPropVal;

    assertx(staticSerializationVisited.size() > prevIt->second);
    if (!staticSerializationVisited[prevIt->second]) {
      curSPropMap[staticSerializationIdx++].serializationIdx = prevIt->second;
      staticSerializationVisited[prevIt->second] = true;
    }
  }
}

template<typename XProp>
void Class::checkPrePropVal(XProp& prop, const PreClass::Prop* preProp) {
  auto const& tv = preProp->val();
  assertx(
    !(preProp->attrs() & AttrSystemInitialValue) ||
    tv.m_type != KindOfNull ||
    !(preProp->attrs() & AttrNoImplicitNullable)
  );

  auto const alwaysPassesAll = [&] {
    for (auto const& tc : preProp->typeConstraints().range()) {
      assertx(tc.validForProp());
      if (!tc.alwaysPasses(&tv)) return false;
    }
    return true;
  }();

  if (alwaysPassesAll) {
    prop.attrs |= AttrInitialSatisfiesTC;
    return;
  }

  // Otherwise the property can have a type which isn't allowed for
  // properties, so we need to check.
  m_allFlags.m_needsPropInitialCheck = true;
  if (preProp->attrs() & AttrStatic) {
    prop.attrs = Attr(prop.attrs & ~AttrPersistent);
  }

  if (Cfg::Eval::CheckPropTypeHints > 0 &&
      !(preProp->attrs() & AttrInitialSatisfiesTC) &&
      (preProp->attrs() & AttrSystemInitialValue) &&
      tv.m_type != KindOfUninit) {
    // System provided initial values should always be correct
    prop.attrs |= AttrInitialSatisfiesTC;
  }
}

template<typename XProp>
void Class::initProp(XProp& prop, const PreClass::Prop* preProp) {
  prop.preProp             = preProp;
  prop.name                = preProp->name();
  prop.attrs               = Attr(preProp->attrs() & ~AttrTrait) |
                             AttrNoBadRedeclare;
  // This is the first class to declare this property
  prop.cls                 = this;
  prop.typeConstraints     = preProp->typeConstraints();
  prop.repoAuthType        = preProp->repoAuthType();

  // Check if this property's initial value needs to be type checked at
  // runtime.
  checkPrePropVal(prop, preProp);
}

void Class::initProp(Prop& prop, const PreClass::Prop* preProp) {
  initProp<Prop>(prop, preProp);
  prop.baseCls = this;
}

void Class::initProp(SProp& prop, const PreClass::Prop* preProp) {
  initProp<SProp>(prop, preProp);
  prop.val = preProp->val();
}

void Class::importTraitProps(int traitIdx,
                             PropMap::Builder& curPropMap,
                             SPropMap::Builder& curSPropMap,
                             std::vector<uint16_t>& slotIndex,
                             Slot& serializationIdx,
                             std::vector<bool>& serializationVisited,
                             Slot& staticSerializationIdx,
                             std::vector<bool>& staticSerializationVisited) {
  if (attrs() & AttrNoExpandTrait) {
    for (Slot p = traitIdx; p < m_preClass->numProperties(); p++) {
      auto const* preProp = &m_preClass->properties()[p];
      assertx(preProp->attrs() & AttrTrait);
      if (!(preProp->attrs() & AttrStatic)) {
        Prop prop;
        initProp(prop, preProp);
        importTraitInstanceProp(prop, preProp->val(),
                                curPropMap, curSPropMap, slotIndex,
                                serializationIdx, serializationVisited);
      } else {
        SProp prop;
        initProp(prop, preProp);
        importTraitStaticProp(
          prop, curPropMap, curSPropMap,
          staticSerializationIdx, staticSerializationVisited);
      }
    }
    return;
  }

  for (auto const& t : m_extra->m_usedTraits) {
    auto trait = t.get();

    m_allFlags.m_needsPropInitialCheck |= trait->m_allFlags.m_needsPropInitialCheck;

    // instance properties
    for (Slot p = 0; p < trait->m_declProperties.size(); p++) {
      auto& traitProp    = trait->m_declProperties[p];
      auto  traitPropIndex = trait->propSlotToIndex(p);
      auto traitPropVal = trait->m_declPropInit[traitPropIndex].val.tv();
      importTraitInstanceProp(traitProp, traitPropVal,
                              curPropMap, curSPropMap, slotIndex,
                              serializationIdx, serializationVisited);
    }

    // static properties
    for (Slot p = 0; p < trait->m_staticProperties.size(); ++p) {
      auto& traitProp = trait->m_staticProperties[p];
      importTraitStaticProp(
        traitProp, curPropMap, curSPropMap,
        staticSerializationIdx, staticSerializationVisited);
    }
  }
}

void Class::addTraitPropInitializers(std::vector<const Func*>& thisInitVec,
                                     Attr which) {
  if (attrs() & AttrNoExpandTrait) return;
  auto getInitVec = [&](Class* trait) -> auto& {
    switch (which) {
    case AttrNone: return trait->m_pinitVec;
    case AttrStatic: return trait->m_sinitVec;
    case AttrLSB: return trait->m_linitVec;
    default: always_assert(false);
    }
  };
  for (auto const& t : m_extra->m_usedTraits) {
    Class* trait = t.get();
    auto& traitInitVec = getInitVec(trait);
    // Insert trait's 86[psl]init into the current class, avoiding repetitions.
    for (unsigned m = 0; m < traitInitVec.size(); m++) {
      // Clone 86[psl]init methods, and set the class to the current class.
      // This allows 86[psl]init to determine the property offset for the
      // initializer array corectly.
      Func *f = traitInitVec[m]->clone(this);
      f->setBaseCls(this);
      f->setHasPrivateAncestor(false);
      thisInitVec.push_back(f);
    }
  }
}

void Class::setReifiedData() {
  auto const ua = m_preClass->userAttributes();
  auto const it = ua.find(s___Reified.get());
  if (it != ua.end()) m_allFlags.m_hasReifiedGenerics = true;
  if (m_parent.get()) {
    m_allFlags.m_hasReifiedParent =
      m_parent->m_allFlags.m_hasReifiedGenerics || m_parent->m_allFlags.m_hasReifiedParent;
  }
  if (m_allFlags.m_hasReifiedGenerics) {
    auto const tv = it->second;
    assertx(tvIsVec(tv));
    allocExtraData();
    m_extra.raw()->m_genericsInfo =
      extractSizeAndPosFromReifiedAttribute(
        tv.m_data.parr,
        m_preClass->typeParamNames()
      );
  } else if (!m_preClass->typeParamNames().empty()) {
    allocExtraData();
    std::vector<TypeParamInfo> typeParamInfos;
    for (auto const& name : m_preClass->typeParamNames()) {
      typeParamInfos.emplace_back(false, false, false, name);
    }
    m_extra.raw()->m_genericsInfo
      = GenericsInfo(std::move(typeParamInfos));
  }
}

namespace {
  const StaticString s_Error("Error");
  const StaticString s_Exception("Exception");
}

void Class::setInitializers() {
  std::vector<const Func*> pinits;
  std::vector<const Func*> sinits;
  std::vector<const Func*> linits;

  if (m_parent.get() != nullptr) {
    // Copy parent's 86pinit() vector, so that the 86pinit() methods can be
    // called in reverse order without any search/recursion during
    // initialization.
    pinits.assign(m_parent->m_pinitVec.begin(), m_parent->m_pinitVec.end());

    // Copy parent's 86linit into the current class, and set class to the
    // current class.
    for (const auto& linit : m_parent->m_linitVec) {
      Func *f = linit->clone(this);
      f->setBaseCls(this);
      f->setHasPrivateAncestor(false);
      linits.push_back(f);
    }
  }

  // Clone 86pinit methods from traits
  addTraitPropInitializers(pinits, AttrNone);

  // If this class has an 86pinit method, append it last so that
  // reverse iteration of the vector runs this class's 86pinit
  // first, in case multiple classes in the hierarchy initialize
  // the same property.
  const Func* meth86pinit = findSpecialMethod(this, s_86pinit.get());
  if (meth86pinit != nullptr) {
    pinits.push_back(meth86pinit);
  }

  // If this class has an 86sinit method, it must be the last element
  // in the vector. See get86sinit().
  addTraitPropInitializers(sinits, AttrStatic);
  const Func* sinit = findSpecialMethod(this, s_86sinit.get());
  if (sinit) {
    sinits.push_back(sinit);
  }

  addTraitPropInitializers(linits, AttrLSB);
  const Func* meth86linit = findSpecialMethod(this, s_86linit.get());
  if (meth86linit != nullptr) {
    linits.push_back(meth86linit);
  }

  m_pinitVec = pinits;
  m_sinitVec = sinits;
  m_linitVec = linits;

  m_allFlags.m_needInitialization =
    (m_pinitVec.size() > 0 ||
     m_staticProperties.size() > 0 ||
     m_allFlags.m_maybeRedefsPropTy ||
     m_allFlags.m_needsPropInitialCheck);

  if (m_allFlags.m_maybeRedefsPropTy || m_allFlags.m_needsPropInitialCheck) allocExtraData();

  // Implementations of Throwable get special treatment.
  if (m_parent.get() != nullptr) {
    m_allFlags.m_needsInitThrowable = m_parent->needsInitThrowable();
  } else {
    m_allFlags.m_needsInitThrowable = name()->same(s_Exception.get()) ||
                                      name()->same(s_Error.get());
  }
}

const StaticString s_HH_Iterator("HH\\Iterator");
const StaticString s_IteratorAggregate("IteratorAggregate");
void Class::checkInterfaceConstraints() {
  if (UNLIKELY(m_interfaces.contains(s_HH_Iterator.get()) &&
      m_interfaces.contains(s_IteratorAggregate.get()))) {
    raise_error("Class %s cannot implement both IteratorAggregate and Iterator"
                " at the same time", name()->data());
  }
}

// Checks if interface methods are OK:
//  - there's no requirement if this is a trait, interface, or abstract class
//  - a non-abstract class must implement all methods from interfaces it
//    declares to implement (either directly or indirectly), arity must be
//    compatible (at least as many parameters, additional parameters must have
//    defaults), and typehints must be compatible
void Class::checkInterfaceMethods() {
  for (int i = 0, size = m_interfaces.size(); i < size; i++) {
    const Class* iface = m_interfaces[i];

    for (size_t m = 0; m < iface->m_methods.size(); m++) {
      Func* imeth = iface->getMethod(m);
      const StringData* methName = imeth->name();

      // Skip special methods
      if (Func::isSpecial(methName)) continue;

      Func* meth = lookupMethod(methName);

      if (attrs() & (AttrTrait | AttrInterface | AttrAbstract)) {
        if (meth == nullptr) {
          // Skip unimplemented method.
          continue;
        }
      } else {
        // Verify that method is not abstract within concrete class.
        if (meth == nullptr || (meth->attrs() & AttrAbstract)) {
          raise_error("Class %s contains abstract method (%s) and "
                      "must therefore be declared abstract or implement "
                      "the remaining methods", name()->data(),
                      methName->data());
        }
      }
      bool ifaceStaticMethod = imeth->attrs() & AttrStatic;
      bool classStaticMethod = meth->attrs() & AttrStatic;
      if (classStaticMethod != ifaceStaticMethod) {
        raise_error("Cannot make %sstatic method %s::%s() %sstatic "
                    "in class %s",
                    ifaceStaticMethod ? "" : "non-",
                    iface->m_preClass->name()->data(), methName->data(),
                    classStaticMethod ? "" : "non-",
                    m_preClass->name()->data());
      }
      if ((imeth->attrs() & AttrPublic) &&
          !(meth->attrs() & AttrPublic)) {
        raise_error("Access level to %s::%s() must be public "
                    "(as in interface %s)", m_preClass->name()->data(),
                    methName->data(), iface->m_preClass->name()->data());
      }
      checkDeclarationCompat(m_preClass.get(), meth, imeth);
    }
  }
}

/*
 * Look up the interfaces implemented by traits used by the class, and add them
 * to the provided builder.
 */
void Class::addInterfacesFromUsedTraits(InterfaceMap::Builder& builder) const {

  for (auto const& traitName : m_preClass->usedTraits()) {
    auto const trait = Class::lookup(traitName);
    assertx(trait->attrs() & AttrTrait);
    int numIfcs = trait->m_interfaces.size();

    for (int i = 0; i < numIfcs; i++) {
      auto interface = trait->m_interfaces[i];
      if (!builder.contains(interface->name())) {
        builder.add(interface->name(), interface);
      }
    }
  }
}

const StaticString s_Stringish("Stringish");
const StaticString s_StringishObject("StringishObject");
const StaticString s_XHPChild("XHPChild");

void Class::setInterfaces() {
  InterfaceMap::Builder interfacesBuilder;
  if (m_parent.get() != nullptr) {
    int size = m_parent->m_interfaces.size();
    for (int i = 0; i < size; i++) {
      auto interface = m_parent->m_interfaces[i];
      interfacesBuilder.add(interface->name(), interface);
    }
  }

  std::vector<ClassPtr> declInterfaces;

  for (auto it = m_preClass->interfaces().begin();
       it != m_preClass->interfaces().end(); ++it) {
    auto cp = Class::load(*it);
    if (cp == nullptr) {
      raise_error("Undefined interface: %s", (*it)->data());
    }
    if (!(cp->attrs() & AttrInterface)) {
      raise_error("%s cannot implement %s - it is not an interface",
                  m_preClass->name()->data(), cp->name()->data());
    }
    m_preClass->enforceInMaybeSealedParentWhitelist(cp->preClass());
    declInterfaces.push_back(ClassPtr(cp));
    if (!interfacesBuilder.contains(cp->name())) {
      interfacesBuilder.add(cp->name(), PackedPtr<Class>(cp));
    }
    int size = cp->m_interfaces.size();
    for (int i = 0; i < size; i++) {
      auto interface = cp->m_interfaces[i];
      if (!interfacesBuilder.contains(interface->name())) {
        interfacesBuilder.add(interface->name(), interface);
      }
    }
  }

  if (auto sz = declInterfaces.size()) {
    m_declInterfaces.reserve(sz);
    for (auto interface : declInterfaces) {
      m_declInterfaces.push_back(interface);
    }
  }

  addInterfacesFromUsedTraits(interfacesBuilder);

  if (m_toString) {
    if ((!interfacesBuilder.contains(s_StringishObject.get()) ||
         !interfacesBuilder.contains(s_Stringish.get())) &&
        (!(attrs() & AttrInterface) ||
         !m_preClass->name()->tsame(s_StringishObject.get()))) {

      // Add Stringish & XHP child (All StringishObjects are also XHPChild)
      const auto maybe_add = [&](StaticString name) {
        if (interfacesBuilder.contains(name.get())) return;
        Class* cls = Class::lookup(name.get());
        assertx(cls != nullptr);
        assertx(cls->attrs() & AttrInterface);
        interfacesBuilder.add(cls->name(), PackedPtr<Class>(cls));
      };
      maybe_add(s_StringishObject);
      maybe_add(s_Stringish);
      if (!m_preClass->name()->tsame(s_XHPChild.get())) {
        maybe_add(s_XHPChild);
      }
    }
  }

  m_interfaces.create(interfacesBuilder);
  checkInterfaceConstraints();
  checkInterfaceMethods();
}

void Class::setInterfaceVtables() {
  // We only need to set interface vtables for classes with callable methods
  // that implement more than 0 interfaces.
  if (!Cfg::Repo::Authoritative ||
      !isNormalClass(this) || m_interfaces.empty()) return;

  size_t totalMethods = 0;
  Slot maxSlot = 0;
  for (auto iface : m_interfaces.range()) {
    auto const slot = iface->preClass()->ifaceVtableSlot();
    if (slot == kInvalidSlot) continue;

    maxSlot = std::max(maxSlot, slot);
    totalMethods += iface->numMethods();
  }

  const size_t nVtables = maxSlot + 1;
  auto const vtableVecSz = nVtables * sizeof(VtableVecSlot);
  auto const memSz = vtableVecSz + totalMethods * sizeof(PackedPtr<Func>);
  auto const mem = static_cast<char*>(vm_malloc(memSz));
  auto cursor = mem;

  ITRACE(3, "Setting interface vtables for class {}. "
         "{} interfaces, {} vtable slots, {} total methods\n",
         name()->data(), m_interfaces.size(), nVtables, totalMethods);
  Trace::Indent indent;

  auto const vtableVec = reinterpret_cast<VtableVecSlot*>(cursor);
  cursor += vtableVecSz;

  if (std::numeric_limits<decltype(m_vtableVecLen)>::max() < nVtables) {
    raise_error("Class %s has %zu vtable entries (maximum is %d)",
                name()->data(), nVtables,
                std::numeric_limits<decltype(m_vtableVecLen)>::max());
  }
  m_vtableVecLen = always_safe_cast<decltype(m_vtableVecLen)>(nVtables);
  m_vtableVec = vtableVec;
  memset(vtableVec, 0, vtableVecSz);

  for (auto iface : m_interfaces.range()) {
    auto const slot = iface->preClass()->ifaceVtableSlot();
    if (slot == kInvalidSlot) continue;
    ITRACE(3, "{} @ slot {}\n", iface->name()->data(), slot);
    Trace::Indent indent2;
    always_assert(slot < nVtables);

    auto const nMethods = iface->numMethods();
    auto const vtable = reinterpret_cast<PackedPtr<Func>*>(cursor);
    cursor += nMethods * sizeof(PackedPtr<Func>);
    if (vtableVec[slot].vtable != nullptr) {
      raise_error("Static analysis failure: "
                  "%s was expected to fatal at runtime, but didn't "
                  "(interfaces %s and %s share slot %d)",
                  m_preClass->name()->data(),
                  iface->preClass()->name()->data(),
                  vtableVec[slot].iface->preClass()->name()->data(),
                  slot);
    }
    vtableVec[slot].vtable = vtable;
    vtableVec[slot].iface = iface;

    for (size_t i = 0; i < nMethods; ++i) {
      auto ifunc = iface->getMethod(i);
      auto func = lookupMethod(ifunc->name());
      ITRACE(3, "{}:{} @ slot {}\n", ifunc->name()->data(), func, i);

      if (!func && isAbstract(this) && ifunc->isStatic() &&
          !Func::isSpecial(ifunc->name())) {
        // When iface vtable is present and used for a static method call, it is
        // expected that the method to be called exists. This may not be true
        // for abstract classes, in that case do not define the iface vtable.
        //
        // Instance methods do not have this issue, as iface vtable of object
        // instance's class is used to invoke them, which is never abstract.
        low_free(vtableVec);
        m_vtableVecLen = 0;
        m_vtableVec = nullptr;
        return;
      }

      always_assert(func || isAbstract(this) || Func::isSpecial(ifunc->name()));
      vtable[i] = func;
    }
  }

  always_assert(cursor == mem + memSz);
}

void Class::setRequirements() {
  RequirementMap::Builder reqBuilder;

  auto addReq = [&](const PreClass::ClassRequirement* req) {
    auto it = reqBuilder.find(req->name());
    if (it == reqBuilder.end()) {
      reqBuilder.add(req->name(), req);
      return;
    }
    auto const existing = reqBuilder[it->second];
    if (existing->is_same(req)) return;
    // Same target name but different requirement kind:
    // - `require this as` is compatible with both `require extends`
    //    and `require class`, the stricter should be propagated;
    // - `require class` is incompatible with `require extends`
    auto const thisAs = PreClass::RequirementThisAs;
    if (existing->kind() == thisAs || req->kind() == thisAs) {
      // Propagate the stricter (non-`require this as`) requirement.
      if (existing->kind() == thisAs) {
        reqBuilder[it->second] = req;
      }
      return;
    }
    raise_error("Conflicting requirements for '%s'",
                req->name()->data());
  };

  if (m_parent.get() != nullptr) {
    for (auto const& req : m_parent->allRequirements().range()) {
      // `require class` constrains the class applying the trait, not subclasses
      if (req->kind() == PreClass::RequirementClass) continue;
      // no need for addReq; parent won't have duplicates
      reqBuilder.add(req->name(), req);
    }
  }
  for (auto const& iface : m_interfaces.range()) {
    for (auto const& req : iface->allRequirements().range()) {
      addReq(req);
    }
  }
  for (auto const& ut : m_extra->m_usedTraits) {
    for (auto const& req : ut->allRequirements().range()) {
      addReq(req);
    }
  }

  if (attrs() & AttrInterface) {
    // Check that requirements are semantically valid
    // We don't require the class to exist yet, to avoid creating
    // pointless circular dependencies, but if it does, we check
    // that it's the right kind.
    for (auto const& req : m_preClass->requirements()) {
      assertx(req.kind() == PreClass::RequirementExtends);
      auto const reqName = req.name();
      auto const reqCls = Class::lookup(reqName);
      if (reqCls) {
        if (reqCls->attrs() & (AttrTrait | AttrInterface | AttrFinal)) {
          raise_error("Interface '%s' requires extension of '%s', but %s "
                      "is not an extendable class",
                      m_preClass->name()->data(),
                      reqName->data(),
                      reqName->data());
        }
      }
      addReq(&req);
    }
  } else if (attrs() & (AttrTrait | AttrNoExpandTrait)) {
    // Check that requirements are semantically valid.
    // Note that trait flattening could have migrated
    // requirements onto a class's preClass.
    for (auto const& req : m_preClass->requirements()) {
      auto const reqName = req.name();
      if (auto const reqCls = Class::lookup(reqName)) {
        switch (req.kind()) {
          case PreClass::RequirementExtends:
          case PreClass::RequirementThisAs: {
            if (reqCls->attrs() & (AttrTrait | AttrInterface | AttrFinal)) {
              raise_error(Strings::TRAIT_BAD_REQ_EXTENDS,
                          m_preClass->name()->data(),
                          reqName->data(),
                          reqName->data());
            }
            break;
          }
          case PreClass::RequirementImplements: {
            if (!(reqCls->attrs() & AttrInterface)) {
              raise_error(Strings::TRAIT_BAD_REQ_IMPLEMENTS,
                          m_preClass->name()->data(),
                          reqName->data(),
                          reqName->data());
            }
            break;
          }
          case PreClass::RequirementClass:
            if (reqCls->attrs() & (AttrInterface | AttrTrait)) {
              raise_error("Trait '%s' may only be used from '%s', but '%s' is not a class",
                          m_preClass->name()->data(),
                          reqName->data(),
                          reqName->data());
            }
            break;
          }
      }
      addReq(&req);
    }
  }

  m_requirements.create(reqBuilder);
  checkRequirementConstraints();
}

void Class::setEnumType() {
  if (attrs() & AttrEnum) {
    allocExtraData();
    auto const resolved = m_preClass->enumBaseTy().resolvedWithAutoload();
    m_extra.raw()->m_enumBaseTy = resolved;

    // Make sure we've loaded a valid underlying type.
    if (!resolved.validForEnumBase()) {
      raise_error("Invalid base type for enum %s", m_preClass->name()->data());
    }
  }
}

void Class::setIncludedEnums() {
  IncludedEnumMap::Builder includedEnumsBuilder;

  if (!isAnyEnum(this) || m_preClass->includedEnums().empty()) {
    return;
  }

  allocExtraData();

  std::vector<ClassPtr> declIncludedEnums;

  for (auto it = m_preClass->includedEnums().begin();
       it != m_preClass->includedEnums().end(); ++it) {
    auto cp = Class::load(*it);
    if (cp == nullptr) {
      raise_error("Undefined enum: %s", (*it)->data());
    }
    if (isEnum(this) != isEnum(cp) || isEnumClass(this) != isEnumClass(cp)) {
      raise_error("%s cannot include %s - it is not an enum%s",
                  m_preClass->name()->data(), cp->name()->data(),
                  isEnumClass(this) ? " class" : "");
    }
    declIncludedEnums.emplace_back(cp);
    m_preClass->enforceInMaybeSealedParentWhitelist(cp->preClass());

    // N.B. This will check for int and string type compatibility, but not for
    //      classname<T> which is also an allowed enum type. The type checker
    //      currently does this and it could be added to HHVM as well.
    auto my_enumBaseTy = enumBaseTy().underlyingDataType();
    auto cp_baseType = cp->enumBaseTy().underlyingDataType();

    if (my_enumBaseTy &&
        (!cp_baseType ||
         isIntType(*cp_baseType) != isIntType(*my_enumBaseTy) ||
         isStringType(*cp_baseType) != isStringType(*my_enumBaseTy))) {
      raise_error("%s cannot include %s - base type mismatch",
                  m_preClass->name()->data(), cp->name()->data());
    }
    if (!includedEnumsBuilder.contains(cp->name())) {
      includedEnumsBuilder.add(cp->name(), PackedPtr<Class>(cp));
    }
    int size = cp->m_extra->m_includedEnums.size();
    for (int i = 0; i < size; i++) {
      auto includedEnum = cp->m_extra->m_includedEnums[i];
      if (!includedEnumsBuilder.contains(includedEnum->name())) {
        includedEnumsBuilder.add(includedEnum->name(), includedEnum);
      }
    }
  }

  m_extra->m_declIncludedEnums.insert(
    m_extra->m_declIncludedEnums.begin(),
    declIncludedEnums.begin(),
    declIncludedEnums.end()
  );
  m_extra->m_includedEnums.create(includedEnumsBuilder);
}

void Class::setLSBMemoCacheInfo() {
  boost::container::flat_map<FuncId, Slot> slots;
  Slot numSlots = 0;

  /* Inherit slots from parent */
  if (m_parent && m_parent->m_extra) {
    numSlots = m_parent->m_extra->m_lsbMemoExtra.m_numSlots;
  }

  /* If any of our methods need slots, insert them at the end */
  auto const methodCount = numMethods();
  uint64_t assignCount = 0;
  for (Slot i = 0; i < methodCount; ++i) {
    auto const f = getMethod(i);
    if (f->isMemoizeWrapperLSB() && f->cls() == this) {
      slots.emplace(f->getFuncId(), numSlots++);
      ++assignCount;
    }
  }
  always_assert(assignCount == slots.size());

  if (numSlots != 0) {
    allocExtraData();
    auto& mx = m_extra->m_lsbMemoExtra;
    mx.m_slots = std::move(slots);
    mx.m_numSlots = numSlots;
    initLSBMemoHandles();
  }
}

void Class::initLSBMemoHandles() {
  /* Allocate handles array */
  auto& mx = m_extra->m_lsbMemoExtra;
  auto const numSlots = mx.m_numSlots;
  assertx(numSlots > 0);
  rds::Handle* handles = static_cast<rds::Handle*>(
    vm_malloc(numSlots * sizeof(rds::Handle)));

  /* Assign handle to every slot */
  uint64_t assignCount = 0;
  uint64_t assignSum = 0;
  const Class* cls = this;
  while (cls != nullptr && cls->m_extra) {
    for (const auto& kv : cls->m_extra->m_lsbMemoExtra.m_slots) {
      auto const func = Func::fromFuncId(kv.first);
      auto const slot = kv.second;
      assertx(slot >= 0 && slot < numSlots);
      if (func->numKeysForMemoize() == 0) {
        handles[slot] = rds::bindLSBMemoValue(this, func).handle();
        mx.m_symbols.emplace_back(
          std::make_pair(rds::LSBMemoValue { this, kv.first }, handles[slot])
        );
      } else {
        handles[slot] = rds::bindLSBMemoCache(this, func).handle();
        mx.m_symbols.emplace_back(
          std::make_pair(rds::LSBMemoCache { this, kv.first }, handles[slot])
        );
      }
      ++assignCount;
      assignSum += slot;
    }
    cls = cls->m_parent.get();
  }
  /* Sanity check: Make sure we assigned every slot exactly once */
  always_assert(numSlots == assignCount);
  always_assert(assignSum == ((numSlots - 1) * numSlots) / 2);

  assertx(mx.m_handles == nullptr);
  mx.m_handles = handles;
}

void Class::setInstanceMemoCacheInfo() {
  // Inherit the data from the parent, if any.
  if (m_parent && m_parent->hasMemoSlots()) {
    allocExtraData();
    m_extra.raw()->m_nextMemoSlot = m_parent->m_extra->m_nextMemoSlot;
    m_extra.raw()->m_sharedMemoSlots = m_parent->m_extra->m_sharedMemoSlots;
  }

  // If we have memo slots defined already, and this class is adding (or
  // changing) native data, forbid it. The parent class won't realize the native
  // data has changed size, so code we generate in it to access the slot will be
  // incorrect (it won't find the slot in the right place). If we ever have a
  // need to actually do this, we'll have to revisit this.
  if (m_preClass->nativeDataInfo() && m_extra->m_nextMemoSlot > 0) {
    raise_error(
      "Class '%s' with existing memoized methods cannot have native-data added",
      m_preClass->name()->data()
    );
  }

  auto const forEachMeth = [&](auto f) {
    auto const methodCount = numMethods();
    for (Slot i = 0; i < methodCount; ++i) {
      auto const m = getMethod(i);
      if (m->isMemoizeWrapper() && !m->isStatic() && m->cls() == this) f(m);
    }
  };

  auto const addNonShared = [&](const Func* f) {
    allocExtraData();
    auto extra = m_extra.raw();
    extra->m_memoMappings.emplace(
      f->getFuncId(),
      std::make_pair(extra->m_nextMemoSlot++, false)
    );
  };

  auto const addShared = [&](const Func* f) {
    allocExtraData();
    auto extra = m_extra.raw();

    // There's no point in allocating slots for parameter counts greater than
    // kMemoCacheMaxSpecializedKeys, since they'll all be generic caches
    // anyways. Cap the count at that.
    auto const keyCount =
      std::min<size_t>(f->numKeysForMemoize(), kMemoCacheMaxSpecializedKeys + 1);
    auto const it = extra->m_sharedMemoSlots.find(keyCount);
    if (it != extra->m_sharedMemoSlots.end()) {
      extra->m_memoMappings.emplace(
        f->getFuncId(),
        std::make_pair(it->second, true)
      );
      return;
    }

    extra->m_sharedMemoSlots.emplace(keyCount, extra->m_nextMemoSlot);
    extra->m_memoMappings.emplace(
      f->getFuncId(),
      std::make_pair(extra->m_nextMemoSlot++, true)
    );
  };

  // First count how many methods we have without parameters or with parameters.
  size_t methNoKeys = 0;
  size_t methWithKeys = 0;
  forEachMeth(
    [&](const Func* f) {
      if (f->numKeysForMemoize() == 0) {
        ++methNoKeys;
      } else {
        ++methWithKeys;
      }
    }
  );

  // We only want to assign up to Eval.NonSharedInstanceMemoCaches non-shared
  // caches. With the remaining non-assigned slots, we give preference to the
  // parameter-less methods first. This is because there's a greater benefit to
  // giving parameter-less methods non-shared slots (they can just be a
  // TypedValue). However, we only give the methods non-shared slots if we can give
  // them to all the methods. If there's more methods than we have available
  // non-shared slots, its not clear which ones should get the non-shared slots
  // and which ones shouldn't, so we treat them all equally and give them all
  // shared slots. After processing the parameter-less methods, we apply the
  // same logic to the methods with parameters.

  // How many non-shared slots do we have left to allocate?
  auto slotsLeft =
    Cfg::Eval::NonSharedInstanceMemoCaches -
    std::min(
      m_extra->m_nextMemoSlot,
      Cfg::Eval::NonSharedInstanceMemoCaches
    );

  if (methNoKeys > 0) {
    forEachMeth(
      [&](const Func* f) {
        if (f->numKeysForMemoize() == 0) {
          (methNoKeys <= slotsLeft) ? addNonShared(f) : addShared(f);
        }
      }
    );
    if (methNoKeys <= slotsLeft) slotsLeft -= methNoKeys;
  }

  if (methWithKeys > 0) {
    forEachMeth(
      [&](const Func* f) {
        if (f->numKeysForMemoize() > 0) {
          (methWithKeys <= slotsLeft) ? addNonShared(f) : addShared(f);
        }
      }
    );
  }
}

namespace {
ObjectData* throwingInstanceCtor(Class* cls) {
  SystemLib::throwExceptionObject(
    fmt::format("Class {} may not be directly instantiated", cls->name()->slice())
  );
}
} // namespace

void Class::setNativeDataInfo() {
  for (auto cls = this; cls; cls = cls->parent()) {
    if (auto ndi = cls->preClass()->nativeDataInfo()) {
      allocExtraData();
      m_extra.raw()->m_nativeDataInfo = ndi;
      if (ndi->ctor_throws) {
        m_extra.raw()->m_instanceCtor = throwingInstanceCtor;
        m_extra.raw()->m_instanceCtorUnlocked = throwingInstanceCtor;
      } else {
        m_extra.raw()->m_instanceCtor = Native::nativeDataInstanceCtor<false>;
        m_extra.raw()->m_instanceCtorUnlocked =
          Native::nativeDataInstanceCtor<true>;
      }
      m_extra.raw()->m_instanceDtor = Native::nativeDataInstanceDtor;
      m_releaseFunc = Native::nativeDataInstanceDtor;
      m_RTAttrs |= ndi->rt_attrs;
      break;
    }
  }
}

void Class::initClosure() {
  if (!isBuiltin() || !m_preClass->name()->equal(s_Closure.get())) return;

  c_Closure::cls_Closure = this;
  assertx(!hasMemoSlots());
  allocExtraData();
  auto& extraData = *m_extra.raw();
  extraData.m_instanceCtor = c_Closure::instanceCtor;
  extraData.m_instanceCtorUnlocked = c_Closure::instanceCtor;
  extraData.m_instanceDtor = c_Closure::instanceDtor;
  m_releaseFunc = c_Closure::instanceDtor;
}

bool Class::hasNativePropHandler() const {
  return m_RTAttrs & Class::HasNativePropHandler;
}

const Native::NativePropHandler* Class::getNativePropHandler() const {
  assertx(hasNativePropHandler());

  for (auto cls = this; cls; cls = cls->parent()) {
    auto propHandler = Native::getNativePropHandler(cls->name());
    if (propHandler != nullptr) {
      return propHandler;
    }
  }

  not_reached();
}

void Class::raiseUnsatisfiedRequirement(const PreClass::ClassRequirement* req)  const {
  assertx(!(attrs() & (AttrInterface | AttrTrait)));

  auto const reqName = req->name();
  switch (req->kind()) {
    case PreClass::RequirementExtends: {
      for (auto const& iface : m_interfaces.range()) {
        if (iface->allRequirements().contains(reqName)) {
          raise_error("Class '%s' required to extend class '%s'"
                      " by interface '%s'",
                      m_preClass->name()->data(),
                      reqName->data(),
                      iface->preClass()->name()->data());
        }
      }

      for (auto const& traitCls : m_extra->m_usedTraits) {
         if (traitCls->allRequirements().contains(reqName)) {
           raise_error(Strings::TRAIT_REQ_EXTENDS,
                       m_preClass->name()->data(),
                       reqName->data(),
                       traitCls->preClass()->name()->data());
         }
      }

      if (attrs() & AttrNoExpandTrait) {
        // A result of trait flattening, as with the is_implements case above
        assertx(!m_extra || m_extra->m_usedTraits.size() == 0);
        assertx(m_preClass->requirements().size() > 0);
        raise_error(Strings::TRAIT_REQ_EXTENDS,
                    m_preClass->name()->data(),
                    reqName->data(),
                    "<<flattened>>");
      }
      break;
    }
    case PreClass::RequirementImplements: {
      // "require implements" is only allowed on traits.
      assertx(attrs() & AttrNoExpandTrait ||
             (m_extra && m_extra->m_usedTraits.size() > 0));
      for (auto const& traitCls : m_extra->m_usedTraits) {
        if (traitCls->allRequirements().contains(reqName)) {
          raise_error(Strings::TRAIT_REQ_IMPLEMENTS,
                      m_preClass->name()->data(),
                      reqName->data(),
                      traitCls->preClass()->name()->data());
        }
      }

      if (attrs() & AttrNoExpandTrait) {
        // As a result of trait flattening, the PreClass of this normal class
        // contains a requirement. To save space, we don't include the source
        // trait in the requirement. For details, see
        // ClassScope::importUsedTraits in the compiler.
        assertx(!m_extra || m_extra->m_usedTraits.size() == 0);
        assertx(m_preClass->requirements().size() > 0);
        raise_error(Strings::TRAIT_REQ_IMPLEMENTS,
                    m_preClass->name()->data(),
                    reqName->data(),
                    "<<flattened>>");
      }

      always_assert(false); // requirements cannot spontaneously generate
      break;
    }
    case PreClass::RequirementClass: {
      assertx(m_preClass->name() != reqName);
      for (auto const& traitCls : m_extra->m_usedTraits) {
        if (traitCls->allRequirements().contains(reqName)) {
          raise_error("Class '%s' uses trait '%s', but trait '%s' may only be used from class '%s'",
                      m_preClass->name()->data(),
                      traitCls->preClass()->name()->data(),
                      traitCls->preClass()->name()->data(),
                      reqName->data());
        }
      }

      if (attrs() & AttrNoExpandTrait) {
        // A result of trait flattening, analogous to the RequirementImplements case above
        assertx(!m_extra || m_extra->m_usedTraits.size() == 0);
        assertx(m_preClass->requirements().size() > 0);
        raise_error("Class '%s' uses trait '<<flattened>>', but trait '<<flattened>>' may only be used from class '%s'",
                    m_preClass->name()->data(),
                    reqName->data());
      }
      break;
    }
    case PreClass::RequirementThisAs: {
      assertx(attrs() & AttrNoExpandTrait ||
      (m_extra && m_extra->m_usedTraits.size() > 0));
      for (auto const& traitCls : m_extra->m_usedTraits) {
        if (traitCls->allRequirements().contains(reqName)) {
          raise_error(Strings::TRAIT_REQ_THIS_AS,
                      m_preClass->name()->data(),
                      reqName->data(),
                      traitCls->preClass()->name()->data());
        }
      }

      if (attrs() & AttrNoExpandTrait) {
        // A result of trait flattening, analogous to the RequirementImplements case above
        assertx(!m_extra || m_extra->m_usedTraits.size() == 0);
        assertx(m_preClass->requirements().size() > 0);
        raise_error(Strings::TRAIT_REQ_THIS_AS,
                    m_preClass->name()->data(),
                    reqName->data(),
                    "<<flattened>>");
      }
      break;
    }
  }
  // calls to this method are expected to come as a result of an error due
  // to a requirement coming from traits or interfaces
  not_reached();
}

void Class::checkRequirementConstraints() const {
  if (attrs() & (AttrInterface | AttrTrait)) return;

  for (auto const& req : m_requirements.range()) {
    auto const reqName = req->name();
    switch (req->kind()) {
      case PreClass::RequirementExtends: {
        auto reqExtCls = Class::lookup(reqName);
        if (UNLIKELY(
              (reqExtCls == nullptr) ||
              (reqExtCls->attrs() & (AttrTrait | AttrInterface)))) {
          raiseUnsatisfiedRequirement(req);
        }
        if (UNLIKELY(!classofNonIFace(reqExtCls))) {
          raiseUnsatisfiedRequirement(req);
        }
        break;
      }
      case PreClass::RequirementImplements: {
        if (UNLIKELY(!ifaceofDirect(reqName))) {
          raiseUnsatisfiedRequirement(req);
        }
        break;
      }
      case PreClass::RequirementClass: {
        // remark: `require class` enforcement is disabled for mock classes
        if ((m_preClass->name() != reqName) &&
            (m_preClass->userAttributes().find(s___MockClass.get()) ==
             m_preClass->userAttributes().end())) {
          raiseUnsatisfiedRequirement(req);
        }
        break;
      }
      case PreClass::RequirementThisAs: {
        if (m_preClass->name() == reqName) break;
        auto reqExtCls = Class::lookup(reqName);
        if (UNLIKELY(reqExtCls == nullptr || !classofNonIFace(reqExtCls))) {
          raiseUnsatisfiedRequirement(req);
        }
        break;
      }
    }
  }
}

void Class::setClassVec() {
  if (m_classVecLen > 1) {
    assertx(m_parent.get() != nullptr);
    memcpy(m_classVec, m_parent->m_classVec,
           (m_classVecLen-1) * sizeof(m_classVec[0]));
  }
  m_classVec[m_classVecLen-1] = this;
}

void Class::setFuncVec(MethodMapBuilder& builder) {
  auto funcVec = this->funcVec();

  memset(funcVec, 0, m_funcVecLen * sizeof(PackedPtr<Func>));

  funcVec = (PackedPtr<Func>*)this;
  assertx(builder.size() <= m_funcVecLen);

  for (Slot i = 0; i < builder.size(); i++) {
    assertx(builder[i]->methodSlot() < builder.size());
    funcVec[-((int32_t)builder[i]->methodSlot() + 1)] = builder[i];
  }
}

void Class::setReleaseData() {
  if (getNativeDataInfo()) return;
  if (hasMemoSlots()) {
    m_memoSize = ObjectData::objOffFromMemoNode(this);
  }
  auto const nProps = numDeclProperties();
  auto const size = m_memoSize + ObjectData::sizeForNProps(nProps);
  m_sizeIdx = MemoryManager::size2Index(size);
}

void Class::getMethodNames(const Class* cls, const Class* ctx, Array& out) {

  // The order of these methods is so that the first ones win on
  // name conflicts.

  auto const numMethods = cls->numMethods();

  for (Slot i = 0; i < numMethods; ++i) {
    auto const meth = cls->getMethod(i);
    auto const declCls = meth->cls();
    auto addMeth = [&]() {
      if (!out.exists(meth->nameStr())) {
        auto const methName = Variant(meth->name(), Variant::PersistentStrInit{});
        out.set(meth->nameStr(), methName);
      }
    };

    // Only pick methods declared in this class, in order to match
    // Zend's order.  Inherited methods will be inserted in the
    // recursive call later.
    if (declCls != cls) continue;

    // Skip generated, internal methods.
    if (meth->isGenerated()) continue;

    // Public methods are always visible.
    if ((meth->attrs() & AttrPublic)) {
      addMeth();
      continue;
    }

    // In anonymous contexts, only public methods are visible.
    if (!ctx) continue;

    // All methods are visible if the context is the class that
    // declared them.  If the context is not the declCls, protected
    // methods are visible in context classes related the declCls.
    if (declCls == ctx ||
        ((meth->attrs() & AttrProtected) &&
         (ctx->classof(declCls) || declCls->classof(ctx)))) {
      addMeth();
    }
  }

  // Now add the inherited methods.
  if (auto const parent = cls->parent()) {
    getMethodNames(parent, ctx, out);
  }

  // Add interface methods that the class may not have implemented yet.
  for (auto& iface : cls->m_declInterfaces) {
    getMethodNames(iface.get(), ctx, out);
  }
}

const StringData* Class::moduleName() const {
  return preClass()->unit()->moduleName();
}

///////////////////////////////////////////////////////////////////////////////
// Trait method import.

namespace {

struct TraitMethod {
  TraitMethod(const Class* trait_, const Func* method_, Attr modifiers_)
      : trait(trait_)
      , method(method_)
      , modifiers(modifiers_)
    {}

  using class_type = const Class*;
  using method_type = const Func*;
  using origin_type = const PreClass*;

  const Class* trait;
  const Func* method;
  Attr modifiers;
};

struct TMIOps {
  using origin_type = TraitMethod::origin_type;

  // Return the name for the trait class.
  static const StringData* clsName(const Class* traitCls) {
    return traitCls->name();
  }

  static origin_type originalClass(const Func* method) {
    return method->preClass();
  }

  // Is-a methods.

  static bool isAbstract(Attr modifiers) {
    return modifiers & AttrAbstract;
  }

  // Whether to exclude methods with name `methName' when adding.
  static bool exclude(const StringData* methName) {
    return Func::isSpecial(methName);
  }

  // Errors.
  static void errorDuplicateMethod(const Class* cls,
                                   const StringData* methName,
                                   const std::vector<const StringData*>& methodDefinitions) {
    // No error if the class will override the method.
    if (cls->preClass()->hasMethod(methName)) return;

    std::vector<folly::StringPiece> traitNames;
    for (auto& clsName : methodDefinitions) {
      traitNames.push_back(clsName->slice());
    }
    std::string traits;
    folly::join(", ", traitNames, traits);

    raise_error(Strings::METHOD_IN_MULTIPLE_TRAITS, methName->data(), traits.c_str());
  }
};

using TMIData = TraitMethodImportData<TraitMethod, TMIOps>;

void importTraitMethod(Class* cls,
                       const TMIData::MethodData& mdata,
                       Class::MethodMapBuilder& builder) {
  const Func* method = mdata.tm.method;
  Attr modifiers = mdata.tm.modifiers;

  auto mm_iter = builder.find(mdata.name);

  ITRACE(5, "  - processing trait method {}\n", method->name()->data());

  // For abstract methods, simply return if method already declared.
  if ((modifiers & AttrAbstract) && mm_iter != builder.end()) {
    return;
  }

  if (modifiers == AttrNone) {
    modifiers = method->attrs();
  } else {
    // Trait alias statements are only allowed to change the attributes that
    // are part 'attrMask' below; all other method attributes are preserved
    Attr attrMask = (Attr)(AttrPublic | AttrProtected | AttrPrivate |
                           AttrAbstract | AttrFinal);
    modifiers = (Attr)((modifiers       &  (attrMask)) |
                       (method->attrs() & ~(attrMask)));
  }

  // Don't propagate AttrHasInheritedReturnTypes from the trait method as this
  // needs to be recomputed in the context of the current class and set via
  // setInheritedReturnTypes.
  modifiers = Attr(modifiers & ~AttrHasInheritedReturnTypes);

  Func* parentMethod = nullptr;
  if (mm_iter != builder.end()) {
    Func* existingMethod = builder[mm_iter->second];
    if (existingMethod->cls() == cls) {
      // Don't override an existing method if this class provided an
      // implementation
      return;
    }
    parentMethod = existingMethod;
  }
  Func* f = method->clone(cls, mdata.name);
  f->setAttrs(modifiers | AttrTrait);
  if (!parentMethod) {
    // New method
    builder.add(mdata.name, f);
    f->setBaseCls(cls);
    f->setHasPrivateAncestor(false);
  } else {
    // Override an existing method
    Class* baseClass;

    cls->methodOverrideCheck(parentMethod, f);

    assertx(!(f->attrs() & AttrPrivate) ||
           (parentMethod->attrs() & AttrPrivate));
    if ((parentMethod->attrs() & AttrPrivate) || (f->attrs() & AttrPrivate)) {
      baseClass = cls;
    } else {
      baseClass = parentMethod->baseCls();
    }
    f->setBaseCls(baseClass);
    f->setHasPrivateAncestor(
      parentMethod->hasPrivateAncestor() ||
      (parentMethod->attrs() & AttrPrivate));
    builder[mm_iter->second] = f;
  }
}

}

void Class::importTraitMethods(MethodMapBuilder& builder) {
  TMIData tmid;

  // Find all methods to be imported.
  for (auto const& t : m_extra->m_usedTraits) {
    Class* trait = t.get();
    for (Slot i = 0; i < trait->m_methods.size(); ++i) {
      auto const method = trait->getMethod(i);

      TraitMethod traitMethod { trait, method, method->attrs() };
      tmid.add(traitMethod, method->name());
    }
  }

  bool enableMethodTraitDiamond = m_preClass->enableMethodTraitDiamond();
  auto traitMethods = tmid.finish(this, enableMethodTraitDiamond);

  // Import the methods.
  for (auto const& mdata : traitMethods) {
    importTraitMethod(this, mdata, builder);
  }
}

///////////////////////////////////////////////////////////////////////////////
// Lookup.

namespace {

void setupClass(Class* newClass, NamedType* nameList) {
  bool const isPersistent = shouldUsePersistentHandles(newClass);
  nameList->m_cachedClass.bind(
    isPersistent ? rds::Mode::Persistent : rds::Mode::Normal,
    rds::LinkName{"NEClass", newClass->name()}
  );

  if (debug) {
    if (newClass->isBuiltin()) {
      assertx(newClass->isPersistent());
      for (auto i = newClass->numMethods(); i--;) {
        auto const func = newClass->getMethod(i);
        if (func->isCPPBuiltin() && func->isStatic()) {
          assertx(func->isPersistent());
        }
      }
    }
  }

  newClass->setClassHandle(nameList->m_cachedClass);
  newClass->incAtomicCount();
  newClass->setNewClassId();

  InstanceBits::ifInitElse(
    [&] { newClass->setInstanceBits();
          nameList->pushClass(newClass); },
    [&] { nameList->pushClass(newClass); }
  );

  if (Cfg::Eval::EnableReverseDataMap) {
    // The corresponding deregister is in NamedType::removeClass().
    data_map::register_start(newClass);
    for (unsigned i = 0, n = newClass->numMethods(); i < n; i++) {
      if (auto meth = newClass->getMethod(i)) {
        if (meth->cls() == newClass) {
          meth->registerInDataMap();
        }
      }
    }
  }
}

}

// Construct a class. If another thread is already defining the class,
// waits for it to finish and reuse the result. Conservative: if two threads
// try to construct classes belonging to same shard, if second thread is newer,
// it waits. Otherwise, it races first thread to prevent deadlock.
Class* newClassImpl(const PreClass* preClass,
                    Class* parent,
                    const Class* top,
                    NamedType* nameList) {
    auto& bucket = getBucket(preClass->name());
    const int64_t ridx = Cfg::Eval::SynchronizeClassLoading
      ? Treadmill::requestIdx() : 0;

    auto setClassInRDS = [&](Class* newClass) -> Class* {
      assertx(newClass);

      // If another thread beat us to creating a class for this name,
      // use it instead.
      //
      // And if someone beat us free the version we created. There is still a
      // memory leak race condition here. When a class is created and is first
      // it just reference the func in preClass. If we create the class a second time
      // all funcs needs to be cloned. So the leak is if you didn't create class
      // first but won the race. Then all your funcs would be clones.

      if (top != nameList->clsList()) {
        newClass->atomicRelease();
        return nullptr;
      }

      Lock l(bucket.mutex);
      if (top != nameList->clsList()) {
        newClass->atomicRelease();
        return nullptr;
      }
      setupClass(newClass, nameList);

      /*
      * call setCached after adding to the class list, otherwise the
      * target-cache short circuit at the top could return a class
      * which is not yet on the clsList().
      */
      newClass->setCached();
      return newClass;
    };

    auto createClass = [&]() -> Class* {
      FrameRestore fr(preClass);
      auto c = Class::newClass(const_cast<PreClass*>(preClass), parent);
      auto res = setClassInRDS(c);
      auto expectedRIdx = ridx;
      if (bucket.inProgress.compare_exchange_strong(expectedRIdx, kInvalidIdx) ||
        res != nullptr) {
        bucket.ec.notifyAll();
      }
      return res;
    };

    while (true) {
      auto const didAcquire = [&] {
        auto expected = bucket.inProgress.load();
        while (expected > ridx) {
          if (bucket.inProgress.compare_exchange_strong(expected, ridx)) {
            return true;
          }
        }
        return expected == ridx;
      }();
      if (didAcquire) return createClass();

      bucket.ec.await([&] {
        return top != nameList->clsList() ||
        bucket.inProgress.load() > ridx;
      });
      if (top != nameList->clsList()) return nullptr;
    }
}

const StaticString s__JitSerdesPriority("__JitSerdesPriority");

Class* Class::def(const PreClass* preClass, bool failIsFatal /* = true */) {
  FTRACE(3, "  Defining cls {} failIsFatal {}\n",
         preClass->name()->data(), failIsFatal);
  NamedType* const nameList = preClass->namedType();
  Class* top = nameList->clsList();

  /*
   * Check if there is already a name defined in this request for this
   * NamedType.
   *
   * Raise a fatal unless the existing class definition is identical to the
   * one this invocation would create.
   */
  auto existingKind = nameList->checkSameName<PreClass>();
  if (existingKind) {
    FrameRestore fr(preClass);
    raise_error("Cannot declare class with the same name (%s) as an "
                "existing %s", preClass->name()->data(), existingKind);
    return nullptr;
  }

  // If there was already a class declared with DefClass, check if it's
  // compatible.
  if (Class* cls = nameList->getCachedClass()) {
    if (cls->preClass() != preClass) {
      if (failIsFatal) {
        FrameRestore fr(preClass);
        raise_error("Class already declared: %s", preClass->name()->data());
      }
      return nullptr;
    }
    assertx(!Cfg::Repo::Authoritative ||
            (cls->isPersistent() && classHasPersistentRDS(cls)));
    return cls;
  }

  // Get a compatible Class, and add it to the list of defined classes.
  Class* parent = nullptr;
  for (;;) {
    // Search for a compatible extant class.  Searching from most to least
    // recently created may have better locality than alternative search orders.
    // In addition, its the only simple way to make this work lock free...
    for (Class* class_ = top; class_ != nullptr; ) {
      Class* cur = class_;
      class_ = class_->m_next;
      if (cur->preClass() != preClass) continue;
      Class::Avail avail = cur->avail(parent, failIsFatal /*tryAutoload*/);
      if (LIKELY(avail == Class::Avail::True)) {
        cur->setCached();
        DEBUGGER_ATTACHED_ONLY(phpDebuggerDefClassHook(cur));
        assertx(!Cfg::Repo::Authoritative ||
                (cur->isPersistent() && classHasPersistentRDS(cur)));
        return cur;
      }
      if (avail == Class::Avail::Fail) {
        if (failIsFatal) {
          FrameRestore fr(preClass);
          raise_error("unknown class %s", parent->name()->data());
        }
        return nullptr;
      }
      assertx(avail == Class::Avail::False);
    }

    if (!parent && preClass->parent()->size() != 0) {
      parent = Class::get(preClass->parent(), failIsFatal);
      if (parent == nullptr) {
        if (failIsFatal) {
          FrameRestore fr(preClass);
          raise_error("unknown class %s", preClass->parent()->data());
        }
        return nullptr;
      }
    }

    if (!failIsFatal) {
      // Check interfaces
      for (auto it = preClass->interfaces().begin();
                 it != preClass->interfaces().end(); ++it) {
        if (!Class::get(*it, false)) return nullptr;
      }
      // traits
      for (auto const& traitName : preClass->usedTraits()) {
        if (!Class::get(traitName, false)) return nullptr;
      }
      // enum
      if (preClass->attrs() & AttrEnum) {
        if (!preClass->enumBaseTy().validForEnumBase()) {
          return nullptr;
        }
      }
      // enum and enum class
      if (preClass->attrs() & (AttrEnum|AttrEnumClass)) {
        for (auto it = preClass->includedEnums().begin();
                   it != preClass->includedEnums().end(); ++it) {
          if (!Class::get(*it, false)) return nullptr;
        }
      }
    }
    Class* newClass = newClassImpl(preClass, parent, top, nameList);
    if (newClass == nullptr) {
      assertx(top != nameList->clsList());
      top = nameList->clsList();
      continue;
    }

    DEBUGGER_ATTACHED_ONLY(phpDebuggerDefClassHook(newClass));
    assertx(!Cfg::Repo::Authoritative ||
            (newClass->isPersistent() &&
             classHasPersistentRDS(newClass)));

    if (UNLIKELY(Cfg::Eval::EnableIntrinsicsExtension)) {
      Lock l(s_priority_serialize_mutex);
      auto const it =
        preClass->userAttributes().find(s__JitSerdesPriority.get());
      if (it != preClass->userAttributes().end()) {
        auto const prio = it->second;
        if (tvIsInt(prio)) {
          s_priority_serialize.emplace(val(prio).num, newClass);
        }
      }
    }

    assertx(newClass != nullptr);
    return newClass;
  }
}

Class* Class::defClosure(const PreClass* preClass, bool cache) {
  auto const nameList = preClass->namedType();

  auto const find = [&] () -> Class* {
    if (Cfg::Repo::Authoritative) {
      // In repo mode the cached class slot in the NamedType should
      // be authoritative. Closure names are unique in repo mode so
      // there should only ever be one pre-class with the same
      // name. This is more than an optimization. We don't want to
      // consult the name list below in repo mode because a class can
      // be put onto it before it's done being setup. This can cause
      // later assertions if we try to look up the closure by name
      // (for example, in an AssertRAT).
      if (auto const cls = nameList->getCachedClass()) {
        assertx(classHasPersistentRDS(cls));
        assertx(cls->preClass() == preClass);
        assertx(nameList->clsList() == cls);
        assertx(!nameList->clsList()->m_next);
        return cls;
      }
      return nullptr;
    }

    if (auto const cls = nameList->getCachedClass()) {
      if (cls->preClass() == preClass) return cls;
    }
    for (auto class_ = nameList->clsList(); class_; class_ = class_->m_next) {
      if (class_->preClass() == preClass) {
        if (cache && !classHasPersistentRDS(class_)) {
          nameList->setCachedClass(class_);
        }
        return class_;
      }
    }
    return nullptr;
  };
  if (auto const cls = find()) return cls;

  auto const parent = c_Closure::classof();
  assertx(preClass->parent() == parent->name());
  // Create a new class.

  ClassPtr newClass {
    Class::newClass(const_cast<PreClass*>(preClass), parent)
  };

  Lock l(getMutex(preClass->name()));

  if (auto const cls = find()) return cls;
  setupClass(newClass.get(), nameList);
  if (cache || classHasPersistentRDS(newClass.get())) {
    newClass.get()->setCached();
  }
  return newClass.get();
}


namespace {
void handleModuleBoundaryViolation(const Class* cls, const Func* caller) {
  if (!Cfg::Eval::EnforceModules || !cls || !caller) return;
  if (will_symbol_raise_module_boundary_violation(cls, caller)) {
    raiseModuleBoundaryViolation(cls, caller->moduleName(), caller->cls());
  }
}
} // namespace

Class* Class::resolve(const NamedType* ne, const StringData* name,
                      const Func* callerFunc) {
  Class* cls = load(ne, name);
  handleModuleBoundaryViolation(cls, callerFunc);
  return cls;
}

Class* Class::resolve(const StringData* name, const Func* callerFunc) {
  Class* cls = load(name);
  handleModuleBoundaryViolation(cls, callerFunc);
  return cls;
}

Class* Class::loadMissing(const NamedType* ne, const StringData* name) {
  VMRegAnchor _;
  CoeffectsAutoGuard _2;
  AutoloadHandler::s_instance->autoloadType(
    StrNR(const_cast<StringData*>(name))
  );
  if (!ne) ne = NamedType::getNoCreate(name);
  return ne ? ne->getCachedClass() : nullptr;
}

Class* Class::load(const NamedType* ne, const StringData* name) {
  return Class::get(ne, name, true);
}

Class* Class::get(const NamedType* ne, const StringData *name, bool tryAutoload) {
  Class *cls = ne ? ne->getCachedClass() : nullptr;
  if (LIKELY(cls != nullptr)) return cls;
  return tryAutoload ? loadMissing(ne, name) : nullptr;
}

bool Class::exists(const StringData* name, bool autoload, ClassKind kind) {
  Class* cls = Class::get(name, autoload);
  return cls &&
    (cls->attrs() & (AttrInterface | AttrTrait)) == classKindAsAttr(kind);
}

std::vector<Class*> prioritySerializeClasses() {
  assertx(Cfg::Eval::EnableIntrinsicsExtension);
  Lock l(s_priority_serialize_mutex);
  std::vector<Class*> ret;
  for (auto [_p, c] : s_priority_serialize) ret.emplace_back(c);
  return ret;
}

///////////////////////////////////////////////////////////////////////////////

namespace {
Mutex s_lazy_apc_mutex;
bool s_lazy_apc_classes_serialized = false;
std::vector<const Class*> s_lazy_apc_classes;
std::atomic<bool> s_lazy_apc_classes_final = false;

bool supportsLazyAPCDeserialization(const Class* cls) {
  return classHasPersistentRDS(cls) && !cls->isClosureClass();
}
}

bool Class::enableLazyAPCDeserialization() {
  auto const done = s_lazy_apc_classes_final.load(std::memory_order_acquire);
  auto const okay = m_useLazyAPCDeserialization.load(std::memory_order_acquire);
  if (okay) return true;
  if (done) return false;

  if (!supportsLazyAPCDeserialization(this)) return false;

  Lock lock(s_lazy_apc_mutex);
  auto const redone = s_lazy_apc_classes_final.load(std::memory_order_acquire);
  auto const reokay = m_useLazyAPCDeserialization.load(std::memory_order_acquire);
  if (reokay) return true;
  if (redone) return false;

  auto cur = this;
  while (cur != nullptr) {
    auto expected = false;
    auto& flag = const_cast<Class*>(cur)->m_useLazyAPCDeserialization;
    auto const changed = flag.compare_exchange_strong(expected, true);
    if (!changed) break;
    s_lazy_apc_classes.push_back(this);
    cur = cur->m_parent.get();
  }
  return true;
}

bool Class::mayUseLazyAPCDeserialization() const {
  if (!supportsLazyAPCDeserialization(this)) return false;
  auto const done = s_lazy_apc_classes_final.load(std::memory_order_acquire);
  auto const okay = m_useLazyAPCDeserialization.load(std::memory_order_acquire);
  return !done || okay;
}

bool Class::currentlyUsingLazyAPCDeserialization() const {
  return m_useLazyAPCDeserialization.load(std::memory_order_acquire);
}

void Class::finalizeLazyAPCClasses() {
  Lock lock(s_lazy_apc_mutex);
  s_lazy_apc_classes_final.store(true, std::memory_order_release);
}

std::vector<const Class*> Class::serializeLazyAPCClasses() {
  auto const done = s_lazy_apc_classes_final.load(std::memory_order_acquire);
  always_assert(!s_lazy_apc_classes_serialized);
  always_assert(done);

  std::vector<const Class*> result;
  std::swap(result, s_lazy_apc_classes);
  s_lazy_apc_classes_serialized = true;
  return result;
}

void Class::deserializeLazyAPCClasses(const std::vector<const Class*>& list) {
  auto const done = s_lazy_apc_classes_final.load(std::memory_order_acquire);
  always_assert(!done);

  for (auto const cls : list) {
    auto cur = cls;
    while (cur != nullptr) {
      auto expected = false;
      auto& flag = const_cast<Class*>(cur)->m_useLazyAPCDeserialization;
      auto const changed = flag.compare_exchange_strong(expected, true);
      if (!changed) break;
      cur = cur->m_parent.get();
    }
  }

  s_lazy_apc_classes_final.store(true, std::memory_order_release);
}

///////////////////////////////////////////////////////////////////////////////

}
