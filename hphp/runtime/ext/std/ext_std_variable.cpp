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
#include "hphp/runtime/ext/std/ext_std_variable.h"

#include "hphp/runtime/base/backtrace.h"
#include "hphp/runtime/base/builtin-functions.h"
#include "hphp/runtime/base/collections.h"
#include "hphp/runtime/base/exceptions.h"
#include "hphp/runtime/base/variable-serializer.h"
#include "hphp/runtime/base/variable-unserializer.h"
#include "hphp/runtime/base/zstd-decompress-source.h"
#include "hphp/runtime/vm/class-meth-data-ref.h"

#include "hphp/runtime/ext/collections/ext_collections-pair.h"
#include "hphp/runtime/server/http-protocol.h"

#include "hphp/util/configs/errorhandling.h"
#include "hphp/util/configs/eval.h"

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

const StaticString
  s_unknown_type("unknown type"),
  s_boolean("boolean"),
  s_bool("bool"),
  s_integer("integer"),
  s_int("int"),
  s_float("float"),
  s_double("double"),
  s_string("string"),
  s_object("object"),
  s_array("array"),
  s_NULL("NULL"),
  s_null("null"),
  s_meth_caller_cls("__SystemLib\\MethCallerHelper"),
  s_dyn_meth_caller_cls("__SystemLib\\DynMethCallerHelper");

OptString HHVM_FUNCTION(gettype, const Variant& v) {
  if (v.getType() == KindOfResource && v.toCResRef().isInvalid()) {
    return s_unknown_type;
  }
  /* Although getDataTypeString also handles the null type, it returns "null"
   * (lower case). Unfortunately, PHP returns "NULL" (upper case) for
   * gettype(). So we make an exception here. */
  if (v.isNull()) {
    return s_NULL;
  }
  if (Cfg::Eval::ClassAsStringGetType &&
      (v.isLazyClass() || v.isClass())) {
    return s_string;
  }
  return getDataTypeString(v.getType());
}

OptString HHVM_FUNCTION(get_resource_type, const OptResource& handle) {
  return handle->o_getResourceName();
}

int64_t HHVM_FUNCTION(intval, const Variant& v, int64_t base /* = 10 */) {
  return v.toInt64(base);
}

double HHVM_FUNCTION(floatval, const Variant& v) {
  return v.toDouble();
}

OptString HHVM_FUNCTION(strval, const Variant& v) {
  return v.toString();
}

bool HHVM_FUNCTION(is_null, const Variant& v) {
  return is_null(v.asTypedValue());
}

bool HHVM_FUNCTION(is_bool, const Variant& v) {
  return is_bool(v.asTypedValue());
}

bool HHVM_FUNCTION(is_int, const Variant& v) {
  return is_int(v.asTypedValue());
}

bool HHVM_FUNCTION(is_float, const Variant& v) {
  return is_double(v.asTypedValue());
}

bool HHVM_FUNCTION(is_numeric, const Variant& v) {
  return v.isNumeric(true);
}

bool HHVM_FUNCTION(is_string, const Variant& v) {
  return is_string(v.asTypedValue());
}

bool HHVM_FUNCTION(is_scalar, const Variant& v) {
  return v.isScalar();
}

bool HHVM_FUNCTION(HH_is_php_array, const Variant& v) {
  return isArrayLikeType(v.getType()) && v.getArrayData()->isLegacyArray();
}

bool HHVM_FUNCTION(is_array, const Variant& v) {
  if (Cfg::Eval::LogOnIsArrayFunction) {
    raise_notice("call to deprecated builtin is_array()");
  }
  return is_any_array(v.asTypedValue());
}

bool HHVM_FUNCTION(HH_is_vec, const Variant& v) {
  return is_vec(v.asTypedValue());
}

bool HHVM_FUNCTION(HH_is_dict, const Variant& v) {
  return is_dict(v.asTypedValue());
}

