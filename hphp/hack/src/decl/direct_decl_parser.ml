(*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *)

open Hh_prelude

type decls = (string * Shallow_decl_defs.decl) list [@@deriving show]

(** See .mli *)
type parsed_file = {
  pf_mode: File_info.mode option;
  pf_file_attributes: Typing_defs.user_attribute list;
  pf_function_attributes: (string * Typing_defs.user_attribute list) list;
  pf_decls: decls;
  pf_disable_xhp_element_mangling: bool;
  pf_has_first_pass_parse_errors: bool;
  pf_module_membership: string option;
  pf_package_membership: string option;
}

(* Not oxidised: the Rust record uses native hash types and hand-written
   serialisation to OCaml Int64 without a temporary Rust vector. Avoid
   special-case generator mappings here; keep the FFI layout in sync manually
   with rust_decl_ffi::OcamlParsedFileWithHashes::to_ocamlrep. *)
type parsed_file_with_hashes = {
  pfh_mode: File_info.mode option;
  pfh_hash: File_info.pfh_hash;
  pfh_decls: (string * Shallow_decl_defs.decl * File_info.Decl_hash.t) list;
}
[@@oxidize.exclude]

external parse_decls :
  Decl_parser_options.t -> Relative_path.t -> string -> parsed_file
  = "hh_parse_decls_ffi"

external parse_and_hash_decls :
  Decl_parser_options.t ->
  bool ->
  Relative_path.t ->
  string ->
  parsed_file_with_hashes = "hh_parse_and_hash_decls_ffi"

(* NB: Must be manually kept in sync with Rust function
   `rust_batch_index_ffi::parsed_file_to_file_info' *)
let decls_to_fileinfo fn (parsed_file : parsed_file_with_hashes) =
  let file_mode = parsed_file.pfh_mode in
  let position_free_decl_hash = Some parsed_file.pfh_hash in
  let ids =
    List.fold
      parsed_file.pfh_decls
      ~init:File_info.empty_ids
      ~f:(fun acc (name, decl, hash) ->
        let pos p = File_info.Full (Pos_or_decl.fill_in_filename fn p) in
        match decl with
        | Shallow_decl_defs.Class c ->
          let pos = pos (fst c.Shallow_decl_defs.sc_name) in
          let decl_hash = Some hash in
          let info = File_info.{ pos; name; decl_hash } in
          File_info.{ acc with classes = info :: acc.classes }
        | Shallow_decl_defs.Fun f ->
          let pos = pos f.Typing_defs.fe_pos in
          let decl_hash = Some hash in
          let info = File_info.{ pos; name; decl_hash } in
          File_info.{ acc with funs = info :: acc.funs }
        | Shallow_decl_defs.Typedef tf ->
          let pos = pos tf.Typing_defs.td_pos in

          let decl_hash = Some hash in
          let info = File_info.{ pos; name; decl_hash } in
          File_info.{ acc with typedefs = info :: acc.typedefs }
        | Shallow_decl_defs.Const c ->
          let pos = pos c.Typing_defs.cd_pos in
          let decl_hash = Some hash in
          let info = File_info.{ pos; name; decl_hash } in
          File_info.{ acc with consts = info :: acc.consts }
        | Shallow_decl_defs.Module m ->
          let pos = pos m.Typing_defs.mdt_pos in
          let decl_hash = Some hash in
          let info = File_info.{ pos; name; decl_hash } in
          File_info.{ acc with modules = info :: acc.modules })
  in
  { File_info.ids; position_free_decl_hash; file_mode; comments = None }

let decls_to_addenda (parsed_file : parsed_file_with_hashes) :
    File_info.si_addendum list =
  (* NB: Must be manually kept in sync with Rust function `si_addenum::get_si_addenda` *)
  List.filter_map parsed_file.pfh_decls ~f:(fun (name, decl, _hash) ->
      let sia_name = Utils.strip_ns name in
      let sia_kind =
        match decl with
        | Shallow_decl_defs.(Class { sc_kind = Ast_defs.Cclass _; _ }) ->
          Some File_info.SI_Class
        | Shallow_decl_defs.(Class { sc_kind = Ast_defs.Cinterface; _ }) ->
          Some File_info.SI_Interface
        | Shallow_decl_defs.(Class { sc_kind = Ast_defs.Ctrait; _ }) ->
          Some File_info.SI_Trait
        | Shallow_decl_defs.(
            Class { sc_kind = Ast_defs.(Cenum | Cenum_class _); _ }) ->
          Some File_info.SI_Enum
        | Shallow_decl_defs.Fun _ -> Some File_info.SI_Function
        | Shallow_decl_defs.Typedef _ -> Some File_info.SI_Typedef
        | Shallow_decl_defs.Const _ -> Some File_info.SI_GlobalConstant
        | Shallow_decl_defs.Module _ -> None
      in
      let (sia_is_abstract, sia_is_final) =
        match decl with
        | Shallow_decl_defs.Class c ->
          (c.Shallow_decl_defs.sc_abstract, c.Shallow_decl_defs.sc_final)
        | _ -> (false, false)
      in
      match sia_kind with
      | None -> None
      | Some sia_kind ->
        Some { File_info.sia_name; sia_kind; sia_is_abstract; sia_is_final })

module type Metadata = sig
  type t
end

module Concurrent (Metadata : Metadata) = struct
  type handle

  type content = string option

  external start :
    opts:Decl_parser_options.t ->
    root:Path.t ->
    hhi:Path.t ->
    tmp:Path.t ->
    dummy:Path.t ->
    handle = "hh_concurrent_parse_start_ffi"

  external enqueue_next_and_get_earlier_results :
    handle ->
    (Relative_path.t * Metadata.t * content) list ->
    (Relative_path.t * Metadata.t * parsed_file) option
    = "hh_concurrent_parse_step_ffi"
end
