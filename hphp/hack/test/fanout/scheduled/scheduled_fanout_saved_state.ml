(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *)

open Hh_prelude

type t = {
  base_file_name: string;
  naming_table_path: string;
  errors_path: string;
  warning_hashes_path: string;
  dependency_graph_path: string;
}

let required_environment name =
  match Sys.getenv_opt name with
  | Some value when not (String.is_empty value) -> value
  | _ -> failwith ("Scheduled saved-state tests require " ^ name)

let check_build_revision base_file_name =
  let revision =
    Save_state_service.saved_state_build_revision_read ~base_file_name
  in
  if not (String.equal revision Build_id.build_revision) then
    failwith
      (Printf.sprintf
         "Saved-state build mismatch: saved state is '%s', test binary is '%s'"
         revision
         Build_id.build_revision)

let run_hh_distc ~root ~directory ~base_file_name ~compressed_graph_path =
  let executable = required_environment "HH_DISTC_PATH" in
  let worker = required_environment "WORKER_PATH" in
  let local_config_directory = Filename.concat directory "local_config" in
  Unix.mkdir local_config_directory 0o700;
  Sys_utils.write_file
    ~file:(Filename.concat local_config_directory "hh.conf")
    "";
  let process =
    Process.exec
      (Exec_command.For_use_in_testing_only executable)
      ~env:
        (Process_types.Augment ["HH_LOCALCONF_PATH=" ^ local_config_directory])
      [
        "--cmd";
        "worker=" ^ worker;
        "check";
        "--use-local-execution";
        "--trace-store-mode";
        "local";
        "--root";
        root;
        "--save-state";
        base_file_name;
        "--save-64bit";
        Filename.concat directory "dependency_edges";
        "--write-compressed-depgraph";
        compressed_graph_path;
      ]
  in
  let result = Process.read_and_wait_pid ~timeout:300 process in
  match result with
  | Ok { Process_types.stdout; stderr }
  | Error
      (Process_types.Abnormal_exit { status = Unix.WEXITED 2; stdout; stderr })
    ->
    (stdout, stderr)
  | Error failure ->
    (match failure with
    | Process_types.Timed_out _ ->
      (* The timeout kills the child and closes its pipes, but does not reap it.
         A second wait completes that cleanup without discarding its output. *)
      ignore (Process.read_and_wait_pid ~timeout:0 process)
    | _ -> ());
    let status =
      match failure with
      | Process_types.Abnormal_exit { status; _ } ->
        " (" ^ Process.status_to_string status ^ ")"
      | _ -> ""
    in
    failwith
      ("hh_distc saved-state generation failed"
      ^ status
      ^ ":\n"
      ^ Process.failure_msg failure)

let create ~root ~parser_options ~directory =
  Sys_utils.write_file
    ~file:(Filename.concat root ".hhconfig")
    (Printf.sprintf
       "deregister_php_stdlib=%b\ndisable_xhp_element_mangling=%b\n"
       parser_options.Parser_options.deregister_php_stdlib
       parser_options.Parser_options.disable_xhp_element_mangling);
  let base_file_name = Filename.concat directory "hh_mini_saved_state" in
  let naming_table_path = base_file_name ^ "_naming.sql" in
  let errors_path = base_file_name ^ ".err" in
  let warning_hashes_path = base_file_name ^ ".warn" in
  let compressed_graph_path = Filename.concat directory "depgraph.zhhdg" in
  let (stdout, stderr) =
    run_hh_distc ~root ~directory ~base_file_name ~compressed_graph_path
  in
  let fail message =
    failwith
      (Printf.sprintf
         "hh_distc saved-state generation: %s\nstdout:\n%s\nstderr:\n%s"
         message
         stdout
         stderr)
  in
  let require_file path =
    if not (Sys.file_exists path) then fail ("Missing artifact: " ^ path)
  in
  List.iter
    [
      naming_table_path;
      errors_path;
      warning_hashes_path;
      Save_state_service.saved_state_info_file_name ~base_file_name;
      compressed_graph_path;
    ]
    ~f:require_file;
  if
    not
      (Warnings_saved_state.is_empty
         (Warnings_saved_state.read_from_disk warning_hashes_path))
  then
    fail
      "Scheduled fanout tests require a saved-state baseline without warnings";
  (try check_build_revision base_file_name with
  | exn -> fail (Exn.to_string exn));
  let dependency_graph_path =
    match
      Depgraph_decompress_ffi.decompress
        ~compressed_dg_path:compressed_graph_path
    with
    | Ok path ->
      require_file path;
      path
    | Error message -> fail ("Cannot decompress dependency graph: " ^ message)
  in
  {
    base_file_name;
    naming_table_path;
    errors_path;
    warning_hashes_path;
    dependency_graph_path;
  }

