(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *)

(* Symptom: Removed definitions can remain available through stale cached
   declarations, even after a consumer is rechecked.

   Cause: Declaration caching can introduce names absent from indexed file
   information. Invalidation uses that indexed information to identify entries
   to remove, so it misses cached declarations whose names were never indexed.

   These cases add a definition between indexing and redeclaration, then remove
   it before indexing observes it. Function and constant lookups still find it.
   Later cases check repairs that introduce an indexed definition or remove
   the reference to the orphan. *)

let producer_contents = "<?hh\n"

let producer_with_temporary_function =
  producer_contents ^ {|
function g(): int {
  return 2;
}
|}

let caller_contents = {|<?hh
function caller(): int {
  return g();
}
|}

let temporary_function_scenario ~add_after_indexing ~recheck_caller =
  Scheduled_fanout.
    {
      (* The initial naming table and caches have never contained g. *)
      initial_files =
        [
          ("Producer.php", producer_contents);
          ("Caller.php", {|<?hh
function caller(): int {
  return 1;
}
|});
        ];
      iterations =
        [
          {
            (* The reproducer indexes Producer without g; the control adds g
               before indexing instead. An ordinary caller edit makes
               Caller.php part of this iteration's selected files. *)
            before_type_check =
              [
                Write_file
                  ( "Producer.php",
                    if add_after_indexing then
                      producer_contents ^ "// Trigger indexing.\n"
                    else
                      producer_with_temporary_function );
                Write_file ("Caller.php", caller_contents);
              ];
            disk_needs_parsing = ["Producer.php"; "Caller.php"];
            (* In the reproducer, the next save adds g after names were collected.
               Redeclaration reads this later version and caches g, although
               neither the old nor new naming table contains that name. *)
            after_indexing =
              (if add_after_indexing then
                [Write_file ("Producer.php", producer_with_temporary_function)]
              else
                []);
            type_check =
              [
                (* Let a consumer observe the temporary cached g, then undo
                   the definition before Producer.php itself is checked. This
                   avoids checking a definition absent from the naming table. *)
                Check
                  {
                    file = "Caller.php";
                    after_check =
                      [Write_file ("Producer.php", producer_contents)];
                  };
                Check { file = "Producer.php"; after_check = [] };
                Remaining;
              ];
          };
          {
            default_iteration with
            (* Deliver all Producer notifications, including the undo above.
               In the reproducer, both indexed snapshots are empty, so g is
               absent from the set of declarations to invalidate. The explicit
               caller-recheck variant makes another real comment-only edit. *)
            before_type_check =
              (if recheck_caller then
                [
                  Write_file
                    ("Caller.php", caller_contents ^ "// Check again.\n");
                ]
              else
                []);
            disk_needs_parsing =
              (if recheck_caller then
                ["Producer.php"; "Caller.php"]
              else
                ["Producer.php"]);
            type_check = [Remaining];
          };
        ];
    }

(** Explicitly rechecking the caller after g is removed must report the
    missing typing declaration, rather than resolve g from its cached entry. *)
let test_temporary_function_removed_with_caller_recheck () =
  Scheduled_fanout.run_expect_bug
    ~bug:"F6: unindexed function removal"
    ~incremental:[("Caller.php", 2049, "Unbound name: `g` (a global function)")]
    ~full:
      [
        ("Caller.php", 4107, "Unbound name (typing): `g`");
        ("Caller.php", 2049, "Unbound name: `g` (a global function)");
      ]
    (temporary_function_scenario ~add_after_indexing:true ~recheck_caller:true);
  true

(** The same add/remove sequence must work when indexing observes g. This
    control changes only the timing of the addition, so g is tracked and its
    later removal can invalidate its declaration and recheck the caller. *)
let test_indexed_temporary_function_removed () =
  Scheduled_fanout.run
    (temporary_function_scenario
       ~add_after_indexing:false
       ~recheck_caller:false);
  true

(** Scenario in which [transient] is written to F.php after indexing, cached by
    redeclaration, and removed before F.php is processed again, so that its
    names never enter the naming table. U.php then refers to one of them. *)
let scenario ~transient ~use =
  Scheduled_fanout.
    {
      initial_files =
        [("F.php", "<?hh\n"); ("AControl.php", "<?hh\n"); ("U.php", "<?hh\n")];
      iterations =
        [
          {
            (* 1. Queue comment-only edits to F.php and AControl.php. *)
            before_type_check =
              [
                Write_file ("F.php", "<?hh\n// edit\n");
                Write_file ("AControl.php", "<?hh\n// edit\n");
              ];
            disk_needs_parsing = ["F.php"; "AControl.php"];
            (* 2. Add the transient definition after indexing. Redeclaring
               F.php caches it, although it has no naming entry. *)
            after_indexing =
              [Write_file ("F.php", "<?hh\n// edit\n" ^ transient)];
            type_check =
              [
                Check
                  {
                    file = "AControl.php";
                    (* 3. Remove it before F.php is checked, so that no check
                       sees its AST. *)
                    after_check = [Write_file ("F.php", "<?hh\n// edit 2\n")];
                  };
                Remaining;
              ];
          };
          {
            default_iteration with
            (* 4. Deliver F.php's notification and make U refer to the
               transient name. Neither F.php's old nor new names include it,
               so its decl remains. *)
            before_type_check = [Write_file ("U.php", use)];
            disk_needs_parsing = ["F.php"; "U.php"];
            type_check = [Remaining];
          };
        ];
    }

(** A later caller of the removed function g must report it as unbound,
    rather than use its cached string return type. *)
let test_transient_function_remains_cached () =
  Scheduled_fanout.run_expect_bug
    ~bug:"F6: unindexed function and constant removal"
    ~incremental:
      [
        ("U.php", 4110, "Invalid return type");
        ("U.php", 2049, "Unbound name: `g` (a global function)");
      ]
    ~full:
      [
        ("U.php", 4107, "Unbound name (typing): `g`");
        ("U.php", 2049, "Unbound name: `g` (a global function)");
      ]
    (scenario
       ~transient:"function g(): string { return ''; }\n"
       ~use:"<?hh\nfunction u(): int {\n  return g();\n}\n");
  (* 5. A fresh check reports g as unbound, in naming and typing. The
     incremental check types the call with the cached g and reports an invalid
     return type instead of the typing error. *)
  true

(** A later use of the removed constant K must report it as unbound,
    rather than use its cached string type. *)
let test_transient_constant_remains_cached () =
  Scheduled_fanout.run_expect_bug
    ~bug:"F6: unindexed function and constant removal"
    ~incremental:
      [
        ("U.php", 4110, "Invalid return type");
        ("U.php", 2049, "Unbound name: `K` (a global constant)");
      ]
    ~full:
      [
        ("U.php", 4106, "Unbound global constant (Typing)");
        ("U.php", 2049, "Unbound name: `K` (a global constant)");
      ]
    (scenario
       ~transient:"const string K = '';\n"
       ~use:"<?hh\nfunction u(): int {\n  return K;\n}\n");
  (* 5. A fresh check reports K as unbound, in naming and typing. The
     incremental check types K with the cached decl and reports an invalid
     return type instead of the typing error. *)
  true

let empty_producer = "<?hh\n"

let int_function = {|<?hh
function g(): int {
  return 1;
}
|}

let string_function = {|<?hh
function g(): string {
  return 'ghost';
}
|}

let initial_caller = {|<?hh
function caller(): int {
  return 1;
}
|}

let unbound_caller = {|<?hh
function caller(): int {
  return g();
}
|}

let redirected_caller = {|<?hh
function caller(): int {
  return h();
}
|}

let helper = {|<?hh
function h(): int {
  return 1;
}
|}

(** Establish an orphaned g, or index it before removal for the control.
    Every setup write is notified by the second iteration; repair then uses
    completed edits only. *)
let run ~temporary_function ~index_temporary ~repair_files =
  let open Scheduled_fanout in
  run
    ~initialization:Full_check
    {
      initial_files =
        [
          ("Producer.php", empty_producer);
          ("Caller.php", initial_caller);
          ("Helper.php", helper);
        ];
      iterations =
        [
          {
            before_type_check =
              [
                Write_file
                  ( "Producer.php",
                    if index_temporary then
                      temporary_function
                    else
                      empty_producer ^ "// Trigger indexing.\n" );
                Write_file ("Caller.php", unbound_caller);
              ];
            disk_needs_parsing = ["Producer.php"; "Caller.php"];
            after_indexing =
              (if index_temporary then
                []
              else
                [Write_file ("Producer.php", temporary_function)]);
            type_check =
              [
                Check
                  {
                    file = "Caller.php";
                    after_check = [Write_file ("Producer.php", empty_producer)];
                  };
                Check { file = "Producer.php"; after_check = [] };
                Remaining;
              ];
          };
          {
            default_iteration with
            disk_needs_parsing = ["Producer.php"];
            type_check = [Remaining];
          };
          {
            default_iteration with
            (* The repair addresses the only unbound reference. When adding g,
               leave Caller.php untouched to test actual repair fanout. *)
            before_type_check =
              List.map
                (fun (path, contents) -> Write_file (path, contents))
                repair_files;
            disk_needs_parsing = List.map fst repair_files;
            type_check = [Remaining];
          };
        ];
    }

(** Adding a real int g in its original producer must repair the naming error
    even when the orphan's signature is already identical. *)
let test_same_file_same_signature () =
  run
    ~temporary_function:int_function
    ~index_temporary:false
    ~repair_files:[("Producer.php", int_function)];
  true

(** A real int g in a different file must supersede the identically typed
    orphan and cause the unchanged caller's naming error to be cleared. *)
let test_different_file_same_signature () =
  run
    ~temporary_function:int_function
    ~index_temporary:false
    ~repair_files:[("Definition.php", int_function)];
  true

(** Replacing the string ghost with an indexed int definition in its original
    producer must also clear the caller's ghost-induced return-type error. *)
let test_same_file_changed_signature () =
  run
    ~temporary_function:string_function
    ~index_temporary:false
    ~repair_files:[("Producer.php", int_function)];
  true

(** Defining int g elsewhere must replace the string ghost and clear both
    diagnostics without editing the caller to force its recheck. *)
let test_different_file_changed_signature () =
  run
    ~temporary_function:string_function
    ~index_temporary:false
    ~repair_files:[("Definition.php", int_function)];
  true

(** Redirecting the only unbound reference to an existing valid function must
    clear both diagnostics even though the unused ghost remains unindexed. *)
let test_redirect_consumer () =
  run
    ~temporary_function:string_function
    ~index_temporary:false
    ~repair_files:[("Caller.php", redirected_caller)];
  true

(** Indexing the temporary function prevents orphan-cache creation. The same
    ordinary different-file repair must finish with the identical valid source. *)
let test_indexed_control () =
  run
    ~temporary_function:string_function
    ~index_temporary:true
    ~repair_files:[("Definition.php", int_function)];
  true

let () =
  Daemon.check_entry_point ();
  Unit_test.run_all
    [
      ( "temporary_function_removed_with_caller_recheck",
        test_temporary_function_removed_with_caller_recheck );
      ( "indexed_temporary_function_removed",
        test_indexed_temporary_function_removed );
      ( "transient_function_remains_cached",
        test_transient_function_remains_cached );
      ( "transient_constant_remains_cached",
        test_transient_constant_remains_cached );
      ("same_file_same_signature", test_same_file_same_signature);
      ("different_file_same_signature", test_different_file_same_signature);
      ("same_file_changed_signature", test_same_file_changed_signature);
      ("different_file_changed_signature", test_different_file_changed_signature);
      ("redirect_consumer", test_redirect_consumer);
      ("indexed_control", test_indexed_control);
    ]
