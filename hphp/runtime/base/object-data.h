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

#include "hphp/runtime/base/classname-is.h"
#include "hphp/runtime/base/countable.h"
#include "hphp/runtime/base/memory-manager.h"
#include "hphp/runtime/base/req-ptr.h"
#include "hphp/runtime/base/tv-conv-throw.h"
#include "hphp/runtime/base/tv-val.h"
#include "hphp/runtime/base/weakref-data.h"

#include "hphp/runtime/vm/class.h"
#include "hphp/runtime/vm/hhbc.h"

#include "hphp/util/ptr.h"
#include "hphp/util/rds-local.h"

#include <vector>

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

struct TypedValue;

void deepInitHelper(ObjectProps* propVec,
                    const Class::PropInitVec* propInitVec,
                    size_t nProps);

namespace Native {
  ObjectData* nativeDataInstanceCopyCtor(ObjectData *src, Class* cls,
                                         size_t nProps);
}

// A slot to store memoization data in. This can be either a TypedValue storing a
// single value, or a pointer to a memoization cache.
struct MemoSlot {
public:
  /*
   * When we initialize a MemoSlot, we both make its type KindOfUninit
   * (so that it's a valid value slot) and make its cache pointer nullptr
   * (so that it's a valid cache slot). We don't need to know which one it is.
   */
  void init() {
    value.m_data.pcache = nullptr;
    value.m_type = KindOfUninit;
  }

  /*
   * We use the type field of the TypedValue to determine whether this is a
   * single value or a memo cache:
   *
   *  - If the type is kInvalidDataType (i.e. it's not a valid TypedValue),
   *    this slot is for a memo cache.
   *
   *  - If the type is valid and not KindOfUninit, then it's a single value.
   *
   *  - If the type is Uninit, and the cache pointer is nullptr, we cannot
   *    distinguish the two cases - that's how we initialize these slots.
   *
   * The ambiguity in the last case is okay, because we only use the helpers
   * below for assertions. For any given memo slot, the single vs. multiple
   * value distinction is implied by whether the function takes arguments.
   */
  bool isCache() const {
    return value.m_type == kInvalidDataType ||
           (value.m_type == KindOfUninit && value.m_data.pcache == nullptr);
  }
  bool isValue() const {
    return value.m_type != kInvalidDataType;
  }

  // Get a reference to the pointer to the cache, for the purpose of a set on
  // the cache. Since we're going to be creating a cache in this slot, change
  // its type to indicate this.
  MemoCacheBase*& getCacheForWrite() {
    assertx(isCache());
    value.m_type = kInvalidDataType;
    return value.m_data.pcache;
  }

  MemoCacheBase* getCache() {
    assertx(isCache());
    return value.m_data.pcache;
  }
  const MemoCacheBase* getCache() const {
    assertx(isCache());
    return value.m_data.pcache;
  }

  TypedValue* getValue() {
    assertx(isValue());
    assertx(tvIsPlausible(value));
    return &value;
  }
  const TypedValue* getValue() const {
    assertx(isValue());
    assertx(tvIsPlausible(value));
    return &value;
  }

  // Used when we've freed the cache manually
  void resetCache() {
    assertx(isCache());
    value.m_data.pcache = nullptr;
  }

private:
  TYPE_SCAN_CUSTOM() {
    isCache() ? scanner.scan(value.m_data.pcache) : scanner.scan(value);
  }

  TypedValue value;
};
static_assert(sizeof(MemoSlot) == sizeof(TypedValue), "");

#if defined(__x86_64__)
#pragma pack(push, 1)
#endif

struct ObjectData : Countable, type_scan::MarkCollectable<ObjectData> {
  enum Attribute : uint8_t {
    NoAttrs            = 0x00,
    IsWeakRefed        = 0x02, // Is pointed to by at least one WeakRef
    HasDynPropArr      = 0x04, // has a dynamic properties array
    IsBeingConstructed = 0x08, // Constructor for most derived class has not
                               // finished. Set during construction to
                               // temporarily allow writing to const props.
    UsedMemoCache      = 0x10, // Object has had data set in its memo slots
    BigAllocSize       = 0x20, // The object was allocated using Big Size
#ifndef NDEBUG
    SmallAllocSize     = 0x40, // For Debug, the object was allocated with
                               // small buffers. If needed can be removed to
                               // free up Attribute bits.
#else
    SmallAllocSize     = NoAttrs,
#endif
  };

