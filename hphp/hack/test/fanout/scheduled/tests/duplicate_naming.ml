(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *)

let class_a_names : File_info.names =
  { File_info.empty_names with File_info.n_classes = S_set.singleton "\\A" }

let unchanged_class_a : Decl_compare.VersionedNames.t =
  { old_names = class_a_names; new_names = class_a_names }

let duplicate_files =
  [
    ( "a.php",
      "<?hh\nclass A { public static function value(): int { return 1; } }\n" );
    ( "z.php",
      "<?hh\nclass A { public static function value(): string { return 'value'; } }\n"
    );
    ("use.php", "<?hh\nfunction use_a(): int { return A::value(); }\n");
  ]

(** After a full baseline check, deleting the winner must promote the untouched
    duplicate and recheck the consumer against its string return type. *)
let test_duplicate_winner_removed () : bool =
  let open Scheduled_fanout in
  run
    ~initialization:Full_check
    ~expected_diagnostics:[("use.php", 4110, "Invalid return type")]
    {
      initial_files = duplicate_files;
      iterations =
        [
          {
            default_iteration with
            before_type_check = [Write_file ("a.php", "<?hh\n")];
            disk_needs_parsing = ["a.php"];
            expected_versioned_names =
              Some
                [
                  ( "a.php",
                    {
                      old_names = class_a_names;
                      new_names = File_info.empty_names;
                    } );
                  ("z.php", unchanged_class_a);
                ];
          };
        ];
    };
  true

(** After a full baseline check, deleting the loser must preserve the untouched
    winner's naming binding and leave its consumer without errors. *)
let test_duplicate_loser_removed () : bool =
  let open Scheduled_fanout in
  run
    ~initialization:Full_check
    ~expected_diagnostics:[]
    {
      initial_files = duplicate_files;
      iterations =
        [
          {
            default_iteration with
            before_type_check = [Write_file ("z.php", "<?hh\n")];
            disk_needs_parsing = ["z.php"];
            expected_versioned_names =
              Some
                [
                  ("a.php", unchanged_class_a);
                  ( "z.php",
                    {
                      old_names = class_a_names;
                      new_names = File_info.empty_names;
                    } );
                ];
          };
        ];
    };
  true

(** Creating a duplicate in a later filename must report the naming error
    and preserve the winner and its consumer's valid return type. *)
let test_duplicate_created () : bool =
  let open Scheduled_fanout in
  run
    ~expected_diagnostics:[("z.php", 2012, "Name already bound: `A`")]
    {
      initial_files =
        [
          ( "a.php",
            "<?hh\nclass A { public static function value(): int { return 1; } }\n"
          );
          ("use.php", "<?hh\nfunction use_a(): int { return A::value(); }\n");
        ];
      iterations =
        [
          {
            default_iteration with
            before_type_check =
              [
                Write_file
                  ( "z.php",
                    "<?hh\nclass A { public static function value(): string { return 'value'; } }\n"
                  );
              ];
            disk_needs_parsing = ["z.php"];
            expected_versioned_names =
              Some
                [
                  ("a.php", unchanged_class_a);
                  ( "z.php",
                    {
                      old_names = File_info.empty_names;
                      new_names = class_a_names;
                    } );
                ];
          };
        ];
    };
  true

(** Record the undesirable history-dependent winner selection behavior
    documented on [Server_type_check.do_naming]: The first iteration keeps the
    previous winner in z.php, disagreeing with a subsequent re-check (or a full
    init). A naming retry (e.g., triggered by an edit to an unrelated file) then
    selects a.php, changing the winner and the consumer's diagnostics without
    another edit to either duplicate definition. If this test ever fails, this
    may indicate that the duplicate-naming behavior has been improved. *)
let test_duplicate_created_then_retried () : bool =
  let open Scheduled_fanout in
  let scenario =
    {
      initial_files =
        [
          ( "z.php",
            "<?hh\nclass A { public static function value(): int { return 1; } }\n"
          );
          ("use.php", "<?hh\nfunction use_a(): int { return A::value(); }\n");
        ];
      iterations =
        [
          {
            default_iteration with
            before_type_check =
              [
                Write_file
                  ( "a.php",
                    "<?hh\nclass A { public static function value(): string { return 'value'; } }\n"
                  );
              ];
            disk_needs_parsing = ["a.php"];
            expected_versioned_names =
              Some
                [
                  ( "a.php",
                    {
                      old_names = File_info.empty_names;
                      new_names = class_a_names;
                    } );
                  ("z.php", unchanged_class_a);
                ];
          };
        ];
    }
  in
  let expected_after_retry =
    [
      ("z.php", 2012, "Name already bound: `A`");
      ("use.php", 4110, "Invalid return type");
    ]
  in
  (* Initially z.php still wins, so only a.php has a duplicate-name error.
     A fresh check instead picks a.php and rejects the consumer's return type. *)
  run_expect_bug
    ~bug:"history-dependent duplicate winner documented on do_naming"
    ~expectation:
      (Diagnostics
         {
           incremental = [("a.php", 2012, "Name already bound: `A`")];
           full = expected_after_retry;
         })
    scenario;
  (* Retry naming with both conflicting files: a.php wins and the consumer must
     be rechecked against its string return type. In the real world, any edit to
     an unrelated file would trigger this. *)
  run
    ~expected_diagnostics:expected_after_retry
    {
      scenario with
      iterations =
        scenario.iterations
        @ [
            {
              default_iteration with
              expected_versioned_names =
                Some
                  [("a.php", unchanged_class_a); ("z.php", unchanged_class_a)];
            };
            (* A further retry observes the now-established winner. *)
            {
              default_iteration with
              expected_versioned_names =
                Some
                  [("a.php", unchanged_class_a); ("z.php", unchanged_class_a)];
            };
          ];
    };
  true

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
            expected_versioned_names =
              Some
                [
                  ( "a.php",
                    {
                      old_names = class_a_names;
                      new_names = File_info.empty_names;
                    } );
                  ("z.php", unchanged_class_a);
                ];
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
            expected_versioned_names =
              Some
                [
                  ("a.php", unchanged_class_a);
                  ( "z.php",
                    {
                      old_names = class_a_names;
                      new_names = File_info.empty_names;
                    } );
                ];
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
      ("duplicate_winner_removed", test_duplicate_winner_removed);
      ("duplicate_loser_removed", test_duplicate_loser_removed);
      ("duplicate_created", test_duplicate_created);
      ("duplicate_created_then_retried", test_duplicate_created_then_retried);
      ("saved_duplicate_winner_removed", test_saved_duplicate_winner_removed);
      ("saved_duplicate_loser_removed", test_saved_duplicate_loser_removed);
      ( "saved_duplicate_removed_during_startup",
        test_saved_duplicate_removed_during_startup );
    ]
