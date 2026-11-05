(*
 * Copyright (c) 2015, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)
open Hh_prelude
open Sys_utils
module Cls = Folded_class

[@@@alert "-dependencies"]
(* hh_single_type_check is not incremental, so
   * we don't need to be careful about dependency-tracking
*)

(*****************************************************************************)
(* Profiling utilities *)
(*****************************************************************************)

let mean samples =
  List.fold ~init:0.0 ~f:( +. ) samples /. Float.of_int (List.length samples)

let standard_deviation mean samples =
  let sosq =
    List.fold
      ~init:0.0
      ~f:(fun acc sample ->
        let diff = sample -. mean in
        (diff *. diff) +. acc)
      samples
  in
  let sosqm = sosq /. Float.of_int (List.length samples) in
  Float.sqrt sosqm

(*****************************************************************************)
(* Types, constants *)
(*****************************************************************************)

type mode =
  | Cst_search
  | Dump_symbol_info
  | RunSimpliHack
  | Glean_index of string
  | Glean_sym_hash
  | Dump_inheritance
  | Errors
  | Lint
  | Lint_json
  | Dump_deps
  | Dump_dep_hashes
  | Get_some_file_deps of {
      depth: int;
      full_hierarchy: bool;
    }
  | Identify_symbol of File_content.Position.t
  | Ide_code_actions of {
      title_prefix: string;
      use_snippet_edits: bool;
    }
  | Ide_diagnostics
  | Ide_signature_help
  | Find_local of File_content.Position.t
  | Get_member of string
  | Outline
  | Outline_for_agents
  | Dump_nast
  | Dump_stripped_tast
  | Dump_tast
  | Find_refs of File_content.Position.t
  | Highlight_refs of File_content.Position.t
  | Go_to_impl of File_content.Position.t
  | Hover of File_content.Position.t option
  | Enforcement of File_content.Position.t option
  | Enforcement_batch
  | Apply_quickfixes
  | Refactor_sound_dynamic of string * string * string
  | RemoveDeadUnsafeCasts
  | CountImpreciseTypes
  | Map_reduce_mode
  | Dump_classish_positions
  | Dump_dynamic_inference of { as_data: bool }

type options = {
  files: string list;
  extra_builtins: string list;
  mode: mode;
  error_format: Diagnostics.format option;
  no_builtins: bool;
  max_errors: int option;
  tcopt: Global_options.t;
  batch_mode: bool;
  out_extension: string;
  should_print_position: bool;
  custom_hhi_path: string option;
  profile_type_check_multi: int option;
  memtrace: string option;
  rust_provider_backend: bool;
  naming_table_path: string option;
  packages_config_path: string option;
  enable_implicit_packages: bool;
}

(** If the user passed --root, then all pathnames have to be canonicalized.
The fact of whether they passed --root is kind of stored inside Relative_path
global variables: the Relative_path.(path_of_prefix Root) is either "/"
if they failed to pass something, or the thing that they passed. *)
let use_canonical_filenames () =
  not (String.equal "/" (Relative_path.path_of_prefix Relative_path.Root))

(* Canonical builtins from our hhi library *)
let hhi_builtins = Hhi.get_raw_hhi_contents ()

(* All of the stuff that hh_single_type_check relies on is sadly not contained
 * in the hhi library, so we include a very small number of magic builtins *)
let magic_builtins =
  [|
    ( "hh_single_type_check_magic.hhi",
      "<?hh\n"
      ^ "namespace {\n"
      ^ "<<__NoAutoDynamic, __SupportDynamicType>> function hh_show<T>(<<__AcceptDisposable>> readonly T $val)[]:T {}\n"
      ^ "<<__NoAutoDynamic, __SupportDynamicType>> function hh_expect<T>(<<__AcceptDisposable>> readonly T $val)[]:T {}\n"
      ^ "<<__NoAutoDynamic, __SupportDynamicType>> function hh_expect_equivalent<T>(<<__AcceptDisposable>> readonly T $val)[]:T {}\n"
      ^ "<<__NoAutoDynamic>> function hh_show_env()[]:void {}\n"
      ^ "<<__NoAutoDynamic>> function hh_log_level(string $key, int $level)[]:void {}\n"
      ^ "<<__NoAutoDynamic>> function hh_force_solve()[]:void {}"
      ^ "<<__NoAutoDynamic>> function hh_time(string $command, string $tag = '_'):void {}\n"
      ^ "<<__NoAutoDynamic>> function hh_sleep(int $seconds)[]:void {}\n"
      ^ "}\n" );
  |]

(*****************************************************************************)
(* Helpers *)
(*****************************************************************************)

let die str =
  let oc = stderr in
  Out_channel.output_string oc str;
  Out_channel.close oc;
  exit 2

let print_error format ?(oc = stderr) l =
  let formatter =
    match format with
    | Diagnostics.Context ->
      (fun e -> Contextual_diagnostic_formatter.to_string e)
    | Diagnostics.Raw -> (fun e -> Raw_diagnostic_formatter.to_string e)
    | Diagnostics.Agent -> (fun e -> Agent_diagnostic_formatter.to_string e)
    | Diagnostics.Plain -> (fun e -> Diagnostics.to_string e)
    | Diagnostics.Highlighted -> Highlighted_diagnostic_formatter.to_string
    | Diagnostics.Extended -> Extended_diagnostic_formatter.to_string
    | Diagnostics.PlainHighlighted ->
      Plain_highlighted_diagnostic_formatter.to_string
  in
  let absolute_errors = User_diagnostic.to_absolute l in
  Out_channel.output_string oc (formatter absolute_errors)

let write_error_list format (errors : Diagnostics.t) oc max_errors =
  let errors = Diagnostics.get_sorted_diagnostic_list errors in
  let (shown_errors, dropped_errors) =
    match max_errors with
    | Some max_errors -> List.split_n errors max_errors
    | None -> (errors, [])
  in
  if not (List.is_empty errors) then (
    List.iter ~f:(print_error format ~oc) shown_errors;
    match
      Diagnostics.format_summary
        format
        ~error_count:(List.length errors)
        ~warning_count:0
        ~dropped_count:(Some (List.length dropped_errors))
        ~max_errors
    with
    | Some summary -> Out_channel.output_string oc summary
    | None -> ()
  ) else
    Out_channel.output_string oc "No errors\n";
  Out_channel.close oc

let print_error_list format (errors : Diagnostics.t) max_errors =
  let errors = Diagnostics.get_sorted_diagnostic_list errors in
  let (shown_errors, dropped_errors) =
    match max_errors with
    | Some max_errors -> List.split_n errors max_errors
    | None -> (errors, [])
  in
  if not (List.is_empty errors) then (
    List.iter ~f:(print_error format) shown_errors;
    match
      Diagnostics.format_summary
        format
        ~error_count:(List.length errors)
        ~warning_count:0
        ~dropped_count:(Some (List.length dropped_errors))
        ~max_errors
    with
    | Some summary -> Out_channel.output_string stderr summary
    | None -> ()
  ) else
    Printf.printf "No errors\n"

let print_errors format (errors : Diagnostics.t) max_errors : unit =
  print_error_list format errors max_errors

let print_errors_if_present (errors : Diagnostics.t) =
  if not (Diagnostics.has_no_errors_or_warnings errors) then (
    let errors_output =
      Diagnostics.convert_errors_to_string
      @@ Diagnostics.get_sorted_diagnostic_list errors
    in
    Printf.printf "Errors:\n";
    List.iter errors_output ~f:(fun err_output ->
        Printf.printf "  %s\n" err_output)
  )

let comma_string_to_iset (s : string) : I_set.t =
  Str.split (Str.regexp ", *") s |> List.map ~f:int_of_string |> I_set.of_list

