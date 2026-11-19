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

#include "hphp/runtime/base/bespoke/type-structure.h"

#include "hphp/runtime/base/bespoke/escalation-logging.h"
#include "hphp/runtime/base/memory-manager.h"
#include "hphp/runtime/base/static-string-table.h"
#include "hphp/runtime/base/string-data.h"
#include "hphp/runtime/base/type-structure-helpers-defs.h"

#include "hphp/runtime/vm/jit/type.h"

namespace HPHP::bespoke {

struct StaticString;

///////////////////////////////////////////////////////////////////////////////

TypeStructure* TypeStructure::As(ArrayData* ad) {
  auto const result = reinterpret_cast<TypeStructure*>(ad);
  assertx(result->checkInvariants());
  return result;
}

const TypeStructure* TypeStructure::As(const ArrayData* ad) {
  return As(const_cast<ArrayData*>(ad));
}

namespace {

size_t getSizeOfTypeStruct(Kind kind) {
  switch (kind) {
#define X(Field, Type) \
    case Kind::T_##Field: return sizeof(Type);
TYPE_STRUCTURE_KINDS
TYPE_STRUCTURE_CHILDREN_KINDS
#undef X
    default: return 0;
  }
}

void copyFieldToVanilla(ArrayData* vad, StringData* key, TypedValue tv) {
  auto const type = tv.m_type;
  if (type == KindOfUninit) return;
  VanillaDict::SetStrMove(vad, key, tv);
  tvIncRefGen(tv);
}

void setStringField(StringData*& field, TypedValue v) {
  assertx(tvIsString(v));
  if (field) field->decRefAndRelease();
  field = val(v).pstr;
}

void setTypeField(ArrayData*& field, TypedValue v, bool nested) {
  assertx(tvIsArrayLike(v));
  if (field) field->decRefAndRelease();
  auto arr = val(v).parr;
  if (!nested) {
    field = arr;
    return;
  }
  field = TypeStructure::MakeFromVanillaNested(arr);
  if (field != arr) tvDecRefGen(v);
}

void setVecField(ArrayData*& field, TypedValue v, bool nested) {
  assertx(tvIsVec(v));
  if (field) field->decRefAndRelease();
  if (!nested) {
    field = val(v).parr;
    return;
  }
  auto arr = val(v).parr;
  auto const size = arr->size();
  auto newarr = Array::CreateVec();
  for (size_t i = 0; i < size; ++i) {
    auto val = arr->getValue(i).asCArrRef();
    auto const r = TypeStructure::MakeFromVanillaNested(val.get());
    newarr.append(Variant(r));
  }
  field = newarr.detach();
  tvDecRefGen(v);
}

void setDictField(ArrayData*& field, TypedValue v, bool nested) {
  assertx(tvIsDict(v));
  if (field) field->decRefAndRelease();
  if (!nested) {
    field = val(v).parr;
    return;
  }
  auto arr = val(v).parr;
  auto const size = arr->size();
  auto newarr = Array::CreateDict();
  for (size_t i = 0; i < size; i++) {
    Variant key = arr->getKey(i);
    auto val = arr->getValue(i).asCArrRef();
    auto const r = TypeStructure::MakeFromVanillaNested(val.get());
    newarr.set(key, Variant(r));
  }
  field = newarr.detach();
  tvDecRefGen(v);
}

TypedValue make_tv_safe(uint8_t val) {
  return make_tv<KindOfInt64>(val);
}
TypedValue make_tv_safe(bool val) {
  // to match the current behaviour of type structures
  return val ? make_tv<KindOfBoolean>(true) : make_tv<KindOfUninit>();
}
TypedValue make_tv_safe(StringData* val) {
  if (val == nullptr) return make_tv<KindOfUninit>();
  return make_tv<KindOfString>(val);
}
TypedValue make_tv_safe(ArrayData* val) {
  if (val == nullptr) return make_tv<KindOfUninit>();
  if (val->isDictType()) {
    return make_tv<KindOfDict>(val);
  }
  assertx(val->isVecType());
  return make_tv<KindOfVec>(val);
}

bool checkBespokeArray(const ArrayData* ad) {
  auto const size = ad->size();
  for (size_t i = 0; i < size; ++i) {
    auto const val = ad->getValue(i).asCArrRef();
    if (!TypeStructure::isFullyBespokeTypeStructure(val.get())) return false;
  }
  return true;
}

} // namespace

size_t TypeStructure::sizeIndex(Kind kind) {
  return MemoryManager::size2Index(getSizeOfTypeStruct(kind));
}

LayoutIndex TypeStructure::GetLayoutIndex() {
  return LayoutIndex{uint16_t(kTypeStructureLayoutByte << 8)};
}

bool TypeStructure::isBespokeTypeStructure(const ArrayData* ad) {
  return !ad->isVanilla() && asBespoke(ad)->layoutIndex() == GetLayoutIndex();
}

bool TypeStructure::isFullyBespokeTypeStructure(const ArrayData* ad) {
  if (!isBespokeTypeStructure(ad)) return false;
  auto const ts = As(ad);

  // check all nested fields that could contain type structures
  if (ts->m_typevar_types && !checkBespokeArray(ts->m_typevar_types)) {
    return false;
  }
  switch (ts->typeKind()) {
#define X(Name, Type) \
  case Kind::T_##Name: \
    reinterpret_cast<const Type*>(ts)->checkBespokeChildren(); \
    break;
TYPE_STRUCTURE_CHILDREN_KINDS
#undef X
    default:
      break;
  }
  return true;
}

void TypeStructure::InitializeLayouts() {
  new TypeStructureLayout();
}

template <bool Static>
TypeStructure* TypeStructure::MakeReserve(bool legacy, Kind kind) {
  auto const index = sizeIndex(kind);
  auto const alloc = [&]{
    if (!Static) return tl_heap->objMallocIndex(index);
    auto const size = MemoryManager::sizeIndex2Size(index);
    return ArrayData::AllocStatic(size);
  }();

  auto const tad = static_cast<TypeStructure*>(alloc);
  auto const aux = packSizeIndexAndAuxBits(
    index, legacy ? ArrayData::kLegacyArray : 0);
  tad->initHeader_16(HeaderKind::BespokeDict,
    Static ? StaticValue : OneReference, aux);

  // need to initialize this to actual size in MakeFromVanilla
  tad->m_size = 0;

  tad->m_layout_index = GetLayoutIndex();
  tad->m_kind = uint8_t(kind);
  tad->m_extra_hi8 = TypeStructure::kFieldsByte;
  tad->m_extra_lo8 = 0;
  tad->m_raw_positions = 0;
  tad->m_alias = nullptr;
  tad->m_typevars = nullptr;
  tad->m_typevar_types = nullptr;

  switch (kind) {
#define X(Name, Type) \
  case Kind::T_##Name: \
    tad->m_extra_hi8 = Type::kFieldsByte; \
    Type::initializeFields(reinterpret_cast<Type*>(tad)); \
    break;
TYPE_STRUCTURE_CHILDREN_KINDS
#undef X
    default:
      break;
  }

  return tad;
}

template <typename Type>
size_t setFields(Type* ts, ArrayData* ad, bool nested) {
  auto i = 0;
  VanillaDict::IterateKV(VanillaDict::as(ad), [&](auto k, auto v) {
    assertx(tvIsString(k));
    auto const fieldKey = val(k).pstr;
    if (Type::setField(ts, fieldKey, v, nested)) {
      ts->setIterationPosition(fieldKey, i);
      tvIncRefGen(v);
      i++;
    }
  });
  return i;
}

void TypeStructure::setBitField(TypedValue v, BitFieldOffsets offset) {
  assertx(tvIsBool(v));
  uint8_t val = v.val().num;
  clearBitField(offset);
  m_extra_lo8 |= val << offset;
}

// caller must incref TypedValue
bool TypeStructure::setField(
    TypeStructure* tad, StringData* k, TypedValue v, bool nested) {
  if (s_kind.same(k)) {
    assertx(tvIsInt(v));
    tad->m_kind = safe_cast<uint8_t>(val(v).num);
  } else if (s_nullable.same(k)) {
    tad->setBitField(v, kNullableOffset);
  } else if (s_soft.same(k)) {
    tad->setBitField(v, kSoftOffset);
  } else if (s_opaque.same(k)) {
    tad->setBitField(v, kOpaqueOffset);
  } else if (s_optional_shape_field.same(k)) {
    tad->setBitField(v, kOptionalShapeFieldOffset);
  } else if (s_alias.same(k)) {
    setStringField(tad->m_alias, v);
  } else if (s_typevars.same(k)) {
    setStringField(tad->m_typevars, v);
  } else if (s_typevar_types.same(k)) {
    setDictField(tad->m_typevar_types, v, nested);
  } else {
    return false;
  }
  return true;
}

int8_t TypeStructure::getPositionValueFromField(StringData* field) {
#define X(Field, FieldString, KindOfType, Pos) \
  if (s_##FieldString.same(field)) return Pos;
TYPE_STRUCTURE_FIELDS
#undef X
#define X(Field, Type, KindOfType, Struct, FieldsByteOffset, Pos) \
  if (s_##Field.same(field)) return Pos;
TYPE_STRUCTURE_CHILDREN_FIELDS
#undef X
  return 0;
}

TypeStructure* TypeStructure::MakeFromVanilla(
    ArrayData* ad, bool forceNonStatic) {
  if (!ad->isVanillaDict() || !ad->exists(s_kind)) return nullptr;
  auto const kind =
    static_cast<Kind>(Variant::wrap(ad->get(s_kind)).toInt64Val());

  auto const result = (!forceNonStatic && ad->isStatic())
    ? MakeReserve<true>(ad->isLegacyArray(), kind)
    : MakeReserve<false>(ad->isLegacyArray(), kind);

  auto size = 0;

  switch (kind) {
#define X(Name, Type) \
  case Kind::T_##Name:
TYPE_STRUCTURE_KINDS
#undef X
    size = setFields<TypeStructure>(result, ad, false); \
    break;
#define X(Name, Type) \
  case Kind::T_##Name: \
    size = setFields<Type>(reinterpret_cast<Type*>(result), ad, false); \
    break;
TYPE_STRUCTURE_CHILDREN_KINDS
#undef X
    default:
      break;
  }

  assertx(ad->size() == size);
  result->m_size = size;

  assertx(result->checkInvariants());
  return result;
}

TypeStructure* TypeStructure::MakeFromVanillaStatic(ArrayData* ad, bool nested) {
  // must create a non-static bespoke array first so that GetScalarArray won't
  // short-circuit on isStatic and will dedup properly
  if (nested) {
    ArrayData* ts = MakeFromVanillaNested(ad);
    GetScalarArray(&ts);
    return As(ts);
  }
  ArrayData* ts = MakeFromVanilla(ad, true);
  if (ts) {
    GetScalarArray(&ts);
    return As(ts);
  }
  return nullptr;
}

// return ad instead of returning nullptr
ArrayData* TypeStructure::MakeFromVanillaNested(ArrayData* ad) {
  if (!isValidTypeStructure(ad)) return ad;

  auto const kind =
    static_cast<Kind>(Variant::wrap(ad->get(s_kind)).toInt64Val());

  // always make non-static
  auto result = MakeReserve<false>(ad->isLegacyArray(), kind);
  auto size = 0;

  switch (kind) {
#define X(Name, Type) \
  case Kind::T_##Name:
TYPE_STRUCTURE_KINDS
#undef X
    size = setFields<TypeStructure>(result, ad, true); \
    break;
#define X(Name, Type) \
  case Kind::T_##Name: \
    size = setFields<Type>(reinterpret_cast<Type*>(result), ad, true); \
    break;
TYPE_STRUCTURE_CHILDREN_KINDS
#undef X
    default:
      break;
  }

  assertx(ad->size() == size);
  result->m_size = size;

  assertx(result->checkInvariants());
  return result;
}

bool TypeStructure::checkInvariants() const {
  assertx(0 <= kind() && kind() <= HPHP::TypeStructure::kMaxResolvedKind);
  assertx(m_alias == nullptr || m_alias->kindIsValid());
  assertx(m_typevars == nullptr || m_typevars->kindIsValid());
  assertx(m_typevar_types == nullptr || m_typevar_types->kindIsValid());

  switch (typeKind()) {
#define X(Name, Type) \
  case Kind::T_##Name: \
    return reinterpret_cast<const Type*>(this)->checkInvariants();
TYPE_STRUCTURE_CHILDREN_KINDS
#undef X
#define X(Name, Type) \
  case Kind::T_##Name: \
    assertx(fieldsByte() == TypeStructure::kFieldsByte); \
    return true;
TYPE_STRUCTURE_KINDS
#undef X
    default:
      return false;
  }
}

bool TypeStructure::isValidTypeStructure(const ArrayData* ad) {
  if (!ad->isVanillaDict() || !ad->exists(s_kind)) return false;
  auto const kind =
    static_cast<Kind>(Variant::wrap(ad->get(s_kind)).toInt64Val());

  // check that the key exists and that the type matches
  auto fields = 0;
#define X(Field, FieldString, KindOfType, ...)                        \
  if (ad->exists(s_##FieldString) &&                                  \
      equivDataTypes(ad->get(s_##FieldString).type(), KindOfType)) {  \
    fields++;                                                         \
  }
TYPE_STRUCTURE_FIELDS
#undef X

  switch (kind) {
#define X(Name, Type) \
  case Kind::T_##Name: \
    fields += Type::countFields(ad); \
    break;
TYPE_STRUCTURE_CHILDREN_KINDS
#undef X
#define X(Name, Type) \
  case Kind::T_##Name: \
    break;
TYPE_STRUCTURE_KINDS
#undef X
  default: \
    return false;
  }

  // all elements in ad must exist in type structure
  return fields == ad->size();
}

ArrayData* TypeStructure::escalateWithCapacity(
    size_t capacity, const char* reason) const {
  assertx(capacity >= size());
  logEscalateToVanilla(this, reason);

  auto ad = VanillaDict::MakeReserveDict(capacity);
  ad->setLegacyArrayInPlace(isLegacyArray());

  for (auto i = 0; i < m_size; i++) {
    auto const keyTV = GetPosKey(this, i);
    auto const valTV = GetPosVal(this, i);
    copyFieldToVanilla(ad, keyTV.val().pstr, valTV);
  }

  assertx(isValidTypeStructure(ad));
  return ad;
}

template <bool Static>
TypeStructure* TypeStructure::copy() const {
  auto const sizeIdx = sizeIndex(typeKind());
  auto const heapSize = MemoryManager::sizeIndex2Size(sizeIdx);
  auto const mem = [&]{
    if (!Static) return tl_heap->objMallocIndex(sizeIdx);
    return ArrayData::AllocStatic(heapSize);
  }();
  auto const ad = static_cast<TypeStructure*>(mem);

  assertx(heapSize % 16 == 0);
  memcpy16_inline(ad, this, heapSize);

  auto const aux = packSizeIndexAndAuxBits(sizeIdx, auxBits());
  ad->initHeader_16(HeaderKind::BespokeDict, Static ? StaticValue : OneReference, aux);
  if (!Static) ad->incRefFields();

  return ad;
}

// explicitly declare to use in runtime tests
template TypeStructure* TypeStructure::copy<true>() const;
template TypeStructure* TypeStructure::copy<false>() const;

void TypeStructure::Scan(
    const TypeStructure* tad, type_scan::Scanner& scanner) {
  scanner.scan(tad->m_alias);
  scanner.scan(tad->m_typevars);
  scanner.scan(tad->m_typevar_types);

  switch (tad->typeKind()) {
#define X(Name, Type) \
  case Kind::T_##Name: \
    Type::scan(reinterpret_cast<const Type*>(tad), scanner); \
    break;
TYPE_STRUCTURE_CHILDREN_KINDS
#undef X
    default:
      break;
  }
}

void TypeStructure::incRefFields() {
  if (m_alias) m_alias->incRefCount();
  if (m_typevars) m_typevars->incRefCount();
  if (m_typevar_types) m_typevar_types->incRefCount();

  switch (typeKind()) {
#define X(Name, Type) \
    case Kind::T_##Name: \
      reinterpret_cast<Type*>(this)->incRefFields(); \
      break;
TYPE_STRUCTURE_CHILDREN_KINDS
#undef X
    default:
      break;
  }
}

void TypeStructure::decRefFields() {
  if (m_alias) m_alias->decRefAndRelease();
  if (m_typevars) m_typevars->decRefAndRelease();
  if (m_typevar_types) m_typevar_types->decRefAndRelease();

  switch (typeKind()) {
#define X(Name, Type) \
    case Kind::T_##Name: \
      reinterpret_cast<Type*>(this)->decRefFields(); \
      break;
TYPE_STRUCTURE_CHILDREN_KINDS
#undef X
    default:
      break;
  }
}

bool TypeStructure::containsField(const StringData* k) const {
#define X(Field, FieldString, KindOfType, ...) \
  if (s_##FieldString.same(k)) return true;
TYPE_STRUCTURE_FIELDS
#undef X

  switch (typeKind()) {
#define X(Name, Type) \
    case Kind::T_##Name: \
      return reinterpret_cast<const Type*>(this)->containsField(k); \
TYPE_STRUCTURE_CHILDREN_KINDS
#undef X
    default:
      return false;
  }
}

std::pair<DataType, uint8_t> TypeStructure::getFieldPair(const StringData* k) {
  // fields in base TypeStructure always exist and are represented by offset 0
#define X(Field, FieldString, KindOfType, ...) \
  if (s_##FieldString.same(k)) return {KindOfType, 0};
  TYPE_STRUCTURE_FIELDS
#undef X

#define X(Field, Type, KindOfType, Struct, FieldsByteOffset, ...) \
  if (s_##Field.same(k)) return {KindOfType, FieldsByteOffset};
  TYPE_STRUCTURE_CHILDREN_FIELDS
#undef X

  return {KindOfUninit, 0};
}

size_t TypeStructure::getFieldOffset(const StringData* key) {
  if (s_kind.same(key)) {
    return kindOffset();
  } else if (s_nullable.same(key)) {
    return bitFieldOffset();
  } else if (s_soft.same(key)) {
    return bitFieldOffset();
  } else if (s_opaque.same(key)) {
    return bitFieldOffset();
  } else if (s_optional_shape_field.same(key)) {
    return bitFieldOffset();
  } else if (s_alias.same(key)) {
    return offsetof(TypeStructure, m_alias);
  } else if (s_typevars.same(key)) {
    return offsetof(TypeStructure, m_typevars);
  } else if (s_typevar_types.same(key)) {
    return offsetof(TypeStructure, m_typevar_types);
  }

#define X(Field, Type, KindOfType, Struct, ...) \
  if (s_##Field.same(key)) return Struct::getFieldOffset(key);
    TYPE_STRUCTURE_CHILDREN_FIELDS
#undef X
  always_assert(false);
}

uint8_t TypeStructure::getBooleanBitOffset(const StringData* key) {
  if (s_nullable.same(key)) return kNullableOffset;
  if (s_opaque.same(key)) return kOpaqueOffset;
  if (s_optional_shape_field.same(key)) return kOptionalShapeFieldOffset;
  // otherwise the boolean field does not require an offset
  return 0;
}

//////////////////////////////////////////////////////////////////////////////
// ArrayData interface

ArrayData* TypeStructure::EscalateToVanilla(
    const TypeStructure* tad, const char* reason) {
  return tad->escalateWithCapacity(tad->size(), reason);
}

void TypeStructure::ConvertToShared(
    TypeStructure* tad, const MakeSharedEnv& env) {
  if (tad->m_alias) MakeSharedString(tad->m_alias, env);
  if (tad->m_typevars) MakeSharedString(tad->m_typevars, env);
  if (tad->m_typevar_types) MakeSharedArray(tad->m_typevar_types, env);

  switch (tad->typeKind()) {
#define X(Name, Type) \
  case Kind::T_##Name: \
    Type::convertToShared(reinterpret_cast<Type*>(tad), env); \
    break;
TYPE_STRUCTURE_CHILDREN_KINDS
#undef X
    default:
      break;
  }
}

void TypeStructure::ReleaseShared(TypeStructure* tad) {
  if (tad->m_alias) DecRefSharedString(tad->m_alias);
  if (tad->m_typevars) DecRefSharedString(tad->m_typevars);
  if (tad->m_typevar_types) DecRefSharedArray(tad->m_typevar_types);

  switch (tad->typeKind()) {
#define X(Name, Type) \
  case Kind::T_##Name: \
    Type::releaseShared(reinterpret_cast<Type*>(tad)); \
    break;
TYPE_STRUCTURE_CHILDREN_KINDS
#undef X
    default:
      break;
  }
}

void TypeStructure::Release(TypeStructure* tad) {
  tad->fixCountForRelease();
  assertx(tad->isRefCounted());
  assertx(tad->hasExactlyOneRef());
  tad->decRefFields();
  tl_heap->objFreeIndex(tad, sizeIndex(tad->typeKind()));
}

bool TypeStructure::IsVectorData(const TypeStructure*) {
  return false;
}

void TypeStructure::OnSetEvalScalar(TypeStructure* tad) {
  if (tad->m_alias && !tad->m_alias->isStatic()) {
    auto field = tad->m_alias;
    tad->m_alias = makeStaticString(field);
    decRefStr(field);
  }
  if (tad->m_typevars && !tad->m_typevars->isStatic()) {
    auto field = tad->m_typevars;
    tad->m_typevars = makeStaticString(field);
    decRefStr(field);
  }
  if (tad->m_typevar_types && !tad->m_typevar_types->isStatic()) {
    auto arr = Array(tad->m_typevar_types);
    arr.setEvalScalar();
    decRefArr(tad->m_typevar_types);
    tad->m_typevar_types = arr.detach();
  }

  switch (tad->typeKind()) {
#define X(Name, Type) \
  case Kind::T_##Name: \
    Type::onSetEvalScalar(reinterpret_cast<Type*>(tad)); \
    break;
TYPE_STRUCTURE_CHILDREN_KINDS
#undef X
    default:
      break;
  }
}

ArrayData* TypeStructure::Copy(const TypeStructure* tad) {
  return tad->copy<false>();
}

ArrayData* TypeStructure::CopyStatic(const TypeStructure* tad) {
  return tad->copy<true>();
}

TypedValue TypeStructure::NvGetInt(const TypeStructure*, int64_t) {
  return make_tv<KindOfUninit>();
}

TypedValue TypeStructure::NvGetStr(
    const TypeStructure* tad, const StringData* k) {
  auto const tv = TypeStructure::tsNvGetStr(tad, k);
  if (tv.is_init()) return tv;

  // at this point, either k exists in a children struct or does not exist at all
  switch (tad->typeKind()) {
#define X(Name, Type) \
  case Kind::T_##Name: return Type::tsNvGetStr(reinterpret_cast<const Type*>(tad), k);
TYPE_STRUCTURE_CHILDREN_KINDS
#undef X
    default: break;
  }
  return make_tv<KindOfUninit>();
}

TypedValue TypeStructure::tsNvGetStr(
    const TypeStructure* tad, const StringData* k) {
#define X(Field, FieldString, ...)                                    \
  if (s_##FieldString.same(k)) {                                      \
    return make_tv_safe(tad->Field());                                \
  }
TYPE_STRUCTURE_FIELDS
#undef X
  return make_tv<KindOfUninit>();
}

TypedValue TypeStructure::GetPosKey(const TypeStructure* tad, ssize_t pos) {
  auto const fieldPosVal = tad->getPositionValue(pos);
  if (fieldPosVal == 0) return make_tv<KindOfUninit>();

  if (fieldPosVal <= TypeStructure::kMaxPositionValue) {
    switch (fieldPosVal) {
#define X(Field, FieldString, KindOfType, Value) \
      case Value: \
        return make_tv<KindOfString>(s_##FieldString.get());
TYPE_STRUCTURE_FIELDS
#undef X
    }
  }

  switch (tad->typeKind()) {
#define X(Name, Type) \
    case Kind::T_##Name: \
      return Type::getKeyFromPositionValue(reinterpret_cast<const Type*>(tad), fieldPosVal);
TYPE_STRUCTURE_CHILDREN_KINDS
#undef X
    default: break;
  }
  return make_tv<KindOfUninit>();
}

TypedValue TypeStructure::GetPosVal(const TypeStructure* tad, ssize_t pos) {
  auto const fieldPosVal = tad->getPositionValue(pos);
  if (fieldPosVal == 0) return make_tv<KindOfUninit>();

  if (fieldPosVal <= TypeStructure::kMaxPositionValue) {
    switch (fieldPosVal) {
  #define X(Field, FieldString, KindOfType, Value) \
      case Value: \
        return make_tv_safe(tad->Field());
  TYPE_STRUCTURE_FIELDS
  #undef X
      default: break;
    }
  }

  switch (tad->typeKind()) {
#define X(Name, Type) \
    case Kind::T_##Name: \
      return Type::getValFromPositionValue(reinterpret_cast<const Type*>(tad), fieldPosVal);
TYPE_STRUCTURE_CHILDREN_KINDS
#undef X
    default: break;
  }
  return make_tv<KindOfUninit>();
}

bool TypeStructure::PosIsValid(const TypeStructure* tad, ssize_t pos) {
  return 0 <= pos && pos < tad->size();
}

ssize_t TypeStructure::IterBegin(const TypeStructure*) {
  return 0;
}
ssize_t TypeStructure::IterLast(const TypeStructure* tad) {
  return tad->size() - 1;
}
ssize_t TypeStructure::IterEnd(const TypeStructure* tad) {
  return tad->size();
}
ssize_t TypeStructure::IterAdvance(const TypeStructure* tad, ssize_t pos) {
  return pos < tad->size() ? pos + 1 : pos;
}
ssize_t TypeStructure::IterRewind(const TypeStructure* tad, ssize_t pos) {
  return pos > 0 ? pos - 1 : tad->size();
}

arr_lval TypeStructure::LvalInt(TypeStructure* tad, int64_t k) {
  throwOOBArrayKeyException(k, tad);
}

// type structure shouldn't return a reference that's able to be mutated,
// so escalate
arr_lval TypeStructure::LvalStr(TypeStructure* tad, StringData* k) {
  auto const vad =
    tad->escalateWithCapacity(tad->size(), __func__);
  assert(vad != nullptr);
  auto const result = vad->lval(k);
  assertx(result.arr == vad);
  return result;
}

tv_lval TypeStructure::ElemInt(tv_lval lval, int64_t k, bool throwOnMissing) {
  if (throwOnMissing) throwOOBArrayKeyException(k, lval.val().parr);
  return const_cast<TypedValue*>(&immutable_null_base);
}

// type structure shouldn't return a reference that's able to be mutated,
// so escalate
tv_lval TypeStructure::ElemStr(
    tv_lval lvalIn, StringData* k, bool /*throwOnMissing*/) {
  auto tadIn = As(lvalIn.val().parr);
  auto const lval = TypeStructure::LvalStr(tadIn, k);
  if (lval.arr != tadIn) {
    lvalIn.val().parr = lval.arr;
    if (tadIn->decReleaseCheck()) Release(tadIn);
  }
  return lval;
}

ArrayData* TypeStructure::SetPosMove(
    TypeStructure* tad, ssize_t pos, TypedValue v) {
  auto const vad = tad->escalateWithCapacity(tad->size(), __func__);
  auto const res = VanillaDict::SetPosMove(vad, pos, v);
  assertx(vad == res);
  if (tad->decReleaseCheck()) Release(tad);
  return res;
}

ArrayData* TypeStructure::SetIntMove(
    TypeStructure* tad, int64_t k, TypedValue v) {
  auto const vad = tad->escalateWithCapacity(tad->size() + 1, __func__);
  auto const res = VanillaDict::SetIntMove(vad, k, v);
  assertx(vad == res);
  if (tad->decReleaseCheck()) Release(tad);
  return res;
}

ArrayData* TypeStructure::SetStrMove(
    TypeStructure* tad, StringData* k, TypedValue v) {
  auto const vad = tad->escalateWithCapacity(tad->size() + 1, __func__);
  auto const res = VanillaDict::SetStrMove(vad, k, v);
  if (tad->decReleaseCheck()) Release(tad);
  return res;
}

ArrayData* TypeStructure::RemoveIntMove(TypeStructure* tad, int64_t) {
  return tad;
}

ArrayData* TypeStructure::RemoveStrMove(
    TypeStructure* tad, const StringData* k) {
  if (!tad->containsField(k)) return tad;

  // type structure fields should not be removed, so escalate
  auto const vad = tad->escalateWithCapacity(tad->size(), __func__);
  if (tad->decReleaseCheck()) Release(tad);
  return VanillaDict::RemoveStrMove(vad, k);
}

ArrayData* TypeStructure::AppendMove(TypeStructure* tad, TypedValue v) {
  auto const vad = tad->escalateWithCapacity(tad->size(), __func__);
  auto const res = VanillaDict::AppendMove(vad, v);
  assertx(vad == res);
  if (tad->decReleaseCheck()) Release(tad);
  return res;
}

// type structure fields should not be removed, so escalate
ArrayData* TypeStructure::PopMove(TypeStructure* tad, Variant& value) {
  auto const vad = tad->escalateWithCapacity(tad->size(), __func__);
  if (tad->decReleaseCheck()) Release(tad);
  return VanillaDict::PopMove(vad, value);
}

ArrayData* TypeStructure::PreSort(TypeStructure* tad, SortFunction sf) {
  return tad->escalateWithCapacity(tad->size(), sortFunctionName(sf));
}

ArrayData* TypeStructure::PostSort(TypeStructure*, ArrayData* vad) {
  return vad;
}

ArrayData* TypeStructure::SetLegacyArray(
    TypeStructure* tadIn, bool copy, bool legacy) {
  auto const tad = copy ? tadIn->copy<false>() : tadIn;
  tad->setLegacyArrayInPlace(legacy);
  return tad;
}
//////////////////////////////////////////////////////////////////////////////
// specific TypeStructures

namespace {

template<class T> void tsMakeShared(T, const MakeSharedEnv&) {}
template<>
void tsMakeShared<StringData*>(StringData* field, const MakeSharedEnv& env) {
  if (field) MakeSharedString(field, env);
}
template<>
void tsMakeShared<ArrayData*>(ArrayData* field, const MakeSharedEnv& env) {
  if (field) MakeSharedArray(field, env);
}

template<class T> void tsDecRefShared(T) {}
template<>
void tsDecRefShared<StringData*>(StringData* field) {
  if (field) DecRefSharedString(field);
}
template<>
void tsDecRefShared<ArrayData*>(ArrayData* field) {
  if (field) DecRefSharedArray(field);
}

template<class T> void initializeField(T) {}
template<>
void initializeField<bool&>(bool& field) {
  field = 0;
}
template<>
void initializeField<StringData*&>(StringData*& field) {
  field = nullptr;
}
template<>
void initializeField<ArrayData*&>(ArrayData*& field) {
  field = nullptr;
}

template<class T> void scanField(const T&, type_scan::Scanner&) {}
template<>
void scanField<StringData*>(StringData *const& field, type_scan::Scanner& scanner) {
  scanner.scan(field);
}
template<>
void scanField<ArrayData*>(ArrayData *const& field, type_scan::Scanner& scanner) {
  scanner.scan(field);
}

template<class T> void incRefField(T) {}
template<>
void incRefField<StringData*>(StringData* field) {
  if (field) field->incRefCount();
}
template<>
void incRefField<ArrayData*>(ArrayData* field) {
  if (field) field->incRefCount();
}

template<class T> void decRefField(T) {}
template<>
void decRefField<StringData*>(StringData* field) {
  if (field) field->decRefAndRelease();
}
template<>
void decRefField<ArrayData*>(ArrayData* field) {
  if (field) field->decRefAndRelease();
}

template<class T> void setEvalScalar(T) {}
template<>
void setEvalScalar(StringData*& field) {
  if (field && !field->isStatic()) {
    auto tempField = field;
    field = makeStaticString(tempField);
    decRefStr(tempField);
  }
}
template<>
void setEvalScalar(ArrayData*& field) {
  if (field && !field->isStatic()) {
    auto arr = Array(field);
    arr.setEvalScalar();
    decRefArr(field);
    field = arr.detach();
  }
}

}

#define MAKE_TV_FIELD(Field, ...)                                           \
  if (s_##Field.same(k)) return make_tv_safe(tad->m_##Field);

#define CONTAINS_FIELD(Field, Type, ...)                                    \
  if (s_##Field.same(k)) return true;

#define CONVERT_TO_SHARED(Field, Type, ...)                              \
  tsMakeShared<Type>(tad->m_##Field, env);

#define RELEASE_SHARED(Field, Type, ...)                                 \
  tsDecRefShared<Type>(tad->m_##Field);

#define INITIALIZE_FIELD(Field, Type, ...) initializeField<Type&>(tad->m_##Field);
#define SCAN_FIELD(Field, Type, ...) scanField<Type>(tad->m_##Field, scanner);
#define INC_REF_FIELD(Field, Type, ...) incRefField<Type>(m_##Field);
#define DEC_REF_FIELD(Field, Type, ...) decRefField<Type>(m_##Field);

#define GET_KEY_FROM_POS(Field, Type, KindOfType, Struct, FieldsByteOffset, Pos) \
  if (value == Pos) return make_tv<KindOfString>(s_##Field.get());

#define GET_VAL_FROM_POS(Field, Type, KindOfType, Struct, FieldsByteOffset, Pos) \
  if (value == Pos) return make_tv_safe(tad->m_##Field);

#define GET_FIELD_OFFSET(Field, Type, KindOfType, Struct, ...)              \
  if (s_##Field.same(k)) return offsetof(Struct, m_##Field);

#define COUNT_FIELDS(Field, Type, KindOfType, ...)              \
  if (ad->exists(s_##Field) &&                                  \
      equivDataTypes(ad->get(s_##Field).type(), KindOfType)) {  \
    fields++;                                                         \
  }

#define SET_EVAL_SCALAR(Field, Type, KindOfType, ...) \
  setEvalScalar<Type&>(tad->m_##Field);

#define O(ChildStruct, FieldsMacro)                                         \
TypedValue ChildStruct::tsNvGetStr(const ChildStruct* tad, const StringData* k) { \
  FieldsMacro(MAKE_TV_FIELD)                                                \
  return make_tv<KindOfUninit>();                                           \
}                                                                           \
void ChildStruct::convertToShared(ChildStruct* tad, const MakeSharedEnv& env) { \
  FieldsMacro(CONVERT_TO_SHARED)                                         \
}                                                                           \
void ChildStruct::releaseShared(ChildStruct* tad) {                      \
  FieldsMacro(RELEASE_SHARED)                                            \
}                                                                           \
void ChildStruct::initializeFields(ChildStruct* tad) {                      \
  FieldsMacro(INITIALIZE_FIELD)                                             \
}                                                                           \
void ChildStruct::scan(const ChildStruct* tad, type_scan::Scanner& scanner) { \
  FieldsMacro(SCAN_FIELD)                                                   \
}                                                                           \
void ChildStruct::incRefFields() {                                          \
  FieldsMacro(INC_REF_FIELD)                                                \
}                                                                           \
void ChildStruct::decRefFields() {                                          \
  FieldsMacro(DEC_REF_FIELD)                                                \
}                                                                           \
bool ChildStruct::containsField(const StringData* k) const {                \
  FieldsMacro(CONTAINS_FIELD)                                               \
  return false;                                                             \
}                                                                           \
TypedValue ChildStruct::getKeyFromPositionValue(const ChildStruct* tad, int value) { \
  FieldsMacro(GET_KEY_FROM_POS)                                             \
  return make_tv<KindOfUninit>();                                           \
}                                                                           \
TypedValue ChildStruct::getValFromPositionValue(const ChildStruct* tad, int value) { \
  FieldsMacro(GET_VAL_FROM_POS)                                             \
  return make_tv<KindOfUninit>();                                           \
}                                                                           \
size_t ChildStruct::getFieldOffset(const StringData* k) {                   \
  FieldsMacro(GET_FIELD_OFFSET)                                             \
  return -1;                                                                \
}                                                                           \
int ChildStruct::countFields(const ArrayData* ad) {                         \
  auto fields = 0;                                                          \
  FieldsMacro(COUNT_FIELDS)                                                 \
  return fields;                                                            \
}                                                                           \
void ChildStruct::onSetEvalScalar(ChildStruct* tad) {                       \
  FieldsMacro(SET_EVAL_SCALAR)                                              \
}

O(TSShape, TSSHAPE_FIELDS)
O(TSTuple, TSTUPLE_FIELDS)
O(TSFun, TSFUN_FIELDS)
O(TSTypevar, TSTYPEVAR_FIELDS)
O(TSWithClassishTypes, TSCLASSISH_FIELDS)
O(TSWithGenericTypes, TSGENERIC_FIELDS)

#undef O

#undef MAKE_TV_FIELD
#undef CONVERT_TO_SHARED
#undef RELEASE_SHARED
#undef INITIALIZE_FIELD
#undef SCAN_FIELD
#undef INC_REF_FIELD
#undef DEC_REF_FIELD
#undef CONTAINS_FIELD
#undef GET_KEY_FROM_POS
#undef GET_VAL_FROM_POS
#undef GET_FIELD_OFFSET
#undef COUNT_FIELDS
#undef SET_EVAL_SCALAR

bool TSShape::checkInvariants() const {
  assertx(typeKind() == Kind::T_shape);
  assertx(fieldsByte() == TSShape::kFieldsByte);
  assertx(m_fields == nullptr || m_fields->kindIsValid());
  return true;
}
bool TSTuple::checkInvariants() const {
  assertx(typeKind() == Kind::T_tuple);
  assertx(fieldsByte() == TSTuple::kFieldsByte);
  assertx(m_elem_types && m_elem_types->kindIsValid());
  return true;
}
bool TSFun::checkInvariants() const {
  assertx(typeKind() == Kind::T_fun);
  assertx(fieldsByte() == TSFun::kFieldsByte);
  assertx(m_return_type && m_return_type->kindIsValid());
  assertx(m_param_types == nullptr || m_param_types->kindIsValid());
  assertx(m_variadic_type == nullptr || m_variadic_type->kindIsValid());
  return true;
}
bool TSTypevar::checkInvariants() const {
  assertx(typeKind() == Kind::T_typevar);
  assertx(fieldsByte() == TSTypevar::kFieldsByte);
  assertx(m_name && m_name->kindIsValid());
  return true;
}
bool TSWithClassishTypes::checkInvariants() const {
  assertx(fieldsByte() == TSWithClassishTypes::kFieldsByte);
  assertx(m_generic_types == nullptr || m_generic_types->kindIsValid());
  assertx(m_classname && m_classname->kindIsValid());
  return true;
}
bool TSWithGenericTypes::checkInvariants() const {
  assertx(fieldsByte() == TSWithGenericTypes::kFieldsByte);
  assertx(m_generic_types == nullptr || m_generic_types->kindIsValid());
  return true;
}

bool TSShape::checkBespokeChildren() const {
  if (m_fields) checkBespokeArray(m_fields);
  return true;
}
bool TSTuple::checkBespokeChildren() const {
  return checkBespokeArray(m_elem_types);
}
bool TSFun::checkBespokeChildren() const {
  if (!TypeStructure::isFullyBespokeTypeStructure(m_return_type)) return false;
  if (m_variadic_type &&
      !TypeStructure::isFullyBespokeTypeStructure(m_variadic_type)) {
    return false;
  }
  if (m_param_types) checkBespokeArray(m_param_types);
  return true;
}
bool TSTypevar::checkBespokeChildren() const {
  return true;
}
bool TSWithClassishTypes::checkBespokeChildren() const {
  if (m_generic_types) return checkBespokeArray(m_generic_types);
  return true;
}
bool TSWithGenericTypes::checkBespokeChildren() const {
  if (m_generic_types) return checkBespokeArray(m_generic_types);
  return true;
}

bool TSShape::setField(TSShape* tad, StringData* k, TypedValue v, bool nested) {
  if (s_fields.same(k)) {
    setDictField(tad->m_fields, v, nested);
  } else if (s_allows_unknown_fields.same(k)) {
    assertx(tvIsBool(v));
    tad->m_allows_unknown_fields = val(v).num;
  } else {
    return TypeStructure::setField(tad, k, v, nested);
  }
  return true;
}
bool TSTuple::setField(TSTuple* tad, StringData* k, TypedValue v, bool nested) {
  if (s_elem_types.same(k)) {
    setVecField(tad->m_elem_types, v, nested);
    return true;
  }
  return TypeStructure::setField(tad, k, v, nested);
}
bool TSFun::setField(TSFun* tad, StringData* k, TypedValue v, bool nested) {
  if (s_param_types.same(k)) {
    setVecField(tad->m_param_types, v, nested);
  } else if (s_return_type.same(k)) {
    setTypeField(tad->m_return_type, v, nested);
  } else if (s_variadic_type.same(k)) {
    setTypeField(tad->m_variadic_type, v, nested);
  } else {
    return TypeStructure::setField(tad, k, v, nested);
  }
  return true;
}
bool TSTypevar::setField(
    TSTypevar* tad, StringData* k, TypedValue v, bool nested) {
  if (s_name.same(k)) {
    setStringField(tad->m_name, v);
    return true;
  }
  return TypeStructure::setField(tad, k, v, nested);
}
bool TSWithClassishTypes::setField(
    TSWithClassishTypes* tad, StringData* k, TypedValue v, bool nested) {
  if (s_generic_types.same(k)) {
    setVecField(tad->m_generic_types, v, nested);
  } else if (s_classname.same(k)) {
    setStringField(tad->m_classname, v);
  } else if (s_exact.same(k)) {
    assertx(tvIsBool(v));
    tad->m_exact = val(v).num;
  } else {
    return TypeStructure::setField(tad, k, v, nested);
  }
  return true;
}
bool TSWithGenericTypes::setField(
    TSWithGenericTypes* tad, StringData* k, TypedValue v, bool nested) {
  if (s_generic_types.same(k)) {
    setVecField(tad->m_generic_types, v, nested);
    return true;
  }
  return TypeStructure::setField(tad, k, v, nested);
}

//////////////////////////////////////////////////////////////////////////////
// TypeStructureLayout

using namespace jit;

const LayoutFunctions* typeStructureVtable() {
  static auto const result = fromArray<TypeStructure>();
  return &result;
}

TypeStructureLayout::TypeStructureLayout()
  : ConcreteLayout(
      LayoutIndex{uint16_t(kTypeStructureLayoutByte << 8)},
      "TypeStructure",
      {AbstractLayout::GetBespokeTopIndex()},
      typeStructureVtable()
    )
{}

// TODO: currently these are placeholder values, change them

ArrayLayout TypeStructureLayout::appendType(Type /*val*/) const {
  return ArrayLayout::Vanilla();
}
ArrayLayout TypeStructureLayout::removeType(Type /*key*/) const {
  return ArrayLayout::Vanilla();
}
ArrayLayout TypeStructureLayout::setType(Type /*key*/, Type /*val*/) const {
  return ArrayLayout::Vanilla();
}

std::pair<Type, bool> TypeStructureLayout::elemType(Type key) const {
  if (key <= TInt) return {TBottom, false};
  if (key.hasConstVal(TStr)) {
    auto const dt = bespoke::TypeStructure::getFieldPair(key.strVal()).first;
    if (dt == KindOfBoolean) return {TBool, false};
    // 'kind' field should always be present
    if (dt == KindOfInt64) return {TInt, true};
    if (dt == KindOfVec) return {TVec, false};
    if (dt == KindOfDict) return {TDict, false};
    if (isStringType(dt)) return {TStr, false};
  }

  return {TInitCell, false};
}
std::pair<Type, bool> TypeStructureLayout::firstLastType(
    bool /*isFirst*/, bool /*isKey*/) const {
  return {TBottom, false};
}
Type TypeStructureLayout::iterPosType(Type /*pos*/, bool /*isKey*/) const {
  return TBottom;
}

}
