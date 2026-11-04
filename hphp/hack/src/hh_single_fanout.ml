(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude

type options = {
  debug: bool;
  show_errors: bool;
  config: Config_file_common.t;
}

module Symbol = struct
  type t = Typing_deps.Dep.dependency Typing_deps.Dep.variant

  let compare x y = Typing_deps.Dep.compare_variant x y
end

module SymbolMap = Wrapped_map.Make (Symbol)
module SymbolSet = Stdlib.Set.Make (Symbol)

(** A bidirectional map between file paths and symbol names *)
type naming_table = Naming_table.t * Relative_path.t SymbolMap.t

let deps_mode = Typing_deps_mode.InMemoryMode None

let popt =
  Parser_options.
    { default with disable_xhp_element_mangling = false; everything_sdt = true }

(* `--config` settings are applied on top, so a test can turn on any `.hhconfig`
   key without this binary having to know about it. *)
let tcopt config =
  Global_options.
    {
      default with
      po = popt;
      tco_allow_all_files_for_module_declarations = true;
    }
  |> Server_config.load_config config

let log_section ~name ~content =
  Printf.printf "%s:\n" (String.uppercase name);
  Printf.printf "%s\n" content;
  Printf.printf "\n";
  ()

let parse_defs (ctx : Provider_context.t) (files : Relative_path.Set.t) :
    File_info.t Relative_path.Map.t =
  let workers = None in
  let done_ = ref false in
  Direct_decl_service.(
    go
      ctx
      ~trace:false
      ~decl_mode:
        (* Not caching here, otherwise oldification done in redo_type_decl will
         * oldify the new version (and override the real old versions *)
        Normal
      workers
      ~get_next:(fun () ->
        if !done_ then
          Bucket.Done
        else (
          done_ := true;
          Bucket.Job (Relative_path.Set.elements files)
        )))

let update_reverse_naming_table
    ctx (defs_per_file : File_info.t Relative_path.Map.t) symbols_to_files :
    Relative_path.t SymbolMap.t =
  Relative_path.Map.fold
    defs_per_file
    ~init:symbols_to_files
    ~f:(fun file file_info symbols_to_files ->
      let {
        File_info.ids;
        comments = _;
        file_mode = _;
        position_free_decl_hash = _;
      } =
        file_info
      in
      Naming_global.ndecl_file_skip_if_already_bound ctx file ids;
      let { File_info.funs; classes; typedefs; consts; modules } = ids in
      let add_symbols names make_dep symbols_to_files =
        List.fold names ~init:symbols_to_files ~f:(fun symbols_to_files id ->
            SymbolMap.add (make_dep id.File_info.name) file symbols_to_files)
      in
      symbols_to_files
      |> add_symbols funs (fun name -> Typing_deps.Dep.Fun name)
      |> add_symbols classes (fun name -> Typing_deps.Dep.Type name)
      |> add_symbols typedefs (fun name -> Typing_deps.Dep.Type name)
      |> add_symbols consts (fun name -> Typing_deps.Dep.GConstName name)
      |> add_symbols modules (fun name -> Typing_deps.Dep.Module name))

let make_reverse_naming_table ctx defs_per_file =
  update_reverse_naming_table ctx defs_per_file SymbolMap.empty

(** Does something very similar to ServerTypeCheck.indexing and ServerTypeCheck.do_naming:
  - parse names in files

  - update provided naming table and return the result

  - update reverse naming table, after discarding old entries

  - in addition, update symbol-to-file table
*)
let redecl_make_new_naming_table
    (ctx : Provider_context.t)
    options
    ((old_naming_table, old_symbols_to_files) : naming_table)
    (files_with_changes : Relative_path.Set.t) : naming_table =
  let defs_per_file_parsed = parse_defs ctx files_with_changes in
  let new_naming_table =
    Naming_table.update_many old_naming_table defs_per_file_parsed
  in
  Server_incremental.remove_defs_from_reverse_naming_table
    old_naming_table
    defs_per_file_parsed;
  let symbols_to_files =
    update_reverse_naming_table ctx defs_per_file_parsed old_symbols_to_files
  in
  if options.debug then
    log_section
      ~name:"new naming table"
      ~content:(Naming_table.show new_naming_table);
  (new_naming_table, symbols_to_files)

let get_old_and_new_defs
    options
    (files_with_changes : Relative_path.Set.t)
    (old_naming_table : Naming_table.t)
    (new_naming_table : Naming_table.t) :
    Decl_compare.VersionedNames.t Relative_path.Map.t =
  let defs =
    Server_incremental.get_old_and_new_defs_in_files
      old_naming_table
      new_naming_table
      files_with_changes
  in
  if options.debug then
    Printf.printf
      "The following defs will be diffed for each file:\n%s\n"
      (Relative_path.Map.show Decl_compare.VersionedNames.pp defs);
  defs

let get_symbols_for_deps
    (deps : Typing_deps.DepSet.t)
    (dep_to_symbol_map : _ Typing_deps.Dep.variant Typing_deps.DepMap.t) :
    SymbolSet.t =
  Typing_deps.DepSet.fold deps ~init:SymbolSet.empty ~f:(fun dep acc ->
      let symbol =
        match Typing_deps.DepMap.find_opt dep dep_to_symbol_map with
        | None -> Typing_deps.Dep.Type (Typing_deps.Dep.to_hex_string dep)
        | Some variant -> variant
      in
      SymbolSet.add symbol acc)

let get_files_for_symbols
    (symbols : SymbolSet.t) (symbols_to_files : Relative_path.t SymbolMap.t) :
    Relative_path.Set.t =
  SymbolSet.fold
    (fun symbol files ->
      match SymbolMap.find_opt symbol symbols_to_files with
      | None -> files
      | Some file -> Relative_path.Set.add files file)
    symbols
    Relative_path.Set.empty

let compute_fanout
    ctx
    naming_table
    (old_and_new_defs : Decl_compare.VersionedNames.t Relative_path.Map.t) :
    Fanout.t =
  let { Decl_redecl_service.fanout; _ } =
    Decl_redecl_service.redo_type_decl
      ~type_check_testing_hooks:None
      ctx
      ~during_init:false
      None
      ~bucket_size:500
      (fun file ->
        match Naming_table.get_file_info naming_table file with
        | None -> S_set.empty
        | Some file_info -> (File_info.simplify file_info).File_info.n_classes)
      ~previously_oldified_defs:File_info.empty_names
      ~defs:old_and_new_defs
  in
  fanout

let print_fanout (symbols, files) =
  SymbolSet.iter
    (fun symbol ->
      Printf.printf "%s\n" (Typing_deps.Dep.variant_to_string symbol))
    symbols;
  Relative_path.Set.iter files ~f:(fun file ->
      Printf.printf
        "%s\n"
        (Relative_path.suffix file
        |> Str.split (Str.regexp "--")
        |> List.last_exn));
  Printf.printf "--\n";
  ()

let compute_fanout_and_resolve_deps
    (ctx : Provider_context.t)
    (options : options)
    files_with_changes
    (errors : Diagnostics.t)
    (old_naming_table, old_symbols_to_files)
    (new_naming_table, new_symbols_to_files)
    (dep_to_symbol_map : _ Typing_deps.Dep.variant Typing_deps.DepMap.t) :
    Relative_path.Set.t =
  let (old_and_new_defs : Decl_compare.VersionedNames.t Relative_path.Map.t) =
    get_old_and_new_defs
      options
      files_with_changes
      old_naming_table
      new_naming_table
  in
  let { Fanout.changed; to_recheck; to_recheck_if_errors } =
    compute_fanout ctx new_naming_table old_and_new_defs
  in
  let dependent_on_files =
    Relative_path.Set.fold
      files_with_changes
      ~init:(Typing_deps.DepSet.make ())
      ~f:(fun file depset ->
        let file_dep = Typing_deps.Dep.File file in
        let file_dependents =
          Typing_deps.get_ideps (Provider_context.get_deps_mode ctx) file_dep
        in

        Typing_deps.(DepSet.union depset file_dependents))
  in
  if options.debug then (
    Printf.printf "Hashes of changed:%s\n" (Typing_deps.DepSet.show changed);
    Printf.printf "Hashes to recheck:%s\n" (Typing_deps.DepSet.show to_recheck);
    Printf.printf
      "Hashes to recheck if errors:%s\n"
      (Typing_deps.DepSet.show to_recheck_if_errors);
    ();
    Printf.printf
      "Hashes of dependent on changed file contents:%s\n"
      (Typing_deps.DepSet.show dependent_on_files)
  );
  let to_recheck = Typing_deps.DepSet.union to_recheck dependent_on_files in
  let to_recheck_symbols = get_symbols_for_deps to_recheck dep_to_symbol_map in
  let to_recheck_if_errors_symbols =
    get_symbols_for_deps to_recheck_if_errors dep_to_symbol_map
  in
  let to_recheck =
    (* We need to look up symbols in the new table, in case a symbol has moved
     * to a different file.
     * If a symbol is not found in the new table, well it's just been deleted and
     * does not need recheck. *)
    get_files_for_symbols to_recheck_symbols new_symbols_to_files
  in
  let to_recheck_files_due_to_errors =
    let files_to_recheck_if_errors =
      get_files_for_symbols to_recheck_if_errors_symbols old_symbols_to_files
    in
    let files_with_errors = Diagnostics.get_failed_files errors in
    Relative_path.Set.inter files_to_recheck_if_errors files_with_errors
    |> Server_incremental.add_files_with_stale_errors
         ctx
         ~reparsed:files_with_changes
         errors
  in
  let fanout =
    Relative_path.Set.union to_recheck to_recheck_files_due_to_errors
  in
  print_fanout (to_recheck_symbols, to_recheck_files_due_to_errors);
  fanout

module FileSystem = struct
  (** Add files using the file provider abstraction. *)
  let provide_files_before (files : Multifile.repo) : Relative_path.t list =
    File_provider.local_changes_push_sharedmem_stack ();
    Relative_path.Map.iter files ~f:File_provider.provide_file_for_tests;
    Relative_path.Map.keys files

  (** Update files using the file provider abstraction.
    Update only files which have changed. *)
  let provide_files_after (files, deleted) : Relative_path.Set.t =
    let changed =
      Relative_path.Map.fold
        files
        ~init:Relative_path.Set.empty
        ~f:(fun file_path content changed_files ->
          let old_content = File_provider.get_contents file_path in
          let changed =
            match old_content with
            | None -> true
            | Some old_content -> not (String.equal old_content content)
          in
          if changed then (
            File_provider.provide_file_for_tests file_path content;
            Relative_path.Set.add changed_files file_path
          ) else
            changed_files)
    in
    File_provider.remove_batch deleted;
    Relative_path.Set.union deleted changed
end

let init_paths (hhi_root : Path.t) : unit =
  (* dummy path, not actually used *)
  Relative_path.set_path_prefix Relative_path.Root (Path.make "/");
  Hhi.set_hhi_root_for_unit_test hhi_root;
  Relative_path.set_path_prefix Relative_path.Hhi hhi_root;
  (* dummy path, not actually used *)
  Relative_path.set_path_prefix Relative_path.Tmp (Path.make "tmp");
  ()

module DepToSymbolsMap : sig
  val get :
    unit ->
    Typing_deps.Dep.dependency Typing_deps.Dep.variant Typing_deps.DepMap.t

  val callback :
    Typing_deps.Dep.dependent Typing_deps.Dep.variant ->
    Typing_deps.Dep.dependency Typing_deps.Dep.variant ->
    unit
end = struct
  let map :
      Typing_deps.Dep.dependency Typing_deps.Dep.variant Typing_deps.DepMap.t
      ref =
    ref Typing_deps.DepMap.empty

  let add (dep : _ Typing_deps.Dep.variant) : unit =
    let hash = Typing_deps.Dep.make dep in
    map :=
      Typing_deps.DepMap.add
        hash
        (Typing_deps.Dep.dependency_of_variant dep)
        !map

  let callback
      (dependent : Typing_deps.Dep.dependent Typing_deps.Dep.variant)
      (dependency : Typing_deps.Dep.dependency Typing_deps.Dep.variant) : unit =
    add dependent;
    add dependency

  let get () = !map
end

let make_nast ctx (filename : Relative_path.t) : Nast.program =
  let ast = Ast_provider.get_ast ~full:true ctx filename in
  Naming.program ctx ast

let make_dep_to_symbol_map ctx options (files : Relative_path.Set.t) :
    _ Typing_deps.Dep.variant Typing_deps.DepMap.t =
  let dep_to_symbol_map =
    Typing_deps.DepMap.union
      (DepToSymbolsMap.get ())
      (Dep_hash_to_symbol.from_nasts
         (Relative_path.Set.elements files |> List.map ~f:(make_nast ctx)))
  in
  if options.debug then
    log_section
      ~name:"dep-to-symbol map"
      ~content:
        (Typing_deps.DepMap.show Typing_deps.Dep.pp_variant dep_to_symbol_map);
  dep_to_symbol_map

(** Initialize a number of backend structures and global states necessary
  for the typechecker to function. *)
let init (hhi_root : Path.t) config : Provider_context.t =
  Event_logger.init_fake ();
  let (_ : Shared_mem.handle) =
    Shared_mem.init ~num_workers:0 Shared_mem.default_config
  in
  init_paths hhi_root;
  let tcopt = tcopt config in
  let ctx =
    Provider_context.empty_for_test
      ~popt:tcopt.Global_options.po
      ~tcopt
      ~deps_mode
  in
  Typing_deps.add_dependency_callback
    ~name:"dep_to_symbol"
    DepToSymbolsMap.callback;
  ctx

let commit_dep_edges () : unit =
  Typing_deps.flush_ideps_batch deps_mode
  |> Typing_deps.register_discovered_dep_edges

(** Build and return the naming table and build the reverse naming table as a side-effect. *)
let make_naming_table
    ctx options (defs_per_file : File_info.t Relative_path.Map.t) : naming_table
    =
  let naming_table = Naming_table.create defs_per_file in
  if options.debug then
    log_section ~name:"naming table" ~content:(Naming_table.show naming_table);
  let symbol_to_file_map = make_reverse_naming_table ctx defs_per_file in
  (naming_table, symbol_to_file_map)

(** Type check given files. Create the dependency graph as a side effect. *)
let type_check_make_depgraph ctx options (files : Relative_path.Set.t) :
    Diagnostics.t =
  let errors =
    Relative_path.Set.fold
      files
      ~init:Diagnostics.empty
      ~f:(fun file errors_acc ->
        let full_ast = Ast_provider.get_ast ctx file ~full:true in
        let (errors, _tasts) =
          Typing_check_job.calc_errors_and_tast ctx file ~full_ast
        in
        Diagnostics.merge errors_acc errors)
  in
  if options.debug then (
    Printf.printf "DEPENDENCY GRAPH:\n";
    Typing_deps.dump_current_edge_buffer_in_memory_mode ();
    Printf.printf "\n";
    ()
  );
  commit_dep_edges ();
  if options.show_errors then (
    Printf.printf "Errors in checked files:\n";
    match Diagnostics.get_sorted_diagnostic_list errors with
    | [] -> Printf.printf "No errors\n"
    | errors ->
      Diagnostics.convert_errors_to_string ~include_filename:true errors
      |> List.iter ~f:(fun error -> Printf.printf "%s\n" error)
  );
  errors

(** Build the naming table and typecheck the base file to create the depgraph.
  Only the naming table is returned. The reverse naming table and depgraph are
  produced as side effects. *)
let process_pre_changes ctx options (files : Relative_path.Set.t) :
    Diagnostics.t * naming_table =
  (* TODO builtins and hhi stuff *)
  let defs_per_file = parse_defs ctx files in
  let naming_table = make_naming_table ctx options defs_per_file in
  let errors = type_check_make_depgraph ctx options files in
  (errors, naming_table)

(** Process changed files and return the list of files with changes:
  - make those files available to the typecheck via the file provider
  - clear the AST provider caches *)
let process_changed_files options ((repo : Multifile.repo), deleted) :
    Relative_path.Set.t =
  let files_with_changes = FileSystem.provide_files_after (repo, deleted) in
  Ast_provider.clear_local_cache ();
  Ast_provider.clear_parser_cache ();
  if options.debug then
    log_section
      ~name:"changed files"
      ~content:
        (String.concat
           ~sep:", "
           (Relative_path.Set.elements files_with_changes
           |> List.map ~f:Relative_path.show));
  files_with_changes

(** This takes the path of a file specifying the base and changed versions of
  files as a multifile, and prints the computed fanout for that change to standard output.
  To do so, it typechecks the base version to create a depgraph and naming table,
  then uses those to compute a fanout. *)
let go (test_file : string) options =
  (if not options.debug then Hh_logger.Level.(set_min_level Off));
  Tempfile.with_tempdir @@ fun hhi_root ->
  let ctx = init hhi_root options.config in
  let { Multifile.States.base; changes } = Multifile.States.parse test_file in
  if options.debug then (
    Printf.printf
      "Parsed repo changes: %s\n"
      (Multifile.States.show_repo_changes changes);
    Printf.printf "Processing base repo (naming table, typecheck, depgraph)\n\n";
    ()
  );
  let files =
    FileSystem.provide_files_before base |> Relative_path.Set.of_list
  in
  let (errors, naming_table) = process_pre_changes ctx options files in
  let (_end_repo, _end_naming_table, _errors) =
    List.fold
      changes
      ~init:(base, naming_table, errors)
      ~f:(fun (repo, naming_table, errors) repo_change ->
        if options.debug then Printf.printf "Applying repo change\n";
        let (repo, deleted) =
          Multifile.States.apply_repo_change repo repo_change
        in
        let files_with_changes =
          process_changed_files options (repo, deleted)
        in
        let dep_to_symbol_map =
          make_dep_to_symbol_map ctx options files_with_changes
        in
        let new_naming_table =
          redecl_make_new_naming_table
            ctx
            options
            naming_table
            files_with_changes
        in
        let fanout =
          compute_fanout_and_resolve_deps
            ctx
            options
            files_with_changes
            errors
            naming_table
            new_naming_table
            dep_to_symbol_map
        in
        let errors = type_check_make_depgraph ctx options fanout in
        (repo, new_naming_table, errors))
  in
  ()

let die str =
  let oc = stderr in
  Out_channel.output_string oc str;
  Out_channel.close oc;
  exit 2

let parse_args () : string * options =
  let usage = Printf.sprintf "Usage: %s filename\n" Sys.argv.(0) in
  let fn_ref = ref [] in
  let debug = ref false in
  let show_errors = ref false in
  let config_overrides = ref [] in
  let options =
    [
      ("--debug", Arg.Set debug, "print debug information");
      ( "--show-errors",
        Arg.Set show_errors,
        "print diagnostics from the initial check and each fanout recheck" );
      ( "--config",
        Arg.String (fun s -> config_overrides := s :: !config_overrides),
        "<key=value> set a .hhconfig option" );
    ]
  in
  Arg.parse options (fun fn -> fn_ref := fn :: !fn_ref) usage;
  let config =
    List.fold
      (List.rev !config_overrides)
      ~init:(Config_file_common.empty ())
      ~f:(fun config setting ->
        let overrides = Config_file_common.parse_contents setting in
        Config_file_common.apply_overrides ~config ~overrides ~log_reason:None)
  in
  let options = { debug = !debug; show_errors = !show_errors; config } in
  let files = !fn_ref in
  match files with
  | [file] -> (file, options)
  | _ -> die usage

let () =
  let (test_file, options) = parse_args () in
  go test_file options
