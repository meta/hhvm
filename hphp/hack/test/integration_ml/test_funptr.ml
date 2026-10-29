(*
 * Copyright (c) 2016, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *
 *)

open Hh_prelude

let source =
  "<?hh // strict

// foo_docblock
function foo(string $s) : string {
  return $s;
}

// Cardoor_docblock
class Cardoor {
  // bar_docblock
  public static function bar<T>(T $x) : T {
    return $x;
  }
}

function test() : void {
  $_ = foo<>;
  //     ^ 17:9

  $_ = Cardoor::bar<string>;
  //     ^ 20:9  ^ 20:18
}
"

let go_identify expected ~ctx ~entry pos =
  let actual =
    Server_identify_function.go_quarantined_absolute ~ctx ~entry pos
    |> Nuclide_rpc_message_printer.identify_symbol_response_to_json
  in
  let (line, column) = File_content.Position.line_column_one_based pos in
  Asserter.Hh_json_json_asserter.assert_equals
    (expected |> Yojson.Safe.from_string)
    actual
    (Printf.sprintf "ServerIdentifyFunction at line %d, column %d" line column);
  ()

let go_hover expected ~ctx ~entry pos =
  let hovers_to_string h =
    List.map h ~f:Hover_service.string_of_result |> String.concat ~sep:"; "
  in
  let actual = Ide_hover.go_quarantined ~ctx ~entry pos in
  let (line, column) = File_content.Position.line_column_one_based pos in
  Asserter.String_asserter.assert_equals
    (expected |> hovers_to_string)
    (actual |> hovers_to_string)
    (Printf.sprintf "Ide_hover at line %d, column %d" line column);
  ()

let pos_at (line1, column1) (line2, column2) =
  Some
    (Pos.make_from_lnum_bol_offset
       ~pos_file:Relative_path.default
       ~pos_start:(line1, 0, column1 - 1)
       ~pos_end:(line2, 0, column2))

let make_hack_marked_code s = [Lsp.MarkedCode ("hack", s)]

let identify_tests =
  [
    ( (17, 9),
      go_identify
        {| [{
      "name":"\\foo",
      "result_type":"function",
      "pos":{"filename":"/source.php","line":17,"char_start":8,"char_end":10},
      "definition_pos":{"filename":"/source.php","line":4,"char_start":10,"char_end":12},
      "definition_span":{"filename":"/source.php","line_start":4,"char_start":1,"line_end":6,"char_end":1},
      "definition_id":"function::foo"}] |}
    );
    ( (17, 9),
      go_hover
        [
          {
            Hover_service.snippet =
              make_hack_marked_code "function foo(string $s): string";
            addendum = [Lsp.MarkedString "foo_docblock"];
            pos = pos_at (17, 8) (17, 10);
          };
        ] );
    ( (20, 9),
      go_identify
        {| [{
      "name":"\\Cardoor",
      "result_type":"class",
      "pos":{"filename":"/source.php","line":20,"char_start":8,"char_end":14},
      "definition_pos":{"filename":"/source.php","line":9,"char_start":7,"char_end":13},
      "definition_span":{"filename":"/source.php","line_start":9,"char_start":1,"line_end":14,"char_end":1},
      "definition_id":"type_id::Cardoor"}] |}
    );
    ( (20, 9),
      go_hover
        [
          {
            Hover_service.snippet = [Lsp.MarkedCode ("hack", "class Cardoor")];
            addendum = [Lsp.MarkedString "Cardoor_docblock"];
            pos = pos_at (20, 8) (20, 14);
          };
        ] );
    ( (20, 18),
      go_identify
        {| [{
      "name":"\\Cardoor::bar",
      "result_type":"method",
      "pos":{"filename":"/source.php","line":20,"char_start":17,"char_end":19},
      "definition_pos":{"filename":"/source.php","line":11,"char_start":26,"char_end":28},
      "definition_span":{"filename":"/source.php","line_start":11,"char_start":3,"line_end":13,"char_end":3},
      "definition_id":"method::Cardoor::bar"}] |}
    );
    ( (20, 18),
      go_hover
        [
          {
            Hover_service.snippet =
              [
                Lsp.MarkedString "Defined in `Cardoor`";
                Lsp.MarkedString "---";
                Lsp.MarkedCode ("hack", "public static function bar<T>(T $x): T");
                Lsp.MarkedString "---";
                Lsp.MarkedString "Instantiation:";
                Lsp.MarkedCode ("hack", "  T = string;");
              ];
            addendum = [Lsp.MarkedString "bar_docblock"];
            pos = pos_at (20, 17) (20, 19);
          };
        ] );
  ]

let test () =
  let root = "/" in
  let hhconfig_filename = Filename.concat root ".hhconfig" in
  let hhconfig_contents = "" in
  let files = [("source.php", source)] in

  Relative_path.set_path_prefix Relative_path.Root (Path.make root);
  Test_disk.set hhconfig_filename hhconfig_contents;
  let (custom_config, _) =
    Server_config.load ~silent:false ~from:"" ~cli_config_overrides:[]
  in
  let env =
    Integration_test_base.setup_server
      ()
      ~custom_config
      ~hhi_files:(Hhi.get_raw_hhi_contents () |> Array.to_list)
  in
  let env = Integration_test_base.setup_disk env files in
  Integration_test_base.assert_no_diagnostics env;

  let path = Relative_path.from_root ~suffix:"source.php" in
  let (ctx, entry) =
    Provider_context.add_entry_if_missing
      ~ctx:(Provider_utils.ctx_from_server_env env)
      ~path
  in

  List.iter identify_tests ~f:(fun ((line, column), go) ->
      let pos = File_content.Position.from_one_based line column in
      Provider_utils.respect_but_quarantine_unsaved_changes ~ctx ~f:(fun () ->
          go ~ctx ~entry pos);
      ());
  ()
