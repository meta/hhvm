(*
 * Copyright (c) 2015, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude
module Printexc = Stdlib.Printexc

(** Callstack is simply a typed way to indicate that a string is a callstack *)
type callstack = Callstack of string [@@deriving show]

let () = Random.self_init ~allow_in_tests:true ()

module Map = struct end

let spf = Printf.sprintf

let print_endlinef fmt = Printf.ksprintf Stdio.print_endline fmt

let prerr_endlinef fmt = Printf.ksprintf Stdio.prerr_endline fmt

let print_count n noun =
  if n = 1 then
    spf "1 %s" noun
  else
    spf "%d %ss" n noun

let pp_large_list ?(pp_sep = None) ?(max_items = 5) pp_elt fmt xs =
  let list_len = List.length xs in
  if list_len <= max_items then
    Format.pp_print_list ?pp_sep pp_elt fmt xs
  else
    let xs = List.take xs max_items in
    Format.fprintf
      fmt
      "<only showing first %d of %d elems: %a>"
      max_items
      list_len
      (Format.pp_print_list ?pp_sep pp_elt)
      xs

let timestring (time : float) : string =
  let tm = Unix.localtime time in
  Printf.sprintf
    "[%d-%02d-%02d %02d:%02d:%02d.%03d]"
    (tm.Unix.tm_year + 1900)
    (tm.Unix.tm_mon + 1)
    tm.Unix.tm_mday
    tm.Unix.tm_hour
    tm.Unix.tm_min
    tm.Unix.tm_sec
    (Int.of_float (time *. 1000.) % 1000)

let time (timestring : string) : float =
  let to_float year mon tm_mday tm_hour tm_min tm_sec millsec =
    let tm =
      Unix.
        {
          tm_sec;
          tm_min;
          tm_hour;
          tm_mday;
          tm_mon = mon - 1;
          tm_year = year - 1900;
          tm_wday = 0;
          tm_yday = 0;
          tm_isdst = true;
        }
    in
    fst (Unix.mktime tm) +. (Float.of_int millsec *. 0.001)
  in
  Scanf.sscanf timestring "[%d-%02d-%02d %02d:%02d:%02d.%03d]" to_float

let opt f env = function
  | None -> (env, None)
  | Some x ->
    let (env, x) = f env x in
    (env, Some x)

let singleton_if cond x =
  if cond then
    [x]
  else
    []

let rec wfold_left2 f env l1 l2 =
  match (l1, l2) with
  | ([], _)
  | (_, []) ->
    env
  | (x1 :: rl1, x2 :: rl2) ->
    let env = f env x1 x2 in
    wfold_left2 f env rl1 rl2

let sl l = List.fold_right l ~f:( ^ ) ~init:""

let maybe f env = function
  | None -> ()
  | Some x -> f env x

(* Since OCaml usually runs w/o backtraces enabled, the note makes errors
 * easier to debug. *)
let unsafe_opt_note note = function
  | None -> raise (Invalid_argument note)
  | Some x -> x

let unsafe_opt x = unsafe_opt_note "unsafe_opt got None" x

let try_with_stack (f : unit -> 'a) : ('a, Exception.t) result =
  try Ok (f ()) with
  | exn ->
    let e = Exception.wrap exn in
    Error e

let set_of_list l = List.fold_right l ~f:S_set.add ~init:S_set.empty

(** \A\B\C -> A\B\C *)
let strip_ns s = String.chop_prefix_if_exists s ~prefix:"\\"

let strip_xhp_ns s = String.chop_prefix_if_exists s ~prefix:":"

let strip_both_ns s = s |> strip_ns |> strip_xhp_ns

(* \HH\C -> C
 * \HH\Lib\C -> C
 * \A\B\C -> A\B\C
 *)
let strip_hh_lib_ns s =
  s
  |> String.chop_prefix_if_exists ~prefix:"\\HH\\Lib\\"
  |> String.chop_prefix_if_exists ~prefix:"\\HH\\"
  |> strip_ns

(* A\B\C -> \A\B\C *)
let add_ns s =
  if String.is_prefix s ~prefix:"\\" then
    s
  else
    "\\" ^ s

(* A:B:C -> :A:B:C *)
let add_xhp_ns s =
  if String.is_prefix s ~prefix:":" then
    s
  else
    ":" ^ s

(* \A\B\C -> C *)
let strip_all_ns s =
  match String.rindex s '\\' with
  | Some pos ->
    let base_name_start = pos + 1 in
    String.sub s ~pos:base_name_start ~len:(String.length s - base_name_start)
  | None -> s

(* "\\A\\B\\C" -> ("\\A\\B\\" * "C") *)
let split_ns_from_name (s : string) : string * string =
  match String.rindex s '\\' with
  | Some pos ->
    let base_name_start = pos + 1 in
    let name_part =
      String.sub s ~pos:base_name_start ~len:(String.length s - base_name_start)
    in
    let namespace_part = String.sub s ~pos:0 ~len:base_name_start in
    (namespace_part, name_part)
  | None -> ("\\", s)

(* Expands a namespace using the namespace map, a list of (string, string) tuples
 * Ensures the beginning backslash is present
 *
 * "Str\\join" -> "\\HH\\Lib\\Str\\join" (when "Str", "HH\\Lib\\Str" is present in map)
 * "HH\\Lib\\Str\\Join" -> "\\HH\\Lib\\Str\\join"
 * "\\HH\\Lib\\Str\\Join" -> "\\HH\\Lib\\Str\\join"
 * "just_plain_func" -> "\\just_plain_func"
 *)
let expand_namespace (ns_map : (string * string) list) (s : string) : string =
  let (raw_ns, name) = split_ns_from_name s in
  (* Might need left backslash *)
  let ns = add_ns raw_ns in
  let matching_alias =
    List.find ns_map ~f:(fun (alias, _) ->
        let fixup = add_ns alias ^ "\\" in
        String.equal fixup ns)
  in
  match matching_alias with
  | None -> add_ns s
  | Some (_, expanded) -> add_ns (expanded ^ "\\" ^ name)

(*****************************************************************************)
(* Same as List.iter2, except that we only iterate as far as the shortest
 * of both lists.
 *)
(*****************************************************************************)

let rec iter2_shortest f l1 l2 =
  match (l1, l2) with
  | ([], _)
  | (_, []) ->
    ()
  | (x1 :: rl1, x2 :: rl2) ->
    f x1 x2;
    iter2_shortest f rl1 rl2

let compose f g x = f (g x)

module With_complete_flag = struct
  type 'a t = {
    is_complete: bool;
    value: 'a;
  }
