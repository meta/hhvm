(*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

val validate_hint :
  Typing_env_types.env -> Aast.hint -> Type_validator.error_emitter -> unit

val validate_type :
  Typing_env_types.env ->
  Pos.t ->
  Typing_defs.decl_ty ->
  Type_validator.error_emitter ->
  unit

(** Under [ban_this_refinement_in_generic_class], errors on an [is]/[as] hint
    that mentions [this] inside a generic class: the runtime checks only the
    class, not its type arguments. *)
val check_this_refinement_in_generic_class :
  Typing_env_types.env -> Aast.hint -> unit
