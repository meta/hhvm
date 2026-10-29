(*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)
open Hh_prelude

type sub_type =
  Typing_env_types.env ->
  ?is_dynamic_aware:bool ->
  ?is_coeffect:bool ->
  ?ignore_readonly:bool ->
  ?class_sub_classname:bool ->
  Typing_defs.locl_ty ->
  Typing_defs.locl_ty ->
  Typing_error.Reasons_callback.t option ->
  Typing_env_types.env * Typing_error.t option

val sub_type_ref : sub_type ref

val sub_type : sub_type

type expected_subtyping_result =
  | Subtyping_result of Typing_env_types.env * Typing_error.t option
      (** The ordinary result of asserting the subtype relation. *)
  | Ambiguous_shape_splat of Tvid.Set.t
      (** The normalized subtype contains multiple unresolved subtype-side
          shape spreads whose contributions cannot be constrained independently. *)

type sub_type_against_expected_type =
  Typing_env_types.env ->
  ?is_dynamic_aware:bool ->
  ?ignore_readonly:bool ->
  Typing_defs.locl_ty ->
  Typing_defs.locl_ty ->
  Typing_error.Reasons_callback.t option ->
  expected_subtyping_result

val sub_type_against_expected_type_ref : sub_type_against_expected_type ref

val sub_type_against_expected_type : sub_type_against_expected_type

type sub_type_i =
  Typing_env_types.env ->
  ?is_coeffect:bool ->
  ?class_sub_classname:bool ->
  Typing_defs_constraints.internal_type ->
  Typing_defs_constraints.internal_type ->
  Typing_error.Reasons_callback.t option ->
  Typing_env_types.env * Typing_error.t option

val sub_type_i_ref : sub_type_i ref

val sub_type_i : sub_type_i

type is_sub_type_type =
  Typing_env_types.env -> Typing_defs.locl_ty -> Typing_defs.locl_ty -> bool

type is_sub_type_opt_type =
  Typing_env_types.env ->
  Typing_defs.locl_ty ->
  Typing_defs.locl_ty ->
  bool option

val is_sub_type_ref : is_sub_type_type ref

val is_sub_type :
  Typing_env_types.env -> Typing_defs.locl_ty -> Typing_defs.locl_ty -> bool

val is_sub_type_for_union_ref :
  (Typing_env_types.env -> Typing_defs.locl_ty -> Typing_defs.locl_ty -> bool)
  ref

val is_sub_type_for_union_i_ref :
  (Typing_env_types.env ->
  Typing_defs_constraints.internal_type ->
  Typing_defs_constraints.internal_type ->
  bool)
  ref

val is_sub_type_for_union :
  Typing_env_types.env -> Typing_defs.locl_ty -> Typing_defs.locl_ty -> bool

val is_sub_type_for_union_i :
  Typing_env_types.env ->
  Typing_defs_constraints.internal_type ->
  Typing_defs_constraints.internal_type ->
  bool

val is_sub_type_ignore_generic_params_ref : is_sub_type_type ref

val is_sub_type_ignore_generic_params :
  Typing_env_types.env -> Typing_defs.locl_ty -> Typing_defs.locl_ty -> bool

val is_sub_type_opt_ignore_generic_params_ref : is_sub_type_opt_type ref

val is_sub_type_opt_ignore_generic_params :
  Typing_env_types.env ->
  Typing_defs.locl_ty ->
  Typing_defs.locl_ty ->
  bool option

val can_sub_type_ref : is_sub_type_type ref

val can_sub_type :
  Typing_env_types.env -> Typing_defs.locl_ty -> Typing_defs.locl_ty -> bool

val unwrap_class_type :
  Typing_defs.decl_phase Typing_defs.ty ->
  Typing_defs.decl_phase Typing_defs.Reason.t_
  * Typing_defs.pos_id
  * Typing_defs.decl_ty list

val try_unwrap_class_type :
  Typing_defs.decl_phase Typing_defs.ty ->
  (Typing_defs.decl_phase Typing_defs.Reason.t_
  * Typing_defs.pos_id
  * Typing_defs.decl_ty list)
  option

val class_is_final_and_invariant : Folded_class.t -> bool

