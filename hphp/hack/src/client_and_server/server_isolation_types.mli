(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

(** A group of files that collectively form a leaf: they reference the monorepo,
    but nothing outside the group references them. *)
type cluster = {
  files: string list;
  common_directory: string;
      (** Longest directory prefix shared by every file in the cluster. *)
  truncated: bool;
      (** Whether a size cap refused this cluster a file it would otherwise have
          taken, leaving it a prefix of a package rather than a package. *)
}

(** Whether a named set of files can move as it stands, judged from the
    dependency graph alone. [Blocked] lists every file outside the set that
    references into it. [Movable] does not promise that a typecheck of the
    moved files will pass. *)
type movability =
  | Movable
  | Blocked of string list

type result = {
  refused: string option;
  movability: movability option;
      (** The answer to [Check_movability]; [None] in the other modes. *)
  grown: bool;
      (** Whether these were grown, or are each the smallest isolatable set
          around a starting file. *)
  total_seeds: int;
  total_isolatable_files: int;
  total_clusters: int;
  total_truncated: int;
      (** A count, because the clusters themselves never come back here. *)
  total_refused: int;
      (** Starting files left out of the clusters because no set could be found
          for them: what depends on them is over the limit, or they declare
          nothing. Only the default mode counts them. *)
  largest_cluster: int;
}

(** Where a run's clusters end up. They never travel in the result: a run can
    produce hundreds of thousands, so they go to a file as they are found and
    the result carries only the summary.

    Which file depends on what the caller asked for: [--isolation-output] names
    one, and for [--json] without it the client makes a temporary file, prints
    it and removes it — so reaching stdout costs no more memory than reaching a
    file. *)
type clusters_destination =
  | To_file of string
      (** Written here as they are produced, one JSON object per line. *)
  | Dropped  (** Counted in the summary and forgotten. *)

(** What a run does with its starting files. *)
type mode =
  | Measure
      (** Report the smallest isolatable set around each starting file: its
          closure under reverse dependencies. *)
  | Grow  (** Grow a bigger cluster around each starting file. *)
  | Check_movability
      (** Ask whether the starting files, taken together, can move as they
          stand, naming what blocks it if not. *)
  | Move_to of string
      (** Move the seed directory into this directory, under its own name, if it
          can move and the move introduces no errors. The client does the
          moving, and asks the server the [Check_movability] question. *)

(** Whether the mode grows clusters. *)
val is_grow : mode -> bool

(** Whether the mode asks if the starting files can move. *)
val is_check_movability : mode -> bool

(** Where to start growing clusters, and how far to take them. *)
type options = {
  mode: mode;
  clusters_destination: clusters_destination;
  seed_framework: string option;
      (** Start from every subclass of this class. *)
  seed_list: string option;
      (** Start from the repo-relative paths in this file, one per line. *)
  seed_dir: string option;
      (** Start from every Hack file under this repo-relative directory. *)
  max_dependents: int;
      (** The most files that may depend on a candidate for it to be absorbed
          along with them — how far the closure rule may reach. At least 1. *)
  max_cluster_size: int option;
      (** Stop growing a cluster at this many files and never report a larger
          one, marking it [truncated]. [None] grows to a fixed point. *)
  batch_size: int;
      (** How many clusters to grow at once. They are all held in memory until
          the batch ends, so this sets the run's peak memory. It also changes
          what comes back: clusters in one batch are grown against the same
          [claimed] snapshot, so a larger batch drops more of them for
          overlapping. At least 1. *)
  min_free_memory_percent: int;
      (** Stop the run when less than this percentage of the machine's memory is
          available, whoever is using the rest. 0 turns the check off. *)
  max_memory_percent: int;
      (** Stop the run when the memory available on the machine has fallen by
          more than this percentage of its total since the run began. 0 turns
          the check off. *)
  allow_long_run: bool;
      (** Run even when the seed count says this will take hours. Without it a
          run that long is refused before it starts. *)
  max_seeds: int option;  (** [None] means every seed. *)
  seed_offset: int;
      (** Skip this many seeds before applying [max_seeds], so a caller can walk
          the seed list in chunks across separate queries. *)
}
