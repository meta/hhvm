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
#include "hphp/runtime/base/array-data.h"

#include <array>
#include <fmt/format.h>
#include <tbb/concurrent_unordered_set.h>

#include "hphp/runtime/base/array-common.h"
#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/base/array-iterator.h"
#include "hphp/runtime/base/bespoke-array.h"
#include "hphp/runtime/base/bespoke/type-structure.h"
#include "hphp/runtime/base/comparisons.h"
#include "hphp/runtime/base/memory-manager.h"
#include "hphp/runtime/base/request-info.h"
#include "hphp/runtime/base/vanilla-dict.h"
#include "hphp/runtime/base/vanilla-keyset.h"
#include "hphp/runtime/base/vanilla-vec.h"
#include "hphp/runtime/server/memory-stats.h"
#include "hphp/runtime/vm/reverse-data-map.h"
#include "hphp/runtime/vm/unit-emitter.h"

#include "hphp/util/configs/eval.h"

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

const StaticString
  s_InvalidKeysetOperationMsg{"Invalid operation on keyset"},
  s_VarrayUnsetMsg{"varrays do not support unsetting non-end elements"},
  s_VecUnsetMsg{"Vecs do not support unsetting non-end elements"};
///////////////////////////////////////////////////////////////////////////////

__thread std::pair<const ArrayData*, size_t> s_cachedHash;

namespace {
static_assert(
  sizeof(ArrayData) == 16,
  "Performance is sensitive to sizeof(ArrayData)."
  " Make sure you changed it with good reason and then update this assert.");

size_t hashArrayPortion(const ArrayData* arr) {
  auto hash = folly::hash::hash_128_to_64(arr->kind(), arr->isLegacyArray());

  auto const isBespoke = bespoke::TypeStructure::isBespokeTypeStructure(arr);
  hash = folly::hash::hash_combine(hash, isBespoke);

  IterateKV(
    arr,
    [&](TypedValue k, TypedValue v) {
      assertx(tvIsString(k) || tvIsInt(k));
      assertx(IMPLIES(tvIsString(k), val(k).pstr->isStatic()));
      hash = folly::hash::hash_combine(
        hash,
        k.m_type,
        k.m_data.num,
        v.m_type
      );
      switch (v.m_type) {
        case KindOfNull:
          break;
        case KindOfBoolean:
        case KindOfInt64:
        case KindOfDouble:
        case KindOfString:
        case KindOfVec:
        case KindOfDict:
        case KindOfKeyset:
        case KindOfEnumClassLabel:
          hash = folly::hash::hash_combine(hash, v.m_data.num);
          break;
        case KindOfLazyClass:
          assertx(v.m_data.plazyclass.name()->isStatic());
          hash = folly::hash::hash_combine(
            hash,
            (uintptr_t)v.m_data.plazyclass.name()
          );
          break;
        case KindOfClass:
          assertx(v.m_data.pclass->isPersistent());
          hash = folly::hash::hash_combine(hash, (uintptr_t)v.m_data.pclass);
          break;
        case KindOfFunc:
          assertx(v.m_data.pfunc->isPersistent());
          hash = folly::hash::hash_combine(hash, (uintptr_t)v.m_data.pfunc);
          break;
        case KindOfClsMeth:
          assertx(v.m_data.pclsmeth->isPersistent());
          hash =
            folly::hash::hash_combine(
              hash, (uintptr_t)v.m_data.pclsmeth->getCls());
          hash =
            folly::hash::hash_combine(
                hash, (uintptr_t)v.m_data.pclsmeth->getFunc());
          break;

        case KindOfUninit:
        case KindOfObject:
        case KindOfResource:
        case KindOfRFunc:
        case KindOfRClsMeth:
          always_assert(false);
      }
    }
  );
  return hash;
}

bool compareArrayPortion(const ArrayData* ad1, const ArrayData* ad2) {
  if (ad1 == ad2) return true;
  if (ad1->kind() != ad2->kind()) return false;
  if (ad1->size() != ad2->size()) return false;
  if (ad1->isLegacyArray() != ad2->isLegacyArray()) return false;

  auto const isBespoke1 = bespoke::TypeStructure::isBespokeTypeStructure(ad1);
  auto const isBespoke2 = bespoke::TypeStructure::isBespokeTypeStructure(ad2);
  if (isBespoke1 != isBespoke2) return false;

  auto const check = [] (const TypedValue& tv1, const TypedValue& tv2) {
    if (tv1.m_type != tv2.m_type) {
      // String keys from arrays might be KindOfString, even when
      // the StringData is static.
      if (!isStringType(tv1.m_type) || !isStringType(tv2.m_type)) {
        return false;
      }
      assertx(tv1.m_data.pstr->isStatic());
      assertx(tv2.m_data.pstr->isStatic());
    }
    if (isNullType(tv1.m_type)) return true;
    return tv1.m_data.num == tv2.m_data.num;
  };

  auto equal = true;
  ArrayIter iter2{ad2};
  IterateKV(
    ad1,
    [&](TypedValue k, TypedValue v) {
      if (!check(k, iter2.nvFirst()) || !check(v, iter2.secondVal())) {
        equal = false;
        return true;
      }
      ++iter2;
      return false;
    }
  );
  return equal;
}

struct ScalarHash {
  size_t operator()(ArrayData* arr) const {
    return s_cachedHash.first == arr
      ? s_cachedHash.second
      : hashArrayPortion(arr);
  }
  bool operator()(ArrayData* arr1, ArrayData* arr2) const {
    return compareArrayPortion(arr1, arr2);
  }
};

using ArrayDataMap =
  tbb::concurrent_unordered_set<ArrayData*, ScalarHash, ScalarHash>;
ArrayDataMap s_arrayDataMap;

}

