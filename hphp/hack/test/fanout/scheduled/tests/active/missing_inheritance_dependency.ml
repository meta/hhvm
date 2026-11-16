(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *)

(* Symptom: Ancestor changes can leave errors missing or out of date in files
   using inherited members. A consumer recheck can still use an old inherited
   declaration, and resolving a removed method can cause an internal error.

   Cause: Folding a class can cache inherited members and ancestor information
   before class typechecking records the corresponding inheritance dependency.
   A failed or cancelled check, or a check of another file version, can leave
   that edge absent when an ancestor changes. Without the edge, the change can
   miss both invalidation of the descendant's folded declaration and fanout to
   its consumers. Recording the edge later does not repair the stale folded
   declaration or diagnostics left by the earlier change. *)

(** Repairing a child and changing its parent in the same iteration misses a
    consumer recheck. Every edit is notified before checking; no phase hooks
    or cancellation are needed. *)
let test_completed_edits_miss_consumer_fanout () =
  let scenario =
    Scheduled_fanout.
      {
        initial_files =
          [
            ("Parent.php", {|<?hh
class P {
  const int K = 1;
}
|});
            ("Child.php", {|<?hh
class C {
  const int K = 1;
}
|});
            ("Use.php", {|<?hh
function use_c(): int {
  return C::K;
}
|});
          ];
        iterations =
          [
            {
              default_iteration with
              (* C's declaration inherits K, but the parse error prevents
                 its check from recording the new C-to-P dependency. *)
              before_type_check =
                [
                  Write_file
                    ( "Child.php",
                      {|<?hh
class C extends P {
  public function unrelated(): void {
    $x = ;
  }
}
|}
                    );
                ];
              disk_needs_parsing = ["Child.php"];
              type_check = [Remaining];
            };
            {
              default_iteration with
              (* The repair leaves C's shallow declaration unchanged. P's
                 fanout needs the missing inheritance edge to reach Use. *)
              before_type_check =
                [
                  Write_file
                    ( "Child.php",
                      {|<?hh
class C extends P {
  public function unrelated(): void {
    $x = 1;
  }
}
|}
                    );
                  Write_file
                    ( "Parent.php",
                      {|<?hh
class P {
  const string K = 'value';
}
|} );
                ];
              disk_needs_parsing = ["Child.php"; "Parent.php"];
              type_check = [Remaining];
            };
          ];
      }
  in
  (* Checking the repaired child before changing P would record the edge in
     time. Explicitly rechecking Use afterwards also finds the error; this
     case misses a consumer check rather than retaining a stale declaration. *)
  Scheduled_fanout.run_expect_bug
    ~bug:"F7: inheritance dependencies"
    ~expectation:
      (Diagnostics
         { incremental = []; full = [("Use.php", 4110, "Invalid return type")] })
    scenario;
  true

(** Cancelling C's first check leaves its inherited constant cached with the
    old type. Changing P before C is retried leaves the consumer unchecked;
    explicitly rechecking the consumer still misses the error. No parse errors
    are involved. *)
