(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude

(** Removes the names that were defined in the files from the reverse naming table *)
let remove_defs_from_reverse_naming_table
    naming_table (defs_per_file_parsed : _ Relative_path.Map.t) =
  Relative_path.Map.iter defs_per_file_parsed ~f:(fun fn _ ->
      match Naming_table.get_file_info naming_table fn with
      | None -> ()
      | Some
          {
            File_info.ids =
              { File_info.funs; classes; typedefs; consts; modules };
            file_mode = _;
            comments = _;
            position_free_decl_hash = _;
          } ->
        (* we use [snd] to strip away positions *)
        let snd id = id.File_info.name in
        Naming_global.remove_decls
          ~backend:(Provider_backend.get ())
          ~funs:(List.map funs ~f:snd)
          ~classes:(List.map classes ~f:snd)
          ~typedefs:(List.map typedefs ~f:snd)
          ~consts:(List.map consts ~f:snd)
          ~modules:(List.map modules ~f:snd))

(** Used only by [get_old_names_batch] to collect parsed files and existing
    owners of their new names before naming is updated.
    The returned files will be the keys of the map returned by [get_old_names_batch]. *)
let get_files_to_snapshot
    (ctx : Provider_context.t)
    ~(defs_per_file_parsed : File_info.t Relative_path.Map.t) :
    Relative_path.Set.t =
  let fun_canon_path name =
    Option.bind
      (Naming_provider.get_fun_canon_name ctx name)
      ~f:(Naming_provider.get_fun_path ctx)
  in
  let type_canon_path name =
    Option.bind
      (Naming_provider.get_type_canon_name ctx name)
      ~f:(Naming_provider.get_type_path ctx)
  in
  let add_owners paths kind ids =
    let get_path =
      match kind with
      | File_info.Fun -> fun_canon_path
      | File_info.Class
      | File_info.Typedef ->
        type_canon_path
      | File_info.Const -> Naming_provider.get_const_path ctx
      | File_info.Module -> Naming_provider.get_module_path ctx
    in
    let add_owner paths id =
      match get_path id.File_info.name with
      | None -> paths
      | Some path -> Relative_path.Set.add paths path
    in
    List.fold ids ~init:paths ~f:add_owner
  in
  let add_file path file_info paths =
    let { File_info.funs; classes; typedefs; consts; modules } =
      file_info.File_info.ids
    in
    let paths = Relative_path.Set.add paths path in
    let paths = add_owners paths File_info.Fun funs in
    let paths = add_owners paths File_info.Class classes in
    let paths = add_owners paths File_info.Typedef typedefs in
    let paths = add_owners paths File_info.Const consts in
    add_owners paths File_info.Module modules
  in
  Relative_path.Map.fold
    defs_per_file_parsed
    ~init:Relative_path.Set.empty
    ~f:add_file

(** Private to the [get_old_names_batch] stages, with one value per kind. *)
type 'a symbol_info = {
  si_funs: 'a;
  si_classes: 'a;
  si_typedefs: 'a;
  si_consts: 'a;
  si_modules: 'a;
}

