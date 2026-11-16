(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *)

open Hh_prelude
open Scheduled_fanout

(* Defs.php is both the saved-state source and the remote fixture. The four
   tested declarations have independent consumers; unrelated() has none. *)
let definitions =
  {|<?hh
namespace Remote {
  new module remote.test {}
  function Shared(): int { return 1; }
  type Alias = int;
  const int Shared = 1;
  function unrelated(): int { return 1; }
}
|}

(* Separate files make a missed recovery for any one kind visible in the
   checking schedule. The same-named function and constant have different
   dependency kinds. *)
let consumers =
  [
    ( "UseConst.php",
      "<?hh\nfunction use_const(): int { return \\Remote\\Shared; }\n" );
    ( "UseFun.php",
      "<?hh\nfunction use_fun(): int { return \\Remote\\Shared(); }\n" );
    ( "UseModule.php",
      "<?hh\nmodule remote.test;\ninternal function use_module(): void {}\n" );
    ( "UseType.php",
      "<?hh\nfunction use_type(\\Remote\\Alias $x): int { return $x; }\n" );
  ]

let initial_files = ("Defs.php", definitions) :: consumers

let consumer_files = List.map consumers ~f:fst

(* Evict only the declarations under test, retaining unrelated() locally. *)
let decl_names : File_info.names =
  {
    File_info.empty_names with
    n_funs = S_set.singleton "\\Remote\\Shared";
    n_types = S_set.singleton "\\Remote\\Alias";
    n_consts = S_set.singleton "\\Remote\\Shared";
    n_modules = S_set.singleton "remote.test";
  }

let changed_function =
  String.substr_replace_all
    definitions
    ~pattern:"function Shared(): int"
    ~with_:"function Shared(): num"

(** A cold saved-state cache must recover each non-class kind. The function
    and constant share a name, but have separate consumers. Excluding all four
    consumers from checking requires successful recovery for every kind. *)
