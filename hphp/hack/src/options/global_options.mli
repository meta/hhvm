(*
 * Copyright (c) 2015, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *
 *)

type saved_state_loading = {
  saved_state_manifold_api_key: string option;
      (** A string from hh.conf. The API key is used for saved state downloads
        when we call out to manifold *)
  manifold_local_port: int option;
      (** If set, connect to a Manifold instance on localhost using this port.
        Useful for testing. *)
  use_eden: bool;  (** Use Eden rather than Watchman to look up saved states. *)
  database_shard_name: string option;
      (** Override the database shard used by saved-state lookup. Useful for testing. *)
  saved_state_max_expected_age_days: int;
      (** Only used for diagnostics when no saved-state was found: If the mergebase is older than
        this value, we assume that the revision's age is why we can't find a saved-state, instead of a
        technical issue. *)
  use_manifold_cython_client: bool;
      (** Required for Hedwig support for saved state downloads *)
  zstd_decompress_by_file: bool;
      (** When decompressing a saved state folder, pass specific files into zstd *)
  use_compressed_dep_graph: bool;
      (** When unpacking a saved state, decompress the dep graph from a .zhhdg file to a .hhdg file  *)
  saved_state_cache_limit: int;
      (** Maximum number of saved states to keep in the download directory.
        Older saved states beyond this limit are cleaned up. *)
}

val default_saved_state_loading : saved_state_loading

type saved_state = {
  loading: saved_state_loading;
  rollouts: Saved_state_rollouts.t;
  project_metadata_w_flags: bool;
}
[@@deriving show, eq]

type 'a all_or_some =
  | All
  | ASome of 'a list
[@@deriving eq, show]

type 'a none_or_all_except =
  | NNone
  | All_except of 'a list
[@@deriving eq, show]

val default_saved_state : saved_state

val with_saved_state_manifold_api_key :
  string option -> saved_state -> saved_state

val with_use_manifold_cython_client : bool -> saved_state -> saved_state

val with_zstd_decompress_by_file : bool -> saved_state -> saved_state

type extended_reasons_config =
  | Extended of int
  | Legacy
  | Debug
[@@deriving eq, show]

(** Naming conventions for fieds:
  - tco_<feature/flag/setting> - type checker option
  - po_<feature/flag/setting> - parser option
  - so_<feature/flag/setting> - server option *)
