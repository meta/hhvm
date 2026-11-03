(*
 * Copyright (c) 2015, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude
open Typing_defs
open Utils
module SN = Naming_special_names

(*****************************************************************************)
(* Module checking the (co/contra)variance annotations (+/-).
 *
 * The algorithm works by tracking the variance of *uses* of a type parameter,
 * and checks that the variance *declaration* on that type parameter is
 * consistent with its uses.
 *
 * For every type parameter use, we keep a witness (a position in the source),
 * that tells us where the covariance (or contravariance) was deduced.
 * This way, when we find an error, we can point to the place that was
 * problematic (as usual).
 *)
(*****************************************************************************)

(* Type describing the kind of position we are dealing with.
 * Pos.t gives us the position in the source, it doesn't tell us the kind
 * of position we are dealing with. This type keeps track of that.
 *)
type position_descr =
  | Rtypedef
  | Rproperty (* Instance variable  *)
  | Rtype_parameter (* The declaration site of a type-parameter *)
  | Rfun_parameter
  | Rfun_return
  | Rtype_argument of string
    (* The argument of a parametric class or
     * typedef:
     * A<T1, ..>, T1 is (Rtype_argument "A")
     *)
  | Rconstraint_as
  | Rconstraint_eq
  | Rconstraint_super
  | Rrefinement_eq
  | Rrefinement_as
  | Rrefinement_super
  | Rwhere_as
  | Rwhere_super
  | Rwhere_eq
  | Rcase_type_where_as
  | Rcase_type_where_super
  | Rcase_type_where_eq
  | Rfun_inout_parameter

type position_variance =
  | Pcovariant
  | Pcontravariant
  | Pinvariant

type reason = Pos.t * position_descr * position_variance

(* The variance that we have inferred for a given use of a type-parameter. We keep
 * a stack of reasons that made us infer the variance of a position.
 * For example:
 * T appears in foo(...): (function(...): T)
 * T is inferred as covariant + a stack made of two elements:
 * -) The first one points to the position of T
 * -) The second one points to the position of (function(...): T)
 * We can, thanks to this stack, detail why we think something is covariant
 * in the error message.
 *)
type variance =
  (* The type parameter appeared in covariant position. *)
  | Vcovariant of reason list
  (* The type parameter appeared in contravariant position. *)
  | Vcontravariant of reason list
  (* The type parameter appeared in both covariant and contravariant position.
   * We keep a stack for each side: the left hand side is proof for covariance,
   * while the right hand side is proof for contravariance.
   *)
  | Vinvariant of reason list * reason list
  (* The type parameter is not used, or is a method type parameter.
   *)
  | Vboth

module Env = struct
  (** The set of type parameters which are in scope, with their variances. *)
  type type_parameter_env = variance S_map.t

  type t = {
    tpenv: type_parameter_env;
    enclosing_class: Nast.class_ option;
    env: Typing_env_types.env;
  }

  let remove_type_parameter : t -> string -> t =
   (fun env name -> { env with tpenv = S_map.remove name env.tpenv })
end

(*****************************************************************************)
(* Reason pretty-printing *)
(*****************************************************************************)

let variance_to_string = function
  | Pcovariant -> "covariant (+)"
  | Pcontravariant -> "contravariant (-)"
  | Pinvariant -> "invariant"

let variance_to_sign = function
  | Pcovariant -> "(+)"
  | Pcontravariant -> "(-)"
  | Pinvariant -> "(I)"

let reason_stack_to_string variance reason_stack =
  Printf.sprintf
    "This position is %s because it is the composition of %s\nThe rest of the error messages decomposes the inference of the variance.\nCheck out this link if you don't understand what this is about:\nhttp://en.wikipedia.org/wiki/Covariance_and_contravariance_(computer_science)"
    variance
    (List.fold_right
       reason_stack
       ~f:
         begin
           (fun (_, _, pvariance) acc -> variance_to_sign pvariance ^ acc)
         end
       ~init:"")

let reason_to_string ~sign (_, descr, variance) =
  (if sign then
    variance_to_sign variance ^ " "
  else
    "")
  ^
  match descr with
  | Rtypedef -> "Aliased types are covariant"
  | Rproperty -> "A non private class property is always invariant"
  | Rtype_parameter ->
    "The type parameter was declared as " ^ variance_to_string variance
  | Rfun_parameter -> "Function parameters are contravariant"
  | Rfun_return -> "Function return types are covariant"
  | Rtype_argument name ->
    Printf.sprintf
      "This type parameter was declared as %s (cf '%s')"
      (variance_to_string variance)
      name
  | Rconstraint_super ->
    "`super` constraints on method type parameters are covariant"
  | Rconstraint_as ->
    "`as` constraints on method type parameters are contravariant"
  | Rconstraint_eq -> "`=` constraints on method type parameters are invariant"
  | Rwhere_as ->
    "`where _ as _` constraints are covariant on the left, contravariant on the right"
  | Rwhere_eq -> "`where _ = _` constraints are invariant on the left and right"
  | Rwhere_super ->
    "`where _ super _` constraints are contravariant on the left, covariant on the right"
  | Rcase_type_where_as ->
    "`case type where _ as _` constraints are covariant on the left, contravariant on the right"
  | Rcase_type_where_eq ->
    "`case type where _ = _` constraints are invariant on the left and right"
  | Rcase_type_where_super ->
    "`case type where _ super _` constraints are contravariant on the left, covariant on the right"
  | Rfun_inout_parameter ->
    "Inout/ref function parameters are both covariant and contravariant"
  | Rrefinement_eq -> "exact refinements are invariant"
  | Rrefinement_as -> "`as` refinement bounds are covariant"
  | Rrefinement_super -> "`super` refinement bounds are contravariant"

