(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

(** Represents a type in disjunctive normal form. For example
   the type (A & B) | C is represented as [[A; B]; [C]] *)
type dnf_ty = Typing_defs.locl_ty list list

val is_safe_for_strict_equality_refinement :
  Typing_env_types.env -> Typing_defs.locl_ty -> Typing_env_types.env * bool

module Uninstantiated_typing_logic : sig
  type subtype_prop

  val valid : subtype_prop

  val invalid : subtype_prop

  val conj : subtype_prop -> subtype_prop -> subtype_prop

  val disj : subtype_prop -> subtype_prop -> subtype_prop

  val instantiate_prop :
    Typing_env_types.env ->
    Typing_defs.locl_ty I_map.t ->
    Pos.t ->
    subtype_prop ->
    Typing_logic.subtype_prop
end

(** The partition of type over [predicate].

  [left] describes a type that is a subset of the values that
    pass the predicate

  [right] describes a type that is a subset of the values that
    fail the predicate

  [span] describes a type whose set of values contain values that
    both pass and fail the predicate

  [assumptions] a subtype prop that should be considered true when the predicate
    is satisfied
*)
type ty_partition = {
  predicate: Typing_defs.type_predicate;
  left: dnf_ty;
  span: dnf_ty;
  right: dnf_ty;
  true_assumptions: Uninstantiated_typing_logic.subtype_prop;
  false_assumptions: Uninstantiated_typing_logic.subtype_prop;
}

(**
  Given a [Typing_defs.locl_ty] and a [Typing_defs.type_predicate], partition the
  [Typing_defs.locl_ty] into three types:

    left: For the portition of the type that is a subset of the values that
      passes the predicate at runtime

    right: For the portition of the type that is a subset of the values that
      fails the predicate at runtime

    span: For the portition of the type that contains values that both
      pass and fails the predicate

  As an example consider partition the type (num | arraykey | T) where T is a generic type
    bounded by nonnull over the predicate (IsTag IntTag)

  This will produce
    left = int (num & (IsTag IntTag) = int and arraykey & (IsTag IntTag) = int)
    right = float | string (num & !(IsTag IntTag) = float and arraykey & !(IsTag IntTag) = string)
    span = T (T can contain values that are ints and values that are not ints)
*)
val partition_ty :
  Typing_env_types.env ->
  Typing_defs.locl_ty ->
  Typing_defs.type_predicate ->
  Typing_env_types.env * ty_partition

(**
  Returns true if the type falls entirely on the left of the predicate
*)
val passes_predicate :
  Typing_env_types.env ->
  Typing_defs.locl_ty ->
  Typing_defs.type_predicate ->
  bool

module TyPredicate : sig
  val of_ty :
    Typing_env_types.env ->
    Typing_defs.locl_ty ->
    (Typing_defs.type_predicate, string) Result.t

  val instantiate_wildcards_for_predicate :
    Typing_env_types.env ->
    Typing_defs.type_predicate ->
    Pos.t ->
    Typing_env_types.env
    * ((Typing_defs.decl_tparam * string) * Typing_defs.locl_ty) I_map.t

  val to_ty :
    Typing_env_types.env ->
    Typing_defs.locl_ty I_map.t ->
    Pos.t ->
    Typing_defs.type_predicate ->
    Typing_defs.locl_ty

  val to_ty_without_instantiation_opt :
    Typing_env_types.env ->
    Typing_defs.type_predicate ->
    Typing_defs.locl_ty option
end
