(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

(**
 * Configuration keys organized by which config file they belong to.
 *
 * - [Hhconfig]: keys for .hhconfig files
 * - [Hhconf]: keys for hh.conf (ServerLocalConfig)
 *
 * All tools (hh_server, hh_client, hh_single_type_check) accept both
 * .hhconfig and hh.conf keys via [--config KEY=VALUE].
 *
 * To add a new config option:
 * 1. Add a new key in the appropriate submodule
 *    using [key] (for .hhconfig) or [hhconf_key] (for hh.conf)
 * 2. Use the key constant (e.g. [Config_keys.Hhconfig.timeout]) in
 *    server_config.ml or serverLocalConfigLoad.ml instead of a raw string
 *)

(** Keys valid in .hhconfig files. *)
module Hhconfig : sig
  (* parser options *)

  val abstract_static_props : string

  val allowed_decl_fixme_codes : string

  val auto_namespace_map : string

  val ban_memoize_erased_generic_return : string

  val ban_this_refinement_in_generic_class : string

  val const_static_props : string

  val deregister_php_stdlib : string

  val disable_xhp_children_declarations : string

  val disable_xhp_element_mangling : string

  val disallow_non_annotated_memoize : string

  val enable_experimental_stx_features : string

  val enable_xhp_class_modifier : string

  val expression_tree_shape_no_unwrap : string

  val allow_single_statement_concurrent : string

  val everything_sdt : string

  val foreach_key_after_value : string

  val forget_static_prop_refinements_by_name : string

  val ignore_string_methods : string

  val interpret_soft_types_as_like_types : string

  val is_systemlib : string

  val keep_user_attributes : string

  val list_assign_right_to_left : string

  val missing_targs_as_fresh_generics : string

  val package_support_multifile_tests : string

  val private_member_variance_check : string

  val treat_non_annotated_memoize_as_kbic : string

  val type_structure_fields_may_be_absent : string

  val union_intersection_type_hints : string

  (* global options and typechecker options *)

  val allow_all_files_for_module_declarations : string

  val allow_class_string_cast : string

  val allowed_expression_tree_visitors : string

  val allowed_files_for_ignore_readonly : string

  val allowed_files_for_module_declarations : string

  val allowed_fixme_codes_strict : string

  val class_class_type : string

  val class_pointer_ban_class_array_key : string

  val class_sub_classname : string

  val code_agnostic_fixme : string

  val const_attribute : string

  val constraint_array_index_assign : string

  val constraint_method_call : string

  val disable_physical_equality : string

  val disallow_discarded_nullable_awaitables : string

  val check_duplicate_enum_values : string

  val require_enum_shape_key_be_resolvable : string

  val include_enum_member_values : string

  val include_const_shape_key_values : string

  val disallow_toplevel_requires : string

  val disallow_unresolved_type_variables : string

  val dump_tast_hashes : string

  val enable_experimental_tc_features : string

  val enable_no_auto_dynamic : string

  val enable_tc_migration_flags : string

  val explicit_consistent_constructors : string

  val extended_reasons : string

  val glean_reponame : string

  val hack_warnings : string

  val hh_distc_exponential_backoff_num_retries : string

  val hh_distc_should_disable_trace_store : string

  val implicit_inherit_sdt : string

  val language_feature_logging : string

  val like_casts : string

  val locl_cache_capacity : string

  val locl_cache_node_threshold : string

  val log_levels : string

  val meth_caller_only_public_visibility : string

  val needs_concrete_class_call_check : string

  val needs_concrete_class_function_pointer_check : string

  val package_allow_enforceable_enum_violations : string

  val package_allow_all_tconst_violations : string

  val package_allow_classconst_violations : string

  val package_exclude_patterns : string

  val permits_bypassing_visibility : string

  val tests_bypass_visibility_static_properties : string

  val pessimise_builtins : string

  val populate_dead_unsafe_cast_heap : string

  val profile_top_level_definitions : string

  val re_no_cache : string

  val recursive_case_types : string

  val repo_stdlib_path : string

  val require_extends_implements_ancestors : string

  val require_types_tco_require_types_class_consts : string

  val silence_errors_under_dynamic : string

  val reject_promoted_property_redeclaration : string

  val skip_check_under_dynamic : string

  val typed_open_shapes : string

  val named_variadic_type : string

  val variadic_named_parameters : string

  val strict_consistent_construct : string

  val symbol_write_exclude_out : string

  val symbol_write_hhi_path : string

  val symbol_write_ignore_paths : string

  val symbol_write_include_hhi : string

  val symbol_write_index_inherited_members : string

  val symbol_write_index_paths : string

  val symbol_write_index_paths_file : string

  val symbol_write_index_paths_file_output : string

  val symbol_write_ownership : string

  val symbol_write_referenced_out : string

  val symbol_write_reindexed_out : string

  val symbol_write_root_path : string

  val symbol_write_sym_hash_in : string

  val symbol_write_sym_hash_out : string

  val timeout : string

  val typecheck_if_name_matches_regexp : string

  val typecheck_sample_rate : string

  val typeconst_concrete_concrete_error : string

  val type_printer_fuel : string

  val warnings_default_all : string

  val warnings_in_sandcastle : string

  (* server options *)

  val current_saved_state_rollout_flag_index : string

  val deactivate_saved_state_rollout : string

  val disabled_warnings : string

  val extra_paths : string

  val formatter_override : string

  val gc_minor_heap_size : string

  val gc_space_overhead : string

  val ide_fall_back_to_full_index : string

  val ignored_paths : string

  val load_script_timeout : string

  val naming_table_compression_level : string

  val naming_table_compression_threads : string

  val packages_config_path : string

  val enable_implicit_packages : string

  val sharedmem_compression : string

  val sharedmem_dirs : string

  val sharedmem_global_size : string

  val sharedmem_hash_table_pow : string

  val sharedmem_heap_size : string

  val sharedmem_log_level : string

  val sharedmem_minimum_available : string

  val sharedmem_sample_rate : string

  val shm_cache_size : string

  val shm_use_sharded_hashtbl : string

  val untrusted_mode : string

  val version : string

  val warn_on_non_opt_build : string

  val warnings_generated_files : string
end

(** Keys valid in hh.conf (ServerLocalConfig). *)
module Hhconf : sig
  (* distc *)

  val enable_fanout_aware_distc : string

  val hh_distc_fanout_full_init_threshold : string

  val hh_distc_fanout_threshold : string

  val use_compressed_dep_graph : string

  val use_distc : string

  val use_distc_crawl_dircache : string

  val eden_prefetch_enabled : string

  val eden_prefetch_fanout_threshold : string

  val eden_prefetch_workers : string

  (* edenfs file watcher *)

  val edenfs_file_watcher_debug_logging : string

  val edenfs_file_watcher_enabled : string

  val edenfs_file_watcher_report_telemetry : string

  val edenfs_file_watcher_state_tracking : string

  val edenfs_file_watcher_sync_queries_obey_deferral : string

  val edenfs_file_watcher_throttle_time_ms : string

  val edenfs_file_watcher_timeout_secs : string

  val edenfs_file_watcher_tracked_states : string

  (* experiments / infrastructure *)

  val art_cli_lite_exit_status_enabled : string

  val experiments : string

  val experiments_config_enabled : string

  val experiments_config_path : string

  val experiments_config_source : string

  val experiments_config_ttl_seconds : string

  val experiments_config_update : string

  val log_categories : string

  val log_inference_constraints : string

  val log_init_proc_stack_also_on_absent_from : string

  val log_large_fanouts_threshold : string

  val min_log_level : string

  val use_justknobs : string

  (* IDE *)

  val go_to_implementation : string

  val ide_file_watcher_enabled : string

  val ide_file_watcher_max_changed_files : string

  val ide_load_naming_table_on_disk : string

  val ide_naming_table_update_threshold : string

  val ide_parser_cache : string

  val ide_symbolindex_search_provider : string

  val symbolindex_quiet : string

  (* misc *)

  val dump_tasts : string

  val eden_fetch_parallelism : string

  val package_config_strict_validation : string

  val rollout_group : string

  val status_single_use_cached_diagnostics : string

  val workload_quantile : string

  val zstd_decompress_by_file : string

  (* profiling *)

  val memtrace_dir : string

  val profile_decling : string

  val profile_desc : string

  val profile_log : string

  val profile_owner : string

  val profile_slow_threshold : string

  val profile_type_check_duration_threshold : string

  val profile_type_check_memory_threshold_mb : string

  val profile_type_check_twice : string

  (* provider backend *)

  val allow_unstable_features : string

  val cache_remote_decls : string

  val rust_provider_backend : string

  val set_global_provider_backend_in_workers : string

  val isolation_allow_decl_repackaging : string

  (* saved state *)

  val load_state_natively_dirty_files_timeout : string

  val load_state_natively_download_timeout : string

  val load_state_natively_v4 : string

  val saved_state_max_expected_age_days : string

  val saved_state_lookup_use_eden : string

  val saved_state_database_shard_name : string

  val saved_state_manifold_local_port : string

  val project_metadata_w_flags : string

  val require_saved_state : string

  val saved_state_cache_limit : string

  val saved_state_manifold_api_key : string

  val specify_manifold_api_key : string

  val ss_force : string

  val store_decls_in_saved_state : string

  val use_manifold_cython_client : string

  val use_mini_state : string

  val use_mini_state_when_indexing : string

  (* server operations *)

  val attempt_fix_credentials : string

  val consume_streaming_errors : string

  val use_agent_error_format : string

  val cpu_priority : string

  val enable_fuzzy_search : string

  val enable_global_access_check : string

  val enable_naming_table_fallback : string

  val enable_on_nfs : string

  val enable_type_check_filter_files : string

  val extend_defs_per_file_bucket_size : string

  val fetch_remote_old_decls : string

  val only_fetch_remote_old_decl_during_init : string

  val disable_rust_provider_shallow_decl_invalidation : string

  val enable_annotation_agnostic_decl_diffing : string

  val heartbeat_interval : string

  val prefetch_decls_enabled : string

  val prefetch_decls_threshold : string

  val block_client_connections_while_deferring : string

  val hg_aware : string

  val allow_restart_from_precomputed_saved_state : string

  val hg_aware_parsing_restart_threshold : string

  val hg_aware_redecl_restart_threshold : string

  val hg_aware_recheck_restart_threshold : string

  val idle_gc_slice : string

  val informant_min_distance_restart : string

  val interrupt_on_client : string

  val interrupt_on_file_changes : string

  val interrupt_on_watchman : string

  val io_priority : string

  val longlived_workers : string

  val max_purgatory_clients : string

  val max_workers : string

  val naming_sqlite_path : string

  val num_local_workers : string

  val populate_member_heaps : string

  val prechecked_files : string

  val predeclare_ide : string

  val produce_streaming_errors : string

  val search_chunk_size : string

  val shm_dirs : string

  val skip_hierarchy_checks : string

  val skip_tast_checks : string

  val tico_invalidate_files : string

  val tico_invalidate_smart : string

  val trace_parsing : string

  val type_decl_bucket_size : string

  val use_dummy_informant : string

  val use_full_fidelity_parser : string

  (* watchman *)

  val use_watchman : string

  val watchman_debug_logging : string

  val watchman_enabled : string

  val watchman_init_timeout : string

  val watchman_sockname : string

  val watchman_subscribe_v2 : string

  val watchman_synchronous_timeout : string
end

(* validation *)

(** When a configuration key looks like a typo, we make a suggestion *)
type did_you_mean = Did_you_mean of string option

(** Validate a key against hhconfig keys only *)
val validate_hhconfig_key : config_key:string -> (unit, did_you_mean) result

(** Validate a key against hhconf keys only *)
val validate_hhconf_key : config_key:string -> (unit, did_you_mean) result

(** Validate a --config CLI override key against all known keys
    (both .hhconfig and hh.conf). *)
val validate : config_key:string -> (unit, did_you_mean) result