let load_and_parse_custom_error_config path =
  match Custom_error_config.initialize (`Absolute path) with
  | Ok cfg ->
    if not @@ List.is_empty cfg.Custom_error_config.invalid then
      eprintf
        "Encountered invalid rules with loading custom error config: \n %s\n"
      @@ String.concat ~sep:"\n"
      @@ List.map ~f:(fun Custom_error.{ name; _ } -> name)
      @@ cfg.Custom_error_config.invalid;
    Some cfg
  | Error msg ->
    eprintf
      "Encountered and error when loading custom error config: \n %s\n"
      msg;
    None

(* Fatal on invalid keys: hh_single_type_check has no release/rollback
   concerns, so we fail fast to catch typos immediately. *)
let exit_if_invalid_config_keys (config : Config_file_common.t) : unit =
  let config_keys = Config_file_common.keys config in
  List.iter config_keys ~f:(fun key ->
      match Config_keys.validate ~config_key:key with
      | Ok () -> ()
      | Error (Config_keys.Did_you_mean suggestion) ->
        let msg =
          match suggestion with
          | None -> Printf.sprintf "Unknown config option: %s\n" key
          | Some s ->
            Printf.sprintf "Unknown config option: %s. Did you mean %s?\n" key s
        in
        Out_channel.output_string Out_channel.stderr msg;
        Out_channel.flush Out_channel.stderr;
        Exit.exit Exit_status.Input_error)

let parse_options () =
  let fn_ref = ref [] in
  let extra_builtins = ref [] in
  let usage = Printf.sprintf "Usage: %s filename\n" Sys.argv.(0) in
  let mode = ref Errors in
  let no_builtins = ref false in
  let log_levels = ref S_map.empty in
  let max_errors = ref None in
  let batch_mode = ref false in
  let set_mode x () =
    match !mode with
    | Errors -> mode := x
    | _ -> raise (Arg.Bad "only a single mode should be specified")
  in
  let config_overrides = ref [] in
  let error_format = ref None in
  let deregister_attributes = ref None in
  let auto_namespace_map = ref None in
  let log_inference_constraints = ref None in
  let timeout = ref None in
  let set_bool x () = x := Some true in
  let set_bool_ x () = x := true in
  let rust_provider_backend = ref Hh_server_provider_backend.is_supported in
  let skip_hierarchy_checks = ref false in
  let skip_tast_checks = ref false in
  let skip_check_under_dynamic = ref false in
  let out_extension = ref ".out" in
  let strict_contexts = ref true in

  let enable_supportdyn_hint = ref false in
  let glean_reponame = ref (Glean_options.reponame Global_options.default) in
  let disable_xhp_element_mangling = ref false in
  let keep_user_attributes = ref false in
  let enable_xhp_class_modifier = ref false in
  let allowed_fixme_codes_strict = ref None in
  let allowed_decl_fixme_codes = ref None in
  let report_pos_from_reason = ref false in
  let consider_type_const_enforceable = ref false in
  let interpret_soft_types_as_like_types = ref false in
  let meth_caller_only_public_visibility = ref true in
  let naming_table = ref None in
  let root = ref None in
  let sharedmem_config = ref Shared_mem.default_config in
  let print_position = ref true in
  let custom_hhi_path = ref None in
  let force_allow_builtins_in_custom_hhi_path = ref false in
  let explicit_consistent_constructors = ref 0 in
  let require_types_class_consts = ref 0 in
  let type_printer_fuel =
    ref (Typechecker_options.type_printer_fuel Global_options.default)
  in
  let profile_type_check_multi = ref None in
  let profile_top_level_definitions =
    ref
      (Typechecker_options.profile_top_level_definitions Global_options.default)
  in
  let memtrace = ref None in
  let enable_global_access_check = ref false in
  let custom_error_config_path = ref None in
  let allow_all_files_for_module_declarations = ref true in
  let loop_iteration_upper_bound = ref None in
  let get_some_file_deps_full_hierarchy = ref false in
  let disallow_non_annotated_memoize = ref false in
  let treat_non_annotated_memoize_as_kbic = ref false in
  let options =
    [
      ( "--config",
        Arg.String (fun s -> config_overrides := s :: !config_overrides),
        "<option=value> Set one hhconfig option; can be used multiple times" );
      ( "--no-print-position",
        Arg.Unit (fun _ -> print_position := false),
        " Don't print positions while printing TASTs and NASTs" );
      ( "--naming-table",
        Arg.String (fun s -> naming_table := Some s),
        " Naming table, to look up undefined symbols; needs --root."
        ^ " (Hint: buck2 run //hphp/hack/src/hh_naming_table_builder)" );
      ( "--root",
        Arg.String (fun s -> root := Some s),
        " Root for where to look up undefined symbols; needs --naming-table" );
      ( "--extra-builtin",
        Arg.String (fun f -> extra_builtins := f :: !extra_builtins),
        " HHI file to parse and declare" );
      ( "--refactor-sound-dynamic",
        (let refactor_analysis_mode = ref "" in
         let refactor_mode = ref "" in
         Arg.Tuple
           [
             Arg.String (( := ) refactor_analysis_mode);
             Arg.String (( := ) refactor_mode);
             Arg.String
               (fun x ->
                 batch_mode := true;
                 set_mode
                   (Refactor_sound_dynamic
                      (!refactor_analysis_mode, !refactor_mode, x))
                   ());
           ]),
        " Run the flow analysis" );
      ( "--deregister-attributes",
        Arg.Unit (set_bool deregister_attributes),
        " Ignore all functions with attribute '__PHPStdLib'" );
      ( "--auto-namespace-map",
        Arg.String
          (fun m ->
            auto_namespace_map :=
              Some (Server_config.convert_auto_namespace_to_map m)),
        " Alias namespaces" );
      ( "--no-strict-contexts",
        Arg.Unit (fun () -> strict_contexts := false),
        " Do not enforce contexts to be defined within Contexts namespace" );
      ( "--cst-search",
        Arg.Unit (set_mode Cst_search),
        " Search the concrete syntax tree of the given file using the pattern"
        ^ " given on stdin."
        ^ " (The pattern is a JSON object adhering to the search DSL.)" );
      ( "--dump-symbol-info",
        Arg.Unit (set_mode Dump_symbol_info),
        " Dump all symbol information" );
      ( "--run-simplihack",
        Arg.Unit
          (fun () ->
            skip_tast_checks := true;
            set_mode RunSimpliHack ()),
        " Run SimpliHack on the file and print the prompts that are produced" );
      ( "--glean-index",
        Arg.String (fun output_dir -> set_mode (Glean_index output_dir) ()),
        " Run indexer and output json in provided dir" );
      ( "--glean-sym-hash",
        Arg.Unit (set_mode Glean_sym_hash),
        " Print symbols hashes used by incremental indexer" );
      ( "--error-format",
        Arg.String
          (fun s ->
            match s with
            | "raw" -> error_format := Some Diagnostics.Raw
            | "agent" -> error_format := Some Diagnostics.Agent
            | "context" -> error_format := Some Diagnostics.Context
            | "highlighted" -> error_format := Some Diagnostics.Highlighted
            | "plain" -> error_format := Some Diagnostics.Plain
            | "extended" -> error_format := Some Diagnostics.Extended
            | "plain_highlighted" ->
              error_format := Some Diagnostics.PlainHighlighted
            | _ -> print_string "Warning: unrecognized error format.\n"),
        "<format> Error formatting style (default: highlighted); valid formats: agent, extended, raw, context, highlighted, plain, plain_highlighted"
      );
      ("--lint", Arg.Unit (set_mode Lint), " Produce lint errors");
      ("--lint-json", Arg.Unit (set_mode Lint_json), " Produce json lint output");
      ( "--no-builtins",
        Arg.Set no_builtins,
        " Don't use builtins (e.g. ConstSet); implied by --root" );
      ( "--out-extension",
        Arg.String (fun s -> out_extension := s),
        " output file extension (default .out)" );
      ("--dump-deps", Arg.Unit (set_mode Dump_deps), " Print dependencies");
      ( "--dump-dep-hashes",
        Arg.Unit (set_mode Dump_dep_hashes),
        " Print dependency hashes" );
      ( "--dump-inheritance",
        Arg.Unit (set_mode Dump_inheritance),
        " Print inheritance" );
      ( "--get-some-file-deps",
        Arg.Int
          (fun depth ->
            set_mode (Get_some_file_deps { depth; full_hierarchy = false }) ()),
        " Print a list of files this file depends on. The provided integer is the depth of the traversal. Requires --root, --naming-table and --depth"
      );
      ( "--get-some-file-deps-full-hierarchy",
        Arg.Set get_some_file_deps_full_hierarchy,
        " Include all dependencies from class hierarchies in the list of files this file depends on regardless of their depth."
      );
      ( "--ide-code-actions",
        Arg.String
          (fun title_prefix ->
            set_mode
              (Ide_code_actions { title_prefix; use_snippet_edits = true })
              ()),
        "<title_prefix> Apply a code action with the given title prefix to the given file, where the selection is indicated with markers in comments: "
        ^ "`/*range-start*/the_code_here/*range-end*/` or `^ at-caret` with the caret pointing to the previous line."
      );
      ( "--ide-code-actions-no-experimental-capabilities",
        Arg.String
          (fun title_prefix ->
            set_mode
              (Ide_code_actions { title_prefix; use_snippet_edits = false })
              ()),
        "<title_prefix> Like --ide-code-actions, but do not use any nonstandard LSP features (experimental capabilities)."
      );
      ( "--ide-diagnostics",
        Arg.Unit (set_mode Ide_diagnostics),
        " Compute where IDE diagnostics (squiggles and dotted lines) will be displayed."
      );
      ( "--ide-signature-help",
        Arg.Unit (set_mode Ide_signature_help),
        " Show signature help at the position indicated by `^ at-caret` comment."
        ^ "Exercises most of the functionality of textDocument/signatureHelp."
        (* https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/#signatureHelp *)
      );
      ( "--dump-classish-positions",
        Arg.Unit (set_mode Dump_classish_positions),
        " Dump certain positional information calculated for the given file." );
      ( "--identify-symbol",
        (let line = ref 0 in
         Arg.Tuple
           [
             Arg.Int (( := ) line);
             Arg.Int
               (fun column ->
                 set_mode
                   (Identify_symbol
                      (File_content.Position.from_one_based !line column))
                   ());
           ]),
        "<pos> Show info about symbol at given line and column" );
      ( "--find-local",
        (let line = ref 0 in
         Arg.Tuple
           [
             Arg.Int (( := ) line);
             Arg.Int
               (fun column ->
                 set_mode
                   (Find_local
                      (File_content.Position.from_one_based !line column))
                   ());
           ]),
        "<pos> Find all usages of local at given line and column" );
      ( "--max-errors",
        Arg.Int (fun num_errors -> max_errors := Some num_errors),
        " Maximum number of errors to display" );
      ("--outline", Arg.Unit (set_mode Outline), " Print file outline");
      ( "--outline-for-agents",
        Arg.Unit (set_mode Outline_for_agents),
        " <file> (mode) outline the file, including line spans, to help agents efficiently navigate large files."
      );
      ("--nast", Arg.Unit (set_mode Dump_nast), " Print out the named AST");
      ("--tast", Arg.Unit (set_mode Dump_tast), " Print out the typed AST");
      ( "--infer-dynamic",
        Arg.Unit (set_mode (Dump_dynamic_inference { as_data = false })),
        " Infer types for dynamic locals" );
      ( "--infer-dynamic-as-data",
        Arg.Unit (set_mode (Dump_dynamic_inference { as_data = true })),
        " Infer types for dynamic locals, interpreting indexed access as shapes/vecs"
      );
      ( "--stripped-tast",
        Arg.Unit (set_mode Dump_stripped_tast),
        " Print out the typed AST, stripped of type information."
        ^ " This can be compared against the named AST to look for holes." );
      ( "--find-refs",
        (let line = ref 0 in
         Arg.Tuple
           [
             Arg.Int (( := ) line);
             Arg.Int
               (fun column ->
                 set_mode
                   (Find_refs
                      (File_content.Position.from_one_based !line column))
                   ());
           ]),
        "<pos> Find all usages of a symbol at given line and column" );
      ( "--go-to-impl",
        Arg.Tuple
          (let line = ref 0 in
           [
             Arg.Int (( := ) line);
             Arg.Int
               (fun column ->
                 set_mode
                   (Go_to_impl
                      (File_content.Position.from_one_based !line column))
                   ());
           ]),
        "<pos> Find all implementations of a symbol at given line and column" );
      ( "--highlight-refs",
        (let line = ref 0 in
         Arg.Tuple
           [
             Arg.Int (( := ) line);
             Arg.Int
               (fun column ->
                 set_mode
                   (Highlight_refs
                      (File_content.Position.from_one_based !line column))
                   ());
           ]),
        "<pos> Highlight all usages of a symbol at given line and column" );
      ( "--get-member",
        Arg.String
          (fun class_and_member_id ->
            set_mode (Get_member class_and_member_id) ()),
        " Given ClassName::MemberName, fetch the decl of members with that name and print them."
      );
      ( "--log-inference-constraints",
        Arg.Unit (set_bool log_inference_constraints),
        " Log inference constraints to Scuba." );
      ( "--timeout",
        Arg.Int (fun secs -> timeout := Some secs),
        " Timeout in seconds for checking a function or a class." );
      ( "--hh-log-level",
        (let log_key = ref "" in
         Arg.Tuple
           [
             Arg.String (( := ) log_key);
             Arg.Int
               (fun level -> log_levels := S_map.add !log_key level !log_levels);
           ]),
        " Set the log level for a key" );
      ( "--batch-files",
        Arg.Set batch_mode,
        " Typecheck each file passed in independently" );
      ( "--skip-hierarchy-checks",
        Arg.Set skip_hierarchy_checks,
        " Do not apply checks on class hierarchy (override, implements, etc)" );
      ( "--skip-tast-checks",
        Arg.Set skip_tast_checks,
        " Do not apply checks using TAST visitors" );
      ( "--skip-check-under-dynamic",
        Arg.Set skip_check_under_dynamic,
        " Do not apply second check to functions and methods under dynamic assumptions"
      );
      ( "--implicit-pess",
        Arg.Unit (fun () -> set_bool_ enable_supportdyn_hint ()),
        " Enables implicit pessimisation" );
      ( "--explicit-pess",
        Arg.String
          (fun dir ->
            set_bool_ enable_supportdyn_hint ();
            custom_hhi_path := Some dir),
        " Enables checking explicitly pessimised files. Requires path to pessimised .hhi files "
      );
      ( "--enable-supportdyn-hint",
        Arg.Set enable_supportdyn_hint,
        " Allow the supportdyn type hint" );
      ( "--glean-reponame",
        Arg.String (fun str -> glean_reponame := str),
        " glean repo name" );
      ( "--disable-xhp-element-mangling",
        Arg.Set disable_xhp_element_mangling,
        " Disable mangling of XHP elements :foo. That is, :foo:bar is now \\foo\\bar, not xhp_foo__bar"
      );
      ( "--keep-user-attributes",
        Arg.Set keep_user_attributes,
        " Keep user attributes when parsing decls" );
      ( "--enable-xhp-class-modifier",
        Arg.Set enable_xhp_class_modifier,
        " Enable the XHP class modifier, xhp class name {} will define an xhp class."
      );
      ( "--allowed-fixme-codes-strict",
        Arg.String
          (fun s -> allowed_fixme_codes_strict := Some (comma_string_to_iset s)),
        " List of fixmes that are allowed in strict mode." );
      ( "--allowed-decl-fixme-codes",
        Arg.String
          (fun s -> allowed_decl_fixme_codes := Some (comma_string_to_iset s)),
        " List of fixmes that are allowed in declarations." );
      ( "--report-pos-from-reason",
        Arg.Set report_pos_from_reason,
        " Flag errors whose position is derived from reason information in types."
      );
      ( "--consider-type-const-enforceable",
        Arg.Set consider_type_const_enforceable,
        " Consider type constants to potentially be enforceable." );
      ( "--interpret-soft-types-as-like-types",
        Arg.Set interpret_soft_types_as_like_types,
        " Types declared with <<__Soft>> (runtime logs but doesn't throw) become like types."
      );
      ( "--meth-caller-only-public-visibility",
        Arg.Bool (fun x -> meth_caller_only_public_visibility := x),
        " Controls whether meth_caller can be used on non-public methods" );
      ( "--hover",
        (let line = ref 0 in
         Arg.Tuple
           [
             Arg.Int (fun x -> line := x);
             Arg.Int
               (fun column ->
                 set_mode
                   (Hover
                      (Some (File_content.Position.from_one_based !line column)))
                   ());
           ]),
        "<pos> Display hover tooltip" );
      ( "--hover-at-caret",
        Arg.Unit (fun () -> set_mode (Hover None) ()),
        " Show the hover information indicated by // ^ hover-at-caret" );
      ( "--enforcement",
        (let line = ref 0 in
         Arg.Tuple
           [
             Arg.Int (fun x -> line := x);
             Arg.Int
               (fun column ->
                 set_mode
                   (Enforcement
                      (Some (File_content.Position.from_one_based !line column)))
                   ());
           ]),
        "<pos> Display enforcement at position" );
      ( "--enforcement-at-caret",
        Arg.Unit (fun () -> set_mode (Enforcement None) ()),
        " Show the enforcement indicated by // ^ enforcement-at-caret" );
      ( "--enforcement-at-caret-batch",
        Arg.Unit (fun () -> set_mode Enforcement_batch ()),
        " Show enforcement at all // ^ enforcement-at-caret positions" );
      ( "--fix",
        Arg.Unit (fun () -> set_mode Apply_quickfixes ()),
        " Apply quickfixes for all the errors in the file, and print the resulting code. Prefer --ide-code-actions, which tests more."
      );
      ( "--custom-hhi-path",
        Arg.String (fun s -> custom_hhi_path := Some s),
        " Use custom hhis" );
      ( "--force-allow-builtins-in-custom-hhi-path-FLAKEY",
        Arg.Set force_allow_builtins_in_custom_hhi_path,
        " (FLAKEY) allow builtins to be written into the custom hhi path" );
      ( "--explicit-consistent-constructors",
        Arg.Int (( := ) explicit_consistent_constructors),
        " Raise an error for <<__ConsistentConstruct>> without an explicit constructor; 1 for traits, 2 for all "
      );
      ( "--require-types-class-consts",
        Arg.Int (( := ) require_types_class_consts),
        " Raise an error for class constants missing types; 1 for abstract constants, 2 for all "
      );
      ( "--profile-type-check-twice",
        Arg.Unit (fun () -> profile_type_check_multi := Some 1),
        " Typecheck the file twice" );
      ( "--profile-type-check-multi",
        Arg.Int (fun n -> profile_type_check_multi := Some n),
        " Typecheck the files n times extra (!)" );
      ( "--profile-top-level-definitions",
        Arg.Set profile_top_level_definitions,
        " Profile typechecking of top-level definitions" );
      ( "--memtrace",
        Arg.String (fun s -> memtrace := Some s),
        " Write memtrace to this file (typical extension .ctf)" );
      ( "--type-printer-fuel",
        Arg.Int (( := ) type_printer_fuel),
        " Sets the amount of fuel that the type printer can use to display an individual type. Default: "
        ^ string_of_int
            (Typechecker_options.type_printer_fuel Global_options.default) );
      ( "--enable-global-access-check",
        Arg.Set enable_global_access_check,
        " Run global access checker to check global writes and reads" );
      ( "--overwrite-loop-iteration-upper-bound",
        Arg.Int (fun u -> loop_iteration_upper_bound := Some u),
        " Sets the maximum number of iterations that will be used to typecheck loops"
      );
      ( "--remove-dead-unsafe-casts",
        Arg.Unit (fun () -> set_mode RemoveDeadUnsafeCasts ()),
        " Removes dead unsafe casts from a file" );
      ( "--count-imprecise-types",
        Arg.Unit (fun () -> set_mode CountImpreciseTypes ()),
        " Counts the number of mixed, dynamic, and nonnull types in a file" );
      ( "--custom-error-config-path",
        Arg.String (fun s -> custom_error_config_path := Some s),
        " Config file for custom error messages" );
      ( "--map-reduce",
        Arg.Unit (set_mode Map_reduce_mode),
        " Run the map reducers and print the result" );
    ]
  in

  (* Sanity check that all option descriptions are well-formed. *)
  List.iter options ~f:(fun (_, _, description) ->
      if
        String.is_prefix description ~prefix:" "
        || String.is_prefix description ~prefix:"<"
      then
        ()
      else
        failwith
          (Printf.sprintf
             "Descriptions should start with <foo> or a leading space, got: %S"
             description));

  let options = Arg.align ~limit:25 options in
  Arg.parse options (fun fn -> fn_ref := fn :: !fn_ref) usage;
  let fns =
    match (!fn_ref, !mode) with
    | ([], Get_member _) -> []
    | ([], _) -> die usage
    | (x, _) -> x
  in

  (match !mode with
  | Get_some_file_deps _ when Option.is_none !root ->
    raise (Arg.Bad "--get-some-file-deps requires --root")
  | _ -> ());
  if Option.is_some !naming_table && Option.is_none !root then
    failwith "--naming-table needs --root";

  (* Unless --no-builtins, then well write into the supplied directory, which would be bad. *)
  if
    Option.is_some !custom_hhi_path
    && (not !no_builtins)
    && not !force_allow_builtins_in_custom_hhi_path
  then
    failwith "--custom-hhi-path needs --no-builtins";

  let config =
    List.fold
      (List.rev !config_overrides)
      ~init:(Config_file_common.empty ())
      ~f:(fun config setting ->
        let c = Config_file_common.parse_contents setting in
        Config_file_common.apply_overrides ~config ~overrides:c ~log_reason:None)
  in
  let ( >?? ) x y = Option.value x ~default:y in
  let () = exit_if_invalid_config_keys config in
  let rust_provider_backend =
    config
    |> Config_file.Getters.bool_opt Config_keys.Hhconf.rust_provider_backend
    >?? !rust_provider_backend
  in
  let packages_config_path =
    config
    |> Config_file.Getters.string_opt Config_keys.Hhconfig.packages_config_path
  in
  let enable_implicit_packages =
    config
    |> Config_file.Getters.bool_
         Config_keys.Hhconfig.enable_implicit_packages
         ~default:false
  in
  (* --root implies certain things... *)
  let root =
    match !root with
    | None ->
      (match packages_config_path with
      | Some _ ->
        Path.make (Sys.getcwd ())
        (* the package_config path supplied as a --config value is relative to the
           parent directory of .hhconfig; to mimic that behavior, use the current working
           directory as the root *)
      | None -> Path.make "/" (* otherwise, we use this dummy *))
    | Some root ->
      if Option.is_none !naming_table then
        naming_table := Some (Hh_single_common.find_naming_table_or_fail ());
      (* Following will throw an exception if .hhconfig not found *)
      let config =
        Config_file.parse_hhconfig
          (Filename.concat root Config_file.file_path_relative_to_repo_root)
      in
      (* We'll pick up values from .hhconfig, then run [Arg.parse] a second time to let CLI options override. *)
      auto_namespace_map :=
        config
        |> Config_file.Getters.string_opt
             Config_keys.Hhconfig.auto_namespace_map
        |> Option.map ~f:Server_config.convert_auto_namespace_to_map;
      allowed_fixme_codes_strict :=
        config
        |> Config_file.Getters.string_opt
             Config_keys.Hhconfig.allowed_fixme_codes_strict
        |> Option.map ~f:comma_string_to_iset;
      sharedmem_config :=
        Server_config.make_sharedmem_config
          config
          Server_local_config_load.default;
      no_builtins := true;
      (* Now let CLI options override whatever we just picked *)
      Arg.parse options (fun _ -> ()) usage;
      (* Final validation *)
      if not !no_builtins then failwith "--root needs --no-builtins";
      (* Path.make canonicalizes it, i.e. resolves symlinks *)
      Path.make root
  in

  (match !mode with
  | Get_some_file_deps { depth; _ } ->
    if Option.is_none !naming_table then
      raise (Arg.Bad "--get-some-file-deps requires --naming-table");
    mode :=
      Get_some_file_deps
        { depth; full_hierarchy = !get_some_file_deps_full_hierarchy }
  | _ -> ());

  let po =
    Parser_options.
      {
        (* These don't have command-line flags, so init them to their defaults. *)
        hhvm_compat_mode = default.hhvm_compat_mode;
        hhi_mode = default.hhi_mode;
        codegen = default.codegen;
        stack_size = default.stack_size;
        unwrap_concurrent = default.unwrap_concurrent;
        no_parser_readonly_check = default.no_parser_readonly_check;
        experimental_features = default.experimental_features;
        consider_unspecified_experimental_features_released =
          default.consider_unspecified_experimental_features_released;
        (* These are set specifically for single type check *)
        use_legacy_experimental_feature_config = false;
        allow_unstable_features = true;
        (* The remainder are set by the command line options *)
        is_systemlib = default.is_systemlib;
        const_static_props = default.const_static_props;
        abstract_static_props = default.abstract_static_props;
        disable_xhp_element_mangling = !disable_xhp_element_mangling;
        disable_xhp_children_declarations =
          default.disable_xhp_children_declarations;
        enable_xhp_class_modifier = !enable_xhp_class_modifier;
        interpret_soft_types_as_like_types = !interpret_soft_types_as_like_types;
        keep_user_attributes = !keep_user_attributes;
        auto_namespace_map = !auto_namespace_map >?? default.auto_namespace_map;
        deregister_php_stdlib =
          !deregister_attributes >?? default.deregister_php_stdlib;
        everything_sdt = true;
        include_enum_member_values = default.include_enum_member_values;
        include_const_shape_key_values = default.include_const_shape_key_values;
        union_intersection_type_hints = default.union_intersection_type_hints;
        allowed_decl_fixme_codes =
          Option.value !allowed_decl_fixme_codes ~default:I_set.empty;
        package_info = default.package_info;
        package_support_multifile_tests =
          default.package_support_multifile_tests;
        disallow_non_annotated_memoize = !disallow_non_annotated_memoize;
        treat_non_annotated_memoize_as_kbic =
          !treat_non_annotated_memoize_as_kbic;
        ignore_string_methods = default.ignore_string_methods;
        enable_intrinsics_extension = default.enable_intrinsics_extension;
        expression_tree_shape_no_unwrap =
          default.expression_tree_shape_no_unwrap;
        allow_single_statement_concurrent =
          default.allow_single_statement_concurrent;
      }
  in

  let tcopt : Global_options.t =
    Global_options.set
      ~po
      ~tco_saved_state:Global_options.default_saved_state
      ?tco_log_inference_constraints:!log_inference_constraints
      ?tco_timeout:!timeout
      ~allowed_fixme_codes_strict:
        (Option.value !allowed_fixme_codes_strict ~default:I_set.empty)
      ~tco_skip_hierarchy_checks:!skip_hierarchy_checks
      ~tco_skip_tast_checks:!skip_tast_checks
      ~tco_strict_contexts:!strict_contexts
      ~tco_like_casts:false
      ~log_levels:!log_levels
      ~glean_reponame:!glean_reponame
      ~tco_skip_check_under_dynamic:!skip_check_under_dynamic
      ~tco_global_access_check_enabled:!enable_global_access_check
      ~tco_meth_caller_only_public_visibility:
        !meth_caller_only_public_visibility
      ~tco_explicit_consistent_constructors:!explicit_consistent_constructors
      ~tco_require_types_class_consts:!require_types_class_consts
      ~tco_type_printer_fuel:!type_printer_fuel
      ~tco_profile_top_level_definitions:!profile_top_level_definitions
      ~tco_allow_all_files_for_module_declarations:
        !allow_all_files_for_module_declarations
      ~tco_loop_iteration_upper_bound:!loop_iteration_upper_bound
      Global_options.default
  in
  let tcopt = Server_config.load_config config tcopt in

  Diagnostics.allowed_fixme_codes_strict :=
    Global_options.allowed_fixme_codes_strict tcopt;

  let tco_legacy_experimental_features =
    tcopt.Global_options.tco_legacy_experimental_features
  in
  let tco_legacy_experimental_features =
    if !enable_supportdyn_hint then
      S_set.add
        Typechecker_options.experimental_supportdynamic_type_hint
        tco_legacy_experimental_features
    else
      tco_legacy_experimental_features
  in
  let tco_legacy_experimental_features =
    if !consider_type_const_enforceable then
      S_set.add
        Typechecker_options.experimental_consider_type_const_enforceable
        tco_legacy_experimental_features
    else
      tco_legacy_experimental_features
  in

  let tcopt = { tcopt with Global_options.tco_legacy_experimental_features } in
  let tco_custom_error_config =
    Option.value ~default:Custom_error_config.empty
    @@ Option.bind
         ~f:load_and_parse_custom_error_config
         !custom_error_config_path
  in
  let tcopt = Global_options.{ tcopt with tco_custom_error_config } in
  ( {
      files = fns;
      extra_builtins = !extra_builtins;
      mode = !mode;
      no_builtins = !no_builtins;
      max_errors = !max_errors;
      error_format = !error_format;
      tcopt;
      batch_mode = !batch_mode;
      out_extension = !out_extension;
      should_print_position = !print_position;
      custom_hhi_path = !custom_hhi_path;
      profile_type_check_multi = !profile_type_check_multi;
      memtrace = !memtrace;
      rust_provider_backend;
      naming_table_path = !naming_table;
      packages_config_path;
      enable_implicit_packages;
    },
    root,
    if rust_provider_backend then
      Shared_mem.
        {
          !sharedmem_config with
          shm_use_sharded_hashtbl = true;
          shm_cache_size =
            max !sharedmem_config.shm_cache_size (2 * 1024 * 1024 * 1024);
        }
    else
      !sharedmem_config )

let print_elapsed fn desc ~start_time =
  let elapsed_ms = Float.(Unix.gettimeofday () - start_time) *. 1000. in
  Printf.printf
    "%s: %s - %0.2fms\n"
    (Relative_path.to_absolute fn |> Filename.basename)
    desc
    elapsed_ms

let check_file
    ctx (errors : Diagnostics.t) files_info ~profile_type_check_multi ~memtrace
    : Diagnostics.t =
  let profiling = Option.is_some profile_type_check_multi in
  if profiling then
    Relative_path.Map.iter files_info ~f:(fun fn (_fileinfo : File_info.t) ->
        let full_ast = Ast_provider.get_ast ctx fn ~full:true in
        let start_time = Unix.gettimeofday () in
        let _ = Typing_check_job.calc_errors_and_tast ctx fn ~full_ast in
        print_elapsed fn "first typecheck+decl" ~start_time);
  let tracer =
    Option.map memtrace ~f:(fun filename ->
        Memtrace.start_tracing
          ~context:None
          ~sampling_rate:Memtrace.default_sampling_rate
          ~filename)
  in
  let add_timing fn timings closure =
    let start_cpu = Sys.time () in
    let result = Lazy.force closure in
    let elapsed_cpu_time = Sys.time () -. start_cpu in
    let add_sample = function
      | Some samples -> Some (elapsed_cpu_time :: samples)
      | None -> Some [elapsed_cpu_time]
    in
    let timings = Relative_path.Map.update fn add_sample timings in
    (result, timings)
  in
  let rec go n timings : Diagnostics.t * _ =
    let (errors, timings) =
      Relative_path.Map.fold
        files_info
        ~f:(fun fn _fileinfo (errors, timings) ->
          let full_ast = Ast_provider.get_ast ctx fn ~full:true in
          let ((new_errors, _tast), timings) =
            add_timing fn timings
            @@ lazy (Typing_check_job.calc_errors_and_tast ctx fn ~full_ast)
          in
          (Diagnostics.merge errors new_errors, timings))
        ~init:(errors, timings)
    in
    if n > 1 then
      go (n - 1) timings
    else
      (errors, timings)
  in
  let n_of_times_to_typecheck =
    max 1 (Option.value ~default:1 profile_type_check_multi)
  in
  let timings = Relative_path.Map.empty in
  let (errors, timings) = go n_of_times_to_typecheck timings in
  let print_elapsed_cpu_time fn samples =
    let mean = mean samples in
    Printf.printf
      "%s: %d typechecks - %f ± %f (s)\n"
      (Relative_path.to_absolute fn |> Filename.basename)
      n_of_times_to_typecheck
      mean
      (standard_deviation mean samples)
  in
  if profiling then Relative_path.Map.iter timings ~f:print_elapsed_cpu_time;
  Option.iter tracer ~f:Memtrace.stop_tracing;
  errors

let create_nasts ctx files_info =
  let build_nast fn _ =
    let (syntax_errors, ast) =
      Ast_provider.get_ast_with_error ~full:true ctx fn
    in
    let error_list = Diagnostics.get_sorted_diagnostic_list syntax_errors in
    List.iter error_list ~f:Diagnostics.add_diagnostic;
    Naming.program ctx ast
  in
  Relative_path.Map.mapi ~f:build_nast files_info

(** This is an almost-pure function which returns what we get out of parsing.
The only side-effect it has is on the global errors list and the AST provider. *)
let parse_and_name ctx files_contents =
  Relative_path.Map.mapi files_contents ~f:(fun fn contents ->
      (* Get parse errors. *)
      let () =
        Diagnostics.run_in_context fn (fun () ->
            let popt = Provider_context.get_popt ctx in
            let parsed_file =
              Full_fidelity_ast.defensive_program popt fn contents
            in
            let ast =
              let { Parser_return.ast; _ } = parsed_file in
              if popt.Parser_options.deregister_php_stdlib then
                Nast.deregister_ignored_attributes ast
              else
                ast
            in
            Ast_provider.provide_ast_hint fn ast Ast_provider.Full;
            ())
      in
      match Direct_decl_utils.direct_decl_parse ctx fn with
      | None -> failwith "no file contents"
      | Some decls -> Direct_decl_utils.decls_to_fileinfo fn decls)

(** This function is used for gathering naming and parsing errors,
and the side-effect of updating the global reverse naming table (and
picking up duplicate-name errors along the way), and for the side effect
of updating the decl heap (and picking up decling errors along the way). *)
let parse_name_and_decl ctx files_contents =
  Diagnostics.do_ (fun () ->
      let files_info = parse_and_name ctx files_contents in
      Relative_path.Map.iter files_info ~f:(fun fn fileinfo ->
          let _failed_naming_fns =
            Naming_global.ndecl_file_and_get_conflict_files
              ctx
              fn
              fileinfo.File_info.ids
          in
          ());
      Relative_path.Map.iter files_info ~f:(fun fn _ ->
          Decl.make_env ~sh:Shared_mem.Uses ctx fn);

      files_info)

let compute_nasts ctx files_info interesting_files =
  let nasts = create_nasts ctx files_info in
  (* Interesting files are usually the non hhi ones. *)
  let filter_non_interesting nasts =
    Relative_path.Map.merge nasts interesting_files ~f:(fun _k nast x ->
        match (nast, x) with
        | (Some nast, Some _) -> Some nast
        | _ -> None)
  in
  filter_non_interesting nasts

(* Returns a list of Tast defs, along with associated type environments. *)
let compute_tasts ?(drop_fixmed = true) ctx files_info interesting_files :
    Diagnostics.t * Tast.program Relative_path.Map.t =
  let _f _k nast x =
    match (nast, x) with
    | (Some nast, Some _) -> Some nast
    | _ -> None
  in
  Diagnostics.do_ ~drop_fixmed (fun () ->
      let nasts = compute_nasts ctx files_info interesting_files in
      let tasts =
        Relative_path.Map.map nasts ~f:(fun nast ->
            let tast =
              Typing_toplevel.nast_to_tast ~do_tast_checks:true ctx nast
            in
            List.concat (Tast_with_dynamic.all tast))
      in
      tasts)

let compute_tasts_by_name ?(drop_fixmed = true) ctx files_info interesting_files
    : Diagnostics.t * Tast.by_names Relative_path.Map.t =
  let (nast_errors, nasts) =
    Diagnostics.do_ ~drop_fixmed (fun () ->
        compute_nasts ctx files_info interesting_files)
  in
  let errors_and_tasts =
    Relative_path.Map.mapi nasts ~f:(fun fn full_ast ->
        Typing_check_job.calc_errors_and_tast ctx ~drop_fixmed fn ~full_ast)
  in
  let tasts = Relative_path.Map.map ~f:snd errors_and_tasts in
  let tast_errors =
    Relative_path.Map.values errors_and_tasts |> List.map ~f:fst
  in

  ( List.fold
      (nast_errors :: tast_errors)
      ~init:Diagnostics.empty
      ~f:Diagnostics.merge,
    tasts )

let caret_pos (src : string) (marker : string) : _ option =
  String.split_lines src
  |> List.findi ~f:(fun _ line -> String.is_substring line ~substring:marker)
  |> Option.map ~f:(fun (line_num, line_src) ->
         let col_num =
           String.lfindi line_src ~f:(fun _ c ->
               match c with
               | '^' -> true
               | _ -> false)
         in
         File_content.Position.from_one_based
           line_num
           (Option.value_exn col_num + 1))

(* Given source code containing a caret marker (e.g. "^ hover-at-caret"), return
   the line and column of the position indicated. *)
let caret_pos_exn (src : string) (marker : string) =
  caret_pos src marker
  |> Option.value_exn
       ~message:
         (Printf.sprintf
            "Could not find any occurrence of '%s' in source code"
            marker)

(** Like [caret_pos] but finds ALL occurrences of the marker in the source. *)
let all_caret_positions (src : string) (marker : string) :
    File_content.Position.t list =
  String.split_lines src
  |> List.filter_mapi ~f:(fun line_num line_src ->
         if String.is_substring line_src ~substring:marker then
           let col_num =
             String.lfindi line_src ~f:(fun _ c ->
                 match c with
                 | '^' -> true
                 | _ -> false)
           in
           Option.map col_num ~f:(fun col ->
               File_content.Position.from_one_based line_num (col + 1))
         else
           None)

let find_ide_range_exn src : Ide_api_types.range =
  let open Option.Let_syntax in
  let start_marker = "/*range-start*/" in
  let end_marker = "/*range-end*/" in
  let hover_at_caret_marker = "^ at-caret" in

  (* Given source code containing the patterns /*range-start*/ and /*range-end*/, calculate the range between the markers *)
  let find_ide_range_from_start_end_comments src : Ide_api_types.range option =
    let lines = String.split_lines src in
    let find_line marker =
      List.findi lines ~f:(fun _ line ->
          String.is_substring line ~substring:marker)
    in
    let find_marker marker after_or_before =
      let+ (line, start_line_src) = find_line marker in
      let column =
        let marker_start =
          Str.search_forward (Str.regexp_string marker) start_line_src 0
        in
        let column_adjustment =
          match after_or_before with
          | `After -> String.length marker
          | `Before -> 0
        in
        marker_start + column_adjustment
      in
      File_content.Position.from_zero_based line column
    in
    let* st = find_marker start_marker `After in
    let+ ed = find_marker end_marker `Before in
    Ide_api_types.{ st; ed }
  in

  let find_ide_range_from_caret_comment src : Ide_api_types.range option =
    let find_ide_pos marker = caret_pos src marker in
    let+ st = find_ide_pos hover_at_caret_marker in
    Ide_api_types.{ st; ed = st }
  in

  match find_ide_range_from_start_end_comments src with
  | Some r -> r
  | None ->
    find_ide_range_from_caret_comment src
    |> Option.value_exn
         ~message:
           (Printf.sprintf
              "could not find pair '%s','%s' nor '%s'"
              start_marker
              end_marker
              hover_at_caret_marker)

(**
 * Compute TASTs for some files, then expand all type variables.
 *)
let compute_tasts_expand_types ctx files_info interesting_files =
  let (errors, tasts) = compute_tasts ctx files_info interesting_files in
  let tasts = Relative_path.Map.map tasts ~f:(Tast_expand.expand_program ctx) in
  (errors, tasts)

let decl_parse_typecheck_and_then ctx files_contents f =
  let (parse_errors, files_info) = parse_name_and_decl ctx files_contents in
  let (errors, _tasts) =
    compute_tasts_expand_types ctx files_info files_contents
  in
  let errors = Diagnostics.merge parse_errors errors in
  if Diagnostics.has_no_errors errors then
    f files_info
  else
    print_errors_if_present errors

let print_nasts ~should_print_position nasts filenames =
  List.iter filenames ~f:(fun filename ->
      match Relative_path.Map.find_opt nasts filename with
      | None ->
        Printf.eprintf
          "Could not find nast for file %s\n"
          (Relative_path.show filename);
        Printf.eprintf "Available nasts:\n";
        Relative_path.Map.iter nasts ~f:(fun path _ ->
            Printf.eprintf "  %s\n" (Relative_path.show path))
      | Some nast ->
        if should_print_position then
          Naming_ast_print.print_nast nast
        else
          Naming_ast_print.print_nast_without_position nast)

let print_tasts ~should_print_position tasts ctx =
  Relative_path.Map.iter tasts ~f:(fun _k (tast : Tast.program) ->
      if should_print_position then
        Typing_ast_print.print_tast ctx tast
      else
        Typing_ast_print.print_tast_without_position ctx tast)

let pp_debug_deps fmt entries =
  Format.fprintf fmt "@[<v>";
  ignore
  @@ List.fold_left entries ~init:false ~f:(fun sep (obj, roots) ->
         if sep then Format.fprintf fmt "@;";
         Format.fprintf fmt "%s -> " obj;
         Format.fprintf fmt "@[<hv>";
         ignore
         @@ List.fold_left roots ~init:false ~f:(fun sep root ->
                if sep then Format.fprintf fmt ",@ ";
                Format.pp_print_string fmt root;
                true);
         Format.fprintf fmt "@]";
         true);
  Format.fprintf fmt "@]"

let show_debug_deps = Format.asprintf "%a" pp_debug_deps

let sort_debug_deps deps =
  Hashtbl.fold deps ~init:[] ~f:(fun ~key:obj ~data:set acc ->
      (obj, set) :: acc)
  |> List.sort ~compare:(fun (a, _) (b, _) -> String.compare a b)
  |> List.map ~f:(fun (obj, roots) ->
         let roots =
           HashSet.fold roots ~init:[] ~f:List.cons
           |> List.sort ~compare:String.compare
         in
         (obj, roots))

(* Note: this prints dependency graph edges in the same direction as the mapping
   which is actually stored in the shared memory table. The line "X -> Y" can be
   read, "X is used by Y", or "X is a dependency of Y", or "when X changes, Y
   must be rechecked". *)
let dump_debug_deps dbg_deps =
  dbg_deps |> sort_debug_deps |> show_debug_deps |> Printf.printf "%s\n"

let handle_constraint_mode
    ~do_
    name
    opts
    ctx
    error_format
    ~iter_over_files
    ~profile_type_check_multi
    ~memtrace =
  (* Process a single typechecked file *)
  let process_file path info =
    match info.File_info.file_mode with
    | Some File_info.Mstrict ->
      let (ctx, entry) = Provider_context.add_entry_if_missing ~ctx ~path in
      let { Tast_provider.Compute_tast.tast; _ } =
        Tast_provider.compute_tast_unquarantined ~ctx ~entry
      in
      let tast = List.concat (Tast_with_dynamic.all tast) in
      do_ opts ctx tast
    | _ ->
      (* We are not interested in partial files and there is nothing in HHI
         files to analyse *)
      ()
  in
  let print_errors errors =
    List.iter
      (Diagnostics.get_sorted_diagnostic_list errors)
      ~f:(print_error ~oc:stdout error_format)
  in
  (* Process a multifile that is not typechecked *)
  let process_multifile filename =
    Printf.printf
      "=== %s analysis results for %s\n%!"
      name
      (Relative_path.to_absolute filename);
    let files_contents = Multifile.file_to_files filename in
    let (parse_errors, file_info) = parse_name_and_decl ctx files_contents in
    let check_errors =
      check_file ctx parse_errors file_info ~profile_type_check_multi ~memtrace
    in
    if not (Diagnostics.has_no_errors_or_warnings check_errors) then
      print_errors check_errors
    else
      Relative_path.Map.iter file_info ~f:process_file
  in
  let process_multifile filename =
    Provider_utils.respect_but_quarantine_unsaved_changes ~ctx ~f:(fun () ->
        process_multifile filename)
  in
  iter_over_files process_multifile

module File_deps = struct
  type dep_kind =
    | Hierarchy  (** Dependencies that come from class hierarchies *)
    | Regular
        (** Dependencies that come from method signatures, expressions, etc. *)

  type 'a deps = {
    hierarchy: 'a;
    regular: 'a * 'a;
  }

  type 'a file_deps = {
    hierarchy: 'a;
    regular: 'a;
  }

  let scrape_class_names (ast : Nast.program) : S_set.t deps =
    (* Visitor that collects class names from AST *)
    let name_collector () =
      let open Aast in
      object
        inherit [_] Aast.iter as super

        val class_names = HashSet.create ()

        val function_names = HashSet.create ()

        (* Note that the unit is not strictly required here, however, this way it's more in line
           with regular OCaml. *)
        method class_names () =
          HashSet.fold class_names ~init:S_set.empty ~f:S_set.add

        method function_names () =
          HashSet.fold function_names ~init:S_set.empty ~f:S_set.add

        method! on_class_name _ (_p, id) = HashSet.add class_names id

        method! on_Call env call =
          let { func = (_, _, expr); _ } = call in
          (match expr with
          | Id (_, name) -> HashSet.add function_names name
          | _ -> ());
          super#on_Call env call
      end
    in
    (* Visitor that invokes [name_collector] on elements of a class hierarchy
       such as extends, implements, uses clauses *)
    let class_hierarchy_visitor name_collector =
      let open Aast in
      object
        inherit [_] Aast.iter

        method! on_Class _ class_ =
          List.iter class_.c_extends ~f:(name_collector#on_hint ());
          List.iter class_.c_uses ~f:(name_collector#on_hint ());
          List.iter class_.c_implements ~f:(name_collector#on_hint ());
          List.iter class_.c_reqs ~f:(name_collector#on_class_req ())
      end
    in
    (* Downstream we need to treat class names relating to class hierarchies
       defined in the file differently from names simply used in method signature
       and expressions.

       Below we do two passes over AST to collect all names and names coming from
       class hierarchies. This could be done more efficiently in one pass but the
       delta in performance is pretty small and overall it is good enough for the
       downstream consumers as-is. *)
    let all_names_collector = name_collector () in
    all_names_collector#on_program () ast;
    let hierarchy_names_collector = name_collector () in
    (class_hierarchy_visitor hierarchy_names_collector)#on_program () ast;
    let hierarchy_class_names = hierarchy_names_collector#class_names () in
    let regular_class_names =
      S_set.diff (all_names_collector#class_names ()) hierarchy_class_names
    in
    let function_names = all_names_collector#function_names () in
    {
      hierarchy = hierarchy_class_names;
      regular = (regular_class_names, function_names);
    }

  (** Collect files that the given [file] depend on based on used class names. *)
  let collect_some_file_dependencies ctx (file : Relative_path.t) :
      Relative_path.Set.t file_deps =
    let open Hh_prelude in
    let nast = Ast_provider.get_ast ctx ~full:true file in
    let names =
      Diagnostics.ignore_ (fun () -> Naming.program ctx nast)
      |> scrape_class_names
    in
    let resolve_to_path names ~resolve =
      S_set.fold
        (fun name files ->
          match resolve ctx name with
          | None -> files
          | Some file -> Relative_path.Set.add files file)
        names
        Relative_path.Set.empty
    in
    let (regular_class_names, function_names) = names.regular in
    let regular_files =
      Relative_path.Set.union
        (resolve_to_path
           ~resolve:Naming_provider.get_class_path
           regular_class_names)
        (resolve_to_path ~resolve:Naming_provider.get_fun_path function_names)
    in
    {
      hierarchy =
        resolve_to_path ~resolve:Naming_provider.get_class_path names.hierarchy;
      regular = regular_files;
    }

  (** Recursively collect names in files and return the files where those names
  are defined. *)
  let traverse_file_dependencies
      ctx (files : Relative_path.t list) ~(depth : int) ~(full_hierarchy : bool)
      : Relative_path.t Seq.t =
    let module Pathtbl = Stdlib.Hashtbl.Make (Relative_path.S) in
    let pending = Queue.create () in
    let visited = Pathtbl.create 17 in
    (* The code below does BFS traversal of file dependencies. When
       [--get-some-file-deps-full-hierarchy] flag is set we collect deps that
       come from class hierarchies regardless of their distance from the input
       file.

       Note that in a chain [input file -> hierarhcy dep -> regular dep] regular
       dep is considered to have distance 2 from the input file. Alternatively,
       we could assume that hierarchy deps are always at distance 0 and then in
       the same chain regular dep would be at distance 1. However, this leads to
       a blow up in the number of deps we collect and is not practical. *)
    let enqueue_file file_depth kind file =
      let should_traverse =
        (match kind with
        | Hierarchy when full_hierarchy -> true
        | _ -> Int.(file_depth < depth))
        && not (Pathtbl.mem visited file)
      in
      if should_traverse then Queue.enqueue pending (file_depth, file);
      Pathtbl.replace visited file ()
    in
    List.iter files ~f:(fun file -> enqueue_file 0 Regular file);
    let iteration = ref 0 in
    while not (Queue.is_empty pending) do
      let (file_depth, file) = Queue.dequeue_exn pending in
      let dependencies = collect_some_file_dependencies ctx file in
      Relative_path.Set.iter
        dependencies.hierarchy
        ~f:(enqueue_file (file_depth + 1) Hierarchy);
      Relative_path.Set.iter
        dependencies.regular
        ~f:(enqueue_file (file_depth + 1) Regular);
      incr iteration;
      if Int.(!iteration mod 100 = 0) then
        Hh_logger.log
          "Iteration %d: visited %d, pending %d"
          !iteration
          (Pathtbl.length visited)
          (Queue.length pending)
    done;
    Pathtbl.to_seq_keys visited
end

(** Used for testing code that generates patches. *)
let codemod
    ~files_info
    ~files_contents
    ctx
    (get_patches :
      files_info:File_info.t Relative_path.Map.t ->
      Server_rename_types.patch list) =
  let decl_parse_typecheck_and_then = decl_parse_typecheck_and_then ctx in
  let backend = Provider_context.get_backend ctx in
  (* Because we repeatedly apply the codemod, positions change. So we need to
     re-parse, decl, and typecheck the file to generated accurate patches as
     well as amend the patched test files in memory. This involves
     invalidating a number of shared heaps and providing in memory
     replacements after patching files. *)
  let invalidate_heaps_and_update_files files_info files_contents =
    let paths_to_purge =
      Relative_path.Map.keys files_contents |> Relative_path.Set.of_list
    in
    let names_to_purge =
      Relative_path.Map.fold
        files_info
        ~init:File_info.empty_names
        ~f:(fun path file_info acc ->
          (* Don't invalidate builtins, otherwise, we can't find them. *)
          if Relative_path.prefix path |> Relative_path.is_hhi then
            acc
          else
            File_info.merge_names
              acc
              (File_info.ids_to_names file_info.File_info.ids))
    in
    begin
      match backend with
      | Provider_backend.Rust_provider_backend rust_backend ->
        Rust_provider_backend.Decl.remove_defs rust_backend names_to_purge
      | Provider_backend.Analysis
      | Provider_backend.Local_memory _
      | Provider_backend.Pessimised_shared_memory _
      | Provider_backend.Shared_memory ->
        ()
    end;
    (* Purge the file, then provide its replacement, otherwise the
       replacement is dropped on the floor. *)
    File_provider.remove_batch paths_to_purge;
    Relative_path.Map.iter
      ~f:File_provider.provide_file_for_tests
      files_contents;
    Ast_provider.remove_batch paths_to_purge;
    Relative_path.Map.iter
      ~f:(fun path file_info ->
        (* Don't invalidate builtins, otherwise, we can't find them. *)
        if not (Relative_path.prefix path |> Relative_path.is_hhi) then
          Naming_global.remove_decls_using_file_info
            backend
            file_info.File_info.ids)
      files_info
  in
  let final_files_info = ref files_info in
  let final_files_contents = ref files_contents in
  let rec go files_info files_contents =
    invalidate_heaps_and_update_files files_info files_contents;
    decl_parse_typecheck_and_then files_contents @@ fun files_info ->
    let patches = get_patches ~files_info in
    let files_contents =
      Server_rename_types.apply_patches_to_file_contents files_contents patches
    in
    if List.is_empty patches then begin
      final_files_info := files_info;
      final_files_contents := files_contents;
      Multifile.print_files_as_multifile files_contents
    end else
      go files_info files_contents
  in
  go files_info files_contents;

  (* Typecheck after the codemod is fully applied to confirm that what we
     produce is not garbage. *)
  Printf.printf
    "\nTypechecking after the codemod... (no output after this is good news)\n";
  invalidate_heaps_and_update_files !final_files_info !final_files_contents;
  decl_parse_typecheck_and_then !final_files_contents ignore

(** Enables testing of eager quickfixes (Quickfix.Eager).
  Prefer using `--ide-code-actions` for more realistic tests that don't care about eagerness *)
module Eager_quickfix_test_util : sig
  (** corresponds to a Quickfix.Eager *)
  type t

  (** convert `Quickfix.Eager` to a `Some t`, otherwise `None *)
  val of_quickfix : Pos.t Quickfix.t -> t option

  (** For testing, with `--fix`, apply all eager quickfixes by replacing/inserting the new text in [src].
      These tests aren't very realistic, better to use the tests driven by --ide-code-actions
      this is useful for testing quickfixes. **)
  val apply_all : src:string -> t list -> string
