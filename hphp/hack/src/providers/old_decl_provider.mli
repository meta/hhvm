(*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Shallow_decl_defs

val get_old_classes_batch :
  Provider_context.t ->
  during_init:bool ->
  S_set.t ->
  shallow_class option S_map.t

(** Remote fallback, when enabled, requires the expected old hash to match
    the saved-state hash. *)
val get_old_non_classes_batch :
  Provider_context.t ->
  during_init:bool ->
  old_names:File_info.Old_names.t ->
  funs:S_set.t ->
  types:S_set.t ->
  consts:S_set.t ->
  modules:S_set.t ->
  fun_decl option S_map.t
  * typedef_decl option S_map.t
  * const_decl option S_map.t
  * module_decl option S_map.t

val oldify_classes_batch : Provider_context.t -> S_set.t -> unit

val remove_old_classes_batch : Provider_context.t -> S_set.t -> unit
