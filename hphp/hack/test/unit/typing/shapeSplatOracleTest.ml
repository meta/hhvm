(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

(* Differential DENOTATIONAL oracle for shape-splat subtyping.
 *
 * The subtyping decision under test is [Typing_subtype.is_sub_type], which routes
 * shape-splat queries through the corner procedure (Shape_splat.Corner). We compare
 * it against a ground-truth oracle that is INDEPENDENT of the subtyping code:
 *
 *   - A finite, RECURSIVE (depth-bounded) value model: a "record" maps each label
 *     of a fixed universe to absent / a scalar (int or bool) / a nested shape value
 *     (one level deep). The value space is finite.
 *   - [denot row] = the set of records a simple row admits.
 *   - Ground subrow truth = [denot sub] is a subset of [denot super].
 *   - The rightmost-wins splat merge is REIMPLEMENTED here (not via
 *     Typing_shape_normalize), so the whole pipeline (merge + subtype) is tested.
 *   - For a rigid generic T with bound [lower, upper], the semantic truth is the
 *     forall-instantiation check: for every concrete row R with lower <: R <: upper
 *     (denotationally), [denot (merge sub[T:=R])] subset [denot (merge super[T:=R])].
 *
 * A mismatch (decision <> oracle) is either an oracle bug or a REAL subtyping bug. *)

open Hh_prelude
open OUnit2
open Typing_defs
module Env = Typing_env
module MakeType = Typing_make_type
module Reason = Typing_reason

let dummy_env () =
  let () = Typing_subtype.set_fun_refs () in
  let ctx =
    Provider_context.empty_for_test
      ~popt:Parser_options.default
      ~tcopt:Typechecker_options.default
      ~deps_mode:(Typing_deps_mode.InMemoryMode None)
  in
  let env = Typing_env_types.empty ctx Relative_path.default ~droot:None in
  let dummy_file = Relative_path.from_root ~suffix:"test.php" in
  let dummy_pos = Pos.make dummy_file (Lexing.from_string "") in
  let (env, _restore) = Env.set_inference_env_pos env (Some dummy_pos) in
  env

let r = Reason.none

(* == Denotational model ==================================================== *)

(* A RECURSIVE, depth-bounded type. A field's type may itself be a shape (one level
   deep). [TUnion] represents the union produced by rightmost-wins merge of optional
   fields; [TUnion []] is never constructed (use [TNothing]). *)
type ty =
  | TNothing
  | TInt
  | TBool
  | TUnion of ty list
  | TShape of orow

(* A field of a simple row: whether it is optional, and its type. A label NOT in
   [fields] is governed by [unknown] as an optional field. *)
and orow = {
  fields: (string * (bool * ty)) list;
  unknown: ty;
}

let rec equal_ty a b =
  match (a, b) with
  | (TNothing, TNothing)
  | (TInt, TInt)
  | (TBool, TBool) ->
    true
  | (TUnion xs, TUnion ys) ->
    Int.equal (List.length xs) (List.length ys)
    && List.for_all2_exn xs ys ~f:equal_ty
  | (TShape r1, TShape r2) -> equal_orow r1 r2
  | _ -> false

and equal_orow r1 r2 =
  equal_ty r1.unknown r2.unknown
  && Int.equal (List.length r1.fields) (List.length r2.fields)
  && List.for_all r1.fields ~f:(fun (l, (o1, t1)) ->
         match List.Assoc.find r2.fields l ~equal:String.equal with
         | Some (o2, t2) -> Bool.equal o1 o2 && equal_ty t1 t2
         | None -> false)

(* Top-level labels. [a] and [b] can be declared as known fields; [z] is a SPARE
   witness label that no shape ever declares, so that the open-vs-closed distinction
   is observable in the denotation (an open shape admits records with [z] present; a
   closed shape does not). Without such a spare label a finite universe whose labels
   are all known cannot distinguish open from closed. *)
let labels = ["a"; "b"; "z"]

(* Nested-shape labels: a single declarable label [p]. Kept to one label to bound
   the nested value space (and hence per-[subrow_denot] cost); all nested shapes are
   closed, so no spare label is needed for an open/closed distinction. *)
let nested_labels = ["p"]

let eff_field row l =
  match List.Assoc.find row.fields l ~equal:String.equal with
  | Some f -> f
  | None -> (true, row.unknown)

(* A RECURSIVE value: a scalar, or a shape value mapping labels to absent / a value. *)
type value =
  | VInt
  | VBool
  | VShape of (string * value option) list

let rec equal_value v1 v2 =
  match (v1, v2) with
  | (VInt, VInt)
  | (VBool, VBool) ->
    true
  | (VShape rc1, VShape rc2) ->
    Int.equal (List.length rc1) (List.length rc2)
    && List.for_all rc1 ~f:(fun (l, o1) ->
           match List.Assoc.find rc2 l ~equal:String.equal with
           | Some o2 -> Option.equal equal_value o1 o2
           | None -> false)
  | _ -> false

(* Nested value space: records over [nested_labels], each field absent / VInt / VBool. *)
let nested_value_space : value list =
  let per_label = [None; Some VInt; Some VBool] in
  let recs =
    List.fold_right nested_labels ~init:[[]] ~f:(fun l acc ->
        List.concat_map per_label ~f:(fun v ->
            List.map acc ~f:(fun rc -> (l, v) :: rc)))
  in
  List.map recs ~f:(fun rc -> VShape rc)

(* Values a top-level field may take: absent-handling is separate; the field's
   present value is a scalar (VInt/VBool) or a nested shape value. *)
let top_field_values : value list = [VInt; VBool] @ nested_value_space

(* Whether a value inhabits a type (recursive). *)
let rec value_has_type v t =
  match (v, t) with
  | (_, TNothing) -> false
  | (VInt, TInt) -> true
  | (VBool, TBool) -> true
  | (_, TUnion tys) -> List.exists tys ~f:(fun t -> value_has_type v t)
  | (VShape rc, TShape row) -> record_sat_over nested_labels row rc
  | _ -> false

(* Does record [rc] (an assignment over the given labels) satisfy row? *)
and record_sat_over lbls row rc =
  List.for_all lbls ~f:(fun l ->
      let (optional, fty) = eff_field row l in
      match List.Assoc.find_exn rc l ~equal:String.equal with
      | None -> optional
      | Some v -> value_has_type v fty)

(* Does a top-level record (assignment over [labels]) satisfy row? *)
let record_sat row rc = record_sat_over labels row rc

(* Top value space: records over [labels], each field absent / VInt / VBool / nested
   shape value. *)