bool HHVM_FUNCTION(HH_is_keyset, const Variant& v) {
  return is_keyset(v.asTypedValue());
}

bool HHVM_FUNCTION(HH_is_varray, const Variant& val) {
  return is_vec(val.asTypedValue());
}

bool HHVM_FUNCTION(HH_is_darray, const Variant& val) {
  return is_dict(val.asTypedValue());
}

bool HHVM_FUNCTION(HH_is_any_array, const Variant& val) {
  return is_any_array(val.asTypedValue());
}

bool HHVM_FUNCTION(HH_is_list_like, const Variant& val) {
  auto const& ty = val.getType();
  if (!isArrayLikeType(ty)) return false;
  if (isVecType(ty)) return true;
  auto const& arr = val.asCArrRef();
  return arr->isVectorData();
}

bool HHVM_FUNCTION(is_object, const Variant& v) {
  return is_object(v.asTypedValue());
}

bool HHVM_FUNCTION(is_resource, const Variant& v) {
  return (v.getType() == KindOfResource && !v.toCResRef().isInvalid());
}

bool HHVM_FUNCTION(HH_is_meth_caller, TypedValue v) {
  if (tvIsFunc(v)) {
    return val(v).pfunc->isMethCaller();
  } else if (tvIsObject(v)) {
    auto const mcCls = Class::lookup(s_meth_caller_cls.get());
    auto const dynMcCls = Class::lookup(s_dyn_meth_caller_cls.get());
    auto const cls = val(v).pobj->getVMClass();
    assertx(mcCls);
    assertx(dynMcCls);
    return mcCls == cls || dynMcCls == cls;
  }
  return false;
}

///////////////////////////////////////////////////////////////////////////////

Array HHVM_FUNCTION(HH_object_prop_array,
                    const Object& obj,
                    bool ignore_late_init /* = false */) {
  return obj.toArray(ignore_late_init).toDict();
}

///////////////////////////////////////////////////////////////////////////////
// input/output

ALWAYS_INLINE Variant print_r_impl(const Variant& expression,
                                   bool ret /* = false */,
                                   bool pure /* = false */) {
  Variant res;
  try {
    VariableSerializer vs(VariableSerializer::Type::PrintR);
    if (pure) {
      vs.setPure();
    }
    if (ret) {
      res = vs.serialize(expression, ret);
    } else {
      vs.serialize(expression, ret);
      res = true;
    }
  } catch (StringBufferLimitException& e) {
    raise_notice("print_r() exceeded max bytes limit");
    res = e.m_result;
  }
  return res;
}

Variant HHVM_FUNCTION(print_r, const Variant& expression,
                               bool ret /* = false */) {
  return print_r_impl(expression, ret, false);
}

Variant HHVM_FUNCTION(print_r_pure, const Variant& expression) {
  return print_r_impl(expression, true, true);
}

ALWAYS_INLINE Variant var_export_impl(const Variant& expression,
                                      bool ret /* = false */,
                                      bool pure /* = false */) {
  Variant res;
  try {
    VariableSerializer vs(VariableSerializer::Type::VarExport);
    if (pure) {
      vs.setPure();
    }
    if (ret) {
      res = vs.serialize(expression, ret);
    } else {
      vs.serialize(expression, ret);
      res = true;
    }
  } catch (StringBufferLimitException& ) {
    raise_notice("var_export() exceeded max bytes limit");
  }
  return res;
}

Variant HHVM_FUNCTION(print_debug_display, const Variant& expression, bool ret) {
  VariableSerializer vs(VariableSerializer::Type::DebuggerDump, 0, 2);
  return vs.serialize(expression, ret);
}

Variant HHVM_FUNCTION(var_export, const Variant& expression,
                                  bool ret /* = false */) {
  return var_export_impl(expression, ret, false);
}

Variant HHVM_FUNCTION(var_export_pure, const Variant& expression) {
  return var_export_impl(expression, true, true);
}

