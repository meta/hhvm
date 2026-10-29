(*
 * Copyright (c) 2015, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude

type t = Global_options.t [@@deriving eq, show]

let num_local_workers t = t.Global_options.tco_num_local_workers

let language_feature_logging t = t.Global_options.tco_language_feature_logging

let legacy_experimental_feature_enabled t feature =
  S_set.mem feature t.Global_options.tco_legacy_experimental_features

let migration_flag_enabled t flag =
  S_set.mem flag t.Global_options.tco_migration_flags

let log_inference_constraints t = t.Global_options.tco_log_inference_constraints

let default = Global_options.default

(* TYPECHECKER-SPECIFIC OPTIONS *)

let experimental_supportdynamic_type_hint = "supportdynamic_type_hint"

let experimental_consider_type_const_enforceable =
  "experimental_consider_type_const_enforceable"

(** Resolve the this type inside enum classes using the proper context *)
let experimental_sound_enum_class_type_const = "sound_enum_class_this_type"

let experimental_try_constraint_method_inference =
  "experimental_try_constraint_method_inference"

let experimental_all =
  List.fold_right
    ~f:S_set.add
    ~init:S_set.empty
    [
      experimental_supportdynamic_type_hint;
      experimental_consider_type_const_enforceable;
      experimental_sound_enum_class_type_const;
      experimental_try_constraint_method_inference;
    ]

let migration_flags_all = List.fold_right ~init:S_set.empty ~f:S_set.add []

let timeout t = t.Global_options.tco_timeout

let constraint_array_index_assign t =
  t.Global_options.tco_constraint_array_index_assign

let constraint_method_call t = t.Global_options.tco_constraint_method_call

let log_levels t = t.Global_options.log_levels

let fetch_remote_old_decls t = t.Global_options.tco_fetch_remote_old_decls

let only_fetch_remote_old_decl_during_init t =
  t.Global_options.tco_only_fetch_remote_old_decl_during_init

let disable_rust_provider_shallow_decl_invalidation t =
  t.Global_options.tco_disable_rust_provider_shallow_decl_invalidation

let populate_member_heaps t = t.Global_options.tco_populate_member_heaps

let enable_legacy_experimental t feature =
  S_set.add feature t.Global_options.tco_legacy_experimental_features

let skip_hierarchy_checks t = t.Global_options.tco_skip_hierarchy_checks

let silence_errors_under_dynamic t =
  t.Global_options.tco_silence_errors_under_dynamic

let reject_promoted_property_redeclaration t =
  t.Global_options.tco_reject_promoted_property_redeclaration

let set_skip_hierarchy_checks t =
  Global_options.{ t with tco_skip_hierarchy_checks = true }

let skip_tast_checks t = t.Global_options.tco_skip_tast_checks

let strict_contexts t = t.Global_options.tco_strict_contexts

let enable_global_access_check t =
  Global_options.{ t with tco_global_access_check_enabled = true }

let global_access_check_enabled t =
  t.Global_options.tco_global_access_check_enabled

let like_casts t = t.Global_options.tco_like_casts

let disallow_unresolved_type_variables t =
  t.Global_options.tco_disallow_unresolved_type_variables

let custom_error_config t = t.Global_options.tco_custom_error_config

let const_static_props t = t.Global_options.po.Parser_options.const_static_props

let const_attribute t = t.Global_options.tco_const_attribute

let disallow_discarded_nullable_awaitables t =
  t.Global_options.tco_disallow_discarded_nullable_awaitables

let check_duplicate_enum_values t =
  t.Global_options.tco_check_duplicate_enum_values

let require_shape_key_attribute t =
  t.Global_options.tco_require_shape_key_attribute

let require_const_shape_key_be_resolvable t =
  t.Global_options.tco_require_const_shape_key_be_resolvable

let is_systemlib t = t.Global_options.po.Parser_options.is_systemlib

let enable_no_auto_dynamic t = t.Global_options.tco_enable_no_auto_dynamic

let skip_check_under_dynamic t = t.Global_options.tco_skip_check_under_dynamic

let set_skip_check_under_dynamic t =
  Global_options.{ t with tco_skip_check_under_dynamic = true }

let typed_open_shapes t = t.Global_options.tco_typed_open_shapes

let named_variadic_type t = t.Global_options.tco_named_variadic_type

let variadic_named_parameters t = t.Global_options.tco_variadic_named_parameters

let interpret_soft_types_as_like_types t =
  t.Global_options.po.Parser_options.interpret_soft_types_as_like_types

let set_tco_no_parser_readonly_check t b =
  let po =
    { t.Global_options.po with Parser_options.no_parser_readonly_check = b }
  in
  Global_options.{ t with po }

let tco_no_parser_readonly_check t =
  t.Global_options.po.Parser_options.no_parser_readonly_check

let set_tco_enable_expression_trees t b =
  Global_options.{ t with tco_enable_expression_trees = b }

let expression_trees_enabled t = t.Global_options.tco_enable_expression_trees

let allowed_expression_tree_visitors t =
  t.Global_options.tco_allowed_expression_tree_visitors

let typeconst_concrete_concrete_error t =
  t.Global_options.tco_typeconst_concrete_concrete_error

let meth_caller_only_public_visibility t =
  t.Global_options.tco_meth_caller_only_public_visibility

let require_extends_implements_ancestors t =
  t.Global_options.tco_require_extends_implements_ancestors

let repo_stdlib_path t = t.Global_options.tco_repo_stdlib_path

let everything_sdt t = t.Global_options.po.Parser_options.everything_sdt

let implicit_inherit_sdt t = t.Global_options.tco_implicit_inherit_sdt

let pessimise_builtins t =
  everything_sdt t || t.Global_options.tco_pessimise_builtins

let explicit_consistent_constructors t =
  t.Global_options.tco_explicit_consistent_constructors

let require_types_class_consts t =
  t.Global_options.tco_require_types_class_consts

let type_printer_fuel t = t.Global_options.tco_type_printer_fuel

let specify_manifold_api_key t = t.Global_options.tco_specify_manifold_api_key

let saved_state t = t.Global_options.tco_saved_state

let saved_state_loading t = Global_options.(t.tco_saved_state.loading)

let saved_state_rollouts t = Global_options.(t.tco_saved_state.rollouts)

let optimized_member_fanout t =
  Global_options.(t.tco_saved_state.rollouts)
    .Saved_state_rollouts.optimized_member_fanout

let enable_annotation_agnostic_decl_diffing t =
  t.Global_options.tco_enable_annotation_agnostic_decl_diffing

let profile_top_level_definitions t =
  t.Global_options.tco_profile_top_level_definitions

let typecheck_if_name_matches_regexp t =
  t.Global_options.tco_typecheck_if_name_matches_regexp

let allow_all_files_for_module_declarations t =
  t.Global_options.tco_allow_all_files_for_module_declarations

let allowed_files_for_module_declarations t =
  t.Global_options.tco_allowed_files_for_module_declarations

let allowed_files_for_ignore_readonly t =
  t.Global_options.tco_allowed_files_for_ignore_readonly

let record_fine_grained_dependencies t =
  t.Global_options.tco_record_fine_grained_dependencies

let loop_iteration_upper_bound t =
  t.Global_options.tco_loop_iteration_upper_bound

let typecheck_sample_rate t = t.Global_options.tco_typecheck_sample_rate

let log_fanout t ~fanout_cardinal =
  match t.Global_options.tco_log_large_fanouts_threshold with
  | None -> false
  | Some threshold -> Int.(fanout_cardinal >= threshold)

let populate_dead_unsafe_cast_heap t =
  t.Global_options.tco_populate_dead_unsafe_cast_heap

let tco_dynamic_inference t = t.Global_options.tco_dynamic_inference

let locl_cache_capacity t = t.Global_options.tco_locl_cache_capacity

let locl_cache_node_threshold t = t.Global_options.tco_locl_cache_node_threshold

let dump_tast_hashes t = t.Global_options.dump_tast_hashes

let dump_tasts t = t.Global_options.dump_tasts

let tco_autocomplete_mode t = t.Global_options.tco_autocomplete_mode

let set_tco_autocomplete_mode t =
  { t with Global_options.tco_autocomplete_mode = true }

let package_info t = t.Global_options.po.Parser_options.package_info

let tco_extended_reasons t = t.Global_options.tco_extended_reasons

let enable_eqeqeq_refinement_filter t =
  t.Global_options.tco_enable_eqeqeq_refinement_filter

let disable_physical_equality t = t.Global_options.tco_disable_physical_equality

let hack_warnings t = t.Global_options.hack_warnings

let warnings_generated_files t = t.Global_options.warnings_generated_files

let set_package_info t package_info =
  let popt = { t.Global_options.po with Parser_options.package_info } in
  { t with Global_options.po = popt }

let package_support_multifile_tests t =
  t.Global_options.po.Parser_options.package_support_multifile_tests

let package_allow_classconst_violations t =
  t.Global_options.tco_package_allow_classconst_violations

let package_allow_all_tconst_violations t =
  t.Global_options.tco_package_allow_all_tconst_violations

let package_allow_enforceable_enum_violations t =
  t.Global_options.tco_package_allow_enforceable_enum_violations

let package_exclude_patterns t = t.Global_options.tco_package_exclude_patterns

let class_sub_classname t = t.Global_options.class_sub_classname

let class_class_type t = t.Global_options.class_class_type

let needs_concrete_class_call_check t =
  t.Global_options.needs_concrete_class_call_check

let needs_concrete_class_function_pointer_check t =
  Option.value
    t.Global_options.needs_concrete_class_function_pointer_check
    ~default:(needs_concrete_class_call_check t)

let needs_concrete_require_extends_class_const_check t =
  t.Global_options.needs_concrete_require_extends_class_const_check

let strict_consistent_construct t = t.Global_options.strict_consistent_construct

let allow_class_string_cast t = t.Global_options.allow_class_string_cast

let class_pointer_ban_class_array_key t =
  t.Global_options.class_pointer_ban_class_array_key

let enable_recursive_case_types t =
  { t with Global_options.recursive_case_types = true }

let is_unstable_feature_enabled t name =
  S_set.mem name t.Global_options.tco_enabled_unstable_features
  ||
  match S_map.find_opt name t.Global_options.po.experimental_features with
  | Some OngoingRelease -> true
  | _ -> false

let permits_bypassing_visibility t =
  t.Global_options.tco_permits_bypassing_visibility

let tests_bypass_visibility_static_properties t =
  t.Global_options.tco_tests_bypass_visibility_static_properties

let implicit_throw_points t = t.Global_options.tco_implicit_throw_points
