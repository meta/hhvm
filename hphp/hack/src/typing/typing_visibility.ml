(*
 * Copyright (c) 2015, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 * *)
open Hh_prelude
open Aast
open Typing_defs
open Utils
open Typing_error.Primary.Modules
open Typing_error.Primary.Package
module Env = Typing_env
module TUtils = Typing_utils
module Cls = Folded_class

type package_access_result =
  | Package_access_ok
  | Package_access_error of Typing_error.t
  | Package_access_linter_error of
      (Pos.t * Typing_packages.package_warning_info)

type tests_bypass_visibility_hint =
  | No_tests_bypass_visibility_hint
  | Add_tests_bypass_visibility_attribute
  | Enable_tests_bypass_visibility_static_properties

(* Is a private member defined on class/trait [origin_id] visible
 * from code in class/trait [self_id]?
 *
 * For almost all circumstances, this is just a case of checking
 * that they are the same, *except* for the special case of
 * where-constraints on the this type defined on a trait, for example:
 *
 * class C { private static function foo():void { } }
 * trait TR where this = C {
 *   ...C::foo...
 * }
 *
 * Using `require_class` also allows a trait to access the private
 * method from the class it requires
 *)
let is_private_visible env origin_id self_id =
  if String.equal origin_id self_id then
    None
  else
    let in_bounds bounds =
      List.exists bounds ~f:(fun t ->
          match get_node t with
          | Tapply ((_, name), _) -> String.equal origin_id name
          | _ -> false)
    in
    match Env.get_class env self_id with
    | Decl_entry.Found cls when Ast_defs.is_c_trait (Cls.kind cls) ->
      (* If the origin class is final and required via `require class` then
       * it is sound to give access to its private members.
       *)
      let bounds_from_require_class_constraints =
        List.map (Cls.all_ancestor_req_class_requirements cls) ~f:snd
      in
      if in_bounds bounds_from_require_class_constraints then
        None
      else
        Some "You cannot access this member"
    | _ -> Some "You cannot access this member"

let is_protected_visible env origin_id self_id =
  if TUtils.has_ancestor_including_req_refl env self_id origin_id then
    None
  else
    match Env.get_class env origin_id with
    | Decl_entry.DoesNotExist
    | Decl_entry.NotYetAvailable ->
      None
    | Decl_entry.Found origin_class ->
      (* Parents can call direct children's protected methods
       * (like a constructor)
       *)
      if Cls.has_ancestor origin_class self_id then
        None
      else
        Some
          ("Cannot access this protected member, you don't extend "
          ^ strip_ns origin_id)

let is_private_visible_for_class env x self_id cid class_ =
  match cid with
  | CIstatic ->
    let my_class = Env.get_class env self_id in
    (match my_class with
    | Decl_entry.Found cls when Cls.final cls -> None
    | _ ->
      Some
        "Private members cannot be accessed with static:: since a child class may also have an identically named private member")
  | CIparent -> Some "You cannot access a private member with parent::"
  | CIself -> is_private_visible env x self_id
  | CI (_, called_ci) ->
    (match is_private_visible env x self_id with
    | None -> None
    | Some _ -> begin
      match Env.get_class env called_ci with
      | Decl_entry.Found cls when Ast_defs.is_c_trait (Cls.kind cls) ->
        Some
          "You cannot access private members using the trait's name (did you mean to use self::?)"
      | _ -> Some "You cannot access this member"
    end)
  | CIreified _ -> Some "You cannot access this member"
  | CIexpr _ ->
    if Cls.final class_ && String.equal self_id (Cls.name class_) then
      None
    else
      Some
        "Private members cannot be accessed dynamically. Did you mean to use 'self::'?"

let is_internal_visible env target =
  match
    Typing_modules.can_access_internal
      ~env
      ~current:(Env.get_current_module env)
      ~target:(Some target)
  with
  | `Yes -> None
  | `Disjoint (current, target) ->
    Some
      (Printf.sprintf
         "You cannot access internal members from module `%s` in module `%s`"
         target
         current)
  | `Outside _ -> Some "You cannot access internal members outside of a module"
  | `OutsideViaTrait _ ->
    Some "You cannot access internal members inside a public trait"

