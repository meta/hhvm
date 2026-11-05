(*
 * Copyright (c) 2015, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude

(*****************************************************************************)
(* Module building workers.
 * A worker is a subprocess executing an arbitrary function.
 * You should first create a fixed amount of workers and then use those
 * because the amount of workers is limited and to make the load-balancing
 * of tasks better (cf multi_worker.ml).
 *)
(*****************************************************************************)

type process_id = int

type worker_id = int

type worker_failure =
  (* Worker foce quit by Out Of Memory. *)
  | Worker_oomed
  | Worker_quit of Unix.process_status

exception Worker_failed of (process_id * worker_failure)

(* Raise this exception when sending work to a worker that is already busy.
 * We should never be doing that, and this is an assertion error. *)
exception Worker_busy

val failure_to_string : worker_failure -> string

type send_job_failure =
  | Worker_already_exited of Unix.process_status
  | Other_send_job_failure of exn

exception Worker_failed_to_send_job of send_job_failure

(* The type of a worker visible to the outside world *)
type worker

(*****************************************************************************)
(* The handle is what we get back when we start a job. It's a "future"
 * (sometimes called a "promise"). The scheduler uses the handle to retrieve
 * the result of the job when the task is done (cf multi_worker.ml).
 *)
(*****************************************************************************)
type ('job, 'result) handle

(* An empty type *)
type void

(* Get the worker's id *)
val worker_id : worker -> worker_id

(* Has the worker been force quit *)
val is_force_quit : worker -> bool

(* Mark the worker as busy. Throw if it is already busy *)
val mark_busy : worker -> unit

(* If the worker is busy, what is it doing. Note that calling this is not
 * type safe: 'a and 'b are free type variables, and they depend on what is the
 * job being executed by worker. *)
val get_handle_UNSAFE : worker -> ('a, 'b) handle option

(* Mark the worker as free *)
val mark_free : worker -> unit

(** Get the worker process handle. *)
val spawn : worker -> (void, Worker.request) Daemon.handle

type call_wrapper = { wrap: 'x 'b. ('x -> 'b) -> 'x -> 'b }

type 'a entry_state = 'a * Gc.control * Shared_mem.handle * int

(* The first bool parameter specifies whether to use worker clones
 * or not: for non-longlived-workers, we must clone. *)
type 'a worker_params = {
  longlived_workers: bool;
  entry_state: 'a entry_state;
  controller_fd: Unix.file_descr option;
}

type 'a entry = ('a worker_params, Worker.request, void) Daemon.entry

(* Creates a pool of workers. *)
val make :
  ?call_wrapper:
    (* See docs in Worker_controller.worker for call_wrapper. *)
    call_wrapper ->
  longlived_workers:bool ->
  saved_state:'a ->
  entry:'a entry ->
  int ->
  gc_control:Gc.control ->
  heap_handle:Shared_mem.handle ->
  worker list

(** Call in a sub-process (CAREFUL, GLOBALS ARE COPIED) *)
val call : ?call_id:int -> worker -> ('a -> 'b) -> 'a -> ('a, 'b) handle

(* See Multi_threaded_call.call_id *)
val get_call_id : ('a, 'b) handle -> int

(* Retrieves the job that the worker is currently processing *)
val get_job : ('a, 'b) handle -> 'a

(* Retrieves the result (once the worker is done) hangs otherwise *)
val get_result : ('a, 'b) handle -> 'b

(* Selects among multiple handles those which are ready. *)
type ('a, 'b) selected = {
  readys: ('a, 'b) handle list;
  waiters: ('a, 'b) handle list;
  (* Additional (non worker) ready fds that we selected on. *)
  ready_fds: Unix.file_descr list;
}

val select : ('a, 'b) handle list -> Unix.file_descr list -> ('a, 'b) selected

(* Returns the worker which produces this handle *)
val get_worker : ('a, 'b) handle -> worker

(* Force quit the workers *)
val force_quit_all : unit -> unit

val cancel : ('a, 'b) handle list -> unit