size_t loadedStaticArrayCount() {
  return s_arrayDataMap.size();
}

///////////////////////////////////////////////////////////////////////////////

void ArrayData::GetScalarArray(ArrayData** parr) {
  auto const base = *parr;
  if (base->isStatic()) return;

  auto const arr = [&]{
    if (base->isVanilla()) return base;
    // don't escalate for type structures
    if (bespoke::TypeStructure::isBespokeTypeStructure(base)) return base;
    *parr = BespokeArray::ToVanilla(base, "ArrayData::GetScalarArray");
    decRefArr(base);
    return *parr;
  }();

  auto const replace = [&] (ArrayData* rep) {
    *parr = rep;
    decRefArr(arr);
    s_cachedHash.first = nullptr;
  };

  if (arr->empty()) {
    return replace([&]{
      auto const legacy = arr->isLegacyArray();
      switch (arr->toDataType()) {
        case KindOfVec:
          return legacy ? staticEmptyMarkedVec() : staticEmptyVec();
        case KindOfDict:
          return legacy ? staticEmptyMarkedDictArray() : staticEmptyDictArray();
        case KindOfKeyset:
          return staticEmptyKeysetArray();
        default:
          always_assert(false);
      }
    }());
  }

  auto const isBespokeTS = bespoke::TypeStructure::isBespokeTypeStructure(arr);

  if (isBespokeTS) {
    auto ts = bespoke::TypeStructure::As(arr);
    bespoke::TypeStructure::OnSetEvalScalar(ts);
  } else {
    arr->onSetEvalScalar();
  }

  s_cachedHash.first = arr;
  s_cachedHash.second = hashArrayPortion(arr);

  auto const lookup = [&] (ArrayData* a) -> ArrayData* {
    if (auto const it = s_arrayDataMap.find(a);
        it != s_arrayDataMap.end()) {
      return *it;
    }
    return nullptr;
  };

  if (auto const a = lookup(arr)) return replace(a);

  static std::array<std::mutex, 128> s_mutexes;
  std::lock_guard<std::mutex> g{
    s_mutexes[s_cachedHash.second % s_mutexes.size()]
  };

  if (auto const a = lookup(arr)) return replace(a);

  ArrayData* ad;
  if (isBespokeTS) {
    auto const ts = bespoke::TypeStructure::As(arr);
    ad = bespoke::TypeStructure::CopyStatic(ts);
  } else {
    ad = arr->copyStatic();
  }
  // We should clear the sampled bit in the new static array regardless of
  // whether the input array was sampled, because specializing the input is
  // not sufficient to specialize this new static array.
  ad->m_aux16 &= ~kSampledArray;
  assertx(ad->isStatic());

  // TODO(T68458896): heapSize rounds up to size class, which we shouldn't do.
  MemoryStats::LogAlloc(AllocKind::StaticArray, ad->heapSize());
  if (Cfg::Eval::EnableReverseDataMap) {
    data_map::register_start(ad);
  }

  s_cachedHash.first = ad;
  assertx(hashArrayPortion(ad) == s_cachedHash.second);

  auto const DEBUG_ONLY inserted = s_arrayDataMap.insert(ad).second;
  assertx(inserted);
  return replace(ad);
}

ArrayData* ArrayData::GetScalarArray(Array&& arr) {
  auto a = arr.detach();
  GetScalarArray(&a);
  return a;
}

ArrayData* ArrayData::GetScalarArray(Variant&& arr) {
  assertx(arr.isArray());
  auto a = arr.detach().m_data.parr;
  GetScalarArray(&a);
  return a;
}

//////////////////////////////////////////////////////////////////////

static_assert(ArrayFunctions::NK == ArrayData::ArrayKind::kNumKinds,
              "add new kinds here");

