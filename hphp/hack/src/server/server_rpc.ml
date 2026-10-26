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

(** [take_max_errors n errors] truncate [errors] so its length is
  at most [n].

  Returns the truncated errors and the dropped error count. *)
let take_max_errors
    (max_errors : int option) (error_list : (_, _) User_diagnostic.t list) :
    (_, _) User_diagnostic.t list * int =
  match max_errors with
  | Some max_errors ->
    let (error_list, dropped_errors) = List.split_n error_list max_errors in
    (error_list, List.length dropped_errors)
  | None -> (error_list, 0)

let single_ctx env path file_input =
  let contents =
    match file_input with
    | Server_command_types.FileName path -> Sys_utils.cat path
    | Server_command_types.FileContent contents -> contents
  in
  let ctx = Provider_utils.ctx_from_server_env env in
  Provider_context.add_or_overwrite_entry_contents ~ctx ~path ~contents

let log_check_response env =
  Hack_event_logger.check_response
    (Diagnostics.get_diagnostic_list env.Server_env.diagnostics
    |> List.map ~f:(fun { User_diagnostic.code; _ } -> code))

let handle :
    type a.
    Server_env.genv ->
    Server_env.env ->
    is_stale:bool ->
    Server_command_types.cmd_metadata ->
    a Server_command_types.t ->
    Server_env.env * a =
 fun genv env ~is_stale metadata -> function
  | Server_command_types.STATUS { max_errors; error_filter } ->
    log_check_response env;
    let (error_list, dropped_count) =
      env.Server_env.diagnostics
      |> Diagnostics.sort_and_finalize
      |> Filter_diagnostics.filter error_filter
      |> take_max_errors max_errors
    in
    let liveness =
      if is_stale then
        Server_command_types.Stale_status
      else
        Server_command_types.Live_status
    in
    let last_recheck_stats =
      match env.Server_env.last_recheck_loop_stats_for_actual_work with
      | None -> None
      | Some recheck_stats ->
        Some
          (Server_env.RecheckLoopStats.to_user_telemetry recheck_stats
          |> Telemetry.string_
               ~key:"init_id"
               ~value:Server_env.(env.init_env.init_id))
    in
    ( env,
      {
        Server_command_types.Server_status.liveness;
        error_list;
        dropped_count;
        last_recheck_stats;
        file_watcher_clock = env.Server_env.clock;
      } )
  | Server_command_types.STATUS_SINGLE
      {
        file_names;
        max_errors;
        error_filter;
        preexisting_warnings;
        return_expanded_tast;
      } ->
    let error_filter =
      {
        Tast_provider.ErrorFilter.error_filter;
        warnings_saved_state =
          Server_env.(env.init_env.mergebase_warning_hashes)
          >>= Option.some_if (not preexisting_warnings);
      }
    in
    let ctx = lazy (Provider_utils.ctx_from_server_env env) in
    let (errors, tasts) =
      let use_cached_diagnostics =
        genv.Server_env.local_config
          .Server_local_config.status_single_use_cached_diagnostics
      in
      let uses_partial_typecheck =
        genv.Server_env.local_config
          .Server_local_config.enable_type_check_filter_files
        || Option.is_some
             genv.Server_env.local_config.Server_local_config.workload_quantile
      in
      let cached_result =
        if use_cached_diagnostics then
          Server_status_single.go_from_cached_diagnostics
            env
            file_names
            ~return_expanded_tast
            ~preexisting_warnings
            ~is_stale
            ~uses_partial_typecheck
            ~error_filter
        else
          None
      in
      match cached_result with
      | Some result -> result
      | None ->
        Server_status_single.go
          genv.Server_env.workers
          file_names
          (Lazy.force ctx)
          ~return_expanded_tast
          ~error_filter
    in
    let errors =
      errors |> Diagnostics.sort_and_finalize |> take_max_errors max_errors
    in
    (* Unforced lazy values are closures which make serialization over RPC fail. *)
    let tasts =
      if return_expanded_tast then
        Some
          (Relative_path.Map.map
             tasts
             ~f:
               (Tast_with_dynamic.map ~f:(fun tast ->
                    tast
                    |> Tast_expand.expand_program (Lazy.force ctx)
                    |> Tast.force_lazy_values)))
      else
        None
    in
    (env, (errors, tasts))
  | Server_command_types.LOG_ERRORS
      { files; log_file; error_filter; preexisting_warnings } ->
    let telemetry =
      Server_log_errors.go
        genv.Server_env.workers
        env
        files
        error_filter
        preexisting_warnings
    in
    let () =
      match log_file with
      | Some path ->
        let oc = Out_channel.create ~binary:false ~append:true path in
        Out_channel.output_string oc (Telemetry.to_string telemetry);
        Out_channel.newline oc;
        Out_channel.close oc
      | None ->
        Hack_event_logger.LogFileErrors.log
          telemetry
          ~from:metadata.Server_command_types.from
    in
    (env, ())
  | Server_command_types.INFER_TYPE (file_input, pos) ->
    let path =
      match file_input with
      | Server_command_types.FileName fn ->
        Relative_path.create_detect_prefix fn
      | Server_command_types.FileContent _ ->
        Relative_path.create_detect_prefix ""
    in
    let (ctx, entry) = single_ctx env path file_input in
    let result =
      Provider_utils.respect_but_quarantine_unsaved_changes ~ctx ~f:(fun () ->
          Server_infer_type.go_ctx ~ctx ~entry pos)
    in
    (env, result)
  | Server_command_types.INFER_DYNAMIC (identifier, as_data) ->
    let ctx = Provider_utils.ctx_from_server_env env in
    let ctx =
      Provider_context.map_tcopt ctx ~f:(fun tcopt ->
          Global_options.{ tcopt with tco_dynamic_inference = true })
    in
    let result =
      Provider_utils.respect_but_quarantine_unsaved_changes ~ctx ~f:(fun () ->
          Server_infer_dynamic.go ~ctx ~identifier ~as_data)
    in
    (env, result)
  | Server_command_types.ENFORCEMENT_AT_POS_BATCH positions ->
    let ctx = Provider_utils.ctx_from_server_env env in
    let results =
      List.map positions ~f:(fun (fn, pos) ->
          let path = Relative_path.create_detect_prefix fn in
          let (ctx, entry) = Provider_context.add_entry_if_missing ~ctx ~path in
          let result =
            Provider_utils.respect_but_quarantine_unsaved_changes
              ~ctx
              ~f:(fun () -> Server_enforcement_at_pos.go_ctx ~ctx ~entry pos)
          in
          Server_enforcement_at_pos.result_to_json_string result (fn, pos))
    in
    (env, results)
  | Server_command_types.INFER_TYPE_BATCH positions ->
    (env, Server_infer_type_batch.go genv.Server_env.workers positions env)
  | Server_command_types.IS_SUBTYPE stdin ->
    (env, Server_is_subtype.check genv.Server_env.workers stdin env)
  | Server_command_types.TAST_HOLES (file_input, hole_filter) ->
    let path =
      match file_input with
      | Server_command_types.FileName fn ->
        Relative_path.create_detect_prefix fn
      | Server_command_types.FileContent _ ->
        Relative_path.create_detect_prefix ""
    in
    let (ctx, entry) = single_ctx env path file_input in
    let result =
      Provider_utils.respect_but_quarantine_unsaved_changes ~ctx ~f:(fun () ->
          Server_collect_tast_holes.go_ctx ~ctx ~entry ~hole_filter)
    in
    (env, result)
  | Server_command_types.TAST_HOLES_BATCH files ->
    (env, Server_tast_holes_batch.go genv.Server_env.workers files env)
  | Server_command_types.INFER_TYPE_ERROR (file_input, line, column) ->
    let path =
      match file_input with
      | Server_command_types.FileName fn ->
        Relative_path.create_detect_prefix fn
      | Server_command_types.FileContent _ ->
        Relative_path.create_detect_prefix ""
    in
    let (ctx, entry) = single_ctx env path file_input in
    let result =
      Provider_utils.respect_but_quarantine_unsaved_changes ~ctx ~f:(fun () ->
          Server_infer_type_error.go_ctx ~ctx ~entry ~line ~column)
    in
    (env, result)
  (* TODO: edit this to look for classname *)
  | Server_command_types.IDENTIFY_SYMBOL arg ->
    let module SO = Symbol_occurrence in
    let ctx = Provider_utils.ctx_from_server_env env in
    let get_def_opt type_ name =
      Server_symbol_definition.go
        ctx
        None
        SO.
          {
            type_;
            name;
            is_declaration = None;
            pos = Pos.none;
            affects_prod_build = true;
          }
      |> Option.to_list
      |> List.map ~f:Symbol_definition.to_absolute
    in
    let arg = Str.split (Str.regexp "::") arg in
    (* The following are all the different named entities I could think of in Hack. *)
    let results =
      match arg with
      | [c_name; member] ->
        let c_name = Utils.add_ns c_name in
        List.concat
          [
            get_def_opt (SO.Method (SO.ClassName c_name, member)) "";
            get_def_opt (SO.Property (SO.ClassName c_name, member)) "";
            get_def_opt (SO.XhpLiteralAttr (c_name, member)) "";
            get_def_opt (SO.ClassConst (SO.ClassName c_name, member)) "";
            get_def_opt (SO.Typeconst (c_name, member)) "";
          ]
      | [name] ->
        let name = Utils.add_ns name in
        List.concat
          [
            get_def_opt (SO.Class SO.ClassId) name;
            (* SO.Record and Class find the same things *)
            get_def_opt SO.Function name;
            get_def_opt SO.GConst name;
          ]
      | _ -> []
    in
    (env, results)
  | Server_command_types.IDENTIFY_FUNCTION (filename, file_input, pos) ->
    let (ctx, entry) =
      single_ctx env (Relative_path.create_detect_prefix filename) file_input
    in
    let result =
      Provider_utils.respect_but_quarantine_unsaved_changes ~ctx ~f:(fun () ->
          Server_identify_function.go_quarantined_absolute ~ctx ~entry pos)
    in
    (env, result)
  | Server_command_types.METHOD_JUMP (class_, filter, find_children) ->
    Printf.printf "%s" class_;
    let ctx = Provider_utils.ctx_from_server_env env in
    ( env,
      Method_jumps.get_inheritance
        ctx
        class_
        ~filter
        ~find_children
        env.Server_env.naming_table
        genv.Server_env.workers )
  | Server_command_types.METHOD_JUMP_BATCH (classes, filter) ->
    let ctx = Provider_utils.ctx_from_server_env env in
    ( env,
      Server_method_jumps_batch.go ctx genv.Server_env.workers classes filter )
  | Server_command_types.FIND_REFS find_refs_action ->
    let ctx = Provider_utils.ctx_from_server_env env in
    Provider_utils.respect_but_quarantine_unsaved_changes ~ctx ~f:(fun () ->
        let open Server_command_types.Done_or_retry in
        let include_defs = false in
        Server_find_refs.(
          go
            ctx
            find_refs_action
            include_defs
            ~stream_file:None
            ~hints:[]
            genv
            env
          |> map_env ~f:to_absolute))
  | Server_command_types.GO_TO_IMPL go_to_impl_action ->
    Server_command_types.Done_or_retry.(
      Server_go_to_impl.go ~action:go_to_impl_action ~genv ~env
      |> map_env ~f:Server_find_refs.to_absolute)
  | Server_command_types.IDE_FIND_REFS_BY_SYMBOL
      {
        Find_refs_wire_format.CliArgs.symbol_name = _;
        action;
        stream_file;
        hint_suffixes;
      } ->
    let hints =
      List.map hint_suffixes ~f:(fun suffix -> Relative_path.from_root ~suffix)
    in
    let ctx = Provider_utils.ctx_from_server_env env in
    Provider_utils.respect_but_quarantine_unsaved_changes ~ctx ~f:(fun () ->
        let open Server_command_types.Done_or_retry in
        let include_defs = false in
        map_env
          ~f:Server_find_refs.to_absolute
          (Server_find_refs.go
             ctx
             action
             include_defs
             ~stream_file
             ~hints
             genv
             env))
  | Server_command_types.IDE_GO_TO_IMPL_BY_SYMBOL
      { Find_refs_wire_format.CliArgs.symbol_name = _; action; _ } ->
    let ctx = Provider_utils.ctx_from_server_env env in
    Provider_utils.respect_but_quarantine_unsaved_changes ~ctx ~f:(fun () ->
        let open Server_command_types.Done_or_retry in
        map_env
          ~f:Server_find_refs.to_absolute
          (Server_go_to_impl.go ~action ~genv ~env))
  | Server_command_types.RENAME rename_action ->
    let ctx = Provider_utils.ctx_from_server_env env in
    Provider_utils.respect_but_quarantine_unsaved_changes ~ctx ~f:(fun () ->
        let definition_for_wrapper =
          match rename_action with
          | Server_rename_types.ClassRename _
          | Server_rename_types.ClassConstRename _
          | Server_rename_types.LocalVarRename _ ->
            None
          | Server_rename_types.MethodRename { class_name; old_name; _ } ->
            Server_symbol_definition.go
              ctx
              None
              {
                Symbol_occurrence.name = "unused for lookup";
                type_ =
                  Symbol_occurrence.Method
                    ( Symbol_occurrence.ClassName (Utils.add_ns class_name),
                      old_name );
                is_declaration = None;
                pos = Pos.none;
                affects_prod_build = true;
              }
          | Server_rename_types.FunctionRename { old_name; _ } ->
            Server_symbol_definition.go
              ctx
              None
              {
                Symbol_occurrence.name = Utils.add_ns old_name;
                type_ = Symbol_occurrence.Function;
                is_declaration = None;
                pos = Pos.none;
                affects_prod_build = true;
              }
        in
        Server_rename.go ctx rename_action genv env ~definition_for_wrapper)
  | Server_command_types.IDE_RENAME_BY_SYMBOL
      (action, new_name, symbol_definition) ->
    let ctx = Provider_utils.ctx_from_server_env env in
    Provider_utils.respect_but_quarantine_unsaved_changes ~ctx ~f:(fun () ->
        let open Server_command_types.Done_or_retry in
        match
          Server_rename.go_ide_with_find_refs_action
            ctx
            ~find_refs_action:action
            ~new_name
            ~symbol_definition
            genv
            env
        with
        | Error e -> (env, Done (Error e))
        | Ok r -> map_env r ~f:(fun x -> Ok x))
  | Server_command_types.REMOVE_DEAD_FIXMES codes ->
    log_check_response env;
    (env, `Ok (Server_rename.get_fixme_patches codes env))
  | Server_command_types.REMOVE_DEAD_UNSAFE_CASTS ->
    log_check_response env;
    (env, `Ok (Server_rename.get_dead_unsafe_cast_patches env))
  | Server_command_types.REWRITE_LAMBDA_PARAMETERS files ->
    let ctx = Provider_utils.ctx_from_server_env env in
    (env, Server_rename.get_lambda_parameter_rewrite_patches ctx files)
  | Server_command_types.DUMP_SYMBOL_INFO file_list ->
    (env, Symbol_info_service.go genv.Server_env.workers file_list env)
  | Server_command_types.IN_MEMORY_DEP_TABLE_SIZE ->
    (* TODO(hverr): Clean up 32-bit/migrate *)
    (env, Ok 0)
  | Server_command_types.SAVE_NAMING filename ->
    (env, Save_state_service.go_naming env.Server_env.naming_table filename)
  | Server_command_types.PAUSE ->
    ( { env with Server_env.full_check_status = Server_env.Full_check_paused },
      () )
  | Server_command_types.CHECK_LIVENESS ->
    (* This is for the client to know "is the server available to process requests?" *)
    (env, ())
  | Server_command_types.LINT fnl ->
    let ctx = Provider_utils.ctx_from_server_env env in
    (env, Server_lint.go genv ctx fnl)
  | Server_command_types.LINT_STDIN Server_command_types.{ filename; contents }
    ->
    let ctx = Provider_utils.ctx_from_server_env env in
    (env, Server_lint.go_stdin ctx ~filename ~contents)
  | Server_command_types.LINT_ALL code ->
    let ctx = Provider_utils.ctx_from_server_env env in
    (env, Server_lint.lint_all genv ctx code)
  | Server_command_types.STATS -> (env, Stats.get_stats ())
  | Server_command_types.DUMP_FULL_FIDELITY_PARSE file ->
    (env, Full_fidelity_parse_service.go file)
  | Server_command_types.RAGE -> (env, Server_rage.go genv env)
  | Server_command_types.CST_SEARCH
      Server_command_types.{ sort_results; input; files_to_search } -> begin
    try
      (env, Cst_search_service.go genv env ~sort_results ~files_to_search input)
    with
    | Multi_threaded_call.Coalesced_failures failures ->
      let failures =
        failures
        |> List.map ~f:Worker_controller.failure_to_string
        |> String.concat ~sep:"\n"
      in
      ( env,
        Error
          (Printf.sprintf
             "Worker failures - check the logs for more details:\n%s\n"
             failures) )
    | exn ->
      let e = Exception.wrap exn in
      (env, Error (Exception.to_string e))
  end
  | Server_command_types.NO_PRECHECKED_FILES ->
    (Server_prechecked_files.expand_all env, ())
  | Server_command_types.FUN_DEPS_BATCH positions ->
    (env, Server_fun_deps_batch.go genv.Server_env.workers positions env)
  | Server_command_types.LIST_FILES_WITH_ERRORS ->
    (env, Server_env.list_files_with_errors env)
  | Server_command_types.FILE_DEPENDENTS filenames ->
    let files = Server_file_dependents.go genv env filenames in
    (env, files)
  | Server_command_types.FIND_ISOLATABLE_CLUSTERS options ->
    let result = Server_isolation.go options genv env in
    (env, result)
  | Server_command_types.VALIDATE_ISOLATION filenames ->
    let files =
      List.map filenames ~f:(fun suffix -> Relative_path.from_root ~suffix)
    in
    (env, Server_isolation_validate.go genv env ~files)
  | Server_command_types.VERBOSE verbose ->
    if verbose then
      Hh_logger.Level.set_min_level Hh_logger.Level.Debug
    else
      Hh_logger.Level.set_min_level
        genv.Server_env.local_config.Server_local_config.min_log_level;
    (env, ())
  | Server_command_types.DEPS_OUT_BATCH positions ->
    let ctx = Provider_utils.ctx_from_server_env env in
    (env, Server_deps_out_batch.go ctx positions)
  | Server_command_types.DEPS_IN_BATCH positions ->
    let ctx = Provider_utils.ctx_from_server_env env in
    (env, Server_deps_in_batch.go ~ctx ~genv ~env positions)
  | Server_command_types.FIND_MY_TESTS (version, config, actions) ->
    let ctx = Provider_utils.ctx_from_server_env env in
    let go =
      match version with
      | Server_command_types.Find_my_tests.V2 -> Find_my_tests_v2.go
      | Server_command_types.Find_my_tests.Staging -> Find_my_tests_staging.go
    in
    let result = go ~ctx ~genv ~env config actions in
    (env, result)
  | Server_command_types.PACKAGE_LINT file ->
    Server_package_lint.go_fast genv env file
  | Server_command_types.PACKAGE_LINT_FULL (file, candidates) ->
    Server_package_lint.go genv env file candidates
