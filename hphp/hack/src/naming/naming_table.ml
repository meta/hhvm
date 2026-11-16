(*
 * Copyright (c) 2015, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude

(**
    +---------------------------------+
    | Let's talk about naming tables! |
    +---------------------------------+

    Every typechecker or compiler that's more complicated than a toy project needs some way to look
    up the location of a given symbol, especially one that operates at the same scale as Hack and
    double especially as Hack moves towards generating data lazily on demand instead of generating
    all of the data up front during strictly defined typechecking phases. In Hack, the data
    structures that map from symbols to filenames and from filenames to symbol names are referred to
    as the "naming table," with the "forward naming table" mapping from filenames to symbol names
    and the "reverse naming table" mapping from symbol names to file names. We store the forward
    naming table as a standard OCaml map since we only read and write it from the main process, and
    we store the reverse naming table in shared memory since we want to allow rapid reads and writes
    from worker processes.

    So, to recap:
      - Forward naming table: OCaml map, maps filename -> symbols in file
      - Reverse naming table: shared memory, maps symbol name to filename it's defined in

    Seems simple enough, right? Unfortunately, this is where the real world and all of its attendant
    complexity comes in. The naming table (forward + reverse) is big. And not only is it big, its
    size is on the order of O(repo). This conflicts with our goal of making Hack use a flat amount
    of memory regardless of the amount of code we're actually typechecking, so it needs to go. In
    this case, we've chosen to use our existing saved state infrastructure and dump the naming
    table to a SQLite file which we can use to serve queries without having to store anything in
    memory other than just the differences between what's in the saved state and what's on disk.

    Right now, only the reverse naming table supports falling back to SQLite, so let's go through
    each operation that the reverse naming table supports and cover how it handles falling back to
    SQLite.


    +---------------------------------+
    | Reverse naming table operations |
    +---------------------------------+

    The reverse naming table supports three operations: [put], [get], and [delete]. [Put] is pretty
    simple, conceptually, but there's some trickiness with [delete] that would benefit from being
    covered in more detail, and that trickiness bleeds into how [get] works as well. One important
    consideration is that *the SQLite database is read-only.* The general idea of these operations
    is that shared memory stores only entries that have changed.

    PUT:
    [Put] is simple. Since the SQLite database is read-only and we use shared memory for storing any
    changes, we just put the value in shared memory.

                        +------------------+    +------------+
                        |                  |    |            |
                        |   Naming Heap    |    |   SQLite   |
                        |                  |    |            |
    [put key value] -----> [put key value] |    |            |
                        |                  |    |            |
                        +------------------+    +------------+

    DELETE:
    We're going to cover [delete] next because it's the only reason that [get] has any complexity
    beyond "look for the value in shared memory and if it's not there look for it in SQLite." At
    first glance, [delete] doesn't seem too difficult. Just delete the entry from shared memory,
    right? Well, that's fine... unless that key also exists in SQLite. If the value is in SQLite,
    then all that deleting it from shared memory does is make us start returning the stored SQLite
    value! This is bad, because if we just deleted a key of course we want [get] operations for it
    to return [None]. But okay, we can work around that. Instead of storing direct values in shared
    memory, we'll store an enum of [[Added of 'a | Deleted]]. Easy peasy!

    ...except what do we do if we want to delete a value in the main process, then add it again in a
    worker process? That would require changing a key's value in shared memory, which is something
    that we don't support due to data integrity concerns with multiple writers and readers. We could
    remove and re-add, except removal can only be done by master because of the same integrity
    concerns. So master would somehow need to know which entries will be added and prematurely
    remove the [Deleted] sentinel from them. This is difficult enough as to be effectively
    impossible.

    So what we're left needing is some way to delete values which can be undone by child processes,
    which means that undeleting a value needs to consist only of [add] operations to shared memory,
    and have no dependency on [remove]. Enter: the blocked entries heap.

    For each of the reverse naming table heaps (types, functions, and constants) we also create a
    heap that only stores values of a single-case enum: [Blocked]. The crucial difference between
    [Blocked] and [[Added of 'a | Deleted]] is that *we only check for [Blocked] if the value is
    not in the shared memory naming heap.* This means that we can effectively undelete an entry by
    using an only an [add] operation.  Exactly what we need! Thus, [delete] becomes:

                        +-----------------+    +---------------------+    +------------+
                        |                 |    |                     |    |            |
                        |   Naming Heap   |    |   Blocked Entries   |    |   SQLite   |
                        |                 |    |                     |    |            |
    [delete key] --+-----> [delete key]   |    |                     |    |            |
                   |    |                 |    |                     |    |            |
                   +----------------------------> [add key Blocked]  |    |            |
                        |                 |    |                     |    |            |
                        +-----------------+    +---------------------+    +------------+

    GET:
    Wow, what a ride, huh? Now that we know how to add and remove entries, let's make this actually
    useful and talk about how to read them! The general idea is that we first check to see if the
    value is in the shared memory naming heap. If so, we can return that immediately. If it's not
    in the naming heap, then we check to see if it's blocked. If it is, we immediately return
    [None]. If it's not in the naming heap, and it's not in the blocked entries, then and only then
    do we read the value from SQLite:

                        +-----------------+    +---------------------+    +------------------+
                        |                 |    |                     |    |                  |
                        |   Naming Heap   |    |   Blocked Entries   |    |      SQLite      |
                        |                 |    |                     |    |                  |
    [get key] -----------> [get key] is:  |    |                     |    |                  |
    [Some value] <---------- [Some value] |    |                     |    |                  |
                        |    [None] -------+   |                     |    |                  |
                        |                 | \  |                     |    |                  |
                        |                 |  +--> [get key] is:      |    |                  |
    [None] <--------------------------------------- [Some Blocked]   |    |                  |
                        |                 |    |    [None] -----------+   |                  |
                        |                 |    |                     | \  |                  |
                        |                 |    |                     |  +--> [get key] is:   |
    [Some value] <------------------------------------------------------------ [Some value]  |
    [None] <------------------------------------------------------------------ [None]        |
                        |                 |    |                     |    |                  |
                        +-----------------+    +---------------------+    +------------------+

    And we're done! I hope this was easy to understand and as fun to read as it was to write :)

    MINUTIAE:
    Q: When do we delete entries from the Blocked Entries heaps?
    A: Never! Once an entry has been removed at least once we never want to retrieve the SQLite
       version for the rest of the program execution.

    Q: Won't we just end up with a heap full of [Blocked] entries now?
    A: Not really. We only have to do deletions once to remove entries for dirty files, and after
       that it's not a concern. Plus [Blocked] entries are incredibly small in shared memory
       (although they do still occupy a hash slot).
*)

(* Changes since baseline can be None if there was no baseline to begin with.
   The scenario where we apply changes since baseline instead of relying on
   the packaged local changes in the LOCAL_CHANGES table is this:
     1) Load the naming table baseline (may include local changes if it was
       incrementally updated at some point - not currently done in practice,
       but possible and likely to happen in the future)
     2) Load the changes since baseline that include naming changes processed
       at another time (perhaps, on another host)
   In the scenario where the naming table is saved to SQLite from an Unbacked
   naming table, there is no baseline to speak of
