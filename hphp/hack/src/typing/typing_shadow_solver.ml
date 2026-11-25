(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude
open Typing_defs
module Inf = Typing_inference_env
module SN = Naming_special_names

type solution = {
  local_name: string;
  local_pos: Pos.t;
  shadow_tvid: Tvid.t;
  inferred_ty: locl_ty option;
}

let solve ~as_data (env : Typing_env_types.env) inf_env dynamic_locals =
  let r = Typing_reason.none in
  let open_shape_unknown_value = mk (r, Toption (mk (r, Tnonnull))) in
  let constraint_to_type ity =
    match ity with
    | Typing_defs_constraints.LoclType ty -> Some ty
    | Typing_defs_constraints.ConstraintType cty ->
      let (_, cty_) = Typing_defs_constraints.deref_constraint_type cty in
      (match cty_ with
      | Typing_defs_constraints.Tcan_index ci ->
        let key_ty = ci.Typing_defs_constraints.ci_key in
        let val_ty = ci.Typing_defs_constraints.ci_val in
        let as_data_ty =
          if as_data then
            let (_, _, index_expr) = ci.Typing_defs_constraints.ci_index_expr in
            let is_optional =
              match ci.Typing_defs_constraints.ci_access_kind with
              | Typing_defs_constraints.Ci_lhs_of_null_coalesce
              | Typing_defs_constraints.Ci_destructure_optional ->
                true
              | Typing_defs_constraints.Ci_normal -> false
            in
            match index_expr with
            | Aast.String s ->
              let field_name =
                TShapeField.make (TSFlit_str (Pos_or_decl.none, s))
              in
              let sft_ty =
                match get_node val_ty with
                | Tunion tyl ->
                  let tyl = List.filter tyl ~f:(fun t -> not (is_dynamic t)) in
                  (match tyl with
                  | [ty] -> ty
                  | _ -> mk (get_reason val_ty, Tunion tyl))
                | _ -> val_ty
              in
              Some
                (mk
                   ( r,
                     Tshape
                       (Shape_simple
                          {
                            s_origin = Missing_origin;
                            s_unknown_value = open_shape_unknown_value;
                            s_fields =
                              TShapeMap.singleton
                                field_name
                                { sft_optional = is_optional; sft_ty };
                          }) ))
            | Aast.Int _ -> Some (Typing_make_type.vec r val_ty)
            | _ -> None
          else
            None
        in
        (match as_data_ty with
        | Some ty -> Some ty
        | None -> Some (Typing_make_type.keyed_container r key_ty val_ty))
      | Typing_defs_constraints.Thas_member _ -> None
      | Typing_defs_constraints.Tcan_index_assign cia ->
        let key_ty = cia.Typing_defs_constraints.cia_key in
        let val_ty = cia.Typing_defs_constraints.cia_write in
        let as_data_ty =
          if as_data then
            let (_, _, index_expr) =
              cia.Typing_defs_constraints.cia_index_expr
            in
            match index_expr with
            | Aast.String s ->
              let field_name =
                TShapeField.make (TSFlit_str (Pos_or_decl.none, s))
              in
              Some
                (mk
                   ( r,
                     Tshape
                       (Shape_simple
                          {
                            s_origin = Missing_origin;
                            s_unknown_value = open_shape_unknown_value;
                            s_fields =
                              TShapeMap.singleton
                                field_name
                                { sft_optional = false; sft_ty = val_ty };
                          }) ))
            | Aast.Int _ -> Some (Typing_make_type.vec r val_ty)
            | _ -> None
          else
            None
        in
        (match as_data_ty with
        | Some ty -> Some ty
        | None -> Some (Typing_make_type.keyed_container r key_ty val_ty))
      | Typing_defs_constraints.Tcan_traverse ct ->
        let val_ty = ct.Typing_defs_constraints.ct_val in
        (match ct.Typing_defs_constraints.ct_key with
        | Some key_ty ->
          Some (Typing_make_type.keyed_traversable r key_ty val_ty)
        | None -> Some (Typing_make_type.traversable r val_ty))
      | Typing_defs_constraints.Tdestructure d ->
        Some
          (mk
             ( r,
               Ttuple
                 {
                   t_required = d.Typing_defs_constraints.d_required;
                   t_optional = d.Typing_defs_constraints.d_optional;
                   t_extra =
                     Tvariadic
                       (Option.value
                          d.Typing_defs_constraints.d_variadic
                          ~default:(Typing_make_type.nothing r));
                 } ))
      | _ -> None)
  in
  let rec is_uninformative ty =
    match get_node ty with
    | Tdynamic _ -> true
    | Tnonnull -> true
    | Tprim Aast.Tnull -> true
    | Toption ty' ->
      (match get_node ty' with
      | Tnonnull -> true
      | _ -> is_uninformative ty')
    | Tnewtype (n, _, bound) when String.equal n SN.Classes.cSupportDyn ->
      is_uninformative bound
    | Tunion tyl -> List.for_all tyl ~f:is_uninformative
    | _ -> false
  in
  let rec strip_like ty =
    match get_node ty with
    | Tunion tyl ->
      let tyl = List.filter tyl ~f:(fun t -> not (is_dynamic t)) in
      (match tyl with
      | [ty] -> strip_like ty
      | _ -> mk (get_reason ty, Tunion tyl))
    | _ -> ty
  in
  (* Track which shadow tyvars are currently being solved to break cycles.
     If solve_shadow is called for a tvid already being solved, we return
     mixed rather than recursing infinitely. *)
  let solving = ref Tvid.Set.empty in
  let rec substitute_shadows ty =
    match get_node ty with
    | Tvar v ->
      if Inf.is_shadow inf_env v then
        match solve_shadow v with
        | Some solved -> solved
        | None -> Typing_make_type.mixed (get_reason ty)
      else
        let (_, expanded) = Typing_env.expand_var env (get_reason ty) v in
        (match get_node expanded with
        | Tvar _ -> Typing_make_type.mixed (get_reason ty)
        | _ ->
          if is_uninformative expanded then
            Typing_make_type.mixed (get_reason ty)
          else
            substitute_shadows expanded)
    | Tdynamic (Some v) ->
      (match solve_shadow v with
      | Some solved -> solved
      | None -> Typing_make_type.mixed (get_reason ty))
    | Tdynamic None -> Typing_make_type.mixed (get_reason ty)
    | Tfun ft ->
      mk
        ( get_reason ty,
          Tfun
            {
              ft with
              ft_params =
                List.map ft.ft_params ~f:(fun p ->
                    { p with fp_type = substitute_shadows p.fp_type });
              ft_ret = substitute_shadows ft.ft_ret;
            } )
    | Tshape (Shape_simple shape) ->
      mk
        ( get_reason ty,
          Tshape
            (Shape_simple
               {
                 shape with
                 s_unknown_value = substitute_shadows shape.s_unknown_value;
                 s_fields =
                   TShapeMap.map
                     (fun sft ->
                       { sft with sft_ty = substitute_shadows sft.sft_ty })
                     shape.s_fields;
               }) )
    | Tshape (Shape_splat { ss_elems }) ->
      mk
        ( get_reason ty,
          Tshape
            (Shape_splat { ss_elems = List.map ss_elems ~f:substitute_shadows })
        )
    | Tclass (name, exact, tyl) ->
      mk
        (get_reason ty, Tclass (name, exact, List.map tyl ~f:substitute_shadows))
    | _ -> ty
  and solve_shadow v =
    if Tvid.Set.mem v !solving then
      Some (Typing_make_type.mixed r)
    else begin
      solving := Tvid.Set.add v !solving;
      let result =
        let upper_bounds = Inf.get_tyvar_upper_bounds inf_env v in
        let bounds = Internal_type_set.elements upper_bounds in
        let has_structural =
          List.exists bounds ~f:(fun ity ->
              match ity with
              | Typing_defs_constraints.ConstraintType _ -> true
              | _ -> false)
        in
        let bounds =
          if has_structural then
            List.filter bounds ~f:(fun ity ->
                match ity with
                | Typing_defs_constraints.ConstraintType _ -> true
                | _ -> false)
          else
            bounds
        in
        let tys = List.filter_map bounds ~f:constraint_to_type in
        let tys = List.map tys ~f:strip_like in
        let tys = List.filter tys ~f:(fun ty -> not (is_uninformative ty)) in
        let tys = List.map tys ~f:substitute_shadows in
        match tys with
        | [] -> Some (Typing_make_type.mixed r)
        | [ty] -> Some ty
        | tys ->
          let (_env, ty) = Typing_intersection.intersect_list env r tys in
          Some ty
      in
      result
    end
  in
  List.map dynamic_locals ~f:(fun dl ->
      {
        local_name = dl.Tast.dl_name;
        local_pos = dl.Tast.dl_pos;
        shadow_tvid = dl.Tast.dl_shadow_tvid;
        inferred_ty = solve_shadow dl.Tast.dl_shadow_tvid;
      })

let solution_to_json env sol =
  let ty_str =
    match sol.inferred_ty with
    | Some ty -> Tast_env.print_ty env ty
    | None -> "dynamic"
  in
  `Assoc
    [
      ("name", `String sol.local_name);
      ("pos", `String (Pos.string (Pos.to_absolute sol.local_pos)));
      ("type", `String ty_str);
    ]
