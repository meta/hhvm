(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude
open Aast
open Typing_defs
module Cls = Folded_class

(* Private members skip variance checks, but they can be reached through
 * receivers other than `$this`, whose type arguments may have been widened
 * (covariant) or narrowed (contravariant). Such a use must respect variance as
 * a public member would: a write needs the property type to be contravariant
 * in the class's variant type parameters, and a call needs the method type to
 * be covariant in them. *)

type use =
  | Write
  | Call

(* The first variant type parameter of [cls] that [ty] uses against its
 * variance for [use], with the position of an offending occurrence. *)
let violation env cls use (ty : decl_ty) =
  let variant =
    List.filter_map (Cls.tparams cls) ~f:(fun tp ->
        match tp.tp_variance with
        | Ast_defs.Invariant -> None
        | (Ast_defs.Covariant | Ast_defs.Contravariant) as variance ->
          Some (snd tp.tp_name, variance))
  in
  let (positive, negative) =
    Typing_variance.get_positive_negative_generics
      ~tracked:(S_set.of_list (List.map variant ~f:fst))
      ~is_mutable:false
      env
      (S_map.empty, S_map.empty)
      ty
  in
  List.find_map variant ~f:(fun (name, variance) ->
      let offending =
        match (use, variance) with
        | (Write, Ast_defs.Covariant)
        | (Call, Ast_defs.Contravariant) ->
          positive
        | (Write, Ast_defs.Contravariant)
        | (Call, Ast_defs.Covariant)
        | (_, Ast_defs.Invariant) ->
          negative
      in
      match S_map.find_opt name offending with
      | Some (r :: _) -> Some (name, variance, Typing_reason.to_pos r)
      | Some []
      | None ->
        None)

(* The class of a receiver that is not `$this`, if its private members could be
 * reached with widened or narrowed type arguments. *)
let receiver_class env (recv : Tast.expr) =
  match recv with
  | (_, _, This) -> None
  | (ty, _, _) ->
    let (env, ty) = Tast_env.expand_type env ty in
    let (env, ty) =
      match get_node ty with
      | Toption ty -> Tast_env.expand_type env ty
      | _ -> (env, ty)
    in
    (match get_node ty with
    | Tclass ((_, name), _, _ :: _) ->
      (match Tast_env.get_class env name |> Decl_entry.to_option with
      | Some cls
        when List.exists (Cls.tparams cls) ~f:(fun tp ->
                 not (Ast_defs.equal_variance tp.tp_variance Ast_defs.Invariant))
        ->
        Some cls
      | _ -> None)
    | _ -> None)

let private_member
    (env : Tast_env.env)
    recv
    (get_member : Typing_env_types.env -> Cls.t -> string -> class_elt option)
    member_name =
  Option.bind (receiver_class env recv) ~f:(fun cls ->
      let Equal = Tast_env.eq_typing_env in
      match get_member env cls member_name with
      | Some ({ ce_visibility = Vprivate _; _ } as ce) ->
        Some (cls, Lazy.force ce.ce_type)
      | _ -> None)

let check (env : Tast_env.env) pos cls member_name use ty =
  let Equal = Tast_env.eq_typing_env in
  Option.iter
    (violation env cls use ty)
    ~f:(fun (tparam_name, variance, use_pos) ->
      Typing_error_utils.add_typing_error
        ~env
        Typing_error.(
          primary
          @@ Primary.Private_member_variance
               {
                 pos;
                 class_name = Cls.name cls;
                 member_name;
                 is_write =
                   (match use with
                   | Write -> true
                   | Call -> false);
                 tparam_name;
                 variance;
                 use_pos;
                 decl_pos = Cls.pos cls;
               }))

let check_write env (recv : Tast.expr) pos prop =
  Option.iter
    (private_member env recv Typing_env.get_prop prop)
    ~f:(fun (cls, ty) -> check env pos cls prop Write ty)

let check_call env (recv : Tast.expr) pos meth =
  Option.iter
    (private_member env recv Typing_env.get_method meth)
    ~f:(fun (cls, ty) ->
      (* The visitor treats an `as` bound of a method type parameter as
         covariant, but for variance it is contravariant (see
         [Typing_variance.tparam_constraint]), so bounds are left out. *)
      let ty =
        match get_node ty with
        | Tfun ft ->
          mk
            ( get_reason ty,
              Tfun { ft with ft_tparams = []; ft_where_constraints = [] } )
        | _ -> ty
      in
      check env pos cls meth Call ty)

(* The property written by an lvalue such as `$x->p`, `$x->p[0]` or `$x->p[]` *)
let rec written_property (lval : Tast.expr) =
  match lval with
  | (_, _, Obj_get (recv, (_, pos, Id (_, prop)), _, Is_prop)) ->
    Some (recv, pos, prop)
  | (_, _, Array_get (lval, _)) -> written_property lval
  | _ -> None

let handler =
  object
    inherit Tast_visitor.handler_base

    method! at_expr env =
      function
      | (_, _, Assign (lval, _, _)) ->
        Option.iter (written_property lval) ~f:(fun (recv, pos, prop) ->
            check_write env recv pos prop)
      | ( _,
          _,
          Call
            {
              func = (_, _, Obj_get (recv, (_, pos, Id (_, meth)), _, Is_method));
              _;
            } ) ->
        check_call env recv pos meth
      | _ -> ()
  end