  static constexpr size_t offsetofAttrs() {
    return offsetof(ObjectData, m_aux16);
  }

  static constexpr size_t sizeofAttrs() {
    return sizeof(m_aux16);
  }

  explicit ObjectData(Class*, uint8_t flags = 0,
                      HeaderKind = HeaderKind::Object);
  ~ObjectData();

  // Disallow copy construction and assignemt
  ObjectData(const ObjectData&) = delete;
  ObjectData& operator=(const ObjectData&) = delete;

  enum class InitRaw {};
 protected:
  enum class NoInit {};

  // for JIT-generated instantiation with inlined property init
  explicit ObjectData(Class* cls, InitRaw, uint8_t flags = 0,
                      HeaderKind = HeaderKind::Object) noexcept;

  // for C++ subclasses with no declared properties
  explicit ObjectData(Class* cls, NoInit, uint8_t flags = 0,
                      HeaderKind = HeaderKind::Object) noexcept;

 public:
  ALWAYS_INLINE void decRefAndRelease() {
    assertx(kindIsValid());
    if (decReleaseCheck()) {
      auto const cls = getVMClass();
      return cls->releaseFunc()(this, cls);
    }
  }
  bool kindIsValid() const { return isObjectKind(headerKind()); }

  void scan(type_scan::Scanner&) const;

  size_t heapSize() const;

  void setWeakRefed() { setAttribute(IsWeakRefed); }

  void setReifiedGenerics(Class*, ArrayData*);

 private:
  template <bool Unlocked, typename Init>
  static ObjectData* newInstanceImpl(Class*, Init);


  /*
   * Allocate space for an object data of type `cls`.  If it should have a
   * Memo header create space for that and initialize it.
   *
   * Return the pointer to address where the object-data should start (`mem`),
   * along with flags indicating if the allocation was a small or big size
   * class alloc (`flags`).  These flags ultimately must be incorporated into
   * the object-data's attributes in order for the deallocation to follow the
   * correct path.
   */
  struct Alloc {
    void* mem;
    uint8_t flags;
  };
  static Alloc allocMemoInit(Class* cls);
 public:
  /*
   * Call newInstance() to instantiate a PHP object. The initial ref-count will
   * be greater than zero. Will raise if the class has reified generics; if
   * the class may have reified generics, you must use newInstanceReified.
   * Since this gives you a raw pointer, it is your responsibility to manage
   * the ref-count yourself. Whenever possible, prefer using the Object class
   * instead, which takes care of this for you.
   */
  template <bool Unlocked = false>
  static ObjectData* newInstance(Class*);

  /*
   * Same as newInstance, but classes with reified generics are allowed.
   * If the class has reified generics, the second arg must be an array of
   * type structures representing the reified types.
   */
  template <bool Unlocked = false>
  static ObjectData* newInstanceReified(Class*, ArrayData*);

  /*
   * Instantiate a new object without initializing its declared properties. The
   * given Class must be a concrete, regular Class.
   */
  static ObjectData* newInstanceNoPropInit(Class*);

  /*
   * Given a Class that is assumed to be a concrete, regular (not a trait or
   * interface), pure PHP class, and an allocation size, return a new,
   * uninitialized object of that class. These are meant to be called from the
   * JIT, where the cls, size, and attributes are constants at JIT time.
   *
   * The big=true versions should be called when size > kMaxSmallSize.
   *
   * The memo versions should be used if the object has memo slots.
   *
   * The initial ref-count will be set to one.
   */
  static ObjectData* newInstanceRawSmall(Class*, size_t size, size_t index);
  static ObjectData* newInstanceRawBig(Class*, size_t size);

  static ObjectData* newInstanceRawMemoSmall(Class*, size_t size,
                                             size_t index, size_t objoff);
  static ObjectData* newInstanceRawMemoBig(Class*, size_t size, size_t objoff);

  /*
   * Default release function, used for non-closure, non-native objects.
   */
  static void release(ObjectData* obj, const Class* cls) noexcept;

  /*
   * An optimised version of the above function when the class can be loaded
   * from the object.
   */
  static void release2(ObjectData* obj) noexcept;

  Class* getVMClass() const;
  StrNR getClassName() const;

  // instanceof() can be used for both classes and interfaces.
  bool instanceof(const OptString&) const;
  bool instanceof(const Class*) const;

  template <typename T>
  typename std::enable_if<
    std::is_same<ObjectData,T>::value,
    bool
  >::type instanceof() { return true; }

