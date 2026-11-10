(*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

external realpath : string -> string option = "hh_realpath"

external is_nfs : string -> bool = "hh_is_nfs"

(** E.g. freopen "file.txt" "a" Unix.stdout will redirect stdout to the file. *)
external freopen : string -> string -> Unix.file_descr -> unit = "hh_freopen"

(** Option type intead of exception throwing. *)
val get_env : string -> string option

(** Get the user from the $USER or $USERNAME or $LOGNAME environment variable. *)
val getenv_user : unit -> string option

val getenv_home : unit -> string option

val getenv_term : unit -> string option

val path_sep : string

val null_path : string

val temp_dir_name : string

val getenv_path : unit -> string option

(** This is a string "fd123:456" which uniquely identifies this file's inode *)
val show_inode : Unix.file_descr -> string

val open_in_no_fail : string -> in_channel

val open_in_bin_no_fail : string -> in_channel

val close_in_no_fail : string -> in_channel -> unit

val open_out_no_fail : string -> out_channel

val open_out_bin_no_fail : string -> out_channel

val close_out_no_fail : string -> out_channel -> unit

val file_exists : string -> bool

val cat : string -> string

val cat_or_failed : string -> string option

val cat_no_fail : string -> string

val nl_regexp : Str.regexp

val split_lines : string -> string list

(** Returns true if substring occurs somewhere inside str. *)
val string_contains : string -> string -> bool

val exec_read : string -> string option

val exec_try_read : string -> string option

val exec_read_lines : ?reverse:bool -> string -> string list

val collect_paths : (string -> bool) -> string -> string list

(**
 * Sometimes the user wants to pass a list of paths on the command-line.
 * However, we have enough files in the codebase that sometimes that list
 * exceeds the maximum number of arguments that can be passed on the
 * command-line. To work around this, we can use the convention that some Unix
 * tools use: a `@` before a path name represents a file that should be read
 * to get the necessary information (in this case, containing a list of files,
 * one per line).
 *)
val parse_path_list : string list -> string list

val rm_dir_tree : ?skip_mocking:bool -> string -> unit

val restart : unit -> 'a

(** Try to get the current system username from environment variables or commands such as `logname` *)
val logname_impl : unit -> string

val logname : unit -> string

val get_primary_owner : unit -> string

val with_umask : int -> (unit -> 'a) -> 'a

val read_stdin_to_string : unit -> string

val read_all : ?buf_size:int -> in_channel -> string

(** Like Python's os.path.expanduser, though probably doesn't cover some
cases. Roughly follow's bash's tilde expansion:
http://www.gnu.org/software/bash/manual/html_node/Tilde-Expansion.html

    ~/foo -> /home/bob/foo if $HOME = "/home/bob"
    ~joe/foo -> /home/joe/foo if joe's home is /home/joe
*)
val expanduser : string -> string

(** Turns out it's surprisingly complex to figure out the path to the current
executable, which we need in order to extract its embedded libraries. If
argv[0] is a path, then we can use that; sometimes it's just the exe name, so
we have to search $PATH for it the same way shells do. for example:
https://www.gnu.org/software/bash/manual/html_node/Command-Search-and-Execution.html

There are other options which might be more reliable when they exist, like
using the `_` env var set by bash, or /proc/self/exe on Linux, but they are
not portable. *)
val executable_path : unit -> string

val lines_of_in_channel : in_channel -> string list

val lines_of_file : string -> string list

val read_file : string -> bytes

val write_file : file:string -> string -> unit

val append_file : file:string -> string -> unit

val write_strings_to_file : file:string -> string list -> unit

val mkdir_p : ?skip_mocking:bool -> string -> unit

(** Emulate "mkdir -p", i.e., no error if already exists. *)
val mkdir_no_fail : string -> unit

val unlink_no_fail : string -> unit

val readlink_no_fail : string -> string

val filemtime : string -> float

external lutimes : string -> unit = "hh_lutimes"

type touch_mode =
  | Touch_existing of { follow_symlinks: bool }
      (** Can be used on files and folders. This won't open/close fds, which is important for some callers. *)
  | Touch_existing_file_or_create_new of {
      mkdir_if_new: bool;
      perm_if_new: Unix.file_perm;
    }  (** Only to be used for files, not folders *)

val touch : touch_mode -> string -> unit

val try_touch : touch_mode -> string -> unit

val splitext : string -> string * string

(* HH_TEST_MODE env var redirects HackEventLogger to a different table and
 * backs the [deterministic_behavior_for_tests] helper below *)
val prod_telemetry : unit -> bool

(** This flag controls whether at runtime we pick A/B experiments to test;
if not, then the output behavior will be solely a function of the inputs
(environment variables, hh.conf, .hhconfig, command-line arguments, repository).
It also controls whether certain key lines out output e.g. times or upload URLs,
which tests can't avoid looking at, will print fixed outputs. *)
val deterministic_behavior_for_tests : unit -> bool

val sleep : seconds:float -> unit

val symlink : string -> string -> unit

(** Creates a symlink at `<dir>/<linkname.ext>` to
`<dir>/<pluralized ext>/<linkname>-<timestamp>.<ext>` *)
val make_link_of_timestamped : string -> string

val setsid : unit -> int

val set_signal : int -> Sys.signal_behavior -> unit

val signal : int -> Sys.signal_behavior -> unit

(** as reported by sysinfo().uptime. Is 0 if not on linux. *)
val uptime : unit -> int

(** in bytes, as reported by sysinfo().totalram. Is 0 if not on linux. *)
val total_ram : int

(** returns the number of processors (which includes hyperthreads),
as reported by  sysconf(_SC_NPROCESSORS_ONLN) *)
val nbr_procs : int

external set_priorities : cpu_priority:int -> io_priority:int -> unit
  = "hh_set_priorities"

val terminate_process : int -> unit

val lstat : string -> Unix.stats

val name_of_signal : int -> string

(** The units for each of these fields is seconds, similar to Unix.times().
`cpu_info` and `processor_info` are constructed by c code (see
`processor_info.c`) so be very careful modifying these types! *)
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

(** returns None if not on linux. *)
external sysinfo : unit -> sysinfo option = "hh_sysinfo"

(** Calls Unix.select but ignores EINTR, i.e. retries select with
    an adjusted timeout upon EINTR.
    We implement timers using sigalarm which means selects can be
    interrupted. This is a wrapper around EINTR which continues the select if it
    gets interrupted by a signal *)
val select_non_intr :
  Unix.file_descr list ->
  Unix.file_descr list ->
  Unix.file_descr list ->
  float ->
  Unix.file_descr list * Unix.file_descr list * Unix.file_descr list

(** "restart_on_EINTR f x" will do "f x", but if it throws EINTR then it will retry.
See https://ocaml.github.io/ocamlunix/ocamlunix.html#sec88 for explanation. *)
val restart_on_EINTR : ('a -> 'b) -> 'a -> 'b

(** Like Unix.write, but ignores EINTR. *)
val write_non_intr : Unix.file_descr -> bytes -> int -> int -> unit

(** Reads specified number of bytes from the file descriptor through repeated calls to Unix.read.
Ignores EINTR. If ever a call to Unix.read returns 0 bytes (e.g. end of file),
then this function will return None. *)
val read_non_intr : Unix.file_descr -> int -> bytes option

(** Flow uses lwt, which installs a sigchld handler. So the old pattern of
fork & waitpid will hit an EINTR when the forked process dies and the parent
gets a sigchld signal.

Note: this is only a problem if you're not using the WNOHANG flag, since
EINTR isn't thrown for WNOHANG *)
val waitpid_non_intr : Unix.wait_flag list -> int -> int * Unix.process_status

val check_dmesg_for_oom : int -> string -> bool

type rusage = {
  ru_maxrss: int;
  ru_ixrss: int;
  ru_idrss: int;
  ru_isrss: int;
  ru_minflt: int;
  ru_majflt: int;
  ru_nswap: int;
  ru_inblock: int;
  ru_oublock: int;
  ru_msgsnd: int;
  ru_msgrcv: int;
  ru_nsignals: int;
  ru_nvcsw: int;
  ru_nivcsw: int;
}

external getrusage : unit -> rusage = "hh_getrusage"

external start_gc_profiling : unit -> unit = "hh_start_gc_profiling" [@@noalloc]

external get_gc_time : unit -> float * float = "hh_get_gc_time"

module For_test : sig
  val find_oom_in_dmesg_output : int -> string -> string list -> bool
end

(** [atomically_create_and_init_file file ~rd ~wr perm ~init] will
(1) create an anomymous FD using [rd], [wr] and [perm],
(2) call the [init] callback allowing you to write contents or lock that FD,
(3) attempt to atomically place that FD at the specified filename [file].
If this third step fails because a file already exists there, the function
will return None. Otherwise, it will return Some fd. *)
val atomically_create_and_init_file :
  string ->
  rd:bool ->
  wr:bool ->
  Unix.file_perm ->
  init:(Unix.file_descr -> unit) ->
  Unix.file_descr option

(** This will acquire a reader-lock on the file then read its content.
Locks in unix are advisory, so this only works if writing is done by
[protected_write_exn]. If the file doesn't exist, Unix.Unix_error(ENOENT). *)
val protected_read_exn : string -> string

(** [protected_write_exn file content] will acquire a writer-lock on the file
then write content. Locks in unix are advisory, so this only works if reading is
done by [protected_read_exn]. Empty content isn't supported and will fail. *)
val protected_write_exn : string -> string -> unit

(** This is a primitive API for atomic locks. It is robust against EINTR. *)
val with_lock : Unix.file_descr -> Unix.lock_command -> f:(unit -> 'a) -> 'a

(** As it says, redirects stdout and stderr to this file, which it will
open in "w" mode. If redirection fails then stdout+stderr are left unchanged.*)
val redirect_stdout_and_stderr_to_file : string -> unit