let map_symbol_info
    (info : 'a symbol_info) ~(f : File_info.name_type -> 'a -> 'b) :
    'b symbol_info =
  {
    si_funs = f File_info.Fun info.si_funs;
    si_classes = f File_info.Class info.si_classes;
    si_typedefs = f File_info.Typedef info.si_typedefs;
    si_consts = f File_info.Const info.si_consts;
    si_modules = f File_info.Module info.si_modules;
  }

let symbol_info_of_ids (ids : File_info.ids) : File_info.id list symbol_info =
  {
    si_funs = ids.File_info.funs;
    si_classes = ids.File_info.classes;
    si_typedefs = ids.File_info.typedefs;
    si_consts = ids.File_info.consts;
    si_modules = ids.File_info.modules;
  }

let get_old_winner_file
    (ctx : Provider_context.t) (kind : File_info.name_type) (name : string) :
    Relative_path.t option =
  match kind with
  | File_info.Fun -> Naming_provider.get_fun_path ctx name
  | File_info.Class -> Naming_provider.get_class_path ctx name
  | File_info.Typedef -> Naming_provider.get_typedef_path ctx name
  | File_info.Const -> Naming_provider.get_const_path ctx name
  | File_info.Module -> Naming_provider.get_module_path ctx name

(** Used only by [get_old_names_batch] to record old symbol names and their
    previous owners. Ordinary file information is sufficient; hash fields are
    ignored. Missing owners retain their symbol keys with [None]. *)
let get_old_symbol_owners
    (ctx : Provider_context.t)
    (old_naming_table : Naming_table.t)
    (files : Relative_path.Set.t) :
    Relative_path.t option S_map.t symbol_info Relative_path.Map.t =
  let owners_of_ids kind ids =
    let add_first owners id =
      if S_map.mem id.File_info.name owners then
        owners
      else
        S_map.add
          id.File_info.name
          (get_old_winner_file ctx kind id.File_info.name)
          owners
    in
    List.fold ids ~init:S_map.empty ~f:add_first
  in
  let add_file path acc =
    let ids =
      match Naming_table.get_file_info old_naming_table path with
      | None -> File_info.empty_ids
      | Some file_info -> file_info.File_info.ids
    in
    let owners = map_symbol_info (symbol_info_of_ids ids) ~f:owners_of_ids in
    Relative_path.Map.add acc ~key:path ~data:owners
  in
  Relative_path.Set.fold files ~init:Relative_path.Map.empty ~f:add_file

(** Used only by [get_old_names_batch] to load decl hashes for snapshot files. *)
let load_old_file_decl_hashes
    (old_naming_table : Naming_table.t) (files : Relative_path.Set.t) :
    File_info.Decl_hash.t option S_map.t symbol_info Relative_path.Map.t =
  let hashes_of_ids _kind ids =
    let add_first hashes id =
      if S_map.mem id.File_info.name hashes then
        hashes
      else
        S_map.add id.File_info.name id.File_info.decl_hash hashes
    in
    List.fold ids ~init:S_map.empty ~f:add_first
  in
  let add_file path acc =
    match
      Naming_table.get_file_info_with_db_decl_hashes old_naming_table path
    with
    | None -> acc
    | Some file_info ->
      let hashes =
        map_symbol_info
          (symbol_info_of_ids file_info.File_info.ids)
          ~f:hashes_of_ids
      in
      Relative_path.Map.add acc ~key:path ~data:hashes
  in
  Relative_path.Set.fold files ~init:Relative_path.Map.empty ~f:add_file

(** Used only by [get_old_names_batch] to assemble [Old_names] from recorded
    owners and loaded hashes. *)
let resolve_old_names_for_file
    ~(file_decl_hashes_by_path :
       File_info.Decl_hash.t option S_map.t symbol_info Relative_path.Map.t)
    (old_owners : Relative_path.t option S_map.t symbol_info) :
    File_info.Old_names.t =
  let resolve_kind kind owners =
    let resolve_hash name owner =
      match owner with
      | None -> None
      | Some path ->
        (match Relative_path.Map.find_opt file_decl_hashes_by_path path with
        | None -> None
        | Some file_hashes ->
          let hashes =
            match kind with
            | File_info.Fun -> file_hashes.si_funs
            | File_info.Class -> file_hashes.si_classes
            | File_info.Typedef -> file_hashes.si_typedefs
            | File_info.Const -> file_hashes.si_consts
            | File_info.Module -> file_hashes.si_modules
          in
          Option.join (S_map.find_opt name hashes))
    in
    S_map.mapi resolve_hash owners
  in
  let resolved = map_symbol_info old_owners ~f:resolve_kind in
  {
    File_info.Old_names.n_funs = resolved.si_funs;
    n_classes = resolved.si_classes;
    n_types = resolved.si_typedefs;
    n_consts = resolved.si_consts;
    n_modules = resolved.si_modules;
  }

let get_old_names_batch
    (ctx : Provider_context.t)
    (old_naming_table : Naming_table.t)
    ~(defs_per_file_parsed : File_info.t Relative_path.Map.t) :
    File_info.Old_names.t Relative_path.Map.t =
  (* 1. Extend files (= keys in [defs_per_file_parsed]) with additional files
        that naming may add through a new conflict. *)
  let result_files : Relative_path.Set.t =
    get_files_to_snapshot ctx ~defs_per_file_parsed
  in

  (* 2. For each such file, look up *old* names therein and their naming winner
        files. The keys of this map are [result_files]. *)
  let owners_by_file :
      Relative_path.t option S_map.t symbol_info Relative_path.Map.t =
    get_old_symbol_owners ctx old_naming_table result_files
  in
  (* 3. Fetch decl hash information for [result_files], which include every old
        winner under the input prerequisite. The keys of this map are
        [result_files], excluding files for which we didn't find the file info. *)
  let file_decl_hashes_by_path :
      File_info.Decl_hash.t option S_map.t symbol_info Relative_path.Map.t =
    load_old_file_decl_hashes old_naming_table result_files
  in
  (* 4. Construct the resolved snapshot from winner decl hashes. *)
  let resolve_file (owners : Relative_path.t option S_map.t symbol_info) :
      File_info.Old_names.t =
    resolve_old_names_for_file ~file_decl_hashes_by_path owners
  in
  Relative_path.Map.map owners_by_file ~f:resolve_file

let get_old_and_new_defs_in_files
    (old_naming_table : Naming_table.t)
    ~(old_names_per_file : File_info.Old_names.t Relative_path.Map.t)
    (new_naming_table : Naming_table.t)
    (files : Relative_path.Set.t) :
    Decl_compare.VersionedNames.t Relative_path.Map.t =
  Relative_path.Set.fold
    files
    ~f:
      begin
        fun path acc ->
          let old_names =
            (* When hash population is disabled, old_names_per_file is empty;
               use the names-only fallback. *)
            match Relative_path.Map.find_opt old_names_per_file path with
            | Some old_names -> Some old_names
            | None ->
              Naming_table.get_file_info old_naming_table path
              |> Option.map ~f:File_info.simplify
              |> Option.map ~f:File_info.Old_names.of_names
          in
          let new_names =
            Naming_table.get_file_info new_naming_table path
            |> Option.map ~f:File_info.simplify
          in
          match (old_names, new_names) with
          | (None, None) -> acc
          | _ ->
            Relative_path.Map.add
              acc
              ~key:path
              ~data:
                {
                  Decl_compare.VersionedNames.old_names =
                    Option.value old_names ~default:File_info.Old_names.empty;
                  new_names =
                    Option.value new_names ~default:File_info.empty_names;
                }
      end
    ~init:Relative_path.Map.empty

(** If the only things that would change about file analysis are positions,
    we're not going to recheck it, and positions in its error list might
    become stale. Look if any of those positions refer to files that have
    actually changed and add them to files to recheck.

  @param reparsed   Set of files that were reparsed (so their ASTs and positions
                    in them could have changed.

  @param diagnostics Current global diagnostic list
*)
let add_files_with_stale_errors ctx ~reparsed diagnostics files_acc =
  Diagnostics.fold_errors
    diagnostics
    ~init:files_acc
    ~f:(fun source error acc ->
      if
        List.exists (User_diagnostic.to_list_ error) ~f:(fun e ->
            Relative_path.Set.mem
              reparsed
              (fst e |> Naming_provider.resolve_position ctx |> Pos.filename))
      then
        Relative_path.Set.add acc source
      else
        acc)

let resolve_files ctx ~reparsed env (fanout : Fanout.t) : Relative_path.Set.t =
  Server_progress.with_message "resolving files" @@ fun () ->
  let { Fanout.changed = _; to_recheck; to_recheck_if_errors } = fanout in
  let dependent_on_files =
    Relative_path.Set.fold
      reparsed
      ~init:(Typing_deps.DepSet.make ())
      ~f:(fun file depset ->
        let file_dep = Typing_deps.Dep.File file in
        let file_dependents =
          Typing_deps.get_ideps (Provider_context.get_deps_mode ctx) file_dep
        in

        Typing_deps.(DepSet.union depset file_dependents))
  in
  let to_recheck = Typing_deps.DepSet.union dependent_on_files to_recheck in
  let files_to_recheck = Naming_provider.get_files ctx to_recheck in
  let files_with_errors_to_recheck =
    let files_with_errors =
      Diagnostics.get_failed_files env.Server_env.diagnostics
    in
    let files_to_recheck_if_errors =
      Naming_provider.get_files ctx to_recheck_if_errors
    in
    Relative_path.Set.inter files_with_errors files_to_recheck_if_errors
  in
  let files =
    Relative_path.Set.union files_to_recheck files_with_errors_to_recheck
  in
  Hh_logger.log
    "%s:\n%s"
    (if Relative_path.Set.cardinal files <= 10 then
      "Files to recheck"
    else
      "First 10 files to recheck")
    (Relative_path.Set.elements files
    |> (fun l -> List.take l 10)
    |> List.map ~f:Relative_path.suffix
    |> String.concat ~sep:"\n");
  files

let get_files_to_recheck ctx env fanout ~reparsed ~diagnostics =
  resolve_files ~reparsed ctx env fanout
  (* We want to also recheck files that have typing errors referring to files that were
   * reparsed, since positions in those errors can be now stale. *)
  |> add_files_with_stale_errors ctx ~reparsed diagnostics
