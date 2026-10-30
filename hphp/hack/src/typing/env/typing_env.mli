(*
 * Copyright (c) 2015, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)
open Typing_env_types
open Decl_provider
open Typing_defs
open Typing_defs_constraints
module Cls = Folded_class
module TPEnv = Type_parameter_env

(* Expose the subset of Typing_env that is safe to use
   * in tast checks: that is, anything that depends only
   * on the tast and providers (not mutable or visitor state).
   *
   * You can use this to avoid the evil Tast_env.tast_env_as_typing_env
*)
module Expose_to_tast_env : sig
  type env

  type 'a class_or_typedef_result =
        'a Decl_enforceability.class_or_typedef_result =
    | ClassResult of 'a
    | TypedefResult of Typing_defs.typedef_type

  (** Get class declaration from the appropriate backend and add dependency. *)
  val get_class : env -> type_key -> class_decl Decl_entry.t

  val get_enum : env -> type_key -> class_decl Decl_entry.t

  val inside_expr_tree : env -> Aast_defs.class_name -> env

  val get_tcopt : env -> Typechecker_options.t

  val get_self_ty : env -> locl_ty option

  val get_self_id : env -> string option

  val get_parent_id : env -> string option

  val is_in_expr_tree : env -> bool

  val is_enum_class : env -> type_key -> bool

  (** Get class constant declaration from the appropriate backend and add dependency. *)
  val get_const : env -> class_decl -> string -> class_const option

  (** Get class constants declaration from the appropriate backend and add dependency. *)
  val consts : env -> class_decl -> (string * class_const) list

  (** Get class method declaration from the appropriate backend and add dependency. *)
  val get_method : env -> class_decl -> string -> class_elt option

  (** Get class property declaration from the appropriate backend and add dependency. *)
  val get_prop : env -> class_decl -> string -> class_elt option

  (** Get static member declaration of a class from the appropriate backend and add dependency. *)
  val get_static_member :
    bool -> env -> class_decl -> string -> class_elt option

  val is_enum : env -> type_key -> bool

  (** Get function declaration from the appropriate backend and add dependency. *)
  val get_fun :
    env -> Decl_provider.fun_key -> Decl_provider.fun_decl Decl_entry.t

  val is_typedef : env -> type_key -> bool

  (** Get type alias declaration from the appropriate backend and add dependency. *)
  val get_typedef : env -> type_key -> typedef_decl Decl_entry.t

  val get_class_or_typedef :
    env -> type_key -> Folded_class.t class_or_typedef_result Decl_entry.t

  val get_class_or_typedef_tparams : env -> type_key -> decl_tparam list

  (** Get type constant declaration from the appropriate backend and add dependency. *)
  val get_typeconst : env -> class_decl -> string -> typeconst_type option

  (** Get global constant declaration from the appropriate backend and add dependency. *)
  val get_gconst : env -> gconst_key -> gconst_decl option

  (** Get class constructor declaration from the appropriate backend and add dependency. *)
  val get_construct : env -> class_decl -> class_elt option * consistent_kind

  (** Check whether a class is a valid newable bound, recording a constructor
      dependency when its consistency is inspected. *)
  val valid_newable_class : env -> class_decl -> bool

  val get_self_class : env -> class_decl Decl_entry.t

  val get_parent_class : env -> class_decl Decl_entry.t
end

include module type of Expose_to_tast_env with type env := Typing_env_types.env

val simplify_unions_ref : (env -> locl_ty -> env * locl_ty) ref

val show_env : env -> string

val pp_env : Format.formatter -> env -> unit

val map_tcopt : env -> f:(Typechecker_options.t -> Typechecker_options.t) -> env

val get_deps_mode : env -> Typing_deps_mode.t

val get_ctx : env -> Provider_context.t

val get_tracing_info : env -> Decl_counters.tracing_info option

(** Functions related to type variable scopes *)

(**
   Open a new "type variable" scope and record this in the environment.
   Within this scope, you can
   - generate fresh type variables, using [fresh_type_X] functions
   - query the currently fresh type variables, using [get_current_tyvars]
   - close the scope

   The usual usage pattern within type inference is:

   1. Open a type variable scope using [open_tyvars] before checking an AST node.

   2. Check the AST node; generate fresh type variables as necessary; make calls
     to subtyping functions such as [Typing_ops.sub_type] that will record constraints
     on those type variables.

   3. Call [set_tyvar_variance] on the type of the expression to correctly set the
     variance of the type variables.

   4. Call [Typing_solver.close_tyvars_and_solve] to solve type variables that can
     be solved immediately, and close the type variable scope.
*)
val open_tyvars : env -> Pos.t -> env

