(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *)

(* Symptom: Diagnostics in files using the previous definition are not refreshed
   when a case-insensitive naming conflict changes which definition is visible.

   Cause: Fanout compares cached declarations by exact name. Declaration caching
   retains names absent from the naming table, including the losing spelling.
   Comparing unchanged declarations under both spellings then misses the change
   in which name consumers can resolve. *)

(** FOO takes over Foo's binding on a naming retry, so Foo's consumer must
    be rechecked. The previous definition wins initially; A.php sorts before
    F.php and wins when both conflicting files are reparsed. *)
let test_case_variant_takes_over_binding () =
  let scenario =
    Scheduled_fanout.
      {
        (* 1. Foo is bound and used without errors. *)
        initial_files =
          [
            ("F.php", "<?hh\nclass Foo {}\n");
            ("X.php", "<?hh\nfunction unrelated(): void {}\n");
            ("U.php", {|<?hh
function u(): void {
  new Foo();
}
|});
          ];
        iterations =
          [
            {
              default_iteration with
              (* 2. Add FOO. It conflicts with Foo, which keeps its binding
                 for now. Both files are recorded as having failed naming. *)
              before_type_check = [Write_file ("A.php", "<?hh\nclass FOO {}\n")];
              disk_needs_parsing = ["A.php"];
              type_check = [Remaining];
            };
            {
              default_iteration with
              (* 3. An unrelated edit starts the next typecheck, which also
                 reparses the failed-naming files. Naming now binds FOO and
                 leaves Foo unbound. Both exact-name declarations remain
                 cached, so their diffs are empty and U.php is not rechecked. *)
              before_type_check =
                [
                  Write_file
                    ("X.php", "<?hh\nfunction unrelated(): void {}\n// edit\n");
                ];
              disk_needs_parsing = ["X.php"];
              type_check = [Remaining];
            };
          ];
      }
  in
  (* 4. A fresh check reports that U.php cannot find Foo. The incremental
     check does not. *)
  Scheduled_fanout.run_expect_bug
    ~bug:"F16: case-variant naming winner changes"
    ~expectation:
      (Diagnostics
         {
           incremental = [("F.php", 2012, "Name already bound: `Foo`")];
           full =
             [
               ("U.php", 2006, "Could not find `Foo`.");
               ("F.php", 2012, "Name already bound: `Foo`");
             ];
         })
    scenario;
  true

let () =
  Daemon.check_entry_point ();
  Unit_test.run_all
    [("case_variant_takes_over_binding", test_case_variant_takes_over_binding)]
