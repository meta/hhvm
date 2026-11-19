(*
 * Copyright (c) 2018, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE fn in the "hack" directory of this source tree.
 *
 *)

(** Sets the root path of the repository.
All functions in this file will throw unless you've already called set_root
at the start of your process. *)
val set_root : Path.t -> unit

(** use this in tests, instead of set_root,
to disable progress-logging and error-streaming. *)
val disable : unit -> unit

(** Disposition of hh_server *)
type disposition =
  | DStopped
      (** Hh_server has failed in some way, so will be unable to handle future work until it's been fixed. *)
  | DWorking
      (** Hh_server is working on something, e.g. doing a typecheck or handling a request in ServerRpc *)
  | DReady
      (** Hh_server is ready to handle requests, i.e. not doing any work. *)
[@@deriving show]

(** Progress is a file in /tmp/hh_server/<repo>.progress.json which is written
by monitor+server. It lives from the moment the monitor starts up until the
moment it finally dies or is killed. You should only read it by the [read]
call, since that protects against races. It also protects against server death,
because if you try to [read] a progress.json that had been created by a now-dead
server PID then it detects that fact and returns an "unknown" status.

The state fields [disposition] and [message] are intended to be shown to the
user, not acted upon in code -- they're slightly handwavey in places (e.g. there's
an interval from when a server dies until the monitor realizes that fact when
[read] will return "unknown"). *)
type t = {
  pid: int;  (** pid of the process that wrote this status *)
  disposition: disposition;
  message: string;  (** e.g. "typechecking 5/15 files" *)
  timestamp: float;
}

(** Reads the current progress.json file. If there is none, or if there is one
but it came from a dead PID, or if it was corrupt, this function synthesizes
a [DStopped] response with a appropriate human-readable message that reflects
on the precise reason, but simply says "stopped" in the typical case of absent
file or dead PID. *)
val read : unit -> t

