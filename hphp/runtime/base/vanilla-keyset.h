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
#include "hphp/runtime/base/data-walker.h"
#include "hphp/runtime/base/hash-table.h"
#include "hphp/runtime/base/string-data.h"
#include "hphp/runtime/base/tv-val.h"
#include "hphp/runtime/base/typed-value.h"


namespace HPHP {

//////////////////////////////////////////////////////////////////////

namespace jit {
struct ArrayAccessProfile;
}
struct APCArray;
struct APCHandle;

struct VanillaKeysetElm {
  using hash_t = strhash_t;
  /*
   * We store elements of the set here, but also some information
   * local to this array: tv.m_aux.u_hash contains either a negative
   * number (for an int key) or a string hashcode (31-bit and thus
   * non-negative).  The field tv.m_type == kInvalidDataType is used
   * to mark a deleted element (tombstone); tv.m_type == KindOfUninit
   * is used for debugging to mark empty elements.
   */
  TypedValueAux tv;

  static auto constexpr kTombstone = kInvalidDataType;
  static auto constexpr kEmpty = kExtraInvalidDataType;

  template <bool Move>
  void setStrKey(StringData* k, strhash_t h) {
    assertx(isEmpty());
    if constexpr (!Move) {
      k->incRefCount();
    }
    tv.m_type = KindOfString;
    tv.m_data.pstr = k;
    tv.hash() = h;
    assertx(!isInvalid());
  }

  void setIntKey(int64_t k, inthash_t h) {
    assertx(isEmpty());
    tv.m_type = KindOfInt64;
    tv.m_data.num = k;
    tv.hash() = h | STRHASH_MSB;
    assertx(!isInvalid());
    assertx(hasIntKey());
  }

  void setTombstone() {
    tv.m_type = kTombstone;
    static_assert(!isRefcountedType(kTombstone), "");
  }

  bool isEmpty() const {
    return tv.m_type == kEmpty;
  }

  // Members below here are required for HashTable implemenation.
  ALWAYS_INLINE const TypedValue* datatv() const {
    return &tv;
  }

  ALWAYS_INLINE bool hasStrKey() const {
    /*
     * Currently string hash is 31-bit, thus it saves us some
     * instructions to encode int keys as a negative hash, so
     * that we don't have to care about the MSB when working
     * with strhash_t.
     */
    assertx(!isInvalid());
    return tv.hash() >= 0;
  }

  ALWAYS_INLINE StringData* strKey() const {
    assertx(hasStrKey());
    return tv.m_data.pstr;
  }

  ALWAYS_INLINE bool hasIntKey() const {
    assertx(!isInvalid());
    return tv.hash() < 0;
  }

  ALWAYS_INLINE int64_t intKey() const {
    return tv.m_data.num;
  }

  ALWAYS_INLINE TypedValue getKey() const {
    assertx(!isInvalid());
    return tv;
  }

  ALWAYS_INLINE hash_t hash() const {
    return tv.hash();
  }

  ALWAYS_INLINE bool isTombstone() const {
    return tv.m_type == kTombstone;
  }

  ALWAYS_INLINE bool isInvalid() const {
    return tv.m_type == kEmpty || isTombstone();
  }

  void erase();

