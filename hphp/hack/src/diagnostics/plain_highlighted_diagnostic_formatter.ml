(*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

(* A plain-text error format inspired by rustc/clang/gcc.

   It is the "highlighted" (default) format with the color removed: rather than
   coloring the characters that belong to each numbered reference, it prints the
   source line and then a row of '^' underlines beneath the span on the next
   line, labelled with the matching marker.

   Like the highlighted format it shows a few lines of surrounding context and
   coalesces nearby references into a single snippet. A reference whose span
   covers several lines is underlined on every line it covers (matching what the
   colored format highlights). Leading and trailing whitespace within a span is
   excluded so the carets line up under code rather than indentation.

   No ANSI color is emitted, so the output is deterministic and suitable for use
   in .exp snapshots. *)

open Hh_prelude
open String_utils

type marked_message = {
  marker: int;
  message: Pos.absolute Message.t;
}

(* A set of references that are close enough together to share one code snippet,
   together with the merged position spanning all of them. *)
type position_group = {
  aggregate_position: Pos.absolute;
  messages: marked_message list;
}

(* Number of context lines shown before and after each snippet. *)
let n_extra_lines = 2

(* Stand-in for a line a position runs onto just past the end of the file. *)
let out_of_bounds_char = "_"

(* Assign a marker to each message in the original error order (the claim first,
   then each reason). Messages with identical positions reuse the same marker,
   so the numbering matches the colored "highlighted" format. *)
let mark_messages (msgl : Pos.absolute Message.t list) : marked_message list =
  List.folding_map msgl ~init:(1, []) ~f:(fun (next_marker, seen) message ->
      let pos = Message.get_message_pos message in
      match
        List.find seen ~f:(fun (_, seen_pos) -> Pos.equal_absolute pos seen_pos)
      with
      | Some (marker, _) -> ((next_marker, seen), { marker; message })
      | None ->
        ( (next_marker + 1, (next_marker, pos) :: seen),
          { marker = next_marker; message } ))

let relative_path (path : string) : string =
  let cwd = Filename.concat (Sys.getcwd ()) "" in
  lstrip path cwd

(* The lines of source covering [start_line - before, end_line + after] for a
   position, as (line_number, line_text option) pairs. Mirrors the window used
   by the highlighted formatter, including its past-end-of-file sentinel. *)
let load_context_lines
    ~(source : (string * string) option)
    ~(before : int)
    ~(after : int)
    (pos : Pos.absolute) : (int * string option) list =
  let path = Pos.filename pos in
  let (start_line, _) = Pos.line_column pos in
  let (end_line, end_col) = Pos.end_line_column pos in
  (* Internal errors can report the whole file; cap the window. *)
  let end_line = min (start_line + 1000) end_line in
  let lines =
    match source with
    | Some (source_path, content) when String.equal path source_path ->
      String.split_lines content
    | Some _
    | None ->
      Diagnostics.read_lines path
  in
  match lines with
  | [] ->
    List.range ~start:`inclusive ~stop:`inclusive start_line end_line
    |> List.map ~f:(fun line_num -> (line_num, None))
  | lines when not (List.for_all lines ~f:Stdlib.String.is_valid_utf_8) -> []
  | lines ->
    let numbered = List.mapi lines ~f:(fun i l -> (i + 1, Some l)) in
    let window =
      List.filter numbered ~f:(fun (i, _) ->
          i >= start_line - before && i <= end_line + after)
    in
    let last_line =
      match List.last window with
      | Some (n, _) -> n
      | None -> 0
    in
    if end_line > last_line && end_col > 0 then
      window @ [(end_line, Some out_of_bounds_char)]
    else
      window

(* The columns [start, stop) of [pos] that fall on [line_num] (0-based, [stop]
   exclusive), or None if the position covers no characters on that line. *)
let span_on_line (pos : Pos.absolute) ~(line_num : int) ~(line_len : int) :
    (int * int) option =
  let (start_line, start_col) = Pos.line_column pos in
  let (end_line, end_col) = Pos.end_line_column pos in
  if line_num < start_line || line_num > end_line then
    None
  else
    let start =
      if line_num = start_line then
        start_col
      else
        0
    in
    let stop =
      if line_num = end_line then
        end_col
      else
        line_len
    in
    (* On the final line of a multi-line span the end column is exclusive, so a
       value of 0 means nothing on this line is actually covered. *)
    if line_num = end_line && start_line <> end_line && end_col = 0 then
      None
    else
      Some (start, stop)

(* Right-aligned line-number gutter, e.g. " 12 |". When [line_num] is None the
   number is blank, used for the underline row so it aligns with the source. *)
let gutter ~(col_width : int) (line_num : int option) : string =
  match line_num with
  | Some n -> Printf.sprintf "%*d |" col_width n
  | None -> Printf.sprintf "%*s |" col_width ""

(* "  | <padding>^^^ [n]" for one marker's span on one line, excluding leading
   and trailing whitespace within the span. Returns None when the covered range
   is only whitespace (e.g. a blank line inside a multi-line span). *)
let underline_row
    ~(col_width : int)
    ~(line : string)
    ~(marker : int)
    ((start, stop) : int * int) : string option =
  let n = String.length line in
  let s = ref start in
  while !s < stop && !s < n && Char.is_whitespace line.[!s] do
    incr s
  done;
  let e = ref (min stop n) in
  while !e > !s && Char.is_whitespace line.[!e - 1] do
    decr e
  done;
  let (caret_start, width) =
    if !e > !s then
      (!s, !e - !s)
    else if start = stop && n > 0 then
      (* A genuine zero-width position: show a single caret at the point. *)
      (min start n, 1)
    else
      (* Whitespace-only (or empty-line) coverage: nothing to underline. *)
      (0, 0)
  in
  if width <= 0 then
    None
  else
    Some
      (Printf.sprintf
         "%s %s%s [%d]"
         (gutter ~col_width None)
         (String.make (max caret_start 0) ' ')
         (String.make width '^')
         marker)

(* All underline rows for [line_num]: one per marker whose span touches the
   line, deduped (a marker maps to a single span) and ordered left-to-right. *)
let underline_rows
    ~(col_width : int)
    ~(group : position_group)
    ~(line_num : int)
    ~(line_text : string option) : string list =
  match line_text with
  | None -> []
  | Some line ->
    let line_len = String.length line in
    group.messages
    |> List.filter_map ~f:(fun mm ->
           Option.map
             (span_on_line
                (Message.get_message_pos mm.message)
                ~line_num
                ~line_len)
             ~f:(fun span -> (mm.marker, span)))
    |> List.dedup_and_sort ~compare:(fun (m1, (s1, _)) (m2, (s2, _)) ->
           match Int.compare s1 s2 with
           | 0 -> Int.compare m1 m2
           | c -> c)
    |> List.filter_map ~f:(fun (marker, span) ->
           underline_row ~col_width ~line ~marker span)

(* Render one snippet: each source line, followed by any underline rows. *)
let render_group
    ~(col_width : int)
    ~(lines : (int * string option) list)
    (group : position_group) : string =
  lines
  |> List.concat_map ~f:(fun (line_num, line_text) ->
         let source =
           match line_text with
           | Some s -> s
           | None -> "No source found"
         in
         let source_row =
           Printf.sprintf "%s %s" (gutter ~col_width (Some line_num)) source
         in
         source_row :: underline_rows ~col_width ~group ~line_num ~line_text)
  |> String.concat ~sep:"\n"

(* Group the references by file (preserving order) and, within a file, into
   snippets of references that are close enough to share context. *)
let position_groups_by_file (marked : marked_message list) :
    position_group list list =
  let msg_pos mm = Message.get_message_pos mm.message in
  let by_file =
    List.group marked ~break:(fun a b ->
        not (String.equal (Pos.filename (msg_pos a)) (Pos.filename (msg_pos b))))
    |> List.map
         ~f:
           (List.sort ~compare:(fun a b ->
                Int.compare (Pos.line (msg_pos a)) (Pos.line (msg_pos b))))
  in
  let close_enough prev_pos curr_pos =
    Pos.end_line prev_pos + n_extra_lines + 2
    >= Pos.line curr_pos - n_extra_lines
  in
  List.map by_file ~f:(fun msgs ->
      List.group msgs ~break:(fun prev curr ->
          not (close_enough (msg_pos prev) (msg_pos curr)))
      |> List.map ~f:(fun messages ->
             {
               aggregate_position =
                 List.map messages ~f:msg_pos |> List.reduce_exn ~f:Pos.merge;
               messages;
             }))

(* "path:line:col" header for a file, anchored at its lowest-numbered marker. *)
let file_header (groups : position_group list) : string =
  let primary_pos =
    List.concat_map groups ~f:(fun g -> g.messages)
    |> List.stable_sort ~compare:(fun a b -> Int.compare a.marker b.marker)
    |> List.hd_exn
    |> fun mm -> Message.get_message_pos mm.message
  in
  let (line, col) = Pos.line_column primary_pos in
  Printf.sprintf
    "%s:%d:%d"
    (relative_path (Pos.filename_for_display primary_pos))
    line
    (col + 1)

(* The numbered claim + reasons header, in the original error order. *)
let format_header
    (severity : User_diagnostic.severity)
    (code : int)
    (ordered : marked_message list) : string =
  match ordered with
  | [] -> ""
  | claim_mm :: reason_mms ->
    let claim_row =
      Printf.sprintf
        "%s: %s %s [%d]"
        (User_diagnostic.Severity.to_string severity)
        (User_diagnostic.error_code_to_string code)
        (Message.get_message_str claim_mm.message)
        claim_mm.marker
    in
    let reason_rows =
      List.map reason_mms ~f:(fun mm ->
          Printf.sprintf
            "-> %s [%d]"
            (Message.get_message_str mm.message)
            mm.marker)
    in
    String.concat ~sep:"\n" (claim_row :: reason_rows)

let format
    ~(source : (string * string) option)
    (error : Diagnostics.finalized_diagnostic) : string =
  let {
    User_diagnostic.severity;
    code;
    claim;
    reasons;
    custom_msgs;
    explanation = _;
    quickfixes = _;
    is_fixmed = _;
    function_pos = _;
  } =
    error
  in
  (* Markers are numbered in the original error order ... *)
  let ordered = mark_messages (claim :: reasons) in
  (* ... but snippets are grouped by file for display. *)
  let by_file =
    Diagnostics.combining_sort ordered ~f:(fun mm ->
        Pos.filename (Message.get_message_pos mm.message))
    |> position_groups_by_file
    |> List.map ~f:(fun groups ->
           List.map groups ~f:(fun g ->
               ( g,
                 load_context_lines
                   ~source
                   ~before:n_extra_lines
                   ~after:n_extra_lines
                   g.aggregate_position )))
  in
  let col_width =
    List.concat_map by_file ~f:(fun groups ->
        List.concat_map groups ~f:(fun (_, lines) -> List.map lines ~f:fst))
    |> List.max_elt ~compare:Int.compare
    |> Option.value ~default:1
    |> Diagnostics.num_digits
  in
  (* Snippets within a file are separated by a lone ":" gutter line. *)
  let snippet_sep = "\n" ^ String.make col_width ' ' ^ " :\n" in
  let file_blocks =
    List.map by_file ~f:(fun groups ->
        let header = file_header (List.map groups ~f:fst) in
        let snippets =
          List.map groups ~f:(fun (g, lines) ->
              render_group ~col_width ~lines g)
        in
        header ^ "\n" ^ String.concat ~sep:snippet_sep snippets)
  in
  let buf = Buffer.create 256 in
  Buffer.add_string buf (format_header severity code ordered);
  Buffer.add_string buf "\n\n";
  Buffer.add_string buf (String.concat ~sep:"\n\n" file_blocks);
  Buffer.add_string buf "\n\n";
  if not (List.is_empty custom_msgs) then
    Buffer.add_string buf (String.concat ~sep:"\n" custom_msgs ^ "\n");
  Buffer.contents buf

let to_string error = format ~source:None error

let to_string_with_source ?source error =
  let path =
    Pos.filename (Message.get_message_pos error.User_diagnostic.claim)
  in
  format ~source:(Option.map source ~f:(fun source -> (path, source))) error