#define DISPATCH(entry)                           \
  { VanillaVec::entry,       /* vanilla vec */    \
    BespokeArray::entry,     /* bespoke vec */    \
    VanillaDict::entry,       /* vanilla dict */   \
    BespokeArray::entry,     /* bespoke dict */   \
    VanillaKeyset::entry,    /* vanilla keyset */ \
    BespokeArray::entry,     /* bespoke keyset */ \
  },

/*
 * "Copy/grow" semantics:
 *
 *   Many of the functions that mutate arrays return an ArrayData* and
 *   take a boolean parameter called 'copy'.  The semantics of these
 *   functions is the following:
 *
 *   If the `copy' argument is false, a COW is not required for
 *   language semantics.  The array may mutate itself in place and
 *   return the same pointer, or the array may allocate a new array
 *   and return that.  This is called "growing", since it may be used
 *   if an array is out of capacity for new elements---but it is also
 *   what happens if an array needs to change kinds and can't do that
 *   in-place.  If an array grows, the old array pointer may not be
 *   used for any more array operations except to eventually call
 *   Release---these grown-from arrays are sometimes described as
 *   being in "zombie" state.
 *
 *   If the `copy' argument is true, the returned array must be
 *   indistinguishable from an array that did a COW, in terms of the
 *   contents of the array.  Whether it is the same pointer value or
 *   not is not specified.  Note, for example, that this means an
 *   array doesn't have to copy if it was asked to remove an element
 *   that doesn't exist.
 *
 *   When a function with these semantics returns a new array, the new array is
 *   already incref'd. In a few cases, an existing array (different than the
 *   source array) may be returned. In this case, the array will already be
 *   incref'd.
 */

