(*
 * Copyright (c) 2015, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

(*****************************************************************************)
(* Checks that a class implements an interface *)
(*****************************************************************************)

[@@@alert "-dependencies"]
(* if Child depends on Parent, then we don't need to also
   * track that Child::foo depends on Parent
*)

open Hh_prelude
open Option.Monad_infix
open Typing_defs
module Env = Typing_env
module Dep = Typing_deps.Dep
module TUtils = Typing_utils
module Inst = Decl_instantiate
module Phase = Typing_phase
module SN = Naming_special_names
module Cls = Folded_class
module MakeType = Typing_make_type
module TCO = Typechecker_options

module MemberKind = struct
  type t =
    | Property
    | Static_property
    | Method
    | Static_method
    | Constructor of { is_consistent: bool }
  [@@deriving eq, ord]

  let is_method = function
    | Method
    | Static_method ->
      true
    | Property
    | Static_property
    | Constructor _ ->
      false

  let is_property = function
    | Property
    | Static_property ->
      true
    | Method
    | Static_method
    | Constructor _ ->
      false

  let is_functional member_kind =
    match member_kind with
    | Method
    | Static_method
    | Constructor _ ->
      true
    | Property
    | Static_property ->
      false

  let is_constructor = function
    | Constructor _ -> true
    | _ -> false

  let is_static = function
    | Property
    | Method
    | Constructor _ ->
      false
    | Static_property
    | Static_method ->
      true
end

module MemberKindMap = Wrapped_map.Make (MemberKind)
module MemberNameMap = S_map

(* This is used to merge members from all parents (direct ancestors) of a class.
 * Certain class hierarchies are heavy in diamond patterns so merging members avoids doing the
 * same member subtyping multiple times. *)
module ParentClassElt = struct
  type parent = {
    parent_pos: Pos.t;
    parent_class: Cls.t;
    parent_type: decl_ty;
  }

  type t = {
    class_elt: class_elt;
    parent: parent;  (** The parent this class element is from. *)
    errors_if_not_overriden: Typing_error.t Lazy.t list;
        (** A list of errors to be added if that class element
            is not overridden in the class being checked. *)
  }

  let make ?errors_if_not_overriden (class_elt, parent) =
    {
      class_elt;
      parent;
      errors_if_not_overriden = Option.value errors_if_not_overriden ~default:[];
    }

  (* Class elements with the same names and origins should be equal
     modulo type instantiations, which is why we also need to compare types.
     For example,

        interface I<T> {
          public function foo():T;
        }
        interface I1 extends I<string> {}
        interface I2 extends I<int> {}
        class C implements I1, I2 {
          public function foo():int { return 3; }
        }

     When unioning members of I1 and I2, we have two `foo` members with the same
     origin (`I`) but with different types. *)
  let compare : t -> t -> int =
   fun { class_elt = elt1; _ } { class_elt = elt2; _ } ->
    let {
      ce_visibility = _;
      ce_type = type1;
      ce_origin = origin1;
      ce_deprecated = _;
      ce_pos = _;
      ce_flags = _;
      ce_sealed_allowlist = _;
      ce_overlapping_tparams = _;
      ce_package_requirement = _;
    } =
      elt1
    in
    let {
      ce_visibility = _;
      ce_type = type2;
      ce_origin = origin2;
      ce_deprecated = _;
      ce_pos = _;
      ce_flags = _;
      ce_sealed_allowlist = _;
      ce_overlapping_tparams = _;
      ce_package_requirement = _;
    } =
      elt2
    in
    match String.compare origin1 origin2 with
    | 0 -> compare_decl_ty (Lazy.force type1) (Lazy.force type2)
    | x -> x
end

module ParentClassEltSet =
  Reordered_argument_collections.Reordered_argument_set
    (Stdlib.Set.Make (ParentClassElt))

module ParentClassConst = struct
  type t = {
    class_const: class_const;
    parent: ParentClassElt.parent;
  }

  let make class_const parent = { class_const; parent }

  let compare { class_const = left; _ } { class_const = right; _ } =
    String.compare left.cc_origin right.cc_origin
end

module ParentClassConstSet = Stdlib.Set.Make (ParentClassConst)

module ParentTypeConst = struct
  type t = {
    typeconst: typeconst_type;
    parent: ParentClassElt.parent;
  }

  let make typeconst parent = { typeconst; parent }

  let compare { typeconst = left; _ } { typeconst = right; _ } =
    String.compare left.ttc_origin right.ttc_origin
end

module ParentTypeConstSet = Stdlib.Set.Make (ParentTypeConst)

let constructor_is_consistent kind =
  match kind with
  | ConsistentConstruct
  | FinalClass ->
    true
  | Inconsistent -> false

(*****************************************************************************)
(* Given a map of members, check that the overriding is correct.
 * Please note that 'members' has a very general meaning here.
 * It can be class variables, methods, static methods etc ... The same logic
 * is applied to verify that the overriding is correct.
 *)
(*****************************************************************************)

(* Rules for visibility *)
let check_visibility env parent_vis c_vis parent_pos pos on_error =
  match (parent_vis, c_vis) with
  | (Vprivate _, _) ->
    (* The only time this case should come into play is when the
     * parent_class_elt comes from a trait *)
    ()
  | (Vpublic, Vpublic)
  | (Vprotected _, Vprotected _)
  | (Vprotected _, Vpublic)
  | (Vprotected_internal _, Vprotected _)
  | (Vprotected_internal _, Vpublic)
  | (Vinternal _, Vpublic) ->
    ()
  | ( Vprotected_internal { module_ = parent_m; _ },
      Vprotected_internal { module_ = child_m; _ } )
  | (Vprotected_internal { module_ = parent_m; _ }, Vinternal child_m)
  | (Vinternal parent_m, Vinternal child_m) ->
    let err_opt =
      match
        Typing_modules.can_access_internal
          ~env
          ~current:(Some child_m)
          ~target:(Some parent_m)
      with
      | `Yes -> None
      | `Disjoint (current, target) ->
        Some
          (Typing_error.Secondary.Visibility_override_internal
             {
               pos;
               module_name = Some current;
               parent_pos;
               parent_module = target;
             })
      | `Outside target ->
        Some
          (Typing_error.Secondary.Visibility_override_internal
             { pos; module_name = None; parent_pos; parent_module = target })
      (* TODO(T109499403) This case *is* possible, but because it refers to
       * class members in traits, any code that runs afoul of this rule will
       * also violate the nast check requiring that any trait member in a
       * non-internal trait must also be non-internal. I can't even figure out
       * a test case where this also doesn't violate _other_ rules about
       * referencing internal symbols in modules, e.g.:
       *
       *
       * internal trait Quuz {
       *   internal function lol(): void {}
       * }
       *
       * trait Corge {
       *   use Quuz;
       *   internal function lol(): void {}
       * }
       *
       * This code snippet alone raises two errors. One for `use Quuz`,
       * and one for `Corge::lol`.
       *
       *)
      | `OutsideViaTrait _ -> None
    in
    Option.iter err_opt ~f:(fun err ->
        Typing_error_utils.add_typing_error ~env
        @@ Typing_error.(apply_reasons ~on_error err))
  | _ ->
    let parent_vis = Typing_defs.string_of_visibility parent_vis in
    let vis = Typing_defs.string_of_visibility c_vis in
    let err =
      Typing_error.Secondary.Visibility_extends
        { pos; vis; parent_pos; parent_vis }
    in
    Typing_error_utils.add_typing_error ~env
    @@ Typing_error.(apply_reasons ~on_error err)

let check_class_elt_visibility env parent_class_elt class_elt on_error =
  let parent_vis = parent_class_elt.ce_visibility in
  let c_vis = class_elt.ce_visibility in
  let (lazy parent_pos) = parent_class_elt.ce_pos in
  let (lazy pos) = class_elt.ce_pos in
  check_visibility env parent_vis c_vis parent_pos pos on_error

let get_member member_kind class_ =
  match member_kind with
  | MemberKind.Property -> Cls.get_prop class_
  | MemberKind.Static_property -> Cls.get_sprop class_
  | MemberKind.Method -> Cls.get_method class_
  | MemberKind.Static_method -> Cls.get_smethod class_
  | MemberKind.Constructor _ -> (fun _ -> fst (Cls.construct class_))

type missing_member_info = {
  member_name: string;
  member_kind: MemberKind.t;
  parent_class_elt: class_elt;
  parent_pos: Pos.t;
  is_override: bool;
}

let stub_all_methods_quickfix
    ~(class_name : string)
    ~(title : string)
    (methods : missing_member_info list) : Pos.t Quickfix.t =
  let method_texts =
    List.map
      methods
      ~f:(fun { member_name; parent_class_elt; member_kind; is_override; _ } ->
        let is_static = MemberKind.is_static member_kind in
        Typing_skeleton.of_method
          member_name
          parent_class_elt
          ~is_static
          ~is_override
          ~open_braces:false
        |> Typing_skeleton.to_string)
  in
  let classish_end_new_text = String.concat method_texts in
  Quickfix.make
    ~title
    ~edits:
      (Quickfix.Classish_end
         { classish_end_new_text; classish_end_name = class_name })
    ~hint_styles:
      Quickfix.
        [
          HintStyleHint
            (Classish_positions_types.Classish_closing_brace class_name);
        ]

(* Emit an error for every missing method or property in this
   class. Offer a single quickfix for adding all the missing
   methods. *)
let members_missing_error
    env
    (class_pos : Pos.t)
    (class_ : Cls.t)
    (members : missing_member_info list) : unit =
  let (missing_methods, missing_props) =
    List.partition_tf members ~f:(fun { member_kind; _ } ->
        MemberKind.is_functional member_kind)
  in

  let (class_methods, interface_methods) =
    List.partition_tf missing_methods ~f:(fun { is_override; _ } -> is_override)
  in

  List.iteri
    interface_methods
    ~f:(fun i { parent_pos; member_name; parent_class_elt; _ } ->
      let quickfixes =
        match i with
        | 0 ->
          [
            stub_all_methods_quickfix
              ~class_name:(Cls.name class_)
              ~title:"Add stubs for missing interface methods"
              interface_methods;
          ]
        | _ -> []
      in
      let (lazy defn_pos) = parent_class_elt.ce_pos in

      let err =
        Typing_error.(
          primary
          @@ Primary.Member_not_implemented
               {
                 pos = parent_pos;
                 member_name;
                 decl_pos = defn_pos;
                 quickfixes;
               })
      in
      Typing_error_utils.add_typing_error ~env err);

  List.iteri class_methods ~f:(fun i { member_name; parent_class_elt; _ } ->
      let quickfixes =
        match i with
        | 0 ->
          [
            stub_all_methods_quickfix
              ~class_name:(Cls.name class_)
              ~title:"Add stubs for missing inherited methods"
              class_methods;
          ]
        | _ -> []
      in

      let trace =
        lazy
          (Ancestor_route.describe_route
             env
             ~classish:(Cls.name class_)
             ~ancestor:parent_class_elt.ce_origin)
      in
      Typing_error_utils.add_typing_error
        ~env
        Typing_error.(
          primary
          @@ Primary.Implement_abstract
               {
                 is_final = Cls.final class_;
                 pos = class_pos;
                 decl_pos = Lazy.force parent_class_elt.ce_pos;
                 trace;
                 kind = `meth;
                 name = member_name;
                 quickfixes;
               }));

  List.iter missing_props ~f:(fun { member_name; parent_class_elt; _ } ->
      let trace =
        lazy
          (Ancestor_route.describe_route
             env
             ~classish:(Cls.name class_)
             ~ancestor:parent_class_elt.ce_origin)
      in
      Typing_error_utils.add_typing_error
        ~env
        Typing_error.(
          primary
          @@ Primary.Implement_abstract
               {
                 is_final = Cls.final class_;
                 pos = class_pos;
                 decl_pos = Lazy.force parent_class_elt.ce_pos;
                 trace;
                 kind = `prop;
                 name = member_name;
                 quickfixes = [];
               }))

let check_subtype_methods
    env ~check_return on_error (r_ancestor, ft_ancestor) (r_child, ft_child) ()
    =
  Typing_subtype_method.(
    (* Add deps here when we override *)
    subtype_method_decl
      ~check_return
      env
      r_child
      ft_child
      r_ancestor
      ft_ancestor
      on_error)

(* An abstract member can be declared in multiple ancestors. Sometimes these
 * declarations can be different, but yet compatible depending on which ancestor
 * we inherit the member from. For example:
 *
 * interface I1 { abstract public function foo(): int; }
 * interface I2 { abstract public function foo(): mixed; }
 *
 * abstract class C implements I1, I2 {}
 *
 * I1::foo() is compatible with I2::foo(), but not vice versa. Hack chooses the
 * signature for C::foo() arbitrarily and can report an error if we make a
 * "wrong" choice. We check for this case and emit an extra line in the error
 * instructing the programmer to redeclare the member to remove the ambiguity.
 *
 * Note: We could detect this case and make the correct choice for the user, but
 * this would require invalidating the current entry we have in the typing heap
 * for this class. We cannot make this choice earlier during typing_decl because
 * a class we depend on during the subtyping may not have been declared yet.
 *)
(* TODO(jjwu): get rid of this for type constants too, and we can delete *)
let check_ambiguous_inheritance f parent child pos class_ origin on_error ~env =
  Diagnostics.try_when
    (f parent child)
    ~if_error_and:(fun () ->
      String.( <> ) (Cls.name class_) origin
      && Diagnostics.try_no_errors (f child parent))
    ~then_:(fun error ->
      Typing_error_utils.ambiguous_inheritance
        pos
        (Cls.name class_)
        origin
        error
        on_error
        ~env)

(** Checks that we're not overriding a final method. *)
let check_override_final_method env parent_class_elt class_elt on_error =
  let is_override_of_final_method =
    get_ce_final parent_class_elt
    && String.( <> ) parent_class_elt.ce_origin class_elt.ce_origin
  in
  if is_override_of_final_method && not (get_ce_synthesized class_elt) then
    (* we have a final method being overridden by a user-declared method *)
    let (lazy parent_pos) = parent_class_elt.ce_pos in
    let (lazy pos) = class_elt.ce_pos in
    Typing_error_utils.add_typing_error
      ~env
      Typing_error.(
        apply_reasons ~on_error @@ Secondary.Override_final { pos; parent_pos })

(** Checks that we're not overriding a concrete static property. *)
let check_override_concrete_static_prop
    ~parent_pos env member_name parent_class_elt class_elt =
  if
    (not (get_ce_abstract parent_class_elt))
    && not (get_ce_const parent_class_elt)
  then
    let child_prop_pos = Lazy.force class_elt.ce_pos in
    Typing_warning_utils.add
      env
      Typing_warning.
        ( parent_pos,
          Static_property_override,
          Static_property_override.{ prop_name = member_name; child_prop_pos }
        )

let bad_sealed_override_error
    env member_name parent_class_elt class_elt on_error =
  let (lazy parent_pos) = parent_class_elt.ce_pos in
  let (lazy pos) = class_elt.ce_pos in
  let (snd_err1, snd_err2) =
    Typing_error.Secondary.
      ( Bad_method_override { pos; member_name },
        Override_sealed { pos; parent_pos } )
  in
  let on_error =
    Typing_error.Reasons_callback.prepend_on_apply on_error snd_err1
  in
  Typing_error_utils.add_typing_error ~env
  @@ Typing_error.apply_reasons ~on_error snd_err2

let check_sealed_allowlist
    env member_name _parent_class parent_class_elt class_ class_elt on_error =
  match parent_class_elt.ce_sealed_allowlist with
  | None -> ()
  | Some allowlist ->
    if not (S_set.mem class_elt.ce_origin allowlist) then
      (* if the origin of class_elt is not in the allowlist we emit an error unless
         the class itself is in the allowlist and the elt is imported from a trait *)
      let is_class_elt_origin_trait =
        match Env.get_class env class_elt.ce_origin |> Decl_entry.to_option with
        | Some origin_class when Ast_defs.is_c_trait (Cls.kind origin_class) ->
          true
        | _ -> false
      in
      let is_class_in_allowlist = S_set.mem (Cls.name class_) allowlist in
      if not (is_class_elt_origin_trait && is_class_in_allowlist) then
        bad_sealed_override_error
          env
          member_name
          parent_class_elt
          class_elt
          on_error

(* The rules for overriding methods with __RequirePackage follow by analogy to
 * visibility modifiers -- a method cannot become "less visible" by override.
 * So, if package a includes package b, a method requiring package a may be
 * overridden by one requiring package b. *)
let check_require_package env member_name parent_class_elt class_elt on_error =
  let (lazy pos) = class_elt.ce_pos in
  let snd_err =
    Typing_error.Secondary.Bad_method_override { pos; member_name }
  in
  let on_error =
    Typing_error.Reasons_callback.prepend_on_apply on_error snd_err
  in
  let strengthening_error p_super (c_pos, c) ~child_str ~parent_str =
    Typing_error_utils.add_typing_error
      ~env
      Typing_error.(
        apply_reasons ~on_error
        @@ Secondary.Require_package_mismatch
             {
               pos = c_pos;
               reason_sub =
                 lazy
                   [
                     ( c_pos,
                       Format.sprintf "This method %s package `%s`" child_str c
                     );
                   ];
               reason_super =
                 lazy
                   [
                     ( p_super,
                       Format.sprintf
                         "The parent method %s attribute. The override must have a weaker requirement."
                         parent_str );
                   ];
             })
  in
  let mismatch_error
      (p_pos, p) (c_pos, c) parent_package ~child_str ~parent_str ~inclusion_str
      =
    Typing_error_utils.add_typing_error
      ~env
      Typing_error.(
        apply_reasons ~on_error
        @@ Secondary.Require_package_mismatch
             {
               pos = c_pos;
               reason_sub =
                 lazy
                   [
                     ( c_pos,
                       Format.sprintf "This method %s package `%s`" child_str c
                     );
                   ];
               reason_super =
                 lazy
                   [
                     ( p_pos,
                       Format.sprintf
                         "It cannot override a method that %s package `%s`"
                         parent_str
                         p );
                     ( Pos_or_decl.of_raw_pos
                         (Package.get_package_pos parent_package),
                       Format.sprintf "Because `%s` %s `%s`" p inclusion_str c
                     );
                   ];
             })
  in

  let check_inclusion p c ~parent_str ~child_str =
    match
      (Env.get_package_by_name env (snd p), Env.get_package_by_name env (snd c))
    with
    | (Some parent_package, Some child_package) ->
      Package.(
        (match relationship parent_package child_package with
        | Package.Includes
        | Package.Equal ->
          ()
        | Package.Unrelated ->
          mismatch_error
            p
            c
            parent_package
            ~child_str
            ~parent_str
            ~inclusion_str:"does not include"
        | Package.Soft_includes ->
          mismatch_error
            p
            c
            parent_package
            ~child_str
            ~parent_str
            ~inclusion_str:"only soft-includes"))
    | (None, _)
    | (_, None) ->
      (* Required package existence and inclusion of file package checked elsewhere *)
      ()
  in

  match
    (parent_class_elt.ce_package_requirement, class_elt.ce_package_requirement)
  with
  | (Some parent_requirent, Some child_requirement) -> begin
    match (parent_requirent, child_requirement) with
    | (RPRequire p, RPRequire c) ->
      let parent_str = "requires" in
      let child_str = "requires" in
      check_inclusion p c ~parent_str ~child_str
    | (RPSoft _, RPRequire c) ->
      let (lazy p_super) = parent_class_elt.ce_pos in
      let child_str = "requires" in
      let parent_str = "has a `__SoftRequirePackage`" in
      strengthening_error p_super c ~child_str ~parent_str
    | (RPNormal, RPRequire c) ->
      let (lazy p_super) = parent_class_elt.ce_pos in
      let child_str = "requires" in
      let parent_str = "does not have a `__RequirePackage`" in
      strengthening_error p_super c ~child_str ~parent_str
    | (RPRequire p, RPSoft c) ->
      let parent_str = "requires" in
      let child_str = "soft-requires" in
      check_inclusion p c ~parent_str ~child_str
    | (RPSoft p, RPSoft c) ->
      let parent_str = "soft-requires" in
      let child_str = "soft-requires" in
      check_inclusion p c ~parent_str ~child_str
    | (RPNormal, RPSoft c) ->
      let (lazy p_super) = parent_class_elt.ce_pos in
      let child_str = "soft-requires" in
      let parent_str = "does not have a `__SoftRequirePackage`" in
      strengthening_error p_super c ~child_str ~parent_str
    | (RPSoft _, RPNormal)
    | (RPRequire _, RPNormal) ->
      (* Child is less restrictive than the parent, also covered by extends check *)
      ()
    | (RPNormal, RPNormal) ->
      (* Covered by the existing check of the symbol reference in `extends` clause *)
      ()
  end
  (* non-methods *)
  | (None, _)
  | (_, None) ->
    ()

(** Check that we are not overriding an __LSB property *)
let check_lsb_overrides
    env member_kind member_name parent_class_elt class_elt on_error =
  let parent_is_lsb = get_ce_lsb parent_class_elt in
  if MemberKind.equal MemberKind.Static_property member_kind && parent_is_lsb
  then
    (* __LSB attribute is being overridden *)
    let (lazy parent_pos) = parent_class_elt.ce_pos in
    let (lazy pos) = class_elt.ce_pos in
    Typing_error_utils.add_typing_error ~env
    @@ Typing_error.(
         apply_reasons ~on_error
         @@ Secondary.Override_lsb { pos; parent_pos; member_name })

(** Check that __LateInit annotation on members are consistent between parents and children. *)
let check_lateinit env parent_class_elt class_elt on_error =
  let lateinit_diff =
    Bool.( <> ) (get_ce_lateinit parent_class_elt) (get_ce_lateinit class_elt)
  in
  if lateinit_diff then
    let (lazy parent_pos) = parent_class_elt.ce_pos in
    let (lazy child_pos) = class_elt.ce_pos in
    Typing_error_utils.add_typing_error ~env
    @@ Typing_error.(
         apply_reasons ~on_error
         @@ Secondary.Bad_lateinit_override
              {
                pos = child_pos;
                parent_pos;
                parent_is_lateinit = get_ce_lateinit parent_class_elt;
              })

let check_async env ft_parent ft_child parent_pos pos on_error =
  match (get_ft_async ft_parent, get_ft_async ft_child) with
  | (true, false) ->
    Typing_error_utils.add_typing_error
      ~env
      Typing_error.(
        apply_reasons ~on_error @@ Secondary.Override_async { pos; parent_pos })
  | _ -> ()

let check_xhp_attr_required env parent_class_elt class_elt on_error =
  let is_less_strict = function
    | (Some Xhp_attribute.Required, _)
    | (Some Xhp_attribute.LateInit, Some Xhp_attribute.LateInit)
    | (Some Xhp_attribute.LateInit, None)
    | (None, None) ->
      false
    | (_, _) -> true
  in
  let parent_attr = get_ce_xhp_attr parent_class_elt in
  let attr = get_ce_xhp_attr class_elt in
  match (parent_attr, attr) with
  | ( Some { Xhp_attribute.xa_tag = parent_tag; _ },
      Some { Xhp_attribute.xa_tag = tag; _ } )
    when is_less_strict (tag, parent_tag) ->
    let (lazy parent_pos) = parent_class_elt.ce_pos in
    let (lazy child_pos) = class_elt.ce_pos in
    let lateinit = Markdown_lite.md_codify "@lateinit" in
    let required = Markdown_lite.md_codify "@required" in
    let show_tag_opt = function
      | None -> Printf.sprintf "not %s or %s" required lateinit
      | Some Xhp_attribute.Required -> required
      | Some Xhp_attribute.LateInit -> lateinit
    in
    Typing_error_utils.add_typing_error ~env
    @@ Typing_error.(
         apply_reasons ~on_error
         @@ Secondary.Bad_xhp_attr_required_override
              {
                pos = child_pos;
                tag = show_tag_opt tag;
                parent_pos;
                parent_tag = show_tag_opt parent_tag;
              })
  | (_, _) -> ()

let add_pessimisation_dependency
    env child_cls member_name member_kind parent_cls =
  let result =
    (* For the time being we only care about methods *)
    match member_kind with
    | MemberKind.Method ->
      ( Cls.get_method child_cls member_name,
        Cls.get_method parent_cls member_name,
        Some (Typing_pessimisation_deps.Method member_name) )
    | MemberKind.Static_method ->
      ( Cls.get_smethod child_cls member_name,
        Cls.get_smethod parent_cls member_name,
        Some (Typing_pessimisation_deps.SMethod member_name) )
    | _ -> (None, None, None)
  in
  match result with
  | (Some child_elt, Some parent_elt, Some member) ->
    (* We resolve both the parent and child to their origin. This allows us
     * to perform hierarchy poisioning for traits correctly: If a class C
     * gets a definition of some method foo by using a trait D, then the
     * following two conditions hold simultaneously:
     * a) If a child of C pessimises foo, then D::foo must be pessimised.
     * b) If the definition of foo in requires it to be pessimised, then
     *    all users of C::foo must be aware of that. Further, if C::foo
     *    overrides P::foo in some parent P of C, then this P::foo must be
     *    poisoned.
     *
     *
     * To resolve this, we effectively unify C:ffoo and D::foo in the
     * (pessimisation) dependency graph:
     * Elsewhere, we make sure that all users of C::foo point to D::foo
     * instead.  Here, we make sure that we mark D::foo as overriding P::foo
     * and any direct overrider of C::foo is marked as overriding D::foo
     * instead. *)
    let child_name = child_elt.Typing_defs.ce_origin in
    let parent_name = parent_elt.Typing_defs.ce_origin in
    Typing_pessimisation_deps.add_override_dep
      (Env.get_deps_mode env)
      member
      ~child_name
      ~parent_name
  | _ -> ()

let add_member_dep
    env class_ parent_class (member_kind, member_name, member_origin) =
  let origin_pos = Cls.pos parent_class in
  if not (Pos_or_decl.is_hhi origin_pos) then (
    let dep =
      match member_kind with
      | MemberKind.Method -> Dep.Method (member_origin, member_name)
      | MemberKind.Static_method -> Dep.SMethod (member_origin, member_name)
      | MemberKind.Static_property -> Dep.SProp (member_origin, member_name)
      | MemberKind.Property -> Dep.Prop (member_origin, member_name)
      | MemberKind.Constructor _ -> Dep.Constructor member_origin
    in
    let class_name = Cls.name class_ in
    Typing_deps.add_idep (Env.get_deps_mode env) (Dep.Type class_name) dep;
    if TCO.record_fine_grained_dependencies @@ Env.get_tcopt env then
      add_pessimisation_dependency
        env
        class_
        member_name
        member_kind
        parent_class
  )

let check_compatible_sound_dynamic_attributes
    env member_name member_kind parent_class_elt class_elt on_error =
  if
    (not (MemberKind.is_constructor member_kind))
    && get_ce_support_dynamic_type parent_class_elt
    && not (get_ce_support_dynamic_type class_elt)
  then
    let (lazy pos) = class_elt.ce_pos in
    let (lazy parent_pos) = parent_class_elt.ce_pos in
    Typing_error_utils.add_typing_error ~env
    @@ Typing_error.(
         apply_reasons ~on_error
         @@ Secondary.Override_method_support_dynamic_type
              {
                pos;
                parent_pos;
                parent_origin = parent_class_elt.ce_origin;
                method_name = member_name;
              })

let check_prop_const_mismatch env parent_class_elt class_elt on_error =
  if Bool.( <> ) (get_ce_const class_elt) (get_ce_const parent_class_elt) then
    Typing_error_utils.add_typing_error
      ~env
      Typing_error.(
        apply_reasons ~on_error
        @@ Secondary.Overriding_prop_const_mismatch
             {
               pos = Lazy.force class_elt.ce_pos;
               is_const = get_ce_const class_elt;
               parent_pos = Lazy.force parent_class_elt.ce_pos;
               parent_is_const = get_ce_const parent_class_elt;
             })

(** Property `readonly`-ness is invariant across overrides: reading a mutable
    property through a `readonly` view, or writing a `readonly` property through
    a mutable view, would both be unsound. We only warn (rather than error) for
    now, following `check_override_concrete_static_prop`. *)
let check_readonly_prop_override
    ~parent_pos env member_name parent_class_elt class_elt =
  if
    Bool.( <> )
      (get_ce_readonly_prop_or_needs_concrete class_elt)
      (get_ce_readonly_prop_or_needs_concrete parent_class_elt)
  then
    let child_prop_pos = Lazy.force class_elt.ce_pos in
    Typing_warning_utils.add
      env
      Typing_warning.
        ( parent_pos,
          Readonly_property_override,
          Readonly_property_override.{ prop_name = member_name; child_prop_pos }
        )

let check_abstract_overrides_concrete
    env member_kind parent_class_elt class_elt on_error =
  if (not (get_ce_abstract parent_class_elt)) && get_ce_abstract class_elt then
    (* It is valid for abstract class to extend a concrete class, but it cannot
     * redefine already concrete members as abstract.
     * See override_abstract_concrete.php test case for example. *)
    Typing_error_utils.add_typing_error
      ~env
      Typing_error.(
        apply_reasons ~on_error
        @@ Secondary.Abstract_concrete_override
             {
               pos = Lazy.force class_elt.ce_pos;
               parent_pos = Lazy.force parent_class_elt.ce_pos;
               kind =
                 (if MemberKind.is_functional member_kind then
                   `method_
                 else
                   `property);
             })

(** Why this check: It would be unsound for a class that needs concrete (`<<__NeedsConcrete>>`) to override one that does not,
  * since a __NeedsConcrete method imposes stricter requirements on its input (`static` must point to a concrete class).
  * A final, concrete class is exempt: its methods only run with `static` bound to the class itself. *)
let check_needs_concrete_override
    (env : Typing_env_types.env)
    ~parent_class_elt
    class_
    class_elt
    ~class_pos
    ~member_name =
  if
    (not (get_ce_readonly_prop_or_needs_concrete parent_class_elt))
    && get_ce_readonly_prop_or_needs_concrete class_elt
    && not (Cls.final class_ && Env.is_concrete_class env class_)
  then
    let defined_in_same_class =
      String.equal class_elt.ce_origin (Cls.name class_)
    in
    let (error_pos, method_name_for_method_defined_outside_class) =
      match
        Pos_or_decl.get_raw_pos_or_decl_reference (Lazy.force class_elt.ce_pos)
      with
      | `Raw pos when defined_in_same_class -> (pos, None)
      | `Raw _
      | `Decl_ref _ ->
        (class_pos, Some member_name)
    in
    let method_pos = Lazy.force class_elt.ce_pos in
    let parent_pos = Lazy.force parent_class_elt.ce_pos in
    Typing_error_utils.add_typing_error
      ~env
      Typing_error.(
        primary
        @@ Primary.Needs_concrete_override
             {
               pos = error_pos;
               method_pos;
               parent_pos;
               method_name_for_method_defined_outside_class;
             })

let detect_multiple_concrete_defs
    (class_elt, class_) (parent_class_elt, parent_class) =
  (* We want to check if there are conflicting trait declarations of a class member.
   * If the parent we are checking is a trait and the member's origin both isn't
   * that parent and isn't the class itself, then it must come from another trait
   * and there is a conflict.
   *
   * We rule out cases where any of the traits' member
   * is synthetic (from a requirement) or abstract. *)
  match Cls.kind parent_class with
  | Ast_defs.Ctrait ->
    (not (get_ce_synthesized class_elt))
    && (not (get_ce_abstract class_elt))
    && (not (get_ce_abstract parent_class_elt))
    && String.( <> ) class_elt.ce_origin (Cls.name class_)
  | Ast_defs.(Cinterface | Cclass _ | Cenum | Cenum_class _) -> false

let check_multiple_concrete_definitions
    env
    member_name
    member_kind
    (class_elt, class_)
    (parent_class_elt, parent_class)
    on_error =
  if
    (MemberKind.is_functional member_kind || get_ce_const class_elt)
    && detect_multiple_concrete_defs
         (class_elt, class_)
         (parent_class_elt, parent_class)
  then
    (* Multiple concrete trait definitions, error *)
    Typing_error_utils.add_typing_error
      ~env
      Typing_error.(
        apply_reasons ~on_error
        @@ Secondary.Multiple_concrete_defs
             {
               pos = Lazy.force class_elt.ce_pos;
               parent_pos = Lazy.force parent_class_elt.ce_pos;
               origin = class_elt.ce_origin;
               parent_origin = parent_class_elt.ce_origin;
               name = member_name;
               class_name = Cls.name class_;
             })

(* Get the type of the value that is returned: for an async function that has
 * declared return type Awaitable<t>, this is t, otherwise it's just
 * the declared return type
 *)
let get_return_value_type ft =
  match (get_ft_async ft, deref ft.ft_ret) with
  | (true, (_, Tapply ((_, class_name), [inner_ty])))
    when String.equal class_name SN.Classes.cAwaitable ->
    inner_ty
  | _ -> ft.ft_ret

let maybe_poison_ancestors
    env
    ft_parent
    ft_child
    parent_class
    child_class
    origin
    member_name
    member_kind =
  if TCO.like_casts (Provider_context.get_tcopt (Env.get_ctx env)) then
    let parent_return_ty = get_return_value_type ft_parent in
    let child_return_ty = get_return_value_type ft_child in
    let (declared_class, declared_return_ty) =
      if String.equal (Cls.name child_class) origin then
        (child_class, child_return_ty)
      else
        match Env.get_class env origin with
        | Decl_entry.DoesNotExist
        | Decl_entry.NotYetAvailable ->
          (child_class, child_return_ty)
        | Decl_entry.Found c ->
          (match Env.get_method env c member_name with
          | None -> (child_class, child_return_ty)
          | Some elt ->
            let (lazy fty) = elt.ce_type in
            (match get_node fty with
            | Tfun ft -> (c, get_return_value_type ft)
            | _ -> (child_class, child_return_ty)))
    in
    match
      ( Typing_enforceability.get_enforcement
          ~top_enforced:true
          ~this_class:(Some parent_class)
          env
          parent_return_ty,
        Typing_enforceability.get_enforcement
          ~top_enforced:true
          ~this_class:(Some declared_class)
          env
          declared_return_ty )
    with
    (* If the parent itself overrides a fully-enforced return type
     * then we need to "copy down" any intersection, so record this in the log
     *)
    | (Unenforced, Unenforced) ->
      let child_pos = Pos_or_decl.unsafe_to_raw_pos (get_pos ft_child.ft_ret) in
      let parent_pos =
        Pos_or_decl.unsafe_to_raw_pos (get_pos ft_parent.ft_ret)
      in
      let p = Pos.to_absolute parent_pos in
      let s = Printf.sprintf "!,%s,%d" (Pos.filename p) (Pos.line p) in
      Typing_log.log_pessimise_return env child_pos (Some s)
    | (Enforced, Unenforced) -> begin
      match get_node parent_return_ty with
      | Tmixed -> ()
      | _ ->
        let enforced_declared_ty =
          Typing_partial_enforcement.get_enforced_type
            env
            (Some declared_class)
            declared_return_ty
        in
        let tmp_env =
          let self_ty =
            MakeType.class_type
              Reason.none
              origin
              (List.map (Cls.tparams declared_class) ~f:(fun tp ->
                   MakeType.generic Reason.none (snd tp.tp_name)))
          in
          Env.env_with_tpenv
            env
            (Type_parameter_env.add_upper_bound
               Type_parameter_env.empty
               SN.Typehints.this
               self_ty)
        in
        let child_pos =
          Pos_or_decl.unsafe_to_raw_pos (get_pos ft_child.ft_ret)
        in
        let enforced_parent_ty =
          Typing_partial_enforcement.get_enforced_type
            env
            (Some parent_class)
            parent_return_ty
        in
        (* We need that the enforced child type is a subtype of the enforced parent type *)
        let sub1 =
          Phase.is_sub_type_decl
            ~is_dynamic_aware:true
            tmp_env
            enforced_declared_ty
            enforced_parent_ty
        in
        (* But also the original child type should be a subtype of the enforced parent type *)
        let sub2 =
          Phase.is_sub_type_decl
            ~is_dynamic_aware:true
            tmp_env
            declared_return_ty
            enforced_parent_ty
        in
        if sub1 && sub2 then
          let ty_str =
            Typing_print.full_decl (Env.get_tcopt env) enforced_parent_ty
          in
          (* Hack to remove "\\" if XHP type is rendered as "\\:X" *)
          (* TODO: fix Typing_print so that it renders XHP correctly *)
          let ty_str =
            let re = Str.regexp "\\\\:" in
            Str.global_replace re ":" ty_str
          in
          Typing_log.log_pessimise_return env child_pos (Some ty_str)
        else
          Cls.all_ancestor_names child_class
          |> List.map ~f:(fun c -> Decl_entry.to_option (Env.get_class env c))
          |> List.filter_opt
          |> List.iter ~f:(fun cls ->
                 MemberKind.(
                   match member_kind with
                   | Static_method -> Cls.get_smethod cls member_name
                   | Method -> Cls.get_method cls member_name
                   | _ -> None)
                 |> Option.iter ~f:(fun elt ->
                        let (lazy fty) = elt.ce_type in
                        match get_node fty with
                        | Tfun { ft_ret; _ } ->
                          let pos =
                            Pos_or_decl.unsafe_to_raw_pos (get_pos ft_ret)
                          in
                          (* The ^ denotes poisoning *)
                          Typing_log.log_pessimise_poisoned_return
                            env
                            pos
                            (Cls.name child_class ^ "::" ^ member_name)
                        | _ -> ()))
    end
    | _ -> ()

let bad_member_override_not_subtype
    env
    ~is_method
    ~class_
    ~parent_class
    ~(parent_type : decl_ty)
    ~member_name
    ~class_elt
    ~parent_class_elt =
  let (lazy member_pos) = class_elt.ce_pos in
  let (lazy member_parent_pos) = parent_class_elt.ce_pos in

  let (lazy member_type) = class_elt.ce_type in
  let (lazy member_parent_type) = parent_class_elt.ce_type in
  let open Option.Monad_infix in
  let get_origin_type class_ class_elt =
    lazy
      (Cls.get_ancestor class_ class_elt.ce_origin
      >>| Typing_print.full_strip_ns_decl ~verbose_fun:true env
      |> Option.value ~default:(Utils.strip_ns class_elt.ce_origin))
  in
  Typing_error.Secondary.Bad_member_override_not_subtype
    {
      is_method;
      class_name = Cls.name class_;
      parent_name = Cls.name parent_class;
      parent_type =
        lazy (Typing_print.full_strip_ns_decl ~verbose_fun:true env parent_type);
      member_pos;
      member_name;
      member_type =
        lazy (Typing_print.full_strip_ns_decl ~verbose_fun:true env member_type);
      origin_name = class_elt.ce_origin;
      origin_type = get_origin_type class_ class_elt;
      member_parent_pos;
      member_parent_type =
        lazy
          (Typing_print.full_strip_ns_decl
             ~verbose_fun:true
             env
             member_parent_type);
      member_parent_origin = parent_class_elt.ce_origin;
      member_parent_origin_type = get_origin_type parent_class parent_class_elt;
    }

(* Check that overriding is correct *)
let check_override
    env
    ~check_member_unique
    ~member_name
    member_kind
    ~class_
    ~parent_class
    ~(parent_type : decl_ty)
    ~parent_pos
    ~class_pos
    ~(class_elt : class_elt)
    ~parent_class_elt
    on_error =
  (* If the class element is defined in the class that we're checking, then
   * don't wrap with the extra
   * "Class ... does not correctly implement all required members" message *)
  let on_error =
    if String.equal class_elt.ce_origin (Cls.name class_) then
      Env.unify_error_assert_primary_pos_in_current_decl env
    else
      on_error
  in

  begin
    match member_kind with
    | MemberKind.Static_property ->
      check_override_concrete_static_prop
        ~parent_pos
        env
        member_name
        parent_class_elt
        class_elt
    | _ -> ()
  end;

  if get_ce_tests_bypass_visibility parent_class_elt then begin
    let is_private_parent =
      match parent_class_elt.ce_visibility with
      | Vprivate _ -> true
      | _ -> false
    in
    let is_public_child =
      match class_elt.ce_visibility with
      | Vpublic -> true
      | _ -> false
    in
    (* Private members marked `<<__TestsBypassVisibility>>` are kept during
       inheritance, so they reach normal override checking. A concrete private
       member with the attribute must remain unique in its hierarchy; otherwise
       a test-context access through a subclass would be ambiguous between the
       inherited private member and the subclass member.

       We do not reject the opposite direction, where the ancestor has an
       ordinary concrete private member and the child introduces a member with
       `<<__TestsBypassVisibility>>`. The ordinary ancestor private member is
       still inaccessible from test contexts, so upcasting cannot expose it. *)
    if is_private_parent && not (get_ce_abstract parent_class_elt) then begin
      let (lazy pos) = class_elt.ce_pos in
      let (lazy parent_pos) = parent_class_elt.ce_pos in
      Typing_error_utils.add_typing_error
        ~env
        Typing_error.(
          apply_reasons ~on_error
          @@ Secondary.Override_tests_bypass_visibility
               { pos; member_name; parent_pos })
    end else if
          (not is_public_child)
          && not (get_ce_tests_bypass_visibility class_elt)
        then begin
      (* Abstract private methods may be implemented in subclasses, but the
         implementation must opt in as well so the inherited test-only access
         contract is preserved. *)
      let (lazy pos) = class_elt.ce_pos in
      let (lazy parent_pos) = parent_class_elt.ce_pos in
      Typing_error_utils.add_typing_error
        ~env
        Typing_error.(
          apply_reasons ~on_error
          @@ Secondary.Abstract_tests_bypass_visibility_missing_attr
               { pos; member_name; parent_pos })
    end
  end;

  if MemberKind.is_method member_kind then begin
    (* We first verify that we aren't overriding a final method.  We only check
     * for final overrides on methods, not properties. Constructors have their
     * own code-path with this check, see `check_constructors`
     *)
    check_override_final_method env parent_class_elt class_elt on_error;
    check_sealed_allowlist
      env
      member_name
      parent_class
      parent_class_elt
      class_
      class_elt
      on_error
  end;

  (* We check visibility on constructor overrides regardless of consistency,
   * so this enforces the same constraint for package requirements. *)
  if MemberKind.is_functional member_kind then begin
    check_require_package env member_name parent_class_elt class_elt on_error
  end;

  (* Verify that we are not overriding an __LSB property *)
  check_lsb_overrides
    env
    member_kind
    member_name
    parent_class_elt
    class_elt
    on_error;
  check_lateinit env parent_class_elt class_elt on_error;
  check_xhp_attr_required env parent_class_elt class_elt on_error;
  check_class_elt_visibility env parent_class_elt class_elt on_error;
  check_prop_const_mismatch env parent_class_elt class_elt on_error;
  check_abstract_overrides_concrete
    env
    member_kind
    parent_class_elt
    class_elt
    on_error;
  (* On properties the shared flag means readonly, not __NeedsConcrete *)
  if MemberKind.is_method member_kind then
    check_needs_concrete_override
      env
      ~parent_class_elt
      class_
      class_elt
      ~class_pos
      ~member_name
  else if MemberKind.is_property member_kind then
    check_readonly_prop_override
      ~parent_pos
      env
      member_name
      parent_class_elt
      class_elt;

  let (lazy pos) = class_elt.ce_pos in

  let (lazy fty_child) = class_elt.ce_type in
  let (lazy fty_parent) = parent_class_elt.ce_type in
  let snd_err =
    let open Typing_error.Secondary in
    if MemberKind.is_functional member_kind then
      Bad_method_override { pos; member_name }
    else
      Bad_prop_override { pos; member_name }
  in
  (* Modify the `Typing_error.Reasons_callback.t` so that we always end up
     with the error code given by `snd_err` and the reasons of whatever
     error it is applied to are appended to the reasons given by `snd_err`
  *)
  let on_error =
    Typing_error.Reasons_callback.prepend_on_apply on_error snd_err
  in

  if check_member_unique then
    check_multiple_concrete_definitions
      env
      member_name
      member_kind
      (class_elt, class_)
      (parent_class_elt, parent_class)
      on_error;
  check_compatible_sound_dynamic_attributes
    env
    member_name
    member_kind
    parent_class_elt
    class_elt
    on_error;

  match (deref fty_parent, deref fty_child) with
  | ((r_parent, Tfun ft_parent), (r_child, Tfun ft_child)) ->
    (match member_kind with
    | MemberKind.Constructor { is_consistent = false } ->
      (* we don't check that constructor signatures follow
       * subtyping rules except with __ConsistentConstruct *)
      env
    | _ ->
      maybe_poison_ancestors
        env
        ft_parent
        ft_child
        parent_class
        class_
        class_elt.ce_origin
        member_name
        member_kind;
      check_async
        env
        ft_parent
        ft_child
        (Typing_reason.to_pos r_parent)
        (Typing_reason.to_pos r_child)
        on_error;
      let check_subtype_methods env on_error x =
        let on_error =
          Typing_error.Reasons_callback.prepend_on_apply on_error
          @@ bad_member_override_not_subtype
               ~is_method:(MemberKind.is_functional member_kind)
               env
               ~class_
               ~parent_class
               ~parent_type
               ~member_name
               ~class_elt
               ~parent_class_elt
        in
        check_subtype_methods
          env
          ~check_return:(not (MemberKind.is_constructor member_kind))
          on_error
          x
      in
      let env =
        check_ambiguous_inheritance
          (check_subtype_methods env on_error)
          (Typing_reason.localize r_parent, ft_parent)
          (Typing_reason.localize r_child, ft_child)
          pos
          class_
          class_elt.ce_origin
          on_error
          ~env
      in
      env)
  | _ ->
    let (env, ty_err_opt) =
      if get_ce_const class_elt then
        Phase.sub_type_decl env fty_child fty_parent @@ Some on_error
      else
        Typing_ops.unify_decl
          pos
          Typing_reason.URnone
          env
          on_error
          fty_parent
          fty_child
    in
    Option.iter ~f:(Typing_error_utils.add_typing_error ~env) ty_err_opt;
    env

(* Constants and type constants with declared values in declared interfaces
 * can never be overridden by other inherited constants.
 * @precondition: both constants must not be synthesized
 *)
let constant_conflict_with_declared_interface_or_trait
    ?(include_traits = true)
    env
    implements
    parent_class
    class_
    parent_origin
    origin
    const_name =
  let is_inherited_and_conflicts_with_parent =
    String.( <> ) origin (Cls.name class_) && String.( <> ) origin parent_origin
  in
  let child_const_from_used_trait =
    if include_traits then
      match Env.get_class env origin with
      | Decl_entry.Found cls -> Cls.kind cls |> Ast_defs.is_c_trait
      | Decl_entry.DoesNotExist
      | Decl_entry.NotYetAvailable ->
        false
    else
      false
  in

  (* True if a declared interface on class_ has a concrete constant with
     the same name and origin as child constant *)
  let child_const_from_declared_interface =
    match Env.get_class env origin with
    | Decl_entry.Found cls ->
      Cls.kind cls |> Ast_defs.is_c_interface
      &&
      if include_traits then
        true
      else
        List.fold implements ~init:false ~f:(fun acc (_, iface) ->
            acc
            ||
            match Cls.get_const iface const_name with
            | None -> false
            | Some const -> String.( = ) const.cc_origin origin)
    | Decl_entry.DoesNotExist
    | Decl_entry.NotYetAvailable ->
      false
  in

  match Cls.kind parent_class with
  | Ast_defs.Cinterface -> is_inherited_and_conflicts_with_parent
  | Ast_defs.Cclass _ ->
    is_inherited_and_conflicts_with_parent
    && (child_const_from_declared_interface || child_const_from_used_trait)
  | Ast_defs.Ctrait ->
    is_inherited_and_conflicts_with_parent
    && (child_const_from_declared_interface || child_const_from_used_trait)
    &&
    (* constant must be declared on a trait (or interface if include_traits == true) to conflict *)
    (match Env.get_class env parent_origin with
    | Decl_entry.Found cls ->
      if include_traits then
        Cls.kind cls |> fun k ->
        Ast_defs.is_c_trait k || Ast_defs.is_c_interface k
      else
        Cls.kind cls |> Ast_defs.is_c_trait
    | Decl_entry.DoesNotExist
    | Decl_entry.NotYetAvailable ->
      false)
  | Ast_defs.Cenum_class _
  | Ast_defs.Cenum ->
    false

let check_abstract_const_in_concrete_class
    env (class_pos, class_) (const_name, class_const) =
  let is_final = Cls.final class_ in
  if Ast_defs.is_c_concrete (Cls.kind class_) || is_final then
    match class_const.cc_abstract with
    | CCAbstract _ ->
      let trace =
        lazy
          (Ancestor_route.describe_route
             env
             ~classish:(Cls.name class_)
             ~ancestor:class_const.cc_origin)
      in
      Typing_error_utils.add_typing_error
        ~env
        Typing_error.(
          primary
          @@ Primary.Implement_abstract
               {
                 is_final;
                 pos = class_pos;
                 decl_pos = class_const.cc_pos;
                 trace;
                 kind = `const;
                 name = const_name;
                 quickfixes = [];
               })
    | Typing_defs.CCConcrete -> ()

