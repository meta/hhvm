(*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Core
open Asttypes
open Parsetree
open Utils
open Output
open Convert_longident

module Env : sig
  type t

  val empty : t

  val add_defined_module : t -> string -> t

  val is_defined_submodule : t -> string -> bool
end = struct
  type t = { defined_submodules: S_set.t }

  let empty = { defined_submodules = S_set.empty }

  let add_defined_module (env : t) (module_name : string) : t =
    let module_name = String.uncapitalize module_name in
    { defined_submodules = S_set.add module_name env.defined_submodules }

  let is_defined_submodule (env : t) (module_name : string) : bool =
    let module_name = String.uncapitalize module_name in
    S_set.mem module_name env.defined_submodules
end

(* HACK: These modules are not used in any type declarations, so importing them
   will result in an "unused import" warning or an "unresolved import" error in
   Rust. *)
let module_blacklist =
  [
    (* nast.ml opens Aast, but doesn't use it in type declarations. *)
    "aast";
    "aast_defs_visitors_ancestors";
    "ast_defs_visitors_ancestors";
    "base::export";
    "core";
    "core_kernel";
    "common";
    "hh_core";
    "hh_json";
    "hh_json_helpers::adhoc_json_helpers";
    "hh_prelude";
    "naming_special_names";
    "pp_type";
    "ppx_yojson_conv_lib::yojson_conv::primitives";
    "reordered_argument_collections";
    "sexplib::std";
    "string_utils";
    "utils";
    "pos::set";
  ]

(* HACK: These submodules are defined inline in another module. We don't convert
   nested modules (I think it would be a bit of work to handle imports
   properly), so we convert them manually and re-export them when we see their
   OCaml definition. *)
let nested_modules =
  [("ast_defs", "ShapeMap"); ("typing_defs_core", "TShapeMap")]

let blacklisted = List.mem module_blacklist ~equal:String.equal

(* HACK: These submodules are defined inline in another module solely to provide
   scoping for the variant constructors. Since Rust enums define their own
   namespace for their variants, there's no need to use submodules for this on
   the Rust side. *)
let enum_modules =
  [
    ("error_codes", "Parsing");
    ("error_codes", "Naming");
    ("error_codes", "NastCheck");
    ("error_codes", "Typing");
    ("error_codes", "Init");
    ("error_codes", "Warning");
    (* An optional error set that runs only for arg --enable-global-access-check. *)
    ("error_codes", "GlobalAccessCheck");
    (* Nested module scoping constant value constructors; flattened to a
       Rust enum in typing_defs. *)
    ("typing_defs", "Const_value");
  ]

let is_manually_converted_nested_module mod_name =
  List.mem
    nested_modules
    (State.curr_module_name (), mod_name)
    ~equal:(Tuple.T2.equal ~eq1:String.equal ~eq2:String.equal)

let is_enum_module mod_name =
  List.mem
    enum_modules
    (State.curr_module_name (), mod_name)
    ~equal:(Tuple.T2.equal ~eq1:String.equal ~eq2:String.equal)

let is_enum_module_import id =
  match Ast_compat.longident id with
  | Ast_compat.(Dot (Ident mod_name, enum_type_name)) ->
    List.mem
      enum_modules
      (convert_module_name mod_name, convert_type_name enum_type_name)
      ~equal:(Tuple.T2.equal ~eq1:String.equal ~eq2:String.equal)
  | _ -> false

let string_of_module_desc = function
  | Pmod_structure _ -> "Pmod_structure"
  | Pmod_functor _ -> "Pmod_functor"
  | Pmod_apply _ -> "Pmod_apply"
  | Pmod_constraint _ -> "Pmod_constraint"
  | Pmod_unpack _ -> "Pmod_unpack"
  | Pmod_extension _ -> "Pmod_extension"
  | Pmod_ident _ -> "Pmod_ident"
  | Pmod_apply_unit _ -> "Pmod_apply_unit"

let structure_item (env : Env.t) (si : structure_item) : Env.t =
  match si.pstr_desc with
  (* A type declaration. The [type_decls] list will contain items in the case of
     mutual recursion, i.e., `type ... and`. *)
  | Pstr_type (_, type_decl :: type_decls) ->
    Convert_type_decl.type_declaration type_decl;
    List.iter
      type_decls
      ~f:(Convert_type_decl.type_declaration ~mutual_rec:true);
    env
  | Pstr_type (_, []) -> failwith "unexpected parse tree: empty Pstr_type"
  (* Convert `open Foo` to `use crate::foo::*;` *)
  | Pstr_open { popen_expr; _ } ->
    let id =
      match popen_expr.pmod_desc with
      | Pmod_ident id -> id
      | _ -> failwith "unsupported 'open' statement"
    in
    let mod_name = longident_to_string id.txt ~for_open:true in
    if blacklisted mod_name then
      log "Not opening %s: it is blacklisted" mod_name
    else
      add_glob_use mod_name;
    env
  (* Convert `module F = Foo` to `use crate::foo as f;` *)
  | Pstr_module
      {
        pmb_name = { txt = Some alias; _ };
        pmb_expr = { pmod_desc = Pmod_ident id; _ };
        _;
      } ->
    let mod_name =
      longident_to_string id.txt ~for_open:(not (is_enum_module_import id.txt))
    in
    if blacklisted mod_name then
      log "Not aliasing %s: it is blacklisted" mod_name
    else
      add_alias
        mod_name
        (if is_enum_module_import id.txt then
          alias
        else
          convert_module_name alias);
    env
  (* Convert `include Foo` to explicit re-exports (`pub use`) for every type
     exported by Foo (see {!Stringify.get_includes}). *)
  | Pstr_include { pincl_mod = { pmod_desc = Pmod_ident id; _ }; _ } ->
    let mod_name = longident_to_string id.txt ~for_open:true in
    if blacklisted mod_name then
      log "Not including %s: it is blacklisted" mod_name
    else if Env.is_defined_submodule env mod_name then
      log
        "Not including %s: its definition is local and hasn't been converted."
        mod_name
    else
      add_include mod_name;
    env
  | Pstr_module
      {
        pmb_name = { txt = Some mod_name; _ };
        pmb_expr = { pmod_desc = Pmod_structure _; _ };
        _;
      }
    when is_manually_converted_nested_module mod_name ->
    let rust_mod_name = convert_module_name mod_name in
    log
      "Not converting submodule %s: importing crate::%s instead"
      mod_name
      rust_mod_name;
    add_alias ("crate::" ^ rust_mod_name) rust_mod_name;
    env
  | Pstr_module
      {
        pmb_name = { txt = Some mod_name; _ };
        pmb_expr =
          {
            pmod_desc =
              Pmod_structure
                ({
                   pstr_desc =
                     Pstr_type
                       (_, [({ ptype_name = { txt = "t"; _ }; _ } as enum_type)]);
                   _;
                 }
                :: _);
            _;
          };
        _;
      }
    when is_enum_module mod_name ->
    log "Converting submodule %s to enum type" mod_name;
    let enum_type =
      {
        enum_type with
        ptype_name = { enum_type.ptype_name with txt = mod_name };
      }
    in
    Convert_type_decl.type_declaration enum_type;
    env
  | Pstr_module
      { pmb_name = { txt = Some name; _ }; pmb_expr = { pmod_desc; _ }; _ } ->
    let kind = string_of_module_desc pmod_desc in
    log "Not converting submodule %s: %s not supported" name kind;
    let env = Env.add_defined_module env name in
    env
  | Pstr_include { pincl_mod = { pmod_desc; _ }; _ } ->
    let kind = string_of_module_desc pmod_desc in
    log "Not converting include: %s not supported" kind;
    env
  | Pstr_exception
      { ptyexn_constructor = { pext_name = { txt = name; _ }; _ }; _ } ->
    log "Not converting exception %s" name;
    env
  | Pstr_eval _ ->
    log "Not converting Pstr_eval";
    env
  | Pstr_primitive _ ->
    log "Not converting Pstr_primitive";
    env
  | Pstr_typext _ ->
    log "Not converting Pstr_typext";
    env
  | Pstr_recmodule _ ->
    log "Not converting Pstr_recmodule";
    env
  | Pstr_module { pmb_name = { txt = None; _ }; _ } ->
    log "Not converting unnamed Pstr_module";
    env
  | Pstr_modtype _ ->
    log "Not converting Pstr_modtype";
    env
  | Pstr_class _ ->
    log "Not converting Pstr_class";
    env
  | Pstr_class_type _ ->
    log "Not converting Pstr_class_type";
    env
  | Pstr_extension _ ->
    log "Not converting Pstr_extension";
    env
  (* Doc comments on files are represented with Pstr_attribute, so silently
     ignore them. *)
  | Pstr_attribute _ -> env
  (* Our goal is to convert types only, so silently ignore values. *)
  | Pstr_value _ -> env

let toplevel_phrase : Env.t -> toplevel_phrase -> Env.t =
 fun env -> function
  | Ptop_def items -> List.fold items ~f:structure_item ~init:env
  | Ptop_dir _ ->
    log "Not converting toplevel directive";
    env
