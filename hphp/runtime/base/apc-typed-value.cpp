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
#include "hphp/runtime/base/apc-typed-value.h"

#include "hphp/runtime/base/apc-bespoke.h"
#include "hphp/runtime/base/tv-shared.h"
#include "hphp/runtime/ext/apc/ext_apc.h"

namespace HPHP {

//////////////////////////////////////////////////////////////////////

struct HandleHazardPointer {
  const APCHandle* raw;
  TYPE_SCAN_IGNORE_ALL;
};
struct StringHazardPointer {
  StringData* raw;
  TYPE_SCAN_IGNORE_ALL;
};

RDS_LOCAL(std::vector<HandleHazardPointer>, s_handle_hazard_pointers);
RDS_LOCAL(std::vector<StringHazardPointer>, s_string_hazard_pointers);

void APCTypedValue::FreeHazardPointers() {
  for (auto const hp : *s_handle_hazard_pointers) {
    hp.raw->unreferenceNonRoot();
  }
  s_handle_hazard_pointers->clear();

  if (!UseStringHazardPointers()) return;
  for (auto const hp : *s_string_hazard_pointers) {
    DecRefSharedString(hp.raw);
  }
  s_string_hazard_pointers->clear();
}

void APCTypedValue::PushHazardPointer(const APCHandle* handle) {
  s_handle_hazard_pointers->push_back({handle});
}

bool APCTypedValue::UseStringHazardPointers() {
  return false;
}

//////////////////////////////////////////////////////////////////////

APCTypedValue* APCTypedValue::ForArray(ArrayData* ad) {
  assertx(!ad->isRefCounted());
  auto const dt = ad->toDataType();
  auto const result = initAPCBespoke(ad);
  ad = result.ad;

  // If we made an APCBespoke, it'll always be part of a joint allocation.
  if (result.tv) {
    auto const kind = ad->isStatic() ? APCKind::StaticBespoke
                                     : APCKind::SharedBespoke;
    return new (result.tv) APCTypedValue(ad, kind, dt);
  }

  // We didn't make an APCBespoke. Just use a regular persistent array.
  auto const kind = ad->isStatic() ? APCKind::StaticArray
                                   : APCKind::SharedArray;

  // Check if the "co-allocate array and APCTypedValue" optimization hit.
  // It hit if we a) made a new shared array and b) its flag is set.
  if (ad->sharedCowCheck() || !ad->hasApcTv()) {
    return new APCTypedValue(ad, kind, dt);
  }

  auto const mem = reinterpret_cast<APCTypedValue*>(ad) - 1;
  return new (mem) APCTypedValue(ad, kind, dt);
}

APCTypedValue::APCTypedValue(ArrayData* ad, APCKind kind, DataType dt)
    : m_handle(kind, dt) {
  assertx(!ad->isRefCounted());
  m_data.arr.store(ad, std::memory_order_release);
  assertx(checkInvariants());
}

APCTypedValue* APCTypedValue::tvUninit() {
  static APCTypedValue* value = new APCTypedValue(KindOfUninit);
  return value;
}

APCTypedValue* APCTypedValue::tvNull() {
  static APCTypedValue* value = new APCTypedValue(KindOfNull);
  return value;
}

APCTypedValue* APCTypedValue::tvTrue() {
  static auto value = new APCTypedValue(APCTypedValue::Bool{}, true);
  return value;
}

APCTypedValue* APCTypedValue::tvFalse() {
  static auto value = new APCTypedValue(APCTypedValue::Bool{}, false);
  return value;
}

bool APCTypedValue::checkInvariants() const {
  assertx(m_handle.checkInvariants());
  switch (m_handle.kind()) {
    case APCKind::Uninit:
    case APCKind::Null: assertx(m_data.num == 0); break;
    case APCKind::Bool:
    case APCKind::Int:
    case APCKind::Double: break;
    case APCKind::PersistentFunc: assertx(m_data.func->isPersistent()); break;
    case APCKind::PersistentClass: assertx(m_data.cls->isPersistent()); break;
    case APCKind::PersistentClsMeth:
      assertx(m_data.pclsmeth->getCls()->isPersistent()); break;
    case APCKind::StaticString: assertx(m_data.str->isStatic()); break;
    case APCKind::SharedString: assertx(m_data.str->isShared()); break;
    case APCKind::LazyClass: assertx(m_data.str->isStatic()); break;

    case APCKind::StaticArray:
    case APCKind::StaticBespoke: {
      DEBUG_ONLY auto const ad = m_data.arr.load(std::memory_order_acquire);
      assertx(ad->isStatic());
      assertx(ad->toDataType() == m_handle.type());
      break;
    }

    case APCKind::SharedArray:
    case APCKind::SharedBespoke: {
      DEBUG_ONLY auto const ad = m_data.arr.load(std::memory_order_acquire);
      assertx(ad->isShared());
      assertx(ad->toDataType() == m_handle.type());
      break;
    }

    case APCKind::FuncEntity:
    case APCKind::ClsMeth:
    case APCKind::RFunc:
    case APCKind::RClsMeth:
    case APCKind::CopiedObject:
    case APCKind::CopiedCollection:
    case APCKind::CopiedVec:
    case APCKind::CopiedLegacyVec:
    case APCKind::CopiedDict:
    case APCKind::CopiedLegacyDict:
    case APCKind::CopiedKeyset:
    case APCKind::SerializedObject:
    case APCKind::SerializedVec:
    case APCKind::SerializedDict:
    case APCKind::SerializedKeyset:
      assertx(false);
      break;
  }
  return true;
}

//////////////////////////////////////////////////////////////////////

void APCTypedValue::deleteShared() {
  assertx(m_handle.isShared());
  static_assert(std::is_trivially_destructible<APCTypedValue>::value,
                "APCTypedValue must be trivially destructible - "
                "*Array::ReleaseShared() frees the memory without "
                "destroying it");

  switch (m_handle.kind()) {
    case APCKind::SharedArray: {
      auto const ad = m_data.arr.load(std::memory_order_acquire);
      DecRefSharedArray(ad);
      if (ad != static_cast<void*>(this + 1)) {
        delete this;
      }
      return;
    }

    case APCKind::SharedBespoke:
      freeAPCBespoke(this);
      return;

    case APCKind::SharedString:
      DecRefSharedString(m_data.str);
      delete this;
      return;

    default:
      always_assert(false);
  }
}

ArrayData* APCTypedValue::getArrayData() const {
  assertx(checkInvariants());
  return m_data.arr.load(std::memory_order_acquire);
}

void APCTypedValue::setArrayData(ArrayData* ad) {
  m_data.arr.store(ad, std::memory_order_release);
  assertx(checkInvariants());
}

TypedValue APCTypedValue::toTypedValue() const {
  assertx(m_handle.isTypedValue());
  TypedValue tv;
  tv.m_type = m_handle.type();
  auto const kind = m_handle.kind();
  if (UseStringHazardPointers() && kind == APCKind::SharedString) {
    s_string_hazard_pointers->push_back({m_data.str});
    m_data.str->sharedIncRef();
    tv.m_data.pstr = m_data.str;
  } else if (kind == APCKind::SharedBespoke) {
    tv.m_data.parr = readAPCBespoke(this);
  } else {
    tv.m_data.num = m_data.num;
  }
  return tv;
}

//////////////////////////////////////////////////////////////////////

}
