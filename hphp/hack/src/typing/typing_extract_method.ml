(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)
open Hh_prelude

let mk_tparam pos name tp_constraints =
  Typing_defs_core.
    {
      tp_variance = Ast_defs.Invariant;
      tp_name = (pos, name);
      tp_constraints;
      tp_reified = Ast_defs.Erased;
      tp_user_attributes = [];
    }

let type_parameter_upper_bounds bounds tparams where_constraints =
  let open Typing_defs_core in
  let bounds =
    List.fold
      tparams
      ~init:bounds
      ~f:(fun bounds { tp_name = (_, name); tp_constraints; _ } ->
        let upper =
          List.filter_map tp_constraints ~f:(function
              | ((Ast_defs.Constraint_as | Ast_defs.Constraint_eq), ty) ->
                Some ty
              | _ -> None)
        in
        S_map.add name upper bounds)
  in
  let add_upper bounds name ty =
    S_map.update
      name
      (fun old -> Some (ty :: Option.value old ~default:[]))
      bounds
  in
  List.fold where_constraints ~init:bounds ~f:(fun bounds (left, kind, right) ->
      let bounds =
        match (get_node left, kind) with
        | (Tgeneric name, (Ast_defs.Constraint_as | Ast_defs.Constraint_eq)) ->
          add_upper bounds name right
        | _ -> bounds
      in
      match (get_node right, kind) with
      | (Tgeneric name, (Ast_defs.Constraint_super | Ast_defs.Constraint_eq)) ->
        add_upper bounds name left
      | _ -> bounds)

module Variance_analysis : sig
  val analyse_this :
    Typing_defs_core.decl_phase Typing_defs_core.ty Typing_defs_core.fun_type ->
    env:Typing_env_types.env ->
    Ast_defs.variance option

  (** Count the occurrences of the bare [this] type inside [ty], treating [ty] as
      a parameter type (i.e. the top-level position is contravariant), from which
      a value of type [this] can be obtained — i.e. occurrences reached
      contravariantly or invariantly. Purely covariant occurrences (a value of
      type [this] cannot be obtained from them) and occurrences of [this] as the
      root of a type constant access (e.g. [this::T]) are not counted. *)
  val count_obtainable_this_in_param :
    follow_bounds:bool ->
    upper_bounds:Typing_defs_core.decl_ty list S_map.t ->
    Typing_defs_core.decl_ty ->
    env:Typing_env_types.env ->
    int

  val analyse_ty_params :
    Typing_defs_core.decl_phase Typing_defs_core.ty Typing_defs_core.fun_type ->
    S_set.t ->
    env:Typing_env_types.env ->
    Ast_defs.variance option S_map.t

  val show_variances : Ast_defs.variance option S_map.t -> string
    [@@warning "-32"]
