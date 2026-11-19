(*
 * Copyright (c) 2019, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

type t =
  | No_error
  | Input_error
  | Kill_error
  | No_server_running_should_retry
  | Server_hung_up_should_retry of finale_data option
  | Server_hung_up_should_abort of finale_data option
  | Out_of_time
  | Out_of_retries
  | Server_already_exists
  | Type_error
  | Build_id_mismatch
  | Monitor_connection_failure
  | Unused_server
  | Lock_stolen
  | Lost_parent_monitor
  | Server_got_eof_from_monitor
  | Interrupted
      (** hh_client installs a SIGINT handler which raises [Exit_with Interrupted].
       Also, MonitorMain installs SIGINT, SIGQUIT, SIGTERM, SIGHUP handlers which
       call [Exit.exit Interrupted]. *)
  | Client_broken_pipe
      (** hh_client code tried to write to stdout, but was thwarted by Sys_error("Broken pipe") *)
  | Worker_oomed
  | Worker_busy
  | Worker_not_found_exception
  | Worker_failed_to_send_job
  | Socket_error
  | Missing_hhi
  | Dfind_died
  | Dfind_unresponsive
  | EventLogger_Timeout
  | EventLogger_restart_out_of_retries
  | EventLogger_broken_pipe
  | CantRunAI
  | Watchman_failed
  | Watchman_fresh_instance
  | Watchman_invalid_result
  | Edenfs_watcher_failed
  | Edenfs_watcher_eden_shutting_down
  | Edenfs_watcher_lost_changes
  | Distc_failed
  | File_provider_stale
  | Depgraph_unavailable
  | Hhconfig_deleted
  | Hhconfig_changed
  | Package_config_changed
  | Typecheck_restarted
      (** an exit status of hh_client check, e.g. because files-on-disk changed *)
  | Typecheck_abandoned
      (** an exit status of hh_client check, e.g. because the server was killed mid-check *)
  | Server_shutting_down_due_to_sigusr2
  | IDE_malformed_request
  | IDE_no_server
  | IDE_out_of_retries
  | Nfs_root
  | IDE_init_failure
  | IDE_typechecker_died
  | Redecl_heap_overflow
  | Out_of_shared_memory
  | Shared_mem_assertion_failure
  | Hash_table_full
  | IDE_new_client_connected
  | IDE_select_error
  | Lazy_decl_bug
  | Decl_heap_elems_bug
  | Parser_heap_build_error
  | Heap_full
  | Sql_assertion_failure
  | Local_type_env_stale
  | Sql_cantopen
  | Sql_corrupt
  | Sql_misuse
  | Uncaught_exception of Exception.t
  | Decl_not_found
  | Big_rebase_detected
  | Failed_to_load_should_retry
  | Failed_to_load_should_abort
  | Server_non_opt_build_mode
  | Not_restarting_server_with_precomputed_saved_state
  | Config_error
  | Client_bad_args
      (** hh_client command-line arguments could not be parsed or its root was invalid. *)
[@@deriving show]

and finale_data = {
  exit_status: t;
      (** exit_status is shown to the user in CLI and LSP,
      just so they have an error code they can quote back at
      developers. It's also used by hh_client to decide, on the
      basis of that hh_server exit_status, whether to auto-restart
      hh_server or not. And it appears in logs and telemetry. *)
  msg: string option;
      (** msg is a human-readable message for the end-user to explain why
      hh_server stopped. It appears in the CLI, and in LSP in a
      hover tooltip. It also is copied into the logs. *)
  stack: Utils.callstack;
  telemetry: Telemetry.t option;
      (** telemetry is unstructured data, for logging, not shown to users *)
}

exception Exit_with of t

val exit_code : t -> int

val exit_code_to_string : int -> string

val unpack : Unix.process_status -> string * int

(** If the server dies through a controlled exit, it leaves behind a "finale file" <pid>.fin
with json-formatted data describing the detailed nature of the exit including callstack.
This method retrieves that file, if it exists. *)
val get_finale_data : string -> finale_data option

(** like [show], but prints a lot more information including callstacks *)
val show_expanded : t -> string
