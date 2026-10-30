(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)
open Hh_prelude

(* A candidate can only ever be shown *not* to be isolatable, so the count of
   files checked is what bounds the claim. *)
let output
    (result : Server_command_types.Isolation_validation.result) ~output_json =
  let open Server_command_types.Isolation_validation in
  if output_json then
    `Assoc
      [
        ( "refused",
          match result.refused with
          | None -> `Null
          | Some reason -> `String reason );
        ("isolatable", `Bool result.isolatable);
        ("files_checked", `Int result.files_checked);
        ( "violations",
          `List
            (List.map result.violations ~f:(fun v ->
                 `Assoc
                   [
                     ("referrer", `String v.referrer);
                     ("line", `Int v.line);
                     ("message", `String v.message);
                   ])) );
        ( "unknown_files",
          `List (List.map result.unknown_files ~f:(fun f -> `String f)) );
        ( "overridden_files",
          `List (List.map result.overridden_files ~f:(fun f -> `String f)) );
      ]
    |> Yojson.Safe.to_string
    |> print_endline
  else begin
    match result.refused with
    | Some reason -> Printf.printf "Cannot run here: %s\n" reason
    | None ->
      if result.isolatable then
        Printf.printf
          "Isolatable: no package boundary violated, over the %s checked.\n"
          (Utils.print_count result.files_checked "file")
      else
        Printf.printf
          "Not isolatable: %s, from %s checked.\n"
          (Utils.print_count
             (List.length result.violations)
             "package violation")
          (Utils.print_count result.files_checked "file");
      (* Paths the candidate could not be built from come before the violations:
         they are an input problem, which outranks the verdict (see [status]),
         and after a long list of violations they went unread. *)
      if not (List.is_empty result.unknown_files) then begin
        Printf.printf
          "Unknown to the naming table, so nothing was checked for them:\n";
        List.iter result.unknown_files ~f:(Printf.printf "  %s\n")
      end;
      if not (List.is_empty result.overridden_files) then begin
        Printf.printf
          "Carry a __PackageOverride, so the candidate package cannot claim them:\n";
        List.iter result.overridden_files ~f:(Printf.printf "  %s\n")
      end;
      List.iter result.violations ~f:(fun v ->
          Printf.printf "  %s:%d  %s\n" v.referrer v.line v.message)
  end

(* A path the candidate could not be built from is an input problem and outranks
   the verdict, which did not cover it. A refusal outranks both: no verdict was
   reached at all, and what has to change is the server rather than the input. *)
let status (result : Server_command_types.Isolation_validation.result) =
  let open Server_command_types.Isolation_validation in
  if Option.is_some result.refused then
    Exit_status.Config_error
  else if
    not
      (List.is_empty result.unknown_files
      && List.is_empty result.overridden_files)
  then
    Exit_status.Input_error
  else if result.isolatable then
    Exit_status.No_error
  else
    Exit_status.Type_error
