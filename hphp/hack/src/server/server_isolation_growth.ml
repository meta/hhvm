(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude
module Memory = Server_isolation_memory

(** What a batch of growth spent and where. Indexing and absorption have
    entirely different fixes, and one elapsed figure cannot tell them apart. *)
module Stats = struct
  type t = {
    index_seconds: float;
    absorb_seconds: float;
    candidates_tested: int;
    candidates_skipped: int;
        (** Candidates whose blocker has not moved, so not retested this
            round. *)
    absorbed_by_subset: int;
        (** Taken because everything referencing them was already inside. *)
    absorbed_by_closure: int;
        (** Taken along with everything referencing them — growth getting past a
            narrowly-shared file, which the cheap rule cannot do. *)
  }

  let zero =
    {
      index_seconds = 0.0;
      absorb_seconds = 0.0;
      candidates_tested = 0;
      candidates_skipped = 0;
      absorbed_by_subset = 0;
      absorbed_by_closure = 0;
    }

  let current = ref zero

  let get () = !current

  let reset () = current := zero

  (** Runs [f] and hands its duration to [add]. *)
  let timed add f =
    let t0 = Unix.gettimeofday () in
    let result = f () in
    add (Unix.gettimeofday () -. t0);
    result

  let add_index_seconds s =
    current := { !current with index_seconds = !current.index_seconds +. s }

  let add_absorb_seconds s =
    current := { !current with absorb_seconds = !current.absorb_seconds +. s }

  let record_candidate_tested () =
    current :=
      { !current with candidates_tested = !current.candidates_tested + 1 }

  let add_candidates_skipped n =
    current :=
      { !current with candidates_skipped = !current.candidates_skipped + n }

  let record_absorbed_by_subset () =
    current :=
      { !current with absorbed_by_subset = !current.absorbed_by_subset + 1 }

  let record_absorbed_by_closure () =
    current :=
      { !current with absorbed_by_closure = !current.absorbed_by_closure + 1 }
end

(** What depends on a candidate, minus what it defines itself. Asked once per
    candidate rather than once per cluster reaching it, and memoised: the walk
    is expensive and the files that recur on frontiers recur everywhere. *)
module Dependents = struct
  type t =
    | Outside of Typing_deps.DepSet.t
    | Too_many
        (** Past [cap], and not stored. This does not refuse the candidate: it
            falls through to the closure rule, which may still take it. *)

  (* Bounds both the walk and what the memo holds, so one candidate cannot keep
     thousands of hashes alive for the rest of the batch. Not measured — a round
     number above the clusters a run produces. Too low costs only the cheap
     rule, and with it the witness that would have let the candidate be skipped
     next round. *)
  let cap = 4096

  let cache : t Relative_path.Map.t ref = ref Relative_path.Map.empty

  (** [outside deps_mode candidate own own_set], where [own] is what the
      candidate defines and [own_set] the same as a set. *)
  let outside deps_mode candidate own own_set =
    match Relative_path.Map.find_opt !cache candidate with
    | Some cached -> cached
    | None ->
      (* [DepSet.diff] below drops the candidate's own symbols from each answer,
         at most [cardinal own_set] of them, so ask the graph for [cap] plus
         that many. A truncated answer then still exceeds [cap] after the
         drop. *)
      let graph_cap = cap + Typing_deps.DepSet.cardinal own_set in
      let result =
        List.fold_until
          own
          ~init:(Typing_deps.DepSet.make ())
          ~f:(fun acc dep ->
            let acc =
              Typing_deps.DepSet.union
                acc
                (Typing_deps.DepSet.diff
                   (Typing_deps.get_ideps_from_hash_bounded
                      deps_mode
                      dep
                      ~max:graph_cap)
                   own_set)
            in
            if Typing_deps.DepSet.cardinal acc > cap then
              Container.Continue_or_stop.Stop Too_many
            else
              Container.Continue_or_stop.Continue acc)
          ~finish:(fun acc -> Outside acc)
      in
      cache := Relative_path.Map.add !cache ~key:candidate ~data:result;
      result

  (** The first dependent lying outside the cluster. Callers keep it as a
      witness: while it stays outside, the candidate cannot have become
      absorbable. *)
  let first_outside cluster_deps dependents =
    Typing_deps.DepSet.fold_result dependents ~init:() ~f:(fun dependent () ->
        if Typing_deps.DepSet.mem cluster_deps dependent then
          Ok ()
        else
          Error dependent)
    |> function
    | Ok () -> None
    | Error dependent -> Some dependent

  let cached_count () = Relative_path.Map.cardinal !cache

  let release () = cache := Relative_path.Map.empty
end

(** Whether one candidate can join one cluster, and what comes with it.
    Two rules, cheapest first; the expensive one runs only on what the cheap
    one turns down. *)
module Absorb = struct
  type absorption =
    | Absorbs of Relative_path.Set.t
    | Blocked_by of Typing_deps.Dep.t
    | Blocked
    | No_room

  (** Room left, or [None] when unbounded. Closure absorption is all-or-nothing, so
      checking the cap afterwards would not bound anything. *)
  let fits room n =
    match room with
    | None -> true
    | Some room -> n <= room

  type batch = {
    ctx: Provider_context.t;
    workers: Multi_worker.worker list option;
    deps_mode: Typing_deps.Mode.t;
    naming_table: Naming_table.t;
    claimed: Relative_path.Set.t;
    max_cluster_size: int option;
    max_dependents: int;
  }

  (* Where the subset rule stands: every dependent already inside, one named
     dependent outside, or too many dependents to have looked at all. *)
  type blocker =
    | Inside
    | Witness of Typing_deps.Dep.t
    | No_witness

  let subset_rule b cluster_deps candidate own own_set =
    match Dependents.outside b.deps_mode candidate own own_set with
    | Dependents.Too_many -> No_witness
    | Dependents.Outside outside ->
      (match Dependents.first_outside cluster_deps outside with
      | None -> Inside
      | Some dependent -> Witness dependent)

  (* [refused] is what the subset rule already concluded: both rules are monotone,
     so a candidate the closure rule also turns down stays blocked by the dependent
     the subset rule named. *)
  let closure_rule b ~held ~room ~refused candidate =
    match
      Server_isolation_inbound.get
        b.ctx
        b.naming_table
        ~max:b.max_dependents
        candidate
    with
    | Some closure
      when Relative_path.Set.is_empty
             (Relative_path.Set.inter closure b.claimed) ->
      (* Only files not already held count against the cap. A candidate is on the
         frontier because a held file references it, so that file depends on it and
         lies in its closure: measuring the whole closure would refuse ones that
         fit. *)
      let joining =
        match room with
        | None -> 0
        | Some _ ->
          Relative_path.Set.cardinal (Relative_path.Set.diff closure held)
      in
      if fits room joining then begin
        Stats.record_absorbed_by_closure ();
        Absorbs closure
      end else
        No_room
    | Some _
    | None ->
      refused

  let absorbable b ~held ~room cluster_deps candidate =
    match Naming_table.get_file_info b.naming_table candidate with
    | None -> Blocked
    | _ when not (fits room 1) -> No_room
    | Some file_info ->
      let own = Typing_deps.deps_of_file_info file_info in
      let own_set = Typing_deps.DepSet.of_list own in
      let closure_rule = closure_rule b ~held ~room in
      (match subset_rule b cluster_deps candidate own own_set with
      | Inside ->
        Stats.record_absorbed_by_subset ();
        Absorbs (Relative_path.Set.singleton candidate)
      | Witness dependent ->
        closure_rule ~refused:(Blocked_by dependent) candidate
      | No_witness -> closure_rule ~refused:Blocked candidate)
end

open Absorb

(* Every file [files] references, added to [init]: a cluster accumulates onto
   what it already reached, a new one starts from nothing. *)
let reached_from b ~init files =
  Relative_path.Set.fold files ~init ~f:(fun path acc ->
      Relative_path.Set.union acc (Server_isolation_outbound.get b.ctx path))

let deps_of_files naming_table files =
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

(* A cluster the rounds are finished with: what it holds, and whether the cap
   stopped it short of closing. This is what a batch hands back. *)
type grown = {
  seed: Relative_path.t;
  files: Relative_path.Set.t;
  truncated: bool;
}

(* The same cluster while the rounds are still running. It carries four things
   [grown] does not, because each round needs them and recomputing any of them
   per round would cost more than holding it. *)
type growing = {
  seed: Relative_path.t;
  cluster: Relative_path.Set.t;
  cluster_deps: Typing_deps.DepSet.t;
      (** Dep hashes the cluster's files define, so absorption tests set
          membership rather than a naming-heap lookup. *)
  blocked: Typing_deps.Dep.t Relative_path.Map.t;
      (** Candidate -> the dependent that kept it out. The frontier is rebuilt
          each round, so a refused candidate is offered again. *)
  reached: Relative_path.Set.t;
      (** Everything the cluster references, extended with new members' edges
          rather than rebuilt, which would be quadratic in cluster size. *)
  truncated: bool;
      (** Whether the cap refused this cluster a candidate it would have
          taken. *)
}

let index b paths =
  Stats.timed Stats.add_index_seconds (fun () ->
      Server_isolation_outbound.ensure_indexed
        ~ctx:b.ctx
        ~workers:b.workers
        (Relative_path.Set.elements paths))

(* Everything the cluster reaches and does not hold, less the candidates whose
   blocker has not moved. *)
let testable_frontier g =
  let frontier = Relative_path.Set.diff g.reached g.cluster in
  let to_test =
    Relative_path.Set.filter frontier ~f:(fun candidate ->
        match Relative_path.Map.find_opt g.blocked candidate with
        | None -> true
        | Some witness -> Typing_deps.DepSet.mem g.cluster_deps witness)
  in
  Stats.add_candidates_skipped
    (Relative_path.Set.cardinal frontier - Relative_path.Set.cardinal to_test);
  (frontier, to_test)

(* One candidate against one cluster, threading what the round holds so far. *)
let offer b cluster_deps candidate (held, blocked, truncated) =
  if Relative_path.Set.mem b.claimed candidate then
    (held, blocked, truncated)
  else if Relative_path.Set.mem held candidate then
    (* Arrived earlier this round in another closure. *)
    (held, blocked, truncated)
  else begin
    Stats.record_candidate_tested ();
    (* Shrinks as the round absorbs, so a round cannot take several candidates
       that each fit alone. *)
    let room =
      Option.map b.max_cluster_size ~f:(fun cap ->
          cap - Relative_path.Set.cardinal held)
    in
    match absorbable b ~held ~room cluster_deps candidate with
    | Absorbs files -> (Relative_path.Set.union held files, blocked, truncated)
    | Blocked_by witness ->
      ( held,
        Relative_path.Map.add blocked ~key:candidate ~data:witness,
        truncated )
    | Blocked -> (held, blocked, truncated)
    | No_room -> (held, blocked, true)
  end

(* The cluster with its blockers updated, and the files it gained — disjoint
   from what it already held, so callers may size the two by adding them. *)
let absorb_round b g to_test =
  let (held, blocked, truncated) =
    Relative_path.Set.fold
      to_test
      ~init:(g.cluster, g.blocked, false)
      ~f:(offer b g.cluster_deps)
  in
  ( { g with blocked; truncated = g.truncated || truncated },
    Relative_path.Set.diff held g.cluster )

(* Retire the cluster, or carry it into the next round with what it gained. *)
let advance b g absorbed : (growing, grown) Either.t =
  let retire files = Second { seed = g.seed; files; truncated = g.truncated } in
  if Relative_path.Set.is_empty absorbed then
    retire g.cluster
  else
    let cluster = Relative_path.Set.union g.cluster absorbed in
    let at_cap =
      match b.max_cluster_size with
      | Some cap -> Relative_path.Set.cardinal cluster >= cap
      | None -> false
    in
    if at_cap then
      (* Absorption already did the bounding; this only retires. *)
      retire cluster
    else
      let cluster_deps =
        Typing_deps.DepSet.union
          g.cluster_deps
          (deps_of_files b.naming_table absorbed)
      in
      let reached = reached_from b ~init:g.reached absorbed in
      First { g with cluster; cluster_deps; reached }

let log_round round ~clusters ~frontier_size ~entering =
  let now = Stats.get () in
  Hh_logger.log
    "[isolation] round %d: %d clusters, %d frontier files | index %.1fs absorb %.1fs | tested %d skipped %d closed %d | deps %d | heap %.1fGiB rss %.1fGiB"
    round
    clusters
    frontier_size
    (now.Stats.index_seconds -. entering.Stats.index_seconds)
    (now.Stats.absorb_seconds -. entering.Stats.absorb_seconds)
    (now.Stats.candidates_tested - entering.Stats.candidates_tested)
    (now.Stats.candidates_skipped - entering.Stats.candidates_skipped)
    (now.Stats.absorbed_by_closure - entering.Stats.absorbed_by_closure)
    (Dependents.cached_count ())
    (Memory.heap_gib ())
    (Memory.rss_gib ())

(* One absorption round over the whole batch, returning the clusters that are
   still growing and those that finished. *)
let run_round b round active =
  (* A round's own figures are the difference against this. *)
  let entering = Stats.get () in
  let with_frontier = List.map active ~f:(fun g -> (g, testable_frontier g)) in
  let frontier_size =
    List.fold
      with_frontier
      ~init:Relative_path.Set.empty
      ~f:(fun acc (_, (frontier, _)) -> Relative_path.Set.union acc frontier)
    |> Relative_path.Set.cardinal
  in
  (* Absorption first, then index what it took. [absorbable] never reads an
     outbound edge; edges are only needed to extend [reached] for files that
     joined, so indexing the frontier would parse everything a cluster can see
     to use the edges of the few that join. *)
  let absorption =
    Stats.timed Stats.add_absorb_seconds (fun () ->
        List.map with_frontier ~f:(fun (g, (_, to_test)) ->
            absorb_round b g to_test))
  in
  index
    b
    (List.fold absorption ~init:Relative_path.Set.empty ~f:(fun acc (_, a) ->
         Relative_path.Set.union acc a));
  let split =
    List.partition_map absorption ~f:(fun (g, absorbed) -> advance b g absorbed)
  in
  (* Accepted closures are spent, and cheap to recompute; refusals are kept. *)
  Server_isolation_inbound.release_closures ();
  log_round round ~clusters:(List.length with_frontier) ~frontier_size ~entering;
  (* The duplication this removes is within a round: most of a batch reaches the
     same widely-used files at the same time. *)
  Dependents.release ();
  (* Releasing marks the memory free without handing it back: the dependency
     sets live behind a Rust custom block the collector barely notices. *)
  Gc.compact ();
  split

let open_cluster b seed cluster =
  {
    seed;
    cluster;
    cluster_deps = deps_of_files b.naming_table cluster;
    blocked = Relative_path.Map.empty;
    truncated = false;
    reached = reached_from b ~init:Relative_path.Set.empty cluster;
  }

(* A cluster starts as the seed's closure, not the seed alone. Growth only looks
   outbound, so it can never reach a file that references the seed without the
   seed referencing back, and the cluster would ship with that file pointing
   into it. A seed whose closure does not fit, or reaches a file another cluster
   holds, is dropped for the same reasons a candidate is. *)
let seed_clusters b seeds =
  let holds_nothing_claimed closure =
    Relative_path.Set.is_empty (Relative_path.Set.inter closure b.claimed)
  in
  List.filter_map seeds ~f:(fun seed ->
      let closure =
        Server_isolation_inbound.get
          b.ctx
          b.naming_table
          ~max:b.max_dependents
          seed
      in
      match closure with
      | Some closure when holds_nothing_claimed closure -> Some (seed, closure)
      | _ -> None)

(* Threaded through every step of the batch, so it is assembled once. *)
let batch
    ctx
    workers
    deps_mode
    naming_table
    ~claimed
    ~max_cluster_size
    ~max_dependents =
  {
    ctx;
    workers;
    deps_mode;
    naming_table;
    claimed;
    max_cluster_size;
    max_dependents;
  }

let grow_batch
    ctx
    workers
    deps_mode
    naming_table
    ~(claimed : Relative_path.Set.t)
    ~(max_cluster_size : int option)
    ~(max_dependents : int)
    ~(check_memory : unit -> unit)
    (seeds : Relative_path.t list) : grown list =
  let b =
    batch
      ctx
      workers
      deps_mode
      naming_table
      ~claimed
      ~max_cluster_size
      ~max_dependents
  in
  let rec rounds round active finished =
    match active with
    | [] -> finished
    | _ ->
      check_memory ();
      let (still_active, done_now) = run_round b round active in
      rounds (round + 1) still_active (done_now @ finished)
  in
  let start = seed_clusters b seeds in
  let seeded_files =
    List.fold start ~init:Relative_path.Set.empty ~f:(fun acc (_, cluster) ->
        Relative_path.Set.union acc cluster)
  in
  let clusters =
    List.map start ~f:(fun (seed, cluster) -> open_cluster b seed cluster)
  in
  (* Every closure must be indexed, since [reached] comes from the outbound edges
     of every member. A file is in its own closure, so indexing the seeds first
     would only have parsed the refused ones. *)
  index b seeded_files;
  rounds 1 clusters []

(** What this batch spent and decided. *)
let stats = Stats.get

(** Drop what growth filled this round. The accepted closures are cheap to
    recompute; the refusals are kept, since each cost a full walk and the files
    that earn them recur everywhere. *)
let release_caches () =
  Server_isolation_inbound.release_closures ();
  Dependents.release ()

let reset_stats = Stats.reset
