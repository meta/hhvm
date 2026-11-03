(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *)

(* Declaration comparison and folding select different occurrences of
   duplicate members. Shallow_class_diff keeps the first occurrence, while
   folding takes method/property visibility and constant/type-constant types
   from the last. Changing that effective member can be invisible to fanout
   comparison even though the declaration used by consumers changes.
   Duplicate members produce parse errors, but their declarations remain usable.

   The two cases use class constants: one changes the last duplicate's type,
   and the other removes it. Removal leaves valid declarations whose consumers
   still need rechecking. *)

(** Changing the second constant to string should produce a return-type error
    in its consumer, but the incremental check misses that error. *)
let test_second_constant_type_changed () =
  let scenario =
    Scheduled_fanout.
      {
        (* 1. C declares K twice; U returns it as int. *)
        initial_files =
          [
            ( "C.php",
              "<?hh\nclass C {\n  const int K = 1;\n  const int K = 2;\n}\n" );
            ("U.php", "<?hh\nfunction u(): int { return C::K; }\n");
          ];
        iterations =
          [
            {
              default_iteration with
              (* 2. Change the second declaration's type. The first is unchanged,
                 so the member diff is empty and U.php is not rechecked. *)
              before_type_check =
                [
                  Write_file
                    ( "C.php",
                      "<?hh\nclass C {\n  const int K = 1;\n  const string K = '';\n}\n"
                    );
                ];
              disk_needs_parsing = ["C.php"];
              type_check = [Remaining];
            };
          ];
      }
  in
  Scheduled_fanout.run_expect_bug
    ~bug:"F18: duplicate member declarations"
    ~incremental:
      [
        ("C.php", 1002, "Cannot redeclare `C::K`");
        ("C.php", 1002, "Cannot redeclare `C::K`");
      ]
    ~full:
      [
        ("C.php", 1002, "Cannot redeclare `C::K`");
        ("C.php", 1002, "Cannot redeclare `C::K`");
        ("U.php", 4110, "Invalid return type");
      ]
    scenario;
  (* 3. A fresh check reports U's invalid return type. The incremental check
     does not recheck U.php. With a single declaration, changing its type
     triggers fanout and the error is reported. *)
  true

(** Removing the string constant leaves valid declarations, but the incremental
    check misses the new return-type error in the consumer. An idle iteration
    checks that the discrepancy persists after all changes have been processed. *)
let test_constant_last_removed () =
  let scenario =
    Scheduled_fanout.
      {
        (* 1. C has duplicate constants and therefore a parse error. U sees the
           last declaration's string type and has no errors. *)
        initial_files =
          [
            ( "C.php",
              "<?hh\nclass C {\n  const int K = 1;\n  const string K = 'value';\n}\n"
            );
            ("U.php", "<?hh\nfunction u(): string {\n  return C::K;\n}\n");
          ];
        iterations =
          [
            {
              default_iteration with
              (* 2. Remove the last declaration. The first is unchanged, so the
                 member diff is empty and U.php is not rechecked. *)
              before_type_check =
                [
                  Write_file
                    ("C.php", "<?hh\nclass C {\n  const int K = 1;\n}\n");
                ];
              disk_needs_parsing = ["C.php"];
              type_check = [Remaining];
            };
            { default_iteration with type_check = [Remaining] };
          ];
      }
  in
  Scheduled_fanout.run_expect_bug
    ~bug:"F18: duplicate member removal"
    ~incremental:[]
    ~full:[("U.php", 4110, "Invalid return type")]
    scenario;
  (* 3. A fresh check reports that U returns an int where a string is
     required. The incremental check reports no diagnostics. Explicitly
     rechecking U.php reports the error: the cached declaration is correct,
     but fanout did not select the consumer. *)
  true

let () =
  Daemon.check_entry_point ();
  Unit_test.run_all
    [
      ("second_constant_type_changed", test_second_constant_type_changed);
      ("constant_last_removed", test_constant_last_removed);
    ]
