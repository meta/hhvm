(*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

type t = {
  auto_namespace_map: (string * string) list;
  disable_xhp_element_mangling: bool;
  interpret_soft_types_as_like_types: bool;
  enable_xhp_class_modifier: bool;
  everything_sdt: bool;
  php5_compat_mode: bool;
  hhvm_compat_mode: bool;
  keep_user_attributes: bool;
  include_assignment_values: bool;
  stack_size: int;
  deregister_php_stdlib: bool;
  package_info: Package_info.t;
  package_support_multifile_tests: bool;
  disallow_non_annotated_memoize: bool;
  treat_non_annotated_memoize_as_kbic: bool;
  ignore_string_methods: bool;
}
[@@deriving show]

let from_parser_options (popt : Parser_options.t) =
  let open Parser_options in
  {
    auto_namespace_map = popt.auto_namespace_map;
    disable_xhp_element_mangling = popt.disable_xhp_element_mangling;
    interpret_soft_types_as_like_types = popt.interpret_soft_types_as_like_types;
    enable_xhp_class_modifier = popt.enable_xhp_class_modifier;
    everything_sdt = popt.everything_sdt;
    php5_compat_mode = false;
    hhvm_compat_mode = false;
    keep_user_attributes = popt.keep_user_attributes;
    include_assignment_values = false;
    stack_size = popt.stack_size;
    deregister_php_stdlib = popt.deregister_php_stdlib;
    package_info = popt.package_info;
    package_support_multifile_tests = popt.package_support_multifile_tests;
    disallow_non_annotated_memoize = popt.disallow_non_annotated_memoize;
    treat_non_annotated_memoize_as_kbic =
      popt.treat_non_annotated_memoize_as_kbic;
    ignore_string_methods = popt.ignore_string_methods;
  }
