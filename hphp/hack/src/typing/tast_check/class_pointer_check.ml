(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude
open Aast
module Env = Tast_env
module SN = Naming_special_names
module ClassPointer = Typing_class_pointers

(** A place to consolidate some static class pointer checks before inference
 * takes care of them all. *)

(** Report [C::class] in [e], or anywhere in a vec, dict, tuple or shape literal
 * [e], when [e] is compared with a value of [other_ty] that may hold a string:
 * HHVM converts the class pointer to its name there, so [nameof C] says what
 * is meant. *)
let check_compared_class_consts env other_ty e =
  let may_hold_string =
    lazy (snd (ClassPointer.may_hold_string env other_ty))
  in
  let rec check ((_, pos, e_) : Tast.expr) =
    match e_ with
    | Class_const ((_, _, cid_), (_, name))
      when String.(name = SN.Members.mClass) -> begin
      match cid_ with
      | CIreified _
      | CIexpr _ ->
        ()
      | CIparent
      | CIself
      | CIstatic
      | CI _ ->
        if Lazy.force may_hold_string then
          ClassPointer.error_at_cls_const_expr
            env
            pos
            (ClassPointer.string_of_class_id_ cid_)
    end
    (* Keys of dicts and elements of keysets are reported as array keys *)
    | ValCollection ((_, (Vec | Vector | ImmVector)), _, exprs)
    | Tuple exprs ->
      List.iter exprs ~f:check
    | KeyValCollection (_, _, fields) ->
      List.iter fields ~f:(fun (_key, value) -> check value)
    | Shape fields ->
      List.iter fields ~f:(function
          | SF_field (_name, e) -> check e
          | SF_splat _ -> ())
    | _ -> ()
  in
  check e

let handler =
  object
    inherit Tast_visitor.handler_base

    method! at_class_const env cc =
      match cc.cc_kind with
      | CCConcrete (_ty, pos, Class_const ((_cty, _cpos, cid_), (_, name)))
        when String.(name = SN.Members.mClass) ->
        (match Env.get_self_id env with
        | Some e when Env.is_enum env e ->
          let Equal = Tast_env.eq_typing_env in
          ClassPointer.error_at_cls_const_expr
            env
            pos
            (ClassPointer.string_of_class_id_ cid_)
        | _ -> ())
      | _ -> ()

    method! at_expr env e =
      let Equal = Tast_env.eq_typing_env in
      let error = function
        | (_ty, pos, Class_const ((_cty, _cpos, cid_), (_, name)))
          when String.(name = SN.Members.mClass) ->
          ClassPointer.error_at_cls_const_expr
            env
            pos
            (ClassPointer.string_of_class_id_ cid_)
        | (ty, pos, _) ->
          let open Typing_defs in
          let ty_str = Typing_print.error env ty in
          (match get_node ty with
          | Tclass_ptr _
            when Typechecker_options.class_pointer_ban_class_array_key
                   env.genv.tcopt ->
            ClassPointer.error_at_cls_ptr_type env pos ty_str
          | _ -> ())
      in
      match e with
      (* $d[C::class] *)
      | (_, _, Array_get (_, Some expr)) -> error expr
      (* dict[C::class => ...] *)
      | (_, _, KeyValCollection (_, _, fields)) ->
        List.iter ~f:(fun (key, _val) -> error key) fields
      (* keyset[C::class] *)
      | (_, _, ValCollection ((_, (Set | ImmSet | Keyset)), _, exprs)) ->
        List.iter ~f:error exprs
      (* $s === C::class *)
      | ( _,
          _,
          Binop
            {
              bop = Ast_defs.(Eqeq | Eqeqeq | Diff | Diff2);
              lhs = (lhs_ty, _, _) as lhs;
              rhs = (rhs_ty, _, _) as rhs;
            } ) ->
        check_compared_class_consts env rhs_ty lhs;
        check_compared_class_consts env lhs_ty rhs
      | _ -> ()

    (* switch ($s) { case C::class: ... } compares with == *)
    method! at_stmt env (_, stmt) =
      let Equal = Tast_env.eq_typing_env in
      match stmt with
      | Switch ((scrutinee_ty, _, _), cases, _) ->
        List.iter cases ~f:(fun (case, _) ->
            check_compared_class_consts env scrutinee_ty case)
      | _ -> ()
  end
