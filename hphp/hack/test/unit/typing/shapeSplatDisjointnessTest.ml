(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

(* Tests for the <<__DisjointShapeSplat>> decision procedure
 * (Typing_shape_disjointness), against denotational oracles
 *
 * Two levels:
 *
 *   - The [Label_bound] lattice. Over an infinite label space [Bottom_plus s]
 *     denotes [s] and [Top_minus s] its complement. We model that with a
 *     FINITE universe containing a spare label that no constructed set ever
 *     mentions, so a complement is always inhabited and two complements always
 *     intersect — which is what makes the finite model faithful. Every operation
 *     is then checked against the corresponding set operation by brute force.
 *
 *   - The whole check. Base Quickcheck generates recursive descriptions of splat
 *     elements. Each description is materialized as a type and independently
 *     interpreted as the set of labels it could supply. The check must report
 *     exactly when two elements' sets intersect.
 *)

open Hh_prelude
open OUnit2
open Typing_defs
module Env = Typing_env
module MakeType = Typing_make_type
module Reason = Typing_reason
module Test_api = Typing_shape_disjointness.For_test

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

let key name = TShapeField.make (TSFlit_str (Pos_or_decl.none, name))

let test_pos line =
  let file = Relative_path.from_root ~suffix:"provenance.php" in
  let offset = line * 10 in
  Pos.make_from_lnum_bol_offset
    ~pos_file:file
    ~pos_start:(line, offset, offset)
    ~pos_end:(line, offset, offset + 1)

let positioned_key pos name =
  TShapeField.make (TSFlit_str (Pos_or_decl.of_raw_pos pos, name))

let sset names =
  List.fold names ~init:TShapeSet.empty ~f:(fun acc n ->
      TShapeSet.add (key n) acc)

let show_set s =
  let names = List.map ~f:TShapeField.name (TShapeSet.elements s) in
  let names_str = String.concat ~sep:"," names in
  Printf.sprintf "{%s}" names_str

(* -- The label universe ------------------------------------------------------
   Constructed sets only ever mention [finite_labels]. ["z"] is the spare: it
   belongs to every complement, so [Top_minus _] is never empty and two of them
   always share it, exactly as in the real infinite label space. *)

let finite_labels = ["a"; "b"; "c"]

let universe = sset ("z" :: finite_labels)

let denot (lbl_bound : Test_api.label_bound) =
  match lbl_bound with
  | Test_api.Bottom_plus s -> s
  | Test_api.Top_minus s -> TShapeSet.diff universe s

(* Every subset of [finite_labels]. *)
let all_sets =
  List.fold finite_labels ~init:[[]] ~f:(fun acc label ->
      List.concat_map acc ~f:(fun names -> [names; label :: names]))
  |> List.map ~f:sset

let all_bounds =
  List.concat_map all_sets ~f:(fun s ->
      [Test_api.Bottom_plus s; Test_api.Top_minus s])

let show_bound (lbl_bound : Test_api.label_bound) =
  match lbl_bound with
  | Test_api.Bottom_plus s -> show_set s
  | Test_api.Top_minus s -> Printf.sprintf "⊤ / %s" (show_set s)

let assert_set_eq msg expected actual =
  assert_bool
    (Printf.sprintf
       "%s: expected %s, got %s"
       msg
       (show_set expected)
       (show_set actual))
    (TShapeSet.equal expected actual)

let iter_pairs f =
  List.iter all_bounds ~f:(fun b1 -> List.iter all_bounds ~f:(f b1))

(* -- Lattice properties ---------------------------------------------------- *)

(* denot (join b1 b2) = denot b1 ∪ denot b2 *)
let prop_join_is_union _ =
  iter_pairs (fun b1 b2 ->
      let joined = Test_api.join b1 b2 in
      assert_set_eq
        (Printf.sprintf "join %s %s" (show_bound b1) (show_bound b2))
        (TShapeSet.union (denot b1) (denot b2))
        (denot joined))

(* denot (meet b1 b2) = denot b1 ∩ denot b2 *)
let prop_meet_is_inter _ =
  iter_pairs (fun b1 b2 ->
      assert_set_eq
        (Printf.sprintf "meet %s %s" (show_bound b1) (show_bound b2))
        (TShapeSet.inter (denot b1) (denot b2))
        (denot (Test_api.meet b1 b2)))

(* disjoint b1 b2 <=> denot b1 ∩ denot b2 = {} *)
let prop_disjoint_is_empty_inter _ =
  iter_pairs (fun b1 b2 ->
      let expected =
        TShapeSet.is_empty (TShapeSet.inter (denot b1) (denot b2))
      in
      assert_bool
        (Printf.sprintf
           "disjoint %s %s should be %b"
           (show_bound b1)
           (show_bound b2)
           expected)
        (Bool.equal expected (Test_api.disjoint b1 b2)))

(* restrict b s = s ∩ denot b *)
let prop_restrict_is_inter _ =
  List.iter all_sets ~f:(fun s ->
      List.iter all_bounds ~f:(fun b ->
          assert_set_eq
            (Printf.sprintf "restrict %s %s" (show_set s) (show_bound b))
            (TShapeSet.inter s (denot b))
            (Test_api.restrict b s)))

(* possible adds the known-present labels to the element's bound. *)
let prop_possible_is_union _ =
  List.iter all_sets ~f:(fun present ->
      List.iter all_bounds ~f:(fun el_bound ->
          let widened =
            Test_api.possible (Test_api.make_element ~present ~bound:el_bound)
          in
          assert_set_eq
            "possible"
            (TShapeSet.union present (denot el_bound))
            (denot (Test_api.element_bound widened))))

(* join, meet and disjoint are symmetric in their two arguments. *)
let prop_commutative _ =
  iter_pairs (fun b1 b2 ->
      let denot_join a b = denot (Test_api.join a b) in
      assert_set_eq "join commutes" (denot_join b1 b2) (denot_join b2 b1);
      assert_set_eq
        "meet commutes"
        (denot (Test_api.meet b1 b2))
        (denot (Test_api.meet b2 b1));
      assert_bool
        "bounds_disjoint commutes"
        (Bool.equal (Test_api.disjoint b1 b2) (Test_api.disjoint b2 b1)))

(* join and meet associate under the denotation, so the grouping the fold
   happens to use does not matter. *)
let prop_associative _ =
  let join a b = Test_api.join a b in
  List.iter all_bounds ~f:(fun b1 ->
      List.iter all_bounds ~f:(fun b2 ->
          List.iter all_bounds ~f:(fun b3 ->
              assert_set_eq
                "join associates"
                (denot (join (join b1 b2) b3))
                (denot (join b1 (join b2 b3)));
              assert_set_eq
                "meet associates"
                (denot (Test_api.meet (Test_api.meet b1 b2) b3))
                (denot (Test_api.meet b1 (Test_api.meet b2 b3))))))

let shape_ty ~unknown fields =
  let s_fields =
    List.fold fields ~init:TShapeMap.empty ~f:(fun acc (name, fd) ->
        TShapeMap.add (key name) fd acc)
  in
  mk
    ( r,
      Tshape
        (Shape_simple
           { s_origin = Missing_origin; s_unknown_value = unknown; s_fields })
    )

let required_fd ty = { sft_optional = false; sft_ty = ty }

let optional_fd ty = { sft_optional = true; sft_ty = ty }

let absent_fd = { sft_optional = true; sft_ty = MakeType.nothing r }

(* A closed shape carrying exactly [present]. *)
let closed present =
  shape_ty
    ~unknown:(MakeType.nothing r)
    (List.map present ~f:(fun n -> (n, required_fd (MakeType.int r))))

let closed_at pos present =
  let s_fields =
    List.fold present ~init:TShapeMap.empty ~f:(fun fields name ->
        TShapeMap.add
          (positioned_key pos name)
          (required_fd (MakeType.int r))
          fields)
  in
  mk
    ( r,
      Tshape
        (Shape_simple
           {
             s_origin = Missing_origin;
             s_unknown_value = MakeType.nothing r;
             s_fields;
           }) )

let closed_optional_at pos optional =
  let s_fields =
    List.fold optional ~init:TShapeMap.empty ~f:(fun fields name ->
        TShapeMap.add
          (positioned_key pos name)
          (optional_fd (MakeType.int r))
          fields)
  in
  mk
    ( r,
      Tshape
        (Shape_simple
           {
             s_origin = Missing_origin;
             s_unknown_value = MakeType.nothing r;
             s_fields;
           }) )

(* An open shape carrying [present] and provably not carrying [absent]. *)
let open_shape ~present ~absent =
  shape_ty
    ~unknown:(MakeType.mixed r)
    (List.map present ~f:(fun n -> (n, required_fd (MakeType.int r)))
    @ List.map absent ~f:(fun n -> (n, absent_fd)))

(* -- Generated element kinds ----------------------------------------------- *)

type element_spec =
  | Spec_nothing
  | Spec_closed of string list
  | Spec_open of {
      present: string list;
      absent: string list;
    }
  | Spec_dynamic
  | Spec_type_var
  | Spec_nonnull
  | Spec_option of element_spec
  | Spec_generic of element_spec list
  | Spec_newtype of element_spec
  | Spec_splat of element_spec list
  | Spec_union of element_spec list
  | Spec_intersection of element_spec list
  | Spec_non_shape

let rec show_element_spec = function
  | Spec_nothing -> "nothing"
  | Spec_closed labels ->
    Printf.sprintf "closed{%s}" (String.concat ~sep:"," labels)
  | Spec_open { present; absent } ->
    Printf.sprintf
      "open{present=%s;absent=%s}"
      (String.concat ~sep:"," present)
      (String.concat ~sep:"," absent)
  | Spec_dynamic -> "dynamic"
  | Spec_type_var -> "type-var"
  | Spec_nonnull -> "nonnull"
  | Spec_option inner -> Printf.sprintf "option[%s]" (show_element_spec inner)
  | Spec_generic bounds ->
    Printf.sprintf
      "generic[%s]"
      (String.concat ~sep:"," (List.map bounds ~f:show_element_spec))
  | Spec_newtype bound -> Printf.sprintf "newtype[%s]" (show_element_spec bound)
  | Spec_splat elements ->
    Printf.sprintf
      "splat[%s]"
      (String.concat ~sep:"," (List.map elements ~f:show_element_spec))
  | Spec_union members ->
    Printf.sprintf
      "union[%s]"
      (String.concat ~sep:"," (List.map members ~f:show_element_spec))
  | Spec_intersection members ->
    Printf.sprintf
      "intersection[%s]"
      (String.concat ~sep:"," (List.map members ~f:show_element_spec))
  | Spec_non_shape -> "int"

let show_element_specs specs =
  String.concat ~sep:" + " (List.map specs ~f:show_element_spec)

let sexp_of_element_specs specs =
  Sexplib0.Sexp.List
    (List.map specs ~f:(fun spec -> Sexplib0.Sexp.Atom (show_element_spec spec)))

let gen_element_spec =
  let module Gen = Quickcheck.Generator in
  let label = String.gen_nonempty' Char.gen_lowercase in
  let labels = Gen.list label in
  Gen.weighted_recursive_union
    [
      (1.0, Gen.return Spec_nothing);
      (4.0, Gen.map labels ~f:(fun labels -> Spec_closed labels));
      ( 4.0,
        Gen.map (Gen.both labels labels) ~f:(fun (present, absent) ->
            Spec_open { present; absent }) );
      (1.0, Gen.return Spec_dynamic);
      (1.0, Gen.return Spec_type_var);
      (1.0, Gen.return Spec_nonnull);
      (1.0, Gen.return (Spec_generic []));
      (0.5, Gen.return (Spec_splat []));
      (0.5, Gen.return (Spec_union []));
      (0.5, Gen.return (Spec_intersection []));
      (1.0, Gen.return Spec_non_shape);
    ]
    ~f:(fun self ->
      [
        (2.0, Gen.map self ~f:(fun inner -> Spec_option inner));
        (2.0, Gen.map self ~f:(fun bound -> Spec_newtype bound));
        ( 3.0,
          Gen.map (Gen.list_non_empty self) ~f:(fun bounds ->
              Spec_generic bounds) );
        (3.0, Gen.map (Gen.list self) ~f:(fun elements -> Spec_splat elements));
        (3.0, Gen.map (Gen.list self) ~f:(fun members -> Spec_union members));
        ( 3.0,
          Gen.map (Gen.list_non_empty self) ~f:(fun members ->
              Spec_intersection members) );
      ])

let gen_element_specs =
  let module Gen = Quickcheck.Generator in
  Gen.bind (Int.gen_incl 2 6) ~f:(fun length ->
      Gen.list_with_length length gen_element_spec)

let rec shrink_element_spec spec =
  let module Shrinker = Quickcheck.Shrinker in
  let shrink shrinker value ~f =
    Sequence.map (Shrinker.shrink shrinker value) ~f
  in
  let shrink_children constructor children =
    let child_shrinker = Shrinker.create shrink_element_spec in
    Sequence.append
      (Sequence.of_list (Spec_nothing :: children))
      (shrink (List.quickcheck_shrinker child_shrinker) children ~f:constructor)
  in
  match spec with
  | Spec_nothing -> Sequence.empty
  | Spec_closed labels ->
    Sequence.append
      (Sequence.of_list [Spec_nothing])
      (shrink
         (List.quickcheck_shrinker String.quickcheck_shrinker)
         labels
         ~f:(fun labels -> Spec_closed labels))
  | Spec_open { present; absent } ->
    Sequence.append
      (Sequence.of_list [Spec_nothing; Spec_closed present])
      (shrink
         (Shrinker.tuple2
            (List.quickcheck_shrinker String.quickcheck_shrinker)
            (List.quickcheck_shrinker String.quickcheck_shrinker))
         (present, absent)
         ~f:(fun (present, absent) -> Spec_open { present; absent }))
  | Spec_dynamic
  | Spec_type_var
  | Spec_nonnull
  | Spec_non_shape ->
    Sequence.of_list [Spec_nothing]
  | Spec_option inner ->
    Sequence.append
      (Sequence.of_list [Spec_nothing; inner])
      (Sequence.map (shrink_element_spec inner) ~f:(fun inner ->
           Spec_option inner))
  | Spec_generic bounds -> shrink_children (fun xs -> Spec_generic xs) bounds
  | Spec_newtype bound ->
    Sequence.append
      (Sequence.of_list [Spec_nothing; bound])
      (Sequence.map (shrink_element_spec bound) ~f:(fun bound ->
           Spec_newtype bound))
  | Spec_splat elements -> shrink_children (fun xs -> Spec_splat xs) elements
  | Spec_union members -> shrink_children (fun xs -> Spec_union xs) members
  | Spec_intersection members ->
    shrink_children (fun xs -> Spec_intersection xs) members

let element_spec_shrinker = Quickcheck.Shrinker.create shrink_element_spec

let element_specs_shrinker = List.quickcheck_shrinker element_spec_shrinker

let rec mentioned_labels = function
  | Spec_closed labels -> sset labels
  | Spec_open { present; absent } -> sset (present @ absent)
  | Spec_generic bounds
  | Spec_splat bounds
  | Spec_union bounds
  | Spec_intersection bounds ->
    List.fold bounds ~init:TShapeSet.empty ~f:(fun labels spec ->
        TShapeSet.union labels (mentioned_labels spec))
  | Spec_newtype bound
  | Spec_option bound ->
    mentioned_labels bound
  | Spec_nothing
  | Spec_dynamic
  | Spec_type_var
  | Spec_nonnull
  | Spec_non_shape ->
    TShapeSet.empty

let universe_of_specs specs =
  let mentioned =
    List.fold specs ~init:TShapeSet.empty ~f:(fun labels spec ->
        TShapeSet.union labels (mentioned_labels spec))
  in
  let rec add_spare index =
    let spare = key (Printf.sprintf "__spare%d" index) in
    if TShapeSet.mem spare mentioned then
      add_spare (index + 1)
    else
      TShapeSet.add spare mentioned
  in
  add_spare 0

let rec possible_labels universe = function
  | Spec_nothing
  | Spec_non_shape ->
    TShapeSet.empty
  | Spec_closed labels -> sset labels
  | Spec_open { absent; _ } -> TShapeSet.diff universe (sset absent)
  | Spec_dynamic
  | Spec_type_var
  | Spec_nonnull
  | Spec_generic [] ->
    universe
  | Spec_option inner -> possible_labels universe inner
  | Spec_generic (first :: rest) ->
    List.fold
      rest
      ~init:(possible_labels universe first)
      ~f:(fun labels bound ->
        TShapeSet.inter labels (possible_labels universe bound))
  | Spec_newtype bound -> possible_labels universe bound
  | Spec_splat elements
  | Spec_union elements ->
    List.fold elements ~init:TShapeSet.empty ~f:(fun labels element ->
        TShapeSet.union labels (possible_labels universe element))
  | Spec_intersection [] -> universe
  | Spec_intersection (first :: rest) ->
    List.fold
      rest
      ~init:(possible_labels universe first)
      ~f:(fun labels element ->
        TShapeSet.inter labels (possible_labels universe element))

let oracle_reports specs =
  let universe = universe_of_specs specs in
  let rec overlaps = function
    | [] -> false
    | spec :: rest ->
      let possible = possible_labels universe spec in
      List.exists rest ~f:(fun other ->
          not
            (TShapeSet.is_empty
               (TShapeSet.inter possible (possible_labels universe other))))
      || overlaps rest
  in
  overlaps specs

let rec materialize_spec env next = function
  | Spec_nothing -> (env, MakeType.nothing r, next)
  | Spec_closed labels -> (env, closed labels, next)
  | Spec_open { present; absent } -> (env, open_shape ~present ~absent, next)
  | Spec_dynamic -> (env, MakeType.dynamic r, next)
  | Spec_nonnull -> (env, MakeType.nonnull r, next)
  | Spec_option inner ->
    let (env, inner, next) = materialize_spec env next inner in
    (env, MakeType.nullable r inner, next)
  | Spec_type_var ->
    let (env, ty) = Env.fresh_type_reason env Pos.none (fun _ -> Reason.none) in
    (env, ty, next)
  | Spec_generic bounds ->
    let (env, bounds, next) = materialize_spec_list env next bounds in
    let name = Printf.sprintf "TGenerated%d" next in
    let env =
      List.fold bounds ~init:env ~f:(fun env bound ->
          Env.add_upper_bound env name bound)
    in
    (env, mk (r, Tgeneric name), next + 1)
  | Spec_newtype bound ->
    let (env, bound, next) = materialize_spec env next bound in
    let name = Printf.sprintf "\\Generated%d" next in
    (env, mk (r, Tnewtype (name, [], bound)), next + 1)
  | Spec_splat elements ->
    let (env, elements, next) = materialize_spec_list env next elements in
    (env, mk (r, Tshape (Shape_splat { ss_elems = elements })), next)
  | Spec_union members ->
    let (env, members, next) = materialize_spec_list env next members in
    (env, mk (r, Tunion members), next)
  | Spec_intersection members ->
    let (env, members, next) = materialize_spec_list env next members in
    (env, mk (r, Tintersection members), next)
  | Spec_non_shape -> (env, MakeType.int r, next)

and materialize_spec_list env next specs =
  match specs with
  | [] -> (env, [], next)
  | spec :: rest ->
    let (env, ty, next) = materialize_spec env next spec in
    let (env, tys, next) = materialize_spec_list env next rest in
    (env, ty :: tys, next)

let reports specs =
  let (env, tys, _) = materialize_spec_list (dummy_env ()) 0 specs in
  not (List.is_empty (Typing_shape_disjointness.violations tys env))

let quickcheck ~seed ~trials ~f =
  Quickcheck.test
    ~seed:(`Deterministic seed)
    ~trials
    ~shrinker:element_specs_shrinker
    ~sexp_of:sexp_of_element_specs
    gen_element_specs
    ~f

(* Both directions matter: a false negative accepts an overlapping splat, while
   a false positive rejects a provably disjoint one. *)
let prop_generated_elements_match_oracle _ =
  quickcheck ~seed:"shape-splat oracle" ~trials:5000 ~f:(fun specs ->
      let expected = oracle_reports specs in
      let actual = reports specs in
      assert_bool
        (Printf.sprintf
           "%s: elements can share a label = %b, but check reported = %b"
           (show_element_specs specs)
           expected
           actual)
        (Bool.equal expected actual))

let prop_order_independent _ =
  quickcheck ~seed:"shape-splat order" ~trials:2000 ~f:(fun specs ->
      let expected = reports specs in
      let rec adjacent_swaps prefix = function
        | left :: right :: rest ->
          List.rev_append prefix (right :: left :: rest)
          :: adjacent_swaps (left :: prefix) (right :: rest)
        | _ -> []
      in
      List.iter (adjacent_swaps [] specs) ~f:(fun permutation ->
          let actual = reports permutation in
          assert_bool
            (Printf.sprintf
               "%s reported %b but the permutation %s reported %b"
               (show_element_specs specs)
               expected
               (show_element_specs permutation)
               actual)
            (Bool.equal expected actual)))

let prop_monotone _ =
  quickcheck ~seed:"shape-splat monotonicity" ~trials:2000 ~f:(fun specs ->
      let rec check prefix previously_reported = function
        | [] -> ()
        | spec :: rest ->
          let prefix = prefix @ [spec] in
          let reported = reports prefix in
          assert_bool
            (Printf.sprintf
               "%s reported an overlap but appending %s cleared it"
               (show_element_specs (List.drop_last_exn prefix))
               (show_element_spec spec))
            ((not previously_reported) || reported);
          check prefix reported rest
      in
      check [] false specs)

let prop_union_preserves_certainty _ =
  let env = dummy_env () in
  let branch_a_pos = test_pos 10 in
  let branch_b_pos = test_pos 11 in
  let other_a_pos = test_pos 12 in
  let union =
    mk (r, Tunion [closed_at branch_a_pos ["a"]; closed_at branch_b_pos ["b"]])
  in
  begin
    match
      Typing_shape_disjointness.violations
        [union; closed_at other_a_pos ["a"]]
        env
    with
    | [Typing_error.Primary.Shape_splat.Possible_overlapping_field overlap] ->
      assert_equal "a" overlap.label;
      assert_bool
        "the certain side retains its field position"
        (List.equal
           Pos_or_decl.equal
           [Pos_or_decl.of_raw_pos other_a_pos]
           overlap.positions);
      assert_bool
        "a field present in only one union branch is a possible source"
        (match overlap.sources with
        | [{ Typing_error.Primary.Shape_splat.pos; origin = Shape_field "a" }]
          ->
          Pos_or_decl.equal pos (Pos_or_decl.of_raw_pos branch_a_pos)
        | _ -> false)
    | _ -> assert_failure "expected one possible-overlap violation"
  end;
  let left_a_pos = test_pos 13 in
  let right_a_pos = test_pos 14 in
  let other_a_pos = test_pos 15 in
  let union =
    mk
      ( r,
        Tunion
          [
            closed_at left_a_pos ["a"; "left"];
            closed_at right_a_pos ["a"; "right"];
          ] )
  in
  match
    Typing_shape_disjointness.violations
      [union; closed_at other_a_pos ["a"]]
      env
  with
  | [Typing_error.Primary.Shape_splat.Overlapping_field overlap] ->
    assert_equal "a" overlap.label;
    assert_bool
      "a field present in every union branch remains certain"
      (List.equal
         Pos_or_decl.equal
         (List.map
            [left_a_pos; right_a_pos; other_a_pos]
            ~f:Pos_or_decl.of_raw_pos)
         overlap.positions)
  | _ -> assert_failure "expected one overlapping-field violation"

let prop_intersection_meets_possibility _ =
  let env = dummy_env () in
  let unrestricted = open_shape ~present:[] ~absent:[] in
  let restricted = mk (r, Tintersection [closed ["a"]; unrestricted]) in
  assert_bool
    "a closed conjunct rules out fields allowed only by an open conjunct"
    (List.is_empty
       (Typing_shape_disjointness.violations [restricted; closed ["x"]] env));
  let inner_x_pos = test_pos 16 in
  let outer_x_pos = test_pos 17 in
  let overlapping =
    mk (r, Tintersection [closed_at inner_x_pos ["x"]; unrestricted])
  in
  match
    Typing_shape_disjointness.violations
      [overlapping; closed_at outer_x_pos ["x"]]
      env
  with
  | [Typing_error.Primary.Shape_splat.Possible_overlapping_field overlap] ->
    assert_equal "x" overlap.label;
    assert_bool
      "an intersection retains the contributing field's position"
      (List.exists overlap.sources ~f:(fun source ->
           Pos_or_decl.equal source.pos (Pos_or_decl.of_raw_pos inner_x_pos)))
  | _ -> assert_failure "expected one possible-overlap violation"

let prop_generic_upper_bounds_keep_shape_inhabitants _ =
  let generic name = mk (r, Tgeneric name) in
  let add_bounds env name bounds =
    List.fold bounds ~init:env ~f:(fun env bound ->
        Env.add_upper_bound env name bound)
  in
  let shape_bound = open_shape ~present:[] ~absent:[] in
  let env =
    add_bounds
      (dummy_env ())
      "TDict"
      [MakeType.(dict r (arraykey r) (mixed r)); shape_bound]
  in
  assert_bool
    "a dict-compatible upper bound does not hide possible shape fields"
    (not
       (List.is_empty
          (Typing_shape_disjointness.violations
             [generic "TDict"; closed ["x"]]
             env)));
  let env = add_bounds (dummy_env ()) "TInt" [MakeType.int r; shape_bound] in
  assert_bool
    "an upper bound disjoint from shapes makes the generic uninhabited"
    (List.is_empty
       (Typing_shape_disjointness.violations [generic "TInt"; closed ["x"]] env))

let prop_cyclic_generic_bounds_remain_precise _ =
  let env = dummy_env () in
  let generic name = mk (r, Tgeneric name) in
  let env =
    Env.add_upper_bound
      env
      "A"
      (mk (r, Tintersection [generic "B"; closed ["a"]]))
  in
  let env =
    Env.add_upper_bound env "B" (mk (r, Tunion [generic "A"; closed ["b"]]))
  in
  let nested =
    mk (r, Tshape (Shape_splat { ss_elems = [generic "A"; generic "B"] }))
  in
  assert_bool
    "cycle-dependent generic bounds do not introduce an unrelated label"
    (List.is_empty
       (Typing_shape_disjointness.violations [nested; closed ["c"]] env));
  assert_bool
    "cycle-dependent generic bounds retain reachable labels"
    (not
       (List.is_empty
          (Typing_shape_disjointness.violations [nested; closed ["a"; "b"]] env)))

let prop_dense_cyclic_generic_bounds_remain_unknown _ =
  let count = 18 in
  let names =
    List.init count ~f:(fun index -> Printf.sprintf "TDense%d" index)
  in
  let generic name = mk (r, Tgeneric name) in
  let env =
    List.foldi names ~init:(dummy_env ()) ~f:(fun index env name ->
        let other_generics =
          List.filter_mapi names ~f:(fun other_index other_name ->
              if Int.equal index other_index then
                None
              else
                Some (generic other_name))
        in
        let bound =
          mk (r, Tshape (Shape_splat { ss_elems = other_generics }))
        in
        Env.add_upper_bound env name bound)
  in
  assert_bool
    "a dense generic cycle can still provide any field"
    (not
       (List.is_empty
          (Typing_shape_disjointness.violations
             [generic (List.hd_exn names); closed ["x"]]
             env)))

let prop_repeated_sibling_generic_bounds_are_disjoint _ =
  let depth = 128 in
  let name index = Printf.sprintf "TPerf%d" index in
  let rec add_bounds env index =
    if index < 0 then
      env
    else
      let bound =
        if Int.equal index (depth - 1) then
          closed []
        else
          mk (r, Tgeneric (name (index + 1)))
      in
      add_bounds (Env.add_upper_bound env (name index) bound) (index - 1)
  in
  let env = add_bounds (dummy_env ()) (depth - 1) in
  let root = mk (r, Tgeneric (name 0)) in
  let siblings = List.init 5_000 ~f:(fun _ -> root) in
  assert_bool
    "repeated sibling generic bounds remain disjoint"
    (List.is_empty (Typing_shape_disjointness.violations siblings env))

let prop_repeated_newtype_bounds_are_disjoint _ =
  let depth = 24 in
  let rec chain index =
    if Int.equal index depth then
      closed ["a"]
    else
      let next = chain (index + 1) in
      let bound = mk (r, Tshape (Shape_splat { ss_elems = [next; next] })) in
      mk (r, Tnewtype (Printf.sprintf "NPerf%d" index, [], bound))
  in
  let root = chain 0 in
  let env = dummy_env () in
  assert_bool
    "repeated newtype bounds remain disjoint"
    (List.is_empty
       (Typing_shape_disjointness.violations [root; closed ["b"]] env));
  let env = Env.add_upper_bound env "TNewtypePerf" root in
  assert_bool
    "repeated newtype bounds remain disjoint through a type parameter"
    (List.is_empty
       (Typing_shape_disjointness.violations
          [mk (r, Tgeneric "TNewtypePerf"); closed ["b"]]
          env))

let prop_cyclic_newtype_and_ty_param_bounds_remain_precise _ =
  let ty_param = mk (r, Tgeneric "TNewtypeCycle") in
  let newtype = mk (r, Tnewtype ("NRecursive", [], ty_param)) in
  let env =
    Env.add_upper_bound
      (dummy_env ())
      "TNewtypeCycle"
      (mk (r, Tintersection [newtype; closed ["a"]]))
  in
  let nested =
    mk (r, Tshape (Shape_splat { ss_elems = [newtype; ty_param] }))
  in
  assert_bool
    "a newtype/type-parameter cycle retains its concrete label"
    (not
       (List.is_empty
          (Typing_shape_disjointness.violations [nested; closed ["a"]] env)));
  assert_bool
    "a newtype/type-parameter cycle excludes unrelated labels"
    (List.is_empty
       (Typing_shape_disjointness.violations [nested; closed ["c"]] env))

let prop_shared_generic_cache_keeps_use_site_provenance _ =
  let left_pos = test_pos 24 in
  let right_pos = test_pos 25 in
  let generic pos = mk (Reason.witness pos, Tgeneric "TRepeated") in
  let violations =
    Typing_shape_disjointness.violations
      [generic left_pos; generic right_pos]
      (dummy_env ())
  in
  match violations with
  | [
   Typing_error.Primary.Shape_splat.Unresolved_sources
     { sources = [left; right] };
  ] ->
    assert_bool
      "cached bounds retain each generic use-site position"
      (Pos_or_decl.equal left.pos (Pos_or_decl.of_raw_pos left_pos)
      && Pos_or_decl.equal right.pos (Pos_or_decl.of_raw_pos right_pos))
  | _ -> assert_failure "expected both repeated generic source positions"

let prop_shared_newtype_cache_keeps_use_site_provenance _ =
  let left_pos = test_pos 29 in
  let right_pos = test_pos 30 in
  let newtype pos =
    mk
      ( Reason.witness pos,
        Tnewtype ("NRepeated", [], closed_at (test_pos 31) ["a"]) )
  in
  let violations =
    Typing_shape_disjointness.violations
      [newtype left_pos; newtype right_pos]
      (dummy_env ())
  in
  match violations with
  | [
   Typing_error.Primary.Shape_splat.Unresolved_sources
     { sources = [left; right] };
  ] ->
    assert_bool
      "cached newtype bounds retain each use-site position"
      (Pos_or_decl.equal left.pos (Pos_or_decl.of_raw_pos left_pos)
      && Pos_or_decl.equal right.pos (Pos_or_decl.of_raw_pos right_pos))
  | _ -> assert_failure "expected both repeated newtype source positions"

let prop_optional_fields_are_possible _ =
  let env = dummy_env () in
  let optional_pos = test_pos 26 in
  let required_pos = test_pos 27 in
  let other_optional_pos = test_pos 28 in
  let optional = closed_optional_at optional_pos ["x"] in
  begin
    match
      Typing_shape_disjointness.violations
        [optional; closed_at required_pos ["x"]]
        env
    with
    | [Typing_error.Primary.Shape_splat.Possible_overlapping_field overlap] ->
      assert_equal "x" overlap.label;
      assert_bool
        "an optional field retains possible-field provenance"
        (match overlap.sources with
        | [{ Typing_error.Primary.Shape_splat.pos; origin = Shape_field "x" }]
          ->
          Pos_or_decl.equal pos (Pos_or_decl.of_raw_pos optional_pos)
        | _ -> false)
    | _ -> assert_failure "expected a possible optional/required overlap"
  end;
  begin
    match
      Typing_shape_disjointness.violations
        [optional; closed_optional_at other_optional_pos ["x"]]
        env
    with
    | [
     Typing_error.Primary.Shape_splat.Unresolved_sources
       { sources = [left; right] };
    ] ->
      assert_bool
        "two optional fields retain both possible-field positions"
        (Pos_or_decl.equal left.pos (Pos_or_decl.of_raw_pos optional_pos)
        && Pos_or_decl.equal
             right.pos
             (Pos_or_decl.of_raw_pos other_optional_pos))
    | _ -> assert_failure "expected an unresolved optional/optional overlap"
  end;
  let absent = shape_ty ~unknown:(MakeType.nothing r) [("x", absent_fd)] in
  assert_bool
    "an optional nothing field remains disjoint from a required field"
    (List.is_empty
       (Typing_shape_disjointness.violations
          [absent; closed_at required_pos ["x"]]
          env))

let prop_certain_overlap_suppresses_possible_for_same_label _ =
  let env = dummy_env () in
  let dynamic_pos = test_pos 18 in
  let xy_pos = test_pos 19 in
  let x_pos = test_pos 20 in
  let violations =
    Typing_shape_disjointness.violations
      [
        MakeType.dynamic (Reason.witness dynamic_pos);
        closed_at xy_pos ["x"; "y"];
        closed_at x_pos ["x"];
      ]
      env
  in
  match violations with
  | [
   Typing_error.Primary.Shape_splat.Overlapping_field certain;
   Typing_error.Primary.Shape_splat.Possible_overlapping_field possible;
  ] ->
    assert_equal "x" certain.label;
    assert_equal "y" possible.label
  | _ ->
    assert_failure
      (Printf.sprintf
         "expected one certain x and one possible y overlap, got %s"
         (String.concat
            ~sep:", "
            (List.map
               violations
               ~f:Typing_error.Primary.Shape_splat.show_disjointness_violation)))

let prop_violations_keep_provenance _ =
  let env = dummy_env () in
  let raw_positions = List.map [1; 2; 3] ~f:test_pos in
  let positions = List.map raw_positions ~f:Pos_or_decl.of_raw_pos in
  let violations =
    Typing_shape_disjointness.violations
      (List.map raw_positions ~f:(fun pos -> closed_at pos ["x"]))
      env
  in
  begin
    match violations with
    | [Typing_error.Primary.Shape_splat.Overlapping_field overlap] ->
      assert_equal "x" overlap.label;
      assert_bool
        "an overlapping label retains every field position"
        (List.equal Pos_or_decl.equal positions overlap.positions)
    | _ ->
      assert_failure
        (Printf.sprintf
           "expected one overlapping-field violation, got %s"
           (String.concat
              ~sep:", "
              (List.map
                 violations
                 ~f:Typing_error.Primary.Shape_splat.show_disjointness_violation)))
  end;
  let field_pos = test_pos 4 in
  let dynamic_pos = test_pos 5 in
  let violations =
    Typing_shape_disjointness.violations
      [
        MakeType.dynamic (Reason.witness dynamic_pos);
        closed_at field_pos ["possibly_dynamic"];
      ]
      env
  in
  begin
    match violations with
    | [Typing_error.Primary.Shape_splat.Possible_overlapping_field overlap] ->
      assert_bool
        "a possible overlap retains the known field position"
        (List.equal
           Pos_or_decl.equal
           [Pos_or_decl.of_raw_pos field_pos]
           overlap.positions);
      assert_bool
        "a possible overlap retains the uncertain source and its position"
        (List.exists overlap.sources ~f:(fun source ->
             Pos_or_decl.equal
               source.Typing_error.Primary.Shape_splat.pos
               (Pos_or_decl.of_raw_pos dynamic_pos)
             &&
             match source.origin with
             | Typing_error.Primary.Shape_splat.Description description ->
               String.equal description "`dynamic`"
             | Typing_error.Primary.Shape_splat.Shape_field _ -> false))
    | _ -> assert_failure "expected one possible-overlap violation"
  end;
  let left_pos = test_pos 6 in
  let right_pos = test_pos 7 in
  let generic pos name = mk (Reason.witness pos, Tgeneric name) in
  let violations =
    Typing_shape_disjointness.violations
      [generic left_pos "TLeft"; generic right_pos "TRight"]
      env
  in
  begin
    match violations with
    | [
     Typing_error.Primary.Shape_splat.Unresolved_sources
       { sources = [left; right] };
    ] ->
      assert_bool
        "an unresolved overlap retains both source positions"
        (Pos_or_decl.equal left.pos (Pos_or_decl.of_raw_pos left_pos)
        && Pos_or_decl.equal right.pos (Pos_or_decl.of_raw_pos right_pos))
    | _ -> assert_failure "expected one unresolved-sources violation"
  end;
  let generic_positions = List.map [6; 7; 8; 9] ~f:test_pos in
  let violations =
    Typing_shape_disjointness.violations
      (List.mapi generic_positions ~f:(fun index pos ->
           generic pos (Printf.sprintf "T%d" index)))
      env
  in
  match violations with
  | [Typing_error.Primary.Shape_splat.Unresolved_sources { sources }] ->
    assert_bool
      "unresolved sources are grouped and each position is retained once"
      (List.equal
         Pos_or_decl.equal
         (List.map generic_positions ~f:Pos_or_decl.of_raw_pos)
         (List.map sources ~f:(fun source -> source.pos)))
  | _ -> assert_failure "expected one compact unresolved-sources violation"

let () =
  "shapeSplatDisjointnessTest"
  >::: [
         "prop_join_is_union" >:: prop_join_is_union;
         "prop_meet_is_inter" >:: prop_meet_is_inter;
         "prop_disjoint_is_empty_inter" >:: prop_disjoint_is_empty_inter;
         "prop_restrict_is_inter" >:: prop_restrict_is_inter;
         "prop_possible_is_union" >:: prop_possible_is_union;
         "prop_commutative" >:: prop_commutative;
         "prop_associative" >:: prop_associative;
         "prop_generated_elements_match_oracle"
         >:: prop_generated_elements_match_oracle;
         "prop_order_independent" >:: prop_order_independent;
         "prop_monotone" >:: prop_monotone;
         "prop_union_preserves_certainty" >:: prop_union_preserves_certainty;
         "prop_intersection_meets_possibility"
         >:: prop_intersection_meets_possibility;
         "prop_generic_upper_bounds_keep_shape_inhabitants"
         >:: prop_generic_upper_bounds_keep_shape_inhabitants;
         "prop_cyclic_generic_bounds_remain_precise"
         >:: prop_cyclic_generic_bounds_remain_precise;
         "prop_dense_cyclic_generic_bounds_remain_unknown"
         >:: prop_dense_cyclic_generic_bounds_remain_unknown;
         "prop_repeated_sibling_generic_bounds_are_disjoint"
         >:: prop_repeated_sibling_generic_bounds_are_disjoint;
         "prop_repeated_newtype_bounds_are_disjoint"
         >:: prop_repeated_newtype_bounds_are_disjoint;
         "prop_cyclic_newtype_and_ty_param_bounds_remain_precise"
         >:: prop_cyclic_newtype_and_ty_param_bounds_remain_precise;
         "prop_shared_generic_cache_keeps_use_site_provenance"
         >:: prop_shared_generic_cache_keeps_use_site_provenance;
         "prop_shared_newtype_cache_keeps_use_site_provenance"
         >:: prop_shared_newtype_cache_keeps_use_site_provenance;
         "prop_optional_fields_are_possible"
         >:: prop_optional_fields_are_possible;
         "prop_certain_overlap_suppresses_possible_for_same_label"
         >:: prop_certain_overlap_suppresses_possible_for_same_label;
         "prop_violations_keep_provenance" >:: prop_violations_keep_provenance;
       ]
  |> run_test_tt_main
