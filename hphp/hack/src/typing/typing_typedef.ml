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
open Aast
open Tast
module Reason = Typing_reason
module Env = Typing_env
module SN = Naming_special_names
module Phase = Typing_phase
module EnvFromDef = Typing_env_from_def

type constraint_direction =
  | As
  | Super

module Constraints : sig
  (** [check env tcstr constraint_direction name ty] checks that
  `ty <: tcstr` or `tcstr <: ty` depending on [constraint_direction] *)
  val check :
    Typing_env_types.env ->
    hint option ->
    constraint_direction ->
    pos * string ->
    ty ->
    Typing_env_types.env * Typing_error.t option
end = struct
  let report_cyclic_constraint env cycles cstr_direction =
    match cstr_direction with
    | As ->
      Type_expansions.report cycles
      |> Option.iter ~f:(Typing_error_utils.add_typing_error ~env)
    | Super -> ()

  let check_subtype_constraint env ~ty ~cstr_ty t_pos cstr_direction =
    let (ty_sub, ty_super) =
      match cstr_direction with
      | As -> (ty, cstr_ty)
      | Super -> (cstr_ty, ty)
    in
    Typing_ops.sub_type
      ~class_sub_classname:false
      t_pos
      Reason.URnewtype_cstr
      env
      ty_sub
      ty_super
      Typing_error.Callback.newtype_alias_must_satisfy_constraint

  let check env tcstr (cstr_direction : constraint_direction) (t_pos, t_name) ty
      : Typing_env_types.env * Typing_error.t option =
    match tcstr with
    | Some tcstr ->
      let ((env, ty_err_opt1, cycles), cstr_ty) =
        Phase.localize_hint_no_subst_report_cycles
          env
          ~ignore_errors:false
          tcstr
          ~report_cycle:(t_pos, Type_expansions.Expandable.Type_alias t_name)
      in
      report_cyclic_constraint env cycles cstr_direction;
      let (env, ty_err_opt2) =
        check_subtype_constraint env t_pos ~ty ~cstr_ty cstr_direction
      in
      (env, Option.merge ~f:Typing_error.both ty_err_opt1 ty_err_opt2)
    | _ -> (env, None)
end

(** Checks that variants of a case type are disjoint *)
let check_overlapping env (t_pos, t_name) hints =
  (* Given two types with their associated data types, check if
       the data types overlap. If they do report an error *)
  let check_overlapping data_type1 acc data_type2 =
    match
      Typing_case_type_check.check_overlapping
        ~pos:t_pos
        ~name:t_name
        env
        data_type1
        data_type2
    with
    | None -> acc
    | Some err -> Typing_error.casetype err :: acc
  in

  (* We check for overlaps pairwise between each variant type in the union *)
  let rec pairwise_check acc = function
    | data_type :: rest ->
      let acc = List.fold ~init:acc ~f:(check_overlapping data_type) rest in
      pairwise_check acc rest
    | [] -> acc
  in
  let (env, data_types) =
    hints
    |> List.fold_map
         ~init:env
         ~f:
           (Typing_case_type_check.data_type_from_hint
              ~safe_for_are_disjoint:false)
  in
  let err = data_types |> pairwise_check [] |> Typing_error.multiple_opt in
  (env, err)

(** Forbid recursive type aliases like `A = A` and `A = ?A`
  (modulo indirection via other type aliases),
  something defined as contractiveness in Pierce's Types and Programming Languages.
  Note that we'd also want to forbid any recursion under union like `A = A | int`,
  but these are already forbidden by `check_overlapping`, so left to check
  are A = A and A = ?A. *)
