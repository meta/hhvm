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

module Utils = struct
  let db_path_of_ctx ~(ctx : Provider_context.t) : Naming_sqlite.db_path option
      =
    ctx |> Provider_context.get_backend |> Db_path_provider.get_naming_db_path

  let name_to_decl_hash_opt ~(name : string) ~(db_path : Naming_sqlite.db_path)
      =
    let dep = Typing_deps.(Dep.make (Dep.Type name)) in
    let decl_hash = Naming_sqlite.get_decl_hash_by_64bit_dep db_path dep in
    decl_hash

  let name_to_file_hash_opt ~(name : string) ~(db_path : Naming_sqlite.db_path)
      =
    let dep = Typing_deps.(Dep.make (Dep.Type name)) in
    let file_hash = Naming_sqlite.get_file_hash_by_64bit_dep db_path dep in
    file_hash
end

let fetch_old_decls_via_file_hashes
    ~(ctx : Provider_context.t)
    ~(db_path : Naming_sqlite.db_path)
    (names : string list) : Shallow_decl_defs.shallow_class option S_map.t =
  (* TODO(bobren): names should really be a list of deps *)
  let file_hashes =
    List.filter_map
      ~f:(fun name ->
        match Utils.name_to_file_hash_opt ~name ~db_path with
        | Some (path, hash) ->
          Hh_logger.log
            "Fetching decl for %s defined in %s from blob %s"
            name
            Relative_path.(suffix path)
            hash;
          Some (path, hash)
        | None ->
          Hh_logger.log "Missing entry for %s" name;
          None)
      names
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
    (* TODO(bobren) do funs typedefs consts and modules *)
    let old_decls =
      List.fold
        ~init:S_map.empty
        ~f:(fun acc ndecl ->
          match ndecl with
          | Shallow_decl_defs.NClass (name, cls) ->
            S_map.add name (Some cls) acc
          | _ -> acc)
        named_old_decls
    in
    old_decls
  | Error msg ->
    Hh_logger.log "Error fetching remote decls: %s" msg;
    S_map.empty

let fetch_old_decls ~(ctx : Provider_context.t) (names : string list) :
    Shallow_decl_defs.shallow_class option S_map.t =
  let db_path_opt = Utils.db_path_of_ctx ~ctx in
  match db_path_opt with
  | None -> S_map.empty
  | Some db_path ->
    let decl_hashes =
      List.filter_map
        ~f:(fun name -> Utils.name_to_decl_hash_opt ~name ~db_path)
        names
    in
    let start_t = Unix.gettimeofday () in
    let old_decls = fetch_old_decls_via_file_hashes ~ctx ~db_path names in
    let to_fetch = List.length decl_hashes in
    let telemetry =
      Telemetry.create ()
      |> Telemetry.int_ ~key:"to_fetch" ~value:to_fetch
      |> Telemetry.int_ ~key:"fetched" ~value:(S_map.cardinal old_decls)
    in
    Hack_event_logger.remote_old_decl_end telemetry start_t;
    Hh_logger.log
      "Fetched %d/%d decls remotely"
      (S_map.cardinal old_decls)
      to_fetch;
    old_decls
