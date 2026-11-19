(*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Core
open Ast_compat

let strip_decl_prefix =
  S_set.of_list
    [
      "DeclFunArity";
      "DeclFunParam";
      "DeclFunParams";
      "DeclFunType";
      "DeclTparam";
      "DeclTy";
      "DeclWhereConstraint";
    ]

type flattened_longident =
  | FLident of string list
  | FLdot of flattened_longident * string list
  | FLapply of flattened_longident list

let rec flatten_longident = function
  | Ident str -> FLident [str]
  | Apply (id1, id2) ->
    let ids =
      match flatten_longident id1 with
      | FLapply ids -> ids
      | id -> [id]
    in
    FLapply (ids @ [flatten_longident id2])
  | Dot (id, str) ->
    (match flatten_longident id with
    | FLident strs -> FLident (str :: strs)
    | FLdot (id, strs) -> FLdot (id, str :: strs)
    | FLapply _ as id -> FLdot (id, [str]))

(* [> `RustType of Rust_type.t | `Module of string ] = *)
let to_string for_open id =
  let rec to_string id =
    match id with
    | FLident []
    | FLdot (_, [])
    | FLapply [] ->
      assert false
    | FLident (ty :: modules) ->
      let ty =
        match (ty, modules) with
        | _ when for_open -> Utils.convert_module_name ty
        | ("t", m :: _) -> Utils.convert_type_name m
        | _ -> Utils.convert_type_name ty
      in
      (* HACK: The oxidized version of `ty` has no phase. *)
      let ty =
        if String.equal ty "LoclTy" then
          "Ty"
        else if S_set.mem ty strip_decl_prefix then
          String.chop_prefix_exn ty ~prefix:"Decl"
        else
          ty
      in
      let modules = List.map modules ~f:Utils.convert_module_name in
      ty :: modules |> List.rev |> String.concat ~sep:"::"
    | FLdot (id, assoc_tys) ->
      let id = to_string id in
      let assoc_tys = List.map assoc_tys ~f:Utils.convert_type_name in
      assoc_tys |> List.rev |> List.cons id |> String.concat ~sep:"::"
    | FLapply (ftor :: args) ->
      let ftor = to_string ftor in
      let args = args |> List.map ~f:to_string |> String.concat ~sep:", " in
      sprintf "%s<%s>" ftor args
  in
  to_string id

let to_rust ?(for_open = false) id = flatten_longident id |> to_string for_open

let longident_to_string ?for_open id =
  to_rust ?for_open (Ast_compat.longident id)