end = struct
  type edit = string * Pos.t

  type t = {
    _title: string;
    edits: edit list;
  }

  let of_quickfix (qf : Pos.t Quickfix.t) : t option =
    match Quickfix.get_edits qf with
    | Quickfix.Eager eager_edits ->
      Some { _title = Quickfix.get_title qf; edits = eager_edits }
    | _ -> None

  (* Sort [quickfixes] with their edit positions in descending
     order. This allows us to iteratively apply the quickfixes without
     messing up positions earlier in the file.*)
  let sort_for_application (quickfixes : t list) : t list =
    let first_qf_offset (quickfix : t) : int =
      let pos =
        match List.hd quickfix.edits with
        | Some (_, pos) -> pos
        | _ -> Pos.none
      in
      snd (Pos.info_raw pos)
    in
    let compare x y = Int.compare (first_qf_offset x) (first_qf_offset y) in
    List.rev (List.sort ~compare quickfixes)

  let sort_edits_for_application (edits : edit list) : edit list =
    let offset (_, pos) = snd (Pos.info_raw pos) in
    let compare x y = Int.compare (offset x) (offset y) in
    List.rev (List.sort ~compare edits)

  (* Apply [edit] to [src], replacing the text at the position specified. *)
  let apply_edit (src : string) (edit : edit) : string =
    let (new_text, pos) = edit in
    if Pos.equal pos Pos.none then
      failwith
        (Printf.sprintf
           "tried to apply quickfix edit with invalid pos 'Pos.none' and text '%s'"
           new_text)
    else
      let (start_offset, end_offset) = Pos.info_raw pos in
      let src_before = String.subo src ~len:start_offset in
      let src_after = String.subo src ~pos:end_offset in
      src_before ^ new_text ^ src_after

  let apply_quickfix (src : string) (eager_quickfix : t) : string =
    List.fold
      (sort_edits_for_application eager_quickfix.edits)
      ~init:src
      ~f:apply_edit

  let apply_all ~(src : string) (eager_quickfixes : t list) : string =
    List.fold
      (sort_for_application eager_quickfixes)
      ~init:src
      ~f:apply_quickfix
