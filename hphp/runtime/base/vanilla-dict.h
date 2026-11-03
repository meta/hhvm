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

#include "hphp/runtime/base/array-common.h"
#include "hphp/runtime/base/array-data.h"
#include "hphp/runtime/base/array-key-types.h"
#include "hphp/runtime/base/data-walker.h"
#include "hphp/runtime/base/hash-table.h"
#include "hphp/runtime/base/string-data.h"
#include "hphp/runtime/base/tv-layout.h"
#include "hphp/runtime/base/tv-val.h"
#include "hphp/runtime/base/typed-value.h"


namespace HPHP {

//////////////////////////////////////////////////////////////////////

struct APCArray;
struct APCHandle;
struct MemoryProfile;

//////////////////////////////////////////////////////////////////////

struct VanillaDictElm {
  using hash_t = strhash_t;

  // We store values here, but also some information local to this array:
  // data.m_aux.u_hash contains either a negative number (for an int key) or
  // a string hashcode (31-bit and thus non-negative); the high bit is the
  // int/string key discriminator. data.m_type == kInvalidDataType if this is
  // an empty slot in the array (e.g. after a key is deleted).  It is
  // critical that when we return &data to clients, that they not read or
  // write the m_aux field!
  TypedValueAux data;

  // Putting the key second lets us cast VanillaDictElm* to TypedValue*, which
  // we can use to simplify the Lval sublattice in the JIT type system.
  union {
    int64_t ikey;
    StringData* skey;
  };

  void setStaticKey(StringData* k, strhash_t h) {
    assertx(k->isStatic());
    setStrKeyNoIncRef(k, h);
  }

  void setStrKeyNoIncRef(StringData* k, strhash_t h) {
    skey = k;
    data.hash() = h;
  }

  void setStrKey(StringData* k, strhash_t h) {
    skey = k;
    data.hash() = h;
    k->incRefCount();
  }

  void setIntKey(int64_t k, inthash_t h) {
    ikey = k;
    data.hash() = static_cast<int32_t>(h) | STRHASH_MSB;
    assertx(hasIntKey());
    static_assert(static_cast<int32_t>(STRHASH_MSB) < 0,
                  "high bit indicates int key");
  }

  void setTombstone() {
    // We don't explicitly check for tombstones in VanillaDict::release(),
    // instead make the deleted element appear to contain uncounted values.
    data.m_type = kInvalidDataType;
    static_assert(!isRefcountedType(kInvalidDataType), "");
    data.hash() = -1;
    assertx(hasIntKey());
  }

  hash_t probe() const {
    return hash();
  }

  TYPE_SCAN_CUSTOM() {
    static_assert(!isRefcountedType(kInvalidDataType), "");
    // if data is a Tombstone, key should have been set to int type.
    if (hasStrKey()) scanner.scan(skey);
    if (isRefcountedType(data.m_type)) scanner.scan(data.m_data.pcnt);
  }

  // Members below here are required for HashTable implemenation.
  ALWAYS_INLINE const TypedValue* datatv() const {
    return &data;
  }

  ALWAYS_INLINE bool hasStrKey() const {
    // Currently string hash is 31-bit, thus it saves us some instructions to
    // encode int keys as a negative hash, so that we don't have to care about
    // the MSB when working with strhash_t.
    return data.hash() >= 0;
  }

  ALWAYS_INLINE StringData* strKey() const {
    assertx(hasStrKey());
    return skey;
  }

  ALWAYS_INLINE bool hasIntKey() const {
    return data.hash() < 0;
  }

  ALWAYS_INLINE int64_t intKey() const {
    return ikey;
  }

  ALWAYS_INLINE TypedValue getKey() const {
    if (hasIntKey()) {
      return make_tv<KindOfInt64>(ikey);
    }
    return skey->isRefCounted() ? make_tv<KindOfString>(skey)
                                : make_tv<KindOfPersistentString>(skey);
  }

  ALWAYS_INLINE hash_t hash() const {
    return data.hash();
  }

  ALWAYS_INLINE bool isInvalid() const {
    return isTombstone();
  }

  ALWAYS_INLINE void erase() {
    if (hasStrKey()) {
      decRefStr(skey);
    }
    tvDecRefGen(data);
  }

  // Elm's data.m_type == kInvalidDataType for deleted slots.
  ALWAYS_INLINE bool isTombstone() const {
    assertx(isRealType(data.m_type) || data.m_type == kInvalidDataType);
    return data.m_type == kInvalidDataType;
  }