const ArrayFunctions g_array_funcs = {
  /*
   * void Release(ArrayData*)
   *
   *   Free the memory associated with a refcounted array. Generally called
   *   when the reference count on the array drops to zero.
   */
  DISPATCH(Release)

  /*
   * void ReleaseShared(ArrayData*)
   *
   *   Free the memory associated with an uncounted array. Generally called
   *   when the reference count on the array drops to "uncounted zero".
   */
  DISPATCH(ReleaseShared)

  /*
   * TypedValue NvGetInt(const ArrayData*, int64_t key)
   * TypedValue NvGetStr(const ArrayData*, const StringData*)
   *
   *   Lookup a value in an array using an int or string key. Returns Uninit if
   *   the key is not in the array. This method does not inc-ref the result.
   */
  DISPATCH(NvGetInt)
  DISPATCH(NvGetStr)

  /*
   * TypedValue GetPosKey(const ArrayData*, ssize_t pos)
   * TypedValue GetPosVal(const ArrayData*, ssize_t pos)
   *
   *   Look up the key or value at a valid iterator position in this array.
   *   Both of these methods return the result without inc-ref-ing it.
   */
  DISPATCH(GetPosKey)
  DISPATCH(GetPosVal)

  /*
   * bool PosIsValid(const ArrayData*, ssize_t pos)
   *
   *   Return true if the given iterator position is valid in this
   *   array.
   */
  DISPATCH(PosIsValid)

  /*
   * ArrayData* SetPosMove(ArrayData*, ssize_t pos, TypedValue v)
   *
   *   Set a value in the array at the given position, which must be valid.
   *   If copy / escalation is necessary, this operation will dec-ref the
   *   array and return a new one; else, it'll return the original array.
   *   It does not inc-ref the value.
   */
  DISPATCH(SetPosMove)

  /*
   * ArrayData* SetIntMove(ArrayData*, int64_t key, TypedValue v)
   *
   *   Set a value in the array for an integer key. If copy / escalation is
   *   necessary, this operation will dec-ref the array and return a new one;
   *   else, it'll return the original array. It does not inc-ref the value.
   */
  DISPATCH(SetIntMove)

  /*
   * ArrayData* SetStrMove(ArrayData*, StringData*, TypedValue v)
   *
   *   Set a value in the array for a string key. If copy / escalation is
   *   necessary, this operation will dec-ref the array and return a new one;
   *   else, it'll return the original array. It does not inc-ref the value.
   *
   *   SetStrMove *does* inc-ref the key if it's newly added to the array.
   */
  DISPATCH(SetStrMove)

  /*
   * bool IsVectorData(const ArrayData*)
   *
   *   Return true if this array is empty, or if it has only contiguous integer
   *   keys and the first key is zero.  Determining this may be an O(N)
   *   operation.
   */
  DISPATCH(IsVectorData)

  /*
   * bool ExistsInt(const ArrayData*, int64_t key)
   *
   *   Return true iff this array contains an element with the supplied integer
   *   key.
   */
  DISPATCH(ExistsInt)

  /*
   * bool ExistsStr(const ArrayData*, const StringData*)
   *
   *   Return true iff this array contains an element with the supplied string
   *   key.  The string will not undergo intish-key cast.
   */
  DISPATCH(ExistsStr)

  /*
   * arr_lval LvalInt(ArrayData*, int64_t k)
   * arr_lval LvalStr(ArrayData*, StringData* key)
   *
   *   Look up a value in the array by the supplied key, throwing if it doesn't
   *   exist, and return a reference to it.  This function has copy/grow
   *   semantics.
   */
  DISPATCH(LvalInt)
  DISPATCH(LvalStr)

  /*
   * ArrayData* RemoveIntMove(ArrayData*, int64_t key)
   *
   *   Remove an array element with an integer key, copying or escalating
   *   as necessary. If there was no entry for that element, this function
   *   will not remove it, but it may still copy the array in that case. If
   *   a copy or escalation was required, there will be a dec-ref of the old
   *   array.
   */
  DISPATCH(RemoveIntMove)

  /*
   * ArrayData* RemoveStr(ArrayData*, const StringData*)
   *
   *   Remove an array element with a string key, copying or escalating
   *   as necessary. If there was no entry for that element, this function
   *   will not remove it, but it may still copy the array in that case. If
   *   a copy or escalation was required, there will be a dec-ref of the old
   *   array.
   */
  DISPATCH(RemoveStrMove)

  /*
   * ssize_t IterEnd(const ArrayData*)
   *
   *   Returns the canonical invalid position for this array.  Note
   *   that if elements are added or removed from the array, the value
   *   of the array's canonical invalid position may change.
   *
   * ssize_t IterBegin(const ArrayData*)
   *
   *   Returns the position of the first element, or the canonical
   *   invalid position if this array is empty.
   *
   * ssize_t IterLast(const ArrayData*)
   *
   *   Returns the position of the last element, or the canonical
   *   invalid position if this array is empty.
   */
  DISPATCH(IterBegin)
  DISPATCH(IterLast)
  DISPATCH(IterEnd)

  /*
   * ssize_t IterAdvance(const ArrayData*, size_t pos)
   *
   *   Returns the position of the element that comes after pos, or the
   *   canonical invalid position if there are no more elements after pos.
   *   If pos is the canonical invalid position, this method will return
   *   the canonical invalid position.
   */
  DISPATCH(IterAdvance)

  /*
   * ssize_t IterRewind(const ArrayData*, size_t pos)
   *
   *   Returns the position of the element that comes before pos, or the
   *   canonical invalid position if there are no elements before pos. If
   *   pos is the canonical invalid position, no guarantees are made about
   *   what this method returns.
   */
  DISPATCH(IterRewind)

  /*
   * ArrayData* EscalateForSort(ArrayData*, SortFunction)
   *
   *   Must be called before calling any of the sort routines on an
   *   array. This gives arrays a chance to change to a kind that
   *   supports sorting.
   */
  DISPATCH(EscalateForSort)

  /*
   * void Ksort(ArrayData*, int sort_flags, bool ascending)
   *
   *   Sort an array by its keys, keeping the values associated with
   *   their respective keys.
   */
  DISPATCH(Ksort)

  /*
   * void Sort(ArrayData*, int sort_flags, bool ascending)
   *
   *   Sort an array, by values, and then assign new keys to the
   *   elements in the resulting array.
   */
  DISPATCH(Sort)

  /*
   * void Asort(ArrayData*, int sort_flags, bool ascending)
   *
   *   Sort an array and maintain index association.  This means sort
   *   the array by values, but keep the keys associated with the
   *   values they used to be associated with.
   */
  DISPATCH(Asort)

  /*
   * bool Uksort(ArrayData*, const Variant&)
   *
   *   Sort on keys with a user-defined compare function (in the
   *   variant argument).  Returns false if the user comparison
   *   function modifies the array we are sorting.
   */
  DISPATCH(Uksort)

  /*
   * bool Usort(ArrayData*, const Variant&)
   *
   *   Sort the array by values with a user-defined comparison
   *   function (in the variant).  Returns false if the user-defined
   *   comparison function modifies the array we are sorting.
   */
  DISPATCH(Usort)

  /*
   * bool Uasort(ArrayData*, const Variant&)
   *
   *   Sort array by values with a user-defined comparison function
   *   (in the variant arg), keeping the original indexes associated
   *   with the values.  Returns false if the user-defined comparison
   *   function modifies the array we are sorting.
   */
  DISPATCH(Uasort)

  /*
   * ArrayData* CopyStatic(const ArrayData*)
   *
   *   Copy an array, allocating the new array with malloc() instead
   *   of from the request local allocator.  This function does
   *   guarantee the returned array is a new copy, but it may throw a
   *   fatal error if this cannot be accomplished.
   */
  DISPATCH(CopyStatic)

  /*
   * ArrayData* Append(ArrayData*, TypedValue v);
   *
   *   Append a new value to the array, with the next available integer key.
   *   If copy / escalation is necessary, this operation will dec-ref the
   *   array and return a new one; else, it'll return the original array.
   *   This operation does not inc-ref the value.
   */
  DISPATCH(AppendMove)

  /*
   * ArrayData* PopMove(ArrayData*, Variant& value);
   *
   *   Remove the last element from the array and assign it to `value'.
   *   If copy / escalation is necessary, this operation will dec-ref the
   *   array and return a new one; else, it'll return the original array.
   */
  DISPATCH(PopMove)

  /*
   * void OnSetEvalScalar(ArrayData*)
   *
   *   Go through an array and call Variant::setEvalScalar on each
   *   value, and make all string keys into static strings.
   */
  DISPATCH(OnSetEvalScalar)

  /*
   * void MakeShared(ArrayData*, DataWalker::PointerMap*, bool hasApcTv)
   *
   *   Return a copy of the array where all its contents are converted to
   *   uncounted values. The caller must confirm that its contents can all be
   *   converted, e.g. by using DataWalker. If hasApcTv is true, this method
   *   will colocate an ApcTypedValue right before the array.
   */
  DISPATCH(MakeShared)

  /*
   * ArrayData* Copy(ArrayData*)
   *
   *  Return a counted copy of the array. A copy is made regardless of
   *  the array's ref-count.
   */
  DISPATCH(Copy)
};

