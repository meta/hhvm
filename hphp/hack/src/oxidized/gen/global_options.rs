// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.
//
// @generated SignedSource<<37ae9f9f936c3b82d6fc0ffb9b22ad76>>
//
// To regenerate this file, run:
//   buck run @fbcode//mode/dev-nosan-lg fbcode//hphp/hack/src:oxidized_regen

use eq_modulo_pos::EqModuloPos;
use no_pos_hash::NoPosHash;
use ocamlrep::FromOcamlRep;
use ocamlrep::ToOcamlRep;
use serde::Deserialize;
use serde::Serialize;

#[allow(unused_imports)]
use crate::*;

#[derive(
    Clone,
    Debug,
    Deserialize,
    Eq,
    EqModuloPos,
    FromOcamlRep,
    Hash,
    NoPosHash,
    Ord,
    PartialEq,
    PartialOrd,
    Serialize,
    ToOcamlRep
)]
#[rust_to_ocaml(attr = "deriving (show, eq)")]
#[repr(C)]
pub struct SavedStateLoading {
    /// A string from hh.conf. The API key is used for saved state downloads
    /// when we call out to manifold
    pub saved_state_manifold_api_key: Option<String>,
    /// If set, connect to a Manifold instance on localhost using this port.
    /// Useful for testing.
    pub manifold_local_port: Option<isize>,
    /// Use Eden rather than Watchman to look up saved states.
    pub use_eden: bool,
    /// Override the database shard used by saved-state lookup. Useful for testing.
    pub database_shard_name: Option<String>,
    /// Only used for diagnostics when no saved-state was found: If the mergebase is older than
    /// this value, we assume that the revision's age is why we can't find a saved-state, instead of a
    /// technical issue.
    pub saved_state_max_expected_age_days: isize,
    /// Required for Hedwig support for saved state downloads
    pub use_manifold_cython_client: bool,
    /// When decompressing a saved state folder, pass specific files into zstd
    pub zstd_decompress_by_file: bool,
    /// When unpacking a saved state, decompress the dep graph from a .zhhdg file to a .hhdg file
    pub use_compressed_dep_graph: bool,
    /// Maximum number of saved states to keep in the download directory.
    /// Older saved states beyond this limit are cleaned up.
    pub saved_state_cache_limit: isize,
}

#[derive(
    Clone,
    Debug,
    Deserialize,
    Eq,
    EqModuloPos,
    FromOcamlRep,
    Hash,
    NoPosHash,
    Ord,
    PartialEq,
    PartialOrd,
    Serialize,
    ToOcamlRep
)]
#[rust_to_ocaml(attr = "deriving (show, eq)")]
#[repr(C)]
pub struct SavedState {
    pub loading: SavedStateLoading,
    pub rollouts: saved_state_rollouts::SavedStateRollouts,
    pub project_metadata_w_flags: bool,
}

#[derive(
    Clone,
    Debug,
    Deserialize,
    Eq,
    EqModuloPos,
    FromOcamlRep,
    Hash,
    NoPosHash,
    Ord,
    PartialEq,
    PartialOrd,
    Serialize,
    ToOcamlRep
)]
#[rust_to_ocaml(attr = "deriving (eq, show)")]
#[repr(C, u8)]
pub enum AllOrSome<A> {
    All,
    ASome(Vec<A>),
}

#[derive(
    Clone,
    Debug,
    Deserialize,
    Eq,
    EqModuloPos,
    FromOcamlRep,
    Hash,
    NoPosHash,
    Ord,
    PartialEq,
    PartialOrd,
    Serialize,
    ToOcamlRep
)]
#[rust_to_ocaml(attr = "deriving (eq, show)")]
#[repr(C, u8)]
pub enum NoneOrAllExcept<A> {
    NNone,
    #[rust_to_ocaml(name = "All_except")]
    AllExcept(Vec<A>),
}

#[derive(
    Clone,
    Debug,
    Deserialize,
    Eq,
    EqModuloPos,
    FromOcamlRep,
    Hash,
    NoPosHash,
    Ord,
    PartialEq,
    PartialOrd,
    Serialize,
    ToOcamlRep
)]
#[rust_to_ocaml(attr = "deriving (eq, show)")]
#[repr(C, u8)]
pub enum ExtendedReasonsConfig {
    Extended(isize),
    Legacy,
    Debug,
}

