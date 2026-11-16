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

open Prim_defs

(*****************************************************************************)
(* Parsing modes *)
(*****************************************************************************)

type mode =
  | Mhhi (* just declare signatures, don't check anything *)
  | Mstrict (* check everything! *)
[@@deriving eq, hash, show, enum, ord, sexp_of]

val is_strict : mode -> bool

val is_hhi : mode -> bool

val string_of_mode : mode -> string

(*****************************************************************************)
(* The record produced by the parsing phase. *)
(*****************************************************************************)

(** This type replicates what's in Naming_types.name_kind, but with less structure.
It'd be nice to unify them. *)
type name_type =
  | Fun [@value 3]
  | Class [@value 0]
  | Typedef [@value 1]
  | Const [@value 4]
  | Module [@value 5]
[@@deriving eq, show, enum, ord]

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

type pos =
  | Full of Pos.t
  | File of name_type * Relative_path.t
[@@deriving eq, show]

module Decl_hash : sig
  (** The OCaml equivalent of Rust's [hh24_types::DeclHash]. Values originate
  from calling [hh_hash::hash] on an [oxidized::shallow_decl_defs::Decl] in Rust
  and enter OCaml through the FFI or by loading a previously stored hash. *)
  type t [@@deriving eq, show]

  (** The only escape hatch for creating these values in OCaml. Only for
  reading declaration hashes from the [DECL_HASH] column of the
  [NAMING_SYMBOLS] table. *)
  val from_naming_table : Int64.t -> t

  val to_int64 : t -> Int64.t
end

type id = {
  pos: pos;
  name: string;
  decl_hash: Decl_hash.t option;
}
[@@deriving eq, show]

val pos_full : Pos.t * string * Decl_hash.t option -> id

val get_pos_filename : pos -> Relative_path.t

type hash_type = Int64.t option [@@deriving eq]

type ids = {
  funs: id list;
  classes: id list;
  typedefs: id list;
  consts: id list;
  modules: id list;
}
[@@deriving show]

(** [File_info.t] is (1) what we get out of the parser, with Full positions;
(2) the API for putting stuff into and taking stuff out of saved-state naming table (with File positions)
*)
type t = {
  position_free_decl_hash: hash_type;
      (** The hash value of all the decls stripped of their positions.
          We use this to see if two versions of a file are "similar", i.e. their
          declarations only differ by position information.  *)
  file_mode: mode option;
  ids: ids;
  comments: (Pos.t * comment) list option;
}
[@@deriving show]

val empty_ids : ids

val empty_t : t

(*****************************************************************************)
(* The simplified record used after parsing. *)
(*****************************************************************************)

(** [File_info.names] is a cut-down version of [File_info.t], one that we use internally
for decl-diffing and other fanout calculations. *)
type names = {
  n_funs: S_set.t;
  n_classes: S_set.t;
  n_types: S_set.t;
  n_consts: S_set.t;
  n_modules: S_set.t;
}
[@@deriving show]

(*****************************************************************************)
(* The record used in our saved state. *)
(*****************************************************************************)

(** Although [File_info.t] is the public API for storing/retrieving entries in the naming-table,
we actually store the naming-table on disk as [File_info.saved] - it's basically the same but
has a slightly more compact representation in order to save space. *)
type saved

val empty_names : names

module Old_names : sig
  (** Each key is a symbol in a file's old contents (i.e., previous contents
      before the ongoing incremental re-check). The hashes describe the old hash
      of the winning decl for that symbol.
      The "winning" part is subtle:
      - If there was no duplicate naming error it just means "the previous
        version of this decl in this file"
      - if there was a duplicate naming error, the winning (i.e., actually used)
        definition may have been in a different file. In that case, the hash is
        for that declaration in a different file!

      These hashes are used for validating the old version of the decl (if we
      can recover it), prior to old decl comparison. That's why we only care
      about the winning decl: If there was a duplicate name error before and we
      have access to an old decl, it will only ever be the winning version.

      Note that this validation is different from deciding fanout:
      - What these hashes are used for: Deciding whether a given decl is indeed
        a valid baseline (i.e., it represents the state of the decl prior to the
        current re-check).
      - What they are not used for: Deciding whether a decl hash changed between
        the previous and current recheck.

      [None] means that the expected hash is unknown; the old declaration may
      still be available in the old decl store. *)
  type t = {
    n_funs: Decl_hash.t option S_map.t;
    n_classes: Decl_hash.t option S_map.t;
    n_types: Decl_hash.t option S_map.t;
    n_consts: Decl_hash.t option S_map.t;
    n_modules: Decl_hash.t option S_map.t;
  }
  [@@deriving show]

  val empty : t

  val of_names : names -> t

  (** Keep the first occurrence of each exact name within a declaration kind,
      including an unknown hash. IDs must be supplied in source order, and their
      hashes must already describe the previous winning declarations. *)
  val of_ids : ids -> t

  val to_names : t -> names

  (** Preserve matching winning hashes when the same symbol occurs in several
      files. Conflicting or unknown hashes merge to an unknown hash. *)
  val merge : t -> t -> t
end

(*****************************************************************************)
(* Functions simplifying the file information. *)
(*****************************************************************************)
val simplify : t -> names

val ids_to_names : ids -> names

val merge_names : names -> names -> names

val to_saved : t -> saved

val from_saved : Relative_path.t -> saved -> t

val saved_to_names : saved -> names

val to_string : t -> string

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

val diff : t -> t -> diff option

(** a position-free hash of decls in the file, generated by [Direct_decl_parser.parse_and_hash_decls] *)
type pfh_hash = Int64.t

type change = {
  path: Relative_path.t;
  old_ids: ids option;
      (** [old_ids] is None if the file didn't previously exist *)
  new_ids: ids option;  (** [new_ids] is None if the file has been deleted *)
  new_pfh_hash: pfh_hash option;
      (** [new_pfh_hash] is None if the file has been deleted *)
}