static ALWAYS_INLINE void do_var_dump(VariableSerializer& vs,
                                      const Variant& expression) {
  // manipulate maxCount to match PHP behavior
  if (!expression.isObject()) {
    vs.incMaxCount();
  }
  vs.serialize(expression, false);
}

void HHVM_FUNCTION(var_dump, const Variant& expression,
                             const Array& _argv /*=null_array */) {

  VariableSerializer vs(VariableSerializer::Type::VarDump, 0, 2);
  do_var_dump(vs, expression);

  auto sz = _argv.size();
  for (int i = 0; i < sz; i++) {
    do_var_dump(vs, _argv[i]);
  }
}

void HHVM_FUNCTION(debug_zval_dump, const Variant& variable) {
  VariableSerializer vs(VariableSerializer::Type::DebugDump);
  vs.serialize(variable, false);
}

void HHVM_FUNCTION(debugger_dump, const Variant& variable) {
  if (!variable.isNull()) {
    VariableSerializer vs(VariableSerializer::Type::DebuggerDump, 0, 2);
    vs.serialize(variable,
                false, // ret
                false // keepCount
                );
  }
}

/*
 * Intrinsic for Containers, i.e. the subset of \HH\Traversable including
 * 1. array:
 *   array, vec, dict, keyset
 * 2. collection: Vector, Map, Set
 * but not including Objects that implement e.g. \HH\Iterable or \HH\Iterator.
 */
Variant HHVM_FUNCTION(HH_first, const Variant& v) {
  // 1. array, vec, dict, keyset
  if (v.isArray() || v.isClsMeth()) {
    auto arr = v.toArray();
    if (arr->empty()) {
      return init_null();
    }
    return arr->getValue(arr->iter_begin());
  }

  if (v.isObject()) {
    auto obj = v.asCObjRef();
    // 2. collection
    if (obj->isCollection()) {
      // Pair
      if (obj->collectionType() == CollectionType::Pair) {
        auto const pair = static_cast<c_Pair*>(obj.get());
        return Variant::wrap(*pair->at(0));
      }

      // Vector, Map, Set, and Imm variants
      auto arr = collections::asArray(obj.get());
      if (arr->empty()) {
        return init_null();
      }
      return arr->getValue(arr->iter_begin());
    }
  }
  SystemLib::throwInvalidArgumentExceptionObject(
    "Argument 1 passed to HH\\Lib\\_Private\\Native\\first() "
     "must be a Container");
}

Variant HHVM_FUNCTION(HH_last, const Variant& v) {
  // 1. array, vec, dict, keyset
  if (v.isArray() || v.isClsMeth()) {
    auto arr = v.toArray();
    if (arr->empty()) {
      return init_null();
    }
    return arr->getValue(arr->iter_last());
  }

  if (v.isObject()) {
    auto obj = v.asCObjRef();
    // 2. collection
    if (obj->isCollection()) {
      // Pair
      if (obj->collectionType() == CollectionType::Pair) {
        auto const pair = static_cast<c_Pair*>(obj.get());
        return Variant::wrap(*pair->at(1));
      }

      // Vector, Map, Set, and Imm variants
      auto arr = collections::asArray(obj.get());
      if (arr->empty()) {
        return init_null();
      }
      return arr->getValue(arr->iter_last());
    }
  }
  SystemLib::throwInvalidArgumentExceptionObject(
    "Argument 1 passed to HH\\Lib\\_Private\\Native\\last() "
    "must be a Container");
}

Variant HHVM_FUNCTION(HH_first_key, const Variant& v) {
  // 1. array, vec, dict, keyset
  if (v.isArray() || v.isClsMeth()) {
    auto arr = v.toArray();
    if (arr->empty()) {
      return init_null();
    }
    return arr->getKey(arr->iter_begin());
  }

  if (v.isObject()) {
    auto obj = v.asCObjRef();
    // 2. collection
    if (obj->isCollection()) {
      // Pair
      if (obj->collectionType() == CollectionType::Pair) {
        return Variant::wrap(make_tv<KindOfInt64>(0));
      }

      // Vector, Map, Set, and Imm variants
      auto arr = collections::asArray(obj.get());
      if (arr->empty()) {
        return init_null();
      }
      return arr->getKey(arr->iter_begin());
    }
  }
  SystemLib::throwInvalidArgumentExceptionObject(
    "Argument 1 passed to HH\\Lib\\_Private\\Native\\first_key() "
    "must be a Container");
}

