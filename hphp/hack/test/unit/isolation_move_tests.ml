(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude

let source = "projects/a"

let parent = "packages/deep"

let target = "packages/deep/a"

let path suffix = Relative_path.from_root ~suffix

let count_php_files dir =
  let rec count dir =
    if not (Stdlib.Sys.file_exists dir) then
      0
    else
      Array.fold (Stdlib.Sys.readdir dir) ~init:0 ~f:(fun total name ->
          let path = Filename.concat dir name in
          if Stdlib.Sys.is_directory path then
            total + count path
          else if String.is_suffix name ~suffix:".php" then
            total + 1
          else
            total)
  in
  count dir

(** A repository with two Hack files under [source], and the root the server
    would report positions under. *)
let with_repo f =
  let dir = Stdlib.Filename.temp_file "hh_isolation_move" "" in
  Stdlib.Sys.remove dir;
  Disk.mkdir_p (Filename.concat dir (source ^ "/sub"));
  Disk.write_file
    ~file:(Filename.concat dir (source ^ "/x.php"))
    ~contents:"<?hh\n";
  Disk.write_file
    ~file:(Filename.concat dir (source ^ "/sub/y.php"))
    ~contents:"<?hh\n";
  (* What is not Hack moves too, and so does a directory with nothing in it. *)
  Disk.write_file
    ~file:(Filename.concat dir (source ^ "/data.json"))
    ~contents:"{}\n";
  Disk.write_file
    ~file:(Filename.concat dir (source ^ "/.hidden"))
    ~contents:"x\n";
  Disk.mkdir_p (Filename.concat dir (source ^ "/sub/empty"));
  Relative_path.set_path_prefix Relative_path.Root (Path.make dir);
  let root =
    String.rstrip
      (Relative_path.path_of_prefix Relative_path.Root)
      ~drop:(Char.equal '/')
  in
  Exn.protect ~f:(fun () -> f root) ~finally:(fun () -> Disk.rm_dir_tree dir)

let diagnostic ?(severity = User_diagnostic.Err) ~code ~file ~line message =
  let pos =
    Pos.make_from_lnum_bol_offset
      ~pos_file:(Relative_path.from_root ~suffix:file)
      ~pos_start:(line, 0, 0)
      ~pos_end:(line, 0, 1)
    |> Pos.to_absolute
  in
  User_diagnostic.make_absolute severity code [(pos, message)]

let warning = User_diagnostic.Warning { is_trusted = true }

let status error_list =
  {
    Server_command_types.Server_status.liveness =
      Server_command_types.Live_status;
    error_list;
    dropped_count = 0;
    last_recheck_stats = None;
    file_watcher_clock = None;
  }

let answer ?(movability = Some Server_isolation_types.Movable) dir =
  {
    Server_isolation_types.refused = None;
    movability;
    grown = false;
    total_seeds = count_php_files (Relative_path.to_absolute dir);
    total_isolatable_files = 0;
    total_clusters = 0;
    total_truncated = 0;
    largest_cluster = 0;
  }

let capture_stdout f =
  let file = Stdlib.Filename.temp_file "hh_isolation_stdout" "" in
  let fd = Unix.openfile file [Unix.O_WRONLY; Unix.O_TRUNC] 0o600 in
  Stdlib.flush Stdlib.stdout;
  let saved = Unix.dup Unix.stdout in
  Unix.dup2 fd Unix.stdout;
  Unix.close fd;
  let result =
    Exn.protect ~f ~finally:(fun () ->
        Stdlib.flush Stdlib.stdout;
        Unix.dup2 saved Unix.stdout;
        Unix.close saved)
  in
  let output = Disk.cat file in
  Stdlib.Sys.remove file;
  (result, output)

let options ~source =
  {
    Server_isolation_types.mode = Server_isolation_types.Move_to parent;
    clusters_destination = Server_isolation_types.To_file "/tmp/clusters";
    seed_framework = Some "framework";
    seed_list = Some "list";
    seed_dir = Some source;
    max_dependents = 7;
    max_cluster_size = Some 9;
    batch_size = 5;
    allow_long_run = true;
    max_seeds = Some 3;
    seed_offset = 1;
  }

type result = {
  exit_status: Exit_status.t;
  output: string;
  asked: Server_isolation_types.options list;
}

(** [Client_isolation_move.move_into] against a server that answers from what is on
    disk, as it would after seeing the changes: [before] while [source] is
    there, [after] once it has moved. [after_move] runs when the server is first
    asked after the move. *)
let drive
    ?(movability = Some Server_isolation_types.Movable)
    ?(unseen = false)
    ?(stuck = false)
    ?(after_move = (fun () -> ()))
    ?(when_asked_after_move = (fun () -> ()))
    ?(output_json = true)
    ?(source = source)
    ?(parent = parent)
    ?(target = target)
    ~root
    ~before
    ~after
    () =
  let moved () = Disk.file_exists (Filename.concat root target) in
  let after_move_ran = ref false in
  let asked = ref [] in
  let status_asked = ref 0 in
  let (exit_status, output) =
    capture_stdout (fun () ->
        Lwt_main.run
          (Client_isolation_move.move_into
             ~options:(options ~source)
             ~parent
             ~output_json
             ~isolation_query:(fun options ->
               asked := options :: !asked;
               let dir =
                 Option.value_exn options.Server_isolation_types.seed_dir
               in
               if moved () then when_asked_after_move ();
               if unseen && String.equal dir target then
                 Lwt.return { (answer (path dir)) with total_seeds = 0 }
               else
                 Lwt.return (answer ~movability (path dir)))
             ~status:(fun () ->
               Int.incr status_asked;
               if moved () && not !after_move_ran then begin
                 after_move_ran := true;
                 after_move ()
               end;
               (* [stuck] is a server that still shows the errors after the
                  files are back, which is the third answer it gives. *)
               Lwt.return
                 (status
                    (if moved () || (stuck && !status_asked >= 3) then
                      after
                    else
                      before)))))
  in
  { exit_status; output; asked = List.rev !asked }

let exists ~root dir = Disk.file_exists (Filename.concat root dir)

let everything_is_under ~root dir =
  List.for_all
    ["x.php"; "sub/y.php"; "data.json"; ".hidden"; "sub/empty"]
    ~f:(fun path -> exists ~root (Filename.concat dir path))

let same_status a b =
  Int.equal (Exit_status.exit_code a) (Exit_status.exit_code b)

let field name result =
  Yojson.Safe.Util.member name (Yojson.Safe.from_string result.output)

let is_json json expected = Yojson.Safe.equal json expected

let error_fields result =
  List.map
    (Yojson.Safe.Util.to_list (field "new_errors" result))
    ~f:(fun error ->
      ( Yojson.Safe.Util.to_int (Yojson.Safe.Util.member "code" error),
        Yojson.Safe.Util.to_string (Yojson.Safe.Util.member "file" error),
        Yojson.Safe.Util.to_int (Yojson.Safe.Util.member "line" error) ))

let a_moved_directory_is_whole ~root =
  everything_is_under ~root target
  && (not (exists ~root source))
  && String.equal
       (Disk.cat (Filename.concat root (target ^ "/data.json")))
       "{}\n"

let the_directory_is_back ~root =
  everything_is_under ~root source
  && (not (exists ~root target))
  && not (exists ~root "packages")

let test_a_directory_that_can_move_and_adds_no_errors_is_moved () =
  with_repo @@ fun root ->
  (* An error the files already had is reported under their new path, and a
     warning the move makes visible is not an error. Neither is new. *)
  let old_error =
    diagnostic ~code:4110 ~file:(source ^ "/x.php") ~line:3 "bad"
  in
  let moved_error =
    diagnostic ~code:4110 ~file:(target ^ "/x.php") ~line:3 "bad"
  in
  let shown_warning =
    diagnostic
      ~severity:warning
      ~code:12005
      ~file:(target ^ "/sub/y.php")
      ~line:9
      "casting"
  in
  let result =
    drive ~root ~before:[old_error] ~after:[moved_error; shown_warning] ()
  in
  same_status result.exit_status Exit_status.No_error
  && is_json (field "moved" result) (`Bool true)
  && is_json (field "files" result) (`Int 2)
  && a_moved_directory_is_whole ~root

let test_the_directory_goes_into_the_parent_under_its_own_name () =
  with_repo @@ fun root ->
  let result =
    drive
      ~source:(source ^ "/")
      ~parent:"packages/deep/"
      ~root
      ~before:[]
      ~after:[]
      ()
  in
  is_json (field "source" result) (`String source)
  && is_json (field "target" result) (`String target)
  && a_moved_directory_is_whole ~root

let test_a_blocked_directory_is_left_alone () =
  with_repo @@ fun root ->
  let result =
    drive
      ~movability:(Some (Server_isolation_types.Blocked ["lib/o.php"]))
      ~root
      ~before:[]
      ~after:[]
      ()
  in
  same_status result.exit_status Exit_status.Type_error
  && is_json (field "blockers" result) (`List [`String "lib/o.php"])
  && the_directory_is_back ~root

