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
open Tast
open Typing_defs
open Utils
module TUtils = Typing_utils
module Reason = Typing_reason
module Env = Typing_env
module Union = Typing_union
module Inter = Typing_intersection
module SN = Naming_special_names
module TVis = Typing_visibility
module Phase = Typing_phase
module MakeType = Typing_make_type
module Cls = Folded_class

type ty_mismatch = (locl_ty, locl_ty * locl_ty) result

type internal_result =
  Typing_env_types.env
  * Typing_error.t option
  * (locl_ty * targ list)
  * ty_mismatch
  * ty_mismatch option

let merge_ty_err
    ty_err_opt1 (env, ty_err_opt2, tals, lval_mismatch, rval_mismatch_opt) =
  let ty_err_opt = Option.merge ty_err_opt1 ty_err_opt2 ~f:Typing_error.both in
  (env, ty_err_opt, tals, lval_mismatch, rval_mismatch_opt)

(** Common arguments to internal `obj_get_...` functions *)
type obj_get_args = {
  is_method: bool;
  is_nonnull: bool;
  nullsafe: Pos.t option;
  obj_pos: pos;
  coerce_from_ty:
    (MakeType.Nast.pos * Reason.ureason * Typing_defs.locl_ty) option;
  explicit_targs: Nast.targ list;
  this_ty: locl_ty;
  this_ty_conjunct: locl_ty;
  is_parent_call: bool;
  dep_kind: Reason.t * Typing_dependent_type.ExprDepTy.dep;
  seen: S_set.t;
}

let log_obj_get env helper id ty this_ty =
  let (fn_name, ty_name) =
    match helper with
    | `concrete -> ("obj_get_concrete_ty", "concrete_ty")
    | `inner -> ("obj_get_inner", "receiver_ty")
  in
  Typing_log.(
    log_with_level env "obj_get" ~level:2 (fun () ->
        log_types
          (Pos_or_decl.of_raw_pos (fst id))
          env
          [
            Log_head
              ( fn_name ^ " (member_id: " ^ snd id ^ ")",
                [Log_type (ty_name, ty); Log_type ("this_ty", this_ty)] );
          ]))

let mk_ety_env class_info paraml this_ty =
  {
    empty_expand_env with
    this_ty;
    substs = TUtils.make_locl_subst_for_class_tparams class_info paraml;
  }

let mk_mismatch_intersection env errs_res =
  Result.fold
    errs_res
    ~ok:(fun tys ->
      let (env, ty) = Inter.intersect_list env Reason.none tys in
      (env, Ok ty))
    ~error:(fun (actuals, expecteds) ->
      let (env, ty_actual) = Inter.intersect_list env Reason.none actuals in
      let (env, ty_expect) = Inter.intersect_list env Reason.none expecteds in
      (env, Error (ty_actual, ty_expect)))

let mk_mismatch_union env =
  Result.fold
    ~ok:(fun tys ->
      let (env, ty) = Union.union_list env Reason.none tys in
      (env, Ok ty))
    ~error:(fun (actuals, expecteds) ->
      let (env, ty_acutal) = Union.union_list env Reason.none actuals in
      let (env, ty_expect) = Union.union_list env Reason.none expecteds in
      (env, Error (ty_acutal, ty_expect)))

let fold_mismatches mismatches =
  List.fold_left mismatches ~init:(Ok []) ~f:(fun acc err ->
      match (acc, err) with
      | (Ok xs, Ok x) -> Ok (x :: xs)
      | (Ok xs, Error (x, y)) -> Error (x :: xs, y :: xs)
      | (Error (xs, ys), Ok x) -> Error (x :: xs, x :: ys)
      | (Error (xs, ys), Error (x, y)) -> Error (x :: xs, y :: ys))

let fold_mismatch_opts opt_errs =
  Option.(map ~f:fold_mismatches @@ all opt_errs)

let get_member ~is_method env class_info member_name =
  if is_method then
    if String.equal member_name SN.Members.__construct then
      fst (Env.get_construct env class_info)
    else
      Env.get_method env class_info member_name
  else
    Env.get_prop env class_info member_name

let smember_not_found
    pos ~is_const ~is_method ~is_function_pointer class_ member_name on_error =
  let kind =
    if is_const then
      `class_constant
    else if is_method then
      `static_method
    else
      `class_variable
  in
  let error hint =
    let (class_pos, class_name) = (Cls.pos class_, Cls.name class_) in
    let quickfixes =
      Option.value_map hint ~default:[] ~f:(fun (_, _, new_text) ->
          [
            Quickfix.make_eager_default_hint_style
              ~title:("Change to ::" ^ new_text)
              ~new_text
              pos;
          ])
    in
    Typing_error.(
      apply ~on_error
      @@ primary
      @@ Primary.Smember_not_found
           { pos; kind; class_name; class_pos; member_name; hint; quickfixes })
  in
  let static_suggestion =
    Env.suggest_static_member is_method class_ member_name
  in
  let method_suggestion = Env.suggest_member is_method class_ member_name in
  match (static_suggestion, method_suggestion) with
  (* If there is a normal method of the same name and the
   * syntax is a function pointer, suggest meth_caller *)
  | (_, Some (_, v)) when is_function_pointer && String.equal v member_name ->
    Typing_error.(
      primary
      @@ Primary.Consider_meth_caller
           { pos; class_name = Cls.name class_; meth_name = member_name })
  (* If there is a normal method of the same name, suggest it *)
  | (Some _, Some (def_pos, v)) when String.equal v member_name ->
    error (Some (`instance, def_pos, v))
  (* Otherwise suggest a different static method *)
  | (Some (def_pos, v), _) -> error (Some (`static, def_pos, v))
  (* Fallback to closest normal method *)
  | (None, Some (def_pos, v)) -> error (Some (`instance, def_pos, v))
  | (None, None) -> error None

