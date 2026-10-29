(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)
open Hh_prelude
open Common
open Aast
open Typing_defs
open Typing_env_types
open Typing_helpers
module FunUtils = Decl_fun_utils
module Reason = Typing_reason
module Env = Typing_env
module MakeType = Typing_make_type
module Type = Typing_ops
module Phase = Typing_phase
module EnvFromDef = Typing_env_from_def
module TUtils = Typing_utils
module TCO = Typechecker_options
module Cls = Folded_class
module SN = Naming_special_names
module Profile = Typing_toplevel_profile
module Enable = Typing_toplevel_enable

(* emit a warning if a method is sealed to a class that does not override it *)
let check_sealed_override env cls m =
  let method_name = snd m.m_name in
  let is_sealed (attr : Nast.user_attribute) =
    String.equal (snd attr.ua_name) SN.UserAttributes.uaSealed
  in
  match List.find m.m_user_attributes ~f:is_sealed with
  | None -> ()
  | Some sealed_attr ->
    let iter_item ((_, pos, expr_) : Nast.expr) =
      match expr_ with
      | Class_const ((_, _, cid), _) ->
        let allowlist_class_name = Nast.class_id_to_str cid in
        (match Typing_env.get_class env allowlist_class_name with
        | Decl_entry.DoesNotExist
        | Decl_entry.NotYetAvailable ->
          ()
        | Decl_entry.Found allowlist_cls ->
          let (allowlist_class_kind, verb) =
            let open Ast_defs in
            match (Cls.kind allowlist_cls, Cls.kind cls) with
            | (Cclass _, Ctrait) -> ("Class", "use")
            | (Cclass _, _) -> ("Class", "extend")
            | (Ctrait, Ctrait) -> ("Trait", "use")
            | (Ctrait, _) -> ("Trait", "require extends")
            | (Cinterface, _)
            | (Cenum, _)
            | (Cenum_class _, _) ->
              ("", "")
          in
          if
            (not (Cls.has_ancestor allowlist_cls (Cls.name cls)))
            && not (Cls.requires_ancestor allowlist_cls (Cls.name cls))
          then
            Typing_warning_utils.add
              env
              ( pos,
                Typing_warning.Sealed_not_subtype,
                {
                  Typing_warning.Sealed_not_subtype.verb;
                  parent_name = Cls.name cls;
                  parent_method_name = Some method_name;
                  child_name = allowlist_class_name;
                  child_kind = allowlist_class_kind;
                } )
          else
            let method_decl_in_allowlist_class =
              if m.m_static then
                Typing_env.get_static_member true env allowlist_cls method_name
              else
                Typing_env.get_method env allowlist_cls (snd m.m_name)
            in
            (match method_decl_in_allowlist_class with
            | None ->
              (* the class in the sealed attribute does not define m, raise a warning *)
              Typing_warning_utils.add
                env
                ( pos,
                  Typing_warning.Sealed_not_override,
                  {
                    Typing_warning.Sealed_not_override.method_name;
                    method_def = Cls.name cls;
                    allowlist_class_name;
                    allowlist_class_kind;
                  } )
            | Some m_allowlist_class ->
              (* the method is defined in the allowlist_class
                 check that the method is defined by the class itself or inherited from a trait *)
              let is_origin_trait =
                match Env.get_class env m_allowlist_class.ce_origin with
                | Decl_entry.Found cls_origin ->
                  Cls.kind cls_origin |> Ast_defs.is_c_trait
                | Decl_entry.DoesNotExist
                | Decl_entry.NotYetAvailable ->
                  false
              in
              if
                not
                  (String.equal m_allowlist_class.ce_origin allowlist_class_name
                  || is_origin_trait)
              then
                Typing_warning_utils.add
                  env
                  ( pos,
                    Typing_warning.Sealed_not_override,
                    {
                      Typing_warning.Sealed_not_override.method_name;
                      method_def = Cls.name cls;
                      allowlist_class_name;
                      allowlist_class_kind;
                    } )))
      | _ -> ()
    in
    List.iter sealed_attr.ua_params ~f:iter_item

let method_dynamically_callable env cls m params_decl_ty return =
  let env = { env with checked = Tast.CUnderDynamicAssumptions } in
  let ret_locl_ty = return.Typing_env_return_info.return_type in
  (* Here the body of the method is typechecked again to ensure it is safe
   * to call it from a dynamic context (eg. under dyn..dyn->dyn assumptions).
   * The code below must be kept in sync with with the method_def checks.
   *)
  let make_dynamic pos = MakeType.dynamic (Reason.support_dynamic_type pos) in
  let dynamic_return_ty = make_dynamic (get_pos ret_locl_ty) in
  let dynamic_return_info =
    Typing_env_return_info.{ return with return_type = dynamic_return_ty }
  in
  let (env, param_tys) =
    Typing_param.make_param_local_tys
      ~dynamic_mode:true
      ~no_auto_likes:false
      env
      params_decl_ty
      m.m_params
  in
  let (env, dynamic_params) =
    Typing.bind_params ~no_auto_likes:false env m.m_ctxs param_tys m.m_params
  in
  let pos = fst m.m_name in
  let env = set_tyvars_variance_in_callable env dynamic_return_ty param_tys in
  let env = Typing_dynamic.add_require_dynamic_bounds env cls in
  let env =
    if Cls.get_support_dynamic_type cls then
      let this_local = Env.get_local env this in
      let this_ty =
        MakeType.intersection
          (Reason.support_dynamic_type Pos_or_decl.none)
          [this_local.Typing_local_types.ty; make_dynamic Pos_or_decl.none]
      in
      Env.set_local env this this_ty Pos.none
    else
      env
  in

  let disable =
    Naming_attributes.mem
      SN.UserAttributes.uaDisableTypecheckerInternal
      m.m_user_attributes
  in

  let (env, dynamic_body) =
    Diagnostics.try_with_result
      (fun () ->
        Typing.fun_
          ~abstract:m.m_abstract
          ~native:(Typing_native.is_native_meth ~env m)
          ~disable
          env
          dynamic_return_info
          pos
          m.m_body
          m.m_fun_kind)
      (fun env_and_dynamic_body error ->
        if not (TCO.silence_errors_under_dynamic env.genv.tcopt) then
          Diagnostics.method_is_not_dynamically_callable
            pos
            (snd m.m_name)
            (Cls.name cls)
            (Env.get_support_dynamic_type env)
            None
            (Some error);
        env_and_dynamic_body)
  in
  (env, dynamic_params, dynamic_body, dynamic_return_ty)

let method_return ~supportdyn env cls m ret_decl_ty =
  let hint_pos =
    match snd m.m_ret with
    | Some (hint_pos, _) -> hint_pos
    | None -> fst m.m_name
  in
  let default_ty =
    match ret_decl_ty with
    | None when String.equal (snd m.m_name) SN.Members.__construct ->
      Some (MakeType.void (Reason.witness (fst m.m_name)))
    | _ -> None
  in
  let ety_env =
    empty_expand_env_with_on_error
      (Env.invalid_type_hint_assert_primary_pos_in_current_decl env)
  in
  let (env, ret_ty) =
    Typing_return.make_return_type
      ~ety_env
      ~this_class:(Some cls)
      env
      ~supportdyn
      ~hint_pos
      ~explicit:ret_decl_ty
      ~default:default_ty
  in
  let return =
    Typing_return.make_info hint_pos m.m_fun_kind m.m_user_attributes env ret_ty
  in
  (env, return)

