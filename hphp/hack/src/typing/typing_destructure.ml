(** Core typing logic for shape and tuple destructuring.

    Shape destructuring extracts fields from shapes into local variables:
    {[
      shape('x' => $x, ?'y' => $y, 'z' => _) = $point;
      shape($x, $y) = $point;           // punning
      shape('x' => $x, ...) = $point;   // open pattern
    ]}

    Tuple destructuring does the same for tuples:
    {[
      tuple($a, $b) = $tup;
      tuple($a, optional $b) = $tup;
      tuple($a, ...) = $tup;            // open pattern
    ]}

    Both also work in [foreach]:
    {[
      foreach ($points as shape('x' => $x, 'y' => $y)) { ... }
    ]}

    Shapes in Hack can be open or closed. An open shape has unknown fields
    beyond those listed — written with [...] in type position:
    {[
      shape('x' => int, ...)   // open: may have other fields
      shape('x' => int)        // closed: exactly these fields
    ]}

    In this file, pattern types are always constructed as open. This avoids
    incorrectly rejecting valid cases like omitting optional fields from a
    closed pattern:
    {[
      $s : shape('a' => int, 'b' => int, ?'c' => int);
      shape('a' => $a, 'b' => $b) = $s;   // valid: ?'c' may be omitted
    ]}
    If we built a closed pattern type [shape('a' => α, 'b' => β)], shape
    subtyping would reject the optional ['c'] in the RHS. Using an open
    pattern type avoids this, and structural validation (required field
    coverage, open/closed enforcement) runs separately in {!Destructure_check}.

    Dynamic and like-type RHS values need special handling because
    [dynamic] is not a subtype of shape or tuple types in normal
    subtyping ([dynamic <: shape(...)] doesn't hold). For example:
    {[
      function f(dynamic $d): void {
        shape('x' => $x, ...) = $d;    // $x should be dynamic
      }
      function g(~shape('x' => int) $s): void {
        shape('x' => $x, ...) = $s;    // $x should be ~int
      }
    ]}
    For like types ([~T = dynamic | T]), union subtyping requires all
    members to satisfy the constraint. The [dynamic] member would cause
    the whole constraint to fail, so we strip it out, constrain against
    the inner type, and separately add [dynamic] as a lower bound on
    each type variable. The solver combines both lower bounds — e.g.,
    [int | dynamic] = [~int].

    Structural validation — required field coverage, open/closed shape
    enforcement, known-field checks, tuple arity — is handled by
    {!Destructure_check}, not this file.

    This file does not handle legacy list destructuring
    ([list($a, $b) = $vec]), which uses the [Tdestructure] constraint
    in {!Typing_subtype}. *)

open Hh_prelude

let set_valid_rvalue
    (assign_pos : Pos.t)
    (env : Typing_env_types.env)
    (lvar : Local_id.t)
    (ty : Typing_defs.locl_ty) : Typing_env_types.env =
  let env =
    if Typing_env.is_using_var env lvar then begin
      Typing_error_utils.add_typing_error
        ~env
        Typing_error.(
          primary
            (Primary.Illegal_disposable { pos = assign_pos; verb = `assigned }));
      env
    end else
      env
  in
  let env = Typing_env.set_local env lvar ty assign_pos in
  match
    Typing_env.set_local_expr_id env lvar (Typing_env.make_expression_id env)
  with
  | Ok env -> env
  | Error (env, err) ->
    Typing_error_utils.add_typing_error ~env err;
    env

let pos_of_target (target : Nast.destructure_target) : Pos.t =
  let (_, _, target_) = target in
  match target_ with
  | Aast.DtLvar (pos, _) -> pos
  | Aast.DtWildcard pos -> pos
  | Aast.DtShape ds -> ds.ds_pos
  | Aast.DtTuple dt -> dt.dt_pos

let field_pos_of_ast_sfn (sfn : Ast_defs.shape_field_name) : Pos.t =
  match sfn with
  | Ast_defs.SFlit_str (pos, _) -> pos
  | Ast_defs.SFclass_const ((pos, _), _) -> pos
  | Ast_defs.SFclassname (pos, _) -> pos

let rec collect_target_vars (target : Nast.destructure_target) :
    (Pos.t * Local_id.t) list =
  let (_, _, target_) = target in
  match target_ with
  | Aast.DtLvar lid -> [lid]
  | Aast.DtWildcard _ -> []
  | Aast.DtShape shape_pat ->
    List.concat_map shape_pat.ds_fields ~f:(fun shape_field ->
        collect_target_vars shape_field.dsf_target)
  | Aast.DtTuple tuple_pat ->
    List.concat_map tuple_pat.dt_entries ~f:(fun tuple_entry ->
        collect_target_vars tuple_entry.dte_target)

let check_duplicate_vars
    (env : Typing_env_types.env) (targets : Nast.destructure_target list) : unit
    =
  let all_vars = List.concat_map targets ~f:collect_target_vars in
  let seen = Hashtbl.create (module String) in
  List.iter all_vars ~f:(fun (pos, lid) ->
      let name = Local_id.get_name lid in
      match Hashtbl.find seen name with
      | Some first_pos ->
        Typing_error_utils.add_typing_error
          ~env
          (Typing_error.primary
             (Typing_error.Primary.Unify_error
                {
                  pos;
                  msg_opt =
                    Some
                      (Printf.sprintf
                         "Variable `%s` is bound multiple times in this destructuring pattern"
                         name);
                  reasons_opt =
                    Some
                      (lazy
                        [
                          ( Pos_or_decl.of_raw_pos first_pos,
                            "First binding was here" );
                        ]);
                }))
      | None -> Hashtbl.set seen ~key:name ~data:pos)

(** Accumulator for typing a shape destructure pattern. *)
module Shape_fields : sig
  type t = {
    typed_fields_rev: Tast.destructure_shape_field list;
        (** TAST nodes, in reverse source order *)
    shape_map:
      Typing_defs.locl_phase Typing_defs.shape_field_type
      Typing_defs.TShapeMap.t;
        (** fresh tyvars keyed by field name, assembled into an open
            shape type for the [rhs_ty <: pattern_ty] subtype constraint *)
    field_tys: Typing_defs.locl_ty list;
        (** all tyvars (including nested ones from sub-patterns) that
            need [dynamic] added as a lower bound when the RHS is dynamic/like *)
  }

  val empty : t

  val add :
    t ->
    typed_field:Tast.destructure_shape_field ->
    shape_field_name:Typing_defs.TShapeField.t ->
    shape_field_type:Typing_defs.locl_phase Typing_defs.shape_field_type ->
    field_ty:Typing_defs.locl_ty ->
    inner_field_tys:Typing_defs.locl_ty list ->
    t
end = struct
  type t = {
    typed_fields_rev: Tast.destructure_shape_field list;
    shape_map:
      Typing_defs.locl_phase Typing_defs.shape_field_type
      Typing_defs.TShapeMap.t;
    field_tys: Typing_defs.locl_ty list;
  }

  let empty =
    {
      typed_fields_rev = [];
      shape_map = Typing_defs.TShapeMap.empty;
      field_tys = [];
    }

  let add
      t
      ~typed_field
      ~shape_field_name
      ~shape_field_type
      ~field_ty
      ~inner_field_tys =
    {
      typed_fields_rev = typed_field :: t.typed_fields_rev;
      shape_map =
        Typing_defs.TShapeMap.add shape_field_name shape_field_type t.shape_map;
      field_tys = field_ty :: (inner_field_tys @ t.field_tys);
    }
end

(** Accumulator for typing a tuple destructure pattern. *)
module Tuple_entries : sig
  type t = {
    typed_entries_rev: Tast.destructure_tuple_entry list;
        (** TAST nodes, in reverse source order *)
    required_tys_rev: Typing_defs.locl_ty list;
        (** required prefix entry tyvars, in reverse source order *)
    optional_tys_rev: Typing_defs.locl_ty list;
        (** optional suffix entry tyvars, in reverse source order *)
    field_tys: Typing_defs.locl_ty list;
        (** all entry tyvars (including nested ones from sub-patterns) that
            need [dynamic] added as a lower bound when the RHS is dynamic/like *)
  }

  val empty : t

  val add :
    t ->
    typed_entry:Tast.destructure_tuple_entry ->
    optional:bool ->
    field_ty:Typing_defs.locl_ty ->
    inner_field_tys:Typing_defs.locl_ty list ->
    t
end = struct
  type t = {
    typed_entries_rev: Tast.destructure_tuple_entry list;
    required_tys_rev: Typing_defs.locl_ty list;
    optional_tys_rev: Typing_defs.locl_ty list;
    field_tys: Typing_defs.locl_ty list;
  }

  let empty =
    {
      typed_entries_rev = [];
      required_tys_rev = [];
      optional_tys_rev = [];
      field_tys = [];
    }

  let add t ~typed_entry ~optional ~field_ty ~inner_field_tys =
    {
      typed_entries_rev = typed_entry :: t.typed_entries_rev;
      required_tys_rev =
        (if optional then
          t.required_tys_rev
        else
          field_ty :: t.required_tys_rev);
      optional_tys_rev =
        (if optional then
          field_ty :: t.optional_tys_rev
        else
          t.optional_tys_rev);
      field_tys = field_ty :: (inner_field_tys @ t.field_tys);
    }
end

(** Add the constraint [rhs_ty <: pattern_ty], handling dynamic and like types.
    For like types ([~T]), strips dynamic and constrains against [T], then
    adds [dynamic] as a lower bound for each field tyvar so they resolve
    to like-wrapped types ([~field_type]). For [dynamic], adds [dynamic]
    as a lower bound for each field tyvar and skips the shape/tuple
    constraint. This follows the precedent of [destructure_dynamic] in
    the [Tdestructure] constraint handler. *)
let add_destructure_constraint
    (env : Typing_env_types.env)
    (pat_pos : Pos.t)
    ~(rhs_ty : Typing_defs.locl_ty)
    ~(pattern_ty : Typing_defs.locl_ty)
    (field_tys : Typing_defs.locl_ty list) : Typing_env_types.env =
  let (env, stripped_opt) =
    Typing_dynamic_utils.try_strip_dynamic ~do_not_solve_likes:true env rhs_ty
  in
  let is_dynamic = Typing_defs.is_dynamic rhs_ty in
  let rhs_has_dynamic = Option.is_some stripped_opt || is_dynamic in
  let env =
    if is_dynamic then
      env
    else
      let effective_rhs =
        match stripped_opt with
        | Some inner -> inner
        | None -> rhs_ty
      in
      let (env, ty_err_opt) =
        Typing_subtype.sub_type
          env
          effective_rhs
          pattern_ty
          (Some (Typing_error.Reasons_callback.unify_error_at pat_pos))
      in
      Option.iter ty_err_opt ~f:(Typing_error_utils.add_typing_error ~env);
      env
  in
  if rhs_has_dynamic then
    let dyn_ty =
      Typing_make_type.dynamic
        (Typing_reason.shape_or_tuple_destructure pat_pos)
    in
    List.fold field_tys ~init:env ~f:(fun env field_ty ->
        let (env, _ty_err_opt) =
          Typing_subtype.sub_type env dyn_ty field_ty None
        in
        env)
  else
    env

(** Type a destructure target.
    @param nullable when true, wraps leaf variable types with [?].
    Set when any ancestor field is optional.
    @param field_ty fresh tyvar representing this target's type —
    the solver resolves it via the subtype constraint emitted by the
    enclosing shape/tuple.
    @return [(env, tast_node, inner_field_tys)] where [inner_field_tys]
    is empty for leaf targets and contains all nested tyvars for
    shape/tuple targets (used to propagate [dynamic] lower bounds). *)
let rec type_destructure_target
    ~(nullable : bool)
    (assign_pos : Pos.t)
    (env : Typing_env_types.env)
    (target : Nast.destructure_target)
    (field_ty : Typing_defs.locl_ty) :
    Typing_env_types.env
    * (Tast.ty
      * Tast.saved_env
      * (Tast.ty, Tast.saved_env) Aast.destructure_target_)
    * Typing_defs.locl_ty list =
  let (_, _, target_) = target in
  match target_ with
  | Aast.DtLvar ((_, x) as lid)
    when String.equal
           (Local_id.get_name x)
           Naming_special_names.SpecialIdents.placeholder ->
    let void_ty = Typing_make_type.void (Typing_reason.placeholder (fst lid)) in
    (env, (void_ty, Tast.dummy_saved_env, Aast.DtLvar lid), [])
  | Aast.DtLvar ((_, x) as lid) ->
    let var_ty =
      if nullable then
        Typing_make_type.nullable
          (Typing_reason.shape_or_tuple_destructure (fst lid))
          field_ty
      else
        field_ty
    in
    let var_ty =
      Typing_env.update_reason env var_ty ~f:(fun rhs ->
          Typing_reason.flow_assign ~rhs ~lval:(Typing_reason.witness (fst lid)))
    in
    let env = set_valid_rvalue assign_pos env x var_ty in
    (env, (var_ty, Tast.dummy_saved_env, Aast.DtLvar lid), [])
  | Aast.DtWildcard wpos ->
    let void_ty = Typing_make_type.void (Typing_reason.witness wpos) in
    (env, (void_ty, Tast.dummy_saved_env, Aast.DtWildcard wpos), [])
  | Aast.DtShape shape_pat ->
    let (env, te, ty, inner_field_tys) =
      type_shape
        ~nullable
        assign_pos
        env
        ~pat_pos:shape_pat.ds_pos
        shape_pat
        field_ty
    in
    let typed_shape =
      match te with
      | (_, _, Aast.DestructureShape typed_shape) -> typed_shape
      | _ ->
        Hack_event_logger.invariant_violation_bug
          ~pos:(Pos.show_absolute (Pos.to_absolute shape_pat.ds_pos))
          "type_shape returned non-DestructureShape expr in DtShape target";
        {
          Aast.ds_pos = shape_pat.ds_pos;
          ds_fields = [];
          ds_ellipsis = shape_pat.ds_ellipsis;
        }
    in
    (env, (ty, Tast.dummy_saved_env, Aast.DtShape typed_shape), inner_field_tys)
  | Aast.DtTuple tuple_pat ->
    let (env, te, ty, inner_field_tys) =
      type_tuple
        ~nullable
        assign_pos
        env
        ~pat_pos:tuple_pat.dt_pos
        tuple_pat
        field_ty
    in
    let typed_tuple =
      match te with
      | (_, _, Aast.DestructureTuple typed_tuple) -> typed_tuple
      | _ ->
        Hack_event_logger.invariant_violation_bug
          ~pos:(Pos.show_absolute (Pos.to_absolute tuple_pat.dt_pos))
          "type_tuple returned non-DestructureTuple expr in DtTuple target";
        {
          Aast.dt_pos = tuple_pat.dt_pos;
          dt_entries = [];
          dt_ellipsis = tuple_pat.dt_ellipsis;
        }
    in
    (env, (ty, Tast.dummy_saved_env, Aast.DtTuple typed_tuple), inner_field_tys)

(** Type a shape destructuring pattern against [rhs_ty].
    Builds an open shape pattern type with fresh tyvars at the leaves and
    emits a single [rhs_ty <: pattern_ty] subtype constraint. Structural
    validation (open/closed, required field coverage, field existence) is
    deferred to {!Destructure_check} where types are fully resolved.

    Returns [(env, tast_expr, result_ty, field_tys)] where [field_tys]
    is the list of all tyvars (including those from nested patterns) that
    need [dynamic] added as a lower bound when the RHS is dynamic or like-typed. *)
and type_shape
    ~(nullable : bool)
    (assign_pos : Pos.t)
    (env : Typing_env_types.env)
    ~(pat_pos : Pos.t)
    (shape_pat : Nast.destructure_shape)
    (rhs_ty : Typing_defs.locl_ty) :
    Typing_env_types.env
    * Tast.expr
    * Typing_defs.locl_ty
    * Typing_defs.locl_ty list =
  let (env, key_errors) =
    Typing_shapes.check_shape_keys_validity
      env
      (List.map shape_pat.ds_fields ~f:(fun field -> field.Aast.dsf_name))
  in
  List.iter key_errors ~f:(Typing_error_utils.add_typing_error ~env);
  let r = Typing_reason.shape_or_tuple_destructure pat_pos in
  let (env, { Shape_fields.typed_fields_rev; shape_map; field_tys }) =
    List.fold
      shape_pat.ds_fields
      ~init:(env, Shape_fields.empty)
      ~f:(fun (env, acc) shape_field ->
        let (env, field_ty) =
          Typing_env.fresh_type env (field_pos_of_ast_sfn shape_field.dsf_name)
        in
        let (env, typed_target, inner_field_tys) =
          type_destructure_target
            ~nullable:(shape_field.dsf_optional || nullable)
            assign_pos
            env
            shape_field.dsf_target
            field_ty
        in
        let acc =
          Shape_fields.add
            acc
            ~typed_field:
              {
                Aast.dsf_optional = shape_field.dsf_optional;
                dsf_name = shape_field.dsf_name;
                dsf_target = typed_target;
              }
            ~shape_field_name:
              (Typing_defs.TShapeField.of_ast
                 Pos_or_decl.of_raw_pos
                 shape_field.dsf_name)
            ~shape_field_type:
              {
                Typing_defs.sft_optional = shape_field.dsf_optional;
                sft_ty = field_ty;
              }
            ~field_ty
            ~inner_field_tys
        in
        (env, acc))
  in
  (* Open with mixed: see comment at the top of this file for why *)
  let pattern_ty =
    Typing_make_type.open_shape r ~kind:(Typing_make_type.mixed r) shape_map
  in
  (* rhs_ty <: pattern_ty *)
  let env =
    add_destructure_constraint env pat_pos ~rhs_ty ~pattern_ty field_tys
  in
  let te =
    Tast.make_typed_expr
      pat_pos
      rhs_ty
      (Aast.DestructureShape
         {
           Aast.ds_pos = shape_pat.ds_pos;
           ds_fields = List.rev typed_fields_rev;
           ds_ellipsis = shape_pat.ds_ellipsis;
         })
  in
  (env, te, rhs_ty, field_tys)

(** Type a tuple destructuring pattern against [rhs_ty].
    Builds a tuple pattern type with fresh tyvars and a variadic [mixed]
    tail, then emits a single [rhs_ty <: pattern_ty] subtype constraint.
    Arity validation is deferred to {!Destructure_check}.

    Returns [(env, tast_expr, result_ty, field_tys)]; see {!type_shape}
    for the meaning of [field_tys]. *)
and type_tuple
    ~(nullable : bool)
    (assign_pos : Pos.t)
    (env : Typing_env_types.env)
    ~(pat_pos : Pos.t)
    (tuple_pat : Nast.destructure_tuple)
    (rhs_ty : Typing_defs.locl_ty) :
    Typing_env_types.env
    * Tast.expr
    * Typing_defs.locl_ty
    * Typing_defs.locl_ty list =
  let r = Typing_reason.shape_or_tuple_destructure pat_pos in
  let ( env,
        _seen_optional,
        {
          Tuple_entries.typed_entries_rev;
          required_tys_rev;
          optional_tys_rev;
          field_tys;
        } ) =
    List.fold
      tuple_pat.dt_entries
      ~init:(env, false, Tuple_entries.empty)
      ~f:(fun (env, seen_optional, acc) tuple_entry ->
        let (env, entry_ty) =
          Typing_env.fresh_type env (pos_of_target tuple_entry.dte_target)
        in
        let optional = seen_optional || tuple_entry.dte_optional in
        let (env, typed_target, inner_field_tys) =
          type_destructure_target
            ~nullable:(optional || nullable)
            assign_pos
            env
            tuple_entry.dte_target
            entry_ty
        in
        let acc =
          Tuple_entries.add
            acc
            ~typed_entry:
              { Aast.dte_optional = optional; dte_target = typed_target }
            ~optional
            ~field_ty:entry_ty
            ~inner_field_tys
        in
        (env, optional, acc))
  in
  (* Variadic mixed tail: permissive for subtyping, arity checks are in {!Destructure_check} *)
  let pattern_ty =
    Typing_defs_core.mk
      ( r,
        Typing_defs.Ttuple
          {
            Typing_defs.t_required = List.rev required_tys_rev;
            t_optional = List.rev optional_tys_rev;
            t_extra = Typing_defs.Tvariadic (Typing_make_type.mixed r);
          } )
  in
  (* rhs_ty <: pattern_ty *)
  let env =
    add_destructure_constraint env pat_pos ~rhs_ty ~pattern_ty field_tys
  in
  let te =
    Tast.make_typed_expr
      pat_pos
      rhs_ty
      (Aast.DestructureTuple
         {
           Aast.dt_pos = tuple_pat.dt_pos;
           dt_entries = List.rev typed_entries_rev;
           dt_ellipsis = tuple_pat.dt_ellipsis;
         })
  in
  (env, te, rhs_ty, field_tys)

let shape
    ~assign_pos
    ~pat_pos
    (env : Typing_env_types.env)
    (shape_pat : Nast.destructure_shape)
    (rhs_ty : Typing_defs.locl_ty) =
  let targets =
    List.map shape_pat.ds_fields ~f:(fun shape_field -> shape_field.dsf_target)
  in
  check_duplicate_vars env targets;
  let (env, te, ty, _field_tys) =
    type_shape ~nullable:false assign_pos env ~pat_pos shape_pat rhs_ty
  in
  (env, te, ty)

let tuple
    ~assign_pos
    ~pat_pos
    (env : Typing_env_types.env)
    (tuple_pat : Nast.destructure_tuple)
    (rhs_ty : Typing_defs.locl_ty) =
  let targets =
    List.map tuple_pat.dt_entries ~f:(fun tuple_entry -> tuple_entry.dte_target)
  in
  check_duplicate_vars env targets;
  let (env, te, ty, _field_tys) =
    type_tuple ~nullable:false assign_pos env ~pat_pos tuple_pat rhs_ty
  in
  (env, te, ty)