let detailed_message variance pos stack =
  match stack with
  | [] -> []
  | [((p, _, _) as r)] -> [(p, reason_to_string ~sign:false r)]
  | _ ->
    (pos, reason_stack_to_string variance stack)
    :: List.map stack ~f:(fun ((p, _, _) as r) ->
           (p, reason_to_string ~sign:true r))

(*****************************************************************************)
(* Converts an annotation (+/-) to a type. *)
(*****************************************************************************)

let make_variance reason pos = function
  | Ast_defs.Covariant -> Vcovariant [(pos, reason, Pcovariant)]
  | Ast_defs.Contravariant -> Vcontravariant [(pos, reason, Pcontravariant)]
  | Ast_defs.Invariant ->
    Vinvariant ([(pos, reason, Pinvariant)], [(pos, reason, Pinvariant)])

(*****************************************************************************)
(* Used when we want to compose with the variance coming from another class.
 * Let's assume: A<-T>
 * public function foo(A<T> $x): void;
 * A<T> is in contravariant position so we process -A<T>.
 * because A is annotated with a minus: we deduce --T (which becomes T).
 *)
(*****************************************************************************)

let compose (pos, param_descr) from to_ =
  (* We don't really care how we deduced the variance that we are composing
   * with (_stack_to). That's because the decomposition could be in a different
   * file and would be too hard to follow anyway.
   * Let's consider the following return type: A<T>.
   * Turns out A is declared as A<-T>.
   * It's not a good idea for us to point to what made us deduce that the
   * position was contravariant (the declaration side). Because the user will
   * wonder where it comes from.
   * It's better to point to the return type (more precisely, point to the T
   * in the return type) and explain that this position is contravariant.
   * Later on, the user can go and check the definition of A for herself.
   *)
  match (from, to_) with
  | (Vcovariant stack_from, Vcovariant _stack_to) ->
    let reason = (pos, param_descr, Pcovariant) in
    Vcovariant (reason :: stack_from)
  | (Vcontravariant stack_from, Vcontravariant _stack_to) ->
    let reason = (pos, param_descr, Pcontravariant) in
    Vcovariant (reason :: stack_from)
  | (Vcovariant stack_from, Vcontravariant _stack_to) ->
    let reason = (pos, param_descr, Pcontravariant) in
    Vcontravariant (reason :: stack_from)
  | (Vcontravariant stack_from, Vcovariant _stack_to) ->
    let reason = (pos, param_descr, Pcovariant) in
    Vcontravariant (reason :: stack_from)
  | ((Vinvariant _ as x), _) -> x
  | (_, Vinvariant (_co, _contra)) ->
    let reason = (pos, param_descr, Pinvariant) in
    Vinvariant ([reason], [reason])
  | (Vboth, x)
  | (x, Vboth) ->
    x

(*****************************************************************************)
(* Given a type parameter, returns the declared variance. *)
(*****************************************************************************)

let get_tparam_variance env name =
  match S_map.find_opt name env with
  | None -> Vboth
  | Some x -> x

(*****************************************************************************)
(* Given a type parameter, returns the variance declared. *)
(*****************************************************************************)

let get_declared_variance : Pos.t -> Ast_defs.variance -> variance =
  make_variance Rtype_parameter

let make_decl_tparam_variance : Typing_defs.decl_tparam -> variance =
 fun t ->
  get_declared_variance
    (fst t.tp_name |> Pos_or_decl.unsafe_to_raw_pos)
    t.tp_variance

let get_declared_variance : Nast.tparam -> variance =
 (fun t -> get_declared_variance (fst t.Aast.tp_name) t.Aast.tp_variance)

(******************************************************************************)
(* Checks that a 'this' type is correctly used at a given contravariant       *)
(* position in a final class.                                                 *)
(******************************************************************************)
let check_final_this_pos_variance :
    Typing_env_types.env -> variance -> Pos.t -> Nast.class_ -> unit =
 fun env env_variance rpos class_ ->
  if class_.Aast.c_final then
    List.iter class_.Aast.c_tparams ~f:(fun t ->
        match (env_variance, t.Aast.tp_variance) with
        | (Vcontravariant _, (Ast_defs.Covariant | Ast_defs.Contravariant)) ->
          Typing_error_utils.add_typing_error
            ~env
            Typing_error.(
              primary
              @@ Primary.Contravariant_this
                   {
                     pos = rpos;
                     class_name =
                       Utils.strip_ns (Ast_defs.get_id class_.Aast.c_name);
                     typaram_name = snd t.Aast.tp_name;
                   })
        | _ -> ())

