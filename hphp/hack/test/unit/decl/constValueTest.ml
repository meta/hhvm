(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *)

(* Check Const_value.concrete_value after parsing, including label expansion and
   class-name normalization. Declaration snapshots only check stored values. *)

open Hh_prelude
open OUnit2

let parser_options =
  Decl_parser_options.from_parser_options
    Parser_options.{ default with include_const_shape_key_values = true }

let parse_value_source ~parser_options source =
  let { Direct_decl_parser.pf_decls; pf_has_first_pass_parse_errors; _ } =
    Direct_decl_parser.parse_decls parser_options Relative_path.default source
  in
  assert_equal ~printer:string_of_bool false pf_has_first_pass_parse_errors;
  match pf_decls with
  | [(_, Shallow_decl_defs.Class cls)] ->
    let const =
      List.find_exn cls.Shallow_decl_defs.sc_consts ~f:(fun const ->
          String.equal (snd const.Shallow_decl_defs.scc_name) "KEY")
    in
    Typing_defs.Const_value.concrete_value
      ~member_name:"KEY"
      const.Shallow_decl_defs.scc_const_value
  | _ -> assert_failure "Expected one classish declaration"

let parse_value
    ?(namespace = "ShapeKeys")
    ?(parser_options = parser_options)
    ?(class_name = "Example")
    classish
    expression =
  let source =
    Printf.sprintf
      {|<?hh
namespace %s {
use Other\Target as Alias;
%s %s {
  <<__ShapeKey>> const arraykey KEY = %s;
}
}
|}
      namespace
      classish
      class_name
      expression
  in
  parse_value_source ~parser_options source

let show_value value =
  let open Typing_defs.Const_value in
  match value with
  | Some (SKVInt value) -> "int " ^ value
  | Some (SKVString value) -> Printf.sprintf "string %S" value
  | None -> "None"

(* Each case is (test name, declaration keyword, initializer, normalized value). *)
let cases =
  let open Typing_defs.Const_value in
  [
    ("integer", "class", "0x2a", Some (SKVInt "42"));
    ("integer_decimal", "class", "42", Some (SKVInt "42"));
    ("negative_integer", "class", "-1", Some (SKVInt "-1"));
    ("unary_plus_integer", "class", "+1", Some (SKVInt "1"));
    ( "ocaml_max_integer_decimal",
      "class",
      "4611686018427387903",
      Some (SKVInt "4611686018427387903") );
    ( "ocaml_max_integer_hex",
      "class",
      "0x3fffffffffffffff",
      Some (SKVInt "4611686018427387903") );
    ( "first_large_integer_decimal",
      "class",
      "4611686018427387904",
      Some (SKVInt "4611686018427387904") );
    ( "first_large_integer_hex",
      "class",
      "0x4000000000000000",
      Some (SKVInt "4611686018427387904") );
    ( "minimum_int64",
      "class",
      "-9223372036854775808",
      Some (SKVInt "-9223372036854775808") );
    ( "large_integer",
      "class",
      "0x7fffffffffffffff",
      Some (SKVInt "9223372036854775807") );
    ("string", "class", "'text'", Some (SKVString "text"));
    ("member_name_string", "class", "'KEY'", Some (SKVString "KEY"));
    ("numeric_string", "class", "'42'", Some (SKVString "42"));
    ("literal_xhp_name", "class", "':foo:bar'", Some (SKVString ":foo:bar"));
    ( "nameof_self",
      "class",
      "nameof self",
      Some (SKVString "ShapeKeys\\Example") );
    ("nameof_alias", "class", "nameof Alias", Some (SKVString "Other\\Target"));
    ( "literal_leading_backslash",
      "class",
      {|'\Other\Target'|},
      Some (SKVString "\\Other\\Target") );
    ("class_pointer", "class", "Alias::class", None);
    ("self_class_pointer", "class", "self::class", None);
    ("trait_class_pointer", "trait", "Alias::class", None);
    ( "interface_member_name_string",
      "interface",
      "'KEY'",
      Some (SKVString "KEY") );
    ("interface_class_pointer", "interface", "Alias::class", None);
    ("interface_self_class_pointer", "interface", "self::class", None);
    ( "interface_nameof_self",
      "interface",
      "nameof self",
      Some (SKVString "ShapeKeys\\Example") );
    ("constant_reference", "class", "Alias::KEY", None);
    ("computed_value", "class", "1 + 2", None);
    ("trait_nameof_self", "trait", "nameof self", None);
  ]

let xhp_cases =
  [
    ("global_xhp_nameof", "", false, "nameof :foo:bar", Some "xhp_foo__bar");
    ("global_xhp_class_pointer", "", false, ":foo:bar::class", None);
    ( "namespaced_xhp_nameof",
      "ShapeKeys",
      false,
      "nameof :foo:bar",
      Some "ShapeKeys\\xhp_foo__bar" );
    ("namespaced_xhp_class_pointer", "ShapeKeys", false, ":foo:bar::class", None);
    ("xhp_hyphen", "", false, ":foo-bar::class", None);
    ("unmangled_xhp_nameof", "", true, "nameof :foo:bar", Some "foo\\bar");
    ("unmangled_xhp_class_pointer", "", true, ":foo:bar::class", None);
  ]

let () =
  "constValueTest"
  >::: List.map cases ~f:(fun (name, classish, expression, expected) ->
           name >:: fun _ ->
           assert_equal
             ~printer:show_value
             expected
             (parse_value classish expression))
       @ List.map
           xhp_cases
           ~f:(fun
                ( name,
                  namespace,
                  disable_xhp_element_mangling,
                  expression,
                  expected )
              ->
             name >:: fun _ ->
             let parser_options =
               {
                 parser_options with
                 Decl_parser_options.disable_xhp_element_mangling;
               }
             in
             assert_equal
               ~printer:show_value
               (Option.map expected ~f:(fun value ->
                    Typing_defs.Const_value.SKVString value))
               (parse_value ~namespace ~parser_options "class" expression))
       @ List.concat_map
           [
             ("global_xhp_self", "", false, ":x:y", "xhp_x__y");
             ( "namespaced_xhp_self",
               "ShapeKeys",
               false,
               ":x:y",
               "ShapeKeys\\xhp_x__y" );
             ("xhp_self_hyphen", "", false, ":x-y:z", "xhp_x_y__z");
             ("unmangled_xhp_self", "", true, ":x:y", "x\\y");
           ]
           ~f:
             (fun ( name,
                    namespace,
                    disable_xhp_element_mangling,
                    class_name,
                    expected ) ->
             List.map
               ["nameof self"; "nameof " ^ class_name]
               ~f:(fun expression ->
                 name ^ ":" ^ expression >:: fun _ ->
                 let parser_options =
                   {
                     parser_options with
                     Decl_parser_options.disable_xhp_element_mangling;
                   }
                 in
                 assert_equal
                   ~printer:show_value
                   (Some (Typing_defs.Const_value.SKVString expected))
                   (parse_value
                      ~namespace
                      ~parser_options
                      ~class_name
                      "class"
                      expression)))
       @ [
           ( "enum_xhp_nameof" >:: fun _ ->
             let parser_options =
               {
                 parser_options with
                 Decl_parser_options.include_enum_member_values = true;
               }
             in
             assert_equal
               ~printer:show_value
               (Some (Typing_defs.Const_value.SKVString "xhp_foo__bar"))
               (parse_value_source
                  ~parser_options
                  "<?hh enum Example: string { KEY = nameof :foo:bar; }") );
         ]
  |> run_test_tt_main
