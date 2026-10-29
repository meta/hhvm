(*
 * Copyright (c) 2015, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude
open Client_command
open Client_env

(** Whether an option is shown in --help or only in --ultrahelp.
    Default new arguments to [Arg_non_user_facing]. Use [Arg_user_facing]
    only for established, commonly used user or agent workflows.
    See snapshot tests: $FBCODE/hphp/hack/test/help *)
type arg_visibility =
  | Arg_user_facing
      (** An argument that is for users and agents.
    * Will be shown in both hh --help and hh --ultrahelp
    *)
  | Arg_non_user_facing
      (** An argument that is for language and release teams,
      or for very rare use cases.
    * will be shown only in hh --ultrahelp
    *)

(** Use [Help_modes] for arguments that select an operation, such as
    [--find-refs], or a standalone action, such as [--version].
    Use [Help_options] for arguments that supply inputs, such as [--single],
    or configure an operation, such as [--json]. *)
type help_section =
  | Help_modes
  | Help_options

(** Arg specs shared across more than 1 arg parser. *)
module Common_argspecs = struct
  (* Failure telemetry can retain the caller parsed so far, including aliases,
     without attempting to interpret the remaining malformed argv. *)
  let from_for_bad_args : string ref option ref = ref None

  let add_key_value ~option_name value_ref value =
    match String_utils.split2 '=' value with
    | Some key_value -> value_ref := key_value :: !value_ref
    | None ->
      raise
        (Arg.Bad
           (Printf.sprintf
              "option '%s' expects an argument in the form <key>=<value>"
              option_name))

  let config value_ref =
    ( "--config",
      Arg.String (add_key_value ~option_name:"--config" value_ref),
      " override arbitrary value from hh.conf and .hhconfig (format: <key>=<value>)"
    )

  let custom_hhi_path value_ref =
    ( "--custom-hhi-path",
      Arg.String (fun s -> value_ref := Some s),
      " use custom hhi files" )

  let custom_telemetry_data value_ref =
    ( "--custom-telemetry-data",
      Arg.String
        (add_key_value ~option_name:"--custom-telemetry-data" value_ref),
      "Add a custom column to all logged telemetry samples (format: <column>=<value>)"
    )

  let force_dormant_start value_ref =
    ( "--force-dormant-start",
      Arg.Bool (fun x -> value_ref := x),
      " If server is dormant, force start a new one instead of waiting for"
      ^ " the next one to start up automatically (default: false)" )

  let from value_ref =
    from_for_bad_args := Some value_ref;
    ( "--from",
      Arg.Set_string value_ref,
      " so we know who's calling hh_client - e.g. nuclide, vim, emacs, vscode"
      (* This setting also controls whether spinner is displayed, and whether
         clientCheckStatus.ml uses error-formatting. *) )

  let no_prechecked value_ref =
    ( "--no-prechecked",
      Arg.Unit (fun () -> value_ref := Some false),
      " override value of \"prechecked_files\" flag from hh.conf" )

  let prechecked value_ref =
    ( "--prechecked",
      Arg.Unit (fun () -> value_ref := Some true),
      " override value of \"prechecked_files\" flag from hh.conf" )

  let with_mini_state (value_ref : string option ref) =
    ( "--with-mini-state",
      Arg.String (fun s -> value_ref := Some s),
      " Init with the given saved state instead of the one based on current repo version."
    )

  let watchman_debug_logging value_ref =
    ( "--watchman-debug-logging",
      Arg.Set value_ref,
      " Logs full Watchman requests and responses. This is very noisy" )

  let allow_non_opt_build value_ref =
    ( "--allow-non-opt-build",
      Arg.Set value_ref,
      " Override build mode check triggered by warn_on_non_opt_build .hhconfig option"
    )

  let ignore_hh_version value_ref =
    ( "--ignore-hh-version",
      Arg.Set value_ref,
      " ignore hh_version check when loading saved states (default: false)" )

  let saved_state_ignore_hhconfig value_ref =
    ( "--saved-state-ignore-hhconfig",
      Arg.Set value_ref,
      " ignore hhconfig hash when loading saved states (default: false)" )

  let naming_table value_ref =
    ( "--naming-table",
      Arg.String (fun s -> value_ref := Some s),
      " use the provided naming table instead of fetching it from a saved state"
    )

  let preexisting_warnings preexisting_warnings =
    ( "--preexisting-warnings",
      Arg.Set preexisting_warnings,
      " show all preexisting warnings in typechecked files (default: false)" )
end

let parse_command () =
  if Array.length Sys.argv < 2 then
    CKNone
  else
    match String.lowercase Sys.argv.(1) with
    | "check" -> CKCheck
    | "start" -> CKStart
    | "stop" -> CKStop
    | "restart" -> CKRestart
    | "lsp" -> CKLsp
    | "saved-state-project-metadata" -> CKSavedStateProjectMetadata
    | "download-saved-state" -> CKDownloadSavedState
    | "rage" -> CKRage
    | "decompress-zhhdg" -> CKDecompressZhhdg
    | _ -> CKNone

type arg_error = {
  message: string;
  usage: string option;
}

exception Parse_error of arg_error

let is_interactive = List.mem [""; "[sh]"] ~equal:String.equal

let log_help ~from log_event =
  let stderr_level = Hh_logger.Level.min_level_stderr () in
  Hh_logger.Level.set_min_level_stderr Hh_logger.Level.Off;
  Utils.try_finally
    ~finally:(fun () -> Hh_logger.Level.set_min_level_stderr stderr_level)
    ~f:(fun () ->
      try
        Hack_event_logger.client_init
          ~init_id:(Random_id.short_string ())
          ~from
          ~is_interactive:(is_interactive from)
          ~custom_columns:[]
          ?agent_session_id:(Agent_session_id.get ())
          (Path.make ".");
        log_event ();
        Hack_event_logger.flush ()
      with
      | Exit_status.Exit_with _ as interrupt -> raise interrupt
      | _ -> ())

(** On malformed command line args, ensure we record telemetry. *)
module Die : sig
  (** Report [message] and optional [usage] on stderr, log [message] in a
      [CLIENT_BAD_ARGS] sample, and exit with [Exit_status.Client_bad_args].
      An empty [message] prints nothing, leaving [usage] to stand alone. *)
  val bad_args : message:string -> usage:string option -> 'a

  (** Like [bad_args], but prefixes [message] with "Error:". *)
  val bad_www_root : message:string -> 'a

  (** Log+re-raise. Should be unreachable:
    * There is no reason to show a user a backtrace when they provide
    * ill-formed command-line arguments to hh *)
  val internal_exception : Exception.t -> 'a
end = struct
  let log ~(exit_code : int) ~(exit_status : Exit_status.t) (e : Exception.t) :
      unit =
    (* Avoid the user seeing stderr from failures to write to scuba *)
    let stderr_level = Hh_logger.Level.min_level_stderr () in
    Hh_logger.Level.set_min_level_stderr Hh_logger.Level.Off;
    Utils.try_finally
      ~finally:(fun () -> Hh_logger.Level.set_min_level_stderr stderr_level)
      ~f:(fun () ->
        try
          let agent_session_id = Agent_session_id.get () in
          let from =
            Option.value_map
              !Common_argspecs.from_for_bad_args
              ~default:""
              ~f:(fun value_ref -> !value_ref)
          in
          (* Root and custom telemetry may be invalid, but the caller reference
             contains only the default and values already consumed by Arg. *)
          Hack_event_logger.client_init
          (* ~init_id is used for correlating things, specific value doesn't matter *)
            ~init_id:(Random_id.short_string ())
            ~from
            ~is_interactive:false
            ~custom_columns:[]
            ?agent_session_id
            (Path.make ".");
          Hack_event_logger.client_bad_args
            ~command_name:"Args"
            ~exit_code
            exit_status
            e;
          Hack_event_logger.flush ()
        with
        (* hh_client's SIGINT handler raises this. Swallowing it would make Ctrl-C
           look like it did nothing. *)
        | Exit_status.Exit_with _ as interrupt -> raise interrupt
        | _ -> ())

  let die ~(exit_status : Exit_status.t) (e : Exception.t) : 'a =
    let exit_code = Exit_status.exit_code exit_status in
    log ~exit_code ~exit_status e;
    Stdlib.exit exit_code

  let report (message : string) : unit = Printf.eprintf "%s\n%!" message

  let bad_args ~(message : string) ~(usage : string option) : 'a =
    if not (String.is_empty message) then report message;
    Option.iter usage ~f:report;
    die
      ~exit_status:Exit_status.Client_bad_args
      (Exception.wrap_unraised (Arg.Bad message))

  let bad_www_root ~(message : string) : 'a =
    report message;
    die
      ~exit_status:Exit_status.Client_bad_args
      (Exception.wrap_unraised (Arg.Bad message))

  let internal_exception (e : Exception.t) : 'a =
    match Exception.unwrap e with
    | Exit_status.Exit_with _ -> Exception.reraise e
    | _ ->
      let exit_status = Exit_status.Uncaught_exception e in
      log ~exit_code:(Exit_status.exit_code exit_status) ~exit_status e;
      Exception.reraise e
end

let parse_without_command options usage command =
  let args = ref [] in
  (try
     Arg.parse_argv
       ~current:(ref 0)
       Sys.argv
       (Arg.align options)
       (fun x -> args := x :: !args)
       usage
   with
  (* [Arg] newline-terminates its messages and the rest of hh_client's are
     unterminated; drop it here so everything downstream has one convention. *)
  | Arg.Bad message ->
    let message = String.chop_suffix_if_exists message ~suffix:"\n" in
    let (message, usage) =
      match String.lsplit2 message ~on:'\n' with
      | None -> (message, None)
      | Some (message, usage) -> (message, Some usage)
    in
    raise (Parse_error { message; usage })
  | Arg.Help _ as help -> raise help
  | e -> Die.internal_exception (Exception.wrap e));
  match List.rev !args with
  | x :: rest when String.(lowercase x = lowercase command) -> rest
  | args -> args

let interpret_root paths =
  match Wwwroot.interpret_command_line_root_parameter paths with
  | exception e -> Die.internal_exception (Exception.wrap e)
  | Ok root -> root
  | Error message -> Die.bad_www_root ~message:("Error: " ^ message)

let validate_check_args ~invalid_warning_codes =
  if not (List.is_empty invalid_warning_codes) then
    Die.bad_args
      ~message:
        (Printf.sprintf
           "Unknown warning codes: %s"
           (String.concat ~sep:", "
           @@ List.map invalid_warning_codes ~f:Int.to_string))
      ~usage:None

let parse_check_args cmd ~from_default : Client_env.client_check_env =
  (* arg parse output refs *)
  let autostart = ref true in
  let config = ref [] in
  let custom_telemetry_data = ref [] in
  let custom_hhi_path = ref None in
  let error_format = ref None in
  let force_dormant_start = ref false in
  let from = ref from_default in
  let show_spinner = ref None in
  let show_tast = ref false in
  let ignore_hh_version = ref false in
  let save_64bit = ref None in
  let save_human_readable_64bit_dep_map = ref None in
  let saved_state_ignore_hhconfig = ref false in
  let max_errors = ref None in
  let mode = ref None in
  let logname = ref false in
  let monitor_logname = ref false in
  let client_logname = ref false in
  let ide_logname = ref false in
  let lsp_logname = ref false in
  let lock_file = ref false in
  let no_load = ref false in
  let output_json = ref false in
  let isolation_grow = ref false in
  let isolation_movable = ref false in
  let isolation_move_to = ref None in
  let isolation_output = ref None in
  let isolation_seed_framework = ref None in
  let isolation_seed_list = ref None in
  let isolation_seed_dir = ref None in
  let isolation_max_dependents = ref None in
  let isolation_max_cluster_size = ref None in
  let isolation_batch_size = ref None in
  let isolation_allow_long_run = ref false in
  let isolation_max_seeds = ref None in
  let isolation_seed_offset = ref 0 in
  (* The mode carries the options, so setting it needs a value before the flags
     that follow it have been parsed. Both sites build from these refs, and the
     one after parsing is the one that counts. *)
  let isolation_options () =
    Server_isolation_types.
      {
        mode =
          (match !isolation_move_to with
          | Some target -> Move_to target
          | None ->
            if !isolation_movable then
              Check_movability
            else if !isolation_grow then
              Grow
            else
              Measure);
        clusters_destination =
          (match !isolation_output with
          | Some path -> To_file path
          | None -> Dropped);
        seed_framework = !isolation_seed_framework;
        seed_list = !isolation_seed_list;
        seed_dir = !isolation_seed_dir;
        (* 600 is the reach a run uses when the caller names none, and so the
           bound the isolatability figures are reported against. Not derived
           from measurement. *)
        max_dependents = Option.value !isolation_max_dependents ~default:600;
        max_cluster_size = !isolation_max_cluster_size;
        (* The largest batch a long run has finished at; not a tuned
           figure. *)
        batch_size = Option.value !isolation_batch_size ~default:400;
        allow_long_run = !isolation_allow_long_run;
        max_seeds = !isolation_max_seeds;
        seed_offset = !isolation_seed_offset;
      }
  in
  let output_jsonl = ref false in
  let prechecked = ref None in
  let mini_state : string option ref = ref None in
  let rename_before = ref "" in
  let sort_results = ref false in
  let stdin_name = ref None in
  let timeout = ref None in
  let version = ref false in
  let watchman_debug_logging = ref false in
  let allow_non_opt_build = ref false in
  let preexisting_warnings = ref false in
  let reason = ref None in
  let warning_switches = ref [] in
  let invalid_warning_codes = ref [] in
  let desc = ref (Client_command.command_name cmd) in
  (* custom behaviors *)
  let current_option = ref None in
  let dump_config = ref false in

  let set_from x () = from := x in
  let files_to_check = ref [] in
  let only_log_errors = ref false in
  let log_to_file = ref None in
  let set_mode ?(validate = true) x =
    if validate && Option.is_some !mode then
      raise (Arg.Bad "only a single mode should be specified")
    else begin
      mode := Some x;
      Option.iter !current_option ~f:(fun option -> desc := option);
      ()
    end
  in
  let add_single x = files_to_check := x :: !files_to_check in
  let set_log_to_file x = log_to_file := Some x in
  let add_multi f =
    let files =
      (try In_channel.read_all f with
      | Sys_error message ->
        raise (Arg.Bad ("could not read --multi file: " ^ message)))
      |> String.strip
      |> String.split ~on:'\n'
    in
    files_to_check := files @ !files_to_check
  in
  let add_warning_switch switch =
    warning_switches := switch :: !warning_switches
  in
  let add_invalid_warning_code i =
    invalid_warning_codes := i :: !invalid_warning_codes
  in
  let set_mode_from_files_to_check (show_tast : bool) preexisting_warnings =
    match !files_to_check with
    | [] -> ()
    | files_to_check ->
      (match !mode with
      | _ ->
        set_mode
          (MODE_STATUS_FILES
             { filenames = files_to_check; show_tast; preexisting_warnings }))
  in
  let set_mode_only_log_errors config =
    if !only_log_errors then set_mode (MODE_LOG_ERRORS config)
  in
  (* parse args *)
  let usage =
    match cmd with
    | CKCheck ->
      Printf.sprintf
        "Usage: %s check [OPTION]... [WWW-ROOT]\n\nWWW-ROOT is assumed to be current directory if unspecified\n"
        Sys.argv.(0)
    | CKNone ->
      Printf.sprintf
        ("Usage: %s [COMMAND] [OPTION]... [WWW-ROOT]\n\nValid values for COMMAND:\n"
        ^^ "\tcheck\t\tShows current Hack errors\n"
        ^^ "\tstart\t\tStarts a Hack server\n"
        ^^ "\tstop\t\tStops a Hack server\n"
        ^^ "\trestart\t\tRestarts a Hack server\n"
        ^^ "\tlsp\t\tRuns a persistent language service\n"
        ^^ "\trage\t\tReport a bug\n"
        ^^ "\nDefault values if unspecified:\n"
        ^^ "\tCOMMAND\t\tcheck\n"
        ^^ "\tWWW-ROOT\tCurrent directory\n\nCheck command options:\n")
        Sys.argv.(0)
    | CKSavedStateProjectMetadata ->
      Printf.sprintf
        "Usage: %s saved-state-project-metadata [OPTION]... [WWW-ROOT]\nOutput the project metadata for the current saved state\n\nWWW-ROOT is assumed to be current directory if unspecified\n"
        Sys.argv.(0)
    | CKDownloadSavedState
    | CKLsp
    | CKRage
    | CKRestart
    | CKStart
    | CKDecompressZhhdg
    | CKStop ->
      failwith "No other keywords should make it here"
  in
  let tuple_3_append (a1, a2, a3) a4 = (a1, a2, a3, a4) in
  let options =
    [
      (* Please keep these sorted in the alphabetical order. *)
      tuple_3_append
        (Common_argspecs.allow_non_opt_build allow_non_opt_build)
        (Arg_non_user_facing, Help_options);
      ( "--autostart-server",
        Arg.Bool (( := ) autostart),
        " automatically start hh_server if it's not running (default: true)",
        (Arg_non_user_facing, Help_options) );
      tuple_3_append
        (Common_argspecs.config config)
        (Arg_non_user_facing, Help_options);
      ( "--cst-search",
        Arg.Unit (fun () -> set_mode (MODE_CST_SEARCH None)),
        " Search the concrete syntax trees of files in the codebase"
        ^ " for a given pattern",
        (Arg_non_user_facing, Help_modes) );
      ( "--cst-search-files",
        Arg.Rest
          begin
            fun fn ->
              set_mode
                ~validate:false
                (match !mode with
                | None
                | Some (MODE_CST_SEARCH None) ->
                  MODE_CST_SEARCH (Some [fn])
                | Some (MODE_CST_SEARCH (Some fnl)) ->
                  MODE_CST_SEARCH (Some (fn :: fnl))
                | _ -> raise (Arg.Bad "only a single mode should be specified"))
          end,
        " Run CST search on this set of files,"
        ^ " rather than all the files in the codebase.",
        (Arg_non_user_facing, Help_modes) );
      tuple_3_append
        (Common_argspecs.custom_hhi_path custom_hhi_path)
        (Arg_non_user_facing, Help_options);
      tuple_3_append
        (Common_argspecs.custom_telemetry_data custom_telemetry_data)
        (Arg_non_user_facing, Help_options);
      ( "--deps-out-at-pos-batch",
        Arg.Rest
          begin
            fun position ->
              set_mode
                ~validate:false
                (match !mode with
                | None -> MODE_DEPS_OUT_AT_POS_BATCH [position]
                | Some (MODE_DEPS_OUT_AT_POS_BATCH positions) ->
                  MODE_FUN_DEPS_AT_POS_BATCH (position :: positions)
                | _ -> raise (Arg.Bad "only a single mode should be specified"))
          end,
        " for each entry in input list get list of what it depends on [file:line:character list]",
        (Arg_non_user_facing, Help_modes) );
      ( "--deps-in-at-pos-batch",
        Arg.Rest
          begin
            fun position ->
              set_mode
                ~validate:false
                (match !mode with
                | None -> MODE_DEPS_IN_AT_POS_BATCH [position]
                | Some (MODE_DEPS_IN_AT_POS_BATCH positions) ->
                  MODE_DEPS_IN_AT_POS_BATCH (position :: positions)
                | _ -> raise (Arg.Bad "only a single mode should be specified"))
          end,
        " for each entry in input list get list of what depends on it [file:line:character list]",
        (Arg_non_user_facing, Help_modes) );
      ( "--dump-config",
        Arg.Set dump_config,
        " Output configuration",
        (Arg_non_user_facing, Help_options) );
      ( "--dump-full-fidelity-parse",
        Arg.String (fun x -> set_mode (MODE_FULL_FIDELITY_PARSE x)),
        "",
        (Arg_non_user_facing, Help_modes) );
      ( "--dump-symbol-info",
        Arg.String (fun files -> set_mode (MODE_DUMP_SYMBOL_INFO files)),
        (*  Input format:
         *  The file list can either be "-" which accepts the input from stdin
         *  separated by newline(for long list) or directly from command line
         *  separated by semicolon.
         *  Output format:
         *    [
         *      "function_calls": list of fun_calls;
         *    ]
         *  Note: results list can be in any order *)
        "",
        (Arg_non_user_facing, Help_modes) );
      ( "--error-format",
        Arg.String
          (fun s ->
            match s with
            | "raw" -> error_format := Some Diagnostics.Raw
            | "agent" -> error_format := Some Diagnostics.Agent
            | "plain" -> error_format := Some Diagnostics.Plain
            | "context" -> error_format := Some Diagnostics.Context
            | "highlighted" -> error_format := Some Diagnostics.Highlighted
            | "extended" -> error_format := Some Diagnostics.Extended
            | "plain_highlighted" ->
              error_format := Some Diagnostics.PlainHighlighted
            | _ -> print_string "Warning: unrecognized error format.\n"),
        "<format> Error formatting style (default: highlighted); valid formats: agent, extended, raw, context, highlighted, plain, plain_highlighted",
        (Arg_user_facing, Help_options) );
      ( "--enforcement-at-pos-batch",
        Arg.Rest
          begin
            fun position ->
              set_mode
                ~validate:false
                (match !mode with
                | None -> MODE_ENFORCEMENT_AT_POS_BATCH [position]
                | Some (MODE_ENFORCEMENT_AT_POS_BATCH positions) ->
                  MODE_ENFORCEMENT_AT_POS_BATCH (position :: positions)
                | _ -> raise (Arg.Bad "only a single mode should be specified"))
          end,
        " show enforcement at multiple positions [file:line:character list]",
        (Arg_non_user_facing, Help_modes) );
      ( "--file-dependents",
        Arg.Unit
          (fun () ->
            let () = prechecked := Some false in
            set_mode MODE_FILE_LEVEL_DEPENDENCIES),
        " Given a list of filepaths, shows list of (possibly) dependent files",
        (Arg_non_user_facing, Help_modes) );
      ( "--find-class-refs",
        Arg.String (fun x -> set_mode (MODE_FIND_CLASS_REFS x)),
        " finds references of the provided class name",
        (Arg_user_facing, Help_modes) );
      ( "--isolation",
        Arg.Unit (fun () -> set_mode (MODE_ISOLATION (isolation_options ()))),
        " find clusters of files that can be isolated from the codebase",
        (Arg_non_user_facing, Help_modes) );
      ( "--isolation-movable",
        Arg.Set isolation_movable,
        " ask whether the starting files can move as they stand, naming the files that block it; reads the dependency graph only, so a typecheck can still object",
        (Arg_non_user_facing, Help_options) );
      ( "--isolation-move-to",
        Arg.String (fun s -> isolation_move_to := Some s),
        " <dir> move the directory named by --isolation-seed-dir into <dir>, under its own name, if it can move and the move introduces no errors; otherwise leave it where it was. Moves the files on disk and nothing else. The server should run with --config edenfs_file_watcher_enabled=true, or it may not see the move",
        (Arg_non_user_facing, Help_options) );
      ( "--isolation-grow",
        Arg.Set isolation_grow,
        " grow a bigger cluster around each starting file, instead of reporting its smallest isolatable set",
        (Arg_non_user_facing, Help_options) );
      ( "--isolation-output",
        Arg.String (fun s -> isolation_output := Some s),
        " <file> write clusters to <file> as JSON lines as they are found, and print only the summary",
        (Arg_non_user_facing, Help_options) );
      ( "--isolation-seed-framework",
        Arg.String (fun s -> isolation_seed_framework := Some s),
        " <class> start from every subclass of <class> — an entry-point family — instead of scanning for seeds",
        (Arg_non_user_facing, Help_options) );
      ( "--isolation-seed-list",
        Arg.String (fun s -> isolation_seed_list := Some s),
        " <file> grow from the repo-relative paths listed in <file> instead of scanning for seeds",
        (Arg_non_user_facing, Help_options) );
      ( "--isolation-seed-dir",
        Arg.String (fun s -> isolation_seed_dir := Some s),
        " <dir> grow from every Hack file under <dir>",
        (Arg_non_user_facing, Help_options) );
      ( "--isolation-max-dependents",
        Arg.Int (fun n -> isolation_max_dependents := Some n),
        " <n> the most files that may depend on a candidate for it to be taken along with them",
        (Arg_non_user_facing, Help_options) );
      ( "--isolation-max-cluster-size",
        Arg.Int (fun n -> isolation_max_cluster_size := Some n),
        " <n> stop growing a cluster once it reaches <n> files",
        (Arg_non_user_facing, Help_options) );
      ( "--isolation-allow-long-run",
        Arg.Set isolation_allow_long_run,
        " run even when the seed count says this will take hours",
        (Arg_non_user_facing, Help_options) );
      ( "--isolation-batch-size",
        Arg.Int (fun n -> isolation_batch_size := Some n),
        " <n> grow <n> clusters at a time; they are all held in memory together, so this sets how much memory the run needs, and a larger batch drops more overlapping clusters so it also changes how many are reported",
        (Arg_non_user_facing, Help_options) );
      ( "--isolation-max-seeds",
        Arg.Int (fun n -> isolation_max_seeds := Some n),
        " <n> grow from at most <n> seeds",
        (Arg_non_user_facing, Help_options) );
      ( "--isolation-seed-offset",
        Arg.Set_int isolation_seed_offset,
        " <n> skip the first <n> seeds; pair with --isolation-max-seeds to walk the seed list in chunks",
        (Arg_non_user_facing, Help_options) );
      ( "--find-refs",
        Arg.String (fun x -> set_mode (MODE_FIND_REFS x)),
        " finds references of the provided symbol; optionally specify the symbol kind like \"Kind|Symbol\" (looks for functions or methods if unspecified)"
        ^ "; valid kinds are Method, Property, Class_const, Typeconst, Function, Class, ExplicitClass, and GConst"
        ^ "; use ExplicitClass instead of Class to exclude references via self/static/parent",
        (Arg_user_facing, Help_modes) );
      ( "--find-my-tests",
        Arg.String
          (fun path -> set_mode ~validate:true (MODE_FIND_MY_TESTS path)),
        " return test files that reference the given symbols, using the version specified in the JSON config (default: staging)",
        (Arg_non_user_facing, Help_modes) );
      ( "--find-my-tests-staging",
        Arg.String
          (fun path -> set_mode ~validate:true (MODE_FIND_MY_TESTS path)),
        " (deprecated, use --find-my-tests) alias for --find-my-tests",
        (Arg_non_user_facing, Help_modes) );
      tuple_3_append
        (Common_argspecs.force_dormant_start force_dormant_start)
        (Arg_non_user_facing, Help_options);
      tuple_3_append (Common_argspecs.from from) (Arg_user_facing, Help_options);
      ( "--from-arc-diff",
        Arg.Unit (set_from "arc_diff"),
        " (deprecated) equivalent to --from arc_diff",
        (Arg_non_user_facing, Help_options) );
      ( "--from-arc-land",
        Arg.Unit (set_from "arc_land"),
        " (deprecated) equivalent to --from arc_land",
        (Arg_non_user_facing, Help_options) );
      ( "--from-check-trunk",
        Arg.Unit (set_from "check_trunk"),
        " (deprecated) equivalent to --from check_trunk",
        (Arg_non_user_facing, Help_options) );
      ( "--from-emacs",
        Arg.Unit (set_from "emacs"),
        " (deprecated) equivalent to --from emacs",
        (Arg_non_user_facing, Help_options) );
      ( "--from-vim",
        Arg.Unit (fun () -> from := "vim"),
        " (deprecated) equivalent to --from vim",
        (Arg_non_user_facing, Help_options) );
      ( "--full-fidelity-schema",
        Arg.Unit (fun () -> set_mode MODE_FULL_FIDELITY_SCHEMA),
        "",
        (Arg_non_user_facing, Help_modes) );
      ( "--fun-deps-at-pos-batch",
        Arg.Rest
          begin
            fun position ->
              set_mode
                ~validate:false
                (match !mode with
                | None -> MODE_FUN_DEPS_AT_POS_BATCH [position]
                | Some (MODE_FUN_DEPS_AT_POS_BATCH positions) ->
                  MODE_FUN_DEPS_AT_POS_BATCH (position :: positions)
                | _ -> raise (Arg.Bad "only a single mode should be specified"))
          end,
        " for each entry in input list get list of function dependencies [file:line:character list]",
        (Arg_non_user_facing, Help_modes) );
      ( "--get-method-name",
        Arg.String (fun x -> set_mode (MODE_IDENTIFY_SYMBOL3 x)),
        (* alias for --identify-function *) "",
        (Arg_non_user_facing, Help_modes) );
      ( "--go-to-impl-class",
        Arg.String (fun x -> set_mode (MODE_GO_TO_IMPL_CLASS x)),
        " goes to implementation of the provided class/trait/interface/etc. with the given name",
        (Arg_non_user_facing, Help_modes) );
      ( "--go-to-impl-method",
        Arg.String (fun x -> set_mode (MODE_GO_TO_IMPL_METHOD x)),
        " goes to implementation of the provided method name",
        (Arg_non_user_facing, Help_modes) );
      ( "--hack-to-notebook",
        Arg.Unit (fun () -> set_mode MODE_HACK_TO_NOTEBOOK),
        " Convert a Hack file generated by `hh --notebook-to-hack` into a notebook. Pass hack file contents via stdin",
        (Arg_non_user_facing, Help_modes) );
      ( "--ide-find-refs-by-symbol",
        Arg.String
          (fun x ->
            set_mode
              (MODE_IDE_FIND_REFS_BY_SYMBOL
                 (Find_refs_wire_format.CliArgs.from_string_exn x))),
        " similar to IDE_FIND_REFS, but takes a symbol name rather than position",
        (Arg_non_user_facing, Help_modes) );
      ( "--ide-find-refs-by-symbol3",
        (let action = ref "" in
         let stream_file = ref "-" in
         Arg.Tuple
           [
             Arg.String (fun s -> action := s);
             Arg.String (fun s -> stream_file := s);
             Arg.String
               (fun hints ->
                 set_mode
                   (MODE_IDE_FIND_REFS_BY_SYMBOL
                      (Find_refs_wire_format.CliArgs.from_string_triple_exn
                         (!action, !stream_file, hints))));
           ]),
        " similar to FIND_REFS, but takes [action stream_file hints]",
        (Arg_non_user_facing, Help_modes) );
      ( "--ide-go-to-impl-by-symbol",
        Arg.String
          (fun x ->
            set_mode
              (MODE_IDE_GO_TO_IMPL_BY_SYMBOL
                 (Find_refs_wire_format.CliArgs.from_string_exn x))),
        " similar to IDE_GO_TO_IMPL, but takes a symbol name rather than position",
        (Arg_non_user_facing, Help_modes) );
      ( "--ide-get-definition",
        Arg.String (fun x -> set_mode (MODE_IDENTIFY_SYMBOL2 x)),
        (* alias for --identify-function *) "",
        (Arg_non_user_facing, Help_modes) );
      ( "--ide-outline",
        Arg.Unit (fun () -> set_mode MODE_OUTLINE2),
        "",
        (Arg_non_user_facing, Help_modes) );
      ( "--ide-rename-by-symbol",
        Arg.String (fun x -> set_mode (MODE_IDE_RENAME_BY_SYMBOL x)),
        " renames, but takes a Find_refs.action. Usage: "
        ^ " <new name>|<comma_separated_action>",
        (Arg_non_user_facing, Help_modes) );
      ( "--identify-function",
        Arg.String (fun x -> set_mode (MODE_IDENTIFY_SYMBOL1 x)),
        " print the full function name at the position "
        ^ "[line:character] of the text on stdin",
        (Arg_non_user_facing, Help_modes) );
      ( "--identify",
        Arg.String (fun x -> set_mode (MODE_IDENTIFY_SYMBOL x)),
        " identify the named symbol",
        (Arg_user_facing, Help_modes) );
      tuple_3_append
        (Common_argspecs.ignore_hh_version ignore_hh_version)
        (Arg_non_user_facing, Help_options);
      ( "--in-memory-dep-table-size",
        Arg.Unit (fun () -> set_mode MODE_IN_MEMORY_DEP_TABLE_SIZE),
        " number of entries in the in-memory dependency table",
        (Arg_non_user_facing, Help_modes) );
      ( "--inheritance-ancestor-classes",
        Arg.String (fun x -> set_mode (MODE_METHOD_JUMP_ANCESTORS (x, "Class"))),
        " prints a list of classes that this class extends",
        (* can use --inheritance-ancestor-classes-batch *)
        (Arg_non_user_facing, Help_modes) );
      ( "--inheritance-ancestor-classes-batch",
        Arg.Rest
          begin
            fun class_ ->
              set_mode
                ~validate:false
                (match !mode with
                | None -> MODE_METHOD_JUMP_ANCESTORS_BATCH ([class_], "Class")
                | Some (MODE_METHOD_JUMP_ANCESTORS_BATCH (classes, "Class")) ->
                  MODE_METHOD_JUMP_ANCESTORS_BATCH (class_ :: classes, "Class")
                | _ -> raise (Arg.Bad "only a single mode should be specified"))
          end,
        " prints a list of classes that these classes extend",
        (Arg_non_user_facing, Help_modes) );
      ( "--inheritance-ancestor-interfaces",
        Arg.String
          (fun x -> set_mode (MODE_METHOD_JUMP_ANCESTORS (x, "Interface"))),
        " prints a list of interfaces that this class implements",
        (* can use --inheritance-ancestor-interfaces-batch *)
        (Arg_non_user_facing, Help_modes) );
      ( "--inheritance-ancestor-interfaces-batch",
        Arg.Rest
          begin
            fun class_ ->
              set_mode
                ~validate:false
                (match !mode with
                | None ->
                  MODE_METHOD_JUMP_ANCESTORS_BATCH ([class_], "Interface")
                | Some (MODE_METHOD_JUMP_ANCESTORS_BATCH (classes, "Interface"))
                  ->
                  MODE_METHOD_JUMP_ANCESTORS_BATCH
                    (class_ :: classes, "Interface")
                | _ -> raise (Arg.Bad "only a single mode should be specified"))
          end,
        " prints a list of interfaces that these classes implement",
        (Arg_non_user_facing, Help_modes) );
      ( "--inheritance-ancestor-traits",
        Arg.String (fun x -> set_mode (MODE_METHOD_JUMP_ANCESTORS (x, "Trait"))),
        " prints a list of traits that this class uses",
        (* can use --inheritance-ancestor-traits-batch *)
        (Arg_non_user_facing, Help_modes) );
      ( "--inheritance-ancestor-traits-batch",
        Arg.Rest
          begin
            fun class_ ->
              set_mode
                ~validate:false
                (match !mode with
                | None -> MODE_METHOD_JUMP_ANCESTORS_BATCH ([class_], "Trait")
                | Some (MODE_METHOD_JUMP_ANCESTORS_BATCH (classes, "Trait")) ->
                  MODE_METHOD_JUMP_ANCESTORS_BATCH (class_ :: classes, "Trait")
                | _ -> raise (Arg.Bad "only a single mode should be specified"))
          end,
        " prints a list of traits that these classes use",
        (Arg_non_user_facing, Help_modes) );
      ( "--inheritance-ancestors",
        Arg.String
          (fun x -> set_mode (MODE_METHOD_JUMP_ANCESTORS (x, "No_filter"))),
        " prints a list of all related classes or methods"
        ^ " to the given class",
        (Arg_user_facing, Help_modes) );
      ( "--inheritance-children",
        Arg.String (fun x -> set_mode (MODE_METHOD_JUMP_CHILDREN x)),
        " prints a list of all related classes or methods"
        ^ " to the given class",
        (Arg_user_facing, Help_modes) );
      ( "--json",
        Arg.Set output_json,
        " output json for machine consumption (default: false)",
        (Arg_user_facing, Help_options) );
      ( "--jsonl",
        Arg.Set output_jsonl,
        " output streaming jsonl (JSON Lines) for machine consumption (default: false)",
        (Arg_user_facing, Help_options) );
      ( "--lint",
        Arg.Unit (fun () -> set_mode MODE_LINT),
        " lint the given list of files",
        (Arg_non_user_facing, Help_modes) );
      ( "--lint-all",
        Arg.Int (fun x -> set_mode (MODE_LINT_ALL x)),
        " find all occurrences of lint with the given error code",
        (Arg_non_user_facing, Help_modes) );
      ( "--lint-stdin",
        Arg.String (fun filename -> set_mode (MODE_LINT_STDIN filename)),
        " lint a file given on stdin; the filename should be the"
        ^ " argument to this option",
        (Arg_non_user_facing, Help_modes) );
      ( "--list-files",
        Arg.Unit (fun () -> set_mode MODE_LIST_FILES),
        " list files with errors",
        (Arg_user_facing, Help_modes) );
      ( "--lock-file",
        Arg.Set lock_file,
        " show lock file name and exit",
        (Arg_non_user_facing, Help_modes) );
      ( "--max-errors",
        Arg.Int (fun num_errors -> max_errors := Some num_errors),
        " Maximum number of errors to display",
        (Arg_user_facing, Help_options) );
      tuple_3_append
        (Common_argspecs.preexisting_warnings preexisting_warnings)
        (Arg_user_facing, Help_options);
      ( "--logname",
        Arg.Set logname,
        " show log filename and exit",
        (Arg_non_user_facing, Help_modes) );
      ( "--monitor-logname",
        Arg.Set monitor_logname,
        " show monitor log filename and exit",
        (Arg_non_user_facing, Help_modes) );
      ( "--client-logname",
        Arg.Set client_logname,
        " show client log filename and exit",
        (Arg_non_user_facing, Help_modes) );
      ( "--ide-logname",
        Arg.Set ide_logname,
        " show client ide log filename and exit",
        (Arg_non_user_facing, Help_modes) );
      ( "--lsp-logname",
        Arg.Set lsp_logname,
        " show client lsp log filename and exit",
        (Arg_non_user_facing, Help_modes) );
      ( "--no-load",
        Arg.Set no_load,
        " start from a fresh state",
        (Arg_non_user_facing, Help_options) );
      ( "--notebook-to-hack",
        (let notebook_number = ref "" in
         Arg.Tuple
           [
             Arg.String (( := ) notebook_number);
             Arg.String
               (fun notebook_header ->
                 set_mode
                   (MODE_NOTEBOOK_TO_HACK
                      { notebook_number = !notebook_number; notebook_header }));
           ]),
        " Convert notebook to a Hack file. Arg: notebook_number (example: \"N12345\"). "
        ^ "Pass notebook in .ipynb format to stdin and a header (such as a copyright notice comment)",
        (Arg_non_user_facing, Help_modes) );
      ( "--outline",
        Arg.Unit (fun () -> set_mode MODE_OUTLINE),
        " prints an outline of the text on stdin",
        (Arg_user_facing, Help_modes) );
      ( "--outline-for-agents",
        Arg.String (fun x -> set_mode (MODE_OUTLINE_FOR_AGENTS x)),
        " <file> outline the file, including line spans, to help agents efficiently navigate large files",
        (Arg_user_facing, Help_modes) );
      ( "--package-lint",
        Arg.String (fun x -> set_mode (MODE_PACKAGE_LINT x)),
        " Support for linting of __PackageOverride annotations",
        (Arg_non_user_facing, Help_modes) );
      ( "--package-lint-full",
        (let target_file = ref "" in
         Arg.Tuple
           [
             Arg.String (( := ) target_file);
             Arg.String
               (fun candidates_csv ->
                 let candidates =
                   String.split candidates_csv ~on:','
                   |> List.filter ~f:(fun s -> not (String.is_empty s))
                 in
                 if List.is_empty candidates then
                   raise
                     (Arg.Bad
                        "--package-lint-full requires at least one candidate file");
                 set_mode (MODE_PACKAGE_LINT_FULL (!target_file, candidates)));
           ]),
        " <target_file> <file1,file2,...> Package-aware find-refs lint",
        (Arg_non_user_facing, Help_modes) );
      ( "--pause",
        Arg.Unit (fun () -> set_mode MODE_PAUSE),
        " pause automatic rechecks after current work finishes; the next client request resumes them",
        (Arg_non_user_facing, Help_modes) );
      tuple_3_append
        (Common_argspecs.prechecked prechecked)
        (Arg_non_user_facing, Help_options);
      tuple_3_append
        (Common_argspecs.no_prechecked prechecked)
        (Arg_non_user_facing, Help_options);
      tuple_3_append
        (Common_argspecs.with_mini_state mini_state)
        (Arg_non_user_facing, Help_options);
      ( "--profile-log",
        Arg.Unit (fun () -> config := ("profile_log", "true") :: !config),
        " enable profile logging",
        (Arg_non_user_facing, Help_options) );
      ( "--refactor",
        (let rename_mode = ref Unspecified in
         Arg.Tuple
           [
             Arg.Symbol
               ( ["Class"; "Function"; "Method"],
                 (fun x -> rename_mode := string_to_rename_mode x) );
             Arg.String (fun x -> rename_before := x);
             Arg.String
               (fun x ->
                 set_mode (MODE_RENAME (!rename_mode, !rename_before, x)));
           ]),
        " rename a symbol, Usage: --refactor "
        ^ "[\"Class\", \"Function\", \"Method\"] <Current Name> <New Name>",
        (Arg_non_user_facing, Help_modes) );
      ( "--remove-dead-fixme",
        Arg.Int
          begin
            fun code ->
              set_mode
                ~validate:false
                (match !mode with
                | None -> MODE_REMOVE_DEAD_FIXMES [code]
                | Some (MODE_REMOVE_DEAD_FIXMES codel) ->
                  MODE_REMOVE_DEAD_FIXMES (code :: codel)
                | _ -> raise (Arg.Bad "only a single mode should be specified"))
          end,
        " remove dead HH_FIXME for specified error code "
        ^ "(first do hh_client restart --no-load)",
        (Arg_non_user_facing, Help_modes) );
      ( "--remove-dead-fixmes",
        Arg.Unit (fun () -> set_mode (MODE_REMOVE_DEAD_FIXMES [])),
        " remove dead HH_FIXME for any error code < 5000 "
        ^ "(first do hh_client restart --no-load)",
        (Arg_non_user_facing, Help_modes) );
      ( "--remove-dead-unsafe-casts",
        Arg.Unit (fun () -> set_mode MODE_REMOVE_DEAD_UNSAFE_CASTS),
        " remove dead UNSAFE_CASTS (first do hh_client restart --no-load)",
        (Arg_non_user_facing, Help_modes) );
      ( "--reason",
        Arg.String (fun value -> reason := Some value),
        " reason for invoking hh, logged to telemetry",
        (Arg_user_facing, Help_options) );
      ( "--retries",
        Arg.Int (fun n -> timeout := Some (float_of_int (max 5 n))),
        " (deprecated) same as --timeout",
        (Arg_non_user_facing, Help_options) );
      ( "--retry-if-init",
        Arg.Bool (fun _ -> ()),
        " (deprecated and ignored)",
        (Arg_non_user_facing, Help_options) );
      ( "--rewrite-declarations",
        Arg.Unit (fun () -> set_mode MODE_REWRITE_DECLARATIONS),
        {| Rewrite Hack source code allowed in notebooks to valid Hack.
For example in `function foo(): void {} function foo(): void{}` we
rewrite to the function names to something like `foo_1` and `foo_2`.
|},
        (Arg_non_user_facing, Help_modes) );
      ( "--rewrite-lambda-parameters",
        Arg.Rest
          begin
            fun fn ->
              set_mode
                ~validate:false
                (match !mode with
                | None -> MODE_REWRITE_LAMBDA_PARAMETERS [fn]
                | Some (MODE_REWRITE_LAMBDA_PARAMETERS fnl) ->
                  MODE_REWRITE_LAMBDA_PARAMETERS (fn :: fnl)
                | _ -> raise (Arg.Bad "only a single mode should be specified"))
          end,
        " rewrite lambdas in the files from the given list"
        ^ " with suggested parameter types",
        (Arg_non_user_facing, Help_modes) );
      ( "--save-naming",
        Arg.String (fun x -> set_mode (MODE_SAVE_NAMING x)),
        " Save the naming table to the given file."
        ^ " Returns the number of files and symbols written to disk.",
        (Arg_non_user_facing, Help_modes) );
      ( "--save-64bit",
        Arg.String (fun x -> save_64bit := Some x),
        " save discovered 64-bit to the given directory",
        (Arg_non_user_facing, Help_options) );
      ( "--save-human-readable-64bit-dep-map",
        Arg.String (fun x -> save_human_readable_64bit_dep_map := Some x),
        " save map of 64bit hashes to names to files in the given directory",
        (Arg_non_user_facing, Help_options) );
      tuple_3_append
        (Common_argspecs.saved_state_ignore_hhconfig
           saved_state_ignore_hhconfig)
        (Arg_non_user_facing, Help_options);
      ( "--search",
        Arg.String (fun x -> set_mode (MODE_SEARCH x)),
        " --search this_is_just_to_check_liveness_of_hh_server",
        (Arg_non_user_facing, Help_modes) );
      ( "--server-rage",
        Arg.Unit (fun () -> set_mode MODE_SERVER_RAGE),
        " dumps internal state of hh_server",
        (Arg_non_user_facing, Help_modes) );
      ( "--show-spinner",
        Arg.Bool (fun x -> show_spinner := Some x),
        " shows a spinner while awaiting the typechecker",
        (Arg_non_user_facing, Help_options) );
      ( "--single",
        Arg.String add_single,
        "<path> Return errors in file with provided name (give '-' for stdin)",
        (Arg_user_facing, Help_options) );
      ( "--multi",
        Arg.String add_multi,
        "<path> Return errors for files read from the given file (one per line)",
        (Arg_user_facing, Help_options) );
      ( "--log-errors",
        Arg.Unit (fun () -> only_log_errors := true),
        " type check the given list of files and log their errors (use --log-to-file to specify output file)",
        (Arg_non_user_facing, Help_modes) );
      ( "--log-to-file",
        Arg.String set_log_to_file,
        "<path> Write logged errors to specified file (use with --log-errors)",
        (Arg_non_user_facing, Help_options) );
      ( "--show-tast",
        Arg.Unit (fun () -> show_tast := true),
        " in combination with `--single`, output the TASTs of the file along with TAST hashes.",
        (Arg_non_user_facing, Help_options) );
      ( "--sort-results",
        Arg.Set sort_results,
        " sort output for CST search.",
        (Arg_non_user_facing, Help_options) );
      ( "--stats",
        Arg.Unit (fun () -> set_mode MODE_STATS),
        " display some server statistics",
        (Arg_non_user_facing, Help_modes) );
      ( "--stdin-name",
        Arg.String (fun x -> stdin_name := Some x),
        " substitute stdin for contents of file with specified name",
        (Arg_non_user_facing, Help_options) );
      ( "--status",
        Arg.Unit (fun () -> set_mode MODE_STATUS),
        " show a human readable list of errors (default)",
        (Arg_user_facing, Help_modes) );
      ( "--timeout",
        Arg.Float (fun x -> timeout := Some (Float.max 5. x)),
        " set the timeout in seconds (default: no timeout)",
        (Arg_user_facing, Help_options) );
      ( "--type-at-pos",
        Arg.String (fun x -> set_mode (MODE_TYPE_AT_POS x)),
        " show type at a given position in file [file:line:character]",
        (* can use --type-at-pos-batch *) (Arg_non_user_facing, Help_modes) );
      ( "--infer-dynamic",
        Arg.String (fun x -> set_mode (MODE_INFER_DYNAMIC (x, false))),
        " infer types for dynamic locals [function_name or ClassName::method_name]",
        (Arg_non_user_facing, Help_modes) );
      ( "--infer-dynamic-as-data",
        Arg.String (fun x -> set_mode (MODE_INFER_DYNAMIC (x, true))),
        " infer types for dynamic locals as data [function_name or ClassName::method_name]",
        (Arg_non_user_facing, Help_modes) );
      ( "--type-at-pos-batch",
        Arg.Rest
          begin
            fun position ->
              set_mode
                ~validate:false
                (match !mode with
                | None -> MODE_TYPE_AT_POS_BATCH [position]
                | Some (MODE_TYPE_AT_POS_BATCH positions) ->
                  MODE_TYPE_AT_POS_BATCH (position :: positions)
                | _ -> raise (Arg.Bad "only a single mode should be specified"))
          end,
        " show types at multiple positions [file:line:character list]",
        (Arg_non_user_facing, Help_modes) );
      ( "--type-error-at-pos",
        Arg.String (fun x -> set_mode (MODE_TYPE_ERROR_AT_POS x)),
        " show type error at a given position in file [line:character]",
        (Arg_non_user_facing, Help_modes) );
      ( "--is-subtype",
        Arg.Unit (fun () -> set_mode MODE_IS_SUBTYPE),
        " take a JSON list of subtype queries via stdin",
        (Arg_non_user_facing, Help_modes) );
      ( "--tast-holes",
        Arg.String (fun x -> set_mode (MODE_TAST_HOLES x)),
        " return all TAST Holes in a given file",
        (Arg_non_user_facing, Help_modes) );
      ( "--tast-holes-batch",
        Arg.String (fun x -> set_mode (MODE_TAST_HOLES_BATCH x)),
        " return all TAST Holes for a set of files. Argument is a file containing a newline-separated list of files",
        (Arg_non_user_facing, Help_modes) );
      ( "--validate-isolation",
        Arg.String (fun f -> set_mode (MODE_VALIDATE_ISOLATION f)),
        " <file> check whether the repo-relative paths listed in <file> could be a strict-isolation package; rebuilds decls in shared heaps, so use a server given over to this and restart it afterwards",
        (Arg_non_user_facing, Help_modes) );
      ( "--verbose-on",
        Arg.Unit (fun () -> set_mode (MODE_VERBOSE true)),
        " turn on verbose server log",
        (Arg_non_user_facing, Help_modes) );
      ( "--verbose-off",
        Arg.Unit (fun () -> set_mode (MODE_VERBOSE false)),
        " turn off verbose server log",
        (Arg_non_user_facing, Help_modes) );
      ( "--version",
        Arg.Set version,
        " show version and exit",
        (Arg_user_facing, Help_modes) );
      ( "-Wall",
        Arg.Unit (fun () -> add_warning_switch Filter_diagnostics.WAll),
        " show all warnings",
        (Arg_user_facing, Help_options) );
      ( "-Wnone",
        Arg.Unit (fun () -> add_warning_switch Filter_diagnostics.WNone),
        " hide all warnings",
        (Arg_user_facing, Help_options) );
      ( "-W",
        Arg.Int
          (fun code ->
            match Filter_diagnostics.Code.of_enum code with
            | None -> add_invalid_warning_code code
            | Some code -> add_warning_switch @@ Filter_diagnostics.Code_on code),
        " show all warnings with a given code, e.g. -W 12001",
        (Arg_user_facing, Help_options) );
      ( "-Wno",
        Arg.Int
          (fun code ->
            match Filter_diagnostics.Code.of_enum code with
            | None -> add_invalid_warning_code code
            | Some code ->
              add_warning_switch @@ Filter_diagnostics.Code_off code),
        " hide all warnings with a given code, e.g. -Wno 12001",
        (Arg_user_facing, Help_options) );
      ( "-Wignore-files",
        Arg.String
          (fun regexp ->
            let regexp =
              try Str.regexp regexp with
              | Failure message ->
                raise
                  (Arg.Bad
                     ("option '-Wignore-files' expects a valid regular expression: "
                     ^ message))
            in
            add_warning_switch (Filter_diagnostics.Ignored_files regexp)),
        " hide warnings in files matching a regexp",
        (Arg_user_facing, Help_options) );
      ( "-Wgenerated",
        Arg.Unit
          (fun () -> add_warning_switch Filter_diagnostics.Generated_files_on),
        " show warnings in generated files",
        (Arg_user_facing, Help_options) );
      tuple_3_append
        (Common_argspecs.watchman_debug_logging watchman_debug_logging)
        (Arg_non_user_facing, Help_options);
      (* Please keep these sorted in the alphabetical order *)
    ]
  in
  (* If the user typed an option like "--type-at-pos" which set a mode which triggered
     a command, we want to be able to show "hh_server is busy [--type-at-pos]" to show that
     hh_server is currently busy with something that the user typed. The string
     description that appears inside the square brackets must go into args.desc.
     Unfortunately this isn't well supported by the Arg library, so we have to hack it up
     ourselves: (1) For any option that takes say Arg.Unit(callback), we'll change it into
     Arg.Unit(modified_callback) where modified_callback sets 'current_option' to the
     option string being handled and then calls the original callback. (2) If the original
     callback calls set_mode, then set_mode will take the opportunity to do desc := current_option.
  *)
  let modify_callback : type a. string -> (a -> unit) -> a -> unit =
   fun option callback value ->
    current_option := Some option;
    callback value;
    current_option := None
  in
  let rec modify_spec ~option spec =
    match spec with
    | Arg.Unit callback -> Arg.Unit (modify_callback option callback)
    | Arg.Bool callback -> Arg.Bool (modify_callback option callback)
    | Arg.String callback -> Arg.String (modify_callback option callback)
    | Arg.Int callback -> Arg.Int (modify_callback option callback)
    | Arg.Float callback -> Arg.Float (modify_callback option callback)
    | Arg.Rest callback -> Arg.Rest (modify_callback option callback)
    | Arg.Tuple specs -> Arg.Tuple (List.map specs ~f:(modify_spec ~option))
    | spec -> spec
  in
  let options =
    List.map options ~f:(fun (option, spec, text, kind) ->
        (option, modify_spec ~option spec, text, kind))
  in
  let argv_has : string -> bool =
    let arg_set =
      Array.fold Sys.argv ~init:S_set.empty ~f:(fun acc s -> S_set.add s acc)
    in
    (fun key -> S_set.mem key arg_set)
  in
  let to_arg_specs options =
    List.map options ~f:(fun (key, spec, doc, _kind) -> (key, spec, doc))
  in
  let help ~ultrahelp =
    let display_options =
      List.filter options ~f:(fun (_key, _spec, _doc, (visibility, _section)) ->
          ultrahelp
          ||
          match visibility with
          | Arg_user_facing -> true
          | Arg_non_user_facing -> false)
    in
    let (modes, options) =
      List.partition_tf
        display_options
        ~f:(fun (_key, _spec, _doc, (_visibility, section)) ->
          match section with
          | Help_modes -> true
          | Help_options -> false)
    in
    let modes =
      List.map (to_arg_specs modes) ~f:(fun (key, spec, doc) ->
          ( key,
            spec,
            if String.is_empty doc then
              doc
            else
              " " ^ String.strip doc ))
      @ [
          ("-help", Arg.Unit (fun () -> ()), "");
          ("--help", Arg.Unit (fun () -> ()), "");
        ]
    in
    let options =
      to_arg_specs options
      @ [
          ( "--ultrahelp",
            Arg.Unit (fun () -> ()),
            " see all modes and options, including less frequently used ones" );
        ]
    in
    Arg.usage_string
      (Arg.align modes)
      (usage ^ "\nModes (mutually exclusive; choose one; default: --status):\n")
    ^ Arg.usage_string (Arg.align options) "\nOptions:\n"
  in
  let ultrahelp = argv_has "--ultrahelp" in
  if ultrahelp || argv_has "--help" || argv_has "-help" then begin
    print_string (help ~ultrahelp);
    Out_channel.flush stdout;
    log_help ~from:from_default (fun () ->
        Hack_event_logger.help
          ~kind:
            (if ultrahelp then
              "ultrahelp"
            else
              "help"));
    exit 0
  end;

  let args =
    try
      parse_without_command
        (to_arg_specs options)
        usage
        (Client_command.command_name cmd)
    with
    | Parse_error { message; usage = Some _ } ->
      let usage =
        String.chop_suffix_if_exists (help ~ultrahelp:false) ~suffix:"\n"
      in
      raise (Parse_error { message; usage = Some usage })
  in

  validate_check_args ~invalid_warning_codes:!invalid_warning_codes;

  if !output_json && !output_jsonl then
    Die.bad_args
      ~message:"--json and --jsonl are mutually exclusive"
      ~usage:None;

  if !version then (
    if !output_json then
      Server_args.print_json_version ()
    else
      print_endline Hh_version.version;
    exit 0
  );

  set_mode_from_files_to_check !show_tast !preexisting_warnings;
  set_mode_only_log_errors
    { log_file = !log_to_file; preexisting_warnings = !preexisting_warnings };
  let mode =
    (* Isolation options are meaningless for every other mode. *)
    match Option.value !mode ~default:MODE_STATUS with
    | MODE_ISOLATION _ ->
      let options = isolation_options () in
      (match
         Client_isolation_args.validate
           ~max_dependents_given:(Option.is_some !isolation_max_dependents)
           ~mode_flags_given:
             (List.filter_map
                [
                  ("--isolation-movable", !isolation_movable);
                  ("--isolation-move-to", Option.is_some !isolation_move_to);
                  ("--isolation-grow", !isolation_grow);
                ]
                ~f:(fun (flag, given) ->
                  if given then
                    Some flag
                  else
                    None))
           options
       with
      | Ok () -> ()
      | Error message -> Die.bad_args ~message ~usage:None);
      MODE_ISOLATION options
    | mode -> mode
  in
  (* fixups *)
  let (root, paths) =
    match (mode, args) with
    | (MODE_LINT, _)
    | (MODE_FILE_LEVEL_DEPENDENCIES, _)
    | (MODE_LOG_ERRORS _, _) ->
      (interpret_root [], args)
    | (_, _) -> (interpret_root args, [])
  in

  if !lock_file then (
    let lock_file_link = Server_files.lock_file root in
    Printf.printf "%s\n%!" lock_file_link;
    exit 0
  );

  if !ide_logname then (
    let ide_log_link = Server_files.client_ide_log root in
    Printf.printf "%s\n%!" ide_log_link;
    exit 0
  );

  if !lsp_logname then (
    let lsp_log_link = Server_files.client_lsp_log root in
    Printf.printf "%s\n%!" lsp_log_link;
    exit 0
  );

  if !monitor_logname then (
    let monitor_log_link = Server_files.monitor_log_link root in
    Printf.printf "%s\n%!" monitor_log_link;
    exit 0
  );

  if !client_logname then (
    let client_log_link = Server_files.client_log root in
    Printf.printf "%s\n%!" client_log_link;
    exit 0
  );

  if !logname then (
    let log_link = Server_files.log_link root in
    Printf.printf "%s\n%!" log_link;
    exit 0
  );

  if String.equal !from "emacs" then
    Printf.fprintf stdout "-*- mode: compilation -*-\n%!";

  let is_interactive = is_interactive !from in
  {
    Client_env.autostart = !autostart;
    config = !config;
    custom_hhi_path = !custom_hhi_path;
    custom_telemetry_data = !custom_telemetry_data;
    error_format = !error_format;
    force_dormant_start = !force_dormant_start;
    from = !from;
    show_spinner = Option.value ~default:is_interactive !show_spinner;
    ignore_hh_version = !ignore_hh_version;
    saved_state_ignore_hhconfig = !saved_state_ignore_hhconfig;
    paths;
    max_errors = !max_errors;
    preexisting_warnings = !preexisting_warnings;
    reason = !reason;
    mode;
    no_load =
      (!no_load
      ||
      match mode with
      | MODE_REMOVE_DEAD_FIXMES _ -> true
      | _ -> false);
    save_64bit = !save_64bit;
    save_human_readable_64bit_dep_map = !save_human_readable_64bit_dep_map;
    output_json = !output_json;
    output_jsonl = !output_jsonl;
    prechecked = !prechecked;
    mini_state = !mini_state;
    root;
    sort_results = !sort_results;
    stdin_name = !stdin_name;
    deadline = Option.map ~f:(fun t -> Unix.time () +. t) !timeout;
    watchman_debug_logging = !watchman_debug_logging;
    allow_non_opt_build = !allow_non_opt_build;
    desc = !desc;
    is_interactive;
    warning_switches = List.rev !warning_switches;
    dump_config = !dump_config;
  }

let parse_start_env command ~from_default =
  let usage =
    Printf.sprintf
      "Usage: %s %s [OPTION]... [WWW-ROOT]\n%s a Hack server\n\nWWW-ROOT is assumed to be current directory if unspecified\n"
      Sys.argv.(0)
      command
      (String.capitalize command)
  in
  let no_load = ref false in
  let watchman_debug_logging = ref false in
  let ignore_hh_version = ref false in
  let saved_state_ignore_hhconfig = ref false in
  let prechecked = ref None in
  let mini_state = ref None in
  let from = ref from_default in
  let config = ref [] in
  let custom_hhi_path = ref None in
  let custom_telemetry_data = ref [] in
  let allow_non_opt_build = ref false in
  let preexisting_warnings = ref false in
  let wait_deprecation_msg () =
    Printf.eprintf
      "WARNING: --wait is deprecated, does nothing, and will be going away soon!\n%!"
  in
  let options =
    [
      (* Please keep these sorted in the alphabetical order *)
      Common_argspecs.allow_non_opt_build allow_non_opt_build;
      Common_argspecs.config config;
      Common_argspecs.custom_hhi_path custom_hhi_path;
      Common_argspecs.custom_telemetry_data custom_telemetry_data;
      Common_argspecs.from from;
      Common_argspecs.ignore_hh_version ignore_hh_version;
      ("--no-load", Arg.Set no_load, " start from a fresh state");
      Common_argspecs.no_prechecked prechecked;
      Common_argspecs.prechecked prechecked;
      Common_argspecs.with_mini_state mini_state;
      ( "--profile-log",
        Arg.Unit (fun () -> config := ("profile_log", "true") :: !config),
        " enable profile logging" );
      Common_argspecs.saved_state_ignore_hhconfig saved_state_ignore_hhconfig;
      ( "--wait",
        Arg.Unit wait_deprecation_msg,
        " this flag is deprecated and does nothing!" );
      Common_argspecs.watchman_debug_logging watchman_debug_logging;
      Common_argspecs.preexisting_warnings preexisting_warnings;
      (* Please keep these sorted in the alphabetical order *)
    ]
  in
  let args = parse_without_command options usage command in
  let root = interpret_root args in
  {
    Client_start.config = !config;
    custom_hhi_path = !custom_hhi_path;
    custom_telemetry_data = !custom_telemetry_data;
    exit_on_failure = true;
    from = !from;
    ignore_hh_version = !ignore_hh_version;
    saved_state_ignore_hhconfig = !saved_state_ignore_hhconfig;
    save_64bit = None;
    save_human_readable_64bit_dep_map = None;
    no_load = !no_load;
    prechecked = !prechecked;
    mini_state = !mini_state;
    root;
    silent = false;
    watchman_debug_logging = !watchman_debug_logging;
    allow_non_opt_build = !allow_non_opt_build;
    preexisting_warnings = !preexisting_warnings;
  }

let parse_saved_state_project_metadata_args ~from_default =
  CSavedStateProjectMetadata
    (parse_check_args CKSavedStateProjectMetadata ~from_default)

let parse_start_args ~from_default =
  CStart (parse_start_env "start" ~from_default)

let parse_restart_args ~from_default =
  CRestart (parse_start_env "restart" ~from_default)

let parse_stop_args ~from_default =
  let usage =
    Printf.sprintf
      "Usage: %s stop [OPTION]... [WWW-ROOT]\nStop a hack server\n\nWWW-ROOT is assumed to be current directory if unspecified\n"
      Sys.argv.(0)
  in
  let from = ref from_default in
  let options = [Common_argspecs.from from] in
  let args = parse_without_command options usage "stop" in
  let root = interpret_root args in
  CStop { Client_stop.root; from = !from }

let parse_lsp_args () =
  let usage =
    Printf.sprintf
      "Usage: %s lsp [OPTION]...\nRuns a persistent language service\n"
      Sys.argv.(0)
  in
  let from = ref "" in
  let config = ref [] in
  let disable_format_on_save = ref false in
  let notebook_mode = ref false in
  let verbose = ref false in
  let ignore_hh_version = ref false in
  let naming_table = ref None in
  let warnings_saved_state_path = ref None in
  let options =
    [
      Common_argspecs.from from;
      Common_argspecs.config config;
      ( "--disable-format-on-save",
        Arg.Set disable_format_on_save,
        " disable `textDocument/willSaveWaitUntil` handling of format-on-save"
      );
      (* Please keep these sorted in the alphabetical order *)
      ("--enhanced-hover", Arg.Unit (fun () -> ()), " [legacy] no-op");
      ("--ffp-autocomplete", Arg.Unit (fun () -> ()), " [legacy] no-op");
      Common_argspecs.ignore_hh_version ignore_hh_version;
      Common_argspecs.naming_table naming_table;
      ( "--warnings-saved-state",
        Arg.String (fun s -> warnings_saved_state_path := Some (Path.make s)),
        " path to the warnings saved state" );
      ( "--notebook-mode",
        Arg.Set notebook_mode,
        " enable notebook mode, which is designed for use in notebooks. For example, this mode enables top-level statements."
      );
      ("--ranked-autocomplete", Arg.Unit (fun () -> ()), " [legacy] no-op");
      ("--serverless-ide", Arg.Unit (fun () -> ()), " [legacy] no-op");
      ( "--verbose",
        Arg.Set verbose,
        " verbose logs to stderr and `hh --ide-logname` and `--lsp-logname`" );
      (* Please keep these sorted in the alphabetical order *)
    ]
  in
  let args = parse_without_command options usage "lsp" in
  let root = interpret_root args in
  CLsp
    {
      Client_lsp.from = !from;
      config = !config;
      disable_format_on_save = !disable_format_on_save;
      ignore_hh_version = !ignore_hh_version;
      naming_table = !naming_table;
      warnings_saved_state_path = !warnings_saved_state_path;
      notebook_mode = !notebook_mode;
      verbose = !verbose;
      root_from_cli = root;
    }

let parse_rage_args () =
  let usage =
    Printf.sprintf "Usage: %s rage [OPTION]... [WWW-ROOT]\n" Sys.argv.(0)
  in
  let from = ref "" in
  let desc = ref None in
  let rageid = ref None in
  let lsp_log = ref None in
  let options =
    [
      Common_argspecs.from from;
      ("--desc", Arg.String (fun s -> desc := Some s), " description of problem");
      ( "--rageid",
        Arg.String (fun s -> rageid := Some s),
        " (optional) use this id, and finish even if parent process dies" );
      ( "--lsp-log",
        Arg.String (fun s -> lsp_log := Some s),
        " (optional) gather lsp logs from this filename" );
    ]
  in
  let args = parse_without_command options usage "rage" in
  let root = interpret_root args in
  (* hh_client normally handles Ctrl+C by printing an exception-stack.
     But for us, in an interactive prompt, Ctrl+C is an unexceptional way to quit. *)
  Sys_utils.set_signal Sys.sigint Sys.Signal_default;

  let desc =
    match !desc with
    | Some desc -> desc
    | None ->
      Printf.printf
        ("Sorry that hh isn't working. What's wrong?\n"
        ^^ "0. There's something wrong relating to VSCode or IDE\n"
        ^^ "1. hh_server takes ages to initialize\n"
        ^^ "2. hh is stuck in an infinite loop\n"
        ^^ "3. hh gives some error message about the monitor\n"
        ^^ "4. hack says it has an internal typecheck bug and asked me to report it\n"
        ^^ "5. hack is reporting errors that are clearly incorrect [please elaborate]\n"
        ^^ "6. I'm not sure how to write my code to avoid these hack errors\n"
        ^^ "7. hh says something about unsaved changes from an editor even after I've quit my editor\n"
        ^^ "8. something's wrong with hack VS Code or other editor\n"
        ^^ "[other] Please type either one of the above numbers, or a freeform description\n"
        ^^ "\nrage> %!");
      let response = In_channel.input_line_exn In_channel.stdin in
      let (response, info) =
        if String.equal response "0" then
          let () =
            Printf.printf
              "Please use the VSCode bug nub (at the right of the status bar) instead of hh rage.\n"
          in
          exit 0
        else if String.equal response "1" then
          ("hh_server slow initialize", `Verbose_hh_start)
        else if String.equal response "2" then
          ("hh stuck in infinite loop", `Verbose_hh_start)
        else if String.equal response "3" then
          ("hh monitor problem", `Verbose_hh_start)
        else if String.equal response "4" then begin
          Client_rage.verify_typechecker_err_src ();
          ("internal typecheck bug", `No_info)
        end else if String.equal response "5" then
          let () =
            Printf.printf
              "Please elaborate on which errors are incorrect...\nrage> %!"
          in
          (In_channel.input_line_exn In_channel.stdin, `No_info)
        else if String.equal response "6" then
          let () =
            Printf.printf
              ("Please ask in the appropriate support groups for advice on coding in Hack; "
              ^^ "`hh rage` is solely for reporting bugs in the tooling, not for reporting typechecker or "
              ^^ "language issues.\n")
          in
          exit 0
        else if String.equal response "7" then
          ("unsaved editor changes", `Unsaved)
        else if String.equal response "8" then
          let () =
            Printf.printf
              ("Please file the bug from within your editor to capture the right logs. "
              ^^ "Note: you can do Preferences > Settings > Hack > Verbose, then `pkill hh_client`, "
              ^^ "then reproduce the error, then file the bug. This way we'll get even richer logs.\n"
              )
          in
          exit 0
        else
          (response, `Verbose_hh_start)
      in
      begin
        match info with
        | `No_info -> ()
        | `Verbose_hh_start ->
          Printf.printf
            ("\nPOWER USERS ONLY: Sometimes the normal logging from hh_server isn't "
            ^^ "enough to diagnose an issue, and we'll ask you to switch hh_server to "
            ^^ "write verbose logs, then have you repro the issue, then use `hh rage` to "
            ^^ "gather up those now-verbose logs. To restart hh_server with verbose logs, "
            ^^ "do `hh stop && hh start --config min_log_level=debug`. "
            ^^ "Once done, then you can repro the issue, and then do rage again.\n\n%!"
            )
        | `Unsaved ->
          Printf.printf
            "\nNote: you can often work around this issue yourself by quitting your editor, then `pkill hh_client`.\n%!"
      end;
      response
  in
  CRage
    {
      Client_rage.root;
      from = !from;
      desc;
      rageid = !rageid;
      lsp_log = !lsp_log;
    }

let parse_decompress_zhhdg_args () =
  let usage =
    Printf.sprintf
      {|Usage: %s decompress-zhhdg --path [PATH]

Decompress a .zhhdg file by running the depgraph decompressor, and write a .hhdg file in the same directory.
|}
      Sys.argv.(0)
  in
  let from = ref "" in
  let path = ref "" in
  let options =
    Arg.align
      [
        Common_argspecs.from from;
        ( "--path",
          Arg.String (fun arg -> path := arg),
          " The path on disk to the .zhhdg file" );
      ]
  in
  let _args = parse_without_command options usage "decompress-zhhdg" in
  let path =
    match !path with
    | "" -> Die.bad_args ~message:"The '--path' option is required." ~usage:None
    | p -> p
  in
  let from = !from in
  CDecompressZhhdg { Client_decompress_zhhdg.path; from }

let parse_download_saved_state_args () =
  let usage =
    Printf.sprintf
      {|Usage: %s download-saved-state [OPTION]... [WWW-ROOT]

Download a saved-state to disk for the given repository, to make future
invocations of `hh` faster.|}
      Sys.argv.(0)
  in

  let from = ref "" in
  let should_save_replay = ref false in
  let replay_token = ref None in
  let saved_state_manifold_api_key = ref None in
  let options =
    Arg.align
      [
        Common_argspecs.from from;
        ("--type", Arg.String (fun _ -> ()), " (unused)");
        ( "--save-replay",
          Arg.Set should_save_replay,
          " Produce a token that can be later consumed by --replay-token to replay the same saved-state download."
        );
        ( "--saved-state-manifold-api-key",
          Arg.String (fun arg -> saved_state_manifold_api_key := Some arg),
          " An API key for Manifold to use when downloading the specified saved state."
        );
        ( "--replay-token",
          Arg.String (fun arg -> replay_token := Some arg),
          " A token produced from a previous invocation of this command with --save-replay."
        );
      ]
  in
  let args = parse_without_command options usage "download-saved-state" in
  let root =
    match args with
    | [] ->
      Die.bad_args
        ~message:"--download-saved-state requires a WWW_ROOT argument"
        ~usage:(Some usage)
    | _ -> interpret_root args
  in
  let from =
    match !from with
    | "" -> Die.bad_args ~message:"The '--from' option is required." ~usage:None
    | from -> from
  in
  CDownloadSavedState
    {
      Client_download_saved_state.root;
      from;
      saved_state_manifold_api_key = !saved_state_manifold_api_key;
      should_save_replay = !should_save_replay;
      replay_token = !replay_token;
    }

let parse_args ~(from_default : string) : command =
  Common_argspecs.from_for_bad_args := None;
  let command = parse_command () in
  try
    match command with
    | CKNone
    | CKCheck ->
      With_config (CCheck (parse_check_args CKCheck ~from_default))
    | CKStart -> With_config (parse_start_args ~from_default)
    | CKStop -> With_config (parse_stop_args ~from_default)
    | CKRestart -> With_config (parse_restart_args ~from_default)
    | CKLsp -> With_config (parse_lsp_args ())
    | CKRage -> With_config (parse_rage_args ())
    | CKSavedStateProjectMetadata ->
      With_config (parse_saved_state_project_metadata_args ~from_default)
    | CKDownloadSavedState -> With_config (parse_download_saved_state_args ())
    | CKDecompressZhhdg -> Without_config (parse_decompress_zhhdg_args ())
  with
  (* [Arg.parse_argv] raises rather than printing, so hh_client has to
     reproduce what [Arg.parse] used to do here. *)
  | Arg.Help message ->
    Printf.printf "%s" message;
    Stdlib.exit 0
  | Parse_error { message; usage } -> Die.bad_args ~message ~usage
  | Arg.Bad message -> Die.bad_args ~message ~usage:None

let root = function
  | CCheck { Client_env.root; _ }
  | CStart { Client_start.root; _ }
  | CRestart { Client_start.root; _ }
  | CStop { Client_stop.root; _ }
  | CRage { Client_rage.root; _ }
  | CSavedStateProjectMetadata { Client_env.root; _ }
  | CDownloadSavedState { Client_download_saved_state.root; _ } ->
    root
  | CLsp { Client_lsp.root_from_cli; _ } -> root_from_cli

let config = function
  | CCheck { Client_env.config; _ }
  | CStart { Client_start.config; _ }
  | CRestart { Client_start.config; _ }
  | CLsp { Client_lsp.config; _ }
  | CSavedStateProjectMetadata { Client_env.config; _ } ->
    Some config
  | CStop _
  | CDownloadSavedState _
  | CRage _ ->
    None

let from = function
  | CCheck { Client_env.from; _ }
  | CStart { Client_start.from; _ }
  | CRestart { Client_start.from; _ }
  | CLsp { Client_lsp.from; _ }
  | CSavedStateProjectMetadata { Client_env.from; _ }
  | CStop { Client_stop.from; _ }
  | CDownloadSavedState { Client_download_saved_state.from; _ }
  | CRage { Client_rage.from; _ } ->
    from

let dump_config = function
  | CCheck { Client_env.dump_config; _ } -> dump_config
  | _ -> false

let is_interactive cmd = from cmd |> is_interactive