Variant HHVM_FUNCTION(HH_last_key, const Variant& v) {
  // 1. array, vec, dict, keyset
  if (v.isArray() || v.isClsMeth()) {
    auto arr = v.toArray();
    if (arr->empty()) {
      return init_null();
    }
    return arr->getKey(arr->iter_last());
  }

  if (v.isObject()) {
    auto obj = v.asCObjRef();
    // 2. collection
    if (obj->isCollection()) {
      // Pair
      if (obj->collectionType() == CollectionType::Pair) {
        return Variant::wrap(make_tv<KindOfInt64>(1));
      }

      // Vector, Map, Set, and Imm variants
      auto arr = collections::asArray(obj.get());
      if (arr->empty()) {
        return init_null();
      }
      return arr->getKey(arr->iter_last());
    }
  }
  SystemLib::throwInvalidArgumentExceptionObject(
    "Argument 1 passed to HH\\Lib\\_Private\\Native\\last_key() "
    "must be a Container");
}

namespace {

const StaticString s_Res("i:0;");

struct SerializeOptions {
  bool keepDVArrays = false;
  bool forcePHPArrays = false;
  bool warnOnHackArrays = false;
  bool warnOnPHPArrays = false;
  bool ignoreLateInit = false;
  bool serializeProvenanceAndLegacy = false;
  bool disallowObjects = false;
  bool disallowCollections = false;
  // When serializing a class or lazy class, do not promote them to strings
  bool keepClasses = false;
  bool ignoreStringSizeLimit = false;
  bool useZstd = false;
};

// Apply parsed SerializeOptions to a freshly-constructed serializer.
template <class VS>
ALWAYS_INLINE void apply_serialize_options(
    VS& vs,
    const SerializeOptions& opts,
    bool pure) {
  if (opts.keepDVArrays)        vs.keepDVArrays();
  if (opts.forcePHPArrays)      vs.setForcePHPArrays();
  if (opts.warnOnHackArrays)    vs.setHackWarn();
  if (opts.warnOnPHPArrays)     vs.setPHPWarn();
  if (opts.ignoreLateInit)      vs.setIgnoreLateInit();
  if (opts.serializeProvenanceAndLegacy) vs.setSerializeProvenanceAndLegacy();
  if (opts.disallowObjects)     vs.setDisallowObjects();
  if (opts.disallowCollections) vs.setDisallowCollections();
  if (opts.keepClasses)         vs.setKeepClasses();
  if (opts.ignoreStringSizeLimit) vs.setIgnoreStringSizeLimit();
  if (pure) vs.setPure();
}

ALWAYS_INLINE OptString serialize_impl(const Variant& value,
                                       const SerializeOptions& opts,
                                       bool pure) {
  if (opts.useZstd) {
    if (isStringType(value.getType())) {
      auto const size = value.getStringData()->size();
      if (size >= Cfg::ErrorHandling::MaxSerializedStringSize) {
        throw Exception("Size of serialized string (%ld) exceeds max", size);
      }
    }
    ZStdVariableSerializer vs(VariableSerializer::Type::Serialize);
    apply_serialize_options(vs, opts, pure);
    return vs.serialize(value, /*ret=*/true, /*keepCount=*/true);
  }

  switch (value.getType()) {
    case KindOfClass:
    case KindOfLazyClass:
      if (opts.keepClasses) {
        auto const str = isClassType(value.getType())
          ? value.toClassVal()->name()
          : value.toLazyClassVal().name();
        auto const size = str->size();
        StringBuffer sb;
        sb.append("l:");
        sb.append(size);
        sb.append(":\"");
        sb.append(str->data(), size);
        sb.append("\";");
        return sb.detach();
      }
      [[fallthrough]];
    case KindOfString: {
      auto const op = "serialize()";
      auto const str =
        isStringType(value.getType()) ? value.getStringData() :
        isClassType(value.getType()) ?
          classToStringHelper(value.toClassVal(), op) :
          lazyClassToStringHelper(value.toLazyClassVal(), op);
      auto const size = str->size();
      if (size >= Cfg::ErrorHandling::MaxSerializedStringSize) {
        throw Exception("Size of serialized string (%ld) exceeds max", size);
      }
      StringBuffer sb;
      sb.append("s:");
      sb.append(size);
      sb.append(":\"");
      sb.append(str->data(), size);
      sb.append("\";");
      return sb.detach();
    }
    case KindOfResource:
      return s_Res;

    case KindOfUninit:
    case KindOfNull:
    case KindOfBoolean:
    case KindOfInt64:
    case KindOfFunc:
    case KindOfVec:
    case KindOfDict:
    case KindOfKeyset:
    case KindOfDouble:
    case KindOfObject:
    case KindOfClsMeth:
    case KindOfRClsMeth:
    case KindOfRFunc:
    case KindOfEnumClassLabel:
      break;
  }
  VariableSerializer vs(VariableSerializer::Type::Serialize);
  apply_serialize_options(vs, opts, pure);
  // Keep the count so recursive calls to serialize() embed references properly.
  return vs.serialize(value, true, true);
}

}