end = struct
  let show_variances variances =
    let str =
      String.concat
        ~sep:"\n"
        (List.map (S_map.elements variances) ~f:(fun (name, variance_opt) ->
             let var_str =
               Option.value_map
                 variance_opt
                 ~default:"bivariant"
                 ~f:Ast_defs.show_variance
             in
             Printf.sprintf "%s: %s" name var_str))
    in
    Format.sprintf "variances:\n%s\n" str

  let mul v1 v2 =
    let open Ast_defs in
    match (v1, v2) with
    | (Invariant, _)
    | (_, Invariant) ->
      Invariant
    | (Covariant, v)
    | (v, Covariant) ->
      v
    | (Contravariant, Contravariant) -> Covariant

  let join v1 v2 =
    let open Ast_defs in
    match (v1, v2) with
    | (Covariant, Covariant) -> Covariant
    | (Contravariant, Contravariant) -> Contravariant
    | (Invariant, _)
    | (_, Invariant)
    | (Covariant, Contravariant)
    | (Contravariant, Covariant) ->
      Invariant

  let rec find
      (ty : Typing_defs_core.decl_ty) ~var ~update ~is_invariant ~acc ~env =
    let open Typing_defs_core in
    (* Update the accumulator *)
    let acc = update ty acc var in
    (* Traverse the type *)
    match get_node ty with
    | Tthis
    | Tmixed
    | Twildcard
    | Tany _
    | Tnonnull
    | Tdynamic _
    | Tprim _
    | Tgeneric _
    | Taccess _ ->
      acc
    | Toption ty
    | Tlike ty
    | Tclass_ptr ty ->
      find ty ~var ~update ~is_invariant ~acc ~env
    | Tvec_or_dict (ty_k, ty_v) ->
      let acc = find ty_k ~var ~update ~is_invariant ~acc ~env in
      if is_invariant acc then
        acc
      else
        find ty_v ~var ~update ~is_invariant ~acc ~env
    | Tunion tys
    | Tintersection tys ->
      find_list tys ~var ~update ~is_invariant ~acc ~env
    | Trefinement (ty, { cr_consts }) ->
      let acc = find ty ~var ~update ~is_invariant ~acc ~env in
      if is_invariant acc then
        acc
      else
        (* The variance of generics / `this` inside refinements depends on
           the refinement kind:

           - Exact refinements (`= T`) are invariant. This prevents us
             substituting for the upper or lower bounds of generated type
             parameters and correctly handles cases like this:

             abstract class WithRfmt {
               abstract const type T  as arraykey;
               public function foo(this::T $_) : void {}
             }

             Before any optimization, the generated type of
             `meth_caller(WithRfmt::class, 'foo')` is:

             ```
             (function
               <TThis as WithRfmt with type { T = T#0 }
               , T#0 as arraykey
               >(TThis, T#0): void
             )
             ```

             If we don't treat `T#0` as invariant in the exact refinement,
             we would substitute it for its upper bound (`arraykey`) and
             lose the restriction that the second argument must be connected
             to the type constant `T` in the first.

           - `as` (upper bound) refinements are covariant: we compose the
             outer variance with covariant (identity).

           - `super` (lower bound) refinements are contravariant: we compose
             the outer variance with contravariant.

           For `TRloose`, the variance of each bound position is composed
           with the outer context variance using `mul`.
        *)
        find_cr_consts
          (S_map.values cr_consts)
          ~var
          ~update
          ~is_invariant
          ~acc
          ~env
    | Tfun fun_ty -> find_fun_ty fun_ty ~var ~update ~is_invariant ~acc ~env
    | Ttuple tuple_ty ->
      find_tuple_ty tuple_ty ~var ~update ~is_invariant ~acc ~env
    | Tshape (Shape_simple shape_ty) ->
      find_shape_ty shape_ty ~var ~update ~is_invariant ~acc ~env
    | Tshape (Shape_splat { ss_elems }) ->
      find_list ss_elems ~var ~update ~is_invariant ~acc ~env
    | Tapply ((_, class_name), tys) -> begin
      let variances =
        match Typing_env.get_class_or_typedef_tparams env class_name with
        | [] ->
          (* In the case the class / type def doesn't exist or isn't available
             we should be conservative and assume all type parameters are
             invariant *)
          List.map tys ~f:(fun _ -> Ast_defs.Invariant)
        | tparams -> List.map tparams ~f:(fun { tp_variance; _ } -> tp_variance)
      in
      find_with_variances tys variances ~var ~update ~is_invariant ~acc ~env
    end

  and find_cr_consts cr_consts ~var ~update ~is_invariant ~acc ~env =
    match cr_consts with
    | [] -> acc
    | Typing_defs_core.{ rc_bound = TRexact ty; _ } :: cr_consts ->
      (* Exact refinements (= T) are invariant *)
      let acc =
        find ty ~var:Ast_defs.Invariant ~update ~is_invariant ~acc ~env
      in
      if is_invariant acc then
        acc
      else
        find_cr_consts cr_consts ~var ~update ~is_invariant ~acc ~env
    | Typing_defs_core.{ rc_bound = TRloose { tr_upper; tr_lower }; _ }
      :: cr_consts ->
      (* Upper bounds (as T) are covariant: compose with outer variance *)
      let acc = find_list tr_upper ~var ~update ~is_invariant ~acc ~env in
      if is_invariant acc then
        acc
      else
        (* Lower bounds (super T) are contravariant: compose with outer variance *)
        let acc =
          find_list
            tr_lower
            ~var:(mul var Ast_defs.Contravariant)
            ~update
            ~is_invariant
            ~acc
            ~env
        in
        if is_invariant acc then
          acc
        else
          find_cr_consts cr_consts ~var ~update ~is_invariant ~acc ~env

  and find_list tys ~var ~update ~is_invariant ~acc ~env =
    match tys with
    | [] -> acc
    | ty :: tys ->
      let acc = find ty ~var ~update ~is_invariant ~acc ~env in
      if is_invariant acc then
        acc
      else
        find_list tys ~var ~update ~is_invariant ~acc ~env

  and find_with_variances tys variances ~var ~update ~is_invariant ~acc ~env =
    match (tys, variances) with
    | (ty :: tys, tp_variance :: variances) ->
      let acc =
        find ty ~var:(mul var tp_variance) ~is_invariant ~update ~acc ~env
      in
      if is_invariant acc then
        acc
      else
        find_with_variances tys variances ~var ~update ~is_invariant ~acc ~env
    | ([], _)
    | (_, []) ->
      (* Assumes well-formedness *)
      acc

  and find_tuple_ty
      Typing_defs_core.{ t_required; t_optional; t_extra }
      ~var
      ~update
      ~is_invariant
      ~acc
      ~env =
    let acc = find_list t_required ~var ~update ~is_invariant ~acc ~env in
    if is_invariant acc then
      acc
    else begin
      let acc = find_list t_optional ~var ~update ~is_invariant ~acc ~env in
      if is_invariant acc then
        acc
      else begin
        match t_extra with
        | Typing_defs_core.Tsplat ty ->
          find ty ~var ~update ~is_invariant ~acc ~env
        | Typing_defs_core.Tvariadic t_variadic ->
          find t_variadic ~var ~update ~is_invariant ~acc ~env
      end
    end

  and find_shape_ty
      Typing_defs_core.{ s_unknown_value; s_fields; _ }
      ~var
      ~update
      ~is_invariant
      ~acc
      ~env =
    let acc = find s_unknown_value ~var ~update ~is_invariant ~acc ~env in
    if is_invariant acc then
      acc
    else
      let tys =
        List.map
          (Typing_defs_core.TShapeMap.bindings s_fields)
          ~f:(fun (_, Typing_defs_core.{ sft_ty; _ }) -> sft_ty)
      in
      find_list tys ~var ~update ~is_invariant ~acc ~env

  and find_fun_ty
      Typing_defs_core.{ ft_params; ft_ret; ft_tparams; _ }
      ~var
      ~update
      ~is_invariant
      ~acc
      ~env =
    let acc = find ft_ret ~var ~update ~is_invariant ~acc ~env in
    if is_invariant acc then
      acc
    else
      let var = mul var Ast_defs.Contravariant in
      let tys =
        List.map ft_params ~f:(fun Typing_defs_core.{ fp_type; _ } -> fp_type)
      in
      let acc = find_list tys ~var ~update ~is_invariant ~acc ~env in
      if is_invariant acc then
        acc
      else
        (* Type parameters may appearing in upper, lower, or equality bounds
           of other type parameters so we need to consider the polarity of these
           occurences too *)
        let (tys, variances) =
          List.unzip
            (List.concat_map ft_tparams ~f:(fun { tp_constraints; _ } ->
                 List.map tp_constraints ~f:(fun (cstr_kind, ty) ->
                     let variance =
                       match cstr_kind with
                       | Ast_defs.Constraint_as -> Ast_defs.Contravariant
                       | Ast_defs.Constraint_super -> Ast_defs.Covariant
                       | Ast_defs.Constraint_eq -> Ast_defs.Invariant
                     in
                     (ty, variance))))
        in
        let var = Ast_defs.Covariant in
        find_with_variances tys variances ~var ~update ~is_invariant ~acc ~env

  let mentions_this ty =
    let open Typing_defs_core in
    let res = ref false in
    let on_rc_bound rc_bound ~ctx = (ctx, `Continue rc_bound)
    and on_ty ty ~ctx =
      match get_node ty with
      | Tthis ->
        res := true;
        (ctx, `Stop ty)
      | _ -> (ctx, `Continue ty)
    in
    let _ =
      Typing_defs_core.transform_top_down_decl_ty ty ~ctx:() ~on_ty ~on_rc_bound
    in
    !res

  let mentioned_ty_params ty =
    let open Typing_defs_core in
    let res = ref S_set.empty in
    let on_rc_bound rc_bound ~ctx = (ctx, `Continue rc_bound)
    and on_ty ty ~ctx =
      match get_node ty with
      | Tgeneric nm ->
        res := S_set.add nm !res;
        (ctx, `Stop ty)
      | _ -> (ctx, `Continue ty)
    in
    let _ =
      Typing_defs_core.transform_top_down_decl_ty ty ~ctx:() ~on_ty ~on_rc_bound
    in
    !res

  let update acc var =
    Some (Option.value_map acc ~default:var ~f:(fun acc -> join acc var))

  let update_this ty acc var =
    let open Typing_defs_core in
    match get_node ty with
    | Tthis -> update acc var
    | Tmixed
    | Twildcard
    | Tany _
    | Tnonnull
    | Tdynamic _
    | Tprim _
    | Tgeneric _
    | Taccess _
    | Toption _
    | Tlike _
    | Tclass_ptr _
    | Tvec_or_dict _
    | Tunion _
    | Tintersection _
    | Trefinement _
    | Tfun _
    | Ttuple _
    | Tshape (Shape_simple _ | Shape_splat _)
    | Tapply _ ->
      acc

  let is_invariant v =
    Option.value_map v ~default:false ~f:(function
        | Ast_defs.Invariant -> true
        | _ -> false)

  let count_obtainable_this_in_param ~follow_bounds ~upper_bounds ty ~env =
    let open Typing_defs_core in
    let on_ty ty ~ctx:(upper_bounds, seen) =
      match get_node ty with
      | Tgeneric name when not (S_set.mem name seen) ->
        (match S_map.find_opt name upper_bounds with
        | Some (_ :: _ as bounds) ->
          let bounds = List.dedup_and_sort bounds ~compare:compare_decl_ty in
          let ty = Typing_make_type.intersection (get_reason ty) bounds in
          ((upper_bounds, S_set.add name seen), `Restart ty)
        | _ -> ((upper_bounds, seen), `Stop ty))
      | Tgeneric _ -> ((upper_bounds, seen), `Stop ty)
      | Tfun ft ->
        let upper_bounds =
          type_parameter_upper_bounds
            upper_bounds
            ft.ft_tparams
            ft.ft_where_constraints
        in
        let seen =
          List.fold ft.ft_tparams ~init:seen ~f:(fun seen tparam ->
              S_set.remove (snd tparam.tp_name) seen)
        in
        ((upper_bounds, seen), `Continue ty)
      | _ -> ((upper_bounds, seen), `Continue ty)
    in
    let ty =
      if not follow_bounds then
        ty
      else
        transform_top_down_decl_ty
          ty
          ~ctx:(upper_bounds, S_set.empty)
          ~on_ty
          ~on_rc_bound:(fun bound ~ctx -> (ctx, `Continue bound))
    in
    (* [find] already excludes [this::T] (it does not descend into [Taccess])
       and threads variance correctly through containers, function types and
       declared class-level variance, so we just count the [this] nodes reached
       in a position from which a value of type [this] can be *obtained*. We
       disable the [is_invariant] early-exit so that every occurrence is visited.

       An occurrence is "obtainable" when it is reached contravariantly or
       invariantly:
       - contravariant (e.g. [this], [vec<this>], [(function(): this)]): the
         caller supplies, or a callback yields, a [this] value;
       - invariant (e.g. [Box<this>] where [Box<T>] is invariant): the value can
         be read out (e.g. [Box::get]). Invariance pins the container's element
         *type*, but its runtime contents may be any subtype, so two invariant
         occurrences can still yield two values of different runtime classes.
       Purely covariant occurrences (e.g. [(function(this): void)], which only
       *accepts* a [this]) yield nothing and are not counted. *)
    let update ty acc var =
      match get_node ty with
      | Tthis ->
        (match var with
        | Ast_defs.Contravariant
        | Ast_defs.Invariant ->
          acc + 1
        | Ast_defs.Covariant -> acc)
      | _ -> acc
    in
    find
      ty
      ~var:Ast_defs.Contravariant
      ~update
      ~is_invariant:(fun _ -> false)
      ~acc:0
      ~env

  let analyse_this fun_ty ~env =
    let v_opt =
      find_fun_ty
        fun_ty
        ~var:Ast_defs.Covariant
        ~update:update_this
        ~is_invariant
        ~acc:None
        ~env
    in
    match v_opt with
    | Some Ast_defs.Invariant -> Some Ast_defs.Invariant
    | _ ->
      (* The previous traversal handles usage of `this` in parameters, return type,
         type parameter bounds (including class refinements within those bounds).
         However, we also need to prevent the substitution of `this` for its bound if it appears
         in a where constraint *)
      let open Typing_defs_core in
      let { ft_where_constraints; _ } = fun_ty in
      let res =
        List.fold_result
          ft_where_constraints
          ~init:false
          ~f:(fun acc (ty1, _, ty2) ->
            if mentions_this ty1 || mentions_this ty2 then
              Error ()
            else
              Ok acc)
      in
      if Result.is_error res then
        Some Ast_defs.Invariant
      else
        v_opt

  let update_ty_params ty_param_names ty acc var =
    let open Typing_defs_core in
    match get_node ty with
    | Tgeneric name when S_set.mem name ty_param_names ->
      S_map.update
        name
        (function
          | None
          | Some None ->
            Some (Some var)
          | Some (Some v) -> Some (Some (join v var)))
        acc
    | Tthis
    | Tmixed
    | Twildcard
    | Tany _
    | Tnonnull
    | Tdynamic _
    | Tprim _
    | Tgeneric _
    | Taccess _
    | Toption _
    | Tlike _
    | Tclass_ptr _
    | Tvec_or_dict _
    | Tunion _
    | Tintersection _
    | Trefinement _
    | Tfun _
    | Ttuple _
    | Tshape (Shape_simple _ | Shape_splat _)
    | Tapply _ ->
      acc

  let all_invariant vs = S_map.for_all (fun _ v -> is_invariant v) vs

  let analyse_ty_params fun_ty ty_param_names ~env =
    let update = update_ty_params ty_param_names
    and acc =
      S_map.of_list
        (List.map (S_set.elements ty_param_names) ~f:(fun name -> (name, None)))
    in
    let var_opt =
      find_fun_ty
        fun_ty
        ~var:Ast_defs.Covariant
        ~update
        ~is_invariant:all_invariant
        ~acc
        ~env
    and in_where_constraint =
      let open Typing_defs_core in
      List.fold_left
        fun_ty.ft_where_constraints
        ~init:S_set.empty
        ~f:(fun acc (ty1, _, ty2) ->
          S_set.union
            (S_set.union acc (mentioned_ty_params ty1))
            (mentioned_ty_params ty2))
    in
    S_map.mapi
      (fun nm var_opt ->
        if S_set.mem nm in_where_constraint then
          Some Ast_defs.Invariant
        else
          var_opt)
      var_opt
end

(* -- Type constant analysis and substitutions ------------------------------ *)

