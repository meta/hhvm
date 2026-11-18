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

#include "hphp/runtime/base/type-structure.h"

#include "hphp/runtime/base/array-data.h"
#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/base/array-iterator.h"
#include "hphp/runtime/base/string-data.h"
#include "hphp/runtime/base/typed-value.h"
#include "hphp/runtime/base/type-array.h"
#include "hphp/runtime/base/type-variant.h"
#include "hphp/runtime/base/bespoke/type-structure.h"
#include "hphp/util/configs/eval.h"
#include "hphp/util/text-util.h"

namespace HPHP {

struct OptString;
struct StaticString;

struct AliasRes { AliasKind ak; Array unres; };

/*
 * NB: This resolution logic and HHBBC must always agree exactly. If
 * you change anything here, HHBBC must be changed to match.
 */

namespace {

/* TSEnv holds values that are propagated through the entire resolution process
 * and TSCtx holds values that change every time we traverse through a
 * T_UNRESOLVED during resolution. They are both bags of data that we need at
 * various points, so we package them up to make them easier to pass around. */
struct TSEnv {
  bool allow_partial{};

  // Passed in by ref variables from the caller

  // Initial value true since something can only lose persistence
  bool persistent{true};
  // Initial value false since unless we have to resort to partial
  // evaluation, nothing is partial
  bool partial{};
  // Initial value false since unless proven there are no invalid types
  bool invalidType{};
  // An invalid shape-splat operand can be erased while normalizing the splat.
  bool invalidShapeSplat{};
  // In some cases we cannot allow preresolved type aliases to be reused as it
  // could result in an extra layer of unrolling for case types.
  bool allowPreresolved{true};
  // Vector of typestructures that need to be put in for reified generics
  const req::vector<Array>* tsList;
  // Set of type aliases currently being resolved
  req::fast_map<OptString, AliasRes, hphp_string_hash, hphp_string_same> resolving;
};

struct TSCtx {
  // The name of the type constant or type alias we're resolving; will be null
  // when we're resolving an anonymous type structure.
  const StringData* name = nullptr;

  // If we're resolving a type constant, these are the class that the type
  // constant was requested from, and the class it is declared in, respectively.
  // You will either have neither or both of these.
  const Class* typeCnsCls = nullptr;
  const Class* declCls = nullptr;

  // Generics for the context we're resolving in, if available.
  const ArrayData* generics = nullptr;
};

struct WithCaseType {
  explicit WithCaseType(TSEnv& env)
    : m_env(env)
    , m_allowPreresolved(env.allowPreresolved)
  {
    for (auto it = m_env.resolving.begin(); it != m_env.resolving.end();) {
      switch (it->second.ak) {
      case AliasKind::CaseType:
        ++it;
        break;
      case AliasKind::TypeAlias:
        m_aliases.emplace_back(std::move(*it));
        it = m_env.resolving.erase(it);
        break;
      }
    }
    m_env.allowPreresolved = false;
  }
  ~WithCaseType() {
    for (auto& p : m_aliases) m_env.resolving.emplace(std::move(p));
    m_env.allowPreresolved = m_allowPreresolved;
  }

