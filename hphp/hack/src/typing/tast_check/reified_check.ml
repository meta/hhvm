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
module Env = Tast_env
module SN = Naming_special_names
module UA = SN.UserAttributes
module Cls = Folded_class
module Nast = Aast

let is_reified tparam = not (equal_reify_kind tparam.tp_reified Erased)

let tparams_has_reified tparams = List.exists tparams ~f:is_reified

let valid_newable_hint env (tp_pos, tp_name) (pos, hint) =
  let err_opt =
    let open Typing_error.Primary in
    match hint with
    | Aast.Happly ((p, h), _) -> begin
      match Env.get_class_or_typedef env h with
      | Decl_entry.Found (Env.ClassResult cls) ->
        if not Ast_defs.(is_c_normal (Cls.kind cls)) then
          Some (Invalid_newable_type_argument { tp_pos; tp_name; pos = p })
        else
          None
      | _ ->
        (* This case should never happen *)
        Some (Invalid_newable_type_argument { tp_pos; tp_name; pos = p })
    end
    | Aast.Habstr name ->
      if not @@ Env.get_newable env name then
        Some (Invalid_newable_type_argument { tp_pos; tp_name; pos })
      else
        None
    | _ -> Some (Invalid_newable_type_argument { tp_pos; tp_name; pos })
  in
  let Equal = Tast_env.eq_typing_env in
  Option.iter err_opt ~f:(fun err ->
      Typing_error_utils.add_typing_error ~env @@ Typing_error.primary err)

