(*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

val to_rust : ?for_open:bool -> Ast_compat.longident -> string

val longident_to_string : ?for_open:bool -> Longident.t -> string