(** Propagate the position for better error reporting.
  * Returns a `restore_pos` function.
  * Call the `restore_pos` function at the end
  * of the scope in cases where you're not throwing
  * the `env` away.
  *)
val set_inference_env_pos : env -> Pos.t option -> env * (env -> env)

(** Generate a fresh type variable type with variance not yet recorded *)
val fresh_type : env -> Pos.t -> env * locl_ty

(** Generate a fresh type variable type with optional variance and
    the given reason information *)
val fresh_type_reason :
  ?variance:Ast_defs.variance ->
  env ->
  Pos.t ->
  (Tvid.t -> Reason.t) ->
  env * locl_ty

(** Generate a fresh type variable type that is assumed to be invariant, so
    it won't be solved automatically at the end of the scope *)
val fresh_type_invariant : env -> Pos.t -> env * locl_ty

(** Generate a fresh type variable type with a specified ranks for use
    in subtyping of rank-n polymorphic types *)
val fresh_type_invariant_with_rank : env -> int -> Pos.t -> env * locl_ty

(** Generate a fresh type variable to stand for an unknown type in the
    case of type errors. *)
val fresh_type_error : env -> Pos.t -> env * locl_ty

val fresh_type_error_contravariant : env -> Pos.t -> env * locl_ty

(** What type variables are fresh in the current scope? *)
val get_current_tyvars : env -> Tvid.t list

(** Close the current type variable scope.
    You might want to call [Typing_solver.close_tyvars_and_solve] instead. *)
val close_tyvars : env -> env

(** [add env v ty] addd the solution [ty] of a type variable [v] to the environment.
    In other words, after calling [add], type variable [v] will
    be associated to type [ty], and [get_type env r v] will return [(env, ty)]. *)
val add : env -> ?tyvar_pos:Pos.t -> Tvid.t -> locl_ty -> env

val make_tyvar_no_more_occur_in_tyvar :
  env -> Tvid.t -> no_more_in:Tvid.t -> env

val tyvar_is_solved : env -> Tvid.t -> bool

val wrap_ty_in_var :
  env -> Reason.t -> locl_ty -> Typing_env_types.env * locl_ty

val get_type : env -> Reason.t -> Tvid.t -> env * locl_ty

val expand_var : env -> Reason.t -> Tvid.t -> env * locl_ty

(** If the provided type is a type variable, expand it. *)
val expand_type : env -> locl_ty -> env * locl_ty

val expand_internal_type : env -> internal_type -> env * internal_type

val should_expand_type_alias :
  env ->
  visibility_behavior:visibility_behavior ->
  name:string ->
  typedef_type ->
  bool

val is_typedef_visible : env -> name:string -> typedef_type -> bool

val get_enum_constraint : env -> type_key -> decl_ty option Decl_entry.t

val env_with_method_droot_member : env -> string -> static:bool -> env

val env_with_constructor_droot_member : env -> env

val add_parent_dep : env -> skip_constructor_dep:bool -> string -> unit

val suggest_static_member :
  bool -> class_decl -> string -> (Pos_or_decl.t * string) option

val suggest_member :
  bool -> class_decl -> string -> (Pos_or_decl.t * string) option

val get_return : env -> Typing_env_return_info.t

val set_return : env -> Typing_env_return_info.t -> env

val get_readonly : env -> bool

val set_readonly : env -> bool -> env

val get_params : env -> (locl_ty * Pos.t * locl_ty option) Local_id.Map.t

val set_param : env -> Local_id.t -> locl_ty * Pos.t * locl_ty option -> env

val set_log_level : env -> string -> int -> env

val get_log_level : env -> string -> int

val log_env_change_ : string -> ?level:int -> env -> env * 'res -> env * 'res

val log_env_change : string -> ?level:int -> env -> env -> env

val clear_params : env -> env

val with_env : env -> (env -> env * 'a) -> env * 'a

val with_origin : env -> Decl_counters.origin -> (env -> env * 'a) -> env * 'a

val with_origin2 :
  env -> Decl_counters.origin -> (env -> env * 'a * 'b) -> env * 'a * 'b

val with_inside_expr_tree :
  env -> Aast_defs.class_name -> (env -> env * 'a * 'b) -> env * 'a * 'b

val with_outside_expr_tree :
  env ->
  macro_variables:Aast_defs.lid list option ->
  (env -> Aast.class_name option -> env * 'a * 'b) ->
  env * 'a * 'b * (Pos.t * locl_ty) Local_id.Map.t option

val outside_expr_tree : env -> macro_variables:Aast_defs.lid list option -> env

val is_static : env -> bool

val get_val_kind : env -> Typing_defs.val_kind

val get_self_class_type : env -> (pos_id * exact * locl_ty list) option

val get_parent_ty : env -> decl_ty option

val get_fn_kind : env -> Ast_defs.fun_kind

val get_file : env -> Relative_path.t

val get_current_decl_and_file : env -> Pos_or_decl.ctx

(** Check that the position is in the current decl and if it is, resolve
    it with the current file. *)
val fill_in_pos_filename_if_in_current_decl :
  env -> Pos_or_decl.t -> Pos.t option

(** This will check that the first position of the given reasons is in the
    current decl and if yes use it as primary error position. If no,
    it will error at a default position in the current file and log the failed
    assertion.
    This also sets the error code to the code for unification error
    if none is provided. *)
val unify_error_assert_primary_pos_in_current_decl :
  env -> Typing_error.Reasons_callback.t

(** This will check that the first position of the given reasons is in the
    current decl and if yes use it as primary error position. If no,
    it will error at a default position in the current file and log the failed
    assertion.
    This also sets the error code to the code for invalid type hint error
    if none is provided. *)
val invalid_type_hint_assert_primary_pos_in_current_decl :
  env -> Typing_error.Reasons_callback.t

val set_fn_kind : env -> Ast_defs.fun_kind -> env

val set_current_module : env -> Ast_defs.id option -> env

val set_internal : env -> bool -> env

val set_support_dynamic_type : env -> bool -> env

val set_everything_sdt : env -> bool -> env

val set_no_auto_likes : env -> bool -> env

val set_needs_concrete : env -> bool -> env

val get_module : env -> module_key -> module_decl option

val get_current_module : env -> string option

val get_current_package : env -> Package.t option

val get_current_package_def_pos : env -> Pos.t option

val get_current_package_membership : env -> Aast_defs.package_membership option

val set_current_package_membership :
  env -> Aast_defs.package_membership option -> env

val get_soft_package_requirement : env -> Ast_defs.id option

val set_soft_package_from_attr : env -> Nast.user_attribute list -> env

(** Register the current top-level structure as being dependent on the current
    module *)
val make_depend_on_current_module : Typing_env_types.env -> unit

val get_internal : env -> bool

val get_support_dynamic_type : env -> bool

val get_no_auto_likes : env -> bool

(** Whether `static` bound to exactly this class satisfies `__NeedsConcrete`:
  the class is non-abstract, or final without an inherited
  `__ConsistentConstruct`. *)
val is_concrete_class : env -> class_decl -> bool

(** when true, the receiver of `static::foo()` is a concrete class
 in the sense of [is_concrete_class] *)
val static_points_to_concrete_class : env -> bool

val set_self : env -> string -> locl_ty -> env

(** Run a given function with self unset in the environment.
    Restore self after the function finishes executing. This is used when
    checking attributes applied to a class, because the self keyword is not
    allowed as part of an argument to an attribute *)
val run_with_no_self : env -> (env -> env * 'a) -> env * 'a

val set_parent : env -> string -> decl_ty -> env

val set_static : env -> env

val set_val_kind : env -> Typing_defs.val_kind -> env

val set_mode : env -> File_info.mode -> env

val get_mode : env -> File_info.mode

val is_strict : env -> bool

val is_hhi : env -> bool

val forget_members : env -> Reason.blame -> env

val forget_prefixed_members : env -> Local_id.t -> Reason.blame -> env

val forget_suffixed_members : env -> string -> Reason.blame -> env

val forget_static_named_members : env -> string -> Reason.blame -> env

val get_fake_members : env -> Typing_fake_members.t

module FakeMembers : sig
  val update_fake_members : env -> Typing_fake_members.t -> env

  val is_valid : env -> Nast.expr -> string -> bool

  val is_valid_static : env -> Nast.class_id_ -> string -> bool

  val check_static_invalid :
    env -> Nast.class_id_ -> string -> locl_ty -> env * locl_ty

  val check_instance_invalid :
    env -> Nast.expr -> string -> locl_ty -> env * locl_ty

  val make : env -> Nast.expr -> string -> Pos.t -> env * Local_id.t

  val make_static : env -> Nast.class_id_ -> string -> Pos.t -> env * Local_id.t
end

val tany : env -> locl_phase ty_

val next_cont_opt : env -> Typing_per_cont_env.per_cont_entry option

val all_continuations : env -> Typing_continuations.t list

val set_local :
  ?immutable:bool ->
  ?macro_splice_vars:(Pos.t * Typing_defs.locl_ty) Local_id.Map.t ->
  env ->
  Local_id.t ->
  locl_ty ->
  Pos.t ->
  env

val set_local_ : env -> Local_id.t -> Typing_local_types.local -> env

val is_using_var : env -> Local_id.t -> bool

val set_using_var : env -> Local_id.t -> env

val unset_local : env -> Local_id.t -> env

val get_local : env -> Local_id.t -> Typing_local_types.local

val get_locals :
  ?quiet:bool -> env -> 'a Aast.capture_lid list -> Typing_local_types.t

val set_locals : env -> Typing_local_types.t -> env

val set_fake_members : env -> Typing_fake_members.t -> env

val set_loaded_packages : env -> Typing_local_packages.t -> env

val is_local_defined : env -> Local_id.t -> bool

val get_local_check_defined : env -> Aast.lid -> Typing_local_types.local

val set_local_expr_id :
  env -> Local_id.t -> Expression_id.t -> (env, env * Typing_error.t) result

val get_local_expr_id : env -> Local_id.t -> Expression_id.t option

val get_tpenv : env -> TPEnv.t

val get_global_tpenv : env -> TPEnv.t

val get_pos_and_kind_of_generic :
  env -> string -> (Pos_or_decl.t * Typing_kinding_defs.kind) option

val get_lower_bounds : env -> string -> TPEnv.tparam_bounds

val get_upper_bounds : env -> string -> TPEnv.tparam_bounds

val get_equal_bounds : env -> string -> TPEnv.tparam_bounds

val get_reified : env -> string -> Aast.reify_kind

val get_enforceable : env -> string -> bool

val get_newable : env -> string -> bool

val get_require_dynamic : env -> string -> bool

val add_upper_bound :
  ?intersect:(locl_ty -> locl_ty list -> locl_ty list) ->
  env ->
  string ->
  locl_ty ->
  env

val add_lower_bound :
  ?union:(locl_ty -> locl_ty list -> locl_ty list) ->
  env ->
  string ->
  locl_ty ->
  env

val get_tparams : env -> locl_ty -> S_set.t

val add_lower_bound_global : env -> string -> locl_ty -> env

val add_upper_bound_global : env -> string -> locl_ty -> env

val env_with_tpenv : env -> TPEnv.t -> env

val env_with_global_tpenv : env -> TPEnv.t -> env

val add_generic_parameters : env -> decl_tparam list -> env

val add_generic_parameters_with_bounds : env -> locl_tparam list -> env

val unbind_generic_parameters : env -> _ tparam list -> env

val get_generic_parameters : env -> string list

val is_generic_parameter : env -> string -> bool

val get_tyvar_pos : env -> Tvid.t -> Pos.t

(* Get or add to bounds on type variables *)
val get_tyvar_lower_bounds : env -> Tvid.t -> Internal_type_set.t

val get_tyvar_upper_bounds : env -> Tvid.t -> Internal_type_set.t

val is_shadow_tyvar : env -> Tvid.t -> bool

val fresh_shadow_tyvar : env -> Pos.t -> env * Tvid.t

val set_tyvar_lower_bounds : env -> Tvid.t -> Internal_type_set.t -> env

val set_tyvar_upper_bounds : env -> Tvid.t -> Internal_type_set.t -> env

(* Optionally supply intersection or union operations to simplify the bounds *)
val add_tyvar_upper_bound :
  ?intersect:(internal_type -> internal_type list -> internal_type list) ->
  env ->
  Tvid.t ->
  internal_type ->
  env

val add_tyvar_upper_bound_and_update_variances :
  ?intersect:(internal_type -> internal_type list -> internal_type list) ->
  env ->
  Tvid.t ->
  internal_type ->
  env

val add_tyvar_lower_bound :
  ?union:(internal_type -> internal_type list -> internal_type list) ->
  env ->
  Tvid.t ->
  internal_type ->
  env

val add_tyvar_lower_bound_and_update_variances :
  ?union:(internal_type -> internal_type list -> internal_type list) ->
  env ->
  Tvid.t ->
  internal_type ->
  env

val remove_tyvar_upper_bound : env -> Tvid.t -> Tvid.t -> env

val remove_tyvar_lower_bound : env -> Tvid.t -> Tvid.t -> env

val set_tyvar_appears_covariantly : env -> Tvid.t -> env

val set_tyvar_appears_contravariantly : env -> Tvid.t -> env

val set_tyvar_eager_solve_fail : env -> Tvid.t -> env

val get_tyvar_appears_covariantly : env -> Tvid.t -> bool

val get_tyvar_appears_contravariantly : env -> Tvid.t -> bool

val get_tyvar_appears_invariantly : env -> Tvid.t -> bool

val get_tyvar_eager_solve_fail : env -> Tvid.t -> bool

val get_tyvar_type_const : env -> Tvid.t -> pos_id -> (pos_id * locl_ty) option

val set_tyvar_type_const : env -> Tvid.t -> pos_id -> locl_ty -> env

val get_tyvar_type_consts : env -> Tvid.t -> (pos_id * locl_ty) S_map.t

val get_all_tyvars : env -> Tvid.t list

val fresh_param_name : env -> string -> env * string

val add_fresh_generic_parameter_by_kind :
  env -> Pos_or_decl.t -> string -> Typing_kinding_defs.kind -> env * string

val add_fresh_generic_parameter :
  env ->
  Pos_or_decl.t ->
  string ->
  reified:Aast.reify_kind ->
  enforceable:bool ->
  newable:bool ->
  env * string

val is_fresh_generic_parameter : string -> bool

val get_tpenv_size : env -> int

val get_tpenv_tparams : env -> S_set.t

val set_env_callable_pos : env -> Pos.t -> env

val set_env_function_pos : env -> Pos.t -> env

val fun_is_constructor : env -> bool

val set_fun_is_constructor : env -> bool -> env

val set_fun_tast_info : env -> Tast.fun_tast_info -> env

val env_with_locals : env -> Typing_per_cont_env.t -> env

val reinitialize_locals : env -> env

val closure : env -> (env -> env * 'a) -> env * 'a

val in_try : env -> (env -> env * 'a) -> env * 'a

val save : TPEnv.t -> env -> Tast.saved_env

val add_subtype_prop : env -> Typing_logic.subtype_prop -> env

(** Compute the type variables appearing covariantly (positively) resp.
    contravariantly (negatively) in a given type ty.
    Return a (possibly updated) environment, the set of type variables appearing
    in positive positions and the set of type variables appearing in negative
    positions. Note that type variables may appear both positively and
    negatively in a given type so may appear in both sets. *)
val get_tyvars : env -> Typing_defs.locl_ty -> env * Tvid.Set.t * Tvid.Set.t

val set_tyvar_variance_i :
  env ->
  ?flip:bool ->
  ?for_all_vars:bool ->
  Typing_defs_constraints.internal_type ->
  env

val set_tyvar_variance :
  env -> ?flip:bool -> ?for_all_vars:bool -> Typing_defs.locl_ty -> env

val update_variance_after_bind : env -> Tvid.t -> Typing_defs.locl_ty -> env

val is_consistent : env -> bool

val mark_inconsistent : env -> env

val get_package_by_name : env -> string -> Package.t option

(** Add the packages in a RequirePackage attribute to the packages assumed
  * to be loaded *)
val assert_packages_loaded_from_attr :
  env -> ('a, 'b) Aast.user_attributes -> env

(** True if package can be assumed to be loaded *)
val is_package_loaded : env -> string -> bool

(** Get list of all packages assumed to be loaded in the current environment,
    along with the position where they were loaded (RequirePackage or if package) *)
val get_loaded_packages_list : env -> Package.pos_id list

(** For error messages.  Computes the list of included packages from both
    the current package and any loaded packages. *)
val get_included_packages : env -> (string * string * Pos.t) list

val package_allow_classconst_violations : env -> bool

val package_allow_all_tconst_violations : env -> bool

val package_allow_enforceable_enum_violations : env -> bool

(** Remove solved variable from environment by replacing it by its binding. *)
val remove_var :
  env ->
  Tvid.t ->
  search_in_upper_bounds_of:Tvid.Set.t ->
  search_in_lower_bounds_of:Tvid.Set.t ->
  env

module Log : sig
  (** Convert a type variable from an environment into json *)
  val tyvar_to_json :
    (locl_ty -> string) ->
    (internal_type -> string) ->
    env ->
    Tvid.t ->
    Yojson.Safe.t

  val expand_env : Typing_env_types.env -> expand_env -> (string * string) list
end

val make_expression_id : env -> Expression_id.t

val update_reason :
  env ->
  locl_ty ->
  f:(locl_phase Typing_reason.t_ -> locl_phase Typing_reason.t_) ->
  locl_ty

val update_cty_reason :
  env ->
  constraint_type ->
  f:(locl_phase Typing_reason.t_ -> locl_phase Typing_reason.t_) ->
  constraint_type

val update_ity_reason :
  env ->
  internal_type ->
  f:(locl_phase Typing_reason.t_ -> locl_phase Typing_reason.t_) ->
  internal_type

val rank_of_tvar : env -> Tvid.t -> int

val rank_of_tparam : env -> string -> int
