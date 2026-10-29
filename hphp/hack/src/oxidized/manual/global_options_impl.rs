// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

use crate::custom_error_config::CustomErrorConfig;
use crate::r#gen::global_options::GlobalOptions;
use crate::r#gen::global_options::SavedState;
use crate::r#gen::global_options::SavedStateLoading;
use crate::r#gen::parser_options::ParserOptions;
use crate::r#gen::saved_state_rollouts::SavedStateRollouts;
use crate::global_options::NoneOrAllExcept;
use crate::i_set;
use crate::s_map;
use crate::s_set;

impl Default for SavedStateLoading {
    fn default() -> Self {
        Self {
            saved_state_manifold_api_key: None,
            manifold_local_port: None,
            use_eden: false,
            database_shard_name: None,
            saved_state_max_expected_age_days: 14,
            use_manifold_cython_client: false,
            zstd_decompress_by_file: true,
            use_compressed_dep_graph: true,
            saved_state_cache_limit: 5,
        }
    }
}

impl Default for SavedState {
    fn default() -> Self {
        Self {
            loading: SavedStateLoading::default(),
            rollouts: SavedStateRollouts::default(),
            project_metadata_w_flags: true,
        }
    }
}

impl Default for GlobalOptions {
    fn default() -> Self {
        Self {
            po: ParserOptions::default(),
            tco_saved_state: SavedState::default(),
            tco_legacy_experimental_features: s_set::SSet::new(),
            tco_migration_flags: s_set::SSet::new(),
            tco_num_local_workers: None,
            tco_locl_cache_capacity: 30,
            tco_locl_cache_node_threshold: 10_000,
            so_naming_sqlite_path: None,
            po_disallow_toplevel_requires: false,
            tco_log_large_fanouts_threshold: None,
            tco_log_inference_constraints: false,
            tco_language_feature_logging: false,
            tco_timeout: 0,
            tco_constraint_array_index_assign: false,
            tco_constraint_method_call: false,
            code_agnostic_fixme: false,
            allowed_fixme_codes_strict: i_set::ISet::new(),
            log_levels: s_map::SMap::new(),
            tco_fetch_remote_old_decls: true,
            tco_only_fetch_remote_old_decl_during_init: true,
            tco_disable_rust_provider_shallow_decl_invalidation: false,
            tco_enable_annotation_agnostic_decl_diffing: false,
            tco_populate_member_heaps: true,
            tco_skip_hierarchy_checks: false,
            tco_silence_errors_under_dynamic: false,
            tco_reject_promoted_property_redeclaration: false,
            tco_skip_tast_checks: false,
            tco_strict_contexts: true,
            tco_like_casts: false,

            tco_disallow_unresolved_type_variables: false,
            tco_custom_error_config: CustomErrorConfig::default(),
            tco_const_attribute: false,
            tco_disallow_discarded_nullable_awaitables: false,
            tco_check_duplicate_enum_values: false,
            tco_require_shape_key_attribute: false,
            tco_require_const_shape_key_be_resolvable: false,
            glean_reponame: String::from("www.hack.light"),
            symbol_write_index_inherited_members: true,
            symbol_write_ownership: false,
            symbol_write_root_path: String::from("www"),
            symbol_write_hhi_path: String::from("hhi"),
            symbol_write_ignore_paths: vec![],
            symbol_write_index_paths: vec![],
            symbol_write_index_paths_file: None,
            symbol_write_include_hhi: false,
            symbol_write_index_paths_file_output: None,
            symbol_write_sym_hash_in: None,
            symbol_write_exclude_out: None,
            symbol_write_referenced_out: None,
            symbol_write_reindexed_out: None,
            symbol_write_sym_hash_out: false,
            tco_typecheck_sample_rate: 1.0,
            tco_pessimise_builtins: false,
            tco_enable_no_auto_dynamic: false,
            tco_skip_check_under_dynamic: false,
            tco_typed_open_shapes: true,
            tco_named_variadic_type: false,
            tco_variadic_named_parameters: false,
            tco_global_access_check_enabled: false,
            tco_enable_expression_trees: false,
            tco_allowed_expression_tree_visitors: vec![],
            tco_typeconst_concrete_concrete_error: false,
            tco_meth_caller_only_public_visibility: true,
            tco_require_extends_implements_ancestors: false,
            tco_implicit_inherit_sdt: false,
            tco_explicit_consistent_constructors: 0,
            tco_require_types_class_consts: 0,
            tco_type_printer_fuel: 100,
            tco_specify_manifold_api_key: false,
            tco_profile_top_level_definitions: false,
            tco_typecheck_if_name_matches_regexp: None,
            tco_allow_all_files_for_module_declarations: true,
            tco_allowed_files_for_module_declarations: vec![],
            tco_record_fine_grained_dependencies: false,
            tco_loop_iteration_upper_bound: None,
            tco_populate_dead_unsafe_cast_heap: false,
            tco_dynamic_inference: false,
            dump_tast_hashes: false,
            dump_tasts: vec![],
            tco_autocomplete_mode: false,
            tco_extended_reasons: None,
            tco_disable_physical_equality: false,
            hack_warnings: NoneOrAllExcept::AllExcept(vec![]),
            warnings_default_all: false,
            warnings_in_sandcastle: true,
            warnings_generated_files: vec![],
            tco_allowed_files_for_ignore_readonly: vec![],
            tco_package_exclude_patterns: vec![
                String::from(".*/__tests__/.*"),
                String::from(".*/flib/intern/makehaste/.*"),
            ],
            tco_package_allow_classconst_violations: true,
            tco_package_allow_all_tconst_violations: true,
            tco_package_allow_enforceable_enum_violations: true,
            re_no_cache: false,
            hh_distc_should_disable_trace_store: false,
            hh_distc_exponential_backoff_num_retries: 10,
            recursive_case_types: false,
            tco_enabled_unstable_features: Default::default(),
            class_sub_classname: true,
            class_class_type: true,
            needs_concrete_abstract_type_const_check: false,
            needs_concrete_class_call_check: 0,
            needs_concrete_class_function_pointer_check: None,
            needs_concrete_abstract_class_const_check: false,
            needs_concrete_require_extends_class_const_check: false,
            strict_consistent_construct: false,
            allow_class_string_cast: true,
            class_pointer_ban_class_array_key: false,
            tco_permits_bypassing_visibility: Default::default(),
            tco_tests_bypass_visibility_static_properties: false,
            tco_implicit_throw_points: false,
            tco_repo_stdlib_path: None,
        }
    }
}