type localize_no_subst =
  Typing_env_types.env ->
  ignore_errors:bool ->
  Typing_defs.decl_ty ->
  (Typing_env_types.env * Typing_error.t option) * Typing_defs.locl_ty

val localize_no_subst_ref : localize_no_subst ref

val localize_no_subst :
  Typing_env_types.env ->
  ignore_errors:bool ->
  Typing_defs.decl_ty ->
  (Typing_env_types.env * Typing_error.t option) * Typing_defs.locl_ty

type localize =
  ety_env:Typing_defs.expand_env ->
  Typing_env_types.env ->
  Typing_defs.decl_ty ->
  (Typing_env_types.env * Typing_error.t option) * Typing_defs.locl_ty

val localize_ref : localize ref

val localize : localize

val localize_disjoint_union_ref : localize ref

val localize_disjoint_union : localize

val is_class_i : Typing_defs_constraints.internal_type -> bool

val is_class : Typing_defs.locl_ty -> bool

val is_mixed_i :
  Typing_env_types.env -> Typing_defs_constraints.internal_type -> bool

val is_mixed : Typing_env_types.env -> Typing_defs.locl_ty -> bool

val is_nothing_i :
  Typing_env_types.env -> Typing_defs_constraints.internal_type -> bool

val is_nothing : Typing_env_types.env -> Typing_defs.locl_ty -> bool

val is_null : Typing_env_types.env -> Typing_defs.locl_ty -> bool