end

let do_hover ~ctx ~filename oc (pos_given : File_content.Position.t option) =
  let (ctx, entry) =
    Provider_context.add_entry_if_missing ~ctx ~path:filename
  in
  let pos =
    match pos_given with
    | Some pos -> pos
    | None ->
      let src = Provider_context.read_file_contents_exn entry in
      caret_pos_exn src "^ hover-at-caret"
  in
  let results = Ide_hover.go_quarantined ~ctx ~entry pos in
  let formatted_results =
    Hover_service.as_marked_string_list results
    |> List.map ~f:(fun (r : Lsp.markedString) ->
           match r with
           | Lsp.MarkedCode (_, s) -> s
           | Lsp.MarkedString s -> s)
    |> String.concat ~sep:"\n"
  in
  Printf.fprintf oc "%s\n" formatted_results

let do_enforcement
    ~ctx ~filename oc (pos_given : File_content.Position.t option) =
  let (ctx, entry) =
    Provider_context.add_entry_if_missing ~ctx ~path:filename
  in
  let pos =
    match pos_given with
    | Some pos -> pos
    | None ->
      let src = Provider_context.read_file_contents_exn entry in
      caret_pos_exn src "^ enforcement-at-caret"
  in
  let open Enforcement_at_pos_service in
  let result = Server_enforcement_at_pos.go_ctx ~ctx ~entry pos in
  match result with
  | Some (Enforced types) ->
    let ty_strs = List.map types ~f:(fun t -> t.ty_str) in
    Printf.fprintf oc "%s: Enforced\n" (String.concat ~sep:", " ty_strs)
  | Some Unenforced -> Printf.fprintf oc "Unenforced\n"
  | None -> Printf.fprintf oc "(unknown)\n"