#undef DISPATCH

///////////////////////////////////////////////////////////////////////////////

namespace {

DEBUG_ONLY void assertForCreate(TypedValue name) {
  always_assert(isIntType(name.m_type) || isStringType(name.m_type));
}

}

// In general, arrays can contain int-valued-strings, even though plain array
// access converts them to integers.  non-int-string assertions should go
// upstream of the ArrayData api.

bool ArrayData::IsValidKey(TypedValue k) {
  return isIntType(k.m_type) ||
        (isStringType(k.m_type) && IsValidKey(k.m_data.pstr));
}

bool ArrayData::IsValidKey(const Variant& k) {
  return IsValidKey(*k.asTypedValue());
}

bool ArrayData::IsValidKey(const OptString& k) {
  return IsValidKey(k.get());
}

///////////////////////////////////////////////////////////////////////////////
// reads

ALWAYS_INLINE
bool ArrayData::EqualHelper(const ArrayData* ad1, const ArrayData* ad2,
                            bool strict) {
  if (ad1 == ad2) return true;
  if (ad1->size() != ad2->size()) return false;

  // Prevent circular referenced objects/arrays or deep ones.
  check_recursion_error();

  if (strict) {
    for (ArrayIter iter1{ad1}, iter2{ad2}; iter1; ++iter1, ++iter2) {
      assertx(iter2);
      if (!HPHP::same(iter1.first(), iter2.first()) ||
          !tvSame(iter1.secondVal(), iter2.secondVal())) {
        return false;
      }
    }
    return true;
  } else {
    bool equal = true;
    IterateKV(
      ad1,
      [&](TypedValue k, TypedValue v) {
        auto const v2 = ad2->get(k);
        if (!v2.is_init() || !tvEqual(v, v2)) {
          equal = false;
          return true;
        }
        return false;
      }
    );
    return equal;
  }
}

ALWAYS_INLINE
int64_t ArrayData::CompareHelper(const ArrayData* ad1, const ArrayData* ad2) {
  auto const size1 = ad1->size();
  auto const size2 = ad2->size();
  if (size1 < size2) return -1;
  if (size1 > size2) return 1;

  // Prevent circular referenced objects/arrays or deep ones.
  check_recursion_error();

  int result = 0;
  IterateKV(
    ad1,
    [&](TypedValue k, TypedValue v) {
      auto const v2 = ad2->get(k);
      if (!v2.is_init()) {
        result = 1;
        return true;
      }
      auto const cmp = tvCompare(v, v2);
      if (cmp != 0) {
        result = cmp;
        return true;
      }
      return false;
    }
  );

  return result;
}

bool ArrayData::Equal(const ArrayData* ad1, const ArrayData* ad2) {
  return EqualHelper(ad1, ad2, false);
}

bool ArrayData::NotEqual(const ArrayData* ad1, const ArrayData* ad2) {
  return !EqualHelper(ad1, ad2, false);
}

bool ArrayData::Same(const ArrayData* ad1, const ArrayData* ad2) {
  return EqualHelper(ad1, ad2, true);
}

bool ArrayData::NotSame(const ArrayData* ad1, const ArrayData* ad2) {
  return !EqualHelper(ad1, ad2, true);
}

