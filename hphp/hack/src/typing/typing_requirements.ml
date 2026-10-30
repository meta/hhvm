(*
 * Copyright (c) 2015, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude
module TUtils = Typing_utils
module Env = Typing_env
module Cls = Folded_class

(* Only applied to classes. Checks that all the requirements of the traits
 * and interfaces it uses are satisfied. *)
let check_fulfillment env class_pos tc (is_strict, (trait_pos, req_ty)) =
  match TUtils.try_unwrap_class_type req_ty with
  | None -> env
  | Some (_r, (_p, req_name), _paraml) ->
    let req_pos = Typing_defs.get_pos req_ty in
    if is_strict || not (String.equal req_name (Cls.name tc)) then (
      match Cls.get_ancestor tc req_name with
      | None ->
        (Typing_error_utils.add_typing_error ~env
        @@ Typing_error.(
             primary
             @@ Primary.Unsatisfied_req
                  { pos = class_pos; trait_pos; req_pos; req_name }));
        env
      | Some impl_ty ->
        let (env, ty_err_opt) =
          Typing_phase.sub_type_decl env impl_ty req_ty
          @@ Some
               (Typing_error.Reasons_callback.unsatisfied_req_callback
                  ~class_pos
                  ~trait_pos
                  ~req_pos
                  req_name)
        in
        Option.iter ~f:(Typing_error_utils.add_typing_error ~env) ty_err_opt;
        env
    ) else
      env

let check_require_class env class_pos tc (trait_pos, req_ty) =
  match TUtils.try_unwrap_class_type req_ty with
  | None -> env
  | Some (_r, (_p, req_name), _paraml) ->
    (* in a `require class t;` trait constraint, t must be a non-generic class
     * name.  Since lowering enforces _paraml to be empty, it is safe to
     * ignore _param here.  Additionally we enforce that the class that uses
     * the trait with a require class constraint does not have type parameters.
     *)
    if
      not (String.equal req_name (Cls.name tc) && List.is_empty (Cls.tparams tc))
    then (
      let req_pos = Typing_defs.get_pos req_ty in
      (Typing_error_utils.add_typing_error ~env
      @@ Typing_error.(
           primary
           @@ Primary.Unsatisfied_req_class
                { pos = class_pos; trait_pos; req_pos; req_name }));
      env
    ) else
      env

(* HHVM enformcement rejects conflicting require class and require extends
    constraints on the same class, eg:

   trait T1 { require extends C; }
   trait T2 { require class C; }
   trait T { use T1, T2; }

   The check below, only run on traits, detects and rejects such cases,
   ensuring that Hack does not accept code on which HHVM enforcement
   raises an error.
*)

let get_require_class_name_pos ty =
  match TUtils.try_unwrap_class_type ty with
  | Some (_, (p, n), _) -> Some (n, p)
  | None -> None

let check_require_class_require_extends_conflict
    env trait_pos required_classes tc =
  let require_extends_implements =
    List.filter_map (Cls.all_ancestor_reqs tc) ~f:(fun (_, ty) ->
        get_require_class_name_pos ty)
  in
  List.iter required_classes ~f:(fun (nc, pc) ->
      match
        List.find require_extends_implements ~f:(fun (ne, _) ->
            String.equal nc ne)
      with
      | None -> ()
      | Some (_, pe) ->
        Typing_error_utils.add_typing_error ~env
        @@ Typing_error.(
             primary
             @@ Primary.Incompatible_reqs
                  {
                    pos = trait_pos;
                    req_name = nc;
                    req_class_pos = pc;
                    req_extends_pos = pe;
                  }));
  env

(* If a trait `T` requires `C`, but `C` does not use `T`, then Hack won't report any
 * type error on the body of `T` because the intersection of `C` and `T` is empty.
 * Although this behaviour is sound, it is likely to be confusing for the programmer.
 * We thus eagerly emit an error if a trait requires a class that does not use it.
 *)
let check_require_class_use env trait_pos required_classes tc =
  List.iter required_classes ~f:(fun (req_constraint_kind, (nc, pc)) ->
      match Env.get_class env nc with
      | Decl_entry.DoesNotExist
      | Decl_entry.NotYetAvailable ->
        (* existence of class is checked in Naming *)
        ()
      | Decl_entry.Found trc ->
        if
          (not (Cls.has_ancestor trc (Cls.name tc)))
          && Ast_defs.is_c_class (Cls.kind trc)
        then
          Typing_error_utils.add_typing_error ~env
          @@ Typing_error.(
               primary
               @@ Primary.Trait_not_used
                    {
                      pos = trait_pos;
                      trait_name = Cls.name tc;
                      req_class_pos = pc;
                      class_pos = Cls.pos trc;
                      class_name = Cls.name trc;
                      req_constraint_kind;
                    }))

let check_trait_require_class env trait_pos tc =
  let (required_classes, required_equal_classes) =
    List.fold_right
      (Cls.all_ancestor_req_constraints_requirements tc)
      ~init:([], [])
      ~f:(fun rc (required_classes, required_equal_classes) ->
        let (req_constraint_kind, (_, ty)) =
          match rc with
          | CR_Equal req -> (`class_, req)
          | CR_Subtype req -> (`this_as, req)
        in
        match get_require_class_name_pos ty with
        | None -> (required_classes, required_equal_classes)
        | Some required_class ->
          ( (req_constraint_kind, required_class) :: required_classes,
            (match req_constraint_kind with
            | `class_ -> required_class :: required_equal_classes
            | `this_as -> required_equal_classes) ))
  in
  check_require_class_use env trait_pos required_classes tc;
  check_require_class_require_extends_conflict
    env
    trait_pos
    required_equal_classes
    tc

(** Check whether a class satifies all the requirements of the traits it uses,
    namely [require extends] and [require implements]. *)
let check_class env class_pos tc =
  match Cls.kind tc with
  | Ast_defs.Cclass _ ->
    let env =
      List.fold
        (List.map
           ~f:(fun r -> (false, r))
           (Cls.all_ancestor_req_this_as_requirements tc)
        @ List.map ~f:(fun r -> (true, r)) (Cls.all_ancestor_reqs tc))
        ~f:(fun env req -> check_fulfillment env class_pos tc req)
        ~init:env
    in
    List.fold
      (Cls.all_ancestor_req_class_requirements tc)
      ~f:(fun env req -> check_require_class env class_pos tc req)
      ~init:env
  | Ast_defs.Ctrait -> check_trait_require_class env class_pos tc
  | Ast_defs.Cinterface
  | Ast_defs.Cenum_class _
  | Ast_defs.Cenum ->
    env