module Typeconst_analysis : sig
  module Trie : sig
    type t
  end

  type t = {
    this_name: string;
    this_folded_class: Folded_class.t;
    this_trie: Trie.t;
    tries: (Typing_defs_core.decl_ty * Trie.t) S_map.t;
  }

  (** Generate an analysis of a function type which resolves all type constants
      mentioned in the functin type  *)
  val of_fun_type :
    Typing_defs_core.decl_phase Typing_defs_core.ty Typing_defs_core.fun_type ->
    string ->
    Folded_class.t ->
    Typing_env_types.env ->
    t

  (** Pretty print an analysis for debugging *)
  val show : t -> Typing_env_types.env -> string
    [@@warning "-32"]

  module Subst : sig
    module Path : sig
      type t = string list

      module Map : Stdlib.Map.S with type key = t
    end

    type t = {
      this_name: string;
      this_ty: Typing_defs_core.decl_phase Typing_defs_core.ty;
      tparams:
        Typing_defs_core.decl_phase Typing_defs_core.ty Typing_defs_core.tparam
        list;
      this_subst: Typing_defs_core.decl_phase Typing_defs_core.ty Path.Map.t;
      class_subst:
        Typing_defs_core.decl_phase Typing_defs_core.ty Path.Map.t S_map.t;
    }

    val apply :
      t ->
      Typing_defs_core.decl_phase Typing_defs_core.ty ->
      Typing_defs_core.decl_phase Typing_defs_core.ty

    val apply_fun_ty :
      t ->
      Typing_defs_core.decl_phase Typing_defs_core.ty Typing_defs_core.fun_type ->
      Typing_defs_core.decl_phase Typing_defs_core.ty Typing_defs_core.fun_type
  end

  val to_subst :
    t ->
    Typing_defs_core.decl_phase Typing_defs_core.ty ->
    Typing_env_types.env ->
    Typing_env_types.env * Subst.t