let method_def ~is_disposable env cls m =
  Worker_cancel.raise_if_stop_requested ();
  let tcopt = Env.get_tcopt env in
  Enable.if_matches_regexp tcopt (Some env) ~default:None m.m_name @@ fun () ->
  Profile.measure_elapsed_time_and_report tcopt (Some env) m.m_name @@ fun () ->
  Diagnostics.run_with_span m.m_span @@ fun () ->
  with_timeout env m.m_name @@ fun env ->
  let errs =
    FunUtils.check_params ~from_abstract_method:m.m_abstract m.m_params
  in
  List.iter ~f:(Typing_error_utils.add_typing_error ~env) errs;
  let method_name = Ast_defs.get_id m.m_name in
  let is_ctor = String.equal method_name SN.Members.__construct in
  let env =
    if is_ctor then
      Env.env_with_constructor_droot_member env
    else
      Env.env_with_method_droot_member env method_name ~static:m.m_static
  in
  let initial_env = env in
  let pos = fst m.m_name in
  let env = Env.open_tyvars env (fst m.m_name) in
  let env = Env.reinitialize_locals env in
  let env = Env.set_env_callable_pos env pos in
  let env = Env.set_env_function_pos env m.m_span in
  let (env, restore_pos) = Env.set_inference_env_pos env (Some pos) in
  let (env, user_attributes) =
    Typing.attributes_check_def env SN.AttributeKinds.mthd m.m_user_attributes
  in
  let env =
    if
      Naming_attributes.mem
        SN.UserAttributes.uaSupportDynamicType
        m.m_user_attributes
    then
      Env.set_support_dynamic_type env true
    else
      env
  in
  let env =
    let needs_concrete =
      Naming_attributes.mem
        SN.UserAttributes.uaNeedsConcrete
        m.m_user_attributes
    in
    Env.set_needs_concrete env needs_concrete
  in
  let no_auto_likes =
    match Cls.get_any_method ~is_static:m.m_static cls method_name with
    | Some ce -> get_ce_no_auto_likes ce
    | None ->
      Naming_attributes.mem SN.UserAttributes.uaNoAutoLikes m.m_user_attributes
  in
  let env =
    if no_auto_likes then
      Env.set_no_auto_likes env true
    else
      env
  in
  check_sealed_override env cls m;
  let env = Env.assert_packages_loaded_from_attr env m.m_user_attributes in
  let env = Env.set_soft_package_from_attr env m.m_user_attributes in
  let (env, cap_ty, unsafe_cap_ty) =
    Typing_coeffects.type_capability env m.m_ctxs m.m_unsafe_ctxs (fst m.m_name)
  in
  let (env, _) =
    Typing_coeffects.register_capabilities env cap_ty unsafe_cap_ty
  in

  let env = Env.set_fun_is_constructor env is_ctor in
  let (env, ty_err_opt) =
    Phase.localize_and_add_ast_generic_parameters_and_where_constraints
      env
      ~ignore_errors:false
      m.m_tparams
      m.m_where_constraints
  in
  Option.iter ~f:(Typing_error_utils.add_typing_error ~env) ty_err_opt;
  let env =
    match Env.get_self_ty env with
    | Some ty when not (Env.is_static env) ->
      Env.set_local env this (MakeType.this (get_reason ty)) Pos.none
    | _ -> env
  in
  let env =
    if is_disposable then
      Env.set_using_var env this
    else
      env
  in
  let env = Env.clear_params env in
  let (ret_decl_ty, params_decl_ty) =
    hint_fun_decl ~params:m.m_params ~ret:m.m_ret env
  in
  (* Is sound dynamic enabled and the function is
   * marked <<__SupportDynamicType>> explicitly or implicitly?
   * (Even constructors must be treated as SDT because they can be called
   * through a classname cast to dynamic).
   *)
  let sdt_method = Env.get_support_dynamic_type env in
  (* Does the body of the method need to be checked again under
   * dynamic assumptions? Note that if there are generic parameters
   * then the check would be done under different assumptions for
   * generics from the normal check, so we play safe and require
   * the second check.
   *)
  let sdt_dynamic_check_required =
    sdt_method
    && not
         (List.is_empty (Cls.tparams cls)
         && Typing_dynamic.function_parameters_safe_for_dynamic
              ~this_class:(Some cls)
              env
              params_decl_ty)
  in
  let env = Env.set_fn_kind env m.m_fun_kind in
  let (env, return) =
    method_return
      ~supportdyn:(sdt_method && not sdt_dynamic_check_required)
      env
      cls
      m
      ret_decl_ty
  in
  let sound_dynamic_check_saved_env = env in
  let no_auto_likes =
    Naming_attributes.mem SN.UserAttributes.uaNoAutoLikes m.m_user_attributes
  in
  let (env, param_tys) =
    Typing_param.make_param_local_tys
      ~dynamic_mode:false
      ~no_auto_likes
      env
      params_decl_ty
      m.m_params
  in
  Typing_memoize.check_method env m;
  let env = Typing_implemented_by.check_method env (pos, Cls.name cls) m in
  let can_read_globals =
    Typing_subtype.is_sub_type
      env
      cap_ty
      (MakeType.capability (get_reason cap_ty) SN.Capabilities.accessGlobals)
  in
  let (env, typed_params) =
    Typing.bind_params
      env
      ~can_read_globals
      ~no_auto_likes
      m.m_ctxs
      param_tys
      m.m_params
  in
  let ret_locl_ty = return.Typing_env_return_info.return_type in
  let env = set_tyvars_variance_in_callable env ret_locl_ty param_tys in
  let local_tpenv = Env.get_tpenv env in
  let disable =
    Naming_attributes.mem
      SN.UserAttributes.uaDisableTypecheckerInternal
      m.m_user_attributes
  in
  let ((env, tb), had_errors) =
    Diagnostics.run_and_check_for_errors (fun () ->
        Typing.fun_
          ~abstract:m.m_abstract
          ~native:(Typing_native.is_native_meth ~env m)
          ~disable
          env
          return
          pos
          m.m_body
          m.m_fun_kind)
  in
  let type_hint' =
    match hint_of_type_hint m.m_ret with
    | None when String.equal method_name SN.Members.__construct ->
      Some (pos, Hprim Tvoid)
    | None ->
      Typing_error_utils.add_typing_error
        ~env
        Typing_error.(primary @@ Primary.Expecting_return_type_hint pos);
      None
    | Some _ -> hint_of_type_hint m.m_ret
  in
  let m = { m with m_ret = (fst m.m_ret, type_hint') } in
  let (env, tparams) = List.map_env env m.m_tparams ~f:Typing.type_param in
  let (env, e1) = Typing_solver.close_tyvars_and_solve env in
  let (env, e2) = Typing_solver.solve_all_unsolved_tyvars env in

  let return_hint = hint_of_type_hint m.m_ret in
  let under_normal_assumptions =
    {
      Aast.m_annotation = Env.save local_tpenv env;
      Aast.m_span = m.m_span;
      Aast.m_final = m.m_final;
      Aast.m_static = m.m_static;
      Aast.m_abstract = m.m_abstract;
      Aast.m_visibility = m.m_visibility;
      Aast.m_readonly_this = m.m_readonly_this;
      Aast.m_name = m.m_name;
      Aast.m_tparams = tparams;
      Aast.m_where_constraints = m.m_where_constraints;
      Aast.m_params = typed_params;
      Aast.m_ctxs = m.m_ctxs;
      Aast.m_unsafe_ctxs = m.m_unsafe_ctxs;
      Aast.m_fun_kind = m.m_fun_kind;
      Aast.m_user_attributes = user_attributes;
      Aast.m_readonly_ret = m.m_readonly_ret;
      Aast.m_ret = (ret_locl_ty, return_hint);
      Aast.m_body = { Aast.fb_ast = tb };
      Aast.m_external = m.m_external;
      Aast.m_hidden = m.m_hidden;
      Aast.m_doc_comment = m.m_doc_comment;
    }
  in
  let sdt_dynamic_check_required =
    sdt_dynamic_check_required
    && (not had_errors)
    && not
       @@
       (* Add `dynamic` lower and upper bound to any type parameters that are marked <<__RequireDynamic>> *)
       let env_with_require_dynamic =
         Typing_dynamic.add_require_dynamic_bounds
           sound_dynamic_check_saved_env
           cls
       in
       Typing_dynamic.sound_dynamic_interface_check
         ~this_class:(Some cls)
         env_with_require_dynamic
         params_decl_ty
         return.Typing_env_return_info.return_type
  in
  let method_def_of_dynamic
      (dynamic_env, dynamic_params, dynamic_body, dynamic_return_ty) =
    let open Aast in
    {
      under_normal_assumptions with
      m_annotation = Env.save local_tpenv dynamic_env;
      m_params = dynamic_params;
      m_body = { Aast.fb_ast = dynamic_body };
      m_ret = (dynamic_return_ty, return_hint);
    }
  in
  let (env, under_normal_assumptions, under_dynamic_assumptions) =
    if sdt_dynamic_check_required && not (TCO.skip_check_under_dynamic tcopt)
    then
      let env = { env with checked = Tast.CUnderNormalAssumptions } in
      let under_normal_assumptions =
        Aast.
          {
            under_normal_assumptions with
            m_annotation = Env.save local_tpenv env;
          }
      in
      let dynamic_components =
        method_dynamically_callable
          sound_dynamic_check_saved_env
          cls
          m
          params_decl_ty
          return
      in
      ( env,
        under_normal_assumptions,
        Some (method_def_of_dynamic dynamic_components) )
    else
      (env, under_normal_assumptions, None)
  in
  let env = restore_pos env in
  let _env = Env.log_env_change "method_def" initial_env env in
  let ty_err_opt = Option.merge e1 e2 ~f:Typing_error.both in
  Option.iter ~f:(Typing_error_utils.add_typing_error ~env) ty_err_opt;
  { Tast_with_dynamic.under_normal_assumptions; under_dynamic_assumptions }

(** Checks that extending the base class is legal, i.e.:
    - the parent is not final
    - if the class is const, the parent should also be *)
let check_parent env class_def class_type =
  match Env.get_parent_class env with
  | Decl_entry.Found parent_type ->
    let position = fst class_def.c_name in
    if
      Cls.has_const_attribute class_type
      && not (Cls.has_const_attribute parent_type)
    then
      Typing_error_utils.add_typing_error
        ~env
        Typing_error.(primary @@ Primary.Self_const_parent_not position);
    if Cls.final parent_type then
      Typing_error_utils.add_typing_error
        ~env
        Typing_error.(
          primary
          @@ Primary.Extend_final
               {
                 pos = position;
                 decl_pos = Cls.pos parent_type;
                 name = Cls.name parent_type;
               })
  | Decl_entry.DoesNotExist
  | Decl_entry.NotYetAvailable ->
    ()

let check_consistent_construct_not_abstract_final env class_def class_type =
  if
    TCO.strict_consistent_construct (Env.get_tcopt env)
    && Cls.abstract class_type
    && class_def.c_final
  then begin
    (* Check the NAST directly for the attribute on the current class —
       no decl lookup needed. *)
    let has_cc_on_self =
      Naming_attributes.mem
        SN.UserAttributes.uaConsistentConstruct
        class_def.c_user_attributes
    in
    (* For abstract final classes, the folded construct kind is FinalClass
       rather than ConsistentConstruct, so we also check the NAST attribute. *)
    let is_cc =
      match snd (Env.get_construct env class_type) with
      | ConsistentConstruct -> true
      | _ -> has_cc_on_self
    in
    if is_cc then begin
      let inherited_from =
        if has_cc_on_self then
          None
        else
          (* The error message would be better if we could
             tell the user *which* ancestor has the explicit __ConsistentConstruct
             attribute, but checking would be too expensive *)
          let cc_parent =
            List.find_map class_def.c_extends ~f:(function
                | (_, Happly ((_, name), _)) -> begin
                  match Env.get_class env name with
                  | Decl_entry.Found parent_cls ->
                    let consistent = snd (Env.get_construct env parent_cls) in
                    (match consistent with
                    | ConsistentConstruct -> Some name
                    | _ -> None)
                  | Decl_entry.DoesNotExist
                  | Decl_entry.NotYetAvailable ->
                    None
                end
                | _ -> None)
          in
          match cc_parent with
          | Some name -> Some name
          | None -> Some "an ancestor"
      in
      Typing_warning_utils.add
        env
        ( fst class_def.c_name,
          Typing_warning.Consistent_construct_abstract_final,
          {
            Typing_warning.Consistent_construct_abstract_final.name =
              snd class_def.c_name;
            inherited_from;
          } )
    end
  end

(** If the class is sealed, check that the elements of the whitelist
    are descendants of the class and emit a warning if not. *)
let sealed_subtype (c : Nast.class_) ~is_enum ~env =
  let parent_name = snd c.c_name in
  let parent_kind = c.c_kind in
  let is_sealed (attr : Nast.user_attribute) =
    String.equal (snd attr.ua_name) SN.UserAttributes.uaSealed
  in
  match List.find c.c_user_attributes ~f:is_sealed with
  | None -> ()
  | Some sealed_attr ->
    let iter_item ((_, pos, expr_) : Nast.expr) =
      match expr_ with
      | Class_const ((_, _, cid), _) ->
        let klass_name = Nast.class_id_to_str cid in
        let klass = Typing_env.get_class env klass_name in
        (match klass with
        | Decl_entry.DoesNotExist
        | Decl_entry.NotYetAvailable ->
          ()
        | Decl_entry.Found decl ->
          let includes_ancestor =
            if is_enum then
              match Cls.enum_type decl with
              | None -> true
              | Some enum_ty ->
                let check x =
                  match get_node x with
                  | Tapply ((_, name), _) -> String.equal name parent_name
                  | _ -> true
                in
                List.exists enum_ty.te_includes ~f:check
            else
              Cls.has_ancestor decl parent_name
          in
          (* Skip the warning if this is a class with a trait in the sealed allowlist.
             This case is now handled as a proper error in nast_check (NastCheck[3108])
             since traits cannot extend classes. *)
          let class_kind = Cls.kind decl in
          let is_class_with_trait =
            Ast_defs.(
              match (parent_kind, class_kind) with
              | (Cclass _, Ctrait) -> true
              | _ -> false)
          in
          if (not includes_ancestor) && not is_class_with_trait then
            let parent_pos = pos in
            let child_name = Cls.name decl in
            let (child_kind, verb) =
              Ast_defs.(
                match (parent_kind, class_kind) with
                | (Cinterface, Cclass _) -> ("Class", "implement")
                | (_, Cclass _) -> ("Class", "extend")
                | (Cinterface, Cinterface) -> ("Interface", "extend")
                | (_, Cinterface) -> ("Interface", "implement")
                | (Cinterface, Ctrait) -> ("Trait", "implement")
                | (_, Ctrait) -> ("Trait", "use")
                | (_, Cenum) -> ("Enum", "use")
                | (_, Cenum_class _) -> ("Enum Class", "extend"))
            in
            Typing_warning_utils.add
              env
              ( parent_pos,
                Typing_warning.Sealed_not_subtype,
                {
                  Typing_warning.Sealed_not_subtype.verb;
                  parent_name;
                  parent_method_name = None;
                  child_name;
                  child_kind;
                } ))
      (* unit below is fine because error cases are handled as Parsing[1002] *)
      | _ -> ()
    in
    List.iter sealed_attr.ua_params ~f:iter_item

(** If the parent is sealed, check that the child being checked
    is in the whitelist. *)
let check_parent_sealed (child_pos, child_type) parent_type ~env =
  match Cls.sealed_whitelist parent_type with
  | None -> ()
  | Some whitelist ->
    let parent_pos = Cls.pos parent_type in
    let parent_name = Cls.name parent_type in
    let child_name = Cls.name child_type in
    let check parent_kind verb =
      if not (S_set.mem child_name whitelist) then
        Typing_error_utils.add_typing_error
          ~env
          Typing_error.(
            primary
            @@ Primary.Extend_sealed
                 { pos = child_pos; parent_pos; parent_name; parent_kind; verb })
    in
    begin
      match (Cls.kind parent_type, Cls.kind child_type) with
      | (Ast_defs.Cinterface, Ast_defs.Cinterface) -> check `intf `extend
      | (Ast_defs.Cinterface, _) -> check `intf `implement
      | (Ast_defs.Ctrait, _) -> check `trait `use
      | (Ast_defs.Cclass _, _) -> check `class_ `extend
      | (Ast_defs.Cenum_class _, _) -> check `enum_class `extend
      | (Ast_defs.Cenum, _) -> check `enum `use
    end

(** If the parents are sealed, check that the child being checked
    is in the whitelists. *)
let check_parents_sealed env child_def child_type =
  let parents =
    match child_def.c_enum with
    | Some enum -> enum.e_includes
    | None -> child_def.c_extends
  in
  let parents = parents @ child_def.c_implements @ child_def.c_uses in
  List.iter parents ~f:(function
      | (_, Happly ((_, name), _)) -> begin
        match Env.get_class env name with
        | Decl_entry.Found parent_type ->
          check_parent_sealed
            (fst child_def.c_name, child_type)
            parent_type
            ~env
        | Decl_entry.DoesNotExist
        | Decl_entry.NotYetAvailable ->
          ()
      end
      | _ -> ())

(* Reject multiple instantiations of the same generic interface
 * in extends and implements clauses.
 * e.g. disallow class C implements I<string>, I<int>
 *
 * O(n^2) but we don't expect number of instantiated interfaces to be large
 *)
let rec check_implements_or_extends_unique impl ~env =
  match impl with
  | [] -> ()
  | (hint, ty) :: rest ->
    (match get_node ty with
    | Tapply ((_, name), _ :: _) ->
      let (pos_list, rest) =
        List.partition_map rest ~f:(fun (h, ty) ->
            match get_node ty with
            | Tapply ((pos', name'), _) when String.equal name name' ->
              First pos'
            | _ -> Second (h, ty))
      in
      if not (List.is_empty pos_list) then
        Typing_error_utils.add_typing_error
          ~env
          Typing_error.(
            primary
            @@ Primary.Duplicate_interface
                 { pos = fst hint; name; others = pos_list });

      check_implements_or_extends_unique rest ~env
    | _ -> check_implements_or_extends_unique rest ~env)

(** Add a dependency to constructors or produce an error if not a Tapply. *)
let check_is_tapply_add_constructor_extends_dep
    env ?(skip_constructor_dep = false) (deps : ((pos * _) * decl_ty) list) =
  List.iter deps ~f:(fun ((p, _dep_hint), dep) ->
      match get_node dep with
      | Tapply ((_, class_name), _) ->
        Env.add_parent_dep env ~skip_constructor_dep class_name
      | Tgeneric _ ->
        Typing_error_utils.add_typing_error
          ~env
          Typing_error.(
            primary
            @@ Primary.Expected_class
                 {
                   suffix = Some (lazy " or interface but got a generic");
                   pos = p;
                 })
      | _ ->
        Typing_error_utils.add_typing_error
          ~env
          Typing_error.(
            primary
            @@ Primary.Expected_class
                 { suffix = Some (lazy " or interface"); pos = p }))

(** For const classes, check that members from traits are all constant. *)
let check_non_const_trait_members pos env use_list =
  let ((_, trait, _), err_opt) = Decl_utils.unwrap_class_hint use_list in
  Option.iter ~f:(Typing_error_utils.add_typing_error ~env) err_opt;
  match Env.get_class env trait with
  | Decl_entry.Found c when Ast_defs.is_c_trait (Cls.kind c) ->
    List.iter (Cls.props c) ~f:(fun (x, ce) ->
        if not (get_ce_const ce) then
          Typing_error_utils.add_typing_error
            ~env
            Typing_error.(
              primary @@ Primary.Trait_prop_const_class { pos; name = x }))
  | _ -> ()

let check_consistent_enum_inclusion
    included_cls ((dest_cls_pos, dest_cls) : Pos.t * Cls.t) ~env =
  let included_kind = Cls.kind included_cls in
  let dest_kind = Cls.kind dest_cls in
  (* Check package visibility *)
  (match
     Typing_visibility.check_package_access
       ~should_check_package_boundary:(`Yes Enum)
       ~use_pos:dest_cls_pos
       ~def_pos:(Cls.pos included_cls)
       env
       (Cls.get_package included_cls)
       (Cls.name included_cls)
   with
  | Typing_visibility.Package_access_error err ->
    Typing_error_utils.add_typing_error ~env err
  | Typing_visibility.Package_access_linter_error (pos, w) ->
    Lints_diagnostics.crosspackage_linter
      pos
      ~current_package:w.current_package
      ~current_package_before_override:w.current_package_before_override
      ~caller_has_package_override:w.caller_has_package_override
      ~target_package:w.target_package
      ~target_package_before_override:w.target_package_before_override
      ~callee_has_package_override:w.callee_has_package_override
      ~classptr_reference_warning:w.classptr_reference_warning
  | Typing_visibility.Package_access_ok -> ());
  match (Cls.enum_type included_cls, Cls.enum_type dest_cls) with
  | (Some included_e, Some dest_e) ->
    (* ensure that the base types are identical *)
    if not (Typing_defs.equal_decl_ty included_e.te_base dest_e.te_base) then
      Typing_error_utils.add_typing_error
        ~env
        Typing_error.(
          enum
          @@ Primary.Enum.Incompatible_enum_inclusion_base
               {
                 pos = dest_cls_pos;
                 classish_name = Cls.name dest_cls;
                 src_classish_name = Cls.name included_cls;
               });
    (* ensure that the visibility constraint are compatible *)
    (match (included_e.te_constraint, dest_e.te_constraint) with
    | (None, Some _) ->
      Typing_error_utils.add_typing_error
        ~env
        Typing_error.(
          enum
          @@ Primary.Enum.Incompatible_enum_inclusion_constraint
               {
                 pos = dest_cls_pos;
                 classish_name = Cls.name dest_cls;
                 src_classish_name = Cls.name included_cls;
               })
    | (_, _) -> ());
    (* ensure normal enums can't include enum classes *)
    if
      Ast_defs.is_c_enum_class included_kind
      && not (Ast_defs.is_c_enum_class dest_kind)
    then
      Typing_error_utils.add_typing_error
        ~env
        Typing_error.(
          primary
          @@ Primary.Wrong_extend_kind
               {
                 parent_pos = Cls.pos included_cls;
                 parent_kind = included_kind;
                 parent_name = Cls.name included_cls;
                 pos = dest_cls_pos;
                 kind = dest_kind;
                 name = Cls.name dest_cls;
               })
  | (None, _) ->
    Typing_error_utils.add_typing_error
      ~env
      Typing_error.(
        enum
        @@ Primary.Enum.Enum_inclusion_not_enum
             {
               pos = dest_cls_pos;
               classish_name = Cls.name dest_cls;
               src_classish_name = Cls.name included_cls;
             })
  | (_, _) -> ()

let skip_check_multiple_instantiations (env : Typing_env_types.env) type_name =
  match Typing_env.get_class env type_name with
  | Decl_entry.DoesNotExist
  | Decl_entry.NotYetAvailable ->
    true
  | Decl_entry.Found cls ->
    (not @@ Ast_defs.is_c_interface @@ Cls.kind cls)
    || Cls.allow_multiple_instantiations cls

(** Check that repeated use of the same interface, but possibly at a different instantiation,
    is textually in subtype order.
    This code should shadow that in decl_folded_class.ml/fold.rs.

    @param  parent_pos  position of the parent which has interface [name] as ancestor
    @param  name        name of the interface we're checking
    @pararm ty          type of the interface we're checking *)
let add_or_check_is_supertype
    env
    ~current_type
    ~parent_pos_id
    ~(interface_name : string)
    (ty : decl_ty)
    ancestors =
  if skip_check_multiple_instantiations env interface_name then
    (env, ancestors)
  else
    match S_map.find_opt interface_name ancestors with
    | None -> (env, S_map.add interface_name (ty, parent_pos_id) ancestors)
    | Some (first_ty, first_parent_pos_id) ->
      (* We want this check to break through module boundaries so we disable
       * the localization of internal newtypes outside their module as newtype *)
      let ety_env = { empty_expand_env with make_internal_opaque = false } in
      let ((env, _err), lty) = Phase.localize ~ety_env env ty in
      let ((env, _err), first_lty) = Phase.localize ~ety_env env first_ty in
      let (env, ty_err_opt) =
        let (_, (winning_pos, _), winning_instantiation) =
          Decl_utils.unwrap_class_type first_ty
        in
        let (_, (losing_pos, _), losing_instantiation) =
          Decl_utils.unwrap_class_type ty
        in
        Typing_ops.sub_type_w_err_prefix
          (fst parent_pos_id)
          env
          first_lty
          lty
          (Typing_error.Primary.Multiple_instantiation_inheritence
             {
               type_name = fst current_type;
               implements_or_extends =
                 (if Ast_defs.is_c_interface (snd current_type) then
                   "extends"
                 else
                   "implements");
               interface_name;
               winning_implements =
                 {
                   Typing_error.Primary.pos = winning_pos;
                   instantiation =
                     List.map
                       winning_instantiation
                       ~f:(Typing_print.full_decl (Env.get_tcopt env));
                   via_direct_parent = first_parent_pos_id;
                 };
               losing_implements =
                 {
                   Typing_error.Primary.pos = losing_pos;
                   instantiation =
                     List.map
                       losing_instantiation
                       ~f:(Typing_print.full_decl (Env.get_tcopt env));
                   via_direct_parent = parent_pos_id;
                 };
             })
      in
      Option.iter ty_err_opt ~f:(Typing_error_utils.add_typing_error ~env);
      (env, ancestors)

let get_instantiated_ancestors_and_self env (_pos, type_name) tyargs ty :
    Typing_defs.decl_ty S_map.t =
  let class_ = Env.get_class env type_name in
  let instantiated_ancestors =
    match class_ with
    | Decl_entry.DoesNotExist
    | Decl_entry.NotYetAvailable ->
      S_map.empty
    | Decl_entry.Found class_ ->
      let subst = Decl_instantiate.make_subst (Cls.tparams class_) tyargs in
      Cls.all_ancestors class_
      |> S_map.of_list
      |> S_map.map (Decl_instantiate.instantiate subst)
  in
  S_map.add type_name ty instantiated_ancestors

(** Check that repeated use of the same interface, but possibly at a different instantiation,
    is textually in subtype order. *)
let check_multiple_instantiation_inheritance
    env ~current_type (parents : hint list) : env =
  if Ast_defs.is_c_enum_class (snd current_type) then
    env
  else
    let parents =
      List.map parents ~f:(fun hint ->
          let decl_ty = Decl_hint.hint env.decl_env hint in
          let (_r, (_name_pos, class_name), tyargs) =
            Decl_utils.unwrap_class_type decl_ty
          in
          ((fst hint, class_name), decl_ty, tyargs))
    in
    let (env, _ancestors) =
      List.fold
        parents
        ~f:(fun ancestors (parent_pos_id, parent_ty, parent_tyargs) ->
          let ancestors_and_self =
            get_instantiated_ancestors_and_self
              env
              parent_pos_id
              parent_tyargs
              parent_ty
          in
          S_map.fold
            (fun ancestor_name ty (env, ancestors) ->
              add_or_check_is_supertype
                env
                ~current_type
                ~parent_pos_id
                ~interface_name:ancestor_name
                ty
                ancestors)
            ancestors_and_self
            ancestors)
        ~init:(env, S_map.empty)
    in
    env

let is_enum_or_enum_class k = Ast_defs.is_c_enum k || Ast_defs.is_c_enum_class k

let check_enum_includes env cls =
  let is_abstract = function
    | CCAbstract has_default -> not has_default
    | CCConcrete -> false
  in
  (* checks that there are no duplicated enum-constants when folded-decls are enabled *)
  if is_enum_or_enum_class cls.c_kind then (
    let (dest_class_pos, dest_class_name) = cls.c_name in
    let enum_constant_map = ref S_map.empty in
    (* prepopulate the map with the constants declared in cls *)
    List.iter cls.c_consts ~f:(fun cc ->
        enum_constant_map :=
          S_map.add
            (snd cc.cc_id)
            (fst cc.cc_id, dest_class_name)
            !enum_constant_map);
    (* for all included enums *)
    let included_enums =
      Aast.enum_includes_map cls.c_enum ~f:(fun ce ->
          List.filter_map ce.e_includes ~f:(fun ie ->
              match snd ie with
              | Happly (sid, _) ->
                (match Env.get_class env (snd sid) with
                | Decl_entry.DoesNotExist
                | Decl_entry.NotYetAvailable ->
                  None
                | Decl_entry.Found ie_cls -> Some (fst ie, ie_cls))
              | _ -> None))
    in
    List.iter included_enums ~f:(fun (ie_pos, ie_cls) ->
        let src_class_name = Cls.name ie_cls in
        (* 1. Check for consistency *)
        (match Env.get_class env dest_class_name with
        | Decl_entry.DoesNotExist
        | Decl_entry.NotYetAvailable ->
          ()
        | Decl_entry.Found cls ->
          check_consistent_enum_inclusion ie_cls (ie_pos, cls) ~env);
        (* 2. Check for duplicates *)
        List.iter
          (Typing_env.consts env ie_cls)
          ~f:(fun (const_name, class_const) ->
            (if String.equal const_name "class" then
              ()
            (* TODO: Check with @fzn *)
            else if is_abstract class_const.cc_abstract then
              ()
            else if S_map.mem const_name !enum_constant_map then
              (* distinguish between multiple inherit and redeclare *)
              let (origin_const_pos, origin_class_name) =
                S_map.find const_name !enum_constant_map
              in
              if String.equal origin_class_name dest_class_name then
                (* redeclare *)
                Typing_error_utils.add_typing_error
                  ~env
                  Typing_error.(
                    primary
                    @@ Primary.Redeclaring_classish_const
                         {
                           pos = dest_class_pos;
                           classish_name = dest_class_name;
                           redeclaration_pos = origin_const_pos;
                           existing_const_origin = src_class_name;
                           const_name;
                         })
              else if String.( <> ) origin_class_name class_const.cc_origin then
                (* check for diamond inclusion, if not raise an error about multiple inherit *)
                Typing_error_utils.add_typing_error
                  ~env
                  Typing_error.(
                    primary
                    @@ Primary.Reinheriting_classish_const
                         {
                           pos = dest_class_pos;
                           classish_name = dest_class_name;
                           src_pos = ie_pos;
                           src_classish_name = src_class_name;
                           existing_const_origin = origin_class_name;
                           const_name;
                         }));
            enum_constant_map :=
              S_map.add
                const_name
                (dest_class_pos, class_const.cc_origin)
                !enum_constant_map))
  )

let skip_hierarchy_checks (ctx : Provider_context.t) : bool =
  TCO.skip_hierarchy_checks (Provider_context.get_tcopt ctx)

let class_type_param env ct =
  let (env, tparam_list) = List.map_env env ct ~f:Typing.type_param in
  (env, tparam_list)

(** Check whether the type of a static property (class variable) contains
    any generic type parameters. Outside of traits, this is illegal as static
     * properties are shared across all generic instantiations. *)
let check_no_generic_static_property env tc =
  if Ast_defs.is_c_trait (Cls.kind tc) then
    ()
  else
    Cls.sprops tc
    |> List.iter ~f:(fun (_prop_name, prop) ->
           let (lazy ty) = prop.ce_type in
           let var_ty_pos = get_pos ty in
           let class_pos = Cls.pos tc in
           match TUtils.contains_generic_decl ty with
           | None -> ()
           | Some generic_pos ->
             Option.iter
               (* If the static property is inherited from another trait, the position may be
                * in a different file. *)
               (Env.fill_in_pos_filename_if_in_current_decl env generic_pos)
               ~f:(fun generic_pos ->
                 Typing_error_utils.add_typing_error
                   ~env
                   Typing_error.(
                     primary
                     @@ Primary.Static_prop_type_generic_param
                          { class_pos; var_ty_pos; pos = generic_pos })))

let typeconst_def
    cls
    env
    {
      c_tconst_name = (pos, _) as id;
      c_tconst_kind;
      c_tconst_user_attributes;
      c_tconst_span;
      c_tconst_doc_comment;
      c_tconst_is_ctx;
    } =
  let tcopt = Env.get_tcopt env in
  Enable.if_matches_regexp
    tcopt
    (Some env)
    ~default:
      ( env,
        {
          c_tconst_name = id;
          c_tconst_kind;
          c_tconst_user_attributes = [];
          c_tconst_span;
          c_tconst_doc_comment;
          c_tconst_is_ctx;
        } )
    id
  @@ fun () ->
  Profile.measure_elapsed_time_and_report tcopt (Some env) id @@ fun () ->
  (if Ast_defs.is_c_enum cls.c_kind then
    let (class_pos, class_name) = cls.c_name in
    Typing_error_utils.add_typing_error
      ~env
      Typing_error.(
        primary
        @@ Primary.Cannot_declare_constant { pos; class_pos; class_name }));
  let expansion =
    Type_expansions.Expandable.Type_constant
      { receiver_name = snd cls.c_name; type_const_name = snd id }
  in
  (* Check constraints and report cycles through the definition *)
  let (env, ty_err_opt) =
    match c_tconst_kind with
    | TCAbstract { c_atc_as_constraint; c_atc_super_constraint; c_atc_default }
      ->
      let (env, ty_default_opt) =
        match c_atc_default with
        | Some ty ->
          let ((env, ty_err_opt1, cycles), ty) =
            Phase.localize_hint_no_subst_report_cycles
              ~ignore_errors:false
              ~report_cycle:(pos, expansion)
              env
              ty
          in
          Option.iter ~f:(Typing_error_utils.add_typing_error ~env) ty_err_opt1;
          Type_expansions.report cycles
          |> Option.iter ~f:(Typing_error_utils.add_typing_error ~env);
          (env, Some ty)
        | None -> (env, None)
      in
      let (env, ty_err_opt2) =
        match c_atc_as_constraint with
        | Some as_ ->
          let ((env, ty_err_opt1), as_) =
            Phase.localize_hint_no_subst ~ignore_errors:false env as_
          in
          (match ty_default_opt with
          | Some ty ->
            let (env, ty_err_opt2) =
              Type.sub_type
                ~class_sub_classname:false
                pos
                Reason.URtypeconst_cstr
                env
                ty
                as_
                Typing_error.Callback.unify_error
            in
            (env, Option.merge ~f:Typing_error.both ty_err_opt1 ty_err_opt2)
          | None -> (env, ty_err_opt1))
        | None -> (env, None)
      in
      let (env, ty_err_opt3) =
        match c_atc_super_constraint with
        | Some super ->
          let ((env, te1), super) =
            Phase.localize_hint_no_subst ~ignore_errors:false env super
          in
          (match ty_default_opt with
          | Some ty ->
            let (env, te2) =
              Type.sub_type
                ~class_sub_classname:false
                pos
                Reason.URtypeconst_cstr
                env
                super
                ty
                Typing_error.Callback.unify_error
            in
            (env, Option.merge ~f:Typing_error.both te1 te2)
          | None -> (env, te1))
        | None -> (env, None)
      in
      let ty_err_opt =
        Option.merge ~f:Typing_error.both ty_err_opt2 ty_err_opt3
      in
      (env, ty_err_opt)
    | TCConcrete { c_tc_type = ty } ->
      let ((env, ty_err_opt, cycles), _ty) =
        Phase.localize_hint_no_subst_report_cycles
          ~ignore_errors:false
          ~report_cycle:(pos, expansion)
          env
          ty
      in
      Type_expansions.report cycles
      |> Option.iter ~f:(Typing_error_utils.add_typing_error ~env);
      (env, ty_err_opt)
  in
  Option.iter ty_err_opt ~f:(Typing_error_utils.add_typing_error ~env);

  let (env, user_attributes) =
    Typing.attributes_check_def
      env
      SN.AttributeKinds.typeconst
      c_tconst_user_attributes
  in
  ( env,
    {
      Aast.c_tconst_name = id;
      Aast.c_tconst_kind;
      Aast.c_tconst_user_attributes = user_attributes;
      Aast.c_tconst_span;
      Aast.c_tconst_doc_comment;
      Aast.c_tconst_is_ctx;
    } )

let class_const_def ~in_enum_class c cls env cc =
  let tcopt = Env.get_tcopt env in
  Enable.if_matches_regexp
    ~default:
      ( env,
        ( {
            cc_user_attributes = [];
            cc_type = None;
            cc_id = cc.cc_id;
            cc_kind = CCAbstract None;
            cc_span = cc.cc_span;
            cc_doc_comment = cc.cc_doc_comment;
          },
          Typing_make_type.nothing Reason.none ) )
    tcopt
    (Some env)
    cc.cc_id
  @@ fun () ->
  Profile.measure_elapsed_time_and_report tcopt (Some env) cc.cc_id @@ fun () ->
  let { cc_type = h; cc_id = id; cc_kind = k; _ } = cc in
  let (env, hint_ty, opt_expected, et_enforced) =
    match h with
    | None ->
      begin
        match k with
        | CCAbstract _
          when (not (is_enum_or_enum_class c.c_kind))
               && TCO.require_types_class_consts tcopt > 0 ->
          let custom_err_config = TCO.custom_error_config tcopt in
          Diagnostics.add_diagnostic
            (Naming_error_utils.to_user_diagnostic
               (Naming_error.Missing_typehint (fst id))
               custom_err_config)
        | _
          when (not (is_enum_or_enum_class c.c_kind))
               && TCO.require_types_class_consts tcopt > 1 ->
          let custom_err_config = TCO.custom_error_config tcopt in
          Diagnostics.add_diagnostic
            (Naming_error_utils.to_user_diagnostic
               (Naming_error.Missing_typehint (fst id))
               custom_err_config)
        | CCAbstract None -> ()
        | CCAbstract (Some e (* default *))
        | CCConcrete e ->
          if
            (not (Decl_utils.is_literal_with_trivially_inferable_type e))
            && (not (is_enum_or_enum_class c.c_kind))
            && not (Env.is_hhi env)
          then
            let custom_err_config = TCO.custom_error_config tcopt in
            Diagnostics.add_diagnostic
              (Naming_error_utils.to_user_diagnostic
                 (Naming_error.Missing_typehint (fst id))
                 custom_err_config)
      end;
      let (env, ty) = Env.fresh_type env (fst id) in
      (env, ty, None, Unenforced)
    | Some h ->
      let ty = Decl_hint.hint env.decl_env h in
      let (et_enforced, ty) =
        Typing_enforceability.compute_enforced_ty ~this_class:(Some cls) env ty
      in
      let ((env, ty_err_opt), ty) =
        Phase.localize_no_subst env ~ignore_errors:false ty
      in
      Option.iter ty_err_opt ~f:(Typing_error_utils.add_typing_error ~env);
      (* Removing the HH\MemberOf wrapper in case of enum classes so the
       * following call to expr_* has the right expected type
       *)
      let opt_ty =
        if in_enum_class then
          match get_node ty with
          | Tnewtype (memberof, [_; et_type], _)
            when String.equal memberof SN.Classes.cMemberOf ->
            et_type
          | _ -> ty
        else
          ty
      in
      ( env,
        ty,
        Some (ExpectedTy.make (fst id) Reason.URhint opt_ty),
        et_enforced )
  in
  let check env ty' =
    (* Lifted out to closure to illustrate which variables are captured *)
    Typing_coercion.coerce_type
      (fst id)
      Reason.URhint
      env
      ty'
      hint_ty
      et_enforced
      Typing_error.Callback.class_constant_value_does_not_match_hint
  in
  let type_and_check env e =
    let (env, (te, ty')) =
      Typing.(
        expr_with_pure_coeffects ?expected:opt_expected env e |> triple_to_pair)
    in
    let env = check env ty' in
    (env, te, ty')
  in
  let check_class env e ty =
    Typing_class_pointers.check_string_coercion_point env e ty
  in
  let ((env, ty_err_opt), kind, ty) =
    match k with
    | CCConcrete e when in_enum_class ->
      let (env, cap, unsafe_cap) =
        (* Enum class constant initializers are restricted to be `write_props` *)
        let make_hint pos s = (pos, Aast.Happly ((pos, s), [])) in
        let e_pos = Aast_utils.get_expr_pos e in
        let enum_class_ctx =
          Some (e_pos, [make_hint e_pos SN.Capabilities.write_props])
        in
        Typing_coeffects.type_capability env enum_class_ctx enum_class_ctx e_pos
      in
      let (env, (te, ty')) =
        Typing.(
          with_special_coeffects env cap unsafe_cap @@ fun env ->
          expr env ?expected:opt_expected e |> triple_to_pair)
      in
      let (te, ty') =
        match deref hint_ty with
        | (r, Tnewtype (memberof, [enum_name; _], def_ty))
          when String.equal memberof SN.Classes.cMemberOf ->
          check_class env e def_ty;
          let lift r ty = mk (r, Tnewtype (memberof, [enum_name; ty], ty)) in
          let (te_ty, p, te) = te in
          let te = (lift (get_reason te_ty) te_ty, p, te) in
          let ty' = lift r ty' in
          (te, ty')
        | _ -> (te, ty')
      in
      let env = check env ty' in
      (env, Aast.CCConcrete te, ty')
    | CCConcrete ((_, _, Omitted) as e) when Env.is_hhi env ->
      (* We don't require class consts to have a value set for decl purposes
       * in HHI files so it may just be a placeholder, therefore we don't care
       * about checking the value and simply pass it through *)
      let (env, te, _ty) = Typing.expr env e in
      ((env, None), CCConcrete te, hint_ty)
    | CCConcrete e ->
      check_class env e hint_ty;
      let (env, te, ty') = type_and_check env e in
      (env, Aast.CCConcrete te, ty')
    | CCAbstract (Some default) ->
      check_class env default hint_ty;
      let (env, tdefault, ty') = type_and_check env default in
      (env, CCAbstract (Some tdefault), ty')
    | CCAbstract None -> ((env, None), CCAbstract None, hint_ty)
  in
  let (env, user_attributes) =
    if
      Ast_defs.is_c_class c.Aast.c_kind
      || Ast_defs.is_c_trait c.Aast.c_kind
      || Ast_defs.is_c_interface c.Aast.c_kind
    then
      Typing.attributes_check_def
        env
        SN.AttributeKinds.clscst
        cc.cc_user_attributes
    else begin
      assert (List.is_empty cc.cc_user_attributes);
      (env, [])
    end
  in
  Option.iter ty_err_opt ~f:(Typing_error_utils.add_typing_error ~env);
  let env =
    match
      Naming_attributes.mem_pos
        SN.UserAttributes.uaShapeKey
        cc.cc_user_attributes
    with
    | None -> env
    | Some pos -> Typing_shapes.check_shape_key_const env pos c cls cc kind
  in
  ( env,
    ( {
        Aast.cc_type = cc.cc_type;
        Aast.cc_id = cc.cc_id;
        Aast.cc_kind = kind;
        Aast.cc_span = cc.cc_span;
        Aast.cc_doc_comment = cc.cc_doc_comment;
        Aast.cc_user_attributes = user_attributes;
      },
      ty ) )

let class_constr_def ~is_disposable env cls constructor =
  Option.bind constructor ~f:(method_def ~is_disposable env cls)

(** Type-check a property declaration, with optional initializer *)
let class_var_def ~is_static ~is_noautodynamic cls env cv =
  let tcopt = Env.get_tcopt env in
  Profile.measure_elapsed_time_and_report tcopt (Some env) cv.cv_id @@ fun () ->
  (* First pick up and localize the hint if it exists *)
  let decl_cty =
    FunUtils.hint_to_type_opt env.decl_env (hint_of_type_hint cv.cv_type)
  in
  let no_auto_likes =
    Naming_attributes.mem SN.UserAttributes.uaNoAutoLikes cv.cv_user_attributes
  in
  let (env, expected) =
    match decl_cty with
    | None -> (env, None)
    | Some decl_cty ->
      let (et_enforced, decl_cty) =
        Typing_enforceability.compute_enforced_ty
          ~this_class:(Some cls)
          env
          decl_cty
      in
      let ((env, ty_err_opt), cty) =
        Phase.localize_no_subst env ~ignore_errors:false decl_cty
      in
      let cty =
        match et_enforced with
        | Enforced when is_none cv.cv_xhp_attr -> cty
        | _ ->
          let add_like = (not is_noautodynamic) && not no_auto_likes in
          if TCO.everything_sdt env.genv.tcopt && add_like then
            TUtils.make_like env cty
          else (
            if add_like then
              Typing_log.log_pessimise_prop env (fst cv.cv_id) (snd cv.cv_id);
            cty
          )
      in
      Option.iter ty_err_opt ~f:(Typing_error_utils.add_typing_error ~env);
      let expected = Some (ExpectedTy.make cv.cv_span Reason.URhint cty) in
      (env, expected)
  in
  (* Next check the expression, passing in expected type if present *)
  let ((env, ty_err_opt), typed_cv_expr) =
    match cv.cv_expr with
    | None -> ((env, None), None)
    | Some e ->
      let (env, te, ty) = Typing.expr_with_pure_coeffects env ?expected e in
      (* Check that the inferred type is a subtype of the expected type.
       * Eventually this will be the responsibility of `expr`
       *)
      let env =
        match expected with
        | None -> (env, None)
        | Some
            ExpectedTy.
              {
                pos = p;
                reason = ur;
                ty = cty;
                is_dynamic_aware;
                ignore_readonly;
              } ->
          Typing_coercion.coerce_type
            ~is_dynamic_aware
            ~ignore_readonly
            p
            ur
            env
            ty
            cty
            Enforced (* TODO akenn: flow this in *)
            Typing_error.Callback
            .class_property_initializer_type_does_not_match_hint
      in
      (env, Some te)
  in
  Option.iter ty_err_opt ~f:(Typing_error_utils.add_typing_error ~env);

  let (env, user_attributes) =
    Typing.attributes_check_def
      env
      (if is_static then
        SN.AttributeKinds.staticProperty
      else
        SN.AttributeKinds.instProperty)
      cv.cv_user_attributes
  in

  (if Option.is_none (hint_of_type_hint cv.cv_type) then
    let vis =
      match cv.cv_visibility with
      | Public -> Naming_error.Vpublic
      | Private -> Naming_error.Vprivate
      | Internal -> Naming_error.Vinternal
      | Protected -> Naming_error.Vprotected
      | ProtectedInternal -> Naming_error.Vprotected_internal
    in
    let (pos, prop_name) = cv.cv_id in
    let custom_err_config = TCO.custom_error_config tcopt in
    Diagnostics.add_diagnostic
      (Naming_error_utils.to_user_diagnostic
         (Naming_error.Prop_without_typehint { vis; pos; prop_name })
         custom_err_config));

  let ((cv_type_ty, _) as cv_type) =
    match expected with
    | Some expected -> (expected.ExpectedTy.ty, hint_of_type_hint cv.cv_type)
    | None -> Tast.dummy_type_hint (hint_of_type_hint cv.cv_type)
  in
  (* if the class implements dynamic, then check that the type of the property
   * is enforceable (for writing) and coerces to dynamic (for reading) *)
  if
    Cls.get_support_dynamic_type cls
    && not (Aast.equal_visibility cv.cv_visibility Private)
  then (
    let env_with_require_dynamic =
      Typing_dynamic.add_require_dynamic_bounds env cls
    in
    Option.iter ~f:(Typing_error_utils.add_typing_error ~env)
    @@ Option.bind decl_cty ~f:(fun ty ->
           Typing_dynamic.check_property_sound_for_dynamic_write
             ~this_class:(Some cls)
             ~on_error:(fun pos prop_name class_name (prop_pos, prop_type) ->
               Typing_error.(
                 primary
                 @@ Primary.Property_is_not_enforceable
                      { pos; prop_name; class_name; prop_pos; prop_type }))
             env_with_require_dynamic
             (Cls.name cls)
             cv.cv_id
             ty
             (Some cv_type_ty));

    Option.iter ~f:(Typing_error_utils.add_typing_error ~env)
    @@ Typing_dynamic.check_property_sound_for_dynamic_read
         ~on_error:(fun pos prop_name class_name (prop_pos, prop_type) ->
           Typing_error.(
             primary
             @@ Primary.Property_is_not_dynamic
                  { pos; prop_name; class_name; prop_pos; prop_type }))
         env_with_require_dynamic
         (Cls.name cls)
         cv.cv_id
         cv_type_ty
  );
  ( env,
    {
      Aast.cv_final = cv.cv_final;
      Aast.cv_xhp_attr = cv.cv_xhp_attr;
      Aast.cv_abstract = cv.cv_abstract;
      Aast.cv_visibility = cv.cv_visibility;
      Aast.cv_type;
      Aast.cv_id = cv.cv_id;
      Aast.cv_expr = typed_cv_expr;
      Aast.cv_user_attributes = user_attributes;
      Aast.cv_doc_comment = cv.cv_doc_comment;
      (* Can make None to save space *)
      Aast.cv_is_static = is_static;
      Aast.cv_span = cv.cv_span;
      Aast.cv_readonly = cv.cv_readonly;
    } )

(** Any class that extends a class or implements an interface
    that declares `<<__SupportDynamicType>>` must itself declare
    `<<__SupportDynamicType>>`. This is checked elsewhere. But if any generic
    parameters are marked `<<__RequireDynamic>>` then we must check that the
    conditional support for dynamic is sound.

  We require that:
  - If t <: dynamic
  - and C<T1,..,Tn> extends t
  - then C<T1,...,Tn> <: dynamic
  *)
let check_generic_class_with_SupportDynamicType env c tc parents =
  let (pc, c_name) = c.c_name in
  let check_support_dynamic_type = Cls.get_support_dynamic_type tc in
  if check_support_dynamic_type then (
    let dynamic_ty =
      MakeType.supportdyn_mixed (Reason.dynamic_coercion (Reason.witness pc))
    in
    let (env, ty_errs) =
      List.fold parents ~init:(env, []) ~f:(fun (env, ty_errs) (_, parent_ty) ->
          let ((env, ty_err_opt1), parent_lty) =
            Phase.localize_no_subst env ~ignore_errors:true parent_ty
          in
          let (env, ty_err_opt2) =
            match get_node parent_lty with
            | Tclass ((_, name), _, _) -> begin
              match Env.get_class env name with
              | Decl_entry.Found c when Cls.get_support_dynamic_type c ->
                let env_with_assumptions =
                  Typing_subtype.add_constraint
                    env
                    Ast_defs.Constraint_as
                    parent_lty
                    dynamic_ty
                  @@ Some (Typing_error.Reasons_callback.unify_error_at pc)
                in
                begin
                  match Env.get_self_ty env with
                  | Some self_ty ->
                    TUtils.supports_dynamic env_with_assumptions self_ty
                    @@ Some
                         (Typing_error.Reasons_callback
                          .bad_conditional_support_dynamic
                            pc
                            ~child:c_name
                            ~parent:name
                            ~ty_name:
                              (lazy
                                (Typing_print.full_strip_ns_decl
                                   ~verbose_fun:false
                                   env
                                   parent_ty))
                            ~self_ty_name:
                              (lazy
                                (Typing_print.full_strip_ns
                                   ~hide_internals:true
                                   env
                                   self_ty)))
                  | _ -> (env, None)
                end
              | _ -> (env, None)
            end
            | _ -> (env, None)
          in
          let ty_errs =
            Option.(
              value_map ~default:ty_errs ~f:(fun e -> e :: ty_errs)
              @@ merge ~f:Typing_error.both ty_err_opt1 ty_err_opt2)
          in
          (env, ty_errs))
    in
    Option.(
      iter ~f:(Typing_error_utils.add_typing_error ~env)
      @@ Typing_error.multiple_opt ty_errs);
    env
  ) else
    env

(** Check that the classish type being checked aligns with its
    parents on the <<__SupportDynamicType>> attribute - if
    a (non-trait) parent has the attribute, then the type should have
    it too.
    The opposite is also true, but not checked here. *)
let check_SupportDynamicType env c tc =
  let support_dynamic_type = Cls.get_support_dynamic_type tc in
  let error_parent_support_dynamic_type parent child_support_dyn =
    Typing_error_utils.add_typing_error
      ~env
      Typing_error.(
        primary
        @@ Primary.Parent_support_dynamic_type
             {
               pos = fst c.c_name;
               child_name = snd c.c_name;
               child_kind = c.c_kind;
               parent_name = Cls.name parent;
               parent_kind = Cls.kind parent;
               child_support_dyn;
             })
  in
  match c.c_kind with
  | Ast_defs.(Cenum | Cenum_class _) ->
    (* Avoid parent SDT check on things that cannot be SDT themselves *)
    ()
  | Ast_defs.Cclass _
  | Ast_defs.Cinterface
  | Ast_defs.Ctrait ->
    List.iter (Cls.all_ancestor_names tc) ~f:(fun name ->
        match Env.get_class env name with
        | Decl_entry.Found parent_type -> begin
          match Cls.kind parent_type with
          | Ast_defs.Cclass _
          | Ast_defs.Cinterface ->
            (* ensure that we implement dynamic if we are a subclass/subinterface of a class/interface
             * that implements dynamic.  Upward well-formedness checks are performed in Typing_extends *)
            if
              Cls.get_support_dynamic_type parent_type
              && not support_dynamic_type
            then
              error_parent_support_dynamic_type parent_type support_dynamic_type
          | Ast_defs.(Cenum | Cenum_class _ | Ctrait) -> ()
        end
        | Decl_entry.DoesNotExist
        | Decl_entry.NotYetAvailable ->
          ())

(** Check methods with <<__Override>> have a corresponding overridden method. *)
let check_override_has_parent (c : ('a, 'b) class_) (tc : Cls.t) ~env : unit =
  if not Ast_defs.(is_c_trait c.c_kind) then
    (* We don't want to check __Override on traits. __Override on
       traits asserts that the *using* class has an inherited method
       that we're overriding. *)
    List.iter c.c_methods ~f:(fun m ->
        let id = snd m.m_name in
        match Cls.get_any_method ~is_static:m.m_static tc id with
        | Some ce -> begin
          if get_ce_superfluous_override ce then
            Typing_error_utils.add_typing_error
              ~env
              Typing_error.(
                primary
                @@ Primary.Should_not_be_override
                     { pos = fst m.m_name; class_id = snd c.c_name; id })
        end
        | None -> ())

(** Check __Override on class methods included from traits. *)
let check_used_methods_with_override env c (tc : Cls.t) : unit =
  let (class_pos, class_name) = c.c_name in

  let check_override (meth_name, ce) =
    if
      get_ce_superfluous_override ce
      && not (String.equal ce.ce_origin class_name)
    then
      match Env.get_class env ce.ce_origin with
      | Decl_entry.Found parent_class
        when Ast_defs.(is_c_trait (Cls.kind parent_class)) ->
        (* If we've included a method from a trait that has
           __Override, but there's no inherited method on this class
           that we're overridding, that's an error. *)
        Typing_error_utils.add_typing_error
          ~env
          Typing_error.(
            primary
            @@ Primary.Override_per_trait
                 {
                   pos = class_pos;
                   class_name;
                   meth_name;
                   trait_name = ce.ce_origin;
                   meth_pos = Lazy.force ce.ce_pos;
                 })
      | _ -> ()
  in

  if not Ast_defs.(is_c_trait c.c_kind) then (
    List.iter (Cls.methods tc) ~f:check_override;
    List.iter (Cls.smethods tc) ~f:check_override
  )

(** Check proper usage of the __Override attribute. *)
let check_override_keyword env c tc =
  check_override_has_parent c tc ~env;
  check_used_methods_with_override env c tc

(** If the class is sealed, check that the elements of the whitelist
    are descendants of the class and emit a warning if not.
    *)
let check_sealed env c =
  let is_enum = is_enum_or_enum_class c.c_kind in
  sealed_subtype c ~is_enum ~env

let check_class_require_non_strict_constraints env c tc =
  let req_non_strict_constraints =
    List.map (Cls.all_ancestor_req_class_requirements tc) ~f:(fun (_p, t) ->
        (mk (Reason.none, Tthis), Ast_defs.Constraint_eq, t))
    @ List.map (Cls.all_ancestor_req_this_as_requirements tc) ~f:(fun (_p, t) ->
          (mk (Reason.none, Tthis), Ast_defs.Constraint_as, t))
  in
  let tparams : decl_tparam list =
    List.map c.c_tparams ~f:(Decl_hint.aast_tparam_to_decl_tparam env.decl_env)
  in
  let (env, ty_err_opt1) =
    Phase.localize_and_add_generic_parameters_and_where_constraints
      ~ety_env:empty_expand_env
      env
      tparams
      req_non_strict_constraints
  in
  Option.iter ty_err_opt1 ~f:(Typing_error_utils.add_typing_error ~env);
  env

type class_parents = {
  extends: (Aast.hint * decl_ty) list;
  implements: (Aast.hint * decl_ty) list;
  uses: (Aast.hint * decl_ty) list;
  req_extends: (Aast.hint * decl_ty) list;
  req_implements: (Aast.hint * decl_ty) list;
  req_class: (Aast.hint * decl_ty) list;
  req_this_as: (Aast.hint * decl_ty) list;
  enum_includes: (Aast.hint * decl_ty) list option;
  xhp_attr_uses: (Aast.hint * decl_ty) list;
}

let class_parents_hints_to_types env c : class_parents =
  let hints_and_decl_tys hints =
    List.map hints ~f:(fun hint -> (hint, Decl_hint.hint env.decl_env hint))
  in
  let extends = hints_and_decl_tys c.c_extends in
  let implements = hints_and_decl_tys c.c_implements in
  let uses = hints_and_decl_tys c.c_uses in
  let (req_extends, req_implements, req_class, req_this_as) =
    split_reqs c.c_reqs
  in
  let req_extends = hints_and_decl_tys req_extends in
  let req_implements = hints_and_decl_tys req_implements in
  let req_class = hints_and_decl_tys req_class in
  let req_this_as = hints_and_decl_tys req_this_as in
  let enum_includes =
    Option.map c.c_enum ~f:(fun e -> hints_and_decl_tys e.e_includes)
  in
  let xhp_attr_uses = hints_and_decl_tys c.c_xhp_attr_uses in
  {
    extends;
    implements;
    uses;
    req_extends;
    req_implements;
    req_class;
    req_this_as;
    enum_includes;
    xhp_attr_uses;
  }

(** Add dependencies to parent constructors or produce errors if they're not a Tapply. *)
let check_parents_are_tapply_add_constructor_deps
    env c (parents : class_parents) =
  let {
    extends;
    implements;
    uses;
    req_extends;
    req_implements;
    req_class;
    req_this_as;
    enum_includes;
    xhp_attr_uses;
  } =
    parents
  in
  check_is_tapply_add_constructor_extends_dep env extends;
  check_is_tapply_add_constructor_extends_dep
    env
    implements
    ~skip_constructor_dep:
      (not (Ast_defs.is_c_trait c.c_kind || Ast_defs.is_c_abstract c.c_kind));
  check_is_tapply_add_constructor_extends_dep env uses;
  check_is_tapply_add_constructor_extends_dep env req_class;
  check_is_tapply_add_constructor_extends_dep env req_this_as;
  check_is_tapply_add_constructor_extends_dep env req_extends;
  check_is_tapply_add_constructor_extends_dep env req_implements;
  Option.iter enum_includes ~f:(check_is_tapply_add_constructor_extends_dep env);
  check_is_tapply_add_constructor_extends_dep
    env
    xhp_attr_uses
    ~skip_constructor_dep:true;
  ()

let check_class_attributes env ~cls =
  let (env, user_attributes) =
    let kind =
      if Ast_defs.is_c_enum cls.c_kind then
        SN.AttributeKinds.enum
      else if Ast_defs.is_c_enum_class cls.c_kind then
        SN.AttributeKinds.enumcls
      else
        SN.AttributeKinds.cls
    in
    Typing.attributes_check_def env kind cls.c_user_attributes
  in
  let (env, file_attrs) = Typing.file_attributes env cls.c_file_attributes in
  (env, (user_attributes, file_attrs))

(** Check type parameter definition, including variance, and add constraints to the environment. *)
let check_class_type_parameters_add_constraints env c tc =
  let env = check_class_require_non_strict_constraints env c tc in
  Typing_variance.class_def env c;
  env

(** Check wellformedness of all type hints in the class. *)
let check_hint_wellformedness_in_class env c parents =
  check_parents_are_tapply_add_constructor_deps env c parents;
  List.iter ~f:(Typing_error_utils.add_typing_error ~env)
  @@ Typing_type_wellformedness.class_ env c;
  env

(** Internal classes can carry `<<__TestsBypassVisibility>>` to become visible
  from configured test ancestors. If an internal member has the attribute, the
  containing internal class must also have it so the class symbol itself can be
  resolved from those tests. *)
let check_tests_bypass_visibility_on_internal_class env c =
  let class_is_internal = c.c_internal in
  let class_has_attr =
    Naming_attributes.mem
      SN.UserAttributes.uaTestsBypassVisibility
      c.c_user_attributes
  in
  if class_is_internal && not class_has_attr then begin
    let check_member attrs member_pos member_name =
      if Naming_attributes.mem SN.UserAttributes.uaTestsBypassVisibility attrs
      then
        Typing_error_utils.add_typing_error
          ~env
          (Typing_error.primary
             (Typing_error.Primary
              .Tests_bypass_visibility_on_member_without_class
                { pos = member_pos; member_name; class_name = snd c.c_name }))
    in
    List.iter c.c_methods ~f:(fun m ->
        check_member m.m_user_attributes (fst m.m_name) (snd m.m_name));
    List.iter c.c_vars ~f:(fun cv ->
        check_member cv.cv_user_attributes (fst cv.cv_id) (snd cv.cv_id))
  end

(** Perform all class wellformedness checks which don't involve the hierarchy:
  - attributes
  - property initialization checks
  - tests-bypass-visibility wellformedness
  - type parameter checks
  - type hint wellformedness
  - generic static properties *)
let class_wellformedness_checks env c tc (parents : class_parents) =
  (* Class and file level attributes are ran outside of the class context and thus should not
     have access to `self` in the environment, so we unset self while running these attribute checks.

     Note: This has the side effect of allowing internal classes to be referenced on class-level attributes
     that are applied to a public trait. This is intended and safe because class-level attributes are not
     copied to the use site of a trait.*)
  let (env, (user_attributes, file_attrs)) =
    Env.run_with_no_self env (check_class_attributes ~cls:c)
  in
  Nast_init_check.class_ env c;
  check_tests_bypass_visibility_on_internal_class env c;
  let env = check_class_type_parameters_add_constraints env c tc in
  let env = check_hint_wellformedness_in_class env c parents in
  check_no_generic_static_property env tc;
  (env, user_attributes, file_attrs)

(** Reject re-promoting (via a constructor parameter visibility modifier) a
  property that is already declared by an ancestor. Such re-declaration is
  error-prone and can make HHVM fatal at runtime when the inherited type-hint
  bound ends up violated (e.g. with generics). The un-promoted forms remain
  legal: declaring the property separately, or forwarding the value to
  `parent::__construct` without a visibility modifier. Private (non-LSB)
  ancestor properties are excluded since those are shadowed per-subclass rather
  than inherited. *)
let check_no_redeclared_promoted_props env c (parents : class_parents) =
  let (constructor, _, _) = split_methods c.c_methods in
  match constructor with
  | _
    when not
           (Typechecker_options.reject_promoted_property_redeclaration
              (Env.get_tcopt env)) ->
    (* Rollout flag, off by default: re-promoting an inherited property is a
       widespread pattern in existing code, so this is gated behind
       [reject_promoted_property_redeclaration] until callers are migrated. *)
    ()
  | None -> ()
  | Some m ->
    let ancestor_prop prop_name =
      List.find_map parents.extends ~f:(fun (_, parent_ty) ->
          match get_node parent_ty with
          | Tapply ((_, parent_class_name), _) ->
            (match Env.get_class env parent_class_name with
            | Decl_entry.Found parent_cls ->
              (* Private (non-LSB) properties are effectively cloned into each
                 subclass, so re-declaring one is legal shadowing rather than a
                 conflicting redeclaration. *)
              Env.get_prop env parent_cls prop_name
              |> Option.filter ~f:(fun ce ->
                     not (class_elt_is_private_not_lsb ce))
            | Decl_entry.DoesNotExist
            | Decl_entry.NotYetAvailable ->
              None)
          | _ -> None)
    in
    List.iter m.m_params ~f:(fun param ->
        match param.param_visibility with
        | None -> ()
        | Some _ ->
          let prop_name =
            String.chop_prefix_if_exists ~prefix:"$" param.param_name
          in
          (match ancestor_prop prop_name with
          | None -> ()
          | Some parent_ce ->
            Typing_error_utils.add_typing_error
              ~env
              Typing_error.(
                primary
                @@ Primary.Redeclaring_promoted_property
                     {
                       pos = param.param_pos;
                       prop_name;
                       parent_pos = Lazy.force parent_ce.ce_pos;
                       parent_name = parent_ce.ce_origin;
                     })))

(** Perform all hierarchy checks,
  i.e. checking that making this class a child of its parents is legal:
  - requirements from `require` statements
  - duplicate parents
  - parents can have children (not final or const)
  - __Sealed attribute
  - dynamic-type related attributes w.r.t. parents
  - __Disposable attribute w.r.t. parents
  - individual member checks:
    - subtyping parent members
    - not redeclaring class properties
    - __Override attribute check
    - enum inclusions
    - abstract members in concrete class
    - non-const members in const class *)
let class_hierarchy_checks env c tc (parents : class_parents) =
  if skip_hierarchy_checks (Env.get_ctx env) then
    env
  else
    let (pc, _) = c.c_name in
    let {
      extends;
      implements;
      uses;
      req_extends;
      req_implements;
      req_class = _;
      req_this_as = _;
      enum_includes = _;
      xhp_attr_uses = _;
    } =
      parents
    in
    let env = Typing_requirements.check_class env pc tc in
    check_override_keyword env c tc;
    check_enum_includes env c;
    check_implements_or_extends_unique implements ~env;
    check_implements_or_extends_unique extends ~env;
    let env =
      check_multiple_instantiation_inheritance
        env
        ~current_type:(snd c.c_name, c.c_kind)
        (c.c_implements @ c.c_extends @ c.c_uses)
    in
    check_parent env c tc;
    check_no_redeclared_promoted_props env c parents;
    check_consistent_construct_not_abstract_final env c tc;
    check_parents_sealed env c tc;
    check_sealed env c;
    let (_ : env) =
      check_generic_class_with_SupportDynamicType env c tc (extends @ implements)
    in
    if Cls.has_const_attribute tc then
      List.iter c.c_uses ~f:(check_non_const_trait_members pc env);
    let impl = extends @ implements @ uses in
    let impl =
      if TCO.require_extends_implements_ancestors (Env.get_tcopt env) then
        impl @ req_extends @ req_implements
      else
        impl
    in
    let env =
      Typing_extends.check_implements_extends_uses
        env
        ~implements:(List.map implements ~f:snd)
        ~parents:impl
        (c, tc)
    in
    if Typing_disposable.is_disposable_class env tc then
      List.iter
        (c.c_extends @ c.c_uses)
        ~f:(Typing_disposable.enforce_is_disposable env);
    env

let check_class_members env c tc =
  let (static_vars, vars) = split_vars c.c_vars in
  let (constructor, static_methods, methods) = split_methods c.c_methods in
  let is_disposable = Typing_disposable.is_disposable_class env tc in
  let is_noautodynamic =
    Naming_attributes.mem
      SN.UserAttributes.uaNoAutoDynamic
      c.Aast.c_user_attributes
  in
  let (env, typed_vars) =
    List.map_env
      env
      vars
      ~f:(class_var_def ~is_static:false ~is_noautodynamic tc)
  in
  let typed_methods =
    List.filter_map methods ~f:(method_def ~is_disposable env tc)
  in
  let typed_methods = Tast_with_dynamic.collect typed_methods in
  let (env, typed_typeconsts) =
    List.map_env env c.c_typeconsts ~f:(typeconst_def c)
  in
  let in_enum_class = Env.is_enum_class env (snd c.c_name) in
  let (env, consts) =
    List.map_env env c.c_consts ~f:(class_const_def ~in_enum_class c tc)
  in
  let (typed_consts, const_types) = List.unzip consts in
  let env =
    Typing_enum.enum_class_check
      ~name_pos:(fst c.c_name)
      env
      tc
      c.c_consts
      const_types
  in
  let typed_constructor = class_constr_def ~is_disposable env tc constructor in
  let env = Env.set_static env in
  let (env, typed_static_vars) =
    List.map_env
      env
      static_vars
      ~f:(class_var_def ~is_static:true ~is_noautodynamic tc)
  in
  let typed_static_methods =
    List.filter_map static_methods ~f:(method_def ~is_disposable env tc)
  in
  let typed_static_methods = Tast_with_dynamic.collect typed_static_methods in
  let typed_methods =
    match typed_constructor with
    | None -> Tast_with_dynamic.append typed_static_methods typed_methods
    | Some ms ->
      Tast_with_dynamic.append
        (Tast_with_dynamic.cons ms typed_static_methods)
        typed_methods
  in
  let typed_members =
    ( typed_consts,
      typed_typeconsts,
      typed_vars,
      typed_static_vars,
      typed_methods )
  in
  (env, typed_members)

let class_def_ ~restore_pos env c tc =
  let parents = class_parents_hints_to_types env c in
  let (env, user_attributes, file_attrs) =
    class_wellformedness_checks env c tc parents
  in
  let env = class_hierarchy_checks env c tc parents in
  Typing_needs_concrete.check_class_def env c tc;
  let ( env,
        ( typed_consts,
          typed_typeconsts,
          typed_vars,
          typed_static_vars,
          typed_methods ) ) =
    if
      env.genv.tcopt.po.ignore_string_methods
      && String.equal SN.Classes.cString (snd c.c_name)
    then
      (env, Tast_with_dynamic.([], [], [], [], mk_without_dynamic []))
    else
      check_class_members env c tc
  in
  let (env, tparams) = class_type_param env c.c_tparams in
  let (env, e1) = Typing_solver.solve_all_unsolved_tyvars env in
  check_SupportDynamicType env c tc;
  Option.iter ~f:(Typing_error_utils.add_typing_error ~env) e1;
  let env = restore_pos env in
  let mk_class_ typed_methods =
    {
      Aast.c_span = c.c_span;
      Aast.c_annotation = Env.save (Env.get_tpenv env) env;
      Aast.c_mode = c.c_mode;
      Aast.c_final = c.c_final;
      Aast.c_is_xhp = c.c_is_xhp;
      Aast.c_has_xhp_keyword = c.c_has_xhp_keyword;
      Aast.c_kind = c.c_kind;
      Aast.c_name = c.c_name;
      Aast.c_tparams = tparams;
      Aast.c_extends = c.c_extends;
      Aast.c_uses = c.c_uses;
      Aast.c_xhp_attr_uses = c.c_xhp_attr_uses;
      Aast.c_xhp_category = c.c_xhp_category;
      Aast.c_reqs = c.c_reqs;
      Aast.c_implements = c.c_implements;
      Aast.c_consts = typed_consts;
      Aast.c_typeconsts = typed_typeconsts;
      Aast.c_vars = typed_static_vars @ typed_vars;
      Aast.c_methods = typed_methods;
      Aast.c_file_attributes = file_attrs;
      Aast.c_user_attributes = user_attributes;
      Aast.c_namespace = c.c_namespace;
      Aast.c_enum = c.c_enum;
      Aast.c_doc_comment = c.c_doc_comment;
      Aast.c_xhp_children = c.c_xhp_children;
      Aast.c_xhp_attrs = [];
      Aast.c_emit_id = c.c_emit_id;
      Aast.c_internal = c.c_internal;
      Aast.c_module = c.c_module;
      Aast.c_docs_url = c.c_docs_url;
      Aast.c_package = c.c_package;
    }
  in
  {
    Tast_with_dynamic.under_normal_assumptions =
      mk_class_ typed_methods.Tast_with_dynamic.under_normal_assumptions;
    under_dynamic_assumptions =
      Option.map
        typed_methods.Tast_with_dynamic.under_dynamic_assumptions
        ~f:mk_class_;
  }

let setup_env_for_class_def_check ctx c =
  let env = EnvFromDef.class_env ~origin:Decl_counters.TopLevel ctx c in
  let env = Env.set_current_module env c.c_module in
  let env = Env.set_internal env c.c_internal in
  let env = Env.set_current_package_membership env c.c_package in
  env

let class_def ctx (c : _ class_) =
  let env = setup_env_for_class_def_check ctx c in
  let pos = fst c.c_name in
  let (env, restore_pos) = Env.set_inference_env_pos env (Some pos) in
  match Env.get_class env (snd c.c_name) with
  | Decl_entry.DoesNotExist
  | Decl_entry.NotYetAvailable ->
    Hack_event_logger.decl_consistency_bug
      "Decl consistency: class_def, but can't find a decl"
      ~data:(snd c.c_name)
      ~pos:(Pos.to_relative_string (fst c.c_name) |> Pos.string);
    None
  | Decl_entry.Found tc ->
    let env =
      Env.set_support_dynamic_type env (Cls.get_support_dynamic_type tc)
    in
    Env.make_depend_on_current_module env;
    Typing_helpers.add_decl_errors ~env (Cls.decl_errors tc);
    let result = class_def_ ~restore_pos env c tc in
    Some result

type class_member_standalone_check_env = {
  cls: Cls.t;
  env: env;
  class_: Nast.class_;
}

let make_class_member_standalone_check_env ctx class_ =
  let env = setup_env_for_class_def_check ctx class_ in
  let pos = fst class_.c_name in
  (* env flows out; restore will happen when method_def sets its own position *)
  let (env, restore_pos) = Env.set_inference_env_pos env (Some pos) in
  let name = Ast_defs.get_id class_.c_name in
  let open Option in
  Env.get_class env name |> Decl_entry.to_option >>| fun cls ->
  let env =
    Env.set_support_dynamic_type env (Cls.get_support_dynamic_type cls)
  in
  let env = check_class_type_parameters_add_constraints env class_ cls in
  let env = restore_pos env in
  (env, { env; cls; class_ })

let method_def_standalone standalone_env method_name =
  let open Option in
  let is_disposable =
    Typing_disposable.is_disposable_class standalone_env.env standalone_env.cls
  in
  List.find standalone_env.class_.Aast.c_methods ~f:(fun m ->
      String.equal (snd m.Aast.m_name) method_name)
  >>= fun method_ ->
  let env =
    if method_.m_static then
      Env.set_static standalone_env.env
    else
      standalone_env.env
  in
  method_def ~is_disposable env standalone_env.cls method_
