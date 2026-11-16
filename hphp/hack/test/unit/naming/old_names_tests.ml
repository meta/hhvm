(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *)

open Hh_prelude

module Hash_asserter = Asserter.Make_asserter (struct
  type t = File_info.Decl_hash.t

  let to_string = File_info.Decl_hash.show

  let is_equal = File_info.Decl_hash.equal
end)

type test_env = {
  ctx: Provider_context.t;
  baseline: Naming_table.t;
  file_infos: File_info.t Relative_path.Map.t;
}

let parse (path : Relative_path.t) (contents : string) :
    Direct_decl_parser.parsed_file_with_hashes =
  Direct_decl_parser.parse_and_hash_decls
    (Decl_parser_options.from_parser_options Parser_options.default)
    false
    path
    contents

let file_info (path : Relative_path.t) (contents : string) : File_info.t =
  Direct_decl_parser.decls_to_fileinfo path (parse path contents)

(** Each test has a fresh Rust backend and real SQLite naming baseline. Fixture
    parsing supplies hashes without populating declaration stores. *)
let with_env (files : (string * string) list) (f : test_env -> unit) : bool =
  let run dir =
    Relative_path.set_path_prefix Relative_path.Root dir;
    Relative_path.set_path_prefix Relative_path.Hhi dir;
    Relative_path.set_path_prefix Relative_path.Tmp dir;
    let backend =
      Hh_server_provider_backend.make
        (Decl_fold_options.from_global_options Global_options.default)
        (Decl_parser_options.from_parser_options Parser_options.default)
    in
    Provider_backend.set_rust_backend backend;
    let ctx =
      Provider_context.empty_for_tool
        ~popt:Parser_options.default
        ~tcopt:Global_options.default
        ~backend:(Provider_backend.get ())
        ~deps_mode:(Typing_deps_mode.InMemoryMode None)
    in
    let parse_file (suffix, contents) =
      let path = Relative_path.from_root ~suffix in
      (path, file_info path contents)
    in
    let file_infos = Relative_path.Map.of_list (List.map files ~f:parse_file) in
    let db_path = Path.to_string (Path.concat dir "naming.sqlite") in
    let saved = Naming_table.save (Naming_table.create file_infos) db_path in
    assert (List.is_empty saved.Naming_sqlite.errors);
    let baseline = Naming_table.load_from_sqlite ctx db_path in
    let check () =
      f { ctx; baseline; file_infos };
      true
    in
    let cleanup () =
      let remove_file path info =
        let names = File_info.simplify info in
        Rust_provider_backend.Decl.remove_defs backend names;
        Rust_provider_backend.Decl.remove_old_defs backend names;
        Naming_provider.update
          ~backend:(Provider_context.get_backend ctx)
          ~path
          ~old_ids:(Some info.File_info.ids)
          ~new_ids:None
      in
      Relative_path.Map.iter file_infos ~f:remove_file;
      Naming_sqlite.free_db_cache ()
    in
    Exn.protect ~f:check ~finally:cleanup
  in
  Tempfile.with_real_tempdir run

let assert_winner_hashes
    (winner : File_info.t) (old_names : File_info.Old_names.t) : unit =
  let check ids hashes =
    Asserter.Int_asserter.assert_equals
      (List.length ids)
      (S_map.cardinal hashes)
      "all old symbols are retained";
    let check_id id =
      let expected = Option.value_exn id.File_info.decl_hash in
      Hash_asserter.assert_option_equals
        (Some expected)
        (S_map.find id.File_info.name hashes)
        ("old winning hash for " ^ id.File_info.name)
    in
    List.iter ids ~f:check_id
  in
  let { File_info.funs; classes; typedefs; consts; modules } =
    winner.File_info.ids
  in
  check funs old_names.File_info.Old_names.n_funs;
  check classes old_names.File_info.Old_names.n_classes;
  check typedefs old_names.File_info.Old_names.n_types;
  check consts old_names.File_info.Old_names.n_consts;
  check modules old_names.File_info.Old_names.n_modules

(** Old records for duplicate symbols carry the same winning hashes. Merging
    those records must preserve their symbol keys and matching hashes. *)
