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

#include "hphp/runtime/base/datatype.h"
#include "hphp/runtime/base/typed-value.h"

namespace HPHP {

struct UnalignedTypedValue final {
  Value m_data;
  DataType m_type;

  INLINE_FLATTEN operator TypedValue() const {
    return make_tv_of_type(m_data, m_type);
  }

  TYPE_SCAN_CUSTOM() {
    if (isRefcountedType(m_type) && m_data.pcnt->isRefCounted()) {
      scanner.scan(m_data.pcnt);
    }
  }
};

#if defined(__x86_64__) || defined(__aarch64__)
static_assert(alignof(UnalignedTypedValue) == 1);
static_assert(sizeof(UnalignedTypedValue) == 9);
#endif
static_assert(sizeof(Value) == 8);
static_assert(sizeof(DataType) == 1);
static_assert(offsetof(UnalignedTypedValue, m_data) == 0);
static_assert(offsetof(UnalignedTypedValue, m_type) == 8);

}
