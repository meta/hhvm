(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

(* used for validation below *)
let registered_keys : S_set.t ref = ref S_set.empty

let registered_hhconf_keys : S_set.t ref = ref S_set.empty

let key (name : string) : string =
  registered_keys := S_set.add name !registered_keys;
  name

(** Register an hh.conf config key (ServerLocalConfig). *)
let hhconf_key (name : string) : string =
  registered_hhconf_keys := S_set.add name !registered_hhconf_keys;
  name

(** Keys valid in .hhconfig files. *)
module Hhconfig = struct
  (* parser options *)

  let abstract_static_props = key "abstract_static_props"

  let allowed_decl_fixme_codes = key "allowed_decl_fixme_codes"

  let auto_namespace_map = key "auto_namespace_map"

  let ban_memoize_erased_generic_return =
    key "ban_memoize_erased_generic_return"

  let ban_this_refinement_in_generic_class =
    key "ban_this_refinement_in_generic_class"

  let const_static_props = key "const_static_props"

  let deregister_php_stdlib = key "deregister_php_stdlib"

  let disable_xhp_children_declarations =
    key "disable_xhp_children_declarations"

  let disable_xhp_element_mangling = key "disable_xhp_element_mangling"

  let disallow_non_annotated_memoize = key "disallow_non_annotated_memoize"

  let enable_experimental_stx_features = key "enable_experimental_stx_features"

  let enable_xhp_class_modifier = key "enable_xhp_class_modifier"

  let expression_tree_shape_no_unwrap = key "expression_tree_shape_no_unwrap"

  let allow_single_statement_concurrent =
    key "allow_single_statement_concurrent"

  let everything_sdt = key "everything_sdt"

  let forget_static_prop_refinements_by_name =
    key "forget_static_prop_refinements_by_name"

  let ignore_string_methods = key "ignore_string_methods"

  let interpret_soft_types_as_like_types =
    key "interpret_soft_types_as_like_types"

  let is_systemlib = key "is_systemlib"

  let keep_user_attributes = key "keep_user_attributes"

  let missing_targs_as_fresh_generics = key "missing_targs_as_fresh_generics"

  let package_support_multifile_tests = key "package_support_multifile_tests"

  let private_member_variance_check = key "private_member_variance_check"

  let treat_non_annotated_memoize_as_kbic =
    key "treat_non_annotated_memoize_as_kbic"

  let union_intersection_type_hints = key "union_intersection_type_hints"

  (* global options and typechecker options *)

  let allow_all_files_for_module_declarations =
    key "allow_all_files_for_module_declarations"

  let allow_class_string_cast = key "allow_class_string_cast"

  let allowed_expression_tree_visitors = key "allowed_expression_tree_visitors"

  let allowed_files_for_ignore_readonly =
    key "allowed_files_for_ignore_readonly"

  let allowed_files_for_module_declarations =
    key "allowed_files_for_module_declarations"

  let allowed_fixme_codes_strict = key "allowed_fixme_codes_strict"

  let class_class_type = key "class_class_type"

  let class_pointer_ban_class_array_key =
    key "class_pointer_ban_class_array_key"

  let class_sub_classname = key "class_sub_classname"

  let code_agnostic_fixme = key "code_agnostic_fixme"

  let const_attribute = key "const_attribute"

  let constraint_array_index_assign = key "constraint_array_index_assign"

  let constraint_method_call = key "constraint_method_call"

  let enable_eqeqeq_refinement_filter = key "enable_eqeqeq_refinement_filter"

  let disable_physical_equality = key "disable_physical_equality"

  let disallow_discarded_nullable_awaitables =
    key "disallow_discarded_nullable_awaitables"

  let check_duplicate_enum_values = key "check_duplicate_enum_values"

  let require_enum_shape_key_be_resolvable =
    key "require_enum_shape_key_be_resolvable"

  let include_enum_member_values = key "include_enum_member_values"

  let include_const_shape_key_values = key "include_const_shape_key_values"

  let disallow_toplevel_requires = key "disallow_toplevel_requires"

  let disallow_unresolved_type_variables =
    key "disallow_unresolved_type_variables"

  let dump_tast_hashes = key "dump_tast_hashes"

  let enable_experimental_tc_features = key "enable_experimental_tc_features"

  let enable_no_auto_dynamic = key "enable_no_auto_dynamic"

  let enable_tc_migration_flags = key "enable_tc_migration_flags"

  let explicit_consistent_constructors = key "explicit_consistent_constructors"

  let extended_reasons = key "extended_reasons"

  let glean_reponame = key "glean_reponame"

  let hack_warnings = key "hack_warnings"

  let hh_distc_exponential_backoff_num_retries =
    key "hh_distc_exponential_backoff_num_retries"

  let hh_distc_should_disable_trace_store =
    key "hh_distc_should_disable_trace_store"

  let implicit_inherit_sdt = key "implicit_inherit_sdt"

  let language_feature_logging = key "language_feature_logging"

  let like_casts = key "like_casts"

  let locl_cache_capacity = key "locl_cache_capacity"

  let locl_cache_node_threshold = key "locl_cache_node_threshold"

  let log_levels = key "log_levels"

  let meth_caller_only_public_visibility =
    key "meth_caller_only_public_visibility"

  let needs_concrete_class_call_check = key "needs_concrete_class_call_check"

  let needs_concrete_class_function_pointer_check =
    key "needs_concrete_class_function_pointer_check"

  let package_allow_enforceable_enum_violations =
    key "package_allow_enforceable_enum_violations"

  let package_allow_all_tconst_violations =
    key "package_allow_all_tconst_violations"

  let package_allow_classconst_violations =
    key "package_allow_classconst_violations"

  let package_exclude_patterns = key "package_exclude_patterns"

  let permits_bypassing_visibility = key "permits_bypassing_visibility"

  let tests_bypass_visibility_static_properties =
    key "tests_bypass_visibility_static_properties"

  let pessimise_builtins = key "pessimise_builtins"

  let populate_dead_unsafe_cast_heap = key "populate_dead_unsafe_cast_heap"

  let profile_top_level_definitions = key "profile_top_level_definitions"

  let re_no_cache = key "re_no_cache"

  let recursive_case_types = key "recursive_case_types"

  let repo_stdlib_path = key "repo_stdlib_path"

  let require_extends_implements_ancestors =
    key "require_extends_implements_ancestors"

  let require_types_tco_require_types_class_consts =
    key "require_types_tco_require_types_class_consts"

  let silence_errors_under_dynamic = key "silence_errors_under_dynamic"

  let reject_promoted_property_redeclaration =
    key "reject_promoted_property_redeclaration"

  let skip_check_under_dynamic = key "skip_check_under_dynamic"

  let typed_open_shapes = key "typed_open_shapes"

  let named_variadic_type = key "named_variadic_type"

  let variadic_named_parameters = key "variadic_named_parameters"

  let strict_consistent_construct = key "strict_consistent_construct"

  let symbol_write_exclude_out = key "symbol_write_exclude_out"

  let symbol_write_hhi_path = key "symbol_write_hhi_path"

  let symbol_write_ignore_paths = key "symbol_write_ignore_paths"

  let symbol_write_include_hhi = key "symbol_write_include_hhi"

  let symbol_write_index_inherited_members =
    key "symbol_write_index_inherited_members"

  let symbol_write_index_paths = key "symbol_write_index_paths"

  let symbol_write_index_paths_file = key "symbol_write_index_paths_file"

  let symbol_write_index_paths_file_output =
    key "symbol_write_index_paths_file_output"

  let symbol_write_ownership = key "symbol_write_ownership"

  let symbol_write_referenced_out = key "symbol_write_referenced_out"

  let symbol_write_reindexed_out = key "symbol_write_reindexed_out"

  let symbol_write_root_path = key "symbol_write_root_path"

  let symbol_write_sym_hash_in = key "symbol_write_sym_hash_in"

  let symbol_write_sym_hash_out = key "symbol_write_sym_hash_out"

  let timeout = key "timeout"

  let typecheck_if_name_matches_regexp = key "typecheck_if_name_matches_regexp"

  let typecheck_sample_rate = key "typecheck_sample_rate"

  let typeconst_concrete_concrete_error =
    key "typeconst_concrete_concrete_error"

  let type_printer_fuel = key "type_printer_fuel"

  let warnings_default_all = key "warnings_default_all"

  let warnings_in_sandcastle = key "warnings_in_sandcastle"

  (* server options *)

  let current_saved_state_rollout_flag_index =
    key "current_saved_state_rollout_flag_index"

  let deactivate_saved_state_rollout = key "deactivate_saved_state_rollout"

  let disabled_warnings = key "disabled_warnings"

  let extra_paths = key "extra_paths"

  let formatter_override = key "formatter_override"

  let gc_minor_heap_size = key "gc_minor_heap_size"

  let gc_space_overhead = key "gc_space_overhead"

  let ide_fall_back_to_full_index = key "ide_fall_back_to_full_index"

  let ignored_paths = key "ignored_paths"

  let load_script_timeout = key "load_script_timeout"

  let naming_table_compression_level = key "naming_table_compression_level"

  let naming_table_compression_threads = key "naming_table_compression_threads"

  let packages_config_path = key "packages_config_path"

  let enable_implicit_packages = key "enable_implicit_packages"

  let sharedmem_compression = key "sharedmem_compression"

  let sharedmem_dirs = key "sharedmem_dirs"

  let sharedmem_global_size = key "sharedmem_global_size"

  let sharedmem_hash_table_pow = key "sharedmem_hash_table_pow"

  let sharedmem_heap_size = key "sharedmem_heap_size"

  let sharedmem_log_level = key "sharedmem_log_level"

  let sharedmem_minimum_available = key "sharedmem_minimum_available"

  let sharedmem_sample_rate = key "sharedmem_sample_rate"

  let shm_cache_size = key "shm_cache_size"

  let shm_use_sharded_hashtbl = key "shm_use_sharded_hashtbl"

  let untrusted_mode = key "untrusted_mode"

  let version = key "version"

  let warn_on_non_opt_build = key "warn_on_non_opt_build"

  let warnings_generated_files = key "warnings_generated_files"
end

(** Keys valid in hh.conf (ServerLocalConfig).
    These keys are accepted by hh_server and hh_client via --config overrides,
    but NOT by hh_single_type_check. *)
module Hhconf = struct
  (* distc *)

  let enable_fanout_aware_distc = hhconf_key "enable_fanout_aware_distc"

  let hh_distc_fanout_full_init_threshold =
    hhconf_key "hh_distc_fanout_full_init_threshold"

  let hh_distc_fanout_threshold = hhconf_key "hh_distc_fanout_threshold"

  let use_compressed_dep_graph = hhconf_key "use_compressed_dep_graph"

  let use_distc = hhconf_key "use_distc"

  let use_distc_crawl_dircache = hhconf_key "use_distc_crawl_dircache"

  let eden_prefetch_enabled = hhconf_key "eden_prefetch_enabled"

  let eden_prefetch_fanout_threshold =
    hhconf_key "eden_prefetch_fanout_threshold"

  let eden_prefetch_workers = hhconf_key "eden_prefetch_workers"

  (* edenfs file watcher *)

  let edenfs_file_watcher_debug_logging =
    hhconf_key "edenfs_file_watcher_debug_logging"

  let edenfs_file_watcher_enabled = hhconf_key "edenfs_file_watcher_enabled"

  let edenfs_file_watcher_report_telemetry =
    hhconf_key "edenfs_file_watcher_report_telemetry"

  let edenfs_file_watcher_state_tracking =
    hhconf_key "edenfs_file_watcher_state_tracking"

  let edenfs_file_watcher_sync_queries_obey_deferral =
    hhconf_key "edenfs_file_watcher_sync_queries_obey_deferral"

  let edenfs_file_watcher_throttle_time_ms =
    hhconf_key "edenfs_file_watcher_throttle_time_ms"

  let edenfs_file_watcher_timeout_secs =
    hhconf_key "edenfs_file_watcher_timeout_secs"

  let edenfs_file_watcher_tracked_states =
    hhconf_key "edenfs_file_watcher_tracked_states"

  (* experiments / infrastructure *)

  let art_cli_lite_exit_status_enabled =
    hhconf_key "art_cli_lite_exit_status_enabled"

  let experiments = hhconf_key "experiments"

  let experiments_config_enabled = hhconf_key "experiments_config_enabled"

  let experiments_config_path = hhconf_key "experiments_config_path"

  let experiments_config_source = hhconf_key "experiments_config_source"

  let experiments_config_ttl_seconds =
    hhconf_key "experiments_config_ttl_seconds"

  let experiments_config_update = hhconf_key "experiments_config_update"

  let log_categories = hhconf_key "log_categories"

  let log_inference_constraints = hhconf_key "log_inference_constraints"

  let log_init_proc_stack_also_on_absent_from =
    hhconf_key "log_init_proc_stack_also_on_absent_from"

  let log_large_fanouts_threshold = hhconf_key "log_large_fanouts_threshold"

  let min_log_level = hhconf_key "min_log_level"

  let use_justknobs = hhconf_key "use_justknobs"

  (* IDE *)

  let go_to_implementation = hhconf_key "go_to_implementation"

  let ide_file_watcher_enabled = hhconf_key "ide_file_watcher_enabled"

  let ide_file_watcher_max_changed_files =
    hhconf_key "ide_file_watcher_max_changed_files"

  let ide_load_naming_table_on_disk = hhconf_key "ide_load_naming_table_on_disk"

  let ide_naming_table_update_threshold =
    hhconf_key "ide_naming_table_update_threshold"

  let ide_parser_cache = hhconf_key "ide_parser_cache"

  let ide_symbolindex_search_provider =
    hhconf_key "ide_symbolindex_search_provider"

  let symbolindex_quiet = hhconf_key "symbolindex_quiet"

  (* misc *)

  let dump_tasts = hhconf_key "dump_tasts"

  let eden_fetch_parallelism = hhconf_key "eden_fetch_parallelism"

  let package_config_strict_validation =
    hhconf_key "package_config_strict_validation"

  let rollout_group = hhconf_key "rollout_group"

  let status_single_use_cached_diagnostics =
    hhconf_key "status_single_use_cached_diagnostics"

  let workload_quantile = hhconf_key "workload_quantile"

  let zstd_decompress_by_file = hhconf_key "zstd_decompress_by_file"

  (* profiling *)

  let memtrace_dir = hhconf_key "memtrace_dir"

  let profile_decling = hhconf_key "profile_decling"

  let profile_desc = hhconf_key "profile_desc"

  let profile_log = hhconf_key "profile_log"

  let profile_owner = hhconf_key "profile_owner"

  let profile_slow_threshold = hhconf_key "profile_slow_threshold"

  let profile_type_check_duration_threshold =
    hhconf_key "profile_type_check_duration_threshold"

  let profile_type_check_memory_threshold_mb =
    hhconf_key "profile_type_check_memory_threshold_mb"

  let profile_type_check_twice = hhconf_key "profile_type_check_twice"

  (* provider backend *)

  let allow_unstable_features = hhconf_key "allow_unstable_features"

  let cache_remote_decls = hhconf_key "cache_remote_decls"

  let rust_provider_backend = hhconf_key "rust_provider_backend"

  let isolation_allow_decl_repackaging =
    hhconf_key "isolation_allow_decl_repackaging"

  (* saved state *)

  let load_state_natively_dirty_files_timeout =
    hhconf_key "load_state_natively_dirty_files_timeout"

  let load_state_natively_download_timeout =
    hhconf_key "load_state_natively_download_timeout"

  let load_state_natively_v4 = hhconf_key "load_state_natively_v4"

  let saved_state_max_expected_age_days =
    hhconf_key "saved_state_max_expected_age_days"

  let saved_state_lookup_use_eden = hhconf_key "saved_state_lookup_use_eden"

  let saved_state_database_shard_name =
    hhconf_key "saved_state_database_shard_name"

  let saved_state_manifold_local_port =
    hhconf_key "saved_state_manifold_local_port"

  let project_metadata_w_flags = hhconf_key "project_metadata_w_flags"

  let require_saved_state = hhconf_key "require_saved_state"

  let saved_state_cache_limit = hhconf_key "saved_state_cache_limit"

  let saved_state_manifold_api_key = hhconf_key "saved_state_manifold_api_key"

  let specify_manifold_api_key = hhconf_key "specify_manifold_api_key"

  let ss_force = hhconf_key "ss_force"

  let store_decls_in_saved_state = hhconf_key "store_decls_in_saved_state"

  let use_manifold_cython_client = hhconf_key "use_manifold_cython_client"

  let use_mini_state = hhconf_key "use_mini_state"

  let use_mini_state_when_indexing = hhconf_key "use_mini_state_when_indexing"

  (* server operations *)

  let attempt_fix_credentials = hhconf_key "attempt_fix_credentials"

  let consume_streaming_errors = hhconf_key "consume_streaming_errors"

  let use_agent_error_format = hhconf_key "use_agent_error_format"

  let cpu_priority = hhconf_key "cpu_priority"

  let enable_fuzzy_search = hhconf_key "enable_fuzzy_search"

  let enable_global_access_check = hhconf_key "enable_global_access_check"

  let enable_naming_table_fallback = hhconf_key "enable_naming_table_fallback"

  let enable_on_nfs = hhconf_key "enable_on_nfs"

  let enable_type_check_filter_files =
    hhconf_key "enable_type_check_filter_files"

  let extend_defs_per_file_bucket_size =
    hhconf_key "extend_defs_per_file_bucket_size"

  let fetch_remote_old_decls = hhconf_key "fetch_remote_old_decls"

  let only_fetch_remote_old_decl_during_init =
    hhconf_key "only_fetch_remote_old_decl_during_init"

  let disable_rust_provider_shallow_decl_invalidation =
    hhconf_key "disable_rust_provider_shallow_decl_invalidation"

  let enable_annotation_agnostic_decl_diffing =
    hhconf_key "enable_annotation_agnostic_decl_diffing"

  let heartbeat_interval = hhconf_key "heartbeat_interval"

  let prefetch_decls_enabled = hhconf_key "prefetch_decls_enabled"

  let prefetch_decls_threshold = hhconf_key "prefetch_decls_threshold"

  let block_client_connections_while_deferring =
    hhconf_key "block_client_connections_while_deferring"

  let hg_aware = hhconf_key "hg_aware"

  let allow_restart_from_precomputed_saved_state =
    hhconf_key "allow_restart_from_precomputed_saved_state"

  let hg_aware_parsing_restart_threshold =
    hhconf_key "hg_aware_parsing_restart_threshold"

  let hg_aware_redecl_restart_threshold =
    hhconf_key "hg_aware_redecl_restart_threshold"

  let hg_aware_recheck_restart_threshold =
    hhconf_key "hg_aware_recheck_restart_threshold"

  let idle_gc_slice = hhconf_key "idle_gc_slice"

  let informant_min_distance_restart =
    hhconf_key "informant_min_distance_restart"

  let interrupt_on_client = hhconf_key "interrupt_on_client"

  let interrupt_on_file_changes = hhconf_key "interrupt_on_file_changes"

  let interrupt_on_watchman = hhconf_key "interrupt_on_watchman"

  let io_priority = hhconf_key "io_priority"

  let longlived_workers = hhconf_key "longlived_workers"

  let max_purgatory_clients = hhconf_key "max_purgatory_clients"

  let max_workers = hhconf_key "max_workers"

  let naming_sqlite_path = hhconf_key "naming_sqlite_path"

  let num_local_workers = hhconf_key "num_local_workers"

  let populate_member_heaps = hhconf_key "populate_member_heaps"

  let prechecked_files = hhconf_key "prechecked_files"

  let predeclare_ide = hhconf_key "predeclare_ide"

  let produce_streaming_errors = hhconf_key "produce_streaming_errors"

  let search_chunk_size = hhconf_key "search_chunk_size"

  let shm_dirs = hhconf_key "shm_dirs"

  let skip_hierarchy_checks = hhconf_key "skip_hierarchy_checks"

  let skip_tast_checks = hhconf_key "skip_tast_checks"

  let tico_invalidate_files = hhconf_key "tico_invalidate_files"

  let tico_invalidate_smart = hhconf_key "tico_invalidate_smart"

  let trace_parsing = hhconf_key "trace_parsing"

  let type_decl_bucket_size = hhconf_key "type_decl_bucket_size"

  let use_dummy_informant = hhconf_key "use_dummy_informant"

  let use_full_fidelity_parser = hhconf_key "use_full_fidelity_parser"

  (* watchman *)

  let use_watchman = hhconf_key "use_watchman"

  let watchman_debug_logging = hhconf_key "watchman_debug_logging"

  let watchman_enabled = hhconf_key "watchman_enabled"

  let watchman_init_timeout = hhconf_key "watchman_init_timeout"

  let watchman_sockname = hhconf_key "watchman_sockname"

  let watchman_subscribe_v2 = hhconf_key "watchman_subscribe_v2"

  let watchman_synchronous_timeout = hhconf_key "watchman_synchronous_timeout"
end

(* validation *)

type did_you_mean = Did_you_mean of string option

let all_hhconfig_keys = !registered_keys

let all_hhconf_keys = !registered_hhconf_keys

let all_keys = S_set.union all_hhconfig_keys all_hhconf_keys

let all_hhconfig_keys_list = lazy (S_set.elements all_hhconfig_keys)

let all_hhconf_keys_list = lazy (S_set.elements all_hhconf_keys)

let all_keys_list = lazy (S_set.elements all_keys)

let validate_hhconfig_key ~(config_key : string) : (unit, did_you_mean) result =
  if S_set.mem config_key all_hhconfig_keys then
    Ok ()
  else
    let suggestion =
      String_utils.most_similar
        ~max_edit_distance:3
        config_key
        (Lazy.force all_hhconfig_keys_list)
        Fun.id
    in
    Error (Did_you_mean suggestion)

let validate_hhconf_key ~(config_key : string) : (unit, did_you_mean) result =
  if S_set.mem config_key all_hhconf_keys then
    Ok ()
  else
    let suggestion =
      String_utils.most_similar
        ~max_edit_distance:3
        config_key
        (Lazy.force all_hhconf_keys_list)
        Fun.id
    in
    Error (Did_you_mean suggestion)

let validate ~(config_key : string) : (unit, did_you_mean) result =
  if S_set.mem config_key all_keys then
    Ok ()
  else
    let suggestion =
      String_utils.most_similar
        ~max_edit_distance:3
        config_key
        (Lazy.force all_keys_list)
        Fun.id
    in
    Error (Did_you_mean suggestion)
