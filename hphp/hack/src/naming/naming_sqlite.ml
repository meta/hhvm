(*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude
open Sqlite_utils

type db_path = Db_path of string [@@deriving eq, show]

type insertion_error = {
  canon_hash: Int64.t;
  hash: Int64.t;
  name_kind: Naming_types.name_kind;
  name: string;
  origin_exception: Exception.t;
      [@printer (fun fmt e -> fprintf fmt "%s" (Exception.get_ctor_string e))]
}
[@@deriving show]

type save_result = {
  files_added: int;
  symbols_added: int;
  errors: insertion_error list;
  checksum: Int64.t;
}
[@@deriving show]

let empty_save_result ~(checksum : Int64.t) =
  { files_added = 0; symbols_added = 0; errors = []; checksum }

let insert_safe ~name ~name_kind ~hash ~canon_hash f :
    (unit, insertion_error) result =
  try Ok (f ()) with
  | e ->
    let origin_exception = Exception.wrap e in
    Error { canon_hash; hash; name_kind; name; origin_exception }

type 'a forward_naming_table_delta =
  | Modified of 'a
  | Deleted
[@@deriving show]

type file_deltas = File_info.t forward_naming_table_delta Relative_path.Map.t

type blob_format =
  File_info.saved forward_naming_table_delta Relative_path.Map.t

let pp_file_deltas =
  Relative_path.Map.make_pp
    Relative_path.pp
    (pp_forward_naming_table_delta File_info.pp)

type local_changes = {
  file_deltas: file_deltas;
  base_content_version: string;
}
[@@deriving show]

type symbol_and_decl_hash = {
  symbol: Int64.t;  (** the HASH column of NAMING_SYMBOLS *)
  decl_hash: File_info.Decl_hash.t;
      (** the DECL_HASH column of NAMING_SYMBOLS *)
}

external checksum_addremove :
  Int64.t ->
  symbol:Int64.t ->
  decl_hash:File_info.Decl_hash.t ->
  path:Relative_path.t ->
  Int64.t = "checksum_addremove_ffi"

let _ = show_forward_naming_table_delta

let _ = show_local_changes

let make_relative_path ~prefix_int ~suffix =
  let prefix =
    let open Option in
    value_exn (Int64.to_int prefix_int >>= Relative_path.prefix_of_enum)
  in
  let full_suffix =
    Filename.concat (Relative_path.path_of_prefix prefix) suffix
  in
  Relative_path.create prefix full_suffix

let to_canon_name_key = Stdlib.String.lowercase_ascii

let fold_sqlite stmt ~f ~init =
  let rec helper acc =
    match Sqlite3.step stmt with
    | Sqlite3.Rc.DONE -> acc
    | Sqlite3.Rc.ROW -> helper (f stmt acc)
    | rc ->
      failwith
        (Printf.sprintf "SQLite operation failed: %s" (Sqlite3.Rc.to_string rc))
  in
  helper init

(* It's kind of weird to just dump this into the SQLite database, but removing
 * entries one at a time during a normal incremental update leads to too many
 * table scans, and I don't want to put this in a separate file because then
 * the naming table and dep table in a given SQLite database might not agree
 * with each other, and that just feels weird. *)
module LocalChanges = struct
  let table_name = "NAMING_LOCAL_CHANGES"

  let create_table_sqlite =
    Printf.sprintf
      "
          CREATE TABLE IF NOT EXISTS %s(
            ID INTEGER PRIMARY KEY,
            LOCAL_CHANGES BLOB NOT NULL,
            BASE_CONTENT_VERSION TEXT
          );
        "
      table_name

  let insert_sqlite =
    Printf.sprintf
      "
          INSERT INTO %s (ID, LOCAL_CHANGES, BASE_CONTENT_VERSION) VALUES (0, ?, ?);
        "
      table_name

  let get_sqlite =
    Printf.sprintf
      "
          SELECT LOCAL_CHANGES, BASE_CONTENT_VERSION FROM %s;
        "
      table_name

  let prime db stmt_cache base_content_version =
    let insert_stmt = StatementCache.make_stmt stmt_cache insert_sqlite in
    let (blob : blob_format) = Relative_path.Map.empty in
    let empty = Marshal.to_string blob [Marshal.No_sharing] in
    Sqlite3.bind insert_stmt 1 (Sqlite3.Data.BLOB empty) |> check_rc db;
    Sqlite3.bind insert_stmt 2 (Sqlite3.Data.TEXT base_content_version)
    |> check_rc db;
    Sqlite3.step insert_stmt |> check_rc db

  let get stmt_cache =
    let get_stmt = StatementCache.make_stmt stmt_cache get_sqlite in
    match Sqlite3.step get_stmt with
    (* We don't include Sqlite3.Rc.DONE in this match because we always expect
     * exactly one row. *)
    | Sqlite3.Rc.ROW ->
      let local_changes_blob = column_blob get_stmt 0 in
      let (local_changes_saved : blob_format) =
        Marshal.from_string local_changes_blob 0
      in
      let base_content_version = column_str get_stmt 1 in
      let file_deltas =
        Relative_path.Map.mapi local_changes_saved ~f:(fun path delta ->
            match delta with
            | Modified saved -> Modified (File_info.from_saved path saved)
            | Deleted -> Deleted)
      in
      if Relative_path.Map.cardinal file_deltas > 0 then
        Hack_event_logger.naming_sqlite_local_changes_nonempty "get";
      { base_content_version; file_deltas }
    | rc ->
      failwith
        (Printf.sprintf "Failure retrieving row: %s" (Sqlite3.Rc.to_string rc))
end

module ChecksumTable = struct
  let create_table_sqlite =
    "CREATE TABLE IF NOT EXISTS CHECKSUM (ID INTEGER PRIMARY KEY, CHECKSUM_VALUE INTEGER NOT NULL);"

  let set db stmt_cache (checksum : Int64.t) : unit =
    let stmt =
      StatementCache.make_stmt
        stmt_cache
        "REPLACE INTO CHECKSUM (ID, CHECKSUM_VALUE) VALUES (0, ?);"
    in
    Sqlite3.bind stmt 1 (Sqlite3.Data.INT checksum) |> check_rc db;
    Sqlite3.step stmt |> check_rc db;
    ()

  let get stmt_cache : Int64.t =
    let stmt =
      StatementCache.make_stmt stmt_cache "SELECT CHECKSUM_VALUE FROM CHECKSUM;"
    in
    match Sqlite3.step stmt with
    | Sqlite3.Rc.ROW -> column_int64 stmt 0
    | rc -> failwith (Sqlite3.Rc.to_string rc)
end

(* These are just done as modules to keep the SQLite for related tables close together. *)
module FileInfoTable = struct
  let table_name = "NAMING_FILE_INFO"

  let create_table_sqlite =
    Printf.sprintf
      "
      CREATE TABLE IF NOT EXISTS %s(
        FILE_INFO_ID INTEGER PRIMARY KEY AUTOINCREMENT,
        FILE_DIGEST TEXT,
        PATH_PREFIX_TYPE INTEGER NOT NULL,
        PATH_SUFFIX TEXT NOT NULL,
        TYPE_CHECKER_MODE INTEGER,
        DECL_HASH INTEGER,
        CLASSES TEXT,
        CONSTS TEXT,
        FUNS TEXT,
        TYPEDEFS TEXT,
        MODULES TEXT
      );
      "
      table_name

  let create_index_sqlite =
    Printf.sprintf
      "
      CREATE UNIQUE INDEX IF NOT EXISTS FILE_INFO_PATH_IDX ON %s (PATH_SUFFIX, PATH_PREFIX_TYPE);
      "
      table_name

  let insert_sqlite =
    Printf.sprintf
      "
        INSERT INTO %s(
          PATH_PREFIX_TYPE,
          PATH_SUFFIX,
          TYPE_CHECKER_MODE,
          DECL_HASH,
          CLASSES,
          CONSTS,
          FUNS,
          TYPEDEFS,
          MODULES
        )
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);
      "
      table_name

  let delete_sqlite =
    Printf.sprintf
      "
        DELETE FROM %s WHERE PATH_PREFIX_TYPE = ? AND PATH_SUFFIX = ?
      "
      table_name

  let get_file_info_id_sqlite =
    Printf.sprintf
      "
        SELECT
          FILE_INFO_ID
        FROM
          %s
        WHERE
          PATH_PREFIX_TYPE = ?
          AND PATH_SUFFIX = ?
      "
      table_name

  let get_sqlite =
    Printf.sprintf
      "
        SELECT
          TYPE_CHECKER_MODE, DECL_HASH, CLASSES, CONSTS, FUNS, TYPEDEFS, MODULES
        FROM %s
        WHERE PATH_PREFIX_TYPE = ? AND PATH_SUFFIX = ?;
      "
      table_name

  let iter_sqlite =
    Printf.sprintf
      "
        SELECT
          PATH_PREFIX_TYPE,
          PATH_SUFFIX,
          TYPE_CHECKER_MODE,
          DECL_HASH,
          CLASSES,
          CONSTS,
          FUNS,
          TYPEDEFS,
          MODULES
        FROM %s
        ORDER BY PATH_PREFIX_TYPE, PATH_SUFFIX;
      "
      table_name

  let delete db stmt_cache relative_path =
    let prefix_type =
      Sqlite3.Data.INT
        (Int64.of_int
           (Relative_path.prefix_to_enum (Relative_path.prefix relative_path)))
    in
    let suffix = Sqlite3.Data.TEXT (Relative_path.suffix relative_path) in
    let delete_stmt = StatementCache.make_stmt stmt_cache delete_sqlite in
    Sqlite3.bind delete_stmt 1 prefix_type |> check_rc db;
    Sqlite3.bind delete_stmt 2 suffix |> check_rc db;
    Sqlite3.step delete_stmt |> check_rc db

  let insert
      db
      stmt_cache
      relative_path
      ~(type_checker_mode : File_info.mode option)
      ~(file_decls_hash : Int64.t option)
      ~(classes : File_info.id list)
      ~(consts : File_info.id list)
      ~(funs : File_info.id list)
      ~(typedefs : File_info.id list)
      ~(modules : File_info.id list) =
    let prefix_type =
      Sqlite3.Data.INT
        (Int64.of_int
           (Relative_path.prefix_to_enum (Relative_path.prefix relative_path)))
    in
    let suffix = Sqlite3.Data.TEXT (Relative_path.suffix relative_path) in
    let type_checker_mode =
      match type_checker_mode with
      | Some type_checker_mode ->
        Sqlite3.Data.INT
          (Int64.of_int (File_info.mode_to_enum type_checker_mode))
      | None -> Sqlite3.Data.NULL
    in
    let file_decls_hash =
      match file_decls_hash with
      | Some file_decls_hash -> Sqlite3.Data.INT file_decls_hash
      | None -> Sqlite3.Data.NULL
    in
    let names_to_data_type ids =
      let open Core in
      let open File_info in
      let names =
        String.concat ~sep:"|" (List.map ids ~f:(fun id -> id.name))
      in
      match String.length names with
      | 0 -> Sqlite3.Data.NULL
      | _ -> Sqlite3.Data.TEXT names
    in
    let insert_stmt = StatementCache.make_stmt stmt_cache insert_sqlite in
    Sqlite3.bind insert_stmt 1 prefix_type |> check_rc db;

    Sqlite3.bind insert_stmt 2 suffix |> check_rc db;

    Sqlite3.bind insert_stmt 3 type_checker_mode |> check_rc db;

    Sqlite3.bind insert_stmt 4 file_decls_hash |> check_rc db;
    Sqlite3.bind insert_stmt 5 (names_to_data_type classes) |> check_rc db;
    Sqlite3.bind insert_stmt 6 (names_to_data_type consts) |> check_rc db;
    Sqlite3.bind insert_stmt 7 (names_to_data_type funs) |> check_rc db;
    Sqlite3.bind insert_stmt 8 (names_to_data_type typedefs) |> check_rc db;
    Sqlite3.bind insert_stmt 9 (names_to_data_type modules) |> check_rc db;
    Sqlite3.step insert_stmt |> check_rc db

  let read_row ~stmt ~path ~base_index =
    let file_mode =
      let open Option in
      column_int64_option stmt base_index
      >>= Int64.to_int
      >>= File_info.mode_of_enum
    in
    let position_free_decl_hash = Some (column_int64 stmt (base_index + 1)) in
    let to_ids ~value ~name_type =
      match value with
      | Sqlite3.Data.TEXT s ->
        Core.(
          List.map (String.split s ~on:'|') ~f:(fun name ->
              let pos = File_info.File (name_type, path) in
              File_info.{ pos; name; decl_hash = None }))
      | Sqlite3.Data.NULL -> []
      | _ -> failwith "Unexpected column type when retrieving names"
    in
    let classes =
      to_ids
        ~value:(Sqlite3.column stmt (base_index + 2))
        ~name_type:File_info.Class
    in
    let consts =
      to_ids
        ~value:(Sqlite3.column stmt (base_index + 3))
        ~name_type:File_info.Const
    in
    let funs =
      to_ids
        ~value:(Sqlite3.column stmt (base_index + 4))
        ~name_type:File_info.Fun
    in
    let typedefs =
      to_ids
        ~value:(Sqlite3.column stmt (base_index + 5))
        ~name_type:File_info.Typedef
    in
    let modules =
      to_ids
        ~value:(Sqlite3.column stmt (base_index + 6))
        ~name_type:File_info.Module
    in
    File_info.
      {
        position_free_decl_hash;
        file_mode;
        ids = { funs; classes; typedefs; consts; modules };
        comments = None;
      }

  let get_file_info db stmt_cache path =
    let get_stmt = StatementCache.make_stmt stmt_cache get_sqlite in
    let prefix_type =
      Relative_path.prefix_to_enum (Relative_path.prefix path)
    in
    let suffix = Relative_path.suffix path in
    Sqlite3.bind get_stmt 1 (Sqlite3.Data.INT (Int64.of_int prefix_type))
    |> check_rc db;
    Sqlite3.bind get_stmt 2 (Sqlite3.Data.TEXT suffix) |> check_rc db;
    match Sqlite3.step get_stmt with
    | Sqlite3.Rc.DONE -> None
    | Sqlite3.Rc.ROW -> Some (read_row ~stmt:get_stmt ~path ~base_index:0)
    | rc ->
      failwith
        (Printf.sprintf "Failure retrieving row: %s" (Sqlite3.Rc.to_string rc))

  let get_file_info_id db stmt_cache path =
    let get_stmt =
      StatementCache.make_stmt stmt_cache get_file_info_id_sqlite
    in
    let prefix_type =
      Relative_path.prefix_to_enum (Relative_path.prefix path)
    in
    let suffix = Relative_path.suffix path in
    Sqlite3.bind get_stmt 1 (Sqlite3.Data.INT (Int64.of_int prefix_type))
    |> check_rc db;
    Sqlite3.bind get_stmt 2 (Sqlite3.Data.TEXT suffix) |> check_rc db;
    match Sqlite3.step get_stmt with
    | Sqlite3.Rc.DONE -> None
    | Sqlite3.Rc.ROW -> Some (column_int64 get_stmt 0)
    | rc ->
      failwith
        (Printf.sprintf "Failure retrieving row: %s" (Sqlite3.Rc.to_string rc))

  let fold stmt_cache ~init ~f =
    let iter_stmt = StatementCache.make_stmt stmt_cache iter_sqlite in
    let f iter_stmt acc =
      let prefix_int = column_int64 iter_stmt 0 in
      let suffix = column_str iter_stmt 1 in
      let path = make_relative_path ~prefix_int ~suffix in
      let fi = read_row ~stmt:iter_stmt ~path ~base_index:2 in
      f path fi acc
    in
    fold_sqlite iter_stmt ~f ~init
