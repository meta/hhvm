(*
 * Copyright (c) 2015, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude
open Worker

(*****************************************************************************
 * Module building workers
 *
 * A worker is a subprocess executing an arbitrary function
 *
 * You should first create a fixed amount of workers and then use those
 * because the amount of workers is limited and to make the load-balancing
 * of tasks better (cf multi_worker.ml)
 *
 * On Unix, we spawn workers when initializing Hack. Then, each
 * worker forks a clone process for each incoming request.
 * The forked clone will exit after processing a single request.
 *
 * On Windows, we do not pre-spawn when initializing Hack, we just
 * allocate all the required information into a record. Then, we
 * spawn a worker process for each incoming request.
 * It will also exit after one request.
 *
 * A worker never handles more than one request at a time.
 *
 *****************************************************************************)

type process_id = int

type worker_id = int

type worker_failure =
  (* Worker force quit by Out Of Memory. *)
  | Worker_oomed
  | Worker_quit of Unix.process_status

exception Worker_failed of (process_id * worker_failure)

exception Worker_busy

type send_job_failure =
  | Worker_already_exited of Unix.process_status
  | Other_send_job_failure of exn

exception Worker_failed_to_send_job of send_job_failure

let status_string = function
  | Unix.WEXITED i -> Printf.sprintf "WEXITED %d" i
  | Unix.WSIGNALED i -> Printf.sprintf "WSIGNALED %d" i
  | Unix.WSTOPPED i -> Printf.sprintf "WSTOPPED %d" i

let failure_to_string f =
  match f with
  | Worker_oomed -> "Worker_oomed"
  | Worker_quit s -> Printf.sprintf "(Worker_quit %s)" (status_string s)

let () =
  Stdlib.Printexc.register_printer @@ function
  | Worker_failed_to_send_job (Other_send_job_failure exn) ->
    Some (Printf.sprintf "Other_send_job_failure: %s" (Exn.to_string exn))
  | Worker_failed_to_send_job (Worker_already_exited status) ->
    Some (Printf.sprintf "Worker_already_exited: %s" (status_string status))
  | Worker_failed (id, failure) ->
    Some
      (Printf.sprintf
         "Worker_failed (process_id = %d): %s"
         id
         (failure_to_string failure))
  | _ -> None

(* The maximum amount of workers *)
let max_workers = 1000

type void (* an empty type *)

type call_wrapper = { wrap: 'x 'b. ('x -> 'b) -> 'x -> 'b }

(*****************************************************************************
 * Everything we need to know about a worker.
 *
 *****************************************************************************)

type worker = {
  (* Simple id for the worker. This is not the worker pid: on Windows, we spawn
   * a new worker for each job.
   *
   * This is also an offset into the shared heap segment, used to access
   * worker-local data. As such, the numbering is important. The IDs must be
   * dense and start at 1. (0 is the controller process offset.) *)
  id: int;
  (* The call wrapper will wrap any workload sent to the worker (via "call"
   * below) before invoking the workload.
   *
   * That is, when calling the worker with workload `f x`, it will be wrapped
   * as `wrap (f x)`.
   *
   * This allows universal handling of workload at the time we create the actual
   * workers. For example, this can be useful to handle exceptions uniformly
   * across workers regardless what workload is called on them. *)
  call_wrapper: call_wrapper option;
  (* On Unix, the main worker process sends status messages over this fd to this
   * controller. On Windows, it doesn't send anything, so don't try to read from
   * it (it should be set to None). *)
  controller_fd: Unix.file_descr option;
  (* Sanity check: is the worker still available ? *)
  mutable force_quit: bool;
  (* Sanity check: is the worker currently busy ? *)
  mutable busy: bool;
  (* If the worker is currently busy, handle of the job it's execuing *)
  mutable handle: 'a 'b. ('a, 'b) handle option;
  (* On Unix, a reference to the 'prespawned' worker. *)
  prespawned: (void, request) Daemon.handle;
}

(*****************************************************************************
 * The handle is what we get back when we start a job. It's a "future"
 * (sometimes called a "promise"). The scheduler uses the handle to retrieve
 * the result of the job when the task is done (cf multi_worker.ml).
 *
 *****************************************************************************)
and ('a, 'b) handle = ('a, 'b) delayed ref

(* Integer represents job the handle belongs to.
 * See Multi_threaded_call.call_id. *)
and ('a, 'b) delayed = ('a * int) * 'b worker_handle

and 'b worker_handle =
  | Processing of 'b job
  | Cached of 'b * worker
  | Canceled
  | Failed of Exception.t

(* The controller's job has a worker. The worker is a single process on Windows.
 * On Unix, the worker consists of a main and a clone worker processes. *)
and 'a job = {
  (* The associated worker *)
  worker: worker;
  (* The file descriptor we might pass to select in order to
     wait for the worker to finish its job. *)
  infd: Unix.file_descr;
  (* A blocking function that returns the job result. *)
  result: unit -> 'a;
  (* A blocking function that waits for job cancellation (see Worker.cancel)
   * to finish *)
  wait_for_cancel: unit -> unit;
}

let worker_id w = w.id

(* Has the worker been force quit *)
let is_force_quit w = w.force_quit

(* Mark the worker as busy. Throw if it is already busy *)
let mark_busy w =
  if w.busy then raise Worker_busy;
  w.busy <- true

let get_handle_UNSAFE w = w.handle

(* Mark the worker as free *)
let mark_free w =
  w.busy <- false;
  w.handle <- None

let spawn w = w.prespawned

(* If there is a call_wrapper, apply it and create the Request *)
let wrap_request w f x metadata_in =
  match w.call_wrapper with
  | Some { wrap } -> Request ((fun { send } -> send (wrap f x)), metadata_in)
  | None -> Request ((fun { send } -> send (f x)), metadata_in)

type 'a entry_state = 'a * Gc.control * Shared_mem.handle * int

(* The first bool parameter specifies whether to use worker clones
 * or not: for non-longlived-workers, we must clone. *)
type 'a worker_params = {
  longlived_workers: bool;
  entry_state: 'a entry_state;
  controller_fd: Unix.file_descr option;
}

type 'a entry = ('a worker_params, request, void) Daemon.entry

