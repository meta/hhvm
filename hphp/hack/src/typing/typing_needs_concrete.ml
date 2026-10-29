(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)
open Hh_prelude

let add_call_needs_concrete
    env check_level pos class_name meth_name decl_pos via =
  match check_level with
  | 1 ->
    Typing_warning_utils.add
      env
      ( pos,
        Typing_warning.Call_needs_concrete,
        {
          Typing_warning.Call_needs_concrete.call_pos = pos;
          class_name;
          meth_name;
          decl_pos;
          via;
        } )
  | 2 ->
    Typing_error_utils.add_typing_error
      ~env
      Typing_error.(
        primary
        @@ Primary.Call_needs_concrete
             { pos; class_name; meth_name; decl_pos; via })
  | _ -> ()

let add_abstract_access_via_static env pos class_name member_name decl_pos =
  let containing_method_pos = Some env.Typing_env_types.genv.function_pos in
  Typing_error_utils.add_typing_error
    ~env
    Typing_error.(
      primary
      @@ Primary.Abstract_access_via_static
           { pos; class_name; member_name; decl_pos; containing_method_pos })

let add_uninstantiable_class_via_static env pos class_name decl_pos =
  let containing_method_pos = Some env.Typing_env_types.genv.function_pos in
  Typing_error_utils.add_typing_error
    ~env
    Typing_error.(
      primary
      @@ Primary.Uninstantiable_class_via_static
           { pos; class_name; decl_pos; containing_method_pos })

let check_abstract_access_via_static
    env class_get_pos def_pos cid mid (e : ('ex, 'en) Aast_defs.class_id_) =
  match e with
  | CIstatic when not (Typing_env.static_points_to_concrete_class env) ->
    add_abstract_access_via_static env class_get_pos cid mid def_pos
  | CIstatic
  | CIself
  | CIparent
  | CI _
  | CIreified _
  | CIexpr _ ->
    ()