end

module SymbolTable = struct
  let table_name = "NAMING_SYMBOLS"

  let create_table_sqlite =
    (* CANON_HASH is the hash of the lowercase form of the name (even for consts which don't need it) *)
    (* FLAGS comes from Naming_types.name_kind *)
    Printf.sprintf
      "
      CREATE TABLE IF NOT EXISTS %s(
        HASH INTEGER PRIMARY KEY NOT NULL,
        CANON_HASH INTEGER NOT NULL,
        DECL_HASH INTEGER NOT NULL,
        FLAGS INTEGER NOT NULL,
        FILE_INFO_ID INTEGER NOT NULL
      );
    "
      table_name

  let create_index_sqlite =
    Printf.sprintf
      "
    CREATE INDEX IF NOT EXISTS TYPES_CANON ON %s (CANON_HASH);
    "
      table_name

  let create_temporary_index_sqlite =
    Printf.sprintf
      "
    CREATE INDEX IF NOT EXISTS SYMBOLS_FILE_INFO_ID ON %s (FILE_INFO_ID);
    "
      table_name

  let drop_temporary_index_sqlite =
    Printf.sprintf "
    DROP INDEX SYMBOLS_FILE_INFO_ID;
    "

  let insert_sqlite =
    Printf.sprintf
      "
      INSERT INTO %s(
        HASH,
        CANON_HASH,
        DECL_HASH,
        FLAGS,
        FILE_INFO_ID)
      VALUES (?, ?, ?, ?, ?);
    "
      table_name

  let delete_sqlite =
    Printf.sprintf
      "
      DELETE FROM %s WHERE FILE_INFO_ID = ?
      "
      table_name

  let (get_sqlite, get_sqlite_case_insensitive) =
    let base =
      Str.global_replace
        (Str.regexp "{table_name}")
        table_name
        "
        SELECT
          NAMING_FILE_INFO.PATH_PREFIX_TYPE,
          NAMING_FILE_INFO.PATH_SUFFIX,
          NAMING_FILE_INFO.FILE_DIGEST,
          {table_name}.FLAGS,
          {table_name}.DECL_HASH
        FROM {table_name}
        LEFT JOIN NAMING_FILE_INFO ON
          {table_name}.FILE_INFO_ID = NAMING_FILE_INFO.FILE_INFO_ID
        WHERE {table_name}.{hash} = ?"
    in
    ( Str.global_replace (Str.regexp "{hash}") "HASH" base,
      Str.global_replace (Str.regexp "{hash}") "CANON_HASH" base )

  let delete db stmt_cache file_info_id =
    let delete_stmt = StatementCache.make_stmt stmt_cache delete_sqlite in
    Sqlite3.bind delete_stmt 1 (Sqlite3.Data.INT file_info_id) |> check_rc db;
    Sqlite3.step delete_stmt |> check_rc db

  (** Note: name parameter is used solely for debugging; it's only the hash and canon_hash that get inserted. *)
  let insert
      db stmt_cache ~name ~hash ~canon_hash ~name_kind ~file_info_id ~decl_hash
      : (unit, insertion_error) result =
    insert_safe ~name ~name_kind ~hash ~canon_hash @@ fun () ->
    let flags = name_kind |> Naming_types.name_kind_to_enum |> Int64.of_int in
    let insert_stmt = StatementCache.make_stmt stmt_cache insert_sqlite in
    Sqlite3.bind insert_stmt 1 (Sqlite3.Data.INT hash) |> check_rc db;
    Sqlite3.bind insert_stmt 2 (Sqlite3.Data.INT canon_hash) |> check_rc db;
    Sqlite3.bind
      insert_stmt
      3
      (Sqlite3.Data.INT (File_info.Decl_hash.to_int64 decl_hash))
    |> check_rc db;
    Sqlite3.bind insert_stmt 4 (Sqlite3.Data.INT flags) |> check_rc db;
    Sqlite3.bind insert_stmt 5 (Sqlite3.Data.INT file_info_id) |> check_rc db;
    Sqlite3.step insert_stmt |> check_rc db

  let get db stmt_cache dep stmt =
    let hash = dep |> Typing_deps.Dep.to_int64 in
    let get_stmt = StatementCache.make_stmt stmt_cache stmt in
    Sqlite3.bind get_stmt 1 (Sqlite3.Data.INT hash) |> check_rc db;
    match Sqlite3.step get_stmt with
    | Sqlite3.Rc.DONE -> None
    | Sqlite3.Rc.ROW ->
      let prefix_type = column_int64 get_stmt 0 in
      let suffix = column_str get_stmt 1 in
      let file_hash_opt = column_str_option get_stmt 2 in
      let file_hash = Option.value file_hash_opt ~default:"" in
      let flag = Option.value_exn (column_int64 get_stmt 3 |> Int64.to_int) in
      let decl_hash = column_int64 get_stmt 4 |> Int64.to_string in
      let name_kind =
        Option.value_exn (flag |> Naming_types.name_kind_of_enum)
      in
      Some
        ( make_relative_path ~prefix_int:prefix_type ~suffix,
          name_kind,
          decl_hash,
          file_hash )
    | rc ->
      failwith
        (Printf.sprintf "Failure retrieving row: %s" (Sqlite3.Rc.to_string rc))

  let get_symbols_and_decl_hashes_for_file_id
      db stmt_cache (file_info_id : Int64.t) : symbol_and_decl_hash list =
    let sqlite =
      Printf.sprintf
        "SELECT HASH, DECL_HASH FROM %s WHERE FILE_INFO_ID = ?"
        table_name
    in
    let stmt = StatementCache.make_stmt stmt_cache sqlite in
    Sqlite3.bind stmt 1 (Sqlite3.Data.INT file_info_id) |> check_rc db;
    fold_sqlite stmt ~init:[] ~f:(fun iter_stmt acc ->
        let symbol = column_int64 iter_stmt 0 in
        let decl_hash =
          column_int64 iter_stmt 1 |> File_info.Decl_hash.from_naming_table
        in
        { symbol; decl_hash } :: acc)
end

let db_cache :
    [ `Not_yet_cached
    | `Cached of (db_path * Sqlite3.db * StatementCache.t) option
    ]
    ref =
  ref `Not_yet_cached

let open_db (Db_path path) : Sqlite3.db =
  let db = Sqlite3.db_open path in
  Sqlite3.exec db "PRAGMA synchronous = OFF;" |> check_rc db;
  Sqlite3.exec db "PRAGMA journal_mode = MEMORY;" |> check_rc db;
  db

let get_db_and_stmt_cache (path : db_path) : Sqlite3.db * StatementCache.t =
  match !db_cache with
  | `Cached (Some (existing_path, db, stmt_cache))
    when equal_db_path path existing_path ->
    (db, stmt_cache)
  | _ ->
    let db = open_db path in
    let stmt_cache = StatementCache.make ~db in
    db_cache := `Cached (Some (path, db, stmt_cache));
    (db, stmt_cache)

let validate_can_open_db (db_path : db_path) : unit =
  (* sqlite is entirely happy opening a non-existent file;
     all it does is touches the file (giving it zero size) and
     reports that the file doesn't contain any tables. Here
     we catch the zero-size case as well as the file-not-found
     case, so we're robust against accidental sqlite touching
     prior to this function. *)
  let (Db_path path) = db_path in
  let exists =
    try (Unix.stat path).Unix.st_size > 0 with
    | exn ->
      Hh_logger.log "opening naming-table sqlite: %s" (Exn.to_string exn);
      false
  in
  if not exists then failwith "naming-table sqlite absent or empty";
  let (_ : Sqlite3.db * StatementCache.t) = get_db_and_stmt_cache db_path in
  ()

let free_db_cache () : unit = db_cache := `Not_yet_cached

let save_file_info db stmt_cache relative_path checksum file_info : save_result
    =
  let {
    File_info.file_mode;
    position_free_decl_hash;
    comments = _;
    ids = { File_info.classes; funs; typedefs; modules; consts };
  } =
    file_info
  in
  let open Core in
  FileInfoTable.insert
    db
    stmt_cache
    relative_path
    ~type_checker_mode:file_mode
    ~file_decls_hash:position_free_decl_hash
    ~consts
    ~classes
    ~funs
    ~typedefs
    ~modules;
  let file_info_id = ref None in
  Sqlite3.exec_not_null_no_headers
    db
    "SELECT last_insert_rowid()"
    ~cb:(fun row ->
      match row with
      | [| row_id |] -> file_info_id := Some (Int64.of_string row_id)
      | [||] -> failwith "Got no columns when querying last inserted row ID"
      | _ -> failwith "Got too many columns when querying last inserted row ID")
  |> check_rc db;
  let file_info_id =
    match !file_info_id with
    | Some id -> id
    | None -> failwith "Could not get last inserted row ID"
  in
  let insert ~name_kind ~dep_ctor (symbols_inserted, errors, checksum) file_info
      =
    let { File_info.pos = _; name; decl_hash } = file_info in
    let hash =
      name |> dep_ctor |> Typing_deps.Dep.make |> Typing_deps.Dep.to_int64
    in
    let canon_hash =
      name
      |> to_canon_name_key
      |> dep_ctor
      |> Typing_deps.Dep.make
      |> Typing_deps.Dep.to_int64
    in
    match decl_hash with
    | None ->
      let origin_exception =
        Exception.wrap (Failure "Missing declaration hash")
      in
      let error = { canon_hash; hash; name_kind; name; origin_exception } in
      (symbols_inserted, error :: errors, checksum)
    | Some decl_hash ->
      let checksum : Int64.t =
        checksum_addremove checksum ~symbol:hash ~decl_hash ~path:relative_path
      in
      (match
         SymbolTable.insert
           db
           stmt_cache
           ~name
           ~name_kind
           ~hash
           ~canon_hash
           ~file_info_id
           ~decl_hash
       with
      | Ok () -> (symbols_inserted + 1, errors, checksum)
      | Error error -> (symbols_inserted, error :: errors, checksum))
  in
  let results = (0, [], checksum) in
  let results =
    List.fold
      funs
      ~init:results
      ~f:
        (insert ~name_kind:Naming_types.Fun_kind ~dep_ctor:(fun name ->
             Typing_deps.Dep.Fun name))
  in
  let results =
    List.fold
      classes
      ~init:results
      ~f:
        (insert
           ~name_kind:Naming_types.(Type_kind TClass)
           ~dep_ctor:(fun name -> Typing_deps.Dep.Type name))
  in
  let results =
    List.fold
      typedefs
      ~init:results
      ~f:
        (insert
           ~name_kind:Naming_types.(Type_kind TTypedef)
           ~dep_ctor:(fun name -> Typing_deps.Dep.Type name))
  in
  let results =
    List.fold
      consts
      ~init:results
      ~f:
        (insert ~name_kind:Naming_types.Const_kind ~dep_ctor:(fun name ->
             Typing_deps.Dep.GConst name))
  in
  let results =
    List.fold
      modules
      ~init:results
      ~f:
        (insert ~name_kind:Naming_types.Module_kind ~dep_ctor:(fun name ->
             Typing_deps.Dep.Module name))
  in
  let (symbols_added, errors, checksum) = results in
  { files_added = 1; symbols_added; errors; checksum }

let save_file_infos db_name file_info_map ~base_content_version =
  let db = Sqlite3.db_open db_name in
  let stmt_cache = StatementCache.make ~db in
  Sqlite3.exec db "BEGIN TRANSACTION;" |> check_rc db;
  try
    Sqlite3.exec db LocalChanges.create_table_sqlite |> check_rc db;
    Sqlite3.exec db FileInfoTable.create_table_sqlite |> check_rc db;
    Sqlite3.exec db SymbolTable.create_table_sqlite |> check_rc db;
    Sqlite3.exec db ChecksumTable.create_table_sqlite |> check_rc db;

    (* Incremental updates only update the single row in this table, so we need
     * to write in some dummy data to start. *)
    LocalChanges.prime db stmt_cache base_content_version;
    let save_result =
      Relative_path.Map.fold
        file_info_map
        ~init:(empty_save_result ~checksum:Int64.zero)
        ~f:(fun path file_info acc ->
          let per_file =
            save_file_info db stmt_cache path acc.checksum file_info
          in
          let () = Hack_event_logger.calling_ocaml_naming_table () in

          {
            files_added = acc.files_added + per_file.files_added;
            symbols_added = acc.symbols_added + per_file.symbols_added;
            errors = List.rev_append acc.errors per_file.errors;
            checksum = per_file.checksum;
          })
    in
    ChecksumTable.set db stmt_cache save_result.checksum;
    Sqlite3.exec db FileInfoTable.create_index_sqlite |> check_rc db;
    Sqlite3.exec db SymbolTable.create_index_sqlite |> check_rc db;
    Sqlite3.exec db "END TRANSACTION;" |> check_rc db;
    StatementCache.close stmt_cache;
    if not @@ Sqlite3.db_close db then
      failwith @@ Printf.sprintf "Could not close database at %s" db_name;
    save_result
  with
  | exn ->
    let e = Exception.wrap exn in
    Sqlite3.exec db "END TRANSACTION;" |> check_rc db;
    Exception.reraise e

let copy_and_update
    ~(existing_db : db_path) ~(new_db : db_path) (local_changes : local_changes)
    : save_result =
  let (Db_path existing_path, Db_path new_path) = (existing_db, new_db) in
  FileUtil.cp ~force:(FileUtil.Ask (fun _ -> false)) [existing_path] new_path;
  let new_db = open_db new_db in
  let stmt_cache = StatementCache.make ~db:new_db in
  Sqlite3.exec new_db "BEGIN TRANSACTION;" |> check_rc new_db;
  Sqlite3.exec new_db SymbolTable.create_temporary_index_sqlite
  |> check_rc new_db;

  (* Our update plan is to use two phases: (1) go through every single file that has been
     changed in any way and delete the old entries, (2) go through every file and add the
     updated entries if any. First step is to gather from the database the FILE_INFO_ID
     of every old file... *)
  let old_files : (Relative_path.t * Int64.t) list =
    List.filter_map
      (Relative_path.Map.elements local_changes.file_deltas)
      ~f:(fun (path, _delta) ->
        match FileInfoTable.get_file_info_id new_db stmt_cache path with
        | None -> None
        | Some file_info_id -> Some (path, file_info_id))
  in

  (* Checksum: phase 1 is to remove from the checksum every item which used to be
     there in the old entries. We've read the forward-naming-table to read which symbols
     used to be there, and now we read the reverse-naming-table to read what decl-hashes they used to have.
     This has to be done before any of those old symbols have yet been removed! *)
  let checksum = ChecksumTable.get stmt_cache in
  let checksum =
    List.fold old_files ~init:checksum ~f:(fun checksum (path, file_info_id) ->
        let symbols_and_decl_hashes =
          SymbolTable.get_symbols_and_decl_hashes_for_file_id
            new_db
            stmt_cache
            file_info_id
        in
        let checksum =
          List.fold
            symbols_and_decl_hashes
            ~init:checksum
            ~f:(fun checksum { symbol; decl_hash } ->
              checksum_addremove checksum ~symbol ~decl_hash ~path)
        in
        checksum)
  in

  (* Symbols: phase 1 is to remove from forward and reverse naming table every symbol
     which used to be there. Our strategy here is to read the forward-naming-table to learn
     the FILE_INFO_ID, and then bulk delete all entries from the reverse-naming-table which
     have this same FILE_INFO_ID. (The reverse table isn't indexed by FILE_INFO_ID, which is
     why we had to create a temporary index for it. It would have been more efficient to
     get ToplevelSymbolHash from the forward naming table and remove by that, since the reverse
     naming table is already indexed by ToplevelSymbolHash. *)
  List.iter old_files ~f:(fun (path, file_info_id) ->
      SymbolTable.delete new_db stmt_cache file_info_id;
      FileInfoTable.delete new_db stmt_cache path;
      ());

  (* Symbols: phase 2 is to add forward and reverse entries for every item in file_deltas.
      Note: if we tried to combine phases 1 and 2, then in the case of symbol X being moved from
      b.php to a.php, we might have done local-change b.php first and tried to add X->b.php,
      even before X->a.php had been deleted. That would have lead to a duplicate primary key violation.
      By doing it in two phases we avoid that problem. (Of course if someone adds a duplicate entry
      then that truly will result in a duplicate primary key violation.)

     Checksum: phase 2 is to add to the checksum every symbol in file_deltas. This would give
     incorrect answers in case of duplicate symbol definitions (since the checksum is only meant
     to include the winner) but symbols phase 2 would already have already raised a sqlite
     duplicate violation if there were any duplicate symbol definitions. *)
  let result =
    Relative_path.Map.fold
      local_changes.file_deltas
      ~init:(empty_save_result ~checksum)
      ~f:(fun path file_info acc ->
        match file_info with
        | Deleted -> acc
        | Modified file_info ->
          let per_file =
            save_file_info new_db stmt_cache path acc.checksum file_info
          in
          let () = Hack_event_logger.calling_ocaml_naming_table () in

          {
            files_added = acc.files_added + per_file.files_added;
            symbols_added = acc.symbols_added + per_file.symbols_added;
            errors = List.rev_append acc.errors per_file.errors;
            checksum = per_file.checksum;
          })
  in
  ChecksumTable.set new_db stmt_cache result.checksum;
  Sqlite3.exec new_db "END TRANSACTION;" |> check_rc new_db;
  StatementCache.close stmt_cache;
  Sqlite3.exec new_db SymbolTable.drop_temporary_index_sqlite |> check_rc new_db;
  (* This reclaims space after deleting the index *)
  Sqlite3.exec new_db "VACUUM;" |> check_rc new_db;
  result

let get_local_changes (db_path : db_path) : local_changes =
  let (_db, stmt_cache) = get_db_and_stmt_cache db_path in
  LocalChanges.get stmt_cache

let fold
    ?(warn_on_naming_costly_iter = true)
    ~(db_path : db_path)
    ~init
    ~f
    ~file_deltas =
  let start_t = Unix.gettimeofday () in
  (* We depend on [Relative_path.Map.bindings] returning results in increasing
   * order here. *)
  let sorted_changes = Relative_path.Map.bindings file_deltas in
  (* This function looks kind of funky, but it's not too terrible when
   * explained. We essentially have two major inputs:
   *   1. a path and file info that we got from SQLite. In order for this
   *      function to work properly, we require that consecutive invocations
   *      of this function within the same [fold] call have paths that are
   *      strictly increasing.
   *   2. the list of local changes. This list is also required to be in
   *      increasing sorted order.
   * In order to make sure that we a) return results in sorted order, and
   * b) properly handle files that have been added (and therefore will never
   * show up in the [path] and [fi] args), we essentially walk both lists in
   * sorted order (walking the SQLite list through successive invocations and
   * walking the local changes list through recursion), at each step folding
   * over whichever entry has the lowest sort order. If both paths are
   * identical, we use the one from our local changes. In this way we get O(n)
   * iteration while handling additions, modifications, and deletions
   * properly (again, given our input sorting restraints). *)
  let rec consume_sorted_changes path fi (sorted_changes, acc) =
    match sorted_changes with
    | hd :: tl when Relative_path.compare (fst hd) path < 0 -> begin
      match snd hd with
      | Modified local_fi ->
        consume_sorted_changes path fi (tl, f (fst hd) local_fi acc)
      | Deleted -> consume_sorted_changes path fi (tl, acc)
    end
    | hd :: tl when Relative_path.equal (fst hd) path -> begin
      match snd hd with
      | Modified fi -> (tl, f path fi acc)
      | Deleted -> (tl, acc)
    end
    | _ -> (sorted_changes, f path fi acc)
  in
  let (_db, stmt_cache) = get_db_and_stmt_cache db_path in
  let (remaining_changes, acc) =
    FileInfoTable.fold
      stmt_cache
      ~init:(sorted_changes, init)
      ~f:consume_sorted_changes
  in
  let acc =
    List.fold_left
      ~f:
        begin
          fun acc (path, delta) ->
            match delta with
            | Modified fi -> f path fi acc
            | Deleted -> (* This probably shouldn't happen? *) acc
        end
      ~init:acc
      remaining_changes
  in
  if warn_on_naming_costly_iter then begin
    Hh_logger.log
      "NAMING_COSTLY_ITER\n%s"
      (Exception.get_current_callstack_string 99 |> Exception.clean_stack);
    Hack_event_logger.naming_costly_iter ~start_t
  end;
  acc

let get_file_info (db_path : db_path) path =
  let (db, stmt_cache) = get_db_and_stmt_cache db_path in
  FileInfoTable.get_file_info db stmt_cache path

(* Same as `get_db_and_stmt_cache` but with logging for when opening the
   database fails. *)
let sqlite_exn_wrapped_get_db_and_stmt_cache db_path name =
  try get_db_and_stmt_cache db_path with
  | Sqlite3.Error _ as exn ->
    let e = Exception.wrap exn in
    Hh_logger.info
      "Sqlite_db_open_bug: couldn't open the DB at `%s` while getting the position of `%s`"
      (show_db_path db_path)
      name;
    Exception.reraise e

let get_file_info_with_decl_hashes (db_path : db_path) (path : Relative_path.t)
    : File_info.t option =
  let (db, stmt_cache) = get_db_and_stmt_cache db_path in
  let add_hash make_dep id =
    let dep = Typing_deps.Dep.make (make_dep id.File_info.name) in
    let decl_hash =
      match SymbolTable.get db stmt_cache dep SymbolTable.get_sqlite with
      | None -> None
      | Some (_, _, hash, _) ->
        Some (File_info.Decl_hash.from_naming_table (Int64.of_string hash))
    in
    { id with File_info.decl_hash }
  in
  let add_hashes make_dep ids = List.map ids ~f:(add_hash make_dep) in
  let add_file_hashes file_info =
    let { File_info.funs; classes; typedefs; consts; modules } =
      file_info.File_info.ids
    in
    let ids =
      {
        File_info.funs = add_hashes (fun name -> Typing_deps.Dep.Fun name) funs;
        classes = add_hashes (fun name -> Typing_deps.Dep.Type name) classes;
        typedefs = add_hashes (fun name -> Typing_deps.Dep.Type name) typedefs;
        consts = add_hashes (fun name -> Typing_deps.Dep.GConst name) consts;
        modules = add_hashes (fun name -> Typing_deps.Dep.Module name) modules;
      }
    in
    { file_info with File_info.ids }
  in
  Option.map (FileInfoTable.get_file_info db stmt_cache path) ~f:add_file_hashes

let get_type_wrapper
    (result :
      (Relative_path.t * Naming_types.name_kind * string * string) option) :
    (Relative_path.t * Naming_types.kind_of_type) option =
  match result with
  | None -> None
  | Some (filename, Naming_types.Type_kind kind_of_type, _, _) ->
    Some (filename, kind_of_type)
  | Some (_, _, _, _) -> failwith "wrong symbol kind"

let get_fun_wrapper
    (result :
      (Relative_path.t * Naming_types.name_kind * string * string) option) :
    Relative_path.t option =
  match result with
  | None -> None
  | Some (filename, Naming_types.Fun_kind, _, _) -> Some filename
  | Some (_, _, _, _) -> failwith "wrong symbol kind"

let get_const_wrapper
    (result :
      (Relative_path.t * Naming_types.name_kind * string * string) option) :
    Relative_path.t option =
  match result with
  | None -> None
  | Some (filename, Naming_types.Const_kind, _, _) -> Some filename
  | Some (_, _, _, _) -> failwith "wrong symbol kind"

let get_module_wrapper
    (result :
      (Relative_path.t * Naming_types.name_kind * string * string) option) :
    Relative_path.t option =
  match result with
  | None -> None
  | Some (filename, Naming_types.Module_kind, _, _) -> Some filename
  | Some (_, _, _, _) -> failwith "wrong symbol kind"

let get_decl_hash_wrapper
    (result :
      (Relative_path.t * Naming_types.name_kind * string * string) option) :
    string option =
  match result with
  | None -> None
  | Some (_, _, decl_hash, _) -> Some decl_hash

let get_file_hash_wrapper
    (result :
      (Relative_path.t * Naming_types.name_kind * string * string) option) :
    (Relative_path.t * string) option =
  match result with
  | None -> None
  | Some (path, _, _, file_hash) -> Some (path, file_hash)

let get_type_path_by_name (db_path : db_path) name =
  let (db, stmt_cache) =
    sqlite_exn_wrapped_get_db_and_stmt_cache db_path name
  in
  SymbolTable.get
    db
    stmt_cache
    (Typing_deps.Dep.Type name |> Typing_deps.Dep.make)
    SymbolTable.get_sqlite
  |> get_type_wrapper

let get_itype_path_by_name (db_path : db_path) name =
  let (db, stmt_cache) =
    sqlite_exn_wrapped_get_db_and_stmt_cache db_path name
  in
  SymbolTable.get
    db
    stmt_cache
    (Typing_deps.Dep.Type (Stdlib.String.lowercase_ascii name)
    |> Typing_deps.Dep.make)
    SymbolTable.get_sqlite_case_insensitive
  |> get_type_wrapper

let get_fun_path_by_name (db_path : db_path) name =
  let (db, stmt_cache) = get_db_and_stmt_cache db_path in
  SymbolTable.get
    db
    stmt_cache
    (Typing_deps.Dep.Fun name |> Typing_deps.Dep.make)
    SymbolTable.get_sqlite
  |> get_fun_wrapper

let get_ifun_path_by_name (db_path : db_path) name =
  let (db, stmt_cache) = get_db_and_stmt_cache db_path in
  SymbolTable.get
    db
    stmt_cache
    (Typing_deps.Dep.Fun (Stdlib.String.lowercase_ascii name)
    |> Typing_deps.Dep.make)
    SymbolTable.get_sqlite_case_insensitive
  |> get_fun_wrapper

let get_const_path_by_name (db_path : db_path) name =
  let (db, stmt_cache) = get_db_and_stmt_cache db_path in
  SymbolTable.get
    db
    stmt_cache
    (Typing_deps.Dep.GConst name |> Typing_deps.Dep.make)
    SymbolTable.get_sqlite
  |> get_const_wrapper

let get_module_path_by_name (db_path : db_path) name =
  let (db, stmt_cache) = get_db_and_stmt_cache db_path in
  SymbolTable.get
    db
    stmt_cache
    (Typing_deps.Dep.Module name |> Typing_deps.Dep.make)
    SymbolTable.get_sqlite
  |> get_module_wrapper

let get_path_by_64bit_dep (db_path : db_path) (dep : Typing_deps.Dep.t) =
  let (db, stmt_cache) = get_db_and_stmt_cache db_path in
  SymbolTable.get db stmt_cache dep SymbolTable.get_sqlite
  |> Option.map ~f:(function (first, second, _, _) -> (first, second))

let get_decl_hash_by_64bit_dep (db_path : db_path) (dep : Typing_deps.Dep.t) =
  let (db, stmt_cache) = get_db_and_stmt_cache db_path in
  SymbolTable.get db stmt_cache dep SymbolTable.get_sqlite
  |> get_decl_hash_wrapper

let get_file_hash_by_64bit_dep (db_path : db_path) (dep : Typing_deps.Dep.t) =
  let (db, stmt_cache) = get_db_and_stmt_cache db_path in
  SymbolTable.get db stmt_cache dep SymbolTable.get_sqlite
  |> get_file_hash_wrapper
