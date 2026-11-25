(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude
open Typing_defs

let runtime_class_name = Hhbc_string_utils.mangle_class_name

module type Env = sig
  type env

  val get_class :
    env -> Decl_provider.type_key -> Decl_provider.class_decl Decl_entry.t

  val get_const :
    env -> Decl_provider.class_decl -> string -> class_const option
end

module Make (Env : Env) = struct
  let runtime_value env (field : tshape_field_name) :
      Const_value.concrete_value option =
    match field with
    | TSFregex_group (_, value) -> Some (Const_value.SKVInt value)
    | TSFlit_str (_, value) -> Some (Const_value.SKVString value)
    | TSFclass_const ((_, class_name), (_, "class")) ->
      let (_ : Decl_provider.class_decl Decl_entry.t) =
        Env.get_class env class_name
      in
      Some (Const_value.SKVString (runtime_class_name class_name))
    | TSFclass_const ((_, class_name), (_, member_name)) ->
      (match Env.get_class env class_name with
      | Decl_entry.Found cls -> Env.get_const env cls member_name
      | Decl_entry.DoesNotExist
      | Decl_entry.NotYetAvailable ->
        None)
      |> Option.bind ~f:(fun const ->
             match const.cc_abstract with
             | CCAbstract _ -> None
             | CCConcrete ->
               Const_value.concrete_value ~member_name const.cc_value)
end

include Make (struct
  type env = Typing_env_types.env

  let get_class = Typing_env.get_class

  let get_const = Typing_env.get_const
end)
