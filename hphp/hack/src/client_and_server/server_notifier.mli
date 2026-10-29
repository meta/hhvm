(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

type changes =
  | Unavailable
      (** e.g. because DFind is not available, or watchman subscription is down *)
  | SyncChanges of S_set.t
      (** contains all changes up to the point that the notifier was invoked *)
  | AsyncChanges of S_set.t
      (** contains whatever changes have been pushed up to this moment *)

type clock = Server_notifier_types.clock =
  | Watchman of Watchman.clock
  | Eden of Edenfs_watcher.clock
[@@deriving show, eq]

(** Prints the name of the file watcher used to create the clock. *)
val show_file_watcher_name : clock -> string

type t

(** This takes a filter, and returns all files under root that match *)
type indexer = (string -> bool) -> unit -> string list

val init :
  Server_args.options -> Server_local_config.t -> num_workers:int -> t * indexer

val init_null : unit -> t

(** Whether this server watches an Eden mount through Watchman. Watchman does
    not report a renamed directory on EdenFS (T118370264, T137602532): the server
    never learns of the files that moved. [Edenfs_watcher] handles it, and on an
    Eden checkout it is the watcher the server is meant to use. The server falls
    back to Watchman when it cannot start [Edenfs_watcher], even with
    [edenfs_file_watcher_enabled] set. Off Eden, Watchman sees the move. *)
val uses_watchman_on_eden_mount : t -> bool

val init_mock :
  get_changes_async:(unit -> changes) -> get_changes_sync:(unit -> S_set.t) -> t

val wait_until_ready : t -> unit

(** This might return AsyncChanges the ones that we happen to have received by now,
or SyncChanges, depending on the underlying notifier's state *)
val get_changes_async : t -> Telemetry.t -> changes * clock option * Telemetry.t

(** Returns all changes up to the point this was invoked, represented as a set
of changed files. This will raise an exception if the file watching service
is unavailable *)
val get_changes_sync : t -> Telemetry.t -> S_set.t * clock option * Telemetry.t

(** If supported by the backend, returns a file descriptor that can be used to
    be notified about file system changes. The FD being read-ready is a
    necessary condition for a subsequent call to `get_changes_{sync,async}`
    returning any changes. Note that this FD must only be used to
    `poll`/`select` on, but never actually read from. *)
val notification_fd : t -> Caml_unix.file_descr option

(** If supported by the backend, returns a value acting as a necessary condition
    for a subsequent call to `get_changes_{sync,async}` returning any changes. *)
val maybe_changes_available : t -> bool option

(** Returns the asserted states causing Eden to defer asynchronous file changes,
    sorted by name. Watchman deferral tracking is unsupported; all non-Eden
    backends return an empty list. Watcher errors are handled as in
    [get_changes_sync]. *)
val get_deferring_states : t -> string list

(** Returns telemetry about asserted repo states *)
val get_repo_states_telemetry : t -> Telemetry.t
