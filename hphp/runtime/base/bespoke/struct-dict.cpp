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

#include <fmt/format.h>

#include "hphp/runtime/base/bespoke/struct-dict.h"

#include "hphp/runtime/base/bespoke/escalation-logging.h"
#include "hphp/runtime/base/datatype.h"
#include "hphp/runtime/base/bespoke/struct-data-layout.h"
#include "hphp/runtime/base/tv-shared.h"
#include "hphp/runtime/base/vanilla-dict-defs.h"
#include "hphp/runtime/base/vanilla-dict.h"

#include "hphp/runtime/vm/jit/array-layout.h"
#include "hphp/runtime/vm/jit/type.h"

#include <sstream>

namespace HPHP::bespoke {

//////////////////////////////////////////////////////////////////////////////

namespace {

size_t s_numStructLayoutsCreated = 0;

struct FieldVectorHash {
  size_t operator()(const StructLayout::FieldVector& fv) const {
    auto hash = size_t {0};
    for (auto const& f : fv) {
      static_assert(sizeof(strhash_t) == 4);
      auto const here = static_cast<uint64_t>(f.key->hash()) |
                        static_cast<uint64_t>(f.required) << 32;
      hash = folly::hash::hash_combine(hash, here);
    }
    return hash;
  }
};

folly::SharedMutex s_fieldVectorLock;
std::unordered_map<StructLayout::FieldVector, LayoutIndex, FieldVectorHash>
  s_fieldVectorToIdx;

const LayoutFunctions* structDictVtable() {
  static auto const result = fromArray<StructDict>();
  return &result;
}

std::string describeStructLayout(const StructLayout::FieldVector& fv) {
  std::stringstream ss;
  for (auto i = 0; i < std::min(fv.size(), 0xfful); ++i) {
    auto const& field = fv[i];
    if (i > 0) ss << ',';
    if (!field.required) ss << '?';
    ss << '"' << folly::cEscape<std::string>(field.key->data()) << '"';
    ss << ':' << StructLayout::MaskToBound(field.type_mask).toString();
  }
  auto const isBigStruct = fv.size() > 0xff;
  if (isBigStruct) ss << ", ...";
  auto const type = isBigStruct ? 'B' : 'S';
  return fmt::format("StructDict<{},{}><{}>", type, fv.size(), ss.str());
}

constexpr uint16_t indexRaw(uint16_t idx) {
  auto const hi_byte = idx >> 8;
  auto const lo_byte = idx & 0xff;
  auto const res = (hi_byte << 9) + lo_byte + 0x100;
  return static_cast<uint16_t>(res);
}

StructLayout::PerfectHashTable* s_hashTableSet = nullptr;
size_t s_maxColoredFields = 1;

}

size_t numStructLayoutsCreated() {
  assertx(Layout::HierarchyFinalized());
  return s_numStructLayoutsCreated;
}

//////////////////////////////////////////////////////////////////////////////

StructDict* StructDict::As(ArrayData* ad) {
  auto const result = reinterpret_cast<StructDict*>(ad);
  assertx(result->checkInvariants());
  return result;
}

const StructDict* StructDict::As(const ArrayData* ad) {
  return As(const_cast<ArrayData*>(ad));
}

bool StructDict::checkInvariants() const {
  static_assert(sizeof(StructDict) == 16);
  assertx(layout()->index() == layoutIndex());
  assertx(layout()->sizeIndex() == sizeIndex());
  assertx(layout()->numFields() == numFields());
  StructDataLayout::checkInvariants(this);
  assertx(StructLayout::IsStructLayout(layoutIndex()));
  return true;
}

size_t StructLayout::typeOffsetForSlot(Slot slot) const {
  return StructDataLayout::typeOffsetForSlot(this, slot);
}

size_t StructLayout::valueOffsetForSlot(Slot slot) const {
  return StructDataLayout::valueOffsetForSlot(this, slot);
}

size_t StructLayout::positionOffset() const {
  return StructDataLayout::positionOffset(this);
}

bool StructLayout::checkTypeBound(Slot slot, TypedValue tv) const {
  assertx(slot < numFields());
  return !(static_cast<uint8_t>(tv.type()) & m_fields[slot].type_mask);
}

jit::Type StructLayout::getTypeBound(Slot slot) const {
  assertx(slot < numFields());
  return MaskToBound(m_fields[slot].type_mask) & jit::TInitCell;
}

jit::Type StructLayout::getUnionTypeBound() const {
  auto result = jit::TBottom;
  for (auto slot = 0; slot < numFields(); slot++) {
    result |= getTypeBound(slot);
  }
  return result;
}

uint8_t StructLayout::BoundToMask(const Type& type) {
  // We support three kinds of type bound on StructDict values:
  //   1. No bound; any value is allowed
  //   2. "uncounted"; a union of scalars like int/float/bool/null
  //   3. A known, non-persistent-flavor DataType
  if (type == jit::TCell) return 0;
  if (type == jit::TUncounted) return safe_cast<uint8_t>(kRefCountedBit);

  // Check that we're in case 3. Any other type_mask is invalid.
  assertx(type.isKnownDataType());
  auto const dt = type.toDataType();
  assertx(type == Type(dt));
  return ~static_cast<uint8_t>(dt);
}

jit::Type StructLayout::MaskToBound(uint8_t mask) {
  if (mask == 0) return jit::TCell;
  if (mask == static_cast<uint8_t>(kRefCountedBit)) return jit::TUncounted;

  auto const dt = static_cast<DataType>(~mask);
  assertx(isRealType(dt));
  return jit::Type(dt);
}

// As documented in bespoke/layout.h, bespoke layout bytes are constrained to
// have bit 0 (the low bit) set and bit 7 (the high bit) unset. Index() turns
// a serialize index into this form; IsStructLayout checks it.
LayoutIndex StructLayout::Index(uint16_t idx) {
  auto const result = indexRaw(idx);
  always_assert(IsStructLayout({result}));
  return {result};
}

bool StructLayout::IsStructLayout(LayoutIndex index) {
  auto const byte = index.byte();
  return (byte & 0b10000001) == 0b00000001;
}

const StructLayout* StructLayout::GetLayout(
    const FieldVector& fv, bool create) {
	if (fv.empty()) return nullptr;
  {
    std::shared_lock rlock{s_fieldVectorLock};
    auto const it = s_fieldVectorToIdx.find(fv);
    if (it != s_fieldVectorToIdx.end()) return As(FromIndex(it->second));
  }
  if (!create) return nullptr;

  std::unique_lock wlock{s_fieldVectorLock};
  auto const it = s_fieldVectorToIdx.find(fv);
  if (it != s_fieldVectorToIdx.end()) return As(FromIndex(it->second));

  if (s_numStructLayoutsCreated == Cfg::Eval::BespokeMaxNumStructLayouts) {
    return nullptr;
  }

  // We only construct this layout if it has at least one child, in order
  // to satisfy invariants in FinalizeHierarchy().
  if (s_numStructLayoutsCreated == 0) {
    new TopStructLayout();
    s_numStructLayoutsCreated++;
  }

  auto const index = Index(safe_cast<uint16_t>(s_numStructLayoutsCreated++));
  auto const bytes = sizeof(StructLayout) + sizeof(Field) * (fv.size() - 1);
  auto const result = new (malloc(bytes)) StructLayout(index, fv);
  s_fieldVectorToIdx.emplace(fv, index);
  return result;
}

const StructLayout* StructLayout::Deserialize(
    LayoutIndex index, const FieldVector& fv) {
  auto const layout = GetLayout(fv, true);
  always_assert(layout != nullptr);
  always_assert(layout->index() == index);
  return layout;
}

StructLayout::StructLayout(LayoutIndex index, const FieldVector& fv)
  : ConcreteLayout(index, describeStructLayout(fv),
                   {TopStructLayout::Index()}, structDictVtable())
{
  Slot i = 0;
  m_key_to_slot.reserve(fv.size());
  for (auto const& field : fv) {
    assertx(field.key->isStatic());
    assertx(BoundToMask(MaskToBound(field.type_mask)) == field.type_mask);
    if (field.required) m_num_required_fields++;
    m_key_to_slot.insert({StaticKey{field.key}, i});
    m_fields[i] = field;
    i++;
  }

  m_header.m_layout_index = index;
  m_header.m_num_fields = fv.size();
  assertx(numFields() == m_key_to_slot.size());
  m_may_contain_counted = getUnionTypeBound().maybe(jit::TCounted);

  StructDataLayout::initSizeIndex(this);

  auto constexpr extra_offset = offsetof(StructLayout, m_extra_initializer);
  static_assert(
    offsetof(StructLayout, m_header.m_layout_index) - extra_offset ==
      ArrayData::offsetOfBespokeIndex() - ArrayData::offsetofExtra());
}

bool StructLayout::isBigStruct() const {
  return StructDataLayout::isBigStruct(this);
}

size_t StructLayout::numFields() const {
  return m_header.m_num_fields;
}

size_t StructLayout::sizeIndex() const {
  return m_size_index;
}

uint32_t StructLayout::extraInitializer() const {
  return m_extra_initializer;
}

Slot StructLayout::keySlot(LayoutIndex index, const StringData* key) {
  if (!key->isStatic()) {
    auto const layout = StructLayout::As(Layout::FromIndex(index));
    return layout->keySlotNonStatic(key);
  }
  return keySlotStatic(index, key);
}

Slot StructLayout::keySlotNonStatic(LayoutIndex index, const StringData* key) {
  auto const layout = StructLayout::As(Layout::FromIndex(index));
  return layout->keySlotNonStatic(key);
}

Slot StructLayout::keySlotStatic(LayoutIndex index, const StringData* key) {
  assertx(key->isStatic());
  auto const col = key->color();
  auto const& entry = s_hashTableSet[index.raw][col];
  auto const match = entry.str == key;
  if constexpr (debug) {
    auto const layout = StructLayout::As(Layout::FromIndex(index));
    auto const slot = layout->keySlotNonStatic(key);
    always_assert(entry.maybeDup ||
                  slot == (match ? Slot(entry.slot) : kInvalidSlot));
  }
  if (match) return Slot(entry.slot);
  if (entry.maybeDup) {
    auto const layout = StructLayout::As(Layout::FromIndex(index));
    return layout->keySlotNonStatic(key);
  }
  return kInvalidSlot;
}

Slot StructLayout::keySlot(const StringData* key) const {
  if (!key->isStatic()) return keySlotNonStatic(key);
  return keySlotStatic(index(), key);
}

NEVER_INLINE
Slot StructLayout::keySlotNonStatic(const StringData* key) const {
  auto const it = m_key_to_slot.find(NonStaticKey{key});
  return it == m_key_to_slot.end() ? kInvalidSlot : it->second;
}

const StructLayout::Field& StructLayout::field(Slot slot) const {
  assertx(slot < numFields());
  return m_fields[slot];
}

template <bool Static>
StructDict* StructDict::MakeReserve(const StructLayout* layout, bool legacy) {
  assertx(layout);
  auto const sizeIdx = layout->sizeIndex();
  auto const alloc = [&] {
    if (!Static) return tl_heap->objMallocIndex(sizeIdx);
    auto const size = MemoryManager::sizeIndex2Size(sizeIdx);
    return ArrayData::AllocStatic(size);
  }();

  auto const sad = static_cast<StructDict*>(alloc);
  auto const flags =
    (legacy ? ArrayData::kLegacyArray : 0) |
    (layout->mayContainCounted() ? ArrayData::kMayContainCounted : 0);
  auto const aux = packSizeIndexAndAuxBits(sizeIdx, flags);
  sad->initHeader_16(HeaderKind::BespokeDict, OneReference, aux);
  sad->m_extra = layout->extraInitializer();
  sad->m_size = 0;
  StructDataLayout::init(sad);
  assertx(sad->checkInvariants());
  return sad;
}

size_t StructDict::numFields() const {
  return StructDataLayout::numFields(this);
}

void StructLayout::createColoringHashMap(size_t maxColoredFields) const {
  auto& table = s_hashTableSet[index().raw];
  for (auto i = 0; i <= kMaxColor; i++) {
    table[i] = { nullptr, 0, false, 0 };
  }
  auto const numColoredFields = std::min(numFields(), maxColoredFields);
  for (auto i = 0; i < numColoredFields; i++) {
    auto const key = m_fields[i].key;
    auto const color = key->color();
    assertx(color != StringData::kInvalidColor);
    assertx(color != StringData::kDupColor);
    assertx(table[color].str == nullptr);
    auto const slot = safe_cast<uint16_t>(keySlotNonStatic(key));
    auto const typeMask = m_fields[i].type_mask;
    table[color] = { key, typeMask, false, slot };
  }
  for (auto i = maxColoredFields; i < numFields(); i++) {
    auto const key = m_fields[i].key;
    auto const color = key->color();
    assertx(color != StringData::kInvalidColor);
    table[color].maybeDup = true;
  }
}

StructLayout::PerfectHashTable* StructLayout::hashTableSet() {
  assertx(Layout::HierarchyFinalized());
  return s_hashTableSet;
}

StructLayout::PerfectHashTable* StructLayout::hashTable(const Layout* layout) {
  assertx(StructLayout::As(layout));
  assertx(Layout::HierarchyFinalized());
  return &s_hashTableSet[layout->index().raw];
}

size_t StructLayout::maxColoredFields() {
  assertx(Layout::HierarchyFinalized());
  return s_maxColoredFields;
}

void StructLayout::setMaxColoredFields(size_t num) {
  assertx(!Layout::HierarchyFinalized());
  s_maxColoredFields = num;
}

const StructLayout* StructLayout::As(const Layout* l) {
  assertx(dynamic_cast<const StructLayout*>(l));
  return reinterpret_cast<const StructLayout*>(l);
}

template<typename PosType>
StructDict* StructDict::MakeFromVanillaImpl(
  ArrayData* ad, const StructLayout* layout) {
  if (!ad->isVanillaDict()) return nullptr;

  assertx(layout);
  auto const result = ad->isStatic()
    ? MakeReserve<true>(layout, ad->isLegacyArray())
    : MakeReserve<false>(layout, ad->isLegacyArray());

  auto fail = false;
  auto required = layout->numRequiredFields();
  auto pos = static_cast<PosType*>(result->rawPositions());

  VanillaDict::IterateKV(VanillaDict::as(ad), [&](auto k, auto v) -> bool {
    if (!tvIsString(k)) {
      fail = true;
      return true;
    }
    auto const slot = layout->keySlot(val(k).pstr);
    if (slot == kInvalidSlot || !layout->checkTypeBound(slot, v)) {
      fail = true;
      return true;
    }
    if (layout->field(slot).required) required--;

    *pos++ = slot;
    result->m_size++;
    auto const lval = result->lvalUnchecked(slot);
    lval.type() = type(v);
    lval.val() = val(v);
    tvIncRefGen(v);
    return false;
  });

  if (fail || required) {
    if (!ad->isStatic()) Release(result);
    return nullptr;
  }

  if (ad->isStatic()) {
    auto const aux = packSizeIndexAndAuxBits(
        result->sizeIndex(), result->auxBits());
    result->initHeader_16(HeaderKind::BespokeDict, StaticValue, aux);
  }

  assertx(result->checkInvariants());
  return result;
}

StructDict* StructDict::MakeFromVanilla(ArrayData* ad, const StructLayout* l) {
  return l->isBigStruct() ?
    MakeFromVanillaImpl<uint16_t>(ad, l) : MakeFromVanillaImpl<uint8_t>(ad, l);
}

StructDict* StructDict::MakeEmpty(const StructLayout* layout) {
  auto const sizeIndex = safe_cast<uint8_t>(layout->sizeIndex());
  return AllocStructDict(sizeIndex,
                         layout->extraInitializer(),
                         layout->mayContainCounted());
}

StructDict* StructDict::AllocStructDict(uint8_t sizeIndex,
                                        uint32_t extra,
                                        bool mayContainCounted) {
  auto const mem = tl_heap->objMallocIndex(sizeIndex);
  auto const sad = static_cast<StructDict*>(mem);
  auto const aux = packSizeIndexAndAuxBits(
    sizeIndex, mayContainCounted ? ArrayData::kMayContainCounted : 0);
  sad->initHeader_16(HeaderKind::BespokeDict, OneReference, aux);
  sad->m_extra = extra;
  sad->m_size = 0;
  StructDataLayout::init(sad);
  assertx(sad->checkInvariants());
  return sad;
}

template<typename PosType>
StructDict* StructDict::MakeStructDict(
    uint8_t sizeIndex, uint32_t extra, uint32_t size, bool mayContainCounted,
    const PosType* slots, const TypedValue* tvs) {
  auto const result = AllocStructDict(sizeIndex, extra, mayContainCounted);

  result->m_size = size;
  auto const positions = result->rawPositions();
  memcpy(positions, slots, size * sizeof(PosType));

  for (auto i = 0; i < size; i++) {
    assertx(slots[i] < result->numFields());
    auto const& tv = tvs[size - i - 1];
    auto const slot = slots[i];
    auto const lval = result->lvalUnchecked(slot);
    lval.type() = type(tv);
    lval.val() = val(tv);
  }

  assertx(result->checkInvariants());
  assertx(result->size() == size);
  return result;
}

StructDict* StructDict::MakeStructDictSmall(
    uint8_t sizeIndex, uint32_t extra, uint32_t size, bool mayContainCounted,
    const uint8_t* slots, const TypedValue* tvs) {
  return MakeStructDict<uint8_t>(sizeIndex, extra, size, mayContainCounted,
                                 slots, tvs);
}

StructDict* StructDict::MakeStructDictBig(
    uint8_t sizeIndex, uint32_t extra, uint32_t size, bool mayContainCounted,
    const uint16_t* slots, const TypedValue* tvs) {
  return MakeStructDict<uint16_t>(sizeIndex, extra, size, mayContainCounted,
                                  slots, tvs);
}

bool StructDict::isBigStruct() const {
  return StructDataLayout::isBigStruct(this);
}

const StructLayout* StructDict::layout() const {
  return StructLayout::As(Layout::FromIndex(layoutIndex()));
}

void* StructDict::rawPositions() {
  return
    reinterpret_cast<char*>(this) + StructDataLayout::positionOffset(this);
}

const void* StructDict::rawPositions() const {
  return const_cast<StructDict*>(this)->rawPositions();
}

tv_lval StructDict::lvalUnchecked(Slot slot) {
  return StructDataLayout::tvAtSlot<false>(this, slot);
}

tv_rval StructDict::rvalUnchecked(Slot slot) const {
  return StructDataLayout::tvAtSlot<true>(this, slot);
}

ArrayData* StructDict::escalateWithCapacity(size_t capacity,
                                            const char* reason) const {
  assertx(capacity >= size());
  logEscalateToVanilla(this, reason);

  auto ad = VanillaDict::MakeReserveDict(capacity);
  ad->setLegacyArrayInPlace(isLegacyArray());

  auto const layout = this->layout();
  auto const fn = [&]<typename PosType>() {
    for (auto i = 0; i < m_size; i++) {
      auto const slot = getSlotInPos<PosType>(i);
      auto const k = layout->field(slot).key;
      auto const tv = rvalUnchecked(slot).tv();
      auto const res =
        VanillaDict::SetStrMove(ad, const_cast<StringData*>(k.get()), tv);
      assertx(ad == res);
      tvIncRefGen(tv);
      ad = res;
    }
  };
  isBigStruct() ?
    fn.template operator()<uint16_t>() : fn.template operator()<uint8_t>();
  assertx(ad->size() == size());
  return ad;
}

void StructDict::ConvertToShared(
    StructDict* sad, const MakeSharedEnv& env) {
  auto const size = sad->size();
  auto const fn = [&]<typename PosType>() {
    for (auto pos = 0; pos < size; pos++) {
      auto const slot = sad->getSlotInPos<PosType>(pos);
      auto const lval = sad->lvalUnchecked(slot);
      ConvertTvToShared(lval, env);
    }
  };
  sad->isBigStruct() ?
    fn.template operator()<uint16_t>() : fn.template operator()<uint8_t>();
}

void StructDict::ReleaseShared(StructDict* sad) {
  auto const size = sad->size();
  auto const fn = [&]<typename PosType>() {
    for (auto pos = 0; pos < size; pos++) {
      auto const slot = sad->getSlotInPos<PosType>(pos);
      DecRefShared(sad->rvalUnchecked(slot).tv());
    }
  };
  sad->isBigStruct() ?
    fn.template operator()<uint16_t>() : fn.template operator()<uint8_t>();
}

void StructDict::Release(StructDict* sad) {
  sad->fixCountForRelease();
  assertx(sad->isRefCounted());
  assertx(sad->hasExactlyOneRef());
  if (sad->mayContainCounted()) sad->decRefValues();
  tl_heap->objFreeIndex(sad, sad->sizeIndex());
}

ArrayData* StructDict::Copy(const StructDict* sad) {
  return sad->copy();
}

bool StructDict::IsVectorData(const StructDict* sad) {
  return sad->empty();
}

TypedValue StructDict::NvGetInt(const StructDict*, int64_t) {
  return make_tv<KindOfUninit>();
}

NEVER_INLINE
TypedValue StructDict::NvGetStrNonStatic(
    const StructDict* sad, const StringData* k) {
  auto const structLayout = sad->layout();
  auto const slot = structLayout->keySlotNonStatic(k);
  if (slot == kInvalidSlot) return make_tv<KindOfUninit>();
  return sad->rvalUnchecked(slot).tv();
}

TypedValue StructDict::NvGetStr(const StructDict* sad, const StringData* k) {
  static_assert(folly::isPowTwo(StructLayout::kMaxColor + 1));
  auto const color = k->color() & StructLayout::kMaxColor;
  auto const& entry = s_hashTableSet[sad->layoutIndex().raw][color];

  // Perfect hash table miss: k is definitely missing if it's a static string
  // and the color cannot be duplicate;
  // else, we need to fall back to a hash table lookup for the non-static k.
  if (entry.str != k) {
    if (k->isStatic() && !entry.maybeDup) {
      assertx(NvGetStrNonStatic(sad, k).m_type == KindOfUninit);
      return make_tv<KindOfUninit>();
    }
    return NvGetStrNonStatic(sad, k);
  }

  // Perfect hash table hit: we can use the field offsets.
  auto const rval = sad->rvalUnchecked(entry.slot);
  if constexpr (debug) {
    auto const result = NvGetStrNonStatic(sad, k);
    always_assert(result.m_type == rval.type());
    always_assert(result.m_data.pcnt == rval.val().pcnt);
  }
  return rval.tv();
}

TypedValue StructDict::GetPosKey(const StructDict* sad, ssize_t pos) {
  auto const layout = sad->layout();
  auto const slot = sad->isBigStruct() ?
    sad->getSlotInPos<uint16_t>(pos) : sad->getSlotInPos<uint8_t>(pos);
  auto const k = layout->field(slot).key;
  return make_tv<KindOfString>(k);
}

TypedValue StructDict::GetPosVal(const StructDict* sad, ssize_t pos) {
  auto const slot = sad->isBigStruct() ?
    sad->getSlotInPos<uint16_t>(pos) : sad->getSlotInPos<uint8_t>(pos);
  return sad->rvalUnchecked(slot).tv();
}

bool StructDict::PosIsValid(const StructDict* sad, ssize_t pos) {
  return pos >= 0 && pos < sad->m_size;
}

ssize_t StructDict::IterBegin(const StructDict*) {
  return 0;
}

ssize_t StructDict::IterLast(const StructDict* sad) {
  return sad->empty() ? 0 : sad->size() - 1;
}

ssize_t StructDict::IterEnd(const StructDict* sad) {
  return sad->size();
}

ssize_t StructDict::IterAdvance(const StructDict* sad, ssize_t pos) {
  return pos < sad->size() ? pos + 1 : pos;
}

ssize_t StructDict::IterRewind(const StructDict* sad, ssize_t pos) {
  return pos > 0 ? pos - 1 : sad->size();
}

arr_lval StructDict::LvalInt(StructDict* sad, int64_t k) {
  throwOOBArrayKeyException(k, sad);
}

arr_lval StructDict::LvalStr(StructDict* sad, StringData* key) {
  auto const slot = StructLayout::keySlot(sad->layoutIndex(), key);
  if (slot == kInvalidSlot) throwOOBArrayKeyException(key, sad);
  auto const currType = sad->rvalUnchecked(slot).type();
  if (currType == KindOfUninit) throwOOBArrayKeyException(key, sad);
  auto const newad = sad->cowCheck() ? sad->copy() : sad;
  return { newad, newad->lvalUnchecked(slot) };
}

tv_lval StructDict::ElemInt(tv_lval lval, int64_t k, bool throwOnMissing) {
  if (throwOnMissing) throwOOBArrayKeyException(k, lval.val().parr);
  return const_cast<TypedValue*>(&immutable_null_base);
}

arr_lval StructDict::elemImpl(StringData* k, bool throwOnMissing) {
  auto const slot = StructLayout::keySlot(layoutIndex(), k);
  if (slot == kInvalidSlot) {
    if (throwOnMissing) throwOOBArrayKeyException(k, this);
    return {this, const_cast<TypedValue*>(&immutable_null_base)};
  }
  auto const currType = rvalUnchecked(slot).type();
  if (currType == KindOfUninit) {
    if (throwOnMissing) throwOOBArrayKeyException(k, this);
    return {this, const_cast<TypedValue*>(&immutable_null_base)};
  }
  auto const sad = cowCheck() ? this->copy() : this;
  auto const lval = sad->lvalUnchecked(slot);
  return arr_lval{sad, lval};
}

tv_lval StructDict::ElemStr(tv_lval lvalIn, StringData* k, bool throwOnMissing) {
  auto sadIn = As(lvalIn.val().parr);
  auto const lval = sadIn->elemImpl(k, throwOnMissing);
  if (lval.arr != sadIn) {
    lvalIn.val().parr = lval.arr;
    if (sadIn->decReleaseCheck()) Release(sadIn);
  }
  return lval;
}

ArrayData* StructDict::SetPosMove(StructDict* sad, ssize_t pos, TypedValue v) {
  auto const vad = sad->escalateWithCapacity(sad->size(), __func__);
  auto const res = VanillaDict::SetPosMove(vad, pos, v);
  assertx(vad == res);
  if (sad->decReleaseCheck()) Release(sad);
  return res;
}

ArrayData* StructDict::SetIntMove(StructDict* sad, int64_t k, TypedValue v) {
  auto const vad = sad->escalateWithCapacity(sad->size() + 1, __func__);
  auto const res = VanillaDict::SetIntMove(vad, k, v);
  assertx(vad == res);
  if (sad->decReleaseCheck()) Release(sad);
  return res;
}

ArrayData* StructDict::SetStrMove(StructDict* sadIn,
                                  StringData* k,
                                  TypedValue v) {
  Slot slot = kInvalidSlot;
  auto constexpr kNoMask = uint8_t{0};

  auto const type_mask = [&] {
    static_assert(folly::isPowTwo(StructLayout::kMaxColor + 1));
    auto const color = k->color() & StructLayout::kMaxColor;
    auto const& entry = s_hashTableSet[sadIn->layoutIndex().raw][color];

    // Perfect hash table miss: k is definitely missing if it's a static string
    // and the color is not duplicate;
    // else, we need to fall back to a hash table lookup for the non-static k.
    if (entry.str != k) {
      if (k->isStatic() && !entry.maybeDup) return kNoMask;
      auto const layout = sadIn->layout();
      slot = layout->keySlotNonStatic(k);
      return slot == kInvalidSlot ? kNoMask : layout->field(slot).type_mask;
    }

    // Perfect hash table hit: we can use the slot and type bound.
    slot = entry.slot;
    return entry.typeMask;
  }();

  auto const present = slot != kInvalidSlot;
  auto const checked = present && !(static_cast<uint8_t>(v.type()) & type_mask);

  if constexpr (debug) {
    auto const layout = sadIn->layout();
    auto const checked_via_layout = present && layout->checkTypeBound(slot, v);
    always_assert(slot == layout->keySlot(k));
    always_assert(checked == checked_via_layout);
  }

  if (!checked) {
    auto const vad = sadIn->escalateWithCapacity(sadIn->size() + 1, __func__);
    auto const res = VanillaDict::SetStrMove(vad, k, v);
    assertx(vad == res);
    if (sadIn->decReleaseCheck()) Release(sadIn);
    return res;
  }
  return SetStrInSlot(sadIn, slot, v);
}

ArrayData* StructDict::SetStrInSlot(StructDict* sadIn, Slot slot,
                                    TypedValue v) {
  assertx(slot != kInvalidSlot);
  assertx(slot < sadIn->numFields());
  assertx(sadIn->layout()->checkTypeBound(slot, v));
  auto const cow = sadIn->cowCheck();
  auto const sad = cow ? sadIn->copy() : sadIn;
  StructDict::SetStrInSlotInPlace(sad, slot, v);
  if (cow) sadIn->decRefCount();
  return sad;
}

void StructDict::SetStrInSlotInPlace(StructDict* sad, Slot slot,
                                     TypedValue v) {
  assertx(sad->hasExactlyOneRef());
  assertx(sad->layout()->checkTypeBound(slot, v));
  auto const lval = sad->lvalUnchecked(slot);
  if (lval.type() == KindOfUninit) {
    sad->addNextSlot(slot);
  } else {
    tvDecRefGen(*lval);
  }
  lval.type() = type(v);
  lval.val() = val(v);
}

NEVER_INLINE
StructDict* StructDict::copy() const {
  auto const sizeIdx = sizeIndex();
  auto const sad = static_cast<StructDict*>(tl_heap->objMallocIndex(sizeIdx));
  auto const heapSize = MemoryManager::sizeIndex2Size(sizeIdx);
  assertx(heapSize % 16 == 0);
  memcpy16_inline(sad, this, heapSize);
  auto const aux = packSizeIndexAndAuxBits(sizeIdx, auxBits());
  sad->initHeader_16(HeaderKind::BespokeDict, OneReference, aux);
  if (sad->mayContainCounted()) sad->incRefValues();
  return sad;
}

void StructDict::incRefValues() {
  auto numFields = this->numFields();
  for (auto i = 0; i < numFields; i++) {
    auto const rval = rvalUnchecked(i);
    tvIncRefGen(rval.tv());
  }
}

void StructDict::decRefValues() {
  auto numFields = this->numFields();
  for (auto i = 0; i < numFields; i++) {
    auto const rval = rvalUnchecked(i);
    tvDecRefGen(rval.tv());
  }
}

ArrayData* StructDict::RemoveIntMove(StructDict* sad, int64_t) {
  return sad;
}

ArrayData* StructDict::RemoveStrMove(StructDict* sadIn, const StringData* k) {
  auto const slot = StructLayout::keySlot(sadIn->layoutIndex(), k);
  if (slot == kInvalidSlot) return sadIn;
  if (sadIn->layout()->field(slot).required) {
    auto const vad = sadIn->escalateWithCapacity(sadIn->size(), __func__);
    if (sadIn->decReleaseCheck()) Release(sadIn);
    return VanillaDict::RemoveStrMove(vad, k);
  }
  return RemoveStrInSlot(sadIn, slot);
}

ArrayData* StructDict::RemoveStrInSlot(StructDict* sadIn, Slot slot) {
  assertx(!sadIn->layout()->field(slot).required);
  auto const currType = sadIn->rvalUnchecked(slot).type();
  if (currType == KindOfUninit) return sadIn;

  auto const sad = [&] {
    if (!sadIn->cowCheck()) return sadIn;
    auto const result = sadIn->copy();
    sadIn->decRefCount();
    return result;
  }();
  auto const lval = sad->lvalUnchecked(slot);
  tvDecRefGen(lval.tv());
  lval.type() = KindOfUninit;
  sad->isBigStruct() ?
    sad->removeSlot<uint16_t>(slot) : sad->removeSlot<uint8_t>(slot);

  return sad;
}

ArrayData* StructDict::AppendMove(StructDict* sad, TypedValue v) {
  auto const vad = sad->escalateWithCapacity(sad->size() + 1, __func__);
  auto const res = VanillaDict::AppendMove(vad, v);
  assertx(vad == res);
  if (sad->decReleaseCheck()) Release(sad);
  return res;
}

ArrayData* StructDict::PopMove(StructDict* sadIn, Variant& value) {
  if (UNLIKELY(sadIn->size() == 0)) {
    value = uninit_null();
    return sadIn;
  }

  auto const sad = [&] {
    if (!sadIn->cowCheck()) return sadIn;
    auto const res = sadIn->copy();
    sadIn->decRefCount();
    return res;
  }();

  auto const pos = sad->size() - 1;
  auto const slot = sad->isBigStruct() ?
    sad->getSlotInPos<uint16_t>(pos) : sad->getSlotInPos<uint8_t>(pos);
  auto const lval = sad->lvalUnchecked(slot);
  value = Variant::attach(lval.tv());
  lval.type() = KindOfUninit;
  sad->m_size--;
  return sad;
}

ArrayData* StructDict::PreSort(StructDict* sad, SortFunction sf) {
  return sad->escalateWithCapacity(sad->size(), sortFunctionName(sf));
}

ArrayData* StructDict::PostSort(StructDict* sad, ArrayData* vad) {
  auto const result = MakeFromVanilla(vad, sad->layout());
  if (!result) return vad;
  VanillaDict::Release(vad);
  return result;
}

ArrayData* StructDict::SetLegacyArray(StructDict* sadIn,
                                       bool copy, bool legacy) {
  auto const sad = copy ? sadIn->copy() : sadIn;
  sad->setLegacyArrayInPlace(legacy);
  return sad;
}

////////////////////////////////////////////////////////////////////////////

void StructDict::Scan(const StructDict* sad, type_scan::Scanner& scanner) {
  for (Slot i = 0; i < sad->numFields(); i++) {
    auto const rval = sad->rvalUnchecked(i);
    if (isRefcountedType(rval.type()) && rval.val().pcnt->isRefCounted()) {
      scanner.scan(rval.val().pcnt);
    }
  }
}

ArrayData* StructDict::EscalateToVanilla(const StructDict* sad,
                                         const char* reason) {
  return sad->escalateWithCapacity(sad->size(), reason);
}

void StructDict::addNextSlot(Slot slot) {
  assertx(slot < StructLayout::maxNumKeys());
  isBigStruct() ?
    static_cast<uint16_t*>(rawPositions())[m_size++] = slot :
    static_cast<uint8_t*>(rawPositions())[m_size++] = slot;
}

template<typename PosType>
void StructDict::removeSlot(Slot slot) {
  auto const pos = static_cast<PosType*>(rawPositions());
  auto idx = 0;
  for (size_t i = 0; i < m_size; i++) {
    auto const curr = pos[i];
    if (curr == slot) continue;
    pos[idx++] = curr;
  }
  m_size--;
}

template<typename PosType>
Slot StructDict::getSlotInPos(size_t pos) const {
  assertx(pos < m_size);
  assertx(pos < StructLayout::maxNumKeys());
  return static_cast<const PosType*>(rawPositions())[pos];
}

//////////////////////////////////////////////////////////////////////////////

TopStructLayout::TopStructLayout()
  : AbstractLayout(Index(), "StructDict<Top>",
                   {AbstractLayout::GetBespokeTopIndex()}, structDictVtable())
{
  assertx(!s_hashTableSet);
  // There are 14 bits available for StructDict indices. See layout.h.
  assertx(Cfg::Eval::BespokeMaxNumStructLayouts <= 1 << 14);
  auto colorTableLen = indexRaw(Cfg::Eval::BespokeMaxNumStructLayouts);
  s_hashTableSet = (StructLayout::PerfectHashTable*) vm_malloc(
      sizeof(StructLayout::PerfectHashTable) * colorTableLen);
}

LayoutIndex TopStructLayout::Index() {
  return StructLayout::Index(0);
}

//////////////////////////////////////////////////////////////////////////////

using namespace jit;

ArrayLayout StructLayout::appendType(Type /*val*/) const {
  return ArrayLayout::Vanilla();
}

ArrayLayout StructLayout::removeType(Type key) const {
  if (!key.hasConstVal(TStr)) return ArrayLayout::Top();
  auto const slot = keySlot(key.strVal());
  if (slot == kInvalidSlot) return ArrayLayout(this);
  auto const& f = field(slot);
  return f.required ? ArrayLayout::Vanilla() : ArrayLayout(this);
}

ArrayLayout StructLayout::setType(Type key, Type val) const {
  if (key <= TInt) return ArrayLayout::Vanilla();
  if (!key.hasConstVal(TStr)) return ArrayLayout::Top();
  auto const slot = keySlotStatic(index(), key.strVal());
  if (slot == kInvalidSlot) return ArrayLayout::Vanilla();
  if (val <= getTypeBound(slot)) return ArrayLayout(this);
  if (!val.maybe(getTypeBound(slot))) return ArrayLayout::Vanilla();
  return ArrayLayout::Top();
}

std::pair<Type, bool> StructLayout::elemType(Type key) const {
  if (key <= TInt) return {TBottom, false};
  if (!key.hasConstVal(TStr)) return {getUnionTypeBound(), false};
  auto const slot = keySlotStatic(index(), key.strVal());
  return slot == kInvalidSlot
    ? std::pair{TBottom, false}
    : std::pair{getTypeBound(slot), field(slot).required};
}

bool StructLayout::slotAlwaysPresent(const Type& slot) const {
  assertx(slot <= TInt);

  if (slot.hasConstVal()) {
    auto const idx = slot.intVal();
    if (idx < 0 || idx >= numFields()) return true;
    return field(idx).required;
  }

  for (auto slot = 0; slot < numFields(); slot++) {
    if (!field(slot).required) return false;
  }
  return true;
}

std::pair<Type, bool> StructLayout::firstLastType(
    bool /*isFirst*/, bool isKey) const {
  return {isKey ? TStaticStr : getUnionTypeBound(), false};
}

Type StructLayout::getTypeBound(Type slot) const {
  assertx(slot <= TInt);
  if (!slot.hasConstVal()) return getUnionTypeBound();
  auto const slotVal = slot.intVal();
  if (slotVal < 0 || slotVal >= numFields()) return TBottom;
  return getTypeBound(slotVal);
}

Type StructLayout::iterPosType(Type /*pos*/, bool isKey) const {
  return isKey ? TStaticStr : getUnionTypeBound();
}

Optional<int64_t> StructLayout::numElements() const {
  for (auto slot = 0; slot < numFields(); slot++) {
    if (!field(slot).required) return std::nullopt;
  }
  return numFields();
}

ArrayLayout TopStructLayout::appendType(Type /*val*/) const {
  return ArrayLayout::Vanilla();
}

ArrayLayout TopStructLayout::removeType(Type /*key*/) const {
  return ArrayLayout::Top();
}

ArrayLayout TopStructLayout::setType(Type /*key*/, Type /*val*/) const {
  return ArrayLayout::Top();
}

std::pair<Type, bool> TopStructLayout::elemType(Type key) const {
  return key <= TInt ? std::pair{TBottom, false}
                     : std::pair{TInitCell, false};
}

bool TopStructLayout::slotAlwaysPresent(const Type&) const {
  return false;
}

std::pair<Type, bool> TopStructLayout::firstLastType(
    bool /*isFirst*/, bool isKey) const {
  return {isKey ? TStaticStr : TInitCell, false};
}

Type TopStructLayout::iterPosType(Type /*pos*/, bool isKey) const {
  return isKey ? TStaticStr : TInitCell;
}

Type TopStructLayout::getTypeBound(Type slot) const {
  assertx(slot <= TInt);
  return TInitCell;
}

//////////////////////////////////////////////////////////////////////////////

}
