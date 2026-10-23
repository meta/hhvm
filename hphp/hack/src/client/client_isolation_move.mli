(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

(** [move_into args ~connect ~options ~error_filter ~parent] moves the
    directory [options.seed_dir], repo-relative, into [parent] under its own
    name, if nothing outside references into it and the move introduces no
    errors, and otherwise leaves it where it was. It asks the running server,
    with the isolation query for each directory and the status for the errors,
    each answered after the server has picked up every change on disk. It prints
    what came of it, as JSON when [args.output_json] is set, and gives the exit
    status: zero only when the directory moved.

    [connect] opens a new connection to the server. Each request needs its own,
    because the server closes the connection after it answers.

    Source control is not told. Raises if an undo cannot be completed. *)
val move_into :
  Client_env.client_check_env ->
  connect:(use_priority_pipe:bool -> Client_connect.conn Lwt.t) ->
  options:Server_isolation_types.options ->
  error_filter:Filter_diagnostics.Filter.t ->
  parent:string ->
  Exit_status.t Lwt.t
