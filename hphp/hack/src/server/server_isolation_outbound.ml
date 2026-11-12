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

module PathSetValue = struct
  type t = Relative_path.Set.t

  let description = "Server_isolation_outbound"
end

module OutboundHeap =
  Shared_mem.Heap
    (Shared_mem.ImmediateBackend (Shared_mem.NonEvictable)) (PathKey)
    (PathSetValue)

(* Members are left out: on a typed receiver the [Class] occurrence beside them
   carries the edge. A dynamic receiver has none, so that edge is missed —
   fewer candidates offered, which costs recall and not correctness.
   [Attribute None] is kept because it is the one class reference with no
   [Class] occurrence of its own; [Attribute (Some _)] is only [__Override],
   which resolves to a method's class instead. *)
let is_top_level_reference (occ : Relative_path.t Symbol_occurrence.t) : bool =
  Symbol_occurrence.(
    match occ.type_ with
    | Class _
    | Function
    | GConst
    | Attribute None ->
      true
    | Module
    | Attribute (Some _)
    | Method _
    | Property _
    | XhpLiteralAttr _
    | ClassConst _
    | Typeconst _
    | EnumClassLabel _
    | LocalVar
    | TypeVar
    | BuiltInType _
    | Keyword _
    | PureFunctionContext
    | BestEffortArgument _
    | HhFixme
    | HhIgnore ->
      false)

(** The files [path] statically references, excluding itself. *)
let outbound_of_file (ctx : Provider_context.t) (path : Relative_path.t) :
    Relative_path.Set.t =
  let (ctx, entry) = Provider_context.add_entry_if_missing ~ctx ~path in
  let (_ast, get_def) = Server_deps_util.get_ast_getdef ctx entry in
  Identify_symbol_service.all_symbols_ctx ~ctx ~entry
  |> List.filter ~f:(fun occ ->
         Option.is_none occ.Symbol_occurrence.is_declaration
         && is_top_level_reference occ)
  |> List.filter_map ~f:get_def
  |> List.map ~f:(fun def -> Pos.filename def.Symbol_definition.pos)
  |> List.filter ~f:(fun target -> not (Relative_path.equal target path))
  (* A builtin resolves to an .hhi in hh_server's own bundle, which no package
     could contain. *)
  |> List.filter ~f:(fun target ->
         Relative_path.is_root (Relative_path.prefix target))
  |> Relative_path.Set.of_list

let indexed_count = ref 0

(* Which paths are in the heap, so [reset] can drop them. *)
let cached : Relative_path.t HashSet.t = HashSet.create ()

let index_one ctx path =
  let outbound =
    try outbound_of_file ctx path with
    (* Only an unreadable file becomes an empty edge set. An empty set is not
       neutral — a file that depends on nothing absorbs into a cluster for free
       — so swallowing a naming-table or dependency-graph failure turns it into
       a wrong cluster. Those fail for everything, not for this file. *)
    | Disk.No_such_file_or_directory _ ->
      Hh_logger.log
        "[isolation] outbound: no such file %s"
        (Relative_path.suffix path);
      Relative_path.Set.empty
  in
  OutboundHeap.add path outbound;
  HashSet.add cached path;
  outbound

(** Indexes [paths] one at a time, and stops at the first one where
    [check_memory] says the machine is low, returning why. Checked here, in the
    worker, before every file: a worker's memory grows with each file it
    indexes, and the master is waiting for the call to return and cannot look
    until then. *)
let index_batch
    (ctx : Provider_context.t)
    (check_memory : unit -> unit)
    (_ : string option)
    (paths : Relative_path.t list) : string option =
  let rec index = function
    | [] -> None
    | path :: rest ->
      (match check_memory () with
      | exception Server_isolation_memory.Below_floor reason -> Some reason
      | () ->
        ignore (index_one ctx path : Relative_path.Set.t);
        index rest)
  in
  index paths

let ensure_indexed
    ~(ctx : Provider_context.t)
    ~(workers : Multi_worker.worker list option)
    ~(check_memory : unit -> unit)
    (paths : Relative_path.t list) : unit =
  let missing =
    (* [mem] rather than [get], which would deserialise every hit's edge set
       only to discard it. *)
    List.filter paths ~f:(fun p -> not (OutboundHeap.mem p))
    (* Deduplicated so a repeated path is not indexed twice, and sorted for
       EdenFS: buckets are carved from this list in order, so each worker
       materializes a contiguous run of the tree rather than scattered reads. *)
    |> List.dedup_and_sort ~compare:Relative_path.compare
  in
  if not (List.is_empty missing) then begin
    (* Before the call, not after: if [Multi_worker.call] raises partway, what
       the workers already wrote would otherwise sit in a non-evictable heap
       that no [reset] can find. Naming a path never written is harmless. *)
    List.iter missing ~f:(HashSet.add cached);
    indexed_count := !indexed_count + List.length missing;
    (* A worker that finds memory low ends its bucket, and every other does the
       same at its next file, so the call returns within a file's time rather
       than raising with workers still busy. *)
    let stopped =
      Multi_worker.call
        workers
        ~job:(index_batch ctx check_memory)
        ~neutral:None
        ~merge:(fun reason so_far ->
          match so_far with
          | Some _ -> so_far
          | None -> reason)
        ~next:(Multi_worker.next workers missing)
    in
    Option.iter stopped ~f:(fun reason ->
        raise (Server_isolation_memory.Below_floor reason))
  end

(* The fallback for anything [ensure_indexed] has not pre-warmed. *)
let get (ctx : Provider_context.t) (path : Relative_path.t) :
    Relative_path.Set.t =
  match OutboundHeap.get path with
  | Some outbound -> outbound
  | None ->
    let outbound = index_one ctx path in
    incr indexed_count;
    outbound

let release_cache () =
  let n = HashSet.length cached in
  if n > 0 then begin
    (* [KeySet] is keyed by [PathKey], a distinct type from the paths in
       [cached]. *)
    OutboundHeap.remove_batch
      (HashSet.fold cached ~init:OutboundHeap.KeySet.empty ~f:(fun path acc ->
           OutboundHeap.KeySet.add path acc));
    HashSet.clear cached;
    Hh_logger.log "[isolation] outbound: released %d cached files" n
  end

let files_indexed () = !indexed_count

let reset () =
  release_cache ();
  indexed_count := 0
