(*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)
open Hh_prelude
open Base.Option.Monad_infix

let lines path =
  try Some (Str.split (Str.regexp_string "\n") (Path.cat (Path.make path))) with
  | _ -> None

let read_status pid_str =
  let colon = Str.regexp_string ":" in
  let to_pairs line =
    match Str.split colon line with
    | [k; v] -> Some (Stdlib.String.trim k, Stdlib.String.trim v)
    | _ -> None
  in
  lines (Printf.sprintf "/proc/%s/status" pid_str)
  |> Option.map ~f:(List.filter_map ~f:to_pairs)

(*
  Convert the human-readable format of /proc/[pid]/status back to bytes
  Because the machine-readable /proc/[pid]/stat,statm files do not have the
  VmHWM we need
*)
let to_bytes_opt str =
  match Str.split (Str.regexp_string " ") str with
  | [amount; "kB"] -> Some (int_of_string amount * 1024)
  | _ -> None

let get_status_entry status ~key =
  List.Assoc.find status ~equal:String.( = ) key >>= to_bytes_opt

let get_vm_rss () = read_status "self" >>= get_status_entry ~key:"VmRSS"

let get_vm_hwm () = read_status "self" >>= get_status_entry ~key:"VmHWM"

(** Some interesting files are made of lines like "a  : b". This function
reads the file and splits it into [(a,b),...] pairs. *)
let colon_file_to_pairs ~(fn : string) : (string * string) list =
  match Sys_utils.cat_or_failed fn with
  | None -> []
  | Some text ->
    let colon = Str.regexp_string ":" in
    Str.split (Str.regexp_string "\n") text
    |> List.filter_map ~f:(fun line ->
           match Str.split colon line with
           | [k; v] -> Some (String.strip k, String.strip v)
           | _ -> None)

let get_mem_available () =
  get_status_entry (colon_file_to_pairs ~fn:"/proc/meminfo") ~key:"MemAvailable"

let get_mem_available_and_total () =
  let pairs = colon_file_to_pairs ~fn:"/proc/meminfo" in
  match
    ( get_status_entry pairs ~key:"MemAvailable",
      get_status_entry pairs ~key:"MemTotal" )
  with
  | (Some available, Some total) -> Some (available, total)
  | _ -> None

(** If we wish to store an arbitrary string as a json key, let's make it
friendly for json path accessors, by retaining only alphanumerics,
replacing everything else with _, and coalescing multiple _.
E.g. "foo:bar" will become "foo_bar". *)
let json_friendly_key (k : string) : string =
  k
  |> Str.global_replace (Str.regexp "[^a-zA-Z0-9]") "_"
  |> Str.global_replace (Str.regexp "_+") "_"

module Histogram = struct
  type t = int S_map.t

  let add (v : string) (hist : t) : t =
    match S_map.find_opt v hist with
    | None -> S_map.add v 1 hist
    | Some count -> S_map.add v (count + 1) hist

  let singleton (v : string) : t = S_map.singleton v 1

  (** If there is only one value in the histogram, just output this value.
      Else if all values have cardinality one, output values as a list.
      Else output the histogram as a dict. *)
  let add_to_telemetry (key : string) (hist : t) (telemetry : Telemetry.t) :
      Telemetry.t =
    if S_map.cardinal hist |> Int.equal 1 then
      Telemetry.string_ ~key ~value:(S_map.choose hist |> fst) telemetry
    else if S_map.for_all (fun _ -> Int.equal 1) hist then
      Telemetry.string_list
        ~key
        ~value:(S_map.keys hist |> List.sort ~compare:String.compare)
        telemetry
    else
      let hist_as_telemetry =
        hist
        |> S_map.bindings
        |> List.sort ~compare:(fun (k1, _) (k2, _) -> String.compare k1 k2)
        |> List.fold
             ~init:(Telemetry.create ())
             ~f:(fun telemetry (value, count) ->
               Telemetry.int_ ~key:value ~value:count telemetry)
      in
      Telemetry.object_ ~key ~value:hist_as_telemetry telemetry
end

(** Turns an assoc-list [(k,v),...] in an S_map. If there are multiple entries for a key
and they're all the same then we'll keep it, k->v. If there are multiple entries for
a key and any of them differ then we'll associate k with a histogram of values for v. *)
let assoc_to_dict (keyvals : (string * string) list) : Histogram.t S_map.t =
  (* first, turn it into a map {k -> [v1,v2,...]} for all values associated with a key *)
  List.fold keyvals ~init:S_map.empty ~f:(fun acc (k, v) ->
      match S_map.find_opt k acc with
      | None -> S_map.add k (Histogram.singleton v) acc
      | Some histogram -> S_map.add k (Histogram.add v histogram) acc)

let get_host_hw_telemetry () =
  (* this regexp will turn arbitrary ("key", "<number> <suffix>") into ("key__suffix", "<number>"),
     most usefully for thing like "Mem : 12345 kB" which becomes ("Mem__kB", "12345"). *)
  let re_suffix = Str.regexp "\\([0-9.]+\\) \\([a-zA-Z]+\\)" in
  let file_to_telemetry fn =
    let dict =
      colon_file_to_pairs ~fn
      |> List.map ~f:(fun (k, v) -> (json_friendly_key k, v))
      |> List.map ~f:(fun (k, v) ->
             if Str.string_match re_suffix v 0 then
               (k ^ "__" ^ Str.matched_group 2 v, Str.matched_group 1 v)
             else
               (k, v))
      |> assoc_to_dict
    in
    S_map.fold Histogram.add_to_telemetry dict (Telemetry.create ())
  in
  let sandcastle_capabilities =
    match
      Sys_utils.cat_or_failed "/data/sandcastle/run/sandcastle.capabilities"
    with
    | None -> `Null
    | Some s ->
      (try Yojson.Safe.from_string s with
      | _ -> `String s)
  in
  Telemetry.create ()
  |> Telemetry.object_ ~key:"meminfo" ~value:(file_to_telemetry "/proc/meminfo")
  |> Telemetry.object_ ~key:"cpuinfo" ~value:(file_to_telemetry "/proc/cpuinfo")
  |> Telemetry.json
       ~key:"sandcastle_capabilities"
       ~value:sandcastle_capabilities