bool ArrayData::Lt(const ArrayData* ad1, const ArrayData* ad2) {
  return CompareHelper(ad1, ad2) < 0;
}

bool ArrayData::Lte(const ArrayData* ad1, const ArrayData* ad2) {
  return CompareHelper(ad1, ad2) <= 0;
}

bool ArrayData::Gt(const ArrayData* ad1, const ArrayData* ad2) {
  return CompareHelper(ad1, ad2) > 0;
}

bool ArrayData::Gte(const ArrayData* ad1, const ArrayData* ad2) {
  return CompareHelper(ad1, ad2) >= 0;
}

int64_t ArrayData::Compare(const ArrayData* ad1, const ArrayData* ad2) {
  return CompareHelper(ad1, ad2);
}

bool ArrayData::same(const ArrayData* v2) const {
  assertx(v2);

  if (toDataType() != v2->toDataType()) return false;

  if (!bothVanilla(this, v2)) return Same(this, v2);
  if (isVanillaVec())  return VanillaVec::VecSame(this, v2);
  if (isVanillaDict()) return VanillaDict::DictSame(this, v2);
  return VanillaKeyset::Same(this, v2);
}

///////////////////////////////////////////////////////////////////////////////
// helpers

void ArrayData::getNotFound(int64_t k) const {
  throwOOBArrayKeyException(k, this);
}

void ArrayData::getNotFound(const StringData* k) const {
  if (isVecType()) throwInvalidArrayKeyException(k, this);
  throwOOBArrayKeyException(k, this);
}

const char* ArrayData::kindToString(ArrayKind kind) {
  std::array<const char*, 6> names = {{
    "VecKind",
    "BespokeVecKind",
    "DictKind",
    "BespokeDictKind",
    "KeysetKind",
    "BespokeKeysetKind",
  }};
  static_assert(names.size() == kNumKinds, "add new kinds here");
  return names[kind];
}

namespace {

////////////////////////////////////////////////////////////////////////////////

std::string describeKeyValue(TypedValue tv) {
  switch (tv.m_type) {
  case KindOfString:
    return fmt::format("\"{}\"", tv.m_data.pstr->data());
  case KindOfInt64:
    return folly::to<std::string>(tv.m_data.num);
  case KindOfUninit:
  case KindOfNull:
  case KindOfBoolean:
  case KindOfDouble:
  case KindOfVec:
  case KindOfDict:
  case KindOfKeyset:
  case KindOfResource:
  case KindOfObject:
  case KindOfRFunc:
  case KindOfFunc:
  case KindOfClass:
  case KindOfLazyClass:
  case KindOfClsMeth:
  case KindOfRClsMeth:
  case KindOfEnumClassLabel:
    return "<invalid key type>";
  }
  not_reached();
}

////////////////////////////////////////////////////////////////////////////////

}

void throwInvalidArrayKeyException(const TypedValue* key, const ArrayData* ad) {
  std::pair<const char*, const char*> kind_type = [&]{
    if (ad->isVecType()) return std::make_pair("vec", "int");
    if (ad->isDictType()) return std::make_pair("dict", "int or string");
    assertx(ad->isKeysetType());
    return std::make_pair("keyset", "int or string");
  }();
  SystemLib::throwInvalidArgumentExceptionObject(
    fmt::format(
      "Invalid {} key: expected a key of type {}, {} given",
      kind_type.first, kind_type.second, describe_actual_type(key)
    )
  );
}

void throwInvalidArrayKeyException(const StringData* key, const ArrayData* ad) {
  auto const tv = make_tv<KindOfString>(const_cast<StringData*>(key));
  throwInvalidArrayKeyException(&tv, ad);
}

void throwFalseyPromoteException(const char* type) {
  SystemLib::throwOutOfBoundsExceptionObject(
    fmt::format("Promoting {} to array", type)
  );
}

void throwOOBArrayKeyException(TypedValue key, const ArrayData* ad) {
  SystemLib::throwOutOfBoundsExceptionObject(
    fmt::format(
      "Out of bounds {} access: invalid index {}",
      getDataTypeString(ad->toDataType(), ad->isLegacyArray()).data(),
      describeKeyValue(key)
    )
  );
}

void throwOOBArrayKeyException(int64_t key, const ArrayData* ad) {
  throwOOBArrayKeyException(make_tv<KindOfInt64>(key), ad);
}

void throwOOBArrayKeyException(const StringData* key, const ArrayData* ad) {
  throwOOBArrayKeyException(
    make_tv<KindOfString>(const_cast<StringData*>(key)),
    ad
  );
}

void throwInvalidKeysetOperation() {
  SystemLib::throwInvalidOperationExceptionObject(s_InvalidKeysetOperationMsg);
}

