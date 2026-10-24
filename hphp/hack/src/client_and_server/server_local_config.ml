(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Config_file.Getters

module Watchman = struct
  type t = {
    (* use_watchman *)
    enabled: bool;
    (* in seconds *)
    debug_logging: bool;
    init_timeout: int;
    sockname: string option;
    subscribe: bool;
    (* in seconds *)
    synchronous_timeout: int;
  }

  let default =
    {
      debug_logging = false;
      enabled = false;
      (* buck and hgwatchman use a 10 second timeout, too *)
      init_timeout = 10;
      sockname = None;
      subscribe = false;
      synchronous_timeout = 120;
    }

  let load ~current_version ~default config =
    let use_watchman =
      bool_if_min_version
        Config_keys.Hhconf.use_watchman
        ~default:default.enabled
        ~current_version
        config
    in
    let enabled =
      bool_if_min_version
        Config_keys.Hhconf.watchman_enabled
        ~default:use_watchman
        ~current_version
        config
    in
    let init_timeout =
      int_
        Config_keys.Hhconf.watchman_init_timeout
        ~default:default.init_timeout
        config
    in
    let sockname = string_opt Config_keys.Hhconf.watchman_sockname config in
    let subscribe =
      bool_if_min_version
        Config_keys.Hhconf.watchman_subscribe_v2
        ~default:default.subscribe
        ~current_version
        config
    in
    let synchronous_timeout =
      int_
        Config_keys.Hhconf.watchman_synchronous_timeout
        ~default:default.synchronous_timeout
        config
    in
    let debug_logging =
      bool_if_min_version
        Config_keys.Hhconf.watchman_debug_logging
        ~default:default.debug_logging
        ~current_version
        config
    in
    {
      debug_logging;
      enabled;
      init_timeout;
      sockname;
      subscribe;
      synchronous_timeout;
    }
end

module EdenfsFileWatcher = struct
  type t = {
    enabled: bool;
    debug_logging: bool;
    timeout_secs: int;  (** Timeout, in seconds *)
    sync_queries_obey_deferral: bool;
        (** If false, ServerNotifier.get_changes_sync backed by Edenfs_watcher will return
            file changes even when we are currently deferring changes (e.g. because meerkat is
            running). *)
    throttle_time_ms: int;
        (** Value of throttle_time_ms parameter passed to Eden's stream_changes_since API.
            This means that this is the minimum period (in milliseconds) between each time
            Eden will send us a change notification. *)
    report_telemetry: bool;
        (** If true, will log additional telemetry collected from within Edenfs_watcher *)
    state_tracking: bool;
        (** Whether to use Eden's state tracking feature for deferring file changes *)
    tracked_states: string list;
        (** List of state names to track for deferring file changes.
            Only used when state_tracking is enabled. *)
  }

  let default =
    {
      debug_logging = false;
      enabled = false;
      timeout_secs = 60;
      throttle_time_ms = 50;
      sync_queries_obey_deferral = true;
      report_telemetry = false;
      state_tracking = false;
      tracked_states =
        [Hg_states.transaction; Hg_states.update; "meerkat-build"];
    }

  let load ~current_version ~default config =
    let enabled =
      bool_if_min_version
        Config_keys.Hhconf.edenfs_file_watcher_enabled
        ~default:default.enabled
        ~current_version
        config
    in
    let debug_logging =
      bool_if_min_version
        Config_keys.Hhconf.edenfs_file_watcher_debug_logging
        ~default:default.debug_logging
        ~current_version
        config
    in
    let timeout_secs =
      int_
        Config_keys.Hhconf.edenfs_file_watcher_timeout_secs
        ~default:default.timeout_secs
        config
    in
    let sync_queries_obey_deferral =
      bool_if_min_version
        Config_keys.Hhconf.edenfs_file_watcher_sync_queries_obey_deferral
        ~default:default.sync_queries_obey_deferral
        ~current_version
        config
    in
    let throttle_time_ms =
      int_
        Config_keys.Hhconf.edenfs_file_watcher_throttle_time_ms
        ~default:default.throttle_time_ms
        config
    in
    let report_telemetry =
      bool_
        Config_keys.Hhconf.edenfs_file_watcher_report_telemetry
        ~default:default.report_telemetry
        config
    in
    let state_tracking =
      bool_
        Config_keys.Hhconf.edenfs_file_watcher_state_tracking
        ~default:default.state_tracking
        config
    in
    let tracked_states =
      string_list
        Config_keys.Hhconf.edenfs_file_watcher_tracked_states
        ~default:default.tracked_states
        config
    in
    {
      debug_logging;
      enabled;
      timeout_secs;
      sync_queries_obey_deferral;
      throttle_time_ms;
      report_telemetry;
      state_tracking;
      tracked_states;
    }
end

(** Allows to typecheck only a certain quantile of the workload. *)
type quantile = {
  count: int;
      (** The number of quantiles we want.
          If this is n, we'll divide the workload in n groups. *)
  index: int;
      (** The index of the subgroup we'll process.
          If this is i, we'll typecheck group number i out of the n groups.
          this should be in interval [0; n] *)
}

type agent_error_format_config =
  | Agent_error_format_unconfigured
  | Agent_error_format_disabled
  | Agent_error_format_enabled

(** These are the flags loaded from the following sources:
  - /etc/hh.conf
  - overridden by Knobs
  - overridden by ExperimentsConfig
  - overridden by CLI --config flags *)
type t = {
  saved_state: Global_options.saved_state;
  min_log_level: Hh_logger.Level.t;
  attempt_fix_credentials: bool;
      (** Indicates whether we attempt to fix the credentials if they're broken *)
  log_categories: string list;
  log_large_fanouts_threshold: int option;
      (** If a fanout is greater than this value, log stats about that fanout. *)
  log_init_proc_stack_also_on_absent_from: bool;
      (** A few select events like to log the init_proc_stack, but it's voluminous!
      The default behavior is to log init_proc_stack only when "--from" is absent.
      This flag lets us log it also when "--from" is present. *)
  art_cli_lite_exit_status_enabled: bool;
  log_inference_constraints: bool;
      (** log type inference constraints into HackEventLogger *)
  experiments: string list;
      (** the list of experiments from the experiments config *)
  experiments_config_meta: string;  (** a free-form diagnostic string *)
  use_saved_state: bool;
      (** should we attempt to load saved-state? (subject to further options) *)
  use_saved_state_when_indexing: bool;
      (** should we attempt to load saved-state when running glean indexing? *)
  require_saved_state: bool;
      (** if attempting saved-state, should we fail upon failure? *)
  load_state_natively: bool;
      (** make hh_server query and download saved state. *)
  load_state_natively_download_timeout: int;  (** in seconds *)
  load_state_natively_dirty_files_timeout: int;  (** in seconds *)
  type_decl_bucket_size: int;
  extend_defs_per_file_bucket_size: int;
  enable_on_nfs: bool;
  enable_fuzzy_search: bool;
  max_purgatory_clients: int;
      (** Monitor: Limit the number of clients that can sit in purgatory waiting
      for a server to be started because we don't want this to grow unbounded. *)
  search_chunk_size: int;
  io_priority: int;
  cpu_priority: int;
  shm_dirs: string list;
  shm_use_sharded_hashtbl: bool;
  shm_cache_size: int;
      (** Maximum shared memory cache size for evictable data.

      If this is set to a negative value, eviction is disabled. *)
  max_workers: int option;
  use_dummy_informant: bool;  (** See Informant. *)
  edenfs_informant_enabled: bool;
      (** If true, the Informant uses Edenfs_watcher instead of Watchman *)
  informant_min_distance_restart: int;
  use_full_fidelity_parser: bool;
  interrupt_on_file_changes: bool;
      (** Whether to interrupt an active type-checking job as soon as any relevant files change.
          Deprecated alias: interrupt_on_watchman *)
  interrupt_on_client: bool;
  trace_parsing: bool;
  prechecked_files: bool;
      (** Whether we use the prechecked algorithm, which only includes the fanout of
          changes since the mergebase when initializing *)
  enable_global_access_check: bool;
      (** run global access checker to check global writes and reads *)
  enable_type_check_filter_files: bool;
      (** Let the user configure which files to type check and
      which files to ignore. This flag is not expected to be
      rolled out broadly, rather it is meant to be used by
      power users only. *)
  ide_symbolindex_search_provider: string;
      (** Selects a search provider for autocomplete and symbol search *)
  predeclare_ide: bool;
  longlived_workers: bool;
  block_client_connections_while_deferring: bool;
      (** Block client admission while Eden defers file changes. Hg states only
          block when [hg_aware] is also enabled. *)
  hg_aware: bool;
  allow_restart_from_precomputed_saved_state: bool;
      (** Allow restarting a server started with a precomputed saved state.
          For example, restarts may be triggered by the Informant or
          [Server_revision_tracker]. *)
  hg_aware_parsing_restart_threshold: int;
  hg_aware_redecl_restart_threshold: int;
  hg_aware_recheck_restart_threshold: int;
  ide_parser_cache: bool;
  store_decls_in_saved_state: bool;
      (** When enabled, save hot class declarations (for now, specified in a special
      file in the repository) when generating a saved state. *)
  idle_gc_slice: int;
      (** Size of Gc.major_slice to be performed when server is idle. 0 to disable *)
  populate_member_heaps: bool;
      (** Populate the member signature heaps.

      If disabled, instead load lazily from shallow classes. *)
  fetch_remote_old_decls: bool;
      (** Option to fetch old decls from remote decl store *)
  only_fetch_remote_old_decl_during_init: bool;
      (** Restrict remote old decl fetches to init-time fanout. *)
  disable_rust_provider_shallow_decl_invalidation: bool;
      (** Preserve Rust shallow decls when invalidating folded decls. *)
  enable_annotation_agnostic_decl_diffing: bool;
      (** Ignore user-defined class attributes when computing fanout. *)
  skip_hierarchy_checks: bool;
      (** Skip checks on hierarchy e.g. overrides, require extend, etc.
      Set to true only for debugging purposes! *)
  skip_tast_checks: bool;
      (** Skip checks implemented using TAST visitors.
      Set to true only for debugging purposes! *)
  silence_errors_under_dynamic: bool;
      (** Do not report errors under the dynamic pass of the typechecker *)
  num_local_workers: int option;
      (** If None, only the type check delegate's logic will be used.
      If the delegate fails to type check, the typing check service as a whole
      will fail. *)
  heartbeat_interval: int option;
      (** If set, periodically log which file is being type-checked, every this many seconds *)
  prefetch_decls_enabled: bool;
      (** If true, prefetch class declarations in parallel before type-checking files
          with many class references. *)
  prefetch_decls_threshold: int;
      (** Minimum number of class name references in a file to trigger parallel decl prefetching. *)
  produce_streaming_errors: bool;
      (** whether hh_server should write errors to errors.bin file *)
  consume_streaming_errors: bool;
      (** whether hh_client should read errors from errors.bin file *)
  use_agent_error_format: agent_error_format_config;
      (** Whether hh_client should use the agent-oriented diagnostic format.
          [Agent_error_format_unconfigured] means the config value was not
          provided. *)
  rust_provider_backend: bool;
      (** Use Provider_backend.Rust_provider_backend as the global provider
       * backend, servicing File_provider, Naming_provider, and Decl_provider
       * using the hackrs implementation. *)
  isolation_allow_decl_repackaging: bool;
      (** Allow --validate-isolation to rebuild decls under a synthesized
       * package. While it runs, the candidate's symbols are absent from the
       * shared heaps or carry a package no repository declares, so only set
       * this on a server dedicated to the analysis. *)
  naming_sqlite_path: string option;
      (** Enables the reverse naming table to fall back to SQLite for queries. *)
  enable_naming_table_fallback: bool;
  symbolindex_quiet: bool;
  tico_invalidate_files: bool;
      (** Allows hh_server to invalidate units in hhvm based on local changes *)
  tico_invalidate_smart: bool;  (** Use finer grain hh_server dependencies *)
  per_file_profiling: Hack_event_logger.PerFileProfilingConfig.t;
      (** turns on memtrace .ctf writes to this directory *)
  memtrace_dir: string option;
  go_to_implementation: bool;
      (** Allows the IDE to show the 'find all implementations' button *)
  allow_unstable_features: bool;
      (** Allows unstable features to be enabled within a file via the '__EnableUnstableFeatures' attribute *)
  watchman: Watchman.t;
  edenfs_file_watcher: EdenfsFileWatcher.t;
  eden_prefetch_enabled: bool;
      (** Prefetch the type-checking fanout from the controller. *)
  eden_prefetch_fanout_threshold: int;
      (** Minimum number of files in the fanout to enable prefetching. *)
  eden_prefetch_workers: int;
      (** Maximum number of concurrent Sapling file fetch workers. *)
  workload_quantile: quantile option;
      (** Allows to typecheck only a certain quantile of the workload. *)
  rollout_group: string option;
      (** A string from hh.conf, written to HackEventLogger telemetry. Before it got
       into here, [t], it was first used as a lookup in ServerLocalConfigKnobs.
       Intended meaning: what class of user is running hh_server, hence what experiments
       should they be subject to. *)
  status_single_use_cached_diagnostics: bool;
      (** Reuse already-computed global diagnostics for hh --single when the server
          has completed typechecking. *)
  specify_manifold_api_key: bool;
  cache_remote_decls: bool;
      (** Configure whether fetch and cache remote decls *)
  use_distc: bool;
      (** use remote type-checking (hh_distc) rather than only local type-checking *)
  enable_fanout_aware_distc: bool;
      (** use fanout-aware remote type-checking (hh_distc) *)
  hh_distc_fanout_threshold: int;
      (** fanout threshold where we trigger hh_distc *)
  hh_distc_fanout_full_init_threshold: int;
      (** fanout threshold above which full-init hh_distc is triggered *)
  hh_distc_exponential_backoff_num_retries: int;
  ide_file_watcher_enabled: bool;
      (** Enable the IDE daemon's built-in watcher in addition to LSP file-change notifications. *)
  ide_file_watcher_max_changed_files: int;
      (** Maximum number of relevant paths in a commit transition to process incrementally. *)
  ide_load_naming_table_on_disk: bool;
      (** POC: @nzthomas - allow ClientIdeDaemon to grab any naming table from disk before trying Watchman / Manifold *)
  ide_naming_table_update_threshold: int;
      (** POC: @nzthomas, if clientIDEDaemon is loading a naming table from disk instead of Manifold, set a globalrev distance threshold *)
  dump_tast_hashes: bool;
      (** Dump tast hashes into /tmp/hh_server/tast_hashes *)
  dump_tasts: string list;
      (** List of files whose TASTs to be dumped in /tmp/hh_server/tasts. *)
  use_compressed_dep_graph: bool;
      (** POC: @bobren, use new fancy compressed dep graph that is 25% the size of the old one *)
  hack_warnings: bool;  (** POC: @catg - turn on hack warnings. *)
  warnings_default_all: bool;
      (** If true, `hh` is equivalent to `hh -Wall`, i.e. warnings are shown.
        Otherwise, `hh` is equivalent to `hh -Wnone`, i.e. warnings are not shown. *)
  warnings_in_sandcastle: bool;
  package_config_strict_validation: bool;
      (** POC: @fzn, if true, `hh` fatals if paths specified in PACAKGES.toml `include_paths` do not exists *)
  (* Fields below are primarily for hh_conf equivalence testing *)
  sharedmem_global_size: int;  (** sharedmem_global_size config key *)
  sharedmem_hash_table_pow: int;  (** sharedmem_hash_table_pow config key *)
  sharedmem_heap_size: int;  (** sharedmem_heap_size config key *)
  eden_fetch_parallelism: int;  (** eden_fetch_parallelism config key *)
  use_distc_crawl_dircache: bool;  (** use_distc_crawl_dircache config key *)
  gc_minor_heap_size: int;  (** gc_minor_heap_size config key *)
  gc_space_overhead: int;  (** gc_space_overhead config key *)
  ide_fall_back_to_full_index: bool;
      (** ide_fall_back_to_full_index config key *)
  naming_table_compression_level: int;
      (** naming_table_compression_level config key *)
  naming_table_compression_threads: int;
      (** naming_table_compression_threads config key *)
  config_version: string option;
      (** version config key - renamed to avoid shadowing *)
  ignored_paths: string list;  (** ignored_paths config key, parsed from JSON *)
}