/// Naming conventions for fields in this struct:
/// - tco_<feature/flag/setting> - type checker option
/// - po_<feature/flag/setting> - parser option
/// - so_<feature/flag/setting> - server option
#[derive(
    Clone,
    Debug,
    Deserialize,
    FromOcamlRep,
    PartialEq,
    PartialOrd,
    Serialize,
    ToOcamlRep
)]
#[rust_to_ocaml(attr = "deriving (eq, show)")]
#[repr(C)]
pub struct GlobalOptions {
    pub po: parser_options::ParserOptions,
    pub tco_saved_state: SavedState,
    /// Set of typechecker-only experimental features, in lowercase.
    /// NB, *not* the experimental features that are controlled by the
    /// file level <<__EnableUnstableFeatures('feature_name')>> attribute.
    /// Those are in ParserOptions and tco_enabled_unstable_features.
    /// DO NOT add new features here. Instead, introduce new feature-specific field and add to Config_keys,
    /// OR use EnableUnstableFeatures as mentioned above.
    pub tco_legacy_experimental_features: s_set::SSet,
    /// Set of opt-in migration behavior flags, in lowercase.
    pub tco_migration_flags: s_set::SSet,
    /// If set to 0, only the type check delegate's logic will be used.
    /// If the delegate fails to type check, the typing check service as a whole
    /// will fail.
    pub tco_num_local_workers: Option<isize>,
    /// The capacity of the localization cache for large types
    pub tco_locl_cache_capacity: isize,
    /// The number of nodes a type has to exceed to enter the localization cache
    pub tco_locl_cache_node_threshold: isize,
    /// Enables the reverse naming table to fall back to SQLite for queries.
    pub so_naming_sqlite_path: Option<String>,
    /// Flag to disallow `require`, `require_once` etc as toplevel statements
    pub po_disallow_toplevel_requires: bool,
    pub tco_log_large_fanouts_threshold: Option<isize>,
    /// Print types of size bigger than 1000 after performing a type union.
    pub tco_log_inference_constraints: bool,
    /// Flag to enable logging of statistics regarding use of language features.
    /// Currently used for lambdas.
    pub tco_language_feature_logging: bool,
    /// If non-zero, give up type checking a class or function after this many seconds
    pub tco_timeout: isize,
    /// Flag to enable the constraint solver to infer that a type can be indexed then assigned
    pub tco_constraint_array_index_assign: bool,
    /// Flag to enable the constraint solver to infer that a type supports method calls
    pub tco_constraint_method_call: bool,
    /// HH_FIXME should silence *any* error, not just the one specified by code
    pub code_agnostic_fixme: bool,
    /// Error codes for which we allow HH_FIXMEs in strict mode
    pub allowed_fixme_codes_strict: i_set::ISet,
    /// Initial hh_log_level settings
    pub log_levels: s_map::SMap<isize>,
    /// Fetch old decls from CAS instead of memcache/manifold
    pub tco_fetch_remote_old_decls: bool,
    /// Restrict remote old decl fetching to init-time fanout.
    pub tco_only_fetch_remote_old_decl_during_init: bool,
    /// Preserve Rust shallow decls when invalidating folded decls.
    pub tco_disable_rust_provider_shallow_decl_invalidation: bool,
    /// Ignore user-defined class attributes when computing fanout.
    pub tco_enable_annotation_agnostic_decl_diffing: bool,
    /// Populate the member signature heaps.
    ///
    /// If disabled, instead load lazily from shallow classes.
    pub tco_populate_member_heaps: bool,
    /// Skip checks on hierarchy e.g. overrides, require extend, etc.
    /// Set to true only for debugging purposes!
    pub tco_skip_hierarchy_checks: bool,
    /// Do not report errors under the dynamic pass of the typechecker
    pub tco_silence_errors_under_dynamic: bool,
    /// Reject a subclass constructor re-promoting (via a visibility modifier) a
    /// property already declared by an ancestor. Rollout flag; off by default.
    pub tco_reject_promoted_property_redeclaration: bool,
    /// Skip checks implemented with TAST visitors.
    /// Set to true only for debugging purposes!
    pub tco_skip_tast_checks: bool,
    /// Internal (for tests-only): whether any type can appear in a context list
    /// or only types defined in the appropriate Context namespace
    pub tco_strict_contexts: bool,
    /// Enables like casts
    pub tco_like_casts: bool,
    /// Flag to produce an error whenever the TAST contains unresolved type variables
    pub tco_disallow_unresolved_type_variables: bool,
    /// Allows additional error messages to be added to typing errors via config
    pub tco_custom_error_config: custom_error_config::CustomErrorConfig,
    /// Allow <<__Const>> attribute
    pub tco_const_attribute: bool,
    /// Reponame used for glean connection, default to "www.autocomplete"
    pub glean_reponame: String,
    pub symbol_write_index_inherited_members: bool,
    /// Index inherited members information from folded decls
    pub symbol_write_ownership: bool,
    /// include fact ownership information, for creating incremental databases.
    pub symbol_write_root_path: String,
    /// Path prefix to use for hhi files when writing symbol info to JSON
    pub symbol_write_hhi_path: String,
    /// Filepaths to ignore when writing symbol info to JSON, relative to path prefix, eg: root|foo.php
    pub symbol_write_ignore_paths: Vec<String>,
    /// When set, write indexing data for these filepaths only. Relative to repository root, eg: bar.php for root|bar.php
    pub symbol_write_index_paths: Vec<String>,
    /// A file which contains a list of Relative_path.t (one per line) to index
    pub symbol_write_index_paths_file: Option<String>,
    /// Write the list of Relative_path.t to this file instead of indexing. Useful for sharding
    pub symbol_write_index_paths_file_output: Option<String>,
    /// Write symbol indexing data for hhi files
    pub symbol_write_include_hhi: bool,
    /// Use symbols hash table for incremental indexing
    pub symbol_write_sym_hash_in: Option<String>,
    /// Path to file containing units to exclude
    pub symbol_write_exclude_out: Option<String>,
    /// Path to file containing referenced files
    pub symbol_write_referenced_out: Option<String>,
    /// Generate symbols hash table
    pub symbol_write_reindexed_out: Option<String>,
    /// Generate reindexed files
    pub symbol_write_sym_hash_out: bool,
    /// Flag to error on using discarded nullable awaitables
    pub tco_disallow_discarded_nullable_awaitables: bool,
    /// Flag to error on enums whose member values are not distinct
    pub tco_check_duplicate_enum_values: bool,
    /// Require the ShapeKey attribute at class-constant shape-key use sites.
    pub tco_require_shape_key_attribute: bool,
    /// Report errors instead of warnings for statically unresolvable ShapeKey
    /// values when include_const_shape_key_values is enabled. Abstract constants
    /// and trait-dependent [nameof self] always produce errors.
    pub tco_require_const_shape_key_be_resolvable: bool,
    /// Type check this proportion of all files. Default is 1.0.
    /// DO NOT set to any other value except for testing purposes.
    pub tco_typecheck_sample_rate: f64,
    /// Under sound dynamic, introduce like-types for built-in operations e.g. on Vector.
    /// This is done anyway if everything_sdt=true
    pub tco_pessimise_builtins: bool,
    /// Allow use of attribute <<__NoAutoDynamic>>
    pub tco_enable_no_auto_dynamic: bool,
    /// Skip second check of method under dynamic assumptions
    pub tco_skip_check_under_dynamic: bool,
    /// Allow `shape(..., T...)` syntax that constrains unknown-field types
    pub tco_typed_open_shapes: bool,
    /// Allow named variadic parameters in function types, e.g.
    /// `(function(named int...): void)`
    pub tco_named_variadic_type: bool,
    /// Allow variadic named parameters on function definitions, e.g.
    /// `function f(named int...): void {}`
    pub tco_variadic_named_parameters: bool,
    pub tco_global_access_check_enabled: bool,
    /// Enable expression trees via unstable features flag
    pub tco_enable_expression_trees: bool,
    /// Allowed expression tree visitors when not enabled via unstable features flag
    pub tco_allowed_expression_tree_visitors: Vec<String>,
    /// Raise an error when a concrete type constant is overridden by a concrete type constant
    /// in a child class.
    pub tco_typeconst_concrete_concrete_error: bool,
    /// meth_caller can only reference public methods
    pub tco_meth_caller_only_public_visibility: bool,
    /// Consider `require extends` and `require implements` as ancestors when checking a class
    pub tco_require_extends_implements_ancestors: bool,
    /// Inherit SDT from parents, without writing <<__SupportDynamicType>>
    pub tco_implicit_inherit_sdt: bool,
    /// Directory of HSL wrappers defined in the repo, warns on unbound name.
    pub tco_repo_stdlib_path: Option<String>,
    /// Raises an error when a classish is declared <<__ConsistentConstruct>> but lacks an
    /// explicit constructor declaration. 0 does not raise, 1 raises for traits, 2 raises
    /// for all classish
    pub tco_explicit_consistent_constructors: isize,
    /// Raises an error when a class constant is missing a type. 0 does not raise, 1 raises
    /// for abstract class constants, 2 raises for all.
    pub tco_require_types_class_consts: isize,
    /// Sets the amount of fuel that the type printer can use to display an
    /// individual type. More of a type is printed as the value increases.
    pub tco_type_printer_fuel: isize,
    /// allows saved_state_loader to shell out to hg to find globalrev and timestamp of revisions
    pub tco_specify_manifold_api_key: bool,
    /// Measures and reports the time it takes to typecheck each top-level
    /// definition.
    pub tco_profile_top_level_definitions: bool,
    /// When set, it checks if the identifier for the definition matches the
    /// given regular expression and only then typechecks the definition.
    pub tco_typecheck_if_name_matches_regexp: Option<String>,
    pub tco_allow_all_files_for_module_declarations: bool,
    pub tco_allowed_files_for_module_declarations: Vec<String>,
    /// If enabled, the type checker records more fine-grained dependencies than usual,
    /// for example between individual methods.
    pub tco_record_fine_grained_dependencies: bool,
    /// When set, uses the given number of iterations while typechecking loops
    pub tco_loop_iteration_upper_bound: Option<isize>,
    /// Dead UNSAFE_CAST codemod stashes patches through a TAST visitor in shared
    /// heap. This is only needed in dead UNSAFE_CAST removal mode. This option
    /// controls whether the heap will be populated or not.
    pub tco_populate_dead_unsafe_cast_heap: bool,
    /// When set, dynamic locals get shadow tyvars that collect upper-bound
    /// constraints for use-type inference
    pub tco_dynamic_inference: bool,
    /// Dump tast hashes in /tmp/hh_server/tast_hashes
    pub dump_tast_hashes: bool,
    /// List of paths whose TASTs to be dumped in /tmp/hh_server/tasts
    pub dump_tasts: Vec<String>,
    /// Are we running in autocomplete mode ?
    pub tco_autocomplete_mode: bool,
    /// Controls whether we retain the full path for reasons or only simple witnesses
    pub tco_extended_reasons: Option<ExtendedReasonsConfig>,
    /// Filter the types used for strict equality intersection refinement.
    pub tco_enable_eqeqeq_refinement_filter: bool,
    /// If set to true, this disables the use of physical equality in subtyping
    pub tco_disable_physical_equality: bool,
    /// turn on hack warnings
    pub hack_warnings: NoneOrAllExcept<isize>,
    pub warnings_default_all: bool,
    pub warnings_in_sandcastle: bool,
    /// Matchers for file paths for which any warning will be ignored.
    /// Useful to ignore warnings from certain generated files.
    pub warnings_generated_files: Vec<String>,
    pub tco_allowed_files_for_ignore_readonly: Vec<String>,
    /// Patterns for files excluded from the package boundary check.
    pub tco_package_exclude_patterns: Vec<String>,
    /// Option for package support to bypass package boundary violation errors on ::class during
    /// the ::class to nameof migration to unblock V1 of intern-prod separation
    pub tco_package_allow_classconst_violations: bool,
    /// Option for package support to bypass package boundary violation errors on definitions of
    /// all type constants to unblock V1 of intern-prod separation.
    pub tco_package_allow_all_tconst_violations: bool,
    /// Scopes the class-like carve-out to genuine classes and interfaces.
    /// Default [true] preserves the carve-out's current behavior: when it is
    /// on it also suppresses errors for enums, which are enforced by their
    /// base type rather than by name. Set to [false] to keep reporting them —
    /// unlike classes/interfaces, an enum is not covered by the class-name
    /// [Cross_pkg_access] check.
    pub tco_package_allow_enforceable_enum_violations: bool,
    /// Disable RE cache when calling hh_distc. Useful for performance testing.
    /// Corresponds to the `--no-cache` options of hh_distc.
    pub re_no_cache: bool,
    /// Disable trace store when calling hh_distc. Useful for performance testing.
    /// Corresponds to the `--trace-store-mode local` options of hh_distc.
    pub hh_distc_should_disable_trace_store: bool,
    /// Number of retries when uploading/download/executing with hh_distc
    pub hh_distc_exponential_backoff_num_retries: isize,
    /// Enable recursive case types
    pub recursive_case_types: bool,
    /// Set of unstable features enabled for the current file
    pub tco_enabled_unstable_features: s_set::SSet,
    /// Whether class<T> <: classname<T>
    pub class_sub_classname: bool,
    /// When true, C::class : class<C>
    pub class_class_type: bool,
    /// Configure checks for calls to __NeedsConcrete methods through a named
    /// non-concrete class: 0 disables, 1 warns, and 2 errors.
    pub needs_concrete_class_call_check: isize,
    /// Like `needs_concrete_class_call_check`, but for function pointers
    /// such as `C::m<>`. When unset, follows `needs_concrete_class_call_check`.
    pub needs_concrete_class_function_pointer_check: Option<isize>,
    /// When true, also error on abstract class constants that a trait inherits through `require extends` (Typing[4529]).
    pub needs_concrete_require_extends_class_const_check: bool,
    /// When true, ban abstract final classes from extending nonabstract __ConsistentConstruct classes
    pub strict_consistent_construct: bool,
    /// Admits (string)$c when $c: class<T>
    pub allow_class_string_cast: bool,
    /// Error on dict[$c => 1] when $c: class<T>
    pub class_pointer_ban_class_array_key: bool,
    pub tco_permits_bypassing_visibility: Vec<String>,
    pub tco_tests_bypass_visibility_static_properties: bool,
    /// Treat `yield`, `clone` and `list()` destructuring as points that may throw when merging try/catch environments.
    pub tco_implicit_throw_points: bool,
}
