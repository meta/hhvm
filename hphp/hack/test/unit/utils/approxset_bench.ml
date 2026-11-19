(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

(** Benchmark comparing ApproxSet vs BddSet construction and query performance
    using workloads that model real production usage in the Hack type checker.

    Run with:
      buck run @fbcode//mode/dev-nosan-lg fbcode//hphp/hack/test/unit/utils:approxset_bench
*)

open Hh_prelude
open Approxset_tag_domain

(* ------------------------------------------------------------------ *)
(* Timing infrastructure *)
(* ------------------------------------------------------------------ *)

module Bench = struct
  type measurement = {
    name: string;
    construct_ns: float;
    query_ns: float;
    total_ns: float;
    trials: int;
  }

  let time f =
    let t0 = Unix.gettimeofday () in
    let result = f () in
    let t1 = Unix.gettimeofday () in
    ((t1 -. t0) *. 1e9, result)

  let fmt_ms ns = Printf.sprintf "%8.1f ms" (ns /. 1e6)

  let print_header () =
    Printf.printf
      "%-35s | %-12s | %-12s | %-12s | %s\n"
      "Scenario"
      "Construct"
      "Query"
      "Total"
      "Trials";
    Printf.printf "%s\n" (String.make 90 '-')

  let print_row m =
    Printf.printf
      "%-35s | %-12s | %-12s | %-12s | %d\n"
      m.name
      (fmt_ms m.construct_ns)
      (fmt_ms m.query_ns)
      (fmt_ms m.total_ns)
      m.trials

  let print_comparison_header () =
    Printf.printf
      "\n%-35s | %-12s | %-12s | %s\n"
      "Scenario"
      "ApproxSet"
      "BddSet"
      "Ratio (B/A)";
    Printf.printf "%s\n" (String.make 75 '-')

  let print_comparison_row name approx_ns bdd_ns =
    let ratio_str =
      if Float.( > ) approx_ns 0.0 then
        Printf.sprintf "%.2fx" (bdd_ns /. approx_ns)
      else
        "n/a"
    in
    Printf.printf
      "%-35s | %-12s | %-12s | %s\n"
      name
      (fmt_ms approx_ns)
      (fmt_ms bdd_ns)
      ratio_str
end

(* ------------------------------------------------------------------ *)
(* Module type for benchmarkable implementations *)
(* ------------------------------------------------------------------ *)

module type ImplForBench = sig
  include Approx_set_intf.S with module Domain := TagDomain

  val name : string
end

(* ------------------------------------------------------------------ *)
(* Pre-generated input types (shared across all implementations) *)
(* ------------------------------------------------------------------ *)

type refinement_input = {
  r_h: hierarchy;
  r_class_id: int;
  r_pred_tag: tag;
}

type variant_filter_input = {
  vf_h: hierarchy;
  vf_variant_tags: tag list list;
  vf_intersecting_tags: tag list;
}

type overlap_input = {
  ov_h: hierarchy;
  ov_variant_cids: int list;
}

type complex_input = {
  cx_h: hierarchy;
  cx_tags1: tag list;
  cx_tags2: tag list;
  cx_ops1: int list;
  cx_ops2: int list;
}

type construction_input = {
  co_h: hierarchy;
  co_tags: tag list;
  co_ops: int list;
}

(* ------------------------------------------------------------------ *)
(* Input generators (deterministic, independent of implementation) *)
(* ------------------------------------------------------------------ *)

let all_hierarchies = Array.append hierarchy_pool featured_hierarchies

let pick_hierarchy random =
  let h_idx =
    Quickcheck.Generator.generate
      (Int.gen_incl 0 (Array.length all_hierarchies - 1))
      ~size:10
      ~random
  in
  all_hierarchies.(h_idx)

let gen_refinement_inputs ~n : refinement_input array =
  let random = random_of_seed 12345 in
  Array.init n ~f:(fun _ ->
      let h = pick_hierarchy random in
      let class_id =
        Quickcheck.Generator.generate
          (Int.gen_incl 0 (h.num_classes - 1))
          ~size:10
          ~random
      in
      let pred_tag =
        Quickcheck.Generator.generate (gen_tag h) ~size:10 ~random
      in
      { r_h = h; r_class_id = class_id; r_pred_tag = pred_tag })

let gen_variant_filter_inputs ~n : variant_filter_input array =
  let random = random_of_seed 23456 in
  Array.init n ~f:(fun _ ->
      let h = pick_hierarchy random in
      let num_variants =
        Quickcheck.Generator.generate (Int.gen_incl 3 10) ~size:10 ~random
      in
      let variant_tags =
        List.init num_variants ~f:(fun _ ->
            let n_tags =
              Quickcheck.Generator.generate (Int.gen_incl 1 3) ~size:10 ~random
            in
            List.init n_tags ~f:(fun _ ->
                Quickcheck.Generator.generate (gen_tag h) ~size:10 ~random))
      in
      let n_intersecting =
        Quickcheck.Generator.generate (Int.gen_incl 1 3) ~size:10 ~random
      in
      let intersecting_tags =
        List.init n_intersecting ~f:(fun _ ->
            Quickcheck.Generator.generate (gen_tag h) ~size:10 ~random)
      in
      {
        vf_h = h;
        vf_variant_tags = variant_tags;
        vf_intersecting_tags = intersecting_tags;
      })

let gen_overlap_inputs ~n : overlap_input array =
  let random = random_of_seed 34567 in
  Array.init n ~f:(fun _ ->
      let h = pick_hierarchy random in
      let num_variants =
        Quickcheck.Generator.generate (Int.gen_incl 4 8) ~size:10 ~random
      in
      let variant_cids =
        List.init num_variants ~f:(fun _ ->
            Quickcheck.Generator.generate
              (Int.gen_incl 0 (h.num_classes - 1))
              ~size:10
              ~random)
      in
      { ov_h = h; ov_variant_cids = variant_cids })

let gen_complex_inputs ~n : complex_input array =
  let random = random_of_seed 45678 in
  Array.init n ~f:(fun _ ->
      let h = pick_hierarchy random in
      let n_tags1 =
        Quickcheck.Generator.generate (Int.gen_incl 2 5) ~size:10 ~random
      in
      let n_tags2 =
        Quickcheck.Generator.generate (Int.gen_incl 2 5) ~size:10 ~random
      in
      let tags1 =
        List.init n_tags1 ~f:(fun _ ->
            Quickcheck.Generator.generate (gen_tag h) ~size:10 ~random)
      in
      let tags2 =
        List.init n_tags2 ~f:(fun _ ->
            Quickcheck.Generator.generate (gen_tag h) ~size:10 ~random)
      in
      let n_ops1 =
        Quickcheck.Generator.generate (Int.gen_incl 1 3) ~size:10 ~random
      in
      let n_ops2 =
        Quickcheck.Generator.generate (Int.gen_incl 1 3) ~size:10 ~random
      in
      let ops1 =
        List.init n_ops1 ~f:(fun _ ->
            Quickcheck.Generator.generate (Int.gen_incl 0 2) ~size:10 ~random)
      in
      let ops2 =
        List.init n_ops2 ~f:(fun _ ->
            Quickcheck.Generator.generate (Int.gen_incl 0 2) ~size:10 ~random)
      in
      {
        cx_h = h;
        cx_tags1 = tags1;
        cx_tags2 = tags2;
        cx_ops1 = ops1;
        cx_ops2 = ops2;
      })

let gen_construction_inputs ~n : construction_input array =
  let random = random_of_seed 56789 in
  Array.init n ~f:(fun _ ->
      let h = pick_hierarchy random in
      let n_tags =
        Quickcheck.Generator.generate (Int.gen_incl 2 6) ~size:10 ~random
      in
      let tags =
        List.init n_tags ~f:(fun _ ->
            Quickcheck.Generator.generate (gen_tag h) ~size:10 ~random)
      in
      let ops =
        List.init (n_tags - 1) ~f:(fun _ ->
            Quickcheck.Generator.generate (Int.gen_incl 0 2) ~size:10 ~random)
      in
      { co_h = h; co_tags = tags; co_ops = ops })

(* ------------------------------------------------------------------ *)
(* Benchmark scenarios, parameterized by implementation *)
(* ------------------------------------------------------------------ *)

module BenchScenario (Impl : ImplForBench) = struct
  (* ---- to_datatypes without ASet ground-truth tracking ---- *)
  let rec to_datatypes_raw (h : hierarchy) (cid : int) : Impl.t =
    let kind =
      if h.is_interface cid then
        Interface
      else if h.is_final cid then
        Final
      else
        NonFinal
    in
    let ancestor_count = Set.length (h.ancestors cid) in
    let base = Impl.singleton (Instance { id = cid; kind; ancestor_count }) in
    match kind with
    | Final -> base
    | NonFinal ->
      (match h.sealed_whitelist cid with
      | None -> base
      | Some whitelist ->
        let wl_union =
          List.fold whitelist ~init:Impl.empty ~f:(fun acc wid ->
              Impl.union acc (to_datatypes_raw h wid))
        in
        Impl.inter wl_union base)
    | Interface ->
      let with_sealed =
        match h.sealed_whitelist cid with
        | Some whitelist ->
          let wl_union =
            List.fold whitelist ~init:Impl.empty ~f:(fun acc wid ->
                Impl.union acc (to_datatypes_raw h wid))
          in
          Impl.inter wl_union base
        | None ->
          (match h.require_extends cid with
          | [] -> base
          | reqs ->
            List.fold reqs ~init:base ~f:(fun acc req_id ->
                Impl.inter acc (to_datatypes_raw h req_id)))
      in
      let special = h.special_interface_tags cid in
      List.fold special ~init:with_sealed ~f:(fun acc t ->
          Impl.union acc (Impl.singleton t))

  let build_complex_set (tags : tag list) (ops : int list) : Impl.t =
    let mixed = Impl.singleton Mixed in
    match tags with
    | [] -> Impl.empty
    | first :: rest ->
      let base = Impl.singleton first in
      let (result, _) =
        List.fold rest ~init:(base, ops) ~f:(fun (acc, remaining_ops) t ->
            let s = Impl.singleton t in
            match remaining_ops with
            | 0 :: ops_tl -> (Impl.union acc s, ops_tl)
            | 1 :: ops_tl -> (Impl.inter acc s, ops_tl)
            | 2 :: ops_tl -> (Impl.diff acc s, ops_tl)
            | _ -> (Impl.union acc s, []))
      in
      let remaining_ops = List.drop ops (List.length tags - 1) in
      (match remaining_ops with
      | 2 :: _ -> Impl.diff mixed result
      | _ -> result)

  (* ---- Scenario 1: Type Refinement ---- *)

  let bench_refinement (inputs : refinement_input array) : Bench.measurement =
    let (construct_ns, sets) =
      Bench.time (fun () ->
          Array.map inputs ~f:(fun { r_h; r_class_id; r_pred_tag } ->
              let ty = to_datatypes_raw r_h r_class_id in
              let pred = Impl.singleton r_pred_tag in
              let mixed = Impl.singleton Mixed in
              let complement = Impl.diff mixed pred in
              (r_h, ty, pred, complement)))
    in
    let (query_ns, _) =
      Bench.time (fun () ->
          Array.iter sets ~f:(fun (h, ty, pred, complement) ->
              ignore (Impl.are_disjoint h ty pred : bool);
              ignore (Impl.are_disjoint h ty complement : bool)))
    in
    {
      Bench.name = Impl.name ^ " / Refinement";
      construct_ns;
      query_ns;
      total_ns = construct_ns +. query_ns;
      trials = Array.length inputs;
    }

  (* ---- Scenario 2: Case Type Variant Filtering ---- *)

  let bench_variant_filter (inputs : variant_filter_input array) :
      Bench.measurement =
    let (construct_ns, sets) =
      Bench.time (fun () ->
          Array.map
            inputs
            ~f:(fun { vf_h = _; vf_variant_tags; vf_intersecting_tags } ->
              let variants =
                List.map vf_variant_tags ~f:(fun tags ->
                    List.fold tags ~init:Impl.empty ~f:(fun acc t ->
                        Impl.union acc (Impl.singleton t)))
              in
              let intersecting =
                List.fold vf_intersecting_tags ~init:Impl.empty ~f:(fun acc t ->
                    Impl.union acc (Impl.singleton t))
              in
              (variants, intersecting)))
    in
    let (query_ns, _) =
      Bench.time (fun () ->
          Array.iteri sets ~f:(fun i (variants, intersecting) ->
              let h = inputs.(i).vf_h in
              List.iter variants ~f:(fun variant ->
                  ignore (Impl.are_disjoint h variant intersecting : bool))))
    in
    {
      Bench.name = Impl.name ^ " / Variant filter";
      construct_ns;
      query_ns;
      total_ns = construct_ns +. query_ns;
      trials = Array.length inputs;
    }

  (* ---- Scenario 3: Case Type Overlap Validation ---- *)

  let bench_overlap_validation (inputs : overlap_input array) :
      Bench.measurement =
    let (construct_ns, sets) =
      Bench.time (fun () ->
          Array.map inputs ~f:(fun { ov_h; ov_variant_cids } ->
              let variants =
                List.map ov_variant_cids ~f:(fun cid ->
                    to_datatypes_raw ov_h cid)
              in
              (ov_h, variants)))
    in
    let (query_ns, _) =
      Bench.time (fun () ->
          Array.iter sets ~f:(fun (h, variants) ->
              let arr = Array.of_list variants in
              let len = Array.length arr in
              for i = 0 to len - 2 do
                for j = i + 1 to len - 1 do
                  ignore (Impl.are_disjoint h arr.(i) arr.(j) : bool)
                done
              done))
    in
    {
      Bench.name = Impl.name ^ " / Overlap validation";
      construct_ns;
      query_ns;
      total_ns = construct_ns +. query_ns;
      trials = Array.length inputs;
    }

  (* ---- Scenario 4: Complex Subtype Checking ---- *)

  let bench_complex_subtype (inputs : complex_input array) : Bench.measurement =
    let (construct_ns, sets) =
      Bench.time (fun () ->
          Array.map
            inputs
            ~f:(fun { cx_h = _; cx_tags1; cx_tags2; cx_ops1; cx_ops2 } ->
              let s1 = build_complex_set cx_tags1 cx_ops1 in
              let s2 = build_complex_set cx_tags2 cx_ops2 in
              (s1, s2)))
    in
    let (query_ns, _) =
      Bench.time (fun () ->
          Array.iteri sets ~f:(fun i (s1, s2) ->
              let h = inputs.(i).cx_h in
              ignore (Impl.are_disjoint h s1 s2 : bool);
              ignore (Impl.is_subset h s1 s2 : bool)))
    in
    {
      Bench.name = Impl.name ^ " / Complex subtype";
      construct_ns;
      query_ns;
      total_ns = construct_ns +. query_ns;
      trials = Array.length inputs;
    }

  (* ---- Scenario 5: Construction Only ---- *)

  let bench_construction (inputs : construction_input array) : Bench.measurement
      =
    let (construct_ns, _) =
      Bench.time (fun () ->
          Array.iter inputs ~f:(fun { co_h = _; co_tags; co_ops } ->
              ignore (build_complex_set co_tags co_ops : Impl.t)))
    in
    {
      Bench.name = Impl.name ^ " / Construction only";
      construct_ns;
      query_ns = 0.0;
      total_ns = construct_ns;
      trials = Array.length inputs;
    }
end

(* ------------------------------------------------------------------ *)
(* Concrete implementations *)
(* ------------------------------------------------------------------ *)

module ApproxSetBench : ImplForBench = struct
  include Approx_set.Make (TagDomain)

  let name = "ApproxSet"
end

module BddSetBench : ImplForBench = struct
  include Bdd_set.Make (OrderedTagDomain)

  let name = "BddSet"
end

(* ------------------------------------------------------------------ *)
(* Main *)
(* ------------------------------------------------------------------ *)

let () =
  Printf.printf "=== ApproxSet vs BddSet Benchmark ===\n";
  Printf.printf
    "Hierarchies: %d (pool) + %d (featured)\n\n"
    (Array.length hierarchy_pool)
    (Array.length featured_hierarchies);

  (* Pre-generate all inputs (shared across implementations) *)
  Printf.printf "Generating inputs...\n%!";

  let refinement_inputs = gen_refinement_inputs ~n:10000 in
  let variant_filter_inputs = gen_variant_filter_inputs ~n:5000 in
  let overlap_inputs = gen_overlap_inputs ~n:2000 in
  let complex_inputs = gen_complex_inputs ~n:10000 in
  let construction_inputs = gen_construction_inputs ~n:10000 in

  Printf.printf "Running benchmarks...\n\n%!";

  (* Run each implementation *)
  let module AS = BenchScenario (ApproxSetBench) in
  let module BS = BenchScenario (BddSetBench) in
  let approx_results =
    [
      AS.bench_refinement refinement_inputs;
      AS.bench_variant_filter variant_filter_inputs;
      AS.bench_overlap_validation overlap_inputs;
      AS.bench_complex_subtype complex_inputs;
      AS.bench_construction construction_inputs;
    ]
  in
  let bdd_results =
    [
      BS.bench_refinement refinement_inputs;
      BS.bench_variant_filter variant_filter_inputs;
      BS.bench_overlap_validation overlap_inputs;
      BS.bench_complex_subtype complex_inputs;
      BS.bench_construction construction_inputs;
    ]
  in

  (* Print per-implementation detail *)
  Printf.printf "--- ApproxSet ---\n";
  Bench.print_header ();
  List.iter approx_results ~f:Bench.print_row;

  Printf.printf "\n--- BddSet ---\n";
  Bench.print_header ();
  List.iter bdd_results ~f:Bench.print_row;

  (* Print comparison table *)
  let scenario_names =
    [
      "Refinement (10K)";
      "Variant filter (5K)";
      "Overlap validation (2K)";
      "Complex subtype (10K)";
      "Construction only (10K)";
    ]
  in
  Bench.print_comparison_header ();
  List.iter2_exn
    scenario_names
    (List.zip_exn approx_results bdd_results)
    ~f:(fun name (a, b) ->
      Bench.print_comparison_row name a.Bench.total_ns b.Bench.total_ns;
      Bench.print_comparison_row
        ("  construct" ^ String.make 24 ' ')
        a.Bench.construct_ns
        b.Bench.construct_ns;
      Bench.print_comparison_row
        ("  query" ^ String.make 28 ' ')
        a.Bench.query_ns
        b.Bench.query_ns);
  Printf.printf "\n"