let member_not_found
    (env : Typing_env_types.env) pos ~is_method class_ member_name r on_error =
  let cls_name = strip_ns (Cls.name class_) in
  if
    Env.is_in_expr_tree env
    && is_method
    && String.is_prefix member_name ~prefix:"__"
  then
    Typing_error.(
      expr_tree
      @@ Primary.Expr_tree.Expression_tree_unsupported_operator
           { class_name = cls_name; member_name; pos })
  else
    let (class_pos, class_name) = (Cls.pos class_, Cls.name class_) in
    let reason =
      lazy
        (Reason.to_string
           ("This is why I think it is an object of type " ^ cls_name)
           r)
    in
    let hint =
      lazy
        (let method_suggestion =
           Env.suggest_member is_method class_ member_name
         in
         let static_suggestion =
           Env.suggest_static_member is_method class_ member_name
         in
         match (method_suggestion, static_suggestion) with
         (* Prefer suggesting a different method, unless there's a
            static method whose name matches exactly. *)
         | (Some _, Some (def_pos, v)) when String.equal v member_name ->
           Some (`static, def_pos, v)
         | (Some (def_pos, v), _) -> Some (`instance, def_pos, v)
         | (None, Some (def_pos, v)) -> Some (`static, def_pos, v)
         | (None, None) -> None)
    in
    Typing_error.(
      apply ~on_error
      @@ primary
      @@ Primary.Member_not_found
           {
             pos;
             kind =
               (if is_method then
                 `method_
               else
                 `property);
             class_name;
             class_pos;
             member_name;
             hint;
             reason;
           })

let sound_dynamic_err_opt args env ((_, id_str) as id) read_context =
  (* Any access to a *private* member through dynamic might potentially
   * be unsound, if the receiver is an instance of a class that implements dynamic,
   * as we do no checks on enforceability or subtype-dynamic at the definition site
   * of private members.
   *)
  match Env.get_self_class env with
  | Decl_entry.Found self_class
    when Cls.get_support_dynamic_type self_class || not (Cls.final self_class)
    ->
    (match get_member ~is_method:args.is_method env self_class id_str with
    | Some { ce_visibility = Vprivate _; ce_type = (lazy ty); _ }
      when not args.is_method ->
      if read_context then
        let ((env, lclz_ty_err_opt), locl_ty) =
          Phase.localize_no_subst ~ignore_errors:true env ty
        in
        Option.merge ~f:Typing_error.both lclz_ty_err_opt
        @@ Typing_dynamic.check_property_sound_for_dynamic_read
             ~on_error:(fun pos prop_name class_name (prop_pos, prop_type) ->
               Typing_error.(
                 primary
                 @@ Primary.Private_property_is_not_dynamic
                      { pos; prop_name; class_name; prop_pos; prop_type }))
             env
             (Cls.name self_class)
             id
             locl_ty
      else
        Typing_dynamic.check_property_sound_for_dynamic_write
          ~this_class:(Some self_class)
          ~on_error:(fun pos prop_name class_name (prop_pos, prop_type) ->
            Typing_error.(
              primary
              @@ Primary.Private_property_is_not_enforceable
                   { pos; prop_name; class_name; prop_pos; prop_type }))
          env
          (Cls.name self_class)
          id
          ty
          None
    | _ -> None)
  | _ -> None

let widen_class_for_obj_get ~is_method ~nullsafe member_name env ty =
  match deref ty with
  | (_, Tprim Tnull) ->
    if Option.is_some nullsafe then
      (env, Some ty)
    else
      (env, None)
  | (r2, Tclass (((_, class_name) as class_id), _, tyl)) ->
    let default () =
      let ty = mk (r2, Tclass (class_id, nonexact, tyl)) in
      (env, Some ty)
    in
    begin
      match Env.get_class env class_name with
      | Decl_entry.DoesNotExist
      | Decl_entry.NotYetAvailable ->
        default ()
      | Decl_entry.Found class_info ->
        (match get_member ~is_method env class_info member_name with
        | Some { ce_origin; _ } ->
          (* If this member was inherited then we obtain the type from which
           * it is inherited as our wider type *)
          if String.equal ce_origin class_name then
            default ()
          else (
            match Cls.get_ancestor class_info ce_origin with
            | None -> default ()
            | Some basety ->
              let ety_env =
                {
                  empty_expand_env with
                  substs =
                    TUtils.make_locl_subst_for_class_tparams class_info tyl;
                  this_ty = ty;
                }
              in
              let ((env, _), basety) = Phase.localize ~ety_env env basety in
              (env, Some basety)
          )
        | None -> (env, None))
    end
  | _ -> (env, None)

(* `ty` is expected to be the type for a property or method that has been
 * accessed using the nullsafe operatore e.g. $x?->prop or $x?->foo(...).
 *
 * For properties, just make the type nullable.
 * For methods, we expect a function type, and make the return type nullable.
 * But in the case that we have type nothing, we just use the type `null`.
 * The `call` helper will deal appropriately with it.
 * Similarly if we have dynamic, or err, or any, we leave as dynamic/err/any respectively.
 *)
let rec make_nullable_member_type env ~is_method id_pos pos ty =
  if is_method then
    Typing_utils.map_supportdyn env ty (fun env ty ->
        let (env, ty) = Env.expand_type env ty in
        match deref ty with
        | (r, Tfun tf) ->
          let (env, ty) =
            make_nullable_member_type ~is_method:false env id_pos pos tf.ft_ret
          in
          (env, mk (r, Tfun { tf with ft_ret = ty }))
        | (r, Tunion (_ :: _ as tyl)) ->
          let (env, tyl) =
            List.map_env env tyl ~f:(fun env ty ->
                make_nullable_member_type ~is_method env id_pos pos ty)
          in
          Union.union_list env r tyl
        | (r, Tintersection tyl) ->
          let (env, tyl) =
            List.map_env env tyl ~f:(fun env ty ->
                make_nullable_member_type ~is_method env id_pos pos ty)
          in
          Inter.intersect_list env r tyl
        | (_, (Tdynamic _ | Tany _)) -> (env, ty)
        | (_, Tunion []) -> (env, MakeType.null (Reason.nullsafe_op pos))
        | _ ->
          (* Shouldn't happen *)
          make_nullable_member_type ~is_method:false env id_pos pos ty)
  else
    let (env, ty) =
      Typing_intersection.intersect_with_nonnull
        env
        (Pos_or_decl.of_raw_pos id_pos)
        ty
    in
    (env, MakeType.nullable (Reason.nullsafe_op pos) ty)

(* Return true if the `this` type appears in a covariant
 * (resp. contravariant, if contra=true) position in ty.
 *)
let rec this_appears_covariantly ~contra env ty =
  let rec this_appears_covariantly_params tparams tyl =
    match (tparams, tyl) with
    | (tp :: tparams, ty :: tyl) ->
      begin
        match tp.tp_variance with
        | Ast_defs.Covariant -> this_appears_covariantly ~contra env ty
        | Ast_defs.Contravariant ->
          this_appears_covariantly ~contra:(not contra) env ty
        | Ast_defs.Invariant ->
          this_appears_covariantly ~contra env ty
          || this_appears_covariantly ~contra:(not contra) env ty
      end
      || this_appears_covariantly_params tparams tyl
    | _ -> false
  in
  match get_node ty with
  | Tthis -> not contra
  | Ttuple { t_required; t_optional; t_extra = Tvariadic t_vs | Tsplat t_vs } ->
    List.exists t_required ~f:(this_appears_covariantly ~contra env)
    || List.exists t_optional ~f:(this_appears_covariantly ~contra env)
    || this_appears_covariantly ~contra env t_vs
  | Tunion tyl
  | Tintersection tyl ->
    List.exists tyl ~f:(this_appears_covariantly ~contra env)
  | Tfun ft ->
    this_appears_covariantly ~contra env ft.ft_ret
    || List.exists ft.ft_params ~f:(fun fp ->
           this_appears_covariantly ~contra:(not contra) env fp.fp_type)
  | Tshape
      (Shape_simple { s_origin = _; s_fields = fm; s_unknown_value = unknown })
    ->
    let fields = TShapeMap.elements fm in
    List.exists fields ~f:(fun (_, f) ->
        this_appears_covariantly ~contra env f.sft_ty)
    || this_appears_covariantly ~contra env unknown
  | Tshape (Shape_splat { ss_elems }) ->
    List.exists ss_elems ~f:(this_appears_covariantly ~contra env)
  | Taccess (ty, _)
  | Trefinement (ty, _)
  | Tlike ty
  | Toption ty ->
    this_appears_covariantly ~contra env ty
  | Tvec_or_dict (ty1, ty2) ->
    this_appears_covariantly ~contra env ty1
    || this_appears_covariantly ~contra env ty2
  | Tapply (pos_name, tyl) ->
    let tparams = Env.get_class_or_typedef_tparams env (snd pos_name) in
    this_appears_covariantly_params tparams tyl
  | Tmixed
  | Twildcard
  | Tany _
  | Tnonnull
  | Tdynamic _
  | Tprim _
  | Tgeneric _ ->
    false
  | Tclass_ptr ty -> this_appears_covariantly ~contra env ty

(** We know that the receiver is a concrete class, not a generic with
    bounds, or a Tunion. *)
let rec obj_get_concrete_ty
    args env concrete_ty ((id_pos, id_str) as id) on_error : internal_result =
  log_obj_get env `concrete id concrete_ty args.this_ty;
  let dflt_rval_mismatch =
    Option.map ~f:(fun (_, _, ty) -> Ok ty) args.coerce_from_ty
  and dflt_lval_mismatch = Ok concrete_ty in

  let default
      ?(lval_mismatch = dflt_lval_mismatch)
      ?(rval_mismatch = dflt_rval_mismatch)
      ty_err_opt =
    let (env, ty) = Env.fresh_type_error env id_pos in
    (env, ty_err_opt, (ty, []), lval_mismatch, rval_mismatch)
  in
  let read_context = Option.is_none args.coerce_from_ty in
  let (env, concrete_ty) = Env.expand_type env concrete_ty in
  match deref concrete_ty with
  | (r, Tclass ((_, class_name), _, paraml)) ->
    obj_get_concrete_class
      args
      env
      concrete_ty
      (id_pos, id_str)
      r
      class_name
      paraml
      on_error
  | (_, Tdynamic shadow_v_opt) ->
    let err_opt = sound_dynamic_err_opt args env id read_context in
    (* Dynamic inference: when accessing a property on a dynamic with a shadow
       type variable, create a fresh shadow type variable for the result and
       record a has_member constraint on the receiver's shadow. This tracks that
       the dynamic value was used as an object with property `id`. *)
    let (env, result_shadow) =
      match shadow_v_opt with
      | Some shadow_v ->
        let (env, v_prop) = Env.fresh_shadow_tyvar env id_pos in
        let prop_ty = mk (Reason.none, Tdynamic (Some v_prop)) in
        let hm =
          Typing_defs_constraints.
            {
              hm_name = id;
              hm_type = prop_ty;
              hm_class_id = Aast.CI (id_pos, "");
              hm_method = None;
            }
        in
        let env =
          Env.add_tyvar_upper_bound
            env
            shadow_v
            (ConstraintType
               (Typing_defs_constraints.mk_constraint_type
                  (Reason.none, Thas_member hm)))
        in
        (env, Some v_prop)
      | None -> (env, None)
    in
    let ty = mk (Reason.dynamic_prop id_pos, Tdynamic result_shadow) in
    (env, err_opt, (ty, []), dflt_lval_mismatch, dflt_rval_mismatch)
  | (_, Tany _) ->
    (env, None, (concrete_ty, []), dflt_lval_mismatch, dflt_rval_mismatch)
  | (r, Tnonnull) ->
    let ty_reasons =
      if Reason.Predicates.is_opaque_type_from_module r then
        lazy (Reason.to_string "This type is mixed" r)
      else
        lazy []
    in
    let ty_err =
      Typing_error.(
        primary
        @@ Primary.Top_member
             {
               pos = id_pos;
               name = id_str;
               ctxt =
                 (if read_context then
                   `read
                 else
                   `write);
               kind =
                 (if args.is_method then
                   `method_
                 else
                   `property);
               is_nullable = false;
               decl_pos = get_pos concrete_ty;
               ty_name = lazy (Typing_print.error env concrete_ty);
               ty_reasons;
             })
    in
    let ty_nothing = MakeType.nothing Reason.none in
    let lval_mismatch = Error (concrete_ty, ty_nothing) in
    default ~lval_mismatch (Some ty_err)
  | _ ->
    let ty_err =
      Typing_error.(
        apply ~on_error
        @@ primary
        @@ Primary.Non_object_member
             {
               pos = id_pos;
               ctxt =
                 (if read_context then
                   `read
                 else
                   `write);
               kind =
                 (if args.is_method then
                   `method_
                 else
                   `property);
               member_name = id_str;
               ty_name = lazy (Typing_print.error env concrete_ty);
               decl_pos = get_pos concrete_ty;
             })
    in
    let ty_nothing = MakeType.nothing Reason.none in
    let lval_mismatch = Error (concrete_ty, ty_nothing) in
    default ~lval_mismatch (Some ty_err)

and obj_get_concrete_class
    args
    env
    concrete_ty
    ((id_pos, id_str) as id)
    reason
    class_name
    params
    on_error : internal_result =
  match Env.get_class env class_name with
  | Decl_entry.DoesNotExist
  | Decl_entry.NotYetAvailable ->
    let ty = MakeType.nothing (Reason.missing_class id_pos) in
    ( env,
      None,
      (ty, []),
      Ok concrete_ty,
      Option.map ~f:(fun (_, _, ty) -> Ok ty) args.coerce_from_ty )
  | Decl_entry.Found class_info ->
    let (env, params) =
      if List.length params <> List.length (Cls.tparams class_info) then
        (* We've already generated an arity error so just fill out params
         * with error types *)
        List.map_env env (Cls.tparams class_info) ~f:(fun env _ ->
            Env.fresh_type_error env id_pos)
      else
        (env, params)
    in
    let old_member_info =
      get_member ~is_method:args.is_method env class_info id_str
    in
    let self_id = Option.value (Env.get_self_id env) ~default:"" in
    let ancestor_tyargs =
      match Cls.get_ancestor class_info self_id with
      | Some self_class_type -> begin
        match get_node self_class_type with
        | Tapply (_, tyargs) -> Some tyargs
        | _ -> None
      end
      | None ->
        let all_reqs = Cls.all_ancestor_reqs class_info in
        let filtered =
          List.filter_map all_reqs ~f:(fun (_, ty) ->
              match get_node ty with
              | Tapply ((_, name), tyargs) when String.equal name self_id ->
                Some tyargs
              | _ -> None)
        in
        List.hd filtered
    in

    let (member_info, shadowed) =
      match ancestor_tyargs with
      | Some tyargs -> begin
        (* We look up the current context to see if there is a field/method with
         * private visibility. If there is one, that one takes precedence *)
        match Env.get_self_class env with
        | Decl_entry.DoesNotExist
        | Decl_entry.NotYetAvailable ->
          (old_member_info, false)
        | Decl_entry.Found self_class -> begin
          match get_member ~is_method:args.is_method env self_class id_str with
          | Some ({ ce_visibility = Vprivate _; _ } as ce) ->
            let ce =
              Decl_instantiate.(
                instantiate_ce (make_subst (Cls.tparams self_class) tyargs) ce)
            in
            (* if a trait T has a require class C constraint, and both T and C
             * define a private member with the same name, then the two members
             * are aliased, not shadowed.  In this case, self_class is the trait
             * and the ancestor is the required class.  We can set the `shadowed`
             * bit by checking if self_class is a class or trait, there is
             * no need to additionally check if the trait has a require class
             * attribute. *)
            (Some ce, Ast_defs.is_c_class (Cls.kind self_class))
          | _ -> (old_member_info, false)
        end
      end
      | None -> (old_member_info, false)
    in
    begin
      match member_info with
      | None ->
        obj_get_concrete_class_without_member_info
          args
          env
          concrete_ty
          id
          reason
          class_info
          on_error
      | Some member_info ->
        obj_get_concrete_class_with_member_info
          args
          env
          concrete_ty
          id
          self_id
          shadowed
          class_name
          class_info
          params
          old_member_info
          member_info
    end

and obj_get_concrete_class_with_member_info
    args
    env
    concrete_ty
    ((id_pos, id_str) as _id)
    self_id
    shadowed
    class_name
    class_info
    params
    old_member_info
    member_info : internal_result =
  let dflt_rval_mismatch =
    Option.map ~f:(fun (_, _, ty) -> Ok ty) args.coerce_from_ty
  and dflt_lval_mismatch = Ok concrete_ty in
  let {
    ce_visibility = vis;
    ce_type = (lazy member_);
    ce_deprecated;
    ce_package_requirement;
    _;
  } =
    member_info
  in
  let mem_pos = get_pos member_ in

  let ty_err_opts =
    [
      Option.bind old_member_info ~f:(fun old_member_info ->
          if
            shadowed
            && not
                 (String.equal member_info.ce_origin old_member_info.ce_origin)
          then
            let (lazy old_member) = old_member_info.ce_type in
            Some
              Typing_error.(
                primary
                @@ Primary.Ambiguous_object_access
                     {
                       pos = id_pos;
                       name = id_str;
                       self_pos = get_pos member_;
                       vis =
                         Typing_defs.string_of_visibility
                           old_member_info.ce_visibility;
                       subclass_pos = get_pos old_member;
                       class_self = self_id;
                       class_subclass = class_name;
                     })
          else
            None);
      Typing_error.multiple_opt
      @@ TVis.check_obj_access
           ~is_method:args.is_method
           ~is_receiver_interface:
             (Ast_defs.is_c_interface (Cls.kind class_info))
           ~use_pos:id_pos
           ~def_pos:mem_pos
           ~tests_bypass_visibility:(get_ce_tests_bypass_visibility member_info)
           env
           vis;
      TVis.check_deprecated ~use_pos:id_pos ~def_pos:mem_pos env ce_deprecated;
      TVis.check_cross_package
        ~use_pos:id_pos
        ~def_pos:mem_pos
        env
        ce_package_requirement;
      (if args.is_parent_call && get_ce_abstract member_info then
        Some
          Typing_error.(
            primary
            @@ Primary.Parent_abstract_call
                 { meth_name = id_str; pos = id_pos; decl_pos = mem_pos })
      else
        None);
      TVis.check_expression_tree_vis ~use_pos:id_pos ~def_pos:mem_pos env vis;
    ]
  in

  let member_decl_ty = Typing_enum.member_type env member_info in
  let widen_this = this_appears_covariantly ~contra:true env member_decl_ty in
  let ety_env = mk_ety_env class_info params args.this_ty in
  let ((env, lcl_ty_err_opt), member_ty, tal, et_enforced, lval_mismatch) =
    match deref member_decl_ty with
    | (r, Tfun ft) when args.is_method ->
      (* We special case function types here to be able to pass explicit type
       * parameters. *)
      let ((env, lclz_ty_err_opt), explicit_targs) =
        Phase.localize_targs
          ~check_type_integrity:true
          ~is_method:args.is_method
          ~use_pos:id_pos
          ~def_pos:mem_pos
          ~use_name:(strip_ns id_str)
          env
          ft.ft_tparams
          (List.map ~f:snd args.explicit_targs)
      in
      Typing_needs_concrete.check_reified_targs
        env
        ft.ft_tparams
        args.explicit_targs;
      let ft =
        Typing_enforceability.compute_enforced_and_pessimize_fun_type
          ~this_class:(Some class_info)
          env
          ft
      in
      let ((env, ft1_ty_err_opt), ft1) =
        Phase.(
          localize_ft
            ~instantiation:
              { use_name = strip_ns id_str; use_pos = id_pos; explicit_targs }
            ~ety_env:{ ety_env with on_error = None }
            ~def_pos:mem_pos
            env
            ft)
      in
      let should_wrap = get_ce_support_dynamic_type member_info in
      let (ft_ty1, lval_mismatch) =
        let lval_mismatch =
          if should_wrap then
            (* If this is supportdyn, suppress the Hole on the receiver *)
            dflt_lval_mismatch
          else
            (* Phase.localize_ft will report an error, generated in
               Typing_generic_constraint.check_where_constraint,
               if we have a 4323 *)
            match ft1_ty_err_opt with
            | Some _ -> Error (concrete_ty, MakeType.nothing Reason.none)
            | _ -> dflt_lval_mismatch
        in

        ( Typing_dynamic.maybe_wrap_with_supportdyn
            ~should_wrap
            (Reason.localize r)
            ft1,
          lval_mismatch )
      in
      let ((env, ft_ty_err_opt), ft_ty) =
        if widen_this then
          let ety_env = { ety_env with this_ty = args.this_ty_conjunct } in
          let ((env, ft2_ty_err_opt), ft2) =
            Phase.(
              localize_ft
                ~instantiation:
                  {
                    use_name = strip_ns id_str;
                    use_pos = id_pos;
                    explicit_targs;
                  }
                ~ety_env:{ ety_env with on_error = None }
                ~def_pos:mem_pos
                env
                ft)
          in
          let ft_ty2 =
            Typing_dynamic.maybe_wrap_with_supportdyn
              ~should_wrap
              (Reason.localize r)
              ft2
          in
          let (env, ty) =
            Inter.intersect_list env (Reason.localize r) [ft_ty1; ft_ty2]
          in
          (* TODO: should we be taking the intersection of the errors? *)
          let ty_err_opt =
            Option.merge ft1_ty_err_opt ft2_ty_err_opt ~f:(fun e1 e2 ->
                Typing_error.multiple [e1; e2])
          in
          ((env, ty_err_opt), ty)
        else
          ((env, ft1_ty_err_opt), ft_ty1)
      in
      let ty_err_opt =
        Option.merge lclz_ty_err_opt ft_ty_err_opt ~f:Typing_error.both
      in

      ((env, ty_err_opt), ft_ty, explicit_targs, Unenforced, lval_mismatch)
    | _ ->
      let is_xhp_attr = Option.is_some (get_ce_xhp_attr member_info) in
      let (et_enforced, et_type) =
        Typing_enforceability.compute_enforced_and_pessimize_ty
          ~this_class:(Some class_info)
          env
          member_decl_ty
          ~explicitly_untrusted:is_xhp_attr
      in
      let (env, member_ty) = Phase.localize ~ety_env env et_type in
      (* TODO(T52753871): same as for class_get *)
      (env, member_ty, [], et_enforced, dflt_lval_mismatch)
  in
  (* TODO: iterate over the returned error rather than side effecting on
     error evaluation *)
  let eff () =
    let open Typing_env_types in
    if Tast.is_under_dynamic_assumptions env.checked then
      Typing_log.log_pessimise_prop
        env
        (Pos_or_decl.unsafe_to_raw_pos mem_pos)
        id_str
  in
  let (env, member_ty, coerce_ty_err_opt, rval_mismatch) =
    Option.value_map
      args.coerce_from_ty
      ~default:(env, member_ty, None, dflt_rval_mismatch)
      ~f:(fun (p, ur, ty) ->
        let err =
          Typing_error.Callback.(
            (with_side_effect ~eff unify_error [@alert "-deprecated"]))
        in
        let lty = TUtils.make_like_if_enforced env et_enforced member_ty in
        let (env, coerce_ty_err_opt) =
          Typing_coercion.coerce_type p ur env ty lty et_enforced err
        in
        let coerce_ty_mismatch =
          match coerce_ty_err_opt with
          | None -> Ok ty
          | _ -> Error (ty, member_ty)
        in
        (env, lty, coerce_ty_err_opt, Some coerce_ty_mismatch))
  in
  let ty_err_opt =
    Typing_error.multiple_opt
    @@ List.filter_map
         ~f:Fn.id
         (lcl_ty_err_opt :: coerce_ty_err_opt :: ty_err_opts)
  in
  (env, ty_err_opt, (member_ty, tal), lval_mismatch, rval_mismatch)

and obj_get_concrete_class_without_member_info
    args env concrete_ty (id_pos, id_str) reason class_info on_error :
    internal_result =
  let dflt_rval_mismatch =
    Option.map ~f:(fun (_, _, ty) -> Ok ty) args.coerce_from_ty
  and dflt_lval_mismatch = Ok concrete_ty in
  let default
      ?(lval_mismatch = dflt_lval_mismatch)
      ?(rval_mismatch = dflt_rval_mismatch)
      ty_err_opt =
    let (env, ty) = Env.fresh_type_error env id_pos in
    (env, ty_err_opt, (ty, []), lval_mismatch, rval_mismatch)
  in
  if not args.is_method then
    let (lval_mismatch, ty_err_opt) =
      if not (SN.Members.is_special_xhp_attribute id_str) then
        let err =
          member_not_found
            env
            id_pos
            ~is_method:args.is_method
            class_info
            id_str
            reason
            on_error
        in
        let ty_nothing = MakeType.nothing Reason.none in
        (Error (concrete_ty, ty_nothing), Some err)
      else
        (dflt_lval_mismatch, None)
    in
    default ~lval_mismatch ty_err_opt
  else if String.equal id_str SN.Members.__clone then
    (* Create a `public function __clone()[]: void {}` for classes that don't declare __clone *)
    let ft =
      {
        ft_tparams = [];
        ft_where_constraints = [];
        ft_params = [];
        ft_implicit_params =
          { capability = CapTy (MakeType.intersection Reason.none []) };
        ft_ret = MakeType.void Reason.none;
        ft_flags = Typing_defs_flags.Fun.default;
        ft_instantiated = true;
      }
    in
    ( env,
      None,
      (mk (Reason.none, Tfun ft), []),
      dflt_lval_mismatch,
      dflt_rval_mismatch )
  else
    let ty_err =
      member_not_found
        env
        id_pos
        ~is_method:args.is_method
        class_info
        id_str
        reason
        on_error
    in
    let ty_nothing = MakeType.nothing Reason.none in
    default ~lval_mismatch:(Error (concrete_ty, ty_nothing)) (Some ty_err)

and nullable_obj_get
    args env ety1 ((id_pos, id_str) as id) on_error ~read_context ty :
    internal_result =
  let (rcv_is_option, rcv_is_nothing) =
    match deref ety1 with
    | (_, Toption inner) ->
      (match get_node inner with
      | Tnonnull -> (false, true)
      | _ -> (true, false))
    | _ -> (false, false)
  in

  match args.nullsafe with
  | Some p1 ->
    let (env, ty_errs, (method_, tal), lval_mismatch, rval_mismatch) =
      obj_get_inner args env ty id on_error
    in
    let (env, ty) =
      make_nullable_member_type ~is_method:args.is_method env id_pos p1 method_
    in
    (env, ty_errs, (ty, tal), lval_mismatch, rval_mismatch)
  | None when rcv_is_option ->
    (* Try to type this as though it were nullsafe *)
    let (env, _ty_errs, (method_, tal), lval_mismatch, rval_mismatch) =
      obj_get_inner args env ty id on_error
    in
    let r = get_reason ety1 in
    (* If this _had_ been a nullsafe access and we would have reported no error
       we special case the error and type mismatch here to get better
       suggested types in holes *)
    let ty_errs =
      Typing_error.(
        primary
        @@ Primary.Null_member
             {
               pos = id_pos;
               obj_pos_opt = Some args.obj_pos;
               member_name = id_str;
               reason = lazy (Reason.to_string "This can be null" r);
               kind =
                 (if args.is_method then
                   `method_
                 else
                   `property);
               ctxt =
                 (if read_context then
                   `read
                 else
                   `write);
             })
    in
    let lval_mismatch =
      match lval_mismatch with
      | Ok _ -> Error (ety1, ty)
      | Error (_, suggest) -> Error (ety1, suggest)
    in
    (env, Some ty_errs, (method_, tal), lval_mismatch, rval_mismatch)
  | None ->
    let (ty_expect, ty_err) =
      let r = get_reason ety1 in
      if rcv_is_nothing then
        let ty_reasons =
          if Reason.Predicates.is_opaque_type_from_module r then
            lazy (Reason.to_string "This type is mixed" r)
          else
            lazy []
        in
        let ty_err =
          Typing_error.(
            primary
            @@ Primary.Top_member
                 {
                   pos = id_pos;
                   name = id_str;
                   ctxt =
                     (if read_context then
                       `read
                     else
                       `write);
                   kind =
                     (if args.is_method then
                       `method_
                     else
                       `property);
                   is_nullable = true;
                   decl_pos = Reason.to_pos r;
                   ty_name = lazy (Typing_print.error env ety1);
                   ty_reasons;
                 })
        in
        (MakeType.nothing Reason.none, ty_err)
      else
        let ty_err =
          Typing_error.(
            primary
            @@ Primary.Null_member
                 {
                   pos = id_pos;
                   obj_pos_opt = Some args.obj_pos;
                   member_name = id_str;
                   reason = lazy (Reason.to_string "This can be null" r);
                   kind =
                     (if args.is_method then
                       `method_
                     else
                       `property);
                   ctxt =
                     (if read_context then
                       `read
                     else
                       `write);
                 })
        in
        (MakeType.nothing Reason.none, ty_err)
    in
    let (env, ty) = Env.fresh_type_error env id_pos in
    ( env,
      Some ty_err,
      (ty, []),
      Error (ety1, ty_expect),
      Option.map ~f:(fun (_, _, ty) -> Ok ty) args.coerce_from_ty )

(* Helper method for obj_get that decomposes the type ty1.
 * The additional parameter this_ty represents the type that will be substitued
 * for `this` in the method signature.
 *
 * If ty1 is an intersection type, we distribute the member access through the
 * conjuncts but maintain the intersection type for this_ty. For example,
 * a call on (T & I & J) will recurse on T, I and J but with this_ty = (T & I & J),
 * so that we don't "lose" information when substituting for `this`.
 *
 * In contrast, if ty1 is a union type, we recurse on the disjuncts but pass these
 * through to this_ty, as the member access must be valid for each disjunct
 * separately. Likewise for nullables (special case of union).
 *)
and obj_get_inner args env receiver_ty ((id_pos, id_str) as id) on_error :
    internal_result =
  log_obj_get env `inner id receiver_ty args.this_ty;
  let (env, ety1') = Env.expand_type env receiver_ty in
  let was_var = is_tyvar ety1' in
  let dflt_rval_mismatch =
    Option.map ~f:(fun (_, _, ty) -> Ok ty) args.coerce_from_ty
  in
  let ((env, expand_ty_err_opt), ety1) =
    if args.is_method then
      Typing_solver.expand_type_and_solve
        env
        ~description_of_expected:"an object"
        args.obj_pos
        receiver_ty
    else
      Typing_solver.expand_type_and_narrow
        env
        ~description_of_expected:"an object"
        (widen_class_for_obj_get
           ~is_method:args.is_method
           ~nullsafe:args.nullsafe
           id_str)
        args.obj_pos
        receiver_ty
  in
  let (env, ety1) =
    if was_var then
      Typing_dependent_type.ExprDepTy.make_with_dep_kind env args.dep_kind ety1
    else
      (env, ety1)
  in
  let nullable_obj_get ~read_context ty =
    merge_ty_err expand_ty_err_opt
    @@ nullable_obj_get
         { args with this_ty = ty; this_ty_conjunct = ty }
         env
         ety1
         id
         on_error
         ~read_context
         ty
  in
  (* coerce_from_ty is used to store the source type for an assignment, so it
   * is a useful marker for whether we're reading or writing *)
  let read_context = Option.is_none args.coerce_from_ty in
  match deref ety1 with
  | (r, Tunion tyl) ->
    (* Filter out null elements *)
    let is_null ty =
      let (_, _, ty) = TUtils.strip_supportdyn env ty in
      match get_node ty with
      | Tprim Tnull -> true
      | _ -> false
    in
    let (tyl_null, tyl_nonnull) = List.partition_tf tyl ~f:is_null in
    if List.is_empty tyl_null then
      merge_ty_err expand_ty_err_opt
      @@ obj_get_inner_union args env on_error id r tyl
    else
      nullable_obj_get ~read_context (MakeType.union r tyl_nonnull)
  | (r, Tintersection tyl) ->
    let (is_nonnull, subty_err_opt) =
      if args.is_nonnull then
        (true, None)
      else
        Typing_solver.is_sub_type env receiver_ty (MakeType.nonnull Reason.none)
    in
    let ty_err_opt =
      Option.merge expand_ty_err_opt subty_err_opt ~f:Typing_error.both
    in
    merge_ty_err ty_err_opt
    @@ obj_get_inner_intersection { args with is_nonnull } env on_error id r tyl
  | (_, Tdependent (_, ty)) ->
    merge_ty_err expand_ty_err_opt @@ obj_get_inner args env ty id on_error
  | (r, Tnewtype (name, tyl, _)) ->
    let (env, ty) = Typing_utils.get_newtype_super env r name tyl in
    (* Unfortunately the bound on a newtype may itself be a type variable *)
    let ((env, _ty_err_opt), ty) =
      Typing_solver.expand_type_and_solve
        env
        ~description_of_expected:"an object"
        args.obj_pos
        ty
    in
    merge_ty_err expand_ty_err_opt @@ obj_get_inner args env ty id on_error
  | (r, Tgeneric name) when not (S_set.mem name args.seen) ->
    let args = { args with seen = S_set.add name args.seen } in
    (match TUtils.get_concrete_supertypes ~abstract_enum:true env ety1 with
    | (env, []) ->
      let ctxt =
        if read_context then
          `read
        else
          `write
      and kind =
        if args.is_method then
          `method_
        else
          `property
      in
      let ty_err =
        Typing_error.(
          apply ~on_error
          @@ primary
          @@ Primary.Non_object_member
               {
                 pos = id_pos;
                 ctxt;
                 kind;
                 member_name = id_str;
                 ty_name = lazy (Typing_print.error env ety1);
                 decl_pos = Reason.to_pos r;
               })
      in
      let ty_err_opt =
        Option.merge expand_ty_err_opt (Some ty_err) ~f:Typing_error.both
      in
      let ty_nothing = MakeType.nothing Reason.none in
      let lval_mismatch = Error (receiver_ty, ty_nothing) in
      let (env, ty) = Env.fresh_type_error env id_pos in
      (env, ty_err_opt, (ty, []), lval_mismatch, dflt_rval_mismatch)
    | (env, tyl) ->
      let (env, ty) = Inter.intersect_list env r tyl in
      let (env, ty) =
        if args.is_nonnull then
          Typing_intersection.intersect_with_nonnull
            env
            (Pos_or_decl.of_raw_pos args.obj_pos)
            ty
        else
          (env, ty)
      in
      merge_ty_err expand_ty_err_opt @@ obj_get_inner args env ty id on_error)
  | (_, Toption ty) -> nullable_obj_get ~read_context ty
  | (r, Tprim Tnull) ->
    let ty = mk (r, Tunion []) in
    nullable_obj_get ~read_context ty
  (* We are trying to access a member through a value of unknown type *)
  | (r, Tvar _) ->
    let ty_err_opt =
      if TUtils.is_tyvar_error env ety1 then
        None
      else
        Some
          Typing_error.(
            primary
            @@ Primary.Unknown_object_member
                 {
                   elt =
                     (if args.is_method then
                       `meth
                     else
                       `prop);
                   member_name = id_str;
                   pos = id_pos;
                   reason = lazy (Reason.to_string "It is unknown" r);
                 })
    in
    let ty_err_opt =
      Option.merge expand_ty_err_opt ty_err_opt ~f:Typing_error.both
    in
    let ty_nothing = MakeType.nothing Reason.none in
    let (env, ty) = Env.fresh_type_error env id_pos in
    ( env,
      ty_err_opt,
      (ty, []),
      Error (receiver_ty, ty_nothing),
      dflt_rval_mismatch )
  | (_, _) ->
    merge_ty_err expand_ty_err_opt
    @@ obj_get_concrete_ty args env ety1 id on_error

and obj_get_inner_union args env on_error id reason tys : internal_result =
  let (env, ty_errs, resultl, lval_mismatches, rval_mismatch_opts) =
    List.fold_left
      tys
      ~init:(env, [], [], [], [])
      ~f:(fun (env, ty_errs, tys, lval_mismatches, rval_mismatch_opts) ty ->
        let (env, ty_err, ty, lval_mismatch, rval_mismatch_opt) =
          obj_get_inner
            { args with this_ty = ty; this_ty_conjunct = ty }
            env
            ty
            id
            on_error
        in
        (* Preserve outer reason on dynamic *)
        let ty =
          if is_dynamic (fst ty) then
            (with_reason (fst ty) reason, snd ty)
          else
            ty
        in
        ( env,
          ty_err :: ty_errs,
          ty :: tys,
          lval_mismatch :: lval_mismatches,
          rval_mismatch_opt :: rval_mismatch_opts ))
  in
  let ty_err_opt = Typing_error.union_opt @@ List.filter_map ~f:Fn.id ty_errs in
  let (env, lval_mismatch) =
    mk_mismatch_union env @@ fold_mismatches lval_mismatches
  in
  let (env, rval_mismatch) =
    Option.value_map ~default:(env, None) ~f:(fun res ->
        let (env, r) = mk_mismatch_union env res in
        (env, Some r))
    @@ fold_mismatch_opts rval_mismatch_opts
  in
  (* TODO: decide what to do about methods with differing generic arity.
   * See T55414751 *)
  let tal =
    List.map ~f:snd resultl
    |> List.max_elt ~compare:(fun a b ->
           Int.compare (List.length a) (List.length b))
    |> Option.value ~default:[]
  in
  let tyl = List.map ~f:fst resultl in
  let (env, ty) = Union.union_list env reason tyl in
  (env, ty_err_opt, (ty, tal), lval_mismatch, rval_mismatch)

and obj_get_inner_intersection args env on_error id reason tys =
  let ((env, ty_err_opt), res) =
    TUtils.run_on_intersection_with_ty_err env tys ~f:(fun env ty ->
        let (env, ty_err_opt, res, lval_mismatch, rval_mismatch) =
          obj_get_inner { args with this_ty_conjunct = ty } env ty id on_error
        in
        ((env, ty_err_opt), (res, lval_mismatch, rval_mismatch)))
  in
  let (resultl, lval_mismatches, rval_mismatch_opts) = List.unzip3 res in

  let (env, lval_mismatch) =
    mk_mismatch_intersection env @@ fold_mismatches lval_mismatches
  in
  let (env, rval_mismatch) =
    Option.value_map ~default:(env, None) ~f:(fun res ->
        let (env, err) = mk_mismatch_intersection env res in
        (env, Some err))
    @@ fold_mismatch_opts rval_mismatch_opts
  in
  (* TODO: decide what to do about methods with differing generic arity.
   * See T55414751 *)
  let tal =
    match resultl with
    | [] -> []
    | (_, tal) :: _ -> tal
  in
  let tyl = List.map ~f:fst resultl in
  let (env, ty) = Inter.intersect_list env reason tyl in
  (env, ty_err_opt, (ty, tal), lval_mismatch, rval_mismatch)

(* Look up the type of the property or method id in the type receiver_ty of the
 * receiver and use the function k to postprocess the result.
 * Return any fresh type variables that were substituted for generic type
 * parameters in the type of the property or method.
 *
 * Essentially, if receiver_ty is a concrete type, e.g., class C, then k is applied
 * to the type of the property id in C; and if receiver_ty is an unresolved type,
 * e.g., a union of classes (C1 | ... | Cn), then k is applied to the type
 * of the property id in each Ci and the results are collected into an
 * unresolved type.
 *
 *)
let obj_get_with_mismatches_helper
    ~obj_pos
    ~is_method
    ~nullsafe
    ~coerce_from_ty
    ~explicit_targs
    ~class_id
    ~member_id
    ~on_error
    ?parent_ty
    env
    receiver_ty =
  Typing_log.(
    log_with_level env "obj_get" ~level:1 (fun () ->
        log_types
          (Pos_or_decl.of_raw_pos obj_pos)
          env
          [
            Log_head
              ( "obj_get (member_id: " ^ snd member_id ^ ")",
                [Log_type ("receiver_ty", receiver_ty)] );
          ]));
  let ((env, e1), receiver_ty) =
    if is_method then
      Typing_solver.expand_type_and_solve
        env
        ~description_of_expected:"an object"
        obj_pos
        receiver_ty
    else
      Typing_solver.expand_type_and_narrow
        env
        ~description_of_expected:"an object"
        (widen_class_for_obj_get ~is_method ~nullsafe (snd member_id))
        obj_pos
        receiver_ty
  in

  (* We will substitute `this` in the function signature with `this_ty`. But first,
   * transform it according to the dependent kind dep_kind that was derived from the
   * class_id in the original call to obj_get. See Typing_dependent_type.ml for
   * more details.
   *)
  let dep_kind =
    Typing_dependent_type.ExprDepTy.from_cid
      env
      (get_reason receiver_ty)
      class_id
  in
  let (env, receiver_ty) =
    Typing_dependent_type.ExprDepTy.make_with_dep_kind env dep_kind receiver_ty
  in
  let receiver_or_parent_ty =
    match parent_ty with
    | Some ty -> ty
    | None -> receiver_ty
  in
  let is_parent_call = Nast.equal_class_id_ class_id Aast.CIparent in
  let args =
    {
      is_method;
      nullsafe;
      obj_pos;
      explicit_targs;
      coerce_from_ty;
      is_nonnull = false;
      is_parent_call;
      dep_kind;
      this_ty = receiver_ty;
      this_ty_conjunct = receiver_ty;
      seen = S_set.empty;
    }
  in
  let (env, e2, ty, lval_err, rval_err_opt) =
    let (env, e2, ty, lvarl_err, rval_err_opt) =
      obj_get_inner args env receiver_or_parent_ty member_id on_error
    in
    (* If we failed on a static receiver type in SDT dynamic method check, try again
     * on dynamic, if the receiver type supports dynamic.
     *)
    if
      Option.is_some e2
      && Tast.is_under_dynamic_assumptions env.Typing_env_types.checked
      && (not (is_dynamic receiver_or_parent_ty))
      && TUtils.is_sub_type
           env
           receiver_or_parent_ty
           (MakeType.dynamic Reason.none)
    then
      obj_get_inner
        args
        env
        (MakeType.dynamic (Reason.witness obj_pos))
        member_id
        on_error
    else
      (env, e2, ty, lvarl_err, rval_err_opt)
  in
  let from_res = Result.fold ~ok:(fun _ -> None) ~error:(fun tys -> Some tys) in
  let ty_err_opt = Option.merge e1 e2 ~f:Typing_error.both
  and lval_err_opt = from_res lval_err
  and rval_err_opt = Option.bind ~f:from_res rval_err_opt in
  ((env, ty_err_opt), ty, lval_err_opt, rval_err_opt)

let obj_get_with_mismatches
    ~obj_pos
    ~is_method
    ~nullsafe
    ~coerce_from_ty
    ~explicit_targs
    ~class_id
    ~member_id
    ~on_error
    ?parent_ty
    env
    receiver_ty =
  let ((env0, ty_err_opt0), (fty0, tal0), lval_err_opt0, rval_err_opt0) =
    obj_get_with_mismatches_helper
      ~obj_pos
      ~is_method
      ~nullsafe
      ~coerce_from_ty
      ~explicit_targs
      ~class_id
      ~member_id
      ~on_error
      ?parent_ty
      env
      receiver_ty
  in
  if Option.is_none ty_err_opt0 || not is_method then
    ((env0, ty_err_opt0), (fty0, tal0), lval_err_opt0, rval_err_opt0)
  else
    (* Under Sound Dynamic, we might be calling through a receiver whose like-type
     * instantiation doesn't satisfy where constraints on the method. For example,
     * class ExpectObj<T> { public function foo() where T as Container<mixed> ...}
     * might be called through ExpectObj<~vec<int>> for which ~vec<int> is not a
     * subtype of Container<mixed>.
     *
     * But by first like-pushing to a supertype, i.e. ~ExpectObj<vec<int>>, we can
     * type-check the call. We do this by attempting a subtype
     *   ExpectObj<~vec<int>> <: ~ExpectObj<#0> <: suppordyn<dynamic>
     * where #0 is a fresh type variable. Because we are calling through a like, we
     * propagate this to the function type of the method.
     *
     * Compare SDT calling for top-level functions e.g.
     *   function foo<T>(ExpectObj<T>) where T as Container<mixed>
     *)
    let rec freshen_receiver_ty_and_assert_subtype env ty =
      let (env, ty) = Env.expand_type env ty in
      let r = get_reason ty in
      (* Now produce fresh type variables for type arguments *)
      match get_node ty with
      | Tclass (id, exact, tyargs) ->
        let (env, freshened_tyargs) =
          List.map_env env tyargs ~f:(fun env _ty ->
              Env.fresh_type_invariant env obj_pos)
        in
        let freshened_receiver_ty =
          mk (r, Tclass (id, exact, freshened_tyargs))
        in
        let like_receiver_ty = MakeType.locl_like r freshened_receiver_ty in
        (* Perform the two subtype assertions mentioned in the comment above. We
         * expect these to succeed because the type variables are unconstrained
         *)
        let (env, _ty_err3) =
          Typing_ops.sub_type
            obj_pos
            Reason.URnone
            env
            freshened_receiver_ty
            (MakeType.supportdyn r (MakeType.mixed r))
            Typing_error.Callback.unify_error
        in
        let (env, _ty_err2) =
          Typing_ops.sub_type
            obj_pos
            Reason.URnone
            env
            ty
            like_receiver_ty
            Typing_error.Callback.unify_error
        in
        (env, freshened_receiver_ty)
      | Tunion tyl ->
        let (env, tyl) =
          List.map_env env tyl ~f:freshen_receiver_ty_and_assert_subtype
        in
        (env, mk (r, Tunion tyl))
      | _ -> (env, ty)
    in
    (* First strip off top-level like, so we can see the class (or union of classes) underneath *)
    let (env, stripped_receiver_ty) =
      Typing_dynamic_utils.strip_dynamic env receiver_ty
    in
    let (env, freshened_receiver_ty) =
      freshen_receiver_ty_and_assert_subtype env stripped_receiver_ty
    in
    let ((env, ty_err_opt), (fty, tal), lval_err_opt, rval_err_opt) =
      obj_get_with_mismatches_helper
        ~obj_pos
        ~is_method
        ~nullsafe
        ~coerce_from_ty
        ~explicit_targs
        ~class_id
        ~member_id
        ~on_error
        ?parent_ty
        env
        freshened_receiver_ty
    in
    (* If calling through both non-like *and* like types had errors, pick the non-like
     * type version because the messages are likely to be more comprehensible *)
    if Option.is_some ty_err_opt then
      ((env0, ty_err_opt0), (fty0, tal0), lval_err_opt0, rval_err_opt0)
    else
      (* Otherwise, use the result from the like-type receiver and add
       * a like to the function type
       *)
      let fty = Typing_make_type.locl_like (get_reason fty) fty in
      ((env, ty_err_opt), (fty, tal), lval_err_opt, rval_err_opt)

let obj_get
    ~obj_pos
    ~is_method
    ~nullsafe
    ~coerce_from_ty
    ~explicit_targs
    ~class_id
    ~member_id
    ~on_error
    ?parent_ty
    env
    receiver_ty =
  let (env, ty, _lval_err_opt, _rval_err_opt) =
    obj_get_with_mismatches
      ~obj_pos
      ~is_method
      ~nullsafe
      ~coerce_from_ty
      ~explicit_targs
      ~class_id
      ~member_id
      ~on_error
      ?parent_ty
      env
      receiver_ty
  in
  (env, ty)