let test_a_move_that_adds_an_error_is_undone () =
  with_repo @@ fun root ->
  let introduced =
    diagnostic ~code:4472 ~file:"lib/o.php" ~line:30 "not available"
  in
  let result = drive ~root ~before:[] ~after:[introduced] () in
  same_status result.exit_status Exit_status.Type_error
  && is_json (field "moved" result) (`Bool false)
  && List.equal
       (fun (a, b, c) (x, y, z) -> a = x && String.equal b y && c = z)
       (error_fields result)
       [(4472, "lib/o.php", 30)]
  && is_json (field "baseline_restored" result) (`Bool true)
  && the_directory_is_back ~root

let test_errors_that_do_not_clear_after_the_undo_are_reported () =
  with_repo @@ fun root ->
  let introduced =
    diagnostic ~code:2049 ~file:(target ^ "/x.php") ~line:5 "unbound"
  in
  let result = drive ~stuck:true ~root ~before:[] ~after:[introduced] () in
  (* Named as it was before the move, and the directory is back. *)
  List.equal
    (fun (a, b, c) (x, y, z) -> a = x && String.equal b y && c = z)
    (error_fields result)
    [(2049, source ^ "/x.php", 5)]
  && is_json (field "baseline_restored" result) (`Bool false)
  && everything_is_under ~root source

let test_an_error_that_names_the_directory_is_not_new () =
  with_repo @@ fun root ->
  let named path = Printf.sprintf "bad %s/x.php" path in
  let old_error =
    diagnostic ~code:4110 ~file:(source ^ "/x.php") ~line:3 (named source)
  in
  let moved_error =
    diagnostic ~code:4110 ~file:(target ^ "/x.php") ~line:3 (named target)
  in
  let result = drive ~root ~before:[old_error] ~after:[moved_error] () in
  same_status result.exit_status Exit_status.No_error
  && is_json (field "moved" result) (`Bool true)

