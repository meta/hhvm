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
module Subst = Decl_subst

let make_subst tparams tyl = Subst.make_decl tparams tyl

let get_tparams_in_ty_and_acc acc ty =
  let tparams_visitor =
    object
      inherit [S_set.t] Type_visitor.decl_type_visitor

      method! on_tgeneric acc _ s = S_set.add s acc
    end
  in
  tparams_visitor#on_type acc ty

let get_tparams_in_subst subst =
  S_map.fold
    (fun _ ty acc -> get_tparams_in_ty_and_acc acc ty)
    subst
    S_set.empty

(*****************************************************************************)
(* Code dealing with instantiation. *)
(*****************************************************************************)

let rec instantiate subst (ty : decl_ty) =
  (* PERF: If subst is empty then instantiation is a no-op. We can save a
   * significant amount of CPU by avoiding recursively deconstructing the ty
   * data type.
   *)
  if S_map.is_empty subst then
    ty
  else
    match deref ty with
    | (r, Tgeneric x) ->
      (match S_map.find_opt x subst with
      | Some found_ty ->
        let (found_r, found_ty_) = deref found_ty in
        let new_r = Reason.instantiate ~type_:found_r x ~var:r in
        mk (new_r, found_ty_)
      | None -> mk (r, Tgeneric x))
    | (r, ty) ->
      let ty = instantiate_ subst ty in
      mk (r, ty)

and instantiate_ subst x =
  match x with
  | Tgeneric _ -> assert false
  (* IMPORTANT: We cannot expand Taccess during instantiation because this can
   * be called before all type consts have been declared and inherited
   *)
  | Taccess (ty, id) ->
    let ty = instantiate subst ty in
    Taccess (ty, id)
  | Trefinement (ty, rs) ->
    let ty = instantiate subst ty in
    let rs = Class_refinement.map (instantiate subst) rs in
    Trefinement (ty, rs)
  | Tvec_or_dict (ty1, ty2) ->
    let ty1 = instantiate subst ty1 in
    let ty2 = instantiate subst ty2 in
    Tvec_or_dict (ty1, ty2)
  | (Tthis | Tmixed | Twildcard | Tdynamic _ | Tnonnull | Tany _ | Tprim _) as x
    ->
    x
  | Ttuple { t_required; t_optional; t_extra } ->
    let t_required = List.map t_required ~f:(instantiate subst) in
    let t_optional = List.map t_optional ~f:(instantiate subst) in
    let t_extra = instantiate_tuple_extra subst t_extra in

    Ttuple { t_required; t_optional; t_extra }
  | Tunion tyl ->
    let tyl = List.map tyl ~f:(instantiate subst) in
    Tunion tyl
  | Tintersection tyl ->
    let tyl = List.map tyl ~f:(instantiate subst) in
    Tintersection tyl
  | Toption ty ->
    let ty = instantiate subst ty in
    (* we want to avoid double option: ??T *)
    (match get_node ty with
    | Toption _ as ty_node -> ty_node
    | _ -> Toption ty)
  | Tlike ty -> Tlike (instantiate subst ty)
  | Tfun ft ->
    let tparams = ft.ft_tparams in
    (* First remove shadowed type parameters from the substitution.
     * For example, for
     *   class C<T> { public function foo<T>(T $x):void }
     *   class B extends C<int> { }
     * we do not want to replace T by int in foo's signature.
     *)
    let subst =
      List.fold_left
        ~f:
          begin
            (fun subst t -> S_map.remove (snd t.tp_name) subst)
          end
        ~init:subst
        tparams
    in
    (* Now collect up all generic parameters that appear in the target of the substitution *)
    let target_generics = get_tparams_in_subst subst in
    (* For generic parameters in the function type, rename them "away" from the parameters
     * that appear in the target of the substitituion, and extend the substitution with
     * the renaming.
     *
     * For example, consider
     *    class Base<T> { public function cast<TF>(T $x):TF; }
     *    class Derived<TF> extends Base<TF> { }
     * Now we start with a substitution T:=TF
     * But when going "under" the generic parameter to the cast method,
     * we must rename it to avoid capture: T:=TF, TF:=TF#0
     * and so we end up with
     *   function cast<TF#0>(TF $x):TF#0
     *)
    let (subst, tparams) =
      List.fold_map ft.ft_tparams ~init:subst ~f:(fun subst tp ->
          let (pos, name) = tp.tp_name in
          if S_set.mem name target_generics then
            (* Fresh only because we don't support nesting of generic function types *)
            let fresh_tp_name = name ^ "#0" in
            let reason = Typing_reason.witness_from_decl pos in
            ( S_map.add name (mk (reason, Tgeneric fresh_tp_name)) subst,
              { tp with tp_name = (pos, fresh_tp_name) } )
          else
            (subst, tp))
    in
    let params =
      List.map ft.ft_params ~f:(fun param ->
          let ty = instantiate subst param.fp_type in
          { param with fp_type = ty })
    in
    let ret = instantiate subst ft.ft_ret in
    let tparams =
      List.map tparams ~f:(fun t ->
          {
            t with
            tp_constraints =
              List.map t.tp_constraints ~f:(fun (ck, ty) ->
                  (ck, instantiate subst ty));
          })
    in
    let where_constraints =
      List.map ft.ft_where_constraints ~f:(fun (ty1, ck, ty2) ->
          (instantiate subst ty1, ck, instantiate subst ty2))
    in
    Tfun
      {
        ft with
        ft_params = params;
        ft_ret = ret;
        ft_tparams = tparams;
        ft_where_constraints = where_constraints;
      }
  | Tapply (x, tyl) ->
    let tyl = List.map tyl ~f:(instantiate subst) in
    Tapply (x, tyl)
  | Tshape
      (Shape_simple
        { s_origin = _; s_unknown_value = shape_kind; s_fields = fdm }) ->
    let fdm = ShapeFieldMap.map (instantiate subst) fdm in
    (* TODO(shapes) Should this be changing s_origin? *)
    Tshape
      (Shape_simple
         {
           s_origin = Missing_origin;
           s_unknown_value = instantiate subst shape_kind;
           s_fields = fdm;
         })
  | Tshape (Shape_splat { ss_elems }) ->
    Tshape (Shape_splat { ss_elems = List.map ss_elems ~f:(instantiate subst) })
  | Tclass_ptr ty -> Tclass_ptr (instantiate subst ty)

and instantiate_tuple_extra subst e =
  match e with
  | Tsplat t_splat -> Tsplat (instantiate subst t_splat)
  | Tvariadic t_variadic -> Tvariadic (instantiate subst t_variadic)

let instantiate_ce subst ({ ce_type = x; _ } as ce) =
  { ce with ce_type = lazy (instantiate subst (Lazy.force x)) }

let instantiate_cc subst ({ cc_type = x; _ } as cc) =
  let x = instantiate subst x in
  { cc with cc_type = x }

(* TODO(T88552052) is this necessary? Type consts are not allowed to
   reference type parameters, which is the substitution which is happening here *)
let instantiate_typeconst subst = function
  | TCAbstract
      { atc_as_constraint = a; atc_super_constraint = s; atc_default = d } ->
    TCAbstract
      {
        atc_as_constraint = Option.map a ~f:(instantiate subst);
        atc_super_constraint = Option.map s ~f:(instantiate subst);
        atc_default = Option.map d ~f:(instantiate subst);
      }
  | TCConcrete { tc_type = t } -> TCConcrete { tc_type = instantiate subst t }

let instantiate_typeconst_type subst tc =
  { tc with ttc_kind = instantiate_typeconst subst tc.ttc_kind }
