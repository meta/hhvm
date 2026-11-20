(*
 * Copyright (c) 2018, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude
open Typing_defs
module Env = Typing_env
module SN = Naming_special_names
module UA = SN.UserAttributes
module Cls = Folded_class
module Nast = Aast

let validator =
  object (this)
    inherit Type_validator.type_validator as super

    method! on_tapply acc r (p, h) tyl =
      if String.equal h SN.Classes.cTypename then
        this#invalid acc r "a typename"
      else if String.equal h SN.Classes.cClassname then
        this#invalid acc r "a classname"
      else
        super#on_tapply acc r (p, h) tyl

    method! on_tclass_ptr acc r _ty =
      (* TODO(T199611023) allow when we enforce inner type *)
      this#invalid acc r "a class pointer type"

    method! on_twildcard acc r =
      if acc.Type_validator.env.Typing_env_types.allow_wildcards then
        acc
      else
        this#invalid acc r "a wildcard"

    method! on_tgeneric acc r t =
      match Env.get_reified acc.Type_validator.env t with
      | Nast.Erased -> this#invalid acc r "not reified"
      | Nast.SoftReified -> this#invalid acc r "soft reified"
      | Nast.Reified -> acc

    method! on_tfun acc r _ = this#invalid acc r "a function type"

    method! on_typeconst acc _ typeconst =
      let open Type_validator in
      match acc.reification with
      | TypeStructure -> acc
      | Unresolved
      | Resolved ->
        (match typeconst.ttc_kind with
        | TCConcrete { tc_type } -> this#on_type acc tc_type
        | TCAbstract
            { atc_as_constraint = _; atc_super_constraint = _; atc_default } ->
          (match typeconst.ttc_reifiable with
          | None ->
            let r = Reason.witness_from_decl (fst typeconst.ttc_name) in
            let kind =
              "an abstract type constant without the __Reifiable attribute"
            in
            this#invalid acc r kind
          | Some _ -> Option.fold ~f:this#on_type ~init:acc atc_default))

    method! on_taccess acc r (root, ids) =
      let acc =
        match acc.Type_validator.reification with
        | Type_validator.Unresolved ->
          (match get_node root with
          | Tthis ->
            { acc with Type_validator.reification = Type_validator.Resolved }
          | _ -> this#on_type acc root)
        | Type_validator.TypeStructure
        | Type_validator.Resolved ->
          acc
      in
      super#on_taccess acc r (root, ids)

    method! on_tthis acc r =
      match acc.class_from_taccess_lhs with
      | Some final_class when Folded_class.final final_class ->
        (* in a final class, `this` is the lexical class, since there are no subclasses *)
        let has_tparams =
          not @@ List.is_empty @@ Folded_class.tparams final_class
        in
        if has_tparams then
          (* Ban because:
           *  `final class C<T> { type Tc = this }` is a way of expressing the same thing as
           *  `class C<T> { type Tc = C<T> }` which is banned (probably due to type constants being reified)
           *)
          this#invalid
            acc
            r
            "the late static bound `this` type of a class with type parameters"
        else
          acc
      | Some _non_final_class ->
        this#invalid
          acc
          r
          "the late static bound `this` type of a non-final class"
      | None -> this#invalid acc r "the late static bound `this` type"

    method! on_trefinement acc r _ty _ = this#invalid acc r "type refinement"
  end

(* A splat operand HHVM merges field by field. Anything that may resolve to a
   union at runtime (case types, type parameters, opaque newtypes) is instead
   distributed into a union of shapes. *)
let is_mergeable_splat_operand env ty =
  let (_sd, env, ty) = Typing_utils.strip_supportdyn env ty in
  let (_env, ty) = Env.expand_type env ty in
  match get_node ty with
  | Tshape _
  | Tdynamic _
  | Tunion [] ->
    true
  | _ -> false

(* HHVM cannot yet compare unions that lack a case type witness when checking
   reified generics. Shape splats merge into such unions at runtime when an
   optional field overlaps another operand's field, or when an operand is
   distributed. Case types are compared by witness, so only their type
   arguments matter. *)
let union_validator =
  object (this)
    inherit Type_validator.type_validator as super

    method! on_tunion acc r tyl =
      if List.is_empty tyl then
        acc
      else
        this#invalid acc r "a union type"

    method! on_tshape acc r shape_ty =
      match shape_ty with
      | Shape_splat { ss_elems } ->
        let ety_env =
          { acc.Type_validator.ety_env with under_type_constructor = true }
        in
        let (env, elems) =
          List.fold_map
            ss_elems
            ~init:acc.Type_validator.env
            ~f:(fun env elem ->
              let ((env, _ty_err_opt), elem) =
                Typing_phase.localize ~ety_env env elem
              in
              (env, elem))
        in
        let acc =
          if not (List.for_all elems ~f:(is_mergeable_splat_operand env)) then
            this#invalid acc r "a shape splat of a type that may be a union"
          else if
            not (List.is_empty (Typing_shape_disjointness.violations elems env))
          then
            this#invalid acc r "a shape splat whose elements may overlap"
          else
            acc
        in
        super#on_tshape acc r shape_ty
      | Shape_simple _ -> super#on_tshape acc r shape_ty

    method! on_newtype acc _ _ tyl _ _ ty =
      List.fold_left (ty :: tyl) ~f:this#on_type ~init:acc

    method! on_tapply acc r (p, h) tyl =
      match Env.get_class_or_typedef acc.Type_validator.env h with
      | Decl_entry.Found
          (Env.TypedefResult { td_type_assignment = CaseType _; _ }) ->
        List.fold_left tyl ~f:this#on_type ~init:acc
      | _ -> super#on_tapply acc r (p, h) tyl
  end
