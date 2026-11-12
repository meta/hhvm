(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

(** Outbound edges: the files a given file statically references.

    The dependency graph only stores dependents, so this direction comes from
    the AST. An edge is a reference to a top-level definition — a class,
    function or global constant — since naming a member also names its class. *)

(** [ensure_indexed ~ctx ~workers paths] indexes [paths] across workers,
    skipping those already cached. Batch widely:
    the pool only pays for itself once there are enough files to fill it.

    Safe on workers because indexing only reads source. Anything reading the
    dependency graph would not be: a forked worker sees the memory-mapped [base]
    but not the [delta] on the master's heap.

    [check_memory] is called by each worker before every file. When it raises
    [Server_isolation_memory.Below_floor] that worker stops, the others stop at
    their next file, and the exception is re-raised here once the call returns.
    The files not reached are not indexed. *)
val ensure_indexed :
  ctx:Provider_context.t ->
  workers:Multi_worker.worker list option ->
  check_memory:(unit -> unit) ->
  Relative_path.t list ->
  unit

(** [get ctx path] is the files referenced by [path], excluding itself,
    computed and cached on first call. Master-only: the cache records what it wrote in process-local state,
    so a worker calling this would leave entries [reset] cannot drop.

    An unreadable file yields no edges rather than raising, and that empty
    result is cached; failures that put the whole run in doubt are re-raised. *)
val get : Provider_context.t -> Relative_path.t -> Relative_path.Set.t

(** Drop every cached entry, keeping the [files_indexed] tally. Nothing evicts
    otherwise, so a caller done with a set of files should release; a dropped
    entry is recomputed if asked for again. *)
val release_cache : unit -> unit

(** How many files have been through indexing, including unreadable ones. *)
val files_indexed : unit -> int

(** Drop every cached edge set and zero the tally. Entries are keyed by path
    alone and outlive a call, so a caller must reset before a run or risk edges
    derived from a file's earlier contents. Only paths recorded as they were
    added are dropped — the heap has no iteration, so a future writer must
    record what it wrote. *)
val reset : unit -> unit
