(*
 * Copyright (c) 2015, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

(**
 * Hack for HipHop: type checker's client code.
 *
 * This code gets called in various different ways:
 * - from emacs, where the output is asynchronous
 * - from vim, where vim is blocked until this code exits
 * - from arc diff, our linter
 * - from arc land, our commit hook
 * - from check trunk, our irc bot which checks the state of trunk
 * - manually, from the command line
 *
 * Usage: hh_client [OPTION]... [WWW DIRECTORY] [FILE]...
 *)

open Hh_prelude

let () = Random.self_init ()

let init_event_logger
    root
    (command : Client_command.heavy_command)
    ~init_id
    ~from
    (config : Server_config.t)
    (local_config : Server_local_config.t) : unit =
  Hack_event_logger.client_init
    ~init_id
    ~from
    ~is_interactive:(Client_args.is_interactive command)
    ~custom_columns:(Client_command.get_custom_telemetry_data command)
    ?agent_session_id:(Agent_session_id.get ())
    root;
  Hack_event_logger.set_hhconfig_version
    (Server_config.version config |> Config_file.version_to_string_opt);
  Hack_event_logger.set_rollout_group
    local_config.Server_local_config.rollout_group;
  Hack_event_logger.set_rollout_flags
    (Server_local_config_load.to_rollout_flags local_config);
  ()

let log_qe_fetch (fetch : Server_local_config_qe.fetch) =
  Hack_event_logger.client_qe_fetch
    ~start_time:fetch.start_time
    ~end_time:fetch.end_time

let apply_agent_error_format
    (local_config : Server_local_config.t)
    (check_env : Client_env.client_check_env) : Client_env.client_check_env =
  match
    ( check_env.Client_env.error_format,
      local_config.Server_local_config.use_agent_error_format )
  with
  | (None, Server_local_config.Agent_error_format_disabled) ->
    Server_local_config_qe.enable_agent_error_format_exposure ();
    check_env
  | (None, Server_local_config.Agent_error_format_enabled) ->
    Server_local_config_qe.enable_agent_error_format_exposure ();
    { check_env with Client_env.error_format = Some Diagnostics.Agent }
  | _ -> check_env

let set_up_signals () =
  (* Ignore SIGPIPE since if it arises from clientConnect then it might indicate server hangup;
     we detect this case already and handle it better than a signal (unhandled signals cause program exit). *)
  Sys_utils.set_signal Sys.sigpipe Sys.Signal_ignore;
  Sys_utils.set_signal
    Sys.sigint
    (Sys.Signal_handle (fun _ -> raise Exit_status.(Exit_with Interrupted)));
  ()

let set_up_logger ~command_name ~init_id ~root =
  (* We'll chose where Hh_logger.log gets sent *)
  Hh_logger.Level.set_min_level_file Hh_logger.Level.Info;
  Hh_logger.Level.set_min_level_stderr Hh_logger.Level.Error;
  Hh_logger.set_id (Printf.sprintf "%s#%s" command_name init_id);
  let client_log_fn = Server_files.client_log root in
  try
    (* For irritating reasons T67177821 we might not have permissions
       to write to the file. Pending a fix, let's only set up Hh_logger
       to write to the file if we can indeed safely write to it. *)
    Sys_utils.touch
      (Sys_utils.Touch_existing_file_or_create_new
         { mkdir_if_new = false; perm_if_new = 0o666 })
      client_log_fn;
    Hh_logger.set_log client_log_fn
  with
  | _ -> ()

let set_up_root root =
  Relative_path.set_path_prefix Relative_path.Root root;
  Server_progress.set_root root;
  ()

let exit_status_of_exn exn =
  let e = Exception.wrap exn in
  let es =
    match exn with
    | Exit_status.Exit_with es -> es
    | _ -> Exit_status.Uncaught_exception e
  in
  (es, e)

let handle_exn_and_exit exn ~command_name =
  let (es, e) = exit_status_of_exn exn in
  (* hide the spinner *)
  Client_spinner.report ~to_stderr:false ~angery_reaccs_only:false None;
  (* We trust that if someone raised Exit_with then they had the decency to print
     out a user-facing message; we will only print out a user-facing message here
     for uncaught exceptions: lvl=Error gets sent to stderr, but lvl=Info doesn't. *)
  let lvl =
    match exn with
    | Exit_status.Exit_with _ -> Hh_logger.Level.Info
    | _ -> Hh_logger.Level.Error
  in
  Hh_logger.log
    ~lvl
    "CLIENT_BAD_EXIT [%s] %s"
    command_name
    (Exit_status.show_expanded es);
  Hack_event_logger.client_bad_exit ~command_name es e;
  Exit.exit es

let exec_command_without_config (command : Client_command.light_command) =
  try
    let exit_status =
      match command with
      | Client_command.CDecompressZhhdg env -> Client_decompress_zhhdg.main env
    in
    Exit.exit exit_status
  with
  | exn ->
    let (es, _e) = exit_status_of_exn exn in
    Printf.printf
      "[%s] %s\n"
      (Client_command.name_camel_case_light command)
      (Exit_status.show_expanded es);
    Exit.exit es

let exec_command_with_config
    (command : Client_command.heavy_command) ~init_proc_stack =
  let init_id = Random_id.short_string () in
  let command_name = Client_command.name_camel_case_heavy command in

  (* The global variable Relative_path.root must be initialized for a wide variety of things *)
  let root = Client_args.root command in
  set_up_root root;
  let from = Client_args.from command in

  set_up_logger ~command_name ~init_id ~root;
  Hh_logger.log
    "[hh_client] %s"
    (String.concat ~sep:" " (Array.to_list Sys.argv));

  let cli_config_overrides =
    Client_args.config command |> Option.value ~default:[]
  in
  Server_config.warn_on_invalid_config_keys cli_config_overrides;

  let qe_fetches = ref [] in
  let apply_qe_overrides ~silent config =
    let (config, fetches) =
      Server_local_config_qe.apply_qe_overrides ~silent config
    in
    qe_fetches := fetches;
    config
  in
  let (config, local_config) =
    Server_config.load_with_dynamic_overrides
      ~apply_dynamic_overrides:apply_qe_overrides
      ~silent:(not @@ Client_args.dump_config command)
      ~from
      ~cli_config_overrides
  in
  if local_config.Server_local_config.art_cli_lite_exit_status_enabled then
    Exit.add_hook_upon_clean_exit (fun { Exit_status.exit_status; _ } ->
        Art_cli_lite.exit_status (Exit_status.show exit_status));
  init_event_logger root command ~init_id ~from config local_config;
  List.iter !qe_fetches ~f:log_qe_fetch;

  let init_proc_stack =
    Option.some_if
      (String.equal "" from
      || local_config
           .Server_local_config.log_init_proc_stack_also_on_absent_from)
      init_proc_stack
  in
  try
    let exit_status =
      match command with
      | Client_command.CCheck check_env ->
        let check_env = apply_agent_error_format local_config check_env in
        Client_check.main check_env config local_config ~init_proc_stack
        (* never returns; does [Exit.exit] itself *)
      | Client_command.CStart env ->
        Lwt_utils.run_main (fun () -> Client_start.main env)
      | Client_command.CStop env ->
        Lwt_utils.run_main (fun () -> Client_stop.main env)
      | Client_command.CRestart env ->
        Lwt_utils.run_main (fun () -> Client_restart.main env)
      | Client_command.CLsp args ->
        Lwt_utils.run_main (fun () ->
            Client_lsp.main args ~init_id ~config ~local_config ~init_proc_stack)
      | Client_command.CRage env ->
        Lwt_utils.run_main (fun () -> Client_rage.main env local_config)
      | Client_command.CSavedStateProjectMetadata env ->
        Lwt_utils.run_main (fun () ->
            Client_saved_state_project_metadata.main env local_config)
      | Client_command.CDownloadSavedState env ->
        Lwt_utils.run_main (fun () ->
            Client_download_saved_state.main env local_config)
    in
    Exit.exit exit_status
  with
  | exn -> handle_exn_and_exit exn ~command_name

let main () =
  (* Resolve once, before argument parsing, so every path through this client
   * process observes the same session ID. *)
  Agent_session_id.initialize ();
  Server_local_config_qe.prepare_client_startup ();
  (* no-op, needed at entry-point for Daemon hookup *)
  Daemon.check_entry_point ();
  (* This invokes fbinit logic, which subsumes Folly.ensure_folly_init () *)
  Startup_initializer.init ();
  set_up_signals ();
  let init_proc_stack = Proc.get_proc_stack (Unix.getpid ()) in
  let command =
    try
      Client_args.parse_args
        ~from_default:
          (if Proc.is_likely_from_interactive_shell init_proc_stack then
            "[sh]"
          else
            "")
    with
    | Exit_status.Exit_with exit_status -> Exit.exit exit_status
  in
  match command with
  | Client_command.Without_config command -> exec_command_without_config command
  | Client_command.With_config command ->
    exec_command_with_config command ~init_proc_stack

let () =
  try main () with
  | Exit_status.Exit_with exit_status -> Exit.exit exit_status
  | exn ->
    let exit_status = Exit_status.Uncaught_exception (Exception.wrap exn) in
    Printf.eprintf "%s\n%!" (Exit_status.show_expanded exit_status);
    Exit.exit exit_status
