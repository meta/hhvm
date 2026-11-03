(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *)

let int_definition = "<?hh\nfunction value(): int { return 1; }\n"

let string_definition = "<?hh\nfunction value(): string { return 'value'; }\n"

let caller = "<?hh\nfunction caller(): int { return value(); }\n"

let initial_files = [("value.php", int_definition); ("caller.php", caller)]

(** Changing the callee after loading a saved state must select its unchanged
    caller through the saved dependency graph. The explicit Check requires the
    caller to be in the computed fanout; it does not add the caller to it.
    Both final checks must report the caller's incompatible return type. *)
let test_saved_dependencies () =
  let open Scheduled_fanout in
  run
    ~initialization:Saved_state
    {
      initial_files;
      iterations =
        [
          {
            default_iteration with
            before_type_check = [Write_file ("value.php", string_definition)];
            disk_needs_parsing = ["value.php"];
            type_check =
              [Check { file = "caller.php"; after_check = [] }; Remaining];
          };
        ];
    };
  true

(** Saved-state loading must leave declarations cold. Change the callee on
    disk but queue only its caller for parsing: checking the caller must load
    the new callee declaration lazily and agree with the fresh check's error.
    An initial warmup check would cache the old callee and hide that error. *)
let test_cold_declarations () =
  let open Scheduled_fanout in
  run
    ~initialization:Saved_state
    {
      initial_files;
      iterations =
        [
          {
            default_iteration with
            before_type_check = [Write_file ("value.php", string_definition)];
            disk_needs_parsing = ["caller.php"];
            type_check = [Remaining];
          };
        ];
    };
  true

(** Saving a repository with an unrelated typing error must accept hh_distc's
    error exit status and retain its error-file set. With no edits or explicit
    parsing inputs, the default iteration must recheck the saved error path;
    otherwise its empty diagnostics would disagree with the fresh check. *)
let test_saved_errors () =
  let open Scheduled_fanout in
  run
    ~initialization:Saved_state
    {
      initial_files =
        [
          ("value.php", int_definition);
          ("broken.php", "<?hh\nfunction broken(): int { return 'wrong'; }\n");
        ];
      iterations = [{ default_iteration with type_check = [Remaining] }];
    };
  true

(** The remote old-declaration fixture must supply a missing comparison
    baseline during an ordinary incremental iteration. A comment-only class
    edit should then leave its unchanged consumer out of the fanout. Without
    the fixture, the cold old declaration is missing and conservative fanout
    includes Use.php, violating the explicit C.php-only checking schedule. *)
let test_remote_old_declarations () =
  let open Scheduled_fanout in
  let class_contents =
    {|<?hh
class C {
  public static function value(): int { return 1; }
}
|}
  in
  run
    ~initialization:Saved_state
    ~remote_old_decls:class_contents
    {
      initial_files =
        [
          ("C.php", class_contents);
          ("Use.php", "<?hh\nfunction use_c(): int { return C::value(); }\n");
        ];
      iterations =
        [
          {
            default_iteration with
            before_type_check =
              [Write_file ("C.php", class_contents ^ "// Recheck C.\n")];
            disk_needs_parsing = ["C.php"];
            type_check = [Check { file = "C.php"; after_check = [] }];
          };
        ];
    };
  true

let () =
  Daemon.check_entry_point ();
  Unit_test.run_all
    [
      ("saved_dependencies", test_saved_dependencies);
      ("cold_declarations", test_cold_declarations);
      ("saved_errors", test_saved_errors);
      ("remote_old_declarations", test_remote_old_declarations);
    ]
