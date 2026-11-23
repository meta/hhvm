(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude
open Asserter
open Server_progress_test_helpers

let pid = Unix.getpid ()

let expect_exn (f : unit -> unit) : unit =
  let got_exn =
    try
      f ();
      false
    with
    | _ -> true
  in
  if not got_exn then failwith "Expected an exception"

let expect_state (expected : string) : unit =
  let actual = Server_progress.ErrorsWrite.get_state_FOR_TEST () in
  String_asserter.assert_equals expected actual "expect_state"

let telemetry = Telemetry.create ()

let cancel_reason = ("test", "")

let root = Path.make "test"

let try_with_tmp (f : root:Path.t -> unit Lwt.t) : unit Lwt.t =
  let tmp = Tempfile.mkdtemp ~skip_mocking:true in
  Lwt_utils.try_finally
    ~f:(fun () ->
      (* We want ServerFiles.errors_file to be placed in this test's temp directory *)
      Server_files.set_tmp_FOR_TESTING_ONLY tmp;
      (* We need Server_progress.Errors to have a root (any root) so it knows how to name the errors file *)
      Server_progress.set_root root;
      Relative_path.set_path_prefix Relative_path.Root root;
      (* To isolate tests, we have to reset Server_progress.Errors global state in memory. *)
      Server_progress.ErrorsWrite.unlink_at_server_stop ();
      let%lwt () = f ~root in
      Lwt.return_unit)
    ~finally:(fun () ->
      Sys_utils.rm_dir_tree ~skip_mocking:true (Path.to_string tmp);
      Lwt.return_unit)

type simple_diagnostic = int * string * string [@@deriving ord, eq, show]

type simple_diagnostic_list = simple_diagnostic list [@@deriving show]

let make_diagnostics (diagnostics : (int * string * string) list) :
    Diagnostics.t =
  let diagnostics =
    List.map diagnostics ~f:(fun (code, rel_path, message) ->
        let path = Relative_path.from_root ~suffix:rel_path in
        let diagnostic =
          User_diagnostic.make_err
            code
            (Pos.make_from path, message)
            []
            Explanation.empty
        in
        (path, diagnostic))
  in
  Diagnostics.from_file_diagnostic_list diagnostics

let make_warning_diagnostics (diagnostics : (int * string * string) list) :
    Diagnostics.t =
  let diagnostics =
    List.map diagnostics ~f:(fun (code, rel_path, message) ->
        let path = Relative_path.from_root ~suffix:rel_path in
        let diagnostic =
          User_diagnostic.make_warning code (Pos.make_from path, message) []
        in
        (path, diagnostic))
  in
  Diagnostics.from_file_diagnostic_list diagnostics

let a_diagnostic : Diagnostics.t = make_diagnostics [(101, "c", "oops")]

let test_completed () : bool Lwt.t =
  let%lwt () =
    try_with_tmp (fun ~root ->
        let errors_file_path = Server_files.errors_file_path root in
        Server_progress.ErrorsWrite.new_empty_file
          ~clock:None
          ~ignore_hh_version:false
          ~cancel_reason;
        Server_progress.ErrorsWrite.report
          (make_diagnostics
             [(101, "a", "hello"); (101, "a", "there"); (101, "b", "world")]);
        Server_progress.ErrorsWrite.report
          (make_diagnostics [(101, "c", "oops")]);
        Server_progress.ErrorsWrite.complete telemetry;
        let fd = Unix.openfile errors_file_path [Unix.O_RDONLY] 0 in
        assert (Server_progress.ErrorsRead.openfile fd |> Result.is_ok);
        String_asserter.assert_equals
          "Errors [a=2,b=1]"
          (Server_progress.ErrorsRead.read_next_errors fd |> show_read)
          "errors";
        String_asserter.assert_equals
          "Errors [c=1]"
          (Server_progress.ErrorsRead.read_next_errors fd |> show_read)
          "errors";
        String_asserter.assert_equals
          "typecheck completed [complete]"
          (Server_progress.ErrorsRead.read_next_errors fd |> show_read)
          "complete";
        Unix.close fd;
        Lwt.return_unit)
  in
  Lwt.return_true

let test_read_empty () : bool Lwt.t =
  let%lwt () =
    try_with_tmp (fun ~root ->
        let errors_file_path = Server_files.errors_file_path root in
        Sys_utils.touch
          (Sys_utils.Touch_existing_file_or_create_new
             { mkdir_if_new = false; perm_if_new = 0o666 })
          errors_file_path;
        let fd = Unix.openfile errors_file_path [Unix.O_RDONLY] 0 in
        String_asserter.assert_equals
          "nothing yet"
          (Server_progress.ErrorsRead.read_next_errors fd |> show_read)
          "killed";
        Unix.close fd;
        Lwt.return_unit)
  in
  Lwt.return_true

let test_read_unlinked () : bool Lwt.t =
  let%lwt () =
    try_with_tmp (fun ~root ->
        let errors_file_path = Server_files.errors_file_path root in
        Server_progress.ErrorsWrite.new_empty_file
          ~clock:None
          ~ignore_hh_version:false
          ~cancel_reason;
        let fd = Unix.openfile errors_file_path [Unix.O_RDONLY] 0 in
        Server_progress.ErrorsWrite.unlink_at_server_stop ();
        assert (Server_progress.ErrorsRead.openfile fd |> Result.is_ok);
        String_asserter.assert_equals
          "hh_server gracefully stopped [unlink]"
          (Server_progress.ErrorsRead.read_next_errors fd |> show_read)
          "end";
        Unix.close fd;
        Lwt.return_unit)
  in
  Lwt.return_true

let test_read_unlinked_empty () : bool Lwt.t =
  let%lwt () =
    try_with_tmp (fun ~root ->
        let errors_file_path = Server_files.errors_file_path root in
        Server_progress.ErrorsWrite.new_empty_file
          ~clock:None
          ~ignore_hh_version:false
          ~cancel_reason;
        let fd = Unix.openfile errors_file_path [Unix.O_RDONLY] 0 in
        assert (Server_progress.ErrorsRead.openfile fd |> Result.is_ok);
        Server_progress.ErrorsWrite.unlink_at_server_stop ();
        String_asserter.assert_equals
          "hh_server gracefully stopped [unlink]"
          (Server_progress.ErrorsRead.read_next_errors fd |> show_read)
          "end";
        Unix.close fd;
        Lwt.return_unit)
  in
  Lwt.return_true

let test_read_restarted () : bool Lwt.t =
  let%lwt () =
    try_with_tmp (fun ~root ->
        let errors_file_path = Server_files.errors_file_path root in
        Server_progress.ErrorsWrite.new_empty_file
          ~clock:None
          ~ignore_hh_version:false
          ~cancel_reason;
        Server_progress.ErrorsWrite.report a_diagnostic;
        let fd = Unix.openfile errors_file_path [Unix.O_RDONLY] 0 in
        Server_progress.ErrorsWrite.new_empty_file
          ~clock:None
          ~ignore_hh_version:false
          ~cancel_reason;
        assert (Server_progress.ErrorsRead.openfile fd |> Result.is_ok);
        String_asserter.assert_equals
          "Errors [c=1]"
          (Server_progress.ErrorsRead.read_next_errors fd |> show_read)
          "errors";
        String_asserter.assert_equals
          "typecheck restarted due to file changes [new_empty_file]"
          (Server_progress.ErrorsRead.read_next_errors fd |> show_read)
          "end";
        Unix.close fd;
        Lwt.return_unit)
  in
  Lwt.return_true

let test_locks () : bool Lwt.t =
  (* This is just too irritating to test, so I'm not going to...
     The Server_progress.Errors functions all wait synchronously until they can acquire a lock,
     then do a tiny amount of work, then release the lock. The only way to test this would be
     with multiple threads, which I'm loathe to do. *)
  Lwt.return_true

let test_read_half_message () : bool Lwt.t =
  let%lwt () =
    try_with_tmp (fun ~root ->
        let errors_file_path = Server_files.errors_file_path root in
        let preamble = Marshal_tools.make_preamble 15 in
        Sys_utils.write_file ~file:errors_file_path (Bytes.to_string preamble);
        let fd = Unix.openfile errors_file_path [Unix.O_RDONLY] 0 in
        String_asserter.assert_equals
          "malformed error"
          (Server_progress.ErrorsRead.read_next_errors fd |> show_read)
          "half";
        Unix.close fd;
        Lwt.return_unit)
  in
  Lwt.return_true

let test_read_dead_pid () : bool Lwt.t =
  let%lwt () =
    try_with_tmp (fun ~root ->
        Server_progress.ErrorsWrite.create_file_FOR_TEST ~pid:1 ~cmdline:"bogus";
        let errors_file_path = Server_files.errors_file_path root in
        let fd = Unix.openfile errors_file_path [Unix.O_RDONLY] 0 in
        begin
          match Server_progress.ErrorsRead.openfile fd with
          | Error
              ( Server_progress.ErrorsRead.OKilled _,
                "Errors-file is from defunct PID" ) ->
            ()
          | Ok _ -> failwith "Expected a dead-pid failure, not success"
          | Error (e, msg) ->
            failwith
              ("Expected OKilled, not ("
              ^ Server_progress.ErrorsRead.show_open_error e
              ^ ", "
              ^ msg
              ^ ")")
        end;
        Unix.close fd;
        Lwt.return_unit)
  in
  Lwt.return_true

let test_produce_disordered () : bool Lwt.t =
  (* This test tests all actions "new_empty_file, report, complete, unlink"
     from all possible states "Absent, Empty, Reporting, Closed" *)
  let%lwt () =
    try_with_tmp (fun ~root:_ ->
        (* Actions from state "Absent"... *)
        expect_state "Absent";
        expect_exn (fun () -> Server_progress.ErrorsWrite.report a_diagnostic);
        expect_state "Absent";
        expect_exn (fun () -> Server_progress.ErrorsWrite.complete telemetry);
        expect_state "Absent";
        Server_progress.ErrorsWrite.unlink_at_server_stop ();
        expect_state "Absent";
        Server_progress.ErrorsWrite.new_empty_file
          ~clock:None
          ~ignore_hh_version:false
          ~cancel_reason;
        expect_state "Reporting[0]";
        Lwt.return_unit)
  in
  let%lwt () =
    try_with_tmp (fun ~root:_ ->
        (* Actions from state "Reporting[0]"... *)
        Server_progress.ErrorsWrite.new_empty_file
          ~clock:None
          ~ignore_hh_version:false
          ~cancel_reason;
        expect_state "Reporting[0]";
        Server_progress.ErrorsWrite.new_empty_file
          ~clock:None
          ~ignore_hh_version:false
          ~cancel_reason;
        expect_state "Reporting[0]";
        Server_progress.ErrorsWrite.report a_diagnostic;
        expect_state "Reporting[1]";
        Server_progress.ErrorsWrite.new_empty_file
          ~clock:None
          ~ignore_hh_version:false
          ~cancel_reason;
        Server_progress.ErrorsWrite.unlink_at_server_stop ();
        expect_state "Absent";
        Server_progress.ErrorsWrite.new_empty_file
          ~clock:None
          ~ignore_hh_version:false
          ~cancel_reason;
        expect_state "Reporting[0]";
        Server_progress.ErrorsWrite.complete telemetry;
        expect_state "Closed";
        Lwt.return_unit)
  in
  let%lwt () =
    try_with_tmp (fun ~root:_ ->
        (* Actions from state "Reporting[1]" *)
        Server_progress.ErrorsWrite.new_empty_file
          ~clock:None
          ~ignore_hh_version:false
          ~cancel_reason;
        Server_progress.ErrorsWrite.report a_diagnostic;
        expect_state "Reporting[1]";
        Server_progress.ErrorsWrite.new_empty_file
          ~clock:None
          ~ignore_hh_version:false
          ~cancel_reason;
        expect_state "Reporting[0]";
        Server_progress.ErrorsWrite.report a_diagnostic;
        expect_state "Reporting[1]";
        Server_progress.ErrorsWrite.report a_diagnostic;
        expect_state "Reporting[2]";
        Server_progress.ErrorsWrite.complete telemetry;
        expect_state "Closed";
        Server_progress.ErrorsWrite.new_empty_file
          ~clock:None
          ~ignore_hh_version:false
          ~cancel_reason;
        Server_progress.ErrorsWrite.report a_diagnostic;
        expect_state "Reporting[1]";
        Server_progress.ErrorsWrite.unlink_at_server_stop ();
        expect_state "Absent";
        Lwt.return_unit)
  in
  let%lwt () =
    try_with_tmp (fun ~root:_ ->
        (* Actions from state "Closed" *)
        Server_progress.ErrorsWrite.new_empty_file
          ~clock:None
          ~ignore_hh_version:false
          ~cancel_reason;
        Server_progress.ErrorsWrite.complete telemetry;
        expect_state "Closed";
        Server_progress.ErrorsWrite.new_empty_file
          ~clock:None
          ~ignore_hh_version:false
          ~cancel_reason;
        expect_state "Reporting[0]";
        Server_progress.ErrorsWrite.complete telemetry;
        expect_state "Closed";
        expect_exn (fun () -> Server_progress.ErrorsWrite.report a_diagnostic);
        expect_state "Closed";
        expect_exn (fun () -> Server_progress.ErrorsWrite.complete telemetry);
        expect_state "Closed";
        Server_progress.ErrorsWrite.unlink_at_server_stop ();
        expect_state "Absent";
        Lwt.return_unit)
  in
  Lwt.return_true

let test_async_read_completed () : bool Lwt.t =
  let%lwt () =
    try_with_tmp (fun ~root ->
        let errors_file_path = Server_files.errors_file_path root in
        Server_progress.ErrorsWrite.new_empty_file
          ~clock:None
          ~ignore_hh_version:false
          ~cancel_reason;
        Server_progress.ErrorsWrite.report
          (make_diagnostics
             [(101, "a", "hello"); (101, "a", "there"); (101, "b", "world")]);
        Server_progress.ErrorsWrite.report
          (make_diagnostics [(101, "c", "oops")]);
        Server_progress.ErrorsWrite.complete telemetry;
        let fd = Unix.openfile errors_file_path [Unix.O_RDONLY] 0 in
        let _open = Server_progress.ErrorsRead.openfile fd in
        let q = Server_progress_lwt.watch_errors_file ~pid fd in
        let%lwt () = expect_qitem q "Errors [a=2,b=1]" in
        let%lwt () = expect_qitem q "Errors [c=1]" in
        let%lwt () = expect_qitem q "typecheck completed [complete]" in
        let%lwt () = expect_qitem q "closed" in
        Lwt.return_unit)
  in
  Lwt.return_true

let test_async_read_partial () : bool Lwt.t =
  let%lwt () =
    try_with_tmp (fun ~root ->
        let errors_file_path = Server_files.errors_file_path root in
        Server_progress.ErrorsWrite.new_empty_file
          ~clock:None
          ~ignore_hh_version:false
          ~cancel_reason;
        let fd = Unix.openfile errors_file_path [Unix.O_RDONLY] 0 in
        let _open = Server_progress.ErrorsRead.openfile fd in
        let q = Server_progress_lwt.watch_errors_file ~pid fd in
        (* initially there are no items available *)
        let%lwt () = expect_qitem q "nothing" in
        (* we'll put in one report, and after this there should be exactly one item available *)
        Server_progress.ErrorsWrite.report
          (make_diagnostics
             [(101, "a", "hello"); (101, "a", "there"); (101, "b", "world")]);
        let%lwt () = expect_qitem q "Errors [a=2,b=1]" in
        let%lwt () = expect_qitem q "nothing" in
        (* we'll complete the file, and after this the stream should be closed *)
        Server_progress.ErrorsWrite.complete (Telemetry.create ());
        let%lwt () = expect_qitem q "typecheck completed [complete]" in
        let%lwt () = expect_qitem q "closed" in
        Lwt.return_unit)
  in
  Lwt.return_true

let test_async_read_unlinked () : bool Lwt.t =
  let%lwt () =
    try_with_tmp (fun ~root ->
        let errors_file_path = Server_files.errors_file_path root in
        Server_progress.ErrorsWrite.new_empty_file
          ~clock:(Some (Server_notifier.Watchman "clock123"))
          ~ignore_hh_version:false
          ~cancel_reason;
        let fd = Unix.openfile errors_file_path [Unix.O_RDONLY] 0 in
        let open_result = Server_progress.ErrorsRead.openfile fd in
        let { Server_progress.ErrorsRead.pid; clock; _ } =
          Result.ok open_result |> Option.value_exn
        in
        assert (
          Option.equal
            Server_notifier.equal_clock
            (Some (Server_notifier.Watchman "clock123"))
            clock);
        assert (pid = Unix.getpid ());
        let q = Server_progress_lwt.watch_errors_file ~pid fd in
        (* we'll unlink the file, and after this the stream should be closed *)
        Server_progress.ErrorsWrite.unlink_at_server_stop ();
        let%lwt () = expect_qitem q "hh_server gracefully stopped [unlink]" in
        let%lwt () = expect_qitem q "closed" in
        Lwt.return_unit)
  in
  Lwt.return_true

let test_start_read_killed () : bool Lwt.t =
  let%lwt () =
    try_with_tmp (fun ~root ->
        (* we'll write an incomplete file *)
        let errors_file_path = Server_files.errors_file_path root in
        Sys_utils.write_file ~file:errors_file_path "a";
        let fd = Unix.openfile errors_file_path [Unix.O_RDONLY] 0 in
        (* files are created atomically; there should be no way to read
           an empty file, not even if the creating process got killed. *)
        let is_ok =
          try
            let _ = Server_progress.ErrorsRead.openfile fd in
            true
          with
          | _ -> false
        in
        Unix.close fd;
        if is_ok then
          failwith "we shouldn't be able to openfile on an impossible file";
        Lwt.return_unit)
  in
  Lwt.return_true

let test_async_read_killed () : bool Lwt.t =
  let%lwt () =
    try_with_tmp (fun ~root ->
        let errors_file_path = Server_files.errors_file_path root in
        Server_progress.ErrorsWrite.new_empty_file
          ~clock:None
          ~ignore_hh_version:false
          ~cancel_reason;
        (* we'll write an incomplete file *)
        let preamble = Marshal_tools.make_preamble 100 in
        Sys_utils.append_file ~file:errors_file_path (Bytes.to_string preamble);
        let fd = Unix.openfile errors_file_path [Unix.O_RDONLY] 0 in
        let { Server_progress.ErrorsRead.pid; _ } =
          Server_progress.ErrorsRead.openfile fd
          |> Result.ok
          |> Option.value_exn
        in
        let q = Server_progress_lwt.watch_errors_file ~pid fd in
        (* because the file is incomplete, the queue should assume that the
           creating process was killed *)
        let%lwt () = expect_qitem q "malformed error" in
        let%lwt () = expect_qitem q "closed" in
        Lwt.return_unit)
  in
  Lwt.return_true

let test_async_pid_killed () : bool Lwt.t =
  let%lwt () =
    try_with_tmp (fun ~root ->
        (* This will simulate a file which is in a complete state, but the producing
           PID simply died *)
        let errors_file_path = Server_files.errors_file_path root in
        Server_progress.ErrorsWrite.new_empty_file
          ~clock:None
          ~ignore_hh_version:false
          ~cancel_reason;
        let fd = Unix.openfile errors_file_path [Unix.O_RDONLY] 0 in
        let _open = Server_progress.ErrorsRead.openfile fd in
        (* For our test, let us pick a pid which doesn't exist.
           This is a bit racey, but it's the best we can do.
           We trust that new pids are assigned in ascending order,
           so we'll start from the current pid and work our way down.
           If we hit 0, we'll wrap-around to Int.max_value_30bits because
           Unix.kill needs a positive PID. *)
        let rec pick_dead_pid pid =
          try
            Unix.kill pid 0;
            let pid =
              if pid = 1 then
                Int.max_value_30_bits
              else
                pid - 1
            in
            pick_dead_pid pid
          with
          | _ -> pid
        in
        let dead_pid = pick_dead_pid (Unix.getpid ()) in
        let q = Server_progress_lwt.watch_errors_file ~pid:dead_pid fd in
        (* because the pid is dead, within 5s, the queue should report
           that the creating process was killed *)
        let%lwt () = expect_qitem ~delay:60.0 q "hh_server died" in
        let%lwt () = expect_qitem q "closed" in
        Lwt.return_unit)
  in
  Lwt.return_true

let test_async_read_start () : bool Lwt.t =
  let%lwt () =
    try_with_tmp (fun ~root ->
        let errors_file_path = Server_files.errors_file_path root in
        let pid = Unix.getpid () in
        Server_progress.ErrorsWrite.new_empty_file
          ~clock:None
          ~ignore_hh_version:false
          ~cancel_reason;
        let fd = Unix.openfile errors_file_path [Unix.O_RDONLY] 0 in
        (* oops! we didn't call Server_progress.ErrorsRead.openfile *)
        let exn =
          try
            let _q = Server_progress_lwt.watch_errors_file ~pid fd in
            "successfully opened queue"
          with
          | exn -> Exn.to_string exn
        in
        assert (
          String.is_substring exn ~substring:"openfile before read_next_error");
        Lwt.return_unit)
  in
  Lwt.return_true

let env =
  Client_env.
    {
      autostart = false;
      config = [];
      custom_hhi_path = None;
      custom_telemetry_data = [];
      error_format = Some Diagnostics.Plain;
      force_dormant_start = false;
      force_streaming_diagnostics = false;
      from = "test";
      show_spinner = false;
      ignore_hh_version = true;
      saved_state_ignore_hhconfig = true;
      paths = [];
      max_errors = None;
      reason = None;
      mode = Client_env.MODE_STATUS;
      no_load = true;
      save_64bit = None;
      save_human_readable_64bit_dep_map = None;
      output_json = false;
      output_jsonl = false;
      prechecked = None;
      mini_state = None;
      root = Path.dummy_path;
      sort_results = true;
      stdin_name = None;
      deadline = None;
      watchman_debug_logging = false;
      allow_non_opt_build = true;
      desc = "testing";
      preexisting_warnings = false;
      is_interactive = false;
      warning_switches = [];
      dump_config = false;
    }

let make_error_filter env =
  Filter_diagnostics.Filter.make
    ~default_all:true
    ~generated_files:[]
    env.Client_env.warning_switches

let test_check_success () : bool Lwt.t =
  let%lwt () =
    try_with_tmp (fun ~root ->
        Server_progress.write ~include_in_logs:false "test1";
        Server_progress.ErrorsWrite.new_empty_file
          ~clock:None
          ~ignore_hh_version:false
          ~cancel_reason;
        let env = { env with Client_env.root } in
        let connect = ref false in
        let connect_then_close () =
          connect := true;
          Lwt.return_unit
        in
        let partial_telemetry_ref = ref None in
        let check_future =
          Client_check_status.go_streaming
            env
            Server_local_config_load.default
            (make_error_filter env)
            ~partial_telemetry_ref
            ~connect_then_close
        in
        let%lwt () = Lwt_unix.sleep 1.0 in
        (* It should have picked up the errors-file, and seen that it's valid, and be just waiting *)
        assert (Lwt.is_sleeping check_future);
        (* Now we'll complete the errors file. This should make our check complete. *)
        Server_progress.ErrorsWrite.complete
          (Telemetry.create () |> Telemetry.string_ ~key:"hot" ~value:"potato");
        let%lwt (exit_status, telemetry) = check_future in
        let exit_status = Exit_status.show exit_status in
        let telemetry = Telemetry.to_string telemetry in
        Printf.eprintf "%s\n%s\n%!" exit_status telemetry;
        String_asserter.assert_equals "Exit_status.No_error" exit_status "x";
        Bool_asserter.assert_equals false !connect "!connect";
        assert (String.is_substring telemetry ~substring:"\"hot\":\"potato\"");
        assert (String.is_substring telemetry ~substring:"\"streaming\":true");
        Lwt.return_unit)
  in
  Lwt.return_true

let test_check_errors () : bool Lwt.t =
  let%lwt () =
    try_with_tmp (fun ~root ->
        Server_progress.write ~include_in_logs:false "test1";
        let env = { env with Client_env.root } in
        let connect_then_close () = Lwt.return_unit in
        let partial_telemetry_ref = ref None in
        let check_future =
          Client_check_status.go_streaming
            env
            Server_local_config_load.default
            (make_error_filter env)
            ~partial_telemetry_ref
            ~connect_then_close
        in
        let%lwt () = Lwt_unix.sleep 0.2 in
        Server_progress.ErrorsWrite.new_empty_file
          ~clock:None
          ~ignore_hh_version:false
          ~cancel_reason;
        Server_progress.ErrorsWrite.report
          (make_diagnostics [(101, "c", "oops")]);
        Server_progress.ErrorsWrite.complete telemetry;
        let%lwt (exit_status, _telemetry) = check_future in
        let exit_status = Exit_status.show exit_status in
        String_asserter.assert_equals "Exit_status.Type_error" exit_status "x";
        Lwt.return_unit)
  in
  Lwt.return_true

let capture_stdout f =
  let (read_fd, write_fd) = Unix.pipe () in
  let old_stdout = Unix.dup Unix.stdout in
  Unix.dup2 write_fd Unix.stdout;
  Unix.close write_fd;
  let%lwt result = f () in
  Unix.dup2 old_stdout Unix.stdout;
  Unix.close old_stdout;
  let ic = Unix.in_channel_of_descr read_fd in
  let buf = Buffer.create 256 in
  (try
     while true do
       match In_channel.input_char ic with
       | Some c -> Buffer.add_char buf c
       | None -> raise Exit
     done
   with
  | Exit -> ());
  In_channel.close ic;
  Lwt.return (result, Buffer.contents buf)

let test_check_jsonl_success () : bool Lwt.t =
  let%lwt () =
    try_with_tmp (fun ~root ->
        Server_progress.write ~include_in_logs:false "test1";
        Server_progress.ErrorsWrite.new_empty_file
          ~clock:None
          ~ignore_hh_version:false
          ~cancel_reason;
        let env =
          { env with Client_env.root; output_jsonl = true; output_json = false }
        in
        let connect_then_close () = Lwt.return_unit in
        let partial_telemetry_ref = ref None in
        let check =
          Client_check_status.go_streaming
            env
            Server_local_config_load.default
            (make_error_filter env)
            ~partial_telemetry_ref
            ~connect_then_close
        in
        let%lwt () = Lwt_unix.sleep 1.0 in
        Server_progress.ErrorsWrite.complete telemetry;
        let%lwt ((exit_status, _telemetry), stdout) =
          capture_stdout (fun () -> check)
        in
        String_asserter.assert_equals
          "Exit_status.No_error"
          (Exit_status.show exit_status)
          "exit_status";
        let lines =
          String.split_lines stdout
          |> List.filter ~f:(fun s -> not (String.is_empty s))
        in
        Int_asserter.assert_equals 1 (List.length lines) "one line (summary)";
        let summary = Yojson.Safe.from_string (List.hd_exn lines) in
        let kind =
          Yojson.Safe.Util.member "kind" summary |> Yojson.Safe.Util.to_string
        in
        let passed =
          Yojson.Safe.Util.member "passed" summary |> Yojson.Safe.Util.to_bool
        in
        String_asserter.assert_equals "summary" kind "kind";
        Bool_asserter.assert_equals true passed "passed";
        Lwt.return_unit)
  in
  Lwt.return_true

let test_check_jsonl_errors () : bool Lwt.t =
  let%lwt () =
    try_with_tmp (fun ~root ->
        Server_progress.write ~include_in_logs:false "test1";
        let env =
          { env with Client_env.root; output_jsonl = true; output_json = false }
        in
        let connect_then_close () = Lwt.return_unit in
        let partial_telemetry_ref = ref None in
        let check =
          Client_check_status.go_streaming
            env
            Server_local_config_load.default
            (make_error_filter env)
            ~partial_telemetry_ref
            ~connect_then_close
        in
        let%lwt () = Lwt_unix.sleep 0.2 in
        Server_progress.ErrorsWrite.new_empty_file
          ~clock:None
          ~ignore_hh_version:false
          ~cancel_reason;
        Server_progress.ErrorsWrite.report
          (make_diagnostics [(101, "c", "oops")]);
        Server_progress.ErrorsWrite.complete telemetry;
        let%lwt ((exit_status, _telemetry), stdout) =
          capture_stdout (fun () -> check)
        in
        String_asserter.assert_equals
          "Exit_status.Type_error"
          (Exit_status.show exit_status)
          "exit_status";
        let lines =
          String.split_lines stdout
          |> List.filter ~f:(fun s -> not (String.is_empty s))
        in
        Int_asserter.assert_equals
          2
          (List.length lines)
          "two lines (diagnostic + summary)";
        let diag = Yojson.Safe.from_string (List.nth_exn lines 0) in
        let summary = Yojson.Safe.from_string (List.nth_exn lines 1) in
        String_asserter.assert_equals
          "diagnostic"
          (Yojson.Safe.Util.member "kind" diag |> Yojson.Safe.Util.to_string)
          "diagnostic kind";
        String_asserter.assert_equals
          "error"
          (Yojson.Safe.Util.member "severity" diag |> Yojson.Safe.Util.to_string)
          "severity";
        String_asserter.assert_equals
          "summary"
          (Yojson.Safe.Util.member "kind" summary |> Yojson.Safe.Util.to_string)
          "summary kind";
        Bool_asserter.assert_equals
          false
          (Yojson.Safe.Util.member "passed" summary |> Yojson.Safe.Util.to_bool)
          "passed";
        Lwt.return_unit)
  in
  Lwt.return_true

let test_check_jsonl_warnings_only () : bool Lwt.t =
  let%lwt () =
    try_with_tmp (fun ~root ->
        Server_progress.write ~include_in_logs:false "test1";
        let env =
          { env with Client_env.root; output_jsonl = true; output_json = false }
        in
        let connect_then_close () = Lwt.return_unit in
        let partial_telemetry_ref = ref None in
        let check =
          Client_check_status.go_streaming
            env
            Server_local_config_load.default
            (make_error_filter env)
            ~partial_telemetry_ref
            ~connect_then_close
        in
        let%lwt () = Lwt_unix.sleep 0.2 in
        Server_progress.ErrorsWrite.new_empty_file
          ~clock:None
          ~ignore_hh_version:false
          ~cancel_reason;
        Server_progress.ErrorsWrite.report
          (make_warning_diagnostics [(12001, "w", "sketchy")]);
        Server_progress.ErrorsWrite.complete telemetry;
        let%lwt ((exit_status, _telemetry), stdout) =
          capture_stdout (fun () -> check)
        in
        (* Warnings only should give exit code 0 *)
        String_asserter.assert_equals
          "Exit_status.No_error"
          (Exit_status.show exit_status)
          "exit_status";
        let lines =
          String.split_lines stdout
          |> List.filter ~f:(fun s -> not (String.is_empty s))
        in
        Int_asserter.assert_equals
          2
          (List.length lines)
          "two lines (diagnostic + summary)";
        let diag = Yojson.Safe.from_string (List.nth_exn lines 0) in
        let summary = Yojson.Safe.from_string (List.nth_exn lines 1) in
        String_asserter.assert_equals
          "diagnostic"
          (Yojson.Safe.Util.member "kind" diag |> Yojson.Safe.Util.to_string)
          "diagnostic kind";
        String_asserter.assert_equals
          "warning"
          (Yojson.Safe.Util.member "severity" diag |> Yojson.Safe.Util.to_string)
          "severity";
        String_asserter.assert_equals
          "summary"
          (Yojson.Safe.Util.member "kind" summary |> Yojson.Safe.Util.to_string)
          "summary kind";
        Bool_asserter.assert_equals
          true
          (Yojson.Safe.Util.member "passed" summary |> Yojson.Safe.Util.to_bool)
          "passed";
        Int_asserter.assert_equals
          0
          (Yojson.Safe.Util.member "error_count" summary
          |> Yojson.Safe.Util.to_int)
          "error_count";
        Int_asserter.assert_equals
          1
          (Yojson.Safe.Util.member "warning_count" summary
          |> Yojson.Safe.Util.to_int)
          "warning_count";
        Lwt.return_unit)
  in
  Lwt.return_true

let test_check_jsonl_mixed_errors_and_warnings () : bool Lwt.t =
  let%lwt () =
    try_with_tmp (fun ~root ->
        Server_progress.write ~include_in_logs:false "test1";
        let env =
          { env with Client_env.root; output_jsonl = true; output_json = false }
        in
        let connect_then_close () = Lwt.return_unit in
        let partial_telemetry_ref = ref None in
        let check =
          Client_check_status.go_streaming
            env
            Server_local_config_load.default
            (make_error_filter env)
            ~partial_telemetry_ref
            ~connect_then_close
        in
        let%lwt () = Lwt_unix.sleep 0.2 in
        Server_progress.ErrorsWrite.new_empty_file
          ~clock:None
          ~ignore_hh_version:false
          ~cancel_reason;
        Server_progress.ErrorsWrite.report
          (make_diagnostics [(4110, "e", "type error")]);
        Server_progress.ErrorsWrite.report
          (make_warning_diagnostics [(12001, "w", "sketchy")]);
        Server_progress.ErrorsWrite.complete telemetry;
        let%lwt ((exit_status, _telemetry), stdout) =
          capture_stdout (fun () -> check)
        in
        (* Mixed errors+warnings should give exit code Type_error *)
        String_asserter.assert_equals
          "Exit_status.Type_error"
          (Exit_status.show exit_status)
          "exit_status";
        let lines =
          String.split_lines stdout
          |> List.filter ~f:(fun s -> not (String.is_empty s))
        in
        Int_asserter.assert_equals
          3
          (List.length lines)
          "three lines (2 diagnostics + summary)";
        let diag_error = Yojson.Safe.from_string (List.nth_exn lines 0) in
        let diag_warning = Yojson.Safe.from_string (List.nth_exn lines 1) in
        let summary = Yojson.Safe.from_string (List.nth_exn lines 2) in
        String_asserter.assert_equals
          "error"
          (Yojson.Safe.Util.member "severity" diag_error
          |> Yojson.Safe.Util.to_string)
          "error severity";
        String_asserter.assert_equals
          "warning"
          (Yojson.Safe.Util.member "severity" diag_warning
          |> Yojson.Safe.Util.to_string)
          "warning severity";
        String_asserter.assert_equals
          "summary"
          (Yojson.Safe.Util.member "kind" summary |> Yojson.Safe.Util.to_string)
          "summary kind";
        Bool_asserter.assert_equals
          false
          (Yojson.Safe.Util.member "passed" summary |> Yojson.Safe.Util.to_bool)
          "passed";
        Int_asserter.assert_equals
          1
          (Yojson.Safe.Util.member "error_count" summary
          |> Yojson.Safe.Util.to_int)
          "error_count";
        Int_asserter.assert_equals
          1
          (Yojson.Safe.Util.member "warning_count" summary
          |> Yojson.Safe.Util.to_int)
          "warning_count";
        Lwt.return_unit)
  in
  Lwt.return_true

(** Mask the "version" field in a JSON string, replacing its value with "<VERSION>".
    This lets us do full-output snapshot assertions without being sensitive to build version. *)
let mask_version (line : string) : string =
  let json = Yojson.Safe.from_string line in
  let masked =
    match json with
    | `Assoc fields ->
      `Assoc
        (List.map fields ~f:(fun (k, v) ->
             if String.equal k "version" then
               (k, `String "<VERSION>")
             else
               (k, v)))
    | other -> other
  in
  Yojson.Safe.to_string masked

let test_check_jsonl_full_output () : bool Lwt.t =
  let%lwt () =
    try_with_tmp (fun ~root ->
        Server_progress.write ~include_in_logs:false "test1";
        let env =
          { env with Client_env.root; output_jsonl = true; output_json = false }
        in
        let connect_then_close () = Lwt.return_unit in
        let partial_telemetry_ref = ref None in
        let check =
          Client_check_status.go_streaming
            env
            Server_local_config_load.default
            (make_error_filter env)
            ~partial_telemetry_ref
            ~connect_then_close
        in
        let%lwt () = Lwt_unix.sleep 0.2 in
        Server_progress.ErrorsWrite.new_empty_file
          ~clock:None
          ~ignore_hh_version:false
          ~cancel_reason;
        Server_progress.ErrorsWrite.report
          (make_diagnostics [(4110, "e.php", "type mismatch")]);
        Server_progress.ErrorsWrite.report
          (make_warning_diagnostics [(12001, "w.php", "sketchy")]);
        Server_progress.ErrorsWrite.complete telemetry;
        let%lwt ((_exit_status, _telemetry), stdout) =
          capture_stdout (fun () -> check)
        in
        let lines =
          String.split_lines stdout
          |> List.filter ~f:(fun s -> not (String.is_empty s))
        in
        Int_asserter.assert_equals
          3
          (List.length lines)
          "three lines (error + warning + summary)";
        (* Mask the version field and compare exact output *)
        let masked_lines = List.map lines ~f:mask_version in
        let actual = String.concat ~sep:"\n" masked_lines in
        (* Build the expected path prefix from root *)
        let prefix = Path.to_string root ^ "/" in
        let expected =
          Printf.sprintf
            {|{"kind":"diagnostic","severity":"error","is_trusted":true,"message":[{"descr":"type mismatch","path":"%se.php","line":0,"start":0,"end":0,"code":4110}],"custom_messages":[]}
{"kind":"diagnostic","severity":"warning","is_trusted":true,"message":[{"descr":"sketchy","path":"%sw.php","line":0,"start":0,"end":0,"code":12001}],"custom_messages":[]}
{"kind":"summary","passed":false,"version":"<VERSION>","error_count":1,"warning_count":1}|}
            prefix
            prefix
        in
        String_asserter.assert_equals expected actual "full jsonl output";
        Lwt.return_unit)
  in
  Lwt.return_true

(** This test proves that JSONL diagnostic lines are emitted to stdout
    *before* the typecheck completes — i.e. they stream incrementally. *)
let test_check_jsonl_streaming () : bool Lwt.t =
  let%lwt () =
    try_with_tmp (fun ~root ->
        Server_progress.write ~include_in_logs:false "test1";
        let env =
          { env with Client_env.root; output_jsonl = true; output_json = false }
        in
        let connect_then_close () = Lwt.return_unit in
        let partial_telemetry_ref = ref None in
        (* Set up stdout pipe redirect manually so we can read incrementally *)
        let (pipe_read_fd, pipe_write_fd) = Unix.pipe () in
        let old_stdout = Unix.dup Unix.stdout in
        Unix.dup2 pipe_write_fd Unix.stdout;
        Unix.close pipe_write_fd;
        (* Create the errors file and start streaming *)
        Server_progress.ErrorsWrite.new_empty_file
          ~clock:None
          ~ignore_hh_version:false
          ~cancel_reason;
        let check_future =
          Client_check_status.go_streaming
            env
            Server_local_config_load.default
            (make_error_filter env)
            ~partial_telemetry_ref
            ~connect_then_close
        in
        (* Give the polling loop time to start *)
        let%lwt () = Lwt_unix.sleep 0.2 in
        (* Report one error — but do NOT complete yet *)
        Server_progress.ErrorsWrite.report
          (make_diagnostics [(4110, "e.php", "type mismatch")]);
        (* The check should still be sleeping (not completed) *)
        let%lwt () = Lwt_unix.sleep 0.5 in
        assert (Lwt.is_sleeping check_future);
        (* Read what's available on the pipe so far.
           We use a non-blocking read: set the read fd to non-blocking,
           read as many bytes as available, then set it back. *)
        Unix.set_nonblock pipe_read_fd;
        let buf = Buffer.create 256 in
        (try
           let tmp = Bytes.create 4096 in
           let rec read_available () =
             let n = Unix.read pipe_read_fd tmp 0 4096 in
             if n > 0 then (
               Buffer.add_subbytes buf tmp ~pos:0 ~len:n;
               read_available ()
             )
           in
           read_available ()
         with
        | Unix.Unix_error (Unix.EAGAIN, _, _)
        | Unix.Unix_error (Unix.EWOULDBLOCK, _, _) ->
          ());
        Unix.clear_nonblock pipe_read_fd;
        let partial_stdout = Buffer.contents buf in
        (* The diagnostic line should already be there — this proves streaming *)
        assert (
          String.is_substring
            partial_stdout
            ~substring:"\"kind\":\"diagnostic\"");
        assert (
          String.is_substring partial_stdout ~substring:"\"severity\":\"error\"");
        (* There should be no summary line yet *)
        assert (
          not
            (String.is_substring
               partial_stdout
               ~substring:"\"kind\":\"summary\""));
        (* Now complete the check *)
        Server_progress.ErrorsWrite.complete telemetry;
        let%lwt (_exit_status, _telemetry) = check_future in
        (* Restore stdout *)
        Unix.dup2 old_stdout Unix.stdout;
        Unix.close old_stdout;
        (* Read remaining pipe contents *)
        let ic = Unix.in_channel_of_descr pipe_read_fd in
        let rest_buf = Buffer.create 256 in
        (try
           while true do
             match In_channel.input_char ic with
             | Some c -> Buffer.add_char rest_buf c
             | None -> raise Exit
           done
         with
        | Exit -> ());
        In_channel.close ic;
        let rest_stdout = Buffer.contents rest_buf in
        (* The summary line should now appear in the remaining output *)
        assert (
          String.is_substring rest_stdout ~substring:"\"kind\":\"summary\"");
        Lwt.return_unit)
  in
  Lwt.return_true

let test_check_connect_success () : bool Lwt.t =
  let%lwt () =
    try_with_tmp (fun ~root ->
        Server_progress.write ~include_in_logs:false "test1";
        let env = { env with Client_env.root } in
        let (future1, trigger1) = Lwt.wait () in
        let (future2, trigger2) = Lwt.wait () in
        let connect = ref "A" in
        let connect_then_close () =
          connect := "B";
          let%lwt () = future1 in
          Server_progress.ErrorsWrite.new_empty_file
            ~clock:None
            ~ignore_hh_version:false
            ~cancel_reason;
          let%lwt () = future2 in
          Lwt.return_unit
        in
        let partial_telemetry_ref = ref None in

        let check_future =
          Client_check_status.go_streaming
            env
            Server_local_config_load.default
            (make_error_filter env)
            ~partial_telemetry_ref
            ~connect_then_close
        in
        let%lwt () = Lwt_unix.sleep 1.0 in
        (* It should have found errors.bin absent, and is waiting on the callback *)
        assert (Lwt.is_sleeping check_future);
        String_asserter.assert_equals "B" !connect "!connect";
        Lwt.wakeup_later trigger1 ();
        (* Even though we created an error file, it won't proceed until the callback's done *)
        let%lwt () = Lwt_unix.sleep 1.0 in
        assert (Lwt.is_sleeping check_future);
        Lwt.wakeup_later trigger2 ();
        (* Now the check will happily have found errors.bin and is waiting on it. *)
        let%lwt () = Lwt_unix.sleep 1.0 in
        assert (Lwt.is_sleeping check_future);
        (* Let us pretend that the server exit; this will trigger check to complete, and return exit status. *)
        Server_progress.ErrorsWrite.unlink_at_server_stop ();
        let%lwt r =
          try%lwt
            let%lwt _ = check_future in
            Lwt.return_error "expected exception"
          with
          | Exit_status.(Exit_with Typecheck_abandoned) -> Lwt.return_ok ()
          | exn ->
            let e = Exception.wrap exn in
            Lwt.return_error (Exception.to_string e)
        in
        let () = Result.ok_or_failwith r in
        Lwt.return_unit)
  in
  Lwt.return_true

let test_check_connect_failure () : bool Lwt.t =
  let%lwt () =
    try_with_tmp (fun ~root ->
        Server_progress.write ~include_in_logs:false "test1";
        let env = { env with Client_env.root } in
        (* This is an async failure, so the exception is stored in check_future
           rather than being returned from ClientCheckStatus.go itself *)
        let connect_then_close () =
          let%lwt () = Lwt_unix.sleep 0.1 in
          failwith "nostart"
        in
        let partial_telemetry_ref = ref None in
        let check_future =
          Client_check_status.go_streaming
            env
            Server_local_config_load.default
            (make_error_filter env)
            ~partial_telemetry_ref
            ~connect_then_close
        in
        let%lwt r =
          try%lwt
            let%lwt _ = check_future in
            Lwt.return_error "expected exception"
          with
          | exn
            when String.is_substring (Exn.to_string exn) ~substring:"nostart" ->
            Lwt.return_ok ()
          | exn ->
            let e = Exception.wrap exn in
            Lwt.return_error (Exception.to_string e)
        in
        let () = Result.ok_or_failwith r in
        Lwt.return_unit)
  in
  Lwt.return_true

let assert_errors ~expected ~(actual : Diagnostics.finalized_diagnostic list) =
  let expected = List.sort expected ~compare:compare_simple_diagnostic in
  let actual =
    List.fold actual ~init:[] ~f:(fun acc err ->
        ( User_diagnostic.get_code err,
          User_diagnostic.get_pos err
          |> Pos.filename
          |> String.chop_prefix_exn ~prefix:(Path.to_string root ^ "/"),
          User_diagnostic.get_messages err |> List.hd_exn |> snd )
        :: acc)
    |> List.sort ~compare:compare_simple_diagnostic
  in
  if not @@ List.equal equal_simple_diagnostic expected actual then (
    Printf.eprintf
      "Expected \n%s\nbut got\n%s"
      (show_simple_diagnostic_list expected)
      (show_simple_diagnostic_list actual);
    assert false
  )

let test_filter_warnings () : bool =
  let error_filter =
    Filter_diagnostics.Filter.make
      ~default_all:true
      ~generated_files:[Str.regexp "gen/"]
      [
        Filter_diagnostics.Code_off Error_codes.Warning.SketchyEquality;
        Filter_diagnostics.Ignored_files (Str.regexp "def");
        Filter_diagnostics.Code_off Error_codes.Warning.NonDisjointCheck;
        Filter_diagnostics.Code_on Error_codes.Warning.SketchyEquality;
        Filter_diagnostics.Ignored_files (Str.regexp "abc");
      ]
  in
  let errors =
    make_diagnostics
      [
        (12001, "a", "SketchyEquality in non-ignored file. Show");
        (12004, "a", "NonDisjointCheck in non-ignored file. Hide");
        (12001, "defgh", "SketchyEquality in ignored file. Hide");
        (12004, "abcd", "other warning in ignored file. Hide");
        (4110, "abc", "non-warning in ignored file. Show");
        (4110, "gen/", "non-warning in generated file. Show");
        (12004, "gen/", "other warning in generated file. Hide");
      ]
    |> Diagnostics.sort_and_finalize
  in
  let actual = Filter_diagnostics.filter error_filter errors in
  let expected =
    [
      (12001, "a", "SketchyEquality in non-ignored file. Show");
      (4110, "abc", "non-warning in ignored file. Show");
      (4110, "gen/", "non-warning in generated file. Show");
    ]
  in
  assert_errors ~expected ~actual;
  true

let test_filter_warnings_generated () : bool =
  let error_filter =
    Filter_diagnostics.Filter.make
      ~default_all:true
      ~generated_files:[Str.regexp "gen/"; Str.regexp "gen2"]
      [
        Filter_diagnostics.Ignored_files (Str.regexp "def");
        Filter_diagnostics.Ignored_files (Str.regexp "gen2");
        Filter_diagnostics.Generated_files_on;
      ]
  in
  let errors =
    make_diagnostics
      [
        (12001, "defgh", "warning in ignored file. Hide");
        (12004, "abcd", "unrelated. show");
        (4110, "gen2/", "non-warning in ignored file. Show");
        (12004, "gen/", "warning in generated file with -Wgenerated. Show");
        ( 12004,
          "gen2/",
          "warning in generated file with -Wgenerated but ignored. Hide" );
      ]
    |> Diagnostics.sort_and_finalize
  in
  let actual = Filter_diagnostics.filter error_filter errors in
  let expected =
    [
      (12004, "abcd", "unrelated. show");
      (4110, "gen2/", "non-warning in ignored file. Show");
      (12004, "gen/", "warning in generated file with -Wgenerated. Show");
    ]
  in
  assert_errors ~expected ~actual;
  true

let test_client_connect_server_exit_should_abort () =
  let assert_should_abort ~exit_status ~expected =
    Bool_asserter.assert_equals
      expected
      (Client_connect.For_test.server_exit_should_abort exit_status)
      (Exit_status.show exit_status)
  in
  assert_should_abort ~exit_status:Exit_status.Distc_failed ~expected:true;
  assert_should_abort
    ~exit_status:Exit_status.Failed_to_load_should_abort
    ~expected:true;
  assert_should_abort
    ~exit_status:Exit_status.Server_non_opt_build_mode
    ~expected:true;
  assert_should_abort
    ~exit_status:Exit_status.Failed_to_load_should_retry
    ~expected:false;
  assert_should_abort ~exit_status:Exit_status.No_error ~expected:false;
  true

let test_percentage_message_no_denominator () =
  (* Before a distc check's typing total arrives the event loop writes
     0/0; the message must not render a nan percentage. *)
  let msg =
    Server_progress.make_percentage_progress_message
      ~operation:"hh_distc checking"
      ~done_count:0
      ~total_count:0
      ~unit:"files"
      ~extra:None
  in
  String_asserter.assert_equals
    "hh_distc checking 0/0 files "
    msg
    "no denominator, no percentage";
  let msg =
    Server_progress.make_percentage_progress_message
      ~operation:"hh_distc checking"
      ~done_count:25
      ~total_count:100
      ~unit:"files"
      ~extra:None
  in
  String_asserter.assert_equals
    "hh_distc checking 25/100 files (25.0%)"
    msg
    "with a denominator the percentage stays";
  true

let () =
  Printexc.record_backtrace true;
  Event_logger.init_fake ();
  Unit_test.run_all
    [
      ( "test_percentage_message_no_denominator",
        test_percentage_message_no_denominator );
      ("test_completed", (fun () -> Lwt_main.run (test_completed ())));
      ("test_read_empty", (fun () -> Lwt_main.run (test_read_empty ())));
      ("test_read_unlinked", (fun () -> Lwt_main.run (test_read_unlinked ())));
      ( "test_read_unlinked_empty",
        (fun () -> Lwt_main.run (test_read_unlinked_empty ())) );
      ("test_read_restarted", (fun () -> Lwt_main.run (test_read_restarted ())));
      ("test_read_halfway", (fun () -> Lwt_main.run (test_read_half_message ())));
      ("test_read_dead_pid", (fun () -> Lwt_main.run (test_read_dead_pid ())));
      ("test_locks", (fun () -> Lwt_main.run (test_locks ())));
      ( "test_produce_disordered",
        (fun () -> Lwt_main.run (test_produce_disordered ())) );
      ( "test_async_read_completed",
        (fun () -> Lwt_main.run (test_async_read_completed ())) );
      ( "test_async_read_partial",
        (fun () -> Lwt_main.run (test_async_read_partial ())) );
      ( "test_async_read_unlinked",
        (fun () -> Lwt_main.run (test_async_read_unlinked ())) );
      ( "test_start_read_killed",
        (fun () -> Lwt_main.run (test_start_read_killed ())) );
      ( "test_async_read_killed",
        (fun () -> Lwt_main.run (test_async_read_killed ())) );
      ( "test_async_pid_killed",
        (fun () -> Lwt_main.run (test_async_pid_killed ())) );
      ( "test_async_read_start",
        (fun () -> Lwt_main.run (test_async_read_start ())) );
      ("test_check_success", (fun () -> Lwt_main.run (test_check_success ())));
      ("test_check_errors", (fun () -> Lwt_main.run (test_check_errors ())));
      ( "test_check_jsonl_success",
        (fun () -> Lwt_main.run (test_check_jsonl_success ())) );
      ( "test_check_jsonl_errors",
        (fun () -> Lwt_main.run (test_check_jsonl_errors ())) );
      ( "test_check_jsonl_warnings_only",
        (fun () -> Lwt_main.run (test_check_jsonl_warnings_only ())) );
      ( "test_check_jsonl_mixed_errors_and_warnings",
        (fun () -> Lwt_main.run (test_check_jsonl_mixed_errors_and_warnings ()))
      );
      ( "test_check_jsonl_full_output",
        (fun () -> Lwt_main.run (test_check_jsonl_full_output ())) );
      ( "test_check_jsonl_streaming",
        (fun () -> Lwt_main.run (test_check_jsonl_streaming ())) );
      ( "test_check_connect_success",
        (fun () -> Lwt_main.run (test_check_connect_success ())) );
      ( "test_check_connect_failure",
        (fun () -> Lwt_main.run (test_check_connect_failure ())) );
      ( "test_client_connect_server_exit_should_abort",
        test_client_connect_server_exit_should_abort );
      ("test_filter_warnings", test_filter_warnings);
      ("test_filter_warnings_generated", test_filter_warnings_generated);
    ]