*)
type changes_since_baseline = Naming_sqlite.local_changes option

type t =
  | Unbacked of File_info.t Relative_path.Map.t
  | Backed of Naming_sqlite.local_changes * Naming_sqlite.db_path
[@@deriving show]

type defs_per_file = File_info.names Relative_path.Map.t [@@deriving show]

type saved_state_info = File_info.saved Relative_path.Map.t

(*****************************************************************************)
(* Forward naming table functions *)
(*****************************************************************************)

let empty = Unbacked Relative_path.Map.empty

let filter a ~f =
  match a with
  | Unbacked a -> Unbacked (Relative_path.Map.filter a ~f)
  | Backed (local_changes, db_path) ->
    let file_deltas = local_changes.Naming_sqlite.file_deltas in
    Backed
      ( {
          local_changes with
          Naming_sqlite.file_deltas =
            Naming_sqlite.fold
              ~warn_on_naming_costly_iter:true
              ~db_path
              ~init:file_deltas
              ~f:
                begin
                  fun path fi acc ->
                    if f path fi then
                      acc
                    else
                      Relative_path.Map.add
                        acc
                        ~key:path
                        ~data:Naming_sqlite.Deleted
                end
              ~file_deltas;
        },
        db_path )

let fold ?(warn_on_naming_costly_iter = true) a ~init ~f =
  match a with
  | Unbacked a -> Relative_path.Map.fold a ~init ~f
  | Backed (local_changes, db_path) ->
    Naming_sqlite.fold
      ~warn_on_naming_costly_iter
      ~db_path
      ~init
      ~f
      ~file_deltas:local_changes.Naming_sqlite.file_deltas

