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
#include "hphp/runtime/base/apc-typed-value.h"

namespace HPHP {

//////////////////////////////////////////////////////////////////////

/*
 * APCString holds the data to create a PHP string from APC.
 * This object only covers KindOfString. KindOfString are handled
 * via APCTypedValue.
 */
struct APCString {

  static APCHandle::Pair MakeSerializedVec(StringData* str) {
    return MakeSerializedString(APCKind::SerializedVec, str);
  }

  static APCHandle::Pair MakeSerializedDict(StringData* str) {
    return MakeSerializedString(APCKind::SerializedDict, str);
  }

  static APCHandle::Pair MakeSerializedKeyset(StringData* str) {
    return MakeSerializedString(APCKind::SerializedKeyset, str);
  }

  static APCHandle::Pair MakeSerializedObject(const OptString& str) {
    return MakeSerializedString(APCKind::SerializedObject, str.get());
  }

  static void Delete(APCString* s) {
    auto const allocSize = sizeof(APCString) + s->m_str.m_len + 1;
    s->~APCString();
    shared_sized_free(s, allocSize);
  }

  static APCString* fromHandle(APCHandle* handle) {
    assertx(handle->checkInvariants());
    assertx(handle->kind() == APCKind::SerializedVec ||
            handle->kind() == APCKind::SerializedDict ||
            handle->kind() == APCKind::SerializedKeyset ||
            handle->kind() == APCKind::SerializedObject);
    static_assert(
      offsetof(APCString, m_handle) == 0,
      "m_handle must appear first in APCString"
    );
    return reinterpret_cast<APCString*>(handle);
  }

  static const APCString* fromHandle(const APCHandle* handle) {
    assertx(handle->checkInvariants());
    assertx(handle->kind() == APCKind::SerializedVec ||
            handle->kind() == APCKind::SerializedDict ||
            handle->kind() == APCKind::SerializedKeyset ||
            handle->kind() == APCKind::SerializedObject);
    static_assert(
      offsetof(APCString, m_handle) == 0,
      "m_handle must appear first in APCString"
    );
    return reinterpret_cast<const APCString*>(handle);
  }

  // When a request references an APCString, we enqueue it in a
  // request-local list; when the request is over, unreference them all.
  void reference() const;
  static void cleanup();

  StringData* getStringData() {
    return &m_str;
  }

  const StringData* getStringData() const {
    return &m_str;
  }

private:
  static APCHandle::Pair MakeSerializedString(APCKind, StringData*);
  explicit APCString(APCKind kind) : m_handle(kind) {}
  ~APCString() {}
  APCString(const APCString&) = delete;
  APCString& operator=(const APCString&) = delete;

private:
  APCHandle m_handle;
  union {
    StringData m_str;
    uintptr_t dummy[sizeof(StringData) / sizeof(uintptr_t)];
  };
};

//////////////////////////////////////////////////////////////////////

}

