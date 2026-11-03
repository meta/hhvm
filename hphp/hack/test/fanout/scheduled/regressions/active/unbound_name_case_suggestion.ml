(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *)

(* Diagnostics depend on case-insensitive name lookup, but the dependency
   graph does not track changes to that lookup. Unbound_name_check records a
   dependency on the exact missing name; its suggestion lookup records none.
   A new spelling can therefore change the diagnostic without selecting the
   consumer for rechecking. *)

(** Adding FOO must replace U's generic unbound-name diagnostic with a
    suggestion for Foo. Both the error code and message change.
    U's original error has no reference to another file that could cause an
    independent recheck when FOO is added. *)
let test_case_variant_class_added () =
  let scenario =
    Scheduled_fanout.
      {
        (* 1. U.php refers to an undefined class Foo. *)
        initial_files =
          [("U.php", {|<?hh
function u(): void {
  new Foo();
}
|})];
        iterations =
          [
            {
              default_iteration with
              (* 2. Add FOO. Its fanout only includes dependents of FOO, while
                 U.php depends on Foo. *)
              before_type_check = [Write_file ("B.php", "<?hh\nclass FOO {}\n")];
              disk_needs_parsing = ["B.php"];
              type_check = [Remaining];
            };
          ];
      }
  in
  (* 3. A fresh check suggests FOO in U's error. The incremental check keeps
     the error without the suggestion. *)
  Scheduled_fanout.run_expect_bug
    ~bug:"F15: unbound-name case suggestions"
    ~incremental:[("U.php", 2049, "Unbound name: `Foo` (an object type)")]
    ~full:[("U.php", 2006, "Could not find `Foo`.")]
    scenario;
  true

let () =
  Daemon.check_entry_point ();
  Unit_test.run_all
    [("case_variant_class_added", test_case_variant_class_added)]
