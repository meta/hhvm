(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *)

(* These tests validate the harness. Some deliberately expect diagnostic
   mismatches to check the runner's ordering and final-state comparison. Real
   fanout bug reproducers belong in separate files under regressions/. *)

open Hh_prelude

let int_definition = "<?hh\nfunction value(): int { return 1; }\n"

let string_definition = "<?hh\nfunction value(): string { return 'value'; }\n"

let bool_definition = "<?hh\nfunction value(): bool { return true; }\n"

let caller = "<?hh\nfunction caller(): int { return value(); }\n"

let initial_files = [("value.php", int_definition); ("caller.php", caller)]

(** A second iteration must use the disk contents written after the first
    iteration's indexing, while retaining the existing caller dependency. The
    final return type is deliberately incompatible with the unchanged caller so
    agreement requires matching nonempty diagnostics. *)
let test_change_after_indexing () =
  Scheduled_fanout.run
    {
      initial_files;
      iterations =
        [
          {
            Scheduled_fanout.default_iteration with
            before_type_check = [Write_file ("value.php", string_definition)];
            disk_needs_parsing = ["value.php"];
            after_indexing = [Write_file ("value.php", bool_definition)];
            type_check = [Remaining];
          };
          {
            Scheduled_fanout.default_iteration with
            disk_needs_parsing = ["value.php"];
            type_check = [Remaining];
          };
        ];
    };
  true

(** A save after z.php is redeclared must be visible when value.php is
    redeclared next. Its changed signature must fan out to the caller, so both
    incremental and fresh checking report the incompatible return type. *)
let test_change_between_redeclarations () =
  Scheduled_fanout.run
    {
      initial_files =
        ("z.php", "<?hh\nfunction z(): void {}\n") :: initial_files;
      iterations =
        [
          {
            Scheduled_fanout.default_iteration with
            disk_needs_parsing = ["z.php"; "value.php"];
            after_redeclaring =
              [("z.php", [Write_file ("value.php", string_definition)])];
            type_check = [Remaining];
          };
        ];
    };
  true

(** The hook must run after its own file's declarations are cached. Changing
    value's signature there leaves its already cached int declaration visible
    to the caller. The expected mismatch distinguishes this from a hook that
    runs before redeclaration or invalidates the cache when it writes. *)
let test_redecl_hook_runs_after_file () =
  let scenario =
    Scheduled_fanout.
      {
        initial_files;
        iterations =
          [
            {
              default_iteration with
              disk_needs_parsing = ["value.php"; "caller.php"];
              after_redeclaring =
                [("value.php", [Write_file ("value.php", string_definition)])];
              type_check = [Remaining];
            };
          ];
      }
  in
  match Scheduled_fanout.run scenario with
  | () -> failwith "Expected the redeclaration hook to run after caching value"
  | exception Scheduled_fanout.Diagnostics_mismatch _ -> true

(** A file created only by a redeclaration hook must participate in the fresh
    check, even though the incremental run never received its notification. *)
let test_full_check_includes_file_added_after_redecl () =
  let scenario =
    Scheduled_fanout.
      {
        initial_files = [("value.php", int_definition)];
        iterations =
          [
            {
              default_iteration with
              disk_needs_parsing = ["value.php"];
              after_redeclaring =
                [
                  ( "value.php",
                    [
                      Write_file
                        ( "new.php",
                          "<?hh\nfunction broken(): int { return 'wrong'; }\n"
                        );
                    ] );
                ];
              type_check = [Remaining];
            };
          ];
      }
  in
  match Scheduled_fanout.run scenario with
  | () -> failwith "Expected the fresh check to include the new file"
  | exception Scheduled_fanout.Diagnostics_mismatch _ -> true

(** Indexing must have captured the file's names before the hook writes a new
    function. Checking its AST without a naming entry produces an internal
    error, which the harness must reject. Moving the hook before indexing would
    register the new function and avoid the expected failure. *)