let check_class_get
    (env : Typing_env_types.env)
    (class_get_pos : Pos.t)
    (def_pos : Pos_or_decl.t)
    (cid : string)
    (mid : string)
    (ce : Typing_defs.class_elt)
    (e : ('ex, 'en) Aast_defs.class_id_)
    ~(is_function_pointer : bool)
    (is_method : bool) : unit =
  let named_class_check_level =
    if is_function_pointer then
      Typechecker_options.needs_concrete_class_function_pointer_check
        env.genv.tcopt
    else
      Typechecker_options.needs_concrete_class_call_check env.genv.tcopt
  in
  let callee_is_needs_concrete_method : bool =
    is_method && Typing_defs.get_ce_readonly_prop_or_needs_concrete ce
  in
  let check_needs_concrete_call (via : [ `Static | `Self | `Parent ]) : unit =
    (* `self` and `parent` forward the referent of `static` so are just as dangerous *)
    if
      callee_is_needs_concrete_method
      && not (Typing_env.static_points_to_concrete_class env)
    then
      add_call_needs_concrete
        env
        2
        class_get_pos
        cid
        mid
        def_pos
        (via :> [ `Id | `Static | `Self | `Parent ])
  in
  match e with
  | CI _ when named_class_check_level > 0 && callee_is_needs_concrete_method ->
    Typing_env.get_class env cid
    |> Decl_entry.to_option
    |> Option.iter ~f:(fun (class_ : Decl_provider.class_decl) ->
           if not (Typing_env.is_concrete_class env class_) then
             add_call_needs_concrete
               env
               named_class_check_level
               class_get_pos
               cid
               mid
               def_pos
               `Id)
  | CIself -> check_needs_concrete_call `Self
  | CIparent -> check_needs_concrete_call `Parent
  | CIstatic ->
    let () = check_needs_concrete_call `Static in
    if Typing_defs.get_ce_abstract ce then
      (* We check for abstract access via `static`
       * as part of the "needs concrete" feature, because
       * checking for calls to `abstract` functions for
       * `self`/`parent`/classname, etc. is already covered by other type
       * errors such as Primary.Self_abstract_call, Primary.Parent_abstract_call, etc.
       *)
      check_abstract_access_via_static env class_get_pos def_pos cid mid e
  | CI _ -> ()
  | CIreified _ -> ()
  | CIexpr _ -> ()

let check_instantiation
    (env : Typing_env_types.env)
    (instantiation_pos : Pos.t)
    (cid : ('ex, 'en) Aast_defs.class_id_) : unit =
  match cid with
  | CIstatic when not (Typing_env.static_points_to_concrete_class env) ->
    Typing_env.get_self_class env
    |> Decl_entry.to_option
    |> Option.iter ~f:(fun (class_ : Decl_provider.class_decl) ->
           let would_be_redundant =
             (* Elsewhere we already generate a 4002 (uninstantiable class)
              * error for `new static(....)` in `abstract final` classes
              * https://www.internalfb.com/code/fbsource/[0032e6cacac2bab09425384b967f12d8d4743daf]/fbcode/hphp/hack/src/typing/typing.ml?lines=4226%2C11972
              *)
             Folded_class.abstract class_ && Folded_class.final class_
           in
           if not would_be_redundant then
             add_uninstantiable_class_via_static
               env
               instantiation_pos
               (Folded_class.name class_)
               (Folded_class.pos class_))
  | CIstatic
  | CIself
  | CIparent
  | CI _
  | CIreified _
  | CIexpr _ ->
    ()

let check_abstract_tconst_via_static
    (env : Typing_env_types.env) (pos : Pos.t) (tconst_name : string) : unit =
  if not (Typing_env.static_points_to_concrete_class env) then
    Typing_env.get_self_class env
    |> Decl_entry.to_option
    |> Option.iter ~f:(fun (class_ : Decl_provider.class_decl) ->
           match Typing_env.get_typeconst env class_ tconst_name with
           | Some
               ({
                  Typing_defs.ttc_kind =
                    Typing_defs.TCAbstract { Typing_defs.atc_default = None; _ };
                  _;
                } as tc) ->
             (* Resolving an abstract type constant without a default through
              * `static`/`this` requires a runtime value that an abstract class
              * does not have.
              * We treat this like abstract member access via `static`
              * (see [check_class_get]) so the containing method must be
              * `__NeedsConcrete`. *)
             add_abstract_access_via_static
               env
               pos
               (Folded_class.name class_)
               tconst_name
               (fst tc.Typing_defs.ttc_name)
           | _ -> ())

let check_reified_targ
    (env : Typing_env_types.env)
    (tparam : Typing_defs.decl_tparam)
    (hint : Nast.hint) : unit =
  match tparam.Typing_defs.tp_reified with
  | Ast_defs.Erased -> ()
  | Ast_defs.SoftReified
  | Ast_defs.Reified ->
    let visitor =
      object
        inherit [_] Aast.iter as super

        method! on_hint env hint =
          match hint with
          | (pos, Aast.Haccess ((_, Aast.Hthis), (_, tconst_name) :: _)) ->
            check_abstract_tconst_via_static env pos tconst_name
          | _ -> super#on_hint env hint
      end
    in
    visitor#on_hint env hint

let check_reified_targs
    (env : Typing_env_types.env)
    (tparams : Typing_defs.decl_tparam list)
    (targs : ('ex * Aast.hint) list) : unit =
  List.iter2 tparams targs ~f:(fun tparam (_, hint) ->
      check_reified_targ env tparam hint)
  |> (ignore : _ List.Or_unequal_lengths.t -> unit)

let check_class_def
    (env : Typing_env_types.env)
    (c : Nast.class_)
    (tc : Decl_provider.class_decl) : unit =
  (* Check for __NeedsConcrete on instance methods (non-static methods) and constructors *)
  List.iter c.c_methods ~f:(fun m ->
      if
        Naming_attributes.mem
          Naming_special_names.UserAttributes.uaNeedsConcrete
          m.m_user_attributes
      then
        let meth_name = snd m.m_name in
        if String.equal meth_name Naming_special_names.Members.__construct then
          (* __NeedsConcrete on constructor *)
          Typing_error_utils.add_typing_error
            ~env
            Typing_error.(
              primary
              @@ Primary.Needs_concrete_on_constructor
                   { pos = fst m.m_name; class_name = snd c.c_name })
        else if not m.m_static then
          (* __NeedsConcrete on instance method *)
          Typing_error_utils.add_typing_error
            ~env
            Typing_error.(
              primary
              @@ Primary.Needs_concrete_on_instance_method
                   { pos = fst m.m_name; class_name = snd c.c_name; meth_name })
        else if
          (* In a final concrete class `static` is always the class itself, so
           * the attribute is redundant *)
          Folded_class.final tc && Typing_env.is_concrete_class env tc
        then
          Typing_error_utils.add_typing_error
            ~env
            Typing_error.(
              primary
              @@ Primary.Needs_concrete_in_final_class
                   { pos = fst m.m_name; class_name = snd c.c_name; meth_name }))
