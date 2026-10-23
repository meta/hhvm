(*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

val to_string : Diagnostics.finalized_diagnostic -> string

(** Use the supplied source text for the diagnostic's claim file. *)
val to_string_with_source :
  ?source:string -> Diagnostics.finalized_diagnostic -> string
