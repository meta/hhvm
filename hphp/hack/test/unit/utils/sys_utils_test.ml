(*
 * Copyright (c) 2015, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *
 *)

open Hh_prelude

let restore_std (f : unit -> 'a) : 'a =
  let old_stdout = Unix.dup Unix.stdout in
  let old_stderr = Unix.dup Unix.stderr in
  let finally () =
    Unix.dup2 old_stdout Unix.stdout;
    Unix.dup2 old_stderr Unix.stderr;
    ()
  in
  Utils.try_finally ~f ~finally

let test_freopen_failure1 () =
  restore_std @@ fun () ->
  try
    Sys_utils.freopen "." "w" Unix.stdout;
    Printf.eprintf "Expected Unix.EISDIR out of freopen . w, not success";
    false
  with
  | Unix.Unix_error (Unix.EISDIR, _, _) -> true
  | e ->
    Printf.eprintf
      "Expected Unix.EISDIR out of freopen . w, not %s"
      (Exn.to_string e);
    false

let test_freopen_failure2 () =
  restore_std @@ fun () ->
  Tempfile.with_real_tempdir (fun path ->
      let fn = Path.concat path "freopen.txt" |> Path.to_string in
      try
        Sys_utils.freopen fn "~az!" Unix.stdout;
        Printf.eprintf "Expected Unix.EINVAL out of freopen . ~az!, not success";
        false
      with
      | Unix.Unix_error (Unix.EINVAL, _, _) -> true
      | e ->
        Printf.eprintf
          "Expected Unix.EINVAL out of freopen . ~az!, not %s"
          (Exn.to_string e);
        false)

let test_freopen_success () =
  Tempfile.with_real_tempdir (fun path ->
      let fn = Path.concat path "freopen.txt" |> Path.to_string in
      Printf.printf "hello\n%!";
      restore_std (fun () ->
          Sys_utils.freopen fn "w" Unix.stdout;
          Printf.printf "there\n%!";
          Printf.eprintf "world\n%!");
      Asserter.String_asserter.assert_equals
        "there\n"
        (Disk.cat fn)
        "Expected stdout to be redirected to file";
      ());
  true

let test_redirect_stdout_and_stderr_to_file_failure () =
  Tempfile.with_real_tempdir (fun path ->
      let fnout = Path.concat path "out.txt" |> Path.to_string in
      let fnerr = Path.concat path "err.txt" |> Path.to_string in
      restore_std (fun () ->
          Sys_utils.freopen fnout "w" Unix.stdout;
          Sys_utils.freopen fnerr "w" Unix.stderr;
          Printf.printf "out.%!";
          Printf.eprintf "err.%!";
          begin
            try Sys_utils.redirect_stdout_and_stderr_to_file "." with
            | _ -> Printf.eprintf "exn.%!"
          end;
          Printf.printf "there.%!";
          Printf.eprintf "world.%!";
          ());
      Asserter.String_asserter.assert_equals
        "err.exn.world."
        (Disk.cat fnerr)
        "Stderr should have been restored upon redirect failure";
      Asserter.String_asserter.assert_equals
        "out.there."
        (Disk.cat fnout)
        "Stdout should have been restored upon redirect failure";
      ());
  true

let test_redirect_stdout_and_stderr_to_file_success () =
  Tempfile.with_real_tempdir (fun path ->
      let fn = Path.concat path "freopen.txt" |> Path.to_string in
      Printf.printf "hello.%!";
      restore_std (fun () ->
          Sys_utils.redirect_stdout_and_stderr_to_file fn;
          Printf.printf "there.%!";
          Printf.eprintf "world.%!");
      Asserter.String_asserter.assert_equals
        "there.world."
        (Disk.cat fn)
        "Stdout+stderr should have been redirected to file";
      ());
  true

let test_non_intr () =
  Tempfile.with_real_tempdir (fun dir ->
      let path = Path.concat dir "a.txt" |> Path.to_string in
      let buf1a = Bytes.make 100000 'h' in
      let buf1b = Bytes.make 200000 'w' in
      let fd = Unix.openfile path [Unix.O_WRONLY; Unix.O_CREAT] 0o666 in
      Sys_utils.write_non_intr fd buf1a 0 0;
      Sys_utils.write_non_intr fd buf1a 0 (Bytes.length buf1a);
      Sys_utils.write_non_intr fd buf1b 0 (Bytes.length buf1b);
      Unix.close fd;
      (* can we read it okay? *)
      let fd = Unix.openfile path [Unix.O_RDONLY] 0o666 in
      let buf2x = Sys_utils.read_non_intr fd 0 in
      let buf2a = Sys_utils.read_non_intr fd (Bytes.length buf1a) in
      let buf2b = Sys_utils.read_non_intr fd (Bytes.length buf1b) in
      let buf2y = Sys_utils.read_non_intr fd 0 in
      let buf2z = Sys_utils.read_non_intr fd 1 in
      Unix.close fd;
      assert (buf2x |> Option.value_exn |> Bytes.length = 0);
      assert (Bytes.equal buf1a (Option.value_exn buf2a));
      assert (Bytes.equal buf1b (Option.value_exn buf2b));
      assert (buf2y |> Option.value_exn |> Bytes.length = 0);
      assert (Option.is_none buf2z);
      ());
  true

let test_atomically_create () =
  Tempfile.with_real_tempdir (fun dir ->
      let buf = Bytes.of_string "hello" in
      let len = Bytes.length buf in

      (* Does it create ok in the success path? *)
      let path = Path.concat dir "test.txt" |> Path.to_string in
      let fd =
        Sys_utils.atomically_create_and_init_file
          path
          ~rd:true
          ~wr:true
          0o666
          ~init:(fun fd -> Sys_utils.write_non_intr fd buf 0 len)
      in
      Unix.close (Option.value_exn fd);
      let fd = Unix.openfile path [Unix.O_RDONLY] 0o666 in
      let buf2 = Sys_utils.read_non_intr fd len in
      assert (Bytes.equal buf (Option.value_exn buf2));
      Unix.close fd;

      (* Does it fail to create if a file pre-exists? *)
      let fd =
        Sys_utils.atomically_create_and_init_file
          path
          ~rd:true
          ~wr:true
          0o666
          ~init:(fun _fd -> ())
      in
      assert (Option.is_none fd);
      ());
  true

let test_lock_fd () =
  Tempfile.with_real_tempdir (fun dir ->
      let path = Path.concat dir "lock" |> Path.to_string in
      assert (Lock.fd_of path = -1);
      assert (Lock.grab path);
      Utils.try_finally
        ~f:(fun () ->
          let fd = Core_unix.File_descr.of_int (Lock.fd_of path) in
          let fd_stat = Unix.fstat fd in
          let path_stat = Unix.stat path in
          assert (fd_stat.Unix.st_dev = path_stat.Unix.st_dev);
          assert (fd_stat.Unix.st_ino = path_stat.Unix.st_ino))
        ~finally:(fun () -> assert (Lock.release path)));
  true

let tests =
  [
    ("test_freopen_failure1", test_freopen_failure1);
    ("test_freopen_failure2", test_freopen_failure2);
    ("test_freopen_success", test_freopen_success);
    ( "test_redirect_stdout_and_stderr_to_file_failure",
      test_redirect_stdout_and_stderr_to_file_failure );
    ( "test_redirect_stdout_and_stderr_to_file_success",
      test_redirect_stdout_and_stderr_to_file_success );
    ("test_non_intr", test_non_intr);
    ("test_atomically_create", test_atomically_create);
    ("test_lock_fd", test_lock_fd);
  ]

let () = Unit_test.run_all tests
