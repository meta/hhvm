(*
 * Copyright (c) 2014, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude

val make :
  longlived_workers:bool ->
  set_global_provider_backend_in_workers:bool ->
  nbr_procs:int ->
  Gc.control ->
  Shared_mem.handle ->
  logging_init:(unit -> unit) ->
  Multi_worker.worker list