let record_space : (string * value option) list list =
  let per_label = None :: List.map top_field_values ~f:Option.some in
  List.fold_right labels ~init:[[]] ~f:(fun l acc ->
      List.concat_map per_label ~f:(fun v ->
          List.map acc ~f:(fun rc -> (l, v) :: rc)))

let denot row = List.filter record_space ~f:(record_sat row)

let record_equal r1 r2 =
  List.for_all labels ~f:(fun l ->
      Option.equal
        equal_value
        (List.Assoc.find_exn r1 l ~equal:String.equal)
        (List.Assoc.find_exn r2 l ~equal:String.equal))

(* Denotational subrow: every record admitted by [sub] is admitted by [super]. *)
let subrow_denot sub super =
  let d_super = denot super in
  List.for_all (denot sub) ~f:(fun rc ->
      List.exists d_super ~f:(record_equal rc))

(* == Independent rightmost-wins merge (reimplemented, NOT the SUT) ========== *)

(* Value-set union of two types: flatten nested [TUnion], dedup by [equal_ty], drop
   [TNothing]; 0 members -> [TNothing], 1 -> that ty, else [TUnion]. The invariant
   (checked in a test) is [value_has_type v (union_ty a b)
    = value_has_type v a || value_has_type v b]. *)
let union_ty a b =
  let rec flatten t =
    match t with
    | TNothing -> []
    | TUnion tys -> List.concat_map tys ~f:flatten
    | _ -> [t]
  in
  let members =
    List.fold_left
      (flatten a @ flatten b)
      ~init:[]
      ~f:(fun acc t ->
        if List.exists acc ~f:(equal_ty t) then
          acc
        else
          acc @ [t])
  in
  match members with
  | [] -> TNothing
  | [t] -> t
  | _ -> TUnion members

