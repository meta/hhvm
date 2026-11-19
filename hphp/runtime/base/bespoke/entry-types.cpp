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

#include "hphp/runtime/base/bespoke/entry-types.h"

#include "hphp/runtime/base/array-data-defs.h"
#include "hphp/runtime/base/array-iterator.h"

namespace HPHP::bespoke {

TRACE_SET_MOD(bespoke)

//////////////////////////////////////////////////////////////////////////////

namespace {

//////////////////////////////////////////////////////////////////////////////

DataType mergeEquivTypes(DataType a, DataType b) {
  assertx(equivDataTypes(a, b));
  return a;
}

KeyTypes keyTypesForKey(TypedValue k, KeyTypes b) {
  auto const a = [&] {
    if (isStringType(k.type())) {
      return k.val().pstr->isStatic() ? KeyTypes::StaticStrings
                                      : KeyTypes::Strings;
    } else if (isIntType(k.type())) {
      return KeyTypes::Ints;
    } else {
      return KeyTypes::Any;
    }
  }();

  if (a == b) return a;

  if (a == KeyTypes::Empty) return b;
  if (b == KeyTypes::Empty) return a;

  if ((a == KeyTypes::StaticStrings && b == KeyTypes::Strings) ||
      (b == KeyTypes::StaticStrings && a == KeyTypes::Strings)) {
    return KeyTypes::Strings;
  }

  return KeyTypes::Any;
}

std::pair<ValueTypes, DataType> valueTypesForValue(TypedValue v,
                                                   ValueTypes vms,
                                                   DataType type) {
  switch (vms) {
    case ValueTypes::Empty:
      return {ValueTypes::Monotype, v.type()};

    case ValueTypes::Monotype:
      if (equivDataTypes(type, v.type())) {
        return {ValueTypes::Monotype, mergeEquivTypes(type, v.type())};
      } else if (isNullType(v.type())) {
        return {ValueTypes::MonotypeNullable, type};
      } else if (isNullType(type)) {
        return {ValueTypes::MonotypeNullable, v.type()};
      } else {
        return {ValueTypes::Any, kInvalidDataType};
      }

    case ValueTypes::MonotypeNullable:
      if (equivDataTypes(type, v.type())) {
        return {ValueTypes::MonotypeNullable, mergeEquivTypes(type, v.type())};
      } else if (isNullType(v.type())) {
        return {ValueTypes::MonotypeNullable, type};
      } else {
        return {ValueTypes::Any, kInvalidDataType};
      }

    case ValueTypes::Any:
      return {ValueTypes::Any, kInvalidDataType};
  }
  not_reached();
}

//////////////////////////////////////////////////////////////////////////////

}

//////////////////////////////////////////////////////////////////////////////

bool EntryTypes::checkInvariants() const {
  assertx(IMPLIES(valueTypes == ValueTypes::Any
                  || valueTypes == ValueTypes::Empty,
          valueDatatype == kInvalidDataType));
  assertx(IMPLIES(valueTypes == ValueTypes::MonotypeNullable,
          valueDatatype != KindOfNull));
  return true;
}

EntryTypes EntryTypes::ForArray(const ArrayData* ad) {
  auto state = EntryTypes(KeyTypes::Empty, ValueTypes::Empty, kInvalidDataType);
  IterateKV(ad, [&](auto k, auto v) { state = state.with(k, v); });
  return state;
}

EntryTypes EntryTypes::with(TypedValue v) const {
  auto const valuePair = valueTypesForValue(v, valueTypes, valueDatatype);

  return EntryTypes(keyTypes, valuePair.first, valuePair.second);
}

EntryTypes EntryTypes::with(TypedValue k, TypedValue v) const {
  auto const newKeyTypes = keyTypesForKey(k, keyTypes);
  auto const valuePair = valueTypesForValue(v, valueTypes, valueDatatype);

  return EntryTypes(newKeyTypes, valuePair.first, valuePair.second);
}

EntryTypes EntryTypes::pessimizeValueTypes() const {
  return EntryTypes(keyTypes, ValueTypes::Any, kInvalidDataType);
}

bool EntryTypes::isMonotypeState() const {
  auto const monotype_key = [&]{
    switch (keyTypes) {
      case KeyTypes::Empty:         return true;
      case KeyTypes::Ints:          return true;
      case KeyTypes::StaticStrings: return true;
      case KeyTypes::Strings:       return true;
      case KeyTypes::Any:           return false;
    }
    always_assert(false);
  }();
  auto const monotype_val = [&]{
    switch (valueTypes) {
      case ValueTypes::Empty:            return true;
      case ValueTypes::Monotype:         return true;
      case ValueTypes::MonotypeNullable: return false;
      case ValueTypes::Any:              return false;
    }
    always_assert(false);
  }();
  return monotype_key && monotype_val;
}

std::string EntryTypes::toString() const {
  auto const valueSt = [&] {
    switch (valueTypes) {
      case ValueTypes::Empty: return fmt::format("Empty");
      case ValueTypes::Monotype:
        return fmt::format("Monotype ({})", tname(valueDatatype));
      case ValueTypes::MonotypeNullable:
        return fmt::format("MonotypeNullable ({})", tname(valueDatatype));
      case ValueTypes::Any: return fmt::format("Any");
    }
    not_reached();
  }();

  return fmt::format("<{}, {}>", show(keyTypes), valueSt);
}

const char* show(KeyTypes kt) {
  switch (kt) {
    case KeyTypes::Empty: return "Empty";
    case KeyTypes::Ints: return "Ints";
    case KeyTypes::StaticStrings: return "StaticStrings";
    case KeyTypes::Strings: return "Strings";
    case KeyTypes::Any: return "Any";
  }
  not_reached();
}

}