let get_files a =
  match a with
  | Unbacked a -> Relative_path.Map.keys a
  | Backed (local_changes, db_path) ->
    (* Reverse at the end to preserve ascending sort order. *)
    Naming_sqlite.fold
      ~warn_on_naming_costly_iter:true
      ~db_path
      ~init:[]
      ~f:(fun path _ acc -> path :: acc)
      ~file_deltas:local_changes.Naming_sqlite.file_deltas
    |> List.rev

let get_files_changed_since_baseline
    (changes_since_baseline : changes_since_baseline) : Relative_path.t list =
  match changes_since_baseline with
  | Some changes_since_baseline ->
    Relative_path.Map.keys changes_since_baseline.Naming_sqlite.file_deltas
  | None -> []

let get_file_info a key =
  match a with
  | Unbacked a -> Relative_path.Map.find_opt a key
  | Backed (local_changes, db_path) ->
    (match
       Relative_path.Map.find_opt local_changes.Naming_sqlite.file_deltas key
     with
    | Some (Naming_sqlite.Modified fi) -> Some fi
    | Some Naming_sqlite.Deleted -> None
    | None -> Naming_sqlite.get_file_info db_path key)

exception File_info_not_found

let get_file_info_with_db_decl_hashes
    (naming_table : t) (path : Relative_path.t) : File_info.t option =
  match naming_table with
  | Unbacked files -> Relative_path.Map.find_opt files path
  | Backed (local_changes, db_path) ->
    (match
       Relative_path.Map.find_opt local_changes.Naming_sqlite.file_deltas path
     with
    | Some (Naming_sqlite.Modified file_info) -> Some file_info
    | Some Naming_sqlite.Deleted -> None
    | None -> Naming_sqlite.get_file_info_with_decl_hashes db_path path)

let get_file_info_exn a key =
  match get_file_info a key with
  | Some info -> info
  | None -> raise File_info_not_found

let get_64bit_dep_set_files (naming_table : t) (deps : Typing_deps.DepSet.t) :
    Relative_path.Set.t =
  match naming_table with
  | Unbacked _ ->
    failwith
      "get_64bit_dep_set_files not supported for unbacked naming tables. Use Typing_deps.get_ifiles instead."
  | Backed (local_changes, db_path) ->
    let base_results =
      Typing_deps.DepSet.fold
        deps
        ~init:Relative_path.Set.empty
        ~f:(fun dep acc ->
          match Naming_sqlite.get_path_by_64bit_dep db_path dep with
          | None -> acc
          | Some (file, _namekind) -> Relative_path.Set.add acc file)
    in

    Relative_path.Map.fold
      local_changes.Naming_sqlite.file_deltas
      ~init:base_results
      ~f:(fun path file_info acc ->
        match file_info with
        | Naming_sqlite.Deleted -> Relative_path.Set.remove acc path
        | Naming_sqlite.Modified file_info ->
          let file_deps =
            Typing_deps.deps_of_file_info file_info
            |> Typing_deps.DepSet.of_list
          in
          if
            not
              (Typing_deps.DepSet.is_empty
                 (Typing_deps.DepSet.inter deps file_deps))
          then
            Relative_path.Set.add acc path
          else
            (* If this file happened to be present in the base changes, it
               would be permissible to include it since we can return an
               overestimate, but we may as well remove it. *)
            Relative_path.Set.remove acc path)

