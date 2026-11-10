(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude

(* open Typing_defs *)
module Env = Typing_env

let is_package_loaded_for_access env package =
  (not package.Package.enable_strict_isolation)
  && Env.is_package_loaded env (Package.get_package_name package)

let get_package_violation env current_pkg target_pkg =
  match (current_pkg, target_pkg) with
  | (_, None) -> None
  | (None, Some target_pkg_info) ->
    if is_package_loaded_for_access env target_pkg_info then
      None
    else
      Some Package.Unrelated
  (* Anyone can call code outside of a package *)
  | (Some current_pkg_info, Some target_pkg_info) ->
    Package.(
      (match relationship current_pkg_info target_pkg_info with
      | Equal
      | Includes ->
        None
      | (Soft_includes | Unrelated) as r ->
        if is_package_loaded_for_access env target_pkg_info then
          None
        else
          Some r))

type package_warning_info = {
  current_package: Package.pos_id option;
  current_package_before_override: string option;
  caller_has_package_override: bool;
  target_package: Package.pos_id option;
  target_package_before_override: string option;
  callee_has_package_override: bool;
  classptr_reference_warning: bool;
}

type package_error_info = {
  current_package: Package.pos_id option;
  current_package_assignment_kind: string;
  target_package: Package.pos_id option;
  target_package_assignment_kind: string;
  target_id: string;
}

type check_reason =
  [ `Yes of Typing_error.Primary.Package.target_symbol_spec
  | `LintOnly
  | `ClassPtrLinterOnly
  | `No
  ]

(* Compiling the exclude patterns is much more expensive than matching them,
   and [is_excluded] runs on every cross-file symbol access, so keep the
   compiled patterns for the last pattern list seen. *)
let compiled_exclude_patterns : (string list * Str.regexp list) option ref =
  ref None

let get_compiled_exclude_patterns patterns =
  match !compiled_exclude_patterns with
  | Some (cached_patterns, regexps)
    when phys_equal cached_patterns patterns
         || List.equal String.equal cached_patterns patterns ->
    regexps
  | Some _
  | None ->
    let regexps = List.map patterns ~f:Str.regexp in
    compiled_exclude_patterns := Some (patterns, regexps);
    regexps

let is_excluded env (file : Relative_path.t) =
  let filename = Relative_path.to_absolute file in
  let excluded_patterns =
    Env.get_tcopt env
    |> Typechecker_options.package_exclude_patterns
    |> get_compiled_exclude_patterns
  in
  List.exists excluded_patterns ~f:(fun regexp ->
      Str.string_match regexp filename 0)

(* package, package info (name * pos), package membership kind *)
let get_package_profile
    (env : Typing_env_types.env)
    (pkg_membership : Aast_defs.package_membership option) :
    Package.t option * Package.pos_id option * string =
  match pkg_membership with
  | Some (Aast_defs.PackageConfigAssignment pkg_name) ->
    let pkg = Env.get_package_by_name env pkg_name in
    let pos =
      match pkg with
      | Some p -> Package.get_package_pos p
      | None -> Pos.none
    in
    (pkg, Some (pos, pkg_name), "entry in the package config")
  | Some (Aast_defs.PackageOverride (pkg_pos, pkg_name)) ->
    ( Env.get_package_by_name env pkg_name,
      Some (pkg_pos, pkg_name),
      "package override" )
  | _ -> (None, None, "")

(* True when [membership] resolves to a package with strict isolation enabled.
   Positions that are exempt from package checks by default (attributes, type
   references, ::class, nameof) consult this to opt strict-isolation targets back
   into enforcement. *)
let is_strict_isolation_target env pkg_membership =
  let (target_pkg, _, _) = get_package_profile env pkg_membership in
  match target_pkg with
  | Some p -> p.Package.enable_strict_isolation
  | None -> false

let package_includes current_pkg target_pkg =
  match (current_pkg, target_pkg) with
  | (Some current_pkg, Some target_pkg) ->
    (match Package.relationship current_pkg target_pkg with
    | Equal
    | Includes ->
      true
    | Soft_includes
    | Unrelated ->
      false)
  | _ -> false

let has_package_override = function
  | Some (Aast_defs.PackageOverride _) -> true
  | Some (Aast_defs.PackageConfigAssignment _)
  | None ->
    false

let package_before_override env file =
  Option.map
    (Package_provider.get_package_for_file
       (Env.get_ctx env)
       ~path:(Relative_path.suffix file))
    ~f:Package.get_package_name

let can_access_ignoring_package_override
    ~(env : Typing_env_types.env)
    ~(current_package : Package.pos_id option)
    ~(current_package_before_override : string option)
    ~(caller_has_package_override : bool)
    ~(target_package : Package.pos_id option)
    ~(callee_has_package_override : bool)
    ~(target_file : Relative_path.t)
    ~(classptr_reference_warning : bool) =
  let target_package_before_override =
    package_before_override env target_file
  in
  let is_target_package_before_override_included =
    match (current_package_before_override, target_package_before_override) with
    | ( Some current_package_before_override_name,
        Some target_package_before_override_name ) ->
      package_includes
        (Env.get_package_by_name env current_package_before_override_name)
        (Env.get_package_by_name env target_package_before_override_name)
    | _ -> true
  in
  let is_target_package_before_override_loaded =
    match target_package_before_override with
    | Some pkg_name -> Env.is_package_loaded env pkg_name
    | None -> true
  in
  if
    is_target_package_before_override_included
    || is_target_package_before_override_loaded
  then
    `Yes
  else
    let warn_info =
      {
        current_package;
        current_package_before_override;
        caller_has_package_override;
        target_package;
        target_package_before_override;
        callee_has_package_override;
        classptr_reference_warning;
      }
    in
    `YesWarning warn_info

let can_access_by_package_rules
    ~(env : Typing_env_types.env)
    ~(target_package_membership : Aast_defs.package_membership option)
    ~(target_pos : Pos_or_decl.t)
    ~(target_id : string) =
  let current_file = Env.get_file env in
  let target_file = Pos_or_decl.filename target_pos in
  (* invariant: if two symbols are in the same file they must be in the same package *)
  let in_same_file =
    String.equal
      (Relative_path.suffix current_file)
      (Relative_path.suffix target_file)
  in
  let accessing_hhi = Pos_or_decl.is_hhi target_pos in
  (* Fast path: same-file and hhi accesses are always allowed. Keep this check
     ahead of the package-profile and is_excluded lookups below, which are
     comparatively expensive (env lookups and regex matching) and run on every
     symbol access -- the same-file case is very common. *)
  if in_same_file || accessing_hhi then
    `Yes
  else
    let current_package_membership = Env.get_current_package_membership env in
    let (current_pkg, current_package, current_package_assignment_kind) =
      get_package_profile env current_package_membership
    in
    let (target_pkg, target_package, target_package_assignment_kind) =
      get_package_profile env target_package_membership
    in
    let target_is_strict_isolation =
      match target_pkg with
      | Some p -> p.Package.enable_strict_isolation
      | None -> false
    in
    let current_excluded = is_excluded env current_file in
    let target_excluded = is_excluded env target_file in
    let mk_err_info () =
      {
        current_package;
        current_package_assignment_kind;
        target_package;
        target_package_assignment_kind;
        target_id;
      }
    in
    if (current_excluded || target_excluded) && not target_is_strict_isolation
    then
      (* Packages without strict isolation: package_exclude_patterns (e.g.
         __tests__) fully exempts the access from package enforcement, in either
         direction. *)
      `Yes
    else
      (* Run the ordinary cross-package rules first. *)
      let standard_result =
        match get_package_violation env current_pkg target_pkg with
        | None ->
          (* No package error, but the edge may be legal only because of a
           * __PackageOverride on one of its endpoints, in which case it is what
           * keeps that override -- and the package bloat it causes -- in
           * place. *)
          let caller_has_package_override =
            has_package_override current_package_membership
          in
          let callee_has_package_override =
            has_package_override target_package_membership
          in
          if not (caller_has_package_override || callee_has_package_override)
          then
            `Yes
          else
            (* Without an override the path-derived package is the one already
               resolved, so skip the lookup. *)
            let current_package_before_override =
              if caller_has_package_override then
                package_before_override env current_file
              else
                Option.map current_package ~f:snd
            in
            can_access_ignoring_package_override
              ~env
              ~current_package
              ~current_package_before_override
              ~caller_has_package_override
              ~target_package
              ~callee_has_package_override
              ~target_file
              ~classptr_reference_warning:false
        | Some pkg_relationship ->
          (match pkg_relationship with
          | Package.Soft_includes -> `PackageSoftIncludes (mk_err_info ())
          | Package.Unrelated -> `PackageNotSatisfied (mk_err_info ())
          | Package.(Equal | Includes) ->
            Utils.assert_false_log_backtrace
              (Some "Package constraints are satisfied with equal and includes"))
      in
      (* Strict isolation adds one boundary the ordinary cross-package rules
         cannot express: within a strict-isolation package, non-excluded code may
         not reference the package's own excluded-path (e.g. __tests__) code. The
         ordinary rules see both files as the same package (an Equal
         relationship) and allow it, so upgrade that -- and only that --
         otherwise-allowed case to an error. A genuine cross-package reference
         keeps its standard violation. *)
      match standard_result with
      | `Yes
        when target_is_strict_isolation
             && target_excluded
             && not current_excluded ->
        `ExcludedPathAccess (mk_err_info ())
      | _ -> standard_result
