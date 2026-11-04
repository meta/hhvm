(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *)

(* Symptom: When checking a file that uses an inherited member, the type-checker
   can use a definition that is no longer on disk. Rechecking the file can still
   produce diagnostics that disagree with a fresh check of the current files.

   Cause: Recursive folding can cache declarations outside the redeclaration
   batch, so cleanup of that batch does not remove them. When the old and new
   shallow declarations compare equal, invalidation driven by shallow changes
   also leaves these entries in the cache. *)

(** Folding C while P temporarily has a string constant leaves D with that
    type after P is restored. A later consumer recheck then misses the error
    from returning the restored int constant as string. *)
let test_indirect_ancestor_retains_transient_constant () =
  let initial_parent = {|<?hh
class P {
  const int K = 1;
}
|} in
  let parent =
    {|<?hh
class P {
  const int K = 1;
  private function untouched(): void {}
}
|}
  in
  let transient_parent =
    {|<?hh
class P {
  const string K = 'value';
  private function untouched(): void {}
}
|}
  in
  let descendant = {|<?hh
class D extends P {}
|} in
  let child = {|<?hh
class C extends D {}
|} in
  let consumer = {|<?hh
function use_d(): int {
  return D::K;
}
|} in
  let changed_consumer =
    {|<?hh
function use_d(): string {
  return D::K;
}
|}
  in
  let comment = "// Trigger an incremental check.\n" in
  let scenario =
    Scheduled_fanout.
      {
        (* A full check establishes all inheritance edges and warms the caches. *)
        initial_files =
          [
            ("ZChild.php", child);
            ("D.php", descendant);
            ("P.php", initial_parent);
            ("Use.php", consumer);
          ];
        iterations =
          [
            {
              default_iteration with
              (* An unused private member invalidates P's folded descendants,
                 but only P needs rechecking. This leaves D cold while
                 preserving its inheritance dependency edges. *)
              before_type_check = [Write_file ("P.php", parent)];
              disk_needs_parsing = ["P.php"];
              type_check = [Check { file = "P.php"; after_check = [] }];
            };
            {
              (* Redeclaration visits ZChild.php before P.php. Folding C loads
                 its parent D, whose file is outside this batch, so direct
                 oldification and cleanup do not remove D's folded entry. *)
              before_type_check =
                [
                  Write_file ("ZChild.php", child ^ comment);
                  Write_file ("P.php", parent ^ comment);
                  Write_file ("Use.php", changed_consumer);
                ];
              disk_needs_parsing = ["ZChild.php"; "P.php"; "Use.php"];
              (* Old declarations still describe the int constant. Folding C
                 reads the transient string version through D before P is
                 explicitly redeclared. Keeping P stable here would avoid
                 caching a transient version. *)
              after_indexing =
                [Write_file ("P.php", transient_parent ^ comment)];
              after_redeclaring =
                [
                  ( "ZChild.php",
                    (* P's explicit reparse restores its shallow int
                       declaration. D retains the string constant, but shallow
                       comparison sees no change. Applying P's changes only
                       between completed iterations would let comparison detect
                       them and invalidate D. *)
                    [Write_file ("P.php", parent ^ comment)] );
                ];
              type_check = [Remaining];
            };
            {
              default_iteration with
              (* Deliver all pending saves and explicitly check Use again. *)
              before_type_check =
                [
                  Write_file ("P.php", parent ^ comment);
                  Write_file ("Use.php", changed_consumer ^ comment);
                ];
              disk_needs_parsing = ["P.php"; "Use.php"];
              type_check = [Remaining];
            };
          ];
      }
  in
  Scheduled_fanout.run_expect_bug
    ~bug:"F8: mixed declaration versions"
    ~incremental:[]
    ~full:[("Use.php", 4110, "Invalid return type")]
    scenario;
  true

let () =
  Daemon.check_entry_point ();
  Unit_test.run_all
    [
      ( "indirect_ancestor_retains_transient_constant",
        test_indirect_ancestor_retains_transient_constant );
    ]
