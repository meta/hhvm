(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *)

let duplicate_files =
  [
    ( "a.php",
      "<?hh\nclass A { public static function value(): int { return 1; } }\n" );
    ( "z.php",
      "<?hh\nclass A { public static function value(): string { return 'value'; } }\n"
    );
    ("use.php", "<?hh\nfunction use_a(): int { return A::value(); }\n");
  ]

(** Only z.php has a saved duplicate-name error. Deleting the winner after
    startup must also rename the untouched z.php and recheck use.php against its
    string return type. Missing owner tracking leaves the surviving definition
    without a naming binding, causing a declaration-consistency error. *)
let test_saved_duplicate_winner_removed () : bool =
  let open Scheduled_fanout in
  run
    ~initialization:
      (Saved_state_with_loading_schedule
         { before_dirty_snapshot = []; after_dirty_snapshot = [] })
    ~expected_diagnostics:[("use.php", 4110, "Invalid return type")]
    {
      initial_files = duplicate_files;
      iterations =
        [
          (* Finish the initial saved-error rechecks before the edit. *)
          default_iteration;
          {
            default_iteration with
            before_type_check = [Write_file ("a.php", "<?hh\n")];
            disk_needs_parsing = ["a.php"];
          };
        ];
    };
  true

(** Removing the saved-state loser must preserve the untouched winner's
    reverse naming entry. Removing old names from z.php alone erases that entry
    and makes typechecking the surviving class fail declaration consistency. *)
let test_saved_duplicate_loser_removed () : bool =
  let open Scheduled_fanout in
  run
    ~initialization:
      (Saved_state_with_loading_schedule
         { before_dirty_snapshot = []; after_dirty_snapshot = [] })
    ~expected_diagnostics:[]
    {
      initial_files = duplicate_files;
      iterations =
        [
          (* Finish the initial saved-error rechecks before the edit. *)
          default_iteration;
          {
            default_iteration with
            before_type_check = [Write_file ("z.php", "<?hh\n")];
            disk_needs_parsing = ["z.php"];
          };
        ];
    };
  true

(** The baseline winner must be recovered before processing a loser that was
    removed between saving and startup. Looking at current contents or waiting
    until after reverse naming removal cannot recover the winner's binding. *)
let test_saved_duplicate_removed_during_startup () : bool =
  let open Scheduled_fanout in
  run
    ~initialization:
      (Saved_state_with_loading_schedule
         {
           before_dirty_snapshot = [Write_file ("z.php", "<?hh\n")];
           after_dirty_snapshot = [];
         })
    ~expected_diagnostics:[]
    { initial_files = duplicate_files; iterations = [default_iteration] };
  true

let () =
  Daemon.check_entry_point ();
  Unit_test.run_all
    [
      ("saved_duplicate_winner_removed", test_saved_duplicate_winner_removed);
      ("saved_duplicate_loser_removed", test_saved_duplicate_loser_removed);
      ( "saved_duplicate_removed_during_startup",
        test_saved_duplicate_removed_during_startup );
    ]
