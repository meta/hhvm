(*
 * Copyright (c) 2015, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

[@@@warning "-33"]

open Hh_prelude

[@@@warning "+33"]

open Common
open Typing_defs
module Env = Typing_env
module Reason = Typing_reason
module TUtils = Typing_utils
module MakeType = Typing_make_type
module Cls = Folded_class

let require_shape_key_attribute cls class_const =
  (not class_const.cc_synthesized)
  && (not class_const.cc_is_shape_key)
  && (not (Ast_defs.is_c_enum (Cls.kind cls)))
  && not (Ast_defs.is_c_enum_class (Cls.kind cls))

let shape_key_attribute_error env pos cls const_name class_const =
  if
    Typechecker_options.require_shape_key_attribute (Env.get_tcopt env)
    && require_shape_key_attribute cls class_const
  then
    Some
      Typing_error.(
        shape
        @@ Primary.Shape.Shape_field_class_const_missing_shape_key_attribute
             {
               pos;
               decl_pos = class_const.cc_pos;
               class_name = Utils.strip_ns class_const.cc_origin;
               const_name;
             })
  else
    None

let check_trait_self_shape_key env (_, pos, field) =
  let key_name =
    match field with
    | Aast.Class_const ((_, _, Aast.CIself), (_, const_name))
      when not (String.equal const_name "class") ->
      Some ("self::" ^ const_name)
    | Aast.Nameof (_, _, Aast.CIself) -> Some "nameof self"
    | _ -> None
  in
  Option.iter key_name ~f:(fun key_name ->
      match Env.get_self_class env with
      | Decl_entry.Found class_ when Ast_defs.is_c_trait (Cls.kind class_) ->
        let custom_err_config =
          Typechecker_options.custom_error_config (Env.get_tcopt env)
        in
        Diagnostics.add_diagnostic
          (Naming_error_utils.to_user_diagnostic
             (Naming_error.Trait_self_shape_key { pos; key_name })
             custom_err_config)
      | Decl_entry.Found _
      | Decl_entry.DoesNotExist
      | Decl_entry.NotYetAvailable ->
        ())

let widen_for_refine_shape ~expr_pos field_name env ty =
  Typing_log.(
    log_with_level env "typing" ~level:1 (fun () ->
        log_types
          (Pos_or_decl.of_raw_pos expr_pos)
          env
          [Log_head ("widen_for_refine_shape", [Log_type ("ty", ty)])]));
  match deref ty with
  | ( r,
      Tshape
        (Shape_simple
          { s_origin = _; s_unknown_value = shape_kind; s_fields = fields }) )
    -> begin
    match TShapeMap.find_opt field_name fields with
    | None ->
      let (env, element_ty) = Env.fresh_type_invariant env expr_pos in
      let sft = { sft_optional = true; sft_ty = element_ty } in
      ( env,
        Some
          (mk
             ( r,
               Tshape
                 (Shape_simple
                    {
                      s_origin = Missing_origin;
                      s_unknown_value = shape_kind;
                      s_fields = TShapeMap.add field_name sft fields;
                    }) )) )
    | Some _ -> (env, Some ty)
  end
  | _ -> (env, None)

let tshape_field_name_with_ty_err env (_, pos, field) =
  let resolve_self env =
    let this =
      match Env.get_self_ty env with
      | None -> None
      | Some c_ty ->
        (match get_node c_ty with
        | Tclass (sid, _, _) -> Some sid
        | _ -> None)
    in
    match this with
    | Some sid -> Ok sid
    | None ->
      Error
        Typing_error.(primary @@ Primary.Expected_class { pos; suffix = None })
  in
  let p = Pos_or_decl.of_raw_pos pos in
  let map_pos = Tuple.T2.map_fst ~f:Pos_or_decl.of_raw_pos in
  let class_const_field sid y =
    let field_name = TSFclass_const (sid, map_pos y) in
    if not (Typechecker_options.require_shape_key_attribute (Env.get_tcopt env))
    then
      Ok field_name
    else
      let error =
        match Env.get_class env (snd sid) with
        | Decl_entry.Found cls ->
          Option.bind
            (Env.get_const env cls (snd y))
            ~f:(shape_key_attribute_error env pos cls (snd y))
        | Decl_entry.DoesNotExist
        | Decl_entry.NotYetAvailable ->
          None
      in
      (* A missing attribute does not prevent resolving the key. Preserve normal
         shape typing and refinement even when this diagnostic is suppressed. *)
      Option.iter error ~f:(Typing_error_utils.add_typing_error ~env);
      Ok field_name
  in
  match field with
  | Aast.Int name -> Ok (TSFregex_group (p, name))
  | Aast.String name -> Ok (TSFlit_str (p, name))
  | Aast.Class_const ((_, _, ((Aast.CI _ | Aast.CIself) as cid)), (_, "class"))
    ->
    Error
      Typing_error.(
        primary
        @@ Primary.Class_const_to_string
             { pos; cls_name = Typing_class_pointers.string_of_class_id_ cid })
  | Aast.Class_const ((_, _, Aast.CI x), y) -> class_const_field (map_pos x) y
  | Aast.Nameof (_, _, Aast.CI x) ->
    Ok (TSFclass_const (map_pos x, (p, "class")))
  | Aast.Class_const ((_, _, Aast.CIself), y) ->
    let self_id = resolve_self env in
    Result.bind ~f:(fun sid -> class_const_field sid y) self_id
  | Aast.Nameof (_, _, Aast.CIself) ->
    let self_id = resolve_self env in
    Result.map ~f:(fun sid -> TSFclass_const (sid, (p, "class"))) self_id
  | _ ->
    let err =
      Typing_error.Primary.Shape.(
        Invalid_shape_field_name { pos; is_empty = false })
    in
    Error (Typing_error.shape err)

let do_with_field_expr
    (type res)
    env
    (field : ('a, 'b) Aast.expr)
    ~(with_error : res)
    (do_f : tshape_field_name -> res) : res =
  match tshape_field_name_with_ty_err env field with
  | Error ty_err ->
    Typing_error_utils.add_typing_error ~env ty_err;
    with_error
  | Ok field_name -> do_f field_name

let mixed_for_refinement env r ty =
  let mixed = MakeType.mixed r in
  if TUtils.is_supportdyn env ty then
    MakeType.supportdyn r mixed
  else
    mixed

(** Refine a shape with the knowledge that field_name
  exists. We do this by intersecting with
 shape(field_name => mixed, ...) *)
let refine_key_exists field_name pos env shape =
  let ((env, e1), shape) =
    Typing_solver.expand_type_and_narrow
      ~description_of_expected:"a shape"
      env
      (widen_for_refine_shape ~expr_pos:pos field_name)
      pos
      shape
  in
  let mixed =
    let r =
      Reason.missing_optional_field
        (get_pos shape, TUtils.get_printable_shape_field_name field_name)
    in
    mixed_for_refinement env r shape
  in
  Option.iter ~f:(Typing_error_utils.add_typing_error ~env) e1;
  Typing_helpers.refine_and_simplify_intersection
    env
    ~is_class:false
    (Reason.witness pos)
    shape
    (MakeType.open_shape
       Reason.none
       ~kind:mixed
       (TShapeMap.singleton field_name { sft_optional = false; sft_ty = mixed }))

(** Returns the shape map value for `?'x' => nothing`,
  which is what we use to signify that x is unset. *)
let unset_field_shape_ty_entry pos field_name =
  {
    sft_optional = true;
    sft_ty =
      MakeType.nothing
        (Reason.unset_field
           (pos, TUtils.get_printable_shape_field_name field_name));
  }

(** Refine a shape with the knowledge that field_name
  does not exists. We do this by intersecting with
  shape(?field_name => nothing, ...) *)
let refine_not_key_exists field_name pos env shape_ty =
  let ((env, e1), shape_ty) =
    Typing_solver.expand_type_and_narrow
      ~description_of_expected:"a shape"
      env
      (widen_for_refine_shape ~expr_pos:pos field_name)
      pos
      shape_ty
  in
  Option.iter ~f:(Typing_error_utils.add_typing_error ~env) e1;
  let r = Reason.witness pos in
  Typing_helpers.refine_and_simplify_intersection
    env
    ~is_class:false
    r
    shape_ty
    (MakeType.open_shape
       (get_reason shape_ty)
       ~kind:(mixed_for_refinement env r shape_ty)
       (TShapeMap.singleton
          field_name
          (unset_field_shape_ty_entry pos field_name)))

let refine_handle_errors
    env
    (shape_ty : locl_ty)
    (field : ('a, 'b) Aast.expr)
    (refine_f :
      Typing_env_types.env ->
      shape_ty:locl_ty ->
      tshape_field_name ->
      (Typing_env_types.env * Typing_error.t option) * locl_ty) :
    Typing_env_types.env * locl_ty =
  let ((env, ty_err_opt), res) =
    do_with_field_expr
      env
      field
      ~with_error:((env, None), shape_ty)
      (refine_f env ~shape_ty)
  in
  Option.iter ~f:(Typing_error_utils.add_typing_error ~env) ty_err_opt;
  (env, res)

(* Mark [field_name] absent in a single concrete shape. A closed shape simply
   drops the field; an open shape gets an explicit [?field => nothing] entry so
   its unknown tail cannot re-supply the field. Returns [None] when there is
   nothing to do (a closed shape that doesn't have the field), leaving the
   element untouched. *)
let mark_field_absent pos field_name s =
  if is_nothing s.s_unknown_value then
    if TShapeMap.mem field_name s.s_fields then
      Some
        {
          s with
          s_origin = Missing_origin;
          s_fields = TShapeMap.remove field_name s.s_fields;
        }
    else
      None
  else
    Some
      {
        s with
        s_origin = Missing_origin;
        s_fields =
          TShapeMap.add
            field_name
            (unset_field_shape_ty_entry pos field_name)
            s.s_fields;
      }

let shape_type_proves_field_absent field_name = function
  | Shape_simple { s_fields; s_unknown_value; _ } ->
    (match TShapeMap.find_opt field_name s_fields with
    | Some { sft_optional = true; sft_ty } -> is_nothing sft_ty
    | Some _ -> false
    | None -> is_nothing s_unknown_value)
  | Shape_splat _ ->
    (* A residual splat (unresolved elements) cannot prove absence. *)
    false

(* Whether localized upper bound [bound] proves that [field_name] is absent:
   viewed as a shape, it either lists the field as [?f => nothing] or omits it
   while being closed. Since an opaque type is a subtype of its bound, a bound
   that cannot contain the field means the opaque type cannot either. Ah open shape
   or one that lists the field as required or optional-and-inhabited, does not
   prove absence. *)
let bound_proves_field_absent env bound field_name =
  let (env, bound) = Env.expand_type env bound in
  match get_node bound with
  | Tshape shape_ty ->
    let (env, _err, view) =
      Typing_shape_normalize.normalize_shape_type
        ~on_error:None
        (get_reason bound)
        shape_ty
        env
    in
    (match view with
    | Typing_shape_normalize.Normalized_shape shape_ty ->
      (env, shape_type_proves_field_absent field_name shape_ty)
    | Typing_shape_normalize.Normalized_union _
    | Typing_shape_normalize.Normalized_intersection _ ->
      (env, false)
    (* An uninhabited bound has no values, so the field is vacuously absent. *)
    | Typing_shape_normalize.Normalized_bottom -> (env, true))
  | _ -> (env, false)

let generic_bound_proves_field_absent env name field_name =
  List.fold
    (Typing_set.elements (Env.get_upper_bounds env name))
    ~init:(env, false)
    ~f:(fun (env, acc) b ->
      if acc then
        (env, true)
      else
        bound_proves_field_absent env b field_name)

(* Remove [field_name] from the elements of a shape splat.

   Merging is rightmost-wins, so marking the field absent on a single element can
   be undone by another element that supplies it (an [?f => nothing] unions back
   to required against any [f] on either side); we therefore edit every concrete
   element. An opaque element (a type parameter, type variable or nested splat)
   cannot be edited, and because [Shapes::removeKey] takes the shape [inout] its
   type is invariant, so we may not change that element either. The removal is
   therefore sound only when every opaque element provably lacks the field (a
   bound that is a closed shape without it, or lists it as [?f => nothing]).
   Otherwise the element might contain the field at runtime while the unchanged
   type still claims it, so we report an error and leave the shape unchanged.
   Concrete elements are edited as usual. *)
let remove_field_from_splat env pos ss_elems field_name =
  let (env, ss_elems) = List.map_env env ss_elems ~f:Env.expand_type in
  let (env, blocked) =
    List.fold ss_elems ~init:(env, false) ~f:(fun (env, blocked) ty ->
        if blocked then
          (env, true)
        else
          match get_node ty with
          | Tshape (Shape_simple _) -> (env, false)
          | Tgeneric name ->
            let (env, absent) =
              generic_bound_proves_field_absent env name field_name
            in
            (env, not absent)
          | Tnewtype (_, _, bound) ->
            let (env, absent) =
              bound_proves_field_absent env bound field_name
            in
            (env, not absent)
          | _ ->
            (* A type variable, nested splat or other non-editable element may
               contain the field; we cannot prove otherwise. *)
            (env, true))
  in
  if blocked then
    let err =
      Typing_error.(
        primary
        @@ Primary.Generic_unify
             {
               pos;
               msg =
                 Printf.sprintf
                   "Cannot remove field `%s`: a type parameter spread into this shape may contain it, so the removal cannot be represented (the shape is passed `inout`, making its type invariant). The type parameter's bound must guarantee the field is absent -- a closed shape without it, or `?%s => nothing`"
                   (TUtils.get_printable_shape_field_name field_name)
                   (TUtils.get_printable_shape_field_name field_name);
             })
    in
    (env, Some err, ss_elems)
  else
    let new_elems =
      List.map ss_elems ~f:(fun ty ->
          match deref ty with
          | (r, Tshape (Shape_simple s)) ->
            (match mark_field_absent pos field_name s with
            | Some s -> mk (r, Tshape (Shape_simple s))
            | None -> ty)
          | _ -> ty)
    in
    (env, None, new_elems)

let refine_handle_unions_dyn pos refine_f env ~shape_ty field_name :
    (Typing_env_types.env * Typing_error.t option) * locl_ty =
  let rec go ~supportdyn env shape_ty :
      (Typing_env_types.env * Typing_error.t option) * locl_ty =
    let ((env, e1), shape) =
      Typing_solver.expand_type_and_solve
        ~freshen:false
        ~description_of_expected:"a shape"
        env
        pos
        shape_ty
    in
    let (supportdyn2, env, stripped_shape) =
      TUtils.strip_supportdyn env shape
    in
    let supportdyn = supportdyn || supportdyn2 in
    match get_node stripped_shape with
    | Tshape
        (Shape_simple
          { s_origin = _; s_unknown_value = shape_kind; s_fields = fields }) ->
      let result = refine_f stripped_shape shape_kind fields field_name in
      ( (env, e1),
        if supportdyn then
          let r = get_reason result in
          MakeType.supportdyn r result
        else
          result )
    | Tshape (Shape_splat { ss_elems }) ->
      let (env, e2, new_elems) =
        remove_field_from_splat env pos ss_elems field_name
      in
      (* Removing a field can empty the element it was on, leaving `...shape()`
         (the merge identity) in the row, so rebuild through the canonical
         constructor rather than reassembling the splat by hand. Safe to
         ignore the error since removing a field can't introduce a malformed shape *)
      let (env, _e3, result) =
        Typing_shape_normalize.splat
          ~on_error:None
          ~reason:(Reason.witness pos)
          new_elems
          env
      in
      ( (env, Option.merge e1 e2 ~f:Typing_error.both),
        if supportdyn then
          MakeType.supportdyn (get_reason result) result
        else
          result )
    | Tunion tyl ->
      let ((env, e2), tyl) =
        List.map_env_ty_err_opt
          env
          tyl
          ~combine_ty_errs:Typing_error.multiple_opt
          ~f:(go ~supportdyn)
      in
      let tyl = List.filter tyl ~f:(fun ty -> not (is_nothing ty)) in
      let result = mk (Reason.witness pos, Tunion tyl) in
      ((env, Option.merge e1 e2 ~f:Typing_error.both), result)
    | _ ->
      ( (env, e1),
        if supportdyn then
          let r = get_reason shape in
          MakeType.supportdyn r shape
        else
          shape )
  in
  go ~supportdyn:false env shape_ty

let refine_handle_errors_unions_dyn env pos shape_ty field refine_f =
  refine_handle_errors env shape_ty field
  @@ refine_handle_unions_dyn pos refine_f

(** Remove a field from a shape. *)
let remove_key pos env shape_ty field =
  refine_handle_errors_unions_dyn env pos shape_ty field
  @@ fun _stripped_shape shape_kind fields field_name ->
  let s_fields =
    if is_nothing shape_kind then
      TShapeMap.remove field_name fields
    else
      TShapeMap.add
        field_name
        (unset_field_shape_ty_entry pos field_name)
        fields
  in
  mk
    ( Reason.witness pos,
      Tshape
        (Shape_simple
           { s_origin = Missing_origin; s_unknown_value = shape_kind; s_fields })
    )

(** Refine the type of a shape knowing that a call to Shapes::idx is not null.
  This means that the shape now has the field, and that the type for this
  field is not nullable.
  We stay quite liberal here: we add the field to the shape type regardless
  of whether this field can be here at all. Errors will anyway be raised
  elsewhere when typechecking the call to Shapes::idx. This allows for more
  useful typechecking of incomplete code (code in the process of being
  written). *)
let shapes_idx_not_null_with_ty_err field_p env ~shape_ty field =
  let ((env, e1), shape_ty) =
    Typing_solver.expand_type_and_narrow
      ~description_of_expected:"a shape"
      env
      (widen_for_refine_shape ~expr_pos:field_p field)
      field_p
      shape_ty
  in
  let refine_type env shape_ty =
    Typing_utils.map_supportdyn env shape_ty (fun env shape_ty ->
        let (env, shape_ty) = Env.expand_type env shape_ty in
        match deref shape_ty with
        | ( r,
            Tshape
              (Shape_simple
                { s_origin = _; s_fields = ftm; s_unknown_value = shape_kind })
          ) ->
          let (env, field_type) =
            let sft_ty =
              match TShapeMap.find_opt field ftm with
              | Some { sft_ty; _ } -> sft_ty
              | None -> shape_kind
            in
            let (env, sft_ty) =
              Typing_intersection.intersect_with_nonnull
                env
                (Pos_or_decl.of_raw_pos field_p)
                sft_ty
            in
            (env, { sft_optional = false; sft_ty })
          in
          let ftm = TShapeMap.add field field_type ftm in
          ( env,
            mk
              ( r,
                Tshape
                  (Shape_simple
                     {
                       s_origin = Missing_origin;
                       s_fields = ftm;
                       s_unknown_value = shape_kind;
                     }) ) )
        | _ ->
          (* This should be an error, but it is already raised when
             typechecking the call to Shapes::idx *)
          (env, shape_ty))
  in
  match deref shape_ty with
  | (r, Tunion tyl) ->
    let (env, tyl) = List.map_env env tyl ~f:refine_type in
    let (env, ty) = Typing_union.union_list env r tyl in
    ((env, e1), ty)
  | _ ->
    let (env, ty) = refine_type env shape_ty in
    ((env, None), ty)

let shapes_idx_not_null env shape_ty (fld : Nast.expr) =
  let (_, p, _) = fld in
  refine_handle_errors env shape_ty fld (shapes_idx_not_null_with_ty_err p)

(* /!\ This is not used fur typing Shapes::idx anymore,
 * only to type field access on the LHS of ??
 *
 * Typing rules for Shapes::idx
 *
 *     e : ?shape(?sfn => t, ...)
 *     ----------------------------
 *     Shapes::idx(e, sfn) : ?t
 *
 *  Typing rules when the shape has a like type:
 *
 *     e : ~?shape(?sfn => t, ...)
 *     ----------------------------
 *     Shapes::idx(e, sfn) : ~?t
 *)
let idx_without_default env ~expr_pos ~shape_pos shape_ty field_name =
  Typing_log.(
    log_with_level env "shapes" ~level:1 (fun () ->
        log_types
          (Pos_or_decl.of_raw_pos expr_pos)
          env
          [
            Log_head
              ( "Typing_shapes.idx_without_default",
                [
                  Log_type ("Shape type", shape_ty);
                  Log_head
                    ( Printf.sprintf "Field: %s" (TShapeField.name field_name),
                      [] );
                ] );
          ]));
  let (env, shape_ty) = Env.expand_type env shape_ty in
  let (env, res) = Env.fresh_type env expr_pos in
  let ((env, ty_err_opt), res) =
    let fake_super_shape_ty =
      let r = Reason.shape (shape_pos, "Shapes::idx") in
      (* Since this shape is only used as a supertype, it is safe to use mixed
         as the kind, regardless of --everything-sdt *)
      MakeType.open_shape
        r
        ~kind:(MakeType.mixed r)
        (TShapeMap.singleton field_name { sft_optional = true; sft_ty = res })
    in
    let nullable_super_shape = mk (Reason.none, Toption fake_super_shape_ty) in
    let super_shape =
      MakeType.locl_like (Reason.witness shape_pos) nullable_super_shape
    in
    let (env, ty_err_opt) =
      Typing_coercion.coerce_type
        shape_pos
        Reason.URparam
        env
        shape_ty
        super_shape
        Unenforced
        Typing_error.Callback.unify_error
    in
    let (env, res) = TUtils.union env res (MakeType.null Reason.none) in
    ((env, ty_err_opt), res)
  in
  Option.iter ty_err_opt ~f:(Typing_error_utils.add_typing_error ~env);
  let (env, stripped_shape_ty) =
    Typing_dynamic_utils.strip_dynamic env shape_ty
  in
  match get_node stripped_shape_ty with
  | Tnewtype (n, _, _)
    when String.equal n Naming_special_names.Classes.cSupportDyn ->
    let r = get_reason shape_ty in
    TUtils.make_supportdyn r env res
  | _ -> (env, res)

let to_collection env pos shape_ty res return_type =
  let mapper =
    object (self)
      inherit Type_mapper.tvar_expanding_type_mapper as super

      method! on_tshape env r s =
        match s with
        | Shape_splat { ss_elems } ->
          let (env, _err, result) =
            Typing_shape_normalize.merge ~on_error:None ss_elems env
          in
          (match result with
          | Typing_shape_normalize.Full (ty, _) -> self#on_type env ty
          | Typing_shape_normalize.Empty_shape _ ->
            self#on_tshape
              env
              r
              (Shape_simple
                 {
                   s_origin = Missing_origin;
                   s_unknown_value = MakeType.nothing r;
                   s_fields = TShapeMap.empty;
                 })
          | Typing_shape_normalize.Partial _
          | Typing_shape_normalize.Union _
          | Typing_shape_normalize.Intersection _ ->
            return_type
              env
              (get_reason res)
              (MakeType.arraykey r)
              (MakeType.mixed r))
        | Shape_simple
            { s_origin = _; s_unknown_value = shape_kind; s_fields = fdm } ->
          (* The key type is the union of the types of the known fields,
           * or arraykey if there may be unknown fields (open shape)
           *)
          let (env, key) =
            if not (TUtils.is_nothing env shape_kind) then
              (env, MakeType.arraykey r)
            else
              let keys = TShapeMap.keys fdm in
              let (env, keys) =
                List.map_env env keys ~f:(fun env key ->
                    match key with
                    | Typing_defs.TSFregex_group (p, _) ->
                      (env, MakeType.int (Reason.witness_from_decl p))
                    | Typing_defs.TSFlit_str (p, _) ->
                      (env, MakeType.string (Reason.witness_from_decl p))
                    | Typing_defs.TSFclass_const ((_, cid), (_, mid)) -> begin
                      match Env.get_class env cid with
                      | Decl_entry.Found class_ -> begin
                        match Env.get_const env class_ mid with
                        | Some const ->
                          let ((env, ty_err_opt), lty) =
                            Typing_phase.localize_no_subst
                              env
                              ~ignore_errors:true
                              const.cc_type
                          in
                          Option.iter
                            ~f:(Typing_error_utils.add_typing_error ~env)
                            ty_err_opt;
                          (env, lty)
                        | None -> Env.fresh_type_error env pos
                      end
                      | Decl_entry.DoesNotExist
                      | Decl_entry.NotYetAvailable ->
                        Env.fresh_type_error env pos
                    end)
              in
              Typing_union.union_list env r keys
          in
          (* The value type is the union of the types of the known fields together
           * with the type of the unknown fields (open shape, typically mixed or supportdyn<mixed>)
           *)
          let (env, value) =
            (* If the unknown fields have type mixed then that's the type of values: no need for union *)
            if TUtils.is_mixed env shape_kind then
              (env, shape_kind)
            else
              (* Otherwise first filter out subtypes (common case,
               * as unknown fields are likely supportdyn<mixed>)
               * and then construct the union.
               *)
              let values = TShapeMap.values fdm in
              let values =
                List.filter_map
                  ~f:(fun { sft_ty; _ } ->
                    if TUtils.is_sub_type env sft_ty shape_kind then
                      None
                    else
                      Some sft_ty)
                  values
              in
              Typing_union.union_list env r (shape_kind :: values)
          in
          return_type env (get_reason res) key value

      method! on_tunion env r tyl =
        let (env, tyl) = List.fold_map tyl ~init:env ~f:self#on_type in
        Typing_union.union_list env r tyl

      method! on_tintersection env r tyl =
        let (env, tyl) = List.fold_map tyl ~init:env ~f:self#on_type in
        Typing_intersection.intersect_list env r tyl

      method! on_type env ty =
        let ((env, _e1), ty) =
          Typing_solver.expand_type_and_solve
            ~description_of_expected:"a shape"
            env
            pos
            ty
        in
        match get_node ty with
        | Tdynamic _ ->
          (* This makes it so that to_collection on a dynamic value returns a dynamic
           * value instead of the standard dict<arraykey, mixed> declared in the HHI,
           * which would otherwise subsume any other inferred type due to covariance. *)
          (env, ty)
        | Tvar _
        | Tshape _
        | Tunion _
        | Tintersection _ ->
          super#on_type env ty
          (* Look through bound on newtype, including supportdyn. We assume that
           * supportdyn has already been pushed into the type of unknown fields *)
        | Tnewtype (c, tyl, _) ->
          let (env, ty) =
            Typing_utils.get_newtype_super env (get_reason ty) c tyl
          in
          super#on_type env ty
        | _ -> (env, res)
    end
  in
  mapper#on_type (Type_mapper.fresh_env env) shape_ty

let to_dict env pos shape_ty res =
  let supports_dynamic = TUtils.is_supportdyn_use_tyvar_bounds env shape_ty in
  let ((env, e1), shape_ty) =
    Typing_solver.expand_type_and_solve
      ~description_of_expected:"a shape"
      env
      pos
      shape_ty
  in
  Option.iter ~f:(Typing_error_utils.add_typing_error ~env) e1;
  let (env, shape_ty) =
    TUtils.get_base_type ~expand_supportdyn:true env shape_ty
  in
  let (env, result) =
    to_collection env pos shape_ty res (fun env r key value ->
        (env, MakeType.dict r key value))
  in
  if supports_dynamic then
    TUtils.make_supportdyn (get_reason result) env result
  else
    (env, result)

let shape_field_pos = function
  | Ast_defs.SFlit_str (p, _)
  | Ast_defs.SFclassname (p, _) ->
    p
  | Ast_defs.SFclass_const ((cls_pos, _), (mem_pos, _)) ->
    Pos.btw cls_pos mem_pos

let check_shape_key_attribute env pos cls const_name class_const =
  Option.iter
    (shape_key_attribute_error env pos cls const_name class_const)
    ~f:(Typing_error_utils.add_typing_error ~env)

let check_shape_keys_validity env keys =
  (* If the key is a class constant, get its class name and type. *)
  let get_field_info env key =
    let key_pos = shape_field_pos key in
    match key with
    | Ast_defs.SFlit_str (_, key_name) ->
      (if Int.equal 0 (String.length key_name) then
        Typing_error_utils.add_typing_error ~env
        @@ Typing_error.(
             shape
             @@ Primary.Shape.Invalid_shape_field_name
                  { pos = key_pos; is_empty = true }));
      (env, key_pos, None)
    | Ast_defs.SFclassname (p, cls) -> begin
      match Env.get_class env cls with
      | Decl_entry.DoesNotExist
      | Decl_entry.NotYetAvailable ->
        let (env, ty) = Env.fresh_type_error env p in
        (env, key_pos, Some (cls, ty))
      | Decl_entry.Found cd ->
        (* Match the sketchy Tany logic of C::class keys *)
        (match Env.get_const env cd "class" with
        | None ->
          let (env, ty) = Env.fresh_type_error env p in
          (env, key_pos, Some (cls, ty))
        | Some { cc_type; _ } ->
          let ((env, ty_err_opt), ty) =
            Typing_phase.localize_no_subst ~ignore_errors:true env cc_type
          in
          Option.iter ~f:(Typing_error_utils.add_typing_error ~env) ty_err_opt;
          (env, key_pos, Some (cls, ty)))
    end
    | Ast_defs.SFclass_const ((_p, cls), (p, y)) -> begin
      match Env.get_class env cls with
      | Decl_entry.DoesNotExist
      | Decl_entry.NotYetAvailable ->
        let (env, ty) = Env.fresh_type_error env p in
        (env, key_pos, Some (cls, ty))
      | Decl_entry.Found cd ->
        (match Env.get_const env cd y with
        | None ->
          Typing_error_utils.add_typing_error ~env
          @@ Typing_object_get.smember_not_found
               p
               ~is_const:true
               ~is_method:false
               ~is_function_pointer:false
               cd
               y
               Typing_error.Callback.unify_error;
          let (env, ty) = Env.fresh_type_error env p in

          (env, key_pos, Some (cls, ty))
        | Some ({ cc_type; cc_abstract; cc_pos; _ } as class_const) ->
          (match cc_abstract with
          | CCAbstract _ ->
            Typing_error_utils.add_typing_error ~env
            @@ Typing_error.(
                 shape
                 @@ Primary.Shape.Abstract_shape_field_const
                      {
                        pos = key_pos;
                        decl_pos = cc_pos;
                        name = cls ^ "::" ^ y;
                      })
          | CCConcrete -> ());
          check_shape_key_attribute env key_pos cd y class_const;
          let ((env, ty_err_opt), ty) =
            Typing_phase.localize_no_subst ~ignore_errors:true env cc_type
          in
          Option.iter ~f:(Typing_error_utils.add_typing_error ~env) ty_err_opt;
          let r = Reason.witness key_pos in
          let (env, e2) =
            TUtils.sub_type
              ~class_sub_classname:false
              env
              ty
              (MakeType.arraykey r)
            @@ Some
                 Typing_error.(
                   Reasons_callback.always
                   @@ shape
                   @@ Primary.Shape.Invalid_shape_field_type
                        {
                          pos = key_pos;
                          ty_pos = get_pos ty;
                          ty_name = lazy (Typing_print.error env ty);
                          trail = [];
                        })
          in
          Option.iter ~f:(Typing_error_utils.add_typing_error ~env) e2;
          (env, key_pos, Some (cls, ty)))
    end
  in
  let check_field witness_pos witness_info env key =
    let (env, key_pos, key_info) = get_field_info env key in
    let ty_errs =
      let open Typing_error in
      match (witness_info, key_info) with
      | (Some _, None) ->
        [
          shape
          @@ Primary.Shape.Invalid_shape_field_literal
               { pos = key_pos; witness_pos };
        ]
      | (None, Some _) ->
        [
          shape
          @@ Primary.Shape.Invalid_shape_field_const
               { pos = key_pos; witness_pos };
        ]
      | (None, None) -> []
      | (Some (cls1, ty1), Some (cls2, ty2)) ->
        List.filter_map
          ~f:Fn.id
          [
            (if TUtils.is_tyvar_error env ty1 || TUtils.is_tyvar_error env ty2
            then
              None
            else if String.( <> ) cls1 cls2 then
              Some
                (shape
                   (Primary.Shape.Shape_field_class_mismatch
                      {
                        pos = key_pos;
                        witness_pos;
                        class_name = Utils.strip_ns cls2;
                        witness_class_name = Utils.strip_ns cls1;
                      }))
            else
              None);
            (if TUtils.is_tyvar_error env ty1 || TUtils.is_tyvar_error env ty2
            then
              None
            else
              let (ty1_sub_ty2, e1) = Typing_solver.is_sub_type env ty1 ty2
              and (ty2_sub_ty1, e2) = Typing_solver.is_sub_type env ty2 ty1 in
              let e3 =
                if not (ty1_sub_ty2 && ty2_sub_ty1) then
                  Some
                    (shape
                       (Primary.Shape.Shape_field_type_mismatch
                          {
                            pos = key_pos;
                            witness_pos;
                            ty_name = lazy (Typing_print.error env ty2);
                            witness_ty_name = lazy (Typing_print.error env ty1);
                          }))
                else
                  None
              in
              Typing_error.multiple_opt @@ List.filter_map ~f:Fn.id [e1; e2; e3]);
          ]
    in
    let ty_err_opt = Typing_error.multiple_opt ty_errs in
    (env, ty_err_opt)
  in
  (* Sort the keys by their positions since the error messages will make
   * more sense if we take the one that appears first as canonical and if
   * they are processed in source order. *)
  let cmp_keys x y = Pos.compare (shape_field_pos x) (shape_field_pos y) in
  let keys = List.sort ~compare:cmp_keys keys in
  match keys with
  | [] -> (env, [])
  | witness :: rest_keys ->
    let (env, pos, info) = get_field_info env witness in
    List.fold_left rest_keys ~init:(env, []) ~f:(fun (env, ty_errs) k ->
        match check_field pos info env k with
        | (env, Some ty_err) -> (env, ty_err :: ty_errs)
        | (env, _) -> (env, ty_errs))

let update_param : decl_fun_param -> decl_ty -> decl_fun_param =
 (fun param ty -> { param with fp_type = ty })

(** For function Shapes::idx called with a literal
  field name, transform the decl function type from the hhi file
  into a type that is specific to the field.

  In the hhi file, the function has type
    Shapes::idx<Tv>(
      ?shape(...) $shape,
      arraykey $index,
      ?Tv,
    )[]: ?Tv;

  If there are two arguments, transform it to
    Shapes::idx<Tv>(
      ?shape('field_name' => Tv, ...) $shape,
      arraykey $index,
    )[]: ?Tv;

  If there are three arguments, transform it to
    Shapes::idx<Tv>(
      ?shape('field_name' => Tv, ...) $shape,
      arraykey $index,
      Tv $default,
    )[]: Tv;
 *)
let transform_idx_fun_ty (field_name : tshape_field_name) nargs fty =
  let (param1, param2, param3) =
    match fty.ft_params with
    | [param1; param2; param3] -> (param1, param2, param3)
    | _ -> failwith "Expected 3 parameters for Shapes::idx in hhi file"
  in
  let rret = get_reason fty.ft_ret in
  let field_ty : decl_ty =
    MakeType.generic (Reason.witness_from_decl param1.fp_pos) "Tv"
  in
  let (params, ret) =
    let r = Reason.witness_from_decl param1.fp_pos in
    let param1 =
      update_param
        param1
        (mk
           ( Reason.none,
             Toption
               (* It is safe to use mixed as the kind, regardless of --everything-sdt, since
                  the idx function doesn't look at any of the other fields besides the explicitly
                  specified one *)
               (MakeType.open_shape
                  r
                  ~kind:(MakeType.mixed r)
                  (TShapeMap.singleton
                     field_name
                     { sft_optional = true; sft_ty = field_ty })) ))
    in
    match nargs with
    | 2 ->
      (* Return type should be ?Tv *)
      let ret = MakeType.nullable rret (MakeType.generic rret "Tv") in
      ([param1; param2], ret)
    | 3 ->
      (* Third parameter should have type Tv *)
      let param3 =
        let r3 = get_reason param1.fp_type in
        update_param param3 (MakeType.generic r3 "Tv")
      in
      (* Return type should be Tv *)
      let ret = MakeType.generic rret "Tv" in
      ([param1; param2; param3], ret)
    (* Shouldn't happen! *)
    | _ -> (fty.ft_params, fty.ft_ret)
  in
  { fty with ft_params = params; ft_ret = ret }

(** For function Shapes::at called with a literal
  field name, transform the decl function type from the hhi file
  into a type that is specific to the field.

  In the hhi file, the function has type
     Shapes::at<Tv>(
       shape(...) $shape,
       arraykey $index,
     )[]: Tv;

  Transform it to
     Shapes::at<Tv>(
       shape('field_name' => Tv, ...) $shape,
       arraykey $index,
     )[]: Tv;
 *)
let transform_at_fun_ty (field_name : tshape_field_name) fty =
  let (param1, param2) =
    match fty.ft_params with
    | [param1; param2] -> (param1, param2)
    | _ -> failwith "Expected 2 parameters for Shapes::at in hhi file"
  in
  let params =
    (* Return type should be Tv already, but first parameter is just shape(...) *)
    let field_ty : decl_ty =
      MakeType.generic (Reason.witness_from_decl param1.fp_pos) "Tv"
    in
    let r = Reason.witness_from_decl param1.fp_pos in
    let param1 =
      update_param
        param1
        (* It is safe to use mixed as the kind, regardless of --everything-sdt, since
           the at function doesn't look at any of the other fields besides the explicitly
           specified one *)
        (MakeType.open_shape
           r
           ~kind:(MakeType.mixed r)
           (TShapeMap.singleton
              field_name
              { sft_optional = true; sft_ty = field_ty }))
    in
    [param1; param2]
  in
  { fty with ft_params = params }

(* For functions Shapes::idx or Shapes::at called with a literal
 * field name, transform the decl function type from the hhi file
 * into a type that is specific to the field.
 *)
let transform_special_shapes_fun_ty field_name id nargs fty =
  if String.equal (snd id) Naming_special_names.Shapes.at then
    transform_at_fun_ty field_name fty
  else if String.equal (snd id) Naming_special_names.Shapes.idx then
    transform_idx_fun_ty field_name nargs fty
  else
    fty