void throwVarrayUnsetException() {
  SystemLib::throwInvalidOperationExceptionObject(s_VarrayUnsetMsg);
}

void throwVecUnsetException() {
  SystemLib::throwInvalidOperationExceptionObject(s_VecUnsetMsg);
}

///////////////////////////////////////////////////////////////////////////////

ArrayData* ArrayData::toDictIntishCast(bool copy) {
  auto const base = toDict(copy);
  if (isVecType()) return base;

  // Check if we need to intish-cast any string keys.
  int64_t i;
  auto intish = false;
  IterateKV(base, [&](TypedValue k, TypedValue) {
    return intish |= tvIsString(k) && val(k).pstr->isStrictlyInteger(i);
  });
  if (!intish) return base;

  // Create a new dict with the casted keys.
  auto result = DictInit(base->size());
  IterateKV(base, [&](TypedValue k, TypedValue v) {
    result.setUnknownKey<IntishCast::Cast>(k, tvAsCVarRef(&v));
  });
  if (base != this) decRefArr(base);
  auto ad = result.create();
  if (!isKeysetType()) return ad;

  // When keysets are intish-casted, we cast the values, too. Do so in place.
  // This case is rare, so we keep the extra checks out of the loop above.
  assertx(ad->hasExactlyOneRef());
  auto elm = VanillaDict::as(ad)->data();
  for (auto const end = elm + ad->size(); elm != end; elm++) {
    if (tvIsString(elm->data) && elm->hasIntKey()) {
      decRefStr(val(elm->data).pstr);
      tvCopy(make_tv<KindOfInt64>(elm->intKey()), elm->data);
    }
  }
  return ad;
}

bool ArrayData::IntishCastKey(const StringData* key, int64_t& i) {
  return key->isStrictlyInteger(i);
}

ArrayData* ArrayData::setLegacyArray(bool copy, bool legacy) {
  assertx(IMPLIES(cowCheck(), copy));
  assertx(isDictType() || isVecType());

  if (legacy == isLegacyArray()) return this;

  if (!isVanilla()) return BespokeArray::SetLegacyArray(this, copy, legacy);

  auto const ad = [&]{
    if (!copy) return this;
    return isVanillaVec() ? VanillaVec::Copy(this) : VanillaDict::Copy(this);
  }();
  ad->setLegacyArrayInPlace(legacy);
  return ad;
}

void ArrayData::setLegacyArrayInPlace(bool legacy) {
  assertx(hasExactlyOneRef());
  if (legacy) {
    m_aux16 |= kLegacyArray;
  } else {
    m_aux16 &= ~kLegacyArray;
  }
}

size_t ArrayData::heapSize() const {
  return kSizeIndex2Size[sizeIndex()];
}

///////////////////////////////////////////////////////////////////////////////

ArrayData* ArrayData::toVec(bool copy) {
  assertx(IMPLIES(cowCheck(), copy));
  if (isVecType()) return this;

  if (empty()) return staticEmptyVec();
  VecInit init{size()};
  IterateV(this, [&](auto v) { init.append(v); });
  return init.create();
}

ArrayData* ArrayData::toDict(bool copy) {
  assertx(IMPLIES(cowCheck(), copy));
  if (isDictType()) return this;

  if (empty()) return staticEmptyDictArray();
  DictInit init{size()};
  IterateKV(this, [&](auto k, auto v) { init.setValidKey(k, v); });
  return init.create();
}

ArrayData* ArrayData::toKeyset(bool copy) {
  assertx(IMPLIES(cowCheck(), copy));
  if (isKeysetType()) return this;
  if (empty()) return staticEmptyKeysetArray();
  KeysetInit init{size()};
  IterateV(this, [&](auto v) { init.add(v); });
  return init.create();
}


void* ArrayData::AllocStatic(size_t size) {
  return Cfg::Eval::LowStaticArrays ? low_malloc(size) : shared_malloc(size);
}

void ArrayData::FreeStatic(void* ptr) {
  Cfg::Eval::LowStaticArrays ? low_free(ptr) : shared_free(ptr);
}

///////////////////////////////////////////////////////////////////////////////
// Serialization

__thread UnitEmitter* BlobEncoderHelper<const ArrayData*>::tl_unitEmitter{nullptr};
__thread Unit* BlobEncoderHelper<const ArrayData*>::tl_unit{nullptr};
__thread bool BlobEncoderHelper<const ArrayData*>::tl_defer{false};

namespace {

enum class ArrayKind {
  Null = 0,
  Vec,
  Dict,
  Keyset,
  BespokeTS,
  VArray,
  DArray
};

}

