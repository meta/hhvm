(*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

[%%if ocaml_version >= (5, 4, 0)]

let unloc id = id.Location.txt

let tuple_types types = List.map snd types

[%%else]

let unloc id = id

let tuple_types types = types

[%%endif]

[%%if ocaml_version >= (5, 3, 0)]

let constant_desc constant = constant.Parsetree.pconst_desc

[%%else]

let constant_desc constant = constant

[%%endif]

type longident =
  | Ident of string
  | Dot of longident * string
  | Apply of longident * longident

let rec longident = function
  | Longident.Lident name -> Ident name
  | Longident.Ldot (id, name) -> Dot (longident (unloc id), unloc name)
  | Longident.Lapply (f, arg) ->
    Apply (longident (unloc f), longident (unloc arg))