let check_invalid_recursive_case_type
    (env : Typing_env_types.env) ((pos, name) : sid) (hints : _ list) =
  let rec is_invalid_single_type (ty : Typing_defs.locl_ty) =
    let open Typing_defs in
    match (get_node ty : locl_phase ty_) with
    | Tnewtype (id, _targs, _bound) -> String.equal name id
    | Toption ty -> is_invalid_single_type ty
    | Taccess _
    | Tgeneric _
    | Tprim _
    | Tfun _
    | Ttuple _
    | Tclass_ptr _
    | Tshape (Shape_simple _ | Shape_splat _)
    | Tnonnull
    | Tvec_or_dict _
    | Tdynamic _
    | Tvar _
    | Tclass _
    | Tany _ ->
      false
    | Tunion tyl
    | Tintersection tyl ->
      List.exists tyl ~f:is_invalid_single_type
    | Tdependent _ ->
      failwith "Unexpected type Tdependent out of localizing a hint"
    | Tneg _ -> failwith "Unexpected type Tneg out of localizing a hint"
    | Tlabel _ -> failwith "Unexpected type Tlabel out of localizing a hint"
  in
  match hints with
  | [hint] ->
    let ty = Decl_hint.hint env.Typing_env_types.decl_env hint in
    let ((env, _err), ty) =
      Typing_phase.localize
        env
        ~ety_env:
          Typing_defs.
            {
              empty_expand_env with
              visibility_behavior = Always_expand_newtype;
              type_expansions =
                Type_expansions.empty_w_cycle_report
                  ~report_cycle:
                    (Some (pos, Type_expansions.Expandable.Type_alias name));
            }
        ty
    in
    let err =
      if is_invalid_single_type ty then
        Some
          (Typing_error.casetype
             (Typing_error.Primary.CaseType.Invalid_recursive { pos; name }))
      else
        None
    in
    (env, err)
  | _ ->
    (* Recursive types like `A = A | int` are already forbidden
       by `check_overlapping`, so we skip the check. *)
    (env, None)

(** Checks that variants of a case type are disjoint, and rule out other invalid recursive case types *)
let casetype_def env typedef =
  let {
    t_annotation = ();
    t_name;
    t_tparams = _;
    t_as_constraint = _;
    t_super_constraint = _;
    t_assignment;
    t_runtime_type = _;
    t_user_attributes = _;
    t_mode = _;
    t_namespace = _;
    t_span = _;
    t_emit_id = _;
    t_is_ctx = _;
    t_file_attributes = _;
    t_internal = _;
    t_module = _;
    t_docs_url = _;
    t_doc_comment = _;
    t_package = _;
  } =
    typedef
  in
  match t_assignment with
  | SimpleTypeDef _ -> env
  | CaseType (variant, variants) ->
    let hints = List.map (variant :: variants) ~f:(fun v -> v.tctv_hint) in
    let (env, err1) = check_overlapping env t_name hints in
    let (env, err2) = check_invalid_recursive_case_type env t_name hints in
    Option.iter ~f:(Typing_error_utils.add_typing_error ~env) err1;
    Option.iter ~f:(Typing_error_utils.add_typing_error ~env) err2;
    env

let check_cycles env (t_pos, t_name) t_assignment hints =
  let case_type_root =
    match t_assignment with
    | SimpleTypeDef _ -> None
    | CaseType _ -> Some t_name
  in
  let (env, cycles) =
    List.fold_left hints ~init:(env, []) ~f:(fun (env, cycles) hint ->
        let ((env, _ty_err_opt, new_cycles), _ty) =
          Phase.localize_typedef_structure
            env
            ~ignore_errors:true
            ~report_cycle:(t_pos, Type_expansions.Expandable.Type_alias t_name)
            case_type_root
            hint
        in
        (env, new_cycles @ cycles))
  in
  Type_expansions.report cycles
  |> Option.iter ~f:(fun e ->
         let e =
           match t_assignment with
           | SimpleTypeDef _ -> e
           | CaseType _ ->
             Typing_error.with_code ~code:Error_codes.Typing.RecursiveCaseType e
         in
         Typing_error_utils.add_typing_error ~env e);
  env

