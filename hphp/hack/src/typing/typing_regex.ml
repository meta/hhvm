(*
 * Copyright (c) 2018, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude
open Typing_defs
open Aast
module Reason = Typing_reason
module MakeType = Typing_make_type

exception Empty_regex_pattern

exception Missing_delimiter

exception Invalid_global_option

let internal_error s =
  failwith ("Something (internal) went wrong while typing a regex string: " ^ s)

(*  bottom is non-inclusive *)
let rec int_keys p top bottom acc_i =
  if top <= bottom then
    acc_i
  else
    int_keys
      p
      (top - 1)
      bottom
      (TShapeField.make
         (TSFregex_group (Pos_or_decl.of_raw_pos p, string_of_int top))
      :: acc_i)

(* Assumes that names_numbers is sorted in DECREASING order of numbers *)
let rec keys_aux p top names_numbers acc =
  match names_numbers with
  | [] -> int_keys p top 0 [] @ acc
  | (name, number) :: t ->
    keys_aux
      p
      (number - 1)
      t
      (TShapeField.make (TSFlit_str (Pos_or_decl.of_raw_pos p, name))
      :: (int_keys p top number [] @ acc))

(*
 *  Any shape keys for our match type except 0. For re"Hel(\D)(?'o'\D)", this is
 * [ TSFregex_group (p, "1"); TSFlit_str (p, 'o') ].
 *
 *)
let keys p s ~flags =
  (* Compile `re`-prefixed string for use in Pcre functions *)
  let pattern = Pcre.regexp s ~flags in
  (* For re"Hel(\D)(?'o'\D)", this is 2. *)
  let count =
    try Pcre.capturecount pattern with
    | Pcre.Error (Pcre.InternalError s) -> internal_error s
  in
  (* For re"Hel(\D)(?'o'\D)", this is ['o']. *)
  let names =
    try Array.to_list (Pcre.names pattern) with
    | Pcre.Error (Pcre.InternalError s) -> internal_error s
  in
  (*  For re"Hel(\D)(?'o'\D)", this is [2] *)
  let numbers =
    try List.map ~f:(Pcre.get_stringnumber pattern) names with
    | Invalid_argument s -> internal_error s
  in
  let names_numbers = List.zip_exn names numbers in
  let names_numbers_sorted =
    List.sort
      ~compare:(fun nn1 nn2 -> ~-(Int.compare (snd nn1) (snd nn2)))
      names_numbers
  in
  keys_aux p count names_numbers_sorted []

let type_match p s ~flags =
  let r = Reason.regex p in
  let sft = { sft_optional = false; sft_ty = MakeType.string r } in
  let keys = keys p s ~flags in
  let shape_map =
    List.fold_left
      ~f:(fun acc key -> TShapeMap.add key sft acc)
      ~init:TShapeMap.empty
      keys
  in
  (* Any Regex\Match will contain the entire matched substring at key 0 *)
  let shape_map =
    TShapeMap.add
      (TShapeField.make (TSFregex_group (Pos_or_decl.of_raw_pos p, "0")))
      sft
      shape_map
  in
  MakeType.closed_shape r shape_map

let get_global_options s =
  List.fold_left (String.to_list_rev s) ~init:[] ~f:(fun acc x ->
      match x with
      | 'u' -> `UTF8 :: acc
      | 'i' -> `CASELESS :: acc
      | 'm' -> `MULTILINE :: acc
      | 's' -> `DOTALL :: acc
      | 'x' -> `EXTENDED :: acc
      | 'A' -> `ANCHORED :: acc
      | 'D' -> `DOLLAR_ENDONLY :: acc
      | 'U' -> `UNGREEDY :: acc
      | 'X' -> `EXTRA :: acc
      | 'S' -> acc
      | _ -> raise Invalid_global_option)

let complement c =
  match c with
  | '(' -> ')'
  | '{' -> '}'
  | '<' -> '>'
  | '[' -> ']'
  | _ -> c

(* Takes in delimiter-stripped string, checks brace-like delimiters *)
let check_balanced_delimiters s delim =
  let length = String.length s in
  let delim_closed = complement delim in
  let rec check_acc d i =
    if d < 0 then
      raise Missing_delimiter
    else if i < length then
      let (d2, i2) =
        match s.[i] with
        | x when Char.equal x delim -> (d + 1, i + 1)
        | x when Char.equal x delim_closed -> (d - 1, i + 1)
        (* Skip escape characters *)
        | '\\' -> (d, i + 2)
        | _ -> (d, i + 1)
      in
      check_acc d2 i2
    else if d > 0 then
      raise Missing_delimiter
    else
      s
  in
  check_acc 0 0

(* Takes in regex string and strips outer delimiters, checks and strips
   nested brace-like delimiters.
*)
let check_and_strip_delimiters s =
  (*  Non-alphanumeric, non-whitespace, non-backslash characters are delimiter-eligible *)
  let delimiter = Str.regexp "[^a-zA-Z0-9\t\n\r\x0b\x0c \\]" in
  let length = String.length s in
  if Int.equal length 0 then
    raise Empty_regex_pattern
  else
    let first = s.[0] in
    if Str.string_match delimiter (String.make 1 first) 0 then
      let closed_delim = complement first in
      let no_first_delim = String.sub s ~pos:1 ~len:(length - 1) in
      match String.rindex_from no_first_delim (length - 2) closed_delim with
      | Some i ->
        let flags =
          get_global_options (String.sub s ~pos:(i + 2) ~len:(length - i - 2))
        in
        let stripped_string = String.sub s ~pos:1 ~len:i in
        if not (Char.equal closed_delim first) then
          (check_balanced_delimiters stripped_string first, flags)
        else
          (stripped_string, flags)
      | None -> raise Missing_delimiter
    else
      raise Missing_delimiter

let type_pattern (_, p, e_) =
  match e_ with
  | String s ->
    let (s, flags) = check_and_strip_delimiters s in
    let match_type = type_match p s ~flags in
    let r = Reason.regex p in
    mk
      ( r,
        Tnewtype
          (Naming_special_names.Regex.tPattern, [match_type], MakeType.string r)
      )
  | _ -> failwith "Should have caught non-Ast_defs.String prefixed expression!"
