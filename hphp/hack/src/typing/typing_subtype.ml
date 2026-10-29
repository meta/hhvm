(*
 * Copyright (c) 2015, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude
open Common
open Typing_defs
open Typing_defs_constraints
open Typing_env_types
open Typing_logic_helpers
module Reason = Typing_reason
module Env = Typing_env
module Inter = Typing_intersection
module TUtils = Typing_utils
module SN = Naming_special_names
module Phase = Typing_phase
module TL = Typing_logic
module Cls = Folded_class
module ITySet = Internal_type_set
module MakeType = Typing_make_type
module Nast = Aast

(* We maintain a "visited" set for subtype goals. We do this only
 * for goals of the form T <: t or t <: T where T is a generic parameter,
 * as this is the more common case.
 * T83096774: work out how to do this *efficiently* for all subtype goals.
 *
 * Here's a non-trivial example (assuming a contravariant type Contra).
 * Under assumption T <: Contra<Contra<T>> show T <: Contra<T>.
 * This leads to cycle of implications
 *    T <: Contra<T> =>
 *    Contra<Contra<T>> <: Contra<T> =>
 *    T <: Contra<T>
 * at which point we are back at the original goal.
 *
 * Note that it's not enough to just keep a set of visited generic parameters,
 * else we would reject good code e.g. consider
 *   class C extends B implements Contra<B>
 * Now under assumption T <: C show T <: Contra<T>
 * This leads to cycle of implications
 *   T <: Contra<T> =>
 *   C <: Contra<T> =>
 *   Contra<B> <: Contra<T> =>
 *   T <: B =>     // DO NOT REJECT here just because we've visited T before!
 *   C <: B => done.
 *
 * We represent the visited set as a map from generic parameters
 * to pairs of sets of types, such that an entry T := ({t1,...,tm},{u1,...,un})
 * represents a set of goals
 *   T <: u1, ..., t <: un , t1 <: T, ..., tn <: T
 *)
module VisitedGoalsFunctor (Tset : Stdlib.Set.S) : sig
  type t

  val empty : t

  val try_add_visited_generic_sub : t -> string -> Tset.elt -> t option

  val try_add_visited_generic_super : t -> Tset.elt -> string -> t option
end = struct
  type t = (Tset.t * Tset.t) S_map.t

  let empty : t = S_map.empty

  (* Return None if (name <: ty) is already present, otherwise return Some v'
   * where v' has the pair added
   *)
  let try_add_visited_generic_sub v name ty =
    match S_map.find_opt name v with
    | None -> Some (S_map.add name (Tset.empty, Tset.singleton ty) v)
    | Some (lower, upper) ->
      if Tset.mem ty upper then
        None
      else
        Some (S_map.add name (lower, Tset.add ty upper) v)

  (* Return None if (ty <: name) is already present, otherwise return Some v'
   * where v' has the pair added
   *)
  let try_add_visited_generic_super v ty name =
    match S_map.find_opt name v with
    | None -> Some (S_map.add name (Tset.singleton ty, Tset.empty) v)
    | Some (lower, upper) ->
      if Tset.mem ty lower then
        None
      else
        Some (S_map.add name (Tset.add ty lower, upper) v)
end

module VisitedGoals = VisitedGoalsFunctor (Typing_set)
module VisitedGoalsInternal = VisitedGoalsFunctor (Internal_type_set)

module Pretty : sig
  val describe_ty_default :
    Typing_env_types.env -> Typing_defs_constraints.internal_type -> string

  val describe_ty_super :
    is_coeffect:bool ->
    Typing_env_types.env ->
    Typing_defs_constraints.internal_type ->
    string
end = struct
  let describe_ty_default env ty =
    Typing_print.full_strip_ns_i ~hide_internals:true env ty

  let describe_ty ~is_coeffect : env -> internal_type -> string =
    (* Optimization: specialize on partial application, i.e.
       *    let describe_ty_sub = describe_ty ~is_coeffect in
       *  will check the flag only once, not every time the function is called *)
    if not is_coeffect then
      describe_ty_default
    else
      fun env -> function
       | LoclType ty -> Lazy.force @@ Typing_coeffects.pretty env ty
       | ty -> describe_ty_default env ty

  let rec describe_ty_super ~is_coeffect env ty =
    let describe_ty_super = describe_ty_super ~is_coeffect in
    let print = (describe_ty ~is_coeffect) env in
    let default () = print ty in
    match ty with
    | LoclType ty ->
      let (env, ty) = Env.expand_type env ty in
      (match get_node ty with
      | Tvar v ->
        let upper_bounds = ITySet.elements (Env.get_tyvar_upper_bounds env v) in
        (* The constraint graph is transitively closed so we can filter tyvars. *)
        let upper_bounds =
          List.filter upper_bounds ~f:(fun t -> not (is_tyvar_i t))
        in
        (match upper_bounds with
        | [] -> "some type not known yet"
        | tyl ->
          let (locl_tyl, cstr_tyl) = List.partition_tf tyl ~f:is_locl_type in
          let sep =
            match (locl_tyl, cstr_tyl) with
            | (_ :: _, _ :: _) -> " and "
            | _ -> ""
          in
          let locl_descr =
            match locl_tyl with
            | [] -> ""
            | tyl ->
              "of type "
              ^ (String.concat ~sep:" & " (List.map tyl ~f:print)
                |> Markdown_lite.md_codify)
          in
          let cstr_descr =
            String.concat
              ~sep:" and "
              (List.map cstr_tyl ~f:(describe_ty_super env))
          in
          "something " ^ locl_descr ^ sep ^ cstr_descr)
      | Toption ty when is_tyvar ty ->
        "`null` or " ^ describe_ty_super env (LoclType ty)
      | _ -> Markdown_lite.md_codify (default ()))
    | ConstraintType ty ->
      (match deref_constraint_type ty with
      | (_, Thas_member hm) ->
        let {
          hm_name = (_, name);
          hm_type = _;
          hm_class_id = _;
          hm_method = method_info;
        } =
          hm
        in
        (match method_info with
        | None -> Printf.sprintf "an object with property `%s`" name
        | Some _ -> Printf.sprintf "an object with method `%s`" name)
      | (_, Thas_type_member htm) ->
        let { htm_id = id; htm_lower = lo; htm_upper = up } = htm in
        if phys_equal lo up then
          (* We use physical equality as a heuristic to generate
             slightly more readable descriptions. *)
          Printf.sprintf
            "a class with `{type %s = %s}`"
            id
            (describe_ty ~is_coeffect:false env (LoclType lo))
        else
          let bound_desc ~prefix ~is_trivial bnd =
            if is_trivial env bnd then
              ""
            else
              prefix ^ describe_ty ~is_coeffect:false env (LoclType bnd)
          in
          Printf.sprintf
            "a class with `{type %s%s%s}`"
            id
            (bound_desc ~prefix:" super " ~is_trivial:TUtils.is_nothing lo)
            (bound_desc ~prefix:" as " ~is_trivial:TUtils.is_mixed up)
      | (_, Tcan_traverse _) -> "an array that can be traversed with foreach"
      | (_, Tcan_index _) -> "an array that can be indexed"
      | (_, Tcan_index_assign _) -> "an array that can be updated"
      | (_, Thas_const { name; ty = _ }) ->
        Printf.sprintf "a class with a member `%s`" name
      | (_, Ttype_switch _)
      | (_, Tdestructure _) ->
        Markdown_lite.md_codify
          (Typing_print.full_strip_ns_i
             ~hide_internals:true
             env
             (ConstraintType ty)))
end

module Subtype_env = struct
  type tparam_constraint_candidate = {
    r_sub: Reason.t;
    r_super: Reason.t;
  }

  type t = {
    require_soundness: bool;
        (** If set, requires the simplification of subtype constraints to be sound,
          meaning that the simplified constraint must imply the original one. *)
    require_completeness: bool;
        (** If set, requires the simplification of subtype constraints to be complete,
          meaning that the original constraint must imply the simplified one.
          If set, we also finish as soon as we see a goal of the form T <: t or
          t <: T for generic parameter T *)
    ignore_readonly: bool;
        (** If set, do not apply readonly subtyping through function types i.e. the
         * readonly qualifier on function types and the readonly qualifier on
         * function parameters. This flag is triggered by the <<__IgnoreReadonlyError>>
         * on parameters.
         *)
    visited: VisitedGoals.t;
        (** If require_completeness is not set, maintain a visited goal set *)
    visited_internal: VisitedGoalsInternal.t;
    no_top_bottom: bool;
    is_dynamic_aware: bool;
        (** is_dynamic_aware indicates whether subtyping should allow
          types that implement dynamic are considered sub-types of dynamic. *)
    on_error: Typing_error.Reasons_callback.t option;
    report_ambiguous_shape_splat: bool;
    has_member_arg_posl: Pos.t list option;
    tparam_constraint_candidates: tparam_constraint_candidate list;
        (** The reason pairs along this subtype path. They are inspected only
          if an error is evaluated. *)
    is_coeffect: bool;
        (** A flag which, if set, indicates that coeffects are being subtyped.
          Note: this is a short-term solution to provide coeffects.pretty-printing of
          `locl_ty`s that represent coeffects, since there is no good way to
          tell apart coeffects from regular types *)
    log_level: int;
        (** Which level the recursive calls to simplify_subtype should be logged at *)
    in_transitive_closure: bool;
        (** This is a subtype check from within transitive closure
          e.g. string <: #1 <: int doing string <: int *)
    ignore_likes: bool;
        (** We're ignoring likes because we had a goal of the form `t <: ~u`
            for t <:D dynamic *)
    class_sub_classname: bool;
        (** When enabled, allow rewriting of class<T> <: U to classname<T> <: U
            for a subset of U types. This is normally controlled by the
            class_sub_classname typechecker option, but it is unconditionally
            false for type well-formedness checks. Method override checks can
            also disable it. *)
    recursion_tracker: Subtype_recursion_tracker.t;
  }

  (* Callback presence is fixed by [create]. Descendants may only contextualize
     an existing callback. *)
  let map_on_error t ~f =
    match t.on_error with
    | None -> t
    | Some on_error -> { t with on_error = Some (f on_error) }

  let set_visited t visited = { t with visited }

  let set_visited_internal t visited_internal = { t with visited_internal }

  let set_is_dynamic_aware t is_dynamic_aware = { t with is_dynamic_aware }

  let create
      ?(require_soundness = true)
      ?(require_completeness = false)
      ?(ignore_readonly = false)
      ?(no_top_bottom = false)
      ?(is_dynamic_aware = false)
      ?(is_coeffect = false)
      ?(in_transitive_closure = false)
      ?(ignore_likes = false)
      ?(has_member_arg_posl = None)
      ?(report_ambiguous_shape_splat = false)
      ~class_sub_classname
      ~(log_level : int)
      on_error =
    {
      require_soundness;
      require_completeness;
      ignore_readonly;
      visited = VisitedGoals.empty;
      visited_internal = VisitedGoalsInternal.empty;
      no_top_bottom;
      is_dynamic_aware;
      is_coeffect;
      on_error;
      report_ambiguous_shape_splat;
      has_member_arg_posl;
      tparam_constraint_candidates = [];
      log_level;
      in_transitive_closure;
      ignore_likes;
      class_sub_classname;
      recursion_tracker = Subtype_recursion_tracker.empty;
    }

  let possibly_add_violated_constraint subtype_env ~r_sub ~r_super =
    match subtype_env.on_error with
    (* Callback presence is fixed by [create], and descendants only transform
       an existing callback through [map_on_error]. *)
    | None -> subtype_env
    | Some _ ->
      {
        subtype_env with
        tparam_constraint_candidates =
          { r_sub; r_super } :: subtype_env.tparam_constraint_candidates;
      }

  let resolve_tparam_constraints candidates =
    let add_candidate constraints { r_sub; r_super } =
      let candidate =
        match Reason.Predicates.unpack_cstr_on_generics_opt r_super with
        | Some candidate -> Some candidate
        | None -> Reason.Predicates.unpack_cstr_on_generics_opt r_sub
      in
      match candidate with
      | None -> constraints
      | Some ((p, tparam) as candidate) ->
        (match constraints with
        | (p_prev, tparam_prev) :: _
          when Pos_or_decl.equal p p_prev
               && Typing_defs.equal_pos_id tparam tparam_prev ->
          constraints
        | _ -> candidate :: constraints)
    in
    List.fold_left (List.rev candidates) ~init:[] ~f:add_candidate

  let lazy_tparam_constraints { tparam_constraint_candidates; _ } =
    lazy (resolve_tparam_constraints tparam_constraint_candidates)

  let choose_on_error cstrs on_error =
    let has_constraints =
      Lazy.map cstrs ~f:(fun cstrs -> not (List.is_empty cstrs))
    in
    Typing_error.Reasons_callback.choose
      has_constraints
      ~if_true:on_error
      ~if_false:(Typing_error.Reasons_callback.retain_code on_error)

  let mk_secondary_error cstrs { is_coeffect; _ } ty_sub ty_super =
    Typing_error.Secondary.Subtyping_error
      { cstrs; ty_sub; ty_sup = ty_super; is_coeffect }

  let fail_with_secondary_error t secondary_error =
    Option.map t.on_error ~f:(fun on_error ->
        let cstrs = lazy_tparam_constraints t in
        let on_error = choose_on_error cstrs on_error in
        Typing_error.apply_reasons ~on_error secondary_error)

  let fail t ~ty_sub ~ty_super =
    Option.map t.on_error ~f:(fun on_error ->
        let cstrs = lazy_tparam_constraints t in
        let secondary_error = mk_secondary_error cstrs t ty_sub ty_super in
        let on_error = choose_on_error cstrs on_error in
        Typing_error.apply_reasons ~on_error secondary_error)

  let fail_with_suffix t ~ty_sub ~ty_super suffix =
    Option.map t.on_error ~f:(fun on_error ->
        let cstrs = lazy_tparam_constraints t in
        let secondary_error = mk_secondary_error cstrs t ty_sub ty_super in
        let on_error = choose_on_error cstrs on_error in
        Typing_error.apply_reasons
          ~on_error:
            (Typing_error.Reasons_callback.prepend_on_apply
               on_error
               secondary_error)
          suffix)

  let check_infinite_recursion (subtype_env : t) op =
    Subtype_recursion_tracker.add_op_and_check_infinite_recursion
      subtype_env.recursion_tracker
      op
    |> Result.map ~f:(fun recursion_tracker ->
           { subtype_env with recursion_tracker })
end

module Logging : sig
  val log_subtype :
    this_ty:Typing_defs.locl_ty option ->
    function_name:string ->
    Typing_env_types.env ->
    Typing_defs.locl_ty ->
    Typing_defs.locl_ty ->
    (unit -> 'a) ->
    'a

  val log_subtype_i :
    this_ty:Typing_defs.locl_ty option ->
    function_name:string ->
    Typing_env_types.env ->
    Typing_defs_constraints.internal_type ->
    Typing_defs_constraints.internal_type ->
    (unit -> 'a) ->
    'a

  val log_subtype_prop :
    Typing_env_types.env ->
    Typing_defs.locl_ty ->
    Typing_defs.locl_ty ->
    this_ty:Typing_defs.locl_ty option ->
    function_name:string ->
    (unit -> Typing_env_types.env * TL.subtype_prop) ->
    Typing_env_types.env * TL.subtype_prop

  val should_log_subtype :
    Typing_env_types.env ->
    level:Core__Int.t ->
    Typing_defs.locl_ty ->
    Typing_defs.locl_ty ->
    bool

  val should_log_subtype_i :
    Typing_env_types.env ->
    level:Core__Int.t ->
    Typing_defs_constraints.internal_type ->
    Typing_defs_constraints.internal_type ->
    bool
end = struct
  (* Given a pair of types `ty_sub` and `ty_super` attempt to apply simplifications
   * and add to the accumulated constraints in `constraints` any necessary and
   * sufficient [(t1,ck1,u1);...;(tn,ckn,un)] such that
   *   ty_sub <: ty_super iff t1 ck1 u1, ..., tn ckn un
   * where ck is `as` or `=`. Essentially we are making solution-preserving
   * simplifications to the subtype assertion, for now, also generating equalities
   * as well as subtype assertions, for backwards compatibility with use of
   * unification.
   *
   * If `constraints = []` is returned then the subtype assertion is valid.
   *
   * If the subtype assertion is unsatisfiable then return `failed = Some f`
   * where `f` is a `unit-> unit` function that records an error message.
   * (Sometimes we don't want to call this function e.g. when just checking if
   *  a subtype holds)
   *
   * Elide singleton unions, treat invariant generics as both-ways
   * subtypes, and actually chase hierarchy for extends and implements.
   *
   * Annoyingly, we need to pass env back too, because Typing_phase.localize
   * expands type constants. (TODO: work out a better way of handling this)
   *
   * Special cases:
   *   If assertion is valid (e.g. string <: arraykey) then
   *     result can be the empty list (i.e. nothing is added to the result)
   *   If assertion is unsatisfiable (e.g. arraykey <: string) then
   *     we record this in the failed field of the result.
   *)

  let log_subtype_i_
      ~(this_ty : locl_ty option) ~function_name ~result env ty_sub ty_super f =
    Typing_log.log_function
      (Reason.to_pos (reason ty_sub))
      ~function_name
      ~arguments:
        [
          ("ty_sub", Typing_print.debug_i env ty_sub);
          ("ty_super", Typing_print.debug_i env ty_super);
          ( "this_ty",
            match this_ty with
            | None -> "None"
            | Some ty -> Typing_print.debug env ty );
        ]
      ~result
      f

  let log_subtype_i_prop ~this_ty ~function_name env ty_sub ty_super f =
    log_subtype_i_
      ~this_ty
      ~function_name
      ~result:(fun (env, prop) ->
        Some (TL.print (Typing_print.debug_i env) prop))
      env
      ty_sub
      ty_super
      f

  let log_subtype_prop env ty_sub ty_super ~this_ty ~function_name f =
    log_subtype_i_prop
      env
      (LoclType ty_sub)
      (LoclType ty_super)
      ~this_ty
      ~function_name
      f

  let should_log_subtype_i env ~level ty_sub ty_super =
    let level =
      if
        (TUtils.is_capability_i ty_sub || TUtils.is_capability_i ty_super)
        && level < 3
      then
        3
      else
        level
    in
    Typing_log.should_log env ~category:"sub" ~level

  let log_subtype_i
      ~(this_ty : locl_ty option) ~function_name env ty_sub ty_super f =
    log_subtype_i_
      ~result:(fun _ -> None)
      ~this_ty
      ~function_name
      env
      ty_sub
      ty_super
      f

  let should_log_subtype env ~level ty_sub ty_super =
    should_log_subtype_i env ~level (LoclType ty_sub) (LoclType ty_super)

  let log_subtype ~this_ty ~function_name env ty_sub ty_super f =
    log_subtype_i
      ~this_ty
      ~function_name
      env
      (LoclType ty_sub)
      (LoclType ty_super)
      f
end

module Subtype_negation = struct
  let is_tprim_disjoint tp1 tp2 =
    let one_side tp1 tp2 =
      Aast_defs.(
        match (tp1, tp2) with
        | (Tnum, Tint)
        | (Tnum, Tfloat)
        | (Tarraykey, Tint)
        | (Tarraykey, Tnum) ->
          false
        | ( _,
            ( Tnum | Tint | Tvoid | Tbool | Tarraykey | Tfloat | Tnull
            | Tresource | Tnoreturn ) ) ->
          true)
    in
    (not (Aast_defs.equal_tprim tp1 tp2))
    && one_side tp1 tp2
    && one_side tp2 tp1

  (* Two classes c1 and c2 are disjoint iff there exists no c3 such that
     c3 <: c1 and c3 <: c2. *)
  let is_class_disjoint env c1 ex1 c2 ex2 =
    let is_interface_or_trait c_def =
      Ast_defs.(
        match Cls.kind c_def with
        | Cinterface
        | Ctrait ->
          true
        | Cclass _
        | Cenum_class _
        | Cenum ->
          false)
    in
    if String.equal c1 c2 then
      false
    else if
      String.equal c1 SN.Classes.cString || String.equal c2 SN.Classes.cString
    then
      (* Strings are disjoint from other classes, except for stringish *)
      not
        ((String.equal c1 SN.Classes.cStringish && is_nonexact ex1)
        || (String.equal c2 SN.Classes.cStringish && is_nonexact ex2))
    else
      match (Env.get_class env c1, Env.get_class env c2) with
      | (Decl_entry.Found c1_def, Decl_entry.Found c2_def) ->
        if Cls.final c1_def then
          (* if c1 is final, then c3 would have to be equal to c1 *)
          not (Cls.has_ancestor c1_def c2)
        else if Cls.final c2_def then
          (* if c2 is final, then c3 would have to be equal to c2 *)
          not (Cls.has_ancestor c2_def c1)
        else
          (* Given two non-final classes, if either is an interface or trait, then
             there could be a c3, and so we consider the classes to not be disjoint.
             However, if they are both classes, then c3 must be either c1 or c2 since
             we don't have multiple inheritance. *)
          (not (is_interface_or_trait c1_def))
          && (not (is_interface_or_trait c2_def))
          && (not (Cls.has_ancestor c2_def c1))
          && not (Cls.has_ancestor c1_def c2)
      | _ ->
        (* This is a decl error that should have already been caught *)
        false

  (** [negate_ak_null_type env r ty] performs type negation similar to
  TUtils.negate_type, but restricted to arraykey and null (and their
  negations). *)
  let negate_ak_null_type env r ty =
    let (env, ty) = Env.expand_type env ty in
    let neg_ty =
      match get_node ty with
      | Tprim Aast.Tnull -> Some (MakeType.nonnull r)
      | Tprim Aast.Tarraykey -> Some (MakeType.neg r (r, IsTag ArraykeyTag))
      | Tneg (r, IsTag ArraykeyTag) ->
        Some (MakeType.prim_type r Aast.Tarraykey)
      | Tneg (r, IsTag NullTag) -> Some (MakeType.null r)
      | Tnonnull -> Some (MakeType.null r)
      | Tneg (_, IsTag (ClassTag (id, _)))
        when not (String.equal SN.Classes.cString id) ->
        None
      | Tneg predicate ->
        (* negate_ak_null_type is ultimately used in simplifcations of the form
           A & !B <: C iff A <: C | B
           However, if we overapproximate (C | B) which we do for shape unions,
           this transform is unsound.
           For this reason, we exclude predicates containing IsShapeOf from this
           calculus.
           Another (better) solution would be to add a flag to Typing_union
           functions forbidding overapproximation.
           We also overapproximate unions of tuples, but I don't think we can
           contrive/observe the same unsoundness since we atomicize tuples
           during type splitting
        *)
        let rec has_isshape predicate =
          match snd predicate with
          | IsTag _ -> false
          | IsUnionOf preds
          | IsTupleOf { tp_required = preds } ->
            List.exists preds ~f:has_isshape
          | IsShapeOf _ -> true
          | IsNot inner -> has_isshape inner
        in
        if has_isshape predicate then
          None
        else
          Typing_refinement.TyPredicate.to_ty_without_instantiation_opt
            env
            predicate
      | _ -> None
    in
    (env, neg_ty)

  let find_type_with_exact_negation env tyl =
    (* Prefer to negate [Tneg _] components first. Negating a [Tneg P]
       gives the predicate-as-type [P]; the resulting union [C | P] is much
       more useful for closing out the subtype check than the [C | Tneg X]
       we'd get from negating, e.g., an [arraykey] component. *)
    let rec find env tyl acc_tyl =
      match tyl with
      | [] -> (env, None, acc_tyl)
      | ty :: tyl' ->
        let (env, neg_ty) = negate_ak_null_type env (get_reason ty) ty in
        (match neg_ty with
        | None -> find env tyl' (ty :: acc_tyl)
        | Some neg_ty -> (env, Some neg_ty, tyl' @ acc_tyl))
    in
    let (tneg_tys, other_tys) = List.partition_tf tyl ~f:is_neg in
    find env (tneg_tys @ other_tys) []
end

let get_tyvar_opt t =
  match t with
  | LoclType lt -> begin
    match get_node lt with
    | Tvar var -> Some var
    | _ -> None
  end
  | _ -> None

(* build the interface corresponding to the can_traverse constraint *)
let can_traverse_to_iface ct =
  match (ct.ct_key, ct.ct_is_await) with
  | (None, false) -> MakeType.traversable ct.ct_reason ct.ct_val
  | (None, true) -> MakeType.async_iterator ct.ct_reason ct.ct_val
  | (Some ct_key, false) ->
    MakeType.keyed_traversable ct.ct_reason ct_key ct.ct_val
  | (Some ct_key, true) ->
    MakeType.async_keyed_iterator ct.ct_reason ct_key ct.ct_val

module Sd = struct
  let liken ~super_like env ty =
    if super_like then
      TUtils.make_like env ty
    else
      ty

  (* At present, we don't distinguish between coercions (<:D) and subtyping (<:) in the
   * type variable and type parameter environments. When closing the environment we use subtyping (<:).
   * To mitigate against this, when adding a dynamic upper bound wrt coercion,
   * transform it first into supportdyn<mixed>,
   * as t <:D dynamic iff t <: supportdyn<mixed>.
   *)
  let transform_dynamic_upper_bound ~is_dynamic_aware env ty =
    if Tast.is_under_dynamic_assumptions env.checked then
      ty
    else
      match (is_dynamic_aware, get_node ty) with
      | (true, Tdynamic _) ->
        let r = get_reason ty in
        MakeType.supportdyn_mixed r
      | (true, _) -> ty
      | _ -> ty
end

let mk_issubtype_prop ~sub_supportdyn ~is_dynamic_aware env ty1 ty2 =
  let (env, ty1) =
    match sub_supportdyn with
    | None -> (env, ty1)
    | Some r ->
      let (env, ty1) = Env.expand_internal_type env ty1 in
      ( env,
        (match ty1 with
        | LoclType ty ->
          if is_tyvar ty then
            ty1
          else
            let ty = MakeType.supportdyn r ty in
            LoclType ty
        | _ -> ty1) )
  in
  match ty2 with
  | LoclType ty2 ->
    (* TODO akenn: somehow we lose this environment *)
    let (env, is_dynamic_aware, ty2) =
      (* If we are in dynamic-aware subtyping mode, that fact will be lost when ty2
         ends up on the upper bound of a type variable. Here we find if ty2 contains
         dynamic and replace it with supportdyn<mixed> which is equivalent, but does not
         require dynamic-aware subtyping mode to be a supertype of types that support dynamic. *)
      match is_dynamic_aware with
      | true ->
        let (env, ty_opt) =
          Typing_dynamic_utils.try_strip_dynamic
            ~do_not_solve_likes:true
            env
            ty2
        in
        begin
          match ty_opt with
          | Some non_dyn_ty ->
            let r = get_reason ty2 in
            ( env,
              false,
              MakeType.union r [non_dyn_ty; MakeType.supportdyn_mixed r] )
          | None -> (env, is_dynamic_aware, ty2)
        end
      | _ -> (env, is_dynamic_aware, ty2)
    in
    (env, TL.IsSubtype (is_dynamic_aware, ty1, LoclType ty2))
  | _ -> (env, TL.IsSubtype (is_dynamic_aware, ty1, ty2))

(* All of our constraints have a type with additional context to support <:D *)
type lhs = {
  sub_supportdyn: Reason.t option;
  ty_sub: locl_ty;
}

(* Just some shorthand *)
let should_cls_sub_cn env =
  Typechecker_options.class_sub_classname env.genv.tcopt

(* Find the first generic occurring in [ty] with a rank higher than [rank]  *)
let check_rank env ty rank =
  (* To avoid this potentially expensive check we record when the sub- or
     supertype _may_ contain a higher rank type parameter. *)
  if Typing_env_types.should_check_rank env then
    let p ty =
      match get_node ty with
      | Tgeneric name -> Env.rank_of_tparam env name > rank
      | Tvar tv -> Env.rank_of_tvar env tv > rank
      | _ -> false
    in
    let ty_opt = Typing_defs_core.find_locl_ty ty ~p in
    Option.map ty_opt ~f:(fun ty ->
        match deref ty with
        | (r, Tgeneric name) -> (r, Some name)
        | (r, Tvar _) -> (r, None)
        | _ ->
          (* We are guaranteed that if the result if [Some(...)] then the type
             is a [Tgeneric] so something is seriously wrong here *)
          failwith "check_rank: unexpected type")
  else
    None

module type Constraint_handler = sig
  type rhs

  val simplify :
    subtype_env:Subtype_env.t ->
    this_ty:Typing_defs.locl_ty option ->
    lhs:lhs ->
    rhs:rhs ->
    Typing_env_types.env ->
    Typing_env_types.env * TL.subtype_prop
end

module rec Subtype : sig
  type rhs = {
    super_supportdyn: bool;
    super_like: bool;
    ty_super: locl_ty;
  }

  (** Given types ty_sub and ty_super, attempt to
   reduce the subtyping proposition ty_sub <: ty_super to
   a logical proposition whose primitive assertions are of the form v <: t or t <: v
   where v is a type variable.

   If super_like=true, then we have already reduced ty_sub <: ~ty_super to ty_sub <: ty_super
   with ty_super known to support dynamic (i.e. ty_super <: supportdyn<mixed>). In this case,
   when "going under" a constructor (for example, we had C<t> <: ~C<u>),
   we can apply "like pushing" on the components (in this example, t <: ~u).
   The parameter defaults to false to guard against incorrectly propagating the option. When
   simplifying ty_sub only (e.g. reducing t|u <: v to t<:v && u<:v) it is correct to
   propagate it.

    The logical relationship between the original and returned proposition
    depends on the flags require_soundness and require_completeness.
    Fail with Unsat error_function if
    the assertion is unsatisfiable. Some examples:
      string <: arraykey  ==>  True    (represented as Conj [])
    (For covariant C and a type variable v)
      C<string> <: C<v>   ==>  string <: v
    (Assuming that C does *not* implement interface J)
      C <: J              ==>  Unsat _
    (Assuming we have T <: D in tpenv, and class D implements I)
      vec<T> <: vec<I>    ==>  True
    This last one would be left as T <: I if subtype_env.require_completeness=true
   *)
  include Constraint_handler with type rhs := rhs

  val simplify_funs :
    subtype_env:Subtype_env.t ->
    check_return:bool ->
    for_override:bool ->
    super_like:bool ->
    Reason.t * Typing_defs.locl_fun_type ->
    Reason.t * Typing_defs.locl_fun_type ->
    Typing_env_types.env ->
    Typing_env_types.env * TL.subtype_prop
end = struct
  type rhs = {
    super_supportdyn: bool;
    super_like: bool;
    ty_super: locl_ty;
  }

  type shape_splat_failure =
    | Unsupported_cycle of Typing_corners.Cycle_info.t
    | Missing_assignment of Typing_corners.Missing_assignment.t

  type 'a shape_splat_result = (env * 'a, env * shape_splat_failure) result

  let project_shape_splat env row label assignment =
    let (env, result) = Typing_corners.proj env row label assignment in
    match result with
    | Ok field -> Ok (env, field)
    | Error missing -> Error (env, Missing_assignment missing)

  let shape_splat_assignments cache env live label r =
    let (env, result) =
      Typing_corners.corner_assignments cache env live label r
    in
    match result with
    | Ok (Typing_corners.Computed assignments) -> Ok (env, Some assignments)
    | Ok Typing_corners.Empty -> Ok (env, None)
    | Ok (Typing_corners.Unsupported_cycle info) ->
      Error (env, Unsupported_cycle info)
    | Error missing -> Error (env, Missing_assignment missing)

  let shape_splat_conj (env, left) right =
    let open Hh_prelude.Result.Let_syntax in
    let* (env_after, right) = right env in
    let (env, prop) = (env, left) &&& fun _ -> (env_after, right) in
    return (env, prop)

  let missing_shape_splat_assignment subtype_env env missing ~ty_sub ~ty_super =
    let subtype_env =
      Subtype_env.map_on_error subtype_env ~f:(fun on_error ->
          Typing_error.Reasons_callback.prepend_reason
            on_error
            ~reason:
              (lazy
                ( Typing_corners.Missing_assignment.position missing,
                  "Shape-splat dependency analysis failed before projection" )))
    in
    let fail = Subtype_env.fail subtype_env ~ty_sub ~ty_super in
    invalid ~fail env

  let unsupported_cycle_position fallback cycle info =
    match cycle with
    | dependency :: _ -> Typing_corners.Dependency.position dependency
    | [] ->
      (match
         Typing_corners.Splat_elem.Set.choose_opt
           (Typing_corners.Cycle_info.members info)
       with
      | Some member -> get_pos member
      | None -> fallback)

  let shape_splat_cycle_edge dependency =
    let open Typing_error.Secondary.Shape_splat_cycle in
    let kind =
      match Typing_corners.Dependency.kind dependency with
      | Typing_corners.Dependency.Direct_upper -> Direct_upper
      | Typing_corners.Dependency.Indirect_upper -> Indirect_upper
      | Typing_corners.Dependency.Nested_upper -> Nested_upper
      | Typing_corners.Dependency.Nested_lower -> Nested_lower
    in
    {
      pos = Typing_corners.Dependency.position dependency;
      source = Typing_corners.Dependency.source dependency;
      target = Typing_corners.Dependency.target dependency;
      kind;
    }

  let unsupported_cyclic_shape_splat_bounds
      subtype_env env ~r info ~ty_sub ~ty_super =
    let dependency_cycle =
      match Typing_corners.Cycle_info.dependency_cycle info with
      | [] -> Typing_corners.Cycle_info.dependencies info
      | cycle -> cycle
    in
    let position =
      unsupported_cycle_position (Reason.to_pos r) dependency_cycle info
    in
    let cycle = List.map dependency_cycle ~f:shape_splat_cycle_edge in
    let subtype_env =
      Subtype_env.map_on_error subtype_env ~f:(fun on_error ->
          Typing_error.Reasons_callback.prepend_on_apply
            on_error
            (Typing_error.Secondary.Unsupported_cyclic_shape_splat_bounds
               { pos = position; cycle }))
    in
    let fail = Subtype_env.fail subtype_env ~ty_sub ~ty_super in
    invalid ~fail env

  let report_shape_splat_failure subtype_env ~r ~sub ~super env failure =
    let ty_sub = LoclType (Typing_shape_normalize.Row.to_ty ~reason:r sub)
    and ty_super =
      LoclType (Typing_shape_normalize.Row.to_ty ~reason:r super)
    in
    match failure with
    | Unsupported_cycle info ->
      unsupported_cyclic_shape_splat_bounds
        subtype_env
        env
        ~r
        info
        ~ty_sub
        ~ty_super
    | Missing_assignment missing ->
      missing_shape_splat_assignment subtype_env env missing ~ty_sub ~ty_super

  module Log = struct
    let level subtype_env ~ty_sub ~ty_super =
      let level = subtype_env.Subtype_env.log_level in
      if
        (TUtils.is_capability ty_sub || TUtils.is_capability ty_super)
        && level < 3
      then
        3
      else
        level

    let should_log env subtype_env ~lhs ~rhs =
      Typing_log.should_log
        env
        ~category:"sub"
        ~level:(level subtype_env ~ty_sub:lhs.ty_sub ~ty_super:rhs.ty_super)

    let function_name subtype_env ~sub_supportdyn ~super_like ~super_supportdyn
        =
      "Typing_subtype.Subtype.simplify"
      ^ (match subtype_env.Subtype_env.is_dynamic_aware with
        | false -> ""
        | true -> " <:D")
      ^
      let flag str = function
        | true -> str
        | false -> ""
      in
      flag " sub_supportdyn" (Option.is_some sub_supportdyn)
      ^ flag " super_supportdyn" super_supportdyn
      ^ flag " super_like" super_like
      ^ flag " ignore_likes" subtype_env.Subtype_env.ignore_likes
      ^ flag " require_soundness" subtype_env.Subtype_env.require_soundness
      ^ flag
          " require_completeness"
          subtype_env.Subtype_env.require_completeness
      ^ flag " ignore_readonly" subtype_env.Subtype_env.ignore_readonly
      ^ flag
          " in_transitive_closure"
          subtype_env.Subtype_env.in_transitive_closure

    let log env subtype_env ~this_ty ~lhs ~rhs =
      let { sub_supportdyn; ty_sub } = lhs in
      let { super_like; super_supportdyn; ty_super } = rhs in
      Logging.log_subtype_prop
        ~this_ty
        ~function_name:
          (function_name
             subtype_env
             ~sub_supportdyn
             ~super_like
             ~super_supportdyn)
        env
        ty_sub
        ty_super
  end

  let is_final_and_invariant env id =
    let class_def = Env.get_class env id in
    match class_def with
    | Decl_entry.Found class_ty -> TUtils.class_is_final_and_invariant class_ty
    | Decl_entry.DoesNotExist
    | Decl_entry.NotYetAvailable ->
      false

  let expands_to ty ~p ~env =
    let (_, ty) = Env.expand_type env ty in
    p ty

  let expands_to_nonnull ty ~env = expands_to ty ~p:is_nonnull ~env

  let expands_to_supportdyn ty ~env =
    expands_to ty ~env ~p:(fun ty ->
        match get_node ty with
        | Tnewtype (name, [_], _) -> String.equal name SN.Classes.cSupportDyn
        | _ -> false)

  let expands_to_var_or_intersection ty ~env =
    expands_to ty ~env ~p:(fun ty ->
        match get_node ty with
        | Tvar _
        | Tintersection _ ->
          true
        | _ -> false)

  (* Helper function for inspecting types occuring inside a union from a top-level
     pattern match *)
  let var_and_arraykey_opt ty1 ty2 ~env =
    let (env, ty1) = Env.expand_type env ty1 in
    let (env, ty2) = Env.expand_type env ty2 in
    match (deref ty1, deref ty2) with
    | ( ((_, Tvar _) as tv),
        ((_, (Tprim Aast.Tarraykey | Tneg (_, IsTag ArraykeyTag))) as ak) )
    | ( ((_, (Tprim Aast.Tarraykey | Tneg (_, IsTag ArraykeyTag))) as ak),
        ((_, Tvar _) as tv) ) ->
      (env, Some (tv, ak))
    | _ -> (env, None)

  let var_and_arraykey_exn ty1 ty2 ~env =
    match var_and_arraykey_opt ty1 ty2 ~env with
    | (env, Some (tv, ak)) -> (env, (tv, ak))
    | _ ->
      failwith
        "Expected a pair of types, one of which a [Tvar] and the other [Tprim Tarraykey] or [Tneg (IsTag ArraykeyTag)]"

  let expands_to_var_and_arraykey ty1 ty2 ~env =
    match var_and_arraykey_opt ty1 ty2 ~env with
    | (_, Some _) -> true
    | _ -> false

  let expands_to_class ty ~env =
    expands_to ty ~env ~p:(fun ty ->
        match get_node ty with
        | Tclass _ -> true
        | _ -> false)

  let class_id_exn ty ~env =
    let (env, ty) = Env.expand_type env ty in
    match get_node ty with
    | Tclass (id, _, _) -> (env, id)
    | _ -> failwith "This type is not a Tclass"

  let newtype_exn ty ~env =
    let (env, ty) = Env.expand_type env ty in
    match get_node ty with
    | Tnewtype (name, tyargs, bound) -> (env, (name, tyargs, bound))
    | _ -> failwith "This type is not a Tnewtype"

  let generic_lower_bounds env ty_super =
    let rec fixpoint new_set bounds_set =
      if Typing_set.is_empty new_set then
        bounds_set
      else
        let add_set =
          Typing_set.fold
            (fun ty add_set ->
              match get_node ty with
              | Tgeneric name ->
                let gen_bounds = Env.get_lower_bounds env name in
                Typing_set.union add_set gen_bounds
              | _ -> add_set)
            new_set
            Typing_set.empty
        in
        let bounds_set = Typing_set.union new_set bounds_set in
        let new_set = Typing_set.diff add_set bounds_set in
        fixpoint new_set bounds_set
    in
    let lower_bounds =
      fixpoint (Typing_set.singleton ty_super) Typing_set.empty
    in
    Typing_set.fold
      (fun bound_ty (g_set, o_set) ->
        match get_node bound_ty with
        | Tgeneric name -> (S_set.add name g_set, o_set)
        | _ -> (g_set, Typing_set.add bound_ty o_set))
      lower_bounds
      (S_set.empty, Typing_set.empty)

  (** If it's clear from the syntax of the type that null isn't in ty, return true. *)
  let rec null_not_subtype env ty =
    match get_node ty with
    | Tprim (Aast_defs.Tnull | Aast_defs.Tvoid)
    | Tgeneric _
    | Tdynamic _
    | Tany _
    | Toption _
    | Tvar _
    | Taccess _
    | Tneg _
    | Tintersection _ ->
      false
    | Tunion tys -> List.for_all tys ~f:(null_not_subtype env)
    | Tclass _
    | Tprim _
    | Tnonnull
    | Tfun _
    | Ttuple _
    | Tshape _
    | Tlabel _
    | Tvec_or_dict _
    | Tclass_ptr _ ->
      true
    | Tnewtype (id, tyargs, _) ->
      let (env, bound) =
        Typing_utils.get_newtype_super env (get_reason ty) id tyargs
      in
      null_not_subtype env bound
    | Tdependent (_, bound) -> null_not_subtype env bound

  let simplify_subtype_by_physical_equality env ty_sub ty_super simplify =
    if
      (not (Typechecker_options.disable_physical_equality env.genv.tcopt))
      && phys_equal ty_sub ty_super
    then
      (env, TL.valid)
    else
      simplify ()

  let rec default_subtype
      ~subtype_env
      ~this_ty
      ~fail
      ~lhs:{ sub_supportdyn; ty_sub }
      ~rhs:{ super_like; ty_super; super_supportdyn }
      env =
    let (env, ty_super) = Env.expand_type env ty_super in
    let (env, ty_sub) = Env.expand_type env ty_sub in
    match deref ty_sub with
    | (_, Tvar _) -> begin
      match (subtype_env.Subtype_env.is_dynamic_aware, get_node ty_super) with
      | (true, Tdynamic _) ->
        let r = get_reason ty_super in
        let ty_super = MakeType.supportdyn_mixed r in
        default_subtype_inner
          ~subtype_env
          ~this_ty
          ~fail
          ~lhs:{ sub_supportdyn; ty_sub }
          ~rhs:{ super_like; super_supportdyn; ty_super }
          env
      | (true, _) ->
        mk_issubtype_prop
          ~sub_supportdyn
          ~is_dynamic_aware:true
          env
          (LoclType ty_sub)
          (LoclType ty_super)
      | (false, _) ->
        default_subtype_inner
          ~subtype_env
          ~this_ty
          ~fail
          ~lhs:{ sub_supportdyn; ty_sub }
          ~rhs:{ super_like; super_supportdyn; ty_super }
          env
    end
    | (r_sub, Tprim Nast.Tvoid) ->
      let r = Reason.implicit_upper_bound (Reason.to_pos r_sub, "mixed") in
      let prop =
        simplify
          ~subtype_env
          ~this_ty
          ~lhs:{ sub_supportdyn = None; ty_sub = MakeType.mixed r }
          ~rhs:{ super_like = false; super_supportdyn = false; ty_super }
      in
      if_unsat (invalid ~fail) @@ prop env
    | (_, Tany _) ->
      if subtype_env.Subtype_env.no_top_bottom then
        mk_issubtype_prop
          ~sub_supportdyn
          ~is_dynamic_aware:subtype_env.Subtype_env.is_dynamic_aware
          env
          (LoclType ty_sub)
          (LoclType ty_super)
      else
        valid env
    | _ ->
      default_subtype_inner
        ~subtype_env
        ~this_ty
        ~fail
        ~lhs:{ sub_supportdyn; ty_sub }
        ~rhs:{ super_like; super_supportdyn; ty_super }
        env

  and default_subtype_inner
      ~subtype_env
      ~this_ty
      ~fail
      ~lhs:{ sub_supportdyn; ty_sub = lty_sub }
      ~rhs:({ super_like; ty_super; _ } as rhs)
      env =
    match deref lty_sub with
    | (r_sub, Tunion tyl) ->
      let mk_prop ~subtype_env ~this_ty:_ ~lhs ~rhs env =
        simplify ~subtype_env ~this_ty:None ~lhs ~rhs env
      in
      Common.simplify_union_l
        ~subtype_env
        ~this_ty
        ~mk_prop
        ~update_reason:
          Typing_env.(
            update_reason ~f:(fun r_sub_prj ->
                Typing_reason.prj_union_sub ~sub:r_sub ~sub_prj:r_sub_prj))
        (sub_supportdyn, tyl)
        rhs
        env
    | (r_sub, Tvar id) -> begin
      (* Ensure that higher-ranked type parameters don't escape their scope *)
      let tvar_rank = Env.rank_of_tvar env id in
      match check_rank env ty_super tvar_rank with
      | Some (generic_reason, generic_name) ->
        let error =
          Typing_error.Secondary.Higher_rank_tparam_escape
            {
              tvar_pos = Typing_reason.to_pos r_sub;
              pos_with_generic = Typing_reason.to_pos (get_reason ty_super);
              generic_reason;
              generic_name;
            }
        in
        let fail = Subtype_env.fail_with_secondary_error subtype_env error in
        invalid ~fail env
      | _ ->
        (* For subtyping queries of the form
         *
         *   Tvar #id <: (Tvar #id | ...)
         *
         * `remove_tyvar_from_upper_bound` simplifies the union to
         * `mixed`. This indicates that the query is discharged. If we find
         * any other upper bound, we leave the subtyping query as it is.
         *)
        let (env, simplified_super_ty) =
          Typing_solver_utils.remove_tyvar_from_upper_bound
            env
            id
            (LoclType ty_super)
        in
        (* If the type is already in the upper bounds of the type variable,
         * then we already know that this subtype assertion is valid
         *)
        if ITySet.mem simplified_super_ty (Env.get_tyvar_upper_bounds env id)
        then
          valid env
        else
          let mixed = MakeType.mixed Reason.none in
          (match simplified_super_ty with
          | LoclType simplified_super_ty when ty_equal simplified_super_ty mixed
            ->
            valid env
          | _ ->
            mk_issubtype_prop
              ~sub_supportdyn
              ~is_dynamic_aware:subtype_env.Subtype_env.is_dynamic_aware
              env
              (LoclType lty_sub)
              (LoclType ty_super))
    end
    | (r_sub, Tintersection tyl) ->
      (* A & B <: C iif A <: C | !B *)
      (match Subtype_negation.find_type_with_exact_negation env tyl with
      | (env, Some non_ty, tyl) -> begin
        let (env, ty_super) = TUtils.union env ty_super non_ty in
        let ty_sub = MakeType.intersection r_sub tyl in
        simplify
          ~subtype_env
          ~this_ty:None
          ~lhs:{ sub_supportdyn; ty_sub }
          ~rhs:{ super_like = false; super_supportdyn = false; ty_super }
          env
      end
      | _ ->
        let mk_prop ~subtype_env ~this_ty:_ ~lhs ~rhs env =
          simplify ~subtype_env ~this_ty:None ~lhs ~rhs env
        in
        (* Otherwise use the incomplete common case which doesn't require inspection of the rhs *)
        Common.simplify_intersection_l
          ~subtype_env
          ~this_ty
          ~fail
          ~mk_prop
          ~update_reason:
            Typing_env.(
              update_reason ~f:(fun r_sub_prj ->
                  Typing_reason.prj_inter_sub ~sub:r_sub ~sub_prj:r_sub_prj))
          (sub_supportdyn, tyl)
          rhs
          env)
    | (_, Tgeneric _) when subtype_env.Subtype_env.require_completeness ->
      mk_issubtype_prop
        ~sub_supportdyn
        ~is_dynamic_aware:subtype_env.Subtype_env.is_dynamic_aware
        env
        (LoclType lty_sub)
        (LoclType ty_super)
    | (r_generic, Tgeneric name_sub) -> begin
      match deref ty_super with
      | (r_super, Tvar id)
        when Env.rank_of_tparam env name_sub > Env.rank_of_tvar env id ->
        let error =
          Typing_error.Secondary.Higher_rank_tparam_escape
            {
              tvar_pos = Typing_reason.to_pos r_super;
              pos_with_generic = Typing_reason.to_pos r_generic;
              generic_reason = r_generic;
              generic_name = Some name_sub;
            }
        in
        let fail = Subtype_env.fail_with_secondary_error subtype_env error in
        invalid ~fail env
      | _ ->
        (match
           VisitedGoals.try_add_visited_generic_sub
             subtype_env.Subtype_env.visited
             name_sub
             ty_super
         with
        | None ->
          (* If we've seen this type parameter before then we must have gone
               * round a cycle so we fail
          *)
          invalid ~fail env
        | Some new_visited -> begin
          let subtype_env = Subtype_env.set_visited subtype_env new_visited in

          let mk_prop ~subtype_env ~this_ty ~lhs ~rhs env =
            simplify ~subtype_env ~this_ty ~lhs ~rhs env
          in
          Common.simplify_generic_l
            ~subtype_env
            ~this_ty
            ~fail
            ~mk_prop
            (sub_supportdyn, r_generic, name_sub)
            { super_like; super_supportdyn = false; ty_super }
            { super_like = false; super_supportdyn = false; ty_super }
            env
        end)
    end
    | (_, Taccess _) -> invalid ~fail env
    | (r, Tnewtype (n, _tyl, ty)) ->
      let mk_prop ~subtype_env ~this_ty ~lhs ~rhs env =
        simplify ~subtype_env ~this_ty ~lhs ~rhs env
      in
      Common.simplify_newtype_l
        ~subtype_env
        ~this_ty
        ~mk_prop
        (sub_supportdyn, r, n, ty)
        rhs
        env (*  *)
    | (r, Tdependent (dep_ty, ty)) ->
      let mk_prop ~subtype_env ~this_ty ~lhs ~rhs env =
        simplify ~subtype_env ~this_ty ~lhs ~rhs env
      in
      Common.simplify_dependent_l
        ~subtype_env
        ~this_ty
        ~mk_prop
        (sub_supportdyn, r, dep_ty, ty)
        rhs
        env
    | _ -> invalid ~fail env

  and simplify_sub_union
      ~subtype_env
      ~sub_supportdyn
      ~this_ty
      ~super_like
      ~fail
      lty_sub
      (r_super, lty_supers)
      env =
    (* We *know* that the assertion is unsatisfiable *)
    let invalid_env env = invalid ~fail env in
    let default_subtype_help env =
      Subtype.(
        default_subtype
          ~subtype_env
          ~this_ty
          ~fail
          ~lhs:{ sub_supportdyn; ty_sub = lty_sub }
          ~rhs:
            {
              super_like;
              super_supportdyn = false;
              ty_super = mk (r_super, Tunion lty_supers);
            }
          env)
    in
    (* Identify cases heuristically where we just want to reduce t <: ~u to
       t <: u with super-like set, and not also try t <: dynamic or run finish *)
    let avoid_disjunctions env ty =
      let (env, ty) = Env.expand_type env ty in
      match (get_node lty_sub, get_node ty) with
      | (Tnewtype (n1, _, _), Tnewtype (n2, _, _))
        when String.equal n1 n2 && not (String.equal n1 SN.Classes.cSupportDyn)
        ->
        (env, true)
      | _ -> (env, false)
    in
    let finish env =
      match get_node lty_sub with
      | Tnewtype _
      | Tdependent _
      | Tgeneric _ ->
        default_subtype_help env
      | _ -> invalid_env env
    in
    let simplify_subtype_of_dynamic env =
      Subtype.(
        simplify
          ~subtype_env
          ~this_ty
          ~lhs:{ sub_supportdyn; ty_sub = lty_sub }
          ~rhs:
            {
              super_like = false;
              super_supportdyn = false;
              ty_super = MakeType.dynamic r_super;
            }
          env)
    in
    let dyn_finish ty env =
      let (env, avoid) = avoid_disjunctions env ty in
      if avoid then
        invalid_env env
      else
        let ( ||| ) = ( ||| ) ~fail in
        simplify_subtype_of_dynamic env ||| finish
    in
    let stripped_dynamic =
      Typing_dynamic_utils.try_strip_dynamic_from_union env lty_supers
    in
    match stripped_dynamic with
    | Some (ty_dynamic, tyl) ->
      (* If subtype is literally `dynamic` then we're done *)
      if is_dynamic lty_sub then
        valid env
      else
        let ty = MakeType.union r_super tyl in
        let (env, ty) = Env.expand_type env ty in
        let delay_push =
          Subtype_ask.is_sub_type_for_union_i
            env
            (LoclType ty)
            (LoclType (MakeType.supportdyn_mixed r_super))
        in
        (* This is Typing_logic_helpers.( ||| ) except with a bias towards p1 *)
        let ( ||| ) (env, p1) (f : env -> env * TL.subtype_prop) =
          if TL.is_valid p1 then
            (env, p1)
          else
            let (env, p2) = f env in
            if TL.is_unsat p2 then
              (env, p1)
            else if TL.is_unsat p1 then
              (env, p2)
            else
              (env, TL.disj ~fail p1 p2)
        in
        (* Implement the declarative subtyping rule C<~t1,...,~tn> <: ~C<t1,...,tn>
           * for a type C<t1,...,tn> that supports dynamic. Algorithmically,
           *   t <: ~C<t1,...,tn> iff
           *   t <: C<~t1,...,~tn> /\ C<~t1,...,~tn> <:D dynamic.
           * An SDT class C generalizes to other SDT constructors such as tuples and shapes.
        *)
        let try_push env =
          if delay_push then
            dyn_finish ty env
          else
            (* "Solve" type variables that are bounded from above and below by the same type.
             * Push this through nullables. This addresses common completeness issues that
             * bedevil like-pushing because of the disjunction that is generated.
             *)
            let rec solve_eq_tyvar env ty =
              let (env, ty) = Env.expand_type env ty in
              match get_node ty with
              | Tvar v ->
                let lower_bounds = Env.get_tyvar_lower_bounds env v in
                let (nulls, nonnulls) =
                  ITySet.partition
                    (fun ty ->
                      match ty with
                      | LoclType t -> is_prim Aast.Tnull t
                      | _ -> false)
                    lower_bounds
                in
                (* Make sure that lower bounds [null;t] intersects with ?t upper bound *)
                let lower_bounds =
                  if ITySet.is_empty nulls then
                    nonnulls
                  else
                    ITySet.map
                      (function
                        | LoclType t ->
                          LoclType (MakeType.nullable Reason.none t)
                        | ConstraintType t -> ConstraintType t)
                      nonnulls
                in
                let upper_bounds = Env.get_tyvar_upper_bounds env v in
                let bounds = ITySet.inter lower_bounds upper_bounds in
                let bounds_list = ITySet.elements bounds in
                begin
                  match bounds_list with
                  | [LoclType lty] -> (env, lty)
                  | _ -> (env, ty)
                end
              | Toption ty1 ->
                let (env, ty1) = solve_eq_tyvar env ty1 in
                (env, mk (get_reason ty, Toption ty1))
              | _ -> (env, ty)
            in
            let (env, ty) = solve_eq_tyvar env ty in
            (* For generic parameters with lower bounds, try like-pushing wrt
             * these lower bounds. For example, we want
             * vec<~int> <: ~T if vec<int> <: T
             *)
            let ty =
              match get_node ty with
              | Tgeneric name ->
                let bounds = Env.get_lower_bounds env name in
                MakeType.union (get_reason ty) (Typing_set.elements bounds)
              | _ -> ty
            in
            let (env, opt_ty) = Typing_dynamic.try_push_like env ty in
            match opt_ty with
            | None ->
              let istyvar =
                match get_node ty with
                | Tvar _ -> true
                | Toption ty ->
                  let (_, ty) = Env.expand_type env ty in
                  is_tyvar ty
                | _ -> false
              in
              if istyvar then
                simplify
                  ~subtype_env
                  ~this_ty
                  ~lhs:{ sub_supportdyn; ty_sub = lty_sub }
                  ~rhs:
                    {
                      super_like = true;
                      super_supportdyn = false;
                      ty_super = ty;
                    }
                  env
                ||| dyn_finish ty
              else
                dyn_finish ty env
            | Some ty ->
              let simplify_pushed_like env =
                simplify
                  ~subtype_env:
                    (Subtype_env.set_is_dynamic_aware subtype_env true)
                  ~this_ty
                  ~lhs:{ sub_supportdyn = None; ty_sub = ty }
                  ~rhs:
                    {
                      super_like = false;
                      super_supportdyn = false;
                      ty_super = ty_dynamic;
                    }
                  env
                &&& simplify
                      ~subtype_env:
                        { subtype_env with Subtype_env.ignore_likes = true }
                      ~this_ty
                      ~lhs:{ sub_supportdyn; ty_sub = lty_sub }
                      ~rhs:
                        {
                          super_like = false;
                          super_supportdyn = false;
                          ty_super = ty;
                        }
              in

              env |> simplify_pushed_like ||| dyn_finish ty
        in
        simplify
          ~subtype_env:
            { subtype_env with Subtype_env.ignore_likes = delay_push }
          ~this_ty
          ~lhs:{ sub_supportdyn; ty_sub = lty_sub }
          ~rhs:
            { super_like = delay_push; super_supportdyn = false; ty_super = ty }
          env
        ||| try_push
    | _ ->
      (* It's sound to reduce t <: t1 | t2 to (t <: t1) || (t <: t2). But
       * not complete e.g. consider (t1 | t3) <: (t1 | t2) | (t2 | t3).
       * But we deal with unions on the left first (see case above), so this
       * particular situation won't arise.
       * TODO: identify under what circumstances this reduction is complete.
       * TODO(T120921930): Don't do this if require_completeness is set.
       *)
      let rec aux ty_supers ~env ~errs ~props =
        match ty_supers with
        | [] ->
          ( env,
            TL.Disj
              ( Typing_error.intersect_opt @@ List.filter_opt errs,
                List.rev props ) )
        | ty_super :: ty_supers ->
          let ty_super =
            Typing_env.(
              update_reason env ty_super ~f:(fun r_super_prj ->
                  Typing_reason.prj_union_super
                    ~super:r_super
                    ~super_prj:r_super_prj))
          in

          let (env, prop) =
            simplify
              ~subtype_env
              ~this_ty
              ~lhs:{ sub_supportdyn; ty_sub = lty_sub }
              ~rhs:{ super_like; super_supportdyn = false; ty_super }
              env
          in
          if TL.is_valid prop then
            (env, prop)
          else
            let (errs, props) =
              match TL.get_error_if_unsat prop with
              | Some err -> (err :: errs, props)
              | _ -> (errs, prop :: props)
            in
            aux ty_supers ~env ~errs ~props
      in
      aux lty_supers ~env ~errs:[fail] ~props:[]

  (* == Function subtyping ================================================== *)
  and simplify_subtype_params_with_variadic
      ~subtype_env
      (r_super, idx_super, fn_param_supers)
      (r_sub, idx_sub, ty_sub)
      env =
    match fn_param_supers with
    | [] -> valid env
    | { fp_type = ty_super; _ } :: fn_param_supers ->
      let param_prop =
        let ty_super =
          Typing_env.update_reason env ty_super ~f:(fun r_super_prj ->
              Typing_reason.prj_fn_param
                ~super:r_super
                ~super_prj:r_super_prj
                ~sub:r_sub
                ~idx_sub
                ~idx_super)
        in
        simplify
          ~subtype_env
          ~this_ty:None
          ~lhs:{ sub_supportdyn = None; ty_sub = ty_super }
          ~rhs:
            { super_like = false; super_supportdyn = false; ty_super = ty_sub }
      in
      param_prop env
      &&& simplify_subtype_params_with_variadic
            ~subtype_env
            (r_super, idx_super + 1, fn_param_supers)
            (r_sub, idx_sub, ty_sub)

  and simplify_subtype_implicit_params
      ~subtype_env { capability = sub_cap } { capability = super_cap } env =
    let expected = Typing_coeffects.get_type sub_cap in
    let got = Typing_coeffects.get_type super_cap in
    let reasons =
      Typing_error.Secondary.Coeffect_subtyping
        {
          pos = get_pos got;
          cap = Typing_coeffects.pretty env got;
          pos_expected = get_pos expected;
          cap_expected = Typing_coeffects.pretty env expected;
        }
    in
    let subtype_env =
      Subtype_env.map_on_error subtype_env ~f:(fun on_error ->
          let err = Typing_error.apply_reasons ~on_error reasons in
          Typing_error.(Reasons_callback.always err))
    in
    match (sub_cap, super_cap) with
    | (CapTy sub, CapTy super) ->
      simplify
        ~subtype_env
        ~this_ty:None
        ~lhs:{ sub_supportdyn = None; ty_sub = sub }
        ~rhs:{ super_like = false; super_supportdyn = false; ty_super = super }
        env
    | (CapTy sub, CapDefaults _p) ->
      simplify
        ~subtype_env
        ~this_ty:None
        ~lhs:{ sub_supportdyn = None; ty_sub = sub }
        ~rhs:{ super_like = false; super_supportdyn = false; ty_super = got }
        env
    | (CapDefaults _p, CapTy super) ->
      simplify
        ~subtype_env
        ~this_ty:None
        ~lhs:{ sub_supportdyn = None; ty_sub = expected }
        ~rhs:{ super_like = false; super_supportdyn = false; ty_super = super }
        env
    | (CapDefaults _p1, CapDefaults _p2) -> valid env

  and simplify_supertype_params_with_variadic
      ~subtype_env
      (r_sub, idx_sub, fn_param_subs)
      (r_super, idx_super, ty_super)
      env =
    match fn_param_subs with
    | [] -> valid env
    | { fp_type = ty_sub; _ } :: fn_param_subs ->
      let param_prop =
        let ty_super =
          Typing_env.(
            update_reason env ty_super ~f:(fun r_super_prj ->
                Typing_reason.prj_fn_param
                  ~super:r_super
                  ~super_prj:r_super_prj
                  ~sub:r_sub
                  ~idx_sub
                  ~idx_super))
        in
        simplify
          ~subtype_env
          ~this_ty:None
          ~lhs:{ sub_supportdyn = None; ty_sub = ty_super }
          ~rhs:
            { super_like = false; super_supportdyn = false; ty_super = ty_sub }
      in
      param_prop env
      &&& simplify_supertype_params_with_variadic
            ~subtype_env
            (r_sub, idx_sub + 1, fn_param_subs)
            (r_super, idx_super, ty_super)

  and simplify_param_modes ~subtype_env ~fn_param_sub ~fn_param_super env =
    let { fp_pos = pos1; fp_name = name1; _ } = fn_param_super in
    let { fp_pos = pos2; fp_name = name2; _ } = fn_param_sub in
    (* Usually an arity check covers this case, except in the case that a splat parameter is preceded by an optional parameter *)
    if
      get_fp_is_optional fn_param_super
      && (not (get_fp_is_optional fn_param_sub))
      && Option.is_none name1
      && Option.is_none name2
    then
      invalid
        ~fail:
          (Option.map
             subtype_env.Subtype_env.on_error
             ~f:
               Typing_error.(
                 fun on_error ->
                   apply_reasons ~on_error
                   @@ Secondary.Fun_param_required_but_expected_optional
                        { pos = pos2; decl_pos = pos1; param_names = [] }))
        env
    else
      match (get_fp_mode fn_param_super, get_fp_mode fn_param_sub) with
      | (FPnormal, FPnormal)
      | (FPinout, FPinout) ->
        valid env
      | (FPnormal, FPinout) ->
        invalid
          ~fail:
            (Option.map
               subtype_env.Subtype_env.on_error
               ~f:
                 Typing_error.(
                   fun on_error ->
                     apply_reasons ~on_error
                     @@ Secondary.Inoutness_mismatch
                          { pos = pos2; decl_pos = pos1 }))
          env
      | (FPinout, FPnormal) ->
        invalid
          ~fail:
            (Option.map
               subtype_env.Subtype_env.on_error
               ~f:
                 Typing_error.(
                   fun on_error ->
                     apply_reasons ~on_error
                     @@ Secondary.Inoutness_mismatch
                          { pos = pos1; decl_pos = pos2 }))
          env

  and simplify_param_accept_disposable
      ~subtype_env ~fn_param_sub ~fn_param_super env =
    let { fp_pos = pos1; _ } = fn_param_super in
    let { fp_pos = pos2; _ } = fn_param_sub in
    match
      ( get_fp_accept_disposable fn_param_super,
        get_fp_accept_disposable fn_param_sub )
    with
    | (true, false) ->
      invalid
        ~fail:
          (Option.map
             subtype_env.Subtype_env.on_error
             ~f:
               Typing_error.(
                 fun on_error ->
                   apply_reasons ~on_error
                   @@ Secondary.Accept_disposable_invariant
                        { pos = pos1; decl_pos = pos2 }))
        env
    | (false, true) ->
      invalid
        ~fail:
          (Option.map
             subtype_env.Subtype_env.on_error
             ~f:
               Typing_error.(
                 fun on_error ->
                   apply_reasons ~on_error
                   @@ Secondary.Accept_disposable_invariant
                        { pos = pos2; decl_pos = pos1 }))
        env
    | (_, _) -> valid env

  and readonly_subtype (r_sub : bool) (r_super : bool) =
    match (r_sub, r_super) with
    | (true, false) ->
      false (* A readonly value is a supertype of a mutable one *)
    | _ -> true

  and simplify_param_readonly ~subtype_env ~fn_param_sub ~fn_param_super env =
    let { fp_pos = pos1; _ } = fn_param_super in
    let { fp_pos = pos2; _ } = fn_param_sub in
    if
      (not subtype_env.Subtype_env.ignore_readonly)
      && (not (get_fp_ignore_readonly_error fn_param_sub))
      && (not (get_fp_ignore_readonly_error fn_param_super))
      && not
           (readonly_subtype
              (get_fp_readonly fn_param_super)
              (get_fp_readonly fn_param_sub))
    then
      invalid
        ~fail:
          (Option.map
             subtype_env.Subtype_env.on_error
             ~f:
               Typing_error.(
                 fun on_error ->
                   apply_reasons ~on_error
                   @@ Secondary.Readonly_mismatch
                        {
                          pos = pos1;
                          kind = `param;
                          reason_sub =
                            lazy [(pos2, "This parameter is mutable")];
                          reason_super =
                            lazy [(pos1, "But this parameter is readonly")];
                        }))
        env
    else
      valid env

  (* Helper function for subtyping on function types: performs all checks that
   * don't involve actual types:
   *   <<__ReturnDisposable>> attribute
   *   variadic arity
   *  <<__Policied>> attribute
   *  Readonlyness
   *)
  and simplify_subtype_funs_attributes
      ~subtype_env (r_sub, ft_sub) (r_super, ft_super) =
    let p_sub = Reason.to_pos r_sub in
    let p_super = Reason.to_pos r_super in
    let splat_super =
      match List.last ft_super.ft_params with
      | Some fp -> get_fp_splat fp
      | None -> false
    in
    (* Readonly this is contravariant, so check ft_super_ro <: ft_sub_ro *)
    let readonly_this_err =
      if
        (not subtype_env.Subtype_env.ignore_readonly)
        && not
             (readonly_subtype
                (get_ft_readonly_this ft_super)
                (get_ft_readonly_this ft_sub))
      then
        Error
          (Option.map
             subtype_env.Subtype_env.on_error
             ~f:
               Typing_error.(
                 fun on_error ->
                   apply_reasons ~on_error
                   @@ Secondary.Readonly_mismatch
                        {
                          pos = p_sub;
                          kind = `fn;
                          reason_sub =
                            lazy
                              [(p_sub, "This function is not marked readonly")];
                          reason_super =
                            lazy [(p_super, "This function is marked readonly")];
                        }))
      else
        Ok ()
      (* Readonly return is covariant, so check ft_sub <: ft_super *)
    and readonly_ret_err =
      if
        (not subtype_env.Subtype_env.ignore_readonly)
        && not
             (readonly_subtype
                (get_ft_returns_readonly ft_sub)
                (get_ft_returns_readonly ft_super))
      then
        Error
          (Option.map
             subtype_env.Subtype_env.on_error
             ~f:
               Typing_error.(
                 fun on_error ->
                   apply_reasons ~on_error
                   @@ Secondary.Readonly_mismatch
                        {
                          pos = p_sub;
                          kind = `fn_return;
                          reason_sub =
                            lazy
                              [
                                (p_sub, "This function returns a readonly value");
                              ];
                          reason_super =
                            lazy
                              [
                                ( p_super,
                                  "This function does not return a readonly value"
                                );
                              ];
                        }))
      else
        Ok ()
    and return_disposable_err =
      if
        not
          (Bool.equal
             (get_ft_return_disposable ft_sub)
             (get_ft_return_disposable ft_super))
      then
        Error
          (Option.map
             subtype_env.Subtype_env.on_error
             ~f:
               Typing_error.(
                 fun on_error ->
                   apply_reasons ~on_error
                   @@ Secondary.Return_disposable_mismatch
                        {
                          pos_super = p_super;
                          pos_sub = p_sub;
                          is_marked_return_disposable =
                            get_ft_return_disposable ft_super;
                        }))
      else
        Ok ()
    and arity_required_err =
      let (arity_required_sub, _sub_required_named_params) =
        arity_and_names_required ft_sub
      in
      let (arity_required_super, _super_required_named_params) =
        arity_and_names_required ft_super
      in
      if (not splat_super) && not (arity_required_sub <= arity_required_super)
      then
        Error
          (Option.map
             subtype_env.Subtype_env.on_error
             ~f:
               Typing_error.(
                 fun on_error ->
                   apply_reasons ~on_error
                   @@ Secondary.Fun_too_many_args
                        {
                          expected = arity_required_super;
                          actual = arity_required_sub;
                          pos = p_sub;
                          decl_pos = p_super;
                        }))
      else
        Ok ()
    and named_params_err =
      let Typing_named_params.
            {
              extra_names;
              missing_names;
              too_optional_names;
              missing_named_variadic;
            } =
        Typing_named_params.find_names_mismatch
          ~actual_ft:ft_sub
          ~expected_ft:ft_super
      in
      (* If the expected type has a named-variadic parameter but the actual
         does not, surface it as a "missing named argument: ..." error
         (matching the naming convention used by decl_hint.ml when it lowers
         the named-variadic to a param with fp_name = Some "..."). *)
      let missing_names =
        if missing_named_variadic then
          "..." :: missing_names
        else
          missing_names
      in
      if
        not
          (List.is_empty extra_names
          && List.is_empty missing_names
          && List.is_empty too_optional_names)
      then
        Error
          (Option.map
             subtype_env.Subtype_env.on_error
             ~f:
               Typing_error.(
                 fun on_error ->
                   let apply_if_nonempty names f : Error.t list =
                     if List.is_empty names then
                       []
                     else
                       [apply_reasons ~on_error (f ())]
                   in
                   multiple
                     (apply_if_nonempty extra_names (fun () ->
                          Secondary.Fun_extra_named_args
                            { extra_names; pos = p_sub; decl_pos = p_super })
                     @ apply_if_nonempty missing_names (fun () ->
                           Secondary.Fun_missing_named_args
                             { missing_names; pos = p_sub; decl_pos = p_super })
                     @ apply_if_nonempty too_optional_names (fun () ->
                           Secondary.Fun_param_required_but_expected_optional
                             {
                               param_names = too_optional_names;
                               pos = p_sub;
                               decl_pos = p_super;
                             }))))
      else
        Ok ()
    and variadic_err =
      let positional_variadic_of ft =
        if Typing_defs.ft_has_positional_variadic ft then
          List.last (Typing_defs.ft_params_without_named_variadic ft)
        else
          None
      in
      let ft_sub_variadic = positional_variadic_of ft_sub in
      let ft_super_variadic = positional_variadic_of ft_super in

      match (ft_sub_variadic, ft_super_variadic) with
      | (Some { fp_name = None; _ }, Some { fp_name = Some _; _ }) ->
        (* The HHVM runtime ignores "..." entirely, but knows about
         * "...$args"; for contexts for which the runtime enforces method
         * compatibility (currently, inheritance from abstract/interface
         * methods), letting "..." override "...$args" would result in method
         * compatibility errors at runtime. *)
        Error
          (Option.map
             subtype_env.Subtype_env.on_error
             ~f:
               Typing_error.(
                 fun on_error ->
                   apply_reasons ~on_error
                   @@ Secondary.Fun_variadicity_hh_vs_php56
                        { pos = p_sub; decl_pos = p_super }))
      | (None, None) when not splat_super ->
        let count_positional_params ft =
          List.count ft.ft_params ~f:(Fn.non Typing_defs_core.get_fp_is_named)
        in
        let sub_max = count_positional_params ft_sub in
        let super_max = count_positional_params ft_super in
        if sub_max < super_max then
          Error
            (Option.map
               subtype_env.Subtype_env.on_error
               ~f:
                 Typing_error.(
                   fun on_error ->
                     apply_reasons ~on_error
                     @@ Secondary.Fun_too_few_args
                          {
                            pos = p_sub;
                            decl_pos = p_super;
                            expected = super_max;
                            actual = sub_max;
                          }))
        else
          Ok ()
      | (None, Some _) ->
        Error
          (Option.map
             subtype_env.Subtype_env.on_error
             ~f:
               Typing_error.(
                 fun on_error ->
                   apply_reasons ~on_error
                   @@ Secondary.Fun_unexpected_nonvariadic
                        { pos = p_sub; decl_pos = p_super }))
      | (_, _) -> Ok ()
    in
    (* Collect attribute errors *)
    let (_, errs) =
      List.partition_result
        [
          readonly_this_err;
          readonly_ret_err;
          return_disposable_err;
          arity_required_err;
          named_params_err;
          variadic_err;
        ]
    in
    (* If we have no errors, return valid *)
    if List.is_empty errs then
      TL.valid
    else
      (* Otherwise, combine the errors and return invalid *)
      TL.invalid ~fail:(Typing_error.multiple_opt @@ List.filter_opt errs)

  (**
    Construct a tuple type from the parameters in a function type.
    This is used for subtyping function types at least one of which contains
    a splat parameter e.g. consider

      (function(t1', _, tm', ...T):r') <:
      (function(t1, _, tm, u1, _, un, optional v1, _, vk, w...):r <:

    (Here ...T is a splat parameter and w... is a variadic).

    We need to subtype t1<:t1', _, tm<:tm' and r'<:r as normal but
    splat parameter T is compared a tuple of the remaining parameters in the supertype
    i.e. (u1, _, un, optional v1, _, optional vk, w...) <: T

    If there is a splat parameter on both sides, e.g.
      (function(t1',_,tm', ...T'):r') <:
      (function(t1, _, tm, u1, _, un, ...T):r)

    then we use splat in a tuple:

      (u1, _, un, ...T) <: T'
   *)
  and params_to_tuple fun_type_pos is_variadic fn_params =
    (* We construct a position that spans all the parameters that we are gathering.
     * If there are no parameters, just use the position from the function type *)
    let acc_pos =
      match fn_params with
      | [] -> fun_type_pos
      | { fp_pos; fp_type; _ } :: _ ->
        Pos_or_decl.merge (get_pos fp_type) fp_pos
    in
    let rec aux
        fn_params acc_pos acc_required acc_optional acc_variadic acc_splat =
      match fn_params with
      | [] ->
        mk
          ( Reason.tuple_from_splat acc_pos,
            Ttuple
              {
                t_required = List.rev acc_required;
                t_optional = List.rev acc_optional;
                t_extra =
                  (match acc_splat with
                  | None -> Tvariadic acc_variadic
                  | Some t -> Tsplat t);
              } )
      | ({ fp_type; fp_pos; _ } as fn_param) :: fn_params ->
        let acc_pos =
          if List.is_empty fn_params then
            Pos_or_decl.merge acc_pos fp_pos
          else
            acc_pos
        in
        let (acc_required, acc_optional, acc_variadic, acc_splat) =
          if List.is_empty fn_params && is_variadic then
            (acc_required, acc_optional, fp_type, acc_splat)
          else if List.is_empty fn_params && get_fp_splat fn_param then
            (acc_required, acc_optional, acc_variadic, Some fp_type)
          else if get_fp_is_optional fn_param then
            (acc_required, fp_type :: acc_optional, acc_variadic, acc_splat)
          else
            (fp_type :: acc_required, acc_optional, acc_variadic, acc_splat)
        in
        aux fn_params acc_pos acc_required acc_optional acc_variadic acc_splat
    in
    aux
      fn_params
      acc_pos
      []
      []
      (MakeType.nothing (Reason.witness_from_decl fun_type_pos))
      None

  and simplify_subtype_params
      ~(subtype_env : Subtype_env.t)
      ~arg_posl
      ~for_override
      (r_sub, idx_sub, all_params_sub, variadic_sub_ty)
      (r_super, idx_super, all_params_super, variadic_super_ty)
      env =
    (* We use this for matching parameters fn_param_sub and fn_param_super
       * Below we match using two separate strategies: by name (for named params) and by position
    *)
    let simplify_subtype_for_matching_parameters
        ~arg_pos subtype_env fn_param_sub fn_param_super env =
      let { fp_type = ty_sub; _ } = fn_param_sub
      and { fp_type = ty_super; _ } = fn_param_super in
      (* Construct the subtype proposition for the two parameters; function
         paramaters are contravariant unless they are marked with `inout`
         in which case they are typed as invariant *)
      let subty_prop =
        let subtype_env_for_param =
          (* When overriding in Sound Dynamic, we treat any dynamic-aware subtype of dynamic as a
           * subtype of the dynamic type itself
           *)
          match get_node ty_sub with
          | Tdynamic _ when for_override ->
            Subtype_env.set_is_dynamic_aware subtype_env true
          | _ -> subtype_env
        in
        let subtype_env_for_param =
          match arg_pos with
          | Some pos ->
            Subtype_env.map_on_error subtype_env_for_param ~f:(fun on_error ->
                Typing_error.Reasons_callback.With_claim
                  (on_error, lazy (pos, "Invalid argument")))
          | None -> subtype_env_for_param
        in
        let subtype_env_for_param =
          if
            get_fp_ignore_readonly_error fn_param_sub
            || get_fp_ignore_readonly_error fn_param_super
          then
            Subtype_env.{ subtype_env_for_param with ignore_readonly = true }
          else
            subtype_env_for_param
        in
        (* Construct the contravariant subtype proposition *)
        let subty_prop_contra is_inout =
          (* Update the provenance on the super type:
             i) Reverse the direction of flow for the containing function-type reasons
             ii) Record the projection into the ith function arg, treated as contravariant, of that function-type
             iii) Record the flow from the supertype into that arg *)
          let ty_super =
            Typing_env.update_reason env ty_super ~f:(fun r_super_prj ->
                if is_inout then
                  Typing_reason.prj_fn_param_inout_contra
                    ~super:r_super
                    ~super_prj:r_super_prj
                    ~sub:r_sub
                    ~idx_sub
                    ~idx_super
                else
                  Typing_reason.prj_fn_param
                    ~super:r_super
                    ~super_prj:r_super_prj
                    ~sub:r_sub
                    ~idx_sub
                    ~idx_super)
          in
          simplify
            ~subtype_env:subtype_env_for_param
            ~this_ty:None
            ~lhs:{ sub_supportdyn = None; ty_sub = ty_super }
            ~rhs:
              {
                super_like = false;
                super_supportdyn = false;
                ty_super = ty_sub;
              }
        in
        (* For parameters with the `inout` calling convention also construct
           the covariant subtype proposition *)
        match (get_fp_mode fn_param_sub, get_fp_mode fn_param_super) with
        | (FPinout, FPinout) ->
          let subty_prop_co =
            (* Update the provenance for the subtype:
               i) Record the flow from the parent function types
               ii) Record the projection in the ith arg, treated as covariant, of that function
               iii) Record the flow from the subtype into that arg *)
            let ty_sub =
              Typing_env.update_reason env ty_sub ~f:(fun r_sub_prj ->
                  Typing_reason.prj_fn_param_inout_co
                    ~sub:r_sub
                    ~sub_prj:r_sub_prj
                    ~super:r_super
                    ~idx_sub
                    ~idx_super)
            in
            simplify
              ~subtype_env:subtype_env_for_param
              ~this_ty:None
              ~lhs:{ sub_supportdyn = None; ty_sub }
              ~rhs:{ super_like = false; super_supportdyn = false; ty_super }
          in
          (fun env -> subty_prop_contra true env &&& subty_prop_co)
        | _ -> (fun env -> subty_prop_contra false env)
      in
      (* Check that the calling conventions of the params are compatible. *)
      env
      |> simplify_param_modes ~subtype_env ~fn_param_sub ~fn_param_super
      &&& simplify_param_readonly ~subtype_env ~fn_param_sub ~fn_param_super
      &&& simplify_param_accept_disposable
            ~subtype_env
            ~fn_param_sub
            ~fn_param_super
      &&& subty_prop
    in
    let partition_by_namedness =
      List.partition_tf ~f:(fun fp ->
          Option.is_some (Named_params.name_of_named_param fp))
    in
    let (named_params_sub, positional_params_sub) =
      partition_by_namedness all_params_sub
    in
    let (named_params_super, positional_params_super) =
      partition_by_namedness all_params_super
    in
    (* Handle named parameters *)
    let (env, prop) =
      let name_to_named_param_super =
        List.fold named_params_super ~init:String.Map.empty ~f:(fun acc fp ->
            match Named_params.name_of_named_param fp with
            | Some name -> begin
              match Map.add ~key:name ~data:fp acc with
              | `Ok acc -> acc
              | `Duplicate -> acc
            end
            | None -> acc)
      in
      List.fold
        named_params_sub
        ~init:(env, TL.valid)
        ~f:(fun (env, prop) fp_sub ->
          let open Option.Let_syntax in
          Option.value ~default:(env, prop)
          @@ let* name = Named_params.name_of_named_param fp_sub in
             let+ fp_super = Map.find name_to_named_param_super name in
             (* we can safely ignore when no matching named parameter is found, since arity checks happen elsewhere *)
             (env, prop)
             &&& simplify_subtype_for_matching_parameters
                   ~arg_pos:None
                   subtype_env
                   fp_sub
                   fp_super)
    in
    (env, prop)
    &&&
    match (positional_params_sub, positional_params_super) with
    (* When either list runs out, we still have to typecheck that
       the remaining portion sub/super types with the other's variadic.
       For example, if
       ChildClass {
         public function a(int $x = 0, string ... $args) // superl = [int], super_var = string
       }
       overrides
       ParentClass {
         public function a(string ... $args) // subl = [], sub_var = string
       }
       , there should be an error because the first argument will be checked against
       int, not string that is, ChildClass::a("hello") would crash,
       but ParentClass::a("hello") wouldn't.

       Similarly, if th{ subtype_env with Subtype_env.ignore_likes = delay_push }e other list is longer, aka
       ChildClass  extends ParentClass {
         public function a(mixed ... $args) // superl = [], super_var = mixed
       }
       overrides
       ParentClass {
         //subl = [string], sub_var = string
         public function a(string $x = 0, string ... $args)
       }
       It should also check that string is a subtype of mixed.
    *)
    (* A lone variadic against a lone splat must package the variadic side
       into a tuple, e.g. `string...` against `...(string...)`. Comparing the
       splat tuple against the variadic element instead would be unsound:
       `(arraykey)...` against `...(arraykey)` passes reflexively, yet the
       implementation receives scalars where it declares 1-tuples. *)
    | ([_], [({ fp_type = ty_super; _ } as fp_super)])
      when variadic_sub_ty && get_fp_splat fp_super ->
      let tuple_ty_sub =
        params_to_tuple
          (Reason.to_pos r_sub)
          variadic_sub_ty
          positional_params_sub
      in
      simplify
        ~subtype_env
        ~this_ty:None
        ~lhs:{ sub_supportdyn = None; ty_sub = ty_super }
        ~rhs:
          {
            super_like = false;
            super_supportdyn = false;
            ty_super = tuple_ty_sub;
          }
    | ([({ fp_type = ty_sub; _ } as fp_sub)], [_])
      when variadic_super_ty && get_fp_splat fp_sub ->
      let tuple_ty =
        params_to_tuple
          (Reason.to_pos r_super)
          variadic_super_ty
          positional_params_super
      in
      simplify
        ~subtype_env
        ~this_ty:None
        ~lhs:{ sub_supportdyn = None; ty_sub = tuple_ty }
        ~rhs:{ super_like = false; super_supportdyn = false; ty_super = ty_sub }
    | ([{ fp_type = ty_sub; _ }], _) when variadic_sub_ty ->
      simplify_subtype_params_with_variadic
        ~subtype_env
        (r_super, idx_super, positional_params_super)
        (r_sub, idx_sub, ty_sub)
    | (_, [{ fp_type = ty_super; _ }]) when variadic_super_ty ->
      simplify_supertype_params_with_variadic
        ~subtype_env
        (r_sub, idx_sub, positional_params_sub)
        (r_super, idx_super, ty_super)
    (* Two splat parameters are just compared directly *)
    | ( [({ fp_type = ty_sub; _ } as fn_param_sub)],
        [({ fp_type = ty_super; _ } as fn_param_super)] )
      when get_fp_splat fn_param_sub && get_fp_splat fn_param_super ->
      simplify
        ~subtype_env
        ~this_ty:None
        ~lhs:{ sub_supportdyn = None; ty_sub = ty_super }
        ~rhs:{ super_like = false; super_supportdyn = false; ty_super = ty_sub }
    (* If supertype is a splat parameter then package up remaining parameters in subtype as
     * a tuple and compare that
     *)
    | (_, [({ fp_type = ty_super; _ } as fn_param_super)])
      when get_fp_splat fn_param_super ->
      let tuple_ty_sub =
        params_to_tuple
          (Reason.to_pos r_sub)
          variadic_sub_ty
          positional_params_sub
      in
      simplify
        ~subtype_env
        ~this_ty:None
        ~lhs:{ sub_supportdyn = None; ty_sub = ty_super }
        ~rhs:
          {
            super_like = false;
            super_supportdyn = false;
            ty_super = tuple_ty_sub;
          }
    (* If subtype is a splat parameter then package up remaining parameters in supertype as
     * a tuple and compare that
     *)
    | ([({ fp_type = ty_sub; _ } as fn_param_sub)], _)
      when get_fp_splat fn_param_sub ->
      let tuple_ty =
        params_to_tuple
          (Reason.to_pos r_super)
          variadic_super_ty
          positional_params_super
      in
      simplify
        ~subtype_env
        ~this_ty:None
        ~lhs:{ sub_supportdyn = None; ty_sub = tuple_ty }
        ~rhs:{ super_like = false; super_supportdyn = false; ty_super = ty_sub }
      (* We've run out of parameters *)
    | ([], _ :: _) ->
      let splat_super =
        match List.last positional_params_super with
        | Some fp -> get_fp_splat fp
        | None -> false
      in
      if splat_super then
        let fail =
          Option.map
            subtype_env.Subtype_env.on_error
            ~f:
              Typing_error.(
                fun on_error ->
                  apply_reasons ~on_error
                  @@ Secondary.Fun_too_few_args
                       {
                         pos = Reason.to_pos r_sub;
                         decl_pos = Reason.to_pos r_super;
                         actual = idx_sub;
                         expected = List.length positional_params_super;
                       })
        in
        invalid ~fail
      else
        valid
    | (_, []) -> valid
    | ( fn_param_sub :: positional_params_sub,
        fn_param_super :: positional_params_super ) ->
      let (arg_pos, arg_posl) =
        match arg_posl with
        | None
        | Some [] ->
          (None, None)
        | Some (p :: ps) -> (Some p, Some ps)
      in
      fun env ->
        simplify_subtype_for_matching_parameters
          ~arg_pos
          subtype_env
          fn_param_sub
          fn_param_super
          env
        &&& simplify_subtype_params
              ~subtype_env
              ~arg_posl
              ~for_override
              (r_sub, idx_sub + 1, positional_params_sub, variadic_sub_ty)
              ( r_super,
                idx_super + 1,
                positional_params_super,
                variadic_super_ty )

  (* Named-variadic subtyping between two function types. In each case, check
     both contravariant element types and parameter attributes. Three cases:

     1. If the actual (sub) function has a named-variadic and the expected
        (super) function declares named parameters that the sub does not
        declare by name, the sub's variadic absorbs them — but the super's
        declared param type must be a subtype of the sub's variadic element
        type (contravariance).

     2. If the expected function has a named-variadic and the actual function
        has an optional named parameter not declared by the expected function,
        the expected variadic's element type must be a subtype of that explicit
        parameter's type. The explicit parameter shadows the actual function's
        variadic for that name.

     3. If both sides have a named-variadic, their element types must
        subtype contravariantly (super's element <: sub's element).

     The "missing named-variadic on sub" and "extra required name on sub"
     cases are handled in [simplify_subtype_funs_attributes] via
     [Typing_named_params.find_names_mismatch]. *)
  and simplify_subtype_named_variadic
      ~subtype_env (r_sub, ft_sub) (r_super, ft_super) env =
    let sub_var = Typing_defs.ft_named_variadic_param ft_sub in
    let super_var = Typing_defs.ft_named_variadic_param ft_super in
    let names_of ft =
      List.fold
        (Typing_defs.ft_params_without_named_variadic ft)
        ~init:S_set.empty
        ~f:(fun acc fp ->
          match Typing_defs.Named_params.name_of_named_param fp with
          | Some name -> S_set.add name acc
          | None -> acc)
    in
    let sub_named_names = names_of ft_sub in
    let super_named_names = names_of ft_super in
    let contra ty_super ty_sub env =
      let ty_super =
        Typing_env.update_reason env ty_super ~f:(fun r_super_prj ->
            Typing_reason.prj_fn_param
              ~super:r_super
              ~super_prj:r_super_prj
              ~sub:r_sub
              ~idx_sub:0
              ~idx_super:0)
      in
      simplify
        ~subtype_env
        ~this_ty:None
        ~lhs:{ sub_supportdyn = None; ty_sub = ty_super }
        ~rhs:{ super_like = false; super_supportdyn = false; ty_super = ty_sub }
        env
    in
    let simplify_parameter_attributes ~fn_param_sub ~fn_param_super env =
      env
      |> simplify_param_readonly ~subtype_env ~fn_param_sub ~fn_param_super
      &&& simplify_param_accept_disposable
            ~subtype_env
            ~fn_param_sub
            ~fn_param_super
    in
    (* Each unmatched named parameter is absorbed by the subtype's variadic. *)
    let (env, prop) =
      match sub_var with
      | None -> valid env
      | Some ({ fp_type = ty_sub_var; _ } as sub_var_fp) ->
        List.fold
          (Typing_defs.ft_params_without_named_variadic ft_super)
          ~init:(valid env)
          ~f:(fun (env, prop) super_fp ->
            match Typing_defs.Named_params.name_of_named_param super_fp with
            | Some name when not (S_set.mem name sub_named_names) ->
              (env, prop)
              &&& simplify_parameter_attributes
                    ~fn_param_sub:sub_var_fp
                    ~fn_param_super:super_fp
              &&& contra super_fp.fp_type ty_sub_var
            | _ -> (env, prop))
    in
    (* An explicit optional parameter in the sub shadows its named variadic.
       If the super only accepts that name through its variadic, check against
       the explicit parameter's type instead of the sub's variadic type. *)
    let (env, prop) =
      match (sub_var, super_var) with
      | (Some _, Some ({ fp_type = ty_super_var; _ } as super_var_fp)) ->
        List.fold
          (Typing_defs.ft_params_without_named_variadic ft_sub)
          ~init:(env, prop)
          ~f:(fun (env, prop) sub_fp ->
            match Typing_defs.Named_params.name_of_named_param sub_fp with
            | Some name
              when Typing_defs_core.get_fp_is_optional sub_fp
                   && not (S_set.mem name super_named_names) ->
              (env, prop)
              &&& simplify_parameter_attributes
                    ~fn_param_sub:sub_fp
                    ~fn_param_super:super_var_fp
              &&& contra ty_super_var sub_fp.fp_type
            | _ -> (env, prop))
      | _ -> (env, prop)
    in
    (env, prop)
    &&&
    match (sub_var, super_var) with
    | ( Some ({ fp_type = ty_sub_var; _ } as sub_var_fp),
        Some ({ fp_type = ty_super_var; _ } as super_var_fp) ) ->
      fun env ->
        simplify_parameter_attributes
          ~fn_param_sub:sub_var_fp
          ~fn_param_super:super_var_fp
          env
        &&& contra ty_super_var ty_sub_var
    | _ -> (fun env -> valid env)

  and simplify_funs
      ~subtype_env
      ~check_return
      ~for_override
      ~super_like
      (r_sub, ft_sub)
      (r_super, ft_super)
      env =
    (* We should always have instantiated polymorphic function types by this point *)
    let (_ : unit) =
      let (_ : unit) =
        if not ft_sub.ft_instantiated then
          Diagnostics.internal_error
            (Pos_or_decl.unsafe_to_raw_pos @@ Typing_reason.to_pos r_sub)
            "Unexpected polymorphic function type"
        else
          ()
      in
      if not ft_super.ft_instantiated then
        Diagnostics.internal_error
          (Pos_or_decl.unsafe_to_raw_pos @@ Typing_reason.to_pos r_super)
          "Unexpected polymorphic function type"
      else
        ()
    in
    let arg_posl = subtype_env.Subtype_env.has_member_arg_posl in
    let subtype_env =
      { subtype_env with Subtype_env.has_member_arg_posl = None }
    in
    (* First apply checks on attributes and variadic arity *)
    let simplify_subtype_implicit_params_help =
      simplify_subtype_implicit_params ~subtype_env
    in
    ( env,
      simplify_subtype_funs_attributes
        ~subtype_env
        (r_sub, ft_sub)
        (r_super, ft_super) )
    &&& (* Now do contravariant subtyping on parameters. The named-variadic
           entry (if present) is stripped here and handled separately below —
           it is not a required parameter and must not participate in either
           positional matching or name-by-name matching. *)
    begin
      simplify_subtype_params
        ~subtype_env
        ~arg_posl
        ~for_override
        ( r_sub,
          0,
          Typing_defs.ft_params_without_named_variadic ft_sub,
          Typing_defs.ft_has_positional_variadic ft_sub )
        ( r_super,
          0,
          Typing_defs.ft_params_without_named_variadic ft_super,
          Typing_defs.ft_has_positional_variadic ft_super )
    end
    &&& simplify_subtype_named_variadic
          ~subtype_env
          (r_sub, ft_sub)
          (r_super, ft_super)
    &&& simplify_subtype_implicit_params_help
          ft_super.ft_implicit_params
          ft_sub.ft_implicit_params
    &&&
    (* Finally do covariant subtyping on return type *)
    if check_return then
      let ty_super = Sd.liken ~super_like env ft_super.ft_ret in
      let subtype_env =
        if for_override then
          (* When overriding in Sound Dynamic, we allow t to override dynamic if
           * t is a dynamic-aware subtype of dynamic. We also allow Awaitable<t>
           * to override Awaitable<dynamic> and and Awaitable<t> to
           * override ~Awaitable<dynamic>.
           *)
          let (_, ty_super) = Typing_dynamic_utils.strip_dynamic env ty_super in
          match get_node ty_super with
          | Tdynamic _ -> Subtype_env.set_is_dynamic_aware subtype_env true
          | Tclass ((_, class_name), _, [ty])
            when String.equal class_name SN.Classes.cAwaitable && is_dynamic ty
            ->
            Subtype_env.set_is_dynamic_aware subtype_env true
          | _ -> subtype_env
        else
          subtype_env
      in
      (* Update the provenance for the subtype:
         i) Record the flow from the parent function types
         ii) Record the projection on the return type of that function
         iii) Record the flow from the subtype into that return type *)
      let ty_sub =
        Typing_env.update_reason env ft_sub.ft_ret ~f:(fun r_sub_prj ->
            Typing_reason.prj_fn_ret
              ~sub:r_sub
              ~sub_prj:r_sub_prj
              ~super:r_super)
      in
      simplify
        ~subtype_env
        ~this_ty:None
        ~lhs:{ sub_supportdyn = None; ty_sub }
        ~rhs:{ super_like = false; super_supportdyn = false; ty_super }
    else
      valid

  (* == Class / newtype subtyping =========================================== *)
  and simplify_subtype_variance_for_injective_loop
      ~(subtype_env : Subtype_env.t)
      ~(sub_supportdyn : Reason.t option)
      ~super_like
      nm
      (ctor_kind : Typing_reason.ctor_kind)
      (idx : int)
      (variance_reifiedl : (Ast_defs.variance * Aast.reify_kind) list)
      (r_sub, children_tyl)
      (r_super, super_tyl)
      env =
    let simplify_subtype_help ~sub_supportdyn ty_sub ty_super env =
      simplify
        ~subtype_env
        ~this_ty:None
        ~lhs:{ sub_supportdyn; ty_sub }
        ~rhs:{ super_like = false; super_supportdyn = false; ty_super }
        env
    in

    match (variance_reifiedl, children_tyl, super_tyl) with
    | ([], _, _)
    | (_, [], _)
    | (_, _, []) ->
      valid env
    | ( (variance, _reify_kind) :: variance_reifiedl,
        child :: childrenl,
        super :: superl ) ->
      let prop env =
        match variance with
        | Ast_defs.Covariant ->
          let ty_sub =
            Typing_env.update_reason env child ~f:(fun r_sub_prj ->
                Typing_reason.prj_ctor_co
                  ~sub:r_sub
                  ~sub_prj:r_sub_prj
                  ~super:r_super
                  ctor_kind
                  nm
                  idx
                  false)
          and ty_super = Sd.liken ~super_like env super in
          simplify_subtype_help ~sub_supportdyn ty_sub ty_super env
        | Ast_defs.Contravariant ->
          let ty_super =
            map_reason super ~f:(fun r_super_prj ->
                Typing_reason.prj_ctor_contra
                  ~sub:r_sub
                  ~super:r_super
                  ~super_prj:r_super_prj
                  ctor_kind
                  nm
                  idx
                  false)
          and ty_sub = child in
          simplify_subtype_help ~sub_supportdyn ty_super ty_sub env
        | Ast_defs.Invariant ->
          let covariant_prop env =
            let ty_sub =
              Typing_env.update_reason env child ~f:(fun r_sub_prj ->
                  Typing_reason.prj_ctor_co
                    ~sub:r_sub
                    ~sub_prj:r_sub_prj
                    ~super:r_super
                    ctor_kind
                    nm
                    idx
                    true)
            and ty_super = Sd.liken ~super_like env super in
            simplify_subtype_help ~sub_supportdyn ty_sub ty_super env
          and contravariant_prop env =
            let f r_super_prj =
              Typing_reason.prj_ctor_contra
                ~sub:r_sub
                ~super:r_super
                ~super_prj:r_super_prj
                ctor_kind
                nm
                idx
                true
            in
            let ty_sub = Sd.liken ~super_like env child
            and ty_super = map_reason super ~f in
            simplify_subtype_help ~sub_supportdyn ty_super ty_sub env
          in
          covariant_prop env &&& contravariant_prop
      in
      prop env
      &&& simplify_subtype_variance_for_injective_loop
            ~subtype_env
            ~sub_supportdyn
            ~super_like
            nm
            ctor_kind
            (idx + 1)
            variance_reifiedl
            (r_sub, childrenl)
            (r_super, superl)

  and simplify_subtype_variance_for_injective
      ~(subtype_env : Subtype_env.t)
      ~(sub_supportdyn : Reason.t option)
      ~super_like
      cid
      ctor_kind
      (class_sub : Cls.t option) =
    (* Before looping through the generic arguments, check to see if we should push
       supportdyn onto them. This depends on the generic class itself. *)
    let sub_supportdyn =
      match (sub_supportdyn, class_sub) with
      | (None, _)
      | (_, None) ->
        None
      | (Some _, Some class_sub) ->
        if
          String.equal cid SN.Collections.cTraversable
          || String.equal cid SN.Collections.cKeyedTraversable
          || String.equal cid SN.Collections.cContainer
          || Cls.has_ancestor class_sub SN.Collections.cContainer
        then
          sub_supportdyn
        else
          None
    in
    simplify_subtype_variance_for_injective_loop
      ~subtype_env
      ~sub_supportdyn
      ~super_like
      cid
      ctor_kind
      0

  and simplify_subtype_classes
      ~fail
      ~(subtype_env : Subtype_env.t)
      ~(sub_supportdyn : Reason.t option)
      ~(this_ty : locl_ty option)
      ~(super_like : bool)
      (r_sub, (class_id_sub, exact_sub, tyl_sub))
      (r_super, (class_id_super, exact_super, tyl_super))
      env : env * TL.subtype_prop =
    let invalid_env = invalid ~fail in
    let (cid_super, cid_sub) = (snd class_id_super, snd class_id_sub) in
    let (exact_match, both_exact) =
      match (exact_sub, exact_super) with
      | (Nonexact _, Exact) -> (false, false)
      | (Exact, Exact) -> (true, true)
      | (_, _) -> (true, false)
    in
    if String.equal cid_super cid_sub then
      if List.is_empty tyl_sub && List.is_empty tyl_super && exact_match then
        valid env
      else
        (* This is side-effecting as it registers a dependency *)
        let class_def_sub = Env.get_class env cid_sub in
        (* If class is final then exactness is superfluous *)
        let (has_generics, is_final) =
          match class_def_sub with
          | Decl_entry.Found tc ->
            (not (List.is_empty (Cls.tparams tc)), Cls.final tc)
          | Decl_entry.DoesNotExist
          | Decl_entry.NotYetAvailable ->
            (false, false)
        in
        if not (exact_match || is_final) then
          invalid_env env
        else if has_generics && List.is_empty tyl_super then
          (* C<t> <: C where C represents all possible instantiations of C's generics *)
          valid env
        else if has_generics && List.is_empty tyl_sub then
          (* C </: C<t>, since C's generic can be instantiated to other things than t *)
          invalid_env env
        else
          let variance_reifiedl =
            if List.is_empty tyl_sub then
              []
            else if both_exact then
              (* Subtyping exact class types following variance
               * annotations is unsound in general (see T142810099).
               * When the class is exact, we must treat all generic
               * parameters as invariant.
               *)
              List.map tyl_sub ~f:(fun _ -> (Ast_defs.Invariant, Aast.Erased))
            else
              match class_def_sub with
              | Decl_entry.DoesNotExist
              | Decl_entry.NotYetAvailable ->
                List.map tyl_sub ~f:(fun _ -> (Ast_defs.Invariant, Aast.Erased))
              | Decl_entry.Found class_sub ->
                List.map (Cls.tparams class_sub) ~f:(fun t ->
                    (t.tp_variance, t.tp_reified))
          in
          simplify_subtype_variance_for_injective
            ~subtype_env
            ~sub_supportdyn
            ~super_like
            cid_sub
            Typing_reason.Ctor_class
            (Decl_entry.to_option class_def_sub)
            variance_reifiedl
            (r_sub, tyl_sub)
            (r_super, tyl_super)
            env
    else if not exact_match then
      invalid_env env
    else
      let class_def_sub = Env.get_class env cid_sub in
      match class_def_sub with
      | Decl_entry.DoesNotExist
      | Decl_entry.NotYetAvailable ->
        (* This should have been caught already in the naming phase *)
        valid env
      | Decl_entry.Found class_sub ->
        (* We handle the case where a generic A<T> is used as A for the sub-class.
           This works because there will be no locls to substitute for type parameters
           T in the type build by get_ancestor. If T does show up in that type, then
           the call to simplify subtype will fail. This is what we expect since we
           would need it to be a sub-type of the super-type for all T. If T is not there,
           then simplify_subtype should succeed. *)
        let ety_env =
          {
            empty_expand_env with
            substs = TUtils.make_locl_subst_for_class_tparams class_sub tyl_sub;
            (* FIXME(T59448452): Unsound in general *)
            this_ty =
              Option.value
                this_ty
                ~default:(mk (r_sub, Tclass (class_id_sub, exact_sub, tyl_sub)));
          }
        in
        let up_obj = Cls.get_ancestor class_sub cid_super in
        (match up_obj with
        | Some up_obj ->
          (* Since we have provided no `Typing_error.Reasons_callback.t`
           * in the `expand_env`, this will not generate any errors *)
          let ((env, _ty_err_opt), up_obj) =
            Phase.localize ~ety_env env up_obj
          in
          let up_obj =
            Typing_env.update_reason env up_obj ~f:(fun ancestor ->
                Typing_reason.axiom_extends ~child:r_sub ~ancestor)
          in

          (match deref up_obj with
          | (r_sub, Tclass (class_id_sub, exact_sub, tyl_sub)) ->
            let subtype_env =
              Subtype_env.possibly_add_violated_constraint
                subtype_env
                ~r_sub
                ~r_super
            in
            let ity_super =
              LoclType
                (mk (r_super, Tclass (class_id_super, exact_super, tyl_super)))
            in
            let ity_sub = LoclType up_obj in
            let fail =
              Subtype_env.fail subtype_env ~ty_sub:ity_sub ~ty_super:ity_super
            in
            simplify_subtype_classes
              ~fail
              ~subtype_env
              ~sub_supportdyn
              ~this_ty
              ~super_like
              (r_sub, (class_id_sub, exact_sub, tyl_sub))
              (r_super, (class_id_super, exact_super, tyl_super))
              env
          | _ ->
            (* The superclass must be classish *)
            invalid env ~fail)
        | None ->
          if
            Ast_defs.is_c_trait (Cls.kind class_sub)
            || Ast_defs.is_c_interface (Cls.kind class_sub)
          then
            let reqs_non_strict =
              List.map
                (Cls.all_ancestor_req_constraints_requirements class_sub)
                ~f:(fun cr -> snd (to_requirement cr))
            in
            let rec try_upper_bounds_on_this up_objs env =
              match up_objs with
              | [] ->
                (* It's crucial that we don't lose updates to tpenv in
                 * env that were introduced by Phase.localize.
                 * TODO: avoid this requirement *)
                invalid_env env
              | ub_obj_typ :: up_objs
                when List.mem reqs_non_strict ub_obj_typ ~equal:equal_decl_ty ->
                (* `require class` constraints do not induce subtyping,
                 * so skipping them *)
                try_upper_bounds_on_this up_objs env
              | ub_obj_typ :: up_objs ->
                (* A trait is never the runtime type, but it can be used
                 * as a constraint if it has requirements for its using classes *)
                (* Since we have provided no `Typing_error.Reasons_callback.t`
                 * in the `expand_env`, this will not generate any errors *)
                let ((env, _ty_err_opt), ub_obj_typ) =
                  Phase.localize ~ety_env env ub_obj_typ
                in
                let prop env =
                  match deref ub_obj_typ with
                  | (_, (Tclass (class_id_sub, exact_sub, tyl_sub) as cls_sub))
                    ->
                    let ity_super =
                      LoclType
                        (mk
                           ( r_super,
                             Tclass (class_id_super, exact_super, tyl_super) ))
                    in
                    (* Retain reason / position for original subclass, not the upper bound *)
                    let ity_sub = LoclType (mk (r_sub, cls_sub)) in
                    let fail =
                      Subtype_env.fail
                        subtype_env
                        ~ty_sub:ity_sub
                        ~ty_super:ity_super
                    in
                    simplify_subtype_classes
                      ~fail
                      ~subtype_env
                      ~sub_supportdyn
                      ~this_ty
                      ~super_like
                      (r_sub, (class_id_sub, exact_sub, tyl_sub))
                      (r_super, (class_id_super, exact_super, tyl_super))
                      env
                  | _ -> invalid env ~fail
                in
                let ( ||| ) = ( ||| ) ~fail in

                env |> prop ||| try_upper_bounds_on_this up_objs
            in
            try_upper_bounds_on_this (Cls.upper_bounds_on_this class_sub) env
          else
            invalid_env env)

  and simplify_subtype_variance_for_non_injective
      ~subtype_env
      ~sub_supportdyn
      ~super_like
      cid
      class_sub
      (variance_reifiedl : (Ast_defs.variance * Aast.reify_kind) list)
      (r_sub, children_tyl)
      (r_super, super_tyl)
      lty_sub
      lty_super
      env =
    let ((env, p) as res) =
      simplify_subtype_variance_for_injective
        ~subtype_env
        ~sub_supportdyn
        ~super_like
        cid
        Typing_reason.Ctor_newtype
        class_sub
        variance_reifiedl
        (r_sub, children_tyl)
        (r_super, super_tyl)
        env
    in

    if subtype_env.Subtype_env.require_completeness && not (TL.is_valid p) then
      (* If we require completeness, then we can still use the incomplete
       * N<t1, .., tn> <: N<u1, .., un> to t1 <:v1> u1 /\ ... /\ tn <:vn> un
       * simplification if all of the latter constraints already hold.
       * If they don't already hold, there is nothing we can (soundly) simplify. *)
      if subtype_env.Subtype_env.require_soundness then
        mk_issubtype_prop
          ~sub_supportdyn
          ~is_dynamic_aware:subtype_env.Subtype_env.is_dynamic_aware
          env
          (LoclType lty_sub)
          (LoclType lty_super)
      else
        (env, TL.valid)
    else
      res

  (* == Shape splat subtyping =============================================== *)

  (* Entry point for shape-splat subtyping  Normalize both shapes then dispatch
     on presence of type variables:
     - no tyvar: ground corner procedure;
     - sub/super only: single-side inference;
     - one-each: couple via a fresh mid row;
     - multiple on both sides: the decoupled fallback. *)
  and simplify_subtype_shape_splat
      ~(subtype_env : Subtype_env.t)
      ~(env : env)
      ~(this_ty : locl_ty option)
      ~super_like
      ((supportdyn_sub, r_sub, shape_type_sub) :
        bool * Typing_reason.t * locl_phase shape_type)
      ((supportdyn_super, r_super, shape_type_super) :
        bool * Typing_reason.t * locl_phase shape_type) =
    let (env, err_opt, norm_sub, norm_super) =
      let on_error = subtype_env.Subtype_env.on_error in
      let (env, err_sub, norm_sub) =
        Typing_shape_normalize.Row.normalize ~on_error r_sub shape_type_sub env
      in
      let (env, err_super, norm_super) =
        Typing_shape_normalize.Row.normalize
          ~on_error
          r_super
          shape_type_super
          env
      in
      let err_opt = Option.merge err_sub err_super ~f:Typing_error.both in
      (env, err_opt, norm_sub, norm_super)
    in
    (* Create an empty cache for the corners procedure *)
    let cache = Typing_corners.Cache.create () in
    match err_opt with
    | Some _ as fail -> invalid ~fail env
    | None ->
      let sub_supportdyn_reason =
        if supportdyn_sub then
          Some r_sub
        else
          None
      in
      let simplify_normalized env norm_sub norm_super =
        let (env, ty_sub) =
          Typing_shape_normalize.Row.normalized_to_ty env ~reason:r_sub norm_sub
        in
        let (env, ty_super) =
          Typing_shape_normalize.Row.normalized_to_ty
            env
            ~reason:r_super
            norm_super
        in
        simplify_
          ~subtype_env
          ~this_ty
          ~lhs:{ sub_supportdyn = sub_supportdyn_reason; ty_sub }
          ~rhs:{ super_supportdyn = supportdyn_super; super_like; ty_super }
          env
      in
      (match
         ( Typing_shape_normalize.Row.as_row norm_sub,
           Typing_shape_normalize.Row.as_row norm_super )
       with
      | (None, _)
      | (_, None) ->
        simplify_normalized env norm_sub norm_super
      | (Some norm_sub, Some norm_super) ->
        let simple_sub = Typing_shape_normalize.Row.as_simple norm_sub
        and simple_super = Typing_shape_normalize.Row.as_simple norm_super in
        let super_is_top_shape =
          match simple_super with
          | Some { s_fields; s_unknown_value; s_origin = _ } ->
            TShapeMap.is_empty s_fields && TUtils.is_mixed env s_unknown_value
          | None -> false
        in
        if (not supportdyn_super) && super_is_top_shape then
          (* [shape(...)] is the top row. Its [supportdyn] counterpart still
             imposes dynamic-support obligations on every field. *)
          valid env
        else if Typing_shape_normalize.Row.is_bottom norm_sub then
          valid env
        else if Typing_shape_normalize.Row.is_bottom norm_super then
          (* A non-bottom subrow may still be opaque and constrained to [nothing]. *)
          let ty_sub =
            Typing_shape_normalize.Row.to_ty ~reason:r_sub norm_sub
          in
          let ty_super =
            Typing_shape_normalize.Row.to_ty ~reason:r_super norm_super
          in
          simplify_
            ~subtype_env
            ~this_ty
            ~lhs:{ sub_supportdyn = None; ty_sub }
            ~rhs:{ super_supportdyn = false; super_like; ty_super }
            env
        else
          let sv_sub = Typing_corners.spread_tyvar_ids norm_sub
          and sv_super = Typing_corners.spread_tyvar_ids norm_super in
          (match (sv_sub, sv_super) with
          | (_ :: _ :: _, _)
            when subtype_env.Subtype_env.report_ambiguous_shape_splat ->
            (env, TL.AmbiguousShapeSplat sv_sub)
          | ([], []) ->
            (match (simple_sub, simple_super) with
            | ( Some
                  {
                    s_origin = origin_sub;
                    s_unknown_value = kind_sub;
                    s_fields = fdm_sub;
                  },
                Some
                  {
                    s_origin = origin_super;
                    s_unknown_value = kind_super;
                    s_fields = fdm_super;
                  } ) ->
              if same_type_origin origin_super origin_sub then
                (* Same origin fast path *)
                valid env
              else
                (* Dispatch to simple shape subtyping *)
                simplify_subtype_shape
                  ~subtype_env
                  ~env
                  ~this_ty
                  ~super_like
                  (supportdyn_sub, r_sub, kind_sub, fdm_sub)
                  (supportdyn_super, r_super, kind_super, fdm_super)
            | _ ->
              (* At least one type is a splat and neither have tyvars so use the
                 ground corners procedure *)
              subrow_splat
                ~subtype_env
                ~env
                ~cache
                ~this_ty
                ~super_like
                ~r:r_sub
                ~sub:norm_sub
                ~super:norm_super)
          | ([], _ :: _) ->
            (* At least one type variable in the supertype, none in the subtype *)
            subrow_infer_super
              ~subtype_env
              ~env
              ~cache
              ~this_ty
              ~super_like
              ~r:r_sub
              ~sub:norm_sub
              ~super:norm_super
              sv_super
          | (_ :: _, []) ->
            (* At least one type variable in the subtype, none in the supertype *)
            subrow_infer_sub
              ~subtype_env
              ~env
              ~cache
              ~this_ty
              ~super_like
              ~r:r_sub
              ~sub:norm_sub
              ~super:norm_super
              sv_sub
          | ([sub_v], [super_v]) ->
            (* Exactly one tyvar in each of the sub- and supertypes; solve using
               a fresh coupled tyvar acting as upper bound for one and lower bound
               for the other. *)
            subrow_infer_couple
              ~subtype_env
              ~env
              ~cache
              ~this_ty
              ~super_like
              ~r:r_sub
              ~sub:norm_sub
              ~sub_spread_var:sub_v
              ~super:norm_super
              ~super_spread_var:super_v
          | (_ :: _, _ :: _) ->
            (* Multiple spread vars on at least one side: decoupled fallback.
               Resolve the other side's spread vars to their current solutions,
               then run single-side inference each way. *)
            let (env, solved_super) =
              Typing_corners.solve_spread_vars env r_sub norm_super
            in
            let (env, prop_sub) =
              match Typing_shape_normalize.Row.as_row solved_super with
              | Some super' ->
                subrow_infer_sub
                  ~subtype_env
                  ~env
                  ~cache
                  ~this_ty
                  ~super_like
                  ~r:r_sub
                  ~sub:norm_sub
                  ~super:super'
                  sv_sub
              | None ->
                let ty_sub =
                  Typing_shape_normalize.Row.to_ty ~reason:r_sub norm_sub
                in
                let (env, ty_super) =
                  Typing_shape_normalize.Row.normalized_to_ty
                    env
                    ~reason:r_super
                    solved_super
                in
                simplify_
                  ~subtype_env
                  ~this_ty
                  ~lhs:{ sub_supportdyn = sub_supportdyn_reason; ty_sub }
                  ~rhs:
                    {
                      super_supportdyn = supportdyn_super;
                      super_like;
                      ty_super;
                    }
                  env
            in
            let (env, solved_sub) =
              Typing_corners.solve_spread_vars env r_sub norm_sub
            in
            let (env, prop_super) =
              match Typing_shape_normalize.Row.as_row solved_sub with
              | Some sub' ->
                subrow_infer_super
                  ~subtype_env
                  ~env
                  ~cache
                  ~this_ty
                  ~super_like
                  ~r:r_sub
                  ~sub:sub'
                  ~super:norm_super
                  sv_super
              | None ->
                let (env, ty_sub) =
                  Typing_shape_normalize.Row.normalized_to_ty
                    env
                    ~reason:r_sub
                    solved_sub
                in
                let ty_super =
                  Typing_shape_normalize.Row.to_ty ~reason:r_super norm_super
                in
                simplify_
                  ~subtype_env
                  ~this_ty
                  ~lhs:{ sub_supportdyn = sub_supportdyn_reason; ty_sub }
                  ~rhs:
                    {
                      super_supportdyn = supportdyn_super;
                      super_like;
                      ty_super;
                    }
                  env
            in
            (env, TL.conj prop_sub prop_super)))

  (* -- Splat subtyping ----------------------------------------------------- *)

  (* At least one type is a splat and neither type contains tyvars; apply
     the ground corners procedure per label *)
  and subrow_splat
      ~(subtype_env : Subtype_env.t)
      ~(env : env)
      ~(cache : Typing_corners.Cache.t)
      ~(this_ty : locl_ty option)
      ~super_like
      ~r
      ~(sub : Typing_shape_normalize.Row.t)
      ~(super : Typing_shape_normalize.Row.t) =
    let labels = Typing_corners.subrow_labels cache env ~sub ~super r in
    List.fold_left labels ~init:(valid env) ~f:(fun acc label ->
        acc &&& fun env ->
        subrow_splat_at
          ~subtype_env
          ~env
          ~cache
          ~this_ty
          ~super_like
          ~r
          ~sub
          ~super
          label)

  and subrow_splat_at
      ~(subtype_env : Subtype_env.t)
      ~(env : env)
      ~(cache : Typing_corners.Cache.t)
      ~(this_ty : locl_ty option)
      ~super_like
      ~r
      ~(sub : Typing_shape_normalize.Row.t)
      ~(super : Typing_shape_normalize.Row.t)
      label =
    let live_super =
      let open Typing_corners in
      Splat_elem.Set.of_list (row_live_spread_at super label)
    in
    (* Attribute field failures to a named supertype spread elem live at this label. *)
    let blame =
      List.find_map
        (Typing_corners.Splat_elem.Set.elements live_super)
        ~f:(fun ty ->
          match get_node ty with
          | Tgeneric name
          | Tnewtype (name, _, _) ->
            Some (get_pos ty, name)
          | _ -> None)
    in
    (* Attribute unknown-tail failures only when checking the tail projection. *)
    let tail_blame =
      match label with
      | Some _ -> None
      | None -> blame
    in
    let f env ~sub ~super =
      subrow_subfield
        ~subtype_env
        ~env
        ~this_ty
        ~super_like
        ~r
        ~tail_blame
        ~blame
        label
        ~sub
        ~super
    in
    (* Check subfields at all corner assignments *)
    let (env, result) =
      Typing_corners.check_subrow_corners
        cache
        env
        ~sub
        ~super
        label
        r
        ~init:valid
        ~conj:( &&& )
        ~f
    in
    match result with
    | Ok (Typing_corners.Computed prop) -> (env, prop)
    | Ok Typing_corners.Empty -> valid env
    | Ok (Typing_corners.Unsupported_cycle info) ->
      report_shape_splat_failure
        subtype_env
        ~r
        ~sub
        ~super
        env
        (Unsupported_cycle info)
    | Error missing ->
      report_shape_splat_failure
        subtype_env
        ~r
        ~sub
        ~super
        env
        (Missing_assignment missing)

  and subrow_subfield
      ~subtype_env
      ~env
      ~this_ty
      ~super_like
      ~r
      ~(tail_blame : (Pos_or_decl.t * string) option)
      ~(blame : (Pos_or_decl.t * string) option)
      (label : TShapeField.t option)
      ~(sub : locl_phase shape_field_type)
      ~(super : locl_phase shape_field_type) =
    match label with
    | Some field_name ->
      (* Map a field descriptor to a shape projection: an [Opt nothing] field
         denotes an absent label *)
      let to_proj (fd : locl_phase shape_field_type) =
        if fd.sft_optional && Typing_defs.is_nothing fd.sft_ty then
          `Absent
        else if fd.sft_optional then
          `Optional fd.sft_ty
        else
          `Required fd.sft_ty
      in
      let on_type_mismatch =
        match blame with
        | None -> None
        | Some (decl_pos, name) ->
          Option.map subtype_env.Subtype_env.on_error ~f:(fun on_error ->
              Typing_error.(
                Reasons_callback.prepend_on_apply
                  on_error
                  (Secondary.Splat_field_not_known
                     {
                       pos = Reason.to_pos r;
                       decl_pos;
                       name;
                       field = TUtils.get_printable_shape_field_name field_name;
                     })))
      in
      (match
         simplify_subtype_shape_projection
           ?on_type_mismatch
           ~subtype_env
           ~this_ty
           ~super_like
           ~env
           (false, r, to_proj sub)
           (false, r, to_proj super)
           field_name
       with
      | Ok f -> f env
      | Error fail -> invalid ~fail env)
    | None ->
      if Typing_corners.Field.requiredness_lte ~sub ~super then
        simplify_
          ~subtype_env
          ~this_ty
          ~lhs:{ sub_supportdyn = None; ty_sub = sub.sft_ty }
          ~rhs:{ super_supportdyn = false; super_like; ty_super = super.sft_ty }
          env
      else
        let (decl_pos, name) =
          match tail_blame with
          | Some (pos, name) -> (pos, Some name)
          | None -> (Reason.to_pos r, None)
        in
        let fail =
          Option.map
            subtype_env.Subtype_env.on_error
            ~f:
              Typing_error.(
                fun on_error ->
                  apply_reasons ~on_error
                  @@ Secondary.Splat_may_require_fields
                       { pos = Reason.to_pos r; decl_pos; name })
        in
        invalid ~fail env

  (* -- Splat inference ----------------------------------------------------- *)

  (* Infer lower bounds for the super row's spread type variables. The sub row
     has no unsolved spread variables, so its projected fields determine what
     each super type variable must accept. *)
  and subrow_infer_super
      ~(subtype_env : Subtype_env.t)
      ~(env : env)
      ~(cache : Typing_corners.Cache.t)
      ~(this_ty : locl_ty option)
      ~super_like
      ~r
      ~(sub : Typing_shape_normalize.Row.t)
      ~(super : Typing_shape_normalize.Row.t)
      (super_spread_vars : Tvid.t list) =
    List.fold_left super_spread_vars ~init:(valid env) ~f:(fun acc v ->
        acc &&& fun env ->
        match Typing_corners.partition_at_tyvar super v with
        | None -> valid env
        | Some (pre_elems, post_elems) ->
          let (env, super_pre, super_post) =
            let on_error = subtype_env.Subtype_env.on_error in
            let (env, _e1, super_pre) =
              let shape_ty = Shape_splat { ss_elems = pre_elems } in
              Typing_shape_normalize.Row.normalize ~on_error r shape_ty env
            in
            let (env, _e2, super_post) =
              let shape_ty = Shape_splat { ss_elems = post_elems } in
              Typing_shape_normalize.Row.normalize ~on_error r shape_ty env
            in
            (env, super_pre, super_post)
          in
          (match
             ( Typing_shape_normalize.Row.as_row super_pre,
               Typing_shape_normalize.Row.as_row super_post )
           with
          | (Some super_pre, Some super_post) ->
            if
              (not (Typing_shape_normalize.Row.is_bottom super_pre))
              && not (Typing_shape_normalize.Row.is_bottom super_post)
            then
              match
                subrow_infer_super_part
                  ~subtype_env
                  ~env
                  ~cache
                  ~this_ty
                  ~super_like
                  ~r
                  ~spread_var_ty:(mk (r, Tvar v))
                  ~sub
                  ~super_pre
                  ~super_post
                  ~super
              with
              | Ok (env, prop) -> (env, prop)
              | Error (env, failure) ->
                report_shape_splat_failure
                  subtype_env
                  ~r
                  ~sub
                  ~super
                  env
                  failure
            else
              valid env
          | (None, _)
          | (_, None) ->
            (* A partition cannot currently distribute a union. Keep this
               conservative fallback if that invariant changes. *)
            invalid ~fail:None env))

  (* Build a shape lower bound for the spread variable, using fresh type variables
     for fields it may contribute. Generate per-field constraints relating the
     subtype's fields to those variables and to fields contributed by the prefix
     and suffix. Fields fully masked by the suffix are checked directly and do
     not constrain the spread variable. *)
  and subrow_infer_super_part
      ~(subtype_env : Subtype_env.t)
      ~(env : env)
      ~(cache : Typing_corners.Cache.t)
      ~(this_ty : locl_ty option)
      ~super_like
      ~r
      ~(spread_var_ty : locl_ty)
      ~(sub : Typing_shape_normalize.Row.t)
      ~(super_pre : Typing_shape_normalize.Row.t)
      ~(super_post : Typing_shape_normalize.Row.t)
      ~(super : Typing_shape_normalize.Row.t) =
    let labels = Typing_corners.subrow_labels cache env ~sub ~super r in
    let open Hh_prelude.Result.Let_syntax in
    let* (env, known, unknown, props) =
      List.fold_result
        labels
        ~init:(env, TShapeMap.empty, MakeType.nothing r, [])
        ~f:(fun (env, known, unknown, props) label ->
          let* (env, (field, prop)) =
            subrow_infer_super_part_at
              ~subtype_env
              ~env
              ~cache
              ~this_ty
              ~super_like
              ~r
              ~sub
              ~super_pre
              ~super_post
              ~super
              label
          in
          let (known, unknown) =
            match label with
            | Some lbl -> (TShapeMap.add lbl field known, unknown)
            | None -> (known, field.sft_ty)
          in
          return (env, known, unknown, prop :: props))
    in
    let bound =
      Typing_shape_normalize.Row.of_simple
        {
          s_origin = Missing_origin;
          s_unknown_value = unknown;
          s_fields = known;
        }
    in
    let bound_ty = Typing_shape_normalize.Row.to_ty ~reason:r bound in
    let (env, prop_resid) =
      simplify_
        ~subtype_env
        ~this_ty
        ~lhs:{ sub_supportdyn = None; ty_sub = bound_ty }
        ~rhs:{ super_supportdyn = false; super_like; ty_super = spread_var_ty }
        env
    in
    return (env, List.fold_left props ~init:prop_resid ~f:TL.conj)

  (* Infer [label]'s field in the spread variable's lower-bound shape and generate
     the constraints needed at that label for every corner assignment. Where the
     spread variable is live, use a fresh field type and constrain the subtype
     field against the rightmost-wins merge of [super_pre], that fresh field, and
     [super_post]. Where [super_post] masks the variable, check the subtype field
     directly against [super_post]. If the variable need not contribute the field
     in any case, represent that lack of a requirement as an absent field *)
  and subrow_infer_super_part_at
      ~(subtype_env : Subtype_env.t)
      ~(env : env)
      ~(cache : Typing_corners.Cache.t)
      ~(this_ty : locl_ty option)
      ~super_like
      ~r
      ~(sub : Typing_shape_normalize.Row.t)
      ~(super_pre : Typing_shape_normalize.Row.t)
      ~(super_post : Typing_shape_normalize.Row.t)
      ~(super : Typing_shape_normalize.Row.t)
      label =
    let absent = { sft_optional = true; sft_ty = MakeType.nothing r } in
    let live_names =
      let open Typing_corners in
      Splat_elem.Set.union
        (Splat_elem.Set.of_list (row_live_spread_at sub label))
        (Splat_elem.Set.of_list (row_live_spread_at super label))
    in
    let open Hh_prelude.Result.Let_syntax in
    let* (env, assignments) =
      shape_splat_assignments cache env live_names label r
    in
    match assignments with
    | None -> return (env, (absent, TL.valid))
    | Some assignments ->
      let* (env, info) =
        List.fold_result
          assignments
          ~init:(env, [])
          ~f:(fun (env, acc) assignment ->
            let* (env, fd_post) =
              project_shape_splat env super_post label assignment
            in
            let* (env, fd_sub) = project_shape_splat env sub label assignment in
            return
              ( env,
                (Typing_corners.Field.is_required fd_post, fd_sub, assignment)
                :: acc ))
      in
      let info = List.rev info in
      let live = List.filter info ~f:(fun (masked, _, _) -> not masked) in
      (match live with
      | [] ->
        (* the spread tyvar is overwritten by [super_post] everywhere: it
           contributes nothing to the lower bound at this label. The field is absent,
           optional-[nothing], not required-[nothing], which would make the
           inferred row uninhabited. *)
        let* (env, prop) =
          List.fold_result
            info
            ~init:(env, TL.valid)
            ~f:(fun acc (_, fd_sub, assignment) ->
              shape_splat_conj acc (fun env ->
                  let* (env, fd_super) =
                    project_shape_splat env super_post label assignment
                  in
                  let (env, prop) =
                    subrow_subfield
                      ~subtype_env
                      ~env
                      ~this_ty
                      ~super_like
                      ~r
                      ~tail_blame:None
                      ~blame:None
                      label
                      ~sub:fd_sub
                      ~super:fd_super
                  in
                  return (env, prop)))
        in
        return (env, (absent, prop))
      | _ ->
        let is_opt =
          List.exists live ~f:(fun (_, fd_sub, _) ->
              Typing_corners.Field.is_optional fd_sub)
        in
        (* A field which contributes `nothing` must not appear as required since
           this means the shape is uninhabited *)
        let* (env, contributes_nothing) =
          List.fold_result
            live
            ~init:(env, true)
            ~f:(fun (env, discharged) (_, fd_sub, assignment) ->
              if not discharged then
                return (env, false)
              else if Typing_corners.Field.is_absent fd_sub env then
                (* [nothing] fits under anything; no need to probe *)
                return (env, true)
              else
                let* (env, fd_post) =
                  project_shape_splat env super_post label assignment
                in
                let* (env, base) =
                  if is_opt then
                    let* (env, fd_pre) =
                      project_shape_splat env super_pre label assignment
                    in
                    let (env, base) =
                      Typing_union.union env fd_pre.sft_ty fd_post.sft_ty
                    in
                    return (env, base)
                  else
                    return (env, fd_post.sft_ty)
                in
                let (_env, p) =
                  simplify_
                    ~subtype_env
                    ~this_ty
                    ~lhs:{ sub_supportdyn = None; ty_sub = fd_sub.sft_ty }
                    ~rhs:
                      { super_supportdyn = false; super_like; ty_super = base }
                    env
                in
                return (env, TL.is_valid p))
        in
        let (env, flex) = Env.fresh_type env Pos.none in
        let* (env, prop) =
          List.fold_result
            info
            ~init:(env, TL.valid)
            ~f:(fun acc (masked, fd_sub, assignment) ->
              shape_splat_conj acc (fun env ->
                  if masked then
                    let* (env, fd_super) =
                      project_shape_splat env super_post label assignment
                    in
                    let (env, prop) =
                      subrow_subfield
                        ~subtype_env
                        ~env
                        ~this_ty
                        ~super_like
                        ~r
                        ~tail_blame:None
                        ~blame:None
                        label
                        ~sub:fd_sub
                        ~super:fd_super
                    in
                    return (env, prop)
                  else
                    let* (env, fd_pre) =
                      project_shape_splat env super_pre label assignment
                    in
                    if
                      Typing_corners.Field.is_optional fd_sub
                      && Typing_corners.Field.is_required fd_pre
                    then
                      (* Opt </:_req Req *)
                      let (env, prop) = invalid ~fail:None env in
                      return (env, prop)
                    else
                      let* (env, fd_post) =
                        project_shape_splat env super_post label assignment
                      in
                      let (env, ty) =
                        Typing_union.union env flex fd_post.sft_ty
                      in
                      let (env, super_base) =
                        if is_opt then
                          Typing_union.union env fd_pre.sft_ty ty
                        else
                          (env, ty)
                      in
                      let (env, prop) =
                        simplify_
                          ~subtype_env
                          ~this_ty
                          ~lhs:{ sub_supportdyn = None; ty_sub = fd_sub.sft_ty }
                          ~rhs:
                            {
                              super_supportdyn = false;
                              super_like;
                              ty_super = super_base;
                            }
                          env
                      in
                      return (env, prop)))
        in
        let field =
          if contributes_nothing then
            { sft_optional = true; sft_ty = MakeType.nothing r }
          else
            { sft_optional = is_opt; sft_ty = flex }
        in
        return (env, (field, prop)))

  (* Infer upper bounds for the sub row's spread inference variables. The super
     row has no unsolved spread variables, so its projected fields determine what
     each sub variable may provide. *)
  and subrow_infer_sub
      ~(subtype_env : Subtype_env.t)
      ~(env : env)
      ~(cache : Typing_corners.Cache.t)
      ~(this_ty : locl_ty option)
      ~super_like
      ~r
      ~(sub : Typing_shape_normalize.Row.t)
      ~(super : Typing_shape_normalize.Row.t)
      (sub_spread_vars : Tvid.t list) =
    List.fold_left sub_spread_vars ~init:(valid env) ~f:(fun acc v ->
        acc &&& fun env ->
        match Typing_corners.partition_at_tyvar sub v with
        | None -> valid env
        | Some (pre_elems, post_elems) ->
          let (env, sub_pre, sub_post) =
            let on_error = subtype_env.Subtype_env.on_error in
            let (env, _e1, sub_pre) =
              let shape_ty = Shape_splat { ss_elems = pre_elems } in
              Typing_shape_normalize.Row.normalize ~on_error r shape_ty env
            in
            let (env, _e2, sub_post) =
              let shape_ty = Shape_splat { ss_elems = post_elems } in
              Typing_shape_normalize.Row.normalize ~on_error r shape_ty env
            in
            (env, sub_pre, sub_post)
          in
          (match
             ( Typing_shape_normalize.Row.as_row sub_pre,
               Typing_shape_normalize.Row.as_row sub_post )
           with
          | (Some sub_pre, Some sub_post) ->
            if
              (not (Typing_shape_normalize.Row.is_bottom sub_pre))
              && not (Typing_shape_normalize.Row.is_bottom sub_post)
            then
              match
                subrow_infer_sub_part
                  ~subtype_env
                  ~env
                  ~cache
                  ~this_ty
                  ~super_like
                  ~r
                  ~spread_var_ty:(mk (r, Tvar v))
                  ~sub_pre
                  ~sub_post
                  ~sub
                  ~super
              with
              | Ok (env, prop) -> (env, prop)
              | Error (env, failure) ->
                report_shape_splat_failure
                  subtype_env
                  ~r
                  ~sub
                  ~super
                  env
                  failure
            else
              valid env
          | (None, _)
          | (_, None) ->
            (* A partition cannot currently distribute a union. Keep this
               conservative fallback if that invariant changes. *)
            invalid ~fail:None env))

  (* Construct an upper-bound shape for a sub-row spread type variable after
     partitioning the sub row into [sub_pre] and [sub_post]. For each field, infer
     what the variable may contribute and generate constraints checking that the
     rightmost-wins merge of [sub_pre], the variable, and [sub_post] is a subtype
     of [super]. Finally constrain the spread type variable below the constructed
     bound. *)
  and subrow_infer_sub_part
      ~(subtype_env : Subtype_env.t)
      ~(env : env)
      ~(cache : Typing_corners.Cache.t)
      ~(this_ty : locl_ty option)
      ~super_like
      ~r
      ~(spread_var_ty : locl_ty)
      ~(sub_pre : Typing_shape_normalize.Row.t)
      ~(sub_post : Typing_shape_normalize.Row.t)
      ~(sub : Typing_shape_normalize.Row.t)
      ~(super : Typing_shape_normalize.Row.t) =
    let labels = Typing_corners.subrow_labels cache env ~sub ~super r in
    let open Hh_prelude.Result.Let_syntax in
    let* (env, known, unknown, props) =
      List.fold_result
        labels
        ~init:(env, TShapeMap.empty, MakeType.mixed r, [])
        ~f:(fun (env, known, unknown, props) label ->
          let* (env, (field, prop)) =
            subrow_infer_sub_part_at
              ~subtype_env
              ~env
              ~cache
              ~this_ty
              ~super_like
              ~r
              ~sub_pre
              ~sub_post
              ~sub
              ~super
              label
          in
          let (known, unknown) =
            match label with
            | Some lbl -> (TShapeMap.add lbl field known, unknown)
            | None -> (known, field.sft_ty)
          in
          return (env, known, unknown, prop :: props))
    in
    let bound =
      Typing_shape_normalize.Row.of_simple
        {
          s_origin = Missing_origin;
          s_unknown_value = unknown;
          s_fields = known;
        }
    in

    let bound_ty = Typing_shape_normalize.Row.to_ty ~reason:r bound in
    let (env, prop_resid) =
      simplify_
        ~subtype_env
        ~this_ty
        ~lhs:{ sub_supportdyn = None; ty_sub = spread_var_ty }
        ~rhs:{ super_supportdyn = false; super_like; ty_super = bound_ty }
        env
    in
    return (env, List.fold_left props ~init:prop_resid ~f:TL.conj)

  (* Infer [label]'s field in the spread variable's upper-bound shape and generate
     constraints for every corner assignment. If [sub_post] masks the variable,
     check [sub_post] directly against [super]. Otherwise, require the variable
     to mask any optional or incompatible [sub_pre] field, bound its field type by
     the live [super] field types, and check the remaining fixed contributions.
     If the variable is always masked, give it the unconstrained top field. *)
  and subrow_infer_sub_part_at
      ~(subtype_env : Subtype_env.t)
      ~(env : env)
      ~(cache : Typing_corners.Cache.t)
      ~(this_ty : locl_ty option)
      ~super_like
      ~r
      ~(sub_pre : Typing_shape_normalize.Row.t)
      ~(sub_post : Typing_shape_normalize.Row.t)
      ~(sub : Typing_shape_normalize.Row.t)
      ~(super : Typing_shape_normalize.Row.t)
      label =
    let top = { sft_optional = true; sft_ty = MakeType.mixed r } in
    let live_names =
      let open Typing_corners in
      Splat_elem.Set.union
        (Splat_elem.Set.of_list (row_live_spread_at sub label))
        (Splat_elem.Set.of_list (row_live_spread_at super label))
    in
    let open Hh_prelude.Result.Let_syntax in
    let* (env, assignments) =
      shape_splat_assignments cache env live_names label r
    in
    match assignments with
    | None -> return (env, (top, TL.valid))
    | Some assignments ->
      let* (env, info) =
        List.fold_result
          assignments
          ~init:(env, [])
          ~f:(fun (env, acc) assignment ->
            let* (env, fd_post) =
              project_shape_splat env sub_post label assignment
            in
            let* (env, fd_super) =
              project_shape_splat env super label assignment
            in
            return
              ( env,
                (Typing_corners.Field.is_required fd_post, fd_super, assignment)
                :: acc ))
      in
      let info = List.rev info in
      let live = List.filter info ~f:(fun (masked, _, _) -> not masked) in
      (match live with
      | [] ->
        (* the spread var is overwritten by [sub_post] everywhere: its upper bound
           is the top field *)
        let* (env, prop) =
          List.fold_result
            info
            ~init:(env, TL.valid)
            ~f:(fun acc (_, fd_super, assignment) ->
              shape_splat_conj acc (fun env ->
                  let* (env, fd_sub) =
                    project_shape_splat env sub_post label assignment
                  in
                  let (env, prop) =
                    subrow_subfield
                      ~subtype_env
                      ~env
                      ~this_ty
                      ~super_like
                      ~r
                      ~tail_blame:None
                      ~blame:None
                      label
                      ~sub:fd_sub
                      ~super:fd_super
                  in
                  return (env, prop)))
        in
        return (env, (top, prop))
      | _ ->
        (* The spread var must be [Req] iff, at some live assignment, the field is [Req]
           in super and [Opt] in [sub_pre], or [sub_pre]'s type does not fit super (so the
           var must mask [sub_pre]). At the [None] unknown label the contribution is
           opt-only. *)
        let* (env, is_req) =
          match label with
          | None -> return (env, false)
          | Some _ ->
            let rec is_required = function
              | [] -> return (env, false)
              | (_, fd_super, assignment) :: rest ->
                let* (env_probe, fd_pre) =
                  project_shape_splat env sub_pre label assignment
                in
                let required =
                  Typing_corners.Field.is_required fd_super
                  && Typing_corners.Field.is_optional fd_pre
                  ||
                  let (_env, prop) =
                    simplify_
                      ~subtype_env
                      ~this_ty
                      ~lhs:{ sub_supportdyn = None; ty_sub = fd_pre.sft_ty }
                      ~rhs:
                        {
                          super_supportdyn = false;
                          super_like;
                          ty_super = fd_super.sft_ty;
                        }
                      env_probe
                  in
                  TL.is_unsat prop
                in
                if required then
                  return (env, true)
                else
                  is_required rest
            in
            is_required live
        in
        (* Decompose the field obligation instead of introducing a fresh tyvar:
           [(fixed | tyvar) <: super] iff both [fixed <: super] and
           [tyvar<: super]. *)
        let upper_ty =
          MakeType.intersection
            r
            (List.map live ~f:(fun (_, fd_super, _) ->
                 Sd.liken ~super_like env fd_super.sft_ty))
        in
        let* (env, prop) =
          List.fold_result
            info
            ~init:(env, TL.valid)
            ~f:(fun acc (masked, fd_super, assignment) ->
              shape_splat_conj acc (fun env ->
                  if masked then
                    let* (env, fd_sub) =
                      project_shape_splat env sub_post label assignment
                    in
                    let (env, prop) =
                      subrow_subfield
                        ~subtype_env
                        ~env
                        ~this_ty
                        ~super_like
                        ~r
                        ~tail_blame:None
                        ~blame:None
                        label
                        ~sub:fd_sub
                        ~super:fd_super
                    in
                    return (env, prop)
                  else
                    let* (env, fd_post) =
                      project_shape_splat env sub_post label assignment
                    in
                    let* (env, sub_base) =
                      if is_req then
                        return (env, fd_post.sft_ty)
                      else
                        let* (env, fd_pre) =
                          project_shape_splat env sub_pre label assignment
                        in
                        return
                          (env, MakeType.union r [fd_pre.sft_ty; fd_post.sft_ty])
                    in
                    let (env, prop) =
                      simplify_
                        ~subtype_env
                        ~this_ty
                        ~lhs:{ sub_supportdyn = None; ty_sub = sub_base }
                        ~rhs:
                          {
                            super_supportdyn = false;
                            super_like;
                            ty_super = fd_super.sft_ty;
                          }
                        env
                    in
                    return (env, prop)))
        in
        return (env, ({ sft_optional = not is_req; sft_ty = upper_ty }, prop)))

  (* Both sub- and super rows have a single type variable. Dispatch on whether
     it's the type variable in both positions *)
  and subrow_infer_couple
      ~(subtype_env : Subtype_env.t)
      ~(env : env)
      ~(cache : Typing_corners.Cache.t)
      ~(this_ty : locl_ty option)
      ~super_like
      ~r
      ~(sub : Typing_shape_normalize.Row.t)
      ~(sub_spread_var : Tvid.t)
      ~(super : Typing_shape_normalize.Row.t)
      ~(super_spread_var : Tvid.t) =
    if Tvid.equal sub_spread_var super_spread_var then
      match
        subrow_infer_same_var
          ~subtype_env
          ~env
          ~cache
          ~this_ty
          ~super_like
          ~r
          ~sub
          ~super
          sub_spread_var
      with
      | Ok (env, Some prop) -> (env, prop)
      | Ok (env, None) ->
        subrow_infer_couple_mid
          ~subtype_env
          ~env
          ~cache
          ~this_ty
          ~super_like
          ~r
          ~sub
          ~sub_spread_var
          ~super
          ~super_spread_var
      | Error (env, failure) ->
        report_shape_splat_failure subtype_env ~r ~sub ~super env failure
    else
      subrow_infer_couple_mid
        ~subtype_env
        ~env
        ~cache
        ~this_ty
        ~super_like
        ~r
        ~sub
        ~sub_spread_var
        ~super
        ~super_spread_var

  (* For splat rows of the form [A, V, B] <: [V, D], derive per-label bounds
     [lo <: V <: hi]. Since we don't have presence variables, the
     [subrow_infer_couple_mid] fallback has to assume all fields are required
     but here we can assume optional and improve completeness for just this case.
     Return [None] when the rows don't have this layout . *)
  and subrow_infer_same_var
      ~(subtype_env : Subtype_env.t)
      ~(env : env)
      ~(cache : Typing_corners.Cache.t)
      ~(this_ty : locl_ty option)
      ~super_like
      ~r
      ~(sub : Typing_shape_normalize.Row.t)
      ~(super : Typing_shape_normalize.Row.t)
      (v : Tvid.t) : TL.subtype_prop option shape_splat_result =
    (* This is a speculative fast path. On [None], discard any environment
       changes made while probing and retry the general algorithm from the
       original environment. *)
    let initial_env = env in
    let open Hh_prelude.Result.Let_syntax in
    (* An empty element list is the empty CLOSED shape (the merge identity), NOT the
       bottom row that [normalize_row] would collapse it to. *)
    let to_shape env elems =
      match elems with
      | [] ->
        let s_fields = TShapeMap.empty
        and s_unknown_value = MakeType.nothing r in
        Some
          ( env,
            Typing_shape_normalize.Row.of_simple
              { s_origin = Missing_origin; s_unknown_value; s_fields } )
      | _ ->
        let (env, _e, row) =
          let shape_ty = Shape_splat { ss_elems = elems } in
          let on_error = subtype_env.Subtype_env.on_error in
          Typing_shape_normalize.Row.normalize ~on_error r shape_ty env
        in
        Typing_shape_normalize.Row.fold_normalized
          row
          ~row:(fun row ->
            if Typing_shape_normalize.Row.is_bottom row then
              None
            else
              Some (env, row))
          ~union:(fun _ ->
            (* Leave this special case and use coupled inference instead. *)
            None)
          ~intersection:(fun _ ->
            (* Leave this special case and use coupled inference instead. *)
            None)
    in
    let bottom_field = { sft_optional = false; sft_ty = MakeType.nothing r } in
    match
      ( Typing_corners.partition_at_tyvar sub v,
        Typing_corners.partition_at_tyvar super v )
    with
    | (Some (sub_pre_e, sub_post_e), Some (super_pre_e, super_post_e))
      when List.is_empty super_pre_e ->
      (* Normalize sequentially to thread [env] and stop at a bottom segment. *)
      (match to_shape env sub_pre_e with
      | None -> return (initial_env, None)
      | Some (env, sub_pre) ->
        (match to_shape env sub_post_e with
        | None -> return (initial_env, None)
        | Some (env, sub_post) ->
          (match to_shape env super_post_e with
          | None -> return (initial_env, None)
          | Some (env, super_post) ->
            let labels =
              None
              :: List.map
                   (TShapeSet.elements
                      (Typing_corners.subrow_label_set cache env ~sub ~super r))
                   ~f:Option.some
            in
            let add_field known unknown label fd =
              match label with
              | Some lbl -> (TShapeMap.add lbl fd known, unknown)
              | None -> (known, fd.sft_ty)
            in
            let bounds_at_label env label =
              let live_names =
                let open Typing_corners in
                Splat_elem.Set.union
                  (Splat_elem.Set.of_list (row_live_spread_at sub label))
                  (Splat_elem.Set.of_list (row_live_spread_at super label))
              in
              let* (env, assignments) =
                shape_splat_assignments cache env live_names label r
              in
              let top_field =
                { sft_optional = true; sft_ty = MakeType.mixed r }
              in
              match assignments with
              | None -> return (env, (bottom_field, top_field, []))
              | Some assignments ->
                let* (env, lower, upper, label_props) =
                  List.fold_result
                    assignments
                    ~init:(env, bottom_field, top_field, [])
                    ~f:(fun (env, lower, upper, label_props) assignment ->
                      let* (env, a) =
                        project_shape_splat env sub_pre label assignment
                      in
                      let* (env, b) =
                        project_shape_splat env sub_post label assignment
                      in
                      let* (env, d) =
                        project_shape_splat env super_post label assignment
                      in
                      if Typing_corners.Field.is_required d then
                        if Typing_corners.Field.is_required b then
                          (* Both sides are masked: LHS=b and RHS=d. *)
                          let (env, prop) =
                            subrow_subfield
                              ~subtype_env
                              ~env
                              ~this_ty
                              ~super_like
                              ~r
                              ~tail_blame:None
                              ~blame:None
                              label
                              ~sub:b
                              ~super:d
                          in
                          return (env, lower, upper, prop :: label_props)
                        else
                          (* The super masks the shared var. Its upper bound
                             masks [sub_pre], while [sub_post]'s optional type
                             must independently fit under [d]. *)
                          let (env, upper) =
                            Typing_corners.Field.meet env ~left:upper ~right:d
                          in
                          let (env, prop) =
                            simplify_
                              ~subtype_env
                              ~this_ty
                              ~lhs:{ sub_supportdyn = None; ty_sub = b.sft_ty }
                              ~rhs:
                                {
                                  super_supportdyn = false;
                                  super_like;
                                  ty_super = d.sft_ty;
                                }
                              env
                          in
                          return (env, lower, upper, prop :: label_props)
                      else if Typing_corners.Field.is_required b then
                        (* The sub masks the shared var, which must cover [b]. *)
                        let (env, lower) =
                          Typing_corners.Field.join env ~left:lower ~right:b
                        in
                        return (env, lower, upper, label_props)
                      else
                        (* Neither side masks it, so it must absorb the sub-side
                           fixed field. *)
                        let (env, merged) =
                          Typing_corners.Field.merge env ~left:a ~right:b
                        in
                        let (env, lower) =
                          Typing_corners.Field.join
                            env
                            ~left:lower
                            ~right:merged
                        in
                        return (env, lower, upper, label_props))
                in
                return (env, (lower, upper, label_props))
            in
            let* (env, lo_known, lo_unknown, hi_known, hi_unknown, props) =
              List.fold_result
                labels
                ~init:
                  ( env,
                    TShapeMap.empty,
                    MakeType.nothing r,
                    TShapeMap.empty,
                    MakeType.mixed r,
                    [] )
                ~f:
                  (fun (env, lo_known, lo_unknown, hi_known, hi_unknown, props)
                       label ->
                  let* (env, (lower, upper, label_props)) =
                    bounds_at_label env label
                  in
                  let (lo_known, lo_unknown) =
                    add_field lo_known lo_unknown label lower
                  in
                  let (hi_known, hi_unknown) =
                    add_field hi_known hi_unknown label upper
                  in
                  return
                    ( env,
                      lo_known,
                      lo_unknown,
                      hi_known,
                      hi_unknown,
                      List.rev_append label_props props ))
            in
            let lo =
              Typing_shape_normalize.Row.of_simple
                {
                  s_fields = lo_known;
                  s_unknown_value = lo_unknown;
                  s_origin = Missing_origin;
                }
            in
            let hi =
              Typing_shape_normalize.Row.of_simple
                {
                  s_fields = hi_known;
                  s_unknown_value = hi_unknown;
                  s_origin = Missing_origin;
                }
            in

            let v_ty = mk (r, Tvar v) in
            let lo_ty = Typing_shape_normalize.Row.to_ty ~reason:r lo in
            let (env, prop_lo) =
              simplify_
                ~subtype_env
                ~this_ty
                ~lhs:{ sub_supportdyn = None; ty_sub = lo_ty }
                ~rhs:{ super_supportdyn = false; super_like; ty_super = v_ty }
                env
            in

            let hi_ty = Typing_shape_normalize.Row.to_ty ~reason:r hi in
            let (env, prop_hi) =
              simplify_
                ~subtype_env
                ~this_ty
                ~lhs:{ sub_supportdyn = None; ty_sub = v_ty }
                ~rhs:{ super_supportdyn = false; super_like; ty_super = hi_ty }
                env
            in
            return
              ( env,
                Some
                  (List.fold_left
                     (prop_lo :: prop_hi :: props)
                     ~init:TL.valid
                     ~f:TL.conj) ))))
    | _ -> return (initial_env, None)

  (* Couple spread variables through a fresh simple row [mid], reducing the
     original check to [sub <: mid <: super]. Its known fields are required
     because it is not inferable (we have no 'presence' variables), so this can
     reject solutions where the only valid intermediary has an optional field. *)
  and subrow_infer_couple_mid
      ~(subtype_env : Subtype_env.t)
      ~(env : env)
      ~(cache : Typing_corners.Cache.t)
      ~(this_ty : locl_ty option)
      ~super_like
      ~r
      ~(sub : Typing_shape_normalize.Row.t)
      ~(sub_spread_var : Tvid.t)
      ~(super : Typing_shape_normalize.Row.t)
      ~(super_spread_var : Tvid.t) =
    let (env, known) =
      TShapeSet.fold
        (fun lbl (env, known) ->
          let (env, flex) = Env.fresh_type env Pos.none in
          (env, TShapeMap.add lbl { sft_optional = false; sft_ty = flex } known))
        (Typing_corners.subrow_label_set cache env ~sub ~super r)
        (env, TShapeMap.empty)
    in
    let (env, unknown) = Env.fresh_type env Pos.none in
    let mid =
      Typing_shape_normalize.Row.of_simple
        {
          s_fields = known;
          s_unknown_value = unknown;
          s_origin = Missing_origin;
        }
    in
    let (env, prop_sub) =
      subrow_infer_sub
        ~subtype_env
        ~env
        ~cache
        ~this_ty
        ~super_like
        ~r
        ~sub
        ~super:mid
        [sub_spread_var]
    in
    let (env, prop_super) =
      subrow_infer_super
        ~subtype_env
        ~env
        ~cache
        ~this_ty
        ~super_like
        ~r
        ~sub:mid
        ~super
        [super_spread_var]
    in
    (env, TL.conj prop_sub prop_super)

  (* == Simple shape subtyping ============================================== *)

  and simplify_subtype_shape
      ~(subtype_env : Subtype_env.t)
      ~(env : env)
      ~(this_ty : locl_ty option)
      ~super_like
      (supportdyn_sub, r_sub, shape_kind_sub, fdm_sub)
      (supportdyn_super, r_super, shape_kind_super, fdm_super) =
    match
      ( TUtils.is_nothing env shape_kind_sub,
        TUtils.is_nothing env shape_kind_super )
    with
    (* An open shape cannot subtype a closed shape *)
    | (false, true) ->
      let fail =
        Option.map
          subtype_env.Subtype_env.on_error
          ~f:
            Typing_error.(
              fun on_error ->
                apply_reasons ~on_error
                @@ Secondary.Shape_fields_unknown
                     {
                       pos = Reason.to_pos r_sub;
                       decl_pos = Reason.to_pos r_super;
                     })
      in
      invalid ~fail env
    (* Otherwise, all projections must subtype *)
    | (sub_unknown_is_nothing, _) ->
      let unknown_tails_identical =
        phys_equal shape_kind_sub shape_kind_super
        && Bool.equal supportdyn_sub supportdyn_super
      in
      if phys_equal fdm_sub fdm_super && unknown_tails_identical then
        (* Fast path: identical field maps and shape kinds *)
        valid env
      else
        (* Use TShapeMap.merge to iterate both maps in O(n) instead of
           constructing a TShapeSet in O(n log n). Collect field names
           that need subtype checking (skip physically identical fields). *)
        let fields_to_check =
          TShapeMap.merge
            (fun _field_name opt_sub opt_super ->
              match (opt_sub, opt_super) with
              | (Some f1, Some f2) when phys_equal f1 f2 -> None
              | _ -> Some ())
            fdm_sub
            fdm_super
        in
        let init =
          if sub_unknown_is_nothing || unknown_tails_identical then
            (* Either the subtype's unknown-field upper bound is provably
               [nothing], or both unknown-field projections are identical. *)
            valid env
          else
            (* Otherwise ensure the proposition holds *)
            simplify_subtype_shape_unknown_fields
              ~subtype_env
              ~this_ty
              ~super_like
              ~env
              (supportdyn_sub, r_sub, shape_kind_sub)
              (supportdyn_super, r_super, shape_kind_super)
        in
        TShapeMap.fold
          (fun field_name () acc ->
            let res =
              shape_project_and_simplify_subtype
                ~subtype_env
                ~this_ty
                ~super_like
                ~env
                (supportdyn_sub, r_sub, shape_kind_sub, fdm_sub)
                (supportdyn_super, r_super, shape_kind_super, fdm_super)
                field_name
            in
            match res with
            | Ok prop -> acc &&& prop
            | Error fail -> acc &&& invalid ~fail)
          fields_to_check
          init

  (* Unknown fields are optional projections at every label absent from both
     field maps. Comparing their types once represents all of those labels. *)
  and simplify_subtype_shape_unknown_fields
      ~subtype_env
      ~this_ty
      ~super_like
      ~env
      (supportdyn_sub, r_sub, shape_kind_sub)
      (supportdyn_super, r_super, shape_kind_super) =
    let ty_sub =
      shape_projection_type ~supportdyn:supportdyn_sub ~env r_sub shape_kind_sub
    and ty_super =
      shape_projection_type
        ~supportdyn:supportdyn_super
        ~env
        r_super
        shape_kind_super
    in
    (simplify_subtype_shape_type
       ~subtype_env
       ~this_ty
       ~super_like
       ~env
       ~supportdyn_sub
       ~r_sub
       ~ty_sub
       ~ty_super)
      env

  (* Helper function to project out a field and then simplify subtype *)
  and shape_project_and_simplify_subtype
      ~subtype_env
      ~this_ty
      ~super_like
      ~env
      (supportdyn_sub, r_sub, shape_kind_sub, shape_map_sub)
      (supportdyn_super, r_super, shape_kind_super, shape_map_super)
      field_name =
    let proj_sub =
      shape_projection
        ~supportdyn:supportdyn_sub
        ~env
        field_name
        shape_kind_sub
        shape_map_sub
        r_sub
    in
    let proj_super =
      shape_projection
        ~supportdyn:supportdyn_super
        ~env
        field_name
        shape_kind_super
        shape_map_super
        r_super
    in
    simplify_subtype_shape_projection
      ~subtype_env
      ~this_ty
      ~super_like
      ~env
      (supportdyn_sub, r_sub, proj_sub)
      (supportdyn_super, r_super, proj_super)
      field_name

  (* For two particular projections `p1` and `p2`, `p1` <: `p2` iff:
      - `p1` = `Required ty1`, `p2` = `Required ty2`, and `ty1` <: `ty2`
      - `p1` = `Required ty1`, `p2` = `Optional ty2`, and `ty1` <: `ty2`
      - `p1` = `Optional ty1`, `p2` = `Optional ty2`, and `ty1` <: `ty2`
      - `p1` = `Absent`, `p2` = `Optional ty2`
      - `p1` = `Absent`, `p2` = `Absent`
     We therefore need to handle all other cases appropriately. *)
  and simplify_subtype_shape_projection
      ?on_type_mismatch
      ~subtype_env
      ~this_ty
      ~super_like
      ~env
      (supportdyn_sub, r_sub, proj_sub)
      (_supportdyn_super, r_super, proj_super)
      field_name =
    let field_pos = TShapeField.pos field_name in
    let printable_name = TUtils.get_printable_shape_field_name field_name in
    let res =
      match (proj_sub, proj_super) with
      (***** "Successful" cases - 5 / 9 total cases *****)
      | (`Required ty_sub, `Required ty_super) ->
        Ok
          (Some
             ( (Typing_reason.Required, ty_sub),
               (Typing_reason.Required, ty_super) ))
      | (`Required ty_sub, `Optional ty_super) ->
        Ok
          (Some
             ( (Typing_reason.Required, ty_sub),
               (Typing_reason.Optional, ty_super) ))
      | (`Optional ty_sub, `Optional ty_super) ->
        Ok
          (Some
             ( (Typing_reason.Optional, ty_sub),
               (Typing_reason.Optional, ty_super) ))
      | (`Absent, `Optional _)
      | (`Absent, `Absent) ->
        Ok None
      (***** Error cases - 4 / 9 total cases *****)
      | (`Required ty_sub, `Absent) ->
        let r_field_sub =
          get_reason
          @@ Typing_env.update_reason env ty_sub ~f:(fun r_sub_prj ->
                 Typing_reason.(
                   prj_shape
                     ~sub:r_sub
                     ~sub_prj:r_sub_prj
                     ~super:r_super
                     printable_name
                     ~kind_sub:Required
                     ~kind_super:Absent))
        and r_field_super = Typing_reason.missing_field in
        let ty_err_opt =
          Option.map
            subtype_env.Subtype_env.on_error
            ~f:
              Typing_error.(
                fun on_error ->
                  apply_reasons ~on_error
                  @@ Secondary.Missing_field
                       {
                         pos = Reason.to_pos r_super;
                         decl_pos = field_pos;
                         name = printable_name;
                         reason_sub = r_field_sub;
                         reason_super = r_field_super;
                       })
        in
        Error ty_err_opt
      | (`Optional ty_sub, `Absent) ->
        (* An optional field [?f => S] is compatible with an absent field only if
           [S <: nothing]. If [S] is an unsolved type variable, as in shape-splat
           inference, return the subtype obligation so inference can solve it to
           [nothing]; otherwise report the more precise missing-field error. *)
        let (_, ty_sub_expanded) = Env.expand_type env ty_sub in
        (match get_node ty_sub_expanded with
        | Tvar _ ->
          Ok
            (Some
               ( (Typing_reason.Optional, ty_sub),
                 (Typing_reason.Absent, MakeType.nothing r_super) ))
        | _ ->
          let r_field_sub =
            get_reason
            @@ Typing_env.update_reason env ty_sub ~f:(fun r_sub_prj ->
                   Typing_reason.(
                     prj_shape
                       ~sub:r_sub
                       ~sub_prj:r_sub_prj
                       ~super:r_super
                       printable_name
                       ~kind_sub:Optional
                       ~kind_super:Absent))
          and r_field_super = Typing_reason.missing_field in
          let ty_err_opt =
            Option.map
              subtype_env.Subtype_env.on_error
              ~f:
                Typing_error.(
                  fun on_error ->
                    apply_reasons ~on_error
                    @@ Secondary.Missing_field
                         {
                           pos = Reason.to_pos r_super;
                           decl_pos = field_pos;
                           name = printable_name;
                           reason_sub = r_field_sub;
                           reason_super = r_field_super;
                         })
          in
          Error ty_err_opt)
      | (`Optional ty_sub, `Required ty_super) ->
        let r_field_sub =
          get_reason
          @@ Typing_env.update_reason env ty_sub ~f:(fun r_sub_prj ->
                 Typing_reason.(
                   prj_shape
                     ~sub:r_sub
                     ~sub_prj:r_sub_prj
                     ~super:r_super
                     printable_name
                     ~kind_sub:Optional
                     ~kind_super:Required))
        and r_field_super = get_reason ty_super in
        let ty_err_opt =
          Option.map
            subtype_env.Subtype_env.on_error
            ~f:
              Typing_error.(
                fun on_error ->
                  apply_reasons ~on_error
                  @@ Secondary.Required_field_is_optional
                       {
                         pos = Reason.to_pos r_sub;
                         decl_pos = Reason.to_pos r_super;
                         name = printable_name;
                         def_pos = get_pos ty_super;
                         reason_sub = r_field_sub;
                         reason_super = r_field_super;
                       })
        in

        Error ty_err_opt
      | (`Absent, `Required ty_super) ->
        let quickfixes_opt =
          match Reason.Predicates.unpack_shape_literal_opt r_sub with
          | Some p ->
            let fix_pos =
              Pos.shrink_to_end (Pos.shrink_by_one_char_both_sides p)
            in
            Some
              [
                Quickfix.make_eager_default_hint_style
                  ~title:("Add field " ^ Markdown_lite.md_codify printable_name)
                  ~new_text:(Printf.sprintf ", '%s' => TODO" printable_name)
                  fix_pos;
              ]
          | None -> None
        in
        let r_field_sub =
          let r_sub_prj = Typing_reason.missing_field in
          Typing_reason.prj_shape
            ~sub:r_sub
            ~sub_prj:r_sub_prj
            ~super:r_super
            printable_name
            ~kind_sub:Typing_reason.Absent
            ~kind_super:Typing_reason.Required
        and r_field_super = get_reason ty_super in
        let ty_err_opt =
          Option.map
            subtype_env.Subtype_env.on_error
            ~f:
              Typing_error.(
                fun on_error ->
                  let on_error =
                    Option.value_map
                      ~default:on_error
                      ~f:(Reasons_callback.add_quickfixes on_error)
                      quickfixes_opt
                  in
                  apply_reasons ~on_error
                  @@ Secondary.Missing_field
                       {
                         decl_pos = field_pos;
                         pos = Reason.to_pos r_sub;
                         name = printable_name;
                         reason_sub = r_field_sub;
                         reason_super = r_field_super;
                       })
        in

        Error ty_err_opt
    in

    match res with
    | Ok None -> Ok valid
    | Ok (Some ((kind_sub, ty_sub), (kind_super, ty_super))) ->
      let ty_sub =
        Typing_env.update_reason env ty_sub ~f:(fun r_sub_prj ->
            Typing_reason.prj_shape
              ~sub:r_sub
              ~sub_prj:r_sub_prj
              ~super:r_super
              printable_name
              ~kind_sub
              ~kind_super)
      in
      (* [on_type_mismatch] replaces the message for the type obligation only.
         The requiredness errors above are already phrased in the user's terms
         and must keep their own wording. *)
      let subtype_env =
        match on_type_mismatch with
        | None -> subtype_env
        | Some on_error ->
          Subtype_env.map_on_error subtype_env ~f:(fun _ -> on_error)
      in
      Ok
        (simplify_subtype_shape_type
           ~subtype_env
           ~this_ty
           ~super_like
           ~env
           ~supportdyn_sub
           ~r_sub
           ~ty_sub
           ~ty_super)
    | Error err -> Error err

  and simplify_subtype_shape_type
      ~subtype_env
      ~this_ty
      ~super_like
      ~env
      ~supportdyn_sub
      ~r_sub
      ~ty_sub
      ~ty_super =
    let ty_super = Sd.liken ~super_like env ty_super in
    let sub_supportdyn =
      if supportdyn_sub then
        Some r_sub
      else
        None
    in
    let lhs = { sub_supportdyn; ty_sub }
    and rhs = { super_like = false; super_supportdyn = false; ty_super } in
    simplify ~subtype_env ~this_ty ~lhs ~rhs

  and shape_projection_type ~supportdyn ~env r ty =
    if
      supportdyn
      && not
           (Subtype_ask.is_sub_type_for_union_i
              env
              (LoclType ty)
              (LoclType (MakeType.supportdyn_mixed r)))
    then
      MakeType.supportdyn r ty
    else
      ty

  (* Shape projection for shape type `s` and field `f` (`s |_ f`) is defined as:
     - if `f` appears in `s` as `f => ty` then `s |_ f` = `Required ty`
     - if `f` appears in `s` as `?f => ty` then `s |_ f` = `Optional ty`
     - if `f` does not appear in `s` and `s` is closed, then `s |_ f` = `Absent`
     - if `f` does not appear in `s` and `s` is open, then `s |_ f` = `Optional mixed`
     EXCEPT
     - `?f => nothing` should be ignored, and treated as `Absent`.
     Such a field cannot be given a value, and so is effectively not present. *)
  and shape_projection ~supportdyn ~env field_name shape_kind shape_map r =
    let make_supportdyn = shape_projection_type ~supportdyn ~env r in
    match TShapeMap.find_opt field_name shape_map with
    | Some { sft_ty; sft_optional } ->
      (match (deref sft_ty, sft_optional) with
      | ((_, Tunion []), true) -> `Absent
      | (_, true) -> `Optional (make_supportdyn sft_ty)
      | (_, false) -> `Required (make_supportdyn sft_ty))
    | None ->
      if TUtils.is_nothing env shape_kind then
        `Absent
      else
        let printable_name = TUtils.get_printable_shape_field_name field_name in
        let ty =
          with_reason
            shape_kind
            (Reason.missing_optional_field (Reason.to_pos r, printable_name))
        in
        `Optional (make_supportdyn ty)

  (* == Entry point ========================================================= *)
  and simplify ~subtype_env ~this_ty ~lhs ~rhs env =
    if Log.should_log env subtype_env ~lhs ~rhs then
      Log.log env subtype_env ~this_ty ~lhs ~rhs @@ fun () ->
      simplify_ ~subtype_env ~this_ty ~lhs ~rhs env
    else
      (* This path should be fast, so we make sure not to create a closure unlike the other path. *)
      simplify_ ~subtype_env ~this_ty ~lhs ~rhs env

  and simplify_ ~subtype_env ~this_ty ~lhs ~rhs env =
    let { sub_supportdyn; ty_sub } = lhs
    and { super_supportdyn; super_like; ty_super } = rhs in
    simplify_subtype_by_physical_equality env ty_sub ty_super @@ fun () ->
    let (env, ty_super) = Env.expand_type env ty_super in
    let (env, ty_sub) = Env.expand_type env ty_sub in
    simplify_subtype_by_physical_equality env ty_sub ty_super @@ fun () ->
    let subtype_env =
      Subtype_env.possibly_add_violated_constraint
        subtype_env
        ~r_sub:(get_reason ty_sub)
        ~r_super:(get_reason ty_super)
    in
    let fail =
      Subtype_env.fail
        subtype_env
        ~ty_sub:(LoclType ty_sub)
        ~ty_super:(LoclType ty_super)
    in
    let simplify_splat_vec env ty =
      let (env, ty) = Env.expand_type env ty in
      match deref ty with
      | (r, Tclass ((_, n), _, [ty])) when String.equal n SN.Collections.cVec ->
        ( env,
          mk
            ( r,
              Ttuple
                { t_required = []; t_optional = []; t_extra = Tvariadic ty } )
        )
      | _ -> (env, ty)
    in
    let (env, ty_sub) =
      if subtype_env.Subtype_env.ignore_likes then
        let (env, ty_opt) =
          Typing_dynamic_utils.try_strip_dynamic
            ~do_not_solve_likes:true
            ~accept_intersections:true
            env
            ty_sub
        in
        (env, Option.value ~default:ty_sub ty_opt)
      else
        (env, ty_sub)
    in
    (* Dynamic inference: when the subtype is a dynamic with a shadow type
       variable, record the supertype as an upper bound on the shadow. This is
       the core mechanism — every [dynamic <: T] check contributes T to the
       shadow's constraint set, building a use-type profile of the dynamic
       expression. *)
    let env =
      match get_node ty_sub with
      | Tdynamic (Some shadow_v) ->
        Env.add_tyvar_upper_bound env shadow_v (LoclType ty_super)
      | _ -> env
    in
    match (deref ty_sub, deref ty_super) with
    (* (...t) <: t *)
    | ( (_, Ttuple { t_required = []; t_optional = []; t_extra = Tsplat ty_sub }),
        _ ) ->
      let (env, ty_sub) = simplify_splat_vec env ty_sub in
      env
      |> simplify
           ~subtype_env
           ~this_ty:None
           ~lhs:{ sub_supportdyn; ty_sub }
           ~rhs:{ super_like; super_supportdyn; ty_super }
      (* t <: (...t) *)
    | ( _,
        ( _,
          Ttuple { t_required = []; t_optional = []; t_extra = Tsplat ty_super }
        ) ) ->
      let (env, ty_sub) = simplify_splat_vec env ty_sub in
      env
      |> simplify
           ~subtype_env
           ~this_ty:None
           ~lhs:{ sub_supportdyn; ty_sub }
           ~rhs:{ super_like; super_supportdyn; ty_super }
    (* First come all the rewrites.
       By "rewrite", we mean stuff like A <: ?B ---> A & nonnull <: B *)
    (* -- Rewrite: x <: Tunion[t] ---> x <: t *)
    | (_, (_, Tunion [ty_super'])) ->
      simplify
        ~subtype_env
        ~this_ty
        ~lhs:{ sub_supportdyn; ty_sub }
        ~rhs:{ super_like; super_supportdyn = false; ty_super = ty_super' }
        env
    (* -- Rewrite:
          t <: #1 | arraykey => t & !arraykey <: #1
          or t <: #1 | !arraykey => t & arraykey <: #1
       TODO(mjt) this is awkward because we need to expand and dereference types
       in a nested position. This would be easier if we didn't have the
       implmentation abstract within constraint solving and we had a function
       (or set of functions) for expansion to a given depth *)
    | (_, (_, Tunion [ty_super1; ty_super2]))
      when expands_to_var_and_arraykey ty_super1 ty_super2 ~env ->
      let (env, (tvar_ty, ak_ty)) =
        var_and_arraykey_exn ty_super1 ty_super2 ~env
      in

      let (env, neg_ty) =
        Inter.negate_type
          env
          (get_reason (mk ak_ty))
          ~approx:Inter.Utils.ApproxDown
          (mk ak_ty)
      in
      let (env, inter_ty) =
        Inter.intersect env ~r:(get_reason lhs.ty_sub) neg_ty lhs.ty_sub
      in
      let lhs = { lhs with ty_sub = inter_ty }
      and rhs =
        { super_like = false; super_supportdyn = false; ty_super = mk tvar_ty }
      in
      simplify ~subtype_env ~this_ty ~lhs ~rhs env
    (* -- Rewrite: x <: ?supportdyn<t> ---> x <: supportdyn<?t> *)
    | ( ( _,
          ( Tany _ | Tnonnull | Toption _ | Tdynamic _ | Tprim _ | Tfun _
          | Ttuple _ | Tshape _ | Tvec_or_dict _ | Taccess _ | Tgeneric _
          | Tnewtype _ | Tdependent _ | Tclass _ | Tneg _ | Tunion _
          | Tintersection _ | Tvar _ | Tclass_ptr _ ) ),
        (r_super, Toption ty_inner) )
      when expands_to_supportdyn ty_inner ~env ->
      let (env, ty_inner) = Env.expand_type env ty_inner in
      (* Since we have guarded on [ty_inner] being a supportdyn new type,
         the following calls will not generate exceptions *)
      let (env, (_, tyargs, _)) = newtype_exn ty_inner ~env in
      let tyarg = List.hd_exn tyargs in
      let tyarg = MakeType.nullable r_super tyarg in
      let ty_super = MakeType.supportdyn r_super tyarg in
      let lhs = { lhs with ty_sub }
      and rhs =
        { super_like = rhs.super_like; super_supportdyn = false; ty_super }
      in
      simplify ~subtype_env ~this_ty ~lhs ~rhs env
    (* -- Rewrite:
          supportdyn<t> <: ?u
          ---> nonnull & supportdyn<t> <: u
          ---> supportdyn<nonnull & t> <: u
    *)
    | ((r, Tnewtype (name, [tyarg1], _)), (r_super, Toption lty_inner))
      when String.equal name SN.Classes.cSupportDyn ->
      (* TODO(mjt) add 'rewrite' reason including both r & r_super? *)
      let (env, ty_sub') =
        Inter.intersect env ~r:r_super tyarg1 (MakeType.nonnull r_super)
      in
      let lty_inner =
        Typing_env.update_reason env lty_inner ~f:(fun r_super_prj ->
            Typing_reason.prj_nullable_super
              ~super:r_super
              ~super_prj:r_super_prj)
      in
      simplify
        ~subtype_env
        ~this_ty:None
        ~lhs:{ sub_supportdyn = Some r; ty_sub = ty_sub' }
        ~rhs:{ super_like; super_supportdyn = false; ty_super = lty_inner }
        env
    (* -- Rewrite: _ & ...  & _ <: ?t ---> _ & .. & _ & nonnull <: t
       but only when the intersection does not contain any type with an exact
       negation (we apply a rewrite in the other direction for those cases) *)
    | ((_, Tintersection ty_subs), (r_super, Toption ty_inner_super))
      when expands_to_var_or_intersection ty_inner_super ~env
           &&
           let (_, non_ty_opt, _) =
             Subtype_negation.find_type_with_exact_negation env ty_subs
           in
           Option.is_none non_ty_opt ->
      let (env, ty_sub) =
        Inter.intersect env ~r:r_super ty_sub (MakeType.nonnull r_super)
      in
      let ty_inner_super =
        Typing_env.update_reason env ty_inner_super ~f:(fun r_super_prj ->
            Typing_reason.prj_nullable_super
              ~super:r_super
              ~super_prj:r_super_prj)
      in
      let lhs = { lhs with ty_sub }
      and rhs =
        { rhs with super_supportdyn = false; ty_super = ty_inner_super }
      in
      simplify ~subtype_env ~this_ty ~lhs ~rhs env
    (* -- C-Var-R ----------------------------------------------------------- *)
    | ((_, Tunion _), (_, Tvar _)) ->
      default_subtype
        ~subtype_env
        ~this_ty
        ~fail
        ~lhs:{ sub_supportdyn; ty_sub }
        ~rhs:{ super_like; super_supportdyn = false; ty_super }
        env
    (* We want to treat nullable as a union with the same rule as above.
       * This is only needed for Tvar on right; other cases are dealt with specially as
       * derived rules.
    *)
    | ((r, Toption t), (_r_super, Tvar _var_super)) ->
      let (env, t) = Env.expand_type env t in
      (match get_node t with
      (* We special case on `mixed <: Tvar _`, adding the entire `mixed` type
         as a lower bound. This enables clearer error messages when upper bounds
         are added to the type variable: transitive closure picks up the
         entire `mixed` type, and not separately consider `null` and `nonnull` *)
      | Tnonnull ->
        mk_issubtype_prop
          ~sub_supportdyn
          ~is_dynamic_aware:subtype_env.Subtype_env.is_dynamic_aware
          env
          (LoclType ty_sub)
          (LoclType ty_super)
      | _ ->
        let ty_null = MakeType.null r in
        let t =
          Typing_env.update_reason env t ~f:(fun r_sub_prj ->
              Typing_reason.prj_nullable_sub ~sub:r ~sub_prj:r_sub_prj)
        in
        simplify
          ~subtype_env
          ~this_ty
          ~lhs:{ sub_supportdyn; ty_sub = t }
          ~rhs:{ super_like; super_supportdyn = false; ty_super }
          env
        &&& simplify
              ~subtype_env
              ~this_ty
              ~lhs:{ sub_supportdyn; ty_sub = ty_null }
              ~rhs:{ super_like = false; super_supportdyn = false; ty_super })
    | ((_, Tvar var_sub), (_, Tvar var_super)) when Tvid.equal var_sub var_super
      ->
      valid env
    | ( ( r_sub,
          ( Tany _ | Tnonnull | Tdynamic _ | Tprim _ | Tvar _ | Tfun _
          | Ttuple _ | Tshape _ | Tgeneric _ | Tintersection _ | Tvec_or_dict _
          | Taccess _ | Tnewtype _ | Tdependent _ | Tclass _ | Tneg _ | Tlabel _
          | Tclass_ptr _ ) ),
        (r_super, Tvar var_super_id) ) -> begin
      (* Ensure that higher-ranked type parameters don't escape their scope *)
      let tvar_rank = Env.rank_of_tvar env var_super_id in
      match check_rank env ty_sub tvar_rank with
      | Some (generic_reason, generic_name) ->
        let error =
          Typing_error.Secondary.Higher_rank_tparam_escape
            {
              tvar_pos = Typing_reason.to_pos r_super;
              pos_with_generic = Typing_reason.to_pos r_sub;
              generic_reason;
              generic_name;
            }
        in
        let fail = Subtype_env.fail_with_secondary_error subtype_env error in
        invalid ~fail env
      | _ -> begin
        let (env, simplified_sub_ty) =
          Typing_solver_utils.remove_tyvar_from_lower_bound
            env
            var_super_id
            (LoclType ty_sub)
        in
        match simplified_sub_ty with
        | LoclType simplified_sub_ty
        (* Better than checking nothing like this might be to change
           remove_tyvar_from_lower_bound to return an Option to distinguish
           whether a simplification was actually done.
           And then we can recursively do a subtype check to check this.
        *)
          when ty_equal simplified_sub_ty (MakeType.nothing Reason.none) ->
          valid env
        | _ ->
          (match subtype_env.Subtype_env.is_dynamic_aware with
          | true ->
            mk_issubtype_prop
              ~sub_supportdyn
              ~is_dynamic_aware:true
              env
              (LoclType ty_sub)
              (LoclType ty_super)
          | false ->
            if super_like then
              let (env, ty_sub) =
                Typing_dynamic.strip_covariant_like env ty_sub
              in
              simplify
                ~subtype_env
                ~this_ty
                ~lhs:{ sub_supportdyn; ty_sub }
                ~rhs:{ super_like = false; super_supportdyn = false; ty_super }
                env
            else
              mk_issubtype_prop
                ~sub_supportdyn
                ~is_dynamic_aware:subtype_env.Subtype_env.is_dynamic_aware
                env
                (LoclType ty_sub)
                (LoclType ty_super))
      end
    end
    (* -- C-Inter-R --------------------------------------------------------- *)
    | ((r_sub, Tunion ty_subs), (_r_super, Tintersection _)) ->
      let mk_prop ~subtype_env ~this_ty:_ ~lhs ~rhs env =
        simplify ~subtype_env ~this_ty:None ~lhs ~rhs env
      in
      Common.simplify_union_l
        ~subtype_env
        ~this_ty
        ~mk_prop
        ~update_reason:
          (Typing_env.update_reason ~f:(fun r_sub_prj ->
               Typing_reason.prj_union_sub ~sub:r_sub ~sub_prj:r_sub_prj))
        (sub_supportdyn, ty_subs)
        rhs
        env
    | ( ( _r_sub,
          ( Tany _ | Tnonnull | Toption _ | Tdynamic _ | Tprim _ | Tvar _
          | Tfun _ | Ttuple _ | Tshape _ | Tgeneric _ | Tintersection _
          | Tvec_or_dict _ | Taccess _ | Tnewtype _ | Tdependent _ | Tclass _
          | Tneg _ | Tlabel _ | Tclass_ptr _ ) ),
        (r_super, Tintersection tyl) ) ->
      (* t <: (t1 & ... & tn)
       *   if and only if
       * t <: t1 /\  ... /\ t <: tn
       *)
      List.fold_left tyl ~init:(env, TL.valid) ~f:(fun res ty_super ->
          let ty_super =
            Typing_env.update_reason env ty_super ~f:(fun r_super_prj ->
                Typing_reason.prj_inter_super
                  ~super:r_super
                  ~super_prj:r_super_prj)
          in
          res
          &&& simplify
                ~subtype_env
                ~this_ty
                ~lhs:{ sub_supportdyn; ty_sub }
                ~rhs:{ super_like = false; super_supportdyn = false; ty_super })
    (* -- C-Bot-R ----------------------------------------------------------- *)
    (* Empty union encodes the bottom type nothing *)
    | ((_, Tunion []), (_, Tunion [])) -> (env, TL.valid)
    | ((r_sub, Tunion tyl), (_r_super, Tunion [])) ->
      let mk_prop ~subtype_env ~this_ty:_ ~lhs ~rhs env =
        simplify ~subtype_env ~this_ty:None ~lhs ~rhs env
      in
      Common.simplify_union_l
        ~subtype_env
        ~this_ty
        ~mk_prop
        ~update_reason:
          (Typing_env.update_reason ~f:(fun r_sub_prj ->
               Typing_reason.prj_union_sub ~sub:r_sub ~sub_prj:r_sub_prj))
        (sub_supportdyn, tyl)
        rhs
        env
    | ((r_sub, Tintersection tyl), (_r_super, Tunion [])) ->
      let mk_prop ~subtype_env ~this_ty:_ ~lhs ~rhs env =
        simplify ~subtype_env ~this_ty:None ~lhs ~rhs env
      in
      (* Otherwise use the incomplete common case which doesn't require inspection of the rhs *)
      Common.simplify_intersection_l
        ~subtype_env
        ~this_ty
        ~fail
        ~mk_prop
        ~update_reason:
          (Typing_env.update_reason ~f:(fun r_sub_prj ->
               Typing_reason.prj_inter_sub ~sub:r_sub ~sub_prj:r_sub_prj))
        (sub_supportdyn, tyl)
        rhs
        env
    | ((r, Tnewtype (n, tyl, _)), (_, Tunion [])) ->
      let (env, ty) = Typing_utils.get_newtype_super env r n tyl in
      let mk_prop ~subtype_env ~this_ty ~lhs ~rhs env =
        simplify ~subtype_env ~this_ty ~lhs ~rhs env
      in
      Common.simplify_newtype_l
        ~subtype_env
        ~this_ty
        ~mk_prop
        (sub_supportdyn, r, n, ty)
        rhs
        env
    | ((_, Tgeneric _), (_, Tunion []))
      when subtype_env.Subtype_env.require_completeness ->
      mk_issubtype_prop
        ~sub_supportdyn
        ~is_dynamic_aware:subtype_env.Subtype_env.is_dynamic_aware
        env
        (LoclType ty_sub)
        (LoclType ty_super)
    | ((r_generic, Tgeneric name_sub), (_, Tunion [])) -> begin
      match
        VisitedGoals.try_add_visited_generic_sub
          subtype_env.Subtype_env.visited
          name_sub
          ty_super
      with
      | None ->
        (* If we've seen this type parameter before then we must have gone
             * round a cycle so we fail
        *)
        invalid ~fail env
      | Some new_visited -> begin
        let subtype_env = Subtype_env.set_visited subtype_env new_visited in

        let mk_prop ~subtype_env ~this_ty ~lhs ~rhs env =
          simplify ~subtype_env ~this_ty ~lhs ~rhs env
        in
        Common.simplify_generic_l
          ~subtype_env
          ~this_ty
          ~fail
          ~mk_prop
          (sub_supportdyn, r_generic, name_sub)
          { super_like; super_supportdyn = false; ty_super }
          { super_like = false; super_supportdyn = false; ty_super }
          env
      end
    end
    | ((r, Tdependent (dep_ty, ty)), (_, Tunion [])) ->
      let mk_prop ~subtype_env ~this_ty ~lhs ~rhs env =
        simplify ~subtype_env ~this_ty ~lhs ~rhs env
      in
      Common.simplify_dependent_l
        ~subtype_env
        ~this_ty
        ~mk_prop
        (sub_supportdyn, r, dep_ty, ty)
        rhs
        env
    | ((_, Tany _), (_, Tunion [])) when subtype_env.Subtype_env.no_top_bottom
      ->
      mk_issubtype_prop
        ~sub_supportdyn
        ~is_dynamic_aware:subtype_env.Subtype_env.is_dynamic_aware
        env
        (LoclType ty_sub)
        (LoclType ty_super)
    | ((_, Tany _), (_, Tunion [])) -> valid env
    | ((_, Tvar _), (_, Tunion [])) ->
      mk_issubtype_prop
        ~sub_supportdyn
        ~is_dynamic_aware:subtype_env.Subtype_env.is_dynamic_aware
        env
        (LoclType ty_sub)
        (LoclType ty_super)
    | ( ( _,
          ( Tnonnull | Toption _ | Tdynamic _ | Tprim _ | Tfun _ | Ttuple _
          | Tshape _ | Tvec_or_dict _ | Taccess _ | Tclass _ | Tneg _ ) ),
        (_, Tunion []) ) ->
      invalid env ~fail
    (* -- C-Union-R --------------------------------------------------------- *)
    | (_, (r_super, Tunion lty_supers)) -> begin
      match deref ty_sub with
      | (r_sub, Tunion ty_subs) ->
        let mk_prop ~subtype_env ~this_ty:_ ~lhs ~rhs env =
          simplify ~subtype_env ~this_ty:None ~lhs ~rhs env
        in
        Common.simplify_union_l
          ~subtype_env
          ~this_ty
          ~mk_prop
          ~update_reason:
            (Typing_env.update_reason ~f:(fun r_sub_prj ->
                 Typing_reason.prj_union_sub ~sub:r_sub ~sub_prj:r_sub_prj))
          (sub_supportdyn, ty_subs)
          rhs
          env
      | (_, Tvar _) when subtype_env.is_dynamic_aware ->
        mk_issubtype_prop
          ~sub_supportdyn
          ~is_dynamic_aware:subtype_env.is_dynamic_aware
          env
          (LoclType ty_sub)
          (LoclType ty_super)
      | (_, Tvar id) ->
        (* For subtyping queries of the form
         *
         *   Tvar #id <: (Tvar #id | ...)
         *
         * `remove_tyvar_from_upper_bound` simplifies the union to
         * `mixed`. This indicates that the query is discharged. If we find
         * any other upper bound, we leave the subtyping query as it is.
         *)
        let (env, simplified_super_ty) =
          Typing_solver_utils.remove_tyvar_from_upper_bound
            env
            id
            (LoclType ty_super)
        in
        (* If the type is already in the upper bounds of the type variable,
         * then we already know that this subtype assertion is valid
         *)
        if ITySet.mem simplified_super_ty (Env.get_tyvar_upper_bounds env id)
        then
          valid env
        else
          let mixed = MakeType.mixed Reason.none in
          (match simplified_super_ty with
          | LoclType simplified_super_ty when ty_equal simplified_super_ty mixed
            ->
            valid env
          | _ ->
            mk_issubtype_prop
              ~sub_supportdyn
              ~is_dynamic_aware:subtype_env.Subtype_env.is_dynamic_aware
              env
              (LoclType ty_sub)
              (LoclType ty_super))
      | (_, Tgeneric _) when subtype_env.Subtype_env.require_completeness ->
        mk_issubtype_prop
          ~sub_supportdyn
          ~is_dynamic_aware:subtype_env.Subtype_env.is_dynamic_aware
          env
          (LoclType ty_sub)
          (LoclType ty_super)
      (* Num is not atomic: it is equivalent to int|float. The rule below relies
       * on ty_sub not being a union e.g. consider num <: arraykey | float, so
       * we break out num first.
       *)
      | (r, Tprim Nast.Tnum) ->
        let r = Typing_reason.prj_num_sub ~sub:r ~sub_prj:r in
        let ty_float = MakeType.float r and ty_int = MakeType.int r in
        simplify
          ~subtype_env
          ~this_ty
          ~lhs:{ sub_supportdyn = None; ty_sub = ty_float }
          ~rhs:{ super_like = false; super_supportdyn = false; ty_super }
          env
        &&& simplify
              ~subtype_env
              ~this_ty
              ~lhs:{ sub_supportdyn = None; ty_sub = ty_int }
              ~rhs:{ super_like = false; super_supportdyn = false; ty_super }
      (* Likewise, reduce nullable on left to a union *)
      | (r, Toption ty) ->
        let r = Typing_reason.prj_nullable_sub ~sub:r ~sub_prj:r in
        let ty_null = MakeType.null r in
        let prop_null =
          let prop =
            simplify
              ~subtype_env
              ~this_ty
              ~lhs:{ sub_supportdyn; ty_sub = ty_null }
              ~rhs:{ super_like = false; super_supportdyn = false; ty_super }
          in

          (fun env -> if_unsat (invalid ~fail) @@ prop env)
        in
        let ty =
          Typing_env.update_reason env ty ~f:(fun r_sub_prj ->
              Typing_reason.prj_nullable_sub ~sub:r ~sub_prj:r_sub_prj)
        in
        prop_null env
        &&& simplify
              ~subtype_env
              ~this_ty
              ~lhs:{ sub_supportdyn; ty_sub = ty }
              ~rhs:{ super_like = false; super_supportdyn = false; ty_super }
      | (r_sub, Tintersection tyl) ->
        (* First try to rewrite
            _ & ... & !t & _ <: _ | ... | _ => _ & ... & _ <: _ | ... | t | _
        *)
        (match Subtype_negation.find_type_with_exact_negation env tyl with
        | (env, Some non_ty, tyl) ->
          let (env, ty_super) = TUtils.union env ty_super non_ty in
          let ty_sub = MakeType.intersection r_sub tyl in
          let lhs = { lhs with ty_sub }
          and rhs =
            { super_like = false; super_supportdyn = false; ty_super }
          in
          simplify ~subtype_env ~this_ty ~lhs ~rhs env
        | _ ->
          let ( ||| ) = ( ||| ) ~fail in
          let simplify_super_intersection env tyl_sub ty_super =
            (* It's sound to reduce t1 & t2 <: t to (t1 <: t) || (t2 <: t), but
             * not complete.
             * TODO(T120921930): Don't do this if require_completeness is set.
             *)
            List.fold_left
              tyl_sub
              ~init:(env, TL.invalid ~fail)
              ~f:(fun res ty_sub ->
                res
                ||| simplify
                      ~subtype_env
                      ~this_ty
                      ~lhs:{ sub_supportdyn; ty_sub }
                      ~rhs:
                        {
                          super_like = false;
                          super_supportdyn = false;
                          ty_super;
                        })
          in
          (* Heuristicky logic to decide whether to "break" the intersection
              or the union first, based on observing that the following cases often occur:
                - A & B <: (A & B) | C
                  In which case we want to "break" the union on the right first
                  in order to have the following recursive calls :
                      A & B <: A & B
                      A & B <: C
                - A & (B | C) <: B | C
                  In which case we want to "break" the intersection on the left first
                  in order to have the following recursive calls:
                      A <: B | C
                      B | C <: B | C
             If there is a type variable in the union, then generally it's helpful to
             break the union apart.
          *)
          if
            List.exists lty_supers ~f:(fun t ->
                TUtils.is_tintersection env t
                || TUtils.is_opt_tyvar env t
                || TUtils.is_tyvar env t)
          then
            simplify_sub_union
              ~subtype_env
              ~sub_supportdyn
              ~this_ty
              ~super_like
              ~fail
              ty_sub
              (r_super, lty_supers)
              env
          else if List.exists tyl ~f:(TUtils.is_tunion env) then
            simplify_super_intersection
              env
              tyl
              (mk (r_super, Tunion lty_supers))
          else
            simplify_sub_union
              ~subtype_env
              ~sub_supportdyn
              ~this_ty
              ~super_like
              ~fail
              ty_sub
              (r_super, lty_supers)
              env)
      | ( _,
          ( Tany _ | Tprim _ | Tnonnull | Tdynamic _ | Tfun _ | Ttuple _
          | Tshape _ | Tgeneric _ | Tvec_or_dict _ | Taccess _ | Tnewtype _
          | Tdependent _ | Tclass _ | Tneg _ | Tlabel _ | Tclass_ptr _ ) ) ->
        simplify_sub_union
          ~subtype_env
          ~sub_supportdyn
          ~this_ty
          ~super_like
          ~fail
          ty_sub
          (r_super, lty_supers)
          env
    end
    (* -- C-Top-R ----------------------------------------------------------- *)
    (* `Toption(Tnonnull)` encodes mixed, which is our top type *)
    | ( ( _,
          ( Tany _ | Tnonnull | Toption _ | Tdynamic _ | Tprim _ | Tfun _
          | Ttuple _ | Tshape _ | Tvec_or_dict _ | Taccess _ | Tgeneric _
          | Tnewtype _ | Tdependent _ | Tclass _ | Tneg _ | Tunion _
          | Tintersection _ | Tvar _ ) ),
        (_, Toption ty_inner) )
      when expands_to_nonnull ty_inner ~env ->
      valid env
    (* -- C-Option-R -------------------------------------------------------- *)
    (* null is the type of null and is a subtype of any option type. *)
    | ((_, Tprim Nast.Tnull), (_, Toption _)) -> valid env
    (* ?ty_sub' <: ?ty_super' iff ty_sub' <: ?ty_super'. Reasoning:
     * If ?ty_sub' <: ?ty_super', then from ty_sub' <: ?ty_sub' (widening) and transitivity
     * of <: it follows that ty_sub' <: ?ty_super'.  Conversely, if ty_sub' <: ?ty_super', then
     * by covariance and idempotence of ?, we have ?ty_sub' <: ??ty_sub' <: ?ty_super'.
     * Therefore, this step preserves the set of solutions.
     *)
    | ((r_sub, Toption ty_sub'), (r_super, Toption lty_inner)) ->
      let ty_sub' =
        Typing_env.update_reason env ty_sub' ~f:(fun r_sub_prj ->
            Typing_reason.prj_nullable_sub ~sub:r_sub ~sub_prj:r_sub_prj)
      in
      simplify
        ~subtype_env
        ~this_ty
        ~lhs:{ sub_supportdyn; ty_sub = ty_sub' }
        ~rhs:
          {
            super_like;
            super_supportdyn = false;
            ty_super = mk (r_super, Toption lty_inner);
          }
        env
    (* If ty_sub <: ?ty_super' and ty_sub does not contain null then we
     * must also have ty_sub <: ty_super'.  The converse follows by
     * widening and transitivity.  Therefore, this step preserves the set
     * of solutions.
     *)
    | ((_, (Tintersection _ | Tunion _)), (r_super, Toption lty_inner))
      when TUtils.is_type_disjoint env ty_sub (MakeType.null Reason.none) ->
      let lty_inner =
        Typing_env.update_reason env lty_inner ~f:(fun r_super_prj ->
            Typing_reason.prj_nullable_super
              ~super:r_super
              ~super_prj:r_super_prj)
      in
      simplify
        ~subtype_env
        ~this_ty
        ~lhs:{ sub_supportdyn; ty_sub }
        ~rhs:{ super_like; super_supportdyn = false; ty_super = lty_inner }
        env
    (* We do not want to decompose Toption for these cases *)
    | ((_, (Tvar _ | Tunion _ | Tintersection _)), (r_super, Toption lty_inner))
      ->
      default_subtype
        ~subtype_env
        ~this_ty
        ~fail
        ~lhs:{ sub_supportdyn; ty_sub }
        ~rhs:
          {
            super_like;
            super_supportdyn = false;
            ty_super = mk (r_super, Toption lty_inner);
          }
        env
    | ((_, Tgeneric _), (r_super, Toption lty_inner))
      when subtype_env.Subtype_env.require_completeness ->
      (* TODO(T69551141) handle type arguments ? *)
      default_subtype
        ~subtype_env
        ~this_ty
        ~fail
        ~lhs:{ sub_supportdyn; ty_sub }
        ~rhs:
          {
            super_like;
            super_supportdyn = false;
            ty_super = mk (r_super, Toption lty_inner);
          }
        env
    (* If t1 <: ?t2 and t1 is an abstract type constrained as t1',
     * then t1 <: t2 or t1' <: ?t2.  The converse is obviously
     * true as well.  We can fold the case where t1 is unconstrained
     * into the case analysis below.
     *
     * In the case where it's easy to determine that null isn't in t1,
     * we need only check t1 <: t2.
     *)
    | ( (_r_sub, (Tnewtype _ | Tdependent _ | Tgeneric _ | Tprim Nast.Tvoid)),
        (r_super, Toption lty_inner) ) ->
      let lty_inner =
        Typing_env.update_reason env lty_inner ~f:(fun r_super_prj ->
            Typing_reason.prj_nullable_super
              ~super:r_super
              ~super_prj:r_super_prj)
      in
      (* TODO(T69551141) handle type arguments? *)
      if null_not_subtype env ty_sub then
        simplify
          ~subtype_env
          ~this_ty
          ~lhs:{ sub_supportdyn; ty_sub }
          ~rhs:{ super_like; super_supportdyn = false; ty_super = lty_inner }
          env
      else
        let ( ||| ) = ( ||| ) ~fail in
        simplify
          ~subtype_env
          ~this_ty
          ~lhs:{ sub_supportdyn; ty_sub }
          ~rhs:{ super_like; super_supportdyn = false; ty_super = lty_inner }
          env
        ||| default_subtype
              ~subtype_env
              ~this_ty
              ~fail
              ~lhs:{ sub_supportdyn; ty_sub }
              ~rhs:{ super_like; super_supportdyn = false; ty_super }
    | ( ( _r_sub,
          ( Tdynamic _ | Tprim _ | Tnonnull | Tfun _ | Ttuple _ | Tshape _
          | Tclass _ | Tvec_or_dict _ | Tany _ | Taccess _ | Tlabel _
          | Tclass_ptr _ ) ),
        (r_super, Toption lty_inner) ) ->
      let lty_inner =
        Typing_env.update_reason env lty_inner ~f:(fun r_super_prj ->
            Typing_reason.prj_nullable_super
              ~super:r_super
              ~super_prj:r_super_prj)
      in
      simplify
        ~subtype_env
        ~this_ty
        ~lhs:{ sub_supportdyn; ty_sub }
        ~rhs:{ super_like; super_supportdyn = false; ty_super = lty_inner }
        env
    (* This is treating the option as a union, and using the sound, but incomplete,
       t <: t1 | t2 to (t <: t1) || (t <: t2) reduction
       TODO(T120921930): Don't do this if require_completeness is set.
    *)
    | ((_r_sub, Tneg _), (r_super, Toption lty_inner)) ->
      let lty_inner =
        Typing_env.update_reason env lty_inner ~f:(fun r_super_prj ->
            Typing_reason.prj_nullable_super
              ~super:r_super
              ~super_prj:r_super_prj)
      in
      simplify
        ~subtype_env
        ~this_ty
        ~lhs:{ sub_supportdyn; ty_sub }
        ~rhs:{ super_like; super_supportdyn = false; ty_super = lty_inner }
        env
    (* -- C-Dep-R ----------------------------------------------------------- *)
    | ((_, Tdependent (d_sub, bound_sub)), (_, Tdependent (d_sup, bound_sup)))
      when equal_dependent_type d_sub d_sup ->
      simplify
        ~subtype_env
        ~this_ty:(Option.first_some this_ty (Some ty_sub))
        ~lhs:{ sub_supportdyn; ty_sub = bound_sub }
        ~rhs:
          { super_like = false; super_supportdyn = false; ty_super = bound_sup }
        env
    | ((_, Tdependent (_, bound_sub)), (_, Tdependent _)) ->
      simplify
        ~subtype_env
        ~this_ty:(Option.first_some this_ty (Some ty_sub))
        ~lhs:{ sub_supportdyn; ty_sub = bound_sub }
        ~rhs:{ super_like = false; super_supportdyn = false; ty_super }
        env
    | ((r_sub, Tclass ((_, y), _, _)), (_r_super, Tdependent (_, bound_sup)))
      when expands_to_class bound_sup ~env ->
      let (env, ((_, x) as id)) = class_id_exn bound_sup ~env in

      if is_final_and_invariant env x then
        (* For final class C, there is no difference between `this as X` and `X`,
           and `expr<#n> as X` and `X`. But we need to take care with variant
           classes, since we can't statically guarantee their runtime type. *)
        simplify
          ~subtype_env
          ~this_ty
          ~lhs:{ sub_supportdyn; ty_sub }
          ~rhs:
            {
              super_like = false;
              super_supportdyn = false;
              ty_super = bound_sup;
            }
          env
      else
        let fail =
          if String.equal x y then
            let p = Reason.to_pos r_sub in
            let (pos_super, class_name) = id in
            Subtype_env.fail_with_suffix
              subtype_env
              ~ty_sub:(LoclType ty_sub)
              ~ty_super:(LoclType ty_super)
              (Typing_error.Secondary.This_final
                 { pos_super; class_name; pos_sub = p })
          else
            Subtype_env.fail
              subtype_env
              ~ty_sub:(LoclType ty_sub)
              ~ty_super:(LoclType ty_super)
        in
        invalid env ~fail
    | ( ( _,
          ( Tany _ | Tunion _ | Toption _ | Tintersection _ | Tgeneric _
          | Taccess _ | Tnewtype _ | Tprim _ | Tnonnull | Tclass _
          | Tvec_or_dict _ | Ttuple _ | Tshape _ | Tdynamic _ | Tneg _ | Tfun _
          | Tvar _ | Tlabel _ | Tclass_ptr _ ) ),
        (_, Tdependent _) ) ->
      default_subtype
        ~subtype_env
        ~this_ty
        ~fail
        ~lhs:{ sub_supportdyn; ty_sub }
        ~rhs:{ super_like; super_supportdyn = false; ty_super }
        env
    (* -- C-Access-R -------------------------------------------------------- *)
    | (_, (_, Taccess _)) -> invalid ~fail env
    (* -- C-Generic-R ------------------------------------------------------- *)
    | ((_, Tgeneric name_sub), (_, Tgeneric name_super))
      when String.equal name_sub name_super ->
      valid env
    | ((_r_sub, Tgeneric name_sub), (_, Tgeneric _))
      when let (generic_lower_bounds, _other_lower_bounds) =
             generic_lower_bounds env ty_super
           in
           S_set.mem name_sub generic_lower_bounds ->
      valid env
    (* Ensure that higher-ranked type parameters don't escape their scope *)
    | ((r_sub, Tvar tv_sub), (r_super, Tgeneric tp_sup))
      when Env.rank_of_tparam env tp_sup > Env.rank_of_tvar env tv_sub ->
      let error =
        Typing_error.Secondary.Higher_rank_tparam_escape
          {
            tvar_pos = Typing_reason.to_pos r_sub;
            pos_with_generic = Typing_reason.to_pos r_super;
            generic_reason = r_super;
            generic_name = Some tp_sup;
          }
      in
      let fail = Subtype_env.fail_with_secondary_error subtype_env error in
      invalid env ~fail
    (* When decomposing subtypes for the purpose of adding bounds on generic
     * parameters to the context, (so seen_generic_params = None), leave
     * subtype so that the bounds get added *)
    | ((_, (Tvar _ | Tunion _)), (_, Tgeneric _)) ->
      default_subtype
        ~subtype_env
        ~this_ty
        ~fail
        ~lhs:{ sub_supportdyn; ty_sub }
        ~rhs:{ super_like; super_supportdyn = false; ty_super }
        env
    | ( ( _,
          ( Tany _ | Toption _ | Tnonnull | Tdynamic _ | Tprim _ | Tfun _
          | Ttuple _ | Tshape _ | Tintersection _ | Tvec_or_dict _ | Taccess _
          | Tnewtype _ | Tdependent _ | Tclass _ | Tneg _ | Tgeneric _
          | Tlabel _ | Tclass_ptr _ ) ),
        (_, Tgeneric _) )
      when subtype_env.Subtype_env.require_completeness ->
      mk_issubtype_prop
        ~sub_supportdyn
        ~is_dynamic_aware:subtype_env.Subtype_env.is_dynamic_aware
        env
        (LoclType ty_sub)
        (LoclType ty_super)
    (* When we have a shape splat <: an opaque type we lift that type into a
       singleton shape splat so that any assignment of a type parameter is
       consistent on both sides of the subtype proposition *)
    | ((r_sub, Tshape (Shape_splat _ as shape_sub)), (r_super, Tgeneric _)) ->
      simplify_subtype_shape_splat
        ~subtype_env
        ~env
        ~this_ty
        ~super_like
        (Option.is_some sub_supportdyn, r_sub, shape_sub)
        (false, r_super, Shape_splat { ss_elems = [ty_super] })
    | ( (r_sub, Tshape (Shape_splat _ as shape_sub)),
        (r_super, Tnewtype (n, _, _)) )
      when (* [supportdyn] is itself a [Tnewtype] and has its own handling. *)
           (not (String.equal n SN.Classes.cSupportDyn))
           && not (String.equal n SN.Classes.cRepresentableAs) ->
      simplify_subtype_shape_splat
        ~subtype_env
        ~env
        ~this_ty
        ~super_like
        (Option.is_some sub_supportdyn, r_sub, shape_sub)
        (false, r_super, Shape_splat { ss_elems = [ty_super] })
    | ( ( _r_sub,
          ( Tany _ | Toption _ | Tnonnull | Tdynamic _ | Tprim _ | Tfun _
          | Ttuple _ | Tshape _ | Tintersection _ | Tvec_or_dict _ | Taccess _
          | Tnewtype _ | Tdependent _ | Tclass _ | Tneg _ | Tgeneric _
          | Tlabel _ | Tclass_ptr _ ) ),
        (r_super, Tgeneric name_super) ) ->
      (* If we've seen this type parameter before then we must have gone
       * round a cycle so we fail
       *)
      (match
         VisitedGoals.try_add_visited_generic_super
           subtype_env.Subtype_env.visited
           ty_sub
           name_super
       with
      | None -> invalid env ~fail
      | Some new_visited ->
        let (_generic_lower_bounds, other_lower_bounds) =
          generic_lower_bounds env ty_super
        in
        let subtype_env = Subtype_env.set_visited subtype_env new_visited in

        (* Collect all the lower bounds ("super" constraints) on the
         * generic parameter, and check ty_sub against each of them in turn
         * until one of them succeeds *)
        let rec aux lower_bounds ~env ~errs ~props =
          match lower_bounds with
          | [] ->
            let (env, prop) =
              default_subtype
                ~subtype_env
                ~this_ty
                ~fail
                ~lhs:{ sub_supportdyn; ty_sub }
                ~rhs:{ super_like; super_supportdyn = false; ty_super }
                env
            in
            if TL.is_valid prop then
              (env, prop)
            else
              let (errs, props) =
                match TL.get_error_if_unsat prop with
                | Some err -> (err :: errs, props)
                | _ -> (errs, prop :: props)
              in
              let err = Typing_error.intersect_opt @@ List.filter_opt errs in
              (env, TL.Disj (err, props))
          | lower_bound :: lower_bounds ->
            let lower_bound =
              Typing_env.update_reason env lower_bound ~f:(fun bound ->
                  Typing_reason.axiom_lower_bound ~bound ~of_:r_super)
            in
            let (env, prop) =
              simplify
                ~subtype_env
                ~this_ty
                ~lhs:{ sub_supportdyn; ty_sub }
                ~rhs:
                  {
                    super_like;
                    super_supportdyn = false;
                    ty_super = lower_bound;
                  }
                env
            in
            if TL.is_valid prop then
              (env, prop)
            else
              let (errs, props) =
                match TL.get_error_if_unsat prop with
                | Some err -> (err :: errs, props)
                | _ -> (errs, prop :: props)
              in
              aux lower_bounds ~env ~errs ~props
        in

        (* Turn error into a generic error about the type parameter *)
        let lower_bounds = Typing_set.elements other_lower_bounds in
        aux lower_bounds ~env ~errs:[fail] ~props:[])
    | ( ( _,
          ( Tprim
              Ast_defs.(
                Tint | Tbool | Tfloat | Tresource | Tnum | Tarraykey | Tnoreturn)
          | Tnonnull | Tfun _ | Ttuple _ | Tshape _ | Tclass _ | Tvec_or_dict _
          | Taccess _ | Tlabel _ | Tclass_ptr _ ) ),
        (_, Tnonnull) ) ->
      valid env
    (* supportdyn<t> <: nonnull iff t <: nonnull *)
    | ((r, Tnewtype (name, [upper_bound], _)), (r_nonnull, Tnonnull))
      when String.equal name SN.Classes.cSupportDyn ->
      (* TODO(mjt) update to be an asymm projection *)
      let ty_sub =
        Typing_env.update_reason env upper_bound ~f:(fun bound ->
            Typing_reason.axiom_upper_bound ~bound ~of_:r ~name)
      in
      env
      |> simplify
           ~subtype_env
           ~this_ty
           ~lhs:{ sub_supportdyn = Some r; ty_sub }
           ~rhs:
             {
               super_like = false;
               super_supportdyn = false;
               ty_super = mk (r_nonnull, Tnonnull);
             }
    (* negations always contain null *)
    | ((_, Tneg _), (_, Tnonnull)) -> invalid ~fail env
    | ( ( _,
          ( Tany _ | Tunion _ | Toption _ | Tintersection _ | Tdynamic _
          | Tprim Aast.(Tvoid | Tnull)
          | Tvar _ | Tgeneric _ | Tnewtype _ | Tdependent _ ) ),
        (_, Tnonnull) ) ->
      default_subtype
        ~subtype_env
        ~this_ty
        ~fail
        ~lhs:{ sub_supportdyn; ty_sub }
        ~rhs:{ super_like; super_supportdyn = false; ty_super }
        env
    (* -- C-Dynamic-R ------------------------------------------------------- *)
    | (_, (r_dynamic, Tdynamic _))
      when subtype_env.is_dynamic_aware
           || Tast.is_under_dynamic_assumptions env.checked -> begin
      let dyn =
        lazy
          (Pretty.describe_ty_super ~is_coeffect:false env (LoclType ty_super))
      in
      let dynamic_part =
        Lazy.map dyn ~f:(fun dyn ->
            Reason.to_string ("Expected " ^ dyn) r_dynamic)
      and ty_name = lazy (Pretty.describe_ty_default env (LoclType ty_sub))
      and pos = Reason.to_pos (get_reason ty_sub) in
      let postprocess =
        if_unsat
          (invalid
             ~fail:
               (Option.map
                  subtype_env.Subtype_env.on_error
                  ~f:
                    Typing_error.(
                      fun on_error ->
                        apply_reasons ~on_error
                        @@ Secondary.Not_sub_dynamic
                             { pos; ty_name; dynamic_part })))
      in
      postprocess
      @@
      if Option.is_some sub_supportdyn then
        valid env
      else
        match deref ty_sub with
        | (_, Tlabel _)
        | (_, Tany _)
        | ( _,
            Tprim
              Ast_defs.(
                ( Tint | Tbool | Tfloat | Tnum | Tarraykey | Tvoid | Tnoreturn
                | Tresource )) ) ->
          valid env
        | (_, Tnewtype (name_sub, [_tyarg_sub], _))
          when String.equal name_sub SN.Classes.cSupportDyn ->
          valid env
        | (_, Tnewtype (name_sub, _, _))
          when String.equal name_sub SN.Classes.cEnumClassLabel ->
          valid env
        (* class_or_classname<T> <D: dynamic, mirroring class<T>: valid by
         * exposure to string when class_sub_classname is set, otherwise valid
         * when the type argument is itself <D: dynamic. Without an explicit arm
         * it falls through to the generic Tnewtype case below, which uses the
         * `as nonnull` bound and so fails. *)
        | (_, Tnewtype (name_sub, _, _))
          when String.equal name_sub SN.Classes.cClassOrClassname
               && subtype_env.Subtype_env.class_sub_classname ->
          valid env
        | (_, Tnewtype (name_sub, [ty], _))
          when String.equal name_sub SN.Classes.cClassOrClassname ->
          simplify
            ~subtype_env
            ~this_ty:None
            ~lhs:{ sub_supportdyn = None; ty_sub = ty }
            ~rhs:{ super_like = false; super_supportdyn = false; ty_super }
            env
        | (r_sub, Toption ty) ->
          (match deref ty with
          (* Special case mixed <: dynamic for better error message *)
          | (_, Tnonnull) -> invalid env ~fail
          | _ ->
            let ty =
              Typing_env.update_reason env ty ~f:(fun r_sub_prj ->
                  Typing_reason.prj_nullable_sub ~sub:r_sub ~sub_prj:r_sub_prj)
            in
            simplify
              ~subtype_env
              ~this_ty:None
              ~lhs:{ sub_supportdyn; ty_sub = ty }
              ~rhs:{ super_like = false; super_supportdyn = false; ty_super }
              env)
        | (_, (Tdynamic _ | Tprim Ast_defs.Tnull)) -> valid env
        | (_, Tnonnull)
        | (_, Tvar _)
        | (_, Tnewtype _)
        | (_, Tdependent _)
        | (_, Taccess _)
        | (_, Tunion _)
        | (_, Tintersection _)
        | (_, Tgeneric _)
        | (_, Tneg _) ->
          default_subtype
            ~subtype_env
            ~this_ty
            ~fail
            ~lhs:{ sub_supportdyn; ty_sub }
            ~rhs:{ super_like; super_supportdyn = false; ty_super }
            env
        | (_, Tvec_or_dict (_, ty)) ->
          simplify
            ~subtype_env
            ~this_ty:None
            ~lhs:{ sub_supportdyn; ty_sub = ty }
            ~rhs:{ super_like = false; super_supportdyn = false; ty_super }
            env
        | (r_sub, Tfun ft_sub) ->
          if get_ft_support_dynamic_type ft_sub then
            valid env
          else
            (* Special case of function type subtype dynamic.
             *   (function(ty1,...,tyn):ty <: supportdyn<nonnull>)
             *   iff
             *   dynamic <D: ty1 & ... & dynamic <D: tyn & ty <D: dynamic
             *)
            (* Contravariant subtyping on parameters *)
            let param_props =
              simplify_supertype_params_with_variadic
                ~subtype_env
                (r_sub, 0, ft_sub.ft_params)
                (r_dynamic, 0, ty_super)
            in
            (* Finally do covariant subtyping on return type *)
            let ret_prop =
              let ty_sub =
                Typing_env.update_reason env ft_sub.ft_ret ~f:(fun r_sub_prj ->
                    Typing_reason.prj_fn_ret
                      ~sub:r_sub
                      ~sub_prj:r_sub_prj
                      ~super:r_dynamic)
              in
              simplify
                ~subtype_env
                ~this_ty:None
                ~lhs:{ sub_supportdyn; ty_sub }
                ~rhs:{ super_like = false; super_supportdyn = false; ty_super }
            in
            param_props env &&& ret_prop
        | (_, Ttuple { t_required; t_optional; t_extra }) ->
          let extras =
            match t_extra with
            | Tvariadic t_variadic -> t_variadic :: t_optional
            | Tsplat t_splat -> [t_splat]
          in
          List.fold_left
            ~init:(env, TL.valid)
            ~f:(fun res ty_sub ->
              res
              &&& simplify
                    ~subtype_env
                    ~this_ty:None
                    ~lhs:{ sub_supportdyn; ty_sub }
                    ~rhs:
                      { super_like = false; super_supportdyn = false; ty_super })
            (extras @ t_required)
        | ( _,
            Tshape
              (Shape_simple
                {
                  s_origin = _;
                  s_unknown_value = unknown_fields_type;
                  s_fields = sftl;
                }) ) ->
          List.fold_left
            ~init:(env, TL.valid)
            ~f:(fun res sft ->
              res
              &&& simplify
                    ~subtype_env
                    ~this_ty:None
                    ~lhs:{ sub_supportdyn; ty_sub = sft.sft_ty }
                    ~rhs:
                      { super_like = false; super_supportdyn = false; ty_super })
            (TShapeMap.values sftl)
          &&& simplify
                ~subtype_env
                ~this_ty:None
                ~lhs:{ sub_supportdyn; ty_sub = unknown_fields_type }
                ~rhs:{ super_like = false; super_supportdyn = false; ty_super }
        | (_, Tshape (Shape_splat { ss_elems })) ->
          (* Each element of the splat must be <: dynamic *)
          List.fold_left ss_elems ~init:(valid env) ~f:(fun res elem_ty ->
              res
              &&& simplify
                    ~subtype_env
                    ~this_ty:None
                    ~lhs:{ sub_supportdyn; ty_sub = elem_ty }
                    ~rhs:
                      { super_like = false; super_supportdyn = false; ty_super })
        (* class<T> <D: dynamic by exposure to string *)
        | (_, Tclass_ptr _) when subtype_env.Subtype_env.class_sub_classname ->
          valid env
        (* class<T> <D: dynamic if T <D: dynamic (class is SDT) *)
        | (_, Tclass_ptr ty) ->
          simplify
            ~subtype_env
            ~this_ty:None
            ~lhs:{ sub_supportdyn = None; ty_sub = ty }
            ~rhs:{ super_like = false; super_supportdyn = false; ty_super }
            env
        | (_, Tclass ((_, class_id), _exact, tyargs)) ->
          let class_def_sub = Env.get_class env class_id in
          (match class_def_sub with
          | Decl_entry.DoesNotExist
          | Decl_entry.NotYetAvailable ->
            (* This should have been caught already in the naming phase *)
            valid env
          | Decl_entry.Found class_sub ->
            if
              Cls.get_support_dynamic_type class_sub || Env.is_enum env class_id
            then
              (* If a class has the __SupportDynamicType annotation, then
                 a type formed from it is a dynamic-aware subtype of dynamic if
                 the type arguments are correctly supplied, which depends on the
                 variance of the parameter, and whether the __RequireDynamic
                 is on the parameter.
              *)
              let rec subtype_args tparams tyargs env =
                match (tparams, tyargs) with
                | ([], _) -> valid env
                | (_, []) ->
                  (* If there are missing type arguments, we don't know that they are subtypes of dynamic, unless the bounds enforce that *)
                  invalid env ~fail
                | (tp :: tparams, tyarg :: tyargs) ->
                  let has_require_dynamic =
                    Attributes.mem
                      SN.UserAttributes.uaRequireDynamic
                      tp.tp_user_attributes
                  in
                  (if
                   has_require_dynamic
                   (* Implicit pessimisation should ignore the RequireDynamic attribute
                      because everything should be pessimised enough that it isn't necessary. *)
                   && not (Typechecker_options.everything_sdt env.genv.tcopt)
                  then
                    (* If the class is marked <<__SupportDynamicType>> then for any
                       * type parameters marked <<__RequireDynamic>> then the class does not
                       * unconditionally implement dynamic, but rather we must check that
                       * it is a subtype of the same type whose corresponding type arguments
                       * are replaced by dynamic, intersected with the parameter's upper bounds.
                       *
                       * For example, to check dict<int,float> <: supportdyn<nonnull>
                       * we check dict<int,float> <D: dict<arraykey,dynamic>
                       * which in turn requires int <D: arraykey and float <D: dynamic.
                    *)
                    let upper_bounds =
                      List.filter_map tp.tp_constraints ~f:(fun (c, ty) ->
                          match c with
                          | Ast_defs.Constraint_as ->
                            let (_env, ty) =
                              Phase.localize_no_subst env ~ignore_errors:true ty
                            in
                            Some ty
                          | _ -> None)
                    in
                    let super =
                      MakeType.intersection r_dynamic (ty_super :: upper_bounds)
                    in
                    match tp.tp_variance with
                    | Ast_defs.Covariant ->
                      simplify
                        ~subtype_env
                        ~this_ty:None
                        ~lhs:{ sub_supportdyn = None; ty_sub = tyarg }
                        ~rhs:
                          {
                            super_like = false;
                            super_supportdyn = false;
                            ty_super = super;
                          }
                        env
                    | Ast_defs.Contravariant ->
                      simplify
                        ~subtype_env
                        ~this_ty:None
                        ~lhs:{ sub_supportdyn = None; ty_sub = super }
                        ~rhs:
                          {
                            super_like = false;
                            super_supportdyn = false;
                            ty_super = tyarg;
                          }
                        env
                    | Ast_defs.Invariant ->
                      simplify
                        ~subtype_env
                        ~this_ty:None
                        ~lhs:{ sub_supportdyn = None; ty_sub = tyarg }
                        ~rhs:
                          {
                            super_like = false;
                            super_supportdyn = false;
                            ty_super = super;
                          }
                        env
                      &&& simplify
                            ~subtype_env
                            ~this_ty:None
                            ~lhs:{ sub_supportdyn = None; ty_sub = super }
                            ~rhs:
                              {
                                super_like = false;
                                super_supportdyn = false;
                                ty_super = tyarg;
                              }
                  else
                    (* If the class is marked <<__SupportDynamicType>> then for any
                       * type parameters not marked <<__RequireDynamic>> then the class is a
                       * subtype of dynamic only when the arguments are also subtypes of dynamic.
                    *)
                    match tp.tp_variance with
                    | Ast_defs.Covariant
                    | Ast_defs.Invariant ->
                      simplify
                        ~subtype_env
                        ~this_ty:None
                        ~lhs:{ sub_supportdyn = None; ty_sub = tyarg }
                        ~rhs:
                          {
                            super_like = false;
                            super_supportdyn = false;
                            ty_super;
                          }
                        env
                    | Ast_defs.Contravariant ->
                      (* If the parameter is contra-variant, then we only need to
                         check that the lower bounds (if present) are subtypes of
                         dynamic. For example, given <<__SDT>> class C<-T> {...},
                         then for any t, C<t> <: C<nothing>, and since
                         `nothing <D: dynamic`, `C<nothing> <D: dynamic` and so
                         `C<t> <D: dynamic`. If there are lower bounds, we can't
                         push the argument below them. It suffices to check only
                         them because if one of them is not <D: dynamic, then
                         none of their supertypes are either.
                      *)
                      let lower_bounds =
                        List.filter_map tp.tp_constraints ~f:(fun (c, ty) ->
                            match c with
                            | Ast_defs.Constraint_super ->
                              let (_env, ty) =
                                Phase.localize_no_subst
                                  env
                                  ~ignore_errors:true
                                  ty
                              in
                              Some ty
                            | _ -> None)
                      in
                      (match lower_bounds with
                      | [] -> valid env
                      | _ ->
                        let sub = MakeType.union r_dynamic lower_bounds in
                        simplify
                          ~subtype_env
                          ~this_ty:None
                          ~lhs:{ sub_supportdyn = None; ty_sub = sub }
                          ~rhs:
                            {
                              super_like = false;
                              super_supportdyn = false;
                              ty_super;
                            }
                          env))
                  &&& subtype_args tparams tyargs
              in
              subtype_args (Cls.tparams class_sub) tyargs env
            else (
              match Cls.kind class_sub with
              | Ast_defs.Cenum_class _ ->
                (match Cls.enum_type class_sub with
                | Some enum_type ->
                  let ((env, _ty_err_opt), subtype) =
                    TUtils.localize_no_subst
                      ~ignore_errors:true
                      env
                      enum_type.te_base
                  in
                  simplify
                    ~subtype_env
                    ~this_ty:None
                    ~lhs:{ sub_supportdyn = None; ty_sub = subtype }
                    ~rhs:
                      { super_like = false; super_supportdyn = false; ty_super }
                    env
                | None ->
                  default_subtype
                    ~subtype_env
                    ~this_ty
                    ~fail
                    ~lhs:{ sub_supportdyn; ty_sub }
                    ~rhs:{ super_like; super_supportdyn = false; ty_super }
                    env)
              | _ ->
                default_subtype
                  ~subtype_env
                  ~this_ty
                  ~fail
                  ~lhs:{ sub_supportdyn; ty_sub }
                  ~rhs:{ super_like; super_supportdyn = false; ty_super }
                  env
            ))
    end
    | (_, (_, Tdynamic _)) when is_dynamic ty_sub -> valid env
    | (_, (_, Tdynamic _)) ->
      default_subtype
        ~subtype_env
        ~this_ty
        ~fail
        ~lhs:{ sub_supportdyn; ty_sub }
        ~rhs:{ super_supportdyn; super_like; ty_super }
        env
    (* -- C-Prim-R ---------------------------------------------------------- *)
    | ((_, Tprim (Nast.Tint | Nast.Tfloat)), (_, Tprim Nast.Tnum)) -> valid env
    (* class<T> <: string because of typename's bound *)
    | ((_, Tclass_ptr _), (_, Tprim Nast.Tarraykey))
      when subtype_env.Subtype_env.class_sub_classname ->
      valid env
    | ((_, Tclass_ptr _), (_, Tclass ((_, id), _, _)))
      when subtype_env.Subtype_env.class_sub_classname
           && String.equal SN.Classes.cString id ->
      valid env
    | ((_, Tprim Nast.Tint), (_, Tprim Nast.Tarraykey)) -> valid env
    | ((_, Tclass ((_, id), _, _)), (_, Tprim Nast.Tarraykey))
      when String.equal SN.Classes.cString id ->
      valid env
    | ((_, Tprim prim_sub), (_, Tprim prim_sup))
      when Aast.equal_tprim prim_sub prim_sup ->
      valid env
    | ((_, Tprim _), (_, Tprim _)) -> invalid env ~fail
    | ((r_sub, Toption arg_ty_sub), (_r_super, Tprim Nast.Tnull)) ->
      let arg_ty_sub =
        Typing_env.update_reason env arg_ty_sub ~f:(fun r_sub_prj ->
            Typing_reason.prj_nullable_sub ~sub:r_sub ~sub_prj:r_sub_prj)
      in
      simplify
        ~subtype_env
        ~this_ty
        ~lhs:{ sub_supportdyn; ty_sub = arg_ty_sub }
        ~rhs:{ super_like = false; super_supportdyn = false; ty_super }
        env
    | ( ( _,
          ( Tany _ | Tdynamic _ | Tunion _ | Toption _ | Tintersection _
          | Tdependent _ | Taccess _ | Tgeneric _ | Tnonnull | Tfun _ | Ttuple _
          | Tshape _ | Tvec_or_dict _ | Tclass _ | Tnewtype _ | Tneg _ | Tvar _
          | Tlabel _ | Tclass_ptr _ ) ),
        (_, Tprim _) ) ->
      default_subtype
        ~subtype_env
        ~this_ty
        ~fail
        ~lhs:{ sub_supportdyn; ty_sub }
        ~rhs:{ super_like; super_supportdyn = false; ty_super }
        env
    (* -- C-Any-R ----------------------------------------------------------- *)
    | ((_, Tany _), (_, Tany _)) -> valid env
    | ((_, (Tunion _ | Tintersection _ | Tvar _)), (_, Tany _)) ->
      default_subtype
        ~subtype_env
        ~this_ty
        ~fail
        ~lhs:{ sub_supportdyn; ty_sub }
        ~rhs:{ super_supportdyn; super_like; ty_super }
        env
    | (_, (_, Tany _)) when subtype_env.Subtype_env.no_top_bottom ->
      mk_issubtype_prop
        ~sub_supportdyn
        ~is_dynamic_aware:subtype_env.Subtype_env.is_dynamic_aware
        env
        (LoclType ty_sub)
        (LoclType ty_super)
    | (_, (_, Tany _)) -> valid env
    (* -- C-Fun ------------------------------------------------------------- *)
    | ((r_sub, Tfun ft_sub), (r_super, Tfun ft_super)) ->
      (* If function type in supertype position is polymorphic we will move
         through a binder. In doing so we need to ensure that any type parameters
         bound at this higher-rank do not escape into the bounds of any
         lower ranked type variables. To do this we increase the current rank
         before binding the type parameters and instantiating the subtype (if
         it is polymorphic). We then decrease the rank again before returning.

         If only the subtype is polymorphic we don't change ranks because we
         aren't going though a binder - we are just instantiating. *)
      let increase_rank = not ft_super.ft_instantiated in
      let env =
        if increase_rank then
          Typing_env_types.increment_rank env
        else
          env
      in
      let rank = Typing_env_types.get_rank env in
      let ((env, err_opt), ft_sub) =
        if ft_sub.ft_instantiated then
          ((env, None), ft_sub)
        else
          let pos =
            Pos_or_decl.unsafe_to_raw_pos @@ Typing_reason.to_pos r_sub
          in
          (* Replace all quantifiers with fresh type variables adding any declared
             bounds *)
          Instantiate.instantiate_fun_type
            ?on_error:subtype_env.Subtype_env.on_error
            pos
            ft_sub
            rank
            ~env
      in
      (* Handle any error - we can fail to instantiate if the function type has
         unsatisfiable bounds *)
      if Option.is_some err_opt then
        (env, TL.invalid ~fail:err_opt)
      else
        let (env, ft_super) =
          if ft_super.ft_instantiated then
            (env, ft_super)
          else
            (* Freshen all type parameters wrt to the enclosing context then
               bind them *)
            Instantiate.freshen_and_bind_fun_type_quantifiers
              r_super
              ft_super
              rank
              ~env
        in
        (* Simplify the subtype proposition between the instantiated subtype
           and the the supertype in the modified context *)
        let (env, prop) =
          simplify_funs
            ~subtype_env
            ~check_return:true
            ~for_override:false
            ~super_like
            (r_sub, ft_sub)
            (r_super, ft_super)
            env
        in
        (* If we incremented the rank during the instantiation of polymorphic
           function types we need to decrement it here *)
        let env =
          if increase_rank then
            Typing_env_types.decrement_rank env
          else
            env
        in
        (env, prop)
    | ( ( _,
          ( Tany _ | Tunion _ | Toption _ | Tintersection _ | Tgeneric _
          | Taccess _ | Tnewtype _ | Tprim _ | Tnonnull | Tclass _
          | Tvec_or_dict _ | Ttuple _ | Tshape _ | Tdynamic _ | Tneg _
          | Tdependent _ | Tvar _ | Tlabel _ | Tclass_ptr _ ) ),
        (_, Tfun _) ) ->
      default_subtype
        ~subtype_env
        ~this_ty
        ~fail
        ~lhs:{ sub_supportdyn; ty_sub }
        ~rhs:{ super_supportdyn; super_like; ty_super }
        env
      (* Tuple against tuple. *)
    | ( ( r_sub,
          Ttuple
            {
              t_required = t_req_sub;
              t_optional = t_optional_sub;
              t_extra = t_extra_sub;
            } ),
        ( r_super,
          Ttuple
            {
              t_required = t_req_super;
              t_optional = t_optional_super;
              t_extra = t_extra_super;
            } ) ) ->
      (* First test the required components of the tuples, pointwise *)
      let rec simplify_elems_req ty_subs ty_supers env =
        match (ty_subs, ty_supers) with
        (* We have required elements in the supertype that can't be satisfied by the subtype *)
        | ([], _ :: _) -> begin
          match t_extra_sub with
          | Tsplat t_splat_sub ->
            let tuple_super_ty =
              mk
                ( r_super,
                  Ttuple
                    {
                      t_required = ty_supers;
                      t_optional = t_optional_super;
                      t_extra = t_extra_super;
                    } )
            in
            let ty_super = Sd.liken ~super_like env tuple_super_ty in
            env
            |> simplify
                 ~subtype_env
                 ~this_ty:None
                 ~lhs:{ sub_supportdyn; ty_sub = t_splat_sub }
                 ~rhs:{ super_like = false; super_supportdyn = false; ty_super }
          | Tvariadic _ -> invalid env ~fail
        end
        (* Pointwise compare sub and super *)
        | (ty_sub :: ty_subs, ty_super :: ty_supers) ->
          let ty_super = Sd.liken ~super_like env ty_super in
          env
          |> simplify
               ~subtype_env
               ~this_ty:None
               ~lhs:{ sub_supportdyn; ty_sub }
               ~rhs:{ super_like = false; super_supportdyn = false; ty_super }
          &&& simplify_elems_req ty_subs ty_supers
        (* We've run out of required elements in the supertype *)
        | (_, []) -> begin
          match t_extra_super with
          (* But we can match the rest of the subtype against a splat in the supertype
           * i.e. (ty_subs, optional t_optional_sub, tvariadic_sub...) <: t_splat_super
           *)
          | Tsplat t_splat_super ->
            let tuple_sub_ty =
              mk
                ( r_sub,
                  Ttuple
                    {
                      t_required = ty_subs;
                      t_optional = t_optional_sub;
                      t_extra = t_extra_sub;
                    } )
            in
            let ty_super = Sd.liken ~super_like env t_splat_super in
            env
            |> simplify
                 ~subtype_env
                 ~this_ty:None
                 ~lhs:{ sub_supportdyn; ty_sub = tuple_sub_ty }
                 ~rhs:{ super_like = false; super_supportdyn = false; ty_super }
          | Tvariadic t_variadic_super -> begin
            (* Shortcut so that error refers to whole tuple *)
            if
              List.length ty_subs + List.length t_optional_sub
              > List.length t_optional_super
              && is_nothing t_variadic_super
            then
              invalid env ~fail
            else
              let rec simplify_elems_opt ty_subs ty_supers env =
                begin
                  match (ty_subs, ty_supers) with
                  | (ty_sub :: ty_subs, []) ->
                    let ty_super = Sd.liken ~super_like env t_variadic_super in
                    env
                    |> simplify
                         ~subtype_env
                         ~this_ty:None
                         ~lhs:{ sub_supportdyn; ty_sub }
                         ~rhs:
                           {
                             super_like = false;
                             super_supportdyn = false;
                             ty_super;
                           }
                    &&& simplify_elems_opt ty_subs ty_supers
                  (* We've run out of elements in the subtype *)
                  | ([], _) -> begin
                    match t_extra_sub with
                    | Tsplat t_splat_sub ->
                      let tuple_super_ty =
                        mk
                          ( r_super,
                            Ttuple
                              {
                                t_required = [];
                                t_optional = ty_supers;
                                t_extra = Tvariadic t_variadic_super;
                              } )
                      in
                      let ty_super = Sd.liken ~super_like env tuple_super_ty in
                      env
                      |> simplify
                           ~subtype_env
                           ~this_ty:None
                           ~lhs:{ sub_supportdyn; ty_sub = t_splat_sub }
                           ~rhs:
                             {
                               super_like = false;
                               super_supportdyn = false;
                               ty_super;
                             }
                    | Tvariadic t_variadic_sub -> begin
                      match ty_supers with
                      | [] ->
                        let ty_super =
                          Sd.liken ~super_like env t_variadic_super
                        in
                        env
                        |> simplify
                             ~subtype_env
                             ~this_ty:None
                             ~lhs:{ sub_supportdyn; ty_sub = t_variadic_sub }
                             ~rhs:
                               {
                                 super_like = false;
                                 super_supportdyn = false;
                                 ty_super;
                               }
                      | ty_super :: ty_supers ->
                        let ty_super = Sd.liken ~super_like env ty_super in
                        env
                        |> simplify
                             ~subtype_env
                             ~this_ty:None
                             ~lhs:{ sub_supportdyn; ty_sub = t_variadic_sub }
                             ~rhs:
                               {
                                 super_like = false;
                                 super_supportdyn = false;
                                 ty_super;
                               }
                        &&& simplify_elems_opt [] ty_supers
                    end
                  end
                  | (ty_sub :: ty_subs, ty_super :: ty_supers) ->
                    let ty_super = Sd.liken ~super_like env ty_super in
                    env
                    |> simplify
                         ~subtype_env
                         ~this_ty:None
                         ~lhs:{ sub_supportdyn; ty_sub }
                         ~rhs:
                           {
                             super_like = false;
                             super_supportdyn = false;
                             ty_super;
                           }
                    &&& simplify_elems_opt ty_subs ty_supers
                end
              in
              env
              |> simplify_elems_opt (ty_subs @ t_optional_sub) t_optional_super
          end
        end
      in
      env |> simplify_elems_req t_req_sub t_req_super
    | (_, (_, Ttuple _)) ->
      default_subtype
        ~subtype_env
        ~this_ty
        ~fail
        ~lhs:{ sub_supportdyn; ty_sub }
        ~rhs:{ super_supportdyn; super_like; ty_super }
        env
    (* Keep this before the shape-splat case below: constraint decomposition must
       retain [T <: shape(...)] so [T]'s declared upper bound is recorded. *)
    | ((_, Tgeneric _), (_, Tshape (Shape_splat _)))
      when subtype_env.Subtype_env.require_completeness ->
      mk_issubtype_prop
        ~sub_supportdyn
        ~is_dynamic_aware:subtype_env.Subtype_env.is_dynamic_aware
        env
        (LoclType ty_sub)
        (LoclType ty_super)
    (* As above, wrap a opaque types in a singleton splat when subtyping
       against a shape splat to ensure all assignments of type parameters
       are consistent *)
    | ( (r_sub, (Tgeneric _ | Tnewtype _)),
        (r_super, Tshape (Shape_splat _ as shape_super)) ) ->
      simplify_subtype_shape_splat
        ~subtype_env
        ~env
        ~this_ty
        ~super_like
        ( Option.is_some sub_supportdyn,
          r_sub,
          Shape_splat { ss_elems = [ty_sub] } )
        (super_supportdyn, r_super, shape_super)
    (* -- C-Shape-R --------------------------------------------------------- *)
    | ((r_sub, Tnewtype (name_sub, tyl, _)), (_r_super, Tshape _)) ->
      let (env, ty_bound_sub) =
        Typing_utils.get_newtype_super env r_sub name_sub tyl
      in
      Common.simplify_newtype_l
        ~subtype_env
        ~this_ty
        ~mk_prop:simplify
        (sub_supportdyn, r_sub, name_sub, ty_bound_sub)
        rhs
        env
    | ( ( r_sub,
          Tshape
            (Shape_simple
              {
                s_origin = origin_sub;
                s_unknown_value = shape_kind_sub;
                s_fields = fdm_sub;
              }) ),
        ( r_super,
          Tshape
            (Shape_simple
              {
                s_origin = origin_super;
                s_unknown_value = shape_kind_super;
                s_fields = fdm_super;
              }) ) ) ->
      if same_type_origin origin_super origin_sub then
        (* Fast path for shape types: if they have the same origin,
         * they are equal type. *)
        valid env
      else
        let sub_supportdyn = Option.is_some sub_supportdyn in
        simplify_subtype_shape
          ~subtype_env
          ~env
          ~this_ty
          ~super_like
          (sub_supportdyn, r_sub, shape_kind_sub, fdm_sub)
          (super_supportdyn, r_super, shape_kind_super, fdm_super)
    | ((r_sub, Tshape shape_sub), (r_super, Tshape shape_super)) ->
      let sub_supportdyn = Option.is_some sub_supportdyn in
      simplify_subtype_shape_splat
        ~subtype_env
        ~env
        ~this_ty
        ~super_like
        (sub_supportdyn, r_sub, shape_sub)
        (super_supportdyn, r_super, shape_super)
    | ( ( _,
          ( Tany _ | Tunion _ | Toption _ | Tintersection _ | Tfun _
          | Tgeneric _ | Taccess _ | Tprim _ | Tnonnull | Tclass _
          | Tvec_or_dict _ | Ttuple _ | Tdynamic _ | Tneg _ | Tdependent _
          | Tvar _ | Tlabel _ | Tclass_ptr _ ) ),
        (_, Tshape _) ) ->
      default_subtype
        ~subtype_env
        ~this_ty
        ~fail
        ~lhs:{ sub_supportdyn; ty_sub }
        ~rhs:{ super_supportdyn; super_like; ty_super }
        env
    (* -- C-Vec-or-Dict-R --------------------------------------------------- *)
    | ( (_, Tvec_or_dict (lty_key_sub, lty_val_sub)),
        (_r_super, Tvec_or_dict (lty_key_sup, lty_val_sup)) ) ->
      let lty_val_sup = Sd.liken ~super_like env lty_val_sup in
      let lty_key_sup = Sd.liken ~super_like env lty_key_sup in
      env
      |> simplify
           ~subtype_env
           ~this_ty
           ~lhs:{ sub_supportdyn; ty_sub = lty_key_sub }
           ~rhs:
             {
               super_like = false;
               super_supportdyn = false;
               ty_super = lty_key_sup;
             }
      &&& simplify
            ~subtype_env
            ~this_ty
            ~lhs:{ sub_supportdyn; ty_sub = lty_val_sub }
            ~rhs:
              {
                super_like = false;
                super_supportdyn = false;
                ty_super = lty_val_sup;
              }
    | ( (_, Tclass ((_, n), _, [lty_key_sub; lty_val_sub])),
        (_r_super, Tvec_or_dict (lty_key_sup, lty_val_sup)) )
      when String.equal n SN.Collections.cDict ->
      let lty_val_sup = Sd.liken ~super_like env lty_val_sup in
      let lty_key_sup = Sd.liken ~super_like env lty_key_sup in
      env
      |> simplify
           ~subtype_env
           ~this_ty
           ~lhs:{ sub_supportdyn; ty_sub = lty_key_sub }
           ~rhs:
             {
               super_like = false;
               super_supportdyn = false;
               ty_super = lty_key_sup;
             }
      &&& simplify
            ~subtype_env
            ~this_ty
            ~lhs:{ sub_supportdyn; ty_sub = lty_val_sub }
            ~rhs:
              {
                super_like = false;
                super_supportdyn = false;
                ty_super = lty_val_sup;
              }
    | ( (_, Tclass ((_, n), _, [lty_val_sub])),
        (_r_super, Tvec_or_dict (lty_key_sup, lty_val_sup)) )
      when String.equal n SN.Collections.cVec ->
      let pos = get_pos ty_sub in
      let lty_key_sub = MakeType.int (Reason.idx_vector_from_decl pos) in
      let lty_val_sup = Sd.liken ~super_like env lty_val_sup in
      let lty_key_sup = Sd.liken ~super_like env lty_key_sup in
      env
      |> simplify
           ~subtype_env
           ~this_ty
           ~lhs:{ sub_supportdyn; ty_sub = lty_key_sub }
           ~rhs:
             {
               super_like = false;
               super_supportdyn = false;
               ty_super = lty_key_sup;
             }
      &&& simplify
            ~subtype_env
            ~this_ty
            ~lhs:{ sub_supportdyn; ty_sub = lty_val_sub }
            ~rhs:
              {
                super_like = false;
                super_supportdyn = false;
                ty_super = lty_val_sup;
              }
    | ( ( _,
          ( Tany _ | Tunion _ | Toption _ | Tintersection _ | Tfun _
          | Tgeneric _ | Taccess _ | Tprim _ | Tnonnull | Tclass _ | Ttuple _
          | Tshape _ | Tnewtype _ | Tdynamic _ | Tneg _ | Tdependent _ | Tvar _
          | Tlabel _ | Tclass_ptr _ ) ),
        (_, Tvec_or_dict _) ) ->
      default_subtype
        ~subtype_env
        ~this_ty
        ~fail
        ~lhs:{ sub_supportdyn; ty_sub }
        ~rhs:{ super_like; super_supportdyn = false; ty_super }
        env
    (* -- C-Newtype-R ------------------------------------------------------- *)
    (* x <: supportdyn<u> *)
    | (_, (r_supportdyn, Tnewtype (name_super, [lty_inner], bound_super)))
      when String.equal name_super SN.Classes.cSupportDyn -> begin
      match deref ty_sub with
      (* supportdyn<t> <: supportdyn<u> *)
      | (r, Tnewtype (name_sub, [tyarg_sub], _))
        when String.equal name_sub SN.Classes.cSupportDyn ->
        let ty_sub =
          Typing_env.update_reason env tyarg_sub ~f:(fun r_sub_prj ->
              Typing_reason.prj_supportdyn
                ~sub:r
                ~sub_prj:r_sub_prj
                ~super:r_supportdyn)
        and ty_super = lty_inner in
        simplify
          ~subtype_env
          ~this_ty
          ~lhs:{ sub_supportdyn = Some r; ty_sub }
          ~rhs:{ super_like; super_supportdyn = true; ty_super }
          env
      | (_, Tvar _) ->
        default_subtype
          ~subtype_env
          ~this_ty
          ~fail
          ~lhs:{ sub_supportdyn; ty_sub }
          ~rhs:
            {
              super_like;
              super_supportdyn = false;
              ty_super =
                mk
                  ( r_supportdyn,
                    Tnewtype (SN.Classes.cSupportDyn, [lty_inner], bound_super)
                  );
            }
          env
      | _ ->
        let ty_dyn = MakeType.dynamic r_supportdyn in
        simplify
          ~subtype_env
          ~this_ty
          ~lhs:{ sub_supportdyn; ty_sub }
          ~rhs:{ super_like; super_supportdyn = true; ty_super = lty_inner }
          env
        &&& simplify
              ~subtype_env:(Subtype_env.set_is_dynamic_aware subtype_env true)
              ~this_ty
              ~lhs:{ sub_supportdyn; ty_sub }
              ~rhs:
                {
                  super_like = false;
                  super_supportdyn = false;
                  ty_super = ty_dyn;
                }
    end
    (* -- C-RepresentableAs-R ------------------------------------------------- *)
    (* x <: RepresentableAs<u> *)
    | ( _,
        (_r_representable_as, Tnewtype (name_super, [lty_inner], _bound_super))
      )
      when String.equal name_super SN.Classes.cRepresentableAs -> begin
      match deref ty_sub with
      (* RepresentableAs<t> <: RepresentableAs<u> ==> t <: u (covariance) *)
      | (_r_sub, Tnewtype (name_sub, [tyarg_sub], _))
        when String.equal name_sub SN.Classes.cRepresentableAs ->
        simplify
          ~subtype_env
          ~this_ty
          ~lhs:{ sub_supportdyn; ty_sub = tyarg_sub }
          ~rhs:{ super_like; super_supportdyn = false; ty_super = lty_inner }
          env
      | (_, Tvar _) ->
        default_subtype
          ~subtype_env
          ~this_ty
          ~fail
          ~lhs:{ sub_supportdyn; ty_sub }
          ~rhs:{ super_like; super_supportdyn = false; ty_super }
          env
      | (r_sub, Tprim Nast.Tvoid) ->
        (* void <: RepresentableAs<U> when null <: U OR void <: U *)
        let ( ||| ) = ( ||| ) ~fail in
        simplify
          ~subtype_env
          ~this_ty
          ~lhs:{ sub_supportdyn; ty_sub = MakeType.null r_sub }
          ~rhs:{ super_like; super_supportdyn = false; ty_super = lty_inner }
          env
        ||| simplify
              ~subtype_env
              ~this_ty
              ~lhs:{ sub_supportdyn; ty_sub }
              ~rhs:
                { super_like; super_supportdyn = false; ty_super = lty_inner }
      | (r_sub, Ttuple { t_required; t_optional; t_extra }) ->
        (* (T1, T2, ...Tn) <: RepresentableAs<U>
           when vec<T1 | T2 | ... | Tn> <: U OR (T1, T2, ...Tn) <: U *)
        let elem_tys =
          t_required
          @ t_optional
          @
          match t_extra with
          | Tvariadic ty
          | Tsplat ty ->
            [ty]
        in
        let union_ty = MakeType.union r_sub elem_tys in
        let vec_ty = MakeType.vec r_sub union_ty in
        let ( ||| ) = ( ||| ) ~fail in
        simplify
          ~subtype_env
          ~this_ty
          ~lhs:{ sub_supportdyn; ty_sub = vec_ty }
          ~rhs:{ super_like; super_supportdyn = false; ty_super = lty_inner }
          env
        ||| simplify
              ~subtype_env
              ~this_ty
              ~lhs:{ sub_supportdyn; ty_sub }
              ~rhs:
                { super_like; super_supportdyn = false; ty_super = lty_inner }
      | (r_sub, Tshape (Shape_splat { ss_elems })) ->
        let (env, result) =
          Typing_corners.resolve_for_read env r_sub ss_elems
        in
        (match result with
        | Error missing ->
          missing_shape_splat_assignment
            subtype_env
            env
            missing
            ~ty_sub:(LoclType ty_sub)
            ~ty_super:(LoclType ty_super)
        | Ok ty_sub ->
          simplify
            ~subtype_env
            ~this_ty
            ~lhs:{ sub_supportdyn; ty_sub }
            ~rhs:{ super_like; super_supportdyn = false; ty_super }
            env)
      | (r_sub, Tshape (Shape_simple { s_fields; s_unknown_value; _ })) ->
        (* shape('a' => T1, 'b' => T2, ...) <: RepresentableAs<U>
           when dict<key, V1 | V2 | ... | Vn> <: U
           OR   shape('a' => T1, 'b' => T2, ...) <: U
           For closed shapes, the key type is the union of the per-field key
           types: TSFlit_str => string, TSFregex_group => int,
           TSFclass_const => looked up from the class constant's declared type.
           For open shapes, key = arraykey.
           For open shapes, s_unknown_value is included in the value union.
           If s_unknown_value is mixed, the value union is just mixed. *)
        let is_closed = TUtils.is_nothing env s_unknown_value in
        let (env, key_ty) =
          if not is_closed then
            (env, MakeType.arraykey r_sub)
          else
            let (env, key_tys) =
              TShapeMap.fold
                (fun key _ (env, acc) ->
                  match key with
                  | TSFlit_str _ ->
                    (env, Typing_set.add (MakeType.string r_sub) acc)
                  | TSFregex_group _ ->
                    (env, Typing_set.add (MakeType.int r_sub) acc)
                  | TSFclass_const ((_, cid), (_, mid)) ->
                    (match Env.get_class env cid with
                    | Decl_entry.Found cls ->
                      (match Env.get_const env cls mid with
                      | Some const ->
                        let ((env, _ty_err_opt), lty) =
                          Phase.localize_no_subst
                            env
                            ~ignore_errors:true
                            const.cc_type
                        in
                        (env, Typing_set.add lty acc)
                      | None ->
                        (env, Typing_set.add (MakeType.arraykey r_sub) acc))
                    | Decl_entry.DoesNotExist
                    | Decl_entry.NotYetAvailable ->
                      (env, Typing_set.add (MakeType.arraykey r_sub) acc)))
                s_fields
                (env, Typing_set.empty)
            in
            (env, MakeType.union r_sub (Typing_set.elements key_tys))
        in
        let value_union =
          if TUtils.is_mixed env s_unknown_value then
            s_unknown_value
          else
            let value_tys =
              TShapeMap.fold
                (fun _key { sft_ty; _ } acc -> sft_ty :: acc)
                s_fields
                (if is_closed then
                  []
                else
                  [s_unknown_value])
            in
            MakeType.union r_sub value_tys
        in
        let dict_ty = MakeType.dict r_sub key_ty value_union in
        let ( ||| ) = ( ||| ) ~fail in
        simplify
          ~subtype_env
          ~this_ty
          ~lhs:{ sub_supportdyn; ty_sub = dict_ty }
          ~rhs:{ super_like; super_supportdyn = false; ty_super = lty_inner }
          env
        ||| simplify
              ~subtype_env
              ~this_ty
              ~lhs:{ sub_supportdyn; ty_sub }
              ~rhs:
                { super_like; super_supportdyn = false; ty_super = lty_inner }
      | (_, Tgeneric _) when subtype_env.Subtype_env.require_completeness ->
        (* In completeness mode (decompose_subtype), preserve the
           constraint shape `T <: RepresentableAs<U>` so it is stored
           as T's upper bound. The iff-U reduction would strip the RA
           wrapper, and `simplify ||| default_subtype` would produce a
           Disj that decompose_subtype_add_prop drops. default_subtype
           already defers Tgeneric in completeness mode via
           mk_issubtype_prop. *)
        default_subtype
          ~subtype_env
          ~this_ty
          ~fail
          ~lhs:{ sub_supportdyn; ty_sub }
          ~rhs:{ super_like; super_supportdyn = false; ty_super }
          env
      | (_, (Tnewtype (_, _, _) | Tgeneric _ | Tdependent _)) ->
        let ( ||| ) = ( ||| ) ~fail in
        default_subtype
          ~subtype_env
          ~this_ty
          ~fail
          ~lhs:{ sub_supportdyn; ty_sub }
          ~rhs:{ super_like; super_supportdyn = false; ty_super }
          env
        ||| fun env ->
        simplify
          ~subtype_env
          ~this_ty
          ~lhs:{ sub_supportdyn; ty_sub }
          ~rhs:{ super_like; super_supportdyn = false; ty_super = lty_inner }
          env
      | _ ->
        (* T <: RepresentableAs<U> iff T <: U *)
        simplify
          ~subtype_env
          ~this_ty
          ~lhs:{ sub_supportdyn; ty_sub }
          ~rhs:{ super_like; super_supportdyn = false; ty_super = lty_inner }
          env
    end
    (* #A <: \\HH\\EnumClass\\Label<u, v> *)
    | ((r_sub, Tlabel name), (r_sup, Tnewtype (label_kind, [ty_from; ty_to], _)))
      when String.equal label_kind SN.Classes.cEnumClassLabel ->
      let ty =
        Sd.liken ~super_like env
        @@ mk (r_sup, Tnewtype (SN.Classes.cMemberOf, [ty_from; ty_to], ty_to))
      in
      Has_const.(
        simplify
          ~subtype_env
          ~this_ty:(Some ty_from)
          ~lhs:{ sub_supportdyn; ty_sub = ty_from }
          ~rhs:{ reason_super = r_sub; name; ty }
          env)
    (* When flag enabled, class<T> <: classname<T>, which is bounded by
     * typename<T> *)
    | ((_r_sub, Tclass_ptr ty_sub), (_r_super, Tnewtype (name, [ty_super], _)))
      when subtype_env.Subtype_env.class_sub_classname
           && (String.equal name SN.Classes.cClassname
              || String.equal name SN.Classes.cTypename) ->
      simplify
        ~subtype_env
        ~this_ty:None
        ~lhs:{ sub_supportdyn = None; ty_sub }
        ~rhs:{ super_like = false; super_supportdyn = false; ty_super }
        env
    | ((_r_sub, Tclass_ptr ty_sub), (_r_super, Tnewtype (name, [ty_super], _)))
      when String.equal name SN.Classes.cClassOrClassname ->
      simplify
        ~subtype_env
        ~this_ty:None
        ~lhs:{ sub_supportdyn = None; ty_sub }
        ~rhs:{ super_like = false; super_supportdyn = false; ty_super }
        env
    | ( (_r_sub, Tnewtype (name_sub, [ty_sub], _)),
        (_r_super, Tnewtype (name_super, [ty_super], _)) )
      when String.equal name_super SN.Classes.cClassOrClassname
           && String.equal name_sub SN.Classes.cClassname ->
      simplify
        ~subtype_env
        ~this_ty:None
        ~lhs:{ sub_supportdyn = None; ty_sub }
        ~rhs:{ super_like = false; super_supportdyn = false; ty_super }
        env
    | (_, (r_super, Tnewtype (name_super, lty_supers, _bound_super))) -> begin
      match deref ty_sub with
      | (_, Tclass ((_, name_sub), _, _))
        when String.equal name_sub name_super && Env.is_enum env name_super ->
        valid env
      | (r_sub, Tnewtype (name_sub, lty_subs, _))
        when String.equal name_sub name_super ->
        if List.is_empty lty_subs then
          valid env
        else if Env.is_enum env name_super && Env.is_enum env name_sub then
          valid env
        else
          let td = Env.get_typedef env name_super in
          begin
            match td with
            | Decl_entry.Found { td_tparams; _ } ->
              let variance_reifiedl =
                List.map td_tparams ~f:(fun t -> (t.tp_variance, t.tp_reified))
              in
              simplify_subtype_variance_for_non_injective
                ~subtype_env
                ~sub_supportdyn
                ~super_like
                name_sub
                None
                variance_reifiedl
                (r_sub, lty_subs)
                (r_super, lty_supers)
                ty_sub
                ty_super
                env
            | Decl_entry.DoesNotExist
            | Decl_entry.NotYetAvailable ->
              (* TODO(hverr): decl_entry propagate *)
              invalid ~fail env
          end
      (* ?t <: A <=> null <: A && t <: A *)
      | (r, Toption ty_sub) ->
        let r = Typing_reason.prj_nullable_sub ~sub:r ~sub_prj:r in
        let ty_null = MakeType.null r in
        (* Errors due to `null` should refer to full option type *)
        let prop_null =
          let prop =
            simplify
              ~subtype_env
              ~this_ty
              ~lhs:{ sub_supportdyn; ty_sub = ty_null }
              ~rhs:{ super_like = false; super_supportdyn = false; ty_super }
          in
          (fun env -> if_unsat (invalid ~fail) @@ prop env)
        in
        let ty_sub =
          Typing_env.update_reason env ty_sub ~f:(fun r_sub_prj ->
              Typing_reason.prj_nullable_sub ~sub:r ~sub_prj:r_sub_prj)
        in
        prop_null env
        &&& simplify
              ~subtype_env
              ~this_ty
              ~lhs:{ sub_supportdyn; ty_sub }
              ~rhs:{ super_like = false; super_supportdyn = false; ty_super }
      | (r, Tprim Aast.Tarraykey) ->
        let r = Typing_reason.prj_arraykey_sub ~sub:r ~sub_prj:r in
        let ty_string = MakeType.string r and ty_int = MakeType.int r in
        let prop env =
          env
          |> simplify
               ~subtype_env
               ~this_ty
               ~lhs:{ sub_supportdyn; ty_sub = ty_string }
               ~rhs:{ super_like = false; super_supportdyn = false; ty_super }
          &&& simplify
                ~subtype_env
                ~this_ty
                ~lhs:{ sub_supportdyn; ty_sub = ty_int }
                ~rhs:{ super_like = false; super_supportdyn = false; ty_super }
        in
        (* Use `if_unsat` so we report arraykey in the error *)
        if_unsat (invalid ~fail) @@ prop env
      | (r, Tprim Aast.Tnum) ->
        let r = Typing_reason.prj_num_sub ~sub:r ~sub_prj:r in
        let ty_float = MakeType.float r and ty_int = MakeType.int r in
        let prop env =
          env
          |> simplify
               ~subtype_env
               ~this_ty
               ~lhs:{ sub_supportdyn; ty_sub = ty_float }
               ~rhs:{ super_like = false; super_supportdyn = false; ty_super }
          &&& simplify
                ~subtype_env
                ~this_ty
                ~lhs:{ sub_supportdyn; ty_sub = ty_int }
                ~rhs:{ super_like = false; super_supportdyn = false; ty_super }
        in
        (* Use `if_unsat` so we report num in the error *)
        if_unsat (invalid ~fail) @@ prop env
      | (_, Tgeneric _) when subtype_env.Subtype_env.require_completeness ->
        default_subtype
          ~subtype_env
          ~this_ty
          ~fail
          ~lhs:{ sub_supportdyn; ty_sub }
          ~rhs:{ super_like; super_supportdyn = false; ty_super }
          env
      | (_, (Tvar _ | Tunion _)) ->
        default_subtype
          ~subtype_env
          ~this_ty
          ~fail
          ~lhs:{ sub_supportdyn; ty_sub }
          ~rhs:{ super_like; super_supportdyn = false; ty_super }
          env
      | ( _,
          ( Tany _ | Tdynamic _ | Tintersection _ | Tdependent _ | Taccess _
          | Tgeneric _ | Tnonnull | Tfun _ | Ttuple _ | Tshape _
          | Tvec_or_dict _ | Tclass _ | Tnewtype _ | Tneg _
          | Tprim
              Aast.(
                Tnull | Tvoid | Tint | Tbool | Tfloat | Tresource | Tnoreturn)
          | Tlabel _ | Tclass_ptr _ ) ) -> begin
        match
          match get_node ty_sub with
          | Tnewtype _ -> Ok subtype_env
          | _ ->
            Subtype_env.check_infinite_recursion
              subtype_env
              { Subtype_recursion_tracker.Subtype_op.ty_sub; ty_super }
        with
        | Error _ ->
          (* We've essentially proven by recursion that this subypting relationship holds.
             For details, see Figure 21-4 p. 395 of Pierce's Types and Programming Languages.
             The subtyping relationship would only fail to hold if the recursive types are not
             contractive.
             We're checking for contractiveness in check_invalid_recursive_case_type *)
          valid env
        | Ok subtype_env ->
          let localize td_tparams hint env =
            let ((env, _err), ty) =
              let ety_env =
                (* The this_ty cannot does not need to be set because newtypes
                   * & case types cannot appear within classes thus cannot us
                   * the this type. If we ever change that this could needs to
                   * be changed *)
                {
                  empty_expand_env with
                  type_expansions =
                    (* Subtyping can be called when localizing
                       a union type, since we attempt to simplify it.
                       Since case types are encoded as union types,
                       a cyclic reference to the same case type will
                       lead to infinite looping. The chain is:
                         localize -> simplify_union -> sub_type -> localize

                       The expand environment is not threaded through the
                       whole way, so we won't be able to tell we entered a cycle.

                       For this reason we want to report cycles on the
                       case type we are currently expanding. If a cycle
                       occurs we say the proposition is invalid, but
                       don't report an error, since that will be done
                       during well-formedness checks on type defs *)
                    Type_expansions.empty_w_cycle_report
                      ~report_cycle:
                        (Some
                           ( Pos.none,
                             Type_expansions.Expandable.Type_alias name_super ));
                  substs =
                    (if List.is_empty lty_supers then
                      S_map.empty
                    else
                      Decl_subst.make_locl td_tparams lty_supers);
                }
              in
              Phase.localize ~ety_env env hint
            in
            (env, ty)
          in
          let simplify_ ty_sub ty_super env =
            simplify
              ~subtype_env
              ~this_ty:None
              ~lhs:{ sub_supportdyn = None; ty_sub }
              ~rhs:{ super_like; super_supportdyn = false; ty_super }
              env
          in
          (* x <: case_type *)
          (match Env.get_typedef env name_super with
          | Decl_entry.Found
              {
                td_type_assignment = CaseType (variant, variants);
                td_tparams;
                _;
              } ->
            let check_where_constraint (left, ck, right) env :
                Typing_env_types.env * TL.subtype_prop =
              let (env, local_left) = localize td_tparams left env in
              let (env, local_right) = localize td_tparams right env in
              match ck with
              | Ast_defs.Constraint_as -> simplify_ local_left local_right env
              | Ast_defs.Constraint_super ->
                simplify_ local_right local_left env
              | Ast_defs.Constraint_eq ->
                simplify_ local_left local_right env
                &&& simplify_ local_right local_left
            in
            (* For
             * CT<T1, ... Tn> =
             * | U1 where [constraints_1] | ... | Um where [constraints_m]
             * Then T < CT<t1, ... tn> iff
             * for any k in [1..m]:
             * T < Vk[t1/T1, ...] &&& constraints_k[t1/T1, ...]
             *)
            let ( ||| ) = ( ||| ) ~fail in
            let try_variant (hint, wcs) env =
              let (env, hint_ty) = localize td_tparams hint env in
              let valid ty =
                not
                @@ Typing_case_type_utils.decl_ty_mentions_name_via_typedef
                     env
                     name_super
                     ty
              in
              let wcs =
                List.filter wcs ~f:(fun (left, _ck, right) ->
                    valid left && valid right)
              in
              List.fold_left
                wcs
                ~init:(simplify_ ty_sub hint_ty env)
                ~f:(fun acc wc -> acc &&& check_where_constraint wc)
            in
            List.fold_left
              (variant :: variants)
              ~init:
                (default_subtype
                   ~subtype_env
                   ~this_ty
                   ~fail
                   ~lhs:{ sub_supportdyn; ty_sub }
                   ~rhs:{ super_like; super_supportdyn = false; ty_super }
                   env)
              ~f:(fun acc variant -> acc ||| try_variant variant)
          | Decl_entry.Found
              {
                td_super_constraint = Some lower;
                td_tparams;
                td_type_assignment = SimpleTypeDef _;
                _;
              } ->
            let ( ||| ) = ( ||| ) ~fail in
            default_subtype
              ~subtype_env
              ~this_ty
              ~fail
              ~lhs:{ sub_supportdyn; ty_sub }
              ~rhs:{ super_like; super_supportdyn = false; ty_super }
              env
            ||| fun env ->
            let (env, lower_ty) = localize td_tparams lower env in
            simplify_ ty_sub lower_ty env
          | _ ->
            default_subtype
              ~subtype_env
              ~this_ty
              ~fail
              ~lhs:{ sub_supportdyn; ty_sub }
              ~rhs:{ super_like; super_supportdyn = false; ty_super }
              env)
      end
    end
    (* -- C-Neg-R ----------------------------------------------------------- *)
    | (_, (r_super, Tneg (r_pred, IsTag (ClassTag (c_super, c_super_args))))) ->
    begin
      match deref ty_sub with
      | (_, Tneg (_, IsTag (ClassTag (c_sub, c_sub_args))))
        when List.is_empty c_sub_args ->
        if TUtils.is_sub_class_refl env c_super c_sub then
          valid env
        else
          invalid ~fail env
      | (_, Tclass ((_, c_sub), ex, _)) ->
        if Subtype_negation.is_class_disjoint env c_sub ex c_super nonexact then
          valid env
        else
          invalid ~fail env
      (* Functions can be instances of the Closure class *)
      | (_, Tfun _) when String.equal SN.Classes.cClosure c_super ->
        invalid ~fail env
      | (_, Tprim Tarraykey) when String.equal SN.Classes.cString c_super ->
        invalid ~fail env
      (* All of these are definitely disjoint from class types *)
      | (_, (Tfun _ | Ttuple _ | Tshape _ | Tprim _)) -> valid env
      | _ ->
        default_subtype
          ~subtype_env
          ~this_ty
          ~fail
          ~lhs:{ sub_supportdyn; ty_sub }
          ~rhs:
            {
              super_like;
              super_supportdyn = false;
              ty_super =
                mk
                  ( r_super,
                    Tneg (r_pred, IsTag (ClassTag (c_super, c_super_args))) );
            }
          env
    end
    | (_, (reason_super, Tneg predicate)) -> begin
      match deref ty_sub with
      | (_, Tvar _) ->
        mk_issubtype_prop
          ~sub_supportdyn
          ~is_dynamic_aware:subtype_env.Subtype_env.is_dynamic_aware
          env
          (LoclType ty_sub)
          (LoclType ty_super)
      | (r_sub, Tunion ty_subs) ->
        Common.simplify_union_l
          ~subtype_env
          ~this_ty
          ~mk_prop:simplify
          ~update_reason:
            (Typing_env.update_reason ~f:(fun r_sub_prj ->
                 Typing_reason.prj_union_sub ~sub:r_sub ~sub_prj:r_sub_prj))
          (sub_supportdyn, ty_subs)
          rhs
          env
      | _ ->
        (* T < !P iff left < nothing and span & P < nothing
           However, just do span < nothing since we know that
           if span is not nothing, it isn't known to be disjoint from P.
           This also avoids the need to do special handling of dynamic
           in the union case above to avoid dynamic & P *)
        let (env, partition) =
          Typing_refinement.partition_ty env ty_sub predicate
        in
        let simplify_subtype tyl ty_super env =
          Subtype.(
            simplify
              ~subtype_env
              ~this_ty
              ~lhs:
                {
                  sub_supportdyn;
                  ty_sub = MakeType.intersection reason_super tyl;
                }
              ~rhs:{ super_supportdyn = false; super_like; ty_super }
              env)
        in
        let nothing = MakeType.nothing reason_super in
        List.fold_left
          (partition.Typing_refinement.left @ partition.Typing_refinement.span)
          ~init:(env, TL.valid)
          ~f:(fun res tyl -> res &&& simplify_subtype tyl nothing)
    end
    (* -- C-Class-R --------------------------------------------------------- *)
    (* class refinement
       x <: A with type T = y <=> x <: A && x <: has_type_member(T, y) *)
    | (_, (r_super, Tclass (class_id_super, Nonexact cr_super, tyargs_super)))
      when (not (Class_refinement.is_empty cr_super))
           && (subtype_env.Subtype_env.require_soundness
              || (* To deal with refinements, the code below generates a
                  * constraint type. That is currently not supported when
                  * require_soundness is not set (see below in the function
                  * decompose_subtype_add_prop). Consequently, if soundness
                  * is not required, we treat the refinement information
                  * only if we know for sure that we can discharge it on
                  * the spot; e.g., when ety_sub is a class-ish. This
                  * limits the information lost by skipping refinements. *)
              TUtils.is_class ty_sub) -> begin
      (* We discharge class refinements before anything
              * else ... *)
      Class_refinement.fold_refined_consts
        cr_super
        ~init:(valid env)
        ~f:(fun type_id { rc_bound; _ } (env, prop) ->
          (env, prop)
          &&&
          let (htm_lower, htm_upper) =
            match rc_bound with
            | TRexact ty -> (ty, ty)
            | TRloose { tr_lower; tr_upper } ->
              let loty = MakeType.union r_super tr_lower in
              let upty = MakeType.intersection r_super tr_upper in
              (loty, upty)
          in
          let htm = { htm_id = type_id; htm_lower; htm_upper } in
          let lhs = { sub_supportdyn; ty_sub }
          and rhs =
            Has_type_member.{ reason_super = r_super; has_type_member = htm }
          in
          let subtype_env =
            Subtype_env.possibly_add_violated_constraint
              subtype_env
              ~r_sub:(get_reason ty_sub)
              ~r_super
          in
          Has_type_member.simplify ~subtype_env ~this_ty ~lhs ~rhs)
      &&&
      (* then recursively check the class with all the
         refinements dropped. *)
      let ty_super =
        mk (r_super, Tclass (class_id_super, nonexact, tyargs_super))
      in

      simplify
        ~subtype_env
        ~this_ty
        ~lhs:{ sub_supportdyn; ty_sub }
        ~rhs:{ super_like; super_supportdyn = false; ty_super }
    end
    | ( _,
        ( _r_super,
          Tclass ((_pos_super, class_nm_super), exact_super, tyargs_super) ) )
      -> begin
      match deref ty_sub with
      (* Enums... *)
      | (_, Tnewtype (enum_name, _, _))
        when String.equal enum_name class_nm_super
             && is_nonexact exact_super
             && Env.is_enum env enum_name ->
        valid env
      (* E <: \HH\BuiltinEnum<t> *)
      | (_, Tnewtype (cid, _, _))
        when String.equal class_nm_super SN.Classes.cHH_BuiltinEnum
             && Env.is_enum env cid ->
        (match tyargs_super with
        | [lty_super'] ->
          env
          |> simplify
               ~subtype_env
               ~this_ty
               ~lhs:{ sub_supportdyn; ty_sub }
               ~rhs:
                 {
                   super_like = false;
                   super_supportdyn = false;
                   ty_super = lty_super';
                 }
        | _ ->
          default_subtype
            ~subtype_env
            ~this_ty
            ~fail
            ~lhs:{ sub_supportdyn; ty_sub }
            ~rhs:{ super_like; super_supportdyn = false; ty_super }
            env)
      | (_, Tnewtype (enum_name, _, _))
        when String.equal enum_name class_nm_super && Env.is_enum env enum_name
        ->
        valid env
      | (_, Tnewtype (enum_name, _, _))
        when Env.is_enum env enum_name
             && String.equal class_nm_super SN.Classes.cXHPChild ->
        valid env
      (* t_prim <: XHPChild *)
      | (_, Tprim Nast.(Tarraykey | Tint | Tfloat | Tnum))
        when String.equal class_nm_super SN.Classes.cXHPChild
             && is_nonexact exact_super ->
        valid env
      | (_, Tclass ((_, id), _, _))
        when String.equal id SN.Classes.cString
             && String.equal class_nm_super SN.Classes.cXHPChild
             && is_nonexact exact_super ->
        valid env
      (* string <: Stringish *)
      | (_, Tclass ((_, id), _, _))
        when String.equal class_nm_super SN.Classes.cStringish
             && is_nonexact exact_super
             && String.equal id SN.Classes.cString ->
        valid env
      (* same as prior two cases by typename's string bound *)
      | (_, Tclass_ptr _)
        when subtype_env.Subtype_env.class_sub_classname
             && is_nonexact exact_super
             && (String.equal class_nm_super SN.Classes.cXHPChild
                || String.equal class_nm_super SN.Classes.cStringish) ->
        valid env
      (* Match what's done in unify for non-strict code *)
      | (r_sub, Tclass (class_id_sub, exact_sub, tyl_sub)) ->
        (match deref ty_super with
        | (r_super, Tclass (class_id_super, exact_super, tyl_super)) ->
          simplify_subtype_classes
            ~fail
            ~subtype_env
            ~sub_supportdyn
            ~this_ty
            ~super_like
            (r_sub, (class_id_sub, exact_sub, tyl_sub))
            (r_super, (class_id_super, exact_super, tyl_super))
            env
        | _ -> invalid env ~fail)
      | (_r_sub, Tvec_or_dict (_, tv)) ->
        (match (exact_super, tyargs_super) with
        | (Nonexact _, [tv_super])
          when String.equal class_nm_super SN.Collections.cTraversable
               || String.equal class_nm_super SN.Collections.cContainer ->
          (* vec<tv> <: Traversable<tv_super>
           * iff tv <: tv_super
           * Likewise for vec<tv> <: Container<tv_super>
           *          and map<_,tv> <: Traversable<tv_super>
           *          and map<_,tv> <: Container<tv_super>
           *)
          simplify
            ~subtype_env
            ~this_ty
            ~lhs:{ sub_supportdyn; ty_sub = tv }
            ~rhs:
              {
                super_like = false;
                super_supportdyn = false;
                ty_super = tv_super;
              }
            env
        | (Nonexact _, [tk_super; tv_super])
          when String.equal class_nm_super SN.Collections.cKeyedTraversable
               || String.equal class_nm_super SN.Collections.cKeyedContainer
               || String.equal class_nm_super SN.Collections.cAnyArray ->
          (match get_node ty_sub with
          | Tvec_or_dict (tk, _) ->
            env
            |> simplify
                 ~subtype_env
                 ~this_ty
                 ~lhs:{ sub_supportdyn; ty_sub = tk }
                 ~rhs:
                   {
                     super_like = false;
                     super_supportdyn = false;
                     ty_super = tk_super;
                   }
            &&& simplify
                  ~subtype_env
                  ~this_ty
                  ~lhs:{ sub_supportdyn; ty_sub = tv }
                  ~rhs:
                    {
                      super_like = false;
                      super_supportdyn = false;
                      ty_super = tv_super;
                    }
          | _ ->
            default_subtype
              ~subtype_env
              ~this_ty
              ~fail
              ~lhs:{ sub_supportdyn; ty_sub }
              ~rhs:{ super_like; super_supportdyn = false; ty_super }
              env)
        | (Nonexact _, [])
          when String.equal class_nm_super SN.Collections.cKeyedTraversable
               || String.equal class_nm_super SN.Collections.cKeyedContainer
               || String.equal class_nm_super SN.Collections.cAnyArray ->
          (* All arrays are subtypes of the untyped KeyedContainer / Traversables *)
          valid env
        | (_, _) ->
          default_subtype
            ~subtype_env
            ~this_ty
            ~fail
            ~lhs:{ sub_supportdyn; ty_sub }
            ~rhs:{ super_like; super_supportdyn = false; ty_super }
            env)
      | _ ->
        default_subtype
          ~subtype_env
          ~this_ty
          ~fail
          ~lhs:{ sub_supportdyn; ty_sub }
          ~rhs:{ super_like; super_supportdyn = false; ty_super }
          env
      (* -- C-Label-R ---------------------------------------------------------- *)
    end
    | ((_, Tlabel sub_name), (_, Tlabel sup_name)) ->
      if String.equal sub_name sup_name then
        valid env
      else
        invalid ~fail env
    | ( ( _,
          ( Tany _ | Tdynamic _ | Tunion _ | Toption _ | Tintersection _
          | Tdependent _ | Taccess _ | Tgeneric _ | Tnonnull | Tfun _ | Ttuple _
          | Tshape _ | Tvec_or_dict _ | Tclass _ | Tnewtype _ | Tneg _ | Tprim _
          | Tvar _ | Tclass_ptr _ ) ),
        (_, Tlabel _) ) ->
      default_subtype
        ~subtype_env
        ~this_ty
        ~fail
        ~lhs:{ sub_supportdyn; ty_sub }
        ~rhs:{ super_like; super_supportdyn = false; ty_super }
        env
    (* -- C-ClassPtr-R ----------------------------------------------------- *)
    | ((_r_sub, Tclass_ptr ty_sub), (_r_super, Tclass_ptr ty_super)) ->
      simplify
        ~subtype_env
        ~this_ty:None
        ~lhs:{ sub_supportdyn = None; ty_sub }
        ~rhs:{ super_like = false; super_supportdyn = false; ty_super }
        env
    | ( ( _,
          ( Tany _ | Tunion _ | Toption _ | Tintersection _ | Tgeneric _
          | Taccess _ | Tnewtype _ | Tprim _ | Tnonnull | Tclass _
          | Tvec_or_dict _ | Ttuple _ | Tshape _ | Tdynamic _ | Tneg _
          | Tdependent _ | Tvar _ | Tlabel _ | Tfun _ ) ),
        (_, Tclass_ptr _) ) ->
      default_subtype
        ~subtype_env
        ~this_ty
        ~fail
        ~lhs:{ sub_supportdyn; ty_sub }
        ~rhs:{ super_supportdyn; super_like; ty_super }
        env
end

(* -- Non-subtype constraints ----------------------------------------------- *)
and Destructure : sig
  type rhs = {
    reason_super: Reason.t;
    destructure: destructure;
  }

  include Constraint_handler with type rhs := rhs
end = struct
  type rhs = {
    reason_super: Reason.t;
    destructure: destructure;
  }

  let destructure_array
      ~subtype_env
      ~this_ty
      (sub_supportdyn, ty_sub_inner)
      {
        reason_super = r_super;
        destructure = { d_kind; d_required; d_optional; d_variadic };
      }
      env =
    (* If this is a splat, there must be a variadic box to receive the elements
     * but for list(...) destructuring this is not required. Example:
     *
     * function f(int $i): void {}
     * function g(vec<int> $v): void {
     *   list($a) = $v; // ok (but may throw)
     *   f(...$v); // error
     * } *)
    let fpos =
      match Reason.Predicates.unpack_unpack_param_opt r_super with
      | Some (_, fpos, _) -> fpos
      | None -> Reason.to_pos r_super
    in
    match (d_kind, d_required, d_variadic) with
    | (SplatUnpack, _ :: _, _) ->
      (* return the env so as not to discard the type variable that might
         have been created for the Traversable type created below. *)
      invalid
        env
        ~fail:
          (Option.map subtype_env.Subtype_env.on_error ~f:(fun on_error ->
               Typing_error.(
                 apply_reasons ~on_error
                 @@ Secondary.Unpack_array_required_argument
                      { pos = Reason.to_pos r_super; decl_pos = fpos })))
    | (SplatUnpack, [], None) ->
      invalid
        env
        ~fail:
          (Option.map subtype_env.Subtype_env.on_error ~f:(fun on_error ->
               Typing_error.(
                 apply_reasons ~on_error
                 @@ Secondary.Unpack_array_variadic_argument
                      { pos = Reason.to_pos r_super; decl_pos = fpos })))
    | (SplatUnpack, [], Some _)
    | (ListDestructure, _, _) ->
      List.fold d_required ~init:(env, TL.valid) ~f:(fun res ty_dest ->
          res
          &&& Subtype.(
                simplify
                  ~subtype_env
                  ~this_ty
                  ~lhs:{ sub_supportdyn; ty_sub = ty_sub_inner }
                  ~rhs:
                    {
                      super_like = false;
                      super_supportdyn = false;
                      ty_super = ty_dest;
                    }))
      &&& fun env ->
      List.fold d_optional ~init:(env, TL.valid) ~f:(fun res ty_dest ->
          res
          &&& Subtype.(
                simplify
                  ~subtype_env
                  ~this_ty
                  ~lhs:{ sub_supportdyn; ty_sub = ty_sub_inner }
                  ~rhs:
                    {
                      super_like = false;
                      super_supportdyn = false;
                      ty_super = ty_dest;
                    }))
      &&& fun env ->
      Option.value_map ~default:(env, TL.valid) d_variadic ~f:(fun vty ->
          Subtype.(
            simplify
              ~subtype_env
              ~this_ty
              ~lhs:{ sub_supportdyn; ty_sub = ty_sub_inner }
              ~rhs:
                { super_like = false; super_supportdyn = false; ty_super = vty }
              env))

  let destructure_dynamic
      ~subtype_env
      ~this_ty
      (sub_supportdyn, ty_sub)
      { destructure = { d_required; d_optional; d_variadic; _ }; _ }
      env =
    List.fold d_required ~init:(env, TL.valid) ~f:(fun res ty_dest ->
        res
        &&& Subtype.(
              simplify
                ~subtype_env
                ~this_ty
                ~lhs:{ sub_supportdyn; ty_sub }
                ~rhs:
                  {
                    super_like = false;
                    super_supportdyn = false;
                    ty_super = ty_dest;
                  }))
    &&& fun env ->
    List.fold d_optional ~init:(env, TL.valid) ~f:(fun res ty_dest ->
        res
        &&& Subtype.(
              simplify
                ~subtype_env
                ~this_ty
                ~lhs:{ sub_supportdyn; ty_sub }
                ~rhs:
                  {
                    super_like = false;
                    super_supportdyn = false;
                    ty_super = ty_dest;
                  }))
    &&& fun env ->
    Option.value_map ~default:(env, TL.valid) d_variadic ~f:(fun vty ->
        Subtype.(
          simplify
            ~subtype_env
            ~this_ty
            ~lhs:{ sub_supportdyn; ty_sub }
            ~rhs:
              { super_like = false; super_supportdyn = false; ty_super = vty }
            env))

  let destructure_tuple
      ~subtype_env
      ~this_ty
      (sub_supportdyn, reason_tuple, ty_subs)
      {
        reason_super = r_super;
        destructure = { d_required; d_optional; d_variadic; _ };
      }
      env =
    (* First fill the required elements. If there are insufficient elements, an error is reported.
     * Fill as many of the optional elements as possible, and the remainder are unioned into the
     * variadic element. Example:
     *
     * (float, bool, string, int) <: Tdestructure(#1, opt#2, ...#3) =>
     * float <: #1 /\ bool <: #2 /\ string <: #3 /\ int <: #3
     *
     * (float, bool) <: Tdestructure(#1, #2, opt#3) =>
     * float <: #1 /\ bool <: #2
     *)
    let len_ts = List.length ty_subs in
    let len_required = List.length d_required in
    let arity_error f =
      let (epos, fpos, prefix) =
        match Reason.Predicates.unpack_unpack_param_opt r_super with
        | Some (epos, fpos, c) -> (Pos_or_decl.of_raw_pos epos, fpos, c)
        | None -> (Reason.to_pos r_super, Reason.to_pos reason_tuple, 0)
      in
      invalid
        env
        ~fail:
          (f
             (prefix + len_required)
             (prefix + len_ts)
             epos
             fpos
             subtype_env.Subtype_env.on_error)
    in
    if len_ts < len_required then
      arity_error (fun expected actual pos decl_pos on_error_opt ->
          Option.map on_error_opt ~f:(fun on_error ->
              let base_err =
                Typing_error.Secondary.Typing_too_few_args
                  { pos; decl_pos; expected; actual }
              in
              Typing_error.(apply_reasons ~on_error base_err)))
    else
      let len_optional = List.length d_optional in
      let (ts_required, remain) = List.split_n ty_subs len_required in
      let (ts_optional, ts_variadic) = List.split_n remain len_optional in
      let (res, n) =
        List.fold2_exn
          ts_required
          d_required
          ~init:((env, TL.valid), 0)
          ~f:(fun (res, n) ty_from ty_into ->
            let ty_from =
              Typing_env.update_reason env ty_from ~f:(fun sub_prj ->
                  Typing_reason.(
                    prj_tuple ~sub:reason_tuple ~sub_prj ~super:r_super n))
            in
            ( (res
              &&& Subtype.(
                    simplify
                      ~subtype_env
                      ~this_ty
                      ~lhs:{ sub_supportdyn; ty_sub = ty_from }
                      ~rhs:
                        {
                          super_like = false;
                          super_supportdyn = false;
                          ty_super = ty_into;
                        })),
              n + 1 ))
      in

      res &&& fun env ->
      let len_ts_opt = List.length ts_optional in
      let d_optional_part =
        if len_ts_opt < len_optional then
          List.take d_optional len_ts_opt
        else
          d_optional
      in
      let (res, n) =
        List.fold2_exn
          ts_optional
          d_optional_part
          ~init:((env, TL.valid), n)
          ~f:(fun (res, n) ty_from ty_into ->
            let ty_from =
              Typing_env.update_reason env ty_from ~f:(fun sub_prj ->
                  Typing_reason.(
                    prj_tuple ~sub:reason_tuple ~sub_prj ~super:r_super n))
            in
            ( (res
              &&& Subtype.(
                    simplify
                      ~subtype_env
                      ~this_ty
                      ~lhs:{ sub_supportdyn; ty_sub = ty_from }
                      ~rhs:
                        {
                          super_like = false;
                          super_supportdyn = false;
                          ty_super = ty_into;
                        })),
              n + 1 ))
      in

      res &&& fun env ->
      match (ts_variadic, d_variadic) with
      | (vars, Some ty_into) ->
        List.fold vars ~init:(env, TL.valid) ~f:(fun res ty_from ->
            res
            &&& Subtype.(
                  let ty_from =
                    Typing_env.update_reason env ty_from ~f:(fun sub_prj ->
                        Typing_reason.(
                          prj_tuple
                            ~sub:reason_tuple
                            ~sub_prj
                            ~super:r_super
                            (* TODO(mjt) allow different indices here *)
                            (n + 1)))
                  in
                  simplify
                    ~subtype_env
                    ~this_ty
                    ~lhs:{ sub_supportdyn; ty_sub = ty_from }
                    ~rhs:
                      {
                        super_like = false;
                        super_supportdyn = false;
                        ty_super = ty_into;
                      }))
      | ([], None) -> valid env
      | (_, None) ->
        (* Elements remain but we have nowhere to put them *)
        arity_error (fun expected actual pos decl_pos on_error_opt ->
            Option.map on_error_opt ~f:(fun on_error ->
                Typing_error.(
                  apply_reasons ~on_error
                  @@ Secondary.Typing_too_many_args
                       { pos; decl_pos; expected; actual })))

  let rec simplify
      ~subtype_env
      ~this_ty
      ~lhs:{ sub_supportdyn; ty_sub }
      ~rhs:({ reason_super = r_super; destructure } as rhs)
      env =
    begin
      let (env, ty_sub) = Env.expand_type env ty_sub in
      let subtype_env =
        Subtype_env.possibly_add_violated_constraint
          subtype_env
          ~r_sub:(get_reason ty_sub)
          ~r_super
      in
      let fail =
        Subtype_env.fail
          subtype_env
          ~ty_sub:(LoclType ty_sub)
          ~ty_super:
            (ConstraintType
               (mk_constraint_type (r_super, Tdestructure destructure)))
      in
      match (deref ty_sub, destructure.d_kind) with
      (* TODO optional and variadic components T201398626 T201398652 *)
      | ((r, Ttuple { t_required; _ }), _) ->
        destructure_tuple
          ~subtype_env
          ~this_ty
          (sub_supportdyn, r, t_required)
          rhs
          env
      | ((r, Tclass ((_, x), _, tyl)), _)
        when String.equal x SN.Collections.cPair ->
        destructure_tuple ~subtype_env ~this_ty (sub_supportdyn, r, tyl) rhs env
      | ((_, Tclass ((_, x), _, [elt_type])), _)
        when String.equal x SN.Collections.cVector
             || String.equal x SN.Collections.cImmVector
             || String.equal x SN.Collections.cVec
             || String.equal x SN.Collections.cConstVector ->
        destructure_array
          ~subtype_env
          ~this_ty
          (sub_supportdyn, elt_type)
          rhs
          env
      | ((_, Tdynamic _), _) ->
        destructure_dynamic
          ~subtype_env
          ~this_ty
          (sub_supportdyn, ty_sub)
          rhs
          env
      | ((_, Tvar _), _) ->
        mk_issubtype_prop
          ~sub_supportdyn
          ~is_dynamic_aware:subtype_env.Subtype_env.is_dynamic_aware
          env
          (LoclType ty_sub)
          (ConstraintType
             (mk_constraint_type (r_super, Tdestructure destructure)))
      | ((r_sub, Tunion ty_subs), _) ->
        Common.simplify_union_l
          ~subtype_env
          ~this_ty
          ~mk_prop:simplify
          ~update_reason:
            (Typing_env.update_reason ~f:(fun r_sub_prj ->
                 Typing_reason.prj_union_sub ~sub:r_sub ~sub_prj:r_sub_prj))
          (sub_supportdyn, ty_subs)
          rhs
          env
      | ((r_sub, Tintersection ty_subs), _) ->
        (* A & B <: C iif A <: C | !B *)
        (match Subtype_negation.find_type_with_exact_negation env ty_subs with
        | (env, Some non_ty, tyl) ->
          let ty_sub = MakeType.intersection r_sub tyl in
          let mk_prop = simplify
          and lift_rhs { reason_super; destructure } =
            mk_constraint_type (reason_super, Tdestructure destructure)
          and lhs = (sub_supportdyn, ty_sub)
          and rhs_subtype =
            Subtype.
              {
                super_supportdyn = false;
                super_like = false;
                ty_super = non_ty;
              }
          and rhs_destructure = { reason_super = r_super; destructure } in
          let rhs = (r_super, rhs_subtype, rhs_destructure) in
          Common.simplify_disj_r
            ~subtype_env
            ~this_ty
            ~fail
            ~lift_rhs
            ~mk_prop
            lhs
            rhs
            env
        | _ ->
          Common.simplify_intersection_l
            ~subtype_env
            ~this_ty
            ~fail
            ~mk_prop:simplify
            ~update_reason:
              (Typing_env.update_reason ~f:(fun r_sub_prj ->
                   Typing_reason.prj_inter_sub ~sub:r_sub ~sub_prj:r_sub_prj))
            (sub_supportdyn, ty_subs)
            rhs
            env)
      | ((r_generic, Tgeneric generic_nm), _) ->
        Common.simplify_generic_l
          ~subtype_env
          ~this_ty
          ~fail
          ~mk_prop:simplify
          (sub_supportdyn, r_generic, generic_nm)
          rhs
          rhs
          env
      | (_, SplatUnpack) ->
        (* Allow splatting of arbitrary Traversables *)
        let (env, ty_inner) = Env.fresh_type env Pos.none in
        let traversable = MakeType.traversable r_super ty_inner in
        env
        |> Subtype.(
             simplify
               ~subtype_env
               ~this_ty
               ~lhs:{ sub_supportdyn; ty_sub }
               ~rhs:
                 {
                   super_like = false;
                   super_supportdyn = false;
                   ty_super = traversable;
                 })
        &&& destructure_array ~subtype_env ~this_ty (None, ty_inner) rhs
      | ((r_newtype, Tnewtype (nm, tyl, _)), ListDestructure) ->
        let (env, ty_newtype) =
          Typing_utils.get_newtype_super env r_newtype nm tyl
        in
        Common.simplify_newtype_l
          ~subtype_env
          ~this_ty
          ~mk_prop:simplify
          (sub_supportdyn, r_newtype, nm, ty_newtype)
          rhs
          env
      | ((r_dep, Tdependent (dep_ty, ty_inner_sub)), ListDestructure) ->
        Common.simplify_dependent_l
          ~subtype_env
          ~this_ty
          ~mk_prop:simplify
          (sub_supportdyn, r_dep, dep_ty, ty_inner_sub)
          rhs
          env
      | ( ( _,
            ( Tany _ | Tnonnull | Toption _ | Tprim _ | Tfun _ | Tshape _
            | Tvec_or_dict _ | Taccess _ | Tclass _ | Tneg _ | Tlabel _
            | Tclass_ptr _ ) ),
          ListDestructure ) ->
        let ty_sub_descr =
          lazy (Typing_print.full_strip_ns ~hide_internals:true env ty_sub)
        in
        invalid
          env
          ~fail:
            (Option.map
               subtype_env.Subtype_env.on_error
               ~f:
                 Typing_error.(
                   fun on_error ->
                     apply_reasons ~on_error
                     @@ Secondary.Invalid_destructure
                          {
                            pos = Reason.to_pos r_super;
                            decl_pos = get_pos ty_sub;
                            ty_name = ty_sub_descr;
                          }))
    end
end

and Can_index : sig
  type rhs = {
    reason_super: Reason.t;
    can_index: can_index;
  }

  include Constraint_handler with type rhs := rhs
end = struct
  type rhs = {
    reason_super: Reason.t;
    can_index: can_index;
  }

  (* We need to handle intersection type with special care.
   * In particular, indexing an /\C... should return /\V... where V is the result of indexing C that succeed.
   * Lets look at three examples:
   * Indexing KeyedContainer<arraykey, nonnull> & Vector[?int] should return nonnull & ?int - the intersection of both type.
   * Indexing dynamic & nonnull, should return the index type of dynamic only (which is a dynamic), because only dynamic is indexable.
   * Indexing mixed & nonnull should be an error, because both type is not indexable.
   *
   * As you can see, we need to behave differently depending on whether a type is indexable or not.
   * However, as we are in subtyping world, we cannot invoke Subtype_ask to determine indexability anymore.
   * So, we make a guess with the below with_hint type.
   *
   * There are multiple cases:
   * - If there are errors for whatever reason, it is likely unsolvable.
   * - If it is a container/tuple/shape/etc, without any obvious error (arity missmatch, index out of bound), it is likely solvable
   * - If the type is complex (union type/intersection type/type variable/etc), recurse to find out
   * - If it is a unexpandable variable, it is likely unsolvable.
   * -- The above rules could go both way.
   *)
  type with_hint = {
    env: env;
    prop: TL.subtype_prop;
    (* A guess whether this simplification is good or not. we will be using this to guide the solving of intersection *)
    likely: bool;
  }

  let with_hint (env, prop) likely = { env; prop; likely }

  let likely_solvable (env, prop) = with_hint (env, prop) true

  let likely_unsolvable (env, prop) = with_hint (env, prop) false

  let un_with_hint { env; prop; _ } = (env, prop)

  let with_hint_and { env; prop = p1; likely = l1 } f =
    match TL.get_error_if_unsat p1 with
    | Some ty_err_opt1 ->
      let (env, p2) = f env |> un_with_hint in
      begin
        (match TL.get_error_if_unsat p2 with
        | Some ty_err_opt2 ->
          let ty_err_opt =
            Typing_error.multiple_opt
            @@ List.filter_opt [ty_err_opt1; ty_err_opt2]
          in
          (env, TL.Disj (ty_err_opt, []))
        | None -> (env, p1))
        |> likely_unsolvable
      end
    | None ->
      let { env; prop = p2; likely = l2 } = f env in
      with_hint (env, TL.conj p1 p2) (l1 && l2)

  let rec simplify
      ~subtype_env
      ~this_ty
      ~lhs:({ ty_sub; _ } as lhs)
      ~rhs:({ reason_super; can_index } as rhs)
      env =
    let (env, ty_sub) = Env.expand_type env ty_sub in
    let ty_sub = LoclType ty_sub in
    let ty_super =
      ConstraintType (mk_constraint_type (reason_super, Tcan_index can_index))
    in
    if Logging.should_log_subtype_i env ~level:2 ty_sub ty_super then
      Logging.log_subtype_i
        ~this_ty
        ~function_name:"Typing_subtype.Can_index.simplify"
        env
        ty_sub
        ty_super
      @@ fun () -> simplify_ ~subtype_env ~this_ty ~lhs ~rhs env |> un_with_hint
    else
      simplify_ ~subtype_env ~this_ty ~lhs ~rhs env |> un_with_hint

  and simplify_
      ~subtype_env
      ~this_ty
      ~lhs:{ sub_supportdyn; ty_sub }
      ~rhs:({ reason_super; can_index } as rhs)
      env =
    (* This call to expand_type_and_narrow is not needed for correctness reasons.
     * However, without it, the typechecker will be significantly slower,
     * And cannot complete typechecking in expected time.
     * It will be good to investigate the root cause and remove this code.
     *)
    let ((env, ty_err), ty_sub) =
      Typing_solver.expand_type_and_narrow
        env
        ~description_of_expected:"an array or collection"
        ~force_solve:false
        (Typing_array_access_util.widen_for_array_get_ci can_index)
        can_index.ci_array_pos
        ty_sub
    in
    Option.iter ~f:(Typing_error_utils.add_typing_error ~env) ty_err;
    let subtype_env =
      Subtype_env.possibly_add_violated_constraint
        subtype_env
        ~r_sub:(get_reason ty_sub)
        ~r_super:reason_super
    in
    let fail =
      Subtype_env.fail
        subtype_env
        ~ty_sub:(LoclType ty_sub)
        ~ty_super:
          (ConstraintType
             (mk_constraint_type (reason_super, Tcan_index can_index)))
    in
    let invalid ~fail env : with_hint =
      invalid ~fail env |> likely_unsolvable
    in
    let arity_error r name env =
      invalid
        ~fail:
          (Some
             Typing_error.(
               primary
               @@ Primary.Array_get_arity
                    {
                      pos = can_index.ci_expr_pos;
                      name;
                      decl_pos = Reason.to_pos r;
                    }))
        env
    in
    let mk_prop ~subtype_env ~this_ty ~lhs ~rhs =
      simplify ~subtype_env ~this_ty ~lhs ~rhs
    in
    let simplify_default ~subtype_env lhs rhs env =
      Subtype.simplify
        ~subtype_env
        ~this_ty
        ~lhs:{ sub_supportdyn; ty_sub = lhs }
        ~rhs:{ super_like = false; super_supportdyn = false; ty_super = rhs }
        env
    in
    let simplify_key tk env =
      let tk =
        Typing_make_type.locl_like (Reason.dynamic_coercion (get_reason tk)) tk
      in
      simplify_default ~subtype_env can_index.ci_key tk env
    in
    let simplify_val tv env =
      let tv =
        match sub_supportdyn with
        | None -> tv
        | Some r -> Typing_make_type.supportdyn r tv
      in
      simplify_default ~subtype_env tv can_index.ci_val env
    in
    let return_val tv env = simplify_val tv env |> likely_solvable in
    let is_container tk tv env =
      likely_solvable (simplify_key tk env &&& simplify_val tv)
    in
    let is_vec_like tv env =
      let tk = MakeType.int (Reason.idx_vector can_index.ci_array_pos) in
      is_container tk tv env
    in
    let is_dict_like tv env =
      let tk = MakeType.arraykey (Reason.idx_dict can_index.ci_array_pos) in
      is_container tk tv env
    in
    let pessimise_type env ty =
      Typing_union.union env ty (MakeType.dynamic (get_reason ty))
    in
    let maybe_pessimise_type env ty =
      if Typechecker_options.pessimise_builtins (Env.get_tcopt env) then
        pessimise_type env ty
      else
        (env, ty)
    in
    let tuple_oob env =
      invalid
        ~fail:
          (Some
             Typing_error.(
               primary
               @@ Primary.Generic_unify
                    {
                      pos = can_index.ci_index_pos;
                      msg = Reason.string_of_ureason Reason.URtuple_OOB;
                    }))
        env
    in
    let do_tuple required rest_k =
      match can_index.ci_index_expr with
      | (a, b, Int i) ->
        (match int_of_string_opt i with
        | None -> tuple_oob env
        | Some i ->
          (match List.nth required i with
          | Some ty -> return_val ty env
          | None ->
            let i = i - List.length required in
            if i >= 0 then
              rest_k i (fun c -> (a, b, c))
            else
              tuple_oob env))
      | _ ->
        invalid
          ~fail:
            (Some
               Typing_error.(
                 primary
                 @@ Primary.Generic_unify
                      {
                        pos = can_index.ci_index_pos;
                        msg = Reason.string_of_ureason Reason.URtuple_access;
                      }))
          env
    in
    let do_tuple_general required optional extra =
      do_tuple required (fun i ie_ctx ->
          match List.nth optional i with
          | Some ty ->
            if
              match can_index.ci_access_kind with
              | Ci_lhs_of_null_coalesce
              | Ci_destructure_optional ->
                true
              | Ci_normal -> false
            then
              return_val ty env
            else
              tuple_oob env
          | None ->
            let i = i - List.length optional in
            (match extra with
            | Tvariadic ty ->
              if
                i >= 0
                && (not (Typing_utils.is_nothing env ty))
                &&
                match can_index.ci_access_kind with
                | Ci_lhs_of_null_coalesce
                | Ci_destructure_optional ->
                  true
                | Ci_normal -> false
              then
                return_val ty env
              else
                tuple_oob env
            | Tsplat ty ->
              simplify_
                ~subtype_env
                ~this_ty
                ~lhs:{ sub_supportdyn; ty_sub = ty }
                ~rhs:
                  {
                    reason_super;
                    can_index =
                      {
                        can_index with
                        ci_index_expr = ie_ctx (Aast.Int (Int.to_string i));
                      };
                  }
                env))
    in
    let do_tuple_basic tup =
      do_tuple_general tup [] (Tvariadic (MakeType.nothing Reason.none))
    in
    let do_shape r_sub { s_fields = fdm; s_origin; s_unknown_value } =
      let (_, p, _) = can_index.ci_index_expr in
      match
        Typing_shapes.tshape_field_name_with_ty_err env can_index.ci_index_expr
      with
      | Error ty_err -> invalid ~fail:(Some ty_err) env
      | Ok field ->
        if
          match can_index.ci_access_kind with
          | Ci_lhs_of_null_coalesce -> true
          | Ci_normal
          | Ci_destructure_optional ->
            false
        then
          (* The expression $s['x'] ?? $y is semantically equivalent to
             Shapes::idx ($s, 'x') ?? $y.  I.e., if $s['x'] occurs on
             the left of a coalesce operator, then for type checking it
             can be treated as if it evaluated to null instead of
             throwing an exception if the field 'x' doesn't exist in $s.
          *)
          let (env, ty) =
            Typing_shapes.idx_without_default
              env
              ty_sub
              field
              ~expr_pos:can_index.ci_expr_pos
              ~shape_pos:can_index.ci_array_pos
          in
          return_val ty env
        else (
          match TShapeMap.find_opt field fdm with
          | None ->
            let decl_pos =
              match s_origin with
              | From_alias (_, Some pos) -> pos
              | _ -> Reason.to_pos r_sub
            in
            (* Don't generate an error under dynamic assumptions because we can implicit upcast to dynamic *)
            if Tast.is_under_dynamic_assumptions env.Typing_env_types.checked
            then
              return_val s_unknown_value env
            else
              invalid
                ~fail:
                  (Some
                     Typing_error.(
                       primary
                       @@ Primary.Undefined_field
                            {
                              pos = p;
                              name = TUtils.get_printable_shape_field_name field;
                              decl_pos;
                            }))
                env
          | Some { sft_optional; sft_ty } ->
            if
              sft_optional
              &&
              match can_index.ci_access_kind with
              | Ci_normal -> true
              | Ci_lhs_of_null_coalesce
              | Ci_destructure_optional ->
                false
            then
              let declared_field =
                List.find_exn
                  ~f:(fun x -> TShapeField.equal field x)
                  (TShapeMap.keys fdm)
              in
              invalid
                ~fail:
                  (Some
                     Typing_error.(
                       primary
                       @@ Primary.Array_get_with_optional_field
                            {
                              recv_pos = can_index.ci_array_pos;
                              field_pos = p;
                              field_name =
                                TUtils.get_printable_shape_field_name field;
                              decl_pos =
                                Typing_defs.TShapeField.pos declared_field;
                            }))
                env
            else
              return_val sft_ty env
        )
    in
    let got_dynamic env =
      simplify_default
        ~subtype_env
        (MakeType.dynamic reason_super)
        can_index.ci_val
        env
      &&& simplify_default
            ~subtype_env:{ subtype_env with is_dynamic_aware = true }
            can_index.ci_key
            (MakeType.dynamic (get_reason ty_sub))
      |> likely_solvable
    in
    let is_nullable =
      (match can_index.ci_access_kind with
      | Ci_lhs_of_null_coalesce -> true
      | Ci_normal
      | Ci_destructure_optional ->
        false)
      || Tast.is_under_dynamic_assumptions env.Typing_env_types.checked
    in
    match deref ty_sub with
    | (_, Tvar _) ->
      mk_issubtype_prop
        ~sub_supportdyn
        ~is_dynamic_aware:subtype_env.Subtype_env.is_dynamic_aware
        env
        (LoclType ty_sub)
        (ConstraintType
           (mk_constraint_type (reason_super, Tcan_index can_index)))
      |> likely_unsolvable
    | (_r_sub, Tdynamic _) -> got_dynamic env
    | (_r_sub, Tclass ((_, cn), _, _))
      when String.equal cn SN.Collections.cAnyArray
           && Tast.is_under_dynamic_assumptions env.Typing_env_types.checked ->
      got_dynamic env
    | (_r_sub, _)
      when Option.is_some sub_supportdyn
           && Tast.is_under_dynamic_assumptions env.checked ->
      got_dynamic env
    | (r_sub, Tclass ((_, n), _, targs))
      when String.equal n SN.Collections.cConstMap
           || String.equal n SN.Collections.cImmMap
           || String.equal n SN.Collections.cKeyedContainer
           || String.equal n SN.Collections.cAnyArray ->
      (match targs with
      | [_tk; tv] ->
        let (env, tv) = maybe_pessimise_type env tv in
        is_dict_like tv env
      | _ -> arity_error r_sub n env)
    | (r_sub, Tclass ((_, n), _, targs))
      when String.equal n SN.Collections.cMap
           || String.equal n SN.Collections.cMutableMap ->
      (match targs with
      | [tk; tv] ->
        let (env, tv) = maybe_pessimise_type env tv in
        is_container tk tv env
      | _ -> arity_error r_sub n env)
    | (r_sub, Tclass ((_, n), _, targs)) when String.equal n SN.Collections.cVec
      ->
      (match targs with
      | [tv] -> is_vec_like tv env
      | _ -> arity_error r_sub n env)
    | (r_sub, Tclass ((_, n), _, targs))
      when String.equal n SN.Collections.cImmVector
           || String.equal n SN.Collections.cConstVector
           || String.equal n SN.Collections.cVector
           || String.equal n SN.Collections.cMutableVector ->
      (match targs with
      | [tv] ->
        let (env, tv) = maybe_pessimise_type env tv in
        is_vec_like tv env
      | _ -> arity_error r_sub n env)
    (* dict and keyset are covariant in the key type, so subsumption
     * lets you upcast the key type beyond ty2 to arraykey.
     *)
    | (r_sub, Tclass ((_, n), _, targs))
      when String.equal n SN.Collections.cDict ->
      (match targs with
      | [_; tv] -> is_dict_like tv env
      | _ -> arity_error r_sub n env)
    | (r_sub, Tclass ((_, n), _, targs))
      when String.equal n SN.Collections.cKeyset ->
      (match targs with
      | [tkv] -> is_dict_like tkv env
      | _ -> arity_error r_sub n env)
    | (_r_sub, Tvec_or_dict (_, tv)) ->
      let (env, tv) = maybe_pessimise_type env tv in
      is_dict_like tv env
    | (_r_sub, Tclass ((_, n), _, _)) when String.equal n SN.Classes.cString ->
      let pos = get_pos ty_sub in
      let tk = MakeType.int (Reason.idx_string_from_decl pos) in
      let tv = MakeType.string reason_super in
      let (env, tv) = maybe_pessimise_type env tv in
      is_container tk tv env
    | (r_sub, Tprim Tnull) ->
      let pos = get_pos ty_sub in
      if is_nullable then
        return_val (MakeType.null (Reason.idx_string_from_decl pos)) env
      else
        invalid
          ~fail:
            (Some
               Typing_error.(
                 primary
                 @@ Primary.Null_container
                      {
                        pos = can_index.ci_expr_pos;
                        null_witness =
                          lazy
                            (Reason.to_string
                               "This is what makes me believe it can be `null`"
                               r_sub);
                      }))
          env
    | (r_sub, Toption t) ->
      if is_nullable then
        simplify_
          ~subtype_env
          ~this_ty
          ~lhs:{ sub_supportdyn; ty_sub = t }
          ~rhs
          env
      else
        invalid
          ~fail:
            (Some
               Typing_error.(
                 primary
                 @@ Primary.Null_container
                      {
                        pos = can_index.ci_expr_pos;
                        null_witness =
                          lazy
                            (Reason.to_string
                               "This is what makes me believe it can be `null`"
                               r_sub);
                      }))
          env
    | (r_sub, Tunion ty_subs) ->
      let simplify_union_l
          ~subtype_env ~this_ty ~update_reason (sub_supportdyn, ty_subs) rhs env
          =
        let f res ty_sub =
          let ty_sub = update_reason env ty_sub in
          with_hint_and
            res
            (simplify_
               ~subtype_env
               ~this_ty
               ~lhs:{ sub_supportdyn; ty_sub }
               ~rhs)
        in
        (* Prioritize types that aren't dynamic or intersections with dynamic
           to get better error messages *)
        let (last_tyl, first_tyl) =
          Typing_dynamic_utils.partition_union
            ~f:Common.contains_dynamic_through_intersection
            env
            ty_subs
        in
        let init =
          List.fold_left first_tyl ~init:((env, TL.valid) |> likely_solvable) ~f
        in
        List.fold_left last_tyl ~init ~f
      in
      simplify_union_l
        ~subtype_env
        ~this_ty
        ~update_reason:
          (Typing_env.update_reason ~f:(fun r_sub_prj ->
               Typing_reason.prj_union_sub ~sub:r_sub ~sub_prj:r_sub_prj))
        (sub_supportdyn, ty_subs)
        rhs
        env
    | (r_sub, Tintersection ty_subs) ->
      (* A & B <: C iif A <: C | !B *)
      (match Subtype_negation.find_type_with_exact_negation env ty_subs with
      | (env, Some non_ty, tyl) ->
        let ty_sub = MakeType.intersection r_sub tyl in
        let lift_rhs { reason_super; can_index } =
          mk_constraint_type (reason_super, Tcan_index can_index)
        and lhs = (sub_supportdyn, ty_sub)
        and rhs_subtype =
          Subtype.
            { super_supportdyn = false; super_like = false; ty_super = non_ty }
        and rhs_can_index = { reason_super; can_index } in
        let rhs = (reason_super, rhs_subtype, rhs_can_index) in
        Common.simplify_disj_r
          ~subtype_env
          ~this_ty
          ~fail
          ~lift_rhs
          ~mk_prop
          lhs
          rhs
          env
        |> likely_solvable
      | _ ->
        let update_reason =
          Typing_env.update_reason ~f:(fun r_sub_prj ->
              Typing_reason.prj_inter_sub ~sub:r_sub ~sub_prj:r_sub_prj)
        in
        let rec loop ty_subs tys errs env : with_hint =
          match ty_subs with
          | [] ->
            (match tys with
            | [] ->
              let err = Typing_error.intersect_opt @@ List.filter_opt errs in
              invalid ~fail:err env
            | _ ->
              let (env, ty) =
                Typing_intersection.intersect_list env r_sub tys
              in
              return_val ty env)
          | ty_sub :: ty_subs ->
            let ty_sub = update_reason env ty_sub in
            let (env, val_ty) = Env.fresh_type env can_index.ci_array_pos in
            let { env; prop; likely } =
              simplify_
                ~subtype_env
                ~this_ty
                ~lhs:{ sub_supportdyn; ty_sub }
                ~rhs:
                  {
                    reason_super;
                    can_index = { can_index with ci_val = val_ty };
                  }
                env
            in
            (match TL.get_error_if_unsat prop with
            | Some err -> loop ty_subs tys (err :: errs) env
            | _ ->
              if likely then
                with_hint_and { env; prop; likely } (fun env ->
                    loop ty_subs (val_ty :: tys) errs env)
              else
                (*I sm dropping errors here. Is that ok?*)
                loop ty_subs tys errs env)
        in
        loop ty_subs [] [] env)
    | (_r_sub, Tclass ((_, id), _, tup))
      when String.equal id SN.Collections.cPair ->
      do_tuple_basic tup
    | (_r_sub, Ttuple { t_required; t_optional; t_extra }) ->
      do_tuple_general t_required t_optional t_extra
    | (r_sub, Tshape (Shape_simple ts)) -> do_shape r_sub ts
    | (r_sub, Tshape (Shape_splat { ss_elems })) ->
      (* For field access on Shape_splat, merge all elements using rightmost-wins
         (expand tvars, recurse nested splats, resolve generics via bounds). A
         distributed union operand yields a union type here: re-dispatch so the
         [Tunion] arm above reads each member (a single shape). *)
      let (env, result) = Typing_corners.resolve_for_read env r_sub ss_elems in
      (match result with
      | Error missing ->
        let subtype_env =
          Subtype_env.map_on_error subtype_env ~f:(fun on_error ->
              Typing_error.Reasons_callback.prepend_reason
                on_error
                ~reason:
                  (lazy
                    ( Typing_corners.Missing_assignment.position missing,
                      "Shape-splat dependency analysis failed before projection"
                    )))
        in
        let fail =
          Subtype_env.fail
            subtype_env
            ~ty_sub:(LoclType ty_sub)
            ~ty_super:
              (ConstraintType
                 (mk_constraint_type (reason_super, Tcan_index can_index)))
        in
        invalid ~fail env
      | Ok read_ty ->
        (match get_node read_ty with
        | Tshape (Shape_simple ts) -> do_shape r_sub ts
        | _ ->
          simplify_
            ~subtype_env
            ~this_ty
            ~lhs:{ sub_supportdyn; ty_sub = read_ty }
            ~rhs
            env))
    | (r_sub, Tgeneric _generic_nm) ->
      let get_transitive_upper_bounds env ty =
        let rec iter seen env acc tyl =
          match tyl with
          | [] -> (env, acc)
          | ty :: tyl ->
            let (env, ety) = Env.expand_type env ty in
            (match get_node ety with
            | Tgeneric n ->
              if S_set.mem n seen then
                iter seen env acc tyl
              else
                iter
                  (S_set.add n seen)
                  env
                  acc
                  (Typing_set.elements (Env.get_upper_bounds env n) @ tyl)
            | _ -> iter seen env (Typing_set.add ty acc) tyl)
        in
        let (env, resl) = iter S_set.empty env Typing_set.empty [ty] in
        (env, Typing_set.elements resl)
      in
      let (env, tyl) = get_transitive_upper_bounds env ty_sub in
      if List.is_empty tyl then
        invalid ~fail env
      else
        let (env, ty) = Typing_intersection.intersect_list env r_sub tyl in
        simplify_
          ~subtype_env
          ~this_ty
          ~lhs:{ sub_supportdyn; ty_sub = ty }
          ~rhs
          env
    | (r_sub, Tnewtype (name_sub, [ty_sub], _))
      when String.equal name_sub SN.Classes.cSupportDyn ->
      simplify_
        ~subtype_env
        ~this_ty
        ~lhs:{ sub_supportdyn = Some r_sub; ty_sub }
        ~rhs
        env
    | (r_sub, Tnewtype (name_sub, [ty], _))
      when String.equal name_sub SN.FB.cTypeStructure ->
      let (env, ty_newtype) =
        Typing_utils.get_newtype_super env r_sub name_sub [ty]
      in
      (match deref ty_newtype with
      | ( r,
          Tshape
            (Shape_simple
              { s_origin = _; s_unknown_value = shape_kind; s_fields = fields })
        ) ->
        let (env, fields) =
          Typing_structure.transform_shapemap
            env
            can_index.ci_array_pos
            ty
            fields
        in
        let ty =
          mk
            ( r,
              Tshape
                (Shape_simple
                   {
                     s_origin = Missing_origin;
                     s_unknown_value = shape_kind;
                     s_fields = fields;
                   }) )
        in
        simplify_
          ~subtype_env
          ~this_ty
          ~lhs:{ sub_supportdyn; ty_sub = ty }
          ~rhs
          env
      | _ ->
        Common.simplify_newtype_l
          ~subtype_env
          ~this_ty
          ~mk_prop:simplify_
          (sub_supportdyn, r_sub, name_sub, ty_newtype)
          rhs
          env)
    | (r_sub, Tnewtype (name_sub, tyl, _)) ->
      let (env, ty_newtype) =
        Typing_utils.get_newtype_super env r_sub name_sub tyl
      in
      Common.simplify_newtype_l
        ~subtype_env
        ~this_ty
        ~mk_prop:simplify_
        (sub_supportdyn, r_sub, name_sub, ty_newtype)
        rhs
        env
    | (r_sub, Tdependent (dep_ty, ty_inner)) ->
      Common.simplify_dependent_l
        ~subtype_env
        ~this_ty
        ~mk_prop:simplify_
        (sub_supportdyn, r_sub, dep_ty, ty_inner)
        rhs
        env
    | (_r_sub, Tany _) -> return_val ty_sub env
    | _ -> invalid ~fail env
end

and Can_index_assign : sig
  type rhs = {
    reason_super: Reason.t;
    can_index_assign: can_index_assign;
  }

  include Constraint_handler with type rhs := rhs
end = struct
  type rhs = {
    reason_super: Reason.t;
    can_index_assign: can_index_assign;
  }

  (* Check that an index to a map-like collection passes the basic test of
   * being a subtype of arraykey
   *)
  let check_arraykey_index error env pos container_ty index_ty =
    let (env, container_ty) = Env.expand_type env container_ty in
    let reason =
      match get_node container_ty with
      | Tclass ((_, cn), _, _) -> Reason.index_class cn
      | _ -> Reason.index_array
    in
    let info_of_type ty = (get_pos ty, lazy (Typing_print.error env ty)) in
    let container_info = info_of_type container_ty in
    let index_info = info_of_type index_ty in
    let ty_arraykey = MakeType.arraykey (Reason.idx_dict pos) in
    (* If we have an error in coercion here, we will add a `Hole` indicating the
       actual and expected type. The `Hole` may then be used in a codemod to
       add a call to `UNSAFE_CAST` so we need to consider what type we expect.
       There is a somewhat common pattern in older parts of www to do something like:

       ```
       function keyset_issue(?string $x): keyset<string> {
         $xs = keyset<string>[];
         ...
         /* HH_FIXME[4435] keyset values must be arraykeys */
         $xs[] = $x;
         return Keyset\filter_nulls($xs);
       }
       ```
       (even though it is impossible for keysets to contain nulls).

       If we were to add an expected type of 'arraykey' here it would be
       correct but adding an `UNSAFE_CAST<?string,arraykey>($x)` means we
       get cascading errors; here, we now have the wrong return type.

       To try and prevent this, if this is an optional type where the nonnull
       part can be coerced to arraykey, we prefer that type as our expected type.
    *)
    let base_error = error pos container_info index_info in
    let (ty_actual, is_option) =
      match deref index_ty with
      | (_, Toption inner_ty) -> (inner_ty, true)
      | _ -> (index_ty, false)
    in
    let (env, e1) =
      Typing_coercion.coerce_type
        ~coerce_for_op:true
        pos
        reason
        env
        ty_actual
        ty_arraykey
        Enforced
      @@ Typing_error.Callback.always base_error
    in
    let (ty_mismatch, e2) =
      match e1 with
      | None when is_option ->
        (Error (index_ty, ty_actual), Some (Typing_error.primary base_error))
      | None -> (Ok index_ty, None)
      | Some _ -> (Error (index_ty, ty_arraykey), None)
    in
    Option.(
      iter ~f:(Typing_error_utils.add_typing_error ~env)
      @@ merge e1 e2 ~f:Typing_error.both);
    (env, ty_mismatch)

  let check_arraykey_index_write =
    let mk_err pos (container_pos, container_ty_name) (key_pos, key_ty_name) =
      Typing_error.Primary.Invalid_arraykey
        {
          pos;
          ctxt = `write;
          container_pos;
          container_ty_name;
          key_pos;
          key_ty_name;
        }
    in
    check_arraykey_index mk_err

  (* Since array update constraint solving is under development,
   * it is an experimental feature and will only be turned on
   * if the flag constraint_array_index_assign is set.
   *)
  let rec simplify
      ~subtype_env
      ~this_ty
      ~lhs:({ ty_sub; _ } as lhs)
      ~rhs:({ reason_super; can_index_assign } as rhs)
      env =
    let (env, ty_sub) = Env.expand_type env ty_sub in
    let ty_sub = LoclType ty_sub in
    let ty_super =
      ConstraintType
        (mk_constraint_type (reason_super, Tcan_index_assign can_index_assign))
    in
    if Logging.should_log_subtype_i env ~level:2 ty_sub ty_super then
      Logging.log_subtype_i
        ~this_ty
        ~function_name:"Typing_subtype.Can_index_assign.simplify"
        env
        ty_sub
        ty_super
      @@ fun () -> simplify_ ~subtype_env ~this_ty ~lhs ~rhs env
    else
      simplify_ ~subtype_env ~this_ty ~lhs ~rhs env

  and simplify_
      ~subtype_env
      ~this_ty
      ~lhs:{ ty_sub; sub_supportdyn }
      ~rhs:({ reason_super; can_index_assign = cia } as rhs)
      env =
    let maybe_make_supportdyn r env ty =
      if Option.is_some sub_supportdyn then
        Typing_utils.make_supportdyn r env ty
      else
        (env, ty)
    in
    let (env, ty_sub) = Env.expand_type env ty_sub in
    let subtype_env =
      Subtype_env.possibly_add_violated_constraint
        subtype_env
        ~r_sub:(get_reason ty_sub)
        ~r_super:reason_super
    in
    let fail =
      Subtype_env.fail
        subtype_env
        ~ty_sub:(LoclType ty_sub)
        ~ty_super:
          (ConstraintType
             (mk_constraint_type (reason_super, Tcan_index_assign cia)))
    in
    let invalid ~fail env = invalid ~fail env in
    let arity_error r name env =
      invalid
        ~fail:
          (Some
             Typing_error.(
               primary
               @@ Primary.Array_get_arity
                    { pos = cia.cia_expr_pos; name; decl_pos = Reason.to_pos r }))
        env
    in
    let simplify_default ~subtype_env lhs rhs env =
      Subtype.simplify
        ~subtype_env
        ~this_ty
        ~lhs:{ sub_supportdyn; ty_sub = lhs }
        ~rhs:{ super_like = false; super_supportdyn = false; ty_super = rhs }
        env
    in
    let simplify_key tk env =
      let tk =
        Typing_make_type.locl_like (Reason.dynamic_coercion (get_reason tk)) tk
      in
      simplify_default ~subtype_env cia.cia_key tk env
    in
    let simplify_val tv env =
      simplify_default ~subtype_env tv cia.cia_val env
    in
    let pessimise_type env ty =
      Typing_union.union env ty (MakeType.dynamic (get_reason ty))
    in
    let maybe_pessimise_type env ty =
      if Typechecker_options.pessimise_builtins (Env.get_tcopt env) then
        pessimise_type env ty
      else
        (env, ty)
    in
    let mk_prop ~subtype_env ~this_ty ~lhs ~rhs =
      simplify ~subtype_env ~this_ty ~lhs ~rhs
    in
    let supports_dynamic ty env =
      let r = get_reason ty in
      simplify_default ~subtype_env ty (MakeType.supportdyn_mixed r) env
    in
    let got_dynamic env =
      let tv = MakeType.dynamic (get_reason ty_sub) in
      supports_dynamic cia.cia_key env
      &&& supports_dynamic cia.cia_write
      &&& simplify_val tv
    in
    match deref ty_sub with
    | (_, Tvar _) ->
      mk_issubtype_prop
        ~sub_supportdyn
        ~is_dynamic_aware:subtype_env.Subtype_env.is_dynamic_aware
        env
        (LoclType ty_sub)
        (ConstraintType
           (mk_constraint_type (reason_super, Tcan_index_assign cia)))
    | (r_sub, Tclass ((_, n), _, argl))
      when String.equal n SN.Collections.cVector ->
      (match argl with
      | [tv] ->
        let tk = MakeType.int (Reason.idx_vector cia.cia_index_pos) in
        let (env, tv) = maybe_pessimise_type env tv in
        let (env, tv) = maybe_make_supportdyn r_sub env tv in
        simplify_key tk env
        &&& simplify_default ~subtype_env cia.cia_write tv
        &&& simplify_val ty_sub
      | _ -> arity_error r_sub n env)
    | (r_sub, Tclass (((_, n) as id), e, argl))
      when String.equal n SN.Collections.cVec ->
      (match argl with
      | [tv] ->
        let tk = MakeType.int (Reason.idx_vector cia.cia_index_pos) in
        simplify_key tk env &&& fun env ->
        let (env, tv) = maybe_make_supportdyn r_sub env tv in
        let (env, tv') = Typing_union.union env tv cia.cia_write in
        let ty = mk (r_sub, Tclass (id, e, [tv'])) in
        simplify_val ty env
      | _ -> arity_error r_sub n env)
    | (r_sub, Tclass (((_, n) as id), e, argl))
      when String.equal n SN.Collections.cDict ->
      let (env, idx_err) =
        check_arraykey_index_write env cia.cia_expr_pos ty_sub cia.cia_key
      in
      (match argl with
      | [tk; tv] ->
        let (env, tv) = maybe_make_supportdyn r_sub env tv in
        let (env, tk') =
          let ak_t = MakeType.arraykey (Reason.idx_dict cia.cia_index_pos) in
          match idx_err with
          | Ok _ ->
            let (env, tkey_new) =
              Typing_intersection.intersect
                env
                ~r:(Reason.idx_dict cia.cia_index_pos)
                cia.cia_key
                ak_t
            in
            Typing_union.union env tk tkey_new
          | _ -> Typing_union.union env tk cia.cia_key
        in
        let (env, tv') = Typing_union.union env tv cia.cia_write in
        let ty = mk (r_sub, Tclass (id, e, [tk'; tv'])) in
        simplify_val ty env
      | _ -> arity_error r_sub n env)
    | (r_sub, Tclass ((_, n), _, argl)) when String.equal n SN.Collections.cMap
      ->
      (match argl with
      | [tk; tv] ->
        let (env, tv) = maybe_pessimise_type env tv in
        let (env, tv) = maybe_make_supportdyn r_sub env tv in
        let (env, tk) = maybe_pessimise_type env tk in
        simplify_key tk env
        &&& simplify_default ~subtype_env cia.cia_write tv
        &&& simplify_val ty_sub
      | _ -> arity_error r_sub n env)
    | (r_sub, Ttuple { t_required; t_optional = []; t_extra = Tvariadic _ }) ->
      let fail reason =
        invalid
          ~fail:
            (Some
               Typing_error.(
                 primary
                 @@ Primary.Generic_unify
                      {
                        pos = cia.cia_index_pos;
                        msg = Reason.string_of_ureason reason;
                      }))
          env
      in
      begin
        match cia.cia_index_expr with
        | (_, _, Int n) ->
          let idx = int_of_string_opt n in
          (match Option.map ~f:(List.split_n t_required) idx with
          | Some (tyl', _ :: tyl'') ->
            let ty = MakeType.tuple r_sub (tyl' @ (cia.cia_write :: tyl'')) in
            let (env, ty) = maybe_make_supportdyn r_sub env ty in
            simplify_val ty env
          | _ -> fail Reason.index_tuple)
        | _ -> fail Reason.URtuple_access
      end
    | ( r_sub,
        Tshape
          (Shape_simple
            { s_origin = _; s_unknown_value = shape_kind; s_fields = fdm }) ) ->
      (match
         Typing_shapes.tshape_field_name_with_ty_err env cia.cia_index_expr
       with
      | Error ty_err -> invalid ~fail:(Some ty_err) env
      | Ok field ->
        let (env, fdm) =
          if Option.is_some sub_supportdyn then
            let f env _name { sft_optional; sft_ty } =
              let (env, sft_ty) = TUtils.make_supportdyn r_sub env sft_ty in
              (env, { sft_optional; sft_ty })
            in
            TShapeMap.map_env f env fdm
          else
            (env, fdm)
        in
        let fdm' =
          TShapeMap.add
            field
            { sft_optional = false; sft_ty = cia.cia_write }
            fdm
        in
        let ty =
          mk
            ( r_sub,
              Tshape
                (Shape_simple
                   {
                     s_origin = Missing_origin;
                     s_unknown_value = shape_kind;
                     s_fields = fdm';
                   }) )
        in
        simplify_val ty env)
    | (r_sub, Tshape (Shape_splat { ss_elems })) ->
      (* A field write is a runtime dict write: it adds a new field or overwrites
         an existing one, and under rightmost-wins the written field must win over
         every element. [set_rightmost_field] sets it on the rightmost concrete
         element in place, or appends a new closed single-field element when the
         rightmost element is opaque matching the direct write path in
         [typing_array_access]. *)
      (match
         Typing_shapes.tshape_field_name_with_ty_err env cia.cia_index_expr
       with
      | Error ty_err -> invalid ~fail:(Some ty_err) env
      | Ok field ->
        let ss_elems =
          List.map ss_elems ~f:(fun ty -> snd (Env.expand_type env ty))
        in
        let (env, _err, ty) =
          Typing_shape_normalize.set_rightmost_field
            ss_elems
            field
            { sft_optional = false; sft_ty = cia.cia_write }
            ~pessimize_existing:(Option.is_some sub_supportdyn)
            ~reason:r_sub
            env
        in
        simplify_val ty env)
    | (_, Tclass ((_, n), _, _))
      when String.equal n SN.Collections.cAnyArray
           && Tast.is_under_dynamic_assumptions env.Typing_env_types.checked ->
      got_dynamic env
    | (r_sub, Tclass ((_, n), _, _))
      when String.equal n SN.Collections.cKeyedContainer
           || String.equal n SN.Collections.cAnyArray
           || String.equal n SN.Collections.cPair
           || String.equal n SN.Collections.cConstVector
           || String.equal n SN.Collections.cImmVector
           || String.equal n SN.Collections.cConstMap
           || String.equal n SN.Collections.cImmMap ->
      invalid
        ~fail:
          (Some
             Typing_error.(
               primary
               @@ Primary.Const_mutation
                    {
                      pos = cia.cia_expr_pos;
                      decl_pos = Reason.to_pos r_sub;
                      ty_name = lazy (Typing_print.error env ty_sub);
                    }))
        env
    | (r_sub, Tintersection ty_subs) ->
      (* A & B <: C iif A <: C | !B *)
      (match Subtype_negation.find_type_with_exact_negation env ty_subs with
      | (env, Some non_ty, tyl) ->
        let ty_sub = MakeType.intersection r_sub tyl in
        let lift_rhs { reason_super; can_index_assign } =
          mk_constraint_type (reason_super, Tcan_index_assign can_index_assign)
        and lhs = (sub_supportdyn, ty_sub)
        and rhs_subtype =
          Subtype.
            { super_supportdyn = false; super_like = false; ty_super = non_ty }
        and rhs_can_index_assign = { reason_super; can_index_assign = cia } in
        let rhs = (reason_super, rhs_subtype, rhs_can_index_assign) in
        Common.simplify_disj_r
          ~subtype_env
          ~this_ty
          ~fail
          ~lift_rhs
          ~mk_prop
          lhs
          rhs
          env
      | _ ->
        let update_reason =
          Typing_env.update_reason ~f:(fun r_sub_prj ->
              Typing_reason.prj_inter_sub ~sub:r_sub ~sub_prj:r_sub_prj)
        in
        let rec loop ty_subs tys errs env =
          match ty_subs with
          | [] ->
            (match tys with
            | [] ->
              let err = Typing_error.intersect_opt @@ List.filter_opt errs in
              invalid ~fail:err env
            | _ ->
              let (env, ty) =
                Typing_intersection.intersect_list env r_sub tys
              in
              simplify_val ty env)
          | ty_sub :: ty_subs ->
            let ty_sub = update_reason env ty_sub in
            let (env, val_ty) = Env.fresh_type env cia.cia_array_pos in
            let (env, prop) =
              simplify_
                ~subtype_env
                ~this_ty
                ~lhs:{ sub_supportdyn; ty_sub }
                ~rhs:
                  {
                    reason_super;
                    can_index_assign = { cia with cia_val = val_ty };
                  }
                env
            in
            (match TL.get_error_if_unsat prop with
            | Some err -> loop ty_subs tys (err :: errs) env
            | _ ->
              if true then
                (env, prop) &&& fun env -> loop ty_subs (val_ty :: tys) errs env
              else
                (*I sm dropping errors here. Is that ok?*)
                loop ty_subs tys errs env)
        in
        loop ty_subs [] [] env)
    | (r_sub, Tgeneric _generic_nm) ->
      let get_transitive_upper_bounds env ty =
        let rec iter seen env acc tyl =
          match tyl with
          | [] -> (env, acc)
          | ty :: tyl ->
            let (env, ety) = Env.expand_type env ty in
            (match get_node ety with
            | Tgeneric n ->
              if S_set.mem n seen then
                iter seen env acc tyl
              else
                iter
                  (S_set.add n seen)
                  env
                  acc
                  (Typing_set.elements (Env.get_upper_bounds env n) @ tyl)
            | _ -> iter seen env (Typing_set.add ty acc) tyl)
        in
        let (env, resl) = iter S_set.empty env Typing_set.empty [ty] in
        (env, Typing_set.elements resl)
      in
      let (env, tyl) = get_transitive_upper_bounds env ty_sub in
      if List.is_empty tyl then
        invalid ~fail env
      else
        let (env, ty) = Typing_intersection.intersect_list env r_sub tyl in
        simplify_
          ~subtype_env
          ~this_ty
          ~lhs:{ sub_supportdyn; ty_sub = ty }
          ~rhs
          env
    | (_, Tdynamic _) -> got_dynamic env
    | (_, Tany _) -> simplify_val ty_sub env
    | (_, Tprim Tnull)
      when Tast.is_under_dynamic_assumptions env.Typing_env_types.checked ->
      got_dynamic env
    | (r_sub, Tclass ((_, id), _, _)) when String.equal SN.Classes.cString id ->
      let tk = MakeType.int (Reason.idx (cia.cia_index_pos, r_sub)) in
      let tv = MakeType.string (Reason.witness cia.cia_expr_pos) in
      let (env, tv) = maybe_pessimise_type env tv in
      simplify_key tk env
      &&& simplify_default ~subtype_env cia.cia_write tv
      &&& simplify_val ty_sub
    | (r_sub, Tunion ty_subs) ->
      Common.simplify_union_l
        ~subtype_env
        ~this_ty
        ~update_reason:
          (Typing_env.update_reason ~f:(fun r_sub_prj ->
               Typing_reason.prj_union_sub ~sub:r_sub ~sub_prj:r_sub_prj))
        ~mk_prop
        (sub_supportdyn, ty_subs)
        rhs
        env
    | (r_sub, Tnewtype (name_sub, tyl, _)) ->
      let (env, ty_newtype) =
        Typing_utils.get_newtype_super env r_sub name_sub tyl
      in
      Common.simplify_newtype_l
        ~subtype_env
        ~this_ty
        ~mk_prop:simplify_
        (sub_supportdyn, r_sub, name_sub, ty_newtype)
        rhs
        env
    | (r_sub, Tdependent (dep_ty, ty_inner)) ->
      Common.simplify_dependent_l
        ~subtype_env
        ~this_ty
        ~mk_prop:simplify_
        (sub_supportdyn, r_sub, dep_ty, ty_inner)
        rhs
        env
    | _ -> invalid ~fail env
end

and Can_traverse : sig
  type rhs = {
    reason_super: Reason.t;
    can_traverse: can_traverse;
  }

  include Constraint_handler with type rhs := rhs
end = struct
  type rhs = {
    reason_super: Reason.t;
    can_traverse: can_traverse;
  }

  let subtype_with_dynamic ~subtype_env ~this_ty ~lhs { ct_key; ct_val; _ } env
      =
    let subty_prop_val env =
      Subtype.(
        simplify
          ~subtype_env
          ~this_ty
          ~lhs
          ~rhs:
            { super_like = false; super_supportdyn = false; ty_super = ct_val }
          env)
    and subty_prop_key env =
      match ct_key with
      | None -> valid env
      | Some ct_key ->
        Subtype.(
          simplify
            ~subtype_env
            ~this_ty
            ~lhs
            ~rhs:
              {
                super_like = false;
                super_supportdyn = false;
                ty_super = ct_key;
              }
            env)
    in
    subty_prop_val env &&& subty_prop_key

  let rec simplify ~subtype_env ~this_ty ~lhs ~rhs env =
    let { sub_supportdyn = _; ty_sub } = lhs in
    let { reason_super; can_traverse } = rhs in
    let (env, ty_sub) = Env.expand_type env ty_sub in
    let ty_sub = LoclType ty_sub in
    let ty_super =
      ConstraintType
        (mk_constraint_type (reason_super, Tcan_traverse can_traverse))
    in
    if Logging.should_log_subtype_i env ~level:2 ty_sub ty_super then
      Logging.log_subtype_i
        ~this_ty
        ~function_name:"Typing_subtype.Can_traverse.simplify"
        env
        ty_sub
        ty_super
      @@ fun () -> simplify_ ~subtype_env ~this_ty ~lhs ~rhs env
    else
      simplify_ ~subtype_env ~this_ty ~lhs ~rhs env

  and simplify_
      ~subtype_env
      ~this_ty
      ~lhs:{ sub_supportdyn; ty_sub = lty_sub }
      ~rhs:({ reason_super = r; can_traverse = ct } as rhs)
      env =
    let (env, lty_sub) = Env.expand_type env lty_sub in
    let subtype_env =
      Subtype_env.possibly_add_violated_constraint
        subtype_env
        ~r_sub:(get_reason lty_sub)
        ~r_super:r
    in
    let fail =
      Subtype_env.fail
        subtype_env
        ~ty_sub:(LoclType lty_sub)
        ~ty_super:(ConstraintType (mk_constraint_type (r, Tcan_traverse ct)))
    in
    if TUtils.is_tyvar_error env lty_sub then
      let trav_ty = can_traverse_to_iface ct in
      Subtype.(
        simplify
          ~subtype_env
          ~this_ty
          ~lhs:{ sub_supportdyn; ty_sub = lty_sub }
          ~rhs:
            { super_like = false; super_supportdyn = false; ty_super = trav_ty }
          env)
    else
      let mk_prop ~subtype_env ~this_ty ~lhs ~rhs =
        simplify ~subtype_env ~this_ty ~lhs ~rhs
      in
      match deref lty_sub with
      | (_, Tdynamic _) ->
        subtype_with_dynamic
          ~subtype_env
          ~this_ty
          ~lhs:{ sub_supportdyn; ty_sub = lty_sub }
          ct
          env
      | _
        when Option.is_some sub_supportdyn
             && Tast.is_under_dynamic_assumptions env.checked ->
        subtype_with_dynamic
          ~subtype_env
          ~this_ty
          ~lhs:{ sub_supportdyn; ty_sub = lty_sub }
          ct
          env
      | (_, Tclass _)
      | (_, Tvec_or_dict _)
      | (_, Tany _) ->
        let trav_ty = can_traverse_to_iface ct in
        Subtype.(
          simplify
            ~subtype_env
            ~this_ty
            ~lhs:{ sub_supportdyn; ty_sub = lty_sub }
            ~rhs:
              {
                super_like = false;
                super_supportdyn = false;
                ty_super = trav_ty;
              }
            env)
      | (r_sub, Tunion ty_subs) ->
        Common.simplify_union_l
          ~subtype_env
          ~this_ty
          ~mk_prop
          ~update_reason:
            (Typing_env.update_reason ~f:(fun r_sub_prj ->
                 Typing_reason.prj_union_sub ~sub:r_sub ~sub_prj:r_sub_prj))
          (sub_supportdyn, ty_subs)
          rhs
          env
      | (_, Tvar id) ->
        (* If the type is already in the upper bounds of the type variable,
           * then we already know that this subtype assertion is valid
        *)
        let cty = ConstraintType (mk_constraint_type (r, Tcan_traverse ct)) in
        if ITySet.mem cty (Env.get_tyvar_upper_bounds env id) then
          valid env
        else
          mk_issubtype_prop
            ~sub_supportdyn
            ~is_dynamic_aware:subtype_env.Subtype_env.is_dynamic_aware
            env
            (LoclType lty_sub)
            (ConstraintType (mk_constraint_type (r, Tcan_traverse ct)))
      | (r_sub, Tintersection ty_subs) ->
        (* A & B <: C iif A <: C | !B *)
        (match Subtype_negation.find_type_with_exact_negation env ty_subs with
        | (env, Some non_ty, tyl) ->
          let ty_sub = MakeType.intersection r_sub tyl in

          let mk_prop = simplify
          and lift_rhs { reason_super; can_traverse } =
            mk_constraint_type (reason_super, Tcan_traverse can_traverse)
          and lhs = (sub_supportdyn, ty_sub)
          and rhs_subtype =
            Subtype.
              {
                super_supportdyn = false;
                super_like = false;
                ty_super = non_ty;
              }
          and rhs_can_traverse = { reason_super = r; can_traverse = ct } in
          let rhs = (r, rhs_subtype, rhs_can_traverse) in
          Common.simplify_disj_r
            ~subtype_env
            ~this_ty
            ~fail
            ~lift_rhs
            ~mk_prop
            lhs
            rhs
            env
        | _ ->
          Common.simplify_intersection_l
            ~subtype_env
            ~this_ty
            ~fail
            ~mk_prop
            ~update_reason:
              (Typing_env.update_reason ~f:(fun r_sub_prj ->
                   Typing_reason.prj_inter_sub ~sub:r_sub ~sub_prj:r_sub_prj))
            (sub_supportdyn, ty_subs)
            rhs
            env)
      | (r_sub, Tgeneric generic_nm) ->
        Common.simplify_generic_l
          ~subtype_env
          ~this_ty
          ~fail
          ~mk_prop
          (sub_supportdyn, r_sub, generic_nm)
          rhs
          rhs
          env
      | (r_sub, Tnewtype (alias_name, tyl, _)) ->
        let (env, ty_newtype) =
          Typing_utils.get_newtype_super env r_sub alias_name tyl
        in
        Common.simplify_newtype_l
          ~subtype_env
          ~this_ty
          ~mk_prop
          (sub_supportdyn, r_sub, alias_name, ty_newtype)
          rhs
          env
      | (r_sub, Tdependent (dep_ty, ty_inner)) ->
        Common.simplify_dependent_l
          ~subtype_env
          ~this_ty
          ~mk_prop
          (sub_supportdyn, r_sub, dep_ty, ty_inner)
          rhs
          env
      | (_, Toption _)
      | (_, Tprim _)
      | (_, Tnonnull)
      | (_, Tneg _)
      | (_, Tfun _)
      | (_, Ttuple _)
      | (_, Tshape _)
      | (_, Taccess _)
      | (_, Tlabel _) ->
        invalid ~fail env
      | (_, Tclass_ptr _) ->
        (* TODO(T199606542) confirm CanTraverse behavior for class pointers *)
        invalid ~fail env
end

and Has_type_member : sig
  type rhs = {
    reason_super: Reason.t;
    has_type_member: has_type_member;
  }

  include Constraint_handler with type rhs := rhs
end = struct
  type rhs = {
    reason_super: Reason.t;
    has_type_member: has_type_member;
  }

  let rec simplify ~subtype_env ~this_ty ~lhs ~rhs env =
    let { sub_supportdyn = _; ty_sub } = lhs in
    let { reason_super; has_type_member } = rhs in
    let (env, ty_sub) = Env.expand_type env ty_sub in
    let ty_sub = LoclType ty_sub in
    let ty_super =
      ConstraintType
        (mk_constraint_type (reason_super, Thas_type_member has_type_member))
    in
    if Logging.should_log_subtype_i env ~level:2 ty_sub ty_super then
      Logging.log_subtype_i
        ~this_ty
        ~function_name:"Typing_subtype.Has_type_member.simplify"
        env
        ty_sub
        ty_super
      @@ fun () -> simplify_ ~subtype_env ~this_ty ~lhs ~rhs env
    else
      simplify_ ~subtype_env ~this_ty ~lhs ~rhs env

  and simplify_
      ~subtype_env
      ~this_ty
      ~lhs:{ sub_supportdyn; ty_sub }
      ~rhs:({ reason_super = r; has_type_member = htm } as rhs)
      env =
    let { htm_id = memid; htm_lower = memloty; htm_upper = memupty } = htm in
    let htmty = ConstraintType (mk_constraint_type (r, Thas_type_member htm)) in
    let (env, ty_sub) = Env.expand_type env ty_sub in
    let subtype_env =
      Subtype_env.possibly_add_violated_constraint
        subtype_env
        ~r_sub:(get_reason ty_sub)
        ~r_super:r
    in
    let fail =
      Subtype_env.fail subtype_env ~ty_sub:(LoclType ty_sub) ~ty_super:htmty
    in

    let ity_super =
      ConstraintType (mk_constraint_type (r, Thas_type_member htm))
    in
    let ity_sub = LoclType ty_sub in
    let cstrs = Subtype_env.lazy_tparam_constraints subtype_env in
    let secondary_error =
      Subtype_env.mk_secondary_error cstrs subtype_env ity_sub ity_super
    in
    (* Contextualize errors that may be generated when
                         * checking refinement bounds. *)
    let subtype_env =
      Subtype_env.map_on_error subtype_env ~f:(fun on_error ->
          let open Typing_error.Reasons_callback in
          prepend_on_apply on_error secondary_error)
    in

    let simplify_subtype_bound kind ~bound ty env =
      let subtype_env =
        Subtype_env.map_on_error subtype_env ~f:(fun on_error ->
            let open Typing_error in
            let pos = Reason.to_pos (get_reason bound) in
            Reasons_callback.prepend_on_apply
              on_error
              (Secondary.Violated_refinement_constraint { cstr = (kind, pos) }))
      in
      let this_ty = None in
      match kind with
      | `As ->
        Subtype.(
          simplify
            ~subtype_env
            ~this_ty
            ~lhs:{ sub_supportdyn = None; ty_sub = ty }
            ~rhs:
              { super_like = false; super_supportdyn = false; ty_super = bound }
            env)
      | `Super ->
        Subtype.(
          simplify
            ~subtype_env
            ~this_ty
            ~lhs:{ sub_supportdyn = None; ty_sub = bound }
            ~rhs:{ super_like = false; super_supportdyn = false; ty_super = ty }
            env)
    in

    let concrete_rigid_tvar_access env ucckind bndtys =
      (* First, we try to discharge the subtype query on the bound; if
       * that fails, we mint a fresh rigid type variable to represent
       * the concrete type constant and try to solve the query using it *)
      let bndty = MakeType.intersection (get_reason ty_sub) bndtys in
      let ( ||| ) = ( ||| ) ~fail in
      simplify
        ~subtype_env
        ~this_ty
        ~lhs:{ sub_supportdyn = None; ty_sub = bndty }
        ~rhs
        env
      ||| fun env ->
      (* TODO(refinements): The treatment of `this_ty` below is
       * no good; see below. *)
      let (env, dtmemty) =
        Typing_type_member.make_type_member
          env
          ~this_ty:(Option.value this_ty ~default:ty_sub)
          ~on_error:subtype_env.Subtype_env.on_error
          ucckind
          bndtys
          (Reason.to_pos r, memid)
      in
      simplify_subtype_bound `As dtmemty ~bound:memupty env
      &&& simplify_subtype_bound `Super ~bound:memloty dtmemty
    in

    match deref ty_sub with
    | (_r_sub, Tclass (x_sub, exact_sub, _tyl_sub)) ->
      let (env, type_member) =
        (* TODO(refinements): The treatment of `this_ty` below is
         * no good; we should not default to `ty_sub`. `this_ty`
         * will be used when a type constant refers to another
         * constant either in its def or in its bounds.
         * See related FIXME(T59448452) above. *)
        Typing_type_member.lookup_class_type_member
          env
          ~this_ty:(Option.value this_ty ~default:ty_sub)
          ~on_error:subtype_env.Subtype_env.on_error
          (x_sub, exact_sub)
          (Reason.to_pos r, memid)
      in
      (match type_member with
      | Typing_type_member.NotYetAvailable ->
        failwith "TODO(hverr): propagate decl_entry"
      | Typing_type_member.Error err -> invalid ~fail:err env
      | Typing_type_member.Exact ty ->
        simplify_subtype_bound `As ty ~bound:memupty env
        &&& simplify_subtype_bound `Super ~bound:memloty ty
      | Typing_type_member.Abstract { name; lower = loty; upper = upty } ->
        let r_bnd = Reason.tconst_no_cstr name in
        let loty = Option.value ~default:(MakeType.nothing r_bnd) loty in
        let upty = Option.value ~default:(MakeType.mixed r_bnd) upty in
        (* In case the refinement is exact we check that upty <: loty;
         * doing the check early gives us a better chance at generating
         * good error messages. The unification errors we get when
         * doing this check are usually unhelpful, so we drop them. *)
        let is_exact = phys_equal memloty memupty in
        (if is_exact then
          let subtype_env =
            Subtype_env.map_on_error
              subtype_env
              ~f:Typing_error.Reasons_callback.drop_reasons_on_apply
          in
          Subtype.(
            simplify
              ~subtype_env
              ~this_ty
              ~lhs:{ sub_supportdyn = None; ty_sub = upty }
              ~rhs:
                {
                  super_like = false;
                  super_supportdyn = false;
                  ty_super = loty;
                }
              env)
        else
          valid env)
        &&& simplify_subtype_bound `As upty ~bound:memupty
        &&& simplify_subtype_bound `Super ~bound:memloty loty)
    | (_r_sub, Tdependent (DTexpr eid, bndty)) ->
      concrete_rigid_tvar_access env (Typing_type_member.EDT eid) [bndty]
    | (_r_sub, Tgeneric s) when String.equal s SN.Typehints.this ->
      let bnd_tys = Typing_set.elements (Env.get_upper_bounds env s) in
      concrete_rigid_tvar_access env Typing_type_member.This bnd_tys
    | (_, Tvar _) ->
      mk_issubtype_prop
        ~sub_supportdyn
        ~is_dynamic_aware:subtype_env.Subtype_env.is_dynamic_aware
        env
        (LoclType ty_sub)
        htmty
    | (r_sub, Tunion ty_subs) ->
      Common.simplify_union_l
        ~subtype_env
        ~this_ty
        ~mk_prop:simplify
        ~update_reason:
          (Typing_env.update_reason ~f:(fun r_sub_prj ->
               Typing_reason.prj_union_sub ~sub:r_sub ~sub_prj:r_sub_prj))
        (sub_supportdyn, ty_subs)
        rhs
        env
    | (r_sub, Tintersection ty_subs) ->
      Common.simplify_intersection_l
        ~subtype_env
        ~this_ty
        ~fail
        ~mk_prop:simplify
        ~update_reason:
          (Typing_env.update_reason ~f:(fun r_sub_prj ->
               Typing_reason.prj_union_sub ~sub:r_sub ~sub_prj:r_sub_prj))
        (sub_supportdyn, ty_subs)
        rhs
        env
    | (r_generic, Tgeneric generic_nm) ->
      (match
         VisitedGoalsInternal.try_add_visited_generic_sub
           subtype_env.Subtype_env.visited_internal
           generic_nm
           htmty
       with
      | None -> invalid ~fail env
      | Some new_visited ->
        let subtype_env =
          Subtype_env.set_visited_internal subtype_env new_visited
        in
        Common.simplify_generic_l
          ~subtype_env
          ~this_ty
          ~fail
          ~mk_prop:simplify
          (sub_supportdyn, r_generic, generic_nm)
          rhs
          rhs
          env)
    | ( _,
        ( Tany _ | Tdynamic _ | Tnonnull | Toption _ | Tprim _ | Tneg _ | Tfun _
        | Ttuple _ | Tshape _ | Tvec_or_dict _ | Taccess _ | Tnewtype _
        | Tlabel _ ) ) ->
      invalid ~fail env
    | (_, Tclass_ptr _) ->
      (* TODO(T199606542) Confirm if class pointers appear in HasTypeMember positions *)
      invalid ~fail env
end

and Has_member : sig
  type rhs = {
    reason_super: Reason.t;
    has_member: has_member;
  }

  include Constraint_handler with type rhs := rhs
end = struct
  type rhs = {
    reason_super: Reason.t;
    has_member: has_member;
  }

  (* This is a duplicate of logic in Typing_error_utils, due to conversion of primary errors to secondary errors
     on some code paths for Typing_object_get, which throws out quickfix information (unsafe for secondary errors). *)
  let add_obj_get_quickfixes
      ty_err (on_error : Typing_error.Reasons_callback.t option) :
      Typing_error.Reasons_callback.t option =
    match ty_err with
    | Typing_error.(Error.Primary (Primary.Null_member { pos; obj_pos_opt; _ }))
      ->
      let quickfixes =
        match obj_pos_opt with
        | Some obj_pos ->
          let (obj_pos_start_line, _) = Pos.line_column obj_pos in
          let (rhs_pos_start_line, rhs_pos_start_column) =
            Pos.line_column pos
          in
          (*
        heuristic: if the lhs and rhs of the Objget are on the same line, then we assume they are
        separated by two characters (`->`). So we do not generate a quickfix for chained Objgets:
        ```
        obj
        ->rhs
        ```
      *)
          if obj_pos_start_line = rhs_pos_start_line then
            let width = 2 (* length of "->" *) in
            let quickfix_pos =
              pos
              |> Pos.set_col_start (rhs_pos_start_column - width)
              |> Pos.set_col_end rhs_pos_start_column
            in
            [
              Quickfix.make_eager_default_hint_style
                ~title:"Add null-safe get"
                ~new_text:"?->"
                quickfix_pos;
            ]
          else
            []
        | None -> []
      in
      Option.map
        ~f:(fun cb ->
          Typing_error.Reasons_callback.add_quickfixes cb quickfixes)
        on_error
    | _ -> on_error

  let typing_obj_get
      ~subtype_env
      ~this_ty
      ~class_id
      ~member_id
      ~explicit_targs
      ~member_ty
      ~has_member
      ty_sub
      env =
    let (explicit_targs, is_method) =
      match explicit_targs with
      | None -> ([], false)
      | Some targs -> (targs, true)
    in
    let (res, (obj_get_ty, _tal)) =
      Typing_object_get.obj_get
        ~obj_pos:(fst member_id)
          (* `~obj_pos:name_pos` is a lie: `name_pos` is the rhs of `->` or `?->` *)
        ~is_method
        ~coerce_from_ty:None
        ~nullsafe:None
        ~explicit_targs
        ~class_id
        ~member_id
        ~on_error:Typing_error.Callback.unify_error
        env
        ty_sub
    in
    let prop =
      match res with
      | (env, None) -> valid env
      | (env, Some ty_err) ->
        let on_error =
          add_obj_get_quickfixes ty_err subtype_env.Subtype_env.on_error
        in
        let discard_outermost (err : Typing_error.t) : Typing_error.t =
          match err with
          | Apply_reasons (_, Of_error err) when is_method -> err
          | _ -> err
        in
        let fail =
          Option.map
            on_error
            ~f:
              Typing_error.(
                fun on_error ->
                  discard_outermost
                    (apply_reasons ~on_error @@ Secondary.Of_error ty_err))
        in
        invalid env ~fail
    in
    let subtype_env =
      match has_member with
      | None -> subtype_env
      | Some has_member ->
        { subtype_env with has_member_arg_posl = Some has_member.hmm_args_pos }
    in
    prop
    &&& Subtype.(
          simplify
            ~subtype_env
            ~this_ty
            ~lhs:{ sub_supportdyn = None; ty_sub = obj_get_ty }
            ~rhs:
              {
                super_like = false;
                super_supportdyn = false;
                ty_super = member_ty;
              })

  let rec simplify ~subtype_env ~this_ty ~lhs ~rhs env =
    let { sub_supportdyn = _; ty_sub } = lhs in
    let { reason_super = r; has_member = has_member_ty } = rhs in
    let (env, ty_sub) = Env.expand_type env ty_sub in
    let ty_sub = LoclType ty_sub in
    let ty_super =
      ConstraintType (mk_constraint_type (r, Thas_member has_member_ty))
    in
    if Logging.should_log_subtype_i env ~level:2 ty_sub ty_super then
      Logging.log_subtype_i
        ~this_ty
        ~function_name:"Typing_subtype.Has_member.simplify"
        env
        ty_sub
        ty_super
      @@ fun () -> simplify_ ~subtype_env ~this_ty ~lhs ~rhs env
    else
      simplify_ ~subtype_env ~this_ty ~lhs ~rhs env

  and simplify_
      ~subtype_env
      ~this_ty
      ~lhs:{ sub_supportdyn; ty_sub }
      ~rhs:({ reason_super = r; has_member = has_member_ty } as rhs)
      env =
    let {
      hm_name = (name_pos, name_) as member_id;
      hm_type = member_ty;
      hm_class_id = class_id;
      hm_method = method_info;
    } =
      has_member_ty
    in
    let is_method = Option.is_some method_info in
    let explicit_targs =
      Option.map method_info ~f:(fun { hmm_explicit_targs; _ } ->
          hmm_explicit_targs)
    in
    let cty_super = mk_constraint_type (r, Thas_member has_member_ty) in
    let ity_super = ConstraintType cty_super in
    let (env, ty_sub) = Env.expand_type env ty_sub in

    let subtype_env =
      Subtype_env.possibly_add_violated_constraint
        subtype_env
        ~r_sub:(get_reason ty_sub)
        ~r_super:r
    in
    let fail =
      Subtype_env.fail subtype_env ~ty_sub:(LoclType ty_sub) ~ty_super:ity_super
    in

    match deref ty_sub with
    | (_, Tvar _) ->
      mk_issubtype_prop
        ~sub_supportdyn
        ~is_dynamic_aware:subtype_env.Subtype_env.is_dynamic_aware
        env
        (LoclType ty_sub)
        ity_super
    | (r_sub, Tunion ty_subs) ->
      Common.simplify_union_l
        ~subtype_env
        ~this_ty
        ~mk_prop:simplify
        ~update_reason:
          (Typing_env.update_reason ~f:(fun r_sub_prj ->
               Typing_reason.prj_union_sub ~sub:r_sub ~sub_prj:r_sub_prj))
        (sub_supportdyn, ty_subs)
        rhs
        env
    | (r_inter, Tintersection []) ->
      (* Tintersection [] = mixed *)
      invalid
        env
        ~fail:
          (Some
             Typing_error.(
               primary
               @@ Primary.Top_member
                    {
                      pos = name_pos;
                      name = name_;
                      is_nullable = true;
                      kind =
                        (if is_method then
                          `method_
                        else
                          `property);
                      ctxt = `read;
                      decl_pos = Reason.to_pos r_inter;
                      ty_name = lazy (Typing_print.error env ty_sub);
                      (* Subtyping already gives these reasons *)
                      ty_reasons = lazy [];
                    }))
    | (r_sub, Tintersection ty_subs) ->
      (* A & B <: C iif A <: C | !B *)
      (match Subtype_negation.find_type_with_exact_negation env ty_subs with
      | (env, Some non_ty, tyl) ->
        let ty_sub = MakeType.intersection r_sub tyl in
        let mk_prop = simplify
        and lift_rhs { reason_super; has_member } =
          mk_constraint_type (reason_super, Thas_member has_member)
        and lhs = (sub_supportdyn, ty_sub)
        and rhs_subtype =
          Subtype.
            { super_supportdyn = false; super_like = false; ty_super = non_ty }
        and rhs_destructure =
          { reason_super = r; has_member = has_member_ty }
        in
        let rhs = (r, rhs_subtype, rhs_destructure) in
        Common.simplify_disj_r
          ~subtype_env
          ~this_ty
          ~fail
          ~lift_rhs
          ~mk_prop
          lhs
          rhs
          env
      | _ ->
        typing_obj_get
          ~subtype_env
          ~this_ty
          ~class_id
          ~member_id
          ~explicit_targs
          ~member_ty
          ~has_member:has_member_ty.hm_method
          ty_sub
          env)
    | (r1, Tnewtype (n, tyargs, _)) ->
      let (env, newtype_ty) = Typing_utils.get_newtype_super env r1 n tyargs in
      let sub_supportdyn =
        match sub_supportdyn with
        | None ->
          if String.equal n SN.Classes.cSupportDyn then
            Some r1
          else
            None
        | _ -> sub_supportdyn
      in
      simplify
        ~subtype_env
        ~this_ty
        ~lhs:{ sub_supportdyn; ty_sub = newtype_ty }
        ~rhs:{ reason_super = r; has_member = has_member_ty }
        env
    | (r, Tdynamic _) when Option.is_some explicit_targs ->
      let subtype_env = Subtype_env.set_is_dynamic_aware subtype_env true in
      Subtype.(
        simplify
          ~subtype_env
          ~this_ty
          ~lhs:{ sub_supportdyn = None; ty_sub = MakeType.make_dynamic_tfun r }
          ~rhs:
            {
              super_like = false;
              super_supportdyn = false;
              ty_super = member_ty;
            })
        env
    | ( _,
        ( Toption _ | Tdynamic _ | Tnonnull | Tany _ | Tprim _ | Tfun _
        | Ttuple _ | Tshape _ | Tgeneric _ | Tdependent _ | Tvec_or_dict _
        | Taccess _ | Tclass _ | Tneg _ | Tlabel _ | Tclass_ptr _ ) ) ->
      typing_obj_get
        ~subtype_env
        ~this_ty
        ~class_id
        ~member_id
        ~explicit_targs
        ~member_ty
        ~has_member:has_member_ty.hm_method
        ty_sub
        env
end

and Has_const : sig
  type rhs = {
    reason_super: Reason.t;
    name: string;
    ty: locl_ty;
  }

  include Constraint_handler with type rhs := rhs
end = struct
  type rhs = {
    reason_super: Reason.t;
    name: string;
    ty: locl_ty;
  }

  let rec simplify ~subtype_env ~this_ty ~lhs ~rhs env =
    let { sub_supportdyn = _; ty_sub } = lhs in
    let { reason_super = r; name; ty = member_ty } = rhs in
    let (env, ty_sub) = Env.expand_type env ty_sub in
    let ty_sub = LoclType ty_sub in
    let ty_super =
      ConstraintType
        (mk_constraint_type (r, Thas_const { name; ty = member_ty }))
    in
    if Logging.should_log_subtype_i env ~level:2 ty_sub ty_super then
      Logging.log_subtype_i
        ~this_ty
        ~function_name:"Typing_subtype.Has_const.simplify"
        env
        ty_sub
        ty_super
      @@ fun () -> simplify_ ~subtype_env ~this_ty ~lhs ~rhs env
    else
      simplify_ ~subtype_env ~this_ty ~lhs ~rhs env

  and simplify_
      ~subtype_env
      ~(this_ty : locl_ty option)
      ~lhs:{ sub_supportdyn; ty_sub }
      ~rhs:({ reason_super = r; name; ty = member_ty } as rhs)
      env =
    let cty_super =
      mk_constraint_type (r, Thas_const { name; ty = member_ty })
    in
    let ity_super = ConstraintType cty_super in
    let fail =
      Subtype_env.fail subtype_env ~ty_sub:(LoclType ty_sub) ~ty_super:ity_super
    in
    simplify_has_const
      ~subtype_env
      ~this_ty:(Option.value this_ty ~default:ty_sub)
      ~fail
      ~lhs:{ sub_supportdyn; ty_sub }
      ~rhs
      env

  and simplify_has_const
      ~subtype_env
      ~(this_ty : locl_ty)
      ~fail
      ~lhs:{ sub_supportdyn; ty_sub }
      ~rhs:({ reason_super = r; name; ty = member_ty } as rhs)
      env =
    let (env, ety_sub) = Env.expand_type env ty_sub in
    match deref ety_sub with
    | (_, Tclass ((_, enum_name), _, _)) ->
      let cls = Env.get_class env enum_name in
      (match cls with
      | Decl_entry.Found cls ->
        let maybe_const =
          match Env.get_typeconst env cls name with
          | Some _ -> None
          | None -> Env.get_const env cls name
        in
        (match maybe_const with
        | Some const_def ->
          let dty = const_def.cc_type in
          let ety_env = { empty_expand_env with this_ty } in
          let ((env, _ty_err_opt), lty) =
            if
              Typechecker_options.legacy_experimental_feature_enabled
                (Env.get_tcopt env)
                Typechecker_options.experimental_sound_enum_class_type_const
            then
              Phase.localize env ~ety_env dty
            else
              Phase.localize_no_subst env ~ignore_errors:true dty
          in
          Subtype.(
            simplify
              ~subtype_env
              ~this_ty:None
              ~lhs:{ sub_supportdyn; ty_sub = lty }
              ~rhs:
                {
                  super_like = false;
                  super_supportdyn = false;
                  ty_super = member_ty;
                }
              env)
        | None ->
          let consts =
            Typing_env.consts env cls
            |> List.filter ~f:(fun (name, _) ->
                   not
                     (String.equal name SN.Members.mClass
                     || Cls.has_typeconst cls name))
          in
          let fail =
            let open Typing_error in
            let pos_opt =
              Pos_or_decl.fill_in_filename_if_in_current_decl
                ~current_decl_and_file:(Env.get_current_decl_and_file env)
              @@ Reason.to_pos r
            in
            let (most_similar, quickfixes_opt) =
              match String_utils.most_similar name consts fst with
              | Some (name, const) ->
                let new_text = name in
                ( Some (new_text, const.cc_pos),
                  Option.map pos_opt ~f:(fun pos ->
                      [
                        (let (_, start_column) = Pos.line_column pos in
                         Quickfix.make_eager_default_hint_style
                           ~title:("Change to " ^ Markdown_lite.md_codify name)
                           ~new_text
                         @@ Pos.set_col_start (start_column + 1) pos);
                      ]) )
              | None -> (None, None)
            in
            let subtype_env =
              match quickfixes_opt with
              | None -> subtype_env
              | Some quickfixes ->
                Subtype_env.map_on_error subtype_env ~f:(fun on_error ->
                    Typing_error.Reasons_callback.add_quickfixes
                      on_error
                      quickfixes)
            in
            Subtype_env.fail_with_suffix
              subtype_env
              ~ty_sub:(LoclType ty_sub)
              ~ty_super:
                (ConstraintType
                   (mk_constraint_type (r, Thas_const { name; ty = member_ty })))
            @@ Secondary.Unknown_label
                 { enum_name; decl_pos = Cls.pos cls; most_similar }
          in
          invalid env ~fail)
      | Decl_entry.NotYetAvailable
      | Decl_entry.DoesNotExist ->
        invalid env ~fail)
    | (_, Tvar _) ->
      let cty_super =
        mk_constraint_type (r, Thas_const { name; ty = member_ty })
      in
      let ity_super = ConstraintType cty_super in
      mk_issubtype_prop
        ~sub_supportdyn
        ~is_dynamic_aware:subtype_env.Subtype_env.is_dynamic_aware
        env
        (LoclType ty_sub)
        ity_super
    | (r_sub, Tunion ty_subs) ->
      Common.simplify_union_l
        ~subtype_env
        ~this_ty:None
        ~mk_prop:simplify
        ~update_reason:
          (Typing_env.update_reason ~f:(fun r_sub_prj ->
               Typing_reason.prj_union_sub ~sub:r_sub ~sub_prj:r_sub_prj))
        (sub_supportdyn, ty_subs)
        rhs
        env
    | (r_sub, Tintersection ty_subs) ->
      (* A & B <: C iif A <: C | !B *)
      (match Subtype_negation.find_type_with_exact_negation env ty_subs with
      | (env, Some non_ty, tyl) ->
        let ty_sub = MakeType.intersection r_sub tyl in
        let mk_prop = simplify
        and lift_rhs { reason_super; name; ty } =
          mk_constraint_type (reason_super, Thas_const { name; ty })
        and lhs = (sub_supportdyn, ty_sub)
        and rhs_subtype =
          Subtype.
            { super_supportdyn = false; super_like = false; ty_super = non_ty }
        and rhs_destructure = { reason_super = r; name; ty = member_ty } in
        let rhs = (r, rhs_subtype, rhs_destructure) in
        Common.simplify_disj_r
          ~subtype_env
          ~this_ty:None
          ~fail
          ~lift_rhs
          ~mk_prop
          lhs
          rhs
          env
      | _ ->
        Common.simplify_intersection_l
          ~subtype_env
          ~this_ty:None
          ~fail
          ~mk_prop:simplify
          ~update_reason:
            (Typing_env.update_reason ~f:(fun r_sub_prj ->
                 Typing_reason.prj_inter_sub ~sub:r_sub ~sub_prj:r_sub_prj))
          (sub_supportdyn, ty_subs)
          rhs
          env)
    | (r_generic, Tgeneric generic_nm) ->
      Common.simplify_generic_l
        ~subtype_env
        ~this_ty:(Some this_ty)
        ~fail
        ~mk_prop:simplify
        (sub_supportdyn, r_generic, generic_nm)
        rhs
        rhs
        env
    | (r_newtype, Tnewtype (nm, tyl, _)) ->
      let (env, ty_newtype) =
        Typing_utils.get_newtype_super env r_newtype nm tyl
      in
      Common.simplify_newtype_l
        ~subtype_env
        ~this_ty:(Some this_ty)
        ~mk_prop:simplify
        (sub_supportdyn, r_newtype, nm, ty_newtype)
        rhs
        env
    | (r_dep, Tdependent (dep_ty, ty_inner_sub)) ->
      Common.simplify_dependent_l
        ~subtype_env
        ~this_ty:(Some this_ty)
        ~mk_prop:simplify
        (sub_supportdyn, r_dep, dep_ty, ty_inner_sub)
        rhs
        env
    | ( _,
        ( Toption _ | Tdynamic _ | Tnonnull | Tany _ | Tprim _ | Tfun _
        | Ttuple _ | Tshape _ | Tvec_or_dict _ | Taccess _ | Tneg _ | Tlabel _
          ) ) ->
      invalid ~fail env
    | (_, Tclass_ptr _) ->
      (* TODO(T199606542) Check if this can be used to improve inference for $c::FOO *)
      invalid ~fail env
end

and Type_switch : sig
  type rhs = {
    super_like: bool;
    reason_super: Reason.t;
    predicate: Typing_defs.type_predicate;
    ty_true: locl_ty;
    ty_false: locl_ty;
  }

  include Constraint_handler with type rhs := rhs
end = struct
  type rhs = {
    super_like: bool;
    reason_super: Reason.t;
    predicate: Typing_defs.type_predicate;
    ty_true: locl_ty;
    ty_false: locl_ty;
  }

  let rec simplify ~subtype_env ~this_ty ~lhs ~rhs env =
    let { sub_supportdyn = _; ty_sub } = lhs in
    let { super_like = _; reason_super; predicate; ty_true; ty_false } = rhs in
    let (env, ty_sub) = Env.expand_type env ty_sub in
    let ty_sub = LoclType ty_sub in
    let ty_super =
      ConstraintType
        (mk_constraint_type
           (reason_super, Ttype_switch { predicate; ty_true; ty_false }))
    in
    if Logging.should_log_subtype_i env ~level:2 ty_sub ty_super then
      Logging.log_subtype_i
        ~this_ty
        ~function_name:"Typing_subtype.Type_switch.simplify"
        env
        ty_sub
        ty_super
      @@ fun () -> simplify_ ~subtype_env ~this_ty ~lhs ~rhs env
    else
      simplify_ ~subtype_env ~this_ty ~lhs ~rhs env

  and simplify_
      ~subtype_env
      ~this_ty
      ~lhs:{ ty_sub; sub_supportdyn }
      ~rhs:({ super_like; reason_super; predicate; ty_true; ty_false } as rhs)
      env =
    let cty =
      mk_constraint_type
        (reason_super, Ttype_switch { predicate; ty_true; ty_false })
    in
    let ty_super = ConstraintType cty in
    let (env, ty_sub) = Env.expand_type env ty_sub in
    match get_node ty_sub with
    | Tvar _ ->
      mk_issubtype_prop
        ~sub_supportdyn
        ~is_dynamic_aware:subtype_env.Subtype_env.is_dynamic_aware
        env
        (LoclType ty_sub)
        ty_super
    | Tunion ty_subs ->
      let r_sub = get_reason ty_sub in
      Common.simplify_union_l
        ~subtype_env
        ~this_ty
        ~mk_prop:simplify
        ~update_reason:
          (Typing_env.update_reason ~f:(fun r_sub_prj ->
               Typing_reason.prj_union_sub ~sub:r_sub ~sub_prj:r_sub_prj))
        (sub_supportdyn, ty_subs)
        rhs
        env
    | _ ->
      let (env, partition) =
        Typing_refinement.partition_ty env ty_sub predicate
      in
      let intersect env tyl =
        Typing_intersection.intersect_list env reason_super tyl
      in
      let simplify_subtype ~f tyl ty_super env =
        let (env, ty_sub) = f env tyl in
        Subtype.(
          simplify
            ~subtype_env
            ~this_ty
            ~lhs:{ sub_supportdyn; ty_sub }
            ~rhs:{ super_supportdyn = false; super_like; ty_super }
            env)
      in
      let simplify_split ~init (tys : Typing_refinement.dnf_ty) ty_sup =
        List.fold_left tys ~init ~f:(fun res tyl ->
            res &&& simplify_subtype ~f:intersect tyl ty_sup)
      in
      let left = partition.Typing_refinement.left in
      let span = partition.Typing_refinement.span in
      let right = partition.Typing_refinement.right in
      let (env, props) =
        simplify_split ~init:(env, TL.valid) (left @ span) ty_true
      in
      simplify_split ~init:(env, props) (right @ span) ty_false
end

and Common : sig
  val contains_dynamic_through_intersection : locl_ty -> bool

  val simplify_union_l :
    subtype_env:Subtype_env.t ->
    this_ty:locl_ty option ->
    mk_prop:
      (subtype_env:Subtype_env.t ->
      this_ty:locl_ty option ->
      lhs:lhs ->
      rhs:'rhs ->
      env ->
      env * TL.subtype_prop) ->
    update_reason:(env -> locl_ty -> locl_ty) ->
    Reason.t option * locl_phase ty list ->
    'rhs ->
    env ->
    env * TL.subtype_prop

  val simplify_intersection_l :
    subtype_env:Subtype_env.t ->
    this_ty:locl_ty option ->
    fail:Typing_error.t option ->
    mk_prop:
      (subtype_env:Subtype_env.t ->
      this_ty:locl_ty option ->
      lhs:lhs ->
      rhs:'rhs ->
      env ->
      env * TL.subtype_prop) ->
    update_reason:(env -> locl_ty -> locl_ty) ->
    Reason.t option * locl_phase ty list ->
    'rhs ->
    env ->
    env * TL.subtype_prop

  val simplify_generic_l :
    subtype_env:Subtype_env.t ->
    this_ty:locl_phase ty option ->
    fail:Typing_error.t option ->
    mk_prop:
      (subtype_env:Subtype_env.t ->
      this_ty:locl_phase ty option ->
      lhs:lhs ->
      rhs:'rhs ->
      env ->
      env * TL.subtype_prop) ->
    Reason.t option * locl_phase Reason.t_ * string ->
    'rhs ->
    'rhs ->
    env ->
    env * TL.subtype_prop

  val simplify_newtype_l :
    subtype_env:Subtype_env.t ->
    this_ty:locl_ty option ->
    mk_prop:
      (subtype_env:Subtype_env.t ->
      this_ty:locl_ty option ->
      lhs:lhs ->
      rhs:'rhs ->
      env ->
      'res) ->
    Reason.t option * Reason.t * string * locl_phase ty ->
    'rhs ->
    env ->
    'res

  val simplify_dependent_l :
    subtype_env:Subtype_env.t ->
    this_ty:locl_ty option ->
    mk_prop:
      (subtype_env:Subtype_env.t ->
      this_ty:locl_ty option ->
      lhs:lhs ->
      rhs:'rhs ->
      env ->
      'res) ->
    Reason.t option * Reason.t * dependent_type * locl_phase ty ->
    'rhs ->
    env ->
    'res

  val simplify_disj_r :
    subtype_env:Subtype_env.t ->
    this_ty:locl_ty option ->
    fail:Typing_error.t option ->
    mk_prop:
      (subtype_env:Subtype_env.t ->
      this_ty:locl_ty option ->
      lhs:lhs ->
      rhs:'rhs ->
      env ->
      env * Typing_logic.subtype_prop) ->
    lift_rhs:('rhs -> constraint_type) ->
    Reason.t option * locl_ty ->
    Reason.t * Subtype.rhs * 'rhs ->
    env ->
    env * Typing_logic.subtype_prop

  val dispatch_constraint :
    subtype_env:Subtype_env.t ->
    this_ty:Typing_defs.locl_ty option ->
    sub_supportdyn:Reason.t option ->
    Typing_defs_constraints.internal_type ->
    Typing_defs_constraints.internal_type ->
    Typing_env_types.env ->
    Typing_env_types.env * TL.subtype_prop
end = struct
  (* Helper function which returns true if a type is dynamic or a (nested)
     intersection of types where any type in the intersection is dynamic. Used
     to delay generation disjunctions in the c-union-l case. *)
  let rec contains_dynamic_through_intersection ty =
    Typing_defs.is_dynamic ty
    ||
    match get_node ty with
    | Tintersection tyl ->
      List.exists ~f:contains_dynamic_through_intersection tyl
    | _ -> false

  let simplify_union_l
      ~subtype_env
      ~this_ty
      ~mk_prop
      ~update_reason
      (sub_supportdyn, ty_subs)
      rhs
      env =
    let f res ty_sub =
      let ty_sub = update_reason env ty_sub in
      res &&& mk_prop ~subtype_env ~this_ty ~lhs:{ sub_supportdyn; ty_sub } ~rhs
    in
    (* Prioritize types that aren't dynamic or intersections with dynamic
       to get better error messages *)
    let (last_tyl, first_tyl) =
      Typing_dynamic_utils.partition_union
        ~f:contains_dynamic_through_intersection
        env
        ty_subs
    in
    let init = List.fold_left first_tyl ~init:(env, TL.valid) ~f in
    List.fold_left last_tyl ~init ~f

  (* It's sound to reduce t1 & t2 <: t to (t1 <: t) || (t2 <: t), but
     not complete.
     TODO(T120921930): Don't do this if require_completeness is set. *)

  let simplify_intersection_l
      ~subtype_env
      ~this_ty
      ~fail
      ~mk_prop
      ~update_reason
      (sub_supportdyn, tys_sub)
      rhs
      env =
    (* If we have an intersection containing a type variable e.g. #0 & dict<string,mixed>
       and the type variable has a supportdyn or dynamic upper bound, then it's sound (and desirable)
       to preserve supportdyn when making incomplete simplification of the intersection. In this
       case we add supportdyn to the other conjuncts to preserve the "supports dynamic" property.
    *)
    let rec is_dynamic_or_supportdyn ty =
      let (_, ty) = Env.expand_type env ty in
      match get_node ty with
      | Tnewtype (name_sub, [_], _)
        when String.equal name_sub Naming_special_names.Classes.cSupportDyn ->
        true
      | Tdynamic _
      | Tprim _ ->
        true
      | Tclass ((_, id), _, _) when String.equal SN.Classes.cString id -> true
      | Toption ty -> is_dynamic_or_supportdyn ty
      | Tunion tyl -> List.for_all tyl ~f:is_dynamic_or_supportdyn
      | Tintersection tyl -> List.exists tyl ~f:is_dynamic_or_supportdyn
      | _ -> false
    in
    let add_supportdyn =
      List.exists tys_sub ~f:(fun ty_sub ->
          let (_, ty_sub) = Env.expand_type env ty_sub in
          match get_node ty_sub with
          | Tvar v ->
            let upper_bounds = Env.get_tyvar_upper_bounds env v in
            ITySet.exists
              (fun ity ->
                match ity with
                | LoclType ty -> is_dynamic_or_supportdyn ty
                | _ -> false)
              upper_bounds
          | _ -> false)
    in
    let rec aux ty_subs ~env ~errs ~props =
      match ty_subs with
      | [] ->
        ( env,
          TL.Disj (Typing_error.intersect_opt @@ List.filter_opt errs, props) )
      | ty_sub :: ty_subs ->
        let ty_sub = update_reason env ty_sub in
        let (env, ty_sub) =
          if add_supportdyn then
            TUtils.make_supportdyn (get_reason ty_sub) env ty_sub
          else
            (env, ty_sub)
        in
        let (env, prop) =
          mk_prop ~subtype_env ~this_ty ~lhs:{ sub_supportdyn; ty_sub } ~rhs env
        in
        if TL.is_valid prop then
          (env, prop)
        else (
          match TL.get_error_if_unsat prop with
          | Some err -> aux ty_subs ~env ~errs:(err :: errs) ~props
          | _ -> aux ty_subs ~env ~errs ~props:(prop :: props)
        )
    in
    aux tys_sub ~env ~errs:[fail] ~props:[]

  let simplify_generic_l
      ~subtype_env
      ~this_ty
      ~fail
      ~mk_prop
      (sub_supportdyn, reason_generic, generic_nm)
      rhs
      rhs_for_mixed
      env =
    begin
      let lty_sub = mk (reason_generic, Tgeneric generic_nm) in
      let (env, prop) =
        (* If the generic is actually an expression dependent type,
           we need to update this_ty
        *)
        let this_ty =
          if
            DependentKind.is_generic_dep_ty generic_nm && Option.is_none this_ty
          then
            Some lty_sub
          else
            this_ty
        in

        (* Otherwise, we collect all the upper bounds ("as" constraints) on
           the generic parameter, and check each of these in turn against
           ty_super until one of them succeeds
        *)
        let accumulate prop ~errs ~props =
          if TL.is_valid prop then
            Ok prop
          else
            match TL.get_error_if_unsat prop with
            | Some err -> Error (err :: errs, props)
            | None -> Error (errs, prop :: props)
        in
        let rec aux upper_bounds ~env ~errs ~props =
          match upper_bounds with
          | [] ->
            let err =
              Typing_error.intersect_opt @@ List.filter_opt @@ List.rev errs
            in
            (env, TL.Disj (err, List.rev props))
          | upper_bound :: upper_bounds ->
            let ty_sub =
              Typing_env.update_reason env upper_bound ~f:(fun bound ->
                  Typing_reason.axiom_upper_bound
                    ~bound
                    ~of_:reason_generic
                    ~name:generic_nm)
            in
            let (env, prop) =
              mk_prop
                ~subtype_env
                ~this_ty
                ~lhs:{ sub_supportdyn; ty_sub }
                ~rhs
                env
            in
            (match accumulate prop ~errs ~props with
            | Ok prop -> (env, prop)
            | Error (errs, props) -> aux upper_bounds ~env ~errs ~props)
        in

        let upper_bounds =
          Typing_set.elements (Env.get_upper_bounds env generic_nm)
        in
        (* TODO(mjt) We reverse bounds here to match the evaluation order
           of older code; not doing so triggers errors due to incompleteness *)
        match List.rev upper_bounds with
        | [] ->
          (* Try an implicit mixed = ?nonnull bound before giving up.
             This can be useful when checking T <: t, where type t is
             equivalent to but syntactically different from ?nonnull.
             E.g., if t is a generic type parameter T with nonnull as
             a lower bound.
          *)
          let r = Reason.implicit_upper_bound (get_pos lty_sub, "mixed") in
          let tmixed = MakeType.mixed r in
          let ty_sub =
            Typing_env.update_reason env tmixed ~f:(fun bound ->
                Typing_reason.axiom_upper_bound
                  ~bound
                  ~of_:reason_generic
                  ~name:generic_nm)
          in
          let (env, prop) =
            mk_prop
              ~subtype_env
              ~this_ty
              ~lhs:{ sub_supportdyn; ty_sub }
              ~rhs:rhs_for_mixed
              env
          in
          if TL.is_valid prop then
            (env, prop)
          else
            let (err, props) =
              match TL.get_error_if_unsat prop with
              | Some err ->
                ( Typing_error.intersect_opt @@ List.filter_opt @@ [fail; err],
                  [] )
              | _ -> (fail, [prop])
            in
            (env, TL.Disj (err, props))
        | upper_bounds -> aux upper_bounds ~env ~errs:[fail] ~props:[]
      in
      (env, prop)
    end

  let simplify_newtype_l
      ~subtype_env
      ~this_ty
      ~mk_prop
      (sub_supportdyn, reason_newtype, newtype_nm, newtype_ty)
      rhs
      env =
    let sub_supportdyn =
      match sub_supportdyn with
      | None ->
        if String.equal newtype_nm SN.Classes.cSupportDyn then
          Some reason_newtype
        else
          None
      | _ -> sub_supportdyn
    in
    let ty_sub =
      Typing_env.update_reason env newtype_ty ~f:(fun bound ->
          Typing_reason.axiom_upper_bound
            ~bound
            ~of_:reason_newtype
            ~name:newtype_nm)
    in

    mk_prop ~subtype_env ~this_ty ~lhs:{ sub_supportdyn; ty_sub } ~rhs env

  let simplify_dependent_l
      ~subtype_env
      ~this_ty
      ~mk_prop
      (sub_supportdyn, reason_dep, dep_ty, ty_inner_sub)
      rhs
      env =
    let this_ty =
      Option.first_some
        this_ty
        (Some (mk (reason_dep, Tdependent (dep_ty, ty_inner_sub))))
    in
    let lhs = { sub_supportdyn; ty_sub = ty_inner_sub } in

    mk_prop ~subtype_env ~this_ty ~lhs ~rhs env

  let rec simplify_disj_r
      ~subtype_env
      ~this_ty
      ~fail
      ~mk_prop
      ~lift_rhs
      (sub_supportdyn, ty_sub)
      ((reason_super, rhs_subtype, rhs_other) as rhs)
      env =
    let (env, ty_sub) = Env.expand_type env ty_sub in

    match deref ty_sub with
    | (r, Toption ty) ->
      let ty_null = MakeType.null r in
      let ty =
        Typing_env.update_reason env ty ~f:(fun r_sub_prj ->
            Typing_reason.prj_nullable_sub ~sub:r ~sub_prj:r_sub_prj)
      in
      let prop env =
        simplify_disj_r
          ~subtype_env
          ~this_ty
          ~fail
          ~lift_rhs
          ~mk_prop
          (sub_supportdyn, ty_null)
          rhs
          env
        &&& simplify_disj_r
              ~subtype_env
              ~this_ty
              ~fail
              ~lift_rhs
              ~mk_prop
              (sub_supportdyn, ty)
              rhs
      in
      if_unsat (invalid ~fail) @@ prop env
    | (r_sub, Tintersection ty_subs) ->
      let mk_prop_intersection
          ~subtype_env ~this_ty ~lhs:{ sub_supportdyn; ty_sub } ~rhs env =
        simplify_disj_r
          ~subtype_env
          ~this_ty
          ~fail
          ~lift_rhs
          ~mk_prop
          (sub_supportdyn, ty_sub)
          rhs
          env
      in
      simplify_intersection_l
        ~subtype_env
        ~this_ty
        ~fail
        ~mk_prop:mk_prop_intersection
        ~update_reason:
          (Typing_env.update_reason ~f:(fun r_sub_prj ->
               Typing_reason.prj_inter_sub ~sub:r_sub ~sub_prj:r_sub_prj))
        (sub_supportdyn, ty_subs)
        rhs
        env
    | (r_sub, Tunion ty_subs) ->
      let mk_prop_union
          ~subtype_env ~this_ty ~lhs:{ sub_supportdyn; ty_sub } ~rhs env =
        simplify_disj_r
          ~subtype_env
          ~this_ty
          ~fail
          ~lift_rhs
          ~mk_prop
          (sub_supportdyn, ty_sub)
          rhs
          env
      in
      simplify_union_l
        ~subtype_env
        ~this_ty
        ~mk_prop:mk_prop_union
        ~update_reason:
          (Typing_env.update_reason ~f:(fun r_sub_prj ->
               Typing_reason.prj_union_sub ~sub:r_sub ~sub_prj:r_sub_prj))
        (sub_supportdyn, ty_subs)
        rhs
        env
    | (_, Tvar _) ->
      let (env, ty_fresh) = Env.fresh_type env Pos.none in
      let mk_cstr_prop env =
        mk_prop
          ~subtype_env
          ~this_ty
          ~lhs:{ sub_supportdyn = None; ty_sub = ty_fresh }
          ~rhs:rhs_other
          env
      in
      let mk_subty_prop env =
        Subtype.(
          simplify
            ~subtype_env
            ~this_ty
            ~lhs:{ sub_supportdyn; ty_sub }
            ~rhs:
              {
                super_like = false;
                super_supportdyn = false;
                ty_super =
                  Typing_make_type.union
                    reason_super
                    [rhs_subtype.Subtype.ty_super; ty_fresh];
              }
            env)
      in
      mk_subty_prop env &&& mk_cstr_prop
    | (r_generic, Tgeneric nm) ->
      (match deref rhs_subtype.Subtype.ty_super with
      (* Before using the upper bounds of the generic in a call to
         [simplify_generic_l] we need to check if the super type is a type
         variable and, if so, ensure the two have compatible ranks *)
      | (r_super, Tvar tv_sup)
        when Env.rank_of_tparam env nm > Env.rank_of_tvar env tv_sup ->
        let error =
          Typing_error.Secondary.Higher_rank_tparam_escape
            {
              tvar_pos = Typing_reason.to_pos r_super;
              pos_with_generic = Typing_reason.to_pos r_generic;
              generic_reason = r_generic;
              generic_name = Some nm;
            }
        in
        let fail = Subtype_env.fail_with_secondary_error subtype_env error in
        invalid ~fail env
      | _ ->
        let mk_prop_generic
            ~subtype_env ~this_ty ~lhs:{ sub_supportdyn; ty_sub } ~rhs env =
          simplify_disj_r
            ~subtype_env
            ~this_ty
            ~fail
            ~lift_rhs
            ~mk_prop
            (sub_supportdyn, ty_sub)
            rhs
            env
        in
        simplify_generic_l
          ~subtype_env
          ~this_ty
          ~fail
          ~mk_prop:mk_prop_generic
          (sub_supportdyn, r_generic, nm)
          rhs
          rhs
          env)
    | (r_dep, Tdependent (dep_ty, ty_sub_inner)) ->
      let mk_prop_dependent
          ~subtype_env ~this_ty ~lhs:{ sub_supportdyn; ty_sub } ~rhs env =
        simplify_disj_r
          ~subtype_env
          ~this_ty
          ~fail
          ~lift_rhs
          ~mk_prop
          (sub_supportdyn, ty_sub)
          rhs
          env
      in
      simplify_dependent_l
        ~subtype_env
        ~this_ty
        ~mk_prop:mk_prop_dependent
        (sub_supportdyn, r_dep, dep_ty, ty_sub_inner)
        rhs
        env
    | (r_newtype, Tnewtype (nm, tyl, _)) ->
      let (env, ty_newtype) =
        Typing_utils.get_newtype_super env r_newtype nm tyl
      in
      let mk_prop_newtype
          ~subtype_env ~this_ty ~lhs:{ sub_supportdyn; ty_sub } ~rhs env =
        simplify_disj_r
          ~subtype_env
          ~this_ty
          ~fail
          ~lift_rhs
          ~mk_prop
          (sub_supportdyn, ty_sub)
          rhs
          env
      in
      let ( ||| ) = ( ||| ) ~fail in
      mk_prop
        ~subtype_env
        ~this_ty:None
        ~lhs:{ sub_supportdyn; ty_sub }
        ~rhs:rhs_other
        env
      ||| Subtype.(
            simplify
              ~subtype_env
              ~this_ty:None
              ~lhs:{ sub_supportdyn; ty_sub }
              ~rhs:rhs_subtype)
      ||| simplify_newtype_l
            ~subtype_env
            ~this_ty
            ~mk_prop:mk_prop_newtype
            (sub_supportdyn, r_newtype, nm, ty_newtype)
            rhs
    | ( _,
        ( Tany _ | Tdynamic _ | Tprim _ | Tneg _ | Tnonnull | Tfun _ | Ttuple _
        | Tshape _ | Tvec_or_dict _ | Taccess _ | Tclass _ | Tlabel _
        | Tclass_ptr _ ) ) ->
      let ( ||| ) = ( ||| ) ~fail in
      mk_prop
        ~subtype_env
        ~this_ty:None
        ~lhs:{ sub_supportdyn; ty_sub }
        ~rhs:rhs_other
        env
      ||| Subtype.(
            simplify
              ~subtype_env
              ~this_ty:None
              ~lhs:{ sub_supportdyn; ty_sub }
              ~rhs:rhs_subtype)

  let dispatch_constraint
      ~subtype_env ~this_ty ~sub_supportdyn ity_sub ity_super env =
    (* Dynamic inference: record constraint-type upper bounds on shadow type
       variables so they accumulate structural use-type information.
       Skip Thas_member — it has a dedicated shadow-aware handler in
       typing_object_get.ml that creates a fresh shadow for the member type.
       Recording the raw constraint here would produce a duplicate without
       proper shadow linkage. *)
    let env =
      match ity_sub with
      | LoclType ty_sub ->
        (match get_node ty_sub with
        | Tdynamic (Some shadow_v) ->
          let dominated_by_shadow_handler =
            match ity_super with
            | ConstraintType cty ->
              (match snd (deref_constraint_type cty) with
              | Thas_member _ -> true
              | _ -> false)
            | _ -> false
          in
          if dominated_by_shadow_handler then
            env
          else
            Env.add_tyvar_upper_bound env shadow_v ity_super
        | _ -> env)
      | _ -> env
    in
    match (ity_sub, ity_super) with
    | (LoclType ty_sub, LoclType ty_super) ->
      Subtype.(
        simplify
          ~subtype_env
          ~this_ty
          ~lhs:{ sub_supportdyn; ty_sub }
          ~rhs:{ super_like = false; super_supportdyn = false; ty_super }
          env)
    | (LoclType ty_sub, ConstraintType cstr) ->
      (match deref_constraint_type cstr with
      | (r_super, Tdestructure destructure) ->
        Destructure.(
          simplify
            ~subtype_env
            ~this_ty
            ~lhs:{ sub_supportdyn; ty_sub }
            ~rhs:{ reason_super = r_super; destructure }
            env)
      | (r, Tcan_index can_index) ->
        Can_index.(
          simplify
            ~subtype_env
            ~this_ty
            ~lhs:{ sub_supportdyn; ty_sub }
            ~rhs:{ reason_super = r; can_index }
            env)
      | (r, Tcan_index_assign can_index_assign) ->
        Can_index_assign.(
          simplify
            ~subtype_env
            ~this_ty
            ~lhs:{ sub_supportdyn; ty_sub }
            ~rhs:{ reason_super = r; can_index_assign }
            env)
      | (r, Tcan_traverse can_traverse) ->
        Can_traverse.(
          simplify
            ~subtype_env
            ~this_ty
            ~lhs:{ sub_supportdyn; ty_sub }
            ~rhs:{ reason_super = r; can_traverse }
            env)
      | (r, Thas_member has_member) ->
        Has_member.(
          simplify
            ~subtype_env
            ~this_ty
            ~lhs:{ sub_supportdyn; ty_sub }
            ~rhs:{ reason_super = r; has_member }
            env)
      | (r, Thas_type_member has_type_member) ->
        Has_type_member.(
          simplify
            ~subtype_env
            ~this_ty
            ~lhs:{ sub_supportdyn; ty_sub }
            ~rhs:{ reason_super = r; has_type_member }
            env)
      | (reason_super, Thas_const { name; ty }) ->
        Has_const.(
          simplify
            ~subtype_env
            ~this_ty
            ~lhs:{ sub_supportdyn; ty_sub }
            ~rhs:{ reason_super; name; ty }
            env)
      | (reason_super, Ttype_switch { predicate; ty_true; ty_false }) ->
        Type_switch.(
          simplify
            ~subtype_env
            ~this_ty
            ~lhs:{ sub_supportdyn; ty_sub }
            ~rhs:
              { reason_super; predicate; ty_true; ty_false; super_like = false }
            env))
    | (ConstraintType _, (LoclType _ | ConstraintType _)) ->
      let subtype_env =
        Subtype_env.possibly_add_violated_constraint
          subtype_env
          ~r_sub:(reason ity_sub)
          ~r_super:(reason ity_super)
      in
      let fail =
        Subtype_env.fail subtype_env ~ty_sub:ity_sub ~ty_super:ity_super
      in
      invalid env ~fail
end

(* -- API drivers ----------------------------------------------------------- *)
and Subtype_ask : sig
  val is_sub_type_alt_i :
    require_completeness:bool ->
    no_top_bottom:bool ->
    is_dynamic_aware:bool ->
    sub_supportdyn:Reason.t option ->
    Typing_env_types.env ->
    Typing_defs_constraints.internal_type ->
    Typing_defs_constraints.internal_type ->
    bool option

  val is_sub_type_for_union_i :
    Typing_env_types.env ->
    ?is_dynamic_aware:bool ->
    Typing_defs_constraints.internal_type ->
    Typing_defs_constraints.internal_type ->
    bool

  val is_sub_type_ignore_generic_params_i :
    Typing_env_types.env ->
    Typing_defs_constraints.internal_type ->
    Typing_defs_constraints.internal_type ->
    bool
end = struct
  let is_sub_type_alt_i
      ~require_completeness
      ~no_top_bottom
      ~is_dynamic_aware
      ~sub_supportdyn
      env
      ity_sub
      ity_super =
    let this_ty =
      match ity_sub with
      | LoclType ty1 -> Some ty1
      | ConstraintType _ -> None
    in
    let subtype_env =
      Subtype_env.create
        ~require_completeness
        ~no_top_bottom
        ~is_dynamic_aware
        ~class_sub_classname:(should_cls_sub_cn env)
        ~log_level:3
        None
    in
    (* It is weird that this can cause errors, but I am wary to discard them.
     * Using the generic unify_error to maintain current behavior. *)
    let (_env, prop) =
      Common.dispatch_constraint
        ~subtype_env
        ~this_ty
        ~sub_supportdyn
        ity_sub
        ity_super
        env
    in

    if TL.is_valid prop then
      Some true
    else if TL.is_unsat prop then
      Some false
    else
      None

  let is_sub_type_for_union_i env ?(is_dynamic_aware = false) ty1 ty2 =
    let ( = ) = Option.equal Bool.equal in
    is_sub_type_alt_i
      ~require_completeness:false
      ~no_top_bottom:true
      ~is_dynamic_aware
      ~sub_supportdyn:None
      env
      ty1
      ty2
    = Some true

  let is_sub_type_ignore_generic_params_i env ty1 ty2 =
    let ( = ) = Option.equal Bool.equal in
    is_sub_type_alt_i
    (* TODO(T121047839): Should this set a dedicated ignore_generic_param flag instead? *)
      ~require_completeness:true
      ~no_top_bottom:true
      ~is_dynamic_aware:false
      ~sub_supportdyn:None
      env
      ty1
      ty2
    = Some true
end

and Subtype_simplify : sig
  (** Attempt to compute the intersection of a type with an existing list intersection.
    If try_intersect env t [t1;...;tn] = [u1; ...; um]
    then u1&...&um must be the greatest lower bound of t and t1&...&tn wrt subtyping.
    For example:
      try_intersect nonnull [?C] = [C]
      try_intersect t1 [t2] = [t1]  if t1 <: t2
    Note: it's acceptable to return [t;t1;...;tn] but the intention is that
    we simplify (as above) wherever practical.
    It can be assumed that the original list contains no redundancy.
   *)
  val try_intersect_i :
    ?ignore_tyvars:bool ->
    Typing_env_types.env ->
    Typing_defs_constraints.internal_type ->
    Typing_defs_constraints.internal_type list ->
    Typing_defs_constraints.internal_type list

  val try_intersect :
    ?ignore_tyvars:bool ->
    Typing_env_types.env ->
    Typing_defs.locl_ty ->
    Typing_defs.locl_ty list ->
    Typing_defs.locl_ty list

  (** Attempt to compute the union of a type with an existing list union.
    If try_union env t [t1;...;tn] = [u1;...;um]
    then u1|...|um must be the least upper bound of t and t1|...|tn wrt subtyping.
    For example:
      try_union int [float] = [num]
      try_union t1 [t2] = [t1] if t2 <: t1

    Notes:
    1. It's acceptable to return [t;t1;...;tn] but the intention is that
       we simplify (as above) wherever practical.
    2. Do not use Tunion for a syntactic union - the caller can do that.
    3. It can be assumed that the original list contains no redundancy.
    TODO: there are many more unions to implement yet.
   *)
  val try_union_i :
    Typing_env_types.env ->
    Typing_defs_constraints.internal_type ->
    Typing_defs_constraints.internal_type list ->
    Typing_defs_constraints.internal_type list

  val try_union :
    Typing_env_types.env ->
    Typing_defs.locl_ty ->
    Typing_defs.locl_ty list ->
    Typing_defs.locl_ty list
end = struct
  let rec try_intersect_i ?(ignore_tyvars = false) env ty tyl =
    match tyl with
    | [] -> [ty]
    | ty' :: tyl' ->
      let (env, ty) = Env.expand_internal_type env ty in
      let (env, ty') = Env.expand_internal_type env ty' in
      let default env = ty' :: try_intersect_i env ~ignore_tyvars ty tyl' in
      (* Do not attempt to simplify intersection of type variables, as we use
       * intersection simplification when transitively closing through type variable
       * upper bounds and this would result in a type failing to be added.
       *)
      if ignore_tyvars && (is_tyvar_i ty || is_tyvar_i ty') then
        default env
      else if Subtype_ask.is_sub_type_ignore_generic_params_i env ty ty' then
        try_intersect_i ~ignore_tyvars env ty tyl'
      else if Subtype_ask.is_sub_type_ignore_generic_params_i env ty' ty then
        tyl
      else
        let nonnull_ty = LoclType (MakeType.nonnull (reason ty)) in
        (match (ty, ty') with
        | (LoclType lty, _)
          when Subtype_ask.is_sub_type_ignore_generic_params_i
                 env
                 ty'
                 nonnull_ty -> begin
          match get_node lty with
          | Toption t ->
            try_intersect_i ~ignore_tyvars env (LoclType t) (ty' :: tyl')
          | _ -> default env
        end
        | (_, LoclType lty)
          when Subtype_ask.is_sub_type_ignore_generic_params_i env ty nonnull_ty
          -> begin
          match get_node lty with
          | Toption t ->
            try_intersect_i ~ignore_tyvars env (LoclType t) (ty :: tyl')
          | _ -> default env
        end
        | (_, _) -> default env)

  let try_intersect ?(ignore_tyvars = false) env ty tyl =
    List.map
      (try_intersect_i
         ~ignore_tyvars
         env
         (LoclType ty)
         (List.map tyl ~f:(fun ty -> LoclType ty)))
      ~f:(function
        | LoclType ty -> ty
        | _ ->
          failwith
            "The intersection of two locl type should always be a locl type.")

  let rec try_union_i env ty tyl =
    match tyl with
    | [] -> [ty]
    | ty' :: tyl' ->
      if Subtype_ask.is_sub_type_for_union_i env ty ty' then
        tyl
      else if Subtype_ask.is_sub_type_for_union_i env ty' ty then
        try_union_i env ty tyl'
      else
        let (env, ty) = Env.expand_internal_type env ty in
        let (env, ty') = Env.expand_internal_type env ty' in
        (match (ty, ty') with
        | (LoclType t1, LoclType t2)
          when (is_prim Nast.Tfloat t1 && is_prim Nast.Tint t2)
               || (is_prim Nast.Tint t1 && is_prim Nast.Tfloat t2) ->
          let num = LoclType (MakeType.num (reason ty)) in
          try_union_i env num tyl'
        | (_, _) -> ty' :: try_union_i env ty tyl')

  let try_union env ty tyl =
    List.map
      (try_union_i env (LoclType ty) (List.map tyl ~f:(fun ty -> LoclType ty)))
      ~f:(function
        | LoclType ty -> ty
        | _ ->
          failwith "The union of two locl type should always be a locl type.")
end

and Subtype_trans : sig
  (** Given a subtype proposition, resolve conjunctions of subtype assertions
    of the form #v <: t or t <: #v by adding bounds to #v in env. Close env
    wrt transitivity i.e. if t <: #v and #v <: u then resolve t <: u which
    may in turn produce more bounds in env.
    For disjunctions, arbitrarily pick the first disjunct that is not
    unsatisfiable. If any unsatisfiable disjunct remains, return it.
   *)
  val prop_to_env :
    Typing_defs_constraints.internal_type ->
    Typing_defs_constraints.internal_type ->
    Typing_env_types.env ->
    TL.subtype_prop ->
    Typing_error.Reasons_callback.t option ->
    Typing_env_types.env * Typing_error.t option

  val prop_to_env_against_expected_type :
    Typing_defs_constraints.internal_type ->
    Typing_defs_constraints.internal_type ->
    Typing_env_types.env ->
    TL.subtype_prop ->
    Typing_error.Reasons_callback.t option ->
    Typing_env_types.env * Typing_error.t option * Tvid.Set.t
end = struct
  let add_non_subtype_constraint
      ~is_dynamic_aware
      (env, prop)
      (r_sub, var_sub)
      cty_super
      (on_error : Typing_error.Reasons_callback.t option) =
    let ty_super = ConstraintType cty_super in
    let upper_bounds_before = Env.get_tyvar_upper_bounds env var_sub in
    let env =
      Env.add_tyvar_upper_bound_and_update_variances
        ~intersect:(Subtype_simplify.try_intersect_i ~ignore_tyvars:true env)
        env
        var_sub
        ty_super
    in
    let upper_bounds_after = Env.get_tyvar_upper_bounds env var_sub in
    (* Because of [try_intersect_i], the changed upper bounds may include
       [locl_ty]s *)
    let added_upper_bounds =
      ITySet.diff upper_bounds_after upper_bounds_before
    in
    (* We have the invariant that the lower bound must always be a [locl_ty] *)
    let lower_bounds =
      List.filter_map ~f:(function
          | LoclType ty -> Some ty
          | _ -> failwith "constraint_type in lowerbounds")
      @@ ITySet.elements
      @@ Env.get_tyvar_lower_bounds env var_sub
    in
    let subtype_env =
      Subtype_env.create
        ~is_dynamic_aware
        ~log_level:2
        ~in_transitive_closure:true
        ~class_sub_classname:(should_cls_sub_cn env)
        on_error
    in
    let (env, prop) =
      ITySet.fold
        (fun upper_bound (env, prop) ->
          let (env, ty_err_opt) =
            Typing_subtype_tconst.make_all_type_consts_equal
              env
              var_sub
              upper_bound
              ~on_error
              ~as_tyvar_with_cnstr:true
          in
          let (env, prop) =
            Option.value_map
              ~default:(env, prop)
              ~f:(fun ty_err -> invalid ~fail:(Some ty_err) env)
              ty_err_opt
          in
          List.fold_left
            ~f:(fun (env, prop1) lower_bound ->
              let lower_bound =
                Typing_env.update_reason env lower_bound ~f:(fun bound ->
                    Typing_reason.trans_lower_bound ~bound ~of_:r_sub)
              in
              (* Since we can have either the rhs of a subtype constraint or
                   the rhs of any other constraint in the upper bounds we
                   have to inspect the upper bound and dispatch to the
                   appropriate top-level handler *)
              let (env, prop2) =
                Common.dispatch_constraint
                  ~subtype_env
                  ~this_ty:None
                  ~sub_supportdyn:None
                  (LoclType lower_bound)
                  upper_bound
                  env
              in
              (env, TL.conj prop1 prop2))
            lower_bounds
            ~init:(env, prop))
        added_upper_bounds
        (env, prop)
    in
    (env, prop)

  let rec is_stringish_upper_bound env ty =
    let (env, ty) = Env.expand_type env ty in
    match get_node ty with
    | Tclass ((_, name), _, []) when String.equal name SN.Classes.cString ->
      (env, true)
    | Tprim Nast.Tarraykey -> (env, true)
    | Tclass ((_, name), _, [])
      when String.equal name SN.Classes.cStringish
           || String.equal name SN.Classes.cXHPChild ->
      (env, true)
    | Toption ty -> is_stringish_upper_bound env ty
    | Tunion tyl ->
      List.fold tyl ~init:(env, false) ~f:(fun (env, found) ty ->
          if found then
            (env, true)
          else
            is_stringish_upper_bound env ty)
    | _ -> (env, false)

  let maybe_widen_class_pointer_bound env var ty_sub =
    let (env, widen) =
      ITySet.fold
        (fun upper_bound (env, found) ->
          if found then
            (env, true)
          else
            match upper_bound with
            | LoclType ty -> is_stringish_upper_bound env ty
            | ConstraintType _ -> (env, false))
        (Env.get_tyvar_upper_bounds env var)
        (env, false)
    in
    if widen then
      Typing_class_pointers.coerce_to_name env ty_sub
    else
      (env, ty_sub)

  let rec maybe_widen_lower_bounds_for_stringish
      ~is_dynamic_aware
      (env, prop)
      (r_sup, var)
      ty_super
      (on_error : Typing_error.Reasons_callback.t option) =
    let (env, widen) = is_stringish_upper_bound env ty_super in
    if not widen then
      (env, prop)
    else
      ITySet.fold
        (fun lower_bound (env, prop) ->
          match lower_bound with
          | LoclType ty ->
            let (env, widened_ty) =
              Typing_class_pointers.coerce_to_name env ty
            in
            if Typing_defs.equal_locl_ty ty widened_ty then
              (env, prop)
            else
              add_tyvar_lower_bound_and_close
                ~is_dynamic_aware
                (env, prop)
                (r_sup, var)
                widened_ty
                on_error
          | ConstraintType _ -> (env, prop))
        (Env.get_tyvar_lower_bounds env var)
        (env, prop)

  (* Add a new upper bound ty on var.  Apply transitivity of sutyping,
     * so if we already have tyl <: var, then check that for each ty_sub
     * in tyl we have ty_sub <: ty.
  *)
  and add_tyvar_upper_bound_and_close
      ~is_dynamic_aware
      (env, prop)
      (r_sub, var)
      ty_super
      (on_error : Typing_error.Reasons_callback.t option) =
    let ty_super =
      Sd.transform_dynamic_upper_bound ~is_dynamic_aware env ty_super
    in
    let upper_bounds_before = Env.get_tyvar_upper_bounds env var in
    (* If the type is already in the upper bounds of the type variable,
     * then we already know that this subtype assertion is valid
     *)
    if ITySet.mem (LoclType ty_super) upper_bounds_before then
      valid env
    else
      let (env, prop) =
        maybe_widen_lower_bounds_for_stringish
          ~is_dynamic_aware
          (env, prop)
          (r_sub, var)
          ty_super
          on_error
      in
      let env =
        Env.add_tyvar_upper_bound_and_update_variances
          ~intersect:(Subtype_simplify.try_intersect_i ~ignore_tyvars:true env)
          env
          var
          (LoclType ty_super)
      in
      let upper_bounds_after = Env.get_tyvar_upper_bounds env var in
      let added_upper_bounds =
        List.filter_map ~f:(function
            | LoclType ty -> Some ty
            | _ -> failwith "constraint_type in added upperbounds")
        @@ ITySet.elements
        @@ ITySet.diff upper_bounds_after upper_bounds_before
      in
      let lower_bounds =
        List.filter_map ~f:(function
            | LoclType ty -> Some ty
            | _ -> failwith "constraint_type in lowerbound")
        @@ ITySet.elements
        @@ Env.get_tyvar_lower_bounds env var
      in
      let subtype_env =
        Subtype_env.create
          ~is_dynamic_aware
          ~log_level:2
          ~in_transitive_closure:true
          ~class_sub_classname:(should_cls_sub_cn env)
          on_error
      in
      let (env, prop) =
        List.fold
          ~f:(fun (env, prop) upper_bound ->
            let (env, ty_err_opt) =
              Typing_subtype_tconst.make_all_type_consts_equal
                env
                var
                (LoclType upper_bound)
                ~on_error
                ~as_tyvar_with_cnstr:true
            in
            let (env, prop) =
              Option.value_map
                ~default:(env, prop)
                ~f:(fun ty_err -> invalid ~fail:(Some ty_err) env)
                ty_err_opt
            in
            List.fold_left
              ~f:(fun (env, prop1) lower_bound ->
                let ty_sub =
                  Typing_env.update_reason env lower_bound ~f:(fun bound ->
                      Typing_reason.trans_lower_bound ~bound ~of_:r_sub)
                in
                let (env, prop2) =
                  Subtype.(
                    simplify
                      ~subtype_env
                      ~this_ty:None
                      ~lhs:{ sub_supportdyn = None; ty_sub }
                      ~rhs:
                        {
                          super_like = false;
                          super_supportdyn = false;
                          ty_super = upper_bound;
                        }
                      env)
                in
                (env, TL.conj prop1 prop2))
              lower_bounds
              ~init:(env, prop))
          added_upper_bounds
          ~init:(env, prop)
      in
      (env, prop)

  (* Add a new lower bound ty on var.  Apply transitivity of subtyping
   * (so if var <: ty1,...,tyn then assert ty <: tyi for each tyi), using
   * simplify_subtype to produce a subtype proposition.
   *)
  and add_tyvar_lower_bound_and_close
      ~is_dynamic_aware
      (env, prop)
      (r_sup, var)
      ty_sub
      (on_error : Typing_error.Reasons_callback.t option) =
    let (env, ty_sub) = maybe_widen_class_pointer_bound env var ty_sub in
    let lower_bounds_before = Env.get_tyvar_lower_bounds env var in
    (* If the type is already in the lower bounds of the type variable, then we
       already know that this subtype assertion is valid. *)
    if ITySet.mem (LoclType ty_sub) lower_bounds_before then
      valid env
    else
      let ty_sub =
        Typing_env.update_reason env ty_sub ~f:(fun bound ->
            Typing_reason.trans_lower_bound ~bound ~of_:r_sup)
      in
      let env =
        Env.add_tyvar_lower_bound_and_update_variances
          ~union:(Subtype_simplify.try_union_i env)
          env
          var
          (LoclType ty_sub)
      in
      let lower_bounds_after = Env.get_tyvar_lower_bounds env var in
      (* We have the invariant that lower bounds _must_ be the lhs of a constraint
         and this is always a [LoclType] *)
      let added_lower_bounds =
        List.filter_map ~f:(function
            | LoclType ty -> Some ty
            | _ -> failwith "constraint_type in added lowerbounds")
        @@ ITySet.elements
        @@ ITySet.diff lower_bounds_after lower_bounds_before
      in
      let upper_bounds = Env.get_tyvar_upper_bounds env var in
      let subtype_env =
        Subtype_env.create
          ~is_dynamic_aware
          ~log_level:2
          ~in_transitive_closure:true
          ~class_sub_classname:(should_cls_sub_cn env)
          on_error
      in
      let (env, prop) =
        List.fold
          ~f:(fun (env, prop) lower_bound ->
            let (env, ty_err_opt) =
              Typing_subtype_tconst.make_all_type_consts_equal
                env
                var
                (LoclType lower_bound)
                ~on_error
                ~as_tyvar_with_cnstr:false
            in
            let (env, prop) =
              Option.value_map
                ~default:(env, prop)
                ~f:(fun err -> invalid ~fail:(Some err) env)
                ty_err_opt
            in
            ITySet.fold
              (fun upper_bound (env, prop1) ->
                (* Since we can have either the rhs of a subtype constraint or
                   the rhs of any other constraint in the upper bounds we
                   have to inspect the upper bound and dispatch to the
                   appropriate top-level handler *)
                let (env, prop2) =
                  Common.dispatch_constraint
                    ~subtype_env
                    ~this_ty:None
                    ~sub_supportdyn:None
                    (LoclType lower_bound)
                    upper_bound
                    env
                in
                (env, TL.conj prop1 prop2))
              upper_bounds
              (env, prop))
          added_lower_bounds
          ~init:(env, prop)
      in
      (env, prop)

  (* Traverse a list of disjuncts and remove obviously redundant ones.
       t1 <: #1 is considered redundant if t2 <: #1 is also a disjunct and t2 <: t1.
     Dually,
       #1 <: t1 is considered redundant if #1 <: t2 is also a disjunct and t1 <: t2.
     It does not preserve the ordering.
  *)
  let simplify_disj env disj =
    (* even if sub_ty is not a supertype of super_ty, still consider super_ty redunant *)
    let additional_heuristic ~is_dynamic_aware env _sub_ty super_ty =
      let nonnull = MakeType.supportdyn_nonnull Reason.none in
      Subtype_ask.is_sub_type_for_union_i
        ~is_dynamic_aware
        env
        (LoclType nonnull)
        super_ty
    in
    let rec add_new_bound ~is_lower ~is_dynamic_aware ~constr ty bounds =
      match bounds with
      | [] -> [(is_lower, ty, constr)]
      | ((is_lower', bound_ty, _) as b) :: bounds ->
        if is_lower && is_lower' then
          if
            Subtype_ask.is_sub_type_for_union_i
              ~is_dynamic_aware
              env
              bound_ty
              ty
          then
            b :: bounds
          else if
            Subtype_ask.is_sub_type_for_union_i
              ~is_dynamic_aware
              env
              ty
              bound_ty
          then
            add_new_bound ~is_lower ~is_dynamic_aware ~constr ty bounds
          else if additional_heuristic ~is_dynamic_aware env bound_ty ty then
            b :: bounds
          else if additional_heuristic ~is_dynamic_aware env ty bound_ty then
            add_new_bound ~is_lower ~is_dynamic_aware ~constr ty bounds
          else
            b :: add_new_bound ~is_lower ~is_dynamic_aware ~constr ty bounds
        else if
          (not is_lower)
          && (not is_lower')
          && Subtype_ask.is_sub_type_for_union_i
               ~is_dynamic_aware
               env
               ty
               bound_ty
        then
          b :: bounds
        else if
          (not is_lower)
          && (not is_lower')
          && Subtype_ask.is_sub_type_for_union_i
               ~is_dynamic_aware
               env
               bound_ty
               ty
        then
          add_new_bound ~is_lower ~is_dynamic_aware ~constr ty bounds
        else
          b :: add_new_bound ~is_lower ~is_dynamic_aware ~constr ty bounds
    in
    (* Map a type variable to a list of lower and upper bound types. For any two types
       t1 and t2 both lower or upper in the list, it is not the case that t1 <: t2 or t2 <: t1.
    *)
    let bound_map = ref Tvid.Map.empty in
    let process_bound ~is_lower ~is_dynamic_aware ~constr ty var =
      let ty =
        match ty with
        | LoclType ty when not is_lower ->
          LoclType (Sd.transform_dynamic_upper_bound ~is_dynamic_aware env ty)
        | _ -> ty
      in
      match Tvid.Map.find_opt var !bound_map with
      | None ->
        bound_map := Tvid.Map.add var [(is_lower, ty, constr)] !bound_map
      | Some bounds ->
        let new_bounds =
          add_new_bound ~is_lower ~is_dynamic_aware ~constr ty bounds
        in
        bound_map := Tvid.Map.add var new_bounds !bound_map
    in
    let rec fill_bound_map disj =
      match disj with
      | [] -> []
      | d :: disj ->
        (match d with
        | TL.Conj _
        | TL.AmbiguousShapeSplat _ ->
          d :: fill_bound_map disj
        | TL.Disj (_, props) -> fill_bound_map (props @ disj)
        | TL.IsSubtype (is_dynamic_aware, ty_sub, ty_super) ->
          (match get_tyvar_opt ty_super with
          | Some var_super ->
            process_bound
              ~is_lower:true
              ~is_dynamic_aware
              ~constr:d
              ty_sub
              var_super;
            fill_bound_map disj
          | None ->
            (match get_tyvar_opt ty_sub with
            | Some var_sub ->
              process_bound
                ~is_lower:false
                ~is_dynamic_aware
                ~constr:d
                ty_super
                var_sub;
              fill_bound_map disj
            | None -> d :: fill_bound_map disj)))
    in
    (* Get the constraints from the table that were not removed, and add them to
       the remaining constraints that were not of the form we were looking for. *)
    let rec rebuild_disj remaining to_process =
      match to_process with
      | [] -> remaining
      | (_, bounds) :: to_process ->
        List.map ~f:(fun (_, _, c) -> c) bounds
        @ rebuild_disj remaining to_process
    in
    let remaining = fill_bound_map disj in
    let bounds = Tvid.Map.elements !bound_map in
    rebuild_disj remaining bounds

  let log_non_singleton_disj ty_sub ty_super env msg disj_prop props =
    let rec aux props =
      match props with
      | [] -> ()
      | [TL.Disj (_, props)] -> aux props
      | [_] -> ()
      | _ ->
        Typing_log.log_prop
          1
          (Reason.to_pos (get_reason_i ty_sub))
          ("non-singleton disjunction "
          ^ msg
          ^ " of "
          ^ Typing_print.debug_i env ty_sub
          ^ " <: "
          ^ Typing_print.debug_i env ty_super)
          env
          disj_prop
    in
    aux props

  let partition_ambiguous_shape_splat_vars props =
    let (vars, props) =
      List.fold props ~init:(Tvid.Set.empty, []) ~f:(fun (vars, props) prop ->
          match prop with
          | TL.AmbiguousShapeSplat new_vars ->
            ( List.fold new_vars ~init:vars ~f:(fun vars var ->
                  Tvid.Set.add var vars),
              props )
          | _ -> (vars, prop :: props))
    in
    (vars, List.rev props)

  let rec tell ty_sub ty_super env prop on_error =
    match prop with
    | TL.Conj props ->
      tell_all ty_sub ty_super env ~ty_errs:[] ~remain:[] props on_error
    (* Encodes the invalid proposition with an accompanying error *)
    | TL.Disj (inf_err_opt, []) -> (env, inf_err_opt, [])
    | TL.Disj (inf_err_opt, props) ->
      log_non_singleton_disj
        ty_sub
        ty_super
        env
        "before simplification"
        prop
        props;
      let props = simplify_disj env props in
      log_non_singleton_disj
        ty_sub
        ty_super
        env
        "after simplification"
        prop
        props;
      let ty_errs = Option.to_list inf_err_opt in
      tell_exists
        ty_sub
        ty_super
        env
        ~ty_errs
        ~remain:[]
        ~ambiguous_shape_splat_vars:Tvid.Set.empty
        props
        on_error
    | TL.IsSubtype (coerce, ty_sub, ty_super) ->
      tell_cstr env (coerce, ty_sub, ty_super) on_error
    | TL.AmbiguousShapeSplat _ -> (env, None, [prop])

  and tell_cstr env (is_dynamic_aware, ty_sub, ty_super) on_error =
    let (env, ty_sub) = Env.expand_internal_type env ty_sub in
    let (env, ty_super) = Env.expand_internal_type env ty_super in

    match (ty_sub, ty_super) with
    | (LoclType lty_sub, LoclType lty_super) -> begin
      match (get_node lty_sub, get_node lty_super) with
      (* var-l-r *)
      | (Tvar var_sub, Tvar var_super)
        when Env.rank_of_tvar env var_sub <> Env.rank_of_tvar env var_super ->
        let tvar_pos = Typing_reason.to_pos (get_reason lty_sub) in
        let generic_reason = get_reason lty_super in
        let pos_with_generic = Typing_reason.to_pos generic_reason in
        let generic_name = None in
        let error =
          Typing_error.Secondary.Higher_rank_tparam_escape
            { tvar_pos; pos_with_generic; generic_reason; generic_name }
        in
        let fail =
          Option.map on_error ~f:(fun on_error ->
              Typing_error.apply_reasons ~on_error error)
        in
        (env, fail, [])
      | (Tvar var_sub, Tvar var_super) ->
        let (env, prop1) =
          add_tyvar_upper_bound_and_close
            ~is_dynamic_aware
            (valid env)
            (get_reason lty_sub, var_sub)
            lty_super
            on_error
        in
        let (env, prop2) =
          add_tyvar_lower_bound_and_close
            ~is_dynamic_aware
            (valid env)
            (get_reason lty_super, var_super)
            lty_sub
            on_error
        in
        tell_all
          ty_sub
          ty_super
          env
          ~ty_errs:[]
          ~remain:[]
          [prop1; prop2]
          on_error
      (* var-l *)
      | (Tvar var, _) ->
        let tvar_rank = Env.rank_of_tvar env var in
        (match check_rank env lty_super tvar_rank with
        | Some (generic_reason, generic_name) ->
          let error =
            Typing_error.Secondary.Higher_rank_tparam_escape
              {
                tvar_pos = Typing_reason.to_pos (get_reason lty_sub);
                pos_with_generic = Typing_reason.to_pos (get_reason lty_super);
                generic_reason;
                generic_name;
              }
          in
          let fail =
            Option.map on_error ~f:(fun on_error ->
                Typing_error.apply_reasons ~on_error error)
          in
          (env, fail, [])
        | _ ->
          let (env, prop) =
            add_tyvar_upper_bound_and_close
              ~is_dynamic_aware
              (valid env)
              (get_reason lty_sub, var)
              lty_super
              on_error
          in
          tell ty_sub ty_super env prop on_error)
      | (_, Tvar var) ->
        let tvar_rank = Env.rank_of_tvar env var in
        (match check_rank env lty_sub tvar_rank with
        | Some (generic_reason, generic_name) ->
          let error =
            Typing_error.Secondary.Higher_rank_tparam_escape
              {
                tvar_pos = Typing_reason.to_pos (get_reason lty_super);
                pos_with_generic = Typing_reason.to_pos (get_reason lty_sub);
                generic_reason;
                generic_name;
              }
          in
          let fail =
            Option.map on_error ~f:(fun on_error ->
                Typing_error.apply_reasons ~on_error error)
          in
          (env, fail, [])
        | _ ->
          let (env, prop) =
            add_tyvar_lower_bound_and_close
              ~is_dynamic_aware
              (valid env)
              (get_reason lty_super, var)
              lty_sub
              on_error
          in
          tell ty_sub ty_super env prop on_error)
      | _ -> (env, None, [TL.IsSubtype (is_dynamic_aware, ty_sub, ty_super)])
    end
    (* [constraint_type]s are only valid on the rhs *)
    | (LoclType lty_sub, ConstraintType cstr) -> begin
      match get_node lty_sub with
      | Tvar var ->
        let (env, prop) =
          add_non_subtype_constraint
            ~is_dynamic_aware
            (valid env)
            (get_reason lty_sub, var)
            cstr
            on_error
        in
        tell ty_sub ty_super env prop on_error
      | _ -> (env, None, [TL.IsSubtype (is_dynamic_aware, ty_sub, ty_super)])
    end
    | (ConstraintType _, (LoclType _ | ConstraintType _)) ->
      (env, None, [TL.IsSubtype (is_dynamic_aware, ty_sub, ty_super)])

  and tell_all ty_sub ty_super env ~ty_errs ~remain props on_error =
    match props with
    | [] ->
      let ty_err_opt = Typing_error.multiple_opt @@ List.rev ty_errs in
      (env, ty_err_opt, List.rev remain)
    | prop :: props ->
      let (env, inf_err_opt, prop_remain) =
        tell ty_sub ty_super env prop on_error
      in
      let remain = prop_remain @ remain in
      let ty_errs =
        Option.value_map
          ~default:ty_errs
          ~f:(fun ty_err -> ty_err :: ty_errs)
          inf_err_opt
      in
      tell_all ty_sub ty_super env ~ty_errs ~remain props on_error

  and tell_exists
      ty_sub
      ty_super
      env
      ~ty_errs
      ~remain
      ~ambiguous_shape_splat_vars
      props
      on_error =
    (* For now, just find the first prop in the disjunction that works *)
    match props with
    | [] when Tvid.Set.is_empty ambiguous_shape_splat_vars ->
      let inf_err_opt = Typing_error.intersect_opt ty_errs in
      (env, inf_err_opt, List.rev remain)
    | [] ->
      ( env,
        None,
        TL.AmbiguousShapeSplat (Tvid.Set.elements ambiguous_shape_splat_vars)
        :: List.rev remain )
    | prop :: props ->
      let (prop_env, prop_inf_err, prop_remain) =
        tell ty_sub ty_super env prop on_error
      in
      let (prop_ambiguous_shape_splat_vars, prop_remain) =
        partition_ambiguous_shape_splat_vars prop_remain
      in
      (match prop_inf_err with
      | Some ty_err ->
        let ty_errs = ty_err :: ty_errs and remain = prop_remain @ remain in
        tell_exists
          ty_sub
          ty_super
          env
          ~ty_errs
          ~remain
          ~ambiguous_shape_splat_vars
          props
          on_error
      | None when not (Tvid.Set.is_empty prop_ambiguous_shape_splat_vars) ->
        tell_exists
          ty_sub
          ty_super
          env
          ~ty_errs
          ~remain
          ~ambiguous_shape_splat_vars:
            (Tvid.Set.union
               ambiguous_shape_splat_vars
               prop_ambiguous_shape_splat_vars)
          props
          on_error
      | None -> (prop_env, None, List.rev remain))

  let prop_to_env ty_sub ty_super env prop on_error =
    let (env, ty_err_opt, _props') = tell ty_sub ty_super env prop on_error in
    (env, ty_err_opt)

  let prop_to_env_against_expected_type ty_sub ty_super env prop on_error =
    let (env, ty_err_opt, props) = tell ty_sub ty_super env prop on_error in
    let (ambiguous_shape_splat_vars, _) =
      partition_ambiguous_shape_splat_vars props
    in
    (env, ty_err_opt, ambiguous_shape_splat_vars)
end

and Subtype_tell : sig
  (** Entry point asserting top-level subtype constraints and all implied constraints *)
  val sub_type :
    env ->
    ?is_dynamic_aware:bool ->
    ?is_coeffect:bool ->
    ?ignore_readonly:bool ->
    ?class_sub_classname:bool ->
    locl_ty ->
    locl_ty ->
    Typing_error.Reasons_callback.t option ->
    env * Typing_error.t option

  val sub_type_against_expected_type : TUtils.sub_type_against_expected_type

  val sub_type_or_fail :
    env ->
    locl_ty ->
    locl_ty ->
    Typing_error.Error.t option ->
    env * Typing_error.t option

  val sub_type_i :
    env ->
    ?is_coeffect:bool ->
    ?class_sub_classname:bool ->
    internal_type ->
    internal_type ->
    Typing_error.Reasons_callback.t option ->
    env * Typing_error.t option
end = struct
  let sub_type_inner_with_ambiguity
      (env : env)
      ~(subtype_env : Subtype_env.t)
      ~(sub_supportdyn : Reason.t option)
      ~(this_ty : locl_ty option)
      (ity_sub : internal_type)
      (ity_super : internal_type) : env * Typing_error.t option * Tvid.Set.t =
    let (env, prop) =
      Common.dispatch_constraint
        ~subtype_env
        ~this_ty
        ~sub_supportdyn
        ity_sub
        ity_super
        env
    in
    if not (TL.is_valid prop) then
      Typing_log.log_prop
        1
        (Reason.to_pos (reason ity_sub))
        "sub_type_inner"
        env
        prop;
    Subtype_trans.prop_to_env_against_expected_type
      ity_sub
      ity_super
      env
      prop
      subtype_env.Subtype_env.on_error

  let sub_type_inner_with_ambiguity
      (env : env)
      ~(subtype_env : Subtype_env.t)
      ~(sub_supportdyn : Reason.t option)
      ~(this_ty : locl_ty option)
      (ity_sub : internal_type)
      (ity_super : internal_type) : env * Typing_error.t option * Tvid.Set.t =
    if Logging.should_log_subtype_i env ~level:1 ity_sub ity_super then
      Logging.log_subtype_i
        ~this_ty
        ~function_name:
          ("Typing_subtype.Subtype_tell.sub_type_inner"
          ^
          match subtype_env.Subtype_env.is_dynamic_aware with
          | true -> " (dynamic aware)"
          | false -> "")
        env
        ity_sub
        ity_super
      @@ fun () ->
      sub_type_inner_with_ambiguity
        env
        ~subtype_env
        ~sub_supportdyn
        ~this_ty
        ity_sub
        ity_super
    else
      sub_type_inner_with_ambiguity
        env
        ~subtype_env
        ~sub_supportdyn
        ~this_ty
        ity_sub
        ity_super

  let sub_type_inner env ~subtype_env ~sub_supportdyn ~this_ty ity_sub ity_super
      =
    let (env, ty_err_opt, _) =
      sub_type_inner_with_ambiguity
        env
        ~subtype_env
        ~sub_supportdyn
        ~this_ty
        ity_sub
        ity_super
    in
    (env, ty_err_opt)

  let sub_type_inner_against_expected_type
      env ~subtype_env ~sub_supportdyn ~this_ty ity_sub ity_super =
    sub_type_inner_with_ambiguity
      env
      ~subtype_env
      ~sub_supportdyn
      ~this_ty
      ity_sub
      ity_super

  (* == Tell API ============================================================ *)

  (* -- sub_type_i entry point ---------------------------------------------- *)

  let sub_type_i
      env
      ?(is_coeffect = false)
      ?(class_sub_classname = should_cls_sub_cn env)
      ty_sub
      ty_super
      on_error =
    let subtype_env =
      Subtype_env.create
        ~log_level:2
        ~is_coeffect
        ~is_dynamic_aware:false
        ~class_sub_classname
        on_error
    in
    let old_env = env in
    let (env, ty_err_opt) =
      sub_type_inner
        ~subtype_env
        ~sub_supportdyn:None
        ~this_ty:None
        env
        ty_sub
        ty_super
    in
    let env =
      Env.log_env_change "sub_type" old_env
      @@
      if Option.is_none ty_err_opt then
        env
      else
        old_env
    in
    (env, ty_err_opt)

  (* -- sub_type entry point -------------------------------------------------- *)

  let sub_type
      env
      ?(is_dynamic_aware = false)
      ?(is_coeffect = false)
      ?(ignore_readonly = false)
      ?(class_sub_classname = should_cls_sub_cn env)
      (ty_sub : locl_ty)
      (ty_super : locl_ty)
      on_error =
    let subtype_env =
      Subtype_env.create
        ~log_level:2
        ~is_coeffect
        ~is_dynamic_aware
        ~class_sub_classname
        on_error
    in
    let subtype_env = Subtype_env.{ subtype_env with ignore_readonly } in
    let old_env = env in
    let (env, ty_err_opt) =
      sub_type_inner
        ~subtype_env
        ~sub_supportdyn:None
        env
        ~this_ty:None
        (LoclType ty_sub)
        (LoclType ty_super)
    in
    let env =
      Env.log_env_change "sub_type" old_env
      @@
      if Option.is_none ty_err_opt then
        env
      else
        old_env
    in
    (env, ty_err_opt)

  let sub_type_against_expected_type
      env
      ?(is_dynamic_aware = false)
      ?(ignore_readonly = false)
      (ty_sub : locl_ty)
      (ty_super : locl_ty)
      on_error =
    let subtype_env =
      Subtype_env.create
        ~log_level:2
        ~is_dynamic_aware
        ~report_ambiguous_shape_splat:true
        ~class_sub_classname:(should_cls_sub_cn env)
        on_error
    in
    let subtype_env = Subtype_env.{ subtype_env with ignore_readonly } in
    let old_env = env in
    let (env, ty_err_opt, ambiguous_shape_splat_vars) =
      sub_type_inner_against_expected_type
        ~subtype_env
        ~sub_supportdyn:None
        env
        ~this_ty:None
        (LoclType ty_sub)
        (LoclType ty_super)
    in
    if Option.is_some ty_err_opt then
      let env = Env.log_env_change "sub_type" old_env @@ old_env in
      TUtils.Subtyping_result (env, ty_err_opt)
    else if Tvid.Set.is_empty ambiguous_shape_splat_vars then
      let env = Env.log_env_change "sub_type" old_env env in
      TUtils.Subtyping_result (env, None)
    else
      TUtils.Ambiguous_shape_splat ambiguous_shape_splat_vars

  (* Entry point *)
  let sub_type_or_fail env ty1 ty2 err_opt =
    sub_type env ty1 ty2
    @@ Option.map ~f:Typing_error.Reasons_callback.always err_opt
end

and Instantiate : sig
  val bind_tparams :
    Typing_reason.t ->
    Typing_defs.locl_ty Typing_defs.tparam list ->
    int ->
    Typing_env_types.env ->
    Type_parameter_env.t ->
    Typing_env_types.env * Type_parameter_env.t

  val instantiate_fun_type :
    ?on_error:Typing_error.Reasons_callback.t ->
    Pos.t ->
    Typing_defs.locl_fun_type ->
    int ->
    env:Typing_env_types.env ->
    (Typing_env_types.env * Typing_error.t option) * Typing_defs.locl_fun_type

  val freshen_and_bind_fun_type_quantifiers :
    Typing_reason.t ->
    Typing_defs.locl_fun_type ->
    int ->
    env:Typing_env_types.env ->
    Typing_env_types.env * Typing_defs.locl_fun_type
end = struct
  let freshen_fun_ty fun_ty env rank =
    let (env, ft_tparams_rev, subst) =
      List.fold_left
        fun_ty.ft_tparams
        ~init:(env, [], S_map.empty)
        ~f:(fun (env, acc, subst) ({ tp_name = (pos, old_name); _ } as tparam)
           ->
          let (env, new_name) = Typing_env.fresh_param_name env old_name in
          let tparam = { tparam with tp_name = (pos, new_name) } in
          let ty =
            mk
              ( Typing_reason.polymorphic_type_param
                  (pos, new_name, old_name, rank),
                Tgeneric new_name )
          in
          (env, tparam :: acc, S_map.add old_name ty subst))
    in
    let ft_tparams = List.rev ft_tparams_rev in
    let combine_reasons ~src:_ ~dest = dest in
    ( env,
      Locl_subst.apply_fun { fun_ty with ft_tparams } ~subst ~combine_reasons )

  let bind_tparams r tparams rank env tp_env =
    List.fold_left
      tparams
      ~init:(env, tp_env)
      ~f:(fun
           (env, tp_env)
           {
             tp_name = (def_pos, name);
             tp_constraints;
             tp_reified = reified;
             tp_user_attributes;
             _;
           }
         ->
        let (lower_bounds, upper_bounds) =
          List.fold_left
            tp_constraints
            ~init:(Typing_set.empty, Typing_set.empty)
            ~f:(fun (lower, upper) (kind, ty) ->
              let (is_mixed, is_nothing) =
                Typing_utils.(is_mixed env ty, is_nothing env ty)
              in
              match kind with
              | Ast_defs.Constraint_as when is_mixed -> (lower, upper)
              | Ast_defs.Constraint_as -> (lower, Typing_set.add ty upper)
              | Ast_defs.Constraint_super when is_nothing -> (lower, upper)
              | Ast_defs.Constraint_super -> (Typing_set.add ty lower, upper)
              | Ast_defs.Constraint_eq ->
                (Typing_set.add ty lower, Typing_set.add ty upper))
        in
        let (enforceable, newable, require_dynamic) =
          List.fold_left
            tp_user_attributes
            ~init:(false, false, false)
            ~f:(fun
                 ((enforceable, newable, require_dynamic) as acc)
                 { ua_name = (_, name); _ }
               ->
              if String.equal name SN.UserAttributes.uaEnforceable then
                (true, newable, require_dynamic)
              else if String.equal name SN.UserAttributes.uaNewable then
                (enforceable, true, require_dynamic)
              else if String.equal name SN.UserAttributes.uaRequireDynamic then
                (enforceable, newable, true)
              else
                acc)
        in
        let (env, lower_bounds) =
          let (env, lower_bound) =
            Typing_union.union_list env r @@ Typing_set.elements lower_bounds
          in
          ( env,
            if Typing_utils.is_nothing env lower_bound then
              Typing_set.empty
            else
              Typing_set.singleton lower_bound )
        in
        let (env, upper_bounds) =
          let (env, upper_bound) =
            Typing_intersection.intersect_list env r
            @@ Typing_set.elements upper_bounds
          in
          ( env,
            if Typing_utils.is_mixed env upper_bound then
              Typing_set.empty
            else
              Typing_set.singleton upper_bound )
        in
        let info =
          Typing_kinding_defs.
            {
              lower_bounds;
              upper_bounds;
              reified;
              enforceable;
              newable;
              require_dynamic;
              rank;
            }
        in
        (env, Type_parameter_env.add ~def_pos name info tp_env))

  let freshen_and_bind_fun_type_quantifiers reason fun_ty rank ~env =
    let (env, fun_ty) = freshen_fun_ty fun_ty env rank in
    let fun_ty = { fun_ty with ft_instantiated = true } in
    let env =
      let { ft_tparams; _ } = fun_ty in
      let (env, tpenv) = bind_tparams reason ft_tparams rank env env.tpenv in
      Typing_env_types.{ env with tpenv }
    in
    (env, fun_ty)

  let add_constraint
      ~on_error (env, err_opts) pos (ty_subj, constraint_kind, ty_cstr) =
    (* Prefer the use-site callback (e.g. the subtyping goal that triggered this
       instantiation) so that an unsatisfiable-bounds error is anchored at the
       use site rather than at the decl position of [pos], which may live in a
       different definition and produce an "incomplete position" error. *)
    let callback =
      match on_error with
      | Some _ -> on_error
      | None -> Some (Typing_error.Reasons_callback.unify_error_at pos)
    in
    match constraint_kind with
    | Ast_defs.Constraint_as ->
      let (env, err) = Subtype_tell.sub_type env ty_subj ty_cstr callback in
      (env, err :: err_opts)
    | Ast_defs.Constraint_super ->
      let (env, err) = Subtype_tell.sub_type env ty_cstr ty_subj callback in
      (env, err :: err_opts)
    | Ast_defs.Constraint_eq ->
      let (env, err_cov) = Subtype_tell.sub_type env ty_subj ty_cstr callback in
      let (env, err_contrav) =
        Subtype_tell.sub_type env ty_cstr ty_subj callback
      in
      (env, Option.merge err_cov err_contrav ~f:Typing_error.both :: err_opts)

  (** Instantiate a polymorphic function type with fresh type variables and
    assert subtype constraints from type parameter bounds and where constraints*)
  let instantiate_fun_type ?on_error pos fun_ty rank ~env =
    (* We need to build the substitution before generating constraints because
       type parameters may appear as upper / lower bounds of other type
       parameters, e.g.
       function foo<T1 as T2,T2>(...)
    *)
    let (env, subst) =
      let (env, name_tys) =
        List.fold_left
          fun_ty.ft_tparams
          ~init:(env, [])
          ~f:(fun (env, subst) { tp_name; _ } ->
            let (_, name) = tp_name in
            (* We don't want to solve these type variable so we create them as
               invariant *)
            let (env, ty) = Env.fresh_type_invariant_with_rank env rank pos in
            (env, (name, ty) :: subst))
      in
      (env, S_map.of_list name_tys)
    in
    (* TODO(mjt) add flow for polymorphic instantiation *)
    let combine_reasons ~src ~dest:_ = src in
    let fun_ty = Locl_subst.apply_fun fun_ty ~subst ~combine_reasons in
    (* Accumulate constraints *)
    let (env, err_opts) =
      let acc =
        List.fold_left
          fun_ty.ft_tparams
          ~init:(env, [])
          ~f:(fun (env, err_opts) { tp_name = (_, id); tp_constraints; _ } ->
            (* We know the substituion contains the name so use the unsafe [find] *)
            let ty_subj = S_map.find id subst in
            List.fold_left
              tp_constraints
              ~init:(env, err_opts)
              ~f:(fun acc (cstr_kind, ty_cstr) ->
                add_constraint ~on_error acc pos (ty_subj, cstr_kind, ty_cstr)))
      in
      List.fold_left
        fun_ty.ft_where_constraints
        ~init:acc
        ~f:(fun acc where_cstr -> add_constraint ~on_error acc pos where_cstr)
    in
    let err_opt = Typing_error.multiple_opt @@ List.filter_opt err_opts in
    ((env, err_opt), { fun_ty with ft_instantiated = true })
end

(* == API =================================================================== *)

(* == Tell API ============================================================== *)

include Subtype_tell

(* -- add_constraint(s) entry point ----------------------------------------- *)
let decompose_subtype_add_bound
    ~is_dynamic_aware (env : env) (ty_sub : locl_ty) (ty_super : locl_ty) : env
    =
  let (env, ty_super) = Env.expand_type env ty_super in
  let (env, ty_sub) = Env.expand_type env ty_sub in
  match (get_node ty_sub, get_node ty_super) with
  | (_, Tany _) -> env
  (* name_sub <: ty_super so add an upper bound on name_sub *)
  | (Tgeneric name_sub, _) when not (phys_equal ty_sub ty_super) ->
    let ty_super =
      Sd.transform_dynamic_upper_bound ~is_dynamic_aware env ty_super
    in
    (* TODO(T69551141) handle type arguments. Passing targs to get_lower_bounds,
       but the add_upper_bound call must be adapted *)
    let tys = Env.get_upper_bounds env name_sub in
    (* Don't add the same type twice! *)
    if Typing_set.mem ty_super tys then
      env
    else
      Env.add_upper_bound
        ~intersect:(Subtype_simplify.try_intersect env)
        env
        name_sub
        ty_super
  (* ty_sub <: name_super so add a lower bound on name_super *)
  | (_, Tgeneric name_super) when not (phys_equal ty_sub ty_super) ->
    let tys = Env.get_lower_bounds env name_super in
    (* Don't add the same type twice! *)
    if Typing_set.mem ty_sub tys then
      env
    else
      Env.add_lower_bound
        ~union:(Subtype_simplify.try_union env)
        env
        name_super
        ty_sub
  | (_, _) -> env

let decompose_subtype_add_bound ~is_dynamic_aware env ty_sub ty_super =
  let (env, ty_sub) = Env.expand_type env ty_sub in
  let (env, ty_super) = Env.expand_type env ty_super in
  if Logging.should_log_subtype env ~level:2 ty_sub ty_super then
    Logging.log_subtype
      ~this_ty:None
      ~function_name:"Typing_subtype.decompose_subtype_add_bound"
      env
      ty_sub
      ty_super
    @@ fun () ->
    decompose_subtype_add_bound ~is_dynamic_aware env ty_sub ty_super
  else
    decompose_subtype_add_bound ~is_dynamic_aware env ty_sub ty_super

let rec decompose_subtype_add_prop env prop =
  match prop with
  | TL.Conj props ->
    List.fold_left ~f:decompose_subtype_add_prop ~init:env props
  | TL.Disj (_, []) -> Env.mark_inconsistent env
  | TL.Disj (_, [prop']) -> decompose_subtype_add_prop env prop'
  | TL.Disj _ ->
    let callable_pos = env.genv.callable_pos in
    Typing_log.log_prop
      2
      (Pos_or_decl.of_raw_pos callable_pos)
      "decompose_subtype_add_prop"
      env
      prop;
    env
  | TL.IsSubtype (is_dynamic_aware, LoclType ty1, LoclType ty2) ->
    decompose_subtype_add_bound ~is_dynamic_aware env ty1 ty2
  | TL.IsSubtype _ ->
    (* Subtyping queries between locl types are not creating
       constraint types only if require_soundness is unset.
       Otherwise type refinement subtyping queries may create
       Thas_type_member() constraint types. *)
    failwith
      ("Subtyping locl types in completeness mode should yield "
      ^ "propositions involving locl types only.")
  | TL.AmbiguousShapeSplat _ ->
    failwith
      "Ambiguous shape-splat propositions are not produced in completeness mode"

let decompose_subtype_add_bound_err
    ~is_dynamic_aware (env : env) (ty_sub : locl_ty) (ty_super : locl_ty) =
  let (env, ty_super) = Env.expand_type env ty_super in
  let (env, ty_sub) = Env.expand_type env ty_sub in
  match (get_node ty_sub, get_node ty_super) with
  | (_, Tany _) -> (env, None)
  (* name_sub <: ty_super so add an upper bound on name_sub *)
  | (Tgeneric name_sub, _) when not (phys_equal ty_sub ty_super) ->
    let ty_super =
      Sd.transform_dynamic_upper_bound ~is_dynamic_aware env ty_super
    in
    let tys = Env.get_upper_bounds env name_sub in
    (* Don't add the same type twice! *)
    if Typing_set.mem ty_super tys then
      (env, None)
    else
      ( Env.add_upper_bound
          ~intersect:(Subtype_simplify.try_intersect env)
          env
          name_sub
          ty_super,
        None )
  (* ty_sub <: name_super so add a lower bound on name_super *)
  | (_, Tgeneric name_super) when not (phys_equal ty_sub ty_super) ->
    let tys = Env.get_lower_bounds env name_super in
    (* Don't add the same type twice! *)
    if Typing_set.mem ty_sub tys then
      (env, None)
    else
      ( Env.add_lower_bound
          ~union:(Subtype_simplify.try_union env)
          env
          name_super
          ty_sub,
        None )
  | (_, _) -> (env, None)

let rec decompose_subtype_add_prop_err env prop =
  match prop with
  | TL.Conj props ->
    let (env, errs) =
      List.fold_left
        ~f:(fun (env, errs) prop ->
          let (env, err_opt) = decompose_subtype_add_prop_err env prop in
          let errs =
            Option.value_map err_opt ~default:errs ~f:(fun err -> err :: errs)
          in
          (env, errs))
        ~init:(env, [])
        props
    in
    let err_opt = Typing_error.multiple_opt errs in
    (env, err_opt)
  | TL.Disj (err_opt, []) -> (env, err_opt)
  | TL.Disj (_, [prop']) -> decompose_subtype_add_prop_err env prop'
  | TL.Disj _ ->
    let callable_pos = env.genv.callable_pos in
    Typing_log.log_prop
      2
      (Pos_or_decl.of_raw_pos callable_pos)
      "decompose_subtype_add_prop"
      env
      prop;
    (env, None)
  | TL.IsSubtype (is_dynamic_aware, LoclType ty1, LoclType ty2) ->
    decompose_subtype_add_bound_err ~is_dynamic_aware env ty1 ty2
  | TL.IsSubtype _ ->
    (* Subtyping queries between locl types are not creating
       constraint types only if require_soundness is unset.
       Otherwise type refinement subtyping queries may create
       Thas_type_member() constraint types. *)
    failwith
      ("Subtyping locl types in completeness mode should yield "
      ^ "propositions involving locl types only.")
  | TL.AmbiguousShapeSplat _ ->
    failwith
      "Ambiguous shape-splat propositions are not produced in completeness mode"

(* Given two types that we know are in a subtype relationship
 *   ty_sub <: ty_super
 * add to env.tpenv any bounds on generic type parameters that must
 * hold for ty_sub <: ty_super to be valid.
 *
 * For example, suppose we know Cov<T> <: Cov<D> for a covariant class Cov.
 * Then it must be the case that T <: D so we add an upper bound D to the
 * bounds for T.
 *
 * Although some of this code is similar to that for sub_type_inner, its
 * purpose is different. sub_type_inner takes two types t and u and makes
 * updates to the substitution of type variables (through unification) to
 * make t <: u true.
 *
 * decompose_subtype takes two types t and u for which t <: u is *assumed* to
 * hold, and makes updates to bounds on generic parameters that *necessarily*
 * hold in order for t <: u.
 *)
let decompose_subtype
    (env : env)
    (ty_sub : locl_ty)
    (ty_super : locl_ty)
    (on_error : Typing_error.Reasons_callback.t option) : env =
  let (env, prop) =
    Subtype.(
      simplify
        ~subtype_env:
          (Subtype_env.create
             ~require_soundness:false
             ~require_completeness:true
             ~log_level:2
             ~class_sub_classname:(should_cls_sub_cn env)
             on_error)
        ~this_ty:None
        ~lhs:{ sub_supportdyn = None; ty_sub }
        ~rhs:{ super_like = false; super_supportdyn = false; ty_super }
        env)
  in
  decompose_subtype_add_prop env prop

let decompose_subtype
    (env : env)
    (ty_sub : locl_ty)
    (ty_super : locl_ty)
    (on_error : Typing_error.Reasons_callback.t option) : env =
  let (env, ty_sub) = Env.expand_type env ty_sub in
  let (env, ty_super) = Env.expand_type env ty_super in
  if Logging.should_log_subtype env ~level:2 ty_sub ty_super then
    Logging.log_subtype
      ~this_ty:None
      ~function_name:"Typing_subtype.decompose_subtype"
      env
      ty_sub
      ty_super
    @@ fun () -> decompose_subtype env ty_sub ty_super on_error
  else
    decompose_subtype env ty_sub ty_super on_error

let decompose_subtype_err
    (env : env)
    (ty_sub : locl_ty)
    (ty_super : locl_ty)
    (on_error : Typing_error.Reasons_callback.t option) =
  let (env, prop) =
    Subtype.(
      simplify
        ~subtype_env:
          (Subtype_env.create
             ~require_soundness:false
             ~require_completeness:true
             ~log_level:2
             ~class_sub_classname:(should_cls_sub_cn env)
             on_error)
        ~this_ty:None
        ~lhs:{ sub_supportdyn = None; ty_sub }
        ~rhs:{ super_like = false; super_supportdyn = false; ty_super }
        env)
  in
  decompose_subtype_add_prop_err env prop

(* Decompose a general constraint *)
let decompose_constraint
    (env : env)
    (ck : Ast_defs.constraint_kind)
    (ty_sub : locl_ty)
    (ty_super : locl_ty)
    on_error : env =
  (* constraints are caught based on reason, not error callback. Using unify_error *)
  match ck with
  | Ast_defs.Constraint_as -> decompose_subtype env ty_sub ty_super on_error
  | Ast_defs.Constraint_super -> decompose_subtype env ty_super ty_sub on_error
  | Ast_defs.Constraint_eq ->
    let env = decompose_subtype env ty_sub ty_super on_error in
    decompose_subtype env ty_super ty_sub on_error

let decompose_constraint_err
    (env : env)
    (ck : Ast_defs.constraint_kind)
    (ty_sub : locl_ty)
    (ty_super : locl_ty)
    on_error =
  (* constraints are caught based on reason, not error callback. Using unify_error *)
  match ck with
  | Ast_defs.Constraint_as -> decompose_subtype_err env ty_sub ty_super on_error
  | Ast_defs.Constraint_super ->
    decompose_subtype_err env ty_super ty_sub on_error
  | Ast_defs.Constraint_eq ->
    let (env, err_opt1) = decompose_subtype_err env ty_sub ty_super on_error in
    let (env, err_opt2) = decompose_subtype_err env ty_super ty_sub on_error in
    (env, Typing_error.multiple_opt @@ List.filter_opt [err_opt1; err_opt2])

(* Given a constraint ty1 ck ty2 where ck is AS, SUPER or =,
 * add bounds to type parameters in the environment that necessarily
 * must hold in order for ty1 ck ty2.
 *
 * First, we invoke decompose_constraint to add initial bounds to
 * the environment. Then we iterate, decomposing constraints that
 * arise through transitivity across bounds.
 *
 * For example, suppose that env already contains
 *   C<T1> <: T2
 * for some covariant class C. Now suppose we add the
 * constraint "T2 as C<T3>" i.e. we end up with
 *   C<T1> <: T2 <: C<T3>
 * Then by transitivity we know that T1 <: T3 so we add this to the
 * environment too.
 *
 * We repeat this process until no further bounds are added to the
 * environment, or some limit is reached. (It's possible to construct
 * types that expand forever under inheritance.)
 *)
let constraint_iteration_limit = 20

let add_constraint
    (env : env)
    (ck : Ast_defs.constraint_kind)
    (ty_sub : locl_ty)
    (ty_super : locl_ty)
    on_error : env =
  let oldsize = Env.get_tpenv_size env in
  let env = decompose_constraint env ck ty_sub ty_super on_error in
  let ( = ) = Int.equal in
  if Env.get_tpenv_size env = oldsize then
    env
  else
    let rec iter n env =
      if n > constraint_iteration_limit then
        env
      else
        let oldsize = Env.get_tpenv_size env in
        let env =
          List.fold_left
            (Env.get_generic_parameters env)
            ~init:env
            ~f:(fun env x ->
              List.fold_left
                (Typing_set.elements (Env.get_lower_bounds env x))
                ~init:env
                ~f:(fun env ty_sub' ->
                  List.fold_left
                    (Typing_set.elements (Env.get_upper_bounds env x))
                    ~init:env
                    ~f:(fun env ty_super' ->
                      decompose_subtype env ty_sub' ty_super' on_error)))
        in
        if Int.equal (Env.get_tpenv_size env) oldsize then
          env
        else
          iter (n + 1) env
    in
    iter 0 env

let add_constraint
    (env : env)
    (ck : Ast_defs.constraint_kind)
    (ty_sub : locl_ty)
    (ty_super : locl_ty)
    on_error : env =
  let (env, ty_sub) = Env.expand_type env ty_sub in
  let (env, ty_super) = Env.expand_type env ty_super in
  if Logging.should_log_subtype env ~level:1 ty_sub ty_super then
    Logging.log_subtype
      ~this_ty:None
      ~function_name:"Typing_subtype.add_constraint"
      env
      ty_sub
      ty_super
    @@ fun () -> add_constraint env ck ty_sub ty_super on_error
  else
    add_constraint env ck ty_sub ty_super on_error

let add_constraint_err
    (env : env)
    (ck : Ast_defs.constraint_kind)
    (ty_sub : locl_ty)
    (ty_super : locl_ty)
    on_error =
  let oldsize = Env.get_tpenv_size env in
  let (env, err_opt) =
    decompose_constraint_err env ck ty_sub ty_super on_error
  in
  let ( = ) = Int.equal in
  if Env.get_tpenv_size env = oldsize then
    (env, err_opt)
  else
    let rec iter n (env, err_opts) =
      if n > constraint_iteration_limit then
        (env, Typing_error.multiple_opt @@ List.filter_opt err_opts)
      else
        let oldsize = Env.get_tpenv_size env in
        let (env, err_opts) =
          List.fold_left
            (Env.get_generic_parameters env)
            ~init:(env, err_opts)
            ~f:(fun (env, err_opts) x ->
              List.fold_left
                (Typing_set.elements (Env.get_lower_bounds env x))
                ~init:(env, err_opts)
                ~f:(fun (env, err_opts) ty_sub' ->
                  List.fold_left
                    (Typing_set.elements (Env.get_upper_bounds env x))
                    ~init:(env, err_opts)
                    ~f:(fun (env, err_opts) ty_super' ->
                      let (env, err_opt) =
                        decompose_subtype_err env ty_sub' ty_super' on_error
                      in
                      (env, err_opt :: err_opts))))
        in
        if Int.equal (Env.get_tpenv_size env) oldsize then
          (env, Typing_error.multiple_opt @@ List.filter_opt err_opts)
        else
          iter (n + 2) (env, err_opts)
    in
    iter 0 (env, [err_opt])

let add_constraints p env constraints =
  let add_constraint env (ty1, ck, ty2) =
    add_constraint env ck ty1 ty2
    @@ Some (Typing_error.Reasons_callback.unify_error_at p)
  in
  List.fold_left constraints ~f:add_constraint ~init:env

let add_constraints_err p env constraints =
  let add_constraint env (ty1, ck, ty2) =
    add_constraint_err env ck ty1 ty2
    @@ Some (Typing_error.Reasons_callback.unify_error_at p)
  in
  let (env, err_opts) =
    List.fold_left
      constraints
      ~f:(fun (env, errs) cstr ->
        let (env, err) = add_constraint env cstr in
        (env, err :: errs))
      ~init:(env, [])
  in
  (env, Typing_error.multiple_opt @@ List.filter_opt err_opts)

let apply_where_constraints pos def_pos tparams where_constraints ~env =
  (* First, build a substitution to fresh type parameter names so that we don't
     accidentally pick up bounds which happen to be in the context but aren't
     implied by the constraints
  *)
  let (env, subst_fwd, subst_bwd, old_to_new) =
    let (env, subst, subst_bwd, old_to_new) =
      List.fold_left
        tparams
        ~init:(env, [], [], [])
        ~f:(fun (env, fwds, bwds, nms) { tp_name = (_, orig_nm); _ } ->
          let (env, new_nm) = Typing_env.fresh_param_name env orig_nm in
          let new_ty = mk (Typing_reason.none, Tgeneric new_nm) in
          let old_ty = mk (Typing_reason.none, Tgeneric orig_nm) in
          ( env,
            (orig_nm, new_ty) :: fwds,
            (new_nm, old_ty) :: bwds,
            (orig_nm, new_nm) :: nms ))
    in
    (env, S_map.of_list subst, S_map.of_list subst_bwd, S_map.of_list old_to_new)
  in
  let combine_reasons ~src ~dest:_ = src in
  (* Apply the substitution to the [where] constraints *)
  let where_constraints =
    let apply ty = Locl_subst.apply ty ~subst:subst_fwd ~combine_reasons in
    List.map where_constraints ~f:(fun (ty_l, cstr_kind, ty_r) ->
        (apply ty_l, cstr_kind, apply ty_r))
  in
  (* Bind the fresh type parameters and decompose the constraints so we can
     can capture the implied bounds on the type parameters*)
  let (tpenv, err_opt) =
    let tparams =
      List.map tparams ~f:(fun ({ tp_name = (pos, orig_nm); _ } as tparam) ->
          let new_nm = S_map.find orig_nm old_to_new in
          let tp_name = (pos, new_nm) in
          { tparam with tp_name })
    in
    let { tpenv; _ } = env in
    let (env, tpenv) =
      Instantiate.bind_tparams
        (Reason.witness_from_decl def_pos)
        tparams
        0
        env
        tpenv
    in
    let env = { env with tpenv } in
    (* Decompose the constraints and grab the modified type parameter env *)
    let (env, err_opt) = add_constraints_err pos env where_constraints in
    (* The new bounds will be applied in the [next] continuation since
       [add_constraints] is written for refinement *)
    let m = env.lenv.per_cont_env in
    ( Option.value_map
        ~default:env.tpenv
        ~f:(fun Typing_per_cont_env.{ tpenv; _ } -> tpenv)
      @@ Typing_continuations.Map.find_opt Typing_cont_key.Next m,
      err_opt )
  in
  (* Update the type parameters with the bounds implied by the where constraint
     by mapping back from the fresh type parameter's bounds *)
  let apply ty = Locl_subst.apply ty ~subst:subst_bwd ~combine_reasons in
  let apply_bounds
      tp_constraints Typing_kinding_defs.{ lower_bounds; upper_bounds; _ } =
    Typing_set.fold
      (fun ty cstrs -> (Ast_defs.Constraint_as, apply ty) :: cstrs)
      upper_bounds
    @@ Typing_set.fold
         (fun ty cstrs -> (Ast_defs.Constraint_super, apply ty) :: cstrs)
         lower_bounds
         tp_constraints
  in
  ( List.map
      tparams
      ~f:(fun ({ tp_name = (_, orig_name); tp_constraints; _ } as default) ->
        let new_name = S_map.find orig_name old_to_new in
        let entry_opt = Type_parameter_env.get new_name tpenv in
        Option.value_map entry_opt ~default ~f:(fun entry ->
            let tp_constraints = apply_bounds tp_constraints entry in
            { default with tp_constraints })),
    err_opt )

(* -- subtype_funs entry point ---------------------------------------------- *)
let subtype_funs
    ~(check_return : bool)
    ~for_override
    ~on_error
    (r_sub : Reason.t)
    (ft_sub : locl_fun_type)
    (r_super : Reason.t)
    (ft_super : locl_fun_type)
    env =
  (* This is used for checking subtyping of function types for method override
   * (see Typing_subtype_method) so types are fully-explicit and therefore we
   * permit subtyping to dynamic
   *)
  let old_env = env in
  let class_sub_classname =
    if for_override then
      not
        (Typechecker_options.disable_class_sub_classname_method_override
           (Env.get_tcopt env))
    else
      should_cls_sub_cn env
  in
  let (env, prop) =
    Subtype.simplify_funs
      ~subtype_env:
        (Subtype_env.create
           ~log_level:2
           ~is_dynamic_aware:false
           ~class_sub_classname
           on_error)
      ~check_return
      ~for_override
      ~super_like:false
      (r_sub, ft_sub)
      (r_super, ft_super)
      env
  in
  let (env, ty_err) =
    Subtype_trans.prop_to_env
      (LoclType (mk (r_sub, Tfun ft_sub)))
      (LoclType (mk (r_super, Tfun ft_super)))
      env
      prop
      on_error
  in
  ( (if Option.is_some ty_err then
      old_env
    else
      env),
    ty_err )

(* == Ask API =============================================================== *)

(* -- is_sub_type entry point ----------------------------------------------- *)

let is_sub_type env ty1 ty2 =
  let ( = ) = Option.equal Bool.equal in
  Subtype_ask.is_sub_type_alt_i
    ~require_completeness:false
    ~no_top_bottom:false
    ~is_dynamic_aware:false
    ~sub_supportdyn:None
    env
    (LoclType ty1)
    (LoclType ty2)
  = Some true

let is_maybe_sub_type env ty1 ty2 =
  let ( = ) = Option.equal Bool.equal in
  not
    (Subtype_ask.is_sub_type_alt_i
       ~require_completeness:false
       ~no_top_bottom:false
       ~is_dynamic_aware:false
       ~sub_supportdyn:None
       env
       (LoclType ty1)
       (LoclType ty2)
    = Some false)

(* -- is_dynamic_aware_sub_type entry point --------------------------------- *)
let is_dynamic_aware_sub_type env ty1 ty2 =
  let ( = ) = Option.equal Bool.equal in
  Subtype_ask.is_sub_type_alt_i
    ~require_completeness:false
    ~no_top_bottom:false
    ~is_dynamic_aware:true
    ~sub_supportdyn:None
    env
    (LoclType ty1)
    (LoclType ty2)
  = Some true

(* -- is_sub_type_for_union entry point ------------------------------------- *)
let is_sub_type_for_union_help env ?(is_dynamic_aware = false) ty1 ty2 =
  let ( = ) = Option.equal Bool.equal in
  Subtype_ask.is_sub_type_alt_i
    ~require_completeness:false
    ~no_top_bottom:true
    ~is_dynamic_aware
    ~sub_supportdyn:None
    env
    (LoclType ty1)
    (LoclType ty2)
  = Some true

let is_sub_type_for_union env ty1 ty2 =
  is_sub_type_for_union_help ~is_dynamic_aware:false env ty1 ty2

(* Entry point *)
let is_sub_type_for_union_i env ty1 ty2 =
  Subtype_ask.is_sub_type_for_union_i ~is_dynamic_aware:false env ty1 ty2

(* -- is_sub_type_ignore_generic_params entry point ------------------------- *)
let is_sub_type_ignore_generic_params env ty1 ty2 =
  let ( = ) = Option.equal Bool.equal in
  Subtype_ask.is_sub_type_alt_i
  (* TODO(T121047839): Should this set a dedicated ignore_generic_param flag instead? *)
    ~require_completeness:true
    ~no_top_bottom:true
    ~is_dynamic_aware:false
    ~sub_supportdyn:None
    env
    (LoclType ty1)
    (LoclType ty2)
  = Some true

let is_sub_type_opt_ignore_generic_params env ty1 ty2 =
  Subtype_ask.is_sub_type_alt_i
  (* TODO(T121047839): Should this set a dedicated ignore_generic_param flag instead? *)
    ~require_completeness:true
    ~no_top_bottom:true
    ~is_dynamic_aware:false
    ~sub_supportdyn:None
    env
    (LoclType ty1)
    (LoclType ty2)

(* -- can_sub_type entry point ---------------------------------------------- *)
let can_sub_type env ty1 ty2 =
  let ( <> ) a b = not (Option.equal Bool.equal a b) in
  Subtype_ask.is_sub_type_alt_i
    ~require_completeness:false
    ~no_top_bottom:true
    ~is_dynamic_aware:false
    ~sub_supportdyn:None
    env
    (LoclType ty1)
    (LoclType ty2)
  <> Some false

(* -- is_type_disjoint entry point ------------------------------------------ *)

(* visited record which type variables & generics we've seen, to cut off cycles. *)
let rec is_type_disjoint_help visited env ty1 ty2 =
  let (env, ty1) = Env.expand_type env ty1 in
  let (env, ty2) = Env.expand_type env ty2 in
  match (get_node ty1, get_node ty2) with
  | (_, (Tany _ | Tdynamic _ | Taccess _))
  | ((Tany _ | Tdynamic _ | Taccess _), _) ->
    false
  | (Tshape _, Tshape _) ->
    (* This could be more precise, e.g., if we have two closed shapes with different fields.
       However, intersection already detects this and simplifies to nothing, so it's not
       so important here. *)
    false
  | (Tshape _, _) ->
    (* Treat shapes as dict<arraykey, mixed> because that implementation detail
       leaks through when doing is dict<_, _> on them, and they are also
       Traversable, KeyedContainer, etc. (along with darrays).
       We could translate darray to a more precise dict type with the same
       type arguments, but it doesn't matter since disjointness doesn't ever
       look at them. *)
    let r = get_reason ty1 in
    is_type_disjoint_help
      visited
      env
      MakeType.(dict r (arraykey r) (mixed r))
      ty2
  | (_, Tshape _) ->
    let r = get_reason ty2 in
    is_type_disjoint_help
      visited
      env
      ty1
      MakeType.(dict r (arraykey r) (mixed r))
  | ( Ttuple
        { t_required = tyl1; t_optional = []; t_extra = Tvariadic t_variadic1 },
      Ttuple
        { t_required = tyl2; t_optional = []; t_extra = Tvariadic t_variadic2 }
    )
    when is_nothing t_variadic1 && is_nothing t_variadic2 ->
    (match List.exists2 ~f:(is_type_disjoint_help visited env) tyl1 tyl2 with
    | List.Or_unequal_lengths.Ok res -> res
    | List.Or_unequal_lengths.Unequal_lengths -> true)
  (* TODO optional and variadic components T201398626 T201398652 *)
  | (Ttuple _, _) ->
    (* Treat tuples as vec<mixed> because that implementation detail
       leaks through when doing is vec<_> on them, and they are also
       Traversable, Container, etc. along with varrays.
       We could translate varray to a more precise vec type with the same
       type argument, but it doesn't matter since disjointness doesn't ever
       look at it. *)
    let r = get_reason ty1 in
    is_type_disjoint_help visited env MakeType.(vec r (mixed r)) ty2
  | (_, Ttuple _) ->
    let r = get_reason ty2 in
    is_type_disjoint_help visited env ty1 MakeType.(vec r (mixed r))
  | (Tvec_or_dict (tyk, tyv), _) ->
    let r = get_reason ty1 in
    is_type_disjoint_help
      visited
      env
      MakeType.(union r [vec r tyv; dict r tyk tyv])
      ty2
  | (_, Tvec_or_dict (tyk, tyv)) ->
    let r = get_reason ty2 in
    is_type_disjoint_help
      visited
      env
      ty1
      MakeType.(union r [vec r tyv; dict r tyk tyv])
  | (Tgeneric name, _) -> is_generic_disjoint visited env name ty1 ty2
  | (_, Tgeneric name) -> is_generic_disjoint visited env name ty2 ty1
  | ((Tnewtype _ | Tdependent _ | Tintersection _), _) ->
    let (env, bounds) =
      TUtils.get_concrete_supertypes ~abstract_enum:false env ty1
    in
    is_intersection_type_disjoint visited env bounds ty2
  | (_, (Tnewtype _ | Tdependent _ | Tintersection _)) ->
    let (env, bounds) =
      TUtils.get_concrete_supertypes ~abstract_enum:false env ty2
    in
    is_intersection_type_disjoint visited env bounds ty1
  | (Tvar tv, _) -> is_tyvar_disjoint visited env tv ty2
  | (_, Tvar tv) -> is_tyvar_disjoint visited env tv ty1
  | (Tunion tyl, _) ->
    List.for_all ~f:(is_type_disjoint_help visited env ty2) tyl
  | (_, Tunion tyl) ->
    List.for_all ~f:(is_type_disjoint_help visited env ty1) tyl
  | (Toption ty1, _) ->
    is_type_disjoint_help visited env ty1 ty2
    && is_type_disjoint_help visited env (MakeType.null Reason.none) ty2
  | (_, Toption ty2) ->
    is_type_disjoint_help visited env ty1 ty2
    && is_type_disjoint_help visited env ty1 (MakeType.null Reason.none)
  | (Tnonnull, _) ->
    is_sub_type_for_union_help env ty2 (MakeType.null Reason.none)
  | (_, Tnonnull) ->
    is_sub_type_for_union_help env ty1 (MakeType.null Reason.none)
  | (Tneg (_, IsTag (ClassTag (c1, []))), Tclass ((_, c2), _, _tyl))
  | (Tclass ((_, c2), _, _tyl), Tneg (_, IsTag (ClassTag (c1, [])))) ->
    (* These are disjoint iff for all objects o, o in c2<_tyl> implies that
       o notin (complement (Union tyl'. c1<tyl'>)), which is just that
       c2<_tyl> subset Union tyl'. c1<tyl'>. If c2 is a subclass of c1, then
       whatever _tyl is, we can chase up the hierarchy to find an instantiation
       for tyl'. If c2 is not a subclass of c1, then no matter what the tyl' are
       the subset realtionship cannot hold, since either c1 and c2 are disjoint tags,
       or c1 is a non-equal subclass of c2, and so objects that are exact c2,
       can't inhabit c1. NB, we aren't allowing abstractness of a class to cause
       types to be considered disjoint.
       e.g., in abstract class C {}; class D extends C {}, we wouldn't consider
       neg D and C to be disjoint.
    *)
    TUtils.is_sub_class_refl env c2 c1
  | (Tneg pred1, _) -> Typing_refinement.passes_predicate env ty2 pred1
  | (_, Tneg pred2) -> Typing_refinement.passes_predicate env ty1 pred2
  | (Tprim tp1, Tprim tp2) -> Subtype_negation.is_tprim_disjoint tp1 tp2
  | (Tclass ((_, cname), ex, _), Tprim Aast.Tarraykey)
  | (Tprim Aast.Tarraykey, Tclass ((_, cname), ex, _))
    when String.equal SN.Classes.cString cname
         || (String.equal cname SN.Classes.cStringish && is_nonexact ex) ->
    false
  (* Same as above case, runtime conflates equality of "A" === A::class *)
  | (Tprim Aast.Tarraykey, Tclass_ptr _)
  | (Tclass_ptr _, Tprim Aast.Tarraykey) ->
    false
  | (Tclass ((_, id), _, _), Tclass_ptr _)
  | (Tclass_ptr _, Tclass ((_, id), _, _))
    when String.equal id SN.Classes.cString ->
    false
  | (Tprim _, Tclass_ptr _)
  | (Tclass_ptr _, Tprim _) ->
    true
  (* A function type can be an instance of a Closure *)
  | (Tfun _, Tclass ((_, closure), _, _))
  | (Tclass ((_, closure), _, _), Tfun _)
    when String.equal SN.Classes.cClosure closure ->
    false
  | (Tprim _, (Tfun _ | Tclass _))
  | ((Tfun _ | Tclass _), Tprim _) ->
    true
  | (Tfun _, Tfun _) -> false
  | (Tfun _, Tclass _)
  | (Tclass _, Tfun _) ->
    true
  | (Tclass ((_, c1), ex1, _), Tclass ((_, c2), ex2, _)) ->
    Subtype_negation.is_class_disjoint env c1 ex1 c2 ex2
  | (Tprim _, Tlabel _)
  | (Tlabel _, Tprim _) ->
    true
  | (Tfun _, Tlabel _)
  | (Tlabel _, Tfun _) ->
    true
  | (Tclass _, Tlabel _)
  | (Tlabel _, Tclass _) ->
    true
  | (Tlabel name1, Tlabel name2) -> not @@ String.equal name1 name2
  | (Tclass_ptr _ty, _)
  | (_, Tclass_ptr _ty) ->
    (* TODO(T199606542) Improve disjointnesss check, should be
     * disjoint iff objects of _ty would be disjoint *)
    false

(* incomplete, e.g., is_intersection_type_disjoint (?int & ?float) num *)
and is_intersection_type_disjoint visited_tvyars env inter_tyl ty =
  List.exists ~f:(is_type_disjoint_help visited_tvyars env ty) inter_tyl

and is_intersection_itype_set_disjoint visited_tvyars env inter_ty_set ty =
  ITySet.exists (is_itype_disjoint visited_tvyars env ty) inter_ty_set

and is_itype_disjoint visited_tvyars env (lty1 : locl_ty) (ity : internal_type)
    =
  match ity with
  | LoclType lty2 -> is_type_disjoint_help visited_tvyars env lty1 lty2
  | ConstraintType _ -> false

and is_tyvar_disjoint visited env tyvar ty =
  let (visited_tyvars, visited_generics) = visited in
  if Tvid.Set.mem tyvar visited_tyvars then
    (* There is a cyclic type variable bound, this will lead to a type error *)
    false
  else
    let bounds = Env.get_tyvar_upper_bounds env tyvar in
    is_intersection_itype_set_disjoint
      (Tvid.Set.add tyvar visited_tyvars, visited_generics)
      env
      bounds
      ty

and is_generic_disjoint visited env (name : string) gen_ty ty =
  let (visited_tyvars, visited_generics) = visited in
  if S_set.mem name visited_generics then
    false
  else
    let (env, bounds) =
      TUtils.get_concrete_supertypes ~abstract_enum:false env gen_ty
    in
    is_intersection_type_disjoint
      (visited_tyvars, S_set.add name visited_generics)
      env
      bounds
      ty

let is_type_disjoint env ty1 ty2 =
  is_type_disjoint_help (Tvid.Set.empty, S_set.empty) env ty1 ty2

(* == Polymorphic type instatiation ========================================= *)

(* Expose for use from typing but ensure tyvar ranks are 0 *)
let instantiate_fun_type pos fun_ty ~env =
  Instantiate.instantiate_fun_type pos fun_ty 0 ~env

(* -- Set function references ----------------------------------------------- *)
let set_fun_refs () =
  TUtils.sub_type_ref := sub_type;
  TUtils.sub_type_against_expected_type_ref := sub_type_against_expected_type;
  TUtils.sub_type_i_ref := sub_type_i;
  TUtils.add_constraint_ref := add_constraint;
  TUtils.is_sub_type_ref := is_sub_type;
  TUtils.is_sub_type_for_union_ref := is_sub_type_for_union;
  TUtils.is_sub_type_for_union_i_ref := is_sub_type_for_union_i;
  TUtils.is_sub_type_ignore_generic_params_ref :=
    is_sub_type_ignore_generic_params;
  TUtils.is_sub_type_opt_ignore_generic_params_ref :=
    is_sub_type_opt_ignore_generic_params;
  TUtils.is_type_disjoint_ref := is_type_disjoint;
  TUtils.can_sub_type_ref := can_sub_type

let () = set_fun_refs ()
