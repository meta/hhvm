(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

val remove_defs_from_reverse_naming_table :
  Naming_table.t -> 'a Relative_path.Map.t -> unit

(** Capture old names and their winning declaration hashes before updating
    reverse naming.

    If an input file participated in a naming conflict in the previous check,
    every other file involved in that conflict must also be included in
    [defs_per_file_parsed]. The server ensures this by including the entire
    previous [failed_naming] set.

    Conflicts introduced by the parsed contents are handled here: the snapshot
    includes the parsed files and the existing owners of all their new names,
    so files subsequently discovered through new naming conflicts are covered. *)
val get_old_names_batch :
  Provider_context.t ->
  Naming_table.t ->
  defs_per_file_parsed:File_info.t Relative_path.Map.t ->
  File_info.Old_names.t Relative_path.Map.t

val get_old_and_new_defs_in_files :
  Naming_table.t ->
  Naming_table.t ->
  Relative_path.Set.t ->
  Decl_compare.VersionedNames.t Relative_path.Map.t

(** If the only things that would change about file analysis are positions,
    we're not going to recheck it, and positions in its error list might
    become stale. Look if any of those positions refer to files that have
    actually changed and add them to files to recheck.

  @param reparsed   Set of files that were reparsed (so their ASTs and positions
                    in them could have changed.

  @param diagnostics     Current global diagnostic list
*)
val add_files_with_stale_errors :
  Provider_context.t ->
  reparsed:Relative_path.Set.t ->
  Diagnostics.t ->
  Relative_path.Set.t ->
  Relative_path.Set.t

val resolve_files :
  Provider_context.t ->
  reparsed:Relative_path.Set.t ->
  Server_env.env ->
  Fanout.t ->
  Relative_path.Set.t

val get_files_to_recheck :
  Provider_context.t ->
  Server_env.env ->
  Fanout.t ->
  reparsed:Relative_path.Set.t ->
  diagnostics:Diagnostics.t ->
  Relative_path.Set.t
