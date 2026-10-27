(*
 * Copyright (c) 2015, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude
open Save_state_service_types

let get_errors_filename (filename : string) : string = filename ^ ".err"

let get_errors_filename_json (filename : string) : string =
  filename ^ ".err.json"

let get_legacy_decls_filename (filename : string) : string = filename ^ ".decls"

let get_shallow_decls_filename (filename : string) : string =
  filename ^ ".shallowdecls"

(* Writes some OCaml object to a file with the given filename. *)
let save_contents (output_filename : string) (contents : 'a) : unit =
  let chan = Stdlib.open_out_bin output_filename in
  Marshal.to_channel chan contents [];
  Stdlib.close_out chan

(** If the contents doesn't contain the value of the expected type, the result
  is undefined behavior. We may crash, or we may continue with a bogus value. *)
let load_contents_unsafe (input_filename : string) : 'a =
  let ic = Stdlib.open_in_bin input_filename in
  let contents = Marshal.from_channel ic in
  Stdlib.close_in ic;
  contents

(** Loads the file info and the errors, if any. *)
let load_saved_state_exn
    ~(naming_table_fallback_path : string option)
    ~(errors_path : string)
    ~(warning_hashes_path : string)
    (ctx : Provider_context.t) : Naming_table.t * old_issues =
  let old_naming_table =
    match naming_table_fallback_path with
    | Some nt_path ->
      let naming_table_exists = Sys.file_exists nt_path in
      Hh_logger.log
        "Checked if the old naming table file exists (%b)"
        naming_table_exists;
      if not naming_table_exists then
        failwith
          (Printf.sprintf
             "Naming table file does not exist on disk: '%s'"
             nt_path);
      Naming_table.load_from_sqlite ctx nt_path
    | None ->
      (*
        If we reach here, then there was neither a SQLite naming table
        in the saved state directory, nor was a path provided via
        genv.local_config.naming_table_path.

        ServerCheckUtils.get_naming_table_fallback_path will return None,
        in which case we raise a failure, since the OCaml-marshalled
        naming table no longer exists.
      *)
      Hack_event_logger.naming_table_sqlite_missing ();
      let str =
        "No naming table path found, either from saved state directory or local config"
      in
      Hh_logger.log "%s" str;
      failwith str
  in
  let (old_errors : saved_state_errors) =
    Marshal.from_channel (In_channel.create ~binary:true errors_path)
  in
  let (old_warnings : Warnings_saved_state.t) =
    Warnings_saved_state.read_from_disk warning_hashes_path
  in
  (old_naming_table, { old_errors; old_warnings })

let get_hot_classes (filename : string) : S_set.t =
  if not (Disk.file_exists filename) then (
    Hh_logger.log "Hot classes file '%s' was not found" filename;
    S_set.empty
  ) else
    Disk.cat filename
    |> Yojson.Safe.from_string
    |> Hh_json_helpers.get_object_exn
    |> List.find_exn ~f:(fun (k, _) -> String.equal k "classes")
    |> snd
    |> Hh_json_helpers.get_array_exn
    |> List.map ~f:Hh_json_helpers.get_string_exn
    |> S_set.of_list

let saved_state_info_file_name ~base_file_name = base_file_name ^ "_info.json"

let saved_state_build_revision_read ~(base_file_name : string) : string =
  let info_file = saved_state_info_file_name ~base_file_name in
  let contents = Real_disk.cat info_file in
  let json = Some (Yojson.Safe.from_string contents) in
  let build_revision = Hh_json_helpers.Jget.string_exn json "build_revision" in
  build_revision

let go_naming (naming_table : Naming_table.t) (output_filename : string) :
    (save_naming_result, string) result =
  Utils.try_with_stack (fun () ->
      let save_result = Naming_table.save naming_table output_filename in
      Hh_logger.log
        "Inserted symbols into the naming table:\n%s"
        (Naming_sqlite.show_save_result save_result);

      if List.length save_result.Naming_sqlite.errors > 0 then begin
        Sys_utils.rm_dir_tree output_filename;
        failwith "Naming table state had errors - deleting output file!"
      end else
        {
          nt_files_added = save_result.Naming_sqlite.files_added;
          nt_symbols_added = save_result.Naming_sqlite.symbols_added;
        })
  |> Result.map_error ~f:(fun e -> Exception.get_ctor_string e)
