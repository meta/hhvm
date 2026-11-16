(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *)

(* Symptom: Saved-state startup can leave errors missing or out of date after
   changed files have been indexed and their notifications consumed.

   Cause: Naming incorporates notifier changes received during loading, but
   fanout uses only the earlier dirty-file sets. Files present only in the
   notifier changes therefore do not schedule their dependents. *)

let initial_class_contents =
  {|<?hh
class C {
  public static function value(): int { return 1; }
}
|}

let changed_class_contents =
  {|<?hh
class C {
  public static function value(): string { return 'changed'; }
}
|}

(** Change C::value after the loader takes its dirty-file snapshot.
    Startup indexes Class.php but does not recheck Use.php, leaving its
    invalid return type undiagnosed. Including the edit in the snapshot
    would trigger the consumer recheck. *)
let test_edit_after_dirty_snapshot () =
  let open Scheduled_fanout in
  Scheduled_fanout.run_expect_bug
    ~bug:"F9: saved-state startup notification timing"
    ~expectation:
      (Diagnostics
         { incremental = []; full = [("Use.php", 4110, "Invalid return type")] })
    ~initialization:
      (Saved_state_with_loading_schedule
         {
           before_dirty_snapshot = [];
           after_dirty_snapshot =
             [Write_file ("Class.php", changed_class_contents)];
         })
    ~remote_old_decls:initial_class_contents
    {
      initial_files =
        [
          ("Class.php", initial_class_contents);
          ("Use.php", {|<?hh
function use_c(): int {
  return C::value();
}
|});
        ];
      (* Startup has consumed the edit's notification. An idle iteration still
         misses the consumer error that a fresh check reports. *)
      iterations = [{ default_iteration with type_check = [Remaining] }];
    };
  true

let () =
  Daemon.check_entry_point ();
  Unit_test.run_all
    [("edit_after_dirty_snapshot", test_edit_after_dirty_snapshot)]