let has_file a key =
  match get_file_info a key with
  | Some _ -> true
  | None -> false

let iter a ~f =
  match a with
  | Unbacked a -> Relative_path.Map.iter a ~f
  | Backed (local_changes, db_path) ->
    Naming_sqlite.fold
      ~warn_on_naming_costly_iter:true
      ~db_path
      ~init:()
      ~f:(fun path fi () -> f path fi)
      ~file_deltas:local_changes.Naming_sqlite.file_deltas

let remove a key =
  match a with
  | Unbacked a -> Unbacked (Relative_path.Map.remove a key)
  | Backed (local_changes, db_path) ->
    Backed
      ( {
          local_changes with
          Naming_sqlite.file_deltas =
            Relative_path.Map.add
              local_changes.Naming_sqlite.file_deltas
              ~key
              ~data:Naming_sqlite.Deleted;
        },
        db_path )

let update a key data =
  match a with
  | Unbacked a -> Unbacked (Relative_path.Map.add a ~key ~data)
  | Backed (local_changes, db_path) ->
    Backed
      ( {
          local_changes with
          Naming_sqlite.file_deltas =
            Relative_path.Map.add
              local_changes.Naming_sqlite.file_deltas
              ~key
              ~data:(Naming_sqlite.Modified data);
        },
        db_path )

let update_many a updates =
  match a with
  | Unbacked a ->
    (* Reverse the order because union always takes the first value. *)
    Unbacked (Relative_path.Map.union updates a)
  | Backed (local_changes, db_path) ->
    let local_updates =
      Relative_path.Map.map updates ~f:(fun data -> Naming_sqlite.Modified data)
    in
    Backed
      ( {
          local_changes with
          Naming_sqlite.file_deltas =
            Relative_path.Map.union
              local_updates
              local_changes.Naming_sqlite.file_deltas;
        },
        db_path )

let update_from_deltas a file_deltas =
  match a with
  | Unbacked file_infos ->
    let file_infos =
      Relative_path.Map.fold
        file_deltas
        ~init:file_infos
        ~f:(fun path file_delta acc ->
          match file_delta with
          | Naming_sqlite.Modified file_info ->
            Relative_path.Map.add acc ~key:path ~data:file_info
          | Naming_sqlite.Deleted -> Relative_path.Map.remove acc path)
    in
    Unbacked file_infos
  | Backed (local_changes, db_path) ->
    let file_deltas =
      (* Reverse the order because union always takes the first value. *)
      Relative_path.Map.union
        file_deltas
        local_changes.Naming_sqlite.file_deltas
    in
    Backed ({ local_changes with Naming_sqlite.file_deltas }, db_path)

let combine a b =
  match b with
  | Unbacked b -> update_many a b
  | _ ->
    failwith
      "SQLite-backed naming tables cannot be the second argument to combine."

(**
  This function saves the local changes structure as a binary blob.
  Local changes represents the (forward) naming table changes since
  the SQLite naming table baseline. Therefore, this function is most meaningful
  when the naming table is backed by SQLite. If the naming table is NOT backed
  by SQLite, meaning that it was computed from scratch by parsing the whole
  repo, then this baseline-to-current difference is an empty set.
  The resulting snapshot can be applied when loading a SQLite naming table.
 *)
let save_changes_since_baseline naming_table ~destination_path =
  let snapshot =
    match naming_table with
    | Unbacked _ -> None
    | Backed (local_changes, _db_path) -> Some local_changes
  in
  let contents = Marshal.to_string snapshot [Marshal.No_sharing] in
  Disk.write_file ~file:destination_path ~contents

