(*
 * Copyright (c) 2015, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

(** On the fly defs-declaration are called when the user modified a file
  and we are not at initilalization time anymore. Therefore, we have a bit more
  work to do. We need to calculate what must be re-checked. *)

open Hh_prelude
open Typing_deps

type get_classes_in_file = Relative_path.t -> S_set.t

type redo_type_decl_result = {
  fanout: Fanout.t;
  old_decl_missing_count: int;
}

let lvl = Hh_logger.Level.Debug

let compute_deps_neutral = (Fanout.empty, 0)

let reconcile_parallel_redecl_fanout
    ~(during_init : bool) ~(deps_mode : Typing_deps_mode.t) (fanout : Fanout.t)
    : Fanout.t =
  if during_init then
    fanout
  else
    (* Dependency edges discovered during typechecking are registered in the
       master process. Redecl workers can therefore compute fanout from a stale
       process-local view of the dependency graph. *)
    let master_dependents =
      DepSet.diff
        (Typing_deps.add_typing_deps deps_mode fanout.Fanout.changed)
        fanout.Fanout.changed
    in
    let to_recheck = DepSet.union fanout.Fanout.to_recheck master_dependents in
    { fanout with Fanout.to_recheck }

module For_test = struct
  let reconcile_parallel_redecl_fanout = reconcile_parallel_redecl_fanout
end

(** This is the place where we are going to put everything necessary for
  the redeclaration. We could "pass" the values directly to the workers,
  but it gives too much work to the master and slows things down,
  so what we do instead is pass the data through shared memory via
  OnTheFlyStore.
  I tried replicating the data to speed things up but it had no effect. *)
module OnTheFlyStore = Global_storage.Make (struct
  type t = Decl_compare.VersionedNames.t Relative_path.Map.t
end)

(** Given a set of functions, compare the old and the new decl and deduce
  what must be rechecked accordingly. *)
let compare_funs_and_get_fanout
    ctx old_funs fanout_acc (funs : Decl_compare.VersionedSSet.diff) =
  let (fanout, old_funs_missing) =
    Decl_compare.get_funs_deps ~ctx old_funs funs
  in
  let fanout_acc = Fanout.union fanout fanout_acc in
  (fanout_acc, old_funs_missing)

(** Given a set of typedefs, compare the old and the new decl and deduce
  what must be rechecked accordingly. *)
let compare_types_and_get_fanout
    ctx old_types fanout_acc (types : Decl_compare.VersionedSSet.diff) =
  let (fanout, old_types_missing) =
    Decl_compare.get_types_deps ~ctx old_types types
  in
  let fanout_acc = Fanout.union fanout fanout_acc in
  (fanout_acc, old_types_missing)

(* Given a set of global constants, compare the old and the new decl and
   deduce what must be rechecked accordingly. *)
let compare_gconsts_and_get_fanout
    ctx old_gconsts fanout_acc (gconsts : Decl_compare.VersionedSSet.diff) =
  let (fanout, old_gconsts_missing) =
    Decl_compare.get_gconsts_deps ~ctx old_gconsts gconsts
  in
  let fanout_acc = Fanout.union fanout fanout_acc in
  (fanout_acc, old_gconsts_missing)

(* Given a set of modules, compare the old and the new decl and
   deduce what must be rechecked accordingly. *)
let compare_modules_and_get_fanout
    ctx old_modules fanout_acc (modules : Decl_compare.VersionedSSet.diff) =
  let (fanout, old_modules_missing) =
    Decl_compare.get_modules_deps ~ctx ~old_modules ~modules
  in
  let fanout_acc = Fanout.union fanout fanout_acc in
  (fanout_acc, old_modules_missing)

(*****************************************************************************)
(* Redeclares a list of files
 * And then computes the files that must be redeclared/rechecked by looking
 * at what changed in the signatures of the classes/functions.
 *)
(*****************************************************************************)

let invoke_testing_hook_after_file_redecl
    (hooks : Server_env.type_check_testing_hooks option)
    (file : Relative_path.t) =
  Option.iter hooks ~f:(fun hooks -> hooks.Server_env.after_file_redecl file)

(** Compute decls in files. Return errors raised during decling. *)
let redeclare_files ~type_check_testing_hooks ctx filel =
  List.iter filel ~f:(fun fn ->
      Decl.make_env ~sh:Shared_mem.Uses ctx fn;
      invoke_testing_hook_after_file_redecl type_check_testing_hooks fn)

(** Invalidate local caches and compute decls in files. Return errors raised during decling. *)
let decl_files ~type_check_testing_hooks ctx filel =
  Shared_mem.invalidate_local_caches ();
  redeclare_files ~type_check_testing_hooks ctx filel

let compare_decls_and_get_fanout
    ctx
    (defs_per_file : Decl_compare.VersionedNames.t Relative_path.Map.t)
    (filel : Relative_path.t list) : Fanout.t * int =
  let all_defs =
    List.fold filel ~init:Decl_compare.VersionedNames.empty ~f:(fun acc fn ->
        Relative_path.Map.find defs_per_file fn
        |> Decl_compare.VersionedNames.merge acc)
  in
  (* Fetching everything at once is faster *)
  let (old_funs, old_types, old_consts, old_modules) =
    let { File_info.n_classes = _; n_funs; n_types; n_consts; n_modules } =
      all_defs.Decl_compare.VersionedNames.old_names
    in
    match Provider_backend.get () with
    | Provider_backend.Rust_provider_backend be ->
      let non_class_defs =
        {
          File_info.n_funs;
          n_classes = S_set.empty;
          n_types;
          n_consts;
          n_modules;
        }
      in
      let (_classes, old_funs, old_types, old_consts, old_modules) =
        Rust_provider_backend.Decl.get_old_defs be non_class_defs
      in
      (old_funs, old_types, old_consts, old_modules)
    | _ ->
      ( Decl_heap.Funs.get_old_batch n_funs,
        Decl_heap.Typedefs.get_old_batch n_types,
        Decl_heap.GConsts.get_old_batch n_consts,
        Decl_heap.Modules.get_old_batch n_modules )
  in
  let { Decl_compare.VersionedFileInfo.Diff.funs; types; gconsts; modules } =
    Decl_compare.VersionedFileInfo.diff_names all_defs
  in
  let acc = Fanout.empty in
  let (acc, old_funs_missing) =
    compare_funs_and_get_fanout ctx old_funs acc funs
  in
  let (acc, old_types_missing) =
    compare_types_and_get_fanout ctx old_types acc types
  in
  let (acc, old_gconsts_missing) =
    compare_gconsts_and_get_fanout ctx old_consts acc gconsts
  in
  let (acc, old_modules_missing) =
    compare_modules_and_get_fanout ctx old_modules acc modules
  in
  let old_decl_missing_count =
    old_funs_missing
    + old_types_missing
    + old_gconsts_missing
    + old_modules_missing
  in
  (acc, old_decl_missing_count)

(*****************************************************************************)
(* Load the environment and then redeclare *)
(*****************************************************************************)

(** Invalidate local caches and compute decls in files. Return the file count. *)
let decl_files_job ctx filel =
  try
    (List.length filel, decl_files ~type_check_testing_hooks:None ctx filel)
  with
  | exn ->
    let e = Exception.wrap exn in
    Printf.printf "Error: %s\n" (Exception.get_ctor_string e);
    Out_channel.flush stdout;
    Exception.reraise e

let load_defs_compare_and_get_fanout ctx _acc (filel : Relative_path.t list) :
    (Fanout.t * int) * int =
  try
    let defs_per_file = OnTheFlyStore.load () in
    let (fanout, old_decl_missing_count) =
      compare_decls_and_get_fanout ctx defs_per_file filel
    in
    ((fanout, List.length filel), old_decl_missing_count)
  with
  | exn ->
    let e = Exception.wrap exn in
    Printf.printf "Error: %s\n" (Exception.get_ctor_string e);
    Out_channel.flush stdout;
    Exception.reraise e

(*****************************************************************************)
(* Merges the results coming back from the different workers *)
(*****************************************************************************)

let merge_on_the_fly files_initial_count files_declared_count (count, ()) () =
  files_declared_count := !files_declared_count + count;
  Server_progress.write_percentage
    ~operation:"declaring"
    ~done_count:!files_declared_count
    ~total_count:files_initial_count
    ~unit:"files"
    ~extra:None;
  ()

let merge_compute_deps
    files_initial_count
    files_computed_count
    ((fanout1, computed_count), old_decl_missing_count1)
    (fanout2, old_decl_missing_count2) =
  files_computed_count := !files_computed_count + computed_count;
  let fanout = Fanout.union fanout1 fanout2 in
  Server_progress.write_percentage
    ~operation:"computing dependencies of"
    ~done_count:!files_computed_count
    ~total_count:files_initial_count
    ~unit:"files"
    ~extra:None;
  (fanout, old_decl_missing_count1 + old_decl_missing_count2)

(*****************************************************************************)
(* The parallel worker *)
(*****************************************************************************)

(** Invalidate local decl caches and recompute decls in files in parallel.
  Compare new and old decls and deduce the fanout.
  Return errors raised during decling, fanout and missing old decl count. *)
let parallel_redecl_compare_and_get_fanout
    (ctx : Provider_context.t)
    (workers : Multi_worker.worker list option)
    (bucket_size : int)
    (defs_per_file : Decl_compare.VersionedNames.t Relative_path.Map.t)
    (fnl : Relative_path.t list) : Fanout.t * int =
  Server_progress.with_frame @@ fun () ->
  try
    OnTheFlyStore.store defs_per_file;
    let files_initial_count = List.length fnl in
    let files_declared_count = ref 0 in
    let t = Unix.gettimeofday () in
    Hh_logger.log ~lvl "Declaring on-the-fly %d files" files_initial_count;
    Server_progress.write_percentage
      ~operation:"declaring"
      ~done_count:!files_declared_count
      ~total_count:files_initial_count
      ~unit:"files"
      ~extra:None;
    Multi_worker.call
      workers
      ~job:(fun () -> decl_files_job ctx)
      ~neutral:()
      ~merge:(merge_on_the_fly files_initial_count files_declared_count)
      ~next:(Multi_worker.next ~max_size:bucket_size workers fnl);
    let t = Hh_logger.log_duration ~lvl "Finished declaring on-the-fly" t in
    Hh_logger.log ~lvl "Computing dependencies of %d files" files_initial_count;
    let files_computed_count = ref 0 in
    Server_progress.write_percentage
      ~operation:"computing dependencies of"
      ~done_count:!files_computed_count
      ~total_count:files_initial_count
      ~unit:"files"
      ~extra:None;
    let (fanout, old_decl_missing_count) =
      Multi_worker.call
        workers
        ~job:(load_defs_compare_and_get_fanout ctx)
        ~neutral:compute_deps_neutral
        ~merge:(merge_compute_deps files_initial_count files_computed_count)
        ~next:(Multi_worker.next ~max_size:bucket_size workers fnl)
    in
    let (_t : float) =
      Hh_logger.log_duration ~lvl "Finished computing dependencies" t
    in
    OnTheFlyStore.clear ();
    (fanout, old_decl_missing_count)
  with
  | exn ->
    let e = Exception.wrap exn in
    if Shared_mem.SMTelemetry.is_heap_overflow () then
      Exit.exit Exit_status.Redecl_heap_overflow
    else
      Exception.reraise e

(*****************************************************************************)
(* Code invalidating the heap *)
(*****************************************************************************)

(** Oldify provided defs and elements (a.k.a. members).
    This is equivalent to moving a the decls for those defs to some heap of old decls,
    i.e. they will only be retrievable when querying old values.
    For classes, it oldifies both shallow and folded classes *)
let[@warning "-21"] oldify_defs (* -21 for dune stubs *)
    (ctx : Provider_context.t)
    ({ File_info.n_funs; n_classes; n_types; n_consts; n_modules } as names)
    (elems : Decl_class_elements.t S_map.t)
    ~(collect_garbage : bool) : unit =
  match Provider_backend.get () with
  | Provider_backend.Rust_provider_backend be ->
    Rust_provider_backend.Decl.oldify_defs be names
  | _ ->
    Decl_heap.Funs.oldify_batch n_funs;
    Decl_class_elements.oldify_all elems;
    Decl_heap.Classes.oldify_batch n_classes;
    Old_shallow_classes_provider.oldify_batch ctx n_classes;
    Decl_heap.Typedefs.oldify_batch n_types;
    Decl_heap.GConsts.oldify_batch n_consts;
    Decl_heap.Modules.oldify_batch n_modules;
    if collect_garbage then Shared_mem.GC.collect `gentle;
    ()

let[@warning "-21"] remove_old_defs (* -21 for dune stubs *)
    (ctx : Provider_context.t)
    ({ File_info.n_funs; n_classes; n_types; n_consts; n_modules } as names)
    (elems : Decl_class_elements.t S_map.t) : unit =
  match Provider_backend.get () with
  | Provider_backend.Rust_provider_backend be ->
    Rust_provider_backend.Decl.remove_old_defs be names
  | _ ->
    Decl_heap.Funs.remove_old_batch n_funs;
    Decl_class_elements.remove_old_all elems;
    Decl_heap.Classes.remove_old_batch n_classes;
    Old_shallow_classes_provider.remove_old_batch ctx n_classes;
    Decl_heap.Typedefs.remove_old_batch n_types;
    Decl_heap.GConsts.remove_old_batch n_consts;
    Decl_heap.Modules.remove_old_batch n_modules;
    Shared_mem.GC.collect `gentle;
    ()

(** Remove provided defs and elements from the heap of current decls.
    For classes, it removes both shallow and folded classes.

    @param elems  elements, a.k.a. members, to remove *)
let[@warning "-21"] remove_defs (* -21 for dune stubs *)
    ({ File_info.n_funs; n_classes; n_types; n_consts; n_modules } as names)
    ~(elems : Decl_class_elements.t S_map.t)
    ~(collect_garbage : bool) : unit =
  match Provider_backend.get () with
  | Provider_backend.Rust_provider_backend be ->
    Rust_provider_backend.Decl.remove_defs be names
  | _ ->
    Decl_heap.Funs.remove_batch n_funs;
    Decl_class_elements.remove_all elems;
    Decl_heap.Classes.remove_batch n_classes;
    Shallow_classes_heap.Classes.remove_batch n_classes;
    Decl_heap.Typedefs.remove_batch n_types;
    Decl_heap.GConsts.remove_batch n_consts;
    Decl_heap.Modules.remove_batch n_modules;
    if collect_garbage then Shared_mem.GC.collect `gentle;
    ()

(** [is_descendant_of_any_of classes c] returns whether
    class [c] is a member of or is a descendant of
    any of the [classes]. *)
let is_descendant_of_any_of classes (c : string) : bool =
  if S_set.mem c classes then
    true
  else
    match Decl_heap.Classes.get c with
    | None -> false
    (* it might be a descendant class, but we are only doing this
     * check for the purpose of invalidating things from the heap
     * - if it's already not there, then we don't care. *)
    | Some c ->
      let intersection_nonempty s1 s2 =
        S_set.exists (fun x -> S_set.mem x s2) s1
      in
      S_map.exists (fun c _ -> S_set.mem c classes) c.Decl_defs.dc_ancestors
      || intersection_nonempty c.Decl_defs.dc_extends classes
      || intersection_nonempty c.Decl_defs.dc_xhp_attr_deps classes
      || intersection_nonempty c.Decl_defs.dc_req_ancestors_extends classes

(** [add_classes_in_files get_classes files classes_acc]
    adds to [classes_acc] the classes in [files],
    obtained with [get_classes file] for [file] in [files] *)
let add_classes_in_files
    (get_classes : Relative_path.t -> S_set.t)
    (files : Relative_path.Set.t)
    (classes_acc : S_set.t) : S_set.t =
  Relative_path.Set.fold files ~init:classes_acc ~f:(fun fn acc ->
      S_set.union acc @@ get_classes fn)

(** Return the files containing all descendants of provided classes *)
let get_files_of_descendants (ctx : Provider_context.t) (class_names : S_set.t)
    : Relative_path.Set.t =
  S_set.fold
    (fun c acc ->
      Typing_deps.get_extend_deps
        ~mode:(Provider_context.get_deps_mode ctx)
        ~visited:(VisitedSet.make ())
        ~source_class:(Dep.make (Dep.Type c))
        ~acc)
    class_names
    Typing_deps.(DepSet.make ())
  |> Naming_provider.get_files ctx

(** [filter_descendant_classes classes ~maybe_descendant_classes]
    filters [maybe_descendant_classes] to keep only those which are
    a descendant or a class in [classes], or a member of [classes]. *)
let filter_descendant_classes
    (classes : S_set.t) ~(maybe_descendant_classes : string list) : string list
    =
  List.filter maybe_descendant_classes ~f:(is_descendant_of_any_of classes)

module ClassSetStore = Global_storage.Make (struct
  type t = S_set.t
end)

let load_and_filter_descendant_classes (maybe_descendant_classes : string list)
    : string list * int =
  let classes = ClassSetStore.load () in
  ( filter_descendant_classes classes ~maybe_descendant_classes,
    List.length maybe_descendant_classes )

let merge_descendant_classes
    classes_initial_count
    classes_filtered_count
    (descendant_classes, filtered)
    acc =
  classes_filtered_count := !classes_filtered_count + filtered;
  Server_progress.write_percentage
    ~operation:"filtering"
    ~done_count:!classes_filtered_count
    ~total_count:classes_initial_count
    ~unit:"classes"
    ~extra:None;
  descendant_classes @ acc

let filter_descendant_classes_parallel
    (workers : Multi_worker.worker list option)
    ~(bucket_size : int)
    (classes : S_set.t)
    (maybe_descendant_classes : string list) : string list =
  let classes_initial_count = List.length maybe_descendant_classes in
  if classes_initial_count < 10 then
    filter_descendant_classes classes ~maybe_descendant_classes
  else
    Server_progress.with_frame @@ fun () ->
    ClassSetStore.store classes;
    let classes_filtered_count = ref 0 in
    let t = Unix.gettimeofday () in
    Hh_logger.log ~lvl "Filtering %d descendant classes" classes_initial_count;
    Server_progress.write_percentage
      ~operation:"filtering"
      ~done_count:!classes_filtered_count
      ~total_count:classes_initial_count
      ~unit:"classes"
      ~extra:None;
    let res =
      Multi_worker.call
        workers
        ~job:(fun _ c -> load_and_filter_descendant_classes c)
        ~merge:
          (merge_descendant_classes
             classes_initial_count
             classes_filtered_count)
        ~neutral:[]
        ~next:
          (Multi_worker.next
             ~max_size:bucket_size
             workers
             maybe_descendant_classes)
    in
    let (_t : float) =
      Hh_logger.log_duration ~lvl "Finished filtering descendant classes" t
    in
    ClassSetStore.clear ();
    res

let get_descendant_classes
    (ctx : Provider_context.t)
    (workers : Multi_worker.worker list option)
    ~(bucket_size : int)
    (get_classes : Relative_path.t -> S_set.t)
    (classes : S_set.t) : S_set.t =
  let files_of_descendants = get_files_of_descendants ctx classes in
  add_classes_in_files get_classes files_of_descendants classes
  |> S_set.elements
  |> filter_descendant_classes_parallel workers ~bucket_size classes
  |> S_set.of_list

let merge_elements
    classes_initial_count classes_processed_count (elements, count) acc =
  classes_processed_count := !classes_processed_count + count;

  let acc = S_map.union elements acc in
  Server_progress.write_percentage
    ~operation:"getting members of"
    ~done_count:!classes_processed_count
    ~total_count:classes_initial_count
    ~unit:"classes"
    ~extra:None;
  acc

(** Get the [Decl_class_elements.t]s (a.k.a. members)
  corresponding to the classes contained in [defs].
  Get them from the element heaps. *)
let get_elems
    (workers : Multi_worker.worker list option)
    ~(bucket_size : int)
    ~(old : bool)
    (defs : File_info.names) : Decl_class_elements.t S_map.t =
  let classes = S_set.elements defs.File_info.n_classes in
  (* Getting the members of a class requires fetching the class from the heap.
   * Doing this for too many classes will cause a large amount of allocations
   * to be performed on the master process triggering the GC and slowing down
   * redeclaration. Using the workers prevents this from occurring
   *)
  let classes_initial_count = List.length classes in
  let t = Unix.gettimeofday () in
  Hh_logger.log ~lvl "Getting elements of %d classes" classes_initial_count;
  let elements =
    if classes_initial_count < 10 then
      Decl_class_elements.get_for_classes ~old classes
    else
      Server_progress.with_frame @@ fun () ->
      let classes_processed_count = ref 0 in
      Server_progress.write_percentage
        ~operation:"getting members of"
        ~done_count:!classes_processed_count
        ~total_count:classes_initial_count
        ~unit:"classes"
        ~extra:None;
      Multi_worker.call
        workers
        ~job:(fun _ c ->
          (Decl_class_elements.get_for_classes ~old c, List.length c))
        ~merge:(merge_elements classes_initial_count classes_processed_count)
        ~neutral:S_map.empty
        ~next:(Multi_worker.next ~max_size:bucket_size workers classes)
  in

  let (_t : float) =
    Hh_logger.log_duration ~lvl "Finished getting elements" t
  in
  elements

let invalidate_folded_classes
    ctx
    workers
    ~bucket_size
    ~get_classes_in_file
    (changed_classes : Shallow_class_fanout.changed_class list) : unit =
  let to_invalidate =
    List.fold
      changed_classes
      ~init:S_set.empty
      ~f:(fun acc { Shallow_class_fanout.descendant_deps; _ } ->
        Shallow_class_fanout.class_names_from_deps
          ~ctx
          ~get_classes_in_file
          descendant_deps
        |> S_set.union acc)
  in
  Hh_logger.log "Invalidating %d folded classes" (S_set.cardinal to_invalidate);
  let (old_members, new_members) =
    let get_elems n_classes =
      get_elems workers ~bucket_size File_info.{ empty_names with n_classes }
    in
    ( lazy (get_elems to_invalidate ~old:true),
      lazy (get_elems to_invalidate ~old:false) )
  in
  Decl_provider.remove_classes ctx to_invalidate ~old_members ~new_members;
  Shared_mem.invalidate_local_caches ();
  Shared_mem.GC.collect `gentle;
  ()

(*****************************************************************************)
(* The main entry point *)
(*****************************************************************************)

(** see .mli **)
let redo_type_decl
    (ctx : Provider_context.t)
    ~during_init
    (workers : Multi_worker.worker list option)
    ~(type_check_testing_hooks : Server_env.type_check_testing_hooks option)
    ~(bucket_size : int)
    (get_classes : Relative_path.t -> S_set.t)
    ~(previously_oldified_defs : File_info.names)
    ~(defs : Decl_compare.VersionedNames.t Relative_path.Map.t) :
    redo_type_decl_result =
  Hh_logger.log "Decl_redecl_service.redo_type_decl: oldifying defs";
  let all_defs =
    Relative_path.Map.fold
      defs
      ~init:Decl_compare.VersionedNames.empty
      ~f:(fun _fn names acc -> Decl_compare.VersionedNames.merge names acc)
  in
  let all_old_defs = all_defs.Decl_compare.VersionedNames.old_names in
  (* Some of the definitions are already in the old heap, left there by a
   * previous lazy check *)
  let (oldified_defs, current_defs) =
    Decl_utils.split_defs all_old_defs previously_oldified_defs
  in
  (* Oldify the remaining defs along with their elements *)
  let get_elems = get_elems workers ~bucket_size in
  let current_elems = get_elems current_defs ~old:false in
  oldify_defs ctx current_defs current_elems ~collect_garbage:true;

  (* Fetch the already oldified elements too so we can remove them later *)
  let oldified_elems = get_elems oldified_defs ~old:true in
  let all_elems = S_map.union current_elems oldified_elems in
  let fnl = Relative_path.Map.keys defs in

  Hh_logger.log
    "Decl_redecl_service.redo_type_decl: computing fanout of non-classes";
  let (fanout_acc, old_decl_missing_count) =
    (* If there aren't enough files, let's do this ourselves ... it's faster! *)
    if List.length fnl < 10 then
      let () = decl_files ~type_check_testing_hooks ctx fnl in
      let (fanout, old_decl_missing_count) =
        compare_decls_and_get_fanout ctx defs fnl
      in
      (fanout, old_decl_missing_count)
    else
      let (fanout, old_decl_missing_count) =
        parallel_redecl_compare_and_get_fanout ctx workers bucket_size defs fnl
      in
      let fanout =
        reconcile_parallel_redecl_fanout
          ~during_init
          ~deps_mode:(Provider_context.get_deps_mode ctx)
          fanout
      in
      (fanout, old_decl_missing_count)
  in
  Hh_logger.log
    "Decl_redecl_service.redo_type_decl: computing fanout of classes";
  let fanout =
    let changes =
      Shallow_decl_compare.compute_changes
        ctx
        ~during_init
        ~class_names:
          (Decl_compare.VersionedSSet.get_classes all_defs
          |> Decl_compare.VersionedSSet.diff)
    in
    invalidate_folded_classes
      ctx
      workers
      ~bucket_size
      ~get_classes_in_file:get_classes
      changes;
    let fanout = Shallow_class_fanout.fanout_of_changes ~ctx changes in
    Fanout.union fanout fanout_acc
  in
  remove_old_defs ctx all_old_defs all_elems;

  Hh_logger.log "Finished recomputing type declarations:";
  Hh_logger.log
    "  changed defs count: %d"
    (DepSet.cardinal fanout.Fanout.changed);
  Hh_logger.log
    "  defs to recheck count: %d"
    (DepSet.cardinal fanout.Fanout.to_recheck);

  { fanout; old_decl_missing_count }

(** see .mli **)
let oldify_decls_and_remove_descendants
    (ctx : Provider_context.t)
    ?(collect_garbage = true)
    (workers : Multi_worker.worker list option)
    (get_classes : Relative_path.t -> S_set.t)
    ~(bucket_size : int)
    ~(defs : File_info.names) : unit =
  let elems = get_elems workers ~bucket_size defs ~old:false in
  oldify_defs ctx defs elems ~collect_garbage;

  (* Oldifying/removing classes also affects their elements
   * (see Decl_class_elements), which might be shared with other classes. We
   * need to remove all of them too to avoid dangling references *)
  let all_classes = defs.File_info.n_classes in
  let descendant_classes =
    get_descendant_classes ctx workers get_classes ~bucket_size all_classes
  in
  let descendant_classes =
    File_info.
      { empty_names with n_classes = S_set.diff descendant_classes all_classes }
  in
  (* This path exists to invalidate folded/member state derived from oldified
   * classes. [remove_defs] also removes descendant shallow decls, but that is
   * likely over-broad: a descendant's shallow decl is a direct function of its
   * own file, not of the oldified parent. If we determine there to be
   * performance impact of this consider correcting it. *)
  remove_defs descendant_classes ~elems:S_map.empty ~collect_garbage

let remove_old_defs
    (ctx : Provider_context.t)
    ~(bucket_size : int)
    (workers : Multi_worker.worker list option)
    (names : File_info.names) : unit =
  let elems = get_elems workers ~bucket_size names ~old:true in
  remove_old_defs ctx names elems
