(*
 * Copyright (c) 2015, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Config_file.Getters
open Hh_prelude
open Option.Monad_infix
open Server_local_config

let output_config_section title output_config =
  Printf.eprintf "** %s:\n%!" title;
  output_config ();
  Printf.eprintf "\n%!";
  ()

let output_build_and_test_mode () =
  output_config_section "Build and test mode" @@ fun () ->
  Printf.eprintf
    "is_dev_build = %b\nHH_TEST_MODE = %s\nprod_telemetry = %b\n"
    Build_id.is_dev_build
    (match Sys.getenv_opt "HH_TEST_MODE" with
    | None -> "<unset>"
    | Some value -> Printf.sprintf "%S" value)
    (Sys_utils.prod_telemetry ())

let default =
  {
    saved_state = Global_options.default_saved_state;
    min_log_level = Hh_logger.Level.Info;
    attempt_fix_credentials = false;
    log_categories = [];
    log_large_fanouts_threshold = None;
    log_init_proc_stack_also_on_absent_from = false;
    art_cli_lite_exit_status_enabled = false;
    log_inference_constraints = false;
    experiments = [];
    experiments_config_meta = "";
    use_saved_state = false;
    use_saved_state_when_indexing = false;
    require_saved_state = true;
    load_state_natively = false;
    load_state_natively_download_timeout = 60;
    load_state_natively_dirty_files_timeout = 200;
    type_decl_bucket_size = 1000;
    extend_defs_per_file_bucket_size = 2000;
    enable_on_nfs = false;
    enable_fuzzy_search = true;
    max_purgatory_clients = 400;
    search_chunk_size = 0;
    io_priority = 7;
    cpu_priority = 10;
    shm_dirs = [Global_config.shm_dir; Global_config.tmp_dir];
    shm_use_sharded_hashtbl = false;
    shm_cache_size = -1;
    max_workers = None;
    use_dummy_informant = true;
    edenfs_informant_enabled = false;
    informant_min_distance_restart = 100;
    use_full_fidelity_parser = true;
    interrupt_on_file_changes = false;
    interrupt_on_client = false;
    trace_parsing = false;
    prechecked_files = true;
    enable_global_access_check = false;
    enable_type_check_filter_files = false;
    predeclare_ide = false;
    longlived_workers = false;
    block_client_connections_while_deferring = true;
    hg_aware = false;
    allow_restart_from_precomputed_saved_state = false;
    hg_aware_parsing_restart_threshold = 0;
    hg_aware_redecl_restart_threshold = 0;
    hg_aware_recheck_restart_threshold = 0;
    ide_parser_cache = false;
    store_decls_in_saved_state = false;
    idle_gc_slice = 0;
    populate_member_heaps = true;
    fetch_remote_old_decls = true;
    only_fetch_remote_old_decl_during_init = true;
    disable_rust_provider_shallow_decl_invalidation = false;
    enable_annotation_agnostic_decl_diffing = false;
    skip_hierarchy_checks = false;
    skip_tast_checks = false;
    silence_errors_under_dynamic = false;
    num_local_workers = None;
    heartbeat_interval = None;
    prefetch_decls_enabled = false;
    prefetch_decls_threshold = 0;
    produce_streaming_errors = true;
    consume_streaming_errors = false;
    use_agent_error_format = Agent_error_format_unconfigured;
    rust_provider_backend = true;
    set_global_provider_backend_in_workers = true;
    isolation_allow_decl_repackaging = false;
    naming_sqlite_path = None;
    enable_naming_table_fallback = false;
    ide_symbolindex_search_provider = "LocalIndex";
    symbolindex_quiet = false;
    tico_invalidate_files = false;
    tico_invalidate_smart = false;
    per_file_profiling = Hack_event_logger.PerFileProfilingConfig.default;
    memtrace_dir = None;
    go_to_implementation = true;
    allow_unstable_features = false;
    watchman = Watchman.default;
    edenfs_file_watcher = EdenfsFileWatcher.default;
    eden_prefetch_enabled = false;
    eden_prefetch_fanout_threshold = 1000;
    eden_prefetch_workers = 4;
    workload_quantile = None;
    rollout_group = None;
    status_single_use_cached_diagnostics = false;
    specify_manifold_api_key = false;
    cache_remote_decls = false;
    use_compressed_dep_graph = true;
    use_distc = true;
    enable_fanout_aware_distc = false;
    hh_distc_fanout_threshold = 250_000;
    hh_distc_fanout_full_init_threshold = 250_000;
    hh_distc_exponential_backoff_num_retries = 10;
    ide_file_watcher_enabled = false;
    ide_file_watcher_max_changed_files = 1000;
    ide_load_naming_table_on_disk = true;
    ide_naming_table_update_threshold = 1000;
    dump_tast_hashes = false;
    dump_tasts = [];
    hack_warnings = true;
    warnings_default_all = false;
    warnings_in_sandcastle = true;
    package_config_strict_validation = false;
    (* Fields primarily for hh_conf equivalence testing *)
    sharedmem_global_size = 0;
    sharedmem_hash_table_pow = 0;
    sharedmem_heap_size = 0;
    eden_fetch_parallelism = 0;
    use_distc_crawl_dircache = false;
    gc_minor_heap_size = 0;
    gc_space_overhead = 0;
    ide_fall_back_to_full_index = false;
    naming_table_compression_level = 0;
    naming_table_compression_threads = 0;
    config_version = None;
    ignored_paths = [];
  }

let system_config_path =
  let dir =
    try Sys.getenv "HH_LOCALCONF_PATH" with
    | _ -> Build_options.system_config_path
  in
  Filename.concat dir "hh.conf"

let warn_on_invalid_hhconf_keys (config : Config_file_common.t) : unit =
  let config_keys = Config_file_common.keys config in
  List.iter config_keys ~f:(fun key ->
      match Config_keys.validate_hhconf_key ~config_key:key with
      | Ok () -> ()
      | Error (Config_keys.Did_you_mean suggestion) ->
        let msg =
          match suggestion with
          | None -> Printf.sprintf "Unrecognized hh.conf config option: %s" key
          | Some s ->
            Printf.sprintf
              "Unrecognized hh.conf config option: %s. Did you mean %s?"
              key
              s
        in
        Hh_logger.warn "%s" msg)

let apply_overrides_in_order
    ~silent
    ~config
    ~overrides
    ~apply_justknobs_overrides
    ~apply_dynamic_overrides
    ~apply_sandbox_experiment_overrides
    ~apply_experiments_config_overrides =
  (* We'll apply CLI overrides now at the start so that the dynamic config
     sources can be informed about them, e.g. "--config rollout_group=foo" will
     guide the manner in which JustKnobs picks up values, and
     "--config use_justknobs=false" will disable it. Don't worry though -- we'll
     apply CLI overrides again at the end, so they overwrite any changes brought
     by dynamic config. *)
  let config =
    Config_file.apply_overrides ~config ~overrides ~log_reason:None
  in
  let config = apply_justknobs_overrides config in
  let config = apply_dynamic_overrides config in
  let config = apply_sandbox_experiment_overrides config in
  let (experiments_meta, config) = apply_experiments_config_overrides config in
  (* Finally, reapply the CLI overrides, since they should take
     precedence over dynamic config. *)
  let config =
    Config_file.apply_overrides
      ~config
      ~overrides
      ~log_reason:(Option.some_if (not silent) "--config")
  in
  (experiments_meta, config)

module For_test = struct
  let apply_overrides_in_order = apply_overrides_in_order
end

(** Apply the following overrides in order:
  * JustKnobs
  * caller-provided dynamic overrides
  * SandboxExperiment
  * Legacy experiments config
  * `overrides`
  *)
let apply_overrides
    ~silent
    ~current_version
    ~config
    ~from
    ~overrides
    ~apply_dynamic_overrides:apply_dynamic_overrides_fn =
  let deterministic_behavior_for_tests =
    Sys_utils.deterministic_behavior_for_tests ()
  in
  let apply_justknobs_overrides config =
    let use_justknobs = bool_opt Config_keys.Hhconf.use_justknobs config in
    match (use_justknobs, deterministic_behavior_for_tests) with
    | (Some false, _)
    (* --config use_justknobs=false (or in hh.conf) will force JK off, regardless of anything else *)
    | (None, true)
    (* if use_justknobs isn't set, HH_TEST_MODE=1 (used in tests) will still turn JK off *)
      ->
      config
    | (Some true, _)
    (* --config use_justknobs=true (or in hh.conf) will force JK on, regardless of anything else *)
    | (None, false)
    (* if use_justknobs isn't set, then HH_TEST_MODE unset or =0 will leave JK on *)
      ->
      Server_local_config_knobs.apply_justknobs_overrides ~silent config ~from
  in
  let apply_dynamic_overrides config =
    if deterministic_behavior_for_tests then
      config
    else
      apply_dynamic_overrides_fn ~silent config
  in
  let apply_sandbox_experiment_overrides config =
    if deterministic_behavior_for_tests then
      config
    else
      Server_local_config_sandbox_experiment.apply_sandbox_experiment_overrides
        ~silent
        config
  in
  let apply_experiments_config_overrides config =
    let experiments_enabled =
      bool_if_min_version
        Config_keys.Hhconf.experiments_config_enabled
        ~default:false
        ~current_version
        config
    in
    if experiments_enabled then begin
      Disk.mkdir_p Global_config.tmp_dir;
      let dir =
        string_
          Config_keys.Hhconf.experiments_config_path
          ~default:Global_config.tmp_dir
          config
      in
      let owner = Sys_utils.get_primary_owner () in
      let file =
        Filename.concat dir (Printf.sprintf "hh.%s.experiments" owner)
      in
      let update =
        bool_if_min_version
          Config_keys.Hhconf.experiments_config_update
          ~default:false
          ~current_version
          config
      in
      let ttl =
        float_of_int
          (int_
             Config_keys.Hhconf.experiments_config_ttl_seconds
             ~default:86400
             config)
      in
      let source =
        string_opt Config_keys.Hhconf.experiments_config_source config
      in
      let meta =
        if update then
          match Experiments_config_file.update ~silent ~file ~source ~ttl with
          | Ok meta -> meta
          | Error message -> message
        else
          "Updating experimental config not enabled"
      in
      if Disk.file_exists file then
        (* Apply the experiments overrides *)
        let experiment_overrides = Config_file.parse_local_config file in
        let config =
          Config_file.apply_overrides
            ~config
            ~overrides:experiment_overrides
            ~log_reason:(Option.some_if (not silent) "Experiment_overrides")
        in
        (meta, config)
      else
        ("Experimental config not found on disk", config)
    end else
      ("Experimental config not enabled", config)
  in
  apply_overrides_in_order
    ~silent
    ~config
    ~overrides
    ~apply_justknobs_overrides
    ~apply_dynamic_overrides
    ~apply_sandbox_experiment_overrides
    ~apply_experiments_config_overrides

let load_
    ?(config : Config_file_common.t option)
    system_config_path
    ~apply_dynamic_overrides
    ~silent
    ~current_version
    ~current_rolled_out_flag_idx
    ~deactivate_saved_state_rollout
    ~from
    ~overrides : t =
  if not silent then output_build_and_test_mode ();
  let (experiments_config_meta, config) =
    match config with
    | Some c ->
      (* Config provided directly - skip file read and overrides *)
      ("", c)
    | None ->
      let parsed = Config_file.parse_local_config system_config_path in
      warn_on_invalid_hhconf_keys parsed;
      apply_overrides
        ~silent
        ~current_version
        ~config:parsed
        ~from
        ~overrides
        ~apply_dynamic_overrides
  in
  (if not silent then
    output_config_section "Combined config" @@ fun () ->
    Config_file.print_to_stderr config);

  let experiments =
    string_list
      Config_keys.Hhconf.experiments
      ~default:default.experiments
      config
  in

  let log_categories =
    string_list
      Config_keys.Hhconf.log_categories
      ~default:default.log_categories
      config
  in
  let log_large_fanouts_threshold =
    int_opt Config_keys.Hhconf.log_large_fanouts_threshold config
  in
  let log_init_proc_stack_also_on_absent_from =
    bool_
      Config_keys.Hhconf.log_init_proc_stack_also_on_absent_from
      ~default:default.log_init_proc_stack_also_on_absent_from
      config
  in
  let art_cli_lite_exit_status_enabled =
    bool_
      Config_keys.Hhconf.art_cli_lite_exit_status_enabled
      ~default:default.art_cli_lite_exit_status_enabled
      config
  in
  let log_inference_constraints =
    bool_
      Config_keys.Hhconf.log_inference_constraints
      ~default:default.log_inference_constraints
      config
  in
  let min_log_level =
    match
      Hh_logger.Level.of_enum_string
        (String.lowercase
           (string_
              Config_keys.Hhconf.min_log_level
              ~default:(Hh_logger.Level.to_enum_string default.min_log_level)
              config))
    with
    | Some level -> level
    | None -> Hh_logger.Level.Debug
  in

  let use_saved_state =
    bool_if_min_version
      Config_keys.Hhconf.use_mini_state
      ~default:default.use_saved_state
      ~current_version
      config
  in
  let use_saved_state_when_indexing =
    bool_if_min_version
      Config_keys.Hhconf.use_mini_state_when_indexing
      ~default:default.use_saved_state_when_indexing
      ~current_version
      config
  in
  let require_saved_state =
    bool_if_min_version
      Config_keys.Hhconf.require_saved_state
      ~default:default.require_saved_state
      ~current_version
      config
  in
  let saved_state_flags =
    Saved_state_rollouts.make
      ~get_default:(fun name -> bool_ name ~default:false config)
      ~current_rolled_out_flag_idx
      ~deactivate_saved_state_rollout
      ~force_flag_value:(string_opt Config_keys.Hhconf.ss_force config)
  in
  (if not silent then
    output_config_section "Saved state rollout flags" @@ fun () ->
    Saved_state_rollouts.output saved_state_flags);
  let project_metadata_w_flags =
    bool_
      Config_keys.Hhconf.project_metadata_w_flags
      ~default:default.saved_state.Global_options.project_metadata_w_flags
      config
  in
  let attempt_fix_credentials =
    bool_if_min_version
      Config_keys.Hhconf.attempt_fix_credentials
      ~default:default.attempt_fix_credentials
      ~current_version
      config
  in
  let enable_on_nfs =
    bool_if_min_version
      Config_keys.Hhconf.enable_on_nfs
      ~default:default.enable_on_nfs
      ~current_version
      config
  in
  let enable_fuzzy_search =
    bool_if_min_version
      Config_keys.Hhconf.enable_fuzzy_search
      ~default:default.enable_fuzzy_search
      ~current_version
      config
  in
  let enable_global_access_check =
    bool_
      Config_keys.Hhconf.enable_global_access_check
      ~default:default.enable_global_access_check
      config
  in
  let max_purgatory_clients =
    int_
      Config_keys.Hhconf.max_purgatory_clients
      ~default:default.max_purgatory_clients
      config
  in
  let search_chunk_size =
    int_
      Config_keys.Hhconf.search_chunk_size
      ~default:default.search_chunk_size
      config
  in
  let load_state_natively =
    bool_if_min_version
      Config_keys.Hhconf.load_state_natively_v4
      ~default:default.load_state_natively
      ~current_version
      config
  in
  let load_state_natively_download_timeout =
    int_
      Config_keys.Hhconf.load_state_natively_download_timeout
      ~default:default.load_state_natively_download_timeout
      config
  in
  let load_state_natively_dirty_files_timeout =
    int_
      Config_keys.Hhconf.load_state_natively_dirty_files_timeout
      ~default:default.load_state_natively_dirty_files_timeout
      config
  in
  let use_dummy_informant =
    bool_if_min_version
      Config_keys.Hhconf.use_dummy_informant
      ~default:default.use_dummy_informant
      ~current_version
      config
  in
  let edenfs_informant_enabled =
    bool_if_min_version
      "edenfs_informant_enabled"
      ~default:default.edenfs_informant_enabled
      ~current_version
      config
  in
  let informant_min_distance_restart =
    int_
      Config_keys.Hhconf.informant_min_distance_restart
      ~default:default.informant_min_distance_restart
      config
  in
  let type_decl_bucket_size =
    int_
      Config_keys.Hhconf.type_decl_bucket_size
      ~default:default.type_decl_bucket_size
      config
  in
  let extend_defs_per_file_bucket_size =
    int_
      Config_keys.Hhconf.extend_defs_per_file_bucket_size
      ~default:default.extend_defs_per_file_bucket_size
      config
  in
  let io_priority =
    int_ Config_keys.Hhconf.io_priority ~default:default.io_priority config
  in
  let cpu_priority =
    int_ Config_keys.Hhconf.cpu_priority ~default:default.cpu_priority config
  in
  let shm_dirs =
    string_list Config_keys.Hhconf.shm_dirs ~default:default.shm_dirs config
    |> List.map ~f:(fun dir -> Path.(to_string @@ make dir))
  in
  let shm_use_sharded_hashtbl =
    bool_if_min_version
      Config_keys.Hhconfig.shm_use_sharded_hashtbl
      ~default:default.shm_use_sharded_hashtbl
      ~current_version
      config
  in
  let shm_cache_size =
    int_
      Config_keys.Hhconfig.shm_cache_size
      ~default:default.shm_cache_size
      config
  in
  let max_workers = int_opt Config_keys.Hhconf.max_workers config in
  (* interrupt_on_watchman is a deprecated alias of interrupt_on_file_changes *)
  let interrupt_on_file_changes =
    bool_if_min_version
      Config_keys.Hhconf.interrupt_on_watchman
      ~default:default.interrupt_on_file_changes
      ~current_version
      config
  in
  let interrupt_on_file_changes =
    bool_if_min_version
      Config_keys.Hhconf.interrupt_on_file_changes
      ~default:interrupt_on_file_changes
      ~current_version
      config
  in
  let interrupt_on_client =
    bool_if_min_version
      Config_keys.Hhconf.interrupt_on_client
      ~default:default.interrupt_on_client
      ~current_version
      config
  in
  let use_full_fidelity_parser =
    bool_if_min_version
      Config_keys.Hhconf.use_full_fidelity_parser
      ~default:default.use_full_fidelity_parser
      ~current_version
      config
  in
  let trace_parsing =
    bool_if_min_version
      Config_keys.Hhconf.trace_parsing
      ~default:default.trace_parsing
      ~current_version
      config
  in
  let prechecked_files =
    bool_if_min_version
      Config_keys.Hhconf.prechecked_files
      ~default:default.prechecked_files
      ~current_version
      config
  in
  let enable_type_check_filter_files =
    bool_if_min_version
      Config_keys.Hhconf.enable_type_check_filter_files
      ~default:default.enable_type_check_filter_files
      ~current_version
      config
  in
  let predeclare_ide =
    bool_if_min_version
      Config_keys.Hhconf.predeclare_ide
      ~default:default.predeclare_ide
      ~current_version
      config
  in
  let longlived_workers =
    bool_if_min_version
      Config_keys.Hhconf.longlived_workers
      ~default:default.longlived_workers
      ~current_version
      config
  in
  let block_client_connections_while_deferring =
    bool_if_min_version
      Config_keys.Hhconf.block_client_connections_while_deferring
      ~default:default.block_client_connections_while_deferring
      ~current_version
      config
  in
  let hg_aware =
    bool_if_min_version
      Config_keys.Hhconf.hg_aware
      ~default:default.hg_aware
      ~current_version
      config
  in
  let allow_restart_from_precomputed_saved_state =
    bool_if_min_version
      Config_keys.Hhconf.allow_restart_from_precomputed_saved_state
      ~default:default.allow_restart_from_precomputed_saved_state
      ~current_version
      config
  in
  let store_decls_in_saved_state =
    bool_if_min_version
      Config_keys.Hhconf.store_decls_in_saved_state
      ~default:default.store_decls_in_saved_state
      ~current_version
      config
  in
  let hg_aware_parsing_restart_threshold =
    int_
      Config_keys.Hhconf.hg_aware_parsing_restart_threshold
      ~default:default.hg_aware_parsing_restart_threshold
      config
  in
  let hg_aware_redecl_restart_threshold =
    int_
      Config_keys.Hhconf.hg_aware_redecl_restart_threshold
      ~default:default.hg_aware_redecl_restart_threshold
      config
  in
  let hg_aware_recheck_restart_threshold =
    int_
      Config_keys.Hhconf.hg_aware_recheck_restart_threshold
      ~default:default.hg_aware_recheck_restart_threshold
      config
  in
  let ide_parser_cache =
    bool_if_min_version
      Config_keys.Hhconf.ide_parser_cache
      ~default:default.ide_parser_cache
      ~current_version
      config
  in
  let idle_gc_slice =
    int_ Config_keys.Hhconf.idle_gc_slice ~default:default.idle_gc_slice config
  in
  let populate_member_heaps =
    bool_if_min_version
      Config_keys.Hhconf.populate_member_heaps
      ~default:default.populate_member_heaps
      ~current_version
      config
  in
  let fetch_remote_old_decls =
    bool_if_min_version
      Config_keys.Hhconf.fetch_remote_old_decls
      ~default:default.fetch_remote_old_decls
      ~current_version
      config
  in
  let only_fetch_remote_old_decl_during_init =
    bool_if_min_version
      Config_keys.Hhconf.only_fetch_remote_old_decl_during_init
      ~default:default.only_fetch_remote_old_decl_during_init
      ~current_version
      config
  in
  let disable_rust_provider_shallow_decl_invalidation =
    bool_if_min_version
      Config_keys.Hhconf.disable_rust_provider_shallow_decl_invalidation
      ~default:default.disable_rust_provider_shallow_decl_invalidation
      ~current_version
      config
  in
  let enable_annotation_agnostic_decl_diffing =
    bool_if_min_version
      Config_keys.Hhconf.enable_annotation_agnostic_decl_diffing
      ~default:default.enable_annotation_agnostic_decl_diffing
      ~current_version
      config
  in
  let skip_hierarchy_checks =
    bool_if_min_version
      Config_keys.Hhconf.skip_hierarchy_checks
      ~default:default.skip_hierarchy_checks
      ~current_version
      config
  in
  let skip_tast_checks =
    bool_if_min_version
      Config_keys.Hhconf.skip_tast_checks
      ~default:default.skip_tast_checks
      ~current_version
      config
  in
  let silence_errors_under_dynamic =
    bool_if_min_version
      Config_keys.Hhconfig.silence_errors_under_dynamic
      ~default:default.silence_errors_under_dynamic
      ~current_version
      config
  in
  let num_local_workers = int_opt Config_keys.Hhconf.num_local_workers config in
  let heartbeat_interval =
    int_opt Config_keys.Hhconf.heartbeat_interval config
  in
  let prefetch_decls_enabled =
    bool_
      Config_keys.Hhconf.prefetch_decls_enabled
      ~default:default.prefetch_decls_enabled
      config
  in
  let prefetch_decls_threshold =
    int_
      Config_keys.Hhconf.prefetch_decls_threshold
      ~default:default.prefetch_decls_threshold
      config
  in
  let produce_streaming_errors =
    bool_
      Config_keys.Hhconf.produce_streaming_errors
      ~default:default.produce_streaming_errors
      config
  in
  let consume_streaming_errors =
    bool_
      Config_keys.Hhconf.consume_streaming_errors
      ~default:default.consume_streaming_errors
      config
  in
  let use_agent_error_format =
    match bool_opt Config_keys.Hhconf.use_agent_error_format config with
    | None -> Agent_error_format_unconfigured
    | Some false -> Agent_error_format_disabled
    | Some true -> Agent_error_format_enabled
  in
  let watchman =
    Watchman.load ~current_version ~default:default.watchman config
  in
  let edenfs_file_watcher =
    EdenfsFileWatcher.load
      ~current_version
      ~default:default.edenfs_file_watcher
      config
  in
  let eden_prefetch_enabled =
    bool_if_min_version
      Config_keys.Hhconf.eden_prefetch_enabled
      ~default:default.eden_prefetch_enabled
      ~current_version
      config
  in
  let eden_prefetch_fanout_threshold =
    int_
      Config_keys.Hhconf.eden_prefetch_fanout_threshold
      ~default:default.eden_prefetch_fanout_threshold
      config
  in
  let eden_prefetch_workers =
    int_
      Config_keys.Hhconf.eden_prefetch_workers
      ~default:default.eden_prefetch_workers
      config
  in
  let enable_naming_table_fallback =
    bool_if_min_version
      Config_keys.Hhconf.enable_naming_table_fallback
      ~default:default.enable_naming_table_fallback
      ~current_version
      config
  in
  let naming_sqlite_path =
    if enable_naming_table_fallback then
      string_opt Config_keys.Hhconf.naming_sqlite_path config
    else
      None
  in
  let ide_symbolindex_search_provider =
    string_
      Config_keys.Hhconf.ide_symbolindex_search_provider
      ~default:default.ide_symbolindex_search_provider
      config
  in
  let symbolindex_quiet =
    bool_if_min_version
      Config_keys.Hhconf.symbolindex_quiet
      ~default:default.symbolindex_quiet
      ~current_version
      config
  in
  let tico_invalidate_files =
    bool_if_min_version
      Config_keys.Hhconf.tico_invalidate_files
      ~default:default.tico_invalidate_files
      ~current_version
      config
  in
  let tico_invalidate_smart =
    bool_if_min_version
      Config_keys.Hhconf.tico_invalidate_smart
      ~default:default.tico_invalidate_smart
      ~current_version
      config
  in
  let profile_log =
    bool_if_min_version
      Config_keys.Hhconf.profile_log
      ~default:Hack_event_logger.PerFileProfilingConfig.(default.profile_log)
      ~current_version
      config
  in
  let profile_type_check_duration_threshold =
    float_
      Config_keys.Hhconf.profile_type_check_duration_threshold
      ~default:
        Hack_event_logger.PerFileProfilingConfig.(
          default.profile_type_check_duration_threshold)
      config
  in
  let profile_type_check_memory_threshold_mb =
    int_
      Config_keys.Hhconf.profile_type_check_memory_threshold_mb
      ~default:
        Hack_event_logger.PerFileProfilingConfig.(
          default.profile_type_check_memory_threshold_mb)
      config
  in
  let profile_type_check_twice =
    bool_if_min_version
      Config_keys.Hhconf.profile_type_check_twice
      ~default:
        Hack_event_logger.PerFileProfilingConfig.(
          default.profile_type_check_twice)
      ~current_version
      config
  in
  let profile_decling =
    match string_opt Config_keys.Hhconf.profile_decling config with
    | None ->
      default.per_file_profiling
        .Hack_event_logger.PerFileProfilingConfig.profile_decling
    | Some value_s ->
      (match
         Hack_event_logger.PerFileProfilingConfig.ProfileDecling.of_config_value
           value_s
       with
      | Some x -> x
      | None ->
        failwith
        @@ Printf.sprintf
             "Unrecognized value %s for profile_decling. Allowed values ar 'off', 'top_counts', 'all_telemetry', 'all_telemetry_callstacks'"
             value_s)
  in
  let profile_owner = string_opt Config_keys.Hhconf.profile_owner config in
  let profile_desc = string_opt Config_keys.Hhconf.profile_desc config in
  let profile_slow_threshold =
    float_
      Config_keys.Hhconf.profile_slow_threshold
      ~default:
        Hack_event_logger.PerFileProfilingConfig.(
          default.profile_slow_threshold)
      config
  in
  let memtrace_dir = string_opt Config_keys.Hhconf.memtrace_dir config in
  let go_to_implementation =
    bool_if_min_version
      Config_keys.Hhconf.go_to_implementation
      ~default:default.go_to_implementation
      ~current_version
      config
  in
  let allow_unstable_features =
    bool_if_min_version
      Config_keys.Hhconf.allow_unstable_features
      ~default:default.allow_unstable_features
      ~current_version
      config
  in
  let saved_state_max_expected_age_days =
    let days =
      int_
        Config_keys.Hhconf.saved_state_max_expected_age_days
        ~default:
          Global_options.(
            default_saved_state_loading.saved_state_max_expected_age_days)
        config
    in
    if days > 0 then
      days
    else
      failwith
        (Printf.sprintf
           "%s must be positive, got %d"
           Config_keys.Hhconf.saved_state_max_expected_age_days
           days)
  in
  let use_eden =
    bool_
      Config_keys.Hhconf.saved_state_lookup_use_eden
      ~default:Global_options.(default_saved_state_loading.use_eden)
      config
  in
  let database_shard_name =
    string_opt Config_keys.Hhconf.saved_state_database_shard_name config
  in
  let use_manifold_cython_client =
    bool_if_min_version
      Config_keys.Hhconf.use_manifold_cython_client
      ~default:
        Global_options.(default_saved_state_loading.use_manifold_cython_client)
      ~current_version
      config
  in
  let workload_quantile =
    int_list_opt Config_keys.Hhconf.workload_quantile config >>= fun l ->
    match l with
    | [m; n] ->
      if 0 <= m && m <= n then
        Some { count = n; index = m }
      else if 0 <= n && n <= m then
        Some { count = m; index = n }
      else
        None
    | _ -> None
  in
  let rollout_group = string_opt Config_keys.Hhconf.rollout_group config in
  let status_single_use_cached_diagnostics =
    bool_
      Config_keys.Hhconf.status_single_use_cached_diagnostics
      ~default:default.status_single_use_cached_diagnostics
      config
  in
  let specify_manifold_api_key =
    bool_if_min_version
      Config_keys.Hhconf.specify_manifold_api_key
      ~default:default.specify_manifold_api_key
      ~current_version
      config
  in
  let saved_state_manifold_api_key =
    (* overriding the local_config value so consumers of saved_state_manifold_api_key
       * don't need to explicitly check for specify_manifold_api_key.
    *)
    if specify_manifold_api_key then
      string_opt Config_keys.Hhconf.saved_state_manifold_api_key config
    else
      None
  in
  let manifold_local_port =
    int_opt Config_keys.Hhconf.saved_state_manifold_local_port config
  in
  let rust_provider_backend =
    bool_if_min_version
      Config_keys.Hhconf.rust_provider_backend
      ~default:default.rust_provider_backend
      ~current_version
      config
  in
  let set_global_provider_backend_in_workers =
    bool_if_min_version
      Config_keys.Hhconf.set_global_provider_backend_in_workers
      ~default:default.set_global_provider_backend_in_workers
      ~current_version
      config
  in
  let isolation_allow_decl_repackaging =
    bool_if_min_version
      Config_keys.Hhconf.isolation_allow_decl_repackaging
      ~default:default.isolation_allow_decl_repackaging
      ~current_version
      config
  in
  let rust_provider_backend =
    if rust_provider_backend && not shm_use_sharded_hashtbl then (
      Hh_logger.warn
        "You have rust_provider_backend=true but shm_use_sharded_hashtbl=false. This is incompatible. Turning off rust_provider_backend";
      false
    ) else
      rust_provider_backend
  in
  let rust_provider_backend =
    if rust_provider_backend && populate_member_heaps then (
      Hh_logger.warn
        "You have rust_provider_backend=true but populate_member_heaps=true. This is incompatible. Turning off rust_provider_backend";
      false
    ) else
      rust_provider_backend
  in
  let cache_remote_decls =
    bool_if_min_version
      Config_keys.Hhconf.cache_remote_decls
      ~default:default.cache_remote_decls
      ~current_version
      config
  in
  let use_distc =
    bool_if_min_version
      Config_keys.Hhconf.use_distc
      ~default:default.use_distc
      ~current_version
      config
  in
  let enable_fanout_aware_distc =
    bool_if_min_version
      Config_keys.Hhconf.enable_fanout_aware_distc
      ~default:default.enable_fanout_aware_distc
      ~current_version
      config
  in
  let use_compressed_dep_graph =
    bool_if_min_version
      Config_keys.Hhconf.use_compressed_dep_graph
      ~default:default.use_compressed_dep_graph
      ~current_version
      config
  in
  let hh_distc_fanout_threshold =
    int_
      Config_keys.Hhconf.hh_distc_fanout_threshold
      ~default:default.hh_distc_fanout_threshold
      config
  in
  let hh_distc_fanout_full_init_threshold =
    int_
      Config_keys.Hhconf.hh_distc_fanout_full_init_threshold
      ~default:default.hh_distc_fanout_full_init_threshold
      config
  in
  let hh_distc_exponential_backoff_num_retries =
    int_
      Config_keys.Hhconfig.hh_distc_exponential_backoff_num_retries
      ~default:default.hh_distc_exponential_backoff_num_retries
      config
  in
  let ide_file_watcher_enabled =
    bool_if_min_version
      Config_keys.Hhconf.ide_file_watcher_enabled
      ~default:default.ide_file_watcher_enabled
      ~current_version
      config
  in
  let ide_file_watcher_max_changed_files =
    int_
      Config_keys.Hhconf.ide_file_watcher_max_changed_files
      ~default:default.ide_file_watcher_max_changed_files
      config
  in
  let ide_load_naming_table_on_disk =
    bool_if_min_version
      Config_keys.Hhconf.ide_load_naming_table_on_disk
      ~default:default.ide_load_naming_table_on_disk
      ~current_version
      config
  in
  let ide_naming_table_update_threshold =
    int_
      Config_keys.Hhconf.ide_naming_table_update_threshold
      ~default:default.ide_naming_table_update_threshold
      config
  in
  let dump_tast_hashes =
    bool_
      Config_keys.Hhconfig.dump_tast_hashes
      ~default:default.dump_tast_hashes
      config
  in
  let dump_tasts =
    let path_opt = string_opt Config_keys.Hhconf.dump_tasts config in
    match path_opt with
    | None -> default.dump_tasts
    | Some path -> In_channel.read_lines path
  in
  let hack_warnings =
    bool_
      Config_keys.Hhconfig.hack_warnings
      ~default:default.hack_warnings
      config
  in
  let zstd_decompress_by_file =
    bool_
      Config_keys.Hhconf.zstd_decompress_by_file
      ~default:
        Global_options.(default_saved_state_loading.zstd_decompress_by_file)
      config
  in
  let saved_state_cache_limit =
    int_
      Config_keys.Hhconf.saved_state_cache_limit
      ~default:
        Global_options.(default_saved_state_loading.saved_state_cache_limit)
      config
  in
  let warnings_default_all =
    bool_
      Config_keys.Hhconfig.warnings_default_all
      ~default:default.warnings_default_all
      config
  in
  let warnings_in_sandcastle =
    bool_
      Config_keys.Hhconfig.warnings_in_sandcastle
      ~default:default.warnings_in_sandcastle
      config
  in
  let package_config_strict_validation =
    bool_
      Config_keys.Hhconf.package_config_strict_validation
      ~default:default.package_config_strict_validation
      config
  in
  (* Fields primarily for hh_conf equivalence testing *)
  let sharedmem_global_size =
    int_
      Config_keys.Hhconfig.sharedmem_global_size
      ~default:default.sharedmem_global_size
      config
  in
  let sharedmem_hash_table_pow =
    int_
      Config_keys.Hhconfig.sharedmem_hash_table_pow
      ~default:default.sharedmem_hash_table_pow
      config
  in
  let sharedmem_heap_size =
    int_
      Config_keys.Hhconfig.sharedmem_heap_size
      ~default:default.sharedmem_heap_size
      config
  in
  let eden_fetch_parallelism =
    int_
      Config_keys.Hhconf.eden_fetch_parallelism
      ~default:default.eden_fetch_parallelism
      config
  in
  let use_distc_crawl_dircache =
    bool_
      Config_keys.Hhconf.use_distc_crawl_dircache
      ~default:default.use_distc_crawl_dircache
      config
  in
  let gc_minor_heap_size =
    int_
      Config_keys.Hhconfig.gc_minor_heap_size
      ~default:default.gc_minor_heap_size
      config
  in
  let gc_space_overhead =
    int_
      Config_keys.Hhconfig.gc_space_overhead
      ~default:default.gc_space_overhead
      config
  in
  let ide_fall_back_to_full_index =
    bool_
      Config_keys.Hhconfig.ide_fall_back_to_full_index
      ~default:default.ide_fall_back_to_full_index
      config
  in
  let naming_table_compression_level =
    int_
      Config_keys.Hhconfig.naming_table_compression_level
      ~default:default.naming_table_compression_level
      config
  in
  let naming_table_compression_threads =
    int_
      Config_keys.Hhconfig.naming_table_compression_threads
      ~default:default.naming_table_compression_threads
      config
  in
  let config_version = string_opt Config_keys.Hhconfig.version config in
  let parse_string_list s =
    try
      match Yojson.Safe.from_string s with
      | `List l ->
        List.map l ~f:(fun v ->
            match v with
            | `String s -> s
            | _ -> failwith "expected string")
      | _ -> []
    with
    | _ -> []
  in
  let ignored_paths =
    match string_opt Config_keys.Hhconfig.ignored_paths config with
    | None -> default.ignored_paths
    | Some s -> parse_string_list s
  in
  {
    saved_state =
      {
        Global_options.loading =
          {
            Global_options.saved_state_manifold_api_key;
            manifold_local_port;
            use_eden;
            database_shard_name;
            saved_state_max_expected_age_days;
            use_manifold_cython_client;
            zstd_decompress_by_file;
            use_compressed_dep_graph;
            saved_state_cache_limit;
          };
        rollouts = saved_state_flags;
        project_metadata_w_flags;
      };
    min_log_level;
    attempt_fix_credentials;
    log_categories;
    log_large_fanouts_threshold;
    log_init_proc_stack_also_on_absent_from;
    art_cli_lite_exit_status_enabled;
    log_inference_constraints;
    experiments;
    experiments_config_meta;
    use_saved_state;
    use_saved_state_when_indexing;
    require_saved_state;
    load_state_natively;
    load_state_natively_download_timeout;
    load_state_natively_dirty_files_timeout;
    max_purgatory_clients;
    type_decl_bucket_size;
    extend_defs_per_file_bucket_size;
    enable_on_nfs;
    enable_fuzzy_search;
    enable_global_access_check;
    search_chunk_size;
    io_priority;
    cpu_priority;
    shm_dirs;
    shm_use_sharded_hashtbl;
    shm_cache_size;
    max_workers;
    use_dummy_informant;
    edenfs_informant_enabled;
    informant_min_distance_restart;
    use_full_fidelity_parser;
    interrupt_on_file_changes;
    interrupt_on_client;
    trace_parsing;
    prechecked_files;
    enable_type_check_filter_files;
    ide_symbolindex_search_provider;
    predeclare_ide;
    longlived_workers;
    block_client_connections_while_deferring;
    hg_aware;
    allow_restart_from_precomputed_saved_state;
    hg_aware_parsing_restart_threshold;
    hg_aware_redecl_restart_threshold;
    hg_aware_recheck_restart_threshold;
    ide_parser_cache;
    store_decls_in_saved_state;
    idle_gc_slice;
    populate_member_heaps;
    fetch_remote_old_decls;
    only_fetch_remote_old_decl_during_init;
    disable_rust_provider_shallow_decl_invalidation;
    enable_annotation_agnostic_decl_diffing;
    skip_hierarchy_checks;
    skip_tast_checks;
    silence_errors_under_dynamic;
    num_local_workers;
    heartbeat_interval;
    prefetch_decls_enabled;
    prefetch_decls_threshold;
    produce_streaming_errors;
    consume_streaming_errors;
    use_agent_error_format;
    rust_provider_backend;
    set_global_provider_backend_in_workers;
    isolation_allow_decl_repackaging;
    naming_sqlite_path;
    enable_naming_table_fallback;
    symbolindex_quiet;
    tico_invalidate_files;
    tico_invalidate_smart;
    per_file_profiling =
      {
        Hack_event_logger.PerFileProfilingConfig.profile_log;
        profile_type_check_duration_threshold;
        profile_type_check_memory_threshold_mb;
        profile_type_check_twice;
        profile_decling;
        profile_owner;
        profile_desc;
        profile_slow_threshold;
      };
    memtrace_dir;
    go_to_implementation;
    allow_unstable_features;
    watchman;
    edenfs_file_watcher;
    eden_prefetch_enabled;
    eden_prefetch_fanout_threshold;
    eden_prefetch_workers;
    workload_quantile;
    rollout_group;
    status_single_use_cached_diagnostics;
    specify_manifold_api_key;
    cache_remote_decls;
    use_compressed_dep_graph;
    use_distc;
    enable_fanout_aware_distc;
    hh_distc_fanout_threshold;
    hh_distc_fanout_full_init_threshold;
    hh_distc_exponential_backoff_num_retries;
    ide_file_watcher_enabled;
    ide_file_watcher_max_changed_files;
    ide_load_naming_table_on_disk;
    ide_naming_table_update_threshold;
    dump_tast_hashes;
    dump_tasts;
    hack_warnings;
    warnings_default_all;
    warnings_in_sandcastle;
    package_config_strict_validation;
    sharedmem_global_size;
    sharedmem_hash_table_pow;
    sharedmem_heap_size;
    eden_fetch_parallelism;
    use_distc_crawl_dircache;
    gc_minor_heap_size;
    gc_space_overhead;
    ide_fall_back_to_full_index;
    naming_table_compression_level;
    naming_table_compression_threads;
    config_version;
    ignored_paths;
  }

let load_with_dynamic_overrides
    ~apply_dynamic_overrides
    ~silent
    ~current_version
    ~current_rolled_out_flag_idx
    ~deactivate_saved_state_rollout
    ~from
    ~overrides : t =
  load_
    ~apply_dynamic_overrides
    system_config_path
    ~silent
    ~current_version
    ~current_rolled_out_flag_idx
    ~deactivate_saved_state_rollout
    ~from
    ~overrides

let load
    ~silent
    ~current_version
    ~current_rolled_out_flag_idx
    ~deactivate_saved_state_rollout
    ~from
    ~overrides : t =
  load_with_dynamic_overrides
    ~apply_dynamic_overrides:(fun ~silent:_ config -> config)
    ~silent
    ~current_version
    ~current_rolled_out_flag_idx
    ~deactivate_saved_state_rollout
    ~from
    ~overrides

(** Load ServerLocalConfig from already-parsed config contents.
    This is intended for testing, bypassing file reads and overrides.
    Now delegates to load_ to avoid code duplication. *)
let load_from_config
    ~silent
    ~current_version
    ~current_rolled_out_flag_idx
    ~deactivate_saved_state_rollout
    (config : Config_file_common.t) : t =
  load_
    ~config
    "" (* system_config_path unused when config is provided *)
    ~apply_dynamic_overrides:(fun ~silent:_ config -> config)
    ~silent
    ~current_version
    ~current_rolled_out_flag_idx
    ~deactivate_saved_state_rollout
    ~from:""
    ~overrides:(Config_file_common.empty ())

let to_rollout_flags (options : t) : Hack_event_logger.rollout_flags =
  Hack_event_logger.
    {
      saved_state_lookup_use_eden =
        Global_options.(options.saved_state.loading.use_eden);
      fetch_remote_old_decls = options.fetch_remote_old_decls;
      only_fetch_remote_old_decl_during_init =
        options.only_fetch_remote_old_decl_during_init;
      disable_rust_provider_shallow_decl_invalidation =
        options.disable_rust_provider_shallow_decl_invalidation;
      enable_annotation_agnostic_decl_diffing =
        options.enable_annotation_agnostic_decl_diffing;
      specify_manifold_api_key = options.specify_manifold_api_key;
      populate_member_heaps = options.populate_member_heaps;
      shm_use_sharded_hashtbl = options.shm_use_sharded_hashtbl;
      shm_cache_size = options.shm_cache_size;
      use_manifold_cython_client =
        Global_options.(options.saved_state.loading.use_manifold_cython_client);
      load_state_natively_v4 = options.load_state_natively;
      rust_provider_backend = options.rust_provider_backend;
      set_global_provider_backend_in_workers =
        options.set_global_provider_backend_in_workers;
      use_distc = options.use_distc;
      enable_fanout_aware_distc = options.enable_fanout_aware_distc;
      use_compressed_dep_graph = options.use_compressed_dep_graph;
      consume_streaming_errors = options.consume_streaming_errors;
      status_single_use_cached_diagnostics =
        options.status_single_use_cached_diagnostics;
      hh_distc_fanout_threshold = options.hh_distc_fanout_threshold;
      hh_distc_fanout_full_init_threshold =
        options.hh_distc_fanout_full_init_threshold;
      ide_file_watcher_enabled = options.ide_file_watcher_enabled;
      ide_load_naming_table_on_disk = options.ide_load_naming_table_on_disk;
      ide_naming_table_update_threshold =
        options.ide_naming_table_update_threshold;
      saved_state_rollouts = options.saved_state.Global_options.rollouts;
      zstd_decompress_by_file =
        Global_options.(options.saved_state.loading.zstd_decompress_by_file);
      warnings_default_all = options.warnings_default_all;
      edenfs_file_watcher_enabled = options.edenfs_file_watcher.enabled;
      edenfs_file_watcher_sync_queries_obey_deferral =
        options.edenfs_file_watcher.sync_queries_obey_deferral;
      edenfs_file_watcher_state_tracking =
        options.edenfs_file_watcher.state_tracking;
      edenfs_informant_enabled = options.edenfs_informant_enabled;
      eden_prefetch_enabled = options.eden_prefetch_enabled;
      eden_prefetch_fanout_threshold = options.eden_prefetch_fanout_threshold;
      prefetch_decls_enabled = options.prefetch_decls_enabled;
    }
