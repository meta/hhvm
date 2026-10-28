(*
 * Copyrighd (c) 2015, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude
open Typing_defs_flags
include Typing_defs_core

(** Origin of Class Constant References:
    In order to be able to detect cycle definitions like
    class C {
      const int A = D::A;
    }
    class D {
      const int A = C::A;
    }
    we need to remember which constants were used during initialization.

    Currently the syntax of constants allows direct references to another class
    like D::A, or self references using self::A.

    class_const_from encodes the origin (class vs self).
 *)
type class_const_from =
  | Self
  | From of string
[@@deriving eq, show]

(** Class Constant References:
    In order to be able to detect cycle definitions like
    class C {
      const int A = D::A;
    }
    class D {
      const int A = C::A;
    }
    we need to remember which constants were used during initialization.

    Currently the syntax of constants allows direct references to another class
    like D::A, or self references using self::A.
 *)
type class_const_ref = class_const_from * string [@@deriving eq, show]

module CCR = struct
  type t = class_const_ref [@@deriving show]

  (* We're deriving the compare function by hand to sync it with the rust code.
   * In the decl parser I need to sort these class_const_from in the same
   * way. I used `cargo expand` to see how Cargo generated they Ord/PartialOrd
   * functions.
   *)
  let compare_class_const_from ccf0 ccf1 =
    match (ccf0, ccf1) with
    | (Self, Self) -> 0
    | (From lhs, From rhs) -> String.compare lhs rhs
    | (Self, From _) -> -1
    | (From _, Self) -> 1

  let compare (ccf0, s0) (ccf1, s1) =
    match compare_class_const_from ccf0 ccf1 with
    | 0 -> String.compare s0 s1
    | x -> x
end

module CCRSet = struct
  include Stdlib.Set.Make (CCR)

  type t_as_list = CCR.t list [@@deriving show]

  let pp fmt set = pp_t_as_list fmt (elements set)

  let show set = show_t_as_list (elements set)
end

type const_decl = {
  cd_pos: Pos_or_decl.t;
  cd_type: decl_ty;
  cd_value: string option;
  cd_package: Aast_defs.package_membership option;
}
[@@deriving show, eq]

type package_requirement =
  | RPRequire of pos_string
  | RPSoft of pos_string
  | RPNormal
[@@deriving eq, show]

type class_elt = {
  ce_visibility: ce_visibility;
  ce_type: decl_ty Lazy.t;
  ce_origin: string;  (** identifies the class from which this elt originates *)
  ce_deprecated: string option;
  ce_pos: Pos_or_decl.t Lazy.t;  (** pos of the type of the elt *)
  ce_flags: Typing_defs_flags.ClassElt.t;
  ce_sealed_allowlist: S_set.t option;
  ce_overlapping_tparams: S_set.t option;
  ce_package_requirement: package_requirement option;
}
[@@deriving show]

type fun_elt = {
  fe_deprecated: string option;
  fe_module: Ast_defs.id option;
  fe_package: Aast_defs.package_membership option;
  fe_internal: bool;  (** Top-level functions have limited visibilities *)
  fe_type: decl_ty;
  fe_pos: Pos_or_decl.t;
  (* TODO: ideally these should be packed flags *)
  fe_php_std_lib: bool;
  fe_support_dynamic_type: bool;
  fe_no_auto_dynamic: bool;
  fe_no_auto_likes: bool;
  fe_gated_by_feature_flag: string option;
  fe_package_requirement: package_requirement;
}
[@@deriving show, eq]

(* TODO(T71787342) This is temporary. It is necessary because legacy decl
 * uses Typing_defs types to represent folded inherited members. This will
 * be removed when shallow decl ships as this information is only necessary
 * there. *)
type class_const_kind =
  | CCAbstract of bool (* has default *)
  | CCConcrete
[@@deriving eq, show]

(** A recorded constant value, used to store enum-member values and
    class-constant values used as shape keys. The representation is abstract
    outside this module (see the .mli), so callers go through the API rather
    than matching raw constructors. *)
module Const_value = struct
  (** `CVInt` holds values that fit
      OCaml's 63-bit `int`; literals outside that range (including `i64::MIN`)
      use `CVLargeInt`, which stores the canonical decimal as a string so it
      marshals without truncation and every spelling of one value compares equal.
      `CVLabel` means the value is a string equal to the member's own name
      (`FOO = 'FOO'`), recorded without storing the string.
      `CVAbsent` represents an unrecorded or unrepresentable value inline, so
      the recorded value needs no `option` wrapper. *)
  type t =
    | CVInt of int
    | CVLargeInt of string
    | CVString of string
    | CVNameof of string
        (** Class name with legacy XHP mangling already applied. *)
    | CVLabel
    | CVAbsent
    | CVConstAccess of string * string
  [@@deriving eq, ord, show] [@@warning "-37"]
  (* Every constructor except [CVAbsent] is built only by the Rust decl parser
     and marshaled in via ocamlrep; OCaml never builds them, so silence the
     unused-constructor warning. *)

  (** The "no recorded value" case. *)
  let absent = CVAbsent

  let is_absent = function
    | CVAbsent -> true
    | _ -> false

  let is_intish s =
    match Int64.of_string_opt s with
    | Some n -> String.equal (Int64.to_string n) s
    | None -> false

  let value_repr ~member_name = function
    | CVInt n -> string_of_int n
    | CVLargeInt s -> s
    | CVString s ->
      if is_intish s then
        s
      else
        "\"" ^ s ^ "\""
    | CVLabel -> "\"" ^ member_name ^ "\""
    | CVNameof s -> "nameof " ^ s
    | CVConstAccess (cls, member) -> cls ^ "::" ^ member
    | CVAbsent -> "<absent>"

  (** Values used only for shape key normalisation. *)
  type concrete_value =
    | SKVInt of string
    | SKVString of string

  let concrete_value ~member_name (value : t) =
    match value with
    | CVInt value -> Some (SKVInt (string_of_int value))
    | CVLargeInt value -> Some (SKVInt value)
    | CVString value -> Some (SKVString value)
    | CVLabel -> Some (SKVString member_name)
    | CVNameof value -> Some (SKVString (Utils.strip_ns value))
    | CVAbsent
    | CVConstAccess _ ->
      None

  (** Whether the value cannot be resolved to a concrete, comparable form:
      absent (no recorded value), or a reference to another enum's member
      (`CVConstAccess`, resolvable only through the pure-alias exemption). *)
  let is_uncheckable = function
    | CVAbsent
    | CVConstAccess _ ->
      true
    | _ -> false

  (** For an aliasing member `X = E::A`, the accessed `(enum, member)`. *)
  let const_access_target = function
    | CVConstAccess (base, target) -> Some (base, target)
    | _ -> None
end

type class_const = {
  cc_synthesized: bool;
  cc_abstract: class_const_kind;
  cc_pos: Pos_or_decl.t;
  cc_type: decl_ty;
  cc_origin: string;
      (** identifies the class from which this const originates *)
  cc_refs: class_const_ref list;
      (** references to the constants used in the initializer *)
  cc_is_shape_key: bool;
      (** Whether the constant is explicitly or intrinsically permitted as a
          shape key. *)
  cc_value: Const_value.t;
      (** The recorded value for a `__ShapeKey` constant or enum member;
          `CVAbsent` means the value was not recorded or cannot be represented. *)
}
[@@deriving show]

type module_def_type = { mdt_pos: Pos_or_decl.t } [@@deriving show]

(** The position is that of the hint in the `use` / `implements` AST node
 * that causes a class to have this requirement applied to it. E.g.
 *
 * ```
 * class Foo {}
 *
 * interface Bar {
 *   require extends Foo; <- position of the decl_phase ty
 * }
 *
 * class Baz extends Foo implements Bar { <- position of the `implements`
 * }
 * ```
 *)
type requirement = Pos_or_decl.t * decl_ty [@@deriving eq, show]

(* Representation for `require class` and `require this as` constraints *)
type constraint_requirement =
  | CR_Equal of requirement
  | CR_Subtype of requirement
[@@deriving eq, show]

let to_requirement cr =
  match cr with
  | CR_Equal r
  | CR_Subtype r ->
    r

type abstract_typeconst = {
  atc_as_constraint: decl_ty option;
  atc_super_constraint: decl_ty option;
  atc_default: decl_ty option;
}
[@@deriving eq, show]

type concrete_typeconst = { tc_type: decl_ty } [@@deriving eq, show]

type partially_abstract_typeconst = {
  patc_constraint: decl_ty;
  patc_type: decl_ty;
}
[@@deriving show]

type typeconst =
  | TCAbstract of abstract_typeconst
  | TCConcrete of concrete_typeconst
[@@deriving eq, show]

type typeconst_type = {
  ttc_synthesized: bool;
  ttc_name: pos_id;
  ttc_kind: typeconst;
  ttc_origin: string;
  ttc_enforceable: Pos_or_decl.t * bool;
      (** If the typeconst had the <<__Enforceable>> attribute on its
          declaration, this will be [(position_of_declaration, true)].

          In legacy decl, the second element of the tuple will also be true if
          the typeconst overrides some parent typeconst which had the
          <<__Enforceable>> attribute. In that case, the position will point to
          the declaration of the parent typeconst.

          In shallow decl, this is not the case--there is no overriding behavior
          modeled here, and the second element will only be true when the
          declaration of this typeconst had the attribute.

          When the second element of the tuple is false, the position will be
          [Pos_or_decl.none].

          To manage the difference between legacy and shallow decl, use
          [Folded_class.get_typeconst_enforceability] rather than
          accessing this field directly. *)
  ttc_reifiable: Pos_or_decl.t option;
  ttc_concretized: bool;
  ttc_is_ctx: bool;
}
[@@deriving show]

type enum_type = {
  te_base: decl_ty;
  te_constraint: decl_ty option;
  te_includes: decl_ty list;
}
[@@deriving eq, show]

type typedef_case_type_variant = decl_ty * decl_where_constraint list
[@@deriving eq, show]

type typedef_type_assignment =
  | SimpleTypeDef of Ast_defs.typedef_visibility * decl_ty
  | CaseType of typedef_case_type_variant * typedef_case_type_variant list
[@@deriving eq, show]

type typedef_type = {
  td_module: Ast_defs.id option;
  td_pos: Pos_or_decl.t;
  td_tparams: decl_tparam list;
  td_as_constraint: decl_ty option;
  td_super_constraint: decl_ty option;
  td_type_assignment: typedef_type_assignment;
  td_is_ctx: bool;
  td_attributes: user_attribute list;
  td_internal: bool;
  td_docs_url: string option;
  td_package: Aast_defs.package_membership option;
}
[@@deriving eq, show]

type phase_ty =
  | DeclTy of decl_ty
  | LoclTy of locl_ty

type deserialization_error =
  | Wrong_phase of string
      (** The type was valid, but some component thereof was a decl_ty when we
          expected a locl_phase ty, or vice versa. *)
  | Not_supported of string
      (** The specific type or some component thereof is not one that we support
          deserializing, usually because not enough information was serialized to be
          able to deserialize it again. *)
  | Deserialization_error of string
      (** The input JSON was invalid for some reason. *)
[@@deriving show]

(** How should we treat the wildcard character _ when localizing?
 *  1. Generate a fresh type variable, e.g. in type argument to constructor or function,
 *     or in a lambda parameter or return type.
 *       Example: foo<shape('a' => _)>($myshape);
 *       Example: ($v : vec<_>) ==> $v[0]
 *  2. As a placeholder in a formal higher-kinded type parameter
 *       Example: function test<T1, T2<_>>() // T2 is HK and takes a type to a type
 *  3. Generate a fresh generic (aka Skolem variable), e.g. in `is` or `as` test
 *       Example: if ($x is Vector<_>) { // $x has type Vector<T#1> }
 *  4. Reject, when in a type argument to a generic parameter marked <<__Explicit>>
 *       Example: makeVec<_>(3)  where function makeVec<<<__Explicit>> T>(T $_): void
 *  5. Reject, because the type must be explicit.
 *)
type wildcard_action =
  | Wildcard_fresh_tyvar
  | Wildcard_fresh_generic
  | Wildcard_higher_kinded_placeholder
  | Wildcard_require_explicit of decl_tparam
  | Wildcard_illegal

type visibility_behavior =
  | Always_expand_newtype
  | Expand_visible_newtype_only
  | Never_expand_newtype
  | Resolve_type_structure of string option
[@@deriving show { with_path = false }]

let is_default_visibility_behaviour = function
  | Expand_visible_newtype_only -> true
  | _ -> false

let default_visibility_behaviour = Expand_visible_newtype_only

(** see .mli **)
type expand_env = {
  type_expansions: Type_expansions.t;
  make_internal_opaque: bool;
  visibility_behavior: visibility_behavior;
  substs: locl_ty S_map.t;
  no_substs: S_set.t;
  this_ty: locl_ty;
  on_error: Typing_error.Reasons_callback.t option;
  wildcard_action: wildcard_action;
  ish_weakening: bool;
  under_type_constructor: bool;
  simplify_intersections: bool;
}

let empty_expand_env =
  {
    type_expansions = Type_expansions.empty;
    visibility_behavior = default_visibility_behaviour;
    make_internal_opaque = true;
    substs = S_map.empty;
    no_substs = S_set.empty;
    this_ty = mk (Reason.none, Tgeneric Naming_special_names.Typehints.this);
    on_error = None;
    wildcard_action = Wildcard_fresh_tyvar;
    ish_weakening = false;
    under_type_constructor = false;
    simplify_intersections = true;
  }

let empty_expand_env_with_on_error on_error =
  { empty_expand_env with on_error = Some on_error }

let add_type_expansion_check_cycles ety_env exp :
    ( expand_env,
      Type_expansions.cycle * Typing_error.Reasons_callback.t option )
    result =
  Type_expansions.add_and_check_cycles ety_env.type_expansions exp
  |> Result.map ~f:(fun type_expansions -> { ety_env with type_expansions })
  |> Result.map_error ~f:(fun cycle -> (cycle, ety_env.on_error))

let cyclic_expansion env = Type_expansions.cyclic_expansion env.type_expansions

let get_class_type t =
  match get_node t with
  | Tclass (id, exact, tyl) -> Some (id, exact, tyl)
  | _ -> None

let is_tyvar t = Option.is_some (get_var t)

let is_generic t =
  match get_node t with
  | Tgeneric _ -> true
  | _ -> false

let is_dynamic t =
  match get_node t with
  | Tdynamic _ -> true
  | _ -> false

let is_nothing t =
  match get_node t with
  | Tunion [] -> true
  | _ -> false

let is_wildcard t =
  match get_node t with
  | Twildcard -> true
  | _ -> false

let is_nonnull t =
  match get_node t with
  | Tnonnull -> true
  | _ -> false

let is_fun t =
  match get_node t with
  | Tfun _ -> true
  | _ -> false

let is_any t =
  match get_node t with
  | Tany _ -> true
  | _ -> false

let is_generic_equal_to n t =
  match get_node t with
  | Tgeneric n' when String.equal n n' -> true
  | _ -> false

let is_prim p t =
  match get_node t with
  | Tprim p' when Aast.equal_tprim p p' -> true
  | _ -> false

let is_union t =
  match get_node t with
  | Tunion _ -> true
  | _ -> false

let is_neg t =
  match get_node t with
  | Tneg _ -> true
  | _ -> false

let is_union_or_inter_type (ty : locl_ty) =
  (* do not expand type here! *)
  match get_node ty with
  | Toption _
  | Tunion _
  | Tintersection _ ->
    true
  | Tnonnull
  | Tneg _
  | Tdynamic _
  | Tany _
  | Tprim _
  | Tfun _
  | Ttuple _
  | Tshape _
  | Tvar _
  | Tnewtype _
  | Tdependent _
  | Tgeneric _
  | Tclass _
  | Tvec_or_dict _
  | Tlabel _
  | Taccess _
  | Tclass_ptr _ ->
    false

module Named_params = struct
  let name_of_named_param (fp : _ fun_param) : string option =
    if Typing_defs_core.get_fp_is_named fp then
      match fp.fp_name with
      | Some raw_name ->
        let name = String.chop_prefix_if_exists ~prefix:"$" raw_name in
        Some name
      | None ->
        let () =
          (* TODO(named_params): remove runtime invariant checking,
           * perhaps by changing fp.fp_name to `string` instead of `string option`
           *)
          Hack_event_logger.invariant_violation_bug
            ~path:(Pos_or_decl.filename fp.fp_pos)
            "named param without name"
        in
        None
    else
      None

  let name_of_arg (arg : _ Aast_defs.argument) : string option =
    match arg with
    | Ainout _
    | Anormal _ ->
      None
    | Anamed ((_, name), _) -> Some name
end

(* A named-variadic parameter is the last entry of [ft.ft_params]. It
 * represents "accepts any number of named args of this element type", NOT a
 * required named parameter. *)
let ft_named_variadic_param (ft : _ fun_type) : _ fun_param option =
  if get_ft_named_variadic ft then
    List.last ft.ft_params
  else
    None

(* [ft.ft_params] with the named-variadic entry (if any) stripped off. *)
let ft_params_without_named_variadic (ft : _ fun_type) : _ fun_param list =
  match ft_named_variadic_param ft with
  | Some _ -> List.drop_last_exn ft.ft_params
  | None -> ft.ft_params

let ft_has_positional_variadic (ft : _ fun_type) : bool = get_ft_variadic ft

(* The identifier for this *)
let this = Local_id.make_scoped "$this"

(* This should be the ONLY way that Tany is constructed anywhere in the
 * codebase. *)
let make_tany () = Tany Tany_sentinel.value

(* Required parameters (number and names). Does not include optional, variadic, or
 * type-splat parameters
 *)
let arity_and_names_required ft : int * S_set.t =
  let non_splat_non_optional =
    List.filter (ft_params_without_named_variadic ft) ~f:(fun fp ->
        (not (get_fp_is_optional fp)) && not (get_fp_splat fp))
  in
  let (names_required, positional_params) =
    List.partition_map non_splat_non_optional ~f:(fun fp ->
        match Named_params.name_of_named_param fp with
        | Some name -> First name
        | None -> Second fp)
  in
  let arity_raw = List.length positional_params in
  let arity_required =
    if ft_has_positional_variadic ft then
      arity_raw - 1
    else
      arity_raw
  in
  (arity_required, S_set.of_list names_required)

let get_param_mode callconv =
  match callconv with
  | Ast_defs.Pinout _ -> FPinout
  | Ast_defs.Pnormal -> FPnormal

module DependentKind = struct
  let to_string = function
    | DTexpr i -> Expression_id.display i

  let is_generic_dep_ty s =
    String.is_substring ~substring:"::" s
    || String.equal s Naming_special_names.Typehints.this

  let strip_generic_dep_ty str =
    try
      let _ =
        Str.search_forward
          (Str.regexp {|<expr#[0-9]+>\:\:\(T[A-Za-z]+\)|})
          str
          0
      in
      Some (Str.matched_group 1 str)
    with
    | _ -> None

  let is_expr_dep_ty str = Option.is_some (strip_generic_dep_ty str)
end

let rec is_denotable ty =
  match get_node ty with
  | Tunion [] (* Possible encodings of nothing *)
  | Tintersection [] (* Possible encodings of mixed *)
  | Tnonnull
  | Tdynamic _
  | Tprim _
  | Tnewtype _ ->
    true
  | Tunion [ty; ty'] -> begin
    match (get_node ty, get_node ty') with
    | (Tprim Aast.Tfloat, Tprim Aast.Tint)
    | (Tprim Aast.Tint, Tprim Aast.Tfloat) ->
      true
    | (Tclass ((_, id), _, _), Tprim Aast.Tint)
    | (Tprim Aast.Tint, Tclass ((_, id), _, _))
      when String.equal Naming_special_names.Classes.cString id ->
      true
    | _ -> false
  end
  | Tclass (_, _, ts) -> List.for_all ~f:is_denotable ts
  | Ttuple { t_required; t_optional; t_extra } ->
    List.for_all ~f:is_denotable t_required
    && List.for_all ~f:is_denotable t_optional
    && tuple_extra_is_denotable t_extra
  | Tvec_or_dict (tk, tv) -> is_denotable tk && is_denotable tv
  | Taccess (ty, _) -> is_denotable ty
  | Tshape
      (Shape_simple
        { s_origin = _; s_unknown_value = unknown_field_type; s_fields = sm })
    ->
    TShapeMap.for_all (fun _ { sft_ty; _ } -> is_denotable sft_ty) sm
    && unknown_field_type_is_denotable unknown_field_type
  | Tshape (Shape_splat _) -> false
  | Tfun { ft_params; ft_ret; _ } ->
    is_denotable ft_ret
    && List.for_all ft_params ~f:(fun { fp_type; _ } -> is_denotable fp_type)
  | Toption ty -> is_denotable ty
  | Tgeneric nm ->
    not
      (DependentKind.is_generic_dep_ty nm
      || String.is_substring ~substring:"#" nm)
  | Tclass_ptr ty -> is_denotable ty
  | Tunion _
  | Tintersection _
  | Tneg _
  | Tany _
  | Tvar _
  | Tdependent _
  | Tlabel _ ->
    false

and tuple_extra_is_denotable e =
  match e with
  | Tsplat t -> is_denotable t
  | Tvariadic t -> unknown_field_type_is_denotable t

and unknown_field_type_is_denotable ty =
  match get_node ty with
  | Tunion [] -> true
  | Tintersection [] -> true
  | Toption ty -> begin
    match get_node ty with
    | Tnonnull -> true
    | _ -> false
  end
  | _ -> false

module ShapeFieldMap = struct
  include TShapeMap

  let map_and_rekey shape_map key_f value_f =
    let f_over_shape_field_type ({ sft_ty; _ } as shape_field_type) =
      { shape_field_type with sft_ty = value_f sft_ty }
    in
    TShapeMap.map_and_rekey shape_map key_f f_over_shape_field_type

  let map_env f env shape_map =
    let f_over_shape_field_type env _key ({ sft_ty; _ } as shape_field_type) =
      let (env, sft_ty) = f env sft_ty in
      (env, { shape_field_type with sft_ty })
    in
    TShapeMap.map_env f_over_shape_field_type env shape_map

  let map_env_ty_err_opt f env shape_map ~combine_ty_errs =
    let f_over_shape_field_type env _key ({ sft_ty; _ } as shape_field_type) =
      let (env, sft_ty) = f env sft_ty in
      (env, { shape_field_type with sft_ty })
    in
    TShapeMap.map_env_ty_err_opt
      f_over_shape_field_type
      env
      shape_map
      ~combine_ty_errs

  let map f shape_map = map_and_rekey shape_map (fun x -> x) f

  let iter f shape_map =
    let f_over_shape_field_type shape_map_key { sft_ty; _ } =
      f shape_map_key sft_ty
    in
    TShapeMap.iter f_over_shape_field_type shape_map

  let iter_values f = iter (fun _ -> f)
end

module ShapeFieldList = struct
  include Common.List

  let map_env env xs ~f =
    let f_over_shape_field_type env ({ sft_ty; _ } as shape_field_type) =
      let (env, sft_ty) = f env sft_ty in
      (env, { shape_field_type with sft_ty })
    in
    Common.List.map_env env xs ~f:f_over_shape_field_type
end

(*****************************************************************************)
(* Suggest mode *)
(*****************************************************************************)

(* Set to true when we are trying to infer the missing type hints. *)
let is_suggest_mode = ref false

let is_type_no_return : locl_ty_ -> bool = equal_locl_ty_ (Tprim Aast.Tnoreturn)

let get_ce_abstract ce = ClassElt.is_abstract ce.ce_flags

let get_ce_final ce = ClassElt.is_final ce.ce_flags

let get_ce_superfluous_override ce =
  ClassElt.has_superfluous_override ce.ce_flags

let get_ce_lsb ce = ClassElt.has_lsb ce.ce_flags

let get_ce_synthesized ce = ClassElt.is_synthesized ce.ce_flags

let get_ce_const ce = ClassElt.is_const ce.ce_flags

let get_ce_lateinit ce = ClassElt.has_lateinit ce.ce_flags

let get_ce_readonly_prop_or_needs_concrete ce =
  ClassElt.is_readonly_prop_or_needs_concrete ce.ce_flags

let get_ce_dynamicallycallable ce = ClassElt.is_dynamicallycallable ce.ce_flags

let get_ce_support_dynamic_type ce = ClassElt.supports_dynamic_type ce.ce_flags

let get_ce_xhp_attr ce = Typing_defs_flags.ClassElt.get_xhp_attr ce.ce_flags

let get_ce_safe_global_variable ce =
  ClassElt.is_safe_global_variable ce.ce_flags

let get_ce_no_auto_likes ce = ClassElt.is_no_auto_likes ce.ce_flags

let get_ce_tests_bypass_visibility ce =
  ClassElt.is_tests_bypass_visibility ce.ce_flags

let make_ce_flags = Typing_defs_flags.ClassElt.make

(** Return true if the element is private and not marked with the __LSB
  attribute. Private elements are not inherited by child classes and are
  namespaced to the containing class--if B extends A, then A may define a
  method A::foo and B may define a method B::foo, and they both will exist in
  the hierarchy and be callable at runtime (which method is invoked depends on
  the caller).

  The __LSB attribute can be applied to properties only. LSB properties are
  (effectively) implicitly cloned into every subclass. This means that in the
  typechecker, we want to avoid filtering them out in subclasses, so we treat
  them as non-private here. *)
let class_elt_is_private_not_lsb (elt : class_elt) : bool =
  match elt.ce_visibility with
  | Vprivate _ ->
    (not (get_ce_lsb elt)) && not (get_ce_tests_bypass_visibility elt)
  | Vprotected _
  | Vpublic
  | Vinternal _
  | Vprotected_internal _ ->
    false

let class_elt_is_private_or_protected_not_lsb elt =
  match elt.ce_visibility with
  | Vprivate _
  | Vprotected _
  | Vprotected_internal _
    when get_ce_lsb elt ->
    false
  | Vprivate _
  | Vprotected _
  | Vprotected_internal _ ->
    true
  | Vpublic
  | Vinternal _ ->
    false

let is_typeconst_type_abstract tc =
  match tc.ttc_kind with
  | TCConcrete _ -> false
  | TCAbstract _ -> true

let is_arraykey t =
  match get_node t with
  | Tprim Aast.Tarraykey -> true
  | _ -> false

let is_string t =
  match get_node t with
  | Tclass ((_, id), _, _) ->
    String.equal Naming_special_names.Classes.cString id
  | _ -> false

module Attributes = struct
  let mem x xs =
    List.exists xs ~f:(fun { ua_name; _ } -> String.equal x (snd ua_name))

  let find x xs =
    List.find xs ~f:(fun { ua_name; _ } -> String.equal x (snd ua_name))
end