  template <typename T>
  typename std::enable_if<
    !std::is_same<ObjectData,T>::value,
    bool
  >::type instanceof() { return instanceof(T::classof()); }

  // Whether the object implements Iterator.
  bool isIterator() const;

  // Has a custom instanceCtor and instanceDtor. If you subclass ObjectData
  // in C++, you need this.
  bool isCppBuiltin() const;

  // Is this an object for which construction has not finished yet?
  bool isBeingConstructed() const;
  // Clear the IsBeingConstructed bit to indicate that construction is done.
  void lockObject();
  // Temporarily set the IsBeingConstructed bit
  void unlockObject();

  // Whether the object is a collection, [and [not] mutable].
  bool isCollection() const;
  bool isMutableCollection() const;
  bool isImmutableCollection() const;
  CollectionType collectionType() const; // asserts(isCollection())
  HeaderKind headerKind() const;

  // True if this is a c_Awaitable or derived
  bool isWaitHandle() const;

  bool getAttribute(Attribute) const;
  void setAttribute(Attribute);
  bool hasInstanceDtor() const;
  bool hasNativeData() const;

  Object iterableObject(bool& isIterable, bool mayImplementIterator = true);

  /*
   * Type conversions. Some subclasses of ObjectData have custom conversions.
   * (e.g. SimpleXMLElement -> bool)
   */
  bool toBoolean() const;
  int64_t toInt64() const;
  double toDouble() const;

  template <IntishCast IC = IntishCast::None>
  Array toArray(bool pubOnly = false, bool ignoreLateInit = false) const;

  /*
   * Comparisons.
   *
   * Note that for objects !(X < Y) does *not* imply (X >= Y).
   */
  bool equal(const ObjectData&) const;
  bool less(const ObjectData&) const;
  bool lessEqual(const ObjectData&) const;
  bool more(const ObjectData&) const;
  bool moreEqual(const ObjectData&) const;
  int64_t compare(const ObjectData&) const;

 private:
  void instanceInit(Class*);

 public:

  Array o_toIterArray(const Class* ctx);

  Variant o_get(const OptString& s, bool error = true,
                const OptString& context = null_string);

  void o_set(const OptString& s, const Variant& v,
             const OptString& context = null_string);

  void o_setArray(const Array& properties);
  void o_getArray(Array& props,
                  bool pubOnly = false,
                  bool ignoreLateInit = false) const;

  static Object FromArray(ArrayData* properties);

  // TODO Task #2584896: o_invoke and o_invoke_few_args are deprecated. These
  // APIs don't properly take class context into account when looking up the
  // method, and they duplicate some of the functionality from invokeFunc(),
  // invokeFuncFew(), and vm_decode_function(). We should remove these APIs and
  // migrate all callers to use invokeFunc(), invokeFuncFew(), and
  // vm_decode_function() instead.
  Variant o_invoke(const OptString& s, const Variant& params, bool fatal = true);
  Variant o_invoke_few_args(const OptString& s,
                            RuntimeCoeffects providedCoeffects,
                            int count,
                            const Variant& a0 = uninit_variant,
                            const Variant& a1 = uninit_variant,
                            const Variant& a2 = uninit_variant,
                            const Variant& a3 = uninit_variant,
                            const Variant& a4 = uninit_variant);

  ObjectData* clone();

  OptString invokeToString();
  bool hasToString();

  Variant invokeSleep(RuntimeCoeffects);
  Variant invokeToDebugDisplay(RuntimeCoeffects);
  Variant invokeWakeup(RuntimeCoeffects);
  Variant invokeDebugInfo(RuntimeCoeffects);

  Variant static InvokeSimple(ObjectData* data, const StaticString& name,
                              RuntimeCoeffects);

  /*
   * Returns whether this object has any dynamic properties.
   */
  bool hasDynProps() const;

  /*
   * Returns a reference to dynamic properties Array for this object.
   * The reference points into an entry in ExecutionContext::dynPropArray,
   * so is only valid for a short lifetime, until another entry is inserted
   * or erased (anything that moves entries).
   *
   * Note: you're generally not going to want to copy-construct the
   * return value of this function.  If you want to make changes to
   * the property array, we need to keep its ref count at 1.
   *
   * Pre: getAttribute(HasDynPropArr)
   */
  Array& dynPropArray() const;

  /*
   * Use the given array for this object's dynamic properties. HasDynPropArry
   * must not already be set. Returns a reference to the Array in its final
   * location.
   */
  void setDynProps(const Array&);
  void reserveDynProps(int nProp);

