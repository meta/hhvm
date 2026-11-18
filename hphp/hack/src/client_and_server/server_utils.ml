(*
 * Copyright (c) 2015, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude
module SN = Naming_special_names

type 'env handle_command_result =
  | Done of 'env
  | Needs_full_recheck of {
      env: 'env;
      finish_command_handling: 'env -> 'env;
      reason: string;
    }

let wrap ~try_ f env = try_ env (fun () -> f env)

let wrap ~try_ = function
  | Done env -> Done env
  | Needs_full_recheck cont ->
    Needs_full_recheck
      {
        cont with
        finish_command_handling = wrap ~try_ cont.finish_command_handling;
      }

let shutdown_client (_ic, oc) =
  let cli = Unix.descr_of_out_channel oc in
  try
    Unix.shutdown cli Unix.SHUTDOWN_ALL;
    Out_channel.close oc
  with
  | _ -> ()

let log_and_get_sharedmem_load_telemetry () : Telemetry.t =
  let telemetry = Telemetry.create () in
  let unwrap (result : (Telemetry.t, Telemetry.t) result) : Telemetry.t =
    match result with
    | Ok telemetry -> telemetry
    | Error telemetry -> telemetry
  in
  Utils.try_with_stack Shared_mem.SMTelemetry.hash_stats
  |> Result.map_error ~f:(fun e ->
         Hh_logger.exception_ e;
         Telemetry.string_
           telemetry
           ~key:"hashtable_stats_error"
           ~value:(Exception.get_ctor_string e))
  |> Result.map
       ~f:(fun { Shared_mem.SMTelemetry.used_slots; slots; nonempty_slots } ->
         let load_factor = float_of_int used_slots /. float_of_int slots in
         Hh_logger.log
           "Hashtable load factor: %d / %d (%.02f) with %d nonempty slots"
           used_slots
           slots
           load_factor
           nonempty_slots;
         Telemetry.float_
           telemetry
           ~key:"hashtable_load_factor"
           ~value:load_factor)
  |> unwrap

let exit_on_exception (e : Exception.t) =
  match Exception.to_exn e with
  | Shared_mem.Out_of_shared_memory ->
    ignore (log_and_get_sharedmem_load_telemetry () : Telemetry.t);
    Printf.eprintf "Error: failed to allocate in the shared heap.\n%!";
    Exit.exit Exit_status.Out_of_shared_memory
  | Shared_mem.Hash_table_full ->
    ignore (log_and_get_sharedmem_load_telemetry () : Telemetry.t);
    Printf.eprintf "Error: failed to allocate in the shared hashtable.\n%!";
    Exit.exit Exit_status.Hash_table_full
  | Watchman.Watchman_error s ->
    Hh_logger.exception_ e;
    Hh_logger.log "Exiting. Failed due to watchman error: %s" s;
    Exit.exit Exit_status.Watchman_failed
  | Multi_threaded_call.Coalesced_failures failures ->
    Hh_logger.exception_ e;
    let failure_msg =
      Multi_threaded_call.coalesced_failures_to_string failures
    in
    Hh_logger.log "%s" failure_msg;
    let is_oom_failure f =
      match f with
      | Worker_controller.Worker_oomed -> true
      | _ -> false
    in
    let has_oom_failure = List.exists ~f:is_oom_failure failures in
    if has_oom_failure then
      let () = Hh_logger.log "Worker oomed. Exiting" in
      Exit.exit Exit_status.Worker_oomed
    else
      (* We attempt to exit with the same code as a worker by folding over
       * all the failures and looking for a WEXITED. *)
      let worker_exit f =
        match f with
        | Worker_controller.Worker_quit (Unix.WEXITED i) -> Some i
        | _ -> None
      in
      let exit_code =
        List.fold_left
          ~f:(fun acc f ->
            if Option.is_some acc then
              acc
            else
              worker_exit f)
          ~init:None
          failures
      in
      (match exit_code with
      | Some i ->
        (* Exit with same code, but through [Exit.exit] so that its hooks run:
           a still-alive Edenfs_watcher instance makes Folly's exit handlers hang. *)
        Exit.exit
          ~msg:failure_msg
          ~exit_code_override:i
          (Exit_status.Uncaught_exception e)
      | None -> failwith failure_msg)
  (* In single-threaded mode, Worker_controller exceptions are raised directly
   * instead of being grouped into MultiThreaadedCall.Coalesced_failures *)
  | Worker_controller.(Worker_failed (_, Worker_oomed)) ->
    Hh_logger.exception_ e;
    Exit.exit Exit_status.Worker_oomed
  | Worker_controller.Worker_busy ->
    Hh_logger.exception_ e;
    Exit.exit Exit_status.Worker_busy
  | Worker_controller.(Worker_failed (_, Worker_quit (Unix.WEXITED i))) ->
    Hh_logger.exception_ e;

    (* Exit with the same exit code that that worker used, through [Exit.exit]
       so that its hooks run (see above). *)
    Exit.exit ~exit_code_override:i (Exit_status.Uncaught_exception e)
  | Worker_controller.Worker_failed_to_send_job _ ->
    Hh_logger.exception_ e;
    Exit.exit Exit_status.Worker_failed_to_send_job
  | File_provider.File_provider_stale ->
    Exit.exit Exit_status.File_provider_stale
  | Typing_deps.Depgraph_unavailable error ->
    let msg = Typing_deps.depgraph_load_error_to_string error in
    Hh_logger.log "Dependency graph unavailable: %s" msg;
    Exit.exit ~msg Exit_status.Depgraph_unavailable
  | Decl_class.Decl_heap_elems_bug _ ->
    Exit.exit Exit_status.Decl_heap_elems_bug
  | Decl_defs.Decl_not_found _ -> Exit.exit Exit_status.Decl_not_found
  | Shared_mem.C_assertion_failure _ ->
    Hh_logger.exception_ e;
    Exit.exit Exit_status.Shared_mem_assertion_failure
  | Shared_mem.Sql_assertion_failure err_num ->
    Hh_logger.exception_ e;
    let exit_code =
      match err_num with
      | 11 -> Exit_status.Sql_corrupt
      | 14 -> Exit_status.Sql_cantopen
      | 21 -> Exit_status.Sql_misuse
      | _ -> Exit_status.Sql_assertion_failure
    in
    Exit.exit exit_code
  | Exit_status.Exit_with ec -> Exit.exit ec
  | _ ->
    Hh_logger.exception_ e;
    Exit.exit (Exit_status.Uncaught_exception e)

let with_exit_on_exception f =
  try f () with
  | exn ->
    let e = Exception.wrap exn in
    exit_on_exception e

let make_next
    ?(hhi_filter = Find_utils.is_hack)
    ~(indexer : unit -> string list)
    ~(extra_roots : Path.t list) : Relative_path.t list Bucket.next =
  let next_files_root =
    Utils.compose (List.map ~f:Relative_path.(create Root)) indexer
  in
  let hhi_root = Hhi.get_hhi_root () in
  let next_files_hhi =
    Utils.compose
      (List.map ~f:Relative_path.(create Hhi))
      (Find.make_next_files ~name:"hhi" ~filter:hhi_filter hhi_root)
  in
  let rec concat_next_files l () =
    match l with
    | [] -> []
    | hd :: tl -> begin
      match hd () with
      | [] -> concat_next_files tl ()
      | x -> x
    end
  in
  let next_files_extra =
    List.map
      ~f:(fun root ->
        Utils.compose
          (List.map ~f:Relative_path.create_detect_prefix)
          (Find.make_next_files ~filter:Find_utils.file_filter root))
      extra_roots
    |> concat_next_files
  in
  fun () ->
    let next =
      concat_next_files [next_files_hhi; next_files_extra; next_files_root] ()
    in
    Bucket.of_list next

(* During naming, we desugar:
 *
 * invariant(foo(), "oh dear");
 *
 * To:
 *
 * if (!foo()) {
 *   invariant_violation("oh dear");
 * }
 *
 * If [cond] and [then_body] look like desugared syntax, return the
 * equivalent expression that calls invariant().
 *)
let resugar_invariant_call
    (env : Tast_env.env) (cond : Tast.expr) (then_body : Tast.block) :
    Tast.expr option =
  (* If a user has actually written
   *
   * if (!foo()) {
   *   invariant_violation("oh dear");
   * }
   *
   * then the position of the if statement and the call will be different. If
   * the positions are the same, we know that we desugared a call to invariant().
   *)
  let has_same_start pos1 pos2 =
    let pos1_start = Pos.start_offset pos1 in
    let pos2_start = Pos.start_offset pos2 in
    pos1_start = pos2_start
  in

  match (cond, then_body) with
  | ( (_, _, Aast.Unop (Ast_defs.Unot, invariant_cond)),
      [
        ( stmt_pos,
          Aast.Expr
            ( call_ty,
              call_pos,
              Aast.(
                Call
                  { func = (recv_ty, recv_pos, Id (name_pos, name)); args; _ })
            ) );
      ] )
    when String.equal name SN.AutoimportedFunctions.invariant_violation
         && has_same_start stmt_pos call_pos ->
    let recv_ty_invariant =
      match Tast_env.get_fun env SN.AutoimportedFunctions.invariant with
      | Decl_entry.Found fun_decl ->
        let (_, f_locl_ty) =
          Tast_env.localize_no_subst
            env
            ~ignore_errors:true
            fun_decl.Typing_defs.fe_type
        in
        f_locl_ty
      | Decl_entry.NotYetAvailable
      | Decl_entry.DoesNotExist ->
        recv_ty
    in
    Some
      ( call_ty,
        call_pos,
        Aast.(
          Call
            {
              func =
                ( recv_ty_invariant,
                  recv_pos,
                  Id (name_pos, SN.AutoimportedFunctions.invariant) );
              targs = [];
              args = Aast_defs.Anormal invariant_cond :: args;
              unpacked_arg = None;
            }) )
  | _ -> None
