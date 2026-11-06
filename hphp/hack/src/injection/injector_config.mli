(*
 * Copyright (c) 2015, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

(* This file provides only the interface, so injector configuration
 * can be retreived without depending on the *actual* implementation
 * file. This is because we want libraries to be able to refer to the config,
 * but the actual injector to be chosen by the binary being built.
 *
 * Note: Buck doesn't currently have a build rule to only build .mli files
 * into .cmi, so you need to compile against this file directly. *)
val use_test_stubbing : bool

(** Per-module exclusions in the test injector, configured through
    [HH_MOCKING_EXCLUDE_*] environment variables. The default injector returns
    [false] for all exclusions (since mocking is disabled anyway).

    Set exclusions before starting the process: implementations are selected
    during module initialization. *)
module Exclusions : sig
  val exclude_watchman : unit -> bool

  val exclude_disk : unit -> bool

  val exclude_hg : unit -> bool

  val exclude_gk : unit -> bool

  val exclude_client_provider : unit -> bool

  val exclude_edenfs_watcher : unit -> bool

  val exclude_everstore_package : unit -> bool

  val exclude_typing_check_service : unit -> bool
end
