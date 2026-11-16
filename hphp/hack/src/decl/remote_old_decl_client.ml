(*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

(*****************************************************************************)
(* Module for getting old decls remotely. *)
(*****************************************************************************)
open Hh_prelude

let test_fixture_directory = ".hh_remote_old_decls_test"

(** Load fixture files at the same paths supplied to the remote file parser.
    Return every declaration kind; selection belongs to the calling client. *)
let fetch_old_decls_from_test_fixture
    ~(ctx : Provider_context.t) (file_hashes : (Relative_path.t * string) list)
    : Shallow_decl_defs.named_decl list option =
  match (Sys.getenv_opt "HH_TEST_MODE", Sys.getenv_opt "HH_LOCALCONF_PATH") with
  | (Some "1", Some root) ->
    let directory = Filename.concat root test_fixture_directory in
    if Sys.file_exists directory then
      let popt = Provider_context.get_popt ctx in
      let opts = Decl_parser_options.from_parser_options popt in
      let to_named_decl (name, decl, _) =
        let open Shallow_decl_defs in
        match decl with
        | Class decl -> NClass (name, decl)
        | Fun decl -> NFun (name, decl)
        | Typedef decl -> NTypedef (name, decl)
        | Const decl -> NConst (name, decl)
        | Module decl -> NModule (name, decl)
      in
      let parse_file path =
        let fixture = Filename.concat directory (Relative_path.suffix path) in
        if Sys.file_exists fixture then
          let parsed_file =
            Direct_decl_parser.parse_and_hash_decls
              opts
              popt.Parser_options.deregister_php_stdlib
              path
              (Sys_utils.cat fixture)
          in
          List.map parsed_file.Direct_decl_parser.pfh_decls ~f:to_named_decl
        else
          []
      in
      let paths = Relative_path.Set.of_list (List.map file_hashes ~f:fst) in
      Some (List.concat_map (Relative_path.Set.elements paths) ~f:parse_file)
    else
      None
  | _ -> None

let dep_of_named_decl (decl : Shallow_decl_defs.named_decl) : Typing_deps.Dep.t
    =
  let open Shallow_decl_defs in
  let open Typing_deps in
  match decl with
  | NClass (name, _)
  | NTypedef (name, _) ->
    Dep.make (Dep.Type name)
  | NFun (name, _) -> Dep.make (Dep.Fun name)
  | NConst (name, _) -> Dep.make (Dep.GConst name)
  | NModule (name, _) -> Dep.make (Dep.Module name)

module Utils = struct
  let db_path_of_ctx ~(ctx : Provider_context.t) : Naming_sqlite.db_path option
      =
    ctx |> Provider_context.get_backend |> Db_path_provider.get_naming_db_path

  let dep_to_decl_hash_opt
      ~(dep : Typing_deps.Dep.dependent Typing_deps.Dep.variant)
      ~(db_path : Naming_sqlite.db_path) : string option =
    let dep = Typing_deps.Dep.make dep in
    let decl_hash = Naming_sqlite.get_decl_hash_by_64bit_dep db_path dep in
    decl_hash

  let dep_to_file_hash_opt
      ~(dep : Typing_deps.Dep.dependent Typing_deps.Dep.variant)
      ~(db_path : Naming_sqlite.db_path) : (Relative_path.t * string) option =
    let dep = Typing_deps.Dep.make dep in
    let file_hash = Naming_sqlite.get_file_hash_by_64bit_dep db_path dep in
    file_hash
end

let fetch_old_decls_via_file_hashes
    ~(ctx : Provider_context.t)
    ~(db_path : Naming_sqlite.db_path)
    (deps : Typing_deps.Dep.dependent Typing_deps.Dep.variant list) :
    Shallow_decl_defs.named_decl list =
  let add_requested_file (file_hashes, owners) dep =
    let name = Typing_deps.Dep.extract_name dep in
    match Utils.dep_to_file_hash_opt ~dep ~db_path with
    | Some (path, hash) ->
      Hh_logger.log
        "Fetching decl for %s defined in %s from blob %s"
        name
        Relative_path.(suffix path)
        hash;
      ( (path, hash) :: file_hashes,
        Typing_deps.DepMap.add (Typing_deps.Dep.make dep) path owners )
    | None ->
      Hh_logger.log "Missing entry for %s" name;
      (file_hashes, owners)
  in
  let (file_hashes, owners) =
    List.fold deps ~init:([], Typing_deps.DepMap.empty) ~f:add_requested_file
  in
  let is_requested_owner decl =
    let open Shallow_decl_defs in
    let pos =
      match decl with
      | NClass (_, decl) -> fst decl.sc_name
      | NFun (_, decl) -> decl.Typing_defs.fe_pos
      | NTypedef (_, decl) -> decl.Typing_defs.td_pos
      | NConst (_, decl) -> decl.Typing_defs.cd_pos
      | NModule (_, decl) -> decl.Typing_defs.mdt_pos
    in
    match Typing_deps.DepMap.find_opt (dep_of_named_decl decl) owners with
    | None -> false
    | Some path -> Relative_path.equal path (Pos_or_decl.filename pos)
  in
  let popt = Provider_context.get_popt ctx in
  let opts = Decl_parser_options.from_parser_options popt in
  let result =
    match fetch_old_decls_from_test_fixture ~ctx file_hashes with
    | Some decls -> Ok decls
    | None -> Remote_old_decls_ffi.get_decls_via_file_hashes opts file_hashes
  in
  match result with
  | Ok named_old_decls ->
    (* Loading whole files also returns declarations we did not request.
       Keep only requested dependencies whose declaration comes from the owner
       file recorded in the saved-state naming DB. A duplicate in another loaded
       file must not replace that owner's declaration. This applies equally to
       fixture files and files downloaded from CAS. *)
    List.filter named_old_decls ~f:is_requested_owner
  | Error msg ->
    Hh_logger.log "Error fetching remote decls: %s" msg;
    []

let fetch_old_decls
    ~(ctx : Provider_context.t)
    (deps : Typing_deps.Dep.dependent Typing_deps.Dep.variant list) :
    Shallow_decl_defs.named_decl list =
  let db_path_opt = Utils.db_path_of_ctx ~ctx in
  match db_path_opt with
  | None -> []
  | Some db_path ->
    let decl_hashes =
      List.filter_map
        ~f:(fun dep -> Utils.dep_to_decl_hash_opt ~dep ~db_path)
        deps
    in
    let start_t = Unix.gettimeofday () in
    let old_decls = fetch_old_decls_via_file_hashes ~ctx ~db_path deps in
    let to_fetch = List.length decl_hashes in
    let telemetry =
      Telemetry.create ()
      |> Telemetry.int_ ~key:"to_fetch" ~value:to_fetch
      |> Telemetry.int_ ~key:"fetched" ~value:(List.length old_decls)
    in
    Hack_event_logger.remote_old_decl_end telemetry start_t;
    Hh_logger.log
      "Fetched %d/%d decls remotely"
      (List.length old_decls)
      to_fetch;
    old_decls
