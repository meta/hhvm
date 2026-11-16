(*
 * Copyright (c) 2015, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Typing_deps
open Decl_heap

module VersionedNames : sig
  type t = {
    old_names: File_info.Old_names.t;
    new_names: File_info.names;
  }
  [@@deriving show]

  val empty : t

  val make_unchanged : File_info.names -> t

  val merge : t -> t -> t
end

module VersionedSSet : sig
  type t = {
    old: S_set.t;
    new_: S_set.t;
  }

  type diff = {
    removed: S_set.t;
    kept: S_set.t;
    added: S_set.t;
  }

  val get_classes : VersionedNames.t -> t

  val empty : t

  val merge : t -> t -> t

  val diff : t -> diff

  val diff_cardinal : diff -> int
end

module VersionedFileInfo : sig
  module Diff : sig
    type t = {
      funs: VersionedSSet.diff;
      types: VersionedSSet.diff;
      gconsts: VersionedSSet.diff;
      modules: VersionedSSet.diff;
    }
  end

  val diff_names : VersionedNames.t -> Diff.t
end

module Class_diff : sig
  val compare : Class.t -> Class.t -> bool
end

module Class_elt_diff : sig
  val compare : Class.t -> Class.t -> [> `Changed | `Unchanged ]
end

val get_extend_deps : Typing_deps_mode.t -> DepSet.elt -> DepSet.t -> DepSet.t

val get_funs_deps :
  ctx:Provider_context.t ->
  Funs.value option S_map.t ->
  VersionedSSet.diff ->
  Fanout.t * int

val get_types_deps :
  ctx:Provider_context.t ->
  Typedef.t option S_map.t ->
  VersionedSSet.diff ->
  Fanout.t * int

val get_gconsts_deps :
  ctx:Provider_context.t ->
  GConsts.value option S_map.t ->
  VersionedSSet.diff ->
  Fanout.t * int

val get_modules_deps :
  ctx:Provider_context.t ->
  old_modules:Modules.value option S_map.t ->
  modules:VersionedSSet.diff ->
  Fanout.t * int