let save naming_table db_name =
  match naming_table with
  | Unbacked naming_table ->
    let t = Unix.gettimeofday () in
    (* Ideally, we would have a commit hash, which would result in the same
       content version given the same version of source files. However,
       using a unique version every time we save the naming table from scratch
       is good enough to enable us to check that the local changes diff
       provided as an argument to load_from_sqlite_with_changes_since_baseline
       is compatible with the underlying SQLite table. *)
    let base_content_version =
      Printf.sprintf "%d-%s" (int_of_float t) (Random_id.short_string ())
    in
    let save_result =
      Naming_sqlite.save_file_infos db_name naming_table ~base_content_version
    in
    Naming_sqlite.free_db_cache ();
    let (_ : float) =
      let open Naming_sqlite in
      Hh_logger.log_duration
        (Printf.sprintf
           "Inserted %d symbols from %d files"
           save_result.symbols_added
           save_result.files_added)
        t
    in
    save_result
  | Backed (local_changes, db_path) ->
    let t = Unix.gettimeofday () in
    let save_result =
      Naming_sqlite.copy_and_update
        ~existing_db:db_path
        ~new_db:(Naming_sqlite.Db_path db_name)
        local_changes
    in
    Naming_sqlite.free_db_cache ();
    let (_ : float) =
      let open Naming_sqlite in
      Hh_logger.log_duration
        (Printf.sprintf
           "Inserted %d symbols from %d files"
           save_result.symbols_added
           save_result.files_added)
        t
    in
    save_result

(*****************************************************************************)
(* Conversion functions *)
(*****************************************************************************)

let from_saved saved =
  Hh_logger.log "Loading naming table from marshalled blob...";
  let t = Unix.gettimeofday () in
  let naming_table =
    Unbacked
      (Relative_path.Map.fold
         saved
         ~init:Relative_path.Map.empty
         ~f:(fun fn saved acc ->
           let file_info = File_info.from_saved fn saved in
           Relative_path.Map.add acc ~key:fn ~data:file_info))
  in
  let _t = Hh_logger.log_duration "Loaded naming table from blob" t in
  naming_table

let to_saved a =
  match a with
  | Unbacked a -> Relative_path.Map.map a ~f:File_info.to_saved
  | Backed _ ->
    fold a ~init:Relative_path.Map.empty ~f:(fun path fi acc ->
        Relative_path.Map.add acc ~key:path ~data:(File_info.to_saved fi))

let to_defs_per_file ?(warn_on_naming_costly_iter = true) a =
  match a with
  | Unbacked a -> Relative_path.Map.map a ~f:File_info.simplify
  | Backed _ ->
    fold
      a
      ~warn_on_naming_costly_iter
      ~init:Relative_path.Map.empty
      ~f:(fun path fi acc ->
        Relative_path.Map.add acc ~key:path ~data:(File_info.simplify fi))

let saved_to_defs_per_file saved =
  Relative_path.Map.map saved ~f:File_info.saved_to_names

(*****************************************************************************)
(* Forward naming table creation functions *)
(*****************************************************************************)

let create a = Unbacked a

(* Helper function to apply new files info to reverse naming table *)
let update_reverse_entries_helper
    (ctx : Provider_context.t)
    (changed_ids : (Relative_path.t * File_info.t option) list) : unit =
  let backend = Provider_context.get_backend ctx in
  let db_path_opt = Db_path_provider.get_naming_db_path backend in
  (* Remove all old file symbols first *)
  List.iter
    ~f:(fun (path, _file_info) ->
      let fi_opt =
        Option.bind db_path_opt ~f:(fun db_path ->
            Naming_sqlite.get_file_info db_path path)
      in
      match fi_opt with
      | Some fi ->
        let { File_info.classes; typedefs; funs; consts; modules } =
          fi.File_info.ids
        in
        Naming_provider.remove_type_batch
          backend
          (classes |> List.map ~f:(fun id -> id.File_info.name));
        Naming_provider.remove_type_batch
          backend
          (typedefs |> List.map ~f:(fun id -> id.File_info.name));
        Naming_provider.remove_fun_batch
          backend
          (funs |> List.map ~f:(fun id -> id.File_info.name));
        Naming_provider.remove_const_batch
          backend
          (consts |> List.map ~f:(fun id -> id.File_info.name));
        Naming_provider.remove_module_batch
          backend
          (modules |> List.map ~f:(fun id -> id.File_info.name))
      | None -> ())
    changed_ids;

  (* Add new file symbols after removing old files symbols *)
  List.iter
    ~f:(fun (_path, new_file_info) ->
      match new_file_info with
      | Some fi ->
        let { File_info.classes; typedefs; funs; consts; modules } =
          fi.File_info.ids
        in
        List.iter
          ~f:(fun id ->
            Naming_provider.add_class backend id.File_info.name id.File_info.pos)
          classes;
        List.iter
          ~f:(fun id ->
            Naming_provider.add_typedef
              backend
              id.File_info.name
              id.File_info.pos)
          typedefs;
        List.iter
          ~f:(fun id ->
            Naming_provider.add_fun backend id.File_info.name id.File_info.pos)
          funs;
        List.iter
          ~f:(fun id ->
            Naming_provider.add_const backend id.File_info.name id.File_info.pos)
          consts;
        List.iter
          ~f:(fun id ->
            Naming_provider.add_module
              backend
              id.File_info.name
              id.File_info.pos)
          modules
      | None -> ())
    changed_ids

