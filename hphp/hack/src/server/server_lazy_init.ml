(*
 * Copyright (c) 2018, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

(* Lazy Initialization:

   During Lazy initialization, hh_server tries to do as little work as possible.
   The init_from_saved_state behavior is this:

     Load from saved state -> Parse dirty files -> Naming -> Dirty Typecheck

   The full_init behavior is this: (similar to eager init, but with lazy decl)

     Full Parsing -> Naming -> Full Typecheck (with lazy decl)
*)

(* module Hack_bucket = Bucket *)
open Hh_prelude

(* module Bucket = Hack_bucket *)
open Global_options
open Result.Export
open Server_env
open Server_init_types
module SLC = Server_local_config

type deptable = CustomDeptable of string

let deptable_with_filename (fn : string) : deptable = CustomDeptable fn

let lock_and_load_deptable
    ~(base_file_name : string)
    ~(deptable : deptable)
    ~(ignore_hh_version : bool) : unit =
  match deptable with
  | CustomDeptable fn ->
    let () =
      if
        not
          (Saved_state_loader.ignore_saved_state_version_mismatch
             ~ignore_hh_version)
      then
        let build_revision =
          Save_state_service.saved_state_build_revision_read ~base_file_name
        in
        if not (String.equal build_revision Build_id.build_revision) then
          raise
          @@ Failure
               (Printf.sprintf
                  ("Saved-state build mismatch, this saved-state was built "
                  ^^ " for version '%s', but we expected '%s'")
                  build_revision
                  Build_id.build_revision)
    in
    (* The new dependency graph is threaded through function calls
     * instead of stored in a global *)
    Hh_logger.log "Custom dependency graph will be loaded lazily from %s" fn

let run_saved_state_future
    (genv : genv)
    (ctx : Provider_context.t)
    (dependency_table_saved_state_future :
      ( Saved_state_loader.load_result,
        Server_init_types.load_state_error )
      result
      Future.t) : (Server_init_types.loaded_info, load_state_error) result =
  let t = Unix.gettimeofday () in
  match Future.get dependency_table_saved_state_future ~timeout:300 with
  | Error error ->
    Hh_logger.log
      "Unhandled Future.error from state loader: %s"
      (Future.error_to_string error);
    let e = Exception.wrap_unraised (Future.error_to_exn error) in
    Error (Load_state_unhandled_exception e)
  | Ok (Error error) -> Error error
  | Ok (Ok deptable_result) ->
    let (_ : float) =
      Hh_logger.log_duration
        "Finished downloading naming table and dependency graph."
        t
    in
    let {
      Saved_state_loader.main_artifacts;
      additional_info;
      changed_files_since_saved_state;
      manifold_path;
      is_cached = _;
    } =
      deptable_result
    in
    let {
      Saved_state_loader.Naming_and_dep_table_info.naming_table_path =
        deptable_naming_table_blob_path;
      dep_table_path;
      compressed_dep_table_path = _;
      naming_sqlite_table_path;
      errors_path;
      warning_hashes_path;
    } =
      main_artifacts
    in
    let {
      Saved_state_loader.Naming_and_dep_table_info.dirty_files_promise;
      saved_state_revs_info;
    } =
      additional_info
    in
    let ignore_hh_version =
      Server_args.ignore_hh_version genv.Server_env.options
    in
    let deptable_fn =
      let deptable = deptable_with_filename (Path.to_string dep_table_path) in
      lock_and_load_deptable
        ~base_file_name:(Path.to_string deptable_naming_table_blob_path)
        ~deptable
        ~ignore_hh_version;
      Path.to_string dep_table_path
    in
    let naming_table_fallback_path =
      if Sys.file_exists (Path.to_string naming_sqlite_table_path) then (
        Hh_logger.log "Using sqlite naming table from hack/64 saved state";
        Some (Path.to_string naming_sqlite_table_path)
      ) else
        Server_check_utils.get_naming_table_fallback_path genv
    in
    let (old_naming_table, { Save_state_service_types.old_errors; old_warnings })
        =
      Save_state_service.load_saved_state_exn
        ctx
        ~naming_table_fallback_path
        ~errors_path:(Path.to_string errors_path)
        ~warning_hashes_path:(Path.to_string warning_hashes_path)
    in
    let t = Unix.time () in
    (match
       dirty_files_promise
       |> Future.get
            ~timeout:
              genv.local_config.SLC.load_state_natively_dirty_files_timeout
     with
    | Error error -> Error (Load_state_dirty_files_failure error)
    | Ok
        {
          Saved_state_loader.Naming_and_dep_table_info.master_changes =
            dirty_master_files;
          local_changes = dirty_local_files;
        } ->
      let () = Hack_event_logger.state_loader_dirty_files t in
      let changed_files_since_saved_state =
        Relative_path.Set.of_list changed_files_since_saved_state
      in
      let naming_table_manifold_path = Some manifold_path in
      Ok
        {
          Server_init_types.naming_table_fn =
            Path.to_string naming_sqlite_table_path;
          naming_table_fallback_fn = naming_table_fallback_path;
          deptable_fn;
          changed_files_since_saved_state_rev = changed_files_since_saved_state;
          dirty_master_files;
          dirty_local_files;
          old_naming_table;
          old_errors;
          old_warnings;
          saved_state_revs_info;
          naming_table_manifold_path;
        })

(** [report update p ~other] is called because we just got a report that progress-meter [p]
should be updated with latest information [update]. The behavior of this function
is to (1) update progress-meter [p], and (2) report overall hh_server status,
making a judgment-call about what message to synthesize out of the progress
of both [p] and [other]. *)
let report
    (update : (Exec_command.t * string list) option)
    (progress : (string * float) option ref)
    ~(other : (string * float) option ref) =
  begin
    match update with
    | None -> progress := None
    | Some (cmd, _args) ->
      progress :=
        Some
          (Exec_command.to_string cmd |> Filename.basename, Unix.gettimeofday ())
  end;
  (* To keep reporting human-understandable, we have to account for the fact that there
     are actually two concurrent progress-meters going on, and we want to keep things
     simple, so we only report the single longest-running of the two progress-meters' updates. *)
  let msg =
    match (!progress, !other) with
    | (None, None) -> "loading saved state" (* if neither is going on *)
    | (None, Some (msg, _time))
    | (Some (msg, _time), None) ->
      Printf.sprintf "waiting on %s..." msg
    | (Some (msg1, time1), Some (_msg2, time2)) when Float.(time1 < time2) ->
      Printf.sprintf "waiting on %s..." msg1
    | (Some _, Some (msg2, _)) -> Printf.sprintf "waiting on %s..." msg2
  in
  Server_progress.write "%s" msg;
  ()

let download_and_load_state_exn
    ~(genv : Server_env.genv) ~(ctx : Provider_context.t) ~(root : Path.t) :
    (loaded_info, load_state_error) result =
  let ignore_hh_version = Server_args.ignore_hh_version genv.options in
  let (progress_naming_table_load, progress_dep_table_load) =
    (ref None, ref None)
  in
  let ssopt = genv.local_config.Server_local_config.saved_state in
  let watchman_sockname =
    let { Server_local_config.Watchman.sockname; _ } =
      genv.local_config.Server_local_config.watchman
    in
    Option.map sockname ~f:Path.make
  in
  let watchman_opts =
    Saved_state_loader.Watchman_options.{ root; sockname = watchman_sockname }
  in
  let eden_opts =
    Saved_state_loader.Eden_options.
      {
        lookup_timeout =
          genv.local_config.SLC.load_state_natively_download_timeout;
      }
  in
  let dependency_table_saved_state_future :
      ( Saved_state_loader.load_result,
        Server_init_types.load_state_error )
      result
      Future.t =
    Hh_logger.log "Downloading dependency graph from DevX infra";
    let loader_future =
      State_loader_futures.load
        ~ssopt
        ~progress_callback:(fun update ->
          report
            update
            progress_dep_table_load
            ~other:progress_naming_table_load)
        ~watchman_opts
        ~eden_opts
        ~ignore_hh_version
      |> Future.with_timeout
           ~timeout:genv.local_config.SLC.load_state_natively_download_timeout
    in
    let loader_future =
      Future.continue_with loader_future @@ function
      | Error e -> Error (Load_state_saved_state_loader_failure e)
      | Ok v -> Ok v
    in
    loader_future
  in
  run_saved_state_future genv ctx dependency_table_saved_state_future

let mergebase_info (root_path : Path.t) (saved_state_rev : Hg.Rev.t) :
    Server_env.saved_state_revs_info =
  let root_path = Path.to_string root_path in
  let future =
    Future.continue_with_future (Hg.current_mergebase_hg_rev root_path)
    @@ fun mergebase_rev ->
    Future.continue_with_future
      (Hg.get_closest_global_ancestor mergebase_rev root_path)
    @@ fun mergebase_globalrev ->
    let saved_state_revs_info =
      {
        age = None;
        distance = None;
        saved_state_rev;
        saved_state_globalrev = None;
        saved_state_rev_timestamp = None;
        mergebase_rev = Some mergebase_rev;
        mergebase_globalrev = Some mergebase_globalrev;
        mergebase_rev_timestamp = None;
      }
    in
    Future.of_value saved_state_revs_info
  in
  match Future.get future with
  | Ok x -> x
  | Error e ->
    Hh_logger.log
      "[serverLazyInit]: mergebase_info failed: %s"
      (Future.error_to_string e);
    Server_env.SavedStateRevsInfo.default ~saved_state_rev

let use_precomputed_state_exn
    ~(root : Path.t)
    (genv : Server_env.genv)
    (ctx : Provider_context.t)
    (info : Server_args.saved_state_target_info)
    (cgroup_steps : Cgroup_profiler.step_group) : loaded_info =
  let {
    Server_args.naming_table_path;
    corresponding_base_revision;
    deptable_fn;
    compressed_deptable_fn = _;
    changes;
    naming_changes;
    prechecked_changes;
  } =
    info
  in
  let ignore_hh_version =
    Server_args.ignore_hh_version genv.Server_env.options
  in
  Cgroup_profiler.step_start_end cgroup_steps "load deptable"
  @@ fun _cgroup_step ->
  let deptable_fn =
    let deptable = deptable_with_filename deptable_fn in
    lock_and_load_deptable
      ~base_file_name:naming_table_path
      ~deptable
      ~ignore_hh_version;
    deptable_fn
  in
  let changes = Relative_path.set_of_list changes in
  let naming_changes = Relative_path.set_of_list naming_changes in
  let prechecked_changes = Relative_path.set_of_list prechecked_changes in
  let naming_sqlite_table_path =
    Server_args.naming_sqlite_path_for_target_info info
  in
  let naming_table_fallback_path =
    if Sys.file_exists naming_sqlite_table_path then (
      Hh_logger.log "Using sqlite naming table from saved state";
      Some naming_sqlite_table_path
    ) else
      Server_check_utils.get_naming_table_fallback_path genv
  in
  let errors_path = Server_args.errors_path_for_target_info info in
  let warning_hashes_path = Server_args.warnings_path_for_target_info info in
  let (old_naming_table, { Save_state_service_types.old_errors; old_warnings })
      =
    Cgroup_profiler.step_start_end cgroup_steps "load saved state"
    @@ fun _cgroup_step ->
    Save_state_service.load_saved_state_exn
      ctx
      ~naming_table_fallback_path
      ~errors_path
      ~warning_hashes_path
  in
  let saved_state_revs_info = mergebase_info root corresponding_base_revision in
  {
    naming_table_fn = naming_table_path;
    naming_table_fallback_fn = naming_table_fallback_path;
    deptable_fn;
    changed_files_since_saved_state_rev = naming_changes;
    dirty_master_files = prechecked_changes;
    dirty_local_files = changes;
    old_naming_table;
    old_errors;
    old_warnings;
    saved_state_revs_info;
    naming_table_manifold_path = None;
  }

(** This function ensures the naming-table is ready for us to do [update_reverse_naming_table_from_env_and_get_duplicate_name_errors]
on the [env.naming_table] forward-naming-table files.
- In the case of saved-state-init, [env.naming_table] has dirty files so we'll
  remove them from the global naming table
- In the case of full init, I don't believe this is ever called...!
*)
let remove_items_from_reverse_naming_table_or_build_new_reverse_naming_table
    (ctx : Provider_context.t)
    (old_naming_table : Naming_table.t)
    (parsing_files : Relative_path.Set.t)
    (naming_table_fallback_fn : string option)
    (t : float)
    ~(cgroup_steps : Cgroup_profiler.step_group) : float =
  Cgroup_profiler.step_start_end cgroup_steps "naming from saved state"
  @@ fun _cgroup_step ->
  begin
    match naming_table_fallback_fn with
    | Some _ ->
      (* Set the SQLite fallback path for the reverse naming table, then block out all entries in
         any dirty files to make sure we properly handle file deletes. *)
      Relative_path.Set.iter parsing_files ~f:(fun k ->
          match Naming_table.get_file_info old_naming_table k with
          | None ->
            (* If we can't find the file in [old_naming_table] we don't consider that an error, since
             * it could be a new file that was added. *)
            ()
          | Some
              {
                File_info.ids =
                  { File_info.funs; classes; typedefs; consts; modules };
                position_free_decl_hash = _;
                file_mode = _;
                comments = _;
              } ->
            let backend = Provider_context.get_backend ctx in
            let snd id = id.File_info.name in
            Naming_provider.remove_type_batch
              backend
              (classes |> List.map ~f:snd);
            Naming_provider.remove_type_batch
              backend
              (typedefs |> List.map ~f:snd);
            Naming_provider.remove_fun_batch backend (funs |> List.map ~f:snd);
            Naming_provider.remove_const_batch
              backend
              (consts |> List.map ~f:snd);
            Naming_provider.remove_module_batch
              backend
              (modules |> List.map ~f:snd))
    | None ->
      Hack_event_logger.invariant_violation_bug
        "unexpected saved-state build-new-reverse-naming-table";
      (* Name all the files from the old naming-table (except the new ones we parsed since
         they'll be named by our caller, next). We assume the old naming-table came from a clean
         state, which is why we skip checking for "already bound" conditions. *)
      let old_hack_names =
        Naming_table.filter old_naming_table ~f:(fun k _v ->
            not (Relative_path.Set.mem parsing_files k))
      in
      Naming_table.fold old_hack_names ~init:() ~f:(fun k info () ->
          Naming_global.ndecl_file_skip_if_already_bound
            ctx
            k
            info.File_info.ids)
  end;
  Hack_event_logger.naming_from_saved_state_end t;
  Hh_logger.log_duration "NAMING_FROM_SAVED_STATE_END" t

(* Prechecked files are gated with a flag and not supported in AI/check modes. *)
let use_prechecked_files (genv : Server_env.genv) : bool =
  Server_prechecked_files.should_use genv.options genv.local_config
  && not (Server_args.check_mode genv.options)

let file_names_to_deps names deps =
  let open Typing_deps in
  let { File_info.n_funs; n_classes; n_types; n_consts; n_modules } = names in
  let add_deps_of_sset dep_ctor sset depset =
    S_set.fold (fun n acc -> DepSet.add acc (Dep.make (dep_ctor n))) sset depset
  in
  let deps = add_deps_of_sset (fun n -> Dep.Fun n) n_funs deps in
  let deps = add_deps_of_sset (fun n -> Dep.Type n) n_classes deps in
  let deps = add_deps_of_sset (fun n -> Dep.Type n) n_types deps in
  let deps = add_deps_of_sset (fun n -> Dep.GConst n) n_consts deps in
  let deps = add_deps_of_sset (fun n -> Dep.GConstName n) n_consts deps in
  let deps = add_deps_of_sset (fun n -> Dep.Module n) n_modules deps in
  deps

let names_to_deps (names : Decl_compare.VersionedNames.t) : Typing_deps.DepSet.t
    =
  let { Decl_compare.VersionedNames.old_names; new_names } = names in
  Typing_deps.DepSet.make ()
  |> file_names_to_deps old_names
  |> file_names_to_deps new_names

let log_fanout_information to_recheck_deps files_to_recheck =
  (* we use lazy here to avoid expensive string generation when logging
       * is not enabled *)
  let max = 1000 in
  Hh_logger.log_lazy ~category:"fanout_tests"
  @@ lazy
       (Yojson.Safe.to_string
          (`Assoc
            [
              ("tag", `String "saved_state_init_fanout");
              ( "hashes",
                `List
                  Typing_deps.(
                    List.map ~f:(fun d -> `String (Dep.to_hex_string d))
                    @@ List.take (DepSet.elements to_recheck_deps) max) );
              ( "hashes_was_truncated",
                `Bool (Typing_deps.DepSet.cardinal to_recheck_deps > max) );
              ( "files",
                `List
                  Relative_path.(
                    List.map ~f:(fun p -> `String (suffix p))
                    @@ List.take (Set.elements files_to_recheck) max) );
              ( "files_was_truncated",
                `Bool (Relative_path.Set.cardinal files_to_recheck > max) );
            ]))

(** Compute fanout for saved state init *)
let get_files_to_recheck
    (genv : Server_env.genv)
    (env : Server_env.env)
    (old_naming_table : Naming_table.t)
    (new_naming_table : Naming_table.t)
    (defs_per_dirty_file : Decl_compare.VersionedNames.t Relative_path.Map.t)
    (files_to_redeclare : Relative_path.Set.t) : Relative_path.Set.t =
  let bucket_size = genv.local_config.SLC.type_decl_bucket_size in
  let defs_per_file_to_redeclare =
    Relative_path.Set.fold
      files_to_redeclare
      ~init:Relative_path.Map.empty
      ~f:(fun path acc ->
        match Relative_path.Map.find_opt defs_per_dirty_file path with
        | Some info -> Relative_path.Map.add acc ~key:path ~data:info
        | None -> acc)
  in
  let get_old_and_new_classes path : S_set.t =
    let old_names =
      Naming_table.get_file_info old_naming_table path
      |> Option.map ~f:File_info.simplify
    in
    let new_names =
      Naming_table.get_file_info new_naming_table path
      |> Option.map ~f:File_info.simplify
    in
    let classes_from_names x = x.File_info.n_classes in
    let old_classes = Option.map old_names ~f:classes_from_names in
    let new_classes = Option.map new_names ~f:classes_from_names in
    Option.merge old_classes new_classes ~f:S_set.union
    |> Option.value ~default:S_set.empty
  in
  let old_dirty_names =
    Relative_path.Map.fold
      defs_per_dirty_file
      ~init:File_info.empty_names
      ~f:(fun _fn { Decl_compare.VersionedNames.old_names; new_names = _ } acc
         -> File_info.merge_names old_names acc)
  in
  let ctx = Provider_utils.ctx_from_server_env env in
  Decl_redecl_service.oldify_decls_and_remove_descendants
    ctx
    ~bucket_size
    genv.workers
    get_old_and_new_classes
    ~defs:old_dirty_names;
  let { Decl_redecl_service.fanout; _ } =
    Decl_redecl_service.redo_type_decl
      ~type_check_testing_hooks:None
      ~bucket_size
      ctx
      ~during_init:true
      genv.workers
      get_old_and_new_classes
      ~previously_oldified_defs:old_dirty_names
      ~defs:defs_per_file_to_redeclare
  in
  Decl_redecl_service.remove_old_defs
    ctx
    ~bucket_size
    genv.workers
    old_dirty_names;
  let reparsed =
    Relative_path.Map.keys defs_per_dirty_file |> Relative_path.set_of_list
  in
  let files_to_recheck =
    Server_incremental.resolve_files ~reparsed ctx env fanout
  in
  log_fanout_information fanout.Fanout.to_recheck files_to_recheck;
  files_to_recheck

(** We start off with a list of files that have changed since the state was
    saved ([dirty_files]), the naming table from the saved state ([old_naming_table])
    and a map of the current class / function declarations per file ([new_defs_per_file]).
    We grab the declarations from both, to account for both the declarations
    that were deleted and those that are newly created.
    Then we use the deptable to figure out the files that
    referred to them. Finally we recheck the lot.

    Args:

    genv, env : environments
    old_naming_table: naming table at the time of the saved state
    new_naming_table: naming table after changes
    dirty_master_files and dirty_local_files: we need to typecheck these and,
       since their decl have changed, also all of their dependencies
    similar_files: we only need to typecheck these,
       not their dependencies since their decl are unchanged
    *)
let calculate_fanout_and_defer_or_do_type_check
    (genv : Server_env.genv)
    (env : Server_env.env)
    ~(old_naming_table : Naming_table.t)
    ~(new_naming_table : Naming_table.t)
    ~(dirty_master_files_unchanged_decls : Relative_path.Set.t)
    ~(dirty_master_files_changed_decls : Relative_path.Set.t)
    ~(dirty_local_files_unchanged_decls : Relative_path.Set.t)
    ~(dirty_local_files_changed_decls : Relative_path.Set.t)
    (t : float)
    (cgroup_steps : Cgroup_profiler.step_group) : Server_env.env * float =
  let start_t : seconds = Unix.gettimeofday () in
  let dirty_files_unchanged_decls =
    Relative_path.Set.union
      dirty_master_files_unchanged_decls
      dirty_local_files_unchanged_decls
  in
  let dirty_files_changed_decls =
    Relative_path.Set.union
      dirty_master_files_changed_decls
      dirty_local_files_changed_decls
  in
  let old_and_new_defs_per_dirty_files_changed_decls =
    Server_incremental.get_old_and_new_defs_in_files
      old_naming_table
      new_naming_table
      dirty_files_changed_decls
  in
  (* We're looking at files with unchanged decls and therefore unchanged
   * def names, so it does not matter which version of the naming table
   * we are looking at (since there should have been no changes). *)
  let old_and_new_defs_per_dirty_files_unchanged_decls =
    Server_check_utils.extend_defs_per_file
      genv
      Relative_path.Map.empty
      env.naming_table
      dirty_files_unchanged_decls
    |> Relative_path.Map.map ~f:Decl_compare.VersionedNames.make_unchanged
  in
  let old_and_new_defs_per_dirty_files =
    Relative_path.Map.union
      old_and_new_defs_per_dirty_files_changed_decls
      old_and_new_defs_per_dirty_files_unchanged_decls
  in
  let old_and_new_defs_in_files files : Decl_compare.VersionedNames.t =
    Relative_path.Map.fold
      old_and_new_defs_per_dirty_files_changed_decls
      ~f:
        begin
          fun k v acc ->
            if Relative_path.Set.mem files k then
              Decl_compare.VersionedNames.merge v acc
            else
              acc
        end
      ~init:Decl_compare.VersionedNames.empty
  in
  let ctx = Provider_utils.ctx_from_server_env env in
  let master_deps =
    old_and_new_defs_in_files dirty_master_files_changed_decls |> names_to_deps
  in
  let local_deps =
    old_and_new_defs_in_files dirty_local_files_changed_decls |> names_to_deps
  in
  let get_files_to_recheck files_to_redeclare =
    get_files_to_recheck
      genv
      env
      old_naming_table
      new_naming_table
      old_and_new_defs_per_dirty_files
      files_to_redeclare
  in
  let (env, to_recheck) =
    if use_prechecked_files genv then (
      (* Start with dirty files and fan-out of local changes only *)
      Hh_logger.log
        "Using prechecked algorithm. Computing fanout from %d files with changed decls since the mergebase."
        (Relative_path.Set.cardinal dirty_local_files_changed_decls);
      let to_recheck =
        if genv.local_config.SLC.fetch_remote_old_decls then
          get_files_to_recheck dirty_local_files_changed_decls
        else
          let deps = Typing_deps.add_all_deps env.deps_mode local_deps in
          let files = Naming_provider.get_files ctx deps in
          log_fanout_information deps files;
          files
      in
      let env =
        Server_prechecked_files.init
          env
          ~dirty_local_deps:local_deps
          ~dirty_master_deps:master_deps
      in
      (env, to_recheck)
    ) else (
      (* Start with full fan-out immediately *)
      Hh_logger.log
        "Prechecked algorithm is disabled. Computing the full fanout from %d changed files since the saved state revision."
        (Relative_path.Set.cardinal dirty_files_changed_decls);
      let to_recheck =
        if genv.local_config.SLC.fetch_remote_old_decls then
          get_files_to_recheck dirty_files_changed_decls
        else
          let deps = Typing_deps.DepSet.union master_deps local_deps in
          let deps = Typing_deps.add_all_deps env.deps_mode deps in
          let files = Naming_provider.get_files ctx deps in
          log_fanout_information deps files;
          files
      in
      (env, to_recheck)
    )
  in
  (* We still need to typecheck files whose declarations did not change *)
  let to_recheck =
    to_recheck
    |> Relative_path.Set.union dirty_files_unchanged_decls
    |> Relative_path.Set.union dirty_files_changed_decls
  in

  (* HACK: dump the fanout that we calculated and exit. This is for
     `hh_fanout`'s regression testing vs. `hh_server`. This can be deleted once
     we no longer worry about `hh_fanout` regressing vs. `hh_server`. Deletion
     is tracked at T65464119. *)
  if Server_args.dump_fanout genv.options then (
    Yojson.Safe.pretty_to_channel
      stdout
      (`Assoc
        [
          ( "recheck_files",
            `List
              (Relative_path.Set.elements to_recheck
              |> List.map ~f:Relative_path.to_absolute
              |> List.map ~f:(fun s -> `String s)) );
        ]);
    Out_channel.newline stdout;
    exit 0
  ) else
    let env = { env with changed_files = dirty_files_changed_decls } in
    let to_recheck =
      if
        not
          genv.Server_env.local_config
            .Server_local_config.enable_type_check_filter_files
      then
        to_recheck
      else
        Server_check_utils.user_filter_type_check_files
          ~to_recheck
          ~reparsed:
            (Relative_path.Set.union
               dirty_files_unchanged_decls
               dirty_files_changed_decls)
    in
    let init_telemetry =
      Server_env.Init_telemetry.make
        Server_env.Init_telemetry.Init_lazy_dirty
        (Telemetry.create ()
        |> Telemetry.float_ ~key:"start_time" ~value:start_t
        |> Telemetry.int_
             ~key:"dirty_master_files_unchanged_decls"
             ~value:
               (Relative_path.Set.cardinal dirty_master_files_unchanged_decls)
        |> Telemetry.int_
             ~key:"dirty_master_files_changed_decls"
             ~value:
               (Relative_path.Set.cardinal dirty_master_files_changed_decls)
        |> Telemetry.int_
             ~key:"dirty_local_files_unchanged_decls"
             ~value:
               (Relative_path.Set.cardinal dirty_local_files_unchanged_decls)
        |> Telemetry.int_
             ~key:"dirty_local_files_changed_decls"
             ~value:(Relative_path.Set.cardinal dirty_local_files_changed_decls)
        |> Telemetry.int_
             ~key:"dirty_files_unchanged_decls"
             ~value:(Relative_path.Set.cardinal dirty_files_unchanged_decls)
        |> Telemetry.int_
             ~key:"dirty_files_changed_decls"
             ~value:(Relative_path.Set.cardinal dirty_files_changed_decls)
        |> Telemetry.int_
             ~key:"to_recheck"
             ~value:(Relative_path.Set.cardinal to_recheck)
        |> Telemetry.object_opt
             ~key:"saved_state_revs_info"
             ~value:
               (Option.map
                  ~f:Server_env.SavedStateRevsInfo.to_telemetry
                  env.init_env.saved_state_revs_info))
    in
    let result =
      Server_init_common.defer_or_do_type_check
        genv
        env
        (Relative_path.Set.elements to_recheck)
        init_telemetry
        t
        ~telemetry_label:"type_check_dirty"
        ~cgroup_steps
    in
    Hack_event_logger.type_check_dirty
      ~start_t
      ~dirty_count:(Relative_path.Set.cardinal dirty_files_changed_decls)
      ~recheck_count:(Relative_path.Set.cardinal to_recheck);
    Hh_logger.log
      "ServerInit type_check_dirty count: %d. Files to recheck count: %d"
      (Relative_path.Set.cardinal dirty_files_changed_decls)
      (Relative_path.Set.cardinal to_recheck);
    result

let get_updates_exn ~(genv : Server_env.genv) ~(root : Path.t) :
    Relative_path.Set.t * Server_notifier.clock option =
  let start_t = Unix.gettimeofday () in
  Hh_logger.log "Getting files changed while parsing...";
  Server_notifier.wait_until_ready genv.notifier;
  let telemetry = Telemetry.create () in
  let (changes, clock, _telemetry) =
    Server_notifier.get_changes_async genv.notifier telemetry
  in
  let files_changed_while_parsing =
    match changes with
    | Server_notifier.Unavailable -> Relative_path.Set.empty
    | Server_notifier.SyncChanges updates
    | Server_notifier.AsyncChanges updates ->
      let root = Path.to_string root in
      let filter p =
        String.is_prefix p ~prefix:root && Find_utils.file_filter p
      in
      S_set.filter filter updates
      |> Relative_path.relativize_set Relative_path.Root
  in
  Hh_logger.log
    "Found %d files changed while parsing"
    (Relative_path.Set.cardinal files_changed_while_parsing);
  ignore
    (Hh_logger.log_duration
       "Finished getting files changed while parsing"
       start_t
      : float);
  Hh_logger.log "Watchclock: %s" (Server_env.show_clock clock);
  Hack_event_logger.changed_while_parsing_end start_t;
  (files_changed_while_parsing, clock)

let initialize_naming_table
    (progress_message : string)
    ?(fnl : Relative_path.t list option = None)
    ?(do_naming : bool = false)
    ~(cache_decls : bool)
    (genv : Server_env.genv)
    (env : Server_env.env)
    (cgroup_steps : Cgroup_profiler.step_group) : Server_env.env * float =
  Server_progress.with_message progress_message @@ fun () ->
  let (get_next, count, t) =
    match fnl with
    | Some fnl ->
      ( Multi_worker.next genv.workers fnl,
        Some (List.length fnl),
        Unix.gettimeofday () )
    | None ->
      let (get_next, t) =
        Server_init_common.directory_walk
          ~telemetry_label:"lazy.nt.indexing"
          genv
      in
      (get_next, None, t)
  in
  (* full init - too many files to trace all of them *)
  let trace = false in
  let (env, t) =
    Server_init_common.parse_files_and_update_forward_naming_table
      genv
      env
      ~get_next
      ?count
      t
      ~trace
      ~decl_mode:
        Direct_decl_service.(
          if cache_decls then
            Cached
          else
            Normal)
      ~telemetry_label:"lazy.nt.parsing"
      ~cgroup_steps
      ~worker_call:Multi_worker.wrapper
  in
  if do_naming then
    Server_init_common
    .update_reverse_naming_table_from_env_and_get_duplicate_name_errors
      env
      t
      ~telemetry_label:"lazy.nt.do_naming.naming"
      ~cgroup_steps
  else
    (env, t)

let write_symbol_info
    (genv : Server_env.genv)
    (env : Server_env.env)
    (cgroup_steps : Cgroup_profiler.step_group)
    (t : float) : Server_env.env * float =
  let open Write_symbol_info in
  let (env, t) =
    Server_init_common
    .update_reverse_naming_table_from_env_and_get_duplicate_name_errors
      env
      t
      ~telemetry_label:"write_symbol_info.naming"
      ~cgroup_steps
  in
  let paths = env.swriteopt.symbol_write_index_paths in
  let paths_file = env.swriteopt.symbol_write_index_paths_file in
  let include_hhi = env.swriteopt.symbol_write_include_hhi in
  let ignore_paths = env.swriteopt.symbol_write_ignore_paths in

  (* files to index, they could be either provided or deduced
     from the naming table, depending on the combination of options *)
  let files =
    if List.length paths > 0 || Option.is_some paths_file then
      Indexable.from_options ~paths ~paths_file ~include_hhi
    else
      Indexable.from_naming_table env.naming_table ~include_hhi ~ignore_paths
  in
  match env.swriteopt.symbol_write_index_paths_file_output with
  | Some output ->
    (* Don't run indexer, just returns list of all files to index *)
    List.map
      ~f:(fun Indexable.{ path; _ } -> Relative_path.storage_to_string path)
      files
    |> Out_channel.write_lines output;
    (env, t)
  | None ->
    let out_dir =
      match Server_args.write_symbol_info genv.options with
      | None -> failwith "No write directory specified for --write-symbol-info"
      | Some s -> s
    in
    let opts = Indexer_options.create env.swriteopt ~out_dir in
    let namespace_map =
      env.tcopt.Global_options.po.Parser_options.auto_namespace_map
    in
    let ctx = Provider_utils.ctx_from_server_env env in
    Entrypoint.go genv.workers ctx opts ~namespace_map ~files;
    (env, t)

let write_symbol_info_full_init
    (genv : Server_env.genv)
    (env : Server_env.env)
    (cgroup_steps : Cgroup_profiler.step_group) : Server_env.env * float =
  let (env, t) =
    initialize_naming_table
      ~cache_decls:true
      "write symbol info initialization"
      genv
      env
      cgroup_steps
  in
  write_symbol_info genv env cgroup_steps t

(* If we fail to load a saved state, fall back to typechecking everything *)
let full_init
    (genv : Server_env.genv)
    (env : Server_env.env)
    (cgroup_steps : Cgroup_profiler.step_group) : Server_env.env * float =
  let init_telemetry =
    Server_env.Init_telemetry.make
      Server_env.Init_telemetry.Init_lazy_full
      (Telemetry.create ()
      |> Telemetry.float_ ~key:"start_time" ~value:(Unix.gettimeofday ()))
  in
  let existing_name_count =
    Naming_table.fold env.naming_table ~init:0 ~f:(fun _ _ i -> i + 1)
  in
  if existing_name_count > 0 then begin
    let desc = "full_init_naming_not_empty" in
    Hh_logger.log
      "INVARIANT_VIOLATION_BUG [%s] count=%d"
      desc
      existing_name_count;
    Hack_event_logger.invariant_violation_bug desc ~data_int:existing_name_count
  end;
  Hh_logger.log "full init";

  let (env, t) =
    initialize_naming_table
      ~do_naming:true
      ~cache_decls:true
      "full initialization"
      genv
      env
      cgroup_steps
  in
  Server_init_common.validate_no_errors env.diagnostics;
  let fnl = Naming_table.get_files env.naming_table in
  Server_init_common.defer_or_do_type_check
    genv
    env
    fnl
    init_telemetry
    t
    ~telemetry_label:"lazy.full.type_check"
    ~cgroup_steps

let parse_only_init
    (genv : Server_env.genv)
    (env : Server_env.env)
    (cgroup_steps : Cgroup_profiler.step_group) : Server_env.env * float =
  initialize_naming_table
    ~cache_decls:false
    "parse-only initialization"
    genv
    env
    cgroup_steps

(** We'll adjust the forward and reverse naming table to reflect
    changes to files which changed since the saved-state. We'll also
    reflect changes in files which had [phase=Diagnostics.(Naming|Parsing)]
    as well. Notionally that's because our current mechanism for handling
    duplicate-name-errors requires all affected files to go through
    the "update reverse naming table" procedure. (but it's redundant
    because, elsewhere, we don't allow saved-state-generation in case
    of naming errors...)

    The actual implementation is confusing because it stores fragments
    of naming-table in places you wouldn't expect:

    1. [old_naming_table], which we got from the saved-state, is a [NamingTable.t]
       that's "backed" i.e. it reflects just the sqlite file plus a delta, initially empty.
    2. [env.naming_table] starts out as [Naming_table.empty] as it was created in
       [Server_main.setup_server]. We will add to it the forward-naming-table File_info.t
       for all files discussed above, [parsing_files]
    3. The reverse naming-table is made up of global mutable shmem delta with
       eventual fallback to sqlite. We will write into that delta the reverse-names
       that arise from the [parsing_files]. The same step also gathers any
       duplicate-name errors.
    4. Finally we'll merge the [env.naming_table] (which is only [parsed_files] at the moment)
       into [old_naming_table] (which represents the sqlite file), and store the result
       back in [env.naming_table]. At this point the naming-table, forward and reverse,
       is complete. *)
let update_naming_table
    env
    genv
    ~(do_indexing : bool)
    ~(state_result :
       loaded_info * Relative_path.Set.t * Server_notifier.clock option)
    (cgroup_steps : Cgroup_profiler.step_group) =
  let ( (loaded_info : Server_init_types.loaded_info),
        changed_while_parsing,
        clock ) =
    state_result
  in
  let trace = genv.local_config.SLC.trace_parsing in
  let {
    naming_table_fallback_fn;
    changed_files_since_saved_state_rev;
    dirty_local_files;
    dirty_master_files;
    old_naming_table;
    old_errors;
    old_warnings = _;
    deptable_fn = _;
    naming_table_fn = _;
    saved_state_revs_info = _;
    naming_table_manifold_path = _;
  } =
    loaded_info
  in
  Server_progress.with_message "updating file-to-symbol table" @@ fun () ->
  let t = Unix.gettimeofday () in
  let naming_files =
    List.fold
      ~init:Relative_path.Set.empty
      ~f:Relative_path.Set.union
      [
        changed_files_since_saved_state_rev;
        dirty_master_files;
        dirty_local_files;
        changed_while_parsing;
      ]
    |> Relative_path.Set.filter ~f:Find_utils.path_filter
  in
  let file_count = Relative_path.Set.cardinal naming_files in
  Hh_logger.log
    "Among the changed files, %d are hack/php files (that are not ignored according to ignore_paths flag)."
    file_count;
  ( Cgroup_profiler.step_start_end cgroup_steps "remove fixmes"
  @@ fun _cgroup_step -> Fixme_provider.remove_batch naming_files );
  (* Parse dirty files only *)
  let (env, t) =
    Server_init_common.parse_files_and_update_forward_naming_table
      genv
      env
      ~get_next:
        (Multi_worker.next
           genv.workers
           (Relative_path.Set.elements naming_files))
      ~count:file_count
      t
      ~trace
      ~decl_mode:Direct_decl_service.Normal
        (* Don't overwrite old decls loaded from saved state *)
      ~telemetry_label:"post_ss1.parsing"
      ~cgroup_steps
      ~worker_call:Multi_worker.wrapper
  in
  let ctx = Provider_utils.ctx_from_server_env env in
  let t =
    remove_items_from_reverse_naming_table_or_build_new_reverse_naming_table
      ctx
      old_naming_table
      naming_files
      naming_table_fallback_fn
      t
      ~cgroup_steps
  in
  if do_indexing then
    let (env, t) = write_symbol_info genv env cgroup_steps t in
    (env, t, None)
  else
    (* Do global naming on all dirty files *)
    let (env, t) =
      Server_init_common
      .update_reverse_naming_table_from_env_and_get_duplicate_name_errors
        env
        t
        ~telemetry_label:"post_ss1.naming"
        ~cgroup_steps
    in
    Server_init_common.validate_no_errors env.diagnostics;

    let new_naming_table = env.naming_table in
    let env =
      {
        env with
        clock;
        naming_table = Naming_table.combine old_naming_table env.naming_table;
        disk_needs_parsing = Relative_path.Set.empty;
        needs_recheck = Relative_path.Set.union env.needs_recheck old_errors;
      }
    in
    (env, t, Some new_naming_table)

let log_changed_files_counts
    ~dirty_local_files_unchanged_decls
    ~dirty_local_files_changed_decls
    ~dirty_master_files_unchanged_decls
    ~dirty_master_files_changed_decls
    ~dirty_master_files
    ~dirty_local_files =
  Hh_logger.log
    "Number of files changed between saved state revision and mergebase: %d"
    (Relative_path.Set.cardinal dirty_master_files);
  Hh_logger.log
    "Out of which %d have changed decls, and the rest (%d) have unchanged decls"
    (Relative_path.Set.cardinal dirty_master_files_changed_decls)
    (Relative_path.Set.cardinal dirty_master_files_unchanged_decls);
  Hh_logger.log
    "Number of files changed since mergebase: %d"
    (Relative_path.Set.cardinal dirty_local_files);
  Hh_logger.log
    "Out of which %d have changed decls, and the rest (%d) have unchanged decls"
    (Relative_path.Set.cardinal dirty_local_files_changed_decls)
    (Relative_path.Set.cardinal dirty_local_files_unchanged_decls);
  ()

(** What files should be checked?

  We've already said that files-with-errors from the dirty saved state must be
  rechecked. And we've already produced "duplicate name" errors if needed from
  all the changed files. The question remaining is, what fanout should be checked?

  Here, for each changed file, we compare its hash to the one saved
  in the saved state. If the hashes are the same, then the declarations
  on the file have not changed and we only need to retypecheck that file,
  not all of its dependencies.
  We call these files "similar" to their previous versions.

  A similar check is also made later, inside [calculate_fanout_and_defer_or_do_type_check]
  when it calls [get_files_to_recheck] which calls [Decl_redecl_service.redo_type_decl].
  That obtains old decls from an online service and uses that for decl-diffing.

  Anyway, the effect of this phase is to calculate fanout, techniques to determine
  decl-diffing, using the prechecked algorithm too. Then, the fanout files are
  combined into [env.needs_recheck], as a way of deferring the check until
  the first iteration of ServerTypeCheck. *)
let compute_fanout
    env
    genv
    t
    ~cgroup_steps
    ~old_naming_table
    ~new_naming_table
    ~dirty_master_files
    ~dirty_local_files =
  Server_progress.with_message "figuring out files to recheck" @@ fun () ->
  let partition_unchanged_hash dirty_files =
    Relative_path.Set.partition
      (fun f ->
        let old_info = Naming_table.get_file_info old_naming_table f in
        let new_info = Naming_table.get_file_info env.naming_table f in
        match (old_info, new_info) with
        | (Some x, Some y) ->
          (match
             ( x.File_info.position_free_decl_hash,
               y.File_info.position_free_decl_hash )
           with
          | (Some x, Some y) -> Int64.equal x y
          | _ -> false)
        | _ -> false)
      dirty_files
  in
  let (dirty_master_files_unchanged_decls, dirty_master_files_changed_decls) =
    partition_unchanged_hash dirty_master_files
  in
  let (dirty_local_files_unchanged_decls, dirty_local_files_changed_decls) =
    partition_unchanged_hash dirty_local_files
  in
  log_changed_files_counts
    ~dirty_local_files_unchanged_decls
    ~dirty_local_files_changed_decls
    ~dirty_master_files_unchanged_decls
    ~dirty_master_files_changed_decls
    ~dirty_master_files
    ~dirty_local_files;

  let (env, t) =
    calculate_fanout_and_defer_or_do_type_check
      genv
      env
      ~old_naming_table
      ~new_naming_table
      ~dirty_master_files_unchanged_decls
      ~dirty_master_files_changed_decls
      ~dirty_local_files_unchanged_decls
      ~dirty_local_files_changed_decls
      t
      cgroup_steps
  in
  (env, t)

(** Among other things, updates the naming table and compute fanout. *)
let post_saved_state_initialization
    ~(do_indexing : bool)
    ~(genv : Server_env.genv)
    ~(env : Server_env.env)
    ~(state_result :
       loaded_info * Relative_path.Set.t * Server_notifier.clock option)
    (cgroup_steps : Cgroup_profiler.step_group) : Server_env.env * float =
  let ( (loaded_info : Server_init_types.loaded_info),
        _changed_while_parsing,
        _clock ) =
    state_result
  in
  let {
    naming_table_fallback_fn = _;
    changed_files_since_saved_state_rev = _;
    dirty_local_files;
    dirty_master_files;
    old_naming_table;
    old_errors;
    old_warnings;
    deptable_fn;
    naming_table_fn = _;
    saved_state_revs_info;
    naming_table_manifold_path;
  } =
    loaded_info
  in
  if genv.local_config.SLC.hg_aware then
    if
      Server_args.is_using_precomputed_saved_state genv.options
      && not genv.local_config.SLC.allow_restart_from_precomputed_saved_state
    then begin
      Hack_event_logger
      .tried_to_be_hg_aware_with_precomputed_saved_state_warning
        ();
      Option.iter
        ~f:Hack_event_logger.set_mergebase_globalrev
        saved_state_revs_info.Server_env.mergebase_globalrev;
      Hh_logger.log
        "Warning: disabling restart on rebase (server was started with precomputed saved-state)"
    end else
      Option.iter
        saved_state_revs_info.Server_env.mergebase_globalrev
        ~f:Server_revision_tracker.initialize
  else
    Option.iter
      ~f:Hack_event_logger.set_mergebase_globalrev
      saved_state_revs_info.Server_env.mergebase_globalrev;
  let env =
    {
      env with
      init_env =
        {
          env.init_env with
          mergebase_warning_hashes =
            Option.some_if
              (not (Server_args.preexisting_warnings genv.Server_env.options))
              old_warnings;
          naming_table_manifold_path;
          saved_state_revs_info = Some saved_state_revs_info;
        };
      deps_mode =
        (match Server_args.save_64bit genv.options with
        | Some new_edges_dir ->
          let human_readable_dep_map_dir =
            Server_args.save_human_readable_64bit_dep_map genv.options
          in
          Typing_deps_mode.SaveToDiskMode
            {
              graph = Some deptable_fn;
              new_edges_dir;
              human_readable_dep_map_dir;
            }
        | None -> Typing_deps_mode.InMemoryMode (Some deptable_fn));
    }
  in

  Hh_logger.log
    "Number of files with errors: %d"
    (Relative_path.Set.cardinal old_errors);

  (***********************************************************
    INVARIANTS.
    These might help make sense of the rest of the function. *)
  (* Invariant: old_naming_table is Backed, and has empty delta *)
  begin
    match Naming_table.get_backed_delta_TEST_ONLY old_naming_table with
    | None ->
      Hack_event_logger.invariant_violation_bug
        "saved-state naming table not backed"
    | Some { Naming_sqlite.file_deltas; _ }
      when not (Relative_path.Map.is_empty file_deltas) ->
      Hack_event_logger.invariant_violation_bug
        "saved-state naming table has deltas"
    | Some _ -> ()
  end;
  (* Invariant: env.naming_table is Unbacked and empty *)
  begin
    match Naming_table.get_backed_delta_TEST_ONLY env.naming_table with
    | None -> ()
    | Some _ ->
      Hack_event_logger.invariant_violation_bug
        "ServerLazyInit env.naming_table is backed"
  end;
  let count =
    Naming_table.fold env.naming_table ~init:0 ~f:(fun _ _ acc -> acc + 1)
  in
  if count > 0 then
    Hack_event_logger.invariant_violation_bug
      "ServerLazyInit env.naming_table is non-empty"
      ~data_int:count;
  (* Invariant: env.disk_needs_parsing and env.needs_recheck are empty *)
  if not (Relative_path.Set.is_empty env.disk_needs_parsing) then
    Hack_event_logger.invariant_violation_bug
      "SeverLazyInit env.disk_needs_parsing is non-empty";
  if not (Relative_path.Set.is_empty env.needs_recheck) then
    Hack_event_logger.invariant_violation_bug
      "SeverLazyInit env.needs_recheck is non-empty";

  (***********************************************************
    NAMING TABLE.
    See doc of update_naming_table.
  *)
  let (env, t, new_naming_table) =
    update_naming_table env genv ~do_indexing ~state_result cgroup_steps
  in

  (***********************************************************
    FANOUT.
    See doc of compute_fanout
  *)
  match new_naming_table with
  | None -> (env, t)
  | Some new_naming_table ->
    compute_fanout
      env
      genv
      t
      ~cgroup_steps
      ~old_naming_table
      ~new_naming_table
      ~dirty_master_files
      ~dirty_local_files

let check_credentials genv =
  let t = Unix.gettimeofday () in
  let attempt_fix = genv.local_config.SLC.attempt_fix_credentials in
  match Security.check_credentials ~attempt_fix with
  | Ok success ->
    Hack_event_logger.credentials_check_end
      (Printf.sprintf "saved_state_init: %s" (Security.show_success success))
      t
  | Error error ->
    let kind = Security.to_error_kind_string error in
    let message = Security.to_error_message_string error in
    Hh_logger.log "Error kind: %s\nError message: %s" kind message;
    Hack_event_logger.credentials_check_failure
      (Printf.sprintf "saved_state_init: [%s]" kind)
      t

let load_saved_state_exn env genv load_state_approach root cgroup_steps =
  Cgroup_profiler.step_start_end cgroup_steps "load saved state"
  @@ fun _cgroup_step ->
  let ctx = Provider_utils.ctx_from_server_env env in
  match load_state_approach with
  | Precomputed info ->
    Ok (use_precomputed_state_exn ~root genv ctx info cgroup_steps)
  | Load_state_natively -> download_and_load_state_exn ~genv ~ctx ~root

let saved_state_init
    ~(do_indexing : bool)
    ~(load_state_approach : load_state_approach)
    (genv : Server_env.genv)
    (env : Server_env.env)
    (root : Path.t)
    (cgroup_steps : Cgroup_profiler.step_group) :
    ( (Server_env.env * float) * (loaded_info * Relative_path.Set.t),
      load_state_error )
    result =
  check_credentials genv;
  Server_progress.write "loading saved state";
  let t = Unix.gettimeofday () in
  let open Result.Monad_infix in
  let state_result =
    try
      load_saved_state_exn env genv load_state_approach root cgroup_steps
      >>| fun loaded_info ->
      let (changed_while_parsing, clock) = get_updates_exn ~genv ~root in
      (loaded_info, changed_while_parsing, clock)
    with
    | exn ->
      let e = Exception.wrap exn in
      Error (Load_state_unhandled_exception e)
  in
  Hack_event_logger.saved_state_download_and_load_done
    ~load_state_approach:(show_load_state_approach load_state_approach)
    ~success:(Result.is_ok state_result)
    ~state_result:
      (match state_result with
      | Error _ -> None
      | Ok (i, _, _) -> Some (yojson_of_loaded_info i))
    t;
  Hack_event_logger.flush ();
  state_result >>| fun (loaded_info, changed_while_parsing, clock) ->
  Server_progress.write "loading saved state succeeded";
  Hh_logger.log "Watchclock: %s" (Server_env.show_clock clock);
  let (env, t) =
    post_saved_state_initialization
      ~do_indexing
      ~state_result:(loaded_info, changed_while_parsing, clock)
      ~env
      ~genv
      cgroup_steps
  in
  ((env, t), (loaded_info, changed_while_parsing))