(**************************************************************************
 * Creates a pool of workers.
 *
 **************************************************************************)

let workers = ref []

(* Build one worker. *)
let make_one ?call_wrapper controller_fd spawn id =
  if id >= max_workers then failwith "Too many workers";

  let prespawned = spawn () in
  let worker =
    {
      call_wrapper;
      controller_fd;
      id;
      busy = false;
      handle = None;
      force_quit = false;
      prespawned;
    }
  in
  workers := worker :: !workers;
  worker

(* Make a few workers. When workload is given to a worker (via "call" below),
 * the workload is wrapped in the call_wrapper. *)
let make
    ?call_wrapper
    ~longlived_workers
    ~saved_state
    ~entry
    nbr_procs
    ~gc_control
    ~heap_handle : worker list =
  let setup_controller_fd () =
    let (parent_fd, child_fd) = Unix.pipe () in
    (* parent_fd is only used in this process. Don't leak it to children.
     * This will auto-close parent_fd in children created with Daemon.spawn
     * since Daemon.spawn uses exec. *)
    let () = Unix.set_close_on_exec parent_fd in
    (parent_fd, child_fd)
  in
  let spawn worker_id name child_fd () =
    Shared_mem.clear_close_on_exec heap_handle;

    (* Daemon.spawn runs exec after forking. We explicitly *do* want to "leak"
     * child_fd to this one spawned process because it will be using that FD to
     * send messages back up to us. Close_on_exec is probably already false, but
     * we force it again to be false here just in case. *)
    Option.iter child_fd ~f:Unix.clear_close_on_exec;
    let state = (saved_state, gc_control, heap_handle, worker_id) in
    let handle =
      Daemon.spawn
        ~name
        (Daemon.null_fd (), Unix.stdout, Unix.stderr)
        entry
        { longlived_workers; entry_state = state; controller_fd = child_fd }
    in
    Shared_mem.set_close_on_exec heap_handle;

    (* This process no longer needs child_fd after its spawned the child.
     * Messages are read using controller_fd. *)
    Option.iter child_fd ~f:Unix.close;
    handle
  in
  let made_workers = ref [] in
  let pid = Unix.getpid () in
  for n = 1 to nbr_procs do
    let (controller_fd, child_fd) = setup_controller_fd () in
    let name = Printf.sprintf Worker_utils.worker_name_fmt n nbr_procs pid in
    made_workers :=
      make_one
        ?call_wrapper
        (Some controller_fd)
        (spawn n name (Some child_fd))
        n
      :: !made_workers
  done;
  !made_workers

(**************************************************************************
 * Send a job to a worker
 *
 **************************************************************************)