  WithCaseType(WithCaseType&&) = delete;
  WithCaseType& operator=(WithCaseType&&) = delete;

private:
  TSEnv& m_env;
  bool m_allowPreresolved;
  std::vector<std::pair<OptString, AliasRes>> m_aliases;
};

/*
 * These static strings are the same as the ones in
 * hphp/compiler/type_annotation.cpp, where the typeAnnotArrays are
 * originally generated. See TypeAnnotation::getScalarArrayRep().
 */
const StaticString
  s_nullable("nullable"),
  s_exact("exact"),
  s_name("name"),
  s_classname("classname"),
  s_kind("kind"),
  s_elem_types("elem_types"),
  s_optional_elem_types("optional_elem_types"),
  s_return_type("return_type"),
  s_variadic_type("variadic_type"),
  s_param_types("param_types"),
  s_generic_types("generic_types"),
  s_root_name("root_name"),
  s_access_list("access_list"),
  s_fields("fields"),
  s_splat_elem_types("splat_elem_types"),
  s_allows_unknown_fields("allows_unknown_fields"),
  s_is_cls_cns("is_cls_cns"),
  s_optional_shape_field("optional_shape_field"),
  s_value("value"),
  s_this(annotTypeName(AnnotType::This)),
  s_self("self"),
  s_parent("parent"),
  s_callable("callable"),
  s_alias("alias"),
  s_case_type("case_type"),
  s_typevars("typevars"),
  s_typevar_types("typevar_types"),
  s_id("id"),
  s_soft("soft"),
  s_opaque("opaque"),
  s_union_types("union_types"),
  s_with_refinements("with_refinements"),
  s_equals("equals"),
  s_as("as"),
  s_super("super"),
  s_is_ctx("is_ctx")
;

const std::string
  s_null("null"),
  s_void("void"),
  s_int("int"),
  s_bool("bool"),
  s_float("float"),
  s_string("string"),
  s_resource("resource"),
  s_num("num"),
  s_arraykey("arraykey"),
  s_nothing("nothing"),
  s_noreturn("noreturn"),
  s_mixed("mixed"),
  s_dynamic("dynamic"),
  s_nonnull("nonnull"),
  s_darray(kAnnotTypeDarrayStr),
  s_varray(kAnnotTypeVarrayStr),
  s_varray_or_darray(kAnnotTypeVarrayOrDarrayStr),
  s_shape("shape"),
  s_hh_vec(annotTypeName(AnnotType::Vec)),
  s_hh_dict(annotTypeName(AnnotType::Dict)),
  s_hh_keyset(annotTypeName(AnnotType::Keyset)),
  s_hh_vec_or_dict(annotTypeName(AnnotType::VecOrDict)),
  s_hh_any_array(annotTypeName(AnnotType::ArrayLike)),
  s_hh_class(annotTypeName(AnnotType::Class)),
  s_hh_class_or_classname(annotTypeName(AnnotType::ClassOrClassname)),
  s_hh("HH\\")
;

std::string fullName(const Array& arr, TypeStructure::TSDisplayType type);
void refinementTypeName(const Array& arr, std::string& name,
                        TypeStructure::TSDisplayType type);

void functionTypeName(const Array& arr, std::string& name,
                      TypeStructure::TSDisplayType type) {
  name += "(function (";

  assertx(arr.exists(s_return_type));
  auto const retType = arr[s_return_type].asCArrRef();

  assertx(arr.exists(s_param_types));
  auto const params = arr[s_param_types].asCArrRef();

  auto sep = "";
  auto const sz = params.size();
  for (auto i = 0; i < sz; i++) {
    auto const param = params[i].asCArrRef();
    folly::toAppend(sep, fullName(param, type), &name);
    sep = ", ";
  }

  auto variadic_tv = arr.lookup(s_variadic_type);
  if (variadic_tv.is_init()) {
    auto const variadicType = tvAsVariant(variadic_tv).asCArrRef();
    folly::toAppend(sep, fullName(variadicType, type), "...", &name);
  }

  folly::toAppend("): ", fullName(retType, type), ")", &name);
}

void accessTypeName(const Array& arr, std::string& name) {
  assertx(arr.exists(s_root_name));
  auto const rootName = arr[s_root_name].asCStrRef();
  name += rootName.toCppString();

  assertx(arr.exists(s_access_list));
  auto const accessList = arr[s_access_list].asCArrRef();
  auto const sz = accessList.size();
  for (auto i = 0; i < sz; i++) {
    folly::toAppend("::",
                    accessList[i].asCStrRef().toCppString(),
                    &name);
  }
}

// xhp names are mangled so we get them back to their original definition
// see the mangling in ScannerToken::xhpLabel
void xhpTypeName(const Array& arr, std::string& name) {
  assertx(arr.exists(s_classname));
  std::string clsName = arr[s_classname].asCStrRef().toCppString();
  // remove prefix if any
  if (clsName.compare(0, sizeof("xhp_") - 1, "xhp_") == 0) {
    name += clsName.replace(0, sizeof("xhp_") - 1, ":");
  } else {
    name += clsName;
  }
  // un-mangle back
  replaceAll(name, "__", ":");
  replaceAll(name, "_", "-");
}

void tupleTypeName(const Array& arr, std::string& name,
                   TypeStructure::TSDisplayType type) {
  name += "(";
  assertx(arr.exists(s_elem_types));
  auto const elems = arr[s_elem_types].asCArrRef();
  auto const sz = elems.size();
  auto sep = "";
  for (auto i = 0; i < sz; i++) {
    auto const elem = elems[i].asCArrRef();
    folly::toAppend(sep, fullName(elem, type), &name);
    sep = ", ";
  }
  auto optional_elems_tv = arr.lookup(s_optional_elem_types);
  if (optional_elems_tv.is_init()) {
    auto const sz_optional_elems = optional_elems_tv.val().parr->size();
    for (auto i = 0; i < sz_optional_elems; i++) {
      auto const elem = tvAsVariant(optional_elems_tv).asCArrRef()[i].asCArrRef();
      folly::toAppend(sep, "optional ", fullName(elem, type), &name);
      sep = ", ";
    }
  }
  auto variadic_tv = arr.lookup(s_variadic_type);
  if (variadic_tv.is_init()) {
    auto const variadicType = tvAsVariant(variadic_tv).asCArrRef();
    folly::toAppend(sep, fullName(variadicType, type), "...", &name);
  }

  name += ")";
}

void genericTypeName(const Array& arr, std::string& name,
                     TypeStructure::TSDisplayType type) {
  auto args_tv = arr.lookup(s_generic_types);
  if (!args_tv.is_init()) return;
  name += "<";
  auto const args = tvAsVariant(args_tv).asCArrRef();
  auto const sz = args.size();
  auto sep = "";
  for (auto i = 0; i < sz; i++) {
    auto const arg = args[i].asCArrRef();
    folly::toAppend(sep, fullName(arg, type), &name);
    sep = ", ";
  }
  name += ">";
}

void unionTypeName(const Array& arr, std::string& name,
                   TypeStructure::TSDisplayType type) {
  auto union_tv = arr.lookup(s_union_types);
  if (!union_tv.is_init()) return;
  auto const unions = tvAsVariant(union_tv).asCArrRef();
  auto const sz = unions.size();
  auto sep = "";
  for (auto i = 0; i < sz; i++) {
    auto const ty = unions[i].asCArrRef();
    folly::toAppend(sep, fullName(ty, type), &name);
    sep = " | ";
  }
}

bool forDisplay(TypeStructure::TSDisplayType type) {
  return type != TypeStructure::TSDisplayType::TSDisplayTypeReflection;
}

void appendShapeFields(const Array& fields, std::string& name,
                       TypeStructure::TSDisplayType type, const char*& sep) {
  auto const sz = fields.size();
  for (auto i = 0; i < sz; i++) {
    name += sep;
    auto const field = fields->getKey(i);
    auto value = fields->getValue(i).asCArrRef();
    auto quote = "'";
    if (value.exists(s_optional_shape_field)) {
      name += "?";
    }
    if (value.exists(s_value)) {
      // if unresolved, ignore wrapper
      if (value.exists(s_is_cls_cns)) quote = "";
      value = value[s_value].asCArrRef();
    }
    auto const fieldType = field.getType();
    if (isStringType(fieldType)) {
      folly::toAppend(quote, field.asCStrRef().data(), quote, &name);
    } else if (isIntType(fieldType)) {
      folly::toAppend(field.toInt64Val(), &name);
    }

    folly::toAppend(
      forDisplay(type) ? " => " : "=>",
      fullName(value, type),
      &name
    );
    sep = ", ";
  }
}

// Display an unresolved shape splat, reconstructing the source-like form from
// its ordered element list: inline field-runs, `...T` splat operands, and the
// trailing `...` / `T...` open element.
void shapeSplatTypeName(const Array& arr, std::string& name,
                        TypeStructure::TSDisplayType type) {
  auto const elems = arr[s_splat_elem_types].asCArrRef();
  auto const sz = elems.size();
  auto sep = "";
  for (auto i = 0; i < sz; i++) {
    auto const elem = elems->getValue(i).asCArrRef();
    auto const kind = TypeStructure::kind(elem);
    if (kind == TypeStructure::Kind::T_shape &&
        (elem.exists(s_splat_elem_types) ||
         elem.exists(s_allows_unknown_fields) ||
         elem[s_fields].asCArrRef().empty())) {
      // A nested unresolved splat, open shape, or empty closed shape remains a
      // splat operand. An open shape's variadic bound belongs to that operand;
      // hoisting it into the outer shape would move it across later operands.
      // An empty shape has no fields to inline, so omitting it would erase the
      // assigned identity operand entirely.
      name += sep;
      folly::toAppend("...", fullName(elem, type), &name);
      sep = ", ";
    } else if (kind == TypeStructure::Kind::T_shape) {
      // an inline run of literal fields
      appendShapeFields(elem[s_fields].asCArrRef(), name, type, sep);
    } else {
      // a splat operand: `...T`
      name += sep;
      folly::toAppend("...", fullName(elem, type), &name);
      sep = ", ";
    }
  }
}

void shapeTypeName(const Array& arr, std::string& name,
                   TypeStructure::TSDisplayType type) {
  // works for both resolved and unresolved TypeStructures
  name += "(";
  if (arr.exists(s_splat_elem_types)) {
    shapeSplatTypeName(arr, name, type);
    name += ")";
    return;
  }
  assertx(arr.exists(s_fields));
  auto sep = "";
  appendShapeFields(arr[s_fields].asCArrRef(), name, type, sep);

  if (arr.exists(s_allows_unknown_fields)) {
    name += sep;
    auto const v = arr.lookup(s_variadic_type);
    if (v.is_init()) name += fullName(tvAsCVarRef(&v).toArray(), type);
    name += "...";
  }

  name += ")";
}

std::string fullName(const Array& arr, TypeStructure::TSDisplayType type) {
  std::string name;

  if (arr.exists(s_nullable)) {
    assertx(arr[s_nullable].toBoolean());
    name += '?';
  }

  if (arr.exists(s_soft)) {
    assertx(arr[s_soft].toBoolean());
    name += '@';
  }

  if (type == TypeStructure::TSDisplayType::TSDisplayTypeInternal) {
    if (arr.exists(s_opaque)) {
      assertx(arr[s_opaque].toBoolean());
      name += "opaque:";
    }
    auto tv = arr.lookup(s_alias);
    if (!tv.is_init()) tv = arr.lookup(s_case_type);
    if (tv.is_init()) {
      auto const alias = tvAsVariant(tv).asCStrRef();
      name += alias.toCppString() + ':';
    }

    tv = arr.lookup(s_typevar_types);
    if (tv.is_init()) {
      name += "typevar_types[";
      auto const types = tvAsVariant(tv).asCArrRef();
      bool first = true;
      for (ArrayIter iter(types); iter; ++iter, first=false) {
        auto key = iter.first();
        auto value = iter.second();
        if (!first) name += ",";
        name += key.asCStrRef().toCppString() + ":" + fullName(value.asCArrRef(), type);
      }
      name += "]:";
    }
  }

  auto addTypeAndMaybeNamespace = [](std::string& name, TypeStructure::TSDisplayType type, const std::string& typeStr) {
    if (!forDisplay(type)) {
      name += s_hh;
    }
    name += typeStr;
  };

  TypeStructure::Kind kind = TypeStructure::kind(arr);
  switch (kind) {
    case TypeStructure::Kind::T_null:
      addTypeAndMaybeNamespace(name, type, s_null);
      break;
    case TypeStructure::Kind::T_void:
      addTypeAndMaybeNamespace(name, type, s_void);
      break;
    case TypeStructure::Kind::T_int:
      addTypeAndMaybeNamespace(name, type, s_int);
      break;
    case TypeStructure::Kind::T_bool:
      addTypeAndMaybeNamespace(name, type, s_bool);
      break;
    case TypeStructure::Kind::T_float:
      addTypeAndMaybeNamespace(name, type, s_float);
      break;
    case TypeStructure::Kind::T_string:
      addTypeAndMaybeNamespace(name, type, s_string);
      break;
    case TypeStructure::Kind::T_resource:
      addTypeAndMaybeNamespace(name, type, s_resource);
      break;
    case TypeStructure::Kind::T_num:
      addTypeAndMaybeNamespace(name, type, s_num);
      break;
    case TypeStructure::Kind::T_arraykey:
      addTypeAndMaybeNamespace(name, type, s_arraykey);
      break;
    case TypeStructure::Kind::T_nothing:
      addTypeAndMaybeNamespace(name, type, s_nothing);
      break;
    case TypeStructure::Kind::T_noreturn:
      addTypeAndMaybeNamespace(name, type, s_noreturn);
      break;
    case TypeStructure::Kind::T_mixed:
      addTypeAndMaybeNamespace(name, type, s_mixed);
      break;
    case TypeStructure::Kind::T_dynamic:
      addTypeAndMaybeNamespace(name, type, s_dynamic);
      break;
    case TypeStructure::Kind::T_nonnull:
      addTypeAndMaybeNamespace(name, type, s_nonnull);
      break;
    case TypeStructure::Kind::T_tuple:
      tupleTypeName(arr, name, type);
      break;
    case TypeStructure::Kind::T_fun:
      functionTypeName(arr, name, type);
      break;
    case TypeStructure::Kind::T_darray:
      name += s_darray;
      genericTypeName(arr, name, type);
      break;
    case TypeStructure::Kind::T_varray:
      name += s_varray;
      genericTypeName(arr, name, type);
      break;
    case TypeStructure::Kind::T_varray_or_darray:
      name += s_varray_or_darray;
      genericTypeName(arr, name, type);
      break;
    case TypeStructure::Kind::T_shape:
      addTypeAndMaybeNamespace(name, type, s_shape);
      shapeTypeName(arr, name, type);
      break;
    case TypeStructure::Kind::T_dict:
      name += s_hh_dict;
      genericTypeName(arr, name, type);
      break;
    case TypeStructure::Kind::T_vec:
      name += s_hh_vec;
      genericTypeName(arr, name, type);
      break;
    case TypeStructure::Kind::T_keyset:
      name += s_hh_keyset;
      genericTypeName(arr, name, type);
      break;
    case TypeStructure::Kind::T_vec_or_dict:
      name += s_hh_vec_or_dict;
      genericTypeName(arr, name, type);
      break;
    case TypeStructure::Kind::T_any_array:
      name += s_hh_any_array;
      genericTypeName(arr, name, type);
      break;
    case TypeStructure::Kind::T_typevar:
      assertx(arr.exists(s_name));
      name += arr[s_name].asCStrRef().toCppString();
      break;
    case TypeStructure::Kind::T_typeaccess:
      accessTypeName(arr, name);
      break;
    case TypeStructure::Kind::T_xhp:
      xhpTypeName(arr, name);
      break;
    case TypeStructure::Kind::T_reifiedtype:
      assertx(arr.exists(s_id));
      name += "reified ";
      name += arr[s_id].asCStrRef().data();
      break;
    case TypeStructure::Kind::T_union:
      assertx(arr.exists(s_union_types));
      unionTypeName(arr, name, type);
      break;
    case TypeStructure::Kind::T_recursiveUnion:
      name += arr[s_case_type].asCStrRef().toCppString();
      break;
    case TypeStructure::Kind::T_class:
    case TypeStructure::Kind::T_interface:
    case TypeStructure::Kind::T_trait:
    case TypeStructure::Kind::T_enum:
    case TypeStructure::Kind::T_unresolved:
      assertx(arr.exists(s_classname));
      name += arr[s_classname].asCStrRef().toCppString();
      genericTypeName(arr, name, type);
      if (arr.exists(s_with_refinements)) {
        refinementTypeName(arr, name, type);
      }
      break;
    case TypeStructure::Kind::T_class_ptr:
      name += s_hh_class;
      break;
    case TypeStructure::Kind::T_class_or_classname:
      name += s_hh_class_or_classname;
      break;
  }

  return name;
}

void refinementTypeName(const Array& arr, std::string& name,
                        TypeStructure::TSDisplayType type) {
  name += " with { ";

  assertx(arr.exists(s_with_refinements));
  auto const refinements = arr[s_with_refinements].asCArrRef();
  auto sep = "";
  IterateKV(refinements.get(), [&](TypedValue k, TypedValue v) {
    assertx(tvIsString(k));
    assertx(tvIsDict(v));
    auto const memberName = k.m_data.pstr->toCppString();
    auto const memberArr = Array(v.m_data.parr);

    auto const isCtx = memberArr.lookup(s_is_ctx);
    bool ctx = isCtx.is_init() && isCtx.val().num;

    folly::toAppend(sep, ctx ? "ctx " : "type ", memberName, &name);

    auto const exact = memberArr.lookup(s_equals);
    if (exact.is_init()) {
      assertx(tvIsDict(exact));
      folly::toAppend(" = ", fullName(Array(exact.val().parr), type), &name);
    } else {
      auto const upper = memberArr.lookup(s_as);
      if (upper.is_init()) {
        assertx(tvIsVec(upper));
        auto const upperArr = Array(upper.val().parr);
        IterateV(upperArr.get(), [&](TypedValue bound) {
          folly::toAppend(" as ", fullName(Array(bound.m_data.parr), type), &name);
        });
      }
      auto const lower = memberArr.lookup(s_super);
      if (lower.is_init()) {
        assertx(tvIsVec(lower));
        auto const lowerArr = Array(lower.val().parr);
        IterateV(lowerArr.get(), [&](TypedValue bound) {
          folly::toAppend(" super ", fullName(Array(bound.m_data.parr), type), &name);
        });
      }
    }

    sep = "; ";
  });
  name += " }";
}

Array resolveTS(TSEnv& env, const TSCtx& ctx, const Array& arr,
                ArrayData* typevarTypes);

Array resolveTSImpl(TSEnv& env, const TSCtx& ctx, const Array& arr);

Array maybeMakeBespoke(Array& ts) {
  if (Cfg::Eval::EmitBespokeTypeStructures &&
      bespoke::TypeStructure::isValidTypeStructure(ts.get())) {
    auto const bespokeTS = bespoke::TypeStructure::MakeFromVanilla(ts.get());
    if (bespokeTS) return Array::attach(bespokeTS);
  }
  return ts;
}

Array resolveList(TSEnv& env, const TSCtx& ctx, const Array& arr) {
  auto const sz = arr.size();

  VecInit newarr(sz);
  for (auto i = 0; i < sz; i++) {
    newarr.append(Variant(resolveTS(env, ctx, arr[i].toArray(), nullptr)));
  }

  return newarr.toArray();
}

void resolveRefinementTypes(TSEnv& env, const TSCtx& ctx,
                            const Array& arr, Array& newarr) {
  if (!arr.exists(s_with_refinements)) return;
  auto const refinements = arr[s_with_refinements].asCArrRef();
  auto resolvedRefinements = Array::CreateDict();
  IterateKV(refinements.get(), [&](TypedValue k, TypedValue v) {
    assertx(tvIsString(k));
    assertx(tvIsDict(v));
    auto const memberArr = Array(v.m_data.parr);
    auto resolvedMember = Array::CreateDict();

    auto const isCtx = memberArr.lookup(s_is_ctx);
    if (isCtx.is_init()) {
      resolvedMember.set(s_is_ctx, isCtx);
    }

    auto const exact = memberArr.lookup(s_equals);
    if (exact.is_init()) {
      assertx(tvIsDict(exact));
      resolvedMember.set(s_equals,
        Variant(resolveTS(env, ctx, Array(exact.val().parr), nullptr)));
    }

    auto const upper = memberArr.lookup(s_as);
    if (upper.is_init()) {
      assertx(tvIsVec(upper));
      resolvedMember.set(s_as,
        Variant(resolveList(env, ctx, Array(upper.val().parr))));
    }

    auto const lower = memberArr.lookup(s_super);
    if (lower.is_init()) {
      assertx(tvIsVec(lower));
      resolvedMember.set(s_super,
        Variant(resolveList(env, ctx, Array(lower.val().parr))));
    }

    resolvedRefinements.set(
      OptString(k.m_data.pstr), Variant(resolvedMember));
  });
  newarr.set(s_with_refinements, Variant(resolvedRefinements));
}

std::string resolveContextMsg(const TSCtx& ctx) {
  std::string msg("when resolving ");
  if (!ctx.name) {
    folly::toAppend("anonymous type structure", &msg);
  } else if (ctx.typeCnsCls) {
    folly::toAppend("type constant ", ctx.typeCnsCls->name()->data(),
                    "::", ctx.name->data(), &msg);
  } else {
    folly::toAppend("type alias ", ctx.name->data(), &msg);
  }
  return msg;
}

/*
 * Returns the type-structure for the alias with the given name. If
 * the name is not an alias, return an empty Array. The bool is
 * whether the returned type-structure is already resolved or not.
*/
std::pair<Array, bool> getAlias(TSEnv& env, const OptString& aliasName,
                                bool usePreResolved) {

  if (aliasName.same(s_this) || Class::lookup(aliasName.get())) {
    return std::make_pair(Array::CreateDict(), false);
  }

  if (env.resolving.contains(aliasName)) {
    // The alias is currently being resolved and we are hitting a recursion.
    // give error
    throw Exception(
      "Alias %s is being used recursively",
      aliasName.data());
  }

  auto persistentTA = true;
  auto typeAlias = env.allow_partial
    ? TypeAlias::lookup(aliasName.get(), &persistentTA)
    : TypeAlias::load(aliasName.get(), &persistentTA);
  if (!typeAlias) {
    env.partial = true;
    return std::make_pair(Array::CreateDict(), false);
  }

  env.persistent &= persistentTA;

  if (usePreResolved && env.allowPreresolved) {
    auto const& preresolved = typeAlias->resolvedTypeStructure();
    if (!preresolved.isNull()) {
      assertx(preresolved.isDict());
      assertx(!preresolved.empty());
      return std::make_pair(preresolved, true);
    }
  }

  // this returned type structure is unresolved.
  assertx(typeAlias->typeStructure().isDict());
  assertx(!typeAlias->typeStructure().empty());
  return std::make_pair(typeAlias->typeStructure(), false);
}

const Class* getClass(TSEnv& env, const TSCtx& ctx, const OptString& clsName) {
  auto checkPersistent = [&](const Class* cls) {
    env.persistent &= classHasPersistentRDS(cls);
    return cls;
  };

  // the original unresolved type structure came from a type constant
  // (instead of a type alias), and may have this/self/parent.
  if (ctx.typeCnsCls) {
    // HH\this: late static binding
    if (clsName.same(s_this)) {
      return checkPersistent(ctx.typeCnsCls);
    }

    // self
    if (clsName.same(s_self)) {
      return checkPersistent(ctx.declCls);
    }
    // parent
    if (clsName.same(s_parent)) {
      auto parent = ctx.declCls->parent();
      if (!parent) {
        throw Exception(
          "%s, class %s does not have a parent",
          resolveContextMsg(ctx).c_str(),
          ctx.declCls->name()->data());
      }
      return checkPersistent(parent);
    }
  }

  auto name = clsName;
  // We can't use preresolved aliases here in order to keep the error
  // message the same.
  auto ts = getAlias(env, name, false).first;
  while (!ts.empty()) {
    assertx(ts.exists(s_kind));
    if (!ts.exists(s_classname)) {
      // not a class, interface, trait, enum, or alias
      throw Exception(
        "%s, alias %s does not resolve to a class",
        resolveContextMsg(ctx).c_str(),
        name.data());
    }
    name = ts[s_classname].asCStrRef();
    ts = getAlias(env, name, false).first;
  }

  auto const cls = env.allow_partial ? Class::lookup(name.get())
                                     : Class::load(name.get());
  if (!cls) {
    env.partial = true;
    if (env.allow_partial) return nullptr;
    throw Exception(
      "%s, class %s not found",
      resolveContextMsg(ctx).c_str(),
      name.data());
  }

  return checkPersistent(cls);
}

/* Given an unresolved T_shape TypeStructure, returns the __fields__
 * portion of the array with all the field names resolved to string
 * literals. */
Array resolveShape(TSEnv& env, const TSCtx& ctx, const Array& arr) {
  assertx(TypeStructure::kind(arr) == TypeStructure::Kind::T_shape);
  assertx(arr.exists(s_fields));

  auto newfields = Array::CreateDict();
  auto const fields = arr[s_fields].asCArrRef();
  auto const sz = fields.size();
  for (auto i = 0; i < sz; i++) {
    Variant key = fields->getKey(i);
    auto const wrapper = fields->getValue(i).asCArrRef();
    if (wrapper.exists(s_is_cls_cns)) {
      // if the shape field name is a class constant, its name is
      // double colon delimited clsName::cnsName
      auto const clsCns = key.asCStrRef().toCppString();
      std::string clsName, cnsName;
      folly::split("::", clsCns, clsName, cnsName);

      // look up clsName::cnsName
      auto cls = getClass(env, ctx, OptString(clsName));
      if (!cls) throw Exception("failed to resolve shape classname");
      auto cnsValue = tvClassToString(cls->clsCnsGet(OptString(cnsName).get()));

      if (isStringType(cnsValue.m_type) || isIntType(cnsValue.m_type)) {
        key = tvAsVariant(&cnsValue);
      } else {
        throw Exception(
          "class constant %s::%s is not a string or an int "
          "and cannot be used as shape field names",
          clsName.c_str(), cnsName.c_str());
      }
    }

    // If the TS was resolved before then value field no longer exists, the
    // value is instead flattened.

    auto tv = wrapper.lookup(s_value);
    auto valueArr = tv.is_init() ? tvAsVariant(tv).toArray() : wrapper;
    auto value = resolveTSImpl(env, ctx, valueArr);

    if (wrapper.exists(s_optional_shape_field)) {
      value.set(s_optional_shape_field, make_tv<KindOfBoolean>(true));
    }

    newfields.set(key, Variant(maybeMakeBespoke(value)));
  }

  return newfields;
}

//////////////////////////////////////////////////////////////////////
// Shape splat merging.
//
// Runtime counterpart of the representable subset of OCaml
// `Typing_shape_normalize`. Given a list of already-resolved shape type
// structures (the operands of a shape splat), merge them left-to-right with
// rightmost-wins semantics. Intersections have no runtime type-structure kind
// and are currently emitted as mixed, so they remain invalid residuals here.

namespace {

Array tsOfKind(TypeStructure::Kind k) {
  auto a = Array::CreateDict();
  TypeStructure::setKind(a, k);
  return a;
}

bool isBottomTypeStructure(const Array& ts) {
  return TypeStructure::kind(ts) == TypeStructure::Kind::T_nothing &&
         !ts.exists(s_nullable);
}

// The type of a shape's unknown fields: its variadic_type, else mixed if open,
// else nothing (closed).
Array unknownValueOf(const Array& shape) {
  if (!shape.exists(s_allows_unknown_fields)) {
    return tsOfKind(TypeStructure::Kind::T_nothing);
  }
  auto const v = shape.lookup(s_variadic_type);
  if (v.is_init()) return tvAsCVarRef(&v).toArray();
  return tsOfKind(TypeStructure::Kind::T_mixed);
}

// Union of two (non-optional) type structures, with the simplifications that
// make closed (nothing) and open (mixed) merges exact.
// TODO(shape-splat): general union fidelity (no dedup/flatten of distinct
// types).
Array unionTS(const Array& a, const Array& b) {
  auto const ak = TypeStructure::kind(a);
  auto const bk = TypeStructure::kind(b);
  // `nothing` is the union identity.
  if (isBottomTypeStructure(a)) return b;
  if (isBottomTypeStructure(b)) return a;
  // `mixed` is the union top.
  if (ak == TypeStructure::Kind::T_mixed ||
      bk == TypeStructure::Kind::T_mixed) {
    return tsOfKind(TypeStructure::Kind::T_mixed);
  }
  auto u = Array::CreateDict();
  TypeStructure::setKind(u, TypeStructure::Kind::T_union);
  VecInit types(2);
  types.append(Variant(a));
  types.append(Variant(b));
  u.set(s_union_types, Variant(types.toArray()));
  return u;
}

// Strip the optional marker from a shape field value, leaving its type.
Array typePartOf(const Array& fieldVal) {
  if (!fieldVal.exists(s_optional_shape_field)) return fieldVal;
  auto copy = fieldVal;
  copy.remove(s_optional_shape_field);
  return copy;
}

// The field descriptor for a key absent from a shape: optional, with the
// shape's unknown-fields type.
Array optionalFieldOf(const Array& unknownTS) {
  auto f = unknownTS;
  f.set(s_optional_shape_field, make_tv<KindOfBoolean>(true));
  return f;
}

// Merge two shape field values under rightmost-wins:
//   (_,        Req t) -> Req t
//   (Req l,    Opt r) -> Req (l|r)
//   (Opt l,    Opt r) -> Opt (l|r)
Array mergeFieldValue(const Array& leftVal, const Array& rightVal) {
  auto const leftOpt = leftVal.exists(s_optional_shape_field);
  auto const rightOpt = rightVal.exists(s_optional_shape_field);
  if (!rightOpt) return rightVal;
  auto merged = unionTS(typePartOf(leftVal), typePartOf(rightVal));
  if (leftOpt) merged.set(s_optional_shape_field, make_tv<KindOfBoolean>(true));
  return merged;
}

// Merge a resolved shape `right` into the accumulator (fields + unknown value).
void mergeShapeInto(Array& accFields, Array& accUnknown, const Array& right) {
  auto const emptyFields = Array::CreateDict();
  auto const rightFields = right.exists(s_fields)
    ? right[s_fields].asCArrRef() : emptyFields;
  auto const rightUnknown = unknownValueOf(right);
  auto const accUnknownOpt = optionalFieldOf(accUnknown);

  // A missing key projects to an optional field at the row's unknown bound.
  auto merged = Array::CreateDict();
  IterateKV(accFields.get(), [&](TypedValue k, TypedValue v) {
    auto const leftVal = tvAsCVarRef(&v).toArray();
    auto const rv = rightFields.lookup(k);
    auto const rightVal = rv.is_init()
      ? tvAsCVarRef(&rv).toArray() : optionalFieldOf(rightUnknown);
    merged.set(tvAsCVarRef(&k), Variant(mergeFieldValue(leftVal, rightVal)));
  });
  IterateKV(rightFields.get(), [&](TypedValue k, TypedValue v) {
    if (accFields.exists(k)) return;
    auto const rightVal = tvAsCVarRef(&v).toArray();
    merged.set(tvAsCVarRef(&k),
               Variant(mergeFieldValue(accUnknownOpt, rightVal)));
  });

  accFields = merged;
  // Either row may provide an unknown key, so their bounds union.
  accUnknown = unionTS(accUnknown, rightUnknown);
}

// Build the resolved shape denoted by the merge accumulator (fields + unknown
// value).
Array shapeOfAccumulator(const Array& accFields, const Array& accUnknown) {
  auto result = Array::CreateDict();
  TypeStructure::setKind(result, TypeStructure::Kind::T_shape);
  result.set(s_fields, Variant(accFields));
  if (!isBottomTypeStructure(accUnknown)) {
    result.set(s_allows_unknown_fields, make_tv<KindOfBoolean>(true));
    if (TypeStructure::kind(accUnknown) != TypeStructure::Kind::T_mixed) {
      result.set(s_variadic_type, Variant(accUnknown));
    }
  }
  return result;
}

bool isEmptyClosedShape(const Array& shape) {
  return TypeStructure::kind(shape) == TypeStructure::Kind::T_shape &&
         !shape.exists(s_nullable) &&
         shape.exists(s_fields) &&
         shape[s_fields].asCArrRef().empty() &&
         isBottomTypeStructure(unknownValueOf(shape));
}

Array shapeSplatOfElements(const req::vector<Array>& elems) {
  auto result = Array::CreateDict();
  TypeStructure::setKind(result, TypeStructure::Kind::T_shape);
  VecInit v(elems.size());
  for (auto const& elem : elems) v.append(Variant(elem));
  result.set(s_splat_elem_types, Variant(v.toArray()));
  return result;
}

// Combine the per-member branch types of a distributed union into one resolved
// type. Nested unions (a branch that itself distributed a union) are flattened;
// `nothing` branches are dropped (bottom is the unit of union). Mirrors OCaml
// `Typing_union.union_list`, modulo deeper simplification: the runtime does not
// run Hack's union simplifier, so members are not deduped.
Array unionOfBranches(const req::vector<Array>& branches) {
  req::vector<Array> flat;
  flat.reserve(branches.size());
  for (auto const& b : branches) {
    auto const bk = TypeStructure::kind(b);
    if (isBottomTypeStructure(b)) continue;
    if (bk == TypeStructure::Kind::T_union) {
      auto const inner = b[s_union_types].asCArrRef();
      auto const n = inner.size();
      for (auto i = 0; i < n; i++) {
        auto const iv = inner->nvGetVal(i);
        flat.push_back(tvAsCVarRef(&iv).toArray());
      }
      continue;
    }
    flat.push_back(b);
  }
  if (flat.empty()) return tsOfKind(TypeStructure::Kind::T_nothing);
  if (flat.size() == 1) return flat[0];
  auto u = Array::CreateDict();
  TypeStructure::setKind(u, TypeStructure::Kind::T_union);
  VecInit v(flat.size());
  for (auto const& m : flat) v.append(Variant(m));
  u.set(s_union_types, Variant(v.toArray()));
  return u;
}

// Result normalization can short-circuit on bottom, but invalid operands must
// still be recorded regardless of their position in the element list.
void markInvalidShapeSplatOperand(const Array& elem, bool& invalidType) {
  auto const k = TypeStructure::kind(elem);
  if (elem.exists(s_nullable)) {
    invalidType = true;
    return;
  }
  if (k == TypeStructure::Kind::T_dynamic) {
    return;
  }
  if (k == TypeStructure::Kind::T_shape) {
    auto const nested = elem.lookup(s_splat_elem_types);
    if (!nested.is_init()) {
      // A non-splat shape must have fields, even when the shape is empty.
      if (!elem.exists(s_fields)) invalidType = true;
      return;
    }
    auto const elems = tvAsCVarRef(&nested).toArray();
    auto const n = elems.size();
    for (auto i = 0; i < n; i++) {
      auto const member = elems->nvGetVal(i);
      markInvalidShapeSplatOperand(
        tvAsCVarRef(&member).toArray(), invalidType
      );
    }
    return;
  }
  if (k == TypeStructure::Kind::T_nothing) return;
  if (k == TypeStructure::Kind::T_union) {
    auto const members = elem[s_union_types].asCArrRef();
    auto const n = members.size();
    for (auto i = 0; i < n; i++) {
      auto const member = members->nvGetVal(i);
      markInvalidShapeSplatOperand(
        tvAsCVarRef(&member).toArray(), invalidType
      );
    }
    return;
  }
  invalidType = true;
}

Array mergeResolvedShapeSplatImpl(const req::vector<Array>& elems,
                                  bool& invalidType);

// Preserve an invalid operand as a barrier while normalizing everything to its
// right. A distributed suffix must retain the same residual prefix.
Array mergeResidualShapeSplat(const req::vector<Array>& elems,
                              size_t residualIndex,
                              const Array& accFields,
                              const Array& accUnknown,
                              bool& invalidType) {
  invalidType = true;

  auto const hasPrefix =
    accFields.size() != 0 || !isBottomTypeStructure(accUnknown);
  req::vector<Array> residualElems;
  residualElems.reserve(
    elems.size() - residualIndex + (hasPrefix ? 1 : 0)
  );
  if (hasPrefix) {
    residualElems.push_back(shapeOfAccumulator(accFields, accUnknown));
  }
  residualElems.push_back(elems[residualIndex]);

  if (residualIndex + 1 == elems.size()) {
    return shapeSplatOfElements(residualElems);
  }

  req::vector<Array> suffix;
  suffix.reserve(elems.size() - (residualIndex + 1));
  for (size_t i = residualIndex + 1; i < elems.size(); i++) {
    suffix.push_back(elems[i]);
  }
  auto const merged = mergeResolvedShapeSplatImpl(suffix, invalidType);
  auto const mergedKind = TypeStructure::kind(merged);
  if (mergedKind == TypeStructure::Kind::T_nothing) {
    // Bottom absorbs the residual prefix.
    return tsOfKind(TypeStructure::Kind::T_nothing);
  }
  if (mergedKind == TypeStructure::Kind::T_union) {
    // Every distributed suffix branch must retain the residual prefix.
    auto const members = merged[s_union_types].asCArrRef();
    req::vector<Array> branches;
    branches.reserve(members.size());
    auto const n = members.size();
    for (auto i = 0; i < n; i++) {
      auto branchElems = residualElems;
      auto const member = members->nvGetVal(i);
      branchElems.push_back(tvAsCVarRef(&member).toArray());
      branches.push_back(
        mergeResolvedShapeSplatImpl(branchElems, invalidType)
      );
    }
    return unionOfBranches(branches);
  }
  if (merged.exists(s_splat_elem_types)) {
    // Splice a residual suffix to keep the element list flat.
    auto const inner = merged[s_splat_elem_types].asCArrRef();
    auto const n = inner.size();
    for (auto i = 0; i < n; i++) {
      auto const member = inner->nvGetVal(i);
      residualElems.push_back(tvAsCVarRef(&member).toArray());
    }
  } else if (!isEmptyClosedShape(merged)) {
    // The empty closed shape is the merge identity.
    residualElems.push_back(merged);
  }
  return shapeSplatOfElements(residualElems);
}

Array mergeResolvedShapeSplatImpl(const req::vector<Array>& elems,
                                  bool& invalidType) {
  auto accFields = Array::CreateDict();
  auto accUnknown = tsOfKind(TypeStructure::Kind::T_nothing);

  for (size_t i = 0; i < elems.size(); i++) {
    auto const& elem = elems[i];
    auto const k = TypeStructure::kind(elem);
    auto const nullable = elem.exists(s_nullable);
    if (k == TypeStructure::Kind::T_nothing && !nullable) {
      // Spreading nothing yields the bottom row, which absorbs everything to
      // its right; Hack has no canonical bottom shape, so collapse to nothing.
      return tsOfKind(TypeStructure::Kind::T_nothing);
    }
    if (k == TypeStructure::Kind::T_dynamic && !nullable) {
      // Spreading `dynamic` yields an open row whose unknown fields are
      // `dynamic` (i.e. `shape(dynamic...)`): merge it as an open shape whose
      // variadic (unknown-field) type is `dynamic`. This unions any concrete
      // field to its left with `dynamic` and makes the accumulated unknown-field
      // bound `dynamic`. Mirrors OCaml `Typing_shape_normalize`'s dynamic arms.
      auto dynOpen = tsOfKind(TypeStructure::Kind::T_shape);
      dynOpen.set(s_allows_unknown_fields, make_tv<KindOfBoolean>(true));
      dynOpen.set(s_variadic_type,
                  Variant(tsOfKind(TypeStructure::Kind::T_dynamic)));
      mergeShapeInto(accFields, accUnknown, dynOpen);
      continue;
    }
    if (k == TypeStructure::Kind::T_shape &&
        !nullable &&
        elem.exists(s_splat_elem_types)) {
      // Flatten in place so shapes across the nested boundary remain adjacent.
      auto const nested = elem[s_splat_elem_types].asCArrRef();
      auto const hasPrefix =
        accFields.size() != 0 || !isBottomTypeStructure(accUnknown);
      req::vector<Array> flattened;
      flattened.reserve(
        (hasPrefix ? 1 : 0) + nested.size() + elems.size() - (i + 1)
      );
      if (hasPrefix) {
        flattened.push_back(shapeOfAccumulator(accFields, accUnknown));
      }
      auto const nn = nested.size();
      for (auto ni = 0; ni < nn; ni++) {
        auto const nv = nested->nvGetVal(ni);
        flattened.push_back(tvAsCVarRef(&nv).toArray());
      }
      for (size_t j = i + 1; j < elems.size(); j++) {
        flattened.push_back(elems[j]);
      }
      return mergeResolvedShapeSplatImpl(flattened, invalidType);
    }
    if (k == TypeStructure::Kind::T_union && !nullable) {
      // Distribute the union outward:
      //   shape(...A, ...(m1|...|mk), ...B)
      //     = shape(...A, ...m1, ...B) | ... | shape(...A, ...mk, ...B).
      // The prefix (...A) is the current accumulator as a shape; the suffix
      // (...B) is the remaining element list. Fork per member — re-merge
      // [prefix, member, suffix...] to completion — then union the finalised
      // branches. Doing this here (rather than as a pre-pass) lets a member that
      // is itself a union/nothing/dynamic go through the full pipeline, and a
      // `nothing` branch correctly absorb. The Hack typechecker performs the same
      // distribution during normalization.
      auto const prefix = shapeOfAccumulator(accFields, accUnknown);
      auto const members = elem[s_union_types].asCArrRef();
      auto const mn = members.size();
      req::vector<Array> branches;
      branches.reserve(mn);
      for (auto mi = 0; mi < mn; mi++) {
        auto const mv = members->nvGetVal(mi);
        req::vector<Array> branchElems;
        branchElems.reserve(elems.size() - i + 1);
        branchElems.push_back(prefix);
        branchElems.push_back(tvAsCVarRef(&mv).toArray());
        for (size_t j = i + 1; j < elems.size(); j++) {
          branchElems.push_back(elems[j]);
        }
        branches.push_back(mergeResolvedShapeSplatImpl(branchElems, invalidType));
      }
      return unionOfBranches(branches);
    }
    if (k == TypeStructure::Kind::T_shape &&
        !nullable &&
        elem.exists(s_fields)) {
      // A resolved simple shape contributes fields and its unknown-row bound.
      mergeShapeInto(accFields, accUnknown, elem);
      continue;
    }

    // Every remaining operand is non-shape, nullable, or malformed. Preserve
    // it as a barrier between the merged prefix and the normalized suffix.
    return mergeResidualShapeSplat(
      elems, i, accFields, accUnknown, invalidType
    );
  }

  return shapeOfAccumulator(accFields, accUnknown);
}

// Resolve each splat element, then merge.
Array resolveShapeSplat(TSEnv& env, const TSCtx& ctx, const Array& arr) {
  auto const elemsArr = arr[s_splat_elem_types].asCArrRef();
  req::vector<Array> resolved;
  auto const n = elemsArr.size();
  resolved.reserve(n);
  for (auto i = 0; i < n; i++) {
    auto const ev = elemsArr->nvGetVal(i);
    resolved.push_back(resolveTSImpl(env, ctx, tvAsCVarRef(&ev).toArray()));
  }
  bool invalidType = false;
  auto result = TypeStructure::mergeResolvedShapeSplat(resolved, invalidType);
  env.invalidType |= invalidType;
  env.invalidShapeSplat |= invalidType;
  return result;
}

} // namespace

bool resolveClass(TSEnv& env, const TSCtx& ctx, Array& ret,
                  const OptString& clsName) {
  auto const cls = getClass(env, ctx, clsName);
  if (!cls) return false;

  TypeStructure::Kind resolvedKind;
  if (isNormalClass(cls) || isEnumClass(cls)) {
    resolvedKind = TypeStructure::Kind::T_class;
  } else if (isInterface(cls)) {
    resolvedKind = TypeStructure::Kind::T_interface;
  } else if (isTrait(cls)) {
    resolvedKind = TypeStructure::Kind::T_trait;
    env.invalidType = true;
  } else if (isEnum(cls)) {
    resolvedKind = TypeStructure::Kind::T_enum;
  } else {
    not_reached();
  }

  TypeStructure::setKind(ret, resolvedKind);
  ret.set(s_classname, Variant(makeStaticString(cls->name())));
  if (clsName.same(s_this)) ret.set(s_exact, make_tv<KindOfBoolean>(true));

  return true;
}

Array resolveGenerics(TSEnv& env, const TSCtx& ctx, const Array& arr) {
  return resolveList(env, ctx, arr[s_generic_types].toArray());
}

Array resolveUnion(TSEnv& env, const TSCtx& ctx, const Array& arr) {
  WithCaseType _{env};
  return resolveList(env, ctx, arr[s_union_types].toArray());
}

/**
 * Copy modifiers, i.e. whether the type is nullable, soft, or a like-type.
 */
void copyTypeModifiers(const Array& from, Array& to) {
  if (from.exists(s_nullable)) to.set(s_nullable, make_tv<KindOfBoolean>(true));
  if (from.exists(s_soft))     to.set(s_soft, make_tv<KindOfBoolean>(true));
}

Array resolveTSImpl(TSEnv& env, const TSCtx& ctx, const Array& arr) {
  auto const kind = TypeStructure::kind(arr);

  auto newarr = Array::CreateDict();
  copyTypeModifiers(arr, newarr);
  TypeStructure::setKind(newarr, kind);

  if (arr.exists(s_allows_unknown_fields)) {
    newarr.set(s_allows_unknown_fields, make_tv<KindOfBoolean>(true));
  }

  switch (kind) {
    case TypeStructure::Kind::T_tuple: {
      assertx(arr.exists(s_elem_types));
      auto const elemsArr = arr[s_elem_types].asCArrRef();
      newarr.set(s_elem_types, Variant(resolveList(env, ctx, elemsArr)));
      if (arr.exists(s_optional_elem_types)) {
        auto const optionalElemsArr = arr[s_optional_elem_types].asCArrRef();
        newarr.set(s_optional_elem_types, Variant(resolveList(env, ctx, optionalElemsArr)));
      }
      auto tv = arr.lookup(s_variadic_type);
      if (tv.is_init()) {
        auto const variadicArr = tvAsVariant(tv).asCArrRef();
        newarr.set(s_variadic_type, Variant(resolveTS(env, ctx, variadicArr, nullptr)));
      }
      break;
    }
    case TypeStructure::Kind::T_fun: {
      env.invalidType = true;
      assertx(arr.exists(s_return_type));
      auto const returnArr = arr[s_return_type].asCArrRef();
      newarr.set(s_return_type, Variant(resolveTS(env, ctx, returnArr, nullptr)));

      assertx(arr.exists(s_param_types));
      auto const paramsArr = arr[s_param_types].asCArrRef();
      newarr.set(s_param_types, Variant(resolveList(env, ctx, paramsArr)));

      auto tv = arr.lookup(s_variadic_type);
      if (tv.is_init()) {
        newarr.set(s_variadic_type, Variant(resolveTS(env, ctx, tvAsVariant(tv).asCArrRef(), nullptr)));
      }

      break;
    }
    case TypeStructure::Kind::T_darray:
    case TypeStructure::Kind::T_varray:
    case TypeStructure::Kind::T_varray_or_darray:
    case TypeStructure::Kind::T_dict:
    case TypeStructure::Kind::T_vec:
    case TypeStructure::Kind::T_keyset:
    case TypeStructure::Kind::T_vec_or_dict:
    case TypeStructure::Kind::T_any_array: {
      if (kind == TypeStructure::Kind::T_darray ||
        kind == TypeStructure::Kind::T_varray ||
        kind == TypeStructure::Kind::T_varray_or_darray
      ) {
        env.invalidType = true;
      }
      if (arr.exists(s_generic_types)) {
        newarr.set(s_generic_types, Variant(resolveGenerics(env, ctx, arr)));
      }
      break;
    }
    case TypeStructure::Kind::T_shape: {
      if (arr.exists(s_splat_elem_types)) {
        // A shape splat: resolve and merge the element list, then carry over
        // the outer modifiers (nullable/soft) onto the merged result.
        newarr = resolveShapeSplat(env, ctx, arr);
        copyTypeModifiers(arr, newarr);
        break;
      }
      newarr.set(s_fields, Variant(resolveShape(env, ctx, arr)));
      auto tv = arr.lookup(s_variadic_type);
      if (tv.is_init()) {
        auto const variadicArr = tvAsVariant(tv).asCArrRef();
        newarr.set(s_variadic_type, Variant(resolveTS(env, ctx, variadicArr, nullptr)));
      }
      break;
    }
    case TypeStructure::Kind::T_unresolved: {
      assertx(arr.exists(s_classname));
      auto const clsName = arr[s_classname].asCStrRef();

      auto const resTypevars = [&] (const Array& ts) {
        if (ts.exists(s_typevars) && arr.exists(s_generic_types)) {
          // If the alias has typevars and the original type has typevars then
          // we need to convert them over.
          std::vector<std::string> typevars;
          folly::split(',', ts[s_typevars].asCStrRef().data(), typevars);

          auto generic_types = resolveGenerics(env, ctx, arr);

          auto const sz = std::min(static_cast<ssize_t>(typevars.size()),
                                   generic_types.size());
          DictInit newarr(sz);
          for (auto i = 0; i < sz; i++) {
            newarr.set(OptString(typevars[i]), generic_types[i]);
          }
          auto generics = newarr.toArray();
          return generics;
        }
        // Either the alias doesn't have typevars OR the original type doesn't
        // have typevars.
        return Array{};
      };

      auto ty = env.resolving.find(clsName);
      if (ty != env.resolving.end()) {
        // The alias is currently being resolved and we are hitting a
        // recursion.
        switch (ty->second.ak) {
          case AliasKind::TypeAlias: {
            // This is what getAlias() would do anyway.
            throw Exception(
              "Alias %s is being used recursively",
              clsName.data());
          }
          case AliasKind::CaseType: {
            // This is a recursive case type. It's allowed - but we just need to
            // set a few fields.
            DictInit res(4);
            TypeStructure::setKind(res, TypeStructure::Kind::T_recursiveUnion);
            res.set(s_case_type, clsName);
            if (arr.exists(s_alias)) {
              res.set(s_alias, Variant{arr[s_alias].asCStrRef()});
            }
            auto const tvs = resTypevars(ty->second.unres);
            if (!tvs.isNull()) res.set(s_typevar_types, tvs);
            return res.toArray();
          }
        }
      }

      auto [ts, preresolved] = getAlias(env, clsName, true);
      if (!ts.empty()) {
        auto resolveAlias = [&, &ts=ts, preresolved=preresolved] (ArrayData* generics) {
          // If it's already resolved, don't do so again.
          if (preresolved) return ts;
          TSCtx newCtx;
          newCtx.name = clsName.get();
          newCtx.generics = generics;

          AliasKind ak = AliasKind::TypeAlias;
          if (ts.exists(s_case_type)) {
            ak = AliasKind::CaseType;
          }
          auto const [_, inserted] = env.resolving.insert_or_assign(
            clsName,
            AliasRes { ak, ts }
          );

          always_assert(inserted);
          auto resolved = resolveTS(env, newCtx, ts, generics);

          // Hack should disallow a situation where an alias is bound with free
          // type variables but there are a few situations where we may trigger
          // this in the frontend compiler.
          resolved.remove(s_typevars);

          assertx(!resolved.isNull());
          env.resolving.erase(clsName);
          return resolved;
        };

        ts = resolveAlias(resTypevars(ts).get());

        // copy the alias typestruct onto the returned typestruct.
        newarr = ts;
        // copy modifiers (nullable & soft) from the source typestruct to
        // the result.
        copyTypeModifiers(arr, newarr);
        break;
      }

      /* Special cases for 'callable': Hack typechecker throws a naming error
       * (unbound name), however, hhvm still supports this type hint to be
       * compatible with php. We simply return as a OF_CLASS with class name
       * set to 'callable'. */
      if (clsName.same(s_callable)) {
        TypeStructure::setKind(newarr, TypeStructure::Kind::T_class);
        newarr.set(s_classname, Variant(clsName));
        break;
      }
      if (!resolveClass(env, ctx, newarr, clsName) && env.allow_partial) {
        env.partial = true;
        TypeStructure::setKind(newarr, TypeStructure::Kind::T_unresolved);
        newarr.set(s_classname, Variant(clsName));
        break;
      }
      if (arr.exists(s_generic_types)) {
        newarr.set(s_generic_types, Variant(resolveGenerics(env, ctx, arr)));
      }
      resolveRefinementTypes(env, ctx, arr, newarr);
      break;
    }
    case TypeStructure::Kind::T_typeaccess: {
      /* type access is a root class (may be HH\this) followed by a series of
       * type constants, i.e., cls::TC1::TC2::TC3. Each type constant other
       * than the last one in the chain must refer to a class or an
       * interface. */
      assertx(arr.exists(s_root_name));
      auto clsName = arr[s_root_name].asCStrRef();
      assertx(arr.exists(s_access_list));
      auto const accList = arr[s_access_list].asCArrRef();
      auto const sz = accList.size();
      Array typeCnsVal;
      for (auto i = 0; i < sz; i++) {
        auto const cls = getClass(env, ctx, clsName);
        if (!cls) throw Exception("failed to resolve type access");
        auto const cnsName = accList[i].asCStrRef();
        if (!cls->hasTypeConstant(cnsName.get())) {
          throw Exception(
            "%s, class %s does not have non-abstract "
            "type constant %s",
            resolveContextMsg(ctx).c_str(),
            clsName.data(),
            cnsName.data());
        }
        auto tv = cls->clsCnsGet(cnsName.get(), ConstModifierFlags::Kind::Type,
                                 !env.allow_partial);
        if (!tv.is_init()) {
          assertx(env.allow_partial);
          throw Exception(
            "Failed to resolve a type constant: %s::%s",
            cls->name()->data(), cnsName.get()->data()
          );
        }
        assertx(isArrayLikeType(tv.m_type));
        typeCnsVal = Array(tv.m_data.parr);
        assertx(typeCnsVal.isDict());
        if (i == sz - 1) break;

        // if there are more accesses, keep resolving
        auto const typeCnsKind = TypeStructure::kind(typeCnsVal);
        if (typeCnsKind != TypeStructure::Kind::T_class
            && typeCnsKind != TypeStructure::Kind::T_interface
            && typeCnsKind != TypeStructure::Kind::T_xhp
            && typeCnsKind != TypeStructure::Kind::T_trait) {
          throw Exception(
            "%s, %s::%s does not resolve to a class or "
            "an interface and cannot contain type constant %s",
            resolveContextMsg(ctx).c_str(),
            clsName.data(),
            cnsName.data(),
            accList[i+1].asCStrRef().data());
        }
        assertx(typeCnsVal.exists(s_classname));
        clsName = typeCnsVal[s_classname].asCStrRef();
      }
      copyTypeModifiers(arr, typeCnsVal);
      newarr = typeCnsVal;
      break;
    }
    case TypeStructure::Kind::T_typevar: {
      env.invalidType = true;
      if (!ctx.generics) return arr.toDict();
      assertx(arr.exists(s_name));
      auto const generic = ctx.generics->get(arr[s_name].asCStrRef().get());
      if (!generic.is_init()) return arr.toDict();
      newarr = Variant::wrap(generic).toDict();
      break;
    }
    case TypeStructure::Kind::T_reifiedtype: {
      assertx(env.tsList != nullptr);
      assertx(arr.exists(s_id));
      auto id = arr.lookup(s_id).val().num;
      assertx(id < env.tsList->size());
      // Overwrite data in newarr with fields in the reified generics type.
      IterateKV(env.tsList->at(id).get(), [&](auto key, auto val) {
        newarr.set(key, val);
      });
      // The spliced `tsList` entry is an unresolved type structure. If it is
      // itself a shape splat (a reified operand `...T` where `T`'s argument is a
      // splat shape), it still carries `splat_elem_types` and no `s_fields`; the
      // shape-splat merge would silently treat it as the empty shape. Re-resolve
      // so the nested splat is merged before this element reaches any outer
      // merge. Reachable only once the `shape_splat_type_parameters` gate + the
      // is/as ban are lifted (a reified operand cannot appear in is/as today), so
      // this is defensive; other reified arguments are left untouched.
      if (TypeStructure::kind(newarr) == TypeStructure::Kind::T_shape &&
          newarr.exists(s_splat_elem_types)) {
        // Use the full resolver so the reified argument's modifiers and alias
        // metadata survive shape-splat normalization.
        newarr = resolveTSImpl(env, ctx, newarr);
        copyTypeModifiers(arr, newarr);
      }
      break;
    }
    case TypeStructure::Kind::T_union:
      assertx(arr.exists(s_union_types));
      newarr.set(s_union_types, Variant(resolveUnion(env, ctx, arr)));
      break;

    case TypeStructure::Kind::T_class_ptr:
    case TypeStructure::Kind::T_class_or_classname:
      // TODO(T199611023) Resolve generics here when we're ready to allow this
      env.invalidType = true;
      break;

    case TypeStructure::Kind::T_xhp:
    case TypeStructure::Kind::T_void:
    case TypeStructure::Kind::T_int:
    case TypeStructure::Kind::T_bool:
    case TypeStructure::Kind::T_float:
    case TypeStructure::Kind::T_string:
    case TypeStructure::Kind::T_resource:
    case TypeStructure::Kind::T_num:
    case TypeStructure::Kind::T_arraykey:
    case TypeStructure::Kind::T_noreturn:
    case TypeStructure::Kind::T_mixed:
    case TypeStructure::Kind::T_class:
    case TypeStructure::Kind::T_interface:
    case TypeStructure::Kind::T_trait:
    case TypeStructure::Kind::T_enum:
    case TypeStructure::Kind::T_nonnull:
    case TypeStructure::Kind::T_null:
    case TypeStructure::Kind::T_nothing:
    case TypeStructure::Kind::T_dynamic:
    case TypeStructure::Kind::T_recursiveUnion:
      // Return the original type structure, no resolution needed
      return arr.toDict();
  }

  if (arr.exists(s_typevars)) newarr.set(s_typevars, arr[s_typevars]);
  if (arr.exists(s_alias)) {
    newarr.set(s_alias, Variant{arr[s_alias].asCStrRef()});
  }
  if (arr.exists(s_case_type)) {
    newarr.set(s_case_type, Variant{arr[s_case_type].asCStrRef()});
  }

  return newarr;
}

Array resolveTS(TSEnv& env, const TSCtx& ctx, const Array& arr,
                ArrayData* typevarTypes) {
  auto ts = resolveTSImpl(env, ctx, arr);
  if (typevarTypes) ts.set(s_typevar_types, Variant(typevarTypes));

  return maybeMakeBespoke(ts);
}

} // anonymous namespace

std::string xhpNameFromTS(const Array& arr) {
  std::string name;
  xhpTypeName(arr, name);
  return name;
}

OptString TypeStructure::toString(const Array& arr, TSDisplayType type) {
  if (arr.empty()) return OptString();
  return OptString(fullName(arr, type));
}

/*
 * Constructs a scalar array with all the shape field names, this/self/parent,
 * classes, type accesses, and type aliases resolved.
 */
Array TypeStructure::resolve(const ArrayData* ts,
                             const StringData* clsName,
                             const Class* declCls,
                             const Class* typeCnsCls,
                             bool& persistent) {
  TSEnv env;
  TSCtx ctx;
  ctx.name = clsName;
  ctx.declCls = declCls;
  ctx.typeCnsCls = typeCnsCls;
  auto resolved = resolveTS(env, ctx, ArrNR(ts), nullptr);
  persistent = env.persistent;
  return resolved;
}

/*
 * Called by TypeAlias to get resolved TypeStructure for type aliases.
 */
Array TypeStructure::resolve(const OptString& aliasName,
                             const Array& arr,
                             bool& persistent,
                             const Array& generics) {
  AliasKind ak =
    (TypeStructure::kind(arr) == TypeStructureKind::T_union)
    ? AliasKind::CaseType
    : AliasKind::TypeAlias;

  TSEnv env;
  env.resolving.insert_or_assign(aliasName, AliasRes { ak, arr });

  TSCtx ctx;
  ctx.name = aliasName.get();
  ctx.generics = generics.get();
  auto resolved = resolveTS(env, ctx, arr, nullptr);
  persistent = env.persistent;
  return resolved;
}

/*
 * Called when resolving anonymous type structures, e.g. from `is` expressions.
 */
Array TypeStructure::resolve(const Array& ts,
                             const Class* typeCnsCls,
                             const Class* declCls,
                             const req::vector<Array>& tsList,
                             bool& persistent) {
  bool invalidShapeSplat;
  return resolve(
    ts, typeCnsCls, declCls, tsList, persistent, invalidShapeSplat
  );
}

Array TypeStructure::resolve(const Array& ts,
                             const Class* typeCnsCls,
                             const Class* declCls,
                             const req::vector<Array>& tsList,
                             bool& persistent,
                             bool& invalidShapeSplat) {
  TSEnv env;
  env.tsList = &tsList;
  TSCtx ctx;
  ctx.declCls = declCls;
  ctx.typeCnsCls = typeCnsCls;
  auto resolved = resolveTS(env, ctx, ts, nullptr);
  persistent = env.persistent;
  invalidShapeSplat = env.invalidShapeSplat;
  return resolved;
}

/*
 * Called when resolving anonymous type structures, e.g. from `is` expressions
 * while allowing partial resolution.
 */
Array TypeStructure::resolvePartial(const Array& ts,
                                    const Class* typeCnsCls,
                                    const Class* declCls,
                                    bool& persistent,
                                    bool& partial,
                                    bool& invalidType) {
  TSEnv env;
  env.allow_partial = true;
  TSCtx ctx;
  ctx.declCls = declCls;
  ctx.typeCnsCls = typeCnsCls;
  auto resolved = resolveTS(env, ctx, ts, nullptr);
  persistent = env.persistent;
  partial = env.partial;
  invalidType = env.invalidType;
  return resolved;
}

Array TypeStructure::mergeResolvedShapeSplat(
    const req::vector<Array>& resolvedElems,
    bool& invalidType) {
  for (auto const& elem : resolvedElems) {
    markInvalidShapeSplatOperand(elem, invalidType);
  }
  return mergeResolvedShapeSplatImpl(resolvedElems, invalidType);
}

namespace {

// Coerces vector-like darrays / dicts to varrays / vecs. Returns true if the
// lval is now a varray (either coerced, or if it started off that way).
bool coerceToVecOrVArray(tv_lval lval) {
  if (tvIsVec(lval)) return true;

  // Must be a dict or darray with vector-like data.
  if (!tvIsDict(lval)) return false;
  auto const ad = val(lval).parr;
  if (!ad->isVectorData()) return false;

  // Do the coercion and replace the value.
  auto const varray = ad->toVec(/*copy=*/true);
  tvCopy(make_array_like_tv(varray), lval);
  assertx(ad != varray);
  decRefArr(ad);
  return true;
}

bool coerceToTypeStructure(Array& arr);
bool coerceToTypeStructureList(Array& arr, bool shape=false);

// Returns true and performs coercion if the given field of `arr` can be
// coerced to a valid, resolved TypeStructure.
bool coerceTSField(Array& arr, const OptString& name) {
  assertx(!arr->cowCheck());
  auto field = arr.lookup(name);
  if (!tvIsDict(field)) return false;
  return coerceToTypeStructure(ArrNR(val(field).parr).asArray());
}

// Returns true and performs coercion if the given field of `arr` can be
// coerced to a list of valid, resolved TypeStructures.
bool coerceTSListField(Array& arr, const OptString& name, bool shape=false) {
  assertx(!arr->cowCheck());
  if (!arr.exists(name)) return false;
  auto field = arr.lval(name);
  if (!coerceToVecOrVArray(field)) return false;
  assertx(tvIsVec(field));
  return coerceToTypeStructureList(ArrNR(val(field).parr).asArray(), shape);
}

// Same as above, except that they allow the field to be missing.
bool coerceOptTSField(Array& arr, const OptString& name) {
  assertx(!arr->cowCheck());
  auto field = arr.lookup(name);
  if (!field.is_init()) return true;
  if (!tvIsDict(field)) return false;
  return coerceToTypeStructure(ArrNR(val(field).parr).asArray());
}
bool coerceOptTSListField(Array& arr, const OptString& name, bool shape=false) {
  assertx(!arr->cowCheck());
  if (!arr.exists(name)) return true;
  auto field = arr.lval(name);
  if (!coerceToVecOrVArray(field)) return false;
  assertx(tvIsVec(field));
  return coerceToTypeStructureList(ArrNR(val(field).parr).asArray(), shape);
}

bool coerceToTypeStructureList(Array& arr, bool shape) {
  assertx(arr->empty() || !arr->cowCheck());
  if (!arr->isVecType()) return false;

  auto valid = true;
  IterateV(arr.get(), [&](TypedValue tv) {
    if (!tvIsDict(tv)) {
      valid = false;
      return true;
    }
    auto const ad = [&] {
      if (shape) {
        auto const value = arr.lookup(s_value);
        if (tvIsDict(value)) return val(value).parr;
        if (value.is_init()) valid = false;
      }
      return val(tv).parr;
    }();
    valid = valid && coerceToTypeStructure(ArrNR(ad).asArray());
    return !valid;
  });
  return valid;
}

bool coerceToTypeStructure(Array& arr) {
  assertx(arr->empty() || !arr->cowCheck());
  if (!arr->isDictType()) return false;

  auto const kindfield = TypeStructure::optKind(arr.get());
  if (!kindfield.has_value()) return false;
  switch (*kindfield) {
    case TypeStructure::Kind::T_darray:
    case TypeStructure::Kind::T_varray:
    case TypeStructure::Kind::T_varray_or_darray:
    case TypeStructure::Kind::T_dict:
    case TypeStructure::Kind::T_vec:
    case TypeStructure::Kind::T_keyset:
    case TypeStructure::Kind::T_vec_or_dict:
    case TypeStructure::Kind::T_any_array:
    case TypeStructure::Kind::T_null:
    case TypeStructure::Kind::T_void:
    case TypeStructure::Kind::T_int:
    case TypeStructure::Kind::T_bool:
    case TypeStructure::Kind::T_float:
    case TypeStructure::Kind::T_string:
    case TypeStructure::Kind::T_resource:
    case TypeStructure::Kind::T_num:
    case TypeStructure::Kind::T_arraykey:
    case TypeStructure::Kind::T_nothing:
    case TypeStructure::Kind::T_noreturn:
    case TypeStructure::Kind::T_mixed:
    case TypeStructure::Kind::T_dynamic:
    case TypeStructure::Kind::T_nonnull:
    case TypeStructure::Kind::T_recursiveUnion:
    // TODO(T199611023) This must change when we enforce the inner class type
    case TypeStructure::Kind::T_class_ptr:
    case TypeStructure::Kind::T_class_or_classname: {
      return true;
    }
    case TypeStructure::Kind::T_fun: {
      return coerceTSField(arr, s_return_type) &&
             coerceTSListField(arr, s_param_types) &&
             coerceOptTSField(arr, s_variadic_type);
    }
    case TypeStructure::Kind::T_typevar: {
      return tvIsString(arr.lookup(s_name));
    }
    case TypeStructure::Kind::T_shape: {
      return coerceTSListField(arr, s_fields, /*shape=*/true) &&
             coerceOptTSField(arr, s_variadic_type);
    }
    case TypeStructure::Kind::T_tuple: {
      return coerceTSListField(arr, s_elem_types) &&
      coerceOptTSListField(arr, s_optional_elem_types) &&
      coerceOptTSField(arr, s_variadic_type);
    }
    case TypeStructure::Kind::T_class:
    case TypeStructure::Kind::T_interface:
    case TypeStructure::Kind::T_trait:
    case TypeStructure::Kind::T_enum: {
      if (!tvIsString(arr.lookup(s_classname)) ||
          !coerceOptTSListField(arr, s_generic_types)) {
        return false;
      }
      auto const refinements = arr.lookup(s_with_refinements);
      if (!refinements.is_init()) return true;
      if (!tvIsDict(refinements)) return false;
      auto valid = true;
      IterateKV(val(refinements).parr, [&](TypedValue k, TypedValue v) {
        if (!tvIsString(k) || !tvIsDict(v)) { valid = false; return true; }
        auto memberArr = ArrNR(val(v).parr).asArray();
        auto const exact = memberArr.lookup(s_equals);
        if (exact.is_init()) {
          if (!tvIsDict(exact) ||
              !coerceToTypeStructure(ArrNR(val(exact).parr).asArray())) {
            valid = false;
            return true;
          }
        }
        if (!coerceOptTSListField(memberArr, s_as) ||
            !coerceOptTSListField(memberArr, s_super)) {
          valid = false;
          return true;
        }
        return false;
      });
      return valid;
    }
    case TypeStructure::Kind::T_union: {
      return coerceTSListField(arr, s_union_types, /*shape=*/false);
    }
    case TypeStructure::Kind::T_unresolved:
    case TypeStructure::Kind::T_typeaccess:
    case TypeStructure::Kind::T_xhp:
    case TypeStructure::Kind::T_reifiedtype: {
      // Unresolved types should not appear in valid resolved type structures.
      return false;
    }
  }
  return false;
}

}

bool TypeStructure::coerceToTypeStructureList_SERDE_ONLY(tv_lval lval) {
  return coerceToVecOrVArray(lval) &&
         coerceToTypeStructureList(ArrNR(val(lval).parr).asArray());
}

Variant typeStructureKindToVariant(TypeStructureKind kind) {
  return Variant(static_cast<uint8_t>(kind));
}

TypeStructureKind TypeStructure::kind(const ArrayData* arr) {
  assertx(arr->exists(s_kind));
  return TypeStructure::Kind(tvCastToInt64(arr->get(s_kind)));
}

Optional<TypeStructureKind> TypeStructure::optKind(const ArrayData* arr) {
  auto const kindfield = arr->get(s_kind);
  if (!isIntType(kindfield.type()) ||
      kindfield.val().num > TypeStructure::kMaxResolvedKind) {
    return std::nullopt;
  }
  return TypeStructure::Kind(kindfield.val().num);
}

void TypeStructure::setKind(Array& arr, TypeStructureKind kind) {
  arr.set(s_kind, Variant(static_cast<uint8_t>(kind)));
}

void TypeStructure::setKind(DictInit& arr, TypeStructureKind kind) {
  arr.set(s_kind, Variant(static_cast<uint8_t>(kind)));
}

} // namespace HPHP