let test_indexing_precedes_hook () =
  let scenario =
    Scheduled_fanout.
      {
        initial_files =
          [
            ("definitions.php", "<?hh\n");
            ("caller.php", "<?hh\nfunction caller(): int { return added(); }\n");
          ];
        iterations =
          [
            {
              default_iteration with
              disk_needs_parsing = ["definitions.php"];
              after_indexing =
                [
                  Write_file
                    ( "definitions.php",
                      "<?hh\nfunction added(): int { return 1; }\n" );
                ];
              type_check = [Remaining];
            };
          ];
      }
  in
  match Scheduled_fanout.run scenario with
  | () -> failwith "Expected indexing to precede the new definition"
  | exception Scheduled_fanout.Internal_typechecker_error _ -> true

(** The oracle must detect stale diagnostics when the last iteration changes
    an unreported file after indexing. An implicit catch-up recheck, skipped
    hook, or oracle that reused the incremental heaps would hide this mismatch.
    The expected-bug wrapper must accept the caller's full-check error while
    requiring the incremental check to remain clean. *)
let test_detects_unreported_change () =
  let scenario =
    Scheduled_fanout.
      {
        initial_files;
        iterations =
          [
            {
              default_iteration with
              after_indexing = [Write_file ("value.php", string_definition)];
              type_check = [Remaining];
            };
          ];
      }
  in
  let expected = ("caller.php", 4110, "Invalid return type") in
  Scheduled_fanout.run_expect_bug
    ~bug:"unreported edit harness control"
    ~expectation:(Diagnostics { incremental = []; full = [expected] })
    scenario;
  (* An internal-error expectation must reject an otherwise matching
     diagnostic discrepancy. *)
  (match
     Scheduled_fanout.run_expect_bug
       ~bug:"internal error harness control"
       ~expectation:(Internal_error "Expected internal typechecker error")
       scenario
   with
  | () ->
    failwith "Expected a diagnostic mismatch to reject internal-error mode"
  | exception Scheduled_fanout.Diagnostics_mismatch _ -> ());
  (* Expecting another occurrence must not accept the single actual error. *)
  match
    Scheduled_fanout.run_expect_bug
      ~bug:"extra diagnostic harness control"
      ~expectation:
        (Diagnostics { incremental = []; full = [expected; expected] })
      scenario
  with
  | () -> failwith "Expected rejection of a different diagnostic count"
  | exception Scheduled_fanout.Diagnostics_mismatch _ -> true

(** A documented internal error can be expected explicitly. Its exact failure
    must be accepted, without accepting other internal typechecker errors. *)
let test_expected_bug_accepts_internal_error () =
  Scheduled_fanout.expect_known_fanout_bug
    ~bug:"known internal error harness control"
    ~matches:(function
      | Scheduled_fanout.Internal_typechecker_error "known internal error" ->
        true
      | _ -> false)
    (fun () ->
      raise (Scheduled_fanout.Internal_typechecker_error "known internal error"));
  true

(** A check that starts succeeding must make its expected-bug test fail with
    the bug identifier and instructions to retain the correctness check. *)
let test_expected_bug_reports_fix () =
  match
    Scheduled_fanout.expect_known_fanout_bug
      ~bug:"T123 harness control"
      ~matches:(fun _ -> failwith "A successful check has no failure to match")
      (fun () -> ())
  with
  | () -> false
  | exception Failure message ->
    String.is_substring message ~substring:"T123 harness control"
    && String.is_substring message ~substring:"You probably fixed this bug"
    && String.is_substring message ~substring:"remove the wrapper"

(** Rejecting the failure matcher must preserve the original mismatch or
    internal error, so a different bug cannot satisfy this expectation. *)
let test_expected_bug_preserves_other_harness_failures () =
  List.for_all
    Scheduled_fanout.
      [
        Diagnostics_mismatch "different mismatch";
        Internal_typechecker_error "different internal error";
      ]
    ~f:(fun failure ->
      Unit_test.expect_throws
        failure
        (fun () ->
          Scheduled_fanout.expect_known_fanout_bug
            ~bug:"different failure harness control"
            ~matches:(fun _ -> false)
            (fun () -> raise failure))
        ())

