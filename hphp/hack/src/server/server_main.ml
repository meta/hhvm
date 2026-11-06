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

(*****************************************************************************)
(* Main initialization *)
(*****************************************************************************)

let () = Printexc.record_backtrace true

let force_break_recheck_loop_for_test_ref = ref false

let force_break_recheck_loop_for_test x =
  force_break_recheck_loop_for_test_ref := x

module Program = struct
  let set_signals () =
    (* Warning: Global references inited in this function, should
         be 'restored' in the workers, because they are not 'forked'
         anymore. See `ServerWorker.{save/restore}_state`. *)
    Sys_utils.set_signal
      Sys.sigusr1
      (Sys.Signal_handle Typing.debug_print_last_pos);
    Sys_utils.set_signal
      Sys.sigusr2
      (Sys.Signal_handle
         (fun _ ->
           Hh_logger.log "Got sigusr2 signal. Going to shut down.";
           Exit.exit
             ~msg:
               "Hh_server received a stop signal. This can happen from a large rebase/update"
             Exit_status.Server_shutting_down_due_to_sigusr2));
    (* this is to transform SIGPIPE in an exception. A SIGPIPE can happen when
     * someone C-c the client.
     *)
    Sys_utils.set_signal Sys.sigpipe Sys.Signal_ignore;
    ()

  let run_once_and_exit genv env =
    let recheck_stats =
      Option.map
        ~f:Server_env.RecheckLoopStats.to_user_telemetry
        env.Server_env.last_recheck_loop_stats_for_actual_work
    in
    Server_error.print_error_list
      stdout
      ~stale_msg:None
      ~output_json:(Server_args.json_mode genv.options)
      ~error_format:(Some Diagnostics.Raw)
      ~error_list:
        (List.map
           (Diagnostics.get_diagnostic_list env.diagnostics)
           ~f:User_diagnostic.to_absolute)
      ~recheck_stats;

    Worker_controller.force_quit_all ();

    let has_errors = Diagnostics.has_errors env.diagnostics in
    let error_code =
      if has_errors then
        if Option.is_some (Server_args.write_symbol_info genv.options) then
          32
        else
          1
      else
        0
    in
    exit error_code

  (* filter and relativize updated file paths *)
  let exit_if_critical_update genv ~(raw_updates : S_set.t) : unit =
    let hhconfig_in_updates =
      S_set.mem
        (Relative_path.to_absolute Server_config.repo_config_path)
        raw_updates
    in
    if hhconfig_in_updates then begin
      let (new_config, _) =
        Server_config.load
          ~silent:false
          ~from:(Server_args.from genv.options)
          ~cli_config_overrides:(Server_args.config genv.options)
      in
      if not (Server_config.is_compatible genv.config new_config) then (
        Hh_logger.log
          "%s changed in an incompatible way; please restart %s.\n"
          (Relative_path.suffix Server_config.repo_config_path)
          Global_config.program_name;

        (* TODO: Notify the server monitor directly about this. *)
        Exit.exit Exit_status.Hhconfig_changed
      )
    end;
    let package_config_in_updates =
      S_set.mem
        (Relative_path.to_absolute Package_config.repo_config_path)
        raw_updates
    in
    if package_config_in_updates then begin
      Hh_logger.log
        "%s changed; please restart %s.\n"
        (Relative_path.suffix Package_config.repo_config_path)
        Global_config.program_name;
      Exit.exit Exit_status.Package_config_changed
    end
end

