(*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Core
open Asttypes
open Longident
open Parsetree
open Utils
open State
open Rust_type

let primitives =
  [ "()"; "isize"; "usize"; "i64"; "u64"; "i32"; "u32"; "i16"; "u16"; "i8";
    "u8"; "f32"; "f64"; "char"; "bool" ]
  [@@ocamlformat "disable"]

let is_primitive ty args =
  List.is_empty args && List.mem primitives ty ~equal:String.equal

let rec is_copy ty =
  if is_ref ty then
    true
  else
    let (ty, targs) = type_name_and_params ty in
    Configuration.is_known (Configuration.copy_type ty) true
    || is_primitive ty targs
    || (String.equal ty "Option"
       || String.equal ty "std::cell::Cell"
       || String.equal ty "std::cell::RefCell")
       && is_copy (List.hd_exn targs)

(** A list of (<module>, <ty1>, <ty2>) tuples where we need to add indirection,
  i.e. add a reference.
  In the definition of <module>::<ty1>, instances of <ty2> need to be boxed
  (for instances of mutual recursion where we would otherwise define types of
  infinite size). *)
let add_indirection_between () =
  [
    ("typing_defs_core", "ConstraintType", "ConstraintType_");
    ("aast_defs", "Hint", "Hint_");
    ("patt_locl_ty", "PattLoclTy", "Shape");
    ("patt_locl_ty", "PattLoclTy", "Params");
    ("patt_locl_ty", "ShapeField", "PattLoclTy");
    ("patt_typing_error", "PattTypingError", "Secondary");
    ("typing_defs_core", "TypePredicate", "TypePredicate_");
    ("typing_defs_core", "Ty", "Ty_");
  ]

let equal_s3 = [%derive.eq: string * string * string]

let should_add_indirection ~seen_indirection (ty : Rust_type.t) =
  let (ty, _) = type_name_and_params ty in
  (not seen_indirection)
  && (String.equal (self ()) ty
     || List.mem
          (add_indirection_between ())
          (curr_module_name (), self (), ty)
          ~equal:equal_s3)

let add_rcoc_between = [("file_info", "Pos", "relative_path::RelativePath")]

let should_add_rcoc ty =
  List.mem add_rcoc_between (curr_module_name (), self (), ty) ~equal:equal_s3

(* These types inherently add an indirection, so we don't need to box instances
   of recursion in their type arguments. *)
let indirection_types = S_set.of_list ["Vec"]

let rec core_type ?(seen_indirection = false) ~safe_ints (ct : core_type) :
    Rust_type.t =
  match ct.ptyp_desc with
  | Ptyp_var name -> convert_type_name name |> rust_type_var
  | Ptyp_alias (_, { txt = name; _ }) ->
    rust_type (convert_type_name name) [] []
  | Ptyp_tuple tys -> tuple ~safe_ints (Ast_compat.tuple_types tys)
  | Ptyp_arrow _ -> raise (Skip_type_decl "it contains an arrow type")
  | Ptyp_constr (id, args) ->
    constructor_type
      ~seen_indirection
      ~safe_ints
      (Ast_compat.longident id.txt)
      args
  | Ptyp_any -> raise (Skip_type_decl "cannot convert type Ptyp_any")
  | Ptyp_object _ -> raise (Skip_type_decl "cannot convert type Ptyp_object")
  | Ptyp_class _ -> raise (Skip_type_decl "cannot convert type Ptyp_class")
  | Ptyp_variant _ -> raise (Skip_type_decl "cannot convert type Ptyp_variant")
  | Ptyp_poly _ -> raise (Skip_type_decl "cannot convert type Ptyp_poly")
  | Ptyp_package _ -> raise (Skip_type_decl "cannot convert type Ptyp_package")
  | Ptyp_extension _ ->
    raise (Skip_type_decl "cannot convert type Ptyp_extension")
  | _ -> raise (Skip_type_decl "cannot convert this type")

and constructor_type ~seen_indirection ~safe_ints name args =
  let open Ast_compat in
  match (name, args) with
  | (Ident ("byte_string" | "t_byte_string"), []) ->
    rust_type "bstr::BString" [] []
  | (Dot (Ident "Path", "t"), []) -> rust_simple_type "std::path::PathBuf"
  | (Dot (Ident "Hash", "hash_value"), [])
  | (Dot (Dot (Ident "Ident_provider", "Ident"), "t"), [])
  | (Dot (Ident "Expression_id", "t"), [])
  | (Dot (Ident "Tvid", "t"), []) ->
    rust_type "isize" [] []
  (* [Const_value.t] is a nested module (in typing_defs) whose sole purpose
     is to scope the variant constructors; [enum_modules] in
     convert_toplevel_phrase flattens it to a Rust enum [ConstValue] defined
     directly in the typing_defs crate, so references must not include the
     [const_value] submodule segment. *)
  | (Dot (Ident "Const_value", "t"), []) -> rust_type "ConstValue" [] []
  | (Dot (Dot (Ident "Typing_defs", "Const_value"), "t"), []) ->
    rust_type "typing_defs::ConstValue" [] []
  | _ ->
    let id =
      match name with
      | Ident "unit" -> "()"
      | Ident "int" ->
        if safe_ints then
          "ocamlrep::OCamlInt"
        else
          "isize"
      | Ident "bool" -> "bool"
      | Ident "float" -> "f64"
      | Ident "list" -> "Vec"
      | Ident "ref" -> "std::cell::RefCell"
      | Dot (Ident "Int64", "t") ->
        Output.add_extern_use "ocamlrep_caml_builtins::Int64";
        "Int64"
      | id -> Convert_longident.to_rust id
    in
    let id =
      if String.equal id "T" then
        convert_type_name @@ curr_module_name ()
      else
        id
    in
    let extern_type = Configuration.extern_type id in
    let id = Option.value extern_type ~default:id in
    let seen_indirection = seen_indirection || S_set.mem id indirection_types in
    let args =
      (* HACK: eliminate phase type arguments *)
      match args with
      | [{ ptyp_desc = Ptyp_var "phase"; _ }]
      | [{ ptyp_desc = Ptyp_var "ty"; _ }]
      | [{ ptyp_desc = Ptyp_constr ({ txt = Lident "decl_phase"; _ }, _); _ }]
      | [{ ptyp_desc = Ptyp_constr ({ txt = Lident "locl_phase"; _ }, _); _ }]
        ->
        []
      | _ when String.equal id "FunType" -> []
      | _ -> args
    in
    let lifetime = [] in
    let args = List.map args ~f:(core_type ~seen_indirection ~safe_ints) in

    if should_add_rcoc id then
      rust_type "std::sync::Arc" [] [rust_type id lifetime args]
    (* Direct or indirect recursion *)
    else if should_add_indirection ~seen_indirection (rust_type id [] args) then
      rust_type "Box" [] [rust_type id [] args]
    else
      rust_type id lifetime args

and tuple ?(seen_indirection = false) ~safe_ints types =
  List.map ~f:(core_type ~seen_indirection ~safe_ints) types
  |> rust_type "()" []

let core_type = core_type ~seen_indirection:false

let is_primitive ty = is_primitive ty []