  static constexpr ptrdiff_t keyOff() {
    return offsetof(VanillaDictElm, ikey);
  }
  static constexpr ptrdiff_t dataOff() {
    return offsetof(VanillaDictElm, data) + offsetof(TypedValue, m_data);
  }
  static constexpr ptrdiff_t typeOff() {
    return offsetof(VanillaDictElm, data) + offsetof(TypedValue, m_type);
  }
  static constexpr ptrdiff_t hashOff() {
    return offsetof(VanillaDictElm, data) + offsetof(TypedValue, m_aux);
  }
};

struct VanillaDict final : ArrayData,
                          array::HashTable<VanillaDict, VanillaDictElm>,
                          type_scan::MarkCollectable<VanillaDict> {
  struct ElmKey {
    ElmKey() {}
    ElmKey(strhash_t hash, StringData* key)
        : skey(key), hash(hash)
      {}
    union {
      StringData* skey;
      int64_t ikey;
    };
    int32_t hash;

    TYPE_SCAN_CUSTOM_FIELD(skey) {
      if (hash >= 0) scanner.scan(skey);
    }
  };

  /*
   * Allocate a new, empty, request-local array in (mixed|dict) mode, with
   * enough space reserved for `capacity' members.
   *
   * The returned array is already incref'd.
   */
  static ArrayData* MakeReserveDict(uint32_t size);
  static auto constexpr MakeReserve = &MakeReserveDict;

  /*
   * Allocates a new request-local array with given key,value,key,value,... in
   * natural order. Returns nullptr if there are duplicate keys. Does not check
   * for integer-like keys. Takes ownership of keys and values iff successful.
   */
  static VanillaDict* MakeDict(uint32_t size, const TypedValue* kvs);

private:
  template<HeaderKind hdr>
  static VanillaDict* MakeMixedImpl(uint32_t size, const TypedValue* kvs);

public:
  static constexpr size_t kKeyTypesOffset = offsetof(ArrayData, m_extra_lo8);

  const ArrayKeyTypes& keyTypes() const {
    auto const pointer = uintptr_t(this) + kKeyTypesOffset;
    return *reinterpret_cast<ArrayKeyTypes*>(pointer);
  }

  ArrayKeyTypes* mutableKeyTypes() {
    auto const pointer = uintptr_t(this) + kKeyTypesOffset;
    return reinterpret_cast<ArrayKeyTypes*>(pointer);
  }

public:
  /*
   * Same semantics as VanillaVec::MakeNatural().
   */
  static VanillaDict* MakeDictNatural(uint32_t size, const TypedValue* vals);

  /*
   * Like MakePacked, but given static strings, make a struct-like array.
   * Also requires size > 0.
   */
  static VanillaDict* MakeStructDict(uint32_t size,
                                    const StringData* const* keys,
                                    const TypedValue* values);

  /*
   * Allocate a struct-like array (with string literal keys), but only init
   * the hash table and the header, leaving elms uninit. Requires size > 0.
   */
  static VanillaDict* AllocStructDict(uint32_t size, const int32_t* hash);

  /*
   * Allocate an uncounted VanillaDict and copy the values from the
   * input 'array' into the uncounted one. All values copied are made
   * uncounted as well.  An uncounted array can only contain uncounted
   * values (primitive values, uncounted or static strings and
   * uncounted or static arrays).
   *
   * If withApcTypedValue is true, space for an APCTypedValue will be
   * allocated in front of the returned pointer.
   */
  static ArrayData* MakeShared(
      ArrayData* array, const MakeSharedEnv& env, bool hasApcTv);

  static ArrayData* MakeDictFromAPC(const APCArray* apc, bool pure, bool isLegacy = false);

  static bool DictEqual(const ArrayData*, const ArrayData*);
  static bool DictNotEqual(const ArrayData*, const ArrayData*);
  static bool DictSame(const ArrayData*, const ArrayData*);
  static bool DictNotSame(const ArrayData*, const ArrayData*);

  using ArrayData::decRefCount;
  using ArrayData::hasMultipleRefs;
  using ArrayData::hasExactlyOneRef;
  using ArrayData::decWillRelease;
  using ArrayData::incRefCount;

  /*
   * VanillaDict is convertible to ArrayData*, but not implicitly.
   * This is to avoid accidentally using virtual dispatch when you
   * already know something is Mixed.
   *
   * I.e., instead of doing things like mixed->nvGet(...) you want to
   * do VanillaDict::NvGetInt(adYouKnowIsMixed, ...).  This means using
   * VanillaDict*'s directly shouldn't really happen very often.
   */
  ArrayData* asArrayData() { return this; }
  const ArrayData* asArrayData() const { return this; }

  // These using directives ensure the full set of overloaded functions
  // are visible in this class, to avoid triggering implicit conversions
  // from a const Variant& key to int64.
private:
  using ArrayData::exists;
  using ArrayData::at;
  using ArrayData::lval;
  using ArrayData::removeMove;
  using ArrayData::release;

public:
  static TypedValue GetPosVal(const ArrayData*, ssize_t pos);
  static bool PosIsValid(const ArrayData*, ssize_t pos);
  static bool IsVectorData(const ArrayData*);
  static bool ExistsInt(const ArrayData*, int64_t k);
  static bool ExistsStr(const ArrayData*, const StringData* k);
  static arr_lval LvalInt(ArrayData* ad, int64_t k);
  static arr_lval LvalStr(ArrayData* ad, StringData* k);
  static ArrayData* SetPosMove(ArrayData*, ssize_t pos, TypedValue v);
  static ArrayData* SetIntMove(ArrayData*, int64_t k, TypedValue v);
  static ArrayData* SetStrMove(ArrayData*, StringData* k, TypedValue v);
  static ArrayData* SetIntMoveSkipConflict(ArrayData* ad, int64_t k, TypedValue v);
  static ArrayData* SetStrMoveSkipConflict(ArrayData* ad, StringData* k, TypedValue v);
  static ArrayData* RemoveIntMove(ArrayData*, int64_t k);
  static ArrayData* RemoveStrMove(ArrayData*, const StringData* k);
  static ArrayData* Copy(const ArrayData*);
  static ArrayData* CopyStatic(const ArrayData*);
  static ArrayData* AppendMove(ArrayData*, TypedValue v);
  static ArrayData* PopMove(ArrayData*, Variant& value);

  static void OnSetEvalScalar(ArrayData*);
  static void Release(ArrayData*);
  static void ReleaseShared(ArrayData*);

  static ArrayData* EscalateForSort(ArrayData* ad, SortFunction sf);
  static void Ksort(ArrayData*, int sort_flags, bool ascending);
  static void Sort(ArrayData*, int sort_flags, bool ascending);
  static void Asort(ArrayData*, int sort_flags, bool ascending);
  static bool Uksort(ArrayData*, const Variant& cmp_function);
  static bool Usort(ArrayData*, const Variant& cmp_function);
  static bool Uasort(ArrayData*, const Variant& cmp_function);

  //////////////////////////////////////////////////////////////////////

  // Helpers used by ArrayInit to update VanillaDicts without refcount ops.
  // These helpers work for both mixed PHP arrays and dicts.
  static ArrayData* SetIntInPlace(ArrayData*, int64_t k, TypedValue v);
  static ArrayData* SetStrInPlace(ArrayData*, StringData* k, TypedValue v);

  // Return an lval to the value at the given key. If `k` is not present,
  // this method will insert a null value for it and return it.
  //  @precondition: !isFull
  //  @precondition: !cowCheck
  static tv_lval LvalInPlace(ArrayData* ad, const Variant& k);

  // If the key is not present in the array, return a null lval; else,
  // COW the array if necessary and return an lval into the new array.
  static arr_lval LvalSilentInt(ArrayData* ad, int64_t k);
  static arr_lval LvalSilentStr(ArrayData* ad, StringData* k);

  // When we escalate a MonotypeDict to a vanilla VanillaDict, we must ensure
  // that iterator positions match (local iterators rely on this invariant).
  static void AppendTombstoneInPlace(ArrayData* ad);

  //////////////////////////////////////////////////////////////////////

private:
  VanillaDict* copyMixed() const;

  static bool DictEqualHelper(const ArrayData*, const ArrayData*, bool);

public:
  const TypedValue* getArrayElmPtr(ssize_t pos) const;

  bool isTombstone(ssize_t pos) const;
  // Elm's data.m_type == kInvalidDataType for deleted slots.
  static bool isTombstone(DataType t) {
    return t == kInvalidDataType;
  }

private:
  friend struct array::HashTable<VanillaDict, VanillaDictElm>;
  friend struct MemoryProfile;
  friend struct VanillaVec;
  friend struct HashCollection;
  friend struct BaseMap;
  friend struct c_Map;
  friend struct c_ImmMap;
  friend struct BaseSet;
  friend struct c_Set;
  friend struct c_ImmSet;
  friend struct c_AwaitAllWaitHandle;

  friend size_t getMemSize(const ArrayData*, bool);
  template <typename AccessorT, class ArrayT>
  friend SortFlavor genericPreSort(ArrayT&, const AccessorT&, bool);

public:
  // Safe downcast helpers
  static VanillaDict* as(ArrayData* ad) {
    assertx(ad->isVanillaDict());
    auto a = static_cast<VanillaDict*>(ad);
    assertx(a->checkInvariants());
    return a;
  }
  static const VanillaDict* as(const ArrayData* ad) {
    assertx(ad->isVanillaDict());
    auto a = static_cast<const VanillaDict*>(ad);
    assertx(a->checkInvariants());
    return a;
  }

  // Fast iteration
  template <class F>
  static void IterateV(const VanillaDict* arr, F fn) {
    assertx(arr->isVanillaDict());
    auto elm = arr->data();
    for (auto i = arr->m_used; i--; elm++) {
      if (LIKELY(!elm->isTombstone())) {
        if (ArrayData::call_helper(fn, elm->data)) break;
      }
    }
  }
  template <class F>
  static void IterateKV(const VanillaDict* arr, F fn) {
    assertx(arr->isVanillaDict());
    auto elm = arr->data();
    for (auto i = arr->m_used; i--; elm++) {
      if (LIKELY(!elm->isTombstone())) {
        TypedValue key;
        key.m_data.num = elm->ikey;
        key.m_type = elm->hasIntKey() ? KindOfInt64 : KindOfString;
        if (ArrayData::call_helper(fn, key, elm->data)) break;
      }
    }
  }

private:
  enum class AllocMode : bool { Request, Static };

  static VanillaDict* CopyMixed(const VanillaDict& other, AllocMode);
  static VanillaDict* CopyReserve(const VanillaDict* src, size_t expectedSize);

  // Slow paths used for VanillaDicts with references or counted string keys.
  // We fall back to SlowCopy and SlowGrow in the middle of iteration when we
  // encounter a reference, so we take an (elm, end) pair as arguments.
  static VanillaDict* SlowCopy(VanillaDict*, const ArrayData& old,
                              VanillaDictElm* elm, VanillaDictElm* end);
  static VanillaDict* SlowGrow(VanillaDict*, const ArrayData& old,
                              VanillaDictElm* elm, VanillaDictElm* end);
  static void SlowRelease(VanillaDict*);

  VanillaDict() = delete;
  VanillaDict(const VanillaDict&) = delete;
  VanillaDict& operator=(const VanillaDict&) = delete;
  ~VanillaDict() = delete;

private:
  // Copy elements as well as `m_nextKI' from one VanillaDict to another.
  // Warning: it could copy up to 24 bytes beyond the array and thus overwrite
  // the hashtable, but it never reads/writes beyond the end of the hash
  // table.  If you use this function, make sure you copy/write the correct
  // data on the hash table afterwards.
  static void copyElmsNextUnsafe(VanillaDict* to, const VanillaDict* from,
                                 uint32_t nElems);

  template <typename AccessorT>
  SortFlavor preSort(const AccessorT& acc, bool checkTypes);
  void postSort(bool resetKeys);

  // Assert a bunch of invariants about this array then return true.
  // usage:  assertx(checkInvariants());
  bool checkInvariants() const;

private:
  // The array should already be sized for the new insertion before
  // calling these methods.
  struct InsertPos {
    InsertPos(bool found, TypedValue& tv) : found(found), tv(tv) {}
    bool found;
    TypedValue& tv;
  };
  InsertPos insert(int64_t k);
  InsertPos insert(StringData* k);

  using HashTable<VanillaDict, VanillaDictElm>::findForRemove;

  void nextInsert(TypedValue);

  template <class K> arr_lval addLvalImpl(K k);
  template <class K> ArrayData* update(K k, TypedValue data);
  template <class K> ArrayData* updateSkipConflict(K k, TypedValue data);

  void updateNextKI(int64_t removedKey, bool updateNext);
  void eraseNoCompact(RemovePos pos);

  VanillaDict* moveVal(TypedValue& tv, TypedValue v);

  /*
   * Grow makes a copy of the array with scale = newScale. Grow rebuilds the
   * hash table, but it does not compact the elements. If copy is true, it
   * will copy elements instead of taking ownership of them.
   */
  static VanillaDict* Grow(VanillaDict* old, uint32_t newScale, bool copy);

  /*
   * prepareForInsert ensures that the array has room to insert an element and
   * has a refcount of 1, copying if requested and growing if needed.
   */
  VanillaDict* prepareForInsert(bool copy);

  /**
   * compact() does not change the hash table size or the number of slots
   * for elements. compact() rebuilds the hash table and compacts the
   * elements into the slots with lower addresses.
   */
  void compact();

  bool isZombie() const { return m_used + 1 == 0; }
  void setZombie() { m_used = -uint32_t{1}; }

public:
  void scan(type_scan::Scanner&) const; // in vanilla-dict-defs.h

private:
  struct DictInitializer;
  static DictInitializer s_dict_initializer;

  struct MarkedDictArrayInitializer;
  static MarkedDictArrayInitializer s_marked_dict_initializer;

  int64_t  m_nextKI;        // Next integer key to use for append.
};

HASH_TABLE_CHECK_OFFSETS(VanillaDict, VanillaDictElm)
//////////////////////////////////////////////////////////////////////

}
