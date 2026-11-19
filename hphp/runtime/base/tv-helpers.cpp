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

#include "hphp/runtime/base/array-data.h"
#include "hphp/runtime/base/datatype.h"
#include "hphp/runtime/base/object-data.h"
#include "hphp/runtime/base/resource-data.h"
#include "hphp/runtime/base/string-data.h"
#include "hphp/runtime/base/typed-value.h"
#include "hphp/runtime/vm/func.h"
#include "hphp/runtime/vm/class-meth-data-ref.h"
#include "hphp/runtime/vm/rclass-meth-data.h"

#include "hphp/util/assertions.h"

namespace HPHP {

///////////////////////////////////////////////////////////////////////////////

bool tvIsPlausible(const TypedValue cell) {
  auto assertPtr = [](const void* ptr) {
    assertx(ptr && (uintptr_t(ptr) % sizeof(ptr) == 0));
  };

  [&] {
    switch (type(cell)) {
      case KindOfUninit:
      case KindOfNull:
        return;
      case KindOfBoolean:
        assertx(cell.m_data.num == 0 || cell.m_data.num == 1);
        return;
      case KindOfInt64:
      case KindOfDouble:
        return;
      case KindOfEnumClassLabel:
        assertPtr(cell.m_data.pstr);
        assertx(cell.m_data.pstr->kindIsValid());
        assertx(!cell.m_data.pstr->isRefCounted());
        return;
      case KindOfString:
        assertPtr(cell.m_data.pstr);
        assertx(cell.m_data.pstr->kindIsValid());
        assertx(cell.m_data.pstr->checkCountZ());
        return;
      case KindOfVec:
        assertPtr(cell.m_data.parr);
        assertx(cell.m_data.parr->checkCountZ());
        assertx(cell.m_data.parr->isVecType());
        return;
      case KindOfDict:
        assertPtr(cell.m_data.parr);
        assertx(cell.m_data.parr->checkCountZ());
        assertx(cell.m_data.parr->isDictType());
        return;
      case KindOfKeyset:
        assertPtr(cell.m_data.parr);
        assertx(cell.m_data.parr->checkCountZ());
        assertx(cell.m_data.parr->isKeysetType());
        return;
      case KindOfObject:
        assertPtr(cell.m_data.pobj);
        assertx(cell.m_data.pobj->kindIsValid());
        assertx(cell.m_data.pobj->checkCountZ());
        return;
      case KindOfResource:
        assertPtr(cell.m_data.pres);
        assertx(cell.m_data.pres->kindIsValid());
        assertx(cell.m_data.pres->checkCountZ());
        return;
      case KindOfRFunc:
        assertPtr(cell.m_data.prfunc);
        assertx(cell.m_data.prfunc->m_func->validate());
        assertPtr(cell.m_data.prfunc->m_arr);
        assertx(cell.m_data.prfunc->m_arr->checkCountZ());
        assertx(cell.m_data.prfunc->m_arr->isVecType());
        return;
      case KindOfFunc:
        assertPtr(cell.m_data.pfunc);
        assertx(cell.m_data.pfunc->validate());
        return;
      case KindOfClass:
        assertPtr(cell.m_data.pclass);
        assertx(cell.m_data.pclass->validate());
        return;
      case KindOfLazyClass:
        assertPtr(cell.m_data.plazyclass.name());
        return;
      case KindOfClsMeth:
        assertx(cell.m_data.pclsmeth->validate());
        return;
      case KindOfRClsMeth: {
        assertPtr(cell.m_data.prclsmeth);
        assertx(cell.m_data.prclsmeth->m_func->validate());
        assertx(cell.m_data.prclsmeth->m_cls->validate());
        assertPtr(cell.m_data.prclsmeth->m_arr);
        assertx(cell.m_data.prclsmeth->m_arr->checkCountZ());
        assertx(cell.m_data.prclsmeth->m_arr->isVecType());
        return;
      }
    }
    not_reached();
  }();

  return true;
}

///////////////////////////////////////////////////////////////////////////////

}