let test_an_undo_that_fails_says_what_went_wrong_first () =
  with_repo @@ fun root ->
  match
    drive
      ~root
      ~before:[]
      ~after:[]
      ~when_asked_after_move:(fun () ->
        (* The directory cannot be put back where a file now is. *)
        Disk.write_file ~file:(Filename.concat root source) ~contents:"x";
        failwith "the server went away")
      ()
  with
  | _ -> false
  | exception Failure message ->
    String.is_substring message ~substring:"the server went away"
    && String.is_substring message ~substring:source

let test_a_move_the_server_did_not_see_is_undone () =
  with_repo @@ fun root ->
  let result = drive ~unseen:true ~root ~before:[] ~after:[] () in
  same_status result.exit_status Exit_status.Input_error
  && (match field "refused" result with
     | `String message ->
       String.is_substring message ~substring:"edenfs_file_watcher_enabled=true"
     | _ -> false)
  && the_directory_is_back ~root

let test_a_move_that_fails_after_making_the_parents_is_undone () =
  with_repo @@ fun root ->
  (* A directory cannot be renamed into another one without write permission on
     it, so the rename fails after the parents of the target were made. *)
  let locked = Filename.concat root source in
  Unix.chmod locked 0o555;
  Exn.protect
    ~finally:(fun () -> Unix.chmod locked 0o755)
    ~f:(fun () ->
      let result = drive ~root ~before:[] ~after:[] () in
      same_status result.exit_status Exit_status.Input_error
      && the_directory_is_back ~root)

let test_a_target_that_exists_is_refused () =
  with_repo @@ fun root ->
  Disk.mkdir_p (Filename.concat root target);
  let result = drive ~root ~before:[] ~after:[] () in
  same_status result.exit_status Exit_status.Input_error
  && exists ~root source
  && exists ~root target

let test_a_directory_without_hack_files_is_refused () =
  with_repo @@ fun root ->
  Disk.mkdir_p (Filename.concat root "projects/empty");
  let result = drive ~source:"projects/empty" ~root ~before:[] ~after:[] () in
  same_status result.exit_status Exit_status.Input_error
  && not (exists ~root "packages/deep/empty")

let test_directories_outside_the_root_are_refused () =
  with_repo @@ fun root ->
  let refused result =
    same_status result.exit_status Exit_status.Input_error
    && exists ~root source
    && not (exists ~root "packages")
  in
  refused (drive ~source:"../elsewhere" ~root ~before:[] ~after:[] ())
  && refused (drive ~parent:"/tmp/elsewhere" ~root ~before:[] ~after:[] ())
  && refused (drive ~parent:source ~root ~before:[] ~after:[] ())

(** A move the server then reports an error for, so that it is undone, and
    something that happens to the disk after the move. *)
let undone_after ~root ~after_move =
  let introduced =
    diagnostic ~code:4472 ~file:"lib/o.php" ~line:30 "not available"
  in
  drive ~root ~before:[] ~after:[introduced] ~after_move ()

let test_undo_does_not_overwrite_what_is_at_the_old_place () =
  with_repo @@ fun root ->
  let file = Filename.concat root (source ^ "/x.php") in
  match
    undone_after ~root ~after_move:(fun () ->
        Disk.mkdir_p (Filename.concat root source);
        Disk.write_file ~file ~contents:"other")
  with
  | _ -> false
  | exception Failure message ->
    (* The directory is where it was moved to, and what is at the old place is
       as it was. *)
    String.is_substring message ~substring:source
    && String.equal (Disk.cat file) "other"
    && everything_is_under ~root target

let test_run_asks_about_each_directory_with_the_options_it_was_given () =
  with_repo @@ fun root ->
  let result = drive ~root ~before:[] ~after:[] () in
  let dirs =
    List.map result.asked ~f:(fun options ->
        options.Server_isolation_types.seed_dir)
  in
  let asks dir = List.mem dirs (Some dir) ~equal:(Option.equal String.equal) in
  Option.equal String.equal (List.hd_exn dirs) (Some source)
  && asks target
  && List.for_all dirs ~f:(fun dir ->
         Option.equal String.equal dir (Some source)
         || Option.equal String.equal dir (Some target))
  && List.for_all result.asked ~f:(fun asked ->
         (match asked.Server_isolation_types.clusters_destination with
         | Server_isolation_types.Dropped -> true
         | Server_isolation_types.To_file _ -> false)
         && Option.equal String.equal asked.seed_list (Some "list")
         && Int.equal asked.max_dependents 7
         && Int.equal asked.seed_offset 1)

let test_a_path_that_climbs_out_of_the_root_is_refused_though_it_exists () =
  with_repo @@ fun root ->
  let climbing =
    Filename.concat (Filename.concat ".." (Filename.basename root)) source
  in
  let result = drive ~source:climbing ~root ~before:[] ~after:[] () in
  same_status result.exit_status Exit_status.Input_error
  && everything_is_under ~root source
  && not (exists ~root target)

let an_error n =
  diagnostic
    ~code:(4000 + n)
    ~file:(Printf.sprintf "%s/e%d.php" target n)
    ~line:(n + 1)
    (Printf.sprintf "first line %d\nsecond line" n)

let contains = String.is_substring

let test_the_output_says_what_moved () =
  with_repo @@ fun root ->
  let moved = drive ~output_json:false ~root ~before:[] ~after:[] () in
  String.equal
    moved.output
    "Moved: 2 files from projects/a to packages/deep/a, with no new errors.\n"
  && same_status moved.exit_status Exit_status.No_error

let test_the_output_counts_the_blockers () =
  with_repo @@ fun root ->
  let blocked blockers =
    (drive
       ~output_json:false
       ~movability:(Some (Server_isolation_types.Blocked blockers))
       ~root
       ~before:[]
       ~after:[]
       ())
      .output
  in
  let one = blocked ["f.php"] in
  let two = blocked ["f.php"; "g.php"] in
  contains one ~substring:"Not moved: 1 file outside references in."
  && contains one ~substring:"  f.php\n"
  && contains two ~substring:"Not moved: 2 files outside reference in."
  && contains two ~substring:"  g.php\n"

let test_the_output_shows_ten_errors_and_counts_the_rest () =
  with_repo @@ fun root ->
  let result =
    drive
      ~output_json:false
      ~root
      ~before:[]
      ~after:(List.init 12 ~f:an_error)
      ()
  in
  let output = result.output in
  contains output ~substring:"the move introduced 12 errors, and it was undone"
  && contains output ~substring:"  4000 projects/a/e0.php:1: first line 0\n"
  && contains output ~substring:"  4009 projects/a/e9.php:10: first line 9\n"
  && (not (contains output ~substring:"e10.php"))
  && contains output ~substring:"  and 2 more\n"
  && not (contains output ~substring:"second line")

let tests =
  [
    ( "test_a_directory_that_can_move_and_adds_no_errors_is_moved",
      test_a_directory_that_can_move_and_adds_no_errors_is_moved );
    ( "test_the_directory_goes_into_the_parent_under_its_own_name",
      test_the_directory_goes_into_the_parent_under_its_own_name );
    ( "test_a_blocked_directory_is_left_alone",
      test_a_blocked_directory_is_left_alone );
    ( "test_a_move_that_adds_an_error_is_undone",
      test_a_move_that_adds_an_error_is_undone );
    ( "test_errors_that_do_not_clear_after_the_undo_are_reported",
      test_errors_that_do_not_clear_after_the_undo_are_reported );
    ( "test_an_error_that_names_the_directory_is_not_new",
      test_an_error_that_names_the_directory_is_not_new );
    ( "test_an_undo_that_fails_says_what_went_wrong_first",
      test_an_undo_that_fails_says_what_went_wrong_first );
    ( "test_a_move_the_server_did_not_see_is_undone",
      test_a_move_the_server_did_not_see_is_undone );
    ( "test_a_move_that_fails_after_making_the_parents_is_undone",
      test_a_move_that_fails_after_making_the_parents_is_undone );
    ( "test_a_target_that_exists_is_refused",
      test_a_target_that_exists_is_refused );
    ( "test_a_directory_without_hack_files_is_refused",
      test_a_directory_without_hack_files_is_refused );
    ( "test_directories_outside_the_root_are_refused",
      test_directories_outside_the_root_are_refused );
    ( "test_a_path_that_climbs_out_of_the_root_is_refused_though_it_exists",
      test_a_path_that_climbs_out_of_the_root_is_refused_though_it_exists );
    ( "test_undo_does_not_overwrite_what_is_at_the_old_place",
      test_undo_does_not_overwrite_what_is_at_the_old_place );
    ( "test_run_asks_about_each_directory_with_the_options_it_was_given",
      test_run_asks_about_each_directory_with_the_options_it_was_given );
    ("test_the_output_says_what_moved", test_the_output_says_what_moved);
    ("test_the_output_counts_the_blockers", test_the_output_counts_the_blockers);
    ( "test_the_output_shows_ten_errors_and_counts_the_rest",
      test_the_output_shows_ten_errors_and_counts_the_rest );
  ]

let () = Unit_test.run_all tests
