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
open Typing_defs_constraints
module ITySet = Internal_type_set
module Occ = Typing_tyvar_occurrences
module TL = Typing_logic

exception InconsistentTypeVarState of string

type tyvar_constraints = {
  appears_covariantly: bool;
      (** Does this type variable appear covariantly in the type of the expression? *)
  appears_contravariantly: bool;
      (** Does this type variable appear contravariantly in the type of the expression?
          If it appears in an invariant position then both will be true; if it doesn't
          appear at all then both will be false. *)
  lower_bounds: ITySet.t;
  upper_bounds: ITySet.t;
  type_constants:
    (pos_id (* id of the type constant "T", containing its position. *)
    * locl_ty)
    S_map.t;
      (** Map associating a type to each type constant id of this variable.
          Whenever we localize "T1::T" in a constraint, we add a fresh type variable
          indexed by "T" in the type_constants of the type variable representing T1.
          This allows to properly check constraints on "T1::T". *)
}

type solving_info =
  | TVIType of locl_ty  (** when the type variable is bound to a type *)
  | TVIConstraints of tyvar_constraints
      (** when the type variable is still unsolved *)

type tyvar_info = {
  tyvar_pos: Pos.t;
      (** Where was the type variable introduced? (e.g. generic method invocation,
          new object construction) *)
  eager_solve_failed: bool;
  solving_info: solving_info;
  is_error: bool;
  is_shadow: bool;
      (** Shadow type variables collect upper-bound constraints for dynamic
          inference but are never solved. Attached to Tdynamic(Some v) types. *)
  shadow_local_name: string option;
      (** If this shadow type variable was created for a local variable, stores
          the variable name (e.g., "$d") for diagnostic output. *)
  rank: int;
}

type tvenv = tyvar_info Tvid.Map.t

type t = {
  tvenv: tvenv;
  tyvars_stack: (Pos.t * Tvid.t list) list;
  subtype_prop: TL.subtype_prop;
  tyvar_occurrences: Typing_tyvar_occurrences.t;
  pos: Pos.t option;
}

module Log = struct
  open Typing_log_value

  let tyvar_constraints_as_value tvcstr =
    let {
      appears_covariantly;
      appears_contravariantly;
      type_constants;
      lower_bounds;
      upper_bounds;
    } =
      tvcstr
    in
    make_map
      [
        ("appears_covariantly", bool_as_value appears_covariantly);
        ("appears_contravariantly", bool_as_value appears_contravariantly);
        ("lower_bounds", internal_type_set_as_value lower_bounds);
        ("upper_bounds", internal_type_set_as_value upper_bounds);
        ( "type_constants",
          smap_as_value (fun (_, ty) -> locl_type_as_value ty) type_constants );
      ]

  let solving_info_as_value sinfo =
    match sinfo with
    | TVIType ty -> variant_as_value "TVIType" (locl_type_as_value ty)
    | TVIConstraints tvcstr ->
      variant_as_value "TVIConstraints" (tyvar_constraints_as_value tvcstr)

  let tyvar_info_as_value tvinfo =
    let {
      tyvar_pos;
      eager_solve_failed;
      solving_info;
      is_error;
      is_shadow;
      shadow_local_name = _;
      rank;
    } =
      tvinfo
    in
    make_map
      [
        ("tyvar_pos", pos_as_value tyvar_pos);
        ("eager_solve_failed", bool_as_value eager_solve_failed);
        ("solving_info", solving_info_as_value solving_info);
        ("is_error", bool_as_value is_error);
        ("is_shadow", bool_as_value is_shadow);
        ("rank", Typing_log_value.Atom (Int.to_string rank));
      ]

  let tvenv_as_value (tvenv : tvenv) =
    Map
      (Tvid.Map.fold
         (fun i tvinfo m ->
           S_map.add (var_as_string i) (tyvar_info_as_value tvinfo) m)
         tvenv
         S_map.empty)

  let tyvars_stack_as_value (tyvars_stack : (Pos.t * Tvid.t list) list) =
    List
      (List.map tyvars_stack ~f:(fun (_p, l) ->
           List (List.map l ~f:(fun i -> Atom (var_as_string i)))))

  let inference_env_as_value env =
    let { tvenv; tyvars_stack; subtype_prop; tyvar_occurrences; pos = _ } =
      env
    in
    make_map
      [
        ("tvenv", tvenv_as_value tvenv);
        ("tyvars_stack", tyvars_stack_as_value tyvars_stack);
        ("subtype_prop", subtype_prop_as_value subtype_prop);
        ("tyvar_occurrences", Occ.Log.as_value tyvar_occurrences);
      ]

  let solving_info_to_json
      (p_locl_ty : locl_ty -> string)
      (p_internal_type : internal_type -> string)
      solving_info =
    let constraints_to_json
        {
          appears_covariantly;
          appears_contravariantly;
          lower_bounds;
          upper_bounds;
          type_constants = _;
        } =
      let bounds_to_json bs =
        ITySet.elements bs |> List.map ~f:(fun b -> `String (p_internal_type b))
      in
      `Assoc
        [
          ("appears_covariantly", `Bool appears_covariantly);
          ("appears_contravariantly", `Bool appears_contravariantly);
          ("lower_bounds", `List (bounds_to_json lower_bounds));
          ("upper_bounds", `List (bounds_to_json upper_bounds));
        ]
    in
    let locl_ty_to_json x = `String (p_locl_ty x) in
    match solving_info with
    | TVIType locl_ty -> `Assoc [("type", locl_ty_to_json locl_ty)]
    | TVIConstraints cstr -> `Assoc [("constraints", constraints_to_json cstr)]

  let tyvar_to_json
      (p_locl_ty : locl_ty -> string)
      (p_internal_type : internal_type -> string)
      (env : t)
      (v : Tvid.t) =
    match Tvid.Map.find_opt v env.tvenv with
    | None -> `Null
    | Some
        {
          tyvar_pos;
          eager_solve_failed;
          solving_info;
          is_error;
          is_shadow;
          shadow_local_name = _;
          rank;
        } ->
      `Assoc
        [
          ("tyvar_pos", `String (Pos.string (Pos.to_absolute tyvar_pos)));
          ("eager_solve_failed", `Bool eager_solve_failed);
          ( "solving_info",
            solving_info_to_json p_locl_ty p_internal_type solving_info );
          ("is_error", `Bool is_error);
          ("is_shadow", `Bool is_shadow);
          ("rank", `Intlit (Int.to_string rank));
        ]