const StaticString
  s_forcePHPArrays("forcePHPArrays"),
  s_keepDVArrays("keepDVArrays"),
  s_warnOnHackArrays("warnOnHackArrays"),
  s_warnOnPHPArrays("warnOnPHPArrays"),
  s_ignoreLateInit("ignoreLateInit"),
  s_disallowObjects("disallowObjects"),
  s_disallowCollections("disallowCollections"),
  s_serializeProvenanceAndLegacy("serializeProvenanceAndLegacy"),
  s_ignoreStringSizeLimit("ignoreStringSizeLimit"),
  s_keepClasses("keepClasses"),
  s_zstd("zstd");

namespace {

// Parse the options dict into a SerializeOptions struct.
ALWAYS_INLINE SerializeOptions build_serialize_options(const Array& options) {
  SerializeOptions opts;
  opts.keepDVArrays = options.exists(s_keepDVArrays) &&
    options[s_keepDVArrays].toBoolean();
  opts.forcePHPArrays = options.exists(s_forcePHPArrays) &&
    options[s_forcePHPArrays].toBoolean();
  opts.warnOnHackArrays = options.exists(s_warnOnHackArrays) &&
    options[s_warnOnHackArrays].toBoolean();
  opts.warnOnPHPArrays = options.exists(s_warnOnPHPArrays) &&
    options[s_warnOnPHPArrays].toBoolean();
  opts.ignoreLateInit = options.exists(s_ignoreLateInit) &&
    options[s_ignoreLateInit].toBoolean();
  opts.serializeProvenanceAndLegacy =
    options.exists(s_serializeProvenanceAndLegacy) &&
    options[s_serializeProvenanceAndLegacy].toBoolean();
  opts.disallowObjects = options.exists(s_disallowObjects) &&
    options[s_disallowObjects].toBoolean();
  opts.disallowCollections = options.exists(s_disallowCollections) &&
    options[s_disallowCollections].toBoolean();
  opts.keepClasses = options.exists(s_keepClasses) &&
    options[s_keepClasses].toBoolean();
  opts.ignoreStringSizeLimit = options.exists(s_ignoreStringSizeLimit) &&
    options[s_ignoreStringSizeLimit].toBoolean();
  opts.useZstd = options.exists(s_zstd) && options[s_zstd].toBoolean();
  return opts;
}

}  // namespace

