(*
 * Copyright (c) 2015, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude
open Ocaml_overrides
module SyntaxTree =
  Full_fidelity_syntax_tree.WithSyntax (Full_fidelity_positioned_syntax)

module SaveNamingResultPrinter = Client_result_printer.Make (struct
  type t = Save_state_service_types.save_naming_result

  let to_string t =
    Printf.sprintf
      "Files added: %d, symbols added: %d"
      t.Save_state_service_types.nt_files_added
      t.Save_state_service_types.nt_symbols_added

  let to_json t =
    `Assoc
      [
        ("files_added", `Int t.Save_state_service_types.nt_files_added);
        ("symbols_added", `Int t.Save_state_service_types.nt_symbols_added);
      ]
end)

let print_refs (results : Search_types.Find_refs.absolute list) ~(json : bool) :
    unit =
  if json then
    Find_refs_wire_format.HackAst.to_json results
    |> Yojson.Safe.to_string
    |> print_endline
  else
    Find_refs_wire_format.CliHumanReadable.print_results results

let print_find_my_tests_result result ~(json : bool) : unit =
  let module FMT = Server_command_types.Find_my_tests in
  if json then
    let result_json = FMT.yojson_of_result_data result in
    print_endline (Yojson.Safe.pretty_to_string result_json)
  else
    List.iter result.FMT.selected_test_files ~f:(fun file ->
        print_endline file.FMT.file_path)

(** [Input_error] when the run was refused: the caller asked for more than the
    limit allows and nothing was computed, so a zero exit would read as a run
    that found nothing. *)
let isolation_status (result : Server_isolation_types.result) =
  match result.Server_isolation_types.refused with
  | Some _ -> Exit_status.Input_error
  | None -> Exit_status.No_error

(** The line about clusters that stopped at the size cap. It says "It" of a lone
    cluster and "N of them" otherwise, and the verb agrees with the count. *)
let truncated_clusters_sentence ~total_clusters ~total_truncated =
  Printf.sprintf
    "%s stopped at the size cap and %s not complete.\n"
    (if total_clusters = 1 then
      "It"
    else
      Printf.sprintf "%d of them" total_truncated)
    (if total_truncated = 1 then
      "is"
    else
      "are")

let output_isolation_result
    (result : Server_isolation_types.result) ~output_json =
  let open Server_isolation_types in
  if output_json then
    `Assoc
      [
        ( "refused",
          match result.refused with
          | None -> `Null
          | Some reason -> `String reason );
        ( "movability",
          match result.movability with
          | None -> `Null
          | Some Movable -> `String "movable"
          | Some (Blocked _) -> `String "blocked" );
        ( "blockers",
          match result.movability with
          | Some (Blocked blockers) ->
            `List (List.map blockers ~f:(fun f -> `String f))
          | _ -> `Null );
        ( "summary",
          `Assoc
            [
              ("grown", `Bool result.grown);
              ("total_seeds", `Int result.total_seeds);
              ("total_isolatable_files", `Int result.total_isolatable_files);
              ("total_clusters", `Int result.total_clusters);
              ("total_truncated", `Int result.total_truncated);
              ("total_refused", `Int result.total_refused);
              ("largest_cluster", `Int result.largest_cluster);
            ] );
      ]
    |> Yojson.Safe.to_string
    |> print_endline
  else begin
    match (result.refused, result.movability) with
    | (Some reason, _) -> Printf.printf "Did not run: %s\n" reason
    | (None, Some Movable) ->
      Printf.printf
        "Movable: %s, nothing outside references in.\n"
        (Utils.print_count result.total_isolatable_files "file")
    | (None, Some (Blocked blockers)) ->
      Printf.printf
        "Blocked: referenced from %s outside.\n"
        (Utils.print_count (List.length blockers) "file");
      List.iter blockers ~f:(Printf.printf "  %s\n")
    | (None, None) ->
      Printf.printf
        "Started from %s.\n"
        (Utils.print_count result.total_seeds "file");
      (* Both modes report sets of files called clusters, and they mean different
         things, so the line a person reads has to say which. *)
      if result.grown then begin
        Printf.printf
          "Grew %s covering %s (largest: %d).\n"
          (Utils.print_count result.total_clusters "cluster")
          (Utils.print_count result.total_isolatable_files "file")
          result.largest_cluster;
        (* A truncated cluster stopped at the cap with more to take, so something
           outside it still references in. Saying how many kept that from being
           read as a count of finished packages. Read off the summary rather than
           counted here, so that it survives a run that streamed its clusters to a
           file and sent back none of them. *)
        if result.total_truncated > 0 then
          print_string
            (truncated_clusters_sentence
               ~total_clusters:result.total_clusters
               ~total_truncated:result.total_truncated)
      end else
        Printf.printf
          "Found %s without growing, covering %s in all; the largest has %s.\n"
          (Utils.print_count result.total_clusters "isolatable cluster")
          (Utils.print_count result.total_isolatable_files "file")
          (Utils.print_count result.largest_cluster "file");
      if result.total_refused > 0 then
        Printf.printf
          "Not in any cluster: %s (what depends on a file is over the limit, or the file declares nothing).\n"
          (Utils.print_count result.total_refused "starting file")
  end

let parse_name_or_member_id ~name_only_action ~name_and_member_action name =
  let pieces = Str.split (Str.regexp "::") name in
  let default_namespace str =
    match Str.first_chars str 1 with
    | "\\" -> str
    | _ -> "\\" ^ str
  in
  try
    match pieces with
    | class_name :: member_name :: _ ->
      name_and_member_action (default_namespace class_name) member_name
    | name :: _ -> name_only_action (default_namespace name)
    | _ -> raise Exit
  with
  | _ ->
    Printf.eprintf "Invalid input\n";
    raise Exit_status.(Exit_with Input_error)

let expand_path file =
  let path = Path.make file in
  if Path.file_exists path then
    Path.to_string path
  else
    let file = Filename.concat (Sys.getcwd ()) file in
    let path = Path.make file in
    if Path.file_exists path then
      Path.to_string path
    else (
      Printf.printf "File not found: %s\n" file;
      exit 2
    )

let expand_file_path file =
  let path = expand_path file in
  if Disk.is_directory path then begin
    Utils.prerr_endlinef
      "Path is a directory, only files are allowed: '%s'"
      file;
    raise Exit_status.(Exit_with Input_error)
  end;
  path

let parse_position_string ~(split_on : string) arg =
  let tpos = Str.split (Str.regexp split_on) arg in
  try
    match tpos with
    | [line; char] -> (int_of_string line, int_of_string char)
    | _ -> raise Exit
  with
  | _ ->
    Printf.eprintf "Invalid position\n";
    raise Exit_status.(Exit_with Input_error)

let connect
    ?(use_priority_pipe = false) ?(do_post_handoff_handshake = true) args :
    Client_connect.conn Lwt.t =
  let {
    Client_env.root;
    from;
    autostart;
    force_dormant_start;
    deadline;
    no_load;
    watchman_debug_logging;
    show_spinner;
    ignore_hh_version;
    save_64bit;
    save_human_readable_64bit_dep_map;
    saved_state_ignore_hhconfig;
    prechecked;
    mini_state;
    config;
    allow_non_opt_build;
    custom_hhi_path;
    custom_telemetry_data;
    preexisting_warnings;
    reason = _;
    error_format = _;
    paths = _;
    max_errors = _;
    mode = _;
    output_json = _;
    output_jsonl = _;
    sort_results = _;
    stdin_name = _;
    desc = _;
    is_interactive = _;
    warning_switches = _;
    dump_config = _;
  } =
    args
  in
  Client_connect.(
    connect
      {
        root;
        from;
        autostart;
        force_dormant_start;
        deadline;
        no_load;
        watchman_debug_logging;
        progress_callback =
          Client_spinner.report
            ~to_stderr:show_spinner
            ~angery_reaccs_only:(Client_messages.angery_reaccs_only ());
        do_post_handoff_handshake;
        ignore_hh_version;
        save_64bit;
        save_human_readable_64bit_dep_map;
        saved_state_ignore_hhconfig;
        use_priority_pipe;
        prechecked;
        mini_state;
        config;
        custom_hhi_path;
        custom_telemetry_data;
        allow_non_opt_build;
        preexisting_warnings;
      })

(* This is a function, because server closes the connection after each command,
 * so we need to be able to reconnect to retry. *)
type connect_fun = unit -> Client_connect.conn Lwt.t

let connect_then_close (args : Client_env.client_check_env) : unit Lwt.t =
  let%lwt Client_connect.{ channels = (_ic, oc); _ } =
    connect args ~use_priority_pipe:true ~do_post_handoff_handshake:false
  in
  Out_channel.close oc;
  (* The connection is derived from [Unix.open_connection]. Its docs explain:
     "The two channels returned by [open_connection] share a descriptor
      to a socket.  Therefore, when the connection is over, you should
      call {!Stdlib.close_out} on the output channel, which will also close
      the underlying socket.  Do not call {!Stdlib.close_in} on the input
      channel; it will be collected by the GC eventually." *)
  Lwt.return_unit

let rpc_with_connection
    (args : Client_env.client_check_env)
    (command : 'a Server_command_types.t)
    (call : connect_fun -> desc:string -> 'a Server_command_types.t -> 'b Lwt.t)
    : 'b Lwt.t =
  let use_priority_pipe = Server_command_types.use_priority_pipe command in
  let conn () = connect args ~use_priority_pipe in
  let%lwt result = call conn ~desc:args.desc @@ command in
  Lwt.return result

let rpc_with_retry
    (args : Client_env.client_check_env)
    (command : 'a Server_command_types.Done_or_retry.t Server_command_types.t) :
    'a Lwt.t =
  let%lwt result =
    rpc_with_connection args command Client_connect.rpc_with_retry
  in
  Lwt.return result

let rpc_with_retry_list
    (args : Client_env.client_check_env)
    (command :
      'a Server_command_types.Done_or_retry.t list Server_command_types.t) :
    'a list Lwt.t =
  let%lwt result =
    rpc_with_connection args command Client_connect.rpc_with_retry_list
  in
  Lwt.return result

let rpc
    (args : Client_env.client_check_env)
    (command : 'result Server_command_types.t) : ('result * Telemetry.t) Lwt.t =
  rpc_with_connection args command (fun conn_f ~desc command ->
      let%lwt conn = conn_f () in
      let%lwt (result, telemetry) = Client_connect.rpc conn ~desc command in
      Lwt.return (result, telemetry))

let parse_positions positions =
  List.map positions ~f:(fun pos ->
      try
        match Str.split (Str.regexp ":") pos with
        | [filename; line; char] ->
          (expand_path filename, int_of_string line, int_of_string char)
        | _ -> raise Exit
      with
      | _ ->
        Printf.eprintf "Invalid position\n";
        raise Exit_status.(Exit_with Input_error))

(* Filters and prints errors when a path is not a realpath *)
let filter_real_paths ~allow_directories paths =
  List.filter_map paths ~f:(fun fn ->
      match Sys_utils.realpath fn with
      | Some path ->
        if (not allow_directories) && Disk.is_directory fn then begin
          Utils.prerr_endlinef
            "Path is a directory, only files are allowed: '%s'"
            fn;
          None
        end else
          Some path
      | None ->
        Utils.prerr_endlinef "Could not find file '%s'" fn;
        None)

let main_internal
    (args : Client_env.client_check_env)
    (config : Server_config.t)
    (local_config : Server_local_config.t)
    (partial_telemetry_ref : Telemetry.t option ref) :
    (Exit_status.t * Telemetry.t) Lwt.t =
  match args.mode with
  | Client_env.MODE_STATUS ->
    let prechecked = Option.value args.prechecked ~default:true in
    let%lwt ((), telemetry1) =
      if prechecked then
        Lwt.return ((), Telemetry.create ())
      else
        rpc args Server_command_types.NO_PRECHECKED_FILES
    in
    let error_filter =
      Filter_diagnostics.Filter.make
        ~default_all:local_config.Server_local_config.warnings_default_all
        ~generated_files:
          (List.map
             ~f:Str.regexp
             (Server_config.warnings_generated_files config))
        args.warning_switches
    in
    (* We don't do streaming errors under [output_json]: our contract
       with the outside world is that if a caller uses [output_json] then they
       will never see [Exit_status.Typecheck_restarted], which streaming might show.

       We don't do streaming errors under [not prechecked]. That's because the
       [go_streaming] contract is to report on a typecheck that reflects all *file*
       changes up until now; it has no guarantee that the typecheck will reflects our
       preceding call to Server_command_types.NO_PRECHECKED_FILES. *)
    let use_streaming =
      local_config.Server_local_config.consume_streaming_errors
      && (not args.output_json)
      && prechecked
      && not (Sandcastle.is_sandcastle ())
    in
    if use_streaming then
      Client_check_status.go_streaming
        args
        local_config
        error_filter
        ~partial_telemetry_ref
        ~connect_then_close:(fun () -> connect_then_close args)
    else
      let%lwt (status, telemetry) =
        rpc
          args
          (Server_command_types.STATUS
             { max_errors = args.max_errors; error_filter })
      in
      let exit_status =
        Client_check_status.go
          status
          args.error_format
          ~output_json:args.output_json
          ~output_jsonl:args.output_jsonl
          ~max_errors:args.max_errors
          ~is_interactive:args.is_interactive
      in
      let telemetry =
        telemetry
        |> Telemetry.bool_ ~key:"streaming" ~value:false
        |> Telemetry.object_ ~key:"no_prechecked" ~value:telemetry1
        |> Telemetry.object_opt
             ~key:"last_recheck_stats"
             ~value:status.Server_command_types.Server_status.last_recheck_stats
      in
      Lwt.return (exit_status, telemetry)
  | Client_env.(
      MODE_STATUS_FILES { filenames; show_tast; preexisting_warnings }) ->
    let file_input filename =
      match filename with
      | "-" ->
        Server_command_types.FileContent (Sys_utils.read_stdin_to_string ())
      | _ -> Server_command_types.FileName (expand_file_path filename)
    in
    let file_inputs = List.map ~f:file_input filenames in
    let error_filter =
      Filter_diagnostics.Filter.make
        ~default_all:local_config.warnings_default_all
        ~generated_files:
          (List.map
             ~f:Str.regexp
             (Server_config.warnings_generated_files config))
        args.warning_switches
    in
    let%lwt (((error_list, dropped_count), tasts), telemetry) =
      rpc
        args
        (Server_command_types.STATUS_SINGLE
           {
             file_names = file_inputs;
             max_errors = args.max_errors;
             error_filter;
             return_expanded_tast = show_tast;
             preexisting_warnings;
           })
    in
    (match tasts with
    | None -> ()
    | Some tasts ->
      Printf.printf "TAST hashes:\n\n";
      Relative_path.Map.map tasts ~f:Tast.program_by_names
      |> Tast_hashes.hash_tasts_by_file
      |> Relative_path.Map.yojson_of_t Tast_hashes.yojson_of_by_names
      |> Yojson.Safe.pretty_to_channel Stdlib.stdout;
      Printf.printf
        "\n\n\nTASTs:\n\n%s\n%!"
        (Relative_path.Map.show (Tast_with_dynamic.pp Tast.pp_program) tasts);
      ());

    let status =
      {
        error_list;
        dropped_count;
        Server_command_types.Server_status.liveness =
          Server_command_types.Live_status;
        last_recheck_stats = None;
        file_watcher_clock = None;
      }
    in
    let exit_status =
      Client_check_status.go
        status
        args.error_format
        ~is_interactive:args.is_interactive
        ~output_json:args.output_json
        ~output_jsonl:args.output_jsonl
        ~max_errors:args.max_errors
    in
    Lwt.return (exit_status, telemetry)
  | Client_env.MODE_LOG_ERRORS { log_file; preexisting_warnings; _ } ->
    let files = filter_real_paths ~allow_directories:false args.paths in
    let error_filter =
      Filter_diagnostics.Filter.make
        ~default_all:local_config.warnings_default_all
        ~generated_files:
          (List.map
             ~f:Str.regexp
             (Server_config.warnings_generated_files config))
        args.warning_switches
    in
    let%lwt ((), telemetry) =
      rpc args
      @@ Server_command_types.LOG_ERRORS
           { files; log_file; error_filter; preexisting_warnings }
    in
    Lwt.return (Exit_status.No_error, telemetry)
  | Client_env.MODE_LIST_FILES ->
    let%lwt (infol, telemetry) =
      rpc args @@ Server_command_types.LIST_FILES_WITH_ERRORS
    in
    List.iter infol ~f:(Printf.printf "%s\n");
    Lwt.return (Exit_status.No_error, telemetry)
  | Client_env.MODE_FIND_CLASS_REFS name ->
    let%lwt results =
      rpc_with_retry args
      @@ Server_command_types.FIND_REFS
           (Server_command_types.Find_refs.Class name)
    in
    print_refs results ~json:args.output_json;
    Lwt.return (Exit_status.No_error, Telemetry.create ())
  | Client_env.MODE_FIND_REFS name ->
    let open Server_command_types.Find_refs in
    let pieces = Str.split (Str.regexp "|") name in
    let (kind, name) =
      match pieces with
      | [name] -> (None, name)
      | [kind; name] -> (Some kind, name)
      | _ ->
        Printf.eprintf "Invalid input\n";
        raise Exit_status.(Exit_with Input_error)
    in
    let action =
      parse_name_or_member_id
        ~name_and_member_action:(fun class_name member_name ->
          let member =
            match kind with
            | Some "Method"
            | None ->
              Method member_name
            | Some "Property" -> Property member_name
            | Some "Class_const" -> Class_const member_name
            | Some "Typeconst" -> Typeconst member_name
            | Some _ -> raise Exit_status.(Exit_with Input_error)
          in
          Member (class_name, member))
        ~name_only_action:(fun name ->
          match kind with
          | Some "Function"
          | None ->
            Function name
          | Some "Class" -> Class name
          | Some "ExplicitClass" -> ExplicitClass name
          | Some "GConst" -> GConst name
          | Some _ -> raise Exit_status.(Exit_with Input_error))
        name
    in
    let%lwt results =
      rpc_with_retry args @@ Server_command_types.FIND_REFS action
    in
    print_refs results ~json:args.output_json;
    Lwt.return (Exit_status.No_error, Telemetry.create ())
  | Client_env.MODE_GO_TO_IMPL_CLASS class_name ->
    let%lwt results =
      rpc_with_retry args
      @@ Server_command_types.GO_TO_IMPL
           (Server_command_types.Find_refs.Class class_name)
    in
    print_refs results ~json:args.output_json;
    Lwt.return (Exit_status.No_error, Telemetry.create ())
  | Client_env.MODE_GO_TO_IMPL_METHOD name ->
    let action =
      parse_name_or_member_id
        ~name_and_member_action:(fun class_name method_name ->
          Server_command_types.Find_refs.Member
            (class_name, Server_command_types.Find_refs.Method method_name))
        ~name_only_action:(fun fun_name ->
          Server_command_types.Find_refs.Function fun_name)
        name
    in
    (match action with
    | Server_command_types.Find_refs.Member _ ->
      let%lwt results =
        rpc_with_retry args @@ Server_command_types.GO_TO_IMPL action
      in
      print_refs results ~json:args.output_json;
      Lwt.return (Exit_status.No_error, Telemetry.create ())
    | _ ->
      Printf.eprintf "Invalid input\n";
      Lwt.return (Exit_status.Input_error, Telemetry.create ()))
  | Client_env.MODE_HACK_TO_NOTEBOOK ->
    let exit_status = Notebook_convert.hack_to_notebook () in
    Lwt.return (exit_status, Telemetry.create ())
  | Client_env.MODE_IDE_FIND_REFS_BY_SYMBOL arg ->
    let%lwt results =
      rpc_with_retry args @@ Server_command_types.IDE_FIND_REFS_BY_SYMBOL arg
    in
    Find_refs_wire_format.IdeShellout.to_string results |> print_endline;
    Lwt.return (Exit_status.No_error, Telemetry.create ())
  | Client_env.MODE_IDE_GO_TO_IMPL_BY_SYMBOL arg ->
    let%lwt results =
      rpc_with_retry args @@ Server_command_types.IDE_GO_TO_IMPL_BY_SYMBOL arg
    in
    Find_refs_wire_format.IdeShellout.to_string results |> print_endline;
    Lwt.return (Exit_status.No_error, Telemetry.create ())
  | Client_env.MODE_DUMP_SYMBOL_INFO files ->
    let%lwt conn = connect args in
    let%lwt () = Client_symbol_info.go conn ~desc:args.desc files expand_path in
    Lwt.return (Exit_status.No_error, Telemetry.create ())
  | Client_env.MODE_RENAME ((ref_mode : Client_env.rename_mode), before, after)
    ->
    let conn () = connect args in
    let%lwt () =
      Client_rename.go conn ~desc:args.desc args ref_mode ~before ~after
    in
    Lwt.return (Exit_status.No_error, Telemetry.create ())
  | Client_env.MODE_IDE_RENAME_BY_SYMBOL arg ->
    let open Server_command_types in
    let (new_name, action, symbol_definition) = Rename.string_to_args arg in
    let%lwt results =
      rpc_with_retry args
      @@ Server_command_types.IDE_RENAME_BY_SYMBOL
           (action, new_name, symbol_definition)
    in
    begin
      match results with
      | Ok patches ->
        Client_rename.go_ide_from_patches patches args.output_json;
        Lwt.return (Exit_status.No_error, Telemetry.create ())
      | Error _msg -> raise Exit_status.(Exit_with Input_error)
    end
  | Client_env.MODE_IDENTIFY_SYMBOL arg ->
    if not args.output_json then begin
      Printf.eprintf "Must use --json\n%!";
      raise Exit_status.(Exit_with Input_error)
    end;
    let%lwt (result, telemetry) =
      rpc args @@ Server_command_types.IDENTIFY_SYMBOL arg
    in
    let definition_to_json (d : string Symbol_definition.t) : Yojson.Safe.t =
      `Assoc
        [
          ("full_name", `String (Symbol_definition.full_name d));
          ("pos", d.Symbol_definition.pos |> Pos.json);
          ( "kind",
            d.Symbol_definition.kind |> Symbol_definition.string_of_kind
            |> fun s -> `String s );
        ]
    in
    let definitions = List.map result ~f:definition_to_json in
    Yojson.Safe.to_string (`List definitions) |> print_endline;
    Lwt.return (Exit_status.No_error, telemetry)
  | Client_env.MODE_IDENTIFY_SYMBOL1 arg
  | Client_env.MODE_IDENTIFY_SYMBOL2 arg
  | Client_env.MODE_IDENTIFY_SYMBOL3 arg ->
    let (line, char) = parse_position_string ~split_on:":" arg in
    let pos = File_content.Position.from_one_based line char in
    let file =
      match args.stdin_name with
      | None -> ""
      | Some f -> expand_path f
    in
    let content =
      Server_command_types.FileContent (Sys_utils.read_stdin_to_string ())
    in
    let%lwt (result, telemetry) =
      rpc args @@ Server_command_types.IDENTIFY_FUNCTION (file, content, pos)
    in
    Client_get_definition.go result args.output_json;
    Lwt.return (Exit_status.No_error, telemetry)
  | Client_env.MODE_TYPE_AT_POS arg ->
    let tpos = Str.split (Str.regexp ":") arg in
    let (fn, line, char) =
      try
        match tpos with
        | [filename; line; char] ->
          let fn = expand_path filename in
          ( Server_command_types.FileName fn,
            int_of_string line,
            int_of_string char )
        | [line; char] ->
          let content = Sys_utils.read_stdin_to_string () in
          ( Server_command_types.FileContent content,
            int_of_string line,
            int_of_string char )
        | _ -> raise Exit
      with
      | _ ->
        Printf.eprintf "Invalid position\n";
        raise Exit_status.(Exit_with Input_error)
    in
    let pos = File_content.Position.from_one_based line char in
    let%lwt (ty, telemetry) =
      rpc args @@ Server_command_types.INFER_TYPE (fn, pos)
    in
    Client_type_at_pos.go ty args.output_json;
    Lwt.return (Exit_status.No_error, telemetry)
  | Client_env.MODE_INFER_DYNAMIC (arg, as_data) ->
    let%lwt (json, telemetry) =
      rpc args @@ Server_command_types.INFER_DYNAMIC (arg, as_data)
    in
    Printf.printf "%s\n" (Yojson.Safe.pretty_to_string json);
    Lwt.return (Exit_status.No_error, telemetry)
  | Client_env.MODE_ENFORCEMENT_AT_POS_BATCH positions ->
    let positions =
      List.map positions ~f:(fun pos ->
          try
            match Str.split (Str.regexp ":") pos with
            | [filename; line; char] ->
              ( expand_path filename,
                File_content.Position.from_one_based
                  (int_of_string line)
                  (int_of_string char) )
            | _ -> raise Exit
          with
          | _ ->
            Printf.eprintf "Invalid position\n";
            raise Exit_status.(Exit_with Input_error))
    in
    let%lwt (responses, telemetry) =
      rpc args @@ Server_command_types.ENFORCEMENT_AT_POS_BATCH positions
    in
    List.iter responses ~f:print_endline;
    Lwt.return (Exit_status.No_error, telemetry)
  | Client_env.MODE_TYPE_AT_POS_BATCH positions ->
    let positions =
      List.map positions ~f:(fun pos ->
          try
            match Str.split (Str.regexp ":") pos with
            | [filename; line; char] ->
              ( expand_path filename,
                File_content.Position.from_one_based
                  (int_of_string line)
                  (int_of_string char),
                None )
            | [filename; start_line; start_char; end_line; end_char] ->
              let filename = expand_path filename in
              let start_line = int_of_string start_line in
              let start_char = int_of_string start_char in
              let end_line = int_of_string end_line in
              let end_char = int_of_string end_char in
              let start_pos =
                File_content.Position.from_one_based start_line start_char
              in
              let end_pos =
                File_content.Position.from_one_based end_line end_char
              in
              (filename, start_pos, Some end_pos)
            | _ -> raise Exit
          with
          | _ ->
            Printf.eprintf "Invalid position\n";
            raise Exit_status.(Exit_with Input_error))
    in
    let%lwt (responses, telemetry) =
      rpc args @@ Server_command_types.INFER_TYPE_BATCH positions
    in
    List.iter responses ~f:print_endline;
    Lwt.return (Exit_status.No_error, telemetry)
  | Client_env.MODE_IS_SUBTYPE ->
    let stdin = Sys_utils.read_stdin_to_string () in
    let%lwt (response, telemetry) =
      rpc args @@ Server_command_types.IS_SUBTYPE stdin
    in
    (match response with
    | Ok str ->
      Printf.printf "%s" str;
      Lwt.return (Exit_status.No_error, telemetry)
    | Error str ->
      Printf.eprintf "%s" str;
      raise Exit_status.(Exit_with Input_error))
  | Client_env.MODE_TYPE_ERROR_AT_POS arg ->
    let tpos = Str.split (Str.regexp ":") arg in
    let (fn, line, char) =
      try
        match tpos with
        | [filename; line; char] ->
          let fn = expand_path filename in
          ( Server_command_types.FileName fn,
            int_of_string line,
            int_of_string char )
        | [line; char] ->
          let content = Sys_utils.read_stdin_to_string () in
          ( Server_command_types.FileContent content,
            int_of_string line,
            int_of_string char )
        | _ -> raise Exit
      with
      | _ ->
        Printf.eprintf
          "Invalid position; expected an argument of the form [filename]:[line]:[column] or [line]:[column]\n";
        raise Exit_status.(Exit_with Input_error)
    in
    let%lwt (ty, telemetry) =
      rpc args @@ Server_command_types.INFER_TYPE_ERROR (fn, line, char)
    in
    Client_type_error_at_pos.go ty args.output_json;
    Lwt.return (Exit_status.No_error, telemetry)
  | Client_env.MODE_TAST_HOLES arg ->
    let parse_hole_filter = function
      | "any" -> Some Server_command_types.Tast_hole.Any
      | "typing" -> Some Server_command_types.Tast_hole.Typing
      | "cast" -> Some Server_command_types.Tast_hole.Cast
      | _ -> None
    in

    let (filename, hole_src_opt) =
      try
        match Str.(split (regexp ":") arg) with
        | [filename; filter_str] ->
          let fn = expand_path filename in
          (match parse_hole_filter filter_str with
          | Some filter -> (Server_command_types.FileName fn, filter)
          | _ -> raise Exit)
        | [part] ->
          (match parse_hole_filter part with
          | Some src_opt ->
            let content = Sys_utils.read_stdin_to_string () in
            (Server_command_types.FileContent content, src_opt)
          | _ ->
            let fn = expand_path part in
            (* No hole source specified; default to `Typing` *)
            ( Server_command_types.FileName fn,
              Server_command_types.Tast_hole.Typing ))
        | _ -> raise Exit
      with
      | Exit ->
        Printf.eprintf
          "Invalid argument; expected an argument of the form [filename](:[any|typing|cast])? or [any|typing|cast]\n";
        raise Exit_status.(Exit_with Input_error)
      | exn ->
        let e = Exception.wrap exn in
        Printf.eprintf
          "An unexpected error occurred: %s"
          (Exception.get_ctor_string e);
        Exception.reraise e
    in
    let%lwt (ty, telemetry) =
      rpc args @@ Server_command_types.TAST_HOLES (filename, hole_src_opt)
    in
    Client_tast_holes.go ty ~print_file:false args.output_json;
    Lwt.return (Exit_status.No_error, telemetry)
  | Client_env.MODE_TAST_HOLES_BATCH (file : string) ->
    let files =
      expand_path file
      |> Sys_utils.read_file
      |> Bytes.to_string
      |> String.strip
      |> String.split ~on:'\n'
      |> List.map ~f:expand_path
    in
    let%lwt (holes, telemetry) =
      rpc args @@ Server_command_types.TAST_HOLES_BATCH files
    in
    Client_tast_holes.go holes ~print_file:true args.output_json;
    Lwt.return (Exit_status.No_error, telemetry)
  | Client_env.MODE_FUN_DEPS_AT_POS_BATCH positions ->
    let positions = parse_positions positions in
    let%lwt (responses, telemetry) =
      rpc args @@ Server_command_types.FUN_DEPS_BATCH positions
    in
    List.iter responses ~f:print_endline;
    Lwt.return (Exit_status.No_error, telemetry)
  | Client_env.MODE_DEPS_OUT_AT_POS_BATCH positions ->
    let positions = parse_positions positions in
    let%lwt (responses, telemetry) =
      rpc args @@ Server_command_types.DEPS_OUT_BATCH positions
    in
    List.iter responses ~f:print_endline;
    Lwt.return (Exit_status.No_error, telemetry)
  | Client_env.MODE_OUTLINE
  | Client_env.MODE_OUTLINE2 ->
    let (_handle : Shared_mem.handle) =
      Shared_mem.init ~num_workers:0 Shared_mem.default_config
    in
    let content = Sys_utils.read_stdin_to_string () in
    let results =
      File_outline.outline
        (*
         * TODO: Don't use default parser options.
         *
         * Parser options enables certain features (such as namespace aliasing)
         * Thus, for absolute correctness of outlining, we need to use the same
         * parser options that the server uses. But this client request doesn't
         * hit the server at all. So either change this to a server RPC, or
         * ask the server what its parser options are, or parse the
         * options from the .hhconfig file (needs to be the same hhconfig file the
         * server used).
         * *)
        Parser_options.default
        content
    in
    Client_outline.go results args.output_json;
    Lwt.return (Exit_status.No_error, Telemetry.create ())
  | Client_env.MODE_OUTLINE_FOR_AGENTS path ->
    let content = Sys_utils.cat path in
    let result = Outline_for_agents.outline content in
    print_string result;
    Lwt.return (Exit_status.No_error, Telemetry.create ())
  | Client_env.MODE_METHOD_JUMP_CHILDREN class_ ->
    let filter = Server_command_types.Method_jumps.No_filter in
    let%lwt (results, telemetry) =
      rpc args @@ Server_command_types.METHOD_JUMP (class_, filter, true)
    in
    Client_method_jumps.go results true args.output_json;
    Lwt.return (Exit_status.No_error, telemetry)
  | Client_env.MODE_METHOD_JUMP_ANCESTORS (class_, filter) ->
    let filter =
      match Method_jumps.string_filter_to_method_jump_filter filter with
      | Some filter -> filter
      | None ->
        Printf.eprintf "Invalid method jump filter %s\n" filter;
        raise Exit_status.(Exit_with Input_error)
    in
    let%lwt (results, telemetry) =
      rpc args @@ Server_command_types.METHOD_JUMP (class_, filter, false)
    in
    Client_method_jumps.go results false args.output_json;
    Lwt.return (Exit_status.No_error, telemetry)
  | Client_env.MODE_METHOD_JUMP_ANCESTORS_BATCH (classes, filter) ->
    let filter =
      match Method_jumps.string_filter_to_method_jump_filter filter with
      | Some filter -> filter
      | None ->
        Printf.eprintf "Invalid method jump filter %s\n" filter;
        raise Exit_status.(Exit_with Input_error)
    in
    let%lwt (results, telemetry) =
      rpc args @@ Server_command_types.METHOD_JUMP_BATCH (classes, filter)
    in
    Client_method_jumps.go results false args.output_json;
    Lwt.return (Exit_status.No_error, telemetry)
  | Client_env.MODE_NOTEBOOK_TO_HACK { notebook_number; notebook_header } ->
    let exit_status =
      Notebook_convert.notebook_to_hack ~notebook_number ~header:notebook_header
    in
    Lwt.return (exit_status, Telemetry.create ())
  | Client_env.MODE_IN_MEMORY_DEP_TABLE_SIZE ->
    let%lwt (result, telemetry) =
      rpc args @@ Server_command_types.IN_MEMORY_DEP_TABLE_SIZE
    in
    Client_result_printer.Int_printer.go result args.output_json;
    Lwt.return (Exit_status.No_error, telemetry)
  | Client_env.MODE_SAVE_NAMING path ->
    let () = Sys_utils.mkdir_p (Filename.dirname path) in
    let path = Path.make path in
    let%lwt (result, telemetry) =
      rpc args @@ Server_command_types.SAVE_NAMING (Path.to_string path)
    in
    SaveNamingResultPrinter.go result args.output_json;
    Lwt.return (Exit_status.No_error, telemetry)
  | Client_env.MODE_PAUSE ->
    let%lwt ((), telemetry) = rpc args Server_command_types.PAUSE in
    print_endline
      "Paused automatic rechecks. The next client request resumes them.";
    Lwt.return (Exit_status.No_error, telemetry)
  | Client_env.MODE_SEARCH query ->
    if not (String.equal query "this_is_just_to_check_liveness_of_hh_server")
    then begin
      prerr_endline
        "Usage: hh --search this_is_just_to_check_liveness_of_hh_server";
      Lwt.return (Exit_status.Input_error, Telemetry.create ())
    end else begin
      let%lwt ((), telemetry) =
        rpc args @@ Server_command_types.CHECK_LIVENESS
      in
      if args.output_json then print_endline "[]";
      Lwt.return (Exit_status.No_error, telemetry)
    end
  | Client_env.MODE_LINT ->
    let fnl = filter_real_paths ~allow_directories:false args.paths in
    begin
      match args.paths with
      | [] ->
        prerr_endline "No lint errors (0 files checked)!";
        prerr_endline "Note: --lint expects a list of filenames to check.";
        Lwt.return (Exit_status.No_error, Telemetry.create ())
      | _ ->
        let%lwt (results, telemetry) =
          rpc args @@ Server_command_types.LINT fnl
        in
        let error_format = Diagnostics.format_or_default args.error_format in
        Client_lint.go results args.output_json error_format;
        Lwt.return (Exit_status.No_error, telemetry)
    end
  | Client_env.MODE_SERVER_RAGE ->
    let open Server_rage_types in
    if not args.output_json then begin
      Printf.eprintf "Must use --json\n%!";
      raise Exit_status.(Exit_with Input_error)
    end;
    (* Our json output format is read by clientRage.ml *)
    let make_item { title; data } =
      `Assoc [("name", `String title); ("contents", `String data)]
    in
    let%lwt (items, telemetry) = rpc args Server_command_types.RAGE in
    Yojson.Safe.to_string (`List (List.map items ~f:make_item)) |> print_endline;
    Lwt.return (Exit_status.No_error, telemetry)
  | Client_env.MODE_LINT_STDIN filename -> begin
    match Sys_utils.realpath filename with
    | None ->
      Utils.prerr_endlinef "Could not find file '%s'" filename;
      Lwt.return (Exit_status.Input_error, Telemetry.create ())
    | Some filename ->
      let contents = Sys_utils.read_stdin_to_string () in
      let%lwt (results, telemetry) =
        rpc args
        @@ Server_command_types.LINT_STDIN
             { Server_command_types.filename; contents }
      in
      let error_format = Diagnostics.format_or_default args.error_format in
      Client_lint.go results args.output_json error_format;
      Lwt.return (Exit_status.No_error, telemetry)
  end
  | Client_env.MODE_LINT_ALL code ->
    let%lwt (results, telemetry) =
      rpc args @@ Server_command_types.LINT_ALL code
    in
    let error_format = Diagnostics.format_or_default args.error_format in
    Client_lint.go results args.output_json error_format;
    Lwt.return (Exit_status.No_error, telemetry)
  | Client_env.MODE_STATS ->
    let%lwt (stats, telemetry) = rpc args @@ Server_command_types.STATS in
    print_string @@ Yojson.Safe.pretty_to_string (Stats.to_json stats);
    Lwt.return (Exit_status.No_error, telemetry)
  | Client_env.MODE_REMOVE_DEAD_FIXMES codes ->
    let%lwt conn = connect args in
    let%lwt (response, telemetry) =
      Client_connect.rpc conn ~desc:args.desc
      @@ Server_command_types.REMOVE_DEAD_FIXMES codes
    in
    begin
      match response with
      | `Error msg ->
        Printf.eprintf "%s\n" msg;
        Lwt.return (Exit_status.Type_error, telemetry)
      | `Ok patches ->
        if args.output_json then
          Client_rename.print_patches_json patches
        else
          Client_rename.apply_patches patches;
        Lwt.return (Exit_status.No_error, telemetry)
    end
  | Client_env.MODE_REMOVE_DEAD_UNSAFE_CASTS ->
    let error_filter =
      Filter_diagnostics.Filter.make
        ~default_all:local_config.Server_local_config.warnings_default_all
        ~generated_files:
          (List.map
             ~f:Str.regexp
             (Server_config.warnings_generated_files config))
        args.warning_switches
    in
    let status_cmd =
      Server_command_types.STATUS { max_errors = args.max_errors; error_filter }
    in
    let rec go () =
      let%lwt (response, telemetry) =
        rpc args @@ Server_command_types.REMOVE_DEAD_UNSAFE_CASTS
      in
      match response with
      | `Error msg ->
        Printf.eprintf "%s\n" msg;
        Lwt.return (Exit_status.Type_error, telemetry)
      | `Ok patches ->
        Client_rename.apply_patches patches;
        if List.is_empty patches then
          Lwt.return (Exit_status.No_error, telemetry)
        else
          let%lwt _ = rpc args status_cmd in
          go ()
    in
    go ()
  | Client_env.MODE_REWRITE_DECLARATIONS ->
    (*
    * HHVM uses existence of this file to indicate it is paused, which
    * happens when HHVM is at a breakpoint.
    * Is also read by the language server:
    * https://www.internalfb.com/code/fbsource/[248c997e7acd028f9980cc9f82113106cc56b63d]/fbcode/hphp/hack/src/client/clientLsp.ml. *)
    let file_whose_existence_indicates_hhvm_is_paused =
      Option.map (Sys.getenv_opt "HOME") ~f:(fun home_dir ->
          home_dir ^ "/.vscode-sockets/hhvm-paused")
    in
    Declarations_rewriter.start
      (Random.State.make_self_init ())
      ~file_whose_existence_indicates_hhvm_is_paused;
    Lwt.return (Exit_status.No_error, Telemetry.create ())
  | Client_env.MODE_REWRITE_LAMBDA_PARAMETERS files ->
    let%lwt conn = connect args in
    let%lwt (patches, telemetry) =
      Client_connect.rpc conn ~desc:args.desc
      @@ Server_command_types.REWRITE_LAMBDA_PARAMETERS files
    in
    if args.output_json then
      Client_rename.print_patches_json patches
    else
      Client_rename.apply_patches patches;
    Lwt.return (Exit_status.No_error, telemetry)
  | Client_env.MODE_FULL_FIDELITY_PARSE file ->
    (* We can cheaply do this on the client today, but we might want to
       do it on the server and cache the results in the future. *)
    let do_it_on_server = false in
    let%lwt (results, telemetry) =
      if do_it_on_server then
        rpc args @@ Server_command_types.DUMP_FULL_FIDELITY_PARSE file
      else
        let file = Relative_path.create Relative_path.Dummy file in
        let source_text = Full_fidelity_source_text.from_file file in
        let syntax_tree = SyntaxTree.make source_text in
        let json = SyntaxTree.to_json syntax_tree in
        Lwt.return (Yojson.Safe.to_string json, Telemetry.create ())
    in
    Client_full_fidelity_parse.go results;
    Lwt.return (Exit_status.No_error, telemetry)
  | Client_env.MODE_FULL_FIDELITY_SCHEMA ->
    let schema = Full_fidelity_schema.schema_as_json () in
    print_string schema;
    Lwt.return (Exit_status.No_error, Telemetry.create ())
  | Client_env.MODE_CST_SEARCH files_to_search ->
    let sort_results = args.sort_results in
    let input = Sys_utils.read_stdin_to_string () |> Yojson.Safe.from_string in
    let%lwt (result, telemetry) =
      rpc args
      @@ Server_command_types.CST_SEARCH
           { Server_command_types.sort_results; input; files_to_search }
    in
    begin
      match result with
      | Ok result ->
        print_endline (Yojson.Safe.to_string result);
        Lwt.return (Exit_status.No_error, telemetry)
      | Error error ->
        print_endline error;
        raise Exit_status.(Exit_with Input_error)
    end
  | Client_env.MODE_FILE_LEVEL_DEPENDENCIES ->
    let paths = filter_real_paths ~allow_directories:true args.paths in
    let%lwt (responses, telemetry) =
      rpc args @@ Server_command_types.FILE_DEPENDENTS paths
    in
    if args.output_json then begin
      let json_path_list = List.map responses ~f:(fun path -> `String path) in
      let output = `Assoc [("dependents", `List json_path_list)] in
      print_endline @@ Yojson.Safe.to_string output
    end else
      List.iter responses ~f:(Printf.printf "%s\n");
    Lwt.return (Exit_status.No_error, telemetry)
  | Client_env.MODE_ISOLATION
      ({
         Server_isolation_types.mode = Server_isolation_types.Move_to parent;
         _;
       } as options) ->
    let error_filter =
      Filter_diagnostics.Filter.make
        ~default_all:local_config.Server_local_config.warnings_default_all
        ~generated_files:
          (List.map
             ~f:Str.regexp
             (Server_config.warnings_generated_files config))
        args.warning_switches
    in
    let%lwt exit_status =
      Client_isolation_move.move_into
        args
        ~connect:(fun ~use_priority_pipe -> connect args ~use_priority_pipe)
        ~options
        ~error_filter
        ~parent
    in
    Lwt.return (exit_status, Telemetry.create ())
  | Client_env.MODE_ISOLATION (options : Server_isolation_types.options) ->
    (* Clusters are written by the server, never returned: there can be
       hundreds of thousands. With [--json] and no file of the caller's own,
       they go to a temporary one that is printed and removed here, so stdout
       costs no more memory than a file does. *)
    let temp_file =
      match (options.clusters_destination, args.output_json) with
      | (Dropped, true) ->
        Some (Stdlib.Filename.temp_file "hh_isolation" ".jsonl")
      | _ -> None
    in
    let options =
      match temp_file with
      | Some path -> { options with clusters_destination = To_file path }
      | None -> options
    in
    (* [Lwt.finalize] rather than wrapping only the printing: the file exists
       before the query is sent, so a query that raises must still clean it
       up. *)
    Lwt.finalize
      (fun () ->
        let%lwt (result, telemetry) =
          rpc args (Server_command_types.FIND_ISOLATABLE_CLUSTERS options)
        in
        (* A line at a time. Reading the file into a list first would hold
           every cluster in memory, which is the cost this avoids. *)
        (match temp_file with
        | Some path ->
          In_channel.with_file path ~f:(fun ic ->
              In_channel.iter_lines ic ~f:print_endline)
        | None -> ());
        output_isolation_result result ~output_json:args.output_json;
        Lwt.return (isolation_status result, telemetry))
      (fun () ->
        (match temp_file with
        | Some path ->
          (try Sys.remove path with
          | _ -> ())
        | None -> ());
        Lwt.return_unit)
  | Client_env.MODE_VALIDATE_ISOLATION list_file ->
    (* Read here rather than on the server: the list file is a path on the
       caller's machine, and the server only ever sees repo-relative paths. *)
    let files =
      match Sys_utils.cat_or_failed list_file with
      | None ->
        Printf.eprintf "Could not read %s\n" list_file;
        raise Exit_status.(Exit_with Input_error)
      | Some contents ->
        String.split_lines contents
        |> List.filter_map ~f:(fun line ->
               match String.strip line with
               | "" -> None
               | path -> Some path)
    in
    if List.is_empty files then begin
      Printf.eprintf
        "%s names no files. --validate-isolation takes a file of repo-relative paths, one per line.\n"
        list_file;
      raise Exit_status.(Exit_with Input_error)
    end;
    let%lwt (result, telemetry) =
      rpc args (Server_command_types.VALIDATE_ISOLATION files)
    in
    Client_isolation_validate.output result ~output_json:args.output_json;
    Lwt.return (Client_isolation_validate.status result, telemetry)
  | Client_env.MODE_VERBOSE verbose ->
    let%lwt ((), telemetry) =
      rpc args @@ Server_command_types.VERBOSE verbose
    in
    Lwt.return (Exit_status.No_error, telemetry)
  | Client_env.MODE_DEPS_IN_AT_POS_BATCH positions ->
    let positions = parse_positions positions in
    let%lwt results =
      rpc_with_retry_list args @@ Server_command_types.DEPS_IN_BATCH positions
    in
    List.iter results ~f:(fun s -> print_refs s ~json:true);
    Lwt.return (Exit_status.No_error, Telemetry.create ())
  | Client_env.MODE_FIND_MY_TESTS path ->
    let open Server_command_types.Find_my_tests in
    let parse_symbol symbol =
      let pieces = Str.split (Str.regexp "|") symbol in
      let (kind, name) =
        match pieces with
        | [name] -> (None, name)
        | [kind; name] -> (Some kind, name)
        | _ ->
          Printf.eprintf "Invalid input\n";
          raise Exit_status.(Exit_with Input_error)
      in
      let (kind, soft) =
        match kind with
        | Some s ->
          (match String.chop_suffix s ~suffix:"@soft" with
          | Some base -> (Some base, true)
          | None -> (Some s, false))
        | None -> (None, false)
      in
      let action_kind =
        parse_name_or_member_id
          ~name_and_member_action:(fun class_name member_name ->
            match kind with
            | Some "Method"
            | None ->
              Method { class_name; member_name }
            | Some "Typeconst" -> Typeconst { class_name; member_name }
            | Some "Class_const" -> Class_const { class_name; member_name }
            | Some _ -> raise Exit_status.(Exit_with Input_error))
          ~name_only_action:(fun name ->
            match kind with
            | Some "Class" -> Class { class_name = name }
            | Some "Typedef" -> Typedef { name }
            | Some _
            | None ->
              raise Exit_status.(Exit_with Input_error))
          name
      in
      { kind = action_kind; soft }
    in
    let json_content = Sys_utils.read_file path |> Bytes.to_string in
    let input =
      try json_input_of_yojson (Yojson.Safe.from_string json_content) with
      | exn ->
        Printf.eprintf "Failed to parse JSON: %s\n" (Exn.to_string exn);
        raise Exit_status.(Exit_with Input_error)
    in
    let actions = List.map ~f:parse_symbol input.roots in
    let%lwt (result, telemtry) =
      rpc args
      @@ Server_command_types.FIND_MY_TESTS
           (input.version, input.config, actions)
    in
    (match result with
    | Ok fmt_result ->
      print_find_my_tests_result fmt_result ~json:args.output_json;
      Lwt.return (Exit_status.No_error, telemtry)
    | Error error ->
      Printf.eprintf "%s\n" error;
      Lwt.return (Exit_status.Input_error, telemtry))
  | Client_env.MODE_PACKAGE_LINT file ->
    let file = expand_file_path file in
    let%lwt (results, telemetry) =
      rpc args @@ Server_command_types.PACKAGE_LINT file
    in
    `List
      (List.map (Relative_path.Set.elements results) ~f:(fun p ->
           `String (Relative_path.to_absolute p)))
    |> Yojson.Safe.to_string
    |> print_endline;
    Lwt.return (Exit_status.No_error, telemetry)
  | Client_env.MODE_PACKAGE_LINT_FULL (file, candidates) ->
    let file = expand_file_path file in
    let candidates = List.map candidates ~f:expand_file_path in
    let%lwt (results, telemetry) =
      rpc args @@ Server_command_types.PACKAGE_LINT_FULL (file, candidates)
    in
    `List
      (List.map (Relative_path.Set.elements results) ~f:(fun p ->
           `String (Relative_path.to_absolute p)))
    |> Yojson.Safe.to_string
    |> print_endline;
    Lwt.return (Exit_status.No_error, telemetry)

let rec flush_event_logger () : unit Lwt.t =
  let%lwt () = Lwt_unix.sleep 1.0 in
  let%lwt () = Event_logger_lwt.flush () in
  flush_event_logger ()

let main
    (args : Client_env.client_check_env)
    (config : Server_config.t)
    (local_config : Server_local_config.t)
    ~(init_proc_stack : string list option) : _ =
  Hack_event_logger.client_set_mode
    (match args.mode with
    | Client_env.MODE_STATUS_FILES _ ->
      (* Telemetry names are stable across internal renames. *)
      "MODE_STATUS_SINGLE"
    | mode -> Client_env.Variants_of_client_mode.to_name mode);

  Hack_event_logger.client_check_start ~reason:args.reason;
  Client_spinner.start_heartbeat_telemetry ();
  Lwt.dont_wait flush_event_logger (fun _exn -> ());
  let partial_telemetry_ref = ref None in

  try
    (* Note: SIGINT exception handler is typically raised from [run_main], not from
       the lwt code that's running in [main_internal]. Thus, [Lwt_utils.run_main]'s caller is
       the only place that we can deal with it. This also motivates the use of a reference
       [partial_telemetry_ref], since there's no way for a return value to survive SIGINT. *)
    let (exit_status, telemetry) =
      Lwt_utils.run_main (fun () ->
          main_internal args config local_config partial_telemetry_ref)
    in
    let spinner = Client_spinner.get_latest_report () in
    Hack_event_logger.client_check
      exit_status
      telemetry
      ~init_proc_stack
      ~spinner;
    Hh_logger.log "CLIENT_CHECK %s" (Exit_status.show exit_status);
    Exit.exit exit_status
  with
  | exn ->
    let e = Exception.wrap exn in
    let spinner = Client_spinner.get_latest_report () in
    (* hide the spinner *)
    Client_spinner.report ~to_stderr:false ~angery_reaccs_only:false None;
    let exit_status =
      match exn with
      | Exit_status.Exit_with exit_status ->
        (* we assume that whoever raised [Exit_with] had the decency to print an explanation *)
        exit_status
      | _ ->
        (* if it was uncaught, then presumably no one else has printed it, so it's up to us.
           Let's include lots of details, including stack trace. *)
        let exit_status = Exit_status.Uncaught_exception e in
        Printf.eprintf "%s\n%!" (Exit_status.show_expanded exit_status);
        exit_status
    in
    begin
      match (exit_status, !partial_telemetry_ref) with
      | (Exit_status.(Interrupted | Client_broken_pipe), Some telemetry) ->
        (* [Interrupted] is raised by a SIGINT exit-handler installed in hh_client.
           [Client_broken_pipe] is raised in [clientCheckStatus.go_streaming] when
           it can't write errors to the pipe. *)
        Hack_event_logger.client_check_partial
          exit_status
          telemetry
          ~init_proc_stack
          ~spinner;
        Hh_logger.log
          "CLIENT_CHECK_PARTIAL [%s] %s"
          (Option.value_map spinner ~f:fst ~default:"")
          (Exit_status.show exit_status)
      | _ ->
        Hack_event_logger.client_check_bad_exit
          exit_status
          e
          ~init_proc_stack
          ~spinner;
        Hh_logger.log
          "CLIENT_CHECK_EXIT [%s] %s"
          (Option.value_map spinner ~f:fst ~default:"")
          (Exit_status.show_expanded exit_status)
    end;
    Exit.exit exit_status
