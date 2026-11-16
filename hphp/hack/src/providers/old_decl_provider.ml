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
      S_map.filter (fun _key -> Option.is_none) old_classes |> S_map.keys
    in
    let remote_old_classes =
      Remote_old_decl_client.fetch_old_decls ~ctx missing_old_classes
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

let get_old_non_classes_batch
    ~(funs : S_set.t)
    ~(types : S_set.t)
    ~(consts : S_set.t)
    ~(modules : S_set.t) :
    fun_decl option S_map.t
    * typedef_decl option S_map.t
    * const_decl option S_map.t
    * module_decl option S_map.t =
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