type t = {
  po: Parser_options.t;
  tco_saved_state: saved_state;
  tco_legacy_experimental_features: S_set.t;
      (** Set of typechecker-only experimental features, in lowercase.
          NB, *not* the experimental features that are controlled by the
          file level <<__EnableUnstableFeatures('feature_name')>> attribute.
          Those are in ParserOptions and tco_enabled_unstable_features.
          DO NOT add new features here. Instead, introduce new feature-specific field and add to Config_keys,
          OR use EnableUnstableFeatures as mentioned above. *)
  tco_migration_flags: S_set.t;
      (** Set of opt-in migration behavior flags, in lowercase. *)
  tco_num_local_workers: int option;
      (** If set to 0, only the type check delegate's logic will be used.
        If the delegate fails to type check, the typing check service as a whole
        will fail. *)
  tco_locl_cache_capacity: int;
      (** The capacity of the localization cache for large types *)
  tco_locl_cache_node_threshold: int;
      (** The number of nodes a type has to exceed to enter the localization cache *)
  so_naming_sqlite_path: string option;
      (** Enables the reverse naming table to fall back to SQLite for queries. *)
  po_disallow_toplevel_requires: bool;
      (** Flag to disallow `require`, `require_once` etc as toplevel statements *)
  tco_log_large_fanouts_threshold: int option;
  tco_log_inference_constraints: bool;
      (** Print types of size bigger than 1000 after performing a type union. *)
  tco_language_feature_logging: bool;
      (** Flag to enable logging of statistics regarding use of language features.
        Currently used for lambdas. *)
  tco_timeout: int;
      (** If non-zero, give up type checking a class or function after this many seconds *)
  tco_constraint_array_index_assign: bool;
      (** Flag to enable the constraint solver to infer that a type can be indexed then assigned *)
  tco_constraint_method_call: bool;
      (** Flag to enable the constraint solver to infer that a type supports method calls *)
  code_agnostic_fixme: bool;
      (** HH_FIXME should silence *any* error, not just the one specified by code *)
  allowed_fixme_codes_strict: I_set.t;
      (** Error codes for which we allow HH_FIXMEs in strict mode *)
  log_levels: int S_map.t;  (** Initial hh_log_level settings *)
  tco_fetch_remote_old_decls: bool;
      (** Fetch old decls from CAS instead of memcache/manifold *)
  tco_only_fetch_remote_old_decl_during_init: bool;
      (** Restrict remote old decl fetching to init-time fanout. *)
  tco_disable_rust_provider_shallow_decl_invalidation: bool;
      (** Preserve Rust shallow decls when invalidating folded decls. *)
  tco_enable_annotation_agnostic_decl_diffing: bool;
      (** Ignore user-defined class attributes when computing fanout. *)
  tco_populate_member_heaps: bool;
      (** Populate the member signature heaps.

        If disabled, instead load lazily from shallow classes. *)
  tco_skip_hierarchy_checks: bool;
      (** Skip checks on hierarchy e.g. overrides, require extend, etc.
        Set to true only for debugging purposes! *)
  tco_silence_errors_under_dynamic: bool;
      (** Do not report errors under the dynamic pass of the typechecker *)
  tco_reject_promoted_property_redeclaration: bool;
      (** Reject a subclass constructor re-promoting (via a visibility modifier) a
        property already declared by an ancestor. Rollout flag; off by default. *)
  tco_skip_tast_checks: bool;
      (** Skip checks implemented with TAST visitors.
        Set to true only for debugging purposes! *)
  tco_strict_contexts: bool;
      (** Internal (for tests-only): whether any type can appear in a context list
         or only types defined in the appropriate Context namespace *)
  tco_like_casts: bool;  (** Enables like casts *)
  tco_disallow_unresolved_type_variables: bool;
      (** Flag to produce an error whenever the TAST contains unresolved type variables *)
  tco_custom_error_config: Custom_error_config.t;
      (** Allows additional error messages to be added to typing errors via config *)
  tco_const_attribute: bool;  (** Allow <<__Const>> attribute *)
  glean_reponame: string;
      (** Reponame used for glean connection, default to "www.autocomplete" *)
  symbol_write_index_inherited_members: bool;
  symbol_write_ownership: bool;
      (** Index inherited members information from folded decls *)
  symbol_write_root_path: string;
      (** include fact ownership information, for creating incremental databases. *)
  symbol_write_hhi_path: string;
      (** Path prefix to use for hhi files when writing symbol info to JSON *)
  symbol_write_ignore_paths: string list;
      (** Filepaths to ignore when writing symbol info to JSON, relative to path prefix, eg: root|foo.php *)
  symbol_write_index_paths: string list;
      (** When set, write indexing data for these filepaths only. Relative to repository root, eg: bar.php for root|bar.php *)
  symbol_write_index_paths_file: string option;
      (** A file which contains a list of Relative_path.t (one per line) to index *)
  symbol_write_index_paths_file_output: string option;
      (** Write the list of Relative_path.t to this file instead of indexing. Useful for sharding *)
  symbol_write_include_hhi: bool;
      (** Write symbol indexing data for hhi files *)
  symbol_write_sym_hash_in: string option;
      (** Use symbols hash table for incremental indexing *)
  symbol_write_exclude_out: string option;
      (** Path to file containing units to exclude *)
  symbol_write_referenced_out: string option;
      (** Path to file containing referenced files *)
  symbol_write_reindexed_out: string option;  (** Generate symbols hash table *)
  symbol_write_sym_hash_out: bool;  (** Generate reindexed files *)
  tco_disallow_discarded_nullable_awaitables: bool;
      (** Flag to error on using discarded nullable awaitables *)
  tco_check_duplicate_enum_values: bool;
      (** Flag to error on enums whose member values are not distinct *)
  tco_require_const_shape_key_be_resolvable: bool;
      (** Report errors instead of warnings for statically unresolvable ShapeKey
          values when include_const_shape_key_values is enabled. Abstract constants
          and trait-dependent [nameof self] always produce errors. *)
  tco_typecheck_sample_rate: float;
      (** Type check this proportion of all files. Default is 1.0.
        DO NOT set to any other value except for testing purposes. *)
  tco_pessimise_builtins: bool;
      (** Under sound dynamic, introduce like-types for built-in operations e.g. on Vector.
         This is done anyway if everything_sdt=true *)
  tco_enable_no_auto_dynamic: bool;
      (** Allow use of attribute <<__NoAutoDynamic>> *)
  tco_skip_check_under_dynamic: bool;
      (** Skip second check of method under dynamic assumptions *)
  tco_typed_open_shapes: bool;
      (** Allow `shape(..., T...)` syntax that constrains unknown-field types *)
  tco_named_variadic_type: bool;
      (** Allow named variadic parameters in function types, e.g.
          `(function(named int...): void)` *)
  tco_variadic_named_parameters: bool;
      (** Allow variadic named parameters on function definitions, e.g.
          `function f(named int...): void {}` *)
  tco_global_access_check_enabled: bool;
  tco_enable_expression_trees: bool;
      (** Enable expression trees via unstable features flag *)
  tco_allowed_expression_tree_visitors: string list;
      (** Allowed expression tree visitors when not enabled via unstable features flag *)
  tco_typeconst_concrete_concrete_error: bool;
      (** Raise an error when a concrete type constant is overridden by a concrete type constant
         in a child class. *)
  tco_meth_caller_only_public_visibility: bool;
      (** meth_caller can only reference public methods *)
  tco_require_extends_implements_ancestors: bool;
      (** Consider `require extends` and `require implements` as ancestors when checking a class *)
  tco_implicit_inherit_sdt: bool;
      (** Inherit SDT from parents, without writing <<__SupportDynamicType>> *)
  tco_repo_stdlib_path: string option;
      (** Directory of HSL wrappers defined in the repo, warns on unbound name. *)
  tco_explicit_consistent_constructors: int;
      (** Raises an error when a classish is declared <<__ConsistentConstruct>> but lacks an
         explicit constructor declaration. 0 does not raise, 1 raises for traits, 2 raises
         for all classish *)
  tco_require_types_class_consts: int;
      (** Raises an error when a class constant is missing a type. 0 does not raise, 1 raises
        for abstract class constants, 2 raises for all. *)
  tco_type_printer_fuel: int;
      (** Sets the amount of fuel that the type printer can use to display an
        individual type. More of a type is printed as the value increases. *)
  tco_specify_manifold_api_key: bool;
      (** allows saved_state_loader to shell out to hg to find globalrev and timestamp of revisions *)
  tco_profile_top_level_definitions: bool;
      (** Measures and reports the time it takes to typecheck each top-level
         definition. *)
  tco_typecheck_if_name_matches_regexp: string option;
      (** When set, it checks if the identifier for the definition matches the
        given regular expression and only then typechecks the definition. *)
  tco_allow_all_files_for_module_declarations: bool;
  tco_allowed_files_for_module_declarations: string list;
  tco_record_fine_grained_dependencies: bool;
      (** If enabled, the type checker records more fine-grained dependencies than usual,
         for example between individual methods. *)
  tco_loop_iteration_upper_bound: int option;
      (** When set, uses the given number of iterations while typechecking loops *)
  tco_populate_dead_unsafe_cast_heap: bool;
      (** Dead UNSAFE_CAST codemod stashes patches through a TAST visitor in shared
         heap. This is only needed in dead UNSAFE_CAST removal mode. This option
         controls whether the heap will be populated or not. *)
  tco_dynamic_inference: bool;
      (** When set, dynamic locals get shadow tyvars that collect upper-bound
          constraints for use-type inference *)
  dump_tast_hashes: bool;  (** Dump tast hashes in /tmp/hh_server/tast_hashes *)
  dump_tasts: string list;
      (** List of paths whose TASTs to be dumped in /tmp/hh_server/tasts *)
  tco_autocomplete_mode: bool;  (** Are we running in autocomplete mode ? *)
  tco_extended_reasons: extended_reasons_config option;
      (** Controls whether we retain the full path for reasons or only simple witnesses *)
  tco_enable_eqeqeq_refinement_filter: bool;
      (** Filter the types used for strict equality intersection refinement. *)
  tco_disable_physical_equality: bool;
      (** If set to true, this disables the use of physical equality in subtyping *)
  hack_warnings: int none_or_all_except;  (** turn on hack warnings *)
  warnings_default_all: bool;
  warnings_in_sandcastle: bool;
  warnings_generated_files: string list;
      (** Matchers for file paths for which any warning will be ignored.
          Useful to ignore warnings from certain generated files.  *)
  tco_allowed_files_for_ignore_readonly: string list;
  tco_package_exclude_patterns: string list;
      (** Patterns for files excluded from the package boundary check. *)
  tco_package_allow_classconst_violations: bool;
      (** Option for package support to bypass package boundary violation errors on ::class during
          the ::class to nameof migration to unblock V1 of intern-prod separation *)
  tco_package_allow_all_tconst_violations: bool;
      (** Option for package support to bypass package boundary violation errors on definitions of
          all type constants to unblock V1 of intern-prod separation. *)
  tco_package_allow_enforceable_enum_violations: bool;
      (** Scopes the class-like carve-out to genuine classes and interfaces.
          Default [true] preserves the carve-out's current behavior: when it is
          on it also suppresses errors for enums, which are enforced by their
          base type rather than by name. Set to [false] to keep reporting them —
          unlike classes/interfaces, an enum is not covered by the class-name
          [Cross_pkg_access] check. *)
  re_no_cache: bool;
      (** Disable RE cache when calling hh_distc. Useful for performance testing.
        Corresponds to the `--no-cache` options of hh_distc. *)
  hh_distc_should_disable_trace_store: bool;
      (** Disable trace store when calling hh_distc. Useful for performance testing.
        Corresponds to the `--trace-store-mode local` options of hh_distc.*)
  hh_distc_exponential_backoff_num_retries: int;
      (** Number of retries when uploading/download/executing with hh_distc *)
  recursive_case_types: bool;  (** Enable recursive case types *)
  tco_enabled_unstable_features: S_set.t;
      (** Set of unstable features enabled for the current file *)
  class_sub_classname: bool;  (** Whether class<T> <: classname<T> *)
  class_class_type: bool;  (** When true, C::class : class<C> *)
  needs_concrete_class_call_check: int;
      (** Configure checks for calls to __NeedsConcrete methods through a named
       * non-concrete class: 0 disables, 1 warns, and 2 errors. *)
  needs_concrete_class_function_pointer_check: int option;
      (** Like `needs_concrete_class_call_check`, but for function pointers
       * such as `C::m<>`. When unset, follows `needs_concrete_class_call_check`. *)
  strict_consistent_construct: bool;
      (** When true, ban abstract final classes from extending nonabstract __ConsistentConstruct classes *)
  allow_class_string_cast: bool;  (** Admits (string)$c when $c: class<T>  *)
  class_pointer_ban_class_array_key: bool;
      (** Error on dict[$c => 1] when $c: class<T>  *)
  tco_permits_bypassing_visibility: string list;
  tco_tests_bypass_visibility_static_properties: bool;
  tco_forget_static_prop_refinements_by_name: bool;
      (** When a static property is assigned, invalidate its refinements under every class spelling (`self::`, `static::`, `C::`), not only the one assigned through. *)
  tco_ban_this_refinement_in_generic_class: bool;
      (** Error on `is this` and `as this` in generic classes, where the runtime checks only the class and not its type arguments. *)
  tco_missing_targs_as_fresh_generics: bool;
      (** Give a generic class named without type arguments in a `catch` clause or `C::class` fresh rigid type arguments, as `is C<_>` does, instead of inference variables. *)
  tco_private_member_variance_check: bool;
      (** Error when a private member that mentions a variant class type parameter is written or called through a receiver other than `$this`. *)
  tco_ban_memoize_erased_generic_return: bool;
      (** Error on `__Memoize`/`__MemoizeLSB` functions whose erased generics appear in an invariant or contravariant position of the return type, or appear in the return type and in the type arguments of an `IMemoizeParam` parameter: the memoization cache ignores erased generics. *)
}
[@@deriving eq, show]