(** Invalid scenarios and infrastructure failures must propagate without
    consulting the harness-failure matcher. *)
let test_expected_bug_preserves_unrelated_failures () =
  List.for_all
    [Invalid_argument "invalid scenario"; Failure "child exited with code 1"]
    ~f:(fun failure ->
      Unit_test.expect_throws
        failure
        (fun () ->
          Scheduled_fanout.expect_known_fanout_bug
            ~bug:"unrelated failure harness control"
            ~matches:(fun _ ->
              failwith "Unrelated exceptions must not reach the matcher")
            (fun () -> raise failure))
        ())

(** A file introduced only at a phase boundary must be included in the fresh
    final-state check even though it was never reported to the incremental run. *)
let test_full_check_includes_new_file () =
  let scenario =
    Scheduled_fanout.
      {
        initial_files = [("value.php", int_definition)];
        iterations =
          [
            {
              default_iteration with
              after_indexing =
                [
                  Write_file
                    ( "new.php",
                      "<?hh\nfunction broken(): int { return 'wrong'; }\n" );
                ];
              type_check = [Remaining];
            };
          ];
      }
  in
  match Scheduled_fanout.run scenario with
  | () -> failwith "Expected the fresh check to include the new file"
  | exception Scheduled_fanout.Diagnostics_mismatch _ -> true

