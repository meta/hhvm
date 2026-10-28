<?hh
/**
 * Copyright (c) 2014, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 */

namespace HH {

  /*
   * The following enum values are defined in
   * hphp/runtime/ext/reflection/ext_reflection-classes.php
   */
  <<__AllowUncheckedEnumValues>>
  enum TypeStructureKind: int {
    OF_VOID = 0;
    OF_INT = 0;
    OF_BOOL = 0;
    OF_FLOAT = 0;
    OF_STRING = 0;
    OF_RESOURCE = 0;
    OF_NUM = 0;
    OF_ARRAYKEY = 0;
    OF_NORETURN = 0;
    OF_MIXED = 0;
    OF_TUPLE = 0;
    OF_FUNCTION = 0;
    OF_ARRAY = 0;
    OF_GENERIC = 0;
    OF_SHAPE = 0;
    OF_CLASS = 0;
    OF_INTERFACE = 0;
    OF_TRAIT = 0;
    OF_ENUM = 0;
    OF_DICT = 0;
    OF_VEC = 0;
    OF_KEYSET = 0;
    OF_VEC_OR_DICT = 0;
    OF_NONNULL = 0;
    OF_DARRAY = 0;
    OF_VARRAY = 0;
    OF_VARRAY_OR_DARRAY = 0;
    OF_NULL = 0;
    OF_NOTHING = 0;
    OF_DYNAMIC = 0;
    OF_UNION = 0;
    OF_RECURSIVE_UNION = 0;
    OF_CLASS_PTR = 0;
    OF_CLASS_OR_CLASSNAME = 0;
    OF_UNRESOLVED = 0;
    OF_XHP = 0;
  }

  // Note: Nullable fields in shapes of this type may not be present, and so
  // should be considered optional. Additionally, shapes of this type may contain
  // additional fields other than those specified here.
  newtype TypeStructure<T> as shape(
    ?'nullable' => bool,
    'kind' => TypeStructureKind,
    ?'name' => string,
    ?'classname' => classname<T>,
    ?'elem_types' => varray<mixed>,
    ?'optional_elem_types' => varray<mixed>,
    ?'variadic_type' => mixed,
    ?'return_type' => mixed,
    ?'param_types' => varray<mixed>,
    ?'generic_types' => varray<mixed>,
    ?'root_name' => string,
    ?'access_list' => varray<string>,
    ?'fields' => darray<arraykey, mixed>,
    ?'allows_unknown_fields' => bool,
    ?'is_cls_cns' => bool,
    ?'optional_shape_field' => bool,
    ?'value' => mixed,
    ?'typevars' => string,
    ?'alias' => string,
    ?'exact' => bool,
    ?'like' => bool,
    ?'with_refinements' => dict<string, shape('is_ctx' => bool, ?'equals' => mixed, ?'as' => vec<mixed>, ?'super' => vec<mixed>)>,
    ?'splat_elem_types' => vec<mixed>
  ) = shape(
    ?'nullable' => bool,
    'kind' => TypeStructureKind,
    // name for generics (type variables)
    ?'name' => string,
    // classname for classes, interfaces, enums, or traits
    ?'classname' => classname<T>,
    // for tuples
    ?'elem_types' => varray<mixed>,
    ?'optional_elem_types' => varray<mixed>,
    // For tuples and functions
    ?'variadic_type' => mixed,
    // for functions
    ?'return_type' => mixed,
    ?'param_types' => varray<mixed>,
    // for arrays, classes
    ?'generic_types' => varray<mixed>,
    ?'root_name' => string,
    ?'access_list' => varray<string>,
    // for simple shapes
    ?'fields' => darray<arraykey, mixed>,
    ?'allows_unknown_fields' => bool,
    ?'is_cls_cns' => bool,
    ?'optional_shape_field' => bool,
    ?'value' => mixed,
    // Comma-separated string
    ?'typevars' => string,
    // for type aliases
    ?'alias' => string,
    // if the type is exact (i.e., not a subtype)
    ?'exact' => bool,
    // if the type is a like-type
    ?'like' => bool,
    // if the type is a with-refinement e.g. class C with { type T = int; }
    ?'with_refinements' => dict<string, shape('is_ctx' => bool, ?'equals' => mixed, ?'as' => vec<mixed>, ?'super' => vec<mixed>)>,
    // for shape splats
    ?'splat_elem_types' => vec<mixed>
  );

  /*
   * returns the shape associated with the type constant.
   */
  function type_structure(
    mixed $cls_or_obj,
    string $cns_name,
  )[]: \HH\FIXME\MISSING_RETURN_TYPE;
  // becomes:
  // type_structure(C::class or new C, 'type_const_name')
  //   : TypeStructure

  /*
   * Replacement for type_structure(C::class, 'T')['classname']
   * for type constants that point to classes.
   */
  function type_structure_classname(
    mixed $cls_or_obj,
    string $cns_name,
  )[]: \HH\FIXME\MISSING_RETURN_TYPE;

  /*
   * Replacement for type_structure(C::class, 'T')['classname']
   * for type constants that point to classes. Returns a resolved
   * class pointer instead of the string name of the class.
   */
  function type_structure_class(
    mixed $cls_or_obj,
    string $cns_name,
  )[]: \HH\FIXME\MISSING_RETURN_TYPE;

  /*
   * Retrieves the TypeStructure for a type alias.
   */
  function type_structure_for_alias<T>(
    typename<T> $cls_or_obj,
  )[]: TypeStructure<T>;

} // namespace HH
