(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *)

(** File paths are relative to the scenario's temporary repository. Writing a
    file replaces its contents, or creates it if it does not exist. *)
type files = (string * string) list

type action =
  | Write_file of string * string
      (** Replace or create a file. Writes do not queue parsing or invalidate
          provider caches. *)

type type_check_step =
  | Check of {
      file: string;
      after_check: action list;
          (** Actions performed after this file finishes checking, before the
              next file begins. Deferred attempts do not count as completion. *)
    }
      (** Expect this file at this position in the server's checking order.
          It must be selected by the server and occur at most once in the
          schedule. *)
  | Remaining
      (** Check all selected files not named by preceding [Check] entries,
          preserving their order. Must be the final step in the schedule. *)

type iteration = {
  before_type_check: action list;
      (** Actions performed before calling [Server_type_check.type_check]. *)
  disk_needs_parsing: string list;
      (** Paths added to [env.disk_needs_parsing] before the call. Any paths
          already pending in the previous iteration's environment are retained. *)
  after_indexing: action list;
      (** Actions performed after indexing returns, before naming begins. *)
  type_check: type_check_step list;
      (** The expected file checking order for this invocation. [Remaining] may
          occur only at the end. Without it, [Check] entries must cover every
          selected file. The expanded schedule must match the server's order. *)
}

(** No actions or additional parsing inputs, and [type_check = [Remaining]].
    The default iteration still runs one invocation. *)
val default_iteration : iteration

type scenario = {
  initial_files: files;
  iterations: iteration list;
      (** Each entry runs exactly one [Server_type_check.type_check] invocation,
          retaining the resulting environment and heaps for the next entry. *)
}

type initialization =
  | Full_check
      (** Establish the baseline by checking all initial files. This is the
          default and populates declaration caches before the first iteration. *)
  | Saved_state
      (** Create a saved state from the initial files with hh_distc, then load
          its naming table, dependency graph, and saved error/warning state into
          a fresh process. Declarations remain cold. Files with saved errors
          are queued for the first iteration; there is no baseline check.

          Requires HH_DISTC_PATH and WORKER_PATH, as supplied by the
          saved_state_tests Buck target. *)

exception Diagnostics_mismatch of string

exception Internal_typechecker_error of string

(** Accept a documented active bug only when [check] raises
    [Diagnostics_mismatch] or [Internal_typechecker_error] and [matches] accepts
    that exception. Match the specific diagnostic, file, and comparison side,
    or the specific internal error; unrelated exceptions always propagate.

    If [check] succeeds, fail with [bug] and instructions to remove the wrapper
    and keep the ordinary correctness check. [bug] should identify the task or
    bug family. The check usually calls [run]. *)
val expect_known_fanout_bug :
  bug:string -> matches:(exn -> bool) -> (unit -> unit) -> unit

(** Establishes the baseline, executes the iterations, and compares the last
    invocation's diagnostics with a fresh all-files check of the final disk.
    There is no extra incremental check before comparison.

    Both runs use independent child processes started with [Daemon.spawn].
    Test binaries using this harness must call [Daemon.check_entry_point] before
    starting their tests. The runner uses real temporary files, the Rust provider
    backend, embedded HHI files, and no worker processes. Source files are never
    executed by HHVM.

    Raises [Internal_typechecker_error] if the baseline, any iteration, or the
    fresh check reports an internal typechecker error, including caught exceptions
    and invariant violations. These are rejected before the next iteration can
    clear them or the final comparison can accept matching internal errors.
    Raises [Diagnostics_mismatch] on disagreement, or fails if a child fails.
    Diagnostics are compared without temporary root paths or ordering.
    Saved-state runs apply the same saved warning hashes to the independent
    full check so both executions use the same warning suppression policy.

    [remote_old_decls] supplies Hack source for the old-declaration client's
    existing test fixture. It enables fetching missing old class declarations
    during incremental iterations; all fetches read that fixture. Requires
    [Saved_state] initialization, because the client requires a naming database.
    The fixture is outside the repository, and the independent full check keeps
    remote old declarations disabled. *)
val run :
  ?initialization:initialization -> ?remote_old_decls:string -> scenario -> unit

(** Run a scenario that reproduces a known bug, requiring exactly the listed
    incremental and fresh-check diagnostics. Each tuple is the repository-relative
    filename, error code, and primary message. Positions and reason messages are
    ignored; diagnostic order does not matter and duplicate entries are retained.

    Runner settings are passed through to [run].

    Unmatched diagnostic differences and internal errors propagate. If the checks
    agree, fail with [bug] and instructions to remove the wrapper. *)
val run_expect_bug :
  ?initialization:initialization ->
  ?remote_old_decls:string ->
  bug:string ->
  incremental:(string * int * string) list ->
  full:(string * int * string) list ->
  scenario ->
  unit
