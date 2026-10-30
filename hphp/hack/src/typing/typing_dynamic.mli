(*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

val add_require_dynamic_bounds :
  Typing_env_types.env -> Decl_provider.class_decl -> Typing_env_types.env

val check_property_sound_for_dynamic_read :
  on_error:('a -> string -> string -> Pos_or_decl.t * string -> Typing_error.t) ->
  Typing_env_types.env ->
  string ->
  'a * string ->
  Typing_defs.locl_ty ->
  Typing_error.t option

val check_property_sound_for_dynamic_write :
  this_class:Folded_class.t option ->
  on_error:('a -> string -> string -> Pos_or_decl.t * string -> Typing_error.t) ->
  Typing_env_types.env ->
  string ->
  'a * string ->
  Typing_defs.decl_ty ->
  Typing_defs.locl_ty option ->
  Typing_error.t option

(* From the signature alone, are function parameter hints safe for dynamic?
 * i.e. (1) present, and (2) fully-enforced, or `dynamic`.
 *)
val function_parameters_safe_for_dynamic :
  this_class:Folded_class.t option ->
  Typing_env_types.env ->
  Typing_defs.decl_ty option list ->
  bool

(* checks that a method can be invoked in a dynamic context by ensuring that
   the types of its arguments are enforceable and its return type can be
   coerced to dynamic *)

val sound_dynamic_interface_check :
  this_class:Folded_class.t option ->
  Typing_env_types.env ->
  Typing_defs.decl_ty option list ->
  Typing_defs.locl_ty ->
  bool

val sound_dynamic_interface_check_from_fun_ty :
  this_class:Folded_class.t option ->
  Typing_env_types.env ->
  Typing_defs.decl_ty Typing_defs.fun_type ->
  bool

val maybe_wrap_with_supportdyn :
  should_wrap:bool ->
  Typing_reason.t ->
  Typing_defs.locl_ty Typing_defs_core.fun_type ->
  Typing_defs.locl_ty

(* Make parameters of SDT types (tuples, shapes, and classes) into like types,
 * if that results in a well-formed type, otherwise leave them alone.
 * Return None if no change was made.
 * Example input: shape('a' => int, 'b' => string)
 * Output: shape('a' => ~int, 'b' => ~string)
 * Shape splats recursively push likes into their operands.
 * Example input: shape(...T, 'a' => int)
 * Output: shape(...T, 'a' => ~int)
 *
 * Example input: dict<int,string>
 * Output: dict<int,~string> (because ~int doesn't subtype arraykey)
 *)
val try_push_like :
  Typing_env_types.env ->
  Typing_defs.locl_ty ->
  Typing_env_types.env * Typing_defs.locl_ty option

(* If ty is ~ty0 then return ty0
 * Otherwise, recursively apply like-stripping to all covariant positions
 *   e.g. tuple and shape components, covariant type arguments to generic classes.
 *)
val strip_covariant_like :
  Typing_env_types.env ->
  Typing_defs.locl_ty ->
  Typing_env_types.env * Typing_defs.locl_ty