let update_reverse_entries ctx file_deltas =
  let file_delta_list = Relative_path.Map.bindings file_deltas in
  let changed_files_info =
    List.map
      ~f:(fun (path, file_delta) ->
        match file_delta with
        | Naming_sqlite.Modified fi -> (path, Some fi)
        | Naming_sqlite.Deleted -> (path, None))
      file_delta_list
  in
  update_reverse_entries_helper ctx changed_files_info

let choose_local_changes ~local_changes ~changes_since_baseline =
  match changes_since_baseline with
  | None -> local_changes
  | Some changes_since_baseline ->
    if
      String.equal
        changes_since_baseline.Naming_sqlite.base_content_version
        local_changes.Naming_sqlite.base_content_version
    then
      changes_since_baseline
    else
      failwith
        (Printf.sprintf
           "%s\nSQLite content version: %s\nLocal changes content version: %s"
           "Incompatible local changes diff supplied."
           local_changes.Naming_sqlite.base_content_version
           changes_since_baseline.Naming_sqlite.base_content_version)

(**
  Loads the naming table from a SQLite database. The optional custom local
  changes represents the naming table changes that occurred since the original
  SQLite database was created.
  To recap, the SQLite-based naming table, once loaded, is not mutated. All the
  forward naming table changes are kept in an in-memory map. When we save an
  update to a naming table that is backed by SQLite, we store the changes
  since the baseline as a blob is a separate table and we rehydrate those
  changes into an in-memory map when we load the updated naming table later.
  The custom local changes is an alternative to the changes serialized as a
  blob into the SQLite table. This enables scenarios that require repeatedly
  loading the same base SQLite naming table with different versions of
  the local changes.
 *)