let test_merge_winner_hashes () : bool =
  let contents =
    "<?hh\nfunction winner_fun(): int { return 1; }\nclass WinnerClass {}\n"
    ^ "type WinnerType = int;\nconst int WINNER_CONST = 1;\n"
    ^ "new module WinnerModule {}\n"
  in
  let run env =
    (* The fixture's SQLite baseline contains only winner.php, which owns all
       five symbols in reverse naming. Parsing here does not populate decl heaps. *)
    let winner_path = Relative_path.from_root ~suffix:"winner.php" in
    let loser_path = Relative_path.from_root ~suffix:"loser.php" in
    let winner = Relative_path.Map.find env.file_infos winner_path in
    let loser = file_info loser_path contents in
    (* Add the duplicate to the forward overlay while reverse naming still
       selects winner.php. This bypasses normal naming-conflict tracking. *)
    let previous = Naming_table.update env.baseline loser_path loser in
    (* Include both conflicting files, as required by get_old_names_batch.
       The winner is unchanged and the loser removes its declarations. *)
    let snapshot =
      Server_incremental.get_old_names_batch
        env.ctx
        previous
        ~defs_per_file_parsed:
          (Relative_path.Map.of_list
             [(winner_path, winner); (loser_path, File_info.empty_t)])
    in
    Asserter.Int_asserter.assert_equals
      2
      (Relative_path.Map.cardinal snapshot)
      "both conflicting files belong to this snapshot";
    let winner_names = Relative_path.Map.find snapshot winner_path in
    let loser_names = Relative_path.Map.find snapshot loser_path in
    assert_winner_hashes winner winner_names;
    assert_winner_hashes winner loser_names;
    (* Matching hashes must survive merging old records from different files. *)
    assert_winner_hashes
      winner
      (File_info.Old_names.merge winner_names loser_names)
  in
  with_env [("winner.php", contents)] run

(** Exercise first-occurrence selection and declaration-kind matching in an
    old file's forward overlay. The first function/constant occurrences remain
    the expected definitions; a later duplicate must not supply their hashes.
    An explicitly unknown first hash must remain unknown, even when another
    occurrence or SQLite has a known hash.

    The class owns Guarded in the shared type namespace, so the losing typedef
    retains its name with an unknown hash while the class keeps its own hash.
    This is a direct helper test with synthetic metadata, including an injected
    [None] hash; it does not run naming or oldify declarations. *)
let test_overlay_first_hash () : bool =
  let contents =
    "<?hh\nclass Guarded {}\nfunction guarded_fun(): void {}\n"
    ^ "const int UNKNOWN = 1;\n"
  in
  let run env =
    let path = Relative_path.from_root ~suffix:"guarded.php" in
    (* Append duplicates after the baseline declarations. The first function
       returns void and the second returns int; the first constant is int and
       the second is string. The typedef also conflicts with the class. *)
    let info =
      file_info
        path
        (contents
        ^ "function guarded_fun(): int { return 1; }\ntype Guarded = int;\n"
        ^ "const string UNKNOWN = '';\n")
    in
    let ids = info.File_info.ids in
    (* Make only the selected first constant's hash unknown. The second
       occurrence and the original SQLite declaration still have known hashes. *)
    let consts =
      match ids.File_info.consts with
      | first :: rest -> { first with File_info.decl_hash = None } :: rest
      | [] -> failwith "expected repeated constant declarations"
    in
    let info = { info with File_info.ids = { ids with File_info.consts } } in
    (* Treat this metadata as the old contents of guarded.php. Reverse naming
       still selects the baseline bindings; only the forward overlay changes. *)
    let previous = Naming_table.update env.baseline path info in
    let snapshot =
      Server_incremental.get_old_names_batch
        env.ctx
        previous
        ~defs_per_file_parsed:(Relative_path.Map.singleton path info)
    in
    let old_names = Relative_path.Map.find snapshot path in
    (* Select the first occurrence, rather than the later int-returning function. *)
    let fun_id = List.hd_exn info.File_info.ids.File_info.funs in
    Hash_asserter.assert_option_equals
      fun_id.File_info.decl_hash
      (S_map.find "\\guarded_fun" old_names.File_info.Old_names.n_funs)
      "the first function occurrence supplies the old hash";
    (* Do not replace an unknown selected hash with a known duplicate or DB hash. *)
    Hash_asserter.assert_option_equals
      None
      (S_map.find "\\UNKNOWN" old_names.File_info.Old_names.n_consts)
      "the first constant's unknown hash is not replaced by a later or database hash";
    (* The type winner is a class. Neither its hash nor the losing typedef's
       own hash is a valid expected old typedef hash. *)
    Hash_asserter.assert_option_equals
      None
      (S_map.find "\\Guarded" old_names.File_info.Old_names.n_types)
      "a typedef cannot borrow the winning class hash";
    (* The class's matching kind allows its known hash to be retained. *)
    let class_id = List.hd_exn info.File_info.ids.File_info.classes in
    Hash_asserter.assert_option_equals
      class_id.File_info.decl_hash
      (S_map.find "\\Guarded" old_names.File_info.Old_names.n_classes)
      "the unique class retains its own hash"
  in
  with_env [("guarded.php", contents)] run

let () =
  let config =
    {
      Shared_mem.default_config with
      Shared_mem.shm_use_sharded_hashtbl = true;
      shm_cache_size = 2 * 1024 * 1024 * 1024;
      global_size = 1024;
      heap_size = 1024 * 1024;
      hash_table_pow = 12;
      shm_dirs = [];
      shm_min_avail = 0;
    }
  in
  let (_ : Shared_mem.handle) = Shared_mem.init config ~num_workers:0 in
  Event_logger.init_fake ();
  Unit_test.run_all
    [
      ("merge winner hashes", test_merge_winner_hashes);
      ("overlay first hash", test_overlay_first_hash);
    ]
