(*
 * Copyright (c) 2017, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)
open Hh_prelude
open Aast
open Typing_defs
open Typing_env_types
module Env = Typing_env
module SN = Naming_special_names
module MakeType = Typing_make_type
module TCO = Typechecker_options

let check_param : env -> Nast.fun_param -> unit =
 fun env { param_type_hint; param_pos; param_name; _ } ->
  let error ty =
    let ty_name = lazy (Typing_print.error env ty) in
    let msgl =
      Lazy.map ty_name ~f:(fun ty_name ->
          Reason.to_string ("This is " ^ ty_name) (get_reason ty))
    in
    Typing_error.Primary.Invalid_memoized_param
      { pos = param_pos; reason = msgl; ty_name }
  in
  let check_memoizable env pos ty =
    let rec check_memoizable : env -> locl_ty -> unit =
     fun env ty ->
      let (env, ty) = Env.expand_type env ty in
      let ety_env = empty_expand_env in
      let (env, ty) = Typing_dynamic_utils.strip_dynamic env ty in
      let ((env, ty_err_opt), ty, _) =
        Typing_tdef_locl.force_expand_typedef ~ety_env env ty
      in
      let (_, env, ty) = Typing_utils.strip_supportdyn env ty in
      Option.iter ~f:(Typing_error_utils.add_typing_error ~env) ty_err_opt;
      if not (Typing_utils.is_tyvar_error env ty) then
        match get_node ty with
        | Tprim (Tnull | Tarraykey | Tbool | Tint | Tfloat | Tnum) -> ()
        | Tnonnull
        | Tany _
        | Tdynamic _
        | Tneg _ ->
          let (_ : env) = Typing_local_ops.enforce_memoize_object pos env in
          ()
        | Tprim (Tvoid | Tresource | Tnoreturn) ->
          Typing_error_utils.add_typing_error ~env
          @@ Typing_error.primary
          @@ error ty
        | Toption ty -> check_memoizable env ty
        | Ttuple { t_required; t_optional; t_extra = Tvariadic t_variadic } ->
          List.iter t_required ~f:(check_memoizable env);
          List.iter t_optional ~f:(check_memoizable env);
          check_memoizable env t_variadic
        (* Just accept all generic types for now. Stricter check_memoizables to come later. *)
        | Tgeneric _ ->
          (* FIXME fun fact:
             the comment above about "stricter check_memoizables to come later" was added in revision
             in August 2015 *)
          ()
        | Tlabel _ -> ()
        | Tnewtype (id, _, _) when String.equal SN.Classes.cEnumClassLabel id ->
          (* EnumClassLabels are memoizable *)
          ()
        (* For parameter type 'this::TID' defined by 'type const TID as Bar' check_memoizables
         * Bar recursively. Also enums represented using AKnewtype.
         *)
        | Tnewtype (_, _, ty)
        | Tdependent (_, ty) ->
          check_memoizable env ty
        (* Handling Tunion and Tintersection case here for completeness, even though it
         * shouldn't be possible to have an unresolved type when check_memoizableing
         * the method declaration. No corresponding test case for this.
         *)
        | Tunion tyl
        | Tintersection tyl ->
          List.iter tyl ~f:(check_memoizable env)
        | Tvec_or_dict (_, ty) -> check_memoizable env ty
        | Tshape (Shape_simple { s_fields = fdm; _ }) ->
          (* TODO(shapes) should unknown type affect memoizability? *)
          TShapeMap.iter
            begin
              (fun _ { sft_ty; _ } -> check_memoizable env sft_ty)
            end
            fdm
        | Tshape (Shape_splat { ss_elems }) ->
          List.iter ss_elems ~f:(check_memoizable env)
        | Tclass ((_, id), _, _)
          when String.equal Naming_special_names.Classes.cString id ->
          ()
        | Tclass _ ->
          let env = Env.open_tyvars env pos in
          let (env, type_param) = Env.fresh_type env pos in
          let container_type = MakeType.container Reason.none type_param in
          let (env, ty_err_opt) =
            Typing_utils.sub_type env ty container_type
            @@ Some (Typing_error.Reasons_callback.unify_error_at pos)
          in
          let is_container = Option.is_none ty_err_opt in

          let mixed = MakeType.mixed Reason.none in
          let hackarray = MakeType.any_array Reason.none mixed mixed in
          let is_hackarray = Typing_utils.is_sub_type env ty hackarray in
          let env =
            if not is_hackarray then
              (* Check if it's a UNSAFEsingleton memoize param *)
              let singleton =
                MakeType.class_type
                  Reason.none
                  SN.Classes.cUNSAFESingletonMemoizeParam
                  []
              in
              if Typing_utils.is_sub_type env ty singleton then
                env
              else
                Typing_local_ops.enforce_memoize_object pos env
            else
              env
          in
          let env = Env.set_tyvar_variance env container_type in
          let (env, e1) = Typing_solver.close_tyvars_and_solve env in
          Option.iter ~f:(Typing_error_utils.add_typing_error ~env) e1;
          if is_container then
            check_memoizable env type_param
          else
            let base_error = error ty in
            let ((env, ty_err_opt), (tfty, _tal)) =
              Typing_object_get.obj_get
                ~obj_pos:param_pos
                ~is_method:true
                ~nullsafe:None
                ~coerce_from_ty:None
                ~explicit_targs:[]
                ~class_id:
                  (CIexpr
                     ( (),
                       param_pos,
                       Lvar (param_pos, Local_id.make_unscoped param_name) ))
                ~member_id:(pos, SN.Members.mGetInstanceKey)
                ~on_error:Typing_error.Callback.(always base_error)
                env
                ty
            in
            Option.iter ty_err_opt ~f:(Typing_error_utils.add_typing_error ~env);
            ignore
              (Typing.call
                 ~expected:None
                 ~expr_pos:pos
                 ~recv_pos:pos
                 ~id_pos:pos
                 env
                 tfty
                 []
                 None)
        | Taccess _ -> ()
        | Tfun _
        | Ttuple { t_extra = Tsplat _; _ }
        | Tvar _ ->
          Typing_error_utils.add_typing_error ~env
          @@ Typing_error.primary
          @@ error ty
        | Tclass_ptr _ ->
          (* Follow classname<T> memo check via string bound *)
          ()
    in
    check_memoizable env ty
  in
  match hint_of_type_hint param_type_hint with
  | None -> ()
  | Some hint ->
    let (hint_pos, _) = hint in
    let ((env, ty_err_opt), ty) =
      Typing_phase.localize_hint_no_subst env ~ignore_errors:true hint
    in
    Option.iter ~f:(Typing_error_utils.add_typing_error ~env) ty_err_opt;
    check_memoizable env hint_pos ty

let check : env -> Nast.user_attribute list -> Nast.fun_param list -> unit =
 fun env user_attributes params ->
  if
    Naming_attributes.mem SN.UserAttributes.uaMemoize user_attributes
    || Naming_attributes.mem SN.UserAttributes.uaMemoizeLSB user_attributes
  then
    List.iter ~f:(check_param env) params

let mentions_generic name (ty : decl_ty) =
  let visitor =
    object
      inherit [bool] Type_visitor.decl_type_visitor as parent

      method! on_tgeneric found _ n = found || String.equal n name

      method! on_type found ty = found || parent#on_type found ty
    end
  in
  visitor#on_type false ty

(* Whether generic [name] occurs in the type arguments of an `IMemoizeParam`
 * class in [ty]. Such an argument is keyed only by its `getInstanceKey()`. *)
let rec under_instance_key env name ~fuel (ty : decl_ty) =
  let go = under_instance_key env name ~fuel in
  match get_node ty with
  | Tapply ((_, cname), targs) ->
    (match Env.get_class env cname with
    | Decl_entry.Found cls ->
      Folded_class.has_ancestor cls SN.Classes.cIMemoizeParam
      && List.exists targs ~f:(mentions_generic name)
      || List.exists targs ~f:go
    | Decl_entry.DoesNotExist
    | Decl_entry.NotYetAvailable ->
      (match Env.get_typedef env cname with
      | Decl_entry.Found
          { td_tparams; td_type_assignment = SimpleTypeDef (_, body); _ }
        when fuel > 0 ->
        let subst = Decl_instantiate.make_subst td_tparams targs in
        under_instance_key
          env
          name
          ~fuel:(fuel - 1)
          (Decl_instantiate.instantiate subst body)
      | _ -> List.exists targs ~f:go))
  | Toption ty
  | Tlike ty ->
    go ty
  | Tvec_or_dict (k, v) -> go k || go v
  | Tunion tyl
  | Tintersection tyl ->
    List.exists tyl ~f:go
  | Ttuple { t_required; t_optional; t_extra } ->
    List.exists (t_required @ t_optional) ~f:go
    ||
    (match t_extra with
    | Tvariadic ty
    | Tsplat ty ->
      go ty)
  | Tshape (Shape_simple { s_fields; _ }) ->
    TShapeMap.exists (fun _ { sft_ty; _ } -> go sft_ty) s_fields
  | _ -> false

(* The memoization cache ignores erased generics, so one cached result serves
 * every instantiation. *)
let check_erased_generics
    env
    (user_attributes : Nast.user_attribute list)
    tparams
    (params : Nast.fun_param list)
    ret =
  if TCO.ban_memoize_erased_generic_return (Env.get_tcopt env) then
    let attr_pos =
      Option.first_some
        (Naming_attributes.mem_pos SN.UserAttributes.uaMemoize user_attributes)
        (Naming_attributes.mem_pos
           SN.UserAttributes.uaMemoizeLSB
           user_attributes)
    in
    match (attr_pos, hint_of_type_hint ret) with
    | (Some attr_pos, Some hint) ->
      let ret_ty = Decl_hint.hint env.decl_env hint in
      let erased =
        List.filter tparams ~f:(fun (tp : Nast.tparam) ->
            match tp.tp_reified with
            | Reified -> false
            | Erased
            | SoftReified ->
              true)
      in
      let (positive, negative) =
        Typing_variance.get_positive_negative_generics
          ~tracked:
            (S_set.of_list
               (List.map erased ~f:(fun (tp : Nast.tparam) -> snd tp.tp_name)))
          ~is_mutable:false
          env
          (S_map.empty, S_map.empty)
          ret_ty
      in
      List.iter erased ~f:(fun (tp : Nast.tparam) ->
          let (pos, tparam_name) = tp.tp_name in
          let report via_param =
            Typing_error_utils.add_typing_error
              ~env
              Typing_error.(
                primary
                @@ Primary.Memoize_erased_generic_return
                     { pos; tparam_name; attr_pos; via_param })
          in
          if S_map.mem tparam_name negative then
            report None
          else if S_map.mem tparam_name positive then
            List.find_map params ~f:(fun param ->
                Option.bind
                  (hint_of_type_hint param.param_type_hint)
                  ~f:(fun hint ->
                    if
                      under_instance_key
                        env
                        tparam_name
                        ~fuel:10
                        (Decl_hint.hint env.decl_env hint)
                    then
                      Some param.param_pos
                    else
                      None))
            |> Option.iter ~f:(fun p -> report (Some p)))
    | _ -> ()

let check_function : env -> Nast.fun_ -> unit =
 fun env { f_user_attributes; f_params; _ } ->
  check env f_user_attributes f_params

let check_method : env -> Nast.method_ -> unit =
 fun env { m_user_attributes; m_params; _ } ->
  check env m_user_attributes m_params
