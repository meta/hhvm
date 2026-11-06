(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)
open Hh_prelude

let empty_errors : Save_state_service_types.saved_state_errors =
  Relative_path.Set.empty

let test_fixture filename () =
  let expected_filename =
    Stdlib.Filename.temp_file "empty_saved_state_errors" ".err"
  in
  Fun.protect
    ~finally:(fun () -> Sys.remove expected_filename)
    (fun () ->
      Save_state_service.save_contents expected_filename empty_errors;
      if
        not
          (String.equal
             (Real_disk.cat filename)
             (Real_disk.cat expected_filename))
      then
        false
      else
        let errors : Save_state_service_types.saved_state_errors =
          Save_state_service.load_contents_unsafe filename
        in
        Relative_path.Set.is_empty errors)

let () =
  match Sys.argv with
  | [| _; "--write"; filename |] ->
    Save_state_service.save_contents filename empty_errors
  | [| _; filename |] ->
    Unit_test.run_all
      [
        ( "perf errors fixture matches the current empty saved state",
          test_fixture filename );
      ]
  | _ -> failwith "Usage: empty_saved_state_errors_tests [--write] <filename>"
