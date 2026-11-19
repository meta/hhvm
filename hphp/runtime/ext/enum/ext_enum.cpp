/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
   | Copyright (c) 1997-2010 The PHP Group                                |
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
#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/base/array-iterator.h"
#include "hphp/runtime/ext/extension.h"
#include "hphp/runtime/base/builtin-functions.h"
#include "hphp/runtime/base/enum-cache.h"
#include "hphp/runtime/base/enum-util.h"
#include "hphp/runtime/base/opaque-resource.h"
#include "hphp/runtime/base/type-variant.h"

namespace HPHP {

//////////////////////////////////////////////////////////////////////////////
// class BuiltinEnum
static Array HHVM_STATIC_METHOD(BuiltinEnum, getValues) {
  const EnumValues* values = EnumCache::getValuesBuiltin(self_);
  assertx(values->values.isDict());
  return values->values;
}

const StaticString s_invariant_violation("HH\\invariant_violation");

static Array HHVM_STATIC_METHOD(BuiltinEnum, getNames) {
  const EnumValues* values = EnumCache::getValuesBuiltin(self_);
  if (values->names.size() != values->values.size()) {
    // Figure out the names for the colliding enum values so we can
    // provide them in the error message. Loop over the name -> value
    // array, and look for any entries that do not map back to the
    // same name in the value -> name array. The (different) names in
    // the name -> value array and the value -> name array are the two
    // offenders.
    auto const [mismatch1, mismatch2] = [&] {
      const StringData* mismatch1 = nullptr;
      const StringData* mismatch2 = nullptr;
      IterateKV(
        values->values.get(),
        [&] (TypedValue k, TypedValue v) {
          assertx(isStringType(k.m_type));

          // Handle key coercion manually
          auto const converted = [&] {
            int64_t num;
            if (isStringType(v.m_type) &&
                v.m_data.pstr->isStrictlyInteger(num)) {
              return make_tv<KindOfInt64>(num);
            } else if (isClassType(v.m_type)) {
              return make_tv<KindOfString>(v.m_data.pclass->name());
            } else if (isLazyClassType(v.m_type)) {
              return make_tv<KindOfString>(
                v.m_data.plazyclass.name()
              );
            }
            return v;
          }();

          auto const name = values->names.lookup(converted);
          assertx(isStringType(name.m_type));

          if (!name.m_data.pstr->same(k.m_data.pstr)) {
            mismatch1 = k.m_data.pstr;
            mismatch2 = name.m_data.pstr;
            return true;
          }
          return false;
        }
      );
      assertx(mismatch1 && mismatch2);
      return std::make_pair(mismatch1, mismatch2);
    }();

    vm_call_user_func(
      Func::lookup(s_invariant_violation.get()),
      make_vec_array(
        OptString{
          fmt::format(
            "Enum {} has overlapping values {} and {}",
            self_->name()->data(),
            mismatch1->slice(),
            mismatch2->slice()
          )
        }
      )
    );
  }

  assertx(values->names.isDict());
  return values->names;
}

static bool HHVM_STATIC_METHOD(BuiltinEnum, isValid, const Variant &value) {
  return enumHasValue(self_, value.asTypedValue());
}

static Variant HHVM_STATIC_METHOD(BuiltinEnum, coerce, const Variant &value) {
  if (UNLIKELY(!value.isInteger() && !value.isString() &&
               !value.isClass() && !value.isLazyClass())) {
    return init_null();
  }

  auto res = value;

  // Manually do int-like string conversion. This is to ensure the lookup
  // succeeds below (since the values array does int-like string key conversion
  // when created, even if its a dict).
  int64_t num;
  if (value.isString() &&
      value.getStringData()->isStrictlyInteger(num)) {
    res = Variant(num);
  } else if (value.isClass()) {
    res = VarNR{value.toClassVal()->name()};
  } else if (value.isLazyClass()) {
    res = VarNR{value.toLazyClassVal().name()};
  }

  auto values = EnumCache::getValuesBuiltin(self_);
  if (!values->names.exists(res)) {
    res = init_null();
  } else if (auto base = self_->enumBaseTy().underlyingDataType()) {
    if (isStringType(*base) && res.isInteger()) {
      res = Variant(res.toString());
    }
  } else {
    // If the value is present, but the enum has no base type, return the value
    // as specified, undoing any int-like string conversion we did on it.
    if (res.isInteger()) return value;
  }

  return res;
}

//////////////////////////////////////////////////////////////////////////////

struct enumExtension final : Extension {
  enumExtension() : Extension("enum", "1.0.0-dev", NO_ONCALL_YET) {}
  void moduleRegisterNative() override {
    HHVM_STATIC_MALIAS(HH\\BuiltinEnum, getValues, BuiltinEnum, getValues);
    HHVM_STATIC_MALIAS(HH\\BuiltinEnum, getNames, BuiltinEnum, getNames);
    HHVM_STATIC_MALIAS(HH\\BuiltinEnum, isValid, BuiltinEnum, isValid);
    HHVM_STATIC_MALIAS(HH\\BuiltinEnum, coerce, BuiltinEnum, coerce);
    HHVM_STATIC_MALIAS(HH\\BuiltinEnumClass, getValues, BuiltinEnum, getValues);
  }
} s_enum_extension;

//////////////////////////////////////////////////////////////////////////////
} // namespace HPHP