let test_cancelled_child_retains_stale_constant () =
  let consumer = {|<?hh
function use_c(): int {
  return C::K;
}
|} in
  let scenario =
    Scheduled_fanout.
      {
        initial_files =
          [
            ("Parent.php", {|<?hh
class P {
  const int K = 1;
}
|});
            ("Child.php", {|<?hh
class C {
  const int K = 1;
}
|});
            ("Use.php", consumer);
          ];
        iterations =
          [
            {
              default_iteration with
              (* Redeclaration establishes C's inherited constant, but its
                 cancelled check leaves the C-to-P edge absent. *)
              before_type_check =
                [Write_file ("Child.php", "<?hh\nclass C extends P {}\n")];
              disk_needs_parsing = ["Child.php"];
              cancelled_checks = ["Child.php"];
              type_check = [Remaining];
            };
            {
              default_iteration with
              (* P changes before the pending child check records the edge.
                 C's folded declaration is therefore not invalidated. *)
              before_type_check =
                [
                  Write_file
                    ( "Parent.php",
                      {|<?hh
class P {
  const string K = 'value';
}
|} );
                ];
              disk_needs_parsing = ["Parent.php"];
              type_check = [Remaining];
            };
            {
              default_iteration with
              (* A real consumer edit forces a recheck, which still reads
                 C's stale int constant. *)
              before_type_check =
                [Write_file ("Use.php", consumer ^ "// Check again.\n")];
              disk_needs_parsing = ["Use.php"];
              type_check = [Remaining];
            };
          ];
      }
  in
  (* Letting C finish before processing P's edit would record the dependency
     in time and invalidate C when P changes. *)
  Scheduled_fanout.run_expect_bug
    ~bug:"F7: inheritance dependencies"
    ~expectation:
      (Diagnostics
         { incremental = []; full = [("Use.php", 4110, "Invalid return type")] })
    scenario;
  true

(** A successful child check can still miss the edge when it reads a different
    version from redeclaration. Making P implement I later leaves a false
    subtype error in the consumer. *)
let test_child_check_reads_different_version () =
  let child_extends_parent = "<?hh\nclass D extends P {}\n" in
  let scenario =
    Scheduled_fanout.
      {
        initial_files =
          [
            ("P.php", "<?hh\nclass P {}\n");
            ("I.php", "<?hh\ninterface I {}\n");
            ("D.php", "<?hh\nclass D {}\n");
            ("AUse.php", {|<?hh
function u(D $d): I {
  return $d;
}
|});
          ];
        iterations =
          [
            {
              default_iteration with
              before_type_check = [Write_file ("D.php", child_extends_parent)];
              disk_needs_parsing = ["D.php"];
              type_check =
                [
                  Check
                    {
                      file = "AUse.php";
                      (* Redeclaration saw D's new parent. Remove it only
                         from the version that D's check will read. *)
                      after_check = [Write_file ("D.php", "<?hh\nclass D {}\n")];
                    };
                  Check
                    {
                      file = "D.php";
                      after_check = [Write_file ("D.php", child_extends_parent)];
                    };
                  Remaining;
                ];
            };
            {
              default_iteration with
              (* Deliver D's saves and change P. D's shallow declaration is
                 unchanged, and P's dependency graph still omits D. *)
              before_type_check =
                [Write_file ("P.php", "<?hh\nclass P implements I {}\n")];
              disk_needs_parsing = ["P.php"; "D.php"];
              type_check = [Remaining];
            };
          ];
      }
  in
  (* Without the intervening saves, D's successful check would record the
     same parent relationship used by its declaration. *)
  Scheduled_fanout.run_expect_bug
    ~bug:"F7: inheritance dependencies"
    ~expectation:
      (Diagnostics
         {
           incremental = [("AUse.php", 4110, "Invalid return type")];
           full = [];
         })
    scenario;
  true

(** Removing a parent method after cancelling D's first check rechecks its
    consumer through the method dependency. D's stale folded declaration still
    lists the removed method. Looking up that method raises
    [Decl_class.Decl_heap_elems_bug], which becomes the internal-error diagnostic. *)
let test_cancelled_child_retains_removed_method () =
  let scenario =
    Scheduled_fanout.
      {
        initial_files =
          [
            ("P.php", {|<?hh
class P {
  public function m(): void {}
}
|});
            ("D.php", "<?hh\nclass D {}\n");
            ("AUse.php", {|<?hh
function u(D $d): void {
  $d->m();
}
|});
          ];
        iterations =
          [
            {
              default_iteration with
              (* U's recheck folds D and records its dependency on P::m.
                 Cancelling D leaves the inheritance dependency absent. *)
              before_type_check =
                [Write_file ("D.php", "<?hh\nclass D extends P {}\n")];
              disk_needs_parsing = ["D.php"];
              cancelled_checks = ["D.php"];
              type_check = [Remaining];
            };
            {
              default_iteration with
              (* P::m's removal selects U, but D is not invalidated and still
                 points to the now-removed method in P's declaration heap. *)
              before_type_check = [Write_file ("P.php", "<?hh\nclass P {}\n")];
              disk_needs_parsing = ["P.php"];
              type_check = [Remaining];
            };
          ];
      }
  in
  Scheduled_fanout.run_expect_bug
    ~bug:"F7: inheritance dependencies"
    ~expectation:
      (Internal_error
         "Internal typechecker errors during iteration 2; the scenario did not complete its checks:
[2:1-4:2] Encountered an internal compiler error while typechecking this. Addressing other errors, restarting the typechecker with `hh restart`, or rebasing might resolve this error. Please run `hh rage` and post in https://fb.workplace.com/groups/hackforhiphop/.")
    scenario;
  true

let () =
  Daemon.check_entry_point ();
  Unit_test.run_all
    [
      ( "completed_edits_miss_consumer_fanout",
        test_completed_edits_miss_consumer_fanout );
      ( "cancelled_child_retains_stale_constant",
        test_cancelled_child_retains_stale_constant );
      ( "child_check_reads_different_version",
        test_child_check_reads_different_version );
      ( "cancelled_child_retains_removed_method",
        test_cancelled_child_retains_removed_method );
    ]
