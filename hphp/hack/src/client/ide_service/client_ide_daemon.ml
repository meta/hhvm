(*
 * Copyright (c) 2019, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude

(** For debugging. When we get to [at_exit], we'll log what the current activities are. *)
let dbg_current_activities : string list S_map.t ref = ref S_map.empty

(** Each activity key is associated with up to five most recent timestamped activity values
under that key. *)
let dbg_set_activity ~(key : string) (value : string) : unit =
  let history =
    S_map.find_opt key !dbg_current_activities |> Option.value ~default:[]
  in
  let history =
    Printf.sprintf "  %s %s" (Unix.gettimeofday () |> Utils.timestring) value
    :: List.take history 4
  in
  dbg_current_activities := S_map.add key history !dbg_current_activities;
  ()

(** This prints a multiline string: for each activity key, the most recent activity values for that key. *)
let dbg_dump_activity () : string =
  S_map.bindings !dbg_current_activities
  |> List.map ~f:(fun (key, history) ->
         key :: List.rev history |> String.concat ~sep:"\n")
  |> String.concat ~sep:"\n"

(** These are messages on Client_ide_daemon's internal message-queue *)
type message =
  | ClientRequest : 'a Client_ide_message.tracked_t -> message
      (** ClientRequest came from Client_ide_service over stdin;
      it expects a response. *)
  | Commit_transition_changes : Relative_path.Set.t -> message
  | GotNamingTable :
      (Client_ide_init.init_result, Client_ide_message.rich_error) result
      -> message
      (** GotNamingTable is posted from within Client_ide_daemon itself once
      our attempt at loading saved-state has finished; it's picked
      up by handle_messages. *)

type message_queue = message Lwt_message_queue.t

exception Outfd_write_error of string * string

let is_outfd_write_error (exn : Exception.t) : bool =
  match Exception.unwrap exn with
  | Outfd_write_error _ -> true
  | _ -> false

type common_state = {
  hhi_root: Path.t;
      (** hhi_root files are written during initialize, deleted at shutdown, and
      refreshed periodically in case the tmp-cleaner has deleted them. *)
  config: Server_config.t; [@opaque]
  local_config: Server_local_config.t; [@opaque]
  local_memory: Provider_backend.local_memory; [@opaque]
      (** Local_memory backend; includes decl caches *)
}
[@@deriving show]

type open_file_state = {
  entry: Provider_context.entry;
  diagnostics: Diagnostics.t option ref;
}

(** The [entry] caches the TAST+diagnostics; the [Diagnostics.t option] stores what was
the most recent version of the diagnostics to have been returned to clientLsp
by didOpen/didChange/didClose/codeAction. *)
type open_files_state = open_file_state Relative_path.Map.t

(** istate, "initialized state", is the state the daemon after it has
finished initialization (i.e. finished loading saved state),
concerning these data-structures:
1. forward-naming-table-delta stored in naming_table
2. reverse-naming-table-delta-and-cache stored in local_memory
3. entries with source text, stored in open_files
3. cached ASTs and TASTs, stored in open_files
4. shallow-decl-cache, folded-decl-cache stored in local-memory

There are two concepts to understand.
1. "Singleton context" (ctx). When processing IDE requests for a file, we create
   a context object in which that entry's source text and AST and TAST
   are present in the context, and no others are.
2. "Quarantine with respect to an entry". We enter a quarantine while
   computing the TAST for a singleton context entry. The invariants within
   the quarantine are different from those without.

The key algorithms which read from these data-structures are:
1. Ast_provider.get_ast will fetch the cached AST for an entry in ctx, or if
   the entry is present but as yet lacks an AST then it will parse and cache,
   or if it's lookinng for the AST of a file not in ctx then it will parse
   off disk but decline to cache.
2. Naming_provider.get_* will get_ast for ctx entry to see if symbol is there.
   If not it will look in reverse-delta-and-cache or read from sqlite
   and store the answer back in reverse-delta-and-cache. But if the answer
   to that fallback was a file in ctx, then it will say that the symbol's
   not defined.
3. Decl_provider.get_shallow_class_* will look it up in shallow-decl-cache, and otherwise
   will ask Naming_provider and Ast_provider for the AST, will compute shallow decl,
   and will store it in shallow-decl-cache
4. Decl_provider.get_* will look it up in folded-decl-cache, computing it if
   not there using shallow provider, and store it back in folded-decl-cache
5. Tast_provider.compute* is only ever called on entries. It returns the cached
   TAST if present; otherwise, it runs normal type-checking-and-inference, relies
   upon all the other providers, and writes the answer back in the entry's TAST cache.

The invariants for forward and reverse naming tables:
1. These tables only ever reflect truth about disk files; they are unaffected
   by open_file entries.
2. They are updated in response to watched-file events and optional commit transitions.
   Because these events arrive asynchronously,
   we might for instance find ourselves being asked to compute a TAST
   by reading the naming-table and fetching a shallow-decl from a file
   that doesn't even exist on disk any more (even though we don't yet know it).

The invariants for AST, TAST, shallow, and folded-decl caches:
1. AST, if present, reflects the AST of its entry's source text,
   and is a "full" AST (not decl-only), and has errors.
2. TAST, if present, reflects the TAST of its entry's source text computed
   against the on-disk state of all other files
3. Outside a quarantine, all entries in shallow cache are correct as of disk
   (at least as far as asynchronous file updates have been processed).
4. Likewise, all entries in folded caches are correct as of disk.
5. We only ever enter quarantine with respect to one single entry.
   For the duration of the quarantine, an AST for that entry,
   if present, is correct as of the entry's source text.
6. Likewise any shallow decls for an entry are correct as of its source text.
   Moreover, if shallow decls for an entry are present, then the entry's AST
   is present and contains those symbols.
7. Any shallow decls not for the entry are correct as of disk.
8. During quarantine, the shallow-decl of all other files is correct as of disk.
9. The entry's TAST, along with every single decl,
   are correct as of this entry's source text plus every other file off disk.

Here are the algorithms we use that satisfy those invariants.
1. Upon a disk-file-change, we invalidate all TASTs (satisfying invariant 2).
   We use the forward-naming-table to find all "old" symbols that were
   defined in the file prior to the disk change, and invalidate those
   shallow decls (satisfying invariant 3). We invalidate all
   folded caches (satisfying invariant 4). Invariant 1 is N/A.
2. Upon an editor change to a file, we invalidate the entry's AST and TAST
   (satisfying invariant 1).
3. Upon request for a TAST of a file, we create a singleton context for
   that entry, and enter quarantine as follows. We parse the file and
   cache its AST and invalidate shallow decls for all symbols inside
   this new AST (satisfying invariant 6). We invalidate all decls
   (satisfying invariant 9). Subsequent fetches,
   thanks to the "key algorithms for reading these datastructures" (above)
   will only cache things in accordance with invariants 6,7,8,9.
4. We leave quarantine as follows. We invalidate shallow decls for
   all symbols in the entry's AST; thanks to invariant 5, this will
   fulfill invariant 3. We invalidate all decls (satisfying invariant 4).
*)
type istate = {
  icommon: common_state;
  iopen_files: open_files_state; [@opaque]
  naming_table: Naming_table.t; [@opaque]
      (** the forward-naming-table is constructed during initialize and updated
      during process_changed_files. It stores an in-memory map of FileInfos that
      have changed since sqlite. When a file is changed on disk, we need this to
      know which shallow decls to invalidate. Note: while the forward-naming-table
      is stored here, the reverse-naming-table is instead stored in ctx. *)
  error_filter: Tast_provider.ErrorFilter.t; [@opaque]
  sienv: Search_utils.si_env; [@opaque]
      (** sienv provides autocomplete and find-symbols. It is constructed during
      initialize and updated during process_changed_files. It stores a few
      in-memory structures such as namespace-list, plus in-memory deltas. *)
}

(** dstate, "during_init state", is the state the daemon after it has received an
init message (and has parsed config files to get popt/tcopt, has initialized
glean, as written out hhi files) but before it has loaded saved-state or processed
file updates. *)
type dstate = {
  start_time: float;
      (** When did we kick off the attempt to load saved-state? *)
  dcommon: common_state;
  dopen_files: open_files_state; [@opaque]
  changed_files_to_process: Relative_path.Set.t;
      (** [changed_files_to_process] is grown [During_init] upon [Did_change_watched_files changes]
    and then discharged in [initialize2] before changing to [Initialized] state. *)
}
[@@deriving show]

type state =
  | Pending_init  (** We haven't yet received init request *)
  | During_init of dstate
      (** We're working on the init request. We're still in
      the process of loading the saved state. *)
  | Initialized of istate  (** Finished work on init request. *)
  | Failed_init of Client_ide_message.rich_error
      (** Failed request, with root cause *)

type t = {
  message_queue: message_queue;
  state: state;
}

let state_to_log_string (state : state) : string =
  let open_files_to_log_string (open_files : open_files_state) : string =
    Printf.sprintf "%d open_files" (Relative_path.Map.cardinal open_files)
  in
  match state with
  | Pending_init -> "Pending_init"
  | During_init { dopen_files; changed_files_to_process; _ } ->
    Printf.sprintf
      "During_init(%s, %d changed during)"
      (open_files_to_log_string dopen_files)
      (Relative_path.Set.cardinal changed_files_to_process)
  | Initialized { iopen_files; _ } ->
    Printf.sprintf "Initialized(%s)" (open_files_to_log_string iopen_files)
  | Failed_init reason ->
    Printf.sprintf "Failed_init(%s)" reason.Client_ide_message.category

let log s = Hh_logger.log ("[ide-daemon] " ^^ s)

let log_debug s = Hh_logger.debug ("[ide-daemon] " ^^ s)

let set_up_hh_logger_for_client_ide_service (root : Path.t) : unit =
  (* Log to a file on disk. Note that calls to `Hh_logger` will always write to
     `stderr`; this is in addition to that. *)
  let client_ide_log_fn = Server_files.client_ide_log root in
  begin
    try Sys.rename client_ide_log_fn (client_ide_log_fn ^ ".old") with
    | _e -> ()
  end;
  Hh_logger.set_log client_ide_log_fn;
  log "Starting client IDE service at %s" client_ide_log_fn

let write_message
    ~(out_fd : Lwt_unix.file_descr)
    ~(message : Client_ide_message.message_from_daemon) : unit Lwt.t =
  try%lwt
    let%lwt (_ : int) = Marshal_tools_lwt.to_fd_with_preamble out_fd message in
    Lwt.return_unit
  with
  | Unix.Unix_error (Unix.EPIPE, fn, param) ->
    raise @@ Outfd_write_error (fn, param)

let log_startup_time
    ?(count : int option) (component : string) (start_time : float) : float =
  let now = Unix.gettimeofday () in
  Hack_event_logger.serverless_ide_startup ?count ~start_time component;
  now

let restore_hhi_root_if_necessary (istate : istate) : istate =
  if Sys.file_exists (Path.to_string istate.icommon.hhi_root) then
    istate
  else
    (* Some processes may clean up the temporary HHI directory we're using.
       Assume that such a process has deleted the directory, and re-write the HHI
       files to disk. *)
    let hhi_root = Hhi.get_hhi_root ~force_write:true () in
    log
      "Old hhi root %s no longer exists. Creating a new hhi root at %s"
      (Path.to_string istate.icommon.hhi_root)
      (Path.to_string hhi_root);
    Relative_path.set_path_prefix Relative_path.Hhi hhi_root;
    { istate with icommon = { istate.icommon with hhi_root } }

(** Deletes the hhi files we've created. *)
let remove_hhi (state : state) : unit =
  match state with
  | Pending_init
  | Failed_init _ ->
    ()
  | During_init { dcommon = { hhi_root; _ }; _ }
  | Initialized { icommon = { hhi_root; _ }; _ } ->
    let hhi_root = Path.to_string hhi_root in
    log "Removing hhi directory %s..." hhi_root;
    (try Sys_utils.rm_dir_tree hhi_root with
    | exn ->
      let e = Exception.wrap exn in
      Client_ide_utils.log_bug "remove_hhi" ~e ~telemetry:true)

type batch_trigger =
  | Initialization
  | Lsp_file_changes
  | Commit_transition
  | Test_index
[@@deriving show { with_path = false }]

(** Helper called to process a batch of file changes.
  Updates the naming table, and invalidates the decl and tast caches for the changes. *)
let batch_update_naming_table_and_invalidate_caches
    ~(trigger : batch_trigger)
    ~(ctx : Provider_context.t)
    ~(naming_table : Naming_table.t)
    ~(sienv : Search_utils.si_env)
    ~(local_memory : Provider_backend.local_memory)
    ~(open_files : open_files_state)
    (changes : Relative_path.Set.t) : Naming_table.t * Search_utils.si_env =
  let start_time = Unix.gettimeofday () in
  let Client_ide_incremental.{ changes; naming_table; sienv } =
    Client_ide_incremental.update_naming_tables_and_si
      ~ctx
      ~naming_table
      ~sienv
      ~changes
  in
  let telemetry =
    Provider_utils.invalidate_upon_file_changes
      ~ctx
      ~local_memory
      ~changes
      ~entries:(Relative_path.Map.map open_files ~f:(fun { entry; _ } -> entry))
    |> Telemetry.string_ ~key:"trigger" ~value:(show_batch_trigger trigger)
  in
  Hack_event_logger.ProfileTypeCheck.invalidate
    ~count:(List.length changes)
    ~start_time
    ~path:
      (List.hd changes |> Option.map ~f:(fun change -> change.File_info.path))
    telemetry;
  (naming_table, sienv)

(** An empty ctx with no entries *)
let make_empty_ctx (common : common_state) : Provider_context.t =
  Provider_context.empty_for_tool
    ~popt:(Server_config.parser_options common.config)
    ~tcopt:(Server_config.typechecker_options common.config)
    ~backend:(Provider_backend.Local_memory common.local_memory)
    ~deps_mode:(Typing_deps_mode.InMemoryMode None)

let process_changed_files
    ~trigger (state : state) (changes : Relative_path.Set.t) : state =
  match state with
  | During_init dstate ->
    let changed_files_to_process =
      Relative_path.Set.union dstate.changed_files_to_process changes
    in
    During_init { dstate with changed_files_to_process }
  | Initialized ({ icommon; naming_table; sienv; iopen_files; _ } as istate) ->
    let (naming_table, sienv) =
      batch_update_naming_table_and_invalidate_caches
        ~trigger
        ~ctx:(make_empty_ctx icommon)
        ~naming_table
        ~sienv
        ~local_memory:icommon.local_memory
        ~open_files:iopen_files
        changes
    in
    Initialized { istate with naming_table; sienv }
  | Pending_init
  | Failed_init _ ->
    state

(** Constructs a temporary ctx with just one entry. *)
let make_singleton_ctx (common : common_state) (entry : Provider_context.entry)
    : Provider_context.t =
  let ctx = make_empty_ctx common in
  let ctx = Provider_context.add_or_overwrite_entry ~ctx entry in
  ctx

(** Initializes:
  - HackEventLogger and Hh_logger
  - Root, hhi and tmp paths
  - Load configs
  - Local memory for the provider backend *)
let initialize1
    ({
       Client_ide_message.Initialize_from_saved_state.root;
       config;
       open_files;
       naming_table_load_info = _;
       warnings_saved_state_path = _;
       ignore_hh_version = _;
     } :
      Client_ide_message.Initialize_from_saved_state.t) : dstate =
  log_debug "initialize1";
  let start_time = Unix.gettimeofday () in
  Hack_event_logger.serverless_ide_set_root root;
  set_up_hh_logger_for_client_ide_service root;

  Relative_path.set_path_prefix Relative_path.Root root;
  let hhi_root = Hhi.get_hhi_root () in
  log "Extracted hhi files to directory %s" (Path.to_string hhi_root);
  Relative_path.set_path_prefix Relative_path.Hhi hhi_root;
  Relative_path.set_path_prefix Relative_path.Tmp (Path.make "/tmp");

  let (config, local_config) =
    Server_config.load ~silent:true ~cli_config_overrides:config ~from:""
  in
  Hack_event_logger.set_hhconfig_version
    (Server_config.version config |> Config_file.version_to_string_opt);
  Hack_event_logger.set_rollout_flags
    (Server_local_config_load.to_rollout_flags local_config);
  Hack_event_logger.set_rollout_group
    local_config.Server_local_config.rollout_group;

  Provider_backend.set_local_memory_backend
    ~max_num_decls:5000
    ~max_num_folded_class_decls:5000
    ~max_num_shallow_class_decls:20000;
  let local_memory =
    match Provider_backend.get () with
    | Provider_backend.Local_memory local_memory -> local_memory
    | _ -> failwith "expected local memory backend"
  in

  (* We only ever serve requests on files that are open. That's why our caller
     passes an initial list of open files, the ones already open in the editor
     at the time we were launched. We don't actually care about their contents
     at this stage, since updated contents will be delivered upon each request.
     (and indeed it's pointless to waste time reading existing contents off disk).
     All we care is that every open file is listed in 'open_files'. *)
  let dopen_files =
    open_files
    |> List.map ~f:(fun path ->
           path |> Path.to_string |> Relative_path.create_detect_prefix)
    |> List.map ~f:(fun path ->
           ( path,
             {
               entry =
                 Provider_context.make_entry
                   ~path
                   ~contents:Provider_context.Raise_exn_on_attempt_to_read;
               diagnostics = ref None;
             } ))
    |> Relative_path.Map.of_list
  in
  let start_time =
    log_startup_time "initialize1" ~count:(List.length open_files) start_time
  in
  log_debug "initialize1.done";
  {
    start_time;
    dcommon = { hhi_root; config; local_config; local_memory };
    dopen_files;
    changed_files_to_process = Relative_path.Set.empty;
  }

(** initialize2 is called by handle_one_message upon receipt of a
[GotNamingTable] message. It sends the appropriate message on to the
client, and transitions into either [Initialized] or [Failed_init]
state. *)
let initialize2
    (out_fd : Lwt_unix.file_descr)
    (dstate : dstate)
    ~error_filter
    (init_result :
      (Client_ide_init.init_result, Client_ide_message.rich_error) result) :
    state Lwt.t =
  let start_time = log_startup_time "load_naming_table" dstate.start_time in
  log_debug "initialize2";
  match init_result with
  | Ok
      {
        Client_ide_init.naming_table;
        warnings_saved_state;
        sienv;
        changed_files;
      } ->
    let changed_files_to_process =
      Relative_path.Set.union
        dstate.changed_files_to_process
        (Relative_path.Set.of_list changed_files)
      |> Relative_path.Set.filter ~f:Find_utils.path_filter
    in
    let (naming_table, sienv) =
      batch_update_naming_table_and_invalidate_caches
        ~trigger:Initialization
        ~ctx:(make_empty_ctx dstate.dcommon)
        ~naming_table
        ~sienv
        ~local_memory:dstate.dcommon.local_memory
        ~open_files:dstate.dopen_files
        changed_files_to_process
    in
    let istate =
      {
        naming_table;
        error_filter =
          { Tast_provider.ErrorFilter.warnings_saved_state; error_filter };
        sienv;
        icommon = dstate.dcommon;
        iopen_files = dstate.dopen_files;
      }
    in
    (* Note: Done_init is needed to (1) transition clientIdeService state, (2) cause
       clientLsp to know to ask for squiggles to be refreshed on open files. *)
    let%lwt () =
      write_message
        ~out_fd
        ~message:
          (Client_ide_message.Notification
             (Client_ide_message.Done_init (Ok ())))
    in
    let count = Relative_path.Set.cardinal changed_files_to_process in
    let (_ : float) = log_startup_time "initialize2" start_time ~count in
    log_debug "initialize2.done";
    Lwt.return (Initialized istate)
  | Error reason ->
    log_debug "initialize2.error";
    let%lwt () =
      write_message
        ~out_fd
        ~message:
          (Client_ide_message.Notification
             (Client_ide_message.Done_init (Error reason)))
    in
    remove_hhi (During_init dstate);
    Lwt.return (Failed_init reason)

(** This funtion is about papering over a bug. Sometimes, rarely, we're
failing to receive DidOpen messages from clientLsp. Our model is to
only ever answer IDE requests on open files, so we know we'll eventually
reveive a DidClose even for them and be able to clear their TAST cache
at that time. But for now, to paper over the bug, we'll call this
function to log the event and we'll assume that we just missed a DidOpen. *)
let log_missing_open_file_BUG (reason : string) (path : Relative_path.t) : unit
    =
  let path = Relative_path.to_absolute path in
  let message =
    Printf.sprintf "Error: action on non-open file [%s] %s" reason path
  in
  Client_ide_utils.log_bug message ~telemetry:true

(** Registers the file in response to DidOpen or DidChange,
during the During_init state, by putting in a new
entry in open_files, with empty AST and TAST. If the LSP client
happened to send us two DidOpens for a file, or DidChange before DidOpen,
well, we won't complain. *)
let open_or_change_file_during_init
    (dstate : dstate) (path : Relative_path.t) (contents : string) : dstate =
  let entry =
    Provider_context.make_entry
      ~path
      ~contents:(Provider_context.Provided_contents contents)
  in
  let dopen_files =
    Relative_path.Map.add
      dstate.dopen_files
      ~key:path
      ~data:{ entry; diagnostics = ref None }
  in
  { dstate with dopen_files }

(** Closes a file, in response to DidClose event, by removing the
entry in open_files. If the LSP client sents us multile DidCloses,
or DidClose for an unopen file, we won't complain. *)
let close_file (open_files : open_files_state) (path : Relative_path.t) :
    open_files_state =
  if not (Relative_path.Map.mem open_files path) then
    log_missing_open_file_BUG "close-without-open" path;
  Relative_path.Map.remove open_files path

(** Updates an existing opened file, with new contents; if the
contents haven't changed then the existing open file's AST and TAST
will be left intact. *)
let update_file
    (open_files : open_files_state) (document : Client_ide_message.document) :
    open_files_state * Provider_context.entry * Diagnostics.t option ref =
  let path =
    document.Client_ide_message.file_path
    |> Path.to_string
    |> Relative_path.create_detect_prefix
  in
  let contents = document.Client_ide_message.file_contents in
  let (entry, prev_diagnostics) =
    match Relative_path.Map.find_opt open_files path with
    | None ->
      (* This is a common scenario although I'm not quite sure why *)
      ( Provider_context.make_entry
          ~path
          ~contents:(Provider_context.Provided_contents contents),
        ref None )
    | Some { entry; diagnostics }
      when Option.equal
             String.equal
             (Some contents)
             (Provider_context.get_file_contents_if_present entry) ->
      (* we can just re-use the existing entry; contents haven't changed *)
      (entry, diagnostics)
    | Some _ ->
      (* We'll create a new entry; existing entry caches, if present, will be dropped
         But first, need to clear the Fixme cache. This is a global cache
         which is updated as a side-effect of the Ast_provider. *)
      Fixme_provider.remove_batch (Relative_path.Set.singleton path);
      ( Provider_context.make_entry
          ~path
          ~contents:(Provider_context.Provided_contents contents),
        ref None )
  in
  let open_files =
    Relative_path.Map.add
      open_files
      ~key:path
      ~data:{ entry; diagnostics = prev_diagnostics }
  in
  (open_files, entry, prev_diagnostics)

(** like [update_file], but for convenience also produces a ctx for
use in typechecking. Also ensures that hhi files haven't been deleted
by tmp_cleaner, so that type-checking will succeed. *)
let update_file_ctx (istate : istate) (document : Client_ide_message.document) :
    istate
    * Provider_context.t
    * Provider_context.entry
    * Diagnostics.t option ref =
  let istate = restore_hhi_root_if_necessary istate in
  let (iopen_files, entry, published_errors) =
    update_file istate.iopen_files document
  in
  let ctx = make_singleton_ctx istate.icommon entry in
  ({ istate with iopen_files }, ctx, entry, published_errors)

(** We avoid showing typing errors if there are parsing errors. *)
let get_user_facing_errors
    ~(ctx : Provider_context.t) ~error_filter ~(entry : Provider_context.entry)
    : Diagnostics.t =
  let (_, ast_errors) =
    Ast_provider.compute_parser_return_and_ast_errors
      ~popt:(Provider_context.get_popt ctx)
      ~entry
  in
  if Diagnostics.has_no_errors ast_errors then
    let { Tast_provider.Compute_tast_and_errors.diagnostics = all_errors; _ } =
      Tast_provider.compute_tast_and_errors_quarantined
        ~ctx
        ~entry
        ~error_filter
    in
    all_errors
  else
    ast_errors

(** Computes the Diagnostics.t for what's on disk at a given path.
We provide [istate] just in case we can benefit from a cached answer. *)
let get_diagnostics_for_path (istate : istate) (path : Relative_path.t) :
    Diagnostics.t =
  let disk_content_opt =
    Sys_utils.cat_or_failed (Relative_path.to_absolute path)
  in
  let cached_entry_opt = Relative_path.Map.find_opt istate.iopen_files path in
  let entry_opt =
    match (disk_content_opt, cached_entry_opt) with
    | (None, _) ->
      (* if the disk file is absent (e.g. it was deleted prior to the user closing it),
         then we naturally can't compute errors for it. *)
      None
    | ( Some disk_content,
        Some
          {
            entry =
              {
                Provider_context.contents =
                  Provider_context.(
                    Contents_from_disk str | Provided_contents str);
                _;
              } as entry;
            _;
          } )
      when String.equal disk_content str ->
      (* file on disk was the same as what we currently have in the entry, and
         the entry very likely already has errors computed for it, so as an optimization
         we'll re-use errors from that entry. *)
      Some entry
    | (Some disk_content, _) ->
      (* file on disk is different from what we have in the entry, e.g. because the
         user closed a modified file, so compute errors from the disk content. *)
      Some
        (Provider_context.make_entry
           ~path
           ~contents:(Provider_context.Provided_contents disk_content))
  in
  match entry_opt with
  | None ->
    (* file couldn't be read off disk (maybe absent); therefore, by definition, no errors *)
    Diagnostics.empty
  | Some entry ->
    (* Here we'll get either cached errors from the cached entry, or will recompute errors
       from the partially cached entry, or will compute errors from the file on disk. *)
    let ctx = make_singleton_ctx istate.icommon entry in
    get_user_facing_errors ~ctx ~error_filter:istate.error_filter ~entry

let path_to_relative_path path =
  path |> Path.to_string |> Relative_path.create_detect_prefix

(** handle_request invariants: Messages are only ever handled serially; we never
handle one message while another is being handled. It is a bug if the client sends
anything other than [Initialize_from_saved_state] as its first message. Upon
receipt+processing of this we transition from [Pre_init] to [During_init]
and kick off some async work to prepare the naming table. During this async work, i.e.
during [During_init], we are able to handle a few requests but will reject
others. Important: files may change during [During_init], and it's important that we keep track of and eventually index these changed files.

Our caller [handle_one_message] is actually the one that transitions
us from [During_init] to either [Failed_init] or [Initialized]. Once in one
of those states, we never thereafter transition state. *)
let handle_request
    (type a)
    (message_queue : message_queue)
    (state : state)
    (_tracking_id : string)
    (message : a Client_ide_message.t) : state * (a, Lsp.Error.t) result =
  let open Client_ide_message in
  match (state, message) with
  (***********************************************************)
  (************************* HANDLED IN ANY STATE ************)
  (***********************************************************)
  | (_, Verbose_to_file verbose) ->
    if verbose then
      Hh_logger.Level.set_min_level_file Hh_logger.Level.Debug
    else
      Hh_logger.Level.set_min_level_file Hh_logger.Level.Info;
    (state, Ok ())
  | (_, Shutdown ()) ->
    Lwt_message_queue.close message_queue;
    remove_hhi state;
    (state, Ok ())
  (***********************************************************)
  (************************* INITIALIZATION ******************)
  (***********************************************************)
  | (Pending_init, Initialize_from_saved_state param) -> begin
    (* Invariant: no message will be sent to us prior to this request,
       and we must send no message until we've sent this response. *)
    try
      let dstate = initialize1 param in
      (* Establish the watcher clock before saved-state catch-up so changes
         during naming-table loading are covered by the subscription. *)
      if
        dstate.dcommon.local_config.Server_local_config.ide_file_watcher_enabled
      then
        Client_ide_eden_watcher.start
          ~root:param.Initialize_from_saved_state.root
          ~max_changed_files:
            dstate.dcommon.local_config
              .Server_local_config.ide_file_watcher_max_changed_files
          ~on_changes:(fun changes ->
            Lwt_message_queue.push
              message_queue
              (Commit_transition_changes changes));
      (* We're going to kick off the asynchronous part of initializing now.
         Once it's done, it will appear as a GotNamingTable message on the queue. *)
      Lwt.async (fun () ->
          let%lwt naming_table_result =
            Client_ide_init.init
              ~config:dstate.dcommon.config
              ~local_config:dstate.dcommon.local_config
              ~param
              ~hhi_root:dstate.dcommon.hhi_root
              ~local_memory:dstate.dcommon.local_memory
          in
          (* if the following push fails, that must be because the queues
             have been shut down, in which case there's nothing to do. *)
          let (_succeeded : bool) =
            Lwt_message_queue.push
              message_queue
              (GotNamingTable naming_table_result)
          in
          Lwt.return_unit);
      log "Finished saved state initialization. State: %s" (show_dstate dstate);
      (During_init dstate, Ok ())
    with
    | exn ->
      let e = Exception.wrap exn in
      let reason = Client_ide_utils.make_rich_error "initialize1" ~e in
      (* Our caller has an exception handler. But we must handle this ourselves
         to change state to Failed_init; our caller's handler doesn't change state. *)
      (* TODO: remove_hhi *)
      (Failed_init reason, Error (Client_ide_utils.to_lsp_error reason))
  end
  | (_, Initialize_from_saved_state _) ->
    failwith ("Unexpected init in " ^ state_to_log_string state)
  (***********************************************************)
  (************************* CAN HANDLE DURING INIT **********)
  (***********************************************************)
  | ((During_init _ | Initialized _), Did_change_watched_files changes) ->
    (process_changed_files ~trigger:Lsp_file_changes state changes, Ok ())
  | (During_init dstate, Did_close file_path) ->
    let path = path_to_relative_path file_path in
    ( During_init
        { dstate with dopen_files = close_file dstate.dopen_files path },
      Ok [] )
  | (Initialized istate, Did_close file_path) ->
    let path = path_to_relative_path file_path in
    let errors =
      Diagnostics.get_sorted_diagnostic_list
        (get_diagnostics_for_path istate path)
    in
    let error_hashes =
      List.map errors ~f:(fun err ->
          ( User_diagnostic.to_absolute err,
            User_diagnostic.hash_diagnostic_for_saved_state err ))
    in
    let diagnostics =
      List.map
        error_hashes
        ~f:
          Client_ide_message.diagnostic_of_finalized_error_without_related_hints
    in
    ( Initialized
        { istate with iopen_files = close_file istate.iopen_files path },
      Ok diagnostics )
  (* didOpen or didChange *)
  | (During_init dstate, Did_open_or_change { file_path; file_contents }) ->
    let path = path_to_relative_path file_path in
    let dstate = open_or_change_file_during_init dstate path file_contents in
    (During_init dstate, Ok ())
  | (Initialized istate, Did_open_or_change document) ->
    let (iopen_files, _entry, _errors) =
      update_file istate.iopen_files document
    in
    (Initialized { istate with iopen_files }, Ok ())
  (* Pull diagnostics *)
  | (Initialized istate, Diagnostics document) ->
    let (istate, ctx, entry, published_errors_ref) =
      update_file_ctx istate document
    in
    let errors =
      get_user_facing_errors ~ctx ~error_filter:istate.error_filter ~entry
    in
    published_errors_ref := Some errors;
    let errors = Diagnostics.get_sorted_diagnostic_list errors in
    let error_hashes =
      List.map errors ~f:(fun err ->
          ( User_diagnostic.to_absolute err,
            User_diagnostic.hash_diagnostic_for_saved_state err ))
    in
    let diagnostics = Ide_diagnostics.convert ~ctx ~entry error_hashes in
    (Initialized istate, Ok diagnostics)
  (* Document Symbol *)
  | (During_init dstate, Document_symbol document) ->
    let (dopen_files, entry, _) = update_file dstate.dopen_files document in
    let result =
      File_outline.outline_entry_no_comments
        ~popt:(Server_config.parser_options dstate.dcommon.config)
        ~entry
    in
    (During_init { dstate with dopen_files }, Ok result)
  | (Initialized istate, Document_symbol document) ->
    let (iopen_files, entry, _) = update_file istate.iopen_files document in
    let result =
      File_outline.outline_entry_no_comments
        ~popt:(Server_config.parser_options istate.icommon.config)
        ~entry
    in
    (Initialized { istate with iopen_files }, Ok result)
  (***********************************************************)
  (************************* UNABLE TO HANDLE ****************)
  (***********************************************************)
  | (During_init dstate, _) ->
    let e =
      {
        Lsp.Error.code = Lsp.Error.RequestCancelled;
        message = "IDE service has not yet completed init";
        data = None;
      }
    in
    (During_init dstate, Error e)
  | (Failed_init reason, _) ->
    (Failed_init reason, Error (Client_ide_utils.to_lsp_error reason))
  | (Pending_init, _) ->
    failwith
      (Printf.sprintf
         "unexpected message '%s' in state '%s'"
         (Client_ide_message.t_to_string message)
         (state_to_log_string state))
  (***********************************************************)
  (************************* NORMAL HANDLING AFTER INIT ******)
  (***********************************************************)
  | (Initialized istate, Hover (document, pos)) ->
    let (istate, ctx, entry, _) = update_file_ctx istate document in
    let result =
      Provider_utils.respect_but_quarantine_unsaved_changes ~ctx ~f:(fun () ->
          Ide_hover.go_quarantined ~ctx ~entry pos)
    in
    (Initialized istate, Ok result)
  | (Initialized istate, Go_to_implementation (document, pos, document_list)) ->
    let (istate, ctx, entry, _) = update_file_ctx istate document in
    let (istate, result) =
      Provider_utils.respect_but_quarantine_unsaved_changes ~ctx ~f:(fun () ->
          match Server_find_refs.go_from_file_ctx ~ctx ~entry pos with
          | Some (_name, action)
            when not @@ Server_go_to_impl.is_searchable ~action ->
            (istate, Client_ide_message.Invalid_symbol_impl)
          | Some (name, action) ->
            (*
                 1) For all open files that we know about in ClientIDEDaemon, uesd the
                 cached TASTs to return positions of implementations
                 2) Return this list, alongside the name and action
                 3) ClientLSP, upon receiving, will shellout to hh_server
                 and reject all server-provided positions for files that ClientIDEDaemon
                 knew about, under the assumption that our cached TAST provides edited
                 file info, if applicable.
            *)
            let (istate, single_file_positions) =
              List.fold
                ~f:(fun (istate, accum) document ->
                  let (istate, ctx, _entry, _errors) =
                    update_file_ctx istate document
                  in
                  let stringified_path =
                    Path.to_string document.Client_ide_message.file_path
                  in
                  let filename =
                    Relative_path.create_detect_prefix stringified_path
                  in
                  let single_file_pos =
                    Server_go_to_impl.go_for_single_file
                      ~ctx
                      ~action
                      ~naming_table:istate.naming_table
                      ~filename
                    |> Server_find_refs.to_absolute
                    |> List.map
                         ~f:(fun Search_types.Find_refs.{ name = _; pos } ->
                           pos)
                  in
                  let urikey =
                    Lsp_helpers.path_string_to_lsp_uri
                      stringified_path
                      ~default_path:stringified_path
                  in

                  let updated_map =
                    Lsp.UriMap.add urikey single_file_pos accum
                  in
                  (istate, updated_map))
                document_list
                ~init:(istate, Lsp.UriMap.empty)
            in
            ( istate,
              Client_ide_message.Go_to_impl_success
                (name, action, single_file_positions) )
          | None -> (istate, Client_ide_message.Invalid_symbol_impl))
    in
    (Initialized istate, Ok result)
    (* textDocument/rename *)
  | (Initialized istate, Rename (document, pos, new_name, document_list)) ->
    let (istate, ctx, entry, _errors) = update_file_ctx istate document in
    let (istate, result) =
      Provider_utils.respect_but_quarantine_unsaved_changes ~ctx ~f:(fun () ->
          match
            Server_find_refs.go_from_file_ctx_with_symbol_definition
              ~ctx
              ~entry
              pos
          with
          | None -> (istate, Client_ide_message.Not_renameable_position)
          | Some (_definition, action) when Server_find_refs.is_local action ->
            let res =
              match Server_rename.go_for_localvar ctx action new_name with
              | Ok (Some patch_list) ->
                Client_ide_message.Rename_success
                  { shellout = None; local = patch_list }
              | Ok None ->
                Client_ide_message.Rename_success
                  { shellout = None; local = [] }
              | Error action ->
                let str =
                  Printf.sprintf
                    "ClientIDEDaemon failed to rename for localvar %s"
                    (Server_command_types.Find_refs.show_action action)
                in
                log "%s" str;
                failwith "ClientIDEDaemon failed to rename for a localvar"
            in
            (istate, res)
          | Some (symbol_definition, action) ->
            let (istate, single_file_patches) =
              List.fold
                ~f:(fun (istate, accum) document ->
                  let (istate, ctx, _entry, _errors) =
                    update_file_ctx istate document
                  in
                  let filename =
                    Path.to_string document.Client_ide_message.file_path
                    |> Relative_path.create_detect_prefix
                  in
                  let single_file_patches =
                    Server_rename.go_for_single_file
                      ctx
                      ~find_refs_action:action
                      ~filename
                      ~symbol_definition
                      ~new_name
                  in
                  let patches =
                    match single_file_patches with
                    | Ok patches -> patches
                    | Error _ -> []
                  in
                  let patch_list = List.rev_append patches accum in
                  (istate, patch_list))
                ~init:(istate, [])
                document_list
            in
            ( istate,
              Client_ide_message.Rename_success
                {
                  shellout = Some (symbol_definition, action);
                  local = single_file_patches;
                } )
          (* not a localvar, must defer to hh_server *))
    in
    (Initialized istate, Ok result)
    (* textDocument/references - localvar only *)
  | (Initialized istate, Find_references (document, pos, document_list)) ->
    let open Result.Monad_infix in
    (* Update the state of the world with the document as it exists in the IDE *)
    let (istate, ctx, entry, _) = update_file_ctx istate document in
    let (istate, result) =
      Provider_utils.respect_but_quarantine_unsaved_changes ~ctx ~f:(fun () ->
          match Server_find_refs.go_from_file_ctx ~ctx ~entry pos with
          | Some (name, action) when Server_find_refs.is_local action ->
            let result =
              Server_find_refs.go_for_localvar ctx action
              >>| Server_find_refs.to_absolute
            in
            let result =
              match result with
              | Ok ide_result ->
                let lsp_uri_map =
                  begin
                    match ide_result with
                    | [] ->
                      (* If we find-refs on a localvar via right-click, is it possible that it doesn't return references?
                         It's possible some nondeterminism changed the cached TAST,
                         but assert that it's a failure for now
                      *)
                      let err =
                        Printf.sprintf
                          "FindRefs returned an empty list of positions for localvar %s"
                          name
                      in
                      log "%s" err;
                      Hack_event_logger.invariant_violation_bug err;
                      failwith err
                    | positions ->
                      let filename =
                        List.hd_exn positions
                        |> (fun Search_types.Find_refs.{ name = _; pos } -> pos)
                        |> Pos.filename
                      in
                      let uri =
                        Lsp_helpers.path_string_to_lsp_uri
                          ~default_path:filename
                          filename
                      in
                      Lsp.UriMap.add uri positions Lsp.UriMap.empty
                  end
                in
                let () =
                  if lsp_uri_map |> Lsp.UriMap.values |> List.length = 1 then
                    ()
                  else
                    (* Can a localvar cross file boundaries? I sure hope not. *)
                    let err =
                      Printf.sprintf
                        "Found more than one file when executing find refs for localvar %s"
                        name
                    in
                    log "%s" err;
                    Hack_event_logger.invariant_violation_bug err;
                    failwith err
                in
                Client_ide_message.Find_refs_success
                  {
                    full_name = name;
                    action = None;
                    hint_suffixes = [];
                    open_file_results = lsp_uri_map;
                  }
              | Error _action ->
                let err =
                  Printf.sprintf "Failed to find refs for localvar %s" name
                in
                log "%s" err;
                Hack_event_logger.invariant_violation_bug err;
                failwith err
            in
            (istate, result)
            (* clientLsp should raise if we return a LocalVar action *)
          | None ->
            (* Clicking a line+col that isn't a symbol *)
            (istate, Client_ide_message.Invalid_symbol)
          | Some (name, action) ->
            (* Not a localvar, so we do the following:
               1) For all open files that we know about in ClientIDEDaemon, uesd the
               cached TASTs to return positions of references
               2) Return this list, alongside the name and action
               3) ClientLSP, upon receiving, will shellout to hh_server
               and reject all server-provided positions for files that ClientIDEDaemon
               knew about, under the assumption that our cached TAST provides edited
               file info, if applicable.
            *)
            let (istate, single_file_refs) =
              List.fold
                ~f:(fun (istate, accum) document ->
                  let (istate, ctx, _entry, _errors) =
                    update_file_ctx istate document
                  in
                  let stringified_path =
                    Path.to_string document.Client_ide_message.file_path
                  in
                  let filename =
                    Relative_path.create_detect_prefix stringified_path
                  in
                  let single_file_ref =
                    Server_find_refs.go_for_single_file ~ctx ~action ~filename
                    |> Server_find_refs.to_absolute
                  in
                  let urikey =
                    Lsp_helpers.path_string_to_lsp_uri
                      stringified_path
                      ~default_path:stringified_path
                  in

                  let updated_map =
                    Lsp.UriMap.add urikey single_file_ref accum
                  in
                  (istate, updated_map))
                document_list
                ~init:(istate, Lsp.UriMap.empty)
            in
            let sienv_ref = ref istate.sienv in
            let hints =
              Symbol_index.find_refs ~sienv_ref ~action ~max_results:100
            in
            let hint_suffixes =
              Option.value_map hints ~default:[] ~f:(fun hints ->
                  List.filter_map hints ~f:(fun path ->
                      if Relative_path.is_root (Relative_path.prefix path) then
                        Some (Relative_path.suffix path)
                      else
                        None))
            in
            ( { istate with sienv = !sienv_ref },
              Client_ide_message.Find_refs_success
                {
                  full_name = name;
                  action = Some action;
                  hint_suffixes;
                  open_file_results = single_file_refs;
                } ))
    in
    (Initialized istate, Ok result)
  (* Autocomplete *)
  | ( Initialized istate,
      Completion (document, pos, { Client_ide_message.is_manually_invoked }) )
    ->
    (* Update the state of the world with the document as it exists in the IDE *)
    let (istate, ctx, entry, _) = update_file_ctx istate document in
    let sienv_ref = ref istate.sienv in
    let result =
      Server_auto_complete.go_ctx
        ~ctx
        ~entry
        ~sienv_ref
        ~is_manually_invoked
        pos
        ~naming_table:istate.naming_table
    in
    let istate = { istate with sienv = !sienv_ref } in
    (Initialized istate, Ok result)
  (* Autocomplete docblock resolve *)
  | ( Initialized istate,
      Completion_resolve Completion_resolve.{ fullname = symbol; kind } ) ->
    Hack_event_logger.completion_call ~method_name:"Completion_resolve";
    let ctx = make_empty_ctx istate.icommon in
    let result = Server_docblock_at.go_docblock_for_symbol ~ctx ~symbol ~kind in
    let signature = Server_auto_complete.get_signature ctx symbol in
    (Initialized istate, Ok Completion_resolve.{ docblock = result; signature })
  (* Autocomplete docblock resolve *)
  | ( Initialized istate,
      Completion_resolve_location (file_path, fullname, pos, kind) ) ->
    (* We're given a location but it often won't be an opened file.
       We will only serve autocomplete docblocks as of truth on disk.
       Hence, we construct temporary entry to reflect the file which
       contained the target of the resolve. *)
    Hack_event_logger.completion_call ~method_name:"Completion_resolve_location";
    let path = path_to_relative_path file_path in
    let ctx = make_empty_ctx istate.icommon in
    let (ctx, entry) = Provider_context.add_entry_if_missing ~ctx ~path in
    let result =
      Provider_utils.respect_but_quarantine_unsaved_changes ~ctx ~f:(fun () ->
          Server_docblock_at.go_docblock_ctx ~ctx ~entry pos ~kind)
    in
    let (Full_name s) = fullname in
    let signature = Server_auto_complete.get_signature ctx s in
    (Initialized istate, Ok Completion_resolve.{ docblock = result; signature })
  (* Document highlighting *)
  | (Initialized istate, Document_highlight (document, pos)) ->
    let (istate, ctx, entry, _) = update_file_ctx istate document in
    let results =
      Provider_utils.respect_but_quarantine_unsaved_changes ~ctx ~f:(fun () ->
          Ide_highlight_refs.go_quarantined ~ctx ~entry pos)
    in
    (Initialized istate, Ok results)
  (* Signature help *)
  | (Initialized istate, Signature_help (document, pos)) ->
    let (istate, ctx, entry, _) = update_file_ctx istate document in
    let results =
      Provider_utils.respect_but_quarantine_unsaved_changes ~ctx ~f:(fun () ->
          Server_signature_help.go_quarantined ~ctx ~entry pos)
    in
    (Initialized istate, Ok results)
  | (Initialized istate, Top_level_def_name_at_pos (document, pos)) ->
    let (istate, ctx, entry, _) = update_file_ctx istate document in
    let res = Ide_top_level_def_name_at_pos.go_quarantined ctx entry pos in
    (Initialized istate, Ok res)
  (* AutoClose *)
  | (Initialized istate, AutoClose (document, pos)) ->
    let (istate, ctx, entry, _) = update_file_ctx istate document in
    let close_tag = Autoclose_tags.go_xhp_close_tag ~ctx ~entry pos in
    (Initialized istate, Ok close_tag)
  (* Code actions (refactorings, quickfixes) *)
  | (Initialized istate, Code_action (document, range)) ->
    let (istate, ctx, entry, published_errors_ref) =
      update_file_ctx istate document
    in

    let results =
      Provider_utils.respect_but_quarantine_unsaved_changes ~ctx ~f:(fun () ->
          Code_actions_services.go
            ~ctx
            ~error_filter:istate.error_filter
            ~entry
            ~range)
    in

    (* We'll take this opportunity to make sure we've returned the latest errors.
       Why only return errors from didOpen,didChange,didClose,codeAction, and not also all
       the other methods like "hover" which might have recomputed TAST+errors? -- simplicity,
       mainly -- it's simpler to perform+handle this logic in just a few places rather than
       everywhere, and also because codeAction is called so frequently (e.g. upon changing
       tabs) that it's the best opportunity we have. *)
    let errors =
      get_user_facing_errors ~ctx ~error_filter:istate.error_filter ~entry
    in
    let errors_opt =
      match !published_errors_ref with
      | Some published_errors when phys_equal published_errors errors ->
        (* If the previous errors we returned are physically equal, that's an indication
           that the entry's TAST+errors hasn't been recomputed since last we returned errors
           back to clientLsp, so no need to do anything.
           And we actively WANT to do nothing in this case, since codeAction is called so frequently --
           e.g. every time the caret moves -- and we wouldn't want errors to be republished that
           frequently. *)
        None
      | Some _
      | None ->
        (* [Some _] -> This case indicates either that we'd previously returned errors back to clientLsp
           but the TAST+errors has changed since then, e.g. maybe the TAST+errors were invalidated
           due to a decl change, and some other action like hover recomputed the TAST+errors but
           didn't return them to clientLsp (because hover doesn't return errors), and so it's
           fallen to us to send them back. Note: only didOpen,didChange,didClose,codeAction
           ever return errors back to clientLsp. *)
        (* [None] -> This case indicates that we don't have a record of previous errors returned back to clientLsp.
           Might happen because a decl change invalidated TAST+errors and we are the first action since
           the decl change. Or because for some reason didOpen didn't arrive prior to codeAction. *)
        published_errors_ref := Some errors;
        Some (Diagnostics.sort_and_finalize errors)
    in
    (Initialized istate, Ok (results, errors_opt))
  (* Code action resolve (refactorings, quickfixes) *)
  | ( Initialized istate,
      Code_action_resolve { document; range; resolve_title; use_snippet_edits }
    ) ->
    let (istate, ctx, entry, _) = update_file_ctx istate document in

    let result =
      Provider_utils.respect_but_quarantine_unsaved_changes ~ctx ~f:(fun () ->
          Code_actions_services.resolve
            ~ctx
            ~error_filter:istate.error_filter
            ~entry
            ~range
            ~resolve_title
            ~use_snippet_edits)
    in
    (Initialized istate, Ok result)
  (* Go to definition *)
  | (Initialized istate, Definition (document, pos)) ->
    let (istate, ctx, entry, _) = update_file_ctx istate document in
    let result =
      Provider_utils.respect_but_quarantine_unsaved_changes ~ctx ~f:(fun () ->
          Server_go_to_definition.go_quarantined ~ctx ~entry pos)
    in
    (Initialized istate, Ok result)
  (* Type Definition *)
  | (Initialized istate, Type_definition (document, pos)) ->
    let (istate, ctx, entry, _) = update_file_ctx istate document in
    let result =
      Provider_utils.respect_but_quarantine_unsaved_changes ~ctx ~f:(fun () ->
          Server_type_definition.go_quarantined ~ctx ~entry pos)
    in
    (Initialized istate, Ok result)
  (* Workspace Symbol *)
  | (Initialized istate, Workspace_symbol query) ->
    (* Note: needs reverse-naming-table, hence only works in initialized
       state: for top-level queries it needs reverse-naming-table to look
       up positions; for member queries "Foo::bar" it needs it to fetch the
       decl for Foo. *)
    (* Note: we intentionally don't give results from unsaved files *)
    let ctx = make_empty_ctx istate.icommon in
    let sienv_ref = ref istate.sienv in
    let result = Ide_search.go ctx query ~kind_filter:"" sienv_ref in
    let istate = { istate with sienv = !sienv_ref } in
    (Initialized istate, Ok result)

(** Awaits until the next message is available, and handles it *)
let handle_one_message_exn
    ~(out_fd : Lwt_unix.file_descr)
    ~(message_queue : message_queue)
    ~(state : state)
    ~error_filter : state option Lwt.t =
  dbg_set_activity ~key:"handle" "popping";
  let%lwt message = Lwt_message_queue.pop message_queue in
  dbg_set_activity
    ~key:"handle"
    (match message with
    | None -> "none"
    | Some (Commit_transition_changes _) -> "commit_transition"
    | Some (GotNamingTable (Ok _)) -> "got_naming_table_ok"
    | Some (GotNamingTable (Error _)) -> "got_naming_table_err"
    | Some (ClientRequest message) ->
      Client_ide_message.tracked_t_to_string message);
  match (state, message) with
  | (_, None) ->
    Lwt.return_none (* exit loop if message_queue has been closed *)
  | (_, Some (Commit_transition_changes changes)) ->
    Lwt.return_some
      (process_changed_files ~trigger:Commit_transition state changes)
  | (During_init dstate, Some (GotNamingTable naming_table_result)) ->
    let%lwt state =
      initialize2 out_fd dstate naming_table_result ~error_filter
    in
    Lwt.return_some state
  | (_, Some (GotNamingTable _)) ->
    failwith ("Unexpected GotNamingTable in " ^ state_to_log_string state)
  | (_, Some (ClientRequest { Client_ide_message.tracking_id; message })) ->
    let unblocked_time = Unix.gettimeofday () in
    Hack_event_logger.serverless_ide_set_tracking_id tracking_id;
    (* Our caller has an exception handler which logs the exception.
       But we instead must fulfil our contract of responding to the client,
       even if we have an exception. Hence we need our own handler here. *)
    let (state, response) =
      try handle_request message_queue state tracking_id message with
      | Worker_cancel.Worker_should_exit as exn ->
        let e = Exception.wrap exn in
        (* When is this exception raised? several places during Typing_toplevel
           inner-loops call [WorkerCancel.raise_if_stop_requested]. So: it will
           be raised during [Tast_provider.compute_tast*] shortly after
           ClientLsp has called [WorkerCancel.stop_workers], which it does
           if it sees $/cancelRequest LSP notification on the incoming queue. *)
        let stack = Exception.get_backtrace_string e |> Exception.clean_stack in
        let lsp_error =
          {
            Lsp.Error.code = Lsp.Error.RequestCancelled;
            message = Exception.get_ctor_string e;
            data = Some (`Assoc [("stack", `String stack)]);
          }
        in
        (state, Error lsp_error)
      | exn ->
        let e = Exception.wrap exn in
        let reason = Client_ide_utils.make_rich_error "handle_request" ~e in
        (state, Error (Client_ide_utils.to_lsp_error reason))
    in
    dbg_set_activity ~key:"handle" "write_response";
    let%lwt () =
      write_message
        ~out_fd
        ~message:
          Client_ide_message.(
            Response { response; tracking_id; unblocked_time })
    in
    dbg_set_activity ~key:"handle" "written_response";
    Lwt.return_some state

let serve
    ~(in_fd : Lwt_unix.file_descr) ~(out_fd : Lwt_unix.file_descr) ~error_filter
    : unit Lwt.t =
  let rec flush_event_logger () : unit Lwt.t =
    dbg_set_activity ~key:"flush" "sleep";
    let%lwt () = Lwt_unix.sleep 0.5 in
    Hack_event_logger.Memory.profile_if_needed ();
    dbg_set_activity ~key:"flush" "flush";
    Lwt.async Event_logger_lwt.flush;
    dbg_set_activity ~key:"flush" "recheck";
    Event_logger.recheck_disk_files ();
    flush_event_logger ()
  in
  let rec pump_stdin (message_queue : message_queue) : unit Lwt.t =
    dbg_set_activity ~key:"pump" "loop";
    let%lwt (message, is_queue_open) =
      try%lwt
        dbg_set_activity ~key:"pump" "from_fd";
        let%lwt { Client_ide_message.tracking_id; message } =
          Marshal_tools_lwt.from_fd_with_preamble in_fd
        in
        dbg_set_activity ~key:"pump" "push";
        let is_queue_open =
          Lwt_message_queue.push
            message_queue
            (ClientRequest { Client_ide_message.tracking_id; message })
        in
        Lwt.return (message, is_queue_open)
      with
      | End_of_file ->
        (* There are two proper ways to close clientIdeDaemon: (1) by sending it a Shutdown message
           over the FD which is handled above, or (2) by closing the FD which is handled here. Neither
           path is considered anomalous and neither will raise an exception.
           Note that closing the message-queue is how we tell handle_messages loop to terminate. *)
        dbg_set_activity ~key:"pump" "eof";
        Lwt_message_queue.close message_queue;
        Lwt.return (Client_ide_message.Shutdown (), false)
      | exn ->
        let e = Exception.wrap exn in
        dbg_set_activity ~key:"pump" ("exn " ^ Exception.get_ctor_string e);
        Lwt_message_queue.close message_queue;
        Exception.reraise e
    in
    match message with
    | Client_ide_message.Shutdown () -> Lwt.return_unit
    | _ when not is_queue_open -> Lwt.return_unit
    | _ ->
      (* Care! The following call should be tail recursive otherwise we'll get huge callstacks.
         The [@tailcall] attribute would normally enforce this, but doesn't help the presence of lwt. *)
      (pump_stdin [@tailcall]) message_queue
  in
  let rec handle_messages ({ message_queue; state } : t) : unit Lwt.t =
    dbg_set_activity ~key:"handle" "loop";
    let%lwt next_state_opt =
      try%lwt
        let%lwt state =
          handle_one_message_exn ~out_fd ~message_queue ~state ~error_filter
        in
        dbg_set_activity ~key:"handle" "done";
        Lwt.return state
      with
      | exn ->
        let e = Exception.wrap exn in
        let is_write_error = is_outfd_write_error e in
        dbg_set_activity
          ~key:"handle"
          (Printf.sprintf
             "exn %s; is_write_error=%b"
             (Exception.get_ctor_string e)
             is_write_error);
        Client_ide_utils.log_bug "handle_one_message" ~e ~telemetry:true;
        if is_write_error then Stdlib.exit 1;
        (* if out_fd is down then there's no use continuing. *)
        dbg_set_activity ~key:"handle" "exn continue";
        Lwt.return_some state
    in
    match next_state_opt with
    | None -> Lwt.return_unit (* exit loop *)
    | Some state ->
      (* Care! The following call should be tail recursive otherwise we'll get huge callstacks.
         The [@tailcall] attribute would normally enforce this, but doesn't help the presence of lwt. *)
      (handle_messages [@tailcall]) { message_queue; state }
  in
  try%lwt
    dbg_set_activity ~key:"main" "serve";
    let message_queue = Lwt_message_queue.create () in
    let flusher_promise = flush_event_logger () in
    let%lwt () = handle_messages { message_queue; state = Pending_init }
    and () = pump_stdin message_queue in
    dbg_set_activity ~key:"main" "ending";
    Lwt.cancel flusher_promise;
    Lwt.return_unit
  with
  | exn ->
    let e = Exception.wrap exn in
    Client_ide_utils.log_bug "fatal clientIdeDaemon" ~e ~telemetry:true;
    dbg_set_activity ~key:"main" ("exception " ^ Exception.get_ctor_string e);
    Lwt.return_unit

let daemon_main
    ({
       Client_ide_message.init_id;
       verbose_to_file;
       verbose_to_stderr;
       shm_handle;
       error_filter;
       client_lsp_log_fn = _;
     } :
      Client_ide_message.daemon_args)
    (channels : ('a, 'b) Daemon.channel_pair) : unit =
  Startup_initializer.init ();
  Printexc.record_backtrace true;
  dbg_set_activity ~key:"main" "daemon_main";
  let (ic, oc) = channels in
  let in_fd = Lwt_unix.of_unix_file_descr (Daemon.descr_of_in_channel ic) in
  let out_fd = Lwt_unix.of_unix_file_descr (Daemon.descr_of_out_channel oc) in
  let daemon_init_id =
    Printf.sprintf "%s.%s" init_id (Random_id.short_string ())
  in
  Hack_event_logger.serverless_ide_init ~init_id:daemon_init_id;

  Typing_log.out_channel := stderr;
  (* where 'hh_show' goes *)
  if verbose_to_stderr then
    Hh_logger.Level.set_min_level_stderr Hh_logger.Level.Debug
  else
    Hh_logger.Level.set_min_level_stderr Hh_logger.Level.Error;
  if verbose_to_file then
    Hh_logger.Level.set_min_level_file Hh_logger.Level.Debug
  else
    Hh_logger.Level.set_min_level_file Hh_logger.Level.Info;

  (* in hh_shared.c, worker_id=0 is used for main process, and _id=1 for the first worker. *)
  Shared_mem.connect shm_handle ~worker_id:1;

  Stdlib.at_exit (fun () ->
      try
        let activities = dbg_dump_activity () in
        Hh_logger.log "SERVERLESS_IDE_EXIT\n%s" activities;
        Hack_event_logger.serverless_ide_exit activities
      with
      | _ -> ());
  try
    dbg_set_activity ~key:"main" "run_main";
    ( Lwt_utils.run_main @@ fun () ->
      (* MAIN ACTION HERE *)
      serve ~in_fd ~out_fd ~error_filter );
    dbg_set_activity ~key:"main" "done";
    Hh_logger.log "SERVERLESS_IDE_DONE(ok)";
    Hack_event_logger.serverless_ide_done None
  with
  | exn ->
    let e = Exception.wrap exn in
    dbg_set_activity ~key:"main" ("exn " ^ Exception.get_ctor_string e);
    Hh_logger.log
      "SERVERLESS_IDE_DONE(exn)\n%s"
      (Exception.to_string e |> Exception.clean_stack);
    Hack_event_logger.serverless_ide_done (Some e);
    (match exn with
    | Unix.Unix_error (Unix.EINVAL, "select", _) ->
      (* T297266104: the Lwt select engine fails once a watched fd number
         exceeds FD_SETSIZE. *)
      Client_ide_utils.log_bug "fatal Unix.select EINVAL" ~e ~telemetry:true;
      Stdlib.exit Exit_status.(exit_code IDE_select_error)
    | _ -> ())

let daemon_entry_point :
    (Client_ide_message.daemon_args, unit, unit) Daemon.entry =
  Daemon.register_entry_point "ClientIdeService" daemon_main

module Test = struct
  type env = istate

  let init ~custom_config ~naming_sqlite : env =
    let config =
      Option.value custom_config ~default:Server_config.default_config
    in
    let local_config = Server_local_config_load.default in
    let tcopt = Server_config.typechecker_options config in
    let popt = Server_config.parser_options config in
    Provider_backend.set_local_memory_backend_with_defaults_for_test ();
    let local_memory =
      match Provider_backend.get () with
      | Provider_backend.Local_memory local_memory -> local_memory
      | _ -> failwith "expected local memory backend"
    in
    let sienv =
      Symbol_index.initialize
        ~gleanopt:(Server_config.glean_options config)
        ~namespace_map:tcopt.Global_options.po.Parser_options.auto_namespace_map
        ~provider_name:
          local_config.Server_local_config.ide_symbolindex_search_provider
        ~quiet:local_config.Server_local_config.symbolindex_quiet
    in
    let ctx =
      Provider_context.empty_for_tool
        ~popt
        ~tcopt
        ~backend:(Provider_backend.Local_memory local_memory)
        ~deps_mode:(Typing_deps_mode.InMemoryMode None)
    in
    let naming_table =
      Naming_table.load_from_sqlite ctx (Path.to_string naming_sqlite)
    in
    {
      naming_table;
      error_filter = Tast_provider.ErrorFilter.default;
      sienv;
      icommon = { hhi_root = Path.make "/"; config; local_config; local_memory };
      iopen_files = Relative_path.Map.empty;
    }

  let index istate changes =
    Hh_logger.log
      "--> [index] %s"
      (Relative_path.Set.elements changes
      |> List.map ~f:Relative_path.suffix
      |> String.concat ~sep:" ");
    let (naming_table, sienv) =
      batch_update_naming_table_and_invalidate_caches
        ~trigger:Test_index
        ~ctx:(make_empty_ctx istate.icommon)
        ~naming_table:istate.naming_table
        ~sienv:istate.sienv
        ~local_memory:istate.icommon.local_memory
        ~open_files:istate.iopen_files
        changes
    in
    { istate with naming_table; sienv }

  let handle istate message =
    Hh_logger.log "--> %s" (Client_ide_message.t_to_string message);
    let message_queue = Lwt_message_queue.create () in
    match
      handle_request message_queue (Initialized istate) "tracking_id" message
    with
    | (Initialized istate, Ok response) -> (istate, response)
    | (_, Error { Lsp.Error.code; message; data }) ->
      let msg =
        Printf.sprintf
          "handle_request %s: %s %s"
          (Lsp.Error.show_code code)
          message
          (Option.value_map data ~default:"" ~f:Yojson.Safe.pretty_to_string)
      in
      failwith msg
    | (_, Ok _) ->
      let msg = Printf.sprintf "handle_request ended in bad state" in
      failwith msg
end
