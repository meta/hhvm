(*
 * Copyright (c) 2015, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude
open Option.Monad_infix
open Ppx_yojson_conv_lib.Yojson_conv.Primitives

(*****************************************************************************)
(* Recheck loop types. *)
(*****************************************************************************)
type seconds = float

type seconds_since_epoch = float

module Rust_disk_provider_read_telemetry = struct
  type t = {
    count: int;
    seconds: float;
  }
  [@@deriving show]

  let get_totals () =
    let record = Measure.peek_global () in
    let count =
      Measure.get_count ~record "rust_disk_provider_read_seconds"
      |> Option.value ~default:0.
      |> Int.of_float
    in
    let seconds =
      Measure.get_sum ~record "rust_disk_provider_read_seconds"
      |> Option.value ~default:0.
    in
    { count; seconds }

  let delta ~start ~finish =
    {
      count = finish.count - start.count;
      seconds = finish.seconds -. start.seconds;
    }

  let to_telemetry { count; seconds } =
    Telemetry.create ()
    |> Telemetry.int_ ~key:"count" ~value:count
    |> Telemetry.float_ ~key:"duration_ms" ~value:(seconds *. 1000.)

  let get_delta_telemetry ~start =
    delta ~start ~finish:(get_totals ()) |> to_telemetry
end

module RecheckLoopStats = struct
  type t = {
    updates_stale: bool;
        (** File watcher subscription has gone down, so state of the world after the
          recheck loop may not reflect what is actually on disk. *)
    per_batch_telemetry: Telemetry.t list;
    total_changed_files_count: int;
        (** Count of files changed on disk and potentially also in the IDE. *)
    total_rechecked_count: int;
    last_iteration_start_time: seconds_since_epoch;
    duration: seconds;
    rust_disk_provider_read: Rust_disk_provider_read_telemetry.t;
    time_first_result: seconds_since_epoch option;
    recheck_id: string;
    any_full_checks: bool;
  }

  let empty ~(recheck_id : string) : t =
    {
      updates_stale = false;
      per_batch_telemetry = [];
      total_changed_files_count = 0;
      total_rechecked_count = 0;
      last_iteration_start_time = 0.;
      duration = 0.;
      rust_disk_provider_read =
        { Rust_disk_provider_read_telemetry.count = 0; seconds = 0. };
      time_first_result = None;
      recheck_id;
      any_full_checks = false;
    }

  (** The format of this json is user-facing, returned from 'hh check --json'
      in field `last_recheck`. *)
  let to_user_telemetry (stats : t) : Telemetry.t =
    let {
      updates_stale;
      per_batch_telemetry;
      total_changed_files_count;
      total_rechecked_count;
      last_iteration_start_time;
      duration;
      rust_disk_provider_read;
      time_first_result;
      recheck_id;
      any_full_checks;
    } =
      stats
    in
    Telemetry.create ()
    |> Telemetry.string_ ~key:"id" ~value:recheck_id
    |> Telemetry.float_ ~key:"time" ~value:duration
    |> Telemetry.object_
         ~key:"rust_disk_provider_read"
         ~value:
           (Rust_disk_provider_read_telemetry.to_telemetry
              rust_disk_provider_read)
    |> Telemetry.int_ ~key:"count" ~value:total_rechecked_count
    |> Telemetry.int_
         ~key:"total_changed_files_count"
         ~value:total_changed_files_count
    |> Telemetry.object_list
         ~key:"per_batch"
         ~value:(List.rev per_batch_telemetry)
    |> Telemetry.bool_ ~key:"updates_stale" ~value:updates_stale
    |> Telemetry.bool_ ~key:"any_full_checks" ~value:any_full_checks
    |> Telemetry.int_opt
         ~key:"time_to_first_result_ms"
         ~value:
           ( time_first_result >>| fun time ->
             int_of_float ((time -. last_iteration_start_time) *. 1000.) )

  (** Update field [time_first_result] if given timestamp is the
      first sent result. *)
  let record_result_sent_ts stats new_result_sent_ts =
    {
      stats with
      time_first_result =
        Option.first_some stats.time_first_result new_result_sent_ts;
    }
end

(*****************************************************************************)

(** The "static" environment, initialized first and then doesn't change *)
type genv = {
  options: Server_args.options;
  config: Server_config.t;
  local_config: Server_local_config.t;
  workers: Multi_worker.worker list option;
  notifier: Server_notifier.t;
  indexer: (string -> bool) -> unit -> string list;
      (** Returns the list of files under .hhconfig, subject to a filter *)
  mutable debug_channels: (Timeout.in_channel * Out_channel.t) option;
}

(*****************************************************************************)
(** The environment constantly maintained by the server *)

(*****************************************************************************)

type full_check_status =
  | Full_check_needed
      (** Some updates have not been fully processed. We get into this state every
          time file contents change (on disk, or through IDE notifications).
          Operations that depend on global state (like taking full error list, or
          looking up things in dependency table) will have stale results. *)
  | Full_check_started
      (** Same as above, except server will actively try to process outstanding
          changes (by going into Server_type_check from main loop - this might need to
          be repeated several times before progressing to Full_check_done, due to
          ability to interrupt typecheck jobs).
          Server starts in this state, and we also enter it from Full_check_needed
          whenever there is a command requiring full check pending, or when user
          saves a file. *)
  | Full_check_done  (** All the changes have been fully processed. *)
  | Full_check_paused
      (** Automatic checks are paused by [hh --pause] until the next accepted client connection.
          Invariants:
            - When transitioning to this state, [disk_needs_parsing] and [needs_recheck] are empty.
            - While paused, [needs_recheck] remains empty, but new file changes
              accumulate in [disk_needs_parsing]. *)
[@@deriving show]

let is_full_check_done = function
  | Full_check_done -> true
  | _ -> false

let is_full_check_needed = function
  | Full_check_needed -> true
  | _ -> false

let is_full_check_started = function
  | Full_check_started -> true
  | _ -> false

module Init_telemetry = struct
  type reason =
    | Init_lazy_dirty
    | Init_typecheck_disabled_after_init
    | Init_lazy_full
    | Init_prechecked_fanout
  [@@deriving show { with_path = false }]

  type t = {
    reason: reason;
    telemetry: Telemetry.t;
  }

  let make reason telemetry = { reason; telemetry }

  let get { reason; telemetry } =
    telemetry |> Telemetry.string_ ~key:"reason" ~value:(show_reason reason)

  let get_reason { reason; _ } = show_reason reason
end

(** How "old" a given saved state is relative to the working revision.
    That is, a saved state has some corresponding revision X, and `hh` is invoked on a commit
    of mergebase Y.

    Distance tracks the number of revisions between X and Y, using globalrev.
    Age tracks the time elapsed between X and Y in seconds, according to hg log data.
*)

type saved_state_revs_info = {
  distance: int option;
  age: int option;
  saved_state_rev: Hg.Rev.t;
  saved_state_globalrev: Hg.global_rev option;
  saved_state_rev_timestamp: int option;
  mergebase_rev: Hg.Rev.t option;
  mergebase_globalrev: Hg.global_rev option;
  mergebase_rev_timestamp: int option;
}
[@@deriving show, yojson]

let mergebase_has_saved_state { saved_state_rev; mergebase_rev; _ } =
  Option.map mergebase_rev ~f:(Hg.Rev.equal saved_state_rev)

module SavedStateRevsInfo = struct
  type t = saved_state_revs_info [@@deriving yojson]

  let default ~saved_state_rev =
    {
      distance = None;
      age = None;
      saved_state_rev;
      saved_state_globalrev = None;
      saved_state_rev_timestamp = None;
      mergebase_rev = None;
      mergebase_globalrev = None;
      mergebase_rev_timestamp = None;
    }

  let to_telemetry
      {
        distance;
        age;
        saved_state_rev;
        saved_state_globalrev;
        saved_state_rev_timestamp;
        mergebase_rev;
        mergebase_globalrev;
        mergebase_rev_timestamp;
      } =
    Telemetry.create ()
    |> Telemetry.int_opt ~key:"distance" ~value:distance
    |> Telemetry.int_opt ~key:"age_seconds" ~value:age
    |> Telemetry.string_
         ~key:"saved_state_rev"
         ~value:(Hg.Rev.to_string saved_state_rev)
    |> Telemetry.int_opt
         ~key:"saved_state_globalrev"
         ~value:saved_state_globalrev
    |> Telemetry.int_opt
         ~key:"saved_state_rev_timestamp"
         ~value:saved_state_rev_timestamp
    |> Telemetry.string_opt
         ~key:"mergebase_rev"
         ~value:(Option.map ~f:Hg.Rev.to_string mergebase_rev)
    |> Telemetry.int_opt ~key:"mergebase_globalrev" ~value:mergebase_globalrev
    |> Telemetry.int_opt
         ~key:"mergebase_rev_timestamp"
         ~value:mergebase_rev_timestamp
end

type dirty_deps = {
  dirty_local_deps: Typing_deps.DepSet.t;
      (** We are rechecking dirty files to bootstrap the dependency graph.
          After this is done we need to also recheck full fan-out (in this updated
          graph) of provided set. *)
  dirty_master_deps: Typing_deps.DepSet.t;
      (** The fan-outs of those nodes were not expanded yet. *)
  rechecked_files: Relative_path.Set.t;
      (** Files that have been rechecked since server startup *)
  clean_local_deps: Typing_deps.DepSet.t;
      (** Those deps have already been checked against their interaction with
          dirty_master_deps. Storing them here to avoid checking it over and over *)
}
[@@deriving show]

(** When using prechecked files we split initial typechecking in two phases
    (dirty files and a subset of their fan-out). Other init types compute the
    full fan-out up-front. *)
type prechecked_files_status =
  | Prechecked_files_disabled
  | Initial_typechecking of dirty_deps
  | Prechecked_files_ready of dirty_deps
[@@deriving show]

type init_env = {
  approach_name: string;
  ci_info: Ci_util.info option Future.t option; [@opaque]
      (** The info describing the CI job the server is a part of, if any *)
  init_error: string option;
  init_id: string;
  init_start_t: float;
  init_type: string;
  mergebase_warning_hashes: Warnings_saved_state.t option;
  why_needed_full_check: Init_telemetry.t option; [@opaque]
      (** This is about the first full check (if any) which was deferred after init.
      It gets reset after that first full check is completed.
      First parameter is a string label used in telemetry. Second is opaque telemetry. *)
  recheck_id: string option;
  (* Additional data associated with init that we want to log when a first full
   * check completes. *)
  saved_state_revs_info: saved_state_revs_info option;
  naming_table_manifold_path: string option;
      (** The manifold path for remote typechecker workers to download the naming table
          saved state. This value will be None in the case of full init *)
}
[@@deriving show]

type type_check_testing_hooks = {
  after_indexing: unit -> unit;
  before_file_checks: Relative_path.t list -> unit;
  after_file_check: Relative_path.t -> unit;
}

(** In addition to this environment, many functions are storing and
    updating ASTs, NASTs, and types in a shared space
    (see respectively Parser_heap, Naming_table, Typing_env).
    The Ast.id are keys to index this shared space. *)
type env = {
  naming_table: Naming_table.t;
  deps_mode: Typing_deps_mode.t; [@opaque]
  tcopt: Typechecker_options.t;
  popt: Parser_options.t;
  gleanopt: Glean_options.t;
  swriteopt: Symbol_write_options.t;
  diagnostics: Diagnostics.t; [@opaque]
      (** Diagnostics are indexed by files that were known to GENERATE diagnostics in
          corresponding phases. Note that this is different from HAVING diagnostics -
          it's possible for checking of A to generate a diagnostic in B - in this case
          Diagnostics.get_failed_files Typing should contain A, not B.
          Conversly, if declaring A will require declaring B, we should put
          B in failed decl. Same if checking A will cause declaring B (via lazy
          decl).

          During recheck, we add those files to the set of files to reanalyze
          at each stage in order to regenerate their diagnostic lists. So those
          failed_ sets are the main piece of mutable state that incremental mode
          needs to maintain - the diagnostics themselves are more of a cache, and should
          always be possible to be regenerated based on those sets. *)
  failed_naming: Relative_path.Set.t;
      (** failed_naming is used as kind of a dependency tracking mechanism:
          if files A.php and B.php both define class C, then those files are
          mutually depending on each other (edit to one might resolve naming
          ambiguity and change the interpretation of the other). Both of those
          files being inside failed_naming is how we track the need to
          check for this condition.

          See test_naming_errors.ml and test_failed_naming.ml *)
  last_command_time: float;
      (** Timestamp of last IDE file synchronization command *)
  last_notifier_check_time: float;
      (** Timestamp of last query for disk changes *)
  last_idle_job_time: float;  (** Timestamp of last Server_idle.go run *)
  disk_needs_parsing: Relative_path.Set.t;
      (** Files which parse trees were invalidated (because they changed on disk)
          and need to be re-parsed *)
  clock: Server_notifier.clock option;
      (** This is the clock as of when disk_needs_parsing was last updated.
      None if not using Watchman or Edenfs_watcher. *)
  needs_phase2_redecl: Relative_path.Set.t;
      (** Declarations that became invalidated and moved to "old" part of the heap.
          We keep them there to be used in "determining changes" step of recheck.
          (when they are compared to "new" versions). Depending on lazy decl to
          compute "new" versions in all the other scenarios (like IDE queries) *)
  needs_recheck: Relative_path.Set.t;
      (** Files that need to be typechecked before commands that depend on global
          state (like full list of errors, build, or find all references) can be
          executed . After full check this should be empty, unless that check was
          cancelled mid-flight, in which case full_check_status will be set to
          Full_check_started and entire thing will be retried on next iteration. *)
  why_needs_server_type_check: string * string;
  init_env: init_env;
  full_check_status: full_check_status;
  prechecked_files: prechecked_files_status;
  changed_files: Relative_path.Set.t;
  interrupt_handlers:
    genv ->
    env ->
    (Unix.file_descr * env Multi_threaded_call.interrupt_handler) list;
  type_check_testing_hooks: type_check_testing_hooks option; [@opaque]
  nonpersistent_client_pending_command_needs_full_check:
    ((env -> env) * string * Client_provider.client) option;
      [@opaque]
      (** When a non-persistent client sends a command that cannot be immediately handled
          (due to needing full check) we put the continuation that finishes handling
          it here. The string specifies a reason why this command needs full
          recheck (for logging/debugging purposes) *)
  last_recheck_loop_stats: RecheckLoopStats.t; [@opaque]
  last_recheck_loop_stats_for_actual_work: RecheckLoopStats.t option; [@opaque]
}
[@@deriving show]

let discard_warnings env =
  if Sandcastle.is_sandcastle () then
    not env.tcopt.Global_options.warnings_in_sandcastle
  else
    let open Option.Monad_infix in
    env.init_env.saved_state_revs_info
    >>= mergebase_has_saved_state
    |> Option.value ~default:false

let list_files_with_errors env =
  let acc =
    List.fold_right
      ~f:
        begin
          fun { User_diagnostic.claim = (pos, _); severity; _ } (acc : S_set.t) ->
            match severity with
            | User_diagnostic.Err ->
              S_set.add (Relative_path.to_absolute (Pos.filename pos)) acc
            | User_diagnostic.Warning _ -> acc
        end
      ~init:S_set.empty
      (Diagnostics.get_diagnostic_list env.diagnostics)
  in
  S_set.elements acc

let prechecked_files_ready_for_diagnostics = function
  | Prechecked_files_disabled -> true
  | Initial_typechecking _ -> false
  | Prechecked_files_ready dirty_deps ->
    Typing_deps.DepSet.is_empty dirty_deps.dirty_master_deps

let are_diagnostics_complete env ~uses_partial_typecheck =
  (not uses_partial_typecheck)
  && is_full_check_done env.full_check_status
  && Relative_path.Set.is_empty env.disk_needs_parsing
  && Relative_path.Set.is_empty env.needs_phase2_redecl
  && Relative_path.Set.is_empty env.needs_recheck
  && prechecked_files_ready_for_diagnostics env.prechecked_files

let add_changed_files env changed_files =
  {
    env with
    changed_files = Relative_path.Set.union env.changed_files changed_files;
  }

let show_clock (clock : Server_notifier.clock option) : string =
  Option.value_map ~f:Server_notifier.show_clock ~default:"[noclock]" clock
