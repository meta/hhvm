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

#include "hphp/runtime/base/vanilla-dict.h"

#include "hphp/runtime/base/apc-array.h"
#include "hphp/runtime/base/array-data.h"
#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/base/comparisons.h"
#include "hphp/runtime/base/runtime-error.h"
#include "hphp/runtime/base/str-key-table.h"
#include "hphp/runtime/base/tv-comparisons.h"
#include "hphp/runtime/base/tv-refcount.h"
#include "hphp/runtime/base/tv-shared.h"
#include "hphp/runtime/base/tv-val.h"

#include "hphp/util/configs/eval.h"
#include "hphp/util/hash.h"
#include "hphp/util/trace.h"


#include <algorithm>

#include "hphp/runtime/base/vanilla-dict-defs.h"

namespace HPHP {

TRACE_SET_MOD(runtime)

struct VanillaDict::DictInitializer {
  DictInitializer() {
    static_assert(
      computeAllocBytes(SmallScale) + sizeof(StrKeyTable) ==
        StaticLiterals::kEmptyDictSize
    );
    auto constexpr scale = 1;
    auto const index = computeIndexFromScale(scale);
    auto const ad    = reinterpret_cast<VanillaDict*>(staticEmptyDictArray());
    auto const flags = kHasStrKeyTable;
    auto const aux   = packSizeIndexAndAuxBits(index, flags);
    ad->initHash(scale);
    ad->m_size         = 0;
    ad->m_layout_index = kVanillaLayoutIndex;
    ad->m_scale_used   = scale;
    ad->m_nextKI       = 0;
    ad->initHeader_16(HeaderKind::Dict, StaticValue, aux);
    *ad->mutableKeyTypes() = ArrayKeyTypes::Empty();
    ad->mutableStrKeyTable()->reset();
    assertx(ad->checkInvariants());
  }
};
VanillaDict::DictInitializer VanillaDict::s_dict_initializer;

struct VanillaDict::MarkedDictArrayInitializer {
  MarkedDictArrayInitializer() {
    static_assert(
      computeAllocBytes(SmallScale) + sizeof(StrKeyTable) ==
        StaticLiterals::kEmptyDictSize
    );
    auto constexpr scale = 1;
    auto const index = computeIndexFromScale(scale);
    auto const ad    =
      reinterpret_cast<VanillaDict*>(staticEmptyMarkedDictArray());
    auto const flags = kLegacyArray | kHasStrKeyTable;
    auto const aux   = packSizeIndexAndAuxBits(index, flags);
    ad->initHash(scale);
    ad->m_size          = 0;
    ad->m_layout_index  = kVanillaLayoutIndex;
    ad->m_scale_used    = scale;
    ad->m_nextKI        = 0;
    ad->initHeader_16(HeaderKind::Dict, StaticValue, aux);
    *ad->mutableKeyTypes() = ArrayKeyTypes::Empty();
    ad->mutableStrKeyTable()->reset();
    assertx(ad->checkInvariants());
  }
};
VanillaDict::MarkedDictArrayInitializer VanillaDict::s_marked_dict_initializer;

//////////////////////////////////////////////////////////////////////

ArrayData* VanillaDict::MakeReserveDict(uint32_t size) {
  auto const scale = computeScaleFromSize(size);
  auto const index = computeIndexFromScale(scale);
  auto const ad    = reqAllocIndex(index);

  // Intialize the hash table first, because the header is already in L1 cache,
  // but the hash table may not be.  So let's issue the cache request ASAP.
  ad->initHash(scale);

  auto const aux = packSizeIndexAndAuxBits(index, 0);
  ad->initHeader_16(HeaderKind::Dict, OneReference, aux);
  *ad->mutableKeyTypes() = ArrayKeyTypes::Empty();
  ad->m_size          = 0;
  ad->m_layout_index  = kVanillaLayoutIndex;
  ad->m_scale_used    = scale; // used=0
  ad->m_nextKI        = 0;

  assertx(ad->m_size == 0);
  assertx(ad->hasExactlyOneRef());
  assertx(ad->m_used == 0);
  assertx(ad->m_nextKI == 0);
  assertx(ad->m_scale == scale);
  assertx(ad->checkInvariants());
  return ad;
}

VanillaDict* VanillaDict::MakeStructDict(uint32_t size,
                                       const StringData* const* keys,
                                       const TypedValue* values) {
  assertx(size > 0);
  auto const scale = computeScaleFromSize(size);
  auto const index = computeIndexFromScale(scale);
  auto const ad    = reqAllocIndex(index);
  auto const aux   = packSizeIndexAndAuxBits(index, 0);

  ad->initHeader_16(HeaderKind::Dict, OneReference, aux);
  *ad->mutableKeyTypes() = ArrayKeyTypes::StaticStrs();
  ad->m_size             = size;
  ad->m_layout_index     = kVanillaLayoutIndex;
  ad->m_scale_used       = scale | uint64_t{size} << 32; // used=size
  ad->m_nextKI           = 0;

  auto const table = ad->initHash(scale);
  auto const mask = ad->mask();
  auto const data = ad->data();

  // Append values by moving -- Caller assumes we update refcount.
  // Values are in reverse order since they come from the stack, which
  // grows down.
  for (uint32_t i = 0; i < size; i++) {
    assertx(keys[i]->isStatic());
    auto k = keys[i];
    auto h = k->hashStatic();
    data[i].setStaticKey(const_cast<StringData*>(k), h);
    const auto& tv = values[size - i - 1];
    data[i].data.m_data = tv.m_data;
    data[i].data.m_type = tv.m_type;
    auto ei = ad->findForNewInsert(table, mask, h);
    *ei = i;
  }

  assertx(ad->m_size == size);
  assertx(ad->m_scale == scale);
  assertx(ad->hasExactlyOneRef());
  assertx(ad->m_used == size);
  assertx(ad->m_nextKI == 0);
  assertx(ad->checkInvariants());
  return ad;
}

VanillaDict* VanillaDict::AllocStructDict(uint32_t size, const int32_t* hash) {
  assertx(size > 0);
  auto const scale = computeScaleFromSize(size);
  auto const index = computeIndexFromScale(scale);
  auto const ad    = reqAllocIndex(index);
  auto const aux   = packSizeIndexAndAuxBits(index, 0);

  ad->initHeader_16(HeaderKind::Dict, OneReference, aux);
  *ad->mutableKeyTypes() = ArrayKeyTypes::StaticStrs();
  ad->m_size             = size;
  ad->m_layout_index     = kVanillaLayoutIndex;
  ad->m_scale_used       = scale | uint64_t{size} << 32; // used=size
  ad->m_nextKI           = 0;

  CopyHash(ad->hashTab(), hash, scale);

  // If we don't trash the elements here, `ad` may fail checkInvariants
  // because some of its element's type bytes happen to be tombstones.
  //
  // Trashing the elements isn't likely to hide real failures, because if we
  // fail to initialize them later, any lookups will return implausible TVs.
  if (debug) memset(ad->data(), kMixedElmFill, sizeof(VanillaDictElm) * size);

  assertx(ad->m_size == size);
  assertx(ad->m_scale == scale);
  assertx(ad->hasExactlyOneRef());
  assertx(ad->m_used == size);
  assertx(ad->m_nextKI == 0);
  assertx(ad->checkInvariants());
  return ad;
}

VanillaDict* VanillaDict::MakeDict(uint32_t size, const TypedValue* kvs) {
  assertx(size > 0);
  auto const scale = computeScaleFromSize(size);
  auto const index = computeIndexFromScale(scale);
  auto const ad    = reqAllocIndex(index);
  auto const aux   = packSizeIndexAndAuxBits(index, 0);

  ad->initHash(scale);
  ad->initHeader_16(HeaderKind::Dict, OneReference, aux);
  *ad->mutableKeyTypes() = ArrayKeyTypes::Empty();
  ad->m_size             = size;
  ad->m_layout_index     = kVanillaLayoutIndex;
  ad->m_scale_used       = scale | uint64_t{size} << 32; // used=size
  ad->m_nextKI           = 0;

  // Append values by moving -- no refcounts are updated.
  auto const data = ad->data();
  for (uint32_t i = 0; i < size; i++) {
    auto& kTv = kvs[i * 2];
    if (isStringType(kTv.m_type)) {
      auto k = kTv.m_data.pstr;
      auto h = k->hash();
      auto ei = ad->findForInsertUpdate(k, h);
      if (isValidPos(ei)) {
        // it's the caller's responsibility to free kvs
        ad->setZombie();
        Release(ad);
        return nullptr;
      }
      data[i].setStrKeyNoIncRef(k, h);
      ad->mutableKeyTypes()->recordStr(k);
      *ei = i;
    } else {
      assertx(kTv.m_type == KindOfInt64);
      auto k = kTv.m_data.num;
      auto h = hash_int64(k);
      auto ei = ad->findForInsertUpdate(k, h);
      if (isValidPos(ei)) {
        // it's the caller's responsibility to free kvs
        ad->setZombie();
        Release(ad);
        return nullptr;
      }
      data[i].setIntKey(k, h);
      ad->mutableKeyTypes()->recordInt();
      *ei = i;
    }
    const auto& tv = kvs[(i * 2) + 1];
    data[i].data.m_data = tv.m_data;
    data[i].data.m_type = tv.m_type;
  }

  assertx(ad->m_size == size);
  assertx(ad->m_scale == scale);
  assertx(ad->hasExactlyOneRef());
  assertx(ad->m_used == size);
  assertx(ad->m_nextKI == 0);
  assertx(ad->checkInvariants());
  return ad;
}

VanillaDict* VanillaDict::MakeDictNatural(uint32_t size, const TypedValue* vals) {
  assertx(size > 0);

  auto const scale = computeScaleFromSize(size);
  auto const index = computeIndexFromScale(scale);
  auto const ad    = reqAllocIndex(index);
  auto const aux   = packSizeIndexAndAuxBits(index, 0);

  ad->initHash(scale);
  ad->initHeader_16(HeaderKind::Dict, OneReference, aux);
  *ad->mutableKeyTypes() = ArrayKeyTypes::Ints();
  ad->m_size             = size;
  ad->m_layout_index     = kVanillaLayoutIndex;
  ad->m_scale_used       = scale | uint64_t{size} << 32; // used=size
  ad->m_nextKI           = size;

  // Append values by moving -- no refcounts are updated.
  auto const data = ad->data();
  for (uint32_t i = 0; i < size; i++) {
    auto h = hash_int64(i);
    auto ei = ad->findForInsertUpdate(i, h);
    assertx(!isValidPos(ei));
    data[i].setIntKey(i, h);
    *ei = i;

    const auto& tv = vals[i];
    data[i].data.m_data = tv.m_data;
    data[i].data.m_type = tv.m_type;
  }

  assertx(ad->m_size == size);
  assertx(ad->m_scale == scale);
  assertx(ad->hasExactlyOneRef());
  assertx(ad->m_used == size);
  assertx(ad->m_nextKI == size);
  assertx(ad->checkInvariants());
  return ad;
}

NEVER_INLINE
VanillaDict* VanillaDict::SlowCopy(VanillaDict* ad, const ArrayData& old,
                                 VanillaDictElm* elm, VanillaDictElm* end) {
  assertx(ad->isRefCounted());

  for (; elm < end; ++elm) {
    if (elm->hasStrKey()) elm->skey->incRefCount();
    // When we convert an element to a tombstone, we set its value type to
    // kInvalidDataType, which is not a refcounted type.
    tvIncRefGenUnsafe(elm->data);
  }
  return ad;
}

// for internal use by copyStatic() and copyMixed()
ALWAYS_INLINE
VanillaDict* VanillaDict::CopyMixed(const VanillaDict& other, AllocMode mode) {
  if (mode == AllocMode::Static) {
    assertx(other.m_size == other.m_used);
    assertx(!other.keyTypes().mayIncludeCounted());
    assertx(!other.keyTypes().mayIncludeTombstone());
  }

  auto const shouldCreateStrKeyTable =
    mode == AllocMode::Static &&
    other.keyTypes().mustBeStaticStrs() &&
    other.size() < StrKeyTable::kStrKeyTableSize * 3 / 4;
  auto const sizeOfStrKeyTable =
    shouldCreateStrKeyTable ? sizeof(StrKeyTable) : 0;
  auto const scale = other.m_scale;
  auto const index = other.sizeIndex();
  auto const ad = mode == AllocMode::Request
    ? reqAllocIndex(index)
    : staticAlloc(scale, sizeOfStrKeyTable);

  // Copy everything including tombstones. This is a requirement for remove() to
  // work correctly, which assumes the position is the same in the original and
  // in the copy of the array, in case copying is needed.
#ifdef USE_JEMALLOC
  // Adding 24 bytes so that we can copy in 32-byte groups. This might
  // overwrite the hash table, but won't overrun the allocated space as long as
  // `malloc' returns multiple of 16 bytes.
  bcopy32_inline(ad, &other,
                 sizeof(VanillaDict) + sizeof(Elm) * other.m_used + 24);
#else
  memcpy(ad, &other, sizeof(VanillaDict) + sizeof(Elm) * other.m_used);
#endif
  auto const count = mode == AllocMode::Request ? OneReference : StaticValue;
  auto const flags = other.auxBits() |
                     (shouldCreateStrKeyTable ? kHasStrKeyTable : 0);
  auto const aux = packSizeIndexAndAuxBits(index, flags);
  ad->initHeader_16(other.m_kind, count, aux);
  *ad->mutableKeyTypes() = other.keyTypes();

  // We want SlowCopy to be a tail call in the opt build, but we still want to
  // check assertions in debug builds, so we check them in this helper.
  auto const check = [&](VanillaDict* res) {
    assertx(res->checkInvariants());
    assertx(res->m_kind == other.m_kind);
    assertx(res->isLegacyArray() == other.isLegacyArray());
    assertx(res->keyTypes() == other.keyTypes());
    assertx(res->m_size == other.m_size);
    assertx(res->m_layout_index == other.m_layout_index);
    assertx(res->m_used == other.m_used);
    assertx(res->m_scale == scale);
    return res;
  };

  VanillaDictElm* elm = ad->data();
  auto const end = elm + ad->m_used;
  if (shouldCreateStrKeyTable) {
    auto table = ad->mutableStrKeyTable();
    for (; elm < end; ++elm) {
      assertx(elm->hasStrKey() && elm->strKey()->isStatic());
      table->add(elm->strKey());
    }
  }

  CopyHash(ad->hashTab(), other.hashTab(), scale);
  if (mode == AllocMode::Static) return check(ad);

  // Bump up refcounts as needed.
  if (other.keyTypes().mayIncludeCounted()) {
    return check(SlowCopy(ad, other, elm, end));
  }
  for (; elm < end; ++elm) {
    assertx(IMPLIES(elm->hasStrKey(), elm->strKey()->isStatic()));
    // When we convert an element to a tombstone, we set its key type to int
    // and value type to kInvalidDataType, neither of which are refcounted.
    tvIncRefGenUnsafe(elm->data);
  }
  return check(ad);
}

NEVER_INLINE ArrayData* VanillaDict::CopyStatic(const ArrayData* in) {
  return CopyMixed(*as(in), AllocMode::Static);
}

NEVER_INLINE VanillaDict* VanillaDict::copyMixed() const {
  assertx(checkInvariants());
  return CopyMixed(*this, AllocMode::Request);
}

//////////////////////////////////////////////////////////////////////

ArrayData* VanillaDict::MakeShared(
    ArrayData* array, const MakeSharedEnv& env, bool hasApcTv) {
  auto a = as(array);
  assertx(!a->empty());
  assertx(a->isRefCounted());

  auto const extra = sharedAllocExtra(array, hasApcTv);
  auto const ad = sharedAlloc(a->scale(), extra);
  auto const used = a->m_used;

  // Do a raw copy first, without worrying about counted types or refcount
  // manipulation.  To copy in 32-byte chunks, we add 24 bytes to the length.
  // This might overwrite the hash table, but won't go beyond the space
  // allocated for the VanillaDict, assuming `malloc()' always allocates
  // multiple of 16 bytes and extra is also a multiple of 16.
  assertx((extra & 0xf) == 0);
  bcopy32_inline(ad, a, sizeof(VanillaDict) + sizeof(Elm) * used + 24);
  ad->m_count = SharedValue; // after bcopy, update count
  if (hasApcTv) {
    ad->m_aux16 |= kHasApcTv;
  } else {
    ad->m_aux16 &= ~kHasApcTv;
  }
  assertx(ad->keyTypes() == a->keyTypes());
  CopyHash(ad->hashTab(), a->hashTab(), a->scale());
  ad->mutableKeyTypes()->makeStatic();

  // Need to make sure keys and values are all uncounted.
  auto dstElem = ad->data();
  for (uint32_t i = 0; i < used; ++i) {
    auto& te = dstElem[i];
    auto const type = te.data.m_type;
    if (UNLIKELY(isTombstone(type))) continue;
    if (te.hasStrKey()) {
      te.skey = MakeSharedString(te.skey, env);
      if (!te.skey->isStatic()) ad->mutableKeyTypes()->recordNonStaticStr();
    }
    ConvertTvToShared(&te.data, env);
  }

  assertx(ad->checkInvariants());
  return ad;
}

ArrayData* VanillaDict::MakeDictFromAPC(const APCArray* apc, bool pure, bool isLegacy) {
  assertx(apc->isHashed());
  auto const apcSize = apc->size();
  DictInit init{apcSize};
  for (uint32_t i = 0; i < apcSize; ++i) {
    init.setValidKey(apc->getHashedKey(i)->toLocal(true /* pure irrelevant for arraykey */),
                     apc->getHashedVal(i)->toLocal(pure));
  }
  auto const ad = init.create();
  ad->setLegacyArrayInPlace(isLegacy);
  return ad;
}

//=============================================================================
// Destruction

NEVER_INLINE
void VanillaDict::SlowRelease(VanillaDict* ad) {
  assertx(ad->isRefCounted());
  assertx(ad->hasExactlyOneRef());
  assertx(!ad->isZombie());

  VanillaDictElm* elm = ad->data();
  for (auto const end = elm + ad->m_used; elm < end; ++elm) {
    if (elm->hasStrKey()) {
      decRefStr(elm->skey);
      // Keep GC from asserting on freed string keys in debug mode.
      if (debug) elm->skey = nullptr;
    }
    // When we convert an element to a tombstone, we set its key type to int
    // and value type to kInvalidDataType, neither of which are refcounted.
    tvDecRefGen(&elm->data);
  }
  tl_heap->objFreeIndex(ad, ad->sizeIndex());
}

NEVER_INLINE
void VanillaDict::Release(ArrayData* in) {
  in->fixCountForRelease();
  assertx(in->isRefCounted());
  assertx(in->hasExactlyOneRef());

  auto const ad = as(in);

  if (!ad->isZombie()) {
    assertx(ad->checkInvariants());
    // keyTypes checks are too slow to go in VanillaDict::checkInvariants.
    assertx(ad->keyTypes().checkInvariants(ad));
    if (ad->keyTypes().mayIncludeCounted()) return SlowRelease(ad);

    VanillaDictElm* elm = ad->data();
    for (auto const end = elm + ad->m_used; elm < end; ++elm) {
      // Keep the GC from asserting on freed string keys in debug mode.
      assertx(IMPLIES(elm->hasStrKey(), elm->strKey()->isStatic()));
      if (debug && elm->hasStrKey()) elm->skey = nullptr;
      // When we convert an element to a tombstone, we set its key type to int
      // and value type to kInvalidDataType, neither of which are refcounted.
      tvDecRefGen(&elm->data);
    }
  }
  tl_heap->objFreeIndex(ad, ad->sizeIndex());
  AARCH64_WALKABLE_FRAME();
}

NEVER_INLINE
void VanillaDict::ReleaseShared(ArrayData* in) {
  assertx(!in->sharedCowCheck());
  auto const ad = as(in);

  if (!ad->isZombie()) {
    auto const data = ad->data();
    auto const stop = data + ad->m_used;

    for (auto ptr = data; ptr != stop; ++ptr) {
      if (isTombstone(ptr->data.m_type)) continue;
      if (ptr->hasStrKey()) DecRefSharedString(ptr->skey);
      DecRefShared(ptr->data);
    }
  }
  auto const extra = sharedAllocExtra(ad, ad->hasApcTv());
  FreeShared(reinterpret_cast<char*>(ad) - extra,
                computeAllocBytes(ad->scale()) + extra);
}

//=============================================================================

/*
 * Invariants:
 *
 *  All arrays are either in a mode, or in zombie state.  The zombie
 *  state happens if an array is "moved from"---the only legal
 *  operation on a zombie array is to decref and release it.
 *
 * All arrays (zombie or not):
 *
 *   m_scale is 2^k (1/4 of the hashtable size and 1/3 of capacity)
 *   mask is 4*scale - 1 (even power of 2 required for quadratic probe)
 *   mask == folly::nextPowTwo(capacity()) - 1;
 *
 * Zombie state:
 *
 *   m_used == UINT32_MAX
 *
 * Non-zombie:
 *
 *   m_size <= m_used; m_used <= capacity()
 *   last element cannot be a tombstone
 *   m_nextKI >= highest actual int key
 *   Elm.data.m_type maybe kInvalidDataType (tombstone)
 *   hash[] maybe Tombstone
 *   static arrays cannot contain tombstones
 */
bool VanillaDict::checkInvariants() const {
  static_assert(ssize_t(Empty) == ssize_t(-1), "");
  static_assert(Tombstone < 0, "");
  static_assert((Tombstone & 1) == 0, "");
  static_assert(sizeof(Elm) == 24, "");
  static_assert(sizeof(ArrayData) == 2 * sizeof(uint64_t), "");
  static_assert(
    sizeof(VanillaDict) == sizeof(ArrayData) + 2 * sizeof(uint64_t),
    "Performance is sensitive to sizeof(VanillaDict)."
    " Make sure you changed it with good reason and then update this assert."
  );

  // All arrays:
  assertx(checkCount());
  assertx(isVanillaDict());
  assertx(m_scale >= 1 && (m_scale & (m_scale - 1)) == 0);
  assertx(sizeIndex() == computeIndexFromScale(m_scale));
  assertx(VanillaDict::HashSize(m_scale) ==
          folly::nextPowTwo<uint64_t>(capacity()));
  assertx(m_layout_index == kVanillaLayoutIndex);

  if (isZombie()) return true;

  // Non-zombie:
  assertx(m_size <= m_used);
  assertx(m_used <= capacity());
  assertx(IMPLIES(isStatic(), m_used == m_size));
  return true;
}

//=============================================================================
// Iteration.

TypedValue VanillaDict::GetPosVal(const ArrayData* ad, ssize_t pos) {
  auto a = as(ad);
  assertx(a->checkInvariants());
  assertx(pos != a->m_used);
  auto const& e = a->data()[pos];
  assertx(!e.isTombstone());
  return e.data;
}

bool VanillaDict::PosIsValid(const ArrayData* ad, ssize_t pos) {
  auto a = as(ad);
  assertx(a->checkInvariants());
  if (pos < 0 || pos >= a->m_used) return false;
  return !a->data()[pos].isTombstone();
}

bool VanillaDict::IsVectorData(const ArrayData* ad) {
  auto a = as(ad);
  if (a->m_size == 0) {
    // any 0-length array is "vector-like" for the sake of this function.
    return true;
  }
  auto const elms = a->data();
  int64_t i = 0;
  for (uint32_t pos = 0, limit = a->m_used; pos < limit; ++pos) {
    auto const& e = elms[pos];
    if (isTombstone(e.data.m_type)) {
      continue;
    }
    if (e.hasStrKey() || e.ikey != i) {
      return false;
    }
    ++i;
  }
  return true;
}

//=============================================================================
// Lookup.

NEVER_INLINE
int32_t* warnUnbalanced(VanillaDict* a, size_t n, int32_t* ei) {
  if (n > size_t(Cfg::Server::MaxArrayChain)) {
    decRefArr(a->asArrayData()); // otherwise, a leaks when exn propagates
    raise_error("Array is too unbalanced (%lu)", n);
  }
  return ei;
}

VanillaDict::InsertPos VanillaDict::insert(int64_t k) {
  assertx(!isFull());
  auto h = hash_int64(k);
  auto ei = findForInsertUpdate(k, h);
  if (isValidPos(ei)) {
    return InsertPos(true, data()[(int32_t)*ei].data);
  }
  if (k >= m_nextKI && m_nextKI >= 0) m_nextKI = static_cast<uint64_t>(k) + 1;
  auto e = allocElm(ei);
  e->setIntKey(k, h);
  mutableKeyTypes()->recordInt();
  return InsertPos(false, e->data);
}

VanillaDict::InsertPos VanillaDict::insert(StringData* k) {
  assertx(!isFull());
  auto const h = k->hash();
  auto ei = findForInsertUpdate(k, h);
  if (isValidPos(ei)) {
    return InsertPos(true, data()[(int32_t)*ei].data);
  }
  auto e = allocElm(ei);
  e->setStrKey(k, h);
  mutableKeyTypes()->recordStr(k);
  return InsertPos(false, e->data);
}

bool VanillaDict::ExistsInt(const ArrayData* ad, int64_t k) {
  return as(ad)->findForExists(k, hash_int64(k));
}

bool VanillaDict::ExistsStr(const ArrayData* ad, const StringData* k) {
  return NvGetStr(ad, k).is_init();
}

//=============================================================================
// Append/insert/update.

/*
 * This is a streamlined copy of Variant.constructValHelper()
 * with no incref+decref because we're moving v to this array.
 */
ALWAYS_INLINE
VanillaDict* VanillaDict::moveVal(TypedValue& tv, TypedValue v) {
  tv.m_type = v.m_type == KindOfUninit ? KindOfNull : v.m_type;
  tv.m_data.num = v.m_data.num;
  return this;
}

NEVER_INLINE
VanillaDict* VanillaDict::SlowGrow(VanillaDict* ad, const ArrayData& old,
                                 VanillaDictElm* elm, VanillaDictElm* end) {
  for (; elm < end; ++elm) {
    if (elm->hasStrKey()) elm->skey->incRefCount();
    // When we convert an element to a tombstone, we set its value type to
    // kInvalidDataType, which is not a refcounted type.
    tvIncRefGenUnsafe(elm->data);
  }

  auto const table = ad->initHash(ad->m_scale);
  auto const mask = VanillaDict::Mask(ad->m_scale);

  elm = ad->data();
  if (UNLIKELY(ad->m_used >= 2000)) {
    return InsertCheckUnbalanced(ad, table, mask, elm, end);
  }
  for (uint32_t i = 0; elm != end; ++elm, ++i) {
    if (elm->isTombstone()) continue;
    *ad->findForNewInsert(table, mask, elm->probe()) = i;
  }
  return ad;
}

NEVER_INLINE
VanillaDict* VanillaDict::Grow(VanillaDict* old, uint32_t newScale, bool copy) {
  assertx(old->m_size > 0);
  assertx(VanillaDict::Capacity(newScale) >= old->m_size);
  assertx(newScale >= 1 && (newScale & (newScale - 1)) == 0);

  auto const index   = computeIndexFromScale(newScale);
  auto const ad      = reqAllocIndex(index);
  auto const oldUsed = old->m_used;
  auto const aux     = packSizeIndexAndAuxBits(index, old->auxBits());

  ad->initHeader_16(old->m_kind, OneReference, aux);
  *ad->mutableKeyTypes() = old->keyTypes();
  ad->m_size = old->m_size;
  ad->m_layout_index = old->m_layout_index;
  ad->m_scale_used = newScale | uint64_t{oldUsed} << 32;
  ad->m_aux16 &= ~(ArrayData::kHasStrKeyTable);

  copyElmsNextUnsafe(ad, old, oldUsed);

  // We want SlowGrow to be a tail call in the opt build, but we still want to
  // check assertions in debug builds, so we check them in this helper.
  auto const check = [&](VanillaDict* res) {
    assertx(res->checkInvariants());
    assertx(res->hasExactlyOneRef());
    assertx(res->kind() == old->kind());
    assertx(res->isLegacyArray() == old->isLegacyArray());
    assertx(res->keyTypes() == old->keyTypes());
    assertx(res->m_size == old->m_size);
    assertx(res->m_used == oldUsed);
    assertx(res->m_scale == newScale);
    assertx(!res->hasStrKeyTable());
    return res;
  };

  if (copy) {
    VanillaDictElm* elm = ad->data();
    auto const end = elm + oldUsed;
    if (ad->keyTypes().mayIncludeCounted()) {
      return check(SlowGrow(ad, *old, elm, end));
    }
    for (; elm < end; ++elm) {
      // When we convert an element to a tombstone, we set its key type to int
      // and value type to kInvalidDataType, neither of which are refcounted.
      tvIncRefGenUnsafe(elm->data);
    }
  } else {
    old->setZombie();
  }

  auto const table = ad->initHash(newScale);

  auto iter = ad->data();
  auto const stop = iter + oldUsed;
  assertx(newScale == ad->m_scale);
  auto mask = VanillaDict::Mask(newScale);

  if (UNLIKELY(oldUsed >= 2000)) {
    return check(InsertCheckUnbalanced(ad, table, mask, iter, stop));
  }
  for (uint32_t i = 0; iter != stop; ++iter, ++i) {
    auto& e = *iter;
    if (e.isTombstone()) continue;
    *ad->findForNewInsert(table, mask, e.probe()) = i;
  }
  return check(ad);
}

ALWAYS_INLINE
VanillaDict* VanillaDict::prepareForInsert(bool copy) {
  assertx(checkInvariants());
  if (isFull()) return Grow(this, m_scale * 2, copy);
  if (copy) return copyMixed();
  return this;
}

void VanillaDict::compact() {
  auto elms = data();
  auto const mask = this->mask();
  auto const table = initHash(m_scale);
  for (uint32_t frPos = 0, toPos = 0; toPos < m_size; ++toPos, ++frPos) {
    while (elms[frPos].isTombstone()) {
      assertx(frPos + 1 < m_used);
      ++frPos;
    }
    auto& toE = elms[toPos];
    if (toPos != frPos) {
      toE = elms[frPos];
    }
    *findForNewInsert(table, mask, toE.probe()) = toPos;
  }

  m_used = m_size;
  // We leave string keys in the array untouched, so the only keyTypes update
  // we can do here is to unset the tombstone bit.
  mutableKeyTypes()->makeCompact();
  assertx(checkInvariants());
}

void VanillaDict::nextInsert(TypedValue v) {
  assertx(m_nextKI >= 0);
  assertx(!isFull());

  int64_t ki = m_nextKI;
  auto h = hash_int64(ki);
  // The check above enforces an invariant that allows us to always
  // know that m_nextKI is not present in the array, so it is safe
  // to use findForNewInsert()
  auto ei = findForNewInsert(h);
  assertx(!isValidPos(ei));
  // Allocate and initialize a new element.
  auto e = allocElm(ei);
  e->setIntKey(ki, h);
  mutableKeyTypes()->recordInt();
  m_nextKI = static_cast<uint64_t>(ki) + 1; // Update next free element.
  tvCopy(v, e->data);
}

template <class K> ALWAYS_INLINE
ArrayData* VanillaDict::update(K k, TypedValue data) {
  assertx(!isFull());
  assertx(data.m_type != KindOfUninit);
  auto p = insert(k);
  if (p.found) {
    tvMove(data, p.tv);
    return this;
  }
  tvCopy(data, p.tv);
  return this;
}

template <class K> ALWAYS_INLINE
ArrayData* VanillaDict::updateSkipConflict(K k, TypedValue data) {
  assertx(!isFull());
  assertx(data.m_type != KindOfUninit);
  auto p = insert(k);
  if (p.found) {
    return this;
  }
  tvCopy(data, p.tv);
  return this;
}

arr_lval VanillaDict::LvalInt(ArrayData* adIn, int64_t key) {
  auto const pos = as(adIn)->find(key, hash_int64(key));
  if (!validPos(pos)) {
    throwOOBArrayKeyException(key, adIn);
  }
  auto const ad = adIn->cowCheck() ? Copy(adIn) : adIn;
  auto const& elm = as(ad)->data()[pos];
  assertx(elm.intKey() == key);
  return { ad, const_cast<TypedValue*>(elm.datatv()) };
}

arr_lval VanillaDict::LvalStr(ArrayData* adIn, StringData* key) {
  auto const pos = as(adIn)->find(key, key->hash());
  if (!validPos(pos)) {
    throwOOBArrayKeyException(key, adIn);
  }
  auto const ad = adIn->cowCheck() ? Copy(adIn) : adIn;
  auto const& elm = as(ad)->data()[pos];
  assertx(elm.strKey()->same(key));
  return { ad, const_cast<TypedValue*>(elm.datatv()) };
}

tv_lval VanillaDict::LvalInPlace(ArrayData* ad, const Variant& k) {
  auto arr = as(ad);
  assertx(!arr->isFull());
  assertx(!arr->cowCheck());
  return k.isInteger() ? arr->addLvalImpl(k.asInt64Val())
                       : arr->addLvalImpl(k.asCStrRef().get());
}

arr_lval VanillaDict::LvalSilentInt(ArrayData* ad, int64_t k) {
  auto a = as(ad);
  auto const pos = a->find(k, hash_int64(k));
  if (UNLIKELY(!validPos(pos))) return arr_lval { a, nullptr };
  if (a->cowCheck()) a = a->copyMixed();
  return arr_lval { a, &a->data()[pos].data };
}

arr_lval VanillaDict::LvalSilentStr(ArrayData* ad, StringData* k) {
  auto a = as(ad);
  auto const pos = a->find(k, k->hash());
  if (UNLIKELY(!validPos(pos))) return arr_lval { a, nullptr };
  if (a->cowCheck()) a = a->copyMixed();
  return arr_lval { a, &a->data()[pos].data };
}

void VanillaDict::AppendTombstoneInPlace(ArrayData* ad) {
  auto a = as(ad);
  assertx(!a->isFull());
  assertx(!a->cowCheck());
  a->mutableKeyTypes()->recordTombstone();
  a->data()[a->m_used].setTombstone();
  a->m_used++;
}

ArrayData* VanillaDict::SetPosMove(ArrayData* ad, ssize_t pos, TypedValue v) {
  assertx(PosIsValid(ad, pos));
  assertx(v.m_type != KindOfUninit);
  assertx(ad->cowCheck() || ad->notCyclic(v));
  auto const result = ad->cowCheck() ? VanillaDict::Copy(ad) : ad;
  tvMove(v, as(result)->data()[pos].data);
  if (ad != result && ad->decReleaseCheck()) VanillaDict::Release(ad);
  return result;
}

ArrayData* VanillaDict::SetIntMove(ArrayData* ad, int64_t k, TypedValue v) {
  assertx(v.m_type != KindOfUninit);
  assertx(ad->cowCheck() || ad->notCyclic(v));
  auto const prepped = as(ad)->prepareForInsert(ad->cowCheck());
  auto const result = prepped->update(k, v);
  if (ad != result && ad->decReleaseCheck()) VanillaDict::Release(ad);
  return result;
}

ArrayData* VanillaDict::SetIntInPlace(ArrayData* ad, int64_t k, TypedValue v) {
  assertx(!ad->cowCheck());
  assertx(ad->notCyclic(v));
  assertx(v.m_type != KindOfUninit);
  tvIncRefGen(v);
  return as(ad)->prepareForInsert(false/*copy*/)->update(k, v);
}

ArrayData* VanillaDict::SetStrMoveSkipConflict(ArrayData* ad, StringData* k, TypedValue v) {
  assertx(v.m_type != KindOfUninit);
  assertx(ad->cowCheck() || ad->notCyclic(v));
  auto const prepped = as(ad)->prepareForInsert(ad->cowCheck());
  auto const result = prepped->updateSkipConflict(k, v);
  if (ad != result && ad->decReleaseCheck()) VanillaDict::Release(ad);
  return result;
}

ArrayData* VanillaDict::SetIntMoveSkipConflict(ArrayData* ad, int64_t k, TypedValue v) {
  assertx(v.m_type != KindOfUninit);
  assertx(ad->cowCheck() || ad->notCyclic(v));
  auto const prepped = as(ad)->prepareForInsert(ad->cowCheck());
  auto const result = prepped->updateSkipConflict(k, v);
  if (ad != result && ad->decReleaseCheck()) VanillaDict::Release(ad);
  return result;
}

ArrayData* VanillaDict::SetStrMove(ArrayData* ad, StringData* k, TypedValue v) {
  assertx(v.m_type != KindOfUninit);
  assertx(ad->cowCheck() || ad->notCyclic(v));
  auto const prepped = as(ad)->prepareForInsert(ad->cowCheck());
  auto const result = prepped->update(k, v);
  if (ad != result && ad->decReleaseCheck()) VanillaDict::Release(ad);
  return result;
}

ArrayData* VanillaDict::SetStrInPlace(ArrayData* ad, StringData* k, TypedValue v) {
  assertx(!ad->cowCheck());
  assertx(ad->notCyclic(v));
  assertx(v.m_type != KindOfUninit);
  tvIncRefGen(v);
  return as(ad)->prepareForInsert(false/*copy*/)->update(k, v);
}

//=============================================================================
// Delete.

void VanillaDict::updateNextKI(int64_t removedKi, bool updateNext) {
  // Conform to PHP5 behavior
  // Hacky: don't removed the unsigned cast, else g++ can optimize away
  // the check for == 0x7fff..., since there is no signed int k
  // for which k-1 == 0x7fff...
  if (((uint64_t)removedKi == (uint64_t)m_nextKI-1) &&
      (removedKi >= 0) &&
      (removedKi == 0x7fffffffffffffffLL || updateNext)) {
    m_nextKI = removedKi;
  }
}

void VanillaDict::eraseNoCompact(RemovePos pos) {
  HashTable::eraseNoCompact(pos);
  mutableKeyTypes()->recordTombstone();
}

ArrayData* VanillaDict::RemoveIntMove(ArrayData* ad, int64_t k) {
  auto a = as(ad);
  auto const pos = a->findForRemove(k, hash_int64(k));
  if (!pos.valid()) return a;

  auto const mad = [&] {
    if (!a->cowCheck()) return a;
    auto const res = a->copyMixed();
    a->decRefCount();
    return res;
  }();

  mad->updateNextKI(k, false);
  mad->erase(pos);
  return mad;
}

ArrayData* VanillaDict::RemoveStrMove(ArrayData* ad, const StringData* key) {
  auto a = as(ad);
  auto const pos = a->findForRemove(key, key->hash());
  if (!pos.valid()) return a;
  auto const mad = [&] {
    if (!a->cowCheck()) return a;
    auto const res = a->copyMixed();
    a->decRefCount();
    return res;
  }();

  mad->erase(pos);
  return mad;
}

ArrayData* VanillaDict::Copy(const ArrayData* ad) {
  return as(ad)->copyMixed();
}

ArrayData* VanillaDict::AppendMove(ArrayData* ad, TypedValue v) {
  auto const copy = ad->cowCheck();
  assertx(copy || ad->notCyclic(v));
  auto a = as(ad);

  // Append is an O(n) operation because we iterate to choose an index to set.
  // We have to be careful about overflow, so we compute the max non-negative
  // integer key first, then compute nextKI with an overflowing add.
  auto maxIntKey = int64_t{-1};
  auto cur = a->data();
  auto const end = cur + a->m_used;
  for (; cur != end; cur++) {
    if (cur->isTombstone() || !cur->hasIntKey()) continue;
    maxIntKey = std::max(maxIntKey, cur->intKey());
  }
  auto const nextKI = int64_t(uint64_t(maxIntKey) + 1);

  if (nextKI != a->m_nextKI && Cfg::Eval::DictDArrayAppendNotices) {
    // Try to eliminate the internal index used for "append", replacing it
    // with a simple set of the key equal to the array's size. If we can make
    // this change now, we can drop appends completely as a follow-up.
    auto const dt = getDataTypeString(a->toDataType());
    raise_notice("Hack Array Compat: append to %s at index %s (maxIntKey + 1)",
                 dt.data(), a->m_nextKI > nextKI ? ">" : "<");
  }

  if (UNLIKELY(a->m_nextKI < 0)) {
    raise_warning("Cannot add element to the array as the next element is "
                  "already occupied");
    return a;
  }
  auto const res = a->prepareForInsert(copy);
  res->nextInsert(v);
  if (res != a && a->decReleaseCheck()) VanillaDict::Release(a);
  return res;
}

/*
 * Copy an array to a new array of mixed kind, with a particular
 * pre-reserved size.
 */
NEVER_INLINE
VanillaDict* VanillaDict::CopyReserve(const VanillaDict* src,
                                     size_t expectedSize) {
  auto const scale = computeScaleFromSize(expectedSize);
  auto const index = computeIndexFromScale(scale);
  auto const ad    = reqAllocIndex(index);
  auto const oldUsed = src->m_used;
  auto const aux = packSizeIndexAndAuxBits(index, src->auxBits());

  ad->initHeader_16(src->m_kind, OneReference, aux);
  *ad->mutableKeyTypes() = src->keyTypes();
  ad->m_size            = src->m_size;
  ad->m_layout_index    = src->m_layout_index;
  ad->m_scale           = scale; // don't set m_used yet
  ad->m_nextKI          = src->m_nextKI;

  auto const table = ad->initHash(scale);

  auto dstElm = ad->data();
  auto srcElm = src->data();
  auto const srcStop = src->data() + oldUsed;
  uint32_t i = 0;

  // Copy the elements
  auto mask = VanillaDict::Mask(scale);
  if (src->keyTypes().mayIncludeCounted()) {
    for (; srcElm != srcStop; ++srcElm) {
      if (srcElm->isTombstone()) continue;
      tvDup(srcElm->data, dstElm->data);
      auto const hash = static_cast<int32_t>(srcElm->probe());
      if (hash < 0) {
        dstElm->setIntKey(srcElm->ikey, hash);
      } else {
        dstElm->setStrKey(srcElm->skey, hash);
      }
      *ad->findForNewInsert(table, mask, hash) = i;
      ++dstElm;
      ++i;
    }
  } else {
    for (; srcElm != srcStop; ++srcElm) {
      if (srcElm->isTombstone()) continue;
      tvDup(srcElm->data, dstElm->data);
      auto const hash = static_cast<int32_t>(srcElm->probe());
      if (hash < 0) {
        dstElm->setIntKey(srcElm->ikey, hash);
      } else {
        dstElm->setStrKeyNoIncRef(srcElm->skey, hash);
      }
      *ad->findForNewInsert(table, mask, hash) = i;
      ++dstElm;
      ++i;
    }
  }

  // Set new used value (we've removed any tombstones).
  assertx(i == dstElm - ad->data());
  ad->m_used = i;

  assertx(ad->kind() == src->kind());
  assertx(ad->m_size == src->m_size);
  assertx(ad->m_layout_index == src->m_layout_index);
  assertx(ad->hasExactlyOneRef());
  assertx(ad->m_used <= oldUsed);
  assertx(ad->m_used == dstElm - ad->data());
  assertx(ad->m_scale == scale);
  assertx(ad->m_nextKI == src->m_nextKI);
  assertx(ad->checkInvariants());
  assertx(!ad->hasStrKeyTable());
  return ad;
}

ArrayData* VanillaDict::PopMove(ArrayData* ad, Variant& value) {
  if (ad->empty()) {
    value = uninit_null();
    return ad;
  }

  auto const a = as(ad);
  auto elms = a->data();
  ssize_t pos = IterLast(a);

  assertx(pos >= 0 && pos < a->m_used);
  auto& e = elms[pos];
  assertx(!isTombstone(e.data.m_type));
  value = tvAsCVarRef(&e.data);

  return e.hasStrKey() ? VanillaDict::RemoveStrMove(a, e.skey)
                       : VanillaDict::RemoveIntMove(a, e.ikey);

}

void VanillaDict::OnSetEvalScalar(ArrayData* ad) {
  auto a = as(ad);
  if (UNLIKELY(a->m_size < a->m_used)) {
    a->compact();
  }
  a->mutableKeyTypes()->makeStatic();
  auto elm = a->data();
  for (auto const end = elm + a->m_used; elm < end; ++elm) {
    assertx(!elm->isTombstone());
    auto key = elm->skey;
    if (elm->hasStrKey() && !key->isStatic()) {
      elm->skey = makeStaticString(key);
      decRefStr(key);
    }
    tvAsVariant(&elm->data).setEvalScalar();
  }
}

//////////////////////////////////////////////////////////////////////

ALWAYS_INLINE
bool VanillaDict::DictEqualHelper(const ArrayData* ad1, const ArrayData* ad2,
                                 bool strict) {
  assertx(as(ad1)->checkInvariants());
  assertx(as(ad2)->checkInvariants());
  assertx(ad1->isVanillaDict());
  assertx(ad2->isVanillaDict());

  if (ad1 == ad2) return true;
  if (ad1->size() != ad2->size()) return false;

  // Prevent circular referenced objects/arrays or deep ones.
  check_recursion_error();

  if (strict) {
    auto const arr1 = as(ad1);
    auto const arr2 = as(ad2);
    auto elm1 = arr1->data();
    auto elm2 = arr2->data();
    auto i1 = arr1->m_used;
    auto i2 = arr2->m_used;
    while (i1 > 0 && i2 > 0) {
      if (UNLIKELY(elm1->isTombstone())) {
        ++elm1;
        --i1;
        continue;
      }
      if (UNLIKELY(elm2->isTombstone())) {
        ++elm2;
        --i2;
        continue;
      }
      if (elm1->hasIntKey()) {
        if (!elm2->hasIntKey()) return false;
        if (elm1->ikey != elm2->ikey) return false;
      } else {
        assertx(elm1->hasStrKey());
        if (!elm2->hasStrKey()) return false;
        if (!HPHP::same(elm1->skey, elm2->skey)) return false;
      }
      if (!tvSame(*tvAssertPlausible(&elm1->data), *tvAssertPlausible(&elm2->data))) {
        return false;
      }
      ++elm1;
      ++elm2;
      --i1;
      --i2;
    }

    if (!i1) {
      while (i2 > 0) {
        if (UNLIKELY(!elm2->isTombstone())) return false;
        ++elm2;
        --i2;
      }
    } else {
      assertx(!i2);
      while (i1 > 0) {
        if (UNLIKELY(!elm1->isTombstone())) return false;
        ++elm1;
        --i1;
      }
    }
  } else {
    auto const arr1 = as(ad1);
    auto elm = arr1->data();
    for (auto i = arr1->m_used; i--; elm++) {
      if (UNLIKELY(elm->isTombstone())) continue;
      auto const other_tv = [&] {
        if (elm->hasIntKey()) {
          return NvGetInt(ad2, elm->ikey);
        } else {
          assertx(elm->hasStrKey());
          return NvGetStr(ad2, elm->skey);
        }
      }();
      if (!other_tv.is_init() ||
          !tvEqual(tvAssertPlausible(elm->data),
                   tvAssertPlausible(other_tv))) {
        return false;
      }
    }
  }

  return true;
}

bool VanillaDict::DictEqual(const ArrayData* ad1, const ArrayData* ad2) {
  return DictEqualHelper(ad1, ad2, false);
}

bool VanillaDict::DictNotEqual(const ArrayData* ad1, const ArrayData* ad2) {
  return !DictEqualHelper(ad1, ad2, false);
}

bool VanillaDict::DictSame(const ArrayData* ad1, const ArrayData* ad2) {
  return DictEqualHelper(ad1, ad2, true);
}

bool VanillaDict::DictNotSame(const ArrayData* ad1, const ArrayData* ad2) {
  return !DictEqualHelper(ad1, ad2, true);
}

//////////////////////////////////////////////////////////////////////

}