let verify_has_consistent_bound env (tparam : Tast.tparam) =
  let upper_bounds =
    (* a newable type parameter cannot be higher-kinded/require arguments *)
    Typing_set.elements (Env.get_upper_bounds env (snd tparam.tp_name))
  in
  let bound_classes =
    List.filter_map upper_bounds ~f:(fun ty ->
        match get_node ty with
        | Tclass ((_, class_id), _, _) -> begin
          match Env.get_class env class_id with
          | Decl_entry.Found cls -> Some (Some cls)
          | Decl_entry.NotYetAvailable -> Some None
          | Decl_entry.DoesNotExist -> None
        end
        | _ -> None)
  in
  let valid_classes =
    List.fold_right bound_classes ~init:(Some []) ~f:(fun cls acc ->
        let open Option.Let_syntax in
        let* acc = acc in
        let* cls = cls in
        if Env.valid_newable_class env cls then
          Some (cls :: acc)
        else
          Some acc)
  in
  match valid_classes with
  | None ->
    (* A class that we queried might or might not exist. Don't emit an error. *)
    ()
  | Some valid_classes ->
    if Int.( <> ) 1 (List.length valid_classes) then
      let constraints = List.map ~f:Cls.name valid_classes in
      let (pos, tp_name) = tparam.tp_name in
      let Equal = Tast_env.eq_typing_env in
      Typing_error_utils.add_typing_error
        ~env
        Typing_error.(
          primary
          @@ Primary.Invalid_newable_typaram_constraints
               { pos; tp_name; constraints })

(* When passing targs to a reified position, they must either be concrete types
 * or reified type parameters. This prevents the case of
 *
 * class C<reify Tc> {}
 * function f<Tf>(): C<Tf> {}
 *
 * where Tf does not exist at runtime.
 *)
let verify_targ_valid (env : Tast_env.env) reification tparam targ =
  let Equal = Tast_env.eq_typing_env in
  (* There is some subtlety here. If a type *parameter* is declared reified,
   * even if it is soft, we require that the argument be concrete or reified, not soft
   * reified or erased *)
  (if is_reified tparam then
    match tparam.tp_reified with
    | Nast.Reified
    | Nast.SoftReified ->
      let (decl_pos, param_name) = tparam.tp_name in
      let emit_error pos arg_info =
        let err =
          Typing_error.(
            primary
            @@ Primary.Invalid_reified_arg
                 { pos; param_name; decl_pos; arg_info })
        in
        Typing_error_utils.add_typing_error ~env err
      in

      Typing_reified_check.validator#validate_hint
        env
        (snd targ)
        ~reification
        emit_error
    | Nast.Erased -> ());

  if Attributes.mem UA.uaEnforceable tparam.tp_user_attributes then
    Typing_enforceable_hint.validate_hint env (snd targ) (fun pos ty_info ->
        let (tp_pos, tp_name) = tparam.tp_name in
        Typing_error_utils.add_typing_error
          ~env
          Typing_error.(
            primary
            @@ Primary.Invalid_enforceable_type
                 { kind = `param; pos; ty_info; tp_pos; tp_name }));

  if Attributes.mem UA.uaNewable tparam.tp_user_attributes then
    valid_newable_hint env tparam.tp_name (snd targ)

let verify_call_targs env expr_pos decl_pos tparams targs reification =
  let Equal = Tast_env.eq_typing_env in
  (if tparams_has_reified tparams then
    let tparams_length = List.length tparams in
    let targs_length = List.length targs in
    if Int.( <> ) tparams_length targs_length then
      if Int.( = ) targs_length 0 then
        let decl_pos =
          Option.value
            ~default:decl_pos
            (List.find_map tparams ~f:(fun tparam ->
                 if is_reified tparam then
                   let Typing_defs_core.{ tp_name = (pos, _); _ } = tparam in
                   Some pos
                 else
                   None))
        in
        Typing_error_utils.add_typing_error
          ~env
          Typing_error.(
            primary @@ Primary.Require_args_reify { decl_pos; pos = expr_pos })
      else
        (* mismatches with targs_length > 0 are not specific to reification and handled
                  elsewhere *)
        ());
  let all_wildcards =
    List.for_all ~f:(fun (_, h) -> Aast_defs.is_wildcard_hint h) targs
  in
  if all_wildcards && tparams_has_reified tparams then
    let decl_pos =
      Option.value
        ~default:decl_pos
        (List.find_map tparams ~f:(fun tparam ->
             if is_reified tparam then
               let Typing_defs_core.{ tp_name = (pos, _); _ } = tparam in
               Some pos
             else
               None))
    in
    Typing_error_utils.add_typing_error
      ~env
      Typing_error.(
        primary @@ Primary.Require_args_reify { decl_pos; pos = expr_pos })
  else
    (* Unequal_lengths case handled elsewhere *)
    List.iter2 tparams targs ~f:(verify_targ_valid env reification) |> ignore

let get_ft_tparams env fun_ty =
  let fun_ty = Tast_env.strip_dynamic env fun_ty in
  match Tast_env.get_underlying_function_type env fun_ty with
  | None -> None
  | Some (_, fun_ty) ->
    Some (fun_ty.ft_tparams, get_ft_is_function_pointer fun_ty)

let handler =
  object
    inherit Tast_visitor.handler_base

    method! at_expr env x =
      let Equal = Tast_env.eq_typing_env in
      (* only considering functions where one or more params are reified *)
      match x with
      | (_, call_pos, Class_get ((_, _, CIreified (_, _t)), _, _)) ->
        Typing_error_utils.add_typing_error
          ~env
          Typing_error.(primary @@ Primary.Class_get_reified call_pos)
      | (fun_ty, pos, Method_caller _) ->
        (match get_ft_tparams env fun_ty with
        | Some (ft_tparams, _) ->
          if tparams_has_reified ft_tparams then
            Typing_error_utils.add_typing_error
              ~env
              Typing_error.(primary @@ Primary.Reified_function_reference pos)
        | None -> ())
      | ( _,
          pos,
          FunctionPointer
            (FP_class_const ((_, _, CIreified (_, _class_id)), _), _, _) )
        when Env.is_in_expr_tree env ->
        Typing_error_utils.add_typing_error
          ~env
          Typing_error.(
            expr_tree
            @@ Primary.Expr_tree.Reified_static_method_in_expr_tree pos)
      | (fun_ty, pos, FunctionPointer (_, targs, _)) -> begin
        match get_ft_tparams env fun_ty with
        | Some (ft_tparams, _) ->
          verify_call_targs
            env
            pos
            (get_pos fun_ty)
            ft_tparams
            targs
            Type_validator.Resolved
        | None -> ()
      end
      | (_, pos, Call { func = (fun_ty, _, expr); targs; _ }) ->
        let reification =
          (* If need be, we can extend this special casing to all
           * HH\ReifiedGenerics functions. *)
          match expr with
          | Id (_, id) when String.(id = SN.StdlibFunctions.get_class_from_type)
            ->
            Type_validator.TypeStructure
          | _ -> Type_validator.Resolved
        in
        let (env, efun_ty) = Env.expand_type env fun_ty in
        (match get_ft_tparams env efun_ty with
        | Some (ft_tparams, is_function_pointer) when not @@ is_function_pointer
          ->
          verify_call_targs
            env
            pos
            (get_pos efun_ty)
            ft_tparams
            targs
            reification
        | _ -> ())
      | (_, pos, New ((_, _, CI (_, class_id)), targs, _, _, _)) ->
        (match Env.get_class env class_id with
        | Decl_entry.Found cls ->
          let tparams = Cls.tparams cls in
          let class_pos = Cls.pos cls in
          verify_call_targs
            env
            pos
            class_pos
            tparams
            targs
            Type_validator.Resolved
        | Decl_entry.DoesNotExist
        | Decl_entry.NotYetAvailable ->
          ())
      | (_, pos, New ((_, _, CIreified (_, name)), _, _, _, _)) ->
        if not (Env.get_newable env name) then
          Typing_error_utils.add_typing_error
            ~env
            Typing_error.(primary @@ Primary.New_without_newable { pos; name })
      | ( _,
          pos,
          New ((_, _, ((CIstatic | CIself | CIparent) as cid)), _, _, _, _) ) ->
        Option.(
          let t =
            Env.get_self_id env
            >>| Env.get_class env
            >>= Decl_entry.to_option
            >>| Cls.tparams
            >>| tparams_has_reified
          in
          Option.iter t ~f:(fun has_reified ->
              if has_reified then
                let (class_kind, suggested_class_name) =
                  match cid with
                  | CIstatic -> ("static", None)
                  | CIself -> ("self", Env.get_self_id env)
                  | CIparent -> ("parent", Env.get_parent_id env)
                  | _ -> failwith "Unexpected match"
                in
                Typing_error_utils.add_typing_error
                  ~env
                  Typing_error.(
                    primary
                    @@ Primary.New_class_reified
                         { pos; class_kind; suggested_class_name })))
      | (_, pos, New ((ty, _, _), targs, _, _, _)) ->
        let (env, ty) = Env.expand_type env ty in
        begin
          match get_node ty with
          | Tclass ((_, cid), _, _) -> begin
            match Env.get_class env cid with
            | Decl_entry.Found cls ->
              let tparams = Cls.tparams cls in
              let class_pos = Cls.pos cls in
              verify_call_targs
                env
                pos
                class_pos
                tparams
                targs
                Type_validator.Resolved
            | Decl_entry.DoesNotExist
            | Decl_entry.NotYetAvailable ->
              ()
          end
          | _ -> ()
        end
      | _ -> ()

    method! at_hint env =
      function
      | (_pos, Aast.Happly ((_, class_id), hints)) ->
        let tc = Env.get_class_or_typedef env class_id in
        begin
          match tc with
          | Decl_entry.Found (Env.ClassResult tc) ->
            let tparams = Cls.tparams tc in
            ignore
              (List.iter2 tparams hints ~f:(fun tp hint ->
                   verify_targ_valid env Type_validator.Unresolved tp ((), hint)))
          | _ -> ()
        end
      | _ -> ()

    method! at_tparam env tparam =
      (* Can't use Attributes.mem here because of a conflict between Nast.user_attributes and Tast.user_attributes *)
      if
        List.exists tparam.tp_user_attributes ~f:(fun { ua_name; _ } ->
            String.equal UA.uaNewable (snd ua_name))
      then
        verify_has_consistent_bound env tparam

    method! at_class_ env { c_name = (pos, name); _ } =
      let Equal = Tast_env.eq_typing_env in
      match Env.get_class env name with
      | Decl_entry.Found cls -> begin
        match Tast_env.get_construct env cls with
        | (_, Typing_defs.ConsistentConstruct) ->
          if
            List.exists
              ~f:(fun t -> not (equal_reify_kind t.tp_reified Erased))
              (Cls.tparams cls)
          then
            Typing_error_utils.add_typing_error
              ~env
              Typing_error.(primary @@ Primary.Consistent_construct_reified pos)
        | _ -> ()
      end
      | Decl_entry.DoesNotExist
      | Decl_entry.NotYetAvailable ->
        ()
  end
