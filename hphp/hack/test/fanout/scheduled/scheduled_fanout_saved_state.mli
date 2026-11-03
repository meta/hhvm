(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *)

(** Local saved-state artifacts generated from a scenario's initial files. *)
type t

(** Run [HH_DISTC_PATH] and [WORKER_PATH] with the saved-state integration tests'
    distc options to create a real saved state. This uses distc's internal
    execution infrastructure. [root] must contain the initial files; this writes
    [.hhconfig] with the standard-library and XHP settings from [parser_options].
    [directory] must be a fresh directory owned by the caller. The generated
    baseline must contain no warnings.

    This does not initialize the coordinator's typechecker or shared memory.
    The caller owns the artifacts and must retain them until both checks finish. *)
val create :
  root:string -> parser_options:Parser_options.t -> directory:string -> t

(** Load naming, dependencies, and saved issue metadata into a fresh environment
    with the Rust backend and HHI root already initialized. No parsing or
    typechecking runs here, so declaration caches stay cold. Files with saved
    errors are queued in [needs_recheck]; the saved state contains their paths,
    not their diagnostics. *)
val load : t -> Server_env.env -> Server_env.env
