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
open Common
module Env = Typing_env
module MakeType = Typing_make_type
module SN = Naming_special_names

let rec partition_union ~f env tyl =
  (* Tail-recursive: unions can have very many members, so recursing once per
     member can overflow the stack. *)
  let rec aux ((dyns, nondyns) as acc) tyl =
    match tyl with
    | [] -> acc
    | t :: tyl ->
      let (_, t) = Env.expand_type env t in
      (match get_node t with
      | Tunion tyl' when not (List.is_empty tyl') -> aux acc (tyl' @ tyl)
      | _ ->
        let acc =
          match get_node t with
          | Tnewtype (n, [ty1], _) when String.equal n SN.Classes.cSupportDyn ->
            (match strip_union ~f env [ty1] with
            | None -> (dyns, t :: nondyns)
            | Some (sub_dyns, sub_non_dyns) ->
              let sd tys =
                MakeType.supportdyn
                  (get_reason ty1)
                  (MakeType.union (get_reason ty1) tys)
              in
              (sd sub_dyns :: dyns, sd sub_non_dyns :: nondyns))
          | _ ->
            if f t then
              (t :: dyns, nondyns)
            else
              (dyns, t :: nondyns)
        in
        aux acc tyl)
  in
  let (dyns, nondyns) = aux ([], []) tyl in
  (List.rev dyns, List.rev nondyns)

and strip_union ~f env tyl =
  let (dyns, nondyns) = partition_union ~f env tyl in
  match (dyns, nondyns) with
  | ([], _) -> None
  | (_, _) -> Some (dyns, nondyns)

let rec is_dynamic_or_intersection_with_dynamic ~accept_intersections env ty =
  let (_, ty) = Env.expand_type env ty in
  match get_node ty with
  | Tintersection tyl when accept_intersections ->
    List.exists
      tyl
      ~f:(is_dynamic_or_intersection_with_dynamic ~accept_intersections env)
  | Tdynamic None -> true
  | _ -> false

let rec try_strip_dynamic_from_union ?(accept_intersections = false) env tyl =
  match
    strip_union
      ~f:(is_dynamic_or_intersection_with_dynamic ~accept_intersections env)
      env
      tyl
  with
  | Some (ty :: _, tyl) -> Some (ty, tyl)
  | _ -> None

(* Strip dynamic off a union, and push supportdyn through. So
 *   try_strip_dynamic(~t) = t
 *   try_strip_dynamic(supportdyn<~t>) = supportdyn<t>
 * Otherwise return None.
 *)
and try_strip_dynamic
    ?(accept_intersections = false) ?(do_not_solve_likes = false) env ty =
  let (env, ty) =
    if do_not_solve_likes then
      Env.expand_type env ty
    else
      Typing_solver.expand_type_for_strip_dynamic env ty
  in
  match get_node ty with
  | Tnewtype (name, [tyarg], _) when String.equal name SN.Classes.cSupportDyn ->
    let (env, ty_opt) = try_strip_dynamic ~accept_intersections env tyarg in
    begin
      match ty_opt with
      | None -> (env, None)
      | Some stripped_ty ->
        (env, Some (MakeType.supportdyn (get_reason ty) stripped_ty))
    end
  | Tunion tyl ->
    let ty_opt = try_strip_dynamic_from_union ~accept_intersections env tyl in
    (match ty_opt with
    | None -> (env, None)
    | Some (_, tyl) -> (env, Some (MakeType.union (get_reason ty) tyl)))
  | _ -> (env, None)

and strip_dynamic env ty =
  let (env, ty_opt) = try_strip_dynamic env ty in
  ( env,
    match ty_opt with
    | None -> ty
    | Some ty -> ty )

let is_inter_dyn env ty =
  let (env, ty) = Env.expand_type env ty in
  match get_node ty with
  | Tintersection [ty1; ty2]
    when Typing_defs.is_dynamic ty1 || Typing_defs.is_dynamic ty2 ->
    (env, Some (ty1, ty2))
  | _ -> (env, None)

let rec find_inter_dyn env acc tyl =
  match tyl with
  | [] -> (env, None)
  | ty :: tyl ->
    (match is_inter_dyn env ty with
    | (env, None) -> find_inter_dyn env (ty :: acc) tyl
    | (env, Some ty_inter) -> (env, Some (ty_inter, acc @ tyl)))

let rec recompose_like_type env orig_ty =
  let (env, ty) = Env.expand_type env orig_ty in
  match get_node ty with
  | Tunion tys ->
    (match find_inter_dyn env [] tys with
    | (env, Some ((ty1, ty2), [ty_sub])) ->
      let (env, ty_union1) = Typing_utils.union env ty1 ty_sub in
      let (env, ty_union2) = Typing_utils.union env ty2 ty_sub in
      (env, MakeType.intersection (get_reason ty) [ty_union1; ty_union2])
    | (env, _) ->
      let (env, ty_opt) = try_strip_dynamic env ty in
      (match ty_opt with
      | None -> (env, ty)
      | Some ty1 ->
        let (env, ty2) = recompose_like_type env ty1 in
        (env, MakeType.locl_like (get_reason ty) ty2)))
  | Tfun ft ->
    let (env, ft_ret) = recompose_like_type env ft.ft_ret in
    (env, mk (get_reason ty, Tfun { ft with ft_ret }))
  | Tnewtype (n, [ty1], _) when String.equal n SN.Classes.cSupportDyn ->
    let (env, ty1) = recompose_like_type env ty1 in
    Typing_utils.simple_make_supportdyn (get_reason ty) env ty1
  | _ -> (env, orig_ty)
