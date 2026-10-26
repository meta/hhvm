(*
 * Copyright (c) 2015, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude
open Common
open Decl_env
open Typing_env_types
open Typing_defs_constraints
open Typing_defs
open Aast
module Inf = Typing_inference_env
module LID = Local_id
module LEnvC = Typing_per_cont_env
module C = Typing_continuations
module Cls = Folded_class
module Fake = Typing_fake_members
module ITySet = Internal_type_set
module TPEnv = Type_parameter_env
module KDefs = Typing_kinding_defs
module TySet = Typing_set

(* Trick that enables us to expose separate subsets of our API via the .mli *)
module M = struct
  type env = Typing_env_types.env

  type 'a class_or_typedef_result =
        'a Decl_enforceability.class_or_typedef_result =
    | ClassResult of 'a
    | TypedefResult of Typing_defs.typedef_type

  let show_env _ = "<env>"

  let pp_env _ _ = Printf.printf "%s\n" "<env>"

  (** see .mli **)
  let get_tcopt env = env.genv.tcopt

  let map_tcopt env ~f =
    let tcopt = f env.genv.tcopt in
    let genv = { env.genv with tcopt } in
    { env with genv }

  let get_deps_mode env =
    Provider_context.get_deps_mode env.decl_env.Decl_env.ctx

  let get_ctx env = env.decl_env.Decl_env.ctx

  let get_file env = env.genv.file

  let get_current_decl_and_file env : Pos_or_decl.ctx =
    { Pos_or_decl.file = get_file env; decl = Deps.get_current_decl env }

  let fill_in_pos_filename_if_in_current_decl env pos =
    Pos_or_decl.fill_in_filename_if_in_current_decl
      ~current_decl_and_file:(get_current_decl_and_file env)
      pos

  let unify_error_assert_primary_pos_in_current_decl env =
    Typing_error.Reasons_callback.unify_error_assert_primary_pos_in_current_decl
      (get_current_decl_and_file env)

  let invalid_type_hint_assert_primary_pos_in_current_decl env =
    Typing_error.Reasons_callback
    .invalid_type_hint_assert_primary_pos_in_current_decl
      (get_current_decl_and_file env)

  let get_tracing_info env = env.tracing_info

  let set_log_level env key log_level =
    { env with log_levels = S_map.add key log_level env.log_levels }

  let get_log_level = Typing_env_types.get_log_level

  let log_env_change_ :
      type res. string -> ?level:int -> env -> env * res -> env * res =
   fun function_name ?(level = 1) old_env (new_env, res) ->
    (if
     get_log_level new_env function_name >= 1
     || get_log_level new_env "env" >= level
    then
      let pos =
        Option.value
          (Inf.get_current_pos_from_tyvar_stack old_env.inference_env)
          ~default:old_env.genv.callable_pos
      in
      Typing_log.log_env_diff pos ~function_name old_env new_env);
    (new_env, res)

  let log_env_change name ?(level = 1) old_env new_env =
    let (env, ()) = log_env_change_ name ~level old_env (new_env, ()) in
    env

  let expand_var env r v =
    let (inference_env, ty_solution) = Inf.expand_var env.inference_env r v in
    ({ env with inference_env }, ty_solution)

  let rank_of_tvar { inference_env; _ } tvid = Inf.get_rank inference_env tvid

  let fresh_type_reason ?variance env p r =
    log_env_change_ "fresh_type_reason" env
    @@
    let (inference_env, res) =
      Inf.fresh_type_reason ?variance env.inference_env env.tvar_id_provider p r
    in
    ({ env with inference_env }, res)

  let fresh_type env p =
    log_env_change_ "fresh_type" env
    @@
    let (inference_env, res) =
      Inf.fresh_type env.inference_env env.tvar_id_provider p
    in
    ({ env with inference_env }, res)

  let fresh_type_invariant_with_rank env rank p =
    log_env_change_ "fresh_type_invariant_with_rank" env
    @@
    let (inference_env, res) =
      Inf.fresh_type_invariant_with_rank
        env.inference_env
        env.tvar_id_provider
        rank
        p
    in
    ({ env with inference_env }, res)

  let fresh_type_invariant env p =
    log_env_change_ "fresh_type_invariant" env
    @@
    let (inference_env, res) =
      Inf.fresh_type_invariant env.inference_env env.tvar_id_provider p
    in
    ({ env with inference_env }, res)

  let fresh_type_error env p =
    log_env_change_ "fresh_type_error" env
    @@
    let (inference_env, res) =
      Inf.fresh_type_reason
        ~variance:Ast_defs.Invariant
        ~is_error:true
        env.inference_env
        env.tvar_id_provider
        p
        (Reason.type_variable_error p)
    in
    ({ env with inference_env }, res)

  let fresh_type_error_contravariant env p =
    log_env_change_ "fresh_type_error_contravariant" env
    @@
    let (inference_env, res) =
      Inf.fresh_type_reason
        ~variance:Ast_defs.Contravariant
        ~is_error:true
        env.inference_env
        env.tvar_id_provider
        p
        (Reason.type_variable_error p)
    in
    ({ env with inference_env }, res)

  let add_subtype_prop env prop =
    log_env_change "add_subtype_prop" env
    @@ { env with inference_env = Inf.add_subtype_prop env.inference_env prop }

  let empty_bounds = TySet.empty

  let tyvar_is_solved env var = Inf.tyvar_is_solved env.inference_env var

  let make_tyvar_no_more_occur_in_tyvar env v ~no_more_in:v' =
    {
      env with
      inference_env =
        Inf.make_tyvar_no_more_occur_in_tyvar env.inference_env v ~no_more_in:v';
    }

  let not_implemented s _ =
    failwith (Printf.sprintf "Function %s not implemented" s)

  type simplify_unions = env -> locl_ty -> env * locl_ty

  let (simplify_unions_ref : simplify_unions ref) =
    ref (not_implemented "simplify_unions")

  let simplify_unions x = !simplify_unions_ref x

  let bind env v ty =
    { env with inference_env = Inf.bind env.inference_env v ty }

  (** Unions and intersections containing unsolved type variables may remain
    in an unsimplified form once those type variables get solved.

    For example, consider the union (#1 | int) where #1 is an unsolved type variable.
    If #1 gets solved to int, then this union will remain in the unsimplified form
    (int | int) which compromise the robustness of some of our logic and might
    cause performance issues (by creating big unsimplified unions).

    To solve this problem, we wrap each union and intersection in a type var,
    so we'd get `#2 -> (#1 | int)` (This is done in Typing_union and
    Typing_intersection), and register that #1 occurs in #2 in
    [env.tyvar_occurrences]. Then when #1 gets solved, we simplify #2.

    This function deals with this simplification.

    The simplification is recursive: simplifying a type variable will
    trigger simplification of its own occurrences. *)
  let simplify_occurrences env v r =
    let rec simplify_occurrences env v ~seen_tyvars =
      let vars = Inf.get_tyvar_occurrences env.inference_env v in
      let (env, seen_tyvars) =
        Tvid.Set.fold
          (fun v' (env, seen_tyvars) ->
            (* This type variable is now solved and does not contain any unsolved
               type variable, so we can remove it from its occurrences. *)
            let env = make_tyvar_no_more_occur_in_tyvar env v ~no_more_in:v' in
            (* Only simplify when the type of v' does not contain any more
               unsolved type variables. *)
            if not @@ Inf.contains_unsolved_tyvars env.inference_env v' then
              simplify_type_of_var env v' ~seen_tyvars
            else
              (env, seen_tyvars))
          vars
          (env, seen_tyvars)
      in
      (env, seen_tyvars)
    and simplify_type_of_var env v ~seen_tyvars =
      if Tvid.Set.mem v seen_tyvars then
        (* TODO raise exception. *)
        (env, seen_tyvars)
      else
        let seen_tyvars = Tvid.Set.add v seen_tyvars in
        match Inf.get_direct_binding env.inference_env v with
        | None -> failwith "Can only simplify type of bounded variables"
        | Some ty ->
          (* Only simplify the type of variables which are bound directly to a
             concrete type to preserve the variable aliasings and save some memory. *)
          let env =
            match get_node ty with
            | Tvar _ -> env
            | _ ->
              let (env, ty) = simplify_unions env ty in
              (* we only call this function when v does not recursively contain unsolved
                 type variables, so ty here should not contain unsolved type variables and
                 it is safe to simply bind it without reupdating the type var occurrences. *)
              let ty =
                map_reason ty ~f:(fun solution ->
                    Typing_reason.(solved v ~solution ~in_:r))
              in
              let env = bind env v ty in
              env
          in
          simplify_occurrences env v ~seen_tyvars
    in
    if not @@ Inf.contains_unsolved_tyvars env.inference_env v then
      fst @@ simplify_occurrences env v ~seen_tyvars:Tvid.Set.empty
    else
      env

  let add env ?(tyvar_pos = Pos.none) v ty =
    let env =
      { env with inference_env = Inf.add env.inference_env ~tyvar_pos v ty }
    in
    let r = get_reason ty in
    let env = simplify_occurrences env v r in
    env

  let get_type env r var =
    let (inference_env, res) = Inf.get_type env.inference_env r var in
    ({ env with inference_env }, res)

  let expand_type env ty =
    match deref ty with
    | (reason, Tvar tvid) -> expand_var env reason tvid
    | _ -> (env, ty)

  let expand_internal_type env ty =
    match ty with
    | LoclType ty ->
      let (env, ty) = expand_type env ty in
      (env, LoclType ty)
    | _ -> (env, ty)

  let get_tyvar_pos env var = Inf.get_tyvar_pos env.inference_env var

  let get_tyvar_lower_bounds env var =
    Inf.get_tyvar_lower_bounds env.inference_env var

  let set_tyvar_lower_bounds env var tys =
    {
      env with
      inference_env = Inf.set_tyvar_lower_bounds env.inference_env var tys;
    }

  let get_tyvar_upper_bounds env var =
    Inf.get_tyvar_upper_bounds env.inference_env var

  let is_shadow_tyvar env var = Inf.is_shadow env.inference_env var

  let fresh_shadow_tyvar env pos =
    let (inference_env, v) =
      Inf.fresh_shadow_tyvar env.inference_env env.tvar_id_provider pos
    in
    ({ env with inference_env }, v)

  let set_tyvar_upper_bounds env var tys =
    {
      env with
      inference_env = Inf.set_tyvar_upper_bounds env.inference_env var tys;
    }

  let get_tyvar_appears_covariantly env var =
    Inf.get_tyvar_appears_covariantly env.inference_env var

  let set_tyvar_appears_covariantly env var =
    {
      env with
      inference_env = Inf.set_tyvar_appears_covariantly env.inference_env var;
    }

  let get_tyvar_appears_contravariantly env var =
    Inf.get_tyvar_appears_contravariantly env.inference_env var

  let set_tyvar_appears_contravariantly env var =
    {
      env with
      inference_env =
        Inf.set_tyvar_appears_contravariantly env.inference_env var;
    }

  let get_tyvar_appears_invariantly env var =
    Inf.get_tyvar_appears_invariantly env.inference_env var

  let get_tyvar_eager_solve_fail env var =
    Inf.get_tyvar_eager_solve_fail env.inference_env var

  let set_tyvar_eager_solve_fail env var =
    {
      env with
      inference_env = Inf.set_tyvar_eager_solve_fail env.inference_env var;
    }

  let get_tyvar_type_consts env var =
    Inf.get_tyvar_type_consts env.inference_env var

  let get_tyvar_type_const env var tid =
    Inf.get_tyvar_type_const env.inference_env var tid

  let set_tyvar_type_const env var tconstid ty =
    {
      env with
      inference_env = Inf.set_tyvar_type_const env.inference_env var tconstid ty;
    }

  let get_current_tyvars env = Inf.get_current_tyvars env.inference_env

  let open_tyvars env p =
    { env with inference_env = Inf.open_tyvars env.inference_env p }

  let set_inference_env_pos env pos =
    let (inference_env, restore_inf) = Inf.set_pos env.inference_env pos in
    let restore env =
      { env with inference_env = restore_inf env.inference_env }
    in
    ({ env with inference_env }, restore)

  let close_tyvars env =
    { env with inference_env = Inf.close_tyvars env.inference_env }

  let wrap_ty_in_var env r ty =
    let (inference_env, res) =
      Inf.wrap_ty_in_var env.inference_env env.tvar_id_provider r ty
    in
    ({ env with inference_env }, res)

  let next_cont_opt = Typing_env_types.next_cont_opt

  let all_continuations env = LEnvC.all_continuations env.lenv.per_cont_env

  let get_tpenv = Typing_env_types.get_tpenv

  let get_global_tpenv env = env.tpenv

  let get_pos_and_kind_of_generic = Typing_env_types.get_pos_and_kind_of_generic

  let get_lower_bounds = Typing_env_types.get_lower_bounds

  let get_upper_bounds = Typing_env_types.get_upper_bounds

  (* Get bounds that are both an upper and lower of a given generic *)
  let get_equal_bounds = Typing_env_types.get_equal_bounds

  let get_reified env name =
    let tpenv = get_tpenv env in
    let local = TPEnv.get_reified tpenv name in
    let global = TPEnv.get_reified env.tpenv name in
    match (local, global) with
    | (Reified, _)
    | (_, Reified) ->
      Reified
    | (SoftReified, _)
    | (_, SoftReified) ->
      SoftReified
    | _ -> Erased

  let get_enforceable env name =
    let tpenv = get_tpenv env in
    let local = TPEnv.get_enforceable tpenv name in
    let global = TPEnv.get_enforceable env.tpenv name in
    local || global

  let get_newable env name =
    let tpenv = get_tpenv env in
    let local = TPEnv.get_newable tpenv name in
    let global = TPEnv.get_newable env.tpenv name in
    local || global

  let get_require_dynamic env name =
    let tpenv = get_tpenv env in
    let local = TPEnv.get_require_dynamic tpenv name in
    let global = TPEnv.get_require_dynamic env.tpenv name in
    local || global

  let env_with_tpenv env tpenv =
    {
      env with
      lenv =
        {
          env.lenv with
          per_cont_env =
            LEnvC.(
              update_cont_entry C.Next env.lenv.per_cont_env (fun entry ->
                  { entry with tpenv }));
        };
    }

  let env_with_global_tpenv env tpenv = { env with tpenv }

  let add_upper_bound_global env name ty =
    let tpenv =
      let (env, ty) = expand_type env ty in
      match deref ty with
      | (r, Tgeneric formal_super) ->
        TPEnv.add_lower_bound env.tpenv formal_super (mk (r, Tgeneric name))
      | _ -> env.tpenv
    in
    { env with tpenv = TPEnv.add_upper_bound tpenv name ty }

  let add_lower_bound_global env name ty =
    let tpenv =
      let (env, ty) = expand_type env ty in
      match deref ty with
      | (r, Tgeneric formal_super) ->
        TPEnv.add_upper_bound env.tpenv formal_super (mk (r, Tgeneric name))
      | _ -> env.tpenv
    in
    { env with tpenv = TPEnv.add_lower_bound tpenv name ty }

  (* Add a single new upper bound [ty] to generic parameter [name] in the local
   * type parameter environment of [env].
   * If the optional [intersect] operation is supplied, then use this to avoid
   * adding redundant bounds by merging the type with existing bounds. This makes
   * sense because a conjunction of upper bounds
   *   (T <: t1) /\ ... /\ (T <: tn)
   * is equivalent to a single upper bound
   *   T <: (t1 & ... & tn)
   *)
  let add_upper_bound ?intersect env name ty =
    env_with_tpenv
      env
      (TPEnv.add_upper_bound ?intersect (get_tpenv env) name ty)

  (* Add a single new upper lower [ty] to generic parameter [name] in the
   * local type parameter environment [env].
   * If the optional [union] operation is supplied, then use this to avoid
   * adding redundant bounds by merging the type with existing bounds. This makes
   * sense because a conjunction of lower bounds
   *   (t1 <: T) /\ ... /\ (tn <: T)
   * is equivalent to a single lower bound
   *   (t1 | ... | tn) <: T
   *)
  let add_lower_bound ?union env name ty =
    env_with_tpenv env (TPEnv.add_lower_bound ?union (get_tpenv env) name ty)

  (* Add type parameters to environment, initially with no bounds.
   * Existing type parameters with the same name will be overridden. *)
  let add_generic_parameters env tparaml =
    env_with_tpenv env (TPEnv.add_generic_parameters (get_tpenv env) tparaml)

  (* Add type parameters to environment with their bounds.
   * Existing type parameters with the same name will be overridden. *)
  let add_generic_parameters_with_bounds env tparams =
    env_with_tpenv
      env
      (TPEnv.add_generic_parameters_with_bounds (get_tpenv env) tparams)

  let unbind_generic_parameters env tparams =
    env_with_tpenv env (TPEnv.unbind_generic_parameters (get_tpenv env) tparams)

  let is_generic_parameter env name =
    TPEnv.mem name (get_tpenv env) || S_set.mem name env.fresh_typarams

  let get_generic_parameters env =
    TPEnv.get_tparam_names (TPEnv.union (get_tpenv env) env.tpenv)

  let get_tpenv_size env = TPEnv.size (get_tpenv env) + TPEnv.size env.tpenv

  let is_consistent env = TPEnv.is_consistent (get_tpenv env)

  let mark_inconsistent env =
    env_with_tpenv env (TPEnv.mark_inconsistent (get_tpenv env))

  let fresh_param_name env prefix : env * string =
    let rec iterate i =
      let name = Printf.sprintf "%s#%d" prefix i in
      if is_generic_parameter env name then
        iterate (i + 1)
      else
        name
    in
    let name = iterate 1 in
    let env = { env with fresh_typarams = S_set.add name env.fresh_typarams } in
    (env, name)

  (* Generate a fresh generic parameter with a specified prefix but distinct
   * from all generic parameters in the environment *)
  let add_fresh_generic_parameter_by_kind env pos prefix kind =
    let (env, name) = fresh_param_name env prefix in
    let env =
      env_with_tpenv env (TPEnv.add ~def_pos:pos name kind (get_tpenv env))
    in
    (env, name)

  let add_fresh_generic_parameter env pos prefix ~reified ~enforceable ~newable
      =
    let kind =
      KDefs.
        {
          lower_bounds = empty_bounds;
          upper_bounds = empty_bounds;
          reified;
          enforceable;
          newable;
          require_dynamic = true;
          rank = 0;
        }
    in
    add_fresh_generic_parameter_by_kind env pos prefix kind

  let is_fresh_generic_parameter name =
    String.contains name '#' && not (DependentKind.is_generic_dep_ty name)

  let get_tparams_in_ty_and_acc = Typing_env_types.get_tparams_in_ty_and_acc

  let get_tparams env ty = get_tparams_in_ty_and_acc env S_set.empty ty

  let get_tpenv_tparams env =
    TPEnv.fold
      begin
        fun _x
            KDefs.
              {
                lower_bounds;
                upper_bounds;
                reified = _;
                enforceable = _;
                newable = _;
                require_dynamic = _;
                rank = _;
              }
            acc ->
          let folder ty acc =
            let (_env, ty) = expand_type env ty in
            match get_node ty with
            | Tgeneric _ -> acc
            | _ -> get_tparams_in_ty_and_acc env acc ty
          in
          TySet.fold folder lower_bounds @@ TySet.fold folder upper_bounds acc
      end
      (get_tpenv env)
      S_set.empty

  (* Replace types for locals with empty environment *)
  let env_with_locals env locals =
    { env with lenv = { env.lenv with per_cont_env = locals } }

  (* This is used whenever we start checking a method. Retain tpenv from the class type parameters *)
  let reinitialize_locals env =
    env_with_locals
      env
      LEnvC.(initial_locals { empty_entry with tpenv = get_tpenv env })

  let set_env_callable_pos env callable_pos =
    { env with genv = { env.genv with callable_pos } }

  let set_env_function_pos env function_pos =
    { env with genv = { env.genv with function_pos } }

  let set_fun_tast_info env fun_tast_info =
    { env with fun_tast_info = Some fun_tast_info }

  let fun_is_constructor env = env.genv.fun_is_ctor

  let set_fun_is_constructor env is_ctor =
    { env with genv = { env.genv with fun_is_ctor = is_ctor } }

  let env_with_method_droot_member env m ~static =
    let child =
      if static then
        Typing_pessimisation_deps.SMethod m
      else
        Typing_pessimisation_deps.Method m
    in
    let decl_env = { env.decl_env with droot_member = Some child } in
    { env with decl_env }

  let env_with_constructor_droot_member env =
    let member = Typing_pessimisation_deps.Constructor in
    let decl_env = { env.decl_env with droot_member = Some member } in
    { env with decl_env }

  let get_module (env : env) (name : Decl_provider.module_key) :
      module_def_type option =
    let res =
      (Decl_provider.get_module
         ?tracing_info:(get_tracing_info env)
         (get_ctx env)
         name [@alert "-dependencies"])
    in
    Deps.make_depend_on_module env name res;
    res

  let set_current_module env m =
    { env with genv = { env.genv with current_module = m } }

  let get_current_module env = Option.map env.genv.current_module ~f:snd

  let get_current_package env =
    let current_pkg_name =
      Option.map env.genv.current_package ~f:Aast_utils.get_package_name
    in
    let info = get_tcopt env |> Typechecker_options.package_info in
    Option.bind current_pkg_name ~f:(Package_info.get_package info)

  let get_current_package_def_pos env =
    let current_pkg_name =
      Option.map env.genv.current_package ~f:Aast_utils.get_package_name
    in
    let info = get_tcopt env |> Typechecker_options.package_info in
    Option.bind current_pkg_name ~f:(fun p ->
        Package_info.get_package info p |> Option.map ~f:Package.get_package_pos)

  let get_current_package_membership env = env.genv.current_package

  let set_current_package_membership env package =
    { env with genv = { env.genv with current_package = package } }

  let get_soft_package_requirement env = env.genv.soft_package_requirement

  let make_depend_on_current_module env =
    Option.iter
      Typing_env_types.(env.genv.current_module)
      ~f:(fun (_, mid) -> Deps.make_depend_on_module_name env mid)

  let set_internal env b =
    { env with genv = { env.genv with this_internal = b } }

  let get_internal env = env.genv.this_internal

  let get_package_by_name env pkg_name =
    let info = get_tcopt env |> Typechecker_options.package_info in
    Package_info.get_package info pkg_name

  let is_package_loaded env package =
    let open Typing_local_packages in
    let per_cont_env = env.lenv.per_cont_env in
    match LEnvC.get_cont_option C.Next per_cont_env with
    | None -> false
    | Some next_cont -> begin
      match S_map.find_opt package next_cont.LEnvC.loaded_packages with
      | None
      | Some { status = Not_exists_in_deployment; _ } ->
        false
      | Some { status = Exists_in_deployment; _ }
      | Some { status = Unsatisfiable_package_constraints; _ } ->
        (* this is dead code due to unsatisfiable package checks; returning
           true to not emit additional errors *)
        true
    end

  let get_loaded_packages_list env =
    let open Typing_local_packages in
    let per_cont_env = env.lenv.per_cont_env in
    match LEnvC.get_cont_option C.Next per_cont_env with
    | None -> []
    | Some next_cont ->
      S_map.fold
        (fun pkg { pos; status; from_includes } acc ->
          match status with
          | Exists_in_deployment ->
            if from_includes then
              acc
            else
              (pos, pkg) :: acc
          | Not_exists_in_deployment
          | Unsatisfiable_package_constraints ->
            acc)
        next_cont.LEnvC.loaded_packages
        []

  let get_included_packages env =
    let current_pkg_includes =
      match get_current_package env with
      | None -> []
      | Some pkg ->
        let current_pkg_name = snd pkg.Package.name in
        List.map pkg.Package.includes ~f:(fun (include_pos, included_name) ->
            (current_pkg_name, included_name, include_pos))
    in
    let loaded_packages = get_loaded_packages_list env in
    let loaded_pkg_includes =
      List.concat_map loaded_packages ~f:(fun (_, loaded_pkg_name) ->
          match get_package_by_name env loaded_pkg_name with
          | None -> []
          | Some pkg ->
            List.map
              pkg.Package.includes
              ~f:(fun (include_pos, included_name) ->
                (loaded_pkg_name, included_name, include_pos)))
    in
    current_pkg_includes @ loaded_pkg_includes

  let package_allow_classconst_violations env =
    Typechecker_options.package_allow_classconst_violations @@ get_tcopt env

  let package_allow_all_tconst_violations env =
    Typechecker_options.package_allow_all_tconst_violations @@ get_tcopt env

  let package_allow_enforceable_enum_violations env =
    Typechecker_options.package_allow_enforceable_enum_violations
    @@ get_tcopt env

  let assert_packages_loaded_from_attr env attr =
    match
      Naming_attributes.find
        Naming_special_names.UserAttributes.uaRequirePackage
        attr
    with
    | Some attr ->
      let pos = fst attr.ua_name in
      let package_info = get_tcopt env |> Typechecker_options.package_info in
      let per_cont_env =
        List.fold
          ~init:env.lenv.per_cont_env
          ~f:
            (fun per_cont_env -> function
              | (_, _, Aast.String pkg) ->
                LEnvC.assert_package_loaded_in_cont
                  ~package_info
                  C.Next
                  pos
                  pkg
                  Typing_local_packages.Exists_in_deployment
                  per_cont_env
              | _ -> per_cont_env)
          attr.ua_params
      in
      { env with lenv = { env.lenv with per_cont_env } }
    | _ -> env

  let set_soft_package_from_attr env (attr : Nast.user_attribute list) =
    match
      Naming_attributes.find
        Naming_special_names.UserAttributes.uaSoftRequirePackage
        attr
    with
    | Some { ua_params = [(_, _, Aast.String pkg_name)]; ua_name = (pos, _) } ->
      {
        env with
        genv = { env.genv with soft_package_requirement = Some (pos, pkg_name) };
      }
    | _ -> env

  let get_typedef env x =
    let td =
      (Decl_provider.get_typedef
         ?tracing_info:(get_tracing_info env)
         (get_ctx env)
         x [@alert "-dependencies"])
    in
    Deps.make_depend_on_typedef env x td;
    td

  let is_typedef env x =
    if is_generic_parameter env x then
      false
    else
      match Naming_provider.get_type_kind (get_ctx env) x with
      | Some Naming_types.TTypedef -> true
      | _ -> false

  let should_expand_type_alias env ~visibility_behavior ~name td =
    match td.td_type_assignment with
    | CaseType _ ->
      (match visibility_behavior with
      | Always_expand_newtype -> true
      | Resolve_type_structure None -> false
      | Resolve_type_structure (Some t) -> String.(t <> name)
      | Expand_visible_newtype_only
      | Never_expand_newtype ->
        false)
    | SimpleTypeDef (td_vis, _td_type) ->
      let td_module = td.td_module in
      (match td_vis with
      | Aast.Opaque ->
        (match visibility_behavior with
        | Resolve_type_structure _
        | Always_expand_newtype ->
          true
        | Never_expand_newtype -> false
        | Expand_visible_newtype_only ->
          let td_path = Naming_provider.get_typedef_path (get_ctx env) name in
          (match td_path with
          | Some s -> Relative_path.equal s (get_file env)
          | None -> (* Not the right place to raise an error *) false))
      | Aast.OpaqueModule ->
        (match visibility_behavior with
        | Resolve_type_structure _
        | Always_expand_newtype ->
          true
        | Never_expand_newtype -> false
        | Expand_visible_newtype_only ->
          Option.equal
            String.equal
            (get_current_module env)
            (Option.map td_module ~f:snd))
      | Aast.Transparent -> true)

  let is_typedef_visible env ~name td =
    should_expand_type_alias
      env
      ~visibility_behavior:default_visibility_behaviour
      ~name
      td

  (** see .mli **)
  let get_class (env : env) (name : Decl_provider.type_key) : Cls.t Decl_entry.t
      =
    let res =
      (Decl_provider.get_class
         ?tracing_info:(get_tracing_info env)
         (get_ctx env)
         name [@alert "-dependencies"])
    in
    Deps.make_depend_on_class env name res;
    res

  let get_parent env ~skip_constructor_dep name : Cls.t Decl_entry.t =
    let res = get_class env name in
    Deps.make_depend_on_parent env ~skip_constructor_dep name res;
    res

  let add_parent_dep env ~skip_constructor_dep name : unit =
    let _ = get_parent env ~skip_constructor_dep name in
    ()

  let get_class_or_typedef env x :
      Folded_class.t class_or_typedef_result Decl_entry.t =
    if is_typedef env x then
      Decl_entry.map (get_typedef env x) ~f:(fun td -> TypedefResult td)
    else
      Decl_entry.map (get_class env x) ~f:(fun cd -> ClassResult cd)

  let get_class_or_typedef_tparams env x =
    match get_class_or_typedef env x with
    | Decl_entry.Found (TypedefResult { td_tparams; _ }) -> td_tparams
    | Decl_entry.Found (ClassResult cls) -> Cls.tparams cls
    | Decl_entry.DoesNotExist
    | Decl_entry.NotYetAvailable ->
      []

  let get_fun env x =
    let res =
      (Decl_provider.get_fun
         ?tracing_info:(get_tracing_info env)
         (get_ctx env)
         x [@alert "-dependencies"])
    in
    Deps.make_depend_on_fun env x res;
    res

  let get_enum_constraint env x =
    Decl_entry.bind (get_class env x) @@ fun tc ->
    match Cls.enum_type tc with
    | None -> Decl_entry.DoesNotExist
    | Some e -> Decl_entry.Found e.te_constraint

  (* TODO: do we want to introduce a Cls.enum_class_type ? *)

  (** see .mli **)
  let get_enum env x =
    Decl_entry.bind (get_class env x) @@ fun tc ->
    if Option.is_some (Cls.enum_type tc) then
      Decl_entry.Found tc
    else
      Decl_entry.DoesNotExist

  let is_enum env x =
    match get_enum env x with
    | Decl_entry.Found cls -> Ast_defs.is_c_enum (Cls.kind cls)
    | Decl_entry.DoesNotExist
    | Decl_entry.NotYetAvailable ->
      false

  (** see .mli **)
  let is_enum_class env x =
    match get_enum env x with
    | Decl_entry.Found cls -> Ast_defs.is_c_enum_class (Cls.kind cls)
    | Decl_entry.DoesNotExist
    | Decl_entry.NotYetAvailable ->
      false

  let get_typeconst env class_ mid =
    Deps.make_depend_on_class_const env class_ mid;
    Cls.get_typeconst class_ mid [@alert "-dependencies"]

  (* Used to access class constants. *)

  (** see .mli **)
  let get_const env class_ mid =
    Deps.make_depend_on_class_const env class_ mid;
    Cls.get_const class_ mid [@alert "-dependencies"]

  (** see .mli **)
  let consts env class_ =
    Deps.make_depend_on_class env (Cls.name class_) (Decl_entry.Found class_);
    Cls.consts class_ [@alert "-dependencies"]

  (* Used to access "global constants". That is constants that were
   * introduced with "const X = ...;" at topelevel, or "define('X', ...);"
   *)
  let get_gconst env cst_name =
    let res =
      (Decl_provider.get_gconst
         ?tracing_info:(get_tracing_info env)
         (get_ctx env)
         cst_name [@alert "-dependencies"])
    in
    Deps.make_depend_on_gconst env cst_name res;
    res

  let get_static_member is_method env class_ mid =
    (* The type of a member is stored separately in the heap. This means that
     * any user of the member also has a dependency on the class where the member
     * originated.
     *)
    let ce_opt =
      if is_method then
        Cls.get_smethod class_ mid [@alert "-dependencies"]
      else
        Cls.get_sprop class_ mid [@alert "-dependencies"]
    in
    Deps.add_member_dep ~is_method ~is_static:true env class_ mid ce_opt;
    ce_opt

  let suggest_member members mid =
    let pairs =
      List.map members ~f:(fun (x, { ce_type = (lazy ty); _ }) ->
          (get_pos ty, x))
    in
    String_utils.most_similar mid pairs snd

  let suggest_static_member is_method class_ mid =
    let mid = String.lowercase mid in
    let members =
      if is_method then
        Cls.smethods class_
      else
        Cls.sprops class_
    in
    suggest_member members mid

  (** see .mli **)
  let get_method env (class_ : Cls.t) mid =
    (* The type of a member is stored separately in the heap. This means that
     * any user of the member also has a dependency on the class where the member
     * originated.
     *)
    let ce_opt = (Cls.get_method class_ mid [@alert "-dependencies"]) in
    Deps.add_member_dep ~is_method:true ~is_static:false env class_ mid ce_opt;
    ce_opt

  (** see .mli **)
  let get_prop env (class_ : Cls.t) mid =
    (* The type of a member is stored separately in the heap. This means that
     * any user of the member also has a dependency on the class where the member
     * originated.
     *)
    let ce_opt = (Cls.get_prop class_ mid [@alert "-dependencies"]) in
    Deps.add_member_dep ~is_method:false ~is_static:false env class_ mid ce_opt;
    ce_opt

  let suggest_member is_method class_ mid =
    let mid = String.lowercase mid in
    let members =
      if is_method then
        Cls.methods class_
      else
        Cls.props class_
    in
    suggest_member members mid

  let get_construct env class_ =
    Deps.make_depend_on_constructor env class_;
    Cls.construct class_ [@alert "-dependencies"]

  let valid_newable_class env class_ =
    if Ast_defs.is_c_class (Cls.kind class_) then
      Cls.final class_
      || not
           (equal_consistent_kind (snd (get_construct env class_)) Inconsistent)
    else
      (* There is currently a bug with interfaces that allows constructors to
         change their signature, so they are not considered here.
         TODO: T41093452 *)
      false

  let get_return env = env.genv.return

  let set_return env x =
    let genv = env.genv in
    let genv = { genv with return = x } in
    { env with genv }

  let get_readonly env = env.genv.readonly

  let set_readonly env x =
    let genv = env.genv in
    let genv = { genv with readonly = x } in
    { env with genv }

  let get_params env = env.genv.params

  let set_params env params = { env with genv = { env.genv with params } }

  let set_param env x param =
    let params = get_params env in
    let params = LID.Map.add x param params in
    set_params env params

  let clear_params env = set_params env LID.Map.empty

  let with_env env f =
    let ret = get_return env in
    let params = get_params env in
    let (env, result) = f env in
    let env = set_params env params in
    let env = set_return env ret in
    (env, result)

  let with_origin env origin f =
    let ti1 = env.tracing_info in
    let ti2 = Option.map ti1 ~f:(fun ti -> { ti with Decl_counters.origin }) in
    let env = { env with tracing_info = ti2 } in
    let (env, result) = f env in
    let env = { env with tracing_info = ti1 } in
    (env, result)

  let with_origin2 env origin f =
    let ti1 = env.tracing_info in
    let ti2 = Option.map ti1 ~f:(fun ti -> { ti with Decl_counters.origin }) in
    let env = { env with tracing_info = ti2 } in
    let (env, r1, r2) = f env in
    let env = { env with tracing_info = ti1 } in
    (env, r1, r2)

  let is_static env = env.genv.static

  let get_val_kind env = env.genv.val_kind

  (** see .mli **)
  let get_self_ty env = Option.map env.genv.self ~f:snd

  let get_self_class_type env =
    match get_self_ty env with
    | Some self -> begin
      match get_node self with
      | Tclass (id, exact, tys) -> Some (id, exact, tys)
      | _ -> None
    end
    | None -> None

  (** see .mli **)
  let get_self_id env = Option.map env.genv.self ~f:fst

  let get_self_class env =
    let self = Decl_entry.of_option_or_does_not_exist (get_self_id env) in
    Decl_entry.bind self (get_class env)

  let get_parent_ty env = Option.map env.genv.parent ~f:snd

  (** see .mli **)
  let get_parent_id env = Option.map env.genv.parent ~f:fst

  let get_parent_class env =
    let self = Decl_entry.of_option_or_does_not_exist (get_parent_id env) in
    Decl_entry.bind self (get_class env)

  let get_fn_kind env = env.genv.fun_kind

  let set_fn_kind env fn_type =
    let genv = env.genv in
    let genv = { genv with fun_kind = fn_type } in
    { env with genv }

  let set_support_dynamic_type env b =
    { env with genv = { env.genv with this_support_dynamic_type = b } }

  let set_no_auto_likes env b =
    { env with genv = { env.genv with no_auto_likes = b } }

  let set_needs_concrete env b =
    { env with genv = { env.genv with needs_concrete = b } }

  let set_everything_sdt env b =
    map_tcopt
      ~f:(fun tcopt ->
        Global_options.
          {
            tcopt with
            po = { tcopt.po with Parser_options.everything_sdt = b };
          })
      env

  let get_support_dynamic_type env = env.genv.this_support_dynamic_type

  let get_no_auto_likes env = env.genv.no_auto_likes

  let is_concrete_class env class_ =
    let has_final_class_consistency () =
      match snd @@ get_construct env class_ with
      | FinalClass -> true
      | Inconsistent
      | ConsistentConstruct ->
        false
    in
    (not (Cls.abstract class_))
    || (Cls.final class_ && has_final_class_consistency ())

  let containing_class_is_final_and_concrete env =
    match get_self_class env |> Decl_entry.to_option with
    | Some class_ -> Cls.final class_ && is_concrete_class env class_
    | None -> false

  let static_points_to_concrete_class env =
    (* if we're in a __NeedsConcrete method, `static` points to a concrete class *)
    env.genv.needs_concrete
    (* if we're in an instance method or constructor then the class must be instantiable and therefore concrete *)
    || (not @@ is_static env)
    || containing_class_is_final_and_concrete env

  let set_self env self_id self_ty =
    let genv = env.genv in
    let genv = { genv with self = Some (self_id, self_ty) } in
    { env with genv }

  let run_with_no_self env f =
    let self = env.genv.self in
    let genv = { env.genv with self = None } in
    let (env, result) = f { env with genv } in
    let genv = { env.genv with self } in
    ({ env with genv }, result)

  let set_parent env parent_id parent_ty =
    let genv = env.genv in
    let genv = { genv with parent = Some (parent_id, parent_ty) } in
    { env with genv }

  let set_static env =
    let genv = env.genv in
    let genv = { genv with static = true } in
    { env with genv }

  let set_val_kind env x =
    let genv = env.genv in
    let genv = { genv with val_kind = x } in
    { env with genv }

  let set_mode env mode =
    let decl_env = env.decl_env in
    let decl_env = { decl_env with mode } in
    { env with decl_env }

  let get_mode env = env.decl_env.mode

  let is_strict env = File_info.is_strict (get_mode env)

  let is_hhi env = File_info.is_hhi (get_mode env)

  (*****************************************************************************)
  (* Locals *)
  (*****************************************************************************)

  let set_local_ env x ty =
    let per_cont_env = LEnvC.add_to_cont C.Next x ty env.lenv.per_cont_env in
    { env with lenv = { env.lenv with per_cont_env } }

  let make_expression_id env = Expression_id.make env.expression_id_provider

  (* We maintain 2 states for a local: the type
   * that the local currently has, and an expression_id generated from
   * the last assignment to this local.
   *)
  let set_local ?(immutable = false) ?macro_splice_vars env x new_type pos =
    let new_type =
      match get_node new_type with
      | Tunion [ty] -> ty
      | _ -> new_type
    in
    match next_cont_opt env with
    | None -> env
    | Some next_cont ->
      let expr_id =
        match LID.Map.find_opt x next_cont.LEnvC.local_types with
        | None -> make_expression_id env
        | Some local -> local.Typing_local_types.eid
      in
      let expr_id =
        if immutable then
          Expression_id.make_immutable expr_id
        else
          expr_id
      in
      (* Dynamic inference: when a local is assigned type `dynamic`, allocate a
         fresh shadow type variable and attach it via Tdynamic(Some v). The shadow
         type variable tracks how the local is used by accumulating upper bounds.
         If the local is re-assigned to a non-dynamic type, we preserve the existing
         shadow type variable from the previous binding so constraints aren't lost. *)
      let (env, new_type) =
        match get_node new_type with
        | Tdynamic None
          when Typechecker_options.tco_dynamic_inference (get_tcopt env) ->
          let local_name = Local_id.to_string x in
          let (inference_env, v) =
            Inf.fresh_shadow_tyvar
              env.inference_env
              env.tvar_id_provider
              ~local_name
              pos
          in
          let env = { env with inference_env } in
          (env, mk (get_reason new_type, Tdynamic (Some v)))
        | Tdynamic (Some _) -> (env, new_type)
        | _ ->
          if Typechecker_options.tco_dynamic_inference (get_tcopt env) then
            match LID.Map.find_opt x next_cont.LEnvC.local_types with
            | Some local ->
              (match get_node local.Typing_local_types.ty with
              | Tdynamic (Some _) -> (env, local.Typing_local_types.ty)
              | _ -> (env, new_type))
            | None -> (env, new_type)
          else
            (env, new_type)
      in
      let local =
        Typing_local_types.
          { ty = new_type; pos; eid = expr_id; macro_splice_vars }
      in
      set_local_ env x local

  let is_using_var env x = LID.Set.mem x env.lenv.local_using_vars

  let set_using_var env x =
    {
      env with
      lenv =
        {
          env.lenv with
          local_using_vars = LID.Set.add x env.lenv.local_using_vars;
        };
    }

  let unset_local env local =
    let { per_cont_env; local_using_vars } = env.lenv in
    let per_cont_env =
      LEnvC.remove_from_cont C.Next local
      @@ LEnvC.remove_from_cont C.Catch local
      @@ per_cont_env
    in
    let local_using_vars = LID.Set.remove local local_using_vars in
    let env = { env with lenv = { per_cont_env; local_using_vars } } in
    env

  let tany _env = Typing_defs.make_tany ()

  let local_undefined_error ~env p x ctx =
    let open Option.Let_syntax in
    (* When inside an expression tree, the user may forget to splice in a
       variable. If that happens we want to suggest splicing in that
       variable. *)
    let all_outer_locals () =
      Option.value ~default:[]
      @@ let* { outer_locals; dsl } = env.in_expr_tree in
         let locals =
           LID.Map.fold
             (fun k v acc ->
               (* $this doesn't have a position. In general best to avoid
                  suggestions that lack positions. *)
               if
                 LID.is_user_denotable k
                 && (not @@ Pos.equal v.Typing_local_types.pos Pos.none)
               then
                 (k, v, Some dsl) :: acc
               else
                 acc)
             outer_locals
             []
         in
         return locals
    in
    match p with
    | Some p ->
      let lid = LID.to_string x in
      let suggest_most_similar lid =
        (* Ignore fake locals *)
        let all_locals =
          LID.Map.fold
            (fun k v acc ->
              if LID.is_user_denotable k then
                (k, v, None) :: acc
              else
                acc)
            ctx.LEnvC.local_types
            (* Prefer suggesting variables within the current scope *)
            (all_outer_locals ())
        in
        let var_name (k, _, _) = LID.to_string k in
        match String_utils.most_similar lid all_locals var_name with
        | Some (k, local, dsl) ->
          (Some (LID.to_string k, local.Typing_local_types.pos), dsl)
        | None -> (None, None)
      in
      let (most_similar, in_dsl) = suggest_most_similar lid in
      let error =
        match in_dsl with
        | None ->
          Naming_error.Undefined
            { pos = p; var_name = lid; did_you_mean = most_similar }
        | Some dsl ->
          Naming_error.Undefined_in_expr_tree
            {
              pos = p;
              var_name = lid;
              dsl = Some (snd dsl);
              did_you_mean = most_similar;
            }
      in
      let custom_err_config =
        Typechecker_options.custom_error_config (get_tcopt env)
      in
      Diagnostics.add_diagnostic
        (Naming_error_utils.to_user_diagnostic error custom_err_config)
    | None -> ()

  let get_local_in_ctx ~undefined_err_fun env x ctx_opt =
    let not_found_is_ok x ctx = Fake.is_valid ctx.LEnvC.fake_members x in
    match ctx_opt with
    | None ->
      (* If the continuation is absent, we are in dead code so the variable should
         have type nothing. *)
      Some
        Typing_local_types.
          {
            ty = Typing_make_type.nothing Reason.none;
            pos = Pos.none;
            eid = make_expression_id env;
            macro_splice_vars = None;
          }
    | Some ctx ->
      let lcl = LID.Map.find_opt x ctx.LEnvC.local_types in
      begin
        match lcl with
        | None ->
          if not_found_is_ok x ctx then
            ()
          else
            undefined_err_fun x ctx
        | Some _ -> ()
      end;
      lcl

  let get_local_ty_in_ctx ~undefined_err_fun env x ctx_opt =
    match get_local_in_ctx ~undefined_err_fun env x ctx_opt with
    | None ->
      ( false,
        Typing_local_types.
          {
            ty = Typing_make_type.nothing Reason.none;
            pos = Pos.none;
            eid = make_expression_id env;
            macro_splice_vars = None;
          } )
    | Some local -> (true, local)

  let get_local_in_next_continuation ?error_if_undef_at_pos:p env x =
    let undefined_err_fun = local_undefined_error ~env p in
    let next_cont = next_cont_opt env in
    get_local_ty_in_ctx ~undefined_err_fun env x next_cont

  let get_local_ ?error_if_undef_at_pos:p env x =
    get_local_in_next_continuation ?error_if_undef_at_pos:p env x

  let get_local env x = snd (get_local_ env x)

  let get_locals ?(quiet = false) env plids =
    let next_cont = next_cont_opt env in
    List.fold plids ~init:LID.Map.empty ~f:(fun locals (_, (p, lid)) ->
        let error_if_undef_at_pos =
          if quiet then
            None
          else
            Some p
        in
        let undefined_err_fun =
          local_undefined_error ~env error_if_undef_at_pos
        in
        match get_local_in_ctx ~undefined_err_fun env lid next_cont with
        | None -> locals
        | Some ty_eid -> LID.Map.add lid ty_eid locals)

  let set_locals env locals =
    LID.Map.fold (fun lid ty env -> set_local_ env lid ty) locals env

  let is_local_defined env x =
    let next_cont = next_cont_opt env in
    Option.is_some next_cont && fst (get_local_ env x)

  let get_local_check_defined env (p, x) =
    snd (get_local_ ~error_if_undef_at_pos:p env x)

  let set_local_expr_id env x new_eid =
    let per_cont_env = env.lenv.per_cont_env in
    match LEnvC.get_cont_option C.Next per_cont_env with
    | None -> Ok env
    | Some next_cont -> begin
      let open Typing_local_types in
      match LID.Map.find_opt x next_cont.LEnvC.local_types with
      | Some Typing_local_types.{ ty; pos; eid; macro_splice_vars }
        when not (Expression_id.equal eid new_eid) ->
        let local = { ty; pos; eid = new_eid; macro_splice_vars } in
        let per_cont_env = LEnvC.add_to_cont C.Next x local per_cont_env in
        let env = { env with lenv = { env.lenv with per_cont_env } } in
        if Expression_id.is_immutable eid then
          Error (env, Typing_error.(primary @@ Primary.Immutable_local pos))
        else
          Ok env
      | _ -> Ok env
    end

  let get_local_expr_id env x =
    match next_cont_opt env with
    | None -> (* dead code *) None
    | Some next_cont ->
      let lcl = LID.Map.find_opt x next_cont.LEnvC.local_types in
      Option.map lcl ~f:(fun x -> x.Typing_local_types.eid)

  let set_fake_members env fake_members =
    let per_cont_env =
      LEnvC.update_cont_entry C.Next env.lenv.per_cont_env (fun entry ->
          { entry with LEnvC.fake_members })
    in
    { env with lenv = { env.lenv with per_cont_env } }

  let set_loaded_packages env loaded_packages =
    let per_cont_env =
      LEnvC.update_cont_entry C.Next env.lenv.per_cont_env (fun entry ->
          { entry with LEnvC.loaded_packages })
    in
    { env with lenv = { env.lenv with per_cont_env } }

  let get_fake_members env =
    match LEnvC.get_cont_option C.Next env.lenv.per_cont_env with
    | None -> Fake.empty
    | Some next_cont -> next_cont.LEnvC.fake_members

  let update_lost_info name blame env ty =
    let info r = Reason.lost_info (name, r, blame) in
    let rec update_ty (env, seen_tyvars) ty =
      let (env, ty) = expand_type env ty in
      match deref ty with
      | (_, Tvar v) ->
        if Tvid.Set.mem v seen_tyvars then
          ((env, seen_tyvars), ty)
        else
          let seen_tyvars = Tvid.Set.add v seen_tyvars in
          let bs = get_tyvar_lower_bounds env v in
          let ((env, seen_tyvars), bs) =
            ITySet.fold_map bs ~init:(env, seen_tyvars) ~f:update_ty_i
          in
          let env = set_tyvar_lower_bounds env v bs in
          let bs = get_tyvar_upper_bounds env v in
          let ((env, seen_tyvars), bs) =
            ITySet.fold_map bs ~init:(env, seen_tyvars) ~f:update_ty_i
          in
          let env = set_tyvar_upper_bounds env v bs in
          ((env, seen_tyvars), ty)
      | (r, Toption ty) ->
        let ((env, seen_tyvars), ty) = update_ty (env, seen_tyvars) ty in
        ((env, seen_tyvars), mk (info r, Toption ty))
      | (r, Tunion tyl) ->
        let ((env, seen_tyvars), tyl) =
          List.fold_map tyl ~init:(env, seen_tyvars) ~f:update_ty
        in
        ((env, seen_tyvars), mk (info r, Tunion tyl))
      | _ -> ((env, seen_tyvars), map_reason ty ~f:info)
    and update_ty_i (env, seen_tyvars) ty =
      match ty with
      | LoclType ty ->
        let ((env, seen_tyvars), ty) = update_ty (env, seen_tyvars) ty in
        ((env, seen_tyvars), LoclType ty)
      | ConstraintType ty ->
        let (r, ty) = deref_constraint_type ty in
        ((env, seen_tyvars), ConstraintType (mk_constraint_type (info r, ty)))
    in
    let ((env, _seen_tyvars), ty) = update_ty (env, Tvid.Set.empty) ty in
    (env, ty)

  let forget_generic forget env blame =
    let fake_members = get_fake_members env in
    let fake_members = forget fake_members blame in
    set_fake_members env fake_members

  let forget_members = forget_generic Fake.forget

  let forget_prefixed_members env lid =
    forget_generic
      (fun fake_members -> Fake.forget_prefixed fake_members lid)
      env

  let forget_suffixed_members env suffix =
    forget_generic
      (fun fake_members -> Fake.forget_suffixed fake_members suffix)
      env

  module FakeMembers = struct
    let update_fake_members env fake_members =
      let per_cont_env =
        LEnvC.update_cont_entry C.Next env.lenv.per_cont_env (fun entry ->
            LEnvC.{ entry with fake_members })
      in
      { env with lenv = { env.lenv with per_cont_env } }

    let is_valid env obj member_name =
      match obj with
      | (_, _, This)
      | (_, _, Lvar _) ->
        let fake_members = get_fake_members env in
        let id = Fake.make_id obj member_name in
        Fake.is_valid fake_members id
      | _ -> false

    let is_valid_static env cid member_name =
      let name = Fake.make_static_id cid member_name in
      let fake_members = get_fake_members env in
      Fake.is_valid fake_members name

    let check_static_invalid env cid member_name ty =
      let fake_members = get_fake_members env in
      let fake_id = Fake.make_static_id cid member_name in
      match Fake.is_invalid fake_members fake_id with
      | None -> (env, ty)
      | Some blame -> update_lost_info (LID.to_string fake_id) blame env ty

    let check_instance_invalid env obj member_name ty =
      match obj with
      | (_, _, This)
      | (_, _, Lvar _) ->
        let fake_members = get_fake_members env in
        let fake_id = Fake.make_id obj member_name in
        begin
          match Fake.is_invalid fake_members fake_id with
          | None -> (env, ty)
          | Some blame -> update_lost_info (LID.to_string fake_id) blame env ty
        end
      | _ -> (env, ty)

    let add_member env fake_id pos =
      let fake_members = get_fake_members env in
      let fake_members = Fake.add fake_members fake_id pos in
      set_fake_members env fake_members

    let make env obj_name member_name pos =
      let my_fake_local_id = Fake.make_id obj_name member_name in
      let env = add_member env my_fake_local_id pos in
      (env, my_fake_local_id)

    let make_static env class_name member_name pos =
      let my_fake_local_id = Fake.make_static_id class_name member_name in
      let env = add_member env my_fake_local_id pos in
      (env, my_fake_local_id)
  end

  (*****************************************************************************)
  (* Sets up/cleans up the environment when typing anonymous function / lambda *)
  (*****************************************************************************)

  let closure env f =
    (* Remember parts of the environment specific to the enclosing function
       that will be overwritten when typing a lambda *)
    let old_lenv = env.lenv in
    let old_return = get_return env in
    let old_params = get_params env in
    let outer_fun_kind = get_fn_kind env in
    let outer_check_status = env.checked in
    let old_in_lambda = env.in_lambda in
    let env = { env with in_lambda = true } in
    (* Typing *)
    let (env, ret) = f env in
    (* Restore the environment fields that were clobbered *)
    let env = { env with in_lambda = old_in_lambda } in
    let env = { env with lenv = old_lenv } in
    let env = set_params env old_params in
    let env = set_return env old_return in
    let env = set_fn_kind env outer_fun_kind in
    let env = { env with checked = outer_check_status } in
    (env, ret)

  let in_try env f =
    let old_in_try = env.in_try in
    let env = { env with in_try = true } in
    let (env, result) = f env in
    ({ env with in_try = old_in_try }, result)

  (* Return the subset of env which is saved in the Typed AST's EnvAnnotation. *)
  let save local_tpenv env =
    {
      Tast.tcopt = get_tcopt env;
      Tast.inference_env = env.inference_env;
      Tast.tpenv = TPEnv.union local_tpenv env.tpenv;
      Tast.fun_tast_info = env.fun_tast_info;
      Tast.checked = env.checked;
    }

  (* Compute the type variables appearing covariantly (positively)
   * resp. contravariantly (negatively) in a given type ty.
   * Return a pair of sets of positive and negative type variables
   * (as well as an updated environment).
   *)
  let rec get_tyvars env (ty : locl_ty) = get_tyvars_i env (LoclType ty)

  and get_tyvars_i env (ty : internal_type) =
    let get_tyvars_union (env, acc_positive, acc_negative) ty =
      let (env, positive, negative) = get_tyvars env ty in
      ( env,
        Tvid.Set.union acc_positive positive,
        Tvid.Set.union acc_negative negative )
    in
    let get_tyvars_param (env, acc_positive, acc_negative) fp =
      let (env, positive, negative) = get_tyvars env fp.fp_type in
      match get_fp_mode fp with
      (* Parameters are treated contravariantly *)
      | FPnormal ->
        ( env,
          Tvid.Set.union negative acc_positive,
          Tvid.Set.union positive acc_negative )
      (* Inout parameters are both co- and contra-variant *)
      | FPinout ->
        let tyvars = Tvid.Set.union negative positive in
        ( env,
          Tvid.Set.union tyvars acc_positive,
          Tvid.Set.union tyvars acc_negative )
    in
    let rec unions_aux tvs acc =
      match tvs with
      | [] -> acc
      | tv :: tvs -> unions_aux tvs (Tvid.Set.union acc tv)
    in
    let unions tvs = unions_aux tvs Tvid.Set.empty in
    let (env, ety) = expand_internal_type env ty in
    match ety with
    | LoclType ety ->
      (match get_node ety with
      | Tvar v -> (env, Tvid.Set.singleton v, Tvid.Set.empty)
      | Tany _
      | Tnonnull
      | Tdynamic _
      | Tprim _
      | Tneg _ ->
        (env, Tvid.Set.empty, Tvid.Set.empty)
      | Toption ty -> get_tyvars env ty
      | Ttuple { t_required; t_optional; t_extra = Tvariadic t_variadic } ->
        get_tyvars env t_variadic |> fun init ->
        List.fold_left t_required ~init ~f:get_tyvars_union |> fun init ->
        List.fold_left t_optional ~f:get_tyvars_union ~init
      | Ttuple { t_required; t_optional; t_extra = Tsplat t_splat } ->
        get_tyvars env t_splat |> fun init ->
        List.fold_left t_required ~f:get_tyvars_union ~init |> fun init ->
        List.fold_left t_optional ~f:get_tyvars_union ~init
      | Tunion tyl
      | Tintersection tyl ->
        List.fold_left
          tyl
          ~init:(env, Tvid.Set.empty, Tvid.Set.empty)
          ~f:get_tyvars_union
      | Tshape (Shape_simple { s_fields = m; _ }) ->
        TShapeMap.fold
          (fun _ { sft_ty; _ } res -> get_tyvars_union res sft_ty)
          m
          (env, Tvid.Set.empty, Tvid.Set.empty)
      | Tshape (Shape_splat { ss_elems }) ->
        List.fold_left
          ss_elems
          ~init:(env, Tvid.Set.empty, Tvid.Set.empty)
          ~f:get_tyvars_union
      | Tfun ft ->
        let (env, params_positive, params_negative) =
          List.fold_left
            ft.ft_params
            ~init:(env, Tvid.Set.empty, Tvid.Set.empty)
            ~f:get_tyvars_param
        in
        let (env, implicit_params_positive, implicit_params_negative) =
          match ft.ft_implicit_params.capability with
          | CapDefaults _ -> (env, Tvid.Set.empty, Tvid.Set.empty)
          | CapTy ty -> get_tyvars env ty
        in
        let (env, ret_positive, ret_negative) = get_tyvars env ft.ft_ret in
        ( env,
          Tvid.Set.union
            implicit_params_positive
            (Tvid.Set.union ret_positive params_positive),
          Tvid.Set.union
            implicit_params_negative
            (Tvid.Set.union ret_negative params_negative) )
      | Tnewtype (name, tyl, _) ->
        if List.is_empty tyl then
          (env, Tvid.Set.empty, Tvid.Set.empty)
        else begin
          match get_typedef env name with
          | Decl_entry.Found { td_tparams; _ } ->
            let variancel = List.map td_tparams ~f:(fun t -> t.tp_variance) in
            get_tyvars_variance_list
              (env, Tvid.Set.empty, Tvid.Set.empty)
              variancel
              tyl
          | Decl_entry.DoesNotExist
          | Decl_entry.NotYetAvailable ->
            (env, Tvid.Set.empty, Tvid.Set.empty)
        end
      | Tdependent (_, ty) -> get_tyvars env ty
      | Tgeneric _ -> (env, Tvid.Set.empty, Tvid.Set.empty)
      | Tclass ((_, cid), _, tyl) ->
        if List.is_empty tyl then
          (env, Tvid.Set.empty, Tvid.Set.empty)
        else begin
          match get_class env cid with
          | Decl_entry.Found cls ->
            let variancel =
              List.map (Cls.tparams cls) ~f:(fun t -> t.tp_variance)
            in
            get_tyvars_variance_list
              (env, Tvid.Set.empty, Tvid.Set.empty)
              variancel
              tyl
          | Decl_entry.DoesNotExist
          | Decl_entry.NotYetAvailable ->
            (env, Tvid.Set.empty, Tvid.Set.empty)
        end
      | Tvec_or_dict (ty1, ty2) ->
        let (env, positive1, negative1) = get_tyvars env ty1 in
        let (env, positive2, negative2) = get_tyvars env ty2 in
        ( env,
          Tvid.Set.union positive1 positive2,
          Tvid.Set.union negative1 negative2 )
      | Tlabel _name -> (env, Tvid.Set.empty, Tvid.Set.empty)
      | Taccess (ty, _ids) -> get_tyvars env ty
      | Tclass_ptr ty -> get_tyvars env ty)
    | ConstraintType ty ->
      (match deref_constraint_type ty with
      | (_, Tdestructure { d_required; d_optional; d_variadic; d_kind = _ }) ->
        let (env, positive1, negative1) =
          List.fold_left
            d_required
            ~init:(env, Tvid.Set.empty, Tvid.Set.empty)
            ~f:get_tyvars_union
        in
        let (env, positive2, negative2) =
          List.fold_left
            d_optional
            ~init:(env, Tvid.Set.empty, Tvid.Set.empty)
            ~f:get_tyvars_union
        in
        let (env, positive3, negative3) =
          match d_variadic with
          | Some ty -> get_tyvars env ty
          | None -> (env, Tvid.Set.empty, Tvid.Set.empty)
        in
        ( env,
          unions [positive1; positive2; positive3],
          unions [negative1; negative2; negative3] )
      | (_, Thas_member hm) ->
        let { hm_type; hm_name = _; hm_class_id = _; hm_method = _ } = hm in
        get_tyvars env hm_type
      | (_, Thas_type_member htm) ->
        let { htm_id = _; htm_lower; htm_upper } = htm in
        let (env, poslo, neglo) = get_tyvars env htm_lower in
        let (env, posup, negup) = get_tyvars env htm_upper in
        (env, Tvid.Set.union posup neglo, Tvid.Set.union poslo negup)
      | (_, Tcan_index ci) ->
        let (env, pos1, neg1) = get_tyvars env ci.ci_key in
        let (env, pos2, neg2) = get_tyvars env ci.ci_val in
        (env, Tvid.Set.union pos1 pos2, Tvid.Set.union neg1 neg2)
      | (_, Tcan_index_assign cia) ->
        let (env, pos1, neg1) = get_tyvars env cia.cia_key in
        let (env, pos2, neg2) = get_tyvars env cia.cia_write in
        let (env, pos3, neg3) = get_tyvars env cia.cia_val in
        (env, unions [pos1; pos2; pos3], unions [neg1; neg2; neg3])
      | (_, Tcan_traverse ct) ->
        let (env, pos1, neg1) = get_tyvars env ct.ct_val in
        let (env, pos2, neg2) =
          match ct.ct_key with
          | None -> (env, Tvid.Set.empty, Tvid.Set.empty)
          | Some ct_key -> get_tyvars env ct_key
        in
        (env, Tvid.Set.union pos1 pos2, Tvid.Set.union neg1 neg2)
      | (_, Ttype_switch { predicate = _; ty_true; ty_false }) ->
        let (env, positive1, negative1) = get_tyvars env ty_true in
        let (env, positive2, negative2) = get_tyvars env ty_false in
        ( env,
          Tvid.Set.union positive1 positive2,
          Tvid.Set.union negative1 negative2 )
      | (_, Thas_const { name = _; ty }) -> get_tyvars env ty)

  and get_tyvars_variance_list (env, acc_positive, acc_negative) variancel tyl =
    match (variancel, tyl) with
    | (variance :: variancel, ty :: tyl) ->
      let (env, positive, negative) = get_tyvars env ty in
      let (acc_positive, acc_negative) =
        match variance with
        | Ast_defs.Covariant ->
          ( Tvid.Set.union acc_positive positive,
            Tvid.Set.union acc_negative negative )
        | Ast_defs.Contravariant ->
          ( Tvid.Set.union acc_positive negative,
            Tvid.Set.union acc_negative positive )
        | Ast_defs.Invariant ->
          let positive_or_negative = Tvid.Set.union positive negative in
          ( Tvid.Set.union acc_positive positive_or_negative,
            Tvid.Set.union acc_negative positive_or_negative )
      in
      get_tyvars_variance_list (env, acc_positive, acc_negative) variancel tyl
    | _ -> (env, acc_positive, acc_negative)

  let rec set_tyvar_appears_covariantly_and_propagate env var =
    if get_tyvar_appears_covariantly env var then
      env
    else
      let env = set_tyvar_appears_covariantly env var in
      let lower_bounds = get_tyvar_lower_bounds env var in
      update_variance_of_tyvars_occurring_in_lower_bounds env lower_bounds

  and set_tyvar_appears_contravariantly_and_propagate env var =
    if get_tyvar_appears_contravariantly env var then
      env
    else
      let env = set_tyvar_appears_contravariantly env var in
      let upper_bounds = get_tyvar_upper_bounds env var in
      update_variance_of_tyvars_occurring_in_upper_bounds env upper_bounds

  and update_variance_of_tyvars_occurring_in_lower_bounds env tys =
    ITySet.fold
      (fun ty env -> update_variance_of_tyvars_occurring_in_lower_bound env ty)
      tys
      env

  and update_variance_of_tyvars_occurring_in_upper_bounds env tys =
    ITySet.fold
      (fun ty env -> update_variance_of_tyvars_occurring_in_upper_bound env ty)
      tys
      env

  and update_variance_of_tyvars_occurring_in_lower_bound env ty =
    let (env, ety) = expand_internal_type env ty in
    match ety with
    | LoclType ty when is_tyvar ty -> env
    | _ ->
      let (env, positive, negative) = get_tyvars_i env ty in
      let env =
        Tvid.Set.fold
          (fun var env -> set_tyvar_appears_covariantly env var)
          positive
          env
      in
      let env =
        Tvid.Set.fold
          (fun var env -> set_tyvar_appears_contravariantly env var)
          negative
          env
      in
      env

  and update_variance_of_tyvars_occurring_in_upper_bound env ty =
    let (env, ety) = expand_internal_type env ty in
    match ety with
    | LoclType ty when is_tyvar ty -> env
    | _ ->
      let (env, positive, negative) = get_tyvars_i env ty in
      let env =
        Tvid.Set.fold
          (fun var env -> set_tyvar_appears_contravariantly env var)
          positive
          env
      in
      let env =
        Tvid.Set.fold
          (fun var env -> set_tyvar_appears_covariantly env var)
          negative
          env
      in
      env

  let set_tyvar_appears_covariantly =
    set_tyvar_appears_covariantly_and_propagate

  let set_tyvar_appears_contravariantly =
    set_tyvar_appears_contravariantly_and_propagate

  (* After a type variable var has been "solved", or bound to a type ty, we need
   * to update the variance of type variables occurring in ty. Suppose that
   * variable var is marked "appears covariantly", i.e. it appears (at least) in
   * positive positions in the type of an expression. Then when we substitute ty
   * for var, variables that appear positively in ty must now be marked as
   * appearing covariantly; variables that appear negatively in ty must now be
   * marked as appearing contravariantly. And the dual, if the variable var is marked
   * "appears contravariantly".
   *)
  let update_variance_after_bind env var ty =
    let appears_contravariantly = get_tyvar_appears_contravariantly env var in
    let appears_covariantly = get_tyvar_appears_covariantly env var in
    let (env, positive, negative) = get_tyvars env ty in
    let env =
      Tvid.Set.fold
        (fun var env ->
          let env =
            if appears_contravariantly then
              set_tyvar_appears_contravariantly env var
            else
              env
          in
          if appears_covariantly then
            set_tyvar_appears_covariantly env var
          else
            env)
        positive
        env
    in
    let env =
      Tvid.Set.fold
        (fun var env ->
          let env =
            if appears_contravariantly then
              set_tyvar_appears_covariantly env var
            else
              env
          in
          if appears_covariantly then
            set_tyvar_appears_contravariantly env var
          else
            env)
        negative
        env
    in
    env

  let set_tyvar_variance_i env ?(flip = false) ?(for_all_vars = false) ty =
    log_env_change "set_tyvar_variance" env
    @@
    let current = get_current_tyvars env in
    if (not for_all_vars) && List.is_empty current then
      env
    else
      let (env, positive, negative) = get_tyvars_i env ty in
      let (positive, negative) =
        if flip then
          (negative, positive)
        else
          (positive, negative)
      in
      let tyvars =
        if for_all_vars then
          Tvid.Set.union positive negative |> Tvid.Set.elements
        else
          current
      in
      List.fold_left tyvars ~init:env ~f:(fun env var ->
          let env =
            if Tvid.Set.mem var positive then
              set_tyvar_appears_covariantly env var
            else
              env
          in
          let env =
            if Tvid.Set.mem var negative then
              set_tyvar_appears_contravariantly env var
            else
              env
          in
          env)

  let set_tyvar_variance env ?(flip = false) ?(for_all_vars = false) ty =
    set_tyvar_variance_i env ~flip ~for_all_vars (LoclType ty)

  let add_tyvar_upper_bound ?intersect env var (ty : internal_type) =
    log_env_change "add_tyvar_upper_bound" env
    @@ {
         env with
         inference_env =
           Inf.add_tyvar_upper_bound ?intersect env.inference_env var ty;
       }

  (* Add a single new upper bound [ty] to type variable [var] in [env.inference_env].
   * If the optional [intersect] operation is supplied, then use this to avoid
   * adding redundant bounds by merging the type with existing bounds. This makes
   * sense because a conjunction of upper bounds
   *   (v <: t1) /\ ... /\ (v <: tn)
   * is equivalent to a single upper bound
   *   v <: (t1 & ... & tn)
   *)
  let add_tyvar_upper_bound_and_update_variances
      ?intersect env var (ty : internal_type) =
    log_env_change "add_tyvar_upper_bound" env
    @@
    let env = add_tyvar_upper_bound ?intersect env var ty in
    if get_tyvar_appears_contravariantly env var then
      update_variance_of_tyvars_occurring_in_upper_bound env ty
    else
      env

  (* Remove type variable `upper_var` from the upper bounds on `var`, if it exists
*)
  let remove_tyvar_upper_bound env var upper_var =
    log_env_change "remove_tyvar_upper_bound" env
    @@ {
         env with
         inference_env =
           Inf.remove_tyvar_upper_bound env.inference_env var upper_var;
       }

  (* Remove type variable `lower_var` from the lower bounds on `var`, if it exists
*)
  let remove_tyvar_lower_bound env var lower_var =
    log_env_change "remove_tyvar_lower_bound var" env
    @@ {
         env with
         inference_env =
           Inf.remove_tyvar_lower_bound env.inference_env var lower_var;
       }

  let add_tyvar_lower_bound ?union env var ty =
    log_env_change "add_tyvar_lower_bound" env
    @@ {
         env with
         inference_env =
           Inf.add_tyvar_lower_bound ?union env.inference_env var ty;
       }

  (* Add a single new lower bound [ty] to type variable [var] in [env.tvenv].
   * If the optional [union] operation is supplied, then use this to avoid
   * adding redundant bounds by merging the type with existing bounds. This makes
   * sense because a conjunction of lower bounds
   *   (t1 <: v) /\ ... /\ (tn <: v)
   * is equivalent to a single lower bound
   *   (t1 | ... | tn) <: v
   *)
  let add_tyvar_lower_bound_and_update_variances ?union env var ty =
    log_env_change "add_tyvar_lower_bound" env
    @@
    let env = add_tyvar_lower_bound ?union env var ty in
    if get_tyvar_appears_covariantly env var then
      update_variance_of_tyvars_occurring_in_lower_bound env ty
    else
      env

  let get_all_tyvars env = Inf.get_vars env.inference_env

  let remove_var env var ~search_in_upper_bounds_of ~search_in_lower_bounds_of =
    log_env_change "remove_var" env
    @@ {
         env with
         inference_env =
           Inf.remove_var
             env.inference_env
             var
             ~search_in_upper_bounds_of
             ~search_in_lower_bounds_of;
       }

  module Log = struct
    let tyvar_to_json
        (p_locl_ty : locl_ty -> string)
        (p_internal_type : internal_type -> string)
        (env : env)
        (v : Tvid.t) =
      Inf.Log.tyvar_to_json p_locl_ty p_internal_type env.inference_env v

    let expand_env
        env
        {
          Typing_defs.type_expansions;
          make_internal_opaque;
          visibility_behavior;
          substs = _;
          no_substs = _;
          this_ty;
          on_error = _;
          wildcard_action = _;
          ish_weakening;
          under_type_constructor = _;
          simplify_intersections = _;
        } =
      if Typing_env_types.get_log_level env "expand_env" |> Int.( = ) 0 then
        []
      else
        [
          ("visibility_behavior", show_visibility_behavior visibility_behavior);
          ("this_ty", Typing_print.debug env this_ty);
          ("type_expansions", Type_expansions.to_log_string type_expansions);
          ("make_internal_opaque", Bool.to_string make_internal_opaque);
          ("ish_weakening", Bool.to_string ish_weakening);
        ]
  end

  let update_reason _ ty ~f = map_reason ty ~f

  let update_cty_reason _ cty ~f =
    let (r, cstr) = deref_constraint_type cty in
    mk_constraint_type (f r, cstr)

  let update_ity_reason t ity ~f =
    match ity with
    | LoclType lty -> LoclType (update_reason t lty ~f)
    | ConstraintType cty -> ConstraintType (update_cty_reason t cty ~f)

  (** see .mli **)
  let inside_expr_tree env expr_tree_hint =
    let outer_locals =
      match next_cont_opt env with
      | None -> LID.Map.empty
      | Some cont -> cont.LEnvC.local_types
    in
    { env with in_expr_tree = Some { dsl = expr_tree_hint; outer_locals } }

  let outside_expr_tree env ~macro_variables =
    let (env, macro_locals) =
      match macro_variables with
      | None -> (env, None)
      | Some lids ->
        let (env, local_env) =
          List.fold_left_env
            env
            lids
            ~init:Typing_local_types.empty
            ~f:(fun env locals (pos, local_id) ->
              let (env, ty) = fresh_type_invariant env pos in
              let local =
                Typing_local_types.
                  {
                    ty;
                    pos;
                    eid = Expression_id.make env.expression_id_provider;
                    macro_splice_vars = None;
                  }
              in
              (env, Typing_local_types.add_to_local_types local_id local locals))
        in
        (env, Some local_env)
    in
    { env with in_expr_tree = None; in_macro_splice = macro_locals }

  let with_inside_expr_tree env expr_tree_hint f =
    let old_in_expr_tree = env.in_expr_tree in
    let env = inside_expr_tree env expr_tree_hint in
    let (env, r1, r2) = f env in
    let env = { env with in_expr_tree = old_in_expr_tree } in
    (env, r1, r2)

  let with_outside_expr_tree env ~macro_variables f =
    let old_in_expr_tree = env.in_expr_tree in
    let old_in_macro_splice = env.in_macro_splice in
    let dsl =
      match old_in_expr_tree with
      | Some { dsl = cls; outer_locals = _ } -> Some cls
      | _ -> None
    in
    let env = outside_expr_tree env ~macro_variables in
    let (env, r1, r2) = f env dsl in
    let new_macro_type_mapping =
      Option.map
        ~f:
          (Local_id.Map.map (fun local ->
               Typing_local_types.(local.pos, local.ty)))
        env.in_macro_splice
    in
    let env =
      {
        env with
        in_expr_tree = old_in_expr_tree;
        in_macro_splice = old_in_macro_splice;
      }
    in
    (env, r1, r2, new_macro_type_mapping)

  (** see .mli **)
  let is_in_expr_tree env = Option.is_some env.in_expr_tree

  let rank_of_tparam env tparam = Type_parameter_env.get_rank env.tpenv tparam
end

include M

(** see .mli **)
module Expose_to_tast_env = M
