(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

(** Produce warnings for "needs concrete" violations.
  * https://fburl.com/hack_needs_concrete
*)
val check_abstract_access_via_static :
  Typing_env_types.env ->
  Pos.t ->
  Pos_or_decl.t ->
  string ->
  string ->
  ('ex, 'en) Aast_defs.class_id_ ->
  unit

val check_class_get :
  Typing_env_types.env ->
  Pos.t ->
  Pos_or_decl.t ->
  string ->
  string ->
  Typing_defs.class_elt ->
  ('ex, 'en) Aast_defs.class_id_ ->
  is_function_pointer:bool ->
  bool ->
  unit

(** Produce warnings for "needs concrete" violations.
 *  https://fburl.com/hack_needs_concrete
 *)
val check_instantiation :
  Typing_env_types.env -> Pos.t -> ('ex, 'en) Aast_defs.class_id_ -> unit

(** Error if an abstract type constant [tconst_name] of the current class is
 *  resolved through `static`/`this` where `static` might refer to an abstract
 *  class (i.e. outside a `<<__NeedsConcrete>>` or instance method, or a
 *  final concrete class). Emits [Abstract_access_via_static] (`Typing[4529]`).
 *)
val check_abstract_tconst_via_static :
  Typing_env_types.env -> Pos.t -> string -> unit

(** For each reified type argument whose hint resolves an abstract type constant
 *  through `this` (e.g. `foo<this::T>()` or `new C<vec<this::T>>()`), apply
 *  [check_abstract_tconst_via_static].
 *)
val check_reified_targs :
  Typing_env_types.env ->
  Typing_defs.decl_tparam list ->
  ('ex * Aast.hint) list ->
  unit

(** Validate `<<__NeedsConcrete>>` usage on class members.
 *  - Errors on instance methods and constructors.
 *  - Errors on static methods in final concrete classes
 *    (see [Typing_env.is_concrete_class]).
 *)
val check_class_def :
  Typing_env_types.env -> Nast.class_ -> Decl_provider.class_decl -> unit
