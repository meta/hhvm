(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)
type t = {
  (* These options are set in both hhvm and hh config *)
  const_static_props: bool;  (** Enable const static properties *)
  abstract_static_props: bool;  (** Static properties can be abstract *)
  enable_xhp_class_modifier: bool;
      (** Enable the new style xhp class.
         Old style: class :name {}
         New style: xhp class name {} *)
  disable_xhp_element_mangling: bool;
      (** Flag to disable the old stype xhp element mangling. `<something/>` would otherwise be resolved as `xhp_something`
         The new style `xhp class something {}` does not do this style of mangling, thus we need a way to disable it on the
         'lookup side'. *)
  allow_unstable_features: bool;
      (** Allows enabling unstable features via the __EnableUnstableFeatures attribute *)
  (* These options are set in hh config, but use the defaults in (from parser_options_impl.rs) hhvm *)
  hhvm_compat_mode: bool;
  hhi_mode: bool;
  codegen: bool;  (** Are we emitting bytecode? *)
  disable_xhp_children_declarations: bool;
      (** Disable `children (foo|bar+|pcdata)` declarations as they can be implemented without special syntax *)
  interpret_soft_types_as_like_types: bool;  (** <<__Soft>> T -> ~T *)
  is_systemlib: bool;  (** Enable features used to typecheck systemlib *)
  auto_namespace_map: (string * string) list;  (** Namespace aliasing map *)
  everything_sdt: bool;
      (** All classes are implcitly marked <<__SupportDynamicType>> *)
  include_enum_member_values: bool;
      (** If set, the parser records enum member values in decls (used by the
          duplicate-enum-value check). *)
  include_const_shape_key_values: bool;
      (** If set, the parser records `__ShapeKey` class-constant metadata in
          decls. *)
  keep_user_attributes: bool;
      (** Parse all user attributes rather than only the ones needed for typing *)
  stack_size: int;
      (** Stack size to use for parallel workers inside the parser. *)
  deregister_php_stdlib: bool;
      (** Flag for disabling functions in HHI files with the __PHPStdLib attribute *)
  union_intersection_type_hints: bool;
      (** Enables union and intersection type hints *)
  unwrap_concurrent: bool;
      (** Replace concurrent blocks with their bodies in the AST *)
  no_parser_readonly_check: bool;  (** Disable parser-based readonly checking *)
  allowed_decl_fixme_codes: I_set.t;
      (** Set of error codes disallowed in decl positions *)
  use_legacy_experimental_feature_config: bool;
      (** Ignore the experimental_features and consider_unspecified_experimental_features_released config
          options and use a hard coded function instead *)
  experimental_features: Experimental_features.feature_status S_map.t;
      (** A mapping of names of experimental features to their status: Unstable/Preview/OngoingRelease *)
  consider_unspecified_experimental_features_released: bool;
      (** Any experimental features not specified in the experimental_features configuration field should
          default to OngoingRelease if this is true. Otherwise they will default to Unstable. This should be true for
          testing and tools that don't read .hhconfig (e.g., like hh_single_type_check and hh_parse). It should
          be false for hh_server. *)
  package_info: Package_info.t;
      (** Information used to determine which package a file belongs to during typechecking. *)
  package_support_multifile_tests: bool;
      (** Option to strip the multifile filename mangling used in Hack tests.  Used in Packages tests.
          Should be set to true only by the unit tests in the Hack test suite *)
  disallow_non_annotated_memoize: bool;
      (** When true, plain <<__Memoize>> will not be allowed. **)
  treat_non_annotated_memoize_as_kbic: bool;
      (** When true, plain <<__Memoize>> will be treated as <<__Memoize(#KeyedByIC)>>. **)
  ignore_string_methods: bool;
      (** Ignore the methods defined in the string class in string.hhi *)
  enable_intrinsics_extension: bool;
  expression_tree_shape_no_unwrap: bool;
      (** When true, the Expression Tree desugarer stops emitting `->__unwrap()` on the first
          argument of the shape ops (`shapeAt`/`shapeIdx`/`shapePut`). Used to roll out making the
          DSL's shape wrapper type a transparent type alias, after which the unwrap call no longer
          typechecks. *)
  disallow_late_bound_defaults: bool;
      (** Disallow late-bound class references in default values: `static` in parameter and XHP
          attribute defaults, and `self` or `parent` in a trait's parameter defaults. *)
}
[@@deriving show, eq]

val default : t

(** Options passed to the Rust parser-error checker. Oxidised into FfiT. *)
type ffi_t = {
  hhvm_compat_mode: bool;
  hhi_mode: bool;
  codegen: bool;
  const_static_props: bool;
  abstract_static_props: bool;
  enable_xhp_class_modifier: bool;
  disable_xhp_element_mangling: bool;
  disable_xhp_children_declarations: bool;
  allow_unstable_features: bool;
  interpret_soft_types_as_like_types: bool;
  is_systemlib: bool;
  use_legacy_experimental_feature_config: bool;
  experimental_features: Experimental_features.feature_status S_map.t;
  consider_unspecified_experimental_features_released: bool;
  ignore_string_methods: bool;
  enable_intrinsics_extension: bool;
}

val to_rust_ffi_t : t -> ffi_t