let load saved_state (env : Server_env.env) =
  check_build_revision saved_state.base_file_name;
  let deps_mode =
    Typing_deps_mode.InMemoryMode (Some saved_state.dependency_graph_path)
  in
  Typing_deps.replace deps_mode;
  let env = { env with Server_env.deps_mode } in
  let ctx = Provider_utils.ctx_from_server_env env in
  let (naming_table, { Save_state_service_types.old_errors; old_warnings = _ })
      =
    Save_state_service.load_saved_state_exn
      ~naming_table_fallback_path:(Some saved_state.naming_table_path)
      ~errors_path:saved_state.errors_path
      ~warning_hashes_path:saved_state.warning_hashes_path
      ctx
  in
  {
    env with
    Server_env.naming_table;
    (* hh_distc includes HHI in the saved naming table. Reindexing it here would
       populate declaration caches before the scenario begins. *)
    disk_needs_parsing = Relative_path.Set.empty;
    needs_recheck = old_errors;
  }

let initialize
    saved_state
    (genv : Server_env.genv)
    (env : Server_env.env)
    ~dirty_files
    ~notifier_changes =
  check_build_revision saved_state.base_file_name;
  let target : Server_args.saved_state_target_info =
    {
      changes = Relative_path.Set.elements dirty_files;
      naming_changes = Relative_path.Set.elements dirty_files;
      corresponding_base_revision =
        Hg.Rev.of_string "0000000000000000000000000000000000000000";
      deptable_fn = saved_state.dependency_graph_path;
      compressed_deptable_fn = None;
      prechecked_changes = [];
      naming_table_path = saved_state.base_file_name;
    }
  in
  let notified = ref false in
  let notifier =
    Server_notifier.init_mock
      ~get_changes_async:(fun () ->
        if !notified then
          Server_notifier.AsyncChanges S_set.empty
        else (
          notified := true;
          let changes =
            notifier_changes ()
            |> Relative_path.Set.elements
            |> List.map ~f:Relative_path.to_absolute
            |> S_set.of_list
          in
          Server_notifier.AsyncChanges changes
        ))
      ~get_changes_sync:(fun () -> S_set.empty)
  in
  let genv =
    {
      genv with
      Server_env.notifier;
      local_config =
        {
          genv.Server_env.local_config with
          Server_local_config.attempt_fix_credentials = false;
          hg_aware = false;
          prechecked_files = false;
          fetch_remote_old_decls =
            env.Server_env.tcopt.Global_options.tco_fetch_remote_old_decls;
        };
    }
  in
  (* The saved state includes HHI. This is the same empty parsing queue that
     real saved-state initialization receives before it selects dirty files. *)
  let env =
    { env with Server_env.disk_needs_parsing = Relative_path.Set.empty }
  in
  Typing_deps.replace
    (Typing_deps_mode.InMemoryMode (Some saved_state.dependency_graph_path));
  let result =
    Cgroup_profiler.step_group "scheduled_saved_state_init" ~log:false
    @@ Server_lazy_init.saved_state_init
         ~do_indexing:false
         ~load_state_approach:(Server_init_types.Precomputed target)
         genv
         env
         (Server_args.root genv.Server_env.options)
  in
  match result with
  | Ok ((env, _time), _) ->
    if not !notified then
      failwith "Saved-state initialization did not query the scheduled notifier";
    env
  | Error error ->
    let error = Server_init_types.load_state_error_to_verbose_string error in
    failwith ("Scheduled saved-state initialization failed: " ^ error.message)