OptString HHVM_FUNCTION(serialize, const Variant& value) {
  return serialize_impl(value, SerializeOptions(), false);
}

OptString HHVM_FUNCTION(serialize_pure, const Variant& value) {
  return serialize_impl(value, SerializeOptions(), true);
}

OptString HHVM_FUNCTION(HH_serialize_with_options,
                        const Variant& value, const Array& options) {
  return serialize_impl(value, build_serialize_options(options), false);
}

OptString serialize_keep_dvarrays(const Variant& value) {
  SerializeOptions opts;
  opts.keepDVArrays = true;
  return serialize_impl(value, opts, false);
}

Variant HHVM_FUNCTION(unserialize, const OptString& str,
                                   const Array& options) {
  if (options.exists(s_zstd) && options[s_zstd].toBoolean()) {
    if (str.get() == nullptr || str.size() <= 0) {
      return false;
    }
    ZStdVariableUnserializer vu(str.data(), str.size(),
                                VariableUnserializer::Type::Serialize,
                                /* allowUnknownSerializableClass = */ true,
                                options);
    try {
      return vu.unserialize();
    } catch (FatalErrorException&) {
      throw;
    } catch (InvalidAllowedClassesException&) {
      raise_warning(
        "unserialize(): allowed_classes option should be array or boolean"
      );
      return false;
    } catch (Exception& e) {
      raise_notice("Unable to unserialize: %s.", e.getMessage().c_str());
      return false;
    }
  }
  return unserialize_from_string(
    str,
    VariableUnserializer::Type::Serialize,
    options
  );
}

Variant HHVM_FUNCTION(unserialize_slice, const OptString& str, int64_t start, int64_t length,
                                   const Array& options) {
  // unserialize_from_buffer accepts empty strings. We ban them here to guarantee behavior
  // consistent with other apis.
  if (start < 0 || length <= 0 || (size_t)start + (size_t)length > (size_t)str.size()) {
    SystemLib::throwInvalidArgumentExceptionObject(
      "Invalid start or length"
    );
  }
  return unserialize_from_buffer(
    str.data()+start,
    length,
    VariableUnserializer::Type::Serialize,
    options,
    false
    );
}

Variant HHVM_FUNCTION(unserialize_pure, const OptString& str,
                                        const Array& options) {
  return unserialize_from_string(
    str,
    VariableUnserializer::Type::Serialize,
    options,
    true
  );
}

///////////////////////////////////////////////////////////////////////////////
// variable table

void HHVM_FUNCTION(parse_str,
                   const OptString& str,
                   Array& arr) {
  arr = Array::CreateDict();
  HttpProtocol::DecodeParameters(arr, str.data(), str.size());
}

/////////////////////////////////////////////////////////////////////////////

bool HHVM_FUNCTION(HH_is_late_init_prop_init,
                   const Object& obj,
                   const OptString& name) {
  auto const func = fromCaller(
    [] (const BTFrame& frm) { return frm.func(); }
  );
  auto const ctx = MemberLookupContext(func->cls(), func->moduleName());
  auto const val = obj->getPropIgnoreLateInit(ctx, name.get());
  if (!val) {
    SystemLib::throwInvalidArgumentExceptionObject(
      fmt::format(
       "Unknown or inaccessible property '{}' on object of class {}",
       name.get()->data(),
       obj->getVMClass()->name()->data()
      )
    );
  }
  return type(val) != KindOfUninit;
}

bool HHVM_FUNCTION(HH_is_late_init_sprop_init,
                   const OptString& clsName,
                   const OptString& name) {
  auto const cls = Class::load(clsName.get());
  if (!cls) {
    SystemLib::throwInvalidArgumentExceptionObject(
      fmt::format("Unknown class {}", clsName.c_str())
    );
  }
  auto const func =fromCaller(
    [] (const BTFrame& frm) { return frm.func(); }
  );
  auto const ctx =  MemberLookupContext(func->cls(), func->unit()->moduleName());
  auto const lookup = cls->getSPropIgnoreLateInit(ctx, name.get());
  if (!lookup.val || !lookup.accessible) {
    SystemLib::throwInvalidArgumentExceptionObject(
      fmt::format(
       "Unknown or inaccessible static property '{}' on class {}",
       name.get()->data(),
       clsName.get()->data()
      )
    );
  }
  return type(lookup.val) != KindOfUninit;
}