(*****************************************************************************)
(* Returns the list of type parameter variance for a given class.
 *
 * N.B.: this function works both with classes and typedefs.
 *)
(*****************************************************************************)

let get_class_variance env class_name =
  let tparams = Typing_env.get_class_or_typedef_tparams env class_name in
  List.map tparams ~f:make_decl_tparam_variance

let union (pos1, neg1) (pos2, neg2) =
  ( S_map.union ~combine:(fun _ x y -> Some (x @ y)) pos1 pos2,
    S_map.union ~combine:(fun _ x y -> Some (x @ y)) neg1 neg2 )

let flip (pos, neg) = (neg, pos)

let rec get_typarams_union ~tracked tenv acc (ty : decl_ty) =
  union acc (get_typarams ~tracked tenv ty)

and get_typarams ~tracked tenv (ty : decl_ty) =
  let empty = (S_map.empty, S_map.empty) in
  let get_typarams_union = get_typarams_union ~tracked tenv in
  let get_typarams_list tyl =
    List.fold_left tyl ~init:empty ~f:get_typarams_union
  in
  let get_typarams = get_typarams ~tracked tenv in
  let single id pos = (S_map.singleton id [pos], S_map.empty) in
  let rec get_typarams_variance_list acc variancel tyl =
    match (variancel, tyl) with
    | (variance :: variancel, ty :: tyl) ->
      let param = get_typarams ty in
      let param =
        match variance with
        | Vcovariant _ -> param
        | Vcontravariant _ -> flip param
        | _ -> union param (flip param)
      in
      get_typarams_variance_list (union acc param) variancel tyl
    | _ -> acc
  in
  match get_node ty with
  | Tgeneric id ->
    (* Only count tracked generic parameters *)
    if S_set.mem id tracked then
      single id (get_reason ty)
    else
      empty
  | Tnonnull
  | Tdynamic _
  | Tprim _
  | Tany _
  | Tthis
  | Tmixed
  | Twildcard ->
    empty
  | Toption ty
  | Tlike ty
  | Taccess (ty, _) ->
    get_typarams ty
  | Trefinement (ty, rs) ->
    S_map.fold
      (fun _ { rc_bound; _ } acc ->
        union acc
        @@
        match rc_bound with
        | TRexact bnd ->
          let tp = get_typarams bnd in
          union (flip tp) tp
        | TRloose bnds ->
          (* Lower bounds on type members are contravariant
           * while upper bounds are covariant. Interestingly,
           * this differs from where constraints because,
           * logically, those sit on the LHS of an
           * implication while refinements are on the RHS.
           *   (A => B) |- (A' => B ) if A' |- A
           *   (A => B) |- (A  => B') if B  |- B'
           *)
          union
            (flip (get_typarams_list bnds.tr_lower))
            (get_typarams_list bnds.tr_upper))
      rs.cr_consts
      (get_typarams ty)
  | Tunion tyl
  | Tintersection tyl ->
    get_typarams_list tyl
  | Ttuple { t_required; t_optional; t_extra = Tvariadic t_vs | Tsplat t_vs } ->
    union
      (get_typarams_list t_optional)
      (union (get_typarams_list t_required) (get_typarams t_vs))
  | Tshape (Shape_simple { s_fields = m; s_unknown_value; _ }) ->
    TShapeMap.fold
      (fun _ { sft_ty; _ } res -> get_typarams_union res sft_ty)
      m
      (get_typarams s_unknown_value)
  | Tshape (Shape_splat { ss_elems }) ->
    List.fold_left ss_elems ~init:empty ~f:(fun acc ty ->
        get_typarams_union acc ty)
  | Tfun ft ->
    let get_typarams_param acc fp =
      let tp = get_typarams fp.fp_type in
      let tp =
        match get_fp_mode fp with
        (* Parameters behave contravariantly *)
        | FPnormal -> flip tp
        (* Inout parameters behave both co- and contra-variantly *)
        | FPinout -> union tp (flip tp)
      in
      union acc tp
    in
    let params =
      List.fold_left ft.ft_params ~init:empty ~f:get_typarams_param
    in
    let implicit_params =
      let { capability } = ft.ft_implicit_params in
      match capability with
      | CapDefaults _ -> empty
      | CapTy ty -> get_typarams ty
    in
    let ret = get_typarams ft.ft_ret in
    let get_typarams_constraint acc (ck, ty) =
      union
        acc
        (match ck with
        | Ast_defs.Constraint_as -> get_typarams ty
        | Ast_defs.Constraint_eq ->
          let tp = get_typarams ty in
          union (flip tp) tp
        | Ast_defs.Constraint_super -> flip (get_typarams ty))
    in
    let get_typarams_tparam acc tp =
      List.fold_left tp.tp_constraints ~init:acc ~f:get_typarams_constraint
    in
    let bounds =
      List.fold_left ft.ft_tparams ~init:empty ~f:get_typarams_tparam
    in
    let get_typarams_where_constraint acc (ty1, ck, ty2) =
      union
        acc
        (match ck with
        | Ast_defs.Constraint_super ->
          union (flip (get_typarams ty1)) (get_typarams ty2)
        | Ast_defs.Constraint_as ->
          union (get_typarams ty1) (flip (get_typarams ty2))
        | Ast_defs.Constraint_eq ->
          let tp = union (get_typarams ty1) (get_typarams ty2) in
          union tp (flip tp))
    in
    let constrs =
      List.fold_left
        ft.ft_where_constraints
        ~init:empty
        ~f:get_typarams_where_constraint
    in

    (* Result so far: bounds, constraints, return, parameters *)
    let result =
      params
      |> union implicit_params
      |> union ret
      |> union constrs
      |> union bounds
    in

    (* Get the lower bounds of a type parameter, including where constraints
     * of the form `where t as T` or `where T super t`
     *)
    let get_lower_bounds tp =
      let name = snd tp.tp_name in
      List.filter_map
        ~f:(fun (ck, ty) ->
          match ck with
          | Ast_defs.Constraint_super
          | Ast_defs.Constraint_eq ->
            Some ty
          | _ -> None)
        tp.tp_constraints
      @ List.filter_map
          ~f:(fun (ty1, ck, ty2) ->
            match ck with
            | Ast_defs.Constraint_as
            | Ast_defs.Constraint_eq
              when is_generic_equal_to name ty2 ->
              Some ty1
            | Ast_defs.Constraint_super
            | Ast_defs.Constraint_eq
              when is_generic_equal_to name ty1 ->
              Some ty2
            | _ -> None)
          ft.ft_where_constraints
    in

    (* Get the upper bounds of a type parameter, including where constraints
     * of the form `where T as t` or `where t super T`
     *)
    let get_upper_bounds tp =
      let name = snd tp.tp_name in
      List.filter_map
        ~f:(fun (ck, ty) ->
          match ck with
          | Ast_defs.Constraint_as
          | Ast_defs.Constraint_eq ->
            Some ty
          | _ -> None)
        tp.tp_constraints
      @ List.filter_map
          ~f:(fun (ty1, ck, ty2) ->
            match ck with
            | Ast_defs.Constraint_as
            | Ast_defs.Constraint_eq
              when is_generic_equal_to name ty1 ->
              Some ty2
            | Ast_defs.Constraint_super
            | Ast_defs.Constraint_eq
              when is_generic_equal_to name ty2 ->
              Some ty1
            | _ -> None)
          ft.ft_where_constraints
    in

    (* If a type parameter appears covariantly, then treat its lower bounds as covariant *)
    let propagate_covariant_to_lower_bounds tp =
      get_typarams_list (get_lower_bounds tp)
    in

    (* If a type parameter appears contravariantly, then treat its upper bounds as contravariant *)
    let propagate_contravariant_to_upper_bounds tp =
      flip (get_typarams_list (get_upper_bounds tp))
    in

    (* Given a type parameter, propagate its variance (co and/or contra) to lower and upper bounds
     * as appropriate. See Typing_env.set_tyvar_appears_covariantly_and_propagate etc for an
     * analagous calculation for type variables.
     *)
    let propagate_typarams_tparam acc tp =
      let acc =
        if S_map.mem (snd tp.tp_name) (fst result) then
          union acc (propagate_covariant_to_lower_bounds tp)
        else
          acc
      in
      let acc =
        if S_map.mem (snd tp.tp_name) (snd result) then
          union acc (propagate_contravariant_to_upper_bounds tp)
        else
          acc
      in
      acc
    in
    List.fold_left ft.ft_tparams ~init:result ~f:propagate_typarams_tparam
  | Tapply ((_, name), tyl) ->
    let variancel = get_class_variance tenv name in
    get_typarams_variance_list empty variancel tyl
  | Tvec_or_dict (ty1, ty2) -> union (get_typarams ty1) (get_typarams ty2)
  | Tclass_ptr ty -> get_typarams ty

let get_positive_negative_generics ~tracked ~is_mutable env acc ty =
  let r = get_typarams ~tracked env ty in
  if is_mutable then
    union acc (union r (flip r))
  else
    union acc r

let generic_ :
    Typing_env_types.env -> Env.type_parameter_env -> variance -> string -> unit
    =
 fun tyenv env variance name ->
  let declared_variance = get_tparam_variance env name in
  match (declared_variance, variance) with
  (* Happens if type parameter isn't from class *)
  | (Vboth, _)
  | (_, Vboth) ->
    ()
  | (Vinvariant _, _)
  | (Vcovariant _, Vcovariant _)
  | (Vcontravariant _, Vcontravariant _) ->
    ()
  | (Vcovariant stack1, (Vcontravariant stack2 | Vinvariant (_, stack2))) ->
    let (pos1, _, _) = List.hd_exn stack1 in
    let (pos2, _, _) = List.hd_exn stack2 in
    let emsg = lazy (detailed_message "contravariant (-)" pos2 stack2) in
    Typing_error_utils.add_typing_error
      ~env:tyenv
      Typing_error.(
        primary
        @@ Primary.Declared_covariant
             { param_pos = pos1; pos = pos2; msgs = emsg })
  | (Vcontravariant stack1, (Vcovariant stack2 | Vinvariant (stack2, _))) ->
    let (pos1, _, _) = List.hd_exn stack1 in
    let (pos2, _, _) = List.hd_exn stack2 in
    let emsg = lazy (detailed_message "covariant (+)" pos2 stack2) in
    Typing_error_utils.add_typing_error
      ~env:tyenv
      Typing_error.(
        primary
        @@ Primary.Declared_contravariant
             { param_pos = pos2; pos = pos1; msgs = emsg })

(*****************************************************************************)
(* Used for the arguments of function. *)
(*****************************************************************************)

let flip reason = function
  | Vcovariant stack -> Vcontravariant (reason :: stack)
  | Vcontravariant stack -> Vcovariant (reason :: stack)
  | Vinvariant _ as x -> x
  | Vboth -> Vboth

let detail reason = function
  | Vcovariant stack -> Vcovariant (reason :: stack)
  | Vcontravariant stack -> Vcontravariant (reason :: stack)
  | variance -> variance

let rec hint : Env.t -> variance -> Aast_defs.hint -> unit =
 fun env variance (pos, h) ->
  let open Aast_defs in
  match h with
  | Habstr name ->
    (* This section makes the position more precise.
     * Say we find a return type that is a tuple (int, int, T).
     * The whole tuple is in covariant position, and so the position
     * is going to include the entire tuple.
     * That can make things pretty unreadable when the type is long.
     * Here we replace the position with the exact position of the generic
     * that was problematic. *)
    let variance =
      match variance with
      | Vcovariant ((pos', x, y) :: rest) when not (Pos.equal pos pos') ->
        Vcovariant ((pos, x, y) :: rest)
      | Vcontravariant ((pos', x, y) :: rest) when not (Pos.equal pos pos') ->
        Vcontravariant ((pos, x, y) :: rest)
      | x -> x
    in
    generic_ env.Env.env env.Env.tpenv variance name
  | Hmixed
  | Hwildcard
  | Hnonnull
  | Hdynamic
  | Hnothing
  | Hvar _
  | Hfun_context _
  | Hprim _ ->
    ()
  | Hvec_or_dict (hk, hv) ->
    Option.iter hk ~f:(hint env variance);
    hint env variance hv
  | Hthis ->
    (* Check that 'this' isn't being improperly referenced in a contravariant
     * position.
     * With the exception of that check, `this` constraints are bivariant
     * (otherwise any class that used the `this` type would not be able to use
     * covariant type params). *)
    Option.value_map
      env.Env.enclosing_class
      ~default:()
      ~f:(check_final_this_pos_variance env.Env.env variance pos)
  | Hclass_ptr (_, h)
  | Hoption h
  | Hlike h
  | Hsoft h
  | Haccess (h, _) ->
    hint env variance h
  | Hunion tyl
  | Hintersection tyl ->
    hint_list env variance tyl
  | Htuple { tup_required; tup_extra } ->
    hint_list env variance tup_required;
    begin
      match tup_extra with
      | Hextra { tup_optional; tup_variadic } ->
        hint_list env variance tup_optional;
        Option.iter tup_variadic ~f:(hint env variance)
      | Hsplat tup_splat -> hint env variance tup_splat
    end
  | Hshape
      { nsi_allows_unknown_fields = _; nsi_field_map; nsi_unknown_fields_type }
    ->
    List.iter nsi_field_map ~f:(function
        | SE_field { sfi_hint; sfi_optional = _; sfi_name = _ } ->
          hint env variance sfi_hint
        | SE_splat h -> hint env variance h);
    Option.iter nsi_unknown_fields_type ~f:(hint env variance)
  | Hrefinement (h, members) ->
    List.iter members ~f:(refinement_member env variance);
    hint env variance h
  | Hfun hfun ->
    let {
      hf_is_readonly = _;
      hf_tparams = _;
      hf_param_tys;
      hf_param_info;
      hf_variadic_ty;
      hf_named_variadic_ty;
      hf_return_ty;
      hf_ctxs = _;
      hf_is_readonly_return = _;
    } =
      hfun
    in
    List.iter2_exn hf_param_info hf_param_tys ~f:(hfun_param env variance);
    fun_arity env variance hf_variadic_ty;
    fun_arity env variance hf_named_variadic_ty;
    fun_ret env variance hf_return_ty
  | Happly (_, []) -> ()
  | Happly (name, hl) ->
    let variancel = get_class_variance env.Env.env (Ast_defs.get_id name) in
    iter2_shortest
      begin
        fun tparam_variance h ->
          let pos = Ast_defs.get_pos h in
          let reason =
            Rtype_argument (Utils.strip_ns @@ Ast_defs.get_id name)
          in
          let variance = compose (pos, reason) variance tparam_variance in
          hint env variance h
      end
      variancel
      hl

and hint_list env variance tyl = List.iter tyl ~f:(hint env variance)

and hfun_param env variance info h =
  let pos = Ast_defs.get_pos h in
  match Option.map info ~f:(fun x -> x.Aast.hfparam_kind) with
  | None
  | Some Ast_defs.Pnormal ->
    let reason = (pos, Rfun_parameter, Pcontravariant) in
    let variance = flip reason variance in
    hint env variance h
  | Some (Ast_defs.Pinout _) ->
    let variance = make_variance Rfun_inout_parameter pos Ast_defs.Invariant in
    hint env variance h

and fun_ret env variance h =
  let pos = Ast_defs.get_pos h in
  let reason_covariant = (pos, Rfun_return, Pcovariant) in
  let variance = detail reason_covariant variance in
  hint env variance h

and fun_arity env variance h =
  let empty_param_info = None in
  Option.iter h ~f:(hfun_param env variance empty_param_info)

and refinement_member env variance member =
  let check_exact h =
    let pos = Ast_defs.get_pos h in
    let reason = [(pos, Rrefinement_eq, Pinvariant)] in
    let var = Vinvariant (reason, reason) in
    hint env var h
  in
  let check_loose (lower, upper) =
    List.iter lower ~f:(fun h ->
        let pos = Ast_defs.get_pos h in
        let reason = (pos, Rrefinement_super, Pcontravariant) in
        let var = flip reason variance in
        hint env var h);
    List.iter upper ~f:(fun h ->
        let pos = Ast_defs.get_pos h in
        let reason = (pos, Rrefinement_as, Pcovariant) in
        let var = detail reason variance in
        hint env var h)
  in
  match member with
  | Aast.Rtype (_, ref) ->
    (match ref with
    | Aast.TRexact h -> check_exact h
    | Aast.TRloose { Aast.tr_lower = lb; tr_upper = ub } -> check_loose (lb, ub))
  | Aast.Rctx (_, ref) ->
    (match ref with
    | Aast.CRexact h -> check_exact h
    | Aast.CRloose { Aast.cr_lower = lb; cr_upper = ub } ->
      check_loose (Option.to_list lb, Option.to_list ub))

let fun_param : Env.t -> variance -> Nast.fun_param -> unit =
 fun env variance param ->
  let {
    Aast.param_type_hint = (_, h);
    param_pos = _;
    param_name = _;
    param_annotation = _;
    param_info = _;
    param_readonly = _;
    param_splat;
    param_callconv;
    param_named = _;
    param_user_attributes = _;
    param_visibility = _;
  } =
    param
  in
  Option.iter
    h
    ~f:
      (hfun_param
         env
         variance
         (Some
            Aast.
              {
                hfparam_kind = param_callconv;
                hfparam_readonlyness = None;
                hfparam_optional = None;
                hfparam_splat = param_splat;
                hfparam_named = None;
              }))

let fun_where_constraint : Env.t -> Aast.where_constraint_hint -> unit =
 fun env (h1, ck, h2) ->
  let pos1 = Ast_defs.get_pos h1 in
  let pos2 = Ast_defs.get_pos h2 in
  match ck with
  | Ast_defs.Constraint_super ->
    let var1 = Vcontravariant [(pos1, Rwhere_super, Pcontravariant)] in
    let var2 = Vcovariant [(pos2, Rwhere_super, Pcovariant)] in
    hint env var1 h1;
    hint env var2 h2
  | Ast_defs.Constraint_eq ->
    let reason1 = [(pos1, Rwhere_eq, Pinvariant)] in
    let reason2 = [(pos2, Rwhere_eq, Pinvariant)] in
    let var = Vinvariant (reason1, reason2) in
    hint env var h1;
    hint env var h2
  | Ast_defs.Constraint_as ->
    let var1 = Vcovariant [(pos1, Rwhere_as, Pcovariant)] in
    let var2 = Vcontravariant [(pos2, Rwhere_as, Pcontravariant)] in
    hint env var1 h1;
    hint env var2 h2

(* `as` constraints on method type parameters must be contravariant
 * and `super` constraints on method type parameters are covariant. To
 * see why, suppose that we allow the wrong variance:
 *
 *   class Foo<+T> {
 *     public function bar<Tu as T>(Tu $x) {}
 *   }
 *
 * Let A and B be classes, with B a subtype of A. Then
 *
 *   function f(Foo<A> $x) {
 *     $x->(new A());
 *   }
 *
 * typechecks. However, covariance means that we could call `f()` with an
 * instance of B. However, B::bar would expect its argument $x to be a subtype
 * of B, but we would be passing an instance of A to it, which should be a type
 * error. In other words, `as` constraints are upper type bounds, and since
 * subtypes must have more relaxed constraints than their supertypes, `as`
 * constraints must be contravariant. (Reversing this argument shows that
 * `super` constraints must be covariant.)
 *
 * The preceding discussion might lead one to think that the constraints should
 * have the same variance as the class parameter types, i.e. that `as`
 * constraints used in the return type should be covariant. In particular,
 * suppose
 *
 *   class Foo<-T> {
 *     public function bar<Tu as T>(Tu $x): Tu { ... }
 *     public function baz<Tu as T>(): Tu { ... }
 *   }
 *
 *   class A {}
 *   class B extends A {
 *     public function qux() {}
 *   }
 *
 *   function f(Foo<B> $x) {
 *     $x->baz()->qux();
 *   }
 *
 * Now `f($x)` could be a runtime error if `$x` was an instance of Foo<A>, and
 * it seems like we should enforce covariance on Tu. However, the real problem
 * is that constraints apply to _instantiation_, not usage. As far as Hack
 * knows, $x->bar() could be of any type, since we have not provided any clues
 * about how `Tu` should be instantiated. In fact `$x->bar()->whatever()`
 * succeeds as well, because `Tu` is of type Tany -- though in an ideal world
 * we would make it Tmixed in order to ensure soundness. Also, the signature
 * of `baz()` doesn't entirely make sense -- why constrain Tu if it it's only
 * getting used in one place?
 *
 * Thus, if one wants type safety, how `$x` _should_ be used is
 *
 *   function f(Foo<B> $x) {
 *     $x->bar(new B()))->qux();
 *   }
 *
 * Thus we can see that, if `$x` is used correctly (or if we enforced correct
 * use by returning Tmixed for uninstantiated type variables), we would always
 * know the exact type of Tu, and Tu can be validly used in both co- and
 * contravariant positions.
 *
 * Remark: This is far more intuitive if you think about how Vector::concat is
 * typed with its `Tu super T` type in both the parameter and return type
 * positions. Uninstantiated `super` types are less likely to cause confusion,
 * however -- you can't imagine doing very much with a returned value that is
 * some (unspecified) supertype of a class.
 *)
let tparam_constraint : Env.t -> Ast_defs.constraint_kind * Aast.hint -> unit =
 fun env (ck, h) ->
  let pos = Ast_defs.get_pos h in
  let var =
    match ck with
    | Ast_defs.Constraint_as ->
      Vcontravariant [(pos, Rconstraint_as, Pcontravariant)]
    | Ast_defs.Constraint_eq ->
      let reasons = [(pos, Rconstraint_eq, Pinvariant)] in
      Vinvariant (reasons, reasons)
    | Ast_defs.Constraint_super ->
      Vcovariant [(pos, Rconstraint_super, Pcovariant)]
  in
  hint env var h

let method_tparam : Env.t -> Nast.tparam -> Env.t =
 fun env tparam ->
  let {
    Aast.tp_variance = _;
    tp_name;
    tp_constraints;
    tp_reified = _;
    tp_user_attributes = _;
  } =
    tparam
  in
  List.iter tp_constraints ~f:(tparam_constraint env);
  let env = Env.remove_type_parameter env (Ast_defs.get_id tp_name) in
  env

let class_property : Env.t -> Nast.class_var -> unit =
 fun env prop ->
  Option.iter (snd prop.Aast.cv_type) ~f:(fun h ->
      if prop.Aast.cv_is_static then
        (* Static properties should not be generic at all. This is checked in
         * Typing_toplevel.check_no_generic_static_property on the class decl,
         * Since we need properties imported from traits as well. *)
        ()
      else
        match prop.Aast.cv_visibility with
        | Aast.Private
          when not
                 (Naming_attributes.mem
                    SN.UserAttributes.uaTestsBypassVisibility
                    prop.Aast.cv_user_attributes) ->
          ()
        | Aast.Private
        | Aast.Public
        | Aast.Protected
        | Aast.Internal
        | Aast.ProtectedInternal ->
          let variance =
            make_variance Rproperty (Ast_defs.get_pos h) Ast_defs.Invariant
          in
          hint env variance h)

let class_method : Env.t -> Nast.class_ -> Nast.method_ -> unit =
 fun env class_ method_ ->
  let {
    Aast.m_visibility;
    m_final;
    m_static;
    m_tparams;
    m_params;
    m_readonly_this = _;
    m_ret = (_, m_ret);
    m_name = (_, m_name);
    m_where_constraints;
    m_ctxs = _;
    m_unsafe_ctxs = _;
    m_span = _;
    m_annotation = _;
    m_abstract = _;
    m_body = _;
    m_fun_kind = _;
    m_user_attributes;
    m_readonly_ret = _;
    m_external = _;
    m_hidden = _;
    m_doc_comment = _;
  } =
    method_
  in
  if String.equal m_name SN.Members.__construct then
    ()
  else
    match m_visibility with
    | Aast.Private
      when not
             (Naming_attributes.mem
                SN.UserAttributes.uaTestsBypassVisibility
                m_user_attributes) ->
      ()
    | Aast.Private
    | Aast.Public
    | Aast.Protected
    | Aast.Internal
    | Aast.ProtectedInternal ->
      if (class_.Aast.c_final || m_final) && m_static then
        ()
      else
        let env = List.fold m_tparams ~f:method_tparam ~init:env in
        let variance = Vcovariant [] in
        List.iter m_params ~f:(fun_param env variance);
        Option.iter m_ret ~f:(fun_ret env variance);
        List.iter m_where_constraints ~f:(fun_where_constraint env);
        ()

(*****************************************************************************)
(* The entry point (for classes). *)
(*****************************************************************************)

let props_from_constructors : Nast.method_ list -> Nast.class_var list =
 fun methods ->
  let constructors =
    List.filter methods ~f:(fun m ->
        Ast_defs.get_id m.Aast.m_name |> String.equal SN.Members.__construct)
  in
  List.map constructors ~f:(fun m ->
      m.Aast.m_params
      |> List.filter_map ~f:(fun param ->
             let {
               Aast.param_type_hint;
               param_pos;
               param_name;
               param_annotation = ();
               param_readonly;
               param_info = _;
               param_splat = _;
               param_callconv = _;
               param_named = _;
               param_user_attributes;
               param_visibility;
             } =
               param
             in
             Option.(
               param_visibility >>| fun cv_visibility ->
               {
                 Aast.cv_final = false;
                 cv_xhp_attr = None;
                 cv_abstract = false;
                 cv_visibility;
                 cv_readonly = Option.is_some param_readonly;
                 cv_type = param_type_hint;
                 cv_id = (param_pos, param_name);
                 cv_expr = Aast_utils.get_param_default param;
                 cv_user_attributes = param_user_attributes;
                 cv_doc_comment = None;
                 cv_is_static = false;
                 cv_span = param_pos;
               })))
  |> List.concat

let class_def : Typing_env_types.env -> Nast.class_ -> unit =
 fun env class_ ->
  let {
    Aast.c_tparams;
    c_extends;
    c_uses;
    c_implements;
    c_vars;
    c_methods;
    c_final = _;
    c_typeconsts = (* TODO *) _;
    c_kind = _;
    c_span = _;
    c_annotation = _;
    c_mode = _;
    c_is_xhp = _;
    c_has_xhp_keyword = _;
    c_name = _;
    c_xhp_attr_uses = _;
    c_xhp_category = _;
    c_reqs = _;
    c_consts = _;
    c_xhp_children = _;
    c_xhp_attrs = _;
    c_namespace = _;
    c_user_attributes = _;
    c_file_attributes = _;
    c_module = _;
    c_enum = _;
    c_doc_comment = _;
    c_emit_id = _;
    c_internal = _;
    c_docs_url = _;
    c_package = _;
  } =
    class_
  in
  let env =
    {
      Env.tpenv =
        List.fold c_tparams ~init:S_map.empty ~f:(fun env tp ->
            S_map.add (snd tp.Aast.tp_name) (get_declared_variance tp) env);
      enclosing_class = Some class_;
      env;
    }
  in
  let parents = c_extends @ c_implements @ c_uses in
  List.iter parents ~f:(hint env Vboth);
  let c_vars = c_vars @ props_from_constructors c_methods in
  List.iter c_vars ~f:(class_property env);
  List.iter c_methods ~f:(class_method env class_);
  ()

(* in case type where clauses:
   `as` constraints are contravariant on the left and covariant on the right
   `super` constraints are covariant on the left and contravariant on the right
*)
let case_type_where_constraint : Env.t -> Aast.where_constraint_hint -> unit =
 fun env (h1, ck, h2) ->
  let pos1 = Ast_defs.get_pos h1 in
  let pos2 = Ast_defs.get_pos h2 in
  match ck with
  | Ast_defs.Constraint_super ->
    let var1 = Vcovariant [(pos1, Rcase_type_where_super, Pcovariant)] in
    let var2 =
      Vcontravariant [(pos2, Rcase_type_where_super, Pcontravariant)]
    in
    hint env var1 h1;
    hint env var2 h2
  | Ast_defs.Constraint_eq ->
    let reason1 = [(pos1, Rcase_type_where_eq, Pinvariant)] in
    let reason2 = [(pos2, Rcase_type_where_eq, Pinvariant)] in
    let var = Vinvariant (reason1, reason2) in
    hint env var h1;
    hint env var h2
  | Ast_defs.Constraint_as ->
    let var1 = Vcontravariant [(pos1, Rcase_type_where_as, Pcontravariant)] in
    let var2 = Vcovariant [(pos2, Rcase_type_where_as, Pcovariant)] in
    hint env var1 h1;
    hint env var2 h2

(*****************************************************************************)
(* The entry point (for typedefs). *)
(*****************************************************************************)
let typedef : Typing_env_types.env -> Nast.typedef -> unit =
 fun env typedef ->
  let {
    Aast.t_tparams;
    t_assignment;
    t_runtime_type = _;
    t_annotation = _;
    t_name = _;
    t_as_constraint = _;
    t_super_constraint = _;
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
  let (hints, where_constraints) =
    match t_assignment with
    | Aast.SimpleTypeDef { Aast.tvh_vis = _; Aast.tvh_hint } -> ([tvh_hint], [])
    | Aast.CaseType (variant, variants) ->
      let variants = variant :: variants in
      let hints = List.map variants ~f:(fun v -> v.Aast.tctv_hint) in
      let where_constraints =
        List.map variants ~f:(fun v -> v.Aast.tctv_where_constraints)
      in
      (hints, List.concat where_constraints)
  in
  let env =
    {
      Env.tpenv =
        List.fold t_tparams ~init:S_map.empty ~f:(fun env tp ->
            S_map.add (snd tp.Aast.tp_name) (get_declared_variance tp) env);
      enclosing_class = None;
      env;
    }
  in
  List.iter hints ~f:(fun t_kind ->
      let reason_covariant =
        [(Ast_defs.get_pos t_kind, Rtypedef, Pcovariant)]
      in
      hint env (Vcovariant reason_covariant) t_kind);
  List.iter where_constraints ~f:(case_type_where_constraint env)
