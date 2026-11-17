(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

(** Inbound closure: every file that transitively depends on a given file.

    Upward-closed over files — not merely over symbols — so nothing outside a
    closure references into it and it is safe to take whole. Take part of one
    and that stops being true. *)

(** [get ctx naming_table ~max path] is the closure of [path], including [path]
    itself; [None] when it passes [max] files, or when the naming table does not
    know [path].

    Memoised on [path] alone, so a caller that varies [max] must [reset] between.

    Master-only: a forked worker sees the memory-mapped [base] graph but not the
    [delta] on the master's heap, so it may be trusted to say a closure is too
    large, never that one is complete. *)
val get :
  Provider_context.t ->
  Naming_table.t ->
  max:int ->
  Relative_path.t ->
  Relative_path.Set.t option

(** Closures computed rather than served from the memo, [None] included. *)
val closures_computed : unit -> int

(** Requests the memo served. *)
val closures_served : unit -> int

(** Refusals held; one path each, the cheap half of the memo. *)
val refusals_held : unit -> int

(** Drop the accepted closures, keep the refusals. Closures are expensive to
    hold and cheap to recompute; refusals are the reverse. *)
val release_closures : unit -> unit

(** Drop everything, refusals included, and zero the counters. Entries outlive a
    call, so a caller must reset before a run or risk a closure computed from a
    file's earlier contents, or under a different [max]. *)
val reset : unit -> unit

(** [referrers ctx naming_table candidate] is every file outside [candidate]
    that references a symbol defined inside it. Direct references only, where
    [get] walks the closure. Not memoised, unlike [get]. Master-only, like
    [get]. *)
val referrers :
  Provider_context.t ->
  Naming_table.t ->
  Relative_path.Set.t ->
  Relative_path.Set.t

(** [referrers_by_target ctx naming_table candidate] is each of the files that
    [referrers] gives, with the files of [candidate] that it references. It asks
    once for each file of [candidate], where [referrers] asks once for all of
    them, so it is for saying why a set is blocked, not for deciding it. *)
val referrers_by_target :
  Provider_context.t ->
  Naming_table.t ->
  Relative_path.Set.t ->
  Relative_path.Set.t Relative_path.Map.t