bool HHVM_FUNCTION(HH_global_key_exists, StringArg key) {
  return g_context->m_globalNVTable->lookup(key.get()) != nullptr;
}

/////////////////////////////////////////////////////////////////////////////

void StandardExtension::registerNativeVariable() {
  HHVM_FE(is_null);
  HHVM_FE(is_bool);
  HHVM_FE(is_int);
  HHVM_FALIAS(is_integer, is_int);
  HHVM_FALIAS(is_long, is_int);
  HHVM_FE(is_float);
  HHVM_FALIAS(is_double, is_float);
  HHVM_FALIAS(is_real, is_float);
  HHVM_FE(is_numeric);
  HHVM_FE(is_string);
  HHVM_FE(is_scalar);
  HHVM_FE(is_array);
  HHVM_FALIAS(HH\\is_vec, HH_is_vec);
  HHVM_FALIAS(HH\\is_dict, HH_is_dict);
  HHVM_FALIAS(HH\\is_keyset, HH_is_keyset);
  HHVM_FALIAS(HH\\is_varray, HH_is_varray);
  HHVM_FALIAS(HH\\is_darray, HH_is_darray);
  HHVM_FALIAS(HH\\is_any_array, HH_is_any_array);
  HHVM_FALIAS(HH\\is_php_array, HH_is_php_array);
  HHVM_FALIAS(HH\\is_list_like, HH_is_list_like);
  HHVM_FALIAS(HH\\is_meth_caller, HH_is_meth_caller);
  HHVM_FE(is_object);
  HHVM_FE(is_resource);
  HHVM_FE(intval);
  HHVM_FE(floatval);
  HHVM_FALIAS(doubleval, floatval);
  HHVM_FE(strval);
  HHVM_FE(gettype);
  HHVM_FE(get_resource_type);
  HHVM_FE(print_r);
  HHVM_FE(print_r_pure);
  HHVM_FE(print_debug_display);
  HHVM_FE(var_export);
  HHVM_FE(var_export_pure);
  HHVM_FE(debug_zval_dump);
  HHVM_FE(debugger_dump);
  HHVM_FE(var_dump);
  HHVM_FALIAS(serialize, HH_serialize_with_options);
  HHVM_FE(serialize_pure);
  HHVM_FE(unserialize);
  HHVM_FE(unserialize_pure);
  HHVM_FE(unserialize_slice);
  HHVM_FE(parse_str);
  HHVM_FALIAS(HH\\object_prop_array, HH_object_prop_array);
  HHVM_FALIAS(HH\\serialize_with_options, HH_serialize_with_options);
  // Clang 15 doesn't like the HHVM_FALIAS macro with \\N
  HHVM_FALIAS_FE_STR("HH\\Lib\\_Private\\Native\\first", HH_first);
  HHVM_FALIAS_FE_STR("HH\\Lib\\_Private\\Native\\last", HH_last);
  HHVM_FALIAS_FE_STR("HH\\Lib\\_Private\\Native\\first_key", HH_first_key);
  HHVM_FALIAS_FE_STR("HH\\Lib\\_Private\\Native\\last_key", HH_last_key);
  HHVM_FALIAS(HH\\is_late_init_prop_init, HH_is_late_init_prop_init);
  HHVM_FALIAS(HH\\is_late_init_sprop_init, HH_is_late_init_sprop_init);
  HHVM_FALIAS(HH\\global_key_exists, HH_global_key_exists);
}

///////////////////////////////////////////////////////////////////////////////
} // namespace HPHP