end

let empty_tvenv = Tvid.Map.empty

let empty_inference_env =
  {
    tvenv = empty_tvenv;
    tyvars_stack = [];
    subtype_prop = TL.valid;
    tyvar_occurrences = Occ.init;
    pos = None;
  }

let empty_tyvar_constraints =
  {
    lower_bounds = ITySet.empty;
    upper_bounds = ITySet.empty;
    appears_covariantly = false;
    appears_contravariantly = false;
    type_constants = S_map.empty;
  }

let empty_tyvar_info tyvar_pos rank =
  {
    tyvar_pos;
    rank;
    eager_solve_failed = false;
    solving_info = TVIConstraints empty_tyvar_constraints;
    is_error = false;
    is_shadow = false;
    shadow_local_name = None;
  }

let get_tyvar_info_opt env v = Tvid.Map.find_opt v env.tvenv

let set_tyvar_info env v tvinfo =
  { env with tvenv = Tvid.Map.add v tvinfo env.tvenv }

let get_solving_info_opt env var =
  match get_tyvar_info_opt env var with
  | None -> None
  | Some tvinfo -> Some tvinfo.solving_info

let set_solving_info env ?(tyvar_pos = Pos.none) x solving_info =
  let tvinfo =
    Option.value
      (get_tyvar_info_opt env x)
      ~default:(empty_tyvar_info tyvar_pos 0)
  in
  let tvinfo = { tvinfo with solving_info } in
  set_tyvar_info env x tvinfo

let tyvar_occurs_in_tyvar env = Occ.occurs_in env.tyvar_occurrences

let get_tyvar_occurrences env = Occ.get_tyvar_occurrences env.tyvar_occurrences

let get_tyvars_in_tyvar env = Occ.get_tyvars_in_tyvar env.tyvar_occurrences

let contains_unsolved_tyvars env =
  Occ.contains_unsolved_tyvars env.tyvar_occurrences

let tyvar_is_solved env var =
  match get_solving_info_opt env var with
  | None -> false
  | Some sinfo ->
    (match sinfo with
    | TVIConstraints _ -> false
    | TVIType _ -> true)

(** Get type variables in a type that are either unsolved or
solved to a type that itself contains unsolved type variables. *)
let get_unsolved_vars_in_ty env ty =
  let gatherer =
    object
      inherit [Tvid.Set.t] Type_visitor.locl_type_visitor

      method! on_tvar vars _r v =
        (* Add it if it has unsolved type vars or if it is itself unsolved. *)
        if
          (not (tyvar_is_solved env v))
          || Occ.contains_unsolved_tyvars env.tyvar_occurrences v
        then
          Tvid.Set.add v vars
        else
          vars
    end
  in
  gatherer#on_type Tvid.Set.empty ty

let make_tyvars_occur_in_tyvar env vars ~occur_in:x =
  {
    env with
    tyvar_occurrences =
      Occ.make_tyvars_occur_in_tyvar env.tyvar_occurrences vars ~occur_in:x;
  }

let make_tyvar_no_more_occur_in_tyvar env v ~no_more_in:v' =
  {
    env with
    tyvar_occurrences =
      Occ.make_tyvar_no_more_occur_in_tyvar
        env.tyvar_occurrences
        v
        ~no_more_in:v';
  }

let get_direct_binding env v =
  match get_solving_info_opt env v with
  | None
  | Some (TVIConstraints _) ->
    None
  | Some (TVIType ty) -> Some ty

let bind env ?(tyvar_pos = Pos.none) v ty =
  set_solving_info env ~tyvar_pos v (TVIType ty)

let update_tyvar_occurrences env v ty =
  let env =
    { env with tyvar_occurrences = Occ.unbind_tyvar env.tyvar_occurrences v }
  in
  let unsolved_vars_in_ty = get_unsolved_vars_in_ty env ty in
  let env = make_tyvars_occur_in_tyvar env unsolved_vars_in_ty ~occur_in:v in
  env

let add env ?(tyvar_pos = Pos.none) v ty =
  let env = bind env ~tyvar_pos v ty in
  let env = update_tyvar_occurrences env v ty in
  env

