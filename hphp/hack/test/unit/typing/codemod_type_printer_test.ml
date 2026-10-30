(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)
open Hh_prelude

let reason = Typing_reason.none

let tint = Typing_make_type.int reason

let tbool = Typing_make_type.bool reason

let shape ~unknown fields =
  let open Typing_defs in
  let fields =
    List.fold fields ~init:TShapeMap.empty ~f:(fun fields (name, ty) ->
        TShapeMap.add
          (TSFlit_str (Pos_or_decl.none, name))
          { sft_optional = false; sft_ty = ty }
          fields)
  in
  Typing_make_type.shape reason unknown fields

let assert_print expected ty =
  OUnit2.assert_equal
    ~printer:(Option.value ~default:"<non-denotable>")
    (Some expected)
    (Codemod_type_printer.print ty)

let closed_and_bare_open _ =
  let closed = Typing_make_type.nothing reason in
  let mixed = Typing_make_type.mixed reason in
  assert_print "shape()" (shape ~unknown:closed []);
  assert_print
    "shape('field' => bool)"
    (shape ~unknown:closed [("field", tbool)]);
  assert_print "shape(...)" (shape ~unknown:mixed []);
  assert_print
    "shape('field' => bool, ...)"
    (shape ~unknown:mixed [("field", tbool)]);
  assert_print
    "shape('field' => bool, ...)"
    (shape
       ~unknown:Typing_defs.(mk (reason, Tintersection []))
       [("field", tbool)])

let typed_tails _ =
  let cases =
    [
      (tint, "shape(int...)", "shape('field' => bool, int...)");
      ( Typing_make_type.string reason,
        "shape(\\HH\\string...)",
        "shape('field' => bool, \\HH\\string...)" );
      ( Typing_defs.(mk (reason, Tgeneric "Value")),
        "shape(Value...)",
        "shape('field' => bool, Value...)" );
      ( Typing_make_type.class_type reason "\\Example\\Tail" [],
        "shape(\\Example\\Tail...)",
        "shape('field' => bool, \\Example\\Tail...)" );
      ( Typing_make_type.nullable reason tint,
        "shape(?int...)",
        "shape('field' => bool, ?int...)" );
      ( Typing_make_type.dynamic reason,
        "shape(dynamic...)",
        "shape('field' => bool, dynamic...)" );
    ]
  in
  List.iter cases ~f:(fun (unknown, empty, with_field) ->
      assert_print empty (shape ~unknown []);
      assert_print with_field (shape ~unknown [("field", tbool)]))

let nested_tails _ =
  let inner = shape ~unknown:tint [("field", tbool)] in
  assert_print
    "shape('nested' => shape('field' => bool, int...))"
    (shape ~unknown:(Typing_make_type.nothing reason) [("nested", inner)]);
  let splat =
    Typing_defs.(
      mk
        ( reason,
          Tshape
            (Shape_splat { ss_elems = [mk (reason, Tgeneric "Row"); inner] }) ))
  in
  assert_print "shape(...Row, ...shape('field' => bool, int...))" splat

let non_denotable_tails _ =
  List.iter
    Typing_defs.
      [
        mk (reason, Tunion [tint; tbool]);
        mk (reason, Tintersection [tint; tbool]);
      ]
    ~f:(fun unknown ->
      OUnit2.assert_equal
        None
        (Codemod_type_printer.print (shape ~unknown [("field", tbool)])))

let () =
  OUnit2.(
    "codemod_type_printer"
    >::: [
           "closed_and_bare_open" >:: closed_and_bare_open;
           "typed_tails" >:: typed_tails;
           "nested_tails" >:: nested_tails;
           "non_denotable_tails" >:: non_denotable_tails;
         ]
    |> run_test_tt_main)