(** Remaining must check unlisted files after the named-check prefix. The first
    check changes second.php before Remaining checks it, so both runs must
    report second.php's new typing error. *)
let test_file_check_order () =
  Scheduled_fanout.run
    {
      initial_files =
        [
          ("first.php", "<?hh\nfunction first(): int { return 1; }\n");
          ("second.php", "<?hh\nfunction second(): int { return 2; }\n");
        ];
      iterations =
        [
          {
            Scheduled_fanout.default_iteration with
            disk_needs_parsing = ["first.php"; "second.php"];
            type_check =
              [
                Check
                  {
                    file = "first.php";
                    after_check =
                      [
                        Write_file
                          ( "second.php",
                            "<?hh\nfunction second(): int { return 'wrong'; }\n"
                          );
                      ];
                  };
                Remaining;
              ];
          };
        ];
    };
  true

(** Completion hooks must execute, and the fresh check must include files they
    create. The incremental check has already finished its only selected file,
    so it cannot see the new file's error without an unrequested catch-up check. *)
let test_full_check_includes_file_added_after_check () =
  let scenario =
    Scheduled_fanout.
      {
        initial_files = [("value.php", int_definition)];
        iterations =
          [
            {
              default_iteration with
              disk_needs_parsing = ["value.php"];
              type_check =
                [
                  Check
                    {
                      file = "value.php";
                      after_check =
                        [
                          Write_file
                            ( "new.php",
                              "<?hh\nfunction broken(): int { return 'wrong'; }\n"
                            );
                        ];
                    };
                  Remaining;
                ];
            };
          ];
      }
  in
  match Scheduled_fanout.run scenario with
  | () -> failwith "Expected the fresh check to include the new file"
  | exception Scheduled_fanout.Diagnostics_mismatch _ -> true

(** A completion hook must run after its own file has been checked. Changing
    that file's body in the hook leaves the incremental result clean while the
    fresh check sees an error. Calling the hook before typing would hide this
    mismatch and could make a real bug reproducer pass without exercising it. *)
let test_completion_hook_runs_after_file () =
  let scenario =
    Scheduled_fanout.
      {
        initial_files = [("value.php", int_definition)];
        iterations =
          [
            {
              default_iteration with
              disk_needs_parsing = ["value.php"];
              type_check =
                [
                  Check
                    {
                      file = "value.php";
                      after_check =
                        [
                          Write_file
                            ( "value.php",
                              "<?hh\nfunction value(): int { return 'wrong'; }\n"
                            );
                        ];
                    };
                  Remaining;
                ];
            };
          ];
      }
  in
  match Scheduled_fanout.run scenario with
  | () -> failwith "Expected the completion hook to run after typing"
  | exception Scheduled_fanout.Diagnostics_mismatch _ -> true

(** A check after Remaining must be rejected as an invalid schedule before
    typechecking starts. Remaining can only terminate the named-check prefix. *)
let test_rejects_check_after_remaining () =
  let scenario =
    Scheduled_fanout.
      {
        initial_files;
        iterations =
          [
            {
              default_iteration with
              disk_needs_parsing = ["value.php"];
              type_check =
                [Remaining; Check { file = "value.php"; after_check = [] }];
            };
          ];
      }
  in
  match Scheduled_fanout.run scenario with
  | () -> failwith "Expected a check after Remaining to be rejected"
  | exception Invalid_argument _ -> true

(** Renaming a function after indexing leaves its AST without a naming entry,
    so test-mode invariant handling produces a caught internal typechecker error.
    A second iteration would repair naming and clear that diagnostic, making
    the final equality check pass. The harness must reject the first iteration
    instead of accepting a scenario that skipped part of typechecking. *)
let test_rejects_transient_internal_error () =
  let scenario =
    Scheduled_fanout.
      {
        initial_files = [("value.php", int_definition)];
        iterations =
          [
            {
              default_iteration with
              disk_needs_parsing = ["value.php"];
              after_indexing =
                [
                  Write_file
                    ( "value.php",
                      "<?hh\nfunction renamed(): int { return 1; }\n" );
                ];
              type_check = [Remaining];
            };
            {
              default_iteration with
              disk_needs_parsing = ["value.php"];
              type_check = [Remaining];
            };
          ];
      }
  in
  let message =
    match Scheduled_fanout.run scenario with
    | () ->
      failwith "Expected the first iteration's internal error to be rejected"
    | exception Scheduled_fanout.Internal_typechecker_error message ->
      if not (String.is_substring message ~substring:"during iteration 1;") then
        failwith ("Expected rejection after the first iteration: " ^ message);
      message
  in
  Scheduled_fanout.run_expect_bug
    ~bug:"known internal error harness control"
    ~expectation:(Internal_error message)
    scenario;
  (* The same failure must propagate when a different message is expected. *)
  match
    Scheduled_fanout.run_expect_bug
      ~bug:"different internal error harness control"
      ~expectation:(Internal_error (message ^ " Different failure."))
      scenario
  with
  | () -> failwith "Expected rejection of a different internal error"
  | exception Scheduled_fanout.Internal_typechecker_error actual ->
    String.equal message actual

(** Unsupported experimental features are ordinary source errors, even though
    they share code zero with InternalError. Matching __Const diagnostics must
    be accepted in both the baseline and the fresh check. *)
let test_accepts_experimental_feature_error () =
  Scheduled_fanout.run
    {
      initial_files = [("const.php", "<?hh\n<<__Const>> class C {}\n")];
      iterations =
        [{ Scheduled_fanout.default_iteration with type_check = [Remaining] }];
    };
  true

(** An initialized parent heap must not contaminate either child. The sharded
    heap can only be initialized once per process, so each check needs its own
    fresh process even when the caller has already initialized shared memory. *)
let test_initialized_parent () =
  let handle =
    Shared_mem.init
      ~num_workers:0
      {
        Shared_mem.default_config with
        shm_use_sharded_hashtbl = true;
        shm_cache_size = 2 * 1024 * 1024 * 1024;
      }
  in
  Shared_mem.set_close_on_exec handle;
  (* Both children must report the caller's now-invalid return type. *)
  Scheduled_fanout.run
    {
      initial_files;
      iterations =
        [
          {
            Scheduled_fanout.default_iteration with
            before_type_check = [Write_file ("value.php", string_definition)];
            disk_needs_parsing = ["value.php"];
            type_check = [Remaining];
          };
        ];
    };
  true

(** Verify that a repaired middle file is retried without requeueing, its old
    error disappears, its action is skipped, and later checks and actions run. *)
let test_cancelled_check_is_retried () =
  let open Scheduled_fanout in
  run_expect_bug
    ~bug:"cancelled middle check harness control"
    ~expectation:
      (Diagnostics
         {
           incremental = [];
           full = [("third.php", 4110, "Invalid return type")];
         })
    {
      (* The middle file starts broken so retrying it must clear an old error. *)
      initial_files =
        [
          ("first.php", "<?hh\nfunction first(): void {}\n");
          ("second.php", "<?hh\nfunction second(): int { return 'wrong'; }\n");
          ("third.php", "<?hh\nfunction third(): int { return 1; }\n");
        ];
      iterations =
        [
          {
            default_iteration with
            before_type_check =
              [
                Write_file
                  ("second.php", "<?hh\nfunction second(): int { return 1; }\n");
              ];
            disk_needs_parsing = ["first.php"; "second.php"; "third.php"];
            type_check =
              [
                Check { file = "first.php"; after_check = [] };
                Check
                  {
                    file = "second.php";
                    (* This error-producing action must be skipped. *)
                    after_check =
                      [
                        Write_file
                          ( "sentinel.php",
                            "<?hh\nfunction sentinel(): int { return 'wrong'; }\n"
                          );
                      ];
                  };
                Check
                  {
                    file = "third.php";
                    (* The deliberate unreported error proves this action ran:
                       only the fresh check should report it. *)
                    after_check =
                      [
                        Write_file
                          ( "third.php",
                            "<?hh\nfunction third(): int { return 'wrong'; }\n"
                          );
                      ];
                  };
                Remaining;
              ];
            cancelled_checks = ["second.php"];
          };
          (* No manual requeue: the cancelled middle file is still pending. *)
          { default_iteration with type_check = [Remaining] };
        ];
    };
  true

(** A scenario ending with a cancelled check is unfinished. Reject it before
    comparing stale diagnostics against the freshly checked final files. *)
let test_rejects_unfinished_cancelled_check () =
  let open Scheduled_fanout in
  let scenario =
    {
      initial_files;
      iterations =
        [
          {
            default_iteration with
            disk_needs_parsing = ["caller.php"];
            type_check = [Remaining];
            cancelled_checks = ["caller.php"];
          };
        ];
    }
  in
  match run scenario with
  | () -> failwith "Expected rejection of an unfinished cancelled check"
  | exception Invalid_argument message ->
    if
      not
        (String.is_substring
           message
           ~substring:"pending parsing or typechecking: caller.php")
    then
      failwith message;
    true

let () =
  Daemon.check_entry_point ();
  Unit_test.run_all
    [
      ("change_after_indexing", test_change_after_indexing);
      ("change_between_redeclarations", test_change_between_redeclarations);
      ("redecl_hook_runs_after_file", test_redecl_hook_runs_after_file);
      ( "full_check_includes_file_added_after_redecl",
        test_full_check_includes_file_added_after_redecl );
      ("indexing_precedes_hook", test_indexing_precedes_hook);
      ("detects_unreported_change", test_detects_unreported_change);
      ( "expected_bug_accepts_internal_error",
        test_expected_bug_accepts_internal_error );
      ("expected_bug_reports_fix", test_expected_bug_reports_fix);
      ( "expected_bug_preserves_other_harness_failures",
        test_expected_bug_preserves_other_harness_failures );
      ( "expected_bug_preserves_unrelated_failures",
        test_expected_bug_preserves_unrelated_failures );
      ("full_check_includes_new_file", test_full_check_includes_new_file);
      ("file_check_order", test_file_check_order);
      ( "full_check_includes_file_added_after_check",
        test_full_check_includes_file_added_after_check );
      ("completion_hook_runs_after_file", test_completion_hook_runs_after_file);
      ("rejects_check_after_remaining", test_rejects_check_after_remaining);
      ("rejects_transient_internal_error", test_rejects_transient_internal_error);
      ( "accepts_experimental_feature_error",
        test_accepts_experimental_feature_error );
      ("cancelled_check_is_retried", test_cancelled_check_is_retried);
      ( "rejects_unfinished_cancelled_check",
        test_rejects_unfinished_cancelled_check );
      ("initialized_parent", test_initialized_parent);
    ]
