(*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

type decls = (string * Shallow_decl_defs.decl) list [@@deriving show]

(** WARNING: This type has not undergone the processing that you often expect in the typechecker,
    e.g. deregistering PHPStdLib symbols, or putting into forward lexical order, or removing
    duplicates. It's often a good idea to use ParsedFileWithHashes instead (or rather, one of
    its wrappers). *)
type parsed_file = {
  pf_mode: File_info.mode option;  (** Was the file Hack or HHI? *)
  pf_file_attributes: Typing_defs.user_attribute list;
  pf_function_attributes: (string * Typing_defs.user_attribute list) list;
      (** User attributes on top-level functions, keyed by elaborated name. *)
  pf_decls: decls;
  pf_disable_xhp_element_mangling: bool;
      (** How did the parser handle xhp elements? This is a copy of
          the setting from DeclParserOptions, for use when converting
          Decls to Facts. *)
  pf_has_first_pass_parse_errors: bool;
      (** True if the FFP detected parse errors while parsing. Other parse errors
          are detected in a second pass over the CST, and this field does not
          indicate whether errors would be detected in that second pass. *)
  pf_module_membership: string option;
      (** The module this file is a part of, if any. *)
  pf_package_membership: string option;  (** The package this file is part of. *)
}

type parsed_file_with_hashes = {
  pfh_mode: File_info.mode option;
  pfh_hash: File_info.pfh_hash;
      (** position insensitive hash of all decls in the file *)
  pfh_decls: (string * Shallow_decl_defs.decl * File_info.Decl_hash.t) list;
      (** (name, decl, position-sensitive hash of this decl) *)
}

(** NOTE: this doesn't respect deregister_php_lib, and has decls in reverse lexical order. *)
val parse_decls :
  Decl_parser_options.t -> Relative_path.t -> string -> parsed_file

(** NOTE: this doesn't respect deregister_php_lib, and has decls in reverse lexical order *)
val parse_and_hash_decls :
  Decl_parser_options.t ->
  bool ->
  Relative_path.t ->
  string ->
  parsed_file_with_hashes

(** NOTE: this takes input in reverse-lexical-order, and emits File_info.t in forward lexical order *)
val decls_to_fileinfo :
  Relative_path.t -> parsed_file_with_hashes -> File_info.t

val decls_to_addenda : parsed_file_with_hashes -> File_info.si_addendum list

module type Metadata = sig
  type t
end

module Concurrent (Metadata : Metadata) : sig
  type handle

  (** For concurrent parsing of files, you can either pass [content=None] to
  have the concurrent worker read the file off disk, or pass [content=Some]
  to have the worker use this content. This is useful for [Provider_context.entry]
  where we want to parse unsaved IDE content. *)
  type content = string option

  (** This does no work, and doesn't spawn any threads. It merely
  stores [opts, root, hhi, ...] in a structure, and allocates a concurrent channel. *)
  val start :
    opts:Decl_parser_options.t ->
    root:Path.t ->
    hhi:Path.t ->
    tmp:Path.t ->
    dummy:Path.t ->
    handle

  (** This causes the listed relative-paths to go in a per-handle queue to be
  direct-decl-parsed. You can trust that multiple cores will be used
  to do that parsing. This function will block until the next
  available path has been parsed -- it might be one of the paths
  we passed into this call, or it might even have been an earlier call.

  It is normal to invoke this function with an empty list, indicating that
  the caller has no further workitems they wish to add, but they still
  want to finish collecting results from previous calls.

  This function will return [Some] with a result, or [None] if there
  will be no further results (i.e. if all pending paths have been done).

  The caller can provide metadata. For a given [(path, metadata)] that's passed in,
  it will be returned (either on this call or a subsequent call) with the exact
  same [(path, metadata)]. *)
  val enqueue_next_and_get_earlier_results :
    handle ->
    (Relative_path.t * Metadata.t * content) list ->
    (Relative_path.t * Metadata.t * parsed_file) option
end
