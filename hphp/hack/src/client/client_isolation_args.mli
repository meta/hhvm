(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

(** [Error] on a combination of [--isolation-*] flags the run cannot honour.
    Combinations it can honour but that almost certainly do not mean what the
    caller intended are warned about on stderr, since they do not stop the run.

    [max_dependents_given] tells the two apart for that bound: it reaches here
    resolved to its default, and warning about a value the caller never chose
    would report their own default back at them.

    [mode_flags_given] names the flags that each select a mode: the options
    carry one mode, so two flags given together could not be seen in them. *)
val validate :
  max_dependents_given:bool ->
  mode_flags_given:string list ->
  Server_isolation_types.options ->
  (unit, string) Stdlib.result

(** The share of the machine's memory that must stay available for a run to
    continue. Growth can use all of it, and the workers are processes of their
    own, so the server checks the machine rather than itself. *)
val default_min_free_memory_percent : int

(** The share of the machine's memory that a run may take, measured from what
    was available when it began. The floor above stops a run when the machine is
    low whoever is to blame; this one stops a run that is taking a lot even when
    the machine is not yet low. *)
val default_max_memory_percent : int
