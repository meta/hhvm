(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude

(** The paths the move deals in: directories and files relative to the root. *)
module Paths = struct
  (** [file] as the server reports it, an absolute path, relative to the root when
      it is under it. *)
  let relative_to_root file =
    match Relative_path.strip_root_if_possible file with
    | Some suffix -> Relative_path.from_root ~suffix
    | None -> Relative_path.create Relative_path.Dummy file

  (** [file], named as it was before [source] moved to [target]. *)
  let original_path ~source ~target file =
    if Relative_path.is_within ~dir:target file then
      let below_target =
        String.chop_prefix_exn
          (Relative_path.suffix file)
          ~prefix:(Relative_path.suffix target)
      in
      Relative_path.from_root
        ~suffix:(Relative_path.suffix source ^ below_target)
    else
      file

  (** [parent] followed by the last name of [source]: where [source] goes when
      moved into [parent]. *)
  let target_name ~parent ~source =
    Filename.concat parent (Filename.basename source)

  let target_under ~parent ~source =
    Relative_path.from_root
      ~suffix:
        (target_name
           ~parent:(Relative_path.suffix parent)
           ~source:(Relative_path.suffix source))
end

module Dir_move = struct
  (** What a move has done to the disk, newest first, so that undoing it is
      reading it from the front. *)
  type step =
    | Made_dir of string
    | Renamed of {
        from: string;
        into: string;
      }

  (** The directories above [target] that do not exist yet, outermost first. *)
  let missing_parents target =
    let rec go dir missing =
      if String.is_empty dir || String.equal dir "." || Disk.file_exists dir
      then
        missing
      else
        go (Filename.dirname dir) (dir :: missing)
    in
    go (Filename.dirname target) []

  let make_dir journal path =
    Unix.mkdir path 0o755;
    journal := Made_dir path :: !journal

  let exists path =
    match Unix.lstat path with
    | _ -> true
    | exception Unix.Unix_error (Unix.ENOENT, _, _) -> false

  (** A step whose result is already there is skipped. *)
  let undo_step = function
    | Renamed { from; into } -> if exists into then Unix.rename into from
    | Made_dir path ->
      (try Unix.rmdir path with
      | Unix.Unix_error ((Unix.ENOTEMPTY | Unix.EEXIST | Unix.ENOENT), _, _) ->
        ())

  (** Every step is tried, so one that fails does not leave the rest undone, and
      the failures are then reported together. *)
  let undo steps =
    let failures =
      List.filter_map steps ~f:(fun step ->
          try
            undo_step step;
            None
          with
          | exn ->
            Some
              (match step with
              | Made_dir path ->
                Printf.sprintf "%s: %s" path (Exn.to_string exn)
              | Renamed { from; into } ->
                Printf.sprintf "%s to %s: %s" into from (Exn.to_string exn)))
    in
    if not (List.is_empty failures) then
      failwith
        (Printf.sprintf
           "could not undo the move completely:\n  %s"
           (String.concat ~sep:"\n  " failures))

  let move ~source ~target =
    let journal = ref [] in
    match
      List.iter (missing_parents target) ~f:(make_dir journal);
      Unix.rename source target;
      journal := Renamed { from = source; into = target } :: !journal
    with
    | () -> Ok !journal
    | exception exn ->
      let reason = Exn.to_string exn in
      (match undo !journal with
      | () -> Error reason
      | exception Failure undo_failure ->
        Error (Printf.sprintf "%s; %s" reason undo_failure))
end

module Move_error = struct
  type t = {
    code: int;
    file: Relative_path.t;
    line: int;
    message: string;
  }

  let of_status (status : Server_command_types.Server_status.t) =
    List.filter_map
      status.Server_command_types.Server_status.error_list
      ~f:(fun diagnostic ->
        match diagnostic.User_diagnostic.severity with
        | User_diagnostic.Warning _ -> None
        | User_diagnostic.Err ->
          let (pos, message) = diagnostic.User_diagnostic.claim in
          Some
            {
              code = diagnostic.User_diagnostic.code;
              file = Paths.relative_to_root (Pos.filename pos);
              line = Pos.line pos;
              message;
            })

  let key { code; file; line; message } =
    Printf.sprintf
      "%d\000%s\000%d\000%s"
      code
      (Relative_path.storage_to_string file)
      line
      message

  let keys errors = Set.of_list (module String) (List.map errors ~f:key)

  let same a b = Set.equal (keys a) (keys b)

  (** [message] with the path [from] named [into], wherever [from] is not part
      of a longer name: [a/b] is in [see a/b/c] and not in [see a/b_c]. *)
  let rename_path message ~from ~into =
    let is_name_char c =
      Char.is_alphanum c
      || Char.equal c '_'
      || Char.equal c '-'
      || Char.equal c '.'
    in
    let length = String.length message in
    let n = String.length from in
    let is_path_at i =
      i + n <= length
      && String.equal (String.sub message ~pos:i ~len:n) from
      && (i = 0 || not (is_name_char (String.get message (i - 1))))
      && (i + n = length || not (is_name_char (String.get message (i + n))))
    in
    let buffer = Buffer.create length in
    let rec go i =
      if i < length then
        if is_path_at i then begin
          Buffer.add_string buffer into;
          go (i + n)
        end else begin
          Buffer.add_char buffer (String.get message i);
          go (i + 1)
        end
    in
    go 0;
    Buffer.contents buffer

  let introduced ~source ~target ~baseline ~after =
    let known = keys baseline in
    List.filter_map after ~f:(fun error ->
        let error =
          {
            error with
            file = Paths.original_path ~source ~target error.file;
            message =
              rename_path
                error.message
                ~from:(Relative_path.suffix target)
                ~into:(Relative_path.suffix source);
          }
        in
        if Set.mem known (key error) then
          None
        else
          Some error)
end

module Output = struct
  type outcome =
    | Refused of string
    | Blocked of string list
    | Not_moved of {
        errors: Move_error.t list;
        baseline_restored: bool;
      }
    | Moved of { files: int }

  let is_moved = function
    | Moved _ -> true
    | Refused _
    | Blocked _
    | Not_moved _ ->
      false

  let error_to_json { Move_error.code; file; line; message } =
    `Assoc
      [
        ("code", `Int code);
        ("file", `String (Relative_path.suffix file));
        ("line", `Int line);
        ("message", `String message);
      ]

  (** The fields only this kind of outcome has. The others are [null]. *)
  let outcome_fields = function
    | Moved { files } -> [("files", `Int files)]
    | Refused reason -> [("refused", `String reason)]
    | Blocked blockers ->
      [("blockers", `List (List.map blockers ~f:(fun file -> `String file)))]
    | Not_moved { errors; baseline_restored } ->
      [
        ("new_errors", `List (List.map errors ~f:error_to_json));
        ("baseline_restored", `Bool baseline_restored);
      ]

  let outcome_field_names =
    ["files"; "refused"; "blockers"; "new_errors"; "baseline_restored"]

  let to_json ~source ~target outcome =
    let fields = outcome_fields outcome in
    `Assoc
      ([
         ("source", `String source);
         ("target", `String target);
         ("moved", `Bool (is_moved outcome));
       ]
      @ List.map outcome_field_names ~f:(fun name ->
            ( name,
              Option.value
                (List.Assoc.find fields name ~equal:String.equal)
                ~default:`Null )))

  let count n noun =
    if n = 1 then
      Printf.sprintf "1 %s" noun
    else
      Printf.sprintf "%d %ss" n noun

  let first_line message =
    String.take_while message ~f:(fun c -> not (Char.equal c '\n'))

  let shown_errors = 10

  let print_blocked blockers =
    let n = List.length blockers in
    Printf.printf
      "Not moved: %s outside %s in.\n"
      (count n "file")
      (if n = 1 then
        "references"
      else
        "reference");
    List.iter blockers ~f:(Printf.printf "  %s\n")

  let print_not_moved ~errors ~baseline_restored =
    Printf.printf
      "Not moved: the move introduced %s, and it was undone.\n"
      (count (List.length errors) "error");
    List.iter
      (List.take errors shown_errors)
      ~f:(fun { Move_error.code; file; line; message } ->
        Printf.printf
          "  %d %s:%d: %s\n"
          code
          (Relative_path.suffix file)
          line
          (first_line message));
    if List.length errors > shown_errors then
      Printf.printf "  and %d more\n" (List.length errors - shown_errors);
    if not baseline_restored then
      Printf.eprintf
        "warning: the server's errors after the undo differ from those before the move; `hh restart` clears that\n"

  let print_text ~source ~target = function
    | Moved { files } ->
      Printf.printf
        "Moved: %s from %s to %s, with no new errors.\n"
        (count files "file")
        source
        target
    | Refused reason -> Printf.printf "Did not run: %s\n" reason
    | Blocked blockers -> print_blocked blockers
    | Not_moved { errors; baseline_restored } ->
      print_not_moved ~errors ~baseline_restored

  let print_outcome ~source ~target ~output_json outcome =
    if output_json then
      print_endline (Yojson.Safe.to_string (to_json ~source ~target outcome))
    else
      print_text ~source ~target outcome

  let exit_status = function
    | Moved _ -> Exit_status.No_error
    | Refused _ -> Exit_status.Input_error
    | Blocked _
    | Not_moved _ ->
      Exit_status.Type_error
end

open Output

let refusal ~source ~target =
  if not (Disk.is_directory (Relative_path.to_absolute source)) then
    Some (Printf.sprintf "%s is not a directory" (Relative_path.suffix source))
  else if Disk.file_exists (Relative_path.to_absolute target) then
    Some
      (Printf.sprintf
         "%s already exists, and moving into it would merge two packages"
         (Relative_path.suffix target))
  else if
    Relative_path.is_within ~dir:source target
    || Relative_path.is_within ~dir:target source
  then
    Some "the source and the target must not contain one another"
  else
    None

(** Asks the server whether it has seen a move of [files] Hack files from
    [old_dir] to [new_dir]: it knows all of them in [new_dir] and none in
    [old_dir]. Otherwise says how many it knew in each. *)
let check_seen ~ask_movable ~new_dir ~old_dir ~files =
  let%lwt asked_new = ask_movable ~dir:new_dir in
  let%lwt asked_old = ask_movable ~dir:old_dir in
  let files_in_new_dir = asked_new.Server_isolation_types.total_seeds in
  let files_in_old_dir = asked_old.Server_isolation_types.total_seeds in
  if files_in_new_dir = files && files_in_old_dir = 0 then
    Lwt.return (Ok ())
  else
    Lwt.return (Error (files_in_new_dir, files_in_old_dir))

let unseen_message ~source ~target ~files (files_in_target, files_in_source) =
  Printf.sprintf
    "the server knew %d of the %d Hack files at %s, and %d at %s, after the move, so a typecheck would not reflect it; the move was undone. The server may not see a renamed directory unless it runs with `--config edenfs_file_watcher_enabled=true`"
    files_in_target
    files
    (Relative_path.suffix target)
    files_in_source
    (Relative_path.suffix source)

(** The errors the server reports once it has seen the move from [old_dir] to
    [new_dir], or how many Hack files it knew in each if it has not. The
    typecheck only means something once the server has seen the move: one that
    missed it would answer for the tree as it was, and pass. *)
let errors_after_move ~ask_movable ~status ~files ~new_dir ~old_dir =
  match%lwt check_seen ~ask_movable ~new_dir ~old_dir ~files with
  | Error counts -> Lwt.return (Error counts)
  | Ok () ->
    let%lwt after = status () in
    Lwt.return (Ok (Move_error.of_status after))

(** What the server says about a directory moved to [target]. *)
type verdict =
  | Unseen of (int * int)
      (** It had not seen the move: how many Hack files it knew at [target]
          and at [source]. *)
  | Clean  (** It saw the move and reports no new errors. *)
  | Introduced of Move_error.t list  (** It saw the move and reports these. *)

(** Runs [f], and calls [revert] if it raises, adding to the error if the undo
    fails too. *)
let reverting_on_exn ~revert f =
  Lwt.catch f (fun exn ->
      match revert () with
      | () -> Lwt.fail exn
      | exception Failure undo_failure ->
        Lwt.fail_with (Printf.sprintf "%s; %s" (Exn.to_string exn) undo_failure))

let verdict ~source ~target ~baseline ~errors_after_move =
  match%lwt errors_after_move ~new_dir:target ~old_dir:source with
  | Error counts -> Lwt.return (Unseen counts)
  | Ok after ->
    Lwt.return
      (match Move_error.introduced ~source ~target ~baseline ~after with
      | [] -> Clean
      | errors -> Introduced errors)

let move_and_check ~source ~target ~files ~ask_movable ~status =
  let errors_after_move = errors_after_move ~ask_movable ~status ~files in
  let%lwt baseline = status () in
  let baseline = Move_error.of_status baseline in
  match
    Dir_move.move
      ~source:(Relative_path.to_absolute source)
      ~target:(Relative_path.to_absolute target)
  with
  | Error reason ->
    Lwt.return
      (Refused
         (Printf.sprintf
            "could not move %s to %s: %s"
            (Relative_path.suffix source)
            (Relative_path.suffix target)
            reason))
  | Ok moved ->
    let revert () = Dir_move.undo moved in
    let%lwt verdict =
      reverting_on_exn ~revert (fun () ->
          verdict ~source ~target ~baseline ~errors_after_move)
    in
    (match verdict with
    | Unseen counts ->
      revert ();
      Lwt.return (Refused (unseen_message ~source ~target ~files counts))
    | Clean -> Lwt.return (Moved { files })
    | Introduced errors ->
      revert ();
      let%lwt restored = errors_after_move ~new_dir:source ~old_dir:target in
      let baseline_restored =
        match restored with
        | Ok restored -> Move_error.same baseline restored
        | Error _ -> false
      in
      Lwt.return (Not_moved { errors; baseline_restored }))

let try_move ~source ~target ~ask_movable ~status =
  match refusal ~source ~target with
  | Some reason -> Lwt.return (Refused reason)
  | None ->
    let%lwt gate = ask_movable ~dir:source in
    (match
       ( gate.Server_isolation_types.refused,
         gate.Server_isolation_types.movability )
     with
    | (Some reason, _) -> Lwt.return (Refused reason)
    | (None, Some (Server_isolation_types.Blocked blockers)) ->
      Lwt.return (Blocked blockers)
    | (None, None) ->
      Lwt.return
        (Refused "the server did not say whether the directory can move")
    | (None, Some Server_isolation_types.Movable) ->
      let files = gate.Server_isolation_types.total_seeds in
      if files = 0 then
        Lwt.return
          (Refused
             (Printf.sprintf
                "%s holds no Hack files"
                (Relative_path.suffix source)))
      else
        move_and_check ~source ~target ~files ~ask_movable ~status)

(** Moves [options.seed_dir] into [parent], then prints the outcome and gives
    its exit status. It first asks the server whether the directory can move at
    all, and moves it only if so. The server is asked about one directory at a
    time, so each question is [options] with that directory as the seed and no
    file for the clusters. *)
let move_into ~options ~parent ~output_json ~isolation_query ~status =
  let seed_dir = Option.value_exn options.Server_isolation_types.seed_dir in
  let ask_movable ~dir =
    isolation_query
      {
        options with
        Server_isolation_types.seed_dir = Some (Relative_path.suffix dir);
        clusters_destination = Server_isolation_types.Dropped;
      }
  in
  let%lwt (source, target, outcome) =
    match
      ( Relative_path.from_root_opt ~suffix:seed_dir,
        Relative_path.from_root_opt ~suffix:parent )
    with
    | (Some source, Some parent) ->
      let target = Paths.target_under ~parent ~source in
      let%lwt outcome = try_move ~source ~target ~ask_movable ~status in
      Lwt.return
        (Relative_path.suffix source, Relative_path.suffix target, outcome)
    | _ ->
      Lwt.return
        ( seed_dir,
          Paths.target_name ~parent ~source:seed_dir,
          Refused
            "the source and the target must be directories relative to the root"
        )
  in
  print_outcome ~source ~target ~output_json outcome;
  Lwt.return (exit_status outcome)
