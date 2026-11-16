(*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude
open Shallow_decl_defs

let fetch_remote_old_decl_flag (ctx : Provider_context.t) =
  Typechecker_options.fetch_remote_old_decls (Provider_context.get_tcopt ctx)

let only_fetch_remote_old_decl_during_init (ctx : Provider_context.t) =
  Typechecker_options.only_fetch_remote_old_decl_during_init
    (Provider_context.get_tcopt ctx)

let fetch_remote_old_decls ctx ~during_init =
  fetch_remote_old_decl_flag ctx
  && ((not (only_fetch_remote_old_decl_during_init ctx)) || during_init)

let fetch_missing_old_classes_remotely ctx ~during_init old_classes =
  if fetch_remote_old_decls ctx ~during_init then
    let missing_old_classes =
      S_map.filter (fun _key -> Option.is_none) old_classes
      |> S_map.keys
      |> List.map ~f:(fun name -> Typing_deps.Dep.Type name)
    in
    let add_remote_class acc decl =
      match decl with
      | NClass (name, cls) -> S_map.add name (Some cls) acc
      | NFun _
      | NTypedef _
      | NConst _
      | NModule _ ->
        acc
    in
    let remote_old_classes =
      List.fold
        (Remote_old_decl_client.fetch_old_decls ~ctx missing_old_classes)
        ~init:S_map.empty
        ~f:add_remote_class
    in
    S_map.union old_classes remote_old_classes ~combine:(fun _key decl1 decl2 ->
        Some (Option.first_some decl1 decl2))
  else
    old_classes

let get_old_classes_batch
    (ctx : Provider_context.t) ~during_init (names : S_set.t) :
    shallow_class option S_map.t =
  match Provider_context.get_backend ctx with
  | Provider_backend.Pessimised_shared_memory _
  | Provider_backend.Analysis ->
    failwith "invalid"
  (* TODO(sf, 2022-10-20): Reduce duplication between the following two cases. *)
  | Provider_backend.Rust_provider_backend be ->
    let (old_classes, _funs, _typedefs, _consts, _modules) =
      Rust_provider_backend.Decl.get_old_defs
        be
        File_info.{ empty_names with n_classes = names }
    in
    fetch_missing_old_classes_remotely ctx ~during_init old_classes
  | Provider_backend.Shared_memory ->
    let old_classes = Shallow_classes_heap.Classes.get_old_batch names in
    fetch_missing_old_classes_remotely ctx ~during_init old_classes
  | Provider_backend.Local_memory _ ->
    failwith "get_old_classes_batch not implemented for Local_memory"

(** We only load a decl remotely if the hash we are expecting to have (per
    [VersionedNames]) matches the hash of the decl in the saved state. That's
    how we know that the old decl is indeed equivalent to the saved state
    version. This rules out the case where we didn't find an old decl in the
    heap because it was evicted, but the decl had changed w.r.t. the saved state
    prior to being evicted. *)
let can_use_saved_state_decl
    (ctx : Provider_context.t)
    (dep : Typing_deps.Dep.dependent Typing_deps.Dep.variant)
    (expected_hash : File_info.Decl_hash.t option) : bool =
  match (expected_hash, Remote_old_decl_client.Utils.db_path_of_ctx ~ctx) with
  | (Some expected_hash, Some db_path) ->
    (match Remote_old_decl_client.Utils.dep_to_decl_hash_opt ~dep ~db_path with
    | None -> false
    | Some saved_hash ->
      File_info.Decl_hash.equal
        expected_hash
        (File_info.Decl_hash.from_naming_table (Int64.of_string saved_hash)))
  | (None, _) ->
    (* We have no reference hash. Conservatively reject fetching an old version
       for the decl. *)
    false
  | _ -> false

let fetch_missing_old_non_classes_remotely
    (ctx : Provider_context.t)
    ~(during_init : bool)
    ~(old_names : File_info.Old_names.t)
    (old_decls :
      fun_decl option S_map.t
      * typedef_decl option S_map.t
      * const_decl option S_map.t
      * module_decl option S_map.t) :
    fun_decl option S_map.t
    * typedef_decl option S_map.t
    * const_decl option S_map.t
    * module_decl option S_map.t =
  let (old_funs, old_types, old_consts, old_modules) = old_decls in
  if
    Typechecker_options.remote_old_decl_fetching_include_non_class
      (Provider_context.get_tcopt ctx)
    && fetch_remote_old_decls ctx ~during_init
  then
    let add_missing_deps make_dep old_hashes old_decls deps =
      let add_missing name decl deps =
        let expected_hash = Option.join (S_map.find_opt name old_hashes) in
        let dep = make_dep name in
        if Option.is_none decl && can_use_saved_state_decl ctx dep expected_hash
        then
          dep :: deps
        else
          deps
      in
      S_map.fold add_missing old_decls deps
    in
    let missing_deps =
      let open Typing_deps in
      let deps =
        add_missing_deps
          (fun name -> Dep.Fun name)
          old_names.File_info.Old_names.n_funs
          old_funs
          []
      in
      let deps =
        add_missing_deps
          (fun name -> Dep.Type name)
          old_names.File_info.Old_names.n_types
          old_types
          deps
      in
      let deps =
        add_missing_deps
          (fun name -> Dep.GConst name)
          old_names.File_info.Old_names.n_consts
          old_consts
          deps
      in
      add_missing_deps
        (fun name -> Dep.Module name)
        old_names.File_info.Old_names.n_modules
        old_modules
        deps
    in
    let add_remote_old_decl (funs, types, consts, modules) decl =
      match decl with
      | NFun (name, decl) ->
        (S_map.add name (Some decl) funs, types, consts, modules)
      | NTypedef (name, decl) ->
        (funs, S_map.add name (Some decl) types, consts, modules)
      | NConst (name, decl) ->
        (funs, types, S_map.add name (Some decl) consts, modules)
      | NModule (name, decl) ->
        (funs, types, consts, S_map.add name (Some decl) modules)
      | NClass _ -> (funs, types, consts, modules)
    in
    let remote_old_decls =
      Remote_old_decl_client.fetch_old_decls ~ctx missing_deps
    in
    List.fold remote_old_decls ~init:old_decls ~f:add_remote_old_decl
  else
    old_decls

let get_old_non_classes_batch
    (ctx : Provider_context.t)
    ~(during_init : bool)
    ~(old_names : File_info.Old_names.t)
    ~(funs : S_set.t)
    ~(types : S_set.t)
    ~(consts : S_set.t)
    ~(modules : S_set.t) :
    fun_decl option S_map.t
    * typedef_decl option S_map.t
    * const_decl option S_map.t
    * module_decl option S_map.t =
  let old_decls =
    match Provider_backend.get () with
    | Provider_backend.Rust_provider_backend be ->
      let non_class_defs =
        File_info.
          {
            n_funs = funs;
            n_classes = S_set.empty;
            n_types = types;
            n_consts = consts;
            n_modules = modules;
          }
      in
      let (_classes, old_funs, old_types, old_consts, old_modules) =
        Rust_provider_backend.Decl.get_old_defs be non_class_defs
      in
      (old_funs, old_types, old_consts, old_modules)
    | _ ->
      ( Decl_heap.Funs.get_old_batch funs,
        Decl_heap.Typedefs.get_old_batch types,
        Decl_heap.GConsts.get_old_batch consts,
        Decl_heap.Modules.get_old_batch modules )
  in
  fetch_missing_old_non_classes_remotely ctx ~during_init ~old_names old_decls

let oldify_classes_batch (ctx : Provider_context.t) (names : S_set.t) : unit =
  match Provider_context.get_backend ctx with
  | Provider_backend.Pessimised_shared_memory _
  | Provider_backend.Analysis ->
    failwith "invalid"
  | Provider_backend.Rust_provider_backend _
  | Provider_backend.Shared_memory ->
    Shallow_classes_heap.Classes.oldify_batch names
  | Provider_backend.Local_memory _ ->
    failwith "oldify_classes_batch not implemented for Local_memory"

let remove_old_classes_batch (ctx : Provider_context.t) (names : S_set.t) : unit
    =
  match Provider_context.get_backend ctx with
  | Provider_backend.Pessimised_shared_memory _
  | Provider_backend.Analysis ->
    failwith "invalid"
  | Provider_backend.Rust_provider_backend _
  | Provider_backend.Shared_memory ->
    Shallow_classes_heap.Classes.remove_old_batch names
  | Provider_backend.Local_memory _ ->
    failwith "remove_old_classes_batch not implemented for Local_memory"
