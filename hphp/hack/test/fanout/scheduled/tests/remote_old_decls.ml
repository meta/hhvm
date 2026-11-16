(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *)

open Scheduled_fanout

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
    [("stale remote old decl fanout", test_stale_remote_old_decl_fanout)]
