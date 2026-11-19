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

#include "hphp/runtime/base/vanilla-vec.h"

#include "hphp/runtime/base/array-data.h"
#include "hphp/runtime/base/hash-table.h"
#include "hphp/runtime/base/memory-manager.h"
#include "hphp/runtime/base/typed-value.h"
#include "hphp/runtime/base/unaligned-typed-value.h"

#include "hphp/util/type-scan.h"

namespace HPHP {

//////////////////////////////////////////////////////////////////////

constexpr size_t bytes2VanillaVecCapacity(size_t bytes) {
  return bytes / sizeof(UnalignedTypedValue);
}

// This method is just to help compilers with types in the macro usage below.
// It also checks that the result of the arithmetic above fits in a uint32_t;
// if this check fails, it probably means that MaxSizeIndex must be adjusted.
constexpr uint32_t sizeClassParams2VanillaVecCapacity(
  size_t index,
  size_t lg_grp,
  size_t lg_delta,
  size_t ndelta
) {
  static_assert(sizeof(ArrayData) <= kSizeIndex2Size[0],
    "This math only works if ArrayData fits in the smallest size class.");
  if (index > VanillaVec::MaxSizeIndex) return 0;
  const size_t total = ((size_t{1} << lg_grp) + (ndelta << lg_delta));
  const size_t capacity = bytes2VanillaVecCapacity(total - sizeof(ArrayData));
  return capacity <= std::numeric_limits<uint32_t>::max()
    ? capacity
    : throw std::length_error("capacity > uint32_t::max");
}

// We can use uint32_t safely because capacity is in units of array elements,
// and an ArrayData's m_size field is a uint32_t. See also the guard above.
alignas(64) constexpr uint32_t kSizeIndex2VanillaVecCapacity[] = {
#define SIZE_CLASS(index, lg_grp, lg_delta, ndelta, lg_delta_lookup, ncontig) \
  sizeClassParams2VanillaVecCapacity(index, lg_grp, lg_delta, ndelta),
  SIZE_CLASSES
#undef SIZE_CLASS
};

// This assertion lets us allocate by index and assume we know the capacity.
static_assert(
  kSizeIndex2VanillaVecCapacity[VanillaVec::SmallSizeIndex]
    == VanillaVec::SmallSize,
  "SmallSizeIndex must be MM size class index for array of SmallSize"
);

//////////////////////////////////////////////////////////////////////

ALWAYS_INLINE UnalignedTypedValue* VanillaVec::entries(ArrayData* arr) {
  return reinterpret_cast<UnalignedTypedValue*>(arr + 1);
}

ALWAYS_INLINE
const UnalignedTypedValue* VanillaVec::entries(const ArrayData* arr) {
  return reinterpret_cast<const UnalignedTypedValue*>(arr + 1);
}

ALWAYS_INLINE ptrdiff_t VanillaVec::entriesOffset() {
  return sizeof(ArrayData);
}

ALWAYS_INLINE
size_t VanillaVec::capacityToSizeBytes(size_t capacity) {
  // We round up to a multiple of 16 since our allocations are aligned.
  auto const size = sizeof(ArrayData) + capacity * sizeof(UnalignedTypedValue);
  return (size + 15) & size_t(-16);
}

ALWAYS_INLINE
size_t VanillaVec::capacityToSizeIndex(size_t capacity) {
  if (capacity <= VanillaVec::SmallSize) {
    return VanillaVec::SmallSizeIndex;
  }
  const auto index = MemoryManager::size2Index(capacityToSizeBytes(capacity));
  assertx(index <= VanillaVec::MaxSizeIndex);
  return index;
}

ALWAYS_INLINE
bool VanillaVec::checkCapacity(size_t capacity) {
  if (capacity <= VanillaVec::SmallSize) {
    return true;
  }
  const auto index = MemoryManager::size2Index(capacityToSizeBytes(capacity));
  return index <= VanillaVec::MaxSizeIndex;
}

ALWAYS_INLINE
uint32_t VanillaVec::capacity(const ArrayData* ad) {
  return kSizeIndex2VanillaVecCapacity[ad->sizeIndex()];
}

ALWAYS_INLINE
uint16_t VanillaVec::packSizeIndexAndAuxBits(uint8_t idx, uint8_t aux) {
  return ArrayData::packSizeIndexAndAuxBits(idx, aux);
}

inline void VanillaVec::scan(const ArrayData* a, type_scan::Scanner& scanner) {
  assertx(checkInvariants(a));
  for (uint32_t i = 0; i < a->size(); ++i) {
    const auto lval = LvalUncheckedInt(const_cast<ArrayData*>(a), i);
    if (isRefcountedType(type(lval)) && val(lval).pcnt->isRefCounted()) {
      scanner.scan(val(lval).pcnt);
    }
  }
}

template <class F>
void VanillaVec::IterateV(const ArrayData* arr, F fn) {
  assertx(checkInvariants(arr));
  auto const size = arr->m_size;
  for (uint32_t i = 0; i < size; ++i) {
    if (ArrayData::call_helper(fn, GetPosVal(arr, i))) break;
  }
}

template <class F>
void VanillaVec::IterateKV(const ArrayData* arr, F fn) {
  assertx(checkInvariants(arr));
  auto const size = arr->m_size;
  for (auto k = make_tv<KindOfInt64>(0); val(k).num < size; ++val(k).num) {
    if (ArrayData::call_helper(fn, k, GetPosVal(arr, val(k).num))) break;
  }
}

//////////////////////////////////////////////////////////////////////

}
