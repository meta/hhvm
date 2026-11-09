(*
 * Copyright (c) 2015, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

module TPEnv = Type_parameter_env
module KDefs = Typing_kinding_defs
module TySet = Typing_set
module Env = Typing_env
open Hh_prelude
open Common
open Typing_defs

let intersect_upper_bounds env r ul =
  Typing_intersection.intersect_list env r (TySet.elements ul)

let union_lower_bounds env r ll =
  Typing_union.union_list env r (TySet.elements ll)

let join_lower_bounds env l1 l2 =
  (* Special case: subset inclusion. Return subset
   * (e.g. if t|u <: T or t <: T then t <: T ) *)
  if TySet.subset l1 l2 then
    (env, l1)
  else if TySet.subset l2 l1 then
    (env, l2)
  else
    (* Convert upper bounds to equivalent union *)
    let (env, union1) = union_lower_bounds env Reason.none l1 in
    let (env, union2) = union_lower_bounds env Reason.none l2 in
    let (env, new_lower) =
      Typing_intersection.intersect env ~r:Reason.none union1 union2
    in
    (env, TySet.singleton new_lower)

let join_upper_bounds env u1 u2 =
  (* Special case: subset inclusion. Return subset
   * (e.g. if T <: t & u, or T <: t, then T <: t) *)
  if TySet.subset u1 u2 then
    (env, u1)
  else if TySet.subset u2 u1 then
    (env, u2)
  else
    (* Convert upper bounds to equivalent intersection *)
    let (env, inter1) = intersect_upper_bounds env Reason.none u1 in
    let (env, inter2) = intersect_upper_bounds env Reason.none u2 in
    let (env, new_upper) = Typing_union.union env inter1 inter2 in
    (env, TySet.singleton new_upper)

(* Merge two type parameter environments. Given tpenv1 and tpenv2 we want
 * to compute a "merged" environment tpenv such that
 *     tpenv1 |- tpenv
 * and tpenv2 |- tpenv
 *
 * A type parameter defined on only one input is kept: it may be a fresh generic
 * the other input never had. Under forget_one_branch_tconst_refinements, one in
 * the global tpenv (a type constant) is dropped, leaving its global bounds.
 * If it appears in both, supposing we have
 *     l1 <: T <: u1 in tpenv1
 * and l2 <: T <: u2 in tpenv2
 * with multiple lower bounds reduced to a union, and multiple upper bounds
 * reduced to an intersection, then the resulting tpenv will have
 *     l1&l2 <: T <: u1|u2
 *)
let join env tpenv1 tpenv2 =
  let merge_pos p1 p2 =
    if Pos_or_decl.equal p1 Pos_or_decl.none then
      p2
    else
      p1
  in
  let forget_one_sided_global =
    Typechecker_options.forget_one_branch_tconst_refinements (Env.get_tcopt env)
  in
  let global_tpenv = Env.get_global_tpenv env in
  TPEnv.merge_env env tpenv1 tpenv2 ~combine:(fun env tparam info1 info2 ->
      match (info1, info2) with
      | ( Some
            (pos1, (KDefs.{ lower_bounds = l1; upper_bounds = u1; _ } as info1)),
          Some (pos2, KDefs.{ lower_bounds = l2; upper_bounds = u2; _ }) ) ->
        let (env, lower_bounds) = join_lower_bounds env l1 l2 in
        let (env, upper_bounds) = join_upper_bounds env u1 u2 in
        let pos = merge_pos pos1 pos2 in
        (env, Some (pos, KDefs.{ info1 with lower_bounds; upper_bounds }))
      | (Some _, None)
      | (None, Some _)
        when forget_one_sided_global && TPEnv.mem tparam global_tpenv ->
        (env, None)
      | (Some (pos, info), _) -> (env, Some (pos, info))
      | (_, Some (pos, info)) -> (env, Some (pos, info))
      | (_, _) -> (env, None))

let get_tpenv_equal_bounds env name =
  let lower = TPEnv.get_lower_bounds env name in
  let upper = TPEnv.get_upper_bounds env name in
  TySet.inter lower upper

(** Given a list of type parameter names, attempt to simplify away those
type parameters by looking for a type to which they are equal in the tpenv.
If such a type exists, remove the type parameter from the tpenv.
Returns a set of substitutions mapping each type parameter name to the type
to which it is equal if found, otherwise to itself. *)
let simplify_tpenv env (tparams : ((_ * string) option * locl_ty) list) r =
  let old_env = env in
  let tpenv = Env.get_tpenv env in
  (* For each tparam, "solve" it if it falls in any of those categories:
   *   - there exists a type ty to which it is equal
   *   - it is covariant and has upper bound ty (or mixed if absent)
   *   - it is contravariant and lower bound ty (or nothing if absent).
   * In which case remove tparam from tpenv and add substitution
   * (tparam -> ty) to substs. *)
  let (env, tpenv, substs) =
    List.fold
      tparams
      ~init:(env, tpenv, S_map.empty)
      ~f:(fun (env, tpenv, substs) (p_opt, ty) ->
        let reason = get_reason ty in
        match p_opt with
        | None -> (env, tpenv, substs)
        | Some (tp, tparam_name) ->
          let equal_bounds = get_tpenv_equal_bounds tpenv tparam_name in
          let lower_bounds = TPEnv.get_lower_bounds tpenv tparam_name in
          let upper_bounds = TPEnv.get_upper_bounds tpenv tparam_name in
          let (env, lower_bound) = union_lower_bounds env reason lower_bounds in
          let (env, upper_bound) =
            intersect_upper_bounds env reason upper_bounds
          in
          (* remove tparam_name from tpenv, and in any lower/upper bound set
           * where it occurs.
           * We don't need to do any merging of lower/upper bounds,
           * because all its lower/upper bounds
           * are already lower/upper bounds of `bound` (and other bounds)
           * thanks to the transitive closure we've done in
           * Typing_subtype.add_constraint. *)
          let (tpenv, substs) =
            match (tp.tp_variance, TySet.choose_opt equal_bounds) with
            | (_, Some bound) ->
              let tpenv = TPEnv.remove tpenv tparam_name in
              let substs = S_map.add tparam_name bound substs in
              (tpenv, substs)
            | (Ast_defs.Covariant, _) ->
              let tpenv = TPEnv.remove tpenv tparam_name in
              let substs = S_map.add tparam_name upper_bound substs in
              (tpenv, substs)
            | (Ast_defs.Contravariant, _) ->
              let tpenv = TPEnv.remove tpenv tparam_name in
              let substs = S_map.add tparam_name lower_bound substs in
              (tpenv, substs)
            | _ ->
              (* TODO see comment at beginning of function *)
              let tparam_ty = mk (r, Tgeneric tparam_name) in
              let substs = S_map.add tparam_name tparam_ty substs in
              (tpenv, substs)
          in
          (env, tpenv, substs))
  in
  let env = Env.env_with_tpenv env tpenv in
  (* reduce the set of substitutions. For example, for a set of substitutions
   * like
   *   Ta -> Tb
   *   Tb -> int
   * simplify to:
   *   Ta -> int
   *   Tb -> int
   *)
  let rec reduce substs tparam =
    match S_map.find_opt tparam substs with
    | None -> (substs, None)
    | Some subst ->
      (match get_node subst with
      | Tgeneric tparam' when String.( <> ) tparam' tparam ->
        (* TODO see comment at beginning of function *)
        let (substs, new_subst_opt) = reduce substs tparam' in
        begin
          match new_subst_opt with
          | None -> (substs, Some subst)
          | Some new_subst ->
            let substs = S_map.add tparam new_subst substs in
            (substs, Some new_subst)
        end
      | _ -> (substs, Some subst))
  in
  let reduce substs (p_opt, _) =
    match p_opt with
    | None -> substs
    | Some (_, name) -> fst (reduce substs name)
  in
  let substs = List.fold tparams ~init:substs ~f:reduce in
  (* Apply substitution to bounds on type parameters that remain *)
  let combine_reasons ~src:_ ~dest = dest in
  let tpenv =
    TPEnv.map
      (fun ty ->
        Typing_defs_core.Locl_subst.apply ty ~subst:substs ~combine_reasons)
      tpenv
  in
  let env = Env.env_with_tpenv env tpenv in
  let env = Env.log_env_change "simplify_tpenv" old_env env in
  (env, substs)
