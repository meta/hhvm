(*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

(** Runs the hooks registered with [add_hook_upon_clean_exit], then exits.
    The process exits with [exit_code_override] if given, ignoring the exit
    code that [Exit_status.exit_code exit_status] would give. *)
val exit :
  ?msg:string ->
  ?telemetry:Telemetry.t ->
  ?stack:string ->
  ?exit_code_override:int ->
  Exit_status.t ->
  'a

val add_hook_upon_clean_exit : (Exit_status.finale_data -> unit) -> unit
