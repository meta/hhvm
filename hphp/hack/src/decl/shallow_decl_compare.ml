(**
 * Copyright (c) 2019, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude
open Class_diff
open Shallow_decl_defs
module VersionedSSet = Decl_compare.VersionedSSet
module Dep = Typing_deps.Dep

let diff_class_in_changed_file
    ~(enable_annotation_agnostic_decl_diffing : bool)
    (old_classes : shallow_class option S_map.t)
    (new_classes : shallow_class option S_map.t)
    (class_name : string) : Class_diff.t option =
  let old_class_opt = S_map.find class_name old_classes in
  let new_class_opt = S_map.find class_name new_classes in
  match (old_class_opt, new_class_opt) with
  | (Some old_class, Some new_class) ->
    Shallow_class_diff.diff_class
      ~enable_annotation_agnostic_decl_diffing
      old_class
      new_class
  | (None, None) -> Some (Major_change (MajorChange.Unknown Neither_found))
  | (None, Some _) ->
    Some (Major_change (MajorChange.Unknown Old_decl_not_found))
  | (Some _, None) ->
    Some (Major_change (MajorChange.Unknown New_decl_not_found))

let compute_class_diffs
    (ctx : Provider_context.t) ~during_init ~(class_names : VersionedSSet.diff)
    : (string * Class_diff.t) list =
  let enable_annotation_agnostic_decl_diffing =
    Typechecker_options.enable_annotation_agnostic_decl_diffing
      (Provider_context.get_tcopt ctx)
  in
  let { VersionedSSet.added; kept; removed } = class_names in
  let acc = [] in
  let acc =
    S_set.fold
      (fun name acc -> (name, Major_change MajorChange.Added) :: acc)
      added
      acc
  in
  let acc =
    S_set.fold
      (fun name acc -> (name, Major_change MajorChange.Removed) :: acc)
      removed
      acc
  in
  let old_classes =
    Old_decl_provider.get_old_classes_batch ctx ~during_init kept
  in
  let new_classes =
    S_set.fold
      (fun name acc ->
        S_map.add name (Decl_provider.get_shallow_class ctx name) acc)
      kept
      S_map.empty
  in
  S_set.fold
    (fun cid acc ->
      let diff =
        diff_class_in_changed_file
          ~enable_annotation_agnostic_decl_diffing
          old_classes
          new_classes
          cid
      in
      match diff with
      | Some diff -> (cid, diff) :: acc
      | None -> acc)
    kept
    acc

let log_changes (changes : (string * Class_diff.t) list) : unit =
  let change_count = List.length changes in
  if List.is_empty changes then
    Hh_logger.log "No class changes detected"
  else
    Hh_logger.log "Found %d changed classes:" change_count;
  let max = 1000 in
  Hh_logger.log_lazy
  @@ lazy
       (Yojson.Safe.pretty_to_string
          (`Assoc
            [
              ( "diffs",
                `Assoc
                  (List.take changes max
                  |> List.map ~f:(fun (name, diff) ->
                         (name, `String (Class_diff.show diff)))) );
              ("truncated", `Bool (change_count > max));
            ]));
  ()

let compute_changes
    (ctx : Provider_context.t) ~during_init ~(class_names : VersionedSSet.diff)
    : Shallow_class_fanout.changed_class list =
  Hh_logger.log
    "Detecting changes to %d classes"
    (VersionedSSet.diff_cardinal class_names);

  let changes = compute_class_diffs ctx ~during_init ~class_names in
  log_changes changes;
  List.map changes ~f:(fun (class_name, diff) ->
      let class_dep = Dep.make (Dep.Type class_name) in
      let descendant_deps =
        Typing_deps.add_extend_deps
          (Provider_context.get_deps_mode ctx)
          (Typing_deps.DepSet.singleton class_dep)
      in
      {
        Shallow_class_fanout.name = class_name;
        diff;
        dep = class_dep;
        descendant_deps;
      })