let finalize_init init_env typecheck_telemetry init_telemetry =
  (* rest is just logging/telemetry *)
  let t' = Unix.gettimeofday () in
  let hash_telemetry = Server_utils.log_and_get_sharedmem_load_telemetry () in
  let telemetry =
    Telemetry.create ()
    |> Telemetry.duration ~start_time:init_env.init_start_t
    |> Telemetry.object_
         ~key:"init"
         ~value:(Server_env.Init_telemetry.get init_telemetry)
    |> Telemetry.object_ ~key:"typecheck" ~value:typecheck_telemetry
    |> Telemetry.object_ ~key:"hash" ~value:hash_telemetry
    |> Telemetry.int_
         ~key:"heap_size"
         ~value:(Shared_mem.SMTelemetry.heap_size ())
  in
  Hack_event_logger.server_is_ready telemetry;
  Hh_logger.log
    "SERVER_IS_READY. Took %f seconds to init. Telemetry:\n%s"
    (t' -. init_env.init_start_t)
    (Telemetry.to_string telemetry);
  ()

(*****************************************************************************)
(* The main loop *)
(*****************************************************************************)

(** [let (env, changes, new_clock, may_be_stale, telemetry) = query_notifier genv env query_kind start_time].

Query for changed files. This is a hard to understand method...

CARE! [query_kind] is hard to understand...
* [`Sync] -- a Watchman/Edenfs_watcher sync query, i.e. guarantees to have picked up all updates
  up to the moment it was invoked.
* [`Async] -- picks up changes that  Watchman/Edenfs_watcher has pushed so far, but we don't
  do a sync, so therefore there might be changes on disk that the file watcher will tell us about
  in future.
* [`Skip] -- CARE! Despite its name, this also behaves much like [`Async].

The return value [may_be_stale] indicates whether the most recently returned result was not "sync"
(i.e., guaranateed to include all changes up to that point). This will always be true for [`Sync]
query kind (which deliberately waits for the sync), but it might be true or false for [`Async] and
[`Skip]. The caller can use this as a hint that we don't know whether there are more disk changes.
Personally, I've never actually seen it be true. It's surfaced to the user in clientCheckStatus.ml
with the message "this may be stale, probably due to file watcher being unresponsive". *)
let query_notifier
    (genv : Server_env.genv)
    (env : Server_env.env)
    (query_kind : [> `Async | `Skip | `Sync ])
    (start_time : float) :
    Server_env.env
    * Relative_path.Set.t
    * Server_notifier.clock option
    * bool
    * Telemetry.t =
  let telemetry =
    Telemetry.create () |> Telemetry.duration ~key:"start" ~start_time
  in
  let (env, (raw_updates, clock, telemetry)) =
    match query_kind with
    | `Sync ->
      ( env,
        begin
          try
            let (changes, clock, telemetry) =
              Server_notifier.get_changes_sync genv.notifier telemetry
            in
            (Server_notifier.SyncChanges changes, clock, telemetry)
          with
          | Watchman.Timeout -> (Server_notifier.Unavailable, None, telemetry)
        end )
    | `Async ->
      ( { env with last_notifier_check_time = start_time },
        Server_notifier.get_changes_async genv.notifier telemetry )
    | `Skip -> (env, (Server_notifier.AsyncChanges S_set.empty, None, telemetry))
  in
  let telemetry = Telemetry.duration telemetry ~key:"notified" ~start_time in
  let unpack_updates = function
    | Server_notifier.Unavailable -> (true, S_set.empty)
    | Server_notifier.AsyncChanges updates -> (true, updates)
    | Server_notifier.SyncChanges updates ->
      (* We get SyncChanges either
         a) due to a call to to  ServerNotifier.get_changes_sync  (see match on query_kind above) or
         b) if we used ServerNotifier.get_changes_async, but the latter actually received
            changes in a synchronous fashion,
         In any case, the updates are up to date, meaning that the staleness flag is set to false *)
      (false, updates)
  in
  let (updates_stale, raw_updates) = unpack_updates raw_updates in
  let stop_pumping_on_empty_updates =
    let open Server_local_config in
    let open Server_local_config.EdenfsFileWatcher in
    (* TODO(T226505256) When using Edenfs_watcher, we currently have a Watchman-based workaround
       in place to support deferring changes while meerkat is running. However, this workaround has
       the side-effect that ServerNotifier.maybe_changes_available may still return true after
       ServerNotifier.get_changes_{sync,async} was called (and nothing happened on disk in
       after the call to ServerNotifier.get_changes_{sync,async}).
       Thus, when Edenfs_watcher is enabled, we allow terminating pump_async_updates if we get an
       empty raw_updates set from ServerNotifier.get_changes_async. *)
    genv.local_config.edenfs_file_watcher.enabled
  in
  let rec pump_async_updates acc acc_clock iteration telemetry =
    match Server_notifier.maybe_changes_available genv.notifier with
    | Some true ->
      let (changes, clock, telemetry) =
        Server_notifier.get_changes_async genv.notifier telemetry
      in
      let (_, raw_updates) = unpack_updates changes in
      if stop_pumping_on_empty_updates && S_set.is_empty raw_updates then
        (acc, clock, iteration + 1, telemetry)
      else
        pump_async_updates
          (S_set.union acc raw_updates)
          clock
          (iteration + 1)
          telemetry
    | _ -> (acc, acc_clock, iteration, telemetry)
  in
  let (raw_updates, clock, pump_iterations, telemetry) =
    pump_async_updates raw_updates clock 0 telemetry
  in
  let telemetry = Telemetry.duration telemetry ~key:"pumped" ~start_time in
  let telemetry =
    Telemetry.int_ telemetry ~key:"pump_iterations" ~value:pump_iterations
  in
  Program.exit_if_critical_update genv ~raw_updates;
  let updates =
    Find_utils.post_file_watcher_filter_from_fully_qualified_raw_updates
      ~root:(Server_args.root genv.options)
      ~raw_updates
  in
  (* CARE! For streaming-errors to work in clientCheckStatus.ml, the test
     which hh_server uses to determine "is there a non-empty set of changes which prompt me
     to start a new check" must be at least as strict as the one in clientCheckStatus.
     They're identical, in fact, because they both use the same file filter
     (FilesToIgnore.server_watch_spec) and the same post-file-watcher-filter. *)
  let telemetry =
    telemetry
    |> Telemetry.duration ~key:"processed" ~start_time
    |> Telemetry.int_ ~key:"raw_updates" ~value:(S_set.cardinal raw_updates)
    |> Telemetry.int_ ~key:"updates" ~value:(Relative_path.Set.cardinal updates)
  in
  if not @@ Relative_path.Set.is_empty updates then
    Hack_event_logger.notifier_returned start_time (S_set.cardinal raw_updates);
  (env, updates, clock, updates_stale, telemetry)

let update_stats_after_recheck :
    RecheckLoopStats.t ->
    Server_type_check.CheckStats.t ->
    telemetry:Telemetry.t ->
    start_time:seconds_since_epoch ->
    RecheckLoopStats.t =
 fun {
       RecheckLoopStats.total_changed_files_count;
       per_batch_telemetry;
       total_rechecked_count;
       updates_stale;
       recheck_id;
       last_iteration_start_time = _;
       duration;
       rust_disk_provider_read;
       time_first_result = _;
       any_full_checks = _;
     }
     {
       Server_type_check.CheckStats.total_rechecked_count =
         total_rechecked_count_in_iteration;
       reparse_count;
       time_first_result;
     }
     ~telemetry
     ~start_time ->
  {
    RecheckLoopStats.total_changed_files_count =
      total_changed_files_count + reparse_count;
    per_batch_telemetry = telemetry :: per_batch_telemetry;
    total_rechecked_count =
      total_rechecked_count + total_rechecked_count_in_iteration;
    updates_stale;
    recheck_id;
    last_iteration_start_time = start_time;
    duration = duration +. (Unix.gettimeofday () -. start_time);
    rust_disk_provider_read;
    time_first_result;
    any_full_checks = true;
  }

(* This function loops until it has processed all outstanding changes.
 *
 * One reason for doing this is so that, if a client makes a request,
 * then we can process all outstanding changes prior to handling that request.
 * That way the client will get an up-to-date answer.
 *
 * Another reason is to get meaningful logging in case of file watcher events.
 * When a rebase occurs, Watchman/dfind/Edenfs_watcher takes a while to give us the full list
 * of updates, and it often comes in batches. To get an accurate measurement
 * of rebase time, we use the heuristic that any changes that come in
 * right after one rechecking round finishes to be part of the same
 * rebase, and we don't log the recheck_end event until the update list
 * is no longer getting populated.
 *
 * The above doesn't apply in presence of interruptions / cancellations -
 * it's possible for client to request current recheck to be stopped.
 *)
let rec recheck_until_no_changes_left
    ?agent_session_id stats genv env select_outcome : RecheckLoopStats.t * env =
  let start_time = Unix.gettimeofday () in
  (* this is telemetry for the current batch, i.e. iteration: *)
  let telemetry =
    Telemetry.create () |> Telemetry.float_ ~key:"start_time" ~value:start_time
  in

  (* When a new client connects, we use the synchronous notifier.
      This is to get synchronous file system changes when invoking
      hh_client in terminal.

     CARE! The [`Skip] option doesn't in fact skip. It will still
     retrieve queued-up file watcher updates.

      NB: This also uses synchronous notify on establishing a persistent
      connection. This is harmless, but could maybe be filtered away. *)
  let query_kind =
    match select_outcome with
    | Client_provider.Select_new _ -> `Sync
    | Client_provider.Select_nothing
    | Client_provider.Select_exception _
    | Client_provider.Not_selecting_deferring _ ->
      if Float.(start_time - env.last_notifier_check_time > 0.5) then
        `Async
      else
        `Skip
  in
  let (env, updates, clock, updates_stale, query_telemetry) =
    query_notifier genv env query_kind start_time
  in
  (* The response from [query_notifier] is tricky to unpick...
     * For [`Sync | `Async] it will always return [clock=Some] and updates may be empty or not.
     * For [`Skip] it might return [clock=Some] and updates empty or not; or it might
       return [clock=None] with updates empty. *)
  let telemetry =
    telemetry
    |> Telemetry.object_ ~key:"query" ~value:query_telemetry
    |> Telemetry.duration ~key:"query_done" ~start_time
  in
  let stats = { stats with RecheckLoopStats.updates_stale } in
  (* saving any file is our trigger to start full recheck *)
  let env =
    if
      Option.is_some clock
      && not (Option.equal Server_notifier.equal_clock clock env.clock)
    then begin
      Hh_logger.log "Recheck at watchclock %s" (Server_env.show_clock clock);
      { env with clock }
    end else
      env
  in
  (* We need to union with the existing env.disk_needs_parsing here and use that to decide whether
     to start another type-check: After a complete type-check (i.e., env.needs_recheck is empty,
     and env.full_check_status is Full_check_done), we may have that env.disk_needs_parsing is
     non-empty due to the interrupt handlers adding files there. *)
  let disk_needs_parsing =
    Relative_path.Set.union updates env.disk_needs_parsing
  in
  let env =
    if Relative_path.Set.is_empty disk_needs_parsing then
      env
    else
      let full_check_status =
        match env.full_check_status with
        | Full_check_paused -> Full_check_paused
        | Full_check_needed
        | Full_check_started
        | Full_check_done ->
          Full_check_started
      in
      { env with disk_needs_parsing; full_check_status }
  in
  let telemetry = Telemetry.duration telemetry ~key:"got_updates" ~start_time in

  (* If the client went away (e.g. user pressed Ctrl+C while waiting for the typecheck),
     let's clean up now. *)
  let env =
    match env.nonpersistent_client_pending_command_needs_full_check with
    | Some (_command, _reason, client)
      when is_full_check_needed env.full_check_status
           && Float.(start_time - env.last_command_time > 5.0) -> begin
      try
        Client_provider.ping client;
        env
      with
      | Client_provider.Client_went_away ->
        Client_provider.shutdown_client client;
        {
          env with
          nonpersistent_client_pending_command_needs_full_check = None;
        }
    end
    | _ -> env
  in

  let env =
    if
      is_full_check_needed env.full_check_status
      && Float.(start_time > env.last_command_time + 5.0)
    then begin
      Hh_logger.log "Restarting full check after 5.0s";
      { env with full_check_status = Full_check_started }
    end else
      env
  in
  (* Same as above, but for persistent clients *)
  let telemetry =
    Telemetry.duration telemetry ~key:"sorted_out_client" ~start_time
  in
  (* We have some new, or previously un-processed updates *)
  let full_check = Server_env.is_full_check_started env.full_check_status in
  let telemetry =
    telemetry
    |> Telemetry.bool_ ~key:"full_check" ~value:full_check
    |> Telemetry.duration ~key:"figured_check_kind" ~start_time
  in
  if not full_check then
    let () =
      match env.full_check_status with
      | Full_check_paused ->
        if not (Relative_path.Set.is_empty updates) then
          Hh_logger.log
            ~lvl:Hh_logger.Level.Debug
            "Queued file changes while paused: %d files awaiting parsing"
            (Relative_path.Set.cardinal env.disk_needs_parsing)
      | Full_check_needed
      | Full_check_started
      | Full_check_done ->
        ()
    in
    let telemetry =
      Telemetry.string_ telemetry ~key:"check_kind" ~value:"None"
    in
    let stats =
      {
        stats with
        RecheckLoopStats.per_batch_telemetry =
          telemetry :: stats.RecheckLoopStats.per_batch_telemetry;
      }
    in
    (stats, env)
  else
    let check_kind_str = "Full_check" in
    let needed_full_init = env.init_env.why_needed_full_check in
    (* HERE'S WHERE WE DO THE HEAVY WORK! **)
    let telemetry =
      telemetry
      |> Telemetry.string_ ~key:"check_kind" ~value:check_kind_str
      |> Telemetry.duration ~key:"type_check_start" ~start_time
    in
    let (env, check_stats, type_check_telemetry) =
      Cgroup_profiler.step_group check_kind_str ~log:true
      @@ Server_type_check.type_check ?agent_session_id genv env start_time
    in
    let telemetry =
      telemetry
      |> Telemetry.object_ ~key:"type_check" ~value:type_check_telemetry
      |> Telemetry.duration ~key:"type_check_end" ~start_time
    in

    (* END OF HEAVY WORK *)

    (* Final telemetry and cleanup... *)
    begin
      match (needed_full_init, env.init_env.why_needed_full_check) with
      | (Some needed_full_init, None) ->
        finalize_init env.init_env telemetry needed_full_init
      | _ -> ()
    end;
    let telemetry =
      Telemetry.duration telemetry ~key:"finalized_and_touched" ~start_time
    in
    let stats =
      update_stats_after_recheck stats check_stats ~start_time ~telemetry
    in
    (* Tests have ability to opt-out of batching completely. *)
    if !force_break_recheck_loop_for_test_ref then
      (stats, env)
    else
      recheck_until_no_changes_left
        ?agent_session_id
        stats
        genv
        env
        select_outcome

let new_serve_iteration_id () = Random_id.short_string ()

(* This is safe to run only in the main loop, when workers are not doing
 * anything. *)
let main_loop_command_handler client result =
  match result with
  | Server_utils.Done env -> env
  | Server_utils.Needs_full_recheck { env; finish_command_handling; reason } ->
    (* We should not accept any new clients until this is cleared *)
    assert (
      Option.is_none env.nonpersistent_client_pending_command_needs_full_check);
    {
      env with
      nonpersistent_client_pending_command_needs_full_check =
        Some (finish_command_handling, reason, client);
    }

let generate_and_update_recheck_id env =
  let recheck_id = new_serve_iteration_id () in
  let env =
    {
      env with
      Server_env.init_env =
        { env.Server_env.init_env with Server_env.recheck_id = Some recheck_id };
    }
  in
  (env, recheck_id)

let idle_if_no_client env waiting_client =
  match waiting_client with
  | Client_provider.Select_nothing
  | Client_provider.Select_exception _
  | Client_provider.Not_selecting_deferring _ ->
    let {
      RecheckLoopStats.per_batch_telemetry;
      total_changed_files_count;
      total_rechecked_count;
      _;
    } =
      env.last_recheck_loop_stats
    in
    (* Ugly hack: We want GC_SHAREDMEM_RAN to record the last rechecked
     * count so that we can figure out if the largest reclamations
     * correspond to massive rebases. However, the logging call is done in
     * the Shared_mem module, which doesn't know anything about Server stuff.
     * So we wrap the call here. *)
    Hack_event_logger.with_rechecked_stats
      ~update_batch_count:(List.length per_batch_telemetry)
      ~total_changed_files:total_changed_files_count
      ~total_rechecked:total_rechecked_count
      (fun () -> Shared_mem.GC.collect `aggressive);
    let t = Unix.gettimeofday () in
    if Float.(t -. env.last_idle_job_time > 0.5) then
      let env = Server_idle.go env in
      { env with last_idle_job_time = t }
    else
      env
  | Client_provider.Select_new _ -> env

let log_recheck_end
    ?agent_session_id (stats : Server_env.RecheckLoopStats.t) ~diagnostics =
  let telemetry =
    Server_env.RecheckLoopStats.to_user_telemetry stats
    |> Telemetry.object_
         ~key:"errors"
         ~value:(Diagnostics.as_telemetry_summary diagnostics)
  in
  let {
    RecheckLoopStats.duration;
    rust_disk_provider_read = _;
    total_changed_files_count;
    total_rechecked_count;
    per_batch_telemetry;
    any_full_checks;
    recheck_id;
    updates_stale = _;
    last_iteration_start_time = _;
    time_first_result = _;
  } =
    stats
  in
  Hack_event_logger.recheck_end
    ?agent_session_id
    ~last_recheck_duration:duration
    ~update_batch_count:(List.length per_batch_telemetry - 1)
    ~total_changed_files:total_changed_files_count
    ~total_rechecked:total_rechecked_count
    (Option.some_if any_full_checks telemetry);
  Hh_logger.log
    "RECHECK_END (recheck_id %s):\n%s"
    recheck_id
    (Telemetry.to_string telemetry);
  ()

let agent_session_id_for_recheck env select_outcome =
  match env.nonpersistent_client_pending_command_needs_full_check with
  | Some (_command, _reason, client) -> Client_provider.agent_session_id client
  | None ->
    (match select_outcome with
    | Client_provider.Select_new { Client_provider.client; _ } ->
      Client_provider.agent_session_id client
    | Client_provider.Select_nothing
    | Client_provider.Select_exception _
    | Client_provider.Not_selecting_deferring _ ->
      None)

let exit_if_parent_dead () =
  (* Cross-platform compatible way; parent PID becomes 1 when parent dies. *)
  if Unix.getppid () = 1 then (
    Hh_logger.log "Server's parent has died; exiting.\n";
    Exit.exit Exit_status.Lost_parent_monitor
  )

let serve_one_iteration genv env client_provider =
  let (env, recheck_id) = generate_and_update_recheck_id env in
  exit_if_parent_dead ();
  let admission_decision =
    Deferral_tracker.should_accept_client_connection
      ~local_config:genv.local_config
      genv.notifier
  in
  let acceptable_new_client_kind =
    let has_default_client_pending =
      Option.is_some env.nonpersistent_client_pending_command_needs_full_check
    in
    match (admission_decision, has_default_client_pending) with
    (* If we are already blocked on some client, do not accept more of them.
     * Other clients (that connect through priority pipe, or persistent clients)
     * can still be handled - unless deferral blocks admission, where we want to
     * stop accepting any new clients, with the exception of forced ones. *)
    | (Deferral_tracker.Ok, true) -> Some `Priority
    | (Deferral_tracker.Ok, false) -> Some `Any
    | (Deferral_tracker.Deferred_by _, true) -> None
    | (Deferral_tracker.Deferred_by _, false) -> Some `Force_dormant_start_only
  in
  let selected_client =
    match acceptable_new_client_kind with
    | None -> Client_provider.Select_nothing
    | Some client_kind ->
      Client_provider.sleep_and_check
        client_provider
        ~idle_gc_slice:genv.local_config.Server_local_config.idle_gc_slice
        client_kind
  in
  let selected_client =
    match (admission_decision, selected_client) with
    | (Deferral_tracker.Deferred_by states, Client_provider.Select_nothing) ->
      Client_provider.Not_selecting_deferring states
    | _ -> selected_client
  in

  let env =
    match selected_client with
    | Client_provider.Select_new _ -> begin
      match env.full_check_status with
      | Full_check_paused ->
        (* We can unconditionally switch to [Full_check_done] here: The invariant for
           [Full_check_paused] tells us that [env.needs_recheck] is empty.
           [recheck_until_no_changes_left] will switch to [Full_check_started] later if
           [env.disk_needs_parsing] is non-empty. *)
        { env with full_check_status = Full_check_done }
      | Full_check_needed
      | Full_check_started
      | Full_check_done ->
        env
    end
    | Client_provider.Select_nothing
    | Client_provider.Select_exception _
    | Client_provider.Not_selecting_deferring _ ->
      env
  in

  (* Server_progress: By updating status now at the start of the serve_one_iteration,
   * it means there's no obligation on the "doing work" part of the previous
   * iteration to clean up its own status-reporting once done.
   *
   * Caveat: that's not quite true, since ClientProvider.sleep_and_check will
   * wait up to 0.1s if there are no pending requests. So theoretically we
   * won't update our status for up to 0.1s after the previous work is done.
   * That doesn't really matter, since (1) if there are no pending requests
   * then no client will even ask for status, and (2) it's worth it to
   * keep the code clean and simple.
   *
   * By the same token, we will be writing "ready" once every 0.1s to the status file.
   * Think of it as a heartbeat!
   *
   * Note: the message here might soon be replaced. If we discover disk changes
   * that prompt a typecheck, then typechecking sends its own status updates.
   * And if the selected_client was a request, then once we discover the nature
   * of that request then ServerCommand.handle will send its own status updates too.
   *)
  begin
    match selected_client with
    | Client_provider.(Select_nothing | Select_exception _) ->
      (* There's some subtle IDE behavior, described in [ServerCommand.handle]
         and [Server_main.recheck_until_no_changes_left]... If an EDIT was received
         over the persistent connection, then we won't resume typechecking
         until either a file-save comes in or 5.0s has elapsed. *)
      let (disposition, msg) =
        match env.full_check_status with
        | Full_check_needed -> (Server_progress.DWorking, "will resume")
        | Full_check_started -> (Server_progress.DWorking, "typechecking")
        | Full_check_done -> (Server_progress.DReady, "ready")
        | Full_check_paused ->
          ( Server_progress.DReady,
            "paused until the next client request (e.g., hh --stats)" )
      in
      Server_progress.write ~include_in_logs:false ~disposition "%s" msg
    | Client_provider.Not_selecting_deferring states ->
      Server_progress.write
        ~include_in_logs:false
        "waiting for %s"
        (String.concat ~sep:", " states)
    | Client_provider.Select_new _ ->
      Server_progress.write ~include_in_logs:false "working"
  end;
  let env = idle_if_no_client env selected_client in
  let agent_session_id = agent_session_id_for_recheck env selected_client in
  let stage =
    if Option.is_some env.init_env.why_needed_full_check then
      `Init
    else
      `Recheck
  in
  Hack_event_logger.with_id ~stage recheck_id @@ fun () ->
  (* We'll first do "recheck_until_no_changes_left" to handle all outstanding changes, so that
   * after that we'll be able to give an up-to-date answer to the client.
   * Except: this might be stopped early in some cases, e.g. IDE checks. *)
  let t_start_recheck = Unix.gettimeofday () in
  let rust_disk_provider_read_start =
    Rust_disk_provider_read_telemetry.get_totals ()
  in
  let (stats, env) =
    recheck_until_no_changes_left
      ?agent_session_id
      (RecheckLoopStats.empty ~recheck_id)
      genv
      env
      selected_client
  in
  let t_done_recheck = Unix.gettimeofday () in
  let stats =
    {
      stats with
      RecheckLoopStats.rust_disk_provider_read =
        Rust_disk_provider_read_telemetry.(
          delta ~start:rust_disk_provider_read_start ~finish:(get_totals ()));
    }
  in
  let t_sent_diagnostics = Unix.gettimeofday () in
  let stats =
    Server_env.RecheckLoopStats.record_result_sent_ts
      stats
      (Some t_sent_diagnostics)
  in
  let did_work = stats.RecheckLoopStats.total_rechecked_count > 0 in
  let env =
    {
      env with
      last_recheck_loop_stats = stats;
      last_recheck_loop_stats_for_actual_work =
        (if did_work then
          Some stats
        else
          env.last_recheck_loop_stats_for_actual_work);
    }
  in

  if did_work then
    log_recheck_end ?agent_session_id stats ~diagnostics:env.diagnostics;

  let env =
    match selected_client with
    | Client_provider.Select_nothing
    | Client_provider.Select_exception _
    | Client_provider.Not_selecting_deferring _ ->
      env
    | Client_provider.Select_new { Client_provider.client; m2s_sequence_number }
      -> begin
      try
        Hh_logger.log
          "Serving new client obtained from monitor handoff #%d"
          m2s_sequence_number;
        (* client here is the new client (not the existing persistent client)
         * whose request we're going to handle. *)
        Client_provider.track
          client
          ~key:Connection_tracker.Server_start_recheck
          ~time:t_start_recheck;
        Client_provider.track
          client
          ~key:Connection_tracker.Server_done_recheck
          ~time:t_done_recheck;
        Client_provider.track
          client
          ~key:Connection_tracker.Server_sent_diagnostics
          ~time:t_sent_diagnostics;
        let env =
          Client_command_handler.handle_client_command_or_persistent_connection
            genv
            env
            client
          |> main_loop_command_handler client
        in
        Hack_event_logger.handled_connection t_start_recheck;
        env
      with
      | exn ->
        let e = Exception.wrap exn in
        Hack_event_logger.handle_connection_exception "outer" e;
        Hh_logger.log
          "HANDLE_CONNECTION_EXCEPTION(outer) [ignoring request] %s"
          (Exception.to_string e);
        env
    end
  in
  let env =
    match env.nonpersistent_client_pending_command_needs_full_check with
    | Some (f, _reason, _client) when is_full_check_done env.full_check_status
      ->
      {
        (f env) with
        nonpersistent_client_pending_command_needs_full_check = None;
      }
    | _ -> env
  in
  env

(** This synthesizes a [Multi_threaded_call.Cancel] in the event that we want
a typecheck cancelled due to files changing on disk. It constructs the
human-readable [user_message] and also [log_message] appropriately. *)
let cancel_due_to_file_changes
    (updates : Relative_path.Set.t) (clock : Server_notifier.clock option) :
    Multi_threaded_call.interrupt_result =
  assert (not (Relative_path.Set.is_empty updates));
  let size = Relative_path.Set.cardinal updates in
  let examples =
    (List.take (Relative_path.Set.elements updates) 5
    |> List.map ~f:Relative_path.suffix)
    @
    if size > 5 then
      ["..."]
    else
      []
  in
  let timestamp = Unix.gettimeofday () in
  let tm = Unix.localtime timestamp in
  Multi_threaded_call.Cancel
    {
      Multi_threaded_call.user_message =
        Printf.sprintf
          "Files have changed on disk! [%02d:%02d:%02d] %s"
          tm.Unix.tm_hour
          tm.Unix.tm_min
          tm.Unix.tm_sec
          (List.hd_exn examples);
      log_message =
        Printf.sprintf
          "file change interrupt handler at clock %s. %d files changed. [%s]"
          (Server_env.show_clock clock)
          (Relative_path.Set.cardinal updates)
          (String.concat examples ~sep:",");
      timestamp;
    }

let file_changes_interrupt_handler genv :
    env Multi_threaded_call.interrupt_handler =
 fun env ->
  let start_time = Unix.gettimeofday () in
  let (env, updates, clock, updates_stale, _telemetry) =
    query_notifier genv env `Async start_time
  in
  (* Async updates can always be stale, so we don't care *)
  ignore updates_stale;
  let size = Relative_path.Set.cardinal updates in
  if size > 0 then (
    Hh_logger.log
      "Interrupted by file watcher message: %d files changed at watchclock %s"
      size
      (Server_env.show_clock clock);
    ( {
        env with
        disk_needs_parsing =
          Relative_path.Set.union env.disk_needs_parsing updates;
        clock;
      },
      cancel_due_to_file_changes updates clock )
  ) else
    (env, Multi_threaded_call.Continue)

(** Handler for events on the priority socket, which is used for priority commands which
    must be served immediately. *)
let priority_client_interrupt_handler genv client_provider :
    env Multi_threaded_call.interrupt_handler =
 fun env ->
  let t = Unix.gettimeofday () in
  Hh_logger.log "Handling message on priority socket.";
  match
    Deferral_tracker.should_accept_client_connection
      ~local_config:genv.local_config
      genv.notifier
  with
  | Deferral_tracker.Deferred_by states ->
    Hh_logger.log
      "Won't handle client message: waiting for %s."
      (String.concat ~sep:", " states);
    (env, Multi_threaded_call.Continue)
  | Deferral_tracker.Ok ->
    (* For non-persistent clients that don't synchronize file contents, users
     * expect that a query they do immediately after saving a file will reflect
     * this file contents. Async notifications are not always fast enough to
     * guarantee it, so we need an additional sync query before accepting such
     * client *)
    let (env, updates, clock, _updates_stale, _telemetry) =
      query_notifier genv env `Sync t
    in
    let n_updates = Relative_path.Set.cardinal updates in
    if n_updates > 0 then (
      Hh_logger.log
        "Interrupted by file watcher sync query: %d files changed at watchclock %s"
        n_updates
        (Server_env.show_clock clock);
      ( {
          env with
          disk_needs_parsing =
            Relative_path.Set.union env.disk_needs_parsing updates;
          clock;
        },
        cancel_due_to_file_changes updates clock )
    ) else
      let idle_gc_slice = genv.local_config.Server_local_config.idle_gc_slice in
      let select_outcome =
        Client_provider.sleep_and_check client_provider ~idle_gc_slice `Priority
      in
      let env =
        match select_outcome with
        | Client_provider.Select_nothing ->
          (* This is possible because client might have gone away during
           * sleep_and_check. *)
          Hh_logger.log "Client went away.";
          env
        | Client_provider.Select_exception e ->
          Hh_logger.log
            "Exception during client FD select: %s"
            (Exception.get_ctor_string e);
          env
        | Client_provider.Not_selecting_deferring _ -> env
        | Client_provider.Select_new
            { Client_provider.client; m2s_sequence_number } ->
          Hh_logger.log
            "Serving new client obtained from monitor handoff #%d"
            m2s_sequence_number;
          (match
             Client_command_handler
             .handle_client_command_or_persistent_connection
               genv
               env
               client
           with
          | Server_utils.Needs_full_recheck { reason; _ } ->
            failwith
              ("unexpected command needing full recheck in priority channel: "
              ^ reason)
          | Server_utils.Done env -> env)
      in

      (env, Multi_threaded_call.Continue)

let setup_interrupts env client_provider =
  {
    env with
    interrupt_handlers =
      (fun genv _env ->
        let {
          Server_local_config.interrupt_on_file_changes;
          interrupt_on_client;
          _;
        } =
          genv.local_config
        in
        let handlers = [] in
        let handlers =
          match Server_notifier.notification_fd genv.notifier with
          | Some fd when interrupt_on_file_changes ->
            (fd, file_changes_interrupt_handler genv) :: handlers
          | _ -> handlers
        in
        let handlers =
          match Client_provider.priority_fd client_provider with
          | Some fd when interrupt_on_client ->
            (fd, priority_client_interrupt_handler genv client_provider)
            :: handlers
          | _ -> handlers
        in
        handlers);
  }

let serve genv env in_fds =
  if genv.local_config.Server_local_config.ide_parser_cache then
    Ide_parser_cache.enable ();
  (* During server lifetime dependency table can be not up-to-date. Because of
   * that, we ban access to it be default, forcing the code trying to read it to
   * take it into account, either by explcitely enabling reads (and being fine
   * with stale results), or declaring (in ServerCommand) that it requires full
   * check to be completed before being executed. *)
  let (_ : bool) =
    Typing_deps.allow_dependency_table_reads env.deps_mode false
  in
  let () = Diagnostics.set_allow_errors_in_default_path false in
  Multi_threaded_call.on_exception (fun e -> Server_utils.exit_on_exception e);
  let client_provider = Client_provider.provider_from_file_descriptors in_fds in

  (* This is needed when typecheck_after_init option is disabled.
   * We're just filling it with placeholder telemetry values since
   * we don't much care about this scenario. *)
  let init_telemetry =
    Server_env.Init_telemetry.make
      Server_env.Init_telemetry.Init_typecheck_disabled_after_init
      (Telemetry.create ()
      |> Telemetry.string_
           ~key:"mode"
           ~value:"serve_due_to_disabled_typecheck_after_init")
  in
  let typecheck_telemetry = Telemetry.create () in
  if Option.is_none env.init_env.why_needed_full_check then
    finalize_init env.init_env typecheck_telemetry init_telemetry;

  let env = setup_interrupts env client_provider in
  let env = ref env in
  while true do
    let new_env = serve_one_iteration genv !env client_provider in
    env := new_env
  done

(* Rules for whether+how to load saved-state...
 * 1. If hh.conf lacks "use_mini_state = true", then don't load it.
 * 2. If hh_server --no-load, then don't load it.
 * 3. If hh_server --save-mini or -s, then save but don't load it.
 * 4. If "hh_server --with-mini-state", then load the one specified there!
 * 5. If hh.conf lacks "load_state_natively_v4", then don't load it
 * 6. Otherwise, load it normally!
 *)
let resolve_init_approach genv : Server_init.init_approach * string =
  if Option.is_some (Server_args.save_naming_filename genv.options) then
    (Server_init.Parse_only_init, "Server_args_saving_naming")
  else if Server_args.no_load genv.options then
    (Server_init.Full_init, "Server_args_no_load")
  else if
    (not genv.local_config.Server_local_config.use_saved_state)
    && Option.is_none (Server_args.write_symbol_info genv.options)
  then
    (Server_init.Full_init, "Local_config_saved_state_disabled")
  else if Option.is_some (Server_args.write_symbol_info genv.options) then
    match
      ( genv.local_config.Server_local_config.use_saved_state_when_indexing,
        Server_args.with_saved_state genv.options )
    with
    | (false, None) ->
      (Server_init.Write_symbol_info, "Server_args_writing_symbol_info")
    | (true, None) ->
      ( Server_init.Write_symbol_info_with_state Server_init.Load_state_natively,
        "Server_args_writing_symbol_info_load_native" )
    | (_, Some (Server_args.Saved_state_target_info target)) ->
      ( Server_init.Write_symbol_info_with_state (Server_init.Precomputed target),
        "Server_args_writing_symbol_info_precomputed" )
  else
    match
      ( genv.local_config.Server_local_config.load_state_natively,
        Server_args.with_saved_state genv.options )
    with
    | (_, Some (Server_args.Saved_state_target_info target)) ->
      ( Server_init.Saved_state_init (Server_init.Precomputed target),
        "Precomputed" )
    | (false, None) ->
      (Server_init.Full_init, "No_native_loading_or_precomputed")
    | (true, None) ->
      (* Use native loading only if the config specifies a load script,
       * and the local config prefers native. *)
      ( Server_init.Saved_state_init Server_init.Load_state_natively,
        "Load_state_natively" )

let program_init genv env =
  Hh_logger.log "Init id: %s" env.init_env.init_id;
  Server_progress.with_message "initializing..." @@ fun () ->
  Server_progress.enable_error_production
    genv.local_config.Server_local_config.produce_streaming_errors;
  Exit.add_hook_upon_clean_exit (fun _finale_data ->
      Server_progress.ErrorsWrite.unlink_at_server_stop ());
  let env =
    {
      env with
      init_env =
        { env.init_env with ci_info = Some (Ci_util.begin_get_info ()) };
    }
  in

  let (init_approach, approach_name) = resolve_init_approach genv in
  Hh_logger.log "Initing with approach: %s" approach_name;

  let (env, init_type, init_error, init_error_telemetry, saved_state_revs_info)
      =
    let (env, init_result) = Server_init.init ~init_approach genv env in
    match init_approach with
    | Server_init.Write_symbol_info
    | Server_init.Full_init ->
      (env, "fresh", None, None, None)
    | Server_init.Parse_only_init -> (env, "parse-only", None, None, None)
    | Server_init.Write_symbol_info_with_state _
    | Server_init.Saved_state_init _ -> begin
      match init_result with
      | Server_init.Load_state_succeeded saved_state_revs_info ->
        let init_type =
          match
            Naming_table.get_forward_naming_fallback_path env.naming_table
          with
          | None -> "state_load_blob"
          | Some _ -> "state_load_sqlite"
        in
        (env, init_type, None, None, Some saved_state_revs_info)
      | Server_init.Load_state_failed (err, telemetry) ->
        (env, "state_load_failed", Some err, Some telemetry, None)
      | Server_init.Load_state_declined reason ->
        (env, "state_load_declined", Some reason, None, None)
    end
  in
  let env =
    {
      env with
      init_env =
        {
          env.init_env with
          saved_state_revs_info;
          approach_name;
          init_error;
          init_type;
        };
    }
  in
  Hh_logger.log "Waiting for daemon(s) to be ready...";
  Server_progress.write "wrapping up init...";
  Server_notifier.wait_until_ready genv.notifier;
  Event_logger.set_init_type init_type;
  let telemetry =
    Server_utils.log_and_get_sharedmem_load_telemetry ()
    |> Telemetry.object_
         ~key:"rust_disk_provider_read"
         ~value:
           Rust_disk_provider_read_telemetry.(get_totals () |> to_telemetry)
    |> Telemetry.object_opt ~key:"init_error" ~value:init_error_telemetry
    |> Telemetry.json
         ~key:"deps_mode"
         ~value:(Typing_deps_mode.to_opaque_json env.deps_mode)
  in
  Hack_event_logger.init_lazy_end
    telemetry
    ~approach_name
    ~init_error
    ~init_type;
  env

let num_workers options local_config =
  (* The number of workers is set both in hh.conf and as an optional server argument.
     if the two numbers given in argument and in hh.conf are different, we always take the minimum
     of the two.
  *)
  let max_procs_opt =
    Option.merge
      ~f:(fun a b ->
        if Int.equal a b then
          a
        else (
          Hh_logger.warn
            ("Warning: both an argument --max-procs and a local config "
            ^^ "for max workers are given. Choosing minimum of the two.");
          min a b
        ))
      (Server_args.max_procs options)
      local_config.Server_local_config.max_workers
  in
  let nbr_procs = Sys_utils.nbr_procs in
  match max_procs_opt with
  | None ->
    (* The hardware we are running on has only 1/2
       actual CPUs, the rest are hyperthreads. Using worker processes for
       hyperthreads is slower than using just the number of actual computation
       cores. *)
    max 1 (nbr_procs / 2)
  | Some max_procs ->
    if max_procs <= nbr_procs then
      max_procs
    else (
      Hh_logger.warn
        "Warning: max workers is higher than the number of processors. Capping.";
      nbr_procs
    )

(** Setup files for IPC. These are used to transmit status messages and errors. *)
let setup_ipc root =
  (* There are three files which are used for IPC.
     1. server_finale_file - we unlink it now upon startup,
        and upon clean exit we'll write finale-date to it.
     2. server_receipt_to_monitor_file - we'll unlink it now upon startup,
        and upon clean exit we'll unlink it.
     3. server_progress_file - we write "starting up" to it now upon startup,
        and upon clean exit we'll write "shutting down" to it.
     In both case of clean exit and abrupt exit there'll be leftover files.
     We'll rely upon tmpclean to eventually clean them up. *)
  let pid = Unix.getpid () in
  Server_progress.set_root root;
  let server_finale_file = Server_files.server_finale_file pid in
  let server_receipt_to_monitor_file =
    Server_files.server_receipt_to_monitor_file pid
  in
  (try Unix.unlink server_finale_file with
  | _ -> ());
  (try Unix.unlink server_receipt_to_monitor_file with
  | _ -> ());
  Exit.add_hook_upon_clean_exit (fun finale_data ->
      begin
        try Unix.unlink server_receipt_to_monitor_file with
        | _ -> ()
      end;
      begin
        try
          Sys_utils.with_umask 0o000 (fun () ->
              let oc = Stdlib.open_out_bin server_finale_file in
              Marshal.to_channel oc finale_data [];
              Stdlib.close_out oc)
        with
        | _ -> ()
      end;
      begin
        try Server_progress.write "shutting down" with
        | _ -> ()
      end;
      ())

let configure_gc () =
  Stdlib.Gc.set
    {
      (Stdlib.Gc.get ()) with
      Stdlib.Gc.max_overhead =
        (* The OCaml default is 500, but we care about minimizing the memory
           overhead *)
        200;
    }

let initialize_logging
    ~informant_managed
    ~is_worker
    ~num_workers
    ~init_id
    options
    config
    local_config
    ~root : unit =
  Hh_logger.Level.set_min_level local_config.Server_local_config.min_log_level;
  Hh_logger.Level.set_categories local_config.Server_local_config.log_categories;

  let hhconfig_version =
    config |> Server_config.version |> Config_file.version_to_string_opt
  in
  if is_worker && Option.is_some (Server_args.ai_mode options) then
    Event_logger.init_fake ()
  else if is_worker then
    Hack_event_logger.init_worker
      ~root
      ~hhconfig_version
      ~init_id
      ~custom_columns:(Server_args.custom_telemetry_data options)
      ~rollout_flags:(Server_local_config_load.to_rollout_flags local_config)
      ~rollout_group:local_config.Server_local_config.rollout_group
      ~time:(Unix.gettimeofday ())
      ~per_file_profiling:local_config.Server_local_config.per_file_profiling
  else
    Hack_event_logger.init
      ~root
      ~hhconfig_version
      ~init_id
      ~custom_columns:(Server_args.custom_telemetry_data options)
      ~informant_managed
      ~rollout_flags:(Server_local_config_load.to_rollout_flags local_config)
      ~rollout_group:local_config.Server_local_config.rollout_group
      ~time:(Unix.gettimeofday ())
      ~max_workers:num_workers
      ~per_file_profiling:local_config.Server_local_config.per_file_profiling

let check_nfs ~root options local_config =
  let root_s = Path.to_string root in
  let check_mode = Server_args.check_mode options in
  if
    (not check_mode)
    && Sys_utils.is_nfs root_s
    && not local_config.Server_local_config.enable_on_nfs
  then (
    Hh_logger.log "Refusing to run on %s: root is on NFS!" root_s;
    Hack_event_logger.nfs_root ();
    Exit.exit Exit_status.Nfs_root
  )

let warn_on_non_opt_build options config =
  if
    Server_config.warn_on_non_opt_build config
    && not Build_id.is_build_optimized
  then begin
    let msg =
      Printf.sprintf
        "hh_server binary was built in \"%s\" mode, "
        Build_id.build_mode
      ^ "is running with Rust version of parser enabled, "
      ^ "and this repository's .hhconfig specifies warn_on_non_opt_build option. "
      ^ "Parsing with non-opt build will take significantly longer"
    in
    if Server_args.allow_non_opt_build options then
      Hh_logger.log
        "Warning: %s. Initializing anyway due to --allow-non-opt-build option."
        msg
    else
      let msg =
        Printf.sprintf
          "Error: %s. Recompile the server in opt or dbgo mode, or pass --allow-non-opt-build to continue anyway."
          msg
      in
      Hh_logger.log "%s" msg;
      Exit.exit ~msg Exit_status.Server_non_opt_build_mode
  end

let get_deps_mode options =
  match Server_args.save_64bit options with
  | Some new_edges_dir ->
    let human_readable_dep_map_dir =
      Server_args.save_human_readable_64bit_dep_map options
    in
    Typing_deps_mode.SaveToDiskMode
      { graph = None; new_edges_dir; human_readable_dep_map_dir }
  | None -> Typing_deps_mode.InMemoryMode None

let make_workers
    ~num_workers
    ~informant_managed
    ~init_id
    ~root
    shmem_handle
    options
    config
    local_config : Multi_worker.worker list =
  (* Make a sub-init_id because we use it to name temporary files for piping to
     scuba logging processes. *)
  let worker_logging_init () =
    initialize_logging
      ~num_workers
      ~informant_managed
      ~init_id:(init_id ^ "." ^ Random_id.short_string ())
      ~is_worker:true
      ~root
      options
      config
      local_config
  in
  let gc_control = Server_config.gc_control config in
  Server_worker.make
    ~longlived_workers:local_config.Server_local_config.longlived_workers
    ~set_global_provider_backend_in_workers:
      local_config.Server_local_config.set_global_provider_backend_in_workers
    ~nbr_procs:num_workers
    gc_control
    shmem_handle
    ~logging_init:worker_logging_init

let log_pids root ~monitor_pid =
  Pid_log.init (Server_files.pids_file root);
  Option.iter monitor_pid ~f:(fun monitor_pid ->
      Pid_log.log ~reason:"monitor" monitor_pid);
  Pid_log.log ~reason:"main" (Unix.getpid ())

(** Does a bunch of operations to get the server up and running, among other things:
  - Initialize shared memory
  - Create a unique init ID
  - Creates a bunch of files for IPC, e.g. for errors and status transmission.
  - Configure GC
  - initialize logging
  - initialize workers *)
let setup_server
    ~(informant_managed : bool)
    ~(monitor_pid : int option)
    (options : Server_args.options)
    (config : Server_config.t)
    (local_config : Server_local_config.t) : Multi_worker.worker list * env =
  let num_workers = num_workers options local_config in
  let shmem_handle =
    Shared_mem.init ~num_workers (Server_config.sharedmem_config config)
  in
  let init_id = Random_id.short_string () in
  let root = Server_args.root options in

  setup_ipc root;

  Server_progress.write "starting up";
  Hh_logger.log "Version: %s" Hh_version.version;
  Hh_logger.log "Hostname: %s" (Unix.gethostname ());

  configure_gc ();

  List.iter (Server_config.ignored_paths config) ~f:Files_to_ignore.ignore_path;

  initialize_logging
    ~is_worker:false
    ~init_id
    ~informant_managed
    ~num_workers
    ~root
    options
    config
    local_config;

  Hack_event_logger.init_start
    ~experiments_config_meta:
      local_config.Server_local_config.experiments_config_meta
    (Memory_stats.get_host_hw_telemetry ());

  check_nfs ~root options local_config;
  warn_on_non_opt_build options config;

  Program.set_signals ();

  let { Server_local_config.cpu_priority; io_priority; _ } = local_config in
  Sys_utils.set_priorities ~cpu_priority ~io_priority;

  log_pids root ~monitor_pid;

  let workers =
    make_workers
      ~num_workers
      ~informant_managed
      ~init_id
      ~root
      shmem_handle
      options
      config
      local_config
  in
  let env =
    Server_env_build.make_env config ~init_id ~deps_mode:(get_deps_mode options)
  in

  (workers, env)

let possibly_save_naming_table env genv =
  match Server_args.save_naming_filename genv.options with
  | None -> ()
  | Some filename ->
    Disk.mkdir_p (Filename.dirname filename);
    let save_result = Naming_table.save env.naming_table filename in
    Hh_logger.log
      "Inserted symbols into the naming table:\n%s"
      (Naming_sqlite.show_save_result save_result);
    if not (List.is_empty save_result.Naming_sqlite.errors) then (
      Sys_utils.rm_dir_tree filename;
      failwith "Naming table state had errors - deleting output file!"
    )

let run_once options config local_config =
  assert (Server_args.check_mode options);
  Hh_logger.log "Server_main.run_once starting";

  let (workers, env) =
    setup_server
      options
      config
      local_config
      ~informant_managed:false
      ~monitor_pid:None
  in
  let genv = Server_env_build.make_genv options config local_config workers in

  (* The type-checking happens here *)
  let env = program_init genv env in
  possibly_save_naming_table env genv;
  (* Finish up by generating the output and the exit code *)
  Hh_logger.log "Running in check mode";
  Program.run_once_and_exit genv env

let log_pid_cgroup () =
  let pid = Unix.getpid () in
  match Proc_fs.first_cgroup_for_pid pid with
  | Ok cgroup ->
    Hh_logger.log "Server Pid: %d" pid;
    Hh_logger.log "Server cGroup: %s" cgroup
  | _ -> ()

let log_server_ready () =
  Hh_logger.log "Server is partially ready";
  Hack_event_logger.server_is_partially_ready ()

let time f =
  let t = Unix.gettimeofday () in
  let res = f () in
  let t' = Unix.gettimeofday () in
  Hh_logger.log "Took %f seconds." (t' -. t);
  res

(*
 * The server monitor will pass client connections to this process
 * via ic.
 *)
let daemon_main_exn ~informant_managed options monitor_pid in_fds =
  assert (not (Server_args.check_mode options));
  (* This invokes fbinit logic, which subsumes Folly.ensure_folly_init () *)
  Startup_initializer.init ();
  Printexc.record_backtrace true;

  Hh_logger.log "Server_main daemon starting.";

  let (config, local_config) =
    Server_config.load
      ~silent:false
      ~from:(Server_args.from options)
      ~cli_config_overrides:(Server_args.config options)
  in
  Option.iter local_config.Server_local_config.memtrace_dir ~f:(fun dir ->
      Daemon.start_memtracing (Filename.concat dir "memtrace.server.ctf"));
  let (workers, env) =
    setup_server
      options
      config
      local_config
      ~informant_managed
      ~monitor_pid:(Some monitor_pid)
  in
  let genv = Server_env_build.make_genv options config local_config workers in

  Hack_event_logger.with_id ~stage:`Init env.init_env.init_id @@ fun () ->
  log_pid_cgroup ();
  Hh_logger.log "Initializing Server (This might take some time)";

  let env = time @@ fun () -> program_init genv env in

  Server_idle.init genv (Server_args.root options);
  log_server_ready ();

  serve genv env in_fds

type params = {
  informant_managed: bool;
  state: Server_global_state.t;
  options: Server_args.options;
  monitor_pid: int;
  priority_in_fd: Unix.file_descr;
  force_dormant_start_only_in_fd: Unix.file_descr;
}

let setup_hhi_root options =
  match Server_args.custom_hhi_path options with
  | None ->
    (* Restore hhi files every time the server restarts
       in case the tmp folder changes *)
    ignore (Hhi.get_hhi_root ())
  | Some path ->
    if Disk.file_exists path && Disk.is_directory path then (
      Hh_logger.log "Custom hhi directory set to %s." path;
      Hhi.set_custom_hhi_root (Path.make path)
    ) else (
      Hh_logger.log "Custom hhi directory %s not found." path;
      Exit.exit Exit_status.Input_error
    )

let daemon_main
    {
      informant_managed;
      state;
      options;
      monitor_pid;
      priority_in_fd;
      force_dormant_start_only_in_fd;
    }
    (default_ic, _default_oc) =
  (* Avoid leaking this fd further *)
  let () = Unix.set_close_on_exec priority_in_fd in
  let () = Unix.set_close_on_exec force_dormant_start_only_in_fd in
  let default_in_fd = Daemon.descr_of_in_channel default_ic in

  (* Restore the root directory and other global states from monitor *)
  Server_global_state.restore state ~worker_id:0;

  setup_hhi_root options;

  Server_utils.with_exit_on_exception @@ fun () ->
  daemon_main_exn
    ~informant_managed
    options
    monitor_pid
    (default_in_fd, priority_in_fd, force_dormant_start_only_in_fd)

let entry = Daemon.register_entry_point "Server_main.daemon_main" daemon_main