let merge_field (l_opt, l_ty) (r_opt, r_ty) =
  if not r_opt then
    (* right required: overrides *)
    (r_opt, r_ty)
  else
    (* right optional: keep left's requiredness, union the types *)
    (l_opt, union_ty l_ty r_ty)

let merge_orow left right =
  let unknown = union_ty left.unknown right.unknown in
  let fields =
    List.map labels ~f:(fun l ->
        (l, merge_field (eff_field left l) (eff_field right l)))
  in
  { fields; unknown }

let empty_orow = { fields = []; unknown = TNothing }

let merge_orows orows = List.fold_left orows ~init:empty_orow ~f:merge_orow

(* == Hack type builders ==================================================== *)

let tint = MakeType.int r

let tbool = MakeType.bool r

let key name = TShapeField.make (TSFlit_str (Pos_or_decl.none, name))

let rec hack_of_ty : ty -> locl_ty = function
  | TNothing -> MakeType.nothing r
  | TInt -> tint
  | TBool -> tbool
  | TUnion tys -> MakeType.union r (List.map tys ~f:hack_of_ty)
  | TShape row -> hack_of_orow row

and hack_of_orow row : locl_ty =
  let s_fields =
    List.fold_left
      row.fields
      ~init:TShapeMap.empty
      ~f:(fun acc (l, (opt, fty)) ->
        TShapeMap.add
          (key l)
          { sft_optional = opt; sft_ty = hack_of_ty fty }
          acc)
  in
  mk
    ( r,
      Tshape
        (Shape_simple
           {
             s_origin = Missing_origin;
             s_unknown_value = hack_of_ty row.unknown;
             s_fields;
           }) )

(* == Enumeration ========================================================== *)

(* SCALAR field types: nothing / int / bool / int|bool. Nested shapes (below) are
   built ONLY over these, enforcing the depth bound (2) by construction. *)
let scalar_field_types = [TNothing; TInt; TBool; TUnion [TInt; TBool]]

(* A handful of NESTED shape types over the nested label [p] (depth 1; their fields
   are only scalar). These make nested shapes appear in field types, bounds and
   instantiations without a further level of nesting. *)
let nested_shape_types =
  [
    (* shape('p' => int) closed *)
    TShape { fields = [("p", (false, TInt))]; unknown = TNothing };
    (* shape(?'p' => int) closed *)
    TShape { fields = [("p", (true, TInt))]; unknown = TNothing };
    (* shape('p' => bool) closed *)
    TShape { fields = [("p", (false, TBool))]; unknown = TNothing };
    (* shape() closed *)
    TShape { fields = []; unknown = TNothing };
  ]

(* Field states for enumeration. [all_rows] must stay INHABITED (uninhabited
   required-nothing rows live only in [witness_rows], the ∃-range), so we do NOT
   include Req nothing here — the engine's incompleteness on uninhabited shapes is
   already known and is not useful signal. We DO cover:
   - Opt nothing (= ABSENT): an inhabited absent field, the useful case.
   - a union field, and one Req + one Opt nested shape, so nested shapes appear as
     top-level field types. Kept MODEST to bound [all_rows]. *)
let field_states : (bool * ty) option list =
  [
    None;
    Some (false, TInt);
    Some (true, TInt);
    (* Opt int — needed so the box covers the [Req lower, Opt upper] corner
       (e.g. ?'a'=>int within [Req int, Opt int|bool]); without it the forall
       oracle misses that instantiation and spuriously reports valid. *)
    Some (true, TBool);
    Some (true, TNothing);
    (* Opt nothing = absent (inhabited) *)
    Some (false, TUnion [TInt; TBool]);
    (* Req int|bool *)
    (* one Req and one Opt nested shape *)
    Some (false, List.nth_exn nested_shape_types 0);
    (* Req shape('p' => int) *)
    Some (true, List.nth_exn nested_shape_types 0);
    (* Opt shape('p' => int) *)
  ]

let unknowns = [TNothing; TUnion [TInt; TBool]]

(* All simple rows over the two labels: field-states ^ 2 * unknowns. *)
let all_rows : orow list =
  List.concat_map field_states ~f:(fun fa ->
      List.concat_map field_states ~f:(fun fb ->
          List.map unknowns ~f:(fun unknown ->
              let fields =
                List.filter_map
                  [("a", fa); ("b", fb)]
                  ~f:(fun (l, o) -> Option.map o ~f:(fun f -> (l, f)))
              in
              { fields; unknown })))

(* The bottom row: every label required at [nothing], so it admits no records.
   A rigid parameter CAN be instantiated with it -- [nothing] is below every
   bound -- and the corner procedure models exactly that as its lower corner.
   Leaving it out of the instantiation space makes the forall skip the one
   instantiation that can fail, so a correct rejection reads as a false alarm. *)
let bottom_row : orow =
  {
    fields = List.map labels ~f:(fun l -> (l, (false, TNothing)));
    unknown = TNothing;
  }

(* == Pretty-printers (for counterexample reporting) ======================= *)

let rec show_ty = function
  | TNothing -> "nothing"
  | TInt -> "int"
  | TBool -> "bool"
  | TUnion tys -> String.concat ~sep:"|" (List.map tys ~f:show_ty)
  | TShape row -> show_orow row

and show_field (opt, fty) =
  (if opt then
    "?"
  else
    "")
  ^ show_ty fty

and show_orow row =
  let fs =
    List.map row.fields ~f:(fun (l, f) ->
        Printf.sprintf "%s:%s" l (show_field f))
  in
  Printf.sprintf
    "{%s | %s...}"
    (String.concat ~sep:", " fs)
    (show_ty row.unknown)

(* == Ground oracle: simple-shape subtyping vs denotation =================== *)

let prop_ground_simple _ =
  let env = dummy_env () in
  List.iter all_rows ~f:(fun sub ->
      List.iter all_rows ~f:(fun super ->
          let decision =
            Typing_subtype.is_sub_type
              env
              (hack_of_orow sub)
              (hack_of_orow super)
          in
          let oracle = subrow_denot sub super in
          assert_bool
            (Printf.sprintf
               "ground simple subtyping disagrees with denotation:\n  sub=%s\n  super=%s\n  decision=%b oracle=%b"
               (show_orow sub)
               (show_orow super)
               decision
               oracle)
            (Bool.equal decision oracle)))

(* == Ground splat oracle: rightmost-wins merge vs denotation =============== *)

let splat elems = mk (r, Tshape (Shape_splat { ss_elems = elems }))

(* A small set of two-element ground splats. *)
let two_elem_splats =
  List.concat_map all_rows ~f:(fun o1 ->
      List.map all_rows ~f:(fun o2 -> (o1, o2)))

(* Cost control: [prop_ground_splat] is O(all_rows^2 * supers) is_sub_type calls.
   With ~128 rows the full cross-product against all supers is prohibitive, so we
   pair every two-element splat against only a SMALL fixed subset of supers chosen
   to cover the interesting closed/open, scalar/union/nested/nothing shapes. *)
let ground_splat_supers : orow list =
  [
    { fields = []; unknown = TNothing };
    (* shape() closed *)
    { fields = []; unknown = TUnion [TInt; TBool] };
    (* shape(...) open *)
    { fields = [("a", (false, TInt))]; unknown = TNothing };
    (* shape('a' => int) *)
    { fields = [("a", (true, TInt))]; unknown = TNothing };
    (* shape(?'a' => int) *)
    { fields = [("a", (false, TNothing))]; unknown = TNothing };
    (* shape('a' => nothing) uninhabited *)
    {
      fields = [("a", (false, List.nth_exn nested_shape_types 0))];
      unknown = TNothing;
    };
    (* shape('a' => shape('p' => int)) nested *)
    {
      fields = [("a", (false, TInt)); ("b", (false, TBool))];
      unknown = TNothing;
    };
    (* shape('a' => int, 'b' => bool) *)
  ]

let prop_ground_splat _ =
  let env = dummy_env () in
  List.iter two_elem_splats ~f:(fun (o1, o2) ->
      List.iter ground_splat_supers ~f:(fun super ->
          let sub_hack = splat [hack_of_orow o1; hack_of_orow o2] in
          let decision =
            Typing_subtype.is_sub_type env sub_hack (hack_of_orow super)
          in
          let oracle = subrow_denot (merge_orows [o1; o2]) super in
          assert_bool
            (Printf.sprintf
               "ground splat subtyping disagrees with denotation:\n  sub=splat[%s; %s] (merged=%s)\n  super=%s\n  decision=%b oracle=%b"
               (show_orow o1)
               (show_orow o2)
               (show_orow (merge_orows [o1; o2]))
               (show_orow super)
               decision
               oracle)
            (Bool.equal decision oracle)))

(* == Rigid-param oracle: forall-instantiation ============================== *)

(* Candidate rows for T's bound (kept small to bound the box + query space). *)
let bound_rows =
  [
    { fields = []; unknown = TNothing };
    (* shape() *)
    { fields = [("a", (false, TInt))]; unknown = TNothing };
    (* shape('a' => int) *)
    { fields = [("a", (true, TUnion [TInt; TBool]))]; unknown = TNothing };
    (* shape(?'a' => int|bool) *)
    {
      fields = [("a", (false, List.nth_exn nested_shape_types 0))];
      unknown = TNothing;
    };
    (* shape('a' => shape('p' => int)) — a nested-shape-typed bound *)
    { fields = [("a", (true, TNothing))]; unknown = TUnion [TInt; TBool] };
    (* shape(?'a' => nothing, ...) — an ABSENT-field bound (the poly_absent_bound
       pattern: 'a' must be absent). Inhabited (records with 'a' absent). *)
    { fields = []; unknown = TUnion [TInt; TBool] } (* shape(...) open *);
  ]

(* Concrete rows T can be instantiated with, from [all_rows]. *)
let instantiations = bottom_row :: all_rows

(* T ranges over rows R with lower <: R <: upper (denotationally). *)
(* [lower = None] means the parameter is declared with NO [super] constraint,
   which is the common case in real code and is NOT the same as a lower bound of
   the empty row: with no bound the default is the bottom row, which projects
   [Req nothing] at every label. That corner is unreachable while every
   parameter is given a concrete lower bound. *)
let box lower upper =
  List.filter instantiations ~f:(fun rr ->
      (match lower with
      | None -> true
      | Some lower -> subrow_denot lower rr)
      && subrow_denot rr upper)

let add_generic env ~name ~lower ~upper =
  let env =
    match lower with
    | None -> env
    | Some lower -> Env.add_lower_bound env name (hack_of_orow lower)
  in
  let env = Env.add_upper_bound env name (hack_of_orow upper) in
  env

let tgeneric name = mk (r, Tgeneric name)

(* Query shapes as a function of the element occupying T's position: this lets us
   build the DECISION shape (T = the generic) and each ORACLE shape (T = a concrete
   row) from the same template. *)
type tmpl = locl_ty -> locl_ty

(* Concrete rows used by the templates. *)
let row_a_bool = { fields = [("a", (false, TBool))]; unknown = TNothing }

let row_b_int = { fields = [("b", (false, TInt))]; unknown = TNothing }

(* A concrete nested-shape-typed row used both as a decision element and as its own
   oracle instantiation, so nested shapes appear in the instantiation range. *)
let row_a_nested =
  {
    fields = [("a", (false, List.nth_exn nested_shape_types 0))];
    unknown = TNothing;
  }

(* A handful of templates covering T leftmost / rightmost / with a concrete field. *)
let sub_templates : (string * tmpl) list =
  [
    (* No [T] at all: pairs with a super that HAS [T], so the parameter is live
       on one side only. Every other template puts [T] on both sides, which
       leaves the one-sided configurations outside the generated space. *)
    ("shape('a'=>bool)", (fun _t -> hack_of_orow row_a_bool));
    ("...T", (fun t -> splat [t]));
    ("shape('a'=>bool), ...T", (fun t -> splat [hack_of_orow row_a_bool; t]));
    ("...T, shape('a'=>bool)", (fun t -> splat [t; hack_of_orow row_a_bool]));
    (* T merged AFTER a nested-shape field, exercising nested shapes in a splat *)
    ( "shape('a'=>shape('p'=>int)), ...T",
      (fun t -> splat [hack_of_orow row_a_nested; t]) );
  ]

let super_templates : (string * tmpl) list =
  [
    ("shape('b'=>int)", (fun _t -> hack_of_orow row_b_int));
    ("...T", (fun t -> splat [t]));
    ("...T, shape('b'=>int)", (fun t -> splat [t; hack_of_orow row_b_int]));
    (* Concrete part matches a SUB template's, so no inline label can be the
       thing that fails. Without such a pair the unknown-tail obligation is
       always masked by a label obligation failing first, and a tail that fails
       silently is indistinguishable from one that passes. *)
    ("...T, shape('a'=>bool)", (fun t -> splat [t; hack_of_orow row_a_bool]));
  ]

(* The oracle template operates on concrete rows and merges independently. *)
let sub_row_templates : (string * (orow -> orow list)) list =
  [
    ("shape('a'=>bool)", (fun _rr -> [row_a_bool]));
    ("...T", (fun rr -> [rr]));
    ("shape('a'=>bool), ...T", (fun rr -> [row_a_bool; rr]));
    ("...T, shape('a'=>bool)", (fun rr -> [rr; row_a_bool]));
    ("shape('a'=>shape('p'=>int)), ...T", (fun rr -> [row_a_nested; rr]));
  ]

let super_row_templates : (string * (orow -> orow list)) list =
  [
    ("shape('b'=>int)", (fun _rr -> [row_b_int]));
    ("...T", (fun rr -> [rr]));
    ("...T, shape('b'=>int)", (fun rr -> [rr; row_b_int]));
    ("...T, shape('a'=>bool)", (fun rr -> [rr; row_a_bool]));
  ]

let prop_rigid_param _ =
  let name = "T" in
  List.iteri sub_templates ~f:(fun si (sub_lbl, sub_t) ->
      let (_, sub_rows_t) = List.nth_exn sub_row_templates si in
      List.iteri super_templates ~f:(fun pi (super_lbl, super_t) ->
          let (_, super_rows_t) = List.nth_exn super_row_templates pi in
          List.iter
            (None :: List.map bound_rows ~f:Option.some)
            ~f:(fun lower ->
              List.iter bound_rows ~f:(fun upper ->
                  (* Only well-formed intervals: lower <: upper. *)
                  if
                    match lower with
                    | None -> true
                    | Some lower -> subrow_denot lower upper
                  then begin
                    let b = box lower upper in
                    (* Skip a vacuously-empty box (an unsatisfiable bound): the
                       forall oracle is trivially true there while [is_sub_type]
                       reasons rigidly, which is a degenerate, uninteresting case. *)
                    if not (List.is_empty b) then begin
                      let env =
                        add_generic (dummy_env ()) ~name ~lower ~upper
                      in
                      let decision =
                        Typing_subtype.is_sub_type
                          env
                          (sub_t (tgeneric name))
                          (super_t (tgeneric name))
                      in
                      (* [is_sub_type] evaluates the constraint and never builds
                         an error, so it cannot see a check that fails without
                         reporting. That gap is not hypothetical: an obligation
                         returning [invalid ~fail:None] makes the decision false
                         while the typechecker stays silent and ACCEPTS the
                         program. Measure what the user would be told. *)
                      let reported =
                        let (_env, err) =
                          Typing_subtype.sub_type
                            env
                            (sub_t (tgeneric name))
                            (super_t (tgeneric name))
                            (Some
                               (Typing_error.Reasons_callback.unify_error_at
                                  Pos.none))
                        in
                        Option.is_none err
                      in
                      (* Oracle: forall R in the box, ground denotational subrow. *)
                      let oracle =
                        List.for_all b ~f:(fun rr ->
                            subrow_denot
                              (merge_orows (sub_rows_t rr))
                              (merge_orows (super_rows_t rr)))
                      in
                      assert_bool
                        (Printf.sprintf
                           "rigid-param disagrees with forall-instantiation oracle:\n  sub=%s  super=%s\n  T lower=%s  upper=%s\n  box size=%d  decision=%b  reported-ok=%b  oracle=%b"
                           sub_lbl
                           super_lbl
                           (match lower with
                           | None -> "<none>"
                           | Some lower -> show_orow lower)
                           (show_orow upper)
                           (List.length b)
                           decision
                           reported
                           oracle)
                        (Bool.equal decision oracle
                        && Bool.equal reported oracle)
                    end
                  end))))

(* == Two parameters, one bounded by the other ============================= *)

(* [T2 as T1] registers [T1] as an upper bound of [T2] AND [T2] as a LOWER bound
   of [T1]. A bound that is itself a parameter is a configuration [add_generic]
   cannot express, so nothing above reaches the code that reads a bound through
   another parameter -- which is where reading a lower bound via SUPERtypes hid.

   Only the SOUNDNESS direction is asserted here (never accept what the oracle
   rejects), not equality. The corner procedure assigns [T1] and [T2] corners
   independently and so over-rejects when a row over one is checked against a
   row over the other, even though [T2 <: T1] makes the relation hold -- see
   two_param_bound.php. Asserting equality would block on that known
   incompleteness and lose the unsoundness check this property exists for.

   Cost control: the pair space is quadratic, so this uses a small row universe
   rather than [all_rows]. *)
let pair_rows : orow list =
  [
    bottom_row;
    { fields = []; unknown = TNothing };
    { fields = [("a", (false, TInt))]; unknown = TNothing };
    { fields = [("b", (false, TBool))]; unknown = TNothing };
    { fields = []; unknown = TUnion [TInt; TBool] };
  ]

let pair_templates :
    (string * (locl_ty -> locl_ty -> locl_ty) * (orow -> orow -> orow list))
    list =
  [
    ("...T2", (fun _t1 t2 -> splat [t2]), (fun _r1 r2 -> [r2]));
    ( "...T2, shape('b'=>int)",
      (fun _t1 t2 -> splat [t2; hack_of_orow row_b_int]),
      (fun _r1 r2 -> [r2; row_b_int]) );
    ("...T1", (fun t1 _t2 -> splat [t1]), (fun r1 _r2 -> [r1]));
    ( "...T1, shape('b'=>int)",
      (fun t1 _t2 -> splat [t1; hack_of_orow row_b_int]),
      (fun r1 _r2 -> [r1; row_b_int]) );
  ]

let prop_param_bounded_by_param _ =
  List.iter pair_templates ~f:(fun (sub_lbl, sub_t, sub_rows_t) ->
      List.iter pair_templates ~f:(fun (super_lbl, super_t, super_rows_t) ->
          List.iter pair_rows ~f:(fun upper1 ->
              let env = dummy_env () in
              (* T1 as upper1; T2 as T1, with the dual lower bound the
                 typechecker would propagate. *)
              let t1 = tgeneric "T1" and t2 = tgeneric "T2" in
              let env = Env.add_upper_bound env "T1" (hack_of_orow upper1) in
              let env = Env.add_upper_bound env "T2" t1 in
              let env = Env.add_lower_bound env "T1" t2 in
              let pairs =
                List.concat_map pair_rows ~f:(fun r1 ->
                    List.filter_map pair_rows ~f:(fun r2 ->
                        if subrow_denot r1 upper1 && subrow_denot r2 r1 then
                          Some (r1, r2)
                        else
                          None))
              in
              if not (List.is_empty pairs) then begin
                let decision =
                  Typing_subtype.is_sub_type env (sub_t t1 t2) (super_t t1 t2)
                in
                let reported =
                  let (_env, err) =
                    Typing_subtype.sub_type
                      env
                      (sub_t t1 t2)
                      (super_t t1 t2)
                      (Some
                         (Typing_error.Reasons_callback.unify_error_at Pos.none))
                  in
                  Option.is_none err
                in
                let oracle =
                  List.for_all pairs ~f:(fun (r1, r2) ->
                      subrow_denot
                        (merge_orows (sub_rows_t r1 r2))
                        (merge_orows (super_rows_t r1 r2)))
                in
                assert_bool
                  (Printf.sprintf
                     "param-bounded-by-param ACCEPTS what the oracle rejects:\n  sub=%s  super=%s\n  T1 upper=%s\n  pairs=%d  decision=%b  reported-ok=%b  oracle=%b"
                     sub_lbl
                     super_lbl
                     (show_orow upper1)
                     (List.length pairs)
                     decision
                     reported
                     oracle)
                  (((not decision) || oracle) && ((not reported) || oracle))
              end)))

(* == Inference oracle: spread type-variables via ∃-instantiation =========== *)

(* At a call site a splat type-parameter becomes a fresh inference variable. The
   local decision [is_sub_type] with a fresh, unconstrained spread var in the sub-
   or super-row is sound+complete iff it accepts exactly when SOME row R can
   instantiate the var to make the ground subrow hold: the ∃-semantics of
   inference, dual to the ∀-semantics of a rigid parameter. This is the property
   the [poly_error_missing_field] soundness bug violated — an unsatisfiable
   required field on the super side was silently accepted. Ranging the oracle over
   [all_rows] models the declared bound [T as shape(...)] (an open shape admits
   every record, so R is unconstrained). *)

(* An inference var standing in for a splat type-parameter carries the declared
   [T as shape(...)] upper bound (an open shape, unknown = mixed). Without it the
   var's combined upper bound would be [mixed] — not a shape — and the corner
   procedure would (correctly) reject spreading it. *)
let open_shape_bound =
  MakeType.open_shape ~kind:(MakeType.mixed r) r TShapeMap.empty

let fresh_splat_var env =
  let (env, tv) = Env.fresh_type env Pos.none in
  let env =
    match get_node tv with
    | Tvar id -> Env.add_tyvar_upper_bound env id (LoclType open_shape_bound)
    | _ -> env
  in
  (env, tv)

(* The ∃-range must include UNINHABITED witnesses: a spread var can be instantiated
   with a row whose field is required-with-type-nothing [(false, [])], which under
   rightmost-wins merge collapses the whole shape to bottom — and bottom is a subtype
   of anything. The corner procedure enumerates this "bottom corner", so without such
   rows the oracle would spuriously report [oracle=false] against a correct
   [decision=true] (e.g. shape('a'=>bool, ...#1) <: shape() closed, satisfied by
   #1 := shape('a'=>nothing)). These rows are only meaningful as instantiations, so
   they extend the ∃-range but are kept out of [all_rows]. *)
let witness_rows =
  [
    { fields = [("a", (false, TNothing))]; unknown = TNothing };
    { fields = [("b", (false, TNothing))]; unknown = TNothing };
    {
      fields = [("a", (false, TNothing)); ("b", (false, TNothing))];
      unknown = TNothing;
    };
    (* a nested-shape-typed witness so nested shapes appear in the ∃-range *)
    {
      fields = [("a", (false, List.nth_exn nested_shape_types 0))];
      unknown = TNothing;
    };
  ]

let inst_rows = all_rows @ witness_rows

let exists_over rows ~sub_rows ~super_rows =
  List.exists rows ~f:(fun rr ->
      subrow_denot (merge_orows (sub_rows rr)) (merge_orows (super_rows rr)))

(* The inference decision models a call site: record the subtyping constraints
   (which may add tvar bounds rather than decide immediately — [is_sub_type]
   returns [false] for that "needs inference" case, so it is NOT the right SUT
   here) then force-solve the remaining vars. The query is accepted iff neither
   step reports an error. *)
(* [sub_type] only CONSTRUCTS a [Typing_error.t] via its callback; driven with a
   [None] callback an unsatisfiable subtype returns [None] and looks accepted. A
   call site always supplies a callback, so we do too. *)
let on_error = Some (Typing_error.Reasons_callback.unify_error_at Pos.none)

let infer_accepts env sub super =
  let (env, err1) = Typing_subtype.sub_type env sub super on_error in
  let (_env, err2) = Typing_solver.solve_all_unsolved_tyvars env in
  ( Option.is_none err1 && Option.is_none err2,
    Option.is_some err1,
    Option.is_some err2 )

(* Inference has ∃-semantics (accept iff SOME instantiation validates the ground
   subrow), but the full instantiation space is not finitely enumerable, so we
   assert two robust half-directions instead of a single equality:

   - SOUNDNESS: if the engine accepts, a witness must exist even over the RICH
     range (which includes uninhabited "bottom-collapse" rows). A [decision=true]
     with no witness would be an unsound over-acceptance (the class of the
     [poly_error_missing_field] bug).
   - COMPLETENESS (restricted): if an INHABITED witness exists (over [all_rows],
     no bottom-collapse), the engine must accept. This catches rejecting a
     realistic satisfiable query, while tolerating the corner procedure's benign
     incompleteness for witnesses that require instantiating a var to an
     uninhabited shape via a label absent from the query. *)
let check_infer ~label ~env ~sub_hack ~super_hack ~sub_rows ~super_rows =
  let (decision, e1, e2) = infer_accepts env sub_hack super_hack in
  let witness_rich = exists_over inst_rows ~sub_rows ~super_rows in
  let witness_inhabited = exists_over all_rows ~sub_rows ~super_rows in
  let sound = (not decision) || witness_rich in
  let complete = (not witness_inhabited) || decision in
  assert_bool
    (Printf.sprintf
       "inference disagrees with ∃-instantiation oracle (%s):\n  decision=%b witness_rich=%b witness_inhabited=%b (sub_type_err=%b solve_err=%b)"
       label
       decision
       witness_rich
       witness_inhabited
       e1
       e2)
    (sound && complete)

(* Var on the SUB side, ground super (subrow_infer_sub). *)
let prop_infer_sub _ =
  List.iteri sub_templates ~f:(fun si (sub_lbl, sub_t) ->
      let (_, sub_rows) = List.nth_exn sub_row_templates si in
      List.iter all_rows ~f:(fun g ->
          let env = dummy_env () in
          let (env, tv) = fresh_splat_var env in
          check_infer
            ~label:(Printf.sprintf "sub=%s  super=%s" sub_lbl (show_orow g))
            ~env
            ~sub_hack:(sub_t tv)
            ~super_hack:(hack_of_orow g)
            ~sub_rows
            ~super_rows:(fun _ -> [g])))

(* Ground sub, var on the SUPER side (subrow_infer_super) — the bug class. *)
let prop_infer_super _ =
  List.iteri super_templates ~f:(fun pi (super_lbl, super_t) ->
      let (_, super_rows) = List.nth_exn super_row_templates pi in
      List.iter all_rows ~f:(fun g ->
          let env = dummy_env () in
          let (env, tv) = fresh_splat_var env in
          check_infer
            ~label:(Printf.sprintf "sub=%s  super=%s" (show_orow g) super_lbl)
            ~env
            ~sub_hack:(hack_of_orow g)
            ~super_hack:(super_t tv)
            ~sub_rows:(fun _ -> [g])
            ~super_rows))

(* The SAME var on both sides (subrow_infer_couple): ∃ over a shared R. *)
let prop_infer_couple _ =
  List.iteri sub_templates ~f:(fun si (sub_lbl, sub_t) ->
      let (_, sub_rows) = List.nth_exn sub_row_templates si in
      List.iteri super_templates ~f:(fun pi (super_lbl, super_t) ->
          let (_, super_rows) = List.nth_exn super_row_templates pi in
          let env = dummy_env () in
          let (env, tv) = fresh_splat_var env in
          check_infer
            ~label:
              (Printf.sprintf "sub=%s  super=%s (shared)" sub_lbl super_lbl)
            ~env
            ~sub_hack:(sub_t tv)
            ~super_hack:(super_t tv)
            ~sub_rows
            ~super_rows))

(* Run a same-variable inference query with the shared spread variable fixed to
   the empty closed shape and rigid [G] fixed to shape('a' => int). *)
let infer_couple_with_rigid_and_empty_var ~sub ~super =
  let empty = hack_of_orow { fields = []; unknown = TNothing } in
  let generic_ty =
    hack_of_orow { fields = [("a", (false, TInt))]; unknown = TNothing }
  in
  let env = Env.add_lower_bound (dummy_env ()) "G" generic_ty in
  let env = Env.add_upper_bound env "G" generic_ty in
  let (env, tv) = fresh_splat_var env in
  let env =
    match get_node tv with
    | Tvar id ->
      let env = Env.add_tyvar_lower_bound env id (LoclType empty) in
      Env.add_tyvar_upper_bound env id (LoclType empty)
    | _ -> env
  in
  infer_accepts env (sub (tgeneric "G") tv) (super (tgeneric "G") tv)

(* Regression for a soundness gap in the same-variable inference path. A rigid
   spread before the shared variable must not be projected as absent. *)
let prop_infer_couple_preserves_rigid_prefix _ =
  let (accepted, sub_type_error, solve_error) =
    infer_couple_with_rigid_and_empty_var
      ~sub:(fun g tv -> splat [g; tv])
      ~super:(fun _g tv -> splat [tv])
  in
  assert_bool
    (Printf.sprintf
       "shape(...G, ...#1) <: shape(...#1) was accepted with #1 fixed to shape() (sub_type_err=%b solve_err=%b)"
       sub_type_error
       solve_error)
    (not accepted)

(* A rigid spread after the shared variable on the sub side must likewise remain
   visible. *)
let prop_infer_couple_preserves_rigid_sub_suffix _ =
  let (accepted, sub_type_error, solve_error) =
    infer_couple_with_rigid_and_empty_var
      ~sub:(fun g tv -> splat [tv; g])
      ~super:(fun _g tv -> splat [tv])
  in
  assert_bool
    (Printf.sprintf
       "shape(...#1, ...G) <: shape(...#1) was accepted with #1 fixed to shape() (sub_type_err=%b solve_err=%b)"
       sub_type_error
       solve_error)
    (not accepted)

(* The same applies to a rigid spread after the shared variable on the super
   side. *)
let prop_infer_couple_preserves_rigid_super_suffix _ =
  let (accepted, sub_type_error, solve_error) =
    infer_couple_with_rigid_and_empty_var
      ~sub:(fun _g tv -> splat [tv])
      ~super:(fun g tv -> splat [tv; g])
  in
  assert_bool
    (Printf.sprintf
       "shape(...#1) <: shape(...#1, ...G) was accepted with #1 fixed to shape() (sub_type_err=%b solve_err=%b)"
       sub_type_error
       solve_error)
    (not accepted)

(* Pins the "uninhabited bottom-collapse" behavior of inference against a closed
   empty super. Both queries are ∃-satisfiable ONLY by instantiating the spread var
   to an uninhabited shape (a required-nothing field), which collapses the merged
   sub to bottom (bottom <: anything):
   - PRE-field  shape('a'=>bool, ...#1) <: shape(): the var is rightmost, so it can
     override 'a' to required-nothing (a label PRESENT in the query). The corner
     procedure enumerates this corner → ACCEPTED (complete here).
   - POST-field shape(...#1, 'a'=>bool) <: shape(): 'a' is rightmost and fixed; the
     only witness makes #1 uninhabited via a FRESH label absent from the query,
     which the corner procedure does not enumerate → REJECTED. This is the documented
     benign incompleteness. The rigid (∀) analogue of both correctly errors. *)
let closed_empty = hack_of_orow { fields = []; unknown = TNothing }

let shape_a_bool =
  hack_of_orow { fields = [("a", (false, TBool))]; unknown = TNothing }

let prop_infer_uninhabited_collapse _ =
  let accepts sub =
    let env = dummy_env () in
    let (env, tv) = fresh_splat_var env in
    let (decision, _, _) = infer_accepts env (sub tv) closed_empty in
    decision
  in
  (* var rightmost — bottom-collapse via the present label 'a' IS found *)
  assert_bool
    "shape('a'=>bool, ...#1) <: shape() should be accepted (bottom corner found)"
    (accepts (fun tv -> splat [shape_a_bool; tv]));
  (* var leftmost — collapse would need a fresh label; NOT found (incompleteness) *)
  assert_bool
    "shape(...#1, 'a'=>bool) <: shape() is rejected (fresh-label collapse not enumerated)"
    (not (accepts (fun tv -> splat [tv; shape_a_bool])))

(* == union_ty sanity: it really is the value-set union ===================== *)

(* Verify [value_has_type v (union_ty a b) = value_has_type v a || value_has_type v b]
   over all field-type/value combinations the model uses. This pins the merge
   semantics independently of the subtyping engine. *)
let prop_union_ty _ =
  let tys = scalar_field_types @ nested_shape_types in
  let vals = top_field_values in
  List.iter tys ~f:(fun a ->
      List.iter tys ~f:(fun b ->
          let u = union_ty a b in
          List.iter vals ~f:(fun v ->
              let lhs = value_has_type v u in
              let rhs = value_has_type v a || value_has_type v b in
              assert_bool
                (Printf.sprintf
                   "union_ty not value-set union:\n  a=%s b=%s union=%s v=%s\n  lhs=%b rhs=%b"
                   (show_ty a)
                   (show_ty b)
                   (show_ty u)
                   (match v with
                   | VInt -> "int"
                   | VBool -> "bool"
                   | VShape _ -> "shape-value")
                   lhs
                   rhs)
                (Bool.equal lhs rhs))))

(* == Union-operand oracle: distribution vs denotation ===================== *)

(* A splat element for the oracle: a concrete row, or a union of rows (which the
   normaliser distributes). A splat containing unions denotes the UNION over the
   cartesian product of each element's choices, each a rightmost-wins merge:
     [shape(e1, .., (o1|o2), .., en)] = OR over branch in cartesian of
       [merge(branch)]. *)
type uelem =
  | C of orow
  | U of orow list

let uelem_hack = function
  | C o -> hack_of_orow o
  | U os -> MakeType.union r (List.map os ~f:hack_of_orow)

let uelem_choices = function
  | C o -> [o]
  | U os -> os

(* Distribute a splat into every concrete branch (cartesian over choices), each
   merged rightmost-wins. This is the denotational reference for distribution. *)
let ubranches (elems : uelem list) : orow list =
  List.fold_right elems ~init:[[]] ~f:(fun e acc ->
      List.concat_map (uelem_choices e) ~f:(fun o ->
          List.map acc ~f:(fun rest -> o :: rest)))
  |> List.map ~f:merge_orows

let usplat elems = splat (List.map elems ~f:uelem_hack)

let show_uelems elems =
  String.concat
    ~sep:", "
    (List.map elems ~f:(function
        | C o -> "..." ^ show_orow o
        | U os ->
          "...(" ^ String.concat ~sep:" | " (List.map os ~f:show_orow) ^ ")"))

(* Small member/rest pool to bound the cross product (the 4 [bound_rows]). *)
let upool = bound_rows

let two_member_unions =
  List.concat_map upool ~f:(fun a -> List.map upool ~f:(fun b -> [a; b]))

(* Configs placing a union at every position, alone and flanked by concrete
   neighbours (leading, trailing, both), plus a 3-member union. Length <= 3 —
   this exercises distribution COMBINED with merge on either side. *)
let union_configs : uelem list list =
  let flanked =
    List.concat_map two_member_unions ~f:(fun u ->
        [U u] :: List.concat_map upool ~f:(fun rc -> [[U u; C rc]; [C rc; U u]]))
  in
  let both_sides =
    match upool with
    | a :: b :: _ -> List.map two_member_unions ~f:(fun u -> [C a; U u; C b])
    | _ -> []
  in
  let three_member =
    match upool with
    | a :: b :: c :: _ -> [[U [a; b; c]]; [C a; U [a; b; c]; C b]]
    | _ -> []
  in
  flanked @ both_sides @ three_member

(* KNOWN PRE-EXISTING UNSOUNDNESS (not introduced by shape splats).
   [Typing_union.union_shapes] collapses a union of shapes into a SINGLE shape that
   is a proper SUPERTYPE of the union: it unions the unknowns (dropping a closed
   branch's closedness) and, for a field present in only some branches, replaces the
   field type with the other branch's [unknown] instead of unioning it (dropping that
   field's type). That collapse is a sound JOIN in value positions, but the splat
   distribution feeds it into subtyping via [union_list] ([ty_of_merge_result] in
   typing_corners.ml). Used as the SUPER it accepts too much; used as the SUB (after
   union-left distribution) it drops fields. Both directions therefore disagree with
   the denotational oracle.

   We do NOT fix that here. Instead we PIN the exact number of disagreements so the
   properties keep guarding every OTHER case, and so a future fix to [union_shapes]
   trips this test and prompts tightening these back to full soundness/equality. *)
let known_union_sub_disagreements = 14

let known_union_super_unsound = 796

(* SUB is a distributed union: union-on-the-left would be COMPLETE (engine decision =
   denotation, every branch <: super) but for the [union_shapes] collapse above, which
   yields [known_union_sub_disagreements] disagreements. *)
let prop_union_sub _ =
  let env = dummy_env () in
  let disagreements = ref 0 in
  List.iter union_configs ~f:(fun elems ->
      List.iter all_rows ~f:(fun super ->
          let decision =
            Typing_subtype.is_sub_type env (usplat elems) (hack_of_orow super)
          in
          let oracle =
            List.for_all (ubranches elems) ~f:(fun m -> subrow_denot m super)
          in
          if not (Bool.equal decision oracle) then incr disagreements));
  assert_bool
    (Printf.sprintf
       "union-sub: %d disagreements with the denotation, expected the known %d from the pre-existing Typing_union.union_shapes over-approximation. If this changed, union_shapes was likely fixed - tighten this property back to equality (see comment)."
       !disagreements
       known_union_sub_disagreements)
    (Int.equal !disagreements known_union_sub_disagreements)

(* SUPER is a distributed union: union-on-the-right is SOUND but INCOMPLETE (a [sub]
   whose records split across branches is admitted denotationally but not by the
   disjunctive rule), so the property is soundness only [decision => denot sub subset
   union of branch denots]. Even soundness fails today for [known_union_super_unsound]
   cases, all unsound over-acceptances from the [union_shapes] collapse above. *)
let prop_union_super _ =
  let env = dummy_env () in
  let unsound = ref 0 in
  List.iter union_configs ~f:(fun elems ->
      let branch_denots = List.map (ubranches elems) ~f:denot in
      List.iter all_rows ~f:(fun sub ->
          let decision =
            Typing_subtype.is_sub_type env (hack_of_orow sub) (usplat elems)
          in
          let oracle =
            List.for_all (denot sub) ~f:(fun rc ->
                List.exists branch_denots ~f:(fun d ->
                    List.exists d ~f:(record_equal rc)))
          in
          if not ((not decision) || oracle) then incr unsound));
  assert_bool
    (Printf.sprintf
       "union-super: %d unsound over-acceptances, expected the known %d from the pre-existing Typing_union.union_shapes over-approximation. If this dropped, union_shapes was likely tightened - restore the full soundness assertion here (see comment)."
       !unsound
       known_union_super_unsound)
    (Int.equal !unsound known_union_super_unsound)

(* == Union with a PARAM member: distribution ∘ rigid-param forall =========== *)

(* The rigid corner reasons about T's UPPER BOUND (its worst case), so the
   forall oracle must range over instantiations reaching that bound — including
   rows with a MULTI-ATOM field ([int|bool]), which [all_rows] omits. Without
   them the forall misses the witness that a wider upper-bound field violates a
   narrower super field (e.g. [T as shape(?'a'=>int|bool)], super [shape(?'a'=>int)]:
   the engine correctly rejects, but a coarse box would spuriously accept). *)
let rich_field_states =
  field_states
  @ [Some (false, TUnion [TInt; TBool]); Some (true, TUnion [TInt; TBool])]

let rich_rows : orow list =
  List.concat_map rich_field_states ~f:(fun fa ->
      List.concat_map rich_field_states ~f:(fun fb ->
          List.map unknowns ~f:(fun unknown ->
              let fields =
                List.filter_map
                  [("a", fa); ("b", fb)]
                  ~f:(fun (l, o) -> Option.map o ~f:(fun f -> (l, f)))
              in
              { fields; unknown })))

let rich_box lower upper =
  List.filter rich_rows ~f:(fun rr ->
      subrow_denot lower rr && subrow_denot rr upper)

(* A union whose members carry a rigid type parameter combines two mechanisms:
   the union distributes, and the [shape(...T)] branch is checked by the corner's
   forall-over-the-bound procedure. Sub-side: [shape(...(shape(...T) | B)) <: C]
   with [T] rigid holds iff (forall R in T's box: [shape(...R) <: C]) AND
   [shape(...B) <: C] — union-left is complete and each branch is the rigid /
   ground check we already trust. Assert equality. *)
let prop_union_rigid_param _ =
  let name = "T" in
  List.iter bound_rows ~f:(fun lower ->
      List.iter bound_rows ~f:(fun upper ->
          if subrow_denot lower upper then begin
            let b = rich_box lower upper in
            if not (List.is_empty b) then
              List.iter upool ~f:(fun other ->
                  List.iter all_rows ~f:(fun super ->
                      let env =
                        add_generic
                          (dummy_env ())
                          ~name
                          ~lower:(Some lower)
                          ~upper
                      in
                      let union_operand =
                        MakeType.union
                          r
                          [splat [tgeneric name]; hack_of_orow other]
                      in
                      let decision =
                        Typing_subtype.is_sub_type
                          env
                          (splat [union_operand])
                          (hack_of_orow super)
                      in
                      let oracle =
                        List.for_all b ~f:(fun rr ->
                            subrow_denot (merge_orows [rr]) super)
                        && subrow_denot (merge_orows [other]) super
                      in
                      assert_bool
                        (Printf.sprintf
                           "union-with-rigid-param-member disagrees with forall oracle:\n  sub=shape(...(shape(...T) | %s))\n  T lower=%s upper=%s (box=%d)\n  super=%s\n  decision=%b oracle=%b"
                           (show_orow other)
                           (show_orow lower)
                           (show_orow upper)
                           (List.length b)
                           (show_orow super)
                           decision
                           oracle)
                        (Bool.equal decision oracle)))
          end))

let () =
  "shapeSplatOracleTest"
  >::: [
         "prop_union_ty" >:: prop_union_ty;
         "prop_ground_simple" >:: prop_ground_simple;
         "prop_ground_splat" >:: prop_ground_splat;
         "prop_union_sub" >:: prop_union_sub;
         "prop_union_super" >:: prop_union_super;
         "prop_union_rigid_param" >:: prop_union_rigid_param;
         "prop_rigid_param" >:: prop_rigid_param;
         "prop_param_bounded_by_param" >:: prop_param_bounded_by_param;
         "prop_infer_sub" >:: prop_infer_sub;
         "prop_infer_super" >:: prop_infer_super;
         "prop_infer_couple" >:: prop_infer_couple;
         "prop_infer_couple_preserves_rigid_prefix"
         >:: prop_infer_couple_preserves_rigid_prefix;
         "prop_infer_couple_preserves_rigid_sub_suffix"
         >:: prop_infer_couple_preserves_rigid_sub_suffix;
         "prop_infer_couple_preserves_rigid_super_suffix"
         >:: prop_infer_couple_preserves_rigid_super_suffix;
         "prop_infer_uninhabited_collapse" >:: prop_infer_uninhabited_collapse;
       ]
  |> run_test_tt_main
