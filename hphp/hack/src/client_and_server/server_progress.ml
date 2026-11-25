(*
 * Copyright (c) 2018, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE fn in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude
open Ppx_yojson_conv_lib.Yojson_conv.Primitives

type disposition =
  | DStopped [@value 1]
  | DWorking [@value 2]
  | DReady [@value 3]
[@@deriving show { with_path = false }, yojson]

type t = {
  pid: int;
  disposition: disposition;
  message: string;
  timestamp: float;
}
[@@deriving yojson]

let stack : string ref list ref = ref [ref ""]

let write_stack message = List.hd_exn !stack := message

let push_stack () = stack := ref "" :: !stack

let pop_stack_and_peek () =
  stack := List.tl_exn !stack;
  !(List.hd_exn !stack)

(** The caller must set this before attempting to send progress, otherwise exception *)
let root : Path.t option ref = ref None

let set_root (r : Path.t) : unit = root := Some r

let disable () : unit = root := Some Path.dummy_path

let server_progress_file () =
  match !root with
  | None -> failwith "Server_progress.set_root must be called first"
  | Some root when Path.equal root Path.dummy_path -> None
  | Some root -> Some (Server_files.server_progress_file root)

let try_delete () : unit =
  match server_progress_file () with
  | None -> ()
  | Some server_progress_file ->
    (try Unix.unlink server_progress_file with
    | _ -> ())

(** This writes to the specified progress file. It first acquires
an exclusive (writer) lock. (Locks on unix are advisory; we trust
[read] below to also acquire a lock). It overwrites
whatever was there before. In case of failure, it logs but is
silent. That's on the principle that defects in
progress-reporting should never break hh_server. *)
let write_file (t : t) : unit =
  match server_progress_file () with
  | None -> ()
  | Some server_progress_file ->
    let content = yojson_of_t t |> Yojson.Safe.pretty_to_string in
    (try Sys_utils.protected_write_exn server_progress_file content with
    | exn ->
      let e = Exception.wrap exn in
      Hh_logger.log
        "SERVER_PROGRESS_EXCEPTION(write) %s\n%s"
        (Exception.get_ctor_string e)
        (Exception.get_backtrace_string e |> Exception.clean_stack);
      Hack_event_logger.server_progress_write_exn ~server_progress_file e;
      ())

(** This reads the specified progress file, which is assumed to exist.
It first acquires a non-exclusive (reader) lock. (Locks on unix are
advisory; we trust [write_file] above to also acquire a writer
lock).  If there are failures, we log, and return a human-readable
string that indicates why. *)
let read () : t =
  let synthesize_stopped message =
    {
      pid = 0;
      disposition = DStopped;
      message;
      timestamp = Unix.gettimeofday ();
    }
  in
  match server_progress_file () with
  | None -> failwith "Server_progress.disable: can't read it"
  | Some server_progress_file ->
    let content = ref "[not yet read content]" in
    (try
       content := Sys_utils.protected_read_exn server_progress_file;
       let t = t_of_yojson @@ Yojson.Safe.from_string !content in
       (* If the status had been left behind on disk by a process that terminated without deleting it,
          well, we'll return the same 'unknown' as if the file didn't exist. *)
       if Proc.is_alive ~pid:t.pid ~expected:"" then
         t
       else
         synthesize_stopped "stopped"
     with
    | Unix.Unix_error (Unix.ENOENT, _, _) -> synthesize_stopped "stopped"
    | exn ->
      let e = Exception.wrap exn in
      Hh_logger.log
        "SERVER_PROGRESS_EXCEPTION(read) %s\n%s\n%s"
        (Exception.get_ctor_string e)
        (Exception.get_backtrace_string e |> Exception.clean_stack)
        !content;
      Hack_event_logger.server_progress_read_exn ~server_progress_file e;
      synthesize_stopped "unknown state")

let write_message ?(include_in_logs = true) ?(disposition = DWorking) message =
  if include_in_logs then Hh_logger.log "[progress] %s" message;
  let timestamp = Unix.gettimeofday () in
  write_file { pid = Unix.getpid (); disposition; message; timestamp };
  ()

let write ?(include_in_logs = true) ?(disposition = DWorking) fmt =
  let f message =
    begin
      write_stack message;
      write_message ~include_in_logs ~disposition message
    end
  in
  Printf.ksprintf f fmt

let with_message ?(include_in_logs = true) ?(disposition = DWorking) message f =
  push_stack ();
  write_stack message;
  write_message ~include_in_logs ~disposition message;
  let res = f () in
  let previous_message = pop_stack_and_peek () in
  write_message ~include_in_logs:false ~disposition previous_message;
  res

let with_frame ?(disposition = DWorking) f =
  push_stack ();
  let res = f () in
  let previous_message = pop_stack_and_peek () in
  write_message ~include_in_logs:false ~disposition previous_message;
  res

(* The message will look roughly like this:
   <operation> <done_count>/<total_count> <unit> <percent done> <extra>*)
let make_percentage_progress_message
    ~(operation : string)
    ~(done_count : int)
    ~(total_count : int)
    ~(unit : string)
    ~(extra : string option) : string =
  let unit =
    if String.equal unit "" then
      unit
    else
      unit ^ " "
  in
  let main_message =
    if total_count <= 0 then
      (* No denominator yet (e.g. a distc check before its typing
         total arrives): a percentage would be nan. *)
      Printf.sprintf "%s %d/%d %s" operation done_count total_count unit
    else begin
      let percent =
        Float.round_down
          (1000.0 *. float_of_int done_count /. float_of_int total_count)
        /. 10.0 (* so that 999999/1000000 will show as 99.9%, not 100.0% *)
      in
      Printf.sprintf
        "%s %d/%d %s(%.1f%%)"
        operation
        done_count
        total_count
        unit
        percent
    end
  in
  match extra with
  | Some extra -> main_message ^ " " ^ extra
  | None -> main_message

let write_percentage
    ~(operation : string)
    ~(done_count : int)
    ~(total_count : int)
    ~(unit : string)
    ~(extra : string option) : unit =
  write
    ~include_in_logs:false
    "%s"
    (make_percentage_progress_message
       ~operation
       ~done_count
       ~total_count
       ~unit
       ~extra)

type completion_reason =
  | Complete of Telemetry.t [@printer (fun fmt _t -> fprintf fmt "Complete")]
  | Restarted of {
      user_message: string;
      log_message: string;
    } [@printer (fun fmt _ _ -> fprintf fmt "Restarted")]
  | Stopped
[@@deriving show { with_path = false }]

let completion_reason_short_description = function
  | Complete _ -> "typecheck completed"
  | Restarted _ -> "typecheck restarted due to file changes"
  | Stopped -> "hh_server gracefully stopped"

type errors_file_item =
  | Errors of {
      errors: (Diagnostics.finalized_diagnostic * int) list Relative_path.Map.t;
          (** we convert to finalized_error (i.e. turn paths absolute) before writing in the file,
            because consumers don't know hhi paths. As for the [Relative_path.Map.t], it
            is guaranteed to only have root-relative paths. (we don't have a type to indicate
            "I am the suffix of a root-relative path" so this is the best we can do.) *)
      timestamp: float;
          (** the errors were detected by reading the files not later than this time. *)
    }
  | Telemetry of Telemetry.t

type errors_file_read_error = Malformed [@@deriving show]

let is_production_enabled = ref true

let enable_error_production (b : bool) : unit = is_production_enabled := b

let errors_file_path () =
  match !root with
  | None -> failwith "Server_progress.set_root must be called first"
  | Some _ when not !is_production_enabled -> None
  | Some root when Path.equal root Path.dummy_path -> None
  | Some root -> Some (Server_files.errors_file_path root)

(** This is an internal module concerned with the binary format of the errors-file. *)
module ErrorsFile = struct
  (** The errors-file is a binary format consisting of a sequence of messages
  written by the hh_server process.

  The first two messages are [VersionHeader] followed by [Header], and these
  always exist (they are placed atomically at error-file creation by
  [ErrorsWrite.new_empty_file]). Then there are zero or more [Item], each
  one appended by a call to [ErrorsWrite.report]. Finally, there may be an [End],
  appended by a call to [ErrorsWrite.complete] or [unlink_after_stopped] or [new_empty_file].

  Each message consists of a 5-byte preamble (one sync byte, 4 size bites)
  followed by a marshalled [message] structure. *)
  type message =
    | VersionHeader of {
        version: string;
            (** from Build_id.build_revision, or empty if dev build or --ignore-hh-version *)
        extra: Yojson.Safe.t;
            (** CARE! The hh_client binary might read the [VersionHeader] message that was written by either
            an older or a newer version of the hh_server binary. Therefore, it is impossible to
            change the datatype! Any new fields will have to be added to 'extra' if they're needed
            cross-version, or more commonly just placed in [Header] since it is version-safe. *)
      }
    | Header of {
        pid: int;
            (** the pid of the hh_server that wrote this file; clients check "sigkill 0 <pid>" to see if it's still alive *)
        cmdline: string;
            (** the /proc/pid/cmdline of hh_server; clients check that the current /proc/pid/cmdline is the same (proving that the pid hasn't been recycled) *)
        timestamp: float;  (** the time at which the typecheck began *)
        clock: Server_notifier_types.clock option;
            (** the file watcher clock at which the typecheck began, i.e. reflecting all file-changes up to here *)
      }
    | Item of errors_file_item
    | End of {
        reason: completion_reason;
        timestamp: float;
        log_message: string;
      }

  let max_payload_size = 4_000_000_000 (* 4 GB *)

  let assert_payload_size_ok size direction (message : message option) =
    if size >= max_payload_size then (
      let msg =
        Printf.sprintf
          "%s error payload with size %d, which is too big.%s"
          (match direction with
          | `Read -> "Reading"
          | `Write -> "Writing")
          size
          (match message with
          | None -> ""
          | Some message ->
            Printf.sprintf
              " Message is %s."
              (match message with
              | VersionHeader _ -> "the version header"
              | Header _ -> "the header"
              | Item (Errors { errors; timestamp = _ }) ->
                let error_count =
                  Relative_path.Map.fold
                    errors
                    ~init:0
                    ~f:(fun _path errors cnt -> cnt + List.length errors)
                in
                Printf.sprintf "some %d errors" error_count
              | Item (Telemetry telemetry) ->
                let nchar = Telemetry.to_string telemetry |> String.length in
                Printf.sprintf "some telemetry of %d characters" nchar
              | End _ -> "the end sentinel"))
      in
      Hack_event_logger.invariant_violation_bug msg;
      failwith msg
    )

  (** This helper acquires an exclusive lock on the file, appends the message, then releases the lock.
     It does not do any state validation - that's left to its caller. *)
  let write_message (fd : Unix.file_descr) (message : message) : unit =
    let payload = Marshal.to_bytes message [] in
    let size = Bytes.length payload in
    (* Since we do this assertion when reading (to avoid creating buffers that are too big),
       we also do the same assertion when writing, just in case we ever write very big
       messages by mistake. *)
    assert_payload_size_ok size `Write (Some message);
    let preamble = Marshal_tools.make_preamble size in
    Sys_utils.with_lock fd Unix.F_LOCK ~f:(fun () ->
        Sys_utils.write_non_intr fd preamble 0 (Bytes.length preamble);
        Sys_utils.write_non_intr fd payload 0 size)

  let read_message (fd : Unix.file_descr) :
      (message option, errors_file_read_error) result =
    Sys_utils.with_lock fd Unix.F_RLOCK ~f:(fun () ->
        let preamble =
          Sys_utils.read_non_intr fd Marshal_tools.expected_preamble_size
        in
        match preamble with
        | None -> Ok None
        | Some preamble ->
          let size = Marshal_tools.parse_preamble preamble in
          (* This assert is in case the file is garbled, and we read a crazy-big size,
             to avoid allocating say a 10gb bytes array and having the machine get stuck. *)
          assert_payload_size_ok size `Read None;
          (match Sys_utils.read_non_intr fd size with
          | None -> Error Malformed
          | Some payload ->
            let message : message = Marshal.from_bytes payload 0 in
            Ok (Some message)))
end

module ErrorsWrite = struct
  (** This structure represent's hh_server's current knowledge about the errors-file.
  Only hh_server should be manipulating the errors-file; hence, this knowledge is authoritative. *)
  type write_state =
    | Absent
        (** The errors-file either doesn't exist because this hh_server instance hasn't
        yet called [new_empty_file], or because it called [unlink_at_server_stop]. *)
    | Reporting of Unix.file_descr * int
        [@printer (fun fmt (_fd, n) -> fprintf fmt "Reporting[%d]" n)]
        (** The errors-file does exist,
        due to a previous call to [new_empty_file], and it's fine to call [report] or [complete]. *)
    | Closed
        (** The errors-file has an end-marker due to a call to [complete]. *)
  [@@deriving show { with_path = false }]

  (** This mutable value tracks the current state of the errors-file belonging to this
  instance of hh_server.
  The errors-file is deemed absent when hh_server starts up. Even if there had been a leftover
  errors-file from a previous hh_server instance, any attempt to read it will fail because it
  necessarily must have come from a now-dead PID. *)
  let write_state : write_state ref = ref Absent

  (** This helper is called by [new_empty_file] and [unlink_at_server_stop]...
     1. It unlinks the current file, if any (in states [Reporting], [Closed])
     2. It calls the [after_unlink] callback
     3. It writes a End message if needed (in state [Reporting])
     4. It closes the file-descriptor for the current file (in state [Reporting]).

     For example: in the case of caller [new_empty_file], its [after_unlink] callback creates a new errors file.
     In this way, if a client with an existing file-descriptor should read a sentinel, then it knows
     for sure it can immediately close that file-descriptor and re-open the new errors file. *)
  let unlink_sentinel_close
      (reason : completion_reason)
      ~(log_message : string)
      ~(errors_file_path : string)
      ~(after_unlink : unit -> 'a) =
    begin
      try Unix.unlink errors_file_path with
      | _ -> ()
    end;
    let result = after_unlink () in
    begin
      match !write_state with
      | Reporting (fd, count) ->
        Hh_logger.log
          "Errors-file: ending old %s with its %d reports, with sentinel %s"
          (Sys_utils.show_inode fd)
          count
          (show_completion_reason reason);
        ErrorsFile.write_message
          fd
          (ErrorsFile.End
             { reason; timestamp = Unix.gettimeofday (); log_message });
        Unix.close fd
      | _ -> ()
    end;
    result

  let new_empty_file
      ~(clock : Server_notifier_types.clock option)
      ~(ignore_hh_version : bool)
      ~(cancel_reason : string * string) : unit =
    match errors_file_path () with
    | None -> ()
    | Some errors_file_path -> begin
      (* (1) unlink the old errors file, (2) atomically create a new errors-file with
         Version_header+Header messages in it, (3) write a End marker into the old errors file.

         **Atomicity** is so that a client can be assured that if they open an errors-file
         then it will necessarily have two errors in it.
         **Sentinel-after-new** is so that a client can be assured that if they encounter
         a sentinel then there's already a new errors-file ready to be read immediately
         or, if not, then the server must have died.

         Both mechanisms are there to make the client-side code easier to write! *)
      let pid = Unix.getpid () in
      let version =
        if ignore_hh_version then
          ""
        else
          Build_id.build_revision
      in
      let version_header =
        ErrorsFile.VersionHeader { version; extra = `Assoc [] }
      in
      let header =
        ErrorsFile.Header
          {
            pid;
            cmdline = Proc.get_cmdline pid |> Result.ok_or_failwith;
            timestamp = Unix.gettimeofday ();
            clock;
          }
      in
      let (user_message, log_message) = cancel_reason in
      let fd =
        unlink_sentinel_close
          (Restarted { user_message; log_message })
          ~log_message:"new_empty_file"
          ~errors_file_path
          ~after_unlink:(fun () ->
            let fd =
              Sys_utils.atomically_create_and_init_file
                errors_file_path
                ~rd:false
                ~wr:true
                0o666
                ~init:(fun fd ->
                  ErrorsFile.write_message fd version_header;
                  ErrorsFile.write_message fd header)
            in
            match fd with
            | None ->
              failwith "Errors-file was created by someone else under our feet"
            | Some fd ->
              Hh_logger.log
                "Errors-file: starting new %s at clock %s"
                (Sys_utils.show_inode fd)
                (Option.value_map
                   clock
                   ~f:Server_notifier_types.show_clock
                   ~default:"[none]");
              fd)
      in
      write_state := Reporting (fd, 0)
    end

  let report (errors : Diagnostics.t) : unit =
    match errors_file_path () with
    | None -> ()
    | Some _ -> begin
      match !write_state with
      | Absent
      | Closed ->
        failwith ("Cannot report in state " ^ show_write_state !write_state)
      | Reporting _
        when Diagnostics.has_no_errors_or_warnings ~drop_fixmed:true errors ->
        ()
      | Reporting (fd, n) ->
        let n = n + 1 in
        (if n <= 5 then
          let (error_count, warning_count) =
            errors
            |> Diagnostics.get_diagnostic_list ~drop_fixmed:true
            |> Diagnostics.count_errors_and_warnings
          in
          Hh_logger.log
            "Errors-file: report#%d on %s: %d new errors, %d new warnings%s"
            n
            (Sys_utils.show_inode fd)
            error_count
            warning_count
            (if n = 5 then
              " [won't report any more this typecheck...]"
            else
              ""));
        (* sort and dedupe *)
        let errors =
          errors
          |> Diagnostics.drop_fixmed_errors_in_files
          |> Diagnostics.as_map
          |> Relative_path.Map.filter ~f:(fun path _errors ->
                 let is_root =
                   Relative_path.is_root (Relative_path.prefix path)
                 in
                 if not is_root then
                   Hack_event_logger.invariant_violation_bug
                     "error in file outside root"
                     ~path;
                 is_root)
          |> Relative_path.Map.map ~f:(fun errors ->
                 errors
                 |> Diagnostics.sort
                 |> List.map ~f:(fun err ->
                        ( User_diagnostic.to_absolute err,
                          User_diagnostic.hash_diagnostic_for_saved_state err )))
        in
        ErrorsFile.write_message
          fd
          (ErrorsFile.Item (Errors { timestamp = Unix.gettimeofday (); errors }));
        write_state := Reporting (fd, n)
    end

  let telemetry (telemetry : Telemetry.t) : unit =
    match errors_file_path () with
    | None -> ()
    | Some _ -> begin
      match !write_state with
      | Absent
      | Closed ->
        failwith ("Cannot report in state " ^ show_write_state !write_state)
      | Reporting (fd, n) ->
        Hh_logger.log
          "Errors-file: report#%d on %s: telemetry [%s]"
          n
          (Sys_utils.show_inode fd)
          (telemetry |> Telemetry.to_string);
        ErrorsFile.write_message fd (ErrorsFile.Item (Telemetry telemetry))
    end

  let complete (telemetry : Telemetry.t) : unit =
    match errors_file_path () with
    | None -> ()
    | Some _ -> begin
      match !write_state with
      | Absent
      | Closed ->
        failwith ("Cannot complete in state " ^ show_write_state !write_state)
      | Reporting (fd, n) ->
        Hh_logger.log
          "Errors-file: completing %s after %d reports"
          (Sys_utils.show_inode fd)
          n;
        ErrorsFile.write_message
          fd
          (ErrorsFile.End
             {
               reason = Complete telemetry;
               timestamp = Unix.gettimeofday ();
               log_message = "complete";
             });
        write_state := Closed
    end

  let unlink_at_server_stop () : unit =
    match errors_file_path () with
    | None -> ()
    | Some errors_file_path ->
      unlink_sentinel_close
        Stopped
        ~log_message:"unlink"
        ~errors_file_path
        ~after_unlink:(fun () -> ());
      write_state := Absent

  let get_state_FOR_TEST () : string = show_write_state !write_state

  let create_file_FOR_TEST ~(pid : int) ~(cmdline : string) : unit =
    let fd =
      Unix.openfile
        (Option.value_exn (errors_file_path ()))
        [Unix.O_WRONLY; Unix.O_CREAT; Unix.O_TRUNC]
        0o666
    in
    ErrorsFile.write_message
      fd
      (ErrorsFile.VersionHeader { version = ""; extra = `Assoc [] });
    ErrorsFile.write_message
      fd
      (ErrorsFile.Header { pid; cmdline; timestamp = 0.0; clock = None });
    Unix.close fd
end

module ErrorsRead = struct
  type log_message = string

  type open_success = {
    pid: int;
    timestamp: float;
    clock: Server_notifier_types.clock option;
  }

  type open_error =
    | ORead_error of errors_file_read_error
    | OBuild_id_mismatch
    | OKilled of Exit_status.finale_data option
  [@@deriving show]

  let open_error_short_description = function
    | ORead_error Malformed -> "malformed header"
    | OBuild_id_mismatch -> "client server version mismatch"
    | OKilled _ -> "hh_server died"

  let openfile (fd : Unix.file_descr) :
      (open_success, open_error * log_message) result =
    let open Result.Monad_infix in
    let read_message fd =
      ErrorsFile.read_message fd
      |> Result.map_error ~f:(fun e -> (ORead_error e, ""))
    in
    read_message fd >>= fun message1 ->
    read_message fd >>= fun message2 ->
    match (message1, message2) with
    | ( Some (ErrorsFile.VersionHeader { version; _ }),
        Some (ErrorsFile.Header { pid; cmdline; clock; timestamp }) ) ->
      if
        String.length version > 16
        && String.length Build_id.build_revision > 16
        && not (String.equal version Build_id.build_revision)
      then
        (* This is a version mismatch which we can't ignore. (We always ignore mismatch from dev-builds...
           version="" means a buck dev build, and version.length<=16 means a dune dev build.) *)
        let msg =
          Printf.sprintf
            "errors-file is version %s, but we are %s"
            version
            Build_id.build_revision
        in
        Error (OBuild_id_mismatch, msg)
      else if not (Proc.is_alive ~pid ~expected:cmdline) then
        let server_finale_file = Server_files.server_finale_file pid in
        let finale_data = Exit_status.get_finale_data server_finale_file in
        Error (OKilled finale_data, "Errors-file is from defunct PID")
      else
        Ok { pid; clock; timestamp }
    | _ ->
      Error
        ( ORead_error Malformed,
          "Errors-file does not start with VersionHeader and Header" )

  type read_result =
    | RItem of errors_file_item
    | RCompleted of completion_reason * string

  let read_next_errors (fd : Unix.file_descr) : (read_result option, _) result =
    let open Result.Monad_infix in
    ErrorsFile.read_message fd
    >>| Option.map ~f:(function
            | ErrorsFile.VersionHeader _
            | ErrorsFile.Header _ ->
              failwith
                "do Server_progress.ErrorsRead.openfile before read_next_error or Server_progress_lwt.watch_errors_file"
            | ErrorsFile.Item item -> RItem item
            | ErrorsFile.End { reason; log_message; _ } ->
              RCompleted (reason, log_message))
end
