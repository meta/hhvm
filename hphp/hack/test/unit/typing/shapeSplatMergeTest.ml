(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

(* Unit tests for the shape-splat merge (Typing_shape_normalize), the OCaml
 * reference for the runtime/HHBBC resolvers. The case table here is kept
 * aligned with the C++ unit test
 * (hphp/runtime/test/type-structure-splat.cpp) so both engines are verified
 * against the same semantics. *)

open Hh_prelude
open OUnit2
open Typing_defs
module Env = Typing_env
module MakeType = Typing_make_type
module Reason = Typing_reason
module Norm = Typing_shape_normalize

let dummy_env =
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

let expect_row normalized =
  match Norm.Row.as_row normalized with
  | Some row -> row
  | None ->
    assert_failure "expected one normalized row, got a distributed result"

let tint = MakeType.int r

let tbool = MakeType.bool r

let tfloat = MakeType.float r

let tnull = MakeType.null r

let tresource = MakeType.resource r

let tnum = MakeType.num r

let tnothing = MakeType.nothing r

let tmixed = MakeType.mixed r

let tdynamic = MakeType.dynamic r

let field ?(optional = false) ty = { sft_optional = optional; sft_ty = ty }

let key name = TShapeField.make (TSFlit_str (Pos_or_decl.none, name))

let mk_fields kvs =
  List.fold kvs ~init:TShapeMap.empty ~f:(fun acc (k, v) ->
      TShapeMap.add (key k) v acc)

let simple ?(unknown = tnothing) kvs =
  {
    s_origin = Missing_origin;
    s_unknown_value = unknown;
    s_fields = mk_fields kvs;
  }

let shape_ty s = mk (r, Tshape (Shape_simple s))

let merge_simple_shapes shape_left shape_right =
  let (_, err, result) =
    Norm.merge
      ~on_error:None
      [shape_ty shape_left; shape_ty shape_right]
      dummy_env
  in
  assert_equal None err;
  match result with
  | Norm.Full (ty, _) ->
    (match get_node ty with
    | Tshape (Shape_simple shape) -> shape
    | _ -> assert_failure "expected a simple shape")
  | Norm.Empty_shape _ -> simple []
  | Norm.Partial _ -> assert_failure "expected a fully merged shape"
  | Norm.Union _
  | Norm.Intersection _ ->
    assert_failure "expected a simple shape, not a distributed result"

let get_field s name = TShapeMap.find_opt (key name) s.s_fields

let is_nothing ty =
  match get_node ty with
  | Tunion [] -> true
  | _ -> false

let assert_merge_result_is_bottom label = function
  | Norm.Full (ty, _) ->
    assert_bool (label ^ ": should collapse to nothing") (is_nothing ty)
  | Norm.Empty_shape _ ->
    assert_failure (label ^ ": expected Full nothing, got empty shape")
  | Norm.Partial _ ->
    assert_failure (label ^ ": expected Full nothing, got residual")
  | Norm.Union _
  | Norm.Intersection _ ->
    assert_failure (label ^ ": expected Full nothing, got distributed result")

let is_dynamic ty =
  match get_node ty with
  | Tdynamic _ -> true
  | _ -> false

let assert_required name fd_opt =
  match fd_opt with
  | Some fd -> assert_bool (name ^ " should be required") (not fd.sft_optional)
  | None -> assert_failure ("missing field " ^ name)

let assert_prim name expected fd_opt =
  match fd_opt with
  | Some fd ->
    (match get_node fd.sft_ty with
    | Tprim p when Aast.equal_tprim p expected -> ()
    | _ -> assert_failure (name ^ " has unexpected type"))
  | None -> assert_failure ("missing field " ^ name)

(* shape('x' => int) + shape('y' => string) -> both required, closed. *)
let disjoint_closed _ =
  let merged =
    merge_simple_shapes
      (simple [("x", field tint)])
      (simple [("y", field tbool)])
  in
  assert_required "x" (get_field merged "x");
  assert_required "y" (get_field merged "y");
  assert_prim "x" Aast.Tint (get_field merged "x");
  assert_prim "y" Aast.Tbool (get_field merged "y");
  assert_bool "result should be closed" (is_nothing merged.s_unknown_value)

(* Rightmost wins: shape('a' => int) + shape('a' => string) -> a: string. *)
let rightmost_wins _ =
  let merged =
    merge_simple_shapes
      (simple [("a", field tint)])
      (simple [("a", field tbool)])
  in
  assert_required "a" (get_field merged "a");
  assert_prim "a" Aast.Tbool (get_field merged "a")

let normalize_shape_type_preserves_distributed_intersection _ =
  let intersection =
    mk
      ( r,
        Tintersection
          [
            shape_ty (simple [("x", field tint)]);
            shape_ty (simple [("x", field tbool)]);
          ] )
  in
  let (_, err, result) =
    Norm.normalize_shape_type
      ~on_error:None
      r
      (Shape_splat { ss_elems = [intersection] })
      dummy_env
  in
  assert_equal None err;
  match result with
  | Norm.Normalized_shape (Shape_simple shape) ->
    assert_required "x" (get_field shape "x");
    (match get_field shape "x" with
    | Some { sft_ty; _ } ->
      assert_bool "the intersected field should be nothing" (is_nothing sft_ty)
    | None -> assert_failure "missing field x")
  | Norm.Normalized_shape (Shape_splat _) ->
    assert_failure "intersection branches were re-spliced"
  | Norm.Normalized_union _
  | Norm.Normalized_intersection _ ->
    assert_failure "concrete intersection branches did not merge"
  | Norm.Normalized_bottom ->
    assert_failure "a required field of type nothing is not the bottom row"

let normalize_shape_type_preserves_distributed_union _ =
  let union =
    mk
      ( r,
        Tunion
          [
            shape_ty (simple [("x", field tint)]);
            shape_ty (simple [("x", field tbool)]);
          ] )
  in
  let (_, err, result) =
    Norm.normalize_shape_type
      ~on_error:None
      r
      (Shape_splat { ss_elems = [union] })
      dummy_env
  in
  assert_equal None err;
  match result with
  | Norm.Normalized_shape (Shape_simple shape) ->
    assert_required "x" (get_field shape "x");
    (match get_field shape "x" with
    | Some { sft_ty; _ } ->
      (match get_node sft_ty with
      | Tunion tys -> assert_equal 2 (List.length tys)
      | _ -> assert_failure "the unioned field should be a union")
    | None -> assert_failure "missing field x")
  | Norm.Normalized_shape (Shape_splat _) ->
    assert_failure "union branches were re-spliced"
  | Norm.Normalized_union _
  | Norm.Normalized_intersection _ ->
    assert_failure "concrete union branches did not merge"
  | Norm.Normalized_bottom -> assert_failure "the union should be inhabited"

(* Required left, optional right -> required (union of types). *)
let required_optional_union _ =
  let merged =
    merge_simple_shapes
      (simple [("a", field tint)])
      (simple [("a", field ~optional:true tbool)])
  in
  assert_required "a" (get_field merged "a")

(* A field only on the left, open shape on the right -> unioned with the right's
   unknown (mixed) and absorbed; result is open. *)
let open_right_absorbs _ =
  let merged =
    merge_simple_shapes (simple [("a", field tint)]) (simple ~unknown:tmixed [])
  in
  assert_required "a" (get_field merged "a");
  assert_bool "result should be open" (not (is_nothing merged.s_unknown_value))

(* Spreading nothing yields the bottom row, collapsing the whole shape. *)
let bottom_absorbs _ =
  let (_, _, result) =
    Norm.merge
      ~on_error:None
      [shape_ty (simple [("a", field tint)]); tnothing]
      dummy_env
  in
  assert_merge_result_is_bottom "shape,nothing" result

let union_with_bottom_absorbs _ =
  let union = mk (r, Tunion [mk (r, Tgeneric "T1"); mk (r, Tgeneric "T2")]) in
  let check label elems =
    let (_, err, result) = Norm.merge ~on_error:None elems dummy_env in
    assert_equal None err;
    assert_merge_result_is_bottom label result;
    let (_, err, normalized) =
      Norm.Row.normalize
        ~on_error:None
        r
        (Shape_splat { ss_elems = elems })
        dummy_env
    in
    assert_equal None err;
    match Norm.Row.as_row normalized with
    | Some row ->
      assert_bool
        (label ^ ": should normalize to bottom")
        (Norm.Row.is_bottom row)
    | None -> assert_failure (label ^ ": normalized to a union")
  in
  check "union,nothing" [union; tnothing];
  check "nothing,union" [tnothing; union]

let intersection_with_bottom_absorbs _ =
  let intersection =
    mk (r, Tintersection [mk (r, Tgeneric "T1"); mk (r, Tgeneric "T2")])
  in
  let check label elems =
    let (_, err, result) = Norm.merge ~on_error:None elems dummy_env in
    assert_equal None err;
    assert_merge_result_is_bottom label result;
    let (_, err, normalized) =
      Norm.Row.normalize
        ~on_error:None
        r
        (Shape_splat { ss_elems = elems })
        dummy_env
    in
    assert_equal None err;
    match Norm.Row.as_row normalized with
    | Some row ->
      assert_bool
        (label ^ ": should normalize to bottom")
        (Norm.Row.is_bottom row)
    | None -> assert_failure (label ^ ": normalized to a distributed result")
  in
  check "intersection,nothing" [intersection; tnothing];
  check "nothing,intersection" [tnothing; intersection]

let malformed_union_with_bottom_still_reports _ =
  let union = mk (r, Tunion [tint; mk (r, Tgeneric "T")]) in
  let check label elems =
    let (_, err, result) =
      Norm.merge
        ~on_error:(Some (Typing_error.Reasons_callback.unify_error_at Pos.none))
        elems
        dummy_env
    in
    (match err with
    | Some err -> assert_equal ~msg:label 1 (Typing_error.count err)
    | None -> assert_failure (label ^ ": expected a malformed-splat error"));
    assert_merge_result_is_bottom label result
  in
  check "malformed union,nothing" [union; tnothing];
  check "nothing,malformed union" [tnothing; union]

let normalize_shape_type_keeps_union_explicit _ =
  let union = mk (r, Tunion [mk (r, Tgeneric "T1"); mk (r, Tgeneric "T2")]) in
  let (_, err, normalized) =
    Norm.normalize_shape_type
      ~on_error:None
      r
      (Shape_splat { ss_elems = [union] })
      dummy_env
  in
  assert_equal None err;
  match normalized with
  | Norm.Normalized_union tys -> assert_equal 2 (List.length tys)
  | Norm.Normalized_intersection _ ->
    assert_failure "a union normalized to an intersection"
  | Norm.Normalized_shape _ ->
    assert_failure "a distributed union was returned as a normalized shape"
  | Norm.Normalized_bottom ->
    assert_failure "a union of type parameters normalized to bottom"

let normalize_shape_type_keeps_intersection_explicit _ =
  let intersection =
    mk (r, Tintersection [mk (r, Tgeneric "T1"); mk (r, Tgeneric "T2")])
  in
  let (_, err, normalized) =
    Norm.normalize_shape_type
      ~on_error:None
      r
      (Shape_splat { ss_elems = [intersection] })
      dummy_env
  in
  assert_equal None err;
  match normalized with
  | Norm.Normalized_intersection tys -> assert_equal 2 (List.length tys)
  | Norm.Normalized_union _ ->
    assert_failure "an intersection normalized to a union"
  | Norm.Normalized_shape _ ->
    assert_failure
      "a distributed intersection was returned as a normalized shape"
  | Norm.Normalized_bottom ->
    assert_failure "an intersection of type parameters normalized to bottom"

let empty_intersection_is_not_a_shape _ =
  let empty_intersection = mk (r, Tintersection []) in
  let (_, err, result) =
    Norm.merge
      ~on_error:(Some (Typing_error.Reasons_callback.unify_error_at Pos.none))
      [empty_intersection]
      dummy_env
  in
  assert_bool "spreading mixed should report an error" (Option.is_some err);
  match result with
  | Norm.Intersection _ ->
    assert_failure "an empty intersection was distributed"
  | Norm.Full _
  | Norm.Empty_shape _
  | Norm.Partial _
  | Norm.Union _ ->
    ()

(* A residual operand (type variable) cannot be flattened: a residual list. *)
let residual_tyvar _ =
  let (env, tv) =
    Env.fresh_type_reason dummy_env Pos.none (fun _ -> Reason.none)
  in
  let (_, _, result) =
    Norm.merge ~on_error:None [shape_ty (simple [("a", field tint)]); tv] env
  in
  match result with
  | Norm.Partial _ -> ()
  | Norm.Full _
  | Norm.Empty_shape _
  | Norm.Union _
  | Norm.Intersection _ ->
    assert_failure "expected residual list for a free type variable"

let printer_flattens_splat_from_solved_tyvar _ =
  let (env, tv) =
    Env.fresh_type_reason dummy_env Pos.none (fun _ -> Reason.none)
  in
  let nested =
    mk
      ( r,
        Tshape
          (Shape_splat
             {
               ss_elems =
                 [
                   shape_ty (simple [("x", field tint)]);
                   shape_ty (simple [("y", field tbool)]);
                 ];
             }) )
  in
  let (env, err) =
    match get_node tv with
    | Tvar v -> Typing_solver.bind env v nested
    | _ -> assert_failure "fresh_type_reason should return a type variable"
  in
  assert_equal None err;
  let outer =
    mk
      ( r,
        Tshape
          (Shape_splat
             { ss_elems = [tv; shape_ty (simple [("z", field tfloat)])] }) )
  in
  assert_equal
    ~printer:Fn.id
    ~msg:"printing should renormalize a solved splat variable"
    "shape('x' => int, 'y' => bool, 'z' => float)"
    (Typing_print.full_strip_ns ~hide_internals:true env outer)

let normalized_row_is_aligned _ =
  let parameter = mk (r, Tgeneric "T") in
  let (env, type_variable) =
    Env.fresh_type_reason dummy_env Pos.none (fun _ -> Reason.none)
  in
  let (_, err, singleton) =
    Norm.Row.normalize
      ~on_error:None
      r
      (Shape_splat { ss_elems = [parameter] })
      env
  in
  assert_equal None err;
  let singleton = expect_row singleton in
  Norm.Row.fold
    singleton
    ~bottom:(fun () ->
      assert_failure "a singleton parameter normalized to bottom")
    ~simple:(fun _ ->
      assert_failure "a singleton parameter normalized to a simple shape")
    ~elements:(function
      | [element] ->
        (match Norm.Row.Element.view element with
        | Norm.Row.Element.Opaque opaque ->
          (match Norm.Row.Opaque.view opaque with
          | Norm.Row.Opaque.Type_parameter ty ->
            assert_bool
              "a singleton parameter should be lifted out of the splat"
              (Typing_defs.ty_equal ty parameter)
          | Norm.Row.Opaque.Type_variable _
          | Norm.Row.Opaque.Newtype _ ->
            assert_failure
              "a singleton parameter was not classified as a type parameter")
        | Norm.Row.Element.Shape _ ->
          assert_failure "a singleton parameter normalized to a shape")
      | _ ->
        assert_failure "a singleton parameter was not normalized to an atom");
  let input =
    Shape_splat
      {
        ss_elems =
          [
            shape_ty (simple [("a", field tint)]);
            parameter;
            shape_ty (simple [("b", field tbool)]);
            shape_ty (simple [("c", field tfloat)]);
            type_variable;
            shape_ty (simple [("d", field tint)]);
          ];
      }
  in
  let (_, err, row) = Norm.Row.normalize ~on_error:None r input env in
  assert_equal None err;
  let row = expect_row row in
  Norm.Row.fold
    row
    ~bottom:(fun () -> assert_failure "the normalized row is bottom")
    ~simple:(fun _ ->
      assert_failure "a row with opaque operands normalized to a simple shape")
    ~elements:(fun elements ->
      match List.map elements ~f:Norm.Row.Element.view with
      | [
       Norm.Row.Element.Shape (_, first);
       Norm.Row.Element.Opaque parameter;
       Norm.Row.Element.Shape (_, middle);
       Norm.Row.Element.Opaque type_variable;
       Norm.Row.Element.Shape (_, last);
      ] ->
        (match Norm.Row.Opaque.view parameter with
        | Norm.Row.Opaque.Type_parameter _ -> ()
        | Norm.Row.Opaque.Type_variable _
        | Norm.Row.Opaque.Newtype _ ->
          assert_failure
            "generic operand was not classified as a type parameter");
        (match Norm.Row.Opaque.view type_variable with
        | Norm.Row.Opaque.Type_variable _ -> ()
        | Norm.Row.Opaque.Type_parameter _
        | Norm.Row.Opaque.Newtype _ ->
          assert_failure
            "inference operand was not classified as a type variable");
        assert_bool
          "first shape fragment should contain a"
          (Option.is_some (get_field first "a"));
        assert_bool
          "contiguous shape fragments should merge b"
          (Option.is_some (get_field middle "b"));
        assert_bool
          "contiguous shape fragments should merge c"
          (Option.is_some (get_field middle "c"));
        assert_bool
          "last shape fragment should contain d"
          (Option.is_some (get_field last "d"))
      | _ ->
        assert_failure "normalized splat elements are not shape/opaque aligned")

(* -- Property tests -----------------------------------------------------------
   The examples above pin specific cases; the properties below check the
   algebraic laws over a small enumerated space of shapes. The space is every
   combination of two fields (each absent / required / optional, int or bool)
   with a closed or open (mixed) unknown value: 5 * 5 * 2 = 50 shapes. Pairwise
   properties therefore range over 2500 merges, which is plenty to catch a law
   violation without a generator dependency. *)

let splat elems = mk (r, Tshape (Shape_splat { ss_elems = elems }))

let tgeneric name = mk (r, Tgeneric name)

let repeated_generic_bottom_query_is_memoized _ =
  let depth = 20 in
  let name i = Printf.sprintf "T%d" i in
  let generic i = tgeneric (name i) in
  let env =
    Env.add_upper_bound
      dummy_env
      (name depth)
      (shape_ty (simple [("leaf", field tint)]))
  in
  let env =
    List.fold_right (List.range 0 depth) ~init:env ~f:(fun i env ->
        let left = generic (i + 1) in
        let right = generic (i + 1) in
        Env.add_upper_bound env (name i) (splat [left; right]))
  in
  let outer = generic 0 in
  let (_, err, result) = Norm.merge ~on_error:None [outer] env in
  assert_bool "merge should not report an error" (Option.is_none err);
  match result with
  | Norm.Partial ([ty], false) ->
    (match get_node ty with
    | Tgeneric name -> assert_equal "T0" name
    | _ -> assert_failure "expected residual T0")
  | Norm.Partial _ ->
    assert_failure "expected exactly one non-supportdyn residual"
  | Norm.Full _
  | Norm.Empty_shape _
  | Norm.Union _
  | Norm.Intersection _ ->
    assert_failure "expected T0 to remain residual"

let malformed_operands_before_union_are_processed_once _ =
  let union = mk (r, Tunion [tgeneric "T1"; tgeneric "T2"]) in
  let (_, err, result) =
    Norm.merge
      ~on_error:(Some (Typing_error.Reasons_callback.unify_error_at Pos.none))
      [tint; tint; union]
      dummy_env
  in
  (match err with
  | Some err -> assert_equal 2 (Typing_error.count err)
  | None -> assert_failure "expected one error per malformed source operand");
  let error_tyvars ty =
    match get_node ty with
    | Tshape (Shape_splat { ss_elems = first :: second :: _ }) ->
      (match (get_node first, get_node second) with
      | (Tvar _, Tvar _) -> (first, second)
      | _ -> assert_failure "expected error type variables")
    | _ -> assert_failure "expected a residual shape splat"
  in
  match result with
  | Norm.Union [first_branch; second_branch] ->
    let (first_1, first_2) = error_tyvars first_branch in
    let (second_1, second_2) = error_tyvars second_branch in
    assert_bool
      "the first malformed operand should reuse its error type variable"
      (Typing_defs.ty_equal first_1 second_1);
    assert_bool
      "the second malformed operand should reuse its error type variable"
      (Typing_defs.ty_equal first_2 second_2);
    assert_bool
      "distinct malformed operands should have distinct error type variables"
      (not (Typing_defs.ty_equal first_1 first_2))
  | Norm.Full _
  | Norm.Empty_shape _
  | Norm.Partial _
  | Norm.Union _
  | Norm.Intersection _ ->
    assert_failure "expected one branch per union member"

let malformed_operands_before_intersection_are_processed_once _ =
  let intersection = mk (r, Tintersection [tgeneric "T1"; tgeneric "T2"]) in
  let (_, err, result) =
    Norm.merge
      ~on_error:(Some (Typing_error.Reasons_callback.unify_error_at Pos.none))
      [tint; tint; intersection]
      dummy_env
  in
  (match err with
  | Some err -> assert_equal 2 (Typing_error.count err)
  | None -> assert_failure "expected one error per malformed source operand");
  let error_tyvars ty =
    match get_node ty with
    | Tshape (Shape_splat { ss_elems = first :: second :: _ }) ->
      (match (get_node first, get_node second) with
      | (Tvar _, Tvar _) -> (first, second)
      | _ -> assert_failure "expected error type variables")
    | _ -> assert_failure "expected a residual shape splat"
  in
  match result with
  | Norm.Intersection [first_branch; second_branch] ->
    let (first_1, first_2) = error_tyvars first_branch in
    let (second_1, second_2) = error_tyvars second_branch in
    assert_bool
      "the first malformed operand should reuse its error type variable"
      (Typing_defs.ty_equal first_1 second_1);
    assert_bool
      "the second malformed operand should reuse its error type variable"
      (Typing_defs.ty_equal first_2 second_2);
    assert_bool
      "distinct malformed operands should have distinct error type variables"
      (not (Typing_defs.ty_equal first_1 first_2))
  | Norm.Full _
  | Norm.Empty_shape _
  | Norm.Partial _
  | Norm.Union _
  | Norm.Intersection _ ->
    assert_failure "expected one branch per intersection member"

let empty_closed = simple []

let prim_of ty =
  match get_node ty with
  | Tprim p -> Some p
  | _ -> None

let ty_prim_equal t1 t2 =
  match (prim_of t1, prim_of t2) with
  | (Some p1, Some p2) -> Aast.equal_tprim p1 p2
  | _ -> false

let field_eq f1 f2 =
  Bool.equal f1.sft_optional f2.sft_optional
  && ty_prim_equal f1.sft_ty f2.sft_ty

(* Coarse comparison of the unknown-value upper bound: closed (nothing) vs open
   (anything else). Enough to distinguish the two we generate. *)
let unknown_eq u1 u2 = Bool.equal (is_nothing u1) (is_nothing u2)

let field_names = ["a"; "b"; "c"]

let shape_struct_eq s1 s2 =
  List.for_all field_names ~f:(fun k ->
      match (get_field s1 k, get_field s2 k) with
      | (None, None) -> true
      | (Some f1, Some f2) -> field_eq f1 f2
      | _ -> false)
  && unknown_eq s1.s_unknown_value s2.s_unknown_value

let all_field_states =
  [
    None;
    Some (field tint);
    Some (field tbool);
    Some (field tfloat);
    Some (field ~optional:true tint);
    Some (field ~optional:true tbool);
    Some (field ~optional:true tfloat);
  ]

(* Every combination of the three fields (each absent / required / optional, of
   int, bool, or float) with a closed or open unknown value:
   7 fields-states ^ 3 fields * 2 unknowns = 686 shapes. *)
let all_shapes =
  List.concat_map all_field_states ~f:(fun fa ->
      List.concat_map all_field_states ~f:(fun fb ->
          List.concat_map all_field_states ~f:(fun fc ->
              List.map [tnothing; tmixed] ~f:(fun unknown ->
                  let kvs =
                    List.filter_map
                      [("a", fa); ("b", fb); ("c", fc)]
                      ~f:(fun (k, o) -> Option.map o ~f:(fun fd -> (k, fd)))
                  in
                  simple ~unknown kvs))))

(* The pairwise (n^2) properties below use a two-field projection to keep the
   cross product tractable (7^2 * 2 = 98 shapes -> ~9.6k merges per property).
   Rightmost-wins and key-union are per-field / per-operand-set properties, so
   two fields already exercise every presence/absence and requiredness x type
   combination that matters. *)
let pair_shapes =
  List.concat_map all_field_states ~f:(fun fa ->
      List.concat_map all_field_states ~f:(fun fb ->
          List.map [tnothing; tmixed] ~f:(fun unknown ->
              let kvs =
                List.filter_map
                  [("a", fa); ("b", fb)]
                  ~f:(fun (k, o) -> Option.map o ~f:(fun fd -> (k, fd)))
              in
              simple ~unknown kvs)))

let full_shape label = function
  | (_, _, Norm.Full (ty, _)) ->
    (match get_node ty with
    | Tshape (Shape_simple s) -> s
    | _ -> assert_failure (label ^ ": expected a simple shape"))
  (* An all-identity merge collapses to the empty closed shape (the unit). *)
  | (_, _, Norm.Empty_shape _) -> empty_closed
  | (_, _, Norm.Partial _) ->
    assert_failure (label ^ ": expected Full/Empty_shape, got residual")
  | (_, _, (Norm.Union _ | Norm.Intersection _)) ->
    assert_failure
      (label ^ ": expected Full/Empty_shape, got union/intersection")

(* Rightmost wins: any field required in the right operand appears required with
   the right operand's exact type in the merge. *)
let prop_rightmost_wins _ =
  List.iter pair_shapes ~f:(fun sl ->
      List.iter pair_shapes ~f:(fun sr ->
          let merged = merge_simple_shapes sl sr in
          List.iter field_names ~f:(fun k ->
              match get_field sr k with
              | Some fd_r when not fd_r.sft_optional ->
                (match get_field merged k with
                | Some fd ->
                  assert_bool
                    (k ^ " should stay required (rightmost wins)")
                    (not fd.sft_optional);
                  assert_bool
                    (k ^ " should keep the right operand's type")
                    (ty_prim_equal fd.sft_ty fd_r.sft_ty)
                | None -> assert_failure (k ^ " missing after rightmost-wins"))
              | _ -> ())))

(* The merged field set is exactly the union of the operands' field sets. *)
let prop_keys_union _ =
  List.iter pair_shapes ~f:(fun sl ->
      List.iter pair_shapes ~f:(fun sr ->
          let merged = merge_simple_shapes sl sr in
          List.iter field_names ~f:(fun k ->
              let in_l = Option.is_some (get_field sl k) in
              let in_r = Option.is_some (get_field sr k) in
              let in_m = Option.is_some (get_field merged k) in
              assert_bool
                (k ^ " should be present iff present in an operand")
                (Bool.equal in_m (in_l || in_r)))))

(* A [nothing] operand collapses the whole merge to [nothing], in either
   position. *)
let prop_bottom_absorbs _ =
  List.iter all_shapes ~f:(fun s ->
      let check label elems =
        match Norm.merge ~on_error:None elems dummy_env with
        | (_, _, Norm.Full (ty, _)) ->
          assert_bool (label ^ ": should collapse to nothing") (is_nothing ty)
        | ( _,
            _,
            ( Norm.Empty_shape _ | Norm.Partial _ | Norm.Union _
            | Norm.Intersection _ ) ) ->
          assert_failure (label ^ ": expected Full nothing")
      in
      check "s,nothing" [shape_ty s; tnothing];
      check "nothing,s" [tnothing; shape_ty s])

(* The empty closed shape is a unit for merge, in either position. *)
let prop_empty_identity _ =
  List.iter all_shapes ~f:(fun s ->
      let left =
        full_shape
          "s,empty"
          (Norm.merge
             ~on_error:None
             [shape_ty s; shape_ty empty_closed]
             dummy_env)
      in
      let right =
        full_shape
          "empty,s"
          (Norm.merge
             ~on_error:None
             [shape_ty empty_closed; shape_ty s]
             dummy_env)
      in
      assert_bool
        "empty shape should be a left/right unit"
        (shape_struct_eq left s);
      assert_bool
        "empty shape should be a left/right unit"
        (shape_struct_eq right s))

(* A splat wrapping a single shape flattens to that shape. *)
let prop_flatten_singleton _ =
  List.iter all_shapes ~f:(fun s ->
      let via_splat =
        full_shape
          "splat"
          (Norm.merge ~on_error:None [splat [shape_ty s]] dummy_env)
      in
      let direct =
        full_shape "direct" (Norm.merge ~on_error:None [shape_ty s] dummy_env)
      in
      assert_bool
        "singleton splat should flatten"
        (shape_struct_eq via_splat direct))

(* Spreading dynamic on the right opens the row with dynamic as the unknown-field
   bound and unions a left field with dynamic:
   shape('a' => int) + dynamic -> a: (int | dynamic), open (unknown = dynamic). *)
let dynamic_on_right_unions _ =
  let s =
    full_shape
      "dyn_right"
      (Norm.merge
         ~on_error:None
         [shape_ty (simple [("a", field tint)]); tdynamic]
         dummy_env)
  in
  assert_required "a" (get_field s "a");
  assert_bool "unknown should be dynamic" (is_dynamic s.s_unknown_value);
  match get_field s "a" with
  | Some fd ->
    assert_bool
      "a should be unioned with dynamic (no longer plain int)"
      (not (ty_prim_equal fd.sft_ty tint))
  | None -> assert_failure "missing a"

(* Spreading dynamic on the left leaves a rightmost concrete field unchanged but
   still opens the row with dynamic:
   dynamic + shape('a' => int) -> a: int (required), open (unknown = dynamic). *)
let dynamic_on_left_field_wins _ =
  let s =
    full_shape
      "dyn_left"
      (Norm.merge
         ~on_error:None
         [tdynamic; shape_ty (simple [("a", field tint)])]
         dummy_env)
  in
  assert_required "a" (get_field s "a");
  assert_bool "unknown should be dynamic" (is_dynamic s.s_unknown_value);
  assert_prim "a" Aast.Tint (get_field s "a")

let rec has_primitive prim ty =
  match get_node ty with
  | Tprim actual -> Aast.equal_tprim prim actual
  | Toption ty -> Aast.equal_tprim prim Aast.Tnull || has_primitive prim ty
  | Tunion tys -> List.exists tys ~f:(has_primitive prim)
  | _ -> false

let deferred_widening_skips_explicit_rows _ =
  let shapes =
    [
      simple ~unknown:tnum [("a", field ~optional:true tnull)];
      simple ~unknown:tresource [];
      simple ~unknown:tfloat [("a", field ~optional:true tbool)];
    ]
  in
  let result =
    Norm.merge ~on_error:None (List.map shapes ~f:shape_ty) dummy_env
  in
  let actual = full_shape "deferred_widening" result in
  match get_field actual "a" with
  | Some field ->
    assert_bool "a should remain optional" field.sft_optional;
    List.iter [Aast.Tnull; Aast.Tresource; Aast.Tbool] ~f:(fun prim ->
        assert_bool
          ("a should include " ^ Nast.show_tprim prim)
          (has_primitive prim field.sft_ty));
    List.iter [Aast.Tnum; Aast.Tfloat] ~f:(fun prim ->
        assert_bool
          ("a should exclude " ^ Nast.show_tprim prim)
          (not (has_primitive prim field.sft_ty)))
  | None -> assert_failure "deferred widening should preserve field a"

let () =
  "shapeSplatMergeTest"
  >::: [
         "disjoint_closed" >:: disjoint_closed;
         "rightmost_wins" >:: rightmost_wins;
         "normalize_shape_type_preserves_distributed_intersection"
         >:: normalize_shape_type_preserves_distributed_intersection;
         "normalize_shape_type_preserves_distributed_union"
         >:: normalize_shape_type_preserves_distributed_union;
         "required_optional_union" >:: required_optional_union;
         "open_right_absorbs" >:: open_right_absorbs;
         "bottom_absorbs" >:: bottom_absorbs;
         "union_with_bottom_absorbs" >:: union_with_bottom_absorbs;
         "intersection_with_bottom_absorbs" >:: intersection_with_bottom_absorbs;
         "malformed_union_with_bottom_still_reports"
         >:: malformed_union_with_bottom_still_reports;
         "normalize_shape_type_keeps_union_explicit"
         >:: normalize_shape_type_keeps_union_explicit;
         "normalize_shape_type_keeps_intersection_explicit"
         >:: normalize_shape_type_keeps_intersection_explicit;
         "empty_intersection_is_not_a_shape"
         >:: empty_intersection_is_not_a_shape;
         "residual_tyvar" >:: residual_tyvar;
         "printer_flattens_splat_from_solved_tyvar"
         >:: printer_flattens_splat_from_solved_tyvar;
         "normalized_row_is_aligned" >:: normalized_row_is_aligned;
         "repeated_generic_bottom_query_is_memoized"
         >:: repeated_generic_bottom_query_is_memoized;
         "malformed_operands_before_union_are_processed_once"
         >:: malformed_operands_before_union_are_processed_once;
         "malformed_operands_before_intersection_are_processed_once"
         >:: malformed_operands_before_intersection_are_processed_once;
         "dynamic_on_right_unions" >:: dynamic_on_right_unions;
         "dynamic_on_left_field_wins" >:: dynamic_on_left_field_wins;
         "deferred_widening_skips_explicit_rows"
         >:: deferred_widening_skips_explicit_rows;
         "prop_rightmost_wins" >:: prop_rightmost_wins;
         "prop_keys_union" >:: prop_keys_union;
         "prop_bottom_absorbs" >:: prop_bottom_absorbs;
         "prop_empty_identity" >:: prop_empty_identity;
         "prop_flatten_singleton" >:: prop_flatten_singleton;
       ]
  |> run_test_tt_main