val set :
  ?po:Parser_options.t ->
  ?tco_saved_state:saved_state ->
  ?po_disallow_toplevel_requires:bool ->
  ?tco_log_large_fanouts_threshold:int ->
  ?tco_log_inference_constraints:bool ->
  ?tco_legacy_experimental_features:S_set.t ->
  ?tco_migration_flags:S_set.t ->
  ?tco_num_local_workers:int ->
  ?tco_locl_cache_capacity:int ->
  ?tco_locl_cache_node_threshold:int ->
  ?so_naming_sqlite_path:string ->
  ?tco_language_feature_logging:bool ->
  ?tco_timeout:int ->
  ?tco_constraint_array_index_assign:bool ->
  ?tco_constraint_method_call:bool ->
  ?code_agnostic_fixme:bool ->
  ?allowed_fixme_codes_strict:I_set.t ->
  ?log_levels:int S_map.t ->
  ?tco_fetch_remote_old_decls:bool ->
  ?tco_only_fetch_remote_old_decl_during_init:bool ->
  ?tco_disable_rust_provider_shallow_decl_invalidation:bool ->
  ?tco_enable_annotation_agnostic_decl_diffing:bool ->
  ?tco_populate_member_heaps:bool ->
  ?tco_skip_hierarchy_checks:bool ->
  ?tco_silence_errors_under_dynamic:bool ->
  ?tco_reject_promoted_property_redeclaration:bool ->
  ?tco_skip_tast_checks:bool ->
  ?tco_strict_contexts:bool ->
  ?tco_like_casts:bool ->
  ?tco_disallow_unresolved_type_variables:bool ->
  ?tco_custom_error_config:Custom_error_config.t ->
  ?tco_const_attribute:bool ->
  ?glean_reponame:string ->
  ?symbol_write_index_inherited_members:bool ->
  ?symbol_write_ownership:bool ->
  ?symbol_write_root_path:string ->
  ?symbol_write_hhi_path:string ->
  ?symbol_write_ignore_paths:string list ->
  ?symbol_write_index_paths:string list ->
  ?symbol_write_index_paths_file:string ->
  ?symbol_write_index_paths_file_output:string ->
  ?symbol_write_include_hhi:bool ->
  ?symbol_write_sym_hash_in:string ->
  ?symbol_write_exclude_out:string ->
  ?symbol_write_referenced_out:string ->
  ?symbol_write_reindexed_out:string ->
  ?symbol_write_sym_hash_out:bool ->
  ?tco_disallow_discarded_nullable_awaitables:bool ->
  ?tco_check_duplicate_enum_values:bool ->
  ?tco_require_const_shape_key_be_resolvable:bool ->
  ?tco_typecheck_sample_rate:float ->
  ?tco_pessimise_builtins:bool ->
  ?tco_enable_no_auto_dynamic:bool ->
  ?tco_skip_check_under_dynamic:bool ->
  ?tco_typed_open_shapes:bool ->
  ?tco_named_variadic_type:bool ->
  ?tco_variadic_named_parameters:bool ->
  ?tco_global_access_check_enabled:bool ->
  ?tco_enable_expression_trees:bool ->
  ?tco_allowed_expression_tree_visitors:string list ->
  ?tco_typeconst_concrete_concrete_error:bool ->
  ?tco_meth_caller_only_public_visibility:bool ->
  ?tco_require_extends_implements_ancestors:bool ->
  ?tco_implicit_inherit_sdt:bool ->
  ?tco_repo_stdlib_path:string ->
  ?tco_explicit_consistent_constructors:int ->
  ?tco_require_types_class_consts:int ->
  ?tco_type_printer_fuel:int ->
  ?tco_specify_manifold_api_key:bool ->
  ?tco_profile_top_level_definitions:bool ->
  ?tco_typecheck_if_name_matches_regexp:string ->
  ?tco_allow_all_files_for_module_declarations:bool ->
  ?tco_allowed_files_for_module_declarations:string list ->
  ?tco_record_fine_grained_dependencies:bool ->
  ?tco_loop_iteration_upper_bound:int option ->
  ?tco_populate_dead_unsafe_cast_heap:bool ->
  ?tco_dynamic_inference:bool ->
  ?dump_tast_hashes:bool ->
  ?dump_tasts:string list ->
  ?tco_autocomplete_mode:bool ->
  ?tco_extended_reasons:extended_reasons_config ->
  ?tco_enable_eqeqeq_refinement_filter:bool ->
  ?tco_disable_physical_equality:bool ->
  ?hack_warnings:int none_or_all_except ->
  ?warnings_default_all:bool ->
  ?warnings_in_sandcastle:bool ->
  ?warnings_generated_files:string list ->
  ?tco_allowed_files_for_ignore_readonly:string list ->
  ?tco_package_exclude_patterns:string list ->
  ?tco_package_allow_classconst_violations:bool ->
  ?tco_package_allow_all_tconst_violations:bool ->
  ?tco_package_allow_enforceable_enum_violations:bool ->
  ?re_no_cache:bool ->
  ?hh_distc_should_disable_trace_store:bool ->
  ?hh_distc_exponential_backoff_num_retries:int ->
  ?recursive_case_types:bool ->
  ?tco_enabled_unstable_features:S_set.t ->
  ?class_sub_classname:bool ->
  ?class_class_type:bool ->
  ?needs_concrete_class_call_check:int ->
  ?needs_concrete_class_function_pointer_check:int option ->
  ?strict_consistent_construct:bool ->
  ?allow_class_string_cast:bool ->
  ?class_pointer_ban_class_array_key:bool ->
  ?tco_permits_bypassing_visibility:string list ->
  ?tco_tests_bypass_visibility_static_properties:bool ->
  ?tco_forget_static_prop_refinements_by_name:bool ->
  ?tco_ban_this_refinement_in_generic_class:bool ->
  ?tco_missing_targs_as_fresh_generics:bool ->
  ?tco_private_member_variance_check:bool ->
  ?tco_ban_memoize_erased_generic_return:bool ->
  t ->
  t

val default : t

(* NOTE: set/getters for tco_* options moved to TypecheckerOptions *)
(* NOTE: set/getters for po_* options moved to ParserOptions *)

val so_naming_sqlite_path : t -> string option

val allowed_fixme_codes_strict : t -> I_set.t

val code_agnostic_fixme : t -> bool

(* NOTE: set/getters for tco_* options moved to TypecheckerOptions *)
(* NOTE: set/getters for po_* options moved to ParserOptions *)
