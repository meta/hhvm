(*
 * Copyright (c) 2015, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

(**
 * Parses and gathers information from the .hhconfig in the repo.
 *)

open Hh_prelude
open Config_file.Getters
open Server_local_config

type t = {
  version: Config_file.version; [@printer (fun fmt _ -> fprintf fmt "version")]
  load_script_timeout: int;
  (* in seconds *)
  (* Configures only the workers. Workers can have more relaxed GC configs as
   * they are short-lived processes *)
  gc_control: Gc.control; [@printer (fun fmt _ -> fprintf fmt "control")]
  sharedmem_config: Shared_mem.config;
  tc_options: Typechecker_options.t;
  parser_options: Parser_options.t;
  glean_options: Glean_options.t;
  symbol_write_options: Symbol_write_options.t;
  formatter_override: Path.t option;
  config_hash: string option;
  (* A list of regexps for paths to ignore *)
  ignored_paths: string list;
  (* A list of extra paths to search for declarations *)
  extra_paths: Path.t list;
  warn_on_non_opt_build: bool;
  (* clientIdeDaemon will fall back to performing a full index of files to construct a naming table if it fails to load one. *)
  ide_fall_back_to_full_index: bool;
  naming_table_compression_level: int;
  naming_table_compression_threads: int;
  warnings_generated_files: string list;
      (** List of regex patterns for file paths for which any warning will be ignored.
        Useful to ignore warnings from certain generated files.
        Passing `-Wgenerated` on the command line will override this behavior
        and show warnings from those files.
        These are sent to Rust via FFI so are not stored as regexes here.
         *)
}
[@@deriving show]

let repo_config_path =
  Relative_path.from_root ~suffix:Config_file.file_path_relative_to_repo_root

let is_compatible c1 c2 =
  (* This comparison can eventually be made more complex; we may not always
   * need to restart hh_server, e.g. changing the path to the load script
   * is immaterial*)
  Poly.equal c1 c2

let make_gc_control config =
  let { Gc.Control.minor_heap_size; space_overhead; _ } =
    Global_config.gc_control
  in
  let minor_heap_size =
    int_ Config_keys.Hhconfig.gc_minor_heap_size ~default:minor_heap_size config
  in
  let space_overhead =
    int_ Config_keys.Hhconfig.gc_space_overhead ~default:space_overhead config
  in
  { Global_config.gc_control with Gc.Control.minor_heap_size; space_overhead }

let make_sharedmem_config config local_config =
  let { Shared_mem.global_size; heap_size; shm_min_avail; _ } =
    Shared_mem.default_config
  in
  let shm_dirs = local_config.Server_local_config.shm_dirs in
  let global_size =
    int_ Config_keys.Hhconfig.sharedmem_global_size ~default:global_size config
  in
  let heap_size =
    int_ Config_keys.Hhconfig.sharedmem_heap_size ~default:heap_size config
  in
  let hash_table_pow =
    int_ Config_keys.Hhconfig.sharedmem_hash_table_pow ~default:18 config
  in
  let log_level =
    int_ Config_keys.Hhconfig.sharedmem_log_level ~default:0 config
  in
  let sample_rate =
    float_ Config_keys.Hhconfig.sharedmem_sample_rate ~default:0.0 config
  in
  let compression =
    int_ Config_keys.Hhconfig.sharedmem_compression ~default:0 config
  in
  let shm_dirs =
    string_list Config_keys.Hhconfig.sharedmem_dirs ~default:shm_dirs config
  in
  let shm_use_sharded_hashtbl =
    bool_
      Config_keys.Hhconfig.shm_use_sharded_hashtbl
      ~default:local_config.Server_local_config.shm_use_sharded_hashtbl
      config
  in
  let shm_cache_size =
    int_
      Config_keys.Hhconfig.shm_cache_size
      ~default:local_config.Server_local_config.shm_cache_size
      config
  in
  let shm_min_avail =
    int_
      Config_keys.Hhconfig.sharedmem_minimum_available
      ~default:shm_min_avail
      config
  in
  {
    Shared_mem.global_size;
    heap_size;
    hash_table_pow;
    log_level;
    sample_rate;
    shm_dirs;
    shm_use_sharded_hashtbl;
    shm_cache_size;
    shm_min_avail;
    compression;
  }

let config_list_regexp = Str.regexp "[, \t]+"

let process_experimental sl =
  match List.map sl ~f:String.lowercase with
  | ["false"] -> S_set.empty
  | ["true"] -> Typechecker_options.experimental_all
  | features ->
    List.fold_left
      features
      ~f:(fun acc feature -> S_set.add feature acc)
      ~init:S_set.empty

let config_experimental_tc_features config =
  Option.map
    (Config_file.Getters.string_opt
       Config_keys.Hhconfig.enable_experimental_tc_features
       config)
    ~f:(fun list_str ->
      let sl = Str.split config_list_regexp list_str in
      process_experimental sl)

let config_experimental_stx_features config =
  Option.map
    (Config_file.Getters.string_opt
       Config_keys.Hhconfig.enable_experimental_stx_features
       config)
    ~f:(fun str ->
      let json = Yojson.Safe.from_string str in
      let pairs =
        match json with
        | `Assoc pairs -> pairs
        | _ -> failwith "expected JSON object"
      in
      S_map.of_list
        (List.map pairs ~f:Experimental_features.parse_experimental_feature))

let process_migration_flags sl =
  match sl with
  | ["false"] -> S_set.empty
  | ["true"] -> Typechecker_options.migration_flags_all
  | flags ->
    List.iter flags ~f:(fun s ->
        if not (S_set.mem s Typechecker_options.migration_flags_all) then
          failwith ("invalid migration flag: " ^ s));
    List.fold_left
      flags
      ~f:(fun acc flag -> S_set.add flag acc)
      ~init:S_set.empty

let config_tc_migration_flags config =
  Option.map
    (Config_file.Getters.string_opt
       Config_keys.Hhconfig.enable_tc_migration_flags
       config)
    ~f:(fun list_str ->
      Str.split config_list_regexp list_str
      |> List.map ~f:String.lowercase
      |> process_migration_flags)

let convert_paths str =
  let json = Yojson.Safe.from_string str in
  let l =
    match json with
    | `List l -> l
    | _ -> failwith "expected JSON array"
  in
  List.filter_map
    ~f:(fun s ->
      match s with
      | `String path -> Some path
      | _ -> None)
    l

let process_ignored_paths config =
  Config_file.Getters.string_opt Config_keys.Hhconfig.ignored_paths config
  |> Option.value_map ~f:convert_paths ~default:[]

let maybe_relative_path fn =
  (* Note: this is not the same as calling realpath; the cwd is not
   * necessarily the same as hh_server's root!!! *)
  Path.make
    begin
      if Filename.is_relative fn then
        Relative_path.(to_absolute (from_root ~suffix:fn))
      else
        fn
    end

let process_extra_paths config =
  match
    Config_file.Getters.string_opt Config_keys.Hhconfig.extra_paths config
  with
  | Some s -> Str.split config_list_regexp s |> List.map ~f:maybe_relative_path
  | _ -> []

let process_untrusted_mode config =
  match
    Config_file.Getters.string_opt Config_keys.Hhconfig.untrusted_mode config
  with
  | Some s ->
    if bool_of_string s then
      let blacklist =
        [
          (* out of tree file access*)
          "extra_paths";
          (* potential resource abuse *)
          "language_feature_logging";
        ]
      in
      let prefix_blacklist =
        [(* potential resource abuse *) "gc_"; "sharedmem_"]
      in
      let invalid_keys =
        List.filter (Config_file.keys config) ~f:(fun ck ->
            let ck = String.lowercase ck in
            let exact_match =
              List.find ~f:(fun bli -> String.equal bli ck) blacklist
            in
            let prefix_match =
              List.find
                ~f:(fun blp -> String.is_prefix ck ~prefix:blp)
                prefix_blacklist
            in
            match (exact_match, prefix_match) with
            | (None, None) -> false
            | _ -> true)
      in
      if not (List.is_empty invalid_keys) then
        failwith
          ("option not permitted in untrusted_mode: "
          ^ String.concat ~sep:", " invalid_keys)
      else
        failwith "untrusted_mode can only be enabled, not disabled"
  | _ -> ()

let extract_auto_namespace_element ns_map element =
  match element with
  | (source, `String target) -> (source, target) :: ns_map
  | _ ->
    (* This means the JSON we received is incorrect *)
    ns_map

let convert_auto_namespace_to_map map =
  let json = Yojson.Safe.from_string map in
  let pairs =
    match json with
    | `Assoc pairs -> pairs
    | _ -> failwith "expected JSON object"
  in
  (* We do a fold instead of a map to filter
   * out the incorrect entrie as we look at each item *)
  List.fold_left ~init:[] ~f:extract_auto_namespace_element pairs

let prepare_auto_namespace_map config =
  Option.map
    (Config_file.Getters.string_opt
       Config_keys.Hhconfig.auto_namespace_map
       config)
    ~f:convert_auto_namespace_to_map

let extract_log_level = function
  | (log_key, `Int log_level) -> (log_key, log_level)
  | (log_key, `Intlit log_level) -> begin
    match int_of_string_opt log_level with
    | Some log_level -> (log_key, log_level)
    | None -> failwith "non-integer log level value"
  end
  | _ -> failwith "non-integer log level value"

let convert_log_levels_to_map map =
  let json = Yojson.Safe.from_string map in
  let pairs =
    match json with
    | `Assoc pairs -> pairs
    | _ -> failwith "expected JSON object"
  in
  List.map ~f:extract_log_level pairs |> S_map.of_list

let prepare_log_levels config =
  Option.map
    (Config_file.Getters.string_opt Config_keys.Hhconfig.log_levels config)
    ~f:convert_log_levels_to_map

let prepare_iset config config_name =
  Option.map
    (Config_file.Getters.string_opt config_name config)
    ~f:(fun list_str ->
      Str.split config_list_regexp list_str
      |> List.map ~f:int_of_string
      |> List.fold_right ~init:I_set.empty ~f:I_set.add)

let prepare_allowed_decl_fixme_codes config =
  prepare_iset config Config_keys.Hhconfig.allowed_decl_fixme_codes

let reasons_config_opt config =
  Option.bind
    (string_opt Config_keys.Hhconfig.extended_reasons config)
    ~f:(fun data_str ->
      if String.equal data_str "debug" then
        Some Global_options.Debug
      else if String.equal data_str "legacy" then
        Some Global_options.Legacy
      else
        Option.map ~f:(fun n -> Global_options.Extended n)
        @@ int_of_string_opt data_str)

let load_config (config : Config_file_common.t) (options : Global_options.t) :
    Global_options.t =
  let ( >?? ) x y = Option.value x ~default:y in
  let po_opt = options.Global_options.po in
  let experimental_features = config_experimental_stx_features config in
  let po =
    Parser_options.
      {
        (* These aren't set in the config file, so init them to their defaults. *)
        hhvm_compat_mode = po_opt.hhvm_compat_mode;
        allow_unstable_features = po_opt.allow_unstable_features;
        hhi_mode = po_opt.hhi_mode;
        codegen = po_opt.codegen;
        stack_size = po_opt.stack_size;
        no_parser_readonly_check = po_opt.no_parser_readonly_check;
        unwrap_concurrent = po_opt.unwrap_concurrent;
        (* Never use the legacy mode for hh_server *)
        use_legacy_experimental_feature_config = false;
        (* The remainder are set in the config file *)
        is_systemlib =
          bool_opt Config_keys.Hhconfig.is_systemlib config
          >?? po_opt.is_systemlib;
        const_static_props =
          bool_opt Config_keys.Hhconfig.const_static_props config
          >?? po_opt.const_static_props;
        abstract_static_props =
          bool_opt Config_keys.Hhconfig.abstract_static_props config
          >?? po_opt.abstract_static_props;
        disable_xhp_element_mangling =
          bool_opt Config_keys.Hhconfig.disable_xhp_element_mangling config
          >?? po_opt.disable_xhp_element_mangling;
        disable_xhp_children_declarations =
          bool_opt Config_keys.Hhconfig.disable_xhp_children_declarations config
          >?? po_opt.disable_xhp_children_declarations;
        enable_xhp_class_modifier =
          bool_opt Config_keys.Hhconfig.enable_xhp_class_modifier config
          >?? po_opt.enable_xhp_class_modifier;
        interpret_soft_types_as_like_types =
          bool_opt
            Config_keys.Hhconfig.interpret_soft_types_as_like_types
            config
          >?? po_opt.interpret_soft_types_as_like_types;
        auto_namespace_map =
          prepare_auto_namespace_map config >?? po_opt.auto_namespace_map;
        everything_sdt =
          bool_opt Config_keys.Hhconfig.everything_sdt config
          >?? po_opt.everything_sdt;
        include_enum_member_values =
          bool_opt Config_keys.Hhconfig.include_enum_member_values config
          >?? po_opt.include_enum_member_values;
        include_const_shape_key_values =
          bool_opt Config_keys.Hhconfig.include_const_shape_key_values config
          >?? po_opt.include_const_shape_key_values;
        keep_user_attributes =
          bool_opt Config_keys.Hhconfig.keep_user_attributes config
          >?? po_opt.keep_user_attributes;
        deregister_php_stdlib =
          bool_opt Config_keys.Hhconfig.deregister_php_stdlib config
          >?? po_opt.deregister_php_stdlib;
        union_intersection_type_hints =
          bool_opt Config_keys.Hhconfig.union_intersection_type_hints config
          >?? po_opt.union_intersection_type_hints;
        allowed_decl_fixme_codes =
          prepare_allowed_decl_fixme_codes config
          >?? po_opt.allowed_decl_fixme_codes;
        experimental_features =
          experimental_features >?? po_opt.experimental_features;
        (* If there was no experimental features status list in configuration,
           consider all existing experimental features to be released and hence
           usable. *)
        consider_unspecified_experimental_features_released =
          Option.is_none experimental_features;
        package_info = po_opt.package_info;
        package_support_multifile_tests =
          bool_opt Config_keys.Hhconfig.package_support_multifile_tests config
          >?? po_opt.package_support_multifile_tests;
        disallow_non_annotated_memoize =
          bool_opt Config_keys.Hhconfig.disallow_non_annotated_memoize config
          >?? po_opt.disallow_non_annotated_memoize;
        treat_non_annotated_memoize_as_kbic =
          bool_opt
            Config_keys.Hhconfig.treat_non_annotated_memoize_as_kbic
            config
          >?? po_opt.treat_non_annotated_memoize_as_kbic;
        ignore_string_methods =
          bool_opt Config_keys.Hhconfig.ignore_string_methods config
          >?? po_opt.ignore_string_methods;
        enable_intrinsics_extension = po_opt.enable_intrinsics_extension;
        expression_tree_shape_no_unwrap =
          bool_opt Config_keys.Hhconfig.expression_tree_shape_no_unwrap config
          >?? po_opt.expression_tree_shape_no_unwrap;
        allow_single_statement_concurrent =
          bool_opt Config_keys.Hhconfig.allow_single_statement_concurrent config
          >?? po_opt.allow_single_statement_concurrent;
      }
  in
  Global_options.set
    ~po
    ?tco_language_feature_logging:
      (bool_opt Config_keys.Hhconfig.language_feature_logging config)
    ?tco_timeout:(int_opt Config_keys.Hhconfig.timeout config)
    ?tco_constraint_array_index_assign:
      (bool_opt Config_keys.Hhconfig.constraint_array_index_assign config)
    ?tco_constraint_method_call:
      (bool_opt Config_keys.Hhconfig.constraint_method_call config)
    ?code_agnostic_fixme:
      (bool_opt Config_keys.Hhconfig.code_agnostic_fixme config)
    ?allowed_fixme_codes_strict:
      (prepare_iset config Config_keys.Hhconfig.allowed_fixme_codes_strict)
    ?tco_legacy_experimental_features:(config_experimental_tc_features config)
    ?tco_migration_flags:(config_tc_migration_flags config)
    ?tco_like_casts:(bool_opt Config_keys.Hhconfig.like_casts config)
    ?tco_disallow_unresolved_type_variables:
      (bool_opt Config_keys.Hhconfig.disallow_unresolved_type_variables config)
    ?tco_locl_cache_capacity:
      (int_opt Config_keys.Hhconfig.locl_cache_capacity config)
    ?tco_locl_cache_node_threshold:
      (int_opt Config_keys.Hhconfig.locl_cache_node_threshold config)
    ?po_disallow_toplevel_requires:
      (bool_opt Config_keys.Hhconfig.disallow_toplevel_requires config)
    ?tco_const_attribute:(bool_opt Config_keys.Hhconfig.const_attribute config)
    ?glean_reponame:(string_opt Config_keys.Hhconfig.glean_reponame config)
    ?symbol_write_index_inherited_members:
      (bool_opt
         Config_keys.Hhconfig.symbol_write_index_inherited_members
         config)
    ?symbol_write_ownership:
      (bool_opt Config_keys.Hhconfig.symbol_write_ownership config)
    ?symbol_write_root_path:
      (string_opt Config_keys.Hhconfig.symbol_write_root_path config)
    ?symbol_write_hhi_path:
      (string_opt Config_keys.Hhconfig.symbol_write_hhi_path config)
    ?symbol_write_ignore_paths:
      (string_list_opt Config_keys.Hhconfig.symbol_write_ignore_paths config)
    ?symbol_write_index_paths:
      (string_list_opt Config_keys.Hhconfig.symbol_write_index_paths config)
    ?symbol_write_index_paths_file:
      (string_opt Config_keys.Hhconfig.symbol_write_index_paths_file config)
    ?symbol_write_index_paths_file_output:
      (string_opt
         Config_keys.Hhconfig.symbol_write_index_paths_file_output
         config)
    ?symbol_write_include_hhi:
      (bool_opt Config_keys.Hhconfig.symbol_write_include_hhi config)
    ?symbol_write_sym_hash_in:
      (string_opt Config_keys.Hhconfig.symbol_write_sym_hash_in config)
    ?symbol_write_exclude_out:
      (string_opt Config_keys.Hhconfig.symbol_write_exclude_out config)
    ?symbol_write_referenced_out:
      (string_opt Config_keys.Hhconfig.symbol_write_referenced_out config)
    ?symbol_write_reindexed_out:
      (string_opt Config_keys.Hhconfig.symbol_write_reindexed_out config)
    ?symbol_write_sym_hash_out:
      (bool_opt Config_keys.Hhconfig.symbol_write_sym_hash_out config)
    ?tco_disallow_discarded_nullable_awaitables:
      (bool_opt
         Config_keys.Hhconfig.disallow_discarded_nullable_awaitables
         config)
    ?tco_check_duplicate_enum_values:
      (bool_opt Config_keys.Hhconfig.check_duplicate_enum_values config)
    ?tco_require_enum_shape_key_be_resolvable:
      (bool_opt
         Config_keys.Hhconfig.require_enum_shape_key_be_resolvable
         config)
    ?tco_typecheck_sample_rate:
      (float_opt Config_keys.Hhconfig.typecheck_sample_rate config)
    ?tco_pessimise_builtins:
      (bool_opt Config_keys.Hhconfig.pessimise_builtins config)
    ?tco_enable_no_auto_dynamic:
      (bool_opt Config_keys.Hhconfig.enable_no_auto_dynamic config)
    ?tco_skip_check_under_dynamic:
      (bool_opt Config_keys.Hhconfig.skip_check_under_dynamic config)
    ?tco_typed_open_shapes:
      (bool_opt Config_keys.Hhconfig.typed_open_shapes config)
    ?tco_named_variadic_type:
      (bool_opt Config_keys.Hhconfig.named_variadic_type config)
    ?tco_variadic_named_parameters:
      (bool_opt Config_keys.Hhconfig.variadic_named_parameters config)
    ?tco_silence_errors_under_dynamic:
      (bool_opt Config_keys.Hhconfig.silence_errors_under_dynamic config)
    ?tco_reject_promoted_property_redeclaration:
      (bool_opt
         Config_keys.Hhconfig.reject_promoted_property_redeclaration
         config)
    ?tco_allowed_expression_tree_visitors:
      (Option.map
         (string_list_opt
            Config_keys.Hhconfig.allowed_expression_tree_visitors
            config)
         ~f:(fun l -> List.map l ~f:Utils.add_ns))
    ?tco_typeconst_concrete_concrete_error:
      (bool_opt Config_keys.Hhconfig.typeconst_concrete_concrete_error config)
    ?tco_meth_caller_only_public_visibility:
      (bool_opt Config_keys.Hhconfig.meth_caller_only_public_visibility config)
    ?tco_require_extends_implements_ancestors:
      (bool_opt
         Config_keys.Hhconfig.require_extends_implements_ancestors
         config)
    ?tco_implicit_inherit_sdt:
      (bool_opt Config_keys.Hhconfig.implicit_inherit_sdt config)
    ?tco_repo_stdlib_path:
      (string_opt Config_keys.Hhconfig.repo_stdlib_path config)
    ?tco_explicit_consistent_constructors:
      (int_opt Config_keys.Hhconfig.explicit_consistent_constructors config)
    ?tco_require_types_class_consts:
      (int_opt
         Config_keys.Hhconfig.require_types_tco_require_types_class_consts
         config)
    ?tco_type_printer_fuel:
      (int_opt Config_keys.Hhconfig.type_printer_fuel config)
    ?tco_profile_top_level_definitions:
      (bool_opt Config_keys.Hhconfig.profile_top_level_definitions config)
    ?tco_typecheck_if_name_matches_regexp:
      (string_opt Config_keys.Hhconfig.typecheck_if_name_matches_regexp config)
    ?log_levels:(prepare_log_levels config)
    ?tco_allowed_files_for_module_declarations:
      (string_list_opt
         Config_keys.Hhconfig.allowed_files_for_module_declarations
         config)
    ?tco_allow_all_files_for_module_declarations:
      (bool_opt
         Config_keys.Hhconfig.allow_all_files_for_module_declarations
         config)
    ?tco_populate_dead_unsafe_cast_heap:
      (bool_opt Config_keys.Hhconfig.populate_dead_unsafe_cast_heap config)
    ?dump_tast_hashes:(bool_opt Config_keys.Hhconfig.dump_tast_hashes config)
    ?warnings_default_all:
      (bool_opt Config_keys.Hhconfig.warnings_default_all config)
    ?warnings_in_sandcastle:
      (bool_opt Config_keys.Hhconfig.warnings_in_sandcastle config)
    ?tco_allowed_files_for_ignore_readonly:
      (string_list_opt
         Config_keys.Hhconfig.allowed_files_for_ignore_readonly
         config)
    ?tco_package_allow_classconst_violations:
      (bool_opt Config_keys.Hhconfig.package_allow_classconst_violations config)
    ?tco_package_allow_all_tconst_violations:
      (bool_opt Config_keys.Hhconfig.package_allow_all_tconst_violations config)
    ?tco_package_allow_enforceable_enum_violations:
      (bool_opt
         Config_keys.Hhconfig.package_allow_enforceable_enum_violations
         config)
    ?tco_package_exclude_patterns:
      (string_list_opt Config_keys.Hhconfig.package_exclude_patterns config)
    ?tco_extended_reasons:(reasons_config_opt config)
    ?tco_enable_eqeqeq_refinement_filter:
      (bool_opt Config_keys.Hhconfig.enable_eqeqeq_refinement_filter config)
    ?tco_disable_physical_equality:
      (bool_opt Config_keys.Hhconfig.disable_physical_equality config)
    ?re_no_cache:(bool_opt Config_keys.Hhconfig.re_no_cache config)
    ?hh_distc_should_disable_trace_store:
      (bool_opt Config_keys.Hhconfig.hh_distc_should_disable_trace_store config)
    ?hh_distc_exponential_backoff_num_retries:
      (int_opt
         Config_keys.Hhconfig.hh_distc_exponential_backoff_num_retries
         config)
    ?hack_warnings:
      (bool_opt Config_keys.Hhconfig.hack_warnings config
      |> Option.map ~f:(function
             | true ->
               Global_options.All_except
                 (int_list_opt Config_keys.Hhconfig.disabled_warnings config
                 |> Option.value ~default:[])
             | false -> Global_options.NNone))
    ?recursive_case_types:
      (bool_opt Config_keys.Hhconfig.recursive_case_types config)
    ?class_sub_classname:
      (bool_opt Config_keys.Hhconfig.class_sub_classname config)
    ?class_class_type:(bool_opt Config_keys.Hhconfig.class_class_type config)
    ?needs_concrete_class_call_check:
      (int_opt Config_keys.Hhconfig.needs_concrete_class_call_check config
      |> Option.map ~f:(fun value ->
             if value < 0 || value > 2 then
               invalid_arg "needs_concrete_class_call_check must be 0, 1, or 2";
             value))
    ?needs_concrete_class_function_pointer_check:
      (int_opt
         Config_keys.Hhconfig.needs_concrete_class_function_pointer_check
         config
      |> Option.map ~f:(fun value ->
             if value < 0 || value > 2 then
               invalid_arg
                 "needs_concrete_class_function_pointer_check must be 0, 1, or 2";
             Some value))
    ?strict_consistent_construct:
      (bool_opt Config_keys.Hhconfig.strict_consistent_construct config)
    ?allow_class_string_cast:
      (bool_opt Config_keys.Hhconfig.allow_class_string_cast config)
    ?class_pointer_ban_class_array_key:
      (bool_opt Config_keys.Hhconfig.class_pointer_ban_class_array_key config)
    ?tco_permits_bypassing_visibility:
      (string_list_opt Config_keys.Hhconfig.permits_bypassing_visibility config
      |> Option.map ~f:(List.map ~f:Utils.add_ns))
    ?tco_tests_bypass_visibility_static_properties:
      (bool_opt
         Config_keys.Hhconfig.tests_bypass_visibility_static_properties
         config)
    ?tco_forget_static_prop_refinements_by_name:
      (bool_opt
         Config_keys.Hhconfig.forget_static_prop_refinements_by_name
         config)
    ?tco_ban_this_refinement_in_generic_class:
      (bool_opt
         Config_keys.Hhconfig.ban_this_refinement_in_generic_class
         config)
    ?tco_missing_targs_as_fresh_generics:
      (bool_opt Config_keys.Hhconfig.missing_targs_as_fresh_generics config)
    ?tco_private_member_variance_check:
      (bool_opt Config_keys.Hhconfig.private_member_variance_check config)
    ?tco_ban_memoize_erased_generic_return:
      (bool_opt Config_keys.Hhconfig.ban_memoize_erased_generic_return config)
    ?tco_list_assign_right_to_left:
      (bool_opt Config_keys.Hhconfig.list_assign_right_to_left config)
    options

let warn_on_did_you_mean
    ~(source : string)
    ~(key : string)
    (result : (unit, Config_keys.did_you_mean) Result.t) : unit =
  match result with
  | Ok () -> ()
  | Error (Config_keys.Did_you_mean suggestion) ->
    let msg =
      match suggestion with
      | None -> Printf.sprintf "Unrecognized %s config option: %s" source key
      | Some s ->
        Printf.sprintf
          "Unrecognized %s config option: %s. Did you mean %s?"
          source
          key
          s
    in
    Hh_logger.warn "%s" msg

(** Validate CLI --config key=value overrides.
    Logs a warning for each key that is not a recognized config option.
    Warn-only (not fatal): unrecognized keys can occur during
    rollouts/rollbacks. *)
let warn_on_invalid_config_keys (cli_config_overrides : (string * string) list)
    : unit =
  List.iter cli_config_overrides ~f:(fun (key, _value) ->
      Config_keys.validate ~config_key:key
      |> warn_on_did_you_mean ~source:"--config" ~key)

(** Validate keys in a parsed .hhconfig file.
    Logs a warning for each key that is not recognized. *)
let warn_on_invalid_hhconfig_keys (config : Config_file_common.t) : unit =
  let config_keys = Config_file_common.keys config in
  List.iter config_keys ~f:(fun key ->
      Config_keys.validate_hhconfig_key ~config_key:key
      |> warn_on_did_you_mean ~source:".hhconfig" ~key)

(** Load local config from the following sources:

  * /etc/hh.conf
  * overridden by Knobs
  * overridden by ExperimentsConfig
  * overridden by command line overrides

If not silent, then prints what it's doing to stderr. *)
let load_local_config
    (config : Config_file_common.t)
    (version : Config_file_version.version)
    ~(command_line_overrides : Config_file_common.t)
    ~apply_dynamic_overrides
    ~silent
    ~from : Server_local_config.t =
  let current_rolled_out_flag_idx =
    int_
      Config_keys.Hhconfig.current_saved_state_rollout_flag_index
      ~default:0
      config
  in
  let deactivate_saved_state_rollout =
    bool_
      Config_keys.Hhconfig.deactivate_saved_state_rollout
      ~default:false
      config
  in
  Server_local_config_load.load_with_dynamic_overrides
    ~apply_dynamic_overrides
    ~silent
    ~current_version:version
    ~current_rolled_out_flag_idx
    ~deactivate_saved_state_rollout
    ~from
    ~overrides:command_line_overrides

let load_with_dynamic_overrides
    ~apply_dynamic_overrides
    ~silent
    ~from
    ~(cli_config_overrides : (string * string) list) : t * Server_local_config.t
    =
  let command_line_overrides = Config_file.of_list cli_config_overrides in
  let hhconfig_abs_path = Relative_path.to_absolute repo_config_path in
  let hhconfig = Config_file.parse_hhconfig hhconfig_abs_path in
  warn_on_invalid_hhconfig_keys hhconfig;
  let config =
    Config_file.apply_overrides
      ~config:hhconfig
      ~overrides:command_line_overrides
      ~log_reason:None
  in
  process_untrusted_mode config;
  let version =
    Config_file.parse_version
      (Config_file.Getters.string_opt Config_keys.Hhconfig.version config)
  in
  let local_config =
    load_local_config
      config
      version
      ~command_line_overrides
      ~apply_dynamic_overrides
      ~silent
      ~from
  in
  Option.iter
    ~f:Config_file_common.set_pkgconfig_path
    (string_opt Config_keys.Hhconfig.packages_config_path config);
  let pkgs_config_abs_path =
    Relative_path.(
      to_absolute
      @@ from_root ~suffix:(Config_file_common.get_pkgconfig_path ()))
  in
  let config_hash =
    Config_file_common.hash
      hhconfig
      ~hhconfig_contents:(Config_file.cat_hhconfig_file hhconfig_abs_path)
      ~pkgconfig_contents:(Config_file.cat_packages_file pkgs_config_abs_path)
  in
  let global_opts_without_package_info =
    let tco_custom_error_config =
      Custom_error_config_loader.load_and_parse ()
    in
    let local_config_opts =
      Global_options.set
        ~po:
          Global_options.
            {
              default.po with
              Parser_options.allow_unstable_features =
                local_config.Server_local_config.allow_unstable_features;
            }
        ?so_naming_sqlite_path:local_config.naming_sqlite_path
        ?tco_log_large_fanouts_threshold:
          local_config.log_large_fanouts_threshold
        ~tco_fetch_remote_old_decls:
          local_config.Server_local_config.fetch_remote_old_decls
        ~tco_only_fetch_remote_old_decl_during_init:
          local_config
            .Server_local_config.only_fetch_remote_old_decl_during_init
        ~tco_disable_rust_provider_shallow_decl_invalidation:
          local_config
            .Server_local_config.disable_rust_provider_shallow_decl_invalidation
        ~tco_enable_annotation_agnostic_decl_diffing:
          local_config
            .Server_local_config.enable_annotation_agnostic_decl_diffing
        ~tco_populate_member_heaps:
          local_config.Server_local_config.populate_member_heaps
        ~tco_skip_hierarchy_checks:
          local_config.Server_local_config.skip_hierarchy_checks
        ~tco_skip_tast_checks:local_config.Server_local_config.skip_tast_checks
        ~tco_silence_errors_under_dynamic:
          local_config.Server_local_config.silence_errors_under_dynamic
        ~tco_saved_state:local_config.Server_local_config.saved_state
        ~tco_log_inference_constraints:
          local_config.Server_local_config.log_inference_constraints
        ~tco_global_access_check_enabled:
          local_config.Server_local_config.enable_global_access_check
        ~dump_tast_hashes:local_config.dump_tast_hashes
        ~dump_tasts:local_config.dump_tasts
        ~tco_custom_error_config
        ~hack_warnings:
          (if local_config.hack_warnings then
            Global_options.All_except
              (int_list_opt Config_keys.Hhconfig.disabled_warnings config
              |> Option.value ~default:[])
          else
            Global_options.NNone)
        ~warnings_default_all:local_config.warnings_default_all
        ~warnings_in_sandcastle:local_config.warnings_in_sandcastle
        ~warnings_generated_files:
          (string_list
             Config_keys.Hhconfig.warnings_generated_files
             ~default:[]
             config)
        ~hh_distc_exponential_backoff_num_retries:
          local_config.hh_distc_exponential_backoff_num_retries
        Global_options.default
    in
    load_config config local_config_opts
  in
  Hh_logger.log "Parsing and loading packages config at %s" pkgs_config_abs_path;
  let package_info =
    Package_config.load_and_parse
      ~strict:local_config.Server_local_config.package_config_strict_validation
      ~enable_implicit_packages:
        (bool_
           Config_keys.Hhconfig.enable_implicit_packages
           ~default:false
           config)
      ~pkgs_config_abs_path
  in
  Package_info.log_package_info package_info;
  let global_opts =
    Global_options.set
      ~po:
        Global_options.{ global_opts_without_package_info.po with package_info }
      global_opts_without_package_info
  in

  Diagnostics.allowed_fixme_codes_strict :=
    Global_options.allowed_fixme_codes_strict global_opts;
  Diagnostics.code_agnostic_fixme :=
    Global_options.code_agnostic_fixme global_opts;
  ( {
      version;
      load_script_timeout =
        (* Since we use the unix alarm() for our timeouts, a timeout value of 0 means
         * to wait indefinitely *)
        int_ Config_keys.Hhconfig.load_script_timeout ~default:0 config;
      gc_control = make_gc_control config;
      sharedmem_config = make_sharedmem_config config local_config;
      tc_options = global_opts;
      parser_options = global_opts.Global_options.po;
      glean_options = global_opts;
      symbol_write_options = global_opts;
      formatter_override =
        Option.map
          (Config_file.Getters.string_opt
             Config_keys.Hhconfig.formatter_override
             config)
          ~f:maybe_relative_path;
      config_hash = Some config_hash;
      ignored_paths = process_ignored_paths config;
      extra_paths = process_extra_paths config;
      warn_on_non_opt_build =
        bool_ Config_keys.Hhconfig.warn_on_non_opt_build ~default:false config;
      ide_fall_back_to_full_index =
        bool_
          Config_keys.Hhconfig.ide_fall_back_to_full_index
          ~default:true
          config;
      naming_table_compression_level =
        int_
          Config_keys.Hhconfig.naming_table_compression_level
          ~default:6
          config;
      naming_table_compression_threads =
        int_
          Config_keys.Hhconfig.naming_table_compression_threads
          ~default:1
          config;
      warnings_generated_files =
        string_list
          Config_keys.Hhconfig.warnings_generated_files
          ~default:[]
          config;
    },
    local_config )

let load ~silent ~from ~cli_config_overrides =
  load_with_dynamic_overrides
    ~apply_dynamic_overrides:(fun ~silent:_ config -> config)
    ~silent
    ~from
    ~cli_config_overrides

(* useful in testing code *)
let default_config =
  {
    version = Config_file.Opaque_version None;
    load_script_timeout = 0;
    gc_control = Global_config.gc_control;
    sharedmem_config = Shared_mem.default_config;
    tc_options = Typechecker_options.default;
    glean_options = Glean_options.default;
    symbol_write_options = Symbol_write_options.default;
    parser_options = Parser_options.default;
    formatter_override = None;
    config_hash = None;
    ignored_paths = [];
    extra_paths = [];
    warn_on_non_opt_build = false;
    ide_fall_back_to_full_index = false;
    naming_table_compression_level = 6;
    naming_table_compression_threads = 1;
    warnings_generated_files = [];
  }

let set_parser_options config popt = { config with parser_options = popt }

let set_tc_options config tcopt = { config with tc_options = tcopt }

let set_glean_options config gleanopt = { config with glean_options = gleanopt }

let set_symbol_write_options config swriteopt =
  { config with symbol_write_options = swriteopt }

let gc_control config = config.gc_control

let sharedmem_config config = config.sharedmem_config

let typechecker_options config = config.tc_options

let parser_options config = config.parser_options

let glean_options config = config.glean_options

let symbol_write_options config = config.symbol_write_options

let formatter_override config = config.formatter_override

let config_hash config = config.config_hash

let ignored_paths config = config.ignored_paths |> List.map ~f:Str.regexp

let extra_paths config = config.extra_paths

let version config = config.version

let warn_on_non_opt_build config = config.warn_on_non_opt_build

let ide_fall_back_to_full_index config = config.ide_fall_back_to_full_index

let warnings_generated_files config = config.warnings_generated_files

let update_config_with_ai_options config local_config ai_options =
  match ai_options with
  | None -> (config, local_config)
  | Some ai ->
    ( {
        config with
        sharedmem_config =
          Ai_options.modify_shared_mem ai config.sharedmem_config;
      },
      {
        local_config with
        watchman =
          {
            local_config.watchman with
            Watchman.enabled = false;
            subscribe = false;
          };
        edenfs_file_watcher =
          {
            local_config.edenfs_file_watcher with
            EdenfsFileWatcher.enabled = false;
          };
        interrupt_on_file_changes = false;
        interrupt_on_client = false;
        trace_parsing = false;
      } )
