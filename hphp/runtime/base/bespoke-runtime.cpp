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

#include "hphp/runtime/base/bespoke-runtime.h"

#include "hphp/runtime/base/array-data-defs.h"
#include "hphp/runtime/base/bespoke-array.h"

#include "hphp/runtime/base/bespoke/struct-dict.h"

#include <algorithm>
#include <fmt/format.h>

namespace HPHP {

TRACE_SET_MOD(bespoke)

//////////////////////////////////////////////////////////////////////////////

using bespoke::StructDict;

namespace {
using StrToRSMap = folly::F14FastMap<const StringData*, RuntimeStruct*>;

StrToRSMap s_runtimeStrMap;
folly::SharedMutex s_mapLock;
}

RuntimeStruct::RuntimeStruct(
    const StringData* stableIdentifier,
    const FieldIndexVector& fields,
    size_t fieldsLength)
  : m_profile(nullptr)
  , m_stableIdentifier(stableIdentifier)
  , m_fields(fieldsLength, nullptr)
  , m_assignedLayout(nullptr)
{
  assertx(stableIdentifier->isStatic());

  for (auto const& [idx, key] : fields) {
    setKey(idx, key.get());
  }
}

RuntimeStruct::RuntimeStruct(
    const StringData* stableIdentifier, FieldKeys&& fields)
  : m_profile(nullptr)
  , m_stableIdentifier(stableIdentifier)
  , m_fields(fields.size(), nullptr)
  , m_assignedLayout(nullptr)
{
  assertx(stableIdentifier->isStatic());

  for (auto idx = 0; idx < fields.size(); idx++) {
    auto const key = fields[idx];
    if (key) setKey(idx, key);
  }
}

RuntimeStruct* RuntimeStruct::registerRuntimeStruct(
    const OptString& stableIdentifier, const FieldIndexVector& fields) {
  assertx(stableIdentifier.get()->isStatic());

  if (!allowBespokeArrayLikes()) return nullptr;

  if (Trace::moduleEnabled(Trace::bespoke, 2)) {
    FTRACE(2, "Register runtime struct {}\n", stableIdentifier);
    for (UNUSED auto const& [idx, str] : fields) {
      FTRACE(2, "  {} -> {}\n", idx, str);
    }
  }

  {
    std::shared_lock lock{s_mapLock};
    auto it = s_runtimeStrMap.find(stableIdentifier.get());
    if (it != s_runtimeStrMap.end()) {
      auto const runtimeStruct = it->second;
      lock.unlock();
      // Validate that the fields provided in the query match the registered
      // fields to avoid catastrophic index mismatches.
      runtimeStruct->validateFieldsMatch(fields);
      return runtimeStruct;
    }
  }

  if (Trace::moduleEnabled(Trace::bespoke, 2)) {
    FTRACE(2, "Register new runtime struct {}\n", stableIdentifier);
    for (UNUSED auto const& [idx, str] : fields) {
      FTRACE(2, "  {} -> {}\n", idx, str);
    }
  }

  // Validate that no keys are subject to intish cast.
  for (auto const& [_, str] : fields) {
    int64_t i;
    if (str.get()->isStrictlyInteger(i)) return nullptr;
  }

  // The fields are not contiguous, so find out how many we need to allocate.
  auto fieldsLength = 0;
  for (auto const& [idx, _] : fields) {
    if (idx >= fieldsLength) fieldsLength = idx + 1;
  }

  std::unique_lock lock{s_mapLock};
  auto const pair = s_runtimeStrMap.emplace(stableIdentifier.get(), nullptr);
  if (!pair.second) return pair.first->second;

  auto const mem = allocate(fieldsLength);
  auto const rs = new (mem) RuntimeStruct(
      stableIdentifier.get(), fields, fieldsLength);
  pair.first->second = rs;
  return rs;
}

void RuntimeStruct::validateFieldsMatch(const FieldIndexVector& fields) const {
  SCOPE_ASSERT_DETAIL("RuntimeStruct::validateFieldsMatch") {
    std::string ret =
      fmt::format("Field mismatch for struct {}\n", m_stableIdentifier->slice());
    ret += "Existing schema:\n";
    for (size_t i = 0; i < m_fields.size(); i++) {
      auto const key = getKey(i);
      if (key) ret += fmt::format("  {}: {}\n", i, key);
    }
    ret += "New schema:\n";
    auto newSchema = FieldIndexVector(fields);
    std::sort(newSchema.begin(), newSchema.end());
    for (auto const& [idx, str] : newSchema) {
      ret += fmt::format("  {}: {}\n", idx, str);
    }
    return ret;
  };

  auto numFields = 0;
  for (auto const key : m_fields) {
    if (key) numFields++;
  }
  always_assert(numFields == fields.size());
  for (auto const& [idx, str] : fields) {
    always_assert(getKey(idx)->same(str.get()));
  }
}

RuntimeStruct* RuntimeStruct::allocate(size_t fieldsLength) {
  static_assert(alignof(RuntimeStruct) % alignof(Field) == 0);

  // TODO(kshaunak): We should use vm_malloc here, but it looks bad on perf.
  auto const bytes = sizeof(RuntimeStruct) + fieldsLength * sizeof(Field);
  auto const rs = reinterpret_cast<RuntimeStruct*>(malloc(bytes));

  for (auto i = 0; i < fieldsLength; i++) {
    auto* field = &reinterpret_cast<Field*>(rs + 1)[i];
    field->slot = kInvalidSlot;
    field->required = 0;
  }
  return rs;
}

PackedStringPtr RuntimeStruct::getKey(size_t idx) const {
  assertx(idx < m_fields.size());
  return m_fields[idx];
}

const RuntimeStruct::Field& RuntimeStruct::getField(size_t idx) const {
  assertx(idx < m_fields.size());
  return reinterpret_cast<const Field*>(this + 1)[idx];
}

void RuntimeStruct::setKey(size_t idx, StringData* key) {
  always_assert(key->isStatic());
  assertx(getKey(idx) == nullptr);
  assertx(getField(idx).slot == kInvalidSlot);
  m_fields[idx] = key;
}

void RuntimeStruct::setField(
    size_t idx, Slot slot, bool required, uint8_t type_mask) {
  assertx(getKey(idx) != nullptr);
  assertx(getField(idx).slot == kInvalidSlot);
  auto* field = &reinterpret_cast<Field*>(this + 1)[idx];
  field->slot = slot;
  field->required = required;
  field->type_mask = type_mask;
}

RuntimeStruct* RuntimeStruct::deserialize(
    const StringData* stableIdentifier, FieldKeys&& fields) {
  assertx(allowBespokeArrayLikes());
  auto const mem = allocate(fields.size());
  auto const rs = new (mem) RuntimeStruct(stableIdentifier, std::move(fields));
  s_runtimeStrMap.emplace(stableIdentifier, rs);
  return rs;
}

RuntimeStruct* RuntimeStruct::findById(const StringData* stableIdentifier) {
  assertx(allowBespokeArrayLikes());
  assertx(stableIdentifier->isStatic());

  std::shared_lock lock{s_mapLock};
  auto const runtimeStruct = s_runtimeStrMap[stableIdentifier];
  assertx(runtimeStruct);
  return runtimeStruct;
}

void RuntimeStruct::eachRuntimeStruct(std::function<void(RuntimeStruct*)> fn) {
  assertx(allowBespokeArrayLikes());

  std::shared_lock lock{s_mapLock};
  for (auto& it : s_runtimeStrMap) fn(it.second);
}

void RuntimeStruct::applyLayout(const bespoke::StructLayout* layout) {
  for (size_t i = 0; i < m_fields.size(); i++) {
    auto const key = getKey(i);
    if (!key) continue;
    auto const slot = layout->keySlot(key);
    if (slot == kInvalidSlot) {
      setField(i, slot, false, 0);
      continue;
    }
    auto const& field = layout->field(slot);
    setField(i, slot, field.required, field.type_mask);
  }
  m_assignedLayout.store(layout, std::memory_order_release);
}

const StringData* RuntimeStruct::toString() const {
  return m_stableIdentifier;
}

const StringData* RuntimeStruct::getStableIdentifier() const {
  return m_stableIdentifier;
}

size_t RuntimeStruct::stableHash() const {
  return m_stableIdentifier->hashStatic();
}

//////////////////////////////////////////////////////////////////////////////

StructDictInit::StructDictInit(RuntimeStruct* structHandle, size_t n) {
  m_struct = structHandle;
  if (allowBespokeArrayLikes() && structHandle) {
    auto const layout =
      structHandle->m_assignedLayout.load(std::memory_order_acquire);

    if (layout) {
      // We have been assigned a layout; use it for the initializer.
      m_arr = StructDict::MakeEmpty(layout);
      assertx(n <= std::numeric_limits<uint32_t>::max());
      m_escalateCapacity = static_cast<uint32_t>(n);
      m_numRequired = layout->numRequiredFields();
      m_vanilla = false;
      return;
    }
  }

  m_arr = VanillaDict::MakeReserveDict(n);
  m_vanilla = true;
}

StructDictInit::~StructDictInit() {
  if (!m_arr) return;

  assertx(m_arr->hasExactlyOneRef());
  m_arr->release();
}

void StructDictInit::set(size_t idx, StringData* key, TypedValue value) {
  value = tvToInit(value);

  if (m_vanilla) {
    assertx(m_arr->isVanilla());
    // We have a vanilla VanillaDict that must be properly sized. Set the
    // field in place.
    auto const DEBUG_ONLY res =
      VanillaDict::SetStrInPlace(m_arr, key, value);
    assertx(res == m_arr);
    return;
  }

  // We are currently wrapping a StructDict.
  assertx(m_struct->getKey(idx)->same(key));
  auto const sad = StructDict::As(m_arr);
  auto const field = m_struct->getField(idx);
  if (field.slot == kInvalidSlot ||
      (field.type_mask & static_cast<uint8_t>(value.type()))) {
    // We are adding a key with no corresponding slot. We escalate to vanilla
    // with the requested capacity and clear the m_struct field.
    FTRACE(2, "StructDictInit set at key {}, escalate\n", key);
    m_arr = sad->escalateWithCapacity(m_escalateCapacity, "StructDictInit");
    StructDict::Release(sad);
    assertx(m_arr->isVanillaDict());
    m_struct = nullptr;
    m_vanilla = true;

    auto const DEBUG_ONLY res =
      VanillaDict::SetStrInPlace(m_arr, key, value);
    assertx(res == m_arr);
  } else {
    // We are adding a key with a known slot. We can do so in place.
    FTRACE(2, "StructDictInit set at key {}, slot {}\n", key, field.slot);
    if (debug) {
      // Validate that the slot translation is correct.
      auto const DEBUG_ONLY layout =
        m_struct->m_assignedLayout.load(std::memory_order_acquire);
      assertx(layout->field(field.slot).key->same(key));
    }
    tvIncRefGen(value);
    StructDict::SetStrInSlotInPlace(sad, field.slot, value);
    m_numRequired -= field.required;
  }
}

void StructDictInit::set(size_t idx, const OptString& key, TypedValue value) {
  set(idx, key.get(), value);
}

void StructDictInit::set(size_t idx, const OptString& key, const Variant& value) {
  set(idx, key, *value.asTypedValue());
}

void StructDictInit::set(int64_t key, TypedValue value) {
  value = tvToInit(value);

  if (m_vanilla) {
    assertx(m_arr->isVanilla());
    // We have a vanilla VanillaDict that must be properly sized. Set the
    // field in place.
    auto const DEBUG_ONLY res =
      VanillaDict::SetIntInPlace(m_arr, key, value);
    assertx(res == m_arr);
    return;
  }

  FTRACE(2, "StructDictInit set at integer key {}, escalate\n", key);
  auto const sad = StructDict::As(m_arr);
  m_arr = sad->escalateWithCapacity(m_escalateCapacity, "StructDictInit");
  StructDict::Release(sad);
  assertx(m_arr->isVanillaDict());
  m_struct = nullptr;
  m_vanilla = true;

  auto const DEBUG_ONLY res = VanillaDict::SetIntInPlace(m_arr, key, value);
  assertx(res == m_arr);
}

void StructDictInit::set(int64_t key, const Variant& value) {
  set(key, *value.asTypedValue());
}

void StructDictInit::setIntishCast(size_t idx, const OptString& key,
                                   const Variant& value) {
  if (m_struct) {
    // No keys registered with a RuntimeStruct will ever intish cast.
    DEBUG_ONLY int64_t n;
    assertx(!key.get()->isStrictlyInteger(n));
    set(idx, key.get(), *value.asTypedValue());
    return;
  }

  int64_t n;
  if (ArrayData::IntishCastKey(key.get(), n)) {
    set(n, value);
  } else {
    set(idx, key.get(), *value.asTypedValue());
  }
}

Variant StructDictInit::toVariant() {
  return Variant(toArrayData(), KindOfDict, Variant::ArrayInitCtor{});
}

Array StructDictInit::toArray() {
  return Array(toArrayData(), Array::ArrayInitCtor::Tag);
}

ArrayData* StructDictInit::toArrayData() {
  assertx(m_arr->hasExactlyOneRef());
  assertx(m_arr->isDictType());
  auto const ad = m_arr;
  m_arr = nullptr;

  if (!m_struct) return ad;
  if (m_vanilla) return bespoke::maybeMakeLoggingArray(ad, m_struct);
  if (m_numRequired == 0) return ad;

  FTRACE(2, "StructDictInit left {} required keys, escalate\n", m_numRequired);
  auto const sad = StructDict::As(ad);
  auto const vad = sad->escalateWithCapacity(m_escalateCapacity, __func__);
  StructDict::Release(sad);
  return vad;
}

}
