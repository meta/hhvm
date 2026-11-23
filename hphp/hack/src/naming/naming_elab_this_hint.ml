(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude

module Env = struct
  let forbid_this
      Naming_phase_env.{ elab_this_hint = Elab_this_hint.{ forbid_this; _ }; _ }
      =
    forbid_this

  let in_req_extends
      Naming_phase_env.
        { elab_this_hint = Elab_this_hint.{ in_req_extends; _ }; _ } =
    in_req_extends

  let in_extends
      Naming_phase_env.{ elab_this_hint = Elab_this_hint.{ in_extends; _ }; _ }
      =
    in_extends

  let is_top_level_haccess_root
      Naming_phase_env.
        { elab_this_hint = Elab_this_hint.{ is_top_level_haccess_root; _ }; _ }
      =
    is_top_level_haccess_root

  let in_interface
      Naming_phase_env.
        { elab_this_hint = Elab_this_hint.{ in_interface; _ }; _ } =
    in_interface

  let in_invariant_final
      Naming_phase_env.
        { elab_this_hint = Elab_this_hint.{ in_invariant_final; _ }; _ } =
    in_invariant_final

  let set_forbid_this t ~forbid_this =
    let elab_this_hint = t.Naming_phase_env.elab_this_hint in
    let elab_this_hint =
      Naming_phase_env.Elab_this_hint.{ elab_this_hint with forbid_this }
    in
    Naming_phase_env.{ t with elab_this_hint }

  let set_in_req_extends t ~in_req_extends =
    let elab_this_hint = t.Naming_phase_env.elab_this_hint in
    let elab_this_hint =
      Naming_phase_env.Elab_this_hint.{ elab_this_hint with in_req_extends }
    in
    Naming_phase_env.{ t with elab_this_hint }

  let set_in_extends t ~in_extends =
    let elab_this_hint = t.Naming_phase_env.elab_this_hint in
    let elab_this_hint =
      Naming_phase_env.Elab_this_hint.{ elab_this_hint with in_extends }
    in
    Naming_phase_env.{ t with elab_this_hint }

  let set_is_top_level_haccess_root t ~is_top_level_haccess_root =
    let elab_this_hint = t.Naming_phase_env.elab_this_hint in
    let elab_this_hint =
      Naming_phase_env.Elab_this_hint.
        { elab_this_hint with is_top_level_haccess_root }
    in
    Naming_phase_env.{ t with elab_this_hint }

  let set_in_interface t ~in_interface =
    let elab_this_hint = t.Naming_phase_env.elab_this_hint in
    let elab_this_hint =
      Naming_phase_env.Elab_this_hint.{ elab_this_hint with in_interface }
    in
    Naming_phase_env.{ t with elab_this_hint }

  let set_in_invariant_final t ~in_invariant_final =
    let elab_this_hint = t.Naming_phase_env.elab_this_hint in
    let elab_this_hint =
      Naming_phase_env.Elab_this_hint.{ elab_this_hint with in_invariant_final }
    in
    Naming_phase_env.{ t with elab_this_hint }
end

let on_class_ c ~ctx =
  let in_interface =
    match c.Aast.c_kind with
    | Ast_defs.Cinterface -> true
    | _ -> false
  in
  let in_invariant_final =
    if c.Aast.c_final then
      match c.Aast.c_tparams with
      | [] -> true
      | tps ->
        let f Aast.{ tp_variance; _ } =
          match tp_variance with
          | Ast_defs.Invariant -> true
          | _ -> false
        in
        List.for_all ~f tps
    else
      false
  in
  let ctx =
    Env.set_in_invariant_final ~in_invariant_final
    @@ Env.set_in_interface ~in_interface ctx
  in
  (ctx, Ok c)

let on_class_c_tparams c_tparams ~ctx =
  let ctx = Env.set_forbid_this ctx ~forbid_this:true in
  (ctx, Ok c_tparams)

let on_class_c_extends c_extends ~ctx =
  let ctx = Env.set_in_extends ~in_extends:true ctx in
  (ctx, Ok c_extends)

let on_class_c_uses c_uses ~ctx =
  let ctx = Env.set_forbid_this ctx ~forbid_this:false in
  (ctx, Ok c_uses)

let on_class_c_xhp_attrs c_xhp_attrs ~ctx =
  let ctx = Env.set_forbid_this ctx ~forbid_this:false in
  (ctx, Ok c_xhp_attrs)

let on_class_c_xhp_attr_uses c_xhp_attr_uses ~ctx =
  let ctx = Env.set_forbid_this ctx ~forbid_this:false in
  (ctx, Ok c_xhp_attr_uses)

let on_class_c_reqs c_reqs ~ctx =
  let ctx = Env.set_forbid_this ctx ~forbid_this:false in
  (ctx, Ok c_reqs)

let on_class_req ((_, require_kind) as elem) ~ctx =
  let in_req_extends =
    match require_kind with
    | Aast.RequireExtends -> true
    | _ -> false
  in
  let ctx = Env.set_in_req_extends ~in_req_extends ctx in
  (ctx, Ok elem)

let on_class_c_implements c_implements ~ctx =
  let ctx = Env.set_forbid_this ctx ~forbid_this:false in
  (ctx, Ok c_implements)

let on_class_var_cv_type cv_type ~ctx =
  let ctx = Env.set_forbid_this ~forbid_this:false ctx in
  (ctx, Ok cv_type)

let on_fun_f_ret f_ret ~ctx =
  let ctx = Env.set_forbid_this ~forbid_this:false ctx in
  (ctx, Ok f_ret)

let on_expr_ expr_ ~ctx =
  let ctx =
    match expr_ with
    | Aast.(Cast _ | Is _ | As _ | Upcast _) ->
      Env.set_forbid_this ~forbid_this:false ctx
    | _ -> ctx
  in
  (ctx, Ok expr_)

(* We want to disallow `this` hints in:
   - _class_ and _abstract class_ type parameters
   - `extends` and `require extends` clauses _unless_ it appears as the
     top-level root of a type access *)
let on_hint on_error hint ~ctx =
  let forbid_in_extends =
    (Env.in_req_extends ctx || Env.in_extends ctx)
    && (not @@ Env.in_interface ctx)
    && (not @@ Env.is_top_level_haccess_root ctx)
    && (not @@ Env.in_invariant_final ctx)
  in
  match hint with
  | (pos, Aast.Hthis) when Env.forbid_this ctx || forbid_in_extends ->
    on_error
    @@ Naming_phase_error.naming
    @@ Naming_error.This_type_forbidden
         {
           pos;
           in_extends = Env.in_extends ctx;
           in_req_extends = Env.in_req_extends ctx;
         };
    (ctx, Ok hint)
  | (_, Aast.Haccess _) ->
    let ctx =
      Env.set_is_top_level_haccess_root ~is_top_level_haccess_root:true ctx
    in
    (ctx, Ok hint)
  | _ ->
    let ctx =
      Env.set_is_top_level_haccess_root ~is_top_level_haccess_root:false ctx
    in
    (ctx, Ok hint)

let on_shape_field_info sfi ~ctx =
  let ctx = Env.set_forbid_this ~forbid_this:false ctx in
  (ctx, Ok sfi)

let on_hint_fun_hf_return_ty hf_return_ty ~ctx =
  let ctx = Env.set_forbid_this ~forbid_this:false ctx in
  (ctx, Ok hf_return_ty)

let on_targ targ ~ctx =
  let ctx = Env.set_forbid_this ~forbid_this:false ctx in
  (ctx, Ok targ)

let pass on_error =
  let id = Aast.Pass.identity () in
  Naming_phase_pass.top_down
    Aast.Pass.
      {
        id with
        on_ty_class_ = Some on_class_;
        on_fld_class__c_tparams = Some on_class_c_tparams;
        on_fld_class__c_extends = Some on_class_c_extends;
        on_fld_class__c_uses = Some on_class_c_uses;
        on_fld_class__c_xhp_attrs = Some on_class_c_xhp_attrs;
        on_fld_class__c_xhp_attr_uses = Some on_class_c_xhp_attr_uses;
        on_fld_class__c_reqs = Some on_class_c_reqs;
        on_ty_class_req = Some on_class_req;
        on_fld_class__c_implements = Some on_class_c_implements;
        on_fld_class_var_cv_type = Some on_class_var_cv_type;
        on_fld_fun__f_ret = Some on_fun_f_ret;
        on_ty_expr_ = Some on_expr_;
        on_ty_hint = Some (on_hint on_error);
        on_ty_shape_field_info = Some on_shape_field_info;
        on_fld_hint_fun_hf_return_ty = Some on_hint_fun_hf_return_ty;
        on_ty_targ = Some on_targ;
      }
