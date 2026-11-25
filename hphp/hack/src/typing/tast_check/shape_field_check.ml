(*
 * Copyright (c) 2018, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude
open Aast
open Typing_defs
module SN = Naming_special_names

(* Returns the status `DoesExists, `DoesNotExist or `Unknown along with
 * a boolean to track if the shape was optional or not.
 *)
let shapes_key_exists env shape field_name =
  let check env pos shape_kind fields =
    match TShapeMap.find_opt field_name fields with
    | None ->
      if is_nothing shape_kind then
        `DoesNotExist (pos, `Undefined)
      else
        `Unknown
    | Some { sft_optional; sft_ty } ->
      if not sft_optional then
        `DoesExist (get_pos sft_ty)
      else
        let nothing = Typing_make_type.nothing Reason.none in
        if Tast_env.is_sub_type env sft_ty nothing then
          `DoesNotExist
            ( pos,
              `Nothing
                (lazy (Reason.to_string "It is nothing" (get_reason sft_ty))) )
        else
          `Unknown
  in
  let rec loop env shape was_optional =
    let shape = Tast_env.strip_dynamic env shape in
    let (_, shape) = Tast_env.strip_supportdyn env shape in
    let (env, shape) = Tast_env.expand_type env shape in
    match get_node shape with
    | Tshape
        (Shape_simple
          { s_origin = _; s_unknown_value = shape_kind; s_fields = fields }) ->
      (check env (get_pos shape) shape_kind fields, was_optional)
    | Tshape (Shape_splat _) -> (`Unknown, was_optional)
    | Toption maybe_shape ->
      let (env, shape) = Tast_env.expand_type env maybe_shape in
      loop env shape true
    | _ -> (`Unknown, was_optional)
  in
  loop env shape false

let trivial_shapes_key_exists_check pos1 env shape field_name =
  let (status, optional) =
    shapes_key_exists env shape (TShapeField.make (TSFlit_str field_name))
  in
  (* Shapes::keyExists only supports non optional shapes, so an error
   * would already have been raised.
   *)
  if not optional then
    match status with
    | `DoesExist decl_pos ->
      let Equal = Tast_env.eq_typing_env in
      Typing_error_utils.add_typing_error
        ~env
        Typing_error.(
          shape
          @@ Primary.Shape.Shapes_key_exists_always_true
               { pos = pos1; field_name = snd field_name; decl_pos })
    | `DoesNotExist (decl_pos, reason) ->
      let Equal = Tast_env.eq_typing_env in
      Typing_error_utils.add_typing_error
        ~env
        Typing_error.(
          shape
          @@ Primary.Shape.Shapes_key_exists_always_false
               { pos = pos1; field_name = snd field_name; decl_pos; reason })
    | `Unknown -> ()

let shapes_method_access_with_non_existent_field
    pos1 env method_name shape field_name =
  let (status, optional_shape) =
    shapes_key_exists env shape (TShapeField.make (TSFlit_str field_name))
  in
  match (status, optional_shape) with
  | (`DoesExist _, false) ->
    Lint.shape_idx_access_required_field pos1 (snd field_name)
  | (`DoesNotExist (decl_pos, reason), _) ->
    let Equal = Tast_env.eq_typing_env in
    Typing_error_utils.add_typing_error
      ~env
      Typing_error.(
        shape
        @@ Primary.Shape.Shapes_method_access_with_non_existent_field
             {
               pos = pos1;
               field_name = snd field_name;
               decl_pos;
               method_name;
               reason;
             })
  | (`DoesExist _, true)
  | (`Unknown, _) ->
    ()

let shape_access_with_non_existent_field pos1 env shape field_name =
  let (status, optional) =
    shapes_key_exists env shape (TShapeField.make (TSFlit_str field_name))
  in
  match (status, optional) with
  | (`DoesNotExist (decl_pos, reason), _) ->
    let Equal = Tast_env.eq_typing_env in
    Typing_error_utils.add_typing_error
      ~env
      Typing_error.(
        shape
        @@ Primary.Shape.Shapes_access_with_non_existent_field
             { pos = pos1; field_name = snd field_name; decl_pos; reason })
  | (`DoesExist _, _)
  | (`Unknown, _) ->
    ()

let dest_args arg1 arg2 =
  match (Aast_utils.arg_to_expr arg1, Aast_utils.arg_to_expr arg2) with
  | ((ty, _, _), (_, pos, String field_name)) -> Some (ty, pos, field_name)
  | _ -> None

let is_shape env ty =
  let (env, ty) = Tast_env.intersect_with_nonnull env (get_pos ty) ty in
  let shape =
    Typing_make_type.(locl_like Reason.none (top_shape Reason.none))
  in
  Tast_env.is_sub_type_for_union env ty shape
  && not (Tast_env.is_sub_type env ty (Typing_make_type.dynamic Reason.none))

let check_trait_self_shape_key (env : Tast_env.env) expr =
  let report field =
    let Equal = Tast_env.eq_typing_env in
    Typing_shapes.check_trait_self_shape_key env field
  in
  match expr with
  | ( _,
      _,
      Array_get
        ( (ty, _, _),
          Some
            ((_, _, (Class_const ((_, _, CIself), _) | Nameof (_, _, CIself)))
            as field) ) )
    when is_shape env ty ->
    report field
  | ( _,
      _,
      Call
        {
          func =
            (_, _, Class_const ((_, _, CI (_, class_name)), (_, method_name)));
          args = _ :: field :: _;
          unpacked_arg = None;
          _;
        } )
    when (String.equal class_name SN.Shapes.cShapes
         || String.equal class_name SN.Shapes.cReadonlyShapes)
         && List.exists
              [
                SN.Shapes.at;
                SN.Shapes.idx;
                SN.Shapes.keyExists;
                SN.Shapes.put;
                SN.Shapes.removeKey;
              ]
              ~f:(String.equal method_name) ->
    report (Aast_utils.arg_to_expr field)
  | _ -> ()

let handler =
  object
    inherit Tast_visitor.handler_base

    method! at_expr env expr =
      check_trait_self_shape_key env expr;
      match expr with
      | ( _,
          p,
          Call
            {
              func =
                ( _,
                  _,
                  Class_const ((_, _, CI (_, class_name)), (_, method_name)) );
              args = [arg1; arg2];
              unpacked_arg = None;
              _;
            } )
        when String.equal class_name SN.Shapes.cShapes
             && String.equal method_name SN.Shapes.keyExists ->
        (match dest_args arg1 arg2 with
        | Some (ty, pos, field_name) ->
          let (env, tyl) =
            Tast_env.get_concrete_supertypes env ty ~abstract_enum:false
          in
          let field = (Pos_or_decl.of_raw_pos pos, field_name) in
          let trivial_shapes_key_exists_check ty =
            trivial_shapes_key_exists_check p env ty field
          in
          List.iter tyl ~f:trivial_shapes_key_exists_check
        | None -> ())
      | ( _,
          _,
          Call
            {
              func =
                ( _,
                  _,
                  Class_const ((_, _, CI (_, class_name)), (_, method_name)) );
              args = [arg1; arg2; _];
              unpacked_arg = None;
              _;
            } )
      | ( _,
          _,
          Call
            {
              func =
                ( _,
                  _,
                  Class_const ((_, _, CI (_, class_name)), (_, method_name)) );
              args = [arg1; arg2];
              unpacked_arg = None;
              _;
            } )
        when String.equal class_name SN.Shapes.cShapes
             && (String.equal method_name SN.Shapes.idx
                || String.equal method_name SN.Shapes.at) ->
        (match dest_args arg1 arg2 with
        | Some (ty, pos, field_name) ->
          let (env, tyl) =
            Tast_env.get_concrete_supertypes env ty ~abstract_enum:false
          in
          let field = (Pos_or_decl.of_raw_pos pos, field_name) in
          let shapes_method_access_with_non_existent_field ty =
            shapes_method_access_with_non_existent_field
              pos
              env
              method_name
              ty
              field
          in
          List.iter tyl ~f:shapes_method_access_with_non_existent_field
        | None -> ())
      | (_, p, Binop { bop = Ast_defs.QuestionQuestion; lhs = exp; _ }) ->
        let rec check_nested_accesses = function
          | (_, _, Array_get (((ty, _, _) as exp), Some indexing_expr)) ->
            (match indexing_expr with
            | (_, pos, String field_name) ->
              let (env, tyl) =
                Tast_env.get_concrete_supertypes env ty ~abstract_enum:false
              in
              let field = (Pos_or_decl.of_raw_pos pos, field_name) in
              let shape_access_with_non_existent_field ty =
                shape_access_with_non_existent_field p env ty field
              in
              List.iter tyl ~f:shape_access_with_non_existent_field
            | _ -> ());
            check_nested_accesses exp
          | _ -> ()
        in
        check_nested_accesses exp
      | _ -> ()
  end
