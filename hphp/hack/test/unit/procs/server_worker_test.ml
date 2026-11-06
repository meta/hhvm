(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *)

open Hh_prelude

let num_workers = 2

let deps_mode = Typing_deps_mode.InMemoryMode None

let run_job ~worker_call worker f input =
  let pending = ref (Some input) in
  let next () =
    match !pending with
    | None -> Bucket.Done
    | Some input ->
      pending := None;
      Bucket.Job input
  in
  worker_call.Multi_worker.f
    (Some [worker])
    ~job:(fun _ input -> Some (f input))
    ~merge:(fun result _ -> result)
    ~neutral:None
    ~next
  |> Option.value_exn

let with_workers ~set_global_provider_backend_in_workers heap_handle f =
  let workers =
    Server_worker.make
      ~longlived_workers:false
      ~set_global_provider_backend_in_workers
      ~nbr_procs:num_workers
      (Gc.get ())
      heap_handle
      ~logging_init:(fun () -> ())
  in
  f workers

let make_rust_context () =
  let backend =
    Hh_server_provider_backend.make
      (Decl_fold_options.from_global_options Global_options.default)
      (Decl_parser_options.from_parser_options Parser_options.default)
  in
  let ctx =
    Provider_context.empty_for_tool
      ~popt:Parser_options.default
      ~tcopt:Global_options.default
      ~backend:(Provider_backend.Rust_provider_backend backend)
      ~deps_mode
  in
  (backend, ctx)

(** Create a naming database and source files without publishing their symbols
    to either reverse naming heap. Lookups must use the database fallback. *)
let make_naming_fixture dir ~suffix ~count =
  let file_infos =
    List.init count ~f:(fun index ->
        let function_name =
          Printf.sprintf "implicit_worker_backend_%s_%d" suffix index
        in
        let file = Relative_path.from_root ~suffix:(function_name ^ ".php") in
        let contents = "<?hh\nfunction " ^ function_name ^ "(): void {}\n" in
        Disk.write_file ~file:(Relative_path.to_absolute file) ~contents;
        let parsed =
          Direct_decl_parser.parse_and_hash_decls
            (Decl_parser_options.from_parser_options Parser_options.default)
            false
            file
            contents
        in
        (file, Direct_decl_parser.decls_to_fileinfo file parsed))
    |> Relative_path.Map.of_list
  in
  let db_path = Path.to_string (Path.concat dir (suffix ^ ".sqlite")) in
  let saved = Naming_table.save (Naming_table.create file_infos) db_path in
  assert (List.is_empty saved.Naming_sqlite.errors);
  (Naming_sqlite.Db_path db_path, file_infos)

(** After creating the worker pool, install the Rust provider backend and set
    its naming table database path in the master. A job dispatched through
    Multi_worker checks that the worker's global backend is Rust and preserves
    the master's naming table path. Naming and Decl_store lookups using the
    database also check that the worker's connection and declaration store work. *)
let test_late_backend_and_naming_path heap_handle dir () =
  Provider_backend.set_shared_memory_backend ();
  let invoked_custom_dispatch = ref false in
  let custom_dispatch =
    {
      Multi_worker.f =
        (fun workers ~job ~merge ~neutral ~next ->
          invoked_custom_dispatch := true;
          Multi_worker.call workers ~job ~merge ~neutral ~next);
    }
  in
  let worker_call = custom_dispatch in
  with_workers
    ~set_global_provider_backend_in_workers:true
    heap_handle
    (fun workers ->
      let (backend, _ctx) = make_rust_context () in
      let (db_path, files) =
        make_naming_fixture dir ~suffix:"late_path" ~count:1
      in
      Rust_provider_backend.Naming.set_db_path backend db_path;
      Provider_backend.set_rust_backend backend;
      let (file, info) = List.hd_exn (Relative_path.Map.bindings files) in
      let symbol = S_set.choose (File_info.simplify info).File_info.n_funs in
      let found =
        run_job
          ~worker_call
          (List.hd_exn workers)
          (fun (db_path, symbol, file) ->
            match Provider_backend.get () with
            | Provider_backend.Rust_provider_backend backend ->
              let actual_db_path =
                Rust_provider_backend.Naming.get_db_path backend
              in
              let path_matches = Poly.(actual_db_path = Some db_path) in
              let decl = (Decl_store.get ()).Decl_store.get_fun symbol in
              let decl_found = Option.is_some decl in
              let pos =
                Rust_provider_backend.Naming.Funs.get_pos backend None symbol
              in
              let file_matches =
                Option.exists pos ~f:(fun pos ->
                    let actual_file = File_info.get_pos_filename pos in
                    Relative_path.equal actual_file file)
              in
              path_matches && decl_found && file_matches
            | Provider_backend.Shared_memory
            | Provider_backend.Pessimised_shared_memory _
            | Provider_backend.Local_memory _
            | Provider_backend.Analysis ->
              false)
          (db_path, symbol, file)
      in
      !invoked_custom_dispatch && found)

let () =
  Daemon.check_entry_point ();
  Server_progress.disable ();
  let config =
    Shared_mem.
      {
        default_config with
        shm_use_sharded_hashtbl = true;
        shm_cache_size = 2 * 1024 * 1024 * 1024;
      }
  in
  let heap_handle = Shared_mem.init config ~num_workers in
  Event_logger.init_fake ();
  Printexc.record_backtrace true;
  let passed =
    Utils.try_finally ~finally:Worker_controller.force_quit_all ~f:(fun () ->
        Tempfile.with_real_tempdir (fun dir ->
            Relative_path.set_path_prefix Relative_path.Root dir;
            Relative_path.set_path_prefix Relative_path.Hhi dir;
            Relative_path.set_path_prefix Relative_path.Tmp dir;
            Unit_test.for_all_non_shortcircuit
              [
                ( "late_backend_and_naming_path",
                  test_late_backend_and_naming_path heap_handle dir );
              ]
              Unit_test.run))
  in
  exit
    (if passed then
      0
    else
      1)