  // Accessors for the declared properties area. Note that if the caller writes
  // to these properties, they are responsible for validating the values with
  // any type-hints on the properties. Likewise the caller is responsible for
  // enforcing AttrLateInit.
  ObjectProps* props();
  const ObjectProps* props() const;

  // TODO(T61738946): These can be const once we remove support for coercing
  // class_meth types.
  void verifyPropTypeHints();
  void verifyPropTypeHints(size_t end);
  void verifyPropTypeHint(Slot slot);

  bool assertPropTypeHints() const;

  // Accessors for declared properties at statically known offsets. In the lval
  // case, the property must be statically known to be mutable. If the caller
  // modifies the lval, they are responsible for validating the value with any
  // type-hint on that property. Likewise the caller is responsible for
  // enforcing AttrLateInit.
  tv_lval propLvalAtOffset(Slot);
  tv_rval propRvalAtOffset(Slot) const;

  // Get a pointer to the i-th memo slot. The object must not have native data.
  MemoSlot* memoSlot(Slot);
  const MemoSlot* memoSlot(Slot) const;

  // Get a pointer to the i-th memo slot. Use these if the object has native
  // data. The second parameter is the size of the object's native data.
  MemoSlot* memoSlotNativeData(Slot, size_t);
  const MemoSlot* memoSlotNativeData(Slot, size_t) const;

 public:
  const Func* methodNamed(const StringData*) const;
  static size_t sizeForNProps(Slot);

  static size_t objOffFromMemoNode(const Class*);

  //============================================================================
  // Properties.
 private:
  /*
   * Use the given array for this object's dynamic properties. HasDynPropArry
   * must not already be set. Returns a reference to the Array in its final
   * location.
   */
  Array& setDynPropArray(const Array&);

  /*
   * Create the dynamic property array for this ObjectData if it
   * doesn't already exist yet.
   *
   * Post: getAttribute(HasDynPropArr)
   */
  Array& reserveProperties(int nProp = 2);

  [[noreturn]] NEVER_INLINE void throwMutateConstProp(Slot prop) const;
  [[noreturn]] NEVER_INLINE void throwMustBeMutable(Slot prop) const;
  [[noreturn]] NEVER_INLINE void throwMustBeEnclosedInReadonly(Slot prop) const;
  [[noreturn]] NEVER_INLINE void throwMustBeReadonly(Slot prop) const;
  [[noreturn]] NEVER_INLINE void throwMustBeValueType(Slot prop) const;

 public:
  // never box the lval returned from getPropLval; use propB instead
  tv_lval getPropLval(const MemberLookupContext&, const StringData*);
  tv_rval getProp(const MemberLookupContext&, const StringData*) const;
  // Like getProp() but does not throw for <<__LateInit>>. Value can be
  // KindOfUninit.
  tv_rval getPropIgnoreLateInit(const MemberLookupContext& ctx,
                                const StringData* key) const;
  // don't use getPropIgnoreAccessibility in new code
  tv_lval getPropIgnoreAccessibility(const StringData*);

  void deserializeAllLazyProps();
  static void deserializeLazyProp(tv_lval prop);
  static bool isLazyProp(tv_rval prop);

 private:
  struct PropLookup {
    tv_lval val;
    const Class::Prop* prop;
    Slot slot;
    bool accessible;
    bool isConst;
    bool readonly;
  };

  void checkReadonly(const PropLookup&, ReadonlyOp, bool) const;

  template <bool forWrite, bool forRead, bool ignoreLateInit>
  ALWAYS_INLINE
  PropLookup getPropImpl(const MemberLookupContext&, const StringData*);

  enum class PropMode : int {
    ReadNoWarn,
    ReadWarn,
    DimForWrite,
  };

  template<PropMode mode>
  tv_lval propImpl(TypedValue* tvRef, const MemberLookupContext& ctx, const StringData* key, const ReadonlyOp op = ReadonlyOp::Any);

  void setDynProp(const StringData* key, TypedValue val);

 public:
  tv_lval prop(TypedValue* tvRef, const MemberLookupContext& ctx, const StringData* key, const ReadonlyOp op);
  tv_lval propW(TypedValue* tvRef, const MemberLookupContext& ctx, const StringData* key, const ReadonlyOp op);
  tv_lval propU(TypedValue* tvRef, const MemberLookupContext& ctx, const StringData* key, const ReadonlyOp op);
  tv_lval propD(TypedValue* tvRef, const MemberLookupContext& ctx, const StringData* key, const ReadonlyOp op);