let check_internal_access
    ~in_signature ~tests_bypass_visibility_context env target pos decl_pos =
  let module_err_opt =
    match
      Typing_modules.can_access_internal
        ~env
        ~current:(Env.get_current_module env)
        ~target
    with
    | `Yes when in_signature && not (Env.get_internal env) ->
      Some (Module_hint { pos; decl_pos })
    | `Yes -> None
    | `Disjoint (current, target) ->
      Some
        (Module_mismatch
           {
             pos;
             decl_pos;
             current_module_opt = Some current;
             target_module = target;
             tests_bypass_visibility_context;
           })
    | `Outside target ->
      Some
        (Module_mismatch
           {
             pos;
             decl_pos;
             current_module_opt = None;
             target_module = target;
             tests_bypass_visibility_context;
           })
    | `OutsideViaTrait trait_pos ->
      Some (Module_unsafe_trait_access { access_pos = pos; trait_pos })
  in
  Option.map ~f:Typing_error.modules module_err_opt

let check_package_access
    ~should_check_package_boundary
    ~use_pos
    ~def_pos
    env
    target_package_membership
    target_id =
  match should_check_package_boundary with
  | `No -> Package_access_ok
  | `Yes target_symbol_spec ->
    let loaded_packages = Env.get_loaded_packages_list env in
    let included_packages = Env.get_included_packages env in
    begin
      match
        Typing_packages.can_access_by_package_rules
          ~env
          ~target_package_membership
          ~target_pos:def_pos
          ~target_id
      with
      | `Yes -> Package_access_ok
      | `YesWarning w -> Package_access_linter_error (use_pos, w)
      | `PackageNotSatisfied
          Typing_packages.
            {
              current_package;
              current_package_assignment_kind;
              target_package;
              target_package_assignment_kind;
              target_id;
            } ->
        Package_access_error
          (Typing_error.package
             (Cross_pkg_access
                {
                  pos = use_pos;
                  decl_pos = def_pos;
                  current_filename = Pos.filename use_pos;
                  target_filename = Pos_or_decl.filename def_pos;
                  current_package;
                  current_package_assignment_kind;
                  target_package;
                  target_package_assignment_kind;
                  target_id;
                  target_symbol_spec;
                  loaded_packages;
                  included_packages;
                }))
      | `PackageSoftIncludes
          Typing_packages.
            {
              current_package;
              current_package_assignment_kind;
              target_package;
              target_package_assignment_kind;
              target_id;
            } ->
        Package_access_error
          (Typing_error.package
             (Soft_included_access
                {
                  pos = use_pos;
                  decl_pos = def_pos;
                  current_filename = Pos.filename use_pos;
                  target_filename = Pos_or_decl.filename def_pos;
                  current_package;
                  current_package_assignment_kind;
                  target_package;
                  target_package_assignment_kind;
                  target_id;
                  target_symbol_spec;
                  loaded_packages;
                  included_packages;
                }))
      | `ExcludedPathAccess Typing_packages.{ target_package; target_id; _ } ->
        Package_access_error
          (Typing_error.package
             (Excluded_path_access
                {
                  pos = use_pos;
                  decl_pos = def_pos;
                  package = target_package;
                  target_filename = Pos_or_decl.filename def_pos;
                  target_id;
                  target_symbol_spec;
                }))
    end
  | `LintOnly -> begin
    match
      Typing_packages.can_access_by_package_rules
        ~env
        ~target_package_membership
        ~target_pos:def_pos
        ~target_id
    with
    | `Yes -> Package_access_ok
    | `YesWarning w -> Package_access_linter_error (use_pos, w)
    | `PackageNotSatisfied _ -> Package_access_ok
    | `PackageSoftIncludes _ -> Package_access_ok
    | `ExcludedPathAccess _ -> Package_access_ok
  end
  | `ClassPtrLinterOnly ->
    let current_file = Env.get_file env in
    let target_file = Pos_or_decl.filename def_pos in
    if
      Typing_packages.is_excluded env current_file
      || Typing_packages.is_excluded env target_file
    then
      Package_access_ok
    else
      let current_package =
        Option.map ~f:(fun p -> p.Package.name) (Env.get_current_package env)
      in
      let current_package_membership = Env.get_current_package_membership env in
      let check_classptr_access () =
        let (_, target_package, _) =
          Typing_packages.get_package_profile env target_package_membership
        in
        match
          Typing_packages.can_access_ignoring_package_override
            ~env
            ~current_package
            ~current_package_before_override:(Option.map current_package ~f:snd)
            ~caller_has_package_override:
              (Typing_packages.has_package_override current_package_membership)
            ~target_package
            ~callee_has_package_override:
              (Typing_packages.has_package_override target_package_membership)
            ~target_file:(Pos_or_decl.filename def_pos)
            ~classptr_reference_warning:true
        with
        | `Yes -> Package_access_ok
        | `YesWarning w -> Package_access_linter_error (use_pos, w)
      in
      (match current_package_membership with
      | Some (Aast_defs.PackageConfigAssignment _) -> check_classptr_access ()
      | Some (Aast_defs.PackageOverride (_, current_override_pkg)) ->
        (* When source has PackageOverride, only warn if target does NOT have
           PackageOverride to the same package. If target also overrides to the
           same package, both are effectively in the same package. *)
        (match target_package_membership with
        | Some (Aast_defs.PackageOverride (_, target_override_pkg))
          when String.equal current_override_pkg target_override_pkg ->
          (* Both override to the same package, no lint needed *)
          Package_access_ok
        | _ ->
          (* Target has no override, or overrides to a different package.
             Check if source's override package includes target's original package. *)
          check_classptr_access ())
      | None -> Package_access_ok)

let is_visible_for_obj ~is_method ~is_receiver_interface env vis =
  let member_ty =
    if is_method then
      "method"
    else
      "property"
  in
  let check_protected x =
    if is_receiver_interface then
      Some "You cannot invoke a protected method on an interface"
    else
      match Env.get_self_id env with
      | None -> Some ("You cannot access this " ^ member_ty)
      | Some self_id -> is_protected_visible env x self_id
  in
  match vis with
  | Vpublic -> None
  | Vprivate x ->
    (match Env.get_self_id env with
    | None -> Some ("You cannot access this " ^ member_ty)
    | Some self_id -> is_private_visible env x self_id)
  | Vprotected x -> check_protected x
  | Vinternal m -> is_internal_visible env m
  | Vprotected_internal { class_id; module_ } ->
    Option.merge
      (check_protected class_id)
      (is_internal_visible env module_)
      ~f:(fun s1 s2 -> s1 ^ "\n" ^ s2)

(* The only permitted way to access an LSB property is via
   static::, ClassName::, or $class_name:: *)
let is_lsb_permitted cid =
  match cid with
  | CIself -> Some "__LSB properties cannot be accessed with self::"
  | CIparent -> Some "__LSB properties cannot be accessed with parent::"
  | _ -> None

(* LSB property accessibility is relative to the defining class *)
let is_lsb_accessible env vis =
  let check_protected x =
    match Env.get_self_id env with
    | None -> Some "You cannot access this property"
    | Some self_id -> is_protected_visible env x self_id
  in
  match vis with
  | Vpublic -> None
  | Vprivate x ->
    (match Env.get_self_id env with
    | None -> Some "You cannot access this property"
    | Some self_id -> is_private_visible env x self_id)
  | Vprotected x -> check_protected x
  | Vinternal m -> is_internal_visible env m
  | Vprotected_internal { class_id; module_ } ->
    Option.merge
      (check_protected class_id)
      (is_internal_visible env module_)
      ~f:(fun s1 s2 -> s1 ^ "\n" ^ s2)

let is_lsb_visible_for_class env vis cid =
  match is_lsb_permitted cid with
  | Some x -> Some x
  | None -> is_lsb_accessible env vis

let is_visible_for_class ~is_method env (vis, lsb) cid cty =
  if lsb then
    is_lsb_visible_for_class env vis cid
  else
    let member_ty =
      if is_method then
        "method"
      else
        "property"
    in
    let check_protected x =
      match Env.get_self_id env with
      | None -> Some ("You cannot access this " ^ member_ty)
      | Some self_id ->
        let their_class = Env.get_class env x in
        (match (cid, their_class) with
        | (CI _, Decl_entry.Found cls) when Ast_defs.is_c_trait (Cls.kind cls)
          ->
          Some
            "You cannot access protected members using the trait's name (did you mean to use static:: or self::?)"
        | _ -> is_protected_visible env x self_id)
    in
    match vis with
    | Vpublic -> None
    | Vprivate x ->
      (match Env.get_self_id env with
      | None -> Some ("You cannot access this " ^ member_ty)
      | Some self_id -> is_private_visible_for_class env x self_id cid cty)
    | Vprotected x -> check_protected x
    | Vinternal m -> is_internal_visible env m
    | Vprotected_internal { class_id; module_ } ->
      Option.merge
        (check_protected class_id)
        (is_internal_visible env module_)
        ~f:(fun s1 s2 -> s1 ^ "\n" ^ s2)

let is_visible ~is_method env (vis, lsb) cid class_ =
  let msg_opt =
    match cid with
    | Some cid -> is_visible_for_class ~is_method env (vis, lsb) cid class_
    | None -> is_visible_for_obj ~is_method ~is_receiver_interface:false env vis
  in
  Option.is_none msg_opt

let visibility_error ~tests_bypass_visibility_hint p msg (p_vis, vis) =
  let s = Typing_defs.string_of_visibility vis in
  let hint =
    match tests_bypass_visibility_hint with
    | No_tests_bypass_visibility_hint -> ""
    | Add_tests_bypass_visibility_attribute ->
      ". To allow access from test contexts, add `<<__TestsBypassVisibility>>` to this member"
    | Enable_tests_bypass_visibility_static_properties ->
      ". To allow static property access from test contexts, set `tests_bypass_visibility_static_properties = true` in `.hhconfig`"
  in
  let msg_vis = "This member is " ^ s ^ hint in
  Typing_error.(
    primary
    @@ Primary.Visibility
         { pos = p; msg; decl_pos = p_vis; reason_msg = msg_vis })

let check_obj_access
    ~is_method
    ~is_receiver_interface
    ~use_pos
    ~def_pos
    ~tests_bypass_visibility
    env
    vis =
  let tests_bypass_visibility_context =
    TUtils.is_tests_bypass_visibility_context env
  in
  if tests_bypass_visibility && tests_bypass_visibility_context then
    []
  else
    let check_visibility visibility_to_check =
      Option.map
        (is_visible_for_obj
           ~is_method
           ~is_receiver_interface
           env
           visibility_to_check)
        ~f:(fun msg ->
          let tests_bypass_visibility_hint =
            if tests_bypass_visibility_context then
              Add_tests_bypass_visibility_attribute
            else
              No_tests_bypass_visibility_hint
          in
          visibility_error
            ~tests_bypass_visibility_hint
            use_pos
            msg
            (def_pos, vis))
    in
    let module_error =
      match vis with
      | Vinternal m ->
        check_internal_access
          ~in_signature:false
          ~tests_bypass_visibility_context
          env
          (Some m)
          use_pos
          def_pos
      | Vprotected_internal { module_; _ } ->
        check_internal_access
          ~in_signature:false
          ~tests_bypass_visibility_context
          env
          (Some module_)
          use_pos
          def_pos
      | _ -> None
    in
    let vis_without_internal =
      match vis with
      | Vinternal _ -> Vpublic
      | Vprotected_internal { class_id; _ } -> Vprotected class_id
      | other -> other
    in
    List.filter_map
      ~f:Fn.id
      [module_error; check_visibility vis_without_internal]

let check_top_level_access
    ~should_check_package_boundary
    ~in_signature
    ~use_pos
    ~def_pos
    ~tests_bypass_visibility
    ~target_supports_tests_bypass_visibility
    env
    is_internal
    target_module
    target_package
    target_id =
  let in_test_context = TUtils.is_tests_bypass_visibility_context env in
  let module_error =
    if is_internal && not (tests_bypass_visibility && in_test_context) then
      let tests_bypass_visibility_context =
        target_supports_tests_bypass_visibility
        && (not tests_bypass_visibility)
        && in_test_context
      in
      check_internal_access
        ~in_signature
        ~tests_bypass_visibility_context
        env
        target_module
        use_pos
        def_pos
    else
      None
  in
  let package_error =
    check_package_access
      ~should_check_package_boundary
      ~use_pos
      ~def_pos
      env
      target_package
      target_id
  in
  match (module_error, package_error) with
  | (Some e1, Package_access_error e2) -> ([e1; e2], [])
  | (Some e, Package_access_linter_error w) -> ([e], [w])
  | (Some e, Package_access_ok) -> ([e], [])
  | (None, Package_access_error e) -> ([e], [])
  | (None, Package_access_linter_error w) -> ([], [w])
  | (None, Package_access_ok) -> ([], [])

let check_expression_tree_vis ~use_pos ~def_pos env vis =
  let open Typing_error in
  if Env.is_in_expr_tree env then
    match vis with
    | Vpublic -> None
    | _ ->
      Some
        (expr_tree
        @@ Primary.Expr_tree.Expression_tree_non_public_member
             { pos = use_pos; decl_pos = def_pos })
  else
    None

let check_meth_caller_access ~use_pos ~def_pos ~tests_bypass_visibility env vis
    =
  if tests_bypass_visibility && TUtils.is_tests_bypass_visibility_context env
  then
    None
  else
    let open Typing_error in
    match vis with
    | Vprivate _ ->
      Some
        (primary
        @@ Primary.Private_meth_caller { decl_pos = def_pos; pos = use_pos })
    | Vprotected _ ->
      Some
        (primary
        @@ Primary.Protected_meth_caller { decl_pos = def_pos; pos = use_pos })
    | Vinternal _ ->
      Some
        (primary
        @@ Primary.Internal_meth_caller { decl_pos = def_pos; pos = use_pos })
    | Vprotected_internal _ ->
      Some
        (primary
        @@ Primary.Protected_internal_meth_caller
             { decl_pos = def_pos; pos = use_pos })
    | _ -> None

let check_class_access
    ~is_method
    ~use_pos
    ~def_pos
    ~tests_bypass_visibility
    ~tests_bypass_visibility_static_properties_blocked
    env
    (vis, lsb)
    cid
    class_ =
  if tests_bypass_visibility && TUtils.is_tests_bypass_visibility_context env
  then
    []
  else
    let tests_bypass_visibility_context =
      TUtils.is_tests_bypass_visibility_context env
    in
    let check_visibility visibility_to_check =
      Option.map
        (is_visible_for_class
           ~is_method
           env
           (visibility_to_check, lsb)
           cid
           class_)
        ~f:(fun msg ->
          let tests_bypass_visibility_hint =
            if tests_bypass_visibility_context then
              if tests_bypass_visibility_static_properties_blocked then
                Enable_tests_bypass_visibility_static_properties
              else
                Add_tests_bypass_visibility_attribute
            else
              No_tests_bypass_visibility_hint
          in
          visibility_error
            ~tests_bypass_visibility_hint
            use_pos
            msg
            (def_pos, vis))
    in
    let module_error =
      match vis with
      | Vinternal m ->
        check_internal_access
          ~in_signature:false
          ~tests_bypass_visibility_context
          env
          (Some m)
          use_pos
          def_pos
      | Vprotected_internal { module_; _ } ->
        check_internal_access
          ~in_signature:false
          ~tests_bypass_visibility_context
          env
          (Some module_)
          use_pos
          def_pos
      | _ -> None
    in
    let vis_without_internal =
      match vis with
      | Vinternal _ -> Vpublic
      | Vprotected_internal { class_id; _ } -> Vprotected class_id
      | other -> other
    in
    List.filter_map
      ~f:Fn.id
      [module_error; check_visibility vis_without_internal]

let check_cross_package ~use_pos ~def_pos:_ env package_requirement =
  let current_pkg = Env.get_current_package env in
  let current_file = Env.get_file env in

  let can_call_in_continuation target =
    let target_pkg = Env.get_package_by_name env target in
    Typing_packages.get_package_violation env current_pkg target_pkg
  in

  let in_exclude_patterns target_pos =
    let target_file = Pos_or_decl.filename target_pos in
    let accessing_hhi = Pos_or_decl.is_hhi target_pos in
    accessing_hhi
    || Typing_packages.is_excluded env current_file
    || Typing_packages.is_excluded env target_file
  in

  match package_requirement with
  | Some (RPRequire (target_pos, target)) ->
    if in_exclude_patterns target_pos then
      None
    else (
      match can_call_in_continuation target with
      | Some _ ->
        (* Convert from Package.t option to (string * Pos.t) option *)
        let current_package =
          Option.map current_pkg ~f:(fun pkg -> pkg.Package.name)
        in
        Some
          (Typing_error.package
             (Cross_pkg_access_with_requirepackage
                {
                  pos = use_pos;
                  decl_pos = target_pos;
                  target_package = target;
                  current_package;
                  loaded_packages = Env.get_loaded_packages_list env;
                  included_packages = Env.get_included_packages env;
                }))
      | _ -> None
    )
  | Some (RPSoft (soft_pos, soft)) ->
    if in_exclude_patterns soft_pos then
      None
    else (
      (* If the per-continuation environment does not provide the softly-required
       * package, we have one more option -- a __SoftRequirePackage function may
       * call another __SoftRequirePackage function to aid monolithic migration
       * to __RequirePackage. In the soft-soft case, the choice of error is
       * arbitrary; we'll focus on the soft annotation rather than the per-cont
       * environment packages. *)
      match can_call_in_continuation soft with
      | Some _ ->
        (match Env.get_soft_package_requirement env with
        | Some (env_soft_pos, env_soft) ->
          let env_soft_pkg = Env.get_package_by_name env env_soft in
          let soft_pkg = Env.get_package_by_name env soft in
          (match
             Typing_packages.get_package_violation env env_soft_pkg soft_pkg
           with
          | Some _ ->
            Some
              (Typing_error.package
                 (Cross_pkg_access_with_softrequirepackage
                    {
                      pos = use_pos;
                      decl_pos = soft_pos;
                      current_soft_package_opt = Some (env_soft_pos, env_soft);
                      target_package = soft;
                      loaded_packages = Env.get_loaded_packages_list env;
                      included_packages = Env.get_included_packages env;
                    }))
          | None -> None)
        | None ->
          Some
            (Typing_error.package
               (Cross_pkg_access_with_softrequirepackage
                  {
                    pos = use_pos;
                    decl_pos = soft_pos;
                    current_soft_package_opt = None;
                    target_package = soft;
                    loaded_packages = Env.get_loaded_packages_list env;
                    included_packages = Env.get_included_packages env;
                  })))
      | _ -> None
    )
  | Some RPNormal -> None
  | None -> None

let check_deprecated ~use_pos ~def_pos env deprecated =
  if Tast.is_under_dynamic_assumptions env.Typing_env_types.checked then
    None
  else
    Option.map deprecated ~f:(fun s ->
        Typing_error.(
          primary
          @@ Primary.Deprecated_use
               { pos = use_pos; decl_pos_opt = Some def_pos; msg = s }))

(** Check if a declaration is gated behind a feature flag via
    [__GatedByFeatureFlag('feature')].  Emits error 4509 if the feature
    is not enabled.  [attrs] are the user attributes from the already-loaded
    decl so no additional decl fetch is needed. *)
let check_gated_by_feature_flag env pos name attrs =
  let feature_opt =
    List.find_map attrs ~f:(fun ua ->
        if
          String.equal
            (snd ua.Typing_defs_core.ua_name)
            Naming_special_names.UserAttributes.uaGatedByFeatureFlag
        then
          List.find_map ua.Typing_defs_core.ua_params ~f:(function
              | Typing_defs_core.String s -> Some s
              | _ -> None)
        else
          None)
  in
  Option.iter feature_opt ~f:(fun feature_name ->
      let tcopt = Env.get_tcopt env in
      if
        not (Typechecker_options.is_unstable_feature_enabled tcopt feature_name)
      then
        Typing_error_utils.add_typing_error
          ~env
          Typing_error.(
            primary
            @@ Primary.Gated_by_feature_flag
                 {
                   pos = Pos_or_decl.unsafe_to_raw_pos pos;
                   name;
                   feature = feature_name;
                 }))