let load_from_sqlite_for_type_checking
    ~(changes_since_baseline : Naming_sqlite.local_changes option)
    (ctx : Provider_context.t)
    (db_path : string) : t =
  Hh_logger.log "Loading naming table from SQLite...";
  let t = Unix.gettimeofday () in
  let db_path = Naming_sqlite.Db_path db_path in
  Naming_sqlite.validate_can_open_db db_path;
  (* throw in master if anything's wrong *)
  Db_path_provider.set_naming_db_path
    (Provider_context.get_backend ctx)
    (Some db_path);
  (* I believe that changes_since_baseline is never used. I want to check... *)
  if Option.is_some changes_since_baseline then
    Hack_event_logger.naming_sqlite_has_changes_since_baseline ();
  let local_changes =
    choose_local_changes
      ~local_changes:(Naming_sqlite.get_local_changes db_path)
      ~changes_since_baseline
  in
  let t = Hh_logger.log_duration "Loaded local naming table changes" t in
  update_reverse_entries ctx local_changes.Naming_sqlite.file_deltas;
  let _t = Hh_logger.log_duration "Updated reverse naming table entries" t in
  Backed (local_changes, db_path)

let load_from_sqlite_with_changed_file_infos
    (ctx : Provider_context.t)
    (changed_file_infos : (Relative_path.t * File_info.t option) list)
    (db_path : string) : t =
  Hh_logger.log "Loading naming table from SQLite...";
  let db_path = Naming_sqlite.Db_path db_path in
  Naming_sqlite.validate_can_open_db db_path;
  (* throw in master if anything's wrong *)
  Db_path_provider.set_naming_db_path
    (Provider_context.get_backend ctx)
    (Some db_path);
  let t = Unix.gettimeofday () in
  (* Get changed files delta from file info *)
  let changed_file_deltas =
    List.fold_left
      ~f:(fun acc_files_delta (path, changed_file_info_opt) ->
        let file_delta =
          match changed_file_info_opt with
          | Some file_info -> Naming_sqlite.Modified file_info
          | None -> Naming_sqlite.Deleted
        in
        Relative_path.Map.add acc_files_delta ~key:path ~data:file_delta)
      ~init:Relative_path.Map.empty
      changed_file_infos
  in
  let base_local_changes = Naming_sqlite.get_local_changes db_path in
  let local_changes =
    Naming_sqlite.
      {
        file_deltas = changed_file_deltas;
        base_content_version =
          base_local_changes.Naming_sqlite.base_content_version;
      }
  in
  let t = Hh_logger.log_duration "Calculate changed files delta" t in
  update_reverse_entries_helper ctx changed_file_infos;
  let _t = Hh_logger.log_duration "Updated reverse naming table entries" t in
  Backed (local_changes, db_path)

let load_from_sqlite_with_changes_since_baseline
    (ctx : Provider_context.t)
    (changes_since_baseline : Naming_sqlite.local_changes option)
    (db_path : string) : t =
  load_from_sqlite_for_type_checking ~changes_since_baseline ctx db_path

let load_from_sqlite (ctx : Provider_context.t) (db_path : string) : t =
  load_from_sqlite_for_type_checking ~changes_since_baseline:None ctx db_path

let get_forward_naming_fallback_path a : string option =
  match a with
  | Unbacked _ -> None
  | Backed (_, Naming_sqlite.Db_path db_path) -> Some db_path

module SaveAsync = struct
  (* The input record that gets passed from the main process to the daemon process *)
  type input = {
    blob_path: string;
    destination_path: string;
    init_id: string;
    root: string;
  }

  (* The main entry point of the daemon process that saves the naming table
      from a blob to the SQLite format. *)
  let save { blob_path; destination_path; root; init_id } : unit =
    Hack_event_logger.init_batch_tool
      ~init_id
      ~root:(Path.make root)
      ~time:(Unix.gettimeofday ());
    let chan = In_channel.create ~binary:true blob_path in
    let (naming_table : t) = Marshal.from_channel chan in
    Sys_utils.close_in_no_fail blob_path chan;
    Sys_utils.rm_dir_tree (Filename.dirname blob_path);
    let (_save_result : Naming_sqlite.save_result) =
      save naming_table destination_path
    in
    (* The intention here is to force the daemon process to exit with
        an failed exit code so that the main process can detect
        the condition and log the outcome. *)
    assert (Disk.file_exists destination_path)

  (* The daemon entry registration - used by the main process *)
  let save_entry =
    Process.register_entry_point "naming_table_save_async_entry" save
end

let save_async naming_table ~init_id ~root ~destination_path =
  Hh_logger.log "Saving naming table to %s" destination_path;
  let blob_dir =
    Tempfile.mkdtemp_with_dir
      ~skip_mocking:false
      (Path.make Global_config.tmp_dir)
  in
  let blob_path = Path.(to_string (concat blob_dir "naming_bin")) in
  let chan = Stdlib.open_out_bin blob_path in
  Marshal.to_channel chan naming_table [];
  Stdlib.close_out chan;

  let open SaveAsync in
  Future_process.make
    (Process.run_entry
       Process_types.Default
       save_entry
       { blob_path; destination_path; init_id; root })
    (fun output -> Hh_logger.log "Output from out-of-proc: %s" output)

(*****************************************************************************)
(* Testing functions *)
(*****************************************************************************)

let get_backed_delta_TEST_ONLY (t : t) : Naming_sqlite.local_changes option =
  match t with
  | Backed (local_changes, _) -> Some local_changes
  | Unbacked _ -> None