let test_cold_recovery () : bool =
  (* Saved_state loads naming and dependencies without warming declaration
     caches. The fixture supplies the original contents of Defs.php. *)
  run
    ~initialization:Saved_state
    ~remote_old_decls:[("Defs.php", definitions)]
    ~remote_old_decls_include_non_class:true
    ~expected_diagnostics:[]
    {
      initial_files;
      iterations =
        [
          {
            default_iteration with
            (* Appending a comment preserves every declaration's position/hash. *)
            before_type_check =
              [Write_file ("Defs.php", definitions ^ "// comment-only edit\n")];
            disk_needs_parsing = ["Defs.php"];
            (* No Remaining: any consumer selected for checking fails the test. *)
            type_check = [Check { file = "Defs.php"; after_check = [] }];
          };
        ];
    };
  true

(** If the fixture has no old declarations, all four unchanged consumers must
    be checked conservatively. Their final diagnostics still agree with a full
    check, so the explicit checking schedule establishes the fallback behavior. *)
let test_missing_remote_declarations () : bool =
  (* An empty list still enables fixture mode. Its folder contains no blobs,
     so missing files must not fall through to an actual CAS download. *)
  run
    ~initialization:Saved_state
    ~remote_old_decls:[]
    ~remote_old_decls_include_non_class:true
    ~expected_diagnostics:[]
    {
      initial_files;
      iterations =
        [
          {
            default_iteration with
            before_type_check =
              [Write_file ("Defs.php", definitions ^ "// comment-only edit\n")];
            disk_needs_parsing = ["Defs.php"];
            (* The declarations are unchanged, but their missing old versions
               require all four consumers even though diagnostics remain clean. *)
            type_check =
              [
                Check { file = "Defs.php"; after_check = [] };
                Check { file = "UseConst.php"; after_check = [] };
                Check { file = "UseFun.php"; after_check = [] };
                Check { file = "UseModule.php"; after_check = [] };
                Check { file = "UseType.php"; after_check = [] };
              ];
          };
        ];
    };
  true

(** A file change must not prevent recovery of individually unchanged decls.
    After a real incremental edit puts Defs.php in the naming overlay, evict all
    four unchanged declarations. Their overlay hashes must still authorize
    saved-state recovery, leaving every consumer out of the next fanout. *)
let test_evicted_declarations_after_file_change () : bool =
  let changed_file =
    String.substr_replace_all
      definitions
      ~pattern:"function unrelated(): int"
      ~with_:"function unrelated(): num"
  in
  run
    ~initialization:Saved_state
    ~remote_old_decls:[("Defs.php", definitions)]
    ~remote_old_decls_include_non_class:true
    ~expected_diagnostics:[]
    {
      initial_files;
      iterations =
        [
          (* Checking the consumers populates the four declaration caches. *)
          { default_iteration with disk_needs_parsing = consumer_files };
          {
            default_iteration with
            (* Replace the SQLite-backed file info with an overlay entry. Only
               unrelated() changes; the tested decl hashes still match SQLite. *)
            before_type_check = [Write_file ("Defs.php", changed_file)];
            disk_needs_parsing = ["Defs.php"];
            type_check = [Check { file = "Defs.php"; after_check = [] }];
          };
          {
            default_iteration with
            (* The old declarations now go missing while their expected hashes
               come from the previous iteration's updated naming overlay. *)
            before_type_check =
              [
                Evict_decls decl_names;
                Write_file ("Defs.php", changed_file ^ "// comment-only edit\n");
              ];
            disk_needs_parsing = ["Defs.php"];
            (* Recovery must exclude the consumers despite Defs.php having
               changed since the saved state. *)
            type_check = [Check { file = "Defs.php"; after_check = [] }];
          };
        ];
    };
  true

(** Local old declarations remain usable after changing beyond the saved state.
    Keep the changed function cached, but evict the other three kinds. The next
    comment-only edit must combine the local function with remote declarations
    without rechecking any consumer. The function consumer accepts num, keeping
    it valid throughout so error retries cannot require an unrelated recheck. *)
let test_local_and_remote_declarations () : bool =
  let accept_num (file, contents) =
    if String.equal file "UseFun.php" then
      ( file,
        String.substr_replace_all contents ~pattern:"(): int" ~with_:"(): num"
      )
    else
      (file, contents)
  in
  (* Accept both int and num so stale-error position updates cannot independently
     schedule UseFun.php and conceal an incorrect declaration comparison. *)
  let initial_files = List.map initial_files ~f:accept_num in
  run
    ~initialization:Saved_state
    ~remote_old_decls:[("Defs.php", definitions)]
    ~remote_old_decls_include_non_class:true
    ~expected_diagnostics:[]
    {
      initial_files;
      iterations =
        [
          (* Warm the original declarations through their actual consumers. *)
          { default_iteration with disk_needs_parsing = consumer_files };
          {
            default_iteration with
            (* int and num have equal lengths, preserving the other decls'
               positions and hashes while changing only the function signature. *)
            before_type_check = [Write_file ("Defs.php", changed_function)];
            disk_needs_parsing = ["Defs.php"];
          };
          {
            default_iteration with
            (* Shared(): num stays cached. The other three old decls must come
               from a blob that also contains the now-stale Shared(): int;
               that unrequested function must not replace the local old value. *)
            before_type_check =
              [
                Evict_decls { decl_names with n_funs = S_set.empty };
                Write_file
                  ("Defs.php", changed_function ^ "// comment-only edit\n");
              ];
            disk_needs_parsing = ["Defs.php"];
            (* No consumer checks: the local function matches the current num
               signature, and the three remotely recovered signatures match too. *)
            type_check = [Check { file = "Defs.php"; after_check = [] }];
          };
        ];
    };
  true

(** An evicted declaration that changed since the saved state must not load
    its stale version. Change the function to num, then evict it and revert it
    to int. Accepting saved int as "old" would hide the change and omit UseFun.php
    from fanout. The consumer accepts num throughout, so existing diagnostics
    cannot independently trigger the expected recheck. *)
let test_reject_stale_saved_state_declaration () : bool =
  let accept_num (file, contents) =
    if String.equal file "UseFun.php" then
      ( file,
        String.substr_replace_all contents ~pattern:"(): int" ~with_:"(): num"
      )
    else
      (file, contents)
  in
  (* Keep this consumer valid through both signature changes; only declaration
     fanout should cause its final check. *)
  let initial_files = List.map initial_files ~f:accept_num in
  run
    ~initialization:Saved_state
    ~remote_old_decls:[("Defs.php", definitions)]
    ~remote_old_decls_include_non_class:true
    ~expected_diagnostics:[]
    {
      initial_files;
      iterations =
        [
          (* Establish an actual cached int declaration before editing it. *)
          { default_iteration with disk_needs_parsing = consumer_files };
          {
            default_iteration with
            (* The next iteration's expected old hash now describes num, while
               the saved naming DB and remote blob still describe int. *)
            before_type_check = [Write_file ("Defs.php", changed_function)];
            disk_needs_parsing = ["Defs.php"];
          };
          {
            default_iteration with
            (* With num evicted, accepting remote int as old would compare
               int against int and incorrectly omit the function consumer. *)
            before_type_check =
              [Evict_decls decl_names; Write_file ("Defs.php", definitions)];
            disk_needs_parsing = ["Defs.php"];
            (* Only the function changed. The other evicted declarations still
               match the saved state and must be recovered without fanout. *)
            type_check =
              [
                Check { file = "Defs.php"; after_check = [] };
                Check { file = "UseFun.php"; after_check = [] };
              ];
          };
        ];
    };
  true

(** Initialization must recover the unchanged function's old declaration using
    its saved hash, even when another declaration in the same file is removed.
    Checking only Defs.php proves the unchanged consumer was left out of fanout. *)
let test_non_class_recovery_during_init () : bool =
  let kept = "<?hh\nfunction kept(): int { return 1; }\n" in
  (* Delete a declaration after kept(), preserving kept()'s positions/hash but
     changing the file's declaration hash so init must compute its fanout. *)
  let original = kept ^ "function removed(): void {}\n" in
  run
    ~initialization:
      (Saved_state_with_loading_schedule
         {
           (* The edit is included in startup's dirty-file snapshot. *)
           before_dirty_snapshot = [Write_file ("Defs.php", kept)];
           after_dirty_snapshot = [];
         })
    ~remote_old_decls:[("Defs.php", original)]
    ~remote_old_decls_include_non_class:true
    ~expected_diagnostics:[]
    {
      initial_files =
        [
          ("Defs.php", original);
          ("Use.php", "<?hh\nfunction use_kept(): int { return kept(); }\n");
        ];
      iterations =
        [
          {
            default_iteration with
            (* Drain the checks queued by startup without another edit that
               could repair or obscure its fanout. *)
            (* No Remaining: the caller must not be selected for checking. *)
            type_check = [Check { file = "Defs.php"; after_check = [] }];
          };
        ];
    };
  true

(** A parent-class edit must not replace the child's current declaration with
    its stale saved-state version. Check each edit prefix independently, so a
    later edit cannot hide incorrect intermediate diagnostics. *)
let test_stale_remote_old_decl_fanout () : bool =
  let base =
    {|<?hh // strict

class Base {
  public function marker(): void {}
}
|}
  in
  let changed_base =
    {|<?hh // strict

class Base {
  public function marker(): void {}
  private function unrelated(): void {}
}
|}
  in
  let saved_decl =
    {|<?hh // strict

class AlertThreshold extends Base {
  public static function getStructMetadata(): string {
    return "saved";
  }
}
|}
  in
  let local_decl =
    {|<?hh // strict

class AlertThreshold extends Base {
  public static function getStructMetadata(): int {
    return 1;
  }
}
|}
  in
  let saved_caller =
    {|<?hh // strict

function take_string(string $_): void {}

function caller(): void {
  take_string(AlertThreshold::getStructMetadata());
}
|}
  in
  let local_caller =
    {|<?hh // strict

function take_int(int $_): void {}

function caller(): void {
  take_int(AlertThreshold::getStructMetadata());
}
|}
  in
  let initial_files =
    [("base.php", base); ("decl.php", saved_decl); ("caller.php", saved_caller)]
  in
  (* Replace the saved string signature and its caller together. *)
  let change_child =
    {
      default_iteration with
      before_type_check =
        [
          Write_file ("decl.php", local_decl);
          Write_file ("caller.php", local_caller);
        ];
      disk_needs_parsing = ["decl.php"; "caller.php"];
    }
  in
  (* Invalidate descendants' folded declarations while preserving their
     current shallow declarations, matching the original integration test. *)
  let change_parent =
    {
      default_iteration with
      before_type_check = [Write_file ("base.php", changed_base)];
      disk_needs_parsing = ["base.php"];
    }
  in
  let revert_child =
    {
      default_iteration with
      before_type_check = [Write_file ("decl.php", saved_decl)];
      disk_needs_parsing = ["decl.php"];
    }
  in
  let check iterations expected_diagnostics =
    run
      ~initialization:
        (Saved_state_with_loading_schedule
           { before_dirty_snapshot = []; after_dirty_snapshot = [] })
      ~remote_old_decls:[("decl.php", saved_decl)]
      ~disable_rust_provider_shallow_decl_invalidation:true
      ~expected_diagnostics
      { initial_files; iterations }
  in
  (* Both the initial signature change and the unrelated parent edit must
     leave the caller valid. Each prefix gets its own saved-state load. *)
  check [change_child] [];
  check [change_child; change_parent] [];
  (* Reverting the method to string must recheck its now-incompatible caller. *)
  check
    [change_child; change_parent; revert_child]
    [("caller.php", 4110, "Invalid argument")];
  true

let () =
  Daemon.check_entry_point ();
  Unit_test.run_all
    [
      ("stale remote old decl fanout", test_stale_remote_old_decl_fanout);
      ("cold recovery", test_cold_recovery);
      ("missing remote declarations", test_missing_remote_declarations);
      ( "evicted declarations after file change",
        test_evicted_declarations_after_file_change );
      ("local and remote declarations", test_local_and_remote_declarations);
      ( "reject stale saved-state declaration",
        test_reject_stale_saved_state_declaration );
      ("non-class recovery during init", test_non_class_recovery_during_init);
    ]
