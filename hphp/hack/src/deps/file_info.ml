(*
 * Copyright (c) 2015, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

(*****************************************************************************)
(* This module defines the data structured used to describe the content of
 * a file.
 * The parser constructs File_info.t structs, that contain names and positions
 * plus some extra info required for the build.
 * After the names have been checked (Naming.make_env), we "simplify" the
 * struct and only keep the names defined in the files we know about.
 *)
(*****************************************************************************)

open Hh_prelude
open Prim_defs

(*****************************************************************************)
(* Parsing modes *)
(*****************************************************************************)

type mode =
  | Mhhi  (** just declare signatures, don't check anything *)
  | Mstrict  (** check everything! *)
[@@deriving eq, hash, show, enum, ord, sexp_of]

let is_strict = function
  | Mstrict -> true
  | Mhhi -> false

let is_hhi = function
  | Mstrict -> false
  | Mhhi -> true

let string_of_mode = function
  | Mhhi -> "hhi"
  | Mstrict -> "strict"

let pp_mode fmt mode =
  Format.pp_print_string fmt
  @@
  match mode with
  | Mhhi -> "Mhhi"
  | Mstrict -> "Mstrict"

(*****************************************************************************)
(* Positions of names in a file *)
(*****************************************************************************)

type name_type =
  | Fun [@value 3]
  | Class [@value 0]
  | Typedef [@value 1]
  | Const [@value 4]
  | Module [@value 5]
[@@deriving eq, show { with_path = false }, enum, ord]

(** And here's a version with more detail and still less structure! This
one is good for members as well as top-level symbols. It's used to get
a bit more out of direct-decl-parse, used to populate the search indexer
(e.g. the icon that apepars in autocomplete suggestions). *)
type si_kind =
  | SI_Class
  | SI_Interface
  | SI_Enum
  | SI_Trait
  | SI_Unknown
  | SI_Mixed
  | SI_Function
  | SI_Typedef
  | SI_GlobalConstant
  | SI_XHP
  | SI_Namespace
  | SI_ClassMethod
  | SI_Literal
  | SI_ClassConstant
  | SI_Property
  | SI_LocalVariable
  | SI_Keyword
  | SI_Constructor
[@@deriving eq, show { with_path = false }]

(** Yet more details. The "is_abstract" and "is_final" are used e.g. for autocomplete
items and to determine whether a type can be suggested e.g. for "$x = new |". *)
type si_addendum = {
  sia_name: string;
      (** This is expected not to contain the leading namespace backslash! See [Utils.strip_ns]. *)
  sia_kind: si_kind;
  sia_is_abstract: bool;
  sia_is_final: bool;
}
[@@deriving show]

(** We define two types of positions establishing the location of a given name:
 * a Full position contains the exact position of a name in a file, and a
 * File position contains just the file and the type of toplevel entity,
 * allowing us to lazily retrieve the name's exact location if necessary.
 *)
type pos =
  | Full of Pos.t
  | File of name_type * Relative_path.t
[@@deriving eq, show]

module Decl_hash : sig
  type t [@@deriving eq, show]

  val from_naming_table : Int64.t -> t

  val to_int64 : t -> Int64.t
end = struct
  type t = Int64.t [@@deriving eq, show]

  let from_naming_table (hash : Int64.t) : t = hash

  let to_int64 (hash : t) : Int64.t = hash
end

(** An id contains a pos, name and a optional decl hash. The decl hash is None
 * only in the case when we didn't compute it for performance reasons
 *)
type id = {
  pos: pos;
  name: string;
  decl_hash: Decl_hash.t option;
}
[@@deriving eq, show]

(*****************************************************************************)
(* The record produced by the parsing phase. *)
(*****************************************************************************)

type hash_type = Int64.t option [@@deriving eq]

let pp_hash_type fmt hash =
  match hash with
  | None -> Format.fprintf fmt "None"
  | Some hash -> Format.fprintf fmt "Some (%s)" (Int64.to_string hash)

(* NB: Type [t] must be manually kept in sync with Rust type [hackrs_provider_backend::File_info] *)

type ids = {
  funs: id list;
  classes: id list;
  typedefs: id list;
  consts: id list;
  modules: id list;
}
[@@deriving show]

(** The record produced by the parsing phase. *)
type t = {
  position_free_decl_hash: hash_type;
  file_mode: mode option;
  ids: ids;
  comments: (Pos.t * comment) list option;
      (** None if loaded from saved state *)
}
[@@deriving show]

let empty_ids =
  { funs = []; classes = []; typedefs = []; consts = []; modules = [] }

let empty_t =
  {
    position_free_decl_hash = None;
    file_mode = None;
    ids = empty_ids;
    comments = Some [];
  }

let pos_full (p, name, decl_hash) =
  let pos = Full p in
  { pos : pos; name; decl_hash }

let get_pos_filename = function
  | Full p -> Pos.filename p
  | File (_, fn) -> fn

type pfh_hash = Int64.t

type change = {
  path: Relative_path.t;
  old_ids: ids option;
  new_ids: ids option;
  new_pfh_hash: pfh_hash option;
}

(*****************************************************************************)
(* The simplified record used after parsing. *)
(*****************************************************************************)

(** The simplified record used after parsing. *)
type names = {
  n_funs: S_set.t;
  n_classes: S_set.t;
  n_types: S_set.t;
  n_consts: S_set.t;
  n_modules: S_set.t;
}
[@@deriving show]

(** The simplified record stored in saved-state.*)
type saved_names = {
  sn_funs: S_set.t;
  sn_classes: S_set.t;
  sn_types: S_set.t;
  sn_consts: S_set.t;
  sn_modules: S_set.t;
}

(** Data structure stored in the saved state *)
type saved = {
  s_names: saved_names;
  s_position_free_decl_hash: Int64.t option;
  s_mode: mode option;
}

let empty_names =
  {
    n_funs = S_set.empty;
    n_classes = S_set.empty;
    n_types = S_set.empty;
    n_consts = S_set.empty;
    n_modules = S_set.empty;
  }

module Old_names = struct
  type t = {
    n_funs: Decl_hash.t option S_map.t;
    n_classes: Decl_hash.t option S_map.t;
    n_types: Decl_hash.t option S_map.t;
    n_consts: Decl_hash.t option S_map.t;
    n_modules: Decl_hash.t option S_map.t;
  }
  [@@deriving show]

  let empty : t =
    {
      n_funs = S_map.empty;
      n_classes = S_map.empty;
      n_types = S_map.empty;
      n_consts = S_map.empty;
      n_modules = S_map.empty;
    }

  let of_names (names : names) : t =
    let unknown_hashes names =
      S_set.fold (fun name acc -> S_map.add name None acc) names S_map.empty
    in
    let ({ n_funs; n_classes; n_types; n_consts; n_modules } : names) = names in
    {
      n_funs = unknown_hashes n_funs;
      n_classes = unknown_hashes n_classes;
      n_types = unknown_hashes n_types;
      n_consts = unknown_hashes n_consts;
      n_modules = unknown_hashes n_modules;
    }

  let of_ids (ids : ids) : t =
    let add_id acc { name; decl_hash; _ } =
      if S_map.mem name acc then
        acc
      else
        S_map.add name decl_hash acc
    in
    let hashes ids = List.fold ids ~init:S_map.empty ~f:add_id in
    let { funs; classes; typedefs; consts; modules } = ids in
    {
      n_funs = hashes funs;
      n_classes = hashes classes;
      n_types = hashes typedefs;
      n_consts = hashes consts;
      n_modules = hashes modules;
    }

  let to_names (old_names : t) : names =
    let names hashes = S_set.of_list (S_map.keys hashes) in
    let { n_funs; n_classes; n_types; n_consts; n_modules } = old_names in
    {
      n_funs = names n_funs;
      n_classes = names n_classes;
      n_types = names n_types;
      n_consts = names n_consts;
      n_modules = names n_modules;
    }

  let merge (left : t) (right : t) : t =
    let merge_hash _name left right =
      match (left, right) with
      | (Some left, Some right) ->
        Some
          (if Option.equal Decl_hash.equal left right then
            left
          else
            None)
      | (Some _, None) -> left
      | (None, Some _) -> right
      | (None, None) -> None
    in
    let merge_hashes = S_map.merge merge_hash in
    {
      n_funs = merge_hashes left.n_funs right.n_funs;
      n_classes = merge_hashes left.n_classes right.n_classes;
      n_types = merge_hashes left.n_types right.n_types;
      n_consts = merge_hashes left.n_consts right.n_consts;
      n_modules = merge_hashes left.n_modules right.n_modules;
    }
end

(*****************************************************************************)
(* Functions simplifying the file information. *)
(*****************************************************************************)

let name_set_of_idl idl =
  List.fold_left idl ~f:(fun acc id -> S_set.add id.name acc) ~init:S_set.empty

let ids_to_names (ids : ids) : names =
  let { funs; classes; typedefs; consts; modules } = ids in
  {
    n_funs = name_set_of_idl funs;
    n_classes = name_set_of_idl classes;
    n_types = name_set_of_idl typedefs;
    n_consts = name_set_of_idl consts;
    n_modules = name_set_of_idl modules;
  }

let simplify info = ids_to_names info.ids

let to_saved info : saved =
  let {
    ids = { funs; classes; typedefs; consts; modules };
    file_mode;
    position_free_decl_hash;
    comments = _;
  } =
    info
  in
  let sn_funs = name_set_of_idl funs in
  let sn_classes = name_set_of_idl classes in
  let sn_types = name_set_of_idl typedefs in
  let sn_consts = name_set_of_idl consts in
  let sn_modules = name_set_of_idl modules in
  let s_names = { sn_funs; sn_classes; sn_types; sn_consts; sn_modules } in
  {
    s_names;
    s_mode = file_mode;
    s_position_free_decl_hash = position_free_decl_hash;
  }

let from_saved fn saved =
  let { s_names; s_mode; s_position_free_decl_hash } = saved in
  let { sn_funs; sn_classes; sn_types; sn_consts; sn_modules } = s_names in
  let funs =
    List.map (S_set.elements sn_funs) ~f:(fun x ->
        { pos = File (Fun, fn); name = x; decl_hash = None })
  in
  let classes =
    List.map (S_set.elements sn_classes) ~f:(fun x ->
        { pos = File (Class, fn); name = x; decl_hash = None })
  in
  let typedefs =
    List.map (S_set.elements sn_types) ~f:(fun x ->
        { pos = File (Typedef, fn); name = x; decl_hash = None })
  in
  let consts =
    List.map (S_set.elements sn_consts) ~f:(fun x ->
        { pos = File (Const, fn); name = x; decl_hash = None })
  in
  let modules =
    List.map (S_set.elements sn_modules) ~f:(fun m ->
        { pos = File (Module, fn); name = m; decl_hash = None })
  in
  {
    file_mode = s_mode;
    position_free_decl_hash = s_position_free_decl_hash;
    ids = { funs; classes; typedefs; consts; modules };
    comments = None;
  }

let saved_to_names saved =
  {
    n_funs = saved.s_names.sn_funs;
    n_classes = saved.s_names.sn_classes;
    n_types = saved.s_names.sn_types;
    n_consts = saved.s_names.sn_consts;
    n_modules = saved.s_names.sn_modules;
  }

let merge_names t_names1 t_names2 =
  let { n_funs; n_classes; n_types; n_consts; n_modules } = t_names1 in
  {
    n_funs = S_set.union n_funs t_names2.n_funs;
    n_classes = S_set.union n_classes t_names2.n_classes;
    n_types = S_set.union n_types t_names2.n_types;
    n_consts = S_set.union n_consts t_names2.n_consts;
    n_modules = S_set.union n_modules t_names2.n_modules;
  }

let ids_to_string (ids : ids) : string =
  let { classes; typedefs; consts; modules; funs } = ids in
  let funs = List.map ~f:(fun id -> { id with decl_hash = None }) funs in
  let classes = List.map ~f:(fun id -> { id with decl_hash = None }) classes in
  let typedefs =
    List.map ~f:(fun id -> { id with decl_hash = None }) typedefs
  in
  let consts = List.map ~f:(fun id -> { id with decl_hash = None }) consts in
  let modules = List.map ~f:(fun id -> { id with decl_hash = None }) modules in
  [
    ("funs", funs);
    ("classes", classes);
    ("typedefs", typedefs);
    ("consts", consts);
    ("modules", modules);
  ]
  |> List.filter ~f:(fun (_, l) -> not @@ List.is_empty l)
  |> List.map ~f:(fun (kind, l) ->
         Printf.sprintf
           "%s: %s %s"
           kind
           (List.map l ~f:(fun id -> id.name) |> String.concat ~sep:",")
           (List.map l ~f:(fun id ->
                Option.value_map id.decl_hash ~default:"0" ~f:(fun hash ->
                    Decl_hash.to_int64 hash |> Int64.to_string))
           |> String.concat ~sep:","))
  |> String.concat ~sep:";"

let to_string (t : t) : string =
  let { ids; file_mode = _; position_free_decl_hash = _; comments = _ } = t in
  ids_to_string ids

type diff = {
  removed_funs: S_set.t;
  added_funs: S_set.t;
  removed_classes: S_set.t;
  added_classes: S_set.t;
  removed_types: S_set.t;
  added_types: S_set.t;
  removed_consts: S_set.t;
  added_consts: S_set.t;
  removed_modules: S_set.t;
  added_modules: S_set.t;
}

let diff f1 f2 =
  let matches_hash =
    match (f1.position_free_decl_hash, f2.position_free_decl_hash) with
    | (Some h1, Some h2) -> Int64.equal h1 h2
    | _ -> false
  in
  if matches_hash then
    None
  else
    let diff_ids ids1 ids2 =
      let removed_ids = S_set.diff ids1 ids2 in
      let added_ids = S_set.diff ids2 ids1 in
      (removed_ids, added_ids)
    in
    let f1 = simplify f1 in
    let f2 = simplify f2 in
    let (removed_funs, added_funs) = diff_ids f1.n_funs f2.n_funs in
    let (removed_classes, added_classes) = diff_ids f1.n_classes f2.n_classes in
    let (removed_types, added_types) = diff_ids f1.n_types f2.n_types in
    let (removed_consts, added_consts) = diff_ids f1.n_consts f2.n_consts in
    let (removed_modules, added_modules) = diff_ids f1.n_modules f2.n_modules in
    let is_empty =
      List.fold
        ~f:(fun acc s -> (not (S_set.is_empty s)) || acc)
        [
          removed_funs;
          added_funs;
          removed_classes;
          added_classes;
          removed_types;
          added_types;
          removed_consts;
          added_consts;
          removed_modules;
          added_modules;
        ]
        ~init:false
    in
    if is_empty then
      None
    else
      Some
        {
          removed_funs;
          added_funs;
          removed_classes;
          added_classes;
          removed_types;
          added_types;
          removed_consts;
          added_consts;
          removed_modules;
          added_modules;
        }
