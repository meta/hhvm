(*
 * Copyright (c) 2015, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

(* This module implements the typing.
 *
 * Given an Nast.program, it infers the type of all the local
 * variables, and checks that all the types are correct (aka
 * consistent) *)
open Hh_prelude
open Common
open Aast
open Typing_defs
open Typing_env_types
open Typing_helpers
module Reason = Typing_reason
module Env = Typing_env
module MakeType = Typing_make_type
module Phase = Typing_phase
module EnvFromDef = Typing_env_from_def
module TCO = Typechecker_options
module SN = Naming_special_names
module Profile = Typing_toplevel_profile
module Enable = Typing_toplevel_enable

let check_if_this_def_is_the_winner ctx name_type (pos, name) : bool =
  if String.is_empty name || String.equal name "\\" then
    (* In case of some parse errors, the AST contains definitions with empty names.
       There's no useful duplicate-name-check we can do here.
       Return [false] because there's no such thing as the winner for the empty name. *)
    false
  else
    match Decl_provider.is_this_def_the_winner ctx name_type (pos, name) with
    | Decl_provider.Winner -> true
    | Decl_provider.Loser_to prev_pos ->
      let custom_err_config =
        Provider_context.get_tcopt ctx |> TCO.custom_error_config
      in
      Diagnostics.add_diagnostic
        (Naming_error_utils.to_user_diagnostic
           (Naming_error.Error_name_already_bound { name; pos; prev_pos })
           custom_err_config);
      false
    | Decl_provider.Not_found ->
      (* This is impossible! The fact that we have an AST def for [name] implies
         that Decl_provider should be able to find at least one decl with this name.
         Only explanations are (1) weirdness with Provider_context.entry which
         gives slightly different answers from what direct-decl-parser provides
         e.g. in case of incorrect parse trees, or order of winner; (2) disk race
         where the file on disk has changed but we haven't yet updated naming table. *)
      Hack_event_logger.decl_consistency_bug
        "Decl consistency: Ast definition but no decl"
        ~data:name
        ~pos:(Pos.to_relative_string pos |> Pos.string);
      false

let fun_def ctx fd : Tast.fun_def Tast_with_dynamic.t option =
  let filename = Pos.filename (fst fd.fd_name) in
  if
    (Provider_context.get_popt ctx).ignore_string_methods
    && Relative_path.(is_hhi (prefix filename))
    && String.equal (Relative_path.suffix filename) "string.hhi"
  then
    None
  else
    let tcopt = Provider_context.get_tcopt ctx in
    Enable.if_matches_regexp tcopt None ~default:None fd.fd_name @@ fun () ->
    let f = fd.fd_fun in
    Profile.measure_elapsed_time_and_report tcopt None fd.fd_name @@ fun () ->
    Counters.count Counters.Category.Typing_toplevel @@ fun () ->
    Diagnostics.run_with_span f.f_span @@ fun () ->
    let (_ : bool) =
      check_if_this_def_is_the_winner ctx File_info.Fun fd.fd_name
    in
    let env = EnvFromDef.fun_env ~origin:Decl_counters.TopLevel ctx fd in
    with_timeout env fd.fd_name @@ fun env ->
    let pos = fst fd.fd_name in
    let env = Env.open_tyvars env (fst fd.fd_name) in
    let env = Env.set_env_callable_pos env pos in
    let env = Env.set_env_function_pos env f.f_span in
    let (env, restore_pos) = Env.set_inference_env_pos env (Some pos) in
    (* Set the package membership before checking attributes so that package
       boundary checks on attribute classes (for strict-isolation packages) see
       the correct current package. *)
    let env = Env.set_current_package_membership env fd.fd_package in
    let (env, user_attributes) =
      Typing.attributes_check_def env SN.AttributeKinds.fn f.f_user_attributes
    in
    let (env, file_attrs) = Typing.file_attributes env fd.fd_file_attributes in
    let (env, cap_ty, unsafe_cap_ty) =
      Typing_coeffects.type_capability
        env
        f.f_ctxs
        f.f_unsafe_ctxs
        (fst fd.fd_name)
    in
    let env = Env.set_current_module env fd.fd_module in
    let env = Env.set_internal env fd.fd_internal in
    let env = Env.set_soft_package_from_attr env f.f_user_attributes in
    let env =
      if
        Naming_attributes.mem
          SN.UserAttributes.uaSupportDynamicType
          f.f_user_attributes
        || Naming_attributes.mem
             SN.UserAttributes.uaDynamicallyCallable
             f.f_user_attributes
      then
        Env.set_support_dynamic_type env true
      else
        env
    in
    let no_auto_likes =
      Naming_attributes.mem SN.UserAttributes.uaNoAutoLikes f.f_user_attributes
    in
    let env =
      if no_auto_likes then
        Env.set_no_auto_likes env true
      else
        env
    in
    let env = Env.assert_packages_loaded_from_attr env f.f_user_attributes in
    (* Is sound dynamic enabled, and the function marked <<__SupportDynamicType>> explicitly or implicitly? *)
    let sdt_function = Env.get_support_dynamic_type env in
    List.iter ~f:(Typing_error_utils.add_typing_error ~env)
    @@ Typing_type_wellformedness.fun_def env fd;
    Env.make_depend_on_current_module env;
    let (env, ty_err_opt) =
      Phase.localize_and_add_ast_generic_parameters_and_where_constraints
        env
        ~ignore_errors:false
        fd.fd_tparams
        fd.fd_where_constraints
    in
    Option.iter ~f:(Typing_error_utils.add_typing_error ~env) ty_err_opt;
    let env = Env.set_fn_kind env f.f_fun_kind in
    let (return_decl_ty, params_decl_ty) =
      hint_fun_decl ~params:f.f_params ~ret:f.f_ret env
    in
    let hint_pos =
      match f.f_ret with
      | (_, None) -> fst fd.fd_name
      | (_, Some (pos, _)) -> pos
    in
    (* Do we need to check the body of the function again, under dynamic assumptions? *)
    let sdt_dynamic_check_required =
      sdt_function
      && not
           (Typing_dynamic.function_parameters_safe_for_dynamic
              ~this_class:None
              env
              params_decl_ty)
    in
    let ety_env =
      empty_expand_env_with_on_error
        (Typing_error.Reasons_callback.invalid_type_hint hint_pos)
    in
    let (env, return_ty) =
      Typing_return.make_return_type
        ~ety_env
        ~this_class:None
        env
        ~supportdyn:(sdt_function && not sdt_dynamic_check_required)
        ~hint_pos
        ~explicit:return_decl_ty
        ~default:None
    in
    let return =
      Typing_return.make_info
        hint_pos
        f.f_fun_kind
        f.f_user_attributes
        env
        return_ty
    in
    let (env, _) =
      Typing_coeffects.register_capabilities env cap_ty unsafe_cap_ty
    in
    let sound_dynamic_check_saved_env = env in
    let (env, param_tys) =
      Typing_param.make_param_local_tys
        ~dynamic_mode:false
        ~no_auto_likes
        env
        params_decl_ty
        f.f_params
    in
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
        f.f_ctxs
        param_tys
        f.f_params
    in
    let env = set_tyvars_variance_in_callable env return_ty param_tys in
    let local_tpenv = Env.get_tpenv env in
    let disable =
      Naming_attributes.mem
        SN.UserAttributes.uaDisableTypecheckerInternal
        f.f_user_attributes
    in
    Typing_memoize.check_function env f;
    Typing_memoize.check_erased_generics
      env
      f.f_user_attributes
      fd.fd_tparams
      f.f_params
      f.f_ret;
    let ((env, tb), had_errors) =
      Diagnostics.run_and_check_for_errors (fun () ->
          Typing.fun_
            ~native:(Typing_native.is_native_fun ~env f)
            ~disable
            env
            return
            pos
            f.f_body
            f.f_fun_kind)
    in
    begin
      match hint_of_type_hint f.f_ret with
      | None ->
        Typing_error_utils.add_typing_error
          ~env
          Typing_error.(primary @@ Primary.Expecting_return_type_hint pos)
      | Some _ -> ()
    end;
    let (env, tparams) = List.map_env env fd.fd_tparams ~f:Typing.type_param in
    let (env, e1) = Typing_solver.close_tyvars_and_solve env in
    let (env, e2) = Typing_solver.solve_all_unsolved_tyvars env in
    let ret_hint = hint_of_type_hint f.f_ret in
    let fun_ =
      {
        Aast.f_annotation = Env.save local_tpenv env;
        Aast.f_readonly_this = f.f_readonly_this;
        Aast.f_tparams = [];
        (* For function definitions the declared type parameters appear on the
           definition rather then function - see [fd_tparams] below *)
        Aast.f_span = f.f_span;
        Aast.f_readonly_ret = f.f_readonly_ret;
        Aast.f_ret = (return_ty, ret_hint);
        Aast.f_params = typed_params;
        Aast.f_ctxs = f.f_ctxs;
        Aast.f_unsafe_ctxs = f.f_unsafe_ctxs;
        Aast.f_fun_kind = f.f_fun_kind;
        Aast.f_user_attributes = user_attributes;
        Aast.f_body = { Aast.fb_ast = tb };
        Aast.f_external = f.f_external;
        Aast.f_hidden = false;
        Aast.f_doc_comment = f.f_doc_comment;
      }
    in
    let under_normal_assumptions =
      {
        Aast.fd_mode = fd.fd_mode;
        Aast.fd_name = fd.fd_name;
        Aast.fd_fun = fun_;
        Aast.fd_file_attributes = file_attrs;
        Aast.fd_namespace = fd.fd_namespace;
        Aast.fd_internal = fd.fd_internal;
        Aast.fd_module = fd.fd_module;
        Aast.fd_tparams = tparams;
        Aast.fd_where_constraints = fd.fd_where_constraints;
        Aast.fd_package = fd.fd_package;
      }
    in
    let fundef_of_dynamic
        (dynamic_env, dynamic_params, dynamic_body, dynamic_return_ty) =
      let open Aast in
      {
        under_normal_assumptions with
        fd_fun =
          {
            under_normal_assumptions.fd_fun with
            f_annotation = Env.save local_tpenv dynamic_env;
            f_ret = (dynamic_return_ty, ret_hint);
            f_params = dynamic_params;
            f_body = { fb_ast = dynamic_body };
          };
      }
    in
    let (under_normal_assumptions, under_dynamic_assumptions) =
      if
        sdt_dynamic_check_required
        && (not had_errors)
        && not (TCO.skip_check_under_dynamic tcopt)
      then
        let env = { env with checked = Tast.CUnderNormalAssumptions } in
        let under_normal_assumptions =
          let f_annotation = Env.save local_tpenv env in
          Aast.
            {
              under_normal_assumptions with
              fd_fun = { under_normal_assumptions.fd_fun with f_annotation };
            }
        in
        let dynamic_components =
          Typing.check_function_dynamically_callable
            ~this_class:None
            sound_dynamic_check_saved_env
            (Some fd.fd_name)
            f
            params_decl_ty
            return_ty
        in
        (under_normal_assumptions, Some (fundef_of_dynamic dynamic_components))
      else
        (under_normal_assumptions, None)
    in
    let env = restore_pos env in
    let ty_err_opt = Option.merge e1 e2 ~f:Typing_error.both in
    Option.iter ~f:(Typing_error_utils.add_typing_error ~env) ty_err_opt;
    { Tast_with_dynamic.under_normal_assumptions; under_dynamic_assumptions }

let class_def ctx class_ =
  Counters.count Counters.Category.Typing_toplevel @@ fun () ->
  Diagnostics.run_with_span class_.c_span @@ fun () ->
  if check_if_this_def_is_the_winner ctx File_info.Class class_.c_name then
    (* [Typing_class.class_def] is unusual in that it can't work properly
       unless it's the winner and has the same capitalization.
       If either isn't met, it will report to telemetry and return None. *)
    Typing_class.class_def ctx class_
  else
    None

let typedef_def ctx typedef =
  let tcopt = Provider_context.get_tcopt ctx in
  Profile.measure_elapsed_time_and_report tcopt None typedef.t_name @@ fun () ->
  Diagnostics.run_with_span typedef.t_span @@ fun () ->
  let (_ : bool) =
    check_if_this_def_is_the_winner ctx File_info.Typedef typedef.t_name
  in
  Typing_typedef.typedef_def ctx typedef

let gconst_def ctx cst =
  let tcopt = Provider_context.get_tcopt ctx in
  Profile.measure_elapsed_time_and_report tcopt None cst.cst_name @@ fun () ->
  Counters.count Counters.Category.Typing_toplevel @@ fun () ->
  Diagnostics.run_with_span cst.cst_span @@ fun () ->
  let (_ : bool) =
    check_if_this_def_is_the_winner ctx File_info.Const cst.cst_name
  in
  let env = EnvFromDef.gconst_env ~origin:Decl_counters.TopLevel ctx cst in
  let env = Env.set_current_package_membership env cst.cst_package in
  Env.make_depend_on_current_module env;
  let pos = fst cst.cst_name in
  let (env, restore_pos) = Env.set_inference_env_pos env (Some pos) in
  List.iter ~f:(Typing_error_utils.add_typing_error ~env)
  @@ Typing_type_wellformedness.global_constant env cst;
  let (typed_cst_value, (env, ty_err_opt)) =
    let ((_, _, init) as value) = cst.cst_value in
    match (cst.cst_type, init) with
    | (Some _, Omitted) when Env.is_hhi env ->
      (* We don't require global consts to have a value set for decl purposes
       * in HHI files so it may just be a placeholder, therefore we don't care
       * about checking the value and simply pass it through *)
      let (env, te, _ty) = Typing.expr env value in
      (te, (env, None))
    | (Some hint, _) ->
      let ty = Decl_hint.hint env.decl_env hint in
      let (enforced, ty) =
        Typing_enforceability.compute_enforced_ty ~this_class:None env ty
      in
      let ((env, ty_err_opt1), dty) =
        Phase.localize_no_subst env ~ignore_errors:false ty
      in
      let (env, te, value_type) =
        let expected = ExpectedTy.make (fst hint) Reason.URhint dty in
        Typing.expr_with_pure_coeffects env ~expected value
      in
      let (env, ty_err_opt2) =
        Typing_coercion.coerce_type
          (fst hint)
          Reason.URhint
          env
          value_type
          dty
          enforced
          Typing_error.Callback.unify_error
      in
      let ty_err_opt =
        Option.merge ~f:Typing_error.both ty_err_opt1 ty_err_opt2
      in
      (te, (env, ty_err_opt))
    | (None, _) ->
      (if
       (not (Decl_utils.is_literal_with_trivially_inferable_type value))
       && not (Env.is_hhi env)
      then
        let custom_err_config =
          Provider_context.get_tcopt ctx |> TCO.custom_error_config
        in
        Diagnostics.add_diagnostic
          (Naming_error_utils.to_user_diagnostic
             (Naming_error.Missing_typehint (fst cst.cst_name))
             custom_err_config));
      let (env, te, _value_type) = Typing.expr_with_pure_coeffects env value in
      (te, (env, None))
  in

  Option.iter ty_err_opt ~f:(Typing_error_utils.add_typing_error ~env);
  let env = restore_pos env in
  {
    Aast.cst_annotation = Env.save (Env.get_tpenv env) env;
    Aast.cst_mode = cst.cst_mode;
    Aast.cst_name = cst.cst_name;
    Aast.cst_type = cst.cst_type;
    Aast.cst_value = typed_cst_value;
    Aast.cst_namespace = cst.cst_namespace;
    Aast.cst_span = cst.cst_span;
    Aast.cst_emit_id = cst.cst_emit_id;
    Aast.cst_module = cst.cst_module;
    Aast.cst_package = cst.cst_package;
  }

let module_def ctx md =
  Counters.count Counters.Category.Typing_toplevel @@ fun () ->
  Diagnostics.run_with_span md.md_span @@ fun () ->
  let (_ : bool) =
    check_if_this_def_is_the_winner ctx File_info.Module md.md_name
  in
  let env = EnvFromDef.module_env ~origin:Decl_counters.TopLevel ctx md in
  let pos = fst md.md_name in
  let (env, restore_pos) = Env.set_inference_env_pos env (Some pos) in
  let (env, file_attributes) =
    Typing.file_attributes env md.md_file_attributes
  in
  let (env, user_attributes) =
    Typing.attributes_check_def
      env
      SN.AttributeKinds.module_
      md.md_user_attributes
  in
  let env = restore_pos env in
  {
    md with
    Aast.md_annotation = Env.save (Env.get_tpenv env) env;
    Aast.md_user_attributes = user_attributes;
    Aast.md_file_attributes = file_attributes;
  }

(* Apply the effect of a file-level [<<file: __EnableUnstableFeatures(...)>>]
   attribute to the typechecker options carried by [ctx]. Both per-file
   check entry points (this module's [nast_to_tast] and
   [Typing_check_job.calc_errors_and_tast]) must run this before checking a
   definition, otherwise feature gating (e.g. [__GatedByFeatureFlag]) fires
   spuriously for definitions that legitimately opted into an unstable feature. *)
let set_tcopt_unstable_features ctx { Aast.fa_user_attributes; _ } =
  match
    Naming_attributes.find
      SN.UserAttributes.uaEnableUnstableFeatures
      fa_user_attributes
  with
  | None -> ctx
  | Some { ua_name = _; ua_params } ->
    List.fold ua_params ~init:ctx ~f:(fun ctx (_, _, feature) ->
        match feature with
        | Aast.String s ->
          (* Always add to the unstable features set *)
          let ctx =
            Provider_context.map_tcopt
              ~f:(fun t ->
                Global_options.
                  {
                    t with
                    tco_enabled_unstable_features =
                      S_set.add s t.tco_enabled_unstable_features;
                  })
              ctx
          in
          (* Then set any feature-specific options *)
          let ctx =
            if String.equal s SN.UnstableFeatures.expression_trees then
              Provider_context.map_tcopt
                ~f:(fun t ->
                  Typechecker_options.set_tco_enable_expression_trees t true)
                ctx
            else
              ctx
          in
          let ctx =
            if String.equal s SN.UnstableFeatures.recursive_case_types then
              Provider_context.map_tcopt
                ~f:Typechecker_options.enable_recursive_case_types
                ctx
            else
              ctx
          in
          ctx
        | _ -> ctx)

let nast_to_tast ~(do_tast_checks : bool) (ctx : Provider_context.t) nast :
    Tast.program Tast_with_dynamic.t =
  let convert_def def =
    Worker_cancel.raise_if_stop_requested ();
    match def with
    (* Sometimes typing will just return `None` but that should only be the case
     * if an error had already been registered e.g. in naming
     *)
    | Fun f -> begin
      let ctx =
        List.fold
          f.Aast.fd_file_attributes
          ~init:ctx
          ~f:set_tcopt_unstable_features
      in
      match fun_def ctx f with
      | Some fs -> Some (Tast_with_dynamic.map ~f:(fun f -> Aast.Fun f) fs)
      | None -> None
    end
    | Constant gc ->
      Some
        (Tast_with_dynamic.mk_without_dynamic
        @@ Aast.Constant (gconst_def ctx gc))
    | Typedef td ->
      let ctx =
        List.fold
          td.Aast.t_file_attributes
          ~init:ctx
          ~f:set_tcopt_unstable_features
      in
      Some
        (Tast_with_dynamic.mk_without_dynamic
        @@ Aast.Typedef (typedef_def ctx td))
    | Class c -> begin
      let ctx =
        List.fold
          c.Aast.c_file_attributes
          ~init:ctx
          ~f:set_tcopt_unstable_features
      in
      match class_def ctx c with
      | Some cs -> Some (Tast_with_dynamic.map ~f:(fun c -> Aast.Class c) cs)
      | None -> None
    end
    | Stmt ((pos, stmt_) as s) ->
      let () =
        Hack_event_logger.invariant_violation_bug
          ~pos:(Pos.show_absolute (Pos.to_absolute pos))
          ~data:
            (let show_ex_en _ _ = () in
             Aast_defs.show_stmt_ show_ex_en show_ex_en stmt_)
          "Top-level statements should be unreachable in nast_to_tast because we convert top-level statements to functions in the Naming phase."
      in
      let env = Typing_env_types.empty ctx Relative_path.default ~droot:None in
      Some
        (Tast_with_dynamic.mk_without_dynamic
        @@ Aast.Stmt (snd (Typing.stmt env s)))
    | Module md ->
      Some
        (Tast_with_dynamic.mk_without_dynamic @@ Aast.Module (module_def ctx md))
    | SetModule sm ->
      Some (Tast_with_dynamic.mk_without_dynamic @@ Aast.SetModule sm)
    | ClassAlias cr ->
      (* TODO(T264548072) *)
      let dummy_ty = MakeType.nothing Reason.none in
      let cr =
        Aast.map_class_alias
          (fun _ -> dummy_ty)
          (fun _ -> Tast.dummy_saved_env)
          cr
      in
      Some (Tast_with_dynamic.mk_without_dynamic @@ Aast.ClassAlias cr)
    | Namespace _
    | NamespaceUse _
    | SetNamespaceEnv _
    | FileAttributes _ ->
      failwith
        "Invalid nodes in NAST. These nodes should be removed during naming."
  in
  if do_tast_checks then Nast_check.program ctx nast;
  let tast = List.filter_map nast ~f:convert_def |> Tast_with_dynamic.collect in
  (* We only do TAST checks for non-dynamic components *)
  if do_tast_checks then begin
    Tast_check.program ctx tast.Tast_with_dynamic.under_normal_assumptions;
    (* Intersect warning checks with dynamic TAST to determine trust *)
    match tast.Tast_with_dynamic.under_dynamic_assumptions with
    | None ->
      (* If there is no dynamic TAST, everything is trusted *)
      Diagnostics.mark_all_warnings_trusted ()
    | Some dyn_tast ->
      (* Otherwise, find which warnings also fire under the dynamic TAST; since
         these cannot be false positives we can mark the as trusted *)
      let dynamic_keys =
        Diagnostics.collect_warning_keys (fun () ->
            Tast_check.warning_program ctx dyn_tast)
      in
      Diagnostics.mark_warnings_trusted dynamic_keys
  end;
  tast