val run_on_intersection :
  'env ->
  f:('env -> Typing_defs.locl_ty -> 'env * 'a) ->
  Typing_defs.locl_ty list ->
  'env * 'a list

val run_on_intersection_with_ty_err :
  'env ->
  f:('env -> Typing_defs.locl_ty -> ('env * Typing_error.t option) * 'a) ->
  Typing_defs.locl_ty list ->
  ('env * Typing_error.t option) * 'a list

val run_on_intersection_res :
  'a ->
  f:('a -> Typing_defs.locl_ty -> 'a * 'b * 'c) ->
  Typing_defs.locl_ty list ->
  'a * 'b list * 'c list

val run_on_intersection_key_value_res :
  'a ->
  f:('a -> Typing_defs.locl_ty -> 'a * 'b * 'c * 'd) ->
  Typing_defs.locl_ty list ->
  'a * 'b list * 'c list * 'd list

val run_on_intersection_array_key_value_res :
  'a ->
  f:('a -> Typing_defs.locl_ty -> 'a * 'b * 'c * 'd * 'e) ->
  Typing_defs.locl_ty list ->
  'a * 'b list * 'c list * 'd list * 'e list

val is_dynamic : Typing_env_types.env -> Typing_defs.locl_ty -> bool

val is_any : Typing_env_types.env -> Typing_defs.locl_ty -> bool

val is_any_and_not_nothing : Typing_env_types.env -> Typing_defs.locl_ty -> bool

val is_tunion : Typing_env_types.env -> Typing_defs.locl_ty -> bool

val is_tintersection : Typing_env_types.env -> Typing_defs.locl_ty -> bool

val is_tyvar : Typing_env_types.env -> Typing_defs.locl_ty -> bool

val is_opt_tyvar : Typing_env_types.env -> Typing_defs.locl_ty -> bool

val is_tyvar_error : Typing_env_types.env -> Typing_defs.locl_ty -> bool

val contains_unresolved_tyvars :
  Typing_env_types.env -> Typing_defs.locl_ty -> Typing_env_types.env * bool

val get_base_type :
  ?expand_supportdyn:bool ->
  Typing_env_types.env ->
  Typing_defs.locl_ty ->
  Typing_env_types.env * Typing_defs.locl_ty

val get_printable_shape_field_name : Typing_defs.tshape_field_name -> string

type add_constraint =
  Typing_env_types.env ->
  Ast_defs.constraint_kind ->
  Typing_defs.locl_ty ->
  Typing_defs.locl_ty ->
  Typing_error.Reasons_callback.t option ->
  Typing_env_types.env

val add_constraint_ref : add_constraint ref

val add_constraint : add_constraint

type expand_typeconst =
  Typing_defs.expand_env ->
  Typing_env_types.env ->
  ?ignore_errors:bool ->
  ?as_tyvar_with_cnstr:Pos.t option ->
  Typing_defs.locl_ty ->
  Typing_defs.pos_id ->
  allow_abstract_tconst:bool ->
  (Typing_env_types.env
  * Typing_error.t option
  * Type_expansions.cycle_reporter list)
  * Typing_defs.locl_ty

val expand_typeconst_ref : expand_typeconst ref

(** Expands a type constant access like A::T to its definition. *)
val expand_typeconst : expand_typeconst

type union =
  Typing_env_types.env ->
  ?reason:Typing_reason.t ->
  ?approx_cancel_neg:bool ->
  Typing_defs.locl_ty ->
  Typing_defs.locl_ty ->
  Typing_env_types.env * Typing_defs.locl_ty

val union_ref : union ref

val union :
  Typing_env_types.env ->
  ?reason:Typing_reason.t ->
  ?approx_cancel_neg:bool ->
  Typing_defs.locl_ty ->
  Typing_defs.locl_ty ->
  Typing_env_types.env * Typing_defs.locl_ty

type make_union =
  Typing_env_types.env ->
  Typing_reason.t ->
  Typing_defs.locl_ty list ->
  Typing_reason.t option ->
  Typing_reason.t option ->
  Typing_env_types.env * Typing_defs.locl_ty

val make_union_ref : make_union ref

val make_union :
  Typing_env_types.env ->
  Typing_reason.t ->
  Typing_defs.locl_ty list ->
  Typing_reason.t option ->
  Typing_reason.t option ->
  Typing_env_types.env * Typing_defs.locl_ty

type normalize_shape_type_result =
  | Normalized_shape of Typing_defs.locl_phase Typing_defs.shape_type
  | Normalized_bottom
  | Normalized_distributed

type normalize_shape_type =
  Typing_env_types.env ->
  Typing_reason.t ->
  Typing_defs.locl_phase Typing_defs.shape_type ->
  Typing_env_types.env * normalize_shape_type_result

(** Normalize a shape type (rightmost-wins merge of its splat elements).
    [Normalized_distributed] means union or intersection distribution did not
    produce a single shape, so callers must handle the input conservatively.
    Implemented in [Typing_shape_normalize] and exposed here via a ref to break
    the module cycle ([Typing_shape_normalize] depends transitively on
    [Typing_refinement]). *)
val normalize_shape_type_ref : normalize_shape_type ref

val normalize_shape_type : normalize_shape_type

type union_list =
  Typing_env_types.env ->
  ?approx_cancel_neg:bool ->
  Typing_reason.t ->
  Typing_defs.locl_ty list ->
  Typing_env_types.env * Typing_defs.locl_ty

val union_list_ref : union_list ref

val union_list :
  Typing_env_types.env ->
  ?approx_cancel_neg:bool ->
  Typing_reason.t ->
  Typing_defs.locl_ty list ->
  Typing_env_types.env * Typing_defs.locl_ty

type fold_union =
  Typing_env_types.env ->
  ?approx_cancel_neg:bool ->
  Typing_reason.t ->
  Typing_defs.locl_ty list ->
  Typing_env_types.env * Typing_defs.locl_ty

val fold_union_ref : fold_union ref

type simplify_unions =
  Typing_env_types.env ->
  ?approx_cancel_neg:bool ->
  ?on_tyvar:
    (Typing_env_types.env ->
    Typing_reason.t ->
    Tvid.t ->
    Typing_env_types.env * Typing_defs.locl_ty) ->
  Typing_defs.locl_ty ->
  Typing_env_types.env * Typing_defs.locl_ty

val simplify_unions_ref : simplify_unions ref

type intersect_list =
  Typing_env_types.env ->
  Typing_reason.t ->
  Typing_defs.locl_ty list ->
  Typing_env_types.env * Typing_defs.locl_ty

val intersect_list_ref : intersect_list ref

val intersect_list : intersect_list

val contains_tvar_decl : Typing_defs.decl_ty -> bool

val contains_generic_decl : Typing_defs.decl_ty -> Pos_or_decl.t option

(** [wrap_union_inter_ty_in_var env r ty] wraps [ty] in a type variable
  if [ty] is a union, intersection or option containing an unsolved type variable.
  Wrapping in a type variable means a new type variable is created and mapped to [ty].
  This is a way to mark [ty] as needing simplification when the unsolved type variable
  later gets solved. *)
val wrap_union_inter_ty_in_var :
  Typing_env_types.env ->
  Typing_defs.Reason.t ->
  Typing_defs.locl_ty ->
  Typing_env_types.env * Typing_defs.locl_ty

module Concrete_supertypes_cache : sig
  type t

  val create : unit -> t
end

val get_concrete_supertypes :
  ?cache:Concrete_supertypes_cache.t ->
  ?expand_supportdyn:bool ->
  ?include_case_types:bool ->
  abstract_enum:bool ->
  Typing_env_types.env ->
  Typing_defs.locl_ty ->
  Typing_env_types.env * Typing_set.elt list

(** The concrete types known to be BELOW the given type: the dual of
    [get_concrete_supertypes]. Use this, not that one, when reading a lower
    bound -- supertypes can only bound from above. *)
val get_concrete_subtypes :
  Typing_env_types.env ->
  Typing_defs.locl_ty ->
  Typing_env_types.env * Typing_set.elt list

(** Over-approximate a shape splat as a flattened list of concrete shape elements
    (each a [Shape_simple] or [nothing]) by resolving each splat-position type
    parameter to its concrete shape supertypes, transitively. Returns [None] if
    some element cannot be reduced to a shape. *)
val get_shape_splat_concrete_supertypes :
  Typing_env_types.env ->
  Typing_defs.locl_ty ->
  Typing_env_types.env * Typing_defs.locl_ty list option

val simplify_unions :
  Typing_env_types.env ->
  ?approx_cancel_neg:bool ->
  ?on_tyvar:
    (Typing_env_types.env ->
    Typing_reason.t ->
    Tvid.t ->
    Typing_env_types.env * Typing_defs.locl_ty) ->
  Typing_defs.locl_ty ->
  Typing_env_types.env * Typing_defs.locl_ty

type approx =
  | ApproxUp
  | ApproxDown
[@@deriving eq]

type negate_type =
  Typing_env_types.env ->
  Typing_reason.t ->
  Typing_defs.locl_ty ->
  approx:approx ->
  Typing_env_types.env * Typing_defs.locl_ty

val negate_type_ref : negate_type ref

val negate_type :
  Typing_env_types.env ->
  Typing_reason.t ->
  Typing_defs.locl_ty ->
  approx:approx ->
  Typing_env_types.env * Typing_defs.locl_ty

val simplify_intersections :
  Typing_env_types.env ->
  ?on_tyvar:
    (Typing_env_types.env ->
    Typing_reason.t ->
    Tvid.t ->
    Typing_env_types.env * Typing_defs.locl_ty) ->
  Typing_defs.locl_ty ->
  Typing_env_types.env * Typing_defs.locl_ty

type simplify_intersections =
  Typing_env_types.env ->
  ?on_tyvar:
    (Typing_env_types.env ->
    Typing_reason.t ->
    Tvid.t ->
    Typing_env_types.env * Typing_defs.locl_ty) ->
  Typing_defs.locl_ty ->
  Typing_env_types.env * Typing_defs.locl_ty

val simplify_intersections_ref : simplify_intersections ref

val is_type_disjoint :
  Typing_env_types.env -> Typing_defs.locl_ty -> Typing_defs.locl_ty -> bool

val is_type_disjoint_ref :
  (Typing_env_types.env -> Typing_defs.locl_ty -> Typing_defs.locl_ty -> bool)
  ref

val collect_enum_class_upper_bounds :
  Typing_env_types.env ->
  string ->
  Typing_env_types.env * Typing_defs.locl_ty option

val default_fun_param :
  readonly:bool -> ?pos:Pos_or_decl.t -> 'a -> 'a Typing_defs.fun_param

val mk_tany :
  Typing_env_types.env -> Pos.t -> Typing_reason.locl_phase Typing_defs.ty

val make_locl_subst_for_class_tparams :
  Folded_class.t -> Typing_defs.locl_ty list -> Typing_defs.locl_ty S_map.t

val is_sub_class_refl : Typing_env_types.env -> string -> string -> bool

val class_has_no_params : Typing_env_types.env -> string -> bool

val has_ancestor_including_req_refl :
  Typing_env_types.env -> string -> string -> bool

val has_ancestor_including_req :
  Typing_env_types.env -> Folded_class.t -> string -> bool

val is_tests_bypass_visibility_context : Typing_env_types.env -> bool

(* Wrap supportdyn<_> around a type. Push through intersections
 * and unions, and leave type alone if it is a subtype of dynamic.
 *)
val make_supportdyn :
  Typing_reason.t ->
  Typing_env_types.env ->
  Typing_defs.locl_ty ->
  Typing_env_types.env * Typing_defs.locl_ty

val simple_make_supportdyn :
  Typing_reason.t ->
  Typing_env_types.env ->
  Typing_defs.locl_ty ->
  Typing_env_types.env * Typing_defs.locl_ty

val make_supportdyn_decl_type :
  Pos_or_decl.t ->
  Typing_defs.Reason.decl_t ->
  Typing_defs.decl_ty ->
  Typing_defs.decl_ty

(* Wrap ~ around a type, unless it is already a dynamic or a like type *)
val make_like :
  ?reason:Typing_reason.t ->
  Typing_env_types.env ->
  Typing_defs.locl_ty ->
  Typing_defs.locl_ty

(* Wrap ~ around a type if it is enforced. This is used for returns, property
 * assignment, and default expression typing
 *)
val make_like_if_enforced :
  Typing_env_types.env ->
  Typing_defs.enforcement ->
  Typing_defs.locl_ty ->
  Typing_defs.locl_ty

val is_capability : Typing_defs.locl_ty -> bool

val is_capability_i : Typing_defs_constraints.internal_type -> bool

val supports_dynamic :
  Typing_env_types.env ->
  Typing_defs.locl_ty ->
  Typing_error.Reasons_callback.t option ->
  Typing_env_types.env * Typing_error.t option

(* Return true if type definitely is a subtype of supportdyn<mixed> *)
val is_supportdyn : Typing_env_types.env -> Typing_defs.locl_ty -> bool

(* Return true if type definitely is a subtype of supportdyn<mixed>.
 * Use type variable bounds (unlike subtype algorithm) e.g. if
 * #0 <: supportdyn<mixed> then is_supportdyn_use_tyvar_bounds(#0) = true
 *)
val is_supportdyn_use_tyvar_bounds :
  Typing_env_types.env -> Typing_defs.locl_ty -> bool

val strip_supportdyn :
  Typing_env_types.env ->
  Typing_defs.locl_ty ->
  bool * Typing_env_types.env * Typing_defs.locl_ty

val get_underlying_function_type :
  Typing_env_types.env ->
  Typing_defs.locl_ty ->
  Typing_env_types.env
  * (Typing_reason.t * Typing_defs.locl_ty Typing_defs.fun_type) option

val map_supportdyn :
  Typing_env_types.env ->
  Typing_defs.locl_ty ->
  (Typing_env_types.env ->
  Typing_defs.locl_ty ->
  Typing_env_types.env * Typing_defs.locl_ty) ->
  Typing_env_types.env * Typing_defs.locl_ty

val no_upper_bound :
  include_sd_mixed:bool ->
  Typing_env_types.env ->
  Typing_defs.locl_ty list ->
  Typing_env_types.env * bool

val get_case_type_variants_as_type :
  Typing_defs.typedef_case_type_variant ->
  Typing_defs.typedef_case_type_variant list ->
  Typing_defs.decl_ty

val get_newtype_sub_opt :
  Typing_env_types.env ->
  string ->
  Typing_defs.locl_ty list ->
  Typing_env_types.env * Typing_defs.locl_ty option

val get_newtype_super :
  Typing_env_types.env ->
  Typing_reason.t ->
  string ->
  Typing_defs.locl_ty list ->
  Typing_env_types.env * Typing_defs.locl_ty
