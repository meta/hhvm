(*
 * Copyright (c) 2015, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

type get_classes_in_file = Relative_path.t -> S_set.t

type redo_type_decl_result = {
  fanout: Fanout.t;
  old_decl_missing_count: int;
      (** The number of old decls we didn't have when calculating the redecl. *)
}

module For_test : sig
  val reconcile_parallel_redecl_fanout :
    during_init:bool -> deps_mode:Typing_deps_mode.t -> Fanout.t -> Fanout.t
end

(** Oldify any defs in [defs] which aren't already in
[previously_oldified_defs], then determines which symbols need to be
re-typechecked as a result of comparing the current versions of the symbols
to their old versions.

When [type_check_testing_hooks] is provided, its [after_file_redecl] hook is
invoked after each file's declarations are computed and before comparison,
only when redeclaration runs in-process rather than in workers. *)
val redo_type_decl :
  Provider_context.t ->
  during_init:bool ->
  Multi_worker.worker list option ->
  type_check_testing_hooks:Server_env.type_check_testing_hooks option ->
  bucket_size:int ->
  get_classes_in_file ->
  previously_oldified_defs:File_info.names ->
  defs:Decl_compare.VersionedNames.t Relative_path.Map.t ->
  redo_type_decl_result

(** Mark all provided [defs] as old, as long as they were not previously
oldified. All definitions in [previously_oldified_defs] are then removed from
the decl heaps.

Typically, there are only any [previously_oldified_defs] during two-phase
redeclaration. *)
val oldify_decls_and_remove_descendants :
  Provider_context.t ->
  ?collect_garbage:bool ->
  Multi_worker.worker list option ->
  get_classes_in_file ->
  bucket_size:int ->
  defs:File_info.names ->
  unit

(** Remove provided defs from the heap of old decls.
    For classes, it removes both shallow and folded classes. *)
val remove_old_defs :
  Provider_context.t ->
  bucket_size:int ->
  Multi_worker.worker list option ->
  File_info.names ->
  unit

(** Exposed for tests only!
  For a set of classes, return all the declared classes that share their class
  elements (see Decl_class_elements).
  Not for general use case since it doesn't use lazy decl and makes sense only
  in a very particular use case of invalidate_type_decl. *)
val get_descendant_classes :
  Provider_context.t ->
  Multi_worker.worker list option ->
  bucket_size:int ->
  (Relative_path.t -> S_set.t) ->
  S_set.t ->
  S_set.t

(** Test-only *)
val remove_defs :
  File_info.names ->
  elems:Decl_class_elements.t S_map.t ->
  collect_garbage:bool ->
  unit
