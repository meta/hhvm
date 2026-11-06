(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

(** Return the positions of the explicit (user-declared) parameters from which a
    value of type [this] can be obtained — i.e. [this] occurs contravariantly or
    invariantly in the parameter type — but only when [this] would be obtainable
    two or more times across the parameters in the resulting function pointer.

    A single obtainable occurrence is supported by extraction; two or more lets
    the caller obtain two values of type [this] whose runtime classes can differ,
    which the method conflates as one self type (the binary method problem).
    Invariance pins a container's element type, not its runtime contents, so
    invariant occurrences are obtainable too. For instance methods the implicit
    [this] receiver is itself an obtainable occurrence, so [~has_implicit_this:true]
    should be passed. In that case, upper bounds on method and class type
    parameters, including [where] constraints, are followed to detect values
    that can be upcast to [this].

    Covariant occurrences (e.g. a [(function(this): void)] parameter) and
    [this::T] type constant projections are never counted. *)
val explicit_this_param_positions :
  ?has_implicit_this:bool ->
  ?class_tparams:Typing_defs.decl_tparam list ->
  env:Typing_env_types.env ->
  Typing_defs_core.decl_phase Typing_defs_core.ty Typing_defs_core.fun_type ->
  Pos_or_decl.t list

val extract_static_method :
  Typing_defs_core.decl_phase Typing_defs_core.ty Typing_defs_core.fun_type ->
  class_name:Typing_defs_core.pos_id ->
  folded_class:Folded_class.t ->
  env:Typing_env_types.env ->
  Typing_env_types.env * Typing_defs_core.decl_ty Typing_defs_core.fun_type

val extract_instance_method :
  Typing_defs_core.decl_phase Typing_defs_core.ty Typing_defs_core.fun_type ->
  class_name:Typing_defs_core.pos_id ->
  folded_class:Folded_class.t ->
  env:Typing_env_types.env ->
  Typing_env_types.env * Typing_defs_core.decl_ty Typing_defs_core.fun_type
