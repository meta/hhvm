(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *)

open Scheduled_fanout

(** Parse explicit fixture contents to compute expected symbol names and hashes. *)
let file_info ~(file : string) (contents : string) : File_info.t =
  let path = Relative_path.from_root ~suffix:file in
  let parsed =
    Direct_decl_parser.parse_and_hash_decls
      (Decl_parser_options.from_parser_options Parser_options.default)
      false
      path
      contents
  in
  Direct_decl_parser.decls_to_fileinfo path parsed

let old_names ~(file : string) (contents : string) : File_info.Old_names.t =
  File_info.Old_names.of_ids (file_info ~file contents).File_info.ids

let names ~(file : string) (contents : string) : File_info.names =
  File_info.simplify (file_info ~file contents)

(** Removing a losing duplicate preserves every old symbol and the previous
    winner's hash for functions, classes, typedefs, constants, and modules. *)
let test_duplicate_winner_hashes () : bool =
  let winner =
    "<?hh\nfunction winner_fun(): int { return 1; }\n"
    ^ "class WinnerClass { const int VALUE = 1; }\n"
    ^ "type WinnerType = int;\nconst int WINNER_CONST = 1;\n"
    ^ "new module WinnerModule {}\n"
  in
  let loser =
    "<?hh\nfunction winner_fun(): string { return ''; }\n"
    ^ "class WinnerClass { const string VALUE = ''; }\n"
    ^ "type WinnerType = string;\nconst string WINNER_CONST = '';\n"
    ^ "new module WinnerModule {}\n"
  in
  let old_winner = old_names ~file:"a.php" winner in
  let winner_names = names ~file:"a.php" winner in
  run
    ~remote_old_decls_include_non_class:true
    ~expected_diagnostics:[]
    {
      initial_files = [("a.php", winner); ("z.php", loser)];
      iterations =
        [
          {
            default_iteration with
            before_type_check = [Write_file ("z.php", "<?hh\n")];
            disk_needs_parsing = ["z.php"];
            expected_versioned_names =
              Some
                [
                  ("a.php", { old_names = old_winner; new_names = winner_names });
                  ( "z.php",
                    {
                      old_names = old_winner;
                      new_names = File_info.empty_names;
                    } );
                ];
          };
        ];
    };
  true

(** Removing the winner compares against its old hash, even though naming
    promotes a different declaration. A later edit observes the promoted hash. *)
let test_snapshot_survives_new_winner () : bool =
  let winner = "<?hh\nclass Promoted { const int VALUE = 1; }\n" in
  let loser = "<?hh\nclass Promoted { const string VALUE = ''; }\n" in
  let old_winner = old_names ~file:"a.php" winner in
  let promoted = old_names ~file:"z.php" loser in
  let promoted_names = names ~file:"z.php" loser in
  run
    ~remote_old_decls_include_non_class:true
    ~expected_diagnostics:[("use.php", 4110, "Invalid return type")]
    {
      initial_files =
        [
          ("a.php", winner);
          ("z.php", loser);
          ( "use.php",
            "<?hh\nfunction use_promoted(): int { return Promoted::VALUE; }\n"
          );
        ];
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
                      old_names = old_winner;
                      new_names = File_info.empty_names;
                    } );
                  ( "z.php",
                    { old_names = old_winner; new_names = promoted_names } );
                ];
          };
          {
            default_iteration with
            before_type_check = [Write_file ("z.php", loser ^ "// edit\n")];
            disk_needs_parsing = ["z.php"];
            expected_versioned_names =
              Some
                [
                  ("z.php", { old_names = promoted; new_names = promoted_names });
                ];
          };
        ];
    };
  true

(** New case-insensitive function/type conflicts include the unchanged owner
    and all its old symbols. Constant/module case variants do not select owners,
    and the newly added file has no old symbols. *)
