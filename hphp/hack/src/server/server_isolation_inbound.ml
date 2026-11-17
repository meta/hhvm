(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude

module PathKey = struct
  type t = Relative_path.t

  let to_string = Relative_path.to_absolute

  let compare = Relative_path.compare
end

module ClosureValue = struct
  type t = Relative_path.Set.t

  let description = "Server_isolation_inbound"
end

module ClosureHeap =
  Shared_mem.Heap
    (Shared_mem.ImmediateBackend (Shared_mem.NonEvictable)) (PathKey)
    (ClosureValue)

(* Budget for the walk, in dep hashes rather than the files the caller bounds.
   Firing early can refuse a closure that would have fitted, never return an
   incomplete one. The floor matters because growth clamps [max] to the cluster
   cap, which alone would put the budget below closures that do fit.

   Neither figure is measured. Twenty hashes per permitted file is a guess at
   what a Hack file defines, and the floor is a round number above the closures
   a run actually returns. Both err towards refusing, so setting either too low
   costs recall — a lower isolatability figure — and never a wrong closure. *)
let deps_per_file_allowance = 20

let min_deps_allowance = 5000

(* A refusal costs a full walk to recompute and one path to hold, so refusals
   outlive a batch; accepted closures are the reverse. *)
let refused : Relative_path.t HashSet.t = HashSet.create ()

(* The heap is keyed by a hash of the path and cannot be enumerated. *)
let cached : Relative_path.t HashSet.t = HashSet.create ()

let computed_count = ref 0

let served_count = ref 0

(* [add_all_deps] chains these two but cannot be used here: it returns its input,
   re-expanding the whole set each iteration, and it runs the recursive extends
   walk first, which completes inside the graph before the bound can refuse it.
   [None] means the walk passed [max_deps], not that there is no closure. *)
let rec grow_to_fixpoint deps_mode ~max_deps ~acc frontier =
  let over s = Typing_deps.DepSet.cardinal s > max_deps in
  (* A truncated result is strictly larger than [max_deps], so it fails [over]
     exactly as the complete one would. *)
  let typed =
    Typing_deps.DepSet.union
      acc
      (Typing_deps.add_typing_deps_bounded deps_mode frontier ~max:max_deps)
  in
  if over typed then
    None
  else
    let next =
      Typing_deps.DepSet.union
        typed
        (Typing_deps.add_extend_deps deps_mode frontier)
    in
    if over next then
      None
    else
      let frontier = Typing_deps.DepSet.diff next acc in
      if Typing_deps.DepSet.is_empty frontier then
        Some acc
      else
        grow_to_fixpoint deps_mode ~max_deps ~acc:next frontier

(* Every top-level symbol defined by any of [files]. *)
let symbols_of_files naming_table files =
  Relative_path.Set.fold
    files
    ~init:(Typing_deps.DepSet.make ())
    ~f:(fun path acc ->
      match Naming_table.get_file_info naming_table path with
      | None -> acc
      | Some file_info ->
        Typing_deps.DepSet.union
          acc
          (Typing_deps.DepSet.of_list (Typing_deps.deps_of_file_info file_info)))

let compute ctx naming_table ~max path =
  match Naming_table.get_file_info naming_table path with
  | None -> None
  | Some file_info ->
    let deps_mode = Provider_context.get_deps_mode ctx in
    let own =
      Typing_deps.deps_of_file_info file_info |> Typing_deps.DepSet.of_list
    in
    let max_deps = Int.max min_deps_allowance (max * deps_per_file_allowance) in
    (* Upward-closed in hash space, but the caller absorbs whole files: a
       resolved file brings symbols the walk never reached, and whatever depends
       on those is missing. So resolved symbols feed back until a round resolves
       no new file, which terminates because [max_deps] bounds the growth. *)
    let rec close acc frontier =
      match grow_to_fixpoint deps_mode ~max_deps ~acc frontier with
      | None -> None
      | Some hashes ->
        let files = Naming_provider.get_files ctx hashes in
        if Relative_path.Set.cardinal files > max then
          None
        else
          let symbols = symbols_of_files naming_table files in
          let frontier = Typing_deps.DepSet.diff symbols hashes in
          if Typing_deps.DepSet.is_empty frontier then
            Some files
          else
            close (Typing_deps.DepSet.union hashes symbols) frontier
    in
    close own own

let get ctx naming_table ~max path =
  if HashSet.mem refused path then begin
    incr served_count;
    None
  end else
    match ClosureHeap.get path with
    | Some closure ->
      incr served_count;
      Some closure
    | None ->
      incr computed_count;
      (match compute ctx naming_table ~max path with
      | None ->
        HashSet.add refused path;
        None
      | Some files ->
        ClosureHeap.add path files;
        HashSet.add cached path;
        Some files)

let closures_computed () = !computed_count

let closures_served () = !served_count

let refusals_held () = HashSet.length refused

let release_closures () =
  if not (HashSet.is_empty cached) then begin
    ClosureHeap.remove_batch
      (HashSet.fold cached ~init:ClosureHeap.KeySet.empty ~f:(fun path acc ->
           ClosureHeap.KeySet.add path acc));
    HashSet.clear cached
  end

let reset () =
  release_closures ();
  HashSet.clear refused;
  computed_count := 0;
  served_count := 0

(* The files outside [candidate] that reference a symbol defined in [files]. *)
let dependents_outside ctx naming_table ~candidate files =
  let deps_mode = Provider_context.get_deps_mode ctx in
  let own = symbols_of_files naming_table files in
  let dependents = Typing_deps.add_typing_deps deps_mode own in
  Relative_path.Set.diff (Naming_provider.get_files ctx dependents) candidate

let referrers ctx naming_table candidate =
  dependents_outside ctx naming_table ~candidate candidate

let referrers_by_target ctx naming_table candidate =
  Relative_path.Set.fold
    candidate
    ~init:Relative_path.Map.empty
    ~f:(fun target acc ->
      dependents_outside
        ctx
        naming_table
        ~candidate
        (Relative_path.Set.singleton target)
      |> Relative_path.Set.fold ~init:acc ~f:(fun referrer acc ->
             let targets =
               Option.value
                 (Relative_path.Map.find_opt acc referrer)
                 ~default:Relative_path.Set.empty
             in
             Relative_path.Map.add
               acc
               ~key:referrer
               ~data:(Relative_path.Set.add targets target)))
