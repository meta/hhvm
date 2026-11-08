(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude

module PerFileProfilingConfig = struct
  type profile_decling =
    | DeclingOff
    | DeclingTopCounts
    | DeclingAllTelemetry of { callstacks: bool }

  module ProfileDecling = struct
    type t = profile_decling

    let of_config_value (s : string) : t option =
      match s with
      | "off" -> Some DeclingOff
      | "top_counts" -> Some DeclingTopCounts
      | "all_telemetry" -> Some (DeclingAllTelemetry { callstacks = false })
      | "all_telemetry_callstacks" ->
        Some (DeclingAllTelemetry { callstacks = true })
      | _ -> None

    let to_config_value (t : t) : string =
      match t with
      | DeclingOff -> "off"
      | DeclingTopCounts -> "top_counts"
      | DeclingAllTelemetry { callstacks } ->
        Printf.sprintf
          "all_telemetry%s"
          (if callstacks then
            "_callstacks"
          else
            "")

    let of_int (i : int) : t =
      match i with
      | 0 -> DeclingOff
      | 1 -> DeclingTopCounts
      | 2 -> DeclingAllTelemetry { callstacks = false }
      | _ -> DeclingAllTelemetry { callstacks = true }

    let config_value_of_int (i : int) : string = i |> of_int |> to_config_value
  end

  type t = {
    profile_log: bool;
    profile_type_check_duration_threshold: float;
    profile_type_check_memory_threshold_mb: int;
    profile_type_check_twice: bool;
    profile_decling: profile_decling;
    profile_owner: string option;
    profile_desc: string option;
    profile_slow_threshold: float;
  }

  let default =
    {
      profile_log = false;
      profile_type_check_duration_threshold = 0.05 (* seconds *);
      profile_type_check_memory_threshold_mb = 100;
      profile_type_check_twice = false;
      profile_decling = DeclingOff;
      profile_owner = None;
      profile_desc = None;
      profile_slow_threshold = Float.max_value;
    }
end

type rollout_flags = {
  saved_state_lookup_use_eden: bool;
      (** Whether to use Eden rather than Watchman to look up saved states. *)
  fetch_remote_old_decls: bool;
      (** POC: @bobren - Fetching old decls remotely to reduce fanout *)
  only_fetch_remote_old_decl_during_init: bool;
      (** POC: @mojpc2, whether remote old decl fetches are restricted to init *)
  disable_rust_provider_shallow_decl_invalidation: bool;
      (** POC: @mojpc2, preserve Rust shallow decls when invalidating folded decls *)
  enable_annotation_agnostic_decl_diffing: bool;
      (** Whether user-defined class attributes are ignored when computing fanout. *)
  specify_manifold_api_key: bool;
      (** POC: @nzthomas, whether the API key in hh.conf should be used for saved state downloads *)
  populate_member_heaps: bool;
      (** POC: @hverr, disable writing signatures to method/property heaps. *)
  shm_use_sharded_hashtbl: bool;  (** POC: @hverr, new shared memory backend. *)
  shm_cache_size: int;
      (** POC: @hverr, evictability cache size, -1 if evictability disabled *)
  use_manifold_cython_client: bool;
      (** POC: @nzthomas, Required for Hedwig support for saved state downloads *)
  rust_provider_backend: bool;
      (** POC: @jakebailey, started Dec 2022.
          Use Provider_backend.Rust_provider_backend as the global provider
          backend, servicing File_provider, Naming_provider, and Decl_provider
          queries using the hackrs implementation. *)
  set_global_provider_backend_in_workers: bool;
      (** This just exists to safely roll out a fix: If true, we transfer the current value of
       *  Provider_backend.get () over from the main process when launching workers *)
  use_distc: bool;
      (** POC: @bobren, use hh_distc instead of hulk for remote typechecking *)
  enable_fanout_aware_distc: bool;
      (** POC: @fzn, use fanout-aware hh_distc if hh_distc_fanout_threshold <= fanout < hh_distc_fanout_full_init_threshold *)
  consume_streaming_errors: bool;
      (** POC: @ljw, hh_client should read errors from errors.bin *)
  status_single_use_cached_diagnostics: bool;
      (** Whether hh --single may reuse cached server diagnostics. *)
  hh_distc_fanout_threshold: int;
      (** POC: @bobren - fanout threshold where we trigger hh_distc *)
  hh_distc_fanout_full_init_threshold: int;
      (** POC: @fzn - fanout threshold where we trigger fanout-aware hh_distc *)
  ide_file_watcher_enabled: bool;
      (** POC: @frankemrich - If true, watch commit transitions with Edenfs_watcher in the IDE daemon *)
  ide_load_naming_table_on_disk: bool;
      (** POC: @nzthomas - allow ClientIdeDaemon to grab any naming table from disk before trying Watchman / Manifold *)
  ide_naming_table_update_threshold: int;
      (** POC: @nzthomas, if clientIDEDaemon is loading a naming table from disk instead of Manifold, set a globalrev distance threshold *)
  use_compressed_dep_graph: bool;
      (** POC: @bobren, use new fancy compressed dep graph that is 25% the size of the old one *)
  load_state_natively_v4: bool;
      (** POC: @bobren, whether to allow hh_server to fetch saved state (thus overriding load_state_natively setting on Sandcastle) *)
  saved_state_rollouts: Saved_state_rollouts.t;
  zstd_decompress_by_file: bool;
      (** POC: @nzthomas, if true then saved state loading will only pass specific files to zstd shellout *)
  warnings_default_all: bool;
      (** POC: @catg - whether the client shows warnings by default. *)
  watchman_enabled: bool;
      (** Whether Watchman is enabled for Server_notifier after config overrides. *)
  edenfs_file_watcher_enabled: bool;
      (** POC: @frankemrich - if true, use Edenfs_watcher to power ServerNotifier (= file watching in server) *)
  edenfs_file_watcher_sync_queries_obey_deferral: bool;
      (** POC: @frankemrich -  If false, ServerNotifier.get_changes_sync backed by Edenfs_watcher will return
          file changes even when we are currently deferring changes (e.g. because meerkat is running). *)
  edenfs_file_watcher_state_tracking: bool;
      (** POC: @frankemrich - Whether to use Eden's state tracking API *)
  edenfs_informant_enabled: bool;
      (** POC: @frankemrich - If true, use Eden backend for InformantNotifier (= file watching in monitor process) *)
  eden_prefetch_enabled: bool;
      (** POC: @frankemrich - If true, prefetch type-checking fanout files with Sapling *)
  eden_prefetch_fanout_threshold: int;
      (** POC: @frankemrich - Minimum number of fanout files required to start Sapling prefetching. *)
  prefetch_decls_enabled: bool;
      (** POC: @izorin - If true, prefetch class declarations in parallel before type-checking files with many class references *)
}
