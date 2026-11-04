(*
 * Copyright (c) 2015, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude
open Server_env
module SLC = Server_local_config

module CheckStats = struct
  type t = {
    reparse_count: int;
    total_rechecked_count: int;
    time_first_result: seconds_since_epoch option;
  }

  (** Update field [time_first_result] if given timestamp is the
      first sent result. *)
  let record_result_sent_ts stats =
    {
      stats with
      time_first_result =
        Option.first_some stats.time_first_result (Some (Unix.gettimeofday ()));
    }
end

(*****************************************************************************)
(* Debugging *)
(*****************************************************************************)

let print_defs prefix defs =
  List.iter defs ~f:(fun (_, fname) -> Printf.printf "  %s %s\n" prefix fname)

let print_defs_per_file_pos defs_per_file_pos =
  S_map.iter
    (fun x (funs, classes) ->
      Printf.printf "File: %s\n" x;
      print_defs "Fun" funs;
      print_defs "Class" classes)
    defs_per_file_pos;
  Printf.printf "\n";
  Out_channel.flush stdout;
  ()

let print_fast defs_per_file =
  S_map.iter
    (fun x (funs, classes) ->
      Printf.printf "File: %s\n" x;
      S_set.iter (Printf.printf "  Fun %s\n") funs;
      S_set.iter (Printf.printf "  Class %s\n") classes)
    defs_per_file;
  Printf.printf "\n";
  Out_channel.flush stdout;
  ()

(*****************************************************************************)
(* We want add all the declarations that were present in a file *before* the
 * current modification. The scenario:
 * File foo.php was defining the class A.
 * The user gets rid of class A (in foo.php)
 * In general, the type-checker determines what must be re-declared or
 * re-typechecked, by comparing the old and the new type-definitions.
 * That's why we are adding the 'old' definitions to the file.
 * In this case, the redecl phase (typing/typing_redecl_service.ml) is going
 * to compare the 'old' definition of A with the new one. It will realize that
 * the new one is missing, and go ahead and retype everything that depends
 * on A.
 * Without a call to add_old_decls, the class A wouldn't appear anywhere,
 * and we wouldn't realize that we have to re-check the types that depend
 * on A.
 *)
(*****************************************************************************)

let add_old_decls
    (old_naming_table : Naming_table.t)
    (defs_per_file : File_info.names Relative_path.Map.t) :
    Decl_compare.VersionedNames.t Relative_path.Map.t =
  Relative_path.Map.mapi
    defs_per_file
    ~f:
      begin
        fun filename new_names ->
          let old_names =
            match Naming_table.get_file_info old_naming_table filename with
            | None -> File_info.empty_names
            | Some old_info -> File_info.simplify old_info
          in
          { Decl_compare.VersionedNames.old_names; new_names }
      end

(*****************************************************************************)
(* Parses the set of modified files *)
(*****************************************************************************)

(** This pushes all [phase] errors in errors, that aren't in [files],
to the errors-file. *)
let push_errors_outside_files_to_errors_file
    (errors : Diagnostics.t) ~(files : Relative_path.Set.t) : unit =
  let typing_errors_not_in_files_to_check =
    errors
    |> Diagnostics.fold_errors
         ~drop_fixmed:true
         ~init:[]
         ~f:(fun path error acc ->
           if Relative_path.Set.mem files path then
             acc
           else
             (path, error) :: acc)
    |> Diagnostics.from_file_diagnostic_list
  in
  Server_progress.ErrorsWrite.report typing_errors_not_in_files_to_check;
  ()

let indexing genv env to_check cgroup_steps :
    env * File_info.t Relative_path.Map.t =
  File_provider.remove_batch to_check;
  Ast_provider.remove_batch to_check;
  Fixme_provider.remove_batch to_check;

  Shared_mem.GC.collect `gentle;
  let get_next =
    Multi_worker.next genv.workers (Relative_path.Set.elements to_check)
  in
  let ctx = Provider_utils.ctx_from_server_env env in
  let defs_per_file =
    Cgroup_profiler.step_start_end cgroup_steps "parsing" @@ fun _cgroup_step ->
    Direct_decl_service.(
      go
        ctx
        genv.workers
        ~get_next
        ~trace:true
        ~decl_mode:
          (* Not caching here, otherwise oldification done in redo_type_decl will
             oldify the new version (and override the real old versions. *)
          Normal)
  in
  (env, defs_per_file)

let get_interrupt_config genv env =
  Multi_threaded_call.{ handlers = env.interrupt_handlers genv; env }

(*****************************************************************************)
(* Where the action is! *)
(*****************************************************************************)

let get_classes_from_old_and_new ~new_naming_table ~old_naming_table path =
  let new_classes =
    match Naming_table.get_file_info new_naming_table path with
    | None -> S_set.empty
    | Some info ->
      List.fold
        info.File_info.ids.File_info.classes
        ~init:S_set.empty
        ~f:(fun acc id -> S_set.add id.File_info.name acc)
  in
  let old_classes =
    match Naming_table.get_file_info old_naming_table path with
    | None -> S_set.empty
    | Some info ->
      List.fold
        info.File_info.ids.File_info.classes
        ~init:S_set.empty
        ~f:(fun acc id -> S_set.add id.File_info.name acc)
  in
  S_set.union new_classes old_classes

type naming_result = {
  failed_naming: Relative_path.Set.t;
  naming_table: Naming_table.t;
  telemetry: Telemetry.t;
}

(** Updates the naming_table, which is a map from filename to the names of
  toplevel symbols declared in that file: at any given point in time, we want
  to know what each file defines. The updated naming table is returned
  in the return value.
  Also runs [Naming_global.ndecl_file_and_get_conflict_files] which updates
  the global reverse naming table.

  The "winner" in case of duplicate definitions? All filenames involved in any
  duplicate definition were stored by the caller in [env.failed_parsing], and
  the caller includes then in [defs_per_file_parsed]. We iterate over them
  in alphabetical order. Thus, the winner definition will be the one from the
  alphabetically first file. (Within a file, classes win over types, and
  after that the lexically first definition wins).

  Note that on the first typecheck after a duplicate definition has been introduced,
  then [env.failed_parsing] doesn't yet contain all filenames involved, so the
  winner in this first typecheck is non-determnistic -- it's simply previous
  definition. We only get the alphabetically first filename as winner on subsequent
  typechecks. Yuck. *)
let do_naming
    (env : env)
    (ctx : Provider_context.t)
    ~(defs_per_file_parsed : File_info.t Relative_path.Map.t)
    ~(cgroup_steps : Cgroup_profiler.step_group) : naming_result =
  let telemetry = Telemetry.create () in
  let start_t = Unix.gettimeofday () in
  let count = Relative_path.Map.cardinal defs_per_file_parsed in
  Cgroup_profiler.step_start_end cgroup_steps "naming" @@ fun _cgroup_step ->
  Server_incremental.remove_defs_from_reverse_naming_table
    env.naming_table
    defs_per_file_parsed;

  let failed_naming =
    Relative_path.Map.fold
      defs_per_file_parsed
      ~init:Relative_path.Set.empty
      ~f:(fun file fileinfo failed ->
        let failed' =
          Naming_global.ndecl_file_and_get_conflict_files
            ctx
            file
            fileinfo.File_info.ids
        in
        Relative_path.Set.union failed' failed)
  in
  let t2 = Hh_logger.log_duration "Declare_names (name->filename)" start_t in
  (* Update filename->File_info.t forward naming table (into this local variable) *)
  let naming_table =
    Naming_table.update_many env.naming_table defs_per_file_parsed
  in
  (* final telemetry *)
  let t3 = Hh_logger.log_duration "Update_many (filename->names)" t2 in
  let heap_size = Shared_mem.SMTelemetry.heap_size () in
  Hack_event_logger.naming_end ~count start_t heap_size;
  let telemetry =
    telemetry
    |> Telemetry.float_ ~key:"update_reverse_duration" ~value:(t2 -. start_t)
    |> Telemetry.float_ ~key:"update_fwd_duration" ~value:(t3 -. t2)
    |> Telemetry.int_ ~key:"end_heap_mb" ~value:heap_size
    |> Telemetry.float_ ~key:"total_duration" ~value:(t3 -. start_t)
    |> Telemetry.int_ ~key:"count" ~value:count
    |> Telemetry.int_
         ~key:"failed_naming_count"
         ~value:(Relative_path.Set.cardinal failed_naming)
  in
  { failed_naming; naming_table; telemetry }

type redecl_result = {
  changed: Typing_deps.DepSet.t;
  to_recheck: Relative_path.Set.t;
  to_recheck_deps: Typing_deps.DepSet.t;
  old_decl_missing_count: int;
}

(** Oldify defs and compute files to recheck. *)
let do_redecl
    (genv : genv)
    (env : env)
    ~(reparsed : Relative_path.Set.t)
    ~(defs_per_file : Decl_compare.VersionedNames.t Relative_path.Map.t)
    ~(naming_table : Naming_table.t)
    ~(cgroup_steps : Cgroup_profiler.step_group) : redecl_result =
  let get_classes =
    get_classes_from_old_and_new
      ~new_naming_table:naming_table
      ~old_naming_table:env.naming_table
  in
  let bucket_size = genv.local_config.SLC.type_decl_bucket_size in
  let ctx = Provider_utils.ctx_from_server_env env in
  let { Decl_redecl_service.old_decl_missing_count; fanout } =
    Cgroup_profiler.step_start_end cgroup_steps "redecl" @@ fun _cgroup_step ->
    Decl_redecl_service.redo_type_decl
      ~type_check_testing_hooks:env.type_check_testing_hooks
      ~bucket_size
      ctx
      ~during_init:false
      genv.workers
      get_classes
      ~previously_oldified_defs:File_info.empty_names
      ~defs:defs_per_file
  in
  let to_recheck =
    Server_incremental.get_files_to_recheck
      ctx
      env
      fanout
      ~reparsed
      ~diagnostics:env.diagnostics
  in
  {
    changed = fanout.Fanout.changed;
    to_recheck;
    to_recheck_deps = fanout.Fanout.to_recheck;
    old_decl_missing_count;
  }

type type_checking_result = {
  env: Server_env.env;
  diagnostics: Diagnostics.t;
  telemetry: Telemetry.t;
  files_checked: Relative_path.Set.t;
  full_check_done: bool;
  needs_recheck: Relative_path.Set.t;
  total_rechecked_count: int;
  time_first_typing_error: seconds option;
  cancel_reason: Multi_threaded_call.cancel_reason option;
}

let do_type_checking
    (genv : Server_env.genv)
    (env : Server_env.env)
    ~(diagnostics : Diagnostics.t)
    ~(files_to_check : Relative_path.Set.t)
    ~(lazy_check_later : Relative_path.Set.t)
    ~(check_reason : string)
    ~(cgroup_steps : Cgroup_profiler.step_group)
    ~(files_with_naming_errors : Relative_path.Set.t) : type_checking_result =
  let telemetry = Telemetry.create () in
  if Relative_path.(Set.mem files_to_check default) then
    Hh_logger.log "WARNING: rechecking definition in a dummy file";
  let interrupt = get_interrupt_config genv env in
  let longlived_workers =
    genv.local_config.Server_local_config.longlived_workers
  in
  let hh_distc_config =
    let use_distc =
      (* hh_distc and hh_server may behave inconsistently in the face of
         duplicate name errors. Eventually we'll want to make duplicate
         name errors a typing error and this check can go away. *)
      Relative_path.Set.cardinal files_with_naming_errors = 0
      && genv.Server_env.local_config.Server_local_config.use_distc
    in
    Option.some_if
      use_distc
      Typing_check_service.
        {
          enable_fanout_aware_distc =
            genv.Server_env.local_config
              .Server_local_config.enable_fanout_aware_distc;
          fanout_threshold =
            genv.Server_env.local_config
              .Server_local_config.hh_distc_fanout_threshold;
          fanout_full_init_threshold =
            genv.Server_env.local_config
              .Server_local_config.hh_distc_fanout_full_init_threshold;
        }
  in
  let cgroup_typecheck_telemetry = ref None in
  let (errorl', telemetry, env, unfinished_and_reason, time_first_typing_error)
      =
    let ctx = Provider_utils.ctx_from_server_env env in
    Cgroup_profiler.step_start_end
      cgroup_steps
      ~telemetry_ref:cgroup_typecheck_telemetry
      "type check"
    @@ fun () ->
    let ( ( ( env,
              {
                Typing_check_service.diagnostics = errorl;
                warnings_saved_state = mergebase_warning_hashes;
                telemetry;
                time_first_error;
              } ),
            cancelled ),
          prefetch_telemetry ) =
      let root = Server_args.root genv.Server_env.options in
      let typecheck () =
        Typing_check_service.go_with_interrupt
          ctx
          genv.workers
          telemetry
          (files_to_check |> Relative_path.Set.elements)
          ~root:(Some root)
          ~interrupt
          ~longlived_workers
          ~hh_distc_config
          ~check_info:
            (Server_check_utils.get_check_info
               ~check_reason
               ~log_errors:true
               ~discard_warnings:
                 (let during_init =
                    Option.is_some env.init_env.why_needed_full_check
                  in
                  during_init && Server_env.discard_warnings env)
               genv
               env)
          ~type_check_testing_hooks:env.type_check_testing_hooks
          ~warnings_saved_state:
            Server_env.(env.init_env.mergebase_warning_hashes)
      in
      if
        genv.local_config.Server_local_config.eden_prefetch_enabled
        && Relative_path.Set.cardinal files_to_check
           >= genv.local_config
                .Server_local_config.eden_prefetch_fanout_threshold
      then
        let (result, telemetry) =
          Edenfs_prefetch.with_prefetch
            ~root
            ~files:files_to_check
            ~workers:genv.local_config.Server_local_config.eden_prefetch_workers
            ~f:typecheck
        in
        (result, Some telemetry)
      else
        (typecheck (), None)
    in
    let telemetry =
      Option.value_map prefetch_telemetry ~default:telemetry ~f:(fun prefetch ->
          Telemetry.object_ ~key:"edenfs_prefetch" ~value:prefetch telemetry)
    in
    let env =
      { env with init_env = { env.init_env with mergebase_warning_hashes } }
    in
    (errorl, telemetry, env, cancelled, time_first_error)
  in
  let telemetry =
    telemetry
    |> Telemetry.object_opt ~key:"cgroup" ~value:!cgroup_typecheck_telemetry
    |> Telemetry.object_ ~key:"gc" ~value:(Telemetry.quick_gc_stat ())
    |> Telemetry.object_
         ~key:"proc"
         ~value:(Proc_fs.telemetry_for_pid (Unix.getpid ()))
  in

  let files_checked = files_to_check in
  (* Add new things that need to be rechecked *)
  let needs_recheck =
    Relative_path.Set.union env.needs_recheck lazy_check_later
  in
  (* Remove things that were cancelled from things we started rechecking... *)
  let (cancelled, cancel_reason) =
    match unfinished_and_reason with
    | None -> ([], None)
    | Some (unfinished, reason) -> (unfinished, Some reason)
  in
  let (files_checked, needs_recheck) =
    List.fold
      cancelled
      ~init:(files_checked, needs_recheck)
      ~f:(fun (files_checked, needs_recheck) path ->
        ( Relative_path.Set.remove files_checked path,
          Relative_path.Set.add needs_recheck path ))
  in
  (* ... leaving only things that we actually checked, and which can be
   * removed from needs_recheck *)
  let needs_recheck = Relative_path.Set.diff needs_recheck files_checked in
  (* Here we do errors paradigm (1) env.diagnostics: merge in typecheck results, to flow into [env.diagnostics].
     As for paradigms (2) persistent-connection and (3) errors-file, they're handled
     inside [Typing_check_service.go_with_interrupt] because they want to push errors
     as soon as they're discovered. *)
  let diagnostics =
    Diagnostics.incremental_update
      ~old:diagnostics
      ~new_:errorl'
      ~rechecked:files_checked
  in
  let full_check_done = Relative_path.Set.is_empty needs_recheck in

  let total_rechecked_count = Relative_path.Set.cardinal files_checked in
  (* TODO(ljw) I wish to prove the invariant (expressed in the type system) that
     either [cancel_reason=None] or [env.disk_needs_parsing] and [env.need_recheck] are empty.
     It's quite hard to reason about at the moment in the presence of lazy checks.
     I'll revisit once they've been removed. *)
  {
    env;
    diagnostics;
    telemetry;
    files_checked;
    full_check_done;
    needs_recheck;
    total_rechecked_count;
    time_first_typing_error;
    cancel_reason;
  }

let quantile ~index ~count : Relative_path.Set.t -> Relative_path.Set.t =
 fun files ->
  let file_count_in_quantile = Relative_path.Set.cardinal files / count in
  let (file_count_in_quantile, index) =
    if Int.equal 0 file_count_in_quantile then
      let count = Relative_path.Set.cardinal files in
      let file_count_in_quantile = 1 in
      let index =
        if index >= count then
          count - 1
        else
          index
      in
      (file_count_in_quantile, index)
    else
      (file_count_in_quantile, index)
  in
  (* Work with BigList-s the same way Typing_check_service does, to preserve
     the same typechecking order within the quantile. *)
  let files = files |> Relative_path.Set.elements |> Big_list.create in
  let rec pop_quantiles n files =
    let (bucket, files) = Big_list.split_n files file_count_in_quantile in
    if n <= 0 then
      bucket
    else
      pop_quantiles (n - 1) files
  in
  pop_quantiles index files |> Relative_path.Set.of_list

let invoke_testing_hook_after_indexing hooks =
  Option.iter hooks ~f:(fun hooks -> hooks.Server_env.after_indexing ())

let type_check_core
    ?agent_session_id
    (genv : genv)
    (env : env)
    start_time
    ~check_reason
    cgroup_steps =
  let rust_disk_provider_read_start =
    Rust_disk_provider_read_telemetry.get_totals ()
  in
  let t = Unix.gettimeofday () in
  (* `start_time` is when the recheck_loop started and includes preliminaries like
   * reading about file-change notifications and communicating with client.
   * We record all our telemetry uniformally with respect to this start.
   * `t` is legacy, used for ad-hoc duration reporting within this function.
   * For the following, env.int_env.why_needed_full_check is set to Some by
   * ServerLazyInit, and we include it here, and then it's subsequently
   * set to None at the end of this method in the "wrap up" section.
   * Thus, if it's present here, it means the typecheck we're about to do is
   * the initial one of a lazy init. *)
  let telemetry =
    Telemetry.create ()
    |> Telemetry.object_opt
         ~key:"init"
         ~value:
           (Option.map
              env.Server_env.init_env.Server_env.why_needed_full_check
              ~f:Server_env.Init_telemetry.get)
  in
  let time_first_error = None in
  let do_errors_file =
    genv.local_config.Server_local_config.produce_streaming_errors
  in
  let env = { env with full_check_status = Full_check_started } in
  let files_to_parse = env.disk_needs_parsing in
  (* We need to do naming phase for files that failed naming in a previous cycle.
   * "Failed_naming" comes from duplicate name errors; the idea is that a change
   * deletes one of the duplicates, well, this change should cause us to re-parse
   * and re-process the remaining duplicate so it can be properly recorded in the
   * forward and reverse naming tables (even though the remaining duplicate
   * didn't itself change). *)
  if not (Relative_path.Set.is_empty env.failed_naming) then
    Hh_logger.log
      "Also reparsing these files with failed naming: %s"
      (Relative_path.Set.elements env.failed_naming
      |> List.map ~f:Relative_path.suffix
      |> String.concat ~sep:" ");
  let files_to_parse =
    Relative_path.Set.union files_to_parse env.failed_naming
  in

  let reparse_count = Relative_path.Set.cardinal files_to_parse in
  if reparse_count = 1 then
    files_to_parse
    |> Relative_path.Set.choose
    |> Relative_path.to_absolute
    |> Hh_logger.log "Processing changes to 1 file: %s"
  else
    Hh_logger.log "Processing changes to %d files" reparse_count;

  let check_count = Relative_path.Set.cardinal env.needs_recheck in
  Hh_logger.log "Processing deferred typechecking for %d file(s)" check_count;
  let telemetry =
    telemetry |> Telemetry.int_ ~key:"check_count" ~value:check_count
  in

  (* PARSING ***************************************************************)
  Server_progress.write ~include_in_logs:false "parsing %d files" reparse_count;
  let logstring = Printf.sprintf "Parsing %d files" reparse_count in
  Hh_logger.log "Begin %s" logstring;

  (* Parse all changed files. This clears the file contents cache prior
      to parsing. *)
  let telemetry = Telemetry.duration telemetry ~key:"parse_start" ~start_time in
  let diagnostics = env.diagnostics in
  let (env, defs_per_file_parsed) =
    indexing genv env files_to_parse cgroup_steps
  in
  invoke_testing_hook_after_indexing env.type_check_testing_hooks;

  let hs = Shared_mem.SMTelemetry.heap_size () in
  let telemetry =
    telemetry
    |> Telemetry.duration ~key:"parse_end" ~start_time
    |> Telemetry.int_ ~key:"parse_end_heap_size" ~value:hs
    |> Telemetry.int_ ~key:"parse_count" ~value:reparse_count
  in
  Hack_event_logger.parsing_end_for_typecheck t hs ~parsed_count:reparse_count;
  let t = Hh_logger.log_duration logstring t in

  (* UPDATE NAMING TABLES **************************************************)
  Server_progress.write ~include_in_logs:false "updating naming tables";
  let logstring = "updating naming tables" in
  Hh_logger.log "Begin %s" logstring;
  let telemetry =
    Telemetry.duration telemetry ~key:"naming_start" ~start_time
  in
  let ctx = Provider_utils.ctx_from_server_env env in
  let { failed_naming; naming_table; telemetry = naming_telemetry } =
    do_naming env ctx ~defs_per_file_parsed ~cgroup_steps
    (* Note: although do_naming updates global reverse-naming-table maps,
       the updated forward-naming-table "naming_table" only gets assigned
       into env.naming_table later on *)
  in
  let t = Hh_logger.log_duration logstring t in
  let telemetry =
    telemetry
    |> Telemetry.duration ~key:"naming_end" ~start_time
    |> Telemetry.object_ ~key:"naming" ~value:naming_telemetry
  in

  (* REDECL PHASE 1 ********************************************************)
  (* The things we redecl `defs_per_file` come from the current content of
     files changed `defs_per_files_parsed`, plus the previous content `add_old_decls`,
     plus those that had duplicate names `failed_naming` *)
  Server_progress.write "determining changes";
  let deptable_unlocked =
    Typing_deps.allow_dependency_table_reads env.deps_mode true
  in

  Hh_logger.log "(Recomputing type declarations in relation to naming)";
  (* failed_naming can be a superset of keys in defs_per_file - see comment in Naming_global.ndecl_file *)
  (* The term [defs_per_file] doesn't mean anything. It's just exactly the same as defs_per_file_parsed,
     that is a filename->File_info.t map of the files we just parsed,
     except it's just filename->File_info.names -- i.e. purely the names, without positions. *)
  let defs_per_file =
    Naming_table.to_defs_per_file (Naming_table.create defs_per_file_parsed)
  in
  let defs_per_file =
    Server_check_utils.extend_defs_per_file
      genv
      defs_per_file
      naming_table
      failed_naming
  in
  let defs_per_file = add_old_decls env.naming_table defs_per_file in

  let count = Relative_path.Map.cardinal defs_per_file in
  let logstring = Printf.sprintf "Type declaration for %d files" count in
  Hh_logger.log "Begin %s" logstring;
  Hh_logger.log
    "(Recomputing type declarations in changed files and determining immediate typechecking fanout)";
  let telemetry =
    telemetry
    |> Telemetry.duration ~key:"redecl_start" ~start_time
    |> Telemetry.int_ ~key:"redecl_file_count" ~value:count
  in

  (* Compute fanout. Here we compare the old and new versions of
     the declarations defined in all changed files, and collect the set of
     files which need to be re-typechecked as a consequence of those changes. *)
  let reparsed = files_to_parse in
  let { changed; to_recheck; to_recheck_deps; old_decl_missing_count } =
    do_redecl genv env ~reparsed ~defs_per_file ~naming_table ~cgroup_steps
  in
  let telemetry =
    telemetry
    |> Telemetry.int_
         ~key:"old_decl_missing_count"
         ~value:old_decl_missing_count
  in

  let hs = Shared_mem.SMTelemetry.heap_size () in
  Hack_event_logger.first_redecl_end t hs;
  let t = Hh_logger.log_duration logstring t in
  let telemetry =
    telemetry
    |> Telemetry.duration ~key:"redecl_end" ~start_time
    |> Telemetry.int_ ~key:"redecl_end_heap_size" ~value:hs
  in

  let telemetry =
    Telemetry.duration telemetry ~key:"revtrack_decl_changed_end" ~start_time
  in

  Server_revision_tracker.typing_changed
    genv.local_config
    (Relative_path.Set.cardinal to_recheck);

  (* we use lazy here to avoid expensive string generation when logging
   * is not enabled *)
  let max = 1000 in
  Hh_logger.log_lazy ~category:"fanout_tests"
  @@ lazy
       (Yojson.Safe.to_string
          (`Assoc
            [
              ("tag", `String "incremental_fanout");
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
                    @@ List.take (Set.elements to_recheck) max) );
              ( "files_was_truncated",
                `Bool (Relative_path.Set.cardinal to_recheck > max) );
            ]));

  (* Here's where we update the forward-naming-table in [env] *)
  let env =
    {
      env with
      naming_table;
      failed_naming;
      disk_needs_parsing = Relative_path.Set.empty;
    }
  in
  (* HANDLE PRECHECKED FILES AFTER LOCAL CHANGES ***************************)
  Server_progress.write "determining trunk changes";
  Hh_logger.log "Begin evaluating prechecked changes";
  let telemetry =
    Telemetry.duration telemetry ~key:"prechecked1_start" ~start_time
  in

  let (env, prechecked1_telemetry) =
    Server_prechecked_files.update_after_local_changes
      genv
      env
      changed
      ~start_time
  in

  let t = Hh_logger.log_duration "Evaluating prechecked changes" t in
  let telemetry =
    telemetry
    |> Telemetry.duration ~key:"prechecked1_end" ~start_time
    |> Telemetry.object_ ~key:"prechecked1" ~value:prechecked1_telemetry
    |> Telemetry.int_
         ~key:"prechecked1_changed"
         ~value:(Typing_deps.DepSet.cardinal changed)
  in

  let (_ : bool) =
    Typing_deps.allow_dependency_table_reads env.deps_mode deptable_unlocked
  in

  (* Checking this before starting typechecking because we want to attribute
   * big rechecks to rebases, even when restarting is disabled *)
  if
    genv.local_config.Server_local_config.hg_aware_recheck_restart_threshold = 0
  then
    Server_revision_tracker.check_blocking ();
  let telemetry =
    Telemetry.duration telemetry ~key:"revtrack3_check_blocking_end" ~start_time
  in

  (* TYPE CHECKING *********************************************************)
  (* The things we recheck are those from the fanout `do_redecl().fanout` plus every file
     whose error reasons were in changed files. *)
  let type_check_start_t = Unix.gettimeofday () in
  Server_progress.write "typechecking";

  (* For a full check, typecheck everything which may be affected by the
     changes. For a lazy check, typecheck only the affected files which are
     open in the IDE, leaving other affected files to be lazily checked later.
     In either case, don't attempt to typecheck files with parse errors. *)
  let enable_type_check_filter_files =
    genv.Server_env.local_config
      .Server_local_config.enable_type_check_filter_files
  in
  let lazy_check_later = Relative_path.Set.empty in
  (* If the user has enabled a custom file filter, we want to only
   * type check files that pass the filter *)
  let to_recheck =
    if enable_type_check_filter_files then
      Server_check_utils.user_filter_type_check_files ~to_recheck ~reparsed
    else
      to_recheck
  in
  let to_recheck = Relative_path.Set.union env.needs_recheck to_recheck in
  let to_recheck =
    Relative_path.Set.union
      (Relative_path.Set.of_list (Relative_path.Map.keys defs_per_file))
      to_recheck
  in

  Server_progress.write
    "typechecking %d files"
    (Relative_path.Set.cardinal to_recheck);

  let to_recheck =
    match genv.local_config.Server_local_config.workload_quantile with
    | None -> to_recheck
    | Some { Server_local_config.index; count } ->
      let to_recheck = quantile ~index ~count to_recheck in
      Hh_logger.log
        "Will typecheck %d-th %d-quantile only, containing %d files."
        index
        count
        (Relative_path.Set.cardinal to_recheck);
      to_recheck
  in

  (* The errors file must accumulate ALL errors. The call below to [do_type_checking ~files_to_check]
     will report all errors in [files_to_check].
     But there might be other errors in [env.diagnostics] from a previous round of typecheck,
     but which aren't in the current fanout i.e. not in [files_to_check]. We must report those too.
     It remains open for discussion whether the user-experience would be better to have these
     not-in-fanout errors reported here before the typecheck starts, or later after the typecheck
     has finished. We'll report them here for now. *)
  if do_errors_file then begin
    push_errors_outside_files_to_errors_file diagnostics ~files:to_recheck
  end;
  (* And what about the files in [files_to_check] which we were going to typecheck but then
     the typecheck got interrupted  and they were returned from [do_typechecking] as [needs_recheck]?
     Shouldn't we report those too into the errors-file? Well, there's no need to bother:
     if there's anything in [needs_recheck] then the current errors-file will be marked as "incomplete"
     and another round of ServerTypeCheck (hence another errors-file) will be created next. *)
  let to_recheck_count = Relative_path.Set.cardinal to_recheck in
  (* The intent of capturing the snapshot here is to increase the likelihood
      of the state-on-disk being the same as what the parser saw *)
  Hh_logger.log "Begin typechecking %d files." to_recheck_count;
  if do_errors_file then
    Server_progress.ErrorsWrite.telemetry
      (Telemetry.create ()
      |> Telemetry.int_ ~key:"to_recheck_count" ~value:to_recheck_count);

  let telemetry =
    Telemetry.duration telemetry ~key:"typecheck_start" ~start_time
  in
  (* Typecheck all of the files we determined might need rechecking as a
     consequence of the changes (or, in a lazy check,
     the subset of those
     files which are open in an IDE buffer). *)
  let {
    env;
    diagnostics;
    telemetry = typecheck_telemetry;
    files_checked;
    full_check_done = _;
    needs_recheck;
    total_rechecked_count;
    time_first_typing_error;
    cancel_reason;
  } =
    do_type_checking
      genv
      env
      ~diagnostics
      ~files_to_check:to_recheck
      ~lazy_check_later
      ~check_reason
      ~cgroup_steps
      ~files_with_naming_errors:env.failed_naming
  in
  let time_first_error =
    Option.first_some time_first_error time_first_typing_error
  in

  let heap_size = Shared_mem.SMTelemetry.heap_size () in

  Server_progress.write "typecheck ending";
  let logstring =
    Printf.sprintf
      "Typechecked %d files [%d errors]"
      total_rechecked_count
      (Diagnostics.count diagnostics)
  in
  let t = Hh_logger.log_duration logstring t in
  Hh_logger.log "Total: %f\n%!" (t -. start_time);

  let telemetry =
    telemetry
    |> Telemetry.duration ~key:"typecheck_end" ~start_time
    |> Telemetry.object_ ~key:"typecheck" ~value:typecheck_telemetry
    |> Telemetry.object_
         ~key:"hash"
         ~value:(Server_utils.log_and_get_sharedmem_load_telemetry ())
    |> Telemetry.int_opt
         ~key:"depgraph_delta_num_edges"
         ~value:
           (Typing_deps.Telemetry.depgraph_delta_num_edges
              (Provider_context.get_deps_mode ctx))
    |> Telemetry.int_ ~key:"typecheck_heap_size" ~value:heap_size
    |> Telemetry.int_ ~key:"typecheck_to_recheck_count" ~value:to_recheck_count
    |> Telemetry.int_
         ~key:"typecheck_total_rechecked_count"
         ~value:total_rechecked_count
    |> Telemetry.int_
         ~key:"typecheck_files_checked"
         ~value:(Relative_path.Set.cardinal files_checked)
    |> Telemetry.int_
         ~key:"typecheck_lazy_check_later_count"
         ~value:(Relative_path.Set.cardinal lazy_check_later)
    |> Telemetry.bool_
         ~key:"enable_type_check_filter_files"
         ~value:
           genv.local_config.Server_local_config.enable_type_check_filter_files
    |> Telemetry.bool_
         ~key:"typecheck_longlived_workers"
         ~value:genv.local_config.Server_local_config.longlived_workers
    |> Telemetry.string_opt
         ~key:"cancel_reason"
         ~value:
           (Option.map cancel_reason ~f:(fun r ->
                r.Multi_threaded_call.user_message))
    |> Telemetry.string_opt
         ~key:"cancel_details"
         ~value:
           (Option.map cancel_reason ~f:(fun r ->
                r.Multi_threaded_call.log_message))
  in

  (* INVALIDATE FILES (EXPERIMENTAL TYPES IN CODEGEN) **********************)
  Server_invalidate_units.go
    genv
    ctx
    files_checked
    defs_per_file_parsed
    naming_table;

  let telemetry =
    Telemetry.duration telemetry ~key:"invalidate_end" ~start_time
  in

  (* WRAP-UP ***************************************************************)
  let needs_recheck =
    if Option.is_some genv.local_config.Server_local_config.workload_quantile
    then
      (* If we were typechecking quantiles only, then artificially assume that everything
         was typechecked. Otherwise the next recheck iteration will keep typechecking the other
         quantiles. *)
      Relative_path.Set.empty
    else
      needs_recheck
  in
  let full_check_status =
    if Relative_path.Set.is_empty needs_recheck then
      (* TODO(frankemrich) It's surprising that we do not look at env.disk_needs_parsing here as
         well:  The latter may be added to by the interrupt handlers and is not necessarily empty
         here. Revisit this once ServerPrecheckedFiles is gone, and it becomes easier to lock down
         the intended semantics of Full_check_done *)
      Full_check_done
    else
      env.full_check_status
  in
  let why_needed_full_check =
    match env.init_env.why_needed_full_check with
    | None -> None
    | Some why_needed_full_check ->
      if is_full_check_done full_check_status then
        None
      else
        Some why_needed_full_check
  in
  let env =
    {
      env with
      diagnostics;
      needs_recheck;
      full_check_status;
      init_env = { env.init_env with why_needed_full_check };
    }
  in

  (* STATS LOGGING *********************************************************)
  if Shared_mem.SMTelemetry.hh_log_level () > 0 then begin
    Measure.print_stats ();
    Measure.print_distributions ()
  end;
  let telemetry =
    if Shared_mem.SMTelemetry.hh_log_level () > 0 then
      Telemetry.object_
        telemetry
        ~key:"shmem"
        ~value:(Shared_mem.SMTelemetry.get_telemetry ())
    else
      telemetry
  in

  let telemetry =
    telemetry
    |> Telemetry.object_
         ~key:"errors"
         ~value:(Diagnostics.as_telemetry_summary env.diagnostics)
    |> Telemetry.object_
         ~key:"repo_states"
         ~value:(Server_notifier.get_repo_states_telemetry genv.notifier)
  in

  (* HANDLE PRECHECKED FILES AFTER RECHECK *********************************)
  let telemetry =
    Telemetry.duration telemetry ~key:"prechecked2_start" ~start_time
  in
  let deptable_unlocked =
    Typing_deps.allow_dependency_table_reads env.deps_mode true
  in
  let (env, prechecked2_telemetry) =
    Server_prechecked_files.update_after_recheck
      genv
      env
      files_checked
      ~start_time
  in
  let (_ : bool) =
    Typing_deps.allow_dependency_table_reads env.deps_mode deptable_unlocked
  in
  let telemetry =
    telemetry
    |> Telemetry.duration ~key:"prechecked2_end" ~start_time
    |> Telemetry.object_ ~key:"prechecked2" ~value:prechecked2_telemetry
  in

  (* We might have completed a full check, which might mean that a rebase was
   * successfully processed. *)
  Server_revision_tracker.check_non_blocking
    ~is_full_check_done:Server_env.(is_full_check_done env.full_check_status);
  let telemetry =
    Telemetry.duration
      telemetry
      ~key:"revtrack4_check_non_blocking_end"
      ~start_time
  in

  let telemetry =
    Telemetry.duration telemetry ~key:"stop_typing_service" ~start_time
  in

  (* We create an ID for the TYPE_CHECK_END event so we can join the server
     events table with the errors table *)
  let type_check_end_id = Random.bits () in
  let telemetry =
    Telemetry.int_ telemetry ~key:"type_check_end_id" ~value:type_check_end_id
  in
  let telemetry =
    Telemetry.object_
      telemetry
      ~key:"rust_disk_provider_read"
      ~value:
        (Rust_disk_provider_read_telemetry.get_delta_telemetry
           ~start:rust_disk_provider_read_start)
  in

  (* CAUTION! Lots of alerts/dashboards depend on this event, particularly start_t  *)
  Hack_event_logger.type_check_end
    ?agent_session_id
    (Some telemetry)
    ~heap_size
    ~started_count:to_recheck_count
    ~total_rechecked_count
    ~experiments:genv.local_config.Server_local_config.experiments
    ~desc:"serverTypeCheck"
    ~start_t:type_check_start_t;
  Hack_event_logger.TypingErrors.log_errors
    ~type_check_end_id
    ~data:
      (let telemetry =
         Diagnostics.as_telemetry
           ~limit:1000
           ~with_context_limit:10
           ~error_to_string:Contextual_diagnostic_formatter.to_string
           env.diagnostics
       in
       let warning_counts =
         Diagnostics.warning_counts_by_code env.diagnostics
       in
       let warning_counts_telemetry =
         I_map.fold
           (fun code count acc ->
             Telemetry.int_ acc ~key:(string_of_int code) ~value:count)
           warning_counts
           (Telemetry.create ())
       in
       telemetry
       |> Telemetry.object_
            ~key:"warning_counts_by_code"
            ~value:warning_counts_telemetry);
  ( env,
    {
      CheckStats.reparse_count;
      total_rechecked_count;
      time_first_result = time_first_error;
    },
    telemetry,
    cancel_reason )

let type_check_unsafe ?agent_session_id genv env start_time profiling =
  let check_kind = "Full_check" in
  let check_reason =
    match env.Server_env.init_env.Server_env.why_needed_full_check with
    | Some init_telemetry -> Server_env.Init_telemetry.get_reason init_telemetry
    | None -> "incremental"
  in
  let telemetry =
    Telemetry.create ()
    |> Telemetry.string_ ~key:"kind" ~value:check_kind
    |> Telemetry.duration ~key:"start" ~start_time
  in

  (* CAUTION! Lots of alerts/dashboards depend on the exact string of check_kind and check_reason *)
  Hack_event_logger.with_check_kind ~check_kind ~check_reason @@ fun () ->
  Hh_logger.log "******************************************";
  Hh_logger.log
    "Check kind: will bring hh_server to consistency with code changes, by checking whatever fanout is needed ('%s')"
    check_kind;
  let telemetry = Telemetry.duration telemetry ~key:"core_start" ~start_time in

  let (env, stats, core_telemetry, cancel_reason) =
    type_check_core
      ?agent_session_id
      genv
      env
      start_time
      ~check_reason
      profiling
  in

  let telemetry =
    telemetry
    |> Telemetry.duration ~key:"core_end" ~start_time
    |> Telemetry.object_ ~key:"core" ~value:core_telemetry
  in
  let stats = CheckStats.record_result_sent_ts stats in
  let telemetry =
    telemetry |> Telemetry.duration ~key:"sent_done" ~start_time
  in
  (env, stats, telemetry, cancel_reason)

let type_check :
    ?agent_session_id:string ->
    genv ->
    env ->
    seconds ->
    Cgroup_profiler.step_group ->
    env * CheckStats.t * Telemetry.t =
 fun ?agent_session_id genv env start_time cgroup_steps ->
  Server_utils.with_exit_on_exception @@ fun () ->
  (*
  (1) THE ENV MODEL FOR DIAGNOSTICS...
  env.{diagnostics, needs_recheck, disk_needs_parsing} are all persistent values that
  might be adjusted as we go:
  * disk_needs_parsing gets initialized in serverLazyInit, augmented in server_main both
    at the start of the loop and during file watcher interrupts, and in serverTypeCheck it
    gets reset to empty once we have computed files-to-parse and decls-to-refresh from it.
    (files-to-recheck is computed from these two).
  * needs_recheck gets augmented in serverTypeCheck from fanout/stale computation,
    and gets discharged by the files we end up typechecking
  * diagnostics starts out empty and it grows+shrinks during serverTypeCheck
    through calls to "diagnostics = Diagnostics.incremental_update ~diagnostics ~new_errors ~phase ~files_examined".
    This will shrink those diagnostics that had been in diagnostics before, and were in files_examined,
    but are not in new_errors. It will replace others. It will grow others.
    And it will leave remaining in diagnostics anything that was not touched by files_examined
    or which came from a different phase.
    To stress, say you have 10k diagnostics and make a small change in one file,
    it is very possible that those 10k other files are not checked and the bulk of diagnostics
    just continues through a recheck loop. (However, we do gather every single file
    mentioned in any of the *reasons* of those 10k files, and where those reasons intersect
    with changed-files then that causes need to redecl and compute fanout, and also need
    to recheck.)

  (3) THE STREAMING MODEL FOR ERRORS...
  The errors-file grows monotonically: it has no "backsies", no way to remove an error
  from it, short of deciding that the current errors-file is wrong and a new error-file must
  be restarted.
  * Hh_server, upon startup, must eventually produce an errors-file.
  * Right here at the start of the type check, we restart the errors-file (because of the
    potential that in the current typecheck we discover that some files no longer have errors).
  * The typecheck fulfills the contract that every single error must be reported to the errors-file.
    Not just newly discovered errors. Every error, even those from files that do not get rechecked.
  * Right here at the end of the type check, if the type check was complete then we need to report
    this fact right away in the errors-file so that the client "hh check" can finish "No errors!".
    But if it was not complete (e.g. it got interrupted by watchman) then there is no need to finish
    for the sake of the client: there will be an immediate next round of
    Server_main.recheck_until_no_changes_left, and it will call us again, and the errors-file
    will be restarted on that next round, and the act of restarting will close the current errors-file.

  How do we guarantee that hh_server produces an errors-file upon startup?
  It boils down to Server_init_common.defer_or_do_type_check, called as the final step of both
  full init and saved-state init. Its design is to defer an initial typecheck
  to the first round of [ServerTypeCheck.type_check] (i.e. us!) which it causes
  to happen after its (synchronous, non-interruptible) init has finished. It does
  this so that clients will be able to connect as soon as init has finished, and
  observe/interrupt the deferred typecheck. It does this by setting
  [env.full_check_status=Full_check_started], so that when Server_main first enters
  its main loop and calls [serve_one_iteration] for the first time, it will believe
  that a full check is needed and hence call [ServerTypeCheck.type_check]. No matter
  if we did a perfect saved-state-load so that [env.disk_needs_parsing] is empty,
  no matter if we did a full init and [env.needs_recheck] contains every file in the
  project, no matter what init path, the start of Server_main main loop will always
  start by calling [ServerTypeCheck.type_check]. And it's at this moment, right here,
  that we'll lay down the first errors file.
  *)
  let ignore_hh_version =
    Server_args.ignore_hh_version genv.Server_env.options
  in
  (* Restart the errors-file at the start of type_check. *)
  Server_progress.ErrorsWrite.new_empty_file
    ~ignore_hh_version
    ~clock:env.clock
    ~cancel_reason:env.why_needs_server_type_check;

  (* This is the main typecheck function. Its contract is to
     (1) tweak env.diagnostics as needed based on what was rechecked , (2) write every single error to errors-file. *)
  let (env, stats, telemetry, cancel_reason) =
    type_check_unsafe ?agent_session_id genv env start_time cgroup_steps
  in

  (* If the typecheck completed, them mark the errors-file as complete.
      A "completed" typecheck means (1) all the [env.needs_recheck] files
      were indeed typechecked, i.e. not interrupted and cancelled by an
      interrupt handler like the file watcher; (2) file watcher interrupt didn't
      insert any [env.disk_needs_parsing] files.
      Because we mark the errors-file as complete, anyone tailing it will
      know that they can finish their tailing.

      For incomplete typechecks, we don't do anything here. Necessarily Server_main
      will do another round of [ServerTypeCheck.type_check] (i.e. us) shortly,
      and then next round will call [Server_progress.ErrorsWrite.new_empty_file]
      which will put a "restarted" sentinel at the end of the current file as
      well as starting a new file. Indeed it's *better* to place the "restarted"
      sentinel at that future time rather than now, because it'll have a more
      up-to-date watchclock at that time.
      NOTE(frankemrich) It is surprising that here we consider a type-check
      "complete" only if needs_recheck AND disk_needs_parsing are both empty, but
      in type_check_core we only look at needs_recheck to decide whether to set
      full_checK_status to Full_check_done. *)
  let is_complete =
    Relative_path.Set.is_empty env.needs_recheck
    && Relative_path.Set.is_empty env.disk_needs_parsing
  in
  if is_complete then Server_progress.ErrorsWrite.complete telemetry;

  (* If this was a full check, store in [env] whether+why it got interrupted+cancelled. *)
  let env =
    match cancel_reason with
    | Some { Multi_threaded_call.user_message; log_message; timestamp = _ } ->
      {
        env with
        Server_env.why_needs_server_type_check = (user_message, log_message);
      }
    | None when not is_complete ->
      (* The typecheck wasn't interrupted, but there are still items to check.
         This is a weird situation, and one that hopefully won't exist.
         Once lazy checks have been eliminated, we'll revisit the TODO
         on this subject at the end of [do_type_checking], and see if we can
         eliminate this path throgh the typesystem.

         Until that time, what now should we put as the value for [env.why_needs_server_type_check]?
         Well, the existing value in [env] said why it needed a type check earlier, and it still needs
         that same type check to be completed, so it is a plausible answer! *)
      env
    | None ->
      {
        env with
        Server_env.why_needs_server_type_check = ("Type check is complete", "");
      }
  in

  (env, stats, telemetry)
