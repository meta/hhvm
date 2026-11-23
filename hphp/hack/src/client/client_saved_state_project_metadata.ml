(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

(** This module executes the CLI command `hh saved-state-project-metadata` which
  computes and prints the project metadata for querying saved states. *)

exception GetProjectMetadataError of string

let main (env : Client_env.client_check_env) (config : Server_local_config.t) :
    Exit_status.t Lwt.t =
  (* Command `hh saved-state-project-metadata` can accept the same flags as `hh`
     (even though most flags will be ignored),
     which is why we take a ClientEnv.client_check_env as parameter.
     The project metadata for querying saved state depends on:
     - the current binary version
     - the content of .hhconfig
     - saved state flags from Saved_state_rollouts *)
  let {
    Client_env.root;
    ignore_hh_version;
    saved_state_ignore_hhconfig = _;
    config = _;
    autostart = _;
    custom_hhi_path = _;
    custom_telemetry_data = _;
    error_format = _;
    force_dormant_start = _;
    force_streaming_diagnostics = _;
    from = _;
    show_spinner = _;
    paths = _;
    max_errors = _;
    preexisting_warnings = _;
    reason = _;
    mode = _;
    no_load = _;
    save_64bit = _;
    save_human_readable_64bit_dep_map = _;
    output_json = _;
    output_jsonl = _;
    prechecked = _;
    mini_state = _;
    sort_results = _;
    stdin_name = _;
    deadline = _;
    watchman_debug_logging = _;
    allow_non_opt_build = _;
    desc = _;
    is_interactive = _;
    warning_switches = _;
    dump_config = _;
  } =
    env
  in
  let%lwt result =
    State_loader_lwt.get_project_metadata
      ~repo:root
      ~ignore_hh_version
      ~opts:config.Server_local_config.saved_state
  in
  match result with
  | Error (error, _telemetry) ->
    raise
      (GetProjectMetadataError
         (Saved_state_loader.LoadError.debug_details_of_error error))
  | Ok (project_metadata, _telemetry) ->
    Printf.printf "%s\n%!" (Option.value project_metadata ~default:"");
    Lwt.return Exit_status.No_error
