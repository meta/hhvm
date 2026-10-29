(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude

type changes =
  | Unavailable
      (** e.g. because DFind is not available, or watchman subscription is down *)
  | SyncChanges of S_set.t
      (** contains all changes up to the point that the notifier was invoked *)
  | AsyncChanges of S_set.t
      (** contains some of the changes up to the point that the notifier was invoked,
          but there may be more pending changes that have not been included *)

type clock = Server_notifier_types.clock =
  | Watchman of Watchman.clock
  | Eden of Edenfs_watcher.clock
[@@deriving show, eq]

let show_file_watcher_name = function
  | Watchman _ -> "Watchman"
  | Eden _ -> "Edenfs_watcher"

let handle_edenfs_watcher_result
    (result : ('t, Edenfs_watcher_types.edenfs_watcher_error) result) : 't =
  match result with
  | Result.Error (Edenfs_watcher_types.EdenfsWatcherError msg) ->
    Hh_logger.log "Edenfs_watcher failed with message: %s" msg;
    raise Exit_status.(Exit_with Edenfs_watcher_failed)
  | Result.Error Edenfs_watcher_types.NonEdenWWW ->
    Hh_logger.log "Edenfs_watcher failed, www repo is not on Eden";
    raise Exit_status.(Exit_with Edenfs_watcher_failed)
  | Result.Error (Edenfs_watcher_types.LostChanges reason) ->
    Hh_logger.log "Edenfs_watcher has lost track of changes, reason: %s" reason;
    raise Exit_status.(Exit_with Edenfs_watcher_lost_changes)
  | Result.Error (Edenfs_watcher_types.EdenShuttingDown msg) ->
    Hh_logger.log "Edenfs_watcher detected Eden shutting down: %s" msg;
    raise Exit_status.(Exit_with Edenfs_watcher_eden_shutting_down)
  | Result.Ok value -> value

type t =
  | IndexOnly of { root: Path.t }
  | Dfind of {
      root: Path.t;
      ready: bool ref;
      dfind: Dfind_lib.t;
    }
  | Watchman of {
      wenv: Watchman.env;
      watchman: Watchman.watchman_instance ref;
          (** Watchman state can change during requests (see Watchamn.Watchman_dead and Watchman_alive).
          This reference will be updated as necessary to the new instance. *)
      root: Path.t;
      local_config: Server_local_config.t;
      num_workers: int;
    }
  | EdenfsFileWatcher of {
      instance: Edenfs_watcher.instance;
      num_workers: int;
      root: Path.t;
      local_config: Server_local_config.t;
      last_clock: Edenfs_watcher.clock ref;
          (** Clock as of the last time we have received changed files.
          Concretely, clock as of the last call to get_changes_sync or get_changes_async
          that returned a non-empty set of changes.
          (or the initial clock, if no changes received so far *)
    }
  | MockChanges of {
      get_changes_async: unit -> changes;
      get_changes_sync: unit -> S_set.t;
    }

type indexer = (string -> bool) -> unit -> string list

(** This returns an "indexer", i.e. unit -> string list, which when invoked
will return all files under root. *)
let indexer (t : t) (filter : string -> bool) : unit -> string list =
  match t with
  | Dfind { root; _ }
  | IndexOnly { root; _ } ->
    Find.make_next_files ~name:"root" ~filter root
  | MockChanges _ -> failwith "indexer not mocked"
  | Watchman { wenv; num_workers; _ } ->
    let files = Watchman.get_all_files wenv in
    Bucket.make_list ~num_workers (List.filter ~f:filter files)
  | EdenfsFileWatcher { instance; num_workers; _ } ->
    let (files, _telemetry_opt) =
      Edenfs_watcher.get_all_files instance |> handle_edenfs_watcher_result
    in
    Hh_logger.debug
      "Edenfs_watcher.get_all_files returned %d files"
      (List.length files);
    Bucket.make_list ~num_workers (List.filter ~f:filter files)

let init
    (options : Server_args.options)
    (local_config : Server_local_config.t)
    ~(num_workers : int) : t * indexer =
  let root = Server_args.root options in
  let watchman_config = local_config.Server_local_config.watchman in
  let watchman_enabled = watchman_config.Server_local_config.Watchman.enabled in
  let edenfs_watcher_config =
    local_config.Server_local_config.edenfs_file_watcher
  in
  let edenfs_watcher_enabled =
    edenfs_watcher_config.Server_local_config.EdenfsFileWatcher.enabled
  in

  let init_dfind () =
    Hh_logger.log "Using dfind";
    let in_fd = Daemon.null_fd () in
    let log_link = Server_files.dfind_log root in
    let log_file = Sys_utils.make_link_of_timestamped log_link in
    let log_fd = Daemon.fd_of_path log_file in
    let dfind =
      Dfind_lib.init
        (in_fd, log_fd, log_fd)
        (Global_config.scuba_table_name, [root])
    in
    Hack_event_logger.set_file_watcher_dfind ();
    Dfind { root; ready = ref false; dfind }
  in

  (* helper to try to construct Watchman, or return None if failed *)
  let try_init_watchman () =
    Hh_logger.log "Using watchman";
    let Server_local_config.Watchman.
          { sockname; subscribe; init_timeout; debug_logging; _ } =
      watchman_config
    in

    let wenv =
      Watchman.init
        {
          Watchman.init_timeout = Watchman.Explicit_timeout (float init_timeout);
          subscribe_mode =
            (if subscribe then
              Some Watchman.Defer_changes
            else
              None);
          expression_terms = Files_to_ignore.watchman_server_expression_terms;
          debug_logging =
            Server_args.watchman_debug_logging options || debug_logging;
          sockname;
          subscription_prefix = "hh_type_check_watcher";
          roots = [root];
        }
        ()
    in
    Option.map wenv ~f:(fun wenv ->
        Hack_event_logger.set_file_watcher_watchman ();
        Watchman
          {
            wenv;
            watchman = ref (Watchman.Watchman_alive wenv);
            root;
            local_config;
            num_workers;
          })
  in

  let try_init_edenfs_watcher () : t option =
    Hh_logger.log "Using EdenFS file watcher";
    let watch_spec = Files_to_ignore.server_watch_spec in
    let {
      Server_local_config.EdenfsFileWatcher.debug_logging;
      timeout_secs;
      throttle_time_ms;
      report_telemetry;
      state_tracking;
      sync_queries_obey_deferral;
      tracked_states;
      _;
    } =
      local_config.edenfs_file_watcher
    in
    (* If state tracking and hg_aware are both enabled, verify that tracked_states
       includes the hg states. *)
    if state_tracking && local_config.hg_aware then begin
      if
        not
          (List.mem tracked_states Hg_states.transaction ~equal:String.equal
          && List.mem tracked_states Hg_states.update ~equal:String.equal)
      then
        failwith
          "state_tracking and hg_aware are enabled, but not tracking hg.update and hg.transaction"
    end;
    let init_settings =
      {
        Edenfs_watcher_types.root;
        watch_spec;
        debug_logging;
        timeout_secs;
        throttle_time_ms;
        report_telemetry;
        state_tracking;
        sync_queries_obey_deferral;
        tracked_states;
      }
    in
    match Edenfs_watcher.init ~destroy_on_exit:true init_settings with
    | Result.Error (Edenfs_watcher_types.EdenfsWatcherError msg) ->
      Hh_logger.log
        "Failed to initialize EdenFS watcher, failed with message:\n%s"
        msg;
      Hack_event_logger.edenfs_watcher_fallback ~msg;
      None
    | Result.Error Edenfs_watcher_types.NonEdenWWW ->
      let msg =
        Printf.sprintf
          "Failed to initialize EdenFS watcher, www repo %s is not on Eden"
          (Path.to_string root)
      in
      Hh_logger.log "%s" msg;
      Hack_event_logger.edenfs_watcher_fallback ~msg;
      None
    | Result.Error (Edenfs_watcher_types.LostChanges reason) ->
      let msg =
        Printf.sprintf
          "Failed to initialize EdenFS watcher with lost changes message, reason %s"
          reason
      in
      Hh_logger.log "%s" msg;
      Hack_event_logger.edenfs_watcher_fallback ~msg;
      None
    | Result.Error (Edenfs_watcher_types.EdenShuttingDown reason) ->
      let msg =
        Printf.sprintf
          "Failed to initialize EdenFS watcher because Eden is shutting down: %s"
          reason
      in
      Hh_logger.log "%s" msg;
      Hack_event_logger.edenfs_watcher_fallback ~msg;
      None
    | Result.Ok (instance, initial_clock) ->
      let last_clock = ref initial_clock in
      Hack_event_logger.set_file_watcher_edenfs ();
      Some
        (EdenfsFileWatcher
           { instance; num_workers; root; local_config; last_clock })
  in

  if edenfs_watcher_enabled && watchman_enabled then
    Hh_logger.warn
      "Both Watchman and EdenFS file watching enabled in server config, will prefer the latter";

  (* We just use these lazy values to make the initialization logic less branchy. *)
  let lazy_edenfs_watcher = lazy (try_init_edenfs_watcher ()) in
  let lazy_watchman = lazy (try_init_watchman ()) in

  let notifier =
    if Server_args.check_mode options then (
      (* check_mode *)
      Hh_logger.log "Not using any file watching mechanism";
      IndexOnly { root }
    ) else if
        edenfs_watcher_enabled
        && (Option.is_some @@ Lazy.force lazy_edenfs_watcher)
      then
      (* This value_exn cannot fail, we just checked that this is Some in the previous line *)
      Option.value_exn @@ Lazy.force lazy_edenfs_watcher
    else if watchman_enabled && (Option.is_some @@ Lazy.force lazy_watchman)
    then
      (* This value_exn cannot fail, we just checked that this is Some in the previous line *)
      Option.value_exn @@ Lazy.force lazy_watchman
    else
      init_dfind ()
  in

  (notifier, indexer notifier)

let init_mock
    ~(get_changes_async : unit -> changes) ~(get_changes_sync : unit -> S_set.t)
    : t =
  MockChanges { get_changes_async; get_changes_sync }

let init_null () : t =
  let f () = SyncChanges S_set.empty in
  let g () = S_set.empty in
  init_mock ~get_changes_async:f ~get_changes_sync:g

(** Whether [root] is on an Eden mount, decided the way [Edenfs_watcher] decides
    it: the nearest ancestor that is a repository root (it has a [.hg] or a
    [.git]) has an [.eden/socket]. *)
let on_eden_mount (root : Path.t) : bool =
  let exists dir name =
    Sys_utils.file_exists (Stdlib.Filename.concat (Path.to_string dir) name)
  in
  let rec repo_root dir =
    if exists dir ".hg" || exists dir ".git" then
      Some dir
    else
      let parent = Path.parent dir in
      if Path.equal parent dir then
        None
      else
        repo_root parent
  in
  match repo_root root with
  | Some dir -> exists dir (Stdlib.Filename.concat ".eden" "socket")
  | None -> false

let uses_watchman_on_eden_mount (t : t) : bool =
  match t with
  | Watchman { root; _ } -> on_eden_mount root
  | EdenfsFileWatcher _
  | Dfind _
  | IndexOnly _
  | MockChanges _ ->
    false

let wait_until_ready (t : t) : unit =
  match t with
  | Dfind { ready; dfind; _ } ->
    if !ready then
      ()
    else begin
      Dfind_lib.wait_until_ready dfind;
      ready := true
    end
  | IndexOnly _ -> ()
  | MockChanges _ -> ()
  | Watchman _ ->
    (* The initial watch-project command blocks until watchman's crawl is
       done, so we don't have anything else to wait for here. *)
    ()
  | EdenfsFileWatcher _ ->
    (* Same as for Watchman *)
    ()

(** Helper conversion function, from a single watchman-changes to a set of changed
    files. Also handles informing ServerRevisionTracker about changes *)
let convert_watchman_changes
    ~(root : Path.t)
    ~(local_config : Server_local_config.t)
    (watchman_changes : Watchman.pushed_changes) : S_set.t =
  match watchman_changes with
  | Watchman.Changed_merge_base _ ->
    let () =
      Hh_logger.log "Error: Typechecker does not use Source Control Aware mode"
    in
    raise Exit_status.(Exit_with Watchman_invalid_result)
  | Watchman.State_enter (name, _metadata) ->
    if local_config.Server_local_config.hg_aware then
      Server_revision_tracker.Watchman.on_state_enter name;
    S_set.empty
  | Watchman.State_leave (name, metadata) ->
    if local_config.Server_local_config.hg_aware then
      Server_revision_tracker.Watchman.on_state_leave root name metadata;
    S_set.empty
  | Watchman.Files_changed changes ->
    Server_revision_tracker.files_changed local_config (S_set.cardinal changes);
    changes

(** Helper to find the earliest translated_at timestamp. The resulting age is added to [telemetry].
    This value gives us an idea how long we have been deferring a change. *)
let eden_add_oldest_change_age_telemetry
    (changes : Edenfs_watcher_types.changes list) telemetry : Telemetry.t =
  let oldest_translated_at =
    List.fold_left changes ~init:None ~f:(fun acc c ->
        match c with
        | Edenfs_watcher_types.FileChanges { translated_at; _ }
        | Edenfs_watcher_types.CommitTransition { translated_at; _ } ->
          (match acc with
          | None -> Some translated_at
          | Some oldest -> Some (Float.min oldest translated_at))
        | Edenfs_watcher_types.StateEnter _
        | Edenfs_watcher_types.StateLeave _ ->
          acc)
  in
  match oldest_translated_at with
  | Some oldest ->
    let age_secs = Unix.gettimeofday () -. oldest in
    Telemetry.float_ ~key:"oldest_change_age" ~value:age_secs telemetry
  | None -> telemetry

let convert_edenfs_watcher_changes
    local_config root (eden_changes : Edenfs_watcher_types.changes) : S_set.t =
  let state_tracking =
    local_config.Server_local_config.edenfs_file_watcher.state_tracking
  in
  let changed_files =
    match eden_changes with
    | Edenfs_watcher_types.CommitTransition { file_changes; to_commit; _ } ->
      (* TODO(T224461521) Need to inform ServerRevisionTracker about commit
         transition, similarly to what convert_watchman_changes does *)
      if state_tracking && local_config.Server_local_config.hg_aware then
        Server_revision_tracker.Edenfs_watcher.on_commit_transition
          root
          to_commit;
      S_set.of_list file_changes
    | Edenfs_watcher_types.FileChanges { files; _ } ->
      (* TODO(T215219438) Need to inform ServerRevisionTracker about changed files,
         similarly to what convert_watchman_changes does *)
      S_set.of_list files
    | Edenfs_watcher_types.StateEnter name ->
      Hh_logger.debug "ServerNotifier: StateEnter(%s)" name;
      if state_tracking && local_config.Server_local_config.hg_aware then
        Server_revision_tracker.Edenfs_watcher.on_state_enter name;
      S_set.empty
    | Edenfs_watcher_types.StateLeave name ->
      Hh_logger.debug "ServerNotifier: StateLeave(%s)" name;
      if state_tracking && local_config.Server_local_config.hg_aware then
        Server_revision_tracker.Edenfs_watcher.on_state_leave root name;
      S_set.empty
  in
  Server_revision_tracker.files_changed
    local_config
    (S_set.cardinal changed_files);
  changed_files

let get_changes_sync (t : t) telemetry : S_set.t * clock option * Telemetry.t =
  let (changes, new_clock, telemetry) =
    match t with
    | IndexOnly _ -> (S_set.empty, None, telemetry)
    | MockChanges { get_changes_sync; _ } ->
      (get_changes_sync (), None, telemetry)
    | Dfind { dfind; _ } ->
      let set =
        try
          Timeout.with_timeout
            ~timeout:120
            ~on_timeout:(fun (_ : Timeout.timings) ->
              Exit.exit Exit_status.Dfind_unresponsive)
            ~do_:(fun _timeout -> Dfind_lib.get_changes dfind)
        with
        | _ -> Exit.exit Exit_status.Dfind_died
      in
      (set, None, telemetry)
    | Watchman { local_config; watchman; root; _ } ->
      let start_time = Unix.gettimeofday () in
      let (watchman', changes) =
        Watchman.get_changes_synchronously
          ~timeout:
            local_config.Server_local_config.watchman
              .Server_local_config.Watchman.synchronous_timeout
          !watchman
      in
      let telemetry =
        Telemetry.add_duration ~key:"sync_watcher" ~start_time telemetry
      in
      watchman := watchman';
      let changes =
        List.fold_left changes ~init:S_set.empty ~f:(fun acc c ->
            S_set.union acc (convert_watchman_changes ~root ~local_config c))
      in
      let clock = Watchman.get_clock !watchman in
      (changes, Some (Server_notifier_types.Watchman clock), telemetry)
    | EdenfsFileWatcher { instance; root; local_config; last_clock; _ } ->
      let start_time = Unix.gettimeofday () in
      (* Note that this will handle all errors by raising Exit_status *)
      let (changes, new_clock, sync_telemetry_opt) =
        handle_edenfs_watcher_result (Edenfs_watcher.get_changes_sync instance)
      in
      let telemetry =
        Telemetry.add_duration ~key:"sync_watcher" ~start_time telemetry
      in
      let telemetry =
        Option.value_map
          ~default:telemetry
          sync_telemetry_opt
          ~f:(fun sync_telemetry ->
            Telemetry.object_
              ~key:"get_changes_sync"
              ~value:sync_telemetry
              telemetry)
      in
      let telemetry = eden_add_oldest_change_age_telemetry changes telemetry in
      let changes_set =
        List.fold_left changes ~init:S_set.empty ~f:(fun acc c ->
            S_set.union acc (convert_edenfs_watcher_changes local_config root c))
      in
      if not (S_set.is_empty changes_set) then last_clock := new_clock;
      (changes_set, Some (Server_notifier_types.Eden !last_clock), telemetry)
  in

  if
    Hh_logger.Level.passes_min_level Hh_logger.Level.Debug
    && not (S_set.is_empty changes)
  then begin
    Hh_logger.log
      ~lvl:Hh_logger.Level.Debug
      "ServerNotifier.get_changes_sync got %d changes"
      (S_set.cardinal changes);
    S_set.iter
      (fun file ->
        Hh_logger.log
          ~lvl:Hh_logger.Level.Debug
          "ServerNotifier.get_changes_sync: changed file %s"
          file)
      changes
  end;
  (changes, new_clock, telemetry)

let get_changes_async (t : t) telemetry : changes * clock option * Telemetry.t =
  let (changes, new_clock, telemetry) =
    match t with
    | IndexOnly _ -> (SyncChanges S_set.empty, None, telemetry)
    | MockChanges { get_changes_async; _ } ->
      (get_changes_async (), None, telemetry)
    | Dfind _ ->
      let (changes, _, telemetry) = get_changes_sync t telemetry in
      (SyncChanges changes, None, telemetry)
    | Watchman { watchman; root; local_config; _ } ->
      let start_time = Unix.gettimeofday () in
      let (watchman', changes) = Watchman.get_changes !watchman in
      let telemetry =
        Telemetry.add_duration ~key:"async_watcher" ~start_time telemetry
      in
      watchman := watchman';
      let changes =
        match changes with
        | Watchman.Watchman_unavailable -> Unavailable
        | Watchman.Watchman_pushed changes ->
          AsyncChanges (convert_watchman_changes ~root ~local_config changes)
        | Watchman.Watchman_synchronous changes ->
          let accumulated_changes =
            List.fold_left changes ~init:S_set.empty ~f:(fun acc c ->
                S_set.union acc (convert_watchman_changes ~root ~local_config c))
          in
          SyncChanges accumulated_changes
      in
      let clock = Watchman.get_clock !watchman in

      (changes, Some (Server_notifier_types.Watchman clock), telemetry)
    | EdenfsFileWatcher { instance; root; local_config; last_clock; _ } ->
      let start_time = Unix.gettimeofday () in
      (* Note that this will handle all errors by raising Exit_status *)
      let (changes, new_clock, async_telemetry_opt) =
        handle_edenfs_watcher_result (Edenfs_watcher.get_changes_async instance)
      in
      let telemetry =
        Telemetry.add_duration ~key:"async_watcher" ~start_time telemetry
      in
      let telemetry =
        Option.value_map
          ~default:telemetry
          async_telemetry_opt
          ~f:(fun async_telemetry ->
            Telemetry.object_
              ~key:"get_changes_async"
              ~value:async_telemetry
              telemetry)
      in
      let telemetry = eden_add_oldest_change_age_telemetry changes telemetry in
      let changes_set =
        List.fold_left changes ~init:S_set.empty ~f:(fun acc c ->
            S_set.union acc (convert_edenfs_watcher_changes local_config root c))
      in
      if not (S_set.is_empty changes_set) then last_clock := new_clock;
      (AsyncChanges changes_set, Some (Eden !last_clock), telemetry)
  in

  if Hh_logger.Level.passes_min_level Hh_logger.Level.Debug then begin
    let change_set =
      match changes with
      | Unavailable -> S_set.empty
      | AsyncChanges set
      | SyncChanges set ->
        set
    in
    if not (S_set.is_empty change_set) then begin
      Hh_logger.log
        ~lvl:Hh_logger.Level.Debug
        "ServerNotifier.get_changes_async got %d changes"
        (S_set.cardinal change_set);
      S_set.iter
        (fun file ->
          Hh_logger.log
            ~lvl:Hh_logger.Level.Debug
            "ServerNotifier.get_changes_async: changed file %s"
            file)
        change_set
    end
  end;
  (changes, new_clock, telemetry)

let notification_fd (t : t) : Caml_unix.file_descr option =
  match t with
  | Dfind _
  | IndexOnly _ ->
    None
  | MockChanges _ -> None
  | Watchman { watchman; _ } ->
    Option.map (Watchman.get_reader !watchman) ~f:Buffered_line_reader.get_fd
  | EdenfsFileWatcher { instance; _ } ->
    let fd_res = Edenfs_watcher.get_notification_fd instance in
    (* Note that this will handle all errors by raising Exit_status *)
    let fd = handle_edenfs_watcher_result fd_res in
    Some fd

let maybe_changes_available (t : t) : bool option =
  let fd_opt = notification_fd t in
  Option.map fd_opt ~f:(fun fd ->
      let (readable, _, _) = Caml_unix.select [fd] [] [] 0.0 in
      not (List.is_empty readable))

let get_deferring_states (t : t) : string list =
  match t with
  | EdenfsFileWatcher { instance; _ } ->
    Edenfs_watcher.get_asserted_states instance
    |> handle_edenfs_watcher_result
    |> List.sort ~compare:String.compare
  | Watchman _ ->
    (* Watchman deferral tracking is intentionally unsupported. *)
    []
  | IndexOnly _
  | Dfind _
  | MockChanges _ ->
    []

let get_repo_states_telemetry (t : t) : Telemetry.t =
  let (current_states, past_states) =
    match t with
    | EdenfsFileWatcher { instance; _ } ->
      Edenfs_watcher.get_repo_states instance
    | Watchman _ -> Watchman.RepoStates.get ()
    | IndexOnly _
    | Dfind _
    | MockChanges _ ->
      ([], S_map.empty)
  in
  let past_states_telemetry =
    S_map.fold
      (fun name ts t -> Telemetry.float_ ~key:name ~value:ts t)
      past_states
      (Telemetry.create ())
  in
  Telemetry.create ()
  |> Telemetry.string_list ~key:"current_states" ~value:current_states
  |> Telemetry.object_ ~key:"past_states" ~value:past_states_telemetry
