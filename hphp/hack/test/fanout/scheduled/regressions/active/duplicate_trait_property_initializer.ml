(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *)

(* The duplicate-property check depends on initializer expressions in other
   files, but those dependencies are not covered by declaration-based fanout.
   Initializer expressions are absent from shallow declarations, so changing an
   expression can leave the classes whose checks depend on it unchecked.

   The check examines whether an initializer refers to a class or enum constant.
   Changing this expression can therefore change a warning in another file,
   even when the property's shallow declaration remains unchanged. *)

(** C inherits T0's property through two traits. The same-origin diamond is
    initially allowed, but changing the initializer to a class constant should
    produce a warning on C without changing T0's shallow declaration. *)
let test_initializer_becomes_class_constant () =
  let scenario =
    Scheduled_fanout.
      {
        (* 1. C inherits $x twice from the same origin. A literal initializer
           is allowed, so there are no diagnostics. *)
        initial_files =
          [
            ("K.php", "<?hh\nclass K { const int V = 1; }\n");
            ("T0.php", "<?hh\ntrait T0 { public int $x = 1; }\n");
            ("T1.php", "<?hh\ntrait T1 { use T0; }\n");
            ("T2.php", "<?hh\ntrait T2 { use T0; }\n");
            ("C.php", "<?hh\nclass C { use T1; use T2; }\n");
          ];
        iterations =
          [
            {
              default_iteration with
              (* 2. Initialize $x with K::V instead. T0's shallow declaration,
                 and hence its diff, is unchanged. *)
              before_type_check =
                [
                  Write_file
                    ("T0.php", "<?hh\ntrait T0 { public int $x = K::V; }\n");
                ];
              disk_needs_parsing = ["T0.php"];
              type_check = [Remaining];
            };
          ];
      }
  in
  (* 3. A fresh check warns about C's duplicate property initialized with a
     constant. The incremental check does not recheck C.php. *)
  Scheduled_fanout.run_expect_bug
    ~bug:"F14: duplicate trait property initializers"
    ~incremental:[]
    ~full:
      [
        ( "C.php",
          12008,
          "Property `x`, defined in T0, is inherited multiple times by class `C` and one of its instances is initialised with a class or enum constant"
        );
      ]
    scenario;
  true

let () =
  Daemon.check_entry_point ();
  Unit_test.run_all
    [
      ( "initializer_becomes_class_constant",
        test_initializer_becomes_class_constant );
    ]