let test_new_conflict_owner_metadata () : bool =
  (* The baseline owns Collision in both the function and type namespaces.
     The unrelated declarations check that we capture the whole owner's file. *)
  let owner =
    "<?hh\nfunction Collision(): void {}\nclass Collision {}\n"
    ^ "type UnrelatedType = int;\nconst int UNRELATED = 1;\n"
    ^ "new module Unrelated {}\n"
  in
  (* Function names are case-insensitive, and classes and typedefs share the
     case-insensitive type namespace. owner.php already owns both bindings and
     sorts before znew.php, so its definitions win throughout this scenario. *)
  let added =
    "<?hh\nfunction collision(): void {}\ntype collision = string;\n"
    ^ "const int case_sensitive = 1;\nnew module casesensitive {}\n"
  in
  run
    ~remote_old_decls_include_non_class:true
    ~expected_diagnostics:
      [
        (* The losing function and typedef each report the same primary message. *)
        ("znew.php", 2012, "Name already bound: `collision`");
        ("znew.php", 2012, "Name already bound: `collision`");
      ]
    {
      initial_files =
        [
          ("owner.php", owner);
          (* Constants and modules use case-sensitive names. The new variants
             must not bring this file into the redeclaration set. *)
          ( "case_sensitive.php",
            "<?hh\nconst int CASE_SENSITIVE = 1;\nnew module CaseSensitive {}\n"
          );
        ];
      iterations =
        [
          {
            default_iteration with
            (* Only the new file is initially queued. Naming discovers its
               conflicts and adds the unchanged owner.php through failed_naming. *)
            before_type_check = [Write_file ("znew.php", added)];
            disk_needs_parsing = ["znew.php"];
            (* Check the complete redeclaration map, including the absence of
               case_sensitive.php. The owner's old metadata must have been
               captured before naming discovered the new conflicts. *)
            expected_versioned_names =
              Some
                [
                  ( "owner.php",
                    {
                      (* Include hashes for all old symbols, even those that
                         did not conflict. The file's new names are unchanged. *)
                      old_names = old_names ~file:"owner.php" owner;
                      new_names = names ~file:"owner.php" owner;
                    } );
                  ( "znew.php",
                    {
                      (* These are new symbols in a new file, so its old names
                         are empty despite the conflicts with existing names. *)
                      old_names = File_info.Old_names.empty;
                      new_names = names ~file:"znew.php" added;
                    } );
                ];
          };
        ];
    };
  true

(** A Rust-written saved state retains an in-file duplicate's selected hash in
    NAMING_SYMBOLS, with the other declaration in overflow. Removing both
    declarations must recover one old symbol with that primary hash from SQLite. *)
let test_database_primary_hash () : bool =
  let first = "<?hh\nfunction primary_fun(): void {}\n" in
  let duplicate = "function primary_fun(): int { return 1; }\n" in
  run
    ~remote_old_decls_include_non_class:true
    ~initialization:Saved_state
    ~expected_diagnostics:[]
    {
      (* The actual saved-state writer handles the two known declaration hashes. *)
      initial_files = [("primary.php", first ^ duplicate)];
      iterations =
        [
          {
            default_iteration with
            (* Remove the definitions before any incremental recheck can replace
               the SQLite-backed old metadata with an in-memory overlay. *)
            before_type_check = [Write_file ("primary.php", "<?hh\n")];
            disk_needs_parsing = ["primary.php"];
            expected_versioned_names =
              Some
                [
                  ( "primary.php",
                    {
                      (* Both old occurrences map to the first declaration's
                         hash; the deleted file contributes no new names. *)
                      old_names = old_names ~file:"primary.php" first;
                      new_names = File_info.empty_names;
                    } );
                ];
          };
        ];
    };
  true

let () =
  Daemon.check_entry_point ();
  Unit_test.run_all
    [
      ("duplicate winner hashes", test_duplicate_winner_hashes);
      ("snapshot survives new winner", test_snapshot_survives_new_winner);
      ("new conflict owner metadata", test_new_conflict_owner_metadata);
      ("database primary hash", test_database_primary_hash);
    ]
