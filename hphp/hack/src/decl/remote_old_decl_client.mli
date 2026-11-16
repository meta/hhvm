(*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

val fetch_old_decls :
  ctx:Provider_context.t ->
  Typing_deps.Dep.dependent Typing_deps.Dep.variant list ->
  Shallow_decl_defs.named_decl list

module Utils : sig
  val dep_to_decl_hash_opt :
    dep:Typing_deps.Dep.dependent Typing_deps.Dep.variant ->
    db_path:Naming_sqlite.db_path ->
    string option

  val dep_to_file_hash_opt :
    dep:Typing_deps.Dep.dependent Typing_deps.Dep.variant ->
    db_path:Naming_sqlite.db_path ->
    (Relative_path.t * string) option

  val db_path_of_ctx : ctx:Provider_context.t -> Naming_sqlite.db_path option
end
