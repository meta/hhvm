(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *)

(* This bug loses track of consumers checked while class C's definition was temporarily missing
   (e.g., file deleted, then restored later). "Consumers" are just functions referring to C.
   After C's definition is restored , its signature matches the cached old declaration. THis causes
   us to miss the required fanout, causing false errors. *)

let class_contents = {|<?hh
class C {
  public function foo(): void {}
}
|}

let class_contents_plus_comment =
  class_contents ^ "// Trigger an incremental check.\n"

let test_missing_class_after_restore () =
  let scenario =
    Scheduled_fanout.
      {
        (* Establish a successful baseline: C is cached, and both consumers
           have dependency edges to it. Neither consumer has diagnostics. *)
        initial_files =
          [
            ("ZC.php", class_contents);
            ( "AUse.php",
              {|<?hh
function use_c(): void {
  $_ = meth_caller(C::class, 'foo');
}
|}
            );
            ( "Late.php",
              {|<?hh
function late_c(): void {
  $_ = meth_caller(C::class, 'foo');
}
|}
            );
          ];
        iterations =
          [
            (* First iteration: check a consumer against missing C, then let
               a later consumer repopulate C's declaration cache. *)
            {
              (* 1. Make a comment-only edit and queue ZC.php for parsing.
                 Indexing will still see C; its name and signature have
                 not changed. *)
              before_type_check =
                [Write_file ("ZC.php", class_contents_plus_comment)];
              disk_needs_parsing = ["ZC.php"];
              (* 2. Remove C from disk after indexing. Naming still says that
                 C lives here, but subsequent declaration reads find no C. *)
              after_indexing = [Write_file ("ZC.php", "<?hh\n")];
              type_check =
                [
                  (* 3. Redeclaration sees the cached old C and no current C,
                     producing New_decl_not_found and scheduling its consumers.
                     Let AUse.php finish while C is absent: it gets a false
                     "Unbound name (typing): C" diagnostic. *)
                  Check
                    {
                      file = "AUse.php";
                      after_check =
                        [
                          (* 4. Restore exactly the contents that indexing saw.
                             This does not queue another parse or populate
                             the declaration cache. *)
                          Write_file ("ZC.php", class_contents_plus_comment);
                        ];
                    };
                  (* 5. Late.php now looks up C successfully and caches it. That
                     cache refill must not erase the need to repair AUse.php. *)
                  Check { file = "Late.php"; after_check = [] };
                  (* 6. Check ZC.php itself after restoration, then finish this
                     iteration. AUse.php's completed check is not retried. *)
                  Check { file = "ZC.php"; after_check = [] };
                  Remaining;
                ];
            };
            (* Second iteration: reparse ZC.php with the file back in its
               final, valid state. *)
            {
              default_iteration with
              (* 7. Queue ZC.php for parsing. Recovery oldifies the C loaded by
                 Late.php and compares it with the identical disk declaration.
                 The buggy comparison computes no class-change fanout,
                 leaving AUse.php unchecked. *)
              disk_needs_parsing = ["ZC.php"];
              type_check = [Remaining];
            };
          ];
      }
  in
  Scheduled_fanout.run_expect_bug
    ~bug:"missing-class recovery after local cache refill"
    ~incremental:[("AUse.php", 4107, "Unbound name (typing): `C`")]
    ~full:[]
    scenario;
  true

let () =
  Daemon.check_entry_point ();
  Unit_test.run_all
    [("missing_class_after_restore", test_missing_class_after_restore)]