end

let try_finally ~f ~(finally : unit -> unit) =
  let res =
    try f () with
    | exn ->
      let e = Exception.wrap exn in
      finally ();
      Exception.reraise e
  in
  finally ();
  res

let try_all steps =
  let first_exception =
    List.fold steps ~init:None ~f:(fun first_exception step ->
        match try_with_stack step with
        | Ok () -> first_exception
        | Error exn -> Option.first_some first_exception (Some exn))
  in
  Option.iter first_exception ~f:Exception.reraise

let with_context
    ~(enter : unit -> unit) ~(exit : unit -> unit) ~(do_ : unit -> 'a) : 'a =
  enter ();
  let result =
    try do_ () with
    | exn ->
      let e = Exception.wrap exn in
      exit ();
      Exception.reraise e
  in
  exit ();
  result

(* We run with exception backtraces turned off for performance reasons. But for
 * some kinds of catastrophic exceptions, which we never recover from (so the
 * performance doesn't matter) we do want the backtrace. "assert false" is one
 * of such conditions.
 *)
let assert_false_log_backtrace msg =
  Printf.eprintf "assert false with backtrace:\n";
  Option.iter msg ~f:(Printf.eprintf "%s\n");
  Printf.eprintf
    "%s"
    (Printexc.raw_backtrace_to_string (Printexc.get_callstack 100));
  assert false

(* Returns the largest element in arr strictly less than `bound` *)
let infimum (arr : 'a array) (bound : 'b) (compare : 'a -> 'b -> int) :
    int option =
  let rec binary_search low high =
    if low = high then
      Some low
    else if low > high then
      None
    else
      let mid = (low + high + 1) / 2 in
      let test = arr.(mid) in
      if compare test bound < 0 then
        binary_search mid high
      else
        binary_search low (mid - 1)
  in
  binary_search 0 (Array.length arr - 1)

let unwrap_snd (a, b_opt) =
  match b_opt with
  | None -> None
  | Some b -> Some (a, b)
