(*
 * Copyright (c) 2015, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude

(** What is the path to the hh_server binary?
1. If HH_SERVER_PATH environment variable is defined, use this (even if it doesn't exist on disk!)
2. Otherwise, if "dirname(realpath(executable_name))/hh_server" exists then use this
3. Otherwise, use unqualified "hh_server" (hence search on PATH).
The third case is currently what happens for "hh switch buck2" *)
let get_hhserver_path () =
  match Sys.getenv_opt "HH_SERVER_PATH" with
  | Some p ->
    Hh_logger.log "For hh_server path, using HH_SERVER_PATH=%s" p;
    p
  | None ->
    let exe_name = "hh_server" in
    (* Path.executable_name is an alias for Sys.executable_name. Its documentation is quite vague:
       "This name may be absolute or relative to the current directory, depending on the platform
       and whether the program was compiled to bytecode or a native executable." In my testing
       on native ocaml binaries on CentOS, it produces the fully-qualified realpath of the executable,
       i.e. it digs through symlinks. *)
    let server_next_to_client =
      Path.concat (Path.dirname Path.executable_name) exe_name |> Path.to_string
    in
    if Sys.file_exists server_next_to_client then begin
      Hh_logger.log
        "For hh_server path, found it adjacent to %s"
        (Path.executable_name |> Path.to_string);
      server_next_to_client
    end else begin
      Hh_logger.log
        "For hh_server path, will do unqualified search for %s"
        exe_name;
      exe_name
    end

type env = {
  root: Path.t;
  from: string;
  no_load: bool;
  watchman_debug_logging: bool;
  silent: bool;
  exit_on_failure: bool;
  ignore_hh_version: bool;
  save_64bit: string option;
  save_human_readable_64bit_dep_map: string option;
  saved_state_ignore_hhconfig: bool;
  prechecked: bool option;
  mini_state: string option;
  config: (string * string) list;
  custom_hhi_path: string option;
  custom_telemetry_data: (string * string) list;
  allow_non_opt_build: bool;
  preexisting_warnings: bool;
}

(* Sometimes systemd-run is available but we can't use it. For example, the
 * systemd might not have a proper working user session, so we might not be
 * able to run commands via systemd-run as a user process *)
let can_run_systemd () =
  (* Verify systemd-run is in the path *)
  let systemd_binary =
    try
      Unix.open_process_in "which systemd-run 2> /dev/null"
      |> In_channel.input_line
    with
    | _ -> None
  in
  if is_none systemd_binary then
    false
  else
    (* Use `timeout` in case it hangs mysteriously.
     * `--quiet` only suppresses stdout. *)
    let ic =
      Unix.open_process_in
        "timeout 1 systemd-run --scope --quiet --user -- true 2> /dev/null"
    in
    (* If all goes right, `systemd-run` will return immediately with exit code 0
     * and run `true` asynchronously as a service. If it goes wrong, it will exit
     * with a non-zero exit code *)
    match Unix.close_process_in ic with
    | Unix.WEXITED 0 -> true
    | _ -> false

let start_server (env : env) =
  let {
    root;
    from;
    no_load;
    watchman_debug_logging;
    silent;
    exit_on_failure;
    ignore_hh_version;
    save_64bit;
    save_human_readable_64bit_dep_map;
    saved_state_ignore_hhconfig;
    prechecked;
    mini_state;
    config;
    custom_hhi_path;
    custom_telemetry_data;
    allow_non_opt_build;
    preexisting_warnings;
  } =
    env
  in
  (* Create a pipe for synchronization with the server: we will wait
     until the server finishes its initialisation phase. *)
  let (in_fd, out_fd) = Unix.pipe () in
  Unix.set_close_on_exec in_fd;
  let ic = Unix.in_channel_of_descr in_fd in
  let serialize_key_value_options
      (option : string) (keyvalues : (string * string) list) =
    keyvalues
    |> List.map ~f:(fun (key, value) ->
           [| option; Printf.sprintf "%s=%s" key value |])
    |> Array.concat
  in
  let hh_server = get_hhserver_path () in
  let hh_server_args =
    Array.concat
      [
        [| hh_server; "-d"; Path.to_string root |];
        (if String.equal from "" then
          [||]
        else
          [| "--from"; from |]);
        (match mini_state with
        | None -> [||]
        | Some state -> [| "--with-mini-state"; state |]);
        (if no_load then
          [| "--no-load" |]
        else
          [||]);
        (if watchman_debug_logging then
          [| "--watchman-debug-logging" |]
        else
          [||]);
        (* If the client starts up a server monitor process, the output of that
         * bootup is passed to this FD - so this FD needs to be threaded
         * through the server monitor process then to the typechecker process.
         *
         * Note: Yes, the FD is available in the monitor process as well, but
         * it doesn't, and shouldn't, use it. *)
        [|
          "--waiting-client"; string_of_int (Core_unix.File_descr.to_int out_fd);
        |];
        (if ignore_hh_version then
          [| "--ignore-hh-version" |]
        else
          [||]);
        (if saved_state_ignore_hhconfig then
          [| "--saved-state-ignore-hhconfig" |]
        else
          [||]);
        (match save_64bit with
        | None -> [||]
        | Some dest -> [| "--save-64bit"; dest |]);
        (match save_human_readable_64bit_dep_map with
        | None -> [||]
        | Some dest -> [| "--save-human-readable-64bit-dep-map"; dest |]);
        [||];
        (match prechecked with
        | Some true -> [| "--prechecked" |]
        | _ -> [||]);
        (match prechecked with
        | Some false -> [| "--no-prechecked" |]
        | _ -> [||]);
        serialize_key_value_options "--config" config;
        (match custom_hhi_path with
        | None -> [||]
        | Some dest -> [| "--custom-hhi-path"; dest |]);
        serialize_key_value_options
          "--custom-telemetry-data"
          custom_telemetry_data;
        (if allow_non_opt_build then
          [| "--allow-non-opt-build" |]
        else
          [||]);
        (if preexisting_warnings then
          [| "--preexisting-warnings" |]
        else
          [||]);
      ]
  in
  let (stdin, stdout, stderr) =
    if silent then
      let nfd = Unix.openfile Sys_utils.null_path [Unix.O_RDWR] 0 in
      (nfd, nfd, nfd)
    else
      Unix.(stdin, stdout, stderr)
  in
  try
    let (exe, args) =
      if can_run_systemd () then
        (* launch command
         * systemd-run    (creates a transient cgroup)
         *    --scope     (allows synchronous execution of hh_server)
         *    --user      (specifies this to be a user instance)
         *    --quiet     (suppresses output to stdout)
         *    --slice=hack.slice   (puts created units under hack.slice)
         *    hh_server <hh_server args>
         *)
        let systemd_exe = "systemd-run" in
        let systemd_args =
          [|
            systemd_exe; "--scope"; "--user"; "--quiet"; "--slice=hack.slice";
          |]
        in
        (systemd_exe, Array.concat [systemd_args; hh_server_args])
      else
        (hh_server, hh_server_args)
    in
    if not silent then
      Printf.eprintf
        "Server launched with the following command:\n\t%s\n%!"
        (String.concat
           ~sep:" "
           (Array.to_list (Array.map ~f:Filename.quote args)));
    let server_pid = Unix.create_process exe args stdin stdout stderr in
    Unix.close out_fd;

    match Sys_utils.waitpid_non_intr [] server_pid with
    | (_, Unix.WEXITED 0) ->
      assert (String.equal (Stdlib.input_line ic) Monitor_utils.ready);
      Stdlib.close_in ic
    | (_, Unix.WEXITED i) ->
      if not silent then
        Printf.eprintf
          "Starting hh_server failed. Exited with status code: %d!\n"
          i;
      if exit_on_failure then Exit.exit Exit_status.Server_already_exists
    | _ ->
      if not silent then Printf.eprintf "Could not start hh_server!\n";
      if exit_on_failure then Exit.exit Exit_status.Server_already_exists
  with
  | _ ->
    if not silent then Printf.eprintf "Could not start hh_server!\n";
    if exit_on_failure then Exit.exit Exit_status.Server_already_exists

let should_start env =
  let root_s = Path.to_string env.root in
  let handoff_options =
    Monitor_rpc.
      {
        force_dormant_start = false;
        pipe_type = Monitor_rpc.Default;
        agent_session_id = None;
      }
  in
  let tracker = Connection_tracker.create () in
  Hh_logger.log
    "[%s] ClientStart.should_start"
    (Connection_tracker.log_id tracker);
  match
    Monitor_connection.connect_once
      ~tracker
      ~timeout:3
      ~terminate_monitor_on_version_mismatch:true
      env.root
      handoff_options
  with
  | Ok _conn -> false
  | Error
      Monitor_utils.(Connect_to_monitor_failure { server_exists = false; _ })
  | Error (Monitor_utils.Build_id_mismatched_monitor_will_terminate _)
  | Error Monitor_utils.Server_died ->
    true
  | Error (Monitor_utils.Build_id_mismatched_client_must_terminate _) ->
    Hack_event_logger.invariant_violation_bug
      "we requested terminate_monitor_on_version_mismatch";
    Printf.eprintf "Internal error. Please try `hh stop`\n";
    false
  | Error Monitor_utils.Server_dormant
  | Error Monitor_utils.Server_dormant_out_of_retries ->
    Printf.eprintf "Server already exists but is dormant";
    false
  | Error Monitor_utils.(Connect_to_monitor_failure { server_exists = true; _ })
    ->
    Printf.eprintf "Replacing unresponsive server for %s\n%!" root_s;
    Client_stop.kill_server env.root env.from;
    true

let main (env : env) : Exit_status.t Lwt.t =
  Hack_event_logger.client_start ();

  (* TODO(ljw): There are some race conditions here. First scenario: two      *)
  (* processes simultaneously do 'hh start' while the server isn't running.   *)
  (* Both their calls to should_start will see the lockfile absent and        *)
  (* immediately get back 'Server_missing'. So both of them launch hh_server. *)
  (* One hh_server process will be slightly faster and will create a lockfile *)
  (* and shortly start listening on the socket. The other will see that the   *)
  (* lockfile already exists and so terminate with error code 0 immediately   *)
  (* without sending the "ready" message. And the second start_server call    *)
  (* will fail its assert that a "ready" message should come.                 *)
  (*                                                                          *)
  (* Second scenario: one process does 'hh start', and creates the lockfile,  *)
  (* and after a short delay will start listening on its socket. Another      *)
  (* process does 'hh start', sees the lockfile is present, tries to connect, *)
  (* but we're still in above short delay and so it gets ECONNREFUSED         *)
  (* immediately, which (given the presence of a lockfile) is returned as     *)
  (* Monitor_not_ready. The should_start routine deems this an unresponsive   *)
  (* server and so kills it and launches a new server again. The first        *)
  (* process might or might not report failure, depending on how far it got   *)
  (* with its 'hh start' -- i.e. whether or not it yet observed the "ready".  *)
  (*                                                                          *)
  (* Third and most typical scenario: an LSP client like Nuclide is running,  *)
  (* and the user does 'hh restart' which kills the server and then calls     *)
  (* start_server. As soon as LSP sees the server killed, it too immediately  *)
  (* calls ClientStart.main. Maybe it will see the lockfile absent, and so    *)
  (* proceed according to the first race scenario above. Maybe it will see    *)
  (* the lockfile present and proceed according to the second race scenario   *)
  (* above. In both cases the LSP client will see problem reports.            *)
  (*                                                                          *)
  (* The root problem is that should_start assumes an invariant that "if      *)
     (* hh_server monitor is busy then it has failed and should be shut down." *)
  (* This invariant isn't true. Note that the reason we call should_start,    *)
  (* rather than just using the default Client_connect.autorestart which calls *)
  (* into Client_start.start_server, is because we do like its ability to kill *)
  (* an unresponsive server. So the should_start function should simply give  *)
  (* a grace period in case of a temporarily busy server.                     *)
  (*                                                                          *)
  (* I believe there's a second similar problem inside Client_connect.connect  *)
  (* when its env.autorestart is true. During the short delay window it might *)
  (* immediately get Server_missing similar to the first scenario, and so     *)
  (* Client_start.start_server, which will fail in the same way. Or it might   *)
  (* immediately get ECONNREFUSED during the delay window, so it will retry   *)
  (* connection attempt immediately, and it might burn through all of its     *)
  (* available retries instantly. That's because ECONNREFUSED is immediate.   *)
  if should_start env then (
    start_server env;
    Lwt.return Exit_status.No_error
  ) else (
    if not env.silent then
      Printf.eprintf
        "Error: Server already exists for %s\nUse hh_client restart if you want to kill it and start a new one\n%!"
        (Path.to_string env.root);
    Lwt.return Exit_status.Server_already_exists
  )
