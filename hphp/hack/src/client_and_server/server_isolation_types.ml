(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

type cluster = {
  files: string list;
  common_directory: string;
  truncated: bool;
}

type movability =
  | Movable
  | Blocked of string list

type result = {
  refused: string option;
  movability: movability option;
  grown: bool;
  total_seeds: int;
  total_isolatable_files: int;
  total_clusters: int;
  total_truncated: int;
  total_refused: int;
  largest_cluster: int;
}

type clusters_destination =
  | To_file of string
  | Dropped

type mode =
  | Measure
  | Grow
  | Check_movability
  | Move_to of string

let is_grow = function
  | Grow -> true
  | Measure
  | Check_movability
  | Move_to _ ->
    false

let is_check_movability = function
  | Check_movability
  | Move_to _ ->
    true
  | Measure
  | Grow ->
    false

type options = {
  mode: mode;
  clusters_destination: clusters_destination;
  seed_framework: string option;
  seed_list: string option;
  seed_dir: string option;
  max_dependents: int;
  max_cluster_size: int option;
  batch_size: int;
  allow_long_run: bool;
  max_seeds: int option;
  seed_offset: int;
}
