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

#include "hphp/runtime/vm/jit/guard-constraint.h"

#include "hphp/runtime/base/datatype.h"

#include <folly/Conv.h>

namespace HPHP::jit {

TRACE_SET_MOD(hhir)

///////////////////////////////////////////////////////////////////////////////

std::string GuardConstraint::toString() const {
  std::string ret = "<" + typeCategoryName(category);

  if (category == DataTypeSpecialized) {
    if (isArrayLayoutSensitive()) {
      ret += ",ArrayLayout";
    } else if (wantClass()) {
      folly::toAppend("Cls:", desiredClass()->name()->data(), &ret);
    }
  }

  if (weak) ret += ",weak";

  return ret + '>';
}

///////////////////////////////////////////////////////////////////////////////

bool typeFitsConstraint(Type t, GuardConstraint gc) {
  switch (gc.category) {
    case DataTypeGeneric:
      return true;

    case DataTypeCountnessInit:
      return (t.isKnownDataType() || !t.maybe(TCounted)) &&
             (t <= TUninit || !t.maybe(TUninit));

    case DataTypeSpecific:
      return t.isKnownDataType();

    case DataTypeSpecialized:
      // Type::isSpecialized() returns true for types like {Arr<Packed>|Int},
      // so we need to check both for specialization and isKnownDataType.
      assertx(gc.wantClass() + gc.isArrayLayoutSensitive() == 1);
      if (!t.isKnownDataType()) return false;
      if (gc.wantClass()) {
        auto const clsSpec = t.clsSpec();
        return clsSpec && clsSpec.cls()->classof(gc.desiredClass());
      } else if (gc.isArrayLayoutSensitive()) {
        auto const arrSpec = t.arrSpec();
        return arrSpec.vanilla() || arrSpec.bespoke();
      }
      return false;
  }
  always_assert(false);
}

static void incCategory(DataTypeCategory& c) {
  always_assert(c != DataTypeSpecialized);
  c = static_cast<DataTypeCategory>(static_cast<uint8_t>(c) + 1);
}

GuardConstraint relaxConstraint(GuardConstraint origGc,
                                Type knownType, Type toRelax) {
  ITRACE(4, "relaxConstraint({}, knownType = {}, toRelax = {})\n",
         origGc, knownType, toRelax);
  Trace::Indent _i;

  auto const dstType = knownType & toRelax;
  always_assert_flog(typeFitsConstraint(dstType, origGc),
                     "refine({}, {}) doesn't fit {}",
                     knownType, toRelax, origGc);

  // Preserve origGc's weak property.
  GuardConstraint newGc{DataTypeGeneric};
  newGc.weak = origGc.weak;
  Type newDstType{};

  while (true) {
    if (newGc.isSpecialized()) {
      if (origGc.isArrayLayoutSensitive()) newGc.setArrayLayoutSensitive();
      if (origGc.wantClass()) newGc.setDesiredClass(origGc.desiredClass());
    }

    newDstType = knownType & relaxToConstraint(toRelax, newGc);
    if (typeFitsConstraint(newDstType, origGc)) break;
    ITRACE(5, "newDstType = {}, newGc = {}; incrementing constraint\n",
           newDstType, newGc);
    incCategory(newGc.category);
  }
  ITRACE(4, "Returning {}\n", newGc);
  // newGc shouldn't be any more specific than origGc.
  always_assert(newGc.category <= origGc.category);
  return newGc;
}

GuardConstraint applyConstraint(GuardConstraint gc,
                                GuardConstraint newGc) {
  gc.category = std::max(newGc.category, gc.category);

  if (newGc.isArrayLayoutSensitive()) gc.setArrayLayoutSensitive();

  if (newGc.wantClass()) {
    if (gc.wantClass()) {
      // It only makes sense to constrain gc with a class that's related to its
      // existing class, and we want to preserve the more derived of the two.
      auto cls1 = gc.desiredClass();
      auto cls2 = newGc.desiredClass();
      gc.setDesiredClass(cls1->classof(cls2) ? cls1 : cls2);
    } else {
      gc.setDesiredClass(newGc.desiredClass());
    }
  }

  return gc;
}

///////////////////////////////////////////////////////////////////////////////

}