let check_where_clauses_with_recursive_mentions env t_name where_constraints =
  let (pos, name) = t_name in
  let report (_wc, decl_tyl) =
    if not @@ List.is_empty decl_tyl then
      let mentions =
        List.map decl_tyl ~f:(fun decl_ty ->
            Reason.to_pos @@ Typing_defs_core.get_reason decl_ty)
      in
      Typing_error_utils.add_typing_error
        ~env
        (Typing_error.casetype
           (Typing_error.Primary.CaseType.Recursive_where_clause
              { pos; name; mentions }))
  in
  let pairs =
    Typing_case_type_utils.find_where_clause_recursive_mentions
      env
      t_name
      where_constraints
  in
  List.iter ~f:report pairs

let rec strip_supportdyn_hint ((_, hint_) as hint) =
  match hint_ with
  | Happly ((_, name), [inner]) when String.equal name SN.Classes.cSupportDyn ->
    strip_supportdyn_hint inner
  | _ -> hint

(** Report non-disjoint shape splats on a typedef carrying the
    [<<__DisjointShapeSplat>>] attribute. The decision procedure is
    [Typing_shape_disjointness]. *)
let rec check_shape_splat_disjointness
    (env : Typing_env_types.env) (t_pos : Pos.t) hint =
  let open Typing_defs in
  let hint = strip_supportdyn_hint hint in
  match hint with
  | (_, (Hunion hints | Hintersection hints)) ->
    List.iter hints ~f:(check_shape_splat_disjointness env t_pos)
  | (_, (Hoption inner | Hlike inner)) ->
    check_shape_splat_disjointness env t_pos inner
  | (_, Hshape { nsi_field_map; _ })
    when List.exists nsi_field_map ~f:(function
             | SE_splat _ -> true
             | SE_field _ -> false) ->
    let decl_ty = Decl_hint.hint env.Typing_env_types.decl_env hint in
    begin
      match get_node decl_ty with
      | Tshape (Shape_splat { ss_elems }) ->
        let ety_env = { empty_expand_env with under_type_constructor = true } in
        let (env, rev_elems) =
          List.fold_left ss_elems ~init:(env, []) ~f:(fun (env, acc) elem ->
              let ((env, _err), elem) = Phase.localize ~ety_env env elem in
              (env, elem :: acc))
        in
        let violations =
          Typing_shape_disjointness.violations (List.rev rev_elems) env
        in
        if not (List.is_empty violations) then
          Typing_error_utils.add_typing_error
            ~env
            Typing_error.(
              primary
              @@ Primary.Shape_splat
                   (Primary.Shape_splat.Disjoint_shape_splat_violation
                      { pos = t_pos; violations }))
      | _ -> ()
    end;
    List.iter nsi_field_map ~f:(function
        | SE_splat inner -> check_shape_splat_disjointness env t_pos inner
        | SE_field _ -> ())
  | _ -> ()