let do_enforcement_batch ~ctx ~filename oc =
  let (ctx, entry) =
    Provider_context.add_entry_if_missing ~ctx ~path:filename
  in
  let src = Provider_context.read_file_contents_exn entry in
  let open Enforcement_at_pos_service in
  let positions = all_caret_positions src "^ enforcement-at-caret" in
  List.iter positions ~f:(fun pos ->
      let result = Server_enforcement_at_pos.go_ctx ~ctx ~entry pos in
      match result with
      | Some (Enforced types) ->
        let ty_strs = List.map types ~f:(fun t -> t.ty_str) in
        Printf.fprintf oc "%s: Enforced\n" (String.concat ~sep:", " ty_strs)
      | Some Unenforced -> Printf.fprintf oc "Unenforced\n"
      | None -> Printf.fprintf oc "(unknown)\n")

let handle_mode
    mode
    filenames
    ctx
    builtins
    files_contents
    files_info
    (parse_errors : Diagnostics.t)
    max_errors
    error_format
    batch_mode
    out_extension
    dbg_deps
    ~should_print_position
    ~profile_type_check_multi
    ~memtrace =
  let expect_single_file () : Relative_path.t =
    match filenames with
    | [x] -> x
    | _ -> die "Only single file expected"
  in
  let iter_over_files f : unit = List.iter filenames ~f in
  match mode with
  | Refactor_sound_dynamic (analysis_mode, refactor_mode, element_name) ->
    let opts =
      match
        ( Refactor_sd_options.parse_analysis_mode analysis_mode,
          Refactor_sd_options.parse_refactor_mode refactor_mode )
      with
      | (Some analysis_mode, Some refactor_mode) ->
        Refactor_sd_options.mk ~analysis_mode ~refactor_mode
      | (None, _) -> die "invalid refactor_sd analysis mode"
      | (_, None) -> die "invalid refactor_sd refactor mode"
    in
    handle_constraint_mode
      ~do_:(Refactor_sd.do_ element_name)
      "Sound Dynamic"
      opts
      ctx
      error_format
      ~iter_over_files
      ~profile_type_check_multi
      ~memtrace
  | Cst_search ->
    let path = expect_single_file () in
    let (ctx, entry) = Provider_context.add_entry_if_missing ~ctx ~path in
    let result =
      let open Result.Monad_infix in
      Sys_utils.read_stdin_to_string ()
      |> Yojson.Safe.from_string
      |> Cst_search_service.compile_pattern ctx
      >>| Cst_search_service.search ctx entry
      >>| Cst_search_service.result_to_json ~sort_results:true
      >>| Yojson.Safe.pretty_to_string
    in
    begin
      match result with
      | Ok result -> Printf.printf "%s\n" result
      | Error message ->
        Printf.printf "%s\n" message;
        exit 1
    end
  | Dump_symbol_info ->
    iter_over_files (fun filename ->
        match Relative_path.Map.find_opt files_info filename with
        | Some _fileinfo ->
          let raw_result = Symbol_info_service_utils.helper ctx [] [filename] in
          let result = Symbol_info_service_utils.format_result raw_result in
          let result_json =
            Server_command_types.Symbol_info_service.to_json result
          in
          print_endline (Yojson.Safe.pretty_to_string result_json)
        | None -> ())
  | Glean_index out_dir ->
    if
      (not (Disk.is_directory out_dir))
      || Array.length (Sys.readdir out_dir) > 0
    then (
      Printf.printf "%s should be an empty dir\n" out_dir;
      exit 1
    ) else
      Write_symbol_info.Entrypoint.index_files ctx ~out_dir ~files:filenames
  | Glean_sym_hash ->
    List.iter
      (Write_symbol_info.Entrypoint.sym_hashes ctx ~files:filenames)
      ~f:(fun (path, hash) -> Printf.printf "%s %s\n" path (Md5.to_hex hash))
  | Lint ->
    let lint_errors =
      Relative_path.Map.fold
        files_contents
        ~init:[]
        ~f:(fun fn content lint_errors ->
          lint_errors
          @ fst (Lints_core.do_ (fun () -> Linting_main.lint ctx fn content)))
    in
    if not (List.is_empty lint_errors) then (
      let lint_errors =
        List.sort
          ~compare:
            begin
              fun x y ->
                Pos.compare (Lints_core.get_pos x) (Lints_core.get_pos y)
            end
          lint_errors
      in
      (match error_format with
      | Diagnostics.PlainHighlighted ->
        List.iter lint_errors ~f:(fun lint ->
            let source =
              Relative_path.Map.find_opt
                files_contents
                (Pos.filename (Lints_core.get_pos lint))
            in
            Lints_core.to_plain_highlighted_string
              ?source
              (Lints_core.to_absolute lint)
            |> Printf.fprintf stdout "%s\n%!")
      | format ->
        Server_lint_types.output_text
          stdout
          (List.map ~f:Lints_core.to_absolute lint_errors)
          format);
      exit 2
    ) else
      Printf.printf "No lint errors\n"
  | Lint_json ->
    let json_errors =
      Relative_path.Map.fold
        files_contents
        ~init:[]
        ~f:(fun fn content json_errors ->
          json_errors
          @ fst (Lints_core.do_ (fun () -> Linting_main.lint ctx fn content)))
    in
    let json_errors =
      List.sort
        ~compare:
          begin
            fun x y ->
              Pos.compare (Lints_core.get_pos x) (Lints_core.get_pos y)
          end
        json_errors
    in
    let json_errors = List.map ~f:Lints_core.to_absolute json_errors in
    Server_lint_types.output_json
      ~from_test:true
      ~pretty:true
      stdout
      json_errors;
    exit 2
  | Dump_deps ->
    Relative_path.Map.iter files_info ~f:(fun fn _fileinfo ->
        let full_ast = Ast_provider.get_ast ctx fn ~full:true in
        ignore @@ Typing_check_job.calc_errors_and_tast ctx fn ~full_ast);
    if Hashtbl.length dbg_deps > 0 then dump_debug_deps dbg_deps
  | Dump_dep_hashes ->
    iter_over_files (fun _ ->
        let nasts = create_nasts ctx files_info in
        Relative_path.Map.iter nasts ~f:(fun _ nast ->
            Dep_hash_to_symbol.dump nast))
  | Get_some_file_deps { depth; full_hierarchy } ->
    let file_deps =
      File_deps.traverse_file_dependencies ctx filenames ~depth ~full_hierarchy
    in
    file_deps
    |> Seq.iter (fun file ->
           Printf.printf "%s\n" (Relative_path.to_absolute file))
  | Dump_inheritance ->
    let open Server_command_types.Method_jumps in
    let naming_table = Naming_table.create files_info in
    Naming_table.iter naming_table ~f:(fun fn fileinfo ->
        if Relative_path.Map.mem builtins fn then
          ()
        else (
          List.iter fileinfo.File_info.ids.File_info.classes ~f:(fun id ->
              Printf.printf
                "Ancestors of %s and their overridden methods:\n"
                id.File_info.name;
              let ancestors =
                (* Might raise {!Naming_table.File_info_not_found} *)
                Method_jumps.get_inheritance
                  ctx
                  id.File_info.name
                  ~filter:No_filter
                  ~find_children:false
                  naming_table
                  None
              in
              Server_command_types.Method_jumps.print_readable
                ancestors
                ~find_children:false;
              Printf.printf "\n");
          Printf.printf "\n";
          List.iter fileinfo.File_info.ids.File_info.classes ~f:(fun id ->
              Printf.printf
                "Children of %s and the methods they override:\n"
                id.File_info.name;
              let children =
                (* Might raise {!Naming_table.File_info_not_found} *)
                Method_jumps.get_inheritance
                  ctx
                  id.File_info.name
                  ~filter:No_filter
                  ~find_children:true
                  naming_table
                  None
              in
              Server_command_types.Method_jumps.print_readable
                children
                ~find_children:true;
              Printf.printf "\n")
        ))
  | Identify_symbol pos ->
    let path = expect_single_file () in
    let (ctx, entry) = Provider_context.add_entry_if_missing ~ctx ~path in
    (* TODO(ljw): surely this doesn't need quarantine? *)
    let result =
      Provider_utils.respect_but_quarantine_unsaved_changes ~ctx ~f:(fun () ->
          Server_identify_function.go_quarantined_absolute ~ctx ~entry pos)
    in
    begin
      match result with
      | [] -> print_endline "None"
      | result -> Client_get_definition.print_readable ~short_pos:true result
    end
  | Ide_code_actions { title_prefix; use_snippet_edits } ->
    let path = expect_single_file () in
    let (ctx, entry) = Provider_context.add_entry_if_missing ~ctx ~path in
    let src = Provider_context.read_file_contents_exn entry in
    let range = find_ide_range_exn src in
    Code_actions_cli_lib.run
      ctx
      ~error_filter:Tast_provider.ErrorFilter.default
      entry
      range
      ~title_prefix
      ~use_snippet_edits
  | Ide_diagnostics ->
    let path = expect_single_file () in
    let (ctx, entry) = Provider_context.add_entry_if_missing ~ctx ~path in
    let (errors, _tasts) =
      compute_tasts_expand_types ctx files_info files_contents
    in
    Ide_diagnostics_cli_lib.run ctx entry errors
  | Ide_signature_help ->
    let path = expect_single_file () in
    let (ctx, entry) = Provider_context.add_entry_if_missing ~ctx ~path in
    let src = Provider_context.read_file_contents_exn entry in
    let pos = caret_pos_exn src "^ at-caret" in
    let result = Server_signature_help.go_quarantined ~ctx ~entry pos in
    print_endline "textDocument/signatureHelp response:\n";
    result
    |> Lsp_fmt.print_signatureHelp
    |> Yojson.Safe.pretty_to_string
    |> print_endline
  | Dump_classish_positions ->
    let path = expect_single_file () in
    let (ctx, entry) = Provider_context.add_entry_if_missing ~ctx ~path in
    Classish_positions_cli_lib.dump ctx entry path
  | Find_local pos ->
    let filename = expect_single_file () in
    let (ctx, entry) =
      Provider_context.add_entry_if_missing ~ctx ~path:filename
    in
    let result = Server_find_locals.go ~ctx ~entry pos in
    let print pos = Printf.printf "%s\n" (Pos.string_no_file pos) in
    List.iter result ~f:print
  | Outline ->
    iter_over_files (fun filename ->
        let file = cat (Relative_path.to_absolute filename) in
        let results =
          File_outline.outline (Provider_context.get_popt ctx) file
        in
        File_outline.print ~short_pos:true results)
  | Outline_for_agents ->
    iter_over_files (fun filename ->
        let file = cat (Relative_path.to_absolute filename) in
        let result = Outline_for_agents.outline file in
        print_string result)
  | Dump_nast ->
    let (errors, nasts) =
      Diagnostics.do_ (fun () -> create_nasts ctx files_info)
    in
    print_errors_if_present errors;

    print_nasts
      ~should_print_position
      nasts
      (Relative_path.Map.keys files_contents)
  | Dump_tast ->
    let (errors, tasts) =
      compute_tasts_expand_types ctx files_info files_contents
    in
    print_errors_if_present (Diagnostics.merge parse_errors errors);
    print_tasts ~should_print_position tasts ctx
  | Dump_dynamic_inference { as_data } ->
    let ctx =
      Provider_context.map_tcopt ctx ~f:(fun tcopt ->
          Global_options.{ tcopt with tco_dynamic_inference = true })
    in
    let (_errors, tasts) = compute_tasts ctx files_info files_contents in
    Relative_path.Map.iter tasts ~f:(fun _fn tast ->
        let Equal = Tast_env.eq_typing_env in
        let print_json json =
          Printf.printf "%s\n" (Yojson.Safe.pretty_to_string json)
        in
        let solve_def ~name env =
          let inf_env = env.Typing_env_types.inference_env in
          let dynamic_locals =
            match env.Typing_env_types.fun_tast_info with
            | Some info -> info.Tast.dynamic_locals
            | None -> []
          in
          let typing_env : Typing_env_types.env = env in
          let solutions =
            Typing_shadow_solver.solve
              ~as_data
              typing_env
              inf_env
              dynamic_locals
          in
          print_json
            (`Assoc
              [
                ("function", `String name);
                ( "solutions",
                  `List
                    (List.map
                       solutions
                       ~f:(Typing_shadow_solver.solution_to_json env)) );
              ])
        in
        List.iter tast ~f:(fun def ->
            match def with
            | Aast.Fun { Aast.fd_name; _ } ->
              let env = Tast_env.def_env ctx def in
              solve_def ~name:(snd fd_name) env
            | Aast.Class { Aast.c_name; c_methods; _ } ->
              let class_env = Tast_env.def_env ctx def in
              let class_name = snd c_name in
              List.iter c_methods ~f:(fun m ->
                  let env = Tast_env.restore_method_env class_env m in
                  let name = class_name ^ "::" ^ snd m.Aast.m_name in
                  solve_def ~name env)
            | _ -> ()))
  | Dump_stripped_tast ->
    iter_over_files (fun filename ->
        let files_contents =
          Relative_path.Map.filter files_contents ~f:(fun k _v ->
              Relative_path.equal k filename)
        in
        let (_, tasts) = compute_tasts ctx files_info files_contents in
        let tast = Relative_path.Map.find tasts filename in
        let nast = Tast.to_nast tast in
        Printf.printf "%s\n" (Nast.show_program nast))
  | RemoveDeadUnsafeCasts ->
    let ctx =
      Provider_context.map_tcopt ctx ~f:(fun tcopt ->
          Global_options.
            { tcopt with tco_populate_dead_unsafe_cast_heap = true })
    in
    let get_patches ~files_info =
      Remove_dead_unsafe_casts.get_patches
        ~is_test:true
        ~files_info
        ~fold:Relative_path.Map.fold
    in
    codemod ~files_info ~files_contents ctx get_patches
  | Find_refs pos ->
    let path = expect_single_file () in
    let naming_table = Naming_table.create files_info in
    let genv = Server_env_build.default_genv in
    let init_id = Random_id.short_string () in
    let env =
      {
        (Server_env_build.make_env
           ~init_id
           ~deps_mode:(Typing_deps_mode.InMemoryMode None)
           genv.config)
        with
        Server_env.naming_table;
        Server_env.tcopt = Provider_context.get_tcopt ctx;
      }
    in
    let include_defs = true in
    let (ctx, entry) =
      Provider_context.add_entry_if_missing
        ~ctx:(Provider_utils.ctx_from_server_env env)
        ~path
    in
    let open Server_command_types.Done_or_retry in
    Provider_utils.respect_but_quarantine_unsaved_changes ~ctx ~f:(fun () ->
        match Server_find_refs.go_from_file_ctx ~ctx ~entry pos with
        | None -> ()
        | Some (name, action) ->
          let results =
            match
              Server_find_refs.go
                ctx
                action
                include_defs
                ~stream_file:None
                ~hints:[]
                genv
                env
            with
            | (_env, Done r) -> Server_find_refs.to_absolute r
            | (_env, Retry) -> failwith "didn't expect retry"
          in
          Printf.printf "%s\n" name;
          Find_refs_wire_format.CliHumanReadable.print_results
            (List.rev results))
  | Go_to_impl pos ->
    let filename = expect_single_file () in
    let naming_table = Naming_table.create files_info in
    let genv = Server_env_build.default_genv in
    let init_id = Random_id.short_string () in
    let env =
      {
        (Server_env_build.make_env
           ~init_id
           ~deps_mode:(Typing_deps_mode.InMemoryMode None)
           genv.Server_env.config)
        with
        Server_env.naming_table;
        tcopt = Provider_context.get_tcopt ctx;
      }
    in
    let filename = Relative_path.to_absolute filename in
    let contents = cat filename in
    let (ctx, entry) =
      Provider_context.add_or_overwrite_entry_contents
        ~ctx:(Provider_utils.ctx_from_server_env env)
        ~path:(Relative_path.create_detect_prefix filename)
        ~contents
    in
    let open Server_command_types.Done_or_retry in
    begin
      match Server_find_refs.go_from_file_ctx ~ctx ~entry pos with
      | None -> ()
      | Some (name, action) ->
        let results =
          match Server_go_to_impl.go ~action ~genv ~env with
          | (_env, Done r) -> Server_find_refs.to_absolute r
          | (_env, Retry) -> failwith "didn't expect retry"
        in
        Printf.printf "%s\n" name;
        Find_refs_wire_format.CliHumanReadable.print_results (List.rev results)
    end
  | Highlight_refs pos ->
    let path = expect_single_file () in
    let (ctx, entry) = Provider_context.add_entry_if_missing ~ctx ~path in
    let results = Ide_highlight_refs.go_quarantined ~ctx ~entry pos in
    Client_highlight_refs.go results ~output_json:false
  | Errors when batch_mode ->
    (* For each file in our batch, run typechecking serially.
       Reset the heaps every time in between. *)
    iter_over_files (fun filename ->
        let out_path = Relative_path.to_absolute filename ^ out_extension in
        (* Write to a process-unique temp file and atomically rename it into
           place. Several hh_single_type_check processes can run concurrently
           against the same test directory - this is what happens under stress
           runs - and a non-atomic writer would let another process observe a
           half-written (i.e. empty) output file. *)
        let tmp_path = Printf.sprintf "%s.%d.tmp" out_path (Unix.getpid ()) in
        let oc = Out_channel.create tmp_path in
        (* This means builtins had errors, so lets just print those if we see them *)
        if not (Diagnostics.has_no_errors_or_warnings parse_errors) then
          (* This closes the out channel *)
          write_error_list error_format parse_errors oc max_errors
        else (
          Typing_log.out_channel := oc;
          Provider_utils.respect_but_quarantine_unsaved_changes
            ~ctx
            ~f:(fun () ->
              let files_contents = Multifile.file_to_files filename in
              Relative_path.Map.iter files_contents ~f:(fun filename contents ->
                  File_provider.(provide_file_for_tests filename contents));
              let (parse_errors, individual_file_info) =
                parse_name_and_decl ctx files_contents
              in
              let errors =
                check_file
                  ctx
                  parse_errors
                  individual_file_info
                  ~profile_type_check_multi
                  ~memtrace
              in
              write_error_list error_format errors oc max_errors)
        );
        Sys.rename tmp_path out_path)
  | Errors ->
    (* Don't typecheck builtins *)
    let errors =
      check_file ctx parse_errors files_info ~profile_type_check_multi ~memtrace
    in
    print_error_list error_format errors max_errors;
    if Diagnostics.has_errors errors then exit 2
  | Get_member class_and_member_id ->
    let (cid, mid) =
      match Str.split (Str.regexp "::") class_and_member_id with
      | [cid; mid] -> (cid, mid)
      | _ ->
        failwith
          (Printf.sprintf "Invalid --get-member ID: %S" class_and_member_id)
    in
    let cid = Utils.add_ns cid in
    (match Decl_provider.get_class ctx cid with
    | Decl_entry.DoesNotExist
    | Decl_entry.NotYetAvailable ->
      Printf.printf "No class named %s\n" cid
    | Decl_entry.Found cls ->
      let ty_to_string ty =
        let env =
          Typing_env_types.empty ctx Relative_path.default ~droot:None
        in
        Typing_print.full_strip_ns_decl ~verbose_fun:false env ty
      in
      let print_class_element member_type get mid =
        match get cls mid with
        | None -> ()
        | Some ce ->
          let abstract =
            if Typing_defs.get_ce_abstract ce then
              "abstract "
            else
              ""
          in
          let origin = ce.ce_origin in
          let from =
            if String.equal origin cid then
              ""
            else
              Printf.sprintf " from %s" (Utils.strip_ns origin)
          in
          Printf.printf
            "  %s%s%s: %s\n"
            abstract
            member_type
            from
            (ty_to_string (Lazy.force ce.ce_type))
      in
      Printf.printf "%s::%s\n" cid mid;
      print_class_element "method" Cls.get_method mid;
      print_class_element "static method" Cls.get_smethod mid;
      print_class_element "property" Cls.get_prop mid;
      print_class_element "static property" Cls.get_sprop mid;
      print_class_element "static property" Cls.get_sprop ("$" ^ mid);
      (match Cls.get_const cls mid with
      | None -> ()
      | Some cc ->
        let abstract =
          Typing_defs.(
            match cc.cc_abstract with
            | CCAbstract _ -> "abstract "
            | CCConcrete -> "")
        in
        let origin = cc.cc_origin in
        let from =
          if String.equal origin cid then
            ""
          else
            Printf.sprintf " from %s" (Utils.strip_ns origin)
        in
        let ty = ty_to_string cc.cc_type in
        Printf.printf "  %sconst%s: %s\n" abstract from ty);
      (match Cls.get_typeconst cls mid with
      | None -> ()
      | Some ttc ->
        let origin = ttc.ttc_origin in
        let from =
          if String.equal origin cid then
            ""
          else
            Printf.sprintf " from %s" (Utils.strip_ns origin)
        in
        let ty =
          match ttc.ttc_kind with
          | TCConcrete { tc_type = ty } -> "= " ^ ty_to_string ty
          | TCAbstract
              {
                atc_as_constraint = as_cstr;
                atc_super_constraint = _;
                atc_default = default;
              } ->
            String.concat
              ~sep:" "
              (List.filter_map
                 [
                   Option.map as_cstr ~f:(fun ty -> "as " ^ ty_to_string ty);
                   Option.map default ~f:(fun ty -> "= " ^ ty_to_string ty);
                 ]
                 ~f:(fun x -> x))
        in
        let abstract =
          match ttc.ttc_kind with
          | TCConcrete _ -> ""
          | TCAbstract _ -> "abstract "
        in
        Printf.printf "  %stypeconst%s: %s %s\n" abstract from mid ty);
      ())
  | Hover None when batch_mode ->
    iter_over_files (fun filename ->
        let oc =
          Out_channel.create (Relative_path.to_absolute filename ^ out_extension)
        in
        Typing_log.out_channel := oc;
        Provider_utils.respect_but_quarantine_unsaved_changes ~ctx ~f:(fun () ->
            do_hover ~ctx ~filename oc None))
  | Hover pos_given ->
    let filename = expect_single_file () in
    do_hover ~ctx ~filename stdout pos_given
  | Enforcement None when batch_mode ->
    iter_over_files (fun filename ->
        let oc =
          Out_channel.create (Relative_path.to_absolute filename ^ out_extension)
        in
        Typing_log.out_channel := oc;
        Provider_utils.respect_but_quarantine_unsaved_changes ~ctx ~f:(fun () ->
            do_enforcement ~ctx ~filename oc None))
  | Enforcement pos_given ->
    let filename = expect_single_file () in
    do_enforcement ~ctx ~filename stdout pos_given
  | Enforcement_batch when batch_mode ->
    iter_over_files (fun filename ->
        let oc =
          Out_channel.create (Relative_path.to_absolute filename ^ out_extension)
        in
        Typing_log.out_channel := oc;
        Provider_utils.respect_but_quarantine_unsaved_changes ~ctx ~f:(fun () ->
            do_enforcement_batch ~ctx ~filename oc))
  | Enforcement_batch ->
    let filename = expect_single_file () in
    do_enforcement_batch ~ctx ~filename stdout
  | Apply_quickfixes ->
    let path = expect_single_file () in
    let (ctx, _entry) = Provider_context.add_entry_if_missing ~ctx ~path in
    let (errors, _) =
      compute_tasts ~drop_fixmed:false ctx files_info files_contents
    in
    let src = Relative_path.Map.find files_contents path in

    let quickfixes =
      Diagnostics.get_diagnostic_list ~drop_fixmed:false errors
      |> List.map ~f:(fun e ->
             (* If an error has multiple possible quickfixes, take the first. *)
             List.hd (User_diagnostic.quickfixes e))
      |> List.filter_opt
    in

    (* Print the title of each quickfix, so we can see text changes in tests. *)
    List.iter quickfixes ~f:(fun qf ->
        Printf.printf "%s\n" (Quickfix.get_title qf));

    let (eagers, rejected_titles) =
      List.partition_map quickfixes ~f:(fun qf ->
          match Eager_quickfix_test_util.of_quickfix qf with
          | Some eager_qf -> Either.First eager_qf
          | None -> Either.Second (Quickfix.get_title qf))
    in
    begin
      if not (List.is_empty rejected_titles) then
        Printf.printf
          "The following quickfixes could not be applied with --fix.\n%s\n%s"
          (String.concat ~sep:"\n" rejected_titles)
          " Use hh_single_type_check --ide-code-actions to handle non-eager quickfixes"
    end;

    (* Print the source code after applying all these quickfixes. *)
    Printf.printf "\n%s" (Eager_quickfix_test_util.apply_all ~src eagers)
  | CountImpreciseTypes ->
    let (errors, tasts) =
      compute_tasts_expand_types ctx files_info files_contents
    in
    if Diagnostics.has_errors errors then (
      print_errors error_format errors max_errors;
      Printf.printf
        "Did not count imprecise types because there are typing errors.";
      exit 2
    ) else
      let tasts = Relative_path.Map.values tasts in
      let results =
        List.map ~f:(Count_imprecise_types.count ctx) tasts
        |> List.fold
             ~f:(S_map.union ~combine:(fun id _ -> failwith ("Clash at " ^ id)))
             ~init:S_map.empty
      in
      let json = Count_imprecise_types.json_of_results results in
      Printf.printf "%s" (Yojson.Safe.to_string json)
  | RunSimpliHack ->
    let (errors, tasts) = compute_tasts ctx files_info files_contents in
    print_error_list error_format errors max_errors;
    Relative_path.Map.iter tasts ~f:(fun fn tast ->
        let prompts = Simplihack_prompt.find ctx tast in
        Printf.printf "%s\n" @@ Relative_path.to_absolute fn;
        List.iter prompts ~f:(fun prompt ->
            let msg =
              match prompt.Simplihack_prompt.derive_prompt () with
              | Some p -> p
              | None -> "<no prompt derived>"
            in
            let result =
              Format.asprintf
                "%a %s"
                Pos.pp
                prompt.Simplihack_prompt.param_pos
                msg
            in
            Printf.printf "  %s\n" result))
  | Map_reduce_mode ->
    let (errors, tasts) = compute_tasts_by_name ctx files_info files_contents in
    print_errors_if_present (Diagnostics.merge parse_errors errors);
    let mapped =
      Relative_path.Map.elements tasts
      |> List.map ~f:(fun (fn, tasts) -> Map_reduce.map ctx fn tasts errors)
    in
    let reduced =
      List.fold mapped ~init:Map_reduce.empty ~f:Map_reduce.reduce
    in
    let json = Map_reduce_ffi.yojson_of_t (Map_reduce.to_ffi reduced) in
    Yojson.Safe.pretty_to_channel Stdlib.stdout json

(*****************************************************************************)
(* Main entry point *)
(*****************************************************************************)

let decl_and_run_mode
    {
      files;
      extra_builtins;
      mode;
      error_format;
      no_builtins;
      tcopt;
      max_errors;
      batch_mode;
      out_extension;
      should_print_position;
      custom_hhi_path;
      profile_type_check_multi;
      memtrace;
      rust_provider_backend;
      naming_table_path;
      packages_config_path;
      enable_implicit_packages;
    }
    (hhi_root : Path.t) : unit =
  Ident.track_names := true;
  let builtins =
    if no_builtins then
      Relative_path.Map.empty
    else
      let extra_builtins =
        let add_file_content map filename =
          Relative_path.create Relative_path.Dummy filename
          |> Multifile.file_to_file_list
          |> List.map ~f:(fun (path, contents) ->
                 (Filename.basename (Relative_path.suffix path), contents))
          |> List.unordered_append map
        in
        extra_builtins
        |> List.fold ~f:add_file_content ~init:[]
        |> Array.of_list
      in
      let magic_builtins = Array.append magic_builtins extra_builtins in
      let hhi_builtins =
        match custom_hhi_path with
        | None -> hhi_builtins
        | Some path -> Array.of_list (Hhi_get.get_hhis_in_dir path)
      in
      (* Check that magic_builtin filenames are unique *)
      let () =
        let n_of_builtins = Array.length magic_builtins in
        let n_of_unique_builtins =
          Array.to_list magic_builtins
          |> List.map ~f:fst
          |> S_set.of_list
          |> S_set.cardinal
        in
        if n_of_builtins <> n_of_unique_builtins then
          die "Multiple magic builtins share the same base name.\n"
      in
      Array.iter magic_builtins ~f:(fun (file_name, file_contents) ->
          let file_path = Path.concat hhi_root file_name in
          let file = Path.to_string file_path in
          Sys_utils.try_touch
            (Sys_utils.Touch_existing { follow_symlinks = true })
            file;
          Sys_utils.write_file ~file file_contents);

      (* Take the builtins (file, contents) array and create relative paths *)
      Array.fold
        (Array.append magic_builtins hhi_builtins)
        ~init:Relative_path.Map.empty
        ~f:(fun acc (f, src) ->
          let f = Path.concat hhi_root f |> Path.to_string in
          Relative_path.Map.add
            acc
            ~key:(Relative_path.create Relative_path.Hhi f)
            ~data:src)
  in
  let files =
    if use_canonical_filenames () then
      let canonicalize_path path =
        match Sys_utils.realpath path with
        | Some path -> Relative_path.create_detect_prefix path
        | None -> failwith ("Couldn't resolve realpath of " ^ path)
      in
      files |> List.map ~f:canonicalize_path
    else
      files |> List.map ~f:(Relative_path.create Relative_path.Dummy)
  in
  let files_contents =
    List.fold
      files
      ~f:(fun acc filename ->
        let files_contents = Multifile.file_to_files filename in
        Relative_path.Map.union acc files_contents)
      ~init:Relative_path.Map.empty
  in
  (* Merge in builtins *)
  let files_contents_with_builtins =
    Relative_path.Map.fold
      builtins
      ~f:
        begin
          (fun k src acc -> Relative_path.Map.add acc ~key:k ~data:src)
        end
      ~init:files_contents
  in
  (* Don't declare all the filenames in batch_errors mode *)
  let to_decl =
    if batch_mode then
      builtins
    else
      files_contents_with_builtins
  in
  let dbg_deps = Hashtbl.Poly.create () in
  (match mode with
  | Dump_deps ->
    (* In addition to actually recording the dependencies in shared memory,
       we build a non-hashed respresentation of the dependency graph
       for printing. *)
    let get_debug_trace root obj =
      let root = Typing_deps.Dep.variant_to_string root in
      let obj = Typing_deps.Dep.variant_to_string obj in
      match Hashtbl.find dbg_deps obj with
      | Some set -> HashSet.add set root
      | None ->
        let set = HashSet.create () in
        HashSet.add set root;
        Hashtbl.set dbg_deps ~key:obj ~data:set
    in
    Typing_deps.add_dependency_callback ~name:"get_debug_trace" get_debug_trace
  | _ -> ());
  let (package_config_errors, package_info) =
    Diagnostics.do_ @@ fun () ->
    match packages_config_path with
    | None -> Package_info.empty
    | Some pkgs_config_relpath ->
      Package_config.load_and_parse
        ~strict:false
        ~enable_implicit_packages
        ~pkgs_config_abs_path:
          Relative_path.(to_absolute @@ from_root ~suffix:pkgs_config_relpath)
  in
  let tcopt = Typechecker_options.set_package_info tcopt package_info in
  let popt = tcopt.Global_options.po in
  let ctx =
    if rust_provider_backend then
      let backend =
        Hh_server_provider_backend.make
          (Decl_fold_options.from_global_options tcopt)
          (Decl_parser_options.from_parser_options popt)
      in
      let () = Provider_backend.set_rust_backend backend in
      Provider_context.empty_for_tool
        ~popt
        ~tcopt
        ~backend:(Provider_backend.get ())
        ~deps_mode:(Typing_deps_mode.InMemoryMode None)
    else
      Provider_context.empty_for_test
        ~popt
        ~tcopt
        ~deps_mode:(Typing_deps_mode.InMemoryMode None)
  in
  Relative_path.Map.iter files_contents ~f:(fun filename contents ->
      File_provider.(provide_file_for_tests filename contents));
  (* We make the following call for the side-effect of updating ctx's "naming-table fallback"
     so it will look in the sqlite database for names it doesn't know.
     This function returns the forward naming table. *)
  let naming_table_for_root : Naming_table.t option =
    Option.map naming_table_path ~f:(fun path ->
        Naming_table.load_from_sqlite ctx path)
  in
  (* If run in naming-table mode, we first have to remove any old names from the files we're about to redeclare --
     otherwise when we declare them it'd count as a duplicate definition! *)
  Option.iter naming_table_for_root ~f:(fun naming_table_for_root ->
      Relative_path.Map.iter files_contents ~f:(fun file _content ->
          let file_info =
            Naming_table.get_file_info naming_table_for_root file
          in
          Option.iter file_info ~f:(fun file_info ->
              let ids_to_strings ids =
                List.map ids ~f:(fun id -> id.File_info.name)
              in
              let { File_info.funs; classes; typedefs; consts; modules } =
                file_info.File_info.ids
              in
              Naming_global.remove_decls
                ~backend:(Provider_context.get_backend ctx)
                ~funs:(ids_to_strings funs)
                ~classes:(ids_to_strings classes)
                ~typedefs:(ids_to_strings typedefs)
                ~consts:(ids_to_strings consts)
                ~modules:(ids_to_strings modules))));

  let (naming_and_parsing_errors, files_info) =
    parse_name_and_decl ctx to_decl
  in
  let errors =
    Diagnostics.merge package_config_errors naming_and_parsing_errors
  in
  let error_format = Diagnostics.format_or_default error_format in
  handle_mode
    mode
    files
    ctx
    builtins
    files_contents
    files_info
    errors
    max_errors
    error_format
    batch_mode
    out_extension
    dbg_deps
    ~should_print_position
    ~profile_type_check_multi
    ~memtrace

let main_hack opts (root : Path.t) (sharedmem_config : Shared_mem.config) : unit
    =
  (* TODO: We should have a per file config *)
  Sys_utils.signal Sys.sigusr1 (Sys.Signal_handle Typing.debug_print_last_pos);
  Event_logger.init_fake ();
  Server_progress.disable ();
  Measure.push_global ();

  let (_handle : Shared_mem.handle) =
    Shared_mem.init ~num_workers:0 sharedmem_config
  in
  let process custom hhi_root =
    if custom then
      let hhi_root_s = Path.to_string hhi_root in
      if Disk.file_exists hhi_root_s && Disk.is_directory hhi_root_s then
        Hhi.set_custom_hhi_root hhi_root
      else
        die ("Custom hhi directory " ^ hhi_root_s ^ " not found")
    else
      Hhi.set_hhi_root_for_unit_test hhi_root;
    Relative_path.set_path_prefix Relative_path.Root root;
    Relative_path.set_path_prefix Relative_path.Hhi hhi_root;
    Relative_path.set_path_prefix Relative_path.Tmp (Path.make "tmp");
    decl_and_run_mode opts hhi_root;
    Typing_logger.flush_buffers ()
  in
  match opts.custom_hhi_path with
  | Some hhi_root -> process true (Path.make hhi_root)
  | None -> Tempfile.with_tempdir (fun hhi_root -> process false hhi_root)

(* command line driver *)
let () =
  if !Sys.interactive then
    ()
  else
    (* On windows, setting 'binary mode' avoids to output CRLF on
       stdout.  The 'text mode' would not hurt the user in general, but
       it breaks the testsuite where the output is compared to the
       expected one (i.e. in given file without CRLF). *)
    Out_channel.set_binary_mode stdout true;
  let (options, root, sharedmem_config) = parse_options () in
  Unix.handle_unix_error main_hack options root sharedmem_config
