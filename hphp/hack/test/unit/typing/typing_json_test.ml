(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude

let reason = Typing_reason.none

let ctx =
  let () =
    Provider_backend.set_local_memory_backend_with_defaults_for_test ()
  in
  Provider_context.empty_for_tool
    ~popt:Parser_options.default
    ~tcopt:Typechecker_options.default
    ~backend:(Provider_backend.get ())
    ~deps_mode:(Typing_deps_mode.InMemoryMode None)

let env = Typing_env_types.empty ctx Relative_path.default ~droot:None

let tint = Typing_make_type.int reason

let tbool = Typing_make_type.bool reason

let tnothing = Typing_make_type.nothing reason

let tmixed = Typing_make_type.mixed reason

let shape ~unknown_fields_type fields =
  let open Typing_defs in
  let s_fields =
    List.fold
      fields
      ~init:TShapeMap.empty
      ~f:(fun fields (name, optional, ty) ->
        TShapeMap.add
          (TSFlit_str (Pos_or_decl.none, name))
          { sft_optional = optional; sft_ty = ty }
          fields)
  in
  mk
    ( reason,
      Tshape
        (Shape_simple
           {
             s_origin = Missing_origin;
             s_fields;
             s_unknown_value = unknown_fields_type;
           }) )

let serialize ty = Typing_json.from_locl_ty env ~show_like_ty:true ty

let deserialize json =
  match Typing_json.to_locl_ty ctx json with
  | Ok ty -> ty
  | Error
      ( Typing_defs.Deserialization_error message
      | Typing_defs.Not_supported message
      | Typing_defs.Wrong_phase message ) ->
    OUnit2.assert_failure message

let assert_type_equal expected actual =
  OUnit2.assert_bool
    (Yojson.Safe.to_string (serialize expected))
    (Typing_defs.ty_equal expected actual)

let assert_round_trip ty = assert_type_equal ty (deserialize (serialize ty))

let legacy_shape fields_known =
  `Assoc
    [
      ("kind", `String "shape");
      ("is_array", `Bool false);
      ("fields_known", `Bool fields_known);
      ("fields", `List []);
    ]

let legacy_shapes _ =
  List.iter
    [(true, tnothing); (false, tmixed)]
    ~f:(fun (fields_known, unknown_fields_type) ->
      let ty = shape ~unknown_fields_type [] in
      assert_type_equal ty (deserialize (legacy_shape fields_known));
      assert_round_trip ty;
      match serialize ty with
      | `Assoc fields ->
        OUnit2.assert_equal
          (`Bool fields_known)
          (Yojson.Safe.Util.member "fields_known" (`Assoc fields));
        OUnit2.assert_bool
          "Legacy shapes retain their existing JSON fields"
          (not
             (List.exists fields ~f:(fun (name, _) ->
                  String.equal name "unknown_fields_type")))
      | _ -> OUnit2.assert_failure "Expected an object")

let unknown_fields_round_trip _ =
  let bounds =
    [
      tint;
      tbool;
      Typing_make_type.string reason;
      Typing_make_type.dynamic reason;
      Typing_defs.(mk (reason, Tgeneric "TRowValue"));
      Typing_defs.(mk (reason, Toption tint));
      Typing_defs.(mk (reason, Tunion [tint; tbool]));
    ]
  in
  List.iter bounds ~f:(fun unknown_fields_type ->
      let ty =
        shape
          ~unknown_fields_type
          [
            ("required", false, tint);
            ("optional", true, tbool);
            ("absent", true, tnothing);
          ]
      in
      assert_round_trip ty;
      OUnit2.assert_equal
        (serialize unknown_fields_type)
        (Yojson.Safe.Util.member "unknown_fields_type" (serialize ty)))

let null_unknown_fields_round_trip _ =
  shape ~unknown_fields_type:(Typing_make_type.null reason) []
  |> assert_round_trip

let assert_invalid ~path json =
  match Typing_json.to_locl_ty ctx json with
  | Error (Typing_defs.Deserialization_error message) ->
    OUnit2.assert_bool message (String.is_substring message ~substring:path)
  | Error (Typing_defs.Not_supported message) -> OUnit2.assert_failure message
  | Error (Typing_defs.Wrong_phase message) -> OUnit2.assert_failure message
  | Ok _ -> OUnit2.assert_failure "Expected a deserialization error"

let invalid_unknown_fields_inputs _ =
  List.iter
    [`String "invalid"; `Assoc []; `Assoc [("kind", `String "unsupported")]]
    ~f:(fun unknown_fields_type ->
      let json =
        match legacy_shape false with
        | `Assoc fields ->
          `Assoc (("unknown_fields_type", unknown_fields_type) :: fields)
        | _ -> OUnit2.assert_failure "Expected an object"
      in
      assert_invalid ~path:"unknown_fields_type" json)

let () =
  OUnit2.(
    "typing_json"
    >::: [
           "legacy_shapes" >:: legacy_shapes;
           "unknown_fields_round_trip" >:: unknown_fields_round_trip;
           "null_unknown_fields_round_trip" >:: null_unknown_fields_round_trip;
           "invalid_unknown_fields_inputs" >:: invalid_unknown_fields_inputs;
         ]
    |> run_test_tt_main)
