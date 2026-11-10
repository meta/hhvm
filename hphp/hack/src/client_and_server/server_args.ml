(*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude
include Cli_args

type saved_state_target = Saved_state_target_info of saved_state_target_info
[@@deriving show]

(*****************************************************************************)
(* The options from the command line *)
(*****************************************************************************)

type options = {
  ai_mode: Ai_options.t option;
  check_mode: bool;
  config: (string * string) list;
  custom_hhi_path: string option;
  custom_telemetry_data: (string * string) list;
  dump_fanout: bool;
  from: string;
  preexisting_warnings: bool;
      (** Whether to show preexisting warnings in typechecked files. *)
  ignore_hh_version: bool;
  saved_state_ignore_hhconfig: bool;
  json_mode: bool;
  max_procs: int option;
  no_load: bool;
  prechecked: bool option;
  root: Path.t;
  save_64bit: string option;
  save_human_readable_64bit_dep_map: string option;
  save_naming_filename: string option;
  should_detach: bool;
  (* AKA, daemon mode *)
  waiting_client: Unix.file_descr option;
  watchman_debug_logging: bool;
  with_saved_state: saved_state_target option;
  allow_non_opt_build: bool;
  write_symbol_info: string option;
}

(*****************************************************************************)
(* Usage code *)
(*****************************************************************************)
let usage = Printf.sprintf "Usage: %s [WWW DIRECTORY]\n" Sys.argv.(0)

(*****************************************************************************)
(* Options *)
(*****************************************************************************)

module Messages = struct
  let ai = " run ai with options"

  let check = " check and exit"

  let config = " override arbitrary value from hh.conf (format: <key>=<value>)"

  let custom_hhi_path = " use custom hhi files"

  let daemon = " detach process"

  let dump_fanout = " dump fanout to stdout as JSON, then exit"

  let enable_global_access_check =
    " run global access checker to check global writes and reads"

  let from = " so we know who's invoking - e.g. nuclide, vim, emacs, vscode"

  let from_vim = " DEPRECATED"

  let from_emacs = " DEPRECATED"

  let from_hhclient = " DEPRECATED"

  let ignore_hh_version = " ignore hh_version check when loading saved states"

  let saved_state_ignore_hhconfig =
    " ignore hhconfig hash when loading saved states"

  let json = " output errors in json format (arc lint mode)"

  let max_procs = " max numbers of workers"

  let no_load = " don't load from a saved state"

  let prechecked = " override value of \"prechecked_files\" flag from hh.conf"

  let profile_log = " enable profile logging"

  let save_naming = " save naming table to file"

  let save_64bit = " save discovered 64-bit to the given directory"

  let save_human_readable_64bit_dep_map =
    " save human readable map of 64bit hashes to names to files in the given directory"

  let waiting_client =
    " send message to fd/handle when server has begun"
    ^ " starting and again when it's done starting"

  let watchman_debug_logging =
    " Logs full Watchman requests and responses. This is very noisy"

  let with_saved_state =
    " Init with the given saved state instead of fetching it.\n"
    ^ "Expects a JSON string specified as: "
    ^ saved_state_json_descr

  let write_symbol_info = " write symbol info to json files"
end

let print_json_version () =
  print_endline @@ Yojson.Safe.to_string Hh_version.version_json

(*****************************************************************************)
(* The main entry point *)
(*****************************************************************************)

let parse_options () : options =
  let ai_mode = ref None in
  let check_mode = ref false in
  let config = ref [] in
  let custom_hhi_path = ref None in
  let custom_telemetry_data = ref [] in
  let dump_fanout = ref false in
  let from = ref "" in
  let from_emacs = ref false in
  let from_hhclient = ref false in
  let from_vim = ref false in
  let preexisting_warnings = ref false in
  let ignore_hh = ref false in
  let saved_state_ignore_hhconfig = ref false in
  let json_mode = ref false in
  let max_procs = ref None in
  let no_load = ref false in
  let prechecked = ref None in
  let root = ref "" in
  let save_64bit = ref None in
  let save_human_readable_64bit_dep_map = ref None in
  let save_naming = ref None in
  let should_detach = ref false in
  let version = ref false in
  let waiting_client = ref None in
  let watchman_debug_logging = ref false in
  let with_saved_state = ref None in
  let allow_non_opt_build = ref false in
  let write_symbol_info = ref None in
  let set_ai s = ai_mode := Some (Ai_options.prepare ~server:true s) in
  let set_max_procs n = max_procs := Some n in
  let set_save_naming s = save_naming := Some s in
  let set_wait fd = waiting_client := Some (Core_unix.File_descr.of_int fd) in
  let set_with_saved_state s = with_saved_state := Some s in
  let set_write_symbol_info s = write_symbol_info := Some s in
  let set_from s = from := s in
  let options =
    [
      ("--ai", Arg.String set_ai, Messages.ai);
      ("--allow-non-opt-build", Arg.Set allow_non_opt_build, "");
      ("--check", Arg.Set check_mode, Messages.check);
      ( "--config",
        Arg.String (fun s -> config := String_utils.split2_exn '=' s :: !config),
        Messages.config );
      ( "--custom-hhi-path",
        Arg.String (fun s -> custom_hhi_path := Some s),
        Messages.custom_hhi_path );
      ( "--custom-telemetry-data",
        Arg.String
          (fun s ->
            custom_telemetry_data :=
              String_utils.split2_exn '=' s :: !custom_telemetry_data),
        "Add a custom column to all logged telemetry samples (format: <column>=<value>)"
      );
      ("--daemon", Arg.Set should_detach, Messages.daemon);
      ("--dump-fanout", Arg.Set dump_fanout, Messages.dump_fanout);
      ( "--enable-global-access-check",
        Arg.Unit
          (fun () ->
            config := ("enable_global_access_check", "true") :: !config),
        Messages.enable_global_access_check );
      ("--from-emacs", Arg.Set from_emacs, Messages.from_emacs);
      ("--from-hhclient", Arg.Set from_hhclient, Messages.from_hhclient);
      ("--from-vim", Arg.Set from_vim, Messages.from_vim);
      ("--from", Arg.String set_from, Messages.from);
      ( "--preexisting-warnings",
        Arg.Set preexisting_warnings,
        " show all preexisting warnings in typechecked files (default: false)"
      );
      ("--ignore-hh-version", Arg.Set ignore_hh, Messages.ignore_hh_version);
      ("--json", Arg.Set json_mode, Messages.json);
      ("--max-procs", Arg.Int set_max_procs, Messages.max_procs);
      ("--no-load", Arg.Set no_load, Messages.no_load);
      ( "--no-prechecked",
        Arg.Unit (fun () -> prechecked := Some false),
        Messages.prechecked );
      ( "--prechecked",
        Arg.Unit (fun () -> prechecked := Some true),
        Messages.prechecked );
      ( "--profile-log",
        Arg.Unit (fun () -> config := ("profile_log", "true") :: !config),
        Messages.profile_log );
      ("--save-naming", Arg.String set_save_naming, Messages.save_naming);
      ( "--save-64bit",
        Arg.String (fun s -> save_64bit := Some s),
        Messages.save_64bit );
      ( "--save-human-readable-64bit-dep-map",
        Arg.String (fun s -> save_human_readable_64bit_dep_map := Some s),
        Messages.save_human_readable_64bit_dep_map );
      ( "--saved-state-ignore-hhconfig",
        Arg.Set saved_state_ignore_hhconfig,
        Messages.saved_state_ignore_hhconfig );
      ("--version", Arg.Set version, "");
      ("--waiting-client", Arg.Int set_wait, Messages.waiting_client);
      ( "--watchman-debug-logging",
        Arg.Set watchman_debug_logging,
        Messages.watchman_debug_logging );
      ( "--with-mini-state",
        Arg.String set_with_saved_state,
        Messages.with_saved_state );
      ( "--write-symbol-info",
        Arg.String set_write_symbol_info,
        Messages.write_symbol_info );
      ("-d", Arg.Set should_detach, Messages.daemon);
    ]
  in
  let options = Arg.align options in
  Arg.parse options (fun s -> root := s) usage;
  if !version then (
    if !json_mode then
      print_json_version ()
    else
      print_endline Hh_version.version;
    exit 0
  );
  (* --json, --write-symbol-info all imply check *)
  let check_mode =
    Option.is_some !write_symbol_info || !check_mode || !json_mode
  in
  if check_mode && Option.is_some !waiting_client then (
    Printf.eprintf "--check is incompatible with wait modes!\n";
    Exit.exit Exit_status.Input_error
  );
  let with_saved_state =
    match get_saved_state_spec !with_saved_state with
    | Ok (Some spec) -> Some (Saved_state_target_info spec)
    | Ok None -> None
    | Error message -> raise (Arg.Bad message)
  in
  (match !root with
  | "" ->
    Printf.eprintf "You must specify a root directory!\n";
    Exit.exit Exit_status.Input_error
  | _ -> ());
  let root_path = Path.make !root in
  Wwwroot.assert_www_directory root_path;
  {
    ai_mode = !ai_mode;
    check_mode;
    config = !config;
    custom_hhi_path = !custom_hhi_path;
    custom_telemetry_data = !custom_telemetry_data;
    dump_fanout = !dump_fanout;
    from = !from;
    preexisting_warnings = !preexisting_warnings;
    ignore_hh_version = !ignore_hh;
    saved_state_ignore_hhconfig = !saved_state_ignore_hhconfig;
    json_mode = !json_mode;
    max_procs = !max_procs;
    no_load = !no_load;
    prechecked = !prechecked;
    root = root_path;
    save_64bit = !save_64bit;
    save_human_readable_64bit_dep_map = !save_human_readable_64bit_dep_map;
    save_naming_filename = !save_naming;
    should_detach = !should_detach;
    waiting_client = !waiting_client;
    watchman_debug_logging = !watchman_debug_logging;
    with_saved_state;
    allow_non_opt_build = !allow_non_opt_build;
    write_symbol_info = !write_symbol_info;
  }

(* useful in testing code *)
let default_options ~root =
  {
    ai_mode = None;
    check_mode = false;
    config = [];
    custom_hhi_path = None;
    custom_telemetry_data = [];
    dump_fanout = false;
    from = "";
    preexisting_warnings = false;
    ignore_hh_version = false;
    saved_state_ignore_hhconfig = false;
    json_mode = false;
    max_procs = None;
    no_load = true;
    prechecked = None;
    root = Path.make root;
    save_64bit = None;
    save_human_readable_64bit_dep_map = None;
    save_naming_filename = None;
    should_detach = false;
    waiting_client = None;
    watchman_debug_logging = false;
    with_saved_state = None;
    allow_non_opt_build = false;
    write_symbol_info = None;
  }

(** Useful for executables which don't want to make a subscription to a
file-watching service. *)
let default_options_with_check_mode ~root =
  { (default_options ~root) with check_mode = true }

(*****************************************************************************)
(* Accessors *)
(*****************************************************************************)

let ai_mode options = options.ai_mode

let check_mode options = options.check_mode

let config options = options.config

let custom_hhi_path options = options.custom_hhi_path

let custom_telemetry_data options = options.custom_telemetry_data

let dump_fanout options = options.dump_fanout

let from options = options.from

let ignore_hh_version options = options.ignore_hh_version

let saved_state_ignore_hhconfig options = options.saved_state_ignore_hhconfig

let json_mode options = options.json_mode

let max_procs options = options.max_procs

let no_load options = options.no_load

let prechecked options = options.prechecked

let root options = options.root

let save_64bit options = options.save_64bit

let save_human_readable_64bit_dep_map options =
  options.save_human_readable_64bit_dep_map

let save_naming_filename options = options.save_naming_filename

let should_detach options = options.should_detach

let waiting_client options = options.waiting_client

let watchman_debug_logging options = options.watchman_debug_logging

let with_saved_state options = options.with_saved_state

let is_using_precomputed_saved_state options =
  match with_saved_state options with
  | Some (Saved_state_target_info _) -> true
  | None -> false

let allow_non_opt_build options = options.allow_non_opt_build

let write_symbol_info options = options.write_symbol_info

let preexisting_warnings options = options.preexisting_warnings

(*****************************************************************************)
(* Setters *)
(*****************************************************************************)

let set_ai_mode options ai_mode = { options with ai_mode }

let set_check_mode options check_mode = { options with check_mode }

let set_max_procs options procs = { options with max_procs = Some procs }

let set_no_load options is_no_load = { options with no_load = is_no_load }

let clear_precomputed_saved_state options =
  { options with with_saved_state = None }

let set_config options config = { options with config }

let set_from options from = { options with from }

let set_save_64bit options save_64bit = { options with save_64bit }

let set_save_naming_filename options save_naming_filename =
  { options with save_naming_filename }

(****************************************************************************)
(* Misc *)
(****************************************************************************)
let to_string
    {
      ai_mode;
      check_mode;
      config;
      custom_hhi_path;
      custom_telemetry_data;
      dump_fanout;
      from;
      preexisting_warnings;
      ignore_hh_version;
      saved_state_ignore_hhconfig;
      json_mode;
      max_procs;
      no_load;
      prechecked;
      root;
      save_64bit;
      save_human_readable_64bit_dep_map;
      save_naming_filename;
      should_detach;
      waiting_client;
      watchman_debug_logging;
      with_saved_state;
      allow_non_opt_build;
      write_symbol_info;
    } =
  let ai_mode_str =
    match ai_mode with
    | None -> "<>"
    | Some _ -> "Some(...)"
  in
  let saved_state_str =
    match with_saved_state with
    | None -> "<>"
    | Some _ -> "SavedStateTarget(...)"
  in
  let waiting_client_str =
    match waiting_client with
    | None -> "<>"
    | Some _ -> "WaitingClient(...)"
  in
  let custom_hhi_path_str =
    match custom_hhi_path with
    | None -> "<>"
    | Some path -> path
  in
  let save_64bit_str =
    match save_64bit with
    | None -> "<>"
    | Some path -> path
  in
  let save_human_readable_64bit_dep_map_str =
    match save_human_readable_64bit_dep_map with
    | None -> "<>"
    | Some path -> path
  in
  let save_naming_filename_str =
    match save_naming_filename with
    | None -> "<>"
    | Some path -> path
  in
  let prechecked_str =
    match prechecked with
    | None -> "<>"
    | Some b -> string_of_bool b
  in
  let max_procs_str =
    match max_procs with
    | None -> "<>"
    | Some n -> string_of_int n
  in
  let write_symbol_info_str =
    match write_symbol_info with
    | None -> "<>"
    | Some s -> s
  in
  let config_str =
    Printf.sprintf
      "[%s]"
      (String.concat ~sep:", "
      @@ List.map
           ~f:(fun (key, value) -> Printf.sprintf "%s=%s" key value)
           config)
  in
  let custom_telemetry_data_str =
    custom_telemetry_data
    |> List.map ~f:(fun (column, value) -> Printf.sprintf "%s=%s" column value)
    |> String.concat ~sep:", "
    |> Printf.sprintf "[%s]"
  in
  [
    "Server_args.options({";
    "ai_mode: ";
    ai_mode_str;
    ", ";
    "check_mode: ";
    string_of_bool check_mode;
    ", ";
    "config: ";
    config_str;
    "custom_hhi_path: ";
    custom_hhi_path_str;
    "custom_telemetry_data: ";
    custom_telemetry_data_str;
    "dump_fanout: ";
    string_of_bool dump_fanout;
    ", ";
    "from: ";
    from;
    ", ";
    "preexisting_warnings: ";
    string_of_bool preexisting_warnings;
    ", ";
    "ignore_hh_version: ";
    string_of_bool ignore_hh_version;
    ", ";
    "saved_state_ignore_hhconfig: ";
    string_of_bool saved_state_ignore_hhconfig;
    ", ";
    "json_mode: ";
    string_of_bool json_mode;
    ", ";
    "maxprocs: ";
    max_procs_str;
    ", ";
    "no_load: ";
    string_of_bool no_load;
    ", ";
    "prechecked: ";
    prechecked_str;
    ", ";
    "root: ";
    Path.to_string root;
    ", ";
    "save_64bit: ";
    save_64bit_str;
    ", ";
    "save_human_readable_64bit_dep_map: ";
    save_human_readable_64bit_dep_map_str;
    ", ";
    "save_naming_filename: ";
    save_naming_filename_str;
    ", ";
    "should_detach: ";
    string_of_bool should_detach;
    ", ";
    "waiting_client: ";
    waiting_client_str;
    ", ";
    "watchman_debug_logging: ";
    string_of_bool watchman_debug_logging;
    ", ";
    "with_saved_state: ";
    saved_state_str;
    ", ";
    "allow_non_opt_build: ";
    string_of_bool allow_non_opt_build;
    ", ";
    "write_symbol_info: ";
    write_symbol_info_str;
    ", ";
    "})";
  ]
  |> String.concat ~sep:""
