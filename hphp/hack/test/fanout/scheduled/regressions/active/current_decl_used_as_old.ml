(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *)

(* This bug treats cached declarations as "old" even when lazy reads have
   already loaded an unprocessed disk edit. Oldifying those declarations makes
   both sides of the comparison use the new signature, so dependents are not
   rechecked. *)

let int_definition = {|<?hh
function f(): int {
  return 1;
}
|}

let string_definition = {|<?hh
function f(): string {
  return 'value';
}
|}

let caller_contents = {|<?hh
function h(): void {
  f();
}
|}

(** Checking C loads an unprocessed change to f. Processing that change must
    still recheck B, whose result reflects f's previous signature. *)
let test_signature_change_hidden_by_cached_decl () =
  let scenario =
    Scheduled_fanout.
      {
        (* 1. Save and load a clean baseline where f returns int. The saved
           dependency graph records B's dependency on f, but declarations stay
           cold in the newly initialized server process. *)
        initial_files =
          [
            ("A.php", int_definition);
            ("B.php", {|<?hh
function g(): int {
  return f();
}
|});
            ("C.php", caller_contents);
          ];
        iterations =
          [
            (* First iteration: C observes A's edit before the server has
               processed A's change notification. *)
            {
              (* 2. Queue a comment-only edit to C. A and B still match the
                 saved state when this iteration begins. *)
              before_type_check =
                [
                  Write_file
                    ( "C.php",
                      caller_contents ^ "// Trigger an incremental check.\n" );
                ];
              disk_needs_parsing = ["C.php"];
              (* 3. Change f's signature after indexing C, before C's first
                 lookup of f. Only disk changes: A is not queued for parsing
                 until the next iteration, and naming still reflects old A. *)
              after_indexing = [Write_file ("A.php", string_definition)];
              type_check =
                [
                  (* 4. Checking C lazily reads A and caches f(): string.
                     C accepts either return type; B has not been rechecked. *)
                  Check { file = "C.php"; after_check = [] };
                  Remaining;
                ];
            };
            (* Second iteration: deliver the previously unprocessed A edit. *)
            {
              default_iteration with
              (* 5. Index A and redeclare f. The buggy comparison oldifies the
                 cached f(): string, then compares it with f(): string from
                 disk. It misses the int-to-string change and B's fanout. *)
              disk_needs_parsing = ["A.php"];
              type_check = [Remaining];
            };
          ];
      }
  in
  (* 6. A fresh full check reports B's incompatible return type. The bug leaves
     incremental diagnostics empty. Require that documented difference. *)
  Scheduled_fanout.run_expect_bug
    ~bug:"T290084171: current declarations used as old"
    ~incremental:[]
    ~full:[("B.php", 4110, "Invalid return type")]
    ~initialization:Saved_state
    scenario;
  true

let () =
  Daemon.check_entry_point ();
  Unit_test.run_all
    [
      ( "signature_change_hidden_by_cached_decl",
        test_signature_change_hidden_by_cached_decl );
    ]
