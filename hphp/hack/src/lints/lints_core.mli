(*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

type severity =
  | Lint_error
  | Lint_warning
  | Lint_advice
  | Lint_disabled

type 'pos t = {
  code: int;
  severity: severity;
  pos: 'pos; [@opaque]
  message: string;
  bypass_changed_lines: bool;
  autofix: (string * Pos.t) option;
  check_status: Tast.check_status option;
}
[@@deriving show]

val get_code : 'pos t -> int

val get_pos : 'pos t -> 'pos

val add :
  ?check_status:Tast.check_status option ->
  ?bypass_changed_lines:bool ->
  ?autofix:(string * Pos.t) option ->
  int ->
  severity ->
  Pos.t ->
  string ->
  unit

val add_lint : Pos.t t -> unit

val to_absolute : Pos.t t -> Pos.absolute t

val to_string : Pos.absolute t -> string

val to_contextual_string : Pos.absolute t -> string

val to_plain_highlighted_string : ?source:string -> Pos.absolute t -> string

val to_highlighted_string : Pos.absolute t -> string

val to_json : Pos.absolute t -> Yojson.Safe.t

val do_ : (unit -> 'a) -> Pos.t t list * 'a
