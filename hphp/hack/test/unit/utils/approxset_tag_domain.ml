(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

(** Shared library for ApproxSet workload tests.

    Provides a mock tag domain (mirroring typing_tag_defs.ml), a QuickCheck-
    generated class hierarchy, domain modules for ApproxSet and BddSet, ground
    truth via witness denotation, and generators for realistic type-like sets.

    Following the fuzz testing approach from Hack Native's BDD type analysis
    (Maessen, TMM Oct 2021 [DHJ1]): randomly generate small class/interface
    hierarchies, randomly generate type operations, check properties against
    ground truth. *)

open Hh_prelude

(* ------------------------------------------------------------------ *)
(* Tag type (mirrors typing_tag_defs.ml without Typing_defs dependency) *)
(* ------------------------------------------------------------------ *)

type prim =
  | Int
  | String
  | Bool
  | Float
  | Null
  | Resource
  | Keyset
  | Label
[@@deriving sexp_of, compare, equal, hash]

let all_of_prim = [Int; String; Bool; Float; Null; Resource; Keyset; Label]

type class_kind =
  | Final
  | NonFinal
  | Interface
[@@deriving sexp_of, compare, equal, hash]

type tag =
  | Prim of prim
  | Dict
  | Shape (* shape ⊂ dict at runtime, relation returns none *)
  | Vec
  | Tuple (* tuple ⊂ vec at runtime, relation returns none *)
  | Object (* superset of all Instance tags *)
  | Instance of {
      id: int;
      kind: class_kind;
      ancestor_count: int;
    }
  | BuiltIn (* overlaps with Object, relation returns none *)
  | Mixed (* top/universe element for BddSet *)
[@@deriving sexp_of, compare, equal, hash]

(* ------------------------------------------------------------------ *)
(* Hierarchy type *)
(* ------------------------------------------------------------------ *)

type hierarchy = {
  num_classes: int;
  ancestors: int -> Int.Set.t;
  descendants: int -> Int.Set.t;
  is_final: int -> bool;
  is_interface: int -> bool;
  sealed_whitelist: int -> int list option;
  require_extends: int -> int list;
  special_interface_tags: int -> tag list;
}

(* ------------------------------------------------------------------ *)
(* Hierarchy QuickCheck generator *)
(* ------------------------------------------------------------------ *)

let gen_hierarchy : hierarchy Quickcheck.Generator.t =
  let open Quickcheck.Let_syntax in
  let%bind num_classes = Int.gen_incl 4 16 in
  (* Decide which ids are interfaces (~30%) *)
  let%bind is_iface_arr =
    List.gen_with_length num_classes
    @@ Quickcheck.Generator.map (Int.gen_incl 0 9) ~f:(fun x -> x < 3)
  in
  let is_iface_arr = Array.of_list is_iface_arr in
  (* Generate DAG edges *)
  let%bind parents =
    List.gen_with_length num_classes @@ Quickcheck.Generator.return ()
  in
  let _ = parents in
  (* Build parent relationships: for each node i, pick parents from [0, i-1] *)
  let parent_edges = Array.create ~len:num_classes [] in
  let%bind edge_choices =
    (* For each node, we need up to 3 random choices *)
    List.gen_with_length (num_classes * 4) (Int.gen_incl 0 1000000)
  in
  let edge_arr = Array.of_list edge_choices in
  let () =
    for i = 1 to num_classes - 1 do
      let base_idx = i * 4 in
      if is_iface_arr.(i) then begin
        (* Interface: extends 0-2 other interfaces from [0, i-1] *)
        let ifaces_before =
          List.filter (List.init i ~f:Fun.id) ~f:(fun j -> is_iface_arr.(j))
        in
        match ifaces_before with
        | [] -> ()
        | _ ->
          let len = List.length ifaces_before in
          let n_parents = edge_arr.(base_idx) mod 3 (* 0, 1, or 2 parents *) in
          let chosen =
            List.filteri ifaces_before ~f:(fun idx _j ->
                (idx mod (len + 1)
                <
                if n_parents = 0 then
                  0
                else
                  (n_parents * (len + 1) / len) + 1)
                && edge_arr.(base_idx + 1 + (idx mod 2)) mod (len + 1) <= idx)
          in
          let chosen = List.take chosen n_parents in
          parent_edges.(i) <- chosen
      end else begin
        (* Concrete class: at most one class parent + 0-2 interface parents *)
        let classes_before =
          List.filter (List.init i ~f:Fun.id) ~f:(fun j -> not is_iface_arr.(j))
        in
        let ifaces_before =
          List.filter (List.init i ~f:Fun.id) ~f:(fun j -> is_iface_arr.(j))
        in
        (* Pick at most one class parent *)
        let class_parent =
          match classes_before with
          | [] -> []
          | _ ->
            let len = List.length classes_before in
            if edge_arr.(base_idx) mod 3 = 0 then
              []
            (* ~33% chance no parent *)
            else
              let idx = edge_arr.(base_idx + 1) mod len in
              [List.nth_exn classes_before idx]
        in
        (* Pick 0-2 interface parents *)
        let iface_parents =
          match ifaces_before with
          | [] -> []
          | _ ->
            let len = List.length ifaces_before in
            let n = edge_arr.(base_idx + 2) mod 3 in
            if n = 0 then
              []
            else
              let p1 =
                List.nth_exn ifaces_before (edge_arr.(base_idx + 3) mod len)
              in
              if n = 1 || len <= 1 then
                [p1]
              else
                let p2_idx =
                  (edge_arr.(base_idx + 3)
                  + 1
                  + (edge_arr.(base_idx + 2) mod max 1 (len - 1)))
                  mod len
                in
                let p2 = List.nth_exn ifaces_before p2_idx in
                if p1 = p2 then
                  [p1]
                else
                  [p1; p2]
        in
        parent_edges.(i) <- class_parent @ iface_parents
      end
    done
  in
  (* Compute transitive ancestors *)
  let ancestors_arr = Array.init num_classes ~f:(fun _ -> Int.Set.empty) in
  let () =
    for i = 0 to num_classes - 1 do
      let direct = Int.Set.of_list parent_edges.(i) in
      let transitive =
        List.fold parent_edges.(i) ~init:direct ~f:(fun acc p ->
            Set.union acc ancestors_arr.(p))
      in
      ancestors_arr.(i) <- transitive
    done
  in
  (* Compute transitive descendants *)
  let descendants_arr = Array.init num_classes ~f:(fun _ -> Int.Set.empty) in
  let () =
    for i = num_classes - 1 downto 0 do
      List.iter parent_edges.(i) ~f:(fun p ->
          descendants_arr.(p) <-
            Set.add (Set.union descendants_arr.(p) descendants_arr.(i)) i)
    done
  in
  (* Mark leaf classes as Final with ~50% probability *)
  let%bind final_choices =
    List.gen_with_length num_classes Bool.quickcheck_generator
  in
  let final_arr = Array.of_list final_choices in
  let is_final_arr = Array.create ~len:num_classes false in
  let () =
    for i = 0 to num_classes - 1 do
      if
        (not is_iface_arr.(i))
        && Set.is_empty descendants_arr.(i)
        && final_arr.(i)
      then
        is_final_arr.(i) <- true
    done
  in
  (* Sealed classes: ~20% of non-final concrete classes *)
  (* Sealed interfaces: ~20% of interfaces *)
  let%bind seal_choices =
    List.gen_with_length num_classes @@ Int.gen_incl 0 9
  in
  let seal_arr = Array.of_list seal_choices in
  let sealed_whitelist_arr : int list option array =
    Array.create ~len:num_classes None
  in
  let%bind seal_subset_choices =
    List.gen_with_length (num_classes * 3) (Int.gen_incl 0 1000000)
  in
  let seal_sub_arr = Array.of_list seal_subset_choices in
  let () =
    for i = 0 to num_classes - 1 do
      let has_desc = not (Set.is_empty descendants_arr.(i)) in
      let should_seal =
        seal_arr.(i) < 2 (* ~20% *)
        && has_desc
        && (is_iface_arr.(i) || not is_final_arr.(i))
      in
      if should_seal then begin
        let desc_list = Set.to_list descendants_arr.(i) in
        let len = List.length desc_list in
        (* Pick a non-empty random subset *)
        let n_pick = max 1 ((seal_sub_arr.(i * 3) mod len) + 1) in
        let n_pick = min n_pick len in
        (* Simple selection: take first n_pick after shuffling by index *)
        let selected =
          List.filteri desc_list ~f:(fun idx _ ->
              (idx + seal_sub_arr.((i * 3) + 1)) mod len < n_pick)
        in
        let selected =
          if List.is_empty selected then
            [List.hd_exn desc_list]
          else
            selected
        in
        sealed_whitelist_arr.(i) <- Some selected
      end
    done
  in
  (* Require extends: ~30% of non-sealed interfaces get require extends *)
  let%bind req_choices = List.gen_with_length num_classes @@ Int.gen_incl 0 9 in
  let req_arr = Array.of_list req_choices in
  let require_extends_arr : int list array = Array.create ~len:num_classes [] in
  let () =
    for i = 0 to num_classes - 1 do
      if
        is_iface_arr.(i)
        && Option.is_none sealed_whitelist_arr.(i)
        && req_arr.(i) < 3 (* ~30% *)
      then begin
        (* Find ancestor concrete classes *)
        let ancestor_classes =
          Set.to_list ancestors_arr.(i)
          |> List.filter ~f:(fun j -> not is_iface_arr.(j))
        in
        match ancestor_classes with
        | [] -> ()
        | cls ->
          let n_req = min (List.length cls) (1 + (req_arr.(i) mod 2)) in
          require_extends_arr.(i) <- List.take cls n_req
      end
    done
  in
  (* Special interfaces: 2-3 interfaces include non-Instance tags *)
  let%bind n_special = Int.gen_incl 2 (min 3 (max 2 (num_classes / 4))) in
  let iface_ids =
    List.filter (List.init num_classes ~f:Fun.id) ~f:(fun i -> is_iface_arr.(i))
  in
  let special_tags_arr : tag list array = Array.create ~len:num_classes [] in
  let extra_tag_choices =
    [
      [Prim String];
      [Prim Int; Prim String; Prim Float];
      [Dict; Vec; Prim Keyset];
    ]
  in
  let () =
    let ifaces_available = List.take iface_ids n_special in
    List.iteri ifaces_available ~f:(fun idx i ->
        let tags =
          List.nth_exn extra_tag_choices (idx mod List.length extra_tag_choices)
        in
        special_tags_arr.(i) <- tags)
  in
  let hierarchy =
    {
      num_classes;
      ancestors = (fun i -> ancestors_arr.(i));
      descendants = (fun i -> descendants_arr.(i));
      is_final = (fun i -> is_final_arr.(i));
      is_interface = (fun i -> is_iface_arr.(i));
      sealed_whitelist = (fun i -> sealed_whitelist_arr.(i));
      require_extends = (fun i -> require_extends_arr.(i));
      special_interface_tags = (fun i -> special_tags_arr.(i));
    }
  in
  return hierarchy

(* ------------------------------------------------------------------ *)
(* Domain Module: TagDomain *)
(* ------------------------------------------------------------------ *)

module TagDomain :
  Approx_set_intf.DomainType with type t = tag and type ctx = hierarchy = struct
  type t = tag

  type ctx = hierarchy

  let relation (t1 : t) ~(ctx : ctx) (t2 : t) : Set_relation.t =
    let h = ctx in
    match (t1, t2) with
    | (Mixed, Mixed) -> Set_relation.equivalent
    | (Mixed, _) -> Set_relation.superset
    | (_, Mixed) -> Set_relation.subset
    (* BuiltIn overlaps with Object *)
    | (BuiltIn, BuiltIn) -> Set_relation.equivalent
    | (BuiltIn, Object)
    | (Object, BuiltIn) ->
      Set_relation.none
    (* Shape ⊂ Dict at runtime *)
    | (Shape, Shape) -> Set_relation.equivalent
    | (Shape, Dict)
    | (Dict, Shape) ->
      Set_relation.none
    | (Dict, Dict) -> Set_relation.equivalent
    (* Tuple ⊂ Vec at runtime *)
    | (Tuple, Tuple) -> Set_relation.equivalent
    | (Tuple, Vec)
    | (Vec, Tuple) ->
      Set_relation.none
    | (Vec, Vec) -> Set_relation.equivalent
    (* Equal tags *)
    | (Prim p1, Prim p2) when equal_prim p1 p2 -> Set_relation.equivalent
    | (Object, Object) -> Set_relation.equivalent
    (* Object ⊃ Instance *)
    | (Object, Instance _) -> Set_relation.superset
    | (Instance _, Object) -> Set_relation.subset
    (* Instance vs Instance *)
    | (Instance { id = id1; _ }, Instance { id = id2; kind = kind2; _ }) ->
      if id1 = id2 then
        Set_relation.equivalent
      else if Set.mem (h.ancestors id2) id1 then
        Set_relation.superset
      else if Set.mem (h.ancestors id1) id2 then
        Set_relation.subset
      else begin
        (* Unrelated *)
        match (t1, Instance { id = id2; kind = kind2; ancestor_count = 0 }) with
        | (Instance { kind = Final; _ }, _)
        | (_, Instance { kind = Final; _ }) ->
          Set_relation.disjoint
        | (Instance { kind = Interface; _ }, _)
        | (_, Instance { kind = Interface; _ }) ->
          Set_relation.none
        | _ -> (* both NonFinal *) Set_relation.disjoint
      end
    (* All other cross-category pairs *)
    | _ -> Set_relation.disjoint
end

(* ------------------------------------------------------------------ *)
(* Ordered Domain (for BddSet) *)
(* ------------------------------------------------------------------ *)

module OrderedTagDomain :
  Approx_set_intf.OrderedDomainType with type t = tag and type ctx = hierarchy =
struct
  include TagDomain

  let top = Mixed

  let depth_of (t : tag) : int =
    match t with
    | Mixed -> 0
    | Object
    | Prim _
    | Dict
    | Shape
    | Vec
    | Tuple
    | BuiltIn ->
      1
    | Instance { ancestor_count; _ } -> ancestor_count + 2

  let category_of (t : tag) : int =
    match t with
    | Mixed -> 0
    | Object -> 1
    | Prim _ -> 2
    | Dict -> 3
    | Shape -> 4
    | Vec -> 5
    | Tuple -> 6
    | BuiltIn -> 7
    | Instance { kind = Final; _ } -> 8
    | Instance { kind = NonFinal; _ } -> 9
    | Instance { kind = Interface; _ } -> 10

  let compare (a : tag) (b : tag) : int =
    let da = depth_of a and db = depth_of b in
    if da <> db then
      Int.compare da db
    else
      let ca = category_of a and cb = category_of b in
      if ca <> cb then
        Int.compare ca cb
      else
        match (a, b) with
        | (Prim p1, Prim p2) -> compare_prim p1 p2
        | (Instance { id = id1; _ }, Instance { id = id2; _ }) ->
          Int.compare id1 id2
        | _ -> compare_tag a b
end

(* ------------------------------------------------------------------ *)
(* Ground Truth via Witness Denotation *)
(* ------------------------------------------------------------------ *)

let make_denotation (h : hierarchy) : tag -> Int.Set.t =
  (* Assign unique witness integers *)
  let next = ref 0 in
  let fresh () =
    let v = !next in
    incr next;
    v
  in
  (* Witnesses for each prim *)
  let w_int = fresh () in
  let w_string = fresh () in
  let w_bool = fresh () in
  let w_float = fresh () in
  let w_null = fresh () in
  let w_resource = fresh () in
  let w_keyset = fresh () in
  let w_label = fresh () in
  let prim_witness = function
    | Int -> w_int
    | String -> w_string
    | Bool -> w_bool
    | Float -> w_float
    | Null -> w_null
    | Resource -> w_resource
    | Keyset -> w_keyset
    | Label -> w_label
  in
  (* Shared/exclusive witnesses for overlapping pairs *)
  let w_shape_dict = fresh () in
  let w_dict_only = fresh () in
  let w_tuple_vec = fresh () in
  let w_vec_only = fresh () in
  let w_builtin_obj = fresh () in
  let w_builtin_only = fresh () in
  (* One witness per leaf class *)
  let leaf_classes =
    List.filter (List.init h.num_classes ~f:Fun.id) ~f:(fun i ->
        (not (h.is_interface i)) && Set.is_empty (h.descendants i))
  in
  let leaf_witness = Hashtbl.create (module Int) in
  List.iter leaf_classes ~f:(fun i ->
      Hashtbl.set leaf_witness ~key:i ~data:(fresh ()));
  (* Also give leaf interfaces (interfaces with no interface descendants)
     their own witness if they have class descendants that are leaves *)
  let all_leaf_witnesses =
    Int.Set.of_list
    @@ List.map leaf_classes ~f:(fun i -> Hashtbl.find_exn leaf_witness i)
  in
  let denote (t : tag) : Int.Set.t =
    match t with
    | Prim p -> Int.Set.singleton (prim_witness p)
    | Dict -> Int.Set.of_list [w_dict_only; w_shape_dict]
    | Shape -> Int.Set.singleton w_shape_dict
    | Vec -> Int.Set.of_list [w_vec_only; w_tuple_vec]
    | Tuple -> Int.Set.singleton w_tuple_vec
    | Object -> Set.add all_leaf_witnesses w_builtin_obj
    | BuiltIn -> Int.Set.of_list [w_builtin_only; w_builtin_obj]
    | Mixed ->
      (* All witnesses *)
      Int.Set.of_list (List.init !next ~f:Fun.id)
    | Instance { id; kind; _ } ->
      (match kind with
      | Final ->
        (match Hashtbl.find leaf_witness id with
        | Some w -> Int.Set.singleton w
        | None ->
          (* Final but has descendants — shouldn't happen, but safe fallback *)
          Int.Set.empty)
      | NonFinal ->
        (* Raw hierarchy-based denotation: all leaf descendants (including self).
           sealed_whitelist narrowing is NOT applied here — it happens at the
           to_datatypes level via A.inter. The atom-level relation function
           only knows about the raw class hierarchy. *)
        Set.add (h.descendants id) id
        |> Set.to_list
        |> List.filter_map ~f:(fun j -> Hashtbl.find leaf_witness j)
        |> Int.Set.of_list
      | Interface ->
        (* Raw hierarchy-based denotation: all leaf descendants (including self).
           sealed_whitelist and require_extends narrowing are NOT applied here —
           they happen at the to_datatypes level via A.inter. Special interface
           extra tags (e.g. Prim String for Stringish) are also NOT included
           here — they are added at the to_datatypes level via union.
           The atom-level relation function treats Instance atoms as pure
           class hierarchy elements. *)
        let desc_self = Set.add (h.descendants id) id in
        Set.to_list desc_self
        |> List.filter_map ~f:(fun j -> Hashtbl.find leaf_witness j)
        |> Int.Set.of_list)
  in
  denote

(* ------------------------------------------------------------------ *)
(* Wrapped ASet (ground-truth tracking) *)
(* ------------------------------------------------------------------ *)

module type S_for_test = sig
  include Approx_set_intf.S with module Domain := TagDomain

  val name : string

  val expect_complete : bool
end

module ASet (Impl : S_for_test) = struct
  module Impl = Impl

  type op =
    | Union of op * op
    | Inter of op * op
    | Diff of op * op
    | Singleton of tag
    | MixedOp
    | EmptyOp
  [@@deriving sexp_of]

  type t = {
    set: (Impl.t[@sexp.opaque]);
    expected: Int.Set.t;
    op: op;
  }

  let sexp_of_t { set = _; op; expected } =
    [%sexp_of: op * Int.Set.t] (op, expected)

  let empty = { set = Impl.empty; expected = Int.Set.empty; op = EmptyOp }

  let singleton (h : hierarchy) (t : tag) : t =
    let denote = make_denotation h in
    { set = Impl.singleton t; expected = denote t; op = Singleton t }

  let singleton_with_denote (denote : tag -> Int.Set.t) (t : tag) : t =
    { set = Impl.singleton t; expected = denote t; op = Singleton t }

  let union (a : t) (b : t) : t =
    {
      set = Impl.union a.set b.set;
      expected = Set.union a.expected b.expected;
      op = Union (a.op, b.op);
    }

  let inter (a : t) (b : t) : t =
    {
      set = Impl.inter a.set b.set;
      expected = Set.inter a.expected b.expected;
      op = Inter (a.op, b.op);
    }

  let diff (a : t) (b : t) : t =
    {
      set = Impl.diff a.set b.set;
      expected = Set.diff a.expected b.expected;
      op = Diff (a.op, b.op);
    }

  let mixed (h : hierarchy) : t =
    let denote = make_denotation h in
    { set = Impl.singleton Mixed; expected = denote Mixed; op = MixedOp }

  let mixed_with_denote (denote : tag -> Int.Set.t) : t =
    { set = Impl.singleton Mixed; expected = denote Mixed; op = MixedOp }

  let comp (h : hierarchy) (s : t) : t = diff (mixed h) s

  let are_disjoint (h : hierarchy) (a : t) (b : t) : bool =
    Impl.are_disjoint h a.set b.set

  let is_subset (h : hierarchy) (a : t) (b : t) : bool =
    Impl.is_subset h a.set b.set
end

(* ------------------------------------------------------------------ *)
(* Tag Generator *)
(* ------------------------------------------------------------------ *)

let gen_tag (h : hierarchy) : tag Quickcheck.Generator.t =
  let open Quickcheck.Generator in
  let gen_instance =
    let%bind.Quickcheck.Generator id = Int.gen_incl 0 (h.num_classes - 1) in
    let kind =
      if h.is_interface id then
        Interface
      else if h.is_final id then
        Final
      else
        NonFinal
    in
    let ancestor_count = Set.length (h.ancestors id) in
    return (Instance { id; kind; ancestor_count })
  in
  let gen_prim =
    let%map.Quickcheck.Generator p = of_list all_of_prim in
    Prim p
  in
  let gen_container = of_list [Dict; Shape; Vec; Tuple] in
  weighted_union
    [
      (40.0, gen_instance);
      (35.0, gen_prim);
      (10.0, return Object);
      (8.0, gen_container);
      (7.0, return BuiltIn);
    ]

(* ------------------------------------------------------------------ *)
(* to_datatypes — hierarchy-aware set construction *)
(* ------------------------------------------------------------------ *)

(* Convenience wrapper for ASet *)
module To_datatypes (Impl : S_for_test) = struct
  module A = ASet (Impl)

  let rec to_datatypes (h : hierarchy) (denote : tag -> Int.Set.t) (cid : int) :
      A.t =
    let kind =
      if h.is_interface cid then
        Interface
      else if h.is_final cid then
        Final
      else
        NonFinal
    in
    let ancestor_count = Set.length (h.ancestors cid) in
    let base_tag = Instance { id = cid; kind; ancestor_count } in
    let base = A.singleton_with_denote denote base_tag in
    match kind with
    | Final -> base
    | NonFinal ->
      (match h.sealed_whitelist cid with
      | None -> base
      | Some whitelist ->
        let wl_union =
          List.fold whitelist ~init:A.empty ~f:(fun acc wid ->
              A.union acc (to_datatypes h denote wid))
        in
        A.inter wl_union base)
    | Interface ->
      let with_sealed =
        match h.sealed_whitelist cid with
        | Some whitelist ->
          let wl_union =
            List.fold whitelist ~init:A.empty ~f:(fun acc wid ->
                A.union acc (to_datatypes h denote wid))
          in
          A.inter wl_union base
        | None ->
          (match h.require_extends cid with
          | [] -> base
          | reqs ->
            List.fold reqs ~init:base ~f:(fun acc req_id ->
                A.inter acc (to_datatypes h denote req_id)))
      in
      (* Special interface: add non-Instance tags *)
      let special = h.special_interface_tags cid in
      List.fold special ~init:with_sealed ~f:(fun acc t ->
          A.union acc (A.singleton_with_denote denote t))
end

(* ------------------------------------------------------------------ *)
(* "Type-like" set generator *)
(* ------------------------------------------------------------------ *)

module Gen_type_set (Impl : S_for_test) = struct
  module A = ASet (Impl)
  module TD = To_datatypes (Impl)

  let gen_type_set (h : hierarchy) (denote : tag -> Int.Set.t) :
      A.t Quickcheck.Generator.t =
    let open Quickcheck.Let_syntax in
    let to_dt cid = TD.to_datatypes h denote cid in
    let gen_class_type =
      let%map cid = Int.gen_incl 0 (h.num_classes - 1) in
      to_dt cid
    in
    let gen_prim_singleton =
      let%map p = Quickcheck.Generator.of_list all_of_prim in
      A.singleton_with_denote denote (Prim p)
    in
    let gen_nullable =
      let%map s = gen_class_type in
      A.union s (A.singleton_with_denote denote (Prim Null))
    in
    let gen_object =
      Quickcheck.Generator.return (A.singleton_with_denote denote Object)
    in
    let gen_neg_interface =
      (* Model Tneg(IsTag(ClassTag(interface_name)))
         = diff(mixed, to_datatypes(interface)) *)
      let iface_ids =
        List.filter (List.init h.num_classes ~f:Fun.id) ~f:(fun i ->
            h.is_interface i)
      in
      match iface_ids with
      | [] -> gen_class_type (* fallback if no interfaces *)
      | _ ->
        let%map iface_id = Quickcheck.Generator.of_list iface_ids in
        A.diff (A.mixed_with_denote denote) (to_dt iface_id)
    in
    Quickcheck.Generator.recursive_union
      [
        Quickcheck.Generator.weighted_union
          [
            (30.0, gen_class_type);
            (15.0, gen_prim_singleton);
            (15.0, gen_neg_interface);
            (10.0, gen_nullable);
            (5.0, gen_object);
          ];
      ]
      ~f:(fun self ->
        let gen_union =
          let%bind a = self in
          let%map b = self in
          A.union a b
        in
        let gen_inter =
          let%bind a = self in
          let%map b = self in
          A.inter a b
        in
        let gen_neg =
          let%map s = self in
          A.diff (A.mixed_with_denote denote) s
        in
        [
          Quickcheck.Generator.weighted_union
            [(15.0, gen_union); (8.0, gen_inter); (7.0, gen_neg)];
        ])
end

(* ------------------------------------------------------------------ *)
(* Helpers for finding specific hierarchy features *)
(* ------------------------------------------------------------------ *)

let find_sealed_interface (h : hierarchy) : int option =
  List.find (List.init h.num_classes ~f:Fun.id) ~f:(fun i ->
      h.is_interface i && Option.is_some (h.sealed_whitelist i))

let find_interface_with_require (h : hierarchy) : int option =
  List.find (List.init h.num_classes ~f:Fun.id) ~f:(fun i ->
      h.is_interface i && not (List.is_empty (h.require_extends i)))

let find_sealed_class (h : hierarchy) : int option =
  List.find (List.init h.num_classes ~f:Fun.id) ~f:(fun i ->
      (not (h.is_interface i))
      && (not (h.is_final i))
      && Option.is_some (h.sealed_whitelist i))

let make_tag (h : hierarchy) (cid : int) : tag =
  let kind =
    if h.is_interface cid then
      Interface
    else if h.is_final cid then
      Final
    else
      NonFinal
  in
  Instance { id = cid; kind; ancestor_count = Set.length (h.ancestors cid) }

(* ------------------------------------------------------------------ *)
(* Pre-generated hierarchy pool *)
(* ------------------------------------------------------------------ *)

(** Seeded random state for [Quickcheck.Generator.generate]. The dune build
    uses Base v0.16, which only provides [Splittable_random.State.of_int]; newer
    releases deprecate [Splittable_random.State] in favor of
    [Splittable_random.of_int]. *)
let random_of_seed (seed : int) =
  (Splittable_random.State.of_int [@alert "-deprecated"]) seed

(** Pre-generate a pool of hierarchies at module load time.
    This avoids the overhead of Quickcheck.Generator.filter on gen_hierarchy
    during tests, which rejects many hierarchies that lack sealed classes,
    sealed interfaces, or require-extends interfaces. *)

let hierarchy_pool : hierarchy array =
  let pool_size = 200 in
  Array.init pool_size ~f:(fun i ->
      let random = random_of_seed (42 + i) in
      Quickcheck.Generator.generate gen_hierarchy ~size:30 ~random)

(** Construct a small hierarchy guaranteed to have a sealed class, sealed
    interface, and an interface with require_extends. *)
let make_featured_hierarchy (seed : int) : hierarchy =
  let random = random_of_seed seed in
  let n = 8 in
  (* Layout (single class inheritance, multiple interface implementation):
     0: concrete (root)
     1: concrete extends 0, implements 5
     2: concrete extends 1 (Final), implements 4, 7
     3: concrete extends 0 (Final), implements 4, 5, 7
     4: interface (sealed, whitelist=[2,3])
     5: interface (require extends 0)
     6: concrete extends 0 (Final)
     7: interface (sealed, whitelist=[2,3], special=[Prim String])
  *)
  let is_iface = [| false; false; false; false; true; true; false; true |] in
  (* ancestors = transitive ancestors (not including self) *)
  let ancestors_arr = Array.init n ~f:(fun _ -> Int.Set.empty) in
  ancestors_arr.(1) <- Int.Set.of_list [0; 5];
  ancestors_arr.(2) <- Int.Set.of_list [0; 1; 4; 5; 7];
  ancestors_arr.(3) <- Int.Set.of_list [0; 4; 5; 7];
  ancestors_arr.(6) <- Int.Set.of_list [0];
  (* descendants = transitive descendants (not including self) *)
  let descendants_arr = Array.init n ~f:(fun _ -> Int.Set.empty) in
  descendants_arr.(0) <- Int.Set.of_list [1; 2; 3; 6];
  descendants_arr.(1) <- Int.Set.of_list [2];
  descendants_arr.(4) <- Int.Set.of_list [2; 3];
  descendants_arr.(5) <- Int.Set.of_list [1; 2; 3];
  descendants_arr.(7) <- Int.Set.of_list [2; 3];
  let is_final = [| false; false; true; true; false; false; true; false |] in
  let sealed_wl : int list option array = Array.create ~len:n None in
  sealed_wl.(4) <- Some [2; 3];
  sealed_wl.(7) <- Some [2; 3];
  let req_ext : int list array = Array.create ~len:n [] in
  req_ext.(5) <- [0];
  let special : tag list array = Array.create ~len:n [] in
  special.(7) <- [Prim String];
  (* Add a bit of randomness to vary the tests *)
  let _ = Splittable_random.bool random in
  {
    num_classes = n;
    ancestors = (fun i -> ancestors_arr.(i));
    descendants = (fun i -> descendants_arr.(i));
    is_final = (fun i -> is_final.(i));
    is_interface = (fun i -> is_iface.(i));
    sealed_whitelist = (fun i -> sealed_wl.(i));
    require_extends = (fun i -> req_ext.(i));
    special_interface_tags = (fun i -> special.(i));
  }

let featured_hierarchies : hierarchy array =
  Array.init 20 ~f:(fun i -> make_featured_hierarchy (50000 + i))

let hierarchies_with_sealed : hierarchy array Lazy.t =
  lazy
    (let from_pool =
       Array.filter hierarchy_pool ~f:(fun h ->
           Option.is_some (find_sealed_class h)
           || Option.is_some (find_sealed_interface h))
     in
     Array.append from_pool featured_hierarchies)

let hierarchies_with_sealed_interface : hierarchy array Lazy.t =
  lazy
    (let from_pool =
       Array.filter hierarchy_pool ~f:(fun h ->
           Option.is_some (find_sealed_interface h))
     in
     Array.append from_pool featured_hierarchies)

let hierarchies_with_require : hierarchy array Lazy.t =
  lazy
    (let from_pool =
       Array.filter hierarchy_pool ~f:(fun h ->
           Option.is_some (find_interface_with_require h))
     in
     Array.append from_pool featured_hierarchies)

let gen_hierarchy_from_pool : hierarchy Quickcheck.Generator.t =
  Quickcheck.Generator.of_list (Array.to_list hierarchy_pool)

let gen_hierarchy_with_sealed : hierarchy Quickcheck.Generator.t =
  Quickcheck.Generator.of_list
    (Array.to_list (Lazy.force hierarchies_with_sealed))

let gen_hierarchy_with_sealed_interface : hierarchy Quickcheck.Generator.t =
  Quickcheck.Generator.of_list
    (Array.to_list (Lazy.force hierarchies_with_sealed_interface))

let gen_hierarchy_with_require : hierarchy Quickcheck.Generator.t =
  Quickcheck.Generator.of_list
    (Array.to_list (Lazy.force hierarchies_with_require))