(** [write ~include_in_logs ~disposition fmt_string] writes
[disposition] and the formatted string [fmt_string] to progress.json.
The signature [('a, unit, string, unit) format4 -> 'a]
is simply the signature of [Printf.printf].

Default disposition is [DWorking]. If you want to indicate that the server is stopped
or ready, you must provide a disposition explicitly. *)
val write :
  ?include_in_logs:bool ->
  ?disposition:disposition ->
  ('a, unit, string, unit) format4 ->
  'a

(** [with_message msg f] writes message [msg], calls [f], then writes
  back the message displayed before calling [with_message].
  [f] can call [write], [with_message] or [with_frame]: we'd still
  show the message displayed before this call to [with_message]
  when [f] finishes. *)
val with_message :
  ?include_in_logs:bool ->
  ?disposition:disposition ->
  string ->
  (unit -> 'a) ->
  'a

(** [with_frame f] calls [f] (which can display messages using
  [write], [with_message] or [with_frame]) then writes
  back the message displayed before calling [with_frame]. *)
val with_frame : ?disposition:disposition -> (unit -> 'a) -> 'a

(** Shorthand for [write ~include_in_logs:false ~disposition:DWorking "%s" message]
for the message "<operation> <done_count>/<total_count> <unit> <percent done> <extra>". *)
val write_percentage :
  operation:string ->
  done_count:int ->
  total_count:int ->
  unit:string ->
  extra:string option ->
  unit

(** The message [write_percentage] writes. Pure. With no denominator
    yet ([total_count <= 0]) the percentage is omitted rather than
    rendered as nan. *)
val make_percentage_progress_message :
  operation:string ->
  done_count:int ->
  total_count:int ->
  unit:string ->
  extra:string option ->
  string

(** Call this upon monitor shutdown to delete the progress file *)
val try_delete : unit -> unit

(**********************************************************************
  OVERVIEW OF STREAMING ERRORS FILE

  Errors is a file at /tmp/hh_server/<repo>/errors.bin which is written
  by server during a typecheck. The file is first created when the server
  starts its first typecheck, gets errors appended to it as the typecheck
  discovers them, is unlinked and then another one with the same name
  created each time the server starts another typecheck, and the file is finally
  unlinked upon server exit. A "typecheck" can be thought of as a typecheck of the
  entire program, and hence the errors file will contain every error reported in WWW.
  But in reality the server does tricks to typecheck a smaller number of files just
  so long as it still manages to report every error into the file.

  Therefore: a client can at any time open an file descriptor to this file,
  and `tail -f -n +1` (i.e. read from the start and then follow) to follow
  the typecheck that is currently underway or most recently completed, from its
  start to its end. It will still be able to read the file descriptor through
  to its end even if, in the meantime, a new typecheck has started and the old
  file has been unlinked. Note that a given file descriptor will only ever point
  to a file which monotonically grows.

  The server can have its typecheck interrupted. Some interruptions, like files changed on disk,
  will cause the current typecheck to be cancelled, then a new typecheck started.
  In this case the existing errors file will be unlinked, a new errors file created
  for the new typecheck, and a sentinel will be written into the old file descriptor
  to show that it ended before completing. Other interruptions, e.g. from
  `hh --type-at-pos`, will have no effect: the current typecheck file can continue
  being read just fine. If a client reads a file descriptor on its sentinel, that is
  a guarantee that a new errors file is already in place and can be opened (or,
  if no errors file exists, that can only be because the server has terminated).

  If the file does not exist, the client knows that either there is no server, or there
  is one but it has not yet started typechecking (it might still be loading saved state).
  In either case it is appropriate for the client to clientConnect {autostart_server=true}
  in the normal way, until it establishes a connection to the server, and then it can
  wait for the errors file.


  SEMANTICS OF ERRORS-FILE CONTENTS

  * An errors-file is tied to a particular Watchman or Edenfs_watcher clock value. It
    reflects all file-changes that happened prior to the clock. (As to whether it reflects
    any file-changes that happened after the clock, that is impossible to tell.)
  * As mentioned above, when the errors-file is complete, it contains the full
    set of errors that hh_server knows about for the project.
  * The error file is in a binary format consisting of a header followed by a series of
    Diagnostics.diagnostic list Relative_path.map along with timestamp,
    followed by an "end sentinel" if the typecheck has finished or been interrupted.
    Let's call each of these maps an error report. Each encompasses
    several files, and for each file it has a sorted list of errors. Currently we make
    one report for all "duplicate name" errors across the project if any, followed by
    one report per batch that had errors. This means that a file might be mentioned
    twice, once in a "duplicate name" report, once later. This will change in future
    so that each file is reported only once.
  * It currently does not write empty reports, though that might change in future
    (e.g. we might decide to write "no errors in file a.php" for a file which
    previously did have errors).
  * Within a single report, the error list has been sorted and de-duped.


  LIFECYCLE SEMANTICS OF PRODUCING AND CONSUMING THE ERRORS-FILE

  The code that produces errors-file lives in server_main.ml (which registers an on-exit
  hook to delete the file), server_type_check.ml (to manage the previous and new
  errors-file immediately before and after a typecheck is performed),
  and typing_check_service.ml (to make the actual error reports).
  1. When hh_server is launched, it either eventually exits or eventually writes
  an errors.bin file with some clock value that came at or after its launch.
  2. When files on disk are changed that pass FilesToIgnore.watch_spec
  and Find_utils.post_file_watcher_filter, then eventually either a new errors.bin will
  be written which reflects the clock after those files, or eventually it will terminate.

  These invariants imply how the client should connect:
  1. If there's no errors.bin, then doing an RPC connect to the server that succeeds
  means that it's fine to just wait around waiting for an errors.bin to succeed.
  (Except for one vulnerability: if the server handles the RPC but then crashes
  before starting its first check).
  2. If there is an errors.bin but files have changed on disk since error's watchclock,
  it's fine for the client to just wait until a new errors.bin gets created.
*)

(** Reason for marking the errors file as complete (using the "complete" sentinel) *)
type completion_reason =
  | Complete of Telemetry.t
      (** The typecheck has finished, i.e. server called [ErrorsWrite.complete]. *)
  | Restarted of {
      user_message: string;
      log_message: string;
    }
      (** The typecheck didn't complete; a new typecheck in a new errors-file has started.
        i.e. server called [ErrorsWrite.new_empty_file] before [ErrorsWrite.complete].
        [user_message] is a human-facing reason for why it was restarted, and [log_message]
        contains extra logging information. *)
  | Stopped
      (** Hh_server was stopped gracefully so we can't read errors. i.e. server called [ErrorsWrite.unlink]. *)
[@@deriving show]

val completion_reason_short_description : completion_reason -> string

(** Each item that a consumer reads from the errors-file is one of these. *)
type errors_file_item =
  | Errors of {
      errors: (Diagnostics.finalized_diagnostic * int) list Relative_path.Map.t;
      timestamp: float;
    }
  | Telemetry of Telemetry.t

type errors_file_read_error = Malformed [@@deriving show]

val enable_error_production : bool -> unit

module ErrorsWrite : sig
  (** To be called at start of typechecking.
  This creates a new errors file. If there had been a previous errors file, then the previous
  one gets unlinked; and if the previous error file was not yet complete then anyone
  reading from the previous errors file will now get a [Error (Restarted cancel_reason)]. *)
  val new_empty_file :
    clock:Server_notifier_types.clock option ->
    ignore_hh_version:bool ->
    cancel_reason:string * string ->
    unit

  (** To be called during typechecking.
  Anyone reading the current errors file will get this error report as [Ok errors].
  This call will failwith if called before [new_empty_file], or after [complete]/[unlink_at_server_stop]. *)
  val report : Diagnostics.t -> unit

  (** To be called during typechecking.
  Anyone reading the current errors file will get this as [Ok Telemetry].
  This call will failwith if called before [new_empty_file], or after [complete]/[unlink_at_server_stop]. *)
  val telemetry : Telemetry.t -> unit

  (** To be called at the end of typechecking.
  After this, anyone reading the errors file will get [Error Complete].
  This call will failwith if called before [new_empty_file] or after [complete]/[unlink_at_server_stop]. *)
  val complete : Telemetry.t -> unit

  (** To be called upon server shutdown, e.g. after "hh stop" or .hhconfig change.
  After this, anyone reading the errors file will get [Error Stopped]. *)
  val unlink_at_server_stop : unit -> unit

  (** Internal, for testing only. *)
  val get_state_FOR_TEST : unit -> string

  (** Internal, for testing only *)
  val create_file_FOR_TEST : pid:int -> cmdline:string -> unit
end

module ErrorsRead : sig
  (** A [log_message] is a string which should go to logs, but not be surfaced to users. *)
  type log_message = string

  (** A successful call to [openfile] returns this. *)
  type open_success = {
    pid: int;
        (** the PID of the server process that produced this errors file *)
    timestamp: float;  (** The time at which the typecheck started. *)
    clock: Server_notifier_types.clock option;
        (** The file watcher clock at which this typecheck started. *)
  }

  type open_error =
    | ORead_error of errors_file_read_error
    | OBuild_id_mismatch
        (** The hh_server that produced these errors is incompatible with the current binary. *)
    | OKilled of Exit_status.finale_data option
  [@@deriving show]

  val open_error_short_description : open_error -> string

  (** [openfile fd] opens an error-file for reading, one that has been created
  through [ErrorsWrite.new_empty_file]. *)
  val openfile :
    Unix.file_descr -> (open_success, open_error * log_message) result

  type read_result =
    | RItem of errors_file_item
    | RCompleted of completion_reason * string

  (** Attempt to get the next batch of errors. It returns based on a queue
  of what the server was written to the errors file...
  * For each time the server did [ErrorsWrite.report errors], this function will return [Ok (Some (RItem (Errors { errors; timestamp })))].
  * If the server hasn't yet done further [ErrorsWrite.report], this will return [Ok None].
  * If the server did [ErrorsWrite.complete] then this will return [Ok (Some (RCompleted (Complete _, _)))].
  * If the server did [ErrorsWrite.new_empty_file] then this will return [Ok (Some (RCompleted (Restarted _, _)))].
  * If the server did [ErrorsWrite.unlink_at_server_stop] then this will return [Ok (Some (RCompleted (Stopped, _)))]. *)
  val read_next_errors :
    Unix.file_descr -> (read_result option, errors_file_read_error) result
end
