(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

(** Resolution of shape field names to the runtime array keys they denote. *)

module type Env = sig
  type env

  val get_class :
    env -> Decl_provider.type_key -> Decl_provider.class_decl Decl_entry.t

  val get_const :
    env -> Decl_provider.class_decl -> string -> Typing_defs.class_const option
end

module Make (Env : Env) : sig
  (** The runtime array key that [field] denotes, or [None] when it is not
      statically known: a missing class or constant, an abstract constant, or
      a constant without a recorded value. Records dependencies on the classes
      and constants it reads. *)
  val runtime_value :
    Env.env ->
    Typing_defs.tshape_field_name ->
    Typing_defs.Const_value.concrete_value option
end

(** [Make] over [Typing_env]. *)
val runtime_value :
  Typing_env_types.env ->
  Typing_defs.tshape_field_name ->
  Typing_defs.Const_value.concrete_value option