end = struct
  (** Find the intersection of a type constant definition; this is used to find
      the implied bounds on nested type constants when the outer type constant
      has upper and lower bounds but here we make no assumptions about
      well-formedness of the bounds i.e. we don't assumed LB <: UB.
      There are three cases to consider
      - Both type constants are abstract; here we take the least upper bound
      and greatest lower bound
      - Both type constants are concrete; here we take the intersection of the
      constants
      - One is concrete and the other abstract; here we treat the concrete
      constants as an abstract constant with equal upper and lower bound

      When the bounds of the type constants _are_ subtypes, this is equivalent
      to using the definition in the lower bound. We don't do that here since
      lifting the method definition is just and elaboration step.
  *)
  let meet_typeconst reason tc1 tc2 =
    let open Typing_defs in
    match (tc1, tc2) with
    | (TCConcrete { tc_type = ty1 }, TCConcrete { tc_type = ty2 }) ->
      let tc_type = Typing_make_type.intersection reason [ty1; ty2] in
      TCConcrete { tc_type }
    | ( TCAbstract
          {
            atc_as_constraint = ub1;
            atc_super_constraint = lb1;
            atc_default = dflt1;
          },
        TCAbstract
          {
            atc_as_constraint = ub2;
            atc_super_constraint = lb2;
            atc_default = dflt2;
          } ) ->
      let atc_as_constraint =
        Option.merge ub1 ub2 ~f:(fun ty1 ty2 ->
            Typing_make_type.intersection reason [ty1; ty2])
      and atc_super_constraint =
        Option.merge lb1 lb2 ~f:(fun ty1 ty2 ->
            Typing_make_type.union reason [ty1; ty2])
      and atc_default =
        Option.merge dflt1 dflt2 ~f:(fun ty1 ty2 ->
            Typing_make_type.intersection reason [ty1; ty2])
      in
      TCAbstract { atc_as_constraint; atc_super_constraint; atc_default }
    | ( TCAbstract { atc_as_constraint; atc_super_constraint; _ },
        TCConcrete { tc_type } )
    | ( TCConcrete { tc_type },
        TCAbstract { atc_as_constraint; atc_super_constraint; _ } ) ->
      let tc_type =
        match (atc_as_constraint, atc_super_constraint) with
        | (None, None) -> tc_type
        | (Some ub, None) -> Typing_make_type.intersection reason [tc_type; ub]
        | (None, Some lb) -> Typing_make_type.union reason [tc_type; lb]
        | (Some ub, Some lb) ->
          Typing_make_type.union
            reason
            [lb; Typing_make_type.intersection reason [tc_type; ub]]
      in
      TCConcrete { tc_type }

  (** Helper to accumulate the path of projections on a given root type *)
  let rec access_path root prefix =
    let open Typing_defs_core in
    match get_node root with
    | Taccess (root, ty_const) -> access_path root (ty_const :: prefix)
    | _ -> (root, prefix)

  (** Helper to lift transformation on [decl_ty] into transformations on a
      [typeconst]  *)
  let transform_typeconst typeconst ~f =
    let open Typing_defs in
    match typeconst with
    | TCConcrete { tc_type } ->
      let tc_type = f tc_type in
      TCConcrete { tc_type }
    | TCAbstract { atc_as_constraint; atc_super_constraint; atc_default } ->
      let atc_as_constraint = Option.map ~f atc_as_constraint
      and atc_super_constraint = Option.map ~f atc_super_constraint
      and atc_default = Option.map ~f atc_default in
      TCAbstract { atc_as_constraint; atc_super_constraint; atc_default }

  module Trie = struct
    type base =
      | Root
      | Typeconst of {
          typeconst: Typing_defs.typeconst;
          pos: Pos_or_decl.t;
          origin: string option;
        }

    let transform_base base ~f =
      match base with
      | Root -> Root
      | Typeconst { typeconst; pos; origin } ->
        let typeconst = transform_typeconst typeconst ~f in
        Typeconst { typeconst; pos; origin }

    type t = {
      base: base;
      children: status S_map.t;
    }

    and status =
      | Undefined
      | Defined of t

    let rec merge_help s1 s2 =
      match (s1, s2) with
      | (Undefined, s)
      | (s, Undefined) ->
        Some s
      | (Defined t1, Defined t2) -> Some (Defined (merge t1 t2))

    and merge t1 t2 =
      let children =
        S_map.union
          ~combine:(fun _key s1 s2 -> merge_help s1 s2)
          t1.children
          t2.children
      in
      { t1 with children }

    let rec transform { base; children } ~f =
      let base = transform_base base ~f
      and children = S_map.map (transform_status ~f) children in
      { base; children }

    and transform_status status ~f =
      match status with
      | Undefined -> Undefined
      | Defined t -> Defined (transform t ~f)
    (* -- Pretty printing ----------------------------------------------------- *)

    let rec show_help t ~indent ~acc ~env =
      List.fold_left (S_map.bindings t) ~init:acc ~f:(fun acc (nm, status) ->
          show_status (nm, status) ~indent ~acc ~env)

    and show_status (nm, status) ~indent ~acc ~env =
      match status with
      | Undefined ->
        let padding = String.make indent ' ' in
        Format.sprintf "%s%s == undefined\n" padding nm :: acc
      | Defined { base = Typeconst { typeconst; _ }; children; _ } ->
        let acc = show_typeconst (nm, typeconst) ~indent ~acc ~env in
        let indent = indent + 4 in
        show_help children ~indent ~acc ~env
      | Defined { base = Root; children; _ } ->
        let padding = String.make indent ' ' in
        let acc = Format.sprintf "%sthis\n" padding :: acc in
        let indent = indent + 4 in
        show_help children ~indent ~acc ~env

    and show_typeconst (nm, typeconst) ~indent ~acc ~env =
      let padding = String.make indent ' ' in
      let data =
        let open Typing_defs in
        match typeconst with
        | TCConcrete { tc_type } ->
          Format.sprintf
            "%s%s == %s\n"
            padding
            nm
            (Typing_print.debug_decl env tc_type)
        | TCAbstract { atc_as_constraint; atc_super_constraint; _ } ->
          Format.sprintf
            "%s%s as %s super %s\n"
            padding
            nm
            (Option.value_map
               atc_as_constraint
               ~default:"(none)"
               ~f:(Typing_print.debug_decl env))
            (Option.value_map
               atc_super_constraint
               ~default:"(none)"
               ~f:(Typing_print.debug_decl env))
      in
      data :: acc

    let show (name, t) env =
      String.concat ~sep:""
      @@ List.rev
      @@ show_help t.children ~indent:4 ~acc:[Format.sprintf "%s\n" name] ~env

    (* -- Helpers ------------------------------------------------------------  *)
    let empty = { base = Root; children = S_map.empty }

    let constants t = S_map.keys t.children

    let update t const_name child =
      let { children; _ } = t in
      let children = S_map.add const_name child children in
      { t with children }

    let of_typeconst ?origin typeconst ~const_pos =
      let base = Typeconst { typeconst; pos = const_pos; origin } in
      { base; children = S_map.empty }

    let status_of_typeconst ?origin typeconst ~const_pos =
      let t = of_typeconst typeconst ?origin ~const_pos in
      Defined t

    let rec find_path t path =
      match path with
      | [] -> Some (Defined t)
      | (_, next) :: rest -> begin
        match S_map.find_opt next t.children with
        | None -> None
        | Some Undefined -> Some Undefined
        | Some (Defined t) -> find_path t rest
      end
  end

  (** An analysis is a set of of type constant tries one of which is considered
      to be [this] *)
  type t = {
    this_name: string;
    this_folded_class: Folded_class.t;
    this_trie: Trie.t;
    tries: (Typing_defs_core.decl_ty * Trie.t) S_map.t;
  }

  let create this_name this_folded_class =
    {
      this_name;
      this_folded_class;
      this_trie = Trie.empty;
      tries = S_map.empty;
    }

  type root = {
    root_name: string;
    root_folded_class: Folded_class.t;
    root_trie: Trie.t;
  }

  type state = {
    root: root;
    analysis: t;
  }

  let show { this_name; this_trie; tries; _ } env =
    let this = (Format.sprintf "this (%s)" this_name, this_trie)
    and others =
      List.map ~f:(fun (key, (_, trie)) -> (key, trie)) (S_map.bindings tries)
    in
    let lines =
      List.map (this :: others) ~f:(fun (name, trie) ->
          Trie.show (name, trie) env)
    in
    String.concat lines ~sep:"\n\n"

  let find_trie_for { root; analysis = { tries; _ } } class_name =
    if String.equal class_name root.root_name then
      Some root.root_trie
    else
      Option.map ~f:snd (S_map.find_opt class_name tries)
  (* -- Helpers ------------------------------------------------------------- *)

  type type_for_access =
    | Constant of Typing_defs_core.decl_ty
    | Upper_bound of Typing_defs_core.decl_ty
    | Lower_bound of Typing_defs_core.decl_ty
    | Upper_and_lower_bounds of {
        upper: Typing_defs_core.decl_ty;
        lower: Typing_defs_core.decl_ty;
      }
    | No_type

  let transform_type_for_access t ~f =
    match t with
    | Constant decl_ty -> Constant (f decl_ty)
    | Upper_bound decl_ty -> Upper_bound (f decl_ty)
    | Lower_bound decl_ty -> Lower_bound (f decl_ty)
    | Upper_and_lower_bounds { upper; lower } ->
      Upper_and_lower_bounds { upper = f upper; lower = f lower }
    | No_type -> No_type

  (** When accessing a type constant through another type constant we need to
      determine the type that contains the next type constant.
      For a 'concrete' type constant this is the definition and for an 'abstract' type constant it is
      the upper-bound, if it exists. *)
  let type_for_access typeconst =
    let open Typing_defs in
    match typeconst with
    | TCConcrete { tc_type } -> Constant tc_type
    | TCAbstract { atc_as_constraint = Some ty; atc_super_constraint = None; _ }
      ->
      Upper_bound ty
    | TCAbstract { atc_as_constraint = None; atc_super_constraint = Some ty; _ }
      ->
      (* We can't actually do anything with this but we return it explicitly
         just in case we want to distinguish from the [No_type] case *)
      Lower_bound ty
    | TCAbstract
        { atc_as_constraint = Some upper; atc_super_constraint = Some lower; _ }
      ->
      Upper_and_lower_bounds { upper; lower }
    | TCAbstract { atc_as_constraint = None; atc_super_constraint = None; _ } ->
      No_type

  (** Helper to convert between type constant refinement representations  *)
  let typeconst_of_refined_const Typing_defs_core.{ rc_bound; _ } ~const_pos =
    match rc_bound with
    | Typing_defs_core.TRexact tc_type -> Typing_defs.(TCConcrete { tc_type })
    | Typing_defs_core.TRloose bounds ->
      let Typing_defs_core.{ tr_lower; tr_upper } = bounds in
      let atc_as_constraint =
        match tr_upper with
        | [] -> None
        | _ ->
          let reason = Typing_reason.witness_from_decl const_pos in
          Some (Typing_make_type.intersection reason tr_upper)
      and atc_super_constraint =
        match tr_lower with
        | [] -> None
        | _ ->
          let reason = Typing_reason.witness_from_decl const_pos in
          Some (Typing_make_type.union reason tr_lower)
      in
      Typing_defs.(
        TCAbstract
          { atc_as_constraint; atc_super_constraint; atc_default = None })

  (** When following a path of type constant accesses the meaning of [this] is
      relative to the class containing the type constant. Since we want to
      extract the contants for use from some base class we need to make these
      references absolute.

      As an example, consider the following

      ```
      class C<T> { }

      class B {
        const type TB as C<this>;
      }

      abstract class A {
        abstract const type TA as B;
        public static function wibble(this::TA::TB $_): void {}
      }
      ```

      When access `this::TA::TB` we end up with `C<this>` where `this` corresponds
      to the class in which `TB` is defined. Since we have access this type
      through `A` we can make it 'absolute' by subsituting occurrences of
      this in the definition of `this::TA::TB` with prefix of that path i.e.
      `this::TA`. Doing so gives us `this::TA::TB as C<this::TA>`
  *)
  let this_to_absolute ty ~prefix =
    let open Typing_defs_core in
    let rec build this reason prefix ~k =
      match prefix with
      | [] -> k this
      | next :: rest ->
        build this reason rest ~k:(fun root ->
            k (mk (reason, Taccess (root, next))))
    in
    let on_ty ty ~ctx =
      match deref ty with
      | (reason, Tthis) -> build ty reason prefix ~k:(fun ty -> (ctx, `Stop ty))
      | _ -> (ctx, `Continue ty)
    and on_rc_bound rc_bound ~ctx = (ctx, `Continue rc_bound) in
    Typing_defs_core.transform_top_down_decl_ty ty ~ctx:() ~on_ty ~on_rc_bound

  let replace_this_with ty ~replacement =
    let open Typing_defs_core in
    let on_ty ty ~ctx =
      match get_node ty with
      | Tthis -> (ctx, `Stop replacement)
      | _ -> (ctx, `Continue ty)
    and on_rc_bound rc_bound ~ctx = (ctx, `Continue rc_bound) in
    Typing_defs_core.transform_top_down_decl_ty ty ~ctx:() ~on_ty ~on_rc_bound

  (** Accumulate all types mentioned in the definition or bounds of a type constant *)
  let accumulate_tys typeconst tys =
    let open Typing_defs in
    match typeconst with
    | TCConcrete { tc_type } -> tc_type :: tys
    | TCAbstract { atc_as_constraint; atc_super_constraint; _ } ->
      (match (atc_as_constraint, atc_super_constraint) with
      | (Some ty1, Some ty2) -> ty1 :: ty2 :: tys
      | (Some ty, _)
      | (_, Some ty) ->
        ty :: tys
      | _ -> tys)

  type path =
    | This of (Pos_or_decl.t * string) list
    | Class of Typing_defs_core.decl_ty * string * (Pos_or_decl.t * string) list

  (** Fold over [ty] and accumulate type constant access paths for any
      [Taccess] which is rooted in [TThis] *)
  let paths_of_ty ty =
    let paths = ref [] in
    let on_ty decl_ty ~ctx =
      let open Typing_defs_core in
      match get_node decl_ty with
      | Taccess (root_ty, ty_const) ->
        let (ty, path) = access_path root_ty [ty_const] in
        let () =
          match get_node ty with
          | Tthis -> paths := This path :: !paths
          | Tapply ((_, class_name), _) ->
            paths := Class (ty, class_name, path) :: !paths
          | _ -> ()
        in
        (ctx, `Stop decl_ty)
      | _ -> (ctx, `Continue decl_ty)
    and on_rc_bound rc_bound ~ctx = (ctx, `Continue rc_bound) in
    let _ =
      Typing_defs_core.transform_top_down_decl_ty ty ~ctx:() ~on_ty ~on_rc_bound
    in
    !paths
  (* -- Core logic ---------------------------------------------------------  *)

  let meet_trie_opt trie1 trie2 reason =
    let Trie.{ base = b1; children = c1 } = trie1
    and Trie.{ base = b2; children = c2 } = trie2 in
    if not (S_map.is_empty c1 && S_map.is_empty c2) then
      None
    else
      let base_opt =
        match (b1, b2) with
        | (Trie.Root, Trie.Root) -> Some b1
        | ( Trie.Typeconst { typeconst = tc1; pos; origin },
            Trie.Typeconst { typeconst = tc2; _ } ) ->
          let typeconst = meet_typeconst reason tc1 tc2 in
          Some (Trie.Typeconst { typeconst; pos; origin })
        | _ -> None
      in
      Option.map base_opt ~f:(fun base -> Trie.{ base; children = c1 })

  (** Find the definition or bounds of the next type constant [const_name]
      when projecting from [base]. If the constant already appeared in some
      other path we can use the information stored in [children] otherwise
      we have to look it up from the decl *)
  let rec find_type_const trie tys const prefix state env =
    let Trie.{ children; base } = trie and (const_pos, const_name) = const in
    match S_map.find_opt const_name children with
    (* Happy path - we've already seen the constant as part of some other path
       so reuse the result *)
    | Some status -> (status, tys, state)
    (* We haven't already seen this constant within this path we need to find the
       definition within the current type we are projecting from *)
    | None -> begin
      match base with
      | Trie.Root ->
        (* If we are projecting from the root we can use the [folded_class]
           corresponding to that root directly - since we are at the root
           we don't need to rewrite `this` to make absolute *)
        let typeconst_opt =
          Typing_env.get_typeconst env state.root.root_folded_class const_name
        in
        let (status, tys) =
          Option.value_map
            typeconst_opt
            ~default:(Trie.Undefined, tys)
            ~f:(fun Typing_defs.{ ttc_kind = typeconst; ttc_origin = origin; _ }
               ->
              let status = Trie.status_of_typeconst typeconst ~origin ~const_pos
              (* The type constant may be an alias - if so we need need to add
                 the path to ensure it is substituted *)
              and tys = accumulate_tys typeconst tys in
              (status, tys))
        in
        (status, tys, state)
      | Trie.Typeconst { typeconst; _ } -> begin
        (* If the root is a type constant we first need to get the type we're
           accessing through:
           - for a 'concrete' type constant this is the definition;
           - for an 'abstract' type constant it is declared bounds *)
        let ty_access = type_for_access typeconst in
        access_typeconsts ty_access tys const prefix state env
      end
    end

  and access_typeconsts ty_access tys const prefix state env =
    match ty_access with
    | Constant ty ->
      (* Concrete type constant *)
      access_typeconst ty tys const prefix state env
    | Upper_bound ty ->
      (* Abstract type constant with an [as] bound only *)
      access_typeconst ty tys const prefix state env
    | Lower_bound _ ->
      (* Abstract type constant with a [super] bound only; this is undefined
         since a type constant in a lower bound doesn't require that a concrete
         definition has such a constant *)
      (Undefined, tys, state)
    | No_type ->
      (* Abstract type constant with no declared bounds *)
      (Undefined, tys, state)
    | Upper_and_lower_bounds { upper; lower } ->
      (* Abstract type constant with both [as] and [super] bounds;
         by transitivity if we have LB <: TC <: UB we must have LB <: UB
         but we are free to declare a type constant where this isn't true.
         Since we are only elaborating here we treat this as though the user
         had instead declared the unsatisfiable bounds on a generic so we
         don't generate an error
      *)
      let (status1, tys, state) =
        access_typeconst upper tys const prefix state env
      in
      let (status2, tys, state) =
        access_typeconst lower tys const prefix state env
      in
      let status =
        match (status1, status2) with
        | (Defined _, Undefined) ->
          (* Defined in upper bound but not present in the lower bound - this breaks
             the requirement that LB <: UB. We still generate a type without
             warning since we are just elaborating the declaration. Any attempt
             to use the function will lead to an error, though *)
          status1
        | (Undefined, (Defined _ | Undefined)) ->
          (* Undefined in the upper bound and since we can't conclude it's present
             in a type satisfying the bounds we have to say it is undefined *)
          status1
        | (Defined trie1, Defined trie2) ->
          (* Defined in both upper and lower bounds; find the meet of the concrete
             or abstract definitions. In practice any instantiable class must have
             agreement between type constants but we can't assume that here
             since it's possible to _declare_ a typeconstant whose upper and
             lower bounds aren't subtypes and so allow disagreement. *)
          let reason =
            let (const_pos, _const_name) = const in
            Typing_reason.witness_from_decl const_pos
          in
          let trie_opt = meet_trie_opt trie1 trie2 reason in
          Option.value_map trie_opt ~default:Trie.Undefined ~f:(fun trie ->
              Defined trie)
      in

      (status, tys, state)

  (** Access [const_name] through [base_ty] and modify occurences of `this` in the
      typeconstant definition so they are absolute with respect to [folded_class]

      This returns a `(typeconst option, path) result` since it may be the case
      that [base_ty] is a path which we haven't yet added to the analysis
  *)
  and access_typeconst base_ty tys const prefix state env =
    let open Typing_defs_core in
    let (const_pos, const_name) = const in
    match get_node base_ty with
    | Tthis ->
      (* For this use the [folded_class] corresponding to the absolute root
         class for the analysis *)
      let typeconst_opt =
        Typing_env.get_typeconst env state.root.root_folded_class const_name
      in
      let (status, tys) =
        Option.value_map
          typeconst_opt
          ~default:(Trie.Undefined, tys)
          ~f:(fun Typing_defs.{ ttc_kind = typeconst; ttc_origin = origin; _ }
             ->
            (* Ensure references to `this` are made absolute *)
            let typeconst =
              transform_typeconst ~f:(this_to_absolute ~prefix) typeconst
            in
            let status = Trie.status_of_typeconst typeconst ~origin ~const_pos
            (* Accumulate the types in the type constant - if they are aliases
               to another type constant we will need to add them to the
               analysis *)
            and tys = accumulate_tys typeconst tys in
            (status, tys))
      in
      (status, tys, state)
    | Tapply ((_, class_name), _) ->
      let folded_class_opt =
        Decl_entry.to_option (Typing_env.get_class env class_name)
      in
      let typeconst_opt =
        Option.bind folded_class_opt ~f:(fun folded_class ->
            Typing_env.get_typeconst env folded_class const_name)
      in
      let (status, tys) =
        Option.value_map
          typeconst_opt
          ~default:(Trie.Undefined, tys)
          ~f:(fun Typing_defs.{ ttc_kind = typeconst; ttc_origin = origin; _ }
             ->
            let typeconst =
              transform_typeconst ~f:(this_to_absolute ~prefix) typeconst
            in
            let status = Trie.status_of_typeconst typeconst ~origin ~const_pos
            (* Accumulate the types in the type constant - if they are aliases
               to another type constant we will need to add them to the
               analysis *)
            and tys = accumulate_tys typeconst tys in
            (status, tys))
      in
      (status, tys, state)
    | Trefinement (base_ty, { cr_consts }) -> begin
      (* If there is a refinement check if it contains the contant we want;
         if not, search in the root type *)
      match S_map.find_opt const_name cr_consts with
      | Some refined_const ->
        (* For the definition of the refined constant [this] is in the same
           class so don't make absolute *)
        let typeconst = typeconst_of_refined_const refined_const ~const_pos in
        let status = Trie.status_of_typeconst typeconst ~const_pos in
        (* Accumulate the types in the type constant - if they are aliases
           to another type constant we will need to add them to the
           analysis *)
        let tys = accumulate_tys typeconst tys in
        (status, tys, state)
      | None -> access_typeconst base_ty tys const prefix state env
    end
    | Taccess (root_ty, ty_const) -> begin
      (* We're accessing a type constant through a type constant access path;
         ensure the path has been added then access the type constant through
         its definition or upper bound *)
      let (ty, path) = access_path root_ty [ty_const] in
      let (state, status_opt) =
        match get_node ty with
        | Tthis ->
          let paths = [This path] in
          let state = add_paths state paths env in
          let status_opt = Trie.find_path state.root.root_trie path in
          (state, status_opt)
        | Tapply ((_, class_name), _) ->
          let paths = [Class (ty, class_name, path)] in
          let state = add_paths state paths env in
          let status_opt =
            Option.bind
              ~f:(fun trie -> Trie.find_path trie path)
              (find_trie_for state class_name)
          in
          (state, status_opt)
        | _ ->
          (* Here we assume the root must either be [Tapply] or [Tthis] -
             the other possibility, [Trefinement], is disallowed *)
          (state, None)
      in
      let base_ty_opt =
        Option.bind status_opt ~f:(fun status ->
            match status with
            | Trie.Undefined -> None
            | Trie.Defined { base = Root; _ } -> Some (Constant ty)
            | Trie.Defined { base = Typeconst { typeconst; _ }; _ } ->
              let replace = replace_this_with ~replacement:ty in
              Some
                (transform_type_for_access
                   (type_for_access typeconst)
                   ~f:replace))
      in
      Option.value_map
        base_ty_opt
        ~default:(Trie.Undefined, tys, state)
        ~f:(fun base_ty -> access_typeconsts base_ty tys const prefix state env)
    end
    | Tintersection tys ->
      (* For intersections, we require the type constant to be defined on at least
         one of its elements; if it is defined on multiple we will refine each element
         to ensure they agree *)
      let (trie_opt, tys, state) =
        List.fold_left
          tys
          ~init:(None, tys, state)
          ~f:(fun (acc, tys, state) ty ->
            match access_typeconst ty tys const prefix state env with
            | (Undefined, _, _) -> (acc, tys, state)
            | (Defined trie2, tys, state) ->
              let reason =
                let (const_pos, _const_name) = const in
                Typing_reason.witness_from_decl const_pos
              in
              let acc =
                match acc with
                | None -> Some trie2
                | Some trie1 -> meet_trie_opt trie1 trie2 reason
              in
              (acc, tys, state))
      in
      let status =
        Option.value_map trie_opt ~default:Trie.Undefined ~f:(fun trie ->
            Defined trie)
      in
      (status, tys, state)
    | Tunion tys ->
      (* For unions, we require the refinement to be present on all elements. If
         it is missing on one the result is undefined *)
      let (trie_opt_res, tys, state) =
        List.fold_left
          tys
          ~init:(Ok None, tys, state)
          ~f:(fun (acc, tys, state) ty ->
            match acc with
            | Error _ -> (acc, tys, state)
            | Ok acc ->
              (match access_typeconst ty tys const prefix state env with
              | (Undefined, _, _) -> (Error (), tys, state)
              | (Defined trie2, tys, state) ->
                let reason =
                  let (const_pos, _const_name) = const in
                  Typing_reason.witness_from_decl const_pos
                in
                (match acc with
                | None -> (Ok (Some trie2), tys, state)
                | Some trie1 ->
                  (match meet_trie_opt trie1 trie2 reason with
                  | Some trie -> (Ok (Some trie), tys, state)
                  | _ -> (Error (), tys, state)))))
      in
      let status =
        match trie_opt_res with
        | Error _ -> Trie.Undefined
        | Ok trie_opt ->
          Option.value_map trie_opt ~default:Trie.Undefined ~f:(fun trie ->
              Defined trie)
      in
      (status, tys, state)
    (* We can't access type constants through any other type so return [Undefined] *)
    | _ -> (Trie.Undefined, tys, state)

  and add_path trie tys (prefix, path) state env =
    match path with
    | [] ->
      (* End of the type constant access path; return the updated trie and
         the types appearing in the definitions or bounds of type constants
         discovered *)
      (trie, tys, state)
    | ((_, const_name) as const) :: path -> begin
      (* Find the type constant [const_name] *)
      match find_type_const trie tys const prefix state env with
      | (Trie.Undefined, tys, state) ->
        (* If it was undefined we have an error so we can't add the rest of the
           path *)
        let trie = Trie.update trie const_name Trie.Undefined in
        (trie, tys, state)
      | (Trie.Defined child, tys, state) ->
        (* We found the constant so continue adding the remainder of the path *)
        let (child, tys, state) =
          add_path child tys (const :: prefix, path) state env
        in
        (* Update the trie with the subtrie for [child] *)
        (Trie.update trie const_name (Trie.Defined child), tys, state)
    end

  (** Add a number of type constant access [paths] while resolving from
      [state.root] *)
  and add_paths state paths env =
    let pending = Queue.create () in
    Queue.enqueue_all pending paths;
    let rec aux state =
      match Queue.dequeue pending with
      | None -> state
      | Some (This path) ->
        let (root_trie, tys, state) =
          add_path state.root.root_trie [] ([], path) state env
        in
        List.iter tys ~f:(fun ty -> Queue.enqueue_all pending (paths_of_ty ty));
        let root = { state.root with root_trie } in
        aux { state with root }
      | Some (Class (this_ty, class_name, path)) ->
        let class_name =
          if String.equal class_name Naming_special_names.Classes.cSelf then
            state.root.root_name
          else
            class_name
        in
        let folded_class_opt =
          Decl_entry.to_option (Typing_env.get_class env class_name)
        in
        (match folded_class_opt with
        | None -> aux state
        | Some folded_class -> begin
          (* Find the existing trie for this class if it exists *)
          let class_trie =
            Option.value ~default:Trie.empty (find_trie_for state class_name)
          in
          (* Add the path with the class as [this] *)
          let class_root =
            {
              root_name = class_name;
              root_folded_class = folded_class;
              root_trie = class_trie;
            }
          in
          let class_state = { root = class_root; analysis = state.analysis } in
          let (class_trie, tys, class_state) =
            add_path class_trie [] ([], path) class_state env
          in
          (* Add the paths ensuring that we replace occurrences of this with class type *)
          List.iter tys ~f:(fun ty ->
              Queue.enqueue_all
                pending
                (paths_of_ty (replace_this_with ty ~replacement:this_ty)));
          (* Move the trie for the current class in to the map *)
          let analysis =
            let tries =
              S_map.update
                class_name
                (function
                  | None -> Some (this_ty, class_trie)
                  | Some (this_ty, existing_trie) ->
                    Some (this_ty, Trie.merge existing_trie class_trie))
                class_state.analysis.tries
            in
            { class_state.analysis with tries }
          in
          aux { state with analysis }
        end)
    in
    aux state

  (* -- API ----------------------------------------------------------------- *)

  let add_type t ty env =
    let paths = paths_of_ty ty in
    let root =
      {
        root_name = t.this_name;
        root_folded_class = t.this_folded_class;
        root_trie = t.this_trie;
      }
    in
    let { root; analysis } = add_paths { root; analysis = t } paths env in
    let t = { analysis with this_trie = root.root_trie } in
    let { this_name; this_trie; tries; _ } = t in
    (* For analysis of classes other than this we need to subsitute occurrences
       of [this] at the root of typeconst accesses for the class type *)
    let tries =
      S_map.map
        (fun (replacement, trie) ->
          let f ty = replace_this_with ty ~replacement in
          (replacement, Trie.transform trie ~f))
        tries
    in
    (* For the analysis of this we need to subsitute occurrences of typeconst
       access rooted in the explicit class with [this] *)
    let this_trie =
      let on_ty ty ~ctx =
        let open Typing_defs_core in
        match deref ty with
        | (reason, Tapply ((_, class_name), _))
          when String.equal this_name class_name ->
          (ctx, `Stop (mk (reason, Tthis)))
        | _ -> (ctx, `Continue ty)
      and on_rc_bound rc_bound ~ctx = (ctx, `Continue rc_bound) in
      Trie.transform
        this_trie
        ~f:
          (Typing_defs_core.transform_top_down_decl_ty
             ~ctx:()
             ~on_ty
             ~on_rc_bound)
    in
    { t with this_trie; tries }

  let add_fun_type t fun_ty env =
    let open Typing_defs_core in
    let ty = mk (Typing_reason.none, Tfun fun_ty) in
    add_type t ty env

  let of_fun_type fun_ty class_name folded_class env =
    let empty = create class_name folded_class in
    add_fun_type empty fun_ty env

  let this_constants { this_trie; _ } = Trie.constants this_trie

  module Subst = struct
    module Path = struct
      module Minimal = struct
        type t = string list [@@deriving ord]
      end

      include Minimal
      module Map = Stdlib.Map.Make (Minimal)
    end

    type t = {
      this_name: string;
      this_ty: Typing_defs_core.decl_phase Typing_defs_core.ty;
      tparams:
        Typing_defs_core.decl_phase Typing_defs_core.ty Typing_defs_core.tparam
        list;
      this_subst: Typing_defs_core.decl_phase Typing_defs_core.ty Path.Map.t;
      class_subst:
        Typing_defs_core.decl_phase Typing_defs_core.ty Path.Map.t S_map.t;
    }

    module Projection = struct
      module Minimal = struct
        type t =
          | This of Path.t
          | Class of string * Path.t
        [@@deriving ord]
      end

      include Minimal
      module Set = Stdlib.Set.Make (Minimal)
    end

    (* -- Application ------------------------------------------------------- *)
    let apply { this_name; this_ty; this_subst; class_subst; _ } ty =
      let on_ty ty ~ctx =
        let open Typing_defs_core in
        match get_node ty with
        | Tthis -> (ctx, `Stop this_ty)
        | Taccess (root_ty, ty_const) -> begin
          (* Try and find a substitution for this type *)
          let (root_ty, path) = access_path root_ty [ty_const] in
          let path = List.map ~f:snd path in
          let projection_opt =
            match get_node root_ty with
            | Tthis -> Some (Projection.This path)
            | Tapply ((_, class_name), _) ->
              Some (Projection.Class (class_name, path))
            | _ -> None
          in
          Option.value_map
            projection_opt
            ~default:(ctx, `Stop ty)
            ~f:(fun projection ->
              if Projection.Set.mem projection ctx then
                (* If we have made this substitution before doing so again will
                   cause us to never terminate so we bail; in practice the
                   type constant cycle check should already have caught this
                   TODO(mjt) add error *)
                (ctx, `Stop ty)
              else
                (* Try and find a substitution *)
                let subst_ty_opt =
                  match projection with
                  | Projection.This _ ->
                    let ty_opt = Path.Map.find_opt path this_subst in
                    if Option.is_some ty_opt then
                      ty_opt
                    else
                      let subst_opt = S_map.find_opt this_name class_subst in
                      Option.bind subst_opt ~f:(fun subst ->
                          Path.Map.find_opt path subst)
                  | Projection.Class (class_name, _) ->
                    let subst_opt = S_map.find_opt class_name class_subst in
                    Option.bind subst_opt ~f:(fun subst ->
                        Path.Map.find_opt path subst)
                in
                (* If there is no substitution, stop; if there is record that we've
                   seen the source key before and restart the rewrite *)
                Option.value_map
                  subst_ty_opt
                  ~default:(ctx, `Stop ty)
                  ~f:(fun ty ->
                    let ctx = Projection.Set.add projection ctx in
                    (ctx, `Restart ty)))
        end
        | _ -> (ctx, `Continue ty)
      and on_rc_bound rc_bound ~ctx = (ctx, `Continue rc_bound) in
      Typing_defs_core.transform_top_down_decl_ty
        ty
        ~on_ty
        ~on_rc_bound
        ~ctx:Projection.Set.empty

    let apply_fun_ty t fun_ty =
      let open Typing_defs_core in
      let ty = mk (Typing_reason.none, Tfun fun_ty) in
      match get_node (apply t ty) with
      | Tfun fun_ty -> fun_ty
      | _ -> failwith "Expected function type"

    (* -- Build substitution for -------------------------------------------- *)

    type generic_info = {
      pos: Pos_or_decl.t;
      upper_bound: Typing_defs_core.decl_phase Typing_defs_core.ty option;
      lower_bound: Typing_defs_core.decl_phase Typing_defs_core.ty option;
    }

    (* -- Build substitution for concrete type constants -------------------- *)

    let class_subst_help trie env generics =
      let rec aux Trie.{ base; children } (const_name, rev_path) acc =
        let acc =
          match base with
          | Typeconst { typeconst = Typing_defs.(TCConcrete { tc_type }); _ } ->
            let (subst, env, generics) = acc in
            let path = List.rev rev_path in
            (Path.Map.add path tc_type subst, env, generics)
          | Typeconst
              {
                typeconst =
                  Typing_defs.(
                    TCAbstract
                      {
                        atc_as_constraint = upper_bound;
                        atc_super_constraint = lower_bound;
                        _;
                      });
                pos;
                _;
              } ->
            let (subst, env, generics) = acc in
            let (env, generic_name) =
              Typing_env.fresh_param_name env const_name
            in
            let subst =
              let path = List.rev rev_path in
              let reason = Typing_reason.witness_from_decl pos in
              let ty = Typing_defs_core.(mk (reason, Tgeneric generic_name)) in
              Path.Map.add path ty subst
            in
            let generics =
              S_map.add generic_name { pos; upper_bound; lower_bound } generics
            in
            (subst, env, generics)
          | Root -> acc
        in
        S_map.fold
          (fun key status acc ->
            match status with
            | Trie.Undefined -> acc
            | Trie.Defined t -> aux t (key, key :: rev_path) acc)
          children
          acc
      in
      aux trie ("", []) (Path.Map.empty, env, generics)

    let class_subst { tries; _ } env generics =
      S_map.fold
        (fun class_name (_ty, trie) (tries, env, generics) ->
          let (subst, env, generics) = class_subst_help trie env generics in
          (S_map.add class_name subst tries, env, generics))
        tries
        (S_map.empty, env, generics)

    (* -- Build substitution for abstract type constants -------------------- *)

    let add_refinement rfmts const_name ~path ~subst =
      let full_path = List.rev (const_name :: path) in
      let ty_opt = Path.Map.find_opt full_path subst in
      Option.value_map ty_opt ~default:rfmts ~f:(fun ty ->
          S_map.add
            const_name
            Typing_defs_core.{ rc_bound = TRexact ty; rc_is_ctx = false }
            rfmts)

    (* Where we have intersections we don't require that all elements of that
       intersection define a type constant for the purposes of projections.
       However, when an element _does_ define the constant we want to refine it
       so we have agreement. This predicate is used to check whether a given
       constant is defined by a given class
    *)
    let should_refine env class_name =
      let folded_class_opt =
        Decl_entry.to_option (Typing_env.get_class env class_name)
      in
      match folded_class_opt with
      | None -> (fun _ -> true)
      | Some class_decl ->
        fun const_name ->
          Option.is_some (Typing_env.get_typeconst env class_decl const_name)

    let rec root_class_name_opt decl_ty =
      let open Typing_defs_core in
      match get_node decl_ty with
      | Tapply ((_, class_name), _) -> Some class_name
      | Trefinement (inner_decl_ty, _) -> root_class_name_opt inner_decl_ty
      | _ -> None

    let add_refinements children ~pred ~path ~subst ~init =
      S_map.fold
        (fun const_name _ rfmts ->
          if pred const_name then
            add_refinement rfmts const_name ~path ~subst
          else
            rfmts)
        children
        init

    let rec refine_ty env decl_ty subst path children =
      if S_map.is_empty children then
        decl_ty
      else
        let open Typing_defs_core in
        match deref decl_ty with
        | (reason, Tapply ((_, class_name), _)) ->
          let pred = should_refine env class_name in
          let cr_consts =
            add_refinements children ~pred ~path ~subst ~init:S_map.empty
          in
          let class_refinements = Typing_defs_core.{ cr_consts } in
          let ty_ = Typing_defs_core.Trefinement (decl_ty, class_refinements) in
          mk (reason, ty_)
        | (reason, Trefinement (inner_decl_ty, { cr_consts })) ->
          let pred =
            Option.value_map
              (root_class_name_opt inner_decl_ty)
              ~default:(fun _ -> true)
              ~f:(should_refine env)
          in
          let cr_consts =
            add_refinements children ~pred ~path ~subst ~init:cr_consts
          in
          let class_refinements = Typing_defs_core.{ cr_consts } in
          mk
            ( reason,
              Typing_defs_core.Trefinement (inner_decl_ty, class_refinements) )
        | (reason, Tunion tys) ->
          let tys =
            List.map tys ~f:(fun ty -> refine_ty env ty subst path children)
          in
          mk (reason, Tunion tys)
        | (reason, Tintersection tys) ->
          let tys =
            List.map tys ~f:(fun ty -> refine_ty env ty subst path children)
          in
          mk (reason, Tintersection tys)
        | _ -> decl_ty

    let update acc ~key ~typeconst ~pos ~children ~path =
      match typeconst with
      | Typing_defs.(TCConcrete { tc_type }) ->
        let (subst, env, generics) = acc in
        let subst = Path.Map.add (List.rev path) tc_type subst in
        (subst, env, generics)
      | Typing_defs.(TCAbstract { atc_as_constraint; atc_super_constraint; _ })
        ->
        let (subst, env, generics) = acc in
        let (env, generic_name) = Typing_env.fresh_param_name env key in
        let subst =
          let reason = Typing_reason.witness_from_decl pos in
          let ty = Typing_defs_core.(mk (reason, Tgeneric generic_name)) in
          Path.Map.add (List.rev path) ty subst
        in
        let upper_bound =
          Option.map atc_as_constraint ~f:(fun ty ->
              refine_ty env ty subst path children)
        and lower_bound =
          Option.map atc_super_constraint ~f:(fun ty ->
              refine_ty env ty subst path children)
        in
        let generics =
          S_map.add generic_name { pos; upper_bound; lower_bound } generics
        in
        (subst, env, generics)

    let this_subst { this_trie; _ } env generics =
      let Trie.{ base; children } = this_trie in
      let rec aux children ~path ~init =
        S_map.fold
          (fun key status acc ->
            match status with
            | Trie.Undefined -> acc
            | Trie.(Defined { children; base }) -> begin
              match base with
              | Trie.Typeconst { typeconst; pos; _ } ->
                let path = key :: path in
                let acc = aux children ~path ~init:acc in
                update acc ~key ~typeconst ~pos ~children ~path
              | Trie.Root -> acc
            end)
          children
          init
      in
      let init = (Path.Map.empty, env, generics) in
      match base with
      | Trie.Root -> aux children ~path:[] ~init
      | _ -> init

    (* -- Build all substitutions from the analysis --------------------------- *)
    let refine_this this_ty this_subst type_constants =
      if List.is_empty type_constants then
        this_ty
      else
        let open Typing_defs_core in
        let add_exact_refinements ~init =
          List.fold_left
            ~f:(fun rfmts name ->
              match Path.Map.find_opt [name] this_subst with
              | Some ty ->
                S_map.add
                  name
                  { rc_bound = TRexact ty; rc_is_ctx = false }
                  rfmts
              | None -> rfmts)
            ~init
            type_constants
        in
        match deref this_ty with
        | (reason, Tapply _) ->
          let cr_consts = add_exact_refinements ~init:S_map.empty in
          let class_refinement = Typing_defs.{ cr_consts } in
          mk (reason, Trefinement (this_ty, class_refinement))
        | ( reason,
            Typing_defs_core.Trefinement (this_ty, Typing_defs.{ cr_consts }) )
          ->
          let cr_consts = add_exact_refinements ~init:cr_consts in
          let class_refinement = Typing_defs.{ cr_consts } in
          mk (reason, Trefinement (this_ty, class_refinement))
        | _ -> this_ty

    let mk_tparams generics env =
      List.map
        (S_map.bindings generics)
        ~f:(fun (name, { pos; upper_bound; lower_bound }) ->
          (* We're generating a decl type so we need to add implicit
             upper bounds of [supportdyn<mixed>] under sdt or [mixed] otherwise
          *)
          let tp_constraints =
            let reason = Typing_reason.witness_from_decl pos in
            let ub =
              let tcopt = Typing_env_types.(env.genv.tcopt) in
              if Typechecker_options.everything_sdt tcopt then
                ( Ast_defs.Constraint_as,
                  Decl_enforceability.supportdyn_mixed pos reason )
              else
                (Ast_defs.Constraint_as, Typing_defs_core.(mk (reason, Tmixed)))
            in
            List.filter_opt
              [
                Option.map upper_bound ~f:(fun ty ->
                    (Ast_defs.Constraint_as, ty));
                Option.map lower_bound ~f:(fun ty ->
                    (Ast_defs.Constraint_super, ty));
                Some ub;
              ]
          in
          mk_tparam pos name tp_constraints)

    let of_typeconst_analysis analysis this_name this_ty env =
      let (this_subst, class_subst, generics, env) =
        let generics = S_map.empty in
        let (this_subst, env, generics) = this_subst analysis env generics in
        let (class_subst, env, generics) = class_subst analysis env generics in
        (this_subst, class_subst, generics, env)
      in
      let tparams = mk_tparams generics env in
      let subst = { this_name; this_ty; this_subst; class_subst; tparams } in
      (* Apply refinements to [this] to ensure it lines up with any abstract constants *)
      let this_ty = refine_this this_ty this_subst (this_constants analysis) in
      (env, { subst with this_ty })
  end

  let to_subst t this_ty env =
    let { this_name; _ } = t in
    Subst.of_typeconst_analysis t this_name this_ty env
end

(* -- Helpers --------------------------------------------------------------- *)
let ty_generics names ty =
  let acc = ref names in
  let on_ty decl_ty ~ctx =
    let open Typing_defs_core in
    match get_node decl_ty with
    | Tgeneric nm ->
      let () = acc := S_set.add nm !acc in
      (ctx, `Stop decl_ty)
    | _ -> (ctx, `Continue decl_ty)
  and on_rc_bound rc_bound ~ctx = (ctx, `Continue rc_bound) in
  let _ =
    Typing_defs_core.transform_top_down_decl_ty ty ~ctx:() ~on_ty ~on_rc_bound
  in
  !acc

(** Returns the positions of the explicit parameters from which a value of type
    [this] can be obtained (i.e. [this] occurs contravariantly or invariantly in
    the parameter type), but only when the function pointer would actually be able
    to relate two unrelated subtypes of [this] — the binary method problem.

    A single obtainable occurrence can be soundly abstracted into one type
    parameter during extraction. Two or more lets the caller obtain two values of
    type [this] whose runtime classes can differ, which the underlying method
    conflates as a single self type — the binary method problem. Invariance on a
    container pins its element *type*, not the runtime class of its contents (a
    [Box<B>] may hold any subtype of [B]), so invariant occurrences are obtainable
    too.

    For instance methods the implicit [this] receiver is itself an obtainable
    occurrence, so [~has_implicit_this:true] should be passed; in that case a
    single obtainable explicit occurrence is enough to trigger the error.

    Covariant occurrences (e.g. a parameter of type [(function(this): void)],
    which only *accepts* a [this]) and [this::T] type constant projections are
    never counted. *)
let explicit_this_param_positions
    ?(has_implicit_this = false) ?(class_tparams = []) ~env fun_ty =
  let open Typing_defs_core in
  let upper_bounds =
    if not has_implicit_this then
      S_map.empty
    else
      type_parameter_upper_bounds
        S_map.empty
        (class_tparams @ fun_ty.ft_tparams)
        fun_ty.ft_where_constraints
  in
  let init =
    if has_implicit_this then
      1
    else
      0
  in
  let (total, positions) =
    List.fold_left
      fun_ty.Typing_defs_core.ft_params
      ~init:(init, [])
      ~f:(fun (total, positions) Typing_defs_core.{ fp_type; fp_pos; _ } ->
        match
          Variance_analysis.count_obtainable_this_in_param
            ~follow_bounds:has_implicit_this
            ~upper_bounds
            fp_type
            ~env
        with
        | 0 -> (total, positions)
        | n -> (total + n, fp_pos :: positions))
  in
  if total >= 2 then
    List.rev positions
  else
    []

let drop_unused_generics fun_ty ~names =
  let open Typing_defs_core in
  let {
    ft_tparams;
    ft_params;
    ft_ret;
    ft_implicit_params = { capability };
    ft_where_constraints;
    _;
  } =
    fun_ty
  in
  (* First find any generics mentioned in parameters or in the return type *)
  let names =
    let names = ty_generics names ft_ret in
    let names =
      match capability with
      | CapTy ty -> ty_generics names ty
      | CapDefaults _ -> names
    in
    let names =
      List.fold_left ft_params ~init:names ~f:(fun names ft_param ->
          ty_generics names ft_param.fp_type)
    in
    (* We have to include where constraints to cover legacy projection from
       generics *)
    List.fold_left
      ft_where_constraints
      ~init:names
      ~f:(fun names (ty1, _, ty2) -> ty_generics (ty_generics names ty1) ty2)
  in
  let dependencies =
    S_map.of_list
      (List.map ft_tparams ~f:(fun { tp_name = (_, name); tp_constraints; _ } ->
           let dependencies =
             List.fold_left
               tp_constraints
               ~init:S_set.empty
               ~f:(fun dependencies (_, ty) -> ty_generics dependencies ty)
           in
           (name, S_set.remove name dependencies)))
  in
  let rec close used pending =
    match pending with
    | [] -> used
    | name :: pending when S_set.mem name used -> close used pending
    | name :: pending ->
      let used = S_set.add name used in
      let pending =
        Option.value_map
          (S_map.find_opt name dependencies)
          ~default:pending
          ~f:(fun dependencies ->
            List.rev_append (S_set.elements dependencies) pending)
      in
      close used pending
  in
  let names = close S_set.empty (S_set.elements names) in
  let ft_tparams =
    List.filter ft_tparams ~f:(fun { tp_name = (_, name); _ } ->
        S_set.mem name names)
  in
  { fun_ty with ft_tparams }

(** Find a nice name for a generic corresponding to [this] for polymorphic
    function pointers *)
let gen_this_name ft_tparams =
  let names =
    S_set.of_list
      (List.map ft_tparams ~f:(fun Typing_defs_core.{ tp_name = (_, nm); _ } ->
           nm))
  in
  let base = "Tthis" in
  let rec aux n_opt =
    let name =
      Option.value_map n_opt ~default:base ~f:(fun n ->
          Format.sprintf "%s#%n" base n)
    in
    if S_set.mem name names then
      aux (Option.value_map n_opt ~default:(Some 0) ~f:(fun n -> Some (n + 1)))
    else
      name
  in
  aux None

let apply_subst_generic fun_ty subst =
  let open Typing_defs_core in
  let on_ty ty ~ctx =
    match get_node ty with
    | Tgeneric nm ->
      (match S_map.find_opt nm subst with
      | Some ty ->
        (* Since other type parameters can appear inside bounds of type parameters
           we restart the transform after making a substitution *)
        (ctx, `Restart ty)
      | None -> (ctx, `Stop ty))
    | _ -> (ctx, `Continue ty)
  and on_rc_bound rc_bound ~ctx = (ctx, `Continue rc_bound) in
  let ty = mk (Typing_reason.none, Tfun fun_ty) in
  let ty = transform_top_down_decl_ty ty ~on_ty ~on_rc_bound ~ctx:() in
  match get_node ty with
  | Tfun fun_ty -> fun_ty
  | _ -> failwith "Expected function type"

(* -- API ------------------------------------------------------------------- *)
let extract_method fun_ty ~class_name ~folded_class ~env =
  (* We will need to handle substitution of [this] differently depending on
     the variance of its appearance in the function type:
     - If occurs co- or contravariantly we can substitute it with the the
       class itself or introduce a new type parameter with a lower or upper bound
       respectively. We prefer the former since instantiated types can take
       advantage of bidirectional typing
     - If it occurs invariantly we want to introduce a new type parameter so
       that function pointers are polymorphic in the type of [this] unless
       the class is final. In this case we can substitute [this] with the class
  *)
  let this_variance =
    if Folded_class.final folded_class then
      (* There is no point being polymorphic in [this] if it corresponds to a
         final class *)
      None
    else
      Variance_analysis.analyse_this fun_ty ~env
  in

  (* Record the list of type parameter appearing in the original function type;
     we need this since even though a parameter may be unused it may have
     reify directives which should be preserved *)
  let original_tparam_names =
    let open Typing_defs_core in
    let names =
      List.map ~f:(fun { tp_name = (_, name); _ } -> name) fun_ty.ft_tparams
    in
    S_set.of_list names
  in

  (* Get the class level generics; we need this to build a type for [this] and
     to add to the function generics. We can set any reified type parameters to
     be erased here since the requirement is met when the class is instantiated
     and no longer necessary on type parameters in the function pointer *)
  let class_tparams =
    List.map (Folded_class.tparams folded_class) ~f:(fun tp ->
        Typing_defs_core.{ tp with tp_reified = Erased })
  in

  (* Build the type corresponding to [this] or its upper bound *)
  let this_ty =
    Typing_defs_core.(
      mk
        ( Typing_reason.witness_from_decl (fst class_name),
          Tapply
            ( class_name,
              List.map
                class_tparams
                ~f:(fun Typing_defs_core.{ tp_name = (pos, name); _ } ->
                  Typing_defs_core.(
                    mk (Typing_reason.witness_from_decl pos, Tgeneric name))) )
        ))
  in

  (* Analyse the function type to determine which type constants have to
     be quantified over and the definition of concrete type constants *)
  let analysis =
    Typeconst_analysis.of_fun_type fun_ty (snd class_name) folded_class env
  in

  (* Generate the type parameters corresponding to abstract type constants, a
     substitution from to be applied to [Taccess] types and a refined version
     of [this] with equalities to generics standing in for abstract type
     constants *)
  let (env, subst) = Typeconst_analysis.to_subst analysis this_ty env in

  (* Add class-level generics, the generics for [this] and any generics
     for abstract type constants; some of these may end up not being used
     but we will clear that up later *)
  let fun_ty =
    let open Typing_defs_core in
    let { ft_tparams; _ } = fun_ty in
    let Typeconst_analysis.Subst.{ tparams; _ } = subst in
    let ft_tparams = class_tparams @ tparams @ ft_tparams in
    { fun_ty with ft_tparams }
  in

  (* At this point the 'this_ty' in the substitution is refined but may still
     contain occurrences of [this]. Depending on the variance of this in the
     function type we need to replace this with the generic it will be the
     upper bound of (invariant case) or the unrefined version of 'this_ty'.
     In the former case, we need to add the type parameter to function type *)
  let (fun_ty, subst) =
    match this_variance with
    | Some Ast_defs.Invariant ->
      let Typeconst_analysis.Subst.{ this_ty = refined_this_ty; _ } = subst in
      let Typing_defs_core.{ ft_tparams; _ } = fun_ty in
      let this_generic_name = gen_this_name ft_tparams in
      let this_generic_ty =
        Typing_defs_core.(
          mk
            ( Typing_reason.witness_from_decl (fst class_name),
              Tgeneric this_generic_name ))
      in
      let subst =
        Typeconst_analysis.Subst.{ subst with this_ty = this_generic_ty }
      in
      (* Replace occurrences of this in the upperbound with the generic *)
      let this_ty_upper_bound =
        Typeconst_analysis.Subst.apply subst refined_this_ty
      in
      (* Add the generic to the function type *)
      let this_tparam =
        mk_tparam
          (fst class_name)
          this_generic_name
          [(Ast_defs.Constraint_as, this_ty_upper_bound)]
      in
      let ft_tparams = this_tparam :: ft_tparams in
      (Typing_defs_core.{ fun_ty with ft_tparams }, subst)
    | _ ->
      let Typeconst_analysis.Subst.{ this_ty = refined_this_ty; _ } = subst in
      let this_ty =
        Typeconst_analysis.Subst.(apply { subst with this_ty } refined_this_ty)
      in
      let subst = Typeconst_analysis.Subst.{ subst with this_ty } in
      (fun_ty, subst)
  in

  (* Apply a subsitution replacing
     - [Tthis] with either a type parameter or the refined class
     - any concrete [Taccess] with its definition (except where its definition is abstract...)
     - any abstract [Taccess] with the corresponding generic *)
  let fun_ty = Typeconst_analysis.Subst.apply_fun_ty subst fun_ty in

  (* Next, analyse the variance of the remaining generics; if any occur
     only co- or contravariantly in the function type we can avoid quantifying
     over them by substituting for their lower- or upper-bound respectively *)
  let fun_ty =
    let open Typing_defs_core in
    let { ft_tparams; _ } = fun_ty in
    let ty_param_names =
      S_set.of_list
        (List.map ft_tparams ~f:(fun { tp_name = (_, nm); _ } -> nm))
    in
    let ty_param_bounds =
      S_map.of_list
        (List.map
           ft_tparams
           ~f:(fun { tp_name = (pos, nm); tp_constraints; _ } ->
             (nm, (pos, tp_constraints))))
    in
    let variances =
      Variance_analysis.analyse_ty_params fun_ty ty_param_names ~env
    in
    let subst =
      S_map.filter_map
        (fun name variance_opt ->
          match variance_opt with
          | None
          | Some Ast_defs.Invariant ->
            None
          | Some Ast_defs.Contravariant ->
            let (pos, constraints) = S_map.find name ty_param_bounds in
            let ubs =
              List.filter_map constraints ~f:(fun (c, ty) ->
                  match c with
                  | Ast_defs.Constraint_as
                  | Ast_defs.Constraint_eq ->
                    Some ty
                  | _ -> None)
            in
            let reason = Typing_reason.witness_from_decl pos in
            let ub = Typing_make_type.intersection reason ubs in
            Some ub
          | Some Ast_defs.Covariant ->
            let (pos, constraints) = S_map.find name ty_param_bounds in
            let lbs =
              List.filter_map constraints ~f:(fun (c, ty) ->
                  match c with
                  | Ast_defs.Constraint_super
                  | Ast_defs.Constraint_eq ->
                    Some ty
                  | _ -> None)
            in
            let reason = Typing_reason.witness_from_decl pos in
            let lb = Typing_make_type.union reason lbs in
            Some lb)
        variances
    in
    apply_subst_generic fun_ty subst
  in

  (* We may have introduced some unused generics; check which ones appear
     in parameters, implicit parameters or the return type and then find the
     transitive closure. Any generics not in this set can be dropped.
     Note: the original set of generic is used as the starting point since
     we need to keep the original type params if they are marked as reify *)
  let fun_ty = drop_unused_generics fun_ty ~names:original_tparam_names in

  (env, fun_ty)

let extract_instance_method fun_ty ~class_name ~folded_class ~env =
  let self_param =
    let fp_flags =
      let readonly_this = Typing_defs_core.Flags.get_ft_readonly_this fun_ty in
      Typing_defs_flags.FunParam.(set_readonly readonly_this default)
    in
    Typing_defs_core.
      {
        fp_pos = fst class_name;
        fp_type = mk (Typing_reason.witness_from_decl (fst class_name), Tthis);
        fp_name = None;
        fp_def_value = None;
        fp_flags;
      }
  in
  let fun_ty =
    Typing_defs_core.{ fun_ty with ft_params = self_param :: fun_ty.ft_params }
  in
  extract_method fun_ty ~class_name ~folded_class ~env

let extract_static_method fun_ty ~class_name ~folded_class ~env =
  let (env, fun_ty) =
    extract_instance_method fun_ty ~class_name ~folded_class ~env
  in
  let Typing_defs_core.{ ft_params; _ } = fun_ty in
  let ft_params =
    match ft_params with
    | _ :: ft_params -> ft_params
    | _ -> failwith "Expected at least one parameter"
  in
  let fun_ty = Typing_defs_core.{ fun_ty with ft_params } in
  (* In generic methods we don't need to retain unused class-level generics even
     if they are marked [reify] *)
  let fun_ty = drop_unused_generics fun_ty ~names:S_set.empty in
  (env, fun_ty)
