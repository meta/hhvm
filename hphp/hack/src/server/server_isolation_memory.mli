(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

(** What a run reports about its own memory. An isolation run is bounded by
    memory rather than by time, so every progress line carries these. *)

(** Resident set size in GiB. [0.] when the platform does not report one, which
    a caller cannot tell from a genuine zero. *)
val rss_gib : unit -> float

(** The shared heap in GiB. *)
val heap_gib : unit -> float

(** Raised by [check] with the sentence that says why the run stopped. *)
exception Below_floor of string

(** The two limits a run is held to, and the memory the machine could give when
    it began. Small and plain, so it can be sent to the workers. *)
type guard

(** [guard ~min_free_percent ~max_used_percent ~available_at_start] holds the
    memory available as the run began, in bytes, which the second limit is
    measured from. [None] where the host does not report it. *)
val guard :
  min_free_percent:int ->
  max_used_percent:int ->
  available_at_start:int option ->
  guard

(** [reason guard ~available ~total] is the sentence that says why a machine
    with [available] of [total] bytes is past either limit, or [None]. *)
val reason : guard -> available:int -> total:int -> string option

(** [check guard] raises [Below_floor] when the memory the machine could still
    give, whoever is using the rest, is under [min_free_percent] of its total,
    or has fallen by more than [max_used_percent] of its total since [guard]
    was made. The machine, not this process, since the workers are processes of
    their own and the whole pool is what exhausts memory. A limit of 0 is off,
    and nothing is checked where the host does not report its memory. *)
val check : guard -> unit
