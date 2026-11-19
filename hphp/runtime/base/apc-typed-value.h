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

#include "hphp/runtime/base/apc-handle.h"
#include "hphp/runtime/base/apc-handle-defs.h"

namespace HPHP {

//////////////////////////////////////////////////////////////////////

struct APCTypedValue {
  // Int or bool
  enum class Bool {};
  APCTypedValue(Bool, bool data)
    : m_handle(APCKind::Bool, KindOfBoolean) {
    m_data.num = data;
  }

  explicit APCTypedValue(int64_t data)
    : m_handle(APCKind::Int, KindOfInt64) {
    m_data.num = data;
  }

  explicit APCTypedValue(double data)
    : m_handle(APCKind::Double, KindOfDouble) {
    m_data.dbl = data;
  }

  explicit APCTypedValue(const Func* data)
    : m_handle(APCKind::PersistentFunc, KindOfFunc) {
    assertx(data->isPersistent());
    assertx(!data->isMethod());
    m_data.func = data;
    assertx(checkInvariants());
  }

  explicit APCTypedValue(const Class* data)
    : m_handle(APCKind::PersistentClass, KindOfClass) {
    assertx(data->isPersistent());
    m_data.cls = data;
    assertx(checkInvariants());
  }

  explicit APCTypedValue(LazyClassData data)
    : m_handle(APCKind::LazyClass, KindOfLazyClass) {
    m_data.str = const_cast<StringData*>(data.name());
    assertx(checkInvariants());
  }

  explicit APCTypedValue(const ClsMethDataRef ref)
    : m_handle(APCKind::PersistentClsMeth, KindOfClsMeth) {
    assertx(ref->getCls()->isPersistent());
    m_data.pclsmeth = ref;
  }

  enum class StaticStr {};
  APCTypedValue(StaticStr, StringData* data)
    : m_handle(APCKind::StaticString, KindOfString) {
    assertx(data->isStatic());
    m_data.str = data;
    assertx(checkInvariants());
  }

  enum class SharedStr {};
  APCTypedValue(SharedStr, StringData* data)
    : m_handle(APCKind::SharedString, KindOfString) {
    assertx(data->isShared());
    m_data.str = data;
    assertx(checkInvariants());
  }

  explicit APCTypedValue(DataType type)
    : m_handle(type == KindOfUninit ? APCKind::Uninit : APCKind::Null, type) {
    assertx(isNullType(type)); // Uninit or Null
    m_data.num = 0;
  }

  static APCTypedValue* fromHandle(APCHandle* handle) {
    assertx(handle->checkInvariants() && !handle->isAtomicCounted());
    static_assert(offsetof(APCTypedValue, m_handle) == sizeof(APCHandle), "");
    return reinterpret_cast<APCTypedValue*>(handle - 1);
  }

  static const APCTypedValue* fromHandle(const APCHandle* handle) {
    assertx(handle->checkInvariants() && !handle->isAtomicCounted());
    static_assert(offsetof(APCTypedValue, m_handle) == sizeof(APCHandle), "");
    return reinterpret_cast<const APCTypedValue*>(handle - 1);
  }

  APCHandle* getHandle() {
    return &m_handle;
  }

  bool getBoolean() const {
    assertx(checkInvariants() && m_handle.kind() == APCKind::Bool);
    return m_data.num != 0;
  }

  int64_t getInt64() const {
    assertx(checkInvariants() && m_handle.kind() == APCKind::Int);
    return m_data.num;
  }

  double getDouble() const {
    assertx(checkInvariants() && m_handle.kind() == APCKind::Double);
    return m_data.dbl;
  }

  StringData* getStringData() const {
    assertx(checkInvariants());
    assertx(m_handle.kind() == APCKind::StaticString ||
           m_handle.kind() == APCKind::SharedString);
    return m_data.str;
  }

  ArrayData* getArrayData() const;
  void setArrayData(ArrayData* ad);

  TypedValue toTypedValue() const;

  static APCTypedValue* ForArray(ArrayData* ad);

  static APCTypedValue* tvUninit();
  static APCTypedValue* tvNull();
  static APCTypedValue* tvTrue();
  static APCTypedValue* tvFalse();

  static void FreeHazardPointers();
  static void PushHazardPointer(const APCHandle* handle);
  static bool UseStringHazardPointers();

  void deleteShared();

  static APCHandle::Pair HandlePersistent(ArrayData* data) {
    if (!data->persistentIncRef()) return {nullptr, 0};
    auto const value = APCTypedValue::ForArray(data);
    return {value->getHandle(), sizeof(APCTypedValue)};
  }

  static APCHandle::Pair HandlePersistent(StringData* data) {
    if (data->isRefCounted()) {
      return {nullptr, 0};
    }
    if (data->isStatic()) {
      auto const value = new APCTypedValue(StaticStr{}, data);
      return {value->getHandle(), sizeof(APCTypedValue)};
    }
    data->sharedIncRef();
    auto const value = new APCTypedValue(SharedStr{}, data);
    return {value->getHandle(), sizeof(APCTypedValue)};
  }

private:
  APCTypedValue(ArrayData* ad, APCKind kind, DataType dt);

  APCTypedValue(const APCTypedValue&) = delete;
  APCTypedValue& operator=(const APCTypedValue&) = delete;
  bool checkInvariants() const;

private:
  union {
    int64_t num;
    double dbl;
    StringData* str;
    const Func* func;
    const Class* cls;
    ClsMethDataRef pclsmeth;
    std::atomic<ArrayData*> arr{};
  } m_data;
  APCHandle m_handle;
};

//////////////////////////////////////////////////////////////////////
// Here because of circular dependencies

inline TypedValue APCHandle::toLazyProp() const {
  if (isTypedValue()) return APCTypedValue::fromHandle(this)->toTypedValue();

  TypedValue result;
  result.m_type = kInvalidDataType;
  result.m_data.num = static_cast<int64_t>(uintptr_t(this));
  return result;
}

inline Variant APCHandle::toLocal(bool pure) const {
  if (isTypedValue()) {
    Variant ret;
    *ret.asTypedValue() = APCTypedValue::fromHandle(this)->toTypedValue();
    return ret;
  }
  return toLocalHelper(pure);
}

//////////////////////////////////////////////////////////////////////

}
