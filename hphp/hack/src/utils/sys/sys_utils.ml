(*
 * Copyright (c) 2015, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude
module Printexc = Stdlib.Printexc

external realpath : string -> string option = "hh_realpath"

external is_nfs : string -> bool = "hh_is_nfs"

external freopen : string -> string -> Unix.file_descr -> unit = "hh_freopen"

external open_tmpfile :
  rd:bool -> wr:bool -> dir:string -> file_perm:int -> Unix.file_descr
  = "hh_open_tmpfile"

let get_env name =
  try Some (Sys.getenv name) with
  | Stdlib.Not_found -> None

(** see .mli *)
let getenv_user () =
  let user_var = "USER" in
  let logname_var = "LOGNAME" in
  let user = get_env user_var in
  let logname = get_env logname_var in
  Option.first_some user logname

let getenv_home () =
  let home_var = "HOME" in
  get_env home_var

let getenv_term () =
  let term_var = "TERM" in
  get_env term_var

let path_sep = ":"

let null_path = "/dev/null"

let temp_dir_name = "/tmp"

let getenv_path () =
  let path_var = "PATH" in
  get_env path_var

let open_in_no_fail fn =
  try In_channel.create fn with
  | e ->
    let e = Printexc.to_string e in
    Printf.fprintf stderr "Could not In_channel.create: '%s' (%s)\n" fn e;
    exit 3

let open_in_bin_no_fail fn =
  try In_channel.create ~binary:true fn with
  | e ->
    let e = Printexc.to_string e in
    Printf.fprintf
      stderr
      "Could not In_channel.create ~binary:true: '%s' (%s)\n"
      fn
      e;
    exit 3

let close_in_no_fail fn ic =
  try In_channel.close ic with
  | e ->
    let e = Printexc.to_string e in
    Printf.fprintf stderr "Could not close: '%s' (%s)\n" fn e;
    exit 3

let open_out_no_fail fn =
  try Out_channel.create fn with
  | e ->
    let e = Printexc.to_string e in
    Printf.fprintf stderr "Could not Out_channel.create: '%s' (%s)\n" fn e;
    exit 3

let open_out_bin_no_fail fn =
  try Out_channel.create ~binary:true fn with
  | e ->
    let e = Printexc.to_string e in
    Printf.fprintf
      stderr
      "Could not Out_channel.create ~binary:true: '%s' (%s)\n"
      fn
      e;
    exit 3

let close_out_no_fail fn oc =
  try Out_channel.close oc with
  | e ->
    let e = Printexc.to_string e in
    Printf.fprintf stderr "Could not close: '%s' (%s)\n" fn e;
    exit 3

let file_exists = Disk.file_exists

let cat = Disk.cat

let cat_or_failed file =
  try Some (Disk.cat file) with
  | Test_disk.No_such_file_or_directory _
  | Sys_error _
  | Failure _ ->
    None

let cat_no_fail filename =
  let ic = open_in_bin_no_fail filename in
  let len = Int64.to_int @@ In_channel.length ic in
  let len = Option.value_exn len in
  let buf = Buffer.create len in
  Stdlib.Buffer.add_channel buf ic len;
  let content = Buffer.contents buf in
  close_in_no_fail filename ic;
  content

let nl_regexp = Str.regexp "[\r\n]"

let split_lines = Str.split nl_regexp

let string_contains str substring =
  (* regexp_string matches only this string and nothing else. *)
  let re = Str.regexp_string substring in
  try Str.search_forward re str 0 >= 0 with
  | Stdlib.Not_found -> false

let exec_read cmd =
  let ic = Unix.open_process_in cmd in
  let result = In_channel.input_line ic in
  assert (Poly.(Unix.close_process_in ic = Unix.WEXITED 0));
  result

let exec_try_read cmd =
  let ic = Unix.open_process_in cmd in
  let out = In_channel.input_line ic in
  let status = Unix.close_process_in ic in
  match (out, status) with
  | (Some _, Unix.WEXITED 0) -> out
  | _ -> None

let exec_read_lines ?(reverse = false) cmd =
  let ic = Unix.open_process_in cmd in
  let result = ref [] in
  (try
     while true do
       match In_channel.input_line ic with
       | Some line -> result := line :: !result
       | None -> raise End_of_file
     done
   with
  | End_of_file -> ());
  assert (Poly.(Unix.close_process_in ic = Unix.WEXITED 0));
  if not reverse then
    List.rev !result
  else
    !result

(**
 * Collects paths that satisfy a predicate, recursively traversing directories.
 *)
let rec collect_paths path_predicate path =
  if Sys.is_directory path then
    path
    |> Sys.readdir
    |> Array.to_list
    |> List.map ~f:(Filename.concat path)
    |> List.concat_map ~f:(collect_paths path_predicate)
  else
    Utils.singleton_if (path_predicate path) path

let parse_path_list (paths : string list) : string list =
  List.concat_map paths ~f:(fun path ->
      if String.is_prefix path ~prefix:"@" then
        let path = String_utils.lstrip path "@" in
        cat path |> split_lines
      else
        [path])

let rm_dir_tree ?(skip_mocking = false) =
  if skip_mocking then
    Real_disk.rm_dir_tree
  else
    Disk.rm_dir_tree

let restart () =
  let cmd = Sys.argv.(0) in
  let argv = Sys.argv in
  Unix.execv cmd argv

(** see .mli *)
let logname_impl () =
  match getenv_user () with
  | Some user -> user
  | None ->
    (try Utils.unsafe_opt (exec_try_read "logname") with
    | Invalid_argument _ ->
      (try Utils.unsafe_opt (exec_try_read "id -un") with
      | Invalid_argument _ -> "[unknown]"))

let logname_lazy = lazy (logname_impl ())

let logname () = Lazy.force logname_lazy

let get_primary_owner () =
  let owners_file = "/etc/devserver.owners" in
  let logged_in_user = logname () in
  if not (Disk.file_exists owners_file) then
    logged_in_user
  else
    let lines = Core.String.split_lines (Disk.cat owners_file) in
    if List.mem lines logged_in_user ~equal:String.( = ) then
      logged_in_user
    else
      lines |> List.last |> Option.value ~default:logged_in_user

let with_umask umask f =
  let old_umask = ref 0 in
  Utils.with_context
    ~enter:(fun () -> old_umask := Unix.umask umask)
    ~exit:(fun () ->
      let _ = Unix.umask !old_umask in
      ())
    ~do_:f

let read_stdin_to_string () =
  let buf = Buffer.create 4096 in
  try
    while true do
      match In_channel.input_line In_channel.stdin with
      | Some line ->
        Buffer.add_string buf line;
        Buffer.add_char buf '\n'
      | None -> raise End_of_file
    done;
    assert false
  with
  | End_of_file -> Buffer.contents buf

let read_all ?(buf_size = 4096) ic =
  let buf = Buffer.create buf_size in
  (try
     while true do
       let data = Bytes.create buf_size in
       let bytes_read = In_channel.input ic ~buf:data ~pos:0 ~len:buf_size in
       if bytes_read = 0 then raise Exit;
       Buffer.add_subbytes buf data ~pos:0 ~len:bytes_read
     done
   with
  | Exit -> ());
  Buffer.contents buf

let expanduser path =
  Str.substitute_first
    (Str.regexp "^~\\([^/]*\\)")
    begin
      fun s ->
        match Str.matched_group 1 s with
        | "" -> begin
          match getenv_home () with
          | None -> (Unix.getpwuid (Unix.getuid ())).Unix.pw_dir
          | Some home -> home
        end
        | unixname ->
          (try (Unix.getpwnam unixname).Unix.pw_dir with
          | Stdlib.Not_found -> Str.matched_string s)
    end
    path

let executable_path : unit -> string =
  let executable_path_ = ref None in
  let dir_sep = Filename.dir_sep.[0] in
  let search_path path =
    let paths =
      match getenv_path () with
      | None -> failwith "Unable to determine executable path"
      | Some paths -> Str.split (Str.regexp_string path_sep) paths
    in
    let path =
      List.fold_left
        paths
        ~f:
          begin
            fun acc p ->
              match acc with
              | Some _ -> acc
              | None -> realpath (expanduser (Filename.concat p path))
          end
        ~init:None
    in
    match path with
    | Some path -> path
    | None -> failwith "Unable to determine executable path"
  in
  fun () ->
    match !executable_path_ with
    | Some path -> path
    | None ->
      let path = Sys.executable_name in
      let path =
        if String.contains path dir_sep then
          match realpath path with
          | Some path -> path
          | None -> failwith "Unable to determine executable path"
        else
          search_path path
      in
      executable_path_ := Some path;
      path

let lines_of_in_channel ic =
  let rec loop accum =
    match In_channel.input_line ic with
    | None -> List.rev accum
    | Some line -> loop (line :: accum)
  in
  loop []

let lines_of_file filename =
  let ic = In_channel.create filename in
  try
    let result = lines_of_in_channel ic in
    let () = In_channel.close ic in
    result
  with
  | _ ->
    In_channel.close ic;
    []

let read_file file =
  let ic = In_channel.create ~binary:true file in
  let size = Int64.to_int @@ In_channel.length ic in
  let size = Option.value_exn size in
  let buf = Bytes.create size in
  Option.value_exn (In_channel.really_input ic ~buf ~pos:0 ~len:size);
  In_channel.close ic;
  buf

let write_file ~file s = Disk.write_file ~file ~contents:s

let append_file ~file s =
  let chan = Out_channel.create ~append:true ~perm:0o666 file in
  Out_channel.output_string chan s;
  Out_channel.close chan

let write_strings_to_file ~file (ss : string list) =
  let chan = Out_channel.create ~perm:0o666 file in
  List.iter ~f:(Out_channel.output_string chan) ss;
  Out_channel.close chan

(* could be in control section too *)

let mkdir_p ?(skip_mocking = false) =
  if skip_mocking then
    Real_disk.mkdir_p
  else
    Disk.mkdir_p

let mkdir_no_fail dir =
  with_umask 0 (fun () ->
      (* Don't set sticky bit since the socket opening code wants to remove any
       * old sockets it finds, which may be owned by a different user. *)
      try Unix.mkdir dir 0o777 with
      | Unix.Unix_error (Unix.EEXIST, _, _) -> ())

let unlink_no_fail fn =
  try Unix.unlink fn with
  | Unix.Unix_error (Unix.ENOENT, _, _) -> ()

let readlink_no_fail fn =
  try Unix.readlink fn with
  | _ -> fn

let filemtime file = (Unix.stat file).Unix.st_mtime

external lutimes : string -> unit = "hh_lutimes"

(** see .mli *)
type touch_mode =
  | Touch_existing of { follow_symlinks: bool }
  | Touch_existing_file_or_create_new of {
      mkdir_if_new: bool;
      perm_if_new: Unix.file_perm;
    }

let touch mode file =
  match mode with
  | Touch_existing { follow_symlinks = true } -> Unix.utimes file 0. 0.
  | Touch_existing { follow_symlinks = false } -> lutimes file
  | Touch_existing_file_or_create_new { mkdir_if_new; perm_if_new } ->
    with_umask 0o000 (fun () ->
        if mkdir_if_new then mkdir_no_fail (Filename.dirname file);
        let oc =
          Out_channel.create ~append:true ~binary:true ~perm:perm_if_new file
        in
        Out_channel.close oc)

let try_touch mode file =
  try touch mode file with
  | _ -> ()

let splitext filename =
  let root = Filename.chop_extension filename in
  let root_length = String.length root in
  (* -1 because the extension includes the period, e.g. ".foo" *)
  let ext_length = String.length filename - root_length - 1 in
  let ext = String.sub filename ~pos:(root_length + 1) ~len:ext_length in
  (root, ext)

let prod_telemetry () =
  match Sys.getenv_opt "HH_TEST_MODE" with
  | Some "0" -> true
  | Some _ -> false
  | None -> not Build_id.is_dev_build

let deterministic_behavior_for_tests () = not (prod_telemetry ())

let sleep ~seconds = ignore @@ Unix.select [] [] [] seconds

let symlink
    (* 4.03 adds an optional argument to Unix.symlink that we want to ignore
     *)
      source
    dest =
  Unix.symlink source dest

let make_link_of_timestamped linkname =
  Unix.(
    let dir = Filename.dirname linkname in
    mkdir_p dir;
    let base = Filename.basename linkname in
    let (base, ext) = splitext base in
    let dir = Filename.concat dir (Printf.sprintf "%ss" ext) in
    mkdir_p dir;
    let tm = localtime (time ()) in
    let year = tm.tm_year + 1900 in
    let time_str =
      Printf.sprintf
        "%d-%02d-%02d-%02d-%02d-%02d"
        year
        (tm.tm_mon + 1)
        tm.tm_mday
        tm.tm_hour
        tm.tm_min
        tm.tm_sec
    in
    let filename =
      Filename.concat dir (Printf.sprintf "%s-%s.%s" base time_str ext)
    in
    unlink_no_fail linkname;
    symlink filename linkname;
    filename)

let setsid = Unix.setsid

let set_signal = Sys.set_signal

let signal a b = ignore (Sys.signal a b)

type sysinfo = {
  uptime: int;
  totalram: int;
  freeram: int;
  sharedram: int;
  bufferram: int;
  totalswap: int;
  freeswap: int;
  totalhigh: int;
  freehigh: int;
}

external sysinfo : unit -> sysinfo option = "hh_sysinfo"

external nprocs : unit -> int = "hh_nproc"

let nbr_procs = nprocs ()

let total_ram =
  Option.value_map (sysinfo ()) ~default:0 ~f:(fun si -> si.totalram)

let uptime () =
  Option.value_map (sysinfo ()) ~default:0 ~f:(fun si -> si.uptime)

external set_priorities : cpu_priority:int -> io_priority:int -> unit
  = "hh_set_priorities"

let terminate_process pid = Unix.kill pid Sys.sigkill

let lstat = Unix.lstat

let name_of_signal = function
  | s when s = Sys.sigabrt -> "SIGABRT (Abnormal termination)"
  | s when s = Sys.sigalrm -> "SIGALRM (Timeout)"
  | s when s = Sys.sigfpe -> "SIGFPE (Arithmetic exception)"
  | s when s = Sys.sighup -> "SIGHUP (Hangup on controlling terminal)"
  | s when s = Sys.sigill -> "SIGILL (Invalid hardware instruction)"
  | s when s = Sys.sigint -> "SIGINT (Interactive interrupt (ctrl-C))"
  | s when s = Sys.sigkill -> "SIGKILL (Termination)"
  | s when s = Sys.sigpipe -> "SIGPIPE (Broken pipe)"
  | s when s = Sys.sigquit -> "SIGQUIT (Interactive termination)"
  | s when s = Sys.sigsegv -> "SIGSEGV (Invalid memory reference)"
  | s when s = Sys.sigterm -> "SIGTERM (Termination)"
  | s when s = Sys.sigusr1 -> "SIGUSR1 (Application-defined signal 1)"
  | s when s = Sys.sigusr2 -> "SIGUSR2 (Application-defined signal 2)"
  | s when s = Sys.sigchld -> "SIGCHLD (Child process terminated)"
  | s when s = Sys.sigcont -> "SIGCONT (Continue)"
  | s when s = Sys.sigstop -> "SIGSTOP (Stop)"
  | s when s = Sys.sigtstp -> "SIGTSTP (Interactive stop)"
  | s when s = Sys.sigttin -> "SIGTTIN (Terminal read from background process)"
  | s when s = Sys.sigttou -> "SIGTTOU (Terminal write from background process)"
  | s when s = Sys.sigvtalrm -> "SIGVTALRM (Timeout in virtual time)"
  | s when s = Sys.sigprof -> "SIGPROF (Profiling interrupt)"
  | s when s = Sys.sigbus -> "SIGBUS (Bus error)"
  | s when s = Sys.sigpoll -> "SIGPOLL (Pollable event)"
  | s when s = Sys.sigsys -> "SIGSYS (Bad argument to routine)"
  | s when s = Sys.sigtrap -> "SIGTRAP (Trace/breakpoint trap)"
  | s when s = Sys.sigurg -> "SIGURG (Urgent condition on socket)"
  | s when s = Sys.sigxcpu -> "SIGXCPU (Timeout in cpu time)"
  | s when s = Sys.sigxfsz -> "SIGXFSZ (File size limit exceeded)"
  | other -> string_of_int other

type cpu_info = {
  cpu_user: float;
  cpu_nice_user: float;
  cpu_system: float;
  cpu_idle: float;
}

type processor_info = {
  proc_totals: cpu_info;
  proc_per_cpu: cpu_info array;
}

external processor_info : unit -> processor_info = "hh_processor_info"

let string_of_fd (fd : Unix.file_descr) : string =
  string_of_int (Core_unix.File_descr.to_int fd)

let show_inode (fd : Unix.file_descr) : string =
  let stat = Unix.fstat fd in
  Printf.sprintf "inode#%d:%d" stat.Unix.st_dev stat.Unix.st_ino

let rec select_non_intr read write exn timeout =
  let start_time = Unix.gettimeofday () in
  try Unix.select read write exn timeout with
  | Unix.Unix_error (Unix.EINTR, _, _) ->
    (* Negative timeouts mean no timeout *)
    let timeout =
      if Float.(timeout < 0.0) then
        (* A negative timeout means no timeout, i.e. unbounded wait. *)
        timeout
      else
        Float.(max 0.0 (timeout -. (Unix.gettimeofday () -. start_time)))
    in
    select_non_intr read write exn timeout
  | Unix.Unix_error (Unix.EINVAL, fun_name, "") ->
    (* There are suspicions that we are receiving this exception because we are
     * leaking file descriptors and end up using fds beyond the allowable
     * range of 1024, so this intends to log all of the file descriptors that
     * we call select on to verify those suspicions.
     *)
    let fdl_to_str fdl =
      "["
      ^ List.fold_left
          ~f:(fun init fd -> init ^ string_of_fd fd ^ ", ")
          ~init:""
          fdl
      ^ "]"
    in
    let fun_params =
      Printf.sprintf
        "%s %s %s %f"
        (fdl_to_str read)
        (fdl_to_str write)
        (fdl_to_str exn)
        timeout
    in
    raise (Unix.Unix_error (Unix.EINVAL, fun_name, fun_params))

let rec restart_on_EINTR f x =
  try f x with
  | Unix.Unix_error (Unix.EINTR, _, _) -> restart_on_EINTR f x

let write_non_intr (fd : Unix.file_descr) (buf : bytes) (pos : int) (len : int)
    : unit =
  let pos = ref pos in
  while !pos < len do
    (* must use Unix.single_write, not Unix.write, because the latter does arbitrarily
       many OS calls and we don't know which one got the EINTR *)
    let n = restart_on_EINTR (Unix.single_write fd buf !pos) (len - !pos) in
    pos := !pos + n
  done

let read_non_intr (fd : Unix.file_descr) (len : int) : bytes option =
  let buf = Bytes.create len in
  let pos = ref 0 in
  let is_eof = ref false in
  while !pos < len && not !is_eof do
    let n = restart_on_EINTR (Unix.read fd buf !pos) (len - !pos) in
    pos := !pos + n;
    if n = 0 then is_eof := true
  done;
  if !is_eof then
    None
  else
    Some buf

let rec waitpid_non_intr flags pid =
  try Unix.waitpid flags pid with
  | Unix.Unix_error (Unix.EINTR, _, _) -> waitpid_non_intr flags pid

let find_oom_in_dmesg_output pid name lines =
  (* oomd: "Memory cgroup out of memory: Killed process 4083583 (hh_server)" (https://facebookmicrosites.github.io/oomd/)
     oomkiller: "Out of memory: Kill process 4083583 (hh_server)" *)
  let re =
    Str.regexp
      (Printf.sprintf
         "[Oo]ut of memory: Kill\\(ed\\)? process \\([0-9]+\\) (%s)"
         name)
  in
  let pid = string_of_int pid in
  List.exists lines ~f:(fun line ->
      try
        ignore @@ Str.search_forward re line 0;
        let pid_s = Str.matched_group 2 line in
        String.equal pid_s pid
      with
      | Stdlib.Not_found -> false)

let check_dmesg_for_oom pid name =
  let dmesg = exec_read_lines ~reverse:true "dmesg" in
  find_oom_in_dmesg_output pid name dmesg

(* Be careful modifying the rusage type! Like other types that interact with C, the order matters!
 * If you change things here you must update hh_getrusage too! *)
type rusage = {
  ru_maxrss: int;
  (* maximum resident set size *)
  ru_ixrss: int;
  (* integral shared memory size *)
  ru_idrss: int;
  (* integral unshared data size *)
  ru_isrss: int;
  (* integral unshared stack size *)
  ru_minflt: int;
  (* page reclaims (soft page faults) *)
  ru_majflt: int;
  (* page faults (hard page faults) *)
  ru_nswap: int;
  (* swaps *)
  ru_inblock: int;
  (* block input operations *)
  ru_oublock: int;
  (* block output operations *)
  ru_msgsnd: int;
  (* IPC messages sent *)
  ru_msgrcv: int;
  (* IPC messages received *)
  ru_nsignals: int;
  (* signals received *)
  ru_nvcsw: int;
  (* voluntary context switches *)
  ru_nivcsw: int; (* involuntary context switches *)
}

external getrusage : unit -> rusage = "hh_getrusage"

external start_gc_profiling : unit -> unit = "hh_start_gc_profiling" [@@noalloc]

external get_gc_time : unit -> float * float = "hh_get_gc_time"

module For_test = struct
  let find_oom_in_dmesg_output = find_oom_in_dmesg_output
end

let atomically_create_and_init_file
    (path : string)
    ~(rd : bool)
    ~(wr : bool)
    (file_perm : Unix.file_perm)
    ~(init : Unix.file_descr -> unit) : Unix.file_descr option =
  (* There's no way to pass O_TMPFILE to Unix.openfile. So, we need our own version... *)
  let fd = open_tmpfile ~rd ~wr ~dir:(Filename.dirname path) ~file_perm in
  try
    init fd;

    (* If the user's callback worked, we can proceed to atomically move the file into place.
       If this fails due to pre-existing, then we'll return None.
       Implementation of Unix.link is at
       https://github.com/ocaml/ocaml/blob/trunk/otherlibs/unix/link_unix.c.
       The ~follow:true flag makes it do
       linkat(AT_FDCWD, "/proc/self/fd/<fd>", AT_FDCWD, filename, AT_SYMLINK_FOLLOW) *)

    (* Here is documentation from "man 2 open": https://man7.org/linux/man-pages/man2/open.2.html
       Note that it only exists on linux (since 2013) and not on other unix platforms.

       O_TMPFILE must be specified with one of O_RDWR or O_WRONLY and, optionally, O_EXCL. If O_EXCL
       is not specified, then linkat(2) can be used to link the temporary file into the filesystem,
       making it permanent, using code like the following:
         char path[PATH_MAX];
         fd = open("/path/to/dir", O_TMPFILE | O_RDWR, S_IRUSR | S_IWUSR);
         /* File I/O on 'fd'... */
         snprintf(path, PATH_MAX,  "/proc/self/fd/%d", fd);
         linkat(AT_FDCWD, path, AT_FDCWD, "/path/for/file", AT_SYMLINK_FOLLOW);

       There are two main use cases for O_TMPFILE:

       * Improved tmpfile(3) functionality: race-free creation of temporary files that (1) are
       automatically deleted when closed; (2) can never be reached via any pathname; (3) are not
       subject to symlink attacks; and (4) do not require the caller to devise unique names.
       *  Creating a file that is initially invisible, which is then populated with data and adjusted to
       have appropriate filesystem attributes (fchown(2), fchmod(2), fsetxattr(2), etc.) before being
       atomically linked into the filesystem in a fully formed state (using linkat(2) as described above)
    *)
    begin
      try
        Unix.link
          ~follow:true
          (Printf.sprintf "/proc/self/fd/%s" (string_of_fd fd))
          path;
        Some fd
      with
      | Unix.Unix_error (Unix.EEXIST, _, _) ->
        Unix.close fd;
        None
    end
  with
  | exn ->
    let e = Exception.wrap exn in
    Unix.close fd;
    Exception.reraise e

let protected_read_exn (filename : string) : string =
  (* TODO(ljw): this function is vulnerable to EINTR. Although we only hit it about once a month. *)
  (* TODO(ljw): we should use atomic-create for this! *)

  (* We can't use the standard Disk.cat because we need to read from an existing (locked)
     fd for the file; not open the file a second time and read from that. *)
  let cat_from_fd (fd : Unix.file_descr) : string =
    let total = Unix.lseek fd 0 Unix.SEEK_END in
    let _0 = Unix.lseek fd 0 Unix.SEEK_SET in
    let buf = Bytes.create total in
    let rec rec_read offset =
      if offset = total then
        ()
      else
        let bytes_read = Unix.read fd buf offset (total - offset) in
        if bytes_read = 0 then raise End_of_file;
        rec_read (offset + bytes_read)
    in
    rec_read 0;
    Bytes.to_string buf
  in
  (* Unix has no way to atomically create a file and lock it; fnctl inherently
     only works on an existing file. There's therefore a race where the writer
     might create the file before locking it, but we get our read lock in first.
     We'll work around this with a hacky sleep+retry. Other solutions would be
     to have the file always exist, or to use a separate .lock file. *)
  let rec retry_if_empty () =
    let fd = Unix.openfile filename [Unix.O_RDONLY] 0o666 in
    let content =
      Utils.try_finally
        ~f:(fun () ->
          Unix.lockf fd Unix.F_RLOCK 0;
          cat_from_fd fd)
        ~finally:(fun () -> Unix.close fd)
    in
    if String.is_empty content then
      let () = Unix.sleepf 0.001 in
      retry_if_empty ()
    else
      content
  in
  retry_if_empty ()

let protected_write_exn (filename : string) (content : string) : unit =
  if String.is_empty content then failwith "Empty content not supported.";
  with_umask 0o000 (fun () ->
      let fd = Unix.openfile filename [Unix.O_RDWR; Unix.O_CREAT] 0o666 in
      Utils.try_finally
        ~f:(fun () ->
          Unix.lockf fd Unix.F_LOCK 0;
          let _written =
            Unix.write_substring fd content 0 (String.length content)
          in
          Unix.ftruncate fd (String.length content))
        ~finally:(fun () -> Unix.close fd))

let with_lock
    (fd : Unix.file_descr) (lock_command : Unix.lock_command) ~(f : unit -> 'a)
    =
  (* helper function to set current file position to 0, then do "f",
     then restore it to what it was before *)
  let with_pos0 ~f =
    let pos = Unix.lseek fd 0 Unix.SEEK_CUR in
    let _ = Unix.lseek fd 0 Unix.SEEK_SET in
    let result = f () in
    let _ = Unix.lseek fd pos Unix.SEEK_SET in
    result
  in

  Utils.try_finally
    ~f:(fun () ->
      (* lockf is applied starting at the file-descriptors current position.
         We use "with_pos0" so that when we acquire or release the lock,
         we're locking from the start of the file through to (len=0) the end. *)
      with_pos0 ~f:(fun () -> restart_on_EINTR (Unix.lockf fd lock_command) 0);
      f ())
    ~finally:(fun () ->
      with_pos0 ~f:(fun () -> restart_on_EINTR (Unix.lockf fd Unix.F_ULOCK) 0);
      ())

let redirect_stdout_and_stderr_to_file (filename : string) : unit =
  let old_stdout = Unix.dup Unix.stdout in
  let old_stderr = Unix.dup Unix.stderr in
  Utils.try_finally
    ~finally:(fun () ->
      (* Those two old_* handles must be closed so as not to have dangling FDs.
         Neither success nor failure path holds onto them: the success path ignores
         then, and the failure path dups them. That's why we can close them here. *)
      (try Unix.close old_stdout with
      | _ -> ());
      (try Unix.close old_stderr with
      | _ -> ());
      ())
    ~f:(fun () ->
      try
        (* We want both stdout and stderr to be redirected to the same file.
           Do not attempt to open a file that's already open:
           https://wiki.sei.cmu.edu/confluence/display/c/FIO24-C.+Do+not+open+a+file+that+is+already+open
           Use dup for this scenario instead:
           https://stackoverflow.com/questions/15155314/redirect-stdout-and-stderr-to-the-same-file-and-restore-it *)
        freopen filename "w" Unix.stdout;
        Unix.dup2 Unix.stdout Unix.stderr;
        ()
      with
      | exn ->
        let e = Exception.wrap exn in
        (try Unix.dup2 old_stdout Unix.stdout with
        | _ -> ());
        (try Unix.dup2 old_stderr Unix.stderr with
        | _ -> ());
        Exception.reraise e)
