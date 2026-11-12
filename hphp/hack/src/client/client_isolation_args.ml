(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude

exception Invalid of string

let reject message = raise (Invalid message)

let warn message = Printf.eprintf "warning: %s\n%!" message

(* Low enough that a run that would have finished is not stopped for a few
   GiB. *)
let default_min_free_memory_percent = 15

let default_max_memory_percent = 25

let check
    ~max_dependents_given
    ~mode_flags_given
    Server_isolation_types.
      {
        mode;
        clusters_destination = _;
        seed_framework;
        seed_list;
        seed_dir;
        max_dependents;
        max_cluster_size;
        batch_size;
        min_free_memory_percent;
        max_memory_percent;
        allow_long_run = _;
        max_seeds;
        seed_offset;
      } =
  let grow = Server_isolation_types.is_grow mode in
  let movable = Server_isolation_types.is_check_movability mode in
  (match max_seeds with
  | Some n when n < 1 ->
    reject (Printf.sprintf "--isolation-max-seeds must be at least 1, got %d" n)
  | _ -> ());
  if min_free_memory_percent < 0 || min_free_memory_percent > 100 then
    reject
      (Printf.sprintf
         "--isolation-min-free-memory is a percentage, from 0 to 100, got %d"
         min_free_memory_percent);
  if max_memory_percent < 0 || max_memory_percent > 100 then
    reject
      (Printf.sprintf
         "--isolation-max-memory is a percentage, from 0 to 100, got %d"
         max_memory_percent);
  (match max_cluster_size with
  | Some n when n < 2 ->
    (* A cap of 1 would retire every cluster before it absorbed anything. *)
    reject
      (Printf.sprintf
         "--isolation-max-cluster-size must be at least 2, got %d"
         n)
  | _ -> ());
  (* One cluster per batch still works, it is just the slowest way to do it:
     the batch exists to pool candidates into a single parallel indexing call,
     and a batch of one leaves the worker pool idle. *)
  if batch_size < 1 then
    reject
      (Printf.sprintf
         "--isolation-batch-size must be at least 1, got %d"
         batch_size);
  if seed_offset < 0 then
    reject
      (Printf.sprintf
         "--isolation-seed-offset cannot be negative, got %d"
         seed_offset);
  (* Growth needs a closure for the seed before it has a cluster, so a reach
     under 1 admits nothing and the run silently reports none. *)
  if max_dependents < 1 then
    reject
      (Printf.sprintf
         "--isolation-max-dependents must be at least 1, got %d"
         max_dependents);
  (* Picking two says the caller means something the run cannot do, and
     preferring one silently answers the wrong question. *)
  let seed_sources =
    [
      ("--isolation-seed-framework", seed_framework);
      ("--isolation-seed-list", seed_list);
      ("--isolation-seed-dir", seed_dir);
    ]
  in
  let seed_sources_given =
    List.filter_map seed_sources ~f:(fun (flag, value) ->
        if Option.is_some value then
          Some flag
        else
          None)
  in
  if List.length seed_sources_given > 1 then
    reject
      (Printf.sprintf
         "%s each name where to start; pass one"
         (String.concat ~sep:" and " seed_sources_given));
  if List.length mode_flags_given > 1 then
    reject
      (Printf.sprintf
         "%s each answer a different question; pass one"
         (String.concat ~sep:" and " mode_flags_given));
  (* A move is of one directory, which a list of files or a framework is not,
     and it is said before the movability rules below because it is the flag
     the caller spelled. *)
  (match mode with
  | Server_isolation_types.Move_to target
    when String.is_empty (String.strip target) ->
    reject "--isolation-move-to needs the directory to move into"
  | Server_isolation_types.Move_to _ when Option.is_none seed_dir ->
    reject
      "--isolation-move-to moves the directory named by --isolation-seed-dir; a seed list or a framework has no directory to move"
  | Server_isolation_types.Move_to _
  | Server_isolation_types.Measure
  | Server_isolation_types.Grow
  | Server_isolation_types.Check_movability ->
    ());
  (* Movability asks about one named set of files. Everything that would
     change which files that is, or what happens to them afterwards, asks a
     different question — and answering a different question from the one the
     caller spelled is worse than refusing. *)
  if movable then begin
    (* The whole-repository scan collects the files nothing references, which
       is already the answer to a different question: asking whether they can
       collectively move would always say yes and mean nothing. *)
    if
      Option.is_none seed_dir
      && Option.is_none seed_list
      && Option.is_none seed_framework
    then
      reject
        "--isolation-movable asks about a named set of files; name it with --isolation-seed-dir, --isolation-seed-list or --isolation-seed-framework";
    (* A window would answer for part of the directory, and a part of a package
       is not a package: the files left out reference in, so the answer would
       be no for a reason the caller created. *)
    if Option.is_some max_seeds || seed_offset > 0 then
      reject
        "--isolation-max-seeds and --isolation-seed-offset take a window of the starting files, and --isolation-movable asks about all of them"
  end;
  (* Growth clamps the reach to the cap, so a larger one is not refused, it is
     silently unused. Only worth saying when the caller picked the reach: the
     default sits above most caps, and naming a flag they never passed reads as
     an accusation rather than a hint. *)
  (match max_cluster_size with
  | Some cap when max_dependents_given && grow && max_dependents > cap ->
    warn
      (Printf.sprintf
         "--isolation-max-dependents %d is clamped to the cluster size cap of %d for growth; a closure larger than the cluster can never be absorbed"
         max_dependents
         cap)
  | _ -> ());
  (* Legal, and almost always a mistake: it skips n and then runs to the end of
     the population, which for a large family is tens of thousands. *)
  if seed_offset > 0 && Option.is_none max_seeds then
    warn
      (Printf.sprintf
         "--isolation-seed-offset %d without --isolation-max-seeds runs to the end of the seed list, not a window of it"
         seed_offset)

let validate ~max_dependents_given ~mode_flags_given options =
  match check ~max_dependents_given ~mode_flags_given options with
  | () -> Ok ()
  | exception Invalid message -> Error message
