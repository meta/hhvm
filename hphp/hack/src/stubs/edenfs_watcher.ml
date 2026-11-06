(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude

type clock = string [@@deriving show, eq]

(** As in the Buck watcher mock, this pipe is never written to and stays unreadable. *)
type instance = { notification_fd: Caml_unix.file_descr }

type edenfs_watcher_error = Edenfs_watcher_types.edenfs_watcher_error
[@@deriving show]

type changes = Edenfs_watcher_types.changes [@@deriving show]

let require_test_stubbing () =
  if
    not
      (Injector_config.use_test_stubbing
      && not (Injector_config.Exclusions.exclude_edenfs_watcher ()))
  then
    failwith "not implemented"

(** Dune cannot link the real Eden watcher, but tests use its mocking interface. *)
module Mocking = struct
  let pending_changes : changes list ref = ref []

  let asserted_states : (string list, edenfs_watcher_error) result ref =
    ref (Ok [])

  let get_changes_async_returns changes =
    require_test_stubbing ();
    pending_changes := changes

  let get_asserted_states_returns states =
    require_test_stubbing ();
    asserted_states := states
end

let init ~destroy_on_exit:(_ : bool) (_settings : Edenfs_watcher_types.settings)
    =
  require_test_stubbing ();
  let (read_fd, _write_fd) = Unix.pipe () in
  Ok ({ notification_fd = read_fd }, "")

let destroy (_instance : instance) : (unit, edenfs_watcher_error) result = Ok ()

let get_changes_sync (_instance : instance) :
    (changes list * clock * Telemetry.t option, edenfs_watcher_error) result =
  failwith "not implemented"

let get_changes_async (_instance : instance) :
    (changes list * clock * Telemetry.t option, edenfs_watcher_error) result =
  require_test_stubbing ();
  let changes = !Mocking.pending_changes in
  Mocking.pending_changes := [];
  Ok (changes, "", None)

let get_notification_fd (instance : instance) :
    (Caml_unix.file_descr, edenfs_watcher_error) result =
  require_test_stubbing ();
  Ok instance.notification_fd

let get_all_files (_instance : instance) :
    (string list * Telemetry.t option, edenfs_watcher_error) result =
  failwith "not implemented"

let get_repo_states (_instance : instance) : string list * float S_map.t =
  failwith "not implemented"

let get_asserted_states (_instance : instance) :
    (string list, edenfs_watcher_error) result =
  require_test_stubbing ();
  !Mocking.asserted_states

module Standalone = struct
  let get_changes_since _settings _clock = failwith "not implemented"
end
