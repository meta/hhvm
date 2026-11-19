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

#include "hphp/runtime/base/memory-manager.h"
#include "hphp/runtime/base/static-literals.h"
#include "hphp/util/portability.h"
#include "hphp/util/safe-cast.h"

namespace HPHP {

///////////////////////////////////////////////////////////////////////////////

ALWAYS_INLINE ArrayData* staticEmptyVec() {
  auto constexpr ptr = StaticLiterals::EmptyVec();
  return reinterpret_cast<ArrayData*>(ptr);
}

ALWAYS_INLINE ArrayData* staticEmptyMarkedVec() {
  auto constexpr ptr = StaticLiterals::EmptyMarkedVec();
  return reinterpret_cast<ArrayData*>(ptr);
}

ALWAYS_INLINE ArrayData* staticEmptyDictArray() {
  auto constexpr ptr = StaticLiterals::EmptyDict() + sizeof(StrKeyTable);
  return reinterpret_cast<ArrayData*>(ptr);
}

ALWAYS_INLINE ArrayData* staticEmptyMarkedDictArray() {
  auto constexpr ptr = StaticLiterals::EmptyMarkedDict() + sizeof(StrKeyTable);
  return reinterpret_cast<ArrayData*>(ptr);
}

ALWAYS_INLINE ArrayData* staticEmptyKeysetArray() {
  auto constexpr ptr = StaticLiterals::EmptyKeyset();
  return reinterpret_cast<ArrayData*>(ptr);
}

///////////////////////////////////////////////////////////////////////////////
// Creation and destruction.

ALWAYS_INLINE void ArrayData::decRefAndRelease() {
  assertx(kindIsValid());
  if (decReleaseCheck()) release();
}

///////////////////////////////////////////////////////////////////////////////
// ArrayFunction dispatch.

inline void ArrayData::release() DEBUG_NOEXCEPT {
  assertx(!hasMultipleRefs());
  g_array_funcs.release[kind()](this);
  AARCH64_WALKABLE_FRAME();
}

inline void ArrayData::releaseShallow() DEBUG_NOEXCEPT {
  assertx(!hasMultipleRefs());
  tl_heap->objFreeIndex(this, this->sizeIndex());
  AARCH64_WALKABLE_FRAME();
}

///////////////////////////////////////////////////////////////////////////////
// Introspection.

inline size_t ArrayData::size() const {
  return m_size;
}

inline bool ArrayData::empty() const {
  return size() == 0;
}

inline bool ArrayData::kindIsValid() const {
  return isArrayKind(m_kind);
}

inline ArrayData::ArrayKind ArrayData::kind() const {
  assertx(kindIsValid());
  return static_cast<ArrayKind>(m_kind);
}

inline bool ArrayData::isVecType() const {
  return (kind() & ~kBespokeKindMask) == kVecKind;
}
inline bool ArrayData::isDictType() const {
  return (kind() & ~kBespokeKindMask) == kDictKind;
}
inline bool ArrayData::isKeysetType() const {
  return (kind() & ~kBespokeKindMask) == kKeysetKind;
}

inline bool ArrayData::isVanillaVec() const { return kind() == kVecKind; }
inline bool ArrayData::isVanillaDict() const { return kind() == kDictKind; }
inline bool ArrayData::isVanillaKeyset() const { return kind() == kKeysetKind; }

inline bool ArrayData::isVanilla() const {
  return !(kind() & kBespokeKindMask);
}

inline bool ArrayData::bothVanilla(const ArrayData* ad1, const ArrayData* ad2) {
  return !((ad1->kind() | ad2->kind()) & kBespokeKindMask);
}

inline bool ArrayData::hasApcTv() const { return m_aux16 & kHasApcTv; }

inline bool ArrayData::isLegacyArray() const { return m_aux16 & kLegacyArray; }

inline bool ArrayData::hasStrKeyTable() const {
  return m_aux16 & kHasStrKeyTable;
}

inline uint8_t ArrayData::auxBits() const {
  return safe_cast<uint8_t>(m_aux16 &
                            (kLegacyArray | kSampledArray | kMayContainCounted));
}

inline uint8_t ArrayData::sizeIndex() const {
  return safe_cast<uint8_t>(m_aux16 >> 8);
}

inline uint16_t ArrayData::packSizeIndexAndAuxBits(
    uint8_t sizeIndex, uint8_t auxBits) {
  return (static_cast<uint16_t>(sizeIndex) << 8) | auxBits;
}

inline bool ArrayData::isSampledArray() const {
  return m_aux16 & kSampledArray;
}

inline void ArrayData::setSampledArrayInPlace() {
  m_aux16 |= ArrayData::kSampledArray;
}

inline ArrayData* ArrayData::makeSampledStaticArray() const {
  assertx(isStatic());
  auto const result = copyStatic();
  result->m_aux16 |= ArrayData::kSampledArray;
  return result;
}

///////////////////////////////////////////////////////////////////////////////

ALWAYS_INLINE
DataType ArrayData::toDataType() const {
  switch (kind()) {
    case kVecKind:
    case kBespokeVecKind:
      return KindOfVec;

    case kDictKind:
    case kBespokeDictKind:
      return KindOfDict;

    case kKeysetKind:
    case kBespokeKeysetKind:
      return KindOfKeyset;

    case kNumKinds:   not_reached();
  }
  not_reached();
}

///////////////////////////////////////////////////////////////////////////////

inline bool ArrayData::IsValidKey(const StringData* k) {
  return k;
}

///////////////////////////////////////////////////////////////////////////////

ALWAYS_INLINE void decRefArr(ArrayData* arr) {
  arr->decRefAndRelease();
}

//////////////////////////////////////////////////////////////////////////////

}
