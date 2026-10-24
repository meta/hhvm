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

#include "hphp/runtime/ext/thrift/spec-holder.h"

#include "hphp/runtime/base/array-iterator.h"
#include "hphp/runtime/base/execution-context.h"
#include "hphp/runtime/ext/collections/ext_collections-map.h"
#include "hphp/runtime/ext/collections/ext_collections-set.h"
#include "hphp/runtime/ext/collections/ext_collections-vector.h"
#include "hphp/runtime/ext/thrift/adapter.h"
#include "hphp/runtime/ext/thrift/field_wrapper.h"
#include "hphp/runtime/ext/thrift/type_wrapper.h"
#include "hphp/runtime/ext/thrift/util.h"
#include "hphp/util/configs/eval.h"

namespace HPHP::thrift {

namespace {

const StaticString
  s_class("class"),
  s_key("key"),
  s_val("val"),
  s_elem("elem"),
  s_var("var"),
  s_union("union"),
  s_type("type"),
  s_ktype("ktype"),
  s_vtype("vtype"),
  s_etype("etype"),
  s_format("format"),
  s_SPEC("SPEC"),
  s_isWrapped("is_wrapped"),
  s_isTypeWrapped("is_type_wrapped"),
  s_isTerse("is_terse"),
  s_IThriftStrictUnion("IThriftStrictUnion");

Array get_tspec(const Class& cls) {
  auto lookup = cls.clsCnsGet(s_SPEC.get());
  if (lookup.m_type == KindOfUninit) {
    thrift_error(
      fmt::format("Class {} does not have a property named {}",
                     cls.name()->data(), s_SPEC.c_str()),
      ERR_INVALID_DATA);
  }
  Variant structSpec = tvAsVariant(&lookup);
  if (!structSpec.isArray()) {
    thrift_error("invalid type of spec", ERR_INVALID_DATA);
  }
  return structSpec.toArray();
}

// Check if the field-spec implies that the field's type-constraint will
// always be satisfied. We don't need to do type verification if so.
bool typeSatisfiesConstraint(const TypeIntersectionConstraint& tc,
                             TType type,
                             const Array& spec) {
  switch (type) {
    case T_STOP:
    case T_VOID:
      return tc.alwaysPasses(KindOfNull);
    case T_STRUCT: {
      auto const className = spec.lookup(s_class);
      if (isNullType(className.type())) return false;
      auto const classNameString = tvCastToString(className);
      return tc.alwaysPasses(classNameString.get());
    }
    case T_BOOL:
      return tc.alwaysPasses(KindOfBoolean);
    case T_BYTE:
    case T_I16:
    case T_I32:
    case T_I64:
    case T_U64:
      return tc.alwaysPasses(KindOfInt64);
    case T_DOUBLE:
    case T_FLOAT:
      return tc.alwaysPasses(KindOfDouble);
    case T_UTF8:
    case T_UTF16:
    case T_STRING:
      return tc.alwaysPasses(KindOfString);
    case T_MAP: {
      auto const format = tvCastToString(spec.lookup(s_format));
      if (format.equal(s_collection)) {
        return tc.alwaysPasses(c_Map::classof()->name());
      } else {
        return tc.alwaysPasses(KindOfDict);
      }
    }
    case T_LIST: {
      auto const format = tvCastToString(spec.lookup(s_format));
      if (format.equal(s_collection)) {
        return tc.alwaysPasses(c_Vector::classof()->name());
      } else {
        return tc.alwaysPasses(KindOfVec);
      }
    }
    case T_SET: {
      auto const format = tvCastToString(spec.lookup(s_format));
      if (format.equal(s_harray)) {
        return tc.alwaysPasses(KindOfKeyset);
      } else if (format.equal(s_collection)) {
        return tc.alwaysPasses(c_Set::classof()->name());
      } else {
        return tc.alwaysPasses(KindOfDict);
      }
    }
  }
  return false;
}

const StaticString s_withDefaultValues("withDefaultValues");

const Func* lookupWithDefaultValuesFunc(const Class& cls) {
  auto const func = cls.lookupMethod(s_withDefaultValues.get());
  if (func == nullptr) {
    thrift_error(
      fmt::format("Method {}::withDefaultValues() not found", cls.name()->data()),
      ERR_INVALID_DATA
    );
  }
  if (!func->isStatic()) {
    thrift_error(
      fmt::format("Method {}::withDefaultValues() not static", cls.name()->data()),
      ERR_INVALID_DATA
    );
  }
  return func;
}

const StaticString s_clearTerseFields("clearTerseFields");

const Func* lookupClearTerseFieldsFunc(const Class& cls) {
  auto const func = cls.lookupMethod(s_clearTerseFields.get());
  // We only add the function if it actually do something
  if (func == nullptr) {
    return nullptr;
  }
  if (func->isStatic()) {
    thrift_error(
      fmt::format("Method {}::clearTerseFields() is static", cls.name()->data()),
      ERR_INVALID_DATA
    );
  }
  return func;
}

StructSpec compileSpec(const Array& spec, const Class& cls) {
  // A union field also writes to a property named __type. If one exists, we
  // need to also verify that it accepts integer values. We only need to do
  // this once, so cache it in the optional.
  Optional<bool> endPropOk;
  std::vector<FieldSpec> temp(spec.size());
  ArrayIter specIt = spec.begin();
  for (int i = 0; i < spec.size(); ++i, ++specIt) {
    if (!specIt.first().isInteger()) {
      thrift_error("Bad keytype in TSPEC (expected 'long')", ERR_INVALID_DATA);
    }
    Array fieldSpec = specIt.second().toArray();
    auto field = FieldSpec::compile(fieldSpec, true);
    field.fieldNum = (short)specIt.first().toInt64();

    // Determine if we can safely skip the type check when deserializing.
    field.noTypeCheck = [&] {
      // Check this first, so we skip the type check even if the next one fails.
      if (Cfg::Eval::CheckPropTypeHints <= 0) return true;
      // If the class isn't persistent, we can't elide any type checks
      // anyways, so set it to false pessimistically.
      if (!cls.isPersistent()) return false;
      auto const slot = cls.lookupDeclProp(field.name);
      if (slot == kInvalidSlot) return false;

      if (field.isUnion) {
        if (!endPropOk) {
          endPropOk = [&] {
            if (cls.numDeclProperties() < spec.size()) return false;
            auto const& prop = cls.declProperties()[spec.size()];
            if (!s__type.equal(prop.name)) return false;
            return prop.typeConstraints.alwaysPasses(KindOfInt64);
          }();
        }
        if (!*endPropOk) return false;
      }

      return typeSatisfiesConstraint(
        cls.declPropTypeConstraint(slot), field.type, fieldSpec);
    }();

    temp[i] = std::move(field);
  }

  if (temp.size() >> 16) {
    thrift_error("Too many keys in TSPEC (expected < 2^16)", ERR_INVALID_DATA);
  }

  // We can precompute the cls::withDefaultValues() func pointer only if the
  // underlying class cannot change.
  auto const withDefaultValuesFunc = cls.isPersistent()
    ? lookupWithDefaultValuesFunc(cls) : nullptr;

  Optional<const Func*> clearTerseFieldsFunc;
  if (cls.isPersistent()) {
    clearTerseFieldsFunc = { lookupClearTerseFieldsFunc(cls) };
  }

  bool isStrictUnion{false};
  auto interface = Class::load(s_IThriftStrictUnion.get());
  if (interface != nullptr && cls.classof(interface) ) {
    isStrictUnion = true;
  }

  return StructSpec{HPHP::FixedVector(std::move(temp)), withDefaultValuesFunc, clearTerseFieldsFunc, isStrictUnion};
}

} // namespace

FieldSpec FieldSpec::compile(const Array& fieldSpec, bool topLevel) {
  FieldSpec field;
  field.type = (TType)tvCastToInt64(
    fieldSpec.lookup(s_type, AccessFlags::ErrorKey));
  if (topLevel) {
    field.name = tvCastToStringData(
      fieldSpec.lookup(s_var, AccessFlags::ErrorKey));
    field.isUnion = tvCastToBoolean(
      fieldSpec.lookup(s_union, AccessFlags::Key));
  }
  switch (field.type) {
  case T_STRUCT:
    field.className_ = tvCastToStringData(
      fieldSpec.lookup(s_class, AccessFlags::ErrorKey));
    break;
  case T_LIST:
  case T_SET:
    field.format = tvCastToStringData(
      fieldSpec.lookup(s_format, AccessFlags::Key));
    field.vtype = (TType)tvCastToInt64(
      fieldSpec.lookup(s_etype, AccessFlags::ErrorKey));
    field.val_ = std::make_unique<FieldSpec>(FieldSpec::compile(
      tvCastToArrayLike(fieldSpec.lookup(s_elem, AccessFlags::ErrorKey)),
      false));
    break;
  case T_MAP:
    field.format = tvCastToStringData(
      fieldSpec.lookup(s_format, AccessFlags::Key));
    field.ktype = (TType)tvCastToInt64(
      fieldSpec.lookup(s_ktype, AccessFlags::ErrorKey));
    field.vtype = (TType)tvCastToInt64(
      fieldSpec.lookup(s_vtype, AccessFlags::ErrorKey));
    field.key_ = std::make_unique<FieldSpec>(FieldSpec::compile(
      tvCastToArrayLike(fieldSpec.lookup(s_key, AccessFlags::ErrorKey)),
      false));
    field.val_ = std::make_unique<FieldSpec>(FieldSpec::compile(
      tvCastToArrayLike(fieldSpec.lookup(s_val, AccessFlags::ErrorKey)),
      false));
    break;
  default:
    break;
  }
  field.adapter = getAdapter(fieldSpec);
  field.isWrapped = tvCastToBoolean(
      fieldSpec.lookup(s_isWrapped, AccessFlags::Key));
  field.isTypeWrapped = tvCastToBoolean(
      fieldSpec.lookup(s_isTypeWrapped, AccessFlags::Key));
  field.isTerse = tvCastToBoolean(
      fieldSpec.lookup(s_isTerse, AccessFlags::Key));
  return field;
}

// The returned reference is valid at least while this SpecHolder is alive.
const StructSpec& SpecHolder::getSpec(const Class& cls) {
  auto const cacheable = cls.isPersistent();
  if (!cacheable) {
    const auto spec = get_tspec(cls);
    m_tempSpec = compileSpec(spec, cls);
    return m_tempSpec;
  }

  auto specSlot = cls.getThriftData();
  if (specSlot != nullptr) {
    auto const spec = specSlot->load(std::memory_order_acquire);
    if (spec) return *static_cast<StructSpec*>(spec);
  }

  const auto spec = get_tspec(cls);
  if (spec->isStatic()) {
    assertx(specSlot);
    auto compiled =
      std::make_unique<StructSpec>(compileSpec(spec, cls));
    void* expected = nullptr;
    if (specSlot->compare_exchange_strong(
          expected, (void*)compiled.get(),
          std::memory_order_acq_rel, std::memory_order_relaxed)) {
      return *compiled.release();
    }
    return *static_cast<StructSpec*>(expected);
  } else {
    m_tempSpec = compileSpec(spec, cls);
    return m_tempSpec;
  }
}

const FieldSpec* getFieldSlow(const StructSpec& spec, int16_t fieldNum) {
  for (const auto& field : spec.fields) {
    if (field.fieldNum == fieldNum) {
      return &field;
    }
  }
  return nullptr;
}

Object StructSpec::newObject(Class& cls) const {
  auto const func = withDefaultValuesFunc != nullptr
    ? withDefaultValuesFunc : lookupWithDefaultValuesFunc(cls);

  auto obj = g_context->invokeFuncFew(
    func, &cls, 0, nullptr, nullptr, RuntimeCoeffects::pure(), false /* dynamic */);
  if (tvIsObject(obj)) {
    return Object::attach(obj.m_data.pobj);
  }

  SCOPE_EXIT { tvDecRefGen(obj); };
  thrift_error(
    fmt::format(
      "Method {}::withDefaultValues() returned a non-object.",
      cls.name()->data()
    ),
    ERR_INVALID_DATA
  );
}

void StructSpec::clearTerseFields(const Class& cls, const Object& obj) const {
  auto const func = [&]{
    if (clearTerseFieldsFunc) {
      return *clearTerseFieldsFunc;
    }
    return lookupClearTerseFieldsFunc(cls);
  }();
  if (func == nullptr) return;
  g_context->invokeFuncFew(
    func, obj.get(), 0, nullptr, nullptr, RuntimeCoeffects::write_props());
}

}
