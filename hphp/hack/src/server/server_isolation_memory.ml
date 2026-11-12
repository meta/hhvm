(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

let bytes_per_gib = 1024. *. 1024. *. 1024.

let gib bytes = float_of_int bytes /. bytes_per_gib

let rss_gib () =
  match Memory_stats.get_vm_rss () with
  | Some bytes -> gib bytes
  | None -> 0.

let heap_gib () = gib (Shared_mem.SMTelemetry.heap_size ())

exception Below_floor of string

type guard = {
  min_free_percent: int;
  max_used_percent: int;
  available_at_start: int option;
}

let guard ~min_free_percent ~max_used_percent ~available_at_start =
  { min_free_percent; max_used_percent; available_at_start }

let reason
    { min_free_percent; max_used_percent; available_at_start } ~available ~total
    =
  let floor = total / 100 * min_free_percent in
  if min_free_percent > 0 && available < floor then
    Some
      (Printf.sprintf
         "stopped because %.1f GiB of this machine's memory is available, under the floor of %d%% (%.1f GiB). Clusters written before this are complete. To fit, lower --isolation-batch-size or --isolation-max-cluster-size; --isolation-min-free-memory sets the floor."
         (gib available)
         min_free_percent
         (gib floor))
  else
    match available_at_start with
    | Some start when max_used_percent > 0 ->
      let budget = total / 100 * max_used_percent in
      if start - available > budget then
        Some
          (Printf.sprintf
             "stopped because the memory available on this machine has fallen by %.1f GiB since the run started, over its limit of %d%% of the machine's memory (%.1f GiB). Clusters written before this are complete. To fit, lower --isolation-batch-size or --isolation-max-cluster-size; --isolation-max-memory sets the limit."
             (gib (start - available))
             max_used_percent
             (gib budget))
      else
        None
    | _ -> None

let check guard =
  match Memory_stats.get_mem_available_and_total () with
  | Some (available, total) ->
    (match reason guard ~available ~total with
    | Some message -> raise (Below_floor message)
    | None -> ())
  | None -> ()
