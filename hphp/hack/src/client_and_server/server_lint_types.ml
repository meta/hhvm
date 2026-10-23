(*
 * Copyright (c) 2016, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude
module Lint = Lints_core

type result = Pos.absolute Lint.t list

let output_json ?(from_test = false) ?(pretty = false) oc el =
  let errors_json = List.map el ~f:Lint.to_json in
  let version =
    if from_test then
      ""
    else
      Hh_version.version
  in
  let res : Yojson.Safe.t =
    `Assoc [("errors", `List errors_json); ("version", `String version)]
  in
  let json_str =
    if pretty then
      Yojson.Safe.pretty_to_string res ^ "\n"
    else
      Yojson.Safe.to_string res
  in
  Out_channel.output_string oc json_str;
  Out_channel.flush stderr

let output_text oc el format =
  (* Essentially the same as type error output, except that we only have one
   * message per error, and no additional 'typing reasons' *)
  (if List.is_empty el then
    Out_channel.output_string oc "No lint errors!\n"
  else
    let f =
      match format with
      | Diagnostics.Extended
      | Diagnostics.Context ->
        Lint.to_contextual_string
      | Diagnostics.PlainHighlighted ->
        (fun lint -> Lint.to_plain_highlighted_string lint)
      | Diagnostics.Raw
      | Diagnostics.Agent
      | Diagnostics.Plain ->
        Lint.to_string
      | Diagnostics.Highlighted -> Lint.to_highlighted_string
    in
    let sl = List.map el ~f in
    List.iter sl ~f:(fun s -> Printf.fprintf oc "%s\n%!" s));
  Out_channel.flush oc