void BlobEncoderHelper<const ArrayData*>::serde(BlobEncoder& encoder,
                                                const ArrayData* ad) {
  assertx(!tl_unit);

  if (auto const ue = tl_unitEmitter; ue && !tl_defer) {
    if (!ad) {
      encoder(Id{0});
      return;
    }
    Id id = ue->mergeArray(ad);
    assertx(id != std::numeric_limits<Id>::max());
    encoder(id+1);
    return;
  }

  auto const oldDefer = tl_defer;
  tl_defer = false;
  SCOPE_EXIT {
    assertx(!tl_defer);
    tl_defer = oldDefer;
  };

  using AK = ArrayKind;
  auto const ak = [&] {
    if (!ad) return AK::Null;
    if (ad->isLegacyArray()) {
      return ad->isVecType() ? AK::VArray : AK::DArray;
    }
    if (ad->isDictType()) {
      return bespoke::TypeStructure::isBespokeTypeStructure(ad)
        ? AK::BespokeTS
        : AK::Dict;
    }
    if (ad->isVecType()) return AK::Vec;
    assertx(ad->isKeysetType());
    return AK::Keyset;
  }();

  encoder(ak);
  if (ak == AK::Null) return;

  assertx(ad->size() <= std::numeric_limits<uint32_t>::max());
  encoder(static_cast<uint32_t>(ad->size()));

  switch (ak) {
    case AK::Vec:
    case AK::VArray:
    case AK::Keyset:
      IterateV(ad, [&] (TypedValue v) { encoder(v); });
      break;
    case AK::Dict:
    case AK::DArray:
    case AK::BespokeTS:
      IterateKV(
        ad,
        [&] (TypedValue k, TypedValue v) {
          assertx(tvIsInt(k) || tvIsString(k));
          encoder(k)(v);
        }
      );
      break;
    case AK::Null:
      not_reached();
  }
}

void BlobEncoderHelper<const ArrayData*>::serde(BlobDecoder& decoder,
                                                const ArrayData*& ad,
                                                bool makeStatic) {
  if (auto const ue = tl_unitEmitter; ue && !tl_defer) {
    Id id;
    decoder(id);
    if (!id) {
      ad = nullptr;
      return;
    }
    if (makeStatic) {
      ad = ue->lookupArrayId(id-1);
      assertx(ad->isStatic());
    } else {
      ad = ue->lookupArrayIdCopy(id-1).detach();
    }
    return;
  } else if (auto const u = tl_unit; u && !tl_defer) {
    assertx(makeStatic);
    Id id;
    decoder(id);
    if (!id) {
      ad = nullptr;
      return;
    }
    ad = u->lookupArrayId(id-1);
    assertx(ad->isStatic());
    return;
  }

  auto const oldDefer = tl_defer;
  tl_defer = false;
  SCOPE_EXIT {
    assertx(!tl_defer);
    tl_defer = oldDefer;
  };

  using AK = ArrayKind;

  AK ak;
  decoder(ak);
  if (ak == AK::Null) {
    ad = nullptr;
    return;
  }

  uint32_t size;
  decoder(size);

  switch (ak) {
    case AK::Vec:
    case AK::VArray: {
      VecInit v{size};
      for (size_t i = 0; i < size; ++i) {
        TypedValue tv;
        decoder(tv, makeStatic);
        v.append(Variant::attach(tv));
      }
      if (ak == AK::VArray) v.setLegacyArray(true);
      auto a = v.toArray();
      if (makeStatic) a.setEvalScalar();
      ad = a.detach();
      break;
    }
    case AK::Keyset: {
      KeysetInit k{size};
      for (size_t i = 0; i < size; ++i) {
        TypedValue tv;
        decoder(tv, makeStatic);
        k.add(Variant::attach(tv));
      }
      auto a = k.toArray();
      if (makeStatic) a.setEvalScalar();
      ad = a.detach();
      break;
    }
    case AK::Dict:
    case AK::DArray:
    case AK::BespokeTS: {
      DictInit d{size};
      for (size_t i = 0; i < size; ++i) {
        TypedValue k;
        TypedValue v;
        decoder(k, makeStatic)(v, makeStatic);
        if (tvIsString(k)) {
          d.set(OptString::attach(k.m_data.pstr), Variant::attach(v));
        } else {
          assertx(tvIsInt(k));
          d.set(k.m_data.num, Variant::attach(v));
        }
      }
      if (ak == AK::DArray) d.setLegacyArray();
      auto a = d.toArray();
      if (ak == AK::BespokeTS) {
        auto const ts =
          bespoke::TypeStructure::MakeFromVanilla(a.get());
        if (ts) a = Array::attach(ts);
      }
      if (makeStatic) a.setEvalScalar();
      ad = a.detach();
      break;
    }
    case AK::Null:
      not_reached();
  }
}

///////////////////////////////////////////////////////////////////////////////

}
