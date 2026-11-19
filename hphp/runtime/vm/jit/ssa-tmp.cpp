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

#include "hphp/runtime/vm/jit/ssa-tmp.h"

#include <sstream>

#include "hphp/runtime/vm/jit/ir-instruction.h"
#include "hphp/runtime/vm/jit/print.h"

namespace HPHP::jit {

SSATmp::SSATmp(uint32_t opndId, IRInstruction* i, int dstId /* = 0 */)
  : m_id(opndId)
{
  setInstruction(i, dstId);
}

void SSATmp::setInstruction(IRInstruction* inst, int dstId) {
  m_inst = inst;
  m_type = outputType(inst, dstId);
}

namespace {
int typeNeededWords(Type t) {
  // Although we say we have zero registers, we always allocate 1 in
  // reg-alloc.cpp.  In practice this doesn't mean much.
  if (t.subtypeOfAny(TUninit,
                     TInitNull,
                     TNullptr)) {
    // These don't need a register because their values are static or unused.
    return 0;
  }
  if (t.maybe(TNullptr)) {
    return typeNeededWords(t - TNullptr);
  }
  if (t <= TPtr) {
    // PtrTo* may be statically unknown but always need just one
    // register.
    return 1;
  }
  if (t <= TLval) {
    // If tv_val<> is ever anything other than 1 or more normal pointers, this
    // will need to change.
    static_assert(sizeof(tv_lval) % 8 == 0, "");
    return sizeof(tv_lval) / 8;
  }
  if (!t.isUnion()) {
    // Not a union type and not a special case: 1 register.
    assertx(IMPLIES(t <= TCell, t.isKnownDataType()));
    return 1;
  }

  assertx(t <= TCell);

  // XXX(t4592459): This will return 2 for TNull, even though it only
  // needs 1 register (one for the type, none for the value). This is to work
  // around limitations in codegen; see the task for details. It does mean we
  // will be loading and storing garbage m_data for Null values but that's fine
  // since m_data is undefined for Null values.
  return t.needsReg() ? 2 : 1;
}
}

int SSATmp::numWords() const {
  return typeNeededWords(type());
}

Variant SSATmp::variantVal() const {
  auto const dt = type().toDataType();
  switch (dt) {
    case KindOfUninit:
    case KindOfNull:
      // Upon return both will converted to KindOfNull anyway.
      return init_null();
    case KindOfBoolean:
      return boolVal();
    case KindOfInt64:
      return intVal();
    case KindOfDouble:
      return dblVal();
    case KindOfString:
      return Variant{strVal(), Variant::PersistentStrInit{}};
    case KindOfVec:
    case KindOfDict:
    case KindOfKeyset:
      return Variant{arrLikeVal(), dt, Variant::PersistentArrInit{}};
    case KindOfClass:
      return Variant{const_cast<Class*>(clsVal())};
    case KindOfLazyClass:
      return Variant{lclsVal()};
    case KindOfFunc:
      return Variant{funcVal()};
    case KindOfClsMeth:
      return Variant{clsmethVal()};
    case KindOfEnumClassLabel:
      return Variant{eclVal(), Variant::EnumClassLabelInit{}};
    case KindOfRClsMeth:
    case KindOfObject:
    case KindOfResource:
    case KindOfRFunc:
      break;
  }
  always_assert(false);
}

std::string SSATmp::toString() const {
  std::ostringstream out;
  print(out, this);
  return out.str();
}

}
