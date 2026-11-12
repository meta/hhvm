(*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

val get_vm_hwm : unit -> int option

val get_vm_rss : unit -> int option

(** Bytes of memory the machine could give a new process without swapping
    (`MemAvailable`). [None] where the host does not say. *)
val get_mem_available : unit -> int option

(** [get_mem_available_and_total ()] is [get_mem_available] and the machine's
    total (`MemTotal`), read together. *)
val get_mem_available_and_total : unit -> (int * int) option

(** A collection of telemetry about the host we're on - cpus, memory etc *)
val get_host_hw_telemetry : unit -> Telemetry.t
