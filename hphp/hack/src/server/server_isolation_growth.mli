(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

(** Growing a set of files that nothing outside it references.

    A set is safe when every file referencing one of its members is also in it.
    Growth starts from a seed's closure and adds a file only when everything
    referencing it is already inside, or when it arrives together with everything
    referencing it — so the set is safe at every step, not only at the end, which
    is what makes a truncated result still correct.

    Which files are tried is a separate question from which are safe to take.
    Getting the first wrong finds fewer sets; only the second finds a wrong one.

    Everything here is scoped to one batch: the rounds, the two rules they
    apply, and the caches those rules fill. Choosing the seeds, running batch
    after batch, and reporting what came out belong to the caller. *)

(** What a batch spent and decided, for the caller's progress reporting. *)
module Stats : sig
  type t = {
    index_seconds: float;
    absorb_seconds: float;
    candidates_tested: int;
    candidates_skipped: int;
        (** Candidates whose blocker had not moved, so not retested. *)
    absorbed_by_subset: int;
    absorbed_by_closure: int;
  }
end

(** A grown cluster. [truncated] means the cap refused it a candidate, so it
    stopped short of closing: a consumer treating it as finished will find files
    outside it referencing in. *)
type grown = {
  seed: Relative_path.t;
  files: Relative_path.Set.t;
  truncated: bool;
}

(** [grow_batch ctx workers deps_mode naming_table ~claimed ~max_cluster_size
    ~max_dependents seeds] grows each of [seeds], returning it with the set it
    grew into.

    [claimed] is every file already reported by an earlier batch; growth will not
    take one. It is not extended as this batch grows, so two clusters in one
    batch can both take the same file and the caller has to settle which keeps
    it.

    [max_dependents] bounds how far the closure rule reaches for one candidate.
    It must be at least 1, since a seed is grown from its own closure, and no
    larger than [max_cluster_size], or a cluster would open already over the
    cap.

    [check_memory] is called before every round, and may raise to stop the batch
    where it is: a round is what takes the memory, and a batch can run for
    hundreds of them. *)
val grow_batch :
  Provider_context.t ->
  Multi_worker.worker list option ->
  Typing_deps.Mode.t ->
  Naming_table.t ->
  claimed:Relative_path.Set.t ->
  max_cluster_size:int option ->
  max_dependents:int ->
  check_memory:(unit -> unit) ->
  Relative_path.t list ->
  grown list

(** Accumulates across a run; the caller resets between queries. *)
val stats : unit -> Stats.t

val reset_stats : unit -> unit

(** Drop what a batch filled. Growth releases per round as it goes; a caller
    finished with a batch should release too, since entries are keyed by path
    and outlive a query. *)
val release_caches : unit -> unit