let typedef_def ctx typedef =
  let env = EnvFromDef.typedef_env ~origin:Decl_counters.TopLevel ctx typedef in
  let {
    t_annotation = ();
    t_name;
    t_tparams;
    t_as_constraint;
    t_super_constraint;
    t_assignment;
    t_runtime_type;
    t_user_attributes;
    t_mode;
    t_namespace;
    t_span;
    t_emit_id;
    t_is_ctx;
    t_file_attributes;
    t_internal;
    t_module;
    t_docs_url;
    t_doc_comment;
    t_package;
  } =
    typedef
  in
  let env = Env.set_current_module env t_module in
  let env = Env.set_internal env t_internal in
  let env = Env.set_current_package_membership env t_package in
  let (env, restore_pos) =
    let (pos, _) = t_name in
    Env.set_inference_env_pos env (Some pos)
  in
  let (env, file_attributes) = Typing.file_attributes env t_file_attributes in
  let (do_report_cycles, hint_constraints_pairs) =
    match t_assignment with
    | SimpleTypeDef { tvh_vis = _; tvh_hint } ->
      ((fun _env -> true), [(tvh_hint, None)])
    | CaseType (variant, variants) ->
      ( (fun env ->
          let recursive_case_types env =
            (Env.get_tcopt env).Global_options.recursive_case_types
          in
          not (recursive_case_types env)),
        List.map (variant :: variants) ~f:(fun v ->
            (v.tctv_hint, Some v.tctv_where_constraints)) )
  in
  let (env, ty_err_opt1) =
    Phase.localize_and_add_ast_generic_parameters_and_where_constraints
      env
      ~ignore_errors:false
      t_tparams
      []
  in
  Option.iter ~f:(Typing_error_utils.add_typing_error ~env) ty_err_opt1;
  Typing_type_wellformedness.typedef env typedef
  |> List.iter ~f:(Typing_error_utils.add_typing_error ~env);
  Env.make_depend_on_current_module env;
  Typing_variance.typedef env typedef;

  let env =
    if do_report_cycles env then
      check_cycles
        env
        t_name
        t_assignment
        (List.map ~f:fst hint_constraints_pairs)
    else
      env
  in
  List.filter_map hint_constraints_pairs ~f:snd
  |> List.iter ~f:(check_where_clauses_with_recursive_mentions env t_name);
  let check_variant env (hint, constraints_opt) =
    let ((env, localize_ty_err_opt), ty) =
      Phase.localize_hint_no_subst env ~ignore_errors:false hint
    in
    Option.iter
      ~f:(Typing_error_utils.add_typing_error ~env)
      localize_ty_err_opt;
    Typing_type_wellformedness.shape_splat_field_denotability_errors
      env
      (fst t_name)
      hint
      ty
    |> List.iter ~f:(Typing_error_utils.add_typing_error ~env);
    let env_for_variant =
      match constraints_opt with
      | None -> env
      | Some constraints ->
        let (env, _err) =
          Phase.localize_and_add_where_constraints
            env
            ~ignore_errors:true
            (Typing_case_type_utils.filter_where_clauses_with_recursive_mentions
               env
               t_name
               constraints)
        in
        env
    in
    if
      Naming_attributes.mem
        SN.UserAttributes.uaDisjointShapeSplat
        t_user_attributes
    then
      check_shape_splat_disjointness env_for_variant (fst t_name) hint;
    let (env_for_variant, t_as_constraint_err_opt) =
      Constraints.check env_for_variant t_as_constraint As t_name ty
    in
    Option.iter
      ~f:(Typing_error_utils.add_typing_error ~env:env_for_variant)
      t_as_constraint_err_opt;
    let (_env_for_variant, t_super_constraint_err_opt) =
      Constraints.check env_for_variant t_super_constraint Super t_name ty
    in
    Option.iter
      ~f:(Typing_error_utils.add_typing_error ~env:env_for_variant)
      t_super_constraint_err_opt;
    (* we return env instead of env_for_variant because env_for_variant contains
       assumptions from the where clause which we do not want to assume for
       other variants *)
    env
  in
  let env = List.fold_left hint_constraints_pairs ~init:env ~f:check_variant in
  let env = casetype_def env typedef in
  let (env, user_attributes) =
    Typing.attributes_check_def
      env
      SN.AttributeKinds.typealias
      t_user_attributes
  in
  let (env, tparams) = List.map_env env t_tparams ~f:Typing.type_param in
  let env = restore_pos env in
  {
    Aast.t_annotation = Env.save (Env.get_tpenv env) env;
    t_name;
    t_mode;
    t_user_attributes = user_attributes;
    t_as_constraint;
    t_super_constraint;
    t_assignment;
    t_runtime_type;
    t_tparams = tparams;
    t_namespace;
    t_span;
    t_emit_id;
    t_is_ctx;
    t_file_attributes = file_attributes;
    t_internal;
    t_module;
    t_docs_url;
    t_doc_comment;
    t_package;
  }
