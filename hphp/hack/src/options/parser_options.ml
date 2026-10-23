(*
 * Copyright (c) 2015, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)
type t = {
  (* These options are set in both hhvm and hh config *)
  const_static_props: bool;
  abstract_static_props: bool;
  enable_xhp_class_modifier: bool;
  disable_xhp_element_mangling: bool;
  allow_unstable_features: bool;
  (* These options are set in hack config, but use the defaults in (from parser_options_impl.rs) hhvm*)
  hhvm_compat_mode: bool;
  hhi_mode: bool;
  codegen: bool;
  disable_xhp_children_declarations: bool;
  interpret_soft_types_as_like_types: bool;
  is_systemlib: bool;
  auto_namespace_map: (string * string) list;
  everything_sdt: bool;
  include_enum_member_values: bool;
  include_const_shape_key_values: bool;
  keep_user_attributes: bool;
  stack_size: int;
  deregister_php_stdlib: bool;
  union_intersection_type_hints: bool;
  unwrap_concurrent: bool;
  no_parser_readonly_check: bool;
  allowed_decl_fixme_codes: I_set.t;
  use_legacy_experimental_feature_config: bool;
  experimental_features: Experimental_features.feature_status S_map.t;
  consider_unspecified_experimental_features_released: bool;
  package_info: Package_info.t;
  package_support_multifile_tests: bool;
  disallow_non_annotated_memoize: bool;
  treat_non_annotated_memoize_as_kbic: bool;
  ignore_string_methods: bool;
  enable_intrinsics_extension: bool;
  expression_tree_shape_no_unwrap: bool;
  disallow_late_bound_defaults: bool;
}
[@@deriving show, eq]

let default =
  {
    const_static_props = false;
    abstract_static_props = false;
    enable_xhp_class_modifier = false;
    disable_xhp_element_mangling = false;
    allow_unstable_features = false;
    hhvm_compat_mode = false;
    hhi_mode = false;
    codegen = false;
    disable_xhp_children_declarations = false;
    interpret_soft_types_as_like_types = false;
    is_systemlib = false;
    auto_namespace_map = [];
    everything_sdt = false;
    include_enum_member_values = false;
    include_const_shape_key_values = false;
    deregister_php_stdlib = false;
    stack_size = 32 * 1024 * 1024;
    keep_user_attributes = false;
    union_intersection_type_hints = false;
    unwrap_concurrent = false;
    no_parser_readonly_check = false;
    allowed_decl_fixme_codes = I_set.empty;
    use_legacy_experimental_feature_config = true;
    experimental_features = S_map.empty;
    consider_unspecified_experimental_features_released = true;
    package_info = Package_info.empty;
    package_support_multifile_tests = false;
    disallow_non_annotated_memoize = false;
    treat_non_annotated_memoize_as_kbic = false;
    ignore_string_methods = true;
    enable_intrinsics_extension = false;
    expression_tree_shape_no_unwrap = false;
    disallow_late_bound_defaults = false;
  }

(* Changes here need to be synchronized with rust_parser_errors_ffi.rs *)
type ffi_t =
  bool
  * bool
  * bool
  * bool
  * bool
  * bool
  * bool
  * bool
  * bool
  * bool
  * bool
  * bool
  * bool
  * Experimental_features.feature_status S_map.t
  * bool
  * bool
  * bool

let to_rust_ffi_t po =
  ( po.hhvm_compat_mode,
    po.hhi_mode,
    po.codegen,
    po.const_static_props,
    po.abstract_static_props,
    po.enable_xhp_class_modifier,
    po.disable_xhp_element_mangling,
    po.disable_xhp_children_declarations,
    po.allow_unstable_features,
    po.interpret_soft_types_as_like_types,
    po.is_systemlib,
    false (* unused slot *),
    po.use_legacy_experimental_feature_config,
    po.experimental_features,
    po.consider_unspecified_experimental_features_released,
    po.ignore_string_methods,
    po.enable_intrinsics_extension )
