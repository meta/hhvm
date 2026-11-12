(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude

(* [Common_setup] lays down Foo.php, which references [Bar], [A], [B] and [D]
   from Bar.php and names [Foo] again in `f1`. Cases needing their own symbols
   pass [extra_files]; [server_tests] pins the decls these two produce. *)
let setup ?(extra_files = []) () =
  Common_setup.setup
    ~sqlite:false
    ~extra_files
    Global_options.default
    ~xhp_as:`Namespaces

(* AttrUser names MyAttr only in the attribute. *)
let attribute_files =
  [
    ("AttrDef.php", {|<?hh
class MyAttr {}
|});
    ("AttrUser.php", {|<?hh
<<MyAttr>>
class AttrUser {}
|});
  ]

(* MemberUser reaches MemberTarget only through a method call on a value whose
   class it never names, and names MemberFactory directly. *)
let member_files =
  [
    ( "MemberTarget.php",
      {|<?hh
class MemberTarget {
  public function ping(): int { return 1; }
}
|}
    );
    ( "MemberFactory.php",
      {|<?hh
function make_target(): MemberTarget { return new MemberTarget(); }
|}
    );
    ( "MemberUser.php",
      {|<?hh
function use_it(): int { return make_target()->ping(); }
|} );
  ]

let test_edges_point_at_the_referenced_file () =
  Server_isolation_outbound.reset ();
  let { Common_setup.ctx; foo_path; bar_path; _ } = setup () in
  let edges = Server_isolation_outbound.get ctx foo_path in
  Relative_path.Set.mem edges bar_path
  && not (Relative_path.Set.mem edges foo_path)

let test_second_request_is_served_from_cache () =
  Server_isolation_outbound.reset ();
  let { Common_setup.ctx; foo_path; _ } = setup () in
  let first = Server_isolation_outbound.get ctx foo_path in
  let after_one = Server_isolation_outbound.files_indexed () in
  let second = Server_isolation_outbound.get ctx foo_path in
  let after_two = Server_isolation_outbound.files_indexed () in
  (* The count is what distinguishes a cache hit from a recomputation that
     happens to agree. *)
  Int.equal after_one 1
  && Int.equal after_two 1
  && Relative_path.Set.equal first second

let test_reset_drops_the_cache () =
  Server_isolation_outbound.reset ();
  let { Common_setup.ctx; foo_path; _ } = setup () in
  ignore (Server_isolation_outbound.get ctx foo_path : Relative_path.Set.t);
  Server_isolation_outbound.reset ();
  let cleared = Int.equal (Server_isolation_outbound.files_indexed ()) 0 in
  (* The count going back to 1 is what shows the heap entry was dropped too: had
     it survived, this [get] would hit the cache and never increment. *)
  ignore (Server_isolation_outbound.get ctx foo_path : Relative_path.Set.t);
  cleared && Int.equal (Server_isolation_outbound.files_indexed ()) 1

let test_unreadable_file_yields_no_edges () =
  Server_isolation_outbound.reset ();
  let { Common_setup.ctx; nonexistent_path; _ } = setup () in
  let edges = Server_isolation_outbound.get ctx nonexistent_path in
  (* Emptiness alone would hold for a file that parsed to nothing, so pin that
     the path was indexed and the empty result cached. *)
  let indexed_once = Int.equal (Server_isolation_outbound.files_indexed ()) 1 in
  let again = Server_isolation_outbound.get ctx nonexistent_path in
  Relative_path.Set.is_empty edges
  && indexed_once
  && Relative_path.Set.is_empty again
  && Int.equal (Server_isolation_outbound.files_indexed ()) 1

let test_attribute_only_reference_yields_an_edge () =
  Server_isolation_outbound.reset ();
  let { Common_setup.ctx; _ } = setup ~extra_files:attribute_files () in
  let user_path = Relative_path.from_root ~suffix:"AttrUser.php" in
  let def_path = Relative_path.from_root ~suffix:"AttrDef.php" in
  (* This edge exists only if [Attribute] occurrences are followed. Asserting the
     whole set, not just membership, so an over-inclusive filter fails here too. *)
  let edges = Server_isolation_outbound.get ctx user_path in
  Relative_path.Set.equal edges (Relative_path.Set.singleton def_path)

let test_member_only_reference_yields_no_edge () =
  Server_isolation_outbound.reset ();
  let { Common_setup.ctx; _ } = setup ~extra_files:member_files () in
  let user_path = Relative_path.from_root ~suffix:"MemberUser.php" in
  let factory_path = Relative_path.from_root ~suffix:"MemberFactory.php" in
  let target_path = Relative_path.from_root ~suffix:"MemberTarget.php" in
  let edges = Server_isolation_outbound.get ctx user_path in
  (* Deliberate: MemberTarget is a real dependency, but no package rule can
     reject a method call. *)
  Relative_path.Set.mem edges factory_path
  && not (Relative_path.Set.mem edges target_path)

let test_module_membership_yields_no_edge () =
  Server_isolation_outbound.reset ();
  let { Common_setup.ctx; foo_path; bar_path; _ } = setup () in
  (* Bar.php reaches Foo.php only through `module foo;`, declared in Foo.php. The
     first case walks this fixture the other way, so an empty result here is not
     resolution silently failing. *)
  let edges = Server_isolation_outbound.get ctx bar_path in
  not (Relative_path.Set.mem edges foo_path)

let test_ensure_indexed_warms_the_cache () =
  Server_isolation_outbound.reset ();
  let { Common_setup.ctx; foo_path; bar_path; _ } = setup () in
  Server_isolation_outbound.ensure_indexed
    ~ctx
    ~workers:None
    [foo_path; bar_path];
  let after_bulk = Server_isolation_outbound.files_indexed () in
  let edges = Server_isolation_outbound.get ctx foo_path in
  (* The count not moving is the assertion: [get] served what the bulk call
     indexed rather than recomputing it. *)
  Int.equal after_bulk 2
  && Int.equal (Server_isolation_outbound.files_indexed ()) 2
  && Relative_path.Set.mem edges bar_path

let test_release_cache_keeps_the_count () =
  Server_isolation_outbound.reset ();
  let { Common_setup.ctx; foo_path; _ } = setup () in
  ignore (Server_isolation_outbound.get ctx foo_path : Relative_path.Set.t);
  Server_isolation_outbound.release_cache ();
  (* This is the whole difference from [reset]: entries go, the tally stays. The
     count is cumulative work over a run, not what is currently resident, so it
     survives a release and the next request recomputes and adds to it. *)
  let kept = Int.equal (Server_isolation_outbound.files_indexed ()) 1 in
  ignore (Server_isolation_outbound.get ctx foo_path : Relative_path.Set.t);
  kept && Int.equal (Server_isolation_outbound.files_indexed ()) 2

let test_memory_guard_stops_a_run_over_its_share () =
  let gib n = n * 1024 * 1024 * 1024 in
  let total = gib 200 in
  let reason ~max_used_percent ~available_at_start available =
    Server_isolation_memory.reason
      (Server_isolation_memory.guard
         ~min_free_percent:15
         ~max_used_percent
         ~available_at_start)
      ~available:(gib available)
      ~total
  in
  let from_150 =
    reason ~max_used_percent:25 ~available_at_start:(Some (gib 150))
  in
  (* A quarter of 200 GiB is 50 GiB, and the floor is 30 GiB available. *)
  Option.is_none (from_150 120)
  && Option.is_none (from_150 100)
  && Option.is_some (from_150 99)
  && Option.is_some (from_150 29)
  (* 0 turns the share off, and the floor still holds. *)
  && Option.is_none
       (reason ~max_used_percent:0 ~available_at_start:(Some (gib 150)) 60)
  && Option.is_some
       (reason ~max_used_percent:0 ~available_at_start:(Some (gib 150)) 29)
  (* A host that did not report its memory at the start has no share to hold. *)
  && Option.is_none (reason ~max_used_percent:25 ~available_at_start:None 60)

let tests =
  [
    ( "test_memory_guard_stops_a_run_over_its_share",
      test_memory_guard_stops_a_run_over_its_share );
    ( "test_edges_point_at_the_referenced_file",
      test_edges_point_at_the_referenced_file );
    ("test_ensure_indexed_warms_the_cache", test_ensure_indexed_warms_the_cache);
    ("test_release_cache_keeps_the_count", test_release_cache_keeps_the_count);
    ( "test_attribute_only_reference_yields_an_edge",
      test_attribute_only_reference_yields_an_edge );
    ( "test_member_only_reference_yields_no_edge",
      test_member_only_reference_yields_no_edge );
    ( "test_module_membership_yields_no_edge",
      test_module_membership_yields_no_edge );
    ( "test_second_request_is_served_from_cache",
      test_second_request_is_served_from_cache );
    ("test_reset_drops_the_cache", test_reset_drops_the_cache);
    ( "test_unreadable_file_yields_no_edges",
      test_unreadable_file_yields_no_edges );
  ]

let () =
  let config =
    Shared_mem.
      {
        global_size = 1024;
        (* Each case re-runs [Common_setup], and decls accumulate in the same
           shared memory across all of them. The 8KB other unit tests use is
           enough for two fixture files but not for the extra ones here. *)
        heap_size = 1024 * 1024 * 8;
        hash_table_pow = 14;
        shm_dirs = [];
        shm_use_sharded_hashtbl = false;
        shm_cache_size = -1;
        shm_min_avail = 0;
        log_level = 0;
        sample_rate = 0.0;
        compression = 0;
      }
  in
  (* Symbol identification logs events on the way through. Without a logger the
     whole computation raises, which this module would then quietly report as a
     file with no outbound edges. *)
  Event_logger.init_fake ();
  let (_ : Shared_mem.handle) = Shared_mem.init config ~num_workers:0 in
  tests
  |> List.map ~f:(fun (name, do_) ->
         ( name,
           fun () ->
             Utils.with_context
               ~enter:Provider_backend.set_shared_memory_backend
               ~exit:(fun () -> ())
               ~do_ ))
  |> Unit_test.run_all