  static constexpr ptrdiff_t keyOff() {
    return offsetof(VanillaKeysetElm, tv) + offsetof(TypedValue, m_data.pstr);
  }
  static constexpr ptrdiff_t dataOff() {
    return offsetof(VanillaKeysetElm, tv) + offsetof(TypedValue, m_data);
  }
  static constexpr ptrdiff_t typeOff() {
    return offsetof(VanillaKeysetElm, tv) + offsetof(TypedValue, m_type);
  }
  static constexpr ptrdiff_t hashOff() {
    return offsetof(VanillaKeysetElm, tv) + offsetof(TypedValue, m_aux);
  }
};

//////////////////////////////////////////////////////////////////////

struct VanillaKeyset final : ArrayData,
                             array::HashTable<VanillaKeyset, VanillaKeysetElm>,
                             type_scan::MarkCollectable<VanillaKeyset> {

//////////////////////////////////////////////////////////////////////
// Set Layout

public:
  void scan(type_scan::Scanner& scanner) const {
    auto const elms = data();
    scanner.scan(*elms, m_used * sizeof(*elms));
  }

//////////////////////////////////////////////////////////////////////
// Initialization, Copies, and Conversions

public:
  /*
   * Allocate a new, empty, request-local set array, with enough space
   * reserved for `size' members.
   *
   * The returned array is already incref'd.
   */
  static ArrayData* MakeReserveSet(uint32_t size);
  static constexpr auto MakeReserve = &MakeReserveSet;

  static ArrayData* MakeSet(uint32_t size, const TypedValue* values);

  /*
   * Allocate an uncounted keyset and copy the values from the
   * input 'array' into the uncounted one.
   *
   * If withApcTypedValue is true, space for an APCTypedValue will be
   * allocated in front of the returned pointer.
   */
  static ArrayData* MakeShared(
      ArrayData* array, const MakeSharedEnv& env, bool hasApcTv);

  static void Release(ArrayData*);
  static void ReleaseShared(ArrayData*);

  /*
   * Safe downcast helpers.
   */
  static VanillaKeyset* asSet(ArrayData* ad);
  static const VanillaKeyset* asSet(const ArrayData* ad);

  static ArrayData* MakeSetFromAPC(const APCArray*);

  /*
   * For array initialization using KeysetInit.
   */
  static ArrayData* AddToSet(ArrayData*, int64_t);
  static ArrayData* AddToSetInPlace(ArrayData*, int64_t);
  static ArrayData* AddToSet(ArrayData*, StringData*);
  static ArrayData* AddToSetInPlace(ArrayData*, StringData*);

private:
  static bool ClearElms(Elm* elms, uint32_t count);

  enum class AllocMode : bool { Request, Static };

  static VanillaKeyset* CopySet(const VanillaKeyset& other, AllocMode);
  VanillaKeyset* copySet() const { return CopySet(*this, AllocMode::Request); }

  void eraseNoCompact(RemovePos pos);

private:
  VanillaKeyset() = delete;
  VanillaKeyset(const VanillaKeyset&) = delete;
  VanillaKeyset& operator=(const VanillaKeyset&) = delete;
  ~VanillaKeyset() = delete;

//////////////////////////////////////////////////////////////////////
// Iteration

private:
  TypedValue getElm(ssize_t ei) const;

public:
  const TypedValue* tvOfPos(uint32_t) const;

  template <class F>
  static void Iterate(const VanillaKeyset* a, F fn) {
    auto const* elm = a->data();
    for (auto i = a->m_used; i--; elm++) {
      if (LIKELY(!elm->isTombstone())) {
        if (ArrayData::call_helper(fn, elm->tv)) break;
      }
    }
  }

//////////////////////////////////////////////////////////////////////
// Sorting

public:
  static ArrayData* EscalateForSort(ArrayData* ad, SortFunction);

  static void Sort(ArrayData*, int, bool);
  static void Ksort(ArrayData*, int, bool);
  static constexpr auto Asort = &Ksort;

  static bool Usort(ArrayData*, const Variant&);
  static bool Uksort(ArrayData*, const Variant&);
  static constexpr auto Uasort = &Uksort;

private:
  template <typename AccessorT>
  SortFlavor preSort(const AccessorT& acc, bool checkTypes);
  void postSort(bool);

//////////////////////////////////////////////////////////////////////
// Set Internals

private:
  bool checkInvariants() const;

  /*
   * These functions require !isFull().  The index of the position
   * where the element was inserted is returned.
   */
  void insert(int64_t k, inthash_t h);
  void insert(int64_t k);
  template <bool Move>
  void insert(StringData* k, strhash_t h);
  template <bool Move>
  void insert(StringData* k);

  /*
   * Returns a copy of the set with twice the scale of the original. It
   * rebuilds the hash table, but it does not compact the elements. If copy is
   * true, it will copy elements instead of taking ownership of them.
   */
  VanillaKeyset* grow(bool copy);

  /*
   * prepareForInsert ensures that the set has room to insert an element and
   * has a refcount of 1, copying if requested and growing if needed.
   */
  VanillaKeyset* prepareForInsert(bool copy);

  /*
   * compact() removes all tombstones from the hash table by going
   * through the inner linked list.
   */
  void compact();

  /*
   * Zombie arrays!
   */
  bool isZombie() const { return m_used + 1 == 0; }
  void setZombie() { m_used = -uint32_t{1}; }

  /*
   * Comparison helper.
   */
  static bool EqualHelper(const ArrayData*, const ArrayData*, bool);

//////////////////////////////////////////////////////////////////////
// Elements

public:

//////////////////////////////////////////////////////////////////////
// JIT Supporting Routines

  static constexpr ptrdiff_t tvOff() {
    return offsetof(Elm, tv);
  }

//////////////////////////////////////////////////////////////////////
// Reference Counting

public:
  using ArrayData::decRefCount;
  using ArrayData::hasMultipleRefs;
  using ArrayData::hasExactlyOneRef;
  using ArrayData::decWillRelease;
  using ArrayData::incRefCount;

//////////////////////////////////////////////////////////////////////
// Misc ArrayData Methods

  /*
   * These using directives ensure the full set of overloaded functions
   * are visible in this class, to avoid triggering implicit conversions
   * from a const Variant& key to int64.
   */
private:
  using ArrayData::exists;
  using ArrayData::at;
  using ArrayData::lval;
  using ArrayData::removeMove;
  using ArrayData::release;

//////////////////////////////////////////////////////////////////////
// Friends

private:
  friend struct array::HashTable<VanillaKeyset, VanillaKeysetElm>;
  friend struct MemoryProfile;
  friend struct jit::ArrayAccessProfile;
  friend struct VanillaDict;
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

//////////////////////////////////////////////////////////////////////
// ArrayData API

public:
  static TypedValue GetPosVal(const ArrayData*, ssize_t);
  static bool PosIsValid(const ArrayData*, ssize_t);
  static bool IsVectorData(const ArrayData*);
  static bool ExistsInt(const ArrayData*, int64_t);
  static bool ExistsStr(const ArrayData*, const StringData*);
  static arr_lval LvalInt(ArrayData*, int64_t);
  static arr_lval LvalStr(ArrayData*, StringData*);
  static ArrayData* SetPosMove(ArrayData*, ssize_t, TypedValue);
  static ArrayData* SetIntMove(ArrayData*, int64_t, TypedValue);
  static ArrayData* SetStrMove(ArrayData*, StringData*, TypedValue);
  static ArrayData* RemoveIntMove(ArrayData*, int64_t);
  static ArrayData* RemoveStrMove(ArrayData*, const StringData*);
  static ArrayData* Copy(const ArrayData*);
  static ArrayData* CopyStatic(const ArrayData*);
  static ArrayData* AppendMove(ArrayData*, TypedValue);
  static ArrayData* AppendIntMove(ArrayData*, int64_t);
  static ArrayData* AppendStrMove(ArrayData*, StringData*);
  static ArrayData* PopMove(ArrayData*, Variant&);
  static void OnSetEvalScalar(ArrayData*);
  static bool Equal(const ArrayData*, const ArrayData*);
  static bool NotEqual(const ArrayData*, const ArrayData*);
  static bool Same(const ArrayData*, const ArrayData*);
  static bool NotSame(const ArrayData*, const ArrayData*);

//////////////////////////////////////////////////////////////////////

private:
  template<class K>
  static ArrayData* RemoveImpl(ArrayData*, K key, bool, VanillaKeysetElm::hash_t);
  static ArrayData* AppendImpl(ArrayData*, TypedValue, bool);

private:
  struct Initializer;
  static Initializer s_initializer;

  uint64_t m_padding;
};

HASH_TABLE_CHECK_OFFSETS(VanillaKeyset, VanillaKeysetElm)
//////////////////////////////////////////////////////////////////////

}