  bool propIsset(const MemberLookupContext& ctx, const StringData* key);

  void setProp(const MemberLookupContext& ctx, const StringData* key, TypedValue val, ReadonlyOp op = ReadonlyOp::Any);
  tv_lval setOpProp(TypedValue& tvRef, const MemberLookupContext& ctx, SetOpOp op,
                    const StringData* key, TypedValue* val);

  TypedValue incDecProp(const MemberLookupContext& ctx, IncDecOp op, const StringData* key);

  void unsetProp(const MemberLookupContext& ctx, const StringData* key);

  tv_lval makeDynProp(const StringData* key);

  static void throwObjToIntException(const char*);
  static void throwObjToDoubleException(const char*);
  static void raiseAbstractClassError(Class*);
  void throwUndefPropException(const StringData*) const;
  void raiseCreateDynamicProp(const StringData*) const;
  void raiseReadDynamicProp(const StringData*) const;

  static constexpr ptrdiff_t getVMClassOffset() {
    return offsetof(ObjectData, m_cls);
  }
  const char* classname_cstr() const;

private:
  friend struct MemoryProfile;
  friend ObjectData* Native::nativeDataInstanceCopyCtor(
    ObjectData* src, Class* cls, size_t nProps);

  bool toBooleanImpl() const noexcept;

  bool slowDestroyCheck() const;
  void slowDestroyCases();

  bool assertTypeHint(tv_rval, Slot) const;

  // TODO(T61738946): We can take a tv_rval here once we remove support for
  // coercing class_meth types.
  void verifyPropTypeHintImpl(tv_lval, const Class::Prop&) const;

// offset:     0       8       12      16
// 64bit:      header  cls             [subclass][props...]
// packedptr:  header  cls     [subclass][props...]

private:
  const PackedPtr<Class> m_cls;
};
#if defined(__x86_64__)
#pragma pack(pop)
#endif

#if defined(__x86_64__)
static_assert(sizeof(ObjectData) == 12, "Change this only on purpose");
static_assert(alignof(ObjectData) == 1);
#else
static_assert(sizeof(ObjectData) == 16, "Change this only on purpose");
static_assert(alignof(ObjectData) == 8);
#endif
///////////////////////////////////////////////////////////////////////////////

ALWAYS_INLINE void decRefObj(ObjectData* obj) {
  obj->decRefAndRelease();
}

/*
 * Write a value to `to', with Dup semantics.
 *
 * @see: tv-mutate.h
 */
ALWAYS_INLINE void tvWriteObject(ObjectData* pobj, TypedValue* to) {
  to->m_type = KindOfObject;
  to->m_data.pobj = pobj;
  to->m_data.pobj->incRefCount();
}

///////////////////////////////////////////////////////////////////////////////

#define DECLARE_CLASS_NO_SWEEP(originalName)                           \
  public:                                                              \
  CLASSNAME_IS(#originalName)                                          \
  friend ObjectData* new_##originalName##_Instance(Class*);            \
  friend void delete_##originalName(ObjectData*, const Class*);        \
  static HPHP::PackedPtr<Class> s_classOf;                             \
  static inline HPHP::PackedPtr<Class>& classof() {                    \
    return s_classOf;                                                  \
  }

#define IMPLEMENT_CLASS_NO_SWEEP(cls)                                  \
  HPHP::PackedPtr<Class> c_##cls::s_classOf;

namespace req {

template<class T, class... Args>
typename std::enable_if<
  std::is_convertible<T*, ObjectData*>::value,
  req::ptr<T>
>::type make(Args&&... args) {
  auto const mem = tl_heap->objMalloc(sizeof(T));
  (void)type_scan::getIndexForMalloc<T>(); // ensure T* ptrs are interesting
  try {
    auto t = new (mem) T(std::forward<Args>(args)...);
    assertx(t->hasExactlyOneRef());
    return req::ptr<T>::attach(t);
  } catch (...) {
    tl_heap->objFree(mem, sizeof(T));
    throw;
  }
}
} // namespace req

///////////////////////////////////////////////////////////////////////////////
}

///////////////////////////////////////////////////////////////////////////////

#define incl_HPHP_OBJECT_DATA_INL_H_
#include "hphp/runtime/base/object-data-inl.h"
#undef incl_HPHP_OBJECT_DATA_INL_H_

///////////////////////////////////////////////////////////////////////////////