(** Call this from any functions below that can produce InconsistentTypeVarState
* That way we can enforce in tests that we are not making
* unactionable error messages in prod (errors without even filenames are hard to repro)
*)
let check_env_pos_invariant env =
  match env.pos with
  | Some pos when Pos.equal pos Pos.none ->
    Hack_event_logger.invariant_violation_bug
      "missing env.pos. Make sure we're propagating positions correctly, see D91705550."
  | None ->
    Hack_event_logger.invariant_violation_bug
      "expected Some pos, but env.pos is None. Make sure we're propagating positions correctly, see D91705550."
  | Some _ -> ()

let get_type env reason_in tvid_in =
  check_env_pos_invariant env;
  let rec get r v aliases =
    let shorten_paths () =
      Tvid.Set.fold (fun v' env -> add env v' (mk (r, Tvar v))) aliases env
    in
    match get_solving_info_opt env v with
    | Some (TVIType ty) -> begin
      match deref ty with
      | (r, Tvar v') ->
        if Tvid.Set.mem v aliases then
          raise
          @@ InconsistentTypeVarState
               "Two type variables are aliasing each other!";
        get r v' (Tvid.Set.add v aliases)
      | _ ->
        (* When we have a concrete solution r, record the flow of that solution as a
           prefix to the original type variables's reason; when report in an error
           we should then see the path of the relevant uppper / lower bounds
           as the prefix for any flow through typing.
           We don't record the flow when we are pointing at another tyvar on the
           heap since we would end up with a chain of flows representing unification *)
        let ty =
          map_reason ty ~f:(fun solution ->
              Typing_reason.(solved tvid_in ~solution ~in_:reason_in))
        in
        let env = shorten_paths () in
        (env, ty)
    end
    | None
    | Some (TVIConstraints _) ->
      let env = shorten_paths () in
      (env, mk (r, Tvar v))
  in
  get reason_in tvid_in Tvid.Set.empty

let create_tyvar_constraints variance =
  let (appears_covariantly, appears_contravariantly) =
    match variance with
    | Some Ast_defs.Invariant -> (true, true)
    | Some Ast_defs.Covariant -> (true, false)
    | Some Ast_defs.Contravariant -> (false, true)
    | None -> (false, false)
  in
  let tyvar_constraints =
    {
      empty_tyvar_constraints with
      appears_covariantly;
      appears_contravariantly;
    }
  in
  TVIConstraints tyvar_constraints

let fresh_unsolved_tyvar env v rank ?variance ?(is_error = false) tyvar_pos =
  let solving_info = create_tyvar_constraints variance in
  let tvinfo =
    {
      tyvar_pos;
      solving_info;
      eager_solve_failed = false;
      is_error;
      is_shadow = false;
      shadow_local_name = None;
      rank;
    }
  in
  set_tyvar_info env v tvinfo

(** Create a shadow type variable — a type variable that only accumulates upper
    bounds and is excluded from solving. Used for dynamic inference: tracks how
    dynamic-typed expressions are used, building a constraint set that describes
    their inferred use-type. *)
let fresh_shadow_tyvar env id_provider ?local_name pos =
  let v = Tvid.make id_provider in
  let solving_info = create_tyvar_constraints (Some Ast_defs.Invariant) in
  let tvinfo =
    {
      tyvar_pos = pos;
      solving_info;
      eager_solve_failed = false;
      is_error = false;
      is_shadow = true;
      shadow_local_name = local_name;
      rank = 0;
    }
  in
  let env = set_tyvar_info env v tvinfo in
  (env, v)

let is_shadow env v =
  match get_tyvar_info_opt env v with
  | Some tvinfo -> tvinfo.is_shadow
  | None -> false

let iter_shadow_tyvars env f =
  Tvid.Map.iter (fun v tvinfo -> if tvinfo.is_shadow then f v) env.tvenv

let collect_shadow_locals env =
  Tvid.Map.fold
    (fun v tvinfo acc ->
      match tvinfo.shadow_local_name with
      | Some name -> (name, tvinfo.tyvar_pos, v) :: acc
      | None -> acc)
    env.tvenv
    []

let add_current_tyvar ?variance ?is_error env p v rank =
  let env = fresh_unsolved_tyvar env v rank ?variance ?is_error p in
  match env.tyvars_stack with
  | (expr_pos, tyvars) :: rest ->
    { env with tyvars_stack = (expr_pos, v :: tyvars) :: rest }
  | _ -> env

let fresh_type_reason ?variance ?is_error env id_provider p mk_reason =
  let v = Tvid.make id_provider in
  let r = mk_reason v in
  let env = add_current_tyvar ?variance ?is_error env p v 0 in
  (env, mk (r, Tvar v))

let fresh_type ?variance env id_provider p =
  fresh_type_reason
    env
    id_provider
    p
    (Reason.type_variable p)
    ?variance
    ~is_error:false

let fresh_type_invariant = fresh_type ~variance:Ast_defs.Invariant

let fresh_type_invariant_with_rank env provider rank pos =
  let tvid = Tvid.make provider in
  let reason = Typing_reason.type_variable pos tvid in
  let env =
    add_current_tyvar
      ~variance:Ast_defs.Invariant
      ~is_error:false
      env
      pos
      tvid
      rank
  in
  (env, mk (reason, Tvar tvid))

let wrap_ty_in_var env id_provider r ty =
  let v = Tvid.make id_provider in
  let tvinfo =
    {
      tyvar_pos = Reason.to_pos r |> Pos_or_decl.unsafe_to_raw_pos;
      eager_solve_failed = false;
      solving_info = TVIType ty;
      is_error = false;
      is_shadow = false;
      shadow_local_name = None;
      rank = 0;
    }
  in
  let env = set_tyvar_info env v tvinfo in
  let env = update_tyvar_occurrences env v ty in
  (env, mk (r, Tvar v))

let open_tyvars env p = { env with tyvars_stack = (p, []) :: env.tyvars_stack }

let close_tyvars env =
  check_env_pos_invariant env;
  match env.tyvars_stack with
  | [] -> raise @@ InconsistentTypeVarState "close_tyvars: empty stack"
  | _ :: rest -> { env with tyvars_stack = rest }

let get_current_tyvars env =
  match env.tyvars_stack with
  | [] -> []
  | (_, tyvars) :: _ -> tyvars

let get_tyvar_constraints_opt env var =
  check_env_pos_invariant env;
  match get_solving_info_opt env var with
  | Some (TVIConstraints constraints) -> Some constraints
  | None
  | Some (TVIType _) ->
    None

let get_tyvar_constraints_exn env var =
  check_env_pos_invariant env;
  let error msg =
    match env.pos with
    | None ->
      InconsistentTypeVarState
        (Format.sprintf
           "Attempting to get constraints for %s type variable %s."
           msg
           (Typing_log_value.var_as_string var))
    | Some pos ->
      InconsistentTypeVarState
        (Format.sprintf
           "Attempting to get constraints for %s type variable %s at %s"
           msg
           (Typing_log_value.var_as_string var)
           (Pos.string (Pos.to_absolute pos)))
  in
  match get_solving_info_opt env var with
  | None -> raise (error "non-existing")
  | Some (TVIType _) -> raise (error "already solved")
  | Some (TVIConstraints constraints) -> constraints

let set_tyvar_constraints env v tyvar_constraints =
  set_solving_info env v (TVIConstraints tyvar_constraints)

let get_tyvar_eager_solve_fail env v =
  match get_tyvar_info_opt env v with
  | None -> false
  | Some tvinfo -> tvinfo.eager_solve_failed

let expand_var env r v =
  let (env, ty) = get_type env r v in
  if get_tyvar_eager_solve_fail env v then
    (env, mk (Reason.solve_fail (Reason.to_pos r), get_node ty))
  else
    (env, ty)

let expand_type env x =
  match deref x with
  | (r, Tvar x) -> expand_var env r x
  | _ -> (env, x)

let full_expander =
  object (this)
    inherit [t] Type_mapper_generic.internal_type_mapper

    method! on_tvar env r v =
      let (env, ty) = expand_var env r v in
      match get_node ty with
      | Tvar _ -> (env, ty)
      | _ -> this#on_type env ty
  end

let fully_expand_type env ty = full_expander#on_type env ty

let expand_internal_type env ty =
  match ty with
  | ConstraintType _ -> (env, ty)
  | LoclType ty ->
    let (env, ty) = expand_type env ty in
    (env, LoclType ty)

let get_tyvar_pos env var =
  match get_tyvar_info_opt env var with
  | None -> Pos.none
  | Some tvinfo -> tvinfo.tyvar_pos

let get_tyvar_lower_bounds_opt env v =
  Option.map (get_tyvar_constraints_opt env v) ~f:(fun x -> x.lower_bounds)

let get_tyvar_upper_bounds_opt env v =
  Option.map (get_tyvar_constraints_opt env v) ~f:(fun x -> x.upper_bounds)

let get_tyvar_lower_bounds env var : ITySet.t =
  match get_solving_info_opt env var with
  | None -> ITySet.empty
  | Some solving_info ->
    (match solving_info with
    | TVIType ty -> ITySet.singleton (LoclType ty)
    | TVIConstraints cstr -> cstr.lower_bounds)

let get_tyvar_upper_bounds env var : ITySet.t =
  match get_solving_info_opt env var with
  | None -> ITySet.empty
  | Some solving_info ->
    (match solving_info with
    | TVIType ty -> ITySet.singleton (LoclType ty)
    | TVIConstraints cstr -> cstr.upper_bounds)

let set_tyvar_lower_bounds env var lower_bounds =
  match get_tyvar_constraints_opt env var with
  | None -> env
  | Some tyvar_constraints ->
    let tyvar_constraints = { tyvar_constraints with lower_bounds } in
    set_tyvar_constraints env var tyvar_constraints

let set_tyvar_upper_bounds env var upper_bounds =
  match get_tyvar_constraints_opt env var with
  | None -> env
  | Some tyvar_constraints ->
    let tyvar_constraints = { tyvar_constraints with upper_bounds } in
    set_tyvar_constraints env var tyvar_constraints

let set_tyvar_eager_solve_fail env v =
  match get_tyvar_info_opt env v with
  | None -> env
  | Some tvinfo ->
    let tvinfo = { tvinfo with eager_solve_failed = true } in
    set_tyvar_info env v tvinfo

let get_tyvar_appears_covariantly env var =
  match get_tyvar_constraints_opt env var with
  | Some cstr -> cstr.appears_covariantly
  | None -> false

let get_tyvar_appears_contravariantly env var =
  match get_tyvar_constraints_opt env var with
  | Some cstr -> cstr.appears_contravariantly
  | None -> false

let get_tyvar_appears_invariantly env var =
  get_tyvar_appears_covariantly env var
  && get_tyvar_appears_contravariantly env var

let set_tyvar_appears_covariantly env v =
  let tyvar_constraints = get_tyvar_constraints_exn env v in
  let tyvar_constraints =
    { tyvar_constraints with appears_covariantly = true }
  in
  let env = set_tyvar_constraints env v tyvar_constraints in
  env

let set_tyvar_appears_contravariantly env v =
  let tyvar_constraints = get_tyvar_constraints_exn env v in
  let tyvar_constraints =
    { tyvar_constraints with appears_contravariantly = true }
  in
  let env = set_tyvar_constraints env v tyvar_constraints in
  env

let get_tyvar_type_consts env var =
  match get_tyvar_constraints_opt env var with
  | Some cstr -> cstr.type_constants
  | None -> S_map.empty

let get_tyvar_type_const env var (_, tyconstid) =
  S_map.find_opt tyconstid (get_tyvar_type_consts env var)

let set_tyvar_type_const env var ((_, tyconstid_) as tyconstid) ty =
  let tvinfo = get_tyvar_constraints_exn env var in
  let type_constants =
    S_map.add tyconstid_ (tyconstid, ty) tvinfo.type_constants
  in
  let env = set_tyvar_constraints env var { tvinfo with type_constants } in
  (* We don't want to solve such type variables too early, because of valid
   * code patterns like that one:
   *
   * abstract class A { abstract type const T; }
   *
   * function f<TA as A, T>(TA $_, T $_) : ... where T = TA::T
   *
   * We force it covariantly until we can implement a proper fix to
   * avoid eager solving (to the upperbound) as it would bind TA to
   * an abstract class
   *)
  let env = set_tyvar_appears_covariantly env var in
  env

(** see .mli **)
let add_subtype_prop env prop =
  { env with subtype_prop = TL.conj env.subtype_prop prop }

let get_current_pos_from_tyvar_stack env =
  match env.tyvars_stack with
  | (p, _) :: _ -> Some p
  | _ -> None

(* Add a single new upper bound [ty] to type variable [var] in [env.tvenv].
 * If the optional [intersect] operation is supplied, then use this to avoid
 * adding redundant bounds by merging the type with existing bounds. This makes
 * sense because a conjunction of upper bounds
 *   (v <: t1) /\ ... /\ (v <: tn)
 * is equivalent to a single upper bound
 *   v <: (t1 & ... & tn)
 *)
let add_tyvar_upper_bound ?intersect env var (ty : internal_type) =
  let tvconstraints = get_tyvar_constraints_exn env var in
  let upper_bounds =
    match intersect with
    | None -> ITySet.add ty tvconstraints.upper_bounds
    | Some intersect ->
      ITySet.of_list (intersect ty (ITySet.elements tvconstraints.upper_bounds))
  in
  let env = set_tyvar_constraints env var { tvconstraints with upper_bounds } in
  env

(* Add a single new lower bound [ty] to type variable [var] in [env.tvenv].
 * If the optional [union] operation is supplied, then use this to avoid
 * adding redundant bounds by merging the type with existing bounds. This makes
 * sense because a conjunction of lower bounds
 *   (t1 <: v) /\ ... /\ (tn <: v)
 * is equivalent to a single lower bound
 *   (t1 | ... | tn) <: v
 *)
let add_tyvar_lower_bound ?union env var ty =
  let tvconstraints = get_tyvar_constraints_exn env var in
  let lower_bounds =
    match union with
    | None -> ITySet.add ty tvconstraints.lower_bounds
    | Some union ->
      ITySet.of_list (union ty (ITySet.elements tvconstraints.lower_bounds))
  in
  let env = set_tyvar_constraints env var { tvconstraints with lower_bounds } in
  env

(* Remove type variable `upper_var` from the upper bounds on `var`, if it exists
 *)
let remove_tyvar_upper_bound env var upper_var =
  match get_tyvar_constraints_opt env var with
  | None -> env
  | Some tvconstraints ->
    let upper_bounds =
      ITySet.filter
        (fun ty ->
          let (_env, ty) = expand_internal_type env ty in
          not @@ InternalType.is_var_v ty ~v:upper_var)
        tvconstraints.upper_bounds
    in
    set_tyvar_constraints env var { tvconstraints with upper_bounds }

(* Remove type variable `lower_var` from the lower bounds on `var`, if it exists
 *)
let remove_tyvar_lower_bound env var lower_var =
  match get_tyvar_constraints_opt env var with
  | None -> env
  | Some tvconstraints ->
    let lower_bounds =
      ITySet.filter
        (fun ty ->
          let (_env, ty) = expand_internal_type env ty in
          not @@ InternalType.is_var_v ty ~v:lower_var)
        tvconstraints.lower_bounds
    in
    set_tyvar_constraints env var { tvconstraints with lower_bounds }

let get_vars (env : t) = Tvid.Map.keys env.tvenv

let get_unsolved_vars (env : t) : Tvid.t list =
  Tvid.Map.fold
    (fun v tvinfo vars ->
      match tvinfo.solving_info with
      | TVIConstraints _ -> v :: vars
      | TVIType _ -> vars)
    env.tvenv
    []

module Size = struct
  (* This can be useful to debug type which blow up in size *)
  let ty_size env ty =
    let ty_size_visitor =
      object
        inherit [int] Type_visitor.locl_type_visitor as super

        method! on_type acc ty = 1 + super#on_type acc ty

        method! on_tvar acc r v =
          let (_, ty) = expand_var env r v in
          match get_node ty with
          | Tvar v' when Tvid.equal v' v -> acc
          | _ -> super#on_type acc ty
      end
    in
    ty_size_visitor#on_type 0 ty

  let constraint_type_size env ty =
    let type_size_list env l =
      List.fold ~init:0 ~f:(fun size ty -> size + ty_size env ty) l
    in
    let type_size_option ~f opt = Option.value_map ~default:0 ~f opt in
    match deref_constraint_type ty with
    | (_, Tdestructure d) ->
      let { d_required; d_optional; d_variadic; _ } = d in
      1
      + type_size_list env d_required
      + type_size_list env d_optional
      + type_size_option ~f:(ty_size env) d_variadic
    | (_, Thas_member hm) -> 1 + ty_size env hm.hm_type
    | (_, Thas_type_member htm) ->
      1 + ty_size env htm.htm_lower + ty_size env htm.htm_upper
    | (_, Tcan_index ci) -> 1 + ty_size env ci.ci_val + ty_size env ci.ci_key
    | (_, Tcan_index_assign cia) ->
      1
      + ty_size env cia.cia_key
      + ty_size env cia.cia_write
      + ty_size env cia.cia_val
    | (_, Tcan_traverse ct) ->
      1 + ty_size env ct.ct_val + type_size_option ~f:(ty_size env) ct.ct_key
    | (_, Ttype_switch { predicate = _; ty_true; ty_false }) ->
      1 + ty_size env ty_true + ty_size env ty_false
    | (_, Thas_const { name = _; ty }) -> 1 + ty_size env ty

  let internal_type_size env ty =
    match ty with
    | LoclType ty -> ty_size env ty
    | ConstraintType ty -> constraint_type_size env ty

  let internal_type_set_size env tys =
    ITySet.elements tys
    |> List.map ~f:(internal_type_size env)
    |> List.fold ~init:0 ~f:( + )

  let type_constants_size env tconsts =
    S_map.map (fun (_id, ty) -> ty_size env ty) tconsts |> fun m ->
    S_map.fold (fun _ x y -> x + y) m 0

  let solving_info_size env solving_info =
    match solving_info with
    | TVIType ty -> ty_size env ty
    | TVIConstraints tvcstr ->
      let {
        appears_covariantly = _;
        appears_contravariantly = _;
        upper_bounds;
        lower_bounds;
        type_constants;
      } =
        tvcstr
      in
      let ubound_size = internal_type_set_size env upper_bounds in
      let lbound_size = internal_type_set_size env lower_bounds in
      let tconst_size = type_constants_size env type_constants in
      ubound_size + lbound_size + tconst_size

  let tyvar_info_size env tvinfo =
    let {
      tyvar_pos = _;
      solving_info;
      eager_solve_failed = _;
      is_error = _;
      is_shadow = _;
      shadow_local_name = _;
      rank = _;
    } =
      tvinfo
    in
    solving_info_size env solving_info

  let inference_env_size env =
    let {
      tvenv;
      subtype_prop = _;
      tyvars_stack = _;
      tyvar_occurrences = _;
      pos = _;
    } =
      env
    in
    Tvid.Map.map (tyvar_info_size env) tvenv |> fun m ->
    Tvid.Map.fold (fun _ x y -> x + y) m 0
end

(* The following merging logic is used to gather all the information we have
 * on a global type variable from multiple sub-graphs. It simply unions
 * all the constraints and does not try to do anything clever like
 * keeping the graph transitively closed. *)

let merge_constraints cstr1 cstr2 =
  let {
    lower_bounds = lb1;
    upper_bounds = ub1;
    type_constants = tc1;
    appears_covariantly = cov1;
    appears_contravariantly = contra1;
  } =
    cstr1
  in
  let {
    lower_bounds = lb2;
    upper_bounds = ub2;
    type_constants = tc2;
    appears_covariantly = cov2;
    appears_contravariantly = contra2;
  } =
    cstr2
  in
  {
    lower_bounds = ITySet.union lb1 lb2;
    upper_bounds = ITySet.union ub1 ub2;
    appears_covariantly = cov1 || cov2;
    appears_contravariantly = contra1 || contra2;
    type_constants =
      (* Type constants must already have been made equivalent, so picking any should be fine *)
      (* TODO: that might actually not be true during initial merging, but let's fix that later. *)
      S_map.union tc1 tc2;
  }

let solving_info_as_constraints sinfo =
  match sinfo with
  | TVIConstraints c -> c
  | TVIType ty ->
    {
      lower_bounds = ITySet.singleton (LoclType ty);
      upper_bounds = ITySet.singleton (LoclType ty);
      appears_covariantly = false;
      appears_contravariantly = false;
      type_constants = S_map.empty;
    }

let merge_solving_infos sinfo1 sinfo2 =
  let cstr1 = solving_info_as_constraints sinfo1 in
  let cstr2 = solving_info_as_constraints sinfo2 in
  let cstr = merge_constraints cstr1 cstr2 in
  TVIConstraints cstr

let merge_tyvar_infos tvinfo1 tvinfo2 =
  let {
    tyvar_pos = pos1;
    eager_solve_failed = esf1;
    solving_info = sinfo1;
    is_error = is_error1;
    is_shadow = is_shadow1;
    shadow_local_name = sln1;
    rank = rank1;
  } =
    tvinfo1
  in
  let {
    tyvar_pos = pos2;
    eager_solve_failed = esf2;
    solving_info = sinfo2;
    is_error = is_error2;
    is_shadow = is_shadow2;
    shadow_local_name = sln2;
    rank = rank2;
  } =
    tvinfo2
  in
  {
    tyvar_pos =
      (if Pos.equal pos1 Pos.none then
        pos2
      else
        pos1);
    eager_solve_failed = esf1 || esf2;
    solving_info = merge_solving_infos sinfo1 sinfo2;
    is_error = is_error1 || is_error2;
    is_shadow = is_shadow1 || is_shadow2;
    shadow_local_name = Option.first_some sln1 sln2;
    rank = min rank1 rank2;
  }

let merge_tyvars env v1 v2 =
  if Tvid.equal v1 v2 then
    env
  else
    let tvenv = env.tvenv in
    let tvinfo1 = Tvid.Map.find v1 env.tvenv in
    let tvinfo2 = Tvid.Map.find v2 env.tvenv in
    let tvinfo = merge_tyvar_infos tvinfo1 tvinfo2 in
    let tvenv = Tvid.Map.add v2 tvinfo tvenv in
    let env = { env with tvenv } in
    let env = add env v1 (mk (Reason.none, Tvar v2)) in
    let env = remove_tyvar_lower_bound env v2 v2 in
    let env = remove_tyvar_upper_bound env v2 v2 in
    env

let simple_merge env1 env2 =
  let {
    tvenv = tvenv1;
    subtype_prop = subtype_prop1;
    tyvars_stack = tyvars_stack1;
    tyvar_occurrences = tyvar_occurrences1;
    pos;
  } =
    env1
  in
  let {
    tvenv = tvenv2;
    subtype_prop = _;
    tyvars_stack = _;
    tyvar_occurrences = _;
    pos = _;
  } =
    env2
  in
  (* Hot path: this is called for every function and lambda visited by each
     TAST check. [union] keeps one-sided entries and their subtrees as-is,
     and the two envs usually share most entries physically, in which case
     merging them is the identity. *)
  let tvenv =
    Tvid.Map.union
      ~combine:(fun _v tvinfo1 tvinfo2 ->
        if phys_equal tvinfo1 tvinfo2 then
          Some tvinfo1
        else
          let {
            tyvar_pos = tyvar_pos1;
            eager_solve_failed = eager_solve_failed1;
            solving_info = sinfo1;
            is_error = is_error1;
            is_shadow = is_shadow1;
            shadow_local_name = sln1;
            rank = rank1;
          } =
            tvinfo1
          in
          let {
            tyvar_pos = tyvar_pos2;
            eager_solve_failed = eager_solve_failed2;
            solving_info = sinfo2;
            is_error = is_error2;
            is_shadow = is_shadow2;
            shadow_local_name = sln2;
            rank = rank2;
          } =
            tvinfo2
          in
          let tvinfo =
            {
              tyvar_pos =
                (if Pos.equal tyvar_pos1 Pos.none then
                  tyvar_pos2
                else
                  tyvar_pos1);
              eager_solve_failed = eager_solve_failed1 || eager_solve_failed2;
              solving_info =
                (match (sinfo1, sinfo2) with
                | (TVIConstraints _, TVIType ty)
                | (TVIType ty, TVIConstraints _) ->
                  TVIType ty
                | (TVIConstraints _, TVIConstraints _)
                | (TVIType _, TVIType _) ->
                  sinfo1);
              is_error = is_error1 || is_error2;
              is_shadow = is_shadow1 || is_shadow2;
              shadow_local_name = Option.first_some sln1 sln2;
              rank = min rank1 rank2;
            }
          in
          Some tvinfo)
      tvenv1
      tvenv2
  in
  {
    tvenv;
    subtype_prop = subtype_prop1;
    tyvars_stack = tyvars_stack1;
    tyvar_occurrences = tyvar_occurrences1;
    pos;
  }

let get_nongraph_subtype_prop env = env.subtype_prop

let is_alias_for_another_var env v =
  match get_solving_info_opt env v with
  | Some (TVIType ty) -> begin
    match get_node ty with
    | Tvar v' when not (Tvid.equal v v') -> true
    | _ -> false
  end
  | _ -> false

(** Some ty vars in the map will carry no additional information, e.g.
some ty vars belong to methods declared in parent classes, even
when these methods are not used in the subclass itself. In those cases,
the ty var will be registered, but the accompagnying ty var info
will contain nothing useful. It will in essence be an identity element
under the merge operation. *)
let solving_info_carries_information = function
  | TVIType _ -> true
  | TVIConstraints tvcstr ->
    let {
      appears_covariantly;
      appears_contravariantly;
      upper_bounds;
      lower_bounds;
      type_constants;
    } =
      tvcstr
    in
    appears_contravariantly
    || appears_covariantly
    || (not @@ ITySet.is_empty upper_bounds)
    || (not @@ ITySet.is_empty lower_bounds)
    || (not @@ S_map.is_empty type_constants)

let tyvar_info_carries_information tvinfo =
  let {
    tyvar_pos = _;
    solving_info;
    eager_solve_failed = _;
    is_error = _;
    is_shadow = _;
    shadow_local_name = _;
    rank;
  } =
    tvinfo
  in
  solving_info_carries_information solving_info || rank > 0

let compress env =
  let { tvenv; subtype_prop; tyvars_stack; tyvar_occurrences; pos } = env in
  let tvenv =
    Tvid.Map.filter (fun _k -> tyvar_info_carries_information) tvenv
  in
  { tvenv; subtype_prop; tyvars_stack; tyvar_occurrences; pos }

let remove_var_from_bounds
    env v ~search_in_upper_bounds_of ~search_in_lower_bounds_of =
  let env =
    Tvid.Set.fold
      (fun v' env ->
        Option.fold
          (get_tyvar_upper_bounds_opt env v')
          ~init:env
          ~f:(fun env bounds ->
            ITySet.filter (InternalType.is_not_var_v ~v) bounds
            |> set_tyvar_upper_bounds env v'))
      search_in_upper_bounds_of
      env
  in
  let env =
    Tvid.Set.fold
      (fun v' env ->
        Option.fold
          (get_tyvar_lower_bounds_opt env v')
          ~init:env
          ~f:(fun env bounds ->
            ITySet.filter (InternalType.is_not_var_v ~v) bounds
            |> set_tyvar_lower_bounds env v'))
      search_in_lower_bounds_of
      env
  in
  env

let remove_var_from_tyvars_stack tyvars_stack var =
  List.map tyvars_stack ~f:(fun (p, vars) ->
      (p, List.filter vars ~f:(fun v -> not (Tvid.equal var v))))

let replace_var_by_ty_in_prop prop v ty =
  let rec replace prop =
    match prop with
    | TL.IsSubtype (cd, ty1, ty2) ->
      let ty1 =
        if InternalType.is_var_v ty1 ~v then
          LoclType ty
        else
          ty1
      in
      let ty2 =
        if InternalType.is_var_v ty2 ~v then
          LoclType ty
        else
          ty2
      in
      TL.IsSubtype (cd, ty1, ty2)
    | TL.Disj (f, props) ->
      let props = List.map props ~f:replace in
      TL.Disj (f, props)
    | TL.Conj props ->
      let props = List.map props ~f:replace in
      TL.Conj props
    | TL.AmbiguousShapeSplat _ -> prop
  in
  replace prop

let remove_var env var ~search_in_upper_bounds_of ~search_in_lower_bounds_of =
  let env =
    remove_var_from_bounds
      env
      var
      ~search_in_upper_bounds_of
      ~search_in_lower_bounds_of
  in
  let (env, ty) = expand_var env Reason.none var in
  let { tvenv; tyvars_stack; tyvar_occurrences; subtype_prop; pos } = env in
  let tyvar_occurrences = Occ.remove_var tyvar_occurrences var in
  let tvenv = Tvid.Map.remove var tvenv in
  let tyvars_stack = remove_var_from_tyvars_stack tyvars_stack var in
  let subtype_prop = replace_var_by_ty_in_prop subtype_prop var ty in
  { tvenv; tyvars_stack; tyvar_occurrences; subtype_prop; pos }

let force_lazy_values_tyvar_constraints (cstrs : tyvar_constraints) =
  let {
    appears_covariantly;
    appears_contravariantly;
    lower_bounds;
    upper_bounds;
    type_constants;
  } =
    cstrs
  in
  {
    appears_covariantly;
    appears_contravariantly;
    lower_bounds = ITySet.force_lazy_values lower_bounds;
    upper_bounds = ITySet.force_lazy_values upper_bounds;
    type_constants =
      S_map.map
        (fun (p, ty) -> (p, Type_force_lazy_values.locl_ty ty))
        type_constants;
  }

let force_lazy_values_solving_info (solving_info : solving_info) =
  match solving_info with
  | TVIType ty -> TVIType (Type_force_lazy_values.locl_ty ty)
  | TVIConstraints cstrs ->
    TVIConstraints (force_lazy_values_tyvar_constraints cstrs)

let force_lazy_values_tyvar_info (tyvar_info : tyvar_info) =
  {
    tyvar_info with
    solving_info = force_lazy_values_solving_info tyvar_info.solving_info;
  }

let force_lazy_values_tvenv (tvenv : tvenv) =
  Tvid.Map.map force_lazy_values_tyvar_info tvenv

let force_lazy_values (env : t) =
  let { tvenv; tyvars_stack; subtype_prop; tyvar_occurrences; pos } = env in
  {
    tvenv = force_lazy_values_tvenv tvenv;
    tyvars_stack;
    subtype_prop = TL.force_lazy_values subtype_prop;
    tyvar_occurrences;
    pos;
  }

let is_error { tvenv; _ } tvid =
  Option.value_map ~default:false ~f:(fun { is_error; _ } -> is_error)
  @@ Tvid.Map.find_opt tvid tvenv

let get_rank { tvenv; _ } tvid =
  Option.value_map ~default:0 ~f:(fun { rank; _ } -> rank)
  @@ Tvid.Map.find_opt tvid tvenv

let get_pos env = env.pos

let set_pos env pos =
  let old_pos = env.pos in
  let restore env = { env with pos = old_pos } in
  ({ env with pos }, restore)
