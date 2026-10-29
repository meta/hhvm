(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude
open Typing_defs
open Typing_env_types
open Common
module Cls = Folded_class
module Env = Typing_env
module DataType = Typing_atomic_data_types
module SN = Naming_special_names

let rec is_safe_for_strict_equality_refinement_impl expansions env ty =
  let all env tys =
    let (env, safe) =
      Common.List.map_env
        env
        tys
        ~f:(is_safe_for_strict_equality_refinement_impl expansions)
    in
    (env, List.for_all safe ~f:Fn.id)
  in
  let (env, ty) = Env.expand_type env ty in
  match get_node ty with
  | Tnonnull -> (env, true)
  | Tprim
      ( Aast.Tnull | Aast.Tbool | Aast.Tint | Aast.Tfloat | Aast.Tnum
      | Aast.Tresource | Aast.Tarraykey ) ->
    (env, true)
  | Tclass ((_, name), _, []) when String.equal name SN.Classes.cString ->
    (env, true)
  | Tclass ((_, name), _, tys)
    when String.equal name SN.Collections.cVec
         || String.equal name SN.Collections.cKeyset ->
    all env tys
  | Tclass ((_, name), _, [_; value])
    when String.equal name SN.Collections.cDict ->
    is_safe_for_strict_equality_refinement_impl expansions env value
  | Tclass ((_, name), _, []) -> begin
    match Env.get_class env name with
    | Decl_entry.Found cls when List.is_empty (Cls.tparams cls) -> (env, true)
    | _ -> (env, false)
  end
  | Tvec_or_dict (_, value) ->
    is_safe_for_strict_equality_refinement_impl expansions env value
  | Ttuple { t_required; t_optional; t_extra = Tvariadic ty } ->
    all env ((ty :: t_required) @ t_optional)
  | Tshape (Shape_simple { s_fields; s_unknown_value; _ }) ->
    let tys =
      TShapeMap.fold
        (fun _ { sft_ty; _ } tys -> sft_ty :: tys)
        s_fields
        [s_unknown_value]
    in
    all env tys
  | Toption ty -> is_safe_for_strict_equality_refinement_impl expansions env ty
  | Tunion [] -> (env, true)
  | Tunion tys
  | Tintersection tys ->
    let (env, tys) = Common.List.map_env env tys ~f:Env.expand_type in
    let tys = List.filter tys ~f:(fun ty -> not (is_dynamic ty)) in
    if List.is_empty tys then
      (env, false)
    else
      all env tys
  | Tnewtype (name, [ty], _) when String.equal name SN.Classes.cSupportDyn ->
    is_safe_for_strict_equality_refinement_impl expansions env ty
  | Tnewtype (name, _, _)
    when S_set.mem name expansions
         || String.equal name SN.Classes.cEnumClassLabel ->
    (env, false)
  | Tnewtype (name, [], _)
    when Env.is_enum env name && not (Env.is_enum_class env name) -> begin
    match Env.get_class env name with
    | Decl_entry.Found cls -> begin
      match Cls.enum_type cls with
      | Some { te_base; _ } ->
        let ((env, err), representation) =
          Typing_utils.localize ~ety_env:empty_expand_env env te_base
        in
        if Option.is_some err then
          (env, false)
        else
          is_safe_for_strict_equality_refinement_impl
            (S_set.add name expansions)
            env
            representation
      | None -> (env, false)
    end
    | _ -> (env, false)
  end
  | Tnewtype (name, tyargs, _) -> begin
    match Env.get_typedef env name with
    | Decl_entry.Found
        {
          td_type_assignment = SimpleTypeDef (_, representation);
          td_tparams;
          _;
        }
      when List.length td_tparams = List.length tyargs ->
      let ety_env =
        {
          empty_expand_env with
          substs = Decl_subst.make_locl td_tparams tyargs;
        }
      in
      let ((env, err), representation) =
        Typing_utils.localize ~ety_env env representation
      in
      if Option.is_some err then
        (env, false)
      else
        is_safe_for_strict_equality_refinement_impl
          (S_set.add name expansions)
          env
          representation
    | _ -> (env, false)
  end
  | _ -> (env, false)

let is_safe_for_strict_equality_refinement env ty =
  let (env, unresolved) = Typing_utils.contains_unresolved_tyvars env ty in
  if unresolved then
    (env, false)
  else
    is_safe_for_strict_equality_refinement_impl S_set.empty env ty

(* given [a..z] *)
(* return [(a, [b..z]); (b, [a;c..z]); (c, [a..b;d..z]); ... (z, [b..z])] *)
(* i.e. map each element in the input to a pair of that element and the other
    elements of the input*)
let list_to_list_of_e_and_others xs =
  let rec helper prefix rest =
    match rest with
    | h :: tail -> (h, prefix @ tail) :: helper (prefix @ [h]) tail
    | [] -> []
  in
  helper [] xs

(* Under pessimise_builtins, the hint Enum is localized as ~Enum & arraykey
   to account for intish string cast. This function matches this intersection
   to extract the Enum *)
let match_pessimized_enum_intersection env tys =
  let enum_name ty =
    match get_node ty with
    | Tnewtype (name, [], _)
      when Typing_env.is_enum env name
           && not (Typing_env.is_enum_class env name) ->
      Some name
    | _ -> None
  in
  let enum_name_from_like ty =
    match get_node ty with
    | Tunion [dyn; enum] when is_dynamic dyn -> enum_name enum
    | Tunion [enum; dyn] when is_dynamic dyn -> enum_name enum
    | _ -> None
  in
  match tys with
  | [t1; t2] when is_arraykey t1 -> enum_name_from_like t2
  | [t1; t2] when is_arraykey t2 -> enum_name_from_like t1
  | _ -> None

module TyPredicate = struct
  let is_runtime_checkable_generic env name =
    (not (Env.is_fresh_generic_parameter name))
    && ((match Env.get_reified env name with
        | Aast.Reified -> true
        | Aast.SoftReified
        | Aast.Erased ->
          false)
       || Env.get_enforceable env name
       || Typing_defs.DependentKind.is_generic_dep_ty name)

  (* Recognize the [dynamic] half and the [this] half of a localized [~this].
     [dyn_ty] must be a [Tdynamic _] and [this_ty] must be the runtime-checkable
     [Tgeneric "this"] (i.e. it would already produce a [GenericTag] predicate
     with [name = "this"] on its own). *)
  let is_like_this env dyn_ty this_ty =
    match (get_node dyn_ty, get_node this_ty) with
    | (Tdynamic _, Tgeneric name) ->
      String.equal name SN.Typehints.this
      && is_runtime_checkable_generic env name
    | _ -> false

  let rec of_ty env next_wildcard_id (ty : locl_ty) :
      (int * type_predicate, string) Result.t =
    let (_is_supportdyn, env, ty) = Typing_utils.strip_supportdyn env ty in
    Result.map ~f:(fun (next_wildcard_id, pred_) ->
        (next_wildcard_id, (get_reason ty, pred_)))
    @@
    match get_node ty with
    | Tprim Aast.Tbool -> Result.Ok (next_wildcard_id, IsTag BoolTag)
    | Tprim Aast.Tint -> Result.Ok (next_wildcard_id, IsTag IntTag)
    | Tprim Aast.Tarraykey -> Result.Ok (next_wildcard_id, IsTag ArraykeyTag)
    | Tprim Aast.Tfloat -> Result.Ok (next_wildcard_id, IsTag FloatTag)
    | Tprim Aast.Tnum -> Result.Ok (next_wildcard_id, IsTag NumTag)
    | Tprim Aast.Tresource -> Result.Ok (next_wildcard_id, IsTag ResourceTag)
    | Tprim Aast.Tnull -> Result.Ok (next_wildcard_id, IsTag NullTag)
    (* TODO: optional and variadic fields T201398626 T201398652 *)
    | Ttuple { t_required; t_optional = []; t_extra = Tvariadic t_variadic }
      when Typing_defs.is_nothing t_variadic -> begin
      match
        List.fold_left
          t_required
          ~init:(Result.Ok (next_wildcard_id, []))
          ~f:(fun acc ty ->
            let open Hh_prelude.Result.Let_syntax in
            let* (next_wildcard_id, predicates) = acc in
            let* (next_wildcard_id, predicate) =
              of_ty env next_wildcard_id ty
            in
            return (next_wildcard_id, predicate :: predicates))
      with
      | Result.Error err -> Result.Error ("tuple-" ^ err)
      | Result.Ok (next_wildcard_id, predicates) ->
        Result.Ok
          (next_wildcard_id, IsTupleOf { tp_required = List.rev predicates })
    end
    | Tshape (Shape_simple { s_origin = _; s_unknown_value; s_fields }) -> begin
      match
        TShapeMap.fold
          (fun key s_field acc ->
            let sfp_optional = s_field.sft_optional in
            let open Hh_prelude.Result.Let_syntax in
            let* (next_wildcard_id, sf_predicates) = acc in
            let* (next_wildcard_id, sfp_predicate) =
              of_ty env next_wildcard_id s_field.sft_ty
            in
            return
              ( next_wildcard_id,
                (key, { sfp_optional; sfp_predicate }) :: sf_predicates ))
          s_fields
          (Result.Ok (next_wildcard_id, []))
      with
      | Result.Error err -> Result.Error ("shape-" ^ err)
      | Result.Ok (next_wildcard_id, elts) ->
        let result_is_open =
          if
            (* this type comes from localizing a hint so a closed shape should have
               the canonical form of the nothing type *)
            Typing_defs.is_nothing s_unknown_value
          then
            Result.Ok false
          else if
            ty_equal s_unknown_value (Typing_make_type.mixed Reason.none)
            || ty_equal
                 s_unknown_value
                 (Typing_make_type.supportdyn Reason.none
                 @@ Typing_make_type.mixed Reason.none)
          then
            Result.Ok true
          else
            Result.Error "unexpected s_unknown_value"
        in
        Result.map result_is_open ~f:(fun sp_allows_unknown_fields ->
            ( next_wildcard_id,
              IsShapeOf
                { sp_allows_unknown_fields; sp_fields = TShapeMap.of_list elts }
            ))
    end
    | Tshape (Shape_splat _) -> Result.Error "shape_splat"
    | Tclass (_, Exact, _) -> Result.Error "exact class"
    | Tclass ((_, name), Nonexact _, args) ->
      let (next_wildcard_id, generics) =
        List.map_env next_wildcard_id args ~f:(fun next_wildcard_id ty ->
            match get_node ty with
            | Tgeneric name when Env.is_fresh_generic_parameter name ->
              (next_wildcard_id + 1, Wildcard next_wildcard_id)
            | _ -> (next_wildcard_id, Filled ty))
      in
      let (next_wildcard_id, generics) =
        match Env.get_class env name with
        | Decl_entry.Found class_info ->
          let tparams = Folded_class.tparams class_info in
          let pad_len = List.length tparams - List.length generics in
          if pad_len > 0 then
            (* if the class's type arguments are underspecified, we need to
               fill them in so that when we zip them with the tparams later,
               the count matches up *)
            let pad_generics =
              List.init pad_len ~f:(fun i -> Wildcard (i + next_wildcard_id))
            in
            let next_wildcard_id = next_wildcard_id + pad_len in
            (next_wildcard_id, generics @ pad_generics)
          else
            (* ignore excess type arguments if the class is known;
               if Tclass comes from a localized hint (as is the case for is/as)
               then this pruning should already happen, but just in case *)
            (next_wildcard_id, List.take generics @@ List.length tparams)
        | Decl_entry.DoesNotExist
        | Decl_entry.NotYetAvailable ->
          (next_wildcard_id, generics)
      in
      Result.Ok (next_wildcard_id, IsTag (ClassTag (name, generics)))
    | Tprim Aast.Tvoid -> Result.Error "void"
    | Tprim Aast.Tnoreturn -> Result.Error "noreturn"
    | Tnonnull ->
      Result.Ok (next_wildcard_id, IsNot (get_reason ty, IsTag NullTag))
    | Tdynamic _ -> Result.Error "dynamic"
    | Tany _ -> Result.Error "any"
    | Toption ty_opt -> begin
      let open Hh_prelude.Result.Let_syntax in
      let* (next_wildcard_id, pred_opt) = of_ty env next_wildcard_id ty_opt in
      let pred_null = (get_reason ty, IsTag NullTag) in
      return (next_wildcard_id, IsUnionOf [pred_opt; pred_null])
      |> Result.map_error ~f:(fun err -> "option-" ^ err)
    end
    | Tfun _ -> Result.Error "fun"
    | Tgeneric name when is_runtime_checkable_generic env name ->
      Result.Ok
        (next_wildcard_id, IsTag (GenericTag { name; from_like = false }))
    | Tgeneric _ -> Result.Error "generic"
    | Tunion [ty1; ty2]
      when is_like_this env ty1 ty2 || is_like_this env ty2 ty1 ->
      (* Recognize [Tunion [dynamic; this]] (in either order) and treat it
         as the [this] predicate, tagged so that [to_ty] can reconstruct
         the [~this] form. This is the localized shape of an [is ~this]
         refinement produced for non-final/generic classes under Sound
         Dynamic; see [Typing_phase.localize] [Tthis] arm. *)
      Result.Ok
        ( next_wildcard_id,
          IsTag (GenericTag { name = SN.Typehints.this; from_like = true }) )
    | Tunion tys -> begin
      match
        List.fold_result tys ~init:(next_wildcard_id, []) ~f:(fun acc ty ->
            let (next_wildcard_id, predicates) = acc in
            let open Hh_prelude.Result.Let_syntax in
            let* (next_wildcard_id, predicate) =
              of_ty env next_wildcard_id ty
            in
            return (next_wildcard_id, predicate :: predicates))
      with
      | Result.Error err -> Result.Error ("union-" ^ err)
      | Result.Ok (next_wildcard_id, predicates) ->
        Result.Ok (next_wildcard_id, IsUnionOf (List.rev predicates))
    end
    | Tintersection tys -> begin
      match match_pessimized_enum_intersection env tys with
      | Some name -> Result.Ok (next_wildcard_id, IsTag (EnumTag name))
      | None -> Result.Error "intersection"
    end
    | Tvec_or_dict (tk, tv) ->
      let r = get_reason ty in
      of_ty env next_wildcard_id
      @@ Typing_make_type.union
           r
           [Typing_make_type.vec r tv; Typing_make_type.dict r tk tv]
      |> Result.map ~f:(fun (next_wildcard_id, (_reason, predicate_)) ->
             (next_wildcard_id, predicate_))
    | Taccess _ -> Result.Error "access"
    | Tvar _ -> Result.Error "tvar"
    | Tnewtype (name, [], _)
      when Env.is_enum env name && not (Env.is_enum_class env name) ->
      Result.Ok (next_wildcard_id, IsTag (EnumTag name))
    | Tnewtype (s, _, _) -> Result.Error ("newtype-" ^ s)
    | Tdependent _ -> Result.Error "dependent"
    | Tneg _ -> Result.Error "neg"
    | Tlabel _ -> Result.Error "label"
    | Ttuple _ -> Result.Error "tuple"
    | Tclass_ptr _ -> Result.Error "class_ptr"

  let of_ty env ty =
    Result.map (of_ty env 0 ty) ~f:(fun (_next_wildcard_id, ty) -> ty)

  let instantiate_wildcards_for_tag env reason tag p =
    match tag with
    | BoolTag
    | IntTag
    | ArraykeyTag
    | FloatTag
    | NumTag
    | ResourceTag
    | NullTag
    | EnumTag _
    | GenericTag _ ->
      (env, I_map.empty)
    | ClassTag (id, generics) ->
      let (env, new_tparams) =
        match Env.get_class env id with
        | Decl_entry.Found class_info ->
          let tparams = Folded_class.tparams class_info in
          (* if we're only getting ClassTag predicates by calling of_ty on
             Tclass localized from is/as hints, then the count should match
             since localization will trim excess arguments and of_ty should
             fill in when there are too few arguments;
             we shouldn't be creating ClassTag just anywhere, so we should
             fail when something is unexpected *)
          let tparam_generic_pairs = List.zip_exn tparams generics in
          let (env, tyl, new_tparams) =
            List.fold_left
              tparam_generic_pairs
              ~init:(env, [], I_map.empty)
              ~f:(fun (env, tyl, new_tparams) (tparam, generic) ->
                match generic with
                | Filled ty -> (env, ty :: tyl, new_tparams)
                | Wildcard wildcard_key ->
                  let {
                    tp_name = (_, tparam_name);
                    tp_reified = reified;
                    tp_user_attributes;
                    _;
                  } =
                    tparam
                  in
                  let enforceable =
                    Attributes.mem
                      Naming_special_names.UserAttributes.uaEnforceable
                      tp_user_attributes
                  in
                  let newable =
                    Attributes.mem
                      Naming_special_names.UserAttributes.uaNewable
                      tp_user_attributes
                  in
                  let (env, new_name) =
                    Env.add_fresh_generic_parameter
                      env
                      (Reason.to_pos reason)
                      tparam_name
                      ~reified
                      ~enforceable
                      ~newable
                  in
                  let ty = Typing_make_type.generic reason new_name in
                  let new_tparam = ((tparam, new_name), ty) in
                  (env, ty :: tyl, I_map.add wildcard_key new_tparam new_tparams))
          in
          let tyl = List.rev tyl in
          let ety_env =
            {
              empty_expand_env with
              substs = Decl_subst.make_locl tparams tyl;
              this_ty = Typing_make_type.class_type reason id tyl;
            }
          in
          let add_bounds env (t, ty_fresh) =
            List.fold_left t.tp_constraints ~init:env ~f:(fun env (ck, ty) ->
                (* Substitute fresh type parameters for
                    * original formals in constraint *)
                let ((env, ty_err_opt), ty) =
                  Typing_utils.localize ~ety_env env ty
                in
                Option.iter
                  ~f:(Typing_error_utils.add_typing_error ~env)
                  ty_err_opt;
                Typing_utils.add_constraint env ck ty_fresh ty
                @@ Some (Typing_error.Reasons_callback.unify_error_at p))
          in
          let env =
            List.fold_left (List.zip_exn tparams tyl) ~f:add_bounds ~init:env
          in
          (env, new_tparams)
        | Decl_entry.NotYetAvailable
        | Decl_entry.DoesNotExist ->
          (env, I_map.empty)
      in
      (env, new_tparams)

  let rec instantiate_wildcards_for_predicate env predicate p :
      Typing_env_types.env * ((decl_tparam * string) * locl_ty) I_map.t =
    match predicate with
    | (reason, IsTag tag) ->
      let (env, new_tparams) = instantiate_wildcards_for_tag env reason tag p in
      (env, new_tparams)
    | (_reason, IsTupleOf { tp_required }) ->
      let (env, new_tparams) =
        List.map_env env tp_required ~f:(fun env predicate ->
            let (env, new_tparams) =
              instantiate_wildcards_for_predicate env predicate p
            in
            (env, new_tparams))
      in
      (env, List.fold new_tparams ~init:I_map.empty ~f:I_map.union)
    | (_reason, IsShapeOf { sp_fields; sp_allows_unknown_fields = _ }) ->
      let (env, new_tparams) =
        TShapeMap.fold_env
          env
          (fun env _key { sfp_predicate; sfp_optional = _ } new_tparams_acc ->
            let (env, new_tparams) =
              instantiate_wildcards_for_predicate env sfp_predicate p
            in
            (env, I_map.union new_tparams_acc new_tparams))
          sp_fields
          I_map.empty
      in
      (env, new_tparams)
    | (_reason, IsUnionOf predicates) ->
      let (env, new_tparams) =
        List.map_env env predicates ~f:(fun env predicate ->
            let (env, new_tparams) =
              instantiate_wildcards_for_predicate env predicate p
            in
            (env, new_tparams))
      in
      (env, List.fold new_tparams ~init:I_map.empty ~f:I_map.union)
    | (_reason, IsNot inner) -> instantiate_wildcards_for_predicate env inner p

  let rec to_ty env lookup_wildcard predicate =
    let tag_to_ty reason tag =
      match tag with
      | BoolTag -> Typing_make_type.bool reason
      | IntTag -> Typing_make_type.int reason
      | ArraykeyTag -> Typing_make_type.arraykey reason
      | FloatTag -> Typing_make_type.float reason
      | NumTag -> Typing_make_type.num reason
      | ResourceTag -> Typing_make_type.resource reason
      | NullTag -> Typing_make_type.null reason
      | ClassTag (id, generics) ->
        let tyargs =
          List.map generics ~f:(fun g ->
              match g with
              | Filled ty -> ty
              | Wildcard key -> lookup_wildcard key)
        in
        Typing_make_type.class_type reason id tyargs
      | EnumTag id ->
        let (_env, cstr) = Typing_utils.get_newtype_super env reason id [] in
        let enum_ty = mk (reason, Tnewtype (id, [], cstr)) in
        if Typechecker_options.pessimise_builtins (Typing_env.get_tcopt env)
        then
          (* Mirror localization of E to ~E & arraykey *)
          Typing_make_type.intersection
            reason
            [
              Typing_make_type.locl_like reason enum_ty;
              Typing_make_type.arraykey reason;
            ]
        else
          enum_ty
      | GenericTag { name; from_like = false } ->
        Typing_make_type.generic reason name
      | GenericTag { name; from_like = true } ->
        (* Reconstruct the [~this] form that [of_ty] collapsed into this tag. *)
        Typing_make_type.locl_like reason (Typing_make_type.generic reason name)
    in
    match predicate with
    | (reason, IsTag tag) -> tag_to_ty reason tag
    | (reason, IsTupleOf { tp_required }) ->
      mk
        ( reason,
          Ttuple
            {
              t_required = List.map tp_required ~f:(to_ty env lookup_wildcard);
              t_optional = [];
              t_extra = Tvariadic (Typing_make_type.nothing reason);
            } )
    | (reason, IsShapeOf { sp_fields; sp_allows_unknown_fields }) ->
      let map =
        TShapeMap.map
          (fun { sfp_predicate; sfp_optional } ->
            {
              sft_optional = sfp_optional;
              sft_ty = to_ty env lookup_wildcard sfp_predicate;
            })
          sp_fields
      in
      let kind =
        if sp_allows_unknown_fields then
          (* open shape predicates from hints always have supportdyn kinds
             https://fburl.com/code/aguo2c7i *)
          Typing_make_type.supportdyn_mixed reason
        else
          Typing_make_type.nothing reason
      in
      Typing_make_type.shape reason kind map
    | (reason, IsUnionOf predicates) -> begin
      match
        List.partition_tf predicates ~f:(fun p ->
            match p with
            | (_, IsTag NullTag) -> true
            | _ -> false)
      with
      | (_ :: _, [other]) ->
        Typing_make_type.nullable reason (to_ty env lookup_wildcard other)
      | (_ :: _, others) ->
        Typing_make_type.nullable reason
        @@ Typing_make_type.union reason
        @@ List.map others ~f:(to_ty env lookup_wildcard)
      | ([], _) ->
        Typing_make_type.union reason
        @@ List.map predicates ~f:(to_ty env lookup_wildcard)
    end
    | (reason, IsNot inner) -> Typing_make_type.neg reason inner

  exception FoundWildcard

  let to_ty_without_instantiation_opt env predicate =
    try Some (to_ty env (fun _key -> raise FoundWildcard) predicate) with
    | FoundWildcard -> None

  let to_ty env instantiation_map p predicate =
    to_ty
      env
      (fun key ->
        match I_map.find_opt key instantiation_map with
        | Some ty -> ty
        | None ->
          Diagnostics.invariant_violation
            p
            (Telemetry.create ())
            "Unexpected failure to instantiate type from predicate"
            ~report_to_user:true;
          Typing_make_type.nothing Reason.none)
      predicate
end

module Uninstantiated_typing_logic = struct
  type is_subtype_special_class = {
    sub_reason: Reason.t;
    sub_id: Ast_defs.id_;
    sub_args: type_tag_generic list;
    super_reason: Reason.t;
    super_id: pos_id;
    super_exact: exact;
    super_args: locl_ty list;
  }

  type subtype_prop =
    | Valid
    | Invalid
    (* The only uninstantiated relation that we produce is for the GADT-style
       inference when refining a value of one class type by a predicate of a
       descendant class type. We represent this using the components of each
       side to (a) represent this assumption and (b) because we need super_exact
       when instantiating the proposition.
       This prop is instantiated into a TL.IsSubtype of two class types but
       where we copy the exact of the super type onto the sub type we get by
       converting the predicate.
       We do this because otherwise with-refinements on the super type might
       cause the subtype relationship to not hold (and not produce any
       refinements on generics).
    *)
    | IsSubtypeSpecialClass of is_subtype_special_class
    | Conj of subtype_prop * subtype_prop
    | Disj of subtype_prop * subtype_prop
    | Instantiated of Typing_logic.subtype_prop

  let valid = Valid

  let invalid = Invalid

  let conj a b = Conj (a, b)

  let disj a b = Disj (a, b)

  module TL = Typing_logic

  (* This converts any exact with-refinements (e.g. type T = int) into the
     equivalent loose refinements (e.g. type T as int super int).
     The reason for this is that an exact refinement will shadow any existing
     information about the type constant on the class while loose refinements
     are added on.
     This is used when copying the with-refinements from the super type onto
     the sub type for IsSubtypeSpecialClass.
     It's relevant in the case that the super class has an exact refinement but
     the sub class already has the type constant bounded or assigned.
  *)
  let replace_exact_with_loose = function
    | Exact -> Exact
    | Nonexact { cr_consts } ->
      let cr_consts =
        S_map.map
          (fun { rc_bound; rc_is_ctx } ->
            let rc_bound =
              match rc_bound with
              | TRloose bounds -> TRloose bounds
              | TRexact ty -> TRloose { tr_lower = [ty]; tr_upper = [ty] }
            in
            { rc_bound; rc_is_ctx })
          cr_consts
      in
      Nonexact { cr_consts }

  let rec instantiate_prop env map pos = function
    | Valid -> TL.valid
    | Invalid -> TL.invalid ~fail:None
    | IsSubtypeSpecialClass
        {
          sub_reason;
          sub_id;
          sub_args;
          super_reason;
          super_id;
          super_exact;
          super_args;
        } ->
      let sub_ty =
        TyPredicate.to_ty
          env
          map
          pos
          (sub_reason, IsTag (ClassTag (sub_id, sub_args)))
      in
      let sub_ty =
        match deref sub_ty with
        | (r, Tclass (sub_id, _, sub_args)) ->
          mk (r, Tclass (sub_id, replace_exact_with_loose super_exact, sub_args))
        | _ ->
          Diagnostics.invariant_violation
            pos
            (Telemetry.create ())
            "Conversion of a class predicate did not produce a class type"
            ~report_to_user:true;
          sub_ty
      in
      let super_ty =
        mk (super_reason, Tclass (super_id, super_exact, super_args))
      in
      TL.IsSubtype
        ( false,
          Typing_defs_constraints.LoclType sub_ty,
          Typing_defs_constraints.LoclType super_ty )
    | Conj (p1, p2) ->
      TL.conj
        (instantiate_prop env map pos p1)
        (instantiate_prop env map pos p2)
    | Disj (p1, p2) ->
      TL.disj
        ~fail:None
        (instantiate_prop env map pos p1)
        (instantiate_prop env map pos p2)
    | Instantiated prop -> prop

  let print_prop env prop =
    let rec print_prop = function
      | Valid -> "TRUE"
      | Invalid -> "FALSE"
      | Conj (p1, p2) -> "(" ^ print_prop p1 ^ " && " ^ print_prop p2 ^ ")"
      | Disj (p1, p2) -> "(" ^ print_prop p1 ^ " || " ^ print_prop p2 ^ ")"
      | IsSubtypeSpecialClass
          {
            sub_reason = _;
            sub_id;
            sub_args;
            super_reason;
            super_id;
            super_exact;
            super_args;
          } ->
        let sub_predicate_ = IsTag (ClassTag (sub_id, sub_args)) in
        let super_ty =
          mk (super_reason, Tclass (super_id, super_exact, super_args))
        in
        Typing_defs.show_type_predicate_ sub_predicate_
        ^ " <S: "
        ^ Typing_print.debug env super_ty
      | Instantiated prop -> Typing_print.subtype_prop env prop
    in
    print_prop prop
end

module UTL = Uninstantiated_typing_logic

let cartesian = Partition.cartesian

module TyPartition = struct
  module Partition = Partition.Make (struct
    type t = locl_ty

    let compare = Typing_defs.compare_locl_ty ~normalize_lists:true
  end)

  type assumptions = UTL.subtype_prop

  type t = Partition.t * assumptions * assumptions

  type base_ty =
    | ClassTy of Reason.t * pos_id * exact * locl_ty list
    | OtherTy

  let valid = UTL.valid

  let invalid = UTL.invalid

  (* Assuming a value `v` is a sub type of [base_ty] and satisifies [predicate], subtyping
     relations must hold *)
  let assume env base_ty predicate : assumptions =
    Option.value ~default:valid
    @@
    match (base_ty, snd predicate) with
    (* Given a value `v` of type `C<T>` and a predicate `is D`, search for a class
       `K<TC>` and `K<TD>` respectively, it must hold that `K<TC> = K<TD>`
    *)
    | ( ClassTy (r, ((_, name) as posid), exact, tyargs),
        IsTag (ClassTag (id, pred_args)) ) ->
      let open Option.Let_syntax in
      (* let* cls1 = Decl_entry.to_option (Env.get_class env id1) in *)
      let* class_info = Decl_entry.to_option (Env.get_class env id) in
      if
        String.equal name (Cls.name class_info)
        || Cls.has_ancestor class_info name
        || Cls.requires_ancestor class_info name
      then
        let prop =
          UTL.IsSubtypeSpecialClass
            {
              sub_reason = fst predicate;
              sub_id = id;
              sub_args = pred_args;
              super_reason = r;
              super_id = posid;
              super_exact = exact;
              super_args = tyargs;
            }
        in
        return prop
      else
        None
    | ((ClassTy _ | OtherTy), _) -> None

  let assume env ty predicate =
    match get_node ty with
    | Tclass (a, b, c) ->
      assume env (ClassTy (get_reason ty, a, b, c)) predicate
    | _ -> assume env OtherTy predicate

  let mk_left ~env ~predicate ty =
    (Partition.mk_left ty, assume env ty predicate, invalid)

  let mk_span ~env ~predicate ty =
    let assumptions = assume env ty predicate in
    (* the false assumptions is `valid` because `valid` is the value that means
       "I don't know anything" since it has the property that it is subsumed
       under the "meet" (conjunction) operation and is the subsumer under the
       "join" (disjunction) operation.
       We always use a trivial `valid` here because, unlike with true assumptions,
       non-trivial false assumptions are filled external to mk_* functions. *)
    (Partition.mk_span ty, assumptions, valid)

  let mk_right ~env:_ ~predicate:_ ty = (Partition.mk_right ty, invalid, valid)

  let mk_bottom = (Partition.mk_bottom, invalid, invalid)

  let join (partition1, t1, f1) (partition2, t2, f2) =
    (Partition.join partition1 partition2, UTL.disj t1 t2, UTL.disj f1 f2)

  let meet (partition1, t1, f1) (partition2, t2, f2) =
    (Partition.meet partition1 partition2, UTL.conj t1 t2, UTL.conj f1 f2)

  let union_combine (partition1, t1, f1) (partition2, t2, f2) =
    ( Partition.union_combine partition1 partition2,
      UTL.disj t1 t2,
      UTL.conj f1 f2 )

  let product ~f sub_splits_and_assumptions =
    let sub_splits = List.map sub_splits_and_assumptions ~f:fst3 in
    let assumption_pairs =
      List.map sub_splits_and_assumptions ~f:(fun (_, t, f) -> [t; f])
    in
    let assumption_sets = cartesian assumption_pairs in
    let (true_assumptions, false_assumptions) =
      match assumption_sets with
      (* the split () by () case *)
      | [] -> (valid, invalid)
      (* we rely on the detail that the first element in the result of cartesian
         is the first element of every input list *)
      | all_true_assumptions :: rest ->
        let true_assumptions =
          List.fold ~init:valid ~f:UTL.conj all_true_assumptions
        in
        let false_assumptions =
          List.fold ~init:invalid ~f:UTL.disj
          @@ List.map rest ~f:(List.fold ~init:valid ~f:UTL.conj)
        in
        (true_assumptions, false_assumptions)
    in
    (Partition.product f sub_splits, true_assumptions, false_assumptions)

  let simplify (partition, true_assumptions, false_assumptions) ty =
    (Partition.simplify partition ty, true_assumptions, false_assumptions)

  let left = Partition.left

  let span = Partition.span

  let right = Partition.right

  let flip (partition, true_assumptions, false_assumptions) =
    (Partition.flip partition, false_assumptions, true_assumptions)
end

type dnf_ty = locl_ty list list

type ty_partition = {
  predicate: type_predicate;
  left: dnf_ty;
  span: dnf_ty;
  right: dnf_ty;
  true_assumptions: UTL.subtype_prop;
  false_assumptions: UTL.subtype_prop;
}

let rec split_ty_by_tuple
    ~(expansions : S_set.t)
    ~(ty_datatype : DataType.t)
    (env : env)
    (ty : locl_ty)
    (sub_predicates : type_predicate list)
    (predicate : type_predicate) : env * TyPartition.t =
  match deref ty with
  (* TODO: optional and variadic fields T201398626 T201398652 *)
  | ( ty_reason,
      Ttuple { t_required; t_optional = []; t_extra = Tvariadic variadic } )
    when is_nothing variadic ->
    let predicate_ty_pairs = List.zip sub_predicates t_required in
    begin
      match predicate_ty_pairs with
      | List.Or_unequal_lengths.Unequal_lengths ->
        (env, TyPartition.mk_right ~env ~predicate ty (* mismatch arity *))
      | List.Or_unequal_lengths.Ok predicate_ty_pairs ->
        (* split each tuple element ty by its respective predicate *)
        let (env, sub_splits) =
          List.fold_map
            predicate_ty_pairs
            ~init:env
            ~f:(fun env (predicate, ty) ->
              split_ty ~other_intersected_tys:[] ~expansions env ty ~predicate)
        in
        ( env,
          TyPartition.product ~f:(Typing_make_type.tuple ty_reason) sub_splits
        )
    end
  | _ ->
    (* Tuples are vecs at runtime, thus if the type's data type is disjoint from a vec
       we can conclude the type must be in the right partition. Otherwise we do not
       precisely know the relationship with the [IsTupleOf] predicate so default to
       spanning
    *)
    let (env, predicate_datatype) =
      DataType.(of_ty ~safe_for_are_disjoint:false env Tuple)
    in
    if DataType.are_disjoint env ty_datatype predicate_datatype then
      (env, TyPartition.mk_right ~env ~predicate ty)
    else
      (env, TyPartition.mk_span ~env ~predicate ty)

and split_ty_by_shape
    ~(ty_datatype : DataType.t)
    (env : env)
    (ty : locl_ty)
    (sp_allows_unknown_fields : bool)
    (sp_fields : shape_field_predicate TShapeMap.t)
    (predicate : type_predicate) : env * TyPartition.t =
  match deref ty with
  | (_, Tshape (Shape_simple { s_origin = _; s_unknown_value; s_fields })) ->
    split_shape_fields
      env
      ty
      ~s_fields
      ~s_unknown_value
      ~sp_allows_unknown_fields
      ~sp_fields
      ~predicate
  | (r, Tshape (Shape_splat _)) ->
    (* Split a splat scrutinee by over-approximating it as a single simple shape.
       [get_shape_splat_concrete_supertypes] resolves each splat-position type
       parameter to its concrete shape bound(s), transitively *)
    let (env, resolved) =
      Typing_utils.get_shape_splat_concrete_supertypes env ty
    in
    (match resolved with
    | None ->
      (* We could not over-approximate the scrutinee as a concrete shape, so we
         conclude nothing about the predicate *)
      (env, TyPartition.mk_span ~env ~predicate ty)
    | Some ss_elems ->
      let (env, normalized) =
        Typing_utils.normalize_shape_type env r (Shape_splat { ss_elems })
      in
      (match normalized with
      | Typing_utils.Normalized_shape
          (Shape_simple { s_fields; s_unknown_value; _ }) ->
        split_shape_fields
          env
          ty
          ~s_fields
          ~s_unknown_value
          ~sp_allows_unknown_fields
          ~sp_fields
          ~predicate
      | Typing_utils.Normalized_shape (Shape_splat _)
      | Typing_utils.Normalized_distributed ->
        (env, TyPartition.mk_span ~env ~predicate ty)
      | Typing_utils.Normalized_bottom -> (env, TyPartition.mk_bottom)))
  | _ ->
    (* Shapes are dicts at runtime, thus if the type's data type is disjoint from a dict
       we can conclude the type must be in the right partition. Otherwise we do not
       precisely know the relationship with the [IsShapeOf] prediacte so default to
       spanning
    *)
    let (env, predicate_datatype) =
      DataType.(of_ty ~safe_for_are_disjoint:false env Shape)
    in
    if DataType.are_disjoint env ty_datatype predicate_datatype then
      (env, TyPartition.mk_right ~env ~predicate ty)
    else
      (env, TyPartition.mk_span ~env ~predicate ty)

and split_shape_fields
    (env : env)
    (ty : locl_ty)
    ~(s_fields : locl_phase shape_field_type TShapeMap.t)
    ~(s_unknown_value : locl_ty)
    ~(sp_allows_unknown_fields : bool)
    ~(sp_fields : shape_field_predicate TShapeMap.t)
    ~(predicate : type_predicate) : env * TyPartition.t =
  let has_class_const_field map =
    TShapeMap.exists
      (fun field _val ->
        match field with
        | TSFclass_const _ -> true
        | TSFregex_group _
        | TSFlit_str _ ->
          false)
      map
  in
  if has_class_const_field s_fields || has_class_const_field sp_fields then
    (* class const field names are unsound, so fall back to SPAN *)
    (env, TyPartition.mk_span ~env ~predicate ty)
  else
    (* RIGHT if: ty has a required field that is not in the predicate and predicate is closed *)
    let has_extra_required_field =
      (not sp_allows_unknown_fields)
      && TShapeMap.exists
           (fun key { sft_optional; _ } ->
             (not sft_optional) && not (TShapeMap.mem key sp_fields))
           s_fields
    in
    if has_extra_required_field then
      (env, TyPartition.mk_right ~env ~predicate ty)
    else
      (* Does the predicate require a field not in the type? *)
      let missing_required_field =
        TShapeMap.exists
          (fun key { sfp_optional; _ } ->
            (not sfp_optional) && not (TShapeMap.mem key s_fields))
          sp_fields
      in

      if missing_required_field && Typing_defs.is_nothing s_unknown_value then
        (* Missing a required field and the type is closed -> RIGHT *)
        (env, TyPartition.mk_right ~env ~predicate ty)
      else
        (* Split each field that exists in both ty and predicate *)
        let (env, field_splits) =
          TShapeMap.fold_env
            env
            (fun env key ty_field splits_acc ->
              match TShapeMap.find_opt key sp_fields with
              | Some pred_field ->
                (* Split this field's type by the predicate field's predicate *)
                let (env, field_split) =
                  split_ty
                    ~other_intersected_tys:[]
                    ~expansions:S_set.empty
                    env
                    ty_field.sft_ty
                    ~predicate:pred_field.sfp_predicate
                in
                (env, (key, ty_field, pred_field, field_split) :: splits_acc)
              | None ->
                (* Field only in ty, not in predicate. Checked above *)
                (env, splits_acc))
            s_fields
            []
        in

        (* Check if any field is fully incompatible (fully right) *)
        let has_incompatible_field =
          List.exists
            field_splits
            ~f:(fun (_, ty_field, _, (field_partition, _, _)) ->
              let field_left = TyPartition.left field_partition in
              let field_span = TyPartition.span field_partition in
              let is_fully_right =
                List.is_empty field_left && List.is_empty field_span
              in
              (* Required field in ty that's fully incompatible *)
              (not ty_field.sft_optional) && is_fully_right)
        in

        if has_incompatible_field then
          (env, TyPartition.mk_right ~env ~predicate ty)
        else if
          TShapeMap.exists
            (fun key _ -> not (TShapeMap.mem key s_fields))
            sp_fields
          && not (Typing_defs.is_nothing s_unknown_value)
        then
          (* predicate specifies a field not in the type and the type is open *)
          (env, TyPartition.mk_span ~env ~predicate ty)
        else
          (* Check if all fields are fully left *)
          let all_fields_fully_left =
            List.for_all
              field_splits
              ~f:(fun (_, _, _, (field_partition, _, _)) ->
                let field_span = TyPartition.span field_partition in
                let field_right = TyPartition.right field_partition in
                List.is_empty field_span && List.is_empty field_right)
          in

          (* For left, the predicate must be open or the type must be closed *)
          let openness_satisfies_left =
            sp_allows_unknown_fields || Typing_defs.is_nothing s_unknown_value
          in

          (* For left, for every shared field: predicate's must be optional or the ty's must be required *)
          let optionality_satisfied_left =
            TShapeMap.for_all
              (fun key { sfp_optional; _ } ->
                sfp_optional
                ||
                match TShapeMap.find_opt key s_fields with
                | Some { sft_optional; _ } -> not sft_optional
                | None -> true)
              sp_fields
          in

          if
            optionality_satisfied_left
            && all_fields_fully_left
            && openness_satisfies_left
          then
            (env, TyPartition.mk_left ~env ~predicate ty)
          else
            (* Cannot conclude fully left or fully right, fall back to span *)
            (env, TyPartition.mk_span ~env ~predicate ty)

and split_ty_by_tag
    ~(ty_datatype : DataType.t)
    (env : env)
    (ty : locl_ty)
    (tag : type_tag)
    (predicate : type_predicate) : env * TyPartition.t =
  let (env, predicate_datatype) =
    DataType.of_tag ~safe_for_are_disjoint:true env tag
  in
  let predicate_complement_datatype = DataType.complement predicate_datatype in
  if DataType.are_disjoint env ty_datatype predicate_datatype then
    (env, TyPartition.mk_right ~env ~predicate ty)
  else if DataType.are_disjoint env ty_datatype predicate_complement_datatype
  then
    (env, TyPartition.mk_left ~env ~predicate ty)
  else
    (env, TyPartition.mk_span ~env ~predicate ty)

and split_ty_by_union
    ~(other_intersected_tys : locl_ty list)
    ~(expansions : S_set.t)
    (env : env)
    (ty : locl_ty)
    (predicates : type_predicate list) : env * TyPartition.t =
  (* Split the type by each predicate in the union.

     When splitting T by union predicate (P1 | P2 | ... | Pn):
     - Left: parts that pass at least one predicate = L1 ∪ L2 ∪ ... ∪ Ln
     - Right: parts that fail all predicates = R1 ∩ R2 ∩ ... ∩ Rn
     - Span: parts that may or may not pass = everything else

     This is NOT the same as joining the partitions (which would give L1∪L2, S1∪S2, R1∪R2).
  *)
  let (env, partitions) =
    List.fold_map
      ~init:env
      ~f:(fun env pred ->
        split_ty ~other_intersected_tys ~expansions env ty ~predicate:pred)
      predicates
  in
  (* Combine partitions using the correct algebra for union predicates *)
  let partition =
    match partitions with
    | [] -> TyPartition.mk_bottom
    | first :: rest -> List.fold rest ~init:first ~f:TyPartition.union_combine
  in
  (env, partition)

and split_ty
    ~(other_intersected_tys : locl_ty list)
    ~(expansions : S_set.t)
    (env : env)
    (ty : locl_ty)
    ~(predicate : type_predicate) : env * TyPartition.t =
  let (env, ety) = Env.expand_type env ty in
  let rec partition_f predicate ((env : env), (ty_datatype : DataType.t)) :
      env * TyPartition.t =
    match snd predicate with
    | IsTupleOf { tp_required = sub_predicates } ->
      split_ty_by_tuple
        ~expansions
        ~ty_datatype
        env
        ety
        sub_predicates
        predicate
    | IsShapeOf { sp_allows_unknown_fields; sp_fields } ->
      split_ty_by_shape
        ~ty_datatype
        env
        ety
        sp_allows_unknown_fields
        sp_fields
        predicate
    | IsTag tag -> split_ty_by_tag ~ty_datatype env ety tag predicate
    | IsUnionOf predicates ->
      split_ty_by_union ~other_intersected_tys ~expansions env ety predicates
    | IsNot inner ->
      let (env, split) = partition_f inner (env, ty_datatype) in
      (env, TyPartition.flip split)
  in
  let partition_f = partition_f predicate in
  let split_union ~other_intersected_tys ~expansions env (tys : locl_ty list) =
    let (env, partitions) =
      List.fold_map
        ~init:env
        ~f:(split_ty ~expansions ~predicate ~other_intersected_tys)
        tys
    in
    let partition =
      List.fold ~init:TyPartition.mk_bottom ~f:TyPartition.join partitions
    in
    (env, partition)
  in
  let split_intersection
      ~other_intersected_tys ~init env ~expansions (tys : locl_ty list) =
    let ty_others_pairs = list_to_list_of_e_and_others tys in
    let (env, partitions) =
      List.fold_map
        ~init:env
        ~f:(fun env (ty, others) ->
          let other_intersected_tys = others @ other_intersected_tys in
          split_ty ~other_intersected_tys ~expansions ~predicate env ty)
        ty_others_pairs
    in
    let partition = List.fold ~init ~f:TyPartition.meet partitions in
    (env, partition)
  in
  let (env, partition) : env * TyPartition.t =
    match get_node ety with
    (* Types we cannot split, that we know will end up being a part of both
       partitions. *)
    | Tvar _
    | Tany _
    | Tdynamic _
    | Taccess _ ->
      (env, TyPartition.mk_span ~env ~predicate ty)
    | Tclass_ptr _ ->
      (* TODO: need a bespoke DataType to model KindOfClass *)
      (env, TyPartition.mk_span ~env ~predicate ty)
    (* Types we cannot split *)
    | Tprim Aast.((Tint | Tnull | Tvoid | Tbool | Tfloat | Tresource) as prim)
      ->
      partition_f
        DataType.(of_ty ~safe_for_are_disjoint:false env @@ Primitive prim)
    | Tfun _ ->
      partition_f DataType.(of_ty ~safe_for_are_disjoint:false env Function)
    | Tnonnull ->
      partition_f DataType.(of_ty ~safe_for_are_disjoint:false env Nonnull)
    | Ttuple _ ->
      partition_f DataType.(of_ty ~safe_for_are_disjoint:false env Tuple)
    | Tshape (Shape_simple _ | Shape_splat _) ->
      partition_f DataType.(of_ty ~safe_for_are_disjoint:false env Shape)
    | Tlabel _ ->
      partition_f DataType.(of_ty ~safe_for_are_disjoint:false env Label)
    | Tnewtype (name, _, _)
      when String.equal name Naming_special_names.Classes.cEnumClassLabel ->
      partition_f DataType.(of_ty ~safe_for_are_disjoint:false env Label)
    | Tclass ((_, name), _, args) ->
      partition_f
        DataType.(of_ty ~safe_for_are_disjoint:true env @@ Class (name, args))
    | Tneg (_, predicate) ->
      let rec to_dty env predicate =
        match predicate with
        | IsTag tag -> DataType.of_tag ~safe_for_are_disjoint:false env tag
        | IsTupleOf _ -> DataType.(of_ty ~safe_for_are_disjoint:false env Tuple)
        | IsShapeOf _ -> DataType.(of_ty ~safe_for_are_disjoint:false env Shape)
        | IsUnionOf preds ->
          List.fold_left_env
            env
            preds
            ~init:DataType.empty
            ~f:(fun env dt_acc pred ->
              let (env, dt) = to_dty env (snd pred) in
              (env, DataType.union dt_acc dt))
        | IsNot inner_pred ->
          let (env, inner_dt) = to_dty env (snd inner_pred) in
          (env, DataType.complement inner_dt)
      in
      let (env, dty) = to_dty env predicate in
      let dty = DataType.complement dty in
      partition_f (env, dty)
    | Tprim Aast.Tnoreturn -> (env, TyPartition.mk_bottom)
    (* Types we can split into a union of types *)
    | Tunion tyl -> split_union ~other_intersected_tys ~expansions env tyl
    | Tprim Aast.Tnum ->
      split_union
        ~other_intersected_tys
        env
        ~expansions
        [
          Typing_make_type.int (get_reason ty);
          Typing_make_type.float (get_reason ty);
        ]
    | Tprim Aast.Tarraykey ->
      split_union
        ~other_intersected_tys
        env
        ~expansions
        [
          Typing_make_type.int (get_reason ty);
          Typing_make_type.string (get_reason ty);
        ]
    | Tvec_or_dict (tk, tv) ->
      split_union
        ~other_intersected_tys
        env
        ~expansions
        [
          Typing_make_type.vec (get_reason ty) tv;
          Typing_make_type.dict (get_reason ty) tk tv;
        ]
    | Toption ty_opt ->
      split_union
        ~other_intersected_tys
        env
        ~expansions
        [Typing_make_type.null (get_reason ty); ty_opt]
    (* Types we need to split across an intersection *)
    | Tintersection [] ->
      split_ty ~other_intersected_tys ~expansions ~predicate env
      @@ Typing_make_type.mixed (get_reason ty)
    | Tintersection (ty :: tyl) ->
      let (env, init) =
        split_ty
          ~other_intersected_tys:(other_intersected_tys @ tyl)
          ~expansions
          ~predicate
          env
          ty
      in
      let other_intersected_tys = ty :: other_intersected_tys in
      split_intersection ~other_intersected_tys env ~init ~expansions tyl
    (* Below are types of the form T <: U. We treat these as T & U *)
    | Tdependent (_, super_ty) ->
      let (env, partition) =
        split_ty ~other_intersected_tys ~expansions ~predicate env super_ty
      in
      (env, TyPartition.(meet (mk_span ~env ~predicate ty) partition))
    | Tgeneric name
    | Tnewtype (name, _, _)
      when S_set.mem name expansions ->
      (env, TyPartition.mk_span ~env ~predicate ty)
    | Tgeneric name
      when match snd predicate with
           | IsTag (GenericTag { name = pred_name; from_like = _ }) ->
             String.equal name pred_name
           | _ -> false ->
      (* [T] trivially satisfies [is T]; without this short-circuit the general
       * Tgeneric case below recurses on upper bounds, where [GenericTag]'s
       * over-approximate DataType collapses the result to [mk_span]. *)
      (env, TyPartition.mk_left ~env ~predicate ty)
    | Tgeneric name ->
      let expansions = S_set.add name expansions in
      let upper_bounds = Env.get_upper_bounds env name |> Typing_set.elements in
      let init = TyPartition.mk_span ~env ~predicate ty in
      split_intersection
        ~other_intersected_tys
        env
        ~init
        ~expansions
        upper_bounds
    | Tnewtype (name, [ty_arg], _)
      when String.equal name Naming_special_names.Classes.cRepresentableAs ->
      (* RepresentableAs<T> shares its runtime data type with T, so a
       * predicate splits RepresentableAs<T> exactly as it splits T. *)
      let expansions = S_set.add name expansions in
      split_ty ~other_intersected_tys ~expansions ~predicate env ty_arg
    | Tnewtype (name, _, _)
      when match snd predicate with
           | IsTag (EnumTag pred_name) -> String.equal name pred_name
           | _ -> false ->
      (env, TyPartition.mk_left ~env ~predicate ty)
    | Tnewtype (name, tyl, _) ->
      let (env, as_ty) =
        Typing_utils.get_newtype_super env (get_reason ty) name tyl
      in
      let init = TyPartition.mk_span ~env ~predicate ty in
      let expansions = S_set.add name expansions in
      begin
        match Env.get_typedef env name with
        | Decl_entry.Found
            { td_type_assignment = CaseType (variant, variants); td_tparams; _ }
          ->
          (* The this_ty does not need to be set because case types cannot
           * appear within classes thus cannot us the this type.
           * If we ever change that this could needs to be changed *)
          let localize =
            Typing_utils.localize
              ~ety_env:
                {
                  empty_expand_env with
                  substs =
                    (if List.is_empty tyl then
                      S_map.empty
                    else
                      Decl_subst.make_locl td_tparams tyl);
                }
          in
          let variants = variant :: variants in
          let mk_subtype_prop sub super =
            Typing_logic.IsSubtype (false, LoclType sub, LoclType super)
          in
          let where_constraint_to_prop env (left, ck, right) =
            let ((env, _err), local_left) = localize env left in
            let ((env, _err), local_right) = localize env right in
            let prop =
              match ck with
              | Ast_defs.Constraint_as -> mk_subtype_prop local_left local_right
              | Ast_defs.Constraint_super ->
                mk_subtype_prop local_right local_left
              | Ast_defs.Constraint_eq ->
                Typing_logic.conj
                  (mk_subtype_prop local_left local_right)
                  (mk_subtype_prop local_right local_left)
            in
            (env, prop)
          in
          let where_constraints_to_prop env wcs =
            let (env, props) =
              List.fold_map ~init:env ~f:where_constraint_to_prop wcs
            in
            (env, Typing_logic.conj_list props)
          in
          let (env, ty_prop_pairs) =
            List.fold_map variants ~init:env ~f:(fun env variant ->
                let (hint, constraints) = variant in
                let ((env, _ty_err_opt), ty) = localize env hint in
                let (env, prop) = where_constraints_to_prop env constraints in
                (env, (ty, prop)))
          in
          let (env, partition_tyl) =
            (* This works for unconditional case types too but its superfluous
               to produce the assumptions in that case. *)
            if Typing_case_type_variant.has_where_clauses variants then
              (* filter tyl that are disjoint from &(other_intersected_tys) *)
              let filter_ty =
                Typing_make_type.intersection Reason.none other_intersected_tys
              in
              let ty_prop_pairs =
                List.filter ty_prop_pairs ~f:(fun (ty, _prop) ->
                    not
                    @@ Typing_atomic_data_types.are_locl_tys_disjoint
                         env
                         filter_ty
                         ty)
              in
              let (env, partitions) =
                List.fold_map
                  ~init:env
                  ~f:(fun env (current_ty, current_prop) ->
                    let (env, (partition_, true_assumptions, false_assumptions))
                        =
                      split_ty
                        ~other_intersected_tys
                        ~expansions
                        ~predicate
                        env
                        current_ty
                    in
                    (* Here we conj the where clause with the true and false
                       assumptions from the split; we do this so that any
                       FALSE from the split will eliminate the clause *)
                    let current_prop = UTL.Instantiated current_prop in
                    ( env,
                      ( partition_,
                        UTL.conj true_assumptions current_prop,
                        UTL.conj false_assumptions current_prop ) ))
                  ty_prop_pairs
              in
              let partition =
                List.fold
                  ~init:TyPartition.mk_bottom
                  ~f:TyPartition.join
                  partitions
              in
              (env, partition)
            else
              let tyl = List.map ty_prop_pairs ~f:fst in
              split_union ~other_intersected_tys env ~expansions tyl
          in
          (* For case types without where clauses, the variants alone
           * fully capture all values — meeting with the upper bound's
           * partition adds no information and introduces noisy
           * (X & null) | (X & nonnull) shapes when the bound is the
           * default mixed. Where-clause case types still need the meet
           * to apply the bound to the unrefined cases. *)
          if Typing_case_type_variant.has_where_clauses variants then
            let (env, partition_as_ty) =
              split_intersection
                ~other_intersected_tys
                env
                ~init
                ~expansions
                [as_ty]
            in
            (env, TyPartition.meet partition_tyl partition_as_ty)
          else
            (env, partition_tyl)
        | _ ->
          split_intersection
            ~other_intersected_tys
            env
            ~init
            ~expansions
            [as_ty]
      end
  in
  (* If one side of the partition is empty that means [ty] falls completely
     under the other side. Set the partition equal to the type to avoid
     computing any unions or intersections *)
  let partition = TyPartition.simplify partition ty in
  (env, partition)

let partition_ty (env : env) (ty : locl_ty) (predicate : type_predicate) =
  let (env, (partition, true_assumptions, false_assumptions)) =
    split_ty ~other_intersected_tys:[] ~expansions:S_set.empty ~predicate env ty
  in
  let left = TyPartition.left partition in
  let span = TyPartition.span partition in
  let right = TyPartition.right partition in
  Typing_log.(
    let from_list kind tyll =
      List.map tyll ~f:(fun tyl ->
          Log_type (kind, Typing_make_type.intersection Reason.none tyl))
    in
    log_with_level env "partition" ~level:1 (fun () ->
        let structures =
          (Log_type ("ty", ty) :: from_list "left" left)
          @ from_list "span" span
          @ from_list "right" right
          @ [
              Log_head
                ("true_assumptions = " ^ UTL.print_prop env true_assumptions, []);
              Log_head
                ( "false_assumptions = " ^ UTL.print_prop env false_assumptions,
                  [] );
            ]
        in
        log_types
          (Reason.to_pos @@ fst predicate)
          env
          [
            Log_head
              ("partition " ^ show_type_predicate_ @@ snd predicate, structures);
          ]));
  (env, { predicate; left; span; right; true_assumptions; false_assumptions })

let passes_predicate env ty predicate =
  let (_, { left = _; span; right; _ }) = partition_ty env ty predicate in
  List.is_empty span && List.is_empty right