let check_const_override
    env
    implements
    const_name
    parent_class
    (class_pos, class_)
    parent_class_const
    class_const
    on_error =
  if String.equal parent_class_const.cc_origin class_const.cc_origin then (
    check_abstract_const_in_concrete_class
      env
      (class_pos, class_)
      (const_name, class_const);
    env
  ) else
    let parent_kind = Cls.kind parent_class in
    let class_kind = Cls.kind class_ in
    (* Shared preconditions for const_interface_member_not_unique and
       is_bad_interface_const_override *)
    let is_concrete = function
      | CCConcrete -> true
      | CCAbstract _ -> false
    in
    let both_are_non_synthetic_and_concrete =
      (* Synthetic  *)
      (not class_const.cc_synthesized)
      (* The parent we are checking is synthetic, no point in checking *)
      && (not parent_class_const.cc_synthesized)
      (* Only check if parent and child have concrete definitions *)
      && is_concrete class_const.cc_abstract
      && is_concrete parent_class_const.cc_abstract
    in
    let const_interface_or_trait_member_not_unique =
      (* Similar to detect_multiple_concrete_defs, we check if there are multiple
         concrete implementations of class constants with no override.
      *)
      constant_conflict_with_declared_interface_or_trait
        env
        implements
        parent_class
        class_
        parent_class_const.cc_origin
        class_const.cc_origin
        const_name
      && both_are_non_synthetic_and_concrete
    in
    let is_bad_interface_const_override =
      (* HHVM does not support one specific case of overriding constants:
         If the original constant was defined as non-abstract in an interface or trait,
         it cannot be overridden when implementing or extending that interface or using that trait. *)
      if Ast_defs.is_c_interface parent_kind then
        both_are_non_synthetic_and_concrete
        (* Check that the constant is indeed defined in class_ *)
        && String.( = ) class_const.cc_origin (Cls.name class_)
      else
        false
    in
    let is_abstract_concrete_override =
      match (parent_class_const.cc_abstract, class_const.cc_abstract) with
      | (CCConcrete, CCAbstract _) -> true
      | _ -> false
    in

    let remove_hh_member_of dty =
      match get_node dty with
      | Tapply (_hh_member_of, [_enum; dty]) -> dty
      | _ -> dty
    in
    let class_const_type =
      if Ast_defs.is_c_enum_class class_kind then
        remove_hh_member_of class_const.cc_type
      else
        class_const.cc_type
    in
    let parent_class_const_type =
      if Ast_defs.is_c_enum_class parent_kind then
        remove_hh_member_of parent_class_const.cc_type
      else
        parent_class_const.cc_type
    in

    let ty_err_opt1 =
      if const_interface_or_trait_member_not_unique then
        let snd_err =
          Typing_error.Secondary.Interface_or_trait_const_multiple_defs
            {
              pos = class_const.cc_pos;
              name = const_name;
              origin = class_const.cc_origin;
              parent_pos = parent_class_const.cc_pos;
              parent_origin = parent_class_const.cc_origin;
            }
        in
        Some (Typing_error.apply_reasons ~on_error snd_err)
      else if is_bad_interface_const_override then
        let snd_err =
          Typing_error.Secondary.Concrete_const_interface_override
            {
              pos = class_const.cc_pos;
              name = const_name;
              parent_pos = parent_class_const.cc_pos;
              parent_origin = parent_class_const.cc_origin;
            }
        in
        Some (Typing_error.apply_reasons ~on_error snd_err)
      else if is_abstract_concrete_override then
        let snd_err =
          Typing_error.Secondary.Abstract_concrete_override
            {
              pos = class_const.cc_pos;
              parent_pos = parent_class_const.cc_pos;
              kind = `constant;
            }
        in
        Some
          (Typing_error.assert_in_current_decl
             ~ctx:(Env.get_current_decl_and_file env)
             snd_err)
      else
        None
    in
    Option.iter ty_err_opt1 ~f:(Typing_error_utils.add_typing_error ~env);
    let (env, ty_err_opt2) =
      Phase.sub_type_decl env class_const_type parent_class_const_type
      @@ Some
           (Typing_error.Reasons_callback.class_constant_type_mismatch on_error)
    in
    Option.iter ty_err_opt2 ~f:(Typing_error_utils.add_typing_error ~env);
    env

let check_inherited_member_is_dynamically_callable
    env
    inheriting_class
    parent_class
    (member_kind, member_name, parent_class_elt) =
  let (inheriting_class_pos, inheriting_class) = inheriting_class in
  if
    Cls.get_support_dynamic_type inheriting_class
    && not (Cls.get_support_dynamic_type parent_class)
    (* TODO: ideally refactor so the last test is not systematically performed on all methods *)
  then
    match Cls.kind parent_class with
    | Ast_defs.Cclass _
    | Ast_defs.Ctrait -> begin
      match member_kind with
      | MemberKind.Method ->
        if not (Typing_defs.get_ce_support_dynamic_type parent_class_elt) then
          (* since the attribute is missing run the inter check *)
          let (lazy (ty : decl_ty)) = parent_class_elt.ce_type in
          (match get_node ty with
          | Tfun fun_ty ->
            if
              not
                (Typing_dynamic.sound_dynamic_interface_check_from_fun_ty
                   ~this_class:(Some parent_class)
                   env
                   fun_ty)
            then
              Diagnostics.method_is_not_dynamically_callable
                inheriting_class_pos
                member_name
                (Cls.name inheriting_class)
                false
                (Some
                   ( Lazy.force parent_class_elt.ce_pos,
                     parent_class_elt.ce_origin ))
                None
          | _ -> ())
      | MemberKind.Static_method
      | MemberKind.Static_property
      | MemberKind.Property
      | MemberKind.Constructor _ ->
        ()
    end
    | Ast_defs.Cinterface
    | Ast_defs.Cenum_class _
    | Ast_defs.Cenum ->
      ()

let eager_resolve_member_via_req_class
    env parent_class_elt class_ member_kind member_name =
  let member_element_opt = get_member member_kind class_ member_name in
  let req_class_constraints = Cls.all_ancestor_req_class_requirements class_ in
  if List.is_empty req_class_constraints then
    (* fast path: if class_ does not have require class constraints then eager resolution cannot apply *)
    member_element_opt
  else
    Option.map member_element_opt ~f:(fun member_element ->
        (* eager resolution should happen only if one of the matched elements is defined in an interface
         * so check the kind of the classish where the elements are defined
         *)
        let origin_is_interface el =
          match Env.get_class env el.ce_origin with
          | Decl_entry.DoesNotExist
          | Decl_entry.NotYetAvailable ->
            false
          | Decl_entry.Found el -> Ast_defs.is_c_interface (Cls.kind el)
        in
        let parent_element_origin_is_interface =
          origin_is_interface parent_class_elt
        in
        let element_origin_is_interface = origin_is_interface member_element in
        if
          Ast_defs.is_c_trait (Cls.kind class_)
          && (parent_element_origin_is_interface || element_origin_is_interface)
        then
          if String.equal member_element.ce_origin (Cls.name class_) then
            member_element
          else
            (* Since at least one of the elements is not defined in a trait, and the base trait has a
             * require class constraint, perform eager fetch of the element via the required class.
             *)
            let member_element_in_req_class =
              List.find_map req_class_constraints ~f:(fun (_, req_ty) ->
                  let (_, (_, cn), _) = TUtils.unwrap_class_type req_ty in
                  Decl_provider.get_class (Env.get_ctx env) cn
                  |> Decl_entry.to_option
                  >>= fun cnc -> get_member member_kind cnc member_name)
            in
            match member_element_in_req_class with
            | Some member_element_in_req_class ->
              {
                member_element_in_req_class with
                ce_flags =
                  Typing_defs_flags.ClassElt.set_synthesized
                    member_element_in_req_class.ce_flags;
              }
            | None -> member_element
        else
          member_element)

let check_class_against_parent_class_elt
    (on_error : Pos.t * string -> Typing_error.Reasons_callback.t)
    (class_pos, class_)
    member_kind
    member_name
    {
      ParentClassElt.class_elt = parent_class_elt;
      parent = { ParentClassElt.parent_pos; parent_class; parent_type };
      errors_if_not_overriden;
    }
    env : missing_member_info list * Typing_env_types.env =
  add_member_dep
    env
    class_
    parent_class
    (member_kind, member_name, parent_class_elt.ce_origin);

  let member_element_opt =
    (* If a trait does not define the element itself, but will inherit the element from a
     * require class constraint then eagerly compare the element from the required class
     * against the parent class elements.  This is useful to eagerly solve conflicts between
     * interfaces implemented by the trait.
     * However, the eager resolution should not be applied to solve conflicts due to multiple
     * definitions in traits used by the trait, as HHVM does not perform the eager resolution
     * and fatals when flattening the trait methods.
     *)
    eager_resolve_member_via_req_class
      env
      parent_class_elt
      class_
      member_kind
      member_name
  in
  match member_element_opt with
  | Some class_elt ->
    if String.equal parent_class_elt.ce_origin class_elt.ce_origin then (
      (* Case where the child's element comes from the parent being checked. *)
      (* if the child class implements dynamic, all inherited methods should be dynamically callable *)
      check_inherited_member_is_dynamically_callable
        env
        (class_pos, class_)
        parent_class
        (member_kind, member_name, parent_class_elt);
      errors_if_not_overriden
      |> List.iter ~f:(fun err ->
             err |> Lazy.force |> Typing_error_utils.add_typing_error ~env);

      let is_final = Cls.final class_ in
      let missing_members =
        if
          (Ast_defs.is_c_concrete (Cls.kind class_) || is_final)
          && Typing_defs_flags.ClassElt.is_abstract class_elt.ce_flags
        then
          [
            {
              member_name;
              parent_class_elt;
              parent_pos;
              member_kind;
              is_override = true;
            };
          ]
        else
          []
      in

      (missing_members, env)
    ) else
      (* We can skip this check if the class elements have the same origin, as we are
         essentially comparing a method against itself *)
      ( [],
        check_override
          ~check_member_unique:true
          env
          ~member_name
          member_kind
          ~class_
          ~parent_class
          ~parent_class_elt
          ~parent_type
          ~parent_pos
          ~class_elt
          ~class_pos
          (on_error (parent_pos, Cls.name parent_class)) )
  | None ->
    (* The only case when a member belongs to a parent but not the child is if the parent is an
       interface and the child is a concrete class. Otherwise, the member would have been inherited.
       In this case, this is an error because the concrete class fails to implement the parent interface. *)
    ( [
        {
          member_name;
          parent_class_elt;
          parent_pos;
          member_kind;
          is_override = false;
        };
      ],
      env )

(**
 * [check_static_member_intersection class_ class_pos parent_members] looks for
 * intersections in the static and instance members of [class_] (at [class_pos])
 * via a precomputed list of class members in [parent_members]. We emit an
 * error if there exists a class member that is defined as both static, and
 * instance, as it will unconditionally fatal in HHVM. For example,
 * the following code will fatal:
 *
 *   abstract class Foo { public int $bar; }
 *   trait Baz { public static int $bar; }
 *   final class Quxx extends Foo { use Baz; }
 *)
let check_static_member_intersection
    env
    (class_ : Cls.t)
    (class_pos : Pos.t)
    (parent_members : ParentClassEltSet.t MemberNameMap.t MemberKindMap.t) =
  let check_single_member
      (member_kind : MemberKind.t)
      (name : MemberNameMap.key)
      (parent_members : ParentClassEltSet.t)
      (acc :
        (MemberNameMap.key * Typing_defs.class_elt * Typing_defs.class_elt) list)
      =
    let name =
      match member_kind with
      | MemberKind.Property -> String.chop_prefix_if_exists ~prefix:"$" name
      | MemberKind.Static_property -> "$" ^ name
      | MemberKind.Static_method
      | MemberKind.Method
      | MemberKind.Constructor _ ->
        name
    in
    match get_member member_kind class_ name with
    | None -> acc
    | Some class_elt ->
      ( name,
        (ParentClassEltSet.choose parent_members).ParentClassElt.class_elt,
        class_elt )
      :: acc
  in
  let gather_violations parent_member_kind child_member_kind =
    MemberKindMap.find_opt parent_member_kind parent_members
    |> Option.value ~default:MemberNameMap.empty
    |> fun map ->
    MemberNameMap.fold (check_single_member child_member_kind) map []
  in
  let on_error ~member_name ~static_elem ~instance_elem ~kind =
    Typing_error_utils.add_typing_error ~env
    @@ Typing_error.(
         primary
         @@ Primary.Static_instance_intersection
              {
                class_pos;
                instance_pos = instance_elem.ce_pos;
                static_pos = static_elem.ce_pos;
                member_name;
                kind;
              })
  in
  let find_intersections static_member_kind instance_member_kind kind =
    let violations =
      gather_violations static_member_kind instance_member_kind
    in
    List.iter violations ~f:(fun (member_name, static_elem, instance_elem) ->
        on_error ~member_name ~static_elem ~instance_elem ~kind);
    let violations =
      gather_violations instance_member_kind static_member_kind
    in
    List.iter violations ~f:(fun (member_name, instance_elem, static_elem) ->
        on_error ~member_name ~static_elem ~instance_elem ~kind)
  in
  find_intersections MemberKind.Static_method MemberKind.Method `meth;
  find_intersections MemberKind.Static_property MemberKind.Property `prop;
  ()

let check_members_from_all_parents
    env
    ((class_pos : Pos.t), class_)
    (on_error : Pos.t * string -> Typing_error.Reasons_callback.t)
    (parent_members : ParentClassEltSet.t MemberNameMap.t MemberKindMap.t) =
  let check member_kind member_map (acc, env) =
    let check member_name class_elts (acc, env) =
      let check elt (acc, env) =
        let (missing, env) =
          check_class_against_parent_class_elt
            on_error
            (class_pos, class_)
            member_kind
            member_name
            elt
            env
        in
        (acc @ missing, env)
      in
      Worker_cancel.raise_if_stop_requested ();
      ParentClassEltSet.fold ~f:check class_elts ~init:(acc, env)
    in
    MemberNameMap.fold check member_map (acc, env)
  in
  let (missing_members, env) =
    MemberKindMap.fold check parent_members ([], env)
  in

  members_missing_error env class_pos class_ missing_members;
  check_static_member_intersection env class_ class_pos parent_members;
  env

let make_all_members ~parent_class =
  let wrap_constructor (ctor, kind) =
    ( MemberKind.Constructor { is_consistent = constructor_is_consistent kind },
      ctor
      |> Option.map ~f:(fun x -> (SN.Members.__construct, x))
      |> Option.to_list )
  in
  [
    (MemberKind.Property, Cls.props parent_class);
    (MemberKind.Static_property, Cls.sprops parent_class);
    (MemberKind.Method, Cls.methods parent_class);
    (MemberKind.Static_method, Cls.smethods parent_class);
    Cls.construct parent_class |> wrap_constructor;
  ]

(* The phantom class element that represents the default constructor:
 * public function __construct()[] {}
 *
 * It isn't added to the tc_construct only because that's used to
 * determine whether a child class needs to call parent::__construct *)
let default_constructor_ce class_ =
  let (pos, name) = (Cls.pos class_, Cls.name class_) in
  let r = Reason.witness_from_decl pos in
  (* reason doesn't get used in, e.g. arity checks *)
  let ft =
    {
      ft_tparams = [];
      ft_where_constraints = [];
      ft_params = [];
      ft_implicit_params = { capability = CapTy (MakeType.mixed r) };
      ft_ret = MakeType.void r;
      ft_flags = Typing_defs_flags.Fun.default;
      ft_instantiated = true;
    }
  in
  {
    ce_visibility = Vpublic;
    ce_type = lazy (mk (r, Tfun ft));
    ce_origin = name;
    ce_deprecated = None;
    ce_pos = lazy pos;
    ce_overlapping_tparams = None;
    ce_package_requirement = Some RPNormal;
    ce_sealed_allowlist = None;
    ce_flags =
      make_ce_flags
        ~xhp_attr:None
        ~abstract:false
        ~final:false
        ~const:false
        ~lateinit:false
        ~superfluous_override:false
        ~lsb:false
        ~synthesized:true
        ~dynamicallycallable:false
        ~readonly_prop_or_needs_concrete:false
        ~support_dynamic_type:false
        ~needs_init:false
        ~safe_global_variable:false
        ~no_auto_likes:false
        ~tests_bypass_visibility:false;
  }

(* When an interface defines a constructor, we check that they are compatible *)
let check_constructors
    ~parent_pos
    env
    (parent_class, parent_type)
    class_
    ~class_pos
    psubst
    on_error =
  let parent_is_interface = Ast_defs.is_c_interface (Cls.kind parent_class) in
  let parent_is_consistent =
    constructor_is_consistent (snd (Cls.construct parent_class))
  in
  let env =
    if parent_is_interface || parent_is_consistent then
      match (fst (Cls.construct parent_class), fst (Cls.construct class_)) with
      | (Some parent_cstr, _) when get_ce_synthesized parent_cstr -> env
      | (Some parent_cstr, None) ->
        let (lazy pos) = parent_cstr.ce_pos in
        Typing_error_utils.add_typing_error
          ~env
          Typing_error.(
            apply_reasons ~on_error @@ Secondary.Missing_constructor pos);
        env
      | (_, Some cstr) when get_ce_superfluous_override cstr ->
        (* <<__UNSAFE_Construct>> *)
        env
      | (opt_parent_cstr, Some cstr)
        when Option.is_some opt_parent_cstr || parent_is_consistent ->
        let parent_cstr =
          match opt_parent_cstr with
          | Some parent_cstr -> parent_cstr
          | None -> default_constructor_ce parent_class
        in
        if String.( <> ) parent_cstr.ce_origin cstr.ce_origin then
          let parent_cstr = Inst.instantiate_ce psubst parent_cstr in
          check_override
            env
            ~check_member_unique:false
            ~member_name:SN.Members.__construct
            (MemberKind.Constructor { is_consistent = true })
            ~class_
            ~parent_class
            ~parent_type
            ~parent_pos
            ~parent_class_elt:parent_cstr
            ~class_pos
            ~class_elt:cstr
            on_error
        else
          env
      | (_, _) -> env
    else
      env
  in
  begin
    match (fst (Cls.construct parent_class), fst (Cls.construct class_)) with
    | (Some parent_cstr, _) when get_ce_synthesized parent_cstr -> ()
    | (Some parent_cstr, Some child_cstr) ->
      check_override_final_method env parent_cstr child_cstr on_error
    | (_, _) -> ()
  end;
  env

(** Checks if a child is compatible with the type constant of its parent.
    This requires the child's constraint and assigned type to be a subtype of
    the parent's type constant. *)
let tconst_subsumption
    env
    class_name
    parent_typeconst
    parent_tconst_enforceable
    child_typeconst
    on_error =
  let (pos, name) = child_typeconst.ttc_name in
  let parent_pos = fst parent_typeconst.ttc_name in
  match (parent_typeconst.ttc_kind, child_typeconst.ttc_kind) with
  | ( TCAbstract { atc_default = Some _; _ },
      TCAbstract { atc_default = None; _ } ) ->
    Typing_error_utils.add_typing_error
      ~env
      Typing_error.(
        assert_in_current_decl ~ctx:(Env.get_current_decl_and_file env)
        @@ Secondary.Override_no_default_typeconst { pos; parent_pos });
    env
  | (TCConcrete _, TCAbstract _) ->
    (* It is valid for abstract class to extend a concrete class, but it cannot
     * redefine already concrete members as abstract.
     * See typecheck/tconst/subsume_tconst5.php test case for example. *)
    Typing_error_utils.add_typing_error
      ~env
      Typing_error.(
        apply_reasons ~on_error
        @@ Secondary.Abstract_concrete_override
             { pos; parent_pos; kind = `typeconst });
    env
  | _ ->
    let inherited = not (String.equal child_typeconst.ttc_origin class_name) in
    (* If the class element is inherited from a parent class, we must
     * wrap any error with
     *   "Class [class_name] does not correctly implement all required members"
     * and the primary position should be on [class_name]
     *)
    let on_error =
      if inherited then
        on_error
      else
        Env.unify_error_assert_primary_pos_in_current_decl env
    in

    (* Check that the child's constraint is compatible with the parent. If the
     * parent has a constraint then the child must also have a constraint if it
     * is abstract.
     *
     * Check that the child's assigned type satisifies parent constraint
     *)
    let default =
      MakeType.generic (Reason.tconst_no_cstr child_typeconst.ttc_name) name
    in
    let is_coeffect =
      parent_typeconst.ttc_is_ctx || child_typeconst.ttc_is_ctx
    in
    let check_cstrs reason env sub super =
      Option.value ~default:(env, None)
      @@ Option.map2
           sub
           super
           ~f:
             (Typing_ops.sub_type_decl
                ~is_coeffect
                ~class_sub_classname:false
                ~on_error
                pos
                reason
                env)
    in
    (* TODO(T88552052) This can be greatly simplified by adopting the { A = S..T } representation
     * from DOT and implementing the Typ-<:-Typ rule, Amin 2016 *)
    let env =
      match parent_typeconst.ttc_kind with
      | TCAbstract
          {
            atc_as_constraint = p_as_opt;
            atc_super_constraint = p_super_opt;
            _;
          } -> begin
        match child_typeconst.ttc_kind with
        | TCAbstract
            {
              atc_as_constraint = c_as_opt;
              atc_super_constraint = c_super_opt;
              _;
            } ->
          (* TODO(T88552052) this transformation can be done with mixed and nothing *)
          let c_as_opt = Some (Option.value c_as_opt ~default) in
          let c_super_opt = Some (Option.value c_super_opt ~default) in

          let (env, e1) =
            check_cstrs Reason.URsubsume_tconst_cstr env c_as_opt p_as_opt
          in
          let (env, e2) =
            check_cstrs Reason.URsubsume_tconst_cstr env p_super_opt c_super_opt
          in
          let ty_err_opt = Option.merge e1 e2 ~f:Typing_error.both in
          Option.iter ~f:(Typing_error_utils.add_typing_error ~env) ty_err_opt;
          env
        | TCConcrete { tc_type = c_t } ->
          let (env, e1) =
            check_cstrs Reason.URtypeconst_cstr env (Some c_t) p_as_opt
          in
          let (env, e2) =
            check_cstrs Reason.URtypeconst_cstr env p_super_opt (Some c_t)
          in
          let ty_err_opt = Option.merge e1 e2 ~f:Typing_error.both in
          Option.iter ~f:(Typing_error_utils.add_typing_error ~env) ty_err_opt;
          env
      end
      | TCConcrete _ ->
        begin
          match child_typeconst.ttc_kind with
          | TCConcrete _ ->
            if
              TCO.typeconst_concrete_concrete_error (Env.get_tcopt env)
              && not inherited
            then
              Typing_error_utils.add_typing_error
                ~env
                Typing_error.(
                  assert_in_current_decl
                    ~ctx:(Env.get_current_decl_and_file env)
                  @@ Secondary.Typeconst_concrete_concrete_override
                       { pos; parent_pos })
          | _ -> ()
        end;
        env
    in

    (match (child_typeconst.ttc_kind, parent_tconst_enforceable) with
    | (TCAbstract { atc_default = Some ty; _ }, (tp_pos, true))
    | (TCConcrete { tc_type = ty }, (tp_pos, true)) ->
      let emit_error pos ty_info =
        Typing_error_utils.add_typing_error
          ~env
          Typing_error.(
            primary
            @@ Primary.Invalid_enforceable_type
                 { pos; ty_info; kind = `constant; tp_pos; tp_name = name })
      in
      Typing_enforceable_hint.validate_type
        env
        (fst child_typeconst.ttc_name |> Pos_or_decl.unsafe_to_raw_pos)
        ty
        emit_error
    | _ ->
      if not inherited then (
        match parent_typeconst.ttc_reifiable with
        | None -> ()
        | Some pos ->
          Typing_const_reifiable.check_reifiable env child_typeconst pos
      ));

    (* If the parent cannot be overridden, we unify the types otherwise we ensure
     * the child's assigned type is compatible with the parent's
     *
     * TODO(T88552052) restrict concrete typeconst overriding
     *)
    let parent_is_final =
      match parent_typeconst.ttc_kind with
      | TCConcrete _ -> true
      | TCAbstract _ -> false
    in
    let check env x y =
      if parent_is_final then
        Typing_ops.unify_decl
          pos
          Reason.URsubsume_tconst_assign
          env
          on_error
          x
          y
      else
        Typing_ops.sub_type_decl
          ~on_error
          pos
          Reason.URsubsume_tconst_assign
          env
          y
          x
    in
    (* TODO(T88552052) this fetching of types is a temporary hack; this whole check will be eliminated *)
    let opt_type__LEGACY t =
      match t.ttc_kind with
      | TCConcrete { tc_type = t } -> Some t
      | TCAbstract _ -> None
    in
    let (env, ty_err_opt) =
      Option.value ~default:(env, None)
      @@ Option.map2
           (opt_type__LEGACY parent_typeconst)
           (opt_type__LEGACY child_typeconst)
           ~f:(check env)
    in
    Option.iter ty_err_opt ~f:(Typing_error_utils.add_typing_error ~env);
    env

let check_abstract_typeconst_in_concrete_class env (class_pos, class_) tconst =
  let is_final = Cls.final class_ in
  if Ast_defs.is_c_concrete (Cls.kind class_) || is_final then
    match tconst.ttc_kind with
    | TCAbstract _ ->
      let (typeconst_pos, typeconst_name) = tconst.ttc_name in
      let trace =
        lazy
          (Ancestor_route.describe_route
             env
             ~classish:(Cls.name class_)
             ~ancestor:tconst.ttc_origin)
      in
      Typing_error_utils.add_typing_error
        ~env
        Typing_error.(
          primary
          @@ Primary.Implement_abstract
               {
                 is_final;
                 pos = class_pos;
                 decl_pos = typeconst_pos;
                 trace;
                 kind = `ty_const;
                 name = typeconst_name;
                 quickfixes = [];
               })
    | TCConcrete _ -> ()

let check_typeconst_override
    env
    implements
    (class_pos, class_)
    parent_tconst
    tconst
    parent_class
    on_error =
  if String.equal parent_tconst.ttc_origin tconst.ttc_origin then (
    check_abstract_typeconst_in_concrete_class env (class_pos, class_) tconst;
    env
  ) else
    (* If the class element is defined in the class that we're checking, then
     * don't wrap with the extra
     * "Class ... does not correctly implement all required members" message *)
    let on_error =
      if String.equal tconst.ttc_origin (Cls.name class_) then
        Env.unify_error_assert_primary_pos_in_current_decl env
      else
        on_error
    in
    let tconst_check parent_tconst tconst () =
      let parent_tconst_enforceable =
        (* We know that this typeconst exists in the parent (else we would not
           have successfully looked up [parent_tconst]), so we know that
           [get_typeconst_enforceability] will return Some. *)
        Option.value_exn
          (Cls.get_typeconst_enforceability parent_class (snd tconst.ttc_name))
      in
      tconst_subsumption
        env
        (Cls.name class_)
        parent_tconst
        parent_tconst_enforceable
        tconst
        on_error
    in
    let env =
      check_ambiguous_inheritance
        tconst_check
        parent_tconst
        tconst
        (fst tconst.ttc_name)
        class_
        tconst.ttc_origin
        on_error
        ~env
    in
    let (pos, name) = tconst.ttc_name in
    let parent_pos = fst parent_tconst.ttc_name in
    (* Temporarily skip checks on context constants
     *
     * TODO(T89366955) elimninate this check *)
    let is_context_constant =
      match (parent_tconst.ttc_kind, tconst.ttc_kind) with
      | ( TCAbstract { atc_default = Some hint1; _ },
          TCAbstract { atc_default = Some hint2; _ } ) ->
        (match (deref hint1, deref hint2) with
        | ((_, Tintersection _), _)
        | (_, (_, Tintersection _)) ->
          true
        | _ -> false)
      | (TCAbstract { atc_default = Some hint; _ }, _)
      | (_, TCAbstract { atc_default = Some hint; _ }) ->
        (match deref hint with
        | (_, Tintersection _) -> true
        | _ -> false)
      | _ -> false
    in
    (match (parent_tconst.ttc_kind, tconst.ttc_kind) with
    | (TCConcrete _, TCConcrete _)
    | ( TCAbstract { atc_default = Some _; _ },
        TCAbstract { atc_default = Some _; _ } ) ->
      if
        (not is_context_constant)
        && (not tconst.ttc_synthesized)
        && (not parent_tconst.ttc_synthesized)
        && constant_conflict_with_declared_interface_or_trait
             ~include_traits:false
             env
             implements
             parent_class
             class_
             parent_tconst.ttc_origin
             tconst.ttc_origin
             name
      then
        let child_is_abstract = is_typeconst_type_abstract tconst in
        let err =
          Typing_error.Secondary.Interface_typeconst_multiple_defs
            {
              pos;
              name;
              is_abstract = child_is_abstract;
              origin = tconst.ttc_origin;
              parent_pos;
              parent_origin = parent_tconst.ttc_origin;
            }
        in
        Typing_error_utils.add_typing_error ~env
        @@ Typing_error.(apply_reasons ~on_error err)
    | _ -> ());
    env

(* Use the [on_error] callback if we need to wrap the basic error with a
 *   "Class ... does not correctly implement all required members"
 * message pointing at the class being checked.
 *)
let check_class_extends_parent_constructors
    ~parent_pos
    env
    (parent_class : (Pos.t * string) * decl_ty * decl_ty list * Cls.t)
    class_
    ~class_pos
    on_error =
  let (_, parent_type, parent_tparaml, parent_class) = parent_class in
  let psubst = Inst.make_subst (Cls.tparams parent_class) parent_tparaml in
  let env =
    check_constructors
      ~parent_pos
      env
      (parent_class, parent_type)
      class_
      ~class_pos
      psubst
      on_error
  in
  env

(** Eliminate all synthesized members (those from requirements) plus
all private members unless they're from traits. *)
let filter_privates_and_synthethized
    ~(is_trait : bool) (members : ('a * class_elt) list) : ('a * class_elt) list
    =
  let eliminate class_elt =
    get_ce_synthesized class_elt
    || ((not is_trait) && Typing_defs.class_elt_is_private_not_lsb class_elt)
  in
  let keep class_elt = not (eliminate class_elt) in
  List.filter members ~f:(fun (_name, class_elt) -> keep class_elt)

let make_parent_member_map parent :
    ParentClassElt.parent * class_elt MemberNameMap.t MemberKindMap.t =
  let ((parent_pos, _parent_name), parent_type, parent_tparaml, parent_class) =
    parent
  in
  let psubst = Inst.make_subst (Cls.tparams parent_class) parent_tparaml in
  let member_map =
    make_all_members ~parent_class
    |> MemberKindMap.of_list
    |> MemberKindMap.map (fun members ->
           members
           |> filter_privates_and_synthethized
                ~is_trait:(Ast_defs.is_c_trait (Cls.kind parent_class))
           |> S_map.of_list
           |> S_map.map (Inst.instantiate_ce psubst))
  in
  ({ ParentClassElt.parent_pos; parent_class; parent_type }, member_map)

(** Check for multiple kinds of forbidden hierarchy diamonds involving traits:
  - Any kind of diamond involving final methods is forbidden unless the class has __EnableMethodTraitDiamond
    and the diamond involves only traits.
  - Diamonds involving only traits, with the trait at the top of the diamond containing at least one method,
    are only allowed if the classish has __EnableMethodTraitDiamond attribute.
  - Any kind of diamond involving properties with a generic type instantiated differently along the
    two paths of the diamond is forbidden.
  [check_trait_diamonds env ~allow_diamonds ~class_name ~member_name elt elts member_kind] considers [elt] and [elts].
  Members from [elt] union [elts] have the same name [member_name]. [elts] represents all the members with this member name
  that we've seen so far from the parents we are merging. In this function, we'll detect diamonds by checking if
  [elt] has the same origin as one of the elements in [elts] and produce an error if that diamond is forbidden. *)
let check_trait_diamonds
    env
    ~(allow_diamonds : bool)
    ~class_name
    ~member_name
    ((class_elt, { ParentClassElt.parent_pos; parent_class; parent_type = _ })
    as parent_class_elt)
    elts
    member_kind :
    (ParentClassElt.t * ParentClassEltSet.t)
    * ((string * string) * Typing_error.t) option =
  let default_parent_class_elt () = ParentClassElt.make parent_class_elt in
  if
    (* We'll check some of those conditions later but check them first
       here to avoid an expensive set lookup if we can. *)
    (* Note that we never check that the origin of class_elt is a trait, because that
       would require a decl heap lookup which is expensive. But that should follow from the
       following facts: 1) the parent from which class_elt comes from is a trait - 2) the member
       is not synthetic, i.e. not from a `require extends`, since we've filtered those out earlier -
       3) the member is not abstract, so not from a `require implements` either. *)
    Ast_defs.is_c_trait (Cls.kind parent_class)
    && (not (get_ce_abstract class_elt))
    && (MemberKind.is_method member_kind
        && ((not allow_diamonds) || get_ce_final class_elt)
       || MemberKind.is_property member_kind)
  then
    (* Let's search for a diamond. *)
    (* We want to find an existing element with the same origin,
     * i.o.w, find a diamond.
     * Set.find_first_opt only works for 'monotonic' `f` which is
     * not the case for the equality test, and Set.find_opt
     * uses the polymorphic equaliy: we only want to test for origin.
     * Set doesn't provide a `find with condition` so we do an iteration
     * and bail at the first occurrence we spot
     *
     * This works in a deterministic way because the set we are working
     * with has the invariant that each element have a unique origin.
     *)
    let candidate =
      ParentClassEltSet.find_one_opt
        elts
        ~f:(fun { ParentClassElt.class_elt = prev_class_elt; _ } ->
          String.equal class_elt.ce_origin prev_class_elt.ce_origin)
    in
    match candidate with
    | None -> ((default_parent_class_elt (), elts), None)
    | Some
        ({
           ParentClassElt.class_elt = prev_class_elt;
           parent = { ParentClassElt.parent_class = prev_parent; _ };
           errors_if_not_overriden = err;
         } as prev_parent_class_elt) ->
      if
        MemberKind.is_method member_kind
        && get_ce_final class_elt
        && ((not allow_diamonds) || Cls.kind prev_parent |> Ast_defs.is_c_class)
      then
        let error =
          (* We want only one such error per diamond (instead of one per diamond per final method),
             so let's return the diamond corners and the error and let the caller aggregate per diamond. *)
          Some
            ( (Cls.name prev_parent, class_elt.ce_origin),
              Trait_reuse_check.trait_reuse_with_final_method_error
                env
                class_elt
                ~class_name:(snd class_name)
                ~first_using_parent_or_trait:prev_parent
                ~second_using_trait:(parent_pos, parent_class) )
        in
        (* Let's keep the previous parent class element, whose parent is a class,
           to detect additional such errors if there are more diamonds. *)
        ((prev_parent_class_elt, elts), error)
      else if
        MemberKind.is_method member_kind
        && (not allow_diamonds)
        && Cls.kind prev_parent |> Ast_defs.is_c_trait
      then
        let parent_class_elt =
          ParentClassElt.make
            parent_class_elt
            ~errors_if_not_overriden:
              (lazy
                 (Trait_reuse_check.method_import_via_diamond_error
                    env
                    class_name
                    (member_name, class_elt)
                    ~first_using_trait:prev_parent
                    ~second_using_trait:parent_class)
              :: err)
        in
        let elts =
          ParentClassEltSet.remove elts (default_parent_class_elt ())
        in
        ((parent_class_elt, elts), None)
      else if MemberKind.is_property member_kind then
        if allow_diamonds then begin
          (* traits with properties cannot be inherited via multiple paths
             if the base class has the <<__EnableMethodTraitDiamond>> attribute *)
          Trait_reuse_check.property_import_via_diamond_error
            ~generic:false
            env
            class_name
            (member_name, class_elt)
            ~first_using_trait:prev_parent
            ~second_using_trait:parent_class;
          ((default_parent_class_elt (), elts), None)
        end else if
              not
                (ty_equal
                   (Lazy.force class_elt.ce_type)
                   (Lazy.force prev_class_elt.ce_type))
            then begin
          (* it is unsound to use a trait that defines a generic property
             at different types along multiple paths *)
          Trait_reuse_check.property_import_via_diamond_error
            ~generic:true
            env
            class_name
            (member_name, class_elt)
            ~first_using_trait:prev_parent
            ~second_using_trait:parent_class;
          ((default_parent_class_elt (), elts), None)
        end else
          ((default_parent_class_elt (), elts), None)
      else
        (* When we have a diamond between a class parent and a used trait
           (e.g. D extends C { use TraitA; use TraitB; } where C also uses
           TraitA), we need to keep the trait's entry rather than the class
           parent's entry. *)
        let elts =
          if Ast_defs.is_c_class (Cls.kind prev_parent) then
            ParentClassEltSet.remove elts (default_parent_class_elt ())
          else
            elts
        in
        ((default_parent_class_elt (), elts), None)
  else
    ((default_parent_class_elt (), elts), None)

(** Selects the minimum classes out of a list using the partial ordering
  provided by the subtyping relationship. *)
let minimum_classes env classes =
  let is_sub_type x y =
    Decl_provider.get_class (Env.get_ctx env) x
    |> Decl_entry.to_option
    >>= (fun x -> Cls.get_ancestor x y)
    |> Option.is_some
  in
  let add_min is_lower x mins =
    let rec go mins result =
      match mins with
      | [] -> x :: result
      | y :: ys ->
        if is_lower x y then
          go ys result
        else if is_lower y x then
          List.rev_append result mins
        else
          go ys (y :: result)
    in
    go mins []
  in
  List.fold classes ~init:[] ~f:(fun minimum_classes class_ ->
      add_min is_sub_type class_ minimum_classes)

let check_no_conflicting_inherited_concrete_constants
    env name constants class_pos =
  let open ParentClassConst in
  let definitions =
    ParentClassConstSet.filter
      (fun { class_const = { cc_abstract; _ }; _ } ->
        match cc_abstract with
        | CCConcrete -> true
        | CCAbstract _ -> false)
      constants
    |> ParentClassConstSet.elements
  in
  if List.length definitions > 1 then
    let err =
      Typing_error.Primary.Constant_multiple_concrete_conflict
        {
          pos = class_pos;
          name;
          definitions =
            List.map
              ~f:(fun { class_const; parent } ->
                let parent_name = Cls.name parent.ParentClassElt.parent_class in
                let via =
                  if String.(parent_name <> class_const.cc_origin) then
                    Some (Utils.strip_ns parent_name)
                  else
                    None
                in
                (class_const.cc_pos, via))
              definitions;
        }
    in
    Typing_error_utils.add_typing_error ~env (Typing_error.primary err)

(* When a class inherits a concrete type constant from two different points in its hierarchy
 * (e.g. a parent class and an interface) HHVM will fail to load the class, and the flag
 * -vEval.TraitConstantInterfaceBehavior=1 extends this behavior to traits. This function reports
 * such cases, pointing to the definitions of the constants and reporting which parent brings them
 * in, e.g. declared in an interface brought in via a trait use. *)
let check_no_conflicting_inherited_concrete_typeconsts
    env name typeconsts class_pos =
  let open ParentTypeConst in
  let definitions =
    ParentTypeConstSet.filter
      (fun { typeconst = { ttc_kind; ttc_synthesized; _ }; _ } ->
        match ttc_kind with
        | TCConcrete _ -> not ttc_synthesized
        | TCAbstract _ -> false)
      typeconsts
    |> ParentTypeConstSet.elements
  in
  if List.length definitions > 1 then
    let err =
      Typing_error.Primary.Constant_multiple_concrete_conflict
        {
          pos = class_pos;
          name;
          definitions =
            List.map
              ~f:(fun { typeconst; parent } ->
                let parent_name = Cls.name parent.ParentClassElt.parent_class in
                let via =
                  if String.(parent_name <> typeconst.ttc_origin) then
                    Some (Utils.strip_ns parent_name)
                  else
                    None
                in
                (fst typeconst.ttc_name, via))
              definitions;
        }
    in
    Typing_error_utils.add_typing_error ~env (Typing_error.primary err)

let union_parent_constants parents : ParentClassConstSet.t MemberNameMap.t =
  let get_declared_consts
      ((parent_pos, _), parent_type, parent_tparaml, parent_class) :
      ParentClassElt.parent * _ =
    let psubst = Inst.make_subst (Cls.tparams parent_class) parent_tparaml in
    let consts =
      Cls.consts parent_class
      |> List.filter ~f:(fun (_, cc) -> not cc.cc_synthesized)
      |> List.map ~f:(Tuple.T2.map_snd ~f:(Inst.instantiate_cc psubst))
    in
    ({ ParentClassElt.parent_pos; parent_class; parent_type }, consts)
  in
  parents
  |> List.map ~f:get_declared_consts
  |> List.fold ~init:MemberNameMap.empty ~f:(fun acc (parent, consts) ->
         List.fold
           ~init:acc
           ~f:(fun acc (name, const) ->
             let open ParentClassConstSet in
             let elt = ParentClassConst.make const parent in
             MemberNameMap.update
               name
               (function
                 | None -> Some (singleton elt)
                 | Some elts -> Some (add elt elts))
               acc)
           consts)

let union_parent_typeconsts parents : ParentTypeConstSet.t MemberNameMap.t =
  let get_declared_consts
      ((parent_pos, _), parent_type, parent_tparaml, parent_class) :
      ParentClassElt.parent * _ =
    let psubst = Inst.make_subst (Cls.tparams parent_class) parent_tparaml in
    let typeconsts =
      Cls.typeconsts parent_class
      |> List.map
           ~f:(Tuple.T2.map_snd ~f:(Inst.instantiate_typeconst_type psubst))
    in
    ({ ParentClassElt.parent_pos; parent_class; parent_type }, typeconsts)
  in
  parents
  |> List.map ~f:get_declared_consts
  |> List.fold ~init:MemberNameMap.empty ~f:(fun acc (parent, typeconsts) ->
         List.fold
           ~init:acc
           ~f:(fun acc (name, typeconst) ->
             let open ParentTypeConstSet in
             let elt = ParentTypeConst.make typeconst parent in
             MemberNameMap.update
               name
               (function
                 | None -> Some (singleton elt)
                 | Some elts -> Some (add elt elts))
               acc)
           typeconsts)

let check_class_extends_parents_constants
    env
    implements
    (class_ast, class_)
    (parents : ((Pos.t * string) * decl_ty * decl_ty list * Cls.t) list)
    (on_error : Pos.t * string -> Typing_error.Reasons_callback.t) =
  let constants : ParentClassConstSet.t MemberNameMap.t =
    union_parent_constants parents
  in

  let class_pos = fst class_ast.Aast.c_name in

  MemberNameMap.fold
    (fun const_name inherited env ->
      check_no_conflicting_inherited_concrete_constants
        env
        const_name
        inherited
        class_pos;

      (* TODO(vmladenov): factor individual checks out of check_const_override and remove [implements] parameter *)
      ParentClassConstSet.fold
        (fun ParentClassConst.
               {
                 parent =
                   { ParentClassElt.parent_pos; parent_class; parent_type = _ };
                 class_const = parent_const;
               }
             env ->
          match Cls.get_const class_ const_name with
          | Some const ->
            (* skip checks for typeconst derived class constants *)
            (match Cls.get_typeconst class_ const_name with
            | None ->
              check_const_override
                env
                implements
                const_name
                parent_class
                (class_pos, class_)
                parent_const
                const
                (on_error (parent_pos, Cls.name parent_class))
            | Some _ -> env)
          | None ->
            let err =
              Typing_error.(
                primary
                @@ Primary.Member_not_implemented
                     {
                       pos = parent_pos;
                       member_name = const_name;
                       decl_pos = parent_const.cc_pos;
                       quickfixes = [];
                     })
            in
            Typing_error_utils.add_typing_error ~env err;
            env)
        inherited
        env)
    constants
    env

let check_class_extends_parents_typeconsts
    env
    implements
    (class_ast, class_)
    (parents : ((Pos.t * string) * decl_ty * decl_ty list * Cls.t) list)
    (on_error : Pos.t * string -> Typing_error.Reasons_callback.t) =
  let typeconsts : ParentTypeConstSet.t MemberNameMap.t =
    union_parent_typeconsts parents
  in
  let class_pos = fst class_ast.Aast.c_name in

  MemberNameMap.fold
    (fun tconst_name inherited env ->
      check_no_conflicting_inherited_concrete_typeconsts
        env
        tconst_name
        inherited
        class_pos;

      ParentTypeConstSet.fold
        (fun ParentTypeConst.
               {
                 parent =
                   { ParentClassElt.parent_pos; parent_class; parent_type = _ };
                 typeconst = parent_tconst;
               }
             env ->
          (* If class_ is a trait that has a require class C constraint, and C provides a concrete definition
           * for the type constant, and the parent definition is abstract, then compare the parent type constant
           * against the type constant defined in the required class.
           * Otherwise compare it against the type constant defined in class_, if any.
           * However, if the parent definition is a concrete type constant, then we can skip checking type constants
           * inherited via require class constants, as these must be concrete and a conflict check will be
           * performed on the class itself *)
          let is_parent_tconst_abstract =
            is_typeconst_type_abstract parent_tconst
          in
          let class_tconst_opt =
            let tconst_element = Cls.get_typeconst class_ tconst_name in
            if
              Ast_defs.is_c_trait (Cls.kind class_) && is_parent_tconst_abstract
            then
              Option.map tconst_element ~f:(fun tconst_element ->
                  if String.equal tconst_element.ttc_origin (Cls.name class_)
                  then
                    tconst_element
                  else
                    let tconst_element_in_req_class =
                      List.find_map
                        (Cls.all_ancestor_req_class_requirements class_)
                        ~f:(fun (_, req_ty) ->
                          let (_, (_, cn), _) =
                            TUtils.unwrap_class_type req_ty
                          in
                          Decl_provider.get_class (Env.get_ctx env) cn
                          |> Decl_entry.to_option
                          >>= fun cnc ->
                          (* Since only final classes can satisfy require class constraints, if the type constant is
                           * found in the require class then it must be concrete.  No need to check that here. *)
                          Cls.get_typeconst cnc tconst_name)
                    in
                    match tconst_element_in_req_class with
                    | Some tconst_element_in_req_class ->
                      {
                        tconst_element_in_req_class with
                        ttc_synthesized = true;
                      }
                    | None -> tconst_element)
            else
              tconst_element
          in

          match class_tconst_opt with
          | Some tconst ->
            check_typeconst_override
              env
              implements
              (class_pos, class_)
              parent_tconst
              tconst
              parent_class
              (on_error (parent_pos, Cls.name parent_class))
          | None ->
            (* The only case when a member belongs to a parent but not the child is if the parent is an
               interface and the child is a concrete class. Otherwise, the member would have been inherited. *)
            let err =
              Typing_error.(
                primary
                @@ Primary.Member_not_implemented
                     {
                       pos = parent_pos;
                       member_name = tconst_name;
                       decl_pos = fst parent_tconst.ttc_name;
                       quickfixes = [];
                     })
            in
            Typing_error_utils.add_typing_error ~env err;
            env)
        inherited
        env)
    typeconsts
    env

let merge_member_maps
    env
    ~allow_diamonds
    ~class_name
    (acc_map : ParentClassEltSet.t MemberNameMap.t MemberKindMap.t)
    ((parent, map) :
      ParentClassElt.parent * class_elt MemberNameMap.t MemberKindMap.t) :
    ParentClassEltSet.t MemberNameMap.t MemberKindMap.t =
  let errors_per_diamond = ref S_map.empty in
  let members =
    MemberKindMap.fold
      (fun mem_kind
           (members : class_elt MemberNameMap.t)
           (acc_map : ParentClassEltSet.t MemberNameMap.t MemberKindMap.t) ->
        let members_for_mem_kind =
          match MemberKindMap.find_opt mem_kind acc_map with
          | None ->
            MemberNameMap.map
              (fun elt ->
                ParentClassElt.make (elt, parent) |> ParentClassEltSet.singleton)
              members
          | Some prev_members ->
            let members =
              MemberNameMap.merge
                (fun member_name elt elts ->
                  match (elt, elts) with
                  | (None, None) -> None
                  | (Some elt, None) ->
                    Some
                      (ParentClassElt.make (elt, parent)
                      |> ParentClassEltSet.singleton)
                  | (None, (Some _ as elts)) -> elts
                  | (Some elt, Some elts) ->
                    let ((elt, elts), error) =
                      check_trait_diamonds
                        env
                        ~allow_diamonds
                        ~class_name
                        ~member_name
                        (elt, parent)
                        elts
                        mem_kind
                    in
                    (* We want only one error per diamond, so we aggregate errors in
                       `errors_per_diamond` before adding them later. *)
                    Option.iter error ~f:(fun ((parent, origin), error) ->
                        errors_per_diamond :=
                          S_map.add
                            parent
                            (S_map.add
                               ~combine:(fun x _ -> x)
                               origin
                               error
                               (S_map.find_opt parent !errors_per_diamond
                               |> Option.value ~default:S_map.empty))
                            !errors_per_diamond);
                    Some (ParentClassEltSet.add elts elt))
                members
                prev_members
            in
            members
        in
        MemberKindMap.add mem_kind members_for_mem_kind acc_map)
      map
      acc_map
  in
  S_map.iter
    (fun _ errors_per_origin ->
      S_map.keys errors_per_origin
      |> minimum_classes env
      |> List.iter ~f:(fun origin ->
             S_map.find origin errors_per_origin
             |> Typing_error_utils.add_typing_error ~env))
    !errors_per_diamond;
  members

let union_parent_members env class_ast parents :
    ParentClassEltSet.t MemberNameMap.t MemberKindMap.t =
  let allow_diamonds =
    Naming_attributes.mem
      SN.UserAttributes.uaEnableMethodTraitDiamond
      class_ast.Aast.c_user_attributes
  in
  let class_name = class_ast.Aast.c_name in
  parents
  |> List.map ~f:make_parent_member_map
  |> List.fold
       ~init:MemberKindMap.empty
       ~f:(merge_member_maps env ~allow_diamonds ~class_name)

let check_class_extends_parents_members
    env
    (class_ast, class_)
    (parents : ((Pos.t * string) * decl_ty * decl_ty list * Cls.t) list)
    (on_error : Pos.t * string -> Typing_error.Reasons_callback.t) =
  let members : ParentClassEltSet.t MemberNameMap.t MemberKindMap.t =
    union_parent_members env class_ast parents
  in
  check_members_from_all_parents
    env
    (fst class_ast.Aast.c_name, class_)
    on_error
    members

let check_consts_are_not_abstract
    env ~is_final ~class_name_pos (consts : Nast.class_const list) =
  List.iter consts ~f:(fun const ->
      match const.Aast.cc_kind with
      | Aast.CCAbstract _ ->
        Typing_error_utils.add_typing_error
          ~env
          Typing_error.(
            primary
            @@ Primary.Abstract_member_in_concrete_class
                 {
                   pos = fst const.Aast.cc_id;
                   class_name_pos;
                   is_final;
                   member_kind = `constant;
                   member_name = snd const.Aast.cc_id;
                 })
      | Aast.CCConcrete _ -> ())

let check_typeconsts_are_not_abstract env ~is_final ~class_name_pos typeconsts =
  List.iter typeconsts ~f:(fun tc ->
      match tc.Aast.c_tconst_kind with
      | Aast.TCAbstract _ ->
        Typing_error_utils.add_typing_error
          ~env
          Typing_error.(
            primary
            @@ Primary.Abstract_member_in_concrete_class
                 {
                   pos = fst tc.Aast.c_tconst_name;
                   class_name_pos;
                   is_final;
                   member_kind = `type_constant;
                   member_name = snd tc.Aast.c_tconst_name;
                 })
      | Aast.TCConcrete _ -> ())

let check_properties_are_not_abstract
    env ~is_final ~class_name_pos (properties : Nast.class_var list) =
  List.iter properties ~f:(fun property ->
      if property.Aast.cv_abstract then
        Typing_error_utils.add_typing_error
          ~env
          Typing_error.(
            primary
            @@ Primary.Abstract_member_in_concrete_class
                 {
                   pos = fst property.Aast.cv_id;
                   class_name_pos;
                   is_final;
                   member_kind = `property;
                   member_name = snd property.Aast.cv_id;
                 }))

let check_methods_are_not_abstract
    env ~is_final ~class_name_pos (methods : Nast.method_ list) =
  List.iter methods ~f:(fun (method_ : (unit, unit) Aast.method_) ->
      if method_.Aast.m_abstract then
        Typing_error_utils.add_typing_error
          ~env
          Typing_error.(
            primary
            @@ Primary.Abstract_member_in_concrete_class
                 {
                   pos = fst method_.Aast.m_name;
                   class_name_pos;
                   is_final;
                   member_kind = `method_;
                   member_name = snd method_.Aast.m_name;
                 }))

(** Error if there are abstract members in a concrete class' AST. *)
let check_concrete_has_no_abstract_members env (class_ : Nast.class_) =
  let {
    Aast.c_kind;
    c_final = is_final;
    c_typeconsts;
    c_consts;
    c_vars;
    c_methods;
    c_name = (class_name_pos, _);
    _;
  } =
    class_
  in
  if Ast_defs.is_c_concrete c_kind || is_final then (
    check_consts_are_not_abstract env ~class_name_pos ~is_final c_consts;
    check_typeconsts_are_not_abstract env ~class_name_pos ~is_final c_typeconsts;
    check_properties_are_not_abstract env ~class_name_pos ~is_final c_vars;
    check_methods_are_not_abstract env ~class_name_pos ~is_final c_methods
  );
  (* Checking that a concrete class does not inherit abstract members is checked
   * when checking against a class' parents. *)
  ()

(*****************************************************************************)
(* The externally visible function *)
(*****************************************************************************)

(* [parents] also contains traits.
   Here's a simple example showing why we need to check overriding traits:

     trait T {
       public function foo(): void {}
       public function bar(): void {
         $this->foo();
       }
     }

     class A {
       use T;
       public function foo(int $x): void {}
     }

     Overriding foo this way is unsound due to bar using foo,
     so A::foo needs to be a subtype of T::foo.
*)
let check_implements_extends_uses
    env
    ~implements
    ~(parents : (Aast.hint * Typing_defs.decl_ty) list)
    (class_ast, class_) =
  let (name_pos, name) = class_ast.Aast.c_name in
  let implements =
    let decl_ty_to_cls x =
      let (_, (pos, name), _) = TUtils.unwrap_class_type x in
      Env.get_class env name |> Decl_entry.to_option >>| fun class_ ->
      (pos, class_)
    in
    List.filter_map implements ~f:decl_ty_to_cls
  in
  let on_error (parent_name_pos, parent_name) : Typing_error.Reasons_callback.t
      =
    (* sadly, enum error reporting requires this to keep the error in the file
       with the enum *)
    if String.equal parent_name SN.Classes.cHH_BuiltinEnum then
      Typing_error.Reasons_callback.bad_enum_decl name_pos
    else
      Typing_error.Reasons_callback.bad_decl_override
        ~name
        ~parent_pos:parent_name_pos
        ~parent_name
  in
  let parents =
    let destructure_type ((p, _h), ty) =
      let (_, (_, name), tparaml) = TUtils.unwrap_class_type ty in
      Env.get_class env name |> Decl_entry.to_option >>| fun class_ ->
      ((p, name), ty, tparaml, class_)
    in
    List.filter_map parents ~f:destructure_type
  in
  let env =
    List.fold
      ~init:env
      parents
      ~f:(fun env ((parent_name, _, _, _) as parent) ->
        check_class_extends_parent_constructors
          ~parent_pos:(fst parent_name)
          env
          parent
          ~class_pos:name_pos
          class_
          (on_error parent_name))
  in
  let env =
    check_class_extends_parents_constants
      env
      implements
      (class_ast, class_)
      parents
      on_error
  in
  let env =
    check_class_extends_parents_typeconsts
      env
      implements
      (class_ast, class_)
      parents
      on_error
  in
  let env =
    check_class_extends_parents_members env (class_ast, class_) parents on_error
  in
  check_concrete_has_no_abstract_members env class_ast;
  env
