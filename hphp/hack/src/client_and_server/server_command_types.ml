(*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

open Hh_prelude

type connection_type = Non_persistent

type connection_response = Connected

type status_liveness =
  | Stale_status
  | Live_status

module Server_status = struct
  type t = {
    liveness: status_liveness;
    error_list: Diagnostics.finalized_diagnostic list;
    dropped_count: int;
    last_recheck_stats: Telemetry.t option;
    file_watcher_clock: Server_notifier_types.clock option;
  }
end

module Identify_symbol = struct
  type single_result =
    string Symbol_occurrence.t * string Symbol_definition.t option

  type result = single_result list
end

module Method_jumps = struct
  type result = {
    orig_name: string;
    orig_pos: Pos.absolute;
    dest_name: string;
    dest_pos: Pos.absolute;
    orig_p_name: string;
    (* Used for methods to find their parent class *)
    dest_p_name: string;
  }

  type filter =
    | No_filter
    | Class
    | Interface
    | Trait
  [@@deriving show]

  let readable_place name pos p_name =
    let readable = Pos.string pos in
    if String.length p_name <> 0 then
      readable ^ " " ^ Utils.strip_ns p_name ^ "::" ^ Utils.strip_ns name
    else
      readable ^ " " ^ Utils.strip_ns name

  let print_readable res ~find_children =
    List.iter res ~f:(fun res ->
        let origin_readable =
          readable_place res.orig_name res.orig_pos res.orig_p_name
        in
        let dest_readable =
          readable_place res.dest_name res.dest_pos res.dest_p_name
        in
        let extended =
          "inherited "
          ^
          if find_children then
            "by"
          else
            "from"
        in
        print_endline
          (origin_readable ^ "\n    " ^ extended ^ " " ^ dest_readable));
    ()
end

module Done_or_retry = struct
  (* This is an ugly hack to support following case:
   * - client issues a command that requires full recheck. Server knows not to
   *   accept it until full check is completed .
   * - while processing the command, it turns out that we need to recheck even
   *   more files for full check to be "good enough" for this command
   * This can happen when combining prechecked files and find references.
   * We could store the client with its command somwhere, and come back to it
   * after this additional rechecking is done, but in practice this is ugly to
   * implement in current atchitecture. What is easier to do instead, is to
   * return a special "Retry" response to the client, which will cause it to
   * re-issue the same request (which will once again not be accepted until full
   * check is completed. Since command is the same, its second execution should
   * not add even more files to recheck, hence a limit of only two attempts.
   *
   * In other words: the goal of this is to avoid implementing even more
   * server-side client queing mechanisms, but re-use an existing "clients waiting
   * for full check" queue. *)
  exception Two_retries_in_a_row

  type 'a t =
    | Done of 'a
    | Retry

  (* Note: this is designed to work with calls that always will succeed on second try
   * (the reason for retrying is a one time event that is resolved during first call).
   * If this ends up throwing, it's a bug in hh_server. *)
  let rec call ~(f : unit -> 'a t Lwt.t) ~(depth : int) : _ Lwt.t =
    Lwt.Infix.(
      (if depth = 2 then
        Lwt.fail Two_retries_in_a_row
      else
        Lwt.return_unit)
      >>= fun () ->
      f () >>= fun result ->
      match result with
      | Done x -> Lwt.return x
      | Retry -> call ~f ~depth:(depth + 1))

  (* Call the function returning Done_or_retry.t with at most one retry, expecting
   * that this is enough to yield a non-Retry value, which is returned *)
  let call ~(f : unit -> 'a t Lwt.t) : _ Lwt.t = call ~f ~depth:0

  (* Helper function useful when mapping over results from functions that (in addition
   * to Done_or_retry.t result) thread through some kind of environment. *)
  let map_env ~(f : 'a -> 'b) ((env, x) : 'env * 'a t) : 'env * 'b t =
    match x with
    | Done x -> (env, Done (f x))
    | Retry -> (env, Retry)
end

module Find_refs = struct
  include Search_types.Find_refs

  type server_result = Search_types.Find_refs.t list

  type result = Search_types.Find_refs.absolute list

  type server_result_or_retry = server_result Done_or_retry.t

  type result_or_retry = result Done_or_retry.t
end

module Package_lint = struct
  type result = (Find_refs.action * Find_refs.result) list

  type result_or_retry = result Done_or_retry.t

  type fast_result = Relative_path.Set.t
end

module Isolation_validation = struct
  (** A package boundary the candidate would violate, of which there are two
      kinds. A file outside it references in — the case the candidate exists to
      rule out. Or the candidate reaches into its own excluded path: strict
      isolation switches off the exemption that normally lets a package use its
      own [__tests__], so [referrer] is then inside the candidate too.

      The second is the common one in practice, and the reason this is not
      described as an inbound reference. *)
  type violation = {
    referrer: string;
    line: int;
    message: string;
  }
  [@@deriving show]

  type result = {
    refused: string option;
        (** Why the command declined to run at all, if it did. The server it was
            asked of is not one this can run on — decl repackaging switched off,
            or a backend that keeps its own copy of the package configuration.
            Reported rather than raised: the caller learns what to change, and
            the server it asked stays up. Everything below is empty when this is
            set. *)
    isolatable: bool;
        (** No package boundary was violated among the files checked, and the
            candidate was one this command could construct in full. *)
    files_checked: int;
        (** How many files were typechecked against the synthesized package.
            The verdict is only as good as this number. *)
    violations: violation list;
    unknown_files: string list;
        (** Candidate paths the naming table does not know, so nothing was
            checked on their behalf. *)
    overridden_files: string list;
        (** Candidate paths that name a package of their own with
            [<<file: __PackageOverride(...)>>]. An override beats include-path
            matching, so the synthesized package never claims the file and the
            candidate asked about is not the one that could be checked. *)
  }
  [@@deriving show]
end

module Find_my_tests = struct
  open Ppx_yojson_conv_lib.Yojson_conv.Primitives

  type member = {
    class_name: string;
    member_name: string;
  }
  [@@deriving show]

  type action_kind =
    | Class of { class_name: string }
    | Typedef of { name: string }
    | Method of member
    | Typeconst of member
    | Class_const of member
  [@@deriving show]

  type action = {
    kind: action_kind;
    soft: bool;
        (** Silently drop this root if the symbol does not exist.
            For top-level definitions (classes, typedefs), the symbol must be
            declared. For class members, it is sufficient for the member to be
            defined on the class, even if only inherited (not declared in the
            class itself). *)
  }
  [@@deriving show]

  type provenance = { root_indices: int list } [@@deriving yojson]

  (** Count for one edge-reason key in a reason-provenance multiset. The
      [edge_key] records the reason used by an edge, together with its source and
      target kinds, as "source_kind[root=...]|reason|target_kind". *)
  type reason_count = {
    edge_key: string;
    count: int;
  }
  [@@deriving yojson]

  (** One minimal multiset of edge-reason keys occurring on paths from a root to a
      selected test (used for reason provenance). *)
  type reason_multiset = reason_count list [@@deriving yojson]

  (** A concrete selection-graph edge. Symbol identities and expansion
      strategies are separate; test targets have no expansion strategy.
      [edge_key] uses the same normalization as reason provenance. *)
  type path_edge = {
    source_symbol: string;
    source_strategy: string;
    target_symbol: string;
    target_strategy: string option;
    reason: string;
    edge_key: string;
  }
  [@@deriving yojson]

  (** Ordered edges from a root symbol to a selected test node. *)
  type selection_path = path_edge list [@@deriving yojson]

  type selected_cldp_member = {
    member_name: string;
    selected_after_secs: float;
        (** Wall-clock seconds from the start of selection to when this CLDP test
            member was first selected. *)
    reason_provenance_indices: int list option;
        (** Indices into [result_data.reason_provenance] for this CLDP test member.
            [None] when reason provenance is not requested. *)
    path_provenance_indices: int list option;
        (** Indices into [result_data.path_provenance] for this CLDP test member. *)
  }
  [@@deriving yojson]

  type selected_test_method = {
    method_name: string;
    selected_after_secs: float;
        (** Wall-clock seconds from the start of selection to when this test
            method was first selected. *)
    uses_cldp: bool option; [@default None]
    cldp_members: selected_cldp_member list option; [@default None]
    reason_provenance_indices: int list option;
        (** Indices into [result_data.reason_provenance] for this test method. *)
    path_provenance_indices: int list option;
  }
  [@@deriving yojson]

  type selected_test_class = {
    class_name: string;
    selected_after_secs: float;
        (** Wall-clock seconds from the start of selection to when this test
            class was first selected. *)
    methods: selected_test_method list;
    reason_provenance_indices: int list option;
        (** Indices into [result_data.reason_provenance] for this test class. *)
    path_provenance_indices: int list option;
  }
  [@@deriving yojson]

  type selected_test_file = {
    file_path: string;
    distance: int;
    selected_after_secs: float;
        (** Wall-clock seconds from the start of selection to when this test was
            first selected (i.e. when its node was first added to the graph). *)
    provenance: provenance option;
    test_classes: selected_test_class list option;
  }
  [@@deriving yojson]

  type result_data = {
    selected_test_files: selected_test_file list;
    out_of_time: bool;
    reason_provenance: reason_multiset list option;
        (** Interned pool of minimal multisets referenced by the
            [reason_provenance_indices] fields throughout the result. [None]
            when reason provenance is not requested. *)
    path_provenance: selection_path list option;
        (** Interned pool of concrete paths whose reason multisets are minimal.
            Distinct paths with equal multisets are retained. *)
  }
  [@@deriving yojson]

  type result = (result_data, string) Result.t

  type version =
    | V2
    | Staging
  [@@deriving show]

  let yojson_of_version = function
    | V2 -> `String "v2"
    | Staging -> `String "staging"

  let version_of_yojson = function
    | `String "v2" -> V2
    | `String "staging" -> Staging
    | json ->
      Ppx_yojson_conv_lib.Yojson_conv.of_yojson_error
        "invalid FindMyTests version"
        json

  type config = {
    max_distance: int; [@default 1]
    max_test_files: int option; [@default None]
    root_provenance: bool; [@default false]
    reason_provenance: bool; [@default false]
        (** Track, per selected test, the minimal multisets of edge-type reasons
            on paths from a root that caused its selection. *)
    path_provenance: bool; [@default false]
        (** Return every concrete path attaining a minimal reason multiset. *)
    time_limit_secs: int option; [@default None]
    test_count_checkpoint_secs: int list; [@default [20; 30; 40]]
        (** Elapsed-time checkpoints (in seconds) at which to record the number of selected test files. *)
    symbol_node_count_checkpoint_interval: int option; [@default None]
        (** Record elapsed time whenever this many symbol nodes have been constructed. *)
    test_file_node_count_checkpoint_interval: int option; [@default None]
        (** Record elapsed time whenever this many test file nodes have been constructed. *)
    max_method_parent_steps: int option; [@default None]
    max_class_parent_steps: int option; [@default None]
    parent_max_fanout_files: int option; [@default None]
        (** If a symbol is obtained from parent walking and its fanout is larger than this number
        of files, we skip searching  those files for references entirely. *)
    non_parent_max_fanout_files: int option; [@default None]
        (** If a symbol is NOT obtained from parent walking and its fanout is larger than this number
        of files, we skip searching those files for references entirely. *)
    enclosing_class_all_methods: bool; [@default true]
        (** Should we go from any method to the enclosing class, or just for root methods? *)
    cldp_member_selection: bool; [@default false]
  }
  [@@deriving yojson, show]

  let default_config =
    {
      max_distance = 1;
      max_test_files = None;
      root_provenance = false;
      reason_provenance = false;
      path_provenance = false;
      time_limit_secs = None;
      test_count_checkpoint_secs = [20; 30; 40];
      symbol_node_count_checkpoint_interval = None;
      test_file_node_count_checkpoint_interval = None;
      max_method_parent_steps = None;
      max_class_parent_steps = None;
      parent_max_fanout_files = None;
      non_parent_max_fanout_files = None;
      enclosing_class_all_methods = true;
      cldp_member_selection = false;
    }

  type json_input = {
    roots: string list;
    config: config;
    version: version; [@default Staging]
  }
  [@@deriving yojson]
end

module Rename = struct
  type ide_result = (Server_rename_types.patch list, string) result

  type result = Server_rename_types.patch list

  type ide_result_or_retry = ide_result Done_or_retry.t

  type result_or_retry = result Done_or_retry.t

  (** Returns a string in the form of
   * NEW_NAME|COMMA_SEPARATED_FIND_REFS_ACTION|OCaml_marshalled_SymbolDefinition.T
  *)
  let arguments_to_string_exn
      (new_name : string)
      (action : Find_refs.action)
      (symbol_def : Relative_path.t Symbol_definition.t) : string =
    let symbol_and_action =
      Find_refs_wire_format.CliArgs.to_string
        {
          Find_refs_wire_format.CliArgs.symbol_name = new_name;
          action;
          stream_file = None;
          hint_suffixes = [];
        }
    in
    let marshalled_def = Marshal.to_string symbol_def [] in
    let encoded = Base64.encode_exn marshalled_def in
    Printf.sprintf "%s|%s" symbol_and_action encoded

  (** Expects a string in the form of
   * NEW_NAME|COMMA_SEPARATED_FIND_REFS_ACTION|OCaml_marshalled_SymbolDefinition.T
   * For example, a valid entry is
   * HackTypecheckerQueryBase::WWWDir|Member,\HackTypecheckerQueryBase,Method,getWWWDir|<byte_string>
  *)
  let string_to_args arg :
      string * Find_refs.action * Relative_path.t Symbol_definition.t =
    let split_arg = Str.split (Str.regexp "|") arg in
    let (symbol_name, action_arg, marshalled_def) =
      match split_arg with
      | [symbol_name; action_arg; marshalled_def] ->
        (symbol_name, action_arg, marshalled_def)
      | _ ->
        Printf.eprintf "Invalid input\n";
        raise Exit_status.(Exit_with Input_error)
    in
    let str = Printf.sprintf "%s|%s" symbol_name action_arg in
    let { Find_refs_wire_format.CliArgs.symbol_name = new_name; action; _ } =
      Find_refs_wire_format.CliArgs.from_string_exn str
    in
    let decoded_str = Base64.decode_exn marshalled_def in
    let symbol_definition : Relative_path.t Symbol_definition.t =
      Marshal.from_string decoded_str 0
    in
    (new_name, action, symbol_definition)
end

module Symbol_type = struct
  type t = {
    pos: string Pos.pos;
    type_: string;
    ident_: int;
  }
  [@@deriving show]
end

module Symbol_info_service = struct
  type target_type =
    | Function
    | Method
    | Constructor
  [@@deriving ord, show]

  type symbol_fun_call = {
    name: string;
    type_: target_type;
    pos: string Pos.pos;
    caller: string;
  }
  [@@deriving show]

  type result = {
    fun_calls: symbol_fun_call list;
    symbol_types: Symbol_type.t list;
  }

  let fun_call_to_json fun_call_results =
    List.map fun_call_results ~f:(fun item ->
        let item_type =
          match item.type_ with
          | Function -> "Function"
          | Method -> "Method"
          | Constructor -> "Constructor"
        in
        `Assoc
          [
            ("name", `String item.name);
            ("type", `String item_type);
            ("pos", Pos.json item.pos);
            ("caller", `String item.caller);
          ])

  let symbol_type_to_json symbol_type_results =
    Symbol_type.(
      List.rev_map symbol_type_results ~f:(fun item ->
          `Assoc
            [
              ("pos", Pos.json item.pos);
              ("type", `String item.type_);
              ("ident", `Int item.ident_);
            ]))

  let to_json result =
    let fun_call_json = fun_call_to_json result.fun_calls in
    let symbol_type_json = symbol_type_to_json result.symbol_types in
    `Assoc
      [
        ("function_calls", `List fun_call_json);
        ("symbol_types", `List symbol_type_json);
      ]
end

module Outline = struct
  type outline = string Symbol_definition.t list
end

module Infer_return_type = struct
  type t =
    | Function of string
    | Method of string * string

  type result = (string, string) Stdlib.result
end

module Ide_rename_type = struct
  type t = {
    filename: Relative_path.t;
    line: int;
    char: int;
    new_name: string;
  }
  [@@deriving show]
end

module Go_to_definition = struct
  type result = (string Symbol_occurrence.t * string Symbol_definition.t) list
end

module Go_to_type_definition = struct
  type result = (Pos.absolute * string) list
end

module Tast_hole = struct
  type filter =
    | Typing
    | Cast
    | Any
  [@@deriving show]
end

type file_input =
  | FileName of string
  | FileContent of string
[@@deriving show]

type lint_stdin_input = {
  filename: string;
  contents: string;
}
[@@deriving show]

type cst_search_input = {
  sort_results: bool;
  input:
    (Yojson.Safe.t
    [@printer
      (fun fmt json -> Format.pp_print_string fmt (Yojson.Safe.to_string json))]);
  files_to_search: string list option; (* if None, search all files *)
}
[@@deriving show]

(* The following datatypes can be interpreted as follows:
 * MESSAGE_TAG : Argument type (sent from client to server) -> return type t *)
type _ t =
  | STATUS : {
      max_errors: int option;
      error_filter: Filter_diagnostics.Filter.t;
    }
      -> Server_status.t t
  | STATUS_SINGLE : {
      file_names: file_input list;
      max_errors: int option;
      error_filter: Filter_diagnostics.Filter.t;
      return_expanded_tast: bool;
      preexisting_warnings: bool;
          (** Whether to show preexisting warnings
            (the ones that were there prior to saved state revision) *)
    }
      -> ((Diagnostics.finalized_diagnostic list * int)
         * Tast.program Tast_with_dynamic.t Relative_path.Map.t option)
         t
  | LOG_ERRORS : {
      files: string list;
      log_file: string option;
      error_filter: Filter_diagnostics.Filter.t;
      preexisting_warnings: bool;
    }
      -> unit t
  | INFER_TYPE :
      file_input * File_content.Position.t
      -> Infer_at_pos_service.result t
  | INFER_DYNAMIC : string * bool -> Yojson.Safe.t t
  | ENFORCEMENT_AT_POS_BATCH :
      (string * File_content.Position.t) list
      -> string list t
  | INFER_TYPE_BATCH :
      (string * File_content.Position.t * File_content.Position.t option) list
      -> string list t
  | INFER_TYPE_ERROR :
      file_input * int * int
      -> Infer_error_at_pos_service.result t
  | IS_SUBTYPE : string -> (string, string) result t
  | TAST_HOLES : file_input * Tast_hole.filter -> Tast_holes_service.result t
  | TAST_HOLES_BATCH : string list -> Tast_holes_service.result t
  | IDENTIFY_SYMBOL : string -> string Symbol_definition.t list t
  | IDENTIFY_FUNCTION :
      string * file_input * File_content.Position.t
      -> Identify_symbol.result t
  | METHOD_JUMP :
      (string * Method_jumps.filter * bool)
      -> Method_jumps.result list t
  | METHOD_JUMP_BATCH :
      (string list * Method_jumps.filter)
      -> Method_jumps.result list t
  | FIND_REFS : Find_refs.action -> Find_refs.result_or_retry t
  | GO_TO_IMPL : Find_refs.action -> Find_refs.result_or_retry t
  | IDE_FIND_REFS_BY_SYMBOL :
      Find_refs_wire_format.CliArgs.t
      -> Find_refs.result_or_retry t
  | IDE_GO_TO_IMPL_BY_SYMBOL :
      Find_refs_wire_format.CliArgs.t
      -> Find_refs.result_or_retry t
  | RENAME : Server_rename_types.action -> Rename.result_or_retry t
  | IDE_RENAME_BY_SYMBOL :
      Find_refs.action * string * Relative_path.t Symbol_definition.t
      -> Rename.ide_result_or_retry t
  | DUMP_SYMBOL_INFO : string list -> Symbol_info_service.result t
  | REMOVE_DEAD_FIXMES :
      int list
      -> [ `Ok of Server_rename_types.patch list | `Error of string ] t
  | REMOVE_DEAD_UNSAFE_CASTS
      : [ `Ok of Server_rename_types.patch list | `Error of string ] t
  | REWRITE_LAMBDA_PARAMETERS : string list -> Server_rename_types.patch list t
  | IN_MEMORY_DEP_TABLE_SIZE : (int, string) Stdlib.result t
  | SAVE_NAMING :
      string
      -> (Save_state_service_types.save_naming_result, string) Stdlib.result t
  | CHECK_LIVENESS : unit t
  | PAUSE : unit t
      (** Pause automatic rechecks until the next client connection. This uses
          the default pipe so that ongoing typechecking finishes first. *)
  | LINT : string list -> Server_lint_types.result t
  | LINT_STDIN : lint_stdin_input -> Server_lint_types.result t
  | LINT_ALL : int -> Server_lint_types.result t
  | STATS : Stats.t t
  | DUMP_FULL_FIDELITY_PARSE : string -> string t
  | RAGE : Server_rage_types.result t
  | CST_SEARCH : cst_search_input -> (Yojson.Safe.t, string) result t
  | NO_PRECHECKED_FILES : unit t
  | FUN_DEPS_BATCH : (string * int * int) list -> string list t
  | LIST_FILES_WITH_ERRORS : string list t
  | FILE_DEPENDENTS : string list -> string list t
  | FIND_ISOLATABLE_CLUSTERS :
      Server_isolation_types.options
      -> Server_isolation_types.result t
  | VALIDATE_ISOLATION : string list -> Isolation_validation.result t
  | VERBOSE : bool -> unit t
  | DEPS_OUT_BATCH : (string * int * int) list -> string list t
  | DEPS_IN_BATCH :
      (string * int * int) list
      -> Find_refs.result_or_retry list t
  | FIND_MY_TESTS :
      (Find_my_tests.version * Find_my_tests.config * Find_my_tests.action list)
      -> Find_my_tests.result t
  | PACKAGE_LINT : string -> Package_lint.fast_result t
  | PACKAGE_LINT_FULL : (string * string list) -> Package_lint.fast_result t

type cmd_metadata = {
  from: string;
  (* a short human-readable string, used in "hh_server is busy [desc]" *)
  desc: string;
}

type 'a command = cmd_metadata * 'a t

and streamed =
  | SHOW of string
  | LIST_MODES

type errors = Diagnostics.finalized_diagnostic list [@@deriving show]

let equal_errors errors1 errors2 =
  let errors1 = Diagnostics.FinalizedErrorSet.of_list errors1 in
  let errors2 = Diagnostics.FinalizedErrorSet.of_list errors2 in
  Diagnostics.FinalizedErrorSet.equal errors1 errors2

type diagnostic_errors = errors S_map.t [@@deriving eq, show]

type 'a message_type =
  | Hello  (** Hello is the first message sent to the client by the server *)
  | Monitor_failed_to_handoff
      (** However, if the handoff failed, this will be sent instead of Hello, and the connection terminated. *)
  | Ping
      (** Server sometimes sends these, after Hello and before Response, to check if client fd is still open *)
  | Response of 'a * Connection_tracker.t
      (** Response message is the response to an RPC. The server will close fd after this. *)

(** Timeout on reading the command from the client - client probably frozen. *)
exception Read_command_timeout

(** Invariant: The server_finale_file is created by Exit.exit and left there. *)
type server_specific_files = {
  server_finale_file: string;  (** just before exit, server will write here *)
}

(** Some client commands require full check to be run in order to update global
  state that they depend on *)
let rpc_command_needs_full_check : type a. a t -> bool =
 fun msg ->
  match msg with
  (* global error list is not updated during small checks *)
  | STATUS _ -> true
  | LIST_FILES_WITH_ERRORS
  | REMOVE_DEAD_FIXMES _
  | REMOVE_DEAD_UNSAFE_CASTS ->
    true (* need same information as STATUS *)
  | REWRITE_LAMBDA_PARAMETERS _ -> true
  (* Finding references/implementations uses global dependency table *)
  | FIND_REFS _ -> true
  | GO_TO_IMPL _ -> true
  | IDE_FIND_REFS_BY_SYMBOL _ -> true
  | IDE_GO_TO_IMPL_BY_SYMBOL _ -> true
  | METHOD_JUMP (_, _, find_children) -> find_children (* uses find refs *)
  | FIND_MY_TESTS _ -> true
  | SAVE_NAMING _ -> false
  (* Codebase-wide rename, uses find references *)
  | RENAME _ -> true
  | IDE_RENAME_BY_SYMBOL _ -> true
  (* Same case as Ai commands *)
  | IN_MEMORY_DEP_TABLE_SIZE -> true
  | NO_PRECHECKED_FILES -> true
  | STATS -> false
  | STATUS_SINGLE _ -> false
  | LOG_ERRORS _ -> false
  | INFER_TYPE _ -> false
  | INFER_DYNAMIC _ -> false
  | ENFORCEMENT_AT_POS_BATCH _ -> false
  | INFER_TYPE_BATCH _ -> false
  | INFER_TYPE_ERROR _ -> false
  | IS_SUBTYPE _ -> false
  | TAST_HOLES _ -> false
  | TAST_HOLES_BATCH _ -> false
  | IDENTIFY_FUNCTION _ -> false
  | IDENTIFY_SYMBOL _ -> false
  | METHOD_JUMP_BATCH _ -> false
  | DUMP_SYMBOL_INFO _ -> false
  | LINT _ -> false
  | LINT_STDIN _ -> false
  | LINT_ALL _ -> false
  | DUMP_FULL_FIDELITY_PARSE _ -> false
  | RAGE -> false
  | CST_SEARCH _ -> false
  | CHECK_LIVENESS -> false
  | PAUSE -> true
  | FUN_DEPS_BATCH _ -> false
  | DEPS_OUT_BATCH _ -> false
  | FILE_DEPENDENTS _ -> true
  | FIND_ISOLATABLE_CLUSTERS _ -> true
  (* Reads the dependency graph to decide which files could reference the
     candidate, and the graph is only guaranteed current after a full check. *)
  | VALIDATE_ISOLATION _ -> true
  | VERBOSE _ -> false
  | DEPS_IN_BATCH _ -> true
  | PACKAGE_LINT _ -> true
  | PACKAGE_LINT_FULL _ -> true

let use_priority_pipe (command : 'result t) : bool =
  not (rpc_command_needs_full_check command)

let handle_after_send : type a. a t -> (a * unit t) option =
 fun cmd ->
  match cmd with
  | LOG_ERRORS _ -> Some ((), cmd)
  | _ -> None
