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

#include "hphp/runtime/base/type-structure-helpers.h"
#include "hphp/runtime/base/type-structure-helpers-defs.h"

#include <fmt/format.h>
#include <folly/Random.h>

#include "hphp/runtime/base/array-data.h"
#include "hphp/runtime/base/array-iterator.h"
#include "hphp/runtime/base/builtin-functions.h"
#include "hphp/runtime/base/enum-util.h"
#include "hphp/runtime/base/tv-comparisons.h"
#include "hphp/runtime/base/typed-value.h"
#include "hphp/runtime/base/type-structure.h"
#include "hphp/runtime/base/unit-cache.h"

#include "hphp/runtime/vm/class-meth-data-ref.h"
#include "hphp/runtime/vm/reified-generics.h"
#include "hphp/runtime/vm/type-constraint.h"
#include "hphp/runtime/vm/vm-regs.h"

#include "hphp/system/systemlib.h"

#include "hphp/util/configs/eval.h"
#include "hphp/util/random.h"

namespace HPHP {

const StaticString s_unresolved("[unresolved]");
const StaticString s_allows_unknown_fields("allows_unknown_fields");
const StaticString s_elem_types("elem_types");
const StaticString s_optional_elem_types("optional_elem_types");
const StaticString s_param_types("param_types");
const StaticString s_return_type("return_type");
const StaticString s_variadic_type("variadic_type");
const StaticString s_splat_elem_types("splat_elem_types");
const StaticString s_fields("fields");
const StaticString s_kind("kind");
const StaticString s_value("value");
const StaticString s_nullable("nullable");
const StaticString s_soft("soft");
const StaticString s_opaque("opaque");
const StaticString s_optional_shape_field("optional_shape_field");
const StaticString s_classname("classname");
const StaticString s_wildcard("_");
const StaticString s_name("name");
const StaticString s_generic_types("generic_types");
const StaticString s_is_cls_cns("is_cls_cns");
const StaticString s_access_list("access_list");
const StaticString s_root_name("root_name");
const StaticString s_alias("alias");
const StaticString s_case_type("case_type");
const StaticString s_callable("callable");
const StaticString s_exact("exact");
const StaticString s_typevars("typevars");
const StaticString s_typevar_types("typevar_types");
const StaticString s_union_types("union_types");
const StaticString s_hh_this(annotTypeName(AnnotType::This));
const StaticString s_type_structure_non_existant_class(
  "HH\\__internal\\type_structure_non_existant_class");
const StaticString s_with_refinements("with_refinements");
const StaticString s_equals("equals");
const StaticString s_as("as");
const StaticString s_super("super");
const StaticString s_is_ctx("is_ctx");

// Fixed error messages
const StaticString s_reified_type_must_be_ts(
  "Reified type must be a type structure");
const StaticString s_new_instance_of_not_string(
  "You cannot create a new instance of this type as it is not a string");

namespace {

template<typename F>
bool tvInstanceOfImpl(const TypedValue* tv, F lookupClass) {
  switch (tv->m_type) {
    case KindOfUninit:
    case KindOfNull:
    case KindOfBoolean:
    case KindOfResource:
    case KindOfRFunc:
    case KindOfClsMeth:
    case KindOfRClsMeth:
    case KindOfEnumClassLabel:
      return false;

    case KindOfClass:
    case KindOfLazyClass: {
      auto const cls = lookupClass();
      if (cls && interface_supports_string(cls->name())) {
        if (folly::Random::oneIn(Cfg::Eval::RaiseClassConversionNoticeSampleRate, threadLocalRng64())) {
          // TODO(vmladenov) appears untested
          raise_class_to_string_conversion_notice("instanceof");
        }
        return true;
      }
      return false;
    }

    case KindOfFunc: {
      return false;
    }

    case KindOfInt64: {
      auto const cls = lookupClass();
      return cls && interface_supports_int(cls->name());
    }

    case KindOfDouble: {
      auto const cls = lookupClass();
      return cls && interface_supports_double(cls->name());
    }

    case KindOfString: {
      auto const cls = lookupClass();
      return cls && interface_supports_string(cls->name());
    }

    case KindOfVec:
    case KindOfDict:
    case KindOfKeyset: {
      auto const cls = lookupClass();
      return cls && interface_supports_arrlike(cls->name());
    }

    case KindOfObject: {
      auto const cls = lookupClass();
      return cls && tv->m_data.pobj->instanceof(cls);
    }
  }
  not_reached();
}

} // namespace

bool tvInstanceOf(const TypedValue* tv, const NamedType* ne) {
  return tvInstanceOfImpl(tv, [ne]() { return ne->getCachedClass(); });
}

bool tvInstanceOf(const TypedValue* tv, const Class* cls) {
  return tvInstanceOfImpl(tv, [cls]() { return cls; });
}

namespace {

ALWAYS_INLINE
bool shapeAllowsUnknownFields(const Array& ts) {
  auto const tv = ts.lookup(s_allows_unknown_fields);
  return tv.is_init() && tv.val().num;
}

ALWAYS_INLINE
bool isOptionalShapeField(const ArrayData* field) {
  auto const property = s_optional_shape_field.get();
  auto const tv = field->get(property);
  return tvToBool(tv);
}

ALWAYS_INLINE
ArrayData* getShapeFieldElement(const TypedValue& v) {
  assertx(tvIsDict(&v));
  auto const result = v.m_data.parr;
  auto const valueField = result->get(s_value.get());
  if (!valueField.is_init()) return result;
  assertx(tvIsDict(valueField));
  return valueField.val().parr;
}

ALWAYS_INLINE
Optional<ArrayData*> getGenericTypesOpt(const ArrayData* ts) {
  auto const generics_field = ts->get(s_generic_types.get());
  if (!generics_field.is_init()) return std::nullopt;
  assertx(tvIsVec(generics_field));
  return generics_field.val().parr;
}

bool typeStructureIsType(
  const ArrayData* input,
  const ArrayData* type,
  bool& warn,
  bool strict
);

bool typeStructureIsTypeList(
  const ArrayData*,
  const ArrayData*,
  const StringData*,
  bool&,
  bool
);

bool caseTypeIsType(
  const StringData* inputCaseType,
  const ArrayData* input,
  const StringData* typeCaseType,
  const ArrayData* type,
  bool& warn,
  bool strict
) {
  if (!inputCaseType) return false;
  if (!typeCaseType) return false;
  if (!inputCaseType->tsame(typeCaseType)) {
    return false;
  }

  bool result = true;

  auto inputTypevar = get_ts_typevar_types_opt(input);
  auto typeTypevar = get_ts_typevar_types_opt(type);
  if (inputTypevar || typeTypevar) {
    if (!inputTypevar) return false;
    if (!typeTypevar) return false;

    IterateKV(
      inputTypevar,
      [&](TypedValue inputK, TypedValue inputV) {
        if (!tvIsString(inputK) || !tvIsArrayLike(inputV)) {
          result = false;
          return true;
        }
        auto typeV = typeTypevar->get(inputK.m_data.pstr);
        if (!typeV.is_init()) {
          result = false;
        } else if (!tvIsArrayLike(typeV)) {
          result = false;
        } else {
          result = typeStructureIsType(inputV.m_data.parr, typeV.m_data.parr, warn, strict);
        }
        return !result;
      });
  }

  return result;
}

/*
 * Checks whether the `input` type structure is of the type denoted by the type
 * structure called `type`
 *
 * For example:
 * typeStructureIsType(int, int) -> true
 * typeStructureIsType((int, string), (int, _)) -> true
 * typeStructureIsType((int, string), (_, int)) -> false
 *
 * This is similar to instanceof check but also works with wildcards but it
 * currently does not implement subtyping checks (TODO(T31677864))
 *
 * Strict parameter dictates whether to return false on erased generics and
 * whether to treat unresolved as a class name.
 * If class C takes an erased generic T, then
 * if strict, then typeStructureIsType(C<_>, C<int>) return false,
 * if not strict, then typeStructureIsType(C<_>, C<int>) returns true.
 */
bool typeStructureIsType(
  const ArrayData* input,
  const ArrayData* type,
  bool& warn,
  bool strict
) {
  assertx(input && type);
  if (input == type) return true; // shortcut
  if (!strict && isWildCard(input)) return true;

  // Case types compare by witness, only allowing exact same case type
  // to be equal to it.
  auto const inputCaseType = get_ts_case_type_opt(input);
  auto const typeCaseType = get_ts_case_type_opt(type);
  if (inputCaseType || typeCaseType) {
    return caseTypeIsType(inputCaseType, input, typeCaseType, type, warn, strict);
  }

  auto tsKind = get_ts_kind(type);
  switch (tsKind) {
    case TypeStructure::Kind::T_int:
    case TypeStructure::Kind::T_bool:
    case TypeStructure::Kind::T_float:
    case TypeStructure::Kind::T_string:
    case TypeStructure::Kind::T_resource:
    case TypeStructure::Kind::T_num:
    case TypeStructure::Kind::T_arraykey:
    case TypeStructure::Kind::T_enum:
    case TypeStructure::Kind::T_null:
    case TypeStructure::Kind::T_void:
    case TypeStructure::Kind::T_nothing:
    case TypeStructure::Kind::T_noreturn:
    case TypeStructure::Kind::T_mixed:
    case TypeStructure::Kind::T_dynamic:
    case TypeStructure::Kind::T_nonnull:
    case TypeStructure::Kind::T_dict:
    case TypeStructure::Kind::T_vec:
    case TypeStructure::Kind::T_keyset:
    case TypeStructure::Kind::T_vec_or_dict:
    case TypeStructure::Kind::T_darray:
    case TypeStructure::Kind::T_varray:
    case TypeStructure::Kind::T_varray_or_darray:
    case TypeStructure::Kind::T_any_array:
    case TypeStructure::Kind::T_class_ptr:
    case TypeStructure::Kind::T_class_or_classname: {
      if (is_ts_nullable(type)) {
        auto const inputT = get_ts_kind(input);
        return inputT == tsKind || inputT == TypeStructure::Kind::T_null;
      }
      auto const soft = is_ts_soft(type);
      auto const aliasInType = type->exists(s_alias);
      auto const aliasInInput = input->exists(s_alias);
      if (type->size() + aliasInInput != input->size() + soft + aliasInType) {
        return false;
      }
      bool result = true;
      IterateKV(
        input,
        [&](TypedValue k, TypedValue v1) {
          assertx(tvIsString(k));
          if (k.m_data.pstr->same(s_alias.get())) return false;
          auto const v2 = type->get(k.m_data.pstr);
          if (v2.is_init() && tvEqual(v1, v2)) return false;
          result = false;
          return true; // short circuit
        }
      );
      return result;
    }
    case TypeStructure::Kind::T_class:
    case TypeStructure::Kind::T_interface:
    case TypeStructure::Kind::T_xhp: {
      auto const classname_field = input->get(s_classname.get());
      if (!classname_field.is_init()) return false;
      assertx(isStringType(classname_field.type()));
      auto const name = classname_field.val().pstr;
      if (!name->tsame(get_ts_classname(type))) return false;
      auto const inputGenerics = getGenericTypesOpt(input);
      auto const typeGenerics = getGenericTypesOpt(type);
      if (!inputGenerics) {
        return !strict || (!typeGenerics && type->same(input));
      }
      return typeGenerics && typeStructureIsTypeList(*inputGenerics,
                                                     *typeGenerics,
                                                     name,
                                                     warn,
                                                     strict);
    }
    case TypeStructure::Kind::T_tuple:
      if (get_ts_kind(input) != TypeStructure::Kind::T_tuple) return false;
      return typeStructureIsTypeList(
        get_ts_elem_types(input),
        get_ts_elem_types(type),
        nullptr,
        warn,
        strict
      );
    case TypeStructure::Kind::T_shape: {
      if (get_ts_kind(input) != TypeStructure::Kind::T_shape) return false;
      auto const inputFields = get_ts_fields(input);
      auto const typeFields = get_ts_fields(type);
      if (inputFields->size() != typeFields->size()) return false;
      if (does_ts_shape_allow_unknown_fields(type) ^
          does_ts_shape_allow_unknown_fields(input)) {
        // either both needs to allow or neither
        return false;
      }
      auto result = true;
      // If any of the generics will warn, we need to keep track of it and
      // only warn if none of the other ones raise an error
      bool willWarn = false;
      IterateKV(
        typeFields,
        [&](TypedValue k, TypedValue v) {
          assertx(tvIsDict(v));
          auto typeField = Array(getShapeFieldElement(v));
          auto const tv = inputFields->get(k);
          if (!tv.is_init()) {
            result = false;
            return true; // short circuit
          }
          if (isOptionalShapeField(v.val().parr) ^
              isOptionalShapeField(tv.val().parr)) {
            // either both needs to be optional or neither
            result = false;
            return true; // short circuit
          }
          auto inputField = Array(getShapeFieldElement(tv));
          // If this field is associated with the value, kill it
          // This is safe since we already checked this above
          inputField.remove(StrNR{s_optional_shape_field.get()});
          typeField.remove(StrNR{s_optional_shape_field.get()});
          if (!typeStructureIsType(inputField.get(), typeField.get(), warn, strict)) {
            if (warn || is_ts_soft(typeField.get())) {
              willWarn = true;
              warn = false;
              return false;
            }
            result = false;
            return true; // short circuit
          }
          return false;
        }
      );
      if (willWarn && result) {
        warn = true;
        result = false;
      }
      return result;
    }
    case TypeStructure::Kind::T_typevar:
      // Only true if the typevar is a wildcard
      return type->exists(s_name.get()) &&
        get_ts_name(type)->equal(s_wildcard.get());
    case TypeStructure::Kind::T_fun: {
      auto const inputR = get_ts_return_type(input);
      auto const typeR = get_ts_return_type(type);
      if (!typeStructureIsType(inputR, typeR, warn, strict)) {
        return false;
      }
      auto const inputP = get_ts_param_types(input);
      auto const typeP = get_ts_param_types(type);
      if (!typeStructureIsTypeList(inputP, typeP, nullptr, warn, strict)) {
        return false;
      }
      auto const inputV = get_ts_variadic_type_opt(input);
      auto const typeV = get_ts_variadic_type_opt(type);
      return inputV && typeV
        ? typeStructureIsType(inputV, typeV, warn, strict)
        : inputV == typeV;
    }
    case TypeStructure::Kind::T_typeaccess:
    case TypeStructure::Kind::T_trait:
    case TypeStructure::Kind::T_reifiedtype:
    case TypeStructure::Kind::T_unresolved:
      raise_error("Invalid generics for type structure");
    case TypeStructure::Kind::T_union:
    case TypeStructure::Kind::T_recursiveUnion:
      always_assert(false && "should be caught by above earlier check");
  }
  not_reached();
}

bool typeStructureIsTypeList(
  const ArrayData* inputL,
  const ArrayData* typeL,
  const StringData* clsname,
  bool& warn,
  bool strict
) {
  assertx(inputL && typeL);
  auto const size = typeL->size();
  if (inputL->size() != size) return false;
  const VMFixedVector<TypeParamInfo>* tpinfo = nullptr;
  if (!strict && clsname) {
    auto const cls = Class::load(clsname);
    if (cls->hasReifiedGenerics()) {
      tpinfo = &cls->getGenericsInfo().m_typeParamInfo;
      if (tpinfo->size() == 0) return true;
      if (tpinfo->size() != size) return false;
    }
  }
  // If any of the generics will warn, we need to keep track of it and only warn
  // if none of the other ones raise an error
  bool willWarn = false;
  for (size_t i = 0; i < size; ++i) {
    if (tpinfo && !(*tpinfo)[i].m_isReified) continue;
    auto const inputElem = inputL->get(i);
    auto const typeElem = typeL->get(i);
    assertx(tvIsDict(inputElem) && tvIsDict(typeElem));
    if (!typeStructureIsType(inputElem.val().parr, typeElem.val().parr,
                             warn, strict)) {
      if (warn || (tpinfo && ((*tpinfo)[i].m_isWarn ||
                             is_ts_soft(typeElem.val().parr)))) {
        willWarn = true;
        warn = false;
        continue;
      }
      return false;
    }
  }
  if (!willWarn) return true;
  warn = true;
  return false;
}

/*
 * Semantics of checkTypeStructureMatchesTVImpl varies depending on this enum
 *    Shallow (used for enforcement):
 *      enums checked only up to base type,
 *      tuple and shapes only checked up to vec/dict,
 *      varray and darray are not distinguished,
 *      reject attempts to test erased generics
 *    Deep (used for "is type" testing or "as type" assertion operations):
 *      enums matter,
 *      deep tuple and shape checking,
 *      varray and darray are treated as vec and dict,
 *      reject attempts to test erased generics
 *    DeepIgnoreErased (used for checked casts when enabled):
 *      enums matter,
 *      deep tuple and shape checking,
 *      varray and darray are treated as vec and dict,
 *      ignore attempts to test erased generics
 */
enum class CheckOpKind : uint8_t { Shallow = 0, Deep = 1, DeepIgnoreErased = 2 };

ALWAYS_INLINE
bool checkReifiedGenericsMatch(
  const Array& ts,
  TypedValue c1,
  const StringData* name,
  bool& warn,
  CheckOpKind opkind
) {
  if (!ts.exists(s_generic_types)) return true;
  // TODO(T31677864): Handle non KindOfObject types
  if (c1.m_type != KindOfObject) return true;
  auto const obj = c1.m_data.pobj;
  auto const cls = Class::load(name);
  assertx(cls);
  if (!cls->hasReifiedGenerics()) {
    if (opkind == CheckOpKind::DeepIgnoreErased || opkind == CheckOpKind::Shallow) return true;
    // Before returning false, lets check if all the generics are wildcards
    // If not all wildcard, since this is not a reified class, then it is false
    return isTSAllWildcards(ts.get());
  }
  auto const obj_generics = getClsReifiedGenericsProp(cls, obj);
  auto const generics = ts[s_generic_types].getArrayData();
  auto const size = obj_generics->size();
  if (size != generics->size()) return false;

  // If any of the generics will warn, we need to keep track of it and only warn
  // if none of the other ones raise an error
  bool willWarn = false;
  for (size_t i = 0; i < size; ++i) {
    auto const objrg = obj_generics->get(i);
    auto const rg = generics->get(i);
    assertx(tvIsDict(objrg) && tvIsDict(rg));
    auto const tsvalue = rg.val().parr;
    if (get_ts_kind(tsvalue) == TypeStructure::Kind::T_typevar &&
        tsvalue->exists(s_name.get()) &&
        get_ts_name(tsvalue)->equal(s_wildcard.get())) {
      continue;
    }
    if (!typeStructureIsType(objrg.val().parr, tsvalue, warn, opkind == CheckOpKind::Deep)) {
      auto const& tpinfo = cls->getGenericsInfo().m_typeParamInfo;
      assertx(tpinfo.size() == size);
      if (warn || tpinfo[i].m_isWarn || is_ts_soft(tsvalue)) {
        willWarn = true;
        warn = false;
        continue;
      }
      return false;
    }
  }
  if (!willWarn) return true;
  warn = true;
  return false;
}

} // namespace

bool isTSAllWildcards(const ArrayData* ts) {
  auto const generics = ts->get(s_generic_types.get());
  if (!generics.is_init()) return true;
  assertx(isArrayLikeType(generics.m_type));
  auto allWildcard = true;
  IterateV(
    generics.m_data.parr,
    [&](TypedValue v) {
      assertx(isArrayLikeType(v.m_type));
      allWildcard &= isWildCard(v.m_data.parr);
      // if there is at least one not wildcard, then we can short circuit
      return !allWildcard;
    }
  );
  return allWildcard;
}

// This function will always be called after the prologue parameter type
// verification, so we can make the assumption that if `param` is an object,
// outermost type in `type_` is correct.
// This function will only be called when either `param` is an object or `type_`
// is a primitive reified type parameter
bool verifyReifiedLocalType(
  tv_rval param,
  const ArrayData* type_,
  const Class* ctx,
  const Func* func,
  bool isTypeVar,
  bool& warn
) {
  if (tvIsNull(param) && is_ts_nullable(type_)) return true;
  Array type = ArrNR(type_);
  auto const isObj = tvIsObject(param);
  // If it is not coming from a reified type variable and it is an object,
  // we do not need to run the check if the object in question does not have
  // reified generics since the parameter type verification should have taken
  // care of it
  if (!isTypeVar && isObj) {
    auto const obj = val(param).pobj;
    auto const objcls = obj->getVMClass();
    // The parameter type verification in the prologue already checked that
    // the class matches the type annocation.
    if (!objcls->hasReifiedGenerics() && !objcls->hasReifiedParent()) {
      return true;
    }
  }
  // We only need to resolve the type structure if the param is an object and
  // outmost type is unresolved, since there is no way for the outmost type to
  // be resolved but not the inner types due to assumption above.
  assertx(get_ts_kind(type_) != TypeStructure::Kind::T_unresolved || isObj);
  if (isObj && get_ts_kind(type_) == TypeStructure::Kind::T_unresolved) {
    try {
      bool persistent = true;
      type = TypeStructure::resolve(type, ctx, func->cls(),
                                    req::vector<Array>(), persistent);
    } catch (Exception& ) {
      if (is_ts_soft(type_)) warn = true;
      return false;
    } catch (Object& ) {
      if (is_ts_soft(type_)) warn = true;
      return false;
    }
  }
  return checkTypeStructureMatchesTV(type, *param, warn);
}

/*
 * Shared implementation for checkTypeStructureMatchesTV().
 *
 * See description of CheckOpKind above for its meaning.
 */
template <bool gen_error>
bool checkTypeStructureMatchesTVImpl(
  const Array& ts,
  TypedValue c1,
  std::string& givenType,
  std::string& expectedType,
  std::string& errorKey,
  bool& warn,
  CheckOpKind kind
) {
  auto const errOnLen = [&givenType](auto cell, auto len) {
    if (!gen_error) return;
    givenType = fmt::format("{} of length {}",
      describe_actual_type(&cell), len);
  };

  auto const errOnKey = [&errorKey](TypedValue key) {
    if (!gen_error) return;
    auto const escapedKey = [key] {
      if (isStringType(type(key))) {
        return fmt::format("\"{}\"",
          folly::cEscape<std::string>(val(key).pstr->toCppString()));
      }
      assertx(isIntType(type(key)));
      return std::to_string(val(key).num);
    }();
    errorKey = fmt::format("[{}]{}", escapedKey, errorKey);
  };

  auto const type = c1.m_type;
  auto const data = c1.m_data;

  auto const tv = ts.lookup(s_nullable);
  if (isNullType(type) && tv.is_init() && tv.val().num) {
    return true;
  }

  auto const ts_kind = TypeStructure::kind(ts);
  auto const result = [&] {
    switch (ts_kind) {
    case TypeStructure::Kind::T_void:
    case TypeStructure::Kind::T_null:
      return isNullType(type);
    case TypeStructure::Kind::T_nonnull:
      return !isNullType(type);
    case TypeStructure::Kind::T_int:
      return isIntType(type);
    case TypeStructure::Kind::T_bool:
      return isBoolType(type);
    case TypeStructure::Kind::T_float:
      return isDoubleType(type);
    case TypeStructure::Kind::T_num:
      return isIntType(type) || isDoubleType(type);
    case TypeStructure::Kind::T_arraykey:
      if (isClassType(type) || isLazyClassType(type)) {
        if (Cfg::Eval::ClassIsStringNotices) {
          raise_notice("Class used in is_string");
        }
        return true;
      }
      return isIntType(type) || isStringType(type);

    case TypeStructure::Kind::T_string:
      if (isClassType(type) || isLazyClassType(type)) {
        if (Cfg::Eval::ClassIsStringNotices) {
          raise_notice("Class used in is_string");
        }
        return true;
      }
      return isStringType(type);

    case TypeStructure::Kind::T_class_ptr:
      // TODO(T199611023) When we enforce inner class types, this can be relaxed
      raise_warning("Using TypeStructure to check class type");
      return isClassType(type) || isLazyClassType(type);
    case TypeStructure::Kind::T_class_or_classname:
      // This is banned by the typechecker, same as the previous case.
      raise_warning("Using TypeStructure to check class_or_classname type");
      return isStringType(type) || isLazyClassType(type) || isClassType(type);

    case TypeStructure::Kind::T_resource:
      return isResourceType(type) &&
             !reinterpret_cast<const OptResource*>(&data.pres)->isInvalid();

    case TypeStructure::Kind::T_darray:
      return kind != CheckOpKind::Shallow ? is_dict(&c1) : is_vec(&c1) || is_dict(&c1);

    case TypeStructure::Kind::T_varray:
      return kind != CheckOpKind::Shallow ? is_vec(&c1) : is_vec(&c1) || is_dict(&c1);

    case TypeStructure::Kind::T_varray_or_darray:
      return is_vec(&c1) || is_dict(&c1);

    case TypeStructure::Kind::T_dict:
      return is_dict(&c1);

    case TypeStructure::Kind::T_vec:
      return is_vec(&c1);

    case TypeStructure::Kind::T_vec_or_dict:
      return is_vec(&c1) || is_dict(&c1);

    case TypeStructure::Kind::T_keyset:
      return is_keyset(&c1);

    case TypeStructure::Kind::T_any_array:
      return isArrayLikeType(type);

    case TypeStructure::Kind::T_enum: {
      assertx(ts.exists(s_classname));
      auto const cls = Class::load(ts[s_classname].asCStrRef().get());
      if (kind == CheckOpKind::Shallow) {
        // N.B. This is currently broken for enums declared with the underlying
        //      type classname<T>, where it will cause us to accept both int and
        //      string.
        auto const dt = cls ? cls->enumBaseTy().underlyingDataType()
                            : std::nullopt;
        if (dt) return equivDataTypes(*dt, type);
        return isIntType(type) || isStringType(type);
      }
      return cls && enumHasValue(cls, &c1);
    }

    case TypeStructure::Kind::T_trait:
      // For is/as, we will not get here since we'll throw an error on the
      // resolution pass.
      // For parameter/return type verification, we treat it as a class type.
    case TypeStructure::Kind::T_class:
    case TypeStructure::Kind::T_interface:
    case TypeStructure::Kind::T_xhp: {
      assertx(ts.exists(s_classname));
      auto const name = ts[s_classname].asCStrRef().get();
      return
        tvInstanceOfImpl(&c1, [&] { return Class::load(name); }) &&
        checkReifiedGenericsMatch(ts, c1, name, warn, kind);
    }

    case TypeStructure::Kind::T_tuple: {
      if (!isVecType(type)) return false;
      if (kind == CheckOpKind::Shallow) return true;

      auto const ad = data.parr;
      assertx(ts.exists(s_elem_types));
      auto const ts_arr = ts[s_elem_types].getArrayData();
      if (ad->size() != ts_arr->size()) {
        errOnLen(c1, ad->size());
        return false;
      }

      auto const matches_tuple = [&] {
        bool keys_match = true; // are keys contiguous and zero-indexed?
        bool vals_match = true; // do vals match the type structure types?
        int index = 0;

        IterateKV(ad,
          [&](TypedValue k, TypedValue v) {
            if (!isIntType(k.m_type) || k.m_data.num != index++) {
              keys_match = false;
              return true;
            }
            auto const tv = ts_arr->get(k.m_data.num);
            auto const& ts2 = asCArrRef(&tv);
            auto thisElemWarns = false;
            if (!checkTypeStructureMatchesTVImpl<gen_error>(
              ts2, v, givenType, expectedType, errorKey, thisElemWarns, kind
            )) {
              errOnKey(k);
              if (thisElemWarns) {
                warn = true;
                return false;
              }
              vals_match = false;
              return true;
            }
            return false;
          }
        );
        // If there is an error, ignore `warn`.
        if (!vals_match || !keys_match) warn = false;

        if (!keys_match || warn) return false;
        return vals_match;
      }();

      return matches_tuple;
    }

    case TypeStructure::Kind::T_shape: {
      if (!isDictType(type)) return false;
      if (kind == CheckOpKind::Shallow) return true;

      auto const ad = data.parr;
      assertx(ts.exists(s_fields));
      auto const ts_arr = ts[s_fields].getArrayData();

      auto const numRequiredFields = [&] {
        auto n = 0;
        IterateV(
          ts_arr,
          [&](TypedValue v) {
            assertx(isArrayLikeType(v.m_type));
            n += !isOptionalShapeField(v.m_data.parr);
          }
        );
        return n;
      }();

      if (ad->size() < numRequiredFields) {
        errOnLen(c1, ad->size());
        return false;
      }

      auto const allowsUnknownFields = shapeAllowsUnknownFields(ts);
      if (!allowsUnknownFields && ad->size() > ts_arr->size()) {
        errOnLen(c1, ad->size());
        return false;
      }

      auto fieldsDidMatch = true;
      auto numExpectedFields = 0;

      IterateKV(
        ts_arr,
        [&](TypedValue k, TypedValue v) {
          assertx(isArrayLikeType(v.m_type));
          auto const tsFieldData = v.m_data.parr;
          if (!ad->exists(k)) {
            if (isOptionalShapeField(tsFieldData)) {
              return false;
            }
            fieldsDidMatch = false;
            errOnKey(k);
            return true;
          }
          auto const tsField = getShapeFieldElement(v);
          auto const field = ad->at(k);
          bool thisFieldWarns = false;
          numExpectedFields++;
          if (!checkTypeStructureMatchesTVImpl<gen_error>(
            ArrNR(tsField), field, givenType,
            expectedType, errorKey, thisFieldWarns, kind
          )) {
            errOnKey(k);
            if (thisFieldWarns) {
              warn = true;
              return false;
            }
            fieldsDidMatch = false;
            return true;
          }
          return false;
        }
      );
      if (!fieldsDidMatch ||
          !(allowsUnknownFields || ad->size() == numExpectedFields)) {
        warn = false;
        return false;
      }
      return !warn;
    }

    case TypeStructure::Kind::T_union: {
      bool match = false;
      IterateV(
        get_ts_union_types(ts.get()),
        [&](TypedValue ty) {
          assertx(isArrayLikeType(ty.m_type));
          match |= checkTypeStructureMatchesTVImpl<false>(
            Array{ty.m_data.parr}, c1, givenType, expectedType, errorKey, warn,
            kind
          );
          return match;
        }
      );
      return match;
    }

    case TypeStructure::Kind::T_nothing:
    case TypeStructure::Kind::T_noreturn:
    case TypeStructure::Kind::T_unresolved:
    case TypeStructure::Kind::T_typeaccess:
      return false;

    case TypeStructure::Kind::T_mixed:
    case TypeStructure::Kind::T_dynamic:
      return true;

    case TypeStructure::Kind::T_fun:
    case TypeStructure::Kind::T_typevar:
    case TypeStructure::Kind::T_recursiveUnion:
      // For is/as, we will not get here since we'll throw an error on the
      // resolution pass.
      // For parameter/return type verification, we don't check these types, so
      // just return true.
      return true;

    case TypeStructure::Kind::T_reifiedtype:
      // This type should have been removed in the resolution phase.
      always_assert(false);
    }
    not_reached();
  }();
  if (!warn && is_ts_soft(ts.get())) warn = true;
  if (gen_error && !result) {
    if (givenType.empty()) givenType = describe_actual_type(&c1);
    if (expectedType.empty()) {
      expectedType =
        TypeStructure::toString(ts,
          TypeStructure::TSDisplayType::TSDisplayTypeUser).toCppString();
    }
  }
  return result;
}

bool checkTypeStructureMatchesTV(const Array& ts, TypedValue c1) {
  std::string givenType, expectedType, errorKey;
  bool warn = false;
  return checkTypeStructureMatchesTVImpl<false>(
    ts, c1, givenType, expectedType, errorKey, warn, CheckOpKind::Deep);
}

bool checkForVerifyTypeStructureMatchesTV(
  const Array& ts,
  TypedValue c1,
  std::string& givenType,
  std::string& expectedType,
  std::string& errorKey
) {
  bool warn = false;
  auto const ts_kind = TypeStructure::kind(ts);
  // For the VerifyType instruction we don't want to flag `nothing` (as people write UNSAFE_CAST<mixed,nothing>)
  // or class pointers and classnames
  if (ts_kind == TypeStructure::Kind::T_class_or_classname || ts_kind == TypeStructure::Kind::T_nothing) {
    return true;
  }
  if (c1.m_type == KindOfClass || c1.m_type == KindOfLazyClass) {
    return true;
  }

  return checkTypeStructureMatchesTVImpl<true>(
    ts, c1, givenType, expectedType, errorKey, warn, CheckOpKind::DeepIgnoreErased);
}

bool checkTypeStructureMatchesTV(
  const Array& ts,
  TypedValue c1,
  std::string& givenType,
  std::string& expectedType,
  std::string& errorKey
) {
  bool warn = false;
  return checkTypeStructureMatchesTVImpl<true>(
    ts, c1, givenType, expectedType, errorKey, warn, CheckOpKind::Deep);
}

bool checkTypeStructureMatchesTV(
  const Array& ts,
  TypedValue c1,
  bool& warn
) {
  std::string givenType, expectedType, errorKey;
  return checkTypeStructureMatchesTVImpl<false>(
    ts, c1, givenType, expectedType, errorKey, warn, CheckOpKind::Shallow);
}

ALWAYS_INLINE
bool errorOnIsAsExpressionInvalidTypesList(const ArrayData* tsFields,
                                           bool dryrun, bool allowWildcard) {
  bool willError = false;
  IterateV(
    tsFields,
    [&](TypedValue v) {
      assertx(isArrayLikeType(v.m_type));
      auto arr = v.m_data.parr;
      auto const value_field = arr->get(s_value.get());
      if (value_field.is_init()) {
        assertx(tvIsDict(value_field));
        arr = value_field.val().parr;
      }
      if (!errorOnIsAsExpressionInvalidTypes(ArrNR(arr), dryrun,
                                             allowWildcard)) {
        return false;
      }
      willError = true;
      return true; // short-circuit
    }
  );
  return willError;
}

bool errorOnIsAsExpressionInvalidTypes(const Array& ts, bool dryrun,
                                       bool allowWildcard) {
  auto const err = [&](const char* errMsg) {
    if (dryrun) return true;
    raise_error("\"is\" and \"as\" operators cannot be used with %s", errMsg);
  };
  auto ts_kind = TypeStructure::kind(ts);
  switch (ts_kind) {
    case TypeStructure::Kind::T_int:
    case TypeStructure::Kind::T_bool:
    case TypeStructure::Kind::T_float:
    case TypeStructure::Kind::T_string:
    case TypeStructure::Kind::T_resource:
    case TypeStructure::Kind::T_num:
    case TypeStructure::Kind::T_arraykey:
    case TypeStructure::Kind::T_dict:
    case TypeStructure::Kind::T_vec:
    case TypeStructure::Kind::T_keyset:
    case TypeStructure::Kind::T_vec_or_dict:
    case TypeStructure::Kind::T_darray:
    case TypeStructure::Kind::T_varray:
    case TypeStructure::Kind::T_varray_or_darray:
    case TypeStructure::Kind::T_any_array:
    case TypeStructure::Kind::T_null:
    case TypeStructure::Kind::T_void:
    case TypeStructure::Kind::T_nothing:
    case TypeStructure::Kind::T_noreturn:
    case TypeStructure::Kind::T_mixed:
    case TypeStructure::Kind::T_dynamic:
    case TypeStructure::Kind::T_unresolved:
    case TypeStructure::Kind::T_typeaccess:
    case TypeStructure::Kind::T_nonnull:
    case TypeStructure::Kind::T_xhp:
      return false;
    case TypeStructure::Kind::T_enum:
    case TypeStructure::Kind::T_class:
    case TypeStructure::Kind::T_interface: {
      if (ts.exists(s_with_refinements)) {
        return err("a type with refinements");
      }
      auto tv = ts.lookup(s_generic_types);
      return tv.is_init() ?
        errorOnIsAsExpressionInvalidTypesList(tv.val().parr, dryrun, true) :
        false;
    }
    case TypeStructure::Kind::T_union: {
      bool error = false;
      IterateV(
        get_ts_union_types(ts.get()),
        [&](TypedValue ty) {
          assertx(isArrayLikeType(ty.m_type));
          error |= errorOnIsAsExpressionInvalidTypes(
            Array{ty.m_data.parr}, dryrun, allowWildcard
          );
          return error;
        }
      );
      return error;
    }
    case TypeStructure::Kind::T_fun:
      return err("a function");
    case TypeStructure::Kind::T_typevar:
      if (allowWildcard && isWildCard(ts.get())) return false;
      return err("a generic type");
    case TypeStructure::Kind::T_trait:
      return err("a trait");
    case TypeStructure::Kind::T_reifiedtype:
      return err("incomplete type structures");
    case TypeStructure::Kind::T_tuple: {
      assertx(ts.exists(s_elem_types));
      auto const elemsArr = ts.lookup(s_elem_types).val().parr;
      return errorOnIsAsExpressionInvalidTypesList(elemsArr, dryrun,
                                                   allowWildcard);
    }
    case TypeStructure::Kind::T_shape: {
      assertx(ts.exists(s_fields));
      auto const tsFields = ts.lookup(s_fields).val().parr;
      return errorOnIsAsExpressionInvalidTypesList(tsFields, dryrun,
                                                   allowWildcard);
    }
    case TypeStructure::Kind::T_class_ptr:
      // classname<T> is not a valid type for is/as or reification so let's
      // preserve this restriction for class<T> initially
      return err("a class type");
    case TypeStructure::Kind::T_class_or_classname:
      // same as class<T>, will not support this in is/as
      return err("a class_or_classname type");
    case TypeStructure::Kind::T_recursiveUnion:
      return err("a recursive case type");
  }
  not_reached();
}

/**
 * Returns whether the type structure may not be able to be resolved statically,
 * i.e. if it contains `this` references.
 */
bool typeStructureCouldBeNonStatic(const ArrayData* ts) {
  switch (get_ts_kind(ts)) {
    case TypeStructure::Kind::T_tuple:
    case TypeStructure::Kind::T_fun:
    case TypeStructure::Kind::T_darray:
    case TypeStructure::Kind::T_varray:
    case TypeStructure::Kind::T_varray_or_darray:
    case TypeStructure::Kind::T_shape:
    case TypeStructure::Kind::T_class:
    case TypeStructure::Kind::T_interface:
    case TypeStructure::Kind::T_trait:
    case TypeStructure::Kind::T_dict:
    case TypeStructure::Kind::T_vec:
    case TypeStructure::Kind::T_keyset:
    case TypeStructure::Kind::T_vec_or_dict:
    case TypeStructure::Kind::T_any_array:
    case TypeStructure::Kind::T_typeaccess:
    case TypeStructure::Kind::T_reifiedtype:
    case TypeStructure::Kind::T_class_ptr:
    case TypeStructure::Kind::T_class_or_classname:
      return true;
    case TypeStructure::Kind::T_unresolved: {
      if (get_ts_classname(ts)->tsame(s_hh_this.get())) return true;
      bool genericsCouldBeNonStatic = false;
      auto const generics = ts->get(s_generic_types.get());
      if (generics.is_init()) {
        assertx(isArrayLikeType(generics.type()));
        IterateV(
          generics.val().parr,
          [&](TypedValue v) {
            assertx(isArrayLikeType(v.m_type));
            genericsCouldBeNonStatic |=
              typeStructureCouldBeNonStatic(v.m_data.parr);
            // If at least one generic could be non-static, short circuit
            return genericsCouldBeNonStatic;
          }
        );
      }
      return genericsCouldBeNonStatic;
    }
    case TypeStructure::Kind::T_union: {
      bool match = false;
      IterateV(
        get_ts_union_types(ts),
        [&](TypedValue ty) {
          assertx(isArrayLikeType(ty.m_type));
          match |= typeStructureCouldBeNonStatic(ty.m_data.parr);
          return match;
        }
      );
      return match;
    }
    case TypeStructure::Kind::T_recursiveUnion: {
      // We don't need to dig through the recursive union - if it contains
      // 'this' (which it probably won't since 'this' isn't allowed in a case
      // type) then it will be found on a different path.
      return false;
    }
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
    case TypeStructure::Kind::T_typevar:
    case TypeStructure::Kind::T_enum:
    case TypeStructure::Kind::T_nonnull:
    case TypeStructure::Kind::T_xhp:
      return false;
  }
  not_reached();
}

template <bool IsOrAsOp>
Array resolveAndVerifyTypeStructure(
  const Array& ts,
  const Class* declaringCls,
  const Class* calledCls,
  const req::vector<Array>& tsList,
  bool suppress
) {
  assertx(!ts.empty());
  assertx(ts.isDict());
  auto const handleResolutionException = [&](auto const& errMsg) {
    if (!suppress || !IsOrAsOp) raise_error(errMsg);
    if (Cfg::Eval::IsExprEnableUnresolvedWarning) raise_warning(errMsg);
    auto unresolved = Array::CreateDict();
    TypeStructure::setKind(unresolved, TypeStructure::Kind::T_unresolved);
    unresolved.set(s_classname, Variant(s_unresolved));
    return unresolved;
  };
  Array resolved;
  bool invalidShapeSplat = false;
  try {
    bool persistent = true;
    resolved = TypeStructure::resolve(
      ts,
      calledCls,
      declaringCls,
      tsList,
      persistent,
      invalidShapeSplat
    );
  } catch (Exception& e) {
    // Catch and throw again so we get a line number
    resolved = handleResolutionException(e.getMessage());
  } catch (Object& ) {
    std::string errMsg = "unable to resolve anonymous type structure";
    resolved = handleResolutionException(errMsg);
  }
  assertx(!resolved.empty());
  assertx(resolved.isDict());
  if (IsOrAsOp && invalidShapeSplat) {
    raise_error(
      "\"is\" and \"as\" operators cannot be used with an invalid shape splat"
    );
  }
  if (IsOrAsOp) errorOnIsAsExpressionInvalidTypes(resolved, false);
  return resolved;
}

template Array resolveAndVerifyTypeStructure<true>(
  const Array&, const Class*, const Class*, const req::vector<Array>&, bool);

template Array resolveAndVerifyTypeStructure<false>(
  const Array&, const Class*, const Class*, const req::vector<Array>&, bool);

void throwTypeStructureDoesNotMatchTVException(
  std::string& givenType,
  std::string& expectedType,
  std::string& errorKey,
  bool raiseError
) {
  assertx(!givenType.empty());
  assertx(!expectedType.empty());
  std::string error;
  if (errorKey.empty()) {
    error = fmt::format("Expected {}, got {}", expectedType, givenType);
  } else {
    error = fmt::format("Expected {} at {}, got {}",
      expectedType, errorKey, givenType);
  }
  if (raiseError) {
    std::string full_error = fmt::format("Typed local assignment: {}", error);
    raise_typehint_error(full_error);
  } else {
    SystemLib::throwTypeAssertionExceptionObject(error);
  }
}

bool doesTypeStructureContainTUnresolved(const ArrayData* ts) {
  bool result = false;
  IterateKV(
    ts,
    [&] (TypedValue k, TypedValue v) {
      if (tvIsInt(v) && tvIsString(k) && k.m_data.pstr->same(s_kind.get()) &&
          static_cast<TypeStructure::Kind>(v.m_data.num) ==
          TypeStructure::Kind::T_unresolved) {
        result = true;
        return true; // short circuit
      }
      if (!isArrayLikeType(type(v))) return false;
      assertx(tvIsVec(v) || tvIsDict(v));
      result |= doesTypeStructureContainTUnresolved(v.m_data.parr);
      return result; // short circuit if true
    }
  );
  return result;
}

} // namespace HPHP