let call ?(call_id = 0) w (type a b) (f : a -> b) (x : a) : (a, b) handle =
  if is_force_quit w then
    Printf.ksprintf failwith "force quit worker (%d)" (worker_id w);
  mark_busy w;

  let { Daemon.pid = worker_pid; channels = (inc, outc) } = spawn w in
  let infd = Daemon.descr_of_in_channel inc in
  let outfd = Daemon.descr_of_out_channel outc in
  let worker_failed pid_stat controller_fd =
    (* If we have a controller fd, we read the clone exit status
     * over that channel instead of using the one of the worker
     * process. *)
    let pid_stat =
      match controller_fd with
      | None -> snd pid_stat
      | Some fd ->
        Timeout.with_timeout
          ~timeout:3
          ~on_timeout:(fun _ -> snd pid_stat)
          ~do_:(fun _ ->
            try
              let (Subprocess_terminated status) =
                Marshal_tools.from_fd_with_preamble fd
              in
              status
            with
            | End_of_file -> snd pid_stat)
    in
    match pid_stat with
    | Unix.WEXITED i when i = Exit_status.(exit_code Out_of_shared_memory) ->
      raise Shared_mem.Out_of_shared_memory
    | Unix.WEXITED i ->
      Printf.eprintf "Subprocess(%d): fail %d" worker_pid i;
      raise (Worker_failed (worker_pid, Worker_quit (Unix.WEXITED i)))
    | Unix.WSTOPPED i ->
      raise (Worker_failed (worker_pid, Worker_quit (Unix.WSTOPPED i)))
    | Unix.WSIGNALED i ->
      raise (Worker_failed (worker_pid, Worker_quit (Unix.WSIGNALED i)))
  in
  (* Checks if the worker has exited. *)
  let with_exit_status_check ?(block_on_waitpid = false) worker_pid f =
    let wait_flags =
      if block_on_waitpid then
        []
      else
        [Unix.WNOHANG]
    in
    let pid_stat = Unix.waitpid wait_flags worker_pid in
    match pid_stat with
    | (0, _) -> f ()
    | (_, Unix.WEXITED 0) ->
      (* This will never actually happen. A worker process only exits if this
       * controller process has exited. *)
      failwith "Worker process exited 0 unexpectedly"
    | _ -> worker_failed pid_stat w.controller_fd
  in
  (* Prepare to read the answer from the worker process. *)
  let get_result_with_status_check ?(block_on_waitpid = false) () : b =
    with_exit_status_check ~block_on_waitpid worker_pid (fun () ->
        let data : b = Marshal_tools.from_fd_with_preamble infd in
        let ({ stats; log_globals } : metadata_out) =
          Marshal_tools.from_fd_with_preamble infd
        in
        Measure.merge (Measure.deserialize stats);
        Hack_event_logger.deserialize_globals log_globals;
        data)
  in
  let result () : b =
    (*
     * We run the "with_exit_status_check" twice (first time non-blockingly).
     * This is because of a race condition.
     *
     * Immediately after the main worker process forks the clone process (see worker.ml),
     * it does a blocking, non-interruptible waitpid on the clone process. This means
     * that if the clone process fails, the main worker process will see the failure and
     * also fail accordingly, which we will catch in with "with_exit_status_check".
     * This is designed around an assumption that, if the clone fails,
     * the main worker process will also fail. Therefore, the Worker_controller here
     * will see the failure and not attempt to read the result with
     * "Marshal_tools.from_fd_with_preamble"
     *
     * However, there is a scenario in which the assumption above cannot hold when
     * the clone process fails
     * - the worker clone process is forked
     * - the Worker_controller checks the worker's main process's status
     * - the non-interruptible waitpid call hasn't started yet
     *
     * Under such circumstances, the Worker_controller could try to read the result
     * with Marshal_tools, get an End_of_file, and crash.
     *
     * To get around this, we give the main worker process time to "catch up" and reach
     * the non-interruptible waitpid that we expect it to be at. Eventually, it will also
     * fail accordingly, since its clone has failed.
     *)
    try get_result_with_status_check () with
    | End_of_file -> get_result_with_status_check ~block_on_waitpid:true ()
  in
  let wait_for_cancel () : unit =
    with_exit_status_check worker_pid (fun () ->
        (* Depending on whether we manage to force quit the worker before it starts writing
         * results back, this will return either actual results, or "anything"
         * (written by interrupt signal that exited). The types don't match, but we
         * ignore both of them anyway. *)
        let (_ : 'c) = Marshal_tools.from_fd_with_preamble infd in
        let (_ : 'c) = Marshal_tools.from_fd_with_preamble infd in
        ())
  in
  let job = { result; infd; worker = w; wait_for_cancel } in
  let metadata_in = { log_globals = Hack_event_logger.serialize_globals () } in
  let (request : Worker.request) = wrap_request w f x metadata_in in
  (* Send the job to the worker. *)
  let () =
    try
      Marshal_tools.to_fd_with_preamble ~flags:[Marshal.Closures] outfd request
      |> ignore
    with
    | e -> begin
      match Unix.waitpid [Unix.WNOHANG] worker_pid with
      | (0, _) -> raise (Worker_failed_to_send_job (Other_send_job_failure e))
      | (_, status) ->
        raise (Worker_failed_to_send_job (Worker_already_exited status))
    end
  in
  (* And returned the 'handle'. *)
  let handle : (a, b) handle = ref ((x, call_id), Processing job) in
  w.handle <- Some (Obj.magic handle);
  handle

(**************************************************************************
 * Read results from a handle.
 * This might block if the worker hasn't finished yet.
 *
 **************************************************************************)

let with_worker_exn (handle : ('a, 'b) handle) job f =
  try f () with
  | Worker_failed (pid, status) as exn ->
    let e = Exception.wrap exn in
    mark_free job.worker;
    handle := (fst !handle, Failed e);
    begin
      match status with
      | Worker_quit (Unix.WSIGNALED -7) ->
        raise (Worker_failed (pid, Worker_oomed))
      | _ -> Exception.reraise e
    end
  | exn ->
    let e = Exception.wrap exn in
    mark_free job.worker;
    handle := (fst !handle, Failed e);
    Exception.reraise e

let get_result d =
  match snd !d with
  | Cached (x, _) -> x
  | Failed e -> Exception.reraise e
  | Canceled -> raise End_of_file
  | Processing s ->
    with_worker_exn d s (fun () ->
        let res = s.result () in
        mark_free s.worker;
        d := (fst !d, Cached (res, s.worker));
        res)

(*****************************************************************************
 * Our polling primitive on workers
 * Given a list of handle, returns the ones that are ready.
 *
 *****************************************************************************)

type ('a, 'b) selected = {
  readys: ('a, 'b) handle list;
  waiters: ('a, 'b) handle list;
  ready_fds: Unix.file_descr list;
}

let get_processing ds =
  List.rev_filter_map ds ~f:(fun d ->
      match snd !d with
      | Processing p -> Some p
      | _ -> None)

let select ds additional_fds =
  let processing = get_processing ds in
  let fds = List.map ~f:(fun { infd; _ } -> infd) processing in
  let (ready_fds, _, _) =
    if List.is_empty fds || List.length processing <> List.length ds then
      ([], [], [])
    else
      Sys_utils.select_non_intr (fds @ additional_fds) [] [] (-1.)
  in
  let additional_ready_fds =
    List.filter ~f:(List.mem ~equal:Poly.( = ) ready_fds) additional_fds
  in
  List.fold_right
    ~f:(fun d acc ->
      match snd !d with
      | Cached _
      | Canceled
      | Failed _ ->
        { acc with readys = d :: acc.readys }
      | Processing s when List.mem ~equal:Poly.( = ) ready_fds s.infd ->
        { acc with readys = d :: acc.readys }
      | Processing _ -> { acc with waiters = d :: acc.waiters })
    ~init:{ readys = []; waiters = []; ready_fds = additional_ready_fds }
    ds

let get_worker h =
  match snd !h with
  | Processing { worker; _ } -> worker
  | Cached (_, worker) -> worker
  | Canceled
  | Failed _ ->
    invalid_arg "Worker.get_worker"

let get_job h = fst (fst !h)

let get_call_id h = snd (fst !h)

(**************************************************************************
 * Worker termination
 **************************************************************************)

let force_quit w =
  if not (is_force_quit w) then (
    w.force_quit <- true;
    Daemon.force_quit w.prespawned
  )

let force_quit_all () = List.iter ~f:force_quit !workers

let wait_for_cancel d =
  match snd !d with
  | Processing s ->
    with_worker_exn d s (fun () ->
        s.wait_for_cancel ();
        mark_free s.worker;
        d := (fst !d, Canceled))
  | _ -> ()

let cancel handles =
  Worker_cancel.stop_workers ();
  List.iter handles ~f:(fun x -> wait_for_cancel x);
  Worker_cancel.resume_workers ();
  ()
